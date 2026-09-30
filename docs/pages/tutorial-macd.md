# Tutorial — MACD on BTCUSDT 15m {#tutorial_macd}

@tableofcontents

End-to-end backtest you can reproduce from a fresh clone in under a
minute. Source: [`tutorial/`](https://github.com/pineforge-4pass/pineforge-engine/tree/main/tutorial).

## What this tutorial covers

| Step | What you learn |
| --- | --- |
| 1 | Build the runtime and the committed, pre-transpiled `generated.cpp` into `strategy.so`. |
| 2 | Load `strategy.so` from Python via `ctypes`. |
| 3 | Push an OHLCV feed and call #run_backtest_full. |
| 4 | Read every interesting field of `pf_report_t`. |
| 5 | Free the report and the handle, in order. |

Re-running with other parameters, sweeping a grid and running strategies in
parallel are the example pages at the end.

By the end you'll have the canonical patterns for ad-hoc backtests,
parameter sweeps, walk-forward windows, and live diagnostic capture.

## Layout

```
tutorial/
├── macd/
│   ├── strategy.pine       # PineScript v6 reference
│   └── generated.cpp       # transpiled C++ → becomes strategy.so
├── mtf/                    # two MTF strategies (see Multi-timeframe)
├── data/
│   ├── btcusdt_15m_7d.csv  # 672 frozen bars (Binance)
│   └── fetch_btcusdt.py    # refresh from Binance public API
├── run.py                  # ctypes harness
├── run_stream.py           # historical OHLCV → realtime trade stream
├── run_advanced.py         # parameter sweep using ABI overrides
├── run_mtf.py              # MTF demo: script_tf switch + lower_tf
├── run.sh                  # one-shot: cmake build + run.py
└── CMakeLists.txt
```

## The Pine source

```pine
//@version=6
strategy("MACD Crossover (tutorial)",
     overlay            = false,
     initial_capital    = 1000000,
     currency           = currency.USD,
     process_orders_on_close = false,
     pyramiding         = 1,
     commission_type    = strategy.commission.percent,
     commission_value   = 0,
     slippage           = 0,
     default_qty_type   = strategy.fixed,
     default_qty_value  = 1)

fastLen   = input.int(12, "Fast Length",   minval=1)
slowLen   = input.int(26, "Slow Length",   minval=1)
signalLen = input.int(9,  "Signal Length", minval=1)
src       = input.source(close, "Source")

[macdLine, signalLine, histLine] = ta.macd(src, fastLen, slowLen, signalLen)

longCond  = ta.crossover(macdLine,  signalLine)
shortCond = ta.crossunder(macdLine, signalLine)

if longCond
    strategy.entry("Long",  strategy.long)
if shortCond
    strategy.entry("Short", strategy.short)
```

## Path A — local toolchain

Requires `cmake`, `g++`, and `python3`.

```bash
bash tutorial/run.sh
```

Configures CMake (first time only), builds
`tutorial/macd/strategy.so`, then runs the harness. Expected output:

```
MACD(12,26,9) on BTCUSDT 15m — 672 bars, 2026-04-29 18:15 → 2026-05-06 18:00 UTC
  trades:    50  (17W / 33L, 34.0% win)
  net pnl:   +569.97
  best/worst:+1149.00 / -1111.97
  max dd:    -4045.15
  elapsed:   0.4 ms
```

Numbers depend on the OHLCV snapshot — refresh with
`python3 tutorial/data/fetch_btcusdt.py` to get current Binance bars.

## Path B — Docker

Mount the strategy + OHLCV into the release hub's image,
`ghcr.io/pineforge-4pass/pineforge-release` (a released runtime plus a pinned
`pineforge-codegen`, named by its `engine<E>-codegen<C>` tag; this repository
publishes no image of its own); get a JSON report on stdout.

```bash
docker run --rm \
  -v "$(pwd)/tutorial/macd/strategy.pine:/in/strategy.pine:ro" \
  -v "$(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro" \
  ghcr.io/pineforge-4pass/pineforge-release:latest > report.json

jq '.summary' report.json
```

The image transpiles the `.pine` with its own codegen and runs it on its own
engine, so it gives the numbers of the engine release it carries; the hub's
1.0.0 image (engine v1.0.0, codegen 1.0.0) gives Path A's numbers above.
To build the image yourself, use pineforge-release's `docker/Dockerfile`,
which vendors this tree's `docker/` harness; this repository ships no
Dockerfile.

## Inside run.py — annotated walkthrough

The full harness is ~200 lines. Here's the dataflow, end to end.

### 1. Mirror the C ABI in ctypes

Skipped here — see [FFI from Python](@ref ffi_python) for the ctypes
mirror of the report structs. The harness defines `BarC`, `TradeC`, and `ReportC` exactly
matching `pf_bar_t`, `pf_trade_t`, `pf_report_t`.

### 2. Load OHLCV into a contiguous array

```python
with OHLCV.open(newline="") as f:
    rows = list(csv.DictReader(f))
n = len(rows)
bars = (BarC * n)()
for i, r in enumerate(rows):
    bars[i] = BarC(float(r["open"]), float(r["high"]), float(r["low"]),
                   float(r["close"]), float(r["volume"]), int(r["timestamp"]))
```

`(BarC * n)()` allocates a contiguous block — the runtime walks it as
`pf_bar_t[]` directly, no copying.

### 3. Wire the symbol table

```python
lib = ctypes.CDLL(str(SO))
lib.strategy_create.argtypes  = [ctypes.c_char_p]
lib.strategy_create.restype   = ctypes.c_void_p
lib.run_backtest_full.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
    ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.POINTER(ReportC)]
lib.strategy_free.argtypes = [ctypes.c_void_p]
lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
```

@warning Always set `argtypes`. Without them, Python passes an `int` as a
32-bit C `int` — an `int64_t` argument such as
`strategy_stream_advance_time`'s timestamp loses half its bits.

### 4. Run

```python
state, report = lib.strategy_create(b"{}"), ReportC()

t0 = time.time()
lib.run_backtest_full(state, bars, n,
                      b"",      # input_tf — auto-detect
                      b"",      # script_tf — same as input
                      0, 4, 3,  # magnifier off, 4 samples, ENDPOINTS
                      ctypes.byref(report))
elapsed = time.time() - t0
```

### 5. Read the report

```python
pnls = [report.trades[i].pnl for i in range(report.trades_len)]
wins, losses = sum(p > 0 for p in pnls), sum(p < 0 for p in pnls)

cum = peak = max_dd = 0.0
for p in pnls:
    cum += p
    peak = max(peak, cum)
    max_dd = min(max_dd, cum - peak)

print(f"  trades:    {report.trades_len}  ({wins}W / {losses}L)")
print(f"  net pnl:   {report.net_profit:+.2f}")
print(f"  max dd:    {max_dd:.2f}")
```

### 6. Free

```python
lib.report_free(ctypes.byref(report))
lib.strategy_free(state)
```

Order matters — see [Lifecycle § Free everything](@ref lifecycle).

## More worked examples

The pages below pick up where this tutorial leaves off — each one
is a self-contained, runnable example targeting a specific use case.

| Example | What it shows |
| --- | --- |
| [Pure C example](@ref examples_c) | Same MACD run, no Python. End-to-end C code with `gcc` build. |
| [Historical to realtime streaming](@ref streaming) | Warm the same MACD instance on OHLCV, then continue it on ordered trade ticks. |
| [Parameter sweep in Python](@ref examples_python_sweep) | Re-run one `.so` with a 2-D MACD grid. No recompile per run. |
| [Multi-strategy harness](@ref examples_multi) | Load N `.so` files, run them in parallel against the same feed. |
| [Magnifier on vs off](@ref examples_magnifier) | A/B comparison showing how intra-bar fills change the trade list. |
| [Calling from Rust](@ref examples_rust) | Idiomatic safe Rust wrapper around the C ABI. |

## What to read next

- [Lifecycle](@ref lifecycle) — the four-step strategy-handle pipeline
- [Configuration](@ref configuration) — every override knob
- [Report schema](@ref report_schema) — every field in `pf_report_t`
- [Bar magnifier](@ref magnifier) — when and how to enable intra-bar fills
- [ABI stability](@ref abi_stability) — what you can rely on across versions
