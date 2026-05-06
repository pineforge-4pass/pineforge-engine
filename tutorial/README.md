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

## Modify

`generated.cpp` is plain C++ over `<pineforge/engine.hpp>`. Edit it
(swap `ta::MACD` for `ta::RSI`, change params, add an exit rule),
then rerun whichever path you used. `strategy.pine` is the PineScript
form the C++ mirrors.

Refresh OHLCV: `python3 tutorial/data/fetch_btcusdt.py`
(supports `--symbol`, `--interval`, `--limit`).

Opt out of the tutorial build: `cmake -B build -DPINEFORGE_BUILD_TUTORIAL=OFF`.
