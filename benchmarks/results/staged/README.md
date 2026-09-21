# Staged benchmark results — the PyneCore half (lane BENCH1)

The owner, 2026-09-21 22:00 Taipei: *"stage result from pynecore firstly, and run
benchmark for pineforge after the followup are done."* This directory holds the
PyneCore (and PineTS) half of the refresh on the new 200-strategy population. The
PineForge half — parity regeneration, the speed sweep, the throughput package and the
numbers in `README.md` / `benchmarks/README.md` / `results/{summary,speed,trade_comparison,indicator_comparison}.md`
— runs in lane **BENCH2**, after the engine follow-up wave D (lanes E1–E5) is merged.
Every PineForge cell here reads ⏳ *pending wave D (BENCH2)*; nothing in `results/`
outside this directory was changed.

## The population

`selection.md` / `selection.json` (`benchmarks/select_population.py`, seed **20260921**):

- **100 corpus probes** (public): drawn from the 310 eligible probes of `corpus/validation/`
  (corpus gitlink `442d497`), one slot or more per each of 19 mechanism families, then by
  TV trade-count quantile bins. Slots `001`–`100`.
- **100 closed scripts** (private): TradingView-scraped strategies from campaign population
  `3d72f815` with `source == "scrapper"`, `BINANCE:ETHUSDT.P`, timeframe `15` — the only
  dataset the bench feed (Binance ETH/USDT-USDT perp 15m, 53,929 bars) and the TV tapes agree
  on. 379 eligible (anomaly surface and the 16 scripts the campaign runs on the 1m feed
  excluded), all `hard` surface, drawn by trade-count quantile bins. Slots `101`–`200`.
- **1 replacement:** slot `192` is kept but PyneSys rejects its source (`"Empty document."`;
  the file carries `//@version=6` followed by two `//@version=5` lines), so the next member
  of its bin became slot **`201`** (same `hard` stratum, bin 21). 200 slots with a PyneCore
  source remain.

## Where the artifacts are

| Half | Location |
|---|---|
| Public 100 (`001`–`100`) | `benchmarks/assets` submodule, gitlink **`4794c69f4ebe63c84d2e6878873b30db8b3468d2`** = `pineforge-4pass/pineforge-benchmarks-assets` `refs/heads/r5/BENCH1` (pushed): `strategy.pine`, `tv_trades.csv`, `inputs.json` (36), `generated.cpp`, `strategy_pyne.py`, `pynecore_trades.csv`, `strategy_vbt.py` (14) |
| Closed 101 (`101`–`201`) | not public: `benchmarks/assets-closed/` (gitignored), published as one tarball to the maintainers' evidence store — **sha256 `6e938f9a9160eeae6bda5c7c2d02078f1b9795d767a48d125fa68245f69ca539`** (14,696,398 bytes; `lab evidence get <sha> --out <file> && tar -xzf <file> -C benchmarks`). Per slot: `strategy.pine`, `tv_trades.csv`, `metrics.json`, `inputs.json` (the campaign's winning verifier run: TV tz, lane syminfo, `ohlcv_start_ms` / `syminfo_metadata` flags), `run_strategy.args` (4 slots), `generated.cpp`, `strategy_pyne.py` (100), `pynecore_trades.csv` (97), `_pynecore_error.log` (4), `PROVENANCE.json`; plus `_selection/` (population document, verify-report export, the slot builder) and the closed `CMakeLists.txt`. Provenance: TradingView scraped, not redistributed. |

## Files here

| File | What |
|---|---|
| `selection.md`, `selection.json` | the manifest: 201 rows (slot, source, probe id / corpus path, TV trades and CSV rows, family / surface, bin, license), stratification counts, seed, input shas, the replacement |
| `pynesys-compile-log.md` | every PyneSys request (timestamp, slot, outcome) — counts below |
| `pynecore_summary.md` | per-strategy PyneCore tier vs TV (201 rows), tallies, every non-excellent row with its failing gates |
| `pynecore_trade_comparison.md` | per-strategy metrics: emitted / in-window / matched trades, coverage, count Δ, entry/exit/PnL p90 |
| `indicator_comparison_pynecore_pinets.md` | canonical 10-indicator script, PyneCore ↔ PineTS per-bar deltas over the 53,929-bar feed |
| `pynecore_speed.md`, `quiet_host_polls.tsv` | PyneCore timing: not measured (the host was never quiet) and every quiet-host probe — see *Speed* |

## PyneSys compiles (the owner's key: 100 requests per hour)

`pineforge-utils/bench-maintenance/cloud_compile_ratelimited.py` wraps `cloud_compile.py`'s
`pyne compile … --force` (one compile = one HTTP request): at most 90 requests in any rolling
hour, sequential with a 5 s gap, HTTP 429 → wait then retry once, an auth error stops.

| Batch | Requests | ok | rejected | HTTP 429 | Window (UTC, 2026-09-21) |
|---|---:|---:|---:|---:|---|
| 0 (usage probe) | 1 | 1 | 0 | 0 | 13:36:47 |
| 1 | 89 | 89 | 0 | 0 | 13:36:57–13:44:37 |
| 2 | 90 | 90 | 0 | 0 | 14:36:50–14:44:39 |
| 3 | 24 | 22 | 2 (slot 192, sent twice to capture the error text) | 0 | 15:36:51–15:40:15 |
| **total** | **204** | **202** | **2** | **0** | peak 90 in any rolling hour |

The service reported a 300/day and 120/clock-hour limit at 13:36Z; compiler PyneComp v6.0.68.
199 selected slots + replacement 201 + the canonical indicator compiled.

## PyneCore vs TradingView (PyneCore 6.10.2, canonical rubric)

Graded by `compare.py --engines PyneCore` with `scripts/verify_corpus.py::analyze_strategy`
(exact count and ≥ 99 % coverage required for *excellent*).

| Scope | Strategies | excellent | strong | moderate | weak | minimal | n/a | PyneCore trades emitted | TV trades |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| corpus | 100 | 84 | 12 | 0 | 3 | 1 | 0 | 199,063 | 139,665 |
| closed (101 slots) | 101 | 49 | 30 | 8 | 9 | 1 | 4 | 172,535 | 126,786 |
| **all slots** | **201** | **133** | **42** | **8** | **12** | **2** | **4** | **371,598** | **266,451** |
| the 200 counted (192 → 201) | 200 | 133 | 42 | 8 | 12 | 2 | 3 | | |

PyneCore emits more trades than TV because it runs the whole feed (from 2024-10-19 21:00 UTC);
TV's deep backtests start at 2025-04-01 (or the probe's chart origin). The four `n/a`: slot 192
(PyneSys compile rejection) and slots 140, 166, 197 (PyneCore `RuntimeError` in its
`request.security` engine: "the developing batch published no round … the child's chart bar
stream diverged", at the feed's first, partial daily period).

Mechanisms of the 64 graded non-excellent rows (failing gates per row in `pynecore_summary.md`):

- **Pre-window broker and equity carry (pnl-only 15, count-only 14):** entries and exits match
  TV to the tick, but PyneCore's broker is live from the feed's first bar. Percent-of-equity
  sizing compounds five months of P&L TV never had (slot 002: qty 536.418 vs TV 547.6178, PnL
  −14,236.53 vs −14,533.78 on identical fills), and a position PyneCore already holds when TV's
  range opens adds one trade at the window's leading edge (slot 014: long 2025-03-31 16:00 →
  16:15). The PyneCore runner has no counterpart of PineForge's TV-window order gate.
- **`request.security` semantics (weak/minimal on 037, 040, 042 and closed 114, 163, 164, 169,
  181, 189):** multi-timeframe scripts reproduce only part of TV's history (coverage 7–85 %).
- **Grid / pyramiding bots (102, 103, 107 moderate):** count, exit and PnL drift on FIFO drains.
- **Other:** 086 (`str.match` regex filter) weak; 135 no aligned trades.

PineTS 0.9.34 reproduces the committed `canonical_pinets.csv` byte-for-byte; PyneCore ↔ PineTS
agree to 3.45e-07 relative (max over the 10 indicators).

## Speed

**Not measured: the host was never quiet.** The PyneCore timing (`speed/time_pynecore.py`,
N=20, median/p95, 8 concurrent) may only start when the 1-minute load average is below 6.0 and
no `cmake --build` / `ctest` / `ci_verify` process runs. The gate was polled every ~5 minutes
for four hours, 2026-09-21 15:49:00Z → 19:49:13Z: 100 probes, none passed — 1-minute load
min 7.74 (17:49:26Z, with 8 build/test processes), median 97.0, max 233.2; 5–16 build/test
processes at every probe (other lanes' `ci_verify` runs). `pynecore_speed.md` states this with
the table; `quiet_host_polls.tsv` is every probe. No PyneCore timing JSON is staged; BENCH2
times PyneCore with PineForge (step 6 below).

## BENCH2: the PineForge half, from cold

Run from the BENCH2 worktree (this lane's commits on the post-wave-D `main`):

```bash
# 0. Assets: public submodule + the closed root (maintainers only)
git submodule update --init benchmarks/assets        # gitlink 4794c69 (assets r5/BENCH1)
source ~/code/pineforge-workflow/campaign/env.sh
lab evidence get 6e938f9a9160eeae6bda5c7c2d02078f1b9795d767a48d125fa68245f69ca539 --out /tmp/bench1-closed.tar.gz
tar -xzf /tmp/bench1-closed.tar.gz -C benchmarks     # -> benchmarks/assets-closed/ (101 slots)

# 1. generated.cpp: re-emit ONLY if wave D moved codegen (else the committed files stand)
cd ~/code/pineforge-utils/bench-maintenance
PINEFORGE_CODEGEN_PATH=<codegen checkout> PYTHONPATH=<engine>/benchmarks python3 _emit_generated_cpp.py
#    closed root: same command with PYTHONPATH=<dir holding a paths.py whose
#    STRATEGIES = <engine>/benchmarks/assets-closed/strategies>
cd -

# 2. Deps (CPython 3.12, PyneCore 6.10.2, PineTS 0.9.34, vectorbt 0.28.2)
(cd benchmarks && uv sync --python 3.12 && npm install)

# 3. Build: runtime, 201 strategy dylibs (both roots), GBench harness
cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON -DPINEFORGE_BUILD_SPEED_BENCH=ON
cmake --build build --target pineforge bench_strategies pineforge_bench -j 12

# 4. PineForge parity (+ canonical indicator, + vectorbt trades); chunk with SLOTS=1-100 / 101-201 under a 10-min cap
SKIP_BUILD=1 SKIP_PYNE=1 SKIP_PINETS=1 SKIP_SPEED=1 SKIP_REPORTS=1 JOBS=8 bash benchmarks/run_all.sh

# 5. Reports, all engines (the PyneCore outputs are already in the slots)
python3 benchmarks/compare.py               # -> results/summary.md, results/trade_comparison.md
python3 benchmarks/compare_indicators.py    # -> results/indicator_comparison.md

# 6. Speed on a quiet host (1-min load < 6, no cmake --build / ctest / ci_verify -- probe as in
#    pynecore_speed.md), all engines. One command (runs past a 10-min tool cap; quiet_gate
#    waits and records the load before every batch):
QUIET_LOAD_MAX=6 SKIP_BUILD=1 SKIP_PINEFORGE=1 SKIP_PYNE=1 SKIP_PINETS=1 SKIP_REPORTS=1 bash benchmarks/run_all.sh
#    or in chunks, re-checking the gate before each (PyneCore ~25 corpus / ~10 closed slots per chunk):
./build/bin/pineforge_bench --benchmark_filter='/throughput/with_magnifier' --benchmark_format=json > benchmarks/_workdir/pf_speed.json
(cd benchmarks && uv run python speed/time_pynecore.py --n 20 --workers 8 --slots 1-25 --out _workdir/pc_speed_c01.json)
#    ... one chunk per slot range up to 201, then merge:
python3 -c "import glob,json; d={}; [d.update(json.load(open(f))) for f in sorted(glob.glob('benchmarks/_workdir/pc_speed_c*.json'))]; json.dump(d, open('benchmarks/_workdir/pc_speed.json','w'), indent=2)"
(cd benchmarks && N=20 node speed/time_pinets.mjs > _workdir/pt_speed.json)
(cd benchmarks && uv run python speed/time_vectorbt.py --out _workdir/vbt_speed.json)
(cd benchmarks && uv run python speed/aggregate.py --pineforge _workdir/pf_speed.json \
    --pynecore _workdir/pc_speed.json --pinets _workdir/pt_speed.json \
    --vectorbt _workdir/vbt_speed.json --loads _workdir/speed_loads.tsv)      # -> results/speed.md

# 7. Throughput package (median of 5 quiet runs; then README table, 'Last measured', plot title N)
bash benchmarks/throughput/reproduce.sh
#    grid search: the retired 19-scalping-wunder-bots is gone; the closest public analogue is
#    021-composite-scalping-integration-01 (inputs "Fast EMA", "Slow EMA", "Take profit (ticks)"
#    against a fixed "Stop loss (ticks)") -- point grid_search_repro.py at it.

# 8. Then README.md (Speed badge, cross-engine table, PyneCore paragraph, Last refresh line)
#    and benchmarks/README.md (headline, Layout with assets-closed/).
```

Observation for BENCH2 (PineForge, not staged): a pre-wave-D PineForge pass on this host
(macOS arm64, AppleClang; engine `main` `8e099884`, codegen `89645d6`) graded the corpus 100
excellent and the closed 99 excellent + 1 strong. The strong row, slot 181, emits one extra
trade (long 2025-03-31 03:30 → 03:45 UTC, the first bar after its chart-EMA na-warmup) that the
campaign's Linux runner on the same trees did not (its exp-r5-p2c run: count Δ 0). It
reproduces locally with the verifier's own flags on the corpus feed and is deterministic across
five runs — a platform-dependent evaluation to re-check after wave D.
