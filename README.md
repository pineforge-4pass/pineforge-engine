<div align="center">

<img src=".github/assets/pineforge-banner.jpg" alt="PineForge — PineScript backtests, deterministic, on your data" width="900">

# PineForge

**An open-source C++17 engine for backtesting and forward execution, with PineScript support through code generation.**

[![CI](https://img.shields.io/github/actions/workflow/status/pineforge-4pass/pineforge-engine/ci.yml?branch=main&label=ci&logo=github)](https://github.com/pineforge-4pass/pineforge-engine/actions) <!-- pf:scoreboard|parity-badge -->[![Parity](https://img.shields.io/badge/TradingView%20parity-7%2C989%20%2F%207%2C989%20excellent%20or%20strong-brightgreen)](#validation-scoreboard)<!-- /pf --> <!-- pf:inventory.tvTrades|trades-badge -->[![Trades](https://img.shields.io/badge/TradingView%20trades%20graded-4.78M-brightgreen)](#validation-scoreboard)<!-- /pf -->
[![Speed](https://img.shields.io/badge/median%2036%C3%97%20vs%20PyneCore-success)](benchmarks/results/speed.md)<br>
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus&logoColor=white)](#)
[![Docs](https://img.shields.io/badge/docs-cdocs.pineforge.dev-1565c0?logo=readthedocs&logoColor=white)](https://cdocs.pineforge.dev)
[![codegen on PyPI](https://img.shields.io/pypi/v/pineforge-codegen?label=codegen&logo=pypi&logoColor=white)](https://pypi.org/project/pineforge-codegen/)
[![MCP server](https://img.shields.io/badge/MCP-server-1565c0?logo=docker&logoColor=white)](https://github.com/pineforge-4pass/pineforge-backtest-mcp)

**[🌐 pineforge.dev](https://www.pineforge.dev) · [☁️ Hosted MCP](https://mcp.pineforge.dev/mcp) · [🐳 Docker MCP](https://github.com/pineforge-4pass/pineforge-backtest-mcp) · [📦 Transpiler](https://github.com/pineforge-4pass/pineforge-codegen-oss) · [📖 C ABI docs](https://cdocs.pineforge.dev) · [🧪 Coverage map](docs/coverage.md) · [🔬 Benchmarks](benchmarks/)**

</div>

---

## Why PineForge

PineForge is a C++17 engine for backtesting and forward execution, with a C ABI for embedding. The engine has two layers:

1. **A generic kernel** — a Pine-agnostic backtest and forward-execution state machine: order matching and fills, sizing, margin and settlement, the bar magnifier, indicator classes, `request.security()`, time and session math. Some TradingView-shaped names survive in its archive, each ruled in [ADR 0001](docs/adr/0001-kernel-adapter-boundary.md) and held there by `scripts/check_kernel_residuals.py`.
2. **A source-adapter parity runtime** (`src/source/` and `src/compat/pine/`, `PineExecutionAdapter` + `PineStrategyHost`) — maps Pine/TradingView execution semantics onto that kernel. This is where TradingView parity lives.

The separate PineForge compiler, [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss), translates a PineScript v6 script into a C++ strategy that attaches the engine's Pine execution adapter (its releases 1.0.0, 1.0.1 and 1.1.0 do; 0.10.4 predates the adapter); it owns translation, not execution semantics. TradingView comparisons measure this Pine path under the tested configurations. The [order model](docs/pages/fill-model.md) describes the current submodels and the remaining migration work; [Architecture](#architecture-kernel-vs-parity) states the boundary.

- **Proven, not promised.** All <!-- pf:scoreboard.graded|int -->7,989<!-- /pf --> graded probes — <!-- pf:inventory.corpusScripts|int -->309<!-- /pf --> open reference strategies, <!-- pf:inventory.communityScripts|int -->680<!-- /pf --> community-shared scripts and <!-- pf:inventory.probeScripts|int -->61<!-- /pf --> probe scripts the maintainers wrote, on <!-- pf:scoreboard.lanes|int -->18<!-- /pf --> market/timeframe lanes — grade against TradingView's own trade lists: **<!-- pf:scoreboard.excellent|int -->7,970<!-- /pf --> excellent, <!-- pf:scoreboard.strong|int -->19<!-- /pf --> strong, <!-- pf:scoreboard.belowStrong|int -->0<!-- /pf --> below strong**. The graded probes' TradingView trade lists hold <!-- pf:inventory.tvTrades|int -->4,776,328<!-- /pf --> trades; <!-- pf:scoreboard.anomaliesExcluded|int -->17<!-- /pf --> probes with TradingView-side defects are excluded.
- **Open runtime.** The engine and native live runner are Apache-2.0. The separately distributed [PineForge compiler](https://github.com/pineforge-4pass/pineforge-codegen-oss/blob/main/LICENSE) uses PolyForm Noncommercial terms with additional personal-trading permission; commercial use requires a separate license. Public reference strategies, benchmarks and validation tooling are available in their respective repositories; the community-script test set is not redistributed.
- **Fast.** In-process, no interpreter: median **36× faster than PyneCore** on the 200 strategies both engines time, measured at engine `35db01c8` on 2026-09-29 on an AWS c7a.8xlarge with PyneCore timed as a subprocess, its interpreter start-up included (PineForge runs a median 807k bars/s per strategy over its 201 slots with the bar magnifier on; [method](benchmarks/results/speed.md)). Parameter sweeps re-run a loaded `.so` with new inputs — no recompile, no fork.
- **Deterministic to the bit.** Two runs with the same inputs produce identical trade lists. Same on Linux and macOS.
- **Yours to embed.** 122 `PF_API` declarations across two headers — `pineforge.h`'s 79 (62 runtime exports and up to 17 per-strategy functions a generated module defines) and `native_c_api.h`'s 43 native-host declarations — an ABI that is append-only within a major version from 1.0 on. Call it from C, Python, Rust, Go, Node, Julia — or let an AI agent drive it over MCP.

---

## Get a backtest in 60 seconds

### With an AI agent (MCP, Docker only)

```bash
claude mcp add pineforge-backtest \
  -- docker run --rm -i -v "$PWD:/work" ghcr.io/pineforge-4pass/pineforge-backtest-mcp:latest
```

For Cursor, which expands `${workspaceFolder}`, or any MCP client (in Claude Desktop, put an absolute path in its place):

```jsonc
{
  "mcpServers": {
    "pineforge-backtest": {
      "command": "docker",
      "args": ["run", "--rm", "-i", "-v", "${workspaceFolder}:/work",
               "ghcr.io/pineforge-4pass/pineforge-backtest-mcp:latest"]
    }
  }
}
```

Then ask: *"Fetch BTC/USDT 15m for the last 90 days and backtest this strategy"* — the container transpiles Pine → C++ with the bundled [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss), compiles, runs, and hands the agent the trade list. The image is built on the [`pineforge-release`](https://github.com/pineforge-4pass/pineforge-release) image, which ships a released engine and codegen pair; its `engine_info` tool reports the image's version. Your Pine and data stay on your machine; only the Binance tools (`fetch_binance_ohlcv`, `binance_symbols`) call out, to Binance's public API. Mount a directory at `/work`; `-i` is required and `-t` must not be added (a TTY corrupts the JSON-RPC stream).

| Ask | Tool |
|---|---|
| "Fetch BTC/USDT 15m data for the last 30 days" | `fetch_binance_ohlcv` |
| "Backtest this SMA-cross strategy on that data" | `backtest_pine` |
| "Sweep fast 8–21 × slow 21–55, rank by net PnL" | `backtest_pine_grid` |
| "What broker overrides are available?" | `list_engine_params` |

Prefer zero install? The hosted server at **[mcp.pineforge.dev/mcp](https://mcp.pineforge.dev/mcp)** (Streamable HTTP, no key) backtests one configuration at a time on crypto OHLCV it fetches itself (seven venues; the free tier serves the last 365 days), metered per IP by a weekly backtest quota. The npm package [`@pineforge/backtest-mcp`](https://www.npmjs.com/package/@pineforge/backtest-mcp) is the local server run from Node 20+ on the host; it runs each transpile and backtest in the `pineforge-release` image through your Docker daemon.

[![Real backtest on Claude in 60 seconds](https://img.youtube.com/vi/lflD47Bum4w/0.jpg)](https://www.youtube.com/watch?v=lflD47Bum4w)

### From source

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git && cd pineforge-engine
python3 scripts/ci_verify.py release --build-dir build --jobs 4
bash tutorial/run.sh                            # MACD on BTC/USDT, end to end
python3 tutorial/run_stream.py                  # OHLCV warm-up → realtime trades
```

The [shared local/CI verifier](docs/ci.md) includes source guards, tests and installed-package smoke checks. Its ABI check links callers against both the current library and seven separately built, pinned historical libraries. Preparation uses local Git history and the configured compiler, fetching a pinned commit the clone lacks; see the [ABI fixture guide](tests/fixtures/settlement_cpp_abi/README.md) for repeated checks. The first run needs network (the verifier initialises the public `corpus` submodule itself; configure fetches Eigen when no system copy is found) and a full clone: a depth-1 clone fails the benchmark provenance row ([CI guide](docs/ci.md)). CTest itself stays offline.

Prerequisites: CMake ≥ 3.16 to build (the verifier and `ctest --test-dir` need 3.20), a C++17 compiler whose standard library has floating-point `std::to_chars` (libstdc++ from GCC 11 or later, or libc++ 14 or later; on macOS a deployment target of 13.3 or later; CI builds with GCC on Ubuntu 24.04 and Apple Clang on macOS 26), Eigen 3.3+ (fetched automatically if absent), Python 3 for the tests (`-DPINEFORGE_BUILD_TESTS=OFF -DPINEFORGE_BUILD_TUTORIAL=OFF` for a library-only build). `cmake --install build --prefix /usr/local` installs `lib/libpineforge.a` and `lib/libpineforge_kernel.a`, `include/pineforge/`, and the `find_package(PineForge)` config.

### Prebuilt library

The [v1.1.0 release](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.1.0) attaches the library prebuilt: `pineforge-v1.1.0-linux-x86_64.tar.gz`, `pineforge-v1.1.0-linux-aarch64.tar.gz` and `pineforge-v1.1.0-macos-universal.tar.gz`, each with a `.sha256`. Each unpacks to one directory holding what the install above writes (the two archives, `include/pineforge/` and the `find_package(PineForge)` config) plus `LICENSE`, `NOTICE` and `VERSION`; point `CMAKE_PREFIX_PATH` at it. The package asks for Eigen 3.3+: install it on Linux, while the macOS tarball carries Eigen 3.4.0 ([install guide](docs/pages/install.md)).

### Embedded in your own harness

```c
#include <pineforge/pineforge.h>
#include <stdio.h>

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

Every PineForge-compiled strategy `.so` exports this same ABI, so one harness serves every module built against the same `PF_ABI_VERSION` (compare `pf_abi_version()` before a run). Build the harness against a module, or look the names up with `dlopen`/`dlsym`: `-lpineforge` alone defines no `strategy_create`, `run_backtest` or `report_free`. Worked examples for [C](https://cdocs.pineforge.dev/examples_c.html), [Python sweeps](https://cdocs.pineforge.dev/examples_python_sweep.html), [Rust](https://cdocs.pineforge.dev/examples_rust.html), [multi-strategy](https://cdocs.pineforge.dev/examples_multi.html) and [magnifier A/B](https://cdocs.pineforge.dev/examples_magnifier.html) are in the docs.

Lifecycle-aware compiled modules reset Pine variables, indicator/history buffers and the broker book before each batch run or `strategy_stream_begin` warmup. Inputs and runtime settings persist until changed; ticks within a stream continue its state. Regenerate and rebuild existing modules with current codegen and matching engine headers/archive to obtain this behavior; the [internal C++ rebuild boundary](docs/pages/abi-stability.md) is checked at compile/link time.

---

## Three front doors

The engine can be driven three ways. All three run the same kernel, so for the
same requests they match trigger, price fills, book lots and settle
identically; what differs is who writes the strategy, who owns TradingView's
quirks and, for C, the C++ capabilities the 1.0 C surface does not spell (the
1.0 C boundary table in the [native engine guide](docs/pages/native-engine.md)).

### 1. PineScript, through codegen

Write Pine, transpile it, run the `.so`. The Pine adapter reproduces
TradingView's execution semantics on top of the kernel; this is the path the
validation scoreboard below measures.

```bash
pip install "pineforge-codegen==1.1.0"
python3 -c "from pathlib import Path; from pineforge_codegen import transpile; Path('generated.cpp').write_text(transpile(Path('strategy.pine').read_text(), filename='strategy.pine'))"
c++ -std=c++17 -O2 -ffp-contract=off -fbracket-depth=1024 -shared -fPIC generated.cpp \
    -Wl,-force_load,/usr/local/lib/libpineforge.a -o strategy.so
python3 scripts/run_strategy.py . --ohlcv tutorial/data/btcusdt_15m_7d.csv --no-trim-output   # or drive it over the C ABI
```

Run it from the engine checkout, with `strategy.pine` there, after the `cmake --install` above (with a prebuilt tarball instead, add `-I<its directory>/include` and link its `lib/libpineforge.a`). Engine v1.1.0 pairs with pineforge-codegen 1.1.0, as v1.0.1 does with 1.0.1 and v1.0.0 with 1.0.0; codegen 0.10.4 emits C++ for engine v0.13.1, which the headers of v1.0.0, v1.0.1 and v1.1.0 no longer compile. No codegen release has a command-line entry point, so transpile through `transpile()`. The compile line is macOS/Clang's: on Linux link `-Wl,--whole-archive /usr/local/lib/libpineforge.a -Wl,--no-whole-archive`, and drop `-fbracket-depth` for GCC. Linking the whole archive is what puts the runtime's C exports (`pf_abi_version`, the `strategy_stream_*` family) in the module; a script that uses `matrix.*` also needs Eigen's include directory.

### 2. C++, against the kernel

Subclass `NativeStrategyHost`, describe the run once, hand it bars. No Pine,
no codegen, no `src/source`. The complete file is
[`examples/native/hello_kernel.cpp`](examples/native/hello_kernel.cpp) —
the strategy half of it:

```cpp
#include <pineforge/native_host.hpp>

class HelloKernel : public pineforge::NativeStrategyHost {
    int bars_ = 0;
    void on_native_run_begin() override { bars_ = 0; }
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit_market({pineforge::order_action::Transact{1.0}, "hello-long", ""});
        } else if (bars_ == 3) {
            submit_market({pineforge::execution::Flatten{}, "hello-flat", ""});
        }
    }
};
// configure_native(spec) applies one NativeRunSpec; run(bars, n) drives them;
// trade_count() / get_trade(i) read the closed rows back.
```

The other hosts under [`examples/native/`](examples/native/) cover kernel
sizing and its fee reserve, anchored brackets on a price grid, trails in ticks,
a margin model with a real liquidation and an FX-curve roll, account risk
limits, calculation timing, higher-timeframe series, an auxiliary finer feed,
the open book lot by lot, per-bar broker-state hashes, and a kernel-recorded
report. Each is a CTest row in a build with `-DPINEFORGE_BUILD_EXAMPLES=ON` (the `ci_verify.py release` build above has it): `ctest --test-dir build -R '^example_'`.

### 3. C, against the same kernel

Hand the runtime a callback table and drive the kernel from any language with
a C FFI — no C++ in your own code. The complete file is
[`examples/native/hello_kernel_c.c`](examples/native/hello_kernel_c.c); the
41 `strategy_native_*` functions (plus two `strategy_configure_native_ext_*`
functions) are declared in
[`include/pineforge/native_c_api.h`](include/pineforge/native_c_api.h) and
summarised in [Driving the kernel from C](#driving-the-kernel-from-c) below.

```c
#include <pineforge/pineforge.h>

pf_native_callbacks_v1 cb = {0};
cb.struct_size = (uint32_t)sizeof cb;
cb.version     = PF_NATIVE_API_VERSION;
cb.user        = &state;
cb.on_bar      = on_bar;            /* submit / replace / cancel from here */

pf_strategy_t s = strategy_native_host_create_v1(&cb);
strategy_configure_native_v1(s, &spec);   /* or strategy_configure_native_ext_v1(s, &spec, &ext) */
strategy_native_run_v1(s, bars, n, &report);
```

**Coming from PineScript?** [PineScript to native C++](docs/pages/pine-to-native.md)
gives every `strategy.*` builtin, the 19 `strategy()` declaration
parameters and every `request.*` form a row: its C++ spelling and, where the 1.0 C surface has one,
its C spelling and the host or test that exercises it, or, for the 18 rows
ruled *none*, why the kernel has no counterpart. It walks one six-feature strategy from Pine to a native host
end to end. The [native engine guide](docs/pages/native-engine.md) is the
reference underneath it.

## Native live runner

The optional C++17 `pineforge-live` executable uses this engine's native
warmup-to-stream lifecycle. It accepts only normalized ticks or confirmed OHLCV
bars as PineForge feed events from stdin, a file or your own
feed service (exchange translation belongs in an external feed adapter), and commits
inputs plus order-action webhooks to a durable SQLite ledger. Hand-written
C++ strategies use the native contract; generated Pine strategies retain their
compatibility path. Both expose the versioned C ABI used by the runner.

Build with `-DPINEFORGE_BUILD_LIVE_RUNNER=ON`; the option is off by default,
so core-only users do not acquire SQLite/libcurl/OpenSSL dependencies. See
the [native runner guide](runner/README.md) for feed modes, symbol metadata,
feed format, recovery and execution limitations. The existing validation
scoreboard below describes batch backtests; it does not certify new native
live behavior or real broker fills.
Known issue (v1.0.0, v1.0.1): a stream whose input and script timeframes are
equal serves stale `request.security` values after the first realtime bar.
Fixed in 1.1.0 (#325). See
[Streaming known issues](docs/pages/streaming.md#streaming_known_issues) for the
workaround for those versions.

## Validation scoreboard

**Measured <!-- pf:scoreboard.date -->2026-10-04<!-- /pf -->** on main engine <!-- pf:scoreboard.engineCommit|short-code -->`6b77f061`<!-- /pf --> with codegen-oss <!-- pf:scoreboard.codegenCommit|short-code -->`285ac035`<!-- /pf --> (baseline <!-- pf:scoreboard.id|code -->`pineforge-parity-baseline-20261004-engine-6b77f061`<!-- /pf -->, snapshot <!-- pf:scoreboard.snapshotSha256|short-code -->`21639bad`<!-- /pf -->): **<!-- pf:scoreboard.graded|int -->7,989<!-- /pf --> graded probes, <!-- pf:scoreboard.excellent|int -->7,970<!-- /pf --> excellent + <!-- pf:scoreboard.strong|int -->19<!-- /pf --> strong**, <!-- pf:scoreboard.belowStrong|int -->0<!-- /pf --> below strong and <!-- pf:scoreboard.engineErrors|int -->0<!-- /pf --> engine errors across <!-- pf:scoreboard.lanes|int -->18<!-- /pf --> market/timeframe lanes. The historical release 1.0.1 inventory holds <!-- pf:inventory.tvTrades|int -->4,776,328<!-- /pf --> trades.

Release **1.1.0 grades <!-- pf:releases[1.1.0].scoreboard.excellent|int -->7,951<!-- /pf --> excellent / <!-- pf:releases[1.1.0].scoreboard.strong|int -->38<!-- /pf --> strong until the next release**, on <!-- pf:releases[1.1.0].scoreboard.graded|int -->7,989<!-- /pf --> probes (baseline <!-- pf:releases[1.1.0].scoreboard.id|code -->`pineforge-parity-baseline-20261004-engine-7b596622`<!-- /pf -->, <!-- pf:releases[1.1.0].scoreboard.date -->2026-10-04<!-- /pf -->). A main scoreboard advance does not change release results.

The quantities above render from the public [facts tokens](https://github.com/pineforge-4pass/pineforge-release/blob/main/facts/facts.json). Maintain them with `lab facts render --repo . --facts <local facts file or pinned raw URL>`; `lab facts check` with the same inputs reports drift. Grades are registry-derived; the authored-script and closed-trade inventory is explicitly sourced to a historical public README for release 1.0.1, independent of future main population rebinds, not to registry row or slug totals.

| Board | Test set | Result |
|---|---|---|
| **Public** — [open corpus](https://github.com/pineforge-4pass/pineforge-corpus) | 312 reference strategies on BINANCE:ETHUSDT.P 15m, Apache-2.0, reproducible by anyone | this repository's sweep of the v1.0.1 library: **311 excellent + 1 declared anomaly**; the <!-- pf:scoreboard.corpusProbes\|int -->309<!-- /pf --> of them in the measured population: **<!-- pf:scoreboard.scopes.corpus.excellent\|int -->309<!-- /pf --> excellent** |
| **Closed test** | <!-- pf:inventory.closedProbes\|int -->7,680<!-- /pf --> probes of <!-- pf:inventory.closedScripts\|int -->741<!-- /pf --> TradingView scripts across the <!-- pf:scoreboard.lanes\|int -->18<!-- /pf --> lanes: <!-- pf:inventory.communityScripts\|int -->680<!-- /pf --> community-shared scripts (<!-- pf:inventory.communityProbes\|int -->7,179<!-- /pf --> probes), private under TradingView's Terms of Service, and <!-- pf:inventory.probeScripts\|int -->61<!-- /pf --> probe scripts the maintainers wrote (<!-- pf:inventory.probeScriptProbes\|int -->501<!-- /pf --> probes) | **<!-- pf:scoreboard.scopes.closed.excellent\|int -->7,661<!-- /pf --> excellent + <!-- pf:scoreboard.scopes.closed.strong\|int -->19<!-- /pf --> strong** |

### Lane by lane

<!-- pf:scoreboard.pairs|lanes -->

| Market · timeframe | Probes graded | Excellent | Strong | Below strong |
|---|---:|---:|---:|---:|
| BINANCE:ETHUSDT.P · 15m *(hard lane: zero regression allowed)* | 1,009 | 1,005 | 4 | 0 |
| BINANCE:BTCUSDT · 15m | 668 | 666 | 2 | 0 |
| BINANCE:BTCUSDT · 1D | 516 | 516 | 0 | 0 |
| BINANCE:ETHUSDT.P · 1D | 540 | 540 | 0 | 0 |
| CME_MINI:ES1! · 15m | 76 | 76 | 0 | 0 |
| CME_MINI:ES1! · 1D | 35 | 35 | 0 | 0 |
| CME_MINI:NQ1! · 15m | 74 | 74 | 0 | 0 |
| CME_MINI:NQ1! · 1D | 37 | 37 | 0 | 0 |
| NASDAQ:AAPL · 15m | 632 | 631 | 1 | 0 |
| NASDAQ:AAPL · 1D | 464 | 464 | 0 | 0 |
| NSE:NIFTY · 15m | 314 | 314 | 0 | 0 |
| NSE:NIFTY · 1D | 225 | 225 | 0 | 0 |
| NYSE:F · 15m | 637 | 630 | 7 | 0 |
| NYSE:F · 1D | 505 | 504 | 1 | 0 |
| OANDA:EURUSD · 15m | 653 | 650 | 3 | 0 |
| OANDA:EURUSD · 1D | 453 | 453 | 0 | 0 |
| OANDA:XAUUSD · 15m | 695 | 694 | 1 | 0 |
| OANDA:XAUUSD · 1D | 456 | 456 | 0 | 0 |
| **Total** | **7,989** | **7,970** | **19** | **0** |

<!-- /pf -->

The hard lane combines <!-- pf:scoreboard.hardLane.corpusProbes|int -->309<!-- /pf --> public-corpus + <!-- pf:scoreboard.hardLane.closedProbes|int -->700<!-- /pf --> closed-test probes for <!-- pf:scoreboard.hardLane.symbol|code -->`BINANCE:ETHUSDT.P`<!-- /pf --> <!-- pf:scoreboard.hardLane.timeframe -->15<!-- /pf -->m (<!-- pf:scoreboard.hardLane.hardProbes|int -->1,009<!-- /pf --> probes); zero regression is allowed. <!-- pf:scoreboard.anomaliesExcluded|int -->17<!-- /pf --> TradingView-side defects are excluded from grading.

### How a probe is graded

Every script is exported from TradingView as-is (its own inputs, its own defaults) with the chart's trade list at full precision, transpiled with [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss), and run by this engine on the same OHLCV bars. The two trade lists are aligned trade-for-trade and graded by [`scripts/verify_corpus.py`](scripts/verify_corpus.py):

- **excellent** — the same number of trades, ≥ 99% of TradingView's trades matched, the distinct-entry identity check passed, entry and exit prices within 0.01% and PnL within 1% at the 90th percentile (trailing-stop scripts are graded on the *production* profile: exits within 0.05% and PnL within 100%, since a trail fill depends on TradingView's sub-bar path);
- **strong** — ≥ 95% matched, trade count within 6%, entries within 0.1% and exits within 0.5% at p90;
- **moderate** — ≥ 75% coverage with ≥ 90% of the in-window trades matched; **weak** — at least one match; **minimal** — none.

The closed-test grades are the maintainers' own measurement of a fixed population; only the public board can be re-run from this repository.

### Distinct-entry identity

At an exact entry time, price and direction, two or more distinct, non-empty
TradingView entry `Signal` values prove separate entries. For `excellent`,
the engine must provide non-empty entry-incarnation identities and exactly
as many distinct identities at each such key as TradingView has Signals.
Raw trade-row count is not identity evidence: one entry can have several
partial-close or FIFO fragments.

The identity check considers **every TradingView entry key**, including keys
with only one Signal or none. Time and direction must match exactly. An
engine price exactly equal to any of those keys belongs only to that key;
it cannot count toward a nearby multi-Signal key. Without an exact match,
the price may map to a multi-Signal key only when that is the sole
TradingView key within the strict relative entry tolerance, less than
0.01%. A price within tolerance of that key and any other TradingView key
is ambiguous and refuses `excellent`, as do missing engine identities or
a distinct-identity count mismatch.

This projection does not change fragment consolidation, trade matching or
any threshold. In particular, an extra engine entry at a nearby price must
not be merged away to pass the count gate. The regression tape and tests
are described in `tests/fixtures/coof_cascade_identity/README.md`.

### How a change is judged

A change is judged by what it moves: the same population is graded before and after it, and two rules must both hold. On the hard surface (ETHUSDT.P at 15 minutes) **no probe may regress**: none may lose a grade, stop reporting a metric or become an engine error. Every other market and timeframe is pooled and scored over two bands, excellent and excellent+strong: +1 each time a probe enters a band, −1 each time one leaves it. A score above 0 passes, as long as no probe that leaves a band falls more than one grade. A score of exactly 0 passes only if no probe lost a grade or became an engine error and no hard-surface probe got worse on a graded metric; it is flagged *no-improve-no-regression*, or *improved* when a probe got better where the bands cannot see it (a grade rising below them or on the hard surface, or an engine error fixed). Any other 0 fails, and so does a negative score. The merge ruleset requires the maintainers' `pineforge/verify` and `pineforge/parity` commit statuses on the exact PR head; GitHub Actions CI is advisory. Baseline promotion also requires a recorded PASS and an exact-head merge.

### What the closed test taught the engine

Every gap was closed by pinning the rule TradingView actually follows — never by loosening the grader. Each rule was isolated with sensor strategies exported from TradingView (capital sweeps, literal replays, per-bar state encoded into order comments) and landed with a replay test on the recorded bars. Among them: the broker carries money at **ten significant digits** (equity rounding, the whole-order drop band, the one-contract margin call, the raw lot floor on every lot-stepped symbol); a trailing stop restarts from the issuing bar's *close* when `trail_points` changes and never folds that bar's extreme; a zero-offset trail rides the raw running best and its arming open fills at the nearest-tick print; a reversal rejected at placement preserves standing exits and a separately queued `strategy.close`, while the distinct fill-time rejection rules govern stop, limit, and trailing legs; sparse `ta.atr`/`ta.tr` read the chart's previous close on every execution; a resting stop or limit level a hair off the tick grid snaps onto it; early-close sessions complete their higher-timeframe bucket; and account-currency conversion is left out of the comparison entirely, because TradingView's FX series is a moving target no fixed table reproduces.

### Reproduce the public board yourself

```bash
git submodule update --init corpus
git -C corpus lfs pull                  # the 1-minute feed is a ~176 MB Git LFS object
VERIFY=1 scripts/regen_corpus_cpp.sh    # optional, needs Docker: re-transpile every strategy.pine, diff against the shipped generated.cpp
JOBS=8 scripts/run_corpus.sh            # build the 312 strategies, run them, grade vs TradingView, rewrite corpus/validation_report.md
```

The corpus feed is a 1-minute Binance ETH/USDT:USDT tape; `scripts/derive_corpus_feeds.py` derives the 15-minute bars from it into `corpus/data/derived/` on the first run. Every probe folder ships `strategy.pine`, `generated.cpp`, TradingView's trade list (`tv_trades.csv`, or the file its `inputs.json` names) and `engine_trades.csv`. One probe, `anomaly-equity-mirror-strategy-equity-01`, declares `expected_tier: anomaly` in its `inputs.json`; the sweep reports it as `anomaly`, not as a failure, and `scripts/check_corpus_parity.sh` pins that headline: 311 excellent, 1 anomaly.

---

## Cross-engine comparison

[`benchmarks/`](benchmarks/) runs **200 strategies** through PineForge, [PyneCore](https://github.com/PyneSys/pynecore), [PineTS](https://github.com/LuxAlgo/PineTS) and [vectorbt](https://github.com/polakowo/vectorbt). Every engine gets the same 53,929-bar Binance ETH/USDT perpetual 15m feed, and each trade list is graded against TradingView's own export (266,451 trades):

- **100 corpus probes** (slots 001–100) are drawn by mechanism family from the public corpus. Their fixtures are in the public [`benchmarks/assets`](https://github.com/pineforge-4pass/pineforge-benchmarks-assets) submodule.
- **100 closed strategies** (slots 101–200) are TradingView-scraped community scripts on `BINANCE:ETHUSDT.P` 15m. Their artifacts are in the maintainers' evidence store (sha `c77a9c70…`) and are not public.
- PyneSys rejects slot 192's source, so slot 201, from the same stratum, stands in for it in the PyneCore count. PineForge runs all 201 slots.

PyneCore sources are official PyneSys cloud-compiler output, with no hand-ports. PineTS runs only the canonical indicator script: the harness has no PineTS strategy runner, although PineTS has shipped a `strategy.*` namespace since 0.9.17. vectorbt runs the 13 hand-written ports that load. `bash benchmarks/run_all.sh` reproduces the public half with no API keys once the `benchmarks/assets` submodule, uv and Node ≥ 20 are in place ([recipe](benchmarks/README.md#reproduce)).

| Group | Engine | Slots | Trades emitted | TV trades | 🟢 excellent | 🟢 strong | 🟡 moderate | 🟠 weak | 🔴 minimal | ⚪ n/a |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corpus | PineForge | 100 | 139,668 | 139,665 | **100** | 0 | 0 | 0 | 0 | 0 |
| corpus | PyneCore | 100 | 199,554 | 139,665 | 86 | 12 | 1 | 1 | 0 | 0 |
| corpus | vectorbt | 13 | 18,040 | 13,637 | 4 | 6 | 2 | 1 | 0 | — |
| closed | PineForge | 101 | 126,731 | 126,786 | **101** | 0 | 0 | 0 | 0 | 0 |
| closed | PyneCore | 101 | 175,031 | 126,786 | 51 | 34 | 11 | 3 | 1 | 1 |

**PineForge.** At engine `35db01c8`, running every slot's `generated.cpp` as codegen `121b3e6a` regenerates it, every slot grades excellent. Closed slot 181, strong on 2026-09-22 with one extra trade on the window's opening bars, lost that trade with the regenerated `generated.cpp`: the codegen now converts an `na` double to `int` through its na-preserving cast instead of C++'s undefined conversion.

**PyneCore 6.10.3.** It has 63 graded non-excellent rows, and most come from the harness window:

- **32 rows** fail only on PnL (16) or only on trade count (16); their fills match TradingView's. PyneCore's broker trades from the feed's first bar, five months before TradingView's range opens. As a result, percent-of-equity sizing compounds P&L that TradingView never had, and a position already open at the range start adds one trade at the window's leading edge. The PyneCore runner has no counterpart of PineForge's TradingView-window order gate.
- **Multi-timeframe scripts:** 6.10.3's `request.security` fixes lift four of the nine that 6.10.2 reproduced only in part to excellent; four others still reproduce 41.6–91.7 % of TradingView's history.
- **3 grid bots** drift on FIFO drains, and a `str.match` regex filter grades weak.
- The remaining rows fail other gates.

Slot 192 has no PyneCore trade list: PyneSys rejects its source (`"Empty document."`). Slots 140, 166 and 197, which raised a `RuntimeError` in 6.10.2's `request.security` engine, run on 6.10.3.

**Speed** was measured on an AWS c7a.8xlarge (AMD EPYC 9R14, 32 cores, SMT off) running Ubuntu 24.04, every engine in the same window at engine `35db01c8`, each timing process pinned to fixed cores ([`benchmarks/results/speed.md`](benchmarks/results/speed.md)). The 2026-09-22 table was timed on an Apple M4 Max, so its times are not compared with these; only ratios measured in one window are.

- **PineForge:** a median of 66.8 ms per strategy over the feed, in-process with the bar magnifier on (807k bars/s; quartiles 505k–1133k).
- **PyneCore 6.10.3:** a median of 2,629 ms per subprocess (20.5k bars/s). The median per-strategy speedup is **36×** across the 200 strategies both engines time (p5 15×, p95 131×).
- **vectorbt:** a median of 209.3 ms for its 13 ports; PineForge's median per-strategy speedup on the same 13 is 4.2×, and vectorbt is faster on 2 of them.
- **PineTS:** 1416.2 ms for the canonical 10-indicator script.
- **Throughput package** ([`benchmarks/throughput/`](benchmarks/throughput/)): the magnifier-off hot loop runs at a median of **0.78 M bars/s** per strategy (N=201, median of five quiet runs).

**Against the 2026-09-22 table** (engine `063e4460`: PineForge 200 excellent + 1 strong, PyneCore 6.10.2 133 excellent, 15× on the Apple M4 Max):

- **Tiers:** PineForge's strong row is excellent (above), and PyneCore 6.10.3 grades 12 slots higher than 6.10.2 did and none lower.
- **Speed, on this host and in one window:** engine `35db01c8` takes 0.35× the time of `063e4460` per strategy at the median over all 201 slots, each running its own `generated.cpp`, and PyneCore 6.10.3 takes 1.01× the time of 6.10.2 on 26 public slots.

**Not comparable with the 2026-06-11 table** (PineForge 100/100, PyneCore 85/100, 162×):

- **Tiers:** they now come from the canonical `scripts/verify_corpus.py::analyze_strategy` rubric. The old table graded a different 100-strategy population with `compare.py`'s own copy of the rubric, which had drifted from the canonical one and no longer parsed the current tape format.
- **Speed:** that table was timed on the Apple M4 Max. The 2026-06-11 engine, rebuilt on the AWS host and timed in the same window, runs the three probes both populations share 4–8× faster than `35db01c8` ([provenance](benchmarks/results/speed.md#provenance)).

Last refresh **2026-09-29** (engine `35db01c8`, codegen `121b3e6a`, PyneCore 6.10.3, PineTS 0.9.34, vectorbt 0.28.2; timed on an AWS c7a.8xlarge running Ubuntu 24.04); v1.0.0, v1.0.1 and v1.1.0 were released after it, and neither the tiers nor the timings were re-measured on them. Per-strategy table: [`benchmarks/results/summary.md`](benchmarks/results/summary.md). Population manifest: [`benchmarks/results/selection.md`](benchmarks/results/selection.md). Method, fairness and the reproduction recipe: [`benchmarks/README.md`](benchmarks/README.md).

---

## Architecture: kernel vs. parity

```
Pine v6 script
   │  pineforge-codegen (separate repo): Pine → C++ translation only
   ▼
GeneratedStrategy  ── indicator math + strategy.entry / exit / close calls
   │  attach_pine_execution_adapter()
   ▼
Source-adapter parity runtime   src/source/, src/compat/pine/
   PineStrategyHost, PineExecutionAdapter: Pine order lifecycle, bracket legs,
   fill-price and slippage rules, process_orders_on_close / calc_on_order_fills, margin revival,
   trail and stop semantics
   │  generic orders, handles, callbacks
   ▼
Generic kernel   src/engine_*, src/native_*, src/ta_*, magnifier, session_time, …
   matching and fills, slippage, fees, opening admission and settlement,
   bar magnifier, indicators, time and session math
```

- **codegen** owns Pine → C++ translation: the `GeneratedStrategy` with its indicator math and `strategy.*` calls. It does not own execution, fill, bracket or margin semantics.
- **The source adapter** owns TradingView parity: how Pine orders live, fill, bracket, revive and trail, expressed as ordinary kernel orders.
- **The kernel** targets Pine-agnosticism. It changes only for a *generic* capability that carries a recorded ruling — for example per-lot excursion accounting exposed as a kernel capability, or a market-if-touched (fill-through) flag on a limit order. No Pine- or TradingView-specific rule belongs in the kernel; such a rule goes to the source adapter or to codegen. Some TradingView-shaped residue does survive in the kernel archive today; every surviving name is ruled by family in [ADR 0001](docs/adr/0001-kernel-adapter-boundary.md) and held there by `scripts/check_kernel_residuals.py`, which reads the built archive and fails on a name the table does not cover.

Every kernel capability is **opt-in**, so adapter runs stay byte-identical by construction: a bare host asks for what it wants in its `NativeRunSpec`. It can size orders in the kernel (`Sized` with a cash or equity-fraction basis, optionally reserving the percentage fee), ask the kernel to record the equity curve and its metrics (`NativeReportPolicy::KernelRecorded`), declare higher-timeframe `request.security()`-style series (`declare_timeframe_subscriptions`, or `NativeRunSpec::subscriptions`) and a finer auxiliary feed beneath them, declare a per-side margin model with a solved liquidation level, and declare account risk limits. The full map, with the C spelling of each, is in [PineScript to native C++](docs/pages/pine-to-native.md).

A `NativeRunSpec` field the adapter never declares is a recorded decision, not an omission: ADR 0001's ruling table gives every one of them a verdict — native-only (with an example and a test), adapter-policy or adapter-hook — and `scripts/check_native_feature_rulings.py` fails when the table stops being true. Bare native engines (`NativeStrategyHost`) run the kernel without the Pine adapter. Pine frontends must attach it explicitly and follow the [execution attachment and regeneration contract](docs/pine-order-priority-boundary.md); cap-only generated constructors do not opt into the full adapter. [ADR 0001](docs/adr/0001-kernel-adapter-boundary.md) states the boundary and its rules for contributors; [the native feature-parity design](docs/design/native-feature-parity.md) is the inventory and the rulings behind it.

## Building, testing and the gates

```bash
# The shared local/CI verifier. Configure, build, ctest, source guards,
# the ABI matrix, the installed-package smoke check — one command per profile.
python3 scripts/ci_verify.py release --build-dir build-ci-release --jobs 6
python3 scripts/ci_verify.py kernel  --build-dir build-ci-kernel  --jobs 6

# The fast wiring and source checks, no build (needs actionlint 1.7.12 and shellcheck):
python3 scripts/ci_preflight.py --output-dir build-ci-preflight

# Or plain CMake, when you only want a library and the tests:
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && ctest --test-dir build --output-on-failure
```

Options worth knowing (all default off unless noted):

| Option | Effect |
|---|---|
| `PINEFORGE_BUILD_TESTS` | The C++ test suite. **ON** by default. |
| `PINEFORGE_BUILD_SOURCE_LAYER` | **ON** by default. `OFF` builds the kernel alone: `libpineforge.a` then holds exactly the objects of `PineForge::kernel`, the Pine headers are not installed, and every Pine-bound target is skipped. |
| `PINEFORGE_BUILD_EXAMPLES` | The Pine-free native hosts under `examples/native/`, each with its CTest row. |
| `PINEFORGE_BUILD_CORPUS_STRATEGIES` | A `strategy.so` per probe in `corpus/`, for the parity sweep. |
| `PINEFORGE_BUILD_TUTORIAL` | **ON** by default. The MACD tutorial's `strategy.so` under `tutorial/`. |
| `PINEFORGE_BUILD_LIVE_RUNNER` | The `pineforge-live` executable (needs SQLite3, libcurl, OpenSSL). |
| `PINEFORGE_ENABLE_SANITIZERS` | ASan + UBSan. |
| `PINEFORGE_ENABLE_COVERAGE` | Source coverage instrumentation; see `scripts/coverage.sh`. |

The gates a pull request passes, one line each:

| Gate | Command | What it refuses |
|---|---|---|
| TradingView parity | `./scripts/check_corpus_parity.sh --subset` | A trade that moved: 54 probes re-run and hashed against `scripts/corpus_parity_baseline.txt`. The full 312-probe sweep (`--subset` dropped) runs nightly. |
| CTest row floors | `ci_verify.py release` / `kernel` | A test row that vanished: each profile counts the rows that actually ran against a floor. |
| Kernel residuals | `scripts/check_kernel_residuals.py` | A TradingView-shaped name reaching the kernel archive or its installed headers without an ADR 0001 row. |
| Kernel seams | `scripts/check_kernel_seam_rows.py` | A kernel `source_*` seam, or a kernel member or row field the source layer writes, with no ADR 0001 row. |
| Feature rulings | `scripts/check_native_feature_rulings.py` | A `NativeRunSpec` field the adapter does not declare and the ADR does not rule. |
| C surface | `scripts/check_c_abi_runtime.py`, `scripts/check_native_c_api_surface.py` | A `PF_API` export added without its inventory row; a public host member with no C spelling and no recorded reason; a 1.0 C boundary row whose gap has closed or that the native engine guide no longer lists. |
| Twin parity | `scripts/check_twin_parity.py` | A frozen assertion quietly rewritten instead of a behaviour change being argued. |
| Documentation | `scripts/check_doc_anchors.py`, `scripts/check_doc_lint.py`, `scripts/check_pine_to_native_coverage.py` | A `file:line` citation that no longer points at its symbol; a stale epoch, roadmap label or negative claim; a Pine builtin with no row on the migration page. |
| Doc reverts | `scripts/check_doc_reverts.py` | A published sentence deleted, or older wording restored over newer, by a commit whose message does not name it. |

New here? [CONTRIBUTING.md](CONTRIBUTING.md) is the human walkthrough of all of
the above; [Contributing as an LLM](docs/pages/contributing-llm.md) is the same
ground written for an agent that has been handed a brief in this repository.

## What ships here

- `libpineforge.a` — the static runtime, in two layers (the kernel alone also installs as `libpineforge_kernel.a`, `PineForge::kernel`):
  - **generic kernel** — order matching and fills, sizing, margin and settlement, the bar magnifier, 70 indicator classes, `request.security()`, time and session math;
  - **source-adapter parity runtime** — `PineStrategyHost` and `PineExecutionAdapter` (`src/source/`) plus the Pine policy helpers in `src/compat/pine/`, which map Pine/TradingView execution semantics onto the kernel.
- `<pineforge/pineforge.h>` — the public C ABI, the stability-pinned consumer surface.
- `<pineforge/native_c_api.h>` and the native C++ host headers (`native_host.hpp`, `native_run_spec.hpp`, `native_order.hpp`, …) — the kernel's host API, which the [public contract](docs/pages/public-contract.md) covers from 1.0.0.
- `engine.hpp`, `ta.hpp`, `<pineforge/source/*.hpp>`, `<pineforge/compat/pine/*.hpp>` — the C++ the transpiler emits against (generated code derives from `pineforge::source::PineStrategyHost`); outside the version-number guarantee, held by the codegen pairing rule instead.
- C++ unit and recorded TradingView replay tests; CI on Linux + macOS × Release + Debug, sanitizers, and a `find_package` smoke consumer.
- `corpus/` — the 312-strategy public validation corpus (submodule).
- `benchmarks/` — the cross-engine comparison harness (PineForge, PyneCore, PineTS, vectorbt) and the throughput package.
- `scripts/` — `run_corpus.sh`, `verify_corpus.py`, `run_strategy.py` (load any `.so` via ctypes), `regen_corpus_cpp.sh`, `coverage.sh`.

**This is the runtime, not the compiler.** The PineScript → C++ transpiler is [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss) (its release 1.1.0, `pip install "pineforge-codegen==1.1.0"`, pairs with engine v1.1.0), bundled with the runtime in the [`pineforge-release`](https://github.com/pineforge-4pass/pineforge-release) image that the MCP server builds on. **It is a backtest engine, not a chart:** `plot` and `bgcolor` compile and draw nothing; `line`, `box`, `label` and `linefill` objects are kept as data the script can read back, never rendered. **It is not a TradingView clone:** where TradingView's behaviour is undocumented or platform-specific (the bar magnifier's intrabar path, float ordering) PineForge chooses deterministic rules and documents them; where it converges, it converges exactly.

Full coverage map — every TA class, every order primitive, every `request.security()` semantic, and what is deliberately not implemented: [`docs/coverage.md`](docs/coverage.md).

### Timezones and day boundaries

TradingView ties some day-boundary logic (intraday order caps, session rollovers) to `syminfo.timezone` and other calculations to the chart timezone. `scripts/run_strategy.py` runs the engine in UTC unless told otherwise; to set a chart timezone, put `"chart_timezone": "<IANA name>"` in the probe's `inputs.json` (`""` keeps UTC) or pass `--chart-tz`.

---

## Public C ABI

What the version number promises from 1.0.0 — for this C ABI, the native C++
API, the script ABI epoch and the pairing with codegen — is the
[public contract](docs/pages/public-contract.md); what a 0.x user must act on
is in [CHANGELOG.md](CHANGELOG.md).

A newly generated strategy `.so` exposes 78 compiled-strategy `PF_API` declarations
(62 runtime implementations plus sixteen generated exports) plus 43 native-host
declarations: 121 `PF_API` exports in total; a script that declares
`use_bar_magnifier = true` also exports `strategy_declares_bar_magnifier`, bringing
the inventories to 79 and 122. Older modules lack the six opt-in
[checked-settings exports](docs/checked-settings.md). Older modules can also lack
the two opt-in [execution-capability exports](docs/strategy-capabilities.md).
In an optimized build `nm -gU` also shows libc++'s
`std::piecewise_construct`; no project-internal C++ symbol is exported. The two
inventories are pinned by `scripts/check_c_abi_runtime.py`:

| Symbol | Role |
|---|---|
| `strategy_create` / `strategy_free` | Allocate / release a strategy instance |
| `strategy_capabilities_api_version` / `strategy_capabilities_receipt` | Optional immutable compiled execution requirements for stream admission |
| `run_backtest` / `run_backtest_full` | Run with auto-detected timeframe / with timeframe + magnifier configuration |
| `report_free` | Free arrays inside a filled `pf_report_t` |
| `strategy_closed_trade_entry_incarnation` | Per-run physical entry provenance of a closed trade |
| `strategy_set_input` / `strategy_set_override` | Override a Pine `input.*()` value / a `strategy(...)` declaration parameter |
| `strategy_set_magnifier_volume_weighted` | Toggle the volume-weighted magnifier |
| `strategy_declares_bar_magnifier` | Present (returning 1) only in a script that declares `use_bar_magnifier = true`: the host runs it magnified |
| `strategy_set_trace_enabled` | Toggle per-bar trace recording |
| `strategy_set_trade_start_time` | Suppress historical order placement before a time |
| `strategy_stream_begin` / `_push_tick` / `_push_ticks` / `_advance_time` / `_end` / `_fill_report` | Warm on OHLCV, then run realtime on ordered trades |
| `strategy_stream_api_version` / `_push_bar` / `_order_actions_len` / `_order_action_get` / `_order_actions_clear` / `_state_hash` | Native live extension v1: confirmed input bars, physical fill events and observable replay state |
| `strategy_set_chart_timezone` / `strategy_set_syminfo_timezone` / `strategy_set_syminfo_session` | Chart and exchange time |
| `strategy_set_syminfo_mintick` / `_pointvalue` / `_metadata` / `_type` / `_string` | Symbol tick size, point value, numeric metadata, instrument class, string members |
| `strategy_set_native_security_feed` / `strategy_set_aux_security_feed` | Feed `request.security()` from a native higher-timeframe series / an auxiliary bar-aligned feed |
| `strategy_set_symbol_feed` / `_feed_column` / `strategy_set_symbol_facts` / `strategy_set_recorded_series` | Another symbol's data for `request.security()` of that symbol: its own bars (each with its close) and named columns, its `syminfo.*` facts, and recorded request values per chart bar (each behind its own `PINEFORGE_HAS_…_V1` probe; historical runs only) |
| `strategy_set_account_currency_fx_series` | Effective-time quote-to-account FX |
| `strategy_get_last_error` | The latest runtime error |
| `pf_version_get` / `pf_version_string` / `pf_abi_version` | Runtime version, version string, struct-layout version (`PF_ABI_VERSION == 4`) |
| `strategy_execution_contract` / `strategy_configure_native_v1` / `strategy_configure_native_fx_curve_v1` / `_fx_curve_ext_v1` | Query Legacy vs NativeMarketV1; apply the versioned native run specification (an invalid or refused base call returns `-1` and leaves the handle Failed; a cooperative `Aborted` handle may be reused with the same key and a higher run number); stage or clear an immutable native FX curve, `_fx_curve_ext_v1` also writing the typed refusal (`pf_native_fx_curve_error_e`) and the offending point's index |
| `strategy_request_abort` / `strategy_last_run_status` | Cooperative abort of a run in progress; `0`=completed, `1`=aborted |
| `strategy_set_realtime_tail` | Live-runtime surface (ABI v4): the array's last bar is a still-forming tail — `barstate.islast=false`, `last_bar_index`/`last_bar_time` frozen at the horizon bar, no range-end row |
| `strategy_set_probe_suppress_tail_logic` | ABI v4: the last bar runs only the broker's pre-`on_bar` steps (pending-order settlement, intraday-cap/loss checks) and returns — no `on_bar`, no margin-call / `process_orders_on_close` second pass / bracket-reissue processing (the range-end row is `strategy_set_realtime_tail`'s to skip; the flags are independent) |
| `strategy_set_path_order` / `strategy_last_bar_dual_entry_path` | ABI v4: force the intrabar O→H/L→C leg order (`AUTO`/`HIGH_FIRST`/`LOW_FIRST`) for path-dependent fill probing; read which side won a same-bar dual-entry-stop arbitration |
| `strategy_set_broker_state_hash_recording` / `strategy_broker_state_hash` | ABI v4: toggle a 64-bit broker-state hash appended at report points to `pf_report_t::broker_state_hash` (a bare host uses `KernelRecorded`); read the final state's hash |
| `strategy_pending_orders_len` / `strategy_pending_order_get` / `strategy_pending_order_layout` | ABI v4: the resting pending-order book after the most recent run — count, a POD snapshot per order (`pf_pending_order_v1_t`), and the snapshot's self-describing field layout |
| `strategy_pending_order_fill_qty` / `_level_resolved` / `_effective_levels` / `strategy_trail_best_price` | ABI v4: engine-computed values for a resting order — the quantity it would open if filled at a given price, whether its relative offsets resolve yet, its resolved stop/limit/trail-activation levels, and the live position's trail extreme |
| `strategy_position_avg_price` / `strategy_position_cycle_seq` / `strategy_position_size` | ABI v4: the live position's volume-weighted average entry price, its cycle id, and its script-facing signed size |
| `strategy_closed_trade_entry_id` / `_exit_id` / `_exit_comment` / `_close_cause` | ABI v4: per-closed-trade id/comment strings and a `close_cause` (`pf_close_cause_t`: `SCRIPT`, `BRACKET`, `LIQUIDATION` for a margin call, `RISK_LIMIT` for the intraday loss cap, `FILL_CAP` for the intraday fill cap, `RANGE_END`), indexed like `strategy_closed_trade_entry_incarnation` |
| `strategy_current_equity` / `strategy_script_bars_processed` | ABI v4: `initial_capital + netprofit` (not Pine's `strategy.equity`, which also adds open profit); total script bars dispatched by the most recent run |

### Driving the kernel from C

`<pineforge/native_c_api.h>` (included by `pineforge.h`) adds **43 further
`PF_API` functions** for the other direction: a host that is not written in
C++ hands the runtime a callback table and drives the kernel itself — submit,
replace, cancel, execute, read the book — instead of loading a compiled
strategy. They are additive; no symbol, struct or behaviour above changes, and
`scripts/check_c_abi_runtime.py` pins them as a second, disjoint inventory.

| Symbol | Role |
|---|---|
| `strategy_native_host_create_v1` / `strategy_native_host_free` | Allocate / release a host backed by a `pf_native_callbacks_v1` table |
| `strategy_native_run_v1` / `strategy_native_report_free_v1` | Run a batch of bars into a `pf_report_t`; release its arrays (the runtime's own `report_free`, which is otherwise a per-strategy export) |
| `strategy_native_submit_v1` / `_replace_v1` / `_replace_ext_v1` / `_cancel_v1` / `_cancel_all_v1` / `_cancel_where_v1` | The order commands, legal inside a callback or between realtime inputs; `cancel_where` withdraws every live request carrying one comment or one label. `_replace_ext_v1` is `_replace_v1` plus submit's own `reject` out-parameter, so a rejected replace names its `RequestRejectReason` |
| `strategy_native_execute_current_v1` | Execute one live request at the current execution point |
| `strategy_native_position_v1` / `_working_len_v1` / `_working_get_v1` | The physical position, and a copy-out snapshot of the live working book (`pf_native_working_v1`: its appended `trail_has_arm_price` tells a trail with no arm price from one armed at 0.0, and a caller sending an earlier published length, `PF_NATIVE_WORKING_V1_BASE_SIZE` or `PF_NATIVE_WORKING_V1_ARM_SIZE`, is filled exactly that far) |
| `strategy_native_open_lot_count_v1` / `_open_lot_get_v1` | The physical book lot by lot (`pf_native_open_lot_v1`: identity, entry facts, signed units, entry fee, fee-net P&L and excursions at a mark) — `strategy.opentrades.*` for a C host |
| `strategy_native_events_v1` / `_state_v1` | Poll the recorded event history by ordinal; read the lifecycle and its typed failure |
| `strategy_native_acknowledge_events_v1` / `strategy_native_event_window_v1` | Under the `WINDOW` event retention: say which events the host has read, so the kernel drops those command events at the next script-bar boundary; read the oldest ordinal a poll can still return |
| `strategy_native_timeframe_bar_interval_v1` | From inside `on_timeframe_bar`, the delivered bucket's own calendar interval (`pf_native_timeframe_interval_v1`: the C++ `NativeTimeframeBarContext::interval`); `PF_NATIVE_E_STATE` anywhere else |
| `strategy_native_partial_bar_v1` / `_series_bar_v1` / `_trail_state_v1` / `_liquidation_price_v1` | The four optional reads — the bar so far through the last path point consumed, a declared higher-timeframe series' latest bucket, a live trail's projection, the solved liquidation level. Each answers `PF_NATIVE_ABSENT` where the C++ `std::optional` is empty |
| `strategy_native_risk_state_v1` / `_marked_equity_v1` / `_recalculations_v1` / `_continuation_hash_v1` | The generic risk ledger, marked equity at a mark, the driven/suppressed recalculation counters, and the run's continuation identity |
| `strategy_native_margin_call_v1` | One margin call's whole economics by its event ordinal (`pf_native_margin_call_v1`: mark, units, the position before and after, the surviving book's equity and requirement, the re-solved liquidation price) |
| `strategy_native_sized_units_v1` | The units a `PF_NATIVE_INTENT_SIZED` request resolves to under the run's spec — the kernel's own sizing function, as a pure query before submitting |
| `strategy_native_cohort_open_v1` / `_add_v1` / `_remove_v1` | Cohort rosters: a cohort close is `PF_NATIVE_INTENT_HOST_SIZED` owned by `PF_NATIVE_OWNER_BIND_COHORT`, sized by the `on_close_units` hook |
| `strategy_native_declare_subscriptions_v1` / `_ext_v1` | Declare the run's higher-timeframe series from inside `on_run_begin`, replacing the staged list; `_ext_v1` adds a series source per row and writes the kernel's typed refusal (`pf_native_spec_error_e` and its field) |
| `strategy_native_declare_auxiliary_feed_v1` | Declare, replace or withdraw the run's auxiliary finer feed from inside `on_run_begin`, with the same typed refusal |
| `strategy_configure_native_ext_v1` / `strategy_configure_native_ext_result_v1` | Configure from `pf_native_run_spec_v1` **plus** `pf_native_run_spec_ext_v1` (report policy, price grid, calculation timing, open-bar view, margin model, higher-timeframe subscriptions, generic risk limits, the auxiliary finer feed, the retained intrabar path, the slot-label / feed-tolerance / path-order / abort-reporting policies, the event retention and the quantity tolerance). The two specs' enum-valued words stay `uint32_t`, and each names its C enumeration: `pf_native_fee_kind_e`, `pf_native_close_execution_e`, `pf_native_open_directions_e`, `pf_native_report_policy_e`, `pf_native_price_grid_e`, `pf_native_grid_rounding_e`, `pf_native_calc_trigger_e`, `pf_native_open_bar_view_e`, `pf_native_liquidation_sizing_e`, `pf_native_event_retention_e` and the others the header's field comments name. `_ext_result_v1` writes the kernel's typed refusal (`pf_native_spec_error_e` and its field) and, with either out-parameter set, also configures the next run of a Completed handle or one an abort failed |
| `strategy_native_append_auxiliary_bars_v1` / `_ext_v1` | Append a realtime stream's later bars to the run's declared auxiliary finer feed; `_ext_v1` writes the typed append refusal (`pf_native_append_error_e`) and the bar it stopped on |
| `strategy_native_declare_opened_lot_entry_bar_mask_v1` | From inside `on_applied`, say where the fill that opened a lot sat on its entry bar (`pf_native_opened_lot_fill_point_e`: on the bar's path, or after it); the kernel derives the lot's entry-bar mask that `on_lot_excursion`'s facts carry back |
| `strategy_native_api_version` | This surface's layout version (`PF_NATIVE_API_VERSION`) |

The header's **COVERAGE** block lists every public member of
`NativeStrategyHost` with either its C spelling or the reason it has none, and
`scripts/check_native_c_api_surface.py` proves that list is exactly that
class's public surface — a member added without a row, a row naming a member
that no longer exists, or a spelling naming a symbol the C headers do not
declare all fail CI. Every other C++ capability the 1.0 C surface lacks is a
row of the 1.0 C boundary table in the native engine guide, and a
`C_V1_EXCLUSIONS` row of the same checker fails when that gap closes or its C++
declaration goes.

Every native struct is size-prefixed (`struct_size`), and all but `pf_native_run_spec_v1`, `pf_native_fx_curve_v1` and `pf_native_subscription_v1` also carry a `version`; an unknown
size, version or enumerator is refused with a documented negative status and
mutates nothing. `pf_native_run_spec_ext_v1` has six published lengths — the
first published layout (`PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE`), the same
struct with the appended risk tail (`PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE`), that
plus the intrabar / policy tail (`PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE`), the
auxiliary-feed tail (`PF_NATIVE_RUN_SPEC_EXT_V1_AUXILIARY_SIZE`), the event-retention
tail (`PF_NATIVE_RUN_SPEC_EXT_V1_RETENTION_SIZE`), and the current layout with
the quantity-tolerance tail; `pf_native_callbacks_v1`
has four — the first published layout (`PF_NATIVE_CALLBACKS_V1_BASE_SIZE`),
that plus its seven-hook tail (`PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE`), that plus the
policy-hook tail (`PF_NATIVE_CALLBACKS_V1_POLICY_SIZE`), and the current one,
whose trailing `reserved1` marker says the host reads the lot-excursion facts'
`entry_commission` tail. The runtime accepts each, so a host
compiled against an earlier one keeps working unchanged. An **observation**
callback that returns non-zero latches
`NativeFailureCode::CallbackException` and ends the run `Failed`; the
**answering** hooks in the table's two tails instead return a `pf_native_answer_e`
choosing whose answer the kernel uses, and can never fail the run. Streaming
needs no new symbol: the `strategy_stream_*` family takes these handles
unchanged. Worked example: [`examples/native/hello_kernel_c.c`](examples/native/hello_kernel_c.c);
reference: [`docs/pages/native-engine.md`](docs/pages/native-engine.md).

POD types `pf_bar_t`, `pf_trade_tick_t`, `pf_trade_t`, `pf_report_t`, `pf_security_diag_t`, `pf_trace_entry_t`, `pf_version_t`, `pf_trade_stats_t`, `pf_equity_stats_t`, `pf_metrics_t`, `pf_equity_point_t`, `pf_stream_order_action_t`, `pf_native_run_spec_v1`, `pf_native_fx_curve_v1`, `pf_pending_order_v1_t`, `pf_field_desc_t`, the opaque handle `pf_strategy_t` and the enums `pf_magnifier_distribution_t`, `pf_execution_contract_t`, `pf_native_spec_optional_t`, `pf_native_fx_curve_error_t`, `pf_fill_qty_partition_t` and `pf_close_cause_t` complete the surface. ABI v2 added computed trading metrics and a per-bar equity curve; ABI v3 added `pf_trade_t::open_at_end`, TradingView's range-end close of a position still open after the last bar; ABI v4 added the live-runtime accessors above plus `pf_report_t::broker_state_hash` / `broker_state_hash_len` (a per-report-point broker-state hash array, appended after `equity_curve_len`, NULL/0-length unless recording is on and a report point exists) and the `pf_pending_order_v1_t` generated POD mirror of the engine's resting-order record. Check `pf_abi_version()` before running: the report struct is caller-allocated.

Full flag semantics, string lifetimes and the three evidence scripts behind the ABI v4 live surface: [`docs/pages/live-surface.md`](docs/pages/live-surface.md).

**Stability guarantee.** From 1.0.0, within a major version, struct layouts and `extern "C"` signatures are append-only — fields and functions are added, never reordered, removed or retyped; `static_assert`s in `src/c_abi.cpp` pin the layouts. Semantic versioning at the ABI level: PATCH never touches the ABI, MINOR appends, MAJOR breaks; the [public contract](docs/pages/public-contract.md) states each rule and what holds it. The 0.x releases did not keep this: v0.10.2 grew `pf_report_t` (ABI v2) and v0.12.1 added exports, so a 0.x consumer checks `pf_abi_version()` and rebuilds against the release it runs.

---

## Repository layout

```
include/pineforge/      public C ABI (pineforge.h, native_c_api.h), the native C++ host API + internal C++ headers
  ├── source/                         Pine source-adapter headers (pine_adapter.hpp, pine_strategy_host.hpp, …)
  └── compat/pine/                    Pine policy helper headers
src/                    53 .cpp files in two layers
  │ generic kernel (Pine-agnostic)
  ├── c_abi.cpp                       C ABI implementations + layout asserts
  ├── engine_*.cpp                    BacktestEngine: run loop, orders, execution, path resolution,
  │                                   lower-TF emulation, security + aux security, stream, consumer,
  │                                   metrics, report, trade accessors, state hash
  ├── native_*.cpp                    native orders, run spec, calendar, FX curve, execution consumer, C host API
  ├── market_driver / pending_order_mirror / reservation_expansion
  ├── ta_*.cpp                        70 indicator classes (moving averages, oscillators,
  │                                   volatility/trend, extremes/volume, misc)
  ├── magnifier / matrix / session_time / timeframe / timezone / math / str_utils
  │ source-adapter parity runtime (Pine / TradingView semantics)
  ├── source/
  │   ├── pine_strategy_host.cpp      PineStrategyHost: the base every GeneratedStrategy derives from
  │   ├── pine_adapter.cpp            PineExecutionAdapter: Pine order lifecycle, brackets, fills, margin
  │   ├── pine_strategy_commands.cpp  strategy.entry / order / exit / close / cancel lowering
  │   ├── pine_scheduler.cpp, pine_scheduler_native.cpp
  │   ├── pine_aux_security.cpp, pine_security_eval.cpp, pine_state_hash.cpp
  │   └── pine_path_resolve.cpp, pine_ta_length.cpp, market_admission.cpp, magnifier_intrabars.cpp
  └── compat/pine/                    exit_activation, exit_lifecycle, market_admission,
                                      order_birth, order_priority, reservation_expansion
tests/                  C++ unit, TradingView replay and pure-C ABI tests
examples/native/        Pine-free native hosts, C++ and C, each a CTest row
corpus/                 public submodule: 312 strategies + the 1-minute feed (Git LFS); the 15m bars are derived locally
benchmarks/             cross-engine comparison harness, throughput package, results/
scripts/                ci_verify.py, ci_preflight.py, check_corpus_parity.sh, run_corpus.sh,
                        verify_corpus.py, run_strategy.py, and the check_*.py source guards
tutorial/               MACD end-to-end + streaming walkthrough
runner/                 the optional native live runner, pineforge-live
docker/                 the release image's JSON entry point (run_json.py)
docs/                   coverage map, Pine v6 audit, Doxygen site (cdocs.pineforge.dev)
  ├── pages/                          the narrative pages, incl. pine-to-native.md
  ├── design/                         the native feature-parity inventory and rulings
  └── adr/                            0001, the kernel/adapter boundary
cmake/                  PineForgeConfig.cmake.in, PineForgeVersion.cmake + the find_package smoke consumer
```

Documentation: [C ABI reference](https://cdocs.pineforge.dev) · [Getting started](https://cdocs.pineforge.dev/getting_started.html) · [MACD tutorial](https://cdocs.pineforge.dev/tutorial_macd.html) · [Streaming](https://cdocs.pineforge.dev/streaming.html) · [Metrics reference](https://cdocs.pineforge.dev/metrics.html) · [FFI from Python](https://cdocs.pineforge.dev/ffi_python.html) · [Rust](https://cdocs.pineforge.dev/examples_rust.html) · [CMake integration](https://cdocs.pineforge.dev/integration_cmake.html) · [ABI stability](https://cdocs.pineforge.dev/abi_stability.html) · [Public contract](https://cdocs.pineforge.dev/public_contract.html) · [Coverage](https://cdocs.pineforge.dev/coverage.html). The site rebuilds on every push to `main`.

---

## Releases

- **v1.1.0** (2026-10-04) — a minor release: a strategy generated by pineforge-codegen 1.1.0 exports six checked-settings C functions and stops C++ exceptions at its C entry points, and codegen 1.1.0's `transpile_full()` lists the other symbols' feeds a script requests; confirmed-bar streams compute what the batch computes, which fixes the v1.0.0 and v1.0.1 stream `request.security` known issue; Pine adapter parity changes, exact tick-built bar volume, a report-only terminal quote, the Docker harness's lot grid from `syminfo.mincontract` and its multi-symbol `request.security` feeds, and native runner webhook routing, with `pineforge/live_parser.h` and the runner's parser plugins removed (the hub image `pineforge-release:1.1.0` carries the pair); regenerate and relink. See [CHANGELOG.md](CHANGELOG.md).
- **v1.0.1** (2026-10-02) — documentation only: no library, C ABI, script ABI or report change since v1.0.0. It pairs with pineforge-codegen 1.0.1, which compiles history reads of objects, drawings, arrays and matrices where 1.0.0's C++ did not, and stores an int `na` given to a `float` field as `na` (the hub image `pineforge-release:1.0.1` carries the pair); regenerate and relink. See [CHANGELOG.md](CHANGELOG.md).
- **v1.0.0** (2026-09-30) — the first stable release under semantic versioning; from it the engine and pineforge-codegen release one version (codegen 1.0.0; the hub image `pineforge-release:1.0.0` carries the pair). Every change since v0.13.1: C ABI version 4, the native kernel and its C host API, the script ABI epoch v19. What a 0.13.1 user must act on is in [CHANGELOG.md](CHANGELOG.md); what 1.x promises is the [public contract](docs/pages/public-contract.md).
- **v0.13.1** (2026-09-06) — the parity campaign's rounds 7–11: TradingView's broker rules pinned with sensor exports and landed with replay tests — ten-significant-digit money, trailing-stop restarts, zero-offset trails, declined-reversal bracket legs, the surviving `strategy.close`, sparse `ta.atr`/`ta.tr`, same-bar market transactions, early-close higher-timeframe buckets; the corpus keeps every USDT-quoted book in USDT. Closed test 3,880/3,881; corpus 309/309. ABI v3, 32 symbols, 198 tests.
- **v0.13.0** (2026-09-05) — native higher-timeframe and auxiliary `request.security()` feeds, TradingView's range-end close (ABI v3, `pf_trade_t::open_at_end`), the market-entry affordability gate for fixed, cash and explicit-quantity entries, `na` handling in the extremes and `ta.stdev`.
- **v0.7 – v0.12** (June–August 2026) — ABI v2 metrics + equity curve (v0.10.2), historical-to-realtime streaming and `calc_on_order_fills` (v0.11.0), a timestamped account-currency FX series (v0.12.1). See [GitHub releases](https://github.com/pineforge-4pass/pineforge-engine/releases).
- **v0.6.0** — performance sprint: cached static inputs, thread-local timestamp caching, lazy timezone caching; up to 6.7M bars/s.
- **v0.5.0** — Pine v6 compatibility sprint (symbol mappings, constant namespaces, timestamp overloads, collection sorting, bare TA property reads); corpus 234 probes.
- **v0.4.1** — clean-room 228-probe corpus, submodule made public, five engine fixes.
- **v0.1.1 – v0.4.0** — initial release with the pinned C ABI; same-id stop/replace resolution, RMA seed, `-ffp-contract=off`; magnifier gap fills and directional mintick rounding (these fixes shipped in v0.4.0).

---

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) (includes the Apache-2.0 contribution grant), or
[Contributing as an LLM](docs/pages/contributing-llm.md) if you are an agent working from a brief.
The short version: TradingView parity for new work goes in the adapter or in codegen, never in the
kernel; a change that moves a parity-corpus trade says so and re-records `scripts/corpus_parity_baseline.txt` with its evidence; anything exported from
`<pineforge/pineforge.h>` or `<pineforge/native_c_api.h>` is append-only within a major version.
Bug reports with a Pine script, an OHLCV slice and TradingView's trade list are the most valuable
thing you can send — that is exactly how every rule above was found.

## License

Apache License 2.0 — [LICENSE](LICENSE). Third-party notices: [NOTICE](NOTICE). Licensing notes (optional AGPL benchmark deps, trademarks): [LEGAL.md](LEGAL.md). [Code of conduct](CODE_OF_CONDUCT.md) · [Security policy](SECURITY.md).
