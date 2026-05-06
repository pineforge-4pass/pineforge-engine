# PineForge tutorial — MACD on BTCUSDT 15m, 7 days

A minimal, end-to-end PineForge backtest you can run from a fresh
clone in under a minute. Everything lives under `tutorial/`:

```
tutorial/
├── macd/
│   ├── strategy.pine       Pine v6 source for reference
│   ├── generated.cpp       C++ shape the runtime loads (compiled)
│   ├── strategy.so         BUILT — `.so` produced by CMake
│   └── trades.csv          BUILT — written by run.py per backtest
├── data/
│   ├── btcusdt_15m_7d.csv  frozen 672 bars, BTCUSDT 15m (Binance)
│   └── fetch_btcusdt.py    refresh the CSV from Binance public API
├── run.py                  ctypes harness + stats summary
├── run.sh                  one-shot: cmake build + run.py
└── CMakeLists.txt          builds tutorial/macd/strategy.so
```

## Quickstart

From the repo root:

```bash
bash tutorial/run.sh
```

That configures CMake (first time only), builds
`tutorial/macd/strategy.so`, then runs the backtest. Expected output:

```
PineForge tutorial — MACD crossover on BTCUSDT 15m (7d)
─────────────────────────────────────────────────────────
Loaded 672 bars  2026-04-29 18:15 → 2026-05-06 18:00 UTC
Strategy: MACD(12, 26, 9), src=close, qty=1 contract, fixed

Backtest: 0.00s

Results
  Total trades:      49
  Wins / Losses:     16 / 33   (32.7% win rate)
  Net PnL:           -190.85 USDT
  Avg trade:         -3.89 USDT
  Best / Worst:      +1149.00 / -1111.97
  Max drawdown:      -4045.15 USDT
  Bars processed:    672

Trades CSV → tutorial/macd/trades.csv
```

(Numbers depend on the OHLCV snapshot — yours will differ if you ran
`fetch_btcusdt.py` first.)

## What gets built

The top-level CMake exposes `option(PINEFORGE_BUILD_TUTORIAL ON)`, so
plain

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

produces `tutorial/macd/strategy.so` alongside the runtime library and
test suite. To opt out:

```bash
cmake -B build -DPINEFORGE_BUILD_TUTORIAL=OFF
```

## The strategy

A textbook MACD line / signal crossover (12 / 26 / 9 EMA, source=
close):

- `ta.crossover(macd, signal)`  → `strategy.entry("Long", strategy.long)`
- `ta.crossunder(macd, signal)` → `strategy.entry("Short", strategy.short)`

Pyramiding 1, fixed qty 1 contract, no commission, no slippage. See
[`macd/strategy.pine`](macd/strategy.pine) for the Pine source and
[`macd/generated.cpp`](macd/generated.cpp) for the compiled C++ shape
the runtime loads. Both files describe the same logic.

## The runner

[`run.py`](run.py) is self-contained — the ctypes mirror of
`<pineforge/pineforge.h>`, the CSV loader, and the stats block all
live in one ~250-line file you can read top-to-bottom. It does not
import anything from `scripts/` so reading it is the tutorial.

Useful flags:

```bash
python3 tutorial/run.py --so tutorial/macd/strategy.so \
                       --ohlcv tutorial/data/btcusdt_15m_7d.csv \
                       -o tutorial/macd/trades.csv
```

## Refreshing the OHLCV

The checked-in CSV is a frozen snapshot so backtests are reproducible.
To pull the latest 7 days of BTCUSDT 15m bars (no auth, public Binance
endpoint):

```bash
python3 tutorial/data/fetch_btcusdt.py
```

Other symbols / intervals work too:

```bash
python3 tutorial/data/fetch_btcusdt.py --symbol ETHUSDT --interval 5m --limit 1000
```

## Modifying the strategy

`generated.cpp` is plain C++ that links against the public engine
headers in `include/pineforge/`. Edit it directly — change indicator
parameters, swap `ta::MACD` for `ta::RSI`, add an exit rule — then
rerun:

```bash
bash tutorial/run.sh
```

The sibling `strategy.pine` is kept for reference so you can see the
PineScript v6 form the C++ mirrors.

## Next steps

- Browse `corpus/basic/` for nine more compact strategy examples
  (requires the private `corpus` submodule; see top-level
  `CONTRIBUTING.md`).
- Compare against `scripts/run_strategy.py` — the production harness
  that drives the 162-strategy parity sweep. The tutorial runner is
  the same shape with the dial-tone code stripped out.
- Read [`docs/coverage.md`](../docs/coverage.md) for the full Pine v6
  surface this runtime supports.
