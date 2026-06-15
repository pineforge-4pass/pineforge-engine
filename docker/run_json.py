#!/usr/bin/env python3
"""PineForge container harness — load strategy.so, run against an
OHLCV CSV, emit a JSON report on stdout.

Schema:
    {
      "engine": "pineforge",
      "input": {
        "ohlcv":      "<path>",
        "bars":       int,
        "first_ts":   int,           # unix ms
        "last_ts":    int,           # unix ms
        "first_time": "YYYY-MM-DD HH:MM UTC",
        "last_time":  "YYYY-MM-DD HH:MM UTC"
      },
      "elapsed_seconds": float,
      "summary": {
        "total_trades": int,
        "wins":         int,
        "losses":       int,
        "win_rate_pct": float,
        "net_pnl":      float,
        "avg_trade":    float,
        "best_trade":   float,
        "worst_trade":  float,
        "max_drawdown": float,
        "bars_processed": int
      },
      "trades": [
        {
          "n":            int,
          "side":         "long" | "short",
          "entry_time":   int,       # unix ms
          "exit_time":    int,       # unix ms
          "entry_price":  float,
          "exit_price":   float,
          "qty":          float,
          "pnl":          float,
          "pnl_pct":      float,
          "max_runup":    float,
          "max_drawdown": float,
          "commission":      float,   # ABI v2
          "entry_bar_index": int,     # ABI v2: script-bar index of entry fill
          "exit_bar_index":  int      # ABI v2: script-bar index of exit fill
        },
        ...
      ],
      "metrics": {                     # ABI v2 computed trading metrics
        "all":    { ...pf_trade_stats_t... },   # all closed trades
        "longs":  { ...pf_trade_stats_t... },   # long trades only
        "shorts": { ...pf_trade_stats_t... },   # short trades only
        "equity": { ...pf_equity_stats_t... }   # sharpe/sortino/cagr/calmar/...
      },                               # any NaN statistic -> null (see _num)
      "equity_curve": [                # ABI v2: one point per script bar
        { "time_ms": int, "equity": float, "open_profit": float },
        ...
      ]
    }

NaN convention: any metric with an empty/zero denominator is null (JSON has no
NaN); a real computed 0 stays 0. See the report-schema + metrics reference docs
for the per-field meaning of every metrics.* key.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import json
import math
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


# --- ctypes mirror of <pineforge/pineforge.h> -------------------------

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
        ("commission",      ctypes.c_double),
        ("entry_bar_index", ctypes.c_int32),
        ("exit_bar_index",  ctypes.c_int32),
    ]


class TradeStatsC(ctypes.Structure):
    """Mirror of pf_trade_stats_t (ABI v2)."""
    _fields_ = [
        ("num_trades", ctypes.c_int32), ("num_wins", ctypes.c_int32),
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
        ("max_consecutive_wins", ctypes.c_int32), ("max_consecutive_losses", ctypes.c_int32),
        ("avg_bars_in_trade", ctypes.c_double), ("avg_bars_in_wins", ctypes.c_double),
        ("avg_bars_in_losses", ctypes.c_double),
    ]


class EquityStatsC(ctypes.Structure):
    """Mirror of pf_equity_stats_t (ABI v2)."""
    _fields_ = [
        ("max_equity_drawdown", ctypes.c_double), ("max_equity_drawdown_pct", ctypes.c_double),
        ("max_equity_runup", ctypes.c_double), ("max_equity_runup_pct", ctypes.c_double),
        ("buy_hold_return", ctypes.c_double), ("buy_hold_return_pct", ctypes.c_double),
        ("sharpe_tv", ctypes.c_double), ("sortino_tv", ctypes.c_double),
        ("sharpe_bar", ctypes.c_double), ("sortino_bar", ctypes.c_double),
        ("cagr", ctypes.c_double), ("calmar", ctypes.c_double),
        ("recovery_factor", ctypes.c_double), ("time_in_market_pct", ctypes.c_double),
        ("open_pl", ctypes.c_double),
    ]


class MetricsC(ctypes.Structure):
    """Mirror of pf_metrics_t (ABI v2)."""
    _fields_ = [("all", TradeStatsC), ("longs", TradeStatsC),
                ("shorts", TradeStatsC), ("equity", EquityStatsC)]


class EquityPointC(ctypes.Structure):
    """Mirror of pf_equity_point_t (ABI v2)."""
    _fields_ = [("time_ms", ctypes.c_int64), ("equity", ctypes.c_double),
                ("open_profit", ctypes.c_double)]


class SecurityDiagC(ctypes.Structure):
    _fields_ = [
        ("sec_id",              ctypes.c_int),
        ("feed_count",          ctypes.c_int64),
        ("eval_complete_count", ctypes.c_int64),
        ("eval_partial_count",  ctypes.c_int64),
    ]


class TraceEntryC(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id",   ctypes.c_int32),
        ("value",     ctypes.c_double),
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
        ("metrics",                      MetricsC),
        ("equity_curve",                 ctypes.POINTER(EquityPointC)),
        ("equity_curve_len",             ctypes.c_int64),  # int64, NOT c_int
    ]


# pf_report_t is CALLER-allocated: a .so built against a different ABI
# writes past (or short of) our ReportC buffer. Assert version up front.
EXPECTED_PF_ABI = 2


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


# --- helpers ----------------------------------------------------------

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


def load_strategy(so_path: Path) -> ctypes.CDLL:
    lib = ctypes.CDLL(str(so_path))
    check_abi(lib)

    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype  = ctypes.c_void_p

    lib.strategy_set_input.argtypes    = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]

    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC),
    ]
    lib.run_backtest_full.restype = None

    if hasattr(lib, "strategy_get_last_error"):
        lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
        lib.strategy_get_last_error.restype  = ctypes.c_char_p

    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    return lib


def fmt_utc(ms: int) -> str:
    return datetime.fromtimestamp(
        ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def _num(x):
    """JSON-safe float. The engine's metric NaN convention (empty / zero
    denominator -> NaN, never 0) cannot survive JSON: json.dump emits a bare
    `NaN` token that a strict downstream JSON.parse (the MCP layer) rejects.
    Collapse every non-finite double to null so the report stays valid JSON."""
    f = float(x)
    return f if math.isfinite(f) else None


def _stats_dict(s) -> dict:
    """Serialize a pf_trade_stats_t / pf_equity_stats_t ctypes struct to a dict,
    keying off each field's ctype: integer counters stay ints, every double is
    sanitized through _num. Driven by _fields_ so it tracks the struct verbatim."""
    out = {}
    for name, ctype in s._fields_:
        v = getattr(s, name)
        out[name] = _num(v) if ctype is ctypes.c_double else int(v)
    return out


def build_report_dict(report: ReportC, ohlcv_path: Path,
                      n_bars: int, first_ts: int, last_ts: int,
                      elapsed: float,
                      applied_inputs: dict[str, str],
                      applied_overrides: dict[str, str],
                      applied_runtime: dict[str, object] | None = None) -> dict:
    trades = []
    pnls: list[float] = []
    for i in range(report.trades_len):
        t = report.trades[i]
        pnls.append(float(t.pnl))
        trades.append({
            "n":            i + 1,
            "side":         "long" if t.is_long else "short",
            "entry_time":   int(t.entry_time),
            "exit_time":    int(t.exit_time),
            "entry_price":  float(t.entry_price),
            "exit_price":   float(t.exit_price),
            "qty":          float(t.qty),
            "pnl":          float(t.pnl),
            "pnl_pct":      float(t.pnl_pct),
            "max_runup":    float(t.max_runup),
            "max_drawdown": float(t.max_drawdown),
            "commission":      float(t.commission),
            "entry_bar_index": int(t.entry_bar_index),
            "exit_bar_index":  int(t.exit_bar_index),
        })

    n = len(pnls)
    wins   = sum(1 for p in pnls if p > 0)
    losses = sum(1 for p in pnls if p < 0)

    cum, peak, max_dd = 0.0, 0.0, 0.0
    for p in pnls:
        cum += p
        peak = max(peak, cum)
        max_dd = min(max_dd, cum - peak)

    # Computed trading metrics (ABI v2): all/longs/shorts trade stats + the
    # equity-curve-derived block (sharpe/sortino/cagr/calmar/...). See the
    # report-schema + metrics reference pages for per-field definitions.
    m = report.metrics
    metrics = {
        "all":    _stats_dict(m.all),
        "longs":  _stats_dict(m.longs),
        "shorts": _stats_dict(m.shorts),
        "equity": _stats_dict(m.equity),
    }

    # Per-script-bar equity curve (ABI v2). equity_curve may be NULL if a
    # mid-run exception truncated it (len then 0); guard the pointer deref.
    equity_curve = []
    if report.equity_curve:
        for i in range(int(report.equity_curve_len)):
            p = report.equity_curve[i]
            equity_curve.append({
                "time_ms":     int(p.time_ms),
                "equity":      _num(p.equity),
                "open_profit": _num(p.open_profit),
            })

    return {
        "engine": "pineforge",
        "input": {
            "ohlcv":      str(ohlcv_path),
            "bars":       n_bars,
            "first_ts":   int(first_ts),
            "last_ts":    int(last_ts),
            "first_time": fmt_utc(first_ts),
            "last_time":  fmt_utc(last_ts),
        },
        "applied_inputs":    applied_inputs,
        "applied_overrides": applied_overrides,
        "applied_runtime":   applied_runtime or {},
        "elapsed_seconds":   round(elapsed, 4),
        "summary": {
            "total_trades":   n,
            "wins":           wins,
            "losses":         losses,
            "win_rate_pct":   round((wins / n * 100.0) if n else 0.0, 4),
            "net_pnl":        float(report.net_profit),
            "avg_trade":      (float(report.net_profit) / n) if n else 0.0,
            "best_trade":     max(pnls) if pnls else 0.0,
            "worst_trade":    min(pnls) if pnls else 0.0,
            "max_drawdown":   max_dd,
            "bars_processed": int(report.input_bars_processed),
        },
        "trades": trades,
        "metrics": metrics,
        "equity_curve": equity_curve,
    }


def parse_kv_json(s: str | None, label: str) -> dict[str, str]:
    """Parse a JSON object of {key: value} into a {str: str} map.
    Empty / None / "{}" → {}. Non-object payloads abort with a clear
    error so junk env vars don't silently noop."""
    if not s or s.strip() in ("", "{}"):
        return {}
    try:
        obj = json.loads(s)
    except json.JSONDecodeError as e:
        sys.exit(f"error: {label} is not valid JSON: {e}")
    if not isinstance(obj, dict):
        sys.exit(f"error: {label} must be a JSON object, got {type(obj).__name__}")
    return {str(k): str(v) for k, v in obj.items()}


# MagnifierDistribution enum values mirror include/pineforge/magnifier.hpp.
MAGNIFIER_DISTS = {
    "uniform":      0,
    "cosine":       1,
    "triangle":     2,
    "endpoints":    3,
    "front_loaded": 4,
    "back_loaded":  5,
}


def parse_magnifier_dist(s: str) -> int:
    if not s:
        return 3
    key = s.strip().lower()
    if key in MAGNIFIER_DISTS:
        return MAGNIFIER_DISTS[key]
    if key.isdigit() and 0 <= int(key) <= 5:
        return int(key)
    sys.exit(
        f"error: --magnifier-dist must be one of "
        f"{sorted(MAGNIFIER_DISTS)} or 0-5, got {s!r}"
    )


def parse_bool(s: str) -> bool:
    return s.strip().lower() in ("1", "true", "yes", "on")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so",        type=Path, required=True, help="strategy.so path")
    ap.add_argument("--ohlcv",     type=Path, required=True, help="OHLCV CSV path")
    ap.add_argument("--inputs",    default="",
                    help='JSON object overriding input.*() values, e.g. \'{"Fast Length": "8"}\'')
    ap.add_argument("--overrides", default="",
                    help='JSON object overriding strategy() header, e.g. \'{"default_qty_value": "5"}\'')
    ap.add_argument("--input-tf", default="",
                    help="Chart bar timeframe (e.g. '1', '5', '15', '60', 'D'). "
                         "Empty = auto-detect from bar timestamps.")
    ap.add_argument("--script-tf", default="",
                    help="Strategy timeframe. Empty = same as input_tf. "
                         "Must be coarser than or equal to input_tf; the engine "
                         "rejects finer values via strategy_get_last_error.")
    ap.add_argument("--bar-magnifier", default="",
                    help="Enable intra-bar price-path sampling for stop/limit fills "
                         "(true/false, default false).")
    ap.add_argument("--magnifier-samples", type=int, default=4,
                    help="Sub-bar sample count when --bar-magnifier=true (default 4).")
    ap.add_argument("--magnifier-dist", default="endpoints",
                    help="Sample distribution: uniform, cosine, triangle, "
                         "endpoints (default), front_loaded, back_loaded.")
    args = ap.parse_args()

    inputs    = parse_kv_json(args.inputs,    "--inputs")
    overrides = parse_kv_json(args.overrides, "--overrides")
    input_tf  = args.input_tf.strip().encode()
    script_tf = args.script_tf.strip().encode()
    bar_magnifier = 1 if parse_bool(args.bar_magnifier) else 0
    magnifier_samples = max(2, int(args.magnifier_samples))
    magnifier_dist = parse_magnifier_dist(args.magnifier_dist)

    bars, n = load_bars(args.ohlcv)
    first_ts, last_ts = bars[0].timestamp, bars[n - 1].timestamp

    lib = load_strategy(args.so)

    state = lib.strategy_create(b"{}")
    for k, v in inputs.items():
        lib.strategy_set_input(state, k.encode(), v.encode())
    for k, v in overrides.items():
        lib.strategy_set_override(state, k.encode(), v.encode())

    report = ReportC()
    started = time.time()
    try:
        lib.run_backtest_full(
            state, bars, n,
            input_tf, script_tf,
            bar_magnifier, magnifier_samples, magnifier_dist,
            ctypes.byref(report),
        )
        elapsed = time.time() - started
        err_msg = ""
        if hasattr(lib, "strategy_get_last_error"):
            err_ptr = lib.strategy_get_last_error(state)
            err_msg = err_ptr.decode("utf-8", "replace") if err_ptr else ""
        if err_msg:
            json.dump({"engine": "pineforge", "error": err_msg},
                      sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
            return 1
        applied_runtime = {
            "input_tf":           input_tf.decode() if input_tf else "",
            "script_tf":          script_tf.decode() if script_tf else "",
            "input_tf_seconds":   int(report.input_tf_seconds),
            "script_tf_seconds":  int(report.script_tf_seconds),
            "script_tf_ratio":    int(report.script_tf_ratio),
            "needs_aggregation":  bool(report.needs_aggregation),
            "bar_magnifier":      bool(bar_magnifier),
            "magnifier_samples":  magnifier_samples,
            "magnifier_dist":     args.magnifier_dist.strip().lower() or "endpoints",
        }
        out = build_report_dict(report, args.ohlcv, n, first_ts, last_ts,
                                elapsed, inputs, overrides, applied_runtime)
        json.dump(out, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
