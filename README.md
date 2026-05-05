# PineForge

> Deterministic PineScript v6 backtest runtime.

PineForge is the **C++ runtime** that PineForge-compiled strategies link against. It implements PineScript v6 strategy semantics — order matching, fills, the magnifier, technical indicators, time/session math — as a static C++ library with a stable C ABI.

The runtime is parity-tested **trade-for-trade against TradingView's "List of Trades" CSV exports**: 158 of 162 reference strategies match at the strictest tier (count + entry-price + exit-price + P&L all within `1.0% / 0.01% / 0.01% / 1.0%`).

This repository ships:

- `libpineforge.a` — the static runtime library
- `<pineforge/pineforge.h>` — the public C ABI (the canonical, stability-pinned consumer surface)
- `<pineforge/*.hpp>` — the internal C++ headers (used by the closed PineForge transpiler; not part of the stability guarantee)
- A 16-binary ctest suite (15 C++ + 1 pure-C ABI sanity test) that runs in CI on every commit (~81% line coverage of `src/` measured via `bash scripts/coverage.sh`)
- [`corpus/`](corpus/) — **162 reference strategies** as a reproducibility kit: each ships its `strategy.pine` source, the `generated.cpp` produced by the closed transpiler, the TradingView trade export (`tv_trades.csv`), and the engine's own trade output (`engine_trades.csv`). One shell command (`bash scripts/run_corpus.sh`) builds every `generated.cpp` into a `.so`, runs each through the Python harness, and verifies parity against TradingView
- [`benchmarks/`](benchmarks/) — **three-way engine comparison** (PineForge ↔ [PyneCore](https://github.com/PyneSys/pynecore) ↔ [PineTS](https://github.com/LuxAlgo/PineTS)) on 50 strategies and 10 canonical indicators. PyneCore Python sources are produced by the official PyneSys cloud compiler (no hand-ports); the OHLCV is a pinned LFS-tracked snapshot. `bash benchmarks/run_all.sh` reproduces every comparison number from a fresh clone with zero external API calls. Headline: PineForge hits canonical *excellent* tier on 48/50 strategies vs PyneCore's 45/50; the 3 outliers are PyneCore-specific defects on bracket / trail / partial-exit semantics

## Coverage

[**`docs/coverage.md`**](docs/coverage.md) is the complete, current map
of which Pine v6 surface this runtime covers — every TA class, every
order primitive, every `request.security()` semantic, plus a structured
inventory of what's deliberately not implemented (with feasibility tags
for each gap). Read it before integrating PineForge as a backend or
auditing the parity claim.

## What this is, and what it isn't

**This is the runtime, not the compiler.** PineForge's PineScript-to-C++ transpiler is closed-source and ships separately. This library is what every compiled strategy `.so` links against: it provides the implementations of `ta.ema`, `strategy.entry`, `request.security`, the bar magnifier, and so on, behind a stable C ABI.

**This is a backtest engine, not a charting library.** PineScript drawing primitives (`plot`, `bgcolor`, `label`, …) compile cleanly but do nothing at runtime. The runtime computes trade execution and reports — it does not render.

**This is not a TradingView clone.** PineForge intentionally diverges from TradingView in a handful of places where TV's behaviour is undocumented or platform-specific (the bar magnifier, deterministic float ordering). Where it converges, it converges **exactly** (`158/162` of the reference corpus passes byte-for-byte). Where it diverges, it documents the divergence.

## Quickstart

### Prerequisites

- CMake ≥ 3.16
- A C++17 compiler (GCC ≥ 9, Clang ≥ 10, Apple Clang ≥ 12)
- [Eigen 3.3+](https://eigen.tuxfamily.org/) — used for matrix-typed PineScript. The build will fetch Eigen via CMake `FetchContent` if no system install is found.

### Build + test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expect 16 tests to pass. The largest (`test_integration`, `test_request_security`) take a few hundred milliseconds; everything else completes faster.

### Install

```bash
cmake --install build --prefix /usr/local
```

Installs:

- `lib/libpineforge.a`
- `include/pineforge/*.hpp` and `include/pineforge/pineforge.h`
- `lib/cmake/PineForge/PineForge{Config,Targets,ConfigVersion}.cmake`

### Use from another CMake project

```cmake
find_package(PineForge 0.1 REQUIRED)
target_link_libraries(my_target PRIVATE PineForge::pineforge)
```

```c
#include <pineforge/pineforge.h>

int main(void) {
    pf_version_t v = pf_version_get();
    /* ... */
}
```

## Public C ABI (the stability surface)

`<pineforge/pineforge.h>` is the **single canonical consumer header**. Every compiled PineForge strategy `.so` exports exactly the 10 symbols declared there:

| Symbol | Role |
| --- | --- |
| `strategy_create` | Allocate a strategy instance |
| `strategy_free` | Release the instance |
| `run_backtest` | Run with auto-detected timeframe |
| `run_backtest_full` | Run with timeframe + magnifier configuration |
| `report_free` | Free arrays inside a filled `pf_report_t` |
| `strategy_set_input` | Override a Pine `input.*()` value |
| `strategy_set_override` | Override a `strategy(...)` declaration param |
| `strategy_set_magnifier_volume_weighted` | Toggle volume-weighted magnifier |
| `strategy_set_trace_enabled` | Toggle per-bar trace recording |
| `pf_version_get` | Runtime version |

POD types (`pf_bar_t`, `pf_trade_t`, `pf_report_t`, `pf_security_diag_t`, `pf_trace_entry_t`, `pf_version_t`) and the `pf_magnifier_distribution_t` enum complete the surface.

**Stability guarantee:** within the same `PINEFORGE_VERSION_MAJOR`, struct layouts and `extern "C"` signatures are append-only. New fields may be appended; existing fields are never reordered, removed, or retyped. New functions may be added; existing functions are never removed or signature-changed. Compile-time `static_assert`s in `src/c_abi.cpp` pin the layouts against drift.

The C++ headers (`<pineforge/engine.hpp>`, `<pineforge/ta.hpp>`, ...) are *internal* implementation surface — used by the closed PineForge transpiler, not part of the stability guarantee, and not recommended for external consumption.

## Repository layout

```
include/pineforge/      - public C ABI + internal C++ headers
src/                    - implementation (~25 .cpp files split by concern)
  ├── c_abi.cpp                       runtime-side C ABI implementations + layout asserts
  ├── engine_*.cpp                    BacktestEngine implementation, split by concern:
  │   ├── engine_path_resolve.cpp     intra-bar OHLC path-resolution helpers
  │   ├── engine_lower_tf.cpp         lower-timeframe emulation
  │   ├── engine_orders.cpp           execute_market_*, partial exits
  │   ├── engine_fills.cpp            process_pending_orders fill loop
  │   ├── engine_security.cpp         request.security registration
  │   ├── engine_run.cpp              run() entrypoints, magnified bar loop
  │   ├── engine_report.cpp           fill_report, trace recording
  │   ├── engine_strategy_commands.cpp strategy.entry/exit/close/cancel/order
  │   ├── engine_trade_accessors.cpp  strategy.opentrades.*
  │   └── engine_risk.cpp             risk gates + per-trade extremes
  ├── engine_internal.hpp             private cross-TU header (path-resolve helpers, types)
  ├── ta_*.cpp                        66 indicator classes split by category:
  │   ├── ta_moving_averages.cpp      RMA, SMA, EMA, WMA, HMA, VWMA, ALMA, SWMA
  │   ├── ta_oscillators.cpp          RSI, Stoch, CCI, MFI, CMO, TSI, WPR, COG, RCI, ...
  │   ├── ta_volatility_trend.cpp     ATR, BB, KC, MACD, DMI, SAR, Supertrend, ...
  │   ├── ta_extremes_volume.cpp      Highest/Lowest, OBV, AccDist, NVI/PVI/PVT, VWAP, ...
  │   └── ta_misc.cpp                 Linreg, PercentRank, BarsSince, ValueWhen, ...
  └── magnifier.cpp / matrix.cpp / session_time.cpp / str_utils.cpp / timeframe.cpp / timezone.cpp / math.cpp
tests/                  - 14 ctest binaries
corpus/                 - 162 reference strategies × {strategy.pine, generated.cpp, tv_trades.csv, engine_trades.csv}
  ├── data/             - reference 36k-bar OHLCV feed (Binance ETH/USDT:USDT 15m)
  └── CMakeLists.txt    - opt-in subproject that compiles every generated.cpp into strategy.so
benchmarks/             - three-way comparison vs PyneCore + PineTS (50 strategies)
  ├── data/             - LFS-tracked extended OHLCV (41,307 bars; covers full TV history)
  ├── strategies/       - 50 × {strategy.pine, strategy_pyne.py, tv_trades.csv, *_trades.csv}
  ├── runners/          - cloud_compile, fetch_extended_ohlcv, regenerate_pineforge_trades, ...
  ├── results/          - summary.md, trade_comparison.md, indicator_comparison.md
  └── run_all.sh        - one-shot: bootstrap + cloud-compile + run + diff
scripts/                - reproducibility tooling
  ├── run_strategy.py   - load any strategy.so via ctypes, write engine_trades.csv
  ├── run_corpus.sh     - one-shot: build all 162 .so + run + verify
  └── verify_corpus.py  - diff each engine_trades.csv against its tv_trades.csv
cmake/                  - PineForgeConfig.cmake.in for downstream find_package()
.github/workflows/      - CI: Linux + macOS × Release + Debug
```

## Visibility hygiene

Every compiled strategy `.so` that statically links `libpineforge.a` exports **exactly the 10 documented C ABI symbols** and zero internal C++ symbols. This is enforced at the library level:

- `libpineforge.a` is built with `-fvisibility=hidden -fvisibility-inlines-hidden`
- Public symbols are tagged `PF_API` (visibility=default)
- Internal C++ classes (`BacktestEngine`, `ta::*`, `pineforge::internal::*`) are not tagged, so they stay hidden in any final `.so`
- The CI verifies the export table after build

## Versioning

PineForge follows **semantic versioning** at the C ABI level:

- `PATCH`: implementation changes that don't affect ABI
- `MINOR`: append-only ABI additions (new functions, new struct fields appended to existing structs)
- `MAJOR`: breaking ABI changes

A pre-compiled strategy `.so` against runtime `0.X.Y` will keep working against any later runtime within `0.X.Z`. Across major versions, all bets are off.

## Reproducing the parity claim end-to-end

The headline number — **158/162 strategies pass at strict TV parity** —
is fully reproducible from this repository alone. Run:

```bash
bash scripts/run_corpus.sh
```

That script builds `libpineforge.a` plus 162 strategy `.so` files (one
per `corpus/<cat>/<name>/generated.cpp`), runs each `.so` against the
reference 36k-bar OHLCV feed, and emits `engine_trades.csv` per strategy.
The same script then diffs every fresh `engine_trades.csv` against the
shipped `tv_trades.csv` via `scripts/verify_corpus.py`.

The engine is deterministic: re-running this pipeline reproduces the
committed `engine_trades.csv` files **byte-for-byte** (same hash, same
trade list, same per-trade P&L). If your machine produces a different
result, that is a bug worth filing.

See [`corpus/README.md`](corpus/README.md) for layout, schema, and
threshold profiles.

## Cross-engine comparison

[`benchmarks/`](benchmarks/) runs the same 50 strategies through
PineForge, PyneCore, and PineTS to spot engine-specific defects vs
TV-side semantics. Each engine consumes the same 41,307-bar Binance
ETH/USDT:USDT 15m feed (LFS-tracked at `benchmarks/data/`). PyneCore
Python sources are the official PyneSys cloud compiler output
(committed; no hand-ports). PineTS handles indicators only — their
strategy backtester is upstream roadmap.

```bash
git lfs install && git lfs pull   # 2.3 MB OHLCV pin
bash benchmarks/run_all.sh        # ~3 min from cold; zero API calls
cat benchmarks/results/summary.md
```

Current standings (window-clipped 4-dimension diff vs TV):

| Match degree | PineForge | PyneCore |
|---|---:|---:|
| 🟢 excellent | **48 / 50** | 45 / 50 |
| 🟢 strong | 2 / 50 | 2 / 50 |
| 🟡 moderate | 0 | 2 |
| 🟠 weak | 0 | 1 |

The 3 PyneCore-only outliers (`liquidity-sweep`, `scalping-strategy`,
`partial-exit-qty-percent`) all involve `strategy.exit(stop=…, limit=…)`
brackets, `trail_*` exits, or `strategy.close(qty_percent=…)` partial
exits — categories where PyneCore's broker emulator differs from TV
and PineForge does not. See [`benchmarks/results/summary.md`](benchmarks/results/summary.md)
for the full per-strategy table and methodology.

## Status

- v0.1 — initial public release. C ABI defined and pinned. 158/162 strategies pass at strict TV parity (corpus); 48/50 strategies hit canonical *excellent* tier in the three-way benchmark. CI runs on Ubuntu + macOS.

## License

Apache License 2.0. See [LICENSE](LICENSE).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The short version:

1. Every contribution must keep the parity test green.
2. Public-API changes (anything exported from `<pineforge/pineforge.h>`) require a major-version bump.
3. Internal C++ helpers can change freely as long as the ABI surface stays put.
