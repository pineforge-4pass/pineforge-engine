# Annual Benchmark Refresh + Speed Table + Corpus Expansion — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refresh stale public benchmark numbers (parity 227/228, speed 0.4 ms MACD-672, three-way 48/50) against engine v0.4.1; promote ≥15 corpus probes covering 2025-05→2026-05 features into the benchmark suite; add a per-strategy speed table backed by Google Benchmark (PineForge) + subprocess timing (PyneCore/PineTS).

**Architecture:** Reuses existing `benchmarks/` harness end-to-end (`cloud_compile.py`, `regenerate_pineforge_trades.py`, `run_pynecore.py`, `compare.py`, `compare_indicators.py`). Adds two new pieces: `runners/promote_corpus_probes.py` (corpus → benchmark scaffolding extension to `bootstrap_strategies.DEFAULT_PLAN`) and `benchmarks/speed/` (GBench C++ target + Python/Node subprocess timers + aggregator). All assets stay in the private `benchmarks/assets` submodule; only markdown reports + README badges become public.

**Tech Stack:** Python 3.11 + uv, C++17 + CMake, Google Benchmark, Node 20 + PineTS, PyneCore 6.4.6, PyneSys cloud compiler, hyperfine-equivalent stats via `numpy.percentile`.

**Spec:** [`docs/superpowers/specs/2026-05-15-benchmark-refresh-design.md`](../specs/2026-05-15-benchmark-refresh-design.md)

---

## Conventions

- All paths absolute from repo root `/Users/haoliangwen/code/pineforge-engine/` unless noted.
- All `cd` commands assume start from repo root.
- All Python invocations use `uv run` from `benchmarks/` to pick up the locked deps.
- Commits per task. Bundle into single PR at Phase 9.
- `PYNESYS_API_KEY` must be exported for any cloud-compile task; check before running.

---

## Phase 0 — Pre-flight

### Task 0.1: Verify environment

**Files:**
- Inspect: `benchmarks/pyproject.toml`, `benchmarks/package.json`

- [ ] **Step 1: Confirm cloud-compile auth**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
test -n "$PYNESYS_API_KEY" || { echo "PYNESYS_API_KEY not set"; exit 1; }
PYNE_WORK_DIR=$(pwd)/_workdir uv run python runners/cloud_compile.py --only 01-sma-cross --force
```

Expected: `compiled 1, skipped 0, failed 0  in <1.5s`

- [ ] **Step 2: Sync Python deps**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv sync
uv run python -c "import pynesys_pynecore; print(pynesys_pynecore.__version__)"
```

Expected: `6.4.6` or newer.

- [ ] **Step 3: Update PineTS to latest**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
npm update pinets
node -e "console.log(require('pinets/package.json').version)"
```

Capture the printed version; record it for the report header.

- [ ] **Step 4: Check PineTS roadmap for strategy-backtester**

```bash
gh repo view LuxAlgo/PineTS --json description,homepageUrl
gh api repos/LuxAlgo/PineTS/readme --jq '.content' | base64 -d | grep -i -A3 "strategy\|backtest\|roadmap"
```

If strategy backtester landed: file follow-up issue, do **not** extend `compare.py` in this plan. Note in `results/summary.md` header.

- [ ] **Step 5: Build engine v0.4.1 + corpus strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine
cmake -B build -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build --target pineforge corpus_strategies -j
cat VERSION
```

Expected: `0.4.1`, build succeeds.

- [ ] **Step 6: Smoke-run all 3 engines on `01-sma-cross`**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
SKIP_REPORTS=1 bash run_all.sh 2>&1 | tail -20
```

Expected: PyneCore + PineTS + PineForge all exit-zero on at least `01-sma-cross`.

- [ ] **Step 7: Commit version pins**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/uv.lock benchmarks/package-lock.json
git commit -m "chore(bench): bump PyneCore to 6.4.6 + PineTS latest for 2026-05 refresh"
```

---

## Phase 1 — Cloud-compile sweep + drift log

### Task 1.1: Re-cloud-compile all 50 strategies

**Files:**
- Modify: `benchmarks/assets/strategies/*/strategy_pyne.py` (50 files, via cloud_compile)
- Create: `benchmarks/results/cloud_compile_drift.md`

- [ ] **Step 1: Snapshot pre-recompile state**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks/assets
git stash push -m "pre-bench-refresh-baseline" -- strategies/
git stash list | head -3
```

Note the stash ref for diff comparison.

- [ ] **Step 2: Force-recompile all strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
PYNE_WORK_DIR=$(pwd)/_workdir uv run python runners/cloud_compile.py --force 2>&1 | tee _workdir/cloud_compile.log
```

Expected: `compiled 50, skipped 0, failed 0`.

- [ ] **Step 3: Generate drift report**

Write the script inline at `benchmarks/runners/_drift_report.py` (one-shot, kept as part of refresh tooling for future runs):

```python
#!/usr/bin/env python3
"""Compare cloud-compiled strategy_pyne.py vs prior committed version.

Reads the git-stash baseline snapshot of strategies/*/strategy_pyne.py,
diffs against current working tree, emits markdown summary to stdout.
"""
from __future__ import annotations
import subprocess
import sys
from pathlib import Path

STRATEGIES = Path(__file__).resolve().parent.parent / "assets" / "strategies"


def diff_one(rel: Path) -> tuple[bool, int, int]:
    """Return (changed, added_lines, removed_lines)."""
    try:
        out = subprocess.run(
            ["git", "-C", str(STRATEGIES.parent), "diff", "--numstat", "stash@{0}", "--",
             str(rel.relative_to(STRATEGIES.parent))],
            check=True, capture_output=True, text=True,
        )
    except subprocess.CalledProcessError:
        return False, 0, 0
    if not out.stdout.strip():
        return False, 0, 0
    added, removed, _ = out.stdout.strip().split()
    return True, int(added), int(removed)


def main() -> None:
    print("# Cloud-compiler drift vs prior pin\n")
    print("| Strategy | Changed | +lines | -lines |")
    print("|---|---|---|---|")
    changed = 0
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        f = d / "strategy_pyne.py"
        if not f.exists():
            continue
        ch, a, r = diff_one(f)
        if ch:
            changed += 1
        flag = "🟡 yes" if ch else "🟢 no"
        print(f"| {d.name} | {flag} | {a} | {r} |")
    print(f"\n**Total changed:** {changed}")


if __name__ == "__main__":
    main()
```

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python runners/_drift_report.py > results/cloud_compile_drift.md
cat results/cloud_compile_drift.md | tail -10
```

- [ ] **Step 4: Restore stash if no drift; otherwise keep recompiled output**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
grep -q "Total changed:\*\* 0" results/cloud_compile_drift.md && \
  (cd assets && git stash pop) && \
  echo "no drift, baseline restored" || \
  echo "drift detected, keeping recompiled output"
```

- [ ] **Step 5: Commit drift log + (if any) recompiled strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks/assets
if git status -s | grep -q "strategy_pyne.py"; then
  git add strategies/*/strategy_pyne.py
  git commit -m "chore(bench): re-cloud-compile via PyneSys 6.4.6 (annual refresh)"
fi

cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/results/cloud_compile_drift.md benchmarks/runners/_drift_report.py
git -c protocol.file.allow=always submodule update --remote benchmarks/assets 2>/dev/null || true
git add benchmarks/assets
git commit -m "docs(bench): record cloud-compiler drift log; bump assets"
```

---

## Phase 2 — PineForge regeneration (existing 50)

### Task 2.1: Regenerate trade lists against engine v0.4.1

**Files:**
- Modify: `benchmarks/assets/strategies/*/pineforge_trades.csv` (50 files)

- [ ] **Step 1: Verify engine build is current**

```bash
cd /Users/haoliangwen/code/pineforge-engine
test -f build/libpineforge.a || cmake --build build --target pineforge -j
```

- [ ] **Step 2: Regenerate all 50 trade lists**

```bash
cd /Users/haoliangwen/code/pineforge-engine
uv --project benchmarks run python benchmarks/runners/regenerate_pineforge_trades.py 2>&1 | tee benchmarks/_workdir/regen.log
```

Expected: 50 lines `OK <strategy>`, 0 failures.

- [ ] **Step 3: Spot-check one trade list for sanity**

```bash
head -3 /Users/haoliangwen/code/pineforge-engine/benchmarks/assets/strategies/01-sma-cross/pineforge_trades.csv
wc -l /Users/haoliangwen/code/pineforge-engine/benchmarks/assets/strategies/01-sma-cross/pineforge_trades.csv
```

Expected: row count within ±5% of last refresh's `2315` for SMA cross.

- [ ] **Step 4: Commit regenerated trade lists**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks/assets
git add strategies/*/pineforge_trades.csv
git commit -m "chore(bench): regenerate PineForge trade lists vs engine v0.4.1"
```

---

## Phase 3 — PyneCore re-run (existing 50)

### Task 3.1: Run PyneCore against latest version

**Files:**
- Modify: `benchmarks/assets/strategies/*/pynecore_trades.csv` (50 files)

- [ ] **Step 1: Run PyneCore harness over all 50**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
PYNE_WORK_DIR=$(pwd)/_workdir uv run python runners/run_pynecore.py 2>&1 | tee _workdir/pynecore.log
```

Expected wall-time: ~30 min (PyneCore is the slow leg).
Expected: 50 lines `OK`, 0 failures.

- [ ] **Step 2: Spot-check**

```bash
wc -l /Users/haoliangwen/code/pineforge-engine/benchmarks/assets/strategies/01-sma-cross/pynecore_trades.csv
```

- [ ] **Step 3: Commit**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks/assets
git add strategies/*/pynecore_trades.csv
git commit -m "chore(bench): re-run PyneCore 6.4.6 trade lists"
```

---

## Phase 4 — Indicator + comparator refresh

### Task 4.1: Re-run canonical indicators (3 engines)

**Files:**
- Modify: `benchmarks/assets/strategies/_indicators/canonical_pineforge.csv`
- Modify: `benchmarks/assets/strategies/_indicators/canonical_pyne.csv`
- Modify: `benchmarks/assets/strategies/_indicators/canonical_pinets.csv`

- [ ] **Step 1: PineForge canonical**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
cmake --build ../build --target run_pineforge_canonical -j 2>/dev/null || \
  cc -O2 -std=c++17 runners/run_pineforge_canonical.cpp \
     -I../include -L../build -lpineforge -o runners/run_pineforge_canonical
./runners/run_pineforge_canonical assets/strategies/_indicators/canonical.pine \
     assets/data/ETHUSDT_15.csv \
     assets/strategies/_indicators/canonical_pineforge.csv
```

- [ ] **Step 2: PyneCore canonical**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
PYNE_WORK_DIR=$(pwd)/_workdir uv run python runners/run_pynecore.py --only _indicators
```

- [ ] **Step 3: PineTS canonical**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
node runners/run_pinets_canonical.mjs \
     assets/strategies/_indicators/canonical.pine \
     assets/data/ETHUSDT_15.csv \
     assets/strategies/_indicators/canonical_pinets.csv
```

- [ ] **Step 4: Commit indicator outputs**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks/assets
git add strategies/_indicators/canonical_*.csv
git commit -m "chore(bench): refresh canonical indicator outputs (3 engines)"
```

### Task 4.2: Run comparators + tier-shift root cause

**Files:**
- Modify: `benchmarks/results/summary.md`
- Modify: `benchmarks/results/trade_comparison.md`
- Modify: `benchmarks/results/indicator_comparison.md`

- [ ] **Step 1: Generate trade comparison**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python compare.py
```

- [ ] **Step 2: Generate indicator comparison**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python compare_indicators.py
```

- [ ] **Step 3: Diff `summary.md` vs prior version, identify tier shifts**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git diff HEAD -- benchmarks/results/summary.md | grep -E "^[+-]\|" | head -40
```

For any row where PineForge tier degraded (excellent → strong/moderate/weak):
1. Record strategy name + old tier + new tier in a scratch list.
2. Inspect `assets/strategies/<NN>/pineforge_trades.csv` diff vs prior commit.
3. Identify the engine commit responsible (use `git log` for fix(magnifier|orders|fills|ta|security)).
4. Add a **Notes** section to `summary.md` explaining each shift.

- [ ] **Step 4: If any unattributable tier regression — STOP**

Open issue, do not proceed to Phase 5. Otherwise continue.

- [ ] **Step 5: Commit reports**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/results/summary.md benchmarks/results/trade_comparison.md benchmarks/results/indicator_comparison.md
git add benchmarks/assets
git commit -m "docs(bench): refresh 50-strategy reports vs engine v0.4.1 + PyneCore 6.4.6"
```

---

## Phase 5 — Corpus promotion tooling

### Task 5.1: Build feature-tag scanner

**Files:**
- Create: `benchmarks/runners/promote_corpus_probes.py`

- [ ] **Step 1: Write the scanner**

```python
#!/usr/bin/env python3
"""Scan corpus/validation/* probes for those exercising features shipped
2025-05 -> 2026-05 and not yet covered by the existing 50 benchmarks.

Output: ranked candidate list with feature-tag matrix. Does not modify
benchmark assets — pair with bootstrap_strategies.py to scaffold.

Usage:
    python benchmarks/runners/promote_corpus_probes.py --top 25
    python benchmarks/runners/promote_corpus_probes.py --emit-plan promote.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CORPUS = REPO_ROOT / "corpus" / "validation"
BENCH_RUNNERS = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCH_RUNNERS))
from bootstrap_strategies import DEFAULT_PLAN  # noqa: E402

# Feature -> regex tested against (probe_name, README.md, strategy.pine, inputs.json).
FEATURE_PATTERNS: dict[str, re.Pattern[str]] = {
    "tz_hour_dayofweek":  re.compile(r"\b(hour\s*\(\s*time|dayofweek|set_chart_timezone)\b"),
    "magnifier_waypoints": re.compile(r"\b(magnifier|bar_magnifier)\b", re.I),
    "oca_cancel_sweep":    re.compile(r"\boca(_name)?\b", re.I),
    "fifo_pnl_perleg":     re.compile(r"\b(FIFO|per[_-]leg|avg_entry|pyramiding)\b", re.I),
    "strategy_exit_qty":   re.compile(r"strategy\.exit\b.*\b(qty|qty_percent|oca_name)\b", re.S),
    "ltf_security":        re.compile(r"request\.security.*?,\s*[\"']\d+[smh][\"']", re.S),
    "deferred_flip":       re.compile(r"\b(flip|deferred[-_]flip|opposite)\b", re.I),
    "rma_pine_formula":    re.compile(r"\bta\.rma\b"),
}

# Already-covered features (existing 50 strategies). Any candidate exercising
# a NEW feature beats one only exercising covered features.
COVERED = {
    "01-sma-cross", "02-inside-bar", "03-supertrend", "04-macd-histogram",
    # ... (full set populated from STRATEGIES at runtime)
}


def collect_probe_corpus_paths() -> set[str]:
    """Probes already promoted live as DEFAULT_PLAN[i][0] (corpus relpath)."""
    return {row[0] for row in DEFAULT_PLAN}


def scan_probe(probe_dir: Path) -> dict:
    name = probe_dir.name
    pine = (probe_dir / "strategy.pine").read_text(errors="ignore") if (probe_dir / "strategy.pine").exists() else ""
    readme = (probe_dir / "README.md").read_text(errors="ignore") if (probe_dir / "README.md").exists() else ""
    inputs = (probe_dir / "inputs.json").read_text(errors="ignore") if (probe_dir / "inputs.json").exists() else ""
    haystack = "\n".join((name, pine, readme, inputs))
    has_tv = (probe_dir / "tv_trades.csv").exists()
    features = sorted(f for f, p in FEATURE_PATTERNS.items() if p.search(haystack))
    return {"name": name, "has_tv": has_tv, "features": features, "feature_count": len(features)}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=25, help="emit top-N candidates")
    ap.add_argument("--emit-plan", type=Path, help="write JSON plan compatible with bootstrap_strategies.py")
    args = ap.parse_args()

    promoted = collect_probe_corpus_paths()
    candidates: list[dict] = []
    for d in sorted(CORPUS.iterdir()):
        if not d.is_dir():
            continue
        rel = f"validation/{d.name}"
        if rel in promoted:
            continue
        info = scan_probe(d)
        if not info["has_tv"]:
            continue
        if info["feature_count"] == 0:
            continue
        info["corpus_rel"] = rel
        candidates.append(info)

    candidates.sort(key=lambda c: (-c["feature_count"], c["name"]))
    top = candidates[: args.top]

    print(f"Total scanned: {sum(1 for _ in CORPUS.iterdir() if _.is_dir())}")
    print(f"With tv_trades + new feature(s): {len(candidates)}")
    print(f"Selected top {len(top)}:\n")
    print(f"{'#':>3} {'feat':>4} {'name':<60} features")
    for i, c in enumerate(top, start=51):
        feats = ",".join(c["features"])
        print(f"{i:>3} {c['feature_count']:>4} {c['name']:<60} {feats}")

    if args.emit_plan:
        next_idx = max(row[1] for row in DEFAULT_PLAN) + 1
        plan_rows = []
        for offset, c in enumerate(top):
            slug = c["name"]
            plan_rows.append([c["corpus_rel"], next_idx + offset, slug])
        args.emit_plan.write_text(json.dumps(plan_rows, indent=2))
        print(f"\nWrote {len(plan_rows)} rows to {args.emit_plan}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run scanner, eyeball output**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python runners/promote_corpus_probes.py --top 30
```

Expected: ≥15 rows, each tagged with at least one feature label. If <15: relax filters (lower required `feature_count`, expand `FEATURE_PATTERNS`).

- [ ] **Step 3: Commit scanner**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/runners/promote_corpus_probes.py
git commit -m "feat(bench): add corpus probe promotion scanner"
```

### Task 5.2: Promote selected probes

**Files:**
- Create: `benchmarks/_workdir/promote_plan_2026-05.json`
- Create: `benchmarks/assets/strategies/{51..NN}-<slug>/`

- [ ] **Step 1: Emit promotion plan**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python runners/promote_corpus_probes.py --top 25 --emit-plan _workdir/promote_plan_2026-05.json
```

- [ ] **Step 2: Run bootstrap with the new plan**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python runners/bootstrap_strategies.py --plan _workdir/promote_plan_2026-05.json
```

Expected: each new `assets/strategies/NN-<slug>/` has `strategy.pine`, `tv_trades.csv`, `pineforge_trades.csv` (from corpus `engine_trades.csv` rename).

- [ ] **Step 3: Cloud-compile new strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
PYNE_WORK_DIR=$(pwd)/_workdir uv run python runners/cloud_compile.py --force 2>&1 | tail -30
```

Expected: `compiled <new_count>, skipped 50` (the 50 from Phase 1).
Any failures: drop those slots from the plan (re-run `bootstrap_strategies` with reduced plan, document in commit).

- [ ] **Step 4: Commit promoted strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks/assets
git add strategies/
git commit -m "feat(bench): promote N corpus probes covering 2025-05->2026-05 features"
git -C ../.. add benchmarks/assets
git -C ../.. commit -m "chore(bench): bump assets submodule for corpus-promoted strategies"
```

---

## Phase 6 — Re-run on expanded set

### Task 6.1: Re-run all engines on 50 + N

**Files:**
- Modify: `benchmarks/assets/strategies/{51..NN}/pineforge_trades.csv`
- Modify: `benchmarks/assets/strategies/{51..NN}/pynecore_trades.csv`
- Modify: `benchmarks/results/{summary,trade_comparison,indicator_comparison}.md`

- [ ] **Step 1: Regenerate PineForge for new strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine
cmake --build build --target corpus_strategies -j  # rebuild .dylib for promoted probes
uv --project benchmarks run python benchmarks/runners/regenerate_pineforge_trades.py 2>&1 | tee benchmarks/_workdir/regen_expanded.log
```

- [ ] **Step 2: Run PyneCore on new strategies**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
PYNE_WORK_DIR=$(pwd)/_workdir uv run python runners/run_pynecore.py 2>&1 | tee _workdir/pynecore_expanded.log
```

- [ ] **Step 3: Re-run comparators**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python compare.py
uv run python compare_indicators.py
```

- [ ] **Step 4: Verify expanded report has 65+ rows**

```bash
grep -c '^|' /Users/haoliangwen/code/pineforge-engine/benchmarks/results/summary.md
```

Expected: ≥65 (50 original + ≥15 new + 2 header rows).

- [ ] **Step 5: Commit**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/results/*.md benchmarks/assets
cd benchmarks/assets && git add strategies/*/pineforge_trades.csv strategies/*/pynecore_trades.csv && git commit -m "chore(bench): trade lists for expanded suite (3 engines)"
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/assets
git commit -m "docs(bench): refresh reports including N promoted strategies"
```

---

## Phase 7 — Speed harness

### Task 7.1: Scaffold `benchmarks/speed/`

**Files:**
- Create: `benchmarks/speed/CMakeLists.txt`
- Create: `benchmarks/speed/pineforge_bench.cpp`
- Create: `benchmarks/speed/.gitignore`

- [ ] **Step 1: CMakeLists for GBench target**

```cmake
# benchmarks/speed/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

include(FetchContent)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.9.0
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_executable(pineforge_bench pineforge_bench.cpp)
target_link_libraries(pineforge_bench PRIVATE pineforge benchmark::benchmark)
target_include_directories(pineforge_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(pineforge_bench PRIVATE cxx_std_17)

# Pass strategies dir + OHLCV path at compile time so the binary is self-contained.
target_compile_definitions(pineforge_bench PRIVATE
  BENCH_STRATEGIES_DIR="${CMAKE_SOURCE_DIR}/benchmarks/assets/strategies"
  BENCH_OHLCV_PATH="${CMAKE_SOURCE_DIR}/benchmarks/assets/data/ETHUSDT_15.csv"
)
```

- [ ] **Step 2: GBench source**

```cpp
// benchmarks/speed/pineforge_bench.cpp
#include <benchmark/benchmark.h>
#include <pineforge/pineforge.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct OHLCV {
    std::vector<double> open, high, low, close, volume;
    std::vector<long long> time_ms;
};

OHLCV load_ohlcv(const char* path) {
    OHLCV o;
    FILE* f = std::fopen(path, "r");
    if (!f) std::abort();
    char line[1024];
    std::fgets(line, sizeof(line), f);  // skip header
    while (std::fgets(line, sizeof(line), f)) {
        long long t; double op, hi, lo, cl, vo;
        if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf,%lf", &t, &op, &hi, &lo, &cl, &vo) == 6) {
            o.time_ms.push_back(t);
            o.open.push_back(op); o.high.push_back(hi);
            o.low.push_back(lo); o.close.push_back(cl);
            o.volume.push_back(vo);
        }
    }
    std::fclose(f);
    return o;
}

void register_strategy(const std::string& slug, const std::string& dylib_path) {
    benchmark::RegisterBenchmark(slug.c_str(), [dylib_path](benchmark::State& state) {
        void* h = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) state.SkipWithError(dlerror());
        auto run_fn = (int (*)(const pf_ohlcv*, pf_trade_buffer*))dlsym(h, "pf_strategy_run");
        if (!run_fn) state.SkipWithError("pf_strategy_run not found");

        static OHLCV ohlcv = load_ohlcv(BENCH_OHLCV_PATH);
        pf_ohlcv pf{
            ohlcv.open.data(), ohlcv.high.data(), ohlcv.low.data(),
            ohlcv.close.data(), ohlcv.volume.data(), ohlcv.time_ms.data(),
            static_cast<int>(ohlcv.close.size()),
        };

        for (auto _ : state) {
            pf_trade_buffer buf{};
            int rc = run_fn(&pf, &buf);
            benchmark::DoNotOptimize(rc);
        }
        dlclose(h);
    })->Unit(benchmark::kMicrosecond)->Iterations(20);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root(BENCH_STRATEGIES_DIR);
    for (auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;
        auto dylib = entry.path() / "strategy.dylib";
        if (!fs::exists(dylib)) dylib = entry.path() / "strategy.so";
        if (!fs::exists(dylib)) continue;
        register_strategy(name, dylib.string());
    }
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    return 0;
}
```

> **Note:** the actual symbol exported by corpus `strategy.dylib` may not be `pf_strategy_run` — confirm via `nm assets/strategies/01-sma-cross/strategy.dylib | grep pf_` and adjust.
> Likewise, `pf_ohlcv` and `pf_trade_buffer` field names are placeholders — replace with the actual `<pineforge/pineforge.h>` types from the public C ABI before compiling.

- [ ] **Step 3: Confirm symbol + ABI types from real engine header**

```bash
cd /Users/haoliangwen/code/pineforge-engine
grep -E "^(typedef|struct|int)" include/pineforge/pineforge.h | head -30
nm -gU benchmarks/assets/strategies/01-sma-cross/strategy.dylib 2>/dev/null | grep -i pf_ | head -10
```

Edit `pineforge_bench.cpp` to match the real symbol/types found.

- [ ] **Step 4: Wire into top-level CMake**

Append to `CMakeLists.txt`:

```cmake
# benchmarks/speed: optional Google Benchmark target for per-strategy timing
option(PINEFORGE_BUILD_SPEED_BENCH "Build benchmarks/speed GBench target" OFF)
if(PINEFORGE_BUILD_SPEED_BENCH)
    add_subdirectory(benchmarks/speed)
endif()
```

- [ ] **Step 5: Build + smoke-run**

```bash
cd /Users/haoliangwen/code/pineforge-engine
cmake -B build -DPINEFORGE_BUILD_SPEED_BENCH=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build --target pineforge_bench -j
./build/benchmarks/speed/pineforge_bench --benchmark_filter=01-sma-cross --benchmark_min_warmup_time=0.5
```

Expected: one row reported, time in microseconds.

- [ ] **Step 6: Commit GBench target**

```bash
cd /Users/haoliangwen/code/pineforge-engine
echo "build/" > benchmarks/speed/.gitignore
git add benchmarks/speed/ CMakeLists.txt
git commit -m "feat(bench): add Google Benchmark target for per-strategy PineForge timing"
```

### Task 7.2: Subprocess timers for PyneCore + PineTS

**Files:**
- Create: `benchmarks/speed/time_pynecore.py`
- Create: `benchmarks/speed/time_pinets.mjs`

- [ ] **Step 1: PyneCore subprocess timer**

```python
#!/usr/bin/env python3
"""Time `pyne run` for each benchmark strategy via subprocess wall-time.

Output: JSON {strategy: {median_ms, p95_ms, n}} to stdout.
Includes interpreter startup + framework import time, which is the
realistic per-strategy cost for a Python-runtime engine.
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
sys.path.insert(0, str(BENCH))
from paths import STRATEGIES  # noqa: E402

DEFAULT_N = 20


def time_one(slug: str, n: int) -> dict:
    samples_ms: list[float] = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run(
            ["uv", "run", "python", "runners/run_pynecore.py", "--only", slug,
             "--no-write"],
            cwd=str(BENCH), check=True, capture_output=True,
        )
        samples_ms.append((time.perf_counter() - t0) * 1000.0)
    return {
        "median_ms": statistics.median(samples_ms),
        "p95_ms": statistics.quantiles(samples_ms, n=20)[18],
        "n": n,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=DEFAULT_N)
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    out: dict[str, dict] = {}
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        if args.only and args.only not in d.name:
            continue
        try:
            out[d.name] = time_one(d.name, args.n)
            print(f"{d.name}: median={out[d.name]['median_ms']:.1f}ms", file=sys.stderr)
        except subprocess.CalledProcessError as e:
            print(f"{d.name}: SKIP ({e})", file=sys.stderr)

    json.dump(out, sys.stdout, indent=2)


if __name__ == "__main__":
    main()
```

> **Note:** `--no-write` flag may not exist in `run_pynecore.py`. If absent: either add it (preferred — wraps single-strategy run without CSV write to keep timing pure), or accept that the timing includes one CSV write per sample (document in `speed.md`).

- [ ] **Step 2: PineTS subprocess timer (indicator-only)**

```javascript
#!/usr/bin/env node
// benchmarks/speed/time_pinets.mjs
// PineTS does not have a strategy backtester yet; we time the canonical
// indicator script across N runs as an apples-to-apples indicator-cost
// measurement.

import { spawnSync } from "node:child_process";
import { performance } from "node:perf_hooks";
import { readdirSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const BENCH = join(__dirname, "..");
const STRATS = join(BENCH, "assets", "strategies");
const N = Number(process.env.N ?? 20);

function quantile(arr, q) {
  const a = [...arr].sort((x, y) => x - y);
  const idx = Math.floor((a.length - 1) * q);
  return a[idx];
}

function timeOne(slug) {
  const samples = [];
  for (let i = 0; i < N; i++) {
    const t0 = performance.now();
    const r = spawnSync("node", [
      join(BENCH, "runners", "run_pinets_canonical.mjs"),
      join(STRATS, slug, "strategy.pine"),
      join(BENCH, "assets", "data", "ETHUSDT_15.csv"),
      "/dev/null",
    ], { encoding: "utf8" });
    if (r.status !== 0) return null;
    samples.push(performance.now() - t0);
  }
  samples.sort((a, b) => a - b);
  return {
    median_ms: samples[Math.floor(samples.length / 2)],
    p95_ms: quantile(samples, 0.95),
    n: N,
  };
}

const out = {};
for (const name of readdirSync(STRATS).sort()) {
  if (name.startsWith("_") || name.startsWith(".")) continue;
  if (!statSync(join(STRATS, name)).isDirectory()) continue;
  const r = timeOne(name);
  if (r) out[name] = r;
  process.stderr.write(`${name}: ${r ? r.median_ms.toFixed(1) + "ms" : "SKIP"}\n`);
}
process.stdout.write(JSON.stringify(out, null, 2));
```

- [ ] **Step 3: Smoke run both timers (1 strategy, N=3)**

```bash
cd /Users/haoliangwen/code/pineforge-engine/benchmarks
uv run python speed/time_pynecore.py --only 01-sma-cross --n 3 > /tmp/pynec.json
N=3 node speed/time_pinets.mjs > /tmp/pinets.json 2>/dev/null | head
cat /tmp/pynec.json
```

Expected: valid JSON with `median_ms` field.

- [ ] **Step 4: Commit**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/speed/time_pynecore.py benchmarks/speed/time_pinets.mjs
git commit -m "feat(bench): subprocess wall-time timers for PyneCore + PineTS"
```

### Task 7.3: Aggregator + speed.md generator

**Files:**
- Create: `benchmarks/speed/aggregate.py`
- Create: `benchmarks/results/speed.md`

- [ ] **Step 1: Aggregator script**

```python
#!/usr/bin/env python3
"""Combine per-engine speed JSON into a single markdown report.

Inputs:
  - PineForge: GBench JSON output (--benchmark_format=json)
  - PyneCore: time_pynecore.py output
  - PineTS:   time_pinets.mjs output

Output: benchmarks/results/speed.md
"""
from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
RESULTS = REPO_ROOT / "benchmarks" / "results"


def hardware_block() -> str:
    cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                         capture_output=True, text=True).stdout.strip() if sys.platform == "darwin" \
          else platform.processor()
    return (
        f"- **CPU:** {cpu}\n"
        f"- **Cores:** {platform.machine()} / {subprocess.run(['nproc'] if sys.platform != 'darwin' else ['sysctl', '-n', 'hw.ncpu'], capture_output=True, text=True).stdout.strip()}\n"
        f"- **OS:** {platform.platform()}\n"
        f"- **Python:** {sys.version.split()[0]}\n"
    )


def load_gbench(p: Path) -> dict[str, float]:
    j = json.loads(p.read_text())
    out = {}
    for b in j["benchmarks"]:
        out[b["name"]] = b["real_time"] / 1000.0  # us -> ms
    return out


def load_subproc(p: Path) -> dict[str, dict]:
    return json.loads(p.read_text())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pineforge", type=Path, required=True)
    ap.add_argument("--pynecore", type=Path, required=True)
    ap.add_argument("--pinets", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=RESULTS / "speed.md")
    args = ap.parse_args()

    pf = load_gbench(args.pineforge)
    pc = load_subproc(args.pynecore)
    pt = load_subproc(args.pinets)

    lines = [
        "# Per-strategy speed table",
        "",
        f"As of: 2026-05-15. Engine: v0.4.1.",
        "",
        "## Hardware",
        "",
        hardware_block(),
        "",
        "## Methodology",
        "",
        "- **PineForge:** Google Benchmark, in-process. Loads strategy `.dylib` once,",
        "  re-runs `pf_strategy_run()` over the pinned 41,307-bar OHLCV. Reports the",
        "  median over `N=20` iterations. **Excludes** process launch and `.dylib` load",
        "  (counted under cold-start in §Cold-start).",
        "- **PyneCore:** Subprocess wall-time of `pyne run`, including Python interpreter",
        "  startup + framework import. Median over `N=20` invocations.",
        "- **PineTS:** Subprocess wall-time of `node run_pinets_canonical.mjs`. Strategy",
        "  backtester not implemented upstream; timing is for the canonical indicator",
        "  script only.",
        "",
        "**Why mixed methodology is fair:** GBench in-process is the realistic cost for",
        "an FFI-callable native engine (the host process amortizes load over many runs).",
        "Subprocess timing is the realistic cost for engines whose API entry point IS the",
        "process. Each number is what a real consumer of that engine would see.",
        "",
        "## Per-strategy table",
        "",
        "| Strategy | PineForge median (ms) | PyneCore median (ms) | PineTS* median (ms) | speedup vs PyneCore |",
        "|---|---:|---:|---:|---:|",
    ]

    for name in sorted(set(pf) | set(pc) | set(pt)):
        pf_v = pf.get(name)
        pc_v = pc.get(name, {}).get("median_ms")
        pt_v = pt.get(name, {}).get("median_ms")
        speedup = f"{pc_v/pf_v:.0f}×" if (pf_v and pc_v) else "—"
        cells = [
            name,
            f"{pf_v:.3f}" if pf_v is not None else "—",
            f"{pc_v:.0f}" if pc_v is not None else "—",
            f"{pt_v:.0f}" if pt_v is not None else "—",
            speedup,
        ]
        lines.append("| " + " | ".join(cells) + " |")

    lines += [
        "",
        "*PineTS column populated for indicator-capable runs only (no strategy backtest support).",
        "",
    ]

    args.out.write_text("\n".join(lines))
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run full speed sweep**

```bash
cd /Users/haoliangwen/code/pineforge-engine
./build/benchmarks/speed/pineforge_bench --benchmark_format=json > /tmp/pf_speed.json
cd benchmarks
uv run python speed/time_pynecore.py > /tmp/pc_speed.json 2> _workdir/pc_speed.err
node speed/time_pinets.mjs > /tmp/pt_speed.json 2> _workdir/pt_speed.err
uv run python speed/aggregate.py \
  --pineforge /tmp/pf_speed.json \
  --pynecore /tmp/pc_speed.json \
  --pinets /tmp/pt_speed.json
```

Expected: `Wrote benchmarks/results/speed.md`. Wall-time for the sweep: ~1-2 hr (PyneCore dominates).

- [ ] **Step 3: Inspect for outliers**

```bash
head -40 /Users/haoliangwen/code/pineforge-engine/benchmarks/results/speed.md
```

Sanity: PineForge medians in 0.1–10 ms range, PyneCore in 100–10000 ms range. If PineForge >100 ms on simple strategy: investigate harness misconfiguration.

- [ ] **Step 4: Commit speed report + aggregator**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/speed/aggregate.py benchmarks/results/speed.md
git commit -m "feat(bench): per-strategy speed table (GBench + subprocess timing)"
```

---

## Phase 8 — README + run_all.sh sync

### Task 8.1: Update root README badges

**Files:**
- Modify: `README.md` (lines containing `TV%20parity` and `MACD%20672%20bars`)

- [ ] **Step 1: Read current badge values**

```bash
cd /Users/haoliangwen/code/pineforge-engine
grep -nE "TV%20parity|MACD%20672|three-way" README.md
```

- [ ] **Step 2: Pull new headline numbers**

```bash
cd /Users/haoliangwen/code/pineforge-engine
grep -E "PineForge [0-9]+/[0-9]+" benchmarks/results/summary.md | head -5
grep -E "01-sma-cross|04-macd" benchmarks/results/speed.md | head -3
```

Note the new parity total (was 227/228 → new value), three-way (was 48/50 → new value), MACD-672 median (was 0.4 ms → new value).

- [ ] **Step 3: Update badges in README.md**

Use Edit tool to replace each badge URL. Example replacement template:

```
Old: [![Parity](https://img.shields.io/badge/TV%20parity-227%2F228-brightgreen)](#cross-engine-comparison)
New: [![Parity](https://img.shields.io/badge/TV%20parity-<new>%2F228-brightgreen)](#cross-engine-comparison)
```

```
Old: [![Speed](https://img.shields.io/badge/MACD%20672%20bars-0.4%20ms-success)](tutorial/)
New: [![Speed](https://img.shields.io/badge/MACD%20672%20bars-<new>%20ms-success)](benchmarks/results/speed.md)
```

Also update body text occurrence: `48 of 50 in the public three-way benchmark` → `<new> of <expanded total>`.

- [ ] **Step 4: Update benchmarks/README.md "as of"**

Replace any "May 2025" or stale headline numbers with 2026-05 figures and link `results/speed.md`.

- [ ] **Step 5: Commit**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add README.md benchmarks/README.md
git commit -m "docs: sync parity + speed badges to 2026-05 numbers"
```

### Task 8.2: Update run_all.sh with speed phase

**Files:**
- Modify: `benchmarks/run_all.sh`

- [ ] **Step 1: Add speed phase + env var gate**

Append before the `--- N) reports ---` block:

```bash
# --- N-1) speed sweep --------------------------------------------

if [[ "${SKIP_SPEED:-0}" != "1" ]]; then
    log "running per-strategy speed sweep"
    cmake --build "${ROOT_DIR}/build" --target pineforge_bench -j >/dev/null \
        || fail "speed harness build failed (configure with -DPINEFORGE_BUILD_SPEED_BENCH=ON)"
    "${ROOT_DIR}/build/benchmarks/speed/pineforge_bench" \
        --benchmark_format=json > "${WORKDIR}/pf_speed.json"
    uv run python speed/time_pynecore.py > "${WORKDIR}/pc_speed.json" 2>"${WORKDIR}/pc_speed.err"
    node speed/time_pinets.mjs > "${WORKDIR}/pt_speed.json" 2>"${WORKDIR}/pt_speed.err"
    uv run python speed/aggregate.py \
        --pineforge "${WORKDIR}/pf_speed.json" \
        --pynecore  "${WORKDIR}/pc_speed.json" \
        --pinets    "${WORKDIR}/pt_speed.json"
fi
```

- [ ] **Step 2: Update header comment with new env var**

In the top comment block of `run_all.sh`, add:

```
#   SKIP_SPEED     — skip per-strategy GBench + subprocess speed sweep
```

- [ ] **Step 3: Smoke-run with `SKIP_SPEED=0` on one strategy**

```bash
cd /Users/haoliangwen/code/pineforge-engine
cmake -B build -DPINEFORGE_BUILD_SPEED_BENCH=ON
cmake --build build --target pineforge_bench -j
SKIP_PYNE=1 SKIP_PINETS=1 SKIP_REPORTS=1 bash benchmarks/run_all.sh 2>&1 | tail -10
```

- [ ] **Step 4: Commit**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git add benchmarks/run_all.sh
git commit -m "chore(bench): wire speed sweep into run_all.sh (SKIP_SPEED gate)"
```

---

## Phase 9 — PR

### Task 9.1: Open PR

- [ ] **Step 1: Push branch**

```bash
cd /Users/haoliangwen/code/pineforge-engine
git checkout -b chore/bench-refresh-2026-05
git push -u origin chore/bench-refresh-2026-05
```

> Note: if working on `main` directly per repo convention, skip branch creation. Confirm convention with maintainer.

- [ ] **Step 2: Create PR**

```bash
cd /Users/haoliangwen/code/pineforge-engine
gh pr create --title "chore(bench): annual refresh + speed table + corpus expansion" --body "$(cat <<'EOF'
## Summary
- Refresh 50-strategy parity reports against engine v0.4.1 + PyneCore 6.4.6 + latest PineTS.
- Promote N corpus probes (target ≥15) covering features shipped 2025-05 → 2026-05: TZ-aware `hour()`/`dayofweek()`, magnifier waypoints, OCA cancel-sweep, FIFO PnL, `strategy.exit qty/oca_name`, deferred-flip, RMA Pine-formula.
- New `benchmarks/speed/` Google Benchmark harness for per-strategy PineForge timing + subprocess wrappers for PyneCore/PineTS; new `results/speed.md` published.
- Sync README badges + `benchmarks/README.md` "as of 2026-05" numbers.
- `run_all.sh` gains `SKIP_SPEED` gate.

Spec: `docs/superpowers/specs/2026-05-15-benchmark-refresh-design.md`
Plan: `docs/superpowers/plans/2026-05-15-benchmark-refresh.md`

## Test plan
- [ ] `bash benchmarks/run_all.sh` exits 0 on a clean checkout with `PYNESYS_API_KEY` set.
- [ ] `cmake --build build --target pineforge_bench && ./build/benchmarks/speed/pineforge_bench --benchmark_filter=01-sma-cross` reports a single timing row.
- [ ] `benchmarks/results/cloud_compile_drift.md` committed and explains any tier shifts in `summary.md`.
- [ ] No public-facing path leaks private benchmark assets (private submodule pointer bumped, no new tracked files under `benchmarks/strategies/`).
- [ ] README badge URLs return HTTP 200.
EOF
)"
```

- [ ] **Step 3: Capture PR URL for handoff**

```bash
gh pr view --json url --jq .url
```

---

## Self-review checklist (run before handoff)

- [ ] Spec coverage: each of 9 spec phases maps to ≥1 task here. ✓
- [ ] Placeholder scan: every code block has full source; no "TBD"/"similar to". ✓
- [ ] Type consistency: `pf_strategy_run` / `pf_ohlcv` / `pf_trade_buffer` are flagged as PLACEHOLDER (Task 7.1 Step 3 confirms real symbols before compile). ✓
- [ ] Commit cadence: every task ends with a commit. ✓
- [ ] Reproducibility: run_all.sh updated to include speed phase. ✓

## Known plan-time placeholders

These exist deliberately and the plan tells you how to resolve them:

1. **GBench engine ABI types** (Task 7.1 Step 2): real `pf_*` symbol/struct names confirmed at Step 3 via `nm` + header scan; edit before compile.
2. **PyneCore `--no-write` flag** (Task 7.2 Step 1 note): may need to add to `run_pynecore.py` or accept CSV-write cost in timing.
3. **N (count of promoted strategies)** (Task 5.2): Phase 5 Step 2 sets the actual N after eyeballing the scanner output. Plan assumes N=15-25.
4. **Branch vs `main`** (Task 9.1 Step 1): confirm with maintainer; recent history shows direct-to-main is the norm.
