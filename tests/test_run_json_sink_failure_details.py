"""Typed sink details and attachment-lifetime success diagnostics.

Reuse the fd suite's native-call doubles and real stream/subprocess helpers.
Native rows use the lab-supplied module to check W1's writer translation.
These tests were written, not executed, on the edit-only lane.
"""
from __future__ import annotations

import contextlib
import ctypes
import errno
import fcntl
import io
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json
import run_json_codes_test as catalog
import test_run_json_sink_fd as fd


REASONS = (
    "arena_exhausted", "arena_io", "arena_backing", "scratch_io",
    "sink_io", "sink_peer_closed", "sink_exhausted",
)
ARENA_DIAGNOSTICS = ("scratch_arena_high_water_bytes", "scratch_arena_bytes")


def payload_for(reason):
    payload = {"reason": reason}
    if reason == "arena_exhausted":
        # Model an attachment with a tail and a failed grow larger than its
        # free space. The transport must retain the snapshot beyond 32 bits.
        chunk, chunks = 1 << 20, 1 << 13
        payload.update(
            arena_bytes=chunks * chunk + len(b"tail"),
            high_water_bytes=(chunks - 1) * chunk,
            claimed_bytes=(chunks - 2) * chunk,
            needed_bytes_lower_bound=3 * chunk,
            store="curve",
        )
    elif reason == "sink_exhausted":
        cap = (1 << 32) + len(b"planned sink")
        payload.update(cap_bytes=cap, sink_bytes=cap + 1)
    return payload


def failure_document(text, payload):
    assert text.endswith("\n") and len(text.splitlines()) == 1
    document = json.loads(text)
    assert document["engine"] == "pineforge"
    assert document["code"] == "report_sink_failed"
    assert document["args"] == payload
    assert catalog.catalogued(document["code"], document["args"])
    for name in ("reason", "high_water_bytes", "arena_bytes"):
        if name in payload:
            assert document[name] == document["args"][name]
            assert type(document[name]) is type(payload[name])
        else:
            assert name not in document
    assert "summary" not in document
    return document


@pytest.mark.parametrize("reason", REASONS)
def test_fd_failure_mirrors_snapshot_after_close(tmp_path, reason):
    source = fd.bars.csv_file(tmp_path)
    payload = payload_for(reason)
    expected = dict(payload)
    library = fd.FdLibrary(fd_fault="engine")
    library.strategy_get_last_error_args = lambda state: json.dumps(payload).encode()
    library.pf_report_scratch_arena_stats = lambda *args: pytest.fail(
        "failure forwarding queried counters after the refusal snapshot")
    original_free = library.strategy_free

    def free(state):
        original_free(state)
        payload.clear()  # cleanup must not replace the already captured args

    library.strategy_free = free
    with fd.Stream("socket") as stream:
        class Output(io.StringIO):
            def write(self, text):
                with pytest.raises(OSError) as closed:
                    os.fstat(stream.writer)
                assert closed.value.errno == errno.EBADF
                return super().write(text)

        closes, real_close = [], os.close

        def close(descriptor):
            if descriptor == stream.writer:
                closes.append(descriptor)
            real_close(descriptor)

        with mock.patch.object(os, "close", close):
            status, text, stderr = fd.invoke(
                source, library, ["--report-sink-fd", str(stream.writer)],
                no_files=True, stdout=Output())
        assert closes == [stream.writer]
    assert status == 1 and stderr == ""
    failure_document(text, expected)
    assert not payload
    assert stream.data and b'"trailer":' not in stream.data


@pytest.mark.parametrize("payload", [
    {}, {"arena_bytes": 0}, {"high_water_bytes": (1 << 63) - 1},
    {"reason": "sink_io"},
])
def test_optional_mirrors_are_never_invented(payload):
    failure_document(run_json.failure_line("refused", "report_sink_failed", payload), payload)


def test_mirrors_use_sanitized_args_and_follow_overflow_drop():
    payload = {"reason": "x\udcff" * run_json.ERROR_ARG_TEXT_MAX, "arena_bytes": 0}
    document = json.loads(run_json.failure_line("refused", "report_sink_failed", payload))
    assert document["reason"] == document["args"]["reason"]
    assert "\udcff" not in document["reason"]
    assert document["arena_bytes"] == document["args"]["arena_bytes"] == payload["arena_bytes"]
    # Deliberately uncatalogued overload: the existing line-size safeguard
    # drops args, and must drop their mirrors at the same time.
    payload.update({f"extra_{index}": "x" * run_json.ERROR_ARG_TEXT_MAX for index in range(100)})
    text = run_json.failure_line("refused", "report_sink_failed", payload)
    assert len(text) <= run_json.ERROR_LINE_MAX + len("\n")
    assert json.loads(text) == {
        "engine": "pineforge", "error": "refused", "code": "report_sink_failed", "args": {}}


def test_sink_exhaustion_catalog_payloads():
    payload = payload_for("sink_exhausted")
    assert catalog.catalogued("report_sink_failed", payload)
    for name in ("cap_bytes", "sink_bytes"):
        assert catalog.catalogued("report_sink_failed", {name: payload[name]})
        for invalid in (str(payload[name]), float(payload[name]), True, None):
            with pytest.raises(AssertionError):
                catalog.catalogued("report_sink_failed", {**payload, name: invalid})


@pytest.mark.parametrize("selected", [False, True])
@pytest.mark.parametrize("available", [False, True])
@pytest.mark.parametrize("used", [False, True])
def test_success_stats_are_optional_and_read_before_detach(tmp_path, selected, available, used):
    library = (fd.SelectedFdLibrary(fd.selected.full_plan()) if selected else fd.FdLibrary())
    source = fd.bars.csv_file(tmp_path)
    chunk, chunks = 1 << 20, 1 << 13
    expected = ((chunks - 1) * chunk if used else 0, chunks * chunk + len(b"tail"))
    events = []
    original_free = library.strategy_free

    def stats(high_water, arena_bytes):
        # Both receipts have been checked, and strategy cleanup has not run.
        assert library.queried == library.summary_queries == 1
        assert events == []
        ctypes.cast(high_water, ctypes.POINTER(ctypes.c_uint64))[0] = expected[0]
        ctypes.cast(arena_bytes, ctypes.POINTER(ctypes.c_uint64))[0] = expected[1]
        events.append("stats")
        return 0

    def free(state):
        events.append("free")
        original_free(state)

    library.strategy_free = free
    if available:
        library.pf_report_scratch_arena_stats = stats
    else:
        assert not hasattr(library, "pf_report_scratch_arena_stats")
    with fd.Stream("socket") as stream:
        extra = ["--report-sink-fd", str(stream.writer)]
        result = (fd.selected_invoke(tmp_path, library, extra, no_files=True) if selected
                  else fd.invoke(source, library, extra, no_files=True))
    assert result[0] == 0 and result[2] == "", result
    document = json.loads(result[1])
    diagnostics = document["diagnostics"]
    for name, value in zip(ARENA_DIAGNOSTICS, expected):
        if available:
            assert type(diagnostics[name]) is int and diagnostics[name] == value
        else:
            assert name not in diagnostics
        assert name.encode() not in stream.data
        assert name not in json.dumps(document.get("fingerprint"))
    assert events == (["stats", "free"] if available else ["free"])
    assert set(json.loads(stream.data.splitlines()[-1])) == {
        "trailer", "version", "counts", "sha256"}


def test_success_stats_binding_is_optional(tmp_path, monkeypatch):
    library = fd.FdLibrary()
    library.pf_report_scratch_arena_stats = library.export("arena_stats", lambda *args: 0)
    monkeypatch.setattr(ctypes, "CDLL", lambda path: library)
    assert run_json.load_strategy(tmp_path / "strategy.so") is library
    getter = library.pf_report_scratch_arena_stats
    assert getter.restype is ctypes.c_int
    assert getter.argtypes == [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
    del library.pf_report_scratch_arena_stats
    assert run_json.load_strategy(tmp_path / "older.so") is library
    assert not hasattr(library, "pf_report_scratch_arena_stats")


@pytest.mark.parametrize("selected", [False, True])
@pytest.mark.parametrize("sink_mode", ["file", "legacy"])
def test_success_stats_also_reach_file_and_legacy_reports(tmp_path, selected, sink_mode):
    library = (fd.SelectedFdLibrary(fd.selected.full_plan()) if selected else fd.FdLibrary())
    if sink_mode == "legacy":
        for name in ("pf_report_sink_version", "strategy_set_report_sink_v1",
                     "strategy_report_sink_receipt_v1"):
            delattr(library, name)
    expected = (ctypes.sizeof(run_json.ReportSinkReceiptC), 1 << 33)
    queried, released = [], []
    original_free = library.strategy_free

    def stats(high_water, arena_bytes):
        assert not released
        ctypes.cast(high_water, ctypes.POINTER(ctypes.c_uint64))[0] = expected[0]
        ctypes.cast(arena_bytes, ctypes.POINTER(ctypes.c_uint64))[0] = expected[1]
        queried.append(True)
        return 0

    def free(state):
        released.append(True)
        original_free(state)

    library.pf_report_scratch_arena_stats = stats
    library.strategy_free = free
    result = (fd.selected_invoke(tmp_path, library, []) if selected
              else fd.invoke(fd.bars.csv_file(tmp_path), library))
    assert result[0] == 0 and result[2] == "", result
    diagnostics = json.loads(result[1])["diagnostics"]
    assert queried == released == [True]
    assert tuple(diagnostics[name] for name in ARENA_DIAGNOSTICS) == expected
    assert all(type(diagnostics[name]) is int for name in ARENA_DIAGNOSTICS)


def test_refused_stats_do_not_publish_success_or_fabricate_zeros(tmp_path):
    library = fd.FdLibrary()
    library.pf_report_scratch_arena_stats = lambda *args: -1
    with fd.Stream() as stream:
        result = fd.invoke(fd.bars.csv_file(tmp_path), library,
                           ["--report-sink-fd", str(stream.writer)], no_files=True)
        with pytest.raises(OSError) as closed:
            os.fstat(stream.writer)
        assert closed.value.errno == errno.EBADF
    assert result[0] == 1 and result[2] == ""
    document = json.loads(result[1])
    assert document["code"] == "engine_invariant" and len(result[1].splitlines()) == 1
    assert "summary" not in document and "diagnostics" not in document


@pytest.mark.parametrize("site", [
    "sink-open", "column-append", "scratch-open", "scratch-close",
    "stream-write", "stream-flush", "stream-seek", "column-read",
])
def test_python_io_raisers_keep_the_actual_storage_reason(tmp_path, monkeypatch, site):
    error = OSError(errno.ENOSPC, "injected storage failure")

    def refuse(*args, **kwargs):
        raise error

    with contextlib.ExitStack() as resources:
        if site == "sink-open":
            monkeypatch.setattr(run_json.tempfile, "mkstemp", refuse)
            action = lambda: run_json._ReportSink(object(), None, directory=tmp_path)
        elif site == "scratch-open":
            monkeypatch.setattr(run_json.tempfile, "TemporaryFile", refuse)
            action = lambda: run_json._json_temporary(resources, tmp_path)
        elif site == "scratch-close":
            file = mock.Mock()
            file.close.side_effect = refuse
            action = lambda: run_json._close_json_temporary(file)
        else:
            destination = io.StringIO()
            resources.callback(destination.close)
            monkeypatch.setattr(run_json, "_json_temporary", lambda *args: destination)
            if site == "column-append":
                column = run_json._JSONColumn(resources, tmp_path)
                monkeypatch.setattr(destination, "write", refuse)
                action = lambda: column.append(1)
            else:
                document = {"rows": [1, 2]}
                if site == "column-read":
                    column = run_json._JSONColumn(resources, tmp_path)
                    column.file = io.StringIO("1,2")
                    resources.callback(column.file.close)
                    monkeypatch.setattr(column.file, "read", refuse)
                    document["rows"] = column
                else:
                    monkeypatch.setattr(destination, site.removeprefix("stream-"), refuse)
                action = lambda: run_json._prepare_stream_report(document, resources, tmp_path)
        with pytest.raises(run_json.RunFailure) as caught:
            action()
    expected = {"reason": "sink_io" if site == "sink-open" else "scratch_io"}
    assert caught.value.code == "report_sink_failed"
    assert caught.value.code_args == expected and caught.value.__cause__ is error
    failure_document(run_json.failure_line(str(caught.value), caught.value.code, expected), expected)


def test_closed_socket_peer_is_typed_and_closed_before_stdout(tmp_path):
    reader, writer = socket.socketpair()
    owned_fd = writer.detach()
    reader.close()
    library = fd.FdLibrary(fd_fault="setter")
    observed = []

    def setter(state, ref):
        library.install_fd(state, ref)
        try:
            os.write(ref._obj.fd, b"peer probe")
        except OSError as error:
            observed.append(error.errno)
            raise
        pytest.fail("a write to the closed socket peer succeeded")

    library.strategy_set_report_sink_fd_v1 = setter

    class Output(io.StringIO):
        def write(self, text):
            with pytest.raises(OSError) as closed:
                os.fstat(owned_fd)
            assert closed.value.errno == errno.EBADF
            return super().write(text)

    closes, real_close = [], os.close

    def close(descriptor):
        if descriptor == owned_fd:
            closes.append(descriptor)
        real_close(descriptor)

    try:
        with mock.patch.object(os, "close", close):
            result = fd.invoke(fd.bars.csv_file(tmp_path), library,
                               ["--report-sink-fd", str(owned_fd)],
                               no_files=True, stdout=Output())
        assert result[0] == 1 and result[2] == "", result
        assert observed == [errno.EPIPE] and closes == [owned_fd]
        failure_document(result[1], {"reason": "sink_peer_closed"})
    finally:
        try:
            real_close(owned_fd)
        except OSError as error:
            assert error.errno == errno.EBADF


@pytest.mark.parametrize("reason", [None, "arena_exhausted", "arena_backing", "sink_peer_closed"])
def test_fd_setter_preserves_typed_failure_args_or_uses_sink_io(tmp_path, reason):
    payload = payload_for(reason) if reason is not None else {"reason": "sink_io"}
    library = fd.FdLibrary(fd_fault="setter")
    if reason is not None:
        library.strategy_get_last_error = lambda state: b"native attach refused"
        library.strategy_get_last_error_code = lambda state: b"report_sink_failed"
        library.strategy_get_last_error_args = lambda state: json.dumps(payload).encode()
    with fd.Stream() as stream:
        result = fd.invoke(fd.bars.csv_file(tmp_path), library,
                           ["--report-sink-fd", str(stream.writer)], no_files=True)
    assert result[0] == 1 and result[2] == "", result
    failure_document(result[1], payload)
    assert stream.data == b""


def test_native_closed_socket_peer_keeps_reason(tmp_path):
    source = fd.bars.csv_file(tmp_path, count=4096)
    result = fd.child_run(source, ["--so", fd.native_module(False)], stub=False,
                          transport="socket", break_after=1)
    assert result[0] == 1 and result[2] == "", result
    failure_document(result[1], {"reason": "sink_peer_closed"})
    assert result[3] and b'"trailer":' not in result[3]
    assert result[4][0] == {"event": "closed"}
    assert result[4][1:] and all(
        event == {"event": "stdout", "closed": True} for event in result[4][1:])


def unjailed_environment():
    return {name: value for name, value in os.environ.items()
            if not name.startswith(("PINEFORGE_", "PF_SCRATCH_"))}


def unjailed_native_run(source, environment=None, *, arena_fd=None):
    module = fd.native_module(False)
    # Install the inherited slot in the child, as the fd suite does. Never
    # replace the parent's fd 5, which may belong to pytest's capture.
    child = """
import errno
import os
import sys
sys.path.insert(0, sys.argv.pop(1))
inherited = int(sys.argv.pop(1))
import run_json
if inherited >= 0:
    os.dup2(inherited, 5)
    if inherited != 5:
        os.close(inherited)
else:
    try:
        os.close(5)
    except OSError as error:
        if error.errno != errno.EBADF:
            raise
run_json.time.time = lambda: 10.0
raise SystemExit(run_json.main(sys.argv[1:]))
"""
    with tempfile.TemporaryDirectory(
            prefix="pineforge-sink-admission-", dir=run_json._disk_report_directory()) as temporary:
        clean = unjailed_environment()
        clean.update(LANG="C", LC_ALL="C", TZ="UTC", PYTHONDONTWRITEBYTECODE="1",
                     PYTHONPATH=str(ROOT / "docker"), TMPDIR=temporary, TMP=temporary, TEMP=temporary)
        clean.update(environment or {})
        result = subprocess.run(
            [sys.executable, "-B", "-c", child, str(ROOT / "docker"),
             str(arena_fd if arena_fd is not None else -1), "--so", module,
             "--ohlcv", str(source), "--input-tf", "1", "--script-tf", "1"],
            cwd=ROOT, env=clean, pass_fds=(() if arena_fd is None else (arena_fd,)),
            capture_output=True, timeout=60)
    return result.returncode, result.stdout.decode("utf-8"), result.stderr.decode("utf-8")


def arena_backing_failure(result):
    status, text, stderr = result
    document = json.loads(text)
    observed = (f"observed code={document.get('code')!r}, args={document.get('args')!r}; "
                f"exit={status}, stdout={text!r}, stderr={stderr!r}")
    assert status == 1 and stderr == "", observed
    assert text.endswith("\n") and len(text.splitlines()) == 1, observed
    assert (document.get("code"), document.get("args")) == (
        "report_sink_failed", {"reason": "arena_backing"}), observed
    assert set(document) == {"engine", "error", "code", "args", "reason"}, observed
    assert document["engine"] == "pineforge", observed
    assert document["reason"] == document["args"]["reason"], observed
    assert document["error"] and document["error"] != (
        "the fresh strategy refused its report sink"), observed
    assert catalog.catalogued(document["code"], document["args"]), observed
    return document


def unjailed_success_document(result):
    status, text, stderr = result
    assert status == 0 and stderr == "", result
    assert text.endswith("\n") and len(text.splitlines()) == 1, result
    document = json.loads(text)
    assert document["engine"] == "pineforge"
    assert {"summary", "trades", "equity_curve"} <= document.keys(), document
    assert not {"error", "code", "args", "report_sink"} & document.keys(), document
    assert isinstance(document["trades"], list) and isinstance(document["equity_curve"], list)
    return document


@pytest.mark.parametrize("arena_fd,arena_bytes,jailed", [
    pytest.param("4", "1048576", None, id="bad-fd-number"),
    pytest.param("5", None, None, id="missing-bytes"),
    pytest.param("5", "", None, id="empty-bytes"),
    pytest.param("5", "0", None, id="zero-bytes"),
    pytest.param("5", "-1", None, id="negative-bytes"),
    pytest.param("5", "invalid", None, id="nondecimal-bytes"),
    pytest.param("5", "18446744073709551616", None, id="overflow-bytes"),
    pytest.param("5", "1048576", "invalid", id="invalid-jailed"),
])
def test_native_unjailed_sink_arena_environment_failure(tmp_path, arena_fd, arena_bytes, jailed):
    environment = {"PF_SCRATCH_ARENA_FD": arena_fd}
    if arena_bytes is not None:
        environment["PF_SCRATCH_ARENA_BYTES"] = arena_bytes
    if jailed is not None:
        environment["PF_SCRATCH_ARENA_JAILED"] = jailed
    arena_backing_failure(unjailed_native_run(fd.bars.csv_file(tmp_path), environment))


def incompletely_sealed_arena_fd(seals):
    flags = 1 | 2  # MFD_CLOEXEC | MFD_ALLOW_SEALING
    if hasattr(os, "memfd_create"):
        descriptor = os.memfd_create("pineforge-sink-arena", flags)
    else:
        # The lab's Python may omit the wrapper even though libc provides it.
        libc = ctypes.CDLL(None, use_errno=True)
        descriptor = libc.memfd_create(b"pineforge-sink-arena", flags)
        if descriptor < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), "memfd_create")
    try:
        os.ftruncate(descriptor, 1048576)
        if seals:
            fcntl.fcntl(descriptor, 1033, seals)  # F_ADD_SEALS
        assert fcntl.fcntl(descriptor, 1034) == seals  # F_GET_SEALS
        assert stat.S_ISREG(os.fstat(descriptor).st_mode)
        assert os.fstat(descriptor).st_size == 1048576
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


@pytest.mark.parametrize("backing", [
    "pipe", "closed",
    pytest.param("unsealed-memfd", marks=pytest.mark.skipif(
        sys.platform != "linux", reason="memfd sealing requires Linux")),
    pytest.param("partially-sealed-memfd", marks=pytest.mark.skipif(
        sys.platform != "linux", reason="memfd sealing requires Linux")),
])
def test_native_unjailed_sink_arena_backing_failure(tmp_path, backing):
    with contextlib.ExitStack() as resources:
        descriptor = None
        if backing == "pipe":
            descriptor, writer = os.pipe()
            resources.callback(os.close, descriptor)
            resources.callback(os.close, writer)
            assert stat.S_ISFIFO(os.fstat(descriptor).st_mode)
        elif backing != "closed":
            seals = 0 if backing == "unsealed-memfd" else 0x0004  # F_SEAL_GROW only
            descriptor = incompletely_sealed_arena_fd(seals)
            resources.callback(os.close, descriptor)
        result = unjailed_native_run(
            fd.bars.csv_file(tmp_path),
            {"PF_SCRATCH_ARENA_FD": "5", "PF_SCRATCH_ARENA_BYTES": "1048576"},
            arena_fd=descriptor)
    arena_backing_failure(result)


def test_native_unjailed_sink_clean_default_success(tmp_path):
    document = unjailed_success_document(unjailed_native_run(fd.bars.csv_file(tmp_path)))
    assert document["summary"]["bars_processed"] == 5
    assert all(document["diagnostics"][name] == 0 for name in ARENA_DIAGNOSTICS)


@pytest.mark.skipif(sys.platform != "linux", reason="sealed memfd backing requires Linux")
def test_native_unjailed_sink_sealed_arena_success(tmp_path):
    source = fd.bars.csv_file(tmp_path)
    reference = unjailed_success_document(unjailed_native_run(source))
    size = 16 * 1048576
    descriptor = fd.sealed_arena_fd(size)
    try:
        actual = unjailed_success_document(unjailed_native_run(
            source, {"PF_SCRATCH_ARENA_FD": "5", "PF_SCRATCH_ARENA_BYTES": str(size)},
            arena_fd=descriptor))
    finally:
        os.close(descriptor)
    assert actual["diagnostics"]["scratch_arena_bytes"] == size
    assert 0 <= actual["diagnostics"]["scratch_arena_high_water_bytes"] <= size
    for name in ARENA_DIAGNOSTICS:
        assert reference["diagnostics"].pop(name) == 0
        actual["diagnostics"].pop(name)
    assert actual == reference


@pytest.mark.parametrize("record", ["no-record", "no-getter-text", "empty-getter-text"])
def test_unjailed_sink_without_typed_record_keeps_invariant(tmp_path, record):
    library, paths = fd.FdLibrary(), []

    def refuse(state, ref):
        paths.append(Path(os.fsdecode(ref._obj.path)))
        assert paths[-1].is_file()
        return -1

    library.strategy_set_report_sink_v1 = refuse
    library.strategy_get_last_error = lambda state: (
        b"" if record == "no-record" else b"untyped native refusal")
    library.strategy_get_last_error_code = lambda state: b""
    library.strategy_last_run_status = lambda state: int(record == "empty-getter-text")
    if record == "no-getter-text":
        del library.strategy_get_last_error_code
    with mock.patch.dict(os.environ, unjailed_environment(), clear=True):
        result = fd.invoke(fd.bars.csv_file(tmp_path), library, ["--spill-dir", str(tmp_path)])
    assert result == (1, '{"engine":"pineforge","error":"the fresh strategy refused its report sink",'
                         '"code":"engine_invariant","args":{}}\n', ""), result
    assert library.calls.count("strategy_free") == 1
    assert "run_backtest_full" not in library.calls
    assert len(paths) == 1 and not paths[0].exists()


def test_unjailed_sink_failure_is_copied_before_free(tmp_path):
    library, paths, events = fd.FdLibrary(), [], []
    record = {"text": "native arena attachment refused", "code": "report_sink_failed",
              "args": {"reason": "arena_backing"}}

    def refuse(state, ref):
        paths.append(Path(os.fsdecode(ref._obj.path)))
        assert paths[-1].is_file()
        events.append("setter")
        return -1

    def read(name):
        assert "free" not in events, "failure getter read after strategy_free"
        events.append(name)
        value = record[name]
        return (json.dumps(value) if name == "args" else value).encode()

    def free(state):
        events.append("free")
        record.clear()

    library.strategy_set_report_sink_v1 = refuse
    library.strategy_get_last_error = lambda state: read("text")
    library.strategy_get_last_error_code = lambda state: read("code")
    library.strategy_get_last_error_args = lambda state: read("args")
    library.strategy_free = library.export("strategy_free", free)
    with mock.patch.dict(os.environ, unjailed_environment(), clear=True):
        result = fd.invoke(fd.bars.csv_file(tmp_path), library, ["--spill-dir", str(tmp_path)])
    document = arena_backing_failure(result)
    assert document["error"] == "native arena attachment refused"
    assert not record and events[0] == "setter" and events[-1] == "free"
    assert all(name in events[:-1] for name in ("text", "code", "args"))
    assert library.calls.count("strategy_free") == 1
    assert "run_backtest_full" not in library.calls
    assert len(paths) == 1 and not paths[0].exists()
