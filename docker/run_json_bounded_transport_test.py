"""R19–R22 transport regressions; run on the lab's disk-backed workspace.

The array producer is the checked-in, immutable pre-L9A oracle. Native report
storage is prepared before tracing: these rows measure Python transport, not
the native report allocation owned by the other lanes.
"""
from contextlib import ExitStack
import ctypes
import hashlib
import io
import json
import mmap
import os
from pathlib import Path
import sys
import tracemalloc
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import run_json
import test_run_json_sink as ordinary
import test_run_json_selected_sink as selected
from test_run_json_sink import frozen  # noqa: F401


def population(count):
    trades = (run_json.TradeC * count)()
    curve = (run_json.EquityPointC * (2 * count))()
    pnls = (-0., 0., 1e16, 1., -1e16, -2., 3.)
    for index, trade in enumerate(trades):
        trade.entry_time, trade.exit_time = ordinary.bars.T0, ordinary.bars.T0 + 60000
        trade.entry_price, trade.exit_price, trade.qty = 100., 101., 2.
        trade.pnl, trade.pnl_pct = pnls[index % len(pnls)], -0.
        trade.max_runup, trade.max_drawdown, trade.commission = 3., -4., .5
        trade.entry_bar_index, trade.exit_bar_index = index, index + 1
        trade.is_long, trade.open_at_end = index % 2, int(index == count - 1)
    for index, point in enumerate(curve):
        point.time_ms = ordinary.bars.T0 + index * 60000
        point.equity, point.open_profit = 10000. + index, -0.
    report = run_json.ReportC()
    report.trades, report.trades_len, report.total_trades = trades, count, count
    report.equity_curve, report.equity_curve_len = curve, len(curve)
    report.input_bars_processed = report.script_bars_processed = len(curve)
    report.net_profit = sum(trade.pnl for trade in trades)
    # Explicit witnesses prevent a flat empty/default report from passing.
    assert report.trades_len == count and report.equity_curve_len == 2 * count
    assert trades[-1].open_at_end and curve[-1].equity == 10000. + 2 * count - 1
    return report, trades, curve


def build(runner, report, incarnations=None):
    return runner.build_report_dict(
        report, Path("fixture.csv"), report.input_bars_processed,
        ordinary.bars.T0, ordinary.bars.T0 + (report.input_bars_processed - 1) * 60000,
        1.25, {}, {}, {}, incarnations)


@pytest.mark.parametrize("count", [1, 2, 7, 41])
def test_array_transport_is_the_frozen_bytes(tmp_path, frozen, count):
    report, trades, curve = population(count)
    incarnations = tuple(range(1, count + 1))
    expected = json.dumps(build(frozen, report, incarnations),
                          separators=(",", ":"), allow_nan=False) + "\n"
    with ExitStack() as resources:
        document = build(run_json, report, incarnations)
        run_json._validate_report_result(document)
        artifact = run_json._prepare_stream_report(document, resources, str(tmp_path))
        assert artifact.read() == expected


def test_python_trade_curve_and_serialization_peak_is_flat(tmp_path):
    """Before: per-trade/point dictionaries and serialized bytes grow by MiBs."""
    peaks = []
    for count in (256, 24000):
        report, trades, curve = population(count)
        tracemalloc.start()
        try:
            with ExitStack() as resources:
                document = build(run_json, report)
                run_json._validate_report_result(document)
                artifact = run_json._prepare_stream_report(document, resources, str(tmp_path))
                digest, consumed = hashlib.sha256(), 0
                while chunk := artifact.read(65536):
                    digest.update(chunk.encode("ascii"))
                    consumed += len(chunk)
                assert document["summary"]["total_trades"] == count
                assert consumed > count * 200 and digest.digest() != bytes(32)
                peaks.append(tracemalloc.get_traced_memory()[1])
        finally:
            tracemalloc.stop()
    assert peaks[1] - peaks[0] <= 512 * 1024, peaks


@pytest.mark.parametrize("outputs", [False, True])
def test_ordinary_and_selected_route_bytes(tmp_path, monkeypatch, frozen, outputs):
    # Both ordinary (also the jailed entrypoint's producer) and selected
    # main() execute their actual assembly, cleanup and publication paths.
    extra = ["--outputs"] if outputs else []
    before = ordinary.invoke(tmp_path, monkeypatch, frozen,
                             library=ordinary.SinkLibrary(outputs=outputs), extra=extra)
    after = ordinary.invoke(tmp_path, monkeypatch,
                            library=ordinary.SinkLibrary(outputs=outputs), extra=extra)
    assert before[:3] == after[:3]
    before = selected.invoke(tmp_path, monkeypatch, runner=frozen, outputs=outputs)
    after = selected.invoke(tmp_path, monkeypatch, outputs=outputs)
    assert before[0] == after[0] == 0 and before[2] == after[2] == ""
    assert selected.masked_bytes(before[1]) == selected.masked_bytes(after[1])


def test_columns_do_not_accumulate_python_objects(tmp_path):
    peaks = []
    for count in (256, 24000):
        tracemalloc.start()
        try:
            with ExitStack() as resources:
                columns = [run_json._JSONColumn(resources, str(tmp_path)) for _ in range(3)]
                for index in range(count):
                    columns[0].append(index)
                    columns[1].append(-0.)
                    columns[2].append({"sequence": index, "message": 'quote"\n漢'})
                artifact = run_json._prepare_stream_report(columns, resources, str(tmp_path))
                consumed = 0
                while chunk := artifact.read(65536):
                    consumed += len(chunk)
                assert consumed > count * 40  # all values and events were consumed
                peaks.append(tracemalloc.get_traced_memory()[1])
        finally:
            tracemalloc.stop()
    assert peaks[1] - peaks[0] <= 256 * 1024, peaks


def test_legacy_routes_never_use_a_stringio_report(tmp_path, monkeypatch):
    library = ordinary.bars.Library()
    source = ordinary.bars.csv_file(tmp_path, count=18)
    report, trades, curve = population(9)

    def run(*args):
        ctypes.memmove(ctypes.addressof(args[-1]._obj),
                       ctypes.addressof(report), ctypes.sizeof(report))

    library.run_backtest_full = run
    monkeypatch.setattr(run_json, "load_strategy", lambda path: library)
    stdout, stderr = io.StringIO(), io.StringIO()
    with mock.patch.object(run_json.io, "StringIO", side_effect=AssertionError("whole JSON buffer")):
        with mock.patch.object(sys, "stdout", stdout), mock.patch.object(sys, "stderr", stderr):
            status = run_json.main(["--so", "unused.so", "--ohlcv", str(source)])
    assert status == 0 and stderr.getvalue() == "", stdout.getvalue()
    assert len(json.loads(stdout.getvalue())["trades"]) == 9


def install_output_fixture(library, rows):
    raw = ordinary.outputs_reader().manifest()
    library.message_buffer = ctypes.create_string_buffer('quote"\n漢'.encode())
    library.output_rows, library.output_events, library.largest_copy = rows, rows, 0
    library.strategy_outputs_api_version = lambda: 1
    library.strategy_outputs_set_enabled = lambda *args: 0
    library.strategy_outputs_series_count = lambda state: 2
    library.strategy_outputs_bars_len = lambda state: rows
    library.strategy_outputs_events_len = lambda state: rows

    def manifest(state, out, capacity, required, error, error_size):
        required._obj.value = len(raw) + 1
        if out is None:
            return -1
        ctypes.memmove(out, raw + b"\0", len(raw) + 1)
        return 0

    def times(state, offset, opens, closes, count, copied):
        library.largest_copy = max(library.largest_copy, count)
        for index in range(count):
            opens[index] = ordinary.bars.T0 + (offset + index) * 60000
            closes[index] = opens[index] + 60000
        copied._obj.value = count
        return 0

    def series(state, slot, offset, out, count, copied):
        library.largest_copy = max(library.largest_copy, count)
        for index in range(count):
            out[index] = -0. if slot == 0 else 4294967295.
        copied._obj.value = count
        return 0

    def constants(state, out, count):
        if out is not None:
            out[0] = 50.
        return 1

    def event(state, index, out, size):
        value = out._obj
        value.sequence, value.output_index, value.bar_index = index + 1, 1, index
        value.bar_open_ms = ordinary.bars.T0 + index * 60000
        value.bar_close_ms = value.bar_open_ms + 60000
        value.value, value.phase = float("nan"), 1
        value.message = ctypes.cast(library.message_buffer, ctypes.c_char_p)
        return 0

    library.strategy_outputs_manifest = manifest
    library.strategy_outputs_bar_times_copy = times
    library.strategy_outputs_series_copy = series
    library.strategy_outputs_constants_copy = constants
    library.strategy_outputs_event_get = event


@pytest.mark.parametrize("outputs", [False, True])
def test_legacy_main_assembly_peak_is_flat(tmp_path, monkeypatch, outputs):
    peaks = []
    for count in (256, 24000):
        source = ordinary.bars.csv_file(tmp_path, count=2 * count)
        report, trades, curve = population(count)
        library = ordinary.bars.Library()
        if outputs:
            install_output_fixture(library, 2 * count)
        original_write = run_json._write_report

        def run(*args):
            assert args[2] == 2 * count  # the actual feed population
            ctypes.memmove(ctypes.addressof(args[-1]._obj),
                           ctypes.addressof(report), ctypes.sizeof(report))
            tracemalloc.start()  # immediately after the native result exists

        def publish(artifact, phases=None):
            peaks.append(tracemalloc.get_traced_memory()[1])
            tracemalloc.stop()  # stdout capture is the test consumer's storage
            return original_write(artifact, phases)

        library.run_backtest_full = run
        stdout, stderr = io.StringIO(), io.StringIO()
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "load_strategy", lambda path: library)
            patch.setattr(run_json, "_write_report", publish)
            with mock.patch.object(sys, "stdout", stdout), mock.patch.object(sys, "stderr", stderr):
                try:
                    status = run_json.main(["--so", "unused.so", "--ohlcv", str(source),
                                            *(["--outputs"] if outputs else [])])
                finally:
                    if tracemalloc.is_tracing():
                        tracemalloc.stop()
        document = json.loads(stdout.getvalue())
        assert status == 0 and stderr.getvalue() == "", document
        assert len(document["trades"]) == count
        assert len(document["equity_curve"]) == 2 * count
        if outputs:
            assert len(document["outputs"]["bars"]["open_ms"]) == library.output_rows == 2 * count
            assert len(document["outputs"]["events"]) == library.output_events == 2 * count
            assert len(document["outputs"]["series"][0]["values"]) == 2 * count
            assert library.largest_copy <= 4096
    assert len(peaks) == 2 and peaks[1] - peaks[0] <= 512 * 1024, peaks


def test_selected_legacy_main_assembly_peak_is_flat(tmp_path, monkeypatch):
    window = selected.selected
    peaks = []
    for count in (256, 24000):
        last = window.F + (count + 3) * window.MINUTE
        plan = window.full_plan(
            supplied_input_bars=count + 4, supplied_script_bars=count + 4,
            fed_input_bars=count + 3, fed_script_bars=count + 3,
            window_input_bars=count, window_script_bars=count,
            supplied_last_data_ms=last, fed_last_data_ms=last, window_last_data_ms=last)
        library = window.FakeLib(plan)
        report, trades, curve = population(count)
        native = library.run_backtest_full
        original_write = run_json._write_report

        def run(*args):
            native(*args)
            actual = args[-1]._obj
            actual.trades, actual.trades_len = report.trades, count
            actual.total_trades, actual.net_profit = count, report.net_profit
            assert args[2] == count + 3 and actual.equity_curve_len == count + 1
            tracemalloc.start()

        def publish(artifact, phases=None):
            peaks.append(tracemalloc.get_traced_memory()[1])
            tracemalloc.stop()
            return original_write(artifact, phases)

        library.run_backtest_full = run
        with monkeypatch.context() as patch:
            patch.setattr(window, "E", window.T + count * window.MINUTE)
            patch.setattr(run_json, "_write_report", publish)
            try:
                result = selected.invoke(tmp_path, patch, library=library, plan=plan)
            finally:
                if tracemalloc.is_tracing():
                    tracemalloc.stop()
        assert result[0] == 0 and result[2] == "", result[:3]
        document = json.loads(result[1])
        assert len(document["trades"]) == count
        assert len(document["equity_curve"]) == count + 1
    assert len(peaks) == 2 and peaks[1] - peaks[0] <= 512 * 1024, peaks


def test_borrowed_utf8_is_encoded_in_fixed_chunks(tmp_path):
    # The valid multi-byte sequence and invalid sequence both cross a chunk
    # boundary; replacement and escaping must match the old whole decode.
    raw = b"x" * 4095 + "漢".encode() + b'y"\n' * 5000 + b"\xf0\x80" + b"z" * 8192
    storage = ctypes.create_string_buffer(raw)
    value = run_json._JSONUTF8(ctypes.addressof(storage))
    with ExitStack() as resources:
        artifact = run_json._prepare_stream_report({"message": value}, resources, str(tmp_path))
        assert artifact.read() == json.dumps(
            {"message": raw.decode("utf-8", "replace")}, separators=(",", ":")) + "\n"


def cached_pages(file):
    size = os.fstat(file.fileno()).st_size
    assert size > 0
    pages = (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE
    vector = (ctypes.c_ubyte * pages)()
    with mmap.mmap(file.fileno(), size, access=mmap.ACCESS_COPY) as mapping:
        address = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
        libc = ctypes.CDLL(None, use_errno=True)
        libc.mincore.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)
        assert libc.mincore(address, size, vector) == 0, ctypes.get_errno()
    return sum(bool(value & 1) for value in vector)


def test_artifact_cache_is_bounded_on_the_deployment_backing(tmp_path):
    """Before: flush() leaves essentially every file page in charged cache."""
    chunk = "x" * 65536
    samples = []
    for windows in (32, 384):
        with ExitStack() as resources:
            artifact = run_json._json_temporary(resources, str(tmp_path))
            for _ in range(windows):
                artifact.write(chunk)
            artifact.write("tail")
            artifact.flush()
            assert os.fstat(artifact.fileno()).st_size == windows * len(chunk) + 4
            written_pages = cached_pages(artifact)
            artifact.seek(0)
            consumed = 0
            while part := artifact.read(65536):
                consumed += len(part)
            assert consumed == windows * len(chunk) + 4
            samples.append((written_pages, cached_pages(artifact)))
    # Truncating the padded final disk block may retain one tail page.
    # Check actual residency; an eviction syscall's return value is no proof.
    assert all(max(pair) <= 1 for pair in samples), samples


def test_uncached_sink_readback_preserves_strict_rows(tmp_path):
    path = tmp_path / "uncached.sink"
    records = [["E", ordinary.bars.T0 + i, 10000. + i, -0.] for i in range(5000)]
    counts, digest = ordinary.write_sink(path, records)
    with run_json._open_uncached_sink(path) as reader:
        observed = [row.as_dict() for row in reader]
        assert reader.complete and reader.counts == counts and reader.sha256 == digest
    assert observed == [
        {"time_ms": row[1], "equity": row[2], "open_profit": row[3]} for row in records]
