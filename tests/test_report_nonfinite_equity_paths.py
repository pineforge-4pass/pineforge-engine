"""Native equity poison through the three real report paths.

The selected-window execution stand-in owns native EquityPointC allocations.
Only its last, ordinary point is poisoned; its anchor stays intact. Arrays
reach the real report builder/validator. Sink points reach the production
C++ LineWriter through a test-only bridge; no Python fixture encodes E rows.
Fd bytes are drained with the existing fd harness and replayed unchanged by
the real standalone converter. Tests never inject JSON NaN or bare null.

Lab only. The supervisor builds report_nonfinite_equity_fd and supplies
PINEFORGE_REPORT_NONFINITE_EQUITY_WRITER (CTest does this automatically).
A missing bridge fails, never skips. No production validator, serializer,
failure classifier, sink reader, or converter is mocked.
"""
from __future__ import annotations

import ctypes
import json
import math
import os
from pathlib import Path
import subprocess
import sys
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_nonfinite_report as nonfinite
import test_run_json_selected_window as selected
import test_run_json_sink_fd as fd_sinks


CASES = [
    pytest.param(field, bad, id=f"{field}-{name}")
    for field in ("equity", "open_profit")
    for name, bad in (("nan", float("nan")), ("posinf", float("inf")),
                      ("neginf", float("-inf")))
]
FINITE = {"equity": 12345.25, "open_profit": -37.5}
SINK_EXPORTS = (
    "pf_report_sink_version", "strategy_set_report_sink_v1",
    "strategy_set_report_sink_fd_v1", "strategy_report_sink_receipt_v1",
    "strategy_report_sink_summary_v1",
)
CONVERTER = ROOT / "docker" / "pineforge_sink_to_json.py"


class NativeWriter:
    def __init__(self, path):
        self.library = ctypes.CDLL(str(path))
        self.library.cr289_write_equity.argtypes = [
            ctypes.POINTER(run_json.ReportSinkC),
            ctypes.POINTER(run_json.ReportSinkFdC),
            ctypes.POINTER(run_json.EquityPointC), ctypes.c_int64,
            ctypes.POINTER(run_json.ReportSinkReceiptC),
        ]
        self.library.cr289_write_equity.restype = ctypes.c_int
        for name in ("text", "code", "args"):
            getter = getattr(self.library, "cr289_writer_error_" + name)
            getter.argtypes, getter.restype = [], ctypes.c_char_p

    def write(self, path, fd, curve, receipt):
        status = self.library.cr289_write_equity(
            ctypes.byref(path) if path is not None else None,
            ctypes.byref(fd) if fd is not None else None,
            curve, len(curve), ctypes.byref(receipt))
        if status != 0:
            # Preserve the real exception's classified metadata. Neither the
            # poisoned field nor its expected code is used to construct it.
            raise run_json.RunFailure(
                self.library.cr289_writer_error_text().decode("utf-8"),
                self.library.cr289_writer_error_code().decode("utf-8"),
                json.loads(self.library.cr289_writer_error_args(),
                           parse_constant=nonfinite.reject_constant))


@pytest.fixture(scope="module")
def native_writer():
    path = Path(os.environ.get(
        "PINEFORGE_REPORT_NONFINITE_EQUITY_WRITER",
        ROOT / "build" / "report_nonfinite_equity" / "report_nonfinite_equity_fd.so"))
    assert path.is_file(), f"build the native writer bridge on the Linux test host: {path}"
    return NativeWriter(path)


class EquityLibrary(nonfinite.ReportLib):
    """Replace execution only; the reporting and refusal boundaries stay real."""

    def __init__(self, plan, events, *, field, bad, route, writer=None, **options):
        super().__init__(plan, events, **options)
        self.field, self.bad, self.route, self.writer = field, bad, route, writer
        self.path_descriptor = self.fd_descriptor = None
        self.writer_calls = self.receipt_queries = self.summary_queries = 0
        self.native_receipt = run_json.ReportSinkReceiptC()
        self.wire = None
        if route != "array":
            self.pf_report_sink_version = lambda: 1
            self.strategy_set_report_sink_v1 = self.install_path
            self.strategy_report_sink_receipt_v1 = self.read_receipt
        if route == "fd":
            self.strategy_set_report_sink_fd_v1 = self.install_fd
            self.strategy_report_sink_summary_v1 = self.read_summary
            self.strategy_report_sink_diagnostics_v1 = self.read_diagnostics

    def install_path(self, state, ref):
        descriptor = ref._obj
        assert self.route == "file"
        assert descriptor.struct_size == ctypes.sizeof(run_json.ReportSinkC)
        assert descriptor.version == 1
        self.path_descriptor = run_json.ReportSinkC(
            descriptor.struct_size, descriptor.version, descriptor.path,
            descriptor.run_id, descriptor.scratch_dir)
        self.calls.append(("set_report_sink",))
        return 0

    def install_fd(self, state, ref):
        descriptor = ref._obj
        assert self.route == "fd"
        assert descriptor.struct_size == ctypes.sizeof(run_json.ReportSinkFdC)
        assert descriptor.version == 1
        self.fd_descriptor = run_json.ReportSinkFdC(
            descriptor.struct_size, descriptor.version, descriptor.fd, descriptor.run_id)
        self.calls.append(("set_report_sink_fd",))
        return 0

    def read_receipt(self, state, ref):
        self.receipt_queries += 1
        assert ref._obj.struct_size == ctypes.sizeof(self.native_receipt)
        assert ref._obj.version == 1
        ctypes.memmove(ref, ctypes.byref(self.native_receipt),
                       ctypes.sizeof(self.native_receipt))
        return 0

    def read_summary(self, state, ref):
        self.summary_queries += 1
        assert not self.pnls and self.native_receipt.count_t == 0
        assert ref._obj.struct_size == ctypes.sizeof(run_json.ReportSinkSummaryC)
        assert ref._obj.version == 1
        # This fixture emits no T rows. The initialized empty summary has
        # zero counts/extrema; completion comes from the real writer.
        ref._obj.complete = self.native_receipt.complete
        return 0

    def read_diagnostics(self, state, ref):
        # The join line's run_json requires this fd export (endpoint/provenance
        # query). Counts and completion come from the writer's own receipt; the
        # endpoints are the native curve the real writer consumed (only times
        # are checked by run_json). Nothing here encodes an E row.
        result = ref._obj
        assert result.struct_size == ctypes.sizeof(run_json.ReportSinkDiagnosticsC) == 72
        assert result.version == 1
        result.count_e = self.native_receipt.count_e
        if result.count_e:
            first, last = self._curve[0], self._curve[len(self._curve) - 1]
            result.first = run_json.EquityPointC(first.time_ms, first.equity, first.open_profit)
            result.last = run_json.EquityPointC(last.time_ms, last.equity, last.open_profit)
        result.trade_at_start = 0
        result.complete = self.native_receipt.complete
        return 0

    def _run(self, *args):
        super()._run(*args)
        self.anchor_before = self.point(self._curve[0])
        self._curve[-1].equity = FINITE["equity"]
        self._curve[-1].open_profit = FINITE["open_profit"]
        setattr(self._curve[-1], self.field, self.bad)
        self.anchor_after = self.point(self._curve[0])
        if self.route == "array":
            return
        report = args[-1]._obj
        # Sink ABI carries only scalars; the writer consumes the retained
        # native backing allocation, not a report dict or reconstructed JSON.
        report.trades = ctypes.POINTER(run_json.TradeC)()
        report.trades_len = 0
        report.equity_curve = ctypes.POINTER(run_json.EquityPointC)()
        report.equity_curve_len = 0
        self.writer_calls += 1
        try:
            self.writer.write(self.path_descriptor, self.fd_descriptor,
                              self._curve, self.native_receipt)
        finally:
            if self.path_descriptor is not None:
                self.wire = Path(os.fsdecode(self.path_descriptor.path)).read_bytes()

    @staticmethod
    def point(point):
        return point.time_ms, point.equity, point.open_profit


def run_path(tmp_path, field, bad, route, *, writer=None, extra=()):
    harness = nonfinite.Harness(tmp_path)

    def library(plan, events, **options):
        return EquityLibrary(plan, events, field=field, bad=bad, route=route,
                             writer=writer, **options)

    with mock.patch.object(nonfinite, "ReportLib", side_effect=library):
        return harness.run("selected", pnls=(), net_profit=0.0, extra=extra)


def assert_native_poison(result, field, bad):
    library = result.library
    assert library.anchor_before == library.anchor_after == (selected.T, selected.CAPITAL, 0.0)
    assert len(library._curve) == library.plan["window_script_bars"] + 1
    point = library._curve[-1]
    assert point.time_ms > library._curve[0].time_ms
    actual = getattr(point, field)
    assert not math.isfinite(actual)
    assert math.isnan(actual) if math.isnan(bad) else actual == bad
    companion = "open_profit" if field == "equity" else "equity"
    assert getattr(point, companion) == FINITE[companion]
    assert math.isfinite(getattr(point, companion))


def assert_written_curve(result, wire, field):
    """Check writer provenance only when it completed, allowing writer refusal."""
    library = result.library
    if not library.native_receipt.complete:
        return
    lines = [
        json.loads(line, parse_constant=nonfinite.reject_constant)
        for line in wire.splitlines()
    ]
    assert lines[0]["schema"] == "pf-report-sink"
    assert lines[-1]["trailer"] == "pf-report-sink"
    points = [row for row in lines[1:-1] if row[0] == "E"]
    assert len(points) == library.native_receipt.count_e == len(library._curve)
    assert points[0] == ["E", *library.anchor_before]
    assert points[-1][1] == library._curve[-1].time_ms
    position = 2 if field == "equity" else 3
    companion = "open_profit" if field == "equity" else "equity"
    # This null is observed ONLY after the native poison above went through
    # the real writer. This is no claim about independently supplied nulls.
    assert points[-1][position] is None
    assert points[-1][5 - position] == FINITE[companion]


def convert_fd_capture(tmp_path, result, wire):
    sink, metadata = tmp_path / "retained.sink", tmp_path / "stdout.json"
    sink.write_bytes(wire)  # Retain the exact drained bytes; never refold/edit.
    metadata.write_text(result.stdout, encoding="utf-8")
    converted = subprocess.run(
        [sys.executable, "-I", "-B", "-S", str(CONVERTER),
         "--in", str(sink), "--stdout-json", str(metadata), "--out", "-",
         "--max-bytes", str(max(1, len(wire)))],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30,
        cwd=tmp_path)
    # Preserve both actual output channels for the supervisor's red run.
    (tmp_path / "converter.stdout.json").write_text(converted.stdout, encoding="utf-8")
    (tmp_path / "converter.stderr.jsonl").write_text(converted.stderr, encoding="utf-8")
    diagnostics = [
        json.loads(line, parse_constant=nonfinite.reject_constant)
        for line in converted.stderr.splitlines()
    ]
    assert diagnostics and converted.stderr.endswith("\n"), converted
    assert all(row["tool"] == "pineforge-sink-to-json" for row in diagnostics)
    assert diagnostics[-1]["exit_code"] == converted.returncode
    return converted, diagnostics[-1]


@pytest.mark.parametrize("field,bad", CASES)
def test_non_sink_native_nonfinite(tmp_path, field, bad):
    # Mutation: for equity, remove ("equity", "equity_curve.equity") at
    # docker/run_json.py:4372; for open_profit, remove
    # ("open_profit", "equity_curve.open_profit") at docker/run_json.py:4373.
    result = run_path(tmp_path, field, bad, "array")
    assert_native_poison(result, field, bad)
    assert not any(hasattr(result.library, name) for name in SINK_EXPORTS)
    assert len(result.built) == 1
    nonfinite.assert_failure(
        result, "report_result_invalid",
        {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")


@pytest.mark.parametrize("field,bad", CASES)
def test_file_sink_native_nonfinite(tmp_path, native_writer, field, bad):
    # Mutation: remove _validate_report_result(out) in _build_sink_report's
    # non-fd branch at docker/run_json.py:8038.
    result = run_path(tmp_path, field, bad, "file", writer=native_writer,
                      extra=("--spill-dir", str(tmp_path)))
    assert_native_poison(result, field, bad)
    assert result.library.writer_calls == 1
    assert result.library.path_descriptor is not None
    assert result.library.fd_descriptor is None
    assert_written_curve(result, result.library.wire, field)
    nonfinite.assert_failure(
        result, "report_result_invalid",
        {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")


@pytest.mark.parametrize("field,bad", CASES)
def test_fd_sink_native_nonfinite(tmp_path, native_writer, field, bad):
    # Mutation: remove _check_equity(row) from _Sink.records in
    # docker/pineforge_sink_to_json.py; all six fd cases then see real
    # run_json/converter statuses 0/0 and fail the status assertion below.
    with fd_sinks.Stream() as stream:
        result = run_path(
            tmp_path, field, bad, "fd", writer=native_writer,
            extra=("--report-sink-fd", str(stream.writer)))
    assert_native_poison(result, field, bad)
    assert result.library.writer_calls == 1
    assert result.library.path_descriptor is None
    assert result.library.fd_descriptor.fd == stream.writer
    assert_written_curve(result, stream.data, field)
    converted, diagnostic = convert_fd_capture(tmp_path, result, stream.data)
    evidence = {
        "run_json_status": result.status, "run_json_stdout": result.stdout,
        "run_json_stderr": result.stderr, "converter_status": converted.returncode,
        "converter_stdout": converted.stdout, "converter_stderr": converted.stderr,
    }
    # These are the real harness/CLI statuses, not a test-created exit code:
    # run_json failure is 1; the converter's unusable-row refusal is 3.
    status = result.status if result.status else converted.returncode
    assert status in (1, 3), evidence
    if result.status:
        # A native writer refusal is relayed by main's real failure handler.
        # A completed stream can also be refused later by report assembly.
        nonfinite.assert_failure(
            result, "report_result_invalid",
            {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")
        if result.library.native_receipt.complete:
            # A valid retained stream still gets its documented converter
            # verdict; it cannot override the producer's typed failure.
            assert converted.returncode in (0, 3), evidence
            if converted.returncode == 0:
                nonfinite.strict_line(converted.stdout)
                assert diagnostic["event"] == "converter_complete", evidence
            else:
                assert converted.stdout == "", evidence
                assert diagnostic["event"] == "sink_refused", evidence
        else:
            # Existing converter convention: EOF without a trailer is 6.
            assert converted.returncode == 6, evidence
            assert converted.stdout == "", evidence
            assert diagnostic["event"] == "sink_refused", evidence
    else:
        # A converter-side fix retains its existing stderr-only failure
        # channel. Require typed metadata there, with no report stdout.
        assert result.status == 0 and result.stderr == "", evidence
        small = nonfinite.strict_line(result.stdout)
        assert "equity_curve" not in small and "trades" not in small, evidence
        assert converted.returncode == 3, evidence
        assert converted.stdout == "", evidence
        assert diagnostic["event"] == "sink_refused", evidence
        assert diagnostic["code"] == "report_result_invalid", evidence
        assert diagnostic["args"] == {
            "field": "equity_curve." + field, "reason": "nonfinite",
        }, evidence


def test_report_schema_documents_e_descriptor():
    document = (ROOT / "docs/pages/report-schema.md").read_text(encoding="utf-8")
    paragraph = " ".join(document.split("For path and fd report sinks,", 1)[1]
                         .split("\n\n", 1)[0].split())
    for clause in (
            "v1 E descriptor keeps the nullable `f64?`",
            "byte compatibility",
            "writer never emits `null` for E",
            "converter and finalizer refuse a null E with exit 3 `report_result_invalid`"):
        assert clause in paragraph
