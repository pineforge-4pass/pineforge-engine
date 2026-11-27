#!/usr/bin/env python3
"""run_strategy.py declares the reference run's first computed chart bar to the
engine as the series origin.

TradingView computes a script from the first chart bar of its backtest range
and from no earlier bar. A tape's metrics.json records that bar as
``rangeProvenance.returnedRange.from`` (_tv_first_bar). When it is recorded,
the driver declares it to the engine as ``series_origin_ms``; when it is not,
nothing is declared. The chart ta.ema seeding does not hang on the
declaration: the engine's default is already TradingView's (na for length - 1
bars, then the simple average of the first length values), and the driver
never writes ``chart_ema_na_warmup`` itself.

What the rows hold apart:

* the origin is the reference's first computed bar: not the bar the trading
  window opens on, not the loaded feed's first bar, not ``ohlcv_start_ms``;
* the loaded feed is left as it is: bars loaded before the origin stay loaded,
  and a feed that starts after the origin is not padded;
* the request.security warm-up is not switched on by the declaration: the
  driver writes ``security_range_start_na_warmup`` = 0 after the origin unless
  the probe sets that key itself;
* a probe's own ``chart_ema_na_warmup``, ``security_range_start_na_warmup`` or
  ``series_origin_ms`` wins.

Public synthetic feeds, tapes and metrics only; the engine is stubbed out, so
the rows read what the driver hands the strategy library.
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

import run_strategy
import pf_release_run

MIN15 = 15 * 60 * 1000
ORIGIN_KEY = "series_origin_ms"
CHART_KEY = "chart_ema_na_warmup"
SECURITY_KEY = "security_range_start_na_warmup"


def _utc_ms(y: int, m: int, d: int, hh: int = 0, mm: int = 0) -> int:
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


def _taipei(ms: int) -> str:
    """A tape stamp as the campaign's verifier writes it (utc_plus_8)."""
    tz = timezone(timedelta(hours=8))
    return datetime.fromtimestamp(ms / 1000.0, tz=tz).strftime("%Y-%m-%d %H:%M")


# TradingView's first computed bar, its first entry and that trade's exit.
ORIGIN = _utc_ms(2025, 4, 1)
ENTRY = ORIGIN + 60 * MIN15
EXIT = ENTRY + 8 * MIN15
RANGE_TO = _utc_ms(2026, 5, 1)
FEED_END = ORIGIN + 3 * 96 * MIN15

RECORDED = {
    "symbol": "BINANCE:ETHUSDT.P", "interval": "15",
    "from": "2025-04-01", "to": "2026-05-01", "deepBacktesting": True,
    "rangeProvenance": {
        "schemaVersion": 1,
        "requestedRange": {"from": ORIGIN, "to": RANGE_TO},
        "returnedRange": {"from": ORIGIN, "to": RANGE_TO},
        "rangeProof": "covered",
    },
}
# A date-only export: no returned range, so no first computed bar.
DATE_ONLY = {"symbol": "BINANCE:ETHUSDT.P", "interval": "15",
             "from": "2025-04-01", "to": "2026-05-01", "deepBacktesting": True}


def _feed(first: int) -> list[int]:
    return list(range(first, FEED_END + 1, MIN15))


def _trade(entry_ms: int, exit_ms: int) -> dict:
    return {
        "is_long": True, "entry_time": entry_ms, "exit_time": exit_ms,
        "entry_price": 10.0, "exit_price": 10.5, "qty": 1.0,
        "pnl": 0.5, "pnl_pct": 5.0, "max_runup": 0.5, "max_drawdown": 0.0,
        "commission": 0.0, "entry_bar_index": 0, "exit_bar_index": 1,
        "open_at_end": False,
    }


class _FakeStrategy:
    """Stands in for Strategy: records the run kwargs, returns ``trades``."""
    calls: list[dict] = []
    declares_bar_magnifier = False
    trades_by_call: list[list[dict]] = []

    def __init__(self, so_path: Path) -> None:
        self.lib = None

    def run(self, bars_csv: Path, params=None, **kwargs) -> dict:
        _FakeStrategy.calls.append({"bars_csv": bars_csv, **kwargs})
        index = min(len(_FakeStrategy.calls), len(_FakeStrategy.trades_by_call)) - 1
        trades = _FakeStrategy.trades_by_call[index]
        return {
            "trades": [dict(t) for t in trades],
            "trace": [], "trace_names": [],
            "net_profit": sum(t["pnl"] for t in trades),
            "input_bars_processed": 0,
        }


def _probe(d: Path, stamps: list[int], *, metrics: dict | None = RECORDED,
           inputs: dict | None = None) -> tuple[Path, Path]:
    feed = d / "feed.csv"
    with feed.open("w", encoding="utf-8") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for ts in stamps:
            f.write(f"{ts},10,11,9,10.5,100\n")
    with (d / "tv_trades.csv").open("w", encoding="utf-8-sig") as f:
        f.write("Trade number,Type,Date and time,Signal,Price USD\n")
        f.write(f"1,Exit long,{_taipei(EXIT)},X,10.5\n")
        f.write(f"1,Entry long,{_taipei(ENTRY)},L,10\n")
    if metrics is not None:
        (d / "metrics.json").write_text(json.dumps(metrics), encoding="utf-8")
    (d / "inputs.json").write_text(
        json.dumps({"tv_trades_csv_tz": "utc_plus_8", **(inputs or {})}), encoding="utf-8")
    return feed, d / "engine_trades.csv"


def _run_main(strategy_dir: Path, feed: Path, out: Path, *extra: str,
              trades_by_call: list[list[dict]] | None = None) -> str:
    """Drive run_strategy.main() end to end with the engine stubbed out;
    returns everything it printed (stdout, then stderr)."""
    _FakeStrategy.calls.clear()
    _FakeStrategy.trades_by_call = trades_by_call or [[_trade(ENTRY, EXIT)]]
    argv = ["run_strategy.py", str(strategy_dir), "--ohlcv", str(feed),
            "-o", str(out), *extra]
    printed = io.StringIO()
    errors = io.StringIO()
    with mock.patch.object(run_strategy, "ensure_derived", lambda: None), \
            mock.patch.object(run_strategy, "find_strategy_lib",
                              lambda d, so_name="strategy.so": d / so_name), \
            mock.patch.object(run_strategy, "Strategy", _FakeStrategy), \
            mock.patch.object(run_strategy, "REFERENCE_OHLCV", feed), \
            mock.patch.object(sys, "argv", argv), \
            contextlib.redirect_stdout(printed), contextlib.redirect_stderr(errors):
        rc = run_strategy.main()
    assert rc == 0, printed.getvalue() + errors.getvalue()
    return printed.getvalue() + errors.getvalue()


def _metadata(call: dict) -> dict:
    """The syminfo metadata of one engine run, in the order it is applied."""
    return dict(call.get("syminfo_metadata") or {})


def _origin_line(printed: str) -> str:
    lines = [line for line in printed.splitlines() if line.startswith("  series-origin: ")]
    return lines[0] if len(lines) == 1 else ""


class DeclaredOriginTests(unittest.TestCase):
    """A tape that records TradingView's first computed bar."""

    def _run(self, first_feed_bar: int, **kwargs):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, _feed(first_feed_bar), inputs=kwargs.pop("inputs", None))
            printed = _run_main(d, feed, out, **kwargs)
            # The driver hands the engine the resolved path.
            return printed, [dict(call) for call in _FakeStrategy.calls], feed.resolve()

    def test_a_feed_that_starts_on_the_reference_first_bar(self) -> None:
        printed, calls, feed = self._run(ORIGIN)
        self.assertEqual(len(calls), 1)
        metadata = _metadata(calls[0])
        # The origin first, then the request.security warm-up left off.
        self.assertEqual(list(metadata.items())[:2], [(ORIGIN_KEY, ORIGIN), (SECURITY_KEY, 0)])
        self.assertNotIn(CHART_KEY, metadata)
        self.assertEqual(calls[0]["bars_csv"], feed)
        self.assertIsNone(calls[0].get("ohlcv_start_ms"))
        line = _origin_line(printed)
        self.assertIn("declared 2025-04-01 00:00 UTC", line)
        self.assertIn("metrics.json rangeProvenance.returnedRange.from", line)
        self.assertIn("where the loaded feed starts", line)

    def test_bars_loaded_before_the_origin_stay_loaded(self) -> None:
        first = ORIGIN - 20 * MIN15
        printed, calls, feed = self._run(first)
        self.assertEqual(len(calls), 1)
        metadata = _metadata(calls[0])
        # The origin is the reference's bar: not the feed's first bar, not the
        # bar the trading window opens on.
        self.assertEqual(metadata.get(ORIGIN_KEY), ORIGIN)
        self.assertEqual(calls[0]["trade_start_time_ms"], ENTRY - MIN15)
        self.assertNotEqual(metadata.get(ORIGIN_KEY), calls[0]["trade_start_time_ms"])
        self.assertNotEqual(metadata.get(ORIGIN_KEY), first)
        # The feed is handed over whole: nothing trims it to the origin.
        self.assertEqual(calls[0]["bars_csv"], feed)
        self.assertIsNone(calls[0].get("ohlcv_start_ms"))
        line = _origin_line(printed)
        self.assertIn("declared 2025-04-01 00:00 UTC", line)
        self.assertIn("20 bar(s) earlier, at 2025-03-31 19:00 UTC", line)

    def test_a_feed_that_starts_after_the_origin_lacks_reference_bars(self) -> None:
        first = ORIGIN + 8 * MIN15
        printed, calls, feed = self._run(first)
        self.assertEqual(len(calls), 1)
        metadata = _metadata(calls[0])
        # The reference's bar is declared as it is, earlier than the feed.
        self.assertEqual(list(metadata.items())[:2], [(ORIGIN_KEY, ORIGIN), (SECURITY_KEY, 0)])
        self.assertLess(metadata.get(ORIGIN_KEY, first), first)
        self.assertEqual(calls[0]["trade_start_time_ms"], ENTRY - MIN15)
        self.assertEqual(calls[0]["bars_csv"], feed)
        self.assertIsNone(calls[0].get("ohlcv_start_ms"))
        line = _origin_line(printed)
        self.assertIn("declared 2025-04-01 00:00 UTC", line)
        self.assertIn("starts later, at 2025-04-01 02:00 UTC", line)
        self.assertIn("without the reference's earlier bars", line)

    def test_a_probe_trim_is_not_the_origin(self) -> None:
        # ohlcv_start_ms cuts the loaded feed; the origin stays the reference's.
        trim = ORIGIN + 4 * MIN15
        printed, calls, _ = self._run(ORIGIN - 20 * MIN15, inputs={"ohlcv_start_ms": trim})
        metadata = _metadata(calls[0])
        self.assertEqual(metadata.get(ORIGIN_KEY), ORIGIN)
        self.assertEqual(calls[0].get("ohlcv_start_ms"), trim)
        self.assertIn("starts later, at 2025-04-01 01:00 UTC", _origin_line(printed))

    def test_a_run_repeated_from_the_signal_bar_keeps_the_origin(self) -> None:
        # The first run opens its window on TradingView's first bar and enters
        # before TradingView's first entry, so it runs again from the signal
        # bar: the window moves, the origin does not.
        early = _trade(ORIGIN + 2 * MIN15, ORIGIN + 6 * MIN15)
        printed, calls, _ = self._run(
            ORIGIN, trades_by_call=[[early, _trade(ENTRY, EXIT)], [_trade(ENTRY, EXIT)]])
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0]["trade_start_time_ms"], ORIGIN)
        self.assertEqual(calls[1]["trade_start_time_ms"], ENTRY - MIN15)
        for call in calls:
            self.assertEqual(_metadata(call).get(ORIGIN_KEY), ORIGIN)
        self.assertEqual(printed.count("  series-origin: "), 1)

    def test_a_probe_that_switches_the_chart_seeding_off_wins(self) -> None:
        printed, calls, _ = self._run(
            ORIGIN, inputs={"runtime_overrides": {"syminfo_metadata": {CHART_KEY: 0}}})
        metadata = _metadata(calls[0])
        keys = list(metadata)
        self.assertEqual(keys[:2], [ORIGIN_KEY, SECURITY_KEY])
        self.assertEqual(metadata.get(ORIGIN_KEY), ORIGIN)
        # Applied after the origin, so the engine's last writer is the probe.
        self.assertEqual(metadata.get(CHART_KEY), 0)
        self.assertGreater(keys.index(CHART_KEY), keys.index(ORIGIN_KEY))
        self.assertIn("chart_ema_na_warmup=0", _origin_line(printed))

    def test_a_probe_that_sets_the_security_warm_up_keeps_it(self) -> None:
        printed, calls, _ = self._run(
            ORIGIN, inputs={"runtime_overrides": {"syminfo_metadata": {SECURITY_KEY: ORIGIN}}})
        metadata = _metadata(calls[0])
        keys = list(metadata)
        self.assertEqual(keys[0], ORIGIN_KEY)
        self.assertEqual(metadata.get(ORIGIN_KEY), ORIGIN)
        self.assertEqual(metadata.get(SECURITY_KEY), ORIGIN)
        self.assertGreater(keys.index(SECURITY_KEY), keys.index(ORIGIN_KEY))
        self.assertIn("as the probe sets it", _origin_line(printed))

    def test_lot_metadata_is_kept(self) -> None:
        _, calls, _ = self._run(ORIGIN, inputs={"runtime_overrides": {"mincontract": 0.25}})
        metadata = _metadata(calls[0])
        self.assertEqual(list(metadata.items())[:2], [(ORIGIN_KEY, ORIGIN), (SECURITY_KEY, 0)])
        self.assertEqual(metadata.get("mincontract"), 0.25)

    def test_the_docker_runner_forwards_the_declared_origin_and_warmup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, _feed(ORIGIN))
            (d / "generated.cpp").write_text("// generated\n")
            with mock.patch.object(pf_release_run, "run_release", return_value={}) as launch:
                printed = _run_main(d, feed, out, "--runner", "docker")
        launch.assert_called_once()
        metadata = launch.call_args.kwargs["syminfo"] or {}
        self.assertEqual(list(metadata.get("syminfo_metadata", {}).items()),
                         [(ORIGIN_KEY, ORIGIN), (SECURITY_KEY, 0)])
        self.assertNotIn("--runner docker does not apply the declared series origin", printed)


class UndeclaredOriginTests(unittest.TestCase):
    """Runs the driver declares nothing for."""

    def test_a_tape_without_a_returned_range(self) -> None:
        for name, metrics in (("date-only export", DATE_ONLY), ("no metrics.json", None)):
            with self.subTest(case=name), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp)
                feed, out = _probe(d, _feed(ORIGIN), metrics=metrics)
                _run_main(d, feed, out)
                self.assertEqual(len(_FakeStrategy.calls), 1)
                metadata = _metadata(_FakeStrategy.calls[0])
                self.assertNotIn(ORIGIN_KEY, metadata)
                self.assertNotIn(SECURITY_KEY, metadata)
                self.assertNotIn(CHART_KEY, metadata)

    def test_a_tape_without_a_returned_range_says_so(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, _feed(ORIGIN), metrics=DATE_ONLY)
            printed = _run_main(d, feed, out)
        self.assertIn("not declared", _origin_line(printed))

    def test_a_run_without_a_tape_window(self) -> None:
        # --no-trim-output and --emit-window-ohlcv measure the feed they were
        # given: no tape window, no range end, no origin.
        for extra in (("--no-trim-output",), ("--emit-window-ohlcv", "FEED")):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp)
                feed, out = _probe(d, _feed(ORIGIN))
                argv = [str(feed) if value == "FEED" else value for value in extra]
                printed = _run_main(d, feed, out, *argv)
                self.assertEqual(len(_FakeStrategy.calls), 1)
                self.assertNotIn(ORIGIN_KEY, _metadata(_FakeStrategy.calls[0]))
                self.assertNotIn(SECURITY_KEY, _metadata(_FakeStrategy.calls[0]))
                self.assertEqual(_origin_line(printed), "")

    def test_a_probe_that_declares_its_own_origin_keeps_it(self) -> None:
        own = ORIGIN - 4 * MIN15
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed, out = _probe(d, _feed(ORIGIN), inputs={
                "runtime_overrides": {"syminfo_metadata": {ORIGIN_KEY: own}}})
            _run_main(d, feed, out)
            metadata = _metadata(_FakeStrategy.calls[0])
        # The probe's declaration is passed on as written: its own origin, and
        # no key of the driver's beside it.
        self.assertEqual(metadata.get(ORIGIN_KEY), own)
        self.assertNotIn(SECURITY_KEY, metadata)


class SeriesOriginPlanTests(unittest.TestCase):
    """The rule itself (_series_origin_plan), away from main()."""

    SOURCE = "metrics.json rangeProvenance.returnedRange.from"

    def _plan(self, metadata, tv_first_bar, feed_ts):
        plan = getattr(run_strategy, "_series_origin_plan", None)
        self.assertIsNotNone(plan, "run_strategy._series_origin_plan is missing")
        return plan(metadata, tv_first_bar, feed_ts)

    def test_the_declared_order(self) -> None:
        probe = {"qty_step": 0.5, CHART_KEY: 0, "mincontract": 0.25}
        before = dict(probe)
        plan = self._plan(probe, (ORIGIN, self.SOURCE), _feed(ORIGIN))
        self.assertEqual(list(plan.metadata.items()), [
            (ORIGIN_KEY, ORIGIN), (SECURITY_KEY, 0),
            ("qty_step", 0.5), (CHART_KEY, 0), ("mincontract", 0.25)])
        self.assertTrue(plan.declared)
        # The probe's own mapping is not written to.
        self.assertEqual(probe, before)

    def test_no_metadata_at_all(self) -> None:
        plan = self._plan(None, (ORIGIN, self.SOURCE), _feed(ORIGIN))
        self.assertEqual(list(plan.metadata.items()), [(ORIGIN_KEY, ORIGIN), (SECURITY_KEY, 0)])
        self.assertTrue(plan.declared)

    def test_nothing_recorded_changes_nothing(self) -> None:
        for metadata in (None, {"mincontract": 0.25}, {CHART_KEY: 1}):
            with self.subTest(metadata=metadata):
                plan = self._plan(metadata, None, _feed(ORIGIN))
                self.assertFalse(plan.declared)
                self.assertEqual(plan.metadata, metadata)
                self.assertIn("not declared", plan.detail)

    def test_the_probe_origin_is_kept(self) -> None:
        own = {ORIGIN_KEY: ORIGIN - MIN15, "mincontract": 0.25}
        plan = self._plan(own, (ORIGIN, self.SOURCE), _feed(ORIGIN))
        self.assertFalse(plan.declared)
        self.assertEqual(list(plan.metadata.items()), list(own.items()))
        self.assertIn("the probe's own series_origin_ms", plan.detail)

    def test_an_empty_feed(self) -> None:
        plan = self._plan(None, (ORIGIN, self.SOURCE), [])
        self.assertTrue(plan.declared)
        self.assertEqual(plan.metadata.get(ORIGIN_KEY), ORIGIN)
        self.assertIn("no bar", plan.detail)


if __name__ == "__main__":
    unittest.main()
