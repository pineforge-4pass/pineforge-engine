#!/usr/bin/env python3
"""Tutorial MTF demo — script_tf switching, input_tf/script_tf pairs,
and request.security_lower_tf sub-bar synthesis.

Three tables, three different switches. Each table prints the exact
``run_backtest_full(...)`` call signature above it so you can copy/paste
the pattern into your own harness.

Build first:
    cmake --build build --target strategy_tutorial_mtf_htf strategy_tutorial_mtf_ltf -j
"""
from __future__ import annotations

import csv
import ctypes
import sys
from pathlib import Path

# Reuse the ctypes mirrors + paths from run.py — same engine, same ABI.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import BarC, ReportC, OHLCV  # noqa: E402

ROOT   = Path(__file__).resolve().parent
SO_HTF = ROOT / "mtf" / "strategy_htf.so"
SO_LTF = ROOT / "mtf" / "strategy_ltf.so"


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


def load_lib(so_path: Path):
    lib = ctypes.CDLL(str(so_path))
    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype  = ctypes.c_void_p
    lib.strategy_set_input.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,           # input_tf, script_tf
        ctypes.c_int, ctypes.c_int, ctypes.c_int,   # magnifier on/samples/dist
        ctypes.POINTER(ReportC)]
    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    return lib


def run_one(lib, bars, n, *, input_tf: bytes, script_tf: bytes,
            inputs: dict[str, str] | None = None) -> dict:
    state = lib.strategy_create(b"{}")
    for k, v in (inputs or {}).items():
        lib.strategy_set_input(state, k.encode(), str(v).encode())

    report = ReportC()
    # The signature this whole tutorial is about:
    #   run_backtest_full(state, bars, n,
    #                     input_tf,  script_tf,         # the two MTF dials
    #                     bar_magnifier, samples, dist, # off here
    #                     out)
    lib.run_backtest_full(state, bars, n,
                          input_tf, script_tf,
                          0, 4, 3,
                          ctypes.byref(report))

    pnls = [report.trades[i].pnl for i in range(report.trades_len)]
    out = {
        "trades":            report.trades_len,
        "net_pnl":           float(report.net_profit),
        "input_tf_seconds":  int(report.input_tf_seconds),
        "script_tf_seconds": int(report.script_tf_seconds),
        "script_tf_ratio":   int(report.script_tf_ratio),
        "needs_aggregation": int(report.needs_aggregation),
        "input_bars":        int(report.input_bars_processed),
        "script_bars":       int(report.script_bars_processed),
        "sec_feeds":         int(report.security_feeds_total),
    }
    lib.report_free(ctypes.byref(report))
    lib.strategy_free(state)
    return out


def fmt_tf(b: bytes) -> str:
    return '""' if b == b"" else f'"{b.decode()}"'


# ─── Table A — script_tf sweep, fixed input ────────────────────────────
def table_a(lib, bars, n):
    print()
    print("=" * 72)
    print("Table A — script_tf sweep, fixed input (HTF .so)")
    print("=" * 72)
    print('  Call: run_backtest_full(.., input_tf=b"",  script_tf=<varying>, ..)')
    print()
    print('  input_tf=b""  → engine auto-detects 15m from bar timestamps.')
    print('  script_tf=b"" → defaults to input_tf (pass-through).')
    print('  script_tf=b"15" → explicit pass-through; ratio == 1.')
    print('  script_tf=b"60" → 4:1 aggregation.')
    print('  script_tf=b"240" → 16:1 aggregation.')
    print()
    print(f"{'script_tf':>10} {'in_s':>5} {'sc_s':>5} {'ratio':>5} {'agg?':>4} "
          f"{'in_bars':>8} {'sc_bars':>8} {'trades':>7} {'net_pnl':>10}")
    print("-" * 72)
    for stf in [b"", b"15", b"60", b"240"]:
        r = run_one(lib, bars, n, input_tf=b"", script_tf=stf)
        print(f"{fmt_tf(stf):>10} "
              f"{r['input_tf_seconds']:>5} {r['script_tf_seconds']:>5} "
              f"{r['script_tf_ratio']:>5} {r['needs_aggregation']:>4} "
              f"{r['input_bars']:>8} {r['script_bars']:>8} "
              f"{r['trades']:>7} {r['net_pnl']:>+10.2f}")


# ─── Table B — input_tf / script_tf pair matrix ────────────────────────
def table_b(lib, bars, n):
    print()
    print("=" * 72)
    print("Table B — input_tf / script_tf pairs (HTF .so)")
    print("=" * 72)
    print("  Call: run_backtest_full(.., input_tf=<a>, script_tf=<b>, ..)")
    print()
    print("  Resolved tf seconds in the report show the auto-detect /")
    print("  defaulting / concatenation chain in action:")
    print('    (b"",   b"")   → both auto-detect, script defaults to input.')
    print('    (b"15", b"60") → explicit input + explicit higher script.')
    print('    (b"15", b"")   → explicit input, script defaults to it.')
    print('    (b"15", b"15") → both pinned to the same TF (pass-through).')
    print()
    print(f"{'input_tf':>10} {'script_tf':>10} {'in_s':>5} {'sc_s':>5} "
          f"{'ratio':>5} {'agg?':>4} {'sc_bars':>8} {'trades':>7} {'net_pnl':>10}")
    print("-" * 80)
    for itf, stf in [(b"", b""), (b"15", b"60"), (b"15", b""), (b"15", b"15")]:
        r = run_one(lib, bars, n, input_tf=itf, script_tf=stf)
        print(f"{fmt_tf(itf):>10} {fmt_tf(stf):>10} "
              f"{r['input_tf_seconds']:>5} {r['script_tf_seconds']:>5} "
              f"{r['script_tf_ratio']:>5} {r['needs_aggregation']:>4} "
              f"{r['script_bars']:>8} {r['trades']:>7} {r['net_pnl']:>+10.2f}")


# ─── Table C — request.security_lower_tf sub-bar synthesis ─────────────
def table_c(bars, n):
    lib = load_lib(SO_LTF)
    print()
    print("=" * 72)
    print("Table C — request.security_lower_tf sub-bar synthesis (LTF .so)")
    print("=" * 72)
    print('  Strategy registers a lower-TF target of "1" (one minute) inside')
    print("  generated_ltf.cpp. The engine synthesizes input_tf / 1m sub-bars")
    print("  per chart bar from each chart bar's OHLC path — there is no")
    print("  separate finer feed (contrast TradingView).")
    print()
    print('  Call: run_backtest_full(.., input_tf=<a>, script_tf=<a>, ..)')
    print("  (lower_tf only makes sense when script_tf == input_tf,")
    print("   since the lower-TF target must be strictly finer than input.)")
    print()
    print(f"{'input_tf':>10} {'in_s':>5} {'in_bars':>8} {'sec_feeds':>10} "
          f"{'feeds/bar':>10} {'trades':>7} {'net_pnl':>10}")
    print("-" * 72)
    # 15m chart → 15 sub-bars per chart bar.
    # 60m chart → 60 sub-bars per chart bar.
    for tf in [b"15", b"60"]:
        r = run_one(lib, bars, n, input_tf=tf, script_tf=tf)
        feeds_per_bar = r["sec_feeds"] / r["input_bars"] if r["input_bars"] else 0
        print(f"{fmt_tf(tf):>10} "
              f"{r['input_tf_seconds']:>5} {r['input_bars']:>8} "
              f"{r['sec_feeds']:>10} {feeds_per_bar:>10.1f} "
              f"{r['trades']:>7} {r['net_pnl']:>+10.2f}")
    print()
    print("  feeds/bar should equal input_tf_seconds / 60 (the 1m divisor).")


def main() -> int:
    if not SO_HTF.exists() or not SO_LTF.exists():
        sys.exit("strategy_*.so missing — run "
                 "`cmake --build build --target strategy_tutorial_mtf_htf "
                 "strategy_tutorial_mtf_ltf -j` first")

    bars, n = load_bars()
    lib_htf = load_lib(SO_HTF)

    table_a(lib_htf, bars, n)
    table_b(lib_htf, bars, n)
    table_c(bars, n)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
