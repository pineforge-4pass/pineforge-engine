#!/usr/bin/env python3
"""The engine's measurement ends where TradingView's range ends.

The engine books a position still open after its FINAL bar as TradingView's
range-end close (open_at_end, at that bar's close). That final bar must be
TV's last bar, not the feed's: the BINANCE:ETHUSDT.P 15 chart feed runs on
to 2026-05-04 15:00 UTC while every ETH tape ends at 2026-05-01 08:00 (+8) =
05-01 00:00 UTC (286 of 305 with an Open row there @ 2261.44 = that bar's
close). Unbounded, the row would be booked on 05-04 15:00 @ 2365.09, a
different bar ~4.6% off TV's, and fail exit/pnl against TV's Open row where
it was previously simply absent. run_strategy.py therefore bounds the feed
(``ohlcv_end_ms``) to the latest ``Date and time`` over every tape row —
entries, exits and the open-position row — on both runners.
"""

from __future__ import annotations

import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

import re

from run_strategy import EXPECTED_PF_ABI, TradeC, _load_bars, _load_tv_range_end_ms, _trim_ohlcv_csv

FIFTEEN_MIN = 15 * 60 * 1000


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


# The ETH chart feed: 15m bars from 04-30 22:00 UTC past the tape's end to
# 05-04 15:00 UTC. TV's last bar is 05-01 00:00 UTC (close 2261.44).
FEED_START = _utc_ms(2026, 4, 30, 22, 0)
TV_LAST_BAR = _utc_ms(2026, 5, 1, 0, 0)
FEED_END = _utc_ms(2026, 5, 4, 15, 0)


def _write_eth_feed(path: Path) -> list[int]:
    stamps = list(range(FEED_START, FEED_END + 1, FIFTEEN_MIN))
    with path.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for ts in stamps:
            close = 2261.44 if ts == TV_LAST_BAR else (2365.09 if ts == FEED_END else 2300.0)
            f.write(f"{ts},2300,2400,2200,{close},1\n")
    return stamps


def _write_tape(path: Path, rows: list[tuple[int, str, str, str, str]]) -> None:
    with path.open("w", encoding="utf-8-sig") as f:
        f.write("Trade number,Type,Date and time,Signal,Price USDT\n")
        for n, typ, when, signal, price in rows:
            f.write(f"{n},{typ},{when},{signal},{price}\n")


class LoadTvRangeEndTests(unittest.TestCase):
    def test_browser_tape_ends_on_the_open_row(self) -> None:
        # Taipei-stamped browser export: the Open row at 05-01 08:00 (+8)
        # is the latest stamp, later than any entry.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-04-29 10:15", "X", "2310.5"),
                (1, "Entry long", "2026-04-28 09:00", "A", "2290.0"),
                (2, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
                (2, "Entry long", "2026-04-30 20:45", "B", "2280.0"),
            ])
            self.assertEqual(_load_tv_range_end_ms(d, {"tv_trades_csv_tz": "utc_plus_8"}),
                             TV_LAST_BAR)

    def test_ws_tape_ends_on_the_last_exit(self) -> None:
        # ws-report-v1 (orb-lite NYSE:F 1D): the range-end row has no
        # Signal; its exit is the last bar, 2026-04-30 13:30 UTC.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit short", "2026-04-30 13:30", "", "12.08"),
                (1, "Entry short", "2026-03-16 13:30", "Short", "11.82"),
            ])
            self.assertEqual(_load_tv_range_end_ms(d, {"tv_trades_csv_tz": "utc"}),
                             _utc_ms(2026, 4, 30, 13, 30))

    def test_no_tape_or_no_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.assertIsNone(_load_tv_range_end_ms(d, {}))
            _write_tape(d / "tv_trades.csv", [])
            self.assertIsNone(_load_tv_range_end_ms(d, {"tv_trades_csv_tz": "utc"}))

    def test_meta_names_the_tape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "other.csv", [(1, "Exit long", "2026-05-01 00:00", "Open", "1"),
                                          (1, "Entry long", "2026-04-30 12:00", "A", "1")])
            self.assertEqual(
                _load_tv_range_end_ms(d, {"tv_trades_csv": "other.csv", "tv_trades_csv_tz": "utc"}),
                TV_LAST_BAR)


class HarnessAbiMirrorTests(unittest.TestCase):
    """The harness's ABI guard and its pf_trade_t mirror follow the header.

    verify-engine-local.py runs every probe through run_strategy.py, so a
    guard left at the previous ABI is a RuntimeError on every slug rather
    than a wrong number on one: the f-1d spark pre-check of the range-end
    change (2026-09-02) failed all 64 probes with ".so reports 3, harness
    expects 2" because only the docker / tutorial / benchmark mirrors had
    been bumped. Pin the constant to PF_ABI_VERSION as the header declares
    it, and the mirror's tail to the v3 field.
    """

    HEADER = Path(__file__).resolve().parents[1] / "include" / "pineforge" / "pineforge.h"

    def test_expected_abi_is_the_headers_macro(self) -> None:
        text = self.HEADER.read_text(encoding="utf-8")
        m = re.search(r"^#define PF_ABI_VERSION (\d+)\s*$", text, re.M)
        self.assertIsNotNone(m)
        assert m is not None
        self.assertEqual(EXPECTED_PF_ABI, int(m.group(1)))
        self.assertEqual(EXPECTED_PF_ABI, 3)

    def test_trade_mirror_ends_with_open_at_end(self) -> None:
        names = [name for name, _ in TradeC._fields_]
        self.assertEqual(names[-3:], ["entry_bar_index", "exit_bar_index", "open_at_end"])


class FeedBoundTests(unittest.TestCase):
    def test_ctypes_feed_ends_on_tv_last_bar(self) -> None:
        # The ETH shape: unbounded, the engine's last bar is 05-04 15:00;
        # bounded to the tape's end it is TV's last bar, close 2261.44.
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            stamps = _write_eth_feed(d / "feed.csv")
            _write_tape(d / "tv_trades.csv", [
                (1, "Exit long", "2026-05-01 08:00", "Open", "2261.44"),
                (1, "Entry long", "2026-04-30 22:15", "A", "2300.0"),
            ])
            end = _load_tv_range_end_ms(d, {"tv_trades_csv_tz": "utc_plus_8"})
            self.assertEqual(end, TV_LAST_BAR)
            full, n_full, sha_full = _load_bars(d / "feed.csv")
            self.assertEqual(n_full, len(stamps))
            self.assertEqual(full[n_full - 1].timestamp, FEED_END)
            bars, n, sha = _load_bars(d / "feed.csv", ohlcv_end_ms=end)
            self.assertEqual(bars[n - 1].timestamp, TV_LAST_BAR)
            self.assertAlmostEqual(bars[n - 1].close, 2261.44)
            self.assertEqual(n, stamps.index(TV_LAST_BAR) + 1)
            # The source identity is the full tape either way.
            self.assertEqual(sha, sha_full)

    def test_docker_pretrim_applies_both_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            stamps = _write_eth_feed(d / "feed.csv")
            start = stamps[3]
            trimmed = _trim_ohlcv_csv(d / "feed.csv", start, TV_LAST_BAR)
            self.assertIsNotNone(trimmed)
            assert trimmed is not None
            try:
                kept, n, _ = _load_bars(trimmed)
                self.assertEqual(kept[0].timestamp, start)
                self.assertEqual(kept[n - 1].timestamp, TV_LAST_BAR)
                self.assertEqual(n, stamps.index(TV_LAST_BAR) - 3 + 1)
            finally:
                trimmed.unlink()
            end_only = _trim_ohlcv_csv(d / "feed.csv", None, TV_LAST_BAR)
            assert end_only is not None
            try:
                kept, n, _ = _load_bars(end_only)
                self.assertEqual(kept[0].timestamp, FEED_START)
                self.assertEqual(kept[n - 1].timestamp, TV_LAST_BAR)
            finally:
                end_only.unlink()
            # No bound: no copy is made, the feed is used as given.
            self.assertIsNone(_trim_ohlcv_csv(d / "feed.csv", None, None))


if __name__ == "__main__":
    unittest.main()
