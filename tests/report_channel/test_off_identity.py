"""Named OFF-identity acceptance row; main's CLOUD goldens are mandatory.

Run with unittest discovery, or directly. This round intentionally ships EMPTY
digests. The row fails visibly until the supervisor supplies main's own bytes.
It never records/rebaselines expected bytes from the tree being tested.

PINEFORGE_REPORT_OFF_GOLDEN_DIR must contain requests.json and reports.json.
requests.json is {"engine_revision":"fc0a2df5d","cases":[...]} with exactly
one case per non-comment line in scripts/corpus_parity_subset.txt. Each case
is {"id":<probe>,"argv":[...],"environment":{...},"fixed_clock":{...}}.
Paths are relative to the engine root, identical for both runs. The case's
fixed_clock maps time/time_ns/perf_counter/perf_counter_ns/monotonic/
monotonic_ns to finite constants recorded by the main-tree generator.
reports.json is {"cases":[{"id":...,"report_base64":...,
"report_sha256":...,"failure_code":<string or null>,"exit_status":<int>},...]}.
Its bytes, request bytes and scenario-list bytes must match the three digests
in off_identity_golden.json before *any* candidate-tree invocation is trusted.
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import unittest

from host import ROOT, clean_environment, require_supervisor


MANIFEST = Path(__file__).with_name("off_identity_golden.json")
CLOCKS = {"time", "time_ns", "perf_counter", "perf_counter_ns", "monotonic", "monotonic_ns"}
# Freeze only nondeterministic observations in the child, identically to the
# main-tree capture. Execution, library loading, report encoding and exits are
# the actual run_json implementation. No producer writer is replaced.
CLOCK_RUNNER = r"""
import importlib.util
import json
from pathlib import Path
import sys
import time

root = Path(sys.argv[1])
case = json.loads(sys.argv[2])
for name, value in case["fixed_clock"].items():
    setattr(time, name, lambda value=value: value)
sys.path.insert(0, str(root / "docker"))
spec = importlib.util.spec_from_file_location("off_identity_run_json", root / "docker/run_json.py")
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)
sys.argv = [str(root / "docker/run_json.py"), *case["argv"]]
sys.exit(runner._host_main())
"""


class OffIdentity(unittest.TestCase):
    def setUp(self):
        require_supervisor()

    def test_off_identity_corpus_parity_subset(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "pf-report-off-identity-golden/v1")
        self.assertEqual(manifest["engine_revision"], "fc0a2df5d")
        self.assertEqual(manifest["corpus_revision"], "8425ca820c4b7272db24d659a64d88e8e05b6407")
        digest_names = ("scenario_list_sha256", "requests_sha256", "reports_sha256")
        missing = [key for key in digest_names if not manifest[key]]
        self.assertFalse(
            missing,
            "OFF-identity is RED: main-generated CLOUD golden digests are EMPTY: "
            + ", ".join(missing) + "; main's own report bytes must be supplied in round two")
        for name in digest_names:
            self.assertRegex(manifest[name], r"^[0-9a-f]{64}$")
        golden_path = os.environ.get("PINEFORGE_REPORT_OFF_GOLDEN_DIR")
        self.assertTrue(golden_path, "set PINEFORGE_REPORT_OFF_GOLDEN_DIR to main's CLOUD artifacts")
        directory = Path(golden_path)
        subset = (ROOT / manifest["scenario_list"]).read_bytes()
        requests_raw = (directory / "requests.json").read_bytes()
        reports_raw = (directory / "reports.json").read_bytes()
        for name, content in (("scenario_list_sha256", subset),
                              ("requests_sha256", requests_raw), ("reports_sha256", reports_raw)):
            self.assertEqual(hashlib.sha256(content).hexdigest(), manifest[name], name)
        ids = [line.partition("#")[0].strip() for line in subset.decode().splitlines()]
        ids = [name for name in ids if name]
        requests = json.loads(requests_raw)
        self.assertEqual(requests["engine_revision"], "fc0a2df5d")
        cases = requests["cases"]
        expected_rows = json.loads(reports_raw)["cases"]
        self.assertEqual([case["id"] for case in cases], ids)
        self.assertEqual([row["id"] for row in expected_rows], ids)
        self.assertEqual(len(ids), len(set(ids)))
        for case, expected in zip(cases, expected_rows):
            with self.subTest(scenario=case["id"]):
                self.assertTrue(case["argv"], "main must record the exact request")
                self.assertFalse(any(token.startswith(("--report-channel", "--terminal-status-fd",
                                                       "--report-max-bytes", "--diagnostic-max-bytes",
                                                       "--channel-wall-ms", "--channel-stall-ms"))
                                     for token in case["argv"]))
                self.assertEqual(set(case["fixed_clock"]), CLOCKS)
                self.assertTrue(all(type(value) in (int, float) and math.isfinite(value)
                                    for value in case["fixed_clock"].values()))
                environment = clean_environment()
                self.assertFalse(any(
                    key.startswith(("PINEFORGE_REPORT_", "PINEFORGE_TERMINAL_",
                                    "PINEFORGE_CHANNEL_", "PINEFORGE_DIAGNOSTIC_"))
                    for key in case["environment"]))
                environment.update(case["environment"])
                baseline = base64.b64decode(expected["report_base64"], validate=True)
                self.assertRegex(expected["report_sha256"], r"^[0-9a-f]{64}$")
                self.assertEqual(hashlib.sha256(baseline).hexdigest(), expected["report_sha256"])
                self.assertIs(type(expected["exit_status"]), int)
                result = subprocess.run(
                    [sys.executable, "-B", "-c", CLOCK_RUNNER, str(ROOT), json.dumps(case)],
                    cwd=ROOT, env=environment, capture_output=True, check=False, timeout=600)
                self.assertEqual(result.stdout, baseline, "OFF report bytes changed")
                self.assertEqual(result.returncode, expected["exit_status"], result.stderr)
                self.assertEqual(json.loads(result.stdout).get("code"), expected["failure_code"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
