#!/usr/bin/env python3
"""The emit window starts on the chart-feed bar that precedes TradingView's
first entry bar.

run_strategy.py gates the engine's strategy commands (``trade_start_time``)
at the start of the TV tape's emit window and used to put that start at
``first TV entry - one bar interval`` (the feed's first-row gap), assuming
the bar the first entry was placed on sits one interval before the bar it
filled on. Across a weekend, a holiday or an overnight session break it does
not: the signal bar fell behind the gate (and its one-script-TF buffer,
engine_strategy_commands.cpp trading_is_active), the strategy.entry was
dropped, and six single-entry tapes graded no-trades on every ladder
candidate (round 7, ledger log-20260905t054904z-a9baf07e).

The start is now the loaded feed bar before the bar at or just before the
first TV entry — walked over the feed the engine is fed, after the probe's
ohlcv_start_ms / range-end bounds — whenever that is earlier than the old
start, never later (_tv_entry_emit_window). Trades keep being reported
from the old bound: the first entry's fill on TV's first entry bar is
written, a fill on the signal bar itself (which TV did not report) is not,
and on a gapless feed nothing changes at all.

Lane RUN-HARNESS: TradingView's deep backtest computes the script from its
first bar (metrics.json wsProvenance.returnedRange.from) and no earlier one,
its broker live from there, so an order placed hours before the signal bar
can make TV's first fill (tests/fixtures/run_harness_window). A run whose
feed starts on that bar opens its window there -- unless it enters before
TV's first entry, where TV filled nothing, when it runs again from the
signal bar; a warmed or later-starting feed keeps the signal bar.
"""

from __future__ import annotations

import contextlib
import csv
import io
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

import run_strategy
from run_strategy import (
    TvEntryWindow,
    _describe_tv_entry_window,
    _feed_timestamps,
    _filter_trades_to_window,
    _infer_bar_interval_ms,
    _load_bars,
    _load_tv_entry_span,
    _tv_entry_emit_window,
)

MIN15 = 15 * 60 * 1000
HOUR = 60 * MIN15 // 15
DAY = 24 * HOUR


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


def _utc(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def _taipei(ms: int) -> str:
    """A tape stamp as the campaign's verifier writes it (utc_plus_8)."""
    tz = timezone(timedelta(hours=8))
    return datetime.fromtimestamp(ms / 1000.0, tz=tz).strftime("%Y-%m-%d %H:%M")


def _weekday_stamps(first: int, last: int, hh: int, mm: int) -> list[int]:
    """One bar per weekday from ``first`` to ``last`` (UTC dates) at hh:mm UTC."""
    out = []
    day = datetime.fromtimestamp(first / 1000.0, tz=timezone.utc)
    end = datetime.fromtimestamp(last / 1000.0, tz=timezone.utc)
    while day <= end:
        if day.weekday() < 5:
            out.append(_utc_ms(day.year, day.month, day.day, hh, mm))
        day += timedelta(days=1)
    return out


def _rth_15m_stamps(days: list[tuple[int, int, int]]) -> list[int]:
    """NASDAQ regular-hours 15m bars, 13:30..19:45 UTC, for each (y, m, d)."""
    out = []
    for y, m, d in days:
        start = _utc_ms(y, m, d, 13, 30)
        out.extend(range(start, _utc_ms(y, m, d, 19, 45) + 1, MIN15))
    return out


def _write_feed(path: Path, stamps: list[int]) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for ts in stamps:
            f.write(f"{ts},10,11,9,10.5,100\n")


def _write_tape(path: Path, rows: list[tuple[int, str, str, str, str]]) -> None:
    with path.open("w", encoding="utf-8-sig") as f:
        f.write("Trade number,Type,Date and time,Signal,Price USD\n")
        for n, typ, when, signal, price in rows:
            f.write(f"{n},{typ},{when},{signal},{price}\n")


# The ws-report-v1 export's range: every campaign tape runs 2025-04-01 to
# 2026-05-01, so the range-end bound never cuts the feeds below.
WS_METRICS = {
    "symbol": "NYSE:F", "interval": "1D",
    "from": "2025-04-01", "to": "2026-05-01",
    "deepBacktesting": True, "tapeChannel": "ws-report-v1",
    "wsProvenance": {
        "schemaVersion": 1,
        "requestedRange": {"from": 1743465600000, "to": 1777593600000},
        "returnedRange": {"from": 1743514200000, "to": 1777555800000},
        "rangeProof": "covered",
    },
}
RANGE_TO_MS = 1777593600000


def _trade(entry_ms: int, exit_ms: int, *, is_long: bool = True) -> dict:
    return {
        "is_long": is_long, "entry_time": entry_ms, "exit_time": exit_ms,
        "entry_price": 10.0, "exit_price": 10.5, "qty": 1.0,
        "pnl": 0.5, "pnl_pct": 5.0, "max_runup": 0.5, "max_drawdown": 0.0,
        "commission": 0.0, "entry_bar_index": 0, "exit_bar_index": 1,
        "open_at_end": False,
    }


class _FakeStrategy:
    """Stands in for Strategy: records the run kwargs, returns ``trades``."""
    calls: list[dict] = []
    declares_bar_magnifier = False
    trades: list[dict] = []

    def __init__(self, so_path: Path) -> None:
        self.lib = None

    # Trades per call when a test hands several lists (a run repeated from
    # the signal bar); else ``trades`` for every call.
    trades_by_call: list[list[dict]] | None = None

    def run(self, bars_csv: Path, params=None, **kwargs) -> dict:
        _FakeStrategy.calls.append({"bars_csv": bars_csv, **kwargs})
        trades = (_FakeStrategy.trades if _FakeStrategy.trades_by_call is None
                  else _FakeStrategy.trades_by_call[len(_FakeStrategy.calls) - 1])
        return {
            "trades": [dict(t) for t in trades],
            "trace": [], "trace_names": [],
            "net_profit": sum(t["pnl"] for t in trades),
            "input_bars_processed": 0,
        }


def _run_main(strategy_dir: Path, feed: Path, out: Path, trades: list[dict],
              *extra: str, trades_by_call: list[list[dict]] | None = None) -> str:
    """Drive run_strategy.main() end to end with the engine stubbed out;
    returns everything it printed."""
    _FakeStrategy.calls.clear()
    _FakeStrategy.trades = trades
    _FakeStrategy.trades_by_call = trades_by_call
    argv = ["run_strategy.py", str(strategy_dir), "--ohlcv", str(feed),
            "-o", str(out), *extra]
    buf = io.StringIO()
    with mock.patch.object(run_strategy, "ensure_derived", lambda: None), \
            mock.patch.object(run_strategy, "find_strategy_lib",
                              lambda d, so_name="strategy.so": d / so_name), \
            mock.patch.object(run_strategy, "Strategy", _FakeStrategy), \
            mock.patch.object(run_strategy, "REFERENCE_OHLCV", feed), \
            mock.patch.object(sys, "argv", argv), \
            contextlib.redirect_stdout(buf):
        rc = run_strategy.main()
    assert rc == 0, buf.getvalue()
    return buf.getvalue()


def _entry_rows(path: Path) -> list[str]:
    with path.open(encoding="utf-8") as f:
        return [r["Date and time"] for r in csv.DictReader(f)
                if r["Type"].startswith("Entry")]


def _probe(d: Path, stamps: list[int], tape: list[tuple[int, str, str, str, str]] | None,
           *, inputs: dict | None = None, metrics: dict | None = WS_METRICS) -> tuple[Path, Path]:
    feed = d / "feed.csv"
    _write_feed(feed, stamps)
    if tape is not None:
        _write_tape(d / "tv_trades.csv", tape)
    if metrics is not None:
        (d / "metrics.json").write_text(json.dumps(metrics), encoding="utf-8")
    (d / "inputs.json").write_text(
        json.dumps({"tv_trades_csv_tz": "utc_plus_8", **(inputs or {})}), encoding="utf-8")
    return feed, d / "engine_trades.csv"


# --- the six round-7 tapes ---------------------------------------------

# (probe, chart feed bars around the first TV entry, first TV entry = TV's
#  fill bar, the bar the entry was placed on, bar interval = script TF).
# The old gate was ``first entry - interval - script TF``; every signal bar
# below lies before it, and is the bar the new start lands on.
SIX_TAPES = [
    ("jos-protrader NQ1@1D",
     [_utc_ms(2025, 6, 24, 22), _utc_ms(2025, 6, 25, 22), _utc_ms(2025, 6, 26, 22),
      _utc_ms(2025, 6, 29, 22), _utc_ms(2025, 6, 30, 22)],
     _utc_ms(2025, 6, 29, 22), _utc_ms(2025, 6, 26, 22), DAY),
    ("rakesh NIFTY@1D",
     [_utc_ms(2025, 11, 12, 3, 45), _utc_ms(2025, 11, 13, 3, 45), _utc_ms(2025, 11, 14, 3, 45),
      _utc_ms(2025, 11, 17, 3, 45), _utc_ms(2025, 11, 18, 3, 45)],
     _utc_ms(2025, 11, 17, 3, 45), _utc_ms(2025, 11, 14, 3, 45), DAY),
    ("vasanth F@1D",
     [_utc_ms(2025, 10, 1, 13, 30), _utc_ms(2025, 10, 2, 13, 30), _utc_ms(2025, 10, 3, 13, 30),
      _utc_ms(2025, 10, 6, 13, 30), _utc_ms(2025, 10, 7, 13, 30)],
     _utc_ms(2025, 10, 6, 13, 30), _utc_ms(2025, 10, 3, 13, 30), DAY),
    ("stockhunter2025 XAUUSD@1D",
     [_utc_ms(2026, 1, 6, 22), _utc_ms(2026, 1, 7, 22), _utc_ms(2026, 1, 8, 22),
      _utc_ms(2026, 1, 11, 22), _utc_ms(2026, 1, 12, 22)],
     _utc_ms(2026, 1, 11, 22), _utc_ms(2026, 1, 8, 22), DAY),
    ("heneralmomo25 XAUUSD@1D (DST shift)",
     [_utc_ms(2025, 10, 28, 21), _utc_ms(2025, 10, 29, 21), _utc_ms(2025, 10, 30, 21),
      _utc_ms(2025, 11, 2, 22), _utc_ms(2025, 11, 3, 22)],
     _utc_ms(2025, 11, 2, 22), _utc_ms(2025, 10, 30, 21), DAY),
    ("ayusattv AAPL@15",
     [_utc_ms(2025, 4, 9, 19, 15), _utc_ms(2025, 4, 9, 19, 30), _utc_ms(2025, 4, 9, 19, 45),
      _utc_ms(2025, 4, 10, 13, 30), _utc_ms(2025, 4, 10, 13, 45)],
     _utc_ms(2025, 4, 10, 13, 30), _utc_ms(2025, 4, 9, 19, 45), MIN15),
]

# The old gates the diagnosis named (signal bar vs gate), UTC.
OLD_GATES = {
    "jos-protrader NQ1@1D": _utc_ms(2025, 6, 27, 22),
    "rakesh NIFTY@1D": _utc_ms(2025, 11, 15, 3, 45),
    "vasanth F@1D": _utc_ms(2025, 10, 4, 13, 30),
    "stockhunter2025 XAUUSD@1D": _utc_ms(2026, 1, 9, 22),
    "heneralmomo25 XAUUSD@1D (DST shift)": _utc_ms(2025, 10, 31, 22),
    "ayusattv AAPL@15": _utc_ms(2025, 4, 10, 13),
}


class SixTapesTests(unittest.TestCase):
    def test_the_start_is_the_signal_bar_on_every_tape(self) -> None:
        for name, feed, first_entry, signal_bar, tf in SIX_TAPES:
            with self.subTest(name):
                w = _tv_entry_emit_window(feed, first_entry, first_entry, tf)
                old_start = first_entry - tf
                self.assertEqual(old_start - tf, OLD_GATES[name])
                # The defect: the signal bar sat before the old gate.
                self.assertLess(signal_bar, OLD_GATES[name])
                # The fix: the window opens on the signal bar itself.
                self.assertEqual(w.fill_bar_ms, first_entry)
                self.assertEqual(w.signal_bar_ms, signal_bar)
                self.assertEqual(w.start_ms, signal_bar)
                self.assertLess(w.start_ms, old_start)
                # Trades keep being reported from the old bound: the fill on
                # TV's first entry bar is in, a fill on the signal bar is out.
                self.assertEqual(w.report_start_ms, old_start)
                self.assertEqual(w.end_ms, first_entry)
                report = (w.report_start_ms, w.end_ms)
                self.assertEqual(
                    [t["entry_time"] for t in _filter_trades_to_window(
                        [_trade(signal_bar, first_entry), _trade(first_entry, first_entry + tf)],
                        report)],
                    [first_entry])
                self.assertIn("widened from", _describe_tv_entry_window(w))


# --- the rule on synthetic feeds --------------------------------------

class EmitWindowRuleTests(unittest.TestCase):
    # NYSE:F 1D: weekday bars at 13:30 UTC, 2025-09-29 .. 2025-10-10.
    F_1D = _weekday_stamps(_utc_ms(2025, 9, 29), _utc_ms(2025, 10, 10), 13, 30)
    MONDAY = _utc_ms(2025, 10, 6, 13, 30)
    FRIDAY = _utc_ms(2025, 10, 3, 13, 30)

    def test_friday_signal_monday_fill(self) -> None:
        w = _tv_entry_emit_window(self.F_1D, self.MONDAY, self.MONDAY + 3 * DAY, DAY)
        self.assertEqual(w, TvEntryWindow(
            start_ms=self.FRIDAY, end_ms=self.MONDAY + 3 * DAY,
            report_start_ms=self.MONDAY - DAY, first_entry_ms=self.MONDAY,
            fill_bar_ms=self.MONDAY, signal_bar_ms=self.FRIDAY))
        self.assertEqual(
            _describe_tv_entry_window(w),
            "start 2025-10-03 13:30 UTC = the feed bar before TV's first entry bar "
            "2025-10-06 13:30 UTC (widened from 2025-10-05 13:30 UTC: the gap before "
            "the first entry exceeds one bar interval); entries reported from "
            "2025-10-05 13:30 UTC to 2025-10-09 13:30 UTC")

    def test_overnight_gap_on_a_15m_feed(self) -> None:
        # ayusattv: the 04-09 19:45 UTC signal (the session's last bar) fills
        # on 04-10 13:30 UTC, the next session's first bar.
        feed = _rth_15m_stamps([(2025, 4, 9), (2025, 4, 10)])
        first = _utc_ms(2025, 4, 10, 13, 30)
        w = _tv_entry_emit_window(feed, first, first, MIN15)
        self.assertEqual(w.start_ms, _utc_ms(2025, 4, 9, 19, 45))
        self.assertEqual(w.report_start_ms, _utc_ms(2025, 4, 10, 13, 15))
        self.assertEqual(w.fill_bar_ms, first)

    def test_gapless_feed_is_unchanged(self) -> None:
        # A 24x7 15m feed: the preceding bar IS one interval back.
        feed = list(range(_utc_ms(2026, 4, 29), _utc_ms(2026, 4, 30) + 1, MIN15))
        first = _utc_ms(2026, 4, 29, 9, 45)
        w = _tv_entry_emit_window(feed, first, first + HOUR, MIN15)
        self.assertEqual(w.start_ms, first - MIN15)
        self.assertEqual(w.report_start_ms, first - MIN15)
        self.assertEqual(w.signal_bar_ms, first - MIN15)
        self.assertIn("not earlier: unchanged", _describe_tv_entry_window(w))
        # Midweek on a daily feed likewise.
        w = _tv_entry_emit_window(self.F_1D, _utc_ms(2025, 10, 8, 13, 30),
                                  _utc_ms(2025, 10, 8, 13, 30), DAY)
        self.assertEqual(w.start_ms, _utc_ms(2025, 10, 7, 13, 30))
        self.assertEqual(w.start_ms, w.report_start_ms)

    def test_widen_only_never_later_than_the_old_start(self) -> None:
        # A feed whose first-row gap is a weekend (it starts on a Friday)
        # infers a 3-day interval: the old start of a Wednesday entry is
        # Sunday, earlier than Tuesday's bar, and is kept.
        feed = _weekday_stamps(_utc_ms(2025, 10, 3), _utc_ms(2025, 10, 10), 13, 30)
        wednesday = _utc_ms(2025, 10, 8, 13, 30)
        w = _tv_entry_emit_window(feed, wednesday, wednesday, 3 * DAY)
        self.assertEqual(w.signal_bar_ms, _utc_ms(2025, 10, 7, 13, 30))
        self.assertEqual(w.start_ms, wednesday - 3 * DAY)
        self.assertEqual(w.start_ms, w.report_start_ms)
        # Every shape: the start is at most the old start.
        for name, bars, first, _signal, tf in SIX_TAPES:
            for interval in (tf, 3 * tf, tf // 3):
                with self.subTest(name, interval=interval):
                    w = _tv_entry_emit_window(bars, first, first, interval)
                    self.assertLessEqual(w.start_ms, first - interval)
                    self.assertEqual(w.report_start_ms, first - interval)

    def test_off_grid_stamp_resolves_to_the_bar_at_or_before_it(self) -> None:
        # A tape stamp 5 minutes into a bar (a tz skew) still names that bar
        # as the fill bar and the one before it as the signal bar.
        feed = list(range(_utc_ms(2026, 4, 29), _utc_ms(2026, 4, 30) + 1, MIN15))
        stamp = _utc_ms(2026, 4, 29, 9, 50)
        w = _tv_entry_emit_window(feed, stamp, stamp, MIN15)
        self.assertEqual(w.fill_bar_ms, _utc_ms(2026, 4, 29, 9, 45))
        self.assertEqual(w.signal_bar_ms, _utc_ms(2026, 4, 29, 9, 30))
        self.assertEqual(w.start_ms, _utc_ms(2026, 4, 29, 9, 30))

    def test_first_entry_on_the_feeds_first_row(self) -> None:
        first = self.F_1D[0]
        w = _tv_entry_emit_window(self.F_1D, first, first, DAY)
        self.assertEqual(w.fill_bar_ms, first)
        self.assertIsNone(w.signal_bar_ms)
        self.assertEqual(w.start_ms, first - DAY)
        self.assertEqual(w.report_start_ms, first - DAY)
        self.assertIn("is on the loaded feed's first bar", _describe_tv_entry_window(w))

    def test_first_entry_earlier_than_the_feed_and_an_empty_feed(self) -> None:
        early = self.F_1D[0] - 5 * DAY
        for feed in (self.F_1D, []):
            with self.subTest(bars=len(feed)):
                w = _tv_entry_emit_window(feed, early, early, DAY)
                self.assertIsNone(w.fill_bar_ms)
                self.assertIsNone(w.signal_bar_ms)
                self.assertEqual(w.start_ms, early - DAY)
                self.assertEqual(w.report_start_ms, early - DAY)
                self.assertIn("precedes the loaded feed: unchanged",
                              _describe_tv_entry_window(w))

    def test_first_entry_past_the_feeds_last_bar(self) -> None:
        late = self.F_1D[-1] + 7 * DAY
        w = _tv_entry_emit_window(self.F_1D, late, late, DAY)
        self.assertEqual(w.fill_bar_ms, self.F_1D[-1])
        self.assertEqual(w.signal_bar_ms, self.F_1D[-2])
        self.assertEqual(w.start_ms, self.F_1D[-2])
        self.assertEqual(w.report_start_ms, late - DAY)
        # Nothing at or after the report bound exists to report.
        self.assertEqual(_filter_trades_to_window(
            [_trade(ts, ts + DAY) for ts in self.F_1D], (w.report_start_ms, w.end_ms)), [])


# --- the loaded feed ---------------------------------------------------

class FeedTimestampsTests(unittest.TestCase):
    def test_bounds_match_load_bars(self) -> None:
        stamps = EmitWindowRuleTests.F_1D
        with tempfile.TemporaryDirectory() as tmp:
            feed = Path(tmp) / "feed.csv"
            _write_feed(feed, stamps)
            for start, end in ((None, None), (stamps[3], None), (None, stamps[6]),
                               (stamps[2], stamps[7]), (stamps[-1] + 1, None)):
                with self.subTest(start=start, end=end):
                    bars, n, _ = _load_bars(feed, ohlcv_start_ms=start, ohlcv_end_ms=end)
                    self.assertEqual(_feed_timestamps(feed, ohlcv_start_ms=start,
                                                      ohlcv_end_ms=end),
                                     [bars[i].timestamp for i in range(n)])
            self.assertEqual(_infer_bar_interval_ms(feed), DAY)

    def test_range_start_trim_decides_which_bar_precedes(self) -> None:
        # Under a range-start trim the bars before it are not loaded: trimmed
        # at the fill bar there is no preceding bar (start unchanged);
        # trimmed at the signal bar it is the first loaded bar.
        stamps = EmitWindowRuleTests.F_1D
        monday, friday = EmitWindowRuleTests.MONDAY, EmitWindowRuleTests.FRIDAY
        with tempfile.TemporaryDirectory() as tmp:
            feed = Path(tmp) / "feed.csv"
            _write_feed(feed, stamps)
            at_fill = _tv_entry_emit_window(
                _feed_timestamps(feed, ohlcv_start_ms=monday), monday, monday, DAY)
            self.assertIsNone(at_fill.signal_bar_ms)
            self.assertEqual(at_fill.start_ms, monday - DAY)
            at_signal = _tv_entry_emit_window(
                _feed_timestamps(feed, ohlcv_start_ms=friday), monday, monday, DAY)
            self.assertEqual(at_signal.start_ms, friday)


# --- the tape ----------------------------------------------------------

class TapeSpanTests(unittest.TestCase):
    MONDAY = EmitWindowRuleTests.MONDAY

    def test_entry_rows_in_the_tapes_timezone(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            _write_tape(d / "tv_trades.csv", [
                (2, "Exit short", _taipei(self.MONDAY + 9 * DAY), "X", "9"),
                (2, "Entry short", _taipei(self.MONDAY + 7 * DAY), "S", "10"),
                (1, "Exit long", _taipei(self.MONDAY + 3 * DAY), "X", "10.5"),
                (1, "Entry long", _taipei(self.MONDAY), "L", "10"),
            ])
            self.assertEqual(_load_tv_entry_span(d, {"tv_trades_csv_tz": "utc_plus_8"}),
                             (self.MONDAY, self.MONDAY + 7 * DAY))
            # An IANA zone: 09:30 New York on 2025-10-06 is 13:30 UTC.
            _write_tape(d / "ny.csv", [
                (1, "Exit long", "2025-10-09 09:30", "X", "10.5"),
                (1, "Entry long", "2025-10-06 09:30", "L", "10"),
            ])
            self.assertEqual(
                _load_tv_entry_span(d, {"tv_trades_csv": "ny.csv",
                                        "tv_trades_csv_tz": "America/New_York"}),
                (self.MONDAY, self.MONDAY))

    def test_no_tape_or_no_entry_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.assertIsNone(_load_tv_entry_span(d, {}))
            _write_tape(d / "tv_trades.csv", [])
            self.assertIsNone(_load_tv_entry_span(d, {}))
            _write_tape(d / "tv_trades.csv",
                        [(1, "Exit long", _taipei(self.MONDAY), "Open", "10.5")])
            self.assertIsNone(_load_tv_entry_span(d, {}))


# --- main() end to end -------------------------------------------------

# --- a run that starts on TradingView's first computed bar ---------------

# BINANCE:ETHUSDT.P 15 over 2025-04-01 .. 2025-04-08, as lab tv exported
# rh-inrange-preplaced (tests/fixtures/run_harness_window): TradingView
# computed from 2025-04-01 00:00 UTC (its returnedRange.from; bar_index 0
# there, rh-diag-firstbar) and filled S1 at 06:30 UTC from a stop the script
# placed at 01:00 UTC.
ETH_FIRST_BAR = _utc_ms(2025, 4, 1)
ETH_METRICS = {
    "symbol": "BINANCE:ETHUSDT.P", "interval": "15",
    "from": "2025-04-01", "to": "2025-04-08",
    "deepBacktesting": True, "tapeChannel": "ws-report-v1",
    "wsProvenance": {
        "schemaVersion": 1,
        "requestedRange": {"from": ETH_FIRST_BAR, "to": _utc_ms(2025, 4, 8)},
        "returnedRange": {"from": ETH_FIRST_BAR, "to": _utc_ms(2025, 4, 8)},
        "rangeProof": "covered",
    },
}
S1_PLACED = _utc_ms(2025, 4, 1, 1)
S1_FILL = _utc_ms(2025, 4, 1, 6, 30)
S1_EXIT = _utc_ms(2025, 4, 1, 12, 15)
L1_FILL = _utc_ms(2025, 4, 2, 22, 45)
L1_EXIT = _utc_ms(2025, 4, 3, 12, 15)


WINDOW_TAPES = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "run_harness_window"


def _tape_trades(directory: Path) -> list[dict]:
    """The tape's trades in number order: entry/exit UTC ms, signal, price, qty."""
    tz = timezone(timedelta(hours=8))
    trades: dict[int, dict] = {}
    with (directory / "tv_trades.csv").open(encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            when = int(datetime.strptime(row["Date and time"], "%Y-%m-%d %H:%M")
                       .replace(tzinfo=tz).timestamp() * 1000)
            side = "entry" if row["Type"].startswith("Entry") else "exit"
            price = next(v for k, v in row.items() if k.startswith("Price"))
            trades.setdefault(int(row["Trade number"]), {})[side] = {
                "ms": when, "signal": row["Signal"], "price": float(price),
                "qty": float(row["Size (qty)"])}
    return [trades[n] for n in sorted(trades)]


class TradingViewFirstBarTapeTests(unittest.TestCase):
    """TradingView's rule, read off its own tapes (lab tv --no-note exports of
    synthetic probes, tests/fixtures/run_harness_window)."""

    def test_the_first_computed_bar_is_the_returned_range_start(self) -> None:
        # rh-diag-firstbar2: a market entry on bar_index 0 fills at bar 1's
        # open, so TradingView's first entry is its first computed bar plus
        # one chart bar -- on every lane, the returnedRange.from it records.
        lanes = {
            "eth15full": (_utc_ms(2025, 4, 1), MIN15),
            "eurusd15": (_utc_ms(2025, 4, 1), MIN15),
            "xauusd15": (_utc_ms(2025, 4, 1), MIN15),
            "es115": (_utc_ms(2025, 4, 1), MIN15),
            "f15": (_utc_ms(2025, 4, 1, 13, 30), MIN15),
            "nifty15": (_utc_ms(2025, 4, 1, 3, 45), MIN15),
            "btc1d": (_utc_ms(2025, 4, 1), DAY),
            "xau1d": (_utc_ms(2025, 4, 1, 21), DAY),
        }
        for lane, (first_bar, bar) in lanes.items():
            with self.subTest(lane=lane):
                d = WINDOW_TAPES / f"rh-diag-firstbar2-{lane}"
                metrics = json.loads((d / "metrics.json").read_text(encoding="utf-8"))
                self.assertEqual(metrics["wsProvenance"]["requestedRange"]["from"],
                                 _utc_ms(2025, 4, 1))
                self.assertEqual(run_strategy._tv_first_bar(d, {})[0], first_bar)
                b0 = _tape_trades(d)[0]["entry"]
                self.assertEqual(b0["signal"], "B0")
                self.assertEqual(b0["ms"], first_bar + bar)
                # D1 fires on bar_index 4, the first bar at/after 00:00 UTC
                # once four bars exist: no bar precedes the returned start.
                self.assertEqual(_tape_trades(d)[1]["entry"]["qty"], 5)

    def test_no_bar_before_the_range_start(self) -> None:
        d = WINDOW_TAPES / "rh-diag-firstbar"
        self.assertEqual(run_strategy._tv_first_bar(d, {})[0], ETH_FIRST_BAR)
        trades = _tape_trades(d)
        self.assertEqual([t["entry"]["signal"] for t in trades], ["D1", "D2", "D3"])
        # bar_index + 1, the steps from the first bar, the pre-range bars + 1.
        self.assertEqual([t["entry"]["qty"] for t in trades], [1, 1, 1])
        self.assertEqual(trades[0]["entry"]["ms"], ETH_FIRST_BAR + MIN15)
        # Orders a script places on 2025-03-31 bars never exist.
        self.assertEqual(_tape_trades(WINDOW_TAPES / "rh-prerange-stop"), [])
        self.assertEqual(_tape_trades(WINDOW_TAPES / "rh-prerange-limit"), [])
        fills = _tape_trades(WINDOW_TAPES / "rh-prerange-fills")
        self.assertEqual([t["entry"]["signal"] for t in fills], ["IN"])
        self.assertEqual(fills[0]["entry"]["ms"], _utc_ms(2025, 4, 2, 0, 15))

    def test_an_order_placed_before_the_first_fill_makes_it(self) -> None:
        # S1's stop was placed at 01:00 UTC, before the 06:15 signal bar.
        trades = _tape_trades(WINDOW_TAPES / "rh-inrange-preplaced")
        self.assertEqual([(t["entry"]["signal"], t["entry"]["ms"], t["entry"]["price"])
                          for t in trades],
                         [("S1", S1_FILL, 1850.0), ("L1", L1_FILL, 1790.0)])
        self.assertLess(S1_PLACED, S1_FILL - MIN15)


class TvFirstBarTests(unittest.TestCase):
    def test_read_from_the_ws_report_range(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "metrics.json").write_text(json.dumps(ETH_METRICS), encoding="utf-8")
            self.assertEqual(run_strategy._tv_first_bar(d, {}), (
                ETH_FIRST_BAR, "metrics.json wsProvenance.returnedRange.from"))
            # A session-bound lane: NYSE:F's range opens at 13:30 UTC.
            (d / "metrics.json").write_text(json.dumps(WS_METRICS), encoding="utf-8")
            self.assertEqual(run_strategy._tv_first_bar(d, {})[0], _utc_ms(2025, 4, 1, 13, 30))

    def test_none_without_a_returned_range(self) -> None:
        browser = {"symbol": "BINANCE:ETHUSDT.P", "interval": "15",
                   "from": "2025-04-01", "to": "2026-05-01", "deepBacktesting": True}
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.assertIsNone(run_strategy._tv_first_bar(d, {}))
            for metrics in (browser, {"wsProvenance": {"returnedRange": None}},
                            {"wsProvenance": {"returnedRange": {"from": True}}},
                            {"wsProvenance": {"returnedRange": {"from": "1743465600000"}}},
                            {"wsProvenance": {"returnedRange": {"from": 0}}}, []):
                with self.subTest(metrics=metrics):
                    (d / "metrics.json").write_text(json.dumps(metrics), encoding="utf-8")
                    self.assertIsNone(run_strategy._tv_first_bar(d, {}))
            (d / "metrics.json").write_text("{not json", encoding="utf-8")
            self.assertIsNone(run_strategy._tv_first_bar(d, {}))

    def test_the_window_opens_on_tvs_first_bar_when_the_feed_starts_there(self) -> None:
        feed = list(range(ETH_FIRST_BAR, _utc_ms(2025, 4, 4) + 1, MIN15))
        w = _tv_entry_emit_window(feed, S1_FILL, L1_FILL, MIN15, ETH_FIRST_BAR)
        self.assertEqual(w, TvEntryWindow(
            start_ms=ETH_FIRST_BAR, end_ms=L1_FILL, report_start_ms=S1_FILL - MIN15,
            first_entry_ms=S1_FILL, fill_bar_ms=S1_FILL, signal_bar_ms=S1_FILL - MIN15,
            tv_first_bar_ms=ETH_FIRST_BAR))
        # S1's stop, placed at 01:00, is inside the window; the signal bar
        # alone (06:15) put it outside.
        self.assertLessEqual(w.start_ms, S1_PLACED)
        self.assertGreater(_tv_entry_emit_window(feed, S1_FILL, L1_FILL, MIN15).start_ms,
                           S1_PLACED)
        self.assertEqual(
            _describe_tv_entry_window(w),
            "start 2025-04-01 00:00 UTC = TV's first computed bar, where this run's feed "
            "starts (orders placed before TV's first entry bar 2025-04-01 06:30 UTC are "
            "live, as on TV); entries reported from 2025-04-01 06:15 UTC to "
            "2025-04-02 22:45 UTC")

    def test_a_feed_with_bars_before_tvs_first_bar_keeps_the_signal_bar(self) -> None:
        # A warmed full-history (or from-1d padded) feed: its script state
        # before TradingView's first bar is not TradingView's.
        feed = list(range(_utc_ms(2025, 3, 30), _utc_ms(2025, 4, 4) + 1, MIN15))
        w = _tv_entry_emit_window(feed, S1_FILL, L1_FILL, MIN15, ETH_FIRST_BAR)
        self.assertEqual(w.start_ms, S1_FILL - MIN15)
        self.assertIsNone(w.tv_first_bar_ms)
        self.assertIn("not earlier: unchanged", _describe_tv_entry_window(w))

    def test_a_feed_starting_after_tvs_first_bar_keeps_the_signal_bar(self) -> None:
        feed = list(range(_utc_ms(2025, 4, 1, 3), _utc_ms(2025, 4, 4) + 1, MIN15))
        w = _tv_entry_emit_window(feed, S1_FILL, L1_FILL, MIN15, ETH_FIRST_BAR)
        self.assertEqual(w.start_ms, S1_FILL - MIN15)
        self.assertIsNone(w.tv_first_bar_ms)

    def test_a_first_fill_on_tvs_first_bar_keeps_the_earlier_start(self) -> None:
        # Widen-only: the old start (one interval before the fill) is earlier.
        feed = list(range(ETH_FIRST_BAR, _utc_ms(2025, 4, 2) + 1, MIN15))
        w = _tv_entry_emit_window(feed, ETH_FIRST_BAR, S1_FILL, MIN15, ETH_FIRST_BAR)
        self.assertEqual(w.start_ms, ETH_FIRST_BAR - MIN15)
        self.assertIsNone(w.signal_bar_ms)
        self.assertIn("is on the loaded feed's first bar", _describe_tv_entry_window(w))

    def test_session_lane_first_bar(self) -> None:
        # NYSE:F 15: TradingView's range opens on the 13:30 UTC session bar.
        feed = _rth_15m_stamps([(2025, 4, 1), (2025, 4, 2)])
        fill = _utc_ms(2025, 4, 2, 14, 0)
        w = _tv_entry_emit_window(feed, fill, fill, MIN15, _utc_ms(2025, 4, 1, 13, 30))
        self.assertEqual(w.start_ms, _utc_ms(2025, 4, 1, 13, 30))
        self.assertEqual(w.report_start_ms, fill - MIN15)


class MainEmitWindowTests(unittest.TestCase):
    F_1D = EmitWindowRuleTests.F_1D
    MONDAY = EmitWindowRuleTests.MONDAY
    FRIDAY = EmitWindowRuleTests.FRIDAY
    THURSDAY_EXIT = _utc_ms(2025, 10, 9, 13, 30)

    def _tape(self) -> list[tuple[int, str, str, str, str]]:
        return [(1, "Exit long", _taipei(self.THURSDAY_EXIT), "X", "10.5"),
                (1, "Entry long", _taipei(self.MONDAY), "L", "10")]

    def _engine_trades(self) -> list[dict]:
        return [
            _trade(_utc_ms(2025, 9, 30, 13, 30), self.FRIDAY),   # pre-window
            _trade(self.FRIDAY, self.MONDAY),                    # a fill ON the signal bar
            _trade(self.MONDAY, self.THURSDAY_EXIT),             # TV's trade
            _trade(_utc_ms(2025, 10, 10, 13, 30), RANGE_TO_MS),  # after TV's last entry
        ]

    def test_friday_signal_monday_fill_1d_feed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, self._tape())
            printed = _run_main(d, feed, out, self._engine_trades())
            call = _FakeStrategy.calls[0]
            # The engine's gate opens on Friday's bar, the feed is bounded
            # at TV's range end as before.
            self.assertEqual(call["trade_start_time_ms"], self.FRIDAY)
            self.assertEqual(call["ohlcv_end_ms"], RANGE_TO_MS)
            self.assertIsNone(call.get("ohlcv_start_ms"))
            # Only the fill on TV's first entry bar is written: not the fill
            # on the signal bar (TV did not report it), not the pre-window
            # trade, not the entry after TV's last entry.
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30"])
            self.assertIn(
                "  emit-window: start 2025-10-03 13:30 UTC = the feed bar before TV's "
                "first entry bar 2025-10-06 13:30 UTC (widened from 2025-10-05 13:30 UTC",
                printed)
            self.assertEqual(printed.count("range-end:"), 1)
            self.assertIn("1 trades (4 raw)", printed)

    def test_broker_flags_keep_their_meaning(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, self._tape())
            # --allow-trading-before-window: no gate, the same rows.
            _run_main(d, feed, out, self._engine_trades(), "--allow-trading-before-window")
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30"])
            # --disable-trading-before-window is what a tape implies anyway.
            _run_main(d, feed, out, self._engine_trades(), "--disable-trading-before-window")
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.FRIDAY)
            # --no-trim-output: no window at all, every trade written.
            printed = _run_main(d, feed, out, self._engine_trades(), "--no-trim-output")
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertEqual(len(_entry_rows(out)), 4)
            self.assertNotIn("emit-window:", printed)

    def test_overnight_gap_15m_feed(self) -> None:
        # ayusattv AAPL@15: the short placed on the 04-09 19:45 UTC bar fills
        # on 04-10 13:30 UTC.
        stamps = _rth_15m_stamps([(2025, 4, 8), (2025, 4, 9), (2025, 4, 10), (2025, 4, 11)])
        signal, fill = _utc_ms(2025, 4, 9, 19, 45), _utc_ms(2025, 4, 10, 13, 30)
        exit_ = _utc_ms(2025, 4, 11, 13, 30)
        tape = [(1, "Exit short", _taipei(exit_), "SL", "9.5"),
                (1, "Entry short", _taipei(fill), "S", "10")]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, stamps, tape, metrics={**WS_METRICS, "symbol": "NASDAQ:AAPL",
                                                         "interval": "15"})
            printed = _run_main(d, feed, out, [
                _trade(signal, fill, is_long=False),
                _trade(fill, exit_, is_long=False),
            ])
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], signal)
            self.assertEqual(_entry_rows(out), ["2025-04-10 13:30"])
            self.assertIn("widened from 2025-04-10 13:15 UTC", printed)

    def test_range_start_trim_is_the_loaded_feed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            # Trimmed at the fill bar: no earlier bar is loaded, the start is
            # the old one.
            feed, out = _probe(d, self.F_1D, self._tape(), inputs={"ohlcv_start_ms": self.MONDAY})
            printed = _run_main(d, feed, out, self._engine_trades())
            call = _FakeStrategy.calls[0]
            self.assertEqual(call["ohlcv_start_ms"], self.MONDAY)
            self.assertEqual(call["trade_start_time_ms"], self.MONDAY - DAY)
            self.assertIn("is on the loaded feed's first bar 2025-10-06 13:30 UTC", printed)
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30"])
            # Trimmed at the signal bar: it is the first loaded bar.
            feed, out = _probe(d, self.F_1D, self._tape(), inputs={"ohlcv_start_ms": self.FRIDAY})
            _run_main(d, feed, out, self._engine_trades())
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.FRIDAY)

    def test_no_tv_entries_falls_back_to_the_reference_window(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, [(1, "Exit long", _taipei(self.MONDAY), "Open", "10")])
            printed = _run_main(d, feed, out, self._engine_trades())
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertIsNone(_FakeStrategy.calls[0].get("ohlcv_end_ms"))
            self.assertNotIn("emit-window:", printed)
            self.assertNotIn("range-end:", printed)
            # The reference window (the feed's span here) reports every
            # trade entered inside it.
            self.assertEqual(_entry_rows(out), ["2025-10-10 13:30", "2025-10-06 13:30",
                                                "2025-10-03 13:30", "2025-09-30 13:30"])
            _run_main(d, feed, out, self._engine_trades(), "--disable-trading-before-window")
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.F_1D[0])
            # No tape at all: the same.
            (d / "tv_trades.csv").unlink()
            printed = _run_main(d, feed, out, self._engine_trades())
            self.assertIsNone(_FakeStrategy.calls[0]["trade_start_time_ms"])
            self.assertNotIn("emit-window:", printed)

    def test_first_tv_entry_before_the_feed(self) -> None:
        early = _utc_ms(2025, 9, 1, 13, 30)
        tape = [(1, "Exit long", _taipei(self.THURSDAY_EXIT), "X", "10.5"),
                (1, "Entry long", _taipei(early), "L", "10")]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, tape)
            printed = _run_main(d, feed, out, self._engine_trades())
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], early - DAY)
            self.assertIn("precedes the loaded feed: unchanged", printed)
            # Entries up to TV's last (= first) entry only: none of the
            # engine's trades, as before.
            self.assertEqual(_entry_rows(out), [])

    def _eth_tape(self) -> list[tuple[int, str, str, str, str]]:
        return [(2, "Exit long", _taipei(L1_EXIT), "L1_X", "1797.78"),
                (2, "Entry long", _taipei(L1_FILL), "L1", "1790"),
                (1, "Exit long", _taipei(S1_EXIT), "S1_X", "1868.83"),
                (1, "Entry long", _taipei(S1_FILL), "S1", "1850")]

    def _eth_engine_trades(self) -> list[dict]:
        return [
            _trade(_utc_ms(2025, 4, 1, 3), _utc_ms(2025, 4, 1, 4)),  # before the report bound
            _trade(S1_FILL, S1_EXIT),
            _trade(L1_FILL, L1_EXIT),
        ]

    def test_a_run_trimmed_to_tvs_first_bar_trades_from_it(self) -> None:
        # The verifier's start-of-window candidate: the feed trimmed at the
        # tape's from (00:00 UTC), which is TradingView's first bar. The
        # engine fills nothing before TV's first entry, as TV did not.
        stamps = list(range(_utc_ms(2025, 3, 30), _utc_ms(2025, 4, 4) + 1, MIN15))
        tv_like = self._eth_engine_trades()[1:]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, stamps, self._eth_tape(), metrics=ETH_METRICS,
                               inputs={"ohlcv_start_ms": ETH_FIRST_BAR})
            printed = _run_main(d, feed, out, tv_like, "--disable-trading-before-window")
            self.assertEqual(len(_FakeStrategy.calls), 1)
            call = _FakeStrategy.calls[0]
            self.assertEqual(call["ohlcv_start_ms"], ETH_FIRST_BAR)
            self.assertEqual(call["trade_start_time_ms"], ETH_FIRST_BAR)
            self.assertIn("= TV's first computed bar, where this run's feed starts", printed)
            self.assertNotIn("run again from", printed)
            # Reported rows: from one bar before TV's first entry, as always.
            self.assertEqual(_entry_rows(out), ["2025-04-02 22:45", "2025-04-01 06:30"])
            # The same feed, warmed from 2025-03-30: the signal-bar start.
            feed, out = _probe(d, stamps, self._eth_tape(), metrics=ETH_METRICS)
            _run_main(d, feed, out, tv_like, "--disable-trading-before-window")
            self.assertEqual(len(_FakeStrategy.calls), 1)
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], S1_FILL - MIN15)
            self.assertEqual(_entry_rows(out), ["2025-04-02 22:45", "2025-04-01 06:30"])

    def test_an_entry_before_tvs_first_entry_runs_again_from_the_signal_bar(self) -> None:
        # The run from TradingView's first bar entered at 03:00, hours before
        # TV's first entry (06:30), where TV filled nothing: its state is not
        # TV's, so the probe runs again from the signal bar and reports that.
        stamps = list(range(_utc_ms(2025, 3, 30), _utc_ms(2025, 4, 4) + 1, MIN15))
        early = self._eth_engine_trades()
        again = self._eth_engine_trades()[1:2]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, stamps, self._eth_tape(), metrics=ETH_METRICS,
                               inputs={"ohlcv_start_ms": ETH_FIRST_BAR})
            printed = _run_main(d, feed, out, [], "--disable-trading-before-window",
                                trades_by_call=[early, again])
            self.assertEqual([c["trade_start_time_ms"] for c in _FakeStrategy.calls],
                             [ETH_FIRST_BAR, S1_FILL - MIN15])
            self.assertIn(
                "  emit-window: the run from TV's first computed bar entered at "
                "2025-04-01 03:00 UTC, before TV's first entry, where TV filled nothing: "
                "run again from 2025-04-01 06:15 UTC", printed)
            # The rows written are the second run's.
            self.assertEqual(_entry_rows(out), ["2025-04-01 06:30"])
            # An entry ON the report bound (one bar before TV's first entry)
            # is a reported row, not an early one: no second run.
            at_bound = [_trade(S1_FILL - MIN15, S1_EXIT)] + self._eth_engine_trades()[2:]
            printed = _run_main(d, feed, out, at_bound, "--disable-trading-before-window")
            self.assertEqual(len(_FakeStrategy.calls), 1)
            self.assertNotIn("run again from", printed)
            # --allow-trading-before-window keeps no gate and never re-runs.
            _run_main(d, feed, out, early, "--allow-trading-before-window")
            self.assertEqual([c["trade_start_time_ms"] for c in _FakeStrategy.calls], [None])

    def test_no_rerun_when_the_signal_bar_gate_admits_every_bar(self) -> None:
        # TradingView's first fill on the feed's third bar (00:30): the signal
        # bar is 00:15, and the engine admits commands one bar before a gate,
        # so the signal-bar gate admits the first bar too -- a re-run would
        # repeat the same run. The early 00:00 entry is simply not reported.
        stamps = list(range(ETH_FIRST_BAR, _utc_ms(2025, 4, 2) + 1, MIN15))
        fill = ETH_FIRST_BAR + 2 * MIN15
        tape = [(1, "Exit long", _taipei(fill + HOUR), "X", "10.5"),
                (1, "Entry long", _taipei(fill), "L", "10")]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, stamps, tape, metrics=ETH_METRICS)
            printed = _run_main(d, feed, out, [_trade(ETH_FIRST_BAR, fill),
                                               _trade(fill, fill + HOUR)],
                                "--disable-trading-before-window")
            self.assertEqual([c["trade_start_time_ms"] for c in _FakeStrategy.calls],
                             [ETH_FIRST_BAR])
            self.assertNotIn("run again from", printed)
            self.assertEqual(_entry_rows(out), ["2025-04-01 00:30"])

    def test_a_browser_tape_keeps_the_signal_bar(self) -> None:
        # No returned range: TradingView's first bar is not on record.
        stamps = list(range(ETH_FIRST_BAR, _utc_ms(2025, 4, 4) + 1, MIN15))
        browser = {k: v for k, v in ETH_METRICS.items()
                   if k not in ("tapeChannel", "wsProvenance")}
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, stamps, self._eth_tape(), metrics=browser)
            _run_main(d, feed, out, self._eth_engine_trades())
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], S1_FILL - MIN15)

    def test_explicit_emit_window_reports_what_it_gates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, self.F_1D, self._tape())
            window = d / "window.csv"
            _write_feed(window, self.F_1D[4:8])   # 10-03 .. 10-08
            printed = _run_main(d, feed, out, self._engine_trades(),
                                "--emit-window-ohlcv", str(window),
                                "--disable-trading-before-window")
            self.assertEqual(_FakeStrategy.calls[0]["trade_start_time_ms"], self.FRIDAY)
            self.assertEqual(_entry_rows(out), ["2025-10-06 13:30", "2025-10-03 13:30"])
            self.assertNotIn("emit-window:", printed)


if __name__ == "__main__":
    unittest.main()
