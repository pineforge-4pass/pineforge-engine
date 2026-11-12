#!/usr/bin/env python3
"""Process contract for inherited inputs and selected admission."""
import argparse
import array
import csv
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import select
import tempfile
import termios
import threading
import unittest


HEADER = b"timestamp,open,high,low,close,volume\n"
SOURCE = HEADER + (
    b"-60000,11,13,9,12,101\n0,21,23,19,22,102\n"
    b"60000,31,33,29,32,103\n900000,41,43,39,42,104\n"
)
PRIMARY = HEADER + b"0,51,53,49,52,201\n60000,61,63,59,62,202\n"
AUXILIARY = HEADER + (
    b"120000,71,73,69,72,301\n180000,81,83,79,82,302\n240000,91,93,89,92,303\n"
)
CHUNK = 65536
BASE = 1704067200000
FIELDS = ("source_rows", "primary_rows", "script_bars", "auxiliary_rows", "bytes")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def calendar():
    return {
        "start_ms": str(BASE + 5 * 60000), "end_ms": str(BASE + 15 * 60000),
        "fed_start_ms": str(BASE), "preroll_bars": "1", "input_tf": "1",
        "script_tf": "5", "chart_timezone": "UTC", "engine_timezone": "UTC",
        "session": "24x7", "feed_tolerant": True,
    }


def product_fixture():
    def text(rows):
        return HEADER + b"".join(
            (",".join(str(v) for v in row) + "\n").encode("ascii") for row in rows
        )
    return [
        ("source", text([(BASE - 60000, 11, 13, 9, 12, 101), (BASE, 21, 23, 19, 22, 102)])),
        ("primary", text([(BASE, 51, 53, 49, 52, 201), (BASE + 60000, 61, 63, 59, 62, 202)])),
        ("auxiliary", text([(BASE + 120000, 31, 33, 29, 32, 301)])),
        ("source", text([(BASE + 960000, 41, 43, 39, 42, 103)])),
        ("primary", text([
            (BASE + 300000, 71, 73, 69, 72, 203),
            (BASE + 360000, 81, 83, 79, 82, 204),
            (BASE + 600000, 91, 93, 89, 92, 205),
        ])),
        ("auxiliary", text([
            (BASE + 180000, 91, 93, 89, 92, 303),
            (BASE + 240000, 101, 103, 99, 102, 304),
        ])),
    ]


def product_oracle(inputs, bounds):
    """Independent arithmetic over the literal LF fixture, never helper output."""
    state = dict.fromkeys(FIELDS, 0)
    consumed = [0] * len(inputs)
    primary = []
    previous_bucket = None
    for index, (role, data) in enumerate(inputs):
        for number, line in enumerate(data.splitlines(keepends=True)):
            consumed[index] += len(line)
            charges = [("bytes", len(line))]
            timestamp = None
            if number:
                timestamp = int(line.split(b",", 1)[0])
                charges.append((role + "_rows", 1))
                if role == "primary":
                    bucket = (timestamp - BASE) // 300000
                    if bucket != previous_bucket:
                        charges.append(("script_bars", 1))
            for field, requested in charges:
                if field in bounds:
                    if state[field] + requested > bounds[field]:
                        return state, consumed, primary, (
                            field, {"limit": str(bounds[field]), "requested": str(requested),
                                    "used": str(state[field])}, index,
                        )
                    state[field] += requested
            if timestamp is not None and role == "primary":
                primary.append(timestamp)
                previous_bucket = (timestamp - BASE) // 300000
    return state, consumed, primary, None


class HelperCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = os.environ.get("PINEFORGE_SELECTED_ADMIT_HELPER")
        cls.identity = os.environ.get("PINEFORGE_SELECTED_ADMIT_EXPECT_GIT_SHA")
        if not cls.helper or not cls.identity:
            raise AssertionError("helper and compiled identity must be supplied by the test registration")
        if len(cls.identity) != 40 or any(c not in "0123456789abcdef" for c in cls.identity):
            raise AssertionError("invalid registered identity")
        cls.root = Path(tempfile.mkdtemp(prefix="selected-admit-", dir=WORK_DIR))
        cls.serial = 0
        cls.file_paths = {}

    def file(self, data, mode=os.O_RDONLY):
        type(self).serial += 1
        path = self.root / ("input-" + str(self.serial))
        path.write_bytes(data)
        fd = os.open(path, mode)
        self.file_paths[fd] = path
        return fd

    def descriptor(self, specs, bounds, cap=4096):
        return {
            "schema": "pineforge-selected-capacity-request/v1",
            "expect": {"git_sha": self.identity},
            "identity": {"run_id": "run-\u00e9", "attempt_id": "attempt"},
            "bounds": {key: str(value) for key, value in bounds.items()},
            "limits": {"max_record_bytes": str(cap)}, "streams": specs,
        }

    def process(self, document, fds=(), stage="selected", stdout=subprocess.PIPE,
                repeat=False, before_spawn=None, stderr=subprocess.PIPE):
        raw = json.dumps(document, separators=(",", ":"), ensure_ascii=True).encode("ascii")
        descriptor_fd = self.file(raw)
        try:
            if before_spawn:
                before_spawn()
            argv = [
                self.helper, "--stage", stage, "--descriptor-fd", str(descriptor_fd),
                "--descriptor-sha256", digest(raw),
            ]
            result = subprocess.run(argv, pass_fds=(*fds, descriptor_fd),
                                    stdout=stdout, stderr=stderr, check=False)
            if stdout != subprocess.PIPE:
                return result, None
            self.assertTrue(result.stdout.endswith(b"\n"))
            self.assertEqual(result.stdout.count(b"\n"), 1)
            self.assertTrue(result.stdout.isascii())
            decoded = json.loads(result.stdout)
            self.assertEqual(decoded["exit"], result.returncode)
            self.assertEqual(decoded["descriptor_sha256"], digest(raw))
            if result.returncode not in (40, 41):
                self.assertEqual(decoded["identity"], document["identity"])
                self.assertEqual(decoded["helper"]["git_sha"], self.identity)
            if repeat:
                second = subprocess.run(argv, pass_fds=(*fds, descriptor_fd),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
                self.assertEqual(second.returncode, result.returncode)
                self.assertEqual(second.stdout, result.stdout)
            return result, decoded
        finally:
            os.close(descriptor_fd)

    def selected(self, inputs, bounds, cap=4096, mutate=None, repeat=False, mutations=()):
        fds = [self.file(data) for role, data in inputs]
        try:
            specs = [
                {"id": "input-" + str(i), "role": role, "kind": "file", "fd": str(fd),
                 "bytes": str(len(data)), "sha256": digest(data)}
                for i, ((role, data), fd) in enumerate(zip(inputs, fds))
            ]
            document = self.descriptor(specs, bounds, cap)
            if mutate:
                mutate(document)
            def change_file_bytes():
                for index, offset, value in mutations:
                    writer = os.open(self.file_paths[fds[index]], os.O_WRONLY)
                    try:
                        self.assertEqual(os.pwrite(writer, value, offset), len(value))
                    finally:
                        os.close(writer)
            return self.process(document, fds, repeat=repeat, before_spawn=change_file_bytes)
        finally:
            for fd in fds:
                os.close(fd)


class SelectedStages(HelperCase):
    def test_identity_gate_and_no_fields(self):
        inputs = [("source", SOURCE), ("primary", PRIMARY)]
        def wrong_extent(doc):
            doc["streams"][0]["bytes"] = "999"
        process, doc = self.selected(inputs, {}, mutate=wrong_extent)
        self.assertEqual(process.returncode, 10)
        self.assertEqual(doc["reason"], "no_requested_fields")
        self.assertEqual([s["bytes_read"] for s in doc["streams"]], ["0", "0"])
        def wrong_identity(document):
            wrong_extent(document)
            identity = document["expect"]["git_sha"]
            document["expect"]["git_sha"] = identity[:-1] + ("0" if identity[-1] != "0" else "1")
        process, doc = self.selected(inputs, {}, mutate=wrong_identity)
        self.assertEqual(process.returncode, 40)
        self.assertEqual(doc["reason"], "helper:helper_mismatch")

    def test_absent_and_zero_are_distinct(self):
        inputs = [("source", HEADER), ("primary", PRIMARY), ("auxiliary", AUXILIARY)]
        process, doc = self.selected(inputs, {"source_rows": 0})
        self.assertEqual(process.returncode, 0)
        self.assertEqual(doc["state"]["source_rows"], "0")
        self.assertEqual(doc["state"]["bytes"], "0")
        primary_records = list(csv.reader(io.StringIO(PRIMARY.decode())))
        auxiliary_records = list(csv.reader(io.StringIO(AUXILIARY.decode())))
        self.assertEqual(doc["coverage"]["primary_rows"], str(len(primary_records) - 1))
        self.assertEqual(doc["coverage"]["auxiliary_rows"], str(len(auxiliary_records) - 1))
        inputs[0] = ("source", SOURCE)
        process, doc = self.selected(inputs, {"source_rows": 0})
        self.assertEqual(process.returncode, 21)
        self.assertEqual(doc["args"], {"limit": "0", "requested": "1", "used": "0"})
        process, doc = self.selected(inputs, {"primary_rows": 2})
        self.assertEqual(process.returncode, 0)
        self.assertEqual(doc["coverage"]["source_rows"], str(len(SOURCE.splitlines()) - 1))
        self.assertEqual(doc["state"]["source_rows"], "0")

    def test_all_five_and_each_crossing(self):
        inputs = product_fixture()
        totals = {
            "source_rows": sum(len(data.splitlines()) - 1 for role, data in inputs if role == "source"),
            "primary_rows": sum(len(data.splitlines()) - 1 for role, data in inputs if role == "primary"),
            "auxiliary_rows": sum(len(data.splitlines()) - 1 for role, data in inputs if role == "auxiliary"),
            "script_bars": len({
                (int(row[0]) - BASE) // 300000 for role, data in inputs if role == "primary"
                for row in list(csv.reader(io.StringIO(data.decode())))[1:]
            }),
            "bytes": sum(len(data) for role, data in inputs),
        }
        self.assertEqual([totals[f] for f in FIELDS], [3, 5, 3, 3, 554])
        for lowered in (None, *FIELDS):
            bounds = totals.copy()
            if lowered:
                bounds[lowered] -= 1
            state, consumed, primary, failure = product_oracle(inputs, bounds)
            def with_calendar(doc):
                doc["calendar"] = calendar()
            process, doc = self.selected(inputs, bounds, mutate=with_calendar, repeat=True)
            self.assertEqual(doc["sink"]["kind"], "counting")
            self.assertEqual(doc["sink"]["bars"], str(len(primary)))
            self.assertEqual(doc["sink"]["first_ts_ms"], str(primary[0]) if primary else "0")
            self.assertEqual(doc["sink"]["last_ts_ms"], str(primary[-1]) if primary else "0")
            self.assertEqual([int(s["bytes_read"]) for s in doc["streams"]], consumed)
            self.assertEqual(doc["custody_complete"], failure is None)
            if failure:
                field, args, index = failure
                self.assertEqual(process.returncode, 21 + FIELDS.index(field))
                self.assertEqual(doc["code"], "selected_" + field + "_limit_exceeded")
                self.assertEqual(doc["class"], "resource")
                self.assertIs(doc["retryable"], False)
                self.assertEqual(doc["args"], args)
                self.assertEqual(doc["where"]["input_id"], "input-" + str(index))
                self.assertNotIn("state", doc)
                self.assertNotIn("coverage", doc)
                catalog = json.loads((Path(__file__).resolve().parents[1] / "docker/run_failure_codes.json").read_text())
                self.assertEqual(doc["english"], catalog["codes"][doc["code"]]["english"][0])
            else:
                self.assertEqual(process.returncode, 0)
                self.assertEqual(doc["state"], {f: str(state[f]) for f in FIELDS})
                self.assertEqual(doc["coverage"]["primary_rows"], str(len(primary)))
                for role in ("source", "primary", "auxiliary"):
                    self.assertEqual(int(doc["coverage"][role + "_bytes"]),
                                     sum(len(data) for r, data in inputs if r == role))
                    self.assertEqual(int(doc["coverage"][role + "_streams"]),
                                     sum(r == role for r, data in inputs))

    def test_missing_primary_source_and_no_data(self):
        process, doc = self.selected([("source", SOURCE)], {"source_rows": 4})
        self.assertEqual(process.returncode, 11)
        self.assertEqual(doc["reason"], "missing_input")
        self.assertEqual(doc["where"]["role"], "primary")
        self.assertEqual(doc["streams"][0]["bytes_read"], "0")
        process, doc = self.selected([("primary", PRIMARY)], {"source_rows": 4})
        self.assertEqual(process.returncode, 11)
        self.assertEqual(doc["where"]["role"], "source")
        self.assertEqual(doc["where"]["field"], "source_rows")
        process, doc = self.selected([("source", SOURCE), ("primary", HEADER)], {"source_rows": 4})
        self.assertEqual(process.returncode, 31)
        self.assertEqual(doc["reason"], "no_data")
        self.assertEqual(doc["state"]["source_rows"], str(len(SOURCE.splitlines()) - 1))
        self.assertEqual(doc["sink"]["bars"], "0")
        self.assertEqual([int(s["bytes_read"]) for s in doc["streams"]], [len(SOURCE), len(HEADER)])

    def test_calendar_is_dormant_until_requested(self):
        def bad_calendar(doc):
            doc["calendar"] = calendar()
            doc["calendar"]["input_tf"] = "7x"
            doc["calendar"]["session"] = "unknown"
        process, doc = self.selected([("primary", PRIMARY)], {"primary_rows": 2}, mutate=bad_calendar)
        self.assertEqual(process.returncode, 0)
        def unaligned(doc):
            doc["calendar"] = calendar()
            doc["calendar"]["start_ms"] = str(BASE + 300001)
        process, doc = self.selected(product_fixture(), {"script_bars": 3}, mutate=unaligned)
        self.assertEqual(process.returncode, 11)
        self.assertEqual(doc["reason"], "boundary_unaligned")
        self.assertTrue(all(s["bytes_read"] == "0" for s in doc["streams"]))

    def test_parse_and_order_precede_reservations(self):
        prefix = HEADER + b"0,11,13,9,12,101\n60000,21,23,19,22,102\n"
        bad = prefix + b"120000,31,33,29,32,invalid-volume\n"
        process, doc = self.selected([("source", bad), ("primary", PRIMARY)], {"source_rows": 2})
        self.assertEqual(process.returncode, 30)
        self.assertEqual(doc["reason"], "csv_value")
        self.assertEqual(doc["state"]["source_rows"], "2")
        process, doc = self.selected(
            [("source", HEADER + b"0,11,13,9,12,101\n0,21,23,19,22,102\n"),
             ("primary", PRIMARY)], {"source_rows": 1})
        self.assertEqual(process.returncode, 31)
        self.assertEqual(doc["reason"], "rows_unordered")

    def test_same_inode_distinct_roles_and_identity_failure(self):
        fd = self.file(PRIMARY)
        duplicate = os.dup(fd)
        try:
            specs = [
                {"id": role, "role": role, "kind": "file", "fd": str(number),
                 "bytes": str(len(PRIMARY)), "sha256": digest(PRIMARY)}
                for role, number in (("source", fd), ("primary", duplicate))
            ]
            process, doc = self.process(self.descriptor(specs, {"bytes": 2 * len(PRIMARY)}), (fd, duplicate))
            self.assertEqual(process.returncode, 0)
            self.assertEqual(doc["state"]["bytes"], str(2 * len(PRIMARY)))
            self.assertEqual(os.lseek(fd, 0, os.SEEK_CUR), 0)
            self.assertEqual(os.lseek(duplicate, 0, os.SEEK_CUR), 0)
            specs[0]["sha256"] = digest(SOURCE)
            process, doc = self.process(self.descriptor(specs, {"source_rows": 2}), (fd, duplicate))
            self.assertEqual(process.returncode, 33)
            self.assertEqual(doc["reason"], "helper:identity_mismatch")
            self.assertIs(doc["custody_complete"], False)
        finally:
            os.close(fd)
            os.close(duplicate)

    def test_record_caps_and_multichunk_mutations(self):
        note_header = b"timestamp,open,high,low,close,volume,note\n"
        prefix = b"0,21,23,19,22,102,"
        multiline = prefix + b'"' + 6 * b"aaaaaaaa\r\n" + b'x\ny\rz""w"\n'
        unclosed = prefix + b'"' + 2048 * b"x\n"
        cases = [
            (SOURCE, len(HEADER), 0),
            (SOURCE.replace(b"\n", b"\r"), len(HEADER), 0),
            (SOURCE.replace(b"\n", b"\r\n"), len(HEADER) + 1, 0),
            (note_header + multiline + b"60000,31,33,29,32,103,x\n", len(multiline), 1),
            (note_header + unclosed, len(unclosed), 1),
        ]
        for data, longest, row in cases:
            process, doc = self.selected([("source", data), ("primary", PRIMARY)],
                                         {"source_rows": 4}, cap=longest)
            self.assertEqual(process.returncode, 0)
            self.assertEqual(int(doc["streams"][0]["bytes_read"]), len(data))
            records = list(csv.reader(io.StringIO(data.decode(), newline="")))
            self.assertEqual(int(doc["coverage"]["source_rows"]), len(records) - 1)
            process, doc = self.selected([("source", data), ("primary", PRIMARY)],
                                         {"source_rows": 4}, cap=longest - 1)
            self.assertEqual(process.returncode, 31)
            self.assertEqual(doc["reason"], "helper:record_too_long")
            self.assertEqual(doc["where"]["row_index"], str(row))
            preceding = 0 if row == 0 else len(note_header)
            self.assertEqual(int(doc["streams"][0]["bytes_read"]), preceding + longest - 1)
            self.assertIs(doc["custody_complete"], False)
        for extra in (0, 1):
            total = 2 * CHUNK + extra
            data = HEADER + prefix + b"a" * (total - len(HEADER) - len(prefix) - 1) + b"\n"
            cap = len(data) - len(HEADER)
            process, doc = self.selected([("source", SOURCE), ("primary", data)],
                                         {"source_rows": 4, "primary_rows": 1}, cap=cap)
            self.assertEqual(process.returncode, 0)
            self.assertIs(doc["custody_complete"], True)
            self.assertEqual([int(s["bytes_read"]) for s in doc["streams"]], [len(SOURCE), len(data)])
            self.assertEqual(doc["coverage"]["primary_rows"], "1")
            self.assertEqual(doc["sink"]["bars"], "1")
            process, doc = self.selected([("source", SOURCE), ("primary", data)],
                                         {"source_rows": 4, "primary_rows": 1}, cap=cap - 1)
            self.assertEqual(process.returncode, 31)
            self.assertEqual(int(doc["streams"][1]["bytes_read"]), len(HEADER) + cap - 1)
            for offset in (100, CHUNK + 34464, len(data) - 1):
                process, doc = self.selected([("source", SOURCE), ("primary", data)],
                                             {"source_rows": 4, "primary_rows": 1}, cap=cap,
                                             mutations=((1, offset,
                                                 b"a" if offset == len(data) - 1 else b"b"),))
                self.assertEqual(process.returncode, 33)
                self.assertEqual(doc["reason"], "helper:identity_mismatch")
                self.assertIs(doc["custody_complete"], False)
        for index, data, token in ((0, SOURCE, b"101"), (1, PRIMARY, b"201")):
            offset = data.index(token) + len(token) - 1
            process, doc = self.selected([("source", SOURCE), ("primary", PRIMARY)],
                                         {"source_rows": 4},
                                         mutations=((index, offset, b"9"),))
            self.assertEqual(process.returncode, 33)
            self.assertEqual(doc["reason"], "helper:identity_mismatch")
            self.assertIs(doc["custody_complete"], False)
        for terminator in (b"\r\n", b"\r"):
            first = prefix + b"a" * (CHUNK - len(HEADER) - len(prefix) - 1) + terminator
            data = HEADER + first + b"60000,31,33,29,32,103\n"
            for cap in (len(first), len(first) - 1):
                process, doc = self.selected([("source", SOURCE), ("primary", data)],
                                             {"source_rows": 4, "primary_rows": 2}, cap=cap)
                self.assertEqual(process.returncode, 0 if cap == len(first) else 31)
                self.assertEqual(int(doc["streams"][1]["bytes_read"]),
                                 len(data) if cap == len(first) else len(HEADER) + cap)
                if cap == len(first):
                    self.assertEqual(doc["coverage"]["primary_rows"], "2")
                    self.assertEqual(doc["sink"]["bars"], "2")

    def test_extent_preflight_and_result_transport(self):
        for delta in (-1, 1):
            def extent(doc, delta=delta):
                doc["streams"][0]["bytes"] = str(len(SOURCE) + delta)
            process, doc = self.selected([("source", SOURCE), ("primary", PRIMARY)],
                                         {"source_rows": 4}, mutate=extent)
            self.assertEqual(process.returncode, 33)
            self.assertEqual(doc["reason"], "helper:extent_mismatch")
            self.assertTrue(all(s["bytes_read"] == "0" for s in doc["streams"]))
        read_end, write_end = os.pipe()
        os.close(read_end)
        fd = self.file(PRIMARY)
        try:
            specs = [{"id": "p", "role": "primary", "kind": "file", "fd": str(fd),
                      "bytes": str(len(PRIMARY)), "sha256": digest(PRIMARY)}]
            process, doc = self.process(self.descriptor(specs, {"primary_rows": 2}),
                                        (fd,), stdout=write_end)
            self.assertEqual(process.returncode, 32)
            self.assertIn(b"helper:result_write_failed", process.stderr)
        finally:
            os.close(fd)
            os.close(write_end)


def pipe_available(fd):
    count = array.array("i", [0])
    fcntl.ioctl(fd, termios.FIONREAD, count, True)
    return count[0]


class SourceStages(HelperCase):
    def source(self, data, bounds, cap=4096, nonblocking=False, paused=False,
               closed_forward=False, mutate=None):
        input_read, input_write = os.pipe()
        forward_read, forward_write = os.pipe()
        os.set_blocking(input_read, not nonblocking)
        forwarded = []
        errors = []
        resume = threading.Event()

        def produce():
            try:
                position = 0
                if paused:
                    threading.Event().wait(0.05)
                    position = os.write(input_write, HEADER)
                    self.assertEqual(position, len(HEADER))
                    resume.wait()
                while position < len(data):
                    count = os.write(input_write, data[position:position + 4096])
                    position += count
            except BrokenPipeError:
                pass
            except BaseException as error:
                errors.append(error)
            finally:
                os.close(input_write)

        def drain():
            try:
                total = 0
                while True:
                    chunk = os.read(forward_read, 4096)
                    if not chunk:
                        break
                    forwarded.append(chunk)
                    total += len(chunk)
                    if total >= len(HEADER):
                        resume.set()
            except BaseException as error:
                errors.append(error)
            finally:
                os.close(forward_read)

        producer = threading.Thread(target=produce)
        drainer = None
        if closed_forward:
            os.close(forward_read)
        else:
            drainer = threading.Thread(target=drain)
            drainer.start()
        producer.start()
        try:
            specs = [{"id": "source", "role": "source", "kind": "pipe",
                      "fd": str(input_read), "forward_fd": str(forward_write)}]
            request = self.descriptor(specs, bounds, cap)
            if mutate:
                mutate(request)
            process, document = self.process(request, (input_read, forward_write), stage="source")
        finally:
            os.close(input_read)
            os.close(forward_write)
            resume.set()
            producer.join()
            if drainer:
                drainer.join()
        self.assertFalse(errors, errors)
        output = b"".join(forwarded)
        self.assertEqual(int(document["forwarded_bytes"]), len(output))
        self.assertEqual(document["forward_sha256"], digest(output))
        self.assertNotIn("sink", document)
        return process, document, output

    def test_source_identity_and_unknown_schema_leave_pipes_unread(self):
        input_read, input_write = os.pipe()
        forward_read, forward_write = os.pipe()
        self.assertEqual(os.write(input_write, SOURCE), len(SOURCE))
        os.close(input_write)
        try:
            specs = [{"id": "source", "role": "source", "kind": "pipe",
                      "fd": str(input_read), "forward_fd": str(forward_write)}]
            request = self.descriptor(specs, {"source_rows": 4})
            for schema in ("pineforge-selected-capacity-request/v2",
                           "pineforge-selected-capacity-request/v0", "",
                           "PINEFORGE-selected-capacity-request/v1", 1, None):
                candidate = json.loads(json.dumps(request))
                if schema is None:
                    del candidate["schema"]
                else:
                    candidate["schema"] = schema
                result, doc = self.process(candidate, (input_read, forward_write), stage="source")
                self.assertEqual(result.returncode, 40)
                self.assertEqual(doc["reason"], "helper:unknown_descriptor_version")
                self.assertEqual(pipe_available(input_read), len(SOURCE))
                self.assertEqual(pipe_available(forward_read), 0)
            wrong = self.identity[:-1] + ("0" if self.identity[-1] != "0" else "1")
            request["expect"]["git_sha"] = wrong
            result, doc = self.process(request, (input_read, forward_write), stage="source")
            self.assertEqual(result.returncode, 40)
            self.assertEqual(doc["reason"], "helper:helper_mismatch")
            self.assertEqual(pipe_available(input_read), len(SOURCE))
            request["expect"]["git_sha"] = self.identity
            result, doc = self.process(request, (input_read, forward_write), stage="source")
            self.assertEqual(result.returncode, 0)
            self.assertEqual(pipe_available(input_read), 0)
            self.assertEqual(int(doc["forwarded_bytes"]), len(SOURCE))
            self.assertEqual(doc["forward_sha256"], digest(SOURCE))
            self.assertEqual(os.read(forward_read, len(SOURCE)), SOURCE)
        finally:
            os.close(input_read)
            os.close(forward_read)
            os.close(forward_write)

    def test_forward_fd_that_is_stdout_or_stderr_is_refused(self):
        """A forward_fd that is the same pipe as stdout or stderr is refused before any read."""
        for name in ("stdout", "stderr"):
            with self.subTest(forward_is=name):
                input_read, input_write = os.pipe()
                shared_read, shared_write = os.pipe()
                forward_write = os.dup(shared_write)
                open_fds = {input_read, input_write, shared_read, shared_write, forward_write}
                try:
                    self.assertEqual(os.write(input_write, SOURCE), len(SOURCE))
                    os.close(input_write)
                    open_fds.discard(input_write)
                    specs = [{"id": "source", "role": "source", "kind": "pipe",
                              "fd": str(input_read), "forward_fd": str(forward_write)}]
                    request = self.descriptor(specs, {"source_rows": 4})
                    standard = {name: shared_write}
                    process, document = self.process(
                        request, (input_read, forward_write), stage="source", **standard)
                    if name == "stdout":
                        # stdout is the shared pipe: read the result document from it.
                        for fd in (shared_write, forward_write):
                            os.close(fd)
                            open_fds.discard(fd)
                        raw = b""
                        while True:
                            chunk = os.read(shared_read, 4096)
                            if not chunk:
                                break
                            raw += chunk
                        self.assertEqual(raw.count(b"\n"), 1, raw)
                        self.assertTrue(raw.endswith(b"\n"), raw)
                        document = json.loads(raw)
                    self.assertEqual(process.returncode, 41)
                    self.assertEqual(document["exit"], 41)
                    self.assertEqual(document["reason"], "helper:descriptor_invalid")
                    self.assertEqual(document["where"]["input_id"], "source")
                    self.assertEqual(document["forwarded_bytes"], "0")
                    # Refused in preflight: nothing was read from the producer.
                    self.assertEqual(pipe_available(input_read), len(SOURCE))
                finally:
                    for fd in open_fds:
                        os.close(fd)
        # A forward pipe that is a different pipe from stdout and stderr still passes.
        process, document, output = self.source(SOURCE, {"source_rows": 4})
        self.assertEqual(process.returncode, 0)
        self.assertEqual(output, SOURCE)

    def test_source_early_stop_arithmetic(self):
        pieces = SOURCE.splitlines(keepends=True)
        for bound in (4, 3, 2, 0):
            result, doc, output = self.source(SOURCE, {"source_rows": bound})
            expected = b"".join(pieces[:1 + bound])
            self.assertEqual(result.returncode, 0 if bound == len(pieces) - 1 else 21)
            self.assertEqual(output, expected)
            self.assertEqual(doc["custody_complete"], bound == len(pieces) - 1)
            if result.returncode == 21:
                self.assertEqual(doc["args"], {"limit": str(bound), "requested": "1", "used": str(bound)})
                self.assertEqual(int(doc["streams"][0]["bytes_read"]),
                                 sum(len(p) for p in pieces[:2 + bound]))
        for bound in (len(SOURCE), len(SOURCE) - 1, len(HEADER), 0):
            used = 0
            expected = b""
            crossing = None
            for piece in pieces:
                if used + len(piece) > bound:
                    crossing = piece
                    break
                used += len(piece)
                expected += piece
            result, doc, output = self.source(SOURCE, {"bytes": bound})
            self.assertEqual(result.returncode, 0 if crossing is None else 25)
            self.assertEqual(output, expected)
            if crossing is not None:
                self.assertEqual(doc["args"], {"limit": str(bound), "requested": str(len(crossing)),
                                              "used": str(used)})
        prefix_size = len(SOURCE) - len(pieces[-1])
        result, doc, output = self.source(SOURCE, {"source_rows": len(pieces) - 2, "bytes": prefix_size})
        self.assertEqual(result.returncode, 25)
        self.assertEqual(doc["args"], {"limit": str(prefix_size), "requested": str(len(pieces[-1])),
                                      "used": str(prefix_size)})
        self.assertEqual(output, SOURCE[:prefix_size])
        result, doc, output = self.source(HEADER, {"source_rows": 0})
        self.assertEqual(result.returncode, 0)
        self.assertEqual(output, HEADER)

    def test_source_inapplicable_bounds_do_not_read(self):
        for bound in ("primary_rows", "script_bars", "auxiliary_rows"):
            input_read, input_write = os.pipe()
            forward_read, forward_write = os.pipe()
            self.assertEqual(os.write(input_write, SOURCE), len(SOURCE))
            os.close(input_write)
            try:
                request = self.descriptor([{
                    "id": "source", "role": "source", "kind": "pipe",
                    "fd": str(input_read), "forward_fd": str(forward_write),
                }], {bound: 0})
                if bound == "script_bars":
                    request["calendar"] = calendar()
                result, doc = self.process(request, (input_read, forward_write), stage="source")
                self.assertEqual(result.returncode, 10)
                self.assertEqual(doc["reason"], "helper:no_stage_fields")
                self.assertEqual(pipe_available(input_read), len(SOURCE))
                self.assertEqual(doc["streams"][0]["bytes_read"], "0")
                self.assertEqual(doc["forwarded_bytes"], "0")
                self.assertIn(bound, doc["deferred"])
            finally:
                os.close(input_read)
                os.close(forward_read)
                os.close(forward_write)

    def test_source_record_windows_and_transport(self):
        note_header = b"timestamp,open,high,low,close,volume,note\n"
        prefix = b"0,21,23,19,22,102,"
        quoted = prefix + b'"' + 6 * b"aaaaaaaa\r\n" + b'x\ny\rz""w"\n'
        unclosed = prefix + b'"' + 2048 * b"x\n"
        cases = [
            (SOURCE, len(HEADER), 0, 0),
            (SOURCE.replace(b"\n", b"\r"), len(HEADER), 0, 0),
            (SOURCE.replace(b"\n", b"\r\n"), len(HEADER) + 1, 0, 0),
            (note_header + quoted + b"60000,31,33,29,32,103,x\n", len(quoted), 1, len(note_header)),
            (note_header + unclosed, len(unclosed), 1, len(note_header)),
        ]
        for terminator in (b"\r\n", b"\r"):
            first = prefix + b"a" * (CHUNK - len(HEADER) - len(prefix) - 1) + terminator
            cases.append((HEADER + first + b"60000,31,33,29,32,103\n",
                          len(first), 1, len(HEADER)))
        for data, longest, row, before in cases:
            for cap in (longest, longest - 1):
                result, doc, output = self.source(data, {"source_rows": 4}, cap=cap)
                self.assertEqual(result.returncode, 0 if cap == longest else 31)
                self.assertEqual(output, data if cap == longest else data[:before])
                self.assertEqual(int(doc["streams"][0]["bytes_read"]),
                                 len(data) if cap == longest else before + cap)
                if cap == longest:
                    records = list(csv.reader(io.StringIO(data.decode(), newline="")))
                    self.assertEqual(int(doc["coverage"]["source_rows"]), len(records) - 1)
                if cap < longest:
                    self.assertEqual(doc["reason"], "helper:record_too_long")
                    self.assertEqual(doc["where"]["row_index"], str(row))
                    self.assertIs(doc["custody_complete"], False)
        result, doc, output = self.source(note_header + unclosed, {"source_rows": 4}, cap=64)
        self.assertEqual(result.returncode, 31)
        self.assertEqual(int(doc["streams"][0]["bytes_read"]), len(note_header) + 64)
        self.assertEqual(output, note_header)
        result, doc, output = self.source(SOURCE, {"source_rows": 4}, closed_forward=True)
        self.assertEqual(result.returncode, 32)
        self.assertEqual(doc["reason"], "helper:forward_write_failed")
        self.assertEqual(output, b"")

    def test_nonblocking_source_survives_paused_producer(self):
        baseline, before, expected = self.source(SOURCE, {"source_rows": 4})
        result, after, output = self.source(SOURCE, {"source_rows": 4}, nonblocking=True, paused=True)
        self.assertEqual(result.returncode, baseline.returncode)
        self.assertEqual(output, SOURCE)
        self.assertEqual(output, expected)
        # Descriptor fd numbers can differ; every other document member agrees.
        del before["descriptor_sha256"]
        del after["descriptor_sha256"]
        self.assertEqual(after, before)

    def test_source_pipe_stops_before_large_tail_is_written(self):
        total = 64 * 1024 * 1024
        limit = 1000
        cap = 4096

        def record(index):
            return (str(index * 60000) + ",21,23,19,22," + str(102 + index % 13) + "\n").encode("ascii")

        admitted = HEADER
        index = 0
        while len(admitted) + len(record(index)) <= limit:
            admitted += record(index)
            index += 1
        crossing_end = len(admitted) + len(record(index))
        input_read, input_write = os.pipe()
        retained_read = os.dup(input_read)
        forward_read, forward_write = os.pipe()
        os.set_blocking(input_write, False)
        written = [0]
        errors = []
        forwarded = []
        stop = threading.Event()

        def produce():
            pending = HEADER
            ordinal = 0
            try:
                while written[0] < total and not stop.is_set():
                    while len(pending) < 4096:
                        pending += record(ordinal)
                        ordinal += 1
                    piece = pending[:min(4096, total - written[0])]
                    try:
                        count = os.write(input_write, piece)
                    except BlockingIOError:
                        select.select([], [input_write], [], 0.01)
                        continue
                    self.assertGreater(count, 0)
                    written[0] += count
                    pending = pending[count:]
            except BaseException as error:
                errors.append(error)
            finally:
                os.close(input_write)

        def drain():
            try:
                while True:
                    chunk = os.read(forward_read, 4096)
                    if not chunk:
                        break
                    forwarded.append(chunk)
            except BaseException as error:
                errors.append(error)
            finally:
                os.close(forward_read)

        producer = threading.Thread(target=produce)
        drainer = threading.Thread(target=drain)
        producer.start()
        drainer.start()
        try:
            request = self.descriptor([{
                "id": "source", "role": "source", "kind": "pipe", "fd": str(input_read),
                "forward_fd": str(forward_write),
            }], {"bytes": limit}, cap)
            result, doc = self.process(request, (input_read, forward_write), stage="source")
        finally:
            stop.set()
            producer.join()
            os.close(forward_write)
            drainer.join()
        unread = pipe_available(retained_read)
        capacity = fcntl.fcntl(retained_read, fcntl.F_GETPIPE_SZ)
        os.close(input_read)
        os.close(retained_read)
        self.assertFalse(errors, errors)
        self.assertEqual(result.returncode, 25)
        self.assertEqual(doc["args"], {
            "limit": str(limit), "requested": str(crossing_end - len(admitted)),
            "used": str(len(admitted)),
        })
        self.assertEqual(b"".join(forwarded), admitted)
        self.assertEqual(int(doc["forwarded_bytes"]), len(admitted))
        self.assertEqual(doc["forward_sha256"], digest(admitted))
        self.assertLess(written[0], total)
        self.assertGreater(written[0], unread)
        self.assertLessEqual(unread, capacity)
        self.assertLessEqual(written[0] - unread, crossing_end + CHUNK + cap)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", required=True)
    options = parser.parse_args()
    WORK_DIR = options.work_dir
    unittest.main(argv=[__file__], failfast=True)
