#!/usr/bin/env bash
# PineForge container entrypoint.
#
# Compiles the user-supplied strategy translation unit at /in/strategy.cpp
# against the prebuilt libpineforge.a, then runs it against the OHLCV at
# /in/ohlcv.csv and emits a JSON report on stdout. Build / compile logs
# go to stderr so stdout stays clean for piping into `jq` etc.
#
# Mount points:
#   /in/strategy.cpp  required  user's generated.cpp
#   /in/ohlcv.csv     required  CSV: timestamp,open,high,low,close,volume
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
#   0  success, JSON written to stdout
#   2  missing input mount
#   3  compile failure
#   4  backtest failure
set -euo pipefail

PREFIX="${PINEFORGE_PREFIX:-/opt/pineforge}"
SRC=/in/strategy.cpp
OHLCV=/in/ohlcv.csv
SO=/tmp/strategy.so

if [[ ! -f "${SRC}" ]]; then
    echo "error: missing /in/strategy.cpp (mount with -v path/to/strategy.cpp:/in/strategy.cpp:ro)" >&2
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
