# PineForge tutorial — MACD on BTCUSDT 15m, 7 days

End-to-end backtest you can run from a fresh clone in under a minute.

```
tutorial/
├── macd/
│   ├── strategy.pine       Pine v6 reference
│   └── generated.cpp       compiled C++ (becomes strategy.so)
├── data/
│   ├── btcusdt_15m_7d.csv  672 frozen bars (Binance)
│   └── fetch_btcusdt.py    refresh from Binance public API
├── run.py                  ctypes harness + stats
├── run_advanced.py         parameter sweep using ABI overrides
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
  ghcr.io/fullpass-4pass/pineforge-engine:latest > report.json

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
  ghcr.io/fullpass-4pass/pineforge-engine:latest \
  | jq '{applied_inputs, applied_overrides, summary}'
```

Both env vars are JSON `{key: value}` objects (values stringified).
Empty / unset → defaults from `strategy.pine`.

## Modify the strategy

`generated.cpp` is plain C++ over `<pineforge/engine.hpp>`. Edit it
(swap `ta::MACD` for `ta::RSI`, change params, add an exit rule),
then rerun whichever path you used. `strategy.pine` is the PineScript
form the C++ mirrors.

Refresh OHLCV: `python3 tutorial/data/fetch_btcusdt.py`
(supports `--symbol`, `--interval`, `--limit`).

Opt out of the tutorial build: `cmake -B build -DPINEFORGE_BUILD_TUTORIAL=OFF`.
