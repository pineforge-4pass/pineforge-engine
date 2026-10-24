#!/usr/bin/env bash
# CR-150 lane L0, acceptance (c), DESIGN.md section 7 / 4.1.
#
# Larger than RAM, on an AWS spot box with 2 GiB RAM and 40 GB disk:
#   100,000,000 synthetic 1m bars (4.8 GB of .bin, about 5 GB of sink,
#   2.4 GB of engine scratch). run_json --ohlcv-bin --report-sink must exit 0,
#   stay under the box's RAM, and its masked summary and sink sha256 must equal
#   the large-box run named by --compare-with.
#
# RED ON 3fc010a8b: run_json has neither --ohlcv-bin nor --report-sink, so this
# script falls back to the CSV path, which holds about 300 B per bar while
# loading (docker/run_json.py:2500-2536): about 30 GB at 100M bars. The run is
# OOM-killed (exit 137) or raises MemoryError. Red on memory, not on setup.
#
# Spot only: not a ctest row (DESIGN.md 7).
#
# Usage:
#   bash tests/flat_memory_spot_bigdata.sh --data 100000000 --so <module.so> \
#       --compare-with large-box.json [--work-dir DIR] [--run-json PATH]
#
# large-box.json:
#   {"summary": <the run_json summary-shaped document, masked>,
#    "sink_sha256": "<the trailer sha256>"}
# (--record-comparison writes that file after a successful large-box run.)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_BARS=100000000
SO=""
COMPARE=""
WORK=""
RUN_JSON="$ROOT_DIR/docker/run_json.py"
RECORD=""
RAM_LIMIT_MIB=1900

usage() {
    sed -n '2,35p' "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

while (( $# > 0 )); do
    case "$1" in
        --data) DATA_BARS="${2:?}"; shift 2 ;;
        --so) SO="${2:?}"; shift 2 ;;
        --compare-with) COMPARE="${2:?}"; shift 2 ;;
        --record-comparison) RECORD="${2:?}"; shift 2 ;;
        --work-dir) WORK="${2:?}"; shift 2 ;;
        --run-json) RUN_JSON="${2:?}"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) printf 'flat_memory_spot_bigdata: unknown argument %s\n' "$1" >&2; exit 2 ;;
    esac
done

[[ -n "$SO" && -f "$SO" ]] || { printf 'SETUP: --so <module.so> is required\n' >&2; exit 2; }
[[ -f "$RUN_JSON" ]] || { printf 'SETUP: %s is not present\n' "$RUN_JSON" >&2; exit 2; }
(( DATA_BARS > 0 )) || { printf 'SETUP: --data must be positive\n' >&2; exit 2; }

command -v python3 >/dev/null || { printf 'SETUP: python3 is required\n' >&2; exit 2; }

CREATED_WORK=0
if [[ -z "$WORK" ]]; then
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/pf-flat-memory-big-XXXXXX")"
    CREATED_WORK=1
fi
mkdir -p "$WORK"

HELP_TEXT="$(python3 "$RUN_JSON" --help 2>&1 || true)"
HAS_BIN=0; grep -q -- '--ohlcv-bin' <<<"$HELP_TEXT" && HAS_BIN=1
HAS_SINK=0; grep -q -- '--report-sink' <<<"$HELP_TEXT" && HAS_SINK=1

printf '[flat-memory] data=%s bars, module=%s\n' "$DATA_BARS" "$SO"
printf '[flat-memory] --ohlcv-bin=%s --report-sink=%s\n' "$HAS_BIN" "$HAS_SINK"
printf '[flat-memory] work=%s\n' "$WORK"

AVAIL_KIB="$(df -Pk "$WORK" | awk 'NR==2 {print $4}')"
NEEDED_KIB=$(( DATA_BARS * 48 / 1024 * 2 + 8 * 1024 * 1024 ))
if (( AVAIL_KIB < NEEDED_KIB )); then
    printf 'SETUP: %s needs about %d KiB free (data + sink), has %d KiB\n' \
        "$WORK" "$NEEDED_KIB" "$AVAIL_KIB" >&2
    exit 2
fi
MEM_KIB="$(awk '/MemTotal/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"
if (( MEM_KIB > 0 )) && (( MEM_KIB / 1024 > RAM_LIMIT_MIB )); then
    printf '[flat-memory] note: this box has %d MiB RAM; (c) is defined on a 2 GiB box\n' \
        "$(( MEM_KIB / 1024 ))"
fi

# ── the input ───────────────────────────────────────────────────────────
# One pass, streaming: never holds the data in this process.
python3 - "$DATA_BARS" "$WORK" "$HAS_BIN" <<'PY'
import hashlib, json, math, os, struct, sys
n = int(sys.argv[1]); work = sys.argv[2]; use_bin = sys.argv[3] == "1"
base = 1_699_999_980_000
step = 60_000
if use_bin:
    bar_path = os.path.join(work, "chart.TEST_FLAT.1.bin")
    tmp = bar_path + ".tmp"
    digest = hashlib.sha256()
    with open(tmp, "wb") as out:
        for i in range(n):
            price = 100.0 + 0.001 * ((i % 20000) if (i % 20000) < 10000 else 20000 - (i % 20000))
            payload = struct.pack("<5dq", price, price + 0.5, price - 0.5, price, 1000.0,
                                  base + i * step)
            out.write(payload); digest.update(payload)
    os.replace(tmp, bar_path)
    sidecar = {"format": "pf_bar_t/v1", "record_size": 48, "symbol": "TEST:FLAT",
               "tf": "1", "count": n, "first_ts": base,
               "last_ts": base + (n - 1) * step, "sha256": digest.hexdigest()}
    with open(bar_path + ".json", "w", encoding="ascii") as out:
        out.write(json.dumps(sidecar, sort_keys=True, separators=(",", ":")) + "\n")
    print(bar_path)
else:
    csv_path = os.path.join(work, "chart.csv")
    with open(csv_path, "w", newline="", encoding="ascii") as out:
        out.write("timestamp,open,high,low,close,volume\n")
        for i in range(n):
            price = 100.0 + 0.001 * ((i % 20000) if (i % 20000) < 10000 else 20000 - (i % 20000))
            ts = base + i * step
            out.write(f"{ts},{price!r},{price + 0.5!r},{price - 0.5!r},{price!r},1000.0\n")
    print(csv_path)
PY

if (( HAS_BIN )); then
    INPUT_ARGS=(--ohlcv-bin "$WORK/chart.TEST_FLAT.1.bin")
else
    INPUT_ARGS=(--ohlcv "$WORK/chart.csv")
fi

REPORT_ARGS=()
SINK=""
if (( HAS_SINK )); then
    SINK="$WORK/report.sink"
    REPORT_ARGS=(--report-sink "$SINK" --run-id "l0-bigdata" --spill-dir "$WORK")
fi

# ── the run, with its own peak RSS ──────────────────────────────────────
PEAK_FILE="$WORK/peak_kib"
SUMMARY_FILE="$WORK/summary.json"
set +e
python3 - "$PEAK_FILE" "$SUMMARY_FILE" "$RUN_JSON" --so "$SO" "${INPUT_ARGS[@]}" \
    --input-tf 1 --script-tf 1 "${REPORT_ARGS[@]}" <<'PY'
import json, os, subprocess, sys
peak_file, summary_file, *command = sys.argv[1:]
command = [sys.executable, *command]
with open(summary_file, "wb") as out, open(os.path.join(os.path.dirname(summary_file),
                                                        "run.stderr"), "wb") as err:
    child = subprocess.Popen(command, stdout=out, stderr=err)
    pid, status, usage = os.wait4(child.pid, 0)
    child.returncode = os.waitstatus_to_exitcode(status)
with open(peak_file, "w", encoding="ascii") as out:
    out.write(str(usage.ru_maxrss) + "\n")
sys.exit(child.returncode)
PY
RUN_STATUS=$?
set -e

PEAK_KIB="$(cat "$PEAK_FILE" 2>/dev/null || echo 0)"
if (( RUN_STATUS != 0 )); then
    printf '[flat-memory] RED: run_json exited %d (peak %d MiB) on the %s path\n' \
        "$RUN_STATUS" "$(( PEAK_KIB / 1024 ))" \
        "$( (( HAS_BIN )) && echo .bin || echo CSV )"
    tail -20 "$WORK/run.stderr" 2>/dev/null || true
    if (( RUN_STATUS == 137 )); then
        printf '[flat-memory] OOM-killed: the loader held about 300 B per bar (DESIGN.md 7 (c))\n'
    fi
    exit 1
fi
if (( PEAK_KIB / 1024 > RAM_LIMIT_MIB )); then
    printf '[flat-memory] FAIL: peak RSS %d MiB exceeds the 2 GiB box guard (%d MiB)\n' \
        "$(( PEAK_KIB / 1024 ))" "$RAM_LIMIT_MIB"
    exit 1
fi
printf '[flat-memory] peak RSS %d MiB\n' "$(( PEAK_KIB / 1024 ))"

# ── summary and sink sha256 ─────────────────────────────────────────────
python3 - "$SUMMARY_FILE" "$SINK" "$HAS_SINK" "$COMPARE" "$RECORD" <<'PY'
import hashlib, json, sys
summary_file, sink, has_sink, compare, record = sys.argv[1:6]
MASK = {"elapsed_seconds", "timing", "throughput", "phase_timing", "fingerprint",
        "source_bytes_sha256", "evaluated_source_values_sha256", "ohlcv", "report_sink"}
def mask(value):
    if isinstance(value, dict):
        return {k: mask(v) for k, v in value.items() if k not in MASK}
    if isinstance(value, list):
        return [mask(v) for v in value]
    return value
document = json.load(open(summary_file, encoding="utf-8"))
masked = mask(document)

sha = None
if has_sink == "1":
    raw = open(sink, "rb").read()
    lines = raw.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    trailer = json.loads(lines[-1])
    if trailer.get("trailer") != "pf-report-sink":
        raise SystemExit("the sink has no trailer")
    prefix = b"".join(line + b"\n" for line in lines[:-1])
    sha = hashlib.sha256(prefix).hexdigest()
    if sha != trailer.get("sha256"):
        raise SystemExit("the trailer sha256 does not equal hashlib over the prefix")
    print(f"  sink sha256: {sha}")
    print(f"  counts: {trailer.get('counts')}")

if record:
    with open(record, "w", encoding="utf-8") as out:
        json.dump({"summary": masked, "sink_sha256": sha}, out, sort_keys=True, indent=2)
        out.write("\n")
    print(f"  comparison written to {record}")

if compare:
    expected = json.load(open(compare, encoding="utf-8"))
    if masked != expected.get("summary"):
        raise SystemExit("masked summary differs from the comparison run")
    if sha != expected.get("sink_sha256"):
        raise SystemExit(f"sink sha256 {sha} differs from {expected.get('sink_sha256')}")
    print("  summary and sink sha256 equal the comparison run")
PY

if (( CREATED_WORK )); then
    rm -rf "$WORK"
fi
printf '[flat-memory] PASS\n'
