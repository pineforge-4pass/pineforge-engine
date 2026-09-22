#!/usr/bin/env python3
"""run_all.sh stops on a PineForge slot failure and never leaves a stale trade list.

Each case copies benchmarks/run_all.sh into a scratch tree whose engine runners
are stubs (scripts/run_strategy.py, benchmarks/runners/run_pynecore.py,
benchmarks/speed/time_vectorbt.py behind a stub uv): a slot named *-fails
makes the stub fail, any other slot gets a fresh trade list. Every slot starts
with a STALE trade list from an earlier run, the committed assets' situation.
A run leaves each slot with either this run's trade list or none, so
compare.py can never grade an earlier run's output.

- PineForge is the engine under test: a run error, or a missing strategy
  library beside a generated.cpp, makes run_all.sh exit non-zero before any
  report is written. A slot without generated.cpp is a codegen failure that
  compare.py grades n/a, not a harness error.
- PyneCore failures are results (compare.py grades them n/a with the error),
  so they do not change the exit status.

    python3 benchmarks/tests/test_run_all.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_ALL = REPO_ROOT / "benchmarks" / "run_all.sh"

STUB_RUN_STRATEGY = """\
import sys
from pathlib import Path
slot = Path(sys.argv[1])
if slot.name.endswith("-fails"):
    sys.exit("RuntimeError: scripted PineForge failure")
Path(sys.argv[sys.argv.index("--output") + 1]).write_text("fresh\\n")
"""
STUB_RUN_PYNECORE = """\
import sys
from pathlib import Path
slot = Path(sys.argv[1])
if slot.name.endswith("-fails"):
    sys.exit("RuntimeError: scripted PyneCore failure")
(slot / "pynecore_trades.csv").write_text("fresh\\n")
"""
STUB_TIME_VECTORBT = """\
import sys
from pathlib import Path
for slot in sorted((Path(__file__).resolve().parents[1] / "assets" / "strategies").iterdir()):
    if (slot / "strategy_vbt.py").exists() and not slot.name.endswith("-fails"):
        (slot / "vectorbt_trades.csv").write_text("fresh\\n")
"""
STUB_UV = '#!/bin/sh\n# uv run python <script> <args>\nshift 2\nexec python3 "$@"\n'
QUIET = {name: "1" for name in ("SKIP_BUILD", "SKIP_PINETS", "SKIP_VECTORBT",
                                "SKIP_INDICATORS", "SKIP_SPEED", "SKIP_REPORTS")}


class RunAllFailures(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="bench-run-all-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        bench = self.root / "benchmarks"
        (bench / "runners").mkdir(parents=True)
        (bench / "speed").mkdir()
        shutil.copy2(RUN_ALL, bench / "run_all.sh")
        (bench / "runners" / "run_pynecore.py").write_text(STUB_RUN_PYNECORE)
        (bench / "speed" / "time_vectorbt.py").write_text(STUB_TIME_VECTORBT)
        (self.root / "scripts").mkdir()
        (self.root / "scripts" / "run_strategy.py").write_text(STUB_RUN_STRATEGY)
        (self.root / "bin").mkdir()
        (self.root / "bin" / "uv").write_text(STUB_UV)
        (self.root / "bin" / "uv").chmod(0o755)
        # The environment run_all.sh sets up once: venv, node_modules, the
        # feed snapshot and its PyneCore conversion (older than the .ohlcv).
        (bench / ".venv" / "bin").mkdir(parents=True)
        (bench / ".venv" / "bin" / "activate").write_text("")
        (bench / "node_modules").mkdir()
        (bench / "assets" / "data").mkdir(parents=True)
        (bench / "_workdir" / "data").mkdir(parents=True)
        feed = "timestamp,open,high,low,close,volume\n0,1,1,1,1,1\n"
        (bench / "assets" / "data" / "ETHUSDT_15.csv").write_text(feed)
        live = bench / "_workdir" / "data" / "ETHUSDT_15.csv"
        live.write_text(feed)
        converted = live.with_suffix(".ohlcv")
        converted.write_text("")
        now = time.time()
        os.utime(live, (now - 60, now - 60))
        os.utime(converted, (now, now))
        self.strategies = bench / "assets" / "strategies"

    def slot(self, name: str, *, codegen: bool = True, library: bool = True) -> Path:
        s = self.strategies / name
        s.mkdir(parents=True)
        (s / "strategy.pine").write_text("//@version=6\n")
        (s / "strategy_pyne.py").write_text("")
        if codegen:
            (s / "generated.cpp").write_text("")
        if library:
            (s / "strategy.dylib").write_text("")
        (s / "strategy_vbt.py").write_text("")
        for csv in ("pineforge_trades.csv", "pynecore_trades.csv", "vectorbt_trades.csv"):
            (s / csv).write_text("STALE\n")
        return s

    def run_all(self, **env: str) -> subprocess.CompletedProcess:
        path = f"{self.root / 'bin'}{os.pathsep}{os.environ['PATH']}"
        return subprocess.run(
            ["bash", str(self.root / "benchmarks" / "run_all.sh")],
            env={**os.environ, **QUIET, "PATH": path, "JOBS": "2", **env},
            capture_output=True, text=True, timeout=120)

    def assert_output(self, slot: Path, csv: str, content: str | None) -> None:
        path = slot / csv
        if content is None:
            self.assertFalse(path.exists(), f"{slot.name}/{csv} left behind: "
                             f"{path.read_text().strip() if path.exists() else ''}")
        else:
            self.assertEqual(path.read_text(), content, f"{slot.name}/{csv}")

    def test_pineforge_run_error_exits_nonzero(self) -> None:
        ok, bad = self.slot("001-ok"), self.slot("002-fails")
        res = self.run_all(SKIP_PYNE="1")
        self.assertNotEqual(res.returncode, 0, "a PineForge slot error exited 0")
        self.assertIn("002-fails", res.stderr)
        self.assert_output(ok, "pineforge_trades.csv", "fresh\n")
        self.assertFalse((ok / "_pineforge_error.log").exists())
        self.assert_output(bad, "pineforge_trades.csv", None)
        self.assertIn("scripted PineForge failure", (bad / "_pineforge_error.log").read_text())

    def test_missing_strategy_library_exits_nonzero(self) -> None:
        self.slot("001-ok")
        unbuilt = self.slot("003-unbuilt", library=False)
        res = self.run_all(SKIP_PYNE="1")
        self.assertNotEqual(res.returncode, 0, "a slot without its strategy library exited 0")
        self.assertIn("003-unbuilt", res.stderr)
        self.assert_output(unbuilt, "pineforge_trades.csv", None)
        self.assertIn("strategy library", (unbuilt / "_pineforge_error.log").read_text())

    def test_every_slot_runs_or_has_no_generated_cpp(self) -> None:
        ok = self.slot("001-ok")
        no_codegen = self.slot("004-no-codegen", codegen=False, library=False)
        res = self.run_all(SKIP_PYNE="1")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assert_output(ok, "pineforge_trades.csv", "fresh\n")
        self.assert_output(no_codegen, "pineforge_trades.csv", None)
        self.assertFalse((no_codegen / "_pineforge_error.log").exists())

    def test_pynecore_error_leaves_no_stale_trade_list(self) -> None:
        ok, bad = self.slot("001-ok"), self.slot("005-fails")
        res = self.run_all(SKIP_PINEFORGE="1")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assert_output(ok, "pynecore_trades.csv", "fresh\n")
        self.assertFalse((ok / "_pynecore_error.log").exists())
        self.assert_output(bad, "pynecore_trades.csv", None)
        self.assertIn("scripted PyneCore failure", (bad / "_pynecore_error.log").read_text())

    def test_vectorbt_port_failure_leaves_no_stale_trade_list(self) -> None:
        ok, bad = self.slot("001-ok"), self.slot("006-fails")
        res = self.run_all(SKIP_PINEFORGE="1", SKIP_PYNE="1", SKIP_VECTORBT="0")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assert_output(ok, "vectorbt_trades.csv", "fresh\n")
        self.assert_output(bad, "vectorbt_trades.csv", None)


if __name__ == "__main__":
    unittest.main()
