#!/usr/bin/env python3
"""run_all.sh stops on a PineForge slot failure and never leaves a stale trade list.

Each case copies benchmarks/run_all.sh into a scratch tree whose engine runners
are stubs (scripts/run_strategy.py, a private harness's runners/run_pynecore.py,
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

import json
import os
import shutil
import subprocess
import sys
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
import os
import sys
from pathlib import Path
repo_root = Path(__file__).resolve().parents[2]
(repo_root / "pynecore_repo_root.txt").write_text(os.environ.get("PINEFORGE_REPO_ROOT", ""))
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
STUB_PYNE = """\
import json
import sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
with (root / "pyne.calls").open("a") as calls:
    calls.write(json.dumps(sys.argv[1:]) + "\\n")
if "convert-from" in sys.argv:
    Path(sys.argv[-1]).with_suffix(".ohlcv").write_text("converted\\n")
elif "--plot" in sys.argv:
    Path(sys.argv[sys.argv.index("--plot") + 1]).write_text("fresh\\n")
else:
    sys.exit("unexpected pyne call")
"""
SKIP_MESSAGE = ("PyneCore harness not present (set PYNECORE_HARNESS_DIR to the private harness); "
                "skipping the PyneCore leg")
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
        self.harness = self.root / "private-harness"
        (self.harness / "runners").mkdir(parents=True)
        (self.harness / "runners" / "run_pynecore.py").write_text(STUB_RUN_PYNECORE)
        (self.harness / "speed").mkdir()
        (self.harness / "speed" / "time_pynecore.py").write_text('print("{}")\n')
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

    def run_all(self, **env: str | None) -> subprocess.CompletedProcess:
        path = f"{self.root / 'bin'}{os.pathsep}{os.environ['PATH']}"
        run_env = {**os.environ, **QUIET, "PATH": path, "JOBS": "2",
                   "SKIP_PYNE": "0", "PYNECORE_HARNESS_DIR": str(self.harness)}
        for name, value in env.items():
            if value is None:
                run_env.pop(name, None)
            else:
                run_env[name] = value
        return subprocess.run(
            ["bash", str(self.root / "benchmarks" / "run_all.sh")],
            env=run_env,
            capture_output=True, text=True)

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

    def prepare_report_stubs(self) -> None:
        bench = self.root / "benchmarks"
        pyne = self.root / "bin" / "pyne"
        pyne.write_text(f"#!{sys.executable}\n" + STUB_PYNE)
        pyne.chmod(0o755)
        (self.harness / "speed" / "time_pynecore.py").write_text(
            'print(\'{"001-ok": {"median_ms": 2, "p95_ms": 3, "n": 1}}\')\n')
        (bench / "speed" / "time_pineforge_docker.py").write_text('print("{}")\n')
        node = self.root / "bin" / "node"
        node.write_text('#!/bin/sh\nprintf "{}\\n"\n')
        node.chmod(0o755)
        (self.strategies / "_indicators").mkdir()
        canonical = bench / "runners" / "run_pineforge_canonical"
        canonical.write_text('#!/bin/sh\nprintf "fresh\\n" > "$2"\n')
        canonical.chmod(0o755)
        (bench / "speed" / "aggregate.py").write_text(
            'import sys\nfrom pathlib import Path\n'
            'Path("_workdir/aggregate_args.txt").write_text("\\n".join(sys.argv[1:]))\n'
            '# Like the real aggregate: the table goes to --out, by default to results/speed.md.\n'
            'out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv '
            'else Path("results/speed.md")\n'
            'out.write_text("SPY TABLE\\n")\n')
        (bench / "compare.py").write_text(
            'from pathlib import Path\nPath("_workdir/trade_report.called").touch()\n')
        (bench / "compare_indicators.py").write_text(
            'from pathlib import Path\nPath("_workdir/indicator_report.called").touch()\n')

    def reset_report_case(self, slot: Path) -> None:
        # All of these files belong to this test's TemporaryDirectory.
        bench = self.root / "benchmarks"
        work = bench / "_workdir"
        for name in ("trade_report.called", "indicator_report.called", "aggregate_args.txt",
                     "pc_speed.json", "pc_speed.err"):
            (work / name).unlink(missing_ok=True)
        for name in ("pyne.calls", "pynecore_repo_root.txt"):
            (self.root / name).unlink(missing_ok=True)
        (work / "data" / "ETHUSDT_15.ohlcv").unlink(missing_ok=True)
        for name in ("pineforge_trades.csv", "vectorbt_trades.csv"):
            (slot / name).write_text("STALE")
        # The committed PyneCore results (published data) that a run without the harness must leave alone.
        (slot / "pynecore_trades.csv").write_text("COMMITTED\n")
        (slot / "_pynecore_error.log").unlink(missing_ok=True)
        results = bench / "results"
        results.mkdir(exist_ok=True)
        (results / "speed.md").write_text("COMMITTED SPEED TABLE\n")
        (work / "speed.md").unlink(missing_ok=True)
        indicators = self.strategies / "_indicators"
        (indicators / "canonical_pineforge.csv").write_text("STALE")
        (indicators / "canonical_pyne.csv").write_text("COMMITTED\n")
        (indicators / "canonical_pyne.py").write_text("")

    def run_report_case(self, **env: str | None) -> subprocess.CompletedProcess:
        return self.run_all(SKIP_SPEED="0", SKIP_VECTORBT="0", SKIP_REPORTS="0",
                            SKIP_INDICATORS="0", SKIP_PINETS="0",
                            SKIP_PINEFORGE="0", SLOTS="",
                            RUNNER="docker", QUIET_LOAD_MAX="", **env)

    def test_absent_pynecore_harness_completes_without_pynecore_outputs(self) -> None:
        ok = self.slot("001-ok")
        self.prepare_report_stubs()
        bench = self.root / "benchmarks"
        for harness_dir, skip in ((None, "0"), (str(self.root / "missing-harness"), "0"),
                                  (str(self.harness), "1")):
            with self.subTest(harness_dir=harness_dir, skip=skip):
                self.reset_report_case(ok)
                res = self.run_report_case(PYNECORE_HARNESS_DIR=harness_dir, SKIP_PYNE=skip)
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertIn("done.", res.stdout)
                self.assert_output(ok, "pineforge_trades.csv", "fresh\n")
                self.assert_output(ok, "vectorbt_trades.csv", "fresh\n")
                self.assert_output(self.strategies / "_indicators", "canonical_pineforge.csv", "fresh\n")
                # Neither the runner nor the indicator run touched the committed PyneCore results.
                self.assert_output(ok, "pynecore_trades.csv", "COMMITTED\n")
                self.assertFalse((ok / "_pynecore_error.log").exists())
                self.assert_output(self.strategies / "_indicators", "canonical_pyne.csv", "COMMITTED\n")
                self.assertFalse((self.root / "pynecore_repo_root.txt").exists())
                args = (bench / "_workdir" / "aggregate_args.txt").read_text().splitlines()
                if skip == "1":
                    self.assertNotIn(SKIP_MESSAGE, res.stderr)
                    pc_speed = bench / "_workdir" / "pc_speed.json"
                    self.assertTrue(pc_speed.exists())
                    self.assertEqual(json.loads(pc_speed.read_text())["001-ok"]["median_ms"], 2)
                    self.assertIn("--pynecore", args)
                    self.assertEqual(args[args.index("--pynecore") + 1], str(pc_speed))
                    self.assertNotIn("--out", args)
                    self.assertEqual((bench / "results" / "speed.md").read_text(), "SPY TABLE\n")
                    self.assertTrue((bench / "_workdir" / "data" / "ETHUSDT_15.ohlcv").exists())
                    calls = [json.loads(line) for line in (self.root / "pyne.calls").read_text().splitlines()]
                    self.assertEqual(len(calls), 1)
                    self.assertIn("convert-from", calls[0])
                else:
                    self.assertIn(SKIP_MESSAGE, res.stderr)
                    self.assertFalse((bench / "_workdir" / "pc_speed.json").exists())
                    self.assertFalse((bench / "_workdir" / "pc_speed.err").exists())
                    self.assertFalse((bench / "_workdir" / "data" / "ETHUSDT_15.ohlcv").exists())
                    self.assertFalse((self.root / "pyne.calls").exists(), "pyne was called without a harness")
                    self.assertNotIn("--pynecore", args)
                    self.assertFalse(any(arg.startswith("--pynecore=") for arg in args), args)
                    self.assertFalse(any(arg.endswith("pc_speed.json") for arg in args), args)
                    # A run without PyneCore timings never overwrites the committed speed table.
                    self.assertEqual((bench / "results" / "speed.md").read_bytes(),
                                     b"COMMITTED SPEED TABLE\n")
                    self.assertEqual(args[args.index("--out") + 1], str(bench / "_workdir" / "speed.md"))
                    self.assertEqual((bench / "_workdir" / "speed.md").read_text(), "SPY TABLE\n")
                for engine in ("--pineforge", "--pinets", "--vectorbt"):
                    self.assertIn(engine, args)
                self.assertTrue((bench / "_workdir" / "trade_report.called").exists())
                # The committed canonical PyneCore indicator output is still there, so the three-way report runs.
                self.assertTrue((bench / "_workdir" / "indicator_report.called").exists())

    def test_absent_harness_without_committed_indicator_output_skips_three_way_report(self) -> None:
        ok = self.slot("001-ok")
        self.prepare_report_stubs()
        bench = self.root / "benchmarks"
        self.reset_report_case(ok)
        (self.strategies / "_indicators" / "canonical_pyne.csv").unlink()
        res = self.run_report_case(PYNECORE_HARNESS_DIR=None, SKIP_PYNE="0")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("PyneCore indicator output not present", res.stderr)
        self.assertFalse((bench / "_workdir" / "indicator_report.called").exists())
        self.assertTrue((bench / "_workdir" / "trade_report.called").exists())

    def test_present_pynecore_harness_receives_repo_root_and_supplies_timings(self) -> None:
        ok = self.slot("001-ok")
        self.prepare_report_stubs()
        bench = self.root / "benchmarks"
        for harness_dir in (str(self.harness), str(self.harness.relative_to(self.root))):
            with self.subTest(harness_dir=harness_dir):
                self.reset_report_case(ok)
                res = self.run_report_case(PYNECORE_HARNESS_DIR=harness_dir, SKIP_PYNE=None)
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertNotIn(SKIP_MESSAGE, res.stderr)
                self.assertIn("done.", res.stdout)
                self.assertEqual((self.root / "pynecore_repo_root.txt").read_text(), str(self.root))
                self.assert_output(ok, "pynecore_trades.csv", "fresh\n")
                self.assert_output(self.strategies / "_indicators", "canonical_pyne.csv", "fresh\n")
                pc_speed = bench / "_workdir" / "pc_speed.json"
                self.assertTrue(pc_speed.exists())
                self.assertEqual(json.loads(pc_speed.read_text())["001-ok"]["median_ms"], 2)
                args = (bench / "_workdir" / "aggregate_args.txt").read_text().splitlines()
                self.assertIn("--pynecore", args)
                self.assertEqual(args[args.index("--pynecore") + 1], str(pc_speed))
                # With the PyneCore timings the table is the published one and goes to its usual place.
                self.assertNotIn("--out", args)
                self.assertEqual((bench / "results" / "speed.md").read_text(), "SPY TABLE\n")
                self.assertTrue((bench / "_workdir" / "trade_report.called").exists())
                self.assertTrue((bench / "_workdir" / "indicator_report.called").exists())


if __name__ == "__main__":
    unittest.main()
