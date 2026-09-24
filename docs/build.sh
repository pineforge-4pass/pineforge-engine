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

run_doxygen_with_retry() {
    local attempt=1 status
    while (( attempt <= 3 )); do
        # A SIGBUS may leave a partial site and an old warning log behind.
        rm -rf site
        mkdir -p site
        if doxygen "$CONFIG"; then
            return 0
        else
            status=$?
        fi
        if (( status != 135 || attempt == 3 )); then
            echo "==> Doxygen failed (exit $status, attempt $attempt/3)" >&2
            return "$status"
        fi
        echo "==> Doxygen SIGBUS (exit 135, attempt $attempt/3); retrying" >&2
        sleep 1
        attempt=$((attempt + 1))
    done
}

self_test_doxygen_retry() {
    local fixture result
    fixture="$(mktemp -d "${TMPDIR:-/tmp}/pineforge-doxygen-retry.XXXXXX")"
    if (
        cd "$fixture"
        CONFIG=fixture
        test_calls=0
        test_succeed_on=3
        test_failure_code=135
        doxygen() {
            test_calls=$((test_calls + 1))
            if (( test_calls < test_succeed_on )); then
                return "$test_failure_code"
            fi
            mkdir -p site/html
            : > site/html/index.html
        }
        run_doxygen_with_retry
        if [[ "$test_calls" != 3 || ! -f site/html/index.html ]]; then
            echo "==> FAIL: two SIGBUS exits did not recover on attempt three" >&2
            exit 1
        fi
        test_calls=0
        test_succeed_on=4
        if run_doxygen_with_retry; then
            echo "==> FAIL: a fourth attempt was accepted" >&2
            exit 1
        else
            test_status=$?
        fi
        if [[ "$test_calls" != 3 || "$test_status" != 135 ]]; then
            echo "==> FAIL: three SIGBUS exits did not stop after attempt three" >&2
            exit 1
        fi
        test_calls=0
        test_failure_code=2
        if run_doxygen_with_retry; then
            echo "==> FAIL: a non-SIGBUS failure was retried" >&2
            exit 1
        else
            test_status=$?
        fi
        if [[ "$test_calls" != 1 || "$test_status" != 2 ]]; then
            echo "==> FAIL: a non-SIGBUS exit did not stop immediately" >&2
            exit 1
        fi
    ); then
        result=0
    else
        result=$?
    fi
    rm -rf "$fixture"
    if (( result == 0 )); then
        echo "==> Doxygen retry self-test: two SIGBUS recover; third and non-SIGBUS fail ... OK"
    fi
    return "$result"
}

if [[ "${1:-}" == "--self-test-retry" && "$#" == 1 ]]; then
    self_test_doxygen_retry
    exit $?
fi
if (( $# != 0 )); then
    echo "usage: docs/build.sh [--self-test-retry]" >&2
    exit 2
fi

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
CONFIG="$(mktemp "${TMPDIR:-/tmp}/pineforge-doxyfile.XXXXXX")"
trap 'rm -f "$CONFIG"' EXIT
{ cat Doxyfile; echo "PROJECT_NUMBER = $VERSION"; } > "$CONFIG"
run_doxygen_with_retry

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
