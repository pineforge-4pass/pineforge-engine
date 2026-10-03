#!/usr/bin/env python3
"""A script that declares ``strategy(..., use_bar_magnifier = true)`` runs
magnified.

TradingView backtests such a script with its bar magnifier: the lab tv tapes
mi-fx-eth-15 (declared) and w9mag-fx-eth-15-off (the same script, declared
off) enter on the same bars and differ on 7 of their 24 exits
(tests/fixtures/magnifier_intrabars; test_adapter_magnifier_intrabar_tapes
replays both). The declaration reaches the host as the library export
``strategy_declares_bar_magnifier()`` (codegen e8f6648); run_strategy.py
reads it (Strategy.declares_bar_magnifier) and, on an intraday chart coarser
than 1m, feeds the engine the lane's 1m feed named by
PINEFORGE_RUN_MAGNIFIER_FEED with the magnifier on, while TradingView's
window is still walked on the chart feed (_declared_magnifier_plan). A daily
chart runs magnified on that feed too when the feed rebuilds the chart's own
daily bars, with the chart feed installed as the run's daily feed, whose
stamps date the days (tests/fixtures/daily_magnifier); it reports
``declared-not-run`` otherwise, as a weekly, monthly or multi-day chart does.
"""

from __future__ import annotations

import contextlib
import hashlib
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
    MAGNIFIER_FEED_ENV,
    MAGNIFIER_FEED_SHA256_ENV,
    BarC,
    _declared_magnifier_plan,
    _tf_seconds,
    _with_tail_bar,
)

MIN = 60 * 1000
MIN15 = 15 * MIN
DAY = 96 * MIN15


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


def _taipei(ms: int) -> str:
    tz = timezone(timedelta(hours=8))
    return datetime.fromtimestamp(ms / 1000.0, tz=tz).strftime("%Y-%m-%d %H:%M")


def _write_feed(path: Path, stamps: list[int]) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for ts in stamps:
            f.write(f"{ts},10,11,9,10.5,100\n")


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


RANGE_START = _utc_ms(2026, 1, 29)
RANGE_END = _utc_ms(2026, 2, 1)
METRICS = {
    "symbol": "BINANCE:ETHUSDT.P", "interval": "15",
    "from": "2026-01-29", "to": "2026-02-01",
    "deepBacktesting": True, "tapeChannel": "ws-report-v1",
    "wsProvenance": {
        "schemaVersion": 1,
        "requestedRange": {"from": RANGE_START, "to": RANGE_END},
        "returnedRange": {"from": RANGE_START, "to": RANGE_END},
        "rangeProof": "covered",
    },
}
ENTRY = _utc_ms(2026, 1, 30, 12, 15)
EXIT = _utc_ms(2026, 1, 30, 13, 0)
# An engine entry ten minutes before TradingView's: inside the report bound a
# chart-feed window gives (one 15m bar before TV's first entry), outside the
# one a 1m-feed window would give (one minute before it).
EARLY = ENTRY - 10 * MIN


class TfSecondsTests(unittest.TestCase):
    def test_matches_tf_to_seconds(self) -> None:
        # src/timeframe.cpp tf_to_seconds, case for case.
        for tf, seconds in (("", 0), ("1", 60), ("15", 900), ("240", 14400),
                            ("D", 86400), ("1D", 86400), ("3D", 259200),
                            ("W", 604800), ("1W", 604800), ("M", -1), ("12M", -1),
                            ("15S", 15), ("S", 0), ("x", 0)):
            with self.subTest(tf=tf):
                self.assertEqual(_tf_seconds(tf), seconds)


class PlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.chart = d / "chart.csv"
        self.finer = d / "finer.csv"
        _write_feed(self.chart, list(range(RANGE_START, RANGE_END + 1, MIN15)))
        _write_feed(self.finer, list(range(RANGE_START, RANGE_END + MIN15, MIN)))
        self.kwargs = {"input_tf": "15", "script_tf": "15", "bar_magnifier": False,
                       "ohlcv_end_ms": RANGE_END, "ohlcv_start_ms": None}

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def env(self, **extra: str) -> dict:
        return {MAGNIFIER_FEED_ENV: str(self.finer), **extra}

    def test_intraday_chart_runs_on_the_1m_feed(self) -> None:
        plan = _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs,
                                        self.env(**{MAGNIFIER_FEED_SHA256_ENV: _sha(self.finer)}))
        self.assertEqual(plan.status, "declared")
        self.assertEqual(plan.ohlcv_path, self.finer.resolve())
        self.assertEqual(plan.run_kwargs["input_tf"], "1")
        self.assertEqual(plan.run_kwargs["script_tf"], "15")
        self.assertIs(plan.run_kwargs["bar_magnifier"], True)
        # The chart feed's loaded bars, minute for minute: from its first
        # bar, to the last minute of TradingView's last chart bar (opening at
        # the range end) -- its fifteen 1m bars, not only the first.
        self.assertEqual(plan.run_kwargs["ohlcv_start_ms"], RANGE_START)
        self.assertEqual(plan.run_kwargs["ohlcv_end_ms"], RANGE_END + MIN15 - 1)
        self.assertIn(f"sha256 {_sha(self.finer)}", plan.detail)
        # The caller's kwargs are not touched.
        self.assertEqual(self.kwargs["input_tf"], "15")
        self.assertEqual(self.kwargs["ohlcv_end_ms"], RANGE_END)

    def test_the_1m_run_reads_the_chart_feeds_span(self) -> None:
        # A 1m feed that starts a day earlier warms nothing the chart run does
        # not; a start bound the chart applies is the 1m run's too.
        _write_feed(self.finer, list(range(RANGE_START - 96 * MIN15, RANGE_END + MIN15, MIN)))
        plan = _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs, self.env())
        self.assertEqual(plan.run_kwargs["ohlcv_start_ms"], RANGE_START)
        bounded = {**self.kwargs, "ohlcv_start_ms": RANGE_START + 7 * MIN}
        plan = _declared_magnifier_plan({"script_tf": "15"}, self.chart, bounded, self.env())
        self.assertEqual(plan.run_kwargs["ohlcv_start_ms"], RANGE_START + MIN15)
        # No end bound: the chart feed's last bar, whole.
        plan = _declared_magnifier_plan({"script_tf": "15"}, self.chart,
                                        {**self.kwargs, "ohlcv_end_ms": None}, self.env())
        self.assertEqual(plan.run_kwargs["ohlcv_end_ms"], RANGE_END + MIN15 - 1)
        # No chart bar inside the bounds: nothing to magnify.
        empty = {**self.kwargs, "ohlcv_start_ms": RANGE_END + DAY}
        plan = _declared_magnifier_plan({"script_tf": "15"}, self.chart, empty, self.env())
        self.assertEqual((plan.status, plan.detail),
                         ("declared-not-run", "the chart feed has no bar in the run's bounds"))

    def test_report_quote_is_pinned_and_does_not_change_execution_inputs(self) -> None:
        baseline = _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs, self.env())
        supplied = self.env(**{
            run_strategy.REPORT_QUOTE_FEED_ENV: str(self.chart),
            run_strategy.REPORT_QUOTE_SHA256_ENV: _sha(self.chart),
        })
        quoted = _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs, supplied)
        self.assertEqual(quoted.run_kwargs["report_terminal_quote"][0], RANGE_END)
        execution = dict(quoted.run_kwargs)
        execution.pop("report_terminal_quote")
        self.assertEqual(execution.pop("report_terminal_quote_source_sha256"), _sha(self.chart))
        self.assertEqual(execution, baseline.run_kwargs)
        self.assertEqual(quoted.ohlcv_path, baseline.ohlcv_path)
        self.assertNotIn("native_security_feeds", quoted.run_kwargs)
        exclusive = _declared_magnifier_plan({"script_tf": "15"}, self.chart,
            {**self.kwargs, "ohlcv_end_ms": RANGE_END - 1}, supplied)
        self.assertEqual(exclusive.run_kwargs["report_terminal_quote"][0], RANGE_END - MIN15)

    def test_report_quote_observable_checks_the_presented_rows(self) -> None:
        report = {"trades": [{"open_at_end": True, "exit_time": RANGE_END}],
                  "equity_curve_time_ms": [RANGE_END], "bar_magnifier_enabled": 1}
        quote = (RANGE_END, 112.0)
        self.assertTrue(run_strategy._report_terminal_quote_applied(report, quote, "15"))
        self.assertFalse(run_strategy._report_terminal_quote_applied(report, quote, "1D"))
        self.assertFalse(run_strategy._report_terminal_quote_applied(report, (RANGE_END + 1, 112.0), "15"))
        self.assertFalse(run_strategy._report_terminal_quote_applied({**report, "trades": []}, quote, "15"))
        self.assertFalse(run_strategy._report_terminal_quote_applied({**report, "bar_magnifier_enabled": 0}, quote, "15"))

    def test_report_quote_refuses_unpinned_or_different_chart_bytes(self) -> None:
        for supplied in (
            {run_strategy.REPORT_QUOTE_FEED_ENV: str(self.chart)},
            {run_strategy.REPORT_QUOTE_SHA256_ENV: _sha(self.chart)},
            {run_strategy.REPORT_QUOTE_FEED_ENV: str(self.chart),
             run_strategy.REPORT_QUOTE_SHA256_ENV: "0" * 64},
            {run_strategy.REPORT_QUOTE_FEED_ENV: str(self.finer),
             run_strategy.REPORT_QUOTE_SHA256_ENV: _sha(self.finer)},
        ):
            with self.subTest(supplied=supplied), self.assertRaises(ValueError):
                _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs,
                    self.env(**supplied))

    def test_chart_tf_from_the_feed_when_the_probe_names_none(self) -> None:
        plan = _declared_magnifier_plan({}, self.chart, self.kwargs, self.env())
        self.assertEqual(plan.status, "declared")
        self.assertEqual(plan.run_kwargs["script_tf"], "15")

    def test_not_run(self) -> None:
        cases = (
            ({"script_tf": "2D"}, self.kwargs, self.env(), "chart 2D is coarser than one day"),
            ({"script_tf": "W"}, self.kwargs, self.env(), "chart W is coarser than one day"),
            ({"script_tf": "M"}, self.kwargs, self.env(), "chart M is coarser than one day"),
            ({"script_tf": "1"}, self.kwargs, self.env(),
             "chart 1 is not coarser than the 1m feed"),
            ({"script_tf": "15"}, self.kwargs, {}, f"no magnifier feed ({MAGNIFIER_FEED_ENV} unset)"),
            ({"script_tf": "15", "runtime_overrides": {"bar_magnifier": False}}, self.kwargs,
             self.env(), "runtime_overrides.bar_magnifier is false"),
            ({"script_tf": "15"}, {**self.kwargs, "aux_security_ohlcv_csv": self.finer},
             self.env(), "the run reads an auxiliary request.security feed"),
        )
        for params, kwargs, env, why in cases:
            with self.subTest(why=why):
                plan = _declared_magnifier_plan(params, self.chart, kwargs, env)
                self.assertEqual(plan.status, "declared-not-run")
                self.assertEqual(plan.detail, why)
                self.assertEqual(plan.ohlcv_path, self.chart)
                self.assertIs(plan.run_kwargs, kwargs)

    def test_a_feed_that_is_not_the_pinned_one_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs,
                                     self.env(**{MAGNIFIER_FEED_SHA256_ENV: "0" * 64}))
        with self.assertRaises(FileNotFoundError):
            _declared_magnifier_plan({"script_tf": "15"}, self.chart, self.kwargs,
                                     {MAGNIFIER_FEED_ENV: str(self.finer) + ".missing"})


# OANDA:XAUUSD's shape: a daily bar stamped at 17:00 New York (21:00 UTC in
# summer) and its session's minutes from 18:00 to 16:59 New York.
XAU_DAY0 = _utc_ms(2026, 3, 23, 21, 0)


def _write_bars(path: Path, rows: list[tuple]) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for row in rows:
            f.write(",".join(str(x) for x in row) + "\n")


def _xau_days(days: int, cut_last_after: int | None = None) -> tuple[list, list]:
    """``days`` daily bars and the 1m bars that rebuild them; the last day's
    minutes stop after ``cut_last_after`` of them when given."""
    daily, minutes = [], []
    for d in range(days):
        stamp = XAU_DAY0 + d * DAY
        first = stamp + 60 * MIN
        count = 23 * 60
        if cut_last_after is not None and d == days - 1:
            count = cut_last_after
        rows = []
        for m in range(23 * 60):
            price = 100.0 + d + (m % 7) * 0.5
            rows.append((first + m * MIN, price, price + 1.0, price - 1.0, price + 0.25, 2.0))
        o, h, l, c = rows[0][1], max(r[2] for r in rows), min(r[3] for r in rows), rows[-1][4]
        daily.append((stamp, o, h, l, c, 2.0 * len(rows)))
        minutes.extend(rows[:count])
    return daily, minutes


class DailyPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.chart = d / "chart.csv"
        self.finer = d / "finer.csv"
        self.daily, self.minutes = _xau_days(4)
        _write_bars(self.chart, self.daily)
        _write_bars(self.finer, self.minutes)
        self.kwargs = {"input_tf": "1D", "script_tf": "1D", "bar_magnifier": False,
                       "ohlcv_start_ms": None, "ohlcv_end_ms": None}

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def plan(self, kwargs=None):
        return _declared_magnifier_plan({"script_tf": "1D"}, self.chart, kwargs or self.kwargs,
                                        {MAGNIFIER_FEED_ENV: str(self.finer)})

    def test_a_rebuilt_daily_chart_runs_on_the_1m_feed(self) -> None:
        plan = self.plan()
        self.assertEqual(plan.status, "declared")
        self.assertEqual(plan.ohlcv_path, self.finer.resolve())
        kw = plan.run_kwargs
        self.assertEqual((kw["input_tf"], kw["script_tf"], kw["bar_magnifier"]), ("1", "1D", True))
        # The chart's own span: from its first stamp to its last bar's end.
        self.assertEqual(kw["ohlcv_start_ms"], XAU_DAY0)
        self.assertEqual(kw["ohlcv_end_ms"], XAU_DAY0 + 4 * DAY - 1)
        # The chart's daily bars are the run's daily feed: their stamps date the days.
        feeds = kw["native_security_feeds"]
        self.assertEqual(list(feeds), ["1D"])
        self.assertEqual(feeds["1D"]["path"], self.chart.resolve())
        self.assertEqual(feeds["1D"]["source_file_sha256"], _sha(self.chart))
        self.assertNotIn("ohlcv_tail_bar", kw)
        self.assertIn(f"the chart's daily bars are its daily feed (sha256 {_sha(self.chart)})",
                      plan.detail)
        self.assertNotIn("native_security_feeds", self.kwargs)

    def test_a_daily_feed_the_run_installs_is_kept(self) -> None:
        other = {"D": {"path": self.finer, "source_file_sha256": "f" * 64}}
        plan = self.plan({**self.kwargs, "native_security_feeds": other})
        self.assertEqual(plan.status, "declared")
        self.assertEqual(plan.run_kwargs["native_security_feeds"], other)
        self.assertNotIn("daily feed (sha256", plan.detail)

    def test_bars_the_feed_does_not_rebuild(self) -> None:
        cases = []
        # An official open the minutes do not print.
        daily = [list(r) for r in self.daily]
        daily[1][1] += 0.5
        cases.append((daily, self.minutes,
                      f"the daily bar of {run_strategy._fmt_utc_ms(XAU_DAY0 + DAY)} has open "))
        # A settlement close.
        daily = [list(r) for r in self.daily]
        daily[2][4] -= 0.25
        cases.append((daily, self.minutes,
                      f"the daily bar of {run_strategy._fmt_utc_ms(XAU_DAY0 + 2 * DAY)} has close "))
        # A daily volume of its own.
        daily = [list(r) for r in self.daily]
        daily[0][5] += 1.0
        cases.append((daily, self.minutes,
                      f"the daily bar of {run_strategy._fmt_utc_ms(XAU_DAY0)} has volume "))
        # A 1m feed that starts after the chart.
        cases.append((self.daily, [m for m in self.minutes if m[0] >= XAU_DAY0 + DAY],
                      f"no finer bar in the daily bar of {run_strategy._fmt_utc_ms(XAU_DAY0)}"))
        for daily, minutes, why in cases:
            with self.subTest(why=why):
                _write_bars(self.chart, [tuple(r) for r in daily])
                _write_bars(self.finer, minutes)
                plan = self.plan()
                self.assertEqual(plan.status, "declared-not-run")
                self.assertTrue(plan.detail.startswith(
                    "the finer feed does not rebuild the chart's own daily bars ("), plan.detail)
                self.assertIn(why, plan.detail)
                self.assertIs(plan.run_kwargs, self.kwargs)
                self.assertEqual(plan.ohlcv_path, self.chart)

    def test_a_cut_last_bar_is_completed_from_the_chart_bar(self) -> None:
        # Both feeds end at TradingView's range end, three hours into the last day.
        daily, minutes = _xau_days(4, cut_last_after=180)
        _write_bars(self.chart, daily)
        _write_bars(self.finer, minutes)
        plan = self.plan()
        self.assertEqual(plan.status, "declared")
        last = daily[-1]
        partial = [m for m in minutes if m[0] >= last[0]]
        # At the last day's last minute (16:59 New York, the previous day's
        # last minute stepped by the stamps): the rest of the chart bar.
        self.assertEqual(plan.run_kwargs["ohlcv_tail_bar"],
                         (last[0] + DAY - MIN, partial[-1][4], last[2], last[3], last[4],
                          last[5] - sum(m[5] for m in partial)))
        self.assertIn(f"completed from the chart bar at {run_strategy._fmt_utc_ms(last[0] + DAY - MIN)}",
                      plan.detail)
        # Its bars are the chart's own bar.
        o = partial[0][1]
        h = max(max(m[2] for m in partial), last[2])
        l = min(min(m[3] for m in partial), last[3])
        self.assertEqual((o, h, l, last[4]), (last[1], last[2], last[3], last[4]))

    def test_a_last_bar_the_feed_cannot_complete(self) -> None:
        last = run_strategy._fmt_utc_ms(XAU_DAY0 + 3 * DAY)
        cases = []
        # The feed stops before the last bar's first minute: the magnified
        # run would end a day early, where the chart run does not.
        cases.append((self.daily, [m for m in self.minutes if m[0] < XAU_DAY0 + 3 * DAY],
                      f"no finer bar in the daily bar of {last}"))
        # The feed's part of the cut last bar opens elsewhere than the bar.
        daily, minutes = _xau_days(4, cut_last_after=180)
        minutes = [list(m) for m in minutes]
        first = next(i for i, m in enumerate(minutes) if m[0] >= XAU_DAY0 + 3 * DAY)
        minutes[first][1] += 0.125
        cases.append((daily, [tuple(m) for m in minutes],
                      f"the daily bar of {last} opens at "))
        # A chart of one bar the feed stops inside: no earlier bar to step
        # its last minute from.
        daily, minutes = _xau_days(1, cut_last_after=180)
        cases.append((daily, minutes, "the finer feed stops inside the chart's one daily bar"))
        for daily, minutes, why in cases:
            with self.subTest(why=why):
                _write_bars(self.chart, [tuple(r) for r in daily])
                _write_bars(self.finer, minutes)
                plan = self.plan()
                self.assertEqual(plan.status, "declared-not-run")
                self.assertIn(why, plan.detail)
                self.assertIs(plan.run_kwargs, self.kwargs)

    def test_an_empty_feed_rebuilds_nothing(self) -> None:
        self.finer.write_text("", encoding="utf-8")
        plan = self.plan()
        self.assertEqual(plan.status, "declared-not-run")
        self.assertIn("the finer feed is empty", plan.detail)

    def test_float_stamps_are_the_feed_loader_s(self) -> None:
        with self.finer.open("w", encoding="utf-8") as f:
            f.write("timestamp,open,high,low,close,volume\n")
            for row in self.minutes:
                f.write(",".join([f"{float(row[0]):.1f}"] + [str(x) for x in row[1:]]) + "\n")
        self.assertEqual(self.plan().status, "declared")

    def test_a_daily_chart_spelled_in_minutes_is_1d(self) -> None:
        # No script_tf: the chart's bars say 1440 minutes, the calendar's 1D.
        plan = _declared_magnifier_plan({}, self.chart, self.kwargs,
                                        {MAGNIFIER_FEED_ENV: str(self.finer)})
        self.assertEqual(plan.status, "declared")
        self.assertEqual(plan.run_kwargs["script_tf"], "1D")
        self.assertEqual(list(plan.run_kwargs["native_security_feeds"]), ["1D"])

    def test_the_tail_bar_follows_the_loaded_bars(self) -> None:
        bars = (BarC * 2)(BarC(1.0, 2.0, 0.5, 1.5, 3.0, 1000), BarC(1.5, 2.5, 1.0, 2.0, 3.0, 2000))
        out, n = _with_tail_bar(bars, 2, (3000, 2.0, 4.0, 0.25, 3.5, 7.0))
        self.assertEqual(n, 3)
        self.assertEqual([out[i].timestamp for i in range(3)], [1000, 2000, 3000])
        self.assertEqual((out[2].open, out[2].high, out[2].low, out[2].close, out[2].volume),
                         (2.0, 4.0, 0.25, 3.5, 7.0))
        # Not after the last bar: the bars as they were.
        same, n = _with_tail_bar(bars, 2, (2000, 2.0, 4.0, 0.25, 3.5, 7.0))
        self.assertIs(same, bars)
        self.assertEqual(n, 2)


class _Lib:
    """A loaded library: ``flag`` None lacks the export."""
    def __init__(self, flag: int | None) -> None:
        if flag is not None:
            self.strategy_declares_bar_magnifier = lambda: flag


class DeclarationTests(unittest.TestCase):
    def test_the_export_is_read(self) -> None:
        for flag, declared in ((None, False), (1, True), (0, False)):
            with self.subTest(flag=flag):
                strat = object.__new__(run_strategy.Strategy)
                strat.lib = _Lib(flag)
                self.assertIs(strat.declares_bar_magnifier, declared)


class _FakeStrategy:
    calls: list[dict] = []
    declares_bar_magnifier = True

    def __init__(self, so_path: Path) -> None:
        self.lib = None

    def run(self, bars_csv: Path, params=None, **kwargs) -> dict:
        _FakeStrategy.calls.append({"bars_csv": bars_csv, **kwargs})
        trades = [{"is_long": True, "entry_time": entry, "exit_time": EXIT,
                   "entry_price": 10.0, "exit_price": 10.5, "qty": 1.0, "pnl": 0.5,
                   "pnl_pct": 5.0, "max_runup": 0.5, "max_drawdown": 0.0,
                   "commission": 0.0, "entry_bar_index": 0, "exit_bar_index": 1,
                   "open_at_end": False} for entry in (EARLY, ENTRY)]
        return {"trades": trades, "trace": [], "trace_names": [],
                "net_profit": 1.0, "input_bars_processed": 0}


class MainTests(unittest.TestCase):
    def _run(self, d: Path, declares: bool, env: dict, script_tf: str = "15") -> str:
        chart = d / "chart.csv"
        _write_feed(chart, list(range(RANGE_START, RANGE_END + 1, MIN15)))
        with (d / "tv_trades.csv").open("w", encoding="utf-8-sig") as f:
            f.write("Trade number,Type,Date and time,Signal,Price USDT\n")
            f.write(f"1,Exit long,{_taipei(EXIT)},X,10.5\n")
            f.write(f"1,Entry long,{_taipei(ENTRY)},L,10\n")
        (d / "metrics.json").write_text(json.dumps(METRICS), encoding="utf-8")
        (d / "inputs.json").write_text(json.dumps({
            "tv_trades_csv_tz": "utc_plus_8", "input_tf": script_tf, "script_tf": script_tf}),
            encoding="utf-8")
        _FakeStrategy.calls.clear()
        _FakeStrategy.declares_bar_magnifier = declares
        argv = ["run_strategy.py", str(d), "--ohlcv", str(chart), "-o", str(d / "out.csv"),
                "--disable-trading-before-window"]
        buf = io.StringIO()
        with mock.patch.object(run_strategy, "ensure_derived", lambda: None), \
                mock.patch.object(run_strategy, "find_strategy_lib",
                                  lambda d, so_name="strategy.so": d / so_name), \
                mock.patch.object(run_strategy, "Strategy", _FakeStrategy), \
                mock.patch.object(run_strategy, "REFERENCE_OHLCV", chart), \
                mock.patch.dict(run_strategy.os.environ, env, clear=False), \
                mock.patch.object(sys, "argv", argv), \
                contextlib.redirect_stdout(buf):
            self.assertEqual(run_strategy.main(), 0)
        return buf.getvalue()

    def test_a_declaring_script_runs_magnified(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            finer = d / "finer.csv"
            # The 1m feed starts a day before the chart feed (and TV's range).
            _write_feed(finer, list(range(RANGE_START - DAY, RANGE_END + MIN15, MIN)))
            printed = self._run(d, True, {MAGNIFIER_FEED_ENV: str(finer),
                                          MAGNIFIER_FEED_SHA256_ENV: _sha(finer)})
            call = _FakeStrategy.calls[0]
            self.assertEqual(call["bars_csv"], finer.resolve())
            self.assertEqual((call["input_tf"], call["script_tf"], call["bar_magnifier"]),
                             ("1", "15", True))
            self.assertEqual(call["ohlcv_start_ms"], RANGE_START)
            self.assertEqual(call["ohlcv_end_ms"], RANGE_END + MIN15 - 1)
            self.assertIn(f"  magnifier: declared: run on the 1m feed finer.csv (sha256 "
                          f"{_sha(finer)}), input_tf=1 script_tf=15, magnifier on", printed)
            # TradingView's window is the chart feed's, exactly as for the
            # same probe unmagnified, and so are the rows written.
            rows = (d / "out.csv").read_text(encoding="utf-8")
            # Both entries are written: the report bound is the chart's.
            self.assertEqual(rows.count("Entry long"), 2)
            self._run(d, False, {MAGNIFIER_FEED_ENV: str(finer)})
            plain = _FakeStrategy.calls[0]
            self.assertEqual(call["trade_start_time_ms"], plain["trade_start_time_ms"])
            self.assertEqual(rows, (d / "out.csv").read_text(encoding="utf-8"))

    def test_unchanged_without_the_declaration_or_the_feed(self) -> None:
        for declares, env in ((False, {MAGNIFIER_FEED_ENV: "/nonexistent.csv"}),
                              (True, {MAGNIFIER_FEED_ENV: ""})):
            with self.subTest(declares=declares), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp)
                printed = self._run(d, declares, env)
                call = _FakeStrategy.calls[0]
                self.assertEqual(call["bars_csv"], (d / "chart.csv").resolve())
                self.assertEqual((call["input_tf"], call["script_tf"], call["bar_magnifier"]),
                                 ("15", "15", False))
                self.assertEqual(call["ohlcv_end_ms"], RANGE_END)
                if declares:
                    self.assertIn("  magnifier: declared-not-run: no magnifier feed", printed)
                else:
                    self.assertNotIn("magnifier:", printed)

    def test_a_daily_chart_the_feed_does_not_rebuild_reports_declared_not_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            finer = d / "finer.csv"
            _write_feed(finer, [RANGE_START])
            printed = self._run(d, True, {MAGNIFIER_FEED_ENV: str(finer)}, script_tf="1D")
            call = _FakeStrategy.calls[0]
            self.assertIs(call["bar_magnifier"], False)
            self.assertIn("  magnifier: declared-not-run: the finer feed does not rebuild the "
                          "chart's own daily bars (", printed)

    def test_a_weekly_chart_reports_declared_not_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            finer = d / "finer.csv"
            _write_feed(finer, [RANGE_START])
            printed = self._run(d, True, {MAGNIFIER_FEED_ENV: str(finer)}, script_tf="W")
            call = _FakeStrategy.calls[0]
            self.assertIs(call["bar_magnifier"], False)
            self.assertIn("  magnifier: declared-not-run: chart W is coarser than one day", printed)


if __name__ == "__main__":
    unittest.main()
