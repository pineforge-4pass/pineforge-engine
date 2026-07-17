#!/usr/bin/env python3
"""Focused tests for fragmented-FIFO schedule metric eligibility."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from verify_corpus import (
    TradePair,
    _apply_declared_tier_override,
    _print_verification,
    analyze_strategy,
    has_cross_entry_fifo_allocation,
    main,
    schedule_exit_metrics,
    schedule_rows_for_gate,
)


def trade(
    number: int,
    entry_time: int,
    entry_price: float,
    exit_time: int,
    exit_price: float,
    *,
    direction: str = "long",
    qty: float = 1.0,
    pnl: float = 0.0,
) -> TradePair:
    return TradePair(
        direction=direction,
        entry_time=entry_time,
        entry_price=entry_price,
        exit_time=exit_time,
        exit_price=exit_price,
        qty=qty,
        pnl=pnl,
        trade_num=number,
    )


class FragmentedFifoEligibilityTests(unittest.TestCase):
    def test_true_fifo_requires_split_entry_and_cross_entry_exit(self) -> None:
        rows = [
            trade(1, 100, 10.0, 200, 12.0, qty=0.5),
            trade(2, 100, 10.0, 300, 14.0, qty=0.5),
            trade(3, 150, 11.0, 300, 14.0, qty=1.0),
        ]
        self.assertTrue(has_cross_entry_fifo_allocation(rows))

    def test_margin_fragments_are_not_fifo_allocation(self) -> None:
        rows = [
            trade(1, 100, 10.0, 200, 9.0, qty=0.1),
            trade(2, 100, 10.0, 250, 8.0, qty=0.9),
            trade(3, 300, 11.0, 400, 12.0, qty=1.0),
        ]
        self.assertFalse(has_cross_entry_fifo_allocation(rows))

    def test_shared_exit_without_split_entry_needs_no_rescue(self) -> None:
        rows = [
            trade(1, 100, 10.0, 300, 14.0),
            trade(2, 150, 11.0, 300, 14.0),
        ]
        self.assertFalse(has_cross_entry_fifo_allocation(rows))

    def test_same_exit_qty_fragments_do_not_create_fifo_allocation(self) -> None:
        rows = [
            trade(1, 100, 10.0, 300, 14.0, qty=0.01),
            trade(2, 100, 10.0, 300, 14.0, qty=0.99),
            trade(3, 150, 11.0, 300, 14.0),
        ]
        self.assertFalse(has_cross_entry_fifo_allocation(rows))

    def test_unrelated_split_and_shared_exit_do_not_compose(self) -> None:
        rows = [
            # One isolated margin-fragmented entry.
            trade(1, 100, 10.0, 200, 9.0, qty=0.1),
            trade(2, 100, 10.0, 250, 8.0, qty=0.9),
            # A separate shared exit whose owners are not fragmented.
            trade(3, 300, 11.0, 500, 14.0),
            trade(4, 350, 12.0, 500, 14.0),
        ]
        self.assertFalse(has_cross_entry_fifo_allocation(rows))

    def test_schedule_rows_include_matched_counterpart_not_in_gate(self) -> None:
        tv_scored = trade(3, 100, 10.0, 200, 11.0)
        eng_scored = trade(3, 90, 10.0, 200, 11.0)
        tv_raw = [
            trade(1, 80, 8.0, 180, 9.0),
            trade(2, 95, 9.5, 195, 10.5),
            tv_scored,
            trade(4, 120, 12.0, 220, 13.0),
        ]
        eng_raw = [
            trade(1, 70, 7.0, 170, 8.0),
            eng_scored,
            trade(2, 95, 9.5, 195, 10.5),
            trade(4, 110, 12.0, 220, 13.0),
        ]

        tv_rows, eng_rows = schedule_rows_for_gate(
            tv_raw,
            eng_raw,
            [tv_scored],
            [],
            [(tv_scored, eng_scored)],
        )

        self.assertEqual([row.entry_time for row in tv_rows], [100])
        self.assertEqual([row.entry_time for row in eng_rows], [90])

    def test_empty_gate_keeps_complete_raw_schedules(self) -> None:
        tv_raw = [trade(1, 100, 10.0, 200, 11.0)]
        eng_raw = [trade(1, 90, 10.0, 200, 11.0)]
        tv_rows, eng_rows = schedule_rows_for_gate(tv_raw, eng_raw, [], [], [])
        self.assertIs(tv_rows, tv_raw)
        self.assertIs(eng_rows, eng_raw)

    def test_schedule_eligibility_ignores_fifo_outside_gate(self) -> None:
        tv_raw = [trade(1, 100, 10.0, 200, 12.0)]
        eng_raw = [
            # A genuine FIFO collision outside the scored window.
            trade(1, 10, 8.0, 20, 9.0, qty=0.5),
            trade(2, 10, 8.0, 30, 10.0, qty=0.5),
            trade(3, 15, 9.0, 30, 10.0),
            # The only engine row inside the scored window.
            trade(4, 100, 10.0, 200, 12.0),
        ]
        self.assertTrue(has_cross_entry_fifo_allocation(eng_raw))

        tv_rows, eng_rows = schedule_rows_for_gate(
            tv_raw,
            eng_raw,
            [tv_raw[-1]],
            [eng_raw[-1]],
            [(tv_raw[-1], eng_raw[-1])],
        )

        self.assertFalse(has_cross_entry_fifo_allocation(tv_rows))
        self.assertFalse(has_cross_entry_fifo_allocation(eng_rows))

    def test_schedule_metrics_penalize_engine_surplus_at_shared_exit(self) -> None:
        tv_raw = [trade(1, 100, 10.0, 200, 12.0, qty=1.0, pnl=2.0)]
        eng_raw = [trade(1, 100, 10.0, 200, 12.0, qty=2.0, pnl=2.0)]

        exit_deltas, pnl_deltas = schedule_exit_metrics(tv_raw, eng_raw)

        self.assertEqual(exit_deltas, [0.0, 1.0])
        self.assertEqual(pnl_deltas, [0.0])


class DeclaredTierOverrideTests(unittest.TestCase):
    CSV_HEADER = (
        "Trade #,Type,Date and time,Price,Position size (qty),Net P&L USD\n"
    )

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def strategy(
        self,
        name: str,
        meta: dict,
        *,
        tv_empty: bool = False,
        engine_empty: bool = False,
        aligned: bool = False,
    ) -> Path:
        strategy = self.root / name
        strategy.mkdir()
        (strategy / "strategy.pine").write_text(
            "//@version=6\nstrategy('zero alignment')\n",
            encoding="utf-8",
        )
        applied_meta = {**meta}
        if aligned:
            applied_meta["tv_trades_csv_tz"] = "utc"
        (strategy / "inputs.json").write_text(
            json.dumps(applied_meta), encoding="utf-8"
        )
        tv = self.CSV_HEADER
        engine = self.CSV_HEADER
        if not tv_empty:
            tv += (
                "1,Entry long,2025-01-01 00:00,100,1,0\n"
                "1,Exit long,2025-01-01 01:00,101,1,1\n"
            )
        if not engine_empty:
            engine_day = "2025-01-01" if aligned else "2025-01-03"
            engine += (
                f"1,Entry long,{engine_day} 00:00,100,1,0\n"
                f"1,Exit long,{engine_day} 01:00,101,1,1\n"
            )
        (strategy / "tv_trades.csv").write_text(tv, encoding="utf-8")
        (strategy / "engine_trades.csv").write_text(engine, encoding="utf-8")
        return strategy

    def test_zero_alignment_honors_declared_tiers_and_precedence(self) -> None:
        cases = (
            ("ordinary", {}, "minimal"),
            ("anomaly", {"expected_tier": "anomaly"}, "anomaly"),
            ("expected-engine", {"expected_tier": "engine_only"}, "engine_only"),
            (
                "override-engine",
                {"validation_overrides": {"expect_tv_match": False}},
                "engine_only",
            ),
            (
                "override-precedence",
                {
                    "expected_tier": "anomaly",
                    "validation_overrides": {"expect_tv_match": False},
                },
                "engine_only",
            ),
        )
        for name, meta, expected in cases:
            with self.subTest(name=name):
                result = analyze_strategy(self.strategy(name, meta))
                self.assertTrue(result.no_aligned_trades)
                self.assertEqual(result.label, expected)

                output = StringIO()
                with redirect_stdout(output):
                    _print_verification(result)
                self.assertIn(f"-> {expected}", output.getvalue())

    def test_both_empty_excellent_is_not_masked_by_stale_declarations(self) -> None:
        strategy = self.strategy(
            "both-empty",
            {
                "expected_tier": "anomaly",
                "validation_overrides": {"expect_tv_match": False},
            },
            tv_empty=True,
            engine_empty=True,
        )

        result = analyze_strategy(strategy)

        self.assertTrue(result.no_aligned_trades)
        self.assertEqual(result.label, "excellent")

    def test_single_sided_empty_results_honor_valid_declarations(self) -> None:
        anomaly = analyze_strategy(self.strategy(
            "tv-empty",
            {"expected_tier": "anomaly"},
            tv_empty=True,
        ))
        engine_only = analyze_strategy(self.strategy(
            "engine-empty",
            {"validation_overrides": {"expect_tv_match": False}},
            engine_empty=True,
        ))

        self.assertEqual((anomaly.tv_raw_count, anomaly.eng_raw_count), (0, 1))
        self.assertEqual(anomaly.label, "anomaly")
        self.assertEqual(
            (engine_only.tv_raw_count, engine_only.eng_raw_count), (1, 0)
        )
        self.assertEqual(engine_only.label, "engine_only")

    def test_matched_excellent_preserves_malformed_override_failure(self) -> None:
        strategy = self.strategy(
            "matched-malformed",
            {"validation_overrides": "oops"},
            aligned=True,
        )

        with self.assertRaisesRegex(AttributeError, "has no attribute 'get'"):
            analyze_strategy(strategy)

    def test_malformed_override_preserves_empty_branch_asymmetry(self) -> None:
        both_empty = self.strategy(
            "both-empty-malformed",
            {"validation_overrides": "oops"},
            tv_empty=True,
            engine_empty=True,
        )
        self.assertEqual(analyze_strategy(both_empty).label, "excellent")

        single_sided = (
            ("tv-empty-malformed", {"tv_empty": True}),
            ("engine-empty-malformed", {"engine_empty": True}),
        )
        for name, empty_side in single_sided:
            with self.subTest(name=name):
                strategy = self.strategy(
                    name,
                    {"validation_overrides": "oops"},
                    **empty_side,
                )
                with self.assertRaisesRegex(
                    AttributeError, "has no attribute 'get'"
                ):
                    analyze_strategy(strategy)

    def test_override_helper_preserves_normal_path_precedence(self) -> None:
        both = {
            "expected_tier": "anomaly",
            "validation_overrides": {"expect_tv_match": False},
        }
        self.assertEqual(
            _apply_declared_tier_override("weak", both), "engine_only"
        )
        self.assertEqual(
            _apply_declared_tier_override("weak", {"expected_tier": "anomaly"}),
            "anomaly",
        )
        self.assertEqual(
            _apply_declared_tier_override("excellent", both), "excellent"
        )

    def test_single_strategy_cli_exit_matches_declared_zero_alignment(self) -> None:
        cases = (
            ("cli-minimal", {}, 1, "minimal"),
            ("cli-anomaly", {"expected_tier": "anomaly"}, 0, "anomaly"),
            (
                "cli-engine-only",
                {"validation_overrides": {"expect_tv_match": False}},
                0,
                "engine_only",
            ),
        )
        for name, meta, expected_rc, expected_label in cases:
            strategy = self.strategy(name, meta)
            output = StringIO()
            with self.subTest(name=name), patch.object(
                sys, "argv", ["verify_corpus.py", str(strategy)]
            ), redirect_stdout(output):
                rc = main()

            self.assertEqual(rc, expected_rc)
            self.assertIn(f"-> {expected_label}", output.getvalue())


if __name__ == "__main__":
    unittest.main()
