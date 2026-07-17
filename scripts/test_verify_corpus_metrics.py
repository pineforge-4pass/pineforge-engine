#!/usr/bin/env python3
"""Focused tests for fragmented-FIFO schedule metric eligibility."""

from __future__ import annotations

import unittest

from verify_corpus import (
    TradePair,
    has_cross_entry_fifo_allocation,
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


if __name__ == "__main__":
    unittest.main()
