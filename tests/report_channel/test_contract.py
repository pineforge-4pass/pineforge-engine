"""Round-one outside-in contract rows. Run with unittest discovery."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from host import ROOT, clean_environment, encoded, launch, reference, require_supervisor, sink_wire


class Contract(unittest.TestCase):
    def setUp(self):
        require_supervisor()

    def assert_record(self, capture, *, complete, exit_class, exit_status):
        record = capture.terminal()
        self.assertEqual(set(record), {
            "schema", "version", "contract", "attempt_id", "invocation", "complete",
            "exit_class", "failure", "exit_status", "producer", "limits",
            "report_charged_bytes", "channels", "sink_verification", "usage", "cleanup"})
        self.assertEqual(record["schema"], "pf-run-terminal")
        self.assertEqual(record["version"], 1)
        self.assertEqual(record["contract"], "supervised/v1")
        self.assertEqual(record["attempt_id"], capture.attempt_id)
        self.assertEqual(record["invocation"], "run_json")
        self.assertIs(record["complete"], complete)
        self.assertEqual(record["exit_class"], exit_class)
        self.assertEqual(record["exit_status"], exit_status)
        self.assertEqual(capture.returncode, exit_status)
        self.assertTrue({"report", "diagnostic", "status"} <= capture.eof)
        self.assertEqual(set(record["producer"]), {
            "stage", "exit_status", "signal", "failure", "failure_claim_omitted"})
        self.assertEqual(set(record["channels"]), {
            "candidate", "report", "sink", "diagnostic", "status"})
        for name, actual in (("report", capture.report), ("diagnostic", capture.diagnostic),
                             ("status", capture.packets[0])):
            cell = record["channels"][name]
            self.assertEqual(cell["written_bytes"], len(actual), name)
            self.assertEqual(cell["sha256"],
                             None if name == "status" else hashlib.sha256(actual).hexdigest())
        packet_cell = record["channels"]["status"]
        for key in ("observed_bytes", "accepted_bytes", "written_bytes"):
            self.assertEqual(packet_cell[key], len(capture.packets[0]))
        self.assertLessEqual(len(capture.packets[0]), 65536)
        self.assertTrue(capture.packets[0].endswith(b"\n"))
        self.assertEqual(record["usage"]["version"], 1)
        stages = record["usage"]["stages"]
        self.assertEqual(set(stages), {"prepare", "compile", "run", "finalize", "publish"})
        resources = {
            "wall", "cpu_user", "cpu_system", "memory_rss", "memory_private", "memory_job",
            "scratch_logical", "scratch_allocated", "work_logical", "work_allocated",
            "scratch_files", "work_files", "open_fds", "processes", "arena_claimed",
            "arena_capacity", "report_bytes", "sink_bytes", "diagnostic_bytes"}
        for name in ("prepare", "compile"):
            self.assertEqual(stages[name]["state"], "not_started")
            self.assertEqual(set(stages[name]["resources"]), resources)
            for cell in stages[name]["resources"].values():
                self.assertEqual(set(cell), {
                    "used", "requested", "limit", "peak", "unit", "scope", "source", "state"})
                self.assertEqual(cell["state"], "not_started")
                self.assertIsNone(cell["used"])
        for entry in record["cleanup"].values():
            self.assertEqual(set(entry), {"value", "measurement_state"})
            if entry["measurement_state"] == "unavailable":
                self.assertIsNone(entry["value"])
        return record

    def refusal(self, capture, code, args, *, exit_class="input_refusal", status=2):
        record = self.assert_record(
            capture, complete=False, exit_class=exit_class, exit_status=status)
        self.assertEqual(record["failure"], {
            "code": code, "args": args, "authority": "supervisor"})
        return record

    def test_t1_sinkless_success_preserves_reference_bytes(self):
        for closes in ([10, 12, 11], [21, 19, 23, 20]):
            for fragment in (1, 65536):
                with self.subTest(closes=closes, fragment=fragment):
                    request = {"closes": closes, "fragment_bytes": fragment}
                    baseline = reference(request)
                    self.assertEqual(baseline.returncode, 0)
                    capture = launch(request)
                    self.assertEqual(capture.report, baseline.stdout)
                    self.assertEqual(json.loads(capture.report)["summary"]["net_pnl"],
                                     closes[-1] - closes[0])
                    record = self.assert_record(
                        capture, complete=True, exit_class="success", exit_status=0)
                    self.assertIsNone(record["failure"])
                    self.assertEqual(record["producer"]["exit_status"], 0)
                    self.assertIsNone(record["producer"]["signal"])
                    self.assertEqual(record["report_charged_bytes"], len(baseline.stdout))
                    self.assertEqual(record["channels"]["candidate"]["observed_bytes"],
                                     len(baseline.stdout))
                    self.assertTrue(record["channels"]["candidate"]["source_total_known"])

    def test_t1_inherited_sink_success_keeps_summary_projection(self):
        request = {"closes": [10, 14, 12], "inherited_sink": True}
        baseline = reference(request)
        capture = launch(request, sink=True)
        self.assertEqual(capture.returncode, 0,
                         "inherited-sink success requires the round-two K/S/V relay")
        self.assertEqual(capture.report, baseline.stdout)
        self.assertEqual(capture.sink, baseline.sink)
        self.assertEqual(capture.sink, sink_wire(request["closes"]))
        self.assertNotIn("trades", json.loads(capture.report))
        record = self.assert_record(capture, complete=True, exit_class="success", exit_status=0)
        self.assertTrue(record["sink_verification"]["complete"])
        self.assertGreater(len(capture.sink), 0)
        self.assertEqual(record["channels"]["sink"]["written_bytes"], len(capture.sink))

    def test_t1_owned_runner_writer_preserves_success_bytes(self):
        for closes in ([9, 10, 15], [15, 12]):
            with self.subTest(closes=closes):
                request = {"closes": closes, "runner_writer": True}
                baseline = reference(request)
                capture = launch(request)
                self.assertEqual(baseline.returncode, 0, baseline.stderr)
                self.assertEqual(capture.report, baseline.stdout)
                self.assertEqual(json.loads(capture.report)["summary"]["net_pnl"],
                                 closes[-1] - closes[0])
                self.assert_record(capture, complete=True, exit_class="success", exit_status=0)

    def test_t1_owned_runner_failure_writer_preserves_failure_bytes(self):
        request = {"failure_writer": {
            "text": "controlled producer failure", "code": "harness_internal_error", "args": {}},
            "exit_status": 1}
        baseline = reference(request)
        capture = launch(request)
        self.assertEqual(baseline.returncode, 1, baseline.stderr)
        self.assertEqual(capture.report, baseline.stdout)
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1)
        self.assertEqual(record["producer"]["failure"]["authority"], "producer")

    def test_t2_defaults_and_not_started_usage(self):
        capture = launch()
        record = self.assert_record(capture, complete=True, exit_class="success", exit_status=0)
        limits = record["limits"]
        for name, expected in (("report_bytes", 67108864), ("diagnostic_bytes", 1048576),
                               ("wall_ms", 600000), ("stall_ms", 5000)):
            self.assertEqual(limits[name], {
                "requested": None, "granted": expected, "effective": expected})
        self.assertEqual(limits["terminal_bytes"], 65536)
        self.assertEqual(limits["stop_grace_ms"], 1000)
        self.assertEqual(limits["relay_queue_bytes"], 65536)
        self.assertEqual(limits["producer_claim_bytes"], 8192)
        self.assertIsNone(limits["sink_bytes"])
        self.assertIsNone(record["sink_verification"])

    def test_t2_unknown_contract_is_prestart_usage_error(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")},
                         options=("--report-channel", "supervised/v999"))
        record = self.refusal(capture, "run_request_invalid", {"option": "arguments"})
        self.assertIsNone(record["producer"]["stage"])
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic)

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
        self.assertEqual(baseline.returncode, 2)
        self.assertEqual(capture.report, baseline.stdout)
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1)
        self.assertEqual(record["producer"]["exit_status"], 2)
        self.assertEqual(record["producer"]["failure"], {
            "code": "run_request_invalid", "args": {"option": "arguments"},
            "authority": "producer"})

    def test_t2_missing_or_wrong_status_never_uses_another_channel(self):
        for endpoint in ("absent", "pipe", "stream", "reserved", "closed", "read_end"):
            with self.subTest(endpoint=endpoint):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, status=endpoint)
                self.assertEqual(capture.returncode, 2)
                self.assertEqual(capture.packets, [])
                self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic)
                self.assertNotIn(b"pf-run-terminal", capture.report + capture.diagnostic)
                self.assertEqual(json.loads(capture.report)["code"], "run_request_invalid")

    def test_t2_isolation_requires_host_attestation(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, attested=False)
        self.refusal(capture, "run_isolation_unavailable", {"reason": "policy"},
                     exit_class="resource_refusal", status=1)
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic)

    def test_t2_zero_host_grant_refuses_before_producer(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, grants={
            "report_bytes": 0, "diagnostic_bytes": 1048576,
            "wall_ms": 600000, "stall_ms": 5000})
        self.refusal(capture, "run_isolation_unavailable", {"reason": "policy"},
                     exit_class="resource_refusal", status=1)
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic)

    def test_t2_new_knobs_without_activation_are_usage_errors(self):
        for name, value in (("--report-max-bytes", "0"), ("--diagnostic-max-bytes", "10"),
                            ("--channel-wall-ms", "10"), ("--channel-stall-ms", "10"),
                            ("--terminal-status-fd", "9")):
            with self.subTest(name=name):
                result = subprocess.run(
                    [sys.executable, "-B", str(ROOT / "docker/run_json.py"), name, value],
                    capture_output=True, env=clean_environment(), timeout=10, check=False)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(json.loads(result.stdout)["code"], "run_request_invalid")
                self.assertEqual(json.loads(result.stdout)["args"], {"option": "arguments"})
        for name in ("PINEFORGE_REPORT_MAX_BYTES", "PINEFORGE_DIAGNOSTIC_MAX_BYTES",
                     "PINEFORGE_CHANNEL_WALL_MS", "PINEFORGE_CHANNEL_STALL_MS",
                     "PINEFORGE_TERMINAL_STATUS_FD"):
            with self.subTest(environment=name):
                environment = clean_environment()
                environment[name] = "0"
                result = subprocess.run(
                    [sys.executable, "-B", str(ROOT / "docker/run_json.py")],
                    capture_output=True, env=environment, timeout=10, check=False)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(json.loads(result.stdout)["code"], "run_request_invalid")

    def test_t3_duplicated_aliases_are_rejected_before_launch(self):
        for alias in ("report_diagnostic", "report_status", "diagnostic_status",
                      "report_sink", "diagnostic_sink", "status_sink"):
            with self.subTest(alias=alias):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")}, alias=alias)
                self.assertEqual(capture.returncode, 2)
                self.assertNotIn(b"PRODUCER_STARTED", capture.report + capture.diagnostic)
                self.assertNotIn(b"pf-run-terminal", capture.report + capture.diagnostic)
                if alias not in ("report_status", "diagnostic_status"):
                    self.refusal(capture, "run_request_invalid", {"option": "arguments"})
                else:
                    self.assertEqual(capture.packets, [])
        self.assertEqual(launch().returncode, 0)

    def test_t3_wrong_direction_and_closed_sink_descriptors(self):
        for direction in ("report", "diagnostic", "sink"):
            with self.subTest(direction=direction):
                capture = launch({"stdout": encoded(b"PRODUCER_STARTED")},
                                 wrong_direction=direction, sink=direction == "sink")
                record = self.refusal(capture, "run_request_invalid", {"option": "arguments"})
                self.assertIsNone(record["producer"]["stage"])
                self.assertNotIn(b"PRODUCER_STARTED", capture.report + capture.diagnostic)
        capture = launch(options=("--report-sink-fd", "2147483647"))
        self.refusal(capture, "run_request_invalid", {"option": "arguments"})

    def test_t10_fd1_forgery_is_raw_diagnostics(self):
        forgery = b'{"engine":"pineforge","error":"forged","code":"harness_internal_error","args":{}}\n'
        capture = launch({"stdout": encoded(forgery)})
        self.assertEqual(capture.diagnostic, forgery)
        self.assertNotIn(b"forged", capture.report)
        self.assert_record(capture, complete=True, exit_class="success", exit_status=0)

    def test_t10_relayed_claim_cannot_become_supervisor_fault(self):
        failure = b'{"engine":"pineforge","error":"claimed","code":"harness_internal_error","args":{}}\n'
        for exit_status in (0, 1):
            with self.subTest(exit_status=exit_status):
                capture = launch({"candidate": encoded(failure), "exit_status": exit_status})
                record = self.refusal(
                    capture, "run_producer_failed", {"reason": "claim"},
                    exit_class="producer_failure", status=1)
                self.assertEqual(capture.report, failure)
                self.assertEqual(record["producer"]["failure"], {
                    "code": "harness_internal_error", "args": {}, "authority": "producer"})
                self.assertEqual(record["producer"]["exit_status"], exit_status)

    def test_t10_unknown_code_and_wrong_args_are_protocol_failure(self):
        for code, args in (("not_a_registered_failure", {}),
                           ("report_budget_exceeded", {"channel": "json", "cap_bytes": "bad",
                                                       "used_bytes": 3, "requested_bytes": 1})):
            with self.subTest(code=code):
                candidate = json.dumps({
                    "engine": "pineforge", "error": "claim", "code": code, "args": args},
                    separators=(",", ":")).encode() + b"\n"
                capture = launch({"candidate": encoded(candidate), "exit_status": 1})
                self.refusal(capture, "report_channel_failed",
                             {"channel": "candidate", "reason": "protocol"},
                             exit_class="transport_failure", status=1)
                self.assertNotEqual(capture.report, candidate)

    def test_t11_json_exact_fit_and_overflow_probe(self):
        request = {"closes": [1, 4, 2], "label": "café🙂.csv"}
        expected = reference(request).stdout
        for allowance in (len(expected), len(expected) - 1):
            with self.subTest(allowance=allowance):
                capture = launch(request, options=("--report-max-bytes", str(allowance)))
                if allowance == len(expected):
                    self.assertEqual(capture.report, expected)
                    self.assert_record(capture, complete=True, exit_class="success", exit_status=0)
                else:
                    record = self.refusal(capture, "report_budget_exceeded", {
                        "channel": "json", "cap_bytes": allowance,
                        "used_bytes": allowance, "requested_bytes": 1},
                        exit_class="resource_refusal", status=1)
                    self.assertEqual(capture.report, b"")
                    self.assertEqual(record["report_charged_bytes"], allowance)
                    cell = record["channels"]["candidate"]
                    self.assertEqual(cell["observed_bytes"], allowance + 1)
                    self.assertEqual(cell["accepted_bytes"], allowance)
                    self.assertTrue(cell["truncated"])
                    self.assertFalse(cell["source_total_known"])

    def test_t11_planned_sink_larger_than_aggregate_refuses_before_start(self):
        capture = launch({"stdout": encoded(b"PRODUCER_STARTED")},
                         options=("--report-max-bytes", "512"), planned_sink_bytes=513)
        record = self.refusal(capture, "report_budget_exceeded", {
            "channel": "aggregate", "cap_bytes": 512, "used_bytes": 0, "requested_bytes": 513},
            exit_class="resource_refusal", status=1)
        self.assertIsNone(record["producer"]["stage"])
        self.assertNotIn(b"PRODUCER_STARTED", capture.diagnostic)
        self.assertLessEqual(record["report_charged_bytes"], 512)

    def test_t17_empty_and_partial_candidates_fail_only_after_wait(self):
        for candidate in (b"", b'{"engine":"pineforge"', b'{"engine":"pineforge"}'):
            for exit_status in (0, 7):
                with self.subTest(candidate=candidate, exit_status=exit_status):
                    capture = launch({"candidate": encoded(candidate), "exit_status": exit_status,
                                      "fragment_bytes": 1})
                    if exit_status:
                        self.refusal(capture, "run_producer_failed",
                                     {"reason": "exit", "exit_status": 7},
                                     exit_class="producer_failure", status=1)
                    else:
                        self.refusal(capture, "report_channel_failed",
                                     {"channel": "candidate", "reason": "protocol"},
                                     exit_class="transport_failure", status=1)
                    self.assertNotEqual(capture.report, candidate)
                    if capture.report:
                        self.assertEqual(capture.report.count(b"\n"), 1)

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
                self.refusal(capture, "report_channel_failed",
                             {"channel": "candidate", "reason": "protocol"},
                             exit_class="transport_failure", status=1)
                self.assertNotEqual(capture.report, candidate)

    def test_t25_legacy_failure_is_relayed_as_a_claim(self):
        candidate = b'{"engine":"pineforge","error":"legacy failure"}\n'
        capture = launch({"candidate": encoded(candidate)})
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1)
        self.assertEqual(capture.report, candidate)
        self.assertIsNone(record["producer"]["failure"])
        self.assertFalse(record["producer"]["failure_claim_omitted"])

    def test_t25_oversized_valid_claim_is_omitted_without_empty_args(self):
        candidate = json.dumps({
            "engine": "pineforge", "error": "large claim", "code": "window_lineage_mismatch",
            "args": {"field": "z" * 9000, "expected": 1, "actual": 2}},
            separators=(",", ":")).encode() + b"\n"
        capture = launch({"candidate": encoded(candidate), "exit_status": 1})
        record = self.refusal(capture, "run_producer_failed", {"reason": "claim"},
                              exit_class="producer_failure", status=1)
        self.assertEqual(capture.report, candidate)
        self.assertIsNone(record["producer"]["failure"])
        self.assertTrue(record["producer"]["failure_claim_omitted"])

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
                    record = self.assert_record(
                        capture, complete=True, exit_class="success", exit_status=0)
                    self.assertEqual(record["limits"][field], {
                        "requested": int(value), "granted": grants[field],
                        "effective": grants[field] if value == "0" else int(value)})
            capture = launch(grants=grants,
                             options=(*other_options, flag, str(grants[field] + 1)))
            record = self.refusal(capture, "run_request_invalid", {"option": "arguments"})
            self.assertIsNone(record["producer"]["stage"])

    def test_t31_cli_overrides_environment(self):
        capture = launch(options=("--report-max-bytes", "4096"), environment={
            "PINEFORGE_REPORT_MAX_BYTES": "invalid"})
        record = self.assert_record(capture, complete=True, exit_class="success", exit_status=0)
        self.assertEqual(record["limits"]["report_bytes"]["requested"], 4096)

    def test_t31_maximum_safe_ceiling_and_leading_zero(self):
        grants = {"report_bytes": 9007199254740990, "diagnostic_bytes": 1048576,
                  "wall_ms": 600000, "stall_ms": 5000}
        for value in ("9007199254740990", "00000000000000000000"):
            with self.subTest(value=value):
                capture = launch(grants=grants, options=("--report-max-bytes", value))
                record = self.assert_record(
                    capture, complete=True, exit_class="success", exit_status=0)
                self.assertEqual(record["limits"]["report_bytes"], {
                    "requested": int(value), "granted": 9007199254740990,
                    "effective": 9007199254740990})

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
                self.assertEqual(capture.diagnostic, diagnostic[:allowance])
                if allowance == len(diagnostic):
                    self.assert_record(capture, complete=True, exit_class="success", exit_status=0)
                else:
                    record = self.refusal(capture, "diagnostic_budget_exceeded", {
                        "cap_bytes": allowance, "used_bytes": allowance, "requested_bytes": 1},
                        exit_class="resource_refusal", status=1)
                    self.assertEqual(record["channels"]["diagnostic"]["observed_bytes"],
                                     allowance + 1)
                    self.assertTrue(record["channels"]["diagnostic"]["truncated"])

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
                    if stream:
                        self.assertEqual(result.returncode, 2)
                        self.assertEqual(receipt, b"")
                    else:
                        self.assertEqual(result.returncode, 8)
                        document = json.loads(receipt)
                        self.assertFalse(document["complete"])
                        self.assertEqual(document["reason"], "sink_exhausted")
                        self.assertEqual(document["sink_bytes"], 1)
                        self.assertEqual(document["cap_bytes"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
