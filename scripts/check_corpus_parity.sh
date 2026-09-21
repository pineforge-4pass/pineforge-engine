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
# --subset — THE HALF A PULL REQUEST CAN WAIT FOR. The whole sweep is ~32 min,
# so it cannot hold a merge (docs/ci.md). With --subset this builds and re-runs
# only the 30 probes scripts/corpus_parity_subset.txt names, in parallel, and
# judges them against the SAME pinned sha256 rows. Measured on a 16-core laptop
# at JOBS=8, corpus 442d497, under sibling load: derive 2 s (a no-op when the
# feeds are fresh), build the runtime + the 30 strategy .so 68 s from clean,
# run 45 s wall for 80 s of probe CPU, judge <1 s — 94 s end to end over an
# up-to-date build directory, against 1792 s for the run phase alone in full
# mode. That fits the ~25 min a required check is budgeted for, which is why
# .github/workflows/ci.yml can make it a dependency of the required `build`
# context.
#
# What --subset does NOT prove: the other 282 probes, and the tier headline
# (scripts/verify_corpus.py grades the whole population, so 30 runs cannot
# print its line). Both stay with the nightly full sweep. The subset is a
# blocking floor, not a replacement.
#
# Environment:
#   BUILD_DIR        CMake build directory      (default: build-corpus-parity)
#   BUILD_TYPE       CMake build type           (default: Release)
#   JOBS             parallel build/run jobs    (default: nproc, or 4)
#   DIFF_FILES       drifted probes to expand   (default: 5)
#   DIFF_LINES       lines per drifted probe    (default: 20)
#   EXPECTED_VERIFY  the pinned verifier headline
#   SUBSET_FILE      the probe list --subset reads
#                    (default: scripts/corpus_parity_subset.txt)
#   SKIP_BUILD=1     reuse an existing BUILD_DIR (developer loop only)
#   SKIP_RUN=1       judge the trades already on disk (developer loop only)
#
# Exit codes: 0 parity holds, 1 parity drifted, 2 the check could not run.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE=full
while (( $# > 0 )); do
    case "$1" in
        --subset) MODE=subset ;;
        -h|--help) sed -n '2,52p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) printf 'check_corpus_parity: unknown argument %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

BUILD_DIR="${BUILD_DIR:-build-corpus-parity}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
else
    JOBS="${JOBS:-4}"
fi
DIFF_FILES="${DIFF_FILES:-5}"
DIFF_LINES="${DIFF_LINES:-20}"
SUBSET_FILE="${SUBSET_FILE:-scripts/corpus_parity_subset.txt}"
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

# --- 2) build every corpus strategy (or the subset) and re-run them ----

if [[ "$MODE" == full ]]; then
    log "running the corpus sweep (build_dir=$BUILD_DIR, jobs=$JOBS)"
    BUILD_DIR="$BUILD_DIR" JOBS="$JOBS" \
        SKIP_BUILD="${SKIP_BUILD:-0}" SKIP_RUN="${SKIP_RUN:-0}" SKIP_VERIFY=1 \
        ./scripts/run_corpus.sh
else
    if [[ ! -f "$SUBSET_FILE" ]]; then
        unrunnable "no subset list at $SUBSET_FILE"
    fi
    # One corpus-relative probe directory per line, '#' starts a comment. The
    # spelling is checked here and not only by the identity checker: these
    # names become CMake target names and shell words below.
    PROBES=()
    while IFS= read -r line; do
        entry="${line%%#*}"
        entry="$(printf '%s' "$entry" | tr -d '[:space:]')"
        [[ -z "$entry" ]] && continue
        if [[ ! "$entry" =~ ^validation/[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
            unrunnable "$SUBSET_FILE names an unusable probe: $entry"
        fi
        if [[ ! -f "corpus/$entry/generated.cpp" ]]; then
            unrunnable "$SUBSET_FILE names $entry, which the corpus does not build"
        fi
        PROBES+=("$entry")
    done < "$SUBSET_FILE"
    if (( ${#PROBES[@]} == 0 )); then
        unrunnable "$SUBSET_FILE names no probe"
    fi
    log "subset parity over ${#PROBES[@]} probes from $SUBSET_FILE (build_dir=$BUILD_DIR, jobs=$JOBS)"

    # The derived 15m chart feeds are rebuilt from the committed 1m Git-LFS
    # feed. Do it ONCE, before anything fans out: a rebuild writes through a
    # temporary and renames, so two probes materializing at the same time race
    # (corpus/CLAUDE.md, "Concurrency"). Fresh, this is a no-op.
    if [[ "${SKIP_RUN:-0}" != "1" ]]; then
        log "materializing derived corpus feeds"
        "$PY" scripts/derive_corpus_feeds.py
    fi

    if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
        log "configuring CMake (build_type=$BUILD_TYPE, dir=$BUILD_DIR)"
        cmake -B "$BUILD_DIR" -S . \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DPINEFORGE_BUILD_TESTS=ON \
            -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON \
            -Wno-dev
        # corpus/CMakeLists.txt derives one target per probe directory:
        # validation/<probe> -> strategy_validation_<probe with - as _>.
        TARGETS=()
        for probe in "${PROBES[@]}"; do
            TARGETS+=("strategy_$(printf '%s' "$probe" | tr '/-' '__')")
        done
        log "building the runtime + ${#TARGETS[@]} strategy libraries ($JOBS jobs)"
        cmake --build "$BUILD_DIR" -j "$JOBS" --target "${TARGETS[@]}"
    fi

    if [[ "${SKIP_RUN:-0}" != "1" ]]; then
        # Which shared-library extension this platform produced.
        SO_NAME=strategy.so
        if [[ -f "corpus/${PROBES[0]}/strategy.dylib" ]]; then
            SO_NAME=strategy.dylib
        elif [[ -f "corpus/${PROBES[0]}/strategy.dll" ]]; then
            SO_NAME=strategy.dll
        fi
        RUN_LOGS="$BUILD_DIR/subset-run-logs"
        rm -rf "$RUN_LOGS"
        mkdir -p "$RUN_LOGS"
        log "re-running the subset ($SO_NAME, $JOBS at a time)"
        started=$(date +%s)
        # The probes write into disjoint directories and share only the
        # read-only feeds, so the run phase parallelises exactly; the full
        # sweep's serial loop is what puts it out of a pull-request budget.
        run_rc=0
        # One probe per child, passed as $1 -- NOT through xargs -I, whose
        # replacement string is capped at 255 bytes on BSD xargs ("command
        # line cannot be assembled, too long").
        # SC2016: the child's body is deliberately unexpanded here — PF_PY,
        # PF_SO and PF_LOGS are read from its own environment, exported above.
        # shellcheck disable=SC2016
        printf '%s\n' "${PROBES[@]}" \
            | PF_PY="$PY" PF_SO="$SO_NAME" PF_LOGS="$RUN_LOGS" \
              xargs -P "$JOBS" -n 1 sh -c '
                  probe="$1"
                  log="$PF_LOGS/$(printf %s "$probe" | tr / _).log"
                  if ! "$PF_PY" scripts/run_strategy.py "corpus/$probe" \
                          --so-name "$PF_SO" > "$log" 2>&1; then
                      printf "FAILED %s\n" "$probe" >&2
                      cat "$log" >&2
                      exit 1
                  fi' corpus-parity-subset || run_rc=$?
        elapsed=$(( $(date +%s) - started ))
        log "ran ${#PROBES[@]} probes in ${elapsed}s"
        if (( run_rc != 0 )); then
            unrunnable "a subset probe failed to run; refusing to judge stale trades"
        fi
    fi
fi

status=0

# --- 3) byte-identity against the recorded baseline --------------------

log "byte-identity against scripts/corpus_parity_baseline.txt"
identity_rc=0
if [[ "$MODE" == subset ]]; then
    "$PY" scripts/corpus_trades_identity.py --corpus corpus \
        --subset "$SUBSET_FILE" \
        --files "$DIFF_FILES" --lines "$DIFF_LINES" || identity_rc=$?
else
    "$PY" scripts/corpus_trades_identity.py --corpus corpus \
        --files "$DIFF_FILES" --lines "$DIFF_LINES" || identity_rc=$?
fi
if (( identity_rc == 2 )); then
    unrunnable "the byte-identity check could not run"
fi
if (( identity_rc != 0 )); then
    status=1
fi

# --- 4) the verifier headline ------------------------------------------

if [[ "$MODE" == subset ]]; then
    # scripts/verify_corpus.py grades the whole population and prints one
    # headline for it; 30 re-runs cannot produce that line, and grading the
    # 282 stale tapes beside them would judge the corpus's own generation,
    # not this engine. The tier gate stays with the nightly full sweep.
    log "subset mode: the tier headline is the nightly sweep's, not judged here"
else
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
fi

if (( status != 0 )); then
    if [[ "$MODE" == subset ]]; then
        fail "TradingView parity DRIFTED on the subset at corpus $GITLINK"
    fi
    fail "TradingView parity DRIFTED at corpus $GITLINK"
fi
if [[ "$MODE" == subset ]]; then
    log "TradingView parity holds on the ${#PROBES[@]}-probe subset at corpus $GITLINK"
else
    log "TradingView parity holds at corpus $GITLINK"
fi
