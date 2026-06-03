#!/usr/bin/env bash
# scripts/regen_corpus_cpp.sh — regenerate (or verify) every corpus
# generated.cpp straight from strategy.pine, using the bundled transpiler
# in the pineforge-engine Docker image. Docker is the only dependency —
# no host Python, pip, or C++ toolchain needed for this step.
#
# This closes the reproducibility loop: the shipped corpus/*/*/generated.cpp
# can be re-derived from corpus/*/*/strategy.pine through the public engine
# image (which bundles pineforge-codegen in transpile-only mode).
#
# Env vars:
#   IMAGE    Engine image to transpile with
#            (default: ghcr.io/pineforge-4pass/pineforge-engine:latest)
#   ONLY     Substring filter; only process strategies whose path matches
#   VERIFY   1 = do NOT overwrite; transpile to a temp file and diff against
#            the committed generated.cpp. Exit non-zero if any file drifts.
#            0 (default) = regenerate generated.cpp in place.
#
# Examples:
#   scripts/regen_corpus_cpp.sh                       # regenerate all in place
#   VERIFY=1 scripts/regen_corpus_cpp.sh             # drift guard, no writes
#   ONLY=matrix VERIFY=1 scripts/regen_corpus_cpp.sh # just matrix probes
#
# Exit codes:
#   0  success (all regenerated, or VERIFY found no drift)
#   1  VERIFY found drift, or a transpile failed
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

IMAGE="${IMAGE:-ghcr.io/pineforge-4pass/pineforge-engine:latest}"
VERIFY="${VERIFY:-0}"

log()  { printf '\033[1;34m[regen_corpus]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[regen_corpus]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[regen_corpus]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ ! -f "$ROOT_DIR/corpus/CMakeLists.txt" ]]; then
    fail "validation corpus is not checked out (missing corpus/CMakeLists.txt).
Maintainers:   git submodule update --init corpus
Public clones: the TV validation corpus lives in a private submodule only; see CONTRIBUTING.md."
fi

command -v docker >/dev/null 2>&1 || fail "docker not found on PATH."

tmp_cpp="$(mktemp)"
trap 'rm -f "$tmp_cpp"' EXIT

n=0; drifted=(); failed=()

for pine in corpus/*/*/strategy.pine; do
    [[ -f "$pine" ]] || continue
    strat_dir="$(dirname "$pine")"
    if [[ -n "${ONLY:-}" && "$strat_dir" != *"$ONLY"* ]]; then
        continue
    fi
    n=$((n + 1))

    # Transpile in-container (transpile-only, no network). stdout = C++.
    if ! docker run --rm --network=none \
            -e PINEFORGE_TRANSPILE_ONLY=1 \
            -v "$ROOT_DIR/$pine:/in/strategy.pine:ro" \
            "$IMAGE" > "$tmp_cpp" 2>/dev/null; then
        warn "transpile failed: $strat_dir"
        failed+=("$strat_dir")
        continue
    fi

    target="$strat_dir/generated.cpp"
    if [[ "$VERIFY" == "1" ]]; then
        if [[ ! -f "$target" ]] || ! diff -q "$tmp_cpp" "$target" >/dev/null 2>&1; then
            warn "drift: $target"
            drifted+=("$strat_dir")
        fi
    else
        cp "$tmp_cpp" "$target"
    fi
done

if [[ "$VERIFY" == "1" ]]; then
    log "verified $n strategies (${#drifted[@]} drifted, ${#failed[@]} failed to transpile)"
else
    log "regenerated $n strategies (${#failed[@]} failed to transpile)"
fi

if (( ${#failed[@]} > 0 )); then
    warn "transpile failures:"; for f in "${failed[@]}"; do warn "  $f"; done
    exit 1
fi
if [[ "$VERIFY" == "1" && ${#drifted[@]} -gt 0 ]]; then
    warn "drifted strategies (committed generated.cpp != transpiler output):"
    for d in "${drifted[@]}"; do warn "  $d"; done
    exit 1
fi

log "done."
