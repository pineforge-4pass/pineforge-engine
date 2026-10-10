"""Tests of docker/run_phase_transport.py, the run-only phase transport writer.

Black box over real AF_UNIX socketpairs: the test reads the far end the way the
parent would and judges the writer by the bytes that arrive, the exceptions it
raises and the state of the borrowed descriptor. Two seams are injected and
nothing else: os.write, only to make a short write deterministic (the fake still
hands a real prefix to the socket), and the clock, only for deterministic timing
and for the type negatives of a reading. Everything else runs on the real
socket, and the cases that do not need a scripted clock use the real monotonic
clock.
"""
import ast
import contextlib
import errno
import faulthandler
import gc
import importlib.util
import inspect
import io
import json
import math
import os
import socket
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "docker" / "run_phase_transport.py"
CATALOG_PATH = ROOT / "docker" / "run_failure_codes.json"
REAL_WRITE = os.write


def load_transport():
    spec = importlib.util.spec_from_file_location("run_phase_transport", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


transport = load_transport()
RunPhaseWriter = transport.RunPhaseWriter
PhaseTransportError = transport.PhaseTransportError


def setUpModule():
    # A writer that blocked would hang the whole suite: dump the stacks and exit instead.
    faulthandler.dump_traceback_later(300, exit=True)


def tearDownModule():
    faulthandler.cancel_dump_traceback_later()


# What the documented contract says, written out here and not read from the module under test.
PHASES = ("preflight", "execution", "result_assembly", "results_digest",
          "serialization", "completed")
WITH_DIGEST = PHASES
WITHOUT_DIGEST = tuple(phase for phase in PHASES if phase != "results_digest")
ENVELOPE_KEYS = ["record", "version", "sequence", "scope", "trial_id", "phase", "elapsed_ms"]
COMPLETED_KEYS = ENVELOPE_KEYS + ["execution_ms", "result_assembly_ms",
                                  "results_digest_ms", "serialization_ms"]
PINNED_FIRST_RECORD = (b'{"record":"run_phase","version":1,"sequence":1,"scope":"run",'
                       b'"trial_id":null,"phase":"preflight","elapsed_ms":0}\n')


def reject_constant(name):
    raise ValueError("non-finite JSON number " + name)


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def descriptor_count():
    """How many descriptors this process holds; None where /dev/fd cannot be listed."""
    gc.collect()
    try:
        return len(os.listdir("/dev/fd"))
    except OSError:
        return None


def prefix_write(cut):
    """A fake os.write: hands the first cut(len(data)) bytes to the real descriptor
    and reports exactly that count (0 hands nothing over and reports 0)."""
    def write(fd, data):
        count = cut(len(data))
        return REAL_WRITE(fd, data[:count]) if count else 0
    return write


class WriteSpy:
    """Stands in for os.write. Calls on `fd` (on every descriptor when fd is None)
    are recorded: the bytes handed over, and whether the descriptor was nonblocking
    at that moment. `fake`, when given, decides what such a call does and returns;
    every other call passes through."""

    def __init__(self, fd, fake=None):
        self.fd = fd
        self.fake = fake
        self.calls = []
        self.nonblocking = []

    def __call__(self, fd, data):
        if self.fd is not None and fd != self.fd:
            return REAL_WRITE(fd, data)
        self.calls.append(bytes(data))
        self.nonblocking.append(not os.get_blocking(fd))
        if self.fake is not None:
            return self.fake(fd, data)
        return REAL_WRITE(fd, data)


@contextlib.contextmanager
def spying_on_os_write(fd=None, fake=None):
    spy = WriteSpy(fd, fake)
    with mock.patch.object(os, "write", spy):
        yield spy


class ScriptedClock:
    """A clock that returns the given readings, one per call, and counts its calls."""

    def __init__(self, *readings):
        self.readings = list(readings)
        self.reads = 0

    def __call__(self):
        reading = self.readings[self.reads]
        self.reads += 1
        return reading


class Channel:
    """A real AF_UNIX stream socketpair. `fd` is the writer's end, the descriptor the
    parent would hand down; read() drains the far end as the parent would."""

    def __init__(self, test):
        self.near, self.far = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        test.addCleanup(self.near.close)
        test.addCleanup(self.far.close)
        self.far.setblocking(False)
        self.fd = self.near.fileno()
        test.assertGreaterEqual(self.fd, 3)

    def read(self):
        chunks = []
        while True:
            try:
                chunk = self.far.recv(65536)
            except BlockingIOError:
                break
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)

    def fill(self):
        """Send filler until the near end takes no more, so that any further write
        finds the socket full. Returns how many filler bytes the socket holds."""
        sent = 0
        for size in (65536, 4096, 256, 1):
            while True:
                try:
                    sent += self.near.send(b"x" * size, socket.MSG_DONTWAIT)
                except BlockingIOError:
                    break
        return sent


class WriterCase(unittest.TestCase):
    def channel(self):
        return Channel(self)

    def decode(self, data):
        """The records of a stream, after the receiver's own framing checks."""
        self.assertFalse(data.startswith(b"\xef\xbb\xbf"), "BOM")
        if not data:
            return []
        self.assertTrue(data.endswith(b"\n"), "the stream ends inside a record")
        records = []
        for line in data[:-1].split(b"\n"):
            self.assertLessEqual(len(line) + 1, 1024)
            self.assertNotIn(b"\r", line)
            self.assertNotIn(b" ", line, "records are compact")
            record = json.loads(line.decode("utf-8"), parse_constant=reject_constant)
            self.assertIsInstance(record, dict)
            records.append(record)
        return records

    def check_records(self, records, phases):
        """The receiver's checks: vocabulary and order, exact members, sequence from 1
        without a gap, scope and trial_id, finite nonnegative nondecreasing times."""
        self.assertEqual([record["phase"] for record in records], list(phases))
        previous = 0
        for index, record in enumerate(records, start=1):
            completed = record["phase"] == "completed"
            self.assertEqual(list(record), COMPLETED_KEYS if completed else ENVELOPE_KEYS)
            self.assertEqual(record["record"], "run_phase")
            self.assertIs(type(record["version"]), int)
            self.assertEqual(record["version"], 1)
            self.assertIs(type(record["sequence"]), int)
            self.assertEqual(record["sequence"], index)
            self.assertEqual(record["scope"], "run")
            self.assertIsNone(record["trial_id"])
            elapsed = record["elapsed_ms"]
            self.assertTrue(is_number(elapsed))
            self.assertTrue(math.isfinite(elapsed))
            self.assertGreaterEqual(elapsed, previous)
            previous = elapsed
            if completed:
                for name in COMPLETED_KEYS[len(ENVELOPE_KEYS):]:
                    value = record[name]
                    if name == "results_digest_ms" and "results_digest" not in phases:
                        self.assertIsNone(value)
                        continue
                    self.assertTrue(is_number(value), name)
                    self.assertTrue(math.isfinite(value), name)
                    self.assertGreaterEqual(value, 0, name)
        if records:
            self.assertEqual(records[0]["elapsed_ms"], 0)

    def assert_never_reused(self, writer):
        """A failed writer refuses every phase and does not touch any descriptor."""
        with spying_on_os_write() as spy:
            for phase in PHASES:
                with self.subTest(refused=phase):
                    with self.assertRaises(PhaseTransportError) as caught:
                        writer.advance(phase)
                    self.assertEqual(caught.exception.reason, "writer_failed")
        self.assertEqual(spy.calls, [])


class NormalRunTests(WriterCase):
    def run_phases(self, phases):
        channel = self.channel()
        with spying_on_os_write(channel.fd) as spy:
            writer = RunPhaseWriter(channel.fd)
            self.assertEqual(spy.calls, [], "the constructor writes nothing")
            for phase in phases:
                writer.advance(phase)
        return channel, writer, spy

    def test_full_run_with_the_digest_phase(self):
        channel, writer, spy = self.run_phases(WITH_DIGEST)
        data = channel.read()
        self.check_records(self.decode(data), WITH_DIGEST)
        self.assertIsNotNone(self.decode(data)[-1]["results_digest_ms"])
        self.assertEqual(len(spy.calls), 6, "exactly one os.write per boundary")
        self.assertEqual(b"".join(spy.calls), data)
        for call in spy.calls:
            self.assertTrue(call.endswith(b"\n"))
            self.assertEqual(call.count(b"\n"), 1, "one whole record per write")
        self.assertEqual(spy.nonblocking, [True] * 6, "O_NONBLOCK is set before any write")
        self.assertEqual((writer.phase, writer.sequence), ("completed", 6))
        self.assertIsNone(writer.failure)
        self.assertEqual(writer.unsent, b"")

    def test_full_run_without_the_digest_phase(self):
        channel, writer, spy = self.run_phases(WITHOUT_DIGEST)
        records = self.decode(channel.read())
        self.check_records(records, WITHOUT_DIGEST)
        self.assertIsNone(records[-1]["results_digest_ms"])
        self.assertEqual(len(spy.calls), 5)
        self.assertEqual((writer.phase, writer.sequence), ("completed", 5))

    def test_each_boundary_is_in_the_socket_before_advance_returns(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        for count, phase in enumerate(WITH_DIGEST, start=1):
            writer.advance(phase)
            records = self.decode(channel.read())
            self.assertEqual([record["phase"] for record in records], [phase])
            self.assertEqual(records[0]["sequence"], count)
            self.assertEqual((writer.phase, writer.sequence), (phase, count))

    def test_first_record_is_the_pinned_example_byte_for_byte(self):
        channel = self.channel()
        RunPhaseWriter(channel.fd).advance("preflight")
        self.assertEqual(channel.read(), PINNED_FIRST_RECORD)

    def test_completed_durations_agree_with_the_records_that_bracket_them(self):
        channel, _, _ = self.run_phases(WITH_DIGEST)
        records = self.decode(channel.read())
        elapsed = {record["phase"]: record["elapsed_ms"] for record in records}
        completed = records[-1]
        self.assertAlmostEqual(completed["execution_ms"],
                               elapsed["result_assembly"] - elapsed["execution"], delta=0.001)
        self.assertAlmostEqual(completed["result_assembly_ms"],
                               elapsed["results_digest"] - elapsed["result_assembly"], delta=0.001)
        self.assertAlmostEqual(completed["results_digest_ms"],
                               elapsed["serialization"] - elapsed["results_digest"], delta=0.001)
        self.assertAlmostEqual(completed["serialization_ms"],
                               elapsed["completed"] - elapsed["serialization"], delta=0.001)

    def test_serialization_ms_is_measured_at_the_completed_call_after_the_callers_flush(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        for phase in WITHOUT_DIGEST[:-1]:
            writer.advance(phase)
        time.sleep(0.02)  # the caller serializes and flushes its report
        writer.advance("completed")
        completed = self.decode(channel.read())[-1]
        self.assertGreaterEqual(completed["serialization_ms"], 15.0)
        timing = writer.phase_timing()
        self.assertNotIn("serialization_ms", timing)
        self.assertEqual(timing["execution_ms"], completed["execution_ms"])
        self.assertEqual(timing["result_assembly_ms"], completed["result_assembly_ms"])

    def test_writers_share_nothing(self):
        first, second = self.channel(), self.channel()
        a, b = RunPhaseWriter(first.fd), RunPhaseWriter(second.fd)
        a.advance("preflight")
        a.advance("execution")
        self.assertIsNone(b.phase)
        self.assertEqual(b.sequence, 0)
        b.advance("preflight")
        self.check_records(self.decode(first.read()), WITH_DIGEST[:2])
        self.check_records(self.decode(second.read()), WITH_DIGEST[:1])

    def test_fd_none_tracks_phases_and_timing_and_exports_nothing(self):
        clock = ScriptedClock(1.0, 1.5, 2.0, 2.5, 3.0)
        with spying_on_os_write() as spy:
            writer = RunPhaseWriter(None, clock=clock)
            for phase in WITHOUT_DIGEST[:4]:
                writer.advance(phase)
            timing = writer.phase_timing()
            writer.advance("completed")
        self.assertEqual(spy.calls, [], "no os.write at all")
        self.assertEqual((writer.phase, writer.sequence), ("completed", 5))
        self.assertEqual(timing, {"version": 1, "execution_ms": 500.0,
                                  "result_assembly_ms": 500.0, "results_digest_ms": None})
        with self.assertRaises(PhaseTransportError) as caught:
            RunPhaseWriter(None).advance("execution")
        self.assertEqual(caught.exception.reason, "phase_order",
                         "the order rules bind an unexported run too")


class PhaseOrderTests(WriterCase):
    LEGAL_PREFIXES = (
        (),
        ("preflight",),
        ("preflight", "execution"),
        ("preflight", "execution", "result_assembly"),
        ("preflight", "execution", "result_assembly", "results_digest"),
        ("preflight", "execution", "result_assembly", "serialization"),
        ("preflight", "execution", "result_assembly", "results_digest", "serialization"),
        ("preflight", "execution", "result_assembly", "serialization", "completed"),
        ("preflight", "execution", "result_assembly", "results_digest", "serialization",
         "completed"),
    )
    ALLOWED_AFTER = {
        None: {"preflight"},
        "preflight": {"execution"},
        "execution": {"result_assembly"},
        "result_assembly": {"results_digest", "serialization"},
        "results_digest": {"serialization"},
        "serialization": {"completed"},
        "completed": set(),
    }

    def test_only_the_pinned_order_is_accepted_from_every_state(self):
        for prefix in self.LEGAL_PREFIXES:
            last = prefix[-1] if prefix else None
            for candidate in PHASES:
                with self.subTest(after=last, next=candidate):
                    writer = RunPhaseWriter(None)
                    for phase in prefix:
                        writer.advance(phase)
                    if candidate in self.ALLOWED_AFTER[last]:
                        writer.advance(candidate)
                        self.assertEqual(writer.phase, candidate)
                        self.assertEqual(writer.sequence, len(prefix) + 1)
                        self.assertIsNone(writer.failure)
                        continue
                    with self.assertRaises(PhaseTransportError) as caught:
                        writer.advance(candidate)
                    self.assertEqual(caught.exception.reason, "phase_order")
                    self.assertEqual(caught.exception.phase, candidate)
                    self.assertEqual(writer.phase, last, "the phase was not entered")
                    self.assertEqual(writer.sequence, len(prefix))
                    self.assertEqual(writer.failure, "phase_order")

    def test_unknown_phases_and_wrong_types_are_refused(self):
        values = ["", "bogus", "Preflight", "preflight ", "completed\n", "result-assembly",
                  "trial", "heartbeat", None, 1, True, b"preflight", ["preflight"],
                  ("preflight",), object()]
        for value in values:
            for prefix in ((), ("preflight", "execution")):
                with self.subTest(value=repr(value)[:30], after=len(prefix)):
                    writer = RunPhaseWriter(None)
                    for phase in prefix:
                        writer.advance(phase)
                    with self.assertRaises(PhaseTransportError) as caught:
                        writer.advance(value)
                    self.assertEqual(caught.exception.code, "harness_internal_error")
                    self.assertEqual(caught.exception.reason, "invalid_phase")
                    self.assertEqual(writer.sequence, len(prefix))

    def test_a_refused_phase_writes_nothing_and_ends_the_writer(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        writer.advance("preflight")
        with spying_on_os_write(channel.fd) as spy:
            with self.assertRaises(PhaseTransportError) as caught:
                writer.advance("serialization")  # a gap: execution was skipped
        self.assertEqual(caught.exception.reason, "phase_order")
        self.assertEqual(spy.calls, [])
        self.check_records(self.decode(channel.read()), ["preflight"])
        self.assert_never_reused(writer)

    def test_no_restart_and_no_seventh_record(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        for phase in WITH_DIGEST:
            writer.advance(phase)
        with self.assertRaises(PhaseTransportError) as caught:
            writer.advance("preflight")
        self.assertEqual(caught.exception.reason, "phase_order")
        self.check_records(self.decode(channel.read()), WITH_DIGEST)
        self.assertEqual(writer.sequence, 6)


class DescriptorTests(WriterCase):
    def assert_rejected(self, value, reason):
        with self.assertRaises(PhaseTransportError) as caught:
            RunPhaseWriter(value)
        self.assertEqual(caught.exception.code, "harness_internal_error")
        self.assertEqual(caught.exception.reason, reason)

    def test_descriptor_must_be_a_plain_int_from_3_up(self):
        values = [True, False, 0, 1, 2, -1, 3.0, "3", b"3", [3], object(),
                  2 ** 31, 2 ** 32 + 3, 2 ** 70]
        for value in values:
            with self.subTest(fd=repr(value)[:30]):
                self.assert_rejected(value, "invalid_fd")

    def test_a_closed_descriptor_is_rejected(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        fd = sock.fileno()
        sock.close()
        self.assert_rejected(fd, "invalid_fd")

    def test_non_sockets_are_rejected_and_left_open_and_untouched(self):
        read_end, write_end = os.pipe()
        null = os.open(os.devnull, os.O_RDWR)
        for fd in (read_end, write_end, null):
            self.addCleanup(os.close, fd)
        for fd in (read_end, write_end, null):
            with self.subTest(fd=fd):
                self.assert_rejected(fd, "invalid_fd")
                os.fstat(fd)
                self.assertTrue(os.get_blocking(fd), "a rejected descriptor is not changed")

    def test_wrong_family_is_rejected_and_left_open_and_untouched(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.addCleanup(sock.close)
        self.assert_rejected(sock.fileno(), "invalid_family")
        os.fstat(sock.fileno())
        self.assertTrue(os.get_blocking(sock.fileno()))

    def test_wrong_socket_type_is_rejected_and_left_open_and_untouched(self):
        left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.addCleanup(left.close)
        self.addCleanup(right.close)
        self.assert_rejected(left.fileno(), "invalid_type")
        os.fstat(left.fileno())
        self.assertTrue(os.get_blocking(left.fileno()))

    def test_a_stream_socket_is_borrowed_not_owned_and_made_nonblocking(self):
        channel = self.channel()
        self.assertTrue(os.get_blocking(channel.fd))
        before = descriptor_count()
        writer = RunPhaseWriter(channel.fd)
        self.assertFalse(os.get_blocking(channel.fd), "O_NONBLOCK is set at construction")
        self.assertEqual(descriptor_count(), before, "validation left no extra descriptor")
        os.fstat(channel.fd)
        self.assertEqual(channel.near.fileno(), channel.fd)
        self.assertIsNone(writer.phase)

    def test_the_descriptor_is_never_closed_or_duplicated_on_any_path(self):
        # A scenario returns how many descriptors the test itself closed, None for none.
        def complete(channel, writer):
            for phase in WITH_DIGEST:
                writer.advance(phase)
            channel.read()
            self.assertEqual(channel.near.send(b"!"), 1)  # still the live socket
            self.assertEqual(channel.far.recv(1), b"!")

        def misordered(channel, writer):
            writer.advance("preflight")
            with self.assertRaises(PhaseTransportError):
                writer.advance("completed")

        def short_write(channel, writer):
            with spying_on_os_write(channel.fd, fake=prefix_write(lambda size: size // 2)):
                with self.assertRaises(PhaseTransportError):
                    writer.advance("preflight")

        def full_socket(channel, writer):
            channel.fill()
            with self.assertRaises(PhaseTransportError):
                writer.advance("preflight")

        def disconnected(channel, writer):
            channel.far.close()
            with self.assertRaises(PhaseTransportError):
                writer.advance("preflight")
            return 1  # the far end's descriptor, which the test itself closed

        for scenario in (complete, misordered, short_write, full_socket, disconnected):
            with self.subTest(scenario=scenario.__name__):
                channel = self.channel()
                before = descriptor_count()
                writer = RunPhaseWriter(channel.fd)
                closed = scenario(channel, writer) or 0
                del writer
                gc.collect()
                os.fstat(channel.fd)
                if before is not None:
                    self.assertEqual(descriptor_count(), before - closed)

    def test_writable_access_mode_predicate(self):
        writable = transport._is_writable_mode
        self.assertFalse(writable(os.O_RDONLY))
        self.assertTrue(writable(os.O_WRONLY))
        self.assertTrue(writable(os.O_RDWR))
        self.assertFalse(writable(os.O_RDONLY | os.O_NONBLOCK))
        self.assertTrue(writable(os.O_RDWR | os.O_NONBLOCK | os.O_APPEND))
        self.assertTrue(writable(os.O_WRONLY | os.O_NONBLOCK))


class BackpressureTests(WriterCase):
    CUTS = {
        "nothing taken": lambda size: 0,
        "one byte": lambda size: 1,
        "half": lambda size: size // 2,
        "all but the LF": lambda size: size - 1,
    }

    def test_full_socket_refuses_the_first_boundary(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        filler = channel.fill()
        work = []
        with spying_on_os_write(channel.fd) as spy:
            with self.assertRaises(PhaseTransportError) as caught:
                writer.advance("preflight")
                work.append("platform preflight")
        error = caught.exception
        self.assertEqual(error.code, "harness_internal_error")
        self.assertEqual(error.reason, "would_block")
        self.assertEqual(error.phase, "preflight")
        self.assertIsInstance(error.__cause__, BlockingIOError)
        self.assertEqual(work, [], "no work transition on a failed handoff")
        self.assertEqual(len(spy.calls), 1, "one write, no retry")
        self.assertIsNone(writer.phase)
        self.assertEqual(writer.sequence, 0)
        self.assertEqual(writer.failure, "would_block")
        self.assertEqual(writer.unsent, PINNED_FIRST_RECORD)
        self.assertEqual(channel.read(), b"x" * filler, "no byte of the record got in")
        self.assert_never_reused(writer)

    def test_full_socket_in_the_middle_of_a_run_keeps_the_next_work_from_starting(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        work = []
        writer.advance("preflight")
        work.append("platform preflight")
        writer.advance("execution")
        work.append("strategy execution")
        filler = channel.fill()  # the parent stops reading
        with spying_on_os_write(channel.fd) as spy:
            with self.assertRaises(PhaseTransportError) as caught:
                writer.advance("result_assembly")
                work.append("report assembly")
        self.assertEqual(caught.exception.reason, "would_block")
        self.assertEqual(caught.exception.phase, "result_assembly")
        self.assertEqual(work, ["platform preflight", "strategy execution"])
        self.assertEqual(len(spy.calls), 1)
        self.assertEqual((writer.phase, writer.sequence), ("execution", 2))
        failed = json.loads(writer.unsent)
        self.assertEqual((failed["phase"], failed["sequence"]), ("result_assembly", 3))
        self.assertLessEqual(len(writer.unsent), 1024)
        data = channel.read()
        self.assertEqual(data[-filler:], b"x" * filler)
        self.check_records(self.decode(data[:-filler]), WITH_DIGEST[:2])
        self.assert_never_reused(writer)

    def test_a_reader_that_catches_up_does_not_revive_a_failed_writer(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        channel.fill()
        with self.assertRaises(PhaseTransportError):
            writer.advance("preflight")
        channel.read()  # the socket is healthy again
        self.assert_never_reused(writer)
        self.assertEqual(channel.read(), b"", "no late record and no retried remainder")

    def test_refusal_is_immediate_and_not_a_wait(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        channel.fill()
        started = time.monotonic()
        with self.assertRaises(PhaseTransportError):
            writer.advance("preflight")
        self.assertLess(time.monotonic() - started, 1.0,
                        "run-only never waits for its reader, not even the optimizer's 2 s")

    def test_disconnect_before_the_first_boundary(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        channel.far.close()
        with spying_on_os_write(channel.fd) as spy:
            with self.assertRaises(PhaseTransportError) as caught:
                writer.advance("preflight")
        error = caught.exception
        self.assertEqual(error.reason, "disconnected")
        self.assertEqual(error.code, "harness_internal_error")
        self.assertIsInstance(error.__cause__, (BrokenPipeError, ConnectionResetError))
        self.assertEqual(len(spy.calls), 1)
        self.assertIsNone(writer.phase)
        self.assertEqual(writer.failure, "disconnected")
        os.fstat(channel.fd)
        self.assert_never_reused(writer)

    def test_disconnect_in_the_middle_of_a_run(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        work = []
        writer.advance("preflight")
        work.append("platform preflight")
        writer.advance("execution")
        work.append("strategy execution")
        channel.far.close()
        with self.assertRaises(PhaseTransportError) as caught:
            writer.advance("result_assembly")
            work.append("report assembly")
        self.assertEqual(caught.exception.reason, "disconnected")
        self.assertEqual(work, ["platform preflight", "strategy execution"])
        self.assertEqual((writer.phase, writer.sequence), ("execution", 2))
        self.assert_never_reused(writer)

    def test_other_write_errors_are_typed_too(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        fd = channel.near.detach()  # the socket object lets go; the descriptor is ours now
        os.close(fd)                # and it is gone behind the writer's back
        with self.assertRaises(PhaseTransportError) as caught:
            writer.advance("preflight")
        error = caught.exception
        self.assertEqual(error.reason, "write_failed")
        self.assertIsInstance(error.__cause__, OSError)
        self.assertEqual(error.__cause__.errno, errno.EBADF)
        self.assertIsNone(writer.phase)
        self.assert_never_reused(writer)

    def test_short_write_retains_a_bounded_remainder_and_fails(self):
        for name, cut in self.CUTS.items():
            with self.subTest(cut=name):
                channel = self.channel()
                writer = RunPhaseWriter(channel.fd)
                writer.advance("preflight")
                self.assertEqual(channel.read(), PINNED_FIRST_RECORD)
                work = []
                with spying_on_os_write(channel.fd, fake=prefix_write(cut)) as spy:
                    with self.assertRaises(PhaseTransportError) as caught:
                        writer.advance("execution")
                        work.append("strategy execution")
                error = caught.exception
                self.assertEqual(error.reason, "short_write")
                self.assertEqual(error.code, "harness_internal_error")
                self.assertEqual(error.phase, "execution")
                self.assertEqual(work, [], "no work transition on a failed handoff")
                self.assertEqual(len(spy.calls), 1, "one write, the rest is not retried")
                line = spy.calls[0]
                taken = cut(len(line))
                sent = channel.read()
                self.assertEqual(sent, line[:taken])
                self.assertEqual(writer.unsent, line[taken:])
                self.assertEqual(sent + writer.unsent, line)
                self.assertGreater(len(writer.unsent), 0)
                self.assertLessEqual(len(writer.unsent), 1024)
                self.assertLessEqual(len(writer.unsent), 1048576)
                self.assertFalse(sent.endswith(b"\n"), "a cut record never looks complete")
                self.assertEqual((writer.phase, writer.sequence), ("preflight", 1))
                self.assertEqual(writer.failure, "short_write")
                self.assert_never_reused(writer)
                self.assertEqual(channel.read(), b"", "the remainder is never sent later")

    def test_a_cut_completed_record_leaves_the_report_timing_available(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        for phase in WITH_DIGEST[:-1]:
            writer.advance(phase)
        with spying_on_os_write(channel.fd, fake=prefix_write(lambda size: 1)):
            with self.assertRaises(PhaseTransportError):
                writer.advance("completed")
        self.assertEqual((writer.phase, writer.sequence), ("serialization", 5))
        timing = writer.phase_timing()
        self.assertEqual(list(timing),
                         ["version", "execution_ms", "result_assembly_ms", "results_digest_ms"])

    def test_a_refused_serialization_boundary_leaves_no_timing(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        for phase in WITH_DIGEST[:4]:
            writer.advance(phase)
        channel.fill()
        with self.assertRaises(PhaseTransportError) as caught:
            writer.advance("serialization")
        self.assertEqual(caught.exception.reason, "would_block")
        with self.assertRaises(PhaseTransportError) as unavailable:
            writer.phase_timing()
        self.assertEqual(unavailable.exception.reason, "timing_unavailable")


class ClockTests(WriterCase):
    BAD_READINGS = [True, False, "1.0", None, b"1", [1.0], 1 + 0j,
                    float("nan"), float("inf"), float("-inf"), 10 ** 400]

    def advance_all(self, writer, phases):
        for phase in phases:
            writer.advance(phase)

    def test_elapsed_and_durations_are_exact_with_the_digest_phase(self):
        channel = self.channel()
        clock = ScriptedClock(10.0, 10.5, 12.0, 12.25, 13.0, 13.125)
        writer = RunPhaseWriter(channel.fd, clock=clock)
        self.assertEqual(clock.reads, 0, "the constructor does not read the clock")
        for count, phase in enumerate(WITH_DIGEST[:4], start=1):
            writer.advance(phase)
            self.assertEqual(clock.reads, count, "one reading per advance")
        with self.assertRaises(PhaseTransportError) as early:
            writer.phase_timing()
        self.assertEqual(early.exception.reason, "timing_unavailable")
        self.assertIsNone(writer.failure, "asking too early does not end the writer")
        writer.advance("serialization")
        timing = writer.phase_timing()
        self.assertEqual(list(timing),
                         ["version", "execution_ms", "result_assembly_ms", "results_digest_ms"])
        self.assertEqual(timing, {"version": 1, "execution_ms": 1500.0,
                                  "result_assembly_ms": 250.0, "results_digest_ms": 750.0})
        writer.advance("completed")
        self.assertEqual(clock.reads, 6)
        records = self.decode(channel.read())
        self.check_records(records, WITH_DIGEST)
        self.assertEqual([record["elapsed_ms"] for record in records],
                         [0, 500, 2000, 2250, 3000, 3125])
        completed = records[-1]
        self.assertEqual(completed["execution_ms"], 1500)
        self.assertEqual(completed["result_assembly_ms"], 250)
        self.assertEqual(completed["results_digest_ms"], 750)
        self.assertEqual(completed["serialization_ms"], 125)
        self.assertEqual(writer.phase_timing(), timing)

    def test_elapsed_and_durations_are_exact_without_the_digest_phase(self):
        channel = self.channel()
        clock = ScriptedClock(100.0, 100.25, 101.0, 101.5, 102.0)
        writer = RunPhaseWriter(channel.fd, clock=clock)
        self.advance_all(writer, WITHOUT_DIGEST[:4])
        self.assertEqual(writer.phase_timing(), {"version": 1, "execution_ms": 750.0,
                                                 "result_assembly_ms": 500.0,
                                                 "results_digest_ms": None})
        writer.advance("completed")
        records = self.decode(channel.read())
        self.check_records(records, WITHOUT_DIGEST)
        self.assertEqual([record["elapsed_ms"] for record in records],
                         [0, 250, 1000, 1500, 2000])
        completed = records[-1]
        self.assertEqual(completed["execution_ms"], 750)
        self.assertEqual(completed["result_assembly_ms"], 500)
        self.assertIsNone(completed["results_digest_ms"])
        self.assertEqual(completed["serialization_ms"], 500)

    def test_integer_and_flat_clocks_are_accepted(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd, clock=ScriptedClock(5, 6, 8, 9, 12))
        self.advance_all(writer, WITHOUT_DIGEST)
        records = self.decode(channel.read())
        self.assertEqual([record["elapsed_ms"] for record in records],
                         [0, 1000, 3000, 4000, 7000])
        self.assertEqual(records[-1]["serialization_ms"], 3000)
        flat = self.channel()
        stopped = RunPhaseWriter(flat.fd, clock=ScriptedClock(7.0, 7.0, 7.0, 7.0, 7.0))
        self.advance_all(stopped, WITHOUT_DIGEST)
        records = self.decode(flat.read())
        self.check_records(records, WITHOUT_DIGEST)
        self.assertEqual([record["elapsed_ms"] for record in records], [0] * 5)
        self.assertEqual(records[-1]["execution_ms"], 0)

    def test_a_bad_first_reading_refuses_preflight(self):
        for bad in self.BAD_READINGS:
            with self.subTest(reading=repr(bad)[:30]):
                channel = self.channel()
                writer = RunPhaseWriter(channel.fd, clock=ScriptedClock(bad))
                with spying_on_os_write(channel.fd) as spy:
                    with self.assertRaises(PhaseTransportError) as caught:
                        writer.advance("preflight")
                self.assertEqual(caught.exception.reason, "clock_invalid")
                self.assertEqual(caught.exception.code, "harness_internal_error")
                self.assertEqual(spy.calls, [])
                self.assertIsNone(writer.phase)
                self.assertEqual(channel.read(), b"")

    def test_a_bad_later_reading_refuses_that_boundary_only(self):
        for bad in self.BAD_READINGS:
            with self.subTest(reading=repr(bad)[:30]):
                channel = self.channel()
                writer = RunPhaseWriter(channel.fd, clock=ScriptedClock(0.0, bad))
                writer.advance("preflight")
                with spying_on_os_write(channel.fd) as spy:
                    with self.assertRaises(PhaseTransportError) as caught:
                        writer.advance("execution")
                self.assertEqual(caught.exception.reason, "clock_invalid")
                self.assertEqual(spy.calls, [], "nothing is handed over for that boundary")
                self.assertEqual((writer.phase, writer.sequence), ("preflight", 1))
                self.check_records(self.decode(channel.read()), ["preflight"])
                self.assert_never_reused(writer)

    def test_a_backward_clock_is_refused_at_any_boundary(self):
        for readings, fails_at in (((5.0, 4.999), 1), ((1.0, 2.0, 3.0, 2.5), 3)):
            with self.subTest(readings=readings):
                channel = self.channel()
                writer = RunPhaseWriter(channel.fd, clock=ScriptedClock(*readings))
                self.advance_all(writer, WITH_DIGEST[:fails_at])
                with self.assertRaises(PhaseTransportError) as caught:
                    writer.advance(WITH_DIGEST[fails_at])
                self.assertEqual(caught.exception.reason, "clock_invalid")
                self.assertEqual(writer.sequence, fails_at)
                self.check_records(self.decode(channel.read()), WITH_DIGEST[:fails_at])

    def test_a_clock_that_raises_is_a_typed_failure(self):
        def broken():
            raise RuntimeError("clock broke")

        channel = self.channel()
        writer = RunPhaseWriter(channel.fd, clock=broken)
        with self.assertRaises(PhaseTransportError) as caught:
            writer.advance("preflight")
        self.assertEqual(caught.exception.reason, "clock_invalid")
        self.assertIsInstance(caught.exception.__cause__, RuntimeError)
        self.assertEqual(channel.read(), b"")

    def test_an_elapsed_span_that_is_not_finite_is_refused(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd, clock=ScriptedClock(-1e308, 1e308))
        writer.advance("preflight")
        with self.assertRaises(PhaseTransportError) as caught:
            writer.advance("execution")
        self.assertEqual(caught.exception.reason, "clock_invalid")
        self.check_records(self.decode(channel.read()), ["preflight"])

    def test_the_default_clock_is_the_monotonic_clock(self):
        parameter = inspect.signature(RunPhaseWriter.__init__).parameters["clock"]
        self.assertIs(parameter.default, time.monotonic)
        self.assertIs(parameter.kind, inspect.Parameter.KEYWORD_ONLY)


class FramingTests(WriterCase):
    def test_long_float_digits_stay_inside_the_record_limit(self):
        channel = self.channel()
        clock = ScriptedClock(0.1, 0.30000000000000004, 1.0 / 3.0 + 1e6,
                              2.0 ** 0.5 + 1e6, 1e6 + 1.0 / 7.0 + 5.0, 1e6 + 1e5 / 3.0 + 9.0)
        writer = RunPhaseWriter(channel.fd, clock=clock)
        self.advance_all(writer, WITH_DIGEST)
        data = channel.read()
        self.check_records(self.decode(data), WITH_DIGEST)
        for line in data[:-1].split(b"\n"):
            self.assertLessEqual(len(line) + 1, 1024)

    def advance_all(self, writer, phases):
        for phase in phases:
            writer.advance(phase)

    def test_encode_record_enforces_the_1024_byte_limit_including_the_lf(self):
        exact = transport.encode_record({"p": "x" * 1015})
        self.assertEqual(len(exact), 1024)
        self.assertTrue(exact.endswith(b"\n"))
        with self.assertRaises(PhaseTransportError) as caught:
            transport.encode_record({"p": "x" * 1016})
        self.assertEqual(caught.exception.reason, "record_oversize")
        self.assertEqual(caught.exception.code, "harness_internal_error")

    def test_encode_record_refuses_values_json_cannot_hold(self):
        for bad in (float("nan"), float("inf"), float("-inf"), object(), {1, 2}):
            with self.subTest(value=repr(bad)[:30]):
                with self.assertRaises(PhaseTransportError) as caught:
                    transport.encode_record({"elapsed_ms": bad})
                self.assertEqual(caught.exception.reason, "record_invalid")

    def test_encode_record_never_leaves_a_raw_newline_inside_a_record(self):
        text = "a\nb\r\nc\u2028d\u00e9"
        line = transport.encode_record({"p": text})
        self.assertEqual(line.count(b"\n"), 1)
        self.assertTrue(line.endswith(b"\n"))
        self.assertNotIn(b"\r", line)
        self.assertFalse(line.startswith(b"\xef\xbb\xbf"))
        self.assertEqual(json.loads(line.decode("utf-8"))["p"], text)
        self.assertNotIn(b" ", transport.encode_record({"a": 1, "b": [1, 2]}))

    def test_limits_are_the_pinned_numbers(self):
        self.assertEqual(transport.PHASES, PHASES)
        self.assertEqual(transport.MAX_RECORD_BYTES, 1024)
        self.assertEqual(transport.MAX_RECORDS, 6)
        self.assertEqual(transport.MAX_UNSENT_BYTES, 1048576)
        self.assertEqual(len(transport.PHASES), transport.MAX_RECORDS)
        self.assertLessEqual(transport.MAX_RECORD_BYTES, transport.MAX_UNSENT_BYTES)


class StdoutIsNotAuthorityTests(WriterCase):
    FORGED = ('{"record":"run_phase","version":1,"sequence":99,"scope":"run","trial_id":null,'
              '"phase":"completed","elapsed_ms":0}')

    def test_nothing_is_printed_and_a_printed_record_is_not_the_transport(self):
        channel = self.channel()
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            writer = RunPhaseWriter(channel.fd)
            for phase in WITHOUT_DIGEST:
                writer.advance(phase)
                print(self.FORGED)  # what a strategy can print
            # failure paths print nothing either
            full = self.channel()
            failing = RunPhaseWriter(full.fd)
            full.fill()
            with self.assertRaises(PhaseTransportError):
                failing.advance("preflight")
            with self.assertRaises(PhaseTransportError):
                RunPhaseWriter(None).advance("serialization")
            with self.assertRaises(PhaseTransportError):
                RunPhaseWriter(1)
        self.assertEqual(stdout.getvalue(), (self.FORGED + "\n") * 5,
                         "only the forged lines: the writer printed nothing")
        self.assertEqual(stderr.getvalue(), "")
        records = self.decode(channel.read())
        self.check_records(records, WITHOUT_DIGEST)
        self.assertNotIn(99, [record["sequence"] for record in records])

    def test_a_forged_record_cannot_move_the_writer(self):
        channel = self.channel()
        stdout = io.StringIO()
        writer = RunPhaseWriter(channel.fd)
        with contextlib.redirect_stdout(stdout):
            writer.advance("preflight")
            print(self.FORGED)
            self.assertEqual((writer.phase, writer.sequence), ("preflight", 1))
            writer.advance("execution")
        self.check_records(self.decode(channel.read()), WITH_DIGEST[:2])


class SourceContractTests(unittest.TestCase):
    FORBIDDEN_CALLS = {"print", "sleep", "select", "poll", "epoll", "kqueue", "close", "dup",
                       "dup2", "fromfd", "fdopen", "makefile", "send", "sendall", "recv",
                       "setblocking", "settimeout", "wait", "join"}
    FORBIDDEN_IMPORTS = {"select", "selectors", "asyncio", "threading", "sys", "logging",
                         "subprocess", "signal"}

    @classmethod
    def setUpClass(cls):
        cls.tree = ast.parse(MODULE_PATH.read_text(encoding="utf-8"))

    def test_exactly_one_os_write_and_no_loop_around_it(self):
        parents = {}
        for parent in ast.walk(self.tree):
            for child in ast.iter_child_nodes(parent):
                parents[child] = parent
        writes = [node for node in ast.walk(self.tree)
                  if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                  and node.func.attr == "write"
                  and isinstance(node.func.value, ast.Name) and node.func.value.id == "os"]
        self.assertEqual(len(writes), 1, "one call site, one write per boundary")
        node = writes[0]
        loops = (ast.For, ast.AsyncFor, ast.While, ast.ListComp, ast.SetComp, ast.DictComp,
                 ast.GeneratorExp)
        while node in parents:
            node = parents[node]
            self.assertNotIsInstance(node, loops, "no retry or spin around the write")

    def test_no_poll_sleep_close_or_output_calls(self):
        called = set()
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Call):
                if isinstance(node.func, ast.Name):
                    called.add(node.func.id)
                elif isinstance(node.func, ast.Attribute):
                    called.add(node.func.attr)
        self.assertEqual(called & self.FORBIDDEN_CALLS, set())

    def test_no_forbidden_import_and_no_standard_stream_access(self):
        imported = set()
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                imported.add((node.module or "").split(".")[0])
        self.assertEqual(imported & self.FORBIDDEN_IMPORTS, set())
        attributes = {node.attr for node in ast.walk(self.tree) if isinstance(node, ast.Attribute)}
        self.assertEqual(attributes & {"stdout", "stderr", "stdin"}, set())


class ApiContractTests(unittest.TestCase):
    def test_constructor_and_methods_take_only_what_the_contract_names(self):
        init = inspect.signature(RunPhaseWriter.__init__).parameters
        self.assertEqual(list(init), ["self", "fd", "clock"])
        self.assertIs(init["clock"].kind, inspect.Parameter.KEYWORD_ONLY)
        self.assertEqual(list(inspect.signature(RunPhaseWriter.advance).parameters),
                         ["self", "phase"])
        self.assertEqual(list(inspect.signature(RunPhaseWriter.phase_timing).parameters),
                         ["self"])

    def test_public_surface_has_no_trial_wait_or_policy_member(self):
        public = {name for name in dir(RunPhaseWriter) if not name.startswith("_")}
        self.assertEqual(public, {"advance", "failure", "phase", "phase_timing", "sequence",
                                  "unsent", "export_requested", "last_handed_frame_bytes"})
        self.assertEqual(set(transport.__all__), {
            "MAX_RECORD_BYTES", "MAX_RECORDS", "MAX_UNSENT_BYTES", "PHASES",
            "PhaseTransportError", "RunPhaseWriter", "encode_record"})

    def test_the_error_is_a_harness_fault_that_an_oserror_handler_cannot_swallow(self):
        error = PhaseTransportError("text", reason="short_write")
        self.assertEqual(error.code, "harness_internal_error")
        self.assertEqual(PhaseTransportError.code, "harness_internal_error")
        self.assertFalse(issubclass(PhaseTransportError, OSError))
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        entry = catalog["codes"]["harness_internal_error"]
        self.assertEqual(entry["class"], "engine_fault")
        self.assertIs(entry["retryable"], False)


if __name__ == "__main__":
    unittest.main()
