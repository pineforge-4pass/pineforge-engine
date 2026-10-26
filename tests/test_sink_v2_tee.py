"""A2 black-box transactions. Real LineWriter, A, C, relay and native runner.

No replaced executables, receipts manufactured as success, or ru_maxrss memory
claims. The publication watcher starts before Popen and ends after owner reap.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import shlex
import struct
import subprocess
import sys
import threading
import time
import unittest

import test_sink_transcode_v2 as fixtures

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "scripts/sink_v2_split_driver.py"
ENGINE = None
LATE_VERIFY = "full"
BARS = 100000


def until(predicate, message, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.005)
    raise AssertionError(message)


def document(path):
    try:
        return json.loads(path.read_bytes())
    except (FileNotFoundError, ValueError):
        return {}


def vm_hwm(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) * 1024
    except FileNotFoundError:
        pass
    return None


def process_state(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()[0]
    except FileNotFoundError:
        return None


class PublicationWatch:
    def __init__(self, directory):
        self.directory = directory
        self.names = set()
        self.samples = 0
        self.stop = threading.Event()
        self.started = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.observe, daemon=True)

    def sample(self):
        self.names.update(str(path) for path in self.directory.rglob("*.pfs2"))
        self.samples += 1

    def observe(self):
        try:
            self.sample()
            self.started.set()
            while not self.stop.wait(0.01):
                self.sample()
            self.sample()
        except BaseException as error:
            self.error = error
            self.started.set()

    def start(self):
        self.thread.start()
        self.started.wait()
        return self

    def finish(self):
        self.stop.set()
        self.thread.join()
        if self.error is not None:
            raise AssertionError("publication watcher failed") from self.error


class TeeTests(fixtures.FixtureCase):
    def setUp(self):
        super().setUp()
        self.published = self.directory / "published"
        self.published.mkdir(mode=0o700)
        self.output = self.published / "out.pfs2"
        self.counter = 0

    def launch(self, *, native=False, extra=(), source=None, caps=None, tool=None):
        self.counter += 1
        label = str(self.counter)
        state, phases = self.directory / (label + "-state.json"), self.directory / (label + "-phases.json")
        progress = self.directory / (label + "-progress.bin")
        output = self.output
        if native:
            command = [sys.executable, "-B", str(DRIVER), "--so", str(ENGINE),
                       "--ohlcv", str(self.feed), "--input-tf", "1", "--script-tf", "1",
                       "--run-id", "a2-real", "--report-sink", str(output), "--report-sink-format", "2"]
        else:
            command = [sys.executable, "-B", str(DRIVER), "from-v1",
                       str(source or self.directory / "input.v1"), str(output),
                       "--summary-json", str(self.summary)]
            if caps:
                command += ["--block-records", caps]
        command += ["--tee", "--tee-state-out", str(state), "--tee-progress-out", str(progress),
                    "--phase-log-out", str(phases), *extra]
        stdout = (self.directory / (label + "-stdout")).open("w+b")
        stderr = (self.directory / (label + "-stderr")).open("w+b")
        watcher = PublicationWatch(self.published).start()
        child = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                 env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(tool or fixtures.TOOL)))
        run = dict(child=child, stdout=stdout, stderr=stderr, watcher=watcher,
                   state=state, phases=phases, progress=progress, output=output)
        self.addCleanup(self.cleanup_run, run)
        return run

    def cleanup_run(self, run):
        child = run["child"]
        if child.poll() is None:
            # Resume any test-stopped child so the real owner can cancel/reap.
            for value in document(run["state"]).values():
                if isinstance(value, dict) and type(value.get("pid")) is int:
                    try:
                        os.kill(value["pid"], signal.SIGCONT)
                    except ProcessLookupError:
                        pass
            child.send_signal(signal.SIGCONT)
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        run["watcher"].finish()
        run["stdout"].close()
        run["stderr"].close()

    def finish(self, run, success=True):
        run["child"].wait(timeout=180)
        run["watcher"].finish()
        run["stdout"].seek(0); run["stderr"].seek(0)
        raw, diagnostic = run["stdout"].read(), run["stderr"].read()
        evidence = document(run["phases"])
        self.assertEqual(bool(run["child"].returncode == 0), success, diagnostic.decode(errors="replace"))
        self.assertFalse(list(self.published.glob(".pf-tee-run-*")), diagnostic.decode(errors="replace"))
        self.assertFalse(list(self.published.rglob(".pf-transcode-v2-*")), diagnostic.decode(errors="replace"))
        self.assertGreater(run["watcher"].samples, 1)
        if not success:
            self.assertFalse(run["output"].exists())
            self.assertEqual(run["watcher"].names, set(), "a .pfs2 was visible during a cancelled transaction")
            self.assertIs(evidence["published"], False)
        else:
            self.assertIn(str(run["output"]), run["watcher"].names, "watcher never observed the successful control")
        for role in ("runner", "stage_a", "stage_c", "relay"):
            usage = evidence[role + "_child"]
            self.assertIn("os.wait4", usage["accounting"])
            self.assertIn("exit_code", usage)
            self.assertGreaterEqual(usage["reaped_monotonic_ns"], usage["start_monotonic_ns"])
        return raw, evidence

    def live(self, run):
        return until(lambda: (value if all(role + "_child" in value
                     for role in ("runner", "stage_a", "stage_c", "relay")) else None)
                     if (value := document(run["state"])) else None, "four real child pids not observed")

    def test_compact_full_serial_bytes_summary_receipts_and_commit_after_reap(self):
        for name, caps in (("COMPACT", None), ("FULL", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1")):
            with self.subTest(profile=name):
                serial = self.published / (name + "-serial.pfs2")
                command = [sys.executable, "-B", str(DRIVER), "from-v1", str(self.directory / "input.v1"),
                           str(serial), "--summary-json", str(self.summary)]
                if caps:
                    command += ["--block-records", caps]
                plain = subprocess.run(command, capture_output=True,
                                       env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(fixtures.TOOL)))
                self.assertEqual(plain.returncode, 0, plain.stderr.decode())
                # Isolate watched directory: serial oracle lives elsewhere.
                oracle = self.directory / (name + "-serial.pfs2")
                serial.rename(oracle)
                self.output = self.published / (name + "-tee.pfs2")
                run = self.launch(caps=caps, extra=("--tee-late-verify", "full"))
                raw, evidence = self.finish(run)
                self.assertEqual(raw, plain.stdout)
                self.assertEqual(self.output.read_bytes(), oracle.read_bytes())
                with fixtures.pf_sink2.SinkReader(self.output) as tee, fixtures.pf_sink2.SinkReader(oracle) as old:
                    tee.verify(); old.verify()
                    self.assertEqual(tee.profile, name)
                    self.assertEqual(tee.summary_json_bytes(), old.summary_json_bytes())
                c, a = evidence["stage_c_receipt"], evidence["stage_a_receipt"]
                self.assertEqual(c.get("late_verify", "full"), "full")
                self.assertIs(c["ready"], True)
                self.assertEqual(a["sha256"], c["input_sha256"])
                self.assertEqual(a["sink_bytes"], c["input_bytes"])
                self.assertEqual(c["input_bytes"], len(self.stream))
                prefix = self.stream[:self.stream.rfind(b'{"trailer"')]
                self.assertEqual(c["input_sha256"], hashlib.sha256(prefix).hexdigest())
                self.assertEqual(evidence["runner_stdout"]["captured_bytes"], self.summary.stat().st_size)
                self.assertIs(evidence["runner_stdout"]["eof"], True)
                self.assertIs(evidence["runner_stdout"]["overflow"], False)
                for tag in "etovkxh":
                    self.assertEqual(a["count_" + tag], c["count_" + tag])
                self.assertTrue(all(evidence[role + "_child"]["reaped_monotonic_ns"]
                                    <= evidence["commit_monotonic_ns"]
                                    for role in ("runner", "stage_a", "stage_c", "relay")))
                reached = [row["event"] for row in evidence["transcoder_phases"]]
                self.assertIn("full_verify_done", reached)
                self.assertIn("to_v1_done", reached)
                self.assertIn("fsync_close_done", reached)
                self.output.rename(self.directory / (name + "-checked.pfs2"))

    def test_missing_trailer_sees_real_bytes_and_never_publishes(self):
        bad = self.directory / "partial.v1"
        bad.write_bytes(self.stream[:self.stream.rfind(b'{"trailer"')])
        run = self.launch(source=bad, extra=("--tee-late-verify", "full"))
        _, evidence = self.finish(run, success=False)
        self.assertEqual(evidence["stage_a_child"]["exit_code"], 6)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], 6)
        self.assertGreater(evidence["relay_receipt"]["input_bytes"], 0)

    def test_structure_tee_matches_full_serial_and_binds_mode(self):
        self.structure_tee_oracle(extra=("--tee-late-verify", "structure"))

    def structure_tee_oracle(self, *, extra):
        for name, caps in (("COMPACT", None), ("FULL", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1")):
            with self.subTest(profile=name):
                oracle = self.directory / (name + "-structure-oracle.pfs2")
                command = [sys.executable, "-B", str(DRIVER), "from-v1", str(self.directory / "input.v1"),
                           str(oracle), "--summary-json", str(self.summary)]
                if caps:
                    command += ["--block-records", caps]
                full = subprocess.run(command, capture_output=True,
                                      env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(fixtures.TOOL)))
                self.assertEqual(full.returncode, 0, full.stderr.decode())
                self.output = self.published / (name + "-structure-tee.pfs2")
                tool = fixtures.TOOL
                if not extra:
                    # Record the conversion's actual argv, then exec the real
                    # tool unchanged. A future driver worker override must not
                    # hide behind a separate default-configuration probe.
                    tools = self.directory / (name + "-default-tool")
                    tools.mkdir(mode=0o700)
                    arguments = tools / "arguments.txt"
                    tool = tools / "pf_sink_transcode_v2"
                    tool.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > " + shlex.quote(str(arguments))
                                    + "\nexec " + shlex.quote(str(fixtures.TOOL)) + ' "$@"\n', encoding="utf-8")
                    tool.chmod(0o700)
                    (tools / "pf_sink_tee").symlink_to(fixtures.TOOL.with_name("pf_sink_tee"))
                run = self.launch(caps=caps, extra=extra, tool=tool)
                raw, evidence = self.finish(run)
                self.assertEqual(self.output.read_bytes(), oracle.read_bytes())
                receipt = json.loads(raw)
                self.assertEqual(receipt.pop("late_verify"), "structure")
                self.assertEqual(receipt, json.loads(full.stdout))
                self.assertEqual(evidence["stage_c_receipt"]["late_verify"], "structure")
                events = [row["event"] for row in evidence["transcoder_phases"]]
                self.assertIn("incremental_validation_done", events)
                self.assertIn("structure_verify_done", events)
                self.assertNotIn("full_verify_done", events)
                self.assertNotIn("to_v1_done", events)
                if not extra:
                    actual = arguments.read_text().splitlines()
                    self.assertFalse(any(arg.split("=", 1)[0] == "--late-threads" for arg in actual),
                                     "default tee conversion unexpectedly overrode the tool's worker default")
                    self.default_thread_receipts(evidence, caps, name, actual)

    def default_thread_receipts(self, evidence, caps, profile, arguments):
        # Conversion and --bounds use the same CLI policy/default. The tee
        # run's exec recorder proves that no worker option overrode it.
        # Infer its configured default from independent, tool-emitted receipts,
        # not from the help string, a copied admission formula or a test label.
        definitions = {row["type"]: row for row in json.loads(self.stream.splitlines()[0])["records"]}
        a, c = evidence["stage_a_receipt"], evidence["stage_c_receipt"]
        command = [
            str(fixtures.TOOL), "--bounds", "--input-bytes", str(a["sink_bytes"]),
            "--process-reserve", str(64 * 1024 * 1024),
            "--outputs", str(int("O" in definitions)), "--trace", str(int("X" in definitions)),
            "--hashes", str(int("H" in definitions)),
            "--series-slots", str(len(definitions["O"]["fields"]) - 3),
            "--constant-count", str(len(definitions["K"]["fields"])),
        ]
        if caps:
            command += ["--block-records", caps]
        for tag in "ETOVKXH":
            command += ["--count-" + tag.lower(), str(a["count_" + tag.lower()])]

        def receipt(extra=()):
            child = subprocess.run([*command, *extra], capture_output=True)
            self.assertEqual(child.returncode, 0, child.stderr.decode())
            result = json.loads(child.stdout)
            self.assertEqual(result["scope"], "conditional-address-space-admission")
            self.assertEqual(result["zstd_build"], c["zstd_build"])
            return result

        default = receipt()
        explicit = {threads: receipt(("--late-threads", str(threads))) for threads in range(1, 5)}
        matches = [threads for threads, bound in explicit.items() if bound == default]
        proof = {
            "source": "same-tool --bounds receipts; same CLI policy/input shape as the unconfigured tee",
            "profile": profile, "tool": str(fixtures.TOOL), "default_receipt": default,
            "explicit_receipts": explicit, "matching_late_threads": matches,
            "conversion_tool_arguments": arguments,
        }
        self.assertEqual(matches, [4], json.dumps(proof, separators=(",", ":")))
        # Persist/print the actual receipts and the derived count, not a
        # constant worker label. This is configuration evidence, not RSS proof.
        proof["configured_default_late_threads"] = matches[0]
        raw = json.dumps(proof, separators=(",", ":"), allow_nan=False)
        (self.directory / (profile + "-default-thread-receipts.json")).write_text(raw + "\n", encoding="utf-8")
        print(raw)

    def test_borrowed_staging_cli_skips_path_checks_but_checks_inode_and_pairing(self):
        for caps in ([], ["--block-records", "E=1,T=1,O=1,V=1,K=1,X=1,H=1,I=1"]):
            self.output = self.published / ("oracle-" + str(len(caps)) + ".pfs2")
            oracle = self.invoke(extra=caps)
            self.assertEqual(oracle.returncode, 0, oracle.stderr.decode())
            # Existing --out is intentionally passed to prove no C publication.
            with (self.directory / "borrowed").open("w+b") as staged:
                child = subprocess.run(
                    [str(fixtures.TOOL), "--staging-fd", str(staged.fileno()), "--publish", "none",
                     "--out", str(self.output), "--spool-dir", "/nonexistent-owner-check",
                     "--summary", str(self.summary), *caps],
                    input=self.stream, capture_output=True, pass_fds=(staged.fileno(),))
                self.assertEqual(child.returncode, 0, child.stderr.decode())
                staged.seek(0)
                self.assertEqual(staged.read(), self.output.read_bytes())
                ready = json.loads(child.stdout)
                self.assertIs(ready.pop("ready"), True)
                prefix = self.stream[:self.stream.rfind(b'{"trailer"')]
                self.assertEqual(ready.pop("input_sha256"), hashlib.sha256(prefix).hexdigest())
                self.assertEqual(ready, json.loads(oracle.stdout))
        for extra in (["--publish", "none"], ["--staging-fd", "0"]):
            child = self.invoke(extra=extra)
            self.assertEqual(child.returncode, 2)


class DefaultTeeTests(TeeTests):
    def test_no_mode_flags_use_structure_four_threads_and_serial_oracle_bytes(self):
        self.structure_tee_oracle(extra=())

    def test_explicit_full_remains_whole_stream_and_inverse(self):
        self.test_compact_full_serial_bytes_summary_receipts_and_commit_after_reap()


class NativeTeeTests(TeeTests):
    # This class is selected explicitly, without inheriting/rerunning base rows.
    def setUp(self):
        super().setUp()
        self.feed = self.directory / "feed.csv"
        with self.feed.open("w", encoding="ascii") as out:
            out.write("timestamp,open,high,low,close,volume\n")
            for n in range(BARS):
                out.write(f"{1735776000000 + n * 60000},100,101,99,100,1\n")

    def progress(self, run):
        try:
            raw = run["progress"].read_bytes()
            return struct.unpack("=7Q", raw) if len(raw) == 56 else None
        except FileNotFoundError:
            return None

    def streaming(self, run):
        live = self.live(run)
        until(lambda: (p if (p := self.progress(run)) and p[1] > 131072 else None),
              "relay never consumed a real mid-run stream")
        self.assertNotIn("exit_code", document(run["state"])["runner_child"])
        return live

    def stalled_slab(self, run):
        # An incomplete A slab must remain stationary while its reader is stopped.
        def blocked():
            first = self.progress(run)
            if first and first[4] and first[5] < first[4]:
                time.sleep(0.1)
                if self.progress(run) == first:
                    return first
            return None
        return until(blocked, "relay did not stop on the stopped A's slab")

    def test_failed_engine_exit_wins_and_partial_bytes_never_publish(self):
        # Native fixture throws after a real 64 KiB sink flush.
        reference = subprocess.run(
            [sys.executable, "-I", "-B", str(ROOT / "docker/run_json.py"),
             "--so", str(ENGINE), "--ohlcv", str(self.feed), "--input-tf", "1", "--script-tf", "1",
             "--inputs", '{"Mode":"throw-after-sink"}'], capture_output=True)
        self.assertNotEqual(reference.returncode, 0)
        run = self.launch(native=True, extra=("--tee-late-verify", "full", "--inputs", '{"Mode":"throw-after-sink"}'))
        _, evidence = self.finish(run, success=False)
        self.assertEqual(run["child"].returncode, reference.returncode)
        self.assertEqual(evidence["primary_failure"]["child"], "runner")
        self.assertEqual(evidence["stage_a_child"]["exit_code"], 6)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], 6)
        self.assertGreater(evidence["relay_receipt"]["input_bytes"], 0, "failure preceded all sink writes")

    def test_killed_runner_truncates_both_consumers_and_never_publishes(self):
        run = self.launch(native=True, extra=("--tee-late-verify", "full"))
        live = self.streaming(run)
        os.kill(live["runner_child"]["pid"], signal.SIGKILL)
        _, evidence = self.finish(run, success=False)
        self.assertEqual(run["child"].returncode, 128 + signal.SIGKILL)
        self.assertEqual(evidence["runner_child"]["exit_code"], -signal.SIGKILL)
        self.assertEqual(evidence["stage_a_child"]["exit_code"], 6)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], 6)

    def test_sigterm_runner_preserves_run_aborted_and_never_publishes(self):
        run = self.launch(native=True, extra=("--tee-late-verify", "full"))
        live = self.streaming(run)
        os.kill(live["runner_child"]["pid"], signal.SIGTERM)
        raw, evidence = self.finish(run, success=False)
        failure = json.loads(raw)
        self.assertEqual(evidence["runner_child"]["exit_code"], 1)
        self.assertEqual(run["child"].returncode, evidence["runner_child"]["exit_code"])
        self.assertEqual(evidence["primary_failure"]["child"], "runner")
        self.assertEqual(failure["code"], "run_aborted")
        self.assertIs(failure["args"]["strategy_started"], True)
        self.assertGreater(failure["args"]["bars_count"], 0)
        self.assertLess(failure["args"]["bars_count"], BARS)
        self.assertEqual(evidence["stage_a_child"]["exit_code"], 6)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], 6)
        self.assertGreater(evidence["relay_receipt"]["input_bytes"], 131072)

    def test_incomplete_real_stage_a_receipt_is_refused_under_watcher(self):
        run = self.launch(native=True, extra=("--tee-late-verify", "full", "--v1-a-max-bytes", "4096"))
        _, evidence = self.finish(run, success=False)
        self.assertEqual(evidence["stage_a_child"]["exit_code"], 8)
        self.assertIs(evidence["stage_a_receipt"]["complete"], False)
        self.assertGreater(evidence["stage_a_receipt"]["sink_bytes"], 0)

    def test_first_consumer_failure_beats_engine_peer_closed(self):
        run = self.launch(native=True, extra=("--tee-late-verify", "full"))
        live = self.streaming(run)
        os.kill(live["stage_c_child"]["pid"], signal.SIGKILL)
        raw, evidence = self.finish(run, success=False)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], -signal.SIGKILL)
        self.assertEqual(evidence["relay_receipt"]["first_failure"]["child"], "stage_c")
        self.assertEqual(evidence["primary_failure"]["child"], "stage_c")
        self.assertEqual(json.loads(raw)["status"], -signal.SIGKILL)
        self.assertEqual(run["child"].returncode, 128 + signal.SIGKILL)
        # This must have exercised the ranking branch, not killed an idle runner.
        self.assertIn("before engine EPIPE", evidence["primary_failure"]["detail"])

    def test_owner_abort_keeps_primary_with_real_consumer_epipe(self):
        run = self.launch(native=True, extra=("--tee-late-verify", "full"))
        live = self.streaming(run)
        a_pid = live["stage_a_child"]["pid"]
        runner_pid = live["runner_child"]["pid"]
        os.kill(a_pid, signal.SIGSTOP)
        self.stalled_slab(run)
        os.kill(run["child"].pid, signal.SIGSTOP)
        try:
            until(lambda: process_state(run["child"].pid) == "T", "owner did not stop")
            # Keep the real owner's wait4 observations pending while a real
            # consumer death releases the blocked engine through relay EPIPE.
            os.kill(a_pid, signal.SIGKILL)
            receipt_path = Path(live["run_directory"]) / "relay.json"
            first = until(lambda: value if (value := document(receipt_path)).get(
                "first_failure", {}).get("child") == "stage_a" else None,
                "relay did not record the dead A")
            self.assertEqual(first["first_failure"]["kind"], "EPIPE")
            until(lambda: process_state(runner_pid) == "Z", "runner did not exit after relay EPIPE")
            # The pending owner abort is observed before those child results.
            os.kill(run["child"].pid, signal.SIGTERM)
        finally:
            os.kill(run["child"].pid, signal.SIGCONT)
        raw, evidence = self.finish(run, success=False)
        self.assertEqual(evidence["cancel"]["child"], "owner")
        self.assertEqual(evidence["relay_receipt"]["first_failure"], first["first_failure"])
        self.assertIs(evidence["runner_peer_closed_after_consumer"], True)
        self.assertEqual(evidence["primary_failure"]["child"], "owner")
        self.assertEqual(json.loads(raw)["child"], "owner")
        self.assertEqual(json.loads(raw)["status"], 128 + signal.SIGTERM)
        self.assertEqual(run["child"].returncode, 128 + signal.SIGTERM)

    def test_stopped_a_backpressures_engine_resume_matches_serial_stream(self):
        run = self.launch(native=True, extra=("--tee-late-verify", "full"))
        live = self.streaming(run)
        a_pid, engine_pid, relay_pid = (live[role + "_child"]["pid"] for role in ("stage_a", "runner", "relay"))
        os.kill(a_pid, signal.SIGSTOP)
        try:
            stationary = self.stalled_slab(run)
            until(lambda: process_state(engine_pid) == "S" and process_state(relay_pid) == "S",
                  "real runner and relay did not enter sleeping backpressure")
            engine_io = Path(f"/proc/{engine_pid}/io").read_text()
            relay_io = Path(f"/proc/{relay_pid}/io").read_text()
            hwm_before = vm_hwm(relay_pid)
            self.assertIsNotNone(hwm_before)
            time.sleep(0.2)
            self.assertEqual(self.progress(run), stationary)
            self.assertEqual(Path(f"/proc/{engine_pid}/io").read_text(), engine_io)
            self.assertEqual(Path(f"/proc/{relay_pid}/io").read_text(), relay_io)
            self.assertEqual(process_state(engine_pid), "S")
            self.assertEqual(process_state(relay_pid), "S")
            self.assertEqual(vm_hwm(relay_pid), hwm_before)
            self.assertNotIn("exit_code", document(run["state"])["runner_child"])
        finally:
            os.kill(a_pid, signal.SIGCONT)
        _, evidence = self.finish(run)
        self.assertEqual(evidence["runner_child"]["exit_code"], 0)
        inverse = subprocess.run(
            [sys.executable, "-B", str(ROOT / "docker/pf_sink2.py"), "to-v1",
             str(run["output"]), str(self.directory / "tee.v1")], capture_output=True)
        self.assertEqual(inverse.returncode, 0, inverse.stderr.decode())
        # Independently execute the same native run through the serial v1 arm;
        # run timing differs, so compare the stream and re-encode SAME SUMMARY.
        serial_v1 = self.directory / "serial.v1"
        child = subprocess.run(
            [sys.executable, "-B", str(DRIVER), "--so", str(ENGINE), "--ohlcv", str(self.feed),
             "--input-tf", "1", "--script-tf", "1", "--run-id", "a2-real",
             "--report-sink", str(serial_v1)], capture_output=True)
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        self.assertEqual((self.directory / "tee.v1").read_bytes(), serial_v1.read_bytes())
        same_summary = self.directory / "same-summary.json"
        with fixtures.pf_sink2.SinkReader(run["output"]) as reader:
            same_summary.write_bytes(reader.summary_json_bytes())
        oracle = self.directory / "resumed-oracle.pfs2"
        child = subprocess.run(
            [sys.executable, "-B", str(DRIVER), "from-v1", str(serial_v1), str(oracle),
             "--summary-json", str(same_summary)], capture_output=True,
            env=dict(os.environ, PF_SINK_V2_TRANSCODER=str(fixtures.TOOL)))
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        self.assertEqual(oracle.read_bytes(), run["output"].read_bytes())
        relay_receipt = evidence["relay_receipt"]
        self.assertLessEqual(stationary[4], relay_receipt["slab_bytes"])
        self.assertTrue(all(capacity > 0 for capacity in relay_receipt["pipe_bytes"]))
        self.assertLessEqual(relay_receipt["slab_bytes"] + sum(relay_receipt["pipe_bytes"]), 256 * 1024)

    def test_altered_real_receipt_digest_is_refused_under_watcher(self):
        original = self.directory / "independent.v1"
        child = subprocess.run(
            [sys.executable, "-B", str(DRIVER), "--so", str(ENGINE), "--ohlcv", str(self.feed),
             "--input-tf", "1", "--script-tf", "1", "--run-id", "a2-real",
             "--report-sink", str(original)], capture_output=True)
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        lines = original.read_bytes().splitlines(keepends=True)[:-1]
        for i, line in enumerate(lines):
            if line.startswith(b'["E",'):
                row = json.loads(line)
                row[2] += 0.5
                changed = fixtures.compact(row) + b"\n"
                self.assertEqual(len(changed), len(line))
                self.assertNotEqual(changed, line)
                lines[i] = changed
                break
        else:
            self.fail("no real E record to alter")
        altered = self.directory / "altered.v1"
        altered.write_bytes(fixtures.lines_resealed(lines))
        with altered.open("rb") as stream, open(os.devnull, "wb") as null, (self.directory / "altered-a.json").open("w+b") as receipt:
            a = subprocess.run(
                [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_finalize.py"),
                 "--in-fd", "0", "--out", "-", "--max-bytes", str(altered.stat().st_size),
                 "--receipt-fd", str(receipt.fileno())],
                stdin=stream, stdout=null, stderr=subprocess.PIPE, pass_fds=(receipt.fileno(),))
            self.assertEqual(a.returncode, 0, a.stderr.decode())
            receipt.seek(0)
            altered_receipt = receipt.read()
        run = self.launch(native=True, extra=("--tee-late-verify", "full"))
        live = self.streaming(run)
        c_pid = live["stage_c_child"]["pid"]
        os.kill(c_pid, signal.SIGSTOP)
        os.kill(run["child"].pid, signal.SIGSTOP)
        try:
            os.kill(c_pid, signal.SIGCONT)
            directory = Path(live["run_directory"])
            captured = directory / "stage-a.json"
            actual = until(lambda: (value if value.get("complete") is True else None)
                           if (value := document(captured)) else None, "real A receipt not completed")
            replacement = json.loads(altered_receipt)
            self.assertIs(replacement["complete"], True)
            self.assertEqual(actual["sink_bytes"], replacement["sink_bytes"])
            for tag in "etovkxh":
                self.assertEqual(actual["count_" + tag], replacement["count_" + tag])
            self.assertNotEqual(actual["sha256"], replacement["sha256"])
            # A deliberately altered, independently certified fixture. No mock
            # process or synthetic success receipt supplies the new identity.
            captured.write_bytes(altered_receipt)
        finally:
            os.kill(run["child"].pid, signal.SIGCONT)
        _, evidence = self.finish(run, success=False)
        self.assertEqual(run["child"].returncode, 4)
        self.assertEqual(evidence["stage_c_child"]["exit_code"], 0, "C never reached ready")
        self.assertIs(evidence["stage_c_receipt"]["ready"], True)
        self.assertIn("digest disagreement", evidence["primary_failure"]["detail"])

    def measure_memory(self, bars):
        if bars != BARS:
            source = self.feed
            self.feed = self.directory / (str(bars) + "-feed.csv")
            with source.open("rb") as inp, self.feed.open("wb") as out:
                for n, line in enumerate(inp):
                    if n > bars:
                        break
                    out.write(line)
        self.output = self.published / (str(bars) + "-memory.pfs2")
        # The production split driver intentionally has no test configuration
        # plumbing. Exec the same real tool with an explicit worker count,
        # using the existing phase-launcher convention, and the REAL relay.
        tools = self.directory / "late-workers"
        tools.mkdir(mode=0o700, exist_ok=True)
        tool = tools / "pf_sink_transcode_v2"
        tool.write_text("#!/bin/sh\nexec " + shlex.quote(str(fixtures.TOOL))
                        + ' --late-threads 4 "$@"\n', encoding="utf-8")
        tool.chmod(0o700)
        relay = tools / "pf_sink_tee"
        if not relay.exists():
            relay.symlink_to(fixtures.TOOL.with_name("pf_sink_tee"))
        run = self.launch(native=True, extra=("--tee-late-verify", LATE_VERIFY), tool=tool)
        live = self.live(run)
        peaks = {"stage_c": [], "relay": []}
        first_input, last_input = None, None
        deadline = time.monotonic() + 180
        while run["child"].poll() is None:
            self.assertLess(time.monotonic(), deadline, "tee memory run stalled; no successful measurement")
            for role in peaks:
                sample = vm_hwm(live[role + "_child"]["pid"])
                if sample is not None:
                    peaks[role].append(sample)
            progress = self.progress(run)
            if progress:
                if first_input is None:
                    first_input = progress[1]
                last_input = progress[1]
            time.sleep(0.02)
        _, evidence = self.finish(run)
        self.assertGreater(last_input, first_input)
        self.assertEqual(evidence["stage_c_receipt"].get("late_verify", "full"), LATE_VERIFY)
        for role, values in peaks.items():
            self.assertGreater(len(values), 2, role + " kernel samples missing")
            self.assertGreater(max(values), 0)
            self.assertLess(max(values), 16 * 1024 * 1024, role + " VmHWM exceeds 16 MiB")
        self.assertLess(max(peaks["stage_c"]) + max(peaks["relay"]), 16 * 1024 * 1024,
                        "combined C/relay VmHWM exceeds the 16 MiB band")
        # Save actual kernel evidence for comparison between the 100k and 1M rows.
        result = {"bars": bars, "counter": "/proc/<pid>/status VmHWM", "late_verify": LATE_VERIFY,
                  "late_threads": 4,
                  "stage_c_peak_bytes": max(peaks["stage_c"]), "relay_peak_bytes": max(peaks["relay"]),
                  "input_bytes": evidence["relay_receipt"]["input_bytes"]}
        print(json.dumps(result, separators=(",", ":")))
        if bars != BARS:
            self.feed = source
        self.output.rename(self.directory / (str(bars) + "-memory-checked.pfs2"))
        self.assertEqual(evidence["stage_c_receipt"]["count_e"], bars)
        return result

    def test_kernel_vmhwm_under_band_and_flat_across_stream(self):
        small = self.measure_memory(100000) if BARS > 100000 else None
        large = self.measure_memory(BARS)
        if small is not None:
            self.assertGreater(large["input_bytes"], small["input_bytes"])
            for role in ("stage_c", "relay"):
                self.assertLessEqual(large[role + "_peak_bytes"] - small[role + "_peak_bytes"], 1024 * 1024,
                                     role + " grows by more than 1 MiB from 100k to 1M")


def main():
    global ENGINE, BARS, LATE_VERIFY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--defaults", action="store_true")
    parser.add_argument("--memory-bars", type=int)
    parser.add_argument("--late-verify", choices=("full", "structure"), default="full")
    args = parser.parse_args()
    fixtures.TOOL, fixtures.FIXTURE = args.tool.resolve(), args.fixture.resolve()
    if (args.native or args.memory_bars) and args.engine is None:
        parser.error("--native requires the real native strategy module")
    ENGINE = args.engine.resolve() if args.engine else None
    BARS = args.memory_bars or BARS
    LATE_VERIFY = args.late_verify
    if args.defaults:
        suite = unittest.TestSuite(DefaultTeeTests(name) for name in DefaultTeeTests.__dict__
                                   if name.startswith("test_"))
    elif args.memory_bars:
        suite = unittest.TestSuite([NativeTeeTests("test_kernel_vmhwm_under_band_and_flat_across_stream")])
    elif args.native:
        suite = unittest.TestSuite(NativeTeeTests(name) for name in NativeTeeTests.__dict__
                                   if name.startswith("test_") and "kernel_vmhwm" not in name)
    else:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(TeeTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == "__main__":
    raise SystemExit(main())
