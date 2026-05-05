#!/usr/bin/env bash
# Move benchmark OHLCV **and all strategy folders** (tv_trades.csv, *_trades.csv,
# .pine copies, _indicators/, etc.) into a private Git repository and attach it
# as a submodule at benchmarks/assets/ (expected layout:
#   assets/data/ETHUSDT_15.csv
#   assets/strategies/<01-…>/…
# ).
#
# This keeps TV-linked validation data (including per-strategy exports) out of
# the public engine repository after you filter-repo old paths.
#
# Run from a clean pineforge-engine tree when you are ready to open-source the
# engine without publishing TV-linked CSV fixtures in the main history.
#
# Prerequisites:
#   - Create an EMPTY private repo on GitHub (no README/License), e.g.
#       fullpass-4pass/pineforge-benchmarks-assets
#   - SSH access: ssh -T git@github.com
#   - Fixture paths committed on your current branch (no uncommitted changes
#     under the trees this script exports)
#
# Usage:
#   BENCHMARKS_ASSETS_REMOTE=git@github.com:ORG/REPO.git \
#     bash scripts/migrate_benchmarks_assets_to_private_submodule.sh
#
# Default remote (edit if your org/name differs):
#   git@github.com:fullpass-4pass/pineforge-benchmarks-assets.git
#
# After open-sourcing: use git filter-repo to strip old paths from history:
#   git filter-repo --invert-paths \
#     --path benchmarks/data --path benchmarks/strategies --force
#   # re-add origin, restore submodules, force-push — see CONTRIBUTING.md
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REMOTE="${BENCHMARKS_ASSETS_REMOTE:-git@github.com:fullpass-4pass/pineforge-benchmarks-assets.git}"

if [[ -f benchmarks/assets/.git || -d benchmarks/assets/.git ]]; then
    echo "benchmarks/assets already looks like a git submodule or nested repo. Aborting." >&2
    exit 1
fi

# --- resolve source layout (legacy vs inline-under-assets) ---
ARCH_DATA=""
ARCH_STRATEGIES=""
if [[ -d benchmarks/strategies ]]; then
    ARCH_DATA="benchmarks/data"
    ARCH_STRATEGIES="benchmarks/strategies"
elif [[ -d benchmarks/assets/strategies ]]; then
    ARCH_DATA="benchmarks/assets/data"
    ARCH_STRATEGIES="benchmarks/assets/strategies"
else
    echo "Neither benchmarks/strategies nor benchmarks/assets/strategies found. Nothing to migrate." >&2
    exit 1
fi

if [[ ! -e "$ARCH_DATA/ETHUSDT_15.csv" ]]; then
    echo "WARN: $ARCH_DATA/ETHUSDT_15.csv missing — submodule would lack OHLCV. Continuing anyway." >&2
fi

if [[ -n "$(git status --porcelain -- "$ARCH_DATA" "$ARCH_STRATEGIES")" ]]; then
    echo "Uncommitted changes under $ARCH_DATA or $ARCH_STRATEGIES. Commit or stash, then retry." >&2
    exit 1
fi

echo "Remote for private benchmark assets: $REMOTE"
echo "Packaging:"
echo "  $ARCH_DATA"
echo "  $ARCH_STRATEGIES"
echo "This will:"
echo "  1) export those trees from HEAD into the empty private repo (data/ + strategies/ at root)"
echo "  2) remove them here and add benchmarks/assets as a git submodule"
echo ""
echo "Press Enter to continue, Ctrl+C to cancel."
read -r _

TMP=
cleanup() {
    if [[ -n "${TMP:-}" && -d "${TMP:-}" ]]; then
        rm -rf "$TMP"
    fi
}
trap cleanup EXIT

TMP="$(mktemp -d)"
git archive --format=tar HEAD "$ARCH_DATA" "$ARCH_STRATEGIES" | tar -x -C "$TMP"
if [[ ! -d "$TMP/benchmarks" ]]; then
    echo "git archive did not produce benchmarks/. Aborting." >&2
    exit 1
fi

# Normalize to data/ + strategies/ at repo root
mkdir -p "$TMP/out"
if [[ "$ARCH_DATA" == benchmarks/data ]]; then
    mv "$TMP/benchmarks/data" "$TMP/out/data"
    mv "$TMP/benchmarks/strategies" "$TMP/out/strategies"
else
    mv "$TMP/benchmarks/assets/data" "$TMP/out/data"
    mv "$TMP/benchmarks/assets/strategies" "$TMP/out/strategies"
fi
rm -rf "$TMP/benchmarks"

(
    cd "$TMP/out"
    git init
    git add -A
    git commit -m "chore: import PineForge benchmark fixtures snapshot"
    git branch -M main
    git remote add origin "$REMOTE"
    git push -u origin main
)

git rm -rf "$ARCH_DATA" "$ARCH_STRATEGIES"

# Remove empty parents if we migrated from assets/*
if [[ "$ARCH_DATA" == benchmarks/assets/data ]]; then
    rmdir benchmarks/assets 2>/dev/null || true
fi

git submodule add "$REMOTE" benchmarks/assets

git commit -m "$(cat <<'EOF'
chore: move benchmark fixtures to private submodule

Public clones omit TV-linked OHLCV and per-strategy CSVs. Maintainers: see CONTRIBUTING.md.
EOF
)"

echo ""
echo "Done. Review 'git show HEAD' and push:"
echo "  git push origin HEAD"
