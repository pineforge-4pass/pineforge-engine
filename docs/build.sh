#!/usr/bin/env bash
# Build the PineForge API documentation site.
#
# Outputs:   docs/site/html/         (static HTML, ready to deploy)
# Requires:  doxygen, dot (graphviz), curl, awk, python3
#
# CI usage (.github/workflows/docs.yml, build job, Build site step):
#            bash docs/build.sh --self-test-seo
#            bash docs/build.sh
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

docs_version_label() {
    local exact_tag="$1" worktree_status="$2"
    if [[ -n "$exact_tag" && -z "$worktree_status" ]]; then
        printf '%s\n' "$exact_tag"
    else
        printf '%s\n' unreleased
    fi
}

self_test_seo() {
    if [[ "$(docs_version_label "v1.5.0" "")" != "v1.5.0" \
       || "$(docs_version_label "" "")" != "unreleased" \
       || "$(docs_version_label "v1.5.0" " M docs/build.sh")" != "unreleased" \
       || "$(docs_version_label "v1.5.0" "?? new-page.md")" != "unreleased" ]]; then
        echo "==> FAIL: docs version must be an exact clean tag or unreleased" >&2
        return 1
    fi
    local fixture script result
    fixture="$(mktemp -d "${TMPDIR:-/tmp}/pineforge-seo.XXXXXX")"
    script="$(cd "$(dirname "$0")" && pwd)/seo_postprocess.py"
    cat > "$fixture/check_seo.py" <<'PY'
from html.parser import HTMLParser
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

script = Path(sys.argv[1])
fixture = Path(sys.argv[2])
site = fixture / "html"
site.mkdir()
old_meta = '<meta name="description" content="Old shared description"/>\n'
old_canonical = '<link href="https://example.com/old.html" rel="canonical"/>\n'
duplicate_canonical = '<link rel="canonical" href="https://cdocs.pineforge.dev/other"/>\n'
titles = {
    "index.html": "PineForge: Landing",
    "x.html": "PineForge: Orders &amp; &quot;fills&quot; &lt;T&gt;",
    "other.html": "PineForge: Other reference",
    "404.html": "Page not found",
}
bodies = {
    "index.html": '''<a href="x.html">X</a>
<a href="index.html">Home</a>
<a href="index.html#start">Start</a>''',
    "x.html": '''<a href="other.html#a">Other</a>
<a href="./other.html">Relative</a>
<a title='href="other.html"' data-href="x.html" href="other.html">Tooltip</a>
<a href="https://example.com/x.html">External</a>
<a href="mailto:team@example.com">Mail</a>
<a href="#frag">Fragment</a>
<a href="other.html#https://example.com">Embedded URL</a>
<a href="//example.com/x.html">Protocol relative</a>
<a href="/other.html">Root path</a>
<a href="../other.html">Parent path</a>
<a href="search/help.html">Search path</a>
<a href="icon.png">Image</a>
<link href="style.css" rel="stylesheet"/>
<script src="links.js"></script>
<script>const text = 'href="other.html"';</script>
<!-- href="other.html" -->''',
    "other.html": '<a href="x.html">X</a>',
    "404.html": '<p>Page missing.</p><a href="/">Home</a><a href="https://pineforge.dev">pineforge.dev</a>',
}
descriptions = {
    "index.html": (
        "PineForge&#x27;s C++17 engine has two layers documented in this API reference: "
        "a kernel that knows nothing about PineScript or TradingView and a Pine "
        "adapter that reproduces TradingView&#x27;s execution semantics."
    ),
    "x.html": "Orders &amp; &quot;fills&quot; &lt;T&gt; — PineForge C++17 engine API documentation.",
    "other.html": "Other reference — PineForge C++17 engine API documentation.",
}

def page(title, body, metadata):
    return (f"<!doctype html>\n<html><head>\n<title>{title}</title>\n"
            f"{metadata}</head>\n<body>\n{body}\n</body></html>\n")

for name, title in titles.items():
    metadata = "" if name == "404.html" else old_meta
    if name == "other.html":
        metadata += old_canonical + duplicate_canonical
    (site / name).write_bytes(page(title, bodies[name], metadata).encode("utf-8"))

untouched = {
    "links.js": b'const page = "foo.html"; const link = \'href="x.html"\';\r\n',
    "style.css": b'/* foo.html */\n',
    "search/searchdata.js": b'const page = "foo.html";\n',
    "search/help.html": b'<a href="x.html">Nested input stays untouched</a>\n',
}
for name, contents in untouched.items():
    (site / name).parent.mkdir(parents=True, exist_ok=True)
    (site / name).write_bytes(contents)

expected_bodies = dict(bodies)
expected_bodies["index.html"] = (bodies["index.html"]
    .replace('href="x.html"', 'href="x"')
    .replace('href="index.html"', 'href="./"')
    .replace('href="index.html#start"', 'href="./#start"'))
expected_bodies["x.html"] = (bodies["x.html"]
    .replace('<a href="other.html#a">', '<a href="other#a">')
    .replace('<a href="./other.html">', '<a href="./other">')
    .replace('data-href="x.html" href="other.html">', 'data-href="x.html" href="other">'))
expected_bodies["other.html"] = '<a href="x">X</a>'

class Metadata(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.canonicals = []
        self.descriptions = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "link" and "canonical" in attrs.get("rel", "").split():
            self.canonicals.append(attrs.get("href"))
        if tag == "meta" and attrs.get("name") == "description":
            self.descriptions.append(attrs.get("content"))

command = [sys.executable, "-I", "-B", str(script), str(site)]
subprocess.run(command, check=True)
parsed_descriptions = []
canonical_urls = {}
for name, title in titles.items():
    url = "https://cdocs.tunaxis.com/" + ("" if name == "index.html" else name[:-5])
    canonical_urls[name] = url
    canonical = f'<link rel="canonical" href="{url}"/>\n'
    actual = (site / name).read_bytes().decode("utf-8")
    if name == "404.html":
        # The error page is left exactly as written: no canonical, no description, no link rewrite.
        assert actual == page(title, bodies[name], ""), "404.html must be left untouched"
        continue
    meta = f'<meta name="description" content="{descriptions[name]}"/>\n'
    assert canonical + "</head>" in actual, f"{name}: missing canonical before </head>"
    assert actual == page(title, expected_bodies[name], meta + canonical), f"{name}: wrong HTML output"
    parsed = Metadata()
    parsed.feed(actual)
    assert parsed.canonicals == [url], f"{name}: canonical must occur exactly once"
    assert len(parsed.descriptions) == 1, f"{name}: description must occur exactly once"
    parsed_descriptions.extend(parsed.descriptions)
assert len(set(parsed_descriptions)) == len(titles) - 1, "page descriptions must differ (404 has none)"
assert '<a href="x">X</a>' in (site / "index.html").read_text(), "x.html was not rewritten"
for name, contents in untouched.items():
    assert (site / name).read_bytes() == contents, f"{name}: asset or nested page changed"

namespace = "{http://www.sitemaps.org/schemas/sitemap/0.9}"
root = ET.parse(site / "sitemap.xml").getroot()
assert root.tag == namespace + "urlset" and root.attrib == {}, "invalid sitemap urlset"
urls = []
for entry in root:
    assert entry.tag == namespace + "url" and entry.attrib == {}, "invalid sitemap entry"
    assert len(entry) == 1 and entry[0].tag == namespace + "loc", "sitemap must contain only locations"
    assert entry[0].attrib == {}, "sitemap location has unexpected attributes"
    urls.append(entry[0].text)
expected_urls = sorted(canonical_urls[path.name] for path in site.glob("*.html") if path.name != "404.html")
assert urls == expected_urls, "sitemap differs from the sorted top-level page set minus 404"

first_pass = {path.relative_to(site): path.read_bytes() for path in site.rglob("*") if path.is_file()}
subprocess.run(command, check=True)
second_pass = {path.relative_to(site): path.read_bytes() for path in site.rglob("*") if path.is_file()}
assert second_pass == first_pass, "a second pass must be byte-identical, including canonical and sitemap"

empty = fixture / "empty"
empty.mkdir()
only_404 = fixture / "only-404"
only_404.mkdir()
(only_404 / "404.html").write_bytes((site / "404.html").read_bytes())
for invalid_site in (empty, only_404, fixture / "missing"):
    result = subprocess.run(command[:-1] + [str(invalid_site)], capture_output=True)
    assert result.returncode != 0, "missing or empty documentation must fail"
    assert not (invalid_site / "sitemap.xml").exists(), "invalid input produced a sitemap"

# Add Doxygen source listings only after checking the original site's exact output.
# These names follow the encoding of the configured headers and C/C++ examples.
source_metadata = {
    "pineforge_8h_source.html": "",
    "native__host_8hpp_source.html": old_meta,
    "hello__kernel_8cpp_source.html": old_meta + '<meta name="robots" content="noindex">\n',
    "hello__kernel__c_8c_source.html": old_meta + '<META NAME="ROBOTS" CONTENT="index, follow"/>\n',
    "native__order_8hpp_source.html": (
        old_meta + '<meta name="robots" content="noindex, follow"/>\n'
        '<meta name="robots" content="index"/>\n'
    ),
}
api_titles = {
    "classpineforge_1_1data__source.html": "PineForge: data_source Class Reference",
    "classpineforge_1_1data__8hpp__source.html": "PineForge: data_8hpp_source Class Reference",
    "structpineforge_1_1NativeRunSpec.html": "PineForge: NativeRunSpec Struct Reference",
    "namespacepineforge.html": "PineForge: pineforge Namespace Reference",
    "pineforge_8h.html": "PineForge: pineforge.h File Reference",
    "native__host_8hpp.html": "PineForge: native_host.hpp File Reference",
    "hello__kernel_8cpp.html": "PineForge: hello_kernel.cpp File Reference",
    "hello__kernel__c_8c.html": "PineForge: hello_kernel_c.c File Reference",
    "dir_0123456789abcdef0123456789abcdef.html": "PineForge: Directory Reference",
    "group__kernel.html": "PineForge: Kernel API",
    "functions.html": "PineForge: Documented Members",
    "md_pages_2raw__source.html": "PineForge: Source integration guide",
}
api_robots = '<meta name="robots" content="index, follow"/>\n'
for name, metadata in source_metadata.items():
    body = '<div class="fragment"><div class="line">int example;</div></div>'
    (site / name).write_bytes(page(f"PineForge: {name} Source File", body, metadata).encode("utf-8"))
for name, title in api_titles.items():
    metadata = old_meta + (api_robots if name == "namespacepineforge.html" else "")
    (site / name).write_bytes(page(title, "<p>Documented API.</p>", metadata).encode("utf-8"))

# An explicit page id can match a source-listing name without being a listing.
guide_name = "guide_8cpp_source.html"
guide_url = "https://cdocs.tunaxis.com/guide_8cpp_source"
guide_canonical = f'<link rel="canonical" href="{guide_url}"/>\n'
(site / guide_name).write_bytes(page(
    "PineForge: Integration guide", "<p>Integration guide.</p>", old_meta + guide_canonical
).encode("utf-8"))

# The parser must strip trailing whitespace inside the title before classifying.
whitespace_source = "whitespace_8hpp_source.html"
source_metadata[whitespace_source] = old_meta
(site / whitespace_source).write_bytes(page(
    "PineForge: whitespace.hpp Source File \t\n",
    '<div class="fragment"><div class="line">int whitespace_example;</div></div>',
    source_metadata[whitespace_source],
).encode("utf-8"))

class RobotsMetadata(Metadata):
    def __init__(self):
        super().__init__()
        self.robots = []
        self.in_head = False

    def handle_starttag(self, tag, attrs):
        super().handle_starttag(tag, attrs)
        if tag == "head":
            self.in_head = True
        attrs = dict(attrs)
        if tag == "meta" and (attrs.get("name") or "").lower() == "robots":
            self.robots.append((attrs.get("content"), self.in_head))

    def handle_endtag(self, tag):
        if tag == "head":
            self.in_head = False

subprocess.run(command, check=True)
for path in sorted(site.glob("*.html")):
    actual = path.read_bytes().decode("utf-8")
    parsed = RobotsMetadata()
    parsed.feed(actual)
    parsed.close()
    if path.name in source_metadata:
        assert parsed.robots == [("noindex", True)], f"{path.name}: expected one noindex tag in the head"
        url = "https://cdocs.tunaxis.com/" + path.stem
        expected_tail = ('<meta name="robots" content="noindex">\n'
                         f'<link rel="canonical" href="{url}"/>\n</head>')
        assert expected_tail in actual, f"{path.name}: robots tag must precede the canonical at the end of the head"
        assert parsed.canonicals == [url], f"{path.name}: source canonical must occur exactly once"
        assert len(parsed.descriptions) == 1, f"{path.name}: source description must occur exactly once"
    elif path.name == "namespacepineforge.html":
        assert parsed.robots == [("index, follow", True)], "API robots metadata must stay unchanged"
        assert api_robots in actual, "API robots markup must stay byte-identical"
    else:
        assert parsed.robots == [], f"{path.name}: API, landing and error pages must not receive robots metadata"
    if path.name in titles:
        assert path.read_bytes() == first_pass[Path(path.name)], f"{path.name}: original page changed after adding source listings"
    if path.name == guide_name:
        assert parsed.robots == [], "a hand-written page with a colliding explicit id must not receive robots metadata"
        assert parsed.canonicals == [guide_url], "the colliding guide must keep exactly one canonical"
        assert guide_canonical + "</head>" in actual, "the colliding guide must keep its canonical at the end of the head"
for name, contents in untouched.items():
    assert (site / name).read_bytes() == contents, f"{name}: source processing changed an asset or nested page"

source_root = ET.parse(site / "sitemap.xml").getroot()
source_urls = [entry.findtext(namespace + "loc") for entry in source_root]
indexable_names = [name for name in titles if name != "404.html"] + list(api_titles)
indexable_names.append(guide_name)
indexable_urls = sorted("https://cdocs.tunaxis.com/" + ("" if name == "index.html" else name[:-5])
                        for name in indexable_names)
assert source_urls == indexable_urls, "sitemap must keep all API pages and omit every source listing and 404"
assert guide_url in source_urls, "the hand-written guide with a colliding explicit id must stay in the sitemap"

source_first_pass = {path.relative_to(site): path.read_bytes() for path in site.rglob("*") if path.is_file()}
subprocess.run(command, check=True)
source_second_pass = {path.relative_to(site): path.read_bytes() for path in site.rglob("*") if path.is_file()}
assert source_second_pass == source_first_pass, "source robots metadata and the full site must be byte-identical on a second pass"
PY
    if python3 -I -B "$fixture/check_seo.py" "$script" "$fixture"; then
        result=0
    else
        result=$?
    fi
    rm -rf "$fixture"
    if (( result == 0 )); then
        echo "==> Docs SEO self-test: canonical, links, descriptions, sitemap, assets, version, source noindex ... OK"
    fi
    return "$result"
}

if [[ "${1:-}" == "--self-test-retry" && "$#" == 1 ]]; then
    self_test_doxygen_retry
    exit $?
fi
if [[ "${1:-}" == "--self-test-seo" && "$#" == 1 ]]; then
    self_test_seo
    exit $?
fi
if (( $# != 0 )); then
    echo "usage: docs/build.sh [--self-test-retry|--self-test-seo]" >&2
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

# 2. Label only a clean, exactly tagged checkout as a release.
VERSION="unreleased"
if command -v git >/dev/null 2>&1 \
   && GIT_VERSION="$(git -C .. describe --tags --exact-match 2>/dev/null)" \
   && GIT_STATUS="$(git -C .. status --porcelain 2>/dev/null)"; then
    VERSION="$(docs_version_label "$GIT_VERSION" "$GIT_STATUS")"
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
# Add metadata to top-level *.html pages and write their canonical sitemap.
# Raw _8<ext>_source.html listings with a title ending in "Source File" get
# noindex and stay out of the sitemap; file references and other docs remain
# indexable, with 404 untouched.
python3 -I -B seo_postprocess.py site/html

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
    echo "==> unguarded warnings (printed, not fatal):"
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
