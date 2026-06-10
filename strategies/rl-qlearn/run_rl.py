#!/usr/bin/env python3
"""RL demo backtest: load strategies/rl-qlearn/strategy.so, feed the corpus
ETH-USDT-USDT 1-minute bars, run the Q-learning agent on a 15-minute script
timeframe (the engine aggregates 1m -> 15m), print summary stats.

Usage:
    python3 strategies/rl-qlearn/run_rl.py [path/to/ohlcv_1m.csv]

The default data path is the corpus submodule feed
(corpus/data/ohlcv_ETH-USDT-USDT_1m.csv), falling back to a sibling
pineforge-corpus checkout.
"""
from __future__ import annotations

import csv
import ctypes
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SO = HERE / "strategy.so"

DATA_CANDIDATES = [
    ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m.csv",
    ROOT.parent / "pineforge-corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m.csv",
]

INPUT_TF = b"1"    # corpus feed resolution: 1 minute
SCRIPT_TF = b"15"  # strategy decision timeframe: 15 minutes


# ctypes mirror of <pineforge/pineforge.h> (same layout as tutorial/run.py)
class BarC(ctypes.Structure):
    _fields_ = [("open",  ctypes.c_double), ("high",   ctypes.c_double),
                ("low",   ctypes.c_double), ("close",  ctypes.c_double),
                ("volume",ctypes.c_double), ("timestamp", ctypes.c_int64)]

class TradeC(ctypes.Structure):
    _fields_ = [("entry_time",  ctypes.c_int64), ("exit_time",  ctypes.c_int64),
                ("entry_price", ctypes.c_double),("exit_price", ctypes.c_double),
                ("pnl",         ctypes.c_double),("pnl_pct",    ctypes.c_double),
                ("is_long",     ctypes.c_int),   ("max_runup",  ctypes.c_double),
                ("max_drawdown",ctypes.c_double),("qty",        ctypes.c_double)]

class _Diag(ctypes.Structure):
    _fields_ = [("sec_id", ctypes.c_int), ("feed_count", ctypes.c_int64),
                ("eval_complete_count", ctypes.c_int64),
                ("eval_partial_count",  ctypes.c_int64)]

class _Trace(ctypes.Structure):
    _fields_ = [("timestamp", ctypes.c_int64), ("bar_index", ctypes.c_int32),
                ("name_id",   ctypes.c_int32), ("value",     ctypes.c_double)]

class ReportC(ctypes.Structure):
    _fields_ = [("total_trades", ctypes.c_int),
                ("trades", ctypes.POINTER(TradeC)), ("trades_len", ctypes.c_int),
                ("net_profit", ctypes.c_double),
                ("input_bars_processed",         ctypes.c_int64),
                ("script_bars_processed",        ctypes.c_int64),
                ("security_feeds_total",         ctypes.c_int64),
                ("security_eval_complete_total", ctypes.c_int64),
                ("security_eval_partial_total",  ctypes.c_int64),
                ("magnifier_sub_bars_total",     ctypes.c_int64),
                ("magnifier_sample_ticks_total", ctypes.c_int64),
                ("input_tf_seconds",  ctypes.c_int),
                ("script_tf_seconds", ctypes.c_int),
                ("script_tf_ratio",   ctypes.c_int),
                ("needs_aggregation", ctypes.c_int),
                ("bar_magnifier_enabled", ctypes.c_int),
                ("security_diag", ctypes.POINTER(_Diag)),
                ("security_diag_len", ctypes.c_int),
                ("trace", ctypes.POINTER(_Trace)), ("trace_len", ctypes.c_int),
                ("trace_names", ctypes.POINTER(ctypes.c_char_p)),
                ("trace_names_len", ctypes.c_int)]


def fmt_ts(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def main() -> int:
    if not SO.exists():
        sys.exit("strategy.so missing — build it first:\n"
                 "  cmake -B build -S . -DPINEFORGE_BUILD_RL_STRATEGY=ON\n"
                 "  cmake --build build --target strategy_rl_qlearn -j")

    if len(sys.argv) > 1:
        data = Path(sys.argv[1])
    else:
        data = next((p for p in DATA_CANDIDATES if p.exists()), None)
    if data is None or not data.exists():
        sys.exit("1m OHLCV feed not found — init the corpus submodule or pass a CSV path")

    with data.open(newline="") as f:
        rows = list(csv.DictReader(f))
    n = len(rows)
    bars = (BarC * n)()
    for i, r in enumerate(rows):
        bars[i] = BarC(float(r["open"]), float(r["high"]), float(r["low"]),
                       float(r["close"]), float(r["volume"]), int(r["timestamp"]))

    lib = ctypes.CDLL(str(SO))
    lib.strategy_create.argtypes  = [ctypes.c_char_p]
    lib.strategy_create.restype   = ctypes.c_void_p
    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC)]
    lib.strategy_free.argtypes    = [ctypes.c_void_p]
    lib.report_free.argtypes      = [ctypes.POINTER(ReportC)]
    if hasattr(lib, "strategy_get_last_error"):
        lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
        lib.strategy_get_last_error.restype  = ctypes.c_char_p

    state, report = lib.strategy_create(b"{}"), ReportC()
    t0 = time.time()
    lib.run_backtest_full(state, bars, n, INPUT_TF, SCRIPT_TF, 0, 4, 3,
                          ctypes.byref(report))
    elapsed = time.time() - t0
    if hasattr(lib, "strategy_get_last_error"):
        err_ptr = lib.strategy_get_last_error(state)
        if err_ptr:
            err_msg = err_ptr.decode("utf-8", "replace")
            if err_msg:
                lib.report_free(ctypes.byref(report))
                lib.strategy_free(state)
                print(f"engine error: {err_msg}", file=sys.stderr)
                return 1

    trades = [report.trades[i] for i in range(report.trades_len)]
    pnls = [t.pnl for t in trades]
    wins, losses = sum(p > 0 for p in pnls), sum(p < 0 for p in pnls)
    gross_win = sum(p for p in pnls if p > 0)
    gross_loss = -sum(p for p in pnls if p < 0)
    cum = peak = max_dd = 0.0
    for p in pnls:
        cum += p; peak = max(peak, cum); max_dd = min(max_dd, cum - peak)

    # Learning-effect split: PnL of trades exited in the first vs second half
    # of the test window (the agent learns online, so the second half runs on
    # a mostly-converged Q-table with low epsilon).
    mid_ts = (bars[0].timestamp + bars[n - 1].timestamp) // 2
    first_half = sum(t.pnl for t in trades if t.exit_time and t.exit_time <= mid_ts)
    second_half = sum(t.pnl for t in trades if t.exit_time and t.exit_time > mid_ts)

    # Buy & hold benchmark for the same 1-unit position size.
    bh = bars[n - 1].close - bars[0].close

    print(f"RL Q-learning agent on ETH-USDT-USDT — 1m feed aggregated to 15m script TF")
    print(f"  data:        {data.name}: {n} x 1m bars -> "
          f"{report.script_bars_processed} x 15m bars, "
          f"{fmt_ts(bars[0].timestamp)} -> {fmt_ts(bars[-1].timestamp)} UTC")
    print(f"  trades:      {report.trades_len}  "
          f"({wins}W / {losses}L, "
          f"{wins / report.trades_len * 100 if report.trades_len else 0:.1f}% win)")
    print(f"  net pnl:     {report.net_profit:+.2f} USDT (1 ETH/position)")
    print(f"  profit fact: {gross_win / gross_loss if gross_loss else float('inf'):.3f}")
    print(f"  best/worst:  {(max(pnls) if pnls else 0):+.2f} / {(min(pnls) if pnls else 0):+.2f}")
    print(f"  max dd:      {max_dd:.2f}")
    print(f"  1st half:    {first_half:+.2f}   2nd half: {second_half:+.2f}  "
          f"(online learning: later trades use the trained Q-table)")
    print(f"  buy & hold:  {bh:+.2f} over the same window")
    print(f"  elapsed:     {elapsed * 1000:.1f} ms")

    lib.report_free(ctypes.byref(report))
    lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
