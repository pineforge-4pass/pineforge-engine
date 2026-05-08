#!/usr/bin/env bash
# Build the PineForge API documentation site.
#
# Outputs:   docs/site/html/         (static HTML, ready to deploy)
# Requires:  doxygen, dot (graphviz), curl, awk
#
# CI usage:  bash docs/build.sh
# Local:     bash docs/build.sh        # docs/site/html/index.html
#            python3 -m http.server -d docs/site/html 8080

set -euo pipefail

cd "$(dirname "$0")"

DOXYGEN_AWESOME_VERSION="${DOXYGEN_AWESOME_VERSION:-v2.3.4}"
THEME_DIR="_theme/doxygen-awesome"

# Prefer Doxygen 1.13.2 if installed (matches CI). Falls back to whatever
# `doxygen` is on $PATH otherwise.
if [[ -x "/Volumes/Doxygen/Doxygen.app/Contents/Resources/doxygen" ]]; then
    export PATH="/Volumes/Doxygen/Doxygen.app/Contents/Resources:$PATH"
fi

# 1. Fetch doxygen-awesome-css if missing or wrong version.
if [[ ! -f "$THEME_DIR/.version" ]] \
   || [[ "$(cat "$THEME_DIR/.version" 2>/dev/null)" != "$DOXYGEN_AWESOME_VERSION" ]]; then
    echo "==> Fetching doxygen-awesome-css $DOXYGEN_AWESOME_VERSION"
    rm -rf "$THEME_DIR"
    mkdir -p "$THEME_DIR"
    curl -sSL "https://github.com/jothepro/doxygen-awesome-css/archive/refs/tags/${DOXYGEN_AWESOME_VERSION}.tar.gz" \
        | tar -xz --strip-components=1 -C "$THEME_DIR"
    echo "$DOXYGEN_AWESOME_VERSION" > "$THEME_DIR/.version"
fi

# 2. Resolve project version from VERSION file (or git tag if available).
VERSION="$(cat ../VERSION 2>/dev/null || echo unknown)"
if command -v git >/dev/null 2>&1 && git -C .. rev-parse --git-dir >/dev/null 2>&1; then
    GIT_VERSION="$(git -C .. describe --tags --dirty --always 2>/dev/null || true)"
    [[ -n "$GIT_VERSION" ]] && VERSION="$GIT_VERSION"
fi
echo "==> Building docs for PineForge $VERSION"

# 3. Run doxygen with PROJECT_NUMBER injected.
rm -rf site
( cat Doxyfile; echo "PROJECT_NUMBER = $VERSION" ) | doxygen -

# 4. Copy a default favicon if the user hasn't shipped one.
if [[ ! -f site/html/favicon.png ]] && [[ -f _theme/favicon.png ]]; then
    cp _theme/favicon.png site/html/favicon.png
fi

# 5. Drop a CNAME / _headers placeholder for Cloudflare Pages if env says so.
if [[ -n "${DOCS_CUSTOM_DOMAIN:-}" ]]; then
    echo "$DOCS_CUSTOM_DOMAIN" > site/html/CNAME
fi

echo "==> Built site/html/index.html"
echo "    Serve locally:  python3 -m http.server -d docs/site/html 8080"
