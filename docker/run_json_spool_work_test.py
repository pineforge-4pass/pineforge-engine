"""Compare complete reports, artifact faults and observed column/proof work."""
from collections import Counter, defaultdict
from contextlib import contextmanager, ExitStack, nullcontext
import errno
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import run_json_spool_reference as reference
import run_json_fused_test_support as support
import run_json_wrapper_test_support as fixtures
from run_json_wrapper_work_test import _EquityWork, _mutate, _EQUITY_WIRES
from run_json_fused_test import (
    _sink_case, _configure_outputs, _synthetic_case, _column_row_width, artifact_window,
)
from run_json_trade_scan_test import _point, _trade, _event
import test_run_json_sink as ordinary


class _ColumnWork:
    """Wrap real writes; reconcile every observed byte with an independent row."""
    def __init__(self):
        self.columns = {}
        self.chunks = defaultdict(list)
        self.trade_stream_calls = 0

    def assert_writes(self, file, expected):
        actual = self.chunks[file]
        assert b"".join(actual) == b"".join(expected)
        assert file.size == sum(map(len, expected))
        assert len(actual) == len(expected), (
            f"SPOOL_ASCII_WRITES: {len(actual)} real writes; "
            f"expected {len(expected)} bounded joins")
        assert actual == expected, "SPOOL_ASCII_WRITES: encoded join boundaries differ"

    def assert_no_trade_streams(self):
        assert self.trade_stream_calls == 0, (
            f"SPOOL_STREAM_CALLS: {self.trade_stream_calls} per-trade stream calls")

    @contextmanager
    def install(self, monkeypatch):
        write = run_json._BoundedJSONFile.write_ascii
        trade = run_json._ReportSink._append_trade_record
        equity = run_json._ReportSink._append_equity_batch
        stream = run_json._stream_json

        def written(file, data):
            result = write(file, data)
            if file in self.columns:
                assert result == len(data)
                self.chunks[file].append(bytes(data))
            return result

        def append_trade(sink, record):
            if sink._trades_spool is not None:
                self.columns[sink._trades_spool.file] = "T"
            return trade(sink, record)

        def append_equity(sink, batch):
            if sink._equity_spool is not None:
                self.columns[sink._equity_spool.file] = "E"
            return equity(sink, batch)

        def streamed(value, destination):
            self.trade_stream_calls += self.columns.get(destination) == "T"
            return stream(value, destination)

        with monkeypatch.context() as patch:
            patch.setattr(run_json._BoundedJSONFile, "write_ascii", written)
            patch.setattr(run_json._ReportSink, "_append_trade_record", append_trade)
            patch.setattr(run_json._ReportSink, "_append_equity_batch", append_equity)
            patch.setattr(run_json, "_stream_json", streamed)
            yield self


def _joined(parts):
    """Independent schedule for complete encoded records and supplied E batches."""
    pending, size, count = [], 0, 0
    result = []
    for index, (data, rows) in enumerate(parts):
        piece = data if index == 0 else b"," + data
        if pending and (count + rows > 512 or size + len(piece) > 262144):
            result.append(b"".join(pending))
            pending, size, count = [], 0, 0
        pending.append(piece)
        size += len(piece)
        count += rows
        if count >= 512 or size >= 262144:
            result.append(b"".join(pending))
            pending, size, count = [], 0, 0
    if pending:
        result.append(b"".join(pending))
    return result


def _trade_text(row):
    fields = tuple(field[0] for field in pf_sink._T[:16])
    return json.dumps(dict(zip(fields, row[1:17])),
                      separators=(",", ":"), allow_nan=False).encode("ascii")


def _run(sink, library, report, monkeypatch, *, old=False, work=None, **kwargs):
    target = support.reference_sink(run_json, sink)
    context = reference.install(monkeypatch) if old else nullcontext()
    with context:
        try:
            observation = work.install(monkeypatch) if work is not None else nullcontext()
            with observation:
                outcome = support.assemble(run_json, target, library, report, **kwargs)
            receipt = target.sha256, dict(target.counts)
            return outcome, receipt, target
        finally:
            target.close()


def _pair(sink, library, report, monkeypatch, **kwargs):
    raw = Path(sink.path).read_bytes()
    expected, expected_receipt, _ = _run(
        sink, library, report, monkeypatch, old=True, **kwargs)
    actual, receipt, _ = _run(sink, library, report, monkeypatch, **kwargs)
    support.assert_same(actual, expected)
    assert receipt == expected_receipt
    assert hashlib.sha256(actual.report).digest() == hashlib.sha256(expected.report).digest()
    assert Path(sink.path).read_bytes() == raw
    return actual


class _TradeDict(dict):
    """A mapping whose subclass identity must preserve generic dispatch."""


def _gate_trade(shape):
    trade = json.loads(_trade_text(_trade(1)))
    if shape == "non-string-key":
        trade[7] = trade.pop("side")
    elif shape == "long-string":
        trade["side"] = "x" * 4097
    elif shape == "dict-subclass":
        trade = _TradeDict(trade)
    elif shape == "escaped-side":
        trade["side"] = ('quote"\\/\b\f\n\r\t' + "".join(map(chr, range(32)))
                         + "\u007f\u0080漢\U0001f600")
    elif shape in ("nan", "inf", "-inf"):
        trade["pnl"] = float(shape)
    else:
        assert shape == "ordinary"
    return trade


def _trade_gate_bytes(trade, tmp_path, monkeypatch, *, old=False):
    context = reference.install(monkeypatch) if old else nullcontext()
    calls, errors = Counter(), []
    with context, ExitStack() as resources, monkeypatch.context() as patch:
        column = run_json._JSONColumn(resources, str(tmp_path))
        # A completed predecessor also pins the separator before a failing row.
        column.append(json.loads(_trade_text(_trade(1))))
        append, append_ascii = column.append, column.append_ascii

        def generic(value):
            calls["generic"] += 1
            return append(value)

        def fast(data, *, row_count=1):
            calls["fast"] += 1
            return append_ascii(data, row_count=row_count)

        patch.setattr(column, "append", generic)
        patch.setattr(column, "append_ascii", fast)
        # Inject values after the sink-to-report projection, which normally
        # produces only exact scalar types and the short side string.
        patch.setattr(run_json, "_sink_trade", lambda record: record)
        patch.setattr(reference, "_sink_trade", lambda record: record)
        sink = types.SimpleNamespace(
            named=False, _trades_spool=column, _trades_spool_failed=False,
            _remember_spool_failure=errors.append)
        run_json._ReportSink._append_trade_record(sink, trade)
        column.file.seek(0)
        data = column.file.read().encode("ascii")
        assert data.startswith(_trade_text(_trade(1)) + b",")
        outcome = (data, tuple((type(error), str(error)) for error in errors),
                   len(column), sink._trades_spool_failed)
    return outcome, calls


def _assert_trade_gate(calls, *, fast):
    expected = Counter(fast=1) if fast else Counter(generic=1)
    assert calls == expected, (
        f"TRADE_ENCODER_GATE: {dict(calls)} dispatches; expected {dict(expected)}")


@pytest.mark.parametrize("shape,fast", (
    ("ordinary", True), ("escaped-side", True),
    ("non-string-key", False), ("long-string", False), ("dict-subclass", False),
    ("nan", False), ("inf", False), ("-inf", False),
))
def test_trade_encoder_gate_matches_generic_bytes(tmp_path, monkeypatch, shape, fast):
    trade = _gate_trade(shape)
    expected, control = _trade_gate_bytes(trade, tmp_path, monkeypatch, old=True)
    _assert_trade_gate(control, fast=False)
    if fast:
        with pytest.raises(AssertionError, match="TRADE_ENCODER_GATE"):
            _assert_trade_gate(control, fast=True)
    actual, calls = _trade_gate_bytes(trade, tmp_path, monkeypatch)
    assert actual == expected
    _assert_trade_gate(calls, fast=fast)
    assert run_json._json_scalar_record(trade) is (
        shape not in ("non-string-key", "dict-subclass"))
    # Non-finite scalars pass the type/size predicate, then allow_nan=False
    # rejects speculative encoding and replays the generic prefix and error.
    assert run_json._json_fast_value(trade) is (fast or shape in ("nan", "inf", "-inf"))
    if shape in ("nan", "inf", "-inf"):
        assert len(expected[1]) == 1 and expected[1][0][0] is ValueError
        assert expected[2:] == (1, True)
    else:
        assert expected[1] == ()
        assert expected[2:] == (2, False)


@pytest.mark.parametrize("shape", ("dict-subclass", "long-string"))
def test_trade_encoder_gate_detects_widened_acceptance(tmp_path, monkeypatch, shape):
    trade = _gate_trade(shape)
    expected, control = _trade_gate_bytes(trade, tmp_path, monkeypatch, old=True)
    _assert_trade_gate(control, fast=False)
    actual, baseline = _trade_gate_bytes(trade, tmp_path, monkeypatch)
    assert actual == expected
    _assert_trade_gate(baseline, fast=False)
    if shape == "dict-subclass":
        name = "_json_scalar_record"
        changed = _mutate(
            run_json._json_scalar_record, "return type(value) is dict and all(",
            "return isinstance(value, dict) and all(")
    else:
        name = "_json_fast_value"
        changed = _mutate(
            run_json._json_fast_value,
            "return sum(len(item) for item in value.values() if type(item) is str) <= _STREAM_FAST_STR_LIMIT",
            "return True")
    monkeypatch.setattr(run_json, name, changed)
    actual, calls = _trade_gate_bytes(trade, tmp_path, monkeypatch)
    assert actual == expected
    assert calls == Counter(fast=1)
    with pytest.raises(AssertionError, match="TRADE_ENCODER_GATE"):
        _assert_trade_gate(calls, fast=False)


@pytest.mark.parametrize("count", (1, 511, 512, 513, 1024, 4097))
def test_trade_spool_boundary_bytes(tmp_path, monkeypatch, count):
    rows = [_point(0)] + [_trade(i + 1, pnl=(-0.0 if i % 2 else 1.0))
                                for i in range(count)]
    with _sink_case(tmp_path, rows) as args:
        outcome = _pair(*args, monkeypatch)
        assert outcome.failure is None
        assert len(json.loads(outcome.report)["trades"]) == count


@pytest.mark.parametrize("count", (511, 512, 513, 1024, 4097))
def test_trade_spool_joins_replace_row_writes(tmp_path, monkeypatch, count):
    trades = [_trade(i + 1) for i in range(count)]
    expected_chunks = _joined([(_trade_text(row), 1) for row in trades])
    with _sink_case(tmp_path, [_point(0), *trades]) as (sink, library, report):
        control = _ColumnWork()
        expected, _, before = _run(
            sink, library, report, monkeypatch, old=True, work=control)
        file = before._trades_spool.file
        assert expected.failure is None
        assert len(control.chunks[file]) == 2 * count - 1
        assert control.trade_stream_calls == count
        with pytest.raises(AssertionError, match="SPOOL_ASCII_WRITES"):
            control.assert_writes(file, expected_chunks)
        work = _ColumnWork()
        actual, _, after = _run(sink, library, report, monkeypatch, work=work)
        support.assert_same(actual, expected)
        work.assert_writes(after._trades_spool.file, expected_chunks)
        work.assert_no_trade_streams()


def test_trade_stream_counter_detects_restored_dispatch(tmp_path, monkeypatch):
    with _sink_case(tmp_path, [_point(0), _trade(1)]) as (sink, library, report):
        work = _ColumnWork()
        with monkeypatch.context() as patch:
            patch.setattr(run_json._ReportSink, "_append_trade_record",
                          reference._append_trade_record)
            outcome, _, _ = _run(sink, library, report, patch, work=work)
        assert outcome.failure is None and work.trade_stream_calls == 1
        with pytest.raises(AssertionError, match="SPOOL_STREAM_CALLS"):
            work.assert_no_trade_streams()


@pytest.mark.parametrize("count", (1, 37))
def test_trade_spool_has_no_stream_dispatch(tmp_path, monkeypatch, count):
    rows = [_point(0)] + [_trade(i + 1) for i in range(count)]
    with _sink_case(tmp_path, rows) as (sink, library, report):
        control, work = _ColumnWork(), _ColumnWork()
        expected, _, _ = _run(
            sink, library, report, monkeypatch, old=True, work=control)
        actual, _, _ = _run(sink, library, report, monkeypatch, work=work)
        support.assert_same(actual, expected)
        assert control.trade_stream_calls == count
        with pytest.raises(AssertionError, match="SPOOL_STREAM_CALLS"):
            control.assert_no_trade_streams()
        work.assert_no_trade_streams()


@pytest.mark.parametrize("restore", ("trade-rows", "equity-rows"))
def test_spool_write_counter_detects_immediate_rows(tmp_path, monkeypatch, restore):
    rows = []
    for index in range(37):
        rows.extend((_point(index), _trade(index + 1)))
    with _sink_case(tmp_path, rows) as (sink, library, report):
        expected, _, _ = _run(sink, library, report, monkeypatch, old=True)
        work = _ColumnWork()
        with monkeypatch.context() as patch:
            if restore == "trade-rows":
                patch.setattr(run_json._ReportSink, "_append_trade_record",
                              reference._append_trade_record)
                tag = "T"
            else:
                patch.setattr(run_json._JSONColumn, "append_ascii", reference._append_ascii)
                tag = "E"
            actual, _, target = _run(sink, library, report, patch, work=work)
        support.assert_same(actual, expected)
        column = target._trades_spool if tag == "T" else target._equity_spool
        with pytest.raises(AssertionError, match="SPOOL_ASCII_WRITES"):
            work.assert_writes(column.file, [b"".join(work.chunks[column.file])])


@pytest.mark.parametrize("batch_size", (1, 12, 127, 128))
def test_interleaved_equity_spool_joins(tmp_path, monkeypatch, batch_size):
    rows = []
    for batch in range(43):
        rows.extend(_point(batch * batch_size + i) for i in range(batch_size))
        rows.append(_trade(batch + 1))
    with _sink_case(tmp_path, rows) as (sink, library, report):
        control, work = _ColumnWork(), _ColumnWork()
        expected, _, before = _run(
            sink, library, report, monkeypatch, old=True, work=control)
        actual, _, after = _run(sink, library, report, monkeypatch, work=work)
        support.assert_same(actual, expected)
        assert expected.failure is None
        chunks = control.chunks[before._equity_spool.file]
        # The immediate writer alternates whole batches and their separators.
        parts = [(data, data.count(b'{"time_ms":')) for data in chunks if data != b","]
        assert sum(count for _, count in parts) == 43 * batch_size
        expected_chunks = _joined(parts)
        with pytest.raises(AssertionError, match="SPOOL_ASCII_WRITES"):
            control.assert_writes(before._equity_spool.file, expected_chunks)
        work.assert_writes(after._equity_spool.file, expected_chunks)


def _wide_trade(size):
    row = json.loads(_trade_text(_trade(1)))
    row["entry_id"] = ""
    overhead = len(json.dumps(row, separators=(",", ":"), allow_nan=False))
    row["entry_id"] = "x" * (size - overhead)
    data = json.dumps(row, separators=(",", ":"), allow_nan=False).encode("ascii")
    assert len(data) == size
    return data


def _byte_boundary_parts(size, count):
    prefix_count = 1 if count == 2 else 511
    parts = [(_trade_text(_trade(index + 1)), 1) for index in range(prefix_count)]
    prefix_size = sum(len(data) for data, _ in parts) + len(parts) - 1
    last_count = count - prefix_count
    tail = [_trade_text(_trade(prefix_count + index + 1)) for index in range(last_count - 1)]
    wide_size = size - prefix_size - 1 - sum(map(len, tail)) - len(tail)
    parts.append((b",".join([*tail, _wide_trade(wide_size)]), last_count))
    assert len(b",".join(data for data, _ in parts)) == size
    assert sum(rows for _, rows in parts) == count
    # At 513, a two-row piece meets the byte bound while crossing the row bound.
    # Its predecessor queue has 511 rows, so both pre-checks are exercised.
    parts.append((_trade_text(_trade(count + 1)), 1))
    return parts


def _encoded_column_bytes(parts, tmp_path, monkeypatch, *, old=False):
    context = reference.install(monkeypatch) if old else nullcontext()
    work = _ColumnWork()
    with context, ExitStack() as resources, work.install(monkeypatch):
        column = run_json._JSONColumn(resources, str(tmp_path))
        work.columns[column.file] = "T"
        for data, count in parts:
            column.append_ascii(data, row_count=count)
        assert len(column) == sum(count for _, count in parts)
        artifact = run_json._prepare_stream_report(
            {"trades": column}, resources, str(tmp_path))
        data = artifact.read().encode("ascii")
        expected = b'{"trades":[' + b",".join(piece for piece, _ in parts) + b"]}\n"
        assert data == expected
        assert len(json.loads(data)["trades"]) == len(column)
    return data, work, column.file


@pytest.mark.parametrize("size", (262143, 262144, 262145))
@pytest.mark.parametrize("count", (2, 512, 513))
def test_encoded_spool_exact_byte_and_record_boundaries(tmp_path, monkeypatch, size, count):
    parts = _byte_boundary_parts(size, count)
    expected_chunks = _joined(parts)
    expected, control, before = _encoded_column_bytes(parts, tmp_path, monkeypatch, old=True)
    with pytest.raises(AssertionError, match="SPOOL_ASCII_WRITES"):
        control.assert_writes(before, expected_chunks)
    actual, work, after = _encoded_column_bytes(parts, tmp_path, monkeypatch)
    assert actual == expected
    work.assert_writes(after, expected_chunks)


@pytest.mark.parametrize("count", (2, 512))
def test_spool_join_counter_detects_inclusive_byte_precheck(tmp_path, monkeypatch, count):
    parts = _byte_boundary_parts(262144, count)
    expected_chunks = _joined(parts)
    expected, control, before = _encoded_column_bytes(parts, tmp_path, monkeypatch, old=True)
    with pytest.raises(AssertionError, match="SPOOL_ASCII_WRITES"):
        control.assert_writes(before, expected_chunks)
    actual, baseline, file = _encoded_column_bytes(parts, tmp_path, monkeypatch)
    assert actual == expected
    baseline.assert_writes(file, expected_chunks)
    changed = _mutate(
        run_json._BoundedJSONFile.append_ascii,
        "or self._pending_bytes + size > _STREAM_BATCH_BYTES",
        "or self._pending_bytes + size >= _STREAM_BATCH_BYTES")
    monkeypatch.setattr(run_json._BoundedJSONFile, "append_ascii", changed)
    actual, work, after = _encoded_column_bytes(parts, tmp_path, monkeypatch)
    assert actual == expected
    with pytest.raises(AssertionError, match="SPOOL_ASCII_WRITES"):
        work.assert_writes(after, expected_chunks)


@pytest.mark.parametrize("size", (262144, 262145, 524305))
@pytest.mark.parametrize("neighbours", (False, True))
def test_encoded_trade_byte_bound_and_oversized_row(
        tmp_path, monkeypatch, size, neighbours):
    wide = _wide_trade(size)
    rows = [_trade_text(_trade(1)), wide, _trade_text(_trade(3))] if neighbours else [wide]

    def run(old):
        work = _ColumnWork()
        context = reference.install(monkeypatch) if old else nullcontext()
        with context, ExitStack() as resources, work.install(monkeypatch):
            column = run_json._JSONColumn(resources, str(tmp_path))
            work.columns[column.file] = "T"
            for row in rows:
                column.append_ascii(row)
            artifact = run_json._prepare_stream_report(
                {"trades": column}, resources, str(tmp_path))
            data = artifact.read().encode("ascii")
            assert json.loads(data)["trades"] == [json.loads(row) for row in rows]
            if not old:
                work.assert_writes(column.file, _joined([(row, 1) for row in rows]))
            return data

    assert run(False) == run(True)


@pytest.mark.parametrize("boundary", (
    "write", "write-ascii", "flush", "seek", "read", "read-at", "close",
))
def test_pending_ascii_precedes_other_artifact_operations(tmp_path, monkeypatch, boundary):
    def run(old):
        context = reference.install(monkeypatch) if old else nullcontext()
        with context, ExitStack() as resources:
            column = run_json._JSONColumn(resources, str(tmp_path))
            column.append_ascii(b'{"n":1}')
            column.append_ascii(b'{"n":2}')
            file = column.file
            if boundary == "write":
                file.write(",3")
            elif boundary == "write-ascii":
                file.write_ascii(b",3")
            elif boundary == "flush":
                file.flush()
            elif boundary == "seek":
                file.seek(0)
            elif boundary == "read":
                assert file.read() == '{"n":1},{"n":2}'
            elif boundary == "read-at":
                assert file.read_at(0, file.window) == '{"n":1},{"n":2}'
            elif boundary == "close":
                # close drains logical text without adding a durability barrier.
                file.close()
                assert file.closed and file.buffer is None
                assert file.size == len(b'{"n":1},{"n":2}')
                return None
            file.seek(0)
            return file.read()

    assert run(False) == run(True)


class _ProofWork(_EquityWork):
    """Observe proof/diagnostic dict calls and fresh batches' stored dictionaries."""
    def __init__(self):
        super().__init__()
        self.bound_calls = Counter()
        self.eager_dicts = 0
        self.endpoint_dicts = 0
        self.batch_dicts = 0
        self.batches = self.proof_calls = 0
        self.active = set()
        self.endpoint_inputs = {}

    def _profile(self, frame, event, arg):
        super()._profile(frame, event, arg)
        observed_proof = frame.f_code is self.root and frame.f_globals is self.namespace
        if event == "call":
            if frame.f_code.co_name == "_bulk_equity_endpoint":
                self.endpoint_inputs[id(frame)] = frame.f_locals["point"]
            if observed_proof or (
                    frame.f_back is not None and id(frame.f_back) in self.active):
                self.active.add(id(frame))
                self.proof_calls += observed_proof
        elif event == "return":
            if observed_proof and arg is not None:
                self.batches += 1
                self.batch_dicts += sum(isinstance(value, dict) for value in vars(arg).values())
            if id(frame) in self.endpoint_inputs:
                before = self.endpoint_inputs.pop(id(frame))
                self.endpoint_dicts += type(arg) is dict and arg is not before
            self.active.discard(id(frame))
        elif event == "c_call" and id(frame) in self.active:
            name = getattr(arg, "__name__", "")
            if (name in ("rpartition", "split", "endswith", "isdigit")
                    or (name in ("rfind", "find", "index")
                        and getattr(arg, "__self__", None) is self.run.pattern)):
                self.bound_calls[name] += 1

    @contextmanager
    def install(self, monkeypatch):
        prove = run_json._bulk_equity
        trades = run_json._SinkReportDiagnostics.trades

        def constructed(*args, **kwargs):
            value = dict(*args, **kwargs)
            self.eager_dicts += 1
            return value

        # Observe dict calls in both functions without changing dict's type in
        # the constructor's isinstance checks. Inspect fresh batch state there.
        namespace = dict(prove.__globals__, dict=constructed)
        observed = types.FunctionType(
            prove.__code__, namespace, prove.__name__, prove.__defaults__, prove.__closure__)
        observed.__kwdefaults__ = prove.__kwdefaults__
        observed_trades = types.FunctionType(
            trades.__code__, dict(trades.__globals__, dict=constructed),
            trades.__name__, trades.__defaults__, trades.__closure__)
        observed_trades.__kwdefaults__ = trades.__kwdefaults__
        self.root, self.namespace = prove.__code__, namespace
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_bulk_equity", observed)
            patch.setattr(run_json._SinkReportDiagnostics, "trades", observed_trades)
            with super().install(patch):
                yield self

    def assert_proofs_observed(self):
        assert self.proof_calls == self.batches > 0 and not self.active, (
            f"EQUITY_PROOF_WORK: {self.proof_calls} instrumented calls for {self.batches} batches")

    def assert_bounds_cached(self):
        self.assert_proofs_observed()
        assert not self.bound_calls, (
            f"EQUITY_BOUND_WORK: {dict(self.bound_calls)} repeated bound operations")

    def assert_endpoints_lazy(self):
        self.assert_proofs_observed()
        assert self.proof_calls == self.batches > 1
        assert self.eager_dicts + self.endpoint_dicts <= 2 and self.batch_dicts == 0, (
            f"EQUITY_ENDPOINT_WORK: {self.eager_dicts} eager dictionaries and "
            f"{self.endpoint_dicts} endpoint dictionaries for {self.batches} batches; "
            f"{self.batch_dicts} dictionaries stored on fresh batches")


def _observed_proof(raw, monkeypatch, *, old=False):
    context = reference.install(monkeypatch) if old else nullcontext()
    with context:
        work = _ProofWork()
        with work.install(monkeypatch):
            batch = run_json._bulk_equity(raw)
            assert batch is not None
            encoded = run_json._bulk_equity_ascii(batch)
    return encoded, work


@pytest.mark.parametrize("old", (False, True), ids=("current", "reference"))
def test_equity_proof_counter_detects_uninstrumented_globals(monkeypatch, old):
    raw = ordinary.encoded(_point(0))
    context = reference.install(monkeypatch) if old else nullcontext()
    with context:
        prove = run_json._bulk_equity
        control = _ProofWork()
        with control.install(monkeypatch):
            batch = run_json._bulk_equity(raw)
            assert batch is not None
            expected = run_json._bulk_equity_ascii(batch)
        control.assert_proofs_observed()
        work = _ProofWork()
        with work.install(monkeypatch):
            # This shares the instrumented function's code but bypasses its globals.
            batch = prove(raw)
            assert batch is not None
            assert run_json._bulk_equity_ascii(batch) == expected
        assert work.proof_calls == work.batches == 0
        with pytest.raises(AssertionError, match="EQUITY_PROOF_WORK"):
            work.assert_proofs_observed()


@pytest.mark.parametrize("batch_size", (1, 12, 127, 128))
def test_equity_bound_work_is_cached(tmp_path, monkeypatch, batch_size):
    raw = b"".join(ordinary.encoded(_point(i)) for i in range(batch_size))

    def run(old):
        context = reference.install(monkeypatch) if old else nullcontext()
        with context:
            work = _ProofWork()
            with work.install(monkeypatch):
                assert run_json._BULK_E_RUN.match(raw).end() == len(raw)
                batch = run_json._bulk_equity(raw)
                assert batch is not None and batch.count == batch_size
                encoded = run_json._bulk_equity_ascii(batch)
            return encoded, work

    expected, control = run(True)
    assert control.bound_calls["rpartition"] == control.bound_calls["split"] == 1
    assert control.eager_dicts == 2
    assert control.batch_dicts == 2
    with pytest.raises(AssertionError, match="EQUITY_BOUND_WORK"):
        control.assert_bounds_cached()
    actual, work = run(False)
    assert actual == expected
    work.assert_bounded()
    work.assert_bounds_cached()
    assert work.eager_dicts == work.endpoint_dicts == 0
    assert work.batch_dicts == 0


def test_equity_endpoint_dictionaries_belong_only_to_curve_endpoints(tmp_path, monkeypatch):
    rows = []
    for index in range(37):
        rows.extend([*(_point(index * 12 + i) for i in range(12)), _trade(index + 1)])
    with _sink_case(tmp_path, rows) as (sink, library, report):
        with reference.install(monkeypatch):
            control = _ProofWork()
            expected, _, _ = _run(sink, library, report, monkeypatch, work=control)
        assert expected.failure is None and control.batches >= 37
        assert control.eager_dicts == 2 * control.batches
        assert control.batch_dicts == 2 * control.batches
        with pytest.raises(AssertionError, match="EQUITY_ENDPOINT_WORK"):
            control.assert_endpoints_lazy()
        work = _ProofWork()
        actual, _, _ = _run(sink, library, report, monkeypatch, work=work)
        support.assert_same(actual, expected)
        work.assert_endpoints_lazy()
        assert work.endpoint_dicts == 2 and work.eager_dicts == 0


@pytest.mark.parametrize("operation", (
    '_BULK_E_RUN.pattern.rpartition(b"{")',
    '_BULK_E_RUN.pattern.split(b",")',
    '_BULK_E_RUN.pattern.endswith(b"}")',
    '_BULK_E_RUN.pattern.rfind(b"{")',
    '_BULK_E_RUN.pattern.find(b"{")',
    '_BULK_E_RUN.pattern.index(b"{")',
    'b"128".isdigit()',
))
def test_equity_bound_counter_detects_repeated_pattern_inspection(monkeypatch, operation):
    prove = run_json._bulk_equity
    raw = ordinary.encoded(_point(0))
    expected, control = _observed_proof(raw, monkeypatch, old=True)
    with pytest.raises(AssertionError, match="EQUITY_BOUND_WORK"):
        control.assert_bounds_cached()
    actual, baseline = _observed_proof(raw, monkeypatch)
    assert actual == expected
    baseline.assert_bounds_cached()
    # This also operates on the immediate proof: it deliberately restores one
    # repeated operation without changing its return value.
    changed = _mutate(prove, "    if not raw:\n",
                      f"    {operation}\n    if not raw:\n")
    monkeypatch.setattr(run_json, "_bulk_equity", changed)
    actual, work = _observed_proof(raw, monkeypatch)
    assert actual == expected
    with pytest.raises(AssertionError, match="EQUITY_BOUND_WORK"):
        work.assert_bounds_cached()


def test_equity_bound_counter_detects_rederived_interval(monkeypatch):
    raw = ordinary.encoded(_point(0))
    expected, control = _observed_proof(raw, monkeypatch, old=True)
    with pytest.raises(AssertionError, match="EQUITY_BOUND_WORK"):
        control.assert_bounds_cached()
    actual, baseline = _observed_proof(raw, monkeypatch)
    assert actual == expected
    baseline.assert_bounds_cached()
    changed = _mutate(
        run_json._bulk_equity, "        lower, upper = _BULK_E_DEFAULT_BOUNDS\n",
        "        pattern = _BULK_E_RUN.pattern\n"
        '        opening = pattern.rfind(b"{")\n'
        '        comma = pattern.rfind(b",")\n'
        "        lower, upper = int(pattern[opening + 1:comma]), int(pattern[comma + 1:-1])\n")
    monkeypatch.setattr(run_json, "_bulk_equity", changed)
    actual, work = _observed_proof(raw, monkeypatch)
    assert actual == expected
    assert work.bound_calls == Counter(rfind=2)
    with pytest.raises(AssertionError, match="EQUITY_BOUND_WORK"):
        work.assert_bounds_cached()


@pytest.mark.parametrize("location", ("constructor", "trades"))
def test_equity_endpoint_counter_detects_dictionaries_outside_proof(
        tmp_path, monkeypatch, location):
    rows = []
    for index in range(5):
        rows.extend((_point(index), _trade(index + 1)))
    with _sink_case(tmp_path, rows) as (sink, library, report):
        control = _ProofWork()
        expected, _, _ = _run(
            sink, library, report, monkeypatch, old=True, work=control)
        assert expected.failure is None and control.batches >= 5
        assert control.batch_dicts == 2 * control.batches
        with pytest.raises(AssertionError, match="EQUITY_ENDPOINT_WORK"):
            control.assert_endpoints_lazy()
        baseline = _ProofWork()
        actual, _, _ = _run(sink, library, report, monkeypatch, work=baseline)
        support.assert_same(actual, expected)
        baseline.assert_endpoints_lazy()
        with monkeypatch.context() as patch:
            if location == "constructor":
                changed = _mutate(
                    run_json._BulkEquity.__init__, "    self.columns = columns\n",
                    "    self.columns = columns\n"
                    '    self._eager_point = dict(zip(("time_ms", "equity", "open_profit"), self._last))\n')
                patch.setattr(run_json._BulkEquity, "__init__", changed)
            else:
                changed = _mutate(
                    run_json._SinkReportDiagnostics.trades, "last_bulk = record",
                    'self.last = dict(zip(("time_ms", "equity", "open_profit"), record._last))\n'
                    "            last_bulk = record")
                patch.setattr(run_json._SinkReportDiagnostics, "trades", changed)
            work = _ProofWork()
            actual, _, _ = _run(sink, library, report, patch, work=work)
        support.assert_same(actual, expected)
        assert work.endpoint_dicts == 2
        if location == "constructor":
            assert work.eager_dicts == 0
            assert work.batch_dicts == work.batches
        else:
            assert work.batch_dicts == 0
            assert work.eager_dicts == work.batches
        with pytest.raises(AssertionError, match="EQUITY_ENDPOINT_WORK"):
            work.assert_endpoints_lazy()


@pytest.mark.parametrize("endpoint", ("first", "last"))
def test_equity_endpoint_counter_detects_eager_dictionaries(monkeypatch, endpoint):
    prove = run_json._bulk_equity
    # Mutations may select the exact operation to restore in their source copy.
    import inspect
    source = inspect.getsource(prove)
    if f"{endpoint} = (" in source:
        expression, replacement = (
            ("first = (timestamps[0], numbers[0], numbers[count])",
             "first = dict(time_ms=timestamps[0], equity=numbers[0], open_profit=numbers[count])")
            if endpoint == "first" else
            ("last = (timestamps[-1], numbers[count - 1], numbers[-1])",
             "last = dict(time_ms=timestamps[-1], equity=numbers[count - 1], open_profit=numbers[-1])"))
        changed = _mutate(prove, expression, replacement)
    else:
        changed = _mutate(
            prove, "    count = len(timestamps)\n",
            "    dict(time_ms=timestamps[0], equity=numbers[0], open_profit=numbers[0])\n"
            "    count = len(timestamps)\n")
    monkeypatch.setattr(run_json, "_bulk_equity", changed)
    work = _ProofWork()
    with work.install(monkeypatch):
        for index in range(3):
            assert run_json._bulk_equity(ordinary.encoded(_point(index))) is not None
    with pytest.raises(AssertionError, match="EQUITY_ENDPOINT_WORK"):
        work.assert_endpoints_lazy()


def test_equity_endpoint_counter_detects_decoding_every_last_point(tmp_path, monkeypatch):
    import inspect
    trades = run_json._SinkReportDiagnostics.trades
    source = inspect.getsource(trades)
    if "last_bulk = record" in source:
        changed = _mutate(
            trades, "last_bulk = record",
            "self.last = record.last\n            last_bulk = record")
    else:
        changed = _mutate(
            trades, "self.last = record.last",
            "self.last = record.last\n            record.last")
    rows = []
    for index in range(5):
        rows.extend((_point(index), _trade(index + 1)))
    with _sink_case(tmp_path, rows) as (sink, library, report):
        expected, _, _ = _run(sink, library, report, monkeypatch, old=True)
        monkeypatch.setattr(run_json._SinkReportDiagnostics, "trades", changed)
        work = _ProofWork()
        actual, _, _ = _run(sink, library, report, monkeypatch, work=work)
        support.assert_same(actual, expected)
        with pytest.raises(AssertionError, match="EQUITY_ENDPOINT_WORK"):
            work.assert_endpoints_lazy()


@pytest.mark.parametrize("family", (
    "empty", "e-before-t", "t-before-e", "interleaved", "escaped", "numeric-edges",
    "window-variant", "batch-boundary", "page-boundary", "named", "outputs",
))
def test_complete_report_families_match_immediate_writer(tmp_path, monkeypatch, family):
    rows, outputs, named, anchor, edit = _synthetic_case(family)
    with _sink_case(tmp_path, rows, outputs=outputs, named=named) as args:
        if edit is not None:
            sink, library, _ = args
            fixtures._rewrite_body(sink, library, edit)
        outcome = _pair(
            *args, monkeypatch, outputs=outputs, window_anchor=anchor,
            configure=_configure_outputs if outputs else None)
        assert outcome.failure is None


def _float_shapes():
    values = [-0.0, 0.0, 1.0, -1.0, 5e-324, -5e-324,
              1e16, 1e-5, 1e-4, 1e-6, 9999999999999998.0,
              1.0000000000000002e16, 1.2345678901234567,
              sys.float_info.max, -sys.float_info.max, sys.float_info.min]
    rng = random.Random(81791)
    # Every exponent class, including subnormals; sign and significand vary.
    for exponent in range(2047):
        bits = (rng.getrandbits(1) << 63) | (exponent << 52) | rng.getrandbits(52)
        values.append(struct.unpack(">d", bits.to_bytes(8, "big"))[0])
    assert all(map(math.isfinite, values))
    return values


def test_report_path_with_finite_trades_and_escaped_sink_ids_matches_reference(
        tmp_path, monkeypatch):
    # The report path verifies these sink strings, then drops ids/comments from
    # report trades. These rows exercise the whole path, not the trade encoder.
    strings = [
        "", "ordinary", 'quote"\\/\b\f\n\r\t',
        "".join(map(chr, range(32))), "\u007f\u0080漢\U0001f600",
        "id-" + "x" * (2 * 65536 + 17),
    ]
    rows = [_point(0)]
    values = _float_shapes()
    for index, value in enumerate(values):
        rows.append(_trade(
            index + 1, entry_price=value, exit_price=value, qty=value,
            pnl=-0.0, pnl_pct=value, max_runup=value, max_drawdown=value,
            commission=value, entry_id=strings[index % 5],
            exit_id=strings[(index + 1) % 5],
            exit_comment=strings[(index + 2) % 5]))
    rows.append(_trade(len(values) + 1, entry_id=strings[-1],
                       exit_comment=strings[-1] + strings[2]))
    with _sink_case(tmp_path, rows) as args:
        outcome = _pair(*args, monkeypatch)
        assert outcome.failure is None
        assert len(json.loads(outcome.report)["trades"]) == len(values) + 1


@pytest.mark.parametrize("wire", _EQUITY_WIRES)
def test_equity_strict_fallback_keeps_error_line_and_envelope(tmp_path, monkeypatch, wire):
    rows = [_point(0), _trade(1), _point(1), _trade(2), _point(2)]
    with _sink_case(tmp_path, rows) as (sink, library, report):
        old = ordinary.encoded(rows[2])
        fixtures._rewrite_body(
            sink, library, lambda body: body.replace(old, wire, 1))
        outcome = _pair(sink, library, report, monkeypatch)
        if outcome.failure is not None:
            assert "line 4:" in outcome.failure[2] or "line 5:" in outcome.failure[2]


@pytest.mark.parametrize("tag", ("T", "E"))
@pytest.mark.parametrize("position", (0, 8, 16), ids=("first", "middle", "last"))
@pytest.mark.parametrize("fault", ("write", "flush", "close", "short"))
def test_column_faults_at_rows_and_batches_match_immediate_writer(
        tmp_path, monkeypatch, tag, position, fault):
    """Explicit caller flushes expose each chosen row/batch's physical faults."""
    parts = []
    for index in range(17):
        if tag == "T":
            parts.append((_trade_text(_trade(index + 1)), 1))
        else:
            batch = reference._bulk_equity(b"".join(
                ordinary.encoded(_point(index * 12 + i)) for i in range(12)))
            parts.append((reference._bulk_equity_ascii(batch), 12))

    def run(old):
        context = reference.install(monkeypatch) if old else nullcontext()
        fired, phase, published = [], [-1], []
        with context, ExitStack() as resources, monkeypatch.context() as patch:
            column = run_json._JSONColumn(resources, str(tmp_path))
            target = column.file
            write, flush, close = target.write_ascii, target.flush, target.close
            libc = target.libc

            def trip(name):
                if fault == name and phase[0] == position and not fired:
                    fired.append((name, phase[0]))
                    raise OSError(errno.ENOSPC, "injected column storage failure")

            def written(data):
                trip("write")
                return write(data)

            def flushed():
                trip("flush")
                return flush()

            def closed():
                close()
                trip("close")

            def pwrite(fd, address, count, offset):
                if fault == "short" and phase[0] == position and not fired:
                    fired.append(("short", phase[0]))
                    return count - 4096
                return libc.pwrite(fd, address, count, offset)

            patch.setattr(target, "write_ascii", written)
            patch.setattr(target, "flush", flushed)
            patch.setattr(target, "close", closed)
            patch.setattr(target, "libc", types.SimpleNamespace(
                pwrite=pwrite, pread=libc.pread))

            def produce():
                for index, (data, count) in enumerate(parts):
                    phase[0] = index
                    column.append_ascii(data, row_count=count)
                    target.flush()
                    if fault == "close" and index == position:
                        target.close()
                    yield index

            with pytest.raises(run_json.RunFailure) as caught:
                artifact = run_json._prepare_stream_report(
                    {"rows": run_json._JSONRecordArray(produce)}, resources,
                    str(tmp_path))
                published.append(artifact.read())
            envelope = support.failure_envelope(run_json, caught.value)
            assert fired == [(fault, position)] and not published
            assert caught.value.code == "report_sink_failed"
            assert caught.value.code_args == {"reason": "scratch_io"}
            assert str(caught.value) == "report sink write failed"
        assert target.closed and target.buffer is None
        return envelope

    assert run(False) == run(True)


@pytest.mark.parametrize("tag", ("E", "T"))
@pytest.mark.parametrize("position", (0, 300, 600), ids=("first", "middle", "last"))
@pytest.mark.parametrize("competing", (
    "none", "format", "trailer", "receipt", "trade", "equity", "summary", "metric",
))
def test_deferred_spool_failure_keeps_report_refusal_precedence(
        tmp_path, monkeypatch, tag, position, competing):
    rows = []
    for index in range(601):
        rows.extend((_point(index), _trade(index + 1)))
    if competing == "trade":
        rows[-1] = _trade(601, pnl=None)
    elif competing == "equity":
        rows[-2] = ["E", ordinary.bars.T0 + 600, None, 0.0]
    with _sink_case(
            tmp_path, rows, fault="incomplete" if competing == "receipt" else None
    ) as (sink, library, report):
        if competing == "format":
            fixtures._rewrite_body(
                sink, library, lambda body: body.replace(
                    ordinary.encoded(rows[-2]), b'["E",1,10000.00,0.0]\n', 1))
        elif competing == "trailer":
            raw = Path(sink.path).read_bytes()
            cut = raw.rfind(b'\n{"trailer":') + 1
            trailer = json.loads(raw[cut:])
            trailer["counts"]["E"] += 1
            Path(sink.path).write_bytes(raw[:cut] + ordinary.encoded(trailer))
        elif competing == "summary":
            report.net_profit = float("inf")
        elif competing == "metric":
            report.metrics.all.profit_factor = float("inf")

        if tag == "T":
            pieces = [_trade_text(row) for row in rows if row[0] == "T"]
        else:
            pieces = [json.dumps(
                dict(zip(("time_ms", "equity", "open_profit"), row[1:])),
                separators=(",", ":"), allow_nan=False).encode("ascii")
                for row in rows if row[0] == "E"]
        threshold = sum(len(piece) + 1 for piece in pieces[:position])

        def run(old):
            context = reference.install(monkeypatch) if old else nullcontext()
            fired, bytes_seen = [], defaultdict(int)
            with context, monkeypatch.context() as patch:
                prepare = run_json._ReportSink._prepare_fused_artifacts
                write = run_json._BoundedJSONFile.write_ascii
                target_files = []

                def prepared(target, *args):
                    prepare(target, *args)
                    column = target._trades_spool if tag == "T" else target._equity_spool
                    target_files.append(column.file)

                def written(file, data):
                    if file in target_files:
                        end = bytes_seen[file] + len(data)
                        if end > threshold and not fired:
                            fired.append(True)
                            raise OSError(errno.ENOSPC, "injected column storage failure")
                        bytes_seen[file] = end
                    return write(file, data)

                patch.setattr(run_json._ReportSink, "_prepare_fused_artifacts", prepared)
                patch.setattr(run_json._BoundedJSONFile, "write_ascii", written)
                outcome, _, _ = _run(sink, library, report, patch)
            assert outcome.report == b"" and outcome.failure is not None
            # A malformed last record may refuse before that record is spooled.
            assert fired or competing in ("format", "trailer", "receipt")
            return outcome

        actual, expected = run(False), run(True)
        support.assert_same(actual, expected)
        if competing == "none":
            assert actual.failure[2:5] == (
                "report sink write failed", "report_sink_failed", {"reason": "scratch_io"})
            assert type(actual.error.__cause__) is OSError
        else:
            assert actual.failure[3] != "report_sink_failed"


@pytest.mark.parametrize("tag", ("T", "E"))
def test_fused_short_write_has_the_same_failure_envelope(
        tmp_path, monkeypatch, tag, artifact_window):
    """Compare full-window and tail faults at three physical spool offsets."""
    windows = support.spool_windows(tmp_path)
    assert windows[tag] == artifact_window
    counts = {
        "E": max(601, 3 * windows["E"] // _column_row_width(_point(0)) + 2),
        "T": max(601, 3 * windows["T"] // _column_row_width(_trade(1)) + 2),
    }
    rows = []
    for index in range(max(counts.values())):
        if index < counts["E"]:
            rows.append(_point(index))
        if index < counts["T"]:
            rows.append(_trade(index + 1))
    parts = ([_trade_text(row) for row in rows if row[0] == "T"] if tag == "T"
             else [reference._bulk_equity_ascii(reference._bulk_equity(
                 ordinary.encoded(row))) for row in rows if row[0] == "E"])
    size = sum(map(len, parts)) + len(parts) - 1
    window = windows[tag]
    assert size > 3 * window and size % window
    offsets = (
        ("first", 0),
        ("middle", size // window // 2 * window),
        ("last", size // window * window),
    )
    observed = {False: [], True: []}
    with _sink_case(tmp_path, rows) as (sink, library, report):
        assert Path(sink.path).stat().st_size < 32 * 1024 * 1024

        def run(old, position, fault_offset):
            context = reference.install(monkeypatch) if old else nullcontext()
            with context, monkeypatch.context() as patch:
                prepare = run_json._ReportSink._prepare_fused_artifacts
                prepare_report = run_json._prepare_report_json
                temporary = run_json._json_temporary
                files, writes, fired, verified, preparing = [], [], [], [], []

                def opened(*args, **kwargs):
                    file = temporary(*args, **kwargs)
                    files.append(file)
                    return file

                def prepared(target, *args):
                    prepare(target, *args)
                    file = (target._trades_spool if tag == "T" else target._equity_spool).file
                    assert file.window == window
                    target_fd = file.fileno()
                    libc = file.libc

                    def pwrite(fd, address, count, offset):
                        assert fd == target_fd
                        writes.append((fd, offset, count))
                        if offset == fault_offset and not fired:
                            fired.append((fd, offset, bool(verified), bool(preparing)))
                            return count - 4096
                        return libc.pwrite(fd, address, count, offset)

                    file.libc = types.SimpleNamespace(pwrite=pwrite, pread=libc.pread)

                def report_started(*args, **kwargs):
                    preparing.append(True)
                    return prepare_report(*args, **kwargs)

                patch.setattr(run_json, "_json_temporary", opened)
                patch.setattr(run_json._ReportSink, "_prepare_fused_artifacts", prepared)
                patch.setattr(run_json, "_prepare_report_json", report_started)
                outcome, _, _ = _run(
                    sink, library, report, patch,
                    after_verify=lambda *args: verified.append(True))
            assert len(fired) == 1 and outcome.report == b""
            fd, offset, after_verify, after_prepare = fired[0]
            assert offset == fault_offset
            assert [(write_fd, write_offset) for write_fd, write_offset, _ in writes] == [
                (fd, index * window) for index in range(fault_offset // window + 1)]
            if position == "last":
                assert after_verify and after_prepare
                assert writes[-1][2] == (size % window + 4095) // 4096 * 4096
            else:
                assert not after_verify and not after_prepare
                assert writes[-1][2] == window
            assert outcome.failure[2:5] == (
                "report sink write failed", "report_sink_failed", {"reason": "scratch_io"})
            assert str(outcome.error.__cause__) == "short report artifact write"
            assert files and all(file.closed and file.buffer is None for file in files)
            assert all(file.size < 32 * 1024 * 1024 for file in files)
            assert list(tmp_path.iterdir()) == [Path(sink.path)]
            observed[old].append((fd, offset))
            return outcome

        for position, fault_offset in offsets:
            support.assert_same(run(False, position, fault_offset),
                                run(True, position, fault_offset))
    for faults in observed.values():
        assert len(set(faults)) == len({offset for _, offset in faults}) == 3
    assert list(tmp_path.iterdir()) == []


def _proof_identity(raw):
    expected = reference._bulk_equity(raw)
    actual = run_json._bulk_equity(raw)
    assert (actual is None) == (expected is None), "EQUITY_PROOF_IDENTITY"
    if expected is not None:
        assert actual.count == expected.count, "EQUITY_PROOF_IDENTITY"
        assert actual.first == expected.first and actual.last == expected.last, (
            "EQUITY_PROOF_IDENTITY")
        assert run_json._bulk_equity_ascii(actual) == reference._bulk_equity_ascii(expected), (
            "EQUITY_PROOF_IDENTITY")


@pytest.mark.parametrize("guard", (
    "timestamp-low", "timestamp-high", "finite", "repr", "coverage", "lower", "upper",
))
def test_equity_identity_detects_dropped_row_proofs(monkeypatch, guard):
    line = ordinary.encoded(_point(0))
    prove = run_json._bulk_equity
    if guard.startswith("timestamp"):
        timestamp = -(1 << 53) if guard.endswith("low") else (1 << 53)
        raw = f'["E",{timestamp},1.0,0.0]\n'.encode("ascii")
        changed = _mutate(
            prove, "if min(timestamps) < -((1 << 53) - 1) or max(timestamps) > (1 << 53) - 1:",
            "if False:")
    elif guard == "repr":
        raw = b'["E",1,0.10,0.0]\n'
        changed = _mutate(prove, " or list(map(repr, numbers)) != texts", "")
    elif guard == "finite":
        # Extend both grammar seams to isolate finite from the repr check:
        # repr(float("inf")) is "inf", yet the strict reader still refuses it.
        old = run_json._BULK_E_FLOAT
        pattern = re.compile(run_json._BULK_E_LINE.pattern.replace(
            old, rb"(?:" + old + rb"|inf)"))
        run = re.compile(rb"(?:" + pattern.pattern + rb"){1,128}")
        monkeypatch.setattr(run_json, "_BULK_E_LINE", pattern)
        monkeypatch.setattr(run_json, "_BULK_E_RUN", run)
        monkeypatch.setattr(reference, "_BULK_E_LINE", pattern)
        monkeypatch.setattr(reference, "_BULK_E_RUN", run)
        raw = b'["E",1,inf,0.0]\n'
        changed = _mutate(prove, "not all(map(math.isfinite, numbers))", "False")
    elif guard == "coverage":
        raw = line + b"x" + line
        changed = _mutate(
            prove,
            "if (sum(map(len, times)) + sum(map(len, equities)) + sum(map(len, profits))\n"
            "            + _BULK_E_LITERAL_BYTES * len(rows) != len(raw)):",
            "if False:")
    else:
        run = re.compile(rb"(?:" + run_json._BULK_E_LINE.pattern + rb"){2,128}")
        monkeypatch.setattr(run_json, "_BULK_E_RUN", run)
        monkeypatch.setattr(reference, "_BULK_E_RUN", run)
        raw = line if guard == "lower" else line * 129
        changed = _mutate(prove, "if not lower <= len(rows) <= upper:", "if False:")
    _proof_identity(raw)
    monkeypatch.setattr(run_json, "_bulk_equity", changed)
    with pytest.raises(AssertionError, match="EQUITY_PROOF_IDENTITY"):
        _proof_identity(raw)
