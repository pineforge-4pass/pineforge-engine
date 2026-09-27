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
window is still walked on the chart feed (_declared_magnifier_plan). Daily
and coarser charts report ``declared-not-run``.
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
    _declared_magnifier_plan,
    _tf_seconds,
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

    def test_chart_tf_from_the_feed_when_the_probe_names_none(self) -> None:
        plan = _declared_magnifier_plan({}, self.chart, self.kwargs, self.env())
        self.assertEqual(plan.status, "declared")
        self.assertEqual(plan.run_kwargs["script_tf"], "15")

    def test_not_run(self) -> None:
        cases = (
            ({"script_tf": "1D"}, self.kwargs, self.env(), "chart 1D is daily or coarser"),
            ({"script_tf": "W"}, self.kwargs, self.env(), "chart W is daily or coarser"),
            ({"script_tf": "M"}, self.kwargs, self.env(), "chart M is daily or coarser"),
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

    def test_a_daily_chart_reports_declared_not_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            finer = d / "finer.csv"
            _write_feed(finer, [RANGE_START])
            printed = self._run(d, True, {MAGNIFIER_FEED_ENV: str(finer)}, script_tf="1D")
            call = _FakeStrategy.calls[0]
            self.assertIs(call["bar_magnifier"], False)
            self.assertIn("  magnifier: declared-not-run: chart 1D is daily or coarser", printed)


if __name__ == "__main__":
    unittest.main()
