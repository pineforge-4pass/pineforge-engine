#!/usr/bin/env python3
"""A browser export's "Open" row is a mark, not a fill: when the engine closes
the same position on the same bar (its range-end close, open_at_end), the
pair counts and covers but its exit price / PnL are not gated.

Evidence: all 12 sampled OANDA:EURUSD registry tapes end with Signal="Open"
rows at 2026-05-01 08:00 (+8) @ 1.17256, while the feed's last bar (05-01
00:00 UTC) is o 1.17289 h 1.17299 l 1.17282 c 1.1729 — the export-time quote,
outside the bar entirely. The engine's row is priced at the bar's close,
1.1729; the 0.029% delta exceeds STRICT_EXIT_DELTA (0.01%) and would fail
exit_ok on every such probe for a number that carries no parity information.
An engine exit on a DIFFERENT bar is a real divergence (TV held to the end,
the engine did not) and keeps its deltas. ws-report-v1 tapes write the row
with no Signal and the last bar's close, and never take this path.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from verify_corpus import STRICT_EXIT_DELTA, analyze_strategy, relative_max

TV_HEADER = "Trade #,Type,Date and time,Signal,Price,Qty,Net PnL\n"
ENG_HEADER = "Trade #,Type,Date and time,Price,Qty,Net PnL\n"

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
ENG_SAME_BAR = ENG_HEADER + (
    "1,Exit long,2026-04-28 12:00,1.1700,1000,10\n"
    "1,Entry long,2026-04-27 12:00,1.1600,1000,10\n"
    "2,Exit long,2026-05-01 00:00,1.1729,1000,-2.1\n"
    "2,Entry long,2026-04-30 12:00,1.1750,1000,-2.1\n"
)
ENG_EARLIER_BAR = ENG_HEADER + (
    "1,Exit long,2026-04-28 12:00,1.1700,1000,10\n"
    "1,Entry long,2026-04-27 12:00,1.1600,1000,10\n"
    "2,Exit long,2026-04-30 18:00,1.1729,1000,-2.1\n"
    "2,Entry long,2026-04-30 12:00,1.1750,1000,-2.1\n"
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

    def test_open_row_closed_on_the_same_bar_is_count_only(self) -> None:
        r = _analyze(TV_BROWSER, ENG_SAME_BAR)
        self.assertEqual(r.open_mark_pairs, 1)
        self.assertEqual(r.matched_count, 2)
        self.assertTrue(r.count_ok)
        self.assertEqual(r.count_abs_delta, 0)
        self.assertTrue(r.entry_ok)
        self.assertTrue(r.exit_ok)            # pre-fix: 0.029% > 0.01%
        self.assertEqual(r.exit_p90, 0.0)
        self.assertTrue(r.pnl_ok)
        self.assertTrue(r.coverage_ok)
        self.assertEqual(r.coverage, 1.0)
        self.assertEqual(r.label, "excellent")

    def test_open_row_closed_on_an_earlier_bar_keeps_its_deltas(self) -> None:
        r = _analyze(TV_BROWSER, ENG_EARLIER_BAR)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.matched_count, 2)
        self.assertFalse(r.exit_ok)
        self.assertNotEqual(r.label, "excellent")

    def test_ws_tape_row_never_takes_the_path(self) -> None:
        r = _analyze(TV_WS, ENG_SAME_BAR)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertTrue(r.exit_ok)
        self.assertEqual(r.label, "excellent")

    def test_unmatched_open_row_still_leaves_the_coverage_denominator(self) -> None:
        # The engine never opened trade 2: the Open row is dropped from the
        # coverage denominator as before, and no pair is an open-mark pair.
        eng = ENG_HEADER + (
            "1,Exit long,2026-04-28 12:00,1.1700,1000,10\n"
            "1,Entry long,2026-04-27 12:00,1.1600,1000,10\n"
        )
        r = _analyze(TV_BROWSER, eng)
        self.assertEqual(r.open_mark_pairs, 0)
        self.assertEqual(r.coverage_tv_count, 1)


if __name__ == "__main__":
    unittest.main()
