"""Outside-in report-channel contract rows. Run with unittest discovery."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

from host import (
    ROOT, clean_environment, encoded, launch, reference, require_supervisor,
    sink_receipt, sink_wire, validator_observer,
)


class Contract(unittest.TestCase):
    def setUp(self):
        require_supervisor()

    def assert_record(self, capture, *, complete, exit_class, exit_status, msg=None):
        if msg is None:
            msg = capture.failure_context()
        record = capture.terminal()
        self.assertEqual(set(record), {
            "schema", "version", "contract", "attempt_id", "invocation", "complete",
            "exit_class", "failure", "exit_status", "producer", "limits",
            "report_charged_bytes", "channels", "sink_verification", "usage", "cleanup"},
            msg=msg)
        self.assertEqual(record["schema"], "pf-run-terminal", msg=msg)
        self.assertEqual(record["version"], 1, msg=msg)
        self.assertEqual(record["contract"], "supervised/v1", msg=msg)
        self.assertEqual(record["attempt_id"], capture.attempt_id, msg=msg)
        self.assertEqual(record["invocation"], "run_json", msg=msg)
        self.assertIs(record["complete"], complete, msg=msg)
        self.assertEqual(record["exit_class"], exit_class, msg=msg)
        self.assertEqual(record["exit_status"], exit_status, msg=msg)
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
        return record

    def refusal(self, capture, code, args, *, exit_class="input_refusal", status=2, msg=None):
        if msg is None:
            msg = capture.failure_context()
        record = self.assert_record(
            capture, complete=False, exit_class=exit_class, exit_status=status, msg=msg)
        self.assertEqual(record["failure"], {
            "code": code, "args": args, "authority": "supervisor"}, msg=msg)
        return record

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
                    record = self.assert_record(
                        capture, complete=True, exit_class="success", exit_status=0, msg=msg)
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
        self.assertEqual(capture.returncode, 0, msg=(
            f"inherited-sink success requires the round-two K/S/V relay; {msg}"))
        self.assertEqual(capture.report, baseline.stdout, msg=msg)
        self.assertEqual(capture.sink, baseline.sink, msg=msg)
        self.assertEqual(capture.sink, sink_wire(request["closes"]), msg=msg)
        self.assertNotIn("trades", json.loads(capture.report), msg=msg)
        record = self.assert_record(
            capture, complete=True, exit_class="success", exit_status=0, msg=msg)
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
                self.assert_record(capture, complete=True, exit_class="success", exit_status=0,
                                   msg=msg)

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
        record = self.assert_record(capture, complete=True, exit_class="success", exit_status=0,
                                    msg=msg)
        limits = record["limits"]
        for name, expected in (("report_bytes", 67108864), ("diagnostic_bytes", 1048576),
                               ("wall_ms", 600000), ("stall_ms", 5000)):
            self.assertEqual(limits[name], {
                "requested": None, "granted": expected, "effective": expected}, msg=msg)
        self.assertEqual(limits["terminal_bytes"], 65536, msg=msg)
        self.assertEqual(limits["stop_grace_ms"], 1000, msg=msg)
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
        self.assert_record(capture, complete=True, exit_class="success", exit_status=0)
        capture = launch(environment={"PINEFORGE_REPORT_CHANNEL": "supervised/unknown"})
        self.assert_record(capture, complete=True, exit_class="success", exit_status=0)

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

    def test_t2_zero_host_grant_refuses_before_producer(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, grants={
            "report_bytes": 0, "diagnostic_bytes": 1048576,
            "wall_ms": 600000, "stall_ms": 5000})
        msg = capture.failure_context()
        self.refusal(capture, "run_isolation_unavailable", {"reason": "policy"},
                     exit_class="resource_refusal", status=1, msg=msg)
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic, msg=msg)

    def test_t2_new_knobs_without_activation_are_usage_errors(self):
        for name, value in (("--report-max-bytes", "0"), ("--diagnostic-max-bytes", "10"),
                            ("--channel-wall-ms", "10"), ("--channel-stall-ms", "10"),
                            ("--terminal-status-fd", "9")):
            with self.subTest(name=name):
                result = subprocess.run(
                    [sys.executable, "-B", str(ROOT / "docker/run_json.py"), name, value],
                    capture_output=True, env=clean_environment(), timeout=10, check=False)
                msg = json.dumps({
                    "terminal": None, "supervisor_wait_status": None,
                    "runner_wait_status": result.returncode, "report_bytes": repr(result.stdout),
                    "diagnostic_length_bytes": len(result.stderr),
                    "diagnostic_prefix": result.stderr[:4096].decode("utf-8", "replace"),
                }, separators=(",", ":"))
                self.assertEqual(result.returncode, 2, msg=msg)
                self.assertEqual(json.loads(result.stdout)["code"], "run_request_invalid", msg=msg)
                self.assertEqual(json.loads(result.stdout)["args"], {"option": "arguments"}, msg=msg)
        for name in ("PINEFORGE_REPORT_MAX_BYTES", "PINEFORGE_DIAGNOSTIC_MAX_BYTES",
                     "PINEFORGE_CHANNEL_WALL_MS", "PINEFORGE_CHANNEL_STALL_MS",
                     "PINEFORGE_TERMINAL_STATUS_FD"):
            with self.subTest(environment=name):
                environment = clean_environment()
                environment[name] = "0"
                result = subprocess.run(
                    [sys.executable, "-B", str(ROOT / "docker/run_json.py")],
                    capture_output=True, env=environment, timeout=10, check=False)
                msg = json.dumps({
                    "terminal": None, "supervisor_wait_status": None,
                    "runner_wait_status": result.returncode, "report_bytes": repr(result.stdout),
                    "diagnostic_length_bytes": len(result.stderr),
                    "diagnostic_prefix": result.stderr[:4096].decode("utf-8", "replace"),
                }, separators=(",", ":"))
                self.assertEqual(result.returncode, 2, msg=msg)
                self.assertEqual(json.loads(result.stdout)["code"], "run_request_invalid", msg=msg)

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
        self.assertEqual(capture.returncode, 0, msg=capture.failure_context())

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

    def test_t10_fd1_forgery_is_raw_diagnostics(self):
        forgery = b'{"engine":"pineforge","error":"forged","code":"harness_internal_error","args":{}}\n'
        capture = launch({"stdout": encoded(forgery)})
        msg = capture.failure_context()
        self.assertEqual(capture.diagnostic, forgery, msg=msg)
        self.assertNotIn(b"forged", capture.report, msg=msg)
        self.assert_record(capture, complete=True, exit_class="success", exit_status=0, msg=msg)

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
                    self.assert_record(capture, complete=True, exit_class="success", exit_status=0,
                                       msg=msg)
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
                    record = self.assert_record(
                        capture, complete=True, exit_class="success", exit_status=0, msg=msg)
                    self.assertEqual(expected.returncode, 0, msg=msg)
                    self.assertEqual(capture.report, expected.stdout, msg=msg)
                    self.assertEqual(capture.sink, native, msg=msg)
                    self.assertEqual(capture.diagnostic, b"native\x00\xff", msg=msg)
                    self.assertEqual(capture.finalizer_receipt(),
                                     sink_receipt(native, complete=True, equity_count=len(closes)),
                                     msg=msg)
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
                            ("diagnostic_bytes", len(b"native\x00\xff"), 1048576)):
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
                kill_stopped_probe=True,
                options=("--report-max-bytes", "65536", "--channel-wall-ms", "2000",
                         "--channel-stall-ms", "500"))
        msg = capture.failure_context()
        record = self.refusal(capture, "report_sink_failed", {
            "reason": "sink_exhausted", "sink_bytes": cap + 1, "cap_bytes": cap},
            exit_class="transport_failure", status=1, msg=msg)
        probes = {int(event[1:]) for event in capture.validator_events if event[:1] == b"P"}
        self.assertEqual(len(probes), 1, msg=f"real V never read the overflow probe; {msg}")
        self.assertEqual(capture.stopped_validators, probes,
                         msg=f"real V was not observed stopped; {msg}")
        self.assertEqual(capture.killed_validators, probes,
                         msg=f"host never issued the actual signal; {msg}")
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
                        record = self.assert_record(
                            capture, complete=True, exit_class="success", exit_status=0, msg=msg)
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
            sink=True, options=("--channel-wall-ms", "3000"))
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
        self.assertLessEqual(record["report_charged_bytes"], 67108864, msg=msg)

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

    def test_t31_new_zero_resolves_to_host_grant_and_positive_is_exact(self):
        grants = {"report_bytes": 4096, "diagnostic_bytes": 64,
                  "wall_ms": 10000, "stall_ms": 2000}
        knobs = (("--report-max-bytes", "report_bytes"),
                 ("--diagnostic-max-bytes", "diagnostic_bytes"),
                 ("--channel-wall-ms", "wall_ms"), ("--channel-stall-ms", "stall_ms"))
        for flag, field in knobs:
            other_options = tuple(part for other, _ in knobs if other != flag
                                  for part in (other, "0"))
            for value in ("0", str(grants[field] - 1)):
                with self.subTest(flag=flag, value=value):
                    capture = launch(grants=grants, options=(*other_options, flag, value))
                    msg = capture.failure_context()
                    record = self.assert_record(
                        capture, complete=True, exit_class="success", exit_status=0, msg=msg)
                    self.assertEqual(record["limits"][field], {
                        "requested": int(value), "granted": grants[field],
                        "effective": grants[field] if value == "0" else int(value)}, msg=msg)
            capture = launch(grants=grants,
                             options=(*other_options, flag, str(grants[field] + 1)))
            msg = capture.failure_context()
            record = self.refusal(capture, "run_request_invalid", {"option": "arguments"}, msg=msg)
            self.assertIsNone(record["producer"]["stage"], msg=msg)

    def test_t31_cli_overrides_environment(self):
        capture = launch(options=("--report-max-bytes", "4096"), environment={
            "PINEFORGE_REPORT_MAX_BYTES": "invalid"})
        msg = capture.failure_context()
        record = self.assert_record(capture, complete=True, exit_class="success", exit_status=0,
                                    msg=msg)
        self.assertEqual(record["limits"]["report_bytes"]["requested"], 4096, msg=msg)

    def test_t31_maximum_safe_ceiling_and_leading_zero(self):
        grants = {"report_bytes": 9007199254740990, "diagnostic_bytes": 1048576,
                  "wall_ms": 600000, "stall_ms": 5000}
        for value in ("9007199254740990", "00000000000000000000"):
            with self.subTest(value=value):
                capture = launch(grants=grants, options=("--report-max-bytes", value))
                msg = capture.failure_context()
                record = self.assert_record(
                    capture, complete=True, exit_class="success", exit_status=0, msg=msg)
                self.assertEqual(record["limits"]["report_bytes"], {
                    "requested": int(value), "granted": 9007199254740990,
                    "effective": 9007199254740990}, msg=msg)

    def test_t31_strict_decimal_parser(self):
        for flag in ("--report-max-bytes", "--diagnostic-max-bytes",
                     "--channel-wall-ms", "--channel-stall-ms"):
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
                    self.assert_record(capture, complete=True, exit_class="success", exit_status=0,
                                       msg=msg)
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
