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
                ("max_drawdown",ctypes.c_double),("qty",        ctypes.c_double),
                ("commission",  ctypes.c_double),
                ("entry_bar_index", ctypes.c_int32),
                ("exit_bar_index",  ctypes.c_int32)]

class TradeStatsC(ctypes.Structure):  # pf_trade_stats_t
    _fields_ = [("num_trades", ctypes.c_int32), ("num_wins", ctypes.c_int32),
                ("num_losses", ctypes.c_int32), ("num_even", ctypes.c_int32),
                ("percent_profitable", ctypes.c_double),
                ("net_profit", ctypes.c_double), ("net_profit_pct", ctypes.c_double),
                ("gross_profit", ctypes.c_double), ("gross_profit_pct", ctypes.c_double),
                ("gross_loss", ctypes.c_double), ("gross_loss_pct", ctypes.c_double),
                ("profit_factor", ctypes.c_double),
                ("avg_trade", ctypes.c_double), ("avg_trade_pct", ctypes.c_double),
                ("avg_win", ctypes.c_double), ("avg_win_pct", ctypes.c_double),
                ("avg_loss", ctypes.c_double), ("avg_loss_pct", ctypes.c_double),
                ("ratio_avg_win_avg_loss", ctypes.c_double),
                ("largest_win", ctypes.c_double), ("largest_win_pct", ctypes.c_double),
                ("largest_loss", ctypes.c_double), ("largest_loss_pct", ctypes.c_double),
                ("commission_paid", ctypes.c_double),
                ("expectancy", ctypes.c_double),
                ("max_consecutive_wins", ctypes.c_int32),
                ("max_consecutive_losses", ctypes.c_int32),
                ("avg_bars_in_trade", ctypes.c_double),
                ("avg_bars_in_wins", ctypes.c_double),
                ("avg_bars_in_losses", ctypes.c_double)]

class EquityStatsC(ctypes.Structure):  # pf_equity_stats_t
    _fields_ = [("max_equity_drawdown", ctypes.c_double),
                ("max_equity_drawdown_pct", ctypes.c_double),
                ("max_equity_runup", ctypes.c_double),
                ("max_equity_runup_pct", ctypes.c_double),
                ("buy_hold_return", ctypes.c_double),
                ("buy_hold_return_pct", ctypes.c_double),
                ("sharpe_tv", ctypes.c_double), ("sortino_tv", ctypes.c_double),
                ("sharpe_bar", ctypes.c_double), ("sortino_bar", ctypes.c_double),
                ("cagr", ctypes.c_double), ("calmar", ctypes.c_double),
                ("recovery_factor", ctypes.c_double),
                ("time_in_market_pct", ctypes.c_double),
                ("open_pl", ctypes.c_double)]

class MetricsC(ctypes.Structure):  # pf_metrics_t
    _fields_ = [("all", TradeStatsC), ("longs", TradeStatsC),
                ("shorts", TradeStatsC), ("equity", EquityStatsC)]

class EquityPointC(ctypes.Structure):  # pf_equity_point_t
    _fields_ = [("time_ms", ctypes.c_int64), ("equity", ctypes.c_double),
                ("open_profit", ctypes.c_double)]

class _Diag(ctypes.Structure):
    _fields_ = [("sec_id", ctypes.c_int), ("feed_count", ctypes.c_int64),
                ("eval_complete_count", ctypes.c_int64),
                ("eval_partial_count",  ctypes.c_int64)]

class _Trace(ctypes.Structure):
    _fields_ = [("timestamp", ctypes.c_int64), ("bar_index", ctypes.c_int32),
                ("name_id",   ctypes.c_int32), ("value",     ctypes.c_double)]

class OrderEventC(ctypes.Structure):
    _fields_ = [
        ("transition_sequence", ctypes.c_uint64),
        ("command_revision_id", ctypes.c_uint64),
        ("order_leg_id", ctypes.c_uint64),
        ("priority_sequence", ctypes.c_uint64),
        ("fill_id", ctypes.c_uint64), ("entry_lot_id", ctypes.c_uint64),
        ("position_episode_id", ctypes.c_uint64),
        ("event_timestamp", ctypes.c_int64),
        ("event_sequence", ctypes.c_uint64),
        ("input_bar_index", ctypes.c_int64),
        ("script_bar_index", ctypes.c_int32),
        ("command_kind", ctypes.c_int32), ("leg_kind", ctypes.c_int32),
        ("state_before", ctypes.c_int32), ("state_after", ctypes.c_int32),
        ("transition", ctypes.c_int32), ("reason", ctypes.c_int32),
        ("side", ctypes.c_int32), ("oca_type", ctypes.c_int32),
        ("requested_quantity", ctypes.c_double),
        ("remaining_quantity", ctypes.c_double),
        ("filled_quantity", ctypes.c_double),
        ("observed_price", ctypes.c_double), ("stop_price", ctypes.c_double),
        ("limit_price", ctypes.c_double),
        ("trail_activation_price", ctypes.c_double),
        ("trail_watermark", ctypes.c_double), ("fill_price", ctypes.c_double),
        ("position_size_before", ctypes.c_double),
        ("position_size_after", ctypes.c_double),
        ("equity_before", ctypes.c_double), ("equity_after", ctypes.c_double),
        ("id", ctypes.c_char_p), ("from_entry", ctypes.c_char_p),
        ("oca_name", ctypes.c_char_p),
    ]

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
                ("trace_names_len", ctypes.c_int),
                ("metrics", MetricsC),
                ("equity_curve", ctypes.POINTER(EquityPointC)),
                ("equity_curve_len", ctypes.c_int64),
                ("order_events", ctypes.POINTER(OrderEventC)),
                ("order_events_len", ctypes.c_int64),
                ("order_event_count", ctypes.c_uint64),
                ("order_event_hash", ctypes.c_uint64),
                ("order_event_dropped", ctypes.c_uint64)]


# pf_report_t is caller-allocated, so a stale mirror means the runtime
# writes past our buffer. Assert the .so's ABI version before any run.
EXPECTED_PF_ABI = 3

def check_abi(lib: ctypes.CDLL) -> None:
    try:
        lib.pf_abi_version.restype = ctypes.c_int
        abi = lib.pf_abi_version()
    except AttributeError:
        raise RuntimeError(
            "strategy .so predates pf_abi_version (ABI v1); rebuild it against "
            "the current pineforge runtime (pf_report_t grew).")
    if abi != EXPECTED_PF_ABI:
        raise RuntimeError(
            f"pineforge ABI mismatch: .so reports {abi}, harness expects "
            f"{EXPECTED_PF_ABI}; rebuild.")


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
    check_abi(lib)
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
    lib.run_backtest_full(state, bars, n, b"", b"", 0, 4, 3, ctypes.byref(report))
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
