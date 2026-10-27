"""Real jailed-host trace request, using CTest's existing native strategy module."""
import contextlib
import ctypes
import fcntl
import io
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_sink_fd as fd_sinks

EXPECTED = (
    '{"engine":"pineforge","error":"error: bounded-memory execution refuses trace recording; '
    'use an unjailed debug run","code":"run_request_invalid","args":{"option":"arguments"}}\n'
)


@pytest.mark.parametrize("selector", ["flag", "environment"])
@pytest.mark.parametrize("transport", ["array", "fd"])
def test_native_jailed_trace_request_is_refused_before_execution(
        tmp_path, monkeypatch, selector, transport):
    artifact = Path(os.environ["PINEFORGE_TRACE_STRATEGY_SO"])
    assert artifact.is_file(), artifact
    monkeypatch.delenv("PINEFORGE_RUN_JSON_PROFILE", raising=False)
    monkeypatch.delenv("PF_OUTPUTS_FIXTURE_UNDECLARED", raising=False)
    library = run_json.load_strategy(artifact)
    library.strategy_set_trace_enabled.argtypes = [ctypes.c_void_p, ctypes.c_int]
    library.strategy_set_trace_enabled.restype = None
    created, freed, reached = [], [], []
    native_create, native_free = library.strategy_create, library.strategy_free

    def create(params):
        state = native_create(params)
        assert state
        created.append(state)
        library.strategy_set_trace_enabled(state, 1)  # the real C ABI trace request
        return state

    def free(state):
        freed.append(state)
        native_free(state)

    monkeypatch.setattr(library, "strategy_create", create)
    monkeypatch.setattr(library, "strategy_free", free)
    # Observe actual native calls, without synthesizing a refusal or a report.
    for name in ("strategy_require_report_scratch_arena_v1",
                 "strategy_set_report_sink_fd_v1", "run_backtest_full"):
        native = getattr(library, name)
        def observe(*args, native=native, name=name):
            reached.append(name)
            return native(*args)
        monkeypatch.setattr(library, name, observe)
    monkeypatch.setattr(run_json, "load_strategy", lambda path: library)
    source = tmp_path / "chart.csv"
    source.write_text(
        "timestamp,open,high,low,close,volume\n"
        "1704067200000,100,110,95,102,50\n"
        "1704067260000,102,112,99,108,50\n", encoding="utf-8")
    options = ["--so", str(artifact), "--ohlcv", str(source)]
    if selector == "flag":
        options += ["--profile", "jailed"]
    else:
        monkeypatch.setenv("PINEFORGE_RUN_JSON_PROFILE", "jailed")
    output, errors = io.StringIO(), io.StringIO()
    with contextlib.ExitStack() as resources:
        stream = None
        if transport == "fd":
            stream = fd_sinks.Stream()
            original = stream.writer
            stream.writer = fcntl.fcntl(original, fcntl.F_DUPFD_CLOEXEC, 17)
            os.close(original)
            resources.enter_context(stream)
            options += ["--report-sink-fd", str(stream.writer)]
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            status = run_json.main(options)
        if stream:
            os.fstat(stream.writer)  # still caller-owned: no sink handoff took place
    assert (status, output.getvalue(), errors.getvalue()) == (1, EXPECTED, "")
    assert len(created) == 1 and created == freed
    assert reached == [], "refusal must precede arena admission, sink installation and callbacks"
    if stream:
        assert stream.data == b""
