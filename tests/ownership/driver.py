#!/usr/bin/env python3
"""Real ordinary/Selected-window Pine -> codegen -> g++ -> run_json probes (Python 3.10+)."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXPECT_KEYS = {
    "outcome", "code", "args", "min_trades", "label", "pinned_by",
    "native_boundary_control",
}
EXPECT_OPTIONAL_KEYS = {"phase_final"}
SELECTED_FLAGS = {
    "--report-policy", "--window-start-ms", "--window-end-ms", "--preroll-bars", "--fed-start-ms",
}
RUN_PHASES = ("preflight", "execution", "result_assembly", "results_digest",
              "serialization", "completed")
MAX_PHASE_RECORD_BYTES = 1024
MAX_PHASE_RECORDS = 6
RUN_DEFAULTS = {
    "input_tf": "", "script_tf": "", "inputs": "", "overrides": "",
    "bar_magnifier": "", "magnifier_samples": 4, "magnifier_dist": "endpoints",
    "options": [],
}
OWNED_FLAGS = {
    "--so", "--ohlcv", "--inputs", "--overrides", "--input-tf", "--script-tf",
    "--bar-magnifier", "--magnifier-samples", "--magnifier-dist",
    "--generated-cpp", "--transpiled",
    # Admission-only routes skip execution; inventory and transport are produced
    # by the driver on the Selected route and must never be supplied by a probe.
    "--capabilities-json", "--validate-window-only", "--run-phase-fd",
    "--request-feed-inventory", "--help", "-h", "--",
}
STAGE_TIMEOUT_SECONDS = 300


class ProbeError(ValueError):
    """An invalid probe, expectation, toolchain or subprocess protocol."""


def _unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ProbeError(f"duplicate JSON key: {key}")
        obj[key] = value
    return obj


def _finite_json(value):
    if isinstance(value, float) and not math.isfinite(value):
        raise ProbeError("nonfinite JSON number")
    if isinstance(value, dict):
        for child in value.values():
            _finite_json(child)
    elif isinstance(value, list):
        for child in value:
            _finite_json(child)


def _bad_constant(value):
    raise ProbeError(f"non-JSON numeric token: {value}")


def load_json(raw):
    try:
        doc = json.loads(raw, object_pairs_hook=_unique_object, parse_constant=_bad_constant)
        _finite_json(doc)
        return doc
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ProbeError(f"invalid JSON: {error}") from error


def validate_expectation(doc):
    if not isinstance(doc, dict):
        raise ProbeError("expect.json must be an object")
    missing = sorted(EXPECT_KEYS - set(doc))
    extra = sorted(set(doc) - EXPECT_KEYS - EXPECT_OPTIONAL_KEYS)
    if missing or extra:
        raise ProbeError(f"expect.json keys: missing={missing}, unknown={extra}")
    if "phase_final" in doc and (not isinstance(doc["phase_final"], str)
                                  or doc["phase_final"] not in RUN_PHASES):
        raise ProbeError("phase_final must name a run phase: " + ", ".join(RUN_PHASES))
    if doc["outcome"] not in ("completed", "failed", "compile_refused"):
        raise ProbeError("outcome must be completed, failed or compile_refused")
    if doc["label"] not in ("S", "G", "R", "control"):
        raise ProbeError("label must be S, G, R or control")
    if type(doc["min_trades"]) is not int or doc["min_trades"] < 0:
        raise ProbeError("min_trades must be a nonnegative integer")
    if not isinstance(doc["args"], dict):
        raise ProbeError("args must be an object")
    if not isinstance(doc["pinned_by"], str) or not doc["pinned_by"].strip():
        raise ProbeError("pinned_by must name the contract row")
    control = doc["native_boundary_control"]
    if control is not None and (not isinstance(control, str) or not control.strip() or "\x00" in control):
        raise ProbeError("native_boundary_control must be a file path or null")
    if doc["outcome"] == "completed":
        if doc["code"] is not None or doc["args"]:
            raise ProbeError("completed expects code=null and args={}")
    else:
        if not isinstance(doc["code"], str) or not doc["code"].strip():
            raise ProbeError("failed/compile_refused expects a nonempty code")
        if doc["min_trades"] != 0:
            raise ProbeError("failed/compile_refused expects min_trades=0")
    return doc


def load_expectation(path):
    return validate_expectation(load_json(Path(path).read_bytes()))


def selected_request(options):
    """Recognize the five exact window flags in S1a's existing options array.

    Values stay verbatim for run_json's request validation; a partial request,
    duplicate flag or unsupported policy must never choose ordinary execution.
    """
    extra = options.get("options", [])
    declared = {}
    index = 0
    while index < len(extra):
        name, equals, value = extra[index].partition("=")
        index += 1
        if name not in SELECTED_FLAGS:
            continue
        if name in declared:
            raise ProbeError(f"run.json duplicates selected flag {name}")
        if not equals:
            if index == len(extra) or extra[index].startswith("--"):
                raise ProbeError(f"run.json selected flag {name} needs a value")
            value = extra[index]
            index += 1
        if not value:
            raise ProbeError(f"run.json selected flag {name} needs a value")
        declared[name] = value
    if not declared:
        return None
    if declared.get("--report-policy") != "selected-window/v1":
        raise ProbeError("run.json selected route requires --report-policy selected-window/v1")
    missing = sorted(SELECTED_FLAGS - declared.keys())
    if missing:
        raise ProbeError(f"run.json selected route is missing {missing}")
    if not options.get("input_tf", "").strip() or not options.get("script_tf", "").strip():
        raise ProbeError("run.json selected route requires explicit input_tf and script_tf timeframes")
    return declared


def load_run_options(path):
    path = Path(path)
    doc = load_json(path.read_bytes()) if path.exists() else {}
    if not isinstance(doc, dict) or set(doc) - set(RUN_DEFAULTS):
        raise ProbeError("run.json must be an object with documented option keys")
    options = {**RUN_DEFAULTS, "options": [], **doc}
    for key in ("input_tf", "script_tf", "bar_magnifier", "magnifier_dist"):
        if not isinstance(options[key], str) or "\x00" in options[key]:
            raise ProbeError(f"run.json {key} must be a string")
    for key in ("inputs", "overrides"):
        value = options[key]
        if isinstance(value, dict):
            options[key] = json.dumps(value, allow_nan=False, separators=(",", ":"))
        elif not isinstance(value, str) or "\x00" in value:
            raise ProbeError(f"run.json {key} must be a JSON object or string")
    if type(options["magnifier_samples"]) is not int:
        raise ProbeError("run.json magnifier_samples must be an integer")
    extra = options["options"]
    if not isinstance(extra, list) or any(not isinstance(token, str) for token in extra):
        raise ProbeError("run.json options must be an argv array of strings")
    for token in extra:
        name = token.partition("=")[0]
        if name in OWNED_FLAGS or (name.startswith("--") and name not in SELECTED_FLAGS and any(
            flag.startswith(name) for flag in OWNED_FLAGS | SELECTED_FLAGS
        )):
            raise ProbeError(f"run.json options cannot supply driver-owned flag {name}")
        if "\x00" in token:
            raise ProbeError("run.json options cannot contain NUL")
    # run_json resolves a symbol-feeds index's nested CSV paths itself. Resolve
    # the two outer file options against the probe, rather than the scratch cwd.
    extra = list(extra)
    for index, token in enumerate(extra):
        flag, equals, value = token.partition("=")
        if flag in ("--syminfo", "--symbol-feeds"):
            if equals:
                extra[index] = flag + "=" + str((path.parent / value).resolve())
            elif index + 1 < len(extra) and not extra[index + 1].startswith("--"):
                extra[index + 1] = str((path.parent / extra[index + 1]).resolve())
            else:
                raise ProbeError(f"run.json option {flag} needs a file path")
    options["options"] = extra
    selected_request(options)
    return options


def observation(outcome, code=None, args=None, *, trade_count=None,
                exit_status=None, stage="runtime", detail=""):
    return {
        "outcome": outcome, "code": code, "args": {} if args is None else args,
        "trade_count": trade_count, "exit_status": exit_status, "stage": stage,
        "detail": detail,
    }


def observe_runtime(stdout, exit_status):
    """Accept exactly one report OR typed failure, consistent with process status."""
    try:
        lines = stdout.decode("utf-8").splitlines()
        if len(lines) != 1 or not lines[0].strip():
            raise ProbeError(f"run_json printed {len(lines)} lines; expected ONE JSON line")
        doc = load_json(lines[0])
        if not isinstance(doc, dict) or doc.get("engine") != "pineforge":
            raise ProbeError("run_json result must be a pineforge object")
        if "error" in doc:
            if exit_status <= 0:
                raise ProbeError("typed failure requires a positive nonzero exit status")
            if not isinstance(doc["error"], str):
                raise ProbeError("failure error must be a string")
            if not isinstance(doc.get("code"), str) or not doc["code"]:
                raise ProbeError("failure is missing a typed code")
            if not isinstance(doc.get("args"), dict):
                raise ProbeError("failure is missing its args object")
            if {"summary", "trades", "metrics", "equity_curve"} & doc.keys():
                raise ProbeError("failure also contains a success report")
            return observation("failed", doc["code"], doc["args"],
                               trade_count=0, exit_status=exit_status)
        if exit_status != 0:
            raise ProbeError("success report requires exit status 0")
        if "code" in doc or "args" in doc:
            raise ProbeError("success report also contains failure fields")
        summary, trades = doc.get("summary"), doc.get("trades")
        if not isinstance(summary, dict) or not isinstance(trades, list):
            raise ProbeError("success report is missing summary or trades")
        count = summary.get("total_trades")
        if type(count) is not int or count < 0 or count != len(trades):
            raise ProbeError("summary.total_trades must equal len(trades)")
        return observation("completed", trade_count=count, exit_status=exit_status)
    except (ProbeError, UnicodeError) as error:
        return observation("driver_error", "driver_protocol_invalid", exit_status=exit_status,
                           detail=str(error))


def exact_json_equal(wanted, actual):
    if type(wanted) is not type(actual):
        return False
    if isinstance(wanted, dict):
        return (wanted.keys() == actual.keys()
                and all(exact_json_equal(value, actual[key]) for key, value in wanted.items()))
    if isinstance(wanted, list):
        return (len(wanted) == len(actual)
                and all(exact_json_equal(a, b) for a, b in zip(wanted, actual)))
    return wanted == actual


def compare(probe, expected, observed, *, repo_root=None):
    reasons = []
    if expected["outcome"] != observed["outcome"]:
        reasons.append(f"outcome: expected {expected['outcome']}, got {observed['outcome']}")
    if expected["code"] != observed["code"]:
        reasons.append(f"code: expected {expected['code']!r}, got {observed['code']!r}")
    for key, value in expected["args"].items():
        if key not in observed["args"]:
            reasons.append(f"args.{key}: missing")
        elif not exact_json_equal(value, observed["args"][key]):
            reasons.append(f"args.{key}: expected {value!r}, got {observed['args'][key]!r}")
    if expected["outcome"] == "completed":
        count = observed["trade_count"]
        if type(count) is not int or count < expected["min_trades"]:
            reasons.append(f"min_trades: expected >= {expected['min_trades']}, got {count}")
    if "phase_final" in expected:
        records = observed.get("phase_records", [])
        final = records[-1]["phase"] if records else None
        if final != expected["phase_final"]:
            reasons.append(f"phase_final: expected {expected['phase_final']!r}, got {final!r}")
    control = expected["native_boundary_control"]
    control_exists = control is not None and (Path(repo_root or ROOT) / control).is_file()
    if expected["outcome"] == "compile_refused" and not control_exists:
        reasons.append("compile_refused requires native_boundary_control naming an existing file")
    passed = not reasons
    runtime_covered = (passed and observed["stage"] == "runtime"
                       and observed["outcome"] in ("completed", "failed"))
    coverage = ("COMPILE COVERAGE" if observed["outcome"] == "compile_refused"
                else "RUNTIME COVERAGE" if runtime_covered else "NONE")
    if reasons and observed["detail"]:
        reasons.append(observed["detail"])
    row = {
        "probe": probe, "outcome": observed["outcome"], "code": observed["code"],
        "args": observed["args"], "trade_count": observed["trade_count"],
        "pass": passed, "reason": "; ".join(reasons) if reasons else "matched pinned expectation",
        "label": expected["label"], "pinned_by": expected["pinned_by"],
        "exit_status": observed["exit_status"], "stage": observed["stage"],
        "coverage": coverage, "runtime_covered": runtime_covered,
        "native_boundary_control": control,
        "native_boundary_control_exists": control_exists,
    }
    if "phase_records" in observed:
        row["phase_records"] = observed["phase_records"]
    return row


def error_result(probe, error):
    return {
        "probe": probe, "outcome": "driver_error", "code": "driver_setup_invalid", "args": {},
        "trade_count": None, "pass": False, "reason": str(error), "label": None,
        "pinned_by": None, "exit_status": None, "stage": "setup", "coverage": "NONE",
        "runtime_covered": False, "native_boundary_control": None,
        "native_boundary_control_exists": False,
    }


def discover_probes(root, only):
    root = Path(root).resolve()
    if not root.is_dir():
        raise ProbeError(f"probes directory does not exist: {root}")
    probes = []
    for pine in sorted(root.rglob("probe.pine")):
        relative = pine.parent.relative_to(root).as_posix()
        probes.append((pine.parent.name if relative == "." else relative, pine.parent))
    if not probes:
        raise ProbeError("no probe.pine files found")
    if not only:
        return probes
    selected = set()
    for name in only:
        matches = [item for item in probes if item[0] == name or item[1].name == name]
        if not matches:
            raise ProbeError(f"unknown probe name: {name}")
        if len(matches) > 1:
            raise ProbeError(f"ambiguous probe name: {name}; use family/name")
        selected.add(matches[0][0])
    return [item for item in probes if item[0] in selected]


def compile_command(prefix, cpp, so):
    # docker/entrypoint.sh:218-224, token for token; do not add or change flags.
    return [
        "g++", "-std=c++17", "-O2", "-ffp-contract=off", "-fPIC", "-shared",
        f"-I{prefix}/include", "-I/usr/include/eigen3", str(cpp),
        "-Wl,--whole-archive", f"{prefix}/lib/libpineforge.a", "-Wl,--no-whole-archive",
        "-o", str(so),
    ]


def runtime_command(run_json, work, options, *, phase_fd=None):
    # docker/entrypoint.sh:287-300, ordinary TRANSPILED=true route.
    command = [
        sys.executable, str(run_json), "--so", str(work / "strategy.so"),
        "--ohlcv", str(work / "bars.csv"),
        "--inputs", options["inputs"], "--overrides", options["overrides"],
        "--input-tf", options["input_tf"], "--script-tf", options["script_tf"],
        "--bar-magnifier", options["bar_magnifier"],
        "--magnifier-samples", str(options["magnifier_samples"]),
        "--magnifier-dist", options["magnifier_dist"],
        "--generated-cpp", str(work / "strategy.cpp"), "--transpiled", "true",
        *options["options"],
    ]
    if selected_request(options) is not None:
        if type(phase_fd) is not int or not 3 <= phase_fd <= 2 ** 31 - 1:
            raise ProbeError("selected runtime needs a run phase descriptor from 3 up")
        command += ["--request-feed-inventory", str(work / "request_feed_inventory.json"),
                    "--run-phase-fd", str(phase_fd)]
    return command


def bind_inventory_command(run_json, work, codegen):
    # docker/entrypoint.sh:238-242. The helper beside this run_json delegates
    # all inventory validation/hashing to the explicitly selected codegen.
    helper = Path(run_json).with_name("bind_compiled_inventory.py")
    if not helper.is_file():
        raise ProbeError(f"selected harness is missing {helper}")
    return [
        sys.executable, "-I", str(Path(__file__).resolve()), "--_bind-inventory",
        str(codegen), str(helper),
        "--inventory", str(work / "request_feed_inventory.source.json"),
        "--cpp", str(work / "strategy.cpp"), "--so", str(work / "strategy.so"),
        "--out", str(work / "request_feed_inventory.json"),
    ]


def resolve_toolchain(prefix, codegen, session):
    prefix, codegen = Path(prefix).resolve(), Path(codegen).resolve()
    if not (codegen / "pineforge_codegen" / "__init__.py").is_file():
        raise ProbeError("--codegen must be the pineforge_codegen package parent")
    if not (prefix / "lib" / "libpineforge.a").is_file():
        raise ProbeError("--prefix must contain lib/libpineforge.a")
    cache = prefix / "CMakeCache.txt"
    source = None
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                source = Path(line.partition("=")[2]).resolve()
                break
    run_json = prefix / "bin" / "run_json.py"
    if not run_json.is_file():
        if source is None:
            raise ProbeError("installed prefix is missing bin/run_json.py")
        run_json = source / "docker" / "run_json.py"
        if not run_json.is_file():
            raise ProbeError("build tree source checkout is missing docker/run_json.py")
    compile_prefix = prefix
    if not (prefix / "include" / "pineforge" / "pineforge.h").is_file():
        if source is None or not (source / "include" / "pineforge" / "pineforge.h").is_file():
            raise ProbeError("prefix has no installed headers or valid CMake source headers")
        # A build tree has lib/, but CMake does not copy include/. A scratch view
        # preserves the exact -IPREFIX/include and whole-archive argv structure.
        compile_prefix = session / "build-prefix"
        compile_prefix.mkdir()
        (compile_prefix / "include").symlink_to(source / "include", target_is_directory=True)
        (compile_prefix / "lib").symlink_to(prefix / "lib", target_is_directory=True)
    return compile_prefix, run_json, codegen


def _write_json(path, doc):
    Path(path).write_text(json.dumps(doc, allow_nan=False, ensure_ascii=True, indent=2) + "\n",
                          encoding="utf-8")


def transpile_worker(codegen, pine, cpp, diagnostic, inventory_out=None):
    """Isolated child: use the entrypoint's API for the explicitly chosen route."""
    sys.path.insert(0, str(Path(codegen).resolve()))
    try:
        if inventory_out is None:
            from pineforge_codegen import transpile
        else:
            from pineforge_codegen import transpile_with_request_inventory
        from pineforge_codegen.errors import CompileError
    except Exception as error:
        _write_json(diagnostic, observation("driver_error", "driver_toolchain_invalid",
                                            stage="transpile", detail=str(error)))
        return 5
    try:
        if inventory_out is None:
            # docker/entrypoint.sh:109-114. No support bypass, timeframe binding,
            # inventory API, or invented provenance file on this ordinary route.
            generated = transpile(Path(pine).read_text(encoding="utf-8"), filename="strategy.pine")
            Path(cpp).write_text(generated, encoding="utf-8")
        else:
            # docker/entrypoint.sh:139-146, including no primary_chart_timeframe
            # binding. Serialize only the inventory returned by pinned codegen.
            result = transpile_with_request_inventory(
                Path(pine).read_text(encoding="utf-8"), filename="strategy.pine")
            Path(cpp).write_bytes(result["cpp"].encode("utf-8"))
            with Path(inventory_out).open("w", encoding="utf-8") as handle:
                json.dump(result["request_feed_inventory"], handle, ensure_ascii=True,
                          allow_nan=False, separators=(",", ":"))
    except CompileError as error:
        errors = [item for item in error.diagnostics
                  if getattr(item.level, "value", item.level) == "error"]
        first = errors[0] if errors else None
        doc = observation("compile_refused", first.code if first else "transpile_refused",
                          first.args if first else {}, exit_status=5,
                          stage="transpile", detail=str(error))
        _write_json(diagnostic, doc)
        print(f"[pineforge] transpile error: {error}", file=sys.stderr)
        return 5
    except Exception as error:
        # An unexpected codegen fault is not acceptance for a language refusal.
        _write_json(diagnostic, observation("driver_error", "driver_transpile_error",
                                            exit_status=5, stage="transpile", detail=str(error)))
        print(f"[pineforge] transpile error: {error}", file=sys.stderr)
        return 5
    return 0


def bind_inventory_worker(codegen, helper, *arguments):
    """Run the real companion binder with --codegen on this isolated child's path."""
    import runpy

    sys.path.insert(0, str(Path(codegen).resolve()))
    sys.argv = [str(helper), *arguments]
    runpy.run_path(str(helper), run_name="__main__")
    return 0


def run_stage(command, stage, work, *, pass_fds=()):
    _write_json(work / f"{stage}.command.json", command)
    try:
        inherited = {"pass_fds": pass_fds} if pass_fds else {}
        result = subprocess.run(command, cwd=work, capture_output=True,
                                timeout=STAGE_TIMEOUT_SECONDS, check=False, **inherited)
    except subprocess.TimeoutExpired as error:
        (work / f"{stage}.stdout").write_bytes(error.stdout or b"")
        (work / f"{stage}.stderr").write_bytes(error.stderr or b"")
        raise ProbeError(f"{stage} exceeded {STAGE_TIMEOUT_SECONDS}s; see stage logs") from error
    (work / f"{stage}.stdout").write_bytes(result.stdout)
    (work / f"{stage}.stderr").write_bytes(result.stderr)
    _write_json(work / f"{stage}.exit.json", {"exit_status": result.returncode})
    return result


def _collect_phase_records(parent, records, errors, finished):
    """Drain UTF-8 NDJSON while the child runs; preserve complete ordered records.

    Limits and framing are phase transport v1 (docker/run_phase_transport.py),
    not an interpretation of stdout or a substitute for the native observer.
    """
    pending = b""
    try:
        parent.settimeout(0.1)
        while True:
            try:
                chunk = parent.recv(4096)
            except socket.timeout:
                if finished.is_set():
                    raise ProbeError("phase transport stayed open after the runtime child exited")
                continue
            if not chunk:
                break
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if len(line) + 1 > MAX_PHASE_RECORD_BYTES or len(records) >= MAX_PHASE_RECORDS:
                    raise ProbeError("phase stream exceeds phase transport v1 bounds")
                record = load_json(line.decode("utf-8"))
                required = {"record", "version", "sequence", "scope", "trial_id", "phase", "elapsed_ms"}
                if (not isinstance(record, dict) or not required <= record.keys()
                        or record["record"] != "run_phase" or type(record["version"]) is not int
                        or record["version"] != 1 or record["scope"] != "run"
                        or record["trial_id"] is not None or type(record["sequence"]) is not int
                        or record["sequence"] != len(records) + 1
                        or record["phase"] not in RUN_PHASES):
                    raise ProbeError("invalid run phase record envelope or sequence")
                position = RUN_PHASES.index(record["phase"])
                previous = RUN_PHASES.index(records[-1]["phase"]) if records else -1
                if position != previous + 1 and (previous, position) != (2, 4):
                    raise ProbeError("run phase records are out of order")
                elapsed = record["elapsed_ms"]
                if (type(elapsed) not in (int, float) or elapsed < 0
                        or (not records and elapsed != 0)
                        or (records and elapsed < records[-1]["elapsed_ms"])):
                    raise ProbeError("invalid run phase elapsed_ms")
                records.append(record)
            if len(pending) >= MAX_PHASE_RECORD_BYTES:
                raise ProbeError("phase record exceeds phase transport v1 byte bound")
        if pending:
            raise ProbeError("phase stream ended with an incomplete record")
    except (OSError, ProbeError, UnicodeError) as error:
        errors.append(str(error))


def run_selected_runtime(run_json, work, options):
    records, errors = [], []
    finished = threading.Event()
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    actual = None
    with parent, child:
        reader = threading.Thread(target=_collect_phase_records,
                                  args=(parent, records, errors, finished))
        reader.start()
        try:
            command = runtime_command(run_json, work, options, phase_fd=child.fileno())
            ran = run_stage(command, "runtime", work, pass_fds=(child.fileno(),))
            actual = observe_runtime(ran.stdout, ran.returncode)
        except (OSError, ProbeError) as error:
            actual = observation("driver_error", "driver_execution_invalid", stage="runtime",
                                 detail=str(error))
        finally:
            child.close()
            finished.set()
            reader.join()
    if errors:
        actual = observation("driver_error", "driver_phase_protocol_invalid", stage="runtime",
                             exit_status=actual["exit_status"], detail="; ".join(errors))
    actual["phase_records"] = records
    _write_json(work / "runtime.phase_records.json", records)
    return actual


def execute_probe(work, options, compile_prefix, run_json, codegen):
    selected = selected_request(options) is not None
    diagnostic = work / "transpile.failure.json"
    transpile_argv = [
        sys.executable, "-I", str(Path(__file__).resolve()),
        "--_transpile-selected" if selected else "--_transpile",
        str(codegen), str(work / "strategy.pine"), str(work / "strategy.cpp"), str(diagnostic),
    ]
    if selected:
        transpile_argv.append(str(work / "request_feed_inventory.source.json"))
    transpile = run_stage(transpile_argv, "transpile", work)
    if transpile.returncode != 0:
        if transpile.returncode < 0:
            return observation("driver_error", "driver_transpile_error", stage="transpile",
                               exit_status=transpile.returncode, detail="transpiler ended by signal")
        if diagnostic.is_file():
            actual = load_json(diagnostic.read_bytes())
            actual["exit_status"] = transpile.returncode
            return actual
        return observation("driver_error", "driver_transpile_error", stage="transpile",
                           exit_status=transpile.returncode, detail="no structured transpile diagnostic")
    compiled = run_stage(compile_command(compile_prefix, work / "strategy.cpp", work / "strategy.so"),
                         "compile", work)
    if compiled.returncode != 0:
        if compiled.returncode < 0:
            return observation("driver_error", "driver_compiler_error", stage="compile",
                               exit_status=compiled.returncode, detail="compiler ended by signal")
        return observation("compile_refused", "cpp_compile_refused", {"stage": "compile"},
                           stage="compile", exit_status=compiled.returncode,
                           detail="g++ refused the generated translation unit; see compile.stderr")
    if selected:
        bound = run_stage(bind_inventory_command(run_json, work, codegen), "bind", work)
        if bound.returncode != 0:
            return observation("driver_error", "driver_inventory_binding_failed", stage="bind",
                               exit_status=bound.returncode,
                               detail="request-feed inventory binding failed; see bind.stderr")
        return run_selected_runtime(run_json, work, options)
    ran = run_stage(runtime_command(run_json, work, options), "runtime", work)
    return observe_runtime(ran.stdout, ran.returncode)


def run_suite(args):
    only = [name for group in args.only for name in group]
    probes = discover_probes(args.probes, only)
    work_root = args.work.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    session = Path(tempfile.mkdtemp(prefix="ownership-", dir=work_root))
    rows, prepared = {}, []
    # Snapshot and validate EVERY selected expectation before any toolchain runs.
    # Nothing below reads expect.json again or manufactures expectations from output.
    for index, (name, probe) in enumerate(probes):
        work = session / f"{index:04d}"
        work.mkdir()
        try:
            pinned = (probe / "expect.json").read_bytes()
            expected = validate_expectation(load_json(pinned))
            options = load_run_options(probe / "run.json")
            (work / "expect.json").write_bytes(pinned)
            (work / "strategy.pine").write_bytes((probe / "probe.pine").read_bytes())
            (work / "bars.csv").write_bytes((probe / "bars.csv").read_bytes())
            (work / "NOTES.md").write_bytes((probe / "NOTES.md").read_bytes())
            _write_json(work / "run.json", options)
            prepared.append((name, work, expected, options, hashlib.sha256(pinned).hexdigest()))
        except (OSError, ProbeError) as error:
            rows[name] = error_result(name, error)
            rows[name]["work"] = str(work)
    if prepared:
        try:
            compile_prefix, run_json, codegen = resolve_toolchain(args.prefix, args.codegen, session)
            toolchain_error = None
        except (OSError, ProbeError, UnicodeError) as error:
            toolchain_error = error
        for name, work, expected, options, digest in prepared:
            try:
                if toolchain_error is not None:
                    raise ProbeError(str(toolchain_error))
                actual = execute_probe(work, options, compile_prefix, run_json, codegen)
            except (OSError, ProbeError, UnicodeError) as error:
                actual = observation("driver_error", "driver_execution_invalid",
                                     stage="driver", detail=str(error))
            if selected_request(options) is not None:
                actual.setdefault("phase_records", [])
            row = compare(name, expected, actual)
            row.update(expect_sha256=digest, work=str(work))
            rows[name] = row
    return [rows[name] for name, _ in probes]


def write_report(path, rows):
    doc = {
        "schema": "pineforge-ownership-probes/v1", "results": rows,
        "summary": {
            "total": len(rows), "passed": sum(row["pass"] for row in rows),
            "failed": sum(not row["pass"] for row in rows),
            "runtime_covered": sum(row["runtime_covered"] for row in rows),
            "compile_coverage": sum(row["coverage"] == "COMPILE COVERAGE" for row in rows),
        },
    }
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    _write_json(path, doc)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--probes", required=True, type=Path)
    parser.add_argument("--only", action="append", nargs="+", default=[], metavar="PROBE")
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--codegen", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        rows = run_suite(args)
    except (OSError, ProbeError) as error:
        rows = [error_result("<driver>", error)]
    if not rows:
        rows = [error_result("<driver>", "no result rows; refusing an empty PASS")]
    try:
        write_report(args.report, rows)
    except OSError as error:
        print(f"cannot write report: {error}", file=sys.stderr)
        return 1
    for row in rows:
        print(f"{'PASS' if row['pass'] else 'FAIL'} {row['probe']}: {row['reason']}", file=sys.stderr)
    return int(any(not row["pass"] for row in rows))


if __name__ == "__main__":
    if sys.argv[1:2] == ["--_transpile"]:
        raise SystemExit(transpile_worker(*sys.argv[2:]))
    if sys.argv[1:2] == ["--_transpile-selected"]:
        raise SystemExit(transpile_worker(*sys.argv[2:]))
    if sys.argv[1:2] == ["--_bind-inventory"]:
        raise SystemExit(bind_inventory_worker(*sys.argv[2:]))
    raise SystemExit(main())
