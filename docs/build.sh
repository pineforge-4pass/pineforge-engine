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
#
# Through a config FILE, not `doxygen -`: doxygen 1.18 aborts with a bus error
# on some configurations read from stdin, and the file form costs nothing.
rm -rf site
mkdir -p site
CONFIG="$(mktemp "${TMPDIR:-/tmp}/pineforge-doxyfile.XXXXXX")"
trap 'rm -f "$CONFIG"' EXIT
{ cat Doxyfile; echo "PROJECT_NUMBER = $VERSION"; } > "$CONFIG"
doxygen "$CONFIG"

# 3b. The scoped warning gate (R5 lane L14-A).
#
# Doxygen has one global WARN_AS_ERROR, so FAIL_ON_WARNINGS would let a stale
# comment in a legacy Pine-adapter header stop the site from building. The
# guarded set is the surface this project asks a new contributor to read: the
# kernel / native API headers, the C API header, the C ABI header, the layer
# map and the examples. A warning there is an error; anywhere else it is
# printed, counted, and left for the page lanes.
GUARDED='include/pineforge/native_host\.hpp|include/pineforge/native_run_spec\.hpp'
GUARDED="$GUARDED"'|include/pineforge/native_order\.hpp|include/pineforge/native_order_identity\.hpp'
GUARDED="$GUARDED"'|include/pineforge/native_toolkit\.hpp|include/pineforge/native_c_api\.h'
GUARDED="$GUARDED"'|include/pineforge/pineforge\.h|docs/groups\.dox|examples/native/'

LOG="site/doxygen-warnings.log"
TOTAL=0
if [[ -f "$LOG" ]]; then
    TOTAL="$(grep -c 'warning:' "$LOG" || true)"
fi
echo "==> Doxygen warnings: $TOTAL"
if [[ -f "$LOG" ]] && grep -E "$GUARDED" "$LOG" | grep -q 'warning:'; then
    echo "==> FAIL: a warning in the guarded public surface" >&2
    grep -E "$GUARDED" "$LOG" | grep 'warning:' >&2
    exit 1
fi
if [[ "$TOTAL" != "0" ]]; then
    echo "==> unguarded warnings (page lanes L14-B / L14-C own these):"
    grep 'warning:' "$LOG" || true
fi

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
