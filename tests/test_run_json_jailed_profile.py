"""RAM host policy at the ctypes boundary; no native-backend proof is claimed.

Modeled on tests/test_run_json_sink.py (assignable fake exports and real report
assembly), with the selected-route observer/planner harness from
tests/test_run_json_nonfinite_report.py. The new CLI is red on d91c96d47d:
--profile is unrecognized; the binding row first lacks ScratchTablesC.
No build or test was run on the executor Mac.
"""
from __future__ import annotations

import contextlib
import ctypes
import importlib.util
import io
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_bin as bars
import test_run_json_nonfinite_report as reports
import test_run_json_sink as sinks
import test_run_json_sink_fd as fd_sinks

REAL_CDLL = ctypes.CDLL

PROFILE_ENV = "PINEFORGE_RUN_JSON_PROFILE"
SETTER = "strategy_set_scratch_tables_v1"
FIELDS = ("K_q", "B_q", "K_e", "B_e", "K_c", "B_c", "U", "B_U", "V", "B_V", "R")
# P0 policy, independently transcribed from the brief, after q removal.
P0_FIXED = (0, 0, 262144, 25165824, 32768, 4227072, 65536, 4194304, 27, 6912)


class Descriptor(ctypes.Structure):
    """Independent ABI mirror; never use the producer's _fields_ to decode it."""
    _fields_ = [(name, ctypes.c_uint32) for name in
                ("struct_size", "version", "backend", "reserved")] + [
        (name, ctypes.c_uint64) for name in FIELDS]


class ScratchRecorder:
    def init_scratch(self, *, symbol=True, setter_status=0):
        self.history, self.configured, self.created, self.released = [], {}, [], []
        self.setter_status = setter_status
        self.on_create = None
        self.arena_status = 0
        self.arena_admissions = []
        def admit_arena(state):
            self.arena_admissions.append((state, tuple(self.history)))
            return self.arena_status
        self.strategy_require_report_scratch_arena_v1 = admit_arena
        if symbol:
            def install(state, pointer):
                copy = Descriptor.from_buffer_copy(ctypes.string_at(pointer, 104))
                snapshot = tuple(getattr(copy, name) for name in FIELDS)
                self.history.append(("scratch", state,
                                     (copy.struct_size, copy.version, copy.backend, copy.reserved),
                                     snapshot))
                if self.setter_status == 0:
                    self.configured[state] = snapshot
                return self.setter_status
            setattr(self, SETTER, install)

    def create(self, params):
        state = (1 << 40) + len(self.created) + 1
        self.created.append(state)
        self.history.append(("create", state))
        if self.on_create is not None:
            self.on_create()
        return state

    def record_run(self, state, count):
        self.history.append(("run", state, count, self.configured.get(state)))

    def release(self, state):
        self.history.append(("free", state))
        self.released.append(state)
        self.configured.pop(state, None)


class ProfileLibrary(ScratchRecorder, sinks.SinkLibrary):
    def __init__(self, *, symbol=True, setter_status=0, fail_run=None):
        super().__init__()
        self.init_scratch(symbol=symbol, setter_status=setter_status)
        self.scratch_failure, self.run_count, self.failure = fail_run, 0, None
        self.strategy_create = self.export("strategy_create", self.create)
        self.strategy_free = self.export("strategy_free", self.free)
        self.strategy_set_input = self.export("strategy_set_input", self.input)
        self.strategy_set_override = self.export("strategy_set_override", self.override)
        if symbol:
            setattr(self, SETTER, self.export(SETTER, getattr(self, SETTER)))
        for name, body in {
            "strategy_get_last_error": lambda state: b"scratch test failure" if self.failure else b"",
            "strategy_get_last_error_code": lambda state: self.failure[0].encode() if self.failure else b"",
            "strategy_get_last_error_args": lambda state: json.dumps(self.failure[1]).encode() if self.failure else b"{}",
            "strategy_last_run_status": lambda state: 1 if self.failure else 0,
        }.items():
            setattr(self, name, self.export(name, body))

    def input(self, state, key, value):
        self.history.append(("input", state, key, value))

    def override(self, state, key, value):
        self.history.append(("override", state, key, value))

    def run(self, state, feed, count, *args):
        self.record_run(state, count)
        self.run_count += 1
        self.failure = self.scratch_failure if self.run_count == 1 else None
        super().run(state, feed, count, *args)

    def free(self, state):
        super().free(state)
        self.release(state)


class SelectedProfileLibrary(ScratchRecorder, reports.ReportLib):
    def __init__(self, *args, symbol=True, **kwargs):
        super().__init__(*args, **kwargs)
        self.init_scratch(symbol=symbol)
        native_free = self.strategy_free
        def create(params):
            self.calls.append(("strategy_create", params))
            return self.create(params)
        def free(state):
            native_free(state)
            self.release(state)
        self.strategy_create, self.strategy_free = create, free

    def _run(self, state, rows, count, *args):
        self.record_run(state, count)
        return super()._run(state, rows, count, *args)


class ProfileFdLibrary(ScratchRecorder, fd_sinks.FdLibrary):
    """Use B5's real stream fixture with the existing scratch ABI recorder."""

    def __init__(self, *, symbol=True, setter_status=0):
        super().__init__()
        self.init_scratch(symbol=symbol, setter_status=setter_status)
        self.strategy_create = self.export("strategy_create", self.create)
        self.strategy_free = self.export("strategy_free", self.free)
        if symbol:
            setattr(self, SETTER, self.export(SETTER, getattr(self, SETTER)))

    def install_fd(self, state, pointer):
        self.history.append(("sink", state, pointer._obj.fd))
        return super().install_fd(state, pointer)

    def run(self, state, rows, count, *args):
        self.record_run(state, count)
        return super().run(state, rows, count, *args)

    def free(self, state):
        super().free(state)
        self.release(state)


class SelectedProfileFdLibrary(ScratchRecorder, fd_sinks.SelectedFdLibrary):
    def __init__(self, *, symbol=True, setter_status=0, empty=False):
        plan = (fd_sinks.selected.empty_window_plan() if empty
                else fd_sinks.selected.full_plan())
        super().__init__(plan, pnls=() if empty else fd_sinks.PNLS)
        self.init_scratch(symbol=symbol, setter_status=setter_status)
        native_free = self.strategy_free

        def free(state):
            native_free(state)
            self.release(state)

        self.strategy_create, self.strategy_free = self.create, free

    def install_fd(self, state, pointer):
        self.history.append(("sink", state, pointer._obj.fd))
        return super().install_fd(state, pointer)

    def _run(self, state, rows, count, *args):
        self.record_run(state, count)
        return super()._run(state, rows, count, *args)


def invoke_profile_sink(tmp_path, mode, library, extra, *, no_files=True):
    if mode == "selected":
        return fd_sinks.selected_invoke(tmp_path, library, extra, no_files=no_files)
    source = bars.csv_file(tmp_path)
    return fd_sinks.invoke(source, library, extra, no_files=no_files)


def invoke_policy_report(tmp_path, mode, transport, library, extra=()):
    with contextlib.ExitStack() as resources:
        options = list(extra)
        if transport == "fd":
            stream = resources.enter_context(fd_sinks.Stream())
            options += ["--report-sink-fd", str(stream.writer)]
        return invoke_profile_sink(
            tmp_path, mode, library, options, no_files=transport == "fd")


@pytest.fixture(autouse=True)
def clear_profile(monkeypatch):
    monkeypatch.delenv(PROFILE_ENV, raising=False)


@pytest.mark.parametrize("profile", [None, "unjailed", "jailed"],
                         ids=["implicit-default", "explicit-unjailed", "jailed"])
@pytest.mark.parametrize("transport", ["array", "fd"])
@pytest.mark.parametrize("route", ["ordinary", "selected", "selected-empty"])
def test_executed_report_names_profile_and_admission(tmp_path, route, transport, profile):
    mode = "ordinary" if route == "ordinary" else "selected"
    library = (ProfileFdLibrary() if mode == "ordinary" else
               SelectedProfileFdLibrary(empty=route == "selected-empty"))
    extra = [] if profile is None else ["--profile", profile]
    status, text, stderr = invoke_policy_report(tmp_path, mode, transport, library, extra)
    assert (status, stderr) == (0, ""), text
    document = reports.strict_line(text)
    sinks.assert_run_profile(document, profile or "unjailed", admitted=profile == "jailed")
    keys = list(document["diagnostics"])
    assert keys.index("scratch_arena_jailed") == keys.index("run_profile") + 1, keys
    if mode == "selected":
        assert keys.index("scratch_arena_jailed") < keys.index("phase_timing"), keys
        assert document["report_window"]["window"]["coverage"] == (
            "empty" if route == "selected-empty" else "nonempty")
    if profile == "jailed":
        roots = 10 if mode == "ordinary" else 6 if route == "selected-empty" else 18
        assert_admission(library, 1, roots, require_arena=True)
    else:
        assert library.arena_admissions == []
        assert not any(row[0] == "scratch" for row in library.history)
        assert library.released == library.created
    assert ("trades" in document) == (transport == "array")
    assert ("equity_curve" in document) == (transport == "array")


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
@pytest.mark.parametrize("transport", ["array", "fd"])
def test_arena_environment_and_sizes_do_not_select_host_profile(
        tmp_path, monkeypatch, mode, transport):
    monkeypatch.setenv("PF_SCRATCH_ARENA_JAILED", "1")
    library = ProfileFdLibrary() if mode == "ordinary" else SelectedProfileFdLibrary()
    counters = (97, 16 * 1024 * 1024)
    sinks.configure_arena_stats(library, counters)
    status, text, stderr = invoke_policy_report(tmp_path, mode, transport, library)
    assert (status, stderr) == (0, ""), text
    document = reports.strict_line(text)
    sinks.assert_run_profile(document)
    assert tuple(document["diagnostics"][key] for key in sinks.ARENA_DIAGNOSTICS) == counters
    keys = list(document["diagnostics"])
    assert keys.index("run_profile") == keys.index("scratch_arena_bytes") + 1, keys
    assert library.arena_admissions == []
    assert not any(row[0] == "scratch" for row in library.history)


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
@pytest.mark.parametrize("transport", ["array", "fd"])
def test_failed_arena_admission_has_no_profile_diagnostics(tmp_path, mode, transport):
    library = ProfileFdLibrary() if mode == "ordinary" else SelectedProfileFdLibrary()
    library.arena_status = -1
    status, text, stderr = invoke_policy_report(
        tmp_path, mode, transport, library, ["--profile", "jailed"])
    assert (status, stderr) == (1, ""), text
    document = reports.strict_line(text)
    assert (document["code"], document["args"]) == (
        "report_sink_failed", {"reason": "arena_backing"}), document
    sinks.assert_no_run_profile(document)
    assert "summary" not in document and "diagnostics" not in document, document
    assert len(library.arena_admissions) == 1
    assert not any(row[0] == "run" for row in library.history)
    assert not library.installed and library.released == library.created


@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
@pytest.mark.parametrize("kind", ["capabilities", "validation"])
def test_non_execution_documents_have_no_profile_diagnostics(
        tmp_path, monkeypatch, profile, kind):
    if kind == "capabilities":
        library = ProfileLibrary()
        status, text, stderr = invoke(
            tmp_path, monkeypatch, library, "--profile", profile, "--capabilities-json")
    else:
        library = SelectedProfileFdLibrary()
        status, text, stderr = invoke_profile_sink(
            tmp_path, "selected", library, ["--profile", profile, "--validate-window-only"])
    assert (status, stderr) == (0, ""), text
    document = reports.strict_line(text)
    sinks.assert_no_run_profile(document)
    assert "summary" not in document and "diagnostics" not in document, document
    assert ("capabilities" in document) == (kind == "capabilities")
    assert document.get("validation_only", False) == (kind == "validation")
    assert library.created == [] and library.arena_admissions == []


def forbid_spools(monkeypatch):
    hits = []
    def forbidden(*args, **kwargs):
        hits.append((args, kwargs))
        raise AssertionError("jailed host attempted a sink/spool file")
    monkeypatch.setattr(run_json.tempfile, "mkstemp", forbidden)
    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", forbidden)
    return hits


def test_jailed_serializer_handles_lazy_arrays_without_a_file(monkeypatch):
    files = forbid_spools(monkeypatch)
    rows = [{"value": -0.0}, {"value": 'quote"\n漢'}]
    document = {"rows": run_json._JSONRecordArray(lambda: iter(rows), len(rows))}
    args = run_json.argparse.Namespace(profile="jailed", spill_dir=None)
    with contextlib.ExitStack() as resources:
        buffer = run_json._prepare_report_json(document, resources, args, None)
        assert buffer.read() == json.dumps(
            {"rows": rows}, separators=(",", ":"), allow_nan=False) + "\n"
    assert buffer.closed and files == []


def invoke(tmp_path, monkeypatch, library, *extra):
    source = bars.csv_file(tmp_path)
    monkeypatch.setattr(run_json.ctypes, "CDLL", lambda path, *args, **kwargs: (
        REAL_CDLL(path, *args, **kwargs) if path is None else library))
    monkeypatch.setattr(run_json.time, "time", lambda: 10.0)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        status = run_json.main(["--so", "unused.so", "--ohlcv", str(source), *extra])
    return status, stdout.getvalue(), stderr.getvalue()


def assert_admission(library, count, roots, *, require_arena=False):
    admissions = [row for row in library.history if row[0] == "scratch"]
    runs = [row for row in library.history if row[0] == "run"]
    assert len(admissions) == len(runs) == len(library.created) == count
    assert library.released == library.created
    assert len(set(library.created)) == count
    assert not library.configured
    if require_arena:
        assert [state for state, before in library.arena_admissions] == library.created
    for state in library.created:
        history = [row for row in library.history if row[1] == state]
        names = [row[0] for row in history]
        assert names.count("scratch") == names.count("run") == 1
        assert names.index("create") < names.index("scratch") < names.index("run") < names.index("free")
        if require_arena:
            before_arena = next(before for owner, before in library.arena_admissions if owner == state)
            assert [row[0] for row in before_arena if row[1] == state][-1] == "scratch"
        for setting in ("input", "override"):
            if setting in names:
                assert names.index(setting) < names.index("scratch")
        admission = next(row for row in history if row[0] == "scratch")
        run = next(row for row in history if row[0] == "run")
        assert admission[2] == (104, 1, 1, 0)
        assert admission[3] == run[3] == P0_FIXED + (roots,)


def test_jailed_configures_every_fresh_bench_and_body_handle_without_a_sink(tmp_path, monkeypatch):
    library = ProfileLibrary()
    files = forbid_spools(monkeypatch)
    result = invoke(tmp_path, monkeypatch, library, "--profile", "jailed", "--bench",
                    "--warmup", "2", "--repeats", "3", "--inputs", '{"Fast EMA":"3"}',
                    "--overrides", '{"pyramiding":"2"}')
    assert (result[0], result[2]) == (0, ""), result
    sinks.assert_run_profile(reports.strict_line(result[1]), "jailed", admitted=True)
    assert json.loads(result[1])["summary"]["total_trades"] == 3
    assert_admission(library, 6, 10, require_arena=True)  # five input bars, including each warmup/repeat.
    assert files == [] and library.installed == [] and library.queried == 0
    assert not list(tmp_path.glob("*.sink"))


def test_selected_uses_retained_fed_count_before_its_native_run(tmp_path, monkeypatch):
    monkeypatch.setattr(reports, "ReportLib", SelectedProfileLibrary)
    harness = reports.Harness(tmp_path)
    files = forbid_spools(monkeypatch)
    result = harness.run("selected", extra=["--profile", "jailed"])
    assert (result.status, result.stderr) == (0, ""), result.stdout
    sinks.assert_run_profile(reports.strict_line(result.stdout), "jailed", admitted=True)
    assert_admission(result.library, 1, 18, require_arena=True)  # ten supplied rows, nine retained/fed.
    assert result.library.run_args[1] == 9
    assert result.library.window_config is None and result.library.observer_descriptor is None
    assert files == []


def test_selected_unjailed_report_and_existing_preroll_prefix_stay_unchanged(tmp_path, monkeypatch):
    monkeypatch.setattr(reports, "ReportLib", SelectedProfileLibrary)
    harness = reports.Harness(tmp_path)
    ordinary = harness.run("selected")
    original_argv = reports.selected.selected_argv
    def abbreviated(*args, **kwargs):
        return ["--p" if token == "--preroll-bars" else token
                for token in original_argv(*args, **kwargs)]
    monkeypatch.setattr(reports.selected, "selected_argv", abbreviated)
    explicit = harness.run("selected", extra=["--profile", "unjailed"])
    assert (ordinary.status, ordinary.stderr, explicit.status, explicit.stderr) == (0, "", 0, "")
    sinks.assert_run_profile(reports.strict_line(ordinary.stdout))
    sinks.assert_run_profile(reports.strict_line(explicit.stdout))
    assert ordinary.stdout == explicit.stdout
    assert ordinary.library.history == explicit.library.history
    assert not any(row[0] == "scratch" for row in explicit.library.history)
    assert [row[0] for row in ordinary.library.calls] == [row[0] for row in explicit.library.calls]


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
def test_missing_setter_is_a_typed_incompatibility_without_execution_or_sink(tmp_path, monkeypatch, mode):
    files = forbid_spools(monkeypatch)
    if mode == "ordinary":
        library = ProfileLibrary(symbol=False)
        status, text, stderr = invoke(tmp_path, monkeypatch, library, "--profile", "jailed")
    else:
        def factory(*args, **kwargs):
            return SelectedProfileLibrary(*args, symbol=False, **kwargs)
        monkeypatch.setattr(reports, "ReportLib", factory)
        result = reports.Harness(tmp_path).run("selected", extra=["--profile", "jailed"])
        library, status, text, stderr = result.library, result.status, result.stdout, result.stderr
    document = reports.strict_line(text)
    assert status == 1 and stderr == ""
    assert document["code"] == "strategy_library_incompatible"
    # 'missing' is optional; the released closed vocabulary does not yet name
    # this export. Do not send a new unregistered argument value to APP.
    assert document["args"] == {"reason": "setter_missing"}
    assert SETTER in document["error"] and "summary" not in document
    assert [row[0] for row in library.history] == ["create", "free"]
    assert library.released == library.created and files == []


def test_setter_refusal_frees_handle_and_never_runs_or_opens_a_sink(tmp_path, monkeypatch):
    library = ProfileLibrary(setter_status=-1)
    files = forbid_spools(monkeypatch)
    status, text, stderr = invoke(tmp_path, monkeypatch, library, "--profile", "jailed")
    assert status == 1 and stderr == ""
    assert reports.strict_line(text)["code"] == "engine_invariant"
    assert [row[0] for row in library.history] == ["create", "scratch", "free"]
    assert library.released == library.created and files == []


def test_sink_factory_refuses_file_work_before_even_probing_the_sink_abi(monkeypatch):
    class NoSinkAccess:
        def __getattr__(self, name):
            raise AssertionError("jailed sink factory probed " + name)
    files = forbid_spools(monkeypatch)
    args = run_json.argparse.Namespace(profile="jailed")
    assert run_json._new_report_sink(NoSinkAccess(), 7, args) is None
    assert files == []


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
def test_jailed_fd_overrides_sinks_off_after_admission(tmp_path, mode):
    factory = SelectedProfileFdLibrary if mode == "selected" else ProfileFdLibrary
    with fd_sinks.Stream() as reference_stream:
        reference = invoke_profile_sink(tmp_path, mode, factory(), [
            "--report-sink-fd", str(reference_stream.writer), "--run-id", fd_sinks.RUN_ID])
    assert (reference[0], reference[2]) == (0, ""), reference
    library = factory()
    with fd_sinks.Stream() as stream:
        result = invoke_profile_sink(tmp_path, mode, library, [
            "--profile", "jailed", "--report-sink-fd", str(stream.writer),
            "--run-id", fd_sinks.RUN_ID])
        assert (result[0], result[2]) == (0, ""), result
        with pytest.raises(OSError) as closed:
            fd_sinks.os.fstat(stream.writer)
        assert closed.value.errno == fd_sinks.errno.EBADF
    assert stream.data == reference_stream.data
    actual, expected = json.loads(result[1]), json.loads(reference[1])
    sinks.assert_run_profile(actual, "jailed", admitted=True)
    sinks.assert_run_profile(expected)
    for document in (actual, expected):
        del document["diagnostics"]["run_profile"]
        del document["diagnostics"]["scratch_arena_jailed"]
    assert "trades" not in actual and "equity_curve" not in actual
    if mode == "selected":
        assert "report_window" in actual and "report_shape" in actual
        actual["diagnostics"].pop("phase_timing")
        expected["diagnostics"].pop("phase_timing")
    fd_sinks.assert_document(json.dumps(actual, separators=(",", ":")) + "\n", expected)
    assert actual["summary"]["total_trades"] == len(fd_sinks.PNLS)
    assert_admission(library, 1, 18 if mode == "selected" else 10, require_arena=True)
    names = [row[0] for row in library.history]
    assert names == ["create", "scratch", "sink", "run", "free"]
    assert library.installed == [stream.writer]
    assert library.queried == library.summary_queries == 1
    assert not list(tmp_path.glob("*.sink"))


@pytest.mark.parametrize("mode", ["ordinary", "selected", "capabilities"])
def test_jailed_path_is_typed_refusal_before_library_calls(tmp_path, mode):
    library = SelectedProfileFdLibrary() if mode == "selected" else ProfileFdLibrary()
    path = tmp_path / "untouched.sink"
    path.write_bytes(b"caller-owned bytes\n")
    extra = ["--capabilities-json"] if mode == "capabilities" else []
    status, text, stderr = invoke_profile_sink(tmp_path, mode, library, [
        "--profile", "jailed", "--report-sink", str(path), *extra])
    document = reports.strict_line(text)
    assert status == 1 and stderr == ""
    assert (document["code"], document["args"]) == (
        "run_request_invalid", {"option": "arguments"})
    assert "summary" not in document and "--report-sink-fd" in document["error"]
    assert library.calls == [] and library.history == [] and library.installed == []
    assert path.read_bytes() == b"caller-owned bytes\n"


def test_jailed_path_factory_refuses_before_probing_sink_abi(monkeypatch):
    class NoSinkAccess:
        def __getattr__(self, name):
            raise AssertionError("refused path probed " + name)
    files = forbid_spools(monkeypatch)
    args = run_json.argparse.Namespace(profile="jailed", report_sink=Path("unused.sink"))
    with pytest.raises(run_json.RunFailure) as failure:
        run_json._new_report_sink(NoSinkAccess(), 7, args)
    assert (failure.value.code, failure.value.code_args) == (
        "run_request_invalid", {"option": "arguments"})
    assert files == []


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
@pytest.mark.parametrize("symbol,setter_status,code,args", [
    (False, 0, "strategy_library_incompatible", {"reason": "setter_missing"}),
    (True, -1, "engine_invariant", {}),
])
def test_jailed_fd_requires_admission_before_installing_the_sink(
        tmp_path, mode, symbol, setter_status, code, args):
    factory = SelectedProfileFdLibrary if mode == "selected" else ProfileFdLibrary
    library = factory(symbol=symbol, setter_status=setter_status)
    with fd_sinks.Stream() as stream:
        status, text, stderr = invoke_profile_sink(tmp_path, mode, library, [
            "--profile", "jailed", "--report-sink-fd", str(stream.writer)])
        fd_sinks.os.fstat(stream.writer)  # Admission failed before ownership transfer.
    document = reports.strict_line(text)
    assert status == 1 and stderr == ""
    assert (document["code"], document["args"]) == (code, args)
    assert "summary" not in document and stream.data == b""
    assert library.installed == [] and library.queried == library.summary_queries == 0
    assert library.released == library.created and len(library.created) == 1
    assert not library.configured
    assert [row[0] for row in library.history] == (
        ["create", "scratch", "free"] if symbol else ["create", "free"])


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
def test_jailed_fd_arena_refusal_preserves_caller_ownership(tmp_path, mode):
    factory = SelectedProfileFdLibrary if mode == "selected" else ProfileFdLibrary
    library = factory()
    library.arena_status = -1
    with fd_sinks.Stream() as stream:
        status, text, stderr = invoke_profile_sink(tmp_path, mode, library, [
            "--profile", "jailed", "--report-sink-fd", str(stream.writer)])
        fd_sinks.os.fstat(stream.writer)
    assert status == 1 and stderr == "", text
    document = reports.strict_line(text)
    assert (document["code"], document["args"]) == (
        "report_sink_failed", {"reason": "arena_backing"})
    sinks.assert_no_run_profile(document)
    assert [row[0] for row in library.history] == ["create", "scratch", "free"]
    assert len(library.arena_admissions) == 1 and library.arena_admissions[0][0] == library.created[0]
    assert library.released == library.created and not library.installed
    assert stream.data == b"" and "summary" not in document


@pytest.mark.parametrize("capabilities", [False, True])
def test_jailed_spill_dir_refuses_before_loading_a_library(tmp_path, monkeypatch, capabilities):
    library = ProfileLibrary()
    files = forbid_spools(monkeypatch)
    extra = ["--capabilities-json"] if capabilities else []
    status, text, stderr = invoke(tmp_path, monkeypatch, library,
                                 "--profile", "jailed", "--spill-dir", str(tmp_path), *extra)
    assert status == 1 and stderr == "" and files == [], text
    assert reports.strict_line(text)["code"] == "run_request_invalid"
    assert library.created == [] and library.calls == []


@pytest.mark.parametrize("code,args", [
    ("scratch_capacity_exceeded", {"resource": "successor_roots"}),
    ("scratch_io_failed", {}),
])
@pytest.mark.parametrize("bench", [False, True])
def test_scratch_failure_keeps_its_type_even_during_warmup(tmp_path, monkeypatch, code, args, bench):
    library = ProfileLibrary(fail_run=(code, args))
    extra = ["--bench", "--warmup", "1", "--repeats", "2"] if bench else []
    forbid_spools(monkeypatch)
    status, text, stderr = invoke(tmp_path, monkeypatch, library, "--profile", "jailed", *extra)
    document = reports.strict_line(text)
    assert status == 1 and stderr == ""
    assert (document["code"], document["args"]) == (code, args)
    sinks.assert_no_run_profile(document)
    assert "summary" not in document
    assert_admission(library, 1, 10, require_arena=True)


@pytest.mark.parametrize("symbol", [False, True])
def test_unjailed_calls_and_report_bytes_are_unchanged_by_explicit_profile(tmp_path, monkeypatch, symbol):
    implicit_lib, explicit_lib = ProfileLibrary(symbol=symbol), ProfileLibrary(symbol=symbol)
    implicit = invoke(tmp_path, monkeypatch, implicit_lib)
    explicit = invoke(tmp_path, monkeypatch, explicit_lib, "--profile", "unjailed")
    sinks.assert_run_profile(reports.strict_line(implicit[1]))
    sinks.assert_run_profile(reports.strict_line(explicit[1]))
    assert implicit == explicit and implicit[0] == 0, (implicit, explicit)
    assert implicit_lib.calls == explicit_lib.calls
    for library in (implicit_lib, explicit_lib):
        assert not any(row[0] == "scratch" for row in library.history)
        assert SETTER not in library.calls
        assert len(library.installed) == len(library.freed) == library.queried == 1
        assert not Path(library.installed[0]).exists()


@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
def test_environment_profile_is_read_once_before_any_fresh_handle(tmp_path, monkeypatch, profile):
    reads, original_get = [], run_json.os.environ.get
    def get(name, *args):
        if name == PROFILE_ENV:
            reads.append(name)
        return original_get(name, *args)
    monkeypatch.setenv(PROFILE_ENV, profile)
    monkeypatch.setattr(run_json.os.environ, "get", get)
    library = ProfileLibrary()
    def change_profile():
        # setenv saves the old value via environ.get, which would count the
        # fixture's own read on every handle. The initial setenv owns cleanup.
        run_json.os.environ[PROFILE_ENV] = "unjailed" if profile == "jailed" else "jailed"
    library.on_create = change_profile
    files = forbid_spools(monkeypatch) if profile == "jailed" else []
    status, text, stderr = invoke(tmp_path, monkeypatch, library,
                                  "--bench", "--warmup", "1", "--repeats", "2")
    assert (status, stderr) == (0, ""), text
    sinks.assert_run_profile(reports.strict_line(text), profile, admitted=profile == "jailed")
    assert reads == [PROFILE_ENV]
    if profile == "jailed":
        assert_admission(library, 4, 10, require_arena=True)
    else:
        assert len(library.created) == 4 and library.released == library.created
        assert library.arena_admissions == []
        assert not any(row[0] == "scratch" for row in library.history)
    assert files == []


@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
@pytest.mark.parametrize("transport", ["array", "fd"])
def test_selected_profile_is_frozen_before_admission(tmp_path, monkeypatch, profile, transport):
    monkeypatch.setenv(PROFILE_ENV, profile)
    library = SelectedProfileFdLibrary()
    library.on_create = lambda: run_json.os.environ.__setitem__(
        PROFILE_ENV, "unjailed" if profile == "jailed" else "jailed")
    status, text, stderr = invoke_policy_report(tmp_path, "selected", transport, library)
    assert (status, stderr) == (0, ""), text
    sinks.assert_run_profile(reports.strict_line(text), profile, admitted=profile == "jailed")
    assert bool(library.arena_admissions) is (profile == "jailed")


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
@pytest.mark.parametrize("transport", ["array", "fd"])
def test_cli_profile_overrides_the_environment(tmp_path, monkeypatch, mode, transport):
    monkeypatch.setenv(PROFILE_ENV, "jailed")
    library = (ProfileFdLibrary(symbol=False) if mode == "ordinary" else
               SelectedProfileFdLibrary(symbol=False))
    status, text, stderr = invoke_policy_report(
        tmp_path, mode, transport, library, ["--profile", "unjailed"])
    assert (status, stderr) == (0, ""), text
    sinks.assert_run_profile(reports.strict_line(text))
    assert not any(row[0] == "scratch" for row in library.history)
    assert library.arena_admissions == []
    assert len(library.installed) == 1


def test_invalid_environment_is_a_request_refusal_before_loading(tmp_path, monkeypatch):
    monkeypatch.setenv(PROFILE_ENV, "unknown")
    library = ProfileLibrary()
    status, text, stderr = invoke(tmp_path, monkeypatch, library)
    assert status == 1 and stderr == ""
    document = reports.strict_line(text)
    assert (document["code"], document["args"]) == ("run_request_invalid", {"option": "arguments"})
    assert library.calls == [] and library.created == []


def test_optional_binding_has_the_exact_descriptor_layout(tmp_path, monkeypatch):
    library = ProfileLibrary()
    monkeypatch.setattr(run_json.ctypes, "CDLL", lambda path: library)
    assert run_json.load_strategy(tmp_path / "strategy.so") is library
    assert ctypes.sizeof(run_json.ScratchTablesC) == ctypes.sizeof(Descriptor) == 104
    setter = getattr(library, SETTER)
    assert setter.argtypes == [ctypes.c_void_p, ctypes.POINTER(run_json.ScratchTablesC)]
    assert setter.restype is ctypes.c_int
    assert [getattr(run_json.ScratchTablesC, name).offset for name, _ in Descriptor._fields_] == [
        0, 4, 8, 12, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96]
    old = ProfileLibrary(symbol=False)
    monkeypatch.setattr(run_json.ctypes, "CDLL", lambda path: old)
    assert run_json.load_strategy(tmp_path / "old.so") is old


@pytest.mark.parametrize("count", [1000, 100000, 1000000, 100000000])
def test_host_policy_mirror_uses_each_run_bar_count(count):
    manifest = run_json.default_scratch_manifest(count)
    rows = max(65536, min(1048576, count))
    erased, consumed = 4 * rows, rows // 2
    expected = (0, 0, erased, 96 * erased, consumed, 129 * consumed,
                rows, 64 * rows, 27, 6912, 2 * count)
    assert tuple(getattr(manifest, name) for name in FIELDS) == expected
    assert (manifest.struct_size, manifest.version, manifest.backend, manifest.reserved) == (104, 1, 1, 0)


def test_host_stream_and_checked_multiply():
    stream = run_json.default_scratch_manifest(None)
    assert tuple(getattr(stream, name) for name in FIELDS) == P0_FIXED + (65536,)
    assert run_json.default_scratch_manifest(0).R == 0
    assert run_json.default_scratch_manifest(((1 << 64) - 1) // 2).R == (1 << 64) - 2
    for count in (1 << 63, (1 << 64) - 1, -1):
        with pytest.raises(run_json.RunFailure) as failure:
            run_json.default_scratch_manifest(count)
        assert (failure.value.code, failure.value.code_args) == (
            "scratch_capacity_exceeded", {"resource": "layout"})


class HpoLibrary(ScratchRecorder, bars.Library):
    def __init__(self, *, symbol=True):
        super().__init__()
        self.init_scratch(symbol=symbol)
        self.strategy_create = self.export("strategy_create", self.create)
        self.strategy_free = self.export("strategy_free", self.release)
        def input(state, key, value):
            self.history.append(("input", state, key, value))
        self.strategy_set_input = self.export("strategy_set_input", input)
        def run(state, feed, count, pointer):
            self.record_run(state, count)
            pointer._obj.net_profit = float(len(self.created))
            pointer._obj.total_trades = 1
        self.run_backtest = self.export("run_backtest", run)


def grid_fixture(tmp_path, monkeypatch, library):
    path = ROOT / "benchmarks/throughput/grid_search_repro.py"
    spec = importlib.util.spec_from_file_location("ramhost_grid_search", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.__file__ = str(tmp_path / "benchmarks/throughput/grid_search_repro.py")
    asset = tmp_path / "benchmarks/assets/strategies" / module.SLUG / "strategy.dylib"
    asset.parent.mkdir(parents=True)
    asset.write_bytes(b"ctypes stand-in, never dlopened\n")
    data = tmp_path / "benchmarks/assets/data/ETHUSDT_15.csv"
    data.parent.mkdir(parents=True)
    data.write_text(bars.csv_file(tmp_path, count=3).read_text())
    monkeypatch.setattr(module.ctypes, "CDLL", lambda path: library)
    return module


def test_hpo_example_installs_this_trial_manifest_on_all_27_fresh_handles(tmp_path, monkeypatch, capsys):
    library = HpoLibrary()
    module = grid_fixture(tmp_path, monkeypatch, library)
    module.main()
    assert_admission(library, 27, 6)
    assert "OPTIMIZATION COMPLETE" in capsys.readouterr().out
    for state in library.created:
        history = [row for row in library.history if row[1] == state]
        assert [row[0] for row in history] == ["create", "input", "input", "input", "scratch", "run", "free"]


def test_hpo_missing_setter_refuses_and_releases_the_trial(tmp_path, monkeypatch):
    library = HpoLibrary(symbol=False)
    module = grid_fixture(tmp_path, monkeypatch, library)
    with pytest.raises(run_json.StrategyLibraryError) as failure:
        module.main()
    assert failure.value.code == "strategy_library_incompatible"
    assert failure.value.code_args == {"reason": "setter_missing"}
    assert not any(row[0] == "run" for row in library.history)
    assert library.released == library.created and len(library.created) == 1
