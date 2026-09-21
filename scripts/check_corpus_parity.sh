#!/usr/bin/env bash
# scripts/check_corpus_parity.sh — the TradingView parity gate, in one command.
#
# No scripts/ci_verify.py profile builds a single corpus strategy
# (PINEFORGE_BUILD_CORPUS_STRATEGIES is OFF in every one), so this is the only
# thing in the repository that re-derives the engine's TradingView parity and
# judges it. It is what .github/workflows/corpus-parity.yml runs.
#
#   1. check the corpus submodule out AT the gitlink this repository records,
#      and refuse to start if a pinned INPUT under validation/ is modified;
#   2. build the runtime and all 312 corpus strategies and re-run every one
#      (scripts/run_corpus.sh);
#   3. byte-identity: every engine_trades.csv must hash to what
#      scripts/corpus_parity_baseline.txt pins, with the first differing rows
#      printed for a probe that moved (scripts/corpus_trades_identity.py);
#   4. tiers: scripts/verify_corpus.py must still print its pinned headline.
#
# The baseline, not the corpus tape, is the byte oracle: pineforge-corpus
# 442d497 re-transpiled every generated.cpp without re-running the tapes, so
# its committed engine_trades.csv are an older harness generation (see
# scripts/corpus_trades_identity.py). The tape is still the locator, and its
# header is still enforced.
#
# Environment:
#   BUILD_DIR        CMake build directory      (default: build-corpus-parity)
#   JOBS             parallel build/run jobs    (default: nproc, or 4)
#   DIFF_FILES       drifted probes to expand   (default: 5)
#   DIFF_LINES       lines per drifted probe    (default: 20)
#   EXPECTED_VERIFY  the pinned verifier headline
#   SKIP_BUILD=1     reuse an existing BUILD_DIR (developer loop only)
#   SKIP_RUN=1       judge the trades already on disk (developer loop only)
#
# Exit codes: 0 parity holds, 1 parity drifted, 2 the check could not run.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build-corpus-parity}"
if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
else
    JOBS="${JOBS:-4}"
fi
DIFF_FILES="${DIFF_FILES:-5}"
DIFF_LINES="${DIFF_LINES:-20}"
PY="${PYTHON:-python3}"

# The parity headline this engine is pinned to. Moving it is a deliberate act:
# it means the corpus gitlink moved, and the commit that moves it carries the
# evidence. scripts/verify_corpus.py prints this line verbatim.
EXPECTED_VERIFY="${EXPECTED_VERIFY:-Verified 312 strategies — excellent=311, strong=0, moderate=0, weak=0, minimal=0, anomaly=1, engine_only=0, missing=0}"

log()  { printf '\033[1;34m[corpus-parity]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[corpus-parity]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[corpus-parity]\033[0m %s\n' "$*" >&2; exit 1; }
unrunnable() { printf '\033[1;31m[corpus-parity]\033[0m %s\n' "$*" >&2; exit 2; }

# --- 1) the corpus, at the gitlink this repository records -------------

GITLINK="$(git rev-parse HEAD:corpus 2>/dev/null || true)"
if [[ ! "$GITLINK" =~ ^[0-9a-f]{40}$ ]]; then
    unrunnable "HEAD pins no corpus submodule (git rev-parse HEAD:corpus)"
fi
log "recorded corpus gitlink: $GITLINK"

if [[ ! -f corpus/CMakeLists.txt ]] || [[ "$(git -C corpus rev-parse HEAD)" != "$GITLINK" ]]; then
    log "checking the corpus out at the gitlink"
    git submodule update --init corpus
fi
HAVE="$(git -C corpus rev-parse HEAD)"
if [[ "$HAVE" != "$GITLINK" ]]; then
    unrunnable "corpus is at $HAVE, not at the recorded gitlink $GITLINK"
fi

# The sweep's INPUTS must be the pinned ones: generated.cpp, strategy.pine,
# tv_trades.csv, inputs.json. engine_trades.csv is the sweep's OUTPUT and is
# expected to be modified after any previous run, so it is excluded here and
# judged in step 3. data/ is excluded too: the Git-LFS chart feed is routinely
# stat-dirty in a working checkout.
if [[ "${SKIP_RUN:-0}" != "1" ]]; then
    DIRTY_INPUTS="$(git -C corpus status --porcelain -- validation \
                      | grep -v "/engine_trades\.csv$" || true)"
    if [[ -n "$DIRTY_INPUTS" ]]; then
        warn "pinned corpus inputs are modified; the sweep would not measure the pin:"
        printf '%s\n' "$DIRTY_INPUTS" | head -n "$DIFF_LINES" >&2
        unrunnable "restore them first: git -C corpus checkout -- validation"
    fi
fi

# --- 2) build every corpus strategy and re-run all of them -------------

log "running the corpus sweep (build_dir=$BUILD_DIR, jobs=$JOBS)"
BUILD_DIR="$BUILD_DIR" JOBS="$JOBS" \
    SKIP_BUILD="${SKIP_BUILD:-0}" SKIP_RUN="${SKIP_RUN:-0}" SKIP_VERIFY=1 \
    ./scripts/run_corpus.sh

status=0

# --- 3) byte-identity against the recorded baseline --------------------

log "byte-identity against scripts/corpus_parity_baseline.txt"
identity_rc=0
"$PY" scripts/corpus_trades_identity.py --corpus corpus \
    --files "$DIFF_FILES" --lines "$DIFF_LINES" || identity_rc=$?
if (( identity_rc == 2 )); then
    unrunnable "the byte-identity check could not run"
fi
if (( identity_rc != 0 )); then
    status=1
fi

# --- 4) the verifier headline ------------------------------------------

log "verifying TradingView parity"
verify_log="$BUILD_DIR/verify_corpus.log"
mkdir -p "$BUILD_DIR"
verify_rc=0
"$PY" scripts/verify_corpus.py --all --quiet > "$verify_log" 2>&1 || verify_rc=$?
headline="$(grep -m1 '^Verified ' "$verify_log" || true)"
printf '%s\n' "$headline"
if [[ "$headline" != "$EXPECTED_VERIFY" ]]; then
    warn "the verifier headline moved"
    warn "  expected: $EXPECTED_VERIFY"
    warn "  actual:   ${headline:-<no headline; see $verify_log>}"
    tail -n "$DIFF_LINES" "$verify_log" >&2
    status=1
fi
if (( verify_rc != 0 )); then
    warn "scripts/verify_corpus.py exited $verify_rc"
    tail -n "$DIFF_LINES" "$verify_log" >&2
    status=1
fi

if (( status != 0 )); then
    fail "TradingView parity DRIFTED at corpus $GITLINK"
fi
log "TradingView parity holds at corpus $GITLINK"
