#!/usr/bin/env bash
# scripts/run_corpus.sh — end-to-end reproducibility driver.
#
# 1. Configure & build the runtime + every corpus strategy as a .so.
# 2. Run each strategy through the Python harness against the reference
#    OHLCV feed; write engine_trades.csv next to each strategy.so.
# 3. Verify TV parity with scripts/verify_corpus.py.
#
# Honours these env vars:
#   BUILD_DIR     — CMake build directory (default: build)
#   BUILD_TYPE    — Release | Debug | RelWithDebInfo (default: Release)
#   JOBS          — parallel build/run jobs (default: $(nproc) or 4)
#   ONLY          — substring filter; only run strategies whose path matches
#   SKIP_BUILD    — set to 1 to reuse an existing build directory
#   SKIP_RUN      — set to 1 to skip the Python harness pass
#   SKIP_VERIFY   — set to 1 to skip the parity verifier

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
else
    JOBS="${JOBS:-4}"
fi

PY="${PYTHON:-python3}"

log()  { printf '\033[1;34m[run_corpus]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[run_corpus]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[run_corpus]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ ! -f "$ROOT_DIR/corpus/CMakeLists.txt" ]]; then
    fail "validation corpus is not checked out (missing corpus/CMakeLists.txt).
Maintainers:   git submodule update --init corpus
Public clones: the TV validation corpus lives in a private submodule only; see CONTRIBUTING.md."
fi

# --- 0) (optional) regenerate generated.cpp from strategy.pine --------
# REGEN=1 re-derives every corpus/*/*/generated.cpp from its strategy.pine
# through the pineforge-release Docker image (engine + bundled transpiler), so
# the build below compiles freshly-transpiled C++ instead of the committed copy.
# Requires Docker. Honours ONLY. Off by default → committed C++ is used.

if [[ "${REGEN:-0}" == "1" ]]; then
    log "regenerating generated.cpp from strategy.pine via the pineforge-release image"
    "$ROOT_DIR/scripts/regen_corpus_cpp.sh"
fi

# --- 1) build ---------------------------------------------------------

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    log "configuring CMake (build_type=$BUILD_TYPE, dir=$BUILD_DIR)"
    cmake -B "$BUILD_DIR" -S . \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DPINEFORGE_BUILD_TESTS=ON \
        -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON \
        -Wno-dev

    log "building runtime + 246 strategy targets ($JOBS jobs)"
    cmake --build "$BUILD_DIR" --target corpus_strategies -j "$JOBS"
fi

# --- 2) run -----------------------------------------------------------

if [[ "${SKIP_RUN:-0}" != "1" ]]; then
    log "running every strategy.so against the reference OHLCV feed"
    n_ok=0
    n_fail=0
    failed=()
    started=$(date +%s)

    for strat_dir in corpus/*/*/; do
        strat_dir="${strat_dir%/}"
        [[ -f "$strat_dir/strategy.so" || -f "$strat_dir/strategy.dylib" || -f "$strat_dir/strategy.dll" ]] || continue
        if [[ -n "${ONLY:-}" && "$strat_dir" != *"$ONLY"* ]]; then
            continue
        fi

        # Pick whichever shared lib extension actually exists for this platform.
        if [[ -f "$strat_dir/strategy.dylib" ]]; then
            so_name="strategy.dylib"
        elif [[ -f "$strat_dir/strategy.dll" ]]; then
            so_name="strategy.dll"
        else
            so_name="strategy.so"
        fi

        if "$PY" scripts/run_strategy.py "$strat_dir" --so-name "$so_name" >/dev/null 2>&1; then
            n_ok=$((n_ok + 1))
        else
            n_fail=$((n_fail + 1))
            failed+=("$strat_dir")
        fi
    done

    elapsed=$(( $(date +%s) - started ))
    log "ran $((n_ok + n_fail)) strategies in ${elapsed}s -- ok=$n_ok fail=$n_fail"
    if (( n_fail > 0 )); then
        warn "failed strategies:"
        for f in "${failed[@]}"; do
            warn "  $f"
        done
    fi
fi

# --- 3) verify --------------------------------------------------------

if [[ "${SKIP_VERIFY:-0}" != "1" ]]; then
    log "printing corpus parity inspection summary"
    if ! "$PY" scripts/verify_corpus.py --all --quiet; then
        warn "corpus inspection reported drift above this helper's strict thresholds."
        warn "This helper is not the canonical parity sweep; see corpus/README.md."
    fi
fi

log "done."
