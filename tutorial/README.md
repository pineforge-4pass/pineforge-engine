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

## Run

```bash
bash tutorial/run.sh
```

Expected:

```
MACD(12,26,9) on BTCUSDT 15m — 672 bars, 2026-04-29 18:15 → 2026-05-06 18:00 UTC
  trades:    49  (16W / 33L, 32.7% win)
  net pnl:   -190.85
  best/worst:+1149.00 / -1111.97
  max dd:    -4045.15
  elapsed:   0.4 ms
```

Numbers depend on the OHLCV snapshot.

## Modify

`generated.cpp` is plain C++ over `<pineforge/engine.hpp>`. Edit it
(swap `ta::MACD` for `ta::RSI`, change params, add an exit rule),
then `bash tutorial/run.sh`. `strategy.pine` is the PineScript form
the C++ mirrors.

Refresh OHLCV: `python3 tutorial/data/fetch_btcusdt.py`
(supports `--symbol`, `--interval`, `--limit`).

Opt out of the tutorial build: `cmake -B build -DPINEFORGE_BUILD_TUTORIAL=OFF`.

## No toolchain? Use Docker

[`docker/`](../docker/README.md) takes any `strategy.cpp` + OHLCV CSV
via mount points and emits a JSON report on stdout — same engine,
zero local install.
