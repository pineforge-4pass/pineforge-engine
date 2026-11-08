#!/usr/bin/env bash
# Ordinary CTest entrypoint. Executables stay literal; Python only reads data.
set -euo pipefail
source_root=$1
module=$2
out=$3
mkdir -p "$out"
if test -e "$out/collection.json"; then
    printf 'CR559 CLI: collection already exists: %s; use a fresh output directory to preserve prior artifacts.\n' "$out/collection.json" >&2
    exit 1
fi
if ! test -s "$module"; then
    printf 'CR559 CLI: generated module is missing or empty: %s; build target cr559_cli_pair before running this test.\n' "$module" >&2
    exit 1
fi
set -x
python3 -B "$source_root/tests/test_cr559_map_capacity_cli.py" prepare --out "$out"

# Boundary and refusal are finite-policy controls, run before unlimited cases.
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --input-tf 1 --script-tf 1 --inputs '{"Pairs":"50000","Salt":"7"}' --limit-map-pairs 50000 >"$out/finite_boundary.stdout" 2>"$out/finite_boundary.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/finite_boundary.exit"
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --input-tf 1 --script-tf 1 --inputs '{"Pairs":"50001","Salt":"7"}' --limit-map-pairs 50000 >"$out/finite_refusal.stdout" 2>"$out/finite_refusal.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/finite_refusal.exit"
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --input-tf 1 --script-tf 1 --inputs '{"Pairs":"50003","Salt":"11"}' >"$out/omitted_a.stdout" 2>"$out/omitted_a.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/omitted_a.exit"
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --input-tf 1 --script-tf 1 --inputs '{"Pairs":"50003","Salt":"11"}' --limit-map-pairs 0 >"$out/zero_a.stdout" 2>"$out/zero_a.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/zero_a.exit"
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --input-tf 1 --script-tf 1 --inputs '{"Pairs":"50009","Salt":"23"}' >"$out/omitted_b.stdout" 2>"$out/omitted_b.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/omitted_b.exit"
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --input-tf 1 --script-tf 1 --inputs '{"Pairs":"50009","Salt":"23"}' --limit-map-pairs 0 >"$out/zero_b.stdout" 2>"$out/zero_b.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/zero_b.exit"
if python3 -B "$source_root/docker/run_json.py" --so "$module" --ohlcv "$out/feed.csv" --profile unjailed --inputs '{"Pairs":"50003","Salt":"11"}' --limit-map-pairs -1 >"$out/negative_option.stdout" 2>"$out/negative_option.stderr"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" >"$out/negative_option.exit"
python3 -B "$source_root/tests/test_cr559_map_capacity_cli.py" check --out "$out"
