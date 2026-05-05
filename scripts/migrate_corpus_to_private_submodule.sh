#!/usr/bin/env bash
# Migrate corpus/ into a private Git repository and attach it as a git submodule
# at corpus/. Run this once from a clean pineforge-engine tree when you are
# ready to open-source the engine without publishing TV validation fixtures.
#
# Prerequisites:
#   - Create an EMPTY private repo on GitHub (no README/License), e.g.
#       fullpass-4pass/pineforge-corpus
#   - SSH access: ssh -T git@github.com
#   - corpus/ matches the last commit on your current branch (no uncommitted changes)
#
# Usage:
#   CORPUS_REMOTE=git@github.com:ORG/REPO.git \
#     bash scripts/migrate_corpus_to_private_submodule.sh
#
# Default remote (edit if your org/name differs):
#   git@github.com:fullpass-4pass/pineforge-corpus.git
#
# Historical note: older commits may still contain corpus/ blobs. For a clean
# public OSS history, use git filter-repo (or equivalent) to strip corpus/
# before advertising a public clone, or keep pre-rewrite history private.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REMOTE="${CORPUS_REMOTE:-git@github.com:fullpass-4pass/pineforge-corpus.git}"

if [[ -f corpus/.git || -d corpus/.git ]]; then
    echo "corpus/ already contains a .git (submodule or nested repo). Aborting." >&2
    exit 1
fi

if [[ ! -f corpus/CMakeLists.txt ]]; then
    echo "corpus/CMakeLists.txt not found — nothing to migrate." >&2
    exit 1
fi

if [[ -n "$(git status --porcelain -- corpus)" ]]; then
    echo "Uncommitted changes under corpus/. Commit or stash, then retry." >&2
    exit 1
fi

echo "Remote for private corpus: $REMOTE"
echo "This will:"
echo "  1) export the committed corpus/ tree from HEAD into that empty repo"
echo "  2) remove corpus/ here and re-add it as a git submodule"
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
git archive --format=tar HEAD corpus | tar -x -C "$TMP"
if [[ ! -d "$TMP/corpus" ]]; then
    echo "git archive did not produce corpus/. Aborting." >&2
    exit 1
fi

(
    cd "$TMP/corpus"
    git init
    git add -A
    git commit -m "chore: import PineForge validation corpus snapshot"
    git branch -M main
    git remote add origin "$REMOTE"
    git push -u origin main
)

git rm -rf corpus
git submodule add "$REMOTE" corpus

git commit -m "$(cat <<'EOF'
chore: move validation corpus to private submodule

Public clones omit TV-linked fixtures. Maintainers: see CONTRIBUTING.md.
EOF
)"

echo ""
echo "Done. Review 'git show HEAD' and push:"
echo "  git push origin HEAD"
