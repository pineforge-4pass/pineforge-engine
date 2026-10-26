"""A1 black-box evidence: real LineWriter, finalizer, wrapper and transcoder."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

import test_sink_transcode_v2 as fixtures

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "scripts/sink_v2_split_driver.py"
GATE = ROOT / "scripts/sink_v2_speed_gate.py"
EVENTS = (
    "process_start", "first_input_byte", "input_eof", "stage_a_binding_done",
    "incremental_validation_done", "summary_parsed", "rank_bucket_done", "seal_root_done", "stored_digest_done",
    "tail_written", "full_verify_done", "to_v1_done", "fsync_close_done", "exit",
)
ENGINE = None


class MeasurementTests(fixtures.FixtureCase):
    def binding(self):
        verified = self.directory / "verified.v1"
        child = subprocess.run(
            [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
             "--in-fd", "0", "--out", str(verified), "--spool-dir", str(self.directory)],
            input=self.stream, capture_output=True)
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        receipt = json.loads(child.stdout)
        self.assertIs(receipt["complete"], True)
        return ["--expect-v1-sha256", receipt["sha256"],
                "--expect-v1-bytes", str(receipt["sink_bytes"]), "--expect-v1-counts",
                ",".join(str(receipt["count_" + tag.lower()]) for tag in "ETOVKXH")]

    def phase_run(self, extra=(), stream=None):
        with tempfile.TemporaryFile() as log:
            before = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            child = subprocess.run(
                [str(fixtures.TOOL), "--out", str(self.output), "--summary", str(self.summary),
                 "--spool-dir", str(self.directory), "--phase-log", str(log.fileno()), *extra],
                input=self.stream if stream is None else stream, pass_fds=(log.fileno(),),
                capture_output=True)
            after = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            log.seek(0)
            rows = [json.loads(line) for line in log.read().splitlines()]
        self.assertTrue(rows)
        times = [row["monotonic_ns"] for row in rows]
        self.assertTrue(all(type(value) is int for value in times))
        self.assertEqual(times, sorted(times))
        self.assertGreaterEqual(times[0], before)
        self.assertLessEqual(times[-1], after)
        self.assertEqual(rows[-1]["event"], "exit")
        self.assertEqual(rows[-1]["status"], child.returncode)
        for row in rows:
            if row["event"] in ("input_eof", "exit"):
                for field in ("cpu_user_ns", "cpu_sys_ns", "max_rss_kb"):
                    self.assertIs(type(row[field]), int)
                    self.assertGreaterEqual(row[field], 0)
                self.assertGreater(row["max_rss_kb"], 0)
        return child, rows

    def test_all_phases_once_in_order_and_identical_compact_and_full_bytes(self):
        binding = self.binding()
        for name, caps in (("compact", []), ("full", [
                "--block-records", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1"])):
            with self.subTest(profile=name):
                self.output = self.directory / (name + "-plain.pfs2")
                plain = self.invoke(extra=(*binding, *caps))
                self.assertEqual(plain.returncode, 0, plain.stderr.decode())
                expected = self.output.read_bytes()
                self.output = self.directory / (name + "-phases.pfs2")
                child, rows = self.phase_run([*binding, *caps])
                self.assertEqual(child.returncode, 0, child.stderr.decode())
                self.assertEqual(tuple(row["event"] for row in rows), EVENTS)
                self.assertEqual(child.stdout, plain.stdout)
                self.assertEqual(self.output.read_bytes(), expected)
                eof, end = rows[2], rows[-1]
                self.assertGreaterEqual(end["cpu_user_ns"], eof["cpu_user_ns"])
                self.assertGreaterEqual(end["cpu_sys_ns"], eof["cpu_sys_ns"])
                self.assertGreaterEqual(end["max_rss_kb"], eof["max_rss_kb"])
                with fixtures.pf_sink2.SinkReader(self.output) as reader:
                    reader.verify()
                    self.assertEqual(reader.profile, name.upper())

    def test_absent_flag_leaves_unrelated_inherited_fds_untouched(self):
        plain = self.invoke()
        self.assertEqual(plain.returncode, 0, plain.stderr.decode())
        expected = self.output.read_bytes()
        self.output = self.directory / "no-log.pfs2"
        read_fd, feed_fd = os.pipe()
        observed_fd, write_fd = os.pipe()
        try:
            os.write(feed_fd, b"unrelated-input")
            os.set_blocking(read_fd, False)
            os.set_blocking(observed_fd, False)
            with tempfile.TemporaryFile() as sentinel:
                sentinel.write(b"unchanged")
                sentinel.seek(0)
                child = subprocess.run(
                    [str(fixtures.TOOL), "--out", str(self.output), "--summary", str(self.summary)],
                    input=self.stream, capture_output=True,
                    pass_fds=(read_fd, write_fd, sentinel.fileno()))
                self.assertEqual(child.returncode, 0, child.stderr.decode())
                self.assertEqual(os.lseek(sentinel.fileno(), 0, os.SEEK_CUR), 0)
                self.assertEqual(sentinel.read(), b"unchanged")
            self.assertEqual(os.read(read_fd, 64), b"unrelated-input")
            with self.assertRaises(BlockingIOError):
                os.read(observed_fd, 64)
        finally:
            for fd in (read_fd, feed_fd, observed_fd, write_fd):
                os.close(fd)
        self.assertEqual(child.stdout, plain.stdout)
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertEqual(child.stderr, plain.stderr)

    def test_refusal_logs_only_reached_phases_and_publishes_nothing(self):
        # Existing missing-trailer fixture: no verified EOF phase is permitted.
        trailer = self.stream.rfind(b'{"trailer"')
        self.assertGreater(trailer, 0)
        child, rows = self.phase_run(stream=self.stream[:trailer])
        self.clean_refusal(child, 6)
        self.assertIs(json.loads(child.stdout)["complete"], False)
        self.assertEqual([row["event"] for row in rows], ["process_start", "first_input_byte", "exit"])
        # Existing B digest refusal reaches verified EOF, but never SUMMARY/seal.
        binding = self.binding()
        binding[1] = ("0" if binding[1][0] != "0" else "1") + binding[1][1:]
        child, rows = self.phase_run(binding)
        self.clean_refusal(child, 4)
        self.assertIs(json.loads(child.stdout)["complete"], False)
        self.assertEqual([row["event"] for row in rows],
                         ["process_start", "first_input_byte", "input_eof", "exit"])

    def driver(self, summary=None, output=None):
        phase = self.directory / "driver-phases.json"
        child = subprocess.run(
            [sys.executable, "-B", str(DRIVER), "from-v1", str(self.directory / "input.v1"),
             str(output or self.output), "--summary-json", str(summary or self.summary),
             "--phase-log-out", str(phase)],
            env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(fixtures.TOOL)), capture_output=True)
        self.assertTrue(phase.is_file(), child.stderr.decode())
        return child, json.loads(phase.read_bytes())

    def test_driver_real_wrapper_phases_and_separate_wait4_rusage(self):
        child, evidence = self.driver()
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        self.assertIs(json.loads(child.stdout)["complete"], True)
        self.assertEqual([r["event"] for r in evidence["transcoder_phases"]], list(EVENTS))
        for role in ("stage_a", "stage_c"):
            usage = evidence[role + "_child"]
            self.assertGreater(usage["pid"], 0)
            self.assertEqual(usage["exit_code"], 0)
            self.assertGreater(usage["wall_seconds"], 0)
            self.assertGreaterEqual(usage["reaped_monotonic_ns"], usage["start_monotonic_ns"])
            self.assertGreaterEqual(usage["cpu_user_seconds"], 0)
            self.assertGreaterEqual(usage["cpu_sys_seconds"], 0)
            self.assertGreater(usage["max_rss_kb"], 0)
            self.assertIn("os.wait4", usage["accounting"])
        c = evidence["stage_c_child"]
        self.assertGreater(c["cpu_user_seconds"] + c["cpu_sys_seconds"], 0)
        self.assertNotEqual(c["pid"], evidence["stage_a_child"]["pid"])
        self.assertEqual(evidence["stage_a_standalone_seconds"], evidence["stage_a_child"]["wall_seconds"])
        self.assertEqual(evidence["stage_c_standalone_seconds"], c["wall_seconds"])
        expected = self.output.read_bytes()
        self.output = self.directory / "plain-wrapper.pfs2"
        plain = subprocess.run(
            [sys.executable, "-B", str(DRIVER), "from-v1", str(self.directory / "input.v1"),
             str(self.output), "--summary-json", str(self.summary)],
            env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(fixtures.TOOL)), capture_output=True)
        self.assertEqual(plain.returncode, 0, plain.stderr.decode())
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertEqual(plain.stdout, child.stdout)

    def test_driver_refusal_retains_partial_log_without_publication(self):
        broken = self.directory / "broken-summary.json"
        broken.write_bytes(b"{")
        child, evidence = self.driver(summary=broken)
        self.assertEqual(child.returncode, 3, child.stderr.decode())
        self.assertFalse(self.output.exists())
        rows = evidence["transcoder_phases"]
        self.assertEqual([row["event"] for row in rows], [
            "process_start", "first_input_byte", "input_eof", "stage_a_binding_done",
            "incremental_validation_done", "exit"])
        self.assertEqual(rows[-1]["status"], 3)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], 3)
        self.assertNotIn("fsync_close_done", [row["event"] for row in rows])

    def test_gate_stdout_mode_checks_real_file_receipt_on_same_stream(self):
        results = {}
        for mode in ("file", "stdout"):
            phase = self.directory / (mode + "-a.json")
            output = self.directory / (mode + ".v1")
            child = subprocess.run(
                [sys.executable, "-B", str(GATE), "stage-a", str(self.directory / "input.v1"),
                 str(output), "--v1-a-mode", mode, "--phase-log-out", str(phase)],
                capture_output=True)
            self.assertEqual(child.returncode, 0, child.stderr.decode())
            results[mode] = json.loads(child.stdout)
            self.assertEqual(results[mode]["v1_a_mode"], mode)
            self.assertIs(results[mode]["receipt"]["complete"], True)
            self.assertEqual(output.read_bytes(), self.stream)
            evidence = json.loads(phase.read_bytes())
            self.assertEqual(evidence["stage_a_mode"], mode)
            self.assertGreater(evidence["stage_a_standalone_seconds"], 0)
        a, b = results["file"]["receipt"], results["stdout"]["receipt"]
        self.assertEqual(a, b)
        self.assertEqual(b["sink_bytes"], len(self.stream))
        self.assertIs(results["stdout"]["receipt_matches"], True)
        self.assertEqual(results["stdout"]["file_mode_receipt"], a)
        # Exercise the driver's stdout-only route directly: no output pathname.
        absent = self.directory / "discarded.v1"
        with open(os.devnull, "wb") as null:
            child = subprocess.run(
                [sys.executable, "-B", str(DRIVER), "stage-a", str(self.directory / "input.v1"),
                 str(absent), "--v1-a-mode", "stdout", "--v1-a-stdout-fd", str(null.fileno())],
                pass_fds=(null.fileno(),), capture_output=True)
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        self.assertEqual(json.loads(child.stdout), a)
        self.assertFalse(absent.exists())


class SamplerTests(fixtures.FixtureCase):
    def test_real_driver_rss_series_roles_peak_time_and_existing_accounting(self):
        spec = importlib.util.spec_from_file_location("measurement_speed_gate", GATE)
        gate = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = gate
        spec.loader.exec_module(gate)
        # Existing whole-archive strategy module, actual ctypes engine, real feed.
        # Enough production/verification work to observe A and C at 20ms.
        feed = self.directory / "feed.csv"
        feed.write_text("timestamp,open,high,low,close,volume\n" + "".join(
            f"{1735776000000 + n * 60000},100,101,99,100,1\n" for n in range(30000)), encoding="ascii")
        temporary, scratch = self.directory / "temp", self.directory / "scratch"
        temporary.mkdir()
        scratch.mkdir()
        phase = self.directory / "engine-phases.json"
        result = gate.run_child(
            [sys.executable, "-B", str(DRIVER), "--so", str(ENGINE), "--ohlcv", str(feed),
             "--input-tf", "1", "--script-tf", "1", "--report-sink", str(self.output),
             "--report-sink-format", "2", "--run-id", "a1-rss", "--phase-log-out", str(phase)],
            self.directory / "receipt.json", self.directory / "stderr",
            memory_paths={"output": self.output, "temporary": temporary, "scratch": scratch},
            child_env={"TMPDIR": str(temporary), "PF_SINK_V2_TRANSCODER": str(fixtures.TOOL)})
        self.assertEqual(result["exit_code"], 0, (self.directory / "stderr").read_text())
        series = result["rss_time_series"]
        self.assertTrue(series)
        self.assertEqual(result["memory_sample_interval_seconds"], 0.02)
        self.assertEqual(result["rss_time_series_cap"], gate.RSS_TIME_SERIES_CAP)
        self.assertLessEqual(len(series), result["rss_time_series_cap"])
        self.assertEqual(result["rss_time_series_dropped"], 0)
        self.assertTrue({"engine", "A", "C"}.issubset({r["role"] for r in series}))
        self.assertEqual([r["monotonic_ns"] for r in series], sorted(r["monotonic_ns"] for r in series))
        engine = [r for r in series if r["role"] == "engine"]
        peak = max(engine, key=lambda row: row["rss_bytes"])
        self.assertEqual(result["engine_peak_sample_rss_bytes"], peak["rss_bytes"])
        self.assertEqual(result["engine_peak_sample_monotonic_ns"], peak["monotonic_ns"])
        self.assertGreaterEqual(result["peak_accounted_rss_bytes"], result["peak_rss_bytes"])
        self.assertGreaterEqual(result["peak_accounted_rss_bytes"], result["peak_sampled_process_rss_bytes"])
        evidence = json.loads(phase.read_bytes())
        self.assertGreaterEqual(evidence["engine_finished_observed_ns"], evidence["engine_start_monotonic_ns"])
        self.assertGreaterEqual(evidence["driver_sink_fd_closed_ns"], evidence["engine_finished_observed_ns"])
        self.assertGreaterEqual(evidence["engine_sink_fd_closed_observed_ns"],
                                evidence["engine_start_monotonic_ns"])
        self.assertIsNone(evidence["engine_last_sink_write_ns"])
        self.assertEqual([r["event"] for r in evidence["transcoder_phases"]], list(EVENTS))
        self.assertGreaterEqual(evidence["stage_c_child"]["start_monotonic_ns"],
                                evidence["stage_a_child"]["reaped_monotonic_ns"])
        report = json.loads((self.directory / "receipt.json").read_bytes())
        self.assertGreater(report["sink_stage_c_timing"]["engine_thread_cpu_seconds"], 0)
        self.assertGreater(report["sink_stage_c_timing"]["writer_thread_cpu_seconds"], 0)


def main():
    global ENGINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--sampler", action="store_true")
    parser.add_argument("--engine", type=Path)
    args = parser.parse_args()
    fixtures.TOOL, fixtures.FIXTURE = args.tool.resolve(), args.fixture.resolve()
    if args.sampler and args.engine is None:
        parser.error("--sampler requires the real --engine strategy module")
    ENGINE = args.engine.resolve() if args.engine else None
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SamplerTests if args.sampler else MeasurementTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    # These rows register only with codec ON; an unavailable fixture is no proof.
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == "__main__":
    raise SystemExit(main())
