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
private monorepo checkouts — do not publish those paths in public history."
fi

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

# OHLCV: prefer the committed snapshot at benchmarks/assets/data/ (submodule)
# or benchmarks/data/ (legacy) over a
# fresh Binance fetch. The committed file is what every committed
# strategy_pyne.py and pineforge_trades.csv was generated against —
# fetching live would silently drift the comparison.
#
# To force a fresh download from Binance, set REFRESH_OHLCV=1.
source "${BENCH_DIR}/.venv/bin/activate"
mkdir -p "${WORKDIR}/data"
SNAPSHOT_CSV="${BENCH_ASSETS}/data/ETHUSDT_15.csv"
LIVE_CSV="${WORKDIR}/data/ETHUSDT_15.csv"

if [[ "${REFRESH_OHLCV:-0}" == "1" ]]; then
    log "REFRESH_OHLCV=1: re-fetching from Binance ETH/USDT:USDT 15m"
    python3 "${BENCH_DIR}/runners/fetch_extended_ohlcv.py" \
        --since "${OHLCV_SINCE:-2025-03-01}" >/dev/null
    cp "${LIVE_CSV}" "${SNAPSHOT_CSV}"
    log "wrote refreshed snapshot to benchmarks/assets/data/ETHUSDT_15.csv"
elif [[ -f "${SNAPSHOT_CSV}" ]]; then
    if [[ ! -f "${LIVE_CSV}" ]] || ! cmp -s "${SNAPSHOT_CSV}" "${LIVE_CSV}"; then
        log "using OHLCV snapshot at benchmarks/assets/data/ETHUSDT_15.csv"
        cp "${SNAPSHOT_CSV}" "${LIVE_CSV}"
        # Re-convert to PyneCore .ohlcv if missing or stale.
        if [[ ! -f "${LIVE_CSV%.csv}.ohlcv" ]] \
           || [[ "${LIVE_CSV}" -nt "${LIVE_CSV%.csv}.ohlcv" ]]; then
            pyne -w "${WORKDIR}" data convert-from \
                --provider pineforge --symbol ETHUSDT --timezone UTC \
                "${LIVE_CSV}" >/dev/null
        fi
    fi
elif [[ ! -f "${LIVE_CSV}" ]]; then
    warn "no OHLCV snapshot at benchmarks/assets/data/ETHUSDT_15.csv"
    warn "init the benchmarks/assets submodule, or run with REFRESH_OHLCV=1"
    warn "set REFRESH_OHLCV=1 to fetch from Binance instead"
    exit 1
fi

# --- 3a) bootstrap strategy folders + cloud-compile -----------------

if [[ "${SKIP_BOOTSTRAP:-0}" != "1" ]]; then
    log "bootstrapping benchmark strategy folders from corpus/"
    python3 "${BENCH_DIR}/runners/bootstrap_strategies.py" >/dev/null
fi

if [[ "${SKIP_COMPILE:-0}" != "1" ]]; then
    log "cloud-compiling .pine -> @pyne via PyneSys (skip if strategy_pyne.py exists)"
    force_compile=0
    if [[ "${REFRESH_COMPILE:-0}" == "1" ]]; then
        force_compile=1
        log "  REFRESH_COMPILE=1: will re-call API for every strategy"
    fi
    needs_key=0
    if (( force_compile == 1 )); then
        needs_key=1
    else
        # Even when not forcing, we still need a key if any strategy is
        # missing its committed strategy_pyne.py.
        for s in "${STRATEGIES_DIR}"/[0-9][0-9]-*/; do
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
        if (( force_compile == 1 )); then
            python3 "${BENCH_DIR}/runners/cloud_compile.py" --force >/dev/null \
                || warn "cloud_compile.py reported failures; continuing with whatever exists"
        else
            python3 "${BENCH_DIR}/runners/cloud_compile.py" >/dev/null \
                || warn "cloud_compile.py reported failures; continuing with whatever exists"
        fi
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
