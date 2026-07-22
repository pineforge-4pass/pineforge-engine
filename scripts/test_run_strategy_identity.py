#!/usr/bin/env python3
"""Focused CSV tests for physical closed-trade entry provenance."""

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

from pf_release_run import report_trades_to_runstrategy_shape
from run_strategy import write_engine_trades_csv


class EngineEntryIdentityCsvTests(unittest.TestCase):
    def test_writer_emits_incarnation_only_on_entry_row(self) -> None:
        trade = {
            "entry_time": 1_735_689_600_000,
            "exit_time": 1_735_689_660_000,
            "entry_price": 100.0,
            "exit_price": 101.0,
            "pnl": 1.0,
            "pnl_pct": 1.0,
            "is_long": True,
            "max_runup": 1.0,
            "max_drawdown": 0.0,
            "qty": 1.0,
            "entry_incarnation": 41,
        }

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "engine_trades.csv"
            write_engine_trades_csv([trade], path)
            with path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))

        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["Type"], "Exit long")
        self.assertEqual(rows[0]["Engine entry incarnation"], "")
        self.assertEqual(rows[1]["Type"], "Entry long")
        self.assertEqual(rows[1]["Engine entry incarnation"], "41")

    def test_legacy_trade_without_incarnation_stays_blank(self) -> None:
        trade = {
            "entry_time": 1_735_689_600_000,
            "exit_time": 1_735_689_660_000,
            "entry_price": 100.0,
            "exit_price": 100.0,
            "pnl": 0.0,
            "pnl_pct": 0.0,
            "is_long": False,
            "max_runup": 0.0,
            "max_drawdown": 0.0,
            "qty": 1.0,
        }

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "engine_trades.csv"
            write_engine_trades_csv([trade], path)
            with path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))

        self.assertTrue(all(
            row["Engine entry incarnation"] == "" for row in rows))

    def test_release_report_mapping_preserves_incarnation(self) -> None:
        mapped = report_trades_to_runstrategy_shape({"trades": [{
            "entry_time": 1,
            "exit_time": 2,
            "entry_price": 100.0,
            "exit_price": 101.0,
            "pnl": 1.0,
            "pnl_pct": 1.0,
            "side": "long",
            "max_runup": 1.0,
            "max_drawdown": 0.0,
            "qty": 1.0,
            "entry_incarnation": 73,
        }]})

        self.assertEqual(mapped[0]["entry_incarnation"], 73)


if __name__ == "__main__":
    unittest.main()
