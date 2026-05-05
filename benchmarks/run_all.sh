#!/usr/bin/env bash
# benchmarks/run_all.sh — full three-way engine benchmark in one command.
#
# Builds the runtime (if not already built), runs every benchmark strategy
# through PyneCore + PineTS + PineForge, and produces the comparison
# reports under benchmarks/results/.
#
# Honours these env vars:
#   SKIP_BUILD     — skip the runtime build step
#   SKIP_PYNE      — skip PyneCore strategy + indicator runs
#   SKIP_PINETS    — skip PineTS indicator run
#   SKIP_REPORTS   — skip the compare.py / compare_indicators.py step

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${BENCH_DIR}/.." && pwd)"
WORKDIR="${BENCH_DIR}/_workdir"

cd "${ROOT_DIR}"

log()  { printf '\033[1;34m[bench]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[bench]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[bench]\033[0m %s\n' "$*" >&2; exit 1; }

# --- 1) build runtime ------------------------------------------------

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    log "configuring + building libpineforge"
    cmake -B build -DPINEFORGE_BUILD_TESTS=ON >/dev/null
    cmake --build build --target pineforge -j >/dev/null
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

# Fetch extended OHLCV from Binance USDT-M futures (covers full TV
# trade-export time range with ~30-day pre-roll for warmup).
source "${BENCH_DIR}/.venv/bin/activate"
if [[ ! -f "${WORKDIR}/data/ETHUSDT_15.ohlcv" || "${REFRESH_OHLCV:-0}" == "1" ]]; then
    log "fetching extended OHLCV (Binance ETH/USDT:USDT 15m, since 2025-03-01)"
    mkdir -p "${WORKDIR}/data"
    python3 "${BENCH_DIR}/runners/fetch_extended_ohlcv.py" \
        --since "${OHLCV_SINCE:-2025-03-01}" >/dev/null \
        || warn "OHLCV fetch failed; falling back to corpus copy"
    if [[ ! -f "${WORKDIR}/data/ETHUSDT_15.csv" ]]; then
        cp "${ROOT_DIR}/corpus/data/ohlcv_ETH-USDT-USDT_15m.csv" \
           "${WORKDIR}/data/ETHUSDT_15.csv"
        pyne -w "${WORKDIR}" data convert-from \
            --provider pineforge --symbol ETHUSDT --timezone UTC \
            "${WORKDIR}/data/ETHUSDT_15.csv" >/dev/null
    fi
fi

# --- 3a) bootstrap strategy folders + cloud-compile -----------------

if [[ "${SKIP_BOOTSTRAP:-0}" != "1" ]]; then
    log "bootstrapping benchmark strategy folders from corpus/"
    python3 "${BENCH_DIR}/runners/bootstrap_strategies.py" >/dev/null
fi

if [[ "${SKIP_COMPILE:-0}" != "1" ]]; then
    log "cloud-compiling .pine -> @pyne via PyneSys (skip if strategy_pyne.py exists)"
    compile_args=()
    if [[ "${REFRESH_COMPILE:-0}" == "1" ]]; then
        compile_args+=("--force")
        log "  REFRESH_COMPILE=1: will re-call API for every strategy"
    fi
    needs_key=0
    if (( ${#compile_args[@]} > 0 )); then
        needs_key=1
    else
        # Even when not forcing, we still need a key if any strategy is
        # missing its committed strategy_pyne.py.
        for s in "${BENCH_DIR}"/strategies/[0-9][0-9]-*/; do
            [[ -f "$s/strategy.pine" ]] || continue
            [[ -f "$s/strategy_pyne.py" ]] || { needs_key=1; break; }
        done
    fi
    if (( needs_key == 1 )) \
       && [[ -z "${PYNESYS_API_KEY:-}" ]] \
       && ! grep -q '^api_key = "[^"]\+' "${WORKDIR}/config/api.toml" 2>/dev/null; then
        warn "missing strategy_pyne.py and no PyneSys API key found"
        warn "set PYNESYS_API_KEY=... or fill ${WORKDIR}/config/api.toml"
        warn "skipping compile; benchmark will run only on existing files"
    else
        python3 "${BENCH_DIR}/runners/cloud_compile.py" "${compile_args[@]}" >/dev/null \
            || warn "cloud_compile.py reported failures; continuing with whatever exists"
    fi
fi

# --- 3c) regenerate PineForge trades on the extended OHLCV ----------

if [[ "${SKIP_PINEFORGE:-0}" != "1" ]]; then
    log "regenerating PineForge trades against extended OHLCV"
    python3 "${BENCH_DIR}/runners/regenerate_pineforge_trades.py" >/dev/null \
        || warn "regenerate_pineforge_trades.py reported failures; continuing"
fi

# --- 3b) run all strategies through PyneCore ------------------------

if [[ "${SKIP_PYNE:-0}" != "1" ]]; then
    n_strats=$(ls -d "${BENCH_DIR}"/strategies/[0-9][0-9]-*/ 2>/dev/null | wc -l | tr -d ' ')
    log "running ${n_strats} strategies through PyneCore"
    failed=()
    for s in "${BENCH_DIR}"/strategies/[0-9][0-9]-*/; do
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
        "${BENCH_DIR}/strategies/_indicators/canonical_pyne.py" \
        "${WORKDIR}/data/ETHUSDT_15.ohlcv" \
        --plot "${BENCH_DIR}/strategies/_indicators/canonical_pyne.csv" >/dev/null
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
# the C++ binary takes the input CSV as its first arg.
(cd "${BENCH_DIR}" && "${CANON_BIN}" "${WORKDIR}/data/ETHUSDT_15.csv" >/dev/null)

# --- 6) reports -------------------------------------------------------

if [[ "${SKIP_REPORTS:-0}" != "1" ]]; then
    log "generating trade-list comparison"
    (cd "${BENCH_DIR}" && python3 compare.py)
    log "generating indicator comparison"
    (cd "${BENCH_DIR}" && python3 compare_indicators.py)
fi

log "done."
log "results: ${BENCH_DIR}/results/{summary,trade_comparison,indicator_comparison}.md"
