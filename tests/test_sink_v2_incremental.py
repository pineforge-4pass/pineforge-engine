"""A3: real tools, independent input comparisons, codec faults, serial bytes."""
from __future__ import annotations

import argparse
import io
from itertools import product
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import types
import unittest

import generate_sink_v2_vectors as vectors
import test_sink_transcode_v2 as fixtures
from test_sink_v2_tee import PublicationWatch

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "scripts/sink_v2_split_driver.py"
PROBE = None


def serial_launcher(directory):
    # Real tool through the same private exec-shim convention as phase logs.
    path = directory / "serial-transcoder"
    path.write_text("#!/bin/sh\nexec " + shlex.quote(str(fixtures.TOOL))
                    + ' --late-threads 1 "$@"\n', encoding="utf-8")
    path.chmod(0o700)
    return path


class IncrementalTests(fixtures.FixtureCase):
    def phases(self, command, *, stream=None):
        with tempfile.TemporaryFile() as log:
            before = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            child = subprocess.run([*command, "--phase-log", str(log.fileno())],
                                   input=self.stream if stream is None else stream,
                                   pass_fds=(log.fileno(),), capture_output=True)
            after = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            log.seek(0)
            rows = [json.loads(line) for line in log.read().splitlines()]
        self.assertTrue(rows)
        times = [row["monotonic_ns"] for row in rows]
        self.assertEqual(times, sorted(times))
        self.assertGreaterEqual(times[0], before)
        self.assertLessEqual(times[-1], after)
        return child, [row["event"] for row in rows]

    def test_fixture_full_structure_and_serial_oracle_bytes_phases_and_receipts(self):
        serial = serial_launcher(self.directory)
        for name, caps in (("COMPACT", []), ("FULL", [
                "--block-records", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1"])):
            with self.subTest(profile=name):
                oracle = self.directory / (name + "-serial.pfs2")
                child = subprocess.run(
                    [sys.executable, "-B", str(DRIVER), "from-v1",
                     str(self.directory / "input.v1"), str(oracle),
                     "--summary-json", str(self.summary), *caps], capture_output=True,
                    env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(serial)))
                self.assertEqual(child.returncode, 0, child.stderr.decode())
                oracle_receipt = json.loads(child.stdout)
                for mode, threads in product(("full", "structure"), (1, 4)):
                    output = self.directory / (name + "-" + mode + "-" + str(threads) + ".pfs2")
                    command = [str(fixtures.TOOL), "--out", str(output), "--summary", str(self.summary),
                               "--late-verify", mode, "--late-threads", str(threads), *caps]
                    child, events = self.phases(command)
                    self.assertEqual(child.returncode, 0, child.stderr.decode())
                    self.assertEqual(output.read_bytes(), oracle.read_bytes())
                    receipt = json.loads(child.stdout)
                    if mode == "structure":
                        self.assertEqual(receipt.pop("late_verify"), mode)
                        self.assertNotIn("full_verify_done", events)
                        self.assertNotIn("to_v1_done", events)
                        self.assertIn("structure_verify_done", events)
                    else:
                        self.assertNotIn("late_verify", receipt)
                        self.assertIn("full_verify_done", events)
                        self.assertIn("to_v1_done", events)
                    self.assertEqual(receipt, oracle_receipt)
                    self.assertEqual(events.count("incremental_validation_done"), 1)
                    self.assertLess(events.index("input_eof"), events.index("incremental_validation_done"))
                    self.assertLess(events.index("incremental_validation_done"), events.index("summary_parsed"))
                    with fixtures.pf_sink2.SinkReader(output) as reader:
                        reader.verify()
                        self.assertEqual(reader.profile, name)
                        inverse = io.BytesIO()
                        reader.to_v1(inverse)
                        self.assertEqual(inverse.getvalue(), self.stream)

    def probe(self, mode, *, stream=None, summary=None):
        directory = self.directory / (mode + "-fault")
        directory.mkdir(mode=0o700)
        output = directory / "out.pfs2"
        watcher = PublicationWatch(directory).start()
        with tempfile.TemporaryFile() as log:
            try:
                child = subprocess.run(
                    [str(PROBE), mode, str(output), str(summary or self.summary), str(log.fileno())],
                    input=self.stream if stream is None else stream, pass_fds=(log.fileno(),),
                    capture_output=True)
            finally:
                watcher.finish()
            log.seek(0)
            events = [json.loads(line)["event"] for line in log.read().splitlines()]
        self.assertGreaterEqual(watcher.samples, 2)
        self.assertFalse(watcher.names, "a refused run published a name during observation")
        self.assertFalse(output.exists())
        self.assertFalse(list(directory.glob("*.probe-stage")))
        self.assertFalse(list(directory.glob(".pf-transcode-v2-*")))
        if mode == "codec":  # this path wires the real transcoder's phase observer
            for event in ("summary_parsed", "tail_written", "structure_verify_done", "full_verify_done", "to_v1_done"):
                self.assertNotIn(event, events, "fault escaped the incremental boundary")
        try:
            return child, json.loads(child.stdout)
        except json.JSONDecodeError:
            self.fail(f"probe {mode} gave no JSON: exit {child.returncode}, stdout {child.stdout!r}, "
                      f"stderr {child.stderr.decode(errors='replace')}")

    def test_valid_block_differs_from_input_and_I_swap_cannot_hide_behind_checksums(self):
        child, receipt = self.probe("good")
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        self.assertIs(receipt["complete"], True)
        for mode, diagnostic in (("row", "incremental input record mismatch"),
                                 ("interleave", "incremental input I order mismatch")):
            with self.subTest(fault=mode):
                child, receipt = self.probe(mode)
                self.assertEqual(child.returncode, 4, child.stderr.decode())
                self.assertIs(receipt["complete"], False)
                self.assertEqual(receipt["reason"], "v2_input_mismatch")
                self.assertIn(diagnostic, receipt["diagnostic"])
                if mode == "interleave":
                    self.assertIn(b"column CRC/SHA repaired", child.stderr)

    def test_corrupt_codec_output_is_refused_incrementally_before_summary(self):
        head = vectors.header("codec-fault")
        head["engine"] = json.loads(self.stream.splitlines()[0])["engine"]
        rows = [["E", i, 10000.0, 0.0] for i in range(4096)]
        summary = self.directory / "codec-summary.json"
        summary.write_bytes(vectors.compact(vectors.producer_metadata("codec-fault", rows)))
        stream = vectors.v1_file(head, rows)
        # Positive control uses exactly the same input through the production tool.
        self.output = self.directory / "codec-positive.pfs2"
        good = self.invoke(stream=stream, extra=("--late-verify", "structure"), summary=summary)
        self.assertEqual(good.returncode, 0, good.stderr.decode())
        child, receipt = self.probe("codec", stream=stream, summary=summary)
        self.assertEqual(child.returncode, 4, child.stderr.decode())
        self.assertIn(b"fault injected before incremental decoding", child.stderr)
        self.assertEqual(receipt["reason"], "v2_input_mismatch")
        self.assertIn("incremental validation", receipt["diagnostic"])
        self.assertIn("decoded column CRC mismatch", receipt["diagnostic"])

    def test_structure_retains_descriptor_and_close_to_reread_storage_checks(self):
        for mode in ("structure-good", "structure-full-good"):
            child, receipt = self.probe(mode)
            self.assertEqual(child.returncode, 0, f"{mode}: stdout {child.stdout!r} stderr {child.stderr.decode()}")
            self.assertIs(receipt["complete"], True)
        for mode, diagnostic in (("stored", "incremental stored data changed after validation"),
                                 ("descriptor", "incremental descriptor claims mismatch"),
                                 ("descriptor-sha", "column SHA table/block SHA mismatch"),
                                 ("descriptor-count", "stream counts mismatch")):
            with self.subTest(fault=mode):
                child, receipt = self.probe(mode)
                self.assertEqual(child.returncode, 4, child.stderr.decode())
                self.assertIs(receipt["complete"], False)
                self.assertEqual(receipt["reason"], "v2_input_mismatch")
                self.assertIn(b"full reader caught:", child.stderr)
                # the reader prefixes every refusal with "sink v2: "; the rest must be exactly the expected text
                self.assertEqual("sink v2: " + diagnostic, receipt["diagnostic"])

    def test_mode_usage_refuses_invalid_repeated_and_nontee_driver_choices(self):
        for extra in (["--late-verify", "none"], ["--late-verify", "full", "--late-verify", "structure"]):
            self.clean_refusal(self.invoke(extra=extra), 2)
        for mode in ("full", "structure"):
            child = subprocess.run(
                [sys.executable, "-B", str(DRIVER), "from-v1", str(self.directory / "input.v1"),
                 str(self.output), "--summary-json", str(self.summary), "--tee-late-verify", mode],
                capture_output=True, env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(fixtures.TOOL)))
            self.assertNotEqual(child.returncode, 0)
            self.assertIn(b"--tee-late-verify requires --tee", child.stderr)
            self.assertFalse(self.output.exists())


class VectorTests(fixtures.FixtureCase):
    def test_every_vector_generator_case_has_identical_full_structure_and_serial_bytes(self):
        version = subprocess.run([str(fixtures.TOOL), "--version"], capture_output=True)
        self.assertEqual(version.returncode, 0, version.stderr.decode())
        args = types.SimpleNamespace(version=json.loads(version.stdout), transcoder=serial_launcher(self.directory),
                                     stage_a=ROOT / "docker/pineforge_sink_finalize.py")
        profiles = set()
        for name, head, rows, summary in vectors.inputs():
            with self.subTest(vector=name), tempfile.TemporaryDirectory(dir=self.directory) as temporary:
                directory = Path(temporary)
                artifacts, evidence = vectors.generate(name, head, rows, summary, args, directory)
                profiles.add(evidence["profile"])
                for mode, threads in product(("full", "structure"), (1, 4)):
                    output = directory / (mode + "-" + str(threads) + ".pfs2")
                    child = subprocess.run(
                        [str(fixtures.TOOL), "--out", str(output), "--summary", str(directory / "stdout.json"),
                         "--late-verify", mode, "--late-threads", str(threads)],
                        input=artifacts[name + ".v1.jsonl"], capture_output=True)
                    self.assertEqual(child.returncode, 0, child.stderr.decode())
                    self.assertEqual(output.read_bytes(), artifacts[name + ".pfs2"])
                    receipt = json.loads(child.stdout)
                    if mode == "structure":
                        self.assertEqual(receipt.pop("late_verify"), mode)
                    self.assertEqual(receipt, json.loads(artifacts[name + ".receipt.json"]))
        self.assertEqual(profiles, {"COMPACT", "FULL"})


def main():
    global PROBE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--vectors", action="store_true")
    parser.add_argument("--reader-negatives", action="store_true")
    args = parser.parse_args()
    fixtures.TOOL, fixtures.FIXTURE, PROBE = args.tool.resolve(), args.fixture.resolve(), args.probe.resolve()
    if args.reader_negatives:
        # The probe replays every reader DATA negative through the incremental path; it must
        # finish with its own count line, exit 0 and the success JSON (no skip, no partial run).
        with tempfile.TemporaryDirectory() as directory:
            child = subprocess.run([str(PROBE), "reader-negatives", str(Path(directory) / "negatives"), "unused", "-1"],
                                   capture_output=True)
        stderr = child.stderr.decode(errors="replace")
        ok = (child.returncode == 0 and json.loads(child.stdout) == {"complete": True}
              and "reader DATA replay: 36 negatives and 36 positive controls, full and incremental" in stderr)
        if not ok:
            print(f"reader-negatives failed: exit {child.returncode}, stdout {child.stdout!r}, stderr {stderr}", file=sys.stderr)
        return 0 if ok else 1
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(VectorTests if args.vectors else IncrementalTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == "__main__":
    raise SystemExit(main())
