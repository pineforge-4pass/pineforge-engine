#!/usr/bin/env python3
"""Regression tests for the trail_* production parity profile."""

from __future__ import annotations

import csv
import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

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
    pnls: list[float | None],
    entry_prices: list[float] | None = None,
) -> None:
    if len(exit_prices) != len(pnls):
        raise ValueError("exit prices and PnL must describe the same trades")
    if entry_prices is not None and len(entry_prices) != len(exit_prices):
        raise ValueError("entry and exit prices must describe the same trades")
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
            entry_price = 100.0 if entry_prices is None else entry_prices[index - 1]
            writer.writerow(
                [index, "Entry long", entry_text, signal, entry_price, 1.0, 0.0, 0.0]
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
    tv_pnls: list[float | None],
    engine_pnls: list[float | None],
    metadata: dict | None = None,
    omit_tv: bool = False,
    omit_engine: bool = False,
    engine_entry_prices: list[float] | None = None,
):
    with tempfile.TemporaryDirectory(prefix="verify-corpus-trail-") as temporary:
        strategy = Path(temporary) / name
        strategy.mkdir()
        (strategy / "strategy.pine").write_text(
            strategy_source, encoding="utf-8"
        )
        if metadata is not None:
            (strategy / "inputs.json").write_text(
                json.dumps(metadata), encoding="utf-8"
            )
        if not omit_tv:
            _write_trades(
                strategy / "tv_trades.csv",
                tv_timezone=True,
                exit_prices=tv_exit_prices,
                pnls=tv_pnls,
            )
        if not omit_engine:
            _write_trades(
                strategy / "engine_trades.csv",
                tv_timezone=False,
                exit_prices=engine_exit_prices,
                pnls=engine_pnls,
                entry_prices=engine_entry_prices,
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
        row = result.report_row()
        self.assertAlmostEqual(row["exit_p100"], 0.0005)
        self.assertGreater(row["exit_p100"], row["exit_p90"])
        self.assertEqual(row["metric_observations"]["exit"], 100)
        self.assertFalse(row["metric_missing"]["exit"])

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


class GradingPolicyReportTests(unittest.TestCase):
    """Exercise report metadata from the analyzer, including its early returns."""

    def assert_metric_report(self, result) -> dict:
        row = result.report_row()
        counts = {}
        missing = {}
        for metric in ("entry", "exit", "pnl"):
            deltas = getattr(result, f"{metric}_deltas")
            counts[metric] = len(deltas)
            missing[metric] = not deltas
            self.assertEqual(
                row[f"{metric}_p100"],
                getattr(result, f"{metric}_p100") if deltas else None,
            )
        self.assertEqual(row["metric_observations"], counts)
        self.assertEqual(row["metric_missing"], missing)
        self.assertEqual(row["tv_exists"], result.tv_exists)
        self.assertEqual(row["eng_exists"], result.eng_exists)
        self.assertEqual(row["no_aligned_trades"], result.no_aligned_trades)
        self.assertEqual(json.loads(json.dumps(row)), row)
        return row

    def assert_policy(self, result, thresholds: dict) -> dict:
        row = self.assert_metric_report(result)
        self.assertEqual(row["tier"], result.tier)
        self.assertEqual(row["profile"], result.profile)
        self.assertIn("grading_policy", row)
        policy = row["grading_policy"]
        self.assertIsInstance(policy, dict)
        self.assertEqual(policy["profile"], result.profile)
        self.assertEqual(policy["thresholds"], thresholds)
        self.assertEqual(policy["aggregation"], "p90")
        self.assertEqual(policy["percentile"], 0.90)
        self.assertEqual(policy["scope"], "per_probe")
        self.assertEqual(policy["comparison"], "<")
        # The profile's nominal count fraction does not replace the exact
        # count gate required for excellent.
        self.assertEqual(policy["count_rule"], "exact")
        # The receipt names what the count counts: unmatched trades on both
        # sides, not the difference of the two totals.
        self.assertEqual(policy.get("count_basis"), "unmatched_both_sides")
        self.assertEqual(
            row["metric_observations"],
            {
                "entry": len(result.entry_deltas),
                "exit": len(result.exit_deltas),
                "pnl": len(result.pnl_deltas),
            },
        )
        json.dumps(row)
        return row

    def test_report_records_auto_and_forced_profiles_with_real_exit_grades(self):
        cases = (
            ("auto-trail", TRAIL_STRATEGY, {}, "production", 0.0002, "excellent"),
            ("auto-fixed", STRICT_STRATEGY, {}, "strict", 0.0001, "strong"),
            (
                "forced-strict", TRAIL_STRATEGY, {"parity_profile": "strict"},
                "strict", 0.0001, "strong",
            ),
            (
                "forced-production", STRICT_STRATEGY,
                {"parity_profile": "production"},
                "production", 0.0002, "excellent",
            ),
            (
                "explicit-auto-trail", TRAIL_STRATEGY,
                {"parity_profile": "auto"},
                "production", 0.0002, "excellent",
            ),
            (
                "explicit-auto-fixed", STRICT_STRATEGY,
                {"parity_profile": "auto"},
                "strict", 0.0001, "strong",
            ),
            (
                "unknown-falls-back-to-source", TRAIL_STRATEGY,
                {"parity_profile": "unknown"},
                "production", 0.0002, "excellent",
            ),
        )
        for name, source, metadata, profile, exit_limit, tier in cases:
            with self.subTest(name=name):
                result = _analyze(
                    name, source,
                    tv_exit_prices=[100.0],
                    engine_exit_prices=[100.015],
                    tv_pnls=[10.0],
                    engine_pnls=[10.0],
                    metadata=metadata,
                )

                self.assertEqual(result.profile, profile)
                self.assertEqual(result.tier, tier)
                self.assertEqual(result.exit_ok, tier == "excellent")
                self.assertTrue(result.pnl_ok)
                self.assert_policy(
                    result,
                    {
                        "count": 0.01, "entry": 0.0001,
                        "exit": exit_limit, "pnl": 0.01,
                    },
                )

    def test_report_retains_the_actual_threshold_snapshot(self):
        """Different resolved limits must change both grades and audit data."""
        narrow = {"count": 0.017, "entry": 0.0003, "exit": 0.0001, "pnl": 0.01}
        wider = {"count": 0.023, "entry": 0.0007, "exit": 0.00025, "pnl": 0.02}
        snapshots = [dict(narrow), dict(wider)]
        results = []
        with patch(
            "verify_corpus.parity_for_profile", side_effect=[narrow, wider]
        ) as resolve:
            for name in ("narrow-policy", "wider-policy"):
                results.append(_analyze(
                    name, TRAIL_STRATEGY,
                    tv_exit_prices=[100.0],
                    engine_exit_prices=[100.015],
                    tv_pnls=[10.0],
                    engine_pnls=[10.15],
                ))
            self.assertEqual(resolve.call_count, 2)
            self.assertEqual(
                [args.args for args in resolve.call_args_list],
                [("production",), ("production",)],
            )

        self.assertEqual([result.tier for result in results], ["strong", "excellent"])
        self.assertEqual([result.exit_ok for result in results], [False, True])
        self.assertEqual([result.pnl_ok for result in results], [False, True])
        # Reporting happens after the resolver is restored and its returned
        # dictionaries change. Neither a fresh lookup nor an aliased dict is
        # a receipt of the configuration that actually graded these trades.
        narrow["exit"] = 0.5
        wider["pnl"] = 0.75
        for result, thresholds in zip(results, snapshots):
            with self.subTest(tier=result.tier):
                self.assert_policy(result, thresholds)

    def test_report_policy_cannot_be_changed_by_a_report_consumer(self):
        thresholds = {"count": 0.01, "entry": 0.0001, "exit": 0.0002, "pnl": 0.01}
        result = _analyze(
            "report-consumer", TRAIL_STRATEGY,
            tv_exit_prices=[100.0],
            engine_exit_prices=[100.03],
            tv_pnls=[10.0],
            engine_pnls=[10.0],
        )
        self.assertEqual(result.tier, "strong")
        self.assertFalse(result.exit_ok)
        row = self.assert_policy(result, thresholds)
        row["grading_policy"]["thresholds"]["exit"] = 1.0
        row["metric_observations"]["pnl"] = 0
        row["metric_missing"]["entry"] = True
        row["exit_p100"] = 999.0
        self.assert_policy(result, thresholds)
        self.assertEqual(result.tier, "strong")
        self.assertFalse(result.exit_ok)

    def test_report_labels_p90_when_worst_trade_exceeds_the_limits(self):
        engine_exits = [100.0] * 100
        engine_pnls = [10.0] * 100
        engine_exits[41] = 100.05002501250625
        engine_pnls[41] = 11.0
        result = _analyze(
            "report-p90-tail", TRAIL_STRATEGY,
            tv_exit_prices=[100.0] * 100,
            engine_exit_prices=engine_exits,
            tv_pnls=[10.0] * 100,
            engine_pnls=engine_pnls,
        )
        self.assertGreater(result.exit_p100, 0.0002)
        self.assertGreater(result.pnl_p100, 0.01)
        self.assertEqual((result.exit_p90, result.pnl_p90), (0.0, 0.0))
        self.assertEqual(result.tier, "excellent")
        row = self.assert_policy(
            result,
            {"count": 0.01, "entry": 0.0001, "exit": 0.0002, "pnl": 0.01},
        )
        self.assertEqual(
            row["metric_observations"], {"entry": 100, "exit": 100, "pnl": 100}
        )
        self.assertAlmostEqual(row["exit_p100"], 0.0005)
        self.assertAlmostEqual(row["pnl_p100"], 0.1)
        self.assertGreater(row["exit_p100"], row["exit_p90"])
        self.assertGreater(row["pnl_p100"], row["pnl_p90"])

    def test_report_worst_values_follow_distinct_real_input_populations(self):
        for size, entry_price, exit_price, tv_pnl, engine_pnl in (
            (12, 100.04, 100.03, 20.0, 21.0),
            (23, 99.96, 99.95, -10.0, -7.5),
        ):
            with self.subTest(size=size):
                entries = [100.0] * size
                exits = [100.0] * size
                pnls = [tv_pnl] * size
                entries[2] = entry_price
                exits[5] = exit_price
                pnls[-1] = engine_pnl
                result = _analyze(
                    "varied-report-population", TRAIL_STRATEGY,
                    tv_exit_prices=[100.0] * size,
                    engine_exit_prices=exits,
                    tv_pnls=[tv_pnl] * size,
                    engine_pnls=pnls,
                    engine_entry_prices=entries,
                )
                row = self.assert_metric_report(result)
                self.assertEqual(
                    row["metric_observations"],
                    {"entry": size, "exit": size, "pnl": size},
                )
                expected = {
                    "entry": abs(100.0 - entry_price) / max(100.0, abs(entry_price)),
                    "exit": abs(100.0 - exit_price) / max(100.0, abs(exit_price)),
                    "pnl": abs(tv_pnl - engine_pnl) / abs(tv_pnl),
                }
                for metric, worst in expected.items():
                    self.assertEqual(row[f"{metric}_p90"], 0.0)
                    self.assertAlmostEqual(row[f"{metric}_p100"], worst)
                    self.assertGreater(row[f"{metric}_p100"], row[f"{metric}_p90"])

    def test_report_counts_filtered_pnl_independently_of_price_observations(self):
        result = _analyze(
            "different-metric-populations", TRAIL_STRATEGY,
            tv_exit_prices=[100.0] * 4,
            engine_exit_prices=[100.0, 100.04, 100.0, 100.05],
            tv_pnls=[0.0, 10.0, 0.005, -20.0],
            engine_pnls=[-1.0, 11.0, 7.0, -18.0],
        )
        row = self.assert_metric_report(result)
        self.assertEqual(result.matched_count, 4)
        self.assertEqual(row["metric_observations"], {"entry": 4, "exit": 4, "pnl": 2})
        self.assertEqual(
            row["metric_missing"], {"entry": False, "exit": False, "pnl": False}
        )
        self.assertEqual(row["entry_p100"], 0.0)
        self.assertAlmostEqual(row["exit_p100"], 0.05 / 100.05)
        self.assertAlmostEqual(row["pnl_p100"], 0.1)

    def test_report_distinguishes_no_eligible_pnl_from_observed_zero(self):
        for name, tv_pnls, engine_pnls, observations in (
            ("no-eligible-pnl", [0.0, 0.005], [-5.0, 1.0], 0),
            ("blank-pnl-cells", [None], [None], 0),
            ("observed-zero-delta", [10.0], [10.0], 1),
        ):
            with self.subTest(name=name):
                result = _analyze(
                    name, TRAIL_STRATEGY,
                    tv_exit_prices=[100.0] * len(tv_pnls),
                    engine_exit_prices=[100.0] * len(engine_pnls),
                    tv_pnls=tv_pnls,
                    engine_pnls=engine_pnls,
                )
                self.assertEqual(result.tier, "excellent")
                self.assertEqual(result.pnl_p90, 0.0)
                self.assertEqual(len(result.pnl_deltas), observations)
                row = self.assert_policy(
                    result,
                    {
                        "count": 0.01, "entry": 0.0001,
                        "exit": 0.0002, "pnl": 0.01,
                    },
                )
                self.assertEqual(row["metric_observations"]["pnl"], observations)
                self.assertEqual(row["metric_missing"]["pnl"], observations == 0)
                self.assertEqual(row["pnl_p100"], 0.0 if observations else None)

    def test_empty_rows_do_not_claim_an_applied_policy(self):
        for name, tv_exits, engine_exits, tier in (
            ("both-empty", [], [], "excellent"),
            ("tv-only", [100.0], [], "minimal"),
            ("engine-only", [], [100.0], "minimal"),
        ):
            with self.subTest(name=name):
                result = _analyze(
                    name, TRAIL_STRATEGY,
                    tv_exit_prices=tv_exits,
                    engine_exit_prices=engine_exits,
                    tv_pnls=[10.0] * len(tv_exits),
                    engine_pnls=[10.0] * len(engine_exits),
                )
                self.assertTrue(result.no_aligned_trades)
                self.assertEqual(result.profile, "production")
                self.assertEqual(result.tier, tier)
                row = self.assert_metric_report(result)
                self.assertIn("grading_policy", row)
                self.assertIsNone(row["grading_policy"])
                self.assertEqual(
                    row["metric_observations"], {"entry": 0, "exit": 0, "pnl": 0}
                )

    def test_missing_tapes_do_not_invent_an_applied_policy(self):
        for omit_tv, omit_engine in ((True, False), (False, True), (True, True)):
            with self.subTest(omit_tv=omit_tv, omit_engine=omit_engine):
                result = _analyze(
                    "missing-tape", TRAIL_STRATEGY,
                    tv_exit_prices=[100.0],
                    engine_exit_prices=[100.0],
                    tv_pnls=[10.0],
                    engine_pnls=[10.0],
                    omit_tv=omit_tv,
                    omit_engine=omit_engine,
                )
                self.assertEqual(result.tier, "missing")
                self.assertEqual(result.profile, "n/a")
                self.assertEqual((result.tv_exists, result.eng_exists),
                                 (not omit_tv, not omit_engine))
                row = self.assert_metric_report(result)
                self.assertIn("grading_policy", row)
                self.assertIsNone(row["grading_policy"])
                self.assertEqual(
                    row["metric_observations"], {"entry": 0, "exit": 0, "pnl": 0}
                )

    def test_failed_analysis_is_not_replaced_by_a_report_row(self):
        with patch(
            "verify_corpus.parity_for_profile",
            side_effect=ValueError("deliberate policy resolution failure"),
        ):
            with self.assertRaisesRegex(
                ValueError, "deliberate policy resolution failure"
            ):
                _analyze(
                    "failed-analysis", TRAIL_STRATEGY,
                    tv_exit_prices=[100.0],
                    engine_exit_prices=[100.0],
                    tv_pnls=[10.0],
                    engine_pnls=[10.0],
                )


if __name__ == "__main__":
    unittest.main()
