#!/usr/bin/env bash
# Verifies entrypoint honors PINEFORGE_IN_DIR and uses a per-run work dir
# (no fixed /tmp/strategy.* collisions). Stubs g++/python so the test needs
# no real toolchain — it only exercises the path/work-dir logic of the script.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ENTRY="$HERE/../docker/entrypoint.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Fake prefix + toolchain. g++ writes the .so it is told to (-o arg);
# run_json.py prints a sentinel JSON. Put stubs first on PATH.
mkdir -p "$work/bin" "$work/prefix/bin" "$work/prefix/lib" "$work/prefix/include"
cat > "$work/bin/g++" <<'SH'
#!/usr/bin/env bash
out=""; prev=""
for a in "$@"; do [ "$prev" = "-o" ] && out="$a"; prev="$a"; done
: > "$out"
SH
cat > "$work/prefix/bin/run_json.py" <<'SH'
#!/usr/bin/env python3
print('{"ok": true, "marker": "indir"}')
SH
: > "$work/prefix/lib/libpineforge.a"
chmod +x "$work/bin/g++" "$work/prefix/bin/run_json.py"

# A custom input dir (NOT /in) with a pre-transpiled cpp + csv.
indir="$work/indir"; mkdir -p "$indir"
echo "int main(){}" > "$indir/strategy.cpp"
echo "timestamp,open,high,low,close,volume" > "$indir/ohlcv.csv"

out="$(PATH="$work/bin:$PATH" \
     PINEFORGE_PREFIX="$work/prefix" \
     PINEFORGE_IN_DIR="$indir" \
     bash "$ENTRY" 2>/dev/null)"

echo "$out" | grep -q '"marker": "indir"' || { echo "FAIL: did not run from PINEFORGE_IN_DIR"; exit 1; }
# Fixed legacy paths must NOT have been created by the run.
[ ! -e /tmp/strategy.so ] || { echo "FAIL: wrote fixed /tmp/strategy.so"; exit 1; }
echo "PASS"
