#!/usr/bin/env python3
"""Advanced tutorial: parameter sweep using the C ABI override hooks.

Demonstrates the two surfaces every PineForge strategy.so exposes for
re-running the same compiled binary with different parameters — no
rebuild needed:

    strategy_set_input(state, key, value)     # input.*() named values
    strategy_set_override(state, key, value)  # strategy(...) header

Sweeps a small (fast, slow) MACD grid crossed with two qty sizes and
prints a comparison table sorted by net PnL.
"""
from __future__ import annotations

import ctypes
import sys
import time
from pathlib import Path

# Reuse the ctypes struct mirrors + paths from run.py — same engine,
# same ABI, no need to retype 60 lines of struct fields.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import BarC, ReportC, SO, OHLCV  # noqa: E402

import csv  # noqa: E402


def load_bars():
    with OHLCV.open(newline="") as f:
        rows = list(csv.DictReader(f))
    n = len(rows)
    bars = (BarC * n)()
    for i, r in enumerate(rows):
        bars[i] = BarC(float(r["open"]), float(r["high"]), float(r["low"]),
                       float(r["close"]), float(r["volume"]),
                       int(r["timestamp"]))
    return bars, n


def load_lib():
    lib = ctypes.CDLL(str(SO))
    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype  = ctypes.c_void_p
    lib.strategy_set_input.argtypes    = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC)]
    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    if hasattr(lib, "strategy_get_last_error"):
        lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
        lib.strategy_get_last_error.restype  = ctypes.c_char_p
    return lib


def run_one(lib, bars, n, *, inputs: dict[str, str],
            overrides: dict[str, str]) -> dict:
    """One backtest with the given input + override map."""
    state = lib.strategy_create(b"{}")
    for k, v in inputs.items():
        lib.strategy_set_input(state, k.encode(), str(v).encode())
    for k, v in overrides.items():
        lib.strategy_set_override(state, k.encode(), str(v).encode())

    report = ReportC()
    t0 = time.time()
    lib.run_backtest_full(state, bars, n, b"", b"", 0, 4, 3,
                          ctypes.byref(report))
    elapsed = time.time() - t0
    if hasattr(lib, "strategy_get_last_error"):
        err_ptr = lib.strategy_get_last_error(state)
        if err_ptr:
            err_msg = err_ptr.decode("utf-8", "replace")
            if err_msg:
                lib.report_free(ctypes.byref(report))
                lib.strategy_free(state)
                raise RuntimeError("pineforge engine rejected run: " + err_msg)

    pnls = [report.trades[i].pnl for i in range(report.trades_len)]
    wins = sum(p > 0 for p in pnls)
    cum = peak = max_dd = 0.0
    for p in pnls:
        cum += p; peak = max(peak, cum); max_dd = min(max_dd, cum - peak)

    out = {
        "trades":   report.trades_len,
        "wins":     wins,
        "win_rate": (wins / report.trades_len * 100.0) if report.trades_len else 0.0,
        "net_pnl":  float(report.net_profit),
        "max_dd":   max_dd,
        "elapsed":  elapsed,
    }
    lib.report_free(ctypes.byref(report))
    lib.strategy_free(state)
    return out


def main() -> int:
    if not SO.exists():
        sys.exit("strategy.so missing — run `bash tutorial/run.sh` first")

    bars, n = load_bars()
    lib = load_lib()

    # Sweep grid: (fast, slow) MACD lengths × default_qty_value.
    # Signal length stays at the Pine default (9). Add anything you want.
    fast_slow = [(8, 21), (12, 26), (19, 39), (26, 52)]
    qty_sizes = [1, 5]

    rows = []
    for fast, slow in fast_slow:
        for qty in qty_sizes:
            r = run_one(
                lib, bars, n,
                # input.*() named values from strategy.pine. Keys must
                # match the input.int(..., "name") second arg.
                inputs={
                    "Fast Length": fast,
                    "Slow Length": slow,
                },
                # strategy(...) header values. Keys are the lower-case
                # Pine attribute names (initial_capital, commission_value,
                # default_qty_value, pyramiding, slippage, ...).
                overrides={
                    "default_qty_value": qty,
                    "commission_value":  0.04,   # 0.04% per side ≈ Binance taker
                },
            )
            r["fast"], r["slow"], r["qty"] = fast, slow, qty
            rows.append(r)

    rows.sort(key=lambda x: -x["net_pnl"])

    print(f"MACD sweep on BTCUSDT 15m — {n} bars, "
          f"{len(rows)} configs (commission 0.04% each side)")
    print()
    print(f"{'fast':>4} {'slow':>4} {'qty':>3}  {'trades':>6} "
          f"{'win%':>5}  {'net_pnl':>10}  {'max_dd':>10}  {'ms':>5}")
    print("-" * 64)
    for r in rows:
        print(f"{r['fast']:>4} {r['slow']:>4} {r['qty']:>3}  "
              f"{r['trades']:>6} {r['win_rate']:>4.1f}%  "
              f"{r['net_pnl']:>+10.2f}  {r['max_dd']:>10.2f}  "
              f"{r['elapsed']*1000:>5.1f}")
    print()
    best = rows[0]
    print(f"best: fast={best['fast']} slow={best['slow']} qty={best['qty']}"
          f" → net {best['net_pnl']:+.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
