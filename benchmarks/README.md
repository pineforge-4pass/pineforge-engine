# Engine benchmarks

This directory compares **PineForge** with two open-source PineScript runtimes and one vectorized backtester:

- [**PyneCore**](https://github.com/PyneSys/pynecore): a Python framework that runs `@pyne` Python translated from Pine source by the [PyneSys cloud compiler](https://pynesys.io/). Apache 2.0.
- [**PineTS**](https://github.com/LuxAlgo/PineTS): a TypeScript transpiler and runtime that runs raw `.pine` source in Node.js or browsers. AGPL-3.0. It has no strategy backtester upstream, so it runs indicators only.
- [**vectorbt**](https://github.com/polakowo/vectorbt): a vectorized Pandas/NumPy/Numba backtester. It runs the hand-written `strategy_vbt.py` ports that some slots ship.

## Headline

Last refresh **2026-09-22**. Versions: engine `main` `063e4460` running the committed `generated.cpp` (codegen `89645d6`), PyneCore 6.10.2, PineTS 0.9.34 and vectorbt 0.28.2, on an Apple M4 Max.

**Population: 200 strategies in 201 slots**, selected by [`select_population.py`](select_population.py) with seed 20260921. The manifest is [`results/selection.md`](results/selection.md).

- **100 corpus probes** (slots `001`–`100`) are public. They come from `corpus/validation/` at gitlink `442d497`: at least one per each of 19 mechanism families, drawn by TradingView trade-count bins.
- **100 closed strategies** (slots `101`–`200`) are TradingView-scraped community scripts on `BINANCE:ETHUSDT.P` 15m, the only market and timeframe that both the bench feed and the TradingView tapes cover. Their artifacts are in the maintainers' evidence store (sha256 `6e938f9a9160eeae6bda5c7c2d02078f1b9795d767a48d125fa68245f69ca539`) and are not public.
- **Slot `201` stands in for slot `192` in the PyneCore count.** PyneSys rejects `192`'s source with `"Empty document."`, so the next strategy from the same bin became slot `201`. PineForge runs all 201 slots.

Each engine's trade list is graded against TradingView's own export (266,451 trades) by the canonical corpus rubric, [`scripts/verify_corpus.py`](../scripts/verify_corpus.py)'s `analyze_strategy`:

| Group | Engine | Slots graded | Trades emitted | TV trades | 🟢 excellent | 🟢 strong | 🟡 moderate | 🟠 weak | 🔴 minimal | ⚪ n/a |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corpus | PineForge | 100 | 139,668 | 139,665 | **100** | 0 | 0 | 0 | 0 | 0 |
| corpus | PyneCore | 100 | 199,063 | 139,665 | 84 | 12 | 0 | 3 | 1 | 0 |
| corpus | vectorbt | 13 | 18,040 | 13,637 | 4 | 6 | 2 | 1 | 0 | — |
| closed | PineForge | 101 | 126,732 | 126,786 | **100** | 1 | 0 | 0 | 0 | 0 |
| closed | PyneCore | 101 | 172,535 | 126,786 | 49 | 30 | 8 | 9 | 1 | 4 |
| **all** | PineForge | 201 | 266,400 | 266,451 | **200** | 1 | 0 | 0 | 0 | 0 |
| **all** | PyneCore | 201 | 371,598 | 266,451 | 133 | 42 | 8 | 12 | 2 | 4 |

Counting the 200 strategies (slot `201` in place of `192`), PineForge grades 199 excellent and 1 strong. PyneCore grades 133 excellent, 42 strong, 8 moderate, 12 weak and 2 minimal, and 3 slots have no trade list.

Speed was measured on a quiet host, re-checked before every timing batch; the details are in [`results/speed.md`](results/speed.md). PineForge was re-timed at engine `063e4460`. PyneCore, vectorbt and PineTS were not re-measured: their rows come from the same host's earlier 2026-09-22 window, at engine `e9ad37dd`.

| Engine | How it is timed | Strategies | Median per strategy | Bars/s: Q1 · **median** · Q3 |
|---|---|---:|---:|---|
| PineForge | in-process Google Benchmark, bar magnifier on | 201 | 89.4 ms | 425k · **603k** · 731k |
| PyneCore | subprocess wall time (interpreter start, import, backtest), 8 concurrent | 196 | 1,475 ms | 23.3k · **36.6k** · 58.5k |
| vectorbt | in-process, the 13 ports that load | 13 | 102.3 ms | — |
| PineTS | subprocess wall time of the canonical 10-indicator script | 1 | 485.8 ms | — |

- PineForge's median speedup is **15× over PyneCore**, per strategy across the 196 strategies both engines time (p5 6×, p95 68×), and 1.3× over vectorbt across its 13 ports.
- The throughput package ([`throughput/`](throughput/)) measures the magnifier-off hot loop at a median of **0.61 M bars/s** per strategy. That figure is over all 201 slots, and is the median of five quiet runs at engine `e9ad37dd`. It was not re-timed at `063e4460` because the host was never quiet.
- Per strategy, the `063e4460` sweep takes 1.02× the `e9ad37dd` sweep's time at the median (p5 0.95×, p95 1.07×), at a higher host load (5.41–5.90 against 3.79).

**These numbers are not comparable with the 2026-06-11 table** (PineForge 100/100 excellent, PyneCore 85/100, 162×):

- **Tiers:** that table graded a different 100-strategy population with `compare.py`'s own copy of the rubric. The copy had drifted from the canonical rubric and no longer parsed the current tape format. `compare.py` now calls the canonical rubric directly, and PyneCore moved from 6.4.6 to 6.10.2 in between.
- **Speed:** the ratio fell because the engine is slower per bar, not because the host changed. The 2026-06-11 engine, rebuilt on this host in the `e9ad37dd` window, reproduced its June timings. On the three probes both populations share, engine `e9ad37dd` was 12–20× slower with the magnifier on. That A/B was measured at `e9ad37dd` and not re-run; `063e4460` times the same probes within 7 % of `e9ad37dd`. See [the provenance](results/speed.md#provenance).

### Where the non-excellent rows come from

**PineForge.** The one strong row is closed slot `181`. Every TradingView trade is matched, but PineForge also emits one extra trade: a long on the window's opening bars (count Δ 1 of 2,411).

**PyneCore.** It has 64 graded non-excellent rows. The per-row failing gates are in [`results/summary.md`](results/summary.md).

- **29 rows** fail only on PnL (15) or only on the trade count (14). In all of them, entries and exits match TradingView to the tick. The difference is the window: PyneCore's broker trades from the feed's first bar, 2024-10-19, five months before TradingView's range opens.
  - With percent-of-equity sizing, PyneCore compounds P&L that TradingView never had. Slot `002`: quantity 536.418 against TradingView's 547.6178 on identical fills.
  - A position PyneCore already holds when the range opens adds one trade at the window's leading edge.
  - The PyneCore runner has no counterpart of PineForge's TradingView-window order gate.
- **9 multi-timeframe scripts** reproduce only 7–85 % of TradingView's history through `request.security`: corpus `037`, `040` and `042`; closed `114`, `163`, `164`, `169`, `181` and `189`.
- **3 grid bots** (`102`, `103`, `107`) drift on FIFO drains and grade moderate.
- Slot `086` (a `str.match` regex filter) grades weak, and slot `135` has no aligned trades.
- The remaining rows fail mixed gates.

**Four PyneCore slots have no trade list:**

- Slot `192`: PyneSys rejects the source. The file carries `//@version=6` followed by two `//@version=5` lines.
- Slots `140`, `166` and `197`: PyneCore raises a `RuntimeError` in its `request.security` engine at the feed's first, partial day.

Slot `143` hits the same error intermittently. Its grade comes from the one run that completed, and it could not be timed.

## Reproduce

**Public half: no API keys, no downloads.** Every input the public 100 slots need is committed to the [`benchmarks/assets`](https://github.com/pineforge-4pass/pineforge-benchmarks-assets) submodule: OHLCV, `.pine` sources, `generated.cpp`, `tv_trades.csv`, `strategy_pyne.py` and `strategy_vbt.py`. Prerequisites: CMake ≥ 3.20, a C++17 compiler, [uv](https://docs.astral.sh/uv/) with CPython 3.12, and Node ≥ 20.

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git
cd pineforge-engine
git submodule update --init benchmarks/assets
(cd benchmarks && uv sync --python 3.12 && npm install)

# runtime, one strategy dylib per slot, Google Benchmark harness
cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON -DPINEFORGE_BUILD_SPEED_BENCH=ON
cmake --build build --target pineforge bench_strategies pineforge_bench -j

# parity for every engine, then the reports (add the speed sweep by dropping SKIP_SPEED=1)
SKIP_BUILD=1 SKIP_SPEED=1 JOBS=8 bash benchmarks/run_all.sh
cat benchmarks/results/summary.md
```

**Maintainers: the full 201 slots.** Fetch the closed root from the evidence store (`lab evidence get` with the campaign environment sourced). Its directory is gitignored, and every harness step picks it up when it exists:

```bash
lab evidence get 6e938f9a9160eeae6bda5c7c2d02078f1b9795d767a48d125fa68245f69ca539 --out /tmp/bench-closed.tar.gz
tar -xzf /tmp/bench-closed.tar.gz -C benchmarks        # -> benchmarks/assets-closed/ (101 slots)
cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON -DPINEFORGE_BUILD_SPEED_BENCH=ON
cmake --build build --target pineforge bench_strategies pineforge_bench -j 12

# PineForge parity (+ canonical indicator, + vectorbt trades), chunked by slot under a 10-minute cap
SKIP_BUILD=1 SKIP_PYNE=1 SKIP_PINETS=1 SKIP_SPEED=1 SKIP_REPORTS=1 JOBS=8 SLOTS=1-100 bash benchmarks/run_all.sh
SKIP_BUILD=1 SKIP_PYNE=1 SKIP_PINETS=1 SKIP_SPEED=1 SKIP_REPORTS=1 SKIP_VECTORBT=1 SKIP_INDICATORS=1 JOBS=8 SLOTS=101-201 bash benchmarks/run_all.sh
python3 benchmarks/compare.py && python3 benchmarks/compare_indicators.py   # results/{summary,trade_comparison,indicator_comparison}.md

# speed on a quiet host (1-min load < 6, no cmake --build / ctest / ci_verify), all engines in one command
QUIET_LOAD_MAX=6 SKIP_BUILD=1 SKIP_PINEFORGE=1 SKIP_PYNE=1 SKIP_PINETS=1 SKIP_REPORTS=1 bash benchmarks/run_all.sh
#   or in chunks, re-checking the gate before each; the closed PyneCore slots run 1-72 s per backtest:
./build/bin/pineforge_bench --benchmark_filter='/throughput/with_magnifier' --benchmark_format=json > benchmarks/_workdir/pf_speed.json
#   (or per slot range, e.g. --benchmark_filter='^(0[0-9][0-9]|100)-[^/]*/throughput/with_magnifier', then merge the "benchmarks" arrays)
(cd benchmarks && uv run python speed/time_pynecore.py --n 20 --workers 8 --slots 1-56 --out _workdir/pc_speed_c01.json)
#   ... one chunk per slot range up to 201, then merge them into _workdir/pc_speed.json
(cd benchmarks && N=20 node speed/time_pinets.mjs > _workdir/pt_speed.json)
(cd benchmarks && uv run python speed/time_vectorbt.py --out _workdir/vbt_speed.json)
(cd benchmarks && uv run python speed/aggregate.py --pineforge _workdir/pf_speed.json \
    --pynecore _workdir/pc_speed.json --pinets _workdir/pt_speed.json \
    --vectorbt _workdir/vbt_speed.json --loads _workdir/speed_loads.tsv \
    --provenance <notes.md>)                                                 # -> results/speed.md

# throughput package: five quiet runs, keep the median run's JSON and chart
bash benchmarks/throughput/reproduce.sh
```

PyneSys is not needed to reproduce: the committed `strategy_pyne.py` files are the compiler's output. Refreshing them takes the maintainer's PyneSys key, capped at 100 requests an hour; this refresh's 204 requests are in [`results/pynesys-compile-log.md`](results/pynesys-compile-log.md). Adding slots, refreshing the OHLCV and re-emitting `generated.cpp` through codegen are done by the maintainer-only bench-maintenance scripts.

**`run_all.sh` knobs:**

- `SKIP_BUILD`, `SKIP_PINEFORGE`, `SKIP_PYNE`, `SKIP_PINETS`, `SKIP_VECTORBT`, `SKIP_INDICATORS`, `SKIP_SPEED` and `SKIP_REPORTS` each skip one step.
- `JOBS` runs parallel parity jobs. It never affects timing.
- `SLOTS` narrows the loops to slot ranges, for example `1-50,120`.
- `QUIET_LOAD_MAX` holds each timing batch until the host is quiet. Each batch's load is recorded in `_workdir/speed_loads.tsv` and in `speed.md`.

## What gets reproduced

The harness writes these reports to [`results/`](results/):

| Report | What it holds | Engines |
|---|---|---|
| [`summary.md`](results/summary.md) | Per-strategy tier, the tallies per group, and every non-excellent row with its failing gates | PineForge, PyneCore, vectorbt vs TV |
| [`trade_comparison.md`](results/trade_comparison.md) | Per-strategy metrics: emitted, in-window and matched trades; coverage; count Δ; entry, exit and PnL p90 | PineForge, PyneCore, vectorbt vs TV |
| [`indicator_comparison.md`](results/indicator_comparison.md) | Per-bar values of 10 indicators over the 53,929-bar feed | PineForge ↔ PyneCore ↔ PineTS |
| [`speed.md`](results/speed.md) | Per-strategy wall time and bars/s, the host load at every timing batch, and the provenance | all four |
| [`selection.md`](results/selection.md), [`selection.json`](results/selection.json) | The population manifest: slots, strata, bins, input shas and the replacement | — |
| [`pynesys-compile-log.md`](results/pynesys-compile-log.md) | Every PyneSys request of the refresh | PyneCore |

Each public slot, `benchmarks/assets/strategies/<NNN-slug>/`, holds:

- `strategy.pine`: the corpus probe's PineScript source (Apache-2.0).
- `generated.cpp`: the codegen output, compiled to `strategy.dylib` by CMake.
- `strategy_pyne.py`: the PyneSys cloud-compiler output.
- `tv_trades.csv`: TradingView's trade list, the ground truth.
- `pineforge_trades.csv`, `pynecore_trades.csv` and `vectorbt_trades.csv`: the engines' outputs, regenerated by `run_all.sh`.
- `inputs.json` (36 slots): the probe's runtime overrides and parity profile.
- `strategy_vbt.py` (14 slots): a vectorbt port.

## Layout

```
benchmarks/
├── assets/                          public git submodule: data/ETHUSDT_15.csv + strategies/001-…100-…/ + _indicators/
├── assets-closed/                   maintainers only (gitignored): the 101 closed slots, from the evidence store
├── runners/
│   ├── run_pineforge_canonical.cpp  PineForge canonical indicator runner (links libpineforge)
│   ├── run_pinets_canonical.mjs     PineTS canonical indicator runner (Node)
│   └── run_pynecore.py              PyneCore strategy runner (wraps `pyne run` + TV-schema normalize)
├── speed/
│   ├── pineforge_bench.cpp          Google Benchmark harness for PineForge (in-process; both slot roots)
│   ├── time_pynecore.py             subprocess wall-time timer for PyneCore (--slots/--out for chunks)
│   ├── time_pinets.mjs              subprocess wall-time timer for PineTS canonical
│   ├── time_vectorbt.py             in-process timer (and trade writer) for the vectorbt ports
│   ├── aggregate.py                 combine the timing JSONs + host loads -> results/speed.md
│   └── CMakeLists.txt               GBench fetch + build config
├── throughput/                      throughput reproduction package (magnifier-off hot loop, grid search)
├── results/                         refreshed reports (committed)
├── select_population.py             the seeded population draw -> results/selection.{json,md}
├── compare.py                       trade-list grader (canonical rubric; every engine vs TV)
├── compare_indicators.py            per-bar indicator comparator
├── paths.py                         path constants (public root + the closed root when present)
├── pyproject.toml + uv.lock         Python deps
├── package.json                     Node deps
└── run_all.sh                       single-command reproducer
```

## Methodology

### Trade-list comparison (every engine against TradingView)

For each slot:

1. **PineForge:** CMake builds `strategy.dylib` from the committed `generated.cpp`. `scripts/run_strategy.py` loads it through ctypes, runs the feed with the slot's `inputs.json` and emits `pineforge_trades.csv` in TradingView's schema.
2. **PyneCore:** `pyne run` executes the committed `strategy_pyne.py` against the same feed. Same-symbol daily, weekly and monthly `request.security` contexts are served from the chart feed. `runners/run_pynecore.py` normalizes the output to TradingView's schema as `pynecore_trades.csv`.
3. **vectorbt:** `speed/time_vectorbt.py --write-trades` runs every port that loads and writes `vectorbt_trades.csv`.
4. **Ground truth:** `tv_trades.csv` is exported from TradingView's broker emulator running the same `.pine` source on the same market and timeframe.

`compare.py` does not re-implement the grader: it calls [`scripts/verify_corpus.py`](../scripts/verify_corpus.py)'s `analyze_strategy`, the rubric the corpus sweep uses, on each engine's CSV. The rubric:

- aligns trades, then trims both lists to their common match window;
- consolidates fragments and pairs range-end marks;
- requires the exact trade count and at least 99 % coverage for *excellent*;
- applies the strict or production threshold profile;
- honours the slot's `inputs.json` overrides.

A slot where an engine produced no trade list reads `n/a`, with the compile or runtime error that explains why.

### Indicator-value comparison (three-way)

A single canonical script ([`assets/strategies/_indicators/canonical.pine`](assets/strategies/_indicators/canonical.pine)) computes 10 common indicators over the full 53,929-bar feed: `ta.ema`, `ta.sma`, `ta.rsi`, `ta.atr`, the three `ta.macd` outputs and the three `ta.bb` outputs. Each engine emits one CSV with per-bar values, and `compare_indicators.py` reports p50, p90, p99 and max relative deltas for every indicator pair.

### Speed measurement

- **PineForge** is timed with Google Benchmark's in-process hot loop, with the bar magnifier on (1→4 ENDPOINTS sub-bar sampling), which is the engine's most expensive configuration. `strategy.dylib` is `dlopen`ed once, outside the timed region. Each timed iteration runs `strategy_create` plus `run_backtest_full` over the whole feed, and the figure is the per-iteration mean over 20 iterations.
- **PyneCore** is timed as the subprocess wall time of `uv run python runners/run_pynecore.py <slot> --no-write`, which includes Python startup and framework import. The figures are the median and p95 over 20 invocations, with 8 slots timed concurrently.
- **vectorbt** is timed in-process: the median over 20 iterations of each port.
- **PineTS** is timed as the subprocess wall time of `node runners/run_pinets_canonical.mjs`. It has no strategy backtester upstream, so the canonical indicator script stands in for its indicator-layer cost.
- **The quiet-host gate:** before every timing batch, the 1-minute load average must be below 6 with no `cmake --build`, `ctest` or `ci_verify` process running, counted by executable name. `speed.md` lists the load at every batch.

The methodologies are mixed on purpose. In-process timing is the realistic cost for an FFI-callable native engine or a library. Subprocess timing is the realistic cost for engines whose API entry point is the process. Each number is what a real consumer of that engine would see.

## Fairness

**What is held equal.** Every engine consumes the same 53,929-bar Binance ETH/USDT-USDT perpetual 15m OHLCV feed at [`assets/data/ETHUSDT_15.csv`](assets/data/ETHUSDT_15.csv). The PyneCore Python is the official cloud-compiler output for the same `.pine` sources PineForge runs, with no hand-translation. Commission, slippage, default quantity and the bar magnifier come from the `strategy(...)` declaration in the `.pine` source, plus the slot's `inputs.json` runtime overrides.

**What is not.** PineForge applies TradingView's trading window. Its runner, `scripts/run_strategy.py`, warms indicators on the pre-window bars but holds strategy orders until the tape's range opens; four closed slots whose TradingView exports carry earlier positions opt out through `run_strategy.args`. PyneCore's runner has no such gate and trades from the feed's first bar, which accounts for almost half of PyneCore's non-excellent rows (see above). A trimmed feed would buy window parity at the cost of indicator warm-up.

PineTS implements no strategy backtesting yet, which is a matter of timing, not architecture. On indicator outputs all three engines must agree within tight tolerances, and a divergence in either direction is flagged as a defect.

## License

The benchmark code has the same license as the parent repository (Apache 2.0). Four pieces deserve explicit notes.

### `assets/data/ETHUSDT_15.csv`

Binance USDT-M futures ETH/USDT-USDT 15-minute OHLCV: 53,929 bars from 2024-10-19 21:00 to 2026-05-04 15:00 UTC, which covers the TradingView chart range plus about five months of warm-up. It is public market data, not copyrightable in the US or EU. The file is pinned for reproducibility; `pineforge-utils/bench-maintenance/fetch_extended_ohlcv.py` refreshes it (maintainer-only).

### `assets/strategies/<NNN-slug>/strategy.pine`

The 100 public slots are probes of the public PineForge corpus: clean-room PineForge originals carrying Apache-2.0 SPDX headers. The 100 closed scripts are third-party TradingView publications. They are used as a factual reference and are not redistributed.

### `assets/strategies/<NNN-slug>/strategy_pyne.py`

These are mechanically translated derivatives of the corresponding `strategy.pine`, produced by the [PyneSys cloud compiler](https://pynesys.io/) (`pyne compile`, PyneComp v6.0.68). They are committed so the benchmark reproduces without an API key; anyone reproducing it uses the committed output. The PyneSys compiler is a tool (like `gcc`), and its output does not transfer copyright to the vendor. These files inherit the underlying `strategy.pine` license (Apache-2.0).

### PineTS (AGPL-3.0)

Running PineTS at benchmark time pulls AGPL-3.0 code into Node's process. That is permissible for *running* the benchmark, but redistributing the whole toolchain as a single binary would trigger copyleft. We publish only numerical results (CSVs and markdown tables), not PineTS source.

Full licensing context: [`../LEGAL.md`](../LEGAL.md).
