# RED reason: scripts/run_parity_jailed.py intentionally does not exist in
# Phase A; every row must fail on that missing adapter import.
"""RED proof for the jailed parity adapter.

The RED reason is deliberate: ``scripts/run_parity_jailed.py`` does not exist
in Phase A.  Keep the import failure visible to every row; do not turn a
missing adapter into a skip.  Phase B supplies the one ``run_cases`` entry
function and makes these observations pass.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import types
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "scripts"))

import run_json
import run_strategy
import test_jailed_temp_files_witness as witness
import test_run_json_selected_window as selected
import test_run_json_sink as sink_fixture
import test_run_strategy_sink as strategy_fixture
from test_run_json_sink_fd import FdLibrary, Stream, sealed_arena_fd


RUN_JSON = ROOT / "docker" / "run_json.py"
FINALIZER = ROOT / "docker" / "pineforge_sink_finalize.py"
ARENA_SEALS = 0x0004 | 0x0002 | 0x0001
REPORT_FD_MIN = 7

try:
    import run_parity_jailed as _adapter
except ModuleNotFoundError as error:
    if error.name != "run_parity_jailed":
        raise
    _adapter = None
    _adapter_import_error = error
else:
    _adapter_import_error = None


def _require_entry():
    if _adapter is None:
        pytest.fail(
            "RED: import scripts/run_parity_jailed.py failed: "
            f"{_adapter_import_error}")
    entry = getattr(_adapter, "run_cases", None)
    if entry is None:
        pytest.fail(
            "RED: scripts/run_parity_jailed.py has no run_cases entry function")
    return entry


def _write_memfd(size: int, *, seal: bool) -> int:
    flags = 1 | 2  # MFD_CLOEXEC | MFD_ALLOW_SEALING
    if hasattr(os, "memfd_create"):
        fd = os.memfd_create("pineforge-parity-arena", flags)
    else:
        libc = ctypes.CDLL(None, use_errno=True)
        fd = libc.memfd_create(b"pineforge-parity-arena", flags)
        if fd < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), "memfd_create")
    try:
        os.ftruncate(fd, size)
        if seal:
            fcntl.fcntl(fd, 1033, ARENA_SEALS)  # F_ADD_SEALS
        return fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 16)
    finally:
        os.close(fd)


class _MetadataJailedLibrary(witness._JailedLibrary):
    """The witness's native boundary with the L9 trade metadata fixture."""

    def __init__(self, event_fd, *, fail=False, accept_bad_arena=False):
        super().__init__(event_fd, fail=fail)
        self.pnls = tuple(
            trade["pnl"] for trade in strategy_fixture.trade_fixture())
        self._accept_bad_arena = accept_bad_arena
        self._arena_invalid = False
        self.strategy_get_last_error_args = self.export(
            "arena_args",
            lambda state: (b'{"reason":"arena_backing"}'
                           if self._arena_invalid else b"{}"))

    def admit(self, state):
        if self._accept_bad_arena:
            # Gate rows deliberately leave arena admission to the adapter.
            # The real child must finish successfully so a child error cannot
            # satisfy the adapter's refusal assertion.
            return 0
        try:
            info = os.fstat(5)
            target = os.readlink("/proc/self/fd/5")
            seals = fcntl.fcntl(5, 1034)  # F_GET_SEALS
            expected = int(os.environ["PF_SCRATCH_ARENA_BYTES"])
            valid = (
                stat.S_ISREG(info.st_mode)
                and target.startswith("/memfd:")
                and seals & ARENA_SEALS == ARENA_SEALS
                and info.st_size == expected
                and expected > 0
            )
        except (OSError, KeyError, ValueError):
            valid = False
        if not valid:
            self._arena_invalid = True
            self.error = (
                "the jailed profile requires an attached scratch arena",
                "report_sink_failed",
            )
            return -1
        _emit(self.event_fd, {
            "kind": "arena-admitted",
            "bytes": os.fstat(5).st_size,
            "seals": fcntl.fcntl(5, 1034),
        })
        return 0

    def write_sink(self, path, rows, *, outputs=False, fault=None):
        """Replace only T rows with the independent metadata fixture."""
        rows = list(rows)
        equity = [row for row in rows if row[0] == "E"]
        trades = []
        for index, trade in enumerate(strategy_fixture.trade_fixture(), 1):
            value = dict(
                trade,
                n=index,
                side="long" if trade["is_long"] else "short",
            )
            trades.append([
                "T", *(value[field] for field in sink_fixture.TRADE_FIELDS)
            ])
        return FdLibrary.write_sink(
            self, path, [*equity, *trades], outputs=outputs, fault=fault)


def _emit(fd, document):
    witness._emit(fd, document)


def _bind_fake_library_harness(child_run_json):
    """Use the ctypes classes from the harness module executed by the child."""
    owners = (
        sys.modules.get(__name__),
        sys.modules.get(FdLibrary.__module__),
        sys.modules.get("test_run_json_sink"),
        sys.modules.get("test_run_json_sink_summary"),
        sys.modules.get(witness.__name__),
    )
    for owner in owners:
        if owner is not None and hasattr(owner, "run_json"):
            owner.run_json = child_run_json


def _arena_observation():
    result = {
        "fd_open": False,
        "is_memfd": False,
        "bytes": 0,
        "seals": 0,
    }
    try:
        info = os.fstat(5)
        result.update(
            fd_open=True,
            is_regular=stat.S_ISREG(info.st_mode),
            target=os.readlink("/proc/self/fd/5"),
            bytes=info.st_size,
            seals=fcntl.fcntl(5, 1034),  # F_GET_SEALS
        )
        result["is_memfd"] = result["target"].startswith("/memfd:")
    except OSError:
        pass
    return result


def _report_observation(fd, *, force_not_writable=False):
    result = {
        "fd": fd,
        "open": False,
        "writable": False,
        "fifo_or_socket": False,
    }
    if fd is None:
        return result
    try:
        info = os.fstat(fd)
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        result.update(
            open=True,
            writable=(flags & os.O_ACCMODE) in (os.O_WRONLY, os.O_RDWR),
            fifo_or_socket=stat.S_ISFIFO(info.st_mode) or stat.S_ISSOCK(info.st_mode),
            mode=stat.S_IFMT(info.st_mode),
        )
    except OSError:
        pass
    if force_not_writable:
        # The child still writes to the real pipe; only the launcher receipt
        # carries the deliberately bad observation for the adapter gate.
        result["writable"] = False
    return result


def _launcher_child(argv):
    separator = argv.index("--")
    parser = argparse.ArgumentParser()
    parser.add_argument("--events-fd", type=int, required=True)
    parser.add_argument("--arena-source-fd", type=int)
    parser.add_argument("--report-source-fd", type=int)
    parser.add_argument("--report-fd", type=int)
    parser.add_argument("--mode", choices=("normal", "typed"), default="normal")
    parser.add_argument("--accept-bad-arena", action="store_true")
    parser.add_argument("--allow-report-fd-five", action="store_true")
    parser.add_argument("--report-not-writable-stream", action="store_true")
    parser.add_argument("--ordinary-file-arena", action="store_true")
    own = parser.parse_args(argv[:separator])
    harness = argv[separator + 1:]

    arena_observation = None
    if own.arena_source_fd is None:
        try:
            os.close(5)
        except OSError:
            pass
    else:
        os.dup2(own.arena_source_fd, 5)
        # A report-fd-five child replaces slot 5 with its sink below. Keep
        # the actual arena observation from before that deliberate collision
        # so the adapter reaches its report-fd gate.
        arena_observation = _arena_observation()
        if own.ordinary_file_arena:
            # Keep the ordinary-file row's other arena facts valid so its
            # mutation isolates the memfd identity check.
            arena_observation.update(
                fd_open=True,
                is_regular=True,
                target=os.readlink("/proc/self/fd/5"),
                bytes=os.fstat(5).st_size,
                seals=ARENA_SEALS,
                is_memfd=False,
            )
    if own.report_source_fd is not None and own.report_fd is not None:
        os.dup2(own.report_source_fd, own.report_fd)

    tempfile.tempdir = os.environ["TMPDIR"]
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

    script = str(Path(harness[1]).resolve()) if len(harness) > 1 else ""
    script_path = Path(script)
    child_run_json = run_json
    if script_path != RUN_JSON:
        sys.path.insert(0, str(script_path.parent))
        spec = importlib.util.spec_from_file_location(
            f"_parity_run_json_{os.getpid()}", script_path)
        if spec is None or spec.loader is None:
            raise ImportError(f"cannot load jailed harness: {script_path}")
        child_run_json = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(child_run_json)
    _bind_fake_library_harness(child_run_json)
    arguments = harness[2:] if len(harness) > 1 else []
    if own.allow_report_fd_five:
        validate_sink_options = child_run_json._validate_sink_options

        def allow_reserved_report_fd(args):
            if args.report_sink_fd != 5:
                return validate_sink_options(args)
            args.report_sink_fd = 7
            try:
                return validate_sink_options(args)
            finally:
                args.report_sink_fd = 5

        # run_json's normal CLI gate is intentionally bypassed in this
        # stand-in; the adapter is the component under test for fd 5.
        child_run_json._validate_sink_options = allow_reserved_report_fd
    sink_fd = None
    if "--report-sink-fd" in arguments:
        sink_fd = int(arguments[arguments.index("--report-sink-fd") + 1])
    _emit(own.events_fd, {
        "kind": "jail",
        "environment_keys": sorted(os.environ),
        "profile": os.environ.get("PINEFORGE_RUN_JSON_PROFILE"),
        "fsize": list(resource.getrlimit(resource.RLIMIT_FSIZE)),
        "sigxfsz_ignored": signal.getsignal(signal.SIGXFSZ) == signal.SIG_IGN,
        "arena": (arena_observation
                  if arena_observation is not None else _arena_observation()),
        "arena_env_bytes": int(os.environ.get("PF_SCRATCH_ARENA_BYTES", "0")),
        "work_read_only": not bool(os.stat(".").st_mode & 0o222),
        "tmp_read_only": not bool(os.stat(os.environ["TMPDIR"]).st_mode & 0o222),
    })
    _emit(own.events_fd, {
        "kind": "command",
        "script": script,
        "real_run_json": (
            getattr(child_run_json, "__file__", None) is not None
            and Path(child_run_json.__file__).resolve() == script_path
        ),
        "profile": next((
            arguments[index + 1]
            for index, value in enumerate(arguments[:-1])
            if value == "--profile"
        ), None),
        "report": _report_observation(
            sink_fd, force_not_writable=own.report_not_writable_stream),
    })
    witness._install_observers(own.events_fd)

    try:
        if "--report-policy" in arguments:
            # Exercise the same selected-window planner fixture as the witness.
            status = witness._selected_json_main(
                types.SimpleNamespace(events_fd=own.events_fd), arguments)
        else:
            library = _MetadataJailedLibrary(
                own.events_fd,
                fail=own.mode == "typed",
                accept_bad_arena=own.accept_bad_arena,
            )
            with mock.patch.object(child_run_json, "load_strategy",
                                   lambda path: library):
                status = child_run_json.main(arguments)
    except OSError as error:
        _emit(own.events_fd, {
            "kind": "os-error",
            "errno": error.errno,
            "type": type(error).__name__,
            "detail": str(error),
        })
        status = 1
    except child_run_json.RunFailure as error:
        _emit(own.events_fd, {
            "kind": "typed-refusal",
            "code": error.code,
            "args": error.code_args,
            "detail": str(error),
        })
        status = error.exit_status
    except BaseException as error:
        _emit(own.events_fd, {
            "kind": "child-error",
            "type": type(error).__name__,
            "detail": str(error),
        })
        status = 1
    finally:
        sys.setprofile(None)
    _emit(own.events_fd, {"kind": "exit", "status": status})
    return status


def _move_fd(fd, target):
    if fd == target:
        return fd
    os.dup2(fd, target)
    os.close(fd)
    return target


class _StandInLauncher:
    """A real child launcher patterned after the jailed witness."""

    def __init__(self, tmp_path, case_id, *, fault=None):
        self.tmp_path = Path(tmp_path)
        self.case_id = case_id
        self.fault = fault
        self.requests = []
        self.last = None

    def __call__(self, *, argv, env, cwd, arena_fd, report_fd):
        self.requests.append({
            "argv": tuple(argv),
            "env": dict(env),
            "cwd": Path(cwd),
            "arena_fd": arena_fd,
            "report_fd": report_fd,
        })
        launch_dir = self.tmp_path / f"launcher-{len(self.requests)}"
        launch_dir.mkdir()
        work, temporary = launch_dir / "work", launch_dir / "tmp"
        work.mkdir(mode=0o555)
        temporary.mkdir(mode=0o555)
        custom_fds = []

        requested_report_fd = int(report_fd)
        effective_report_fd = requested_report_fd
        if self.fault == "report_fd_below_seven":
            effective_report_fd = REPORT_FD_MIN - 1
        elif self.fault == "report_fd_five":
            effective_report_fd = 5

        with Stream() as events, Stream() as report:
            events.writer = _move_fd(events.writer, 32)
            report_source_fd = fcntl.fcntl(
                report.writer, fcntl.F_DUPFD_CLOEXEC, requested_report_fd)
            os.close(report.writer)
            report.writer = report_source_fd

            actual_arena_fd = arena_fd
            if self.fault == "missing_arena":
                actual_arena_fd = None
            elif self.fault == "ordinary_file_arena":
                path = launch_dir / "ordinary-arena"
                file_fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
                os.ftruncate(file_fd, int(env["PF_SCRATCH_ARENA_BYTES"]))
                actual_arena_fd = file_fd
                custom_fds.append(file_fd)
            elif self.fault == "unsealed_memfd":
                actual_arena_fd = _write_memfd(
                    int(env["PF_SCRATCH_ARENA_BYTES"]), seal=False)
                custom_fds.append(actual_arena_fd)
            elif self.fault == "small_arena":
                actual_arena_fd = sealed_arena_fd(
                    max(1, int(env["PF_SCRATCH_ARENA_BYTES"]) // 2))
                custom_fds.append(actual_arena_fd)

            child_env = dict(env)
            child_env.update(
                PYTHONDONTWRITEBYTECODE="1",
                TMPDIR=str(temporary),
                TMP=str(temporary),
                TEMP=str(temporary),
            )
            harness = list(argv)
            if self.fault == "sink_off" and "--report-sink-fd" in harness:
                index = harness.index("--report-sink-fd")
                del harness[index:index + 2]
            elif "--report-sink-fd" in harness:
                index = harness.index("--report-sink-fd") + 1
                harness[index] = str(effective_report_fd)

            child_command = [
                sys.executable, "-B", str(Path(__file__).resolve()),
                "--launcher-child", "--events-fd", str(events.writer),
                "--report-fd", str(effective_report_fd),
                "--mode", "typed" if self.fault == "typed" else "normal",
            ]
            if self.fault in {
                    "ordinary_file_arena", "unsealed_memfd",
                    "small_arena", "report_fd_five"}:
                child_command.append("--accept-bad-arena")
            if self.fault == "ordinary_file_arena":
                child_command.append("--ordinary-file-arena")
            if self.fault == "report_fd_five":
                child_command.append("--allow-report-fd-five")
            if self.fault == "report_fd_not_writable_stream":
                child_command.append("--report-not-writable-stream")
            if actual_arena_fd is not None:
                child_command += ["--arena-source-fd", str(actual_arena_fd)]
            if report_source_fd is not None:
                child_command += ["--report-source-fd", str(report_source_fd)]
            child_command += ["--", *harness]
            pass_fds = tuple(dict.fromkeys(
                fd for fd in (events.writer, report_source_fd, actual_arena_fd)
                if fd is not None))
            process = subprocess.Popen(
                child_command,
                cwd=work,
                env=child_env,
                pass_fds=pass_fds,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            events.close_writer()
            report.close_writer()
            try:
                stdout, stderr = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise AssertionError("stand-in launcher child did not finish")
            report_bytes = report.data
            event_bytes = events.data

        observed = [json.loads(line) for line in event_bytes.splitlines()]
        if self.fault == "corrupted_report" and report_bytes:
            changed = bytearray(report_bytes)
            changed[len(changed) // 2] ^= 1
            report_bytes = bytes(changed)
        elif self.fault == "wrong_case_report" and report_bytes:
            report_bytes = report_bytes.replace(
                self.case_id.encode(), (self.case_id + "-other").encode(), 1)
        elif self.fault == "finalizer_rejection" and report_bytes:
            report_bytes += b"\n"

        leftovers = {
            str(directory.relative_to(launch_dir)): sorted(
                str(child.relative_to(directory))
                for child in directory.rglob("*")
            )
            for directory in (work, temporary)
        }
        self.last = types.SimpleNamespace(
            status=process.returncode,
            stdout=stdout,
            stderr=stderr,
            report_bytes=report_bytes,
            events=observed,
            leftovers=leftovers,
        )
        for fd in custom_fds:
            try:
                os.close(fd)
            except OSError:
                pass
        work.chmod(0o755)
        temporary.chmod(0o755)
        return self.last


def _case_fixture(tmp_path):
    source, probe, fixture = witness._inputs(tmp_path)
    chart = probe / "ohlcv.csv"
    chart.write_bytes(source.read_bytes())
    (probe / "inputs.json").write_text(
        json.dumps({"ohlcv_csv": chart.name}), encoding="ascii")
    so = probe / "strategy.so"
    so.write_bytes(b"fixture strategy library\n")
    return types.SimpleNamespace(
        case_dir=probe,
        case_id=probe.name,
        chart=chart,
        strategy_so=so,
        native_fixture=fixture,
    )


def _selected_case_fixture(tmp_path):
    case_dir = tmp_path / "selected" / "strategy"
    case_dir.mkdir(parents=True)
    chart = case_dir / "chart.csv"
    rows = selected.csv_rows(10)
    selected.write_ohlcv(chart, rows)
    so, inventory = selected.write_inventory(case_dir, [])
    (case_dir / "generated.cpp").write_text(
        "int fixture = 0;\n", encoding="ascii")
    (case_dir / "inputs.json").write_text(json.dumps({
        "ohlcv_csv": chart.name,
        "selected_window": {
            "input_tf": "1",
            "script_tf": "1",
            "window_start_ms": selected.T,
            "window_end_ms": selected.E,
            "preroll_bars": selected.N,
            "fed_start_ms": selected.F,
            "request_feed_inventory": inventory.name,
        },
    }), encoding="ascii")
    return types.SimpleNamespace(
        case_dir=case_dir,
        case_id=case_dir.name,
        chart=chart,
        strategy_so=so,
        native_fixture=None,
        planner=selected.full_plan(),
    )


def _missing_input_case(tmp_path):
    case_dir = tmp_path / "missing" / "strategy"
    case_dir.mkdir(parents=True)
    (case_dir / "generated.cpp").write_text(
        "int fixture = 0;\n", encoding="ascii")
    so = case_dir / "strategy.so"
    so.write_bytes(b"fixture strategy library\n")
    missing = case_dir / "not-materialized.csv"
    (case_dir / "inputs.json").write_text(
        json.dumps({"ohlcv_csv": missing.name}), encoding="ascii")
    return types.SimpleNamespace(
        case_dir=case_dir,
        case_id=case_dir.name,
        chart=missing,
        strategy_so=so,
        native_fixture=None,
    )


def _output_fixture(tmp_path, case_id, *, stale):
    output = Path(tmp_path) / "output"
    output.mkdir()
    if stale:
        (output / "stale.csv").write_bytes(
            (case_id + ":previous-output\n").encode("ascii"))
    return output


def _csv_snapshot(directory):
    return {
        path.relative_to(directory): path.read_bytes()
        for path in Path(directory).rglob("*.csv")
    }


def _invoke(cases, output, launcher, **options):
    entry = _require_entry()
    return entry(
        [case.case_dir for case in cases],
        output,
        launcher=launcher,
        arena_bytes=witness.ARENA_BYTES,
        **options,
    )


def _reference_trades(case):
    library = strategy_fixture.HarnessLibrary(strategy_fixture.trade_fixture())
    report = strategy_fixture.strategy(library).run(case.chart)
    return list(report["trades"])


def _write_oracle(trades, path):
    run_strategy.write_engine_trades_csv(trades, path)
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        return reader.fieldnames, list(reader)


def _assert_jail_observed(launcher):
    jail = next(event for event in launcher.last.events if event["kind"] == "jail")
    command = next(event for event in launcher.last.events
                   if event["kind"] == "command")
    assert jail["profile"] == "jailed"
    assert jail["fsize"] == [0, 0]
    assert jail["sigxfsz_ignored"] is True
    assert jail["work_read_only"] is True
    assert jail["tmp_read_only"] is True
    assert jail["arena"]["fd_open"] is True
    assert jail["arena"]["is_memfd"] is True
    assert jail["arena"]["seals"] & ARENA_SEALS == ARENA_SEALS
    assert jail["arena"]["bytes"] > 0
    assert jail["arena"]["bytes"] == jail["arena_env_bytes"]
    assert command["real_run_json"] is True
    assert command["profile"] == "jailed"
    assert command["report"]["fd"] >= REPORT_FD_MIN
    assert command["report"]["fd"] != 5
    assert command["report"]["fd"] != 1
    assert command["report"]["open"] is True
    assert command["report"]["writable"] is True
    assert command["report"]["fifo_or_socket"] is True
    assert not any(
        event["kind"] == "filesystem" and event["creation"]
        for event in launcher.last.events
    )
    assert not any(launcher.last.leftovers.values())


def _assert_success(result):
    assert result.ok is True
    assert result.failure_code is None
    assert result.failure_document is None


def _assert_refused(result, output, before):
    assert result.ok is False
    assert _csv_snapshot(output) == before
    assert result.csv_paths == {}


def _assert_child_document(result, launcher):
    document = json.loads(launcher.last.stdout)
    assert result.failure_document == document
    assert result.failure_code == document.get("code")


def _assert_adapter_gate(result, output, before, launcher, expected):
    _assert_refused(result, output, before)
    assert len(launcher.requests) == 1
    assert launcher.last is not None
    assert launcher.last.status == 0, launcher.last.stderr.decode(
        "utf-8", errors="replace")
    child_document = json.loads(launcher.last.stdout)
    assert "error" not in child_document
    assert result.failure_text == expected
    assert result.failure_document is None
    assert result.failure_code is None
    assert result.failure_document != child_document


def _assert_prelaunch_refusal(result, output, before, launcher, expected):
    _assert_refused(result, output, before)
    assert result.failure_text == expected
    assert launcher.requests == []
    assert launcher.last is None
    assert result.preparations == {}
    assert result.trades == {}
    assert result.reports == {}
    assert result.receipts == {}
    assert result.finalizer_outputs == {}


def _finalizer_receipt(report_bytes, case_id):
    environment = dict(os.environ, PINEFORGE_RUN_JSON_PROFILE="jailed")
    result = subprocess.run(
        [
            sys.executable, "-B", str(FINALIZER),
            "--in-fd", "0", "--out", "-", "--max-bytes",
            str(max(len(report_bytes), 1)), "--run-id", case_id,
            "--receipt-fd", "2",
        ],
        input=report_bytes,
        capture_output=True,
        env=environment,
        check=False,
    )
    assert result.returncode != 0
    return json.loads(result.stderr)


def _assert_positive(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    result = _invoke([case], output, launcher)
    _assert_success(result)
    _assert_jail_observed(launcher)
    return case, output, launcher, result


@pytest.fixture(autouse=True)
def _clean_environment(monkeypatch):
    for name in (
        "PINEFORGE_RUN_JSON_PROFILE",
        "PF_SCRATCH_ARENA_FD",
        "PF_SCRATCH_ARENA_BYTES",
        "PF_SCRATCH_WINDOW_BYTES",
        "PF_SCRATCH_ARENA_JAILED",
        "PINEFORGE_REPORT_SINK",
        "PINEFORGE_PARITY_HARNESS_DIR",
        "PINEFORGE_PARITY_CORPUS_ROOT",
        "PINEFORGE_PARITY_DERIVED_WORKSPACE",
        "STRAY_CREDENTIAL_CANARY",
        "PF_STRAY_AMBIENT_CANARY",
        "PF_SEC_TRACE",
    ):
        monkeypatch.delenv(name, raising=False)


def test_launcher_request_environment_is_closed(tmp_path, monkeypatch):
    monkeypatch.setenv("STRAY_CREDENTIAL_CANARY", "credential-canary-present")
    monkeypatch.setenv("PF_STRAY_AMBIENT_CANARY", "engine-canary-present")
    monkeypatch.setenv("PF_SEC_TRACE", "trace-canary-present")
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = _invoke([case], output, launcher)

    _assert_success(result)
    environment = launcher.requests[0]["env"]
    assert set(environment) == set([
        "PINEFORGE_RUN_JSON_PROFILE",
        "PF_SCRATCH_ARENA_FD",
        "PF_SCRATCH_ARENA_BYTES",
        "PF_SCRATCH_WINDOW_BYTES",
        "PF_SCRATCH_ARENA_JAILED",
        "PYTHONDONTWRITEBYTECODE",
        "TMPDIR",
        "TMP",
        "TEMP",
    ])
    assert "STRAY_CREDENTIAL_CANARY" not in environment
    assert "PF_STRAY_AMBIENT_CANARY" not in environment
    assert "PF_SEC_TRACE" not in environment


def test_child_environment_is_closed(tmp_path, monkeypatch):
    monkeypatch.setenv("STRAY_CREDENTIAL_CANARY", "credential-canary-present")
    monkeypatch.setenv("PF_STRAY_AMBIENT_CANARY", "engine-canary-present")
    monkeypatch.setenv("PF_SEC_TRACE", "trace-canary-present")
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = _invoke([case], output, launcher)

    _assert_success(result)
    jail = next(event for event in launcher.last.events
                if event["kind"] == "jail")
    allowed_child_names = [
        "PINEFORGE_RUN_JSON_PROFILE",
        "PF_SCRATCH_ARENA_FD",
        "PF_SCRATCH_ARENA_BYTES",
        "PF_SCRATCH_WINDOW_BYTES",
        "PF_SCRATCH_ARENA_JAILED",
        "PYTHONDONTWRITEBYTECODE",
        "TMPDIR",
        "TMP",
        "TEMP",
    ]
    allowed_python_extra_names = [
        # PEP 538: CPython exports LC_CTYPE while coercing a C/POSIX locale.
        "LC_CTYPE",
    ]
    # This subset assertion still rejects every other ambient name; the three
    # canary assertions below prove the adapter did not pass canaries through.
    assert set(jail["environment_keys"]) <= set(
        allowed_child_names + allowed_python_extra_names)
    assert "STRAY_CREDENTIAL_CANARY" not in jail["environment_keys"]
    assert "PF_STRAY_AMBIENT_CANARY" not in jail["environment_keys"]
    assert "PF_SEC_TRACE" not in jail["environment_keys"]


def test_finalizer_environment_is_closed(tmp_path, monkeypatch):
    _require_entry()
    monkeypatch.setenv("STRAY_CREDENTIAL_CANARY", "credential-canary-present")
    monkeypatch.setenv("PF_STRAY_AMBIENT_CANARY", "engine-canary-present")
    monkeypatch.setenv("PF_SEC_TRACE", "trace-canary-present")
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    recorded_environments = []
    real_run = _adapter.subprocess.run

    def record_run(*args, **kwargs):
        recorded_environments.append(dict(kwargs["env"]))
        return real_run(*args, **kwargs)

    with mock.patch.object(
            _adapter.subprocess, "run", side_effect=record_run):
        result = _invoke([case], output, launcher)

    _assert_success(result)
    assert len(recorded_environments) == 1
    environment = recorded_environments[0]
    assert set(environment) == set([
        "PINEFORGE_RUN_JSON_PROFILE",
        "PYTHONDONTWRITEBYTECODE",
    ])
    assert "STRAY_CREDENTIAL_CANARY" not in environment
    assert "PF_STRAY_AMBIENT_CANARY" not in environment
    assert "PF_SEC_TRACE" not in environment


def test_clean_case_matches_unjailed_route_and_writer(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    reference = _reference_trades(case)
    oracle = tmp_path / "unjailed-oracle.csv"
    fieldnames, rows = _write_oracle(reference, oracle)

    result = _invoke([case], output, launcher)

    _assert_success(result)
    _assert_jail_observed(launcher)
    assert result.trades[case.case_id] == reference
    actual = result.csv_paths[case.case_id]
    assert actual.read_bytes() == oracle.read_bytes()
    assert "Engine entry incarnation" in fieldnames
    assert run_strategy.ENGINE_RANGE_END_COLUMN in fieldnames
    assert any(row["Engine entry incarnation"] for row in rows)
    assert any(row[run_strategy.ENGINE_RANGE_END_COLUMN] for row in rows)


def test_case_without_inputs_uses_explicit_engine_clock_and_detected_timeframe(tmp_path, monkeypatch):
    case_dir = tmp_path / "without-inputs"
    case_dir.mkdir()
    chart = strategy_fixture.bars.csv_file(
        tmp_path, name="default-chart.csv", count=8, step=180000)
    (case_dir / "generated.cpp").write_text("int fixture = 0;\n", encoding="ascii")
    (case_dir / "strategy.so").write_bytes(b"fixture strategy library\n")
    case = types.SimpleNamespace(case_dir=case_dir, case_id=case_dir.name, chart=chart)
    assert not (case_dir / "inputs.json").exists()
    # Replace only the source-owned default feed, not the normalization or
    # the independent Strategy.run path. Three-minute gaps deliberately
    # distinguish detection from a fixed one-minute/fifteen-minute default.
    monkeypatch.setattr(run_strategy, "DEFAULT_OHLCV", chart)
    reference = _reference_trades(case)
    oracle = tmp_path / "unjailed-without-inputs.csv"
    _write_oracle(reference, oracle)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = _invoke([case], output, launcher)

    _assert_success(result)
    _assert_jail_observed(launcher)
    request = launcher.requests[0]["argv"]
    assert request[request.index("--ohlcv") + 1] == str(chart)
    assert request[request.index("--chart-tz") + 1] == "UTC"
    assert request[request.index("--input-tf") + 1] == "3"
    assert request[request.index("--script-tf") + 1] == "3"
    assert result.trades[case.case_id] == reference
    assert result.csv_paths[case.case_id].read_bytes() == oracle.read_bytes()
    assert not (case_dir / "inputs.json").exists()


def test_selected_planner_positive_control(tmp_path):
    case = _selected_case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    result = _invoke([case], output, launcher)
    _assert_success(result)
    assert case.planner["window_input_bars"] > 0
    command = next(event for event in launcher.last.events
                   if event["kind"] == "command")
    assert command["real_run_json"] is True
    assert any(
        event["kind"] == "entered"
        and event["file"] == "docker/run_json.py"
        and event["function"] == "_selected_run"
        for event in launcher.last.events
    )


def test_positive_control_for_typed_failure(tmp_path):
    _assert_positive(tmp_path)


def test_typed_failure_document_is_refused_verbatim(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id, fault="typed")
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    _assert_child_document(result, launcher)
    # Cloud Build 16:16 and 16:20 showed what the stand-in's typed fault really leaves: the sink of the
    # real child is NOT empty and NOT header-only; it holds the complete stream written before the typed
    # failure document was produced (13 lines: header, equity records, ...). The row's point is that a
    # VALID sink is still refused because the harness reported a typed failure, so the sink must start
    # with the fixture header for this case (same schema, version, run id and record descriptors; the
    # `engine` member is the real engine identity and is only required to be present) and carry records.
    expected_header = sink_fixture.header()
    lines = launcher.last.report_bytes.split(b"\n")
    assert len(lines) > 2, "the typed-failure row must leave a real sink stream behind"
    actual_header = json.loads(lines[0])
    assert actual_header.pop("engine")
    expected_header.pop("engine")
    expected_header["run_id"] = case.case_id
    assert actual_header == expected_header


def test_positive_control_for_missing_arena(tmp_path):
    _assert_positive(tmp_path)


def _assert_missing_arena_case(module, tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id, fault="missing_arena")
    validation = module._validate_arena
    decisions = []

    def observe(jail, expected_bytes):
        admitted = validation(jail, expected_bytes)
        decisions.append((jail, expected_bytes, admitted))
        return admitted

    with mock.patch.object(module, "_validate_arena", side_effect=observe):
        result = module.run_cases(
            [case.case_dir], output, launcher=launcher, arena_bytes=witness.ARENA_BYTES)
    _assert_refused(result, output, before)
    assert launcher.last is not None
    _assert_child_document(result, launcher)
    jail = next(event for event in launcher.last.events if event["kind"] == "jail")
    assert jail["arena"]["fd_open"] is False
    assert decisions == [(jail, witness.ARENA_BYTES, False)], (
        "arena admission branch did not inspect and reject the child's missing fd 5")


def test_missing_arena_is_refused(tmp_path):
    _require_entry()
    _assert_missing_arena_case(_adapter, tmp_path)


def test_positive_control_for_ordinary_file_arena(tmp_path):
    _assert_positive(tmp_path)


def test_ordinary_file_arena_adapter_gate(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="ordinary_file_arena")
    result = _invoke([case], output, launcher)
    _assert_adapter_gate(
        result, output, before, launcher,
        "the jailed profile requires an attached scratch arena",
    )
    assert next(event for event in launcher.last.events
                if event["kind"] == "jail")["arena"]["is_memfd"] is False


def test_positive_control_for_unsealed_memfd(tmp_path):
    _assert_positive(tmp_path)


def test_unsealed_memfd_adapter_gate(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id, fault="unsealed_memfd")
    result = _invoke([case], output, launcher)
    _assert_adapter_gate(
        result, output, before, launcher,
        "the jailed profile requires an attached scratch arena",
    )
    assert next(event for event in launcher.last.events
                if event["kind"] == "jail")["arena"]["seals"] & ARENA_SEALS != ARENA_SEALS


def test_positive_control_for_small_arena(tmp_path):
    _assert_positive(tmp_path)


def test_small_arena_adapter_gate(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id, fault="small_arena")
    result = _invoke([case], output, launcher)
    _assert_adapter_gate(
        result, output, before, launcher,
        "the jailed profile requires an attached scratch arena",
    )
    jail = next(event for event in launcher.last.events if event["kind"] == "jail")
    assert 0 < jail["arena"]["bytes"] < jail["arena_env_bytes"]


def test_positive_control_for_corrupted_report(tmp_path):
    _assert_positive(tmp_path)


def test_corrupted_report_is_refused(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="corrupted_report")
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    assert result.failure_document == _finalizer_receipt(
        launcher.last.report_bytes, case.case_id)


def test_positive_control_for_wrong_case_report(tmp_path):
    _assert_positive(tmp_path)


def test_wrong_case_report_is_refused(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="wrong_case_report")
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    assert result.failure_document == _finalizer_receipt(
        launcher.last.report_bytes, case.case_id)


def test_positive_control_for_zero_cases(tmp_path):
    _assert_positive(tmp_path)


def test_zero_cases_are_refused(tmp_path):
    output = _output_fixture(tmp_path, "zero-cases", stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, "zero-cases")
    result = _invoke([], output, launcher)
    _assert_refused(result, output, before)
    assert result.failure_text == "refusing to run parity: zero cases"
    assert launcher.last is None


def test_positive_control_for_missing_corpus_input(tmp_path):
    _assert_positive(tmp_path)


def test_missing_corpus_input_is_refused(tmp_path):
    case = _missing_input_case(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    assert result.failure_text == f"missing corpus input: {case.chart.resolve()}"
    assert launcher.last is None


def test_positive_control_for_report_fd_five(tmp_path):
    _assert_positive(tmp_path)


def test_report_fd_five_adapter_gate(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="report_fd_five")
    result = _invoke([case], output, launcher)
    _assert_adapter_gate(
        result, output, before, launcher,
        "error: --report-sink-fd 5 collides with PF_SCRATCH_ARENA_FD",
    )
    command = next(event for event in launcher.last.events
                   if event["kind"] == "command")
    assert command["report"]["fd"] == 5


def test_positive_control_for_report_fd_below_seven(tmp_path):
    _assert_positive(tmp_path)


def test_report_fd_below_seven_is_refused(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="report_fd_below_seven")
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    command = next(event for event in launcher.last.events
                   if event["kind"] == "command")
    assert command["report"]["fd"] == REPORT_FD_MIN - 1
    assert ">= 7" in result.failure_text


def test_positive_control_for_report_fd_not_writable_stream(tmp_path):
    _assert_positive(tmp_path)


def test_report_fd_not_writable_stream_adapter_gate(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="report_fd_not_writable_stream")
    result = _invoke([case], output, launcher)
    _assert_adapter_gate(
        result, output, before, launcher,
        "report fd is not a writable stream",
    )
    report = next(event for event in launcher.last.events
                  if event["kind"] == "command")["report"]
    assert report["open"] is True
    assert report["writable"] is False


def test_positive_control_for_sink_off(tmp_path):
    _assert_positive(tmp_path)


def test_sink_off_is_refused(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id, fault="sink_off")
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    command = next(event for event in launcher.last.events
                   if event["kind"] == "command")
    assert command["report"]["fd"] is None
    assert "sink" in result.failure_text.lower()


def test_positive_control_for_finalizer_rejection(tmp_path):
    _assert_positive(tmp_path)


def test_finalizer_rejection_is_refused(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id,
                                fault="finalizer_rejection")
    result = _invoke([case], output, launcher)
    _assert_refused(result, output, before)
    assert result.failure_document == _finalizer_receipt(
        launcher.last.report_bytes, case.case_id)


_ADAPTER_GATE_MUTANTS = (
    pytest.param(
        "unsealed_memfd",
        '        and arena["seals"] & _ARENA_SEALS == _ARENA_SEALS',
        "        and True",
        "the jailed profile requires an attached scratch arena",
        id="ignore-seals",
    ),
    pytest.param(
        "ordinary_file_arena",
        (
            '        and arena.get("is_memfd") is True\n'
            '        and isinstance(arena.get("target"), str)\n'
            '        and arena["target"].startswith("/memfd:")'
        ),
        "        and True",
        "the jailed profile requires an attached scratch arena",
        id="ignore-is-memfd",
    ),
    pytest.param(
        "small_arena",
        '        and arena["bytes"] == expected_bytes > 0',
        '        and arena["bytes"] > 0',
        "the jailed profile requires an attached scratch arena",
        id="ignore-arena-bytes-equality",
    ),
    pytest.param(
        "report_fd_five",
        (
            '    if fd == 5:\n'
            '        message = "error: --report-sink-fd 5 collides with PF_SCRATCH_ARENA_FD"\n'
            '    elif fd == 1:\n'
            '        message = "report fd must differ from stdout (fd 1)"\n'
            '    elif type(fd) is not int or fd < 7:\n'
            '        message = "report fd must be >= 7"'
        ),
        (
            '    if fd == 1:\n'
            '        message = "report fd must differ from stdout (fd 1)"\n'
            '    elif type(fd) is not int or (fd < 7 and fd != 5):\n'
            '        message = "report fd must be >= 7"'
        ),
        "error: --report-sink-fd 5 collides with PF_SCRATCH_ARENA_FD",
        id="drop-fd-five-branch",
    ),
    pytest.param(
        "report_fd_not_writable_stream",
        (
            '    elif not (report.get("open") is True and report.get("writable") is True\n'
            '              and report.get("fifo_or_socket") is True):\n'
            '        message = "report fd is not a writable stream"'
        ),
        (
            '    elif False:\n'
            '        message = "report fd is not a writable stream"'
        ),
        "report fd is not a writable stream",
        id="drop-writable-fifo-check",
    ),
)


def _load_mutated_adapter(tmp_path, old, new):
    _require_entry()
    source_path = ROOT / "scripts" / "run_parity_jailed.py"
    source = source_path.read_text(encoding="utf-8")
    assert source.count(old) == 1, (
        f"mutation target must occur exactly once: {old!r}")
    mutated_path = Path(tmp_path) / "run_parity_jailed_mutated.py"
    mutated_source = source.replace(old, new, 1)
    mutated_path.write_text(mutated_source, encoding="utf-8")
    spec = importlib.util.spec_from_file_location(
        f"run_parity_jailed_mutated_{id(mutated_path)}", mutated_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    exec(compile(mutated_source, str(mutated_path), "exec"), module.__dict__)
    return module


_CLOSED_LAUNCHER_ENVIRONMENT = re.compile(
    r"(?ms)^(?P<prefix>[ \t]*environment[ \t]*=[ \t]*)"
    r"(?P<value>\{"
    r"(?=[^{}]*PINEFORGE_RUN_JSON_PROFILE)"
    r"(?=[^{}]*PF_SCRATCH_ARENA_FD)"
    r"(?=[^{}]*PF_SCRATCH_ARENA_BYTES)"
    r"(?=[^{}]*PF_SCRATCH_WINDOW_BYTES)"
    r"(?=[^{}]*PF_SCRATCH_ARENA_JAILED)"
    r"(?=[^{}]*PYTHONDONTWRITEBYTECODE)"
    r"(?=[^{}]*TMPDIR)"
    r"(?=[^{}]*TMP)"
    r"(?=[^{}]*TEMP)"
    r"[^{}]*\})"
)


def _load_closed_environment_mutation(tmp_path):
    _require_entry()
    source_path = ROOT / "scripts" / "run_parity_jailed.py"
    source = source_path.read_text(encoding="utf-8")
    matches = list(_CLOSED_LAUNCHER_ENVIRONMENT.finditer(source))
    assert matches, "closed launcher environment literal is missing"
    assert len(matches) == 1, (
        "closed launcher environment mutation target must occur exactly once")
    mutated_source = _CLOSED_LAUNCHER_ENVIRONMENT.sub(
        lambda match: (
            match.group("prefix")
            + "dict(os.environ, **"
            + match.group("value")
            + ")"
        ),
        source,
        count=1,
    )
    mutated_path = Path(tmp_path) / "run_parity_jailed_env_mutated.py"
    mutated_path.write_text(mutated_source, encoding="utf-8")
    spec = importlib.util.spec_from_file_location(
        f"run_parity_jailed_env_mutated_{id(mutated_path)}", mutated_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    exec(compile(mutated_source, str(mutated_path), "exec"), module.__dict__)
    return module


def test_closed_launcher_environment_mutation_is_killed(tmp_path, monkeypatch):
    monkeypatch.setenv("STRAY_CREDENTIAL_CANARY", "credential-canary-present")
    monkeypatch.setenv("PF_STRAY_AMBIENT_CANARY", "engine-canary-present")
    monkeypatch.setenv("PF_SEC_TRACE", "trace-canary-present")
    mutated = _load_closed_environment_mutation(tmp_path)
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = mutated.run_cases(
        [case.case_dir],
        output,
        launcher=launcher,
        arena_bytes=witness.ARENA_BYTES,
    )

    _assert_success(result)
    environment = launcher.requests[0]["env"]
    assert environment["STRAY_CREDENTIAL_CANARY"] == (
        "credential-canary-present")
    assert environment["PF_STRAY_AMBIENT_CANARY"] == "engine-canary-present"
    assert environment["PF_SEC_TRACE"] == "trace-canary-present"


def _run_adapter_gate(module, tmp_path, fault):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id, fault=fault)
    result = module.run_cases(
        [case.case_dir],
        output,
        launcher=launcher,
        arena_bytes=witness.ARENA_BYTES,
    )
    return result, output, before, launcher


@pytest.mark.parametrize(
    ("fault", "old", "new", "expected"),
    _ADAPTER_GATE_MUTANTS,
)
def test_adapter_gate_mutations_are_killed(
        tmp_path, fault, old, new, expected):
    mutated = _load_mutated_adapter(tmp_path, old, new)
    result, output, before, launcher = _run_adapter_gate(
        mutated, tmp_path, fault)
    assert result.ok is True, (
        f"mutant did not accept {fault}: {result.failure_text!r}")
    with pytest.raises(AssertionError):
        _assert_adapter_gate(result, output, before, launcher, expected)


def _finalizer_cap(result, case_id):
    arguments = result.finalizer_outputs[case_id].args
    return int(arguments[arguments.index("--max-bytes") + 1])


def test_report_max_bytes_small_cap_preserves_finalizer_receipt(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = _invoke([case], output, launcher, report_max_bytes=1)

    _assert_refused(result, output, before)
    finalized = result.finalizer_outputs[case.case_id]
    assert finalized.returncode == 8
    assert _finalizer_cap(result, case.case_id) == 1
    assert result.failure_document_bytes == finalized.stderr
    assert result.failure_document == json.loads(finalized.stderr)
    assert result.failure_document["reason"] == "sink_exhausted"
    assert result.failure_document["cap_bytes"] == 1


def test_report_max_bytes_zero_uses_received_length(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = _invoke([case], output, launcher, report_max_bytes=0)

    _assert_success(result)
    assert _finalizer_cap(result, case.case_id) == len(launcher.last.report_bytes)


def test_report_max_bytes_large_explicit_cap_passes(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    cap = 10_000_000

    result = _invoke([case], output, launcher, report_max_bytes=cap)

    _assert_success(result)
    assert _finalizer_cap(result, case.case_id) == cap


def test_report_max_bytes_none_uses_launcher_attribute(tmp_path):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    launcher.report_max_bytes = 10_000_000

    result = _invoke([case], output, launcher, report_max_bytes=None)

    _assert_success(result)
    assert _finalizer_cap(result, case.case_id) == launcher.report_max_bytes


@pytest.mark.parametrize("value", [-1, True, 1.5, "large"])
def test_report_max_bytes_invalid_parameter_is_refused(tmp_path, value):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)

    result = _invoke([case], output, launcher, report_max_bytes=value)

    _assert_refused(result, output, before)
    assert result.failure_text == "unsupported case parameter: report_max_bytes"
    assert launcher.last is None


_INSTALLED_SCRIPT_FILES = (
    "run_parity_jailed.py",
    "parity_paths.py",
    "run_strategy.py",
    "pf_release_run.py",
    "verify_corpus.py",
    "derive_corpus_feeds.py",
)
_INSTALLED_HARNESS_FILES = (
    "run_json.py",
    "pineforge_sink_finalize.py",
    "pf_sink.py",
    "pf_bars.py",
    "run_phase_transport.py",
    "run_execution_observer.py",
    "selected_window_plan.py",
    "selected_window_report.py",
    "request_feed_inventory.py",
)
_INSTALLED_PATH_PROBE = r"""
import importlib
import json
from pathlib import Path
import sys

config = json.loads(sys.argv[1])
script = Path(config["script"]).resolve()
sys.path.insert(0, str(script.parent))
adapter = importlib.import_module("run_parity_jailed")

def probe(value):
    paths = adapter._harness_paths(value)
    sys.path.insert(0, str(paths.directory))
    sys.modules.pop("pf_sink", None)
    sink = importlib.import_module("pf_sink")
    return {
        "run_json": str(paths.run_json),
        "finalizer": str(paths.finalizer),
        "pf_sink": str(Path(sink.__file__).resolve()),
    }

if config["mode"] == "precedence":
    observed = {
        "explicit": probe(config["explicit"]),
        "environment": probe(None),
    }
elif config["mode"] == "refusal":
    output = Path(config["output"])
    result = adapter.run_cases(
        [], output, launcher=object(), arena_bytes=1)
    observed = {
        "failure": result.failure_text,
        "csv_paths": result.csv_paths,
        "output_exists": output.exists(),
        "published": sorted(
            str(path.relative_to(output))
            for path in output.rglob("*.csv")
        ) if output.is_dir() else [],
    }
else:
    observed = {"selected": probe(config.get("harness_dir"))}

print(json.dumps(observed, sort_keys=True))
"""


def _copy_harness_closure(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name in _INSTALLED_HARNESS_FILES:
        shutil.copyfile(Path(source) / name, destination / name)


def _installed_layout(tmp_path):
    base = Path(tmp_path) / "installed-layout"
    parity = base / "opt" / "pineforge" / "parity"
    harness = base / "opt" / "pineforge" / "bin"
    cwd = base / "unrelated"
    for directory in (parity, harness, cwd):
        directory.mkdir(parents=True)
    for name in _INSTALLED_SCRIPT_FILES:
        shutil.copyfile(ROOT / "scripts" / name, parity / name)
    (parity / "PARITY-MANIFEST.json").write_text("{}\n", encoding="ascii")
    _copy_harness_closure(ROOT / "docker", harness)
    return parity, harness, cwd


def _parity_child(script, cwd, *, mode, env_harness=None,
                  explicit=None, output=None):
    config = {
        "script": str(Path(script).resolve()),
        "mode": mode,
        "explicit": None if explicit is None else str(Path(explicit).resolve()),
        "harness_dir": (
            None if explicit is None else str(Path(explicit).resolve())
        ),
        "output": None if output is None else str(Path(output).resolve()),
    }
    child_env = {
        key: value for key, value in os.environ.items()
        if key not in {"PYTHONPATH", "PINEFORGE_PARITY_HARNESS_DIR"}
    }
    if env_harness is not None:
        child_env["PINEFORGE_PARITY_HARNESS_DIR"] = str(
            Path(env_harness).resolve())
    result = subprocess.run(
        [
            sys.executable, "-I", "-B", "-c", _INSTALLED_PATH_PROBE,
            json.dumps(config),
        ],
        cwd=cwd,
        env=child_env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(result.stdout)


def _load_installed_adapter(parity):
    saved_path = sys.path[:]
    names = (
        "parity_paths", "run_strategy", "pf_release_run",
        "verify_corpus", "derive_corpus_feeds", "pf_sink",
    )
    sentinel = object()
    saved_modules = {
        name: sys.modules.get(name, sentinel)
        for name in names
    }
    for name in names:
        sys.modules.pop(name, None)
    sys.path.insert(0, str(parity))
    module_name = f"_installed_run_parity_jailed_{id(parity)}"
    spec = importlib.util.spec_from_file_location(
        module_name, Path(parity) / "run_parity_jailed.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module

    def restore():
        sys.path[:] = saved_path
        sys.modules.pop(module_name, None)
        for name, prior in saved_modules.items():
            if prior is sentinel:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = prior

    try:
        spec.loader.exec_module(module)
    except BaseException:
        restore()
        raise
    return module, restore


def test_installed_adapter_uses_explicit_and_environment_harness_paths(tmp_path):
    parity, harness, cwd = _installed_layout(tmp_path)
    environment_harness = Path(tmp_path) / "environment-harness"
    explicit_harness = Path(tmp_path) / "explicit-harness"
    _copy_harness_closure(harness, environment_harness)
    _copy_harness_closure(harness, explicit_harness)

    observed = _parity_child(
        parity / "run_parity_jailed.py",
        cwd,
        mode="precedence",
        env_harness=environment_harness,
        explicit=explicit_harness,
    )

    assert observed["explicit"] == {
        "run_json": str(explicit_harness / "run_json.py"),
        "finalizer": str(explicit_harness / "pineforge_sink_finalize.py"),
        "pf_sink": str(explicit_harness / "pf_sink.py"),
    }
    assert observed["environment"] == {
        "run_json": str(environment_harness / "run_json.py"),
        "finalizer": str(environment_harness / "pineforge_sink_finalize.py"),
        "pf_sink": str(environment_harness / "pf_sink.py"),
    }


def test_installed_adapter_without_harness_refuses_without_publication(tmp_path):
    parity, _, cwd = _installed_layout(tmp_path)
    output = Path(tmp_path) / "unpublished"

    observed = _parity_child(
        parity / "run_parity_jailed.py",
        cwd,
        mode="refusal",
        output=output,
    )

    assert observed["failure"] == (
        "parity harness directory is not configured: "
        "pass harness_dir or set PINEFORGE_PARITY_HARNESS_DIR"
    )
    assert observed["csv_paths"] == {}
    assert observed["output_exists"] is False
    assert observed["published"] == []


def test_source_checkout_adapter_keeps_docker_harness_locations(tmp_path):
    source = ROOT / "scripts" / "run_parity_jailed.py"
    observed = _parity_child(
        source,
        Path(tmp_path),
        mode="paths",
    )

    assert observed["selected"] == {
        "run_json": str(ROOT / "docker" / "run_json.py"),
        "finalizer": str(ROOT / "docker" / "pineforge_sink_finalize.py"),
        "pf_sink": str(ROOT / "docker" / "pf_sink.py"),
    }


def test_installed_adapter_full_case_uses_installed_child_and_finalizer(tmp_path):
    parity, harness, _ = _installed_layout(tmp_path)
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_installed_adapter(parity)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
        )
    finally:
        restore()

    _assert_success(result)
    _assert_jail_observed(launcher)
    command = next(event for event in launcher.last.events
                   if event["kind"] == "command")
    assert command["script"] == str(harness / "run_json.py")
    assert command["real_run_json"] is True
    assert result.finalizer_outputs[case.case_id].args[2] == str(
        harness / "pineforge_sink_finalize.py")


_STAGED_COMMIT = "0123456789abcdef0123456789abcdef01234567"
_STAGED_FILES = (
    "ohlcv_ETH-USDT-USDT_15m.csv",
    "ohlcv_ETH-USDT-USDT_15m_window.csv",
)


def _staged_case_fixture(tmp_path):
    case = _case_fixture(tmp_path)
    corpus = Path(tmp_path) / "corpus"
    derived = corpus / "data" / "derived"
    derived.mkdir(parents=True)
    contents = case.chart.read_bytes()
    for name in _STAGED_FILES:
        (derived / name).write_bytes(contents)
    (derived / ".derive_corpus_feeds.version").write_bytes(
        b"v2:first-positive-volume-open\n")
    window = derived / _STAGED_FILES[1]
    (case.case_dir / "inputs.json").write_text(
        json.dumps({"ohlcv_csv": str(window)}), encoding="ascii")
    case.chart = window
    hashes = {
        name: hashlib.sha256((derived / name).read_bytes()).hexdigest()
        for name in _STAGED_FILES
    }
    return case, corpus, derived, hashes


def _load_staged_adapter(parity, corpus, monkeypatch):
    monkeypatch.setenv("PINEFORGE_PARITY_CORPUS_ROOT", str(corpus))
    monkeypatch.setenv(
        "PINEFORGE_PARITY_DERIVED_WORKSPACE",
        str(Path(corpus) / "data" / "derived"),
    )
    return _load_installed_adapter(parity)


def _unstaged_case_fixture(tmp_path, ensure_derived):
    case = _case_fixture(tmp_path)
    corpus = Path(tmp_path) / "corpus"
    data = corpus / "data"
    derived = data / "derived"
    data.mkdir(parents=True)
    source = strategy_fixture.bars.csv_file(
        data,
        name="ohlcv_ETH-USDT-USDT_1m.csv",
        count=256,
    )
    # Keep the fixture's generated values and timestamps, while making the
    # real derivation input large enough to pass its LFS-pointer guard.
    rows = source.read_text(encoding="ascii").splitlines()
    padded = [rows[0]]
    for row in rows[1:]:
        fields = row.split(",")
        fields[-1] = "0" * 4096 + fields[-1]
        padded.append(",".join(fields))
    source.write_text("\n".join(padded) + "\n", encoding="ascii")
    ensure_derived(corpus_root=corpus, derived_workspace=derived)
    chart = derived / _STAGED_FILES[0]
    (case.case_dir / "inputs.json").write_text(
        json.dumps({"ohlcv_csv": str(chart)}), encoding="ascii")
    case.chart = chart
    return case, corpus, derived


def test_unstaged_writable_corpus_default_read_only_gate(tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    corpus = Path(tmp_path) / "corpus"
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        case, corpus, _derived = _unstaged_case_fixture(
            tmp_path, installed._strategy.ensure_derived)
        assert not (os.statvfs(corpus).f_flag & os.ST_RDONLY)
        output = _output_fixture(tmp_path, case.case_id, stale=True)
        before = _csv_snapshot(output)
        launcher = _StandInLauncher(tmp_path, case.case_id)
        # Omit require_read_only_corpus to pin its default of True and omit
        # staged_derived to exercise the unstaged path.
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=corpus,
        )
    finally:
        restore()
    _assert_prelaunch_refusal(
        result,
        output,
        before,
        launcher,
        f"corpus is not mounted read-only: {corpus.resolve()}",
    )


def test_unstaged_corpus_root_mismatch_gate(tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    corpus = Path(tmp_path) / "corpus"
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        case, corpus, _derived = _unstaged_case_fixture(
            tmp_path, installed._strategy.ensure_derived)
        wrong_root = Path(tmp_path) / "different-corpus"
        assert wrong_root.resolve() != corpus.resolve()
        output = _output_fixture(tmp_path, case.case_id, stale=True)
        before = _csv_snapshot(output)
        launcher = _StandInLauncher(tmp_path, case.case_id)
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=wrong_root,
        )
    finally:
        restore()
    _assert_prelaunch_refusal(
        result,
        output,
        before,
        launcher,
        "corpus_root differs from the resolved parity paths",
    )


def test_unstaged_writable_corpus_opt_out_runs(tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    corpus = Path(tmp_path) / "corpus"
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        case, corpus, _derived = _unstaged_case_fixture(
            tmp_path, installed._strategy.ensure_derived)
        output = _output_fixture(tmp_path, case.case_id, stale=False)
        launcher = _StandInLauncher(tmp_path, case.case_id)
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=corpus,
            require_read_only_corpus=False,
        )
    finally:
        restore()
    _assert_success(result)
    _assert_jail_observed(launcher)


@pytest.mark.parametrize(
    "staged",
    (False, True),
    ids=("unstaged-read-only-positive", "staged-read-only-positive"),
)
def test_corpus_read_only_positive_control(tmp_path, monkeypatch, staged):
    parity, harness, _ = _installed_layout(tmp_path)
    corpus = Path(tmp_path) / "corpus"
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        if staged:
            case, corpus, _derived, hashes = _staged_case_fixture(tmp_path)
        else:
            case, corpus, _derived = _unstaged_case_fixture(
                tmp_path, installed._strategy.ensure_derived)
            hashes = None
        output = _output_fixture(tmp_path, case.case_id, stale=False)
        launcher = _StandInLauncher(tmp_path, case.case_id)
        parameters = {
            "corpus_root": corpus,
            "require_read_only_corpus": True,
        }
        if staged:
            parameters.update(
                corpus_commit=_STAGED_COMMIT,
                staged_derived=hashes,
            )
        real_statvfs = installed.os.statvfs

        def read_only_for_corpus(path):
            flags = real_statvfs(path)
            if Path(path).resolve() == corpus.resolve():
                return types.SimpleNamespace(f_flag=flags.f_flag | os.ST_RDONLY)
            return flags

        with mock.patch.object(
                installed.os, "statvfs", side_effect=read_only_for_corpus):
            result = installed.run_cases(
                [case.case_dir],
                output,
                launcher=launcher,
                arena_bytes=witness.ARENA_BYTES,
                harness_dir=harness,
                **parameters,
            )
    finally:
        restore()
    _assert_success(result)
    _assert_jail_observed(launcher)


def _staged_refusal(
        tmp_path, monkeypatch, *, staged_derived, expected,
        require_read_only_corpus=False, corpus_commit=_STAGED_COMMIT):
    parity, harness, _ = _installed_layout(tmp_path)
    case, corpus, _derived, _hashes = _staged_case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=corpus,
            corpus_commit=corpus_commit,
            staged_derived=staged_derived,
            require_read_only_corpus=require_read_only_corpus,
        )
    finally:
        restore()
    _assert_refused(result, output, before)
    assert result.failure_text == expected
    assert launcher.last is None
    return result


def test_staged_derived_matching_hashes_runs_without_ensure_derived(
        tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    case, corpus, _derived, hashes = _staged_case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        with mock.patch.object(
                installed._strategy, "ensure_derived",
                side_effect=AssertionError("staged corpus called ensure_derived")) as ensure:
            result = installed.run_cases(
                [case.case_dir],
                output,
                launcher=launcher,
                arena_bytes=witness.ARENA_BYTES,
                harness_dir=harness,
                corpus_root=corpus,
                corpus_commit=_STAGED_COMMIT,
                staged_derived=hashes,
                require_read_only_corpus=False,
            )
            assert ensure.call_count == 0
    finally:
        restore()

    _assert_success(result)
    _assert_jail_observed(launcher)
    assert result.corpus_commit == _STAGED_COMMIT
    assert result.preparations[case.case_id]["manifest"]["corpus_commit"] == (
        _STAGED_COMMIT)
    assert not (corpus / "data" / "ohlcv_ETH-USDT-USDT_1m.csv").exists()


def test_staged_derived_wrong_hash_refuses_without_publication(
        tmp_path, monkeypatch):
    _staged_refusal(
        tmp_path, monkeypatch,
        staged_derived={name: "0" * 64 for name in _STAGED_FILES},
        expected="staged derived feed differs: " + _STAGED_FILES[0],
    )


def test_staged_derived_missing_file_refuses_without_publication(
        tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    case, corpus, derived, hashes = _staged_case_fixture(tmp_path)
    (derived / _STAGED_FILES[0]).unlink()
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=corpus,
            corpus_commit=_STAGED_COMMIT,
            staged_derived=hashes,
            require_read_only_corpus=False,
        )
    finally:
        restore()
    _assert_refused(result, output, before)
    assert result.failure_text == (
        f"missing corpus input: {derived / _STAGED_FILES[0]}")
    assert launcher.last is None


def test_staged_derived_symlink_refuses_as_missing_input_without_publication(
        tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    case, corpus, derived, hashes = _staged_case_fixture(tmp_path)
    target = case.chart
    (derived / _STAGED_FILES[0]).unlink()
    (derived / _STAGED_FILES[0]).symlink_to(target)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=corpus,
            corpus_commit=_STAGED_COMMIT,
            staged_derived=hashes,
            require_read_only_corpus=False,
        )
    finally:
        restore()
    _assert_refused(result, output, before)
    assert result.failure_text == (
        f"missing corpus input: {derived / _STAGED_FILES[0]}")
    assert launcher.last is None


def test_staged_derived_wrong_stamp_refuses_without_publication(
        tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    case, corpus, derived, hashes = _staged_case_fixture(tmp_path)
    (derived / ".derive_corpus_feeds.version").write_bytes(b"old-rule\n")
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=corpus,
            corpus_commit=_STAGED_COMMIT,
            staged_derived=hashes,
            require_read_only_corpus=False,
        )
    finally:
        restore()
    _assert_refused(result, output, before)
    assert result.failure_text == "staged derived stamp differs"
    assert launcher.last is None


def test_staged_derived_bad_key_set_refuses_without_publication(
        tmp_path, monkeypatch):
    _staged_refusal(
        tmp_path, monkeypatch,
        staged_derived={"unexpected.csv": "0" * 64},
        expected="unsupported case parameter: staged_derived",
    )


def test_staged_derived_writable_corpus_refuses_when_read_only_is_required(
        tmp_path, monkeypatch):
    corpus = Path(tmp_path) / "corpus"
    _staged_refusal(
        tmp_path, monkeypatch,
        staged_derived={
            name: "0" * 64
            for name in _STAGED_FILES
        },
        expected=f"corpus is not mounted read-only: {corpus.resolve()}",
        require_read_only_corpus=True,
    )
    assert os.access(corpus, os.W_OK)


def test_staged_derived_invalid_commit_refuses_without_publication(
        tmp_path, monkeypatch):
    _staged_refusal(
        tmp_path, monkeypatch,
        staged_derived={
            name: "0" * 64
            for name in _STAGED_FILES
        },
        expected="unsupported case parameter: corpus_commit",
        corpus_commit="not-a-lowercase-40-hex-commit",
    )


def test_staged_derived_rejects_corpus_root_that_differs_from_import_paths(
        tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    case, corpus, _derived, hashes = _staged_case_fixture(tmp_path)
    wrong_root = Path(tmp_path) / "wrong-corpus"
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_staged_adapter(parity, corpus, monkeypatch)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_root=wrong_root,
            corpus_commit=_STAGED_COMMIT,
            staged_derived=hashes,
            require_read_only_corpus=False,
        )
    finally:
        restore()
    _assert_refused(result, output, before)
    assert result.failure_text == "corpus_root differs from the resolved parity paths"
    assert launcher.last is None


def test_staged_derived_requires_corpus_root(tmp_path, monkeypatch):
    parity, harness, _ = _installed_layout(tmp_path)
    case, _corpus, _derived, hashes = _staged_case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=True)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    installed, restore = _load_installed_adapter(parity)
    try:
        result = installed.run_cases(
            [case.case_dir],
            output,
            launcher=launcher,
            arena_bytes=witness.ARENA_BYTES,
            harness_dir=harness,
            corpus_commit=_STAGED_COMMIT,
            staged_derived=hashes,
            require_read_only_corpus=False,
        )
    finally:
        restore()
    _assert_refused(result, output, before)
    assert result.failure_text == "unsupported case parameter: corpus_root"
    assert launcher.last is None


def test_memfd_without_os_wrapper_runs_full_jailed_case(tmp_path, monkeypatch):
    for name in ("memfd_create", "MFD_CLOEXEC", "MFD_ALLOW_SEALING"):
        monkeypatch.delattr(os, name, raising=False)

    case, _output, launcher, result = _assert_positive(tmp_path)

    assert len(launcher.requests) == 1
    assert result.csv_paths[case.case_id].read_bytes()
    assert result.trades[case.case_id] == _reference_trades(case)


def test_memfd_without_os_wrapper_refuses_unknown_machine(tmp_path, monkeypatch):
    case = _case_fixture(tmp_path)
    output = _output_fixture(tmp_path, case.case_id, stale=False)
    before = _csv_snapshot(output)
    launcher = _StandInLauncher(tmp_path, case.case_id)
    machine = mock.Mock(return_value="pineforge-unsupported-memfd-machine")
    monkeypatch.setattr(platform, "machine", machine)
    for name in ("memfd_create", "MFD_CLOEXEC", "MFD_ALLOW_SEALING"):
        monkeypatch.delattr(os, name, raising=False)

    result = _invoke([case], output, launcher)

    _assert_refused(result, output, before)
    assert result.failure_text == "memfd_create is unavailable on this host"
    assert result.failure_document is None
    assert result.failure_code is None
    machine.assert_called_once_with()
    assert set(result.preparations) == {case.case_id}
    assert launcher.requests == []
    assert launcher.last is None
    assert result.trades == {}
    assert result.reports == {}
    assert result.receipts == {}
    assert result.diagnostics == {}
    assert result.finalizer_outputs == {}
    assert list(output.iterdir()) == []


def test_memfd_fallback_uses_real_syscall_and_seals_arena(monkeypatch):
    _require_entry()
    number = {
        "x86_64": 319,
        "aarch64": 279,
        "riscv64": 279,
        "i686": 356,
        "armv7l": 385,
        "ppc64le": 360,
        "s390x": 350,
    }[platform.machine()]
    real_syscall = ctypes.CDLL(None, use_errno=True).syscall
    real_syscall.restype = ctypes.c_long
    calls = []

    def record_syscall(number, name, flags):
        calls.append((number, name, flags))
        return real_syscall(number, name, flags)

    for name in ("memfd_create", "MFD_CLOEXEC", "MFD_ALLOW_SEALING"):
        monkeypatch.delattr(os, name, raising=False)
    monkeypatch.delattr(fcntl, "F_ADD_SEALS", raising=False)
    libc = types.SimpleNamespace(syscall=record_syscall)
    with mock.patch.object(ctypes, "CDLL", return_value=libc) as load_libc:
        fd = _adapter._arena_fd(witness.ARENA_BYTES)
    try:
        load_libc.assert_called_once_with(None, use_errno=True)
        assert calls == [(number, b"pineforge-parity-arena", 0x3)]
        info = os.fstat(fd)
        assert stat.S_ISREG(info.st_mode)
        assert info.st_size == witness.ARENA_BYTES
        assert os.readlink(f"/proc/self/fd/{fd}").startswith(
            "/memfd:pineforge-parity-arena")
        assert fcntl.fcntl(fd, 1034) & ARENA_SEALS == ARENA_SEALS  # F_GET_SEALS
    finally:
        os.close(fd)


if __name__ == "__main__":
    # The stand-in launcher starts this file as "python file --launcher-child ..."; that mode flag
    # is a dispatch marker, not an option of the child parser.
    sys.exit(_launcher_child([item for item in sys.argv[1:] if item != "--launcher-child"]))
