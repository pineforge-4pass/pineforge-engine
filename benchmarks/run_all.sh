#!/usr/bin/env bash
# benchmarks/run_all.sh — full three-way engine benchmark in one command.
#
# Builds the runtime (if not already built), runs every benchmark strategy
# through PyneCore + PineTS + PineForge, and produces the comparison
# reports under benchmarks/results/.
#
# Prerequisites:
#   - cmake >= 3.16 + a C++17 compiler (clang or gcc)
#   - uv (Python package manager) + Python 3.11+
#   - node >= 20
#   - git submodule update --init benchmarks/assets  (assets data + strategies)
#
# No API keys required. All inputs (OHLCV, strategy.pine, generated.cpp,
# tv_trades.csv, strategy_pyne.py) are committed in the assets submodule.
#
# Honours these env vars:
#   SKIP_BUILD          — skip the runtime + bench-strategy build step
#   SKIP_PYNE           — skip PyneCore strategy + indicator runs
#   SKIP_PINETS         — skip PineTS indicator run
#   SKIP_PINEFORGE      — skip PineForge trade regeneration
#   SKIP_SPEED          — skip the per-strategy speed sweep (pineforge_bench + timers)
#   SKIP_REPORTS        — skip the compare.py / compare_indicators.py step
#
# Maintenance scripts (refresh OHLCV, add new bench slots, refresh
# strategy_pyne.py, re-emit generated.cpp) are NOT part of this script —
# they live in pineforge-utils/bench-maintenance/ and require closed-source
# dependencies (codegen, PyneSys API key) that public reproducers don't need.

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "${BENCH_DIR}/assets/strategies" ]]; then
    BENCH_ASSETS="${BENCH_DIR}/assets"
else
    BENCH_ASSETS="${BENCH_DIR}"
fi
STRATEGIES_DIR="${BENCH_ASSETS}/strategies"
ROOT_DIR="$(cd "${BENCH_DIR}/.." && pwd)"
WORKDIR="${BENCH_DIR}/_workdir"

cd "${ROOT_DIR}"

log()  { printf '\033[1;34m[bench]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[bench]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[bench]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ ! -f "${STRATEGIES_DIR}/01-sma-cross/strategy.pine" ]]; then
    fail "benchmark fixtures missing (expected ${STRATEGIES_DIR}/01-sma-cross/strategy.pine).

TV-linked strategy folders and OHLCV live in the private benchmarks/assets
submodule (init: git submodule update --init benchmarks/assets).
See CONTRIBUTING.md.

Note: an inline benchmarks/strategies tree exists only in pre-migration /
private monorepo checkouts — do not publish those paths in public Git
history once the repo is open-sourced (see CONTRIBUTING.md)."
fi

# --- 1) build runtime + bench strategies --------------------------------

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    log "configuring + building libpineforge + bench strategy dylibs"
    cmake -B build \
        -DPINEFORGE_BUILD_TESTS=ON \
        -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON >/dev/null
    cmake --build build --target pineforge bench_strategies -j >/dev/null
fi

# --- 2) ensure benchmark deps ----------------------------------------

if [[ ! -d "${BENCH_DIR}/.venv" ]]; then
    log "creating Python venv at benchmarks/.venv"
    python3 -m venv "${BENCH_DIR}/.venv"
    "${BENCH_DIR}/.venv/bin/pip" install --quiet "pynesys-pynecore[cli]" pandas numpy
fi
if [[ ! -d "${BENCH_DIR}/node_modules" ]]; then
    log "installing pinets via npm"
    (cd "${BENCH_DIR}" && npm install --silent)
fi

# OHLCV is the committed snapshot at benchmarks/assets/data/ETHUSDT_15.csv
# (submodule). Every committed strategy_pyne.py + pineforge_trades.csv was
# generated against this file. To extend / re-fetch, see
# pineforge-utils/bench-maintenance/fetch_extended_ohlcv.py (maintainer-only).
source "${BENCH_DIR}/.venv/bin/activate"
mkdir -p "${WORKDIR}/data"
SNAPSHOT_CSV="${BENCH_ASSETS}/data/ETHUSDT_15.csv"
LIVE_CSV="${WORKDIR}/data/ETHUSDT_15.csv"

if [[ ! -f "${SNAPSHOT_CSV}" ]]; then
    fail "no OHLCV snapshot at benchmarks/assets/data/ETHUSDT_15.csv
init the benchmarks/assets submodule: git submodule update --init benchmarks/assets"
fi

if [[ ! -f "${LIVE_CSV}" ]] || ! cmp -s "${SNAPSHOT_CSV}" "${LIVE_CSV}"; then
    log "copying OHLCV snapshot to ${WORKDIR}/data/"
    cp "${SNAPSHOT_CSV}" "${LIVE_CSV}"
fi
# Re-convert to PyneCore .ohlcv if missing or stale.
if [[ ! -f "${LIVE_CSV%.csv}.ohlcv" ]] \
   || [[ "${LIVE_CSV}" -nt "${LIVE_CSV%.csv}.ohlcv" ]]; then
    pyne -w "${WORKDIR}" data convert-from \
        --provider pineforge --symbol ETHUSDT --timezone UTC \
        "${LIVE_CSV}" >/dev/null
fi

# --- 3c) regenerate PineForge trades on the extended OHLCV ----------
# Reads strategy.dylib built by cmake --target bench_strategies (step 1).

if [[ "${SKIP_PINEFORGE:-0}" != "1" ]]; then
    log "regenerating PineForge trades against extended OHLCV"
    python3 "${BENCH_DIR}/runners/regenerate_pineforge_trades.py" >/dev/null \
        || warn "regenerate_pineforge_trades.py reported failures; continuing"
fi

# --- 3d) run all strategies through PyneCore ------------------------

if [[ "${SKIP_PYNE:-0}" != "1" ]]; then
    n_strats=$(ls -d "${STRATEGIES_DIR}"/[0-9][0-9]-*/ 2>/dev/null | wc -l | tr -d ' ')
    log "running ${n_strats} strategies through PyneCore"
    failed=()
    for s in "${STRATEGIES_DIR}"/[0-9][0-9]-*/; do
        s="${s%/}"
        if ! python3 "${BENCH_DIR}/runners/run_pynecore.py" "$s" >/dev/null 2>&1; then
            failed+=("$(basename "$s")")
        fi
    done
    if (( ${#failed[@]} > 0 )); then
        warn "PyneCore runtime failed on ${#failed[@]} strategies: ${failed[*]}"
    fi

    log "running canonical indicators through PyneCore"
    pyne -w "${WORKDIR}" run \
        "${STRATEGIES_DIR}/_indicators/canonical_pyne.py" \
        "${WORKDIR}/data/ETHUSDT_15.ohlcv" \
        --plot "${STRATEGIES_DIR}/_indicators/canonical_pyne.csv" >/dev/null
fi

# --- 4) run canonical indicators through PineTS ----------------------

if [[ "${SKIP_PINETS:-0}" != "1" ]]; then
    log "running canonical indicators through PineTS"
    (cd "${BENCH_DIR}" && node runners/run_pinets_canonical.mjs >/dev/null)
fi

# --- 5) build + run PineForge canonical indicator runner -------------

CANON_BIN="${BENCH_DIR}/runners/run_pineforge_canonical"
if [[ ! -x "${CANON_BIN}" || "${BENCH_DIR}/runners/run_pineforge_canonical.cpp" -nt "${CANON_BIN}" ]]; then
    log "building PineForge canonical indicator runner"
    c++ -std=c++17 -O2 -I "${ROOT_DIR}/include" \
        "${BENCH_DIR}/runners/run_pineforge_canonical.cpp" \
        -L "${ROOT_DIR}/build/lib" \
        -Wl,-force_load,"${ROOT_DIR}/build/lib/libpineforge.a" \
        -o "${CANON_BIN}"
fi
log "running canonical indicators through PineForge"
# Prefer the extended OHLCV (matches the trade-list comparison feed);
# the C++ binary takes input and output CSV paths.
(cd "${BENCH_DIR}" && "${CANON_BIN}" \
    "${WORKDIR}/data/ETHUSDT_15.csv" \
    "${STRATEGIES_DIR}/_indicators/canonical_pineforge.csv" >/dev/null)

# --- 6) speed sweep ---------------------------------------------------

if [[ "${SKIP_SPEED:-0}" != "1" ]]; then
    log "running per-strategy speed sweep"
    if [[ ! -f "${ROOT_DIR}/build/CMakeCache.txt" ]] \
       || ! grep -q "PINEFORGE_BUILD_SPEED_BENCH:BOOL=ON" "${ROOT_DIR}/build/CMakeCache.txt"; then
        log "configuring with -DPINEFORGE_BUILD_SPEED_BENCH=ON"
        cmake -B "${ROOT_DIR}/build" -DPINEFORGE_BUILD_SPEED_BENCH=ON -DPINEFORGE_BUILD_TESTS=ON >/dev/null
    fi
    cmake --build "${ROOT_DIR}/build" --target pineforge_bench -j >/dev/null \
        || fail "speed harness build failed (configure with -DPINEFORGE_BUILD_SPEED_BENCH=ON)"
    "${ROOT_DIR}/build/bin/pineforge_bench" \
        --benchmark_format=json > "${WORKDIR}/pf_speed.json"
    (cd "${BENCH_DIR}" && uv run python speed/time_pynecore.py) > "${WORKDIR}/pc_speed.json" 2>"${WORKDIR}/pc_speed.err"
    (cd "${BENCH_DIR}" && node speed/time_pinets.mjs) > "${WORKDIR}/pt_speed.json" 2>"${WORKDIR}/pt_speed.err"
    (cd "${BENCH_DIR}" && uv run python speed/aggregate.py \
        --pineforge "${WORKDIR}/pf_speed.json" \
        --pynecore  "${WORKDIR}/pc_speed.json" \
        --pinets    "${WORKDIR}/pt_speed.json")
fi

# --- 7) reports -------------------------------------------------------

if [[ "${SKIP_REPORTS:-0}" != "1" ]]; then
    log "generating trade-list comparison"
    (cd "${BENCH_DIR}" && python3 compare.py)
    log "generating indicator comparison"
    (cd "${BENCH_DIR}" && python3 compare_indicators.py)
fi

log "done."
log "results: ${BENCH_DIR}/results/{summary,trade_comparison,indicator_comparison}.md"
