#!/usr/bin/env python3
"""A lot still open at the range's end is marked twice, and the two marks
pair: a date-only TradingView tape writes its exit row with Signal "Open";
the engine, fed to TradingView's range end, books the same lot as its
range-end close (open_at_end) and marks the exit row in the trailing
``Engine range-end`` column of engine_trades.csv. The pair is a matched
trade — it counts, it covers, its entry is gated — and its exit and P&L are
gated nowhere: the rows leave both sides before consolidation, alignment,
the trim window, the schedule aggregate and every percentile, and the P&L
agreement is reported at TradingView's precision (cents) instead.

Why the P&L must stay out of the aggregates: TV's Net PnL is rounded to
cents and the engine's is exact (3commas-xlm #615: qty 0.0912, entry
2189.13, mark 2261.44 -> TV 6.35, engine 6.351137). On the three 3commas
grid bots (22/22/28 open lots) pnlP90 is scored by the fragmented-FIFO
schedule path as ONE aggregate |sum(tv) - sum(eng)| / |sum(tv)|, and the
marks' rounding summed into it moved the metric by itself (round 3,
2026-09-02: 0.5536 -> 0.6675 on xlm with 21 of 22 marks exact to the cent).

Why the exit must stay out: a reference Open row can carry the quote
at recording time, not a bar's print — all 12 sampled OANDA:EURUSD registry
tapes end with Open rows at 2026-05-01 08:00 (+8) @ 1.17256 while the
feed's last bar is o 1.17289 h 1.17299 l 1.17282 c 1.1729; the engine's
row is priced at that close, and the 0.029% is not parity information.

Unpaired marks are ordinary rows. A TV Open row the engine closed for real
on an earlier bar keeps its exit/pnl deltas (TV held to the end, the
engine did not); one the engine never opened leaves the coverage
denominator as before. An engine range-end row for a lot the tape closed
is matched by entry and gated like any close. A reference tape marks
nothing and prices the row at the last bar's close — the engine's row —
so it pairs as an ordinary closed trade and the grader is inert.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from verify_corpus import (
    ENGINE_RANGE_END_COLUMN,
    STRICT_EXIT_DELTA,
    TV_PNL_ROUNDING_EPSILON_USD,
    analyze_strategy,
    relative_max,
)

TV_HEADER = "Trade #,Type,Date and time,Signal,Price,Qty,Net PnL\n"
ENG_HEADER = ("Trade #,Type,Date and time,Price,Qty,Net PnL,"
              f"Engine entry incarnation,{ENGINE_RANGE_END_COLUMN}\n")
# The engine CSV as the baseline wrote it: no range-end column at all.
ENG_HEADER_LEGACY = "Trade #,Type,Date and time,Price,Qty,Net PnL\n"

# Trade 1 closes normally; trade 2 is open at the range's end.
TV_BROWSER = TV_HEADER + (
    "1,Exit long,2026-04-28 12:00,X,1.1700,1000,10\n"
    "1,Entry long,2026-04-27 12:00,A,1.1600,1000,10\n"
    "2,Exit long,2026-05-01 00:00,Open,1.17256,1000,-2.44\n"
    "2,Entry long,2026-04-30 12:00,B,1.1750,1000,-2.44\n"
)
TV_WS = TV_HEADER + (
    "1,Exit long,2026-04-28 12:00,X,1.1700,1000,10\n"
    "1,Entry long,2026-04-27 12:00,A,1.1600,1000,10\n"
    "2,Exit long,2026-05-01 00:00,,1.1729,1000,-2.1\n"
    "2,Entry long,2026-04-30 12:00,B,1.1750,1000,-2.1\n"
)
# The engine bounded at the range end: lot 2 marked on TV's last bar.
ENG_MARKED = ENG_HEADER + (
    "1,Exit long,2026-04-28 12:00,1.1700,1000,10,,\n"
    "1,Entry long,2026-04-27 12:00,1.1600,1000,10,,\n"
    "2,Exit long,2026-05-01 00:00,1.1729,1000,-2.1,,open\n"
    "2,Entry long,2026-04-30 12:00,1.1750,1000,-2.1,,\n"
)
# The engine closed lot 2 for real, on an earlier bar.
ENG_EARLIER_BAR = ENG_HEADER + (
    "1,Exit long,2026-04-28 12:00,1.1700,1000,10,,\n"
    "1,Entry long,2026-04-27 12:00,1.1600,1000,10,,\n"
    "2,Exit long,2026-04-30 18:00,1.1729,1000,-2.1,,\n"
    "2,Entry long,2026-04-30 12:00,1.1750,1000,-2.1,,\n"
)


def _analyze(tv_csv: str, eng_csv: str):
    with tempfile.TemporaryDirectory() as tmp:
        strategy = Path(tmp) / "eurusd-probe"
        strategy.mkdir()
        (strategy / "inputs.json").write_text(json.dumps({"tv_trades_csv_tz": "utc"}),
                                              encoding="utf-8")
        (strategy / "tv_trades.csv").write_text(tv_csv, encoding="utf-8")
        (strategy / "engine_trades.csv").write_text(eng_csv, encoding="utf-8")
        return analyze_strategy(strategy)


class OpenMarkPairTests(unittest.TestCase):
    def test_the_mark_itself_is_outside_strict_exit(self) -> None:
        self.assertGreater(relative_max(1.17256, 1.1729), STRICT_EXIT_DELTA)

    def test_marked_lot_pairs_with_the_engines_mark(self) -> None:
        r = _analyze(TV_BROWSER, ENG_MARKED)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.matched_count, 2)
        self.assertEqual((r.tv_count, r.eng_count), (2, 2))
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.gating_matched_count), (2, 2, 2))
        self.assertTrue(r.count_ok)
        self.assertEqual(r.count_abs_delta, 0)
        self.assertTrue(r.entry_ok)
        self.assertTrue(r.exit_ok)            # the mark's 0.029% is not gated
        self.assertEqual(r.exit_p90, 0.0)
        self.assertEqual(r.exit_deltas, [0.0])   # trade 1 only
        self.assertTrue(r.pnl_ok)
        self.assertEqual(r.pnl_deltas, [0.0])    # trade 1 only
        self.assertTrue(r.coverage_ok)
        self.assertEqual(r.coverage, 1.0)
        self.assertEqual(r.coverage_tv_count, 2)
        self.assertEqual(r.unmatched_in_window, 0)
        self.assertEqual(r.label, "excellent")
        # The pair's P&L is reported at cents: TV -2.44 against the engine's
        # -2.1 rounded is a 0.34 miss — visible, not gated.
        self.assertEqual(r.open_mark_pnl_cent_exact, 0)
        self.assertAlmostEqual(r.open_mark_pnl_max_abs_usd, 0.34)
        self.assertEqual([(t.trade_num, e.trade_num) for t, e in r.open_mark_matched], [(2, 2)])

    def test_marks_pair_on_the_lot_not_the_bar(self) -> None:
        # An engine mark booked on a later bar (a feed left unbounded) is
        # still the same lot's mark: paired, exit not gated.
        eng = ENG_MARKED.replace("2,Exit long,2026-05-01 00:00,1.1729",
                                 "2,Exit long,2026-05-04 15:00,1.2100")
        r = _analyze(TV_BROWSER, eng)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertTrue(r.exit_ok)
        self.assertEqual(r.label, "excellent")

    def test_mark_pnl_is_reported_at_cents(self) -> None:
        # The xlm #615 lot: TV 6.35, engine 6.351137 — exact at cents.
        tv = TV_BROWSER.replace("-2.44", "6.35")
        eng = ENG_MARKED.replace("1000,-2.1,,", "1000,6.351137,,")   # both rows of lot 2
        r = _analyze(tv, eng)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.open_mark_pnl_cent_exact, 1)
        self.assertLess(r.open_mark_pnl_max_abs_usd, TV_PNL_ROUNDING_EPSILON_USD)

    def test_open_row_closed_for_real_on_an_earlier_bar_keeps_its_deltas(self) -> None:
        r = _analyze(TV_BROWSER, ENG_EARLIER_BAR)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.matched_count, 2)
        self.assertFalse(r.exit_ok)
        self.assertNotEqual(r.label, "excellent")

    def test_legacy_engine_csv_without_the_column_pairs_nothing(self) -> None:
        # The baseline's CSV shape: the engine's row on the same bar is an
        # ordinary close against TV's Open row, and its exit is gated.
        eng = ENG_HEADER_LEGACY + (
            "1,Exit long,2026-04-28 12:00,1.1700,1000,10\n"
            "1,Entry long,2026-04-27 12:00,1.1600,1000,10\n"
            "2,Exit long,2026-05-01 00:00,1.1729,1000,-2.1\n"
            "2,Entry long,2026-04-30 12:00,1.1750,1000,-2.1\n"
        )
        r = _analyze(TV_BROWSER, eng)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.matched_count, 2)
        self.assertFalse(r.exit_ok)

    def test_ws_tape_pairs_the_engines_row_as_a_closed_trade(self) -> None:
        r = _analyze(TV_WS, ENG_MARKED)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.matched_count, 2)
        self.assertEqual(r.exit_deltas, [0.0, 0.0])
        self.assertTrue(r.exit_ok)
        self.assertTrue(r.pnl_ok)
        self.assertEqual(r.label, "excellent")

    def test_unmatched_open_row_still_leaves_the_coverage_denominator(self) -> None:
        # The engine never opened trade 2: the Open row is dropped from the
        # coverage denominator as before, and no pair is a mark pair.
        eng = ENG_HEADER + (
            "1,Exit long,2026-04-28 12:00,1.1700,1000,10,,\n"
            "1,Entry long,2026-04-27 12:00,1.1600,1000,10,,\n"
        )
        r = _analyze(TV_BROWSER, eng)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.coverage_tv_count, 1)
        self.assertEqual(r.label, "excellent")

    def test_engine_mark_without_a_tv_open_row_is_an_ordinary_engine_row(self) -> None:
        # TV closed lot 2 for real before the range end; the engine held it
        # and marked it. Matched by entry, the engine's row is gated like
        # any close: the exit differs, and the tier says so.
        tv = TV_HEADER + (
            "1,Exit long,2026-04-28 12:00,X,1.1700,1000,10\n"
            "1,Entry long,2026-04-27 12:00,A,1.1600,1000,10\n"
            "2,Exit long,2026-04-30 18:00,X,1.1800,1000,5\n"
            "2,Entry long,2026-04-30 12:00,B,1.1750,1000,5\n"
        )
        r = _analyze(tv, ENG_MARKED)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.matched_count, 2)
        self.assertFalse(r.exit_ok)
        self.assertNotEqual(r.label, "excellent")
        # TV never opened lot 2 at all (a later trade 3 keeps lot 2 inside
        # the common window): the engine's mark is an unmatched engine row.
        tv = TV_HEADER + (
            "1,Exit long,2026-04-28 12:00,X,1.1700,1000,10\n"
            "1,Entry long,2026-04-27 12:00,A,1.1600,1000,10\n"
            "3,Exit long,2026-04-30 15:00,X,1.1800,1000,5\n"
            "3,Entry long,2026-04-30 13:00,C,1.1750,1000,5\n"
        )
        eng = ENG_MARKED + (
            "3,Exit long,2026-04-30 15:00,1.1800,1000,5,,\n"
            "3,Entry long,2026-04-30 13:00,1.1750,1000,5,,\n"
        )
        r = _analyze(tv, eng)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.matched_count, 2)
        self.assertEqual((r.tv_gate_count, r.eng_gate_count), (2, 3))
        self.assertEqual(r.count_abs_delta, 1)
        self.assertFalse(r.count_ok)

    def test_a_tape_of_one_open_lot_is_one_matched_trade(self) -> None:
        tv = TV_HEADER + (
            "1,Exit long,2026-05-01 00:00,Open,1.17256,1000,-2.44\n"
            "1,Entry long,2026-04-30 12:00,B,1.1750,1000,-2.44\n"
        )
        eng = ENG_HEADER + (
            "1,Exit long,2026-05-01 00:00,1.1729,1000,-2.1,,open\n"
            "1,Entry long,2026-04-30 12:00,1.1750,1000,-2.1,,\n"
        )
        r = _analyze(tv, eng)
        self.assertFalse(r.no_aligned_trades)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.matched_count, 1)
        self.assertEqual(r.coverage, 1.0)
        self.assertEqual(r.label, "excellent")


# --- the grid-bot shape: the schedule aggregate ---------------------------
#
# A fragmented FIFO grid (the 3commas shape): entry E1 drains through two
# exits and exit X2 closes quantity owned by E1 and E2, which is what sends
# pnlP90 to the schedule path's one aggregate. E3 is half closed at X3 and
# half still open at the range end (mark M). The interior carries one real
# P&L miss (the E2/X2 lot: TV 3.10, engine 3.00) so the aggregate is not 0.

def _grid(tv: bool, *, mark_pnl: str, with_mark: bool = True) -> str:
    sig = (lambda s: s) if tv else (lambda s: "")
    rows = [
        # (trade#, exit time, exit price, entry time, entry price, qty, pnl, exit signal, entry signal)
        (1, "2026-04-20 03:00", "11", "2026-04-20 01:00", "10", "1", "1", "", "E1"),
        (2, "2026-04-20 04:00", "12", "2026-04-20 01:00", "10", "1", "2", "", "E1"),
        (3, "2026-04-20 04:00", "12", "2026-04-20 02:00", "9", "1", "3.10" if tv else "3.00", "", "E2"),
        (4, "2026-04-20 06:00", "10.5", "2026-04-20 05:00", "10", "1", "0.5", "", "E3"),
    ]
    if with_mark:
        rows.append((5, "2026-05-01 00:00", "10.2", "2026-04-20 05:00", "10", "1", mark_pnl,
                     "Open" if tv else "open", "E3"))
    out = TV_HEADER if tv else ENG_HEADER
    for n, xt, xp, et, ep, q, pnl, xsig, esig in rows:
        if tv:
            out += f"{n},Exit long,{xt},{xsig},{xp},{q},{pnl}\n"
            out += f"{n},Entry long,{et},{sig(esig)},{ep},{q},{pnl}\n"
        else:
            out += f"{n},Exit long,{xt},{xp},{q},{pnl},,{xsig}\n"
            out += f"{n},Entry long,{et},{ep},{q},{pnl},,\n"
    return out


class ScheduleAggregateTests(unittest.TestCase):
    # The interior aggregate: |(1+2+3.10+0.5) - (1+2+3.00+0.5)| / 6.60.
    INTERIOR = 0.10 / 6.60

    def test_the_shape_takes_the_schedule_path(self) -> None:
        r = _analyze(_grid(True, mark_pnl="0.20", with_mark=False),
                     _grid(False, mark_pnl="0.2041", with_mark=False))
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.entry_p90, 0.0)
        self.assertAlmostEqual(r.pnl_p90, self.INTERIOR)
        self.assertAlmostEqual(r.pnl_p100, self.INTERIOR)   # one aggregate, not a percentile

    def test_marks_do_not_move_the_aggregate(self) -> None:
        # The mark's rounding (TV 0.20, engine 0.2041) is out of the sum...
        r = _analyze(_grid(True, mark_pnl="0.20"), _grid(False, mark_pnl="0.2041"))
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.matched_count, 4)
        self.assertTrue(r.count_ok)
        self.assertEqual(r.coverage, 1.0)
        self.assertAlmostEqual(r.pnl_p90, self.INTERIOR)
        self.assertAlmostEqual(r.pnl_p100, self.INTERIOR)
        self.assertEqual(r.open_mark_pnl_cent_exact, 1)
        # ... and so is a mark that is plainly wrong: reported, not gated.
        r = _analyze(_grid(True, mark_pnl="0.20"), _grid(False, mark_pnl="5.00"))
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertAlmostEqual(r.pnl_p90, self.INTERIOR)
        self.assertEqual(r.open_mark_pnl_cent_exact, 0)
        self.assertAlmostEqual(r.open_mark_pnl_max_abs_usd, 4.80)

    def test_an_unpaired_mark_stays_in_the_aggregate(self) -> None:
        # The engine closed E3's second lot for real on an earlier bar: the
        # TV Open row and the engine's close are ordinary rows, both summed.
        eng = _grid(False, mark_pnl="0.2041").replace(
            "5,Exit long,2026-05-01 00:00,10.2,1,0.2041,,open",
            "5,Exit long,2026-04-25 00:00,10.2,1,0.2041,,")
        r = _analyze(_grid(True, mark_pnl="0.20"), eng)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertAlmostEqual(r.pnl_p90, abs(6.80 - 6.7041) / 6.80)


# --- the distinct-entry identity gate sees the marks -----------------------
#
# Two physical TV entries at ONE key (same bar, same price, same direction)
# with distinct Signals A and B prove a multi-entry key; B's lot is still
# open at the range's end. The Open row leaves the list as a mark pair, but
# the key it helped prove stays proven, and the engine's paired range-end
# row is held to the same identity requirement as any fill: without an
# entry incarnation the key fails closed, exactly as it did when the marks
# paired through the matcher (650b0cb).

TV_TWO_SIGNALS_ONE_KEY = TV_HEADER + (
    "1,Exit long,2026-04-28 12:00,X,1.1700,1000,10\n"
    "1,Entry long,2026-04-27 12:00,A,1.1600,1000,10\n"
    "2,Exit long,2026-05-01 00:00,Open,1.17256,1000,-2.44\n"
    "2,Entry long,2026-04-27 12:00,B,1.1600,1000,-2.44\n"
)
ENG_TWO_LOTS_NO_IDENTITY = ENG_HEADER + (
    "1,Exit long,2026-04-28 12:00,1.1700,1000,10,,\n"
    "1,Entry long,2026-04-27 12:00,1.1600,1000,10,,\n"
    "2,Exit long,2026-05-01 00:00,1.1729,1000,-2.1,,open\n"
    "2,Entry long,2026-04-27 12:00,1.1600,1000,-2.1,,\n"
)
ENG_TWO_LOTS_WITH_IDENTITY = ENG_HEADER + (
    "1,Exit long,2026-04-28 12:00,1.1700,1000,10,,\n"
    "1,Entry long,2026-04-27 12:00,1.1600,1000,10,1,\n"
    "2,Exit long,2026-05-01 00:00,1.1729,1000,-2.1,,open\n"
    "2,Entry long,2026-04-27 12:00,1.1600,1000,-2.1,2,\n"
)


class MarkPairIdentityGateTests(unittest.TestCase):
    def test_open_lot_still_proves_its_key_and_the_paired_mark_is_identity_checked(self) -> None:
        r = _analyze(TV_TWO_SIGNALS_ONE_KEY, ENG_TWO_LOTS_NO_IDENTITY)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.matched_count, 2)
        self.assertEqual(r.distinct_entry_mismatches, 1)
        self.assertFalse(r.distinct_entry_identity_ok)
        self.assertNotEqual(r.label, "excellent")
        self.assertIn("distinct-entry multiplicity", r.notes)

    def test_paired_mark_with_its_own_incarnation_satisfies_the_key(self) -> None:
        r = _analyze(TV_TWO_SIGNALS_ONE_KEY, ENG_TWO_LOTS_WITH_IDENTITY)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.matched_count, 2)
        self.assertEqual(r.distinct_entry_mismatches, 0)
        self.assertTrue(r.distinct_entry_identity_ok)
        self.assertEqual(r.label, "excellent")

    def test_the_pairing_matches_the_unpaired_grader_verdict(self) -> None:
        # The same tape graded as the baseline saw it -- the engine's row an
        # ordinary close, nothing paired -- reaches the same identity verdict.
        legacy = ENG_TWO_LOTS_NO_IDENTITY.replace(",,open\n", ",,\n")
        r_paired = _analyze(TV_TWO_SIGNALS_ONE_KEY, ENG_TWO_LOTS_NO_IDENTITY)
        r_plain = _analyze(TV_TWO_SIGNALS_ONE_KEY, legacy)
        self.assertEqual(r_plain.open_mark_pairs, 0)
        self.assertEqual((r_paired.distinct_entry_mismatches, r_paired.distinct_entry_identity_ok),
                         (r_plain.distinct_entry_mismatches, r_plain.distinct_entry_identity_ok))


if __name__ == "__main__":
    unittest.main()
