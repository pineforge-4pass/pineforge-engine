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
# Optional env var (instrument metadata):
#   PINEFORGE_SYMINFO    Path to a syminfo JSON file, a flat object or {"syminfo": {...}}.
#                        Keys applied: mincontract (the lot size: order quantities are
#                        floored to it), then mintick, pointvalue, timezone, session; other
#                        keys are ignored. mincontract absent or null: no lot grid. Any other
#                        mincontract that is not a positive finite number fails the run
#                        (exit 4, one {"engine":"pineforge","error":...} line on stdout).
#   PINEFORGE_SYMBOL_FEEDS  Path to a JSON index of other symbols' bars for
#                        request.security on another symbol: {"symbols": {"<symbol string>":
#                        {"syminfo": {...}, "feeds": {"<timeframe>": "<csv path>"}}}}, the
#                        symbol string exactly as the script passes it, one CSV per
#                        timeframe the script requests (paths relative to the index). Unset:
#                        nothing is installed and such a request stops the run where its
#                        value is read. An index or feed the harness cannot install fails
#                        the run (exit 4, one {"engine":"pineforge","error":...} line).
#
# Optional env vars (selected-window mode; unset PINEFORGE_REPORT_POLICY = the
# unchanged path above, which reads no inventory):
#   PINEFORGE_REPORT_POLICY   'selected-window/v1' turns the selected path on. A .pine is
#                        transpiled with transpile_with_request_inventory, with no chart
#                        timeframe binding; the generated C++ (exact UTF-8 bytes) and its
#                        source-bound request-feed inventory are kept in the per-run work dir.
#                        After g++ links the library, bind_compiled_inventory.py binds that
#                        inventory to the C++ and library bytes (pinned codegen binder) and the
#                        bound file goes to run_json.py as --request-feed-inventory. Any other
#                        non-empty value is forwarded to run_json.py as --report-policy to be
#                        refused there; no inventory is read or bound for it.
#   PINEFORGE_WINDOW_START_MS / PINEFORGE_WINDOW_END_MS / PINEFORGE_PREROLL_BARS /
#   PINEFORGE_FED_START_MS    forwarded, with the policy set and only when non-empty, as
#                        --window-start-ms / --window-end-ms / --preroll-bars / --fed-start-ms,
#                        raw. Nothing is defaulted or checked here: run_json.py validates them.
#   PINEFORGE_REQUEST_FEED_INVENTORY   Path to the source-bound inventory of a pre-transpiled
#                        /in/strategy.cpp, from the trusted caller; read only on the selected
#                        path, and not used with a .pine (that run makes its own). Unset: no
#                        inventory is passed to run_json.py, never an empty one, and run_json.py
#                        refuses the run. A supplied file that is unreadable, not strict JSON or
#                        not that of the C++ bytes compiled ends the run (exit 3).
#                        On the selected path /in/strategy.cpp is copied once into the per-run
#                        work dir, and that private copy is what g++ compiles, the binder reads
#                        and run_json.py gets as --generated-cpp (--transpiled stays false), so a
#                        change the caller makes to its file afterwards cannot change what was
#                        bound. A copy that fails ends the run (exit 3). The digest the binder
#                        checks is the inventory's own, never one read from the strategy code.
#
# Exit codes:
#   0  success (JSON report, or C++ in transpile-only mode, on stdout)
#   2  missing input mount
#   3  compile failure (g++; on the selected path also copying a pre-transpiled strategy.cpp
#      into the work dir, or binding the request-feed inventory)
#   4  backtest failure
#   5  transpile failure (unsupported Pine construct or syntax error)
set -euo pipefail

PREFIX="${PINEFORGE_PREFIX:-/opt/pineforge}"
IN_DIR="${PINEFORGE_IN_DIR:-/in}"
PINE="${IN_DIR}/strategy.pine"
SRC_CPP="${IN_DIR}/strategy.cpp"
OHLCV="${IN_DIR}/ohlcv.csv"
# Per-run work dir so parallel in-process invocations never collide on the
# generated TU / shared object. Cleaned up on exit. (Was fixed /tmp/strategy.*)
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
GEN="${WORK}/strategy.cpp"
SO="${WORK}/strategy.so"

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

# Selected path: transpile $1 once with transpile_with_request_inventory, passing no
# primary_chart_timeframe (the inventory stays chart-unbound). Writes the generated C++ to
# $2 as exact UTF-8 bytes (the inventory's source_sha256 is over those bytes) and the
# source-bound inventory (artifact_sha256 null) to $3. The C++ is the one run_transpile
# writes. Any failure, an installed codegen without the inventory API included, is exit 5
# with a message on stderr: there is no fallback to run_transpile.
run_transpile_with_inventory() {
    python3 - "$1" "$2" "$3" <<'PY'
import json
import sys

pine, out, inventory_out = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    from pineforge_codegen import transpile_with_request_inventory
    from pineforge_codegen.errors import CompileError
except Exception as e:  # a codegen without the inventory API
    sys.stderr.write(f"[pineforge] transpile error: no request-feed inventory API: {e}\n"); sys.exit(5)
try:
    result = transpile_with_request_inventory(open(pine).read(), filename="strategy.pine")
    cpp = result["cpp"]
    inventory = result["request_feed_inventory"]
    with open(out, "wb") as handle:
        handle.write(cpp.encode("utf-8"))
    with open(inventory_out, "w", encoding="utf-8") as handle:
        json.dump(inventory, handle, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
except CompileError as e:
    sys.stderr.write(f"[pineforge] transpile error: {e}\n"); sys.exit(5)
except Exception as e:  # syntax / unexpected — still a transpile failure
    sys.stderr.write(f"[pineforge] transpile error: {e}\n"); sys.exit(5)
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

# --- Selected-window path on? Only this exact policy value (see the header). -
SELECTED=false
if [[ "${PINEFORGE_REPORT_POLICY:-}" == "selected-window/v1" ]]; then
    SELECTED=true
fi
# The source-bound inventory the bind step will read; empty = none to bind.
INVENTORY_IN=""

# --- Resolve the translation unit: prefer .pine, fall back to .cpp. --------
if [[ -f "${PINE}" ]]; then
    echo "[pineforge] transpiling strategy.pine ..." >&2
    if [[ "${SELECTED}" == "true" ]]; then
        INVENTORY_IN="${WORK}/request_feed_inventory.source.json"
        run_transpile_with_inventory "${PINE}" "${GEN}" "${INVENTORY_IN}"   # set -e aborts (exit 5)
    else
        run_transpile "${PINE}" "${GEN}"   # set -e aborts (exit 5) on failure
    fi
    SRC="${GEN}"
    TRANSPILED=true
elif [[ -f "${SRC_CPP}" ]]; then
    SRC="${SRC_CPP}"
    TRANSPILED=false
    if [[ "${SELECTED}" == "true" ]]; then
        INVENTORY_IN="${PINEFORGE_REQUEST_FEED_INVENTORY:-}"
        # Freeze the caller's bytes once into the private work dir (GEN). From here g++, the
        # binder and run_json.py's --generated-cpp all use that one file, so the caller's
        # mutable file is read only here. A failed copy ends the run before compile and run.
        cp -- "${SRC_CPP}" "${GEN}" \
            || { echo "[pineforge] cannot snapshot strategy.cpp into the work dir" >&2; exit 3; }
        SRC="${GEN}"
    fi
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
# -ffp-contract=off is parity-critical: the static lib is built with it
# (CMakeLists.txt) so FMA contraction can't reassociate float ops. The strategy
# TU must match or any inlined TA/sizing math drifts at the last ULP.
g++ -std=c++17 -O2 -ffp-contract=off -fPIC -shared \
    -I"${PREFIX}/include" \
    -I/usr/include/eigen3 \
    "${SRC}" \
    -Wl,--whole-archive "${PREFIX}/lib/libpineforge.a" -Wl,--no-whole-archive \
    -o "${SO}" \
    || { echo "[pineforge] compile failed" >&2; exit 3; }

# --- Selected path: bind the inventory to the C++ and library just built. ---
# Reached only after g++ linked (a failed link exited 3 above). A refusal ends the
# run: there is no fallback to the ordinary path and run_json.py is not started.
# Without an inventory (a .cpp the caller gave none for) nothing is bound and none is
# invented; run_json.py gets no --request-feed-inventory and refuses the run.
BOUND_INVENTORY=""
if [[ "${SELECTED}" == "true" ]]; then
    if [[ -n "${INVENTORY_IN}" ]]; then
        echo "[pineforge] binding request-feed inventory ..." >&2
        BOUND_INVENTORY="${WORK}/request_feed_inventory.json"
        python3 "${PREFIX}/bin/bind_compiled_inventory.py" \
            --inventory "${INVENTORY_IN}" \
            --cpp "${SRC}" \
            --so "${SO}" \
            --out "${BOUND_INVENTORY}" \
            || { echo "[pineforge] request-feed inventory binding failed" >&2; exit 3; }
    else
        echo "[pineforge] no request-feed inventory for this strategy.cpp; none is passed on" >&2
    fi
fi

echo "[pineforge] running backtest ..." >&2

# Optional per-run knobs (mirror scripts/run_strategy.py). Built conditionally so
# unset env never passes an empty/invalid flag.
#   PINEFORGE_TRADE_START_MS            unix-ms; suppress orders before it
#   PINEFORGE_CHART_TZ                  IANA tz for date builtins
#   PINEFORGE_MAGNIFIER_VOLUME_WEIGHTED 1/true → vw magnifier (needs BAR_MAGNIFIER)
#   PINEFORGE_SYMINFO                   path to a syminfo.json (see the header)
#   PINEFORGE_SYMBOL_FEEDS              path to other symbols' feed index (see the header)
#   PINEFORGE_BENCH (+_WARMUP/_REPEATS) 1/true → timing mode
extra=()
[[ -n "${PINEFORGE_TRADE_START_MS:-}" ]] && extra+=(--trade-start-ms "${PINEFORGE_TRADE_START_MS}")
[[ -n "${PINEFORGE_CHART_TZ:-}" ]]       && extra+=(--chart-tz "${PINEFORGE_CHART_TZ}")
[[ "${PINEFORGE_MAGNIFIER_VOLUME_WEIGHTED:-}" =~ ^(1|true|yes|on)$ ]] && extra+=(--magnifier-volume-weighted)
[[ -n "${PINEFORGE_SYMINFO:-}" ]]        && extra+=(--syminfo "${PINEFORGE_SYMINFO}")
[[ -n "${PINEFORGE_SYMBOL_FEEDS:-}" ]]   && extra+=(--symbol-feeds "${PINEFORGE_SYMBOL_FEEDS}")
if [[ "${PINEFORGE_BENCH:-}" =~ ^(1|true|yes|on)$ ]]; then
    extra+=(--bench --warmup "${PINEFORGE_WARMUP:-3}" --repeats "${PINEFORGE_REPEATS:-20}")
fi
# Selected-window flags (see the header): raw values for run_json.py to validate, none
# defaulted here, and the inventory bound above.
if [[ -n "${PINEFORGE_REPORT_POLICY:-}" ]]; then
    extra+=(--report-policy "${PINEFORGE_REPORT_POLICY}")
    if [[ -n "${PINEFORGE_WINDOW_START_MS:-}" ]]; then
        extra+=(--window-start-ms "${PINEFORGE_WINDOW_START_MS}")
    fi
    if [[ -n "${PINEFORGE_WINDOW_END_MS:-}" ]]; then
        extra+=(--window-end-ms "${PINEFORGE_WINDOW_END_MS}")
    fi
    if [[ -n "${PINEFORGE_PREROLL_BARS:-}" ]]; then
        extra+=(--preroll-bars "${PINEFORGE_PREROLL_BARS}")
    fi
    if [[ -n "${PINEFORGE_FED_START_MS:-}" ]]; then
        extra+=(--fed-start-ms "${PINEFORGE_FED_START_MS}")
    fi
fi
if [[ -n "${BOUND_INVENTORY}" ]]; then
    extra+=(--request-feed-inventory "${BOUND_INVENTORY}")
fi

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
    --generated-cpp     "${SRC}" \
    --transpiled        "${TRANSPILED}" \
    ${extra[@]+"${extra[@]}"} \
    || { echo "[pineforge] backtest failed" >&2; exit 4; }
