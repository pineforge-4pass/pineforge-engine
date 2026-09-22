#!/usr/bin/env python3
"""compare.py grades a slot whose last engine run left an error log n/a.

run_all.sh leaves <slot>/_<engine>_error.log when an engine run fails. A trade
list beside that log is not this run's output (an earlier run's, or a partial
one), so compare.py must report the slot n/a with the error, not grade the
list. The scratch slot's trade list is TradingView's own tape, which grades
excellent when no error log is present (the positive control).

    python3 benchmarks/tests/test_compare.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "benchmarks"))
import compare  # noqa: E402

TAPE = """\
Trade number,Type,Date and time,Signal,Price USDT,Size (qty),Size (value),Net PnL USDT
1,Exit long,2025-01-02 06:00,Exit,3400,1,3400,10
1,Entry long,2025-01-02 00:45,Long,3390,1,3390,10
2,Exit short,2025-01-03 21:30,Exit,3380,1,3380,5
2,Entry short,2025-01-03 07:45,Short,3385,1,3385,5
3,Exit long,2025-01-04 12:00,Exit,3420,1,3420,20
3,Entry long,2025-01-04 02:15,Long,3400,1,3400,20
"""


class ErrorLogGradesNa(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="bench-compare-test-")
        self.addCleanup(tmp.cleanup)
        self.scratch = Path(tmp.name) / "scratch"
        self.slot = Path(tmp.name) / "001-scratch-slot"
        self.slot.mkdir()
        (self.slot / "strategy.pine").write_text("//@version=6\n")
        (self.slot / "generated.cpp").write_text("")
        (self.slot / "strategy_pyne.py").write_text("")
        (self.slot / "tv_trades.csv").write_text(TAPE)
        (self.slot / "inputs.json").write_text('{"tv_trades_csv_tz": "utc"}\n')
        for csv in ("pineforge_trades.csv", "pynecore_trades.csv"):
            (self.slot / csv).write_text(TAPE)

    def grade(self, engine: str, csv: str) -> "compare.Grade":
        return compare.grade(self.slot, engine, csv, self.scratch, {})

    def test_trade_list_without_error_log_is_graded(self) -> None:
        g = self.grade("PineForge", "pineforge_trades.csv")
        self.assertEqual(compare.label_of(g), "excellent")

    def test_pineforge_error_log_grades_na(self) -> None:
        (self.slot / "_pineforge_error.log").write_text(
            "Traceback (most recent call last):\nFileNotFoundError: feed missing\n")
        g = self.grade("PineForge", "pineforge_trades.csv")
        self.assertEqual(compare.label_of(g), "n/a",
                         f"graded as measured: {compare.cell(g)}")
        self.assertEqual(g.reason, "run error: FileNotFoundError: feed missing")

    def test_pynecore_error_log_grades_na(self) -> None:
        (self.slot / "_pynecore_error.log").write_text("RuntimeError: security failed\n")
        g = self.grade("PyneCore", "pynecore_trades.csv")
        self.assertEqual(compare.label_of(g), "n/a",
                         f"graded as measured: {compare.cell(g)}")
        self.assertEqual(g.reason, "PyneCore runtime error: RuntimeError: security failed")


if __name__ == "__main__":
    unittest.main()
