"""Temporary-file witnesses for the Python wrappers under the jailed profile.

Run the real entry points in fresh children, with the existing native and
planner fixtures. Tempfile, report assembly and finalization remain real.
Creation attempts are audited as well as directory contents, since
TemporaryFile can unlink a file before the parent has a chance to see it.
"""
from __future__ import annotations

import argparse
import ctypes
import fcntl
import io
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import sys
import tempfile
import types
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "scripts"))

import pf_sink
import pineforge_sink_finalize as finalizer
import run_json
import run_strategy
import selected_window_plan
import test_run_json_bin as bars
import test_run_json_selected_window as selected
import test_run_json_sink as sink_fixture
import test_run_strategy_sink as strategy_fixture
from test_run_json_sink_fd import FdLibrary, SelectedFdLibrary, Stream, sealed_arena_fd


ARENA_BYTES = 16 * 1024 * 1024
RUN_JSON = "docker/run_json.py"
RUN_STRATEGY = "scripts/run_strategy.py"
FINALIZER = "docker/pineforge_sink_finalize.py"


def _write_all(fd, data):
    while data:
        written = os.write(fd, data)
        if written <= 0:
            raise OSError("witness event pipe made no progress")
        data = data[written:]


def _emit(fd, document):
    _write_all(fd, json.dumps(document, separators=(",", ":")).encode() + b"\n")


def _site_stack():
    frames = []
    frame = sys._getframe(1)
    while frame is not None:
        path = frame.f_code.co_filename
        if path.startswith(str(ROOT) + os.sep):
            path = path[len(str(ROOT)) + 1:]
            if path in (RUN_JSON, RUN_STRATEGY, FINALIZER):
                frames.append({"file": path, "line": frame.f_lineno,
                               "function": frame.f_code.co_name})
        frame = frame.f_back
    return frames


def _install_observers(event_fd):
    """Observe actual calls; do not replace file-opening or tempfile helpers."""
    functions = {
        RUN_JSON: {"_prepare_report_json", "_prepare_stream_report",
                   "_json_temporary", "_sink_outputs_block",
                   "_legacy_streamed_outputs_block", "_selected_run", "write_failure"},
        RUN_STRATEGY: {"main", "_run_via_docker", "_trim_ohlcv_csv",
                       "_write_sink_engine_trades_csv", "_report_from_sink"},
        FINALIZER: {"main", "_receive", "_verify"},
    }

    def audit(event, args):
        creation = False
        if event in ("tempfile.mkstemp", "tempfile.mkdtemp"):
            path, creation = args[0], True
        elif event == "open":
            path, _mode, flags = args
            if isinstance(path, int) or not isinstance(flags, int):
                return
            writable = flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC)
            anonymous = getattr(os, "O_TMPFILE", 0)
            is_anonymous = bool(anonymous and flags & anonymous == anonymous)
            if not writable and not is_anonymous:
                return
            # Opening an existing device or inherited pipe is not creating
            # a temporary file. Still include the write-open in the audit.
            try:
                special = not stat.S_ISREG(os.stat(path).st_mode)
            except FileNotFoundError:
                special = False
            creation = is_anonymous or not special
        elif event == "os.mkdir":
            path = args[0]
            # pathlib.mkdir(exist_ok=True) also tries existing directories.
            creation = not os.path.isdir(path)
        else:
            return
        _emit(event_fd, {"kind": "filesystem", "event": event,
                         "path": os.fsdecode(path), "creation": creation,
                         "stack": _site_stack()})

    def profile(frame, event, value):
        path = frame.f_code.co_filename
        prefix = str(ROOT) + os.sep
        if not path.startswith(prefix):
            return
        path = path[len(prefix):]
        name = frame.f_code.co_name
        if (path == RUN_STRATEGY and name == "__iter__"
                and type(frame.f_locals.get("self")).__name__ == "_SinkTraceNames"):
            name = "_SinkTraceNames.__iter__"
        elif name not in functions.get(path, ()):
            return
        if event == "call":
            _emit(event_fd, {"kind": "entered", "file": path, "function": name})
        elif event == "return" and name == "_prepare_report_json" and value is not None:
            resident = (isinstance(value, io.StringIO)
                        or isinstance(getattr(value, "buffer", None), io.BytesIO))
            _emit(event_fd, {"kind": "prepared", "resident": resident})

    sys.addaudithook(audit)
    sys.setprofile(profile)


class _JailedLibrary(FdLibrary):
    """Extend the existing fd fixture only at the native-call boundary."""

    def __init__(self, event_fd, *, fail=False):
        super().__init__(outputs=False)
        self.event_fd, self.fail = event_fd, fail
        exports = {
            "strategy_set_scratch_tables_v1": self.scratch,
            "strategy_require_report_scratch_arena_v1": self.admit,
            "strategy_outputs_api_version": lambda: run_json.OUTPUTS_API_VERSION,
            "strategy_outputs_set_enabled": self.enable,
            "strategy_outputs_manifest": self.manifest,
            "strategy_outputs_bars_len": lambda state: 2,
            "strategy_outputs_bar_times_copy": self.times,
            "strategy_outputs_series_copy": self.series,
            "strategy_outputs_events_len": lambda state: 0,
            "strategy_outputs_event_get": lambda *args: -1,
            "strategy_outputs_events_clear": lambda state: None,
        }
        for name, body in exports.items():
            setattr(self, name, self.export(name, body))

    def scratch(self, state, ref):
        manifest = ref._obj
        assert manifest.struct_size == ctypes.sizeof(run_json.ScratchTablesC)
        assert manifest.version == manifest.backend == 1
        _emit(self.event_fd, {"kind": "scratch-configured"})
        return 0

    def admit(self, state):
        assert os.environ["PF_SCRATCH_ARENA_FD"] == "5"
        assert os.fstat(5).st_size == ARENA_BYTES
        assert fcntl.fcntl(5, 1034) & 7 == 7  # F_GET_SEALS
        _emit(self.event_fd, {"kind": "arena-admitted"})
        return 0

    def enable(self, state, enabled):
        self.outputs = bool(enabled)
        _emit(self.event_fd, {"kind": "outputs-enabled", "enabled": bool(enabled)})
        return 0

    def manifest(self, state, destination, capacity, required, error, error_capacity):
        data = sink_fixture.outputs_reader().manifest() + b"\0"
        required._obj.value = len(data)
        if destination is not None and capacity >= len(data):
            ctypes.memmove(destination, data, len(data))
            return 0
        return -1

    def times(self, state, offset, opens, closes, count, copied):
        for index in range(count):
            row = offset + index
            opens[index] = bars.T0 if row == 0 else -(1 << 63)
            closes[index] = bars.T0 + 60000 if row == 0 else -(1 << 63)
        copied._obj.value = count
        return 0

    def series(self, state, slot, offset, values, count, copied):
        for index in range(count):
            values[index] = ((-0. if slot == 0 else 4294967295.)
                             if offset + index == 0 else float("nan"))
        copied._obj.value = count
        return 0

    def run(self, *args):
        super().run(*args)
        _emit(self.event_fd, {"kind": "native-run", "outputs": self.outputs})
        if self.fail:
            self.error = ("injected native refusal", "scratch_capacity_exceeded")


class _RecordedSink:
    """Read the prepared native-result fixture through the real strict reader."""

    def __init__(self, path):
        self.path, self.keep = path, False
        with pf_sink.SinkReader(path) as reader:
            for _record in reader:
                pass
            assert reader.complete
            self.counts = dict(reader.counts)

    def records(self, tag=None):
        with pf_sink.SinkReader(self.path) as reader:
            for record in reader:
                if tag is None or record.tag == tag:
                    yield record
            assert reader.complete

    def close(self):
        # This is an input fixture, prepared before the resource restrictions.
        # Each records() call has already closed its own reader.
        return None


def _strategy_main(own, argv):
    if own.mode == "feed-trim":
        with mock.patch.object(sys, "argv", ["run_strategy.py", *argv]):
            return run_strategy.main()

    sink = _RecordedSink(Path(own.fixture))
    report = run_strategy.ReportC()
    report.total_trades = sink.counts["T"]
    report.net_profit = strategy_fixture.trade_fixture()[0]["pnl"]
    report.input_bars_processed = report.script_bars_processed = 5

    def native_result(*args, **kwargs):
        result = run_strategy._report_from_sink(report, sink, sink.counts["E"])
        _emit(own.events_fd, {
            "kind": "native-result",
            "trades": type(result["trades"]).__name__,
            "trace_names": type(result["trace_names"]).__name__,
        })
        return result

    strategy = types.SimpleNamespace(declares_bar_magnifier=False, run=native_result)
    with mock.patch.object(run_strategy, "Strategy", lambda path: strategy), \
            mock.patch.object(sys, "argv", ["run_strategy.py", *argv]):
        return run_strategy.main()


def _selected_json_main(own, arguments):
    plan = selected.full_plan()
    library = SelectedFdLibrary(plan)
    admission = _JailedLibrary(own.events_fd)
    library.strategy_set_scratch_tables_v1 = admission.strategy_set_scratch_tables_v1
    library.strategy_require_report_scratch_arena_v1 = (
        admission.strategy_require_report_scratch_arena_v1)
    # Match the selected fd fixture's planner/provenance setup. Neither
    # replacement handles files or changes report serialization.
    with mock.patch.object(run_json, "load_strategy", lambda path: library), \
            mock.patch.object(selected_window_plan, "SelectedPrimaryPlanner",
                              lambda path: types.SimpleNamespace(
                                  plan=lambda *args, **kwargs: dict(plan))), \
            mock.patch.object(run_json, "normalize_release_provenance",
                              lambda provenance, *args: provenance):
        return run_json.main(arguments)


def _child_main(argv):
    separator = argv.index("--")
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True)
    parser.add_argument("--arena-fd", type=int, required=True)
    parser.add_argument("--events-fd", type=int, required=True)
    parser.add_argument("--fixture")
    own = parser.parse_args(argv[:separator])
    arguments = argv[separator + 1:]
    os.dup2(own.arena_fd, 5)
    os.close(own.arena_fd)
    os.environ.update(PF_SCRATCH_ARENA_FD="5",
                      PF_SCRATCH_ARENA_BYTES=str(ARENA_BYTES),
                      PF_SCRATCH_WINDOW_BYTES=str(1024 * 1024))
    # CPython otherwise probes TMPDIR, then falls back to writable /tmp.
    # Pin its cached selection; the real creating calls still execute.
    tempfile.tempdir = os.environ["TMPDIR"]
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
    _install_observers(own.events_fd)
    _emit(own.events_fd, {
        "kind": "jail",
        "profile": os.environ["PINEFORGE_RUN_JSON_PROFILE"],
        "fsize": list(resource.getrlimit(resource.RLIMIT_FSIZE)),
        "sigxfsz_ignored": signal.getsignal(signal.SIGXFSZ) == signal.SIG_IGN,
        "arena_bytes": os.fstat(5).st_size,
        "arena_seals": fcntl.fcntl(5, 1034),
        "work_read_only": not bool(os.stat(".").st_mode & 0o222),
        "tmp_read_only": not bool(os.stat(os.environ["TMPDIR"]).st_mode & 0o222),
    })
    try:
        if own.mode == "json-selected":
            status = _selected_json_main(own, arguments)
        elif own.mode.startswith("json"):
            library = _JailedLibrary(own.events_fd, fail=own.mode == "json-error")
            with mock.patch.object(run_json, "load_strategy", lambda path: library):
                status = run_json.main(arguments)
        elif own.mode.startswith("finalizer"):
            status = finalizer.main(arguments)
        else:
            status = _strategy_main(own, arguments)
    except OSError as error:
        _emit(own.events_fd, {"kind": "os-error", "errno": error.errno,
                              "type": type(error).__name__, "detail": str(error)})
        status = 1
    except run_json.RunFailure as error:
        _emit(own.events_fd, {"kind": "typed-refusal", "code": error.code,
                              "args": error.code_args, "detail": str(error)})
        status = error.exit_status
    except RuntimeError as error:
        if not getattr(error, "run_failure_code", None):
            raise
        _emit(own.events_fd, {"kind": "typed-refusal", "code": error.run_failure_code,
                              "args": json.loads(error.run_failure_args),
                              "detail": str(error)})
        status = 1
    except SystemExit as error:
        status = error.code
    finally:
        sys.setprofile(None)
    _emit(own.events_fd, {"kind": "exit", "status": status})
    return status


def _inputs(tmp_path, *, trim=False):
    directory = tmp_path / "inputs"
    directory.mkdir()
    source = bars.csv_file(directory)
    probe = directory / "strategy"
    probe.mkdir()
    (probe / "generated.cpp").write_text("int fixture = 0;\n", encoding="ascii")
    (probe / "inputs.json").write_text(
        json.dumps({"ohlcv_start_ms": bars.T0 + 60000} if trim else {}), encoding="ascii")
    trade = strategy_fixture.trade_fixture()[0]
    record = dict(trade, n=1, side="long" if trade["is_long"] else "short")
    fixture = directory / "native-result.sink"
    strategy_fixture.write_diagnostic_sink(fixture, [
        ["E", bars.T0, 10000., 0.],
        ["T", *(record[field] for field in sink_fixture.TRADE_FIELDS)],
        ["X", bars.T0, 0, "repeated", 1.],
        ["X", bars.T0 + 60000, 1, "repeated", 2.],
        ["X", bars.T0 + 120000, 2, "other", 3.],
    ])
    return source, probe, fixture


def _child(tmp_path, mode, arguments, *, fixture=None, input_bytes=None, sink_fd=False):
    directory = tmp_path / mode
    directory.mkdir()
    work, temporary = directory / "work", directory / "tmp"
    work.mkdir(mode=0o555)
    temporary.mkdir(mode=0o555)
    arena = sealed_arena_fd(ARENA_BYTES)
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("PF_SCRATCH_", "PINEFORGE_"))
    }
    environment.update(PYTHONDONTWRITEBYTECODE="1", PINEFORGE_RUN_JSON_PROFILE="jailed",
                       TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
    try:
        with Stream() as events, Stream() as sink:
            # Keep every passed endpoint clear of the arena's fixed slot 5.
            for stream in (events, sink):
                moved = fcntl.fcntl(stream.writer, fcntl.F_DUPFD_CLOEXEC, 32)
                os.close(stream.writer)
                stream.writer = moved
            command = [sys.executable, "-B", str(Path(__file__).resolve()),
                       "--mode", mode, "--arena-fd", str(arena),
                       "--events-fd", str(events.writer)]
            if fixture is not None:
                command += ["--fixture", str(fixture)]
            actual_arguments = [
                str(value).replace("{work}", str(work)).replace("{tmp}", str(temporary))
                for value in arguments
            ]
            if sink_fd:
                actual_arguments += ["--report-sink-fd", str(sink.writer)]
            command += ["--", *actual_arguments]
            process = subprocess.Popen(
                command, cwd=work, env=environment,
                pass_fds=(arena, events.writer, sink.writer),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            events.close_writer()
            sink.close_writer()
            try:
                stdout, stderr = process.communicate(input_bytes, timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise AssertionError(f"{mode}: child did not finish")
        observed = [json.loads(line) for line in events.data.splitlines()]
        leftovers = {path.name: sorted(str(child.relative_to(path))
                                     for child in path.rglob("*"))
                     for path in (work, temporary)}
        return types.SimpleNamespace(
            status=process.returncode, stdout=stdout, stderr=stderr,
            events=observed, sink=sink.data, leftovers=leftovers)
    finally:
        os.close(arena)
        work.chmod(0o755)
        temporary.chmod(0o755)


def _entered(result, file, function):
    return any(event == {"kind": "entered", "file": file, "function": function}
               for event in result.events)


def _attempts(result, file=None, function=None):
    return [
        event for event in result.events
        if event["kind"] == "filesystem" and event["creation"]
        and (file is None or any(
            frame["file"] == file and (function is None or frame["function"] == function)
            for frame in event["stack"]))
    ]


def _assert_jail(result):
    assert result.events[0] == {
        "kind": "jail", "profile": "jailed", "fsize": [0, 0],
        "sigxfsz_ignored": True, "arena_bytes": ARENA_BYTES, "arena_seals": 7,
        "work_read_only": True, "tmp_read_only": True,
    }, result.events
    assert result.events[-1] == {"kind": "exit", "status": result.status}, result.events


def _assert_no_files(result, site):
    assert not _attempts(result) and not any(result.leftovers.values()), (
        f"{site}: no temporary file may be created under the jailed profile; "
        f"creation attempts={_attempts(result)!r}; "
        f"directory entries={result.leftovers!r}; "
        f"exit={result.status}; stderr={result.stderr!r}")


def _assert_driver_refusal(result):
    _assert_jail(result)
    assert result.status == 1 and result.stdout == result.stderr == b"", result
    refusals = [event for event in result.events if event["kind"] == "typed-refusal"]
    assert refusals == [{
        "kind": "typed-refusal", "code": "run_request_invalid",
        "args": {"option": "arguments"},
        "detail": ("jailed profile excludes scripts/run_strategy.py: "
                   "strategy_driver_no_jailed_route"),
    }], result.events
    assert _entered(result, RUN_STRATEGY, "main"), result.events
    assert not any(event["kind"] == "native-result" for event in result.events)
    assert not _entered(result, RUN_STRATEGY, "_report_from_sink"), result.events
    assert not _entered(result, RUN_STRATEGY, "_run_via_docker"), result.events
    assert not _entered(result, RUN_STRATEGY, "_trim_ohlcv_csv"), result.events
    assert not _entered(result, RUN_STRATEGY, "_write_sink_engine_trades_csv")
    assert not _entered(result, RUN_STRATEGY, "_SinkTraceNames.__iter__"), result.events


def test_final_json_spool_stays_resident(tmp_path):
    """The TemporaryFile at docker/run_json.py:7659 is bypassed by the
    resident serializer in docker/run_json.py:7905-7910.

    Ordinary arrays, recorded arrays, benchmark and selected-window reports
    reach the real resident serializer; a native error reaches write_failure.
    """
    source, _probe, _fixture = _inputs(tmp_path)
    base = ["--so", "unused.so", "--ohlcv", source, "--profile", "jailed"]
    for mode, extra in (
        ("json-array", []),
        ("json-outputs", ["--outputs"]),
        ("json-bench", ["--bench", "--warmup", "1", "--repeats", "1"]),
        ("json-error", []),
    ):
        result = _child(tmp_path, mode, [*base, *extra])
        _assert_jail(result)
        if mode == "json-error":
            assert result.status == 1 and result.stderr == b"", result
            document = json.loads(result.stdout)
            assert document["code"] == "scratch_capacity_exceeded", document
            assert "summary" not in document
            assert _entered(result, RUN_JSON, "write_failure"), result.events
            assert any(event["kind"] == "native-run" for event in result.events)
        else:
            assert result.status == 0 and result.stderr == b"", result
            document = json.loads(result.stdout)
            sink_fixture.assert_run_profile(document, "jailed", admitted=True)
            assert document["summary"]["total_trades"] == 3, document
            assert len(document["trades"]) == 3 and len(document["equity_curve"]) == 5
            assert _entered(result, RUN_JSON, "_prepare_report_json"), result.events
            assert {"kind": "prepared", "resident": True} in result.events
            if mode == "json-outputs":
                assert _entered(result, RUN_JSON, "_legacy_streamed_outputs_block")
                assert len(document["outputs"]["series"]) == 2
                assert len(document["outputs"]["bars"]["open_ms"]) == 2
            if mode == "json-bench":
                assert len(document["diagnostics"]["timing"]["samples_ns"]) == 1
        _assert_no_files(result, "final JSON spool (_json_temporary)")

    selected_inputs = tmp_path / "selected-inputs"
    selected_inputs.mkdir()
    chart = selected_inputs / "chart.csv"
    selected.write_ohlcv(chart, selected.csv_rows(10))
    artifact, inventory = selected.write_inventory(selected_inputs, [])
    result = _child(tmp_path, "json-selected", selected.selected_argv(
        artifact, chart, inventory, extra=["--profile", "jailed"]), sink_fd=True)
    _assert_jail(result)
    assert result.status == 0 and result.stderr == b"", result
    document = json.loads(result.stdout)
    sink_fixture.assert_run_profile(document, "jailed", admitted=True)
    assert document["summary"]["bars_processed"] == 9
    assert document["report_window"]["applied"] is True
    assert document["report_window"]["window"]["coverage"] == "nonempty"
    assert document["report_shape"]["emitted_equity_points"] == 7
    assert document["fingerprint"]["version"] == 2
    assert _entered(result, RUN_JSON, "_selected_run")
    assert _entered(result, RUN_JSON, "_prepare_report_json")
    assert {"kind": "prepared", "resident": True} in result.events
    with pf_sink.SinkReader(stream=io.BytesIO(result.sink)) as reader:
        tags = [record.tag for record in reader]
        assert reader.complete and tags.count("T") == 3 and tags.count("E") == 7
    _assert_no_files(result, "final JSON spool (selected-window report)")


def test_output_columns_use_fd_metadata_without_implicit_recording(tmp_path):
    """The lazy columns in `_legacy_streamed_outputs_block`
    (docker/run_json.py:8157-8164) need no JSON temporary.

    The fd metadata return in `_sink_outputs_block`
    (docker/run_json.py:8279-8281) precedes all columns; the jailed
    path-sink refusal closes the other route. Plain fd runs must not enable
    recording. Both the document and verified wire pin those paths.
    """
    source, _probe, _fixture = _inputs(tmp_path)
    base = ["--so", "unused.so", "--ohlcv", source, "--profile", "jailed"]
    for mode, outputs in (("json-fd-outputs", True), ("json-fd-plain", False)):
        result = _child(tmp_path, mode, [*base, *(["--outputs"] if outputs else [])],
                        sink_fd=True)
        _assert_jail(result)
        assert result.status == 0 and result.stderr == b"", result
        document = json.loads(result.stdout)
        sink_fixture.assert_run_profile(document, "jailed", admitted=True)
        assert document["summary"]["total_trades"] == 3
        assert "trades" not in document and "equity_curve" not in document
        with pf_sink.SinkReader(stream=io.BytesIO(result.sink)) as reader:
            tags = [record.tag for record in reader]
            assert reader.complete and tags.count("T") == 3
        enabled = [event for event in result.events if event["kind"] == "outputs-enabled"]
        if outputs:
            assert enabled == [{"kind": "outputs-enabled", "enabled": True}]
            assert _entered(result, RUN_JSON, "_sink_outputs_block")
            assert tags.count("O") == 2 and tags.count("V") == tags.count("K") == 1
            assert set(document["outputs"]) == {
                "schema_version", "message_format", "manifest_sha256",
                "manifest", "constants", "hlines",
            }
        else:
            assert enabled == [] and not any(tag in tags for tag in "OVK"), result.events
            assert "outputs" not in document
            assert not _entered(result, RUN_JSON, "_sink_outputs_block")
        _assert_no_files(result, "output columns and series (_JSONColumn)")

    refused = _child(tmp_path, "json-path-refusal",
                     [*base, "--outputs", "--report-sink", "{work}/report.sink"])
    _assert_jail(refused)
    assert refused.status == 1 and refused.stderr == b"", refused
    document = json.loads(refused.stdout)
    assert document["code"] == "run_request_invalid", document
    assert document["args"] == {"option": "arguments"}, document
    assert "refuses --report-sink paths" in document["error"], document
    assert not any(event["kind"] == "native-run" for event in refused.events)
    _assert_no_files(refused, "output columns and series (jailed path-sink refusal)")


def test_trace_name_dedup_requires_a_jailed_guard(tmp_path):
    """The trace CLI must refuse before native results or dedup staging.

    The former static fallback is replaced by an exact typed refusal from
    the real entry point. No earlier file operation is mocked away.
    """
    source, probe, fixture = _inputs(tmp_path)
    result = _child(tmp_path, "trace-names", [
        probe, "--ohlcv", source, "--no-trim-output",
        "--output", os.devnull, "--trace-json", os.devnull,
    ], fixture=fixture)
    _assert_driver_refusal(result)
    _assert_no_files(result, "trace-name dedup (jailed refusal)")


def test_feed_trim_creates_no_temporary_under_jailed_environment(tmp_path):
    """The Docker CLI must refuse before mkstemp(prefix='pf_ohlcv_').

    The input bound would require feed trimming without the entry guard.
    """
    source, probe, _fixture = _inputs(tmp_path, trim=True)
    result = _child(tmp_path, "feed-trim", [
        probe, "--ohlcv", source, "--no-trim-output",
        "--runner", "docker", "--output", os.devnull,
    ])
    _assert_driver_refusal(result)
    _assert_no_files(result, "feed trim (_trim_ohlcv_csv / pf_ohlcv_*.csv)")


def test_reverse_csv_staging_creates_no_temporaries_under_jailed_environment(tmp_path):
    """The CSV CLI must refuse before rows and offsets TemporaryFiles.

    The verified fixture would give main() real _SinkRows without the guard.
    """
    source, probe, fixture = _inputs(tmp_path)
    result = _child(tmp_path, "reverse-csv", [
        probe, "--ohlcv", source, "--no-trim-output", "--output", os.devnull,
    ], fixture=fixture)
    _assert_driver_refusal(result)
    _assert_no_files(result, "reverse-CSV staging (rows and offsets TemporaryFile)")


def test_finalizer_spool_creates_no_file_under_jailed_environment(tmp_path):
    """File mode must refuse before mkstemp('.pineforge-sink-').

    The same real finalizer still verifies a complete stdout-mode stream
    with a positive size cap and no spool.
    """
    _source, _probe, fixture = _inputs(tmp_path)
    wire = fixture.read_bytes()
    stdout = _child(tmp_path, "finalizer-stdout", [
        "--in-fd", "0", "--out", "-", "--receipt-fd", "2",
        "--max-bytes", str(len(wire)), "--spool-dir", "{tmp}",
    ], input_bytes=wire)
    # Use the actual finalizer entry point for both modes.
    _assert_jail(stdout)
    assert stdout.status == 0 and stdout.stdout == wire, stdout
    receipt = json.loads(stdout.stderr)
    assert receipt["complete"] is True and receipt["count_t"] == 1
    assert receipt["count_x"] == 3 and receipt["sink_bytes"] == len(wire)
    assert _entered(stdout, FINALIZER, "_verify")
    assert not _entered(stdout, FINALIZER, "_receive")
    _assert_no_files(stdout, "finalizer stdout mode")

    result = _child(tmp_path, "finalizer-file", [
        "--in-fd", "0", "--out", "{work}/result.sink",
        "--max-bytes", str(len(wire)), "--spool-dir", "{tmp}",
    ], input_bytes=wire)
    _assert_jail(result)
    assert result.status == 3, result
    assert result.stderr == (
        b"pineforge-sink-finalize: jailed profile excludes "
        b"pineforge-sink-finalize file-output mode: "
        b"finalizer_file_output_no_jailed_route\n"), result
    receipt = json.loads(result.stdout)
    expected = {
        "complete": False,
        **{"count_" + tag: 0 for tag in "etovkxh"},
        "sink_bytes": 0, "sha256": "", "finalizer_version": "1.1.0",
    }
    assert receipt == expected and tuple(receipt) == tuple(expected), receipt
    assert not any(event["kind"] == "typed-refusal" for event in result.events)
    assert _entered(result, FINALIZER, "main")
    assert not _entered(result, FINALIZER, "_receive")
    assert not _entered(result, FINALIZER, "_verify")
    _assert_no_files(result, "finalizer spool (.pineforge-sink-*.spool)")


if __name__ == "__main__":
    sys.exit(_child_main(sys.argv[1:]))
