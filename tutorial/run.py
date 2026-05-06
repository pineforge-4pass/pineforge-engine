#!/usr/bin/env python3
"""Tutorial backtest: load tutorial/macd/strategy.so, run it against
tutorial/data/btcusdt_15m_7d.csv, print summary stats."""
from __future__ import annotations

import csv
import ctypes
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT  = Path(__file__).resolve().parent
SO    = ROOT / "macd" / "strategy.so"
OHLCV = ROOT / "data" / "btcusdt_15m_7d.csv"


# ctypes mirror of <pineforge/pineforge.h>
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


def main() -> int:
    if not SO.exists():
        sys.exit(f"strategy.so missing — run `bash tutorial/run.sh` first")

    with OHLCV.open(newline="") as f:
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

    state, report = lib.strategy_create(b"{}"), ReportC()
    t0 = time.time()
    lib.run_backtest_full(state, bars, n, b"", b"", 0, 4, 3, ctypes.byref(report))
    elapsed = time.time() - t0

    pnls = [report.trades[i].pnl for i in range(report.trades_len)]
    wins, losses = sum(p > 0 for p in pnls), sum(p < 0 for p in pnls)
    cum = peak = max_dd = 0.0
    for p in pnls:
        cum += p; peak = max(peak, cum); max_dd = min(max_dd, cum - peak)

    fmt = lambda ms: datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")
    print(f"MACD(12,26,9) on BTCUSDT 15m — {n} bars, "
          f"{fmt(bars[0].timestamp)} → {fmt(bars[-1].timestamp)} UTC")
    print(f"  trades:    {report.trades_len}  "
          f"({wins}W / {losses}L, {wins/report.trades_len*100 if report.trades_len else 0:.1f}% win)")
    print(f"  net pnl:   {report.net_profit:+.2f}")
    print(f"  best/worst:{(max(pnls) if pnls else 0):+.2f} / {(min(pnls) if pnls else 0):+.2f}")
    print(f"  max dd:    {max_dd:.2f}")
    print(f"  elapsed:   {elapsed*1000:.1f} ms")

    lib.report_free(ctypes.byref(report))
    lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
