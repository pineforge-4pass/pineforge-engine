#!/usr/bin/env bash
# scripts/regen_corpus_cpp.sh — regenerate (or verify) every corpus
# generated.cpp straight from strategy.pine, using the exact paired codegen
# commit mounted read-only into an immutable pineforge-release Python image.
# Docker and Git are the only dependencies — no host Python, pip, or C++
# toolchain is needed for this step.
#
# This closes the reproducibility loop: the shipped corpus/*/*/generated.cpp
# can be re-derived from corpus/*/*/strategy.pine through the public
# pineforge-release image (engine runtime + bundled pineforge-codegen),
# in transpile-only mode. The image's bundled transpiler is deliberately
# shadowed by PYTHONPATH=/codegen, whose HEAD is authenticated below.
#
# Env vars:
#   IMAGE    Immutable Python runtime image used to execute the pinned checkout
#   CODEGEN_DIR  Optional existing clean checkout at CODEGEN_COMMIT. When
#            unset, the script fetches that exact public commit into a temp dir.
#   CODEGEN_REPO  Fetch URL used only when CODEGEN_DIR is unset.
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

IMAGE="${IMAGE:-ghcr.io/pineforge-4pass/pineforge-release@sha256:a69c3700e44868d9657697f43e3f3954a7ed8bde039b55b52ec901538720ac05}"
CODEGEN_COMMIT="66612eda9ea834e872f48dbe3689c500b1e22cb5"
CODEGEN_REPO="${CODEGEN_REPO:-https://github.com/pineforge-4pass/pineforge-codegen-oss.git}"
VERIFY="${VERIFY:-0}"

log()  { printf '\033[1;34m[regen_corpus]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[regen_corpus]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[regen_corpus]\033[0m %s\n' "$*" >&2; exit 1; }

if [[ ! -f "$ROOT_DIR/corpus/CMakeLists.txt" ]]; then
    fail "validation corpus is not checked out (missing corpus/CMakeLists.txt).
Run:  git submodule update --init corpus
(the TV validation corpus is a PUBLIC submodule: https://github.com/pineforge-4pass/pineforge-corpus)"
fi

command -v docker >/dev/null 2>&1 || fail "docker not found on PATH."
command -v git >/dev/null 2>&1 || fail "git not found on PATH."

owned_codegen=0
if [[ -n "${CODEGEN_DIR:-}" ]]; then
    codegen_checkout="$(cd "$CODEGEN_DIR" && pwd)"
else
    codegen_checkout="$(mktemp -d)"
    owned_codegen=1
    git -C "$codegen_checkout" init --quiet
    git -C "$codegen_checkout" remote add origin "$CODEGEN_REPO"
    git -C "$codegen_checkout" fetch --quiet --depth=1 origin "$CODEGEN_COMMIT"
    git -C "$codegen_checkout" checkout --quiet --detach FETCH_HEAD
fi

actual_codegen="$(git -C "$codegen_checkout" rev-parse HEAD)"
[[ "$actual_codegen" == "$CODEGEN_COMMIT" ]] || \
    fail "codegen checkout is $actual_codegen, required $CODEGEN_COMMIT"
[[ -z "$(git -C "$codegen_checkout" status --porcelain)" ]] || \
    fail "codegen checkout has local changes: $codegen_checkout"
[[ -f "$codegen_checkout/pineforge_codegen/__init__.py" ]] || \
    fail "codegen checkout lacks pineforge_codegen package: $codegen_checkout"

tmp_cpp="$(mktemp)"
cleanup() {
    rm -f "$tmp_cpp"
    if [[ "$owned_codegen" == "1" ]]; then rm -rf "$codegen_checkout"; fi
}
trap cleanup EXIT

n=0; drifted=(); failed=()

for pine in corpus/*/*/strategy.pine; do
    [[ -f "$pine" ]] || continue
    strat_dir="$(dirname "$pine")"
    if [[ -n "${ONLY:-}" && "$strat_dir" != *"$ONLY"* ]]; then
        continue
    fi
    n=$((n + 1))

    # Transpile in-container with the authenticated checkout and no network.
    if ! docker run --rm --network=none \
            --entrypoint python3 \
            -e PYTHONPATH=/codegen \
            -v "$codegen_checkout:/codegen:ro" \
            -v "$ROOT_DIR/$pine:/in/strategy.pine:ro" \
            "$IMAGE" -c 'from pathlib import Path; import sys; from pineforge_codegen import transpile; sys.stdout.write(transpile(Path(sys.argv[1]).read_text(), filename="strategy.pine"))' \
            /in/strategy.pine > "$tmp_cpp" 2>/dev/null; then
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

log "done (codegen $CODEGEN_COMMIT)."
