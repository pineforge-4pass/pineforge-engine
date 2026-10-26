"""CX-SINK-B5: fd-only run_json versus its unchanged file-based producer.

Unit rows replace native calls, using the existing L9 trade/output fixtures.
Native rows use CTest's real modules. The external child entry at the bottom
lets subprocess rows apply RLIMIT_FSIZE without an inline Python program.
No executor-side execution or verification is claimed.
"""
from __future__ import annotations

import argparse
import builtins
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import io
import json
import math
import os
from pathlib import Path
import resource
import signal
import socket
import subprocess
import sys
import threading
import types
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_bin as bars
import test_run_json_sink as full
import test_run_json_sink_summary as summaries
import test_run_json_selected_sink as selected_sinks
import test_run_json_selected_window as selected


RUN_ID = 'fd "run"\n漢'
PNLS = (10., -7., -0.)


class FdLibrary(summaries.SummaryLibrary):
    """Native-call double; rows still come from L9's actual ReportC fixture.

    The fd summary API is emulated from those produced T rows. Expected JSON
    comes from run_json's independent file reader/fold, never this emulation.
    """

    def __init__(self, *, fd_fault=None, **kwargs):
        super().__init__(**kwargs)
        self.fd, self.fd_fault, self.error = None, fd_fault, None
        self.trade_rows, self.summary_queries = [], 0
        self.strategy_set_report_sink_fd_v1 = self.export("set_fd", self.install_fd)
        self.strategy_report_sink_summary_v1 = self.export("summary", self.summary_receipt)
        self.strategy_report_sink_diagnostics_v1 = self.export("diagnostics", self.diagnostics_receipt)
        self.strategy_get_last_error = self.export("last_error", lambda state: (
            self.error[0].encode() if self.error else b""))
        self.strategy_get_last_error_code = self.export("last_code", lambda state: (
            self.error[1].encode() if self.error else b""))
        self.strategy_get_last_error_args = self.export("last_args", lambda state: b"{}")
        self.strategy_outputs_series_count = self.export("slots", lambda state: 2)
        self.strategy_outputs_constants_copy = self.export("constants", self.constants)

    def install_fd(self, state, ref):
        descriptor = ref._obj
        assert descriptor.struct_size == ctypes.sizeof(run_json.ReportSinkFdC)
        assert descriptor.version == 1
        self.fd, self.run_id = descriptor.fd, descriptor.run_id.decode()
        self.installed.append(self.fd)
        # Only tells L9's run fixture to suppress arrays. No path operation
        # ever consumes this marker; write_sink/free are overridden below.
        self.path = "<inherited-stream>"
        if self.fd_fault == "setter":
            return -1
        if self.fd_fault == "setter-exception":
            raise RuntimeError("injected fd setter failure")
        return 0

    def run(self, *args):
        try:
            with mock.patch.object(full, "write_sink", self.write_sink):
                full.SinkLibrary.run(self, *args)
            args[-1]._obj.metrics.all.num_trades = len(self.pnls)
            if self.fd_fault == "metric-count":
                args[-1]._obj.metrics.all.num_trades += 1
            if self.fd_fault == "report-count":
                args[-1]._obj.total_trades += 1
        except OSError as error:
            # The engine's existing writer boundary turns EPIPE/open/write
            # failures into this catalog code (whose args are empty here).
            self.error = (str(error), "report_sink_failed")

    def write_sink(self, path, rows, *, outputs=False, fault=None):
        rows = list(rows)
        self.trade_rows = [row[1:] for row in rows if row[0] == "T"]
        self.equity_rows = [row[1:] for row in rows if row[0] == "E"]
        if self.fd is None:
            return super().write_sink(path, rows, outputs=outputs, fault=fault)
        header = full.header(outputs)
        header["run_id"] = self.run_id
        digest = hashlib.sha256()
        counts = {record["type"]: 0 for record in header["records"]}

        def send(line):
            remaining = memoryview(line)
            while remaining:
                written = os.write(self.fd, remaining)
                if written <= 0:
                    raise OSError(errno.EIO, "injected sink made no progress")
                remaining = remaining[written:]

        line = full.encoded(header)
        send(line)
        digest.update(line)
        if self.fd_fault == "typed":
            raise run_json.RunFailure("injected run refusal", "report_sink_failed")
        if self.fd_fault == "exception":
            raise RuntimeError("injected execution exception")
        if self.fd_fault == "already-closed":
            os.close(self.fd)
            raise run_json.RunFailure("injected closed descriptor", "report_sink_failed")
        if self.fd_fault == "engine":
            self.error = ("injected native failure", "report_sink_failed")
            return counts, ""
        for row in rows:
            line = full.encoded(row)
            send(line)
            digest.update(line)
            counts[row[0]] += 1
        trailer = {"trailer": "pf-report-sink", "version": 1,
                   "counts": counts, "sha256": digest.hexdigest()}
        send(full.encoded(trailer))
        return counts, digest.hexdigest()

    def summary_receipt(self, state, ref):
        self.summary_queries += 1
        result = ref._obj
        assert result.struct_size == ctypes.sizeof(run_json.ReportSinkSummaryC) == 64
        assert result.version == 1
        pnls = [float("nan") if row[7] is None else row[7] for row in self.trade_rows]
        result.count_t = len(pnls)
        result.wins = sum(pnl > 0 for pnl in pnls)
        result.losses = sum(pnl < 0 for pnl in pnls)
        result.best_trade = max(pnls) if pnls else 0.
        result.worst_trade = min(pnls) if pnls else 0.
        prefixes = [0.]
        for pnl in pnls:
            prefixes.append(prefixes[-1] + pnl)
        result.max_drawdown = min(
            (value - max(prefixes[:index + 1]) for index, value in enumerate(prefixes)),
            default=0.)
        invalid = next((index + 1 for row in self.trade_rows for index in range(4, 12)
                        if row[index] is None or not math.isfinite(row[index])), 0)
        result.nonfinite_t_field = invalid
        result.complete = 1
        if self.fd_fault == "summary-incomplete":
            result.complete = 0
        elif self.fd_fault == "summary-count":
            result.count_t += 1
        elif self.fd_fault == "summary-status":
            return -1
        elif self.fd_fault == "unknown-field":
            result.nonfinite_t_field = 99
        elif self.fd_fault == "summary-overflow":
            result.max_drawdown = float("-inf")
        return 0

    def receipt(self, state, ref):
        status = super().receipt(state, ref)
        if self.fd_fault == "receipt-incomplete":
            ref._obj.complete = 0
        elif self.fd_fault == "receipt-count":
            ref._obj.count_t += 1
        elif self.fd_fault == "negative-count":
            ref._obj.count_e = -1
        elif self.fd_fault == "receipt-status":
            return -1
        return status

    def diagnostics_receipt(self, state, ref):
        result = ref._obj
        assert result.struct_size == ctypes.sizeof(run_json.ReportSinkDiagnosticsC) == 72
        assert result.version == 1
        result.count_e = len(self.equity_rows)
        if self.equity_rows:
            result.first = run_json.EquityPointC(*self.equity_rows[0])
            result.last = run_json.EquityPointC(*self.equity_rows[-1])
        result.trade_at_start = any(row[12] <= 0 for row in self.trade_rows)
        result.complete = 1
        if self.fd_fault == "diagnostics-incomplete":
            result.complete = 0
        elif self.fd_fault == "diagnostics-count":
            result.count_e += 1
        elif self.fd_fault == "diagnostics-status":
            return -1
        return 0

    def constants(self, state, destination, capacity):
        values = full.outputs_reader().constants()
        for index in range(min(capacity, len(values))):
            destination[index] = values[index]
        return len(values)

    def free(self, state):
        if self.fd is None:
            return super().free(state)
        # The runner must retain the endpoint until the library is freed.
        if self.fd_fault not in ("already-closed", "setter", "setter-exception"):
            os.fstat(self.fd)
        self.freed.append(self.fd)


class SelectedFdLibrary(selected_sinks.NamedSinkLibrary):
    """Keep L9S's real observer callbacks, planner counts and presented rows."""

    def __init__(self, plan, *, fd_fault=None, **kwargs):
        super().__init__(plan, **kwargs)
        self.fd, self.fd_fault, self.error = None, fd_fault, None
        self.trade_rows, self.summary_queries, self.all_tags = [], 0, False
        self.strategy_set_report_sink_fd_v1 = self.install_fd
        self.strategy_report_sink_summary_v1 = self.summary_receipt
        self.strategy_report_sink_diagnostics_v1 = self.diagnostics_receipt
        self.strategy_outputs_series_count = lambda state: 2
        self.strategy_outputs_constants_copy = self.constants
        self.strategy_get_last_error = lambda state: self.error[0].encode() if self.error else b""
        self.strategy_get_last_error_code = lambda state: self.error[1].encode() if self.error else b""
        self.strategy_get_last_error_args = lambda state: b"{}"
        report_free, strategy_free = self.report_free, self.strategy_free

        def free_report(ref):
            if self.fd is None:
                report_free(ref)
            else:
                self.calls.append(("report_free",))

        def free_state(state):
            if self.fd is None:
                strategy_free(state)
            else:
                os.fstat(self.fd)
                self.closed.append(self.fd)
                self.calls.append(("strategy_free", state))

        self.report_free, self.strategy_free = free_report, free_state

    install_fd = FdLibrary.install_fd
    summary_receipt = FdLibrary.summary_receipt
    diagnostics_receipt = FdLibrary.diagnostics_receipt
    constants = FdLibrary.constants

    def write_sink(self, path, rows, **kwargs):
        if self.fd is None:
            return summaries.SummaryLibrary.write_sink(self, path, rows, **kwargs)
        return FdLibrary.write_sink(self, path, rows, **kwargs)

    def _run(self, *args):
        with mock.patch.object(full, "write_sink", self.write_sink):
            super()._run(*args)
        args[-1]._obj.metrics.all.num_trades = len(self.pnls)


@contextlib.contextmanager
def forbid_report_files():
    """Fail on every tempfile/read-back helper and every write-open in fd mode."""
    real_open, real_os_open = builtins.open, os.open

    def forbidden(*args, **kwargs):
        raise AssertionError("fd mode attempted a report tempfile or sink read-back")

    def read_only_open(file, mode="r", *args, **kwargs):
        assert not any(letter in mode for letter in "wax+"), (file, mode)
        return real_open(file, mode, *args, **kwargs)

    def read_only_os_open(path, flags, *args, **kwargs):
        assert not flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC), (path, flags)
        return real_os_open(path, flags, *args, **kwargs)

    with contextlib.ExitStack() as stack:
        for name in ("gettempdir", "mkstemp", "TemporaryFile", "NamedTemporaryFile",
                     "TemporaryDirectory", "mkdtemp"):
            stack.enter_context(mock.patch.object(run_json.tempfile, name, forbidden))
        for name in ("_ReportSink", "_TemporaryReportSink", "_json_temporary",
                     "_prepare_stream_report"):
            stack.enter_context(mock.patch.object(run_json, name, forbidden))
        # The serializer uses this class as an isinstance target on every
        # value. Forbid construction without replacing the type itself.
        stack.enter_context(mock.patch.object(run_json._JSONColumn, "__init__", forbidden))
        stack.enter_context(mock.patch.object(pf_sink, "SinkReader", forbidden))
        stack.enter_context(mock.patch.object(builtins, "open", read_only_open))
        stack.enter_context(mock.patch.object(io, "open", read_only_open))
        stack.enter_context(mock.patch.object(os, "open", read_only_os_open))
        yield


@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
def test_fd_file_guard_allows_serialization_and_refuses_columns(profile):
    rows = [{"value": -0.0}, {"value": 'quote"\n漢'}]
    document = {"rows": run_json._JSONRecordArray(lambda: iter(rows), len(rows))}
    args = argparse.Namespace(profile=profile, spill_dir=None)
    with forbid_report_files(), contextlib.ExitStack() as resources:
        buffer = run_json._prepare_report_json(
            document, resources, args, types.SimpleNamespace(fd_mode=True))
        assert buffer.read() == json.dumps(
            {"rows": rows}, separators=(",", ":"), allow_nan=False) + "\n"
        with pytest.raises(AssertionError, match="report tempfile or sink read-back"):
            run_json._JSONColumn(resources, None)
    assert buffer.closed


class Stream:
    """Caller-selected endpoint drained by one reader; runner owns the writer."""

    def __init__(self, transport="pipe", *, break_after=None):
        if transport == "pipe":
            self.reader, self.writer = os.pipe()
        else:
            reader, writer = socket.socketpair()
            writer.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
            self.reader, self.writer = reader.detach(), writer.detach()
        self.writer_closed = False
        self.chunks, self.errors, self.break_after = [], [], break_after
        self.thread = threading.Thread(target=self.drain)

    def drain(self):
        try:
            while chunk := os.read(self.reader, 4096):
                self.chunks.append(chunk)
                if self.break_after is not None and sum(map(len, self.chunks)) >= self.break_after:
                    break
        except BaseException as error:
            self.errors.append(error)
        finally:
            os.close(self.reader)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        # On a pre-handoff usage/library refusal the caller still owns it.
        self.close_writer()
        self.thread.join()
        assert not self.errors, self.errors

    def close_writer(self):
        if self.writer_closed:
            return
        self.writer_closed = True
        try:
            os.close(self.writer)
        except OSError as error:
            if error.errno != errno.EBADF:
                raise

    @property
    def data(self):
        return b"".join(self.chunks)


def invoke(source, library, extra=(), *, no_files=False, stdout=None):
    output, errors = stdout if stdout is not None else io.StringIO(), io.StringIO()
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.object(run_json, "load_strategy", lambda path: library))
        stack.enter_context(mock.patch.object(run_json.time, "time", lambda: 10.))
        stack.enter_context(mock.patch.object(run_json, "require_outputs", lambda lib: None))
        stack.enter_context(mock.patch.object(run_json, "enable_outputs", lambda lib, state: None))
        stack.enter_context(mock.patch.object(
            run_json, "_outputs_manifest", lambda lib, state: full.outputs_reader().manifest()))
        stack.enter_context(mock.patch.object(run_json, "make_outputs_reader", lambda *args: (
            full.outputs_reader() if not no_files else pytest.fail("retained outputs read"))))
        if no_files:
            stack.enter_context(forbid_report_files())
        stack.enter_context(contextlib.redirect_stdout(output))
        stack.enter_context(contextlib.redirect_stderr(errors))
        try:
            status = run_json.main(["--so", "unused.so", "--ohlcv", str(source), *extra])
        except SystemExit as error:
            status = error.code
    return status, output.getvalue(), errors.getvalue()


def projection(text):
    result = json.loads(text)
    result.pop("trades", None)
    result.pop("equity_curve", None)
    result.pop("report_sink", None)
    if "outputs" in result:
        result["outputs"] = {key: result["outputs"][key] for key in summaries.OUTPUT_KEYS}
    return result


def assert_document(text, expected):
    # JSON equality alone treats -0.0 and +0.0 as equal. Require the complete
    # serialized document (with only the approved projection) to agree too.
    assert text == json.dumps(expected, separators=(",", ":"), allow_nan=False) + "\n"
    actual = json.loads(text)
    for key in ("best_trade", "worst_trade", "max_drawdown"):
        assert repr(actual["summary"][key]) == repr(expected["summary"][key])


@pytest.mark.parametrize("transport", ["pipe", "socket"])
@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("pnls", [
    PNLS, (), (-2., -3., -1.), (2., 3., 1.), (-0., 0.), (0., -0.),
    (1e16, 1., -1e16, -2.),
])
def test_fd_matches_file_document_and_wire(tmp_path, transport, outputs, pnls):
    source, sink_path = bars.csv_file(tmp_path), tmp_path / "reference.sink"
    extra = ["--outputs"] if outputs else []
    reference = invoke(source, FdLibrary(pnls=pnls, outputs=outputs),
                       [*extra, "--report-sink", str(sink_path), "--run-id", RUN_ID])
    assert reference[0] == 0 and reference[2] == "", reference
    full_reference = invoke(source, FdLibrary(pnls=pnls, outputs=outputs),
                            [*extra, "--run-id", RUN_ID])
    assert full_reference[0] == 0 and full_reference[2] == "", full_reference
    with Stream(transport) as stream:
        library = FdLibrary(pnls=pnls, outputs=outputs)
        actual = invoke(source, library,
                        [*extra, "--report-sink-fd", str(stream.writer), "--run-id", RUN_ID,
                         "--report-sink-format", "1"],
                        no_files=True)
        assert actual[0] == 0 and actual[2] == "", actual
        with pytest.raises(OSError) as closed:
            os.fstat(stream.writer)
        assert closed.value.errno == errno.EBADF
    assert stream.data == sink_path.read_bytes()
    assert_document(actual[1], projection(reference[1]))
    assert_document(actual[1], projection(full_reference[1]))
    assert library.queried == library.summary_queries == 1
    assert library.freed == library.installed == [stream.writer]
    assert "trades" not in json.loads(actual[1]) and "equity_curve" not in json.loads(actual[1])


def test_fd_annualization_matches_verified_file_endpoints(tmp_path):
    source = bars.csv_file(tmp_path)
    reference = invoke(source, FdLibrary(), [
        "--report-sink", str(tmp_path / "annualization.sink")])
    assert (reference[0], reference[2]) == (0, ""), reference
    expected = json.loads(reference[1])["diagnostics"]["annualization"]
    with Stream() as stream:
        actual = invoke(source, FdLibrary(), [
            "--report-sink-fd", str(stream.writer)], no_files=True)
    assert (actual[0], actual[2]) == (0, ""), actual
    assert json.loads(actual[1])["diagnostics"]["annualization"] == expected


@pytest.mark.parametrize("span,final,trade_at_start,open_profit,has_reason", [
    (120000, 2., True, 0., True),
    (0, 2., False, 0., False),
    (172800000, 2., False, 0., False),
    (86400000, 1e308, False, 0., True),
    (86400000, 1e308, True, 0., False),
    (86400000, 1e308, False, 1., False),
])
def test_fd_annualization_uses_presented_endpoints_not_input_span(
        tmp_path, span, final, trade_at_start, open_profit, has_reason):
    class PresentedEndpoints(FdLibrary):
        def write_sink(self, path, rows, **kwargs):
            rows = [list(row) for row in rows]
            equity = [row for row in rows if row[0] == "E"]
            assert len(equity) > 1
            for index, row in enumerate(equity):
                row[1] = bars.T0 + span * index // (len(equity) - 1)
                row[2] = 1. + (final - 1.) * (index / (len(equity) - 1))
                row[3] = open_profit if index == 0 else 0.
            for row in rows:
                if row[0] == "T" and not trade_at_start:
                    row[13] += 1
            return super().write_sink(path, rows, **kwargs)

    source = bars.csv_file(tmp_path)
    reference = invoke(source, PresentedEndpoints(), [
        "--report-sink", str(tmp_path / "presented.sink")])
    assert (reference[0], reference[2]) == (0, ""), reference
    expected = json.loads(reference[1])["diagnostics"].get("annualization")
    assert bool(expected) == has_reason
    with Stream() as stream:
        actual = invoke(source, PresentedEndpoints(), [
            "--report-sink-fd", str(stream.writer)], no_files=True)
    assert (actual[0], actual[2]) == (0, ""), actual
    assert json.loads(actual[1])["diagnostics"].get("annualization") == expected


def test_fd_layout_and_bindings(tmp_path, monkeypatch):
    library = FdLibrary()
    monkeypatch.setattr(ctypes, "CDLL", lambda path: library)
    run_json.load_strategy(tmp_path / "strategy.so")
    assert library.strategy_set_report_sink_fd_v1.restype is ctypes.c_int
    assert library.strategy_set_report_sink_fd_v1.argtypes == [
        ctypes.c_void_p, ctypes.POINTER(run_json.ReportSinkFdC)]
    assert library.strategy_report_sink_summary_v1.restype is ctypes.c_int
    assert library.strategy_report_sink_summary_v1.argtypes == [
        ctypes.c_void_p, ctypes.POINTER(run_json.ReportSinkSummaryC)]
    assert library.strategy_report_sink_diagnostics_v1.argtypes == [
        ctypes.c_void_p, ctypes.POINTER(run_json.ReportSinkDiagnosticsC)]
    assert library.strategy_report_sink_diagnostics_v1.restype is ctypes.c_int
    assert ctypes.sizeof(run_json.ReportSinkDiagnosticsC) == 72
    for name, offset in (("struct_size", 0), ("version", 4), ("count_e", 8),
                         ("first", 16), ("last", 40), ("trade_at_start", 64), ("complete", 68)):
        assert getattr(run_json.ReportSinkDiagnosticsC, name).offset == offset
    fd_type, summary_type = run_json.ReportSinkFdC, run_json.ReportSinkSummaryC
    assert fd_type.struct_size.offset == 0 and fd_type.version.offset == 4
    assert fd_type.fd.offset == 8
    assert fd_type.run_id.offset == (16 if ctypes.sizeof(ctypes.c_void_p) == 8 else 12)
    assert ctypes.sizeof(fd_type) == (24 if ctypes.sizeof(ctypes.c_void_p) == 8 else 16)
    assert ctypes.sizeof(summary_type) == 64
    for name, offset in (("struct_size", 0), ("version", 4), ("count_t", 8),
                         ("wins", 16), ("losses", 24), ("best_trade", 32),
                         ("worst_trade", 40), ("max_drawdown", 48),
                         ("nonfinite_t_field", 56), ("complete", 60)):
        assert getattr(summary_type, name).offset == offset


@pytest.mark.parametrize("fault", [
    "summary-incomplete", "summary-count", "summary-status", "unknown-field",
    "metric-count", "report-count", "receipt-incomplete", "receipt-count",
    "receipt-status", "negative-count",
    "diagnostics-incomplete", "diagnostics-count", "diagnostics-status",
])
def test_fd_rejects_incomplete_or_inconsistent_summary(tmp_path, fault):
    source = bars.csv_file(tmp_path)
    with Stream() as stream:
        status, text, stderr = invoke(
            source, FdLibrary(fd_fault=fault), ["--report-sink-fd", str(stream.writer)],
            no_files=True)
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "engine_invariant"
    assert len(text.splitlines()) == 1 and "summary" not in json.loads(text)


@pytest.mark.parametrize("field", full.TRADE_FLOATS)
def test_fd_nonfinite_trade_failure_matches_file(tmp_path, field):
    source = bars.csv_file(tmp_path)
    reference = invoke(source, FdLibrary(bad_field=field),
                       ["--report-sink", str(tmp_path / "bad.sink")])
    with Stream() as stream:
        actual = invoke(source, FdLibrary(bad_field=field),
                        ["--report-sink-fd", str(stream.writer)], no_files=True)
    assert reference == actual
    assert actual[0] == 1 and actual[2] == ""
    assert json.loads(actual[1])["args"] == {"field": "trades." + field, "reason": "nonfinite"}


def test_fd_summary_overflow_keeps_canonical_validation(tmp_path):
    source = bars.csv_file(tmp_path)
    with Stream() as stream:
        status, text, stderr = invoke(source, FdLibrary(fd_fault="summary-overflow"),
                                     ["--report-sink-fd", str(stream.writer)], no_files=True)
    assert status == 1 and stderr == ""
    assert json.loads(text)["args"] == {"field": "summary.max_drawdown", "reason": "nonfinite"}


@pytest.mark.parametrize("extra", [
    ["--report-sink", "unused.sink"], ["--report-sink-format", "2"], ["--bench"],
    ["--validate-window-only"], ["--capabilities-json"],
])
def test_fd_exclusions_are_usage_errors_before_library_calls(tmp_path, extra):
    source, library = bars.csv_file(tmp_path), FdLibrary()
    with Stream() as stream:
        status, text, stderr = invoke(source, library,
            ["--report-sink-fd", str(stream.writer), *extra], no_files=True)
    assert status == 2 and stderr == "" and library.calls == []
    document = json.loads(text)
    assert document["code"] == "run_request_invalid" and document["args"] == {"option": "arguments"}
    if "2" in extra:
        assert "transcoder" in document["error"]


@pytest.mark.parametrize("value", ["-1", "0", "1", "2", str(1 << 40), "word"])
def test_fd_bad_descriptor_is_a_usage_error(tmp_path, value):
    source, library = bars.csv_file(tmp_path), FdLibrary()
    status, text, _ = invoke(source, library, ["--report-sink-fd", value], no_files=True)
    assert status == 2 and library.calls == []
    assert json.loads(text)["code"] == "run_request_invalid"


def test_fd_and_phase_transport_cannot_share_the_endpoint(tmp_path):
    source, library = bars.csv_file(tmp_path), FdLibrary()
    with Stream() as stream:
        status, text, _ = invoke(source, library,
            ["--report-sink-fd", str(stream.writer), "--run-phase-fd", "0" + str(stream.writer)],
            no_files=True)
    assert status == 2 and library.calls == []
    assert json.loads(text)["code"] == "run_request_invalid"


@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
def test_fd_five_is_reserved_by_admission_guard(profile):
    # Exercise admission alone: a fail-before run must never hand pytest's
    # own fd 5 to the old implementation, which would take and close it.
    args = argparse.Namespace(
        profile=profile, report_sink_fd=5, report_sink=None, spill_dir=None,
        bench=False, validate_window_only=False, run_phase_fd=None,
        report_sink_format=1, run_id="")
    with pytest.raises(run_json.RunFailure) as failure:
        run_json._validate_sink_options(args)
    assert failure.value.exit_status == 2
    assert failure.value.code == "run_request_invalid"


@pytest.mark.parametrize("missing", [
    "strategy_set_report_sink_fd_v1", "strategy_report_sink_summary_v1",
    "strategy_report_sink_diagnostics_v1",
])
def test_fd_missing_api_never_falls_back_to_a_path(tmp_path, missing):
    source, library = bars.csv_file(tmp_path), FdLibrary()
    delattr(library, missing)
    with Stream() as stream:
        status, text, stderr = invoke(source, library,
            ["--report-sink-fd", str(stream.writer)], no_files=True)
        os.fstat(stream.writer)  # never handed to the engine; caller still owns it
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "strategy_library_incompatible"
    assert json.loads(text)["args"] == {"reason": "symbol_missing"}
    assert not library.installed and "run_backtest_full" not in library.calls


@pytest.mark.parametrize("fault,code", [
    ("engine", "report_sink_failed"), ("typed", "report_sink_failed"),
    ("exception", "harness_internal_error"), ("already-closed", "report_sink_failed"),
    ("setter", "report_sink_failed"), ("setter-exception", "harness_internal_error"),
])
def test_fd_every_failure_closes_once_before_stdout(tmp_path, fault, code):
    source = bars.csv_file(tmp_path)
    closes = []
    real_close = os.close
    with Stream() as stream:
        class Output(io.StringIO):
            def write(self, text):
                with pytest.raises(OSError) as closed:
                    os.fstat(stream.writer)
                assert closed.value.errno == errno.EBADF
                return super().write(text)

        def close(fd):
            if fd == stream.writer:
                closes.append(fd)
            return real_close(fd)

        with mock.patch.object(os, "close", close):
            status, text, _ = invoke(source, FdLibrary(fd_fault=fault),
                ["--report-sink-fd", str(stream.writer)], no_files=True, stdout=Output())
        assert closes == [stream.writer] * (2 if fault == "already-closed" else 1)
    assert status == 1 and json.loads(text)["code"] == code
    assert len(text.splitlines()) == 1 and "summary" not in json.loads(text)
    assert b'"trailer":' not in stream.data


def selected_invoke(tmp_path, library, extra, *, no_files=False):
    source = tmp_path / "chart.csv"
    selected.write_ohlcv(source, selected.csv_rows(library.plan["supplied_input_bars"]))
    artifact, inventory = selected.write_inventory(tmp_path, [])
    output, errors = io.StringIO(), io.StringIO()
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.object(run_json, "load_strategy", lambda path: library))
        stack.enter_context(mock.patch.object(run_json.time, "time", lambda: 10.))
        stack.enter_context(mock.patch.object(
            selected_sinks.planner, "SelectedPrimaryPlanner", lambda path: types.SimpleNamespace(
                plan=lambda *args, **kwargs: dict(library.plan))))
        stack.enter_context(mock.patch.object(
            run_json, "normalize_release_provenance", lambda provenance, *args: provenance))
        stack.enter_context(mock.patch.object(run_json, "require_outputs", lambda lib: None))
        stack.enter_context(mock.patch.object(run_json, "enable_outputs", lambda lib, state: None))
        stack.enter_context(mock.patch.object(
            run_json, "_outputs_manifest", lambda lib, state: full.outputs_reader().manifest()))
        if no_files:
            stack.enter_context(forbid_report_files())
        stack.enter_context(contextlib.redirect_stdout(output))
        stack.enter_context(contextlib.redirect_stderr(errors))
        status = run_json.main(selected.selected_argv(
            artifact, source, inventory,
            extra=[*(["--outputs"] if library.outputs else []), *extra]))
    return status, output.getvalue(), errors.getvalue()


@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("empty", [False, True])
def test_selected_fd_preserves_document_and_stream(tmp_path, outputs, empty):
    plan = selected.empty_window_plan() if empty else selected.full_plan()
    pnls = () if empty else PNLS
    path = tmp_path / "selected.sink"
    reference = selected_invoke(tmp_path, SelectedFdLibrary(plan, pnls=pnls, outputs=outputs),
                                ["--report-sink", str(path), "--run-id", RUN_ID])
    assert reference[0] == 0 and reference[2] == "", reference
    with Stream() as stream:
        library = SelectedFdLibrary(plan, pnls=pnls, outputs=outputs)
        actual = selected_invoke(tmp_path, library,
            ["--report-sink-fd", str(stream.writer), "--run-id", RUN_ID], no_files=True)
    assert actual[0] == 0 and actual[2] == "", actual
    assert stream.data == path.read_bytes()
    expected, document = projection(reference[1]), json.loads(actual[1])
    # The sole selected wall-clock field is outside the fingerprint.
    expected["diagnostics"].pop("phase_timing")
    document["diagnostics"].pop("phase_timing")
    assert_document(json.dumps(document, separators=(",", ":")) + "\n", expected)
    assert library.queried == library.summary_queries == 1
    assert library.closed == library.installed == [stream.writer]


@pytest.mark.parametrize("fault,code", [
    ("typed", "report_sink_failed"), ("engine", "report_sink_failed"),
    ("exception", "report_post_execution_failed"),
])
def test_selected_fd_failure_closes_before_stdout(tmp_path, fault, code):
    plan = selected.full_plan()
    observed = []
    write_failure = run_json.write_failure
    with Stream() as stream:
        def failure(*args):
            with pytest.raises(OSError) as closed:
                os.fstat(stream.writer)
            assert closed.value.errno == errno.EBADF
            observed.append(True)
            write_failure(*args)

        with mock.patch.object(run_json, "write_failure", failure):
            status, text, _ = selected_invoke(
                tmp_path, SelectedFdLibrary(plan, fd_fault=fault),
                ["--report-sink-fd", str(stream.writer)], no_files=True)
    assert status == 1 and json.loads(text)["code"] == code and observed == [True]
    assert "summary" not in json.loads(text) and b'"trailer":' not in stream.data


def snapshot(*directories):
    return sorted((str(path), path.stat().st_mode, path.stat().st_size)
                  for directory in directories for path in directory.rglob("*"))


def sealed_arena_fd(size):
    """A memfd sized to `size` and sealed GROW|SHRINK|SEAL (CR-267: the jailed
    admission requires a sealed memfd on Linux). The caller owns the fd."""
    flags = 1 | 2  # MFD_CLOEXEC | MFD_ALLOW_SEALING (linux/memfd.h)
    if hasattr(os, "memfd_create"):
        fd = os.memfd_create("pineforge-sink-fd-arena", flags)
    else:
        # The lab's relocatable CPython is built without os.memfd_create.
        libc = ctypes.CDLL(None, use_errno=True)
        fd = libc.memfd_create(b"pineforge-sink-fd-arena", flags)
        if fd < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), "memfd_create")
    try:
        os.ftruncate(fd, size)
        seals = 0x0004 | 0x0002 | 0x0001  # F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL
        fcntl.fcntl(fd, 1033, seals)  # F_ADD_SEALS
        assert fcntl.fcntl(fd, 1034) == seals  # F_GET_SEALS
        # Keep the number above the child's fixed arena slot 5 (seals live on
        # the inode, so the duplicate carries them).
        moved = fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 16)
        os.close(fd)
        return moved
    except BaseException:
        os.close(fd)
        raise


def child_run(source, extra, *, transport="pipe", stub=True, jail=False,
              work=None, temporary=None, fault=None, break_after=None, pnls=PNLS,
              without_arena=False, arena_path=None, arena_fd=None,
              without_scratch_overrides=False):
    event_read, event_write = os.pipe()
    with contextlib.ExitStack() as inherited, Stream(transport, break_after=break_after) as stream:
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--events-fd", str(event_write), "--owned-fd", str(stream.writer)]
        pass_fds = (stream.writer, event_write)
        if arena_fd is not None:
            command += ["--arena-fd", str(arena_fd),
                        "--arena-bytes", str(os.fstat(arena_fd).st_size)]
            pass_fds += (arena_fd,)
        if arena_path is not None:
            arena = inherited.enter_context(open(arena_path, "r+b", buffering=0))
            command += ["--arena-fd", str(arena.fileno()),
                        "--arena-bytes", str(os.fstat(arena.fileno()).st_size)]
            pass_fds += (arena.fileno(),)
        if stub:
            command += ["--stub", "--pnls", json.dumps(pnls)]
        if fault:
            command += ["--fault", fault]
        if jail:
            command += ["--jail"]
        command += ["--", "--ohlcv", str(source), *map(str, extra)]
        if jail and temporary is not None and "--profile" not in extra:
            # Match the wrapper's explicit scratch directory for the
            # path-sink refusal too; tempfile's directory probing is not
            # the native writer/open failure that this control exercises.
            command += ["--spill-dir", str(temporary)]
        if "--report-sink" not in extra:
            command += ["--report-sink-fd", str(stream.writer)]
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        if without_arena:
            for name in ("PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES",
                         "PF_SCRATCH_WINDOW_BYTES"):
                environment.pop(name, None)
        if without_scratch_overrides:
            for name in ("PINEFORGE_COMMAND_SCRATCH_DIR", "PINEFORGE_RUN_JSON_PROFILE"):
                environment.pop(name, None)
        if temporary is not None:
            environment.update(TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
        try:
            process = subprocess.Popen(command, pass_fds=pass_fds,
                                       cwd=work or ROOT, env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        finally:
            os.close(event_write)
        # Only the child may hold a writer while it is executing.
        stream.close_writer()
        stdout, stderr = process.communicate()
    with os.fdopen(event_read, "rb") as events:
        observed = [json.loads(line) for line in events]
    return process.returncode, stdout.decode(), stderr.decode(), stream.data, observed


def test_fd_child_fsize_zero_read_only_work_and_tmp(tmp_path):
    source = bars.csv_file(tmp_path)
    reference = invoke(source, FdLibrary(), ["--report-sink", str(tmp_path / "reference.sink")])
    assert reference[0] == 0 and reference[2] == "", reference
    work, temporary = tmp_path / "work", tmp_path / "tmp"
    work.mkdir()
    temporary.mkdir()
    work.chmod(0o555)
    temporary.chmod(0o555)
    try:
        before = snapshot(work, temporary)
        result = child_run(source, ["--so", "unused.so"], jail=True, work=work, temporary=temporary)
        assert result[0] == 0 and result[2] == "", result
        assert_document(result[1], projection(reference[1]))
        assert result[3] == (tmp_path / "reference.sink").read_bytes()
        assert snapshot(work, temporary) == before
        refused = child_run(source, ["--so", "unused.so", "--report-sink", str(work / "bad.sink")],
                            jail=True, work=work, temporary=temporary)
        assert refused[0] == 1 and refused[2] == "", refused
        assert json.loads(refused[1])["code"] == "report_sink_failed"
        assert "summary" not in json.loads(refused[1])
    finally:
        work.chmod(0o755)
        temporary.chmod(0o755)


@pytest.mark.parametrize("transport", ["pipe", "socket"])
def test_fd_child_epipe_closes_before_typed_failure_line(tmp_path, transport):
    source = bars.csv_file(tmp_path)
    result = child_run(source, ["--so", "unused.so"], transport=transport,
                       break_after=1, pnls=PNLS * 1024)
    assert result[0] == 1 and result[2] == "", result
    document = json.loads(result[1])
    assert document["code"] == "report_sink_failed" and document["args"] == {}
    assert "summary" not in document and len(result[1].splitlines()) == 1
    assert result[3] and b'"trailer":' not in result[3]
    assert result[4][0] == {"event": "closed"}
    assert result[4][1:] and all(event == {"event": "stdout", "closed": True}
                               for event in result[4][1:])


def native_module(outputs):
    name = "PINEFORGE_L22V_OUTPUTS_SO" if outputs else "PINEFORGE_L22V_STRATEGY_SO"
    module = os.environ.get(name)
    if module is None:
        pytest.skip("SKIP lab module not supplied: " + name)
    assert Path(module).is_file(), module
    return module


@pytest.mark.parametrize("transport", ["pipe", "socket"])
@pytest.mark.parametrize("outputs", [False, True])
def test_native_fd_matches_file_document_and_wire(tmp_path, transport, outputs):
    source, path = bars.csv_file(tmp_path, count=97), tmp_path / "native.sink"
    extra = ["--so", native_module(outputs), "--input-tf", "1", "--script-tf", "1",
             "--run-id", RUN_ID, *(["--outputs"] if outputs else [])]
    reference = child_run(source, [*extra, "--report-sink", str(path)], stub=False)
    assert reference[0] == 0 and reference[2] == "", reference
    actual = child_run(source, extra, stub=False, transport=transport)
    assert actual[0] == 0 and actual[2] == "", actual
    assert actual[3] == path.read_bytes()
    assert_document(actual[1], projection(reference[1]))
    assert actual[4][0] == {"event": "closed"}
    assert all(event["closed"] for event in actual[4][1:])


def test_native_fd_records_leave_only_small_stdout(tmp_path):
    source = bars.csv_file(tmp_path, count=97)
    module = native_module(False)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("PINEFORGE_", "PF_SCRATCH_"))}
    environment.update(PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(tmp_path))
    with Stream() as stream:
        argv = ["--so", module, "--ohlcv", str(source), "--input-tf", "1",
                "--script-tf", "1", "--report-sink-fd", str(stream.writer)]
        process = subprocess.Popen([sys.executable, "-B", str(ROOT / "docker" / "run_json.py"),
                                    *argv], env=environment, pass_fds=(stream.writer,),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stream.close_writer()
        stdout, stderr = process.communicate()
    observed = f"observed run_json argv: {argv!r}\n{process.returncode}, {stdout!r}, {stderr!r}"
    assert process.returncode == 0 and stderr == b"", observed
    assert stdout.endswith(b"\n") and len(stdout.splitlines()) == 1, observed
    document = json.loads(stdout)
    assert document["engine"] == "pineforge" and "summary" in document, observed
    assert not {"trades", "equity_curve", "report_sink"} & document.keys(), observed
    with pf_sink.SinkReader(stream=io.BytesIO(stream.data)) as reader:
        for _ in reader:
            pass
        assert reader.complete and reader.trailer is not None, observed
        assert reader.counts["T"] == document["summary"]["total_trades"], observed
        assert reader.counts["E"] > 0, observed


def test_native_fd_fsize_zero_without_arena_refuses_before_file_operations(tmp_path):
    source = bars.csv_file(tmp_path, count=17)
    extra = ["--so", native_module(False), "--input-tf", "1", "--script-tf", "1"]
    path, work, temporary = tmp_path / "native.sink", tmp_path / "work", tmp_path / "tmp"
    reference = child_run(source, [*extra, "--report-sink", str(path)], stub=False)
    assert reference[0] == 0 and reference[2] == "", reference
    work.mkdir()
    temporary.mkdir()
    work.chmod(0o555)
    temporary.chmod(0o555)
    try:
        before = snapshot(work, temporary)
        # Stage-0 admission belongs to the jailed profile; an unjailed fd
        # request does not require an arena. Keep the resource restrictions,
        # but reach that admission explicitly without inherited backing.
        actual = child_run(source, [*extra, "--profile", "jailed"], stub=False, jail=True,
                           work=work, temporary=temporary, without_arena=True)
        assert actual[0] == 1 and actual[2] == "", actual
        failure = json.loads(actual[1])
        assert (failure["code"], failure["args"]) == (
            "report_sink_failed", {"reason": "arena_backing"})
        assert set(failure) == {"engine", "error", "code", "args", "reason"}
        assert failure["reason"] == failure["args"]["reason"]
        assert len(actual[1].splitlines()) == 1 and "summary" not in failure
        assert actual[3] == b"" and snapshot(work, temporary) == before
    finally:
        work.chmod(0o755)
        temporary.chmod(0o755)


def test_native_fd_fsize_zero_without_arena_tmpdir_cause(tmp_path):
    source = bars.csv_file(tmp_path, count=17)
    extra = ["--so", native_module(False), "--input-tf", "1", "--script-tf", "1"]
    work, temporary = tmp_path / "work", tmp_path / "tmp"
    work.mkdir()
    temporary.mkdir()
    work.chmod(0o555)
    temporary.chmod(0o555)
    try:
        try:
            probe = os.open(temporary / "probe", os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except PermissionError:
            pass
        else:
            os.close(probe)
            pytest.skip("mode-0555 TMPDIR permits create: root or DAC capabilities "
                        "invalidate this Python row's permission precondition")
        before = snapshot(work, temporary)
        refused = child_run(source, [*extra, "--report-sink", str(work / "bad.sink")],
                            stub=False, jail=True, work=work, temporary=temporary,
                            without_arena=True, without_scratch_overrides=True)
        assert refused[0] == 1 and refused[2] == "", refused
        # With the unjailed profile and no arena or explicit scratch override,
        # command scratch resolves TMPDIR and its create fails before sink open.
        failure = json.loads(refused[1])
        assert failure["code"] == "scratch_io_failed"
        assert "command scratch create" in failure["error"], failure
        assert "requires disk backing" not in failure["error"], failure
        assert snapshot(work, temporary) == before
    finally:
        work.chmod(0o755)
        temporary.chmod(0o755)


def test_native_fd_fsize_zero_path_sink_refused_in_read_only_work(tmp_path):
    source = bars.csv_file(tmp_path, count=17)
    extra = ["--so", native_module(False), "--input-tf", "1", "--script-tf", "1"]
    work, temporary = tmp_path / "work", tmp_path / "tmp"
    work.mkdir()
    temporary.mkdir()
    work.chmod(0o555)
    temporary.chmod(0o755)
    try:
        try:
            probe = os.open(work / "probe", os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except PermissionError:
            pass
        else:
            os.close(probe)
            pytest.skip("mode-0555 work permits create: root or DAC capabilities "
                        "invalidate this Python row's permission precondition")
        before = snapshot(work)
        # At 3b76e98b86 command scratch used /tmp and succeeded. The old red was
        # the native writer opening work/bad.sink in mode-0555 work:
        # engine_report.cpp:1194-1198 -> report_sink_writer.cpp:491-493,
        # EACCES -> report_sink_failed {reason: sink_io}, before TMPDIR use.
        # Writable, disk-backed temporary restores that sink-open subject here.
        refused = child_run(source, [*extra, "--report-sink", str(work / "bad.sink")],
                            stub=False, jail=True, work=work, temporary=temporary,
                            without_arena=True, without_scratch_overrides=True)
        assert refused[0] == 1 and refused[2] == "", refused
        failure = json.loads(refused[1])
        assert failure["code"] == "report_sink_failed"
        assert failure["args"] == {"reason": "sink_io"}
        assert snapshot(work) == before
    finally:
        work.chmod(0o755)
        temporary.chmod(0o755)


def test_native_jailed_fd_without_arena_refuses_before_scratch_or_sink(tmp_path):
    source = bars.csv_file(tmp_path, count=17)
    actual = child_run(source, ["--so", native_module(False), "--profile", "jailed"],
                       stub=False, jail=True, without_arena=True)
    assert actual[0] == 1 and actual[2] == "", actual
    failure = json.loads(actual[1])
    assert (failure["code"], failure["args"]) == (
        "report_sink_failed", {"reason": "arena_backing"})
    assert actual[3] == b"" and "summary" not in failure


def test_native_jailed_fd_with_arena_and_zero_fsize_creates_no_files(tmp_path):
    source = bars.csv_file(tmp_path, count=17)
    # On Linux the jailed admission requires a sealed memfd arena (CR-267); an
    # ordinary file is refused with report_sink_failed/arena_backing (the test
    # above). Off Linux there are no sealed memfds and the admission accepts
    # any attached arena, so the ordinary-file arena of the original row stays.
    if sys.platform.startswith("linux"):
        arena = sealed_arena_fd(16 * 1024 * 1024)
        arena_args = {"arena_fd": arena}
    else:
        arena = None
        arena_file = tmp_path / "inherited-arena"
        with arena_file.open("wb") as prepared:
            prepared.truncate(16 * 1024 * 1024)
        arena_args = {"arena_path": arena_file}
    work, temporary = tmp_path / "work", tmp_path / "tmp"
    work.mkdir(mode=0o555)
    temporary.mkdir(mode=0o555)
    try:
        before = snapshot(work, temporary)
        actual = child_run(
            source, ["--so", native_module(False), "--profile", "jailed",
                     "--input-tf", "1", "--script-tf", "1"],
            stub=False, jail=True, work=work, temporary=temporary, **arena_args)
        assert actual[0] == 0 and actual[2] == "", actual
        document = json.loads(actual[1])
        assert "trades" not in document and "equity_curve" not in document
        assert document["summary"]["bars_processed"] == 17
        with pf_sink.SinkReader(stream=io.BytesIO(actual[3])) as reader:
            records = list(reader)
            assert reader.complete
            assert sum(row.tag == "T" for row in records) == document["summary"]["total_trades"]
        assert snapshot(work, temporary) == before
    finally:
        if arena is not None:
            os.close(arena)
        work.chmod(0o755)
        temporary.chmod(0o755)


@pytest.mark.parametrize("transport", ["pipe", "socket"])
def test_native_fd_epipe_is_typed_and_ordered(tmp_path, transport):
    source = bars.csv_file(tmp_path, count=4096)
    result = child_run(source, ["--so", native_module(False)], stub=False,
                       transport=transport, break_after=1)
    assert result[0] == 1 and result[2] == "", result
    assert json.loads(result[1])["code"] == "report_sink_failed"
    assert "summary" not in json.loads(result[1]) and len(result[1].splitlines()) == 1
    assert result[3] and b'"trailer":' not in result[3]
    assert result[4][0] == {"event": "closed"}
    assert all(event["closed"] for event in result[4][1:])


@pytest.mark.parametrize("empty", [False, True])
def test_native_selected_fd_keeps_the_file_document_and_wire(tmp_path, empty):
    module = native_module(False)
    argv = selected_sinks.native_argv(tmp_path, module, count=97, empty=empty)
    source, path = Path(argv[1]), tmp_path / "selected-native.sink"
    extra = ["--so", module, *argv[2:], "--run-id", RUN_ID]
    reference = child_run(source, [*extra, "--report-sink", str(path)], stub=False)
    assert reference[0] == 0 and reference[2] == "", reference
    actual = child_run(source, extra, stub=False)
    assert actual[0] == 0 and actual[2] == "", actual
    assert actual[3] == path.read_bytes()
    expected, document = projection(reference[1]), json.loads(actual[1])
    expected["diagnostics"].pop("phase_timing")
    document["diagnostics"].pop("phase_timing")
    assert_document(json.dumps(document, separators=(",", ":")) + "\n", expected)
    assert actual[4][0] == {"event": "closed"}
    assert all(event["closed"] for event in actual[4][1:])


def test_named_sink_open_refusal_keeps_typed_failure_through_cleanup(tmp_path):
    source, library = bars.csv_file(tmp_path), FdLibrary()
    path = tmp_path / "refused.sink"
    refusal = PermissionError(errno.EACCES, "Permission denied", str(path))
    with mock.patch.object(summaries, "open", side_effect=refusal, create=True) as opened:
        status, text, stderr = invoke(
            source, library, ["--profile", "unjailed", "--report-sink", str(path)])
    opened.assert_called_once_with(str(path), "wb")
    assert status == 1 and stderr == "", (status, text, stderr)
    assert len(text.splitlines()) == 1
    document = json.loads(text)
    assert document["code"] == "report_sink_failed" and document["args"] == {}
    assert "[Errno 13]" in document["error"] and "summary" not in document
    assert library.calls[-2:] == ["report_free", "strategy_free"]
    assert library.installed == library.freed == [str(path)]
    assert library.path is None and not path.exists()


def child_main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--events-fd", type=int, required=True)
    parser.add_argument("--owned-fd", type=int, required=True)
    parser.add_argument("--stub", action="store_true")
    parser.add_argument("--jail", action="store_true")
    parser.add_argument("--pnls", default=json.dumps(PNLS))
    parser.add_argument("--fault")
    parser.add_argument("--arena-fd", type=int)
    parser.add_argument("--arena-bytes", type=int)
    own, runner_argv = parser.parse_args(argv[:argv.index("--")]), argv[argv.index("--") + 1:]
    real_close = os.close
    original_stdout = sys.stdout
    if own.arena_fd is not None:
        os.dup2(own.arena_fd, 5)
        os.environ.update(PF_SCRATCH_ARENA_FD="5",
                          PF_SCRATCH_ARENA_BYTES=str(own.arena_bytes),
                          PF_SCRATCH_WINDOW_BYTES=str(1024 * 1024))

    def event(document):
        line = full.encoded(document)
        while line:
            written = os.write(own.events_fd, line)
            if written <= 0:
                raise OSError("event pipe made no progress")
            line = line[written:]

    def close(fd):
        real_close(fd)
        if fd == own.owned_fd:
            event({"event": "closed"})

    class StdoutProbe:
        def write(self, text):
            try:
                os.fstat(own.owned_fd)
                closed = False
            except OSError as error:
                if error.errno != errno.EBADF:
                    raise
                closed = True
            event({"event": "stdout", "closed": closed})
            return original_stdout.write(text)

        def flush(self):
            return original_stdout.flush()

        def fileno(self):
            return original_stdout.fileno()

    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.object(run_json.time, "time", lambda: 10.))
        stack.enter_context(mock.patch.object(os, "close", close))
        stack.enter_context(contextlib.redirect_stdout(StdoutProbe()))
        planner = os.environ.get("PINEFORGE_L22V_PLANNER_SO")
        if planner:
            stack.enter_context(mock.patch.object(run_json, "trusted_planner_path", lambda: planner))
        if own.stub:
            library = FdLibrary(fd_fault=own.fault, pnls=tuple(json.loads(own.pnls)))
            stack.enter_context(mock.patch.object(run_json, "load_strategy", lambda path: library))
        if own.jail:
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
        if "--report-sink-fd" in runner_argv:
            stack.enter_context(forbid_report_files())
        return run_json.main(runner_argv)


if __name__ == "__main__":
    sys.exit(child_main(sys.argv[1:]))
