#!/usr/bin/env python3
"""Focused CSV tests for physical closed-trade entry provenance."""

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

from pf_release_run import report_trades_to_runstrategy_shape
from run_strategy import format_trade_qty, write_engine_trades_csv


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


class EngineTradeQtyFormatTests(unittest.TestCase):
    """The exported ``Qty`` column must carry the ledger quantity to the lot
    step: ``%g``'s 6 significant digits truncated (and sometimes rounded UP)
    every OANDA:EURUSD-scale lot (TV KI-52 all-in: 92293.36 units)."""

    def test_large_lots_keep_every_lot_step_digit(self) -> None:
        self.assertEqual(format_trade_qty(923941.16), "923941.16")
        self.assertEqual(format_trade_qty(897902.68), "897902.68")
        self.assertEqual(format_trade_qty(92293.36), "92293.36")
        self.assertEqual(format_trade_qty(1000000.0), "1000000")
        self.assertEqual(format_trade_qty(8741.59), "8741.59")

    def test_binary_noise_and_trailing_zeros_are_trimmed(self) -> None:
        self.assertEqual(format_trade_qty(0.30000000000000004), "0.3")
        self.assertEqual(format_trade_qty(2.7051000000000001), "2.7051")
        self.assertEqual(format_trade_qty(1.0), "1")
        self.assertEqual(format_trade_qty(44.0), "44")
        self.assertEqual(format_trade_qty(0.0001), "0.0001")
        self.assertEqual(format_trade_qty(0.0), "0")

    def test_values_the_old_formatter_rendered_exactly_are_unchanged(self) -> None:
        for qty in (55.2872, 5.4103, 0.0196, 44.9622, 48, 7.7232, 30.3796):
            self.assertEqual(format_trade_qty(float(qty)), f"{float(qty):g}")

    def test_writer_uses_lot_faithful_qty(self) -> None:
        trade = {
            "entry_time": 1_735_689_600_000,
            "exit_time": 1_735_689_660_000,
            "entry_price": 1.08232,
            "exit_price": 1.07836,
            "pnl": -3658.81,
            "pnl_pct": -0.37,
            "is_long": True,
            "max_runup": 0.0,
            "max_drawdown": 0.0,
            "qty": 923941.16,
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "engine_trades.csv"
            write_engine_trades_csv([trade], path)
            with path.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
        self.assertEqual([row["Qty"] for row in rows], ["923941.16", "923941.16"])


if __name__ == "__main__":
    unittest.main()
