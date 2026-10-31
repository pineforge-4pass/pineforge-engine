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
# Optional compile guards (Linux; each is OFF when unset):
#   PINEFORGE_COMPILE_WALL_MS       Absolute compiler/helper/binder lifetime in ms.
#   PINEFORGE_COMPILE_AS_BYTES      Inherited per-process RLIMIT_AS, soft=hard, in bytes.
#   PINEFORGE_COMPILE_DIAGNOSTIC_MAX_BYTES
#                                  Combined compiler/binder stdout+stderr byte cap.
# Set values are unsigned decimal integers in 0..9007199254740990. Zero selects
# a finite local policy: 120000 ms, 4294967296 AS bytes, or 1048576 diagnostic
# bytes, respectively (AS respects a lower inherited hard cap); it never disables
# a guard. Other unset dimensions stay
# unset. Positive values are exact; an AS cap above the inherited hard limit is
# refused. Empty, signed, whitespace, nondecimal and out-of-range values are
# usage errors (run_request_invalid, exit 4), before compiler launch.
# Deadline: run_deadline_exceeded{stage:compile}; diagnostic overflow:
# diagnostic_budget_exceeded{cap_bytes,used_bytes,requested_bytes:1}; exit 4.
# Stops kill/reap compiler descendants, including adopted orphans, with at most
# 1000 ms cleanup grace. Capture reads at most cap+1 and forwards at most cap,
# as raw bytes on stderr. The fixed shell progress/failure messages are separate.
# These are cooperative engine guards, not a host isolation or memory quota.
# AS failures alone retain ordinary compile exit 3, without inferred cause.
# No compile guard set: the original g++/binder path, arguments and environment.
# Details: docker/README.md, "Opt-in compile guards".
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
#   PINEFORGE_RUN_ID             Sink header id: raw --run-id, even empty (no limit added here: a wrapper that needs one refuses empty itself); unset keeps default ""; run_json.py validates UTF-8 without NUL.
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
#   4  backtest failure, or an opt-in compile guard's usage/limit/channel failure
#   5  transpile failure (unsupported Pine construct or syntax error)
#
# Memory and scratch controls (environment): PINEFORGE_COMMAND_SCRATCH_MAX_BYTES
# caps the bytes the command-store scratch may extend (one thread, in total);
# PINEFORGE_COMMAND_SCRATCH_DIR or TMPDIR picks the disk-backed scratch directory;
# PF_SCRATCH_ARENA_FD=5 with PF_SCRATCH_ARENA_BYTES supplies an inherited arena,
# PF_SCRATCH_ARENA_JAILED=1 requires a sealed memfd, and PF_SCRATCH_WINDOW_BYTES
# (default 32 MiB) caps what is mapped at once; PINEFORGE_RUN_JSON_PROFILE=jailed
# selects the bounded-memory profile. Defaults, units and limits:
# docs/pages/memory-management.md in the engine repository.
set -euo pipefail

PREFIX="${PINEFORGE_PREFIX:-/opt/pineforge}"
IN_DIR="${PINEFORGE_IN_DIR:-/in}"
PINE="${IN_DIR}/strategy.pine"
SRC_CPP="${IN_DIR}/strategy.cpp"
OHLCV="${IN_DIR}/ohlcv.csv"
# Per-run work dir so parallel in-process invocations never collide on the
# generated TU / shared object. Cleaned up on exit. (Was fixed /tmp/strategy.*)
WORK="$(mktemp -d "${TMPDIR:-/tmp}/pf-work.XXXXXXXXXX")"
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
        run_transpile_with_inventory "${PINE}" "${GEN}" "${INVENTORY_IN}" >&2   # set -e aborts (exit 5)
    else
        run_transpile "${PINE}" "${GEN}" >&2   # set -e aborts (exit 5) on failure
    fi
    SRC="${GEN}"
    TRANSPILED=true
elif [[ -f "${SRC_CPP}" ]]; then
    SRC="${SRC_CPP}"
    TRANSPILED=false
    if [[ "${SELECTED}" == "true" ]]; then
        INVENTORY_IN="${PINEFORGE_REQUEST_FEED_INVENTORY:-}"
    fi
    # Freeze the caller's bytes once into the private work dir (GEN). From here g++, the
    # binder and run_json.py's --generated-cpp all use that one file, so the caller's
    # mutable file is read only here. A failed copy ends the run before compile and run.
    # The unattested local-only path (PINEFORGE_ALLOW_UNATTESTED_CPP=1) freezes only on the
    # selected-window path, as before.
    if [[ "${SELECTED}" == "true" || "${PINEFORGE_ALLOW_UNATTESTED_CPP:-}" != "1" ]]; then
        cp -- "${SRC_CPP}" "${GEN}" >&2 \
            || { echo "[pineforge] cannot snapshot strategy.cpp into the work dir" >&2; exit 3; }
        SRC="${GEN}"
    fi
    # A mounted strategy.cpp is native code run in this process. Compile it only when the
    # host vouches for these exact bytes (PINEFORGE_STRATEGY_CPP_SHA256, set in the container's
    # environment, which the mount's author does not control), or on the explicit local opt-out.
    # The digest is taken over the frozen copy that g++ then reads, so there is no gap between
    # the check and the use.
    if [[ "${PINEFORGE_ALLOW_UNATTESTED_CPP:-}" != "1" ]]; then
        want="${PINEFORGE_STRATEGY_CPP_SHA256:-}"
        if command -v sha256sum >/dev/null 2>&1; then
            have="$(sha256sum -- "${GEN}")"
        else
            have="$(shasum -a 256 -- "${GEN}")"
        fi
        have="${have%% *}"
        if [[ ! "${want}" =~ ^[0-9a-f]{64}$ || "${have}" != "${want}" ]]; then
            echo "[pineforge] refused: mounted strategy.cpp is not vouched for by PINEFORGE_STRATEGY_CPP_SHA256" >&2
            # Same shape as the catalog's "{prog}: error: {argparse message}" text for run_request_invalid.
            printf '%s\n' '{"engine":"pineforge","error":"entrypoint.sh: error: mounted strategy.cpp is not vouched for by a matching PINEFORGE_STRATEGY_CPP_SHA256","code":"run_request_invalid","args":{"option":"generated_cpp"}}'
            exit 2
        fi
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
if [[ -z "${PINEFORGE_COMPILE_WALL_MS+x}" &&
      -z "${PINEFORGE_COMPILE_AS_BYTES+x}" &&
      -z "${PINEFORGE_COMPILE_DIAGNOSTIC_MAX_BYTES+x}" ]]; then
g++ -std=c++17 -O2 -ffp-contract=off -fPIC -shared \
    -I"${PREFIX}/include" \
    -I/usr/include/eigen3 \
    "${SRC}" \
    -Wl,--whole-archive "${PREFIX}/lib/libpineforge.a" -Wl,--no-whole-archive \
    -o "${SO}" >&2 \
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
            --out "${BOUND_INVENTORY}" >&2 \
            || { echo "[pineforge] request-feed inventory binding failed" >&2; exit 3; }
    else
        echo "[pineforge] no request-feed inventory for this strategy.cpp; none is passed on" >&2
    fi
fi
else
    BOUND_INVENTORY=""
    if [[ "${SELECTED}" == "true" && -n "${INVENTORY_IN}" ]]; then
        BOUND_INVENTORY="${WORK}/request_feed_inventory.json"
    fi
    python3 - "${PREFIX}" "${SRC}" "${SO}" "${BOUND_INVENTORY}" "${INVENTORY_IN}" <<'PY' || exit "$?"
import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import subprocess
import sys
import time


class GuardFailure(Exception):
    def __init__(self, code, args, text):
        super().__init__(text)
        self.code, self.args_json, self.text = code, args, text


def usage(text):
    raise GuardFailure("run_request_invalid", {"option": "arguments"},
                       "entrypoint.sh: error: " + text)


def ceiling(name, policy):
    if name not in os.environ:
        return None
    raw = os.environ[name]
    if not re.fullmatch(r"[0-9]+", raw):
        usage(name + " must be an unsigned decimal integer in 0..9007199254740990")
    digits = raw.lstrip("0") or "0"
    if len(digits) > 16 or int(digits) > 9007199254740990:
        usage(name + " is outside 0..9007199254740990")
    value = int(digits)
    return policy if value == 0 else value


def now_ns():
    return time.clock_gettime_ns(time.CLOCK_BOOTTIME)


cancelled = False


def cancel(_signum, _frame):
    global cancelled
    cancelled = True


class CompileGuard:
    def __init__(self):
        self.wall = ceiling("PINEFORGE_COMPILE_WALL_MS", 120000)
        self.address = ceiling("PINEFORGE_COMPILE_AS_BYTES", 4294967296)
        self.cap = ceiling("PINEFORGE_COMPILE_DIAGNOSTIC_MAX_BYTES", 1048576)
        if sys.platform != "linux" or not hasattr(time, "CLOCK_BOOTTIME"):
            usage("compile guards require Linux with CLOCK_BOOTTIME and procfs")
        self.children_file = Path(f"/proc/self/task/{os.getpid()}/children")
        try:
            self.children_file.read_text()
            libc = ctypes.CDLL(None, use_errno=True)
            # Linux PR_SET_CHILD_SUBREAPER: detached orphans become our children.
            if libc.prctl(36, 1, 0, 0, 0) != 0:
                raise OSError(ctypes.get_errno(), "cannot become a child subreaper")
        except (OSError, AttributeError):
            usage("compile guards require procfs and child-subreaper support")
        if self.address is not None:
            hard = resource.getrlimit(resource.RLIMIT_AS)[1]
            if hard != resource.RLIM_INFINITY and self.address > hard:
                if os.environ["PINEFORGE_COMPILE_AS_BYTES"].strip("0") == "" and hard > 0:
                    self.address = hard
                else:
                    usage("PINEFORGE_COMPILE_AS_BYTES exceeds the inherited hard limit")
        self.accepted = 0
        self.deadline = None if self.wall is None else now_ns() + self.wall * 1000000
        self.original_stderr_flags = fcntl.fcntl(2, fcntl.F_GETFL)
        fcntl.fcntl(2, fcntl.F_SETFL, self.original_stderr_flags | os.O_NONBLOCK)
        signal.signal(signal.SIGTERM, cancel)
        signal.signal(signal.SIGINT, cancel)

    def restore(self):
        fcntl.fcntl(2, fcntl.F_SETFL, self.original_stderr_flags)

    def install_child_limits(self):
        if self.address is not None:
            resource.setrlimit(resource.RLIMIT_AS, (self.address, self.address))

    def kill_children(self):
        # Direct children cannot have their PIDs recycled before we reap them.
        # Killing parents adopts further descendants; repeat until none remain.
        for spelling in self.children_file.read_text().split():
            try:
                os.kill(int(spelling), signal.SIGKILL)
            except ProcessLookupError:
                pass

    @staticmethod
    def reap(driver):
        while True:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return False
            if pid == 0:
                return True
            if pid == driver.pid:
                driver.returncode = os.waitstatus_to_exitcode(status)

    def run(self, command, failure_message):
        if cancelled:
            raise GuardFailure("run_aborted", {}, "native run aborted")
        if self.deadline is not None and now_ns() >= self.deadline:
            raise GuardFailure("run_deadline_exceeded", {"stage": "compile"},
                               "The admitted channel lifetime expired.")
        try:
            driver = subprocess.Popen(command, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, start_new_session=True,
                                      preexec_fn=self.install_child_limits)
        except OSError:
            # A failed compiler exec is not proof of a particular resource denial.
            sys.stderr.write(failure_message)
            return 3
        except subprocess.SubprocessError:
            usage("could not install PINEFORGE_COMPILE_AS_BYTES before compiler launch")
        reader = driver.stdout
        os.set_blocking(reader.fileno(), False)
        pending = bytearray()
        primary = None
        cleanup_deadline = None

        def latch(failure):
            nonlocal primary, cleanup_deadline, reader
            if primary is None:
                primary = failure
                cleanup_deadline = now_ns() + 1000000000
            if reader is not None:
                reader.close()
                reader = None

        try:
            while True:
                if primary is not None:
                    self.kill_children()
                children_alive = self.reap(driver)
                current = now_ns()
                if cancelled:
                    latch(GuardFailure("run_aborted", {}, "native run aborted"))
                if primary is None and self.deadline is not None and current >= self.deadline:
                    latch(GuardFailure("run_deadline_exceeded", {"stage": "compile"},
                                       "The admitted channel lifetime expired."))
                if not children_alive and reader is None and not pending:
                    break
                if cleanup_deadline is not None and current >= cleanup_deadline:
                    self.kill_children()
                    self.reap(driver)
                    break

                poller = select.poll()
                read_fd = None
                if reader is not None and len(pending) < 65536:
                    read_fd = reader.fileno()
                    poller.register(read_fd, select.POLLIN | select.POLLHUP | select.POLLERR)
                if pending:
                    poller.register(2, select.POLLOUT | select.POLLHUP | select.POLLERR)
                expiry = cleanup_deadline if primary is not None else self.deadline
                wait_ms = 10
                if expiry is not None:
                    wait_ms = min(wait_ms, max(0, (expiry - now_ns() + 999999) // 1000000))
                events = dict(poller.poll(wait_ms))
                if read_fd is not None and read_fd in events:
                    amount = 65536 - len(pending)
                    if self.cap is not None:
                        amount = min(amount, self.cap - self.accepted + 1)
                    try:
                        block = os.read(read_fd, amount)
                    except BlockingIOError:
                        block = None
                    if block == b"":
                        reader.close()
                        reader = None
                    elif block:
                        take = len(block)
                        if self.cap is not None:
                            take = min(take, self.cap - self.accepted)
                        pending.extend(block[:take])
                        self.accepted += take
                        if take < len(block):
                            latch(GuardFailure(
                                "diagnostic_budget_exceeded",
                                {"cap_bytes": self.cap, "used_bytes": self.accepted,
                                 "requested_bytes": 1},
                                "The diagnostics exceed their admitted byte budget."))
                if pending and 2 in events:
                    try:
                        written = os.write(2, pending)
                        if written == 0:
                            raise OSError(errno.EIO, "zero diagnostic write")
                        del pending[:written]
                    except BlockingIOError:
                        pass
                    except OSError as error:
                        reason = "peer_closed" if error.errno in (
                            errno.EPIPE, errno.ECONNRESET, errno.ENOTCONN) else "io"
                        latch(GuardFailure("report_channel_failed",
                                           {"channel": "diagnostic", "reason": reason},
                                           "A report transport could not complete."))
                        pending.clear()
            if primary is not None:
                raise primary
            if driver.returncode != 0:
                sys.stderr.write(failure_message)
                return 3
            return 0
        finally:
            if reader is not None:
                reader.close()
            # Also stop descendants on an unexpected controller exception.
            until = cleanup_deadline if cleanup_deadline is not None else now_ns() + 1000000000
            while self.reap(driver) and now_ns() < until:
                self.kill_children()
                select.poll().poll(1)


def main():
    prefix, source, shared_object, bound_inventory, inventory = sys.argv[1:]
    guard = CompileGuard()
    try:
        command = [
            "g++", "-std=c++17", "-O2", "-ffp-contract=off", "-fPIC", "-shared",
            "-I" + prefix + "/include", "-I/usr/include/eigen3", source,
            "-Wl,--whole-archive", prefix + "/lib/libpineforge.a", "-Wl,--no-whole-archive",
            "-o", shared_object,
        ]
        status = guard.run(command, "[pineforge] compile failed\n")
        if status:
            return status
        if bound_inventory:
            return guard.run(
                ["python3", prefix + "/bin/bind_compiled_inventory.py",
                 "--inventory", inventory, "--cpp", source, "--so", shared_object,
                 "--out", bound_inventory],
                "[pineforge] request-feed inventory binding failed\n")
        return 0
    finally:
        guard.restore()


try:
    status = main()
except GuardFailure as failure:
    if failure.code == "run_request_invalid":
        sys.stderr.write(failure.text + "\n")
    try:
        sys.stdout.write(json.dumps({"engine": "pineforge", "error": failure.text,
                                    "code": failure.code, "args": failure.args_json},
                                   separators=(",", ":")) + "\n")
        sys.stdout.flush()
    except OSError:
        os._exit(4)
    status = 4
except Exception:
    try:
        sys.stdout.write('{"engine":"pineforge","error":"The compile guard could not complete.",'
                         '"code":"harness_internal_error","args":{}}\n')
        sys.stdout.flush()
    except OSError:
        os._exit(4)
    status = 4
sys.exit(status)
PY
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
[[ -n "${PINEFORGE_REPORT_SINK_FD+x}" ]] && extra+=(--report-sink-fd "${PINEFORGE_REPORT_SINK_FD}")  # set values incl. empty; run_json.py validates
[[ -n "${PINEFORGE_RUN_ID+x}" ]] && extra+=(--run-id "${PINEFORGE_RUN_ID}")  # set values incl. empty; run_json.py validates
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
