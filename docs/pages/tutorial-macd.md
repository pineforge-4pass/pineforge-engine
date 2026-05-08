# Tutorial — MACD on BTCUSDT 15m {#tutorial_macd}

@tableofcontents

End-to-end backtest you can reproduce from a fresh clone in under a
minute. Source: [`tutorial/`](https://github.com/fullpass-4pass/pineforge-engine/tree/main/tutorial).

## What this tutorial covers

| Step | What you learn |
| --- | --- |
| 1 | Build the runtime + transpile a Pine MACD into `strategy.so`. |
| 2 | Load `strategy.so` from Python via `ctypes`. |
| 3 | Push an OHLCV feed and call #run_backtest_full. |
| 4 | Read every interesting field of `pf_report_t`. |
| 5 | Re-run the same handle with different parameters — no recompile. |
| 6 | Sweep a 2-D parameter grid in parallel using one `.so` per worker. |

By the end you'll have the canonical patterns for ad-hoc backtests,
parameter sweeps, walk-forward windows, and live diagnostic capture.

## Layout

```
tutorial/
├── macd/
│   ├── strategy.pine       # PineScript v6 reference
│   └── generated.cpp       # transpiled C++ → becomes strategy.so
├── data/
│   ├── btcusdt_15m_7d.csv  # 672 frozen bars (Binance)
│   └── fetch_btcusdt.py    # refresh from Binance public API
├── run.py                  # ctypes harness
├── run_advanced.py         # parameter sweep using ABI overrides
├── run.sh                  # one-shot: cmake build + run.py
└── CMakeLists.txt
```

## The Pine source

```pine
//@version=6
strategy("MACD Cross", overlay=false,
         initial_capital=10000, default_qty_type=strategy.percent_of_equity,
         default_qty_value=100, commission_type=strategy.commission.percent,
         commission_value=0.04)

fast   = input.int(12,  "Fast Length")
slow   = input.int(26,  "Slow Length")
signal = input.int(9,   "Signal Length")

[macd, sig, hist] = ta.macd(close, fast, slow, signal)

longCond  = ta.crossover(macd, sig)
shortCond = ta.crossunder(macd, sig)

if longCond
    strategy.entry("L", strategy.long)
if shortCond
    strategy.entry("S", strategy.short)
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
  trades:    49  (16W / 33L, 32.7% win)
  net pnl:   -190.85
  best/worst:+1149.00 / -1111.97
  max dd:    -4045.15
  elapsed:   0.4 ms
```

Numbers depend on the OHLCV snapshot — refresh with
`python3 tutorial/data/fetch_btcusdt.py` to get current Binance bars.

## Path B — Docker

Mount the strategy + OHLCV into the published runtime image; get a JSON
report on stdout.

```bash
docker run --rm \
  -v "$(pwd)/tutorial/macd/generated.cpp:/in/strategy.cpp:ro" \
  -v "$(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro" \
  ghcr.io/fullpass-4pass/pineforge-engine:latest > report.json

jq '.summary' report.json
```

Same engine, identical numbers. Build the image locally instead with
`docker build -t pineforge -f docker/Dockerfile .` if you don't want to
pull from GHCR.

## Inside `run.py` — annotated walkthrough

The full harness is ~80 lines. Here's the dataflow, end to end.

### 1. Mirror the C ABI in `ctypes`

Skipped here — see [FFI from Python](@ref ffi_python) for the complete
mirror. The harness defines `BarC`, `TradeC`, and `ReportC` exactly
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

@warning Always set `argtypes`. Without them, Python silently passes
`int` as 32-bit — your timestamps lose half their bits.

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

The four pages below pick up where this tutorial leaves off — each one
is a self-contained, runnable example targeting a specific use case.

| Example | What it shows |
| --- | --- |
| [Pure C example](@ref examples_c) | Same MACD run, no Python. End-to-end C code with `gcc` build. |
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
