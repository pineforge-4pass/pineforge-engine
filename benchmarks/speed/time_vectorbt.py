#!/usr/bin/env python3
"""Time 5 core strategies in vectorbt vs PineForge.

Output: comparative timing table of PineForge vs vectorbt.
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path
import numpy as np
import pandas as pd
import vectorbt as vbt
from numba import njit

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
DATA_CSV = BENCH / "_workdir" / "data" / "ETHUSDT_15.csv"
PF_JSON = BENCH / "_workdir" / "pf_speed.json"

DEFAULT_N = 20

# ---------------------------------------------------------------------------
# Indicator calculation functions
# ---------------------------------------------------------------------------

def wma(s: pd.Series, period: int) -> pd.Series:
    weights = np.arange(1, period + 1)
    return s.rolling(period).apply(lambda x: np.dot(x, weights) / weights.sum(), raw=True)


def hma(s: pd.Series, period: int) -> pd.Series:
    half_len = period // 2
    sqrt_len = int(np.sqrt(period))
    wma_half = wma(s, half_len)
    wma_full = wma(s, period)
    diff = 2 * wma_half - wma_full
    return wma(diff, sqrt_len)


def atr(high: pd.Series, low: pd.Series, close: pd.Series, period: int) -> pd.Series:
    prev_close = close.shift(1)
    tr1 = high - low
    tr2 = (high - prev_close).abs()
    tr3 = (low - prev_close).abs()
    tr = np.maximum(tr1, np.maximum(tr2, tr3))
    return tr.ewm(alpha=1/period, adjust=False).mean()


@njit
def supertrend_loop(h: np.ndarray, l: np.ndarray, c: np.ndarray, atr_val: np.ndarray, multiplier: float):
    n = len(c)
    upperband = np.zeros(n)
    lowerband = np.zeros(n)
    in_uptrend = np.ones(n, dtype=np.bool_)
    supertrend = np.zeros(n)

    for i in range(n):
        hl2 = (h[i] + l[i]) / 2
        upperband[i] = hl2 + multiplier * atr_val[i]
        lowerband[i] = hl2 - multiplier * atr_val[i]

    for i in range(1, n):
        if upperband[i] > upperband[i-1] and c[i-1] < upperband[i-1]:
            upperband[i] = upperband[i-1]
        if lowerband[i] < lowerband[i-1] and c[i-1] > lowerband[i-1]:
            lowerband[i] = lowerband[i-1]

        if c[i] > upperband[i-1]:
            in_uptrend[i] = True
        elif c[i] < lowerband[i-1]:
            in_uptrend[i] = False
        else:
            in_uptrend[i] = in_uptrend[i-1]

        if in_uptrend[i]:
            supertrend[i] = lowerband[i]
        else:
            supertrend[i] = upperband[i]

    return supertrend, in_uptrend


def supertrend(high: pd.Series, low: pd.Series, close: pd.Series, period=10, multiplier=3.0) -> tuple[pd.Series, pd.Series]:
    atr_val = atr(high, low, close, period).fillna(0).values
    supertrend_band, in_uptrend = supertrend_loop(high.values, low.values, close.values, atr_val, multiplier)
    return pd.Series(supertrend_band, index=close.index), pd.Series(in_uptrend, index=close.index)


# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

def run_vbt_sma_cross(df: pd.DataFrame) -> vbt.Portfolio:
    close = df['close']
    fast = close.rolling(9).mean()
    slow = close.rolling(21).mean()

    entries = (fast > slow) & (fast.shift(1) <= slow.shift(1))
    short_entries = (fast < slow) & (fast.shift(1) >= slow.shift(1))

    return vbt.Portfolio.from_signals(close, entries=entries, short_entries=short_entries, init_cash=1000000)


def run_vbt_supertrend(df: pd.DataFrame) -> vbt.Portfolio:
    close = df['close']
    _, in_uptrend = supertrend(df['high'], df['low'], close)

    entries = in_uptrend & (~in_uptrend.shift(1).fillna(False))
    short_entries = (~in_uptrend) & in_uptrend.shift(1).fillna(True)

    return vbt.Portfolio.from_signals(close, entries=entries, short_entries=short_entries, init_cash=1000000)


def run_vbt_keltner(df: pd.DataFrame) -> vbt.Portfolio:
    close = df['close']
    middle = close.ewm(span=20, adjust=False).mean()
    atr_val = atr(df['high'], df['low'], close, 10)
    upper = middle + 2.0 * atr_val
    lower = middle - 2.0 * atr_val

    entries = (close > upper) & (close.shift(1) <= upper.shift(1))
    short_entries = (close < lower) & (close.shift(1) >= lower.shift(1))

    return vbt.Portfolio.from_signals(close, entries=entries, short_entries=short_entries, init_cash=1000000)


def run_vbt_hma_cross(df: pd.DataFrame) -> vbt.Portfolio:
    close = df['close']
    hma_val = hma(close, 9)

    entries = (close > hma_val) & (close.shift(1) <= hma_val.shift(1))
    short_entries = (close < hma_val) & (close.shift(1) >= hma_val.shift(1))

    return vbt.Portfolio.from_signals(close, entries=entries, short_entries=short_entries, init_cash=1000000)


def run_vbt_momentum_roc(df: pd.DataFrame) -> vbt.Portfolio:
    close = df['close']
    roc = ((close - close.shift(9)) / close.shift(9)) * 100.0

    entries = (roc > 0) & (roc.shift(1) <= 0)
    short_entries = (roc < 0) & (roc.shift(1) >= 0)

    return vbt.Portfolio.from_signals(close, entries=entries, short_entries=short_entries, init_cash=1000000)


# ---------------------------------------------------------------------------
# Main Runner
# ---------------------------------------------------------------------------

def main() -> None:
    if not DATA_CSV.exists():
        print(f"Error: Data file not found at {DATA_CSV}", file=sys.stderr)
        sys.exit(1)

    df = pd.read_csv(DATA_CSV)

    # Warmup Numba compiler
    _ = run_vbt_supertrend(df.head(100))
    _ = run_vbt_hma_cross(df.head(100))

    strategies = {
        "01-sma-cross": run_vbt_sma_cross,
        "03-supertrend": run_vbt_supertrend,
        "12-keltner": run_vbt_keltner,
        "22-hma-cross": run_vbt_hma_cross,
        "32-momentum-roc": run_vbt_momentum_roc
    }

    print("Running vectorbt benchmarks...", file=sys.stderr)
    vbt_results: dict[str, dict] = {}

    for name, run_fn in strategies.items():
        samples_ms = []
        for _ in range(DEFAULT_N):
            t0 = time.perf_counter()
            portfolio = run_fn(df)
            # Access any metric to ensure lazy calculations inside vbt portfolio are forced
            _ = portfolio.total_return()
            elapsed = (time.perf_counter() - t0) * 1000.0
            samples_ms.append(elapsed)

        arr = np.array(samples_ms)
        vbt_results[name] = {
            "median_ms": float(np.median(arr)),
            "p95_ms": float(np.percentile(arr, 95)),
            "n": DEFAULT_N
        }
        print(f"  {name}: vbt_median={vbt_results[name]['median_ms']:.1f}ms", file=sys.stderr)

    # Load PineForge results
    if not PF_JSON.exists():
        print(f"Warning: {PF_JSON} not found. Ensure PineForge speed sweep was run.", file=sys.stderr)
        pf_results = {}
    else:
        try:
            with open(PF_JSON, "r") as f:
                raw_pf = json.load(f)
            pf_results = {}
            for b in raw_pf.get("benchmarks", []):
                if b.get("run_type") == "aggregate":
                    continue
                b_name = b["name"]
                slug = b_name.split("/")[0]
                time_unit = b.get("time_unit", "us")
                real_time = b["real_time"]
                if time_unit == "us":
                    median_ms = real_time / 1000.0
                elif time_unit == "ns":
                    median_ms = real_time / 1_000_000.0
                elif time_unit == "ms":
                    median_ms = real_time
                else:
                    median_ms = real_time / 1000.0
                pf_results[slug] = {"median_ms": median_ms}
        except Exception as e:
            print(f"Warning: Failed to parse {PF_JSON}: {e}", file=sys.stderr)
            pf_results = {}

    # Output comparative report
    print("\n## PineForge vs. vectorbt Speed Comparison\n")
    print("| Strategy | PF median (ms) | vbt median (ms) | Speedup PF vs. vbt |")
    print("|---|---:|---:|---:|")

    for name in sorted(strategies.keys()):
        pf_med = pf_results.get(name, {}).get("median_ms", 0.0)
        vbt_med = vbt_results.get(name, {}).get("median_ms", 0.0)

        pf_med_str = f"{pf_med:.2f}" if pf_med > 0 else "—"
        vbt_med_str = f"{vbt_med:.2f}" if vbt_med > 0 else "—"

        if pf_med > 0 and vbt_med > 0:
            ratio = vbt_med / pf_med
            if ratio < 1.0:
                speedup = f"{ratio:.2f}×"
            elif ratio < 10.0:
                speedup = f"{ratio:.1f}×"
            else:
                speedup = f"{ratio:.0f}×"
        else:
            speedup = "—"

        print(f"| {name} | {pf_med_str} | {vbt_med_str} | {speedup} |")

    print("\n*Throughput benchmark over 41,307-bar ETHUSDT_15 OHLCV series (N=20 iterations).*")


if __name__ == "__main__":
    main()
