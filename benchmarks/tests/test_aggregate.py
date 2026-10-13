#!/usr/bin/env python3
"""Exercise the real speed report generator with small, standalone inputs."""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH = REPO_ROOT / "benchmarks"


class AggregateOptionalPyneCore(unittest.TestCase):
    def test_report_with_and_without_pynecore(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bench-aggregate-") as tmp:
            root = Path(tmp).resolve()
            bench = root / "benchmarks"
            (bench / "speed").mkdir(parents=True)
            # Relocate the unchanged real generator and its path module so
            # --feed can name a file under its scratch REPO_ROOT.
            aggregate = bench / "speed" / "aggregate.py"
            shutil.copy2(BENCH / "speed" / "aggregate.py", aggregate)
            shutil.copy2(BENCH / "paths.py", bench / "paths.py")
            feed = bench / "feed.csv"
            feed.write_text("timestamp,open,high,low,close,volume\n"
                            "0,1,1,1,1,1\n1,1,1,1,1,1\n")
            pf = bench / "pf.json"
            pf.write_text(json.dumps({"benchmarks": [{
                "name": "001-probe/throughput/with_magnifier/iterations:20",
                "real_time": 1, "time_unit": "ms", "iterations": 20,
            }]}))
            pc = bench / "pc.json"
            pc.write_text(json.dumps({"001-probe": {"median_ms": 10, "p95_ms": 12, "n": 20}}))
            pt = bench / "pt.json"
            pt.write_text(json.dumps({"canonical": {"median_ms": 4, "p95_ms": 5, "n": 20}}))
            vbt = bench / "vbt.json"
            vbt.write_text(json.dumps({"001-probe": {"median_ms": 2, "p95_ms": 3, "n": 20}}))
            host = bench / "host.json"
            host.write_text(json.dumps({"cpu": "test host", "cores": 1, "os": "test OS",
                                        "python": "3.11", "pynecore": "6.10.3", "pinets": "0.9.34"}))
            for with_pc in (False, True):
                with self.subTest(with_pynecore=with_pc):
                    out = bench / f"report-{with_pc}.md"
                    cmd = [sys.executable, str(aggregate), "--pineforge", str(pf),
                           "--pinets", str(pt), "--vectorbt", str(vbt), "--feed", str(feed),
                           "--host", str(host), "--engine-label", "test-engine",
                           "--as-of", "2026-09-29", "--out", str(out)]
                    if with_pc:
                        cmd += ["--pynecore", str(pc)]
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    report = out.read_text()
                    headers = [line for line in report.splitlines() if line.startswith("| ")
                               and any(cell in line for cell in ("Strategy", "Throughput", "Run"))]
                    self.assertTrue(headers)
                    if with_pc:
                        self.assertIn("As of: 2026-09-29. PineForge engine test-engine; "
                                      "PyneCore 6.10.3; PineTS 0.9.34;", report)
                        self.assertIn("- **Median speedup PineForge vs PyneCore** "
                                      "(across 1 commonly-timed strategies, 1 corpus probes and 0 "
                                      "held-out private strategies): **10×** (p5 10×, p95 10×)", report)
                        self.assertIn("PC median (ms) | PC p95 (ms) | PC bars/s", report)
                        self.assertIn("Speedup PF vs PC", report)
                        self.assertIn("| 001-probe | 1.00 | 2.0 k | 10 | 12 | 0.2 k | 2.0 | 10× | 2.0× |", report)
                        self.assertIn("| Throughput (bars/s) | Q1 | Median | Q3 |", report)
                        self.assertIn("| PyneCore (subprocess wall time) | 0.2 k | **0.2 k** | 0.2 k |", report)
                        expected_method = (
                            "- **PyneCore:** timed with a harness kept outside this repository, as subprocess\n"
                            "  wall time including Python interpreter startup, PyneCore framework\n"
                            "  import and the full backtest; median and p95 over `N=20` invocations, "
                            "8 strategies timed concurrently.\n"
                            "- **Bars/s:** feed bars ÷ median seconds per strategy. For PyneCore that is the\n"
                            "  subprocess wall time above, so startup and import are inside the figure;\n"
                            "  PineForge's is the in-process backtest alone.\n"
                        )
                        self.assertIn(expected_method, report)
                        self.assertIn(expected_method, (BENCH / "results" / "speed.md").read_text())
                    else:
                        self.assertFalse(any("PyneCore" in header or "PC " in header for header in headers))
                        self.assertNotIn("PyneCore", report)
                        self.assertIn("PyneCore timings not supplied", result.stderr)
                        self.assertIn("| Strategy | PF median (ms) | PF bars/s | "
                                      "vbt median (ms) | Speedup PF vs vbt |", report)
                        self.assertIn("| 001-probe | 1.00 | 2.0 k | 2.0 | 2.0× |", report)
                        self.assertIn("| PineForge (in-process, magnifier ON) | "
                                      "2.0 k | **2.0 k** | 2.0 k |", report)


if __name__ == "__main__":
    unittest.main()
