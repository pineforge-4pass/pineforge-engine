"""Native fd 1 isolation checks for the JSON host."""

from __future__ import annotations

from difflib import unified_diff
import io
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json
from test_run_json_bin import csv_file


MODULE = os.environ["PINEFORGE_STDOUT_STRATEGY_SO"]
MARKER = b"NATIVE_STDOUT_MARKER\n"


def _capture(argv, *, mode=None):
    output_read, output_write = os.pipe()
    error_read, error_write = os.pipe()
    saved_output = os.dup(1)
    saved_error = os.dup(2)
    status = None
    caught = None
    try:
        os.dup2(output_write, 1)
        os.close(output_write)
        os.dup2(error_write, 2)
        os.close(error_write)
        # Match a real host process: Python stderr and native fd 2 share the
        # diagnostic destination, even while pytest replaces sys.stderr.
        with os.fdopen(os.dup(2), "w", encoding="utf-8") as diagnostics, \
                pytest.MonkeyPatch.context() as patch:
            patch.setattr(sys, "stderr", diagnostics)
            patch.setattr(run_json.time, "time", lambda: 1000.0)
            # Control native printing without changing reported strategy inputs.
            patch.setenv("PINEFORGE_STDOUT_TEST_MODE", mode or "")
            try:
                status = run_json._host_main(argv)
            except BaseException as error:
                caught = error
            diagnostics.flush()
    finally:
        os.dup2(saved_output, 1)
        os.close(saved_output)
        os.dup2(saved_error, 2)
        os.close(saved_error)
    try:
        output = os.read(output_read, 4 * 1024 * 1024)
        errors = os.read(error_read, 4 * 1024 * 1024)
    finally:
        os.close(output_read)
        os.close(error_read)
    if caught is not None:
        raise caught
    return status, output, errors


def _argv(source, extra=()):
    argv = [
        "--so", str(MODULE),
        "--ohlcv", str(source),
        "--input-tf", "1",
        "--script-tf", "1",
    ]
    argv += list(extra)
    return argv


def _invoke(tmp_path, mode=None, *, extra=()):
    source = csv_file(tmp_path, name="chart.csv")
    return _capture(_argv(source, extra), mode=mode)


def _invoke_fd(tmp_path, mode=None):
    sink_read, sink_write = os.pipe()
    try:
        result = _capture(_argv(
            csv_file(tmp_path, name="chart.csv"),
            extra=("--report-sink-fd", str(sink_write), "--run-id", "fd-test")),
            mode=mode)
    finally:
        try:
            os.close(sink_write)
        except OSError:
            pass
    try:
        stream = os.read(sink_read, 4 * 1024 * 1024)
    finally:
        os.close(sink_read)
    return result, stream


def _document_difference(clean, printed):
    offset = next((index for index, (left, right) in enumerate(zip(clean, printed))
                   if left != right), min(len(clean), len(printed)))
    start, end = max(0, offset - 80), offset + 120
    context = (
        f"first differing byte={offset}; clean bytes={len(clean)}; "
        f"printed bytes={len(printed)}\n"
        f"clean[{start}:{end}]={clean[start:end]!r}\n"
        f"printed[{start}:{end}]={printed[start:end]!r}"
    )
    try:
        clean_document = json.loads(clean)
        printed_document = json.loads(printed)
    except ValueError as error:
        return f"{context}\nJSON decode error: {error}\nclean={clean!r}\nprinted={printed!r}"
    return context + "\n" + "\n".join(unified_diff(
        json.dumps(clean_document, indent=2, sort_keys=True).splitlines(),
        json.dumps(printed_document, indent=2, sort_keys=True).splitlines(),
        fromfile="clean", tofile="printed", lineterm=""))


def test_native_fd1_output_is_diagnostic_only(tmp_path):
    clean_status, clean_stdout, clean_stderr = _invoke(tmp_path)
    printed_status, printed_stdout, printed_stderr = _invoke(tmp_path, "print")
    assert clean_status == printed_status == 0, (
        clean_status, printed_status, clean_stderr, printed_stderr)
    assert clean_stderr == b"", clean_stderr
    assert printed_stdout == clean_stdout, _document_difference(clean_stdout, printed_stdout)
    assert MARKER not in clean_stdout, clean_stdout
    assert MARKER in printed_stderr, printed_stderr


def test_native_fd1_output_does_not_join_a_typed_failure(tmp_path):
    status, stdout, stderr = _invoke(tmp_path, "print-fail")
    assert status == 1
    assert stdout.count(b"\n") == 1
    assert json.loads(stdout)["code"] == "strategy_runtime_error"
    assert MARKER not in stdout
    assert MARKER in stderr


def test_fd_sink_stream_is_unchanged_by_native_fd1_output(tmp_path):
    clean, clean_stream = _invoke_fd(tmp_path)
    printed, printed_stream = _invoke_fd(tmp_path, "print")
    assert clean[0] == printed[0] == 0
    assert clean_stream == printed_stream
    assert MARKER not in printed[1]
    assert MARKER in printed[2]


@pytest.mark.parametrize(("field", "value"), [
    ("report_sink_fd", 71),
    ("run_phase_fd", "71"),
    ("arena", 5),
])
def test_reserved_report_descriptor_collisions_are_typed(monkeypatch, field, value):
    # os.dup returns an int; argparse leaves --run-phase-fd as text.
    owner = SimpleNamespace(fd=int(value))
    monkeypatch.setattr(run_json, "_REPORT_OUTPUT", owner)
    args = SimpleNamespace(
        profile="unjailed", report_sink=None, spill_dir=None,
        report_sink_fd=value if field == "report_sink_fd" else None,
        run_phase_fd=value if field == "run_phase_fd" else None,
    )
    operands = (
        f"owner.fd={owner.fd!r} ({type(owner.fd).__name__}); "
        f"{field}={value!r} ({type(value).__name__})"
    )
    try:
        run_json._refuse_jailed_path_sink(args)
    except run_json.RunFailure as failure:
        observed = f"{operands}; code={failure.code!r}; exit_status={failure.exit_status!r}"
        assert failure.code == "run_request_invalid", observed
        assert failure.exit_status == 2, observed
    else:
        pytest.fail(f"expected RunFailure for colliding descriptors; {operands}")


def test_reserved_report_descriptor_rejects_a_shared_open_file(monkeypatch):
    first, second = os.pipe()
    alias = os.dup(second)
    try:
        monkeypatch.setattr(run_json, "_REPORT_OUTPUT", SimpleNamespace(fd=second))
        args = SimpleNamespace(
            profile="unjailed", report_sink=None, spill_dir=None,
            report_sink_fd=alias, run_phase_fd=None)
        with pytest.raises(run_json.RunFailure) as caught:
            run_json._refuse_jailed_path_sink(args)
        assert caught.value.code == "run_request_invalid"
        assert caught.value.exit_status == 2
    finally:
        os.close(first)
        os.close(second)
        os.close(alias)


def test_late_fault_after_publication_has_no_second_document(monkeypatch):
    expected = '{"engine":"pineforge","published":true}\n'

    def publish_then_fault(_):
        assert run_json._write_report(expected) == 0
        raise RuntimeError("late fault")

    monkeypatch.setattr(run_json, "_REPORT_MAIN", publish_then_fault)
    status, stdout, stderr = _capture([])
    observed = f"status={status!r}; stdout={stdout!r}; stderr={stderr!r}"
    assert status == 1, observed
    assert stdout == expected.encode("ascii"), observed
    assert stdout.count(b"\n") == 1, observed
    assert b"late fault" in stderr, observed


class _LibraryStdout(io.StringIO):
    def __init__(self):
        super().__init__()
        self.flushes = 0

    def flush(self):
        self.flushes += 1
        return super().flush()


def _forbid_report_reservation():
    pytest.fail("a library call must not reserve or redirect stdout")


def test_library_main_uses_replaced_stdout_on_each_run(tmp_path, monkeypatch):
    source = csv_file(tmp_path, name="chart.csv")
    monkeypatch.setattr(run_json, "_ReportOutput", _forbid_report_reservation)
    monkeypatch.setattr(run_json.time, "time", lambda: 1000.0)
    monkeypatch.setenv("PINEFORGE_STDOUT_TEST_MODE", "")
    outputs = []
    for _ in range(2):
        stdout, stderr = _LibraryStdout(), io.StringIO()
        with monkeypatch.context() as patch:
            patch.setattr(sys, "stdout", stdout)
            patch.setattr(sys, "stderr", stderr)
            status = run_json.main(_argv(source))
        text = stdout.getvalue()
        observed = f"status={status!r}; stdout={text!r}; stderr={stderr.getvalue()!r}"
        assert status == 0, observed
        assert stderr.getvalue() == "", observed
        assert stdout.flushes == 1, observed
        assert text.startswith('{"engine":"pineforge","input":'), observed
        assert text.count("\n") == 1 and text.endswith("\n"), observed
        document = json.loads(text)
        # The fixture consumes all five bars and emits no trades.
        assert document["input"]["ohlcv"] == str(source), observed
        assert document["input"]["bars"] == document["summary"]["bars_processed"] == 5, observed
        assert document["summary"]["total_trades"] == 0, observed
        outputs.append(text.encode("utf-8"))
    assert outputs[0] == outputs[1], _document_difference(*outputs)


@pytest.mark.parametrize("kind", ["text", "prepared", "failure"])
def test_library_writers_use_current_stdout_without_an_owner(monkeypatch, kind):
    monkeypatch.setattr(run_json, "_ReportOutput", _forbid_report_reservation)
    report = '{"engine":"pineforge","library":true}\n'
    expected = ('{"engine":"pineforge","error":"library failure"}\n'
                if kind == "failure" else report)
    for _ in range(2):
        stdout = _LibraryStdout()
        with monkeypatch.context() as patch:
            patch.setattr(sys, "stdout", stdout)
            if kind == "failure":
                run_json.write_failure("library failure")
            elif kind == "prepared":
                text = io.StringIO(report)
                assert run_json._write_report(text) == 0
                assert text.closed
            else:
                assert run_json._write_report(report) == 0
        assert stdout.getvalue() == expected, stdout.getvalue()
        assert stdout.flushes == 1, stdout.flushes


@pytest.mark.parametrize("publication", ["report", "failure"])
@pytest.mark.parametrize("site", ["handler", "resource_cleanup", "signal_cleanup"])
def test_library_late_fault_preserves_one_document_per_run(monkeypatch, publication, site):
    report = '{"engine":"pineforge","published":true}\n'
    expected = ('{"engine":"pineforge","error":"initial failure"}\n'
                if publication == "failure" else report)

    def fault():
        raise RuntimeError("late library fault")

    def publish_then_fault(argv, resources):
        if publication == "failure":
            run_json.write_failure("initial failure")
        else:
            assert run_json._write_report(report) == 0
        if site == "handler":
            fault()
        if site == "resource_cleanup":
            resources.callback(fault)
        return 0

    if site == "signal_cleanup":
        close = run_json._SigtermAbort.close

        def close_then_fault(abort):
            close(abort)
            fault()

        monkeypatch.setattr(run_json._SigtermAbort, "close", close_then_fault)
    monkeypatch.setattr(run_json, "_ReportOutput", _forbid_report_reservation)
    monkeypatch.setattr(run_json, "_main", publish_then_fault)
    for _ in range(2):
        stdout, stderr = _LibraryStdout(), io.StringIO()
        with monkeypatch.context() as patch:
            patch.setattr(sys, "stdout", stdout)
            patch.setattr(sys, "stderr", stderr)
            status = run_json.main([])
        observed = (f"status={status!r}; stdout={stdout.getvalue()!r}; "
                    f"stderr={stderr.getvalue()!r}")
        assert status == 1, observed
        assert stdout.getvalue() == expected, observed
        assert stdout.flushes == 1, observed
        assert "late library fault" in stderr.getvalue(), observed


def test_library_discard_uses_the_current_stdout_descriptor(tmp_path, monkeypatch):
    path = tmp_path / "library-stdout.txt"
    with path.open("w", encoding="utf-8") as stdout:
        stdout.write("before discard\n")
        stdout.flush()
        stdout.write("buffered after discard\n")
        with monkeypatch.context() as patch:
            patch.setattr(sys, "stdout", stdout)
            run_json._discard_stdout()
        stdout.flush()
    assert path.read_bytes() == b"before discard\n"
