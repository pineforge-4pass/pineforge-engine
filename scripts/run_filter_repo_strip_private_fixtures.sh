#!/usr/bin/env bash
# Strip TV-linked / maintainer-only trees from Git history using git-filter-repo.
#
# Removes these paths from *every* commit:
#   corpus/
#   benchmarks/data/
#   benchmarks/strategies/
#   benchmarks/assets/   (optional — uncomment in PATHS if you ever committed fixtures there)
#
# Prerequisites:
#   pip install git-filter-repo   # or: brew install git-filter-repo
#   A clean working tree (commit or stash first).
#
# WARNING:
#   - Rewrites ALL commits. Coordinates with collaborators; everyone must re-clone
#     or reset hard to the new history.
#   - git-filter-repo REMOVES the `origin` remote — this script re-adds it from a
#     saved URL.
#   - After rewrite, submodule gitlinks at `corpus/` (and `benchmarks/assets/`) are
#     gone from history until you re-add them in a NEW commit (see bottom).
#
# Usage (from repo root):
#   chmod +x scripts/run_filter_repo_strip_private_fixtures.sh
#   bash scripts/run_filter_repo_strip_private_fixtures.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v git-filter-repo >/dev/null 2>&1; then
    echo "git-filter-repo not found. Install:  pip install git-filter-repo" >&2
    echo "  or:  brew install git-filter-repo" >&2
    exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "Working tree is not clean. Commit or stash, then retry." >&2
    exit 1
fi

ORIGIN_URL=""
if git remote get-url origin >/dev/null 2>&1; then
    ORIGIN_URL="$(git remote get-url origin)"
fi

BUNDLE_DEFAULT="$ROOT/../pineforge-engine-pre-filter-$(date +%Y%m%d%H%M).bundle"
echo "About to rewrite history in: $ROOT"
echo "Optional full backup (all refs):"
echo "  git bundle create \"$BUNDLE_DEFAULT\" --all"
echo ""
echo "Paths removed from every commit:"
echo "  corpus/"
echo "  benchmarks/data/"
echo "  benchmarks/strategies/"
echo ""
echo "Press Enter to run git-filter-repo, Ctrl+C to cancel."
read -r _

# --- rewrite ---------------------------------------------------------------

git filter-repo --force \
    --invert-paths \
    --path corpus \
    --path benchmarks/data \
    --path benchmarks/strategies

# Add if you ever committed inline fixtures under assets/:
#   --path benchmarks/assets \

if [[ -n "$ORIGIN_URL" ]]; then
    git remote add origin "$ORIGIN_URL"
    echo "Re-added remote: origin -> $ORIGIN_URL"
else
    echo "No 'origin' remote was configured; add it manually: git remote add origin <url>" >&2
fi

echo ""
echo "=== Filter complete ==="
echo ""
echo "Next steps (maintainers):"
echo "  1. If corpus/ or benchmarks/ still has stray files, remove them:"
echo "       rm -rf corpus benchmarks/assets"
echo "  2. Re-attach private submodules (pick your remotes):"
echo "       git submodule add git@github.com:fullpass-4pass/pineforge-corpus.git corpus"
echo "       # After benchmarks private repo exists:"
echo "       # git submodule add git@github.com:fullpass-4pass/pineforge-benchmarks-assets.git benchmarks/assets"
echo "  3. Commit submodule pointers:"
echo "       git add .gitmodules corpus benchmarks/assets"
echo "       git commit -m 'chore: restore private fixture submodules after history rewrite'"
echo "  4. Force-push (ONLY after team agrees):"
echo "       git push --force-with-lease origin main"
echo ""
