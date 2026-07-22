#!/usr/bin/env python3
"""Focused tests for fragmented-FIFO schedule metric eligibility."""

from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from datetime import timezone
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from verify_corpus import (
    STRICT_ENTRY_DELTA,
    TradePair,
    _apply_declared_tier_override,
    _print_verification,
    analyze_strategy,
    consolidate_fragments,
    distinct_entry_fill_keys,
    distinct_entry_fill_mismatches,
    has_cross_entry_fifo_allocation,
    main,
    parse_trades,
    relative_max,
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
    entry_signal: str = "",
    entry_identity: str = "",
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
        entry_signal=entry_signal,
        entry_identity=entry_identity,
    )


class FragmentConsolidationIdentityTests(unittest.TestCase):
    def test_distinct_tv_entry_signals_preserve_same_tick_trades(self) -> None:
        rows = [
            trade(221, 100, 4627.73, 100, 4627.73,
                  entry_signal="Long"),
            trade(222, 100, 4627.73, 100, 4627.73,
                  entry_signal="Close entry(s) order Short"),
        ]

        self.assertEqual(len(consolidate_fragments(rows)), 2)
        self.assertEqual(
            distinct_entry_fill_keys(rows), {(100, 4627.73, "long")})

    def test_engine_fragments_share_one_physical_identity(self) -> None:
        tv_rows = [
            trade(221, 100, 4627.73, 100, 4627.73,
                  entry_signal="Long"),
            trade(222, 100, 4627.73, 100, 4627.73,
                  entry_signal="Close entry(s) order Short"),
        ]
        engine_rows = [
            trade(221, 100, 4627.73, 100, 4627.73,
                  entry_identity="41"),
            trade(222, 100, 4627.73, 100, 4627.73,
                  entry_identity="41"),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        self.assertEqual(
            len(consolidate_fragments(
                engine_rows, preserve_entry_keys=preserve,
                identity_field="entry_identity")), 1)
        self.assertEqual(
            distinct_entry_fill_mismatches(tv_rows, engine_rows, preserve), 1)

    def test_direction_substitution_fails_distinct_entry_identity(self) -> None:
        tv_rows = [
            trade(221, 100, 4627.73, 100, 4627.73,
                  entry_signal="Long"),
            trade(222, 100, 4627.73, 100, 4627.73,
                  entry_signal="Close entry(s) order Short"),
        ]
        engine_rows = [
            trade(221, 100, 4627.73, 100, 4627.73),
            trade(222, 100, 4627.73, 100, 4627.73, direction="short"),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        self.assertEqual(
            distinct_entry_fill_mismatches(tv_rows, engine_rows, preserve), 1)

    def test_distinct_engine_incarnations_pass_identity_gate(self) -> None:
        tv_rows = [
            trade(221, 100, 4627.73, 100, 4627.73,
                  entry_signal="Long"),
            trade(222, 100, 4627.73, 100, 4627.73,
                  entry_signal="Close entry(s) order Short"),
        ]
        engine_rows = [
            trade(221, 100, 4627.73, 100, 4627.73,
                  entry_identity="41"),
            trade(222, 100, 4627.73, 100, 4627.73,
                  entry_identity="42"),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        self.assertEqual(
            distinct_entry_fill_mismatches(tv_rows, engine_rows, preserve), 0)

    def test_tolerant_projection_preserves_fragmented_multiplicity(self) -> None:
        tv_rows = [
            trade(1, 100, 100.0, 200, 102.0, qty=0.25,
                  entry_signal="A"),
            trade(2, 100, 100.0, 250, 103.0, qty=0.75,
                  entry_signal="A"),
            trade(3, 100, 100.0, 300, 104.0, qty=1.0,
                  entry_signal="B"),
        ]
        engine_rows = [
            trade(1, 100, 100.000001, 200, 102.0, qty=0.25,
                  entry_identity="41"),
            trade(2, 100, 100.000001, 250, 103.0, qty=0.75,
                  entry_identity="41"),
            trade(3, 100, 100.000001, 300, 104.0, qty=1.0,
                  entry_identity="42"),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        consolidated = consolidate_fragments(
            engine_rows, preserve_entry_keys=preserve,
            identity_field="entry_identity")

        self.assertEqual(len(consolidated), 2)
        self.assertEqual(
            [row.entry_identity for row in consolidated], ["41", "42"])
        self.assertEqual(
            distinct_entry_fill_mismatches(
                tv_rows, consolidated, preserve), 0)

    def test_tolerant_projection_rejects_just_outside_price(self) -> None:
        tv_rows = [
            trade(1, 100, 100.0, 200, 102.0, entry_signal="A"),
            trade(2, 100, 100.0, 200, 102.0, entry_signal="B"),
        ]
        engine_rows = [
            trade(1, 100, 100.0101, 200, 102.0, entry_identity="41"),
            trade(2, 100, 100.0101, 200, 102.0, entry_identity="42"),
        ]

        self.assertGreater(
            relative_max(100.0, 100.0101), STRICT_ENTRY_DELTA)
        preserve = distinct_entry_fill_keys(tv_rows)
        self.assertEqual(
            distinct_entry_fill_mismatches(
                tv_rows, engine_rows, preserve), 1)

    def test_tolerant_projection_uses_strict_boundary(self) -> None:
        tv_rows = [
            trade(1, 100, 100.0, 200, 102.0, entry_signal="A"),
            trade(2, 100, 100.0, 200, 102.0, entry_signal="B"),
        ]
        last_inside = 100.0 / (1.0 - STRICT_ENTRY_DELTA)
        first_outside = math.nextafter(last_inside, math.inf)
        self.assertLess(
            relative_max(100.0, last_inside), STRICT_ENTRY_DELTA)
        self.assertGreaterEqual(
            relative_max(100.0, first_outside), STRICT_ENTRY_DELTA)
        preserve = distinct_entry_fill_keys(tv_rows)

        def mismatches_at(price: float) -> int:
            engine_rows = [
                trade(1, 100, price, 200, 102.0, entry_identity="41"),
                trade(2, 100, price, 200, 102.0, entry_identity="42"),
            ]
            return distinct_entry_fill_mismatches(
                tv_rows, engine_rows, preserve)

        self.assertEqual(mismatches_at(last_inside), 0)
        self.assertEqual(mismatches_at(first_outside), 1)

    def test_tolerant_projection_fails_closed_on_non_transitive_overlap(
        self,
    ) -> None:
        low, middle, high = 100.0, 100.009, 100.018
        self.assertLess(relative_max(low, middle), STRICT_ENTRY_DELTA)
        self.assertLess(relative_max(middle, high), STRICT_ENTRY_DELTA)
        self.assertGreater(relative_max(low, high), STRICT_ENTRY_DELTA)
        tv_rows = [
            trade(1, 100, low, 200, 102.0, entry_signal="A1"),
            trade(2, 100, low, 200, 102.0, entry_signal="A2"),
            trade(3, 100, high, 200, 102.0, entry_signal="B1"),
            trade(4, 100, high, 200, 102.0, entry_signal="B2"),
        ]
        engine_rows = [
            trade(1, 100, middle, 200, 102.0, entry_identity="41"),
            trade(2, 100, middle, 200, 102.0, entry_identity="42"),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        consolidated = consolidate_fragments(
            engine_rows, preserve_entry_keys=preserve,
            identity_field="entry_identity")

        self.assertEqual(len(consolidated), 2)
        self.assertEqual(
            distinct_entry_fill_mismatches(
                tv_rows, consolidated, preserve), 2)

    def test_exact_distinct_prices_win_inside_overlapping_tolerance(
        self,
    ) -> None:
        low, high = 100.0, 100.005
        self.assertLess(relative_max(low, high), STRICT_ENTRY_DELTA)
        tv_rows = [
            trade(1, 100, low, 200, 102.0, entry_signal="A1"),
            trade(2, 100, low, 200, 102.0, entry_signal="A2"),
            trade(3, 100, high, 200, 102.0, entry_signal="B1"),
            trade(4, 100, high, 200, 102.0, entry_signal="B2"),
        ]
        engine_rows = [
            trade(1, 100, low, 200, 102.0, entry_identity="41"),
            trade(2, 100, low, 200, 102.0, entry_identity="42"),
            trade(3, 100, high, 200, 102.0, entry_identity="43"),
            trade(4, 100, high, 200, 102.0, entry_identity="44"),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        consolidated = consolidate_fragments(
            engine_rows, preserve_entry_keys=preserve,
            identity_field="entry_identity")

        self.assertEqual(len(preserve), 2)
        self.assertEqual(len(consolidated), 4)
        self.assertEqual(
            distinct_entry_fill_mismatches(
                tv_rows, consolidated, preserve), 0)

    def test_tolerant_projection_keeps_time_and_direction_exact(self) -> None:
        tv_rows = [
            trade(1, 100, 100.0, 200, 102.0, entry_signal="A"),
            trade(2, 100, 100.0, 200, 102.0, entry_signal="B"),
        ]
        preserve = distinct_entry_fill_keys(tv_rows)

        for entry_time, direction in [(101, "long"), (100, "short")]:
            with self.subTest(entry_time=entry_time, direction=direction):
                engine_rows = [
                    trade(1, entry_time, 100.000001, 200, 102.0,
                          direction=direction, entry_identity="41"),
                    trade(2, entry_time, 100.000001, 200, 102.0,
                          direction=direction, entry_identity="42"),
                ]
                self.assertEqual(
                    distinct_entry_fill_mismatches(
                        tv_rows, engine_rows, preserve), 1)

    def test_missing_engine_identity_fails_closed_despite_matching_rows(self) -> None:
        tv_rows = [
            trade(1, 100, 10.0, 200, 12.0, qty=0.25,
                  entry_signal="A"),
            trade(2, 100, 10.0, 200, 12.0, qty=0.75,
                  entry_signal="A"),
            trade(3, 100, 10.0, 200, 12.0, qty=1.0,
                  entry_signal="B"),
        ]
        engine_rows = [
            trade(1, 100, 10.0, 200, 12.0, qty=1.0),
            trade(2, 100, 10.0, 200, 12.0, qty=1.0),
        ]

        preserve = distinct_entry_fill_keys(tv_rows)
        self.assertEqual(
            distinct_entry_fill_mismatches(tv_rows, engine_rows, preserve), 1)

    def test_same_signal_fragments_still_merge(self) -> None:
        rows = [
            trade(1, 100, 10.0, 200, 12.0, qty=0.25,
                  entry_signal="Long"),
            trade(2, 100, 10.0, 200, 12.0, qty=0.75,
                  entry_signal="Long"),
        ]

        merged = consolidate_fragments(rows)
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].qty, 1.0)
        self.assertEqual(merged[0].entry_signal, "Long")

    def test_mixed_unique_and_repeated_signals_preserve_physical_buckets(self) -> None:
        rows = [
            trade(1, 100, 10.0, 200, 12.0, qty=0.25,
                  entry_signal="Grid_L35"),
            trade(2, 100, 10.0, 200, 12.0, qty=0.75,
                  entry_signal="Grid_L35"),
            trade(3, 100, 10.0, 200, 12.0, qty=1.0,
                  entry_signal="Grid_L36"),
        ]

        self.assertEqual(
            distinct_entry_fill_keys(rows), {(100, 10.0, "long")})
        merged = consolidate_fragments(rows)
        self.assertEqual(len(merged), 2)
        self.assertEqual([trade.entry_signal for trade in merged],
                         ["Grid_L35", "Grid_L36"])
        self.assertEqual([trade.qty for trade in merged], [1.0, 1.0])

    def test_mixed_signal_identity_cannot_false_certify_one_engine_row(self) -> None:
        tv_rows = [
            trade(1, 100, 10.0, 200, 12.0, qty=0.25,
                  entry_signal="Grid_L35"),
            trade(2, 100, 10.0, 200, 12.0, qty=0.75,
                  entry_signal="Grid_L35"),
            trade(3, 100, 10.0, 200, 12.0, qty=1.0,
                  entry_signal="Grid_L36"),
        ]
        engine_rows = [trade(1, 100, 10.0, 200, 12.0, qty=2.0)]

        preserve = distinct_entry_fill_keys(tv_rows)
        tv_consolidated = consolidate_fragments(
            tv_rows, preserve_entry_keys=preserve)
        engine_consolidated = consolidate_fragments(
            engine_rows, preserve_entry_keys=preserve)

        self.assertEqual(len(tv_consolidated), 2)
        self.assertEqual(len(engine_consolidated), 1)
        self.assertEqual(
            distinct_entry_fill_mismatches(
                tv_consolidated, engine_consolidated, preserve),
            1,
        )

    def test_parser_retains_entry_side_signal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "trades.csv"
            path.write_text(
                "Trade #,Type,Date and time,Signal,Price,Qty,Net PnL\n"
                "1,Exit long,2025-01-01 00:00,Short,100,1,0\n"
                "1,Entry long,2025-01-01 00:00,"
                "Close entry(s) order Short,100,1,0\n",
                encoding="utf-8",
            )

            parsed = parse_trades(path, tz=timezone.utc)

        self.assertEqual(len(parsed), 1)
        self.assertEqual(
            parsed[0].entry_signal, "Close entry(s) order Short")

    def test_parser_retains_engine_entry_incarnation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "engine_trades.csv"
            path.write_text(
                "Trade #,Type,Date and time,Price,Qty,Net PnL,"
                "Engine entry incarnation\n"
                "1,Exit long,2025-01-01 00:00,100,1,0,41\n"
                "1,Entry long,2025-01-01 00:00,100,1,0,41\n",
                encoding="utf-8",
            )

            parsed = parse_trades(path, tz=timezone.utc)

        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0].entry_identity, "41")

    def test_end_to_end_engine_identity_matrix(self) -> None:
        tv_csv = (
            "Trade #,Type,Date and time,Signal,Price,Qty,Net PnL\n"
            "1,Exit long,2025-08-15 12:45,X,100,0.5,0\n"
            "1,Entry long,2025-08-15 12:45,A,100,0.5,0\n"
            "2,Exit long,2025-08-15 12:45,X,100,0.5,0\n"
            "2,Entry long,2025-08-15 12:45,A,100,0.5,0\n"
            "3,Exit long,2025-08-15 12:45,X,100,1,0\n"
            "3,Entry long,2025-08-15 12:45,B,100,1,0\n"
        )
        scenarios = {
            "same-incarnation": (
                "Trade #,Type,Date and time,Price,Qty,Net PnL,"
                "Engine entry incarnation\n"
                "1,Exit long,2025-08-15 12:45,100,0.5,0,41\n"
                "1,Entry long,2025-08-15 12:45,100,0.5,0,41\n"
                "2,Exit long,2025-08-15 12:45,100,0.5,0,41\n"
                "2,Entry long,2025-08-15 12:45,100,0.5,0,41\n",
                "weak", 1, False, 1,
            ),
            "distinct-incarnations": (
                "Trade #,Type,Date and time,Price,Qty,Net PnL,"
                "Engine entry incarnation\n"
                "1,Exit long,2025-08-15 12:45,100,1,0,41\n"
                "1,Entry long,2025-08-15 12:45,100,1,0,41\n"
                "2,Exit long,2025-08-15 12:45,100,1,0,42\n"
                "2,Entry long,2025-08-15 12:45,100,1,0,42\n",
                "excellent", 0, True, 0,
            ),
            "tolerant-distinct-incarnations": (
                "Trade #,Type,Date and time,Price,Qty,Net PnL,"
                "Engine entry incarnation\n"
                "1,Exit long,2025-08-15 12:45,100,1,0,41\n"
                "1,Entry long,2025-08-15 12:45,100.000001,1,0,41\n"
                "2,Exit long,2025-08-15 12:45,100,1,0,42\n"
                "2,Entry long,2025-08-15 12:45,100.000001,1,0,42\n",
                "excellent", 0, True, 0,
            ),
            "missing-identity": (
                "Trade #,Type,Date and time,Price,Qty,Net PnL\n"
                "1,Exit long,2025-08-15 12:45,100,1,0\n"
                "1,Entry long,2025-08-15 12:45,100,1,0\n"
                "2,Exit long,2025-08-15 12:45,100,1,0\n"
                "2,Entry long,2025-08-15 12:45,100,1,0\n",
                "strong", 0, False, 1,
            ),
        }

        for name, (
            engine_csv, expected_label, expected_count_delta,
            expected_identity_ok, expected_identity_delta,
        ) in scenarios.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                strategy = Path(tmp) / name
                strategy.mkdir()
                (strategy / "inputs.json").write_text(
                    json.dumps({"tv_trades_csv_tz": "utc"}),
                    encoding="utf-8",
                )
                (strategy / "tv_trades.csv").write_text(
                    tv_csv, encoding="utf-8")
                (strategy / "engine_trades.csv").write_text(
                    engine_csv, encoding="utf-8")

                result = analyze_strategy(strategy)

            self.assertEqual(result.label, expected_label)
            self.assertEqual(result.count_abs_delta, expected_count_delta)
            self.assertEqual(
                result.distinct_entry_identity_ok, expected_identity_ok)
            self.assertEqual(
                result.distinct_entry_mismatches, expected_identity_delta)

    def test_identity_collision_outside_declared_interior_is_excluded(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            strategy = Path(tmp) / "outside-interior"
            strategy.mkdir()
            (strategy / "inputs.json").write_text(
                json.dumps({"tv_trades_csv_tz": "utc", "trim_bars": 1}),
                encoding="utf-8",
            )
            (strategy / "ohlcv.csv").write_text(
                "timestamp,open\n"
                "1735689600,1\n"
                "1735693200,1\n"
                "1735696800,1\n"
                "1735700400,1\n"
                "1735704000,1\n",
                encoding="utf-8",
            )
            (strategy / "tv_trades.csv").write_text(
                "Trade #,Type,Date and time,Signal,Price,Qty,Net PnL\n"
                "1,Exit long,2025-01-01 00:00,X,100,0.5,0\n"
                "1,Entry long,2025-01-01 00:00,A,100,0.5,0\n"
                "2,Exit long,2025-01-01 00:00,X,100,0.5,0\n"
                "2,Entry long,2025-01-01 00:00,A,100,0.5,0\n"
                "3,Exit long,2025-01-01 00:00,X,100,1,0\n"
                "3,Entry long,2025-01-01 00:00,B,100,1,0\n",
                encoding="utf-8",
            )
            (strategy / "engine_trades.csv").write_text(
                "Trade #,Type,Date and time,Price,Qty,Net PnL\n"
                "1,Exit long,2025-01-01 00:00,100,1,0\n"
                "1,Entry long,2025-01-01 00:00,100,1,0\n"
                "2,Exit long,2025-01-01 00:00,100,1,0\n"
                "2,Entry long,2025-01-01 00:00,100,1,0\n",
                encoding="utf-8",
            )

            result = analyze_strategy(strategy)

        self.assertIsNotNone(result.bounds)
        self.assertEqual(result.tv_gate_count, 0)
        self.assertEqual(result.eng_gate_count, 0)
        self.assertTrue(result.distinct_entry_identity_ok)
        self.assertEqual(result.distinct_entry_mismatches, 0)
        self.assertEqual(result.label, "excellent")

    def test_identity_gate_blocks_false_excellent_direction_substitution(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            strategy = Path(tmp) / "identity-substitution"
            strategy.mkdir()
            (strategy / "inputs.json").write_text(
                json.dumps({"tv_trades_csv_tz": "utc"}),
                encoding="utf-8",
            )
            (strategy / "tv_trades.csv").write_text(
                "Trade #,Type,Date and time,Signal,Price,Qty,Net PnL\n"
                "1,Exit long,2025-08-15 12:45,Short,100,1,0\n"
                "1,Entry long,2025-08-15 12:45,Long,100,1,0\n"
                "2,Exit long,2025-08-15 12:45,Short,100,1,0\n"
                "2,Entry long,2025-08-15 12:45,"
                "Close entry(s) order Short,100,1,0\n",
                encoding="utf-8",
            )
            (strategy / "engine_trades.csv").write_text(
                "Trade #,Type,Date and time,Price,Qty,Net PnL\n"
                "1,Exit long,2025-08-15 12:45,100,1,0\n"
                "1,Entry long,2025-08-15 12:45,100,1,0\n"
                "2,Exit short,2025-08-15 12:45,100,1,0\n"
                "2,Entry short,2025-08-15 12:45,100,1,0\n",
                encoding="utf-8",
            )

            result = analyze_strategy(strategy)

        self.assertEqual(result.tv_count, 2)
        self.assertEqual(result.eng_count, 2)
        self.assertEqual(result.count_abs_delta, 0)
        self.assertEqual(result.unmatched_in_window, 1)
        self.assertTrue(result.coverage_ok)
        self.assertFalse(result.distinct_entry_identity_ok)
        self.assertEqual(result.distinct_entry_mismatches, 1)
        self.assertEqual(result.label, "strong")


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
