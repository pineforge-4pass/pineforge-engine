"""Tests for one-pass report assembly (written to fail on the multi-pass wrapper first).

The tests use the existing sink writer and fixture encoders. The only
reference implementation is the byte-pinned 6b33c47f fixture loaded by
run_json_fused_test_support; no product counter or test-only product seam is
required.
"""
from contextlib import contextmanager, ExitStack
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import time
import tracemalloc
import types

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_selected_sink as selected
import test_run_json_sink as ordinary
import test_run_json_sink_summary as summaries
from run_json_trade_scan_test import (
    _event,
    _mixed,
    _padding_before,
    _point,
    _trade,
    _trailer_start,
)
from run_json_bulk_equity_test import BAD_LINES
import run_json_fused_test_support as support
from run_json_fused_test_support import frozen


for _name, _expected_sha in support.FROZEN_FILES:
    _fixture = ROOT / "tests" / "fixtures" / f"{_name}_frozen_6b33c47f_rjone.py"
    assert hashlib.sha256(_fixture.read_bytes()).hexdigest() == _expected_sha, (
        f"ONE_PASS_FROZEN_SOURCE_CHANGED: {_fixture}")


@contextmanager
def _sink_case(tmp_path, rows, *, outputs=False, named=False, fault=None):
    """Create a real product sink with the existing fixture writer."""
    library = ordinary.SinkLibrary(outputs=outputs, fault=fault)
    named_path = None
    if named:
        named_path = Path(tmp_path) / "named-report.sink"
        named_path.touch()
        sink = run_json._ReportSink(library, 1, path=str(named_path))
    else:
        sink = run_json._ReportSink(library, 1, directory=str(tmp_path))
    library.counts, library.sha = ordinary.write_sink(
        sink.path, rows, outputs=outputs)
    report = run_json.ReportC()
    support.set_report(report, rows)
    try:
        yield sink, library, report
    finally:
        sink.close()
        if named_path is not None:
            named_path.unlink(missing_ok=True)


def _rewrite_sink(path, edit, *, library=None, trailer_edit=None):
    """Edit bytes emitted by ordinary.write_sink and re-emit its receipt."""
    raw = Path(path).read_bytes()
    cut = _trailer_start(raw)
    body = raw[:cut]
    trailer = ordinary.json.loads(raw[cut:])
    body = edit(body)
    if library is not None:
        library.sha = hashlib.sha256(body).hexdigest()
    trailer["sha256"] = hashlib.sha256(body).hexdigest()
    if trailer_edit is not None:
        trailer_edit(trailer)
    Path(path).write_bytes(body + ordinary.encoded(trailer))


def _replace_first(body, old, new):
    assert body.count(old) == 1, (old, body.count(old))
    return body.replace(old, new, 1)


@contextmanager
def _output_manifest(runner):
    """Give path-sink output rows the same existing fixture manifest."""
    reader = ordinary.outputs_reader()
    original = runner._outputs_manifest
    runner._outputs_manifest = lambda library, state: reader.manifest()
    try:
        yield
    finally:
        runner._outputs_manifest = original


def _configure_outputs(runner):
    return _output_manifest(runner)


def _synthetic_case(name):
    if name == "empty":
        return [], False, False, False, None
    if name == "e-before-t":
        return [_point(0), _trade(1)], False, False, False, None
    if name == "t-before-e":
        return [_trade(1), _point(0)], False, False, False, None
    if name == "interleaved":
        return [_point(0), _trade(1), _point(1), _trade(2), _point(2)], False, False, False, None
    if name == "escaped":
        return ([_point(0), _trade(1)], False, False, False,
                lambda body: _replace_first(
                    _replace_first(body, b'\n["E",', b'\n["\\u0045",'),
                    b'\n["T",', b'\n["\\u0054",'))
    if name == "numeric-edges":
        return ([_point(0), _trade(1, pnl=-0.0),
                 _point(1), _trade(2, pnl=0.0),
                 _point(2), _trade(3, pnl=1e16),
                 _point(3), _trade(4, pnl=-1e16)],
                False, False, False, None)
    if name == "window-variant":
        return ([_event("p" * (64 * 1024 + 17)),
                 ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0., 1.],
                 ["K", 50.], _point(0), _trade(1)],
                True, False, False, None)
    if name == "batch-boundary":
        rows = [_point(index) for index in range(513)]
        rows.extend(_trade(index + 1, pnl=(-0.0 if index % 2 else 1.0))
                    for index in range(513))
        return rows, False, False, False, None
    if name == "page-boundary":
        rows = [
            _point(0), _event("x" * (64 * 1024 - 97)),
            ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0., 1.],
            ["K", 50.], _trade(1),
        ]
        return rows, True, False, False, None
    if name == "named":
        return [_point(0), _trade(1)], False, True, False, None
    if name == "outputs":
        return _mixed(), True, False, False, None
    raise AssertionError(name)


SYNTHETIC_CASES = (
    "empty", "e-before-t", "t-before-e", "interleaved", "escaped",
    "numeric-edges", "window-variant", "batch-boundary", "page-boundary",
    "named", "outputs",
)


def _iteration_rows(count, *, equity_count=None):
    rows = [_point(index) for index in range(count if equity_count is None else equity_count)]
    pnls = (-0.0, 1e-7, -7.0, 1e16, -1e16, 5e-324)
    for index in range(count):
        rows.append(_trade(
            index + 1, pnl=pnls[index % len(pnls)], pnl_pct=-0.0))
    return rows


def _assert_decoded_rows(actual, expected):
    """Include insertion order, scalar types and signed zero in the comparison."""
    assert type(actual) is type(expected)
    if isinstance(expected, dict):
        assert tuple(actual) == tuple(expected)
        for key in expected:
            _assert_decoded_rows(actual[key], expected[key])
    elif isinstance(expected, list):
        assert len(actual) == len(expected)
        for left, right in zip(actual, expected):
            _assert_decoded_rows(left, right)
    elif isinstance(expected, float):
        assert actual.hex() == expected.hex()
    else:
        assert actual == expected


def _assert_column_iteration(column, expected):
    assert type(column) is run_json._JSONColumn
    assert type(column.file) is run_json._BoundedJSONFile
    assert len(column) == len(expected)
    _assert_decoded_rows(list(column), expected)
    _assert_decoded_rows(list(column), expected)
    left, right = iter(column), iter(column)
    for row in expected:
        _assert_decoded_rows(next(left), row)
        _assert_decoded_rows(next(right), row)
    sentinel = object()
    assert next(left, sentinel) is next(right, sentinel) is sentinel
    # A decoded-row cache is forbidden even if repeatability looks correct.
    assert not any(isinstance(value, (list, dict, tuple, set))
                   for value in vars(column).values())


def _assert_spool_crosses_windows(column, expected):
    window = run_json._BoundedJSONFile.window
    position, crosses = 0, False
    for index, row in enumerate(expected):
        position += bool(index)
        end = position + len(json.dumps(row, separators=(",", ":"), allow_nan=False))
        crosses |= position // window != (end - 1) // window
        position = end
    assert position == column.file.size
    assert position > 3 * window and crosses


def _drain_report_artifact(artifact):
    chunks = []
    while chunk := artifact.read(65536):
        chunks.append(chunk.encode("ascii") if isinstance(chunk, str) else chunk)
    return b"".join(chunks)


@pytest.mark.parametrize("trade_count,equity_count", (
    (0, 0), (0, 4097), (1, 1), (4097, 4097),
), ids=("empty", "no-trades-with-equity", "one", "many"))
@pytest.mark.parametrize("bulk", (False, True), ids=("record-equity", "bulk-equity"))
def test_fused_report_columns_iterate_serialized_rows(
        tmp_path, monkeypatch, trade_count, equity_count, bulk):
    monkeypatch.setattr(run_json, "_BULK_E_ENABLED", bulk)
    before = {}

    def before_prepare(document):
        for key, count in (("trades", trade_count), ("equity_curve", equity_count)):
            column = document[key]
            before[key] = list(column)
            assert len(column) == count

    def consume(artifact, document):
        expected = json.loads(_drain_report_artifact(artifact))
        assert document["trades"] is sink._trades_spool
        assert document["equity_curve"] is sink._equity_spool
        assert expected["summary"]["total_trades"] == sink.counts["T"] == trade_count
        trade_metadata = document.get("metadata", {}).get("trades", {})
        if "logical_count" in trade_metadata:
            assert len(document["trades"]) == trade_metadata["logical_count"]
        for key, tag, count in (
                ("trades", "T", trade_count), ("equity_curve", "E", equity_count)):
            assert len(expected[key]) == sink.counts[tag] == count
            _assert_decoded_rows(before[key], expected[key])
            _assert_column_iteration(document[key], expected[key])
            if count > 1:
                _assert_spool_crosses_windows(document[key], expected[key])

    rows = _iteration_rows(trade_count, equity_count=equity_count)
    with _sink_case(tmp_path, rows) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("key", ("trades", "equity_curve"))
@pytest.mark.parametrize("count", (1, 4097), ids=("one", "many"))
def test_fused_column_iteration_survives_serialization(tmp_path, monkeypatch, key, count):
    captured = []

    def before_prepare(document):
        column = document[key]
        fast, slow = iter(column), iter(column)
        fast_rows, slow_rows = [next(fast)], [next(slow)]
        for _ in range(count // 2):
            fast_rows.append(next(fast))
        assert column.file.position == 0
        read = column.file.read
        sentinel = object()
        reads = 0

        def interleaved_read(size=-1):
            nonlocal reads
            # Short reads keep the serializer inside its cached window while
            # the iterator below reads far enough to replace that window.
            chunk = read(min(size, 8191) if size >= 0 else 8191)
            if chunk:
                reads += 1
                position = column.file.position
                for _ in range(512):
                    row = next(slow, sentinel)
                    if row is sentinel:
                        break
                    slow_rows.append(row)
                # The decoder may replace the shared aligned window, but it
                # must never advance the serializer's sequential cursor.
                assert column.file.position == position
            return chunk

        with monkeypatch.context() as patch, ExitStack() as resources:
            patch.setattr(column.file, "read", interleaved_read)
            artifact = run_json._prepare_stream_report(document, resources, str(tmp_path))
            chunks = []
            while chunk := artifact.read(65536):
                chunks.append(chunk.encode("ascii"))
            during = b"".join(chunks)
        assert reads > 0
        if count > 1:
            assert reads > 3
        position = column.file.position
        fast_rows.extend(fast)
        slow_rows.extend(slow)
        assert column.file.position == position
        captured.append((fast_rows, slow_rows, during))

    def consume(artifact, document):
        serialized = _drain_report_artifact(artifact)
        fast_rows, slow_rows, during = captured[0]
        assert during == serialized
        expected = json.loads(serialized)[key]
        _assert_decoded_rows(fast_rows, expected)
        _assert_decoded_rows(slow_rows, expected)
        _assert_column_iteration(document[key], expected)

    with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("count", (0, 1, 32769), ids=("empty", "one", "many"))
def test_fused_output_columns_iterate_serialized_rows(tmp_path, count):
    rows = []
    values = (-0.0, 1e-7, 5e-324, 1.7976931348623157e308, None)
    for index in range(count):
        start = ordinary.bars.T0 + index
        rows.append(["O", index, start, start + 1,
                     values[index % len(values)], 4294967295.])
        event = _event(f'event {index}, quote"\\\n漢\U0001f680')
        event[1], event[3] = index + 1, index
        if count > 1 and index == count // 2:
            event[-1] += "x" * (2 * run_json._BoundedJSONFile.window + 17)
        rows.append(event)
    rows.append(["K", 50.])

    def consume(artifact, document):
        actual = document["outputs"]
        expected = json.loads(_drain_report_artifact(artifact))["outputs"]
        pairs = [(actual["bars"][key], expected["bars"][key])
                 for key in ("open_ms", "close_ms")]
        pairs.extend((entry["values"], reference["values"])
                     for entry, reference in zip(actual["series"], expected["series"]))
        pairs.append((actual["events"], expected["events"]))
        assert len(actual["series"]) == len(expected["series"]) == 2
        for column, reference in pairs:
            assert len(reference) == count
            _assert_column_iteration(column, reference)
            if count > 1:
                _assert_spool_crosses_windows(column, reference)

    with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report, outputs=True,
            configure=_configure_outputs, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("number", (1e-7, 1.2345), ids=("exponent", "fraction"))
@pytest.mark.parametrize("first", (0.0, 100.0), ids=("numeric-prefix", "incomplete-number"))
def test_fused_output_number_crossing_window_is_not_truncated(tmp_path, number, first):
    # "0.0," is four bytes. Starting four or two bytes before the boundary
    # exposes either a valid numeric prefix ("1e-0"/"1.23") or an incomplete
    # number ("1e"/"1."). Neither may be yielded before its remaining digits.
    padding = run_json._BoundedJSONFile.window // 4 - 1
    values = [0.0] * padding + [number, -0.0]
    values[0] = first
    rows = [["O", index, ordinary.bars.T0, ordinary.bars.T0 + 1, value, 1.]
            for index, value in enumerate(values)] + [["K", 50.]]

    def consume(artifact, document):
        expected = json.loads(_drain_report_artifact(artifact))["outputs"]["series"][0]["values"]
        column = document["outputs"]["series"][0]["values"]
        start = padding * 4 + len(repr(first)) - len(repr(0.0))
        assert start < column.file.window < start + len(repr(number))
        _assert_column_iteration(column, expected)

    with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report, outputs=True,
            configure=_configure_outputs, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


def test_fused_column_iteration_keeps_bounded_memory(tmp_path, monkeypatch):
    class ReadProbe:
        def __init__(self, inner):
            self.inner = inner
            self.calls = self.largest = 0

        def __getattr__(self, name):
            return getattr(self.inner, name)

        def pread(self, fd, address, count, offset):
            assert offset % run_json._BoundedJSONFile.window == 0
            assert address % 4096 == 0
            self.calls += 1
            self.largest = max(self.largest, count)
            return self.inner.pread(fd, address, count, offset)

    peaks = []

    def before_prepare(document):
        probes = []
        with monkeypatch.context() as patch:
            for key in ("trades", "equity_curve"):
                file = document[key].file
                probe = ReadProbe(file.libc)
                probes.append(probe)
                patch.setattr(file, "libc", probe)
            tracemalloc.start()
            try:
                for key, field in (("trades", "n"), ("equity_curve", "time_ms")):
                    seen, previous = 0, None
                    for row in document[key]:
                        if previous is not None:
                            assert row[field] > previous
                        previous = row[field]
                        seen += 1
                    assert seen == count  # empty iteration must not prove flat memory
                    assert len(document[key]) == seen
                    assert not any(isinstance(value, (list, dict, tuple, set))
                                   for value in vars(document[key]).values())
                peaks.append(tracemalloc.get_traced_memory()[1])
            finally:
                tracemalloc.stop()
        assert all(probe.calls and probe.largest == run_json._BoundedJSONFile.window
                   for probe in probes)
        if count > 256:
            assert all(probe.calls > 3 for probe in probes)

    for count in (256, 12000):
        with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
            outcome = support.assemble(
                run_json, sink, library, report, before_prepare=before_prepare)
            assert outcome.failure is None, outcome.failure
    assert peaks[1] - peaks[0] <= 512 * 1024, peaks


@pytest.mark.parametrize("payload", (
    b'{"x":', b'{}{}', b',1', b'1,', b'1,,2', b'1e+',
    b'NaN', b'{"x":Infinity}', b'"\xff"',
    b'{"x":"' + b"x" * (run_json._BoundedJSONFile.window + 17) + b'"',
), ids=("truncated", "missing-comma", "leading-comma", "trailing-comma",
        "double-comma", "incomplete-exponent", "non-json-number",
        "nested-non-json-number", "non-ascii", "truncated-wide-row"))
def test_fused_column_iteration_rejects_malformed_spool(tmp_path, payload):
    with _sink_case(tmp_path, []) as (sink, library, report):
        sink.verify(library, 1, report)
        document = run_json._build_sink_report(*support.report_args(report, sink))
        # Corrupt the real, still-writable spool before its first read.
        document["trades"].append_ascii(payload)
        with pytest.raises(ValueError, match="invalid JSON report column"):
            list(document["trades"])


def _sink_column_rows(sink):
    """Read the independent row oracle before any report spool is frozen."""
    assert not sink._trades_spool.file.reading
    assert not sink._equity_spool.file.reading
    rows = {
        "trades": [run_json._sink_trade(row) for row in sink.records("T")],
        "equity_curve": [row.as_dict() for row in sink.records("E")],
    }
    assert not sink._trades_spool.file.reading
    assert not sink._equity_spool.file.reading
    return rows


def test_fused_columns_reject_iteration_after_resources_close(tmp_path):
    columns = []

    def consume(artifact, document):
        _drain_report_artifact(artifact)
        for key in ("trades", "equity_curve"):
            column = document[key]
            assert len(column) == count
            assert not column.file.closed
            columns.append(column)

    for count in (0, 1):
        with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
            assert isinstance(sink._spool_resources, ExitStack)
            outcome = support.assemble(
                run_json, sink, library, report,
                consume=consume, consume_before_close=True)
            assert outcome.failure is None, outcome.failure
        # Preparation closes the spool ExitStack; _sink_case closes the sink.
        assert sink.closed
    for column in columns:
        assert column.file.closed
        with pytest.raises(
                ValueError, match="iterated after the report resources were closed") as caught:
            list(column)
        assert type(caught.value) is ValueError
        assert str(caught.value) == (
            "a report column cannot be iterated after the report resources were closed")


@pytest.mark.parametrize("trade_count,equity_count", (
    (0, 0), (0, 4097), (1, 1), (4097, 4097),
), ids=("empty", "no-trades-with-equity", "one", "many"))
@pytest.mark.parametrize("bulk", (False, True), ids=("record-equity", "bulk-equity"))
def test_fused_report_columns_match_sink_rows(
        tmp_path, monkeypatch, trade_count, equity_count, bulk):
    monkeypatch.setattr(run_json, "_BULK_E_ENABLED", bulk)
    expected = {}

    def before_prepare(document):
        expected.update(_sink_column_rows(sink))
        for key in ("trades", "equity_curve"):
            _assert_decoded_rows(list(document[key]), expected[key])
            assert document[key].file.reading

    def consume(artifact, document):
        serialized = json.loads(_drain_report_artifact(artifact))
        for key, count in (("trades", trade_count), ("equity_curve", equity_count)):
            assert len(expected[key]) == count
            _assert_decoded_rows(serialized[key], expected[key])

    with _sink_case(
            tmp_path, _iteration_rows(trade_count, equity_count=equity_count)
    ) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("kind", ("ascii", "batch"))
@pytest.mark.parametrize("count", (0, 1), ids=("empty", "one"))
def test_fused_column_zero_row_appends_preserve_spool(tmp_path, kind, count):
    expected, spools = {}, {}
    batch = run_json._BulkEquity(b"", 0, None, None)
    assert run_json._bulk_equity(b"") is None
    assert run_json._bulk_equity_ascii(batch) == b""

    def before_prepare(document):
        expected.update(_sink_column_rows(sink))
        keys = ("trades", "equity_curve") if kind == "ascii" else ("equity_curve",)
        for key in keys:
            column = document[key]
            file = column.file
            assert file.base == 0 and file.size == file.used < file.window
            before = file.buffer[:file.used]
            length, empty = len(column), column.empty
            assert not file.reading
            if kind == "ascii":
                column.append_ascii(b"", row_count=0)
            else:
                sink._append_equity_batch(batch)
                assert not sink._equity_spool_failed
                assert sink._spool_failure is None
            assert len(column) == length
            assert file.size == file.used == len(before)
            assert file.buffer[:file.used] == before
            assert column.empty is empty
            assert not file.reading
            spools[key] = before

    def consume(artifact, document):
        serialized = json.loads(_drain_report_artifact(artifact))
        for key in ("trades", "equity_curve"):
            _assert_decoded_rows(serialized[key], expected[key])
        for key, before in spools.items():
            column = document[key]
            assert column.file.read_at(0, column.file.window).encode("ascii") == before
            _assert_decoded_rows(list(column), expected[key])

    with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("trade_count,equity_count", (
    (0, 0), (0, 257), (1, 1), (257, 257),
), ids=("empty", "no-trades-with-equity", "one", "many"))
@pytest.mark.parametrize("bulk", (False, True), ids=("record-equity", "bulk-equity"))
def test_fused_report_column_index_contract(
        tmp_path, monkeypatch, trade_count, equity_count, bulk):
    monkeypatch.setattr(run_json, "_BULK_E_ENABLED", bulk)
    expected = {}

    def before_prepare(document):
        expected.update(_sink_column_rows(sink))
        for key in ("trades", "equity_curve"):
            column, rows = document[key], expected[key]
            assert type(column) is run_json._JSONColumn
            assert isinstance(column, run_json._JSONRecordArray)
            length = len(rows)
            assert len(column) == length
            for index in (None, 0.0, "0", slice(None), slice(0, 1)):
                with pytest.raises(TypeError) as caught:
                    column[index]
                assert type(caught.value) is TypeError
                assert str(caught.value) == "a streamed array has no random access"
            invalid = [length, -length - 1]
            if not length:
                invalid.extend((0, -1, length - 1))
            invalid.extend(index for index in (False, True) if int(index) >= length)
            for index in invalid:
                normalized = index + length if index < 0 else index
                with pytest.raises(IndexError) as caught:
                    column[index]
                assert type(caught.value) is IndexError
                assert caught.value.args == (normalized,)
                assert type(caught.value.args[0]) is type(normalized)
            if length:
                for index in (0, -1, length - 1, False):
                    _assert_decoded_rows(column[index], rows[index])
                if length > 1:
                    _assert_decoded_rows(column[True], rows[True])
                rng = random.Random(0)
                walks = (
                    range(length), range(length - 1, -1, -1),
                    [rng.randrange(length) for _ in range(32)],
                    [length // 2] * 3,
                )
                for walk in walks:
                    for index in walk:
                        _assert_decoded_rows(column[index], rows[index])
            assert not any(isinstance(value, (list, dict, tuple, set))
                           for value in vars(column).values())

    def consume(artifact, document):
        serialized = json.loads(_drain_report_artifact(artifact))
        for key in ("trades", "equity_curve"):
            _assert_decoded_rows(serialized[key], expected[key])
            _assert_decoded_rows(
                [document[key][index] for index in range(len(expected[key]))],
                serialized[key])

    with _sink_case(
            tmp_path, _iteration_rows(trade_count, equity_count=equity_count)
    ) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("count", (0, 1), ids=("empty", "one"))
def test_fused_output_columns_keep_random_access_disabled(tmp_path, count):
    rows = _mixed() if count else [["K", 50.]]

    def consume(artifact, document):
        _drain_report_artifact(artifact)
        block = document["outputs"]
        columns = [block["bars"]["open_ms"], block["bars"]["close_ms"], block["events"]]
        columns.extend(entry["values"] for entry in block["series"])
        for column in columns:
            assert type(column) is run_json._JSONColumn
            assert isinstance(column, run_json._JSONRecordArray)
            assert len(column) == count
            for index in (0, -1, len(column), False, True, slice(None)):
                with pytest.raises(TypeError) as caught:
                    column[index]
                assert type(caught.value) is TypeError
                assert str(caught.value) == "a streamed array has no random access"

    with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report, outputs=True,
            configure=_configure_outputs, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("key", ("trades", "equity_curve"))
@pytest.mark.parametrize("count", (1, 4097), ids=("one", "many"))
def test_fused_column_index_interleaves_iteration_and_serialization(
        tmp_path, monkeypatch, key, count):
    captured = []

    def before_prepare(document):
        expected = _sink_column_rows(sink)[key]
        column = document[key]
        iterator = iter(column)
        seen = [next(iterator)]
        for index in (count // 2, 0, count - 1):
            _assert_decoded_rows(column[index], expected[index])
            assert column.file.position == 0
        read, reads = column.file.read, 0
        sentinel = object()

        def interleaved_read(size=-1):
            nonlocal reads
            chunk = read(min(size, 8191) if size >= 0 else 8191)
            if chunk:
                position = column.file.position
                indexes = (count - 1, 0, count // 2)
                index = indexes[reads % len(indexes)]
                _assert_decoded_rows(column[index], expected[index])
                reads += 1
                for _ in range(512):
                    row = next(iterator, sentinel)
                    if row is sentinel:
                        break
                    seen.append(row)
                assert column.file.position == position
            return chunk

        with monkeypatch.context() as patch, ExitStack() as resources:
            patch.setattr(column.file, "read", interleaved_read)
            artifact = run_json._prepare_stream_report(document, resources, str(tmp_path))
            chunks = []
            while chunk := artifact.read(65536):
                chunks.append(chunk.encode("ascii"))
            during = b"".join(chunks)
        assert reads > 0
        if count > 1:
            assert reads > 3
        position = column.file.position
        seen.extend(iterator)
        for index in (0, count - 1, 0):
            _assert_decoded_rows(column[index], expected[index])
        assert column.file.position == position
        captured.append((expected, seen, during))

    def consume(artifact, document):
        expected, seen, during = captured[0]
        column = document[key]
        iterator, sentinel = iter(column), object()
        chunks, drained_rows = [], []
        reads = 0
        while chunk := artifact.read(8191):
            chunks.append(chunk.encode("ascii") if isinstance(chunk, str) else chunk)
            position = column.file.position
            for _ in range(512):
                row = next(iterator, sentinel)
                if row is sentinel:
                    break
                drained_rows.append(row)
            _assert_decoded_rows(column[reads % count], expected[reads % count])
            assert column.file.position == position
            reads += 1
        assert reads > 0
        if count > 1:
            assert reads > 3
        position = column.file.position
        drained_rows.extend(iterator)
        assert column.file.position == position
        raw = b"".join(chunks)
        assert during == raw
        serialized = json.loads(raw)[key]
        _assert_decoded_rows(serialized, expected)
        _assert_decoded_rows(seen, serialized)
        _assert_decoded_rows(drained_rows, serialized)
        _assert_decoded_rows(document[key][-1], serialized[-1])

    with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


def test_fused_column_forward_index_timing(tmp_path, capsys):
    count, factor = 25000, 8
    rows = [_point(0)] + [_trade(index + 1) for index in range(count)]
    sink_rows = {}

    def before_prepare(document):
        sink_rows.update(_sink_column_rows(sink))

    def consume(artifact, document):
        column = document["trades"]
        expected = json.loads(_drain_report_artifact(artifact))["trades"]
        assert len(column) == len(expected) == count
        _assert_decoded_rows(expected, sink_rows["trades"])
        _assert_decoded_rows(column[0], expected[0])
        _assert_decoded_rows(column[-1], expected[-1])
        sequential, indexed = [], []
        for _ in range(3):
            start = time.perf_counter()
            for row in column:
                pass
            sequential.append(time.perf_counter() - start)
            start = time.perf_counter()
            for index in range(len(column)):
                column[index]
            indexed.append(time.perf_counter() - start)
        sequential_time, indexed_time = min(sequential), min(indexed)
        ratio = indexed_time / sequential_time
        with capsys.disabled():
            print(
                f"JSON column forward walk: iteration={sequential_time:.6f}s "
                f"indexing={indexed_time:.6f}s ratio={ratio:.3f} limit={factor}",
                flush=True)
        assert indexed_time < factor * sequential_time
        _assert_decoded_rows(column[count // 2], expected[count // 2])
        assert not any(isinstance(value, (list, dict, tuple, set))
                       for value in vars(column).values())

    with _sink_case(tmp_path, rows) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            before_prepare=before_prepare, consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("count", (0, 1), ids=("empty", "one"))
def test_fused_report_column_truthiness(tmp_path, count):
    def consume(artifact, document):
        serialized = json.loads(_drain_report_artifact(artifact))
        for key in ("trades", "equity_curve"):
            assert bool(document[key]) is bool(serialized[key]) is bool(count)

    with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("key", ("trades", "equity_curve"))
@pytest.mark.parametrize("count", (0, 1, 4097), ids=("empty", "one", "many"))
def test_fused_column_iterators_are_fresh_and_independent(tmp_path, key, count):
    def consume(artifact, document):
        column, expected = document[key], json.loads(_drain_report_artifact(artifact))[key]
        assert type(column) is run_json._JSONColumn
        assert isinstance(column, run_json._JSONRecordArray)
        left, right = iter(column), iter(column)
        assert left is not right
        _assert_decoded_rows(list(left), expected)
        _assert_decoded_rows(list(right), expected)
        left, right = iter(column), iter(column)
        assert left is not right
        left_rows, right_rows = [], []
        for _ in expected:
            left_rows.append(next(left))
            right_rows.append(next(right))
        sentinel = object()
        assert next(left, sentinel) is next(right, sentinel) is sentinel
        _assert_decoded_rows(left_rows, expected)
        _assert_decoded_rows(right_rows, expected)
        if count:
            _assert_decoded_rows(next(iter(column)), expected[0])
            _assert_decoded_rows(next(iter(column)), expected[0])
        else:
            assert next(iter(column), sentinel) is sentinel
            assert next(iter(column), sentinel) is sentinel
        # Init-bypassing callers supply only .file, including for an empty spool.
        bare = object.__new__(run_json._JSONColumn)
        bare.file = column.file
        _assert_decoded_rows(list(bare), expected)

    with _sink_case(tmp_path, _iteration_rows(count)) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report,
            consume=consume, consume_before_close=True)
        assert outcome.failure is None, outcome.failure


def test_fused_frozen_oracle_uses_frozen_pf_sink(tmp_path, monkeypatch, frozen):
    assert frozen.pf_sink is frozen._rjone_pf_sink
    assert frozen.pf_sink is not pf_sink
    monkeypatch.setattr(
        pf_sink, "_parse",
        lambda line: (_ for _ in ()).throw(AssertionError("product pf_sink used by oracle")),
    )
    with _sink_case(tmp_path, [_point(0)], outputs=False) as (sink, library, report):
        outcome = support.assemble(frozen, sink, library, report)
    assert outcome.failure is None
    assert outcome.report.endswith(b"\n")


@pytest.mark.parametrize("name", SYNTHETIC_CASES)
def test_fused_report_bytes_synthetic(tmp_path, frozen, name):
    rows, outputs, named, window_anchor, edit = _synthetic_case(name)
    with _sink_case(tmp_path, rows, outputs=outputs, named=named) as (sink, library, report):
        if edit is not None:
            _rewrite_sink(sink.path, edit, library=library)
        configure = _configure_outputs if outputs else None
        expected = support.assemble(
            frozen, sink, library, report, outputs=outputs,
            window_anchor=window_anchor, configure=configure)
        actual = support.assemble(
            run_json, sink, library, report, outputs=outputs,
            window_anchor=window_anchor, configure=configure)
        support.assert_same(actual, expected)


@pytest.mark.parametrize("kind", ("canonical", "fallback", "zero-trades", "trailer-window"))
def test_fused_sink_read_once(tmp_path, monkeypatch, kind):
    if kind == "canonical":
        rows, outputs, edit = [_point(0), _trade(1)], False, None
    elif kind == "fallback":
        rows, outputs, edit = [_point(0), _trade(1)], False, (
            lambda body: _replace_first(body, b'["E",', b'["\\u0045",'))
    elif kind == "zero-trades":
        rows, outputs, edit = [_point(0), _event("other" * 32)], True, None
    else:
        rows = [_padding_before(
            64 * 1024 - 17, following=[_point(0), _trade(1)]),
                _point(0), _trade(1)]
        outputs, edit = True, None
    with _sink_case(tmp_path, rows, outputs=outputs) as (sink, library, report):
        if edit is not None:
            _rewrite_sink(sink.path, edit, library=library)
        raw = Path(sink.path).read_bytes()
        audit = support.SinkIO(sink.path)
        with audit.install(monkeypatch):
            outcome = support.assemble(run_json, sink, library, report)
        assert outcome.failure is None
        audit.assert_one_pass(raw)
        assert sink.counts["E"] >= 1
        assert sink.counts["T"] == (0 if kind == "zero-trades" else 1)
        assert (b'["T",' in raw) == (kind != "zero-trades")


def test_fused_sink_read_once_hostile_bound(tmp_path, monkeypatch):
    rows = [_point(0), _event("x" * (70 * 1024)), _trade(1)]
    with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
        audit = support.SinkIO(sink.path)
        with audit.install(monkeypatch):
            outcome = support.assemble(run_json, sink, library, report)
        assert outcome.failure is None
        assert len(audit.opens) <= 1
        assert not audit.seeks


@pytest.mark.parametrize("mode", ("outputs", "named", "selected-window"))
def test_fused_sink_read_once_entrypoints(tmp_path, monkeypatch, mode):
    """Audit main(), including output preparation and selected anchor checks."""
    captured = []
    original_verify = run_json._ReportSink.verify
    with ExitStack() as scopes, monkeypatch.context() as patch:
        def verify(sink, *args, **kwargs):
            assert not captured, "the CLI row must verify exactly one sink"
            # Native fixture generation may inspect its own file. Start the
            # audit at the product boundary and keep it through publication.
            raw = Path(sink.path).read_bytes()
            audit = support.SinkIO(sink.path)
            scopes.enter_context(audit.install(monkeypatch))
            captured.append((audit, raw))
            return original_verify(sink, *args, **kwargs)

        patch.setattr(run_json._ReportSink, "verify", verify)
        if mode == "outputs":
            result = ordinary.invoke(
                tmp_path, monkeypatch, library=ordinary.SinkLibrary(outputs=True),
                extra=["--outputs"], forbid_output_arrays=True)
        elif mode == "named":
            result = ordinary.invoke(
                tmp_path, monkeypatch, library=summaries.SummaryLibrary(),
                extra=["--report-sink", str(tmp_path / "named-cli.sink")])
        else:
            result = selected.invoke(tmp_path, monkeypatch)
    status, text, stderr = result[:3]
    assert (status, stderr) == (0, ""), text
    assert len(captured) == 1
    audit, raw = captured[0]
    audit.assert_one_pass(raw)
    document = json.loads(text)
    if mode == "outputs":
        assert "outputs" in document
    elif mode == "named":
        assert document["report_sink"]["path"] == str(tmp_path / "named-cli.sink")
    else:
        assert {"report_window", "report_shape"} <= document.keys()


def test_fused_report_bytes_interleaved_trade_and_equity_spools(
        tmp_path, monkeypatch, frozen):
    rows = [_point(0), _trade(1), _point(1), _trade(2), _point(2)]
    with _sink_case(tmp_path, rows, outputs=False) as (sink, library, report):
        expected = support.assemble(frozen, sink, library, report)
        raw = Path(sink.path).read_bytes()
        audit = support.SinkIO(sink.path)
        with audit.install(monkeypatch):
            actual = support.assemble(run_json, sink, library, report)
        support.assert_same(actual, expected)
        audit.assert_one_pass(raw)


@pytest.mark.parametrize("change", ("replace-inode", "grow", "shrink", "mtime"))
def test_fused_end_identity_recheck(tmp_path, monkeypatch, change):
    rows = [_point(0), _trade(1)]
    with _sink_case(tmp_path, rows, outputs=False, named=True) as (sink, library, report):
        path = Path(sink.path)
        raw = path.read_bytes()
        original = path.stat()
        changed = False

        def mutate(fd, offset, data):
            nonlocal changed
            if changed or not data or offset + len(data) < len(raw):
                return
            assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY
            changed = True
            if change == "replace-inode":
                replacement = Path(tmp_path) / "replacement.sink"
                replacement.write_bytes(raw)
                os.replace(replacement, path)
            elif change == "grow":
                with path.open("ab") as file:
                    file.write(b"x")
            elif change == "shrink":
                with path.open("r+b") as file:
                    file.truncate(len(raw) - 1)
            else:
                os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns + 1_000_000))
            if change != "mtime":
                os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))

        audit = support.SinkIO(path)
        with audit.install(monkeypatch, after_read=mutate):
            outcome = support.assemble(run_json, sink, library, report)
        assert changed
        status = path.stat()
        assert (status.st_ino, status.st_size, status.st_mtime_ns) != (
            original.st_ino, original.st_size, original.st_mtime_ns)
        if change == "replace-inode":
            assert status.st_ino != original.st_ino
            assert (status.st_size, status.st_mtime_ns) == (
                original.st_size, original.st_mtime_ns)
        assert audit.files and all(file.closed for file in audit.files)
        assert outcome.report == b""
        assert outcome.failure is not None
        assert outcome.failure[2] == (
            "the report sink changed after verification: file identity differs")
        assert outcome.failure[3] == "engine_invariant"


def test_fused_format_error_precedes_end_identity_change(tmp_path, monkeypatch, frozen):
    rows = [_point(0), _trade(1)]
    with _sink_case(tmp_path, rows, outputs=False, named=True) as (sink, library, report):
        path = Path(sink.path)
        _rewrite_sink(
            path, lambda body: _replace_first(body, b'["T",', b'["?",'),
            library=library)
        raw, original = path.read_bytes(), path.stat()

        def run(runner):
            changed = []

            def mutate(_fd, offset, data):
                if not changed and data and offset + len(data) == len(raw):
                    changed.append(True)
                    os.utime(path, ns=(
                        original.st_atime_ns, original.st_mtime_ns + 1_000_000))

            os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
            audit = support.SinkIO(path)
            with audit.install(monkeypatch, runner=runner, after_read=mutate):
                outcome = support.assemble(runner, sink, library, report)
            assert changed == [True], "the format refusal must compete with an identity change"
            assert path.stat().st_mtime_ns != original.st_mtime_ns
            return outcome

        expected, actual = run(frozen), run(run_json)
        support.assert_same(actual, expected)
        assert actual.failure is not None
        assert actual.failure[3] == "engine_invariant"
        assert "record tag is absent from the header" in actual.failure[2]
        assert "file identity differs" not in actual.failure[2]


def test_fused_same_size_same_inode_rewrite_after_read_is_accepted(tmp_path, frozen):
    rows = [_point(0), _trade(1)]
    with _sink_case(tmp_path, rows, outputs=False) as (sink, library, report):
        expected = support.assemble(frozen, sink, library, report)
        assert expected.failure is None
        original = Path(sink.path).stat()

        def rewrite_after_pass(target, _native):
            path = Path(target.path)
            with path.open("r+b") as file:
                raw = path.read_bytes()
                position = raw.index(b"100.0")
                file.seek(position)
                file.write(b"101.0")
            # Closing flushes the buffered write, which updates mtime on CLOUD.
            # Restore the timestamp only after that write has reached the file.
            os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
            status = path.stat()
            assert (status.st_ino, status.st_size, status.st_mtime_ns) == (
                original.st_ino, original.st_size, original.st_mtime_ns)

        outcome = support.assemble(
            run_json, sink, library, report, after_verify=rewrite_after_pass)
        assert outcome.failure is None
        assert outcome.report == expected.report


@pytest.mark.parametrize("kind", ("canonical", "noncanonical"))
def test_fused_first_e_type_normalization(tmp_path, kind):
    rows = [_point(0), _trade(1)]
    with _sink_case(tmp_path, rows, outputs=False) as (sink, library, report):
        if kind == "noncanonical":
            _rewrite_sink(
                sink.path, lambda body: _replace_first(body, b'["E",', b'["\\u0045",'),
                library=library)
        with pf_sink.SinkReader(sink.path) as reader:
            first = next(record for record in reader if record.tag == "E")
        assert type(first) is pf_sink.Record
        expected = first.as_dict()
        outcome = support.assemble(run_json, sink, library, report)
        assert outcome.failure is None
        assert type(sink.diagnostics.first) is dict
        assert sink.diagnostics.first == expected


def test_fused_encoded_artifact_write_error_retains_code(tmp_path, monkeypatch):
    rows = [_point(0), _trade(1)]
    attempted = []
    original = run_json._BoundedJSONFile.write_ascii

    def fail(file, data):
        if b'"time_ms":' in data and not attempted:
            attempted.append(bytes(data))
            raise OSError(errno.ENOSPC, "injected equity spool full")
        return original(file, data)

    monkeypatch.setattr(run_json._BoundedJSONFile, "write_ascii", fail)
    with _sink_case(tmp_path, rows, outputs=False) as (sink, library, report):
        outcome = support.assemble(run_json, sink, library, report)
    assert attempted
    assert outcome.report == b""
    assert outcome.failure is not None
    assert outcome.failure[2] == "report sink write failed"
    assert outcome.failure[3] == "report_sink_failed"
    assert outcome.failure[4] == {"reason": "scratch_io"}


TRACEMALLOC_SLACK = 4 * 1024 * 1024


def _retaining_assembler(sink, tag):
    """Deliberate mutant: replace a streamed array with all decoded dictionaries."""
    def retain(document):
        with pf_sink.SinkReader(sink.path) as reader:
            document["equity_curve" if tag == "E" else "trades"] = [
                record.as_dict() if tag == "E" else run_json._sink_trade(record)
                for record in reader if record.tag == tag
            ]
        assert len(document["equity_curve" if tag == "E" else "trades"]) == sink.counts[tag]
    return retain


def _assembly_peak(tmp_path, rows, *, retain_tag=None):
    with _sink_case(tmp_path, rows, outputs=False) as (sink, library, report):
        consumed, final_byte = 0, b""

        def drain(artifact, document):
            nonlocal consumed, final_byte
            assert document["summary"]["total_trades"] == library.counts["T"]
            while chunk := artifact.read(65536):
                consumed += len(chunk)
                final_byte = chunk[-1:].encode("ascii") if isinstance(chunk, str) else chunk[-1:]

        before_prepare = _retaining_assembler(sink, retain_tag) if retain_tag else None
        tracemalloc.start()
        try:
            outcome = support.assemble(
                run_json, sink, library, report,
                before_prepare=before_prepare, consume=drain)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
    assert outcome.failure is None
    assert outcome.report == b""
    assert consumed > 40 * library.counts["E"] + 200 * library.counts["T"]
    assert final_byte == b"\n"
    return peak


def _assert_flat_memory(peaks):
    assert max(peaks.values()) - min(peaks.values()) <= TRACEMALLOC_SLACK, (
        f"ONE_PASS_RETAINED_ROWS: {peaks}")
    assert peaks[50000] - peaks[1000] <= TRACEMALLOC_SLACK, (
        f"ONE_PASS_RETAINED_ROWS: {peaks}")


def test_fused_flat_equity_memory(tmp_path):
    warm = Path(tmp_path) / "warm"
    warm.mkdir()
    _assembly_peak(warm, [_point(0), _trade(1)])
    peaks, retaining_peaks = {}, {}
    for count in (1000, 10000, 50000):
        directory = Path(tmp_path) / f"equity-{count}"
        directory.mkdir()
        rows = [_point(index) for index in range(count)] + [_trade(1)]
        peaks[count] = _assembly_peak(directory, rows)
        retaining_peaks[count] = _assembly_peak(directory, rows, retain_tag="E")
    with pytest.raises(AssertionError, match="ONE_PASS_RETAINED_ROWS"):
        _assert_flat_memory(retaining_peaks)
    _assert_flat_memory(peaks)


def test_fused_flat_trades_memory(tmp_path):
    warm = Path(tmp_path) / "warm-trades"
    warm.mkdir()
    _assembly_peak(warm, [_point(0), _trade(1)])
    peaks, retaining_peaks = {}, {}
    for count in (1000, 10000, 50000):
        directory = Path(tmp_path) / f"trades-{count}"
        directory.mkdir()
        rows = [_point(0)] + [
            _trade(index + 1, pnl=(-0.0 if index % 3 == 0 else 1.0))
            for index in range(count)
        ]
        peaks[count] = _assembly_peak(directory, rows)
        retaining_peaks[count] = _assembly_peak(directory, rows, retain_tag="T")
    with pytest.raises(AssertionError, match="ONE_PASS_RETAINED_ROWS"):
        _assert_flat_memory(retaining_peaks)
    _assert_flat_memory(peaks)


def _unsafe_finish(reader, value, runner):
    """Test-only control: accept trailer shape and EOF, skip count and SHA."""
    runner.pf_sink._keys(
        value, ("trailer", "version", "counts", "sha256"), "trailer")
    runner.pf_sink._version(value["version"])
    if reader._file is None or reader._file.read(1):
        raise runner.pf_sink.SinkFormatError("bytes after the trailer")
    reader.close()
    reader.trailer = value
    reader.sha256 = value["sha256"]
    reader.complete = True


@pytest.mark.parametrize(
    "mutation",
    ("same-size-before-window", "valid-replacement-receipt",
     "malformed-t-versus-digest", "trailer-count"),
)
def test_fused_mutation_contract(tmp_path, monkeypatch, frozen, mutation):
    rows = [_point(0), _trade(1, entry_id="trade-a")]
    with _sink_case(tmp_path, rows, named=True) as (sink, library, report):
        path = Path(sink.path)
        raw, original = path.read_bytes(), path.stat()
        cut = _trailer_start(raw)
        body, trailer = raw[:cut], json.loads(raw[cut:])
        if mutation == "trailer-count":
            trailer["counts"]["E"] += 1
        elif mutation == "malformed-t-versus-digest":
            body = _replace_first(body, b'["T",', b"?")
        else:
            body = _replace_first(body, b"trade-a", b"trade-b")
            if mutation == "valid-replacement-receipt":
                # A complete replacement trailer is valid on its own; the
                # native receipt still authenticates this run's original sink.
                trailer["sha256"] = hashlib.sha256(body).hexdigest()
        path.write_bytes(body + ordinary.encoded(trailer))
        os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
        if mutation != "malformed-t-versus-digest":
            status = path.stat()
            assert (status.st_ino, status.st_size, status.st_mtime_ns) == (
                original.st_ino, original.st_size, original.st_mtime_ns)
        if mutation == "valid-replacement-receipt":
            with frozen.pf_sink.SinkReader(path) as reader:
                for _ in reader:
                    pass
                assert reader.complete and reader.sha256 != library.sha

        expected = support.assemble(frozen, sink, library, report)
        actual = support.assemble(run_json, sink, library, report)
        support.assert_same(actual, expected)
        assert actual.failure is not None
        assert actual.failure[3] == "engine_invariant"

        mutant = support.load_frozen_copy("rjone_finish_mutant")
        with monkeypatch.context() as patch:
            patch.setattr(
                mutant.pf_sink.SinkReader, "_finish",
                lambda reader, value: _unsafe_finish(reader, value, mutant))
            broken = support.assemble(mutant, sink, library, report)
        if mutation in ("same-size-before-window", "trailer-count"):
            assert broken.failure is None, "the count/SHA-skipping control must be unsafe"
            assert broken.report != b""
            with pytest.raises(AssertionError):
                support.assert_same(broken, expected)
        else:
            support.assert_same(broken, expected)
        if mutation == "same-size-before-window":
            assert "trailer SHA-256 mismatch" in actual.failure[2]
        elif mutation == "trailer-count":
            assert "trailer count mismatch for E" in actual.failure[2]
        elif mutation == "malformed-t-versus-digest":
            assert "invalid UTF-8 or JSON" in actual.failure[2]
        else:
            assert actual.failure[2] == "the report sink trailer differs from its engine receipt"
            # A fresh native receipt makes it a different, valid run. Pin that
            # acceptance too, rather than demanding refusal of valid input.
            library.sha = trailer["sha256"]
            fresh_expected = support.assemble(frozen, sink, library, report)
            fresh_actual = support.assemble(run_json, sink, library, report)
            assert fresh_expected.failure is None
            support.assert_same(fresh_actual, fresh_expected)


def test_fused_mutation_contract_valid_replacement_receipt(tmp_path, monkeypatch, frozen):
    """A valid new native receipt cannot authenticate bytes already consumed."""
    rows = [_point(0), _trade(1, entry_id="trade-a")]
    with _sink_case(tmp_path, rows, named=True) as (sink, library, report):
        path = Path(sink.path)
        raw, original = path.read_bytes(), path.stat()
        original_sha = library.sha
        cut = _trailer_start(raw)
        body = _replace_first(raw[:cut], b"trade-a", b"trade-b")
        trailer = json.loads(raw[cut:])
        replacement_sha = trailer["sha256"] = hashlib.sha256(body).hexdigest()
        replacement = body + ordinary.encoded(trailer)
        assert len(replacement) == len(raw) and replacement_sha != original_sha

        def run(runner):
            path.write_bytes(raw)
            os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
            library.sha = original_sha
            changed = []

            def mutate(_fd, offset, data):
                if not changed and data and offset + len(data) == len(raw):
                    # The read buffer still holds the original complete sink.
                    # Both receipts now describe the valid replacement instead.
                    path.write_bytes(replacement)
                    os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))
                    library.sha = replacement_sha
                    changed.append(True)

            audit = support.SinkIO(path)
            with audit.install(monkeypatch, runner=runner, after_read=mutate):
                outcome = support.assemble(runner, sink, library, report)
            assert changed == [True]
            assert audit.files and all(file.closed for file in audit.files)
            status = path.stat()
            assert (status.st_ino, status.st_size, status.st_mtime_ns) == (
                original.st_ino, original.st_size, original.st_mtime_ns)
            assert path.read_bytes() == replacement
            with frozen.pf_sink.SinkReader(path) as reader:
                for _ in reader:
                    pass
                assert reader.complete and reader.sha256 == library.sha == replacement_sha
            return outcome

        expected, actual = run(frozen), run(run_json)
        support.assert_same(actual, expected)
        assert actual.failure is not None
        assert actual.failure[3] == "engine_invariant"
        assert actual.failure[2] == "the report sink trailer differs from its engine receipt"


def _transient_trade_flip():
    """First read of a window is altered; a replay sees the unchanged disk."""
    visits, flipped = {}, []

    def transform(_fd, offset, data):
        if b"trade-a" not in data:
            return data
        window = offset, len(data)
        visits[window] = visits.get(window, 0) + 1
        if visits[window] == 1:
            flipped.append(window)
            return _replace_first(data, b"trade-a", b"trade-b")
        return data
    return transform, flipped


def _transient_assembly(runner, sink, library, report, monkeypatch):
    transform, flipped = _transient_trade_flip()
    audit = support.SinkIO(sink.path)
    with audit.install(monkeypatch, runner=runner, transform=transform):
        outcome = support.assemble(runner, sink, library, report)
    assert len(flipped) == 1, "the product must consume the altered window"
    assert audit.files and all(file.closed for file in audit.files)
    return outcome


def test_fused_mutation_contract_transient_read_flip(tmp_path, monkeypatch, frozen):
    with _sink_case(tmp_path, [_point(0), _trade(1, entry_id="trade-a")]) as (
            sink, library, report):
        path = Path(sink.path)
        raw, original = path.read_bytes(), path.stat()
        transform, flipped = _transient_trade_flip()
        audit = support.SinkIO(path)
        with audit.install(monkeypatch, transform=transform):
            file = run_json._open_uncached_sink(path, raw=True)
            try:
                first = file.read(file.window)
            finally:
                file.close()
            file = run_json._open_uncached_sink(path, raw=True)
            try:
                second = file.read(file.window)
            finally:
                file.close()
        assert audit.files and all(file.closed for file in audit.files)
        assert [(offset, len(data)) for _, offset, data, _ in audit.reads] == [
            (0, len(raw)), (0, len(raw))]
        assert flipped == [(0, len(raw))]
        assert first == _replace_first(raw, b"trade-a", b"trade-b")
        assert second == raw and first != second

        expected = _transient_assembly(frozen, sink, library, report, monkeypatch)
        actual = _transient_assembly(run_json, sink, library, report, monkeypatch)
        support.assert_same(actual, expected)
        assert actual.failure is not None
        assert actual.failure[3] == "engine_invariant"
        assert "trailer SHA-256 mismatch" in actual.failure[2]
        status = path.stat()
        assert path.read_bytes() == raw
        assert (status.st_ino, status.st_size, status.st_mtime_ns) == (
            original.st_ino, original.st_size, original.st_mtime_ns)


def test_fused_mutation_contract_hash_path_mutant(tmp_path, monkeypatch, frozen):
    with _sink_case(tmp_path, [_point(0), _trade(1, entry_id="trade-a")], named=True) as (
            sink, library, report):
        raw = Path(sink.path).read_bytes()
        expected = _transient_assembly(frozen, sink, library, report, monkeypatch)
        actual = _transient_assembly(run_json, sink, library, report, monkeypatch)
        support.assert_same(actual, expected)
        assert actual.failure is not None
        assert actual.failure[3] == "engine_invariant"
        assert "trailer SHA-256 mismatch" in actual.failure[2]

        mutant = support.load_frozen_copy("rjone_hash_mutant")
        original_hash, weakened = hashlib.sha256, []

        class UnchangedDiskHash:
            """Unsafe copy: hash old disk bytes instead of the parsed read."""
            def __init__(self, data=b"", **kwargs):
                self.inner = original_hash(**kwargs)
                if data:
                    self.update(data)

            def update(self, data):
                data = bytes(data)
                if b"trade-b" in data:
                    weakened.append(True)
                    data = _replace_first(data, b"trade-b", b"trade-a")
                return self.inner.update(data)

            def __getattr__(self, name):
                return getattr(self.inner, name)

        # Mutate only a separately loaded test copy; the frozen oracle and the
        # product both retain their real hash feeds and refuse the same read.
        mutant.pf_sink.hashlib = types.SimpleNamespace(sha256=UnchangedDiskHash)
        broken = _transient_assembly(mutant, sink, library, report, monkeypatch)
        assert weakened == [True]
        assert broken.failure is None and broken.report.endswith(b"\n"), (
            "ONE_PASS_HASH_MUTANT_NOT_DETECTED", broken.failure)
        with pytest.raises(AssertionError):
            support.assert_same(broken, expected)
        assert Path(sink.path).read_bytes() == raw


CORRUPT_VARIANTS = tuple(BAD_LINES) + (
    "digest", "truncated", "duplicate-trailer", "after-trailer", "count",
    "long-record", "header-bom", "receipt", "incomplete", "run-id",
    "trade-order", "receipt-count", "report-trade-count",
)


@pytest.mark.parametrize("fault", CORRUPT_VARIANTS)
def test_fused_corrupt_sink_refusals(tmp_path, frozen, fault):
    if fault == "trade-order":
        rows = [_point(0), _trade(2), _trade(1)]
    else:
        rows = [_point(index) for index in range(40)]
    receipt_fault = fault if fault in ("receipt", "incomplete") else None
    with _sink_case(
            tmp_path, rows, outputs=False, fault=receipt_fault) as (sink, library, report):
        path = Path(sink.path)
        target = ordinary.encoded(rows[-1]) if rows else None
        if fault in BAD_LINES:
            _rewrite_sink(
                path,
                lambda body: body.replace(target, BAD_LINES[fault], 1),
                library=library)
        elif fault == "long-record":
            line = b'["E",0,' + b"1" * pf_sink.MAX_LINE_BYTES + b",0.0]\n"
            _rewrite_sink(path, lambda body: body.replace(target, line, 1), library=library)
        elif fault == "digest":
            _rewrite_sink(
                path, lambda body: body,
                trailer_edit=lambda trailer: trailer.__setitem__("sha256", "0" * 64))
        elif fault == "count":
            _rewrite_sink(
                path, lambda body: body,
                trailer_edit=lambda trailer: trailer["counts"].__setitem__(
                    "E", trailer["counts"]["E"] + 1))
        elif fault == "truncated":
            path.write_bytes(path.read_bytes()[:-1])
        elif fault == "duplicate-trailer":
            raw = path.read_bytes()
            path.write_bytes(raw + raw[_trailer_start(raw):])
        elif fault == "after-trailer":
            path.write_bytes(path.read_bytes() + b"x\n")
        elif fault == "header-bom":
            path.write_bytes(b"\xef\xbb\xbf" + path.read_bytes())
        elif fault == "run-id":
            sink.run_id = "different"
        elif fault == "receipt-count":
            library.counts = dict(library.counts, E=library.counts["E"] + 1)
        elif fault == "report-trade-count":
            report.total_trades += 1
        expected = support.assemble(frozen, sink, library, report)
        actual = support.assemble(run_json, sink, library, report)
        support.assert_same(actual, expected)
        if fault in ("negative-zero", "escaped-tag"):
            assert expected.failure is None
        else:
            assert expected.failure is not None


COMPETING_FAULTS = ("format", "trailer", "receipt", "trade",
                    "summary", "metric", "equity")


def _configure_competing(report, fault):
    if fault == "summary":
        report.net_profit = float("inf")
    elif fault == "metric":
        report.metrics.all.profit_factor = float("inf")


@pytest.mark.parametrize("fault", COMPETING_FAULTS)
def test_fused_competing_failures(tmp_path, monkeypatch, frozen, fault):
    if fault == "trade":
        rows = [_point(0), _trade(1, pnl=None)]
    elif fault == "equity":
        rows = [["E", 1_700_000_000_000, None, 0.0], _trade(1)]
    else:
        rows = [_point(0), _trade(1)]
    receipt_fault = "incomplete" if fault == "receipt" else None
    with _sink_case(
            tmp_path, rows, outputs=False, fault=receipt_fault) as (sink, library, report):
        if fault == "format":
            _rewrite_sink(
                sink.path,
                lambda body: _replace_first(body, b"10000.0", b"10000.00"),
                library=library)
        elif fault == "trailer":
            _rewrite_sink(
                sink.path, lambda body: body,
                trailer_edit=lambda trailer: trailer["counts"].__setitem__(
                    "E", trailer["counts"]["E"] + 1))
        _configure_competing(report, fault)
        expected = support.assemble(frozen, sink, library, report)
        fired = []
        original = run_json._BoundedJSONFile.write_ascii

        def fail_on_equity(file, data):
            if b'"time_ms":' in data and not fired:
                fired.append(bytes(data))
                raise OSError(errno.ENOSPC, "early equity spool full")
            return original(file, data)

        monkeypatch.setattr(run_json._BoundedJSONFile, "write_ascii", fail_on_equity)
        actual = support.assemble(run_json, sink, library, report)
        support.assert_same(actual, expected)
        assert fired or expected.failure is not None


@pytest.mark.parametrize("fault", ("open", "write", "flush", "close"))
def test_fused_atomic_publication(tmp_path, monkeypatch, frozen, fault):
    with _sink_case(tmp_path, [_point(0), _trade(1)], outputs=False) as (
            sink, library, report):
        def run(runner):
            with support.artifact_fault(monkeypatch, runner, fault) as (
                    files, calls, fired):
                outcome = support.assemble(runner, sink, library, report)
            assert outcome.report == b""
            assert outcome.failure is not None
            assert fired == [fault]
            assert fault in calls
            assert all(file.closed for file in files)
            return outcome

        expected, actual = run(frozen), run(run_json)
        support.assert_same(actual, expected)
        if fault == "close":
            # The shared close seam also covers _UncachedSinkFile. Base first
            # closes that reader during verification, before any report
            # artifact is created; its OSError becomes a strict-reader refusal.
            assert expected.failure[3] == "engine_invariant"
            assert expected.failure[4] == {}
        else:
            assert actual.failure[3] == "report_sink_failed"
            assert actual.failure[4] == {"reason": "scratch_io"}


@contextmanager
def _artifact_sync_probe(monkeypatch, runner, *, full_sync=False, fail=False):
    """Wrap real artifact syscalls without changing the shared os module."""
    real_os = runner.os
    proxy = types.SimpleNamespace(**vars(real_os))
    probe = types.SimpleNamespace(syncs=[], truncates=[])

    def counted_sync(name):
        original = getattr(real_os, name)

        def sync(fd):
            probe.syncs.append((name, fd, real_os.fstat(fd).st_size))
            if fail:
                raise OSError(errno.ENOSPC, "injected artifact sync failure")
            return original(fd)

        return sync

    for name in ("fdatasync", "fsync"):
        if hasattr(real_os, name):
            setattr(proxy, name, counted_sync(name))
    if full_sync and hasattr(proxy, "fdatasync"):
        del proxy.fdatasync

    def truncate(fd, size):
        probe.truncates.append((fd, size))
        return real_os.ftruncate(fd, size)

    proxy.ftruncate = truncate
    with monkeypatch.context() as patch:
        patch.setattr(runner, "os", proxy)
        yield probe


@pytest.mark.parametrize("full_sync", (False, True), ids=("native-sync", "fsync"))
@pytest.mark.parametrize("tail", (0, 4096, 17),
                         ids=("full-window", "aligned-tail", "padded-tail"))
def test_report_artifact_sync_count_follows_flush_events(
        tmp_path, monkeypatch, capsys, full_sync, tail):
    size = 8 * run_json._BoundedJSONFile.window + tail
    overhead = len(json.dumps({"payload": ""}, separators=(",", ":"))) + 1
    document = {"payload": "x" * (size - overhead)}
    expected = json.dumps(document, separators=(",", ":")) + "\n"
    assert len(expected) == size
    with _artifact_sync_probe(monkeypatch, run_json, full_sync=full_sync) as probe:
        with ExitStack() as resources:
            artifact = run_json._prepare_stream_report(document, resources, str(tmp_path))
            assert type(artifact) is run_json._BoundedJSONFile
            assert os.fstat(artifact.fileno()).st_size == size
            # Preparation explicitly flushes, then rewinds (which flushes).
            flush_seek_events = 2
            assert 0 < len(probe.syncs) <= 1 + flush_seek_events
            assert len(probe.syncs) == 1
            assert probe.syncs[0][1:] == (artifact.fileno(), size)
            method = "fdatasync" if not full_sync and hasattr(os, "fdatasync") else "fsync"
            assert probe.syncs[0][0] == method
            assert probe.truncates == (
                [(artifact.fileno(), size)] if tail % 4096 else [])
            assert run_json._write_report(artifact) == 0
            assert len(probe.syncs) == 1
        assert artifact.closed
    captured = capsys.readouterr()
    assert captured.out == expected and captured.err == ""


@pytest.mark.parametrize("full_sync", (False, True), ids=("native-sync", "fsync"))
def test_fused_artifact_sync_count_follows_rewinds(tmp_path, monkeypatch, full_sync):
    window = run_json._BoundedJSONFile.window
    message = "x" * (8 * window + 17)
    rows = _mixed()
    rows[2] = _event(message)
    with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
        with _artifact_sync_probe(monkeypatch, run_json, full_sync=full_sync) as probe:
            outcome = support.assemble(
                run_json, sink, library, report, outputs=True, configure=_configure_outputs)
        assert outcome.failure is None and len(outcome.report) >= 8 * window
        assert json.loads(outcome.report)["outputs"]["events"][0]["message"] == message
        descriptors = {fd for _, fd, _ in probe.syncs}
        # Trade/equity, open/close times, two series, events, and the document.
        assert len(descriptors) == 8
        assert len(probe.syncs) == len(descriptors)
        assert sum(size >= 8 * window for _, _, size in probe.syncs) == 2


def test_report_artifact_full_windows_are_readable_before_flush(tmp_path, monkeypatch):
    window = run_json._BoundedJSONFile.window
    data = b"".join(bytes([65 + index]) * window for index in range(8))
    with _artifact_sync_probe(monkeypatch, run_json) as probe, ExitStack() as resources:
        artifact = run_json._json_temporary(resources, str(tmp_path))
        assert artifact.write_ascii(data) == len(data)
        assert os.fstat(artifact.fileno()).st_size == len(data)
        # Use a second aligned window so raw readback cannot change append state.
        with run_json.mmap.mmap(-1, window) as buffer:
            address = ctypes.addressof(ctypes.c_char.from_buffer(buffer))
            for offset in range(0, len(data), window):
                assert artifact.libc.pread(
                    artifact.fileno(), address, window, offset) == window
                assert buffer[:] == data[offset:offset + window]
        assert probe.syncs == [] and probe.truncates == []
        assert artifact.read().encode("ascii") == data
        assert len(probe.syncs) == 1
        artifact.seek(0)
        assert artifact.read().encode("ascii") == data
        assert len(probe.syncs) == 1


def test_report_artifact_flush_then_append_keeps_exact_bytes(tmp_path, monkeypatch):
    window = run_json._BoundedJSONFile.window
    first, second = b"a" * (window + 17), b"b" * window
    with _artifact_sync_probe(monkeypatch, run_json) as probe, ExitStack() as resources:
        artifact = run_json._json_temporary(resources, str(tmp_path))
        artifact.write_ascii(first)
        artifact.flush()
        assert os.fstat(artifact.fileno()).st_size == len(first)
        assert len(probe.syncs) == 1
        artifact.write_ascii(second)
        assert len(probe.syncs) == 1
        artifact.flush()
        assert os.fstat(artifact.fileno()).st_size == len(first + second)
        assert len(probe.syncs) == 2
        artifact.seek(0)
        assert artifact.read().encode("ascii") == first + second
        assert len(probe.syncs) == 2
        assert probe.truncates == [
            (artifact.fileno(), len(first)), (artifact.fileno(), len(first + second))]


@pytest.mark.parametrize("fault", ("errno", "short", "interrupted"))
def test_report_artifact_window_write_errors_match_frozen(
        tmp_path, frozen, fault):
    def run(runner):
        window = runner._BoundedJSONFile.window
        data = b"".join(bytes([65 + index]) * window for index in range(3))
        calls = []
        with ExitStack() as resources:
            artifact = runner._json_temporary(resources, str(tmp_path))
            libc = artifact.libc

            def pwrite(fd, address, count, offset):
                calls.append(offset)
                if len(calls) == 2:
                    if fault == "short":
                        return count - 4096
                    ctypes.set_errno(errno.EINTR if fault == "interrupted" else errno.ENOSPC)
                    return -1
                return libc.pwrite(fd, address, count, offset)

            artifact.libc = types.SimpleNamespace(pwrite=pwrite, pread=libc.pread)
            if fault == "interrupted":
                assert artifact.write_ascii(data) == len(data)
                assert calls == [0, window, window, 2 * window]
                assert artifact.read().encode("ascii") == data
                return None
            with pytest.raises(OSError) as caught:
                artifact.write_ascii(data)
            assert calls == [0, window]
            assert artifact.size == 2 * window
            error = caught.value
            assert type(error) is OSError
            if fault == "errno":
                assert error.errno == errno.ENOSPC
                assert str(error) == str(OSError(errno.ENOSPC, os.strerror(errno.ENOSPC)))
            else:
                assert str(error) == "short report artifact write"
            return support.failure_envelope(runner, error)

    assert run(run_json) == run(frozen)


@pytest.mark.parametrize("full_sync", (False, True), ids=("native-sync", "fsync"))
@pytest.mark.parametrize("sink_errors", (False, True), ids=("ordinary-error", "sink-error"))
def test_report_artifact_sync_failure_waits_for_flush(
        tmp_path, monkeypatch, capsys, frozen, full_sync, sink_errors):
    def prepare(runner):
        completed = []

        def rows():
            yield "x" * (8 * runner._BoundedJSONFile.window + 17)
            completed.append(True)

        exception = runner.RunFailure if sink_errors else OSError
        with _artifact_sync_probe(
                monkeypatch, runner, full_sync=full_sync, fail=True) as probe:
            with ExitStack() as resources, pytest.raises(exception) as caught:
                artifact = runner._prepare_stream_report(
                    {"rows": runner._JSONRecordArray(rows)}, resources, str(tmp_path),
                    sink_errors=sink_errors)
                runner._write_report(artifact)
        assert len(probe.syncs) == 1
        for _, fd, _ in probe.syncs:
            with pytest.raises(OSError):
                os.fstat(fd)
        error = caught.value
        storage_error = error.__cause__ if sink_errors else error
        assert type(storage_error) is OSError
        assert storage_error.errno == errno.ENOSPC
        assert str(storage_error) == str(
            OSError(errno.ENOSPC, "injected artifact sync failure"))
        if sink_errors:
            assert type(error) is runner.RunFailure
            assert str(error) == "report sink write failed"
            assert error.code == "report_sink_failed"
            assert error.code_args == {"reason": "scratch_io"}
        return support.failure_envelope(runner, error), completed

    expected, _ = prepare(frozen)
    actual, completed = prepare(run_json)
    assert actual == expected
    assert completed == [True]
    captured = capsys.readouterr()
    assert captured.out == captured.err == ""
    assert not list(tmp_path.iterdir())


@pytest.mark.parametrize("full_sync", (False, True), ids=("native-sync", "fsync"))
@pytest.mark.parametrize("outputs", (False, True), ids=("report", "outputs"))
def test_fused_sync_failure_never_publishes(
        tmp_path, monkeypatch, frozen, full_sync, outputs):
    with _sink_case(tmp_path, _mixed() if outputs else [_point(0), _trade(1)],
                    outputs=outputs) as (sink, library, report):
        published = []

        def run(runner):
            with _artifact_sync_probe(
                    monkeypatch, runner, full_sync=full_sync, fail=True) as probe:
                outcome = support.assemble(
                    runner, sink, library, report, outputs=outputs,
                    configure=_configure_outputs if outputs else None,
                    consume=lambda *args: published.append(True))
            assert len(probe.syncs) == 1
            assert outcome.report == b"" and outcome.failure is not None
            assert outcome.failure[1:5] == (
                "RunFailure", "report sink write failed",
                "report_sink_failed", {"reason": "scratch_io"})
            assert isinstance(outcome.error.__cause__, OSError)
            assert outcome.error.__cause__.errno == errno.ENOSPC
            for _, fd, _ in probe.syncs:
                with pytest.raises(OSError):
                    os.fstat(fd)
            return outcome

        support.assert_same(run(run_json), run(frozen))
        assert published == []


def test_fused_large_report_bytes_match_frozen_artifacts(tmp_path, frozen):
    rows = []
    for index in range(10_000):
        rows.extend((
            _point(index), _trade(index + 1, pnl=-0.0 if index % 2 else 1.0),
            ["O", index, ordinary.bars.T0 + index,
             ordinary.bars.T0 + index + 1, -0.0, float(index)],
        ))
    rows.extend((["K", 50.], _event('quote"\n漢' * 2048)))
    with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
        expected = support.assemble(
            frozen, sink, library, report, outputs=True, configure=_configure_outputs)
        actual = support.assemble(
            run_json, sink, library, report, outputs=True, configure=_configure_outputs)
        assert expected.failure is None
        assert len(expected.report) >= 8 * run_json._BoundedJSONFile.window
        support.assert_same(actual, expected)


BAR_HEADER = b"timestamp,open,high,low,close,volume\n"


def _bar_line(index, *, timestamp=None, fields=None, ending=b"\n"):
    if fields is None:
        fields = (
            repr(100.0 + index / 1000.0),
            repr(102.0 + index / 1000.0),
            repr(99.0 + index / 1000.0),
            repr(101.0 + index / 1000.0),
            repr(10.0 + index / 100.0),
        )
    timestamp = 1_700_000_000_000 + index * 60_000 if timestamp is None else timestamp
    return (f"{timestamp}," + ",".join(fields)).encode("ascii") + ending


def _write_canonical_feed(path, count):
    state = 411
    with Path(path).open("wb") as file:
        file.write(BAR_HEADER)
        for index in range(count):
            state = (state * 1_103_515_245 + 12_345) & 0x7FFFFFFF
            fields = (
                repr(100.0 + (state % 1000) / 1000.0),
                repr(102.0 + (state % 1000) / 1000.0),
                repr(99.0 + (state % 1000) / 1000.0),
                repr(101.0 + (state % 1000) / 1000.0),
                repr(10.0 + (state % 10000) / 100.0),
            )
            file.write(_bar_line(index, fields=fields))


def _write_named_feed(path, name):
    path = Path(path)
    if name == "F1-100k":
        _write_canonical_feed(path, 100_000)
        return
    if name == "F2-floats":
        values = (
            ("0.1", "1e-7", "5e-324", "1e+22", "-0.0"),
            ("1.2345678901234567", "2.2250738585072014e-308",
             "1.7976931348623157e+308", "0.30000000000000004", "1.0"),
            ("-1e-100", "1e-100", "-1e16", "1e16", "2.5"),
        )
        with path.open("wb") as file:
            file.write(BAR_HEADER)
            for index, fields in enumerate(values):
                file.write(_bar_line(index, fields=fields))
        return
    with path.open("wb") as file:
        if name == "F3-quoted":
            file.write(BAR_HEADER)
            file.write(b'1700000000000,"100.0","102.0","99.0","101.0","10.0"\n')
        elif name == "F3-extra":
            file.write(b"timestamp,open,high,low,close,volume,extra\n")
            file.write(b"1700000000000,100.0,102.0,99.0,101.0,10.0,ignored\n")
        elif name == "F3-reordered":
            file.write(b"open,high,low,close,volume,timestamp\n")
            file.write(b"100.0,102.0,99.0,101.0,10.0,1700000000000\n")
        elif name == "F3-missing":
            file.write(b"timestamp,open,high,low,close\n")
            file.write(b"1700000000000,100.0,102.0,99.0,101.0\n")
        elif name == "F3-header-case":
            file.write(b"Timestamp,open,high,low,close,volume\n")
            file.write(b"1700000000000,100.0,102.0,99.0,101.0,10.0\n")
        elif name == "F4-whitespace":
            file.write(BAR_HEADER)
            file.write(b"1700000000000, 100.0 , 102.0 , 99.0 , 101.0 , 10.0 \n")
        elif name == "F4-crlf":
            file.write(BAR_HEADER.replace(b"\n", b"\r\n"))
            file.write(_bar_line(0, ending=b"\r\n"))
        elif name == "F4-blank":
            file.write(BAR_HEADER)
            file.write(b"\n")
            file.write(_bar_line(0))
            file.write(b"\n")
        elif name == "F4-bom":
            file.write(b"\xef\xbb\xbf" + BAR_HEADER)
            file.write(_bar_line(0))
        elif name == "F4-invalid-utf8":
            file.write(BAR_HEADER)
            file.write(b"1700000000000,\xff,102.0,99.0,101.0,10.0\n")
        elif name == "F4-malformed":
            file.write(BAR_HEADER)
            file.write(b'1700000000000,"100.0,102.0,99.0,101.0,10.0\n')
        elif name == "F5-nonfinite":
            file.write(BAR_HEADER)
            file.write(b"1700000000000,nan,inf,-inf,-0.0,1.0\n")
        elif name == "F5-integer-float":
            file.write(BAR_HEADER)
            file.write(b"1700000000000,100,102.0,99,101.0,10\n")
        elif name == "F5-seconds-ms":
            file.write(BAR_HEADER)
            file.write(b"1700000000,100.0,102.0,99.0,101.0,10.0\n")
            file.write(b"1700000060000,101.0,103.0,100.0,102.0,11.0\n")
        elif name == "F5-invalid-number":
            file.write(BAR_HEADER)
            file.write(b"1700000000000,oops,102.0,99.0,101.0,10.0\n")
        else:
            raise AssertionError(name)


BAR_FEEDS = (
    "F1-100k", "F2-floats", "F3-quoted", "F3-extra", "F3-reordered",
    "F3-missing", "F3-header-case", "F4-whitespace", "F4-crlf",
    "F4-blank", "F4-bom", "F4-invalid-utf8", "F4-malformed",
    "F5-nonfinite", "F5-integer-float", "F5-seconds-ms", "F5-invalid-number",
)


def _load_outcome(runner, path):
    try:
        bars, count, digest = runner.load_bars(Path(path))
    except Exception as error:
        return (
            "error", type(error).__qualname__, str(error), error.args,
            getattr(error, "code", None), getattr(error, "code_args", None),
            getattr(error, "exit_status", 1),
        )
    raw = (ctypes.string_at(ctypes.addressof(bars), ctypes.sizeof(bars))
           if count else b"")
    first = int(bars[0].timestamp) if count else None
    last = int(bars[count - 1].timestamp) if count else None
    return "ok", raw, count, first, last, digest


@pytest.mark.parametrize("name", BAR_FEEDS)
def test_bulk_load_bars_bytes_and_hash(tmp_path, frozen, name):
    path = Path(tmp_path) / f"{name}.csv"
    _write_named_feed(path, name)
    expected = _load_outcome(frozen, path)
    actual = _load_outcome(run_json, path)
    assert actual == expected
    if actual[0] == "ok":
        assert len(actual[1]) == actual[2] * ctypes.sizeof(run_json.BarC)


def _odd_index(label, count=4097):
    return {
        "first": 0,
        "middle": count // 2,
        "last": count - 1,
        "slab-edge": 4095,
    }[label]


def _write_odd_feed(path, kind, label):
    count = 4097
    position = _odd_index(label, count)
    with Path(path).open("wb") as file:
        file.write(BAR_HEADER)
        for index in range(count):
            if index != position:
                file.write(_bar_line(index))
                continue
            if kind == "quoted":
                fields = ('"100.0"', "102.0", "99.0", "101.0", "10.0")
                file.write(_bar_line(index, fields=fields))
            elif kind == "whitespace":
                fields = (" 100.0 ", "102.0", "99.0", "101.0", "10.0")
                file.write(_bar_line(index, fields=fields))
            elif kind == "nonfinite":
                fields = ("nan", "inf", "-inf", "-0.0", "1.0")
                file.write(_bar_line(index, fields=fields))
            elif kind == "integer":
                fields = ("100", "102.0", "99", "101.0", "10")
                file.write(_bar_line(index, fields=fields))
            else:
                raise AssertionError(kind)


@pytest.mark.parametrize("kind", ("quoted", "whitespace", "nonfinite", "integer"))
@pytest.mark.parametrize("label", ("first", "middle", "last", "slab-edge"))
def test_bulk_load_bars_odd_rows_at_slab_edges(tmp_path, frozen, kind, label):
    path = Path(tmp_path) / f"odd-{kind}-{label}.csv"
    _write_odd_feed(path, kind, label)
    assert _load_outcome(run_json, path) == _load_outcome(frozen, path)


@pytest.mark.parametrize("label", ("first", "middle", "last", "slab-edge"))
def test_bulk_load_bars_decode_error_positions(tmp_path, frozen, label):
    count = 4097
    position = _odd_index(label, count)
    path = Path(tmp_path) / f"invalid-utf8-{label}.csv"
    with path.open("wb") as file:
        file.write(BAR_HEADER)
        for index in range(count):
            if index == position:
                file.write(
                    f"{1_700_000_000_000 + index * 60_000},".encode("ascii")
                    + b"\xff,102.0,99.0,101.0,10.0\n")
            else:
                file.write(_bar_line(index))
    expected = _load_outcome(frozen, path)
    actual = _load_outcome(run_json, path)
    assert actual == expected
    assert actual[0] == "error"
    assert re.findall(r"position (\d+)", actual[2]) == re.findall(
        r"position (\d+)", expected[2])
    assert re.findall(r"position (\d+)", actual[2])


def test_bulk_load_bars_bulk_engagement(tmp_path, monkeypatch):
    path = Path(tmp_path) / "engagement.csv"
    count = 8193
    _write_canonical_feed(path, count)
    calls = []
    original = run_json.csv.DictReader

    def counted(*args, **kwargs):
        calls.append((args, kwargs))
        return original(*args, **kwargs)

    monkeypatch.setattr(run_json.csv, "DictReader", counted)
    bars, loaded, _ = run_json.load_bars(path)
    assert loaded == count
    assert ctypes.sizeof(bars) == count * ctypes.sizeof(run_json.BarC)
    assert not calls, (
        f"ONE_PASS_CANONICAL_DICTREADER: constructed {len(calls)} readers for the canonical feed")


_CORPUS_DRIVER = r"""
import importlib.util
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
runner_path = pathlib.Path(sys.argv[2])
frozen_pf_path = pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "docker"))
sys.path.insert(0, str(root / "tests"))
if runner_path.name.startswith("run_json_frozen_"):
    pf_spec = importlib.util.spec_from_file_location("pf_sink", frozen_pf_path)
    pf_module = importlib.util.module_from_spec(pf_spec)
    sys.modules["pf_sink"] = pf_module
    pf_spec.loader.exec_module(pf_module)
runner_spec = importlib.util.spec_from_file_location("rjone_corpus_runner", runner_path)
runner = importlib.util.module_from_spec(runner_spec)
sys.modules[runner_spec.name] = runner
# Use the real runner's resource base, also for the pinned source copy.
runner.__file__ = str(root / "docker" / "run_json.py")
runner_spec.loader.exec_module(runner)
runner.time.time = lambda: 10.0
raise SystemExit(runner.main(["--so", sys.argv[4], *sys.argv[5:]]))
"""


def _corpus_run(runner_path, frozen_pf, case):
    argv = [str(value) for value in case["argv"]]
    assert "--so" not in argv
    return subprocess.run(
        [sys.executable, "-B", "-c", _CORPUS_DRIVER, str(ROOT),
         str(runner_path), str(frozen_pf), str(case["so"]), *argv],
        cwd=ROOT, capture_output=True)


@pytest.mark.skipif(
    not os.environ.get("PINEFORGE_ONE_PASS_CORPUS_CASES"),
    reason="SKIP_ONE_PASS_CORPUS_FIXTURE_ABSENT: PINEFORGE_ONE_PASS_CORPUS_CASES")
def test_fused_report_bytes_corpus_40():
    manifest_name = "PINEFORGE_ONE_PASS_CORPUS_CASES"
    manifest = os.environ.get(manifest_name)
    assert manifest, f"ONE_PASS_CORPUS_FIXTURE_REQUIRED: set {manifest_name}"
    cases = json.loads(Path(manifest).read_text(encoding="utf-8"))
    assert isinstance(cases, list) and len(cases) >= 40
    identities = {
        json.dumps((case["so"], case["argv"]), sort_keys=True)
        for case in cases
    }
    assert len(identities) >= 40, "the corpus row must execute 40 distinct runs"
    product = ROOT / "docker" / "run_json.py"
    frozen_runner = ROOT / "tests" / "fixtures" / "run_json_frozen_6b33c47f_rjone.py"
    frozen_pf = ROOT / "tests" / "fixtures" / "pf_sink_frozen_6b33c47f_rjone.py"
    for case in cases:
        product_result = _corpus_run(product, frozen_pf, case)
        frozen_result = _corpus_run(frozen_runner, frozen_pf, case)
        assert product_result.returncode == frozen_result.returncode == 0, (
            case, product_result.stdout, product_result.stderr,
            frozen_result.stdout, frozen_result.stderr)
        assert product_result.stderr == frozen_result.stderr == b""
        assert product_result.stdout == frozen_result.stdout
