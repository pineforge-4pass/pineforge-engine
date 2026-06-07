# PineForge tutorial — MACD on BTCUSDT 15m, 7 days

End-to-end backtest you can run from a fresh clone in under a minute.

```
tutorial/
├── macd/
│   ├── strategy.pine       Pine v6 reference
│   └── generated.cpp       compiled C++ (becomes strategy.so)
├── mtf/
│   ├── strategy_htf.pine   HTF SMA filter via request.security
│   ├── generated_htf.cpp   → strategy_htf.so
│   ├── strategy_ltf.pine   intra-bar via request.security_lower_tf
│   └── generated_ltf.cpp   → strategy_ltf.so
├── data/
│   ├── btcusdt_15m_7d.csv  672 frozen bars (Binance)
│   └── fetch_btcusdt.py    refresh from Binance public API
├── run.py                  ctypes harness + stats
├── run_advanced.py         parameter sweep using ABI overrides
├── run_mtf.py              MTF demo — script_tf switch + lower_tf
├── run.sh                  one-shot: cmake build + run.py
└── CMakeLists.txt
```

Pick whichever path matches what you have installed.

## Path A — local toolchain (cmake + g++ + python3)

```bash
bash tutorial/run.sh
```

Configures CMake (first time only), builds
`tutorial/macd/strategy.so`, then runs `tutorial/run.py`. Expected:

```
MACD(12,26,9) on BTCUSDT 15m — 672 bars, 2026-04-29 18:15 → 2026-05-06 18:00 UTC
  trades:    49  (16W / 33L, 32.7% win)
  net pnl:   -190.85
  best/worst:+1149.00 / -1111.97
  max dd:    -4045.15
  elapsed:   0.4 ms
```

Numbers depend on the OHLCV snapshot.

## Path B — Docker (no local toolchain)

Mount the strategy + OHLCV into the published runtime image; get a
JSON report on stdout.

```bash
docker run --rm \
  -v "$(pwd)/tutorial/macd/generated.cpp:/in/strategy.cpp:ro" \
  -v "$(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro" \
  ghcr.io/pineforge-4pass/pineforge-engine:latest > report.json

jq '.summary' report.json
```

Same engine, same numbers. Build the image locally instead with
`docker build -t pineforge -f docker/Dockerfile .` if you don't want
to pull from GHCR. Full mount/schema reference in
[`docker/README.md`](../docker/README.md).

## Advanced — re-run with different params, no rebuild

The compiled strategy.so exports two C ABI hooks for runtime overrides:

| Hook                        | Overrides                                  |
| --------------------------- | ------------------------------------------ |
| `strategy_set_input(k, v)`  | `input.*()` named values from strategy.pine (e.g. `"Fast Length"`, `"Slow Length"`, `"Source"`) |
| `strategy_set_override(k, v)` | `strategy(...)` header fields (`initial_capital`, `commission_value`, `default_qty_value`, `pyramiding`, `slippage`, `default_qty_type`, `commission_type`, `process_orders_on_close`) |

### Path A — sweep grid in Python

```bash
python3 tutorial/run_advanced.py
```

Loops a small `(fast, slow)` MACD grid × two qty sizes, prints a
ranked table:

```
MACD sweep on BTCUSDT 15m — 672 bars, 8 configs (commission 0.04% each side)

fast slow qty  trades  win%     net_pnl      max_dd     ms
----------------------------------------------------------------
  12   26   1      49 28.6%    -3270.77    -6093.70    0.1
   8   21   1      65 27.7%    -3318.18    -7270.83    0.4
  ...
```

### Path B — overrides via env vars to the docker image

```bash
docker run --rm \
  -e PINEFORGE_INPUTS='{"Fast Length": "8", "Slow Length": "21"}' \
  -e PINEFORGE_OVERRIDES='{"default_qty_value": "5", "commission_value": "0.04"}' \
  -v "$(pwd)/tutorial/macd/generated.cpp:/in/strategy.cpp:ro" \
  -v "$(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro" \
  ghcr.io/pineforge-4pass/pineforge-engine:latest \
  | jq '{applied_inputs, applied_overrides, summary}'
```

Both env vars are JSON `{key: value}` objects (values stringified).
Empty / unset → defaults from `strategy.pine`.

## Multi-timeframe (MTF) demo

Two extra `.so` files demonstrate the runtime's two MTF surfaces:

- `tutorial/mtf/strategy_htf.so` — chart at 15m, HTF SMA trend filter
  pulled in via `request.security`. Demonstrates **upward** aggregation
  (input feed → coarser TF inside the strategy).
- `tutorial/mtf/strategy_ltf.so` — chart at 15m (or any TF), intra-bar
  1m sub-bars synthesized via `request.security_lower_tf` from each
  chart bar's OHLC path. Demonstrates **downward** synthesis — PF's
  design is that the input feed's resolution is the upper bound on what
  the lower-TF target can be, with no separate finer feed (contrast
  TradingView).

Build and run:

```bash
cmake --build build --target strategy_tutorial_mtf_htf strategy_tutorial_mtf_ltf -j
python3 tutorial/run_mtf.py
```

`run_mtf.py` prints three tables, each with the exact
`run_backtest_full(...)` call signature above it:

1. **Table A** — `script_tf` sweep (`b""`, `b"15"`, `b"60"`, `b"240"`)
   over a fixed input feed. Shows how the same compiled `.so` reinterprets
   cadence per run.
2. **Table B** — `(input_tf, script_tf)` pair matrix. Shows the
   auto-detect → defaulting → concatenation chain on resolved
   `input_tf_seconds` / `script_tf_seconds` / `script_tf_ratio`.
3. **Table C** — lower-TF synthesis ratio at two different input TFs
   (`b"15"` → 15 sub-bars/bar; `b"60"` → 60 sub-bars/bar). Confirms
   `security_feeds_total == (input_tf_seconds / 60) * input_bars_processed`.

Full design notes (validation rules, codegen contract, comparison with
TradingView's lower-TF model) live in
[docs/pages/mtf.md](../docs/pages/mtf.md).

## Modify the strategy

`generated.cpp` is plain C++ over `<pineforge/engine.hpp>`. Edit it
(swap `ta::MACD` for `ta::RSI`, change params, add an exit rule),
then rerun whichever path you used. `strategy.pine` is the PineScript
form the C++ mirrors.

Refresh OHLCV: `python3 tutorial/data/fetch_btcusdt.py`
(supports `--symbol`, `--interval`, `--limit`).

Opt out of the tutorial build: `cmake -B build -DPINEFORGE_BUILD_TUTORIAL=OFF`.
