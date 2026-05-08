# Example — Multi-strategy harness {#examples_multi}

@tableofcontents

Load N different compiled strategies, run them in parallel against the
same OHLCV feed, and rank by net PnL.

The PineForge ABI is **pure C** with **no global state** beyond what
each `.so` keeps internally — different strategy handles never collide,
even across `.so` files compiled at different runtime versions.

## What you'll build

```
$ python3 multi.py strategies/*.so
loaded 12 strategies, 672 bars
running 12 backtests across 8 workers...
  [ ✓] macd_cross.so          trades=49  pnl=  -190.85   1.2 ms
  [ ✓] rsi_meanreversion.so   trades=87  pnl=  +412.30   2.1 ms
  [ ✓] supertrend.so          trades=23  pnl= +1840.55   0.9 ms
  [ ✓] bbands_squeeze.so      trades=14  pnl=  -382.00   0.7 ms
  ...
ranking:
   1. supertrend           pnl=+1840.55  trades=23
   2. ema_ribbon           pnl= +905.20  trades=31
   3. rsi_meanreversion    pnl= +412.30  trades=87
   ...
```

## Why this works

- **Hidden visibility** — each `.so` exports only the 10 documented C
  symbols. Internal C++ classes (`BacktestEngine`, `ta::*`) stay
  hidden. No symbol clashes between strategies.
- **No global runtime state** — every strategy handle owns its own bar
  loop, TA buffers, security caches. Two handles can run on different
  threads with zero coordination.
- **Append-only ABI** — loading a `.so` built against runtime 0.1.0
  alongside one built against 0.1.5 works; both speak the same
  ABI (see [ABI stability](@ref abi_stability)).

## Threading rule

**One handle per thread.** A single `pf_strategy_t` is *not*
thread-safe — its internal state machine assumes serial advance. But
N handles on N threads is fine.

## Source: multi.py

```python
#!/usr/bin/env python3
"""Run multiple strategy.so files in parallel against a shared feed."""
from __future__ import annotations
import csv, ctypes, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "../../tutorial"))
from run import BarC, ReportC          # noqa: E402

ROOT  = Path(__file__).resolve().parent.parent.parent
OHLCV = ROOT / "tutorial" / "data" / "btcusdt_15m_7d.csv"

def load_bars():
    with OHLCV.open(newline="") as f:
        rows = list(csv.DictReader(f))
    n = len(rows)
    bars = (BarC * n)()
    for i, r in enumerate(rows):
        bars[i] = BarC(float(r["open"]), float(r["high"]), float(r["low"]),
                       float(r["close"]), float(r["volume"]), int(r["timestamp"]))
    return bars, n

def make_lib(so_path: str):
    """Open the .so once, wire argtypes, return a ready callable bundle."""
    lib = ctypes.CDLL(so_path)
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

def run_one(so_path: str, bars, n: int):
    name = Path(so_path).stem
    lib  = make_lib(so_path)

    state = lib.strategy_create(b"{}")
    report = ReportC()
    t0 = time.time()
    lib.run_backtest_full(state, bars, n, b"", b"", 0, 4, 3,
                          ctypes.byref(report))
    elapsed_ms = (time.time() - t0) * 1000

    result = {
        "name":     name,
        "trades":   report.trades_len,
        "pnl":      report.net_profit,
        "elapsed":  elapsed_ms,
    }
    lib.report_free(ctypes.byref(report))
    lib.strategy_free(state)
    return result

def main(so_paths: list[str]) -> int:
    bars, n = load_bars()
    print(f"loaded {len(so_paths)} strategies, {n} bars")
    print(f"running {len(so_paths)} backtests across 8 workers...")

    results = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        futures = {ex.submit(run_one, p, bars, n): p for p in so_paths}
        for f in futures:
            r = f.result()
            results.append(r)
            print(f"  [ ✓] {r['name']:25s} trades={r['trades']:3d}  "
                  f"pnl={r['pnl']:+9.2f}  {r['elapsed']:.1f} ms")

    print("\nranking:")
    for i, r in enumerate(sorted(results, key=lambda x: -x["pnl"])[:5], 1):
        print(f"  {i:2d}. {r['name']:25s} pnl={r['pnl']:+8.2f}  trades={r['trades']}")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

## A note on the GIL

`ctypes` releases the GIL across the C call. Multiple Python threads
running `run_backtest_full` concurrently *do* execute in parallel
inside the runtime — the bottleneck is real CPU, not the GIL.

For embarrassingly-parallel workloads (e.g. 1000-strategy comparison
sweeps), `ThreadPoolExecutor` scales linearly with `nproc` until
memory-bandwidth-bound.

## See also

- [ABI stability](@ref abi_stability) — symbol isolation guarantees
- [Lifecycle](@ref lifecycle) — handle thread-safety rule
- The shipping
  [`benchmarks/`](https://github.com/fullpass-4pass/pineforge-engine/tree/main/benchmarks)
  harness uses this exact pattern across 50 strategies.
