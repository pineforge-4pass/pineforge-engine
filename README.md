<div align="center">

# PineForge

### **The fastest deterministic PineScript v6 backtest runtime — validated trade-for-trade against TradingView.**

[![CI](https://img.shields.io/github/actions/workflow/status/fullpass-4pass/pineforge-engine/ci.yml?branch=main&label=ci&logo=github)](https://github.com/fullpass-4pass/pineforge-engine/actions)
[![Docs](https://img.shields.io/badge/docs-cdocs.pineforge.dev-1565c0?logo=readthedocs&logoColor=white)](https://cdocs.pineforge.dev)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus&logoColor=white)](#)
[![Parity](https://img.shields.io/badge/TV%20parity-167%2F168-brightgreen)](#cross-engine-comparison)
[![Speed](https://img.shields.io/badge/MACD%20672%20bars-0.4%20ms-success)](tutorial/)
[![Free tier](https://img.shields.io/badge/free%20tier-pineforge.dev-22c55e?logo=rocket&logoColor=white)](https://www.pineforge.dev)

**[🚀 Get API Key (free)](https://www.pineforge.dev) · [🎮 Live Playground](https://codegen.pineforge.dev) · [📖 API Documentation](https://cdocs.pineforge.dev) · [⚡ 60-second Tutorial](tutorial/) · [🧪 Coverage Map](docs/coverage.md) · [🔬 Benchmarks](benchmarks/)**

</div>

---

## Backtest PineScript with AI — no build step

The fastest way to use PineForge: let your AI agent write, run, and optimize strategies for you via the **[`@pineforge/codegen-mcp`](https://www.npmjs.com/package/@pineforge/codegen-mcp)** MCP server.

**The workflow:**
1. Agent writes (or you paste) PineScript v6 source
2. MCP transpiles Pine → C++ via the hosted API (source leaves your machine; OHLCV never does)
3. Engine runs locally in Docker — microsecond-class, bit-reproducible results
4. Agent reads the trade list, suggests improvements, sweeps parameters

**Prerequisites:** Node ≥ 20, Docker, a PineForge API key ([free tier at pineforge.dev](https://www.pineforge.dev))

### Claude Code (one command)

```bash
claude mcp add pineforge-codegen \
  --transport stdio \
  --env PINEFORGE_API_KEY=pf_... \
  -- npx -y @pineforge/codegen-mcp
```

### Claude Desktop / Cursor / any MCP client

```jsonc
{
  "mcpServers": {
    "pineforge-codegen": {
      "command": "npx",
      "args": ["-y", "@pineforge/codegen-mcp"],
      "env": { "PINEFORGE_API_KEY": "pf_..." }
    }
  }
}
```

Once connected, your AI agent can:

| What to ask | Tool used |
|---|---|
| "Fetch BTC/USDT 15m data for the last 30 days" | `fetch_binance_ohlcv` |
| "Backtest this SMA-cross strategy on that data" | `backtest_pine` |
| "Sweep fast length 8–21, slow 21–55, rank by net PnL" | `backtest_pine_grid` |
| "What broker overrides are available?" | `list_engine_params` |

> **Try it first.** Paste any Pine v6 strategy at [codegen.pineforge.dev](https://codegen.pineforge.dev) to see the generated C++ before running anything locally.

> **Free tier included.** Sign up at [pineforge.dev](https://www.pineforge.dev) — no credit card required to start.

---

## Why PineForge?

- 🎯 **TradingView-exact.** 167 of 168 internal reference strategies match TV trade-for-trade (165 strict-excellent + 2 strong). The lone outlier is a stress probe at the 1× margin boundary — an edge case where TV's published behaviour is ambiguous and we follow the deterministic interpretation. 48 of 50 in the public three-way benchmark vs PyneCore + PineTS.
- ⚡ **Microsecond-class.** A 672-bar MACD backtest runs in **0.4 ms** end-to-end. Parameter sweeps load one `.so` and re-run with new inputs — no recompile, no fork, no IPC.
- 🔒 **Stable C ABI.** 10 functions, 6 POD types, one header (`<pineforge/pineforge.h>`). Append-only across minor versions, `static_assert`-pinned struct layouts, hidden-visibility hygiene. Drop a strategy `.so` in any harness; it just runs.
- 🧪 **Reproducible to the bit.** Deterministic float ordering, deterministic bar magnifier, no internal RNG seeded from time. Two runs with the same inputs produce bit-identical trade lists.
- 🧰 **FFI-friendly.** Call from Python (`ctypes`), Rust (`libloading`), Go (`cgo`), Node, Julia. Worked examples for [pure C](https://cdocs.pineforge.dev/examples_c.html), [Python sweep](https://cdocs.pineforge.dev/examples_python_sweep.html), [Rust](https://cdocs.pineforge.dev/examples_rust.html), [multi-strategy harness](https://cdocs.pineforge.dev/examples_multi.html), and [magnifier A/B](https://cdocs.pineforge.dev/examples_magnifier.html) ship in the docs.
- 🌍 **Cross-platform CI.** Linux + macOS × Release + Debug. Universal mac binary. Static library, no runtime DSO surprises at deploy time.

---

## For developers: embed the runtime directly

PineForge ships as a static C library (`libpineforge.a`) with a stable 10-symbol C ABI. Call from C, Python, Rust, Go, Node, Julia — one harness, swap strategies forever.

### See it in 30 seconds

```c
#include <pineforge/pineforge.h>

int main(void) {
    pf_strategy_t s = strategy_create(NULL);
    pf_bar_t bars[] = { /* OHLCV ... */ };
    pf_report_t r = {0};

    run_backtest(s, bars, sizeof(bars)/sizeof(*bars), &r);

    printf("%d trades, net %.2f\n", r.trades_len, r.net_profit);

    report_free(&r);
    strategy_free(s);
    return 0;
}
```

That's the entire integration. Every PineForge-compiled strategy `.so` exports the same 10 symbols — write your harness once, swap strategies forever.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 16 tests, ~1 s
bash tutorial/run.sh                          # full MACD backtest in 0.4 ms
```

## Documentation

| Resource | What it covers |
| --- | --- |
| 📖 **[cdocs.pineforge.dev](https://cdocs.pineforge.dev)** | Full C ABI reference, lifecycle, report schema, configuration knobs, magnifier, FFI bindings, ABI stability contract |
| 🚀 **[Getting Started](https://cdocs.pineforge.dev/getting_started.html)** | 60-second build + install + smoke test |
| 🧪 **[Tutorial: MACD on BTC/USDT](https://cdocs.pineforge.dev/tutorial_macd.html)** | End-to-end annotated walkthrough |
| 🔌 **[FFI from Python](https://cdocs.pineforge.dev/ffi_python.html)** | Complete `ctypes` mirror — paste-ready |
| 🦀 **[Calling from Rust](https://cdocs.pineforge.dev/examples_rust.html)** | Idiomatic `libloading` wrapper |
| 🧰 **[CMake integration](https://cdocs.pineforge.dev/integration_cmake.html)** | `find_package(PineForge)` recipe |
| 🔒 **[ABI stability](https://cdocs.pineforge.dev/abi_stability.html)** | Versioning contract + symbol inventory |
| 🗺️ **[Pine v6 coverage map](https://cdocs.pineforge.dev/coverage.html)** | What's implemented, what's not, and why |

The site auto-rebuilds on every push to `main` and every release tag.

---

## What is PineForge?

PineForge is the **C++ runtime** that PineForge-compiled strategies link against. It implements PineScript v6 strategy semantics — order matching, fills, the magnifier, technical indicators, time/session math — as a static C++ library with a stable C ABI.

The runtime is parity-tested **trade-for-trade against TradingView's "List of Trades" CSV exports** on an internal reference corpus: **165 strict-excellent + 2 strong = 167/168 matching**, with **1 stress probe at the 1× margin boundary** that exercises an edge case where TV's published semantics are ambiguous. 168 total strategies = 162 reference + 6 parity probes under the canonical verifier. That corpus is **not shipped** in public checkouts; see **Contributing** / private `corpus` submodule below.

This repository ships:

- `libpineforge.a` — the static runtime library
- `<pineforge/pineforge.h>` — the public C ABI (the canonical, stability-pinned consumer surface)
- `<pineforge/*.hpp>` — the internal C++ headers (used by the closed PineForge transpiler; not part of the stability guarantee)
- A 16-binary ctest suite (15 C++ + 1 pure-C ABI sanity test) that runs in CI on every commit (~81% line coverage of `src/` measured via `bash scripts/coverage.sh`)
- `**corpus/`** (optional **git submodule**) — **168 reference strategies** (162 base + 6 parity probes) for maintainers who have access: each folder has `strategy.pine`, `generated.cpp`, `tv_trades.csv`, and `engine_trades.csv`, plus shared OHLCV under `corpus/data/`. Run `bash scripts/run_corpus.sh` after `git submodule update --init corpus`. Public clones omit this content.
- `[benchmarks/](benchmarks/)` — **three-way engine comparison** (PineForge ↔ [PyneCore](https://github.com/PyneSys/pynecore) ↔ [PineTS](https://github.com/LuxAlgo/PineTS)) on 50 strategies and 10 canonical indicators. The harness code and reports live here; **fixtures** (pinned OHLCV, every `strategies/`* folder with TV exports and trade CSVs) ship only via an optional **private `benchmarks/assets` submodule** — same confidentiality class as `corpus/`. With that init’d, `bash benchmarks/run_all.sh` reproduces the headline numbers with zero external API calls. PyneCore Python is official cloud-compiler output (no hand-ports). Headline: PineForge hits canonical *excellent* tier on 48/50 strategies vs PyneCore's 45/50; the 3 outliers are PyneCore-specific defects on bracket / trail / partial-exit semantics

## Coverage

`**[docs/coverage.md](docs/coverage.md)`** is the complete, current map
of which Pine v6 surface this runtime covers — every TA class, every
order primitive, every `request.security()` semantic, plus a structured
inventory of what's deliberately not implemented (with feasibility tags
for each gap). Read it before integrating PineForge as a backend or
auditing the parity claim.

## What this is, and what it isn't

**This is the runtime, not the compiler.** PineForge's PineScript-to-C++ transpiler is closed-source and ships separately. This library is what every compiled strategy `.so` links against: it provides the implementations of `ta.ema`, `strategy.entry`, `request.security`, the bar magnifier, and so on, behind a stable C ABI.

**This is a backtest engine, not a charting library.** PineScript drawing primitives (`plot`, `bgcolor`, `label`, …) compile cleanly but do nothing at runtime. The runtime computes trade execution and reports — it does not render.

**This is not a TradingView clone.** PineForge intentionally diverges from TradingView in a handful of places where TV's behaviour is undocumented or platform-specific (the bar magnifier, deterministic float ordering). Where it converges, it converges **exactly** on the internal reference corpus (`165/168` strict-excellent + 2 strong = `167/168` matching; the 1 outlier is a 1×-margin stress probe that lands on an undocumented TV edge case. **Maintainers only** — init the private `corpus` submodule per `[CONTRIBUTING.md](CONTRIBUTING.md)`). Where it diverges, it documents the divergence.

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


| Symbol                                   | Role                                         |
| ---------------------------------------- | -------------------------------------------- |
| `strategy_create`                        | Allocate a strategy instance                 |
| `strategy_free`                          | Release the instance                         |
| `run_backtest`                           | Run with auto-detected timeframe             |
| `run_backtest_full`                      | Run with timeframe + magnifier configuration |
| `report_free`                            | Free arrays inside a filled `pf_report_t`    |
| `strategy_set_input`                     | Override a Pine `input.*()` value            |
| `strategy_set_override`                  | Override a `strategy(...)` declaration param |
| `strategy_set_magnifier_volume_weighted` | Toggle volume-weighted magnifier             |
| `strategy_set_trace_enabled`             | Toggle per-bar trace recording               |
| `pf_version_get`                         | Runtime version                              |


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
tests/                  - 16 ctest binaries (15 C++ + 1 pure-C ABI sanity)
corpus/                 - private submodule: 168 strategies (maintainers only); see CONTRIBUTING.md
  ├── data/             - reference 36k-bar OHLCV feed (Binance ETH/USDT:USDT 15m)
  └── CMakeLists.txt    - opt-in subproject that compiles every generated.cpp into strategy.so
benchmarks/             - three-way comparison harness vs PyneCore + PineTS
  ├── assets/           - private submodule: data/ (OHLCV) + strategies/ (50 folders, TV-linked CSVs) — OSS omit
  ├── runners/          - cloud_compile, fetch_extended_ohlcv, regenerate_pineforge_trades, ...
  ├── results/          - summary.md, trade_comparison.md, indicator_comparison.md (methodology samples)
  └── run_all.sh        - one-shot: bootstrap + cloud-compile + run + diff (needs assets submodule)
scripts/                - reproducibility tooling
  ├── run_strategy.py   - load any strategy.so via ctypes, write engine_trades.csv
  ├── run_corpus.sh     - one-shot: build all 168 .so + run + verify
  └── verify_corpus.py  - diff each engine_trades.csv against its tv_trades.csv
cmake/                  - PineForgeConfig.cmake.in for downstream find_package()
cmake/smoke_consumer/   - Minimal find_package(PineForge) CI smoke project
.github/workflows/      - CI: Linux + macOS × Release + Debug
```

## Visibility hygiene

Every compiled strategy `.so` that statically links `libpineforge.a` exports **exactly the 10 documented C ABI symbols** and zero internal C++ symbols. This is enforced at the library level:

- `libpineforge.a` is built with `-fvisibility=hidden -fvisibility-inlines-hidden`
- Public symbols are tagged `PF_API` (visibility=default)
- Internal C++ classes (`BacktestEngine`, `ta::`*, `pineforge::internal::`*) are not tagged, so they stay hidden in any final `.so`
- CI runs `scripts/check_c_abi_runtime.py` (runtime `PF_API` split vs transpiler-emitted symbols). Full trade-list parity sweeps use the private `corpus` submodule locally, not in GitHub Actions.

## Versioning

PineForge follows **semantic versioning** at the C ABI level:

- `PATCH`: implementation changes that don't affect ABI
- `MINOR`: append-only ABI additions (new functions, new struct fields appended to existing structs)
- `MAJOR`: breaking ABI changes

A pre-compiled strategy `.so` against runtime `0.X.Y` will keep working against any later runtime within `0.X.Z`. Across major versions, all bets are off.

## Reproducing the corpus run end-to-end

The private validation corpus lets maintainers rebuild and rerun all **168**
compiled PineForge strategies from a fresh clone. If you have access:

```bash
git submodule update --init corpus
bash scripts/run_corpus.sh
```

That builds `libpineforge.a` plus 168 strategy `.so` files, runs each against the
reference OHLCV feed, and rewrites each `engine_trades.csv`. The script also
prints the canonical five-tier corpus summary described in `corpus/README.md`.

Without the submodule, use `**ctest**` and optional local fixtures you own.

## Cross-engine comparison

`[benchmarks/](benchmarks/)` runs the same 50 strategies through
PineForge, PyneCore, and PineTS to spot engine-specific defects vs
TV-side semantics. **Strategy folders and** the 41,307-bar Binance
ETH/USDT:USDT 15m OHLCV live under the private `**benchmarks/assets`**
submodule (`assets/strategies/`, `assets/data/`); public clones omit them.
PyneCore Python sources are the official PyneSys cloud compiler output
(no hand-ports). PineTS handles indicators only — their
strategy backtester is upstream roadmap.

```bash
git submodule update --init corpus benchmarks/assets   # if you have access
git lfs install && git lfs pull   # when OHLCV is LFS-tracked in-repo
bash benchmarks/run_all.sh        # ~3 min from cold; zero API calls
cat benchmarks/results/summary.md
```

Current standings (window-clipped 4-dimension diff vs TV):


| Match degree | PineForge   | PyneCore |
| ------------ | ----------- | -------- |
| 🟢 excellent | **48 / 50** | 45 / 50  |
| 🟢 strong    | 2 / 50      | 2 / 50   |
| 🟡 moderate  | 0           | 2        |
| 🟠 weak      | 0           | 1        |


The 3 PyneCore-only outliers (`liquidity-sweep`, `scalping-strategy`,
`partial-exit-qty-percent`) all involve `strategy.exit(stop=…, limit=…)`
brackets, `trail_`* exits, or `strategy.close(qty_percent=…)` partial
exits — categories where PyneCore's broker emulator differs from TV
and PineForge does not. See `[benchmarks/results/summary.md](benchmarks/results/summary.md)`
for the full per-strategy table and methodology.

## Status

- v0.1 — initial public release. C ABI defined and pinned. Reported **165 strict-excellent + 2 strong = 167/168** TV parity on the internal corpus (private submodule, 168 strategies including 6 parity probes); the lone outlier is a 1×-margin stress probe on an undocumented TV edge case. 48/50 strategies hit canonical *excellent* tier in the three-way benchmark. CI runs on Ubuntu + macOS (ctest + install smoke; no corpus).

## License

Apache License 2.0. See [LICENSE](LICENSE). Third-party notices: [NOTICE](NOTICE). Extended licensing notes (optional benchmark AGPL deps, trademarks, private submodules): [LEGAL.md](LEGAL.md). Community standards: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Security contact: [SECURITY.md](SECURITY.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) (includes the **Apache-2.0** contribution license grant). The short version:

1. Every contribution must keep the parity test green.
2. Public-API changes (anything exported from `<pineforge/pineforge.h>`) require a major-version bump.
3. Internal C++ helpers can change freely as long as the ABI surface stays put.

