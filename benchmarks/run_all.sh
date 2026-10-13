#!/usr/bin/env bash
# benchmarks/run_all.sh — full three-way engine benchmark in one command.
#
# Builds the runtime (if not already built), runs every benchmark strategy
# through PyneCore + PineTS + PineForge, and produces the comparison
# reports under benchmarks/results/.
#
# Prerequisites:
#   - cmake >= 3.20 (the speed sweep's Google Benchmark target) + a C++17 compiler (clang or gcc)
#   - uv (Python package manager) + Python 3.11+
#   - node >= 20
#   - git submodule update --init benchmarks/assets  (assets data + strategies)
#
# No API keys required. All inputs (OHLCV, strategy.pine, generated.cpp,
# tv_trades.csv, strategy_pyne.py) are committed in the assets submodule.
#
# Honours these env vars:
#   SKIP_BUILD          — skip the runtime + bench-strategy build step
#   SKIP_PYNE           — skip only PyneCore strategy runs and the canonical indicator run
#   PYNECORE_HARNESS_DIR — private harness directory containing runners/run_pynecore.py
#                         and speed/time_pynecore.py (unset = skip PyneCore)
#   SKIP_PINETS         — skip PineTS indicator run
#   SKIP_PINEFORGE      — skip PineForge trade regeneration and its canonical indicator run
#   SKIP_SPEED          — skip the per-strategy speed sweep (pineforge_bench + timers)
#   SKIP_REPORTS        — skip the compare.py / compare_indicators.py step
#   SKIP_VECTORBT       — skip the vectorbt trades + timing (slots shipping strategy_vbt.py)
#   SKIP_INDICATORS     — skip the canonical indicator runs (PineForge, PyneCore, PineTS)
#   JOBS                — parallel PineForge / PyneCore parity runs (default 1; timing never parallelizes further)
#   SLOTS               — only these slot numbers in the parity loops, e.g. "1-50,120" (default: all)
#   QUIET_LOAD_MAX      — before each timing batch, wait until the 1-minute load average is below
#                         this and no cmake --build / ctest / ci_verify runs (poll 60 s, up to
#                         QUIET_WAIT_S, default 14400); unset = record the load and go on.
#                         Loads land in _workdir/speed_loads.tsv and speed.md's header.
#                         Without ps, wait on load alone; quiet is unverified and figures are labelled quiet-unverified.
#
# Exit status: every engine run first removes the slot's previous trade list,
# so a failed run never leaves one behind for compare.py to grade. PineForge is
# the engine under test: a run error, or a strategy library missing beside a
# generated.cpp, stops the script with status 1 before any report is written.
# PyneCore and vectorbt failures are results: compare.py grades them n/a with
# the error, and they do not change the status.
#
# Maintainer-local closed slots (benchmarks/assets-closed/strategies, never
# public) join every loop when that directory exists; without it the run is the
# public assets alone.
#
# Maintenance scripts (refresh OHLCV, add new bench slots, refresh
# strategy_pyne.py, re-emit generated.cpp) are NOT part of this script —
# they are maintainer-only and require closed-source
# dependencies (codegen, PyneSys API key) that public reproducers don't need.

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "${BENCH_DIR}/assets/strategies" ]]; then
    BENCH_ASSETS="${BENCH_DIR}/assets"
else
    BENCH_ASSETS="${BENCH_DIR}"
fi
STRATEGIES_DIR="${BENCH_ASSETS}/strategies"
CLOSED_STRATEGIES_DIR="${BENCH_DIR}/assets-closed/strategies"
STRATEGY_ROOTS=("${STRATEGIES_DIR}")
[[ -d "${CLOSED_STRATEGIES_DIR}" ]] && STRATEGY_ROOTS+=("${CLOSED_STRATEGIES_DIR}")
ROOT_DIR="$(cd "${BENCH_DIR}/.." && pwd)"
WORKDIR="${BENCH_DIR}/_workdir"

cd "${ROOT_DIR}"

log()  { printf '\033[1;34m[bench]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[bench]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[bench]\033[0m %s\n' "$*" >&2; exit 1; }

pynecore_harness_has() {
    [[ -n "${PYNECORE_HARNESS_DIR:-}" \
        && -f "${PYNECORE_HARNESS_DIR:-}/$1" ]]
}
pynecore_available() {
    [[ "${SKIP_PYNE:-0}" != "1" ]] && pynecore_harness_has "$1"
}
skip_pynecore() {
    warn "PyneCore harness not present (set PYNECORE_HARNESS_DIR to the private harness); skipping the PyneCore leg"
}
# Resolve a relative harness directory before timing commands change directory.
if [[ -n "${PYNECORE_HARNESS_DIR:-}" && -d "${PYNECORE_HARNESS_DIR:-}" ]]; then
    PYNECORE_HARNESS_DIR="$(cd "${PYNECORE_HARNESS_DIR}" && pwd)"
fi

if ! compgen -G "${STRATEGIES_DIR}/[0-9]*-*/strategy.pine" >/dev/null; then
    fail "benchmark fixtures missing (expected ${STRATEGIES_DIR}/<NNN-slug>/strategy.pine).

TV-linked strategy folders and OHLCV live in the public benchmarks/assets
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
# the maintainer-only OHLCV fetch script.
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
if { pynecore_harness_has runners/run_pynecore.py || pynecore_harness_has speed/time_pynecore.py; } \
   && { [[ ! -f "${LIVE_CSV%.csv}.ohlcv" ]] \
        || [[ "${LIVE_CSV}" -nt "${LIVE_CSV%.csv}.ohlcv" ]]; }; then
    pyne -w "${WORKDIR}" data convert-from \
        --provider pineforge --symbol ETHUSDT --timezone UTC \
        "${LIVE_CSV}" >/dev/null
fi

# Every slot directory of every root, in slot order (SLOTS narrows it).
in_slots() {
    local n=$((10#${1%%-*})) part lo hi
    [[ -z "${SLOTS:-}" ]] && return 0
    for part in ${SLOTS//,/ }; do
        lo=${part%%-*}; hi=${part##*-}
        (( n >= lo && n <= hi )) && return 0
    done
    return 1
}
slot_dirs() {
    local root s
    for root in "${STRATEGY_ROOTS[@]}"; do
        for s in "${root}"/[0-9]*-*/; do
            [[ -d "$s" ]] || continue
            s="${s%/}"
            in_slots "$(basename "$s")" && printf '%s\n' "$s"
        done
    done
    return 0
}

# One parity run per slot. It removes the slot's previous trade list first; a
# failure leaves <slot>/_<engine>_error.log (its stderr) and no trade list, and
# compare.py reports the slot n/a with that error. A success removes the log.
run_pineforge_one() {
    local s="$1" extra=()
    rm -f "$s/pineforge_trades.csv"
    # No generated.cpp: a codegen transpile error, which compare.py grades n/a.
    [[ -f "$s/generated.cpp" ]] || { rm -f "$s/_pineforge_error.log"; return 0; }
    if [[ ! -f "$s/strategy.dylib" && ! -f "$s/strategy.so" ]]; then
        echo "no strategy library (strategy.dylib / strategy.so): build the bench_strategies target" \
            > "$s/_pineforge_error.log"
        return 0
    fi
    # Per-slot run flags the graded TV run needs (e.g. --allow-trading-before-window).
    [[ -f "$s/run_strategy.args" ]] && read -r -a extra < "$s/run_strategy.args"
    if python3 "${ROOT_DIR}/scripts/run_strategy.py" "$s" \
            --ohlcv "${SNAPSHOT_CSV}" \
            --output "$s/pineforge_trades.csv" ${extra[@]+"${extra[@]}"} \
            >/dev/null 2>"$s/_pineforge_error.log"; then
        rm -f "$s/_pineforge_error.log"
    else
        rm -f "$s/pineforge_trades.csv"
    fi
}
run_pynecore_one() {
    local s="$1"
    if ! pynecore_available runners/run_pynecore.py; then
        if ! pynecore_harness_has runners/run_pynecore.py; then
            skip_pynecore
        fi
        return 0
    fi
    rm -f "$s/pynecore_trades.csv"
    if python3 "${PYNECORE_HARNESS_DIR}/runners/run_pynecore.py" "$s" >/dev/null 2>"$s/_pynecore_error.log"; then
        rm -f "$s/_pynecore_error.log"
    else
        rm -f "$s/pynecore_trades.csv"
    fi
}
export -f run_pineforge_one run_pynecore_one
export -f pynecore_harness_has pynecore_available skip_pynecore warn
# The private harness finds this checkout through PINEFORGE_REPO_ROOT (it no longer sits inside it).
export PINEFORGE_REPO_ROOT="${ROOT_DIR}"
export ROOT_DIR BENCH_DIR SNAPSHOT_CSV PYNECORE_HARNESS_DIR

# --- 3c) regenerate PineForge trades on the extended OHLCV ----------
# Reads strategy.dylib built by cmake --target bench_strategies (step 1).
# Drives scripts/run_strategy.py (engine-level harness) per strategy dir.

if [[ "${SKIP_PINEFORGE:-0}" != "1" ]]; then
    log "regenerating PineForge trades for $(slot_dirs | wc -l | tr -d ' ') strategies (JOBS=${JOBS:-1})"
    slot_dirs | xargs -P "${JOBS:-1}" -I{} bash -c 'run_pineforge_one "$1"' _ {}
    failed=$(slot_dirs | while read -r s; do [[ -f "$s/_pineforge_error.log" ]] && basename "$s"; done || true)
    if [[ -n "${failed}" ]]; then
        fail "PineForge failed on $(wc -l <<<"${failed}" | tr -d ' ') strategies, no report written (each slot's _pineforge_error.log has the error): $(tr '\n' ' ' <<<"${failed}")"
    fi
fi

# --- 3d) run all strategies through PyneCore ------------------------

if pynecore_available runners/run_pynecore.py; then
    log "running $(slot_dirs | wc -l | tr -d ' ') strategies through PyneCore (JOBS=${JOBS:-1})"
    slot_dirs | xargs -P "${JOBS:-1}" -I{} bash -c 'run_pynecore_one "$1"' _ {}
    failed=$(slot_dirs | while read -r s; do [[ -f "$s/_pynecore_error.log" ]] && basename "$s"; done || true)
    if [[ -n "${failed}" ]]; then
        warn "PyneCore runtime failed on $(wc -l <<<"${failed}" | tr -d ' ') strategies: $(tr '\n' ' ' <<<"${failed}")"
    fi
else
    # A skipped leg leaves the committed PyneCore trade lists as they are (they are published results).
    if ! pynecore_harness_has runners/run_pynecore.py; then
        skip_pynecore
    fi
fi
if [[ "${SKIP_INDICATORS:-0}" != "1" ]]; then
    if pynecore_available runners/run_pynecore.py; then
        log "running canonical indicators through PyneCore"
        pyne -w "${WORKDIR}" run \
            "${STRATEGIES_DIR}/_indicators/canonical_pyne.py" \
            "${WORKDIR}/data/ETHUSDT_15.ohlcv" \
            --plot "${STRATEGIES_DIR}/_indicators/canonical_pyne.csv" >/dev/null
    elif ! pynecore_harness_has runners/run_pynecore.py; then
        skip_pynecore
    fi
fi

# --- 3e) vectorbt trades (slots that ship a strategy_vbt.py port) ------

if [[ "${SKIP_VECTORBT:-0}" != "1" ]]; then
    log "writing vectorbt trades for the slots that ship strategy_vbt.py"
    # time_vectorbt.py rewrites every public port's trade list; a port that
    # fails to load or run is left without one.
    rm -f "${STRATEGIES_DIR}"/[0-9]*-*/vectorbt_trades.csv
    (cd "${BENCH_DIR}" && uv run python speed/time_vectorbt.py --write-trades >/dev/null)
fi

# --- 4) run canonical indicators through PineTS ----------------------

if [[ "${SKIP_PINETS:-0}" != "1" && "${SKIP_INDICATORS:-0}" != "1" ]]; then
    log "running canonical indicators through PineTS"
    (cd "${BENCH_DIR}" && node runners/run_pinets_canonical.mjs >/dev/null)
fi

# --- 5) build + run PineForge canonical indicator runner -------------

if [[ "${SKIP_PINEFORGE:-0}" != "1" && "${SKIP_INDICATORS:-0}" != "1" ]]; then
    CANON_BIN="${BENCH_DIR}/runners/run_pineforge_canonical"
    # The runner links libpineforge.a statically: a rebuilt library needs a relink too.
    if [[ ! -x "${CANON_BIN}" || "${BENCH_DIR}/runners/run_pineforge_canonical.cpp" -nt "${CANON_BIN}" \
          || "${ROOT_DIR}/build/lib/libpineforge.a" -nt "${CANON_BIN}" ]]; then
        log "building PineForge canonical indicator runner"
        # -ffp-contract=off as libpineforge and every strategy target: the
        # ta::* arithmetic inlined here must round like TradingView's (no FMA).
        c++ -std=c++17 -O2 -ffp-contract=off -I "${ROOT_DIR}/include" \
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
fi

# --- 6) speed sweep ---------------------------------------------------

# Record the host load before a timing batch (and, with QUIET_LOAD_MAX, wait
# for a quiet host first). One line per batch in _workdir/speed_loads.tsv.
# Without ps, the busy count is unknown and figures are labelled quiet-unverified.
QUIET_UNVERIFIED_NOTICE=""
quiet_gate() {
    local label="$1" waited=0 load busy ps_available
    while :; do
        load=$( (sysctl -n vm.loadavg 2>/dev/null || cat /proc/loadavg) | tr -d '{}' | awk '{print $1}')
        # Build/test processes by executable name (a full-command grep also
        # matches any process whose arguments merely quote these commands).
        if command -v ps >/dev/null 2>&1; then
            ps_available=1
            busy=$(ps -axo comm=,args= | awk '{ c = $1; sub(".*/", "", c)
                if (c == "ctest" || (c == "cmake" && / --build /) || (c ~ /^[Pp]ython/ && /ci_verify\.py/)) n++ }
                END { print n + 0 }')
        else
            ps_available=0
            busy="unknown (ps absent)"
            QUIET_UNVERIFIED_NOTICE="quiet-unverified: no ps on this host, the busy count was not measured; do not use these figures as performance evidence"
        fi
        if [[ -z "${QUIET_LOAD_MAX:-}" ]] \
           || { awk -v l="${load}" -v m="${QUIET_LOAD_MAX}" 'BEGIN { exit !(l < m) }' \
                && [[ "${ps_available}" == 0 || "${busy}" == 0 ]]; }; then
            break
        fi
        (( waited >= ${QUIET_WAIT_S:-14400} )) && fail "host never quiet before ${label}: load ${load}, busy ${busy}"
        sleep 60
        waited=$((waited + 60))
    done
    printf '%s\t%s\t%s\t%s\n' "${label}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${load}" "${busy}" \
        >> "${WORKDIR}/speed_loads.tsv"
    if [[ "${ps_available}" == 0 ]]; then
        log "timing batch ${label}: 1-min load ${load}, busy count ${busy}"
    else
        log "timing batch ${label}: 1-min load ${load}, build/test processes ${busy}"
    fi
}

if [[ "${SKIP_SPEED:-0}" != "1" ]]; then
    log "running per-strategy speed sweep"
    : > "${WORKDIR}/speed_loads.tsv"
    if [[ "${RUNNER:-native}" == "docker" ]]; then
        # SECONDARY path: time PineForge inside the pineforge-release image
        # (run_json.py --bench → raw samples_ns). GBench (native) stays the
        # authoritative source; this is the no-toolchain / ecosystem-parity path.
        log "PineForge speed via pineforge-release image (--bench); GBench stays authoritative"
        (cd "${BENCH_DIR}" && uv run python speed/time_pineforge_docker.py \
            --strategies "${STRATEGIES_DIR}" \
            --ohlcv "${BENCH_DIR}/assets/data/ETHUSDT_15.csv" \
            ${IMAGE:+--image "${IMAGE}"}) > "${WORKDIR}/pf_speed.json" 2>"${WORKDIR}/pf_speed.err"
        PF_FMT=subproc
    else
        if [[ ! -f "${ROOT_DIR}/build/CMakeCache.txt" ]] \
           || ! grep -q "PINEFORGE_BUILD_SPEED_BENCH:BOOL=ON" "${ROOT_DIR}/build/CMakeCache.txt"; then
            log "configuring with -DPINEFORGE_BUILD_SPEED_BENCH=ON"
            cmake -B "${ROOT_DIR}/build" -DPINEFORGE_BUILD_SPEED_BENCH=ON -DPINEFORGE_BUILD_TESTS=ON >/dev/null
        fi
        cmake --build "${ROOT_DIR}/build" --target pineforge_bench -j >/dev/null \
            || fail "speed harness build failed (configure with -DPINEFORGE_BUILD_SPEED_BENCH=ON)"
        quiet_gate pineforge
        "${ROOT_DIR}/build/bin/pineforge_bench" \
            --benchmark_filter='/throughput/with_magnifier' \
            --benchmark_format=json > "${WORKDIR}/pf_speed.json"
        PF_FMT=gbench
    fi
    PC_ARGS=()
    if pynecore_harness_has speed/time_pynecore.py; then
        quiet_gate pynecore
        (cd "${BENCH_DIR}" && uv run python "${PYNECORE_HARNESS_DIR}/speed/time_pynecore.py") \
            > "${WORKDIR}/pc_speed.json" 2>"${WORKDIR}/pc_speed.err"
        PC_ARGS=(--pynecore "${WORKDIR}/pc_speed.json")
    else
        rm -f "${WORKDIR}/pc_speed.json" "${WORKDIR}/pc_speed.err"
        skip_pynecore
        # Without PyneCore the table is a different table: never overwrite the committed results/speed.md with it.
        PC_ARGS=(--out "${WORKDIR}/speed.md")
        log "no PyneCore timings: writing the speed table to ${WORKDIR}/speed.md, not results/speed.md"
    fi
    quiet_gate pinets
    (cd "${BENCH_DIR}" && node speed/time_pinets.mjs) > "${WORKDIR}/pt_speed.json" 2>"${WORKDIR}/pt_speed.err"
    VBT_ARGS=()
    if [[ "${SKIP_VECTORBT:-0}" != "1" ]]; then
        quiet_gate vectorbt
        (cd "${BENCH_DIR}" && uv run python speed/time_vectorbt.py --out "${WORKDIR}/vbt_speed.json") \
            >/dev/null 2>"${WORKDIR}/vbt_speed.err"
        VBT_ARGS=(--vectorbt "${WORKDIR}/vbt_speed.json")
    fi
    # Unverified figures, even with PyneCore timings, must not replace the committed table.
    if [[ -n "${QUIET_UNVERIFIED_NOTICE}" ]] && pynecore_harness_has speed/time_pynecore.py; then
        PC_ARGS+=(--out "${WORKDIR}/speed.md")
    fi
    (cd "${BENCH_DIR}" && uv run python speed/aggregate.py \
        --pineforge "${WORKDIR}/pf_speed.json" \
        --pineforge-format "${PF_FMT}" \
        ${PC_ARGS[@]+"${PC_ARGS[@]}"} \
        --pinets    "${WORKDIR}/pt_speed.json" \
        --loads     "${WORKDIR}/speed_loads.tsv" ${VBT_ARGS[@]+"${VBT_ARGS[@]}"})
    if [[ -n "${QUIET_UNVERIFIED_NOTICE}" ]]; then
        printf '\n%s\n' "${QUIET_UNVERIFIED_NOTICE}" >> "${WORKDIR}/speed.md"
    fi
fi

# --- 7) reports -------------------------------------------------------

if [[ "${SKIP_REPORTS:-0}" != "1" ]]; then
    log "generating trade-list comparison"
    (cd "${BENCH_DIR}" && python3 compare.py)
    if ! pynecore_available runners/run_pynecore.py \
       && [[ ! -f "${STRATEGIES_DIR}/_indicators/canonical_pyne.csv" ]]; then
        warn "PyneCore indicator output not present; skipping the three-way indicator comparison"
    else
        log "generating indicator comparison"
        (cd "${BENCH_DIR}" && python3 compare_indicators.py)
    fi
fi

log "done."
log "results: ${BENCH_DIR}/results/{summary,trade_comparison,indicator_comparison}.md"
if [[ -n "${QUIET_UNVERIFIED_NOTICE}" ]]; then
    printf '%s\n' "${QUIET_UNVERIFIED_NOTICE}"
fi
