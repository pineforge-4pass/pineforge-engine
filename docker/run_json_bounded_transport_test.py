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
sys.path.insert(0, str(ROOT / "scripts"))

import measure_run_json_temporary_sink as sink_driver
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


@pytest.mark.parametrize("count", [1, 2, 7, 41, 517])
def test_array_transport_is_the_frozen_bytes(tmp_path, frozen, count):
    report, trades, curve = population(count)
    incarnations = tuple(range(1, count + 1))
    reference = build(frozen, report, incarnations)
    # Revision 3.1 (34f9b3a39f) added the two named forms after
    # this oracle froze. Every population here has a positive sub-day span
    # and finite cagr/calmar; preserve every other byte of the frozen report.
    assert 0 < curve[-1].time_ms - curve[0].time_ms < 86_400_000
    assert report.metrics.equity.cagr == report.metrics.equity.calmar == 0.0
    assert "annualization" not in reference["diagnostics"]
    reference["diagnostics"]["annualization"] = {
        "cagr": "simple_proportional_subday",
        "calmar": "simple_proportional_subday",
    }
    expected = json.dumps(reference,
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
def test_ordinary_and_selected_route_bytes(tmp_path, monkeypatch, outputs):
    # Both ordinary (also the jailed entrypoint's producer) and selected
    # main() execute their actual assembly, cleanup and publication paths.
    extra = ["--outputs"] if outputs else []
    before = ordinary.invoke(tmp_path, monkeypatch,
                             library=ordinary.without_report_sink(
                                 ordinary.SinkLibrary(outputs=outputs)), extra=extra)
    after = ordinary.invoke(tmp_path, monkeypatch,
                            library=ordinary.SinkLibrary(outputs=outputs), extra=extra)
    assert not before[3].installed and after[3].installed
    assert before[0] == after[0] == 0 and before[2] == after[2] == ""
    assert before[:3] == after[:3]
    before = selected.invoke(tmp_path, monkeypatch, outputs=outputs,
                             library=ordinary.without_report_sink(
                                 selected.SelectedSinkLibrary(
                                     selected.selected.full_plan(), outputs=outputs)))
    after = selected.invoke(tmp_path, monkeypatch, outputs=outputs)
    assert not before[3].installed and after[3].installed
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


def report_sink_fixture(tmp_path, *, temporary=False):
    """Exercise the real owner and receipt checks, with mixed physical row order."""
    library = ordinary.SinkLibrary(outputs=True)
    path = tmp_path / "caller-owned.sink"
    if not temporary:
        path.touch()
    sink = run_json._ReportSink(
        library, 1, directory=str(tmp_path), path=None if temporary else path)
    rows = []
    for index in range(5003):
        rows.append(["E", ordinary.bars.T0 + index, 10000. + index, -0.])
        if index in (1, 4096, 5002):
            number = sum(row[0] == "T" for row in rows) + 1
            rows.append(["T", number, "long", ordinary.bars.T0, ordinary.bars.T0 + 1,
                         100., 101., 2., float(number), -0., 3., -4., .5,
                         index, index + 1, False, number, 0, 'quote"\n漢', "", ""])
    rows += [["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0., 4294967295.],
             ["V", 1, 1, 0, ordinary.bars.T0, ordinary.bars.T0 + 1, 0, 1, 1,
              None, "0123456789abcdef", 'tail"\n漢' + "x" * 65537],
             ["K", 50.]]
    library.counts, library.sha = ordinary.write_sink(sink.path, rows, outputs=True)
    report = run_json.ReportC()
    report.total_trades = library.counts["T"]
    assert library.counts == {"E": 5003, "T": 3, "O": 1, "V": 1, "K": 1}
    assert Path(sink.path).stat().st_size > 3 * run_json._BoundedJSONFile.window
    return sink, library, report, rows


def test_report_sink_methods_use_uncached_reader_and_preserve_order(tmp_path, monkeypatch):
    from pf_sink import SinkReader

    sink, library, report, rows = report_sink_fixture(tmp_path)
    with SinkReader(sink.path) as reader:
        expected = list(reader)
        expected_header, expected_counts, expected_sha = reader.header, reader.counts, reader.sha256
    opened = []
    strict_opened, raw_opened, seen_files = [], [], []
    original = run_json._open_uncached_sink

    def open_reader(path, **kwargs):
        reader = original(path, **kwargs)
        if kwargs.get("raw"):
            assert isinstance(reader, run_json._UncachedSinkFile)
            file = reader.file
            assert isinstance(file, io.FileIO)  # buffering=0, no buffered I/O wrapper
            descriptor, position = reader.fileno(), reader.position
            assert descriptor == file.fileno()
            assert position == file.tell() == 0
            # Retain FileIO objects: a new wrapper around a reused strict file
            # must fail, even when that file has already been closed.
            assert all(file is not previous for previous in seen_files)
            assert all(previous.closed or descriptor != previous.fileno()
                       for previous in seen_files)
            raw_opened.append((descriptor, position))
        else:
            assert isinstance(reader, SinkReader)
            assert isinstance(reader._file, run_json._UncachedSinkFile)
            file = reader._file.file
            strict_opened.append(path)
        # Strict readers own buffered files; compare their underlying FileIOs.
        seen_files.append(getattr(file, "raw", file))
        opened.append(path)
        return reader

    monkeypatch.setattr(run_json, "_open_uncached_sink", open_reader)
    try:
        sink.verify(library, 1, report)
        assert (sink.header, sink.counts, sink.sha256) == (
            expected_header, expected_counts, expected_sha)
        for tag in (None, "E", "T", "O", "V", "K", "absent"):
            raw_before = len(raw_opened)
            actual = list(sink.records(tag))
            assert len(raw_opened) == raw_before + (1 if tag in ("T", "E") else 0)
            # The T and E iterations compare raw output in file order.
            assert actual == [row for row in expected if tag is None or row.tag == tag]
        assert [row.tag for row in expected] == [row[0] for row in rows]
        assert expected[-1].values == (50.,)
        last = list(sink.records("E"))[-1]
        assert last.values == (ordinary.bars.T0 + 5002, 15002., -0.)
        assert last.values[-1].hex() == "-0x0.0p+0"
        # Strict: verify + five filters (None/O/V/K/absent).
        # Raw: the T and E filters + the tail E read.
        assert len(strict_opened) == 6
        assert len(raw_opened) == 3
        assert len(opened) == len(strict_opened) + len(raw_opened)
    finally:
        sink.close()
    assert Path(sink.path).read_bytes().endswith(
        ordinary.encoded({"trailer": "pf-report-sink", "version": 1,
                          "counts": library.counts, "sha256": library.sha}))


@pytest.mark.parametrize("method", ["verify", "records"])
@pytest.mark.parametrize("fault", ["hash", "truncated", "extra", "missing_lf"])
def test_report_sink_reader_errors_match_buffered_reader(tmp_path, method, fault):
    from pf_sink import SinkFormatError, SinkReader

    sink, library, report, rows = report_sink_fixture(tmp_path)
    try:
        sink.verify(library, 1, report)
        ordinary.write_sink(sink.path, rows, outputs=True,
                            fault=fault if fault in ("hash", "truncated") else None)
        if fault == "extra":
            with open(sink.path, "ab") as file:
                file.write(b"x")
        elif fault == "missing_lf":
            with open(sink.path, "r+b") as file:
                file.truncate(os.fstat(file.fileno()).st_size - 1)
        with pytest.raises(SinkFormatError) as reference:
            with SinkReader(sink.path) as reader:
                list(reader)
        with pytest.raises(run_json.RunFailure) as actual:
            if method == "verify":
                sink.verify(library, 1, report)
            else:
                list(sink.records())
        assert type(actual.value) is run_json.RunFailure
        assert actual.value.code == "engine_invariant"
        assert str(actual.value) == (
            "the engine's report sink is invalid: " + str(reference.value))
    finally:
        sink.close()
    assert Path(sink.path).exists()  # an error never deletes the caller's sink


@pytest.mark.parametrize("tag", [None, "E", "absent"])
def test_report_sink_changed_after_verification_consumes_the_tail(tmp_path, tag):
    sink, library, report, rows = report_sink_fixture(tmp_path, temporary=True)
    try:
        sink.verify(library, 1, report)
        rows[-1] = ["K", 51.]
        ordinary.write_sink(sink.path, rows, outputs=True)
        observed = []
        with pytest.raises(run_json.RunFailure) as error:
            observed.extend(sink.records(tag))
        assert error.value.code == "engine_invariant"
        assert str(error.value) == (
            "the report sink changed after verification: digest differs"
            if tag == "E" else "the report sink changed after verification")
        assert len(observed) == sum(tag is None or row[0] == tag for row in rows)
        if tag is None:
            assert observed[-1].values == (51.,)
        elif tag == "E":
            assert observed[-1].values[0] == ordinary.bars.T0 + 5002
    finally:
        sink.close()
    assert not Path(sink.path).exists()


def test_json_temporary_is_unbuffered_and_preserves_multichunk_bytes(tmp_path):
    rows = [{"n": index, "message": 'quote"\n漢' + "x" * 513, "value": -0.}
            for index in range(2051)]
    expected = (json.dumps({"rows": rows}, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")
    assert len(expected) > 1024 * 1024 + run_json._BoundedJSONFile.window
    with ExitStack() as resources:
        artifact = run_json._prepare_stream_report(
            {"rows": run_json._JSONRecordArray(lambda: iter(rows))}, resources, str(tmp_path))
        assert isinstance(artifact.file, io.FileIO)
        assert os.fstat(artifact.fileno()).st_size == len(expected)
        actual = bytearray()
        while chunk := artifact.read(65536):
            assert len(chunk) <= 65536
            actual.extend(chunk.encode("ascii"))
        assert actual == expected
        assert bytes(actual).endswith(b'"value":-0.0}]}\n')
        assert not list(tmp_path.iterdir())
    assert artifact.closed and artifact.buffer is None


@pytest.mark.parametrize("failure", [OSError, MemoryError, ValueError, KeyboardInterrupt])
@pytest.mark.parametrize("sink_errors", [False, True])
def test_failed_serialization_closes_unpublished_artifact_immediately(
        tmp_path, monkeypatch, failure, sink_errors):
    sentinel = tmp_path / "user-file"
    sentinel.write_bytes(b"preserve the caller's file\n")
    artifacts = []
    original = run_json._json_temporary

    def temporary(resources, directory):
        file = original(resources, directory)
        artifacts.append((file, file.fileno()))
        return file

    def rows():
        yield "x" * (run_json._BoundedJSONFile.window + 19)
        raise failure("injected after a real write")

    monkeypatch.setattr(run_json, "_json_temporary", temporary)
    expected_error = run_json.RunFailure if sink_errors and failure is OSError else failure
    with ExitStack() as resources:
        with pytest.raises(expected_error) as error:
            run_json._prepare_stream_report(
                {"rows": run_json._JSONRecordArray(rows)}, resources, str(tmp_path),
                sink_errors=sink_errors)
        assert len(artifacts) == 1
        artifact, fd = artifacts[0]
        assert artifact.size > artifact.window
        assert artifact.closed and artifact.buffer is None
        with pytest.raises(OSError):
            os.fstat(fd)
        if expected_error is run_json.RunFailure:
            assert error.value.code == "report_sink_failed"
            assert isinstance(error.value.__cause__, OSError)
        else:
            assert str(error.value) == "injected after a real write"
        assert sentinel.read_bytes() == b"preserve the caller's file\n"
        assert list(tmp_path.iterdir()) == [sentinel]


def test_automatic_sink_midwrite_failure_publishes_only_failure(tmp_path, monkeypatch):
    original = run_json._BoundedJSONFile.write
    artifacts = []

    def fail_after_window(file, text):
        written = original(file, text)
        if file.size > file.window:
            artifacts.append(file)
            raise OSError("injected after a real write")
        return written

    monkeypatch.setattr(run_json._BoundedJSONFile, "write", fail_after_window)
    library = ordinary.SinkLibrary(pnls=tuple(float(i) for i in range(601)))
    status, text, stderr, library = ordinary.invoke(
        tmp_path, monkeypatch, library=library, extra=["--spill-dir", str(tmp_path)])
    assert artifacts and artifacts[0].size > artifacts[0].window
    assert status == 1 and stderr == ""
    assert text == run_json.failure_line(
        "report sink write failed", "report_sink_failed", {"reason": "scratch_io"})
    assert artifacts[0].closed and artifacts[0].buffer is None
    assert library.freed == library.installed
    assert not any(path.exists() for path in map(Path, library.installed))
    assert not list(tmp_path.glob("pineforge-report-sink-*"))


@pytest.mark.parametrize("text,code,args", [
    ('quote"\n漢\udcff', None, None),
    ("x" * 20000, "setting_rejected", {"input": "漢" * 2000, "reason": "unknown_key"}),
    ("\x01" * 20000, "pine_invalid_argument", {"argument": 'quote"\n漢'}),
    ("the real reason", "pine_invalid_argument", {f"a{i}": "\x01" * 1024 for i in range(11)}),
])
def test_failure_only_stringio_keeps_frozen_bytes(frozen, text, code, args):
    actual = run_json.failure_line(text, code, args)
    assert actual == frozen.failure_line(text, code, args)
    assert len(actual.encode("ascii")) <= 60 * 1024 + 1
    document = json.loads(actual)
    assert tuple(document) == (("engine", "error") if code is None
                               else ("engine", "error", "code", "args"))
    assert document["engine"] == "pineforge"


def test_automatic_report_never_calls_failure_stringio(tmp_path, monkeypatch):
    with mock.patch.object(run_json, "_dump_line", side_effect=AssertionError("failure writer in report")):
        status, text, stderr, library = ordinary.invoke(tmp_path, monkeypatch)
    assert status == 0 and stderr == ""
    document = json.loads(text)
    assert len(document["trades"]) == document["summary"]["total_trades"] == 3
    assert len(document["equity_curve"]) == document["input"]["bars"]
    assert library.freed == library.installed


@pytest.mark.parametrize("window", [17, 65536])
def test_driver_census_counts_real_arrays_and_hashes_all_bytes(monkeypatch, frozen, window):
    report, trades, curve = population(517)
    document = build(frozen, report)
    document["outputs"] = {
        "events": [{"message": "x" * 65531 + '\\",]}{"trades":[{}]\n漢' + "y" * 65539}],
        "series": [{"values": [None, -0., 4294967295]}],
    }
    expected = (json.dumps(document, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")
    assert len(expected) > 4 * 65536
    requests = []

    class BoundedInput(io.BytesIO):
        def read(self, count):
            requests.append(count)
            assert 0 < count <= window
            return super().read(count)

    monkeypatch.setattr(sink_driver, "WINDOW", window)
    observed = sink_driver._ReportCensus(BoundedInput(expected)).read()
    assert observed["trades"] == 517 and observed["equity"] == 1034
    assert observed["input_bars"] == document["input"]["bars"] == 1034
    assert observed["report_bytes"] == len(expected)
    assert observed["sha256"] == hashlib.sha256(expected).hexdigest()
    assert len(requests) >= len(expected) // window


@pytest.mark.parametrize("suffix", [b"", b"\n{}\n", b"x"])
def test_driver_census_refuses_truncated_or_extra_documents(suffix):
    raw = b'{"engine":"pineforge","input":{"bars":2},"trades":[],"equity_curve":[{},{}]}'
    # The empty suffix case removes the closing brace instead of appending.
    raw = raw[:-1] if not suffix else raw + suffix
    with pytest.raises(ValueError):
        sink_driver._ReportCensus(io.BytesIO(raw)).read()


def test_driver_help_and_missing_input_are_parser_only(tmp_path, capsys):
    with pytest.raises(SystemExit) as help_exit:
        sink_driver.arguments(["--help"])
    assert help_exit.value.code == 0
    help_text = capsys.readouterr().out
    assert all(name in help_text for name in ("input_csv", "run_label", "output_dir", "--so"))
    with pytest.raises(SystemExit) as missing_argument:
        sink_driver.arguments([])
    assert missing_argument.value.code == 2
    assert "input_csv" in capsys.readouterr().err
    missing = tmp_path / "missing.csv"
    with pytest.raises(SystemExit) as missing_file:
        sink_driver.arguments([str(missing), "missing", str(tmp_path), "--so", __file__])
    assert missing_file.value.code == 2
    assert str(missing) in capsys.readouterr().err
    assert not list(tmp_path.iterdir())


def driver_producer(tmp_path, *, fault=None, has_sink=True):
    """Load the production main in the child; replace native exports only."""
    producer = tmp_path / "producer.py"
    source = ROOT / "docker/run_json.py"
    library = f"SinkLibrary(fault={fault!r})" if has_sink else "bars.Library()"
    producer.write_text(
        "import sys\n"
        f"sys.path[:0] = [{str(ROOT / 'docker')!r}, {str(ROOT / 'tests')!r}]\n"
        f"__file__ = {str(source)!r}\n"
        "with open(__file__, encoding='utf-8') as source:\n"
        "    exec(compile(source.read(), __file__, 'exec'), globals())\n"
        "from test_run_json_sink import SinkLibrary, bars\n"
        f"load_strategy = lambda path: {library}\n",
        encoding="utf-8")
    module = tmp_path / "lab-module.so"
    module.write_bytes(b"native exports replaced by the Python fixture\n")
    return producer, module


def test_driver_measures_real_main_once_and_preserves_published_bytes(tmp_path, monkeypatch, capsys):
    producer, module = driver_producer(tmp_path)
    source = ordinary.bars.csv_file(tmp_path, count=23)
    output = tmp_path / "results"
    status = sink_driver.main([
        str(source), "real-run", str(output), "--so", str(module), "--producer", str(producer),
        "--expect-bars", "23", "--elapsed-seconds", "0"])
    captured = capsys.readouterr()
    assert status == 0 and captured.err == "", captured
    assert len(captured.out.splitlines()) == 1
    measured = json.loads(captured.out)
    raw = (output / "real-run.report.json").read_bytes()
    document = json.loads(raw)
    assert measured["wall_s"] > 0 and measured["peak_rss_mb"] > 0
    assert measured["report_bytes"] == len(raw)
    assert measured["sha256"] == hashlib.sha256(raw).hexdigest()
    assert measured["trades"] == len(document["trades"]) == 3
    assert measured["equity"] == len(document["equity_curve"]) == 23
    assert document["input"]["bars"] == 23 and document["elapsed_seconds"] == 0.
    assert list(output.iterdir()) == [output / "real-run.report.json"]

    library = ordinary.SinkLibrary()
    monkeypatch.setattr(run_json, "load_strategy", lambda path: library)
    monkeypatch.setattr(run_json.time, "time", lambda: 0.)
    stdout, stderr = io.StringIO(), io.StringIO()
    with mock.patch.object(sys, "stdout", stdout), mock.patch.object(sys, "stderr", stderr):
        expected_status = run_json.main([
            "--so", str(module), "--ohlcv", str(source), "--spill-dir", str(tmp_path)])
    assert expected_status == 0 and stderr.getvalue() == ""
    assert raw == stdout.getvalue().encode("ascii")
    assert library.freed == library.installed


@pytest.mark.parametrize("refusal", ["population", "sha256", "producer", "fallback"])
def test_driver_failure_is_nonzero_with_no_report_or_private_artifact(tmp_path, capsys, refusal):
    producer, module = driver_producer(
        tmp_path, fault="hash" if refusal == "producer" else None, has_sink=refusal != "fallback")
    source = ordinary.bars.csv_file(tmp_path, count=23)
    output = tmp_path / "results"
    output.mkdir()
    sentinel = output / "user-file"
    sentinel.write_bytes(b"keep the user's file\n")
    options = (["--expect-bars", "24"] if refusal == "population" else
               ["--expected-sha256", "0" * 64] if refusal == "sha256" else [])
    status = sink_driver.main([
        str(source), "failed-run", str(output), "--so", str(module), "--producer", str(producer),
        "--elapsed-seconds", "0", *options])
    captured = capsys.readouterr()
    assert status != 0 and captured.out == ""
    expected = {"population": "CSV/report population mismatch", "sha256": "report SHA-256 mismatch",
                "producer": "engine_invariant", "fallback": "automatic temporary sink"}
    assert expected[refusal] in captured.err
    assert list(output.iterdir()) == [sentinel]
    assert sentinel.read_bytes() == b"keep the user's file\n"


def test_driver_never_replaces_a_caller_report(tmp_path, capsys):
    source = ordinary.bars.csv_file(tmp_path, count=7)
    report = tmp_path / "existing.report.json"
    report.write_bytes(b"the user's report\n")
    status = sink_driver.main([str(source), "existing", str(tmp_path), "--so", __file__])
    captured = capsys.readouterr()
    assert status != 0 and captured.out == ""
    assert "refusing to replace an existing file" in captured.err
    assert report.read_bytes() == b"the user's report\n"
    assert not list(tmp_path.glob(".existing-*"))
