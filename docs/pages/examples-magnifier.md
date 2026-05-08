# Example — Magnifier on vs off {#examples_magnifier}

@tableofcontents

A/B comparison of the same strategy run with and without the
[bar magnifier](@ref magnifier). Quantifies the difference in trade
count, fill prices, and net PnL.

## When the magnifier actually changes anything

The magnifier only fires when **`script_tf > input_tf`** — i.e. the
runtime is aggregating finer input bars up to a coarser script
timeframe. With `input_tf == script_tf`, there's no extra resolution
to recover; turning the magnifier on changes nothing.

The tutorial MACD strategy ships at 15-minute timeframe. Running it
with `input_tf="15", script_tf="60"` (15-minute feed driving an
hourly strategy) makes the magnifier sample intra-hour fills from the
15-minute bars.

## What you'll build

```
$ python3 magnifier_ab.py
PineForge 0.1.1 — 672 input bars → 168 hourly script bars (ratio 4:1)

without magnifier:
  trades:  19   net pnl:  -5096.73   sub_bars:     0   ticks:     0
with magnifier (4 samples, ENDPOINTS):
  trades:  20   net pnl:  -5322.57   sub_bars:   671   ticks:  2684
delta:
  trades:   +1
  net pnl:  -225.84
  one extra trade exited at an intra-hour OHLC point instead of the next bar close

per-mode comparison:
  ENDPOINTS    : trades=20  pnl=-5322.57
  UNIFORM      : trades=20  pnl=-5322.57
  COSINE       : trades=20  pnl=-5322.57
  TRIANGLE     : trades=20  pnl=-5322.57
  FRONT_LOADED : trades=20  pnl=-5322.57
  BACK_LOADED  : trades=20  pnl=-5322.57
```

@note Trade count is identical across all six distribution modes here
because the MACD entry condition (`ta.crossover` evaluated at script-bar
close) is bar-close-deterministic. Distribution modes only differ when
the script has intra-bar `strategy.exit(stop=…)` brackets, trail stops,
or take-profit limits — see [Bar magnifier](@ref magnifier).

## Source: magnifier_ab.py

```python
#!/usr/bin/env python3
"""A/B: same strategy with magnifier off vs on, plus all 6 distributions.
Requires script_tf > input_tf for the magnifier to do anything."""
from __future__ import annotations
import csv, ctypes, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "../../tutorial"))
from run import BarC, ReportC          # noqa: E402

ROOT  = Path(__file__).resolve().parent.parent.parent
SO    = ROOT / "tutorial" / "macd"  / "strategy.so"
OHLCV = ROOT / "tutorial" / "data"  / "btcusdt_15m_7d.csv"

PF_MAGNIFIER_UNIFORM      = 0
PF_MAGNIFIER_COSINE       = 1
PF_MAGNIFIER_TRIANGLE     = 2
PF_MAGNIFIER_ENDPOINTS    = 3
PF_MAGNIFIER_FRONT_LOADED = 4
PF_MAGNIFIER_BACK_LOADED  = 5

DIST_NAMES = {
    PF_MAGNIFIER_UNIFORM:      "UNIFORM",
    PF_MAGNIFIER_COSINE:       "COSINE",
    PF_MAGNIFIER_TRIANGLE:     "TRIANGLE",
    PF_MAGNIFIER_ENDPOINTS:    "ENDPOINTS",
    PF_MAGNIFIER_FRONT_LOADED: "FRONT_LOADED",
    PF_MAGNIFIER_BACK_LOADED:  "BACK_LOADED",
}

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
    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype  = ctypes.c_void_p
    lib.strategy_free.argtypes   = [ctypes.c_void_p]
    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC)]
    lib.report_free.argtypes = [ctypes.POINTER(ReportC)]
    return lib

def run(lib, bars, n, *, magnifier: int, samples: int = 4,
        dist: int = PF_MAGNIFIER_ENDPOINTS) -> tuple[ReportC, int]:
    """Fresh handle per run — never reuse for A/B."""
    s = lib.strategy_create(b"{}")
    r = ReportC()
    # input_tf=15, script_tf=60 — magnifier samples intra-hour from 15m feed.
    lib.run_backtest_full(s, bars, n, b"15", b"60",
                          magnifier, samples, dist, ctypes.byref(r))
    return r, s

def main() -> int:
    if not SO.exists():
        sys.exit("strategy.so missing — run `bash tutorial/run.sh` first")

    lib = make_lib()
    bars, n = load_bars()

    off, h_off = run(lib, bars, n, magnifier=0)
    print(f"input_bars={off.input_bars_processed} -> "
          f"script_bars={off.script_bars_processed} (ratio {off.script_tf_ratio}:1)\n")

    print("without magnifier:")
    print(f"  trades: {off.trades_len:3d}   net pnl: {off.net_profit:+8.2f}   "
          f"sub_bars: {off.magnifier_sub_bars_total:5d}   "
          f"ticks: {off.magnifier_sample_ticks_total:5d}")

    on, h_on = run(lib, bars, n, magnifier=1, samples=4,
                   dist=PF_MAGNIFIER_ENDPOINTS)
    print("with magnifier (4 samples, ENDPOINTS):")
    print(f"  trades: {on.trades_len:3d}   net pnl: {on.net_profit:+8.2f}   "
          f"sub_bars: {on.magnifier_sub_bars_total:5d}   "
          f"ticks: {on.magnifier_sample_ticks_total:5d}")

    print("delta:")
    print(f"  trades:   {on.trades_len - off.trades_len:+3d}")
    print(f"  net pnl:  {on.net_profit - off.net_profit:+8.2f}")

    print("\nper-mode comparison:")
    handles = []
    for dist in (PF_MAGNIFIER_ENDPOINTS, PF_MAGNIFIER_UNIFORM,
                 PF_MAGNIFIER_COSINE, PF_MAGNIFIER_TRIANGLE,
                 PF_MAGNIFIER_FRONT_LOADED, PF_MAGNIFIER_BACK_LOADED):
        r, h = run(lib, bars, n, magnifier=1, samples=4, dist=dist)
        handles.append((r, h))
        print(f"  {DIST_NAMES[dist]:13s}: trades={r.trades_len:3d}  "
              f"pnl={r.net_profit:+8.2f}")

    for r, h in [(off, h_off), (on, h_on), *handles]:
        lib.report_free(ctypes.byref(r))
        lib.strategy_free(h)
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

## How to read the deltas

| Observation | What it tells you |
| --- | --- |
| `sub_bars_total` is 0 with magnifier on | Either the magnifier was actually off, OR `script_tf == input_tf` and there's nothing to magnify. |
| Trade count goes up with magnifier on | Stops/limits resolved intra-bar that would otherwise have rolled over to next-script-bar fills. |
| Trade count unchanged, PnL changes | Same fills, but at finer-grained prices — typical for `strategy.exit(profit, loss)` brackets. |
| Distribution modes give different PnL | Strategy has intra-bar exits (stops, limits, trailing). |
| Distribution modes give identical PnL | Strategy is bar-close-deterministic — distribution choice doesn't matter. |
| `ticks_total ≈ samples * sub_bars_total` | Healthy sample density. Big gap → check for sub-bars too small to magnify. |

## Strategies where the magnifier matters most

- `strategy.exit(stop=…, limit=…)` brackets — the OCA pair fills on whichever level is hit first inside the bar.
- `strategy.exit(trail_points=…, trail_offset=…)` — trailing stops update on every magnified sample.
- `strategy.close(qty_percent=…)` partial closes triggered by an intra-bar level.
- Any strategy that issues `strategy.entry` from inside an `if barstate.isconfirmed == false` block.

For pure bar-close strategies (the tutorial MACD is one), the magnifier
is a no-op on PnL — only `magnifier_sub_bars_total` changes.

## See also

- [Bar magnifier](@ref magnifier) — full sampling model + distribution reference
- [Report schema § Bar magnifier diagnostics](@ref report_schema)
- [Timeframes](@ref timeframes) — when input_tf and script_tf differ
