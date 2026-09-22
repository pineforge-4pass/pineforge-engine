<div align="center">

<img src=".github/assets/pineforge-banner.jpg" alt="PineForge — TradingView-exact PineScript backtests, open source, on your own data" width="900">

# PineForge

**An open-source C++17 engine for backtesting and forward execution, with PineScript support through code generation.**

[![CI](https://img.shields.io/github/actions/workflow/status/pineforge-4pass/pineforge-engine/ci.yml?branch=main&label=ci&logo=github)](https://github.com/pineforge-4pass/pineforge-engine/actions)
[![Parity](https://img.shields.io/badge/TradingView%20parity-4%2C190%20%2F%204%2C190%20probes-brightgreen)](#validation-scoreboard)
[![Trades](https://img.shields.io/badge/trades%20matched-2.8M-brightgreen)](#validation-scoreboard)
[![Speed](https://img.shields.io/badge/median%2015%C3%97%20vs%20PyneCore-success)](benchmarks/results/speed.md)<br>
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

1. **A generic kernel** — a Pine-agnostic backtest and forward-execution state machine: order matching and fills, sizing, margin and settlement, the bar magnifier, indicator classes, `request.security()`, time and session math. It knows nothing about Pine or TradingView.
2. **A source-adapter parity runtime** (`src/source/`, `PineExecutionAdapter` + `PineStrategyHost`) — maps Pine/TradingView execution semantics onto that kernel. This is where TradingView parity lives.

The separate PineForge compiler, [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss), translates a PineScript v6 script into a C++ strategy that attaches the engine's Pine execution adapter; it owns translation, not execution semantics. TradingView comparisons measure this Pine path under the tested configurations. The [order model](docs/pages/fill-model.md) describes the current submodels and the remaining migration work; [Architecture](#architecture-kernel-vs-parity) states the boundary.

- **Proven, not promised.** All 4,190 probes — 312 open reference strategies plus 413 real community scripts on 15 markets and timeframes — grade *excellent* or *strong* against TradingView's own trade lists: **4,182 excellent, 8 strong, zero moderate**. The current full sweep evaluates 2,819,967 TradingView trades, with 2,818,237 matched by the verifier.
- **Open runtime.** The engine and native live runner are Apache-2.0. The separately distributed [PineForge compiler](https://github.com/pineforge-4pass/pineforge-codegen-oss/blob/main/LICENSE) uses PolyForm Noncommercial terms with additional personal-trading permission; commercial use requires a separate license. Public reference strategies, benchmarks and validation tooling are available in their respective repositories; the community-script test set is not redistributed.
- **Fast.** In-process, no interpreter: median **15× faster than PyneCore** on 196 timed strategies (a median 603k bars/s per strategy with the bar magnifier on). Parameter sweeps re-run a loaded `.so` with new inputs — no recompile, no fork.
- **Deterministic to the bit.** Two runs with the same inputs produce identical trade lists. Same on Linux and macOS.
- **Yours to embed.** 97 `extern "C"` functions across two headers — 65 to run a compiled strategy, 32 to drive the kernel yourself — append-only ABI. Call it from C, Python, Rust, Go, Node, Julia — or let an AI agent drive it over MCP.

---

## Get a backtest in 60 seconds

### With an AI agent (MCP, Docker only)

```bash
claude mcp add pineforge-backtest \
  -- docker run --rm -i -v "$PWD:/work" ghcr.io/pineforge-4pass/pineforge-backtest-mcp:latest
```

For Claude Desktop, Cursor or any MCP client:

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

Then ask: *"Fetch BTC/USDT 15m for the last 90 days and backtest this strategy"* — the container transpiles Pine → C++ with the bundled [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss), compiles, runs, and hands the agent the trade list. Nothing leaves your machine. Mount a directory at `/work`; `-i` is required and `-t` must not be added (a TTY corrupts the JSON-RPC stream).

| Ask | Tool |
|---|---|
| "Fetch BTC/USDT 15m data for the last 30 days" | `fetch_binance_ohlcv` |
| "Backtest this SMA-cross strategy on that data" | `backtest_pine` |
| "Sweep fast 8–21 × slow 21–55, rank by net PnL" | `backtest_pine_grid` |
| "What broker overrides are available?" | `list_engine_params` |

Prefer zero install? The hosted server at **[mcp.pineforge.dev/mcp](https://mcp.pineforge.dev/mcp)** (Streamable HTTP, no key) backtests against a sealed Binance spot + USDT-perp data lake, metered per IP. The npm package [`@pineforge/backtest-mcp`](https://www.npmjs.com/package/@pineforge/backtest-mcp) mirrors the same server.

[![Real backtest on Claude in 60 seconds](https://img.youtube.com/vi/lflD47Bum4w/0.jpg)](https://www.youtube.com/watch?v=lflD47Bum4w)

### From source

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git && cd pineforge-engine
python3 scripts/ci_verify.py release --build-dir build --jobs 4
bash tutorial/run.sh                            # MACD on BTC/USDT, end to end
python3 tutorial/run_stream.py                  # OHLCV warm-up → realtime trades
```

The [shared local/CI verifier](docs/ci.md) includes source guards, tests and installed-package smoke checks. Its ABI check links callers against both the current library and a separately built, pinned historical library. Preparation uses local Git history and the configured compiler; see the [ABI fixture guide](tests/fixtures/settlement_cpp_abi/README.md) for shallow clones and repeated checks. CTest itself stays offline.

Prerequisites: CMake ≥ 3.16, a C++17 compiler (GCC ≥ 9, Clang ≥ 10, Apple Clang ≥ 12), Eigen 3.3+ (fetched automatically if absent), Python 3 for the tests (`-DPINEFORGE_BUILD_TESTS=OFF` for a library-only build). `cmake --install build --prefix /usr/local` installs `lib/libpineforge.a`, `include/pineforge/`, and the `find_package(PineForge)` config.

### Embedded in your own harness

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

Every PineForge-compiled strategy `.so` exports this same ABI — write the harness once, swap strategies forever. Worked examples for [C](https://cdocs.pineforge.dev/examples_c.html), [Python sweeps](https://cdocs.pineforge.dev/examples_python_sweep.html), [Rust](https://cdocs.pineforge.dev/examples_rust.html), [multi-strategy](https://cdocs.pineforge.dev/examples_multi.html) and [magnifier A/B](https://cdocs.pineforge.dev/examples_magnifier.html) are in the docs.

Lifecycle-aware compiled modules reset Pine variables, indicator/history buffers and the broker book before each batch run or `strategy_stream_begin` warmup. Inputs and runtime settings persist until changed; ticks within a stream continue its state. Regenerate and rebuild existing modules with current codegen and matching engine headers/archive to obtain this behavior; the [internal C++ rebuild boundary](docs/pages/abi-stability.md) is checked at compile/link time.

---

## Three front doors

The engine can be driven three ways. All three run the same kernel, so they
match trigger, price fills, book lots and settle identically; what differs is
who writes the strategy and who owns TradingView's quirks.

### 1. PineScript, through codegen

Write Pine, transpile it, run the `.so`. The Pine adapter reproduces
TradingView's execution semantics on top of the kernel; this is the path the
validation scoreboard below measures.

```bash
pip install pineforge-codegen
pineforge-codegen strategy.pine -o generated.cpp
c++ -std=c++17 -shared -fPIC generated.cpp -lpineforge -o strategy.so
python3 scripts/run_strategy.py .          # or drive it over the C ABI
```

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

Thirteen more hosts under [`examples/native/`](examples/native/) cover kernel
sizing, anchored brackets on a price grid, trails in ticks, a margin model with
a real liquidation, account risk limits, calculation timing, higher-timeframe
series, an auxiliary finer feed, the open book lot by lot, and a
kernel-recorded report. Each is a CTest row: `ctest --test-dir build -R example_`.

### 3. C, against the same kernel

Hand the runtime a callback table and drive the kernel from any language with
a C FFI — no C++ in your own code. The complete file is
[`examples/native/hello_kernel_c.c`](examples/native/hello_kernel_c.c); the
32 `strategy_native_*` functions are declared in
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
strategy_configure_native_ext_v1(s, &spec, &ext);
strategy_native_run_v1(s, bars, n, &report);
```

**Coming from PineScript?** [PineScript to native C++](docs/pages/pine-to-native.md)
maps every `strategy.*` builtin, every `strategy()` declaration parameter and
every `request.*` form to its C++ **and** C spelling, names the example that
exercises each, and walks one six-feature strategy from Pine to a native host
end to end. The [native engine guide](docs/pages/native-engine.md) is the
reference underneath it.

## Native live runner

The optional C++17 `pineforge-live` executable uses this engine's native
warmup-to-stream lifecycle. It accepts normalized ticks or confirmed OHLCV bars,
supports user-defined C++ parsers for broker/provider messages, and commits
inputs plus order-action webhooks to a durable SQLite ledger. Hand-written
C++ strategies use the native contract; generated Pine strategies retain their
compatibility path. Both expose the versioned C ABI used by the runner.

Build with `-DPINEFORGE_BUILD_LIVE_RUNNER=ON`; the option is off by default,
so core-only users do not acquire SQLite/libcurl/OpenSSL dependencies. See
the [native runner guide](runner/README.md) for feed modes, symbol metadata,
parser ABI, recovery and execution limitations. The existing validation
scoreboard below describes batch backtests; it does not certify new native
live behavior or real broker fills.

## Validation scoreboard

**Native policy refactoring · 2026-09-10:** engine [#234](https://github.com/pineforge-4pass/pineforge-engine/pull/234) and codegen [#127](https://github.com/pineforge-4pass/pineforge-codegen-oss/pull/127) preserve **4,182 excellent / 8 strong**. Both the old-engine/new-codegen and new-engine/new-codegen Cloud runs retain all **4,190 raw trade CSVs, counts and full grades** unchanged. The target bands have zero entrants and zero leavers: **net 0, no individual regression**. These changes remove an unused source flag and give Pine cap behavior explicit ownership; they do not claim complete engine independence. The formal gates remain zero-improvement FAILs and the official baseline remains unchanged.

**Round 39 · 2026-09-09:** **4,182 excellent / 8 strong / zero moderate** across all **4,190 scored probes**. This round adds one excellent result, with zero regressions on any canonical metric.

| Board | Test set | Result | TradingView trades evaluated |
|---|---|---|---|
| **Public** — [open corpus](https://github.com/pineforge-4pass/pineforge-corpus) | 312 reference strategies, Apache-2.0, reproducible by anyone | **309/309 graded excellent** (ETH/USDT-perp 15m; the corpus' declared engine-only / anomaly probes are not graded) | 429,866 |
| **Closed test** — the parity campaign | 413 community-shared TradingView scripts across 15 market/timeframe lanes: **3,881 script-lane probes** — private under TradingView's Terms of Service | **3,873 excellent + 8 strong + zero moderate** = 3,881/3,881 (100%) excellent-or-strong | 2,390,101 |

**2,819,967 TradingView trades** evaluated, **2,818,237 matched by the verifier** (99.94%), from the round 39 full Cloud Run sweep. **18 TradingView-side anomalies** remain excluded under the unchanged population; each was documented before exclusion. No scored probe remains below *strong*.

Round 39 extends the existing price-scale admission check to ordinary, fee-free fractional market entries when one minimum lot is worth at least one account unit. It shares the existing financial and order-book scope with the signal-cost check. A separately queued close still fills when the opening is declined, and entries placed after a close retain their established exception.

**PK Willow Pulse UT Williams Live Movement** on **BINANCE:BTCUSDT 15m** moves from strong to excellent: canonical match rises from 96.9% to 100%, with zero trade-count gap and zero entry-price, exit-price, PnL and quantity error at the 90th percentile. Independent comparison of the unfiltered files matches all **12,408 physical trade pairs** on side, entry/exit times, prices and quantity, gaining **10,756 matches with none lost**. Including the displayed PnL in the exact comparison gains **2,193 matches with none lost**; the files use different PnL display precision.

The fix has no strategy, symbol or date lookup. TradingView controls pin both directions, flat entries, reversal/close ordering and funding boundaries. A Cloud diagnostic reproduces the original selected trade CSV across all six observed invocations and confirms that the engine passed signal-cost admission but skipped the price-scale check for a high-value fractional lot. The exact explicit-quantity reversal control exposed a separate gap that remains unchanged by this default-sizing fix. All **704 hard-surface probes** retain their canonical grades, quantity metrics and trade CSVs; **4,189 of 4,190 CSVs are unchanged**. Verifier code, grading rules, profile-selection code, reference tapes, feeds, input files and scored population are unchanged.

### The closed test, lane by lane

| Market · timeframe | Probes | Excellent | Strong | Moderate |
|---|---:|---:|---:|---:|
| BINANCE:ETHUSDT.P · 15m *(hard lane: zero regression allowed)* | 395 | 394 | 1 | — |
| BINANCE:BTCUSDT · 15m | 354 | 354 | — | — |
| BINANCE:BTCUSDT · 1D | 259 | 259 | — | — |
| CME_MINI:ES1! · 15m | 174 | 173 | 1 | — |
| CME_MINI:ES1! · 1D | 117 | 117 | — | — |
| CME_MINI:NQ1! · 15m | 174 | 174 | — | — |
| CME_MINI:NQ1! · 1D | 116 | 116 | — | — |
| NASDAQ:AAPL · 15m | 356 | 354 | 2 | — |
| NSE:NIFTY · 15m | 191 | 191 | — | — |
| NSE:NIFTY · 1D | 145 | 145 | — | — |
| NYSE:F · 15m | 340 | 338 | 2 | — |
| NYSE:F · 1D | 263 | 263 | — | — |
| OANDA:EURUSD · 15m | 373 | 372 | 1 | — |
| OANDA:XAUUSD · 15m | 376 | 375 | 1 | — |
| OANDA:XAUUSD · 1D | 248 | 248 | — | — |
| **Total** | **3,881** | **3,873** | **8** | **0** |

### How a probe is graded

Every script is exported from TradingView as-is (its own inputs, its own defaults) with the chart's trade list at full precision, transpiled with [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss), and run by this engine on the same OHLCV bars. The two trade lists are aligned trade-for-trade and graded by [`scripts/verify_corpus.py`](scripts/verify_corpus.py):

- **excellent** — the same number of trades, ≥ 99% of TradingView's trades matched, entry and exit prices within 0.01% and PnL within 1% at the 90th percentile (trailing-stop scripts are graded on the *production* profile: exits within 0.05%, since a trail fill depends on TradingView's sub-bar path);
- **strong** — ≥ 95% matched, trade count within 6%, entries within 0.1% and exits within 0.5% at p90;
- **moderate / weak** — ≥ 75% coverage, or less.

Published parity results use a fixed population and reproducible Cloud Run measurements. The formal gate requires **no hard-surface regression** and strictly positive pooled movement across the target excellent and excellent+strong bands. A documented native-correctness exception permits exactly zero target-band movement with no individual regression, after full comparison, independent review and CI; its actual FAIL remains recorded and baseline promotion is deferred. Negative movement is outside this exception. Baseline promotion requires a recorded PASS and an exact-head merge with green CI.

### What the closed test taught the engine

Every gap was closed by pinning the rule TradingView actually follows — never by loosening the grader. Each rule was isolated with sensor strategies exported from TradingView (capital sweeps, literal replays, per-bar state encoded into order comments) and landed with a replay test on the recorded bars. Among them: the broker carries money at **ten significant digits** (equity rounding, the whole-order drop band, the one-contract margin call, the raw lot floor on every lot-stepped symbol); a trailing stop restarts from the issuing bar's *close* when `trail_points` changes and never folds that bar's extreme; a zero-offset trail rides the raw running best and its arming open fills at the nearest-tick print; a reversal rejected at placement preserves standing exits and a separately queued `strategy.close`, while the distinct fill-time rejection rules govern stop, limit, and trailing legs; sparse `ta.atr`/`ta.tr` read the chart's previous close on every execution; pivot levels snap to the tick grid; early-close sessions complete their higher-timeframe bucket; and account-currency conversion is left out of the comparison entirely, because TradingView's FX series is a moving target no fixed table reproduces.

### Reproduce the public board yourself

```bash
git submodule update --init corpus
docker pull ghcr.io/pineforge-4pass/pineforge-release:latest   # optional: re-derive every generated.cpp
VERIFY=1 scripts/regen_corpus_cpp.sh                            # proves the shipped C++ is byte-identical
JOBS=8 scripts/run_corpus.sh                                    # build 312 .so, run, grade vs TradingView
python3 scripts/regen_validation_report.py                      # optional: the corpus report
```

The corpus feed is a 1-minute Binance ETH/USDT:USDT tape with the 15-minute bars derived from it (`corpus/data/derived/`). Every probe folder ships `strategy.pine`, `generated.cpp`, `tv_trades.csv` and `engine_trades.csv`. The probe once filed as a TradingView anomaly (`anomaly-equity-mirror-strategy-equity-01`) turned out to be TradingView's ten-significant-digit margin call; the rule is pinned and the probe matches trade-for-trade.

---

## Cross-engine comparison

[`benchmarks/`](benchmarks/) runs **200 strategies** through PineForge, [PyneCore](https://github.com/PyneSys/pynecore), [PineTS](https://github.com/LuxAlgo/PineTS) and [vectorbt](https://github.com/polakowo/vectorbt). Every engine gets the same 53,929-bar Binance ETH/USDT perpetual 15m feed, and each trade list is graded against TradingView's own export (266,451 trades):

- **100 corpus probes** (slots 001–100) are drawn by mechanism family from the public corpus. Their fixtures are in the public [`benchmarks/assets`](https://github.com/pineforge-4pass/pineforge-benchmarks-assets) submodule.
- **100 closed strategies** (slots 101–200) are TradingView-scraped community scripts on `BINANCE:ETHUSDT.P` 15m. Their artifacts are in the maintainers' evidence store (sha `6e938f9a…`) and are not public.
- PyneSys rejects slot 192's source, so slot 201, from the same stratum, stands in for it in the PyneCore count. PineForge runs all 201 slots.

PyneCore sources are official PyneSys cloud-compiler output, with no hand-ports. PineTS runs indicators only, because its strategy backtester is still on the upstream roadmap. vectorbt runs the 13 hand-written ports that load. `bash benchmarks/run_all.sh` reproduces the public half with no API keys.

| Group | Engine | Slots | Trades emitted | TV trades | 🟢 excellent | 🟢 strong | 🟡 moderate | 🟠 weak | 🔴 minimal | ⚪ n/a |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corpus | PineForge | 100 | 139,668 | 139,665 | **100** | 0 | 0 | 0 | 0 | 0 |
| corpus | PyneCore | 100 | 199,063 | 139,665 | 84 | 12 | 0 | 3 | 1 | 0 |
| corpus | vectorbt | 13 | 18,040 | 13,637 | 4 | 6 | 2 | 1 | 0 | — |
| closed | PineForge | 101 | 126,732 | 126,786 | **100** | 1 | 0 | 0 | 0 | 0 |
| closed | PyneCore | 101 | 172,535 | 126,786 | 49 | 30 | 8 | 9 | 1 | 4 |

**PineForge.** The only non-excellent row is closed slot 181. Every TradingView trade is matched, and PineForge emits one extra trade on the window's opening bars.

**PyneCore.** It has 64 graded non-excellent rows, and most come from the harness window and from `request.security`:

- **29 rows** fail only on PnL (15) or only on trade count (14). In all of them, entries and exits match TradingView to the tick. PyneCore's broker trades from the feed's first bar, five months before TradingView's range opens. As a result, percent-of-equity sizing compounds P&L that TradingView never had, and a position already open at the range start adds one trade at the window's leading edge. The PyneCore runner has no counterpart of PineForge's TradingView-window order gate.
- **9 multi-timeframe scripts** reproduce 0–85.5 % of TradingView's history; one has no aligned trades.
- **3 grid bots** drift on FIFO drains, and a `str.match` regex filter grades weak.
- The remaining rows fail mixed gates.

Four slots have no PyneCore trade list:

- **1 compile rejection:** PyneSys rejects slot 192 (`"Empty document."`).
- **3 runtime errors:** PyneCore raises a `RuntimeError` in its `request.security` engine on slots 140, 166 and 197. It hits the same error intermittently on a fourth slot, 143, which could not be timed.

**Speed** was measured on a quiet host ([`benchmarks/results/speed.md`](benchmarks/results/speed.md)). PineForge was re-timed at engine `063e4460`. PyneCore, vectorbt and PineTS were not re-measured; their figures come from the same host's earlier 2026-09-22 window, at engine `e9ad37dd`.

- **PineForge:** a median of 89 ms per strategy over the feed, in-process with the bar magnifier on (603k bars/s; quartiles 425k–731k).
- **PyneCore:** a median of 1,475 ms per subprocess (36.6k bars/s). The median per-strategy speedup is **15×** across the 196 strategies both engines time (p5 6×, p95 68×).
- **vectorbt:** a median of 102 ms for its 13 ports; PineForge is 1.3× faster on the same 13.
- **PineTS:** 486 ms for the canonical 10-indicator script.
- **Throughput package** ([`benchmarks/throughput/`](benchmarks/throughput/)): the magnifier-off hot loop runs at a median of **0.64 M bars/s** per strategy (N=201, median of five quiet runs).

**Not comparable with the 2026-06-11 table** (PineForge 100/100, PyneCore 85/100, 162×):

- **Tiers:** they now come from the canonical `scripts/verify_corpus.py::analyze_strategy` rubric. The old table graded a different 100-strategy population with `compare.py`'s own copy of the rubric, which had drifted from the canonical one and no longer parsed the current tape format.
- **Speed:** the ratio fell because the engine is slower per bar, not because the host changed. The 2026-06-11 engine, rebuilt and timed on the same host in the same window, still runs close to its June timings (5–16 % over them in the quieter pass), while the current engine is 10–18× slower on the probes both populations share ([provenance](benchmarks/results/speed.md#provenance)).

Last refresh **2026-09-22** (engine `063e4460`, PyneCore 6.10.2, PineTS 0.9.34, vectorbt 0.28.2, Apple M4 Max). Per-strategy table: [`benchmarks/results/summary.md`](benchmarks/results/summary.md). Population manifest: [`benchmarks/results/selection.md`](benchmarks/results/selection.md). Method, fairness and the reproduction recipe: [`benchmarks/README.md`](benchmarks/README.md).

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
   fill-price and slippage rules, POOC / calc_on_order_fills, margin revival,
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

# The fast wiring and source checks, no build:
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
| `PINEFORGE_BUILD_LIVE_RUNNER` | The `pineforge-live` executable (needs SQLite3, libcurl, OpenSSL). |
| `PINEFORGE_ENABLE_SANITIZERS` | ASan + UBSan. |
| `PINEFORGE_ENABLE_COVERAGE` | Source coverage instrumentation; see `scripts/coverage.sh`. |

The gates a pull request passes, one line each:

| Gate | Command | What it refuses |
|---|---|---|
| TradingView parity | `./scripts/check_corpus_parity.sh --subset` | A trade that moved: 30 probes re-run and hashed against `scripts/corpus_parity_baseline.txt`. The full 312-probe sweep (`--subset` dropped) runs nightly. |
| CTest row floors | `ci_verify.py release` / `kernel` | A test row that vanished: each profile counts the rows that actually ran against a floor. |
| Kernel residuals | `scripts/check_kernel_residuals.py` | A TradingView-shaped name reaching the kernel archive without an ADR 0001 row. |
| Feature rulings | `scripts/check_native_feature_rulings.py` | A `NativeRunSpec` field the adapter does not declare and the ADR does not rule. |
| C surface | `scripts/check_c_abi_runtime.py`, `scripts/check_native_c_api_surface.py` | A `PF_API` export added without its inventory row; a public host member with no C spelling and no recorded reason. |
| Twin parity | `scripts/check_twin_parity.py` | A frozen assertion quietly rewritten instead of a behaviour change being argued. |
| Documentation | `scripts/check_doc_anchors.py`, `scripts/check_doc_lint.py`, `scripts/check_pine_to_native_coverage.py` | A `file:line` citation that no longer points at its symbol; a stale epoch, roadmap label or negative claim; a Pine builtin with no row on the migration page. |

New here? [CONTRIBUTING.md](CONTRIBUTING.md) is the human walkthrough of all of
the above; [Contributing as an LLM](docs/pages/contributing-llm.md) is the same
ground written for an agent that has been handed a brief in this repository.

## What ships here

- `libpineforge.a` — the static runtime, in two layers:
  - **generic kernel** — order matching and fills, sizing, margin and settlement, the bar magnifier, 66 indicator classes, `request.security()`, time and session math;
  - **source-adapter parity runtime** — `PineStrategyHost` and `PineExecutionAdapter` (`src/source/`) plus the Pine policy helpers in `src/compat/pine/`, which map Pine/TradingView execution semantics onto the kernel.
- `<pineforge/pineforge.h>` — the public C ABI, the stability-pinned consumer surface.
- `<pineforge/*.hpp>`, `<pineforge/source/*.hpp>` — internal C++ headers the transpiler emits against (generated code derives from `pineforge::source::PineStrategyHost`); not part of the stability guarantee.
- C++ unit and recorded TradingView replay tests; CI on Linux + macOS × Release + Debug, sanitizers, and a `find_package` smoke consumer.
- `corpus/` — the 312-strategy public validation corpus (submodule).
- `benchmarks/` — the three-way comparison harness and the throughput package.
- `scripts/` — `run_corpus.sh`, `verify_corpus.py`, `run_strategy.py` (load any `.so` via ctypes), `regen_corpus_cpp.sh`, `coverage.sh`.

**This is the runtime, not the compiler.** The PineScript → C++ transpiler is [`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss) (`pip install pineforge-codegen`), bundled with the runtime in the [`pineforge-release`](https://github.com/pineforge-4pass/pineforge-release) image that the MCP server builds on. **It is a backtest engine, not a chart:** `plot`, `label`, `bgcolor` compile and do nothing. **It is not a TradingView clone:** where TradingView's behaviour is undocumented or platform-specific (the bar magnifier's intrabar path, float ordering) PineForge chooses deterministic rules and documents them; where it converges, it converges exactly.

Full coverage map — every TA class, every order primitive, every `request.security()` semantic, and what is deliberately not implemented: [`docs/coverage.md`](docs/coverage.md).

### Timezones and day boundaries

TradingView ties some day-boundary logic (intraday order caps, session rollovers) to `syminfo.timezone` and other calculations to the chart timezone. The validator derives the chart timezone from the input CSV; to force one, set `"engine_chart_timezone": "<IANA name>"` (or `""` for UTC) in the probe's `inputs.json`.

---

## Public C ABI

`<pineforge/pineforge.h>` is the single canonical consumer header. It declares
exactly 65 public `PF_API` functions: 57 runtime implementations and eight
per-strategy generated exports. Every compiled strategy `.so` exports that
public set and no internal C++ symbol (`-fvisibility=hidden`, `PF_API` on the
public set, checked in CI by `scripts/check_c_abi_runtime.py`):

| Symbol | Role |
|---|---|
| `strategy_create` / `strategy_free` | Allocate / release a strategy instance |
| `run_backtest` / `run_backtest_full` | Run with auto-detected timeframe / with timeframe + magnifier configuration |
| `report_free` | Free arrays inside a filled `pf_report_t` |
| `strategy_closed_trade_entry_incarnation` | Per-run physical entry provenance of a closed trade |
| `strategy_set_input` / `strategy_set_override` | Override a Pine `input.*()` value / a `strategy(...)` declaration parameter |
| `strategy_set_magnifier_volume_weighted` | Toggle the volume-weighted magnifier |
| `strategy_set_trace_enabled` | Toggle per-bar trace recording |
| `strategy_set_trade_start_time` | Suppress historical order placement before a time |
| `strategy_stream_begin` / `_push_tick` / `_push_ticks` / `_advance_time` / `_end` / `_fill_report` | Warm on OHLCV, then run realtime on ordered trades |
| `strategy_stream_api_version` / `_push_bar` / `_order_actions_len` / `_order_action_get` / `_order_actions_clear` / `_state_hash` | Native live extension v1: confirmed input bars, physical fill events and observable replay state |
| `strategy_set_chart_timezone` / `strategy_set_syminfo_timezone` / `strategy_set_syminfo_session` | Chart and exchange time |
| `strategy_set_syminfo_mintick` / `_pointvalue` / `_metadata` / `_type` / `_string` | Symbol tick size, point value, numeric metadata, instrument class, string members |
| `strategy_set_native_security_feed` / `strategy_set_aux_security_feed` | Feed `request.security()` from a native higher-timeframe series / an auxiliary bar-aligned feed |
| `strategy_set_account_currency_fx_series` | Effective-time quote-to-account FX |
| `strategy_get_last_error` | The latest runtime error |
| `pf_version_get` / `pf_version_string` / `pf_abi_version` | Runtime version, version string, struct-layout version (`PF_ABI_VERSION == 4`) |
| `strategy_execution_contract` / `strategy_configure_native_v1` / `strategy_configure_native_fx_curve_v1` | Query Legacy vs NativeMarketV1; apply the versioned native run specification; stage or clear an immutable native FX curve |
| `strategy_request_abort` / `strategy_last_run_status` | Cooperative abort of a run in progress; `0`=completed, `1`=aborted |
| `strategy_set_realtime_tail` | Live-runtime surface (ABI v4): the array's last bar is a still-forming tail — `barstate.islast=false`, `last_bar_index`/`last_bar_time` frozen at the horizon bar, no range-end row |
| `strategy_set_probe_suppress_tail_logic` | ABI v4: the last bar runs only the broker's pre-`on_bar` steps (pending-order settlement, intraday-cap/loss checks) and returns — no `on_bar`, no margin-call / POOC second pass / bracket-reissue processing (the range-end row is `strategy_set_realtime_tail`'s to skip; the flags are independent) |
| `strategy_set_path_order` / `strategy_last_bar_dual_entry_path` | ABI v4: force the intrabar O→H/L→C leg order (`AUTO`/`HIGH_FIRST`/`LOW_FIRST`) for path-dependent fill probing; read which side won a same-bar dual-entry-stop arbitration |
| `strategy_set_broker_state_hash_recording` / `strategy_broker_state_hash` | ABI v4: toggle a 64-bit broker-state hash appended per script bar to `pf_report_t::broker_state_hash`; read the final state's hash |
| `strategy_pending_orders_len` / `strategy_pending_order_get` / `strategy_pending_order_layout` | ABI v4: the resting pending-order book after the most recent run — count, a POD snapshot per order (`pf_pending_order_v1_t`), and the snapshot's self-describing field layout |
| `strategy_pending_order_fill_qty` / `_level_resolved` / `_effective_levels` / `strategy_trail_best_price` | ABI v4: engine-computed values for a resting order — the quantity it would open if filled at a given price, whether its relative offsets resolve yet, its resolved stop/limit/trail-activation levels, and the live position's trail extreme |
| `strategy_position_avg_price` / `strategy_position_cycle_seq` / `strategy_position_size` | ABI v4: the live position's volume-weighted average entry price, its cycle id, and its script-facing signed size |
| `strategy_closed_trade_entry_id` / `_exit_id` / `_exit_comment` / `_close_cause` | ABI v4: per-closed-trade id/comment strings and a `close_cause` enum (`SCRIPT`/`BRACKET`/`MARGIN_CALL`/`INTRADAY_LOSS_CAP`/`INTRADAY_FILL_CAP`/`RANGE_END`), indexed like `strategy_closed_trade_entry_incarnation` |
| `strategy_current_equity` / `strategy_script_bars_processed` | ABI v4: `initial_capital + netprofit` (not Pine's `strategy.equity`, which also adds open profit); total script bars dispatched by the most recent run |

### Driving the kernel from C

`<pineforge/native_c_api.h>` (included by `pineforge.h`) adds **34 further
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
| `strategy_native_position_v1` / `_working_len_v1` / `_working_get_v1` | The physical position, and a copy-out snapshot of the live working book (`pf_native_working_v1`: its appended `trail_has_arm_price` tells a trail with no arm price from one armed at 0.0, and a caller sending `PF_NATIVE_WORKING_V1_BASE_SIZE` is filled exactly that far) |
| `strategy_native_open_lot_count_v1` / `_open_lot_get_v1` | The physical book lot by lot (`pf_native_open_lot_v1`: identity, entry facts, signed units, entry fee, fee-net P&L and excursions at a mark) — `strategy.opentrades.*` for a C host |
| `strategy_native_events_v1` / `_state_v1` | Poll the recorded event history by ordinal; read the lifecycle and its typed failure |
| `strategy_native_partial_bar_v1` / `_series_bar_v1` / `_trail_state_v1` / `_liquidation_price_v1` | The four optional reads — the bar so far at the cursor, a declared higher-timeframe series' latest bucket, a live trail's projection, the solved liquidation level. Each answers `PF_NATIVE_ABSENT` where the C++ `std::optional` is empty |
| `strategy_native_risk_state_v1` / `_marked_equity_v1` / `_recalculations_v1` / `_continuation_hash_v1` | The generic risk ledger, marked equity at a mark, the driven/suppressed recalculation counters, and the run's continuation identity |
| `strategy_native_cohort_open_v1` / `_add_v1` / `_remove_v1` | Cohort rosters: a cohort close is `PF_NATIVE_INTENT_HOST_SIZED` owned by `PF_NATIVE_OWNER_BIND_COHORT`, sized by the `on_close_units` hook |
| `strategy_native_declare_subscriptions_v1` | Declare the run's higher-timeframe series from inside `on_run_begin`, replacing the staged list |
| `strategy_configure_native_ext_v1` | Configure from `pf_native_run_spec_v1` **plus** `pf_native_run_spec_ext_v1` (report policy, price grid, calculation timing, open-bar view, margin model, higher-timeframe subscriptions, generic risk limits, the auxiliary finer feed, the retained intrabar path, and the slot-label / feed-tolerance / path-order / abort-reporting policies). The two specs' nine enum-valued words stay `uint32_t` and each has a C enumeration: `pf_native_fee_kind_e`, `pf_native_close_execution_e`, `pf_native_open_directions_e`, `pf_native_report_policy_e`, `pf_native_price_grid_e`, `pf_native_grid_rounding_e`, `pf_native_calc_trigger_e`, `pf_native_open_bar_view_e`, `pf_native_liquidation_sizing_e` |
| `strategy_native_append_auxiliary_bars_v1` | Append a realtime stream's later bars to the run's declared auxiliary finer feed |
| `strategy_native_declare_opened_lot_entry_bar_mask_v1` | From inside `on_applied`, say where the fill that opened a lot sat on its entry bar (`pf_native_opened_lot_fill_point_e`: on the bar's path, or after it); the kernel derives the lot's entry-bar mask that `on_lot_excursion`'s facts carry back |
| `strategy_native_api_version` | This surface's layout version (`PF_NATIVE_API_VERSION`) |

The header's **COVERAGE** block lists every public member of
`NativeStrategyHost` with either its C spelling or the reason it has none, and
`scripts/check_native_c_api_surface.py` proves that list is exactly that
class's public surface — a member added without a row, a row naming a member
that no longer exists, or a spelling naming a symbol the C headers do not
declare all fail CI.

Every struct is tagged and size-prefixed (`struct_size`, `version`); an unknown
size, version or enumerator is refused with a documented negative status and
mutates nothing. `pf_native_run_spec_ext_v1` has three published lengths — the
layout the lane first shipped (`PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE`), the same
struct with L9's appended risk tail (`PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE`) and
the current one with the auxiliary-feed tail behind it; `pf_native_callbacks_v1`
has two — the layout the lane first shipped (`PF_NATIVE_CALLBACKS_V1_BASE_SIZE`)
and the same struct with its appended tail. The runtime accepts each, so a host
compiled against an earlier one keeps working unchanged. An **observation**
callback that returns non-zero latches
`NativeFailureCode::CallbackException` and ends the run `Failed`; the four
**answering** hooks in the table's tail instead return a `pf_native_answer_e`
choosing whose answer the kernel uses, and can never fail the run. Streaming
needs no new symbol: the `strategy_stream_*` family takes these handles
unchanged. Worked example: [`examples/native/hello_kernel_c.c`](examples/native/hello_kernel_c.c);
reference: [`docs/pages/native-engine.md`](docs/pages/native-engine.md).

POD types `pf_bar_t`, `pf_trade_tick_t`, `pf_trade_t`, `pf_report_t`, `pf_security_diag_t`, `pf_trace_entry_t`, `pf_version_t`, `pf_trade_stats_t`, `pf_equity_stats_t`, `pf_metrics_t`, `pf_equity_point_t`, `pf_pending_order_v1_t`, `pf_field_desc_t` and the `pf_magnifier_distribution_t` enum complete the surface. ABI v2 added computed trading metrics and a per-bar equity curve; ABI v3 added `pf_trade_t::open_at_end`, TradingView's range-end close of a position still open after the last bar; ABI v4 added the live-runtime accessors above plus `pf_report_t::broker_state_hash` / `broker_state_hash_len` (a per-script-bar broker-state hash array, appended after `equity_curve_len`, NULL/0-length unless `strategy_set_broker_state_hash_recording` is on) and the `pf_pending_order_v1_t` generated POD mirror of the engine's resting-order record. Check `pf_abi_version()` before running: the report struct is caller-allocated.

Full flag semantics, string lifetimes and the three L0 evidence lanes behind the ABI v4 live surface: [`docs/pages/live-surface.md`](docs/pages/live-surface.md).

**Stability guarantee.** Within a major version, struct layouts and `extern "C"` signatures are append-only — fields and functions are added, never reordered, removed or retyped; `static_assert`s in `src/c_abi.cpp` pin the layouts. Semantic versioning at the ABI level: PATCH never touches the ABI, MINOR appends, MAJOR breaks. A `.so` built against `0.X.Y` keeps working on any later `0.X.Z`.

---

## Repository layout

```
include/pineforge/      public C ABI (pineforge.h) + internal C++ headers
  ├── source/                         Pine source-adapter headers (pine_adapter.hpp, pine_strategy_host.hpp, …)
  └── compat/pine/                    Pine policy helper headers
src/                    48 .cpp files in two layers
  │ generic kernel (Pine-agnostic)
  ├── c_abi.cpp                       C ABI implementations + layout asserts
  ├── engine_*.cpp                    BacktestEngine: run loop, orders, execution, path resolution,
  │                                   lower-TF emulation, security + aux security, stream, consumer,
  │                                   metrics, report, trade accessors, state hash
  ├── native_*.cpp                    native orders, run spec, calendar, FX curve, execution consumer
  ├── market_admission / market_driver / pending_order_mirror / reservation_expansion
  ├── ta_*.cpp                        66 indicator classes (moving averages, oscillators,
  │                                   volatility/trend, extremes/volume, misc)
  ├── magnifier / matrix / session_time / timeframe / timezone / math / str_utils
  │ source-adapter parity runtime (Pine / TradingView semantics)
  ├── source/
  │   ├── pine_strategy_host.cpp      PineStrategyHost: the base every GeneratedStrategy derives from
  │   ├── pine_adapter.cpp            PineExecutionAdapter: Pine order lifecycle, brackets, fills, margin
  │   ├── pine_strategy_commands.cpp  strategy.entry / order / exit / close / cancel lowering
  │   ├── pine_scheduler.cpp, pine_scheduler_native.cpp
  │   └── pine_aux_security.cpp, pine_state_hash.cpp
  └── compat/pine/                    exit_activation, exit_lifecycle, market_admission,
                                      order_birth, order_priority, reservation_expansion
tests/                  C++ unit, TradingView replay and pure-C ABI tests
examples/native/        Pine-free native hosts, C++ and C, each a CTest row
corpus/                 public submodule: 312 strategies + the 1-minute feed and derived 15m bars
benchmarks/             three-way comparison harness, throughput package, results/
scripts/                ci_verify.py, ci_preflight.py, check_corpus_parity.sh, run_corpus.sh,
                        verify_corpus.py, run_strategy.py, and the check_*.py source guards
tutorial/               MACD end-to-end + streaming walkthrough
docs/                   coverage map, Pine v6 audit, Doxygen site (cdocs.pineforge.dev)
  ├── pages/                          the narrative pages, incl. pine-to-native.md
  ├── design/                         the native feature-parity inventory and rulings
  └── adr/                            0001, the kernel/adapter boundary
cmake/                  PineForgeConfig.cmake.in + the find_package smoke consumer
```

Documentation: [C ABI reference](https://cdocs.pineforge.dev) · [Getting started](https://cdocs.pineforge.dev/getting_started.html) · [MACD tutorial](https://cdocs.pineforge.dev/tutorial_macd.html) · [Streaming](https://cdocs.pineforge.dev/streaming.html) · [Metrics reference](https://cdocs.pineforge.dev/metrics.html) · [FFI from Python](https://cdocs.pineforge.dev/ffi_python.html) · [Rust](https://cdocs.pineforge.dev/examples_rust.html) · [CMake integration](https://cdocs.pineforge.dev/integration_cmake.html) · [ABI stability](https://cdocs.pineforge.dev/abi_stability.html) · [Coverage](https://cdocs.pineforge.dev/coverage.html). The site rebuilds on every push to `main`.

---

## Releases

- **Unreleased** (branch `live/abi-v4`) — ABI v4 live surface for `pineforge-live`: 24 new default-off exports (cooperative abort, realtime tail, probe-suppress tail logic, forced path order, a per-bar broker-state hash, the pending-order book as a generated POD mirror, closed-trade id/comment/close-cause, position and equity accessors). No flag changes a historical run: `scripts/live_flags_off_identity.py` (312 corpus probes, 0 differ vs the pre-v4 branch point), `scripts/live_flags_lane.py` (312 probes, 0 positives, 130 open-at-end trades subtracted), and `scripts/bar_identity_lane.py` (row 1: 222,295 bars compared, 2 explained open divergences, 0 else) all pass. 56 symbols.
- **v0.13.0** (2026-09-05) — the parity campaign, rounds 1–11: TradingView's broker rules pinned with sensor exports and landed with replay tests — ten-significant-digit money, trailing-stop restarts, zero-offset trails, declined-reversal bracket legs, the surviving `strategy.close`, sparse `ta.atr`/`ta.tr`, pivot tick snap, same-bar entry/close transactions, early-close higher-timeframe buckets, 64-bit epoch arrays. Closed test 3,880/3,881; corpus 309/309. ABI v3, 32 symbols, 198 tests.
- **v0.7 – v0.12** (June–August 2026) — native and auxiliary `request.security()` feeds, ABI v2 metrics + equity curve, streaming mode, range-end accounting. See [GitHub releases](https://github.com/pineforge-4pass/pineforge-engine/releases).
- **v0.6.0** — performance sprint: cached static inputs, thread-local timestamp caching, lazy timezone caching; up to 6.7M bars/s.
- **v0.5.0** — Pine v6 compatibility sprint (symbol mappings, constant namespaces, timestamp overloads, collection sorting, bare TA property reads); corpus 234 probes.
- **v0.4.1** — clean-room 228-probe corpus, submodule made public, five engine fixes.
- **v0.1 – v0.3** — initial release with the pinned C ABI; same-id stop/replace resolution, RMA seed, `-ffp-contract=off`; magnifier gap fills and directional mintick rounding.

---

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) (includes the Apache-2.0 contribution grant), or
[Contributing as an LLM](docs/pages/contributing-llm.md) if you are an agent working from a brief.
The short version: TradingView parity for new work goes in the adapter or in codegen, never in the
kernel; every change keeps the parity corpus byte-identical; anything exported from
`<pineforge/pineforge.h>` or `<pineforge/native_c_api.h>` is append-only within a major version.
Bug reports with a Pine script, an OHLCV slice and TradingView's trade list are the most valuable
thing you can send — that is exactly how every rule above was found.

## License

Apache License 2.0 — [LICENSE](LICENSE). Third-party notices: [NOTICE](NOTICE). Licensing notes (optional AGPL benchmark deps, trademarks): [LEGAL.md](LEGAL.md). [Code of conduct](CODE_OF_CONDUCT.md) · [Security policy](SECURITY.md).
