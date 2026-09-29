# Engine benchmarks

This directory compares **PineForge** with two open-source PineScript runtimes and one vectorized backtester:

- [**PyneCore**](https://github.com/PyneSys/pynecore): a Python framework that runs `@pyne` Python translated from Pine source by the [PyneSys cloud compiler](https://pynesys.io/). Apache 2.0.
- [**PineTS**](https://github.com/LuxAlgo/PineTS): a TypeScript transpiler and runtime that runs raw `.pine` source in Node.js or browsers. AGPL-3.0. It has shipped a `strategy.*` namespace since 0.9.17, but this harness runs only its indicator layer, through the canonical indicator script.
- [**vectorbt**](https://github.com/polakowo/vectorbt): a vectorized Pandas/NumPy/Numba backtester. It runs the hand-written `strategy_vbt.py` ports that some slots ship.

## Headline

Last refresh **2026-09-29**. Versions: engine `main` `35db01c8` running the committed `generated.cpp` (codegen `121b3e6a`), PyneCore 6.10.3, PineTS 0.9.34 and vectorbt 0.28.2, on an AWS c7a.8xlarge (AMD EPYC 9R14, 32 cores, SMT off) running Ubuntu 24.04.

**The host changed.** The 2026-09-22 table was timed on an Apple M4 Max, this one on an AWS c7a.8xlarge: times and bars/s are not comparable between the hosts, and only ratios measured on one host, in one window, are compared below.

**Population: 200 strategies in 201 slots**, selected by [`select_population.py`](select_population.py) with seed 20260921. The manifest is [`results/selection.md`](results/selection.md).

- **100 corpus probes** (slots `001`–`100`) are public. They come from `corpus/validation/` at gitlink `442d497`: at least one per each of 19 mechanism families, drawn by TradingView trade-count bins.
- **100 closed strategies** (slots `101`–`200`) are TradingView-scraped community scripts on `BINANCE:ETHUSDT.P` 15m, the only market and timeframe that both the bench feed and the TradingView tapes cover. Their artifacts are in the maintainers' private evidence store (sha256 `c77a9c70891b5e4e97405fe6673389c06652a0cc449b380a4d982f1709e62019`) and are not public; the committed results name each by its slot number only.
- **Slot `201` stands in for slot `192` in the PyneCore count.** PyneSys rejects `192`'s source with `"Empty document."`, so the next strategy from the same bin became slot `201`. PineForge runs all 201 slots.

Each engine's trade list is graded against TradingView's own export (266,451 trades) by the canonical corpus rubric, [`scripts/verify_corpus.py`](../scripts/verify_corpus.py)'s `analyze_strategy`:

| Group | Engine | Slots graded | Trades emitted | TV trades | 🟢 excellent | 🟢 strong | 🟡 moderate | 🟠 weak | 🔴 minimal | ⚪ n/a |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corpus | PineForge | 100 | 139,668 | 139,665 | **100** | 0 | 0 | 0 | 0 | 0 |
| corpus | PyneCore | 100 | 199,554 | 139,665 | 86 | 12 | 1 | 1 | 0 | 0 |
| corpus | vectorbt | 13 | 18,040 | 13,637 | 4 | 6 | 2 | 1 | 0 | — |
| closed | PineForge | 101 | 126,731 | 126,786 | **101** | 0 | 0 | 0 | 0 | 0 |
| closed | PyneCore | 101 | 175,031 | 126,786 | 51 | 34 | 11 | 3 | 1 | 1 |
| **all** | PineForge | 201 | 266,399 | 266,451 | **201** | 0 | 0 | 0 | 0 | 0 |
| **all** | PyneCore | 201 | 374,585 | 266,451 | 137 | 46 | 12 | 4 | 1 | 1 |

Counting the 200 strategies (slot `201` in place of `192`), PineForge grades all 200 excellent. PyneCore grades 137 excellent, 46 strong, 12 moderate, 4 weak and 1 minimal. The tapes predate a change TradingView made to `strategy()` defaults, and a slot whose script omits one runs with the value its tape ran with (see [Fairness](#fairness)).

Every engine was timed in the same window on this host, at engine `35db01c8`, with the quiet-host gate re-checked before every timing batch and every timing process pinned to fixed cores; the loads are in [`results/speed.md`](results/speed.md).

| Engine | How it is timed | Strategies | Median per strategy | Bars/s: Q1 · **median** · Q3 |
|---|---|---:|---:|---|
| PineForge | in-process Google Benchmark, bar magnifier on | 201 | 66.8 ms | 505k · **807k** · 1133k |
| PyneCore | subprocess wall time (interpreter start, import, backtest), 8 concurrent | 200 | 2,629 ms | 13.1k · **20.5k** · 32.2k |
| vectorbt | in-process, the 13 ports that load | 13 | 209.3 ms | — |
| PineTS | subprocess wall time of the canonical 10-indicator script | 1 | 1416.2 ms | — |

- PineForge's median speedup is **36× over PyneCore**, per strategy across the 200 strategies both engines time (p5 15×, p95 131×), and 4.2× over vectorbt across its 13 ports.
- The throughput package ([`throughput/`](throughput/)) measures the magnifier-off hot loop at a median of **0.78 M bars/s** per strategy. That figure is over all 201 slots, and is the median of five quiet runs.
- The PineForge sweep ran at a 1-minute load of 4.68 (slots 001–100) and 1.32 (slots 101–201). Against engine `063e4460`, timed alternately with `35db01c8` in one window on 201 slots, `35db01c8` takes 0.35× the time per strategy at the median, each engine running its own `generated.cpp`.
- On the same host and in the same window, PyneCore 6.10.3 takes 1.01× the time of 6.10.2 per strategy at the median, over 26 public slots timed alternately.

**Against the 2026-09-22 table** (engine `063e4460`: PineForge 200 excellent and 1 strong; PyneCore 6.10.2: 133 excellent; 15× over PyneCore at a median 603k bars/s per PineForge strategy):

- **Tiers:** PineForge's strong row, closed slot `181`, grades excellent. PyneCore 6.10.3 grades 12 slots higher than 6.10.2 did and none lower; among them are `140`, `166` and `197`, which raised a `RuntimeError` on 6.10.2 and now run.
- **Speed:** that table was timed on the other host, so its speedup is not set against this one's. On this host, the same-window comparisons above measure what changed: the engine's time per strategy, and PyneCore's with its release.

**These numbers are not comparable with the 2026-06-11 table** (PineForge 100/100 excellent, PyneCore 85/100, 162×):

- **Tiers:** that table graded a different 100-strategy population with `compare.py`'s own copy of the rubric. The copy had drifted from the canonical rubric and no longer parsed the current tape format. `compare.py` now calls the canonical rubric directly, and PyneCore moved from 6.4.6 to 6.10.3 in between.
- **Speed:** it was timed on an Apple M4 Max. The 2026-06-11 engine, rebuilt on this host and timed in the same window, runs the three probes both populations share 4–8× faster than `35db01c8` with the magnifier on; see [the provenance](results/speed.md#provenance).

Every number in this section traces to a committed file or a pinned commit. The raw timing files are in [`results/raw/`](results/raw/), and [`check_provenance.py`](check_provenance.py) derives each number from its source and fails on any number without one.

### Where the non-excellent rows come from

**PineForge.** Every slot grades excellent. Closed slot `181`, strong in the 2026-09-22 table with one extra long on the window's opening bars, lost that trade with the regenerated `generated.cpp`. Codegen `58e5f3e` converts every double-valued expression that enters an integer slot through the na-preserving cast: the script converted an `na` double to `int`, which C++ leaves undefined, and the old build booked one trade more than TradingView.

**PyneCore.** It has 63 graded non-excellent rows. The per-row failing gates are in [`results/summary.md`](results/summary.md).

- **32 rows** fail only on PnL (16) or only on the trade count (16). Their entries match TradingView's exactly and their exits within 0.0002 % (entry and exit p90). The difference is the window: PyneCore's broker trades from the feed's first bar, 2024-10-19, five months before TradingView's range opens.
  - With percent-of-equity sizing, PyneCore compounds P&L that TradingView never had. Slot `002`: quantity 536.418 against TradingView's 547.6178 on identical fills.
  - A position PyneCore already holds when the range opens adds a trade at the window's leading edge. In 11 of the 16 count-only rows, a position PyneCore opened before the range is still open when TradingView's first trade opens: 7 hold it past that bar and 4 close it on that bar.
  - The PyneCore runner has no counterpart of PineForge's TradingView-window order gate.
- **Multi-timeframe scripts.** PyneCore 6.10.2 reproduced 0–85.5 % of TradingView's history through `request.security` on nine of them. With 6.10.3's `request.security` fixes, corpus `037` and `040` and closed `163` and `164` grade excellent, and closed `181` is one of the count-only rows above. Corpus `042` and closed `114` and `189` grade moderate, and closed `169` still reproduces 41.6 % (weak).
- **3 grid bots** (`102`, `103`, `107`) drift on FIFO drains and grade moderate.
- Slot `086` (a `str.match` regex filter) grades weak, and slot `135` has no aligned trades.
- The other 22 rows fail other gates, one or several.

**One PyneCore slot has no trade list:** slot `192`, whose source PyneSys rejects. The file carries `//@version=6` followed by two `//@version=5` lines. Slots `140`, `166` and `197` raised a `RuntimeError` in 6.10.2's `request.security` engine at the feed's first, partial day; on 6.10.3 they run.

Slot `143`, whose `request.security` failed intermittently on 6.10.2, completed all 20 timed runs on 6.10.3.

## Reproduce

**Public half: no API keys, no data downloads.** Every input the public 100 slots need is committed to the [`benchmarks/assets`](https://github.com/pineforge-4pass/pineforge-benchmarks-assets) submodule: OHLCV, `.pine` sources, `generated.cpp`, `tv_trades.csv`, `strategy_pyne.py` and `strategy_vbt.py`. Prerequisites: CMake ≥ 3.20, a C++17 compiler, [uv](https://docs.astral.sh/uv/) with CPython 3.12, Node ≥ 20, and network access: the build fetches Google Benchmark, and `uv sync` and `npm install` fetch the Python and Node packages.

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

**Maintainers: the full 201 slots.** Fetch the closed root from the maintainers' private evidence store (`lab evidence get`, a maintainer-only tool, with the campaign environment sourced). Its directory is gitignored, and every harness step picks it up when it exists:

```bash
lab evidence get c77a9c70891b5e4e97405fe6673389c06652a0cc449b380a4d982f1709e62019 --out /tmp/bench-closed.tar.gz
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
(cd benchmarks && uv run python speed/time_pynecore.py --n 20 --workers 8 \
    --first 178,162,164,163,165,132,114,171,181,168,189,105 --out _workdir/pc_speed.json)
#   (--first starts the longest slots first, so the pool stays 8 wide; one batch took an hour on the AWS host)
(cd benchmarks && N=20 node speed/time_pinets.mjs > _workdir/pt_speed.json)
(cd benchmarks && uv run python speed/time_vectorbt.py --out _workdir/vbt_speed.json)
(cd benchmarks && uv run python speed/aggregate.py --pineforge _workdir/pf_speed.json \
    --pynecore _workdir/pc_speed.json --pinets _workdir/pt_speed.json \
    --vectorbt _workdir/vbt_speed.json --loads _workdir/speed_loads.tsv \
    --provenance <notes.md>)                                                 # -> results/speed.md

# throughput package: five quiet runs, keep the median run's JSON and chart
bash benchmarks/throughput/reproduce.sh
```

PyneSys is not needed to reproduce: the committed `strategy_pyne.py` files are the compiler's output. Refreshing them takes the maintainer's PyneSys key, which the API limits to 120 requests per clock hour and 300 a day; the 204 requests that compiled them on 2026-09-21 are in [`results/pynesys-compile-log.md`](results/pynesys-compile-log.md). The 2026-09-30 refresh sent none. Adding slots, refreshing the OHLCV and re-emitting `generated.cpp` through codegen are done by the maintainer-only bench-maintenance scripts.

**`run_all.sh` knobs:**

- `SKIP_BUILD`, `SKIP_PINEFORGE`, `SKIP_PYNE`, `SKIP_PINETS`, `SKIP_VECTORBT`, `SKIP_INDICATORS`, `SKIP_SPEED` and `SKIP_REPORTS` each skip one step.
- `JOBS` runs parallel parity jobs. It never affects timing.
- `SLOTS` narrows the loops to slot ranges, for example `1-50,120`.
- `QUIET_LOAD_MAX` holds each timing batch until the host is quiet. Each batch's load is recorded in `_workdir/speed_loads.tsv` and in `speed.md`.

Every engine run removes the slot's previous trade list first, so a failed run leaves an `_<engine>_error.log` and no trade list, never an earlier run's. A PineForge failure on any slot (a run error, or a `generated.cpp` without its built strategy library) stops `run_all.sh` with exit status 1 before any report is written. PyneCore and vectorbt failures are results: the reports grade those slots n/a with the error.

The harness checks itself without a build or the assets: `python3 benchmarks/check_provenance.py` traces every headline number to its committed source (in a full clone: it reads the 2026-06-11 and 2026-09-22 tables from commits `933fe583` and `35db01c8`), and `python3 -m unittest discover -s benchmarks/tests` runs the harness tests.

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
| [`raw/`](results/raw/) | The raw timing files behind the speed figures, with a sha256 [`manifest.json`](results/raw/manifest.json); committed by hand, not regenerated | all four |

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
│   └── raw/                         the timing files behind the published speed figures + manifest.json
├── tests/                           the harness's own tests (python3 -m unittest discover -s benchmarks/tests)
├── check_provenance.py              every headline number -> its committed source
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

A slot where an engine produced no trade list, or whose run left an `_<engine>_error.log`, reads `n/a`, with the compile or runtime error that explains why.

### Indicator-value comparison (three-way)

A single canonical script ([`assets/strategies/_indicators/canonical.pine`](https://github.com/pineforge-4pass/pineforge-benchmarks-assets/blob/6aedbcf5c263ec49239dc55b91f39046d470aef1/strategies/_indicators/canonical.pine)) computes 10 common indicators over the full 53,929-bar feed: `ta.ema`, `ta.sma`, `ta.rsi`, `ta.atr`, the three `ta.macd` outputs and the three `ta.bb` outputs. Each engine emits one CSV with per-bar values, and `compare_indicators.py` reports p50, p90, p99 and max relative deltas for every indicator pair.

### Speed measurement

- **PineForge** is timed with Google Benchmark's in-process hot loop, with the bar magnifier on (1→4 ENDPOINTS sub-bar sampling), which is the engine's most expensive configuration. The strategy library is `dlopen`ed once, outside the timed region. Each timed iteration runs `strategy_create`, applies the slot's `inputs.json` `strategy_overrides` (the settings the graded run uses), and runs `run_backtest_full` over the whole feed; the figure is the per-iteration mean over 20 iterations, and each benchmark also reports its run's trade count.
- **PyneCore** is timed as the subprocess wall time of `uv run python runners/run_pynecore.py <slot> --no-write`, which includes Python startup and framework import. The figures are the median and p95 over 20 invocations, with 8 slots timed concurrently.
- **vectorbt** is timed in-process: the median over 20 iterations of each port.
- **PineTS** is timed as the subprocess wall time of `node runners/run_pinets_canonical.mjs`. The harness has no PineTS strategy runner, so the canonical indicator script stands in for its indicator-layer cost.
- **The quiet-host gate:** before every timing batch, the 1-minute load average must be below 6 with no `cmake --build`, `ctest` or `ci_verify` process running, counted by executable name. `speed.md` lists the load at every batch.
- **Pinning:** on the timing host every single-threaded timer runs pinned to one core and PyneCore's eight concurrent subprocesses to eight others (`taskset`), and the gate also requires those cores idle. `speed.md` names the cores.
- **Same-host comparisons:** a change of engine or of a PyneCore release is measured by timing both sides on the same host in one window, alternating new, old, old, new; times from different hosts are never set against each other.

The methodologies are mixed on purpose. In-process timing is the realistic cost for an FFI-callable native engine or a library. Subprocess timing is the realistic cost for engines whose API entry point is the process. Each number is what a real consumer of that engine would see.

## Fairness

**What is held equal.** Every engine consumes the same 53,929-bar Binance ETH/USDT-USDT perpetual 15m OHLCV feed at [`assets/data/ETHUSDT_15.csv`](https://github.com/pineforge-4pass/pineforge-benchmarks-assets/blob/6aedbcf5c263ec49239dc55b91f39046d470aef1/data/ETHUSDT_15.csv). The PyneCore Python is the official cloud-compiler output for the same `.pine` sources PineForge runs, with no hand-translation. Commission, slippage, default quantity and the bar magnifier come from the `strategy(...)` declaration in the `.pine` source, plus the slot's `inputs.json` runtime overrides.

**The tapes' `strategy()` defaults.** TradingView changed three Pine v6 `strategy()` defaults on 2026-09-24: `initial_capital` 1,000,000 → 100,000, `default_qty_type` fixed → percent of equity, and `default_qty_value` 1 → 100. Every tape here was recorded before that, and the codegen declares the new values when a script omits them. The 64 slots whose script omits one of the three (2 public, 62 closed) pin the value the tape ran with in `inputs.json`'s `strategy_overrides`, as the corpus does. PyneCore's own defaults are the old values, so both engines run each tape's settings. Without the pins, the regenerated closed half grades 45 excellent and 42 weak: capital ten times smaller, and percent-of-equity orders where the script meant one contract.

**What is not.** PineForge applies TradingView's trading window. Its runner, `scripts/run_strategy.py`, warms indicators on the pre-window bars but holds strategy orders until the tape's range opens; four closed slots whose TradingView exports carry earlier positions opt out through `run_strategy.args`. PyneCore's runner has no such gate and trades from the feed's first bar, which accounts for almost half of PyneCore's non-excellent rows (see above). A trimmed feed would buy window parity at the cost of indicator warm-up.

This harness does not run PineTS strategies, although PineTS has shipped a `strategy.*` namespace since 0.9.17. On indicator outputs all three engines must agree within tight tolerances, and a divergence in either direction is flagged as a defect.

## License

The benchmark code has the same license as the parent repository (Apache 2.0). Four pieces deserve explicit notes.

### `assets/data/ETHUSDT_15.csv`

Binance USDT-M futures ETH/USDT-USDT 15-minute OHLCV: 53,929 bars from 2024-10-19 21:00 to 2026-05-04 15:00 UTC, which covers the TradingView chart range plus about five months of warm-up. It is public market data, not copyrightable in the US or EU. The file is pinned for reproducibility; a maintainer-only script refreshes it.

### `assets/strategies/<NNN-slug>/strategy.pine`

The 100 public slots are probes of the public PineForge corpus: clean-room PineForge originals under the corpus's Apache-2.0 license. The 100 closed scripts are third-party TradingView publications. They are used as a factual reference and are not redistributed.

### `assets/strategies/<NNN-slug>/strategy_pyne.py`

These are mechanically translated derivatives of the corresponding `strategy.pine`, produced by the [PyneSys cloud compiler](https://pynesys.io/) (`pyne compile`, PyneComp v6.0.68). They are committed so the benchmark reproduces without an API key; anyone reproducing it uses the committed output. The PyneSys compiler is a tool (like `gcc`), and its output does not transfer copyright to the vendor. These files inherit the underlying `strategy.pine` license (Apache-2.0).

### PineTS (AGPL-3.0)

Running PineTS at benchmark time pulls AGPL-3.0 code into Node's process. That is permissible for *running* the benchmark, but redistributing the whole toolchain as a single binary would trigger copyleft. We publish only numerical results (CSVs and markdown tables), not PineTS source.

Full licensing context: [`../LEGAL.md`](../LEGAL.md).
