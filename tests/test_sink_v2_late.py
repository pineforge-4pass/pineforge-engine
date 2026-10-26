"""A3(ii): real tools, serial oracle, independent late faults and worker joins."""
from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import types
import unittest

import generate_sink_v2_vectors as vectors
import test_sink_transcode_v2 as fixtures
from test_sink_v2_incremental import IncrementalTests
from test_sink_v2_tee import PublicationWatch

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "scripts/sink_v2_split_driver.py"
PROBE = None


class LateCase(fixtures.FixtureCase):
    # Reuse phase capture only, without inheriting/rerunning A3's test methods.
    phases = IncrementalTests.phases

    def setUp(self):
        super().setUp()
        self.serial_tool = self.directory / "serial-transcoder"
        self.serial_tool.write_text(
            "#!/bin/sh\nexec " + shlex.quote(str(fixtures.TOOL)) + ' --late-threads 1 "$@"\n',
            encoding="utf-8")
        self.serial_tool.chmod(0o700)
        self.probe_runs = 0

    def oracle(self, directory, caps=()):
        output = directory / "oracle.pfs2"
        child = subprocess.run(
            [sys.executable, "-B", str(DRIVER), "from-v1", str(directory / "input.v1"),
             str(output), "--summary-json", str(directory / "stdout.json"), *caps],
            capture_output=True,
            env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(self.serial_tool)))
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        return output.read_bytes(), json.loads(child.stdout)

    def compare(self, directory, stream, oracle, receipt, caps=(), workers=(1, 2, 3, 4)):
        for mode in ("full", "structure"):
            for threads in workers:
                with self.subTest(mode=mode, threads=threads):
                    output = directory / f"{mode}-{threads}.pfs2"
                    child, events = self.phases(
                        [str(fixtures.TOOL), "--out", str(output),
                         "--summary", str(directory / "stdout.json"), "--late-verify", mode,
                         "--late-threads", str(threads), *caps], stream=stream)
                    self.assertEqual(child.returncode, 0, child.stderr.decode())
                    self.assertEqual(output.read_bytes(), oracle)
                    actual = json.loads(child.stdout)
                    if mode == "structure":
                        self.assertEqual(actual.pop("late_verify"), mode)
                        self.assertEqual(events.count("rank_bucket_done"), 1)
                        self.assertEqual(events.count("structure_verify_done"), 1)
                        self.assertLess(events.index("rank_bucket_done"), events.index("structure_verify_done"))
                        self.assertLess(events.index("tail_written"), events.index("structure_verify_done"))
                        self.assertLess(events.index("structure_verify_done"), events.index("fsync_close_done"))
                        self.assertNotIn("full_verify_done", events)
                        self.assertNotIn("to_v1_done", events)
                    else:
                        self.assertNotIn("late_verify", actual)
                        self.assertIn("full_verify_done", events)
                        self.assertIn("to_v1_done", events)
                    self.assertEqual(actual, receipt)
                    if threads in (1, 4):
                        with fixtures.pf_sink2.SinkReader(output) as reader:
                            reader.verify()
                            inverse = io.BytesIO()
                            reader.to_v1(inverse)
                            self.assertEqual(inverse.getvalue(), stream)

    def probe(self, fault, threads):
        self.probe_runs += 1
        directory = self.directory / f"fault-{self.probe_runs}"
        directory.mkdir(mode=0o700)
        output = directory / "out.pfs2"
        watcher = PublicationWatch(directory).start()
        try:
            child = subprocess.run([str(PROBE), fault, str(threads), str(output)], capture_output=True)
        finally:
            watcher.finish()
        self.assertGreaterEqual(watcher.samples, 2)
        self.assertFalse(list(directory.glob("*.probe-stage")))
        try:
            receipt = json.loads(child.stdout)
        except ValueError:
            self.fail(f"probe returned {child.returncode}, {child.stdout!r}, {child.stderr!r}")
        if fault == "good":
            self.assertEqual(child.returncode, 0, child.stderr.decode())
            self.assertIs(receipt["complete"], True)
            self.assertTrue(output.exists())
            self.assertIn(str(output), watcher.names)
        else:
            self.assertEqual(child.returncode, 4, child.stderr.decode())
            self.assertIs(receipt["complete"], False)
            self.assertEqual(receipt["exception_type"], "ReaderError")
            self.assertFalse(watcher.names, "a failed worker published a name during observation")
            self.assertFalse(output.exists())
            for marker in ("rank", "figure") if fault == "both" else (fault,):
                self.assertIn((marker + " fault sealed with independent CRC/SHA repair").encode(), child.stderr)
        return child, receipt


class LateTests(LateCase):
    def test_fixture_compact_full_serial_threaded_and_serial_driver_oracle(self):
        for name, caps in (("COMPACT", []), ("FULL", [
                "--block-records", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1"])):
            directory = self.directory / name
            directory.mkdir(mode=0o700)
            (directory / "input.v1").write_bytes(self.stream)
            (directory / "stdout.json").write_bytes(self.summary.read_bytes())
            oracle, receipt = self.oracle(directory, caps)
            with fixtures.pf_sink2.open(oracle) as reader:
                reader.verify()
                self.assertEqual(reader.profile, name)
            self.compare(directory, self.stream, oracle, receipt, caps)

    def test_two_independent_failures_keep_serial_diagnostic_for_twenty_runs(self):
        for threads in (1, 4):
            self.probe("good", threads)
        _, rank = self.probe("rank", 1)
        _, figure = self.probe("figure", 1)
        self.assertIn("invalid rank permutation/comparator", rank["diagnostic"])
        self.assertIn("default figure witness mismatch", figure["diagnostic"])
        self.assertNotEqual(rank["diagnostic"], figure["diagnostic"])
        _, serial = self.probe("both", 1)
        self.assertEqual(serial["diagnostic"], rank["diagnostic"])
        for repetition in range(20):
            with self.subTest(repetition=repetition):
                _, threaded = self.probe("both", 4)
                self.assertEqual(threaded["diagnostic"], serial["diagnostic"])
                self.assertEqual(threaded["failure"], serial["failure"])
                self.assertIs(threaded["worker_origin"], True)
        # Removing only the earlier fault must expose the independent later one.
        _, later = self.probe("figure", 4)
        self.assertEqual(later["diagnostic"], figure["diagnostic"])

    def test_typed_spawned_worker_exception_reaches_caller_with_serial_exit_and_no_name(self):
        self.probe("good", 4)
        serial_child, serial = self.probe("rank", 1)
        worker_child, worker = self.probe("rank", 4)
        self.assertIs(serial["worker_origin"], False)
        self.assertIs(worker["worker_origin"], True)
        self.assertEqual(worker["exception_type"], serial["exception_type"])
        self.assertEqual(worker["failure"], serial["failure"])
        self.assertEqual(worker["diagnostic"], serial["diagnostic"])
        self.assertEqual(worker_child.returncode, serial_child.returncode)

    def test_worker_count_usage_refuses_zero_oversized_and_repeated_values(self):
        for extra in (["--late-threads", "0"], ["--late-threads", "5"],
                      ["--late-threads", "4", "--late-threads", "1"]):
            self.clean_refusal(self.invoke(extra=extra), 2)

    def test_bounds_charge_aggregate_workers_without_changing_spool_population(self):
        _, counts, _ = self.strict_v1(self.stream)
        definitions = {row["type"]: row for row in json.loads(self.stream.splitlines()[0])["records"]}
        command = [str(fixtures.TOOL), "--bounds", "--input-bytes", str(len(self.stream)),
                   "--process-reserve", str(64 * 1024 * 1024), "--outputs", "1", "--trace", "1", "--hashes", "1",
                   "--series-slots", str(len(definitions["O"]["fields"]) - 3),
                   "--constant-count", str(len(definitions["K"]["fields"]))]
        for tag in "ETOVKXH":
            command += ["--count-" + tag.lower(), str(counts[tag])]
        bounds = []
        for threads in (1, 4):
            child = subprocess.run([*command, "--late-threads", str(threads)], capture_output=True)
            self.assertEqual(child.returncode, 0, child.stderr.decode())
            bound = json.loads(child.stdout)
            self.assertEqual(bound["E_as"], bound["buffer_bytes"] + 64 * 1024 * 1024)
            bounds.append(bound)
        serial, threaded = bounds
        self.assertGreater(threaded["buffer_bytes"], serial["buffer_bytes"])
        for field in ("spool_bytes", "spool_files", "block_count_bound"):
            self.assertEqual(threaded[field], serial[field])


class VectorTests(LateCase):
    def test_committed_generator_inputs_serial_threaded_and_serial_driver_oracle(self):
        version = subprocess.run([str(self.serial_tool), "--version"], capture_output=True)
        self.assertEqual(version.returncode, 0, version.stderr.decode())
        args = types.SimpleNamespace(version=json.loads(version.stdout), transcoder=self.serial_tool,
                                     stage_a=ROOT / "docker/pineforge_sink_finalize.py")
        profiles = set()
        for name, head, rows, summary in vectors.inputs():
            with self.subTest(vector=name), tempfile.TemporaryDirectory(dir=self.directory) as temporary:
                directory = Path(temporary)
                artifacts, evidence = vectors.generate(name, head, rows, summary, args, directory)
                stream = artifacts[name + ".v1.jsonl"]
                (directory / "input.v1").write_bytes(stream)
                oracle, receipt = self.oracle(directory)
                self.assertEqual(oracle, artifacts[name + ".pfs2"])
                self.assertEqual(receipt, json.loads(artifacts[name + ".receipt.json"]))
                profiles.add(evidence["profile"])
                self.compare(directory, stream, oracle, receipt, workers=(1, 4))
        self.assertEqual(profiles, {"COMPACT", "FULL"})


class MergeTests(LateCase):
    def test_partitioned_rank_arena_crossing_has_serial_bytes_and_independent_ranks(self):
        # This population crosses the two-lane initial-run boundary while the
        # serial arena still holds one run. Final byte equality must survive a
        # different number of in-place merges, including all ten directions.
        version = subprocess.run([str(fixtures.TOOL), "--version"], capture_output=True)
        self.assertEqual(version.returncode, 0, version.stderr.decode())
        head = vectors.header("late-rank-merge")
        head["engine"] = json.loads(version.stdout)["engine"]
        rows = vectors.population_rows(16, 30853)
        stream = vectors.v1_file(head, rows)
        (self.directory / "input.v1").write_bytes(stream)
        (self.directory / "stdout.json").write_bytes(vectors.compact(vectors.producer_metadata("late-rank-merge", rows)))
        oracle, receipt = self.oracle(self.directory)
        self.compare(self.directory, stream, oracle, receipt, workers=(1, 4))


def main():
    global PROBE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--vectors", action="store_true")
    group.add_argument("--merge", action="store_true")
    args = parser.parse_args()
    fixtures.TOOL, fixtures.FIXTURE, PROBE = args.tool.resolve(), args.fixture.resolve(), args.probe.resolve()
    case = VectorTests if args.vectors else MergeTests if args.merge else LateTests
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(case)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == "__main__":
    raise SystemExit(main())
