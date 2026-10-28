"""Native fd 1 isolation checks for the JSON host."""

from __future__ import annotations

from difflib import unified_diff
import io
import json
import os
from pathlib import Path
import subprocess
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


@pytest.mark.parametrize("value", ["-1", "0", "1", "2", str(1 << 31), str(1 << 40), str(1 << 100)])
def test_out_of_range_report_sink_descriptor_keeps_typed_refusal(tmp_path, value):
    status, stdout, stderr = _invoke(tmp_path, extra=("--report-sink-fd", value))
    observed = f"value={value!r}; status={status!r}; stdout={stdout!r}; stderr={stderr!r}"
    assert status == 2, observed
    assert stdout == (
        b'{"engine":"pineforge","error":"error: --report-sink-fd must be a descriptor number '
        b'from 3 through INT_MAX","code":"run_request_invalid","args":{"option":"arguments"}}\n'
    ), observed
    assert stderr == b"", observed


@pytest.mark.parametrize(("field", "value"), [
    ("report_sink_fd", 71),
    ("run_phase_fd", "71"),
    ("arena", 5),
])
def test_reserved_report_descriptor_collisions_are_typed(monkeypatch, field, value):
    # os.dup returns an int; argparse leaves --run-phase-fd as text.
    # The arena case exercises only fd == owner.fd; real owners relocate off 5.
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


def test_host_merged_streams_diagnose_and_still_publish_one_report(tmp_path):
    source = csv_file(tmp_path, name="chart.csv")
    result = subprocess.run(
        [sys.executable, "-B", str(ROOT / "docker/run_json.py"), *_argv(source)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env={**os.environ, "PINEFORGE_STDOUT_TEST_MODE": "print"}, timeout=30)
    observed = f"status={result.returncode!r}; merged={result.stdout!r}"
    assert result.returncode == 0, observed
    lines = result.stdout.splitlines(keepends=True)
    assert len(lines) == 3, observed
    assert lines[0] == (
        b"report channel not isolated: stdout and stderr are the same file\n"
    ), observed
    assert lines[1] == MARKER, observed
    assert lines[2].endswith(b"\n"), observed
    document = json.loads(lines[2])
    assert document["engine"] == "pineforge", observed
    assert document["input"]["ohlcv"] == str(source), observed
    assert document["input"]["bars"] == document["summary"]["bars_processed"] == 5, observed
    assert document["summary"]["total_trades"] == 0, observed
    assert document["trades"] == [], observed


@pytest.mark.parametrize(("argv", "option"), [
    (["--help"], b"--so"),
    (["csv2bin", "--help"], b"--chart"),
])
def test_host_help_stays_on_original_stdout(argv, option):
    result = subprocess.run(
        [sys.executable, "-B", str(ROOT / "docker/run_json.py"), *argv],
        capture_output=True, timeout=30)
    observed = (f"status={result.returncode!r}; stdout={result.stdout!r}; "
                f"stderr={result.stderr!r}")
    assert result.returncode == 0, observed
    assert result.stdout.startswith(b"usage: run_json.py"), observed
    assert option in result.stdout, observed
    assert result.stderr == b"", observed


def test_host_relocates_report_descriptor_when_three_and_four_are_occupied(tmp_path):
    source = csv_file(tmp_path, name="chart.csv")
    clean_status, clean_stdout, clean_stderr = _capture(_argv(source))
    bootstrap = """
import os
import sys
sys.path.insert(0, sys.argv[1])
import run_json

for expected_fd in (3, 4):
    fd = os.open(os.devnull, os.O_RDONLY)
    assert fd == expected_fd, (expected_fd, fd)
probe = os.dup(1)
assert probe == 5, probe
os.close(probe)

original_main = run_json._REPORT_MAIN
def check_reserved_descriptor(argv):
    owner = run_json._REPORT_OUTPUT
    assert owner.fd != 5, owner.fd
    assert not os.get_inheritable(owner.fd), owner.fd
    return original_main(argv)
run_json._REPORT_MAIN = check_reserved_descriptor
run_json.time.time = lambda: 1000.0
sys.exit(run_json._host_main(sys.argv[2:]))
"""
    result = subprocess.run(
        [sys.executable, "-B", "-c", bootstrap, str(ROOT / "docker"), *_argv(source)],
        capture_output=True, close_fds=True,
        env={**os.environ, "PINEFORGE_STDOUT_TEST_MODE": ""}, timeout=30)
    observed = (f"status={result.returncode!r}; stdout={result.stdout!r}; "
                f"stderr={result.stderr!r}; clean_status={clean_status!r}; "
                f"clean_stderr={clean_stderr!r}")
    assert result.returncode == clean_status == 0, observed
    assert result.stderr == clean_stderr == b"", observed
    assert result.stdout == clean_stdout, _document_difference(clean_stdout, result.stdout)
    document = json.loads(result.stdout)
    assert document["input"]["bars"] == document["summary"]["bars_processed"] == 5, observed
    assert document["trades"] == [], observed


@pytest.mark.parametrize("literal", [False, True], ids=["escaped-json", "literal-unicode"])
def test_host_failure_channel_encodes_non_ascii_text_as_utf8(monkeypatch, literal):
    def fail(_):
        if literal:
            # failure_line escapes Unicode; inject a literal line to test the channel encoder.
            run_json._publish_failure('{"engine":"pineforge","error":"caf\u00e9"}\n')
        else:
            run_json.write_failure("caf\u00e9")
        return 1

    monkeypatch.setattr(run_json, "_REPORT_MAIN", fail)
    status, stdout, stderr = _capture([])
    expected = (b'{"engine":"pineforge","error":"caf\xc3\xa9"}\n' if literal
                else b'{"engine":"pineforge","error":"caf\\u00e9"}\n')
    observed = f"status={status!r}; stdout={stdout!r}; stderr={stderr!r}"
    assert status == 1, observed
    assert stdout == expected, observed
    assert json.loads(stdout)["error"] == "caf\u00e9", observed
    assert stderr == b"", observed


@pytest.mark.parametrize("publication", ["report", "failure"])
def test_library_second_report_does_not_discard_callers_stdout(tmp_path, monkeypatch, publication):
    report = '{"engine":"pineforge","published":true}\n'
    expected = (b'{"engine":"pineforge","error":"initial failure"}\n'
                if publication == "failure" else report.encode("ascii"))

    def publish_twice(argv, resources):
        if publication == "failure":
            run_json.write_failure("initial failure")
        else:
            assert run_json._write_report(report) == 0
        return run_json._write_report('{"engine":"pineforge","second":true}\n')

    monkeypatch.setattr(run_json, "_main", publish_twice)
    path = tmp_path / "library-stdout.txt"
    stderr = io.StringIO()
    with path.open("w", encoding="utf-8") as stdout:
        with monkeypatch.context() as patch:
            patch.setattr(sys, "stdout", stdout)
            patch.setattr(sys, "stderr", stderr)
            status = run_json.main([])
        # A real descriptor makes an accidental dup2-to-null observable to the caller.
        os.write(stdout.fileno(), b"caller still writable\n")
    observed = (f"status={status!r}; stdout={path.read_bytes()!r}; "
                f"stderr={stderr.getvalue()!r}")
    assert status == 1, observed
    assert path.read_bytes() == expected + b"caller still writable\n", observed
    assert stderr.getvalue() == (
        "run_json: the report could not be written: "
        "the report channel has already been published\n"
    ), observed
