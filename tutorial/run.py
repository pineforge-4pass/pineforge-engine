#!/usr/bin/env python3
"""PineForge tutorial runner — load strategy.so, drive it through
tutorial/data/btcusdt_15m_7d.csv, print stats, write trades CSV.

This script is intentionally self-contained: it mirrors the pieces of
scripts/run_strategy.py that a tutorial reader needs (ctypes binding,
CSV loader, report dump) so you can read one file end-to-end. Skip to
``main()`` if you only want the flow.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

TUTORIAL_DIR = Path(__file__).resolve().parent
DEFAULT_SO   = TUTORIAL_DIR / "macd" / "strategy.so"
DEFAULT_CSV  = TUTORIAL_DIR / "data" / "btcusdt_15m_7d.csv"
DEFAULT_OUT  = TUTORIAL_DIR / "macd" / "trades.csv"


# --- ctypes mirror of <pineforge/pineforge.h> -------------------------
# Field order, types, and widths must match the C struct exactly.
# The runtime library has corresponding static_assert checks in
# src/c_abi.cpp; if either side drifts the runtime fails to link.

class BarC(ctypes.Structure):
    _fields_ = [
        ("open",      ctypes.c_double),
        ("high",      ctypes.c_double),
        ("low",       ctypes.c_double),
        ("close",     ctypes.c_double),
        ("volume",    ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]


class TradeC(ctypes.Structure):
    _fields_ = [
        ("entry_time",   ctypes.c_int64),
        ("exit_time",    ctypes.c_int64),
        ("entry_price",  ctypes.c_double),
        ("exit_price",   ctypes.c_double),
        ("pnl",          ctypes.c_double),
        ("pnl_pct",      ctypes.c_double),
        ("is_long",      ctypes.c_int),
        ("max_runup",    ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty",          ctypes.c_double),
    ]


class SecurityDiagC(ctypes.Structure):
    _fields_ = [
        ("sec_id",              ctypes.c_int),
        ("feed_count",          ctypes.c_int64),
        ("eval_complete_count", ctypes.c_int64),
        ("eval_partial_count",  ctypes.c_int64),
    ]


class TraceEntryC(ctypes.Structure):
    _fields_ = [
        ("timestamp",  ctypes.c_int64),
        ("bar_index",  ctypes.c_int32),
        ("name_id",    ctypes.c_int32),
        ("value",      ctypes.c_double),
    ]


class ReportC(ctypes.Structure):
    _fields_ = [
        ("total_trades",                 ctypes.c_int),
        ("trades",                       ctypes.POINTER(TradeC)),
        ("trades_len",                   ctypes.c_int),
        ("net_profit",                   ctypes.c_double),
        ("input_bars_processed",         ctypes.c_int64),
        ("script_bars_processed",        ctypes.c_int64),
        ("security_feeds_total",         ctypes.c_int64),
        ("security_eval_complete_total", ctypes.c_int64),
        ("security_eval_partial_total",  ctypes.c_int64),
        ("magnifier_sub_bars_total",     ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),
        ("input_tf_seconds",             ctypes.c_int),
        ("script_tf_seconds",            ctypes.c_int),
        ("script_tf_ratio",              ctypes.c_int),
        ("needs_aggregation",            ctypes.c_int),
        ("bar_magnifier_enabled",        ctypes.c_int),
        ("security_diag",                ctypes.POINTER(SecurityDiagC)),
        ("security_diag_len",            ctypes.c_int),
        ("trace",                        ctypes.POINTER(TraceEntryC)),
        ("trace_len",                    ctypes.c_int),
        ("trace_names",                  ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len",              ctypes.c_int),
    ]


# --- CSV → BarC[] ------------------------------------------------------

def load_bars(csv_path: Path) -> tuple[ctypes.Array, int]:
    rows: list[tuple[float, float, float, float, float, int]] = []
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append((
                float(row["open"]),
                float(row["high"]),
                float(row["low"]),
                float(row["close"]),
                float(row["volume"]),
                int(row["timestamp"]),
            ))
    n = len(rows)
    bars = (BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open      = o
        bars[i].high      = h
        bars[i].low       = l
        bars[i].close     = c
        bars[i].volume    = v
        bars[i].timestamp = ts
    return bars, n


# --- engine binding ---------------------------------------------------

def load_strategy(so_path: Path) -> ctypes.CDLL:
    if not so_path.exists():
        raise FileNotFoundError(
            f"strategy.so not found: {so_path}\n"
            f"hint: run `tutorial/run.sh` (builds + runs) or "
            f"`cmake -B build && cmake --build build` first"
        )
    lib = ctypes.CDLL(str(so_path))

    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype  = ctypes.c_void_p

    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC),
    ]
    lib.run_backtest_full.restype = None

    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    return lib


def run_backtest(lib: ctypes.CDLL, bars: ctypes.Array, n: int) -> ReportC:
    state = lib.strategy_create(b"{}")
    report = ReportC()
    lib.run_backtest_full(
        state, bars, n,
        b"", b"",
        0, 4, 3,            # bar_magnifier off, samples=4, dist=ENDPOINTS
        ctypes.byref(report),
    )
    return state, report


# --- stats + output ---------------------------------------------------

def compute_stats(report: ReportC) -> dict:
    n = report.trades_len
    pnls = [report.trades[i].pnl for i in range(n)]
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p < 0]

    cum, peak, max_dd = 0.0, 0.0, 0.0
    for p in pnls:
        cum += p
        peak = max(peak, cum)
        max_dd = min(max_dd, cum - peak)

    return {
        "total":     n,
        "wins":      len(wins),
        "losses":    len(losses),
        "win_rate":  (len(wins) / n * 100.0) if n else 0.0,
        "net_pnl":   report.net_profit,
        "avg_trade": (report.net_profit / n) if n else 0.0,
        "best":      max(pnls) if pnls else 0.0,
        "worst":     min(pnls) if pnls else 0.0,
        "max_dd":    max_dd,
        "bars":      int(report.input_bars_processed),
    }


def write_trades_csv(report: ReportC, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fmt_t = lambda ms: datetime.fromtimestamp(
        ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["#", "side", "entry_time", "entry_price",
                    "exit_time", "exit_price", "qty", "pnl", "pnl_pct",
                    "max_runup", "max_drawdown"])
        for i in range(report.trades_len):
            t = report.trades[i]
            w.writerow([
                i + 1,
                "long" if t.is_long else "short",
                fmt_t(int(t.entry_time)),  f"{t.entry_price:.2f}",
                fmt_t(int(t.exit_time)),   f"{t.exit_price:.2f}",
                f"{t.qty:g}",
                f"{t.pnl:.2f}", f"{t.pnl_pct:.4f}",
                f"{t.max_runup:.2f}", f"{t.max_drawdown:.2f}",
            ])


def print_summary(stats: dict, csv_path: Path, n_bars: int,
                  first_ts: int, last_ts: int, elapsed: float,
                  out_path: Path) -> None:
    fmt = lambda ts: datetime.fromtimestamp(
        ts / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")
    print("PineForge tutorial — MACD crossover on BTCUSDT 15m (7d)")
    print("─" * 57)
    print(f"Loaded {n_bars} bars  {fmt(first_ts)} → {fmt(last_ts)} UTC")
    print(f"Strategy: MACD(12, 26, 9), src=close, qty=1 contract, fixed")
    print()
    print(f"Backtest: {elapsed:.2f}s")
    print()
    print("Results")
    print(f"  Total trades:      {stats['total']}")
    print(f"  Wins / Losses:     {stats['wins']} / {stats['losses']}"
          f"   ({stats['win_rate']:.1f}% win rate)")
    print(f"  Net PnL:           {stats['net_pnl']:+.2f} USDT")
    print(f"  Avg trade:         {stats['avg_trade']:+.2f} USDT")
    print(f"  Best / Worst:      {stats['best']:+.2f} / {stats['worst']:+.2f}")
    print(f"  Max drawdown:      {stats['max_dd']:.2f} USDT")
    print(f"  Bars processed:    {stats['bars']}")
    print()
    try:
        rel = out_path.relative_to(Path.cwd())
    except ValueError:
        rel = out_path
    print(f"Trades CSV → {rel}")


# --- CLI --------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so",     type=Path, default=DEFAULT_SO,
                    help=f"strategy.so path (default: {DEFAULT_SO.name})")
    ap.add_argument("--ohlcv",  type=Path, default=DEFAULT_CSV,
                    help=f"OHLCV CSV (default: {DEFAULT_CSV.name})")
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT,
                    help=f"trades CSV out (default: {DEFAULT_OUT.name})")
    args = ap.parse_args()

    bars, n = load_bars(args.ohlcv.resolve())
    first_ts, last_ts = bars[0].timestamp, bars[n - 1].timestamp

    lib = load_strategy(args.so.resolve())

    started = time.time()
    state, report = run_backtest(lib, bars, n)
    elapsed = time.time() - started

    try:
        stats = compute_stats(report)
        write_trades_csv(report, args.output.resolve())
        print_summary(stats, args.ohlcv, n, first_ts, last_ts,
                      elapsed, args.output.resolve())
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
