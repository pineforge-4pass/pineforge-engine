#!/usr/bin/env python3
"""Regression tests for the trail_* production parity profile."""

from __future__ import annotations

import csv
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from verify_corpus import analyze_strategy, parity_for_profile


TRAIL_STRATEGY = """//@version=6
strategy("synthetic trail profile")
strategy.entry("L", strategy.long)
strategy.exit("trail", from_entry="L", trail_points=1)
"""

STRICT_STRATEGY = """//@version=6
strategy("synthetic strict profile")
strategy.entry("L", strategy.long)
strategy.exit("fixed", from_entry="L", limit=101)
"""

TRADE_COLUMNS = [
    "Trade #",
    "Type",
    "Date and time",
    "Signal",
    "Price",
    "Qty",
    "Net PnL",
    "Net PnL %",
]


def _write_trades(
    path: Path,
    *,
    tv_timezone: bool,
    exit_prices: list[float],
    pnls: list[float],
) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(TRADE_COLUMNS)
        for index, (exit_price, pnl) in enumerate(zip(exit_prices, pnls), start=1):
            entry = datetime(2025, 1, 1, tzinfo=timezone.utc) + timedelta(
                days=index - 1
            )
            if tv_timezone:
                entry += timedelta(hours=8)
            exit_time = entry + timedelta(hours=1)
            entry_text = entry.strftime("%Y-%m-%d %H:%M")
            exit_text = exit_time.strftime("%Y-%m-%d %H:%M")
            signal = f"entry-{index}" if tv_timezone else ""
            writer.writerow(
                [index, "Entry long", entry_text, signal, 100.0, 1.0, 0.0, 0.0]
            )
            writer.writerow(
                [index, "Exit long", exit_text, "", exit_price, 1.0, pnl, 0.0]
            )


def _analyze(
    name: str,
    strategy_source: str,
    *,
    tv_exit_prices: list[float],
    engine_exit_prices: list[float],
    tv_pnls: list[float],
    engine_pnls: list[float],
):
    with tempfile.TemporaryDirectory(prefix="verify-corpus-trail-") as temporary:
        strategy = Path(temporary) / name
        strategy.mkdir()
        (strategy / "strategy.pine").write_text(
            strategy_source, encoding="utf-8"
        )
        _write_trades(
            strategy / "tv_trades.csv",
            tv_timezone=True,
            exit_prices=tv_exit_prices,
            pnls=tv_pnls,
        )
        _write_trades(
            strategy / "engine_trades.csv",
            tv_timezone=False,
            exit_prices=engine_exit_prices,
            pnls=engine_pnls,
        )
        return analyze_strategy(strategy)


class TrailProductionProfileTests(unittest.TestCase):
    def test_trail_exit_at_three_basis_points_fails_new_production_exit_gate(self):
        """TV exit 100.0 vs engine 100.03 is about 0.03% by relative_max.

        The TV P&L is 10.0 and the engine P&L is 10.05, so the real grader
        reports a 0.5% P&L delta after its exit-coupling check. The exit p90
        is above the new 0.0002 fraction while the P&L p90 remains below
        0.01. This assertion is red on the old 0.0005 production limit.
        """
        result = _analyze(
            "trail-exit-0030pct",
            TRAIL_STRATEGY,
            tv_exit_prices=[100.0],
            engine_exit_prices=[100.03],
            tv_pnls=[10.0],
            engine_pnls=[10.05],
        )

        self.assertEqual(result.profile, "production")
        self.assertFalse(result.exit_ok)
        self.assertTrue(result.pnl_ok)
        self.assertEqual(result.tier, "strong")
        self.assertIn("exit p90", result.notes)

    def test_trail_exit_at_one_and_a_half_basis_points_passes_new_limits(self):
        """TV exit 100.0 vs engine 100.015 is about 0.015%.

        The same 10.0 to 10.05 P&L pair produces a 0.5% P&L p90, so both
        production gates are below the new fractions 0.0002 and 0.01.
        """
        result = _analyze(
            "trail-exit-0015pct",
            TRAIL_STRATEGY,
            tv_exit_prices=[100.0],
            engine_exit_prices=[100.015],
            tv_pnls=[10.0],
            engine_pnls=[10.05],
        )

        self.assertEqual(result.profile, "production")
        self.assertTrue(result.exit_ok)
        self.assertTrue(result.pnl_ok)
        self.assertEqual(result.tier, "excellent")

    def test_trail_two_percent_pnl_fails_new_production_pnl_gate(self):
        """Exact exits with TV P&L 10.0 and engine P&L 10.2 produce 2%.

        The exit p90 is zero, while the P&L p90 is 0.02. The old production
        P&L limit of 1.0 lets this pass; the new 0.01 fraction must fail it.
        """
        result = _analyze(
            "trail-pnl-0020",
            TRAIL_STRATEGY,
            tv_exit_prices=[100.0],
            engine_exit_prices=[100.0],
            tv_pnls=[10.0],
            engine_pnls=[10.2],
        )

        self.assertEqual(result.profile, "production")
        self.assertTrue(result.exit_ok)
        self.assertFalse(result.pnl_ok)
        self.assertEqual(result.tier, "strong")
        self.assertIn("pnl p90", result.notes)

    def test_one_large_trail_tail_is_hidden_by_the_real_p90_rule(self):
        """One 0.05% outlier among 100 exits has an interpolated p90 of zero.

        Ninety-nine trades use 100.0 on both sides. One uses
        100.05002501250625 on the engine side; with the grader's
        max-price denominator this is a 0.0005 exit delta. The sorted p90
        interpolation lands between the 89th and 90th zero-valued entries,
        so the real grader must pass both old and new production profiles.
        """
        tv_exit_prices = [100.0] * 100
        engine_exit_prices = [100.0] * 100
        engine_exit_prices[41] = 100.05002501250625
        result = _analyze(
            "trail-p90-tail",
            TRAIL_STRATEGY,
            tv_exit_prices=tv_exit_prices,
            engine_exit_prices=engine_exit_prices,
            tv_pnls=[10.0] * 100,
            engine_pnls=[10.0] * 100,
        )

        self.assertGreater(max(result.exit_deltas), 0.0004)
        self.assertEqual(result.exit_p90, 0.0)
        self.assertTrue(result.exit_ok)
        self.assertTrue(result.pnl_ok)
        self.assertEqual(result.tier, "excellent")

    def test_non_trail_exit_at_one_and_a_half_basis_points_stays_strict(self):
        """A non-trail 0.015% exit remains above strict's 0.0001 fraction."""
        result = _analyze(
            "strict-exit-0015pct",
            STRICT_STRATEGY,
            tv_exit_prices=[100.0],
            engine_exit_prices=[100.015],
            tv_pnls=[10.0],
            engine_pnls=[10.0],
        )

        self.assertEqual(result.profile, "strict")
        self.assertFalse(result.exit_ok)
        self.assertTrue(result.pnl_ok)
        self.assertEqual(result.tier, "strong")

    def test_profile_thresholds_return_new_production_and_unchanged_strict_values(self):
        """The direct threshold pin is a constant check, not grade evidence."""
        result = _analyze(
            "threshold-direct-pin",
            TRAIL_STRATEGY,
            tv_exit_prices=[100.0],
            engine_exit_prices=[100.0],
            tv_pnls=[10.0],
            engine_pnls=[10.0],
        )

        self.assertEqual(result.profile, "production")
        self.assertEqual(
            parity_for_profile("production"),
            {"count": 0.01, "entry": 0.0001, "exit": 0.0002, "pnl": 0.01},
        )
        self.assertEqual(
            parity_for_profile("strict"),
            {"count": 0.01, "entry": 0.0001, "exit": 0.0001, "pnl": 0.01},
        )


if __name__ == "__main__":
    unittest.main()
