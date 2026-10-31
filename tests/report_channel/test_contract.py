"""Outside-in report-channel contract rows. Run with unittest discovery."""
from __future__ import annotations

import errno
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from host import (
    ROOT, SUPERVISOR, HostWatchdogExpired, clean_environment, encoded,
    fixture_subreaper, group_members, launch, process_info, reference,
    require_supervisor, sink_receipt, sink_wire, supervisor_fixture, validator_observer,
)


KNOBS = (
    ("--report-max-bytes", "PINEFORGE_REPORT_MAX_BYTES", "report_bytes", 4096),
    ("--diagnostic-max-bytes", "PINEFORGE_DIAGNOSTIC_MAX_BYTES", "diagnostic_bytes", 64),
    ("--channel-wall-ms", "PINEFORGE_CHANNEL_WALL_MS", "wall_ms", 10000),
    ("--channel-stall-ms", "PINEFORGE_CHANNEL_STALL_MS", "stall_ms", 2000),
)
CLAIM_BYTES = 8192
INACTIVE_CONTROLS = (
    *((flag, "0") for flag, _, _, _ in KNOBS),
    ("--terminal-status-fd", "9"), ("--attempt-id", "0"),
    ("--host-attestation-fd", "9"), ("--test-only-host-attestation-fd", "9"),
    ("--post-exit-drain-ms", "0"), ("--stop-grace-ms", "0"),
    ("--planned-sink-bytes", "0"),
)
INACTIVE_ENVIRONMENT = (
    *(environment for _, environment, _, _ in KNOBS),
    "PINEFORGE_TERMINAL_STATUS_FD", "PINEFORGE_REPORT_ATTEMPT_ID",
    "PINEFORGE_REPORT_HOST_ATTESTATION_FD",
    "PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS", "PINEFORGE_CHANNEL_STOP_GRACE_MS",
    "PINEFORGE_REPORT_PLANNED_SINK_BYTES",
)


class Contract(unittest.TestCase):
    def setUp(self):
        require_supervisor()

    def assert_record(self, capture, *, complete, exit_class, exit_status,
                      invocation="run_json", msg=None):
        if msg is None:
            msg = capture.failure_context()
        record = capture.terminal()
        self.assertEqual(set(record), {
            "schema", "version", "contract", "attempt_id", "invocation", "complete",
            "exit_class", "failure", "exit_status", "producer", "limits",
            "report_charged_bytes", "channels", "sink_verification", "usage", "cleanup",
            "diagnostics"},
            msg=msg)
        self.assertEqual(record["schema"], "pf-run-terminal", msg=msg)
        self.assertEqual(record["version"], 1, msg=msg)
        self.assertEqual(record["contract"], "supervised/v1", msg=msg)
        self.assertEqual(record["attempt_id"], capture.attempt_id, msg=msg)
        self.assertEqual(record["invocation"], invocation, msg=msg)
        self.assertIs(record["complete"], complete, msg=msg)
        self.assertEqual(record["exit_class"], exit_class, msg=msg)
        self.assertEqual(record["exit_status"], exit_status, msg=msg)
        self.assertIsInstance(record["diagnostics"], list, msg=msg)
        self.assertTrue(all(isinstance(text, str) for text in record["diagnostics"]), msg=msg)
        self.assertEqual(capture.returncode, exit_status, msg=msg)
        self.assertTrue({"report", "diagnostic", "status"} <= capture.eof, msg=msg)
        self.assertEqual(set(record["producer"]), {
            "stage", "exit_status", "signal", "failure", "failure_claim_omitted"}, msg=msg)
        self.assertEqual(set(record["channels"]), {
            "candidate", "report", "sink", "diagnostic", "status"}, msg=msg)
        for name, actual in (("report", capture.report), ("diagnostic", capture.diagnostic),
                             ("status", capture.packets[0])):
            cell = record["channels"][name]
            self.assertEqual(cell["written_bytes"], len(actual), msg=f"{name}; {msg}")
            self.assertEqual(cell["sha256"],
                             None if name == "status" else hashlib.sha256(actual).hexdigest(),
                             msg=msg)
        packet_cell = record["channels"]["status"]
        for key in ("observed_bytes", "accepted_bytes", "written_bytes"):
            self.assertEqual(packet_cell[key], len(capture.packets[0]), msg=msg)
        self.assertLessEqual(len(capture.packets[0]), 65536, msg=msg)
        self.assertTrue(capture.packets[0].endswith(b"\n"), msg=msg)
        self.assertEqual(record["usage"]["version"], 1, msg=msg)
        stages = record["usage"]["stages"]
        self.assertEqual(set(stages), {"prepare", "compile", "run", "finalize", "publish"},
                         msg=msg)
        resources = {
            "wall", "cpu_user", "cpu_system", "memory_rss", "memory_private", "memory_job",
            "scratch_logical", "scratch_allocated", "work_logical", "work_allocated",
            "scratch_files", "work_files", "open_fds", "processes", "arena_claimed",
            "arena_capacity", "report_bytes", "sink_bytes", "diagnostic_bytes"}
        for name in ("prepare", "compile"):
            self.assertEqual(stages[name]["state"], "not_started", msg=msg)
            self.assertEqual(set(stages[name]["resources"]), resources, msg=msg)
            for cell in stages[name]["resources"].values():
                self.assertEqual(set(cell), {
                    "used", "requested", "limit", "peak", "unit", "scope", "source", "state"},
                    msg=msg)
                self.assertEqual(cell["state"], "not_started", msg=msg)
                self.assertIsNone(cell["used"], msg=msg)
        for entry in record["cleanup"].values():
            self.assertEqual(set(entry), {"value", "measurement_state"}, msg=msg)
            if entry["measurement_state"] == "unavailable":
                self.assertIsNone(entry["value"], msg=msg)
        domain = record["cleanup"]["domain_empty"]
        if domain != {"value": True, "measurement_state": "measured"}:
            self.assertFalse(record["complete"], msg=msg)
            self.assertNotEqual(record["exit_class"], "success", msg=msg)
        return record

    def assert_fixture_delivery(self, capture, *, msg=None):
        """Successful byte delivery is observable without certifying isolation."""
        if msg is None:
            msg = capture.failure_context()
        record = self.assert_record(
            capture, complete=False, exit_class="resource_refusal", exit_status=1, msg=msg)
        self.assertIsNone(record["failure"], msg=msg)
        self.assertEqual(record["diagnostics"], [], msg=msg)
        self.assertEqual(record["cleanup"]["domain_empty"], {
            "value": None, "measurement_state": "unavailable"}, msg=msg)
        self.assertEqual(record["producer"]["exit_status"], 0, msg=msg)
        self.assertIsNone(record["producer"]["signal"], msg=msg)
        for name in ("candidate", "report", "diagnostic", "sink"):
            self.assertTrue(record["channels"][name]["complete"], msg=f"{name}; {msg}")
        return record

    def refusal(self, capture, code, args, *, exit_class="input_refusal", status=2,
                invocation="run_json", msg=None):
        if msg is None:
            msg = capture.failure_context()
        record = self.assert_record(
            capture, complete=False, exit_class=exit_class, exit_status=status,
            invocation=invocation, msg=msg)
        self.assertEqual(record["failure"], {
            "code": code, "args": args, "authority": "supervisor"}, msg=msg)
        return record

    def launch_run_quota_case(self, controls, *, observer, invocation="run_json",
                              attested=True, options=(), supervisor_path=None, direct=False):
        environment = clean_environment()
        self.assertEqual(
            {name: value for name, value in environment.items()
             if name.startswith("PF_RUN_QUOTA_")}, {},
            msg="quota cases require an unmanaged TEST baseline before launch")
        environment.update(controls)
        self.assertEqual(
            {name: value for name, value in environment.items()
             if name.startswith("PF_RUN_QUOTA_")},
            {name: value for name, value in controls.items()
             if name.startswith("PF_RUN_QUOTA_")},
            msg="quota case must install exactly its requested markers before launch")
        self.assertTrue(Path(observer).is_file())
        with tempfile.TemporaryDirectory(prefix="pf-channel-quota-") as directory:
            marker = Path(directory) / "producer.json"
            self.assertFalse(marker.exists(), msg="producer marker must be absent before launch")
            if direct:
                self.assertEqual(invocation, "run_json")
            capture = launch(
                {"closes": [7, 13, 9], "inherited_sink": True,
                 "producer_identity_path": str(marker), "stdout": encoded(b"PRODUCER_STARTED")},
                environment=environment, sink=True, observer=observer, attested=attested,
                options=(*(() if direct else ("--invocation", invocation)), *options),
                supervisor_path=supervisor_path, direct=direct,
                command=["--an-unrecognized-runner-option"] if direct else None)
            identity = json.loads(marker.read_text()) if marker.exists() else None
            return capture, identity

    def assert_run_quota_prelaunch(self, capture, identity):
        msg = capture.failure_context()
        record = capture.terminal()
        self.assertIsNone(
            record["producer"]["stage"], msg=f"pre-launch refusal started a child; {msg}")
        self.assertIsNone(identity, msg=msg)
        self.assertEqual(record["producer"], {
            "stage": None, "exit_status": None, "signal": None,
            "failure": None, "failure_claim_omitted": False}, msg=msg)
        self.assertEqual(capture.validator_events, [], msg=msg)
        self.assertIsNone(record["sink_verification"], msg=msg)
        self.assertEqual(capture.diagnostic, b"", msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        self.assertEqual(record["channels"]["candidate"]["observed_bytes"], 0, msg=msg)
        for stage in ("run", "finalize"):
            self.assertEqual(record["usage"]["stages"][stage]["state"], "not_started", msg=msg)

    def assert_run_quota_refusal(self, capture, identity, variables, *, invocation="run_json"):
        self.assert_run_quota_prelaunch(capture, identity)
        msg = capture.failure_context()
        record = self.refusal(
            capture, "run_isolation_unavailable", {"reason": "policy"},
            exit_class="resource_refusal", status=4 if invocation == "entrypoint" else 1,
            invocation=invocation, msg=msg)
        self.assertEqual(record["diagnostics"], [
            ("the supervised report channel does not forward the host run-quota marker: "
             + name + " is set (unsupported combination)").encode("utf-8")[:1024].decode(
                 "utf-8", "ignore")
            for name in sorted(variables)], msg=msg)
        self.assertTrue(all(len(text.encode("utf-8")) <= 1024
                            for text in record["diagnostics"]), msg=msg)
        report = json.loads(capture.report)
        self.assertEqual(report["code"], "run_isolation_unavailable", msg=msg)
        self.assertEqual(report["args"], {"reason": "policy"}, msg=msg)

    def assert_producer_group_gone(self, capture, identity_path):
        msg = capture.failure_context()
        identity = json.loads(identity_path.read_text())
        self.assertEqual(identity["group"], identity["pid"], msg=msg)
        self.assertIsNone(process_info(identity["pid"]), msg=msg)
        self.assertEqual(group_members(identity["group"]), [], msg=msg)

    def assert_cancel_bound(self, capture, phase):
        msg = capture.failure_context()
        control = capture.control
        self.assertIn(phase, control["stalled_at"], msg=msg)
        self.assertIsNotNone(control["cancelled_at"], msg=msg)
        self.assertIsNotNone(control["exited_at"], msg=msg)
        self.assertGreaterEqual(
            control["cancelled_at"] - control["stalled_at"][phase], 0.15, msg=msg)
        self.assertGreaterEqual(control["exited_at"], control["cancelled_at"], msg=msg)
        self.assertLess(control["exited_at"] - control["cancelled_at"], 0.75, msg=msg)
        self.assertIn("pre_cancel", control, msg=msg)
        before = control["pre_cancel"]
        self.assertEqual(before["phase"], phase, msg=msg)
        self.assertLessEqual(control["stalled_at"][phase], before["at"], msg=msg)
        self.assertLessEqual(before["at"], control["cancelled_at"], msg=msg)
        if phase in ("report", "sink"):
            self.assertEqual(before["pending_bytes"], 4096, msg=msg)

    def assert_status_stall(self, capture, *, released):
        msg = capture.failure_context()
        control = capture.control
        observation = control["status_pending_send"]
        self.assertEqual(observation["pid"], control["supervisor_pid"], msg=msg)
        self.assertIn(observation["send_errno"], (errno.EAGAIN, errno.EWOULDBLOCK), msg=msg)
        self.assertGreater(observation["packet_bytes"], 0, msg=msg)
        self.assertLessEqual(observation["packet_bytes"], 65536, msg=msg)
        self.assertEqual(len([event for event in capture.validator_events
                              if event[:1] == b"M"]), 1, msg=msg)
        self.assertGreater(control["status_prefill_packets"], 0, msg=msg)
        self.assertEqual(control["status_prefill_packets"],
                         control["status_prefill"]["packets"], msg=msg)
        self.assertIn(control["status_prefill"]["eagain_errno"],
                      (errno.EAGAIN, errno.EWOULDBLOCK), msg=msg)
        self.assertIn(control["status_probe_errno"], (errno.EAGAIN, errno.EWOULDBLOCK), msg=msg)
        before = control["pre_cancel"]
        self.assertIn(before["status_probe_errno"], (errno.EAGAIN, errno.EWOULDBLOCK), msg=msg)
        self.assertEqual(before["status_gate"]["process"]["pid"],
                         control["supervisor_pid"], msg=msg)
        self.assertEqual(before["status_gate"]["process"]["state"], "S", msg=msg)
        self.assertLessEqual(control["status_gate_observed"]["at"],
                             control["stalled_at"]["status_ready" if released else "status"],
                             msg=msg)
        if released:
            self.assertIs(before["status_writable"], True, msg=msg)
            self.assertEqual(control["status_release_packets"],
                             control["status_prefill_packets"], msg=msg)
            self.assertLessEqual(control["status_released_at"], before["at"], msg=msg)
            self.assertEqual(control["held_until_exit"], [], msg=msg)
        else:
            self.assertNotIn("status_released_at", control, msg=msg)

    def assert_cancel_record(self, capture, invocation):
        record = self.refusal(
            capture, "run_aborted", {}, exit_class="aborted",
            status=4 if invocation == "entrypoint" else 1, invocation=invocation)
        msg = capture.failure_context()
        for name in ("wall_ms", "stall_ms"):
            self.assertEqual(record["limits"][name], {
                "requested": None, "granted": None, "effective": None}, msg=msg)
        self.assertEqual(record["cleanup"]["producer_reaped"], {
            "value": True, "measurement_state": "measured"}, msg=msg)
        return record

    def test_cancel_stalled_report_publication_with_unlimited_times(self):
        expected = reference({"label": "stalled-report-" + "x" * 16384}).stdout
        self.assertGreater(len(expected), 4096)
        for signum in (signal.SIGTERM, signal.SIGINT):
            for invocation in ("run_json", "entrypoint"):
                with self.subTest(signum=signum, invocation=invocation), tempfile.TemporaryDirectory(
                        prefix="pf-cancel-report-") as directory:
                    identity = Path(directory) / "producer.json"
                    capture = launch(
                        {"candidate": encoded(expected), "producer_identity_path": str(identity)},
                        options=("--invocation", invocation), stalled_channels=("report",),
                        cancel_channel="report", cancel_signal=signum, watchdog_seconds=5)
                    msg = capture.failure_context()
                    self.assert_cancel_bound(capture, "report")
                    record = self.assert_cancel_record(capture, invocation)
                    self.assert_producer_group_gone(capture, identity)
                    self.assertEqual(capture.control["held_until_exit"], ["report"], msg=msg)
                    self.assertEqual(capture.report, expected[:4096], msg=msg)
                    self.assertTrue(record["channels"]["candidate"]["complete"], msg=msg)
                    cell = record["channels"]["report"]
                    self.assertEqual(cell["accepted_bytes"], len(expected), msg=msg)
                    self.assertEqual(cell["written_bytes"], 4096, msg=msg)
                    self.assertTrue(cell["truncated"], msg=msg)
                    self.assertFalse(cell["complete"], msg=msg)

    def test_cancel_stalled_external_sink_publication_with_unlimited_times(self):
        closes = list(range(600))
        native = sink_wire(closes)
        self.assertGreater(len(native), 4096)
        with validator_observer() as observer:
            for signum in (signal.SIGTERM, signal.SIGINT):
                for invocation in ("run_json", "entrypoint"):
                    with self.subTest(signum=signum, invocation=invocation), tempfile.TemporaryDirectory(
                            prefix="pf-cancel-sink-") as directory:
                        identity = Path(directory) / "producer.json"
                        capture = launch(
                            {"closes": closes, "inherited_sink": True,
                             "producer_identity_path": str(identity)},
                            sink=True, observer=observer, options=("--invocation", invocation),
                            stalled_channels=("sink",), cancel_channel="sink",
                            cancel_signal=signum, watchdog_seconds=5)
                        msg = capture.failure_context()
                        self.assert_cancel_bound(capture, "sink")
                        record = self.assert_cancel_record(capture, invocation)
                        self.assert_producer_group_gone(capture, identity)
                        self.assertEqual(capture.control["held_until_exit"], ["sink"], msg=msg)
                        self.assertEqual(capture.sink, native[:4096], msg=msg)
                        self.assertEqual(capture.report, b"", msg=msg)
                        self.assertTrue(record["sink_verification"]["complete"], msg=msg)
                        self.assertEqual(record["sink_verification"]["exit_status"], 0, msg=msg)
                        self.assertTrue(record["channels"]["candidate"]["complete"], msg=msg)
                        self.assertEqual(record["channels"]["sink"]["accepted_bytes"],
                                         len(native), msg=msg)
                        self.assertEqual(record["channels"]["sink"]["written_bytes"], 4096, msg=msg)
                        self.assertTrue(record["channels"]["sink"]["truncated"], msg=msg)
                        self.assertFalse(record["channels"]["sink"]["complete"], msg=msg)

    def test_cancel_terminal_delivery_to_unread_full_status_socket(self):
        expected = reference({}).stdout
        for signum in (signal.SIGTERM, signal.SIGINT):
            for invocation in ("run_json", "entrypoint"):
                with self.subTest(signum=signum, invocation=invocation), tempfile.TemporaryDirectory(
                        prefix="pf-cancel-full-status-") as directory:
                    identity = Path(directory) / "producer.json"
                    capture = launch(
                        {"producer_identity_path": str(identity)},
                        options=("--invocation", invocation), saturate_status=True,
                        cancel_channel="status", cancel_signal=signum, watchdog_seconds=5)
                    msg = capture.failure_context()
                    self.assert_cancel_bound(capture, "status")
                    self.assert_status_stall(capture, released=False)
                    self.assert_producer_group_gone(capture, identity)
                    self.assertEqual(capture.returncode, 4 if invocation == "entrypoint" else 1,
                                     msg=msg)
                    self.assertGreater(capture.control["status_prefill_packets"], 0, msg=msg)
                    self.assertEqual(capture.control["held_until_exit"], ["status"], msg=msg)
                    self.assertEqual(capture.packets, [], msg=msg)
                    self.assertEqual(capture.packet_flags, [], msg=msg)
                    self.assertEqual(capture.report, expected, msg=msg)
                    self.assertEqual(capture.diagnostic, b"", msg=msg)
                    self.assertTrue({"report", "diagnostic", "status"} <= capture.eof, msg=msg)

    def test_cancel_pending_terminal_send_rebuilds_available_packet(self):
        expected = reference({}).stdout
        with validator_observer() as observer:
            for signum in (signal.SIGTERM, signal.SIGINT):
                for invocation in ("run_json", "entrypoint"):
                    with self.subTest(signum=signum, invocation=invocation), tempfile.TemporaryDirectory(
                            prefix="pf-cancel-ready-status-") as directory:
                        identity = Path(directory) / "producer.json"
                        capture = launch(
                            {"producer_identity_path": str(identity)},
                            options=("--invocation", invocation), observer=observer,
                            gate_status=True, cancel_channel="status_ready",
                            cancel_signal=signum, watchdog_seconds=5)
                        msg = capture.failure_context()
                        self.assert_cancel_bound(capture, "status_ready")
                        self.assert_status_stall(capture, released=True)
                        record = self.assert_cancel_record(capture, invocation)
                        self.assert_producer_group_gone(capture, identity)
                        self.assertEqual(capture.report, expected, msg=msg)
                        self.assertTrue(record["channels"]["report"]["complete"], msg=msg)

    def assert_finite_report_stall(self, *, supervisor_path=None):
        expected = reference({"label": "finite-report-" + "x" * 16384}).stdout
        with tempfile.TemporaryDirectory(prefix="pf-finite-report-") as directory:
            identity = Path(directory) / "producer.json"
            capture = launch(
                {"candidate": encoded(expected), "producer_identity_path": str(identity)},
                options=("--channel-stall-ms", "600", "--channel-wall-ms", "60000"),
                stalled_channels=("report",), supervisor_path=supervisor_path, watchdog_seconds=4)
            msg = capture.failure_context()
            record = self.refusal(
                capture, "report_channel_failed", {"channel": "report", "reason": "deadline"},
                exit_class="transport_failure", status=1, msg=msg)
            self.assert_producer_group_gone(capture, identity)
            self.assertIn("report", capture.control["stalled_at"], msg=msg)
            duration = capture.control["exited_at"] - capture.control["stalled_at"]["report"]
            self.assertGreaterEqual(duration, 0.5, msg=msg)
            self.assertLess(duration, 0.8, msg=msg)
            self.assertEqual(capture.control["held_until_exit"], ["report"], msg=msg)
            self.assertEqual(record["limits"]["stall_ms"]["effective"], 600, msg=msg)
            self.assertEqual(record["limits"]["wall_ms"]["effective"], 60000, msg=msg)
            self.assertIsNone(capture.control["cancelled_at"], msg=msg)
            self.assertEqual(capture.report, expected[:4096], msg=msg)
            self.assertEqual(record["channels"]["report"]["accepted_bytes"], len(expected), msg=msg)
            self.assertTrue(record["channels"]["report"]["truncated"], msg=msg)
            self.assertFalse(record["channels"]["report"]["complete"], msg=msg)

    def test_finite_stall_expires_through_supervisor_report_pipe(self):
        self.assert_finite_report_stall()

    def test_finite_report_stall_assertions_reject_removed_pipe_deadline(self):
        self.assert_finite_report_stall()
        original = SUPERVISOR.read_text(encoding="utf-8")
        line = '                if pending.stall_remaining_ms() <= 0:\n'
        self.assertEqual(original.count(line), 1, "expected one publication stall check")
        with tempfile.TemporaryDirectory(prefix="pf-report-deadline-mutant-") as directory:
            altered = Path(directory) / SUPERVISOR.name
            for sibling in SUPERVISOR.parent.iterdir():
                if sibling != SUPERVISOR and sibling.suffix in (".py", ".json", ".def"):
                    (altered.parent / sibling.name).symlink_to(sibling)
            altered.write_text(
                original.replace(line, '                if False:\n', 1), encoding="utf-8")
            with self.assertRaises(HostWatchdogExpired) as rejected:
                self.assert_finite_report_stall(supervisor_path=altered)
            partial = rejected.exception.capture
            msg = partial.failure_context()
            self.assertEqual(partial.control["watchdog_pending_bytes"], {"report": 4096}, msg=msg)
            self.assertEqual(partial.control["watchdog_producer_group"], [], msg=msg)
            self.assertIn("report", partial.control["stalled_at"], msg=msg)
            self.assertGreater(
                partial.elapsed_seconds - partial.control["stalled_at"]["report"], 2, msg=msg)
            self.assertIsNone(partial.control["cancelled_at"], msg=msg)
            self.assertIsNone(partial.returncode, msg=msg)
            self.assertEqual(partial.packets, [], msg=msg)
            self.assertEqual(partial.diagnostic, b"", msg=msg)

    def test_finite_wall_expires_through_supervisor_producer_wait(self):
        with tempfile.TemporaryDirectory(prefix="pf-finite-producer-wall-") as directory:
            identity = Path(directory) / "producer.json"
            capture = launch(
                {"producer_identity_path": str(identity), "stall_before_candidate": True},
                options=("--channel-wall-ms", "1800"), watchdog_seconds=5)
            msg = capture.failure_context()
            record = self.refusal(
                capture, "run_deadline_exceeded", {"stage": "run"},
                exit_class="resource_refusal", status=1, msg=msg)
            self.assert_producer_group_gone(capture, identity)
            self.assertGreaterEqual(capture.control["exited_at"], 1.7, msg=msg)
            self.assertLess(capture.control["exited_at"], 3.15, msg=msg)
            self.assertEqual(record["limits"]["wall_ms"]["effective"], 1800, msg=msg)
            self.assertIsNone(record["limits"]["stall_ms"]["effective"], msg=msg)
            self.assertIsNone(capture.control["cancelled_at"], msg=msg)
            self.assertEqual(record["producer"]["signal"], signal.SIGTERM, msg=msg)
            self.assertEqual(record["channels"]["candidate"]["observed_bytes"], 0, msg=msg)
            self.assertEqual(record["cleanup"]["producer_reaped"], {
                "value": True, "measurement_state": "measured"}, msg=msg)

    def test_finite_stall_expires_through_supervisor_diagnostic_pipe(self):
        diagnostic = b"finite diagnostic stall\n" * 8192
        with tempfile.TemporaryDirectory(prefix="pf-finite-diagnostic-") as directory:
            identity = Path(directory) / "producer.json"
            capture = launch(
                {"stdout": encoded(diagnostic), "producer_identity_path": str(identity)},
                options=("--channel-stall-ms", "600", "--channel-wall-ms", "60000"),
                stalled_channels=("diagnostic",), watchdog_seconds=5)
            msg = capture.failure_context()
            record = self.refusal(
                capture, "report_channel_failed", {"channel": "diagnostic", "reason": "deadline"},
                exit_class="transport_failure", status=1, msg=msg)
            self.assert_producer_group_gone(capture, identity)
            self.assertIn("diagnostic", capture.control["stalled_at"], msg=msg)
            duration = capture.control["exited_at"] - capture.control["stalled_at"]["diagnostic"]
            self.assertGreaterEqual(duration, 0.5, msg=msg)
            self.assertLess(duration, 1.8, msg=msg)
            self.assertEqual(capture.control["held_until_exit"], ["diagnostic"], msg=msg)
            self.assertIsNone(capture.control["cancelled_at"], msg=msg)
            self.assertEqual(record["limits"]["stall_ms"]["effective"], 600, msg=msg)
            self.assertEqual(capture.diagnostic, diagnostic[:4096], msg=msg)
            self.assertFalse(record["channels"]["diagnostic"]["complete"], msg=msg)
            self.assertTrue(record["channels"]["diagnostic"]["truncated"], msg=msg)
            self.assertEqual(record["producer"]["signal"], signal.SIGTERM, msg=msg)

    def test_paused_validator_is_ended_by_supervisor_wall(self):
        closes = [11, 15, 13, 17]
        native = sink_wire(closes)
        with tempfile.TemporaryDirectory(prefix="pf-paused-validator-wall-") as directory:
            identity = Path(directory) / "producer.json"
            release = Path(directory) / "native-ready"
            capture = launch(
                {"closes": closes, "inherited_sink": True,
                 "producer_identity_path": str(identity), "native_release_path": str(release)},
                sink=True, pause_validator_until_wall=True,
                options=("--channel-wall-ms", "1800"), watchdog_seconds=5)
            msg = capture.failure_context()
            self.assertIn("native_release_precondition", capture.control, msg=msg)
            before = capture.control["native_release_precondition"]
            self.assertEqual(before["validator"]["state"], "T", msg=msg)
            self.assertLessEqual(before["at"], capture.control["native_released_at"], msg=msg)
            self.assertLess(capture.control["native_released_at"],
                            capture.control["exited_at"], msg=msg)
            record = self.refusal(
                capture, "run_deadline_exceeded", {"stage": "finalize"},
                exit_class="resource_refusal", status=1, msg=msg)
            self.assert_producer_group_gone(capture, identity)
            validators = {int(event[1:]) for event in capture.validator_events if event[:1] == b"S"}
            self.assertEqual(len(validators), 1, msg=msg)
            self.assertEqual(capture.stopped_validators, validators, msg=msg)
            self.assertEqual(set(capture.control["host_paused_validators"]), validators, msg=msg)
            self.assertTrue(release.is_file(), msg=msg)
            self.assertEqual(record["limits"]["wall_ms"]["effective"], 1800, msg=msg)
            self.assertIsNone(record["limits"]["stall_ms"]["effective"], msg=msg)
            self.assertIsNone(capture.control["cancelled_at"], msg=msg)
            self.assertGreaterEqual(capture.control["exited_at"], 1.7, msg=msg)
            self.assertLess(capture.control["exited_at"], 3.15, msg=msg)
            self.assertEqual(record["sink_verification"], {
                "exit_status": None, "signal": signal.SIGKILL, "complete": False,
                "counts": dict.fromkeys("ETOVKXH"), "sha256": None, "finalizer_version": None},
                msg=msg)
            for pid in validators:
                self.assertIsNone(process_info(pid), msg=msg)
                self.assertEqual(group_members(pid), [], msg=msg)
            self.assertEqual(capture.finalizer_receipt(), b"", msg=msg)
            self.assertEqual(record["channels"]["sink"]["accepted_bytes"], len(native), msg=msg)
            self.assertEqual(record["channels"]["sink"]["accepted_sha256"],
                             hashlib.sha256(native).hexdigest(), msg=msg)
            self.assertTrue(record["channels"]["sink"]["source_total_known"], msg=msg)
            self.assertEqual(capture.sink, b"", msg=msg)
            self.assertEqual(capture.report, b"", msg=msg)

    def test_t1_sinkless_success_preserves_reference_bytes(self):
        for closes in ([10, 12, 11], [21, 19, 23, 20]):
            for fragment in (1, 65536):
                with self.subTest(closes=closes, fragment=fragment):
                    request = {"closes": closes, "fragment_bytes": fragment}
                    baseline = reference(request)
                    capture = launch(request)
                    msg = capture.failure_context()
                    self.assertEqual(baseline.returncode, 0, msg=msg)
                    self.assertEqual(capture.report, baseline.stdout, msg=msg)
                    self.assertEqual(json.loads(capture.report)["summary"]["net_pnl"],
                                     closes[-1] - closes[0], msg=msg)
                    record = self.assert_fixture_delivery(capture, msg=msg)
                    self.assertIsNone(record["failure"], msg=msg)
                    self.assertEqual(record["producer"]["exit_status"], 0, msg=msg)
                    self.assertIsNone(record["producer"]["signal"], msg=msg)
                    self.assertEqual(record["report_charged_bytes"], len(baseline.stdout), msg=msg)
                    self.assertEqual(record["channels"]["candidate"]["observed_bytes"],
                                     len(baseline.stdout), msg=msg)
                    self.assertTrue(record["channels"]["candidate"]["source_total_known"], msg=msg)

    def test_t1_inherited_sink_success_keeps_summary_projection(self):
        request = {"closes": [10, 14, 12], "inherited_sink": True}
        baseline = reference(request)
        capture = launch(request, sink=True)
        msg = capture.failure_context()
        self.assertEqual(capture.returncode, 1, msg=(
            f"the unisolated fixture must preserve the inherited-sink projection; {msg}"))
        self.assertEqual(capture.report, baseline.stdout, msg=msg)
        self.assertEqual(capture.sink, baseline.sink, msg=msg)
        self.assertEqual(capture.sink, sink_wire(request["closes"]), msg=msg)
        self.assertNotIn("trades", json.loads(capture.report), msg=msg)
        record = self.assert_fixture_delivery(capture, msg=msg)
        self.assertTrue(record["sink_verification"]["complete"], msg=msg)
        self.assertGreater(len(capture.sink), 0, msg=msg)
        self.assertEqual(record["channels"]["sink"]["written_bytes"], len(capture.sink),
                         msg=msg)

    def test_t1_owned_runner_writer_preserves_success_bytes(self):
        for closes in ([9, 10, 15], [15, 12]):
            with self.subTest(closes=closes):
                request = {"closes": closes, "runner_writer": True}
                baseline = reference(request)
                capture = launch(request)
                msg = capture.failure_context()
                self.assertEqual(baseline.returncode, 0,
                                 msg=f"reference_stderr={baseline.stderr!r}; {msg}")
                self.assertEqual(capture.report, baseline.stdout, msg=msg)
                self.assertEqual(json.loads(capture.report)["summary"]["net_pnl"],
                                 closes[-1] - closes[0], msg=msg)
                self.assert_fixture_delivery(capture, msg=msg)

    def test_t1_owned_runner_failure_writer_preserves_failure_bytes(self):
        request = {"failure_writer": {
            "text": "controlled producer failure", "code": "harness_internal_error", "args": {}},
            "exit_status": 1}
        baseline = reference(request)
        capture = launch(request)
        msg = capture.failure_context()
        self.assertEqual(baseline.returncode, 1,
                         msg=f"reference_stderr={baseline.stderr!r}; {msg}")
        self.assertEqual(capture.report, baseline.stdout, msg=msg)
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1, msg=msg)
        self.assertEqual(record["producer"]["failure"]["authority"], "producer", msg=msg)

    def test_t2_defaults_and_not_started_usage(self):
        capture = launch()
        msg = capture.failure_context()
        record = self.assert_fixture_delivery(capture, msg=msg)
        limits = record["limits"]
        # The wire uses null effective limits for every unlimited dimension.
        for _, _, name, _ in KNOBS:
            self.assertEqual(limits[name], {
                "requested": None, "granted": None, "effective": None}, msg=msg)
        for name in ("report", "candidate", "diagnostic"):
            self.assertIsNone(record["channels"][name]["limit_bytes"], msg=msg)
        self.assertEqual(limits["terminal_bytes"], 65536, msg=msg)
        self.assertEqual(limits["stop_grace_ms"], {
            "requested": None, "granted": None, "effective": 1000}, msg=msg)
        self.assertEqual(limits["post_exit_drain_ms"], {
            "requested": None, "granted": None, "effective": 1000}, msg=msg)
        self.assertEqual(limits["relay_queue_bytes"], 65536, msg=msg)
        self.assertEqual(limits["producer_claim_bytes"], 8192, msg=msg)
        self.assertIsNone(limits["sink_bytes"], msg=msg)
        self.assertIsNone(record["sink_verification"], msg=msg)

    def test_t2_unknown_contract_is_prestart_usage_error(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")},
                         options=("--report-channel", "supervised/v999"))
        msg = capture.failure_context()
        record = self.refusal(capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
        self.assertIsNone(record["producer"]["stage"], msg=msg)
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic, msg=msg)

    def test_t2_environment_activation_and_cli_version_precedence(self):
        capture = launch(activate=False, environment={
            "PINEFORGE_REPORT_CHANNEL": "supervised/v1"})
        self.assert_fixture_delivery(capture)
        capture = launch(environment={"PINEFORGE_REPORT_CHANNEL": "supervised/unknown"})
        self.assert_fixture_delivery(capture)

    def test_t2_direct_runner_parser_failure_uses_candidate_boundary(self):
        argv = ["--an-unrecognized-runner-option"]
        baseline = subprocess.run(
            [sys.executable, "-B", str(ROOT / "docker/run_json.py"), *argv],
            capture_output=True, env=clean_environment(), check=False, timeout=10)
        capture = launch(direct=True, command=argv)
        msg = capture.failure_context()
        self.assertEqual(baseline.returncode, 2, msg=msg)
        self.assertEqual(capture.report, baseline.stdout, msg=msg)
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1, msg=msg)
        self.assertEqual(record["producer"]["exit_status"], 2, msg=msg)
        self.assertEqual(record["producer"]["failure"], {
            "code": "run_request_invalid", "args": {"option": "arguments"},
            "authority": "producer"}, msg=msg)

    def test_t2_missing_or_wrong_status_never_uses_another_channel(self):
        for endpoint in ("absent", "pipe", "stream", "reserved", "closed", "read_end"):
            with self.subTest(endpoint=endpoint):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, status=endpoint)
                msg = capture.failure_context()
                self.assertEqual(capture.returncode, 2, msg=msg)
                self.assertEqual(capture.packets, [], msg=msg)
                self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic, msg=msg)
                self.assertNotIn(b"pf-run-terminal", capture.report + capture.diagnostic, msg=msg)
                self.assertEqual(json.loads(capture.report)["code"], "run_request_invalid", msg=msg)

    def test_t2_isolation_requires_host_attestation(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, attested=False)
        msg = capture.failure_context()
        self.refusal(capture, "run_isolation_unavailable", {"reason": "policy"},
                     exit_class="resource_refusal", status=1, msg=msg)
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic, msg=msg)

    def test_run_quota_named_markers_refuse_before_launch_including_empty_values(self):
        with validator_observer() as observer:
            for name, value in (("PF_RUN_QUOTA_MODE", "managed"),
                                ("PF_RUN_QUOTA_NATIVE_FD", "9"),
                                ("PF_RUN_QUOTA_PYTHON_FD", "10")):
                for setting in (value, ""):
                    for invocation in ("run_json", "entrypoint"):
                        with self.subTest(name=name, value=setting, invocation=invocation):
                            variables = {name: setting}
                            capture, identity = self.launch_run_quota_case(
                                variables, observer=observer, invocation=invocation)
                            self.assert_run_quota_refusal(
                                capture, identity, variables, invocation=invocation)

    def test_run_quota_future_prefix_name_refuses_before_launch(self):
        with validator_observer() as observer:
            for value in ("future-marker", ""):
                for invocation in ("run_json", "entrypoint"):
                    with self.subTest(value=value, invocation=invocation):
                        variables = {"PF_RUN_QUOTA_FUTURE_X": value}
                        capture, identity = self.launch_run_quota_case(
                            variables, observer=observer, invocation=invocation)
                        self.assert_run_quota_refusal(
                            capture, identity, variables, invocation=invocation)

    def test_run_quota_multiple_markers_have_separate_sorted_diagnostics(self):
        variables = {"PF_RUN_QUOTA_PYTHON_FD": "python-marker-value",
                     "PF_RUN_QUOTA_MODE": "mode-marker-value"}
        self.assertNotEqual(list(variables), sorted(variables))
        with validator_observer() as observer:
            for invocation in ("run_json", "entrypoint"):
                with self.subTest(invocation=invocation):
                    capture, identity = self.launch_run_quota_case(
                        variables, observer=observer, invocation=invocation)
                    self.assert_run_quota_refusal(
                        capture, identity, variables, invocation=invocation)

    def test_run_quota_diagnostic_names_stay_within_the_utf8_bound(self):
        name = "PF_RUN_QUOTA_" + "é" * 600
        self.assertGreater(len(name.encode("utf-8")), 1024)
        variables = {name: "future-marker"}
        with validator_observer() as observer:
            for invocation in ("run_json", "entrypoint"):
                with self.subTest(invocation=invocation):
                    capture, identity = self.launch_run_quota_case(
                        variables, observer=observer, invocation=invocation)
                    self.assert_run_quota_refusal(
                        capture, identity, variables, invocation=invocation)

    def test_run_quota_policy_refusal_precedes_missing_host_attestation(self):
        variables = {"PF_RUN_QUOTA_MODE": ""}
        with validator_observer() as observer:
            for invocation in ("run_json", "entrypoint"):
                with self.subTest(invocation=invocation):
                    capture, identity = self.launch_run_quota_case(
                        variables, observer=observer, invocation=invocation, attested=False)
                    self.assert_run_quota_refusal(
                        capture, identity, variables, invocation=invocation)

    def test_run_quota_direct_runner_dispatch_refuses_before_producer(self):
        with validator_observer() as observer:
            for name in ("PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD",
                         "PF_RUN_QUOTA_PYTHON_FD", "PF_RUN_QUOTA_FUTURE_X"):
                with self.subTest(name=name):
                    variables = {name: ""}
                    capture, identity = self.launch_run_quota_case(
                        variables, observer=observer, direct=True)
                    self.assert_run_quota_refusal(capture, identity, variables)

    def test_run_quota_control_usage_errors_win_before_policy_refusal(self):
        variables = {"PF_RUN_QUOTA_MODE": "managed"}
        invalid_controls = (
            (("--report-channel", "supervised/v999"), {}),
            (("--not-a-channel-control", "1"), {}),
            (("--post-exit-drain-ms", "1", "--post-exit-drain-ms", "2"), {}),
            (("--stop-grace-ms",), {}),
            (("--planned-sink-bytes", "-1"), {}),
            ((), {"PINEFORGE_RUN_JSON_PROFILE": "invalid"}),
            *(((flag, "-1"), {}) for flag, _, _, _ in KNOBS),
            *(((), {name: ""}) for _, name, _, _ in KNOBS),
        )
        with validator_observer() as observer:
            capture, identity = self.launch_run_quota_case(variables, observer=observer)
            self.assert_run_quota_refusal(capture, identity, variables)
            for invocation in ("run_json", "entrypoint"):
                for options, environment in invalid_controls:
                    with self.subTest(invocation=invocation, options=options,
                                      environment=environment):
                        capture, identity = self.launch_run_quota_case(
                            {**variables, **environment}, observer=observer,
                            invocation=invocation, options=options)
                        self.assert_run_quota_prelaunch(capture, identity)
                        record = self.refusal(
                            capture, "run_request_invalid", {"option": "arguments"},
                            status=4 if invocation == "entrypoint" else 2,
                            invocation=invocation)
                        self.assertTrue(record["diagnostics"], msg=capture.failure_context())
                        self.assertFalse(any("host run-quota marker" in text
                                             for text in record["diagnostics"]),
                                         msg=capture.failure_context())

    def test_run_quota_lookalikes_and_unmanaged_environment_preserve_delivery(self):
        expected = reference({"closes": [7, 13, 9], "inherited_sink": True})
        self.assertEqual(expected.returncode, 0, msg=repr(expected.stderr))
        self.assertEqual(expected.sink, sink_wire([7, 13, 9]))
        with validator_observer() as observer:
            for environment in ({}, {"XPF_RUN_QUOTA_MODE": "managed"},
                                {"PF_RUN_QUOTA": "managed"},
                                {"XPF_RUN_QUOTA_MODE": "", "PF_RUN_QUOTA": ""}):
                with self.subTest(environment=environment):
                    self.assertFalse(any(name.startswith("PF_RUN_QUOTA_")
                                         for name in environment))
                    capture, identity = self.launch_run_quota_case(
                        environment, observer=observer)
                    msg = capture.failure_context()
                    self.assertIsNotNone(identity, msg=msg)
                    self.assert_fixture_delivery(capture, msg=msg)
                    self.assertEqual(capture.report, expected.stdout, msg=msg)
                    self.assertEqual(capture.sink, expected.sink, msg=msg)
                    self.assertEqual(capture.diagnostic, b"PRODUCER_STARTED", msg=msg)
                    self.assertTrue(capture.validator_events, msg=msg)
                    self.assertIsNone(process_info(identity["pid"]), msg=msg)
                    variables = {**environment, "PF_RUN_QUOTA_FUTURE_X": ""}
                    refused, identity = self.launch_run_quota_case(
                        variables, observer=observer)
                    self.assert_run_quota_refusal(
                        refused, identity, ("PF_RUN_QUOTA_FUTURE_X",))

    def test_run_quota_prefix_assertions_reject_a_narrowed_prefix(self):
        variables = {"PF_RUN_QUOTA_FUTURE_X": "future-marker"}
        with validator_observer() as observer:
            capture, identity = self.launch_run_quota_case(variables, observer=observer)
            self.assert_run_quota_refusal(capture, identity, variables)
            text = SUPERVISOR.read_text(encoding="utf-8")
            original = 'RUN_QUOTA_PREFIX = "PF_RUN_QUOTA_"\n'
            self.assertEqual(text.count(original), 1)
            with tempfile.TemporaryDirectory(prefix="pf-quota-prefix-mutation-") as directory:
                changed = supervisor_fixture(directory, text=text.replace(
                    original, 'RUN_QUOTA_PREFIX = "PF_RUN_QUOTA_MODE"\n'))
                capture, identity = self.launch_run_quota_case(
                    variables, observer=observer, supervisor_path=changed)
                msg = capture.failure_context()
                self.assertIsNotNone(identity, msg=msg)
                self.assert_fixture_delivery(capture, msg=msg)
                self.assertTrue(capture.validator_events, msg=msg)
                with self.assertRaisesRegex(AssertionError, "pre-launch refusal started a child"):
                    self.assert_run_quota_refusal(capture, identity, variables)

    def test_t2_zero_or_absent_host_grants_are_unlimited(self):
        diagnostic = (b"unlimited\x00\xff" * 100000) + b"tail"
        request = {"stdout": encoded(diagnostic)}
        expected = reference(request)
        zeros = {field: 0 for _, _, field, _ in KNOBS}
        zero_options = tuple(part for flag, _, _, _ in KNOBS for part in (flag, "0"))
        for grants in ({}, zeros):
            for options in ((), zero_options):
                with self.subTest(grants=grants, options=options):
                    capture = launch(request, grants=grants, options=options)
                    msg = capture.failure_context()
                    record = self.assert_fixture_delivery(capture, msg=msg)
                    # The reference stdout begins with raw diagnostics.
                    self.assertEqual(capture.report, expected.stdout[len(diagnostic):], msg=msg)
                    self.assertEqual(capture.diagnostic, diagnostic, msg=msg)
                    for _, _, field, _ in KNOBS:
                        self.assertEqual(record["limits"][field], {
                            "requested": 0 if options else None,
                            "granted": grants.get(field), "effective": None}, msg=msg)
                    for name in ("candidate", "report", "diagnostic"):
                        self.assertIsNone(record["channels"][name]["limit_bytes"], msg=msg)
                    self.assertEqual(record["channels"]["diagnostic"]["observed_bytes"],
                                     len(diagnostic), msg=msg)
                    self.assertEqual(record["report_charged_bytes"],
                                     len(expected.stdout) - len(diagnostic), msg=msg)

    def test_t2_host_grant_range_and_types(self):
        for value in (0, 9007199254740990):
            with self.subTest(valid_report_grant=value):
                capture = launch(grants={"report_bytes": value})
                msg = capture.failure_context()
                record = self.assert_fixture_delivery(capture, msg=msg)
                self.assertEqual(record["limits"]["report_bytes"], {
                    "requested": None, "granted": value, "effective": value or None}, msg=msg)
        for grants in ({"report_bytes": -1}, {"wall_ms": True}, {"stall_ms": 1.0},
                       {"diagnostic_bytes": 9007199254740991}, {"unknown": 0}):
            with self.subTest(grants=grants):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, grants=grants)
                msg = capture.failure_context()
                record = self.refusal(capture, "run_isolation_unavailable", {"reason": "policy"},
                                      exit_class="resource_refusal", status=1, msg=msg)
                self.assertIsNone(record["producer"]["stage"], msg=msg)
                self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic, msg=msg)

    def inactive_control_results(self, runner):
        for name, value in INACTIVE_CONTROLS:
            result = subprocess.run(
                [sys.executable, "-B", str(runner), name, value],
                capture_output=True, env=clean_environment(), timeout=10, check=False)
            yield name, result
        for name in INACTIVE_ENVIRONMENT:
            environment = clean_environment()
            environment[name] = "0"
            # Main succeeds on help. A missing environment dispatch cannot
            # masquerade as a missing-arguments usage refusal.
            result = subprocess.run(
                [sys.executable, "-B", str(runner), "--help"],
                capture_output=True, env=environment, timeout=10, check=False)
            yield name, result

    def assert_dispatch_refusal(self, name, result):
        msg = "channel dispatch refusal missing for " + name + "; " + json.dumps({
            "terminal": None, "supervisor_wait_status": None,
            "runner_wait_status": result.returncode, "report_bytes": repr(result.stdout),
            "diagnostic_length_bytes": len(result.stderr),
            "diagnostic_prefix": result.stderr[:4096].decode("utf-8", "replace"),
        }, separators=(",", ":"))
        expected_text = json.loads((ROOT / "docker/run_failure_codes.json").read_bytes())[
            "codes"]["run_request_invalid"]["english"][0]
        self.assertEqual(result.returncode, 2, msg=msg)
        self.assertEqual(result.stderr, b"", msg=msg)
        self.assertEqual(json.loads(result.stdout), {
            "engine": "pineforge", "error": expected_text,
            "code": "run_request_invalid", "args": {"option": "arguments"}}, msg=msg)
        self.assertEqual(result.stdout.count(b"\n"), 1, msg=msg)
        self.assertTrue(result.stdout.endswith(b"\n"), msg=msg)

    def test_t2_new_knobs_without_activation_are_usage_errors(self):
        for name, result in self.inactive_control_results(ROOT / "docker/run_json.py"):
            with self.subTest(control=name):
                self.assert_dispatch_refusal(name, result)

    def test_t2_inactive_knob_assertions_detect_removed_dispatch(self):
        runner = ROOT / "docker/run_json.py"
        original = runner.read_text(encoding="utf-8")
        dispatch = (
            '        if (any(token.partition("=")[0] in channel_flags for token in channel_tokens)\n'
            '                or channel_environment.intersection(os.environ)):\n'
            '            import pineforge_report_supervise\n'
            '            return pineforge_report_supervise.main(\n'
            '                pineforge_report_supervise.direct_arguments(tokens))\n'
        )
        self.assertEqual(original.count(dispatch), 1, "the real runner dispatch must be present")
        with tempfile.TemporaryDirectory(prefix="pf-no-channel-dispatch-") as directory:
            altered = Path(directory) / "run_json.py"
            for sibling in runner.parent.iterdir():
                if sibling != runner and sibling.suffix in (".py", ".json", ".def"):
                    (altered.parent / sibling.name).symlink_to(sibling)
            altered.write_text(original.replace(dispatch, "", 1), encoding="utf-8")
            baseline = subprocess.run(
                [sys.executable, "-B", str(altered), "--help"],
                capture_output=True, env=clean_environment(), timeout=10, check=False)
            self.assertEqual(baseline.returncode, 0, baseline.stderr.decode("utf-8", "replace"))
            self.assertIn(b"--magnifier-samples", baseline.stdout)
            self.assertEqual(baseline.stderr, b"")
            tested = []
            for name, result in self.inactive_control_results(altered):
                with self.subTest(control=name):
                    if name.startswith("--"):
                        self.assertEqual(result.returncode, 2, result.stderr)
                        self.assertIn(name.encode(), result.stderr)
                        self.assertEqual(json.loads(result.stdout)["code"], "run_request_invalid")
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout, baseline.stdout)
                        self.assertEqual(result.stderr, b"")
                    with self.assertRaisesRegex(AssertionError, "channel dispatch refusal"):
                        self.assert_dispatch_refusal(name, result)
                    tested.append(name)
            self.assertEqual(tested, [name for name, _ in INACTIVE_CONTROLS]
                             + list(INACTIVE_ENVIRONMENT))

    def test_t3_duplicated_aliases_are_rejected_before_launch(self):
        for alias in ("report_diagnostic", "report_status", "diagnostic_status",
                      "report_sink", "diagnostic_sink", "status_sink"):
            with self.subTest(alias=alias):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, alias=alias)
                msg = capture.failure_context()
                self.assertEqual(capture.returncode, 2, msg=msg)
                self.assertNotIn(b"PRODUCER_STARTED", capture.report + capture.diagnostic, msg=msg)
                self.assertNotIn(b"pf-run-terminal", capture.report + capture.diagnostic, msg=msg)
                if alias not in ("report_status", "diagnostic_status"):
                    self.refusal(capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                else:
                    self.assertEqual(capture.packets, [], msg=msg)
        capture = launch()
        self.assert_fixture_delivery(capture)

    def test_t3_wrong_direction_and_closed_sink_descriptors(self):
        for direction in ("report", "diagnostic", "sink"):
            with self.subTest(direction=direction):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")},
                                 wrong_direction=direction, sink=direction == "sink")
                msg = capture.failure_context()
                record = self.refusal(capture, "run_request_invalid", {"option": "arguments"},
                                      msg=msg)
                self.assertIsNone(record["producer"]["stage"], msg=msg)
                self.assertNotIn(b"PRODUCER_STARTED", capture.report + capture.diagnostic, msg=msg)
        capture = launch(options=("--report-sink-fd", "2147483647"))
        self.refusal(capture, "run_request_invalid", {"option": "arguments"})

    def test_inherited_data_flags_are_restored_on_every_outcome(self):
        request = {"stdout": encoded(b"flag-check\x00\xff")}
        candidate_bytes = len(reference({}).stdout)
        cases = (
            ("delivery", request, ()),
            ("admission", request, ("--report-channel", "supervised/unknown")),
            ("diagnostic-overflow", request, ("--diagnostic-max-bytes", "1")),
            ("report-overflow", {}, ("--report-max-bytes", str(candidate_bytes - 1))),
        )
        for report_nonblocking in (False, True):
            for diagnostic_nonblocking in (False, True):
                for outcome, inputs, options in cases:
                    with self.subTest(report_nonblocking=report_nonblocking,
                                      diagnostic_nonblocking=diagnostic_nonblocking, outcome=outcome):
                        capture = launch(
                            inputs, options=options,
                            grants={field: 9007199254740990 for _, _, field, _ in KNOBS},
                            endpoint_nonblocking=(report_nonblocking, diagnostic_nonblocking))
                        msg = capture.failure_context()
                        self.assertEqual(set(capture.flags_before), {"report", "diagnostic"}, msg=msg)
                        self.assertEqual(set(capture.flags_after), {"report", "diagnostic"}, msg=msg)
                        for name, nonblocking in (
                                ("report", report_nonblocking), ("diagnostic", diagnostic_nonblocking)):
                            self.assertEqual(bool(capture.flags_before[name] & os.O_NONBLOCK),
                                             nonblocking, msg=msg)
                            self.assertEqual(capture.flags_after[name], capture.flags_before[name], msg=msg)
                        if outcome == "delivery":
                            self.assert_fixture_delivery(capture, msg=msg)
                            self.assertEqual(capture.diagnostic, b"flag-check\x00\xff", msg=msg)
                        elif outcome == "admission":
                            self.refusal(capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                        elif outcome == "diagnostic-overflow":
                            self.refusal(capture, "diagnostic_budget_exceeded", {
                                "cap_bytes": 1, "used_bytes": 1, "requested_bytes": 1},
                                exit_class="resource_refusal", status=1, msg=msg)
                            self.assertEqual(capture.diagnostic, b"f", msg=msg)
                        else:
                            self.refusal(capture, "report_budget_exceeded", {
                                "channel": "json", "cap_bytes": candidate_bytes - 1,
                                "used_bytes": candidate_bytes - 1, "requested_bytes": 1},
                                exit_class="resource_refusal", status=1, msg=msg)
                            self.assertEqual(capture.report, b"", msg=msg)

    def test_inherited_alias_flags_are_restored_after_admission(self):
        for nonblocking in (False, True):
            with self.subTest(nonblocking=nonblocking):
                capture = launch(alias="report_diagnostic",
                                 endpoint_nonblocking=(nonblocking, nonblocking))
                msg = capture.failure_context()
                self.refusal(capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                self.assertEqual(set(capture.flags_before), {"report", "diagnostic"}, msg=msg)
                self.assertEqual(set(capture.flags_after), {"report", "diagnostic"}, msg=msg)
                self.assertEqual(capture.flags_before["report"], capture.flags_before["diagnostic"],
                                 msg=msg)
                for name in ("report", "diagnostic"):
                    self.assertEqual(bool(capture.flags_before[name] & os.O_NONBLOCK),
                                     nonblocking, msg=msg)
                    self.assertEqual(capture.flags_after[name], capture.flags_before[name], msg=msg)

    def wait_fixture_child(self, pid, msg):
        end = time.monotonic() + 1
        while True:
            reaped, status = os.waitpid(pid, os.WNOHANG)
            if reaped:
                self.assertEqual(reaped, pid, msg=msg)
                return status
            self.assertLess(time.monotonic(), end,
                            msg=f"fixture child still present: {process_info(pid)}; {msg}")
            time.sleep(0.01)

    def cleanup_fixture_child(self, state_path):
        if not state_path.exists():
            return
        identities = json.loads(state_path.read_text())
        pid = identities["child"]
        if process_info(pid) is not None:
            try:
                os.killpg(identities["leader"], signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            end = time.monotonic() + 2
            while process_info(pid) is not None:
                try:
                    os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    pass  # Adoption can follow the leader's final exit.
                if time.monotonic() >= end:
                    raise AssertionError(f"TEST cleanup could not reap {process_info(pid)}")
                time.sleep(0.01)

    def test_diagnostic_refusal_stops_descendant_after_leader_exit(self):
        with tempfile.TemporaryDirectory(prefix="pf-diagnostic-descendant-") as directory, fixture_subreaper():
            state = Path(directory) / "child.json"
            progress = Path(directory) / "after-leader"
            try:
                capture = launch({
                    "diagnostic_child_state": str(state),
                    "diagnostic_child_progress": str(progress),
                }, grants={"report_bytes": 65536, "diagnostic_bytes": 65536,
                           "wall_ms": 60000, "stall_ms": 1000},
                   options=("--report-max-bytes", "65536", "--diagnostic-max-bytes", "64",
                            "--channel-wall-ms", "60000", "--channel-stall-ms", "1000"))
                msg = capture.failure_context()
                identities = json.loads(state.read_text())
                self.assertLess(capture.elapsed_seconds, 5, msg=msg)
                record = self.refusal(capture, "diagnostic_budget_exceeded", {
                    "cap_bytes": 64, "used_bytes": 64, "requested_bytes": 1},
                    exit_class="resource_refusal", status=1, msg=msg)
                self.assertEqual(record["producer"]["signal"], signal.SIGTERM, msg=msg)
                self.assertEqual(record["channels"]["diagnostic"]["observed_bytes"], 65, msg=msg)
                self.assertEqual(capture.diagnostic, (b"diagnostic child continues\n" * 3)[:64], msg=msg)
                status = self.wait_fixture_child(identities["child"], msg)
                self.assertTrue(os.WIFSIGNALED(status), msg=msg)
                self.assertEqual(os.WTERMSIG(status), signal.SIGKILL, msg=msg)
                self.assertIsNone(process_info(identities["child"]), msg=msg)
                self.assertTrue(progress.exists(), msg=f"child never attempted D after U exited; {msg}")
                self.assertTrue(any(
                    sample["child"] is not None
                    and sample["child"]["group"] == identities["leader"]
                    and sample["child"]["state"] != "Z" and sample["leader"] is None
                    for sample in capture.descendant_samples), msg=msg)
            finally:
                self.cleanup_fixture_child(state)

    def test_diagnostic_drain_is_bounded_after_leader_exit(self):
        with tempfile.TemporaryDirectory(prefix="pf-diagnostic-drain-") as directory, fixture_subreaper():
            state = Path(directory) / "child.json"
            progress = Path(directory) / "after-leader"
            try:
                capture = launch({
                    "diagnostic_child_state": str(state),
                    "diagnostic_child_progress": str(progress),
                    "leader_completes": True, "detached_diagnostic_child": True,
                }, grants={"report_bytes": 65536, "diagnostic_bytes": 1000000,
                           "wall_ms": 60000, "stall_ms": 1000},
                   options=("--report-max-bytes", "65536", "--diagnostic-max-bytes", "1000000",
                            "--channel-wall-ms", "60000", "--channel-stall-ms", "1000"))
                msg = capture.failure_context()
                identities = json.loads(state.read_text())
                self.assertLess(capture.elapsed_seconds, 5, msg=msg)
                record = self.refusal(capture, "report_channel_failed", {
                    "channel": "diagnostic", "reason": "io"},
                    exit_class="transport_failure", status=1, msg=msg)
                self.assertEqual(record["producer"]["exit_status"], 0, msg=msg)
                self.assertIsNone(process_info(identities["leader"]), msg=msg)
                self.assertTrue(progress.exists(), msg=msg)
                # This TEST escape keeps D open even after the original group
                # is gone, making the independent drain bound indispensable.
                child = process_info(identities["child"])
                self.assertIsNotNone(child, msg=msg)
                self.assertEqual(child["group"], identities["child"], msg=msg)
                diagnostic = record["channels"]["diagnostic"]
                self.assertEqual(diagnostic["limit_bytes"], 1000000, msg=msg)
                self.assertTrue(diagnostic["truncated"], msg=msg)
                self.assertFalse(diagnostic["source_total_known"], msg=msg)
                self.assertTrue(capture.diagnostic, msg=msg)
                line = b"diagnostic child continues\n"
                self.assertEqual(capture.diagnostic,
                                 (line * (len(capture.diagnostic) // len(line) + 1))[
                                     :len(capture.diagnostic)], msg=msg)
            finally:
                self.cleanup_fixture_child(state)

    def assert_post_exit_drain(self, *, options=(), environment=None, grants=None,
                               requested=None, effective=1000, supervisor_path=None):
        with tempfile.TemporaryDirectory(prefix="pf-drain-parameter-") as directory, fixture_subreaper():
            state = Path(directory) / "child.json"
            progress = Path(directory) / "after-parent"
            child_exit = Path(directory) / "child-exit.json"
            inputs = {"closes": [5, 9, 6]}
            expected = reference(inputs)
            request = {
                **inputs, "diagnostic_child_state": str(state),
                "diagnostic_child_progress": str(progress), "diagnostic_child_exit": str(child_exit),
                "leader_completes": True, "detached_diagnostic_child": True,
                "double_fork_diagnostic_child": True,
                "diagnostic_child_lifetime_ms": 2200 if effective is None else 5000,
            }
            try:
                capture = launch(
                    request, options=options, environment=environment, grants=grants,
                    supervisor_path=supervisor_path, watchdog_seconds=8)
                msg = capture.failure_context()
                identities = json.loads(state.read_text())
                orphan_observations = [
                    sample["at"] for sample in capture.descendant_samples
                    if sample["leader"] is None and sample["child"] is not None
                    and sample["child"]["state"] != "Z"]
                self.assertTrue(orphan_observations, msg=msg)
                waited = capture.control["exited_at"] - min(orphan_observations)
                if effective is not None:
                    self.assertGreaterEqual(
                        waited, effective / 1000 - 0.15,
                        msg=f"drain grace timing: returned too soon; {msg}")
                    self.assertLess(
                        waited, effective / 1000 + 0.35,
                        msg=f"drain grace timing: returned too late; {msg}")
                else:
                    self.assertGreaterEqual(
                        waited, 1.9, msg=f"unlimited drain did not wait for the descendant; {msg}")
                    self.assertLess(waited, 2.65, msg=msg)
                self.assertIsNone(process_info(identities["leader"]), msg=msg)
                self.assertEqual(group_members(identities["leader"]), [], msg=msg)
                self.assertNotEqual(identities["group"], identities["leader"], msg=msg)
                self.assertNotEqual(identities["child"], identities["group"], msg=msg)
                self.assertTrue(progress.exists(), msg=msg)
                if effective is None:
                    self.assertTrue(child_exit.exists(), msg=msg)
                    status = self.wait_fixture_child(identities["child"], msg)
                    self.assertTrue(os.WIFEXITED(status), msg=msg)
                    self.assertEqual(os.WEXITSTATUS(status), 0, msg=msg)
                    self.assertIsNone(process_info(identities["child"]), msg=msg)
                    record = self.assert_fixture_delivery(capture, msg=msg)
                    self.assertEqual(capture.report, expected.stdout, msg=msg)
                    self.assertFalse(record["channels"]["diagnostic"]["truncated"], msg=msg)
                    self.assertTrue(record["channels"]["diagnostic"]["source_total_known"], msg=msg)
                else:
                    self.assertFalse(child_exit.exists(), msg=msg)
                    child = process_info(identities["child"])
                    self.assertIsNotNone(child, msg=msg)
                    self.assertNotEqual(child["state"], "Z", msg=msg)
                    self.assertEqual(child["group"], identities["group"], msg=msg)
                    record = self.refusal(capture, "report_channel_failed", {
                        "channel": "diagnostic", "reason": "io"},
                        exit_class="transport_failure", status=1, msg=msg)
                    self.assertEqual(record["producer"]["exit_status"], 0, msg=msg)
                    self.assertIn("--post-exit-drain-ms", " ".join(record["diagnostics"]), msg=msg)
                    self.assertIn("PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS",
                                  " ".join(record["diagnostics"]), msg=msg)
                    self.assertTrue(record["channels"]["diagnostic"]["truncated"], msg=msg)
                    self.assertFalse(record["channels"]["diagnostic"]["source_total_known"], msg=msg)
                self.assertEqual(record["limits"]["post_exit_drain_ms"], {
                    "requested": requested, "granted": (grants or {}).get("post_exit_drain_ms"),
                    "effective": effective}, msg=msg)
                self.assertEqual(record["limits"]["stop_grace_ms"], {
                    "requested": None, "granted": None, "effective": 1000}, msg=msg)
                self.assertTrue(capture.diagnostic, msg=msg)
                line = b"diagnostic child continues\n"
                self.assertEqual(
                    capture.diagnostic,
                    (line * (len(capture.diagnostic) // len(line) + 1))[:len(capture.diagnostic)],
                    msg=msg)
                return capture
            finally:
                self.cleanup_fixture_child(state)

    def test_post_exit_drain_default_names_parameter_and_expires_after_one_second(self):
        self.assert_post_exit_drain()

    def test_post_exit_drain_larger_cli_and_environment_values_are_not_renewed_by_output(self):
        self.assert_post_exit_drain(
            options=("--post-exit-drain-ms", "2200"), requested=2200, effective=2200)
        self.assert_post_exit_drain(
            environment={"PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS": "2200"},
            requested=2200, effective=2200)

    def test_post_exit_drain_zero_waits_until_the_double_fork_descendant_exits_cleanly(self):
        self.assert_post_exit_drain(
            options=("--post-exit-drain-ms", "0"), requested=0, effective=None)
        self.assert_post_exit_drain(
            environment={"PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS": "0"},
            requested=0, effective=None)

    def test_post_exit_drain_default_assertions_reject_a_doctored_grace(self):
        self.assert_post_exit_drain()
        text = SUPERVISOR.read_text(encoding="utf-8")
        original = "DEFAULT_POST_EXIT_DRAIN_MS = 1000"
        self.assertEqual(text.count(original), 1)
        with tempfile.TemporaryDirectory(prefix="pf-drain-mutation-") as directory:
            changed = supervisor_fixture(
                directory, text=text.replace(original, "DEFAULT_POST_EXIT_DRAIN_MS = 3000"))
            with self.assertRaisesRegex(AssertionError, "drain grace timing: returned too late"):
                self.assert_post_exit_drain(supervisor_path=changed)

    def test_post_exit_drain_parameter_grammar_precedence_and_grants(self):
        maximum = 9007199254740990
        cases = (
            ((), {}, {}, None, None, 1000),
            (("--post-exit-drain-ms", "0"), {}, {}, 0, None, None),
            ((), {"PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS": "0"}, {}, 0, None, None),
            (("--post-exit-drain-ms", "0002300"),
             {"PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS": "invalid"}, {}, 2300, None, 2300),
            (("--post-exit-drain-ms", str(maximum)), {}, {}, maximum, None, maximum),
            ((), {}, {"post_exit_drain_ms": 2700}, None, 2700, 2700),
            ((), {}, {"post_exit_drain_ms": 0}, None, 0, 1000),
            (("--post-exit-drain-ms", "1800"), {}, {"post_exit_drain_ms": 2700}, 1800, 2700, 1800),
        )
        for options, environment, grants, requested, granted, effective in cases:
            with self.subTest(options=options, environment=environment, grants=grants):
                capture = launch(options=options, environment=environment, grants=grants)
                record = self.assert_fixture_delivery(capture)
                self.assertEqual(record["limits"]["post_exit_drain_ms"], {
                    "requested": requested, "granted": granted, "effective": effective},
                    msg=capture.failure_context())
                self.assertEqual(record["limits"]["stop_grace_ms"], {
                    "requested": None, "granted": None, "effective": 1000},
                                 msg=capture.failure_context())
        for value in ("", "-1", "+1", " 1", "1 ", "1.0", "1e3", "0x10", "١", str(maximum + 1)):
            for environment_only in (False, True):
                with self.subTest(value=value, environment_only=environment_only):
                    capture = launch(
                        {"stdout": encoded(b"producer marker")},
                        options=() if environment_only else ("--post-exit-drain-ms", value),
                        environment={"PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS": value}
                        if environment_only else {})
                    msg = capture.failure_context()
                    record = self.refusal(
                        capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                    self.assertIsNone(record["producer"]["stage"], msg=msg)
                    self.assertEqual(capture.diagnostic, b"", msg=msg)
        capture = launch(
            {"stdout": encoded(b"producer marker")},
            options=("--post-exit-drain-ms", "2701"), grants={"post_exit_drain_ms": 2700})
        record = self.refusal(capture, "run_request_invalid", {"option": "arguments"})
        self.assertEqual(record["limits"]["post_exit_drain_ms"], {
            "requested": 2701, "granted": 2700, "effective": None}, msg=capture.failure_context())
        self.assertIsNone(record["producer"]["stage"], msg=capture.failure_context())
        capture = launch(
            {"stdout": encoded(b"producer marker")},
            options=("--post-exit-drain-ms", "0"), grants={"post_exit_drain_ms": 2700})
        record = self.refusal(capture, "run_request_invalid", {"option": "arguments"})
        self.assertEqual(record["limits"]["post_exit_drain_ms"], {
            "requested": 0, "granted": 2700, "effective": None},
            msg=capture.failure_context())
        self.assertIsNone(record["producer"]["stage"], msg=capture.failure_context())

    def assert_stop_grace_producer(self, *, options=(), environment=None,
                                   requested=None, granted=None, effective=1000,
                                   release_after_ms=None, output_after_term=False,
                                   supervisor_path=None):
        with tempfile.TemporaryDirectory(prefix="pf-stop-grace-producer-") as directory:
            directory = Path(directory)
            identity = directory / "producer.json"
            term = directory / "term.json"
            hold = directory / "release"
            request = {
                "stdout": encoded(b"diagnostic output " * 512),
                "producer_identity_path": str(identity),
                "producer_term_path": str(term),
                "producer_release_path": str(hold),
                "hold_after_output_path": str(hold),
                "ignore_sigterm": True,
                "exit_after_hold": True,
                "output_after_term": output_after_term,
            }
            base_options = (
                "--diagnostic-max-bytes", "64",
                "--channel-wall-ms", "60000",
                "--channel-stall-ms", "1000",
            )
            capture = launch(
                request,
                options=(*base_options, *options),
                environment=environment,
                release_producer_after_ms=release_after_ms,
                supervisor_path=supervisor_path,
                watchdog_seconds=8,
            )
            msg = capture.failure_context()
            self.assertIsNotNone(capture.control.get("producer_term_at"), msg=msg)
            self.assertIsNotNone(capture.control.get("producer_term_process"), msg=msg)
            identities = json.loads(identity.read_text())
            self.assertEqual(capture.control["producer_term_process"]["pid"],
                             identities["pid"], msg=msg)
            if effective is None:
                self.assertIsNotNone(
                    capture.control.get("producer_alive_before_release"), msg=msg)
                self.assertIsNotNone(capture.control.get("producer_released_at"), msg=msg)
                waited = (capture.control["producer_released_at"]
                          - capture.control["producer_term_at"])
                self.assertGreaterEqual(
                    waited, release_after_ms / 1000 - 0.1, msg=msg)
                self.assertGreater(waited, 1.0, msg=msg)
            else:
                self.assertIsNotNone(capture.control.get("exited_at_monotonic"), msg=msg)
                elapsed = (capture.control["exited_at_monotonic"]
                           - capture.control["producer_term_at"])
                self.assertGreaterEqual(
                    elapsed, effective / 1000 - 0.15,
                    msg=f"stop grace timing: returned too soon; {msg}")
                self.assertLess(
                    elapsed, effective / 1000 + 0.5,
                    msg=f"stop grace timing: returned too late; {msg}")
            record = self.refusal(capture, "diagnostic_budget_exceeded", {
                "cap_bytes": 64, "used_bytes": 64, "requested_bytes": 1},
                exit_class="resource_refusal", status=1, msg=msg)
            self.assertEqual(record["limits"]["stop_grace_ms"], {
                "requested": requested, "granted": granted, "effective": effective}, msg=msg)
            if effective is None:
                self.assertEqual(record["producer"]["exit_status"], 0, msg=msg)
                self.assertIsNone(record["producer"]["signal"], msg=msg)
            else:
                self.assertIsNone(record["producer"]["exit_status"], msg=msg)
                self.assertEqual(record["producer"]["signal"], signal.SIGKILL, msg=msg)
                diagnostics = " ".join(record["diagnostics"])
                self.assertIn("--stop-grace-ms", diagnostics, msg=msg)
                self.assertIn("PINEFORGE_CHANNEL_STOP_GRACE_MS", diagnostics, msg=msg)
            self.assertIsNone(process_info(identities["pid"]), msg=msg)
            self.assertEqual(group_members(identities["group"]), [], msg=msg)
            return capture

    def test_stop_grace_default_kills_term_ignoring_producer_after_one_second(self):
        self.assert_stop_grace_producer()

    def test_stop_grace_larger_cli_and_environment_values_are_not_renewed_by_output(self):
        self.assert_stop_grace_producer(
            options=("--stop-grace-ms", "2200"), requested=2200, effective=2200,
            output_after_term=True)
        self.assert_stop_grace_producer(
            environment={"PINEFORGE_CHANNEL_STOP_GRACE_MS": "2200"},
            requested=2200, effective=2200, output_after_term=True)

    def test_stop_grace_zero_waits_for_host_release_without_killing_producer(self):
        self.assert_stop_grace_producer(
            options=("--stop-grace-ms", "0"), requested=0, effective=None,
            release_after_ms=1300)
        self.assert_stop_grace_producer(
            environment={"PINEFORGE_CHANNEL_STOP_GRACE_MS": "0"},
            requested=0, effective=None, release_after_ms=1300)

    def test_stop_grace_default_assertions_reject_a_doctored_grace(self):
        self.assert_stop_grace_producer()
        text = SUPERVISOR.read_text(encoding="utf-8")
        original = "DEFAULT_STOP_GRACE_MILLISECONDS = 1000"
        self.assertEqual(text.count(original), 1)
        with tempfile.TemporaryDirectory(prefix="pf-stop-grace-mutation-") as directory:
            changed = supervisor_fixture(
                directory, text=text.replace(
                    original, "DEFAULT_STOP_GRACE_MILLISECONDS = 3000"))
            with self.assertRaisesRegex(AssertionError, "stop grace timing"):
                self.assert_stop_grace_producer(supervisor_path=changed)

    def test_stop_grace_parameter_grammar_precedence_and_grants(self):
        maximum = 9007199254740990
        cases = (
            ((), {}, {}, None, None, 1000),
            (("--stop-grace-ms", "0"), {}, {}, 0, None, None),
            ((), {"PINEFORGE_CHANNEL_STOP_GRACE_MS": "0"}, {}, 0, None, None),
            (("--stop-grace-ms", "0002300"),
             {"PINEFORGE_CHANNEL_STOP_GRACE_MS": "invalid"}, {}, 2300, None, 2300),
            (("--stop-grace-ms", str(maximum)), {}, {}, maximum, None, maximum),
            ((), {}, {"stop_grace_ms": 2700}, None, 2700, 2700),
            ((), {}, {"stop_grace_ms": 0}, None, 0, 1000),
            (("--stop-grace-ms", "1800"), {}, {"stop_grace_ms": 2700}, 1800, 2700, 1800),
        )
        for options, environment, grants, requested, granted, effective in cases:
            with self.subTest(options=options, environment=environment, grants=grants):
                capture = launch(options=options, environment=environment, grants=grants)
                record = self.assert_fixture_delivery(capture)
                self.assertEqual(record["limits"]["stop_grace_ms"], {
                    "requested": requested, "granted": granted, "effective": effective},
                    msg=capture.failure_context())
        for value in ("", "-1", "+1", " 1", "1 ", "1.0", "1e3", "0x10", "١",
                      str(maximum + 1)):
            for environment_only in (False, True):
                with self.subTest(value=value, environment_only=environment_only):
                    capture = launch(
                        {"stdout": encoded(b"producer marker")},
                        options=() if environment_only else ("--stop-grace-ms", value),
                        environment={"PINEFORGE_CHANNEL_STOP_GRACE_MS": value}
                        if environment_only else {})
                    msg = capture.failure_context()
                    record = self.refusal(
                        capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                    self.assertIsNone(record["producer"]["stage"], msg=msg)
                    self.assertEqual(capture.diagnostic, b"", msg=msg)
        for options, grants in (
                (("--stop-grace-ms", "2701"), {"stop_grace_ms": 2700}),
                (("--stop-grace-ms", "0"), {"stop_grace_ms": 2700})):
            capture = launch(
                {"stdout": encoded(b"producer marker")},
                options=options, grants=grants)
            record = self.refusal(capture, "run_request_invalid", {"option": "arguments"})
            requested = int(options[1])
            self.assertEqual(record["limits"]["stop_grace_ms"], {
                "requested": requested, "granted": 2700, "effective": None},
                msg=capture.failure_context())
            self.assertIsNone(record["producer"]["stage"], msg=capture.failure_context())

    def test_t10_fd1_forgery_is_raw_diagnostics(self):
        forgery = b'{"engine":"pineforge","error":"forged","code":"harness_internal_error","args":{}}\n'
        capture = launch({"stdout": encoded(forgery)})
        msg = capture.failure_context()
        self.assertEqual(capture.diagnostic, forgery, msg=msg)
        self.assertNotIn(b"forged", capture.report, msg=msg)
        self.assert_fixture_delivery(capture, msg=msg)

    def test_t10_relayed_claim_cannot_become_supervisor_fault(self):
        failure = b'{"engine":"pineforge","error":"claimed","code":"harness_internal_error","args":{}}\n'
        for exit_status in (0, 1):
            with self.subTest(exit_status=exit_status):
                capture = launch({"candidate": encoded(failure), "exit_status": exit_status})
                msg = capture.failure_context()
                record = self.refusal(
                    capture, "run_producer_failed", {"reason": "claim"},
                    exit_class="producer_failure", status=1, msg=msg)
                self.assertEqual(capture.report, failure, msg=msg)
                self.assertEqual(record["producer"]["failure"], {
                    "code": "harness_internal_error", "args": {}, "authority": "producer"}, msg=msg)
                self.assertEqual(record["producer"]["exit_status"], exit_status, msg=msg)

    def test_t10_unknown_code_and_wrong_args_are_protocol_failure(self):
        for code, args in (("not_a_registered_failure", {}),
                           ("report_budget_exceeded", {"channel": "json", "cap_bytes": "bad",
                                                       "used_bytes": 3, "requested_bytes": 1})):
            with self.subTest(code=code):
                candidate = json.dumps({
                    "engine": "pineforge", "error": "claim", "code": code, "args": args},
                    separators=(",", ":")).encode() + b"\n"
                capture = launch({"candidate": encoded(candidate), "exit_status": 1})
                msg = capture.failure_context()
                self.refusal(capture, "report_channel_failed",
                             {"channel": "candidate", "reason": "protocol"},
                             exit_class="transport_failure", status=1, msg=msg)
                self.assertNotEqual(capture.report, candidate, msg=msg)

    def test_t11_json_exact_fit_and_overflow_probe(self):
        request = {"closes": [1, 4, 2], "label": "café🙂.csv"}
        expected = reference(request).stdout
        for allowance in (len(expected), len(expected) - 1):
            with self.subTest(allowance=allowance):
                capture = launch(request, options=("--report-max-bytes", str(allowance)))
                msg = capture.failure_context()
                if allowance == len(expected):
                    self.assertEqual(capture.report, expected, msg=msg)
                    self.assert_fixture_delivery(capture, msg=msg)
                else:
                    record = self.refusal(capture, "report_budget_exceeded", {
                        "channel": "json", "cap_bytes": allowance,
                        "used_bytes": allowance, "requested_bytes": 1},
                        exit_class="resource_refusal", status=1, msg=msg)
                    self.assertEqual(capture.report, b"", msg=msg)
                    self.assertEqual(record["report_charged_bytes"], allowance, msg=msg)
                    cell = record["channels"]["candidate"]
                    self.assertEqual(cell["observed_bytes"], allowance + 1, msg=msg)
                    self.assertEqual(cell["accepted_bytes"], allowance, msg=msg)
                    self.assertTrue(cell["truncated"], msg=msg)
                    self.assertFalse(cell["source_total_known"], msg=msg)

    def test_t11_planned_sink_larger_than_aggregate_refuses_before_start(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")},
                         options=("--report-max-bytes", "512"), planned_sink_bytes=513)
        msg = capture.failure_context()
        record = self.refusal(capture, "report_budget_exceeded", {
            "channel": "aggregate", "cap_bytes": 512, "used_bytes": 0, "requested_bytes": 513},
            exit_class="resource_refusal", status=1, msg=msg)
        self.assertIsNone(record["producer"]["stage"], msg=msg)
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic, msg=msg)
        self.assertLessEqual(record["report_charged_bytes"], 512, msg=msg)

    def test_t12_sink_exact_fit_and_real_finalizer_receipt(self):
        with validator_observer() as observer:
            for closes, first_ts, fragment in (([3, 8, 5], 2000, 1),
                                                ([21, 17, 19, 23], 9000, 65536)):
                with self.subTest(closes=closes, fragment=fragment):
                    request = {"closes": closes, "first_ts": first_ts, "inherited_sink": True,
                               "fragment_bytes": fragment, "stderr": encoded(b"native\x00\xff")}
                    expected = reference(request)
                    native = sink_wire(closes, first_ts)
                    cap = len(native)
                    allowance = cap + len(expected.stdout)
                    capture = launch(request, sink=True, planned_sink_bytes=cap, observer=observer,
                                     options=("--report-max-bytes", str(allowance)))
                    msg = capture.failure_context()
                    record = self.assert_fixture_delivery(capture, msg=msg)
                    self.assertEqual(expected.returncode, 0, msg=msg)
                    self.assertEqual(capture.report, expected.stdout, msg=msg)
                    self.assertEqual(capture.sink, native, msg=msg)
                    self.assertEqual(capture.diagnostic, b"native\x00\xff", msg=msg)
                    self.assertEqual(capture.finalizer_receipt(),
                                     sink_receipt(native, complete=True, equity_count=len(closes)),
                                     msg=msg)
                    arguments = capture.finalizer_arguments()
                    self.assertEqual(arguments.count(b"--max-bytes"), 1, msg=msg)
                    self.assertEqual(arguments[arguments.index(b"--max-bytes") + 1],
                                     str(cap).encode(), msg=msg)
                    self.assertEqual(len([event for event in capture.validator_events
                                          if event[:1] == b"S"]), 1, msg=msg)
                    logical = hashlib.sha256(
                        b"".join(native.splitlines(keepends=True)[:-1])).hexdigest()
                    self.assertEqual(record["sink_verification"], {
                        "exit_status": 0, "signal": None, "complete": True,
                        "counts": {tag: len(closes) if tag == "E" else 0 for tag in "ETOVKXH"},
                        "sha256": logical, "finalizer_version": "1.1.0"}, msg=msg)
                    cell = record["channels"]["sink"]
                    self.assertEqual(cell["observed_bytes"], cap, msg=msg)
                    self.assertEqual(cell["accepted_bytes"], cap, msg=msg)
                    self.assertEqual(cell["written_bytes"], cap, msg=msg)
                    self.assertEqual(cell["limit_bytes"], cap, msg=msg)
                    self.assertEqual(cell["accepted_sha256"], hashlib.sha256(native).hexdigest(),
                                     msg=msg)
                    self.assertEqual(cell["sha256"], hashlib.sha256(native).hexdigest(), msg=msg)
                    self.assertEqual(cell["logical_sha256"], logical, msg=msg)
                    self.assertTrue(cell["complete"], msg=msg)
                    self.assertTrue(cell["source_total_known"], msg=msg)
                    self.assertFalse(cell["truncated"], msg=msg)
                    self.assertEqual(record["report_charged_bytes"], allowance, msg=msg)
                    self.assertEqual(record["channels"]["candidate"]["limit_bytes"],
                                     len(expected.stdout), msg=msg)
                    job = record["usage"]["job"]["resources"]
                    for name, used, limit in (
                            ("report_bytes", allowance, allowance), ("sink_bytes", cap, cap),
                            ("diagnostic_bytes", len(b"native\x00\xff"), None)):
                        self.assertEqual(job[name]["used"], used, msg=msg)
                        self.assertEqual(job[name]["limit"], limit, msg=msg)
                        self.assertEqual(job[name]["state"], "measured", msg=msg)
                        self.assertEqual(job[name]["source"], "channel", msg=msg)
                    stages = record["usage"]["stages"]
                    self.assertEqual(stages["run"]["resources"]["sink_bytes"]["used"], cap,
                                     msg=msg)
                    self.assertEqual(stages["finalize"]["resources"]["report_bytes"]["used"],
                                     len(expected.stdout), msg=msg)
                    self.assertEqual(stages["publish"]["resources"]["sink_bytes"]["used"], cap,
                                     msg=msg)
                    self.assertEqual(stages["publish"]["resources"]["report_bytes"]["used"],
                                     len(expected.stdout), msg=msg)

    def test_t12_sink_one_over_real_finalizer_receipt(self):
        with validator_observer() as observer:
            for closes, fragment in (([4, 9], 1), ([31, 33, 29, 34], 65536)):
                with self.subTest(closes=closes, fragment=fragment):
                    native = sink_wire(closes)
                    cap = len(native) - 1
                    capture = launch(
                        {"closes": closes, "inherited_sink": True, "fragment_bytes": fragment,
                         "sink_suffix": encoded(b"unread" * 65536)},
                        sink=True, planned_sink_bytes=cap, observer=observer,
                        options=("--report-max-bytes", "65536"))
                    msg = capture.failure_context()
                    record = self.refusal(capture, "report_sink_failed", {
                        "reason": "sink_exhausted", "sink_bytes": cap + 1, "cap_bytes": cap},
                        exit_class="transport_failure", status=1, msg=msg)
                    # This is V's actual FILE receipt, observed at its write,
                    # compared byte-for-byte with finalizer 1.1.0's contract.
                    self.assertEqual(capture.finalizer_receipt(),
                                     sink_receipt(native, complete=False, cap=cap), msg=msg)
                    self.assertTrue(any(event[:1] == b"P" for event in capture.validator_events),
                                    msg=msg)
                    verification = record["sink_verification"]
                    self.assertEqual(verification["exit_status"], 8, msg=msg)
                    self.assertIsNone(verification["signal"], msg=msg)
                    self.assertFalse(verification["complete"], msg=msg)
                    self.assertEqual(verification["counts"], dict.fromkeys("ETOVKXH", 0),
                                     msg=msg)
                    self.assertIsNone(verification["sha256"], msg=msg)
                    cell = record["channels"]["sink"]
                    self.assertEqual(cell["observed_bytes"], cap + 1, msg=msg)
                    self.assertEqual(cell["accepted_bytes"], cap, msg=msg)
                    self.assertEqual(cell["accepted_sha256"], hashlib.sha256(native[:cap]).hexdigest(),
                                     msg=msg)
                    self.assertEqual(cell["written_bytes"], 0, msg=msg)
                    self.assertIsNone(cell["sha256"], msg=msg)
                    self.assertIsNone(cell["logical_sha256"], msg=msg)
                    self.assertTrue(cell["truncated"], msg=msg)
                    self.assertFalse(cell["source_total_known"], msg=msg)
                    self.assertFalse(cell["complete"], msg=msg)
                    self.assertEqual(capture.sink, b"", msg=msg)
                    self.assertEqual(record["channels"]["candidate"]["observed_bytes"], 0,
                                     msg=msg)
                    self.assertEqual(record["report_charged_bytes"], cap + len(capture.report),
                                     msg=msg)
                    if capture.report:
                        self.assertEqual(json.loads(capture.report)["args"],
                                         record["failure"]["args"], msg=msg)

    def test_t12_paused_validator_cannot_erase_overflow_evidence(self):
        native = sink_wire([11, 15, 13, 17])
        cap = len(native) - 1
        with validator_observer() as observer:
            capture = launch(
                {"closes": [11, 15, 13, 17], "inherited_sink": True,
                 "sink_suffix": encoded(b"unread" * 65536)},
                sink=True, planned_sink_bytes=cap, observer=observer, pause_probe=True,
                options=("--report-max-bytes", "65536", "--channel-wall-ms", "2000",
                         "--channel-stall-ms", "500"))
        msg = capture.failure_context()
        probes = {int(event[1:]) for event in capture.validator_events if event[:1] == b"P"}
        self.assertEqual(len(probes), 1, msg=f"real V never read the overflow probe; {msg}")
        self.assertEqual(capture.stopped_validators, probes,
                         msg=f"real V was not observed stopped; {msg}")
        self.assertEqual(capture.control["host_paused_validators"], [], msg=msg)
        record = self.refusal(capture, "report_sink_failed", {
            "reason": "sink_exhausted", "sink_bytes": cap + 1, "cap_bytes": cap},
            exit_class="transport_failure", status=1, msg=msg)
        self.assertLess(capture.elapsed_seconds, 3.5, msg=msg)
        for pid in probes:
            self.assertIsNone(process_info(pid), msg=msg)
            self.assertEqual(group_members(pid), [], msg=msg)
        self.assertEqual(capture.finalizer_receipt(), b"", msg=msg)
        self.assertEqual(record["sink_verification"], {
            "exit_status": None, "signal": signal.SIGKILL, "complete": False,
            "counts": dict.fromkeys("ETOVKXH"), "sha256": None, "finalizer_version": None},
            msg=msg)
        cell = record["channels"]["sink"]
        self.assertEqual(cell["observed_bytes"], cap + 1, msg=msg)
        self.assertEqual(cell["accepted_bytes"], cap, msg=msg)
        self.assertEqual(cell["accepted_sha256"], hashlib.sha256(native[:cap]).hexdigest(), msg=msg)
        self.assertFalse(cell["source_total_known"], msg=msg)
        self.assertIsNone(cell["logical_sha256"], msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        job = record["usage"]["job"]["resources"]
        self.assertEqual(job["sink_bytes"]["used"], cap, msg=msg)
        self.assertEqual(job["sink_bytes"]["state"], "measured", msg=msg)

    def assert_sink_overflow_stop_grace(self, *, options=(), requested=None,
                                        effective=1000, release_after_ms=None):
        native = sink_wire([11, 15, 13, 17])
        cap = len(native) - 1
        with validator_observer() as observer:
            capture = launch(
                {"closes": [11, 15, 13, 17], "inherited_sink": True,
                 "sink_suffix": encoded(b"unread" * 65536)},
                sink=True, planned_sink_bytes=cap, observer=observer,
                ignore_validator_term=True, hold_validator_overflow=True,
                output_after_validator_term=True,
                release_validator_after_ms=release_after_ms,
                watchdog_seconds=8,
                options=("--report-max-bytes", "65536",
                         "--channel-wall-ms", "60000", "--channel-stall-ms", "500",
                         *options))
        msg = capture.failure_context()
        hold = capture.control.get("validator_overflow_hold")
        self.assertIsNotNone(hold, msg=msg)
        if effective is None:
            self.assertIsNotNone(
                capture.control.get("validator_alive_before_release"), msg=msg)
            self.assertIsNotNone(capture.control.get("validator_released_at"), msg=msg)
            waited = (capture.control["validator_released_at"] - hold["at"])
            self.assertGreaterEqual(
                waited, release_after_ms / 1000 - 0.1, msg=msg)
            self.assertGreater(waited, 1.0, msg=msg)
            self.assertIsNone(capture.control.get("validator_term_at"), msg=msg)
        else:
            self.assertIsNotNone(capture.control.get("validator_term_at"), msg=msg)
            self.assertIsNotNone(
                capture.control.get("validator_term_process"), msg=msg)
            self.assertEqual(
                len([event for event in capture.validator_events if event[:1] == b"T"]),
                1, msg=msg)
            elapsed = (capture.control["exited_at_monotonic"]
                       - capture.control["validator_term_at"])
            self.assertGreaterEqual(
                elapsed, effective / 1000 - 0.15,
                msg=f"stop grace timing: validator returned too soon; {msg}")
            self.assertLess(
                elapsed, effective / 1000 + 0.75,
                msg=f"stop grace timing: validator returned too late; {msg}")
        record = self.refusal(capture, "report_sink_failed", {
            "reason": "sink_exhausted", "sink_bytes": cap + 1, "cap_bytes": cap},
            exit_class="transport_failure", status=1, msg=msg)
        self.assertEqual(record["limits"]["stop_grace_ms"], {
            "requested": requested, "granted": None, "effective": effective}, msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        if effective is None:
            self.assertEqual(record["sink_verification"], {
                "exit_status": 8, "signal": None, "complete": False,
                "counts": {tag: 0 for tag in "ETOVKXH"},
                "sha256": None, "finalizer_version": "1.1.0"}, msg=msg)
            self.assertEqual(
                capture.finalizer_receipt(),
                sink_receipt(native, complete=False, cap=cap), msg=msg)
        else:
            pid = capture.control["validator_term_pid"]
            self.assertEqual(record["sink_verification"], {
                "exit_status": None, "signal": signal.SIGKILL, "complete": False,
                "counts": dict.fromkeys("ETOVKXH"), "sha256": None,
                "finalizer_version": None}, msg=msg)
            diagnostics = " ".join(record["diagnostics"])
            self.assertIn("--stop-grace-ms", diagnostics, msg=msg)
            self.assertIn("PINEFORGE_CHANNEL_STOP_GRACE_MS", diagnostics, msg=msg)
            self.assertIsNone(process_info(pid), msg=msg)
            self.assertEqual(group_members(pid), [], msg=msg)

    def test_sink_overflow_stop_grace_covers_ignored_term_validator(self):
        self.assert_sink_overflow_stop_grace()
        self.assert_sink_overflow_stop_grace(
            options=("--stop-grace-ms", "2200"), requested=2200, effective=2200)
        self.assert_sink_overflow_stop_grace(
            options=("--stop-grace-ms", "0"), requested=0, effective=None,
            release_after_ms=1300)

    def test_t13_exported_sink_and_candidate_share_one_allowance(self):
        for closes, label in (([6, 10, 8], "café🙂.csv"), ([20, 25, 21, 27], "longer.csv")):
            request = {"closes": closes, "label": label, "inherited_sink": True}
            expected = reference(request).stdout
            native = sink_wire(closes)
            for allowance in (len(native) + len(expected) - 1, len(native) + len(expected)):
                with self.subTest(closes=closes, allowance=allowance):
                    # No planned B: S must translate R into a positive V cap.
                    capture = launch(request, sink=True,
                                     options=("--report-max-bytes", str(allowance)))
                    msg = capture.failure_context()
                    self.assertLess(len(native), allowance, msg=msg)
                    self.assertLess(len(expected), allowance, msg=msg)
                    if allowance == len(native) + len(expected):
                        record = self.assert_fixture_delivery(capture, msg=msg)
                        self.assertEqual(capture.report, expected, msg=msg)
                        self.assertEqual(capture.sink, native, msg=msg)
                        self.assertEqual(record["report_charged_bytes"], allowance, msg=msg)
                    else:
                        record = self.refusal(capture, "report_budget_exceeded", {
                            "channel": "aggregate", "cap_bytes": allowance,
                            "used_bytes": allowance, "requested_bytes": 1},
                            exit_class="resource_refusal", status=1, msg=msg)
                        self.assertEqual(capture.report, b"", msg=msg)
                        self.assertEqual(capture.sink, b"", msg=msg)
                        self.assertEqual(record["report_charged_bytes"], allowance, msg=msg)
                        candidate = record["channels"]["candidate"]
                        self.assertEqual(candidate["limit_bytes"], len(expected) - 1, msg=msg)
                        self.assertEqual(candidate["observed_bytes"], len(expected), msg=msg)
                        self.assertEqual(candidate["accepted_bytes"], len(expected) - 1, msg=msg)
                        self.assertEqual(candidate["sha256"], hashlib.sha256(expected[:-1]).hexdigest(),
                                         msg=msg)
                        self.assertTrue(candidate["truncated"], msg=msg)
                        self.assertFalse(candidate["source_total_known"], msg=msg)
                    self.assertEqual(record["limits"]["sink_bytes"], allowance, msg=msg)
                    sink = record["channels"]["sink"]
                    self.assertEqual(sink["observed_bytes"], len(native), msg=msg)
                    self.assertEqual(sink["accepted_bytes"], len(native), msg=msg)
                    self.assertEqual(sink["accepted_sha256"], hashlib.sha256(native).hexdigest(),
                                     msg=msg)
                    self.assertTrue(record["sink_verification"]["complete"], msg=msg)

    def test_t17_native_fragments_and_explicit_empty_sink(self):
        with validator_observer() as observer:
            for fragment in (b"", b'{"schema":"pf-report-sink"'):
                for status in (0, 7):
                    with self.subTest(fragment=fragment, status=status):
                        capture = launch(
                            {"inherited_sink": True, "native_sink": encoded(fragment),
                             "fragment_bytes": 1, "exit_status": status},
                            sink=True, observer=observer)
                        msg = capture.failure_context()
                        if status:
                            record = self.refusal(capture, "run_producer_failed", {
                                "reason": "exit", "exit_status": status},
                                exit_class="producer_failure", status=1, msg=msg)
                        else:
                            record = self.refusal(capture, "report_channel_failed", {
                                "channel": "sink", "reason": "protocol"},
                                exit_class="transport_failure", status=1, msg=msg)
                        self.assertEqual(capture.finalizer_receipt(),
                                         sink_receipt(fragment, complete=False), msg=msg)
                        self.assertEqual(record["producer"]["exit_status"], status, msg=msg)
                        self.assertEqual(record["sink_verification"]["exit_status"], 6, msg=msg)
                        self.assertFalse(record["sink_verification"]["complete"], msg=msg)
                        self.assertEqual(capture.sink, b"", msg=msg)
                        sink = record["channels"]["sink"]
                        self.assertEqual(sink["observed_bytes"], len(fragment), msg=msg)
                        self.assertEqual(sink["accepted_bytes"], len(fragment), msg=msg)
                        self.assertEqual(sink["accepted_sha256"], hashlib.sha256(fragment).hexdigest(),
                                         msg=msg)
                        self.assertTrue(sink["source_total_known"], msg=msg)
                        self.assertFalse(sink["complete"], msg=msg)
                        if capture.report:
                            self.assertEqual(capture.report.count(b"\n"), 1, msg=msg)
                            self.assertIn("error", json.loads(capture.report), msg=msg)

    def test_t17_candidate_while_native_sink_open_is_protocol_refusal(self):
        capture = launch(
            {"inherited_sink": True, "closes": [9, 14, 11],
             "candidate_before_sink_close": True, "hold_sink_open_ms": 10000},
            sink=True, options=("--channel-wall-ms", "3000", "--report-max-bytes", "65536"))
        msg = capture.failure_context()
        record = self.refusal(capture, "report_channel_failed", {
            "channel": "candidate", "reason": "protocol"},
            exit_class="transport_failure", status=1, msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        candidate = record["channels"]["candidate"]
        self.assertEqual(candidate["observed_bytes"], 1, msg=msg)
        self.assertEqual(candidate["accepted_bytes"], 0, msg=msg)
        self.assertFalse(candidate["source_total_known"], msg=msg)
        self.assertTrue(candidate["truncated"], msg=msg)
        self.assertLessEqual(record["report_charged_bytes"], 65536, msg=msg)

    def assert_jailed_sink_delivery(self, capture, request, expected, bound, hold):
        msg = capture.failure_context()
        arguments = capture.finalizer_arguments()
        self.assertEqual(arguments[arguments.index(b"--out") + 1], b"-", msg=msg)
        record = self.assert_fixture_delivery(capture, msg=msg)
        native = sink_wire(request["closes"], request.get("first_ts", 1000))
        receipt = sink_receipt(native, complete=True, equity_count=len(request["closes"]))
        self.assertEqual(capture.report, expected.stdout, msg=msg)
        self.assertEqual(capture.sink, native, msg=msg)
        self.assertEqual(capture.diagnostic, b"", msg=msg)
        self.assertEqual(capture.finalizer_stdout(), native, msg=msg)
        self.assertEqual(capture.finalizer_receipt(), receipt, msg=msg)
        self.assertEqual([event[1:] for event in capture.validator_events if event[:1] == b"F"],
                         [], msg=f"jailed V attempted file creation; {msg}")
        self.assertEqual([event[1:] for event in capture.validator_events if event[:1] == b"Q"],
                         [b"jailed"], msg=msg)
        for flag, value in ((b"--out", b"-"), (b"--in-fd", b"0"),
                            (b"--max-bytes", str(bound).encode()), (b"--receipt-fd", b"2")):
            self.assertEqual(arguments[arguments.index(flag) + 1], value, msg=msg)
        self.assertEqual(record["limits"]["sink_bytes"], bound, msg=msg)
        self.assertEqual(record["report_charged_bytes"], len(native) + len(expected.stdout), msg=msg)
        cell = record["channels"]["sink"]
        for name in ("observed_bytes", "accepted_bytes", "written_bytes"):
            self.assertEqual(cell[name], len(native), msg=msg)
        self.assertEqual(cell["accepted_sha256"], hashlib.sha256(native).hexdigest(), msg=msg)
        self.assertEqual(cell["sha256"], hashlib.sha256(native).hexdigest(), msg=msg)
        logical_hash = hashlib.sha256(b"".join(native.splitlines(keepends=True)[:-1])).hexdigest()
        self.assertEqual(cell["logical_sha256"], logical_hash, msg=msg)
        self.assertEqual(record["sink_verification"]["sha256"], logical_hash, msg=msg)
        self.assertEqual(record["sink_verification"]["exit_status"], 0, msg=msg)
        self.assertTrue(record["sink_verification"]["complete"], msg=msg)
        self.assertIn("receipt_hold", capture.control, msg=msg)
        gate = capture.control["receipt_hold"]
        self.assertEqual(gate["validator_before_release"]["state"], "T", msg=msg)
        self.assertGreaterEqual(gate["released_at"] - gate["stopped_at"], 0.2, msg=msg)
        self.assertEqual(gate["report_bytes_while_stopped"], 0, msg=msg)
        self.assertEqual(gate["sink_bytes_while_stopped"], 0, msg=msg)
        self.assertEqual(gate["report_queued_while_stopped"], 0, msg=msg)
        self.assertEqual(gate["sink_queued_while_stopped"], 0, msg=msg)
        self.assertLessEqual(gate["release_checked_at"], gate["released_at"], msg=msg)
        self.assertEqual(gate["receipt_bytes_while_stopped"],
                         0 if hold == "before" else len(receipt), msg=msg)
        self.assertEqual(len(set(gate["stdio"].values())), 3, msg=msg)
        self.assertTrue(all(value.startswith("pipe:[") for value in gate["stdio"].values()),
                        msg=msg)
        for name in ("report", "sink"):
            observation = capture.control["first_publication"][name]
            self.assertGreaterEqual(observation["at"], gate["released_at"], msg=msg)
            self.assertIsNone(observation["validator"], msg=msg)
        return record

    def test_jailed_stdout_route_has_no_file_and_waits_for_receipt_and_exit(self):
        with tempfile.TemporaryDirectory(prefix="pf-jailed-finalizer-") as directory, \
                validator_observer() as observer:
            main_supervisor = supervisor_fixture(directory, main_finalizer=True)
            for script in (None, main_supervisor):
                for hold in ("before", "after"):
                    for environment, arguments in (
                            ({"PINEFORGE_RUN_JSON_PROFILE": "jailed"}, ()),
                            ({"PINEFORGE_RUN_JSON_PROFILE": "unjailed"}, ("--profile", "jailed")),
                            ({"PINEFORGE_RUN_JSON_PROFILE": "invalid"}, ("--profile=jailed",))):
                        with self.subTest(script=script, hold=hold, environment=environment,
                                          arguments=arguments):
                            request = {"closes": [7, 13, 9, 18], "first_ts": 73000,
                                       "fragment_bytes": 17, "inherited_sink": True}
                            expected = reference(request)
                            self.assertEqual(expected.returncode, 0, msg=repr(expected.stderr))
                            native = sink_wire(request["closes"], request["first_ts"])
                            capture = launch(
                                request, sink=True, environment=environment,
                                producer_arguments=arguments, planned_sink_bytes=len(native),
                                observer=observer, audit_stdout=True, receipt_hold=hold,
                                supervisor_path=script)
                            self.assert_jailed_sink_delivery(
                                capture, request, expected, len(native), hold)

    def test_jailed_unlimited_sink_refuses_before_any_producer_marker(self):
        with tempfile.TemporaryDirectory(prefix="pf-jailed-admission-") as directory, \
                validator_observer() as observer:
            marker = Path(directory) / "producer.json"
            for options, grants, plan in (
                    ((), {}, None), (("--report-max-bytes", "0"), {}, None),
                    (("--planned-sink-bytes", "0"), {"report_bytes": 0}, None),
                    ((), {}, 0)):
                with self.subTest(options=options, grants=grants, plan=plan):
                    capture = launch(
                        {"producer_identity_path": str(marker),
                         "stdout": encoded(b"producer must not start")},
                        sink=True, observer=observer, grants=grants, options=options,
                        planned_sink_bytes=plan,
                        environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed"})
                    msg = capture.failure_context()
                    self.assertFalse(marker.exists(), msg=msg)
                    self.assertIsNone(capture.terminal()["producer"]["stage"], msg=msg)
                    record = self.refusal(
                        capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                    self.assertEqual(capture.diagnostic, b"", msg=msg)
                    self.assertIsNone(record["producer"]["stage"], msg=msg)
                    self.assertIsNone(record["limits"]["report_bytes"]["effective"], msg=msg)
                    self.assertIn("--planned-sink-bytes", " ".join(record["diagnostics"]), msg=msg)
                    self.assertEqual(capture.validator_events, [], msg=msg)
                    self.assertEqual(capture.sink, b"", msg=msg)

    def test_jailed_missing_plan_derives_actual_validator_cap_from_positive_report(self):
        request = {"closes": [2, 5, 4], "inherited_sink": True}
        expected = reference(request)
        native = sink_wire(request["closes"])
        allowance = len(native) + len(expected.stdout)
        with validator_observer() as observer:
            for options, environment, grants in (
                    (("--report-max-bytes", str(allowance)), {}, {}),
                    (("--planned-sink-bytes", "0"), {"PINEFORGE_REPORT_MAX_BYTES": str(allowance)}, {}),
                    ((), {}, {"report_bytes": allowance})):
                with self.subTest(options=options, environment=environment, grants=grants):
                    capture = launch(
                        request, sink=True, observer=observer, options=options, grants=grants,
                        environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed", **environment},
                        audit_stdout=True, receipt_hold="after")
                    record = self.assert_jailed_sink_delivery(
                        capture, request, expected, allowance, "after")
                    self.assertEqual(record["limits"]["report_bytes"]["effective"], allowance,
                                     msg=capture.failure_context())

    def test_jailed_planned_sink_cli_and_environment_supply_the_bound(self):
        request = {"closes": [11, 16, 12], "inherited_sink": True}
        expected = reference(request)
        bound = len(sink_wire(request["closes"]))
        with validator_observer() as observer:
            for options, environment in (
                    (("--planned-sink-bytes", str(bound)), {}),
                    ((), {"PINEFORGE_REPORT_PLANNED_SINK_BYTES": str(bound)}),
                    (("--planned-sink-bytes", str(bound)),
                     {"PINEFORGE_REPORT_PLANNED_SINK_BYTES": "invalid"})):
                with self.subTest(options=options, environment=environment):
                    capture = launch(
                        request, sink=True, observer=observer, options=options,
                        environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed", **environment},
                        audit_stdout=True, receipt_hold="before")
                    record = self.assert_jailed_sink_delivery(
                        capture, request, expected, bound, "before")
                    self.assertIsNone(record["limits"]["report_bytes"]["effective"],
                                      msg=capture.failure_context())

    def test_jailed_overflow_receipt_discards_provisional_stdout(self):
        request = {"closes": [19, 22, 20], "inherited_sink": True}
        native = sink_wire(request["closes"])
        bound = len(native) - 1
        with tempfile.TemporaryDirectory(prefix="pf-jailed-overflow-") as directory, \
                validator_observer() as observer:
            main_supervisor = supervisor_fixture(directory, main_finalizer=True)
            for script in (None, main_supervisor):
                with self.subTest(script=script):
                    capture = launch(
                        request, sink=True, planned_sink_bytes=bound, observer=observer,
                        audit_stdout=True, supervisor_path=script,
                        environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed"})
                    msg = capture.failure_context()
                    record = self.refusal(capture, "report_sink_failed", {
                        "reason": "sink_exhausted", "sink_bytes": bound + 1, "cap_bytes": bound},
                        exit_class="transport_failure", status=1, msg=msg)
                    self.assertEqual(record["sink_verification"]["exit_status"], 8, msg=msg)
                    self.assertFalse(record["sink_verification"]["complete"], msg=msg)
                    self.assertEqual(capture.finalizer_receipt(), sink_receipt(
                        native, complete=False, cap=bound, equity_count=len(request["closes"])),
                        msg=msg)
                    self.assertTrue(capture.finalizer_stdout(), msg=msg)
                    self.assertTrue(native.startswith(capture.finalizer_stdout()), msg=msg)
                    self.assertLessEqual(len(capture.finalizer_stdout()), bound, msg=msg)
                    self.assertEqual(capture.sink, b"", msg=msg)
                    self.assertEqual(record["channels"]["sink"]["written_bytes"], 0, msg=msg)
                    self.assertEqual(record["channels"]["sink"]["accepted_bytes"], bound, msg=msg)
                    self.assertEqual(record["channels"]["sink"]["observed_bytes"], bound + 1, msg=msg)
                    self.assertEqual(record["channels"]["sink"]["accepted_sha256"],
                                     hashlib.sha256(native[:bound]).hexdigest(), msg=msg)
                    self.assertEqual([event for event in capture.validator_events
                                      if event[:1] == b"F"], [], msg=msg)

    def test_jailed_complete_receipt_followed_by_nonzero_exit_discards_stdout(self):
        request = {"closes": [8, 15, 10], "inherited_sink": True}
        native = sink_wire(request["closes"])
        capture = launch(
            request, sink=True, planned_sink_bytes=len(native), audit_stdout=True,
            environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed",
                         "PF_REPORT_TEST_FAIL_AFTER_RECEIPT": "1"})
        msg = capture.failure_context()
        record = self.refusal(capture, "report_channel_failed", {
            "channel": "sink", "reason": "protocol"}, exit_class="transport_failure", status=1, msg=msg)
        self.assertEqual(capture.finalizer_stdout(), native, msg=msg)
        self.assertTrue(json.loads(capture.finalizer_receipt())["complete"], msg=msg)
        self.assertEqual(record["sink_verification"]["exit_status"], 5, msg=msg)
        self.assertFalse(record["sink_verification"]["complete"], msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)

    def test_jailed_buffer_hash_is_cross_checked_against_independent_native_bytes(self):
        request = {"closes": [4, 9, 6], "inherited_sink": True}
        native = sink_wire(request["closes"])
        capture = launch(
            request, sink=True, planned_sink_bytes=len(native), audit_stdout=True,
            environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed",
                         "PF_REPORT_TEST_CORRUPT_STDOUT": "1"})
        msg = capture.failure_context()
        record = self.refusal(capture, "report_channel_failed", {
            "channel": "sink", "reason": "protocol"}, exit_class="transport_failure", status=1, msg=msg)
        self.assertEqual(record["sink_verification"]["exit_status"], 0, msg=msg)
        self.assertEqual(capture.finalizer_receipt(),
                         sink_receipt(native, complete=True, equity_count=3), msg=msg)
        self.assertEqual(len(capture.finalizer_stdout()), len(native), msg=msg)
        self.assertNotEqual(capture.finalizer_stdout(), native, msg=msg)
        self.assertEqual(record["channels"]["sink"]["accepted_sha256"],
                         hashlib.sha256(native).hexdigest(), msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        self.assertFalse(record["sink_verification"]["complete"], msg=msg)

    def test_jailed_validator_stdout_cannot_extend_the_buffer_beyond_bound(self):
        request = {"closes": [3, 8, 7], "inherited_sink": True}
        native = sink_wire(request["closes"])
        capture = launch(
            request, sink=True, planned_sink_bytes=len(native), audit_stdout=True,
            environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed",
                         "PF_REPORT_TEST_EXCESS_STDOUT": "1"})
        msg = capture.failure_context()
        record = self.refusal(capture, "report_channel_failed", {
            "channel": "sink", "reason": "protocol"}, exit_class="transport_failure", status=1, msg=msg)
        self.assertEqual(capture.finalizer_stdout(), native + b"x", msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        self.assertEqual(record["channels"]["sink"]["accepted_bytes"], len(native), msg=msg)
        self.assertEqual(record["channels"]["sink"]["written_bytes"], 0, msg=msg)

    def test_validator_environment_is_explicit_and_excludes_supervisor_variable(self):
        request = {"closes": [6, 12, 8], "inherited_sink": True}
        expected = reference(request)
        native = sink_wire(request["closes"])
        capture = launch(
            request, sink=True, planned_sink_bytes=len(native), audit_stdout=True,
            receipt_hold="after", environment={
                "PINEFORGE_RUN_JSON_PROFILE": "jailed",
                "PF_REPORT_UNLISTED_MARKER": "present in S only",
                "PYTHONPATH": "/a-test-path-that-must-not-reach-the-validator"})
        self.assert_jailed_sink_delivery(capture, request, expected, len(native), "after")
        msg = capture.failure_context()
        allowed = {"PINEFORGE_RUN_JSON_PROFILE", "LD_PRELOAD", "PF_REPORT_TEST_TRACE",
                   "PF_REPORT_TEST_STDOUT_AUDIT", "PF_REPORT_TEST_HOLD_RECEIPT", "LC_CTYPE"}
        names = capture.finalizer_environment_names()
        self.assertTrue(names <= allowed, msg=msg)
        self.assertIn("PINEFORGE_RUN_JSON_PROFILE", names, msg=msg)
        gate = capture.control["receipt_hold"]
        self.assertIn("PF_REPORT_UNLISTED_MARKER", gate["supervisor_environment_names"], msg=msg)
        self.assertIn("PYTHONPATH", gate["supervisor_environment_names"], msg=msg)
        self.assertNotIn("PF_REPORT_UNLISTED_MARKER", names, msg=msg)
        self.assertNotIn("PYTHONPATH", names, msg=msg)
        self.assertTrue(set(gate["environment_names"]) <= allowed, msg=msg)

    def test_effective_profile_cli_overrides_environment_for_unjailed_file_route(self):
        request = {"closes": [9, 14, 10], "inherited_sink": True}
        expected = reference(request)
        native = sink_wire(request["closes"])
        with validator_observer() as observer:
            for environment, arguments in (
                    ({"PINEFORGE_RUN_JSON_PROFILE": "jailed"}, ("--profile", "unjailed")),
                    ({"PINEFORGE_RUN_JSON_PROFILE": "invalid"}, ("--profile=unjailed",))):
                with self.subTest(environment=environment, arguments=arguments):
                    capture = launch(
                        request, sink=True, observer=observer, environment=environment,
                        producer_arguments=arguments)
                    msg = capture.failure_context()
                    self.assertEqual([event[1:] for event in capture.validator_events
                                      if event[:1] == b"Q"], [b"unjailed"], msg=msg)
                    self.assert_fixture_delivery(capture, msg=msg)
                    self.assertEqual(capture.report, expected.stdout, msg=msg)
                    self.assertEqual(capture.sink, native, msg=msg)
                    argv = capture.finalizer_arguments()
                    self.assertNotIn(b"--max-bytes", argv, msg=msg)
                    self.assertNotEqual(argv[argv.index(b"--out") + 1], b"-", msg=msg)
                    self.assertTrue(any(event[:1] == b"F" for event in capture.validator_events),
                                    msg=msg)

    def test_ambiguous_or_invalid_effective_profile_refuses_before_launch(self):
        with tempfile.TemporaryDirectory(prefix="pf-profile-admission-") as directory:
            marker = Path(directory) / "producer.json"
            for environment, arguments, named in (
                    ({}, ("--profile", "jailed", "--profile=unjailed"), "ambiguous"),
                    ({}, ("--prof", "jailed"), "ambiguous"),
                    ({}, ("--profile",), "ambiguous"),
                    ({}, ("--profile=other",), "--profile"),
                    ({"PINEFORGE_RUN_JSON_PROFILE": "other"}, (), "PINEFORGE_RUN_JSON_PROFILE")):
                with self.subTest(environment=environment, arguments=arguments):
                    capture = launch(
                        {"producer_identity_path": str(marker)}, sink=True,
                        environment=environment, producer_arguments=arguments)
                    msg = capture.failure_context()
                    self.assertFalse(marker.exists(), msg=msg)
                    self.assertIsNone(capture.terminal()["producer"]["stage"], msg=msg)
                    record = self.refusal(
                        capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
                    self.assertIn(named, " ".join(record["diagnostics"]), msg=msg)
                    self.assertFalse(marker.exists(), msg=msg)
                    self.assertIsNone(record["producer"]["stage"], msg=msg)
                    self.assertEqual(capture.validator_events, [], msg=msg)

    def test_main_finalizer_jailed_file_refusal_before_target_creation(self):
        with tempfile.TemporaryDirectory(prefix="pf-main-finalizer-contract-") as directory:
            supervisor = supervisor_fixture(directory, main_finalizer=True)
            output = Path(directory) / "output"
            output.mkdir()
            target = output / "verified.sink"
            finalizer = supervisor.with_name("pineforge_sink_finalize.py")
            result = subprocess.run(
                [sys.executable, "-B", str(finalizer), "--in-fd", "0", "--out", str(target)],
                input=sink_wire([5, 8, 6]), capture_output=True, check=False, timeout=10,
                env={"PINEFORGE_RUN_JSON_PROFILE": "jailed"})
            context = repr((result.returncode, result.stdout, result.stderr))
            self.assertEqual(result.returncode, 3, msg=context)
            self.assertIn(b"finalizer_file_output_no_jailed_route", result.stderr, msg=context)
            self.assertFalse(json.loads(result.stdout)["complete"], msg=context)
            self.assertEqual(list(output.iterdir()), [], msg=context)

    def test_jailed_named_sink_route_remains_a_descriptor_refusal(self):
        capture = launch(
            options=("--report-sink", "full-projection.sink"),
            environment={"PINEFORGE_RUN_JSON_PROFILE": "jailed"})
        msg = capture.failure_context()
        record = self.refusal(
            capture, "run_isolation_unavailable", {"reason": "descriptor"},
            exit_class="resource_refusal", status=1, msg=msg)
        self.assertIsNone(record["producer"]["stage"], msg=msg)
        self.assertEqual(capture.validator_events, [], msg=msg)

    def test_t12_unlimited_file_finalizer_omits_cap(self):
        request = {"closes": [8, 13, 11, 17], "inherited_sink": True}
        native = sink_wire(request["closes"])
        expected = reference(request)
        zero_options = tuple(part for flag, _, _, _ in KNOBS for part in (flag, "0"))
        with validator_observer() as observer:
            for grants, options, plan in (
                    ({}, (), None),
                    ({field: 0 for _, _, field, _ in KNOBS}, zero_options, None),
                    ({}, (), len(native))):
                with self.subTest(grants=grants, options=options, plan=plan):
                    capture = launch(
                        request, sink=True, grants=grants, options=options,
                        planned_sink_bytes=plan, observer=observer)
                    msg = capture.failure_context()
                    record = self.assert_fixture_delivery(capture, msg=msg)
                    self.assertEqual(expected.returncode, 0, msg=msg)
                    self.assertEqual(capture.report, expected.stdout, msg=msg)
                    self.assertEqual(capture.sink, native, msg=msg)
                    self.assertEqual(record["report_charged_bytes"],
                                     len(native) + len(expected.stdout), msg=msg)
                    self.assertIsNone(record["limits"]["report_bytes"]["effective"], msg=msg)
                    self.assertIsNone(record["limits"]["sink_bytes"], msg=msg)
                    arguments = capture.finalizer_arguments()
                    self.assertNotIn(b"--max-bytes", arguments, msg=msg)
                    self.assertNotEqual(arguments[arguments.index(b"--out") + 1], b"-", msg=msg)
                    self.assertEqual([event[1:] for event in capture.validator_events
                                      if event[:1] == b"Q"], [b"unjailed"], msg=msg)
                    self.assertTrue(any(event[:1] == b"F" for event in capture.validator_events),
                                    msg=f"the file observer must detect the real spool; {msg}")
                    self.assertFalse(any(event[:1] == b"P" for event in capture.validator_events),
                                     msg=msg)
                    self.assertEqual(capture.finalizer_receipt(),
                                     sink_receipt(native, complete=True,
                                                  equity_count=len(request["closes"])), msg=msg)
                    self.assertEqual(record["sink_verification"]["exit_status"], 0, msg=msg)
                    self.assertTrue(record["sink_verification"]["complete"], msg=msg)
                    sink = record["channels"]["sink"]
                    self.assertIsNone(sink["limit_bytes"], msg=msg)
                    for key in ("observed_bytes", "accepted_bytes", "written_bytes"):
                        self.assertEqual(sink[key], len(native), msg=msg)
                    self.assertEqual(sink["accepted_sha256"], hashlib.sha256(native).hexdigest(),
                                     msg=msg)
                    self.assertEqual(sink["sha256"], hashlib.sha256(native).hexdigest(), msg=msg)
                    self.assertIsNone(record["channels"]["candidate"]["limit_bytes"], msg=msg)

    def test_t17_runner_startup_failure_closes_private_sink_before_candidate(self):
        argv = ["--an-unrecognized-runner-option"]
        baseline = subprocess.run(
            [sys.executable, "-B", str(ROOT / "docker/run_json.py"), *argv],
            capture_output=True, env=clean_environment(), check=False, timeout=10)
        capture = launch(direct=True, command=argv, sink=True)
        msg = capture.failure_context()
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1, msg=msg)
        self.assertEqual(baseline.returncode, 2, msg=msg)
        self.assertEqual(capture.report, baseline.stdout, msg=msg)
        self.assertEqual(capture.sink, b"", msg=msg)
        self.assertEqual(record["producer"]["exit_status"], 2, msg=msg)
        self.assertEqual(record["sink_verification"]["exit_status"], 6, msg=msg)
        self.assertEqual(record["channels"]["sink"]["observed_bytes"], 0, msg=msg)
        self.assertTrue(record["channels"]["sink"]["source_total_known"], msg=msg)
        self.assertTrue(record["channels"]["candidate"]["complete"], msg=msg)

    def test_t17_empty_and_partial_candidates_fail_only_after_wait(self):
        for candidate in (b"", b'{"engine":"pineforge"', b'{"engine":"pineforge"}'):
            for exit_status in (0, 7):
                with self.subTest(candidate=candidate, exit_status=exit_status):
                    capture = launch({"candidate": encoded(candidate), "exit_status": exit_status,
                                      "fragment_bytes": 1})
                    msg = capture.failure_context()
                    if exit_status:
                        self.refusal(capture, "run_producer_failed",
                                     {"reason": "exit", "exit_status": 7},
                                     exit_class="producer_failure", status=1, msg=msg)
                    else:
                        self.refusal(capture, "report_channel_failed",
                                     {"channel": "candidate", "reason": "protocol"},
                                     exit_class="transport_failure", status=1, msg=msg)
                    self.assertNotEqual(capture.report, candidate, msg=msg)
                    if capture.report:
                        self.assertEqual(capture.report.count(b"\n"), 1, msg=msg)

    def test_t25_candidate_framing_and_shape(self):
        bad = (
            b'{}\n{}\n', b'{"engine":"pineforge","engine":"pineforge"}\n',
            b'{"engine":"pineforge","error":"\xff"}\n',
            b'{"engine":"pineforge","error":"x","extra":NaN}\n',
            b'{"engine":"pineforge","error":"x","extra":Infinity}\n',
            b'{"engine":"pineforge","error":"x","extra":1e999}\n',
            b'[]\n', b'{}\n', b'{"engine":"pineforge","summary":[]}\n',
            b'{"engine":"pineforge","error":"x"}\n\n',
            b'{"engine":"pineforge","error":"x"}\nextra',
            b'{"engine":"pineforge","error":"x","args":{}}\n',
            b'{"engine":"pineforge","error":"x","extra":0}\n',
            b'{"engine":"pineforge","error":"x","code":"window_lineage_mismatch",'
            b'"args":{"actual":0,"expected":1,"field":"a","field":"b"}}\n',
            b'{"engine":"pineforge","input":{},"summary":{},"applied_inputs":{},'
            b'"applied_overrides":{},"applied_runtime":{},"diagnostics":{},'
            b'"metrics":{},"trades":[],"equity_curve":[],"elapsed_seconds":0}\n',
        )
        for candidate in bad:
            with self.subTest(candidate=candidate):
                capture = launch({"candidate": encoded(candidate), "fragment_bytes": 1})
                msg = capture.failure_context()
                self.refusal(capture, "report_channel_failed",
                             {"channel": "candidate", "reason": "protocol"},
                             exit_class="transport_failure", status=1, msg=msg)
                self.assertNotEqual(capture.report, candidate, msg=msg)

    def test_strategy_create_series_limit_cause_is_relayed_intact_with_producer_authority(self):
        with tempfile.TemporaryDirectory(prefix="pf-series-cause-catalog-") as directory:
            supervisor = supervisor_fixture(directory, main_catalog=True)
            catalog = json.loads(supervisor.with_name("run_failure_codes.json").read_bytes())
            self.assertIn("too_many_series", catalog["codes"]["outputs_limit"]["args"]["reason"]["values"])
            for maximum, exit_status, error in (
                    (1, 0, "series capacity"), (37, 1, "series capacity"),
                    (9007199254740991, 1, "large description " + "z" * 9000)):
                with self.subTest(maximum=maximum, exit_status=exit_status):
                    cause = {"code": "outputs_limit",
                             "args": {"max": maximum, "reason": "too_many_series"}}
                    candidate = json.dumps({
                        "engine": "pineforge", "error": error, "cause": cause,
                        "code": "strategy_create_failed", "args": {}},
                        separators=(", ", ": "), ensure_ascii=True).encode() + b"\n"
                    capture = launch(
                        {"candidate": encoded(candidate), "exit_status": exit_status,
                         "fragment_bytes": 19}, supervisor_path=supervisor)
                    msg = capture.failure_context()
                    # This must distinguish relay from a replacement refusal.
                    self.assertEqual(capture.report, candidate, msg=msg)
                    record = self.refusal(
                        capture, "run_producer_failed", {"reason": "claim"},
                        exit_class="producer_failure", status=1, msg=msg)
                    self.assertEqual(record["producer"]["failure"], {
                        "code": "strategy_create_failed", "args": {},
                        "cause": cause, "authority": "producer"}, msg=msg)
                    self.assertEqual(record["producer"]["exit_status"], exit_status, msg=msg)
                    self.assertFalse(record["producer"]["failure_claim_omitted"], msg=msg)
                    self.assertLessEqual(len(json.dumps(
                        record["producer"]["failure"], separators=(",", ":")).encode()), 8192, msg=msg)
                    self.assertEqual(record["channels"]["candidate"]["accepted_bytes"],
                                     len(candidate), msg=msg)
                    self.assertEqual(record["channels"]["candidate"]["sha256"],
                                     hashlib.sha256(candidate).hexdigest(), msg=msg)

    def test_strategy_create_cause_wrong_shapes_remain_protocol_refusals(self):
        valid = {"code": "outputs_limit", "args": {"max": 7, "reason": "too_many_series"}}
        variants = [
            ("harness_internal_error", {}, valid),
            ("strategy_create_failed", {}, {**valid, "extra": 1}),
            ("strategy_create_failed", {}, {"code": "not_an_installed_code", "args": valid["args"]}),
            ("strategy_create_failed", {}, {"code": "run_aborted", "args": {}}),
            ("strategy_create_failed", {}, None),
            ("strategy_create_failed", {}, []),
            ("strategy_create_failed", {}, "outputs_limit"),
            ("strategy_create_failed", {}, {"code": "outputs_limit"}),
            ("strategy_create_failed", {}, {"args": valid["args"]}),
            ("strategy_create_failed", {}, {"code": 1, "args": valid["args"]}),
            ("strategy_create_failed", {}, {"code": True, "args": valid["args"]}),
            ("strategy_create_failed", {}, {"code": "outputs_limit", "args": []}),
            ("strategy_create_failed", {}, {"code": "outputs_limit", "args": "arguments"}),
            ("strategy_create_failed", {}, {"code": "outputs_limit", "args": None}),
            ("strategy_create_failed", {}, {"code": "outputs_limit", "args": {"max": 7}}),
            ("strategy_create_failed", {}, {"code": "outputs_limit",
                                            "args": {**valid["args"], "extra": 1}}),
            ("strategy_create_failed", {}, {"code": "outputs_limit",
                                            "args": {"max": 7, "reason": "too_many_events"}}),
            ("strategy_create_failed", {}, {"code": "outputs_limit",
                                            "args": {"max": 7, "reason": "too_many_rows"}}),
            ("strategy_create_failed", {}, {"code": "outputs_limit",
                                            "args": {"max": 7, "reason": 1}}),
            ("strategy_create_failed", {"unexpected": 1}, valid),
        ]
        for maximum in (0, -1, True, 1.0, "1", None, [], {}, 9007199254740992):
            variants.append(("strategy_create_failed", {}, {
                "code": "outputs_limit", "args": {"max": maximum, "reason": "too_many_series"}}))
        with tempfile.TemporaryDirectory(prefix="pf-series-cause-refusals-") as directory:
            supervisor = supervisor_fixture(directory, main_catalog=True)
            candidates = [
                json.dumps({"engine": "pineforge", "error": "creation refused",
                            "code": code, "args": args, "cause": cause},
                           separators=(",", ":")).encode() + b"\n"
                for code, args, cause in variants]
            candidates.extend((
                b'{"engine":"pineforge","error":"legacy","cause":'
                b'{"code":"outputs_limit","args":{"max":1,"reason":"too_many_series"}}}\n',
                b'{"engine":"pineforge","error":"duplicate","code":"strategy_create_failed","args":{},'
                b'"cause":{"code":"outputs_limit","code":"outputs_limit",'
                b'"args":{"max":1,"reason":"too_many_series"}}}\n',
                b'{"engine":"pineforge","error":"duplicate","code":"strategy_create_failed","args":{},'
                b'"cause":{"code":"outputs_limit","args":'
                b'{"max":1,"max":2,"reason":"too_many_series"}}}\n',
                b'{"engine":"pineforge","cause":'
                b'{"code":"outputs_limit","args":{"max":1,"reason":"too_many_series"}}}\n',
            ))
            for candidate in candidates:
                with self.subTest(candidate=candidate):
                    capture = launch(
                        {"candidate": encoded(candidate), "exit_status": 1, "fragment_bytes": 7},
                        supervisor_path=supervisor)
                    msg = capture.failure_context()
                    record = self.refusal(capture, "report_channel_failed", {
                        "channel": "candidate", "reason": "protocol"},
                        exit_class="transport_failure", status=1, msg=msg)
                    self.assertNotEqual(capture.report, candidate, msg=msg)
                    self.assertIsNone(record["producer"]["failure"], msg=msg)

    def test_failure_claim_size_boundary_is_same_with_and_without_cause(self):
        def wire_size(value):
            return len(json.dumps(
                value, separators=(",", ":"), ensure_ascii=True).encode()) + 1

        with tempfile.TemporaryDirectory(prefix="pf-claim-size-boundary-") as directory:
            supervisor = supervisor_fixture(directory, main_catalog=True)
            catalog_path = supervisor.with_name("run_failure_codes.json")
            catalog = json.loads(catalog_path.read_bytes())
            catalog["codes"]["strategy_create_failed"]["args"] = {
                "message": {"kind": "string"}}
            catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
            args = None
            for length in range(1, CLAIM_BYTES + 1):
                candidate_args = {"message": "x" * length}
                values_size = wire_size(candidate_args)
                proposed_size = wire_size({
                    "code": "strategy_create_failed", "args": candidate_args,
                    "authority": "producer"})
                if values_size - 1 <= 8192 and proposed_size - 1 > 8192:
                    args = candidate_args
                    break
            self.assertIsNotNone(args)
            cause = {"code": "outputs_limit",
                     "args": {"max": 1, "reason": "too_many_series"}}
            for extra in ({}, {"cause": cause}):
                with self.subTest(with_cause=bool(extra)):
                    value = {
                        "engine": "pineforge", "error": "creation refused",
                        "code": "strategy_create_failed", "args": args, **extra}
                    candidate = json.dumps(
                        value, separators=(",", ":"), ensure_ascii=True).encode() + b"\n"
                    capture = launch(
                        {"candidate": encoded(candidate), "exit_status": 1},
                        supervisor_path=supervisor)
                    msg = capture.failure_context()
                    record = self.refusal(
                        capture, "run_producer_failed", {"reason": "claim"},
                        exit_class="producer_failure", status=1, msg=msg)
                    self.assertEqual(capture.report, candidate, msg=msg)
                    self.assertTrue(record["producer"]["failure_claim_omitted"], msg=msg)
                    self.assertIsNone(record["producer"]["failure"], msg=msg)

    def test_t25_legacy_failure_is_relayed_as_a_claim(self):
        candidate = b'{"engine":"pineforge","error":"legacy failure"}\n'
        capture = launch({"candidate": encoded(candidate)})
        msg = capture.failure_context()
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1, msg=msg)
        self.assertEqual(capture.report, candidate, msg=msg)
        self.assertIsNone(record["producer"]["failure"], msg=msg)
        self.assertFalse(record["producer"]["failure_claim_omitted"], msg=msg)

    def test_t25_oversized_valid_claim_is_omitted_without_empty_args(self):
        candidate = json.dumps({
            "engine": "pineforge", "error": "large claim", "code": "window_lineage_mismatch",
            "args": {"field": "z" * 9000, "expected": 1, "actual": 2}},
            separators=(",", ":")).encode() + b"\n"
        capture = launch({"candidate": encoded(candidate), "exit_status": 1})
        msg = capture.failure_context()
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1, msg=msg)
        self.assertEqual(capture.report, candidate, msg=msg)
        self.assertIsNone(record["producer"]["failure"], msg=msg)
        self.assertTrue(record["producer"]["failure_claim_omitted"], msg=msg)

    def test_t31_positive_host_grants_are_optional_constraints(self):
        grants = {"report_bytes": 4096, "diagnostic_bytes": 64,
                  "wall_ms": 10000, "stall_ms": 2000}
        for flag, _, field, _ in KNOBS:
            for value in (None, 0, grants[field] - 1, grants[field]):
                with self.subTest(flag=flag, value=value):
                    capture = launch(grants=grants, options=() if value is None else (flag, str(value)))
                    msg = capture.failure_context()
                    record = self.assert_fixture_delivery(capture, msg=msg)
                    for _, _, name, _ in KNOBS:
                        requested = value if name == field else None
                        self.assertEqual(record["limits"][name], {
                            "requested": requested, "granted": grants[name],
                            "effective": requested or grants[name]}, msg=msg)
            capture = launch(grants=grants, options=(flag, str(grants[field] + 1)))
            msg = capture.failure_context()
            record = self.refusal(capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
            self.assertIsNone(record["producer"]["stage"], msg=msg)
            self.assertEqual(record["limits"][field], {
                "requested": grants[field] + 1, "granted": grants[field], "effective": None},
                msg=msg)

    def test_t31_unlimited_grants_accept_exact_positive_requests(self):
        for grants in ({}, {field: 0 for _, _, field, _ in KNOBS},
                       {"report_bytes": 8192, "diagnostic_bytes": 0}):
            for flag, environment, field, value in KNOBS:
                for route in ("cli", "environment"):
                    with self.subTest(grants=grants, flag=flag, route=route):
                        capture = launch(
                            grants=grants, options=(flag, str(value)) if route == "cli" else (),
                            environment={environment: str(value)} if route == "environment" else {})
                        msg = capture.failure_context()
                        record = self.assert_fixture_delivery(capture, msg=msg)
                        for _, _, name, _ in KNOBS:
                            requested = value if name == field else None
                            self.assertEqual(record["limits"][name], {
                                "requested": requested, "granted": grants.get(name),
                                "effective": requested or grants.get(name) or None}, msg=msg)

    def test_t31_zero_environment_is_unlimited(self):
        capture = launch(environment={environment: "0" for _, environment, _, _ in KNOBS})
        msg = capture.failure_context()
        record = self.assert_fixture_delivery(capture, msg=msg)
        for _, _, field, _ in KNOBS:
            self.assertEqual(record["limits"][field], {
                "requested": 0, "granted": None, "effective": None}, msg=msg)

    def test_t31_unlimited_stall_survives_delayed_diagnostic_reader(self):
        diagnostic = bytes(range(256)) * 272 + b"z"
        request = {"stderr": encoded(diagnostic)}
        expected = reference(request)
        zero_options = tuple(part for flag, _, _, _ in KNOBS for part in (flag, "0"))
        for options in ((), zero_options):
            with self.subTest(options=options):
                # The host constrains its D pipe to 4096 bytes. U can exit
                # with buffered input still behind S's fixed pending queue.
                capture = launch(request, options=options, diagnostic_delay_ms=6000)
                msg = capture.failure_context()
                record = self.assert_fixture_delivery(capture, msg=msg)
                self.assertGreaterEqual(capture.elapsed_seconds, 6, msg=msg)
                self.assertEqual(expected.returncode, 0, msg=msg)
                self.assertEqual(capture.report, expected.stdout, msg=msg)
                self.assertEqual(capture.diagnostic, diagnostic, msg=msg)
                self.assertEqual(expected.stderr, diagnostic, msg=msg)
                for _, _, field, _ in KNOBS:
                    self.assertEqual(record["limits"][field], {
                        "requested": 0 if options else None, "granted": None, "effective": None},
                        msg=msg)
                self.assertEqual(record["channels"]["diagnostic"]["observed_bytes"],
                                 len(diagnostic), msg=msg)

    def test_t31_cli_overrides_environment(self):
        capture = launch(options=("--report-max-bytes", "4096"), environment={
            "PINEFORGE_REPORT_MAX_BYTES": "invalid"})
        msg = capture.failure_context()
        record = self.assert_fixture_delivery(capture, msg=msg)
        self.assertEqual(record["limits"]["report_bytes"]["requested"], 4096, msg=msg)

    def test_t31_maximum_safe_ceiling_and_leading_zero(self):
        grants = {"report_bytes": 9007199254740990}
        for value in ("9007199254740990", "00000000000000000000"):
            with self.subTest(value=value):
                capture = launch(grants=grants, options=("--report-max-bytes", value))
                msg = capture.failure_context()
                record = self.assert_fixture_delivery(capture, msg=msg)
                self.assertEqual(record["limits"]["report_bytes"], {
                    "requested": int(value), "granted": 9007199254740990,
                    "effective": 9007199254740990}, msg=msg)

    def test_t31_strict_decimal_parser(self):
        for flag in ("--report-max-bytes", "--diagnostic-max-bytes",
                     "--channel-wall-ms", "--channel-stall-ms", "--stop-grace-ms"):
            for value in ("", "-1", "+1", " 1", "1 ", "1.0", "1e3", "0x10", "١",
                          "9007199254740991"):
                with self.subTest(flag=flag, value=value):
                    capture = launch(options=(flag + "=" + value,))
                    self.refusal(capture, "run_request_invalid", {"option": "arguments"})

    def test_diagnostic_exact_fit_and_one_probe(self):
        diagnostic = b"a\x00\xffb" + b"tail"
        for allowance in (len(diagnostic), len(diagnostic) - 1):
            with self.subTest(allowance=allowance):
                capture = launch({"stdout": encoded(diagnostic[:4]),
                                  "stderr": encoded(diagnostic[4:])},
                                 options=("--diagnostic-max-bytes", str(allowance)))
                msg = capture.failure_context()
                self.assertEqual(capture.diagnostic, diagnostic[:allowance], msg=msg)
                if allowance == len(diagnostic):
                    self.assert_fixture_delivery(capture, msg=msg)
                else:
                    record = self.refusal(capture, "diagnostic_budget_exceeded", {
                        "cap_bytes": allowance, "used_bytes": allowance, "requested_bytes": 1},
                        exit_class="resource_refusal", status=1, msg=msg)
                    self.assertEqual(record["channels"]["diagnostic"]["observed_bytes"],
                                     allowance + 1, msg=msg)
                    self.assertTrue(record["channels"]["diagnostic"]["truncated"], msg=msg)

    def test_t31_existing_finalizer_zero_semantics(self):
        finalizer = ROOT / "docker/pineforge_sink_finalize.py"
        with tempfile.TemporaryDirectory(prefix="pf-finalizer-zero-") as directory:
            for stream in (False, True):
                with self.subTest(stream=stream):
                    reader, writer = os.pipe()
                    receipt_read, receipt_write = os.pipe()
                    os.write(writer, b"x")
                    os.close(writer)
                    result = subprocess.run(
                        [sys.executable, "-B", str(finalizer), "--in-fd", str(reader),
                         "--out", "-" if stream else str(Path(directory) / "result.sink"),
                         "--max-bytes", "0", "--receipt-fd", str(receipt_write)],
                        pass_fds=(reader, receipt_write), capture_output=True, timeout=10,
                        check=False)
                    os.close(reader)
                    os.close(receipt_write)
                    receipt = os.read(receipt_read, 65536)
                    os.close(receipt_read)
                    msg = json.dumps({
                        "terminal": None, "supervisor_wait_status": None,
                        "validator_exit_status": result.returncode, "receipt_bytes": repr(receipt),
                        "diagnostic_length_bytes": len(result.stderr),
                        "diagnostic_prefix": result.stderr[:4096].decode("utf-8", "replace"),
                    }, separators=(",", ":"))
                    if stream:
                        self.assertEqual(result.returncode, 2, msg=msg)
                        self.assertEqual(receipt, b"", msg=msg)
                    else:
                        self.assertEqual(result.returncode, 8, msg=msg)
                        document = json.loads(receipt)
                        self.assertFalse(document["complete"], msg=msg)
                        self.assertEqual(document["reason"], "sink_exhausted", msg=msg)
                        self.assertEqual(document["sink_bytes"], 1, msg=msg)
                        self.assertEqual(document["cap_bytes"], 0, msg=msg)


if __name__ == "__main__":
    unittest.main(verbosity=2)
