"""A late structure refusal must reach the real tee publication owner."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import unittest

import test_sink_transcode_v2 as fixtures
from test_sink_v2_tee import TeeTests

FAULT = None
CONTROL = None


class StructureFaultTests(TeeTests):
    def launch_with_codec(self, tool):
        previous = fixtures.TOOL
        try:
            fixtures.TOOL = tool
            return self.launch(extra=("--tee-late-verify", "structure"))
        finally:
            fixtures.TOOL = previous

    def test_late_structure_failure_cancels_before_ready_and_never_names_output(self):
        # Same real fixture, owner/relay/A/C, parser, receipt code and pinned
        # codec. The control build removes only the post-validation bit flip.
        self.output = self.published / "control.pfs2"
        control = self.launch_with_codec(CONTROL)
        raw, good = self.finish(control)
        self.assertIs(json.loads(raw)["complete"], True)
        self.assertEqual(good["stage_c_child"]["exit_code"], 0)
        self.assertIs(good["stage_c_receipt"]["ready"], True)
        self.assertEqual(good["stage_c_receipt"]["late_verify"], "structure")
        events = [row["event"] for row in good["transcoder_phases"]]
        self.assertIn("test_late_storage_control", events)
        self.assertIn("structure_verify_done", events)
        with fixtures.pf_sink2.SinkReader(self.output) as reader:
            reader.verify()
        self.output.rename(self.directory / "checked-control.pfs2")

        # TeeTests.launch starts its shared 10 ms PublicationWatch BEFORE
        # Popen. finish stops it AFTER owner reap and checks staging cleanup.
        self.output = self.published / "refused.pfs2"
        fault = self.launch_with_codec(FAULT)
        raw, bad = self.finish(fault, success=False)
        self.assertEqual(fault["child"].returncode, 4)
        self.assertEqual(bad["stage_c_child"]["exit_code"], 4)
        self.assertEqual(bad["primary_failure"]["child"], "stage_c")
        self.assertEqual(bad["primary_failure"]["status"], 4)
        self.assertEqual(bad["cancel"]["child"], "stage_c")
        self.assertEqual(bad["cancel"]["status"], 4)
        self.assertEqual(bad["stage_a_child"]["exit_code"], 0)
        self.assertEqual(bad["runner_child"]["exit_code"], 0)
        self.assertNotIn("commit_monotonic_ns", bad)
        self.assertFalse(Path(bad["run_directory"]).exists())
        self.assertFalse(list(self.published.iterdir()), "owner left staging or another public name")
        self.assertEqual(fault["watcher"].names, set())
        failure = bad["stage_c_receipt"]
        self.assertIs(failure["complete"], False)
        self.assertNotIn("ready", failure)
        self.assertEqual(failure["code"], "report_sink_failed")
        self.assertEqual(failure["reason"], "v2_input_mismatch")
        self.assertEqual(failure["status"], 4)
        self.assertEqual(failure["late_verify"], "structure")
        # the reader prefixes every refusal with "sink v2: "; the rest must be exactly the expected text
        self.assertEqual(failure["diagnostic"], "sink v2: incremental stored data changed after validation")
        reported = json.loads(raw)
        for key in ("complete", "code", "reason", "diagnostic", "status", "late_verify"):
            self.assertEqual(reported[key], failure[key])
        self.assertEqual(reported["child"], "stage_c")
        fault["stderr"].seek(0)
        self.assertIn(b"late fault injected in H after incremental validation and before tail",
                      fault["stderr"].read())
        phases = bad["transcoder_phases"]
        events = [row["event"] for row in phases]
        for earlier, later in zip(
                ("incremental_validation_done", "summary_parsed", "test_late_storage_mutated", "tail_written"),
                ("summary_parsed", "test_late_storage_mutated", "tail_written", "exit")):
            self.assertLess(events.index(earlier), events.index(later))
        self.assertEqual(phases[-1]["event"], "exit")
        self.assertEqual(phases[-1]["status"], 4)
        for event in ("structure_verify_done", "full_verify_done", "to_v1_done", "fsync_close_done"):
            self.assertNotIn(event, events, "fault escaped the retained late structure check")


def main():
    global FAULT, CONTROL
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--fault", type=Path, required=True)
    parser.add_argument("--control", type=Path, required=True)
    args = parser.parse_args()
    fixtures.TOOL, fixtures.FIXTURE = args.tool.resolve(), args.fixture.resolve()
    FAULT, CONTROL = args.fault.resolve(), args.control.resolve()
    suite = unittest.TestSuite([StructureFaultTests(
        "test_late_structure_failure_cancels_before_ready_and_never_names_output")])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() and not result.skipped else 1


if __name__ == "__main__":
    raise SystemExit(main())
