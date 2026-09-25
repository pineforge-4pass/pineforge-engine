#!/usr/bin/env bash
# The corpus half of lane N12's strategy.risk measurement (design §3.6.1), as a re-runnable check
# that changes nothing under src/ (R5 lane H-MEASURE). Pine's risk statements reach the adapter on
# script bar 0, after the run spec is digested, so no in-process test can seed the kernel's
# NativeRunSpec::risk for a Pine run; the env-gated patch that does is a FIXTURE
# (tests/fixtures/seeded_risk/n12-experiment.patch), applied only to a throwaway copy of the tree,
# which builds the runtime + the five strategy.risk corpus probes and runs each probe
#   (1) with PF_N12_ROUTE unset: must hash to scripts/corpus_parity_baseline.txt (the patch is inert),
#   (2) routed + seeded (the kernel's rule instead of the adapter's): the TradingView-recorded
#       columns must move exactly as tests/fixtures/seeded_risk/expected.tsv says.
# Exit 0 = reproduced, 1 = the measurement moved (re-measure, then re-pin design §3.6.1), 2 = could
# not run (the fixture no longer applies to this tree: re-anchor it).
#   ROOT  the tree to copy (default: this checkout; its corpus/ must hold the LFS feed)
#   FIX   the fixture directory (default: tests/fixtures/seeded_risk)
#   WORK  a scratch directory (default: mktemp -d)   JOBS  build parallelism (default 8)
#   RUN   a command prefix for every build and run step (e.g. "nice -n 10")
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=${ROOT:-$HERE}; FIX=${FIX:-$HERE/tests/fixtures/seeded_risk}
WORK=${WORK:-$(mktemp -d)}; JOBS=${JOBS:-8}; RUN=${RUN:-}
T0=$(date +%s)
rm -rf "$WORK/src" "$WORK/build" "$WORK/out"; mkdir -p "$WORK/out/base" "$WORK/out/seeded"
cp -a "$ROOT" "$WORK/src" && rm -f "$WORK"/src/corpus/validation/*/strategy.so "$WORK"/src/corpus/validation/*/strategy.dylib
(cd "$WORK/src" && patch -p1 --dry-run --quiet < "$FIX/n12-experiment.patch" >/dev/null) \
    || { echo "seeded-risk: the fixture patch no longer applies to this tree (exit 2)"; exit 2; }
(cd "$WORK/src" && patch -p1 --quiet < "$FIX/n12-experiment.patch")
PROBES=$(grep -v '^#' "$FIX/expected.tsv" | cut -f1)
TARGETS=$(for p in $PROBES; do printf 'strategy_validation_%s ' "$(echo "$p" | tr '-' '_')"; done)
# shellcheck disable=SC2086  # $RUN and $TARGETS are word lists on purpose
$RUN cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPINEFORGE_VERSION_SOURCE=FILE \
    -DPINEFORGE_BUILD_TESTS=OFF -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON -Wno-dev > "$WORK/configure.log" 2>&1 \
    && $RUN ninja -C "$WORK/build" -j"$JOBS" pineforge $TARGETS > "$WORK/build.log" 2>&1 \
    || { tail -20 "$WORK/build.log"; echo "seeded-risk: build failed (exit 2)"; exit 2; }
(cd "$WORK/src" && python3 scripts/derive_corpus_feeds.py > /dev/null)
status=0
while IFS=$'\t' read -r p seed ndiff rb rs first when; do
  [[ "$p" == \#* || -z "$p" ]] && continue
  (cd "$WORK/src" && $RUN python3 scripts/run_strategy.py "corpus/validation/$p" --so-name strategy.so \
       -o "$WORK/out/base/$p.csv" > "$WORK/out/base/$p.log" 2>&1) || { echo "$p: inert run failed"; exit 2; }
  (cd "$WORK/src" && PF_N12_ROUTE=drawdown,cap PF_N12_SEED="$seed" $RUN python3 scripts/run_strategy.py \
       "corpus/validation/$p" --so-name strategy.so -o "$WORK/out/seeded/$p.csv" > "$WORK/out/seeded/$p.log" 2>&1) \
       || { echo "$p: seeded run failed"; exit 2; }
  pinned=$(awk -v f="validation/$p/engine_trades.csv" '$2==f {print $1}' "$WORK/src/scripts/corpus_parity_baseline.txt")
  got=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1]).read().encode()).hexdigest())' "$WORK/out/base/$p.csv")
  line=$(N12_WIDTH=10 python3 "$FIX/n12_diff.py" "$WORK/out/base" "$WORK/out/seeded" "$p" | head -1)
  if [[ "$ndiff" == 0 ]]; then want="identical on all $rs rows"; else want="differ=$ndiff of $(( rb > rs ? rb : rs )) first_chronological_row=$first (Entry"; fi
  verdict=ok
  [[ "$got" == "$pinned" ]] || { verdict="INERT-RUN-MOVED"; status=1; }
  [[ "$line" == *"$want"* ]] || { verdict="${verdict/ok/}SEEDED-MOVED"; status=1; }
  [[ "$ndiff" == 0 || "$line" == *"$when"* ]] || { verdict="${verdict/ok/}SEEDED-MOVED"; status=1; }
  printf '%-45s inert=%s seeded:%s  [%s]\n' "$p" "$([[ "$got" == "$pinned" ]] && echo baseline || echo MOVED)" "${line#*columns:}" "$verdict"
  grep -h 'N12:' "$WORK/out/seeded/$p.log" | sed 's/^/    /' | sort -u | head -2
done < "$FIX/expected.tsv"
echo "seeded-risk: $( ((status==0)) && echo REPRODUCED || echo MOVED ) in $(( $(date +%s) - T0 ))s (work dir $WORK)"
exit $status
