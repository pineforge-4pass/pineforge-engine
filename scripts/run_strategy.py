#!/usr/bin/env python3
"""Run a compiled PineForge strategy `.so` against an OHLCV bar feed and
emit `engine_trades.csv` in TradingView-compatible format.

This script is the user-facing piece of the reproducibility kit: given
the open-source runtime and the codegen-emitted `generated.cpp`, anyone
can compile a `strategy.so` (see `corpus/CMakeLists.txt`) and drive it
through this harness to regenerate the exact same `engine_trades.csv`
file shipped in the corpus.

It binds *only* the C ABI declared in `<pineforge/pineforge.h>` via
ctypes. There is no transpiler dependency. The struct layouts here are
mirrors of the C declarations and pinned by static_asserts in the
runtime library — if either side drifts, the runtime fails to link
rather than corrupting reads.

Usage examples
--------------

    # Single strategy (auto-finds strategy.so next to generated.cpp)
    python scripts/run_strategy.py corpus/basic/greedy

    # Custom OHLCV input
    python scripts/run_strategy.py corpus/basic/greedy \\
        --ohlcv corpus/data/ohlcv_ETH-USDT-USDT_15m.csv

    # Don't overwrite engine_trades.csv if it already exists
    python scripts/run_strategy.py corpus/basic/greedy --no-overwrite

    # All strategies (matches `bash scripts/run_corpus.sh`)
    for d in corpus/*/*/; do python scripts/run_strategy.py "$d"; done
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

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OHLCV = REPO_ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_15m.csv"


# --- ctypes mirror of <pineforge/pineforge.h> -------------------------
#
# Field order, types, and widths must match the C struct exactly. The
# runtime library has corresponding static_assert(sizeof(...) == ...) and
# offsetof checks in src/c_abi.cpp; if any of these structs drift, the
# library fails to compile.

class BarC(ctypes.Structure):
    _fields_ = [
        ("open", ctypes.c_double),
        ("high", ctypes.c_double),
        ("low", ctypes.c_double),
        ("close", ctypes.c_double),
        ("volume", ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]


class TradeC(ctypes.Structure):
    _fields_ = [
        ("entry_time", ctypes.c_int64),
        ("exit_time", ctypes.c_int64),
        ("entry_price", ctypes.c_double),
        ("exit_price", ctypes.c_double),
        ("pnl", ctypes.c_double),
        ("pnl_pct", ctypes.c_double),
        ("is_long", ctypes.c_int),
        ("max_runup", ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty", ctypes.c_double),
    ]


class SecurityDiagC(ctypes.Structure):
    _fields_ = [
        ("sec_id", ctypes.c_int),
        ("feed_count", ctypes.c_int64),
        ("eval_complete_count", ctypes.c_int64),
        ("eval_partial_count", ctypes.c_int64),
    ]


class TraceEntryC(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id", ctypes.c_int32),
        ("value", ctypes.c_double),
    ]


class ReportC(ctypes.Structure):
    _fields_ = [
        ("total_trades", ctypes.c_int),
        ("trades", ctypes.POINTER(TradeC)),
        ("trades_len", ctypes.c_int),
        ("net_profit", ctypes.c_double),
        ("input_bars_processed", ctypes.c_int64),
        ("script_bars_processed", ctypes.c_int64),
        ("security_feeds_total", ctypes.c_int64),
        ("security_eval_complete_total", ctypes.c_int64),
        ("security_eval_partial_total", ctypes.c_int64),
        ("magnifier_sub_bars_total", ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),
        ("input_tf_seconds", ctypes.c_int),
        ("script_tf_seconds", ctypes.c_int),
        ("script_tf_ratio", ctypes.c_int),
        ("needs_aggregation", ctypes.c_int),
        ("bar_magnifier_enabled", ctypes.c_int),
        ("security_diag", ctypes.POINTER(SecurityDiagC)),
        ("security_diag_len", ctypes.c_int),
        ("trace", ctypes.POINTER(TraceEntryC)),
        ("trace_len", ctypes.c_int),
        ("trace_names", ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len", ctypes.c_int),
    ]


# --- Strategy harness --------------------------------------------------

class Strategy:
    """Thin ctypes wrapper around one strategy.so."""

    def __init__(self, so_path: Path):
        if not so_path.exists():
            raise FileNotFoundError(
                f"strategy library not found: {so_path}\n"
                f"hint: run `cmake --build build --target corpus_strategies` first"
            )
        self.lib = ctypes.CDLL(str(so_path))
        self._setup_signatures()

    def _setup_signatures(self) -> None:
        L = self.lib
        L.strategy_create.argtypes = [ctypes.c_char_p]
        L.strategy_create.restype = ctypes.c_void_p

        L.run_backtest_full.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(BarC), ctypes.c_int,
            ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ReportC),
        ]
        L.run_backtest_full.restype = None

        L.strategy_free.argtypes = [ctypes.c_void_p]
        L.report_free.argtypes = [ctypes.POINTER(ReportC)]

    def run(self, bars_csv: Path, params: dict | None = None) -> dict:
        """Read OHLCV from CSV, drive the engine, return a report dict."""
        bars, n = _load_bars(bars_csv)
        params_json = json.dumps(params or {}).encode()

        state = self.lib.strategy_create(params_json)
        report = ReportC()
        try:
            self.lib.run_backtest_full(
                state, bars, n,
                b"", b"",            # auto-detect input_tf, default script_tf
                0, 4, 3,             # bar_magnifier off, samples=4, dist=ENDPOINTS
                ctypes.byref(report),
            )
            return _report_to_dict(report)
        finally:
            self.lib.report_free(ctypes.byref(report))
            self.lib.strategy_free(state)


def _load_bars(csv_path: Path) -> tuple[ctypes.Array, int]:
    """Read OHLCV CSV (timestamp, open, high, low, close, volume) into BarC[]."""
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
        bars[i].open = o
        bars[i].high = h
        bars[i].low = l
        bars[i].close = c
        bars[i].volume = v
        bars[i].timestamp = ts
    return bars, n


def _report_to_dict(r: ReportC) -> dict:
    trades = []
    for i in range(r.trades_len):
        t = r.trades[i]
        trades.append({
            "entry_time": int(t.entry_time),
            "exit_time": int(t.exit_time),
            "entry_price": float(t.entry_price),
            "exit_price": float(t.exit_price),
            "pnl": float(t.pnl),
            "pnl_pct": float(t.pnl_pct),
            "is_long": bool(t.is_long),
            "max_runup": float(t.max_runup),
            "max_drawdown": float(t.max_drawdown),
            "qty": float(t.qty),
        })
    return {
        "total_trades": int(r.total_trades),
        "net_profit": float(r.net_profit),
        "input_bars_processed": int(r.input_bars_processed),
        "trades": trades,
    }


# --- TradingView-compatible CSV writer --------------------------------

def _fmt_time_utc(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def write_engine_trades_csv(trades: list[dict], path: Path) -> None:
    """Emit one row per trade *side* (exit then entry) in reverse-chronological
    order — byte-for-byte alignable with TradingView's `trades.csv` export."""
    cum_pnls: dict[int, float] = {}
    running = 0.0
    for n, t in enumerate(trades, 1):
        running += t["pnl"]
        cum_pnls[n] = running

    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "Trade #", "Type", "Date and time", "Price", "Qty",
            "Net PnL", "Net PnL %", "MFE", "MAE", "Cumulative PnL",
        ])
        for n, t in reversed(list(enumerate(trades, 1))):
            direction = "long" if t["is_long"] else "short"
            cum = cum_pnls[n]
            for side, time_key, price_key in (
                (f"Exit {direction}", "exit_time", "exit_price"),
                (f"Entry {direction}", "entry_time", "entry_price"),
            ):
                w.writerow([
                    n, side,
                    _fmt_time_utc(t[time_key]),
                    f"{t[price_key]:.6f}",
                    f"{t['qty']:g}",
                    f"{t['pnl']:.6f}",
                    f"{t['pnl_pct']:.4f}",
                    f"{t['max_runup']:.6f}",
                    f"{t['max_drawdown']:.6f}",
                    f"{cum:.6f}",
                ])


# --- CLI ---------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("strategy_dir", type=Path,
                    help="Path to corpus/<cat>/<name>/ — must contain strategy.so")
    ap.add_argument("--ohlcv", type=Path, default=DEFAULT_OHLCV,
                    help=f"OHLCV CSV (default: {DEFAULT_OHLCV.relative_to(REPO_ROOT)})")
    ap.add_argument("--so-name", default="strategy.so",
                    help="Library filename inside strategy_dir (default: strategy.so)")
    ap.add_argument("--no-overwrite", action="store_true",
                    help="Skip if engine_trades.csv already exists.")
    args = ap.parse_args()

    strategy_dir = args.strategy_dir.resolve()
    out_path = strategy_dir / "engine_trades.csv"
    if args.no_overwrite and out_path.exists():
        print(f"SKIP (exists): {out_path}")
        return 0

    so_path = strategy_dir / args.so_name
    if not so_path.exists():
        for alt in ("strategy.dylib", "strategy.so", "strategy.dll"):
            cand = strategy_dir / alt
            if cand.exists():
                so_path = cand
                break

    started = time.time()
    strat = Strategy(so_path)
    report = strat.run(args.ohlcv.resolve())
    write_engine_trades_csv(report["trades"], out_path)
    elapsed = time.time() - started

    try:
        rel = strategy_dir.relative_to(REPO_ROOT)
    except ValueError:
        rel = strategy_dir
    print(
        f"{rel}: "
        f"{report['total_trades']} trades, "
        f"net_profit={report['net_profit']:.2f}, "
        f"bars={report['input_bars_processed']}, "
        f"{elapsed:.2f}s -> {out_path.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
