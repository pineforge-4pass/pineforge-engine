# Example — Parameter sweep in Python {#examples_python_sweep}

@tableofcontents

Run a single compiled `strategy.so` over a 2-D grid of MACD periods.
**No recompile** — every iteration creates a fresh handle, sets new
inputs via the C ABI, and runs a clean backtest.

## What you'll build

```
$ python3 sweep.py
sweeping 12 combos
  fast=08 slow=21 sig=09  trades= 65  pnl= -3318.18  win=27.7%
  fast=08 slow=26 sig=09  trades= 63  pnl= -4723.92  win=25.4%
  fast=08 slow=32 sig=09  trades= 57  pnl= -4356.87  win=26.3%
  fast=12 slow=26 sig=09  trades= 49  pnl= -3270.77  win=28.6%   <- baseline params
  fast=12 slow=32 sig=09  trades= 47  pnl= -2884.76  win=29.8%
  fast=16 slow=21 sig=09  trades= 47  pnl= -3005.30  win=29.8%
  ...
12 runs in 6.0 ms (0.5 ms/run)

top 3 by net pnl:
  (12,32, 9)  pnl=-2884.76
  (16,21, 9)  pnl=-3005.30
  (12,26, 9)  pnl=-3270.77
```

Numbers depend on the OHLCV snapshot — refresh with
`python3 tutorial/data/fetch_btcusdt.py` for current Binance bars.

## The fresh-handle rule

A `pf_strategy_t` carries trade history, equity curve, and position
state. Calling #run_backtest twice on the same handle accumulates
trades from both runs into the second report — the engine never
rewinds.

**For sweeps and walk-forward windows, always create a fresh handle
per run.** Configuration overrides set on a handle apply to all runs
on that handle, but trade state does not reset.

@warning Reusing a handle across runs in a sweep is the most common
PineForge integration bug. The runtime will not warn you — the second
report's `trades_len` simply grows.

## Source: sweep.py

```python
#!/usr/bin/env python3
"""MACD parameter sweep — one fresh handle per backtest."""
from __future__ import annotations
import csv, ctypes, sys, time
from itertools import product
from pathlib import Path

# Reuse the ctypes mirror from tutorial/run.py.
sys.path.insert(0, str(Path(__file__).parent / "../../tutorial"))
from run import BarC, ReportC          # noqa: E402

ROOT  = Path(__file__).resolve().parent.parent.parent
SO    = ROOT / "tutorial" / "macd"  / "strategy.so"
OHLCV = ROOT / "tutorial" / "data"  / "btcusdt_15m_7d.csv"

def load_bars():
    with OHLCV.open(newline="") as f:
        rows = list(csv.DictReader(f))
    n = len(rows)
    bars = (BarC * n)()
    for i, r in enumerate(rows):
        bars[i] = BarC(float(r["open"]), float(r["high"]), float(r["low"]),
                       float(r["close"]), float(r["volume"]), int(r["timestamp"]))
    return bars, n

def make_lib():
    lib = ctypes.CDLL(str(SO))
    lib.strategy_create.argtypes      = [ctypes.c_char_p]
    lib.strategy_create.restype       = ctypes.c_void_p
    lib.strategy_set_input.argtypes   = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.run_backtest_full.argtypes    = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC)]
    lib.report_free.argtypes  = [ctypes.POINTER(ReportC)]
    lib.strategy_free.argtypes = [ctypes.c_void_p]
    return lib

def run_one(lib, bars, n, inputs: dict, overrides: dict) -> ReportC:
    """One backtest with a freshly-allocated strategy handle."""
    state = lib.strategy_create(b"{}")
    for k, v in overrides.items():
        lib.strategy_set_override(state, k.encode(), str(v).encode())
    for k, v in inputs.items():
        lib.strategy_set_input(state, k.encode(), str(v).encode())

    report = ReportC()
    lib.run_backtest_full(state, bars, n, b"", b"", 0, 4, 3,
                          ctypes.byref(report))
    # Caller frees the report; handle can go away now (report has its own arrays).
    lib.strategy_free(state)
    return report

def main() -> int:
    if not SO.exists():
        sys.exit("strategy.so missing — run `bash tutorial/run.sh` first")

    lib = make_lib()
    bars, n = load_bars()

    overrides = {"initial_capital": 10000, "commission_value": 0.04}
    fasts, slows, signals = [8, 12, 16], [21, 26, 32, 40], [9]
    combos = [(f, sl, sg) for f, sl, sg in product(fasts, slows, signals) if f < sl]

    print(f"sweeping {len(combos)} combos")
    results, t0 = [], time.time()
    for fast, slow, sig in combos:
        inputs = {"Fast Length": fast, "Slow Length": slow, "Signal Length": sig}
        report = run_one(lib, bars, n, inputs, overrides)

        wins = sum(1 for i in range(report.trades_len) if report.trades[i].pnl > 0)
        win_pct = (wins / report.trades_len * 100) if report.trades_len else 0.0
        results.append((fast, slow, sig, report.trades_len, report.net_profit, win_pct))
        print(f"  fast={fast:02d} slow={slow:02d} sig={sig:02d}  "
              f"trades={report.trades_len:3d}  pnl={report.net_profit:+9.2f}  "
              f"win={win_pct:.1f}%")
        lib.report_free(ctypes.byref(report))

    elapsed = time.time() - t0
    print(f"\n{len(combos)} runs in {elapsed*1000:.1f} ms "
          f"({elapsed*1000/len(combos):.1f} ms/run)")

    print("\ntop 3 by net pnl:")
    for f, sl, sg, _, pnl, _ in sorted(results, key=lambda r: -r[4])[:3]:
        print(f"  ({f:2d},{sl:2d},{sg:2d})  pnl={pnl:+.2f}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

## Walk-forward variant

Same fresh-handle pattern, sliced bar windows:

```python
window = 30 * 24 * 4   # 30 days at 15m
step   = 7  * 24 * 4   #  7 days

for start in range(0, n - window, step):
    sub = (BarC * window)()
    ctypes.memmove(sub, ctypes.byref(bars, ctypes.sizeof(BarC) * start),
                   ctypes.sizeof(BarC) * window)

    report = run_one(lib, sub, window,
                     inputs={"Fast Length": 12, "Slow Length": 26, "Signal Length": 9},
                     overrides={"initial_capital": 10000})
    print(f"window [{start:5d}..{start+window:5d}]  pnl={report.net_profit:+.2f}")
    lib.report_free(ctypes.byref(report))
```

Each window starts cold — same equity, same TA seeds, no carry-over
trades from the previous window.

## Why is this still fast?

`strategy_create` is cheap — it allocates one struct and zeros a few
buffers. The expensive work happens inside `run_backtest_full`. On the
tutorial's 672-bar feed, a fresh-handle MACD run takes ~0.5 ms — the
sweep loop's dominant cost is Python-side argument marshalling, not
the engine.

## See also

- [Tutorial: MACD](@ref tutorial_macd) — the single-run baseline
- [Lifecycle](@ref lifecycle) — why one handle per run
- [Configuration](@ref configuration) — every override key
- [`tutorial/run_advanced.py`](https://github.com/pineforge-4pass/pineforge-engine/blob/main/tutorial/run_advanced.py)
  — the shipping reference implementation this example mirrors
