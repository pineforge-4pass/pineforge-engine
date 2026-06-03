#!/usr/bin/env bash
# PineForge container entrypoint.
#
# Accepts a PineScript v6 strategy (/in/strategy.pine) OR a pre-transpiled
# translation unit (/in/strategy.cpp). When a .pine is given, it is transpiled
# locally with the bundled pineforge-codegen (no hosted API, no API key, source
# never leaves the container). The TU is then compiled against the prebuilt
# libpineforge.a and run against the OHLCV at /in/ohlcv.csv; a JSON report is
# emitted on stdout. Build / compile / transpile logs go to stderr so stdout
# stays clean for piping into `jq` etc.
#
# Mount points:
#   /in/strategy.pine  user's PineScript v6 source   (preferred)
#   /in/strategy.cpp   pre-transpiled TU             (back-compat; used if no .pine)
#   /in/ohlcv.csv      required (unless transpile-only): timestamp,open,high,low,close,volume
#   (provide exactly one of strategy.pine / strategy.cpp)
#
# Transpile-only mode:
#   PINEFORGE_TRANSPILE_ONLY=1   transpile /in/strategy.pine and write the C++
#                                to stdout, then exit (no compile, no backtest,
#                                no OHLCV needed).
#
# Optional env vars (parameter overrides — applied before backtest):
#   PINEFORGE_INPUTS     JSON object of input.*() name -> value
#                        e.g. '{"Fast Length": "8", "Slow Length": "21"}'
#   PINEFORGE_OVERRIDES  JSON object of strategy() header field -> value
#                        e.g. '{"default_qty_value": "5", "commission_value": "0.04"}'
#
# Optional env vars (runtime args — applied to run_backtest_full):
#   PINEFORGE_INPUT_TF           Chart bar timeframe ('1','5','15','60','D',...).
#                                Empty / unset = auto-detect from bar timestamps.
#   PINEFORGE_SCRIPT_TF          Strategy timeframe; empty = same as input_tf.
#                                Must be coarser than or equal to input_tf.
#   PINEFORGE_BAR_MAGNIFIER      'true' / 'false' (default false).
#   PINEFORGE_MAGNIFIER_SAMPLES  Sub-bar sample count when magnifier=true (>=2, default 4).
#   PINEFORGE_MAGNIFIER_DIST     Sample distribution: uniform / cosine / triangle /
#                                endpoints (default) / front_loaded / back_loaded.
#
# Exit codes:
#   0  success (JSON report, or C++ in transpile-only mode, on stdout)
#   2  missing input mount
#   3  compile failure
#   4  backtest failure
#   5  transpile failure (unsupported Pine construct or syntax error)
set -euo pipefail

PREFIX="${PINEFORGE_PREFIX:-/opt/pineforge}"
PINE=/in/strategy.pine
SRC_CPP=/in/strategy.cpp
OHLCV=/in/ohlcv.csv
GEN=/tmp/strategy.cpp
SO=/tmp/strategy.so

# Transpile Pine -> C++. $1 = pine path, $2 = output path ('-' for stdout).
# Maps any transpile/parse error to exit 5 with a clean message on stderr.
run_transpile() {
    python3 - "$1" "$2" <<'PY'
import sys
from pineforge_codegen import transpile
from pineforge_codegen.errors import CompileError

pine, out = sys.argv[1], sys.argv[2]
try:
    cpp = transpile(open(pine).read(), filename="strategy.pine")
except CompileError as e:
    sys.stderr.write(f"[pineforge] transpile error: {e}\n"); sys.exit(5)
except Exception as e:  # syntax / unexpected — still a transpile failure
    sys.stderr.write(f"[pineforge] transpile error: {e}\n"); sys.exit(5)
if out == "-":
    sys.stdout.write(cpp)
else:
    open(out, "w").write(cpp)
PY
}

# --- Transpile-only mode: emit C++ on stdout and exit. ---------------------
if [[ "${PINEFORGE_TRANSPILE_ONLY:-}" == "1" || "${PINEFORGE_TRANSPILE_ONLY:-}" == "true" ]]; then
    if [[ ! -f "${PINE}" ]]; then
        echo "error: transpile-only mode needs /in/strategy.pine (mount with -v path/to/strategy.pine:/in/strategy.pine:ro)" >&2
        exit 2
    fi
    run_transpile "${PINE}" "-"
    exit 0
fi

# --- Resolve the translation unit: prefer .pine, fall back to .cpp. --------
if [[ -f "${PINE}" ]]; then
    echo "[pineforge] transpiling strategy.pine ..." >&2
    run_transpile "${PINE}" "${GEN}"   # set -e aborts (exit 5) on failure
    SRC="${GEN}"
elif [[ -f "${SRC_CPP}" ]]; then
    SRC="${SRC_CPP}"
else
    echo "error: missing input — mount /in/strategy.pine (preferred) or /in/strategy.cpp" >&2
    exit 2
fi

if [[ ! -f "${OHLCV}" ]]; then
    echo "error: missing /in/ohlcv.csv (mount with -v path/to/ohlcv.csv:/in/ohlcv.csv:ro)" >&2
    exit 2
fi

echo "[pineforge] compiling strategy.cpp ..." >&2

# Same link incantation as tutorial/CMakeLists.txt, condensed:
# whole-archive forces the c_abi.cpp symbols (pf_version_get,
# strategy_set_trace_enabled, etc.) into the .so even though the
# strategy body never references them.
g++ -std=c++17 -O2 -fPIC -shared \
    -I"${PREFIX}/include" \
    -I/usr/include/eigen3 \
    "${SRC}" \
    -Wl,--whole-archive "${PREFIX}/lib/libpineforge.a" -Wl,--no-whole-archive \
    -o "${SO}" \
    || { echo "[pineforge] compile failed" >&2; exit 3; }

echo "[pineforge] running backtest ..." >&2

python3 "${PREFIX}/bin/run_json.py" \
    --so "${SO}" \
    --ohlcv "${OHLCV}" \
    --inputs    "${PINEFORGE_INPUTS:-}" \
    --overrides "${PINEFORGE_OVERRIDES:-}" \
    --input-tf          "${PINEFORGE_INPUT_TF:-}" \
    --script-tf         "${PINEFORGE_SCRIPT_TF:-}" \
    --bar-magnifier     "${PINEFORGE_BAR_MAGNIFIER:-}" \
    --magnifier-samples "${PINEFORGE_MAGNIFIER_SAMPLES:-4}" \
    --magnifier-dist    "${PINEFORGE_MAGNIFIER_DIST:-endpoints}" \
    || { echo "[pineforge] backtest failed" >&2; exit 4; }
