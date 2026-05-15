# Benchmark Refresh + Speed Table + Corpus Expansion

**Date:** 2026-05-15
**Engine version at refresh:** v0.4.1
**Last refresh:** 2025-05-05 (commit `d760761`)
**Stale window:** ~12 months, 148 engine commits, 2 minor releases.

## Motivation

Marketing claims surfaced in `README.md` and the `benchmarks/` reports were
generated against engine v0.3.x in May 2025. Since then:

- 5 fix commits with trade-list-visible behavior change: magnifier wrong-side
  exits + directional mintick rounding, RMA Pine-reference formula, OCA
  cancel-sweep predicate, FIFO PnL per-leg snap, deferred-flip carry, security
  LTF rejection, deterministic chart-TZ.
- 1 feature commit changing time semantics: `strategy_set_chart_timezone()` ABI
  + codegen TZ-aware emission for `hour()` / `dayofweek()`.
- 1 magnifier feature: real input bars as waypoints when `input < script TF`.
- Codegen 0.4.1: magnifier-aware `run()` overload, `strategy.exit qty/oca_name`
  plumbing, timeframe literal validation, `Series<int>` promotion from
  time/timestamp builtins.

Public-facing claims at risk:
- `TV parity 227/228` badge in root README.
- `MACD 672 bars: 0.4 ms` speed badge.
- `48 of 50 in three-way benchmark vs PyneCore + PineTS` body text.

If any of those numbers shift materially, the slogan is wrong; if they don't
shift, we have a defended dataset for the next year. Either outcome is worth
the run.

## Goals

1. Regenerate the parity report (50 strategies × 3 engines) against engine
   v0.4.1 + latest PyneCore + latest PineTS.
2. Promote ≥15 corpus probes covering features shipped 2025-05 → 2026-05 into
   the benchmark suite, expanding to ~65–75 strategies.
3. Replace the single anecdotal speed badge with a per-strategy timing table
   (PineForge vs PyneCore vs PineTS) backed by a reproducible harness.
4. Update README badges + `benchmarks/README.md` with "as of 2026-05" date and
   refreshed numbers.

## Non-goals

- Do **not** extend the OHLCV pin past 2025-03 for parity tests (would
  invalidate every `tv_trades.csv` and require re-export of 75 TV CSVs by
  hand). Speed harness MAY use a longer feed; parity must not.
- Do **not** add hand-authored net-new PineScript strategies. Promotion source
  is `corpus/probes/` only.
- Do **not** re-pin OHLCV to a fresh download; reuse the LFS-tracked
  `ETHUSDT_15.csv` already in `benchmarks/assets/data/`.
- Do **not** publish private benchmark assets. The submodule remains private;
  results (markdown reports, charts) are public.

## Decisions (already taken with user)

| Decision | Choice |
|---|---|
| Scope | Parity refresh + per-strategy speed + corpus-mined expansion |
| Competitor pins | Latest PyneCore + latest PineTS (apples-to-apples now) |
| New-strategy source | Promote existing `corpus/probes/` covering new features |
| Speed scope | Full per-strategy timing for all 50–75 strategies |
| OHLCV pin | Keep 2025-03 pin (parity stays valid) |
| Speed harness | Google Benchmark C++ for PineForge; subprocess timing for PyneCore + PineTS |

## Architecture overview

```
benchmarks/
├── assets/                              (private submodule, unchanged structure)
│   ├── data/ETHUSDT_15.csv              (PIN: 2025-03 snapshot, do not extend)
│   └── strategies/
│       ├── 01-50/                       (existing 50, refresh outputs)
│       ├── 51-NN/                       (NEW: corpus-promoted, this spec)
│       └── _indicators/                 (refresh canonical_*.csv)
├── runners/
│   ├── cloud_compile.py                 (existing, --force run all)
│   ├── regenerate_pineforge_trades.py   (existing, run against engine v0.4.1)
│   ├── run_pynecore.py                  (existing, latest PyneCore)
│   ├── run_pinets_canonical.mjs         (existing, latest PineTS)
│   ├── run_pineforge_canonical.cpp      (existing)
│   └── promote_corpus_probes.py         (NEW: mining + scaffolding tool)
├── speed/                               (NEW dir)
│   ├── pineforge_bench.cpp              (Google Benchmark harness)
│   ├── time_pynecore.py                 (subprocess wall-time wrapper, N runs)
│   ├── time_pinets.mjs                  (subprocess wall-time wrapper, N runs)
│   ├── aggregate.py                     (collect + emit speed.md table)
│   └── CMakeLists.txt                   (add to engine build)
├── compare.py                           (existing, no change)
├── compare_indicators.py                (existing, no change)
├── results/
│   ├── summary.md                       (REFRESH)
│   ├── trade_comparison.md              (REFRESH, expanded rows)
│   ├── indicator_comparison.md          (REFRESH)
│   └── speed.md                         (NEW)
└── run_all.sh                           (UPDATE: add speed phase)
```

## Phase plan

Each phase is a checkpoint — commit at end, allow user review before next.

### Phase 0 — Pre-flight

- `cd benchmarks && uv sync` (DONE 2026-05-15: PyneCore 6.4.4 → 6.4.6).
- `cd benchmarks && npm update pinets` (capture installed version).
- Smoke-test all three engines on `01-sma-cross` to confirm no environmental
  regression before running the full sweep.
- Check PineTS roadmap; if strategy backtester landed since 2025-05, plan a
  `compare.py` extension as Phase 4b. (Out of scope for this spec if it
  shipped — track separately.)

**Verification trial already done (this session):**
PyneSys cloud-compile of `01-sma-cross` returns byte-identical output to the
pinned `strategy_pyne.py`. Cloud compiler v6.4.6 has not drifted for this
script. ~1.1 s per strategy.

**Exit criteria:** all 3 engines exit-zero on smoke run, versions captured in
`results/summary.md` header.

### Phase 1 — Cloud-compile sweep

- `python runners/cloud_compile.py --force` (~50 × 1.1s ≈ 1 min).
- Collect per-file diff vs the pinned `strategy_pyne.py` set. Write diff
  log to `results/cloud_compile_drift.md` (committed).
- Any strategies with non-trivial drift are flagged: subsequent PyneCore
  trade-list deltas attributable to compiler change, not engine change.

**Exit criteria:** all 50 cloud-compile OK; drift log committed and reviewed.

### Phase 2 — PineForge regeneration (existing 50)

- `python runners/regenerate_pineforge_trades.py` against engine v0.4.1 build.
- Outputs `pineforge_trades.csv` per strategy.

**Exit criteria:** 50 trade lists generated, no engine errors logged.

### Phase 3 — PyneCore re-run (existing 50)

- `python runners/run_pynecore.py` (latest PyneCore from Phase 0).
- ~30 min wall-time.

**Exit criteria:** 50 PyneCore trade lists generated.

### Phase 4 — Indicator + comparator refresh

- Re-run canonical indicator script across 3 engines.
- `python compare.py && python compare_indicators.py` regenerates the three
  markdown reports under `results/`.
- Inspect for tier shifts since 2025-05-05; tier deltas (especially
  excellent → strong/moderate) MUST be investigated and root-caused before
  publishing — they may indicate engine regression.

**Exit criteria:** `summary.md`, `trade_comparison.md`, `indicator_comparison.md`
refreshed; tier-shift root-cause notes (if any) added to `summary.md`.

### Phase 5 — Corpus promotion (NEW: 51 onward)

Build `runners/promote_corpus_probes.py`:

1. Scan `corpus/probes/` for probes whose `meta.yaml` (or equivalent metadata)
   tags or whose `.pine` source touches: `hour(`, `dayofweek(`, `time(`,
   `set_chart_timezone`, `bar_magnifier`, `oca_name`, `strategy.exit` with
   `qty=` or `qty_percent=`, `request.security` with LTF, deferred-flip
   patterns.
2. Filter to probes that have a `tv_trades.csv` (or canonical equivalent)
   suitable as ground truth.
3. Rank candidates by feature coverage; pick ≥15 (target ~25) avoiding
   duplication of features already exercised by the 50.
4. Scaffold each into `assets/strategies/NN-<slug>/` with `strategy.pine`,
   `tv_trades.csv` copied, ready for cloud-compile.

**Exit criteria:** new strategy folders committed to assets submodule;
coverage matrix appended to `benchmarks/README.md` showing which feature each
new entry exercises.

### Phase 6 — Run expansion through Phases 1–4

- Re-run cloud-compile + PineForge + PyneCore + comparator on the expanded
  set.
- Reports now have 65–75 rows.

**Exit criteria:** expanded reports replace prior `results/*.md`.

### Phase 7 — Speed harness

Scaffold `benchmarks/speed/`:

- `pineforge_bench.cpp` — Google Benchmark target. One `BENCHMARK` per
  strategy; loads strategy `.so` once, runs `pf_strategy_run()` over the
  pinned OHLCV. Reports median + p95 wall-time, allocations, and
  cold-start (load + first run) vs hot rerun (subsequent runs amortizing
  the load).
- `CMakeLists.txt` — link against `libpineforge` + `benchmark::benchmark`,
  expose as ctest target so CI can sample (subset, not full sweep) on PR.
- `time_pynecore.py` / `time_pinets.mjs` — N=20 runs per strategy, capture
  wall-time via `time.perf_counter` / `process.hrtime.bigint`. Output JSON
  per strategy.
- `aggregate.py` — read all three JSON sets, emit `speed.md` with
  per-strategy table:

  ```
  | Strategy | PineForge median | PineForge p95 | PyneCore median | PineTS median* | speedup vs PyneCore |
  ```

  *PineTS column only populated for indicator-only strategies (no strategy
  backtest support).

- Hardware spec captured at top of `speed.md` (CPU model, core count, OS,
  build flags, container/host).

**Exit criteria:** `speed.md` published; MACD-672 spot number from old badge
either confirmed or replaced; methodology section explains why mixed
methodology (GBench for PineForge, subprocess for others) is fair (process
launch + interpreter startup is part of the realistic workload for those
engines and is *not* counted in PineForge's GBench number — disclose this
explicitly).

### Phase 8 — README + badge sync

- Update root `README.md` parity badge `227/228` → current.
- Update speed badge to point to `speed.md` rather than a single number; or
  swap to `MACD 672 bars: <new median> ms` if number is still hero-worthy.
- Update `benchmarks/README.md` "as of YYYY-MM" header + headline numbers.
- Bump `benchmarks/assets` submodule pointer in engine repo to capture
  new strategies + refreshed CSVs.

**Exit criteria:** README badges accurate; PR opened.

### Phase 9 — PR + merge

- One PR per major phase boundary OR one bundled PR labelled
  `chore(bench): annual refresh + speed table + corpus expansion`.
- Decide at Phase 8 based on diff size; default to bundled.

## Risks + mitigations

| Risk | Mitigation |
|---|---|
| Cloud-compiler drift attributed to engine fixes | Phase 1 commit separates drift log from engine reruns; auditable. |
| PineTS released a strategy backtester since 2025-05 | Phase 0 checks roadmap. If shipped, file follow-up spec to extend `compare.py` (do not block this refresh). |
| Some corpus probes lack `tv_trades.csv` | Phase 5 filter discards them; accept lower count if needed. |
| Speed harness mixed methodology weakens claim | Phase 7 explicit disclosure in `speed.md` methodology section; numbers stay honest. |
| Tier regression on existing 50 (engine fix changed semantics) | Phase 4 exit criteria require root-cause notes; do not publish silent regression. |
| GBench harness adds ABI-coupling to engine build | Confine to `benchmarks/speed/` ctest target; use only the public C ABI (`<pineforge/pineforge.h>`). |

## Success criteria

- `summary.md`, `trade_comparison.md`, `indicator_comparison.md`,
  `speed.md` all dated 2026-05 with engine v0.4.1.
- ≥65 strategies in trade comparison (50 existing + ≥15 promoted).
- Per-strategy speed table covers 100% of strategies in trade comparison
  for PineForge, ≥80% for PyneCore, indicator-set for PineTS.
- README badges reflect new numbers (parity + speed).
- All run commands reproducible via `bash run_all.sh` (with speed phase
  added) + `cmake --build && ctest -R bench_*`.

## Out-of-scope follow-ups

- PineTS strategy-backtester support (separate spec when their roadmap lands).
- Hardware variance tracking across CI runners (separate spec; needs runner
  inventory).
- Public-readiness of benchmark assets repo (separate, intentional).
- Extending OHLCV beyond 2025-03 (deferred — needs full TV re-export).
