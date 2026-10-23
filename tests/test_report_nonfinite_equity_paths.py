"""CR-289 step 1B: native equity poison through the three real report paths.

The selected-window execution stand-in owns native EquityPointC allocations.
Only its last, ordinary point is poisoned; its anchor stays intact. Arrays
reach the real report builder/validator. Sink points reach the production
C++ LineWriter through a test-only bridge.
Fd bytes are drained with the existing fd harness and replayed unchanged by
the real standalone converter. Separate completed-stream producer doubles
encode a final null E field to reach the downstream validation boundaries
that a native writer refusal cannot exercise. No test injects JSON NaN.

Lab only. The supervisor builds report_nonfinite_equity_fd and supplies
PINEFORGE_REPORT_NONFINITE_EQUITY_WRITER (CTest does this automatically).
A missing bridge fails, never skips. No production validator, serializer,
failure classifier, sink reader, or converter is mocked.
"""
from __future__ import annotations

import ctypes
import hashlib
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
import test_run_json_sink as full_sinks
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
    assert path.is_file(), f"build the CR-289 native writer bridge on the lab: {path}"
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


def assert_written_curve(result, wire):
    """H2-4: inspect the finite prefix actually written before native refusal."""
    library = result.library
    lines = [
        json.loads(line, parse_constant=nonfinite.reject_constant)
        for line in wire.splitlines()
    ]
    assert lines[0]["schema"] == "pf-report-sink"
    # cr289_write_equity copies counts/digest only after write_trailer; a
    # write_e refusal leaves the receipt zeroed but preserves earlier bytes.
    assert library.native_receipt.complete == 0
    assert library.native_receipt.count_e == 0
    assert bytes(library.native_receipt.sha256_hex) == b""
    points = lines[1:]
    assert points == [["E", *library.point(point)] for point in library._curve[:-1]]
    assert len(points) == len(library._curve) - 1
    assert points[0] == ["E", *library.anchor_before]
    assert points[-1][1] < library._curve[-1].time_ms
    assert all(all(math.isfinite(value) for value in row[2:]) for row in points)


def convert_fd_capture(tmp_path, stdout, wire):
    sink, metadata = tmp_path / "retained.sink", tmp_path / "stdout.json"
    sink.write_bytes(wire)  # Retain the exact drained bytes; never refold/edit.
    metadata.write_text(stdout, encoding="utf-8")
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


class CompletedNullEquityWriter:
    """H2-4: a receipt-valid producer double that bypasses LineWriter."""

    def __init__(self, field):
        self.field = field

    def write(self, path, fd, curve, receipt):
        assert path is not None and fd is None
        rows = [["E", point.time_ms, point.equity, point.open_profit] for point in curve]
        rows[-1][2 if self.field == "equity" else 3] = None
        counts, digest = full_sinks.write_sink(os.fsdecode(path.path), rows)
        receipt.struct_size, receipt.version = ctypes.sizeof(receipt), 1
        for tag in "ETOVKXH":
            setattr(receipt, "count_" + tag.lower(), counts.get(tag, 0))
        receipt.sha256_hex = digest.encode("ascii")
        receipt.complete = 1


def assert_completed_null_curve(wire, field):
    """Validate framing/digest separately from the canonical null-E refusal."""
    encoded = wire.splitlines(keepends=True)
    lines = [json.loads(line, parse_constant=nonfinite.reject_constant) for line in encoded]
    assert lines[0]["schema"] == lines[-1]["trailer"] == "pf-report-sink"
    points = lines[1:-1]
    assert len(points) >= 2 and all(row[0] == "E" for row in points)
    assert lines[-1]["counts"] == {"E": len(points), "T": 0}
    assert lines[-1]["sha256"] == hashlib.sha256(b"".join(encoded[:-1])).hexdigest()
    assert all(all(math.isfinite(value) for value in row[2:]) for row in points[:-1])
    position = 2 if field == "equity" else 3
    assert points[-1][position] is None
    assert math.isfinite(points[-1][5 - position])
    return lines[-1]


@pytest.mark.parametrize("field", ["equity", "open_profit"])
def test_file_sink_validator_refuses_completed_null_e(tmp_path, field):
    # H2-4: writer refusal must not mask _build_sink_report's validation.
    result = run_path(
        tmp_path, field, float("nan"), "file", writer=CompletedNullEquityWriter(field),
        extra=("--spill-dir", str(tmp_path)))
    library = result.library
    trailer = assert_completed_null_curve(library.wire, field)
    assert_native_poison(result, field, float("nan"))
    assert library.writer_calls == library.receipt_queries == 1
    assert library.native_receipt.complete == 1
    assert library.native_receipt.count_e == trailer["counts"]["E"] == len(library._curve)
    assert bytes(library.native_receipt.sha256_hex).decode("ascii") == trailer["sha256"]
    rows = [json.loads(line) for line in library.wire.splitlines()][1:-1]
    assert rows[:-1] == [["E", *library.point(point)] for point in library._curve[:-1]]
    expected_last = ["E", library._curve[-1].time_ms, FINITE["equity"], FINITE["open_profit"]]
    expected_last[2 if field == "equity" else 3] = None
    assert rows[-1] == expected_last
    nonfinite.assert_failure(
        result, "report_result_invalid",
        {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")


@pytest.mark.parametrize("field", ["equity", "open_profit"])
def test_converter_refuses_completed_null_e(tmp_path, field):
    # Obtain real, successful fd metadata with finite native-call fixtures.
    # Only the retained E payload is then changed; rebuild its real digest.
    source = full_sinks.bars.csv_file(tmp_path)
    library = fd_sinks.FdLibrary(pnls=())
    with fd_sinks.Stream() as stream:
        status, stdout, stderr = fd_sinks.invoke(
            source, library, ("--report-sink-fd", str(stream.writer)))
    assert status == 0 and stderr == "", (stdout, stderr)
    metadata = nonfinite.strict_line(stdout)
    assert "trades" not in metadata and "equity_curve" not in metadata
    lines = [json.loads(line, parse_constant=nonfinite.reject_constant)
             for line in stream.data.splitlines()]
    assert lines[-1]["trailer"] == "pf-report-sink"
    rows = lines[1:-1]
    assert all(row[0] == "E" for row in rows)
    rows[-1][2 if field == "equity" else 3] = None
    retained = tmp_path / "completed-null.sink"
    full_sinks.write_sink(retained, rows)
    wire = retained.read_bytes()
    assert_completed_null_curve(wire, field)
    converted, diagnostic = convert_fd_capture(tmp_path, stdout, wire)
    assert converted.returncode == 3, (converted.stdout, converted.stderr)
    assert converted.stdout == ""
    assert diagnostic["event"] == "sink_refused"
    assert diagnostic["code"] == "report_result_invalid"
    assert diagnostic["args"] == {"field": "equity_curve." + field, "reason": "nonfinite"}


@pytest.mark.parametrize("field,bad", CASES)
def test_non_sink_native_nonfinite(tmp_path, field, bad):
    # Removing the matching equity field from _validate_report_result's
    # equity_curve loop must make the corresponding array cases fail.
    result = run_path(tmp_path, field, bad, "array")
    assert_native_poison(result, field, bad)
    assert not any(hasattr(result.library, name) for name in SINK_EXPORTS)
    assert len(result.built) == 1
    nonfinite.assert_failure(
        result, "report_result_invalid",
        {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")


@pytest.mark.parametrize("field,bad", CASES)
def test_file_sink_native_nonfinite(tmp_path, native_writer, field, bad):
    # This native path proves LineWriter::write_e refusal, not the later
    # _build_sink_report validator covered by the completed-stream row.
    result = run_path(tmp_path, field, bad, "file", writer=native_writer,
                      extra=("--spill-dir", str(tmp_path)))
    assert_native_poison(result, field, bad)
    assert result.library.writer_calls == 1
    assert result.library.path_descriptor is not None
    assert result.library.fd_descriptor is None
    assert_written_curve(result, result.library.wire)
    nonfinite.assert_failure(
        result, "report_result_invalid",
        {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")


@pytest.mark.parametrize("field,bad", CASES)
def test_fd_sink_native_nonfinite(tmp_path, native_writer, field, bad):
    # The writer rejects the last native point before emitting it or a
    # trailer. Completed-null converter coverage uses its own fixture above.
    with fd_sinks.Stream() as stream:
        result = run_path(
            tmp_path, field, bad, "fd", writer=native_writer,
            extra=("--report-sink-fd", str(stream.writer)))
    assert_native_poison(result, field, bad)
    assert result.library.writer_calls == 1
    assert result.library.path_descriptor is None
    assert result.library.fd_descriptor.fd == stream.writer
    assert_written_curve(result, stream.data)
    converted, diagnostic = convert_fd_capture(tmp_path, result.stdout, stream.data)
    evidence = {
        "run_json_status": result.status, "run_json_stdout": result.stdout,
        "run_json_stderr": result.stderr, "converter_status": converted.returncode,
        "converter_stdout": converted.stdout, "converter_stderr": converted.stderr,
    }
    # The ctypes bridge raises the writer's typed Python RunFailure. This
    # does not exercise the native last-error relay.
    assert result.status == 1, evidence
    nonfinite.assert_failure(
        result, "report_result_invalid",
        {"field": "equity_curve." + field, "reason": "nonfinite"}, mode="selected")
    # Existing converter convention: EOF without a trailer is 6.
    assert converted.returncode == 6, evidence
    assert converted.stdout == "", evidence
    assert diagnostic["event"] == "sink_refused", evidence
