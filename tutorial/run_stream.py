#!/usr/bin/env python3
"""Historical OHLCV warmup -> realtime trade-stream tutorial.

The frozen tutorial dataset contains bars rather than exchange trades. To keep
this example self-contained, the final LIVE_BARS candles are expanded into a
deterministic open/high/low/close trade path. Production callers should pass
their exchange's actual ordered trade feed through the same ABI.
"""
from __future__ import annotations

import csv
import ctypes
import sys
from datetime import datetime, timezone

from run import BarC, OHLCV, ReportC, SO, check_abi

LIVE_BARS = 32
INPUT_TF_MS = 15 * 60 * 1000


class TradeTickC(ctypes.Structure):
    """ctypes mirror of pf_trade_tick_t."""

    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("trade_id", ctypes.c_uint64),
        ("price", ctypes.c_double),
        ("qty", ctypes.c_double),
        ("is_buyer_maker", ctypes.c_int),
    ]


def error_message(lib: ctypes.CDLL, state: int) -> str:
    raw = lib.strategy_get_last_error(state)
    return raw.decode("utf-8", "replace") if raw else "unknown engine error"


def check_call(lib: ctypes.CDLL, state: int, status: int, operation: str) -> None:
    if status != 0:
        raise RuntimeError(f"{operation}: {error_message(lib, state)}")


def price_path(bar: BarC) -> tuple[float, float, float, float]:
    """Choose the same nearest-extreme-first convention as an OHLC emulator."""
    if abs(bar.open - bar.high) < abs(bar.open - bar.low):
        return bar.open, bar.high, bar.low, bar.close
    return bar.open, bar.low, bar.high, bar.close


def main() -> int:
    if not SO.exists():
        sys.exit("strategy.so missing — run `bash tutorial/run.sh` first")

    with OHLCV.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) <= LIVE_BARS:
        sys.exit(f"need more than {LIVE_BARS} tutorial bars")

    bars = (BarC * len(rows))()
    for i, row in enumerate(rows):
        bars[i] = BarC(
            float(row["open"]), float(row["high"]), float(row["low"]),
            float(row["close"]), float(row["volume"]), int(row["timestamp"]))

    warmup_n = len(rows) - LIVE_BARS
    tick_count = LIVE_BARS * 4
    ticks = (TradeTickC * tick_count)()
    offsets = (0, INPUT_TF_MS // 3, 2 * INPUT_TF_MS // 3,
               INPUT_TF_MS - 1)
    tick_index = 0
    trade_id = 1
    for bar in bars[warmup_n:]:
        for offset, price in zip(offsets, price_path(bar)):
            ticks[tick_index] = TradeTickC(
                bar.timestamp + offset, trade_id, price,
                bar.volume / 4.0, 0)
            tick_index += 1
            trade_id += 1

    lib = ctypes.CDLL(str(SO))
    check_abi(lib)
    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype = ctypes.c_void_p
    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes = [ctypes.POINTER(ReportC)]
    lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
    lib.strategy_get_last_error.restype = ctypes.c_char_p
    lib.strategy_stream_begin.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_stream_begin.restype = ctypes.c_int
    lib.strategy_stream_push_ticks.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(TradeTickC), ctypes.c_int]
    lib.strategy_stream_push_ticks.restype = ctypes.c_int
    lib.strategy_stream_advance_time.argtypes = [
        ctypes.c_void_p, ctypes.c_int64]
    lib.strategy_stream_advance_time.restype = ctypes.c_int
    lib.strategy_stream_end.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.strategy_stream_end.restype = ctypes.c_int
    lib.strategy_stream_fill_report.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ReportC)]
    lib.strategy_stream_fill_report.restype = ctypes.c_int

    state = lib.strategy_create(b"{}")
    if not state:
        sys.exit("strategy_create failed")
    report = ReportC()
    try:
        check_call(lib, state, lib.strategy_stream_begin(
            state, bars, warmup_n, b"15", b"15"), "stream begin")

        # One contiguous call covers the complete simulated live session.
        check_call(lib, state, lib.strategy_stream_push_ticks(
            state, ticks, tick_count), "stream ticks")

        session_end = bars[-1].timestamp + INPUT_TF_MS
        check_call(lib, state, lib.strategy_stream_advance_time(
            state, session_end), "stream advance")
        check_call(lib, state, lib.strategy_stream_end(
            state, 0), "stream end")
        check_call(lib, state, lib.strategy_stream_fill_report(
            state, ctypes.byref(report)), "stream report")

        if report.input_bars_processed != len(rows):
            raise RuntimeError(
                f"bar continuity failed: {report.input_bars_processed} != {len(rows)}")

        fmt = lambda ms: datetime.fromtimestamp(
            ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")
        print("MACD historical -> realtime stream")
        print(f"  warmup:     {warmup_n} confirmed 15m bars")
        print(f"  realtime:   {LIVE_BARS} bars from {tick_count} ordered trades")
        print(f"  handoff:    {fmt(bars[warmup_n].timestamp)} UTC")
        print(f"  processed:  {report.input_bars_processed} input / "
              f"{report.script_bars_processed} script bars")
        print(f"  trades:     {report.trades_len}")
        print(f"  net pnl:    {report.net_profit:+.2f}")
    except RuntimeError as exc:
        print(f"stream error: {exc}", file=sys.stderr)
        return 1
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
