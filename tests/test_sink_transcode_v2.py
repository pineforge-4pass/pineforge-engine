"""L22T: real LineWriter fixtures, strict-reader cross-checks and pipe/RSS proof.

No committed/generated binary vectors or observed digest pins. The fixture
executable calls LineWriter and the independent direct BlockWriter path.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import resource
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import pf_sink  # noqa: E402
import pf_sink2  # noqa: E402

TOOL: Path
FIXTURE: Path


def compact(value):
    return json.dumps(value, separators=(",", ":"), allow_nan=False).encode("ascii")


def lines_resealed(lines):
    """Independent v1 trailer after a mutation; grammar failures cannot hide behind SHA."""
    head = json.loads(lines[0])
    counts = {row["type"]: 0 for row in head["records"]}
    for line in lines[1:]:
        counts[json.loads(line)[0]] += 1
    return b"".join(lines) + compact({
        "trailer": "pf-report-sink", "version": 1, "counts": counts,
        "sha256": hashlib.sha256(b"".join(lines)).hexdigest(),
    }) + b"\n"


def scalar_identity(value):
    # Python equality alone treats -0.0 and +0.0 as equal.
    if isinstance(value, float):
        return ("f64", struct.pack("<d", value))
    return (type(value).__name__, value)


class FixtureCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pf-transcode-cli-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        result = subprocess.run([str(FIXTURE), "--fixtures", str(self.directory)],
                                capture_output=True, check=False)
        if result.returncode == 77:
            self.skipTest(result.stdout.decode())
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.stream = (self.directory / "input.v1").read_bytes()
        self.summary = self.directory / "stdout.json"
        self.output = self.directory / "out.pfs2"

    def invoke(self, stream=None, extra=(), *, summary=None):
        return subprocess.run(
            [str(TOOL), "--out", str(self.output), "--summary", str(summary or self.summary),
             "--spool-dir", str(self.directory), *extra],
            input=self.stream if stream is None else stream, capture_output=True, check=False)

    def clean_refusal(self, result, status):
        self.assertEqual(result.returncode, status, result.stderr.decode())
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.directory.glob(".pf-transcode-v2-*")))

    def strict_v1(self, data):
        path = self.directory / "cross-check.v1"
        path.write_bytes(data)
        with pf_sink.SinkReader(path) as reader:
            records = list(reader)
            return records, reader.counts, reader.sha256


class TranscodeTests(FixtureCase):
    def stage_a_handoff(self, data=None):
        data = self.stream if data is None else data
        verified = self.directory / "verified.v1"
        stage = subprocess.run(
            [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
             "--in-fd", "0", "--out", str(verified), "--spool-dir", str(self.directory)],
            input=data, capture_output=True)
        self.assertEqual(stage.returncode, 0, stage.stderr.decode())
        receipt = json.loads(stage.stdout)
        self.assertIs(receipt["complete"], True)
        self.assertEqual(receipt["sink_bytes"], len(data))
        self.assertEqual(verified.read_bytes(), data)
        receipt_path = self.directory / "stage-a.json"
        receipt_path.write_bytes(stage.stdout)
        return verified, receipt_path, receipt

    def wrapper_command(self, verified, receipt_path, *, summary=None, transcoder=None):
        return [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_encode_v2.py"),
                "--transcoder", str(transcoder or TOOL), "--verified-input", str(verified),
                "--stage-a-receipt", str(receipt_path), "--summary", str(summary or self.summary),
                "--out", str(self.output), "--spool-dir", str(self.directory),
                "--spool-max-bytes", str((1 << 53) - 1), "--spool-max-files", "1"]

    @staticmethod
    def expectations(receipt):
        return ["--expect-v1-sha256", receipt["sha256"],
                "--expect-v1-bytes", str(receipt["sink_bytes"]),
                "--expect-v1-counts", ",".join(str(receipt["count_" + tag.lower()])
                                             for tag in "ETOVKXH")]

    def input_mismatch(self, result, *, consumed=None, diagnostic=None):
        self.clean_refusal(result, 4)
        receipt = json.loads(result.stdout)
        self.assertIs(receipt["complete"], False)
        self.assertEqual(receipt["code"], "report_sink_failed")
        self.assertEqual(receipt["reason"], "v2_input_mismatch")
        if consumed is not None:
            # A C++ failure receipt proves the binding ran in the consumer,
            # after trailer/EOF, rather than a Python validation shortcut.
            self.assertEqual(receipt["status"], 4)
            self.assertEqual(receipt["input_bytes"], consumed)
            self.assertEqual(receipt["sink_bytes"], 0)
        if diagnostic is not None:
            self.assertIn(diagnostic, receipt["diagnostic"])

    def resource_failure(self, result, diagnostic):
        self.clean_refusal(result, 5)
        receipt = json.loads(result.stdout)
        self.assertIs(receipt["complete"], False)
        self.assertEqual(receipt["code"], "report_sink_failed")
        self.assertEqual(receipt["reason"], "v2_resource")
        self.assertIn(diagnostic, receipt["diagnostic"])

    def test_pipe_direct_identity_all_values_and_inverse(self):
        records, counts, v1_hash = self.strict_v1(self.stream)
        result = self.invoke(extra=("--run-id", "writer-input-reference", "--level", "1"))
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        receipt = json.loads(result.stdout)
        self.assertTrue(receipt["complete"])
        self.assertEqual(receipt["input_bytes"], len(self.stream))
        self.assertEqual(receipt["sink_bytes"], self.output.stat().st_size)
        for tag in "ETOVKXH":
            self.assertEqual(receipt["count_" + tag.lower()], counts[tag])
        with pf_sink2.SinkReader(self.output) as actual, pf_sink2.SinkReader(
                self.directory / "direct.pfs2") as direct:
            actual.verify()
            direct.verify()
            self.assertEqual(actual.logical_sha256, direct.logical_sha256)
            self.assertEqual(receipt["sha256"], direct.logical_sha256)
            merged = list(actual.iter_merged())
            self.assertEqual(len(records), len(merged))
            for source, decoded in zip(records, merged):
                self.assertEqual(source.tag, decoded.tag)
                self.assertEqual(tuple(map(scalar_identity, source.values)),
                                 tuple(map(scalar_identity, decoded.values)))
            target = io.BytesIO()
            actual.to_v1(target)
            self.assertEqual(target.getvalue(), self.stream)
        self.assertEqual(hashlib.sha256(self.stream[:self.stream.rfind(b'{"trailer"')]).hexdigest(), v1_hash)
        self.assertFalse(list(self.directory.glob(".pf-transcode-v2-*")))

    def test_summary_projection_preserves_captured_bytes_and_folds(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with pf_sink2.SinkReader(self.output) as reader:
            reader.verify()
            actual = reader.summary()
            for key, value in json.loads((self.directory / "summary.json").read_bytes()).items():
                self.assertEqual(actual[key], value)
            receipt = json.loads(result.stdout)
            for key in ("closed_count", "open_at_end_count", "open_at_end_pnl"):
                self.assertEqual(scalar_identity(receipt[key]), scalar_identity(actual["sink_folds"][key]))
            self.assertGreater(receipt["block_count"], 0)
            self.assertIn(receipt["scratch_flags"], (0, 1, 2))
            version = subprocess.run([str(TOOL), "--version"], capture_output=True, check=True)
            self.assertEqual(receipt["zstd_build"], json.loads(version.stdout)["zstd_build"])

    def test_summary_projection_requires_compact_json(self):
        document = json.loads(self.summary.read_bytes())
        document["applied_inputs"]["whitespace"] = "space newline\nand tab\t"
        encoded = compact(document)
        self.assertIn(b":", encoded)
        noncompact = [json.dumps(document, indent=2, allow_nan=False).encode("ascii")]
        noncompact.extend(encoded.replace(b":", b":" + whitespace, 1)
                          for whitespace in (b" ", b"\n", b"\t", b"\r"))
        noncompact.extend((b" " + encoded, encoded[:-1] + b"\n}", encoded + b"\t"))
        summary = self.directory / "noncompact-summary.json"
        for data in noncompact:
            with self.subTest(data=data[:80]):
                summary.write_bytes(data)
                result = self.invoke(summary=summary)
                self.clean_refusal(result, 3)
                receipt = json.loads(result.stdout)
                self.assertEqual(receipt["reason"], "v2_unrepresentable")
                self.assertEqual(receipt["diagnostic"], "report JSON must be compact")
        # Whitespace inside strings and the producer's final LF remain legal.
        summary.write_bytes(encoded + b"\n")
        result = self.invoke(summary=summary)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with pf_sink2.SinkReader(self.output) as reader:
            reader.verify()
            self.assertEqual(reader.summary()["applied_inputs"]["whitespace"],
                             document["applied_inputs"]["whitespace"])

    def test_inherited_fds_and_receipt_fd(self):
        receipt_path = self.directory / "receipt.json"
        with (self.directory / "input.v1").open("rb") as source, self.summary.open("rb") as summary, receipt_path.open("wb") as receipt:
            result = subprocess.run(
                [str(TOOL), "--in-fd", str(source.fileno()), "--out", str(self.output),
                 "--summary-fd", str(summary.fileno()), "--receipt-fd", str(receipt.fileno())],
                pass_fds=(source.fileno(), summary.fileno(), receipt.fileno()), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, b"")
        self.assertTrue(json.loads(receipt_path.read_bytes())["complete"])
        self.output.unlink()
        with self.summary.open("rb") as summary:
            result = subprocess.run(
                [str(TOOL), "--out", str(self.output), "--summary", f"fd:{summary.fileno()}"],
                pass_fds=(summary.fileno(),), input=self.stream, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_tiny_pipe_chunks(self):
        with tempfile.TemporaryFile() as receipt, tempfile.TemporaryFile() as error:
            process = subprocess.Popen([str(TOOL), "--out", str(self.output),
                                        "--summary", str(self.summary)],
                                       stdin=subprocess.PIPE, stdout=receipt, stderr=error)
            try:
                for i in range(0, len(self.stream), 7):
                    process.stdin.write(self.stream[i:i + 7])
                    process.stdin.flush()
                process.stdin.close()
                status = process.wait()
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            error.seek(0)
            self.assertEqual(status, 0, error.read().decode())
        with pf_sink2.SinkReader(self.output) as reader:
            inverse = io.BytesIO()
            reader.to_v1(inverse)
            self.assertEqual(inverse.getvalue(), self.stream)

    def test_strict_grammar_rejections_agree_with_python(self):
        original = self.stream.splitlines(keepends=True)[:-1]
        first_equity = next(i for i, row in enumerate(original) if row.startswith(b'["E",'))
        corruptions = [
            b'["E",0,1,null]\n', b'["E",0,1.00,null]\n', b'["E",0,1e+00,null]\n',
            b'["E",0,NaN,null]\n', b'["E",0,1e999,null]\n', b'["E",true,1.0,null]\n',
            b'["E",9007199254740992,1.0,null]\n', b'["E",0,1.0]\n',
            b'["E",0,1.0,null,0]\n', b'["Q",0,1.0,null]\n',
            b'["E",0,1.0,null] \n', b'["E",0,1.0,null]\r\n',
        ]
        for line in corruptions:
            with self.subTest(line=line):
                lines = original.copy()
                lines[first_equity] = line
                # The original trailer stays here: malformed grammar must
                # refuse before a reader can reach its hash comparison.
                data = b"".join(lines) + self.stream[self.stream.rfind(b'{"trailer"'):]
                with self.assertRaises(pf_sink.SinkFormatError):
                    self.strict_v1(data)
                self.clean_refusal(self.invoke(data), 3)
        # Unicode, escaping, hex, header order, arity and duplicate keys.
        for old, new in [
                (b'"long"', b'"\\ud800"'),
                (b'"long"', b'"\xff"'),
                (b'"long"', b'"sideways"'),
                (b'fedcba9876543210', b'Fedcba9876543210'),
                (b'"version":1', b'"version":1,"version":1'),
                (b'"fields":["time_ms"', b'"fields":["bad_time_ms"')]:
            with self.subTest(new=new):
                self.assertIn(old, self.stream)
                data = self.stream.replace(old, new, 1)
                with self.assertRaises(pf_sink.SinkFormatError):
                    self.strict_v1(data)
                self.clean_refusal(self.invoke(data), 3)

    def test_canonical_spelling_is_required_for_byte_inverse(self):
        # pf_sink intentionally accepts equivalent JSON string escapes; the
        # canonical producer never emits these. The transcoder refuses rather
        # than claiming a byte-identical inverse for an unrepresentable spelling.
        lines = self.stream.splitlines(keepends=True)[:-1]
        target = next(i for i, row in enumerate(lines) if b"\\u000a" in row)
        lines[target] = lines[target].replace(b"\\u000a", b"\\n", 1)
        data = lines_resealed(lines)
        self.strict_v1(data)
        self.clean_refusal(self.invoke(data), 3)

    def test_failure_codes_and_no_output(self):
        trailer = self.stream.rfind(b'{"trailer"')
        incomplete = self.invoke(self.stream[:trailer])
        self.clean_refusal(incomplete, 6)
        receipt = json.loads(incomplete.stdout)
        self.assertFalse(receipt["complete"])
        self.assertEqual(receipt["input_bytes"], trailer)
        self.clean_refusal(self.invoke(self.stream[:-2]), 6)
        self.clean_refusal(self.invoke(extra=("--run-id", "wrong-run")), 3)
        self.clean_refusal(self.invoke(extra=("--max-bytes", str(len(self.stream) - 1))), 8)
        self.clean_refusal(self.invoke(self.stream + b"\n"), 3)
        corrupted = bytearray(self.stream)
        at = corrupted.rfind(b'"sha256":"') + len(b'"sha256":"')
        corrupted[at] = ord("1") if corrupted[at] == ord("0") else ord("0")
        self.clean_refusal(self.invoke(bytes(corrupted)), 4)
        self.clean_refusal(self.invoke(extra=("--spool-dir", str(self.directory / "missing"))), 5)
        self.clean_refusal(self.invoke(extra=("--level", "20")), 2)

    def test_size_limit_applies_to_projected_metadata(self):
        document = json.loads(self.summary.read_bytes())
        document["trades"] = [0] * (1024 * 1024)
        large = self.directory / "large-report.json"
        large.write_bytes(compact(document) + b"\n")
        result = self.invoke(summary=large)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.output.unlink()
        document["applied_inputs"]["too_large"] = "x" * (1024 * 1024)
        large.write_bytes(compact(document))
        result = self.invoke(summary=large)
        self.clean_refusal(result, 4)
        self.assertIn(b"summary_limit", result.stderr)

    def test_exact_summary_limit_is_admitted(self):
        document = json.loads(self.summary.read_bytes())
        projection = {key: value for key, value in document.items()
                      if key not in ("report_sink", "trades", "equity_curve")}
        projection["outputs"] = {key: projection["outputs"][key] for key in (
            "schema_version", "message_format", "manifest_sha256", "manifest", "constants", "hlines")}
        projection["padding"] = ""
        projection["sink_folds"] = json.loads((self.directory / "summary.json").read_bytes())["sink_folds"]
        padding = "x" * (1024 * 1024 - len(compact(projection)))
        document["padding"] = padding
        projection["padding"] = padding
        self.assertEqual(len(compact(projection)), 1024 * 1024)
        boundary = self.directory / "boundary.json"
        boundary.write_bytes(compact(document) + b"\n")
        result = self.invoke(summary=boundary)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with pf_sink2.SinkReader(self.output) as reader:
            self.assertEqual(reader.summary_json_length, len(compact(projection)))
        self.output.unlink()
        document["padding"] += "x"
        boundary.write_bytes(compact(document))
        self.clean_refusal(self.invoke(summary=boundary), 4)

    def test_receipt_write_failure_removes_published_file(self):
        # A read-only receipt fd fails after conversion and must not leave a
        # successful-looking object under a failing process exit code.
        with self.summary.open("rb") as receipt:
            result = subprocess.run(
                [str(TOOL), "--out", str(self.output), "--summary", str(self.summary),
                 "--receipt-fd", str(receipt.fileno())],
                pass_fds=(receipt.fileno(),), input=self.stream, capture_output=True)
        self.clean_refusal(result, 5)

    def test_spool_caps_exact_fit_and_typed_abort(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        size = self.output.stat().st_size
        self.output.unlink()
        for cap in (0, 1, size - 1):
            with self.subTest(cap=cap):
                result = self.invoke(extra=("--spool-max-bytes", str(cap)))
                self.clean_refusal(result, 9)
                self.assertEqual(json.loads(result.stdout)["reason"], "v2_spool_exhausted")
        result = self.invoke(extra=("--spool-max-bytes", str(size), "--spool-max-files", "1"))
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self.output.stat().st_size, size)
        self.output.unlink()
        self.clean_refusal(self.invoke(extra=("--spool-max-files", "0")), 10)
        result = self.invoke(extra=("--max-bytes", str(len(self.stream))))
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_fsize_is_typed_at_header_data_footer_and_tail(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        size = self.output.stat().st_size
        self.output.unlink()
        for cap in (0, 64, 4096, size - 160, size - 1, size):
            with self.subTest(fsize=cap):
                def limit():
                    resource.setrlimit(resource.RLIMIT_FSIZE, (cap, cap))
                result = subprocess.run(
                    [str(TOOL), "--out", str(self.output), "--summary", str(self.summary)],
                    input=self.stream, capture_output=True, preexec_fn=limit)
                if cap == size:
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.output.unlink()
                else:
                    self.clean_refusal(result, 9)
                    self.assertEqual(json.loads(result.stdout)["reason"], "v2_spool_exhausted")

    def test_bound_uses_verified_counts_and_pinned_codec(self):
        _, counts, _ = self.strict_v1(self.stream)
        head = json.loads(self.stream.splitlines()[0])
        slots = next(len(d["fields"]) - 3 for d in head["records"] if d["type"] == "O")
        constants = next(len(d["fields"]) for d in head["records"] if d["type"] == "K")
        command = [str(TOOL), "--bounds", "--input-bytes", str(len(self.stream)),
                   "--process-reserve", str(64 * 1024 * 1024),
                   "--series-slots", str(slots), "--constant-count", str(constants),
                   "--outputs", "1", "--trace", "1", "--hashes", "1"]
        for tag in "ETOVKXH":
            command += ["--count-" + tag.lower(), str(counts[tag])]
        bound = subprocess.run(command, capture_output=True, check=True)
        bound = json.loads(bound.stdout)
        result = self.invoke(extra=("--spool-max-bytes", str(bound["spool_bytes"]),
                                    "--spool-max-files", str(bound["spool_files"])))
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        receipt = json.loads(result.stdout)
        self.assertGreaterEqual(bound["spool_bytes"], receipt["sink_bytes"])
        self.assertGreaterEqual(bound["block_count_bound"], receipt["block_count"])
        self.assertEqual(bound["E_as"], bound["buffer_bytes"] + 64 * 1024 * 1024)

    def test_verified_only_handoff_rejects_an_unbound_stage_a_receipt(self):
        verified = self.directory / "verified.v1"
        with (self.directory / "input.v1").open("rb") as stream:
            stage = subprocess.run(
                [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
                 "--in-fd", str(stream.fileno()), "--out", str(verified),
                 "--spool-dir", str(self.directory)], pass_fds=(stream.fileno(),), capture_output=True)
        self.assertEqual(stage.returncode, 0, stage.stderr.decode())
        receipt_path = self.directory / "stage-a.json"
        receipt_path.write_bytes(stage.stdout)
        command = [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_encode_v2.py"),
                   "--transcoder", str(TOOL), "--verified-input", str(verified),
                   "--stage-a-receipt", str(receipt_path), "--summary", str(self.summary),
                   "--out", str(self.output), "--spool-dir", str(self.directory),
                   "--spool-max-bytes", str((1 << 53) - 1), "--spool-max-files", "1"]
        result = subprocess.run(command, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.output.unlink()
        receipt = json.loads(stage.stdout)
        receipt["sha256"] = "1" + receipt["sha256"][1:] if receipt["sha256"][0] != "1" else "0" + receipt["sha256"][1:]
        receipt_path.write_bytes(compact(receipt))
        self.input_mismatch(subprocess.run(command, capture_output=True),
                            consumed=len(self.stream), diagnostic="stage-A digest")

    def test_wrapper_missing_transcoder_is_resource_failure(self):
        verified, receipt_path, _ = self.stage_a_handoff()
        missing = self.directory / "missing-transcoder"
        self.assertFalse(missing.exists())
        command = self.wrapper_command(verified, receipt_path, transcoder=missing)
        self.resource_failure(subprocess.run(command, capture_output=True), str(missing))

    def test_wrapper_missing_verified_input_is_resource_failure(self):
        _, receipt_path, _ = self.stage_a_handoff()
        missing = self.directory / "missing-verified.v1"
        self.assertFalse(missing.exists())
        command = self.wrapper_command(missing, receipt_path)
        self.resource_failure(subprocess.run(command, capture_output=True), str(missing))

    def test_wrapper_missing_summary_is_resource_failure(self):
        verified, receipt_path, _ = self.stage_a_handoff()
        missing = self.directory / "missing-summary.json"
        self.assertFalse(missing.exists())
        command = self.wrapper_command(verified, receipt_path, summary=missing)
        self.resource_failure(subprocess.run(command, capture_output=True), str(missing))

    def test_stage_a_digest_binds_a_different_valid_same_size_stream(self):
        verified, receipt_path, receipt = self.stage_a_handoff()
        lines = self.stream.splitlines(keepends=True)[:-1]
        at = next(i for i, line in enumerate(lines) if line.startswith(b'["T",'))
        changed = lines[at].replace(b'"exit-', b'"Exit-', 1)
        self.assertNotEqual(changed, lines[at])
        self.assertEqual(len(changed), len(lines[at]))
        lines[at] = changed
        alternate = lines_resealed(lines)
        _, counts, digest = self.strict_v1(alternate)
        self.assertNotEqual(digest, receipt["sha256"])
        self.assertEqual(len(alternate), receipt["sink_bytes"])
        for tag in "ETOVKXH":
            self.assertEqual(counts[tag], receipt["count_" + tag.lower()])
        # Its own grammar/trailer/SUMMARY are valid: only the independent
        # stage-A digest distinguishes this stream from the certified one.
        raw = self.invoke(alternate)
        self.assertEqual(raw.returncode, 0, raw.stderr.decode())
        self.output.unlink()
        verified.write_bytes(alternate)
        result = subprocess.run(self.wrapper_command(verified, receipt_path), capture_output=True)
        self.input_mismatch(result, consumed=len(alternate), diagnostic="stage-A digest")

    def test_stage_a_counts_bind_each_record_type(self):
        verified, receipt_path, receipt = self.stage_a_handoff()
        for tag in "ETOVKXH":
            with self.subTest(tag=tag):
                altered = dict(receipt)
                altered["count_" + tag.lower()] += 1
                receipt_path.write_bytes(compact(altered))
                result = subprocess.run(self.wrapper_command(verified, receipt_path), capture_output=True)
                self.input_mismatch(result, consumed=len(self.stream), diagnostic="stage-A counts")

    def test_stage_a_counts_bind_zero_absent_types(self):
        equity = self.directory / "equity"
        equity.mkdir(mode=0o700)
        fixture = subprocess.run([str(FIXTURE), "--equity-fixtures", str(equity)], capture_output=True)
        self.assertEqual(fixture.returncode, 0, fixture.stderr.decode())
        data = (equity / "input.v1").read_bytes()
        verified, receipt_path, receipt = self.stage_a_handoff(data)
        command = self.wrapper_command(verified, receipt_path, summary=equity / "stdout.json")
        result = subprocess.run(command, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.output.unlink()
        for tag in "TOVKXH":
            with self.subTest(tag=tag):
                self.assertEqual(receipt["count_" + tag.lower()], 0)
                altered = dict(receipt)
                altered["count_" + tag.lower()] += 1
                receipt_path.write_bytes(compact(altered))
                result = subprocess.run(command, capture_output=True)
                self.input_mismatch(result, consumed=len(data), diagnostic="stage-A counts")

    def test_stage_a_size_mismatch_is_refused_by_wrapper_and_consumer(self):
        verified, receipt_path, receipt = self.stage_a_handoff()
        for delta in (-1, 1):
            with self.subTest(delta=delta):
                altered = dict(receipt)
                altered["sink_bytes"] += delta
                receipt_path.write_bytes(compact(altered))
                result = subprocess.run(self.wrapper_command(verified, receipt_path), capture_output=True)
                self.input_mismatch(result, diagnostic="receipted size")
                # Independently prove the consuming check: do not let the
                # wrapper's fstat or --max-bytes reject before C++ compares.
                result = self.invoke(extra=self.expectations(altered))
                self.input_mismatch(result, consumed=len(self.stream), diagnostic="stage-A byte length")

    def test_stage_a_binding_is_checked_before_summary(self):
        verified, receipt_path, receipt = self.stage_a_handoff()
        malformed = self.directory / "malformed-summary.json"
        malformed.write_bytes(b"not-json\n")
        command = self.wrapper_command(verified, receipt_path, summary=malformed)
        self.clean_refusal(subprocess.run(command, capture_output=True), 3)
        altered = dict(receipt)
        altered["count_h"] += 1
        receipt_path.write_bytes(compact(altered))
        self.input_mismatch(subprocess.run(command, capture_output=True),
                            consumed=len(self.stream), diagnostic="stage-A counts")

    def test_correct_stage_a_binding_preserves_serial_bytes(self):
        verified, receipt_path, receipt = self.stage_a_handoff()
        policies = [(), ("--level", "1", "--block-records", "E=3,T=5,O=3,V=4,K=1,X=6,H=7,I=11")]
        for policy in policies:
            with self.subTest(policy=policy):
                raw = self.invoke(extra=policy)
                self.assertEqual(raw.returncode, 0, raw.stderr.decode())
                serial_bytes = self.output.read_bytes()
                self.output.unlink()
                pipe = self.invoke(extra=(*policy, *self.expectations(receipt)))
                self.assertEqual(pipe.returncode, 0, pipe.stderr.decode())
                self.assertEqual(self.output.read_bytes(), serial_bytes)
                self.assertEqual(json.loads(pipe.stdout), json.loads(raw.stdout))
                self.output.unlink()
                bound = subprocess.run([*self.wrapper_command(verified, receipt_path), *policy],
                                       capture_output=True)
                self.assertEqual(bound.returncode, 0, bound.stderr.decode())
                self.assertEqual(self.output.read_bytes(), serial_bytes)
                self.assertEqual(json.loads(bound.stdout), json.loads(raw.stdout))
                with pf_sink2.SinkReader(self.output) as reader:
                    reader.verify()
                    inverse = io.BytesIO()
                    reader.to_v1(inverse)
                    self.assertEqual(inverse.getvalue(), self.stream)
                self.output.unlink()
                self.assertFalse(list(self.directory.glob(".pf-transcode-v2-*")))

    def test_bound_regular_input_must_cover_the_whole_inode(self):
        verified, _, receipt = self.stage_a_handoff()
        prefix = b"outside-the-certified-stream\n"
        verified.write_bytes(prefix + self.stream)
        with verified.open("rb") as source:
            source.seek(len(prefix))
            result = subprocess.run(
                [str(TOOL), "--in-fd", str(source.fileno()), "--out", str(self.output),
                 "--summary", str(self.summary), "--spool-dir", str(self.directory),
                 *self.expectations(receipt)],
                pass_fds=(source.fileno(),), capture_output=True)
        # Consumed digest/counts/bytes all match, but the fd denotes a larger
        # regular file. A receipt for only its suffix cannot bind that inode.
        self.input_mismatch(result, consumed=len(self.stream), diagnostic="stage-A input changed")

    def test_cli_expectations_are_all_or_none_without_repeats(self):
        _, _, receipt = self.stage_a_handoff()
        expected = self.expectations(receipt)
        for mask in range(1, 7):
            with self.subTest(partial=mask):
                partial = [value for i in range(3) if mask & (1 << i)
                           for value in expected[2 * i:2 * i + 2]]
                self.clean_refusal(self.invoke(extra=partial), 2)
        for i in range(3):
            with self.subTest(repeated=expected[2 * i]):
                self.clean_refusal(self.invoke(extra=expected + expected[2 * i:2 * i + 2]), 2)
        self.clean_refusal(self.invoke(extra=expected + ["--bounds"]), 2)
        self.clean_refusal(self.invoke(extra=expected + ["--count-e", str(receipt["count_e"])]), 2)

    def test_cli_expectations_require_canonical_sha_and_safe_integers(self):
        _, _, receipt = self.stage_a_handoff()
        expected = self.expectations(receipt)
        counts = expected[5].split(",")
        invalid = {
            1: ["", receipt["sha256"][:-1], receipt["sha256"] + "0",
                "A" + receipt["sha256"][1:], "g" + receipt["sha256"][1:]],
            3: ["", "-1", "+1", "1.0", " 1", str(1 << 53), str(1 << 64)],
            5: ["", ",".join(counts[:-1]), ",".join(counts + ["0"]),
                expected[5] + ",", ",".join([""] + counts[1:]),
                ",".join(["-1"] + counts[1:]), ",".join(["1.0"] + counts[1:]),
                ",".join([str(1 << 53)] + counts[1:])],
        }
        for at, values in invalid.items():
            for value in values:
                with self.subTest(option=expected[at - 1], value=value):
                    altered = expected.copy()
                    altered[at] = value
                    self.clean_refusal(self.invoke(extra=altered), 2)
        # The inclusive safe upper bound is syntactically valid, but these
        # expectations still disagree with independently consumed bytes/counts.
        for field in ("sink_bytes", "count_h"):
            with self.subTest(safe_max=field):
                altered = dict(receipt)
                altered[field] = (1 << 53) - 1
                self.input_mismatch(self.invoke(extra=self.expectations(altered)),
                                    consumed=len(self.stream))

    def test_wrapper_rejects_malformed_stage_a_receipt_fields(self):
        verified, receipt_path, receipt = self.stage_a_handoff()
        command = self.wrapper_command(verified, receipt_path)
        for field in ["sink_bytes", *("count_" + tag.lower() for tag in "ETOVKXH")]:
            for value in (True, False, float(receipt[field]), str(receipt[field]), None, -1, 1 << 53):
                with self.subTest(field=field, value=value):
                    altered = dict(receipt)
                    altered[field] = value
                    receipt_path.write_bytes(compact(altered))
                    self.input_mismatch(subprocess.run(command, capture_output=True))
        for value in (None, False, 1, "", receipt["sha256"][:-1], receipt["sha256"] + "0",
                      "A" + receipt["sha256"][1:], "g" + receipt["sha256"][1:]):
            with self.subTest(sha256=value):
                altered = dict(receipt)
                altered["sha256"] = value
                receipt_path.write_bytes(compact(altered))
                self.input_mismatch(subprocess.run(command, capture_output=True))
        for value in (False, 1, "true", None):
            with self.subTest(complete=value):
                altered = dict(receipt)
                altered["complete"] = value
                receipt_path.write_bytes(compact(altered))
                self.input_mismatch(subprocess.run(command, capture_output=True))
        for field in ["complete", "sha256", "sink_bytes",
                      *("count_" + tag.lower() for tag in "ETOVKXH")]:
            with self.subTest(missing=field):
                altered = dict(receipt)
                del altered[field]
                receipt_path.write_bytes(compact(altered))
                self.input_mismatch(subprocess.run(command, capture_output=True))
        for raw in (b"[]", b"null", b"{", b" " * 65537):
            with self.subTest(raw_length=len(raw)):
                receipt_path.write_bytes(raw)
                self.input_mismatch(subprocess.run(command, capture_output=True))


class MemoryTests(unittest.TestCase):
    def test_stream_and_rss_do_not_scale_with_record_count(self):
        # wait4 reports THIS exec'ed child's RSS, not a cumulative maximum of
        # fixture generators or Python. Units: bytes on Darwin, KiB on Linux.
        with tempfile.TemporaryDirectory(prefix="pf-transcode-rss-") as temp:
            directory = Path(temp)
            fixture = subprocess.run([str(FIXTURE), "--equity-fixtures", temp], capture_output=True)
            if fixture.returncode == 77:
                self.skipTest(fixture.stdout.decode())
            self.assertEqual(fixture.returncode, 0, fixture.stderr.decode())
            header = (directory / "input.v1").read_bytes().splitlines(keepends=True)[0]

            def measure(target_bytes, name):
                output = directory / name
                with tempfile.TemporaryFile() as receipt, tempfile.TemporaryFile() as error:
                    process = subprocess.Popen(
                        [str(TOOL), "--out", str(output), "--summary", str(directory / "stdout.json")],
                        stdin=subprocess.PIPE, stdout=receipt, stderr=error)
                    digest = hashlib.sha256(header)
                    byte_count, count, trades = len(header), 0, 0
                    write_error = None
                    try:
                        process.stdin.write(header)
                        while byte_count < target_bytes:
                            line = f'["E",{count},{10000.0 + (count % 64) / 4.0!r},-0.0]\n'.encode()
                            process.stdin.write(line)
                            digest.update(line)
                            byte_count += len(line)
                            count += 1
                            if count % 128 == 0:
                                trades += 1
                                trade = (
                                    f'["T",{trades},"long",{count},{count},'
                                    '1.0,1.0,1.0,0.0,0.0,0.0,0.0,0.0,0,0,false,0,0,"","",""]\n'
                                ).encode()
                                process.stdin.write(trade)
                                digest.update(trade)
                                byte_count += len(trade)
                        trailer = compact({"trailer": "pf-report-sink", "version": 1,
                                           "counts": {"E": count, "T": trades},
                                           "sha256": digest.hexdigest()}) + b"\n"
                        process.stdin.write(trailer)
                    except BrokenPipeError as exc:
                        write_error = exc
                    finally:
                        try:
                            process.stdin.close()
                        except BrokenPipeError:
                            pass
                        _, status, usage = os.wait4(process.pid, 0)
                        process.returncode = os.waitstatus_to_exitcode(status)
                    error.seek(0)
                    self.assertEqual(process.returncode, 0, error.read().decode())
                    self.assertIsNone(write_error)
                    receipt.seek(0)
                    record = json.load(receipt)
                    self.assertEqual(record["count_e"], count)
                    self.assertEqual(record["count_t"], trades)
                    self.assertEqual(record["input_bytes"], byte_count + len(trailer))
                    self.assertTrue(output.exists())
                    peak = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024
                    output.unlink()
                    return peak

            small = measure(2 * 1024 * 1024, "small.pfs2")
            large = measure(64 * 1024 * 1024, "large.pfs2")
            self.assertLessEqual(large, small + 16 * 1024 * 1024, (small, large))
            self.assertLess(large, 48 * 1024 * 1024, (small, large))
            self.assertFalse(list(directory.glob(".pf-transcode-v2-*")))
            print(f"transcoder RSS: 2 MiB stream={small} bytes; 64 MiB stream={large} bytes")


def main():
    global TOOL, FIXTURE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--memory", action="store_true")
    args = parser.parse_args()
    TOOL, FIXTURE = args.tool.resolve(), args.fixture.resolve()
    # Fail loudly if a target was not built; codec-OFF is the only expected skip.
    with tempfile.TemporaryDirectory(prefix="pf-transcode-availability-") as temp:
        availability = subprocess.run([str(FIXTURE), "--equity-fixtures", temp], capture_output=True)
    if availability.returncode == 77:
        print(availability.stdout.decode(), end="")
        return 77
    if availability.returncode:
        print(availability.stderr.decode(), file=sys.stderr)
        return 1
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(MemoryTests if args.memory else TranscodeTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
