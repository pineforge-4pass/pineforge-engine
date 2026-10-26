"""CLI orchestration with real A/C/relay and deliberately different bytes.

Build inventory, scheduling and the native seed-producing job are fixture
substitutes. Negative cases explicitly change real encoder policy or bytes, or
misreport a completed driver's late mode. The gate's experiment/comparison,
tools, hashes, raw rows, summary and receipts are real. The reduced schedule
always leaves the performance verdict FAIL; these rows claim no independent builds.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import unittest
from unittest.mock import patch

import test_sink_transcode_v2 as fixtures
import test_sink_v2_speed_gate as gate_tests

gate = gate_tests.gate
ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
DRIVER = ROOT / "scripts/sink_v2_split_driver.py"
MODES = ["tee-full", "tee-structure-t4", "tee-structure-t1", "serial-full"]


def seed_report(argv):
    source, metadata, output = map(Path, argv)
    child = subprocess.run(
        [sys.executable, "-B", str(DRIVER), "stage-a", str(source), str(output)],
        capture_output=True, check=False)
    if child.returncode:
        sys.stderr.buffer.write(child.stderr)
        return child.returncode
    receipt = json.loads(child.stdout)
    report = json.loads(metadata.read_bytes())
    report["report_sink"] = {
        **receipt, "version": 1,
        "counts": {tag: receipt["count_" + tag.lower()] for tag in gate.TAGS},
    }
    print(json.dumps(report, separators=(",", ":"), allow_nan=False))
    return 0


class LateModeGateTests(fixtures.FixtureCase):
    def exercise_gate(self, *, corrupt=False, timed_variant=False, compare=True, mode="tee",
                      late_verify=None, keep_artifacts=None, misreport_timed_mode=False):
        builds = gate_tests.fake_builds(self.directory / "inventory")
        self.timed_observed = []
        for build in builds:
            binary = Path(build) / "build/bin"
            binary.mkdir()
            tool = binary / "pf_sink_transcode_v2"
            if corrupt:
                tool.write_text(
                    "#!/bin/sh\nexec " + shlex.quote(sys.executable) + " -B "
                    + shlex.quote(str(ROOT / "tests/sink_v2_late_mode_corrupt.py")) + " "
                    + shlex.quote(str(fixtures.TOOL)) + ' "$@"\n', encoding="utf-8")
                tool.chmod(0o700)
            elif timed_variant:
                tool.write_text(
                    "#!/bin/sh\nexec " + shlex.quote(sys.executable) + " -B "
                    + shlex.quote(str(ROOT / "tests/sink_v2_late_mode_timed.py")) + " "
                    + shlex.quote(str(fixtures.TOOL)) + " " + shlex.quote(str(binary / "timed-policy.txt"))
                    + ' "$@"\n', encoding="utf-8")
                tool.chmod(0o700)
            else:
                tool.symlink_to(fixtures.TOOL)
            (binary / "pf_sink_tee").symlink_to(fixtures.TOOL.with_name("pf_sink_tee"))
        # Add actual fixture population facts required by the gate's report
        # contract. The same compact JSON then seeds every encoding unchanged.
        bars = sum(line.startswith(b'["E",') for line in self.stream.splitlines())
        self.assertGreater(bars, 0)
        metadata = json.loads(self.summary.read_bytes())
        metadata["diagnostics"]["script_bars_processed"] = bars
        metadata["report_window"]["counts"] = {"window_script_bars": bars, "anchor_points": 0}
        seed_meta = self.directory / "seed-report.json"
        seed_meta.write_text(json.dumps(metadata, separators=(",", ":"), allow_nan=False), encoding="utf-8")
        output = self.directory / "gate"
        arguments = [
            "--build-roots", *builds, "--sizes", str(bars), str(bars + 1), str(bars + 2),
            "--strategies", "validation/fake", "--levels", "3",
            "--ohlcv", str(self.directory / "input.v1"), "--output", str(output),
            "--bootstrap-iterations", "200", "--skip-size-grid",
            "--zstd-library", os.environ["PF_SINK_ZSTD_LIBRARY"], "--v2-mode", mode,
        ]
        if compare:
            arguments += ["--v2-compare-late-modes"]
        if keep_artifacts is None:
            keep_artifacts = not (corrupt or timed_variant)
        if keep_artifacts:
            arguments += ["--keep-artifacts"]  # Reread successful files independently below.
        if late_verify is not None:
            arguments += ["--v2-late-verify", late_verify]
        real_schedule, real_run_child = gate.schedule, gate.run_child
        schedule_calls, seed_commands = [], []

        def fixture_schedule(plan):
            schedule_calls.append(True)
            if len(schedule_calls) > 1:
                return real_schedule(plan)  # Analysis must reject the missing production rows.
            return iter([
                gate.Job("backtest", builds[0], bars, "validation/fake", "v1", 0, True),
                gate.Job("encode", builds[0], bars, "validation/fake", "v2-L3", 0, True),
                gate.Job("encode", builds[0], bars, "validation/fake", "v2-L3", 0, False),
            ])

        def fixture_child(command, *args, **kwargs):
            if "_worker" in command:
                self.assertEqual(command[command.index("--report-sink-format") + 1], "1")
                sink = command[command.index("--report-sink") + 1]
                seed_commands.append(sink)
                command = [sys.executable, "-B", str(SELF), "_seed-report",
                           str(self.directory / "input.v1"), str(seed_meta), sink]
            observation = real_run_child(command, *args, **kwargs)
            if timed_variant and "from-v1" in command and observation["exit_code"] == 0:
                # Independent test evidence, after the real timer and sampler
                # stopped, before the gate can delete the timed output.
                sink = Path(command[command.index("from-v1") + 2])
                data = sink.read_bytes()
                self.timed_observed.append({"path": str(sink), "sha256": hashlib.sha256(data).hexdigest(),
                                            "bytes": len(data)})
            if misreport_timed_mode and "from-v1" in command and observation["exit_code"] == 0:
                # Test the gate's receipt check independently of the owner's
                # own check, by altering stdout only after the driver exits.
                stdout = args[0]
                receipt = gate.load_json(stdout)
                receipt["late_verify"] = "full"
                gate.dump_json(stdout, receipt)
            return observation

        # Inventory is explicitly synthetic, as in the pre-existing gate
        # orchestration suite. Every from-v1 and comparison uses real children.
        with patch.object(gate, "inspect_split_proofs", return_value=None), \
                patch.object(gate, "prepare_feeds", return_value={("validation/fake", bars): self.summary}), \
                patch.object(gate, "schedule", side_effect=fixture_schedule), \
                patch.object(gate, "run_child", side_effect=fixture_child), \
                contextlib.redirect_stdout(io.StringIO()):
            status = gate.main(arguments)
        self.assertEqual(status, 1, (output / "failure.json").read_text()
                         if (output / "failure.json").exists() else "incomplete fixture must stay FAIL")
        self.assertEqual(len(seed_commands), 1)
        rows = [json.loads(line) for line in (output / "raw.jsonl").read_text().splitlines()]
        summary = gate.load_json(output / "summary.json")
        receipt = gate.load_json(output / "late-modes-receipt.json")
        self.assertEqual(summary["gate"], "FAIL")
        self.assertEqual(summary["late_mode_comparison"], receipt)
        plan = gate.load_json(output / "manifest.json")["plan"]
        self.assertEqual(plan["v2_compare_late_modes"], compare)
        self.assertEqual(plan["v2_late_verify"], late_verify or ("structure" if mode == "tee" else "full"))
        self.assertEqual(receipt["v2_late_verify"], plan["v2_late_verify"])
        self.assertEqual(receipt["v2_mode"], mode)
        if compare:
            seeds = [json.loads(line) for line in (output / "late-mode-seeds.jsonl").read_text().splitlines()]
            self.assertEqual(summary["late_mode_seeds"], seeds)
        else:
            self.assertEqual(summary["late_mode_seeds"], [])
            self.assertFalse((output / "late-modes").exists())
        return rows, summary, receipt

    def assert_after_timed_rows(self, cell, rows):
        last_end = max(row["start_monotonic_ns"] + int(row["wall_seconds"] * 1e9) for row in rows)
        self.assertGreaterEqual(cell["started_monotonic_ns"], last_end)
        for item in cell["outputs"][:4]:
            phases = gate.load_json(Path(item["phase_log_path"]))["transcoder_phases"]
            self.assertTrue(phases, "real replay phase times are required")
            self.assertGreaterEqual(min(phase["monotonic_ns"] for phase in phases), last_end)

    def test_same_seed_four_real_outputs_agree_and_stay_outside_timed_rows(self):
        rows, summary, receipt = self.exercise_gate()
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["ok"] for row in rows), [row.get("error") for row in rows])
        self.assertIs(receipt["equal"], True)
        self.assertEqual(receipt["cells_compared"], 1, "replicates must share one seed replay")
        self.assertEqual(receipt["missing"], 0)
        self.assertEqual(len(summary["late_mode_seeds"]), 1)
        self.assertTrue(summary["late_mode_seeds"][0]["ok"])
        for row in rows[1:]:
            self.assertEqual(row["v2_late_verify"], "structure")
            self.assertEqual(row["command"][-3:], ["--tee", "--tee-late-verify", "structure"])
            self.assertNotIn("late_mode_comparison", row)
        cell = receipt["cells"][0]
        self.assert_after_timed_rows(cell, rows)
        self.assertEqual(cell["modes_compared"], MODES + ["timed-structure"])
        replays, timed = cell["outputs"][:4], cell["outputs"][4:]
        self.assertEqual([item["mode"] for item in replays], MODES)
        self.assertEqual(gate.load_json(Path(cell["directory"]) / "receipt.json"), cell)
        self.assertEqual(Path(cell["directory"]).parent, self.directory.resolve() / "gate/late-modes"
                         / f"0-{rows[0]['bars']}-validation/fake")
        self.assertEqual(len({Path(item["path"]).stat().st_ino for item in cell["outputs"]}), 6)
        self.assertEqual(len(timed), 2, "both the warm-up and measured encode must be checked")
        self.assertEqual([item["sequence"] for item in timed], [row["sequence"] for row in rows[1:]])
        self.assertEqual([item["warmup"] for item in timed], [True, False])
        for item, row in zip(timed, rows[1:]):
            self.assertEqual(item["mode"], "timed-structure")
            self.assertEqual(item["path"], row["sink_path"])
            self.assertEqual(item["sha256"], row["metadata_provenance"]["artifact_sha256"])
            self.assertEqual(item["bytes"], row["sink_bytes"])
        for item in cell["outputs"]:
            data = Path(item["path"]).read_bytes()
            self.assertIs(item["retained"], True)
            self.assertEqual(hashlib.sha256(data).hexdigest(), cell["sha256"])
            self.assertEqual(len(data), cell["bytes"])
            self.assertEqual(item["sha256"], cell["sha256"])
            self.assertEqual(item["bytes"], cell["bytes"])
        for item in replays:
            phases = gate.load_json(Path(item["phase_log_path"]))["transcoder_phases"]
            events = [phase["event"] for phase in phases]
            self.assertIn("incremental_validation_done", events)
            if item["late_verify"] == "structure":
                self.assertIn("structure_verify_done", events)
                self.assertNotIn("full_verify_done", events)
                self.assertNotIn("to_v1_done", events)
            else:
                self.assertIn("full_verify_done", events)
                self.assertIn("to_v1_done", events)
        self.assertEqual(summary["v2_late_verify"], "structure")
        text = (self.directory / "gate/summary.txt").read_text()
        self.assertIn("v2_late_verify=structure (resolved)", text)
        self.assertIn("UNTIMED LATE-MODE BYTES", text)

    def test_finished_full_output_byte_change_refuses_gate_and_keeps_both_digests(self):
        rows, summary, receipt = self.exercise_gate(corrupt=True)
        self.assertEqual(len(rows), 3, "replay refusal must follow every timed row")
        self.assertTrue(all(row["ok"] for row in rows), [row.get("error") for row in rows])
        failed = summary["late_mode_seeds"][0]
        self.assertFalse(failed["ok"])
        self.assertTrue(all(row["v2_late_verify"] == "structure" and row["exit_code"] == 0
                            for row in rows[1:]), "timed structure controls must succeed")
        self.assertEqual(failed["refusal_kind"], "late_mode_mismatch")
        self.assertIn("LateModeMismatch:", failed["error"])
        self.assertIs(receipt["equal"], False)
        cell = failed["late_mode_comparison"]
        self.assert_after_timed_rows(cell, rows)
        self.assertEqual(cell["modes_compared"], MODES + ["timed-structure"])
        self.assertEqual(len(cell["outputs"]), 6)
        self.assertIn(str(cell["cell"]), failed["error"])
        self.assertEqual(gate.load_json(Path(cell["directory"]) / "receipt.json"), cell)
        full, *controls = cell["outputs"][:4]
        for item in cell["outputs"][:4]:
            self.assertIs(item["retained"], True, "differences must survive without --keep-artifacts")
            data = Path(item["path"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
            self.assertEqual(len(data), item["bytes"])
        self.assertNotEqual(full["sha256"], controls[-1]["sha256"])
        self.assertEqual(full["bytes"], controls[-1]["bytes"])
        self.assertIn(full["sha256"], failed["error"])
        self.assertIn(controls[-1]["sha256"], failed["error"])
        full_bytes, oracle = Path(full["path"]).read_bytes(), Path(controls[-1]["path"]).read_bytes()
        self.assertEqual(full_bytes[0], oracle[0] ^ 1)
        self.assertEqual(full_bytes[1:], oracle[1:])
        for control in controls[:-1]:
            self.assertEqual(control["sha256"], controls[-1]["sha256"])
        for timed in cell["outputs"][4:]:
            self.assertEqual(timed["sha256"], controls[-1]["sha256"])
            self.assertEqual(timed["bytes"], controls[-1]["bytes"])
        events = [row["event"] for row in gate.load_json(Path(full["phase_log_path"]))["transcoder_phases"]]
        self.assertIn("test_output_byte_flipped", events)
        self.assertTrue(any("late_mode_mismatch" in problem for problem in summary["problems"]))

    def test_recorded_timed_warmup_bytes_refuse_even_when_all_four_replays_agree(self):
        rows, summary, receipt = self.exercise_gate(timed_variant=True)
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["ok"] and row["exit_code"] == 0 for row in rows),
                        [row.get("error") for row in rows])
        failed = summary["late_mode_seeds"][0]
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["refusal_kind"], "late_mode_mismatch")
        self.assertIn("LateModeMismatch:", failed["error"])
        self.assertIs(receipt["equal"], False)
        self.assertEqual(receipt["missing"], 0, "the unequal seed must remain present")
        cell = failed["late_mode_comparison"]
        self.assert_after_timed_rows(cell, rows)
        self.assertIn(str(cell["cell"]), failed["error"])
        replays, timed = cell["outputs"][:4], cell["outputs"][4:]
        self.assertEqual([item["mode"] for item in replays], MODES)
        self.assertEqual(len(timed), 2)
        oracle = replays[-1]
        for item in replays:
            data = Path(item["path"]).read_bytes()
            self.assertIs(item["retained"], True)
            self.assertEqual(hashlib.sha256(data).hexdigest(), oracle["sha256"])
            self.assertEqual(len(data), oracle["bytes"])
        for item, row, observed in zip(timed, rows[1:], self.timed_observed):
            self.assertEqual(item["mode"], "timed-structure")
            self.assertEqual(item["path"], observed["path"])
            self.assertEqual(item["sha256"], observed["sha256"])
            self.assertEqual(item["sha256"], row["metadata_provenance"]["artifact_sha256"])
            self.assertEqual(item["bytes"], observed["bytes"])
            self.assertEqual(item["bytes"], row["sink_bytes"])
            self.assertFalse(Path(item["path"]).exists(), "comparison must use recorded, deleted timed bytes")
            self.assertIs(item["retained"], False)
        self.assertEqual(len(self.timed_observed), 2)
        self.assertTrue(timed[0]["warmup"], "the deliberately different output is the warm-up")
        self.assertFalse(timed[1]["warmup"])
        self.assertNotEqual(timed[0]["sha256"], oracle["sha256"])
        self.assertEqual((timed[1]["sha256"], timed[1]["bytes"]), (oracle["sha256"], oracle["bytes"]))
        for item in (timed[0], oracle):
            self.assertIn(item["mode"], failed["error"])
            self.assertIn(item["sha256"], failed["error"])
            self.assertIn(f"bytes={item['bytes']}", failed["error"])
        self.assertTrue((Path(rows[0]["build"]) / "build/bin/timed-policy.txt").is_file())
        self.assertEqual(gate.load_json(Path(cell["directory"]) / "receipt.json"), cell)
        self.assertTrue(any("late_mode_mismatch" in problem for problem in summary["problems"]))

    def test_deleted_timed_output_without_recorded_digest_refuses(self):
        rows, _, _ = self.exercise_gate(compare=False, keep_artifacts=False)
        self.assertTrue(all(row["ok"] for row in rows), [row.get("error") for row in rows])
        seed, timed = rows[:2]
        self.assertFalse(Path(timed["sink_path"]).exists())
        directory = self.directory / "missing-timed-digest"
        directory.mkdir()
        args = argparse.Namespace(v2_mode="tee", codec=DRIVER, keep_artifacts=False)
        with self.assertRaises(gate.LateModeMismatch) as raised:
            gate.compare_late_modes(
                args, Path(seed["sink_path"]), Path(seed["artifact_dir"]) / "summary-meta.json",
                directory, seed["build"], {"missing_timed_digest": timed["sequence"]}, 3,
                timed_outputs=[{"mode": "timed-structure", "path": timed["sink_path"],
                                "bytes": timed["sink_bytes"], "retained": False}])
        cell = raised.exception.evidence
        self.assertIs(cell["equal"], False)
        self.assertIsNone(cell["outputs"][-1]["sha256"])
        self.assertEqual(cell["refusal_kind"], "late_mode_mismatch")
        self.assertIn("timed-structure sha256=None", str(raised.exception))
        self.assertIn(cell["outputs"][3]["sha256"], str(raised.exception))
        for item in cell["outputs"][:4]:
            self.assertTrue(Path(item["path"]).is_file())
            self.assertIs(item["retained"], True)
        self.assertEqual(gate.load_json(Path(cell["directory"]) / "receipt.json"), cell)

    def test_comparison_is_opt_in_and_explicit_full_is_forwarded(self):
        rows, _, receipt = self.exercise_gate(compare=False, late_verify="full")
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["ok"] for row in rows), [row.get("error") for row in rows])
        self.assertIs(receipt["enabled"], False)
        self.assertEqual(receipt["cells"], [])
        for row in rows[1:]:
            self.assertEqual(row["v2_late_verify"], "full")
            self.assertEqual(row["command"][-3:], ["--tee", "--tee-late-verify", "full"])
            self.assertNotIn("late_mode_comparison", row)
            self.assertFalse((Path(row["artifact_dir"]) / "late-mode-comparison").exists())

    def test_timed_encode_receipt_must_bind_the_resolved_mode(self):
        rows, summary, receipt = self.exercise_gate(compare=False, misreport_timed_mode=True)
        self.assertEqual(len(rows), 2, "the first wrong timed receipt must stop the experiment")
        failed = rows[-1]
        self.assertEqual(failed["exit_code"], 0, "the real driver must finish successfully")
        self.assertEqual(failed["v2_late_verify"], "structure")
        self.assertFalse(failed["ok"])
        self.assertIn("tee encode receipt does not bind requested late verification", failed["error"])
        self.assertTrue(any(failed["error"] in problem for problem in summary["problems"]))
        self.assertFalse(receipt["enabled"])
        self.assertEqual(receipt["cells"], [])

    def test_serial_unset_still_uses_full_without_tee(self):
        rows, summary, receipt = self.exercise_gate(compare=False, mode="serial")
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(row["ok"] for row in rows), [row.get("error") for row in rows])
        self.assertEqual(summary["v2_late_verify"], "full")
        self.assertEqual(receipt["cells"], [])
        for row in rows[1:]:
            self.assertEqual(row["v2_late_verify"], "full")
            self.assertNotIn("--tee", row["command"])
            self.assertNotIn("--tee-late-verify", row["command"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--negative", action="store_true")
    args = parser.parse_args()
    fixtures.TOOL, fixtures.FIXTURE = args.tool.resolve(), args.fixture.resolve()
    names = ([
        "test_finished_full_output_byte_change_refuses_gate_and_keeps_both_digests",
        "test_recorded_timed_warmup_bytes_refuse_even_when_all_four_replays_agree",
        "test_deleted_timed_output_without_recorded_digest_refuses",
    ] if args.negative else [
        "test_same_seed_four_real_outputs_agree_and_stay_outside_timed_rows",
        "test_comparison_is_opt_in_and_explicit_full_is_forwarded",
        "test_timed_encode_receipt_must_bind_the_resolved_mode",
        "test_serial_unset_still_uses_full_without_tee",
    ])
    suite = unittest.TestSuite(LateModeGateTests(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "_seed-report":
        raise SystemExit(seed_report(sys.argv[2:]))
    raise SystemExit(main())
