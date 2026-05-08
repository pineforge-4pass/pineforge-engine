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
          "max_drawdown": float
        },
        ...
      ]
    }
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import json
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
    ]


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


def build_report_dict(report: ReportC, ohlcv_path: Path,
                      n_bars: int, first_ts: int, last_ts: int,
                      elapsed: float,
                      applied_inputs: dict[str, str],
                      applied_overrides: dict[str, str]) -> dict:
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
        })

    n = len(pnls)
    wins   = sum(1 for p in pnls if p > 0)
    losses = sum(1 for p in pnls if p < 0)

    cum, peak, max_dd = 0.0, 0.0, 0.0
    for p in pnls:
        cum += p
        peak = max(peak, cum)
        max_dd = min(max_dd, cum - peak)

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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so",        type=Path, required=True, help="strategy.so path")
    ap.add_argument("--ohlcv",     type=Path, required=True, help="OHLCV CSV path")
    ap.add_argument("--inputs",    default="",
                    help='JSON object overriding input.*() values, e.g. \'{"Fast Length": "8"}\'')
    ap.add_argument("--overrides", default="",
                    help='JSON object overriding strategy() header, e.g. \'{"default_qty_value": "5"}\'')
    args = ap.parse_args()

    inputs    = parse_kv_json(args.inputs,    "--inputs")
    overrides = parse_kv_json(args.overrides, "--overrides")

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
            b"", b"",
            0, 4, 3,            # bar_magnifier off, samples=4, dist=ENDPOINTS
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
        out = build_report_dict(report, args.ohlcv, n, first_ts, last_ts,
                                elapsed, inputs, overrides)
        json.dump(out, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
