#!/usr/bin/env python3
"""The TV-entry emit / trade-start window opens ON TradingView's first entry
bar; the one-bar pad for the signal bar lives in the engine, by index on
the feed (compute_trade_start_preceding_script_bar / trading_is_active).

orb-lite on NYSE:F 1D: the short fires on the 2026-03-13 (Friday) bar and TV
fills it on 2026-03-16 (Monday), its first entry. The validator used to open
the window one bar interval early — ``Monday - 1 day`` is Sunday, not a feed
bar — and the engine's own one-script-TF buffer reached Saturday, so Friday's
strategy.entry was dropped. Padding here as well as in the engine would admit
TWO bars before the tape (a first cut opened the window on the previous FEED
bar, Friday, and the engine then admitted Thursday), so the window now starts
on the entry itself and the engine admits exactly the bar before it.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from run_strategy import _load_tv_entry_window

DAY = 86_400_000
FIFTEEN_MIN = 15 * 60 * 1000


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


def _write_tv_trades(path: Path, entry_times: list[str], *,
                     exit_times: list[str] | None = None) -> None:
    exits = exit_times or entry_times
    with path.open("w", encoding="utf-8-sig") as f:
        f.write("Trade number,Type,Date and time,Signal,Price USD\n")
        for n, (t, x) in enumerate(zip(entry_times, exits), 1):
            f.write(f"{n},Exit short,{x},,12.08\n")
            f.write(f"{n},Entry short,{t},Short,11.82\n")


# NYSE 1D bars are labelled at the 13:30 UTC session open.
FRI = _utc_ms(2026, 3, 13, 13, 30)
MON = _utc_ms(2026, 3, 16, 13, 30)
WED = _utc_ms(2026, 3, 18, 13, 30)


class LoadTvEntryWindowTests(unittest.TestCase):
    def test_daily_tape_window_opens_on_the_first_entry(self) -> None:
        # orb-lite: first TV entry Monday 03-16. The window starts THERE —
        # not on Sunday (the old interval pad) and not on Friday (the
        # first cut's previous-feed-bar pad); the engine admits Friday.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tv_trades(d / "tv_trades.csv", ["2026-03-16 13:30", "2026-03-18 13:30"],
                             exit_times=["2026-03-17 13:30", "2026-04-30 13:30"])
            window = _load_tv_entry_window(d, {"tv_trades_csv_tz": "utc"})
            self.assertEqual(window, (MON, WED))
            assert window is not None
            self.assertNotEqual(window[0], MON - DAY)
            self.assertNotEqual(window[0], FRI)

    def test_window_end_is_the_last_entry_not_the_last_exit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tv_trades(d / "tv_trades.csv", ["2026-03-16 13:30"],
                             exit_times=["2026-04-30 13:30"])
            self.assertEqual(_load_tv_entry_window(d, {"tv_trades_csv_tz": "utc"}), (MON, MON))

    def test_intraday_tape_is_unchanged_in_shape(self) -> None:
        base = _utc_ms(2026, 3, 16, 0, 0)
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tv_trades(d / "tv_trades.csv", ["2026-03-16 01:45", "2026-03-16 03:00"])
            self.assertEqual(_load_tv_entry_window(d, {"tv_trades_csv_tz": "utc"}),
                             (base + 7 * FIFTEEN_MIN, base + 12 * FIFTEEN_MIN))

    def test_tape_timezone_is_honoured(self) -> None:
        # Taipei-stamped tape (the browser exports): 21:30 +8 is 13:30 UTC.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tv_trades(d / "tv_trades.csv", ["2026-03-16 21:30"])
            self.assertEqual(_load_tv_entry_window(d, {"tv_trades_csv_tz": "utc_plus_8"}),
                             (MON, MON))

    def test_no_tape_returns_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(_load_tv_entry_window(Path(tmp), {}))

    def test_tape_without_entries_returns_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tv_trades(d / "tv_trades.csv", [])
            self.assertIsNone(_load_tv_entry_window(d, {"tv_trades_csv_tz": "utc"}))

    def test_meta_names_the_tape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tv_trades(d / "other.csv", ["2026-03-16 13:30"])
            meta = {"tv_trades_csv": "other.csv", "tv_trades_csv_tz": "utc"}
            self.assertEqual(_load_tv_entry_window(d, meta), (MON, MON))
            self.assertEqual(json.dumps(meta), json.dumps(meta))  # meta untouched


if __name__ == "__main__":
    unittest.main()
