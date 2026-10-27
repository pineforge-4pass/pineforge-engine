"""CR-397 red contract, based on 30e9f9a0 (post #529/#534/#536).

No optional-symbol skips. CTest supplies a whole-archive module built from the
real tutorial/macd/generated.cpp. T-bearing acceptance requires native prepare
and both native guards, with no Python E/T parsing or per-record encoding.
No code in this file has been executed by the executor on the Mac.
"""
from __future__ import annotations

import contextlib
import copy
import ctypes
import hashlib
import io
import json
import math
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(os.environ.get("PINEFORGE_CR397_ENGINE_ROOT", Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(ROOT / "docker"))
import pf_sink
import run_json
import test_run_json_sink as fixtures


@pytest.fixture
def native_library():
    module = os.environ.get("PINEFORGE_CR397_GENERATED_SO")
    assert module, "lab must supply PINEFORGE_CR397_GENERATED_SO (real generated strategy)"
    assert Path(module).is_file(), module
    lib = run_json.load_strategy(Path(module))
    # Red on 30e9f9a0 for the missing facility, never a skip/fallback assertion.
    function = lib.pf_report_sink_json_v1
    assert callable(function)
    return module, lib, function


def wire(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii") + b"\n"


def packed(rows, *, header=None, trailer_change=None):
    header = fixtures.header() if header is None else header
    encoded = [row if isinstance(row, bytes) else wire(row) for row in rows]
    counts = {d["type"]: 0 for d in header["records"]} if isinstance(header["records"], list) else {}
    for line in encoded:
        try:
            row = json.loads(line)
        except (ValueError, UnicodeError):
            continue
        if isinstance(row, list) and row and isinstance(row[0], str) and row[0] in counts:
            counts[row[0]] += 1
    prefix = wire(header) + b"".join(encoded)
    trailer = {"trailer": "pf-report-sink", "version": 1, "counts": counts,
               "sha256": hashlib.sha256(prefix).hexdigest()}
    if trailer_change:
        trailer_change(trailer)
    return prefix + wire(trailer)


def raw_call(function, path, *, verified=None, tag="E", expected_count=None,
             output=None, trades_output=None):
    header = ctypes.create_string_buffer(1048577)
    error = ctypes.create_string_buffer(1048577)
    request = run_json.ReportSinkJSONRequestC()
    request.struct_size, request.version = ctypes.sizeof(request), 1
    request.operation = 1 if verified is None else 2
    request.trades_fd = request.equity_fd = -1
    if output is not None:
        request.equity_fd = output.fileno()
    if trades_output is not None:
        request.trades_fd = trades_output.fileno()
    request.header_buffer, request.header_capacity = ctypes.addressof(header), len(header)
    request.error_buffer, request.error_capacity = ctypes.addressof(error), len(error)
    kept_header = None
    if verified is not None:
        old, raw_header = verified
        request.guard_tag = ord(tag)
        request.expected_ino, request.expected_size = old.source_ino, old.source_size
        request.expected_counts[:] = old.counts[:]
        if expected_count is not None:
            request.expected_counts["ETOVKXH".index(tag)] = expected_count
        request.expected_sha256_hex = bytes(old.sha256_hex)
        kept_header = ctypes.create_string_buffer(raw_header)
        request.verified_header = ctypes.addressof(kept_header)
        request.verified_header_bytes = len(raw_header)
    result = run_json.ReportSinkJSONResultC()
    result.struct_size, result.version = ctypes.sizeof(result), 1
    with path.open("rb", buffering=0) as source:
        request.sink_fd = source.fileno()
        assert function(ctypes.byref(request), ctypes.byref(result)) == 0
    return result, header.raw[:result.header_bytes], error.raw[:result.error_bytes]


def guard_refusal(result, original_header, fresh_header, error, tag="E"):
    run_json._check_native_sink_guard_header(result, fresh_header)
    run_json._raise_native_sink_json(result, original_header, error, guard_tag=tag)


def test_native_accepted_numbers_and_decreasing_times(native_library, tmp_path):
    numbers = [-0.0, 0.0, 5e-324, 2.2250738585072014e-308,
               1.7976931348623157e308, 1e-05, 0.0001, 1e15, 1e16,
               0.1 + 0.2, -1.2345678901234567]
    header = fixtures.header()
    header["run_id"] = 'quote"\n漢𝄞'
    rows = [["E", 9007199254740991 - i, value, -value]
            for i, value in enumerate(numbers)]
    path = tmp_path / "accepted.sink"
    path.write_bytes(packed(rows, header=header))
    with pf_sink.SinkReader(path) as reader:
        expected = b",".join(json.dumps(row.as_dict(), separators=(",", ":")).encode()
                             for row in reader)
        digest = reader.sha256
    with (tmp_path / "fragment").open("w+b", buffering=0) as output:
        result, raw_header, _ = raw_call(native_library[2], path, output=output)
        assert result.status == 0 and result.complete
        assert bytes(result.sha256_hex).decode() == digest
        assert raw_header == wire(header)
        assert output.read() == expected
        assert result.equity_json_bytes == len(expected)
        assert bytes(result.equity_json_sha256_hex).decode() == hashlib.sha256(expected).hexdigest()


def refusal(call):
    try:
        call()
    except run_json.RunFailure as error:
        return type(error), error.code, error.code_args, str(error)
    raise AssertionError("corrupted sink was accepted")


def strict(path):
    try:
        with pf_sink.SinkReader(path) as reader:
            for _ in reader:
                pass
    except pf_sink.SinkFormatError as error:
        raise run_json._engine_invariant(
            "the engine's report sink is invalid: " + str(error)) from error


def configured_sink(function, path):
    sink = run_json._ReportSink.__new__(run_json._ReportSink)
    sink.directory, sink.path = str(path.parent), str(path)
    sink.header, sink.counts, sink.sha256 = {}, {}, ""
    sink._identity = sink.summary = sink.summary_failure = sink.diagnostics = None
    sink.native_json = None
    sink._native_json_function = function
    sink.named, sink.keep, sink.closed = True, False, False
    sink.run_id = fixtures.header()["run_id"]
    return sink


# Handwritten known answers, independent of the engine writer and codec.
TRADE_ONE = (
    b'["T",1,"long",-0,60000,100.0,110.0,2.0,20.0,10.0,25.0,-2.0,0.5,'
    b'0,1,false,9007199254740991,1,"id\\ud834\\udd1e","exit","quote\\"\\n"]\n')
TRADE_TWO = (
    b'["\\u0054",2,"short",60000,120000,110.0,115.0,1.0,-5.0,-5.0,1e+16,-5.0,-0.0,'
    b'1,2,true,2,0,"","",""]\n')
TRADE_THREE = (
    b'["T",3,"long",120000,180000,115.0,115.0,5e-324,-0.0,1e-05,0.0,-0.0,0.0,'
    b'2,3,true,3,0,"","",""]\n')
TRADE_ONE_JSON = (
    b'{"n":1,"side":"long","entry_time":0,"exit_time":60000,'
    b'"entry_price":100.0,"exit_price":110.0,"qty":2.0,"pnl":20.0,'
    b'"pnl_pct":10.0,"max_runup":25.0,"max_drawdown":-2.0,"commission":0.5,'
    b'"entry_bar_index":0,"exit_bar_index":1,"open_at_end":false,'
    b'"entry_incarnation":9007199254740991}')
TRADE_TWO_JSON = (
    b'{"n":2,"side":"short","entry_time":60000,"exit_time":120000,'
    b'"entry_price":110.0,"exit_price":115.0,"qty":1.0,"pnl":-5.0,'
    b'"pnl_pct":-5.0,"max_runup":1e+16,"max_drawdown":-5.0,"commission":-0.0,'
    b'"entry_bar_index":1,"exit_bar_index":2,"open_at_end":true,"entry_incarnation":2}')
TRADE_THREE_JSON = (
    b'{"n":3,"side":"long","entry_time":120000,"exit_time":180000,'
    b'"entry_price":115.0,"exit_price":115.0,"qty":5e-324,"pnl":-0.0,'
    b'"pnl_pct":1e-05,"max_runup":0.0,"max_drawdown":-0.0,"commission":0.0,'
    b'"entry_bar_index":2,"exit_bar_index":3,"open_at_end":true,"entry_incarnation":3}')
EQUITY_JSON = (
    b'{"time_ms":0,"equity":10000.0,"open_profit":-0.0},'
    b'{"time_ms":1,"equity":10001.25,"open_profit":5e-324},'
    b'{"time_ms":2,"equity":1e+16,"open_profit":1e-05}')


class ReceiptFromStream:
    def __init__(self, path, *, complete=True):
        with pf_sink.SinkReader(path) as reader:
            for _ in reader:
                pass
            self.counts, self.digest = reader.counts, reader.sha256
        self.complete = complete
        self.queries = 0

    def strategy_report_sink_receipt_v1(self, state, pointer):
        self.queries += 1
        receipt = pointer._obj
        for tag in "ETOVKXH":
            setattr(receipt, "count_" + tag.lower(), self.counts.get(tag, 0))
        receipt.sha256_hex = self.digest.encode("ascii")
        receipt.complete = int(self.complete)
        return 0


@pytest.mark.parametrize("count", (0, 2, 3))
def test_native_literal_trade_fragments_and_host_summary(native_library, tmp_path, count):
    rows = [b'["E",-0,10000.0,-0.0]\n']
    if count:
        rows += [TRADE_ONE]
    rows += [b'["\\u0045",1,10001.25,5e-324]\n']
    if count:
        rows += [TRADE_TWO]
    rows += [b'["E",2,1e+16,1e-05]\n']
    if count == 3:
        rows += [TRADE_THREE]
    expected = (b"" if not count else TRADE_ONE_JSON + b"," + TRADE_TWO_JSON
                + (b"," + TRADE_THREE_JSON if count == 3 else b""))
    path = tmp_path / "literal.sink"
    path.write_bytes(packed(rows))
    with (tmp_path / "equity").open("w+b", buffering=0) as equity, (
            tmp_path / "trades").open("w+b", buffering=0) as trades:
        native, _, _ = raw_call(native_library[2], path, output=equity, trades_output=trades)
        assert native.status == 0 and native.complete == 1
        assert native.counts[:2] == [3, count]
        assert equity.read() == EQUITY_JSON
        assert trades.read() == expected
        assert native.trades_json_bytes == len(expected)
        assert bytes(native.trades_json_sha256_hex).decode() == hashlib.sha256(expected).hexdigest()
        assert native.summary.count_t == count and native.summary.complete == 1
        assert (native.summary.wins, native.summary.losses) == ((1, 1) if count else (0, 0))
        assert (native.summary.best_trade, native.summary.worst_trade,
                native.summary.max_drawdown) == ((20.0, -5.0, -5.0) if count else (0.0, 0.0, 0.0))
        assert native.diagnostics.trade_at_start == bool(count)
    report = run_json.ReportC()
    report.total_trades = count
    report.input_bars_processed = report.script_bars_processed = 3
    report.net_profit = 99.5  # Intentionally NOT the sink PnL sum of 15.0.
    receipt = ReceiptFromStream(path)
    documents = []
    for function in (None, native_library[2]):
        sink = configured_sink(function, path)
        sink.named, sink.keep = False, True
        try:
            sink.verify(receipt, None, report)
            if function is not None:
                assert sink.native_json.accepted, "T-bearing sink fell back"
            document = run_json._build_sink_report(
                report, Path("fixed.csv"), 3, 0, 2, 0.0, {}, {}, {}, sink)
            buffer = io.StringIO()
            run_json._stream_json(document, buffer)
            documents.append(buffer.getvalue().encode("ascii"))
        finally:
            sink.close()
    assert documents[0] == documents[1]
    assert b'"trades":[' + expected + b']' in documents[1]
    assert b'"equity_curve":[' + EQUITY_JSON + b']' in documents[1]
    summary = json.loads(documents[1])["summary"]
    assert summary == {
        "total_trades": count, "wins": int(bool(count)), "losses": int(bool(count)),
        "win_rate_pct": 0.0 if not count else 50.0 if count == 2 else 33.3333,
        "net_pnl": 99.5, "avg_trade": 0.0 if not count else 49.75 if count == 2 else 33.166666666666664,
        "best_trade": 20.0 if count else 0.0, "worst_trade": -5.0 if count else 0.0,
        "max_drawdown": -5.0 if count else 0.0, "bars_processed": 3}
    assert receipt.queries == 2


def production_verify(function, path):
    # Real adapter and real native codec; only the engine receipt is forbidden,
    # because every corrupted source must refuse BEFORE that boundary.
    sink = configured_sink(function, path)
    class NoReceipt:
        def strategy_report_sink_receipt_v1(self, *args):
            raise AssertionError("corrupt source reached engine-receipt admission")
    try:
        sink.verify(NoReceipt(), None, run_json.ReportC())
    finally:
        sink.close()


@pytest.mark.parametrize("field", (2, 3))
@pytest.mark.parametrize("earlier", ("none", "summary", "receipt", "run-id"))
def test_native_null_result_keeps_admission_and_small_result_priority(
        native_library, tmp_path, field, earlier):
    point = ["E", 0, 1.0, 0.0]
    point[field] = None
    path = tmp_path / "result.sink"
    path.write_bytes(packed([point, ["E", 60000, 2.0, 0.0]]))
    with pf_sink.SinkReader(path) as reader:
        for _ in reader:
            pass
        counts, digest = reader.counts, reader.sha256
    class ReceiptFromActualStream:
        def strategy_report_sink_receipt_v1(self, state, pointer):
            receipt = pointer._obj
            for tag in "ETOVKXH":
                setattr(receipt, "count_" + tag.lower(), counts.get(tag, 0))
            receipt.sha256_hex = digest.encode("ascii")
            receipt.complete = 0 if earlier == "receipt" else 1
            return 0
    def assemble(function):
        sink = configured_sink(function, path)
        if earlier == "run-id":
            sink.run_id += "-different"
        report = run_json.ReportC()
        report.input_bars_processed = report.script_bars_processed = 2
        if earlier == "summary":
            report.net_profit = float("inf")
        try:
            sink.verify(ReceiptFromActualStream(), None, report)
            run_json._build_sink_report(report, Path("fixed.csv"), 2, 0, 60000,
                                       0.0, {}, {}, {}, sink)
        finally:
            sink.close()
    expected = refusal(lambda: assemble(None))
    actual = refusal(lambda: assemble(native_library[2]))
    assert actual == expected
    if earlier == "none":
        assert actual[1:3] == ("report_result_invalid",
                              {"field": "equity_curve." + ("equity" if field == 2 else "open_profit"),
                               "reason": "nonfinite"})


@pytest.mark.parametrize("field", range(5, 13))
@pytest.mark.parametrize("earlier", ("none", "order", "receipt", "run-id"))
def test_native_trade_result_priority(native_library, tmp_path, field, earlier):
    path = tmp_path / "trade-result.sink"
    trade = trade_bad(field, None)
    if earlier == "order":
        trade[1] = 2
    # An earlier E failure and an invalid small summary must both lose to T.
    path.write_bytes(packed([["E", 0, None, 0.0], trade, ["E", 1, 2.0, 0.0]]))
    report = run_json.ReportC()
    report.total_trades = 1
    report.input_bars_processed = report.script_bars_processed = 2
    report.net_profit = float("inf")
    def assemble(function):
        receipt = ReceiptFromStream(path, complete=earlier != "receipt")
        sink = configured_sink(function, path)
        if earlier == "run-id":
            sink.run_id += "-wrong"
        try:
            sink.verify(receipt, None, report)
            run_json._build_sink_report(report, Path("fixed.csv"), 2, 0, 1,
                                       0.0, {}, {}, {}, sink)
        finally:
            sink.close()
    expected = refusal(lambda: assemble(None))
    actual = refusal(lambda: assemble(native_library[2]))
    assert actual == expected
    if earlier == "none":
        assert actual[1:3] == (
            "report_result_invalid",
            {"field": "trades." + fixtures.TRADE_FIELDS[field - 1], "reason": "nonfinite"})
    elif earlier == "order":
        assert actual[3] == "the report sink trade rows are out of report order"


@pytest.mark.parametrize("pnls,expected_field", (
    ((-0.0, 0.0, -0.0), None),
    ((0.1, 0.2, -0.3), None),
    ((1e16, 1.0, -1e16), None),
    ((1.7976931348623157e308, 1.7976931348623157e308), "summary.max_drawdown"),
    ((-1.7976931348623157e308, -1.7976931348623157e308), "summary.max_drawdown"),
    ((1.7976931348623157e308, -1.7976931348623157e308, -1.7976931348623157e308),
     "summary.max_drawdown"),
))
def test_native_trade_left_folds_and_overflow(native_library, tmp_path, pnls, expected_field):
    rows = []
    for index, pnl in enumerate(pnls):
        row = trade_bad(8, pnl)
        row[1], row[13] = index + 1, index + 1
        rows.append(row)
    path = tmp_path / "trade-fold.sink"
    path.write_bytes(packed(rows))
    report = run_json.ReportC()
    report.total_trades = len(rows)
    report.net_profit = 77.0
    def fold(function):
        sink = configured_sink(function, path)
        try:
            sink.verify(ReceiptFromStream(path), None, report)
            assert sink.diagnostics.trade_at_start is False
            return wire(run_json._sink_summary(report, sink))
        finally:
            sink.close()
    if expected_field:
        expected = refusal(lambda: fold(None))
        assert refusal(lambda: fold(native_library[2])) == expected
        assert expected[1:3] == ("report_result_invalid",
                                 {"field": expected_field, "reason": "nonfinite"})
    else:
        assert fold(native_library[2]) == fold(None)
        if pnls[0] == 0.0 and math.copysign(1.0, pnls[0]) < 0:
            assert b'"best_trade":-0.0,"worst_trade":-0.0,"max_drawdown":0.0' in fold(native_library[2])


@pytest.mark.parametrize("case", ("later-source", "trailer", "first-trade"))
def test_native_first_trade_failure_is_deferred_but_not_replaced(native_library, tmp_path, case):
    first = trade_bad(12, None)
    second = trade_bad(1, 5)
    path = tmp_path / "trade-priority.sink"
    if case == "later-source":
        path.write_bytes(packed([first, b'["E",0,1e4,0.0]\n']))
    elif case == "trailer":
        path.write_bytes(packed([first], trailer_change=lambda t: t.update(sha256="0" * 64)))
    else:
        path.write_bytes(packed([first, second]))
    if case != "first-trade":
        assert refusal(lambda: production_verify(native_library[2], path)) == refusal(lambda: strict(path))
        return
    report = run_json.ReportC()
    report.total_trades = 2
    def assemble(function):
        sink = configured_sink(function, path)
        try:
            sink.verify(ReceiptFromStream(path), None, report)
            run_json._sink_summary(report, sink)
        finally:
            sink.close()
    expected = refusal(lambda: assemble(None))
    assert refusal(lambda: assemble(native_library[2])) == expected
    assert expected[1:3] == ("report_result_invalid",
                             {"field": "trades.commission", "reason": "nonfinite"})


def test_native_descriptor_layout():
    assert ctypes.sizeof(run_json.ReportSinkJSONRequestC) == 216
    assert ctypes.sizeof(run_json.ReportSinkJSONResultC) == 960
    assert ctypes.sizeof(run_json.ReportSinkJSONErrorC) == 496
    assert ctypes.alignment(run_json.ReportSinkJSONRequestC) == 8
    assert ctypes.alignment(run_json.ReportSinkJSONResultC) == 8
    assert ctypes.alignment(run_json.ReportSinkJSONErrorC) == 8
    for name, offset in (
            ("struct_size", 0), ("version", 4), ("operation", 8), ("flags", 12),
            ("sink_fd", 16), ("trades_fd", 20), ("equity_fd", 24), ("guard_tag", 28),
            ("expected_ino", 32), ("expected_size", 40), ("expected_counts", 48),
            ("expected_sha256_hex", 104), ("reserved", 169), ("verified_header", 176),
            ("verified_header_bytes", 184), ("header_capacity", 188), ("header_buffer", 192),
            ("error_buffer", 200), ("error_capacity", 208), ("reserved_word", 212)):
        assert getattr(run_json.ReportSinkJSONRequestC, name).offset == offset
    for name, offset in (
            ("struct_size", 0), ("version", 4), ("status", 8), ("complete", 12),
            ("counts", 16), ("source_ino", 72), ("source_size", 80), ("sha256_hex", 88),
            ("reserved", 153), ("summary", 160), ("diagnostics", 224),
            ("equity_json_bytes", 296), ("trades_json_bytes", 304),
            ("equity_json_sha256_hex", 312), ("trades_json_sha256_hex", 377),
            ("reserved_fragments", 442), ("header_bytes", 448), ("error_bytes", 452),
            ("result_field", 456), ("result_tag", 460), ("error", 464)):
        assert getattr(run_json.ReportSinkJSONResultC, name).offset == offset
    for name, offset in (
            ("struct_size", 0), ("version", 4), ("stage", 8), ("category", 12),
            ("line_number", 16), ("byte_offset", 24), ("field_index", 32), ("tag", 36),
            ("os_errno", 40), ("offending_bytes", 44), ("message", 48), ("offending", 240)):
        assert getattr(run_json.ReportSinkJSONErrorC, name).offset == offset


# One row per field/token/record rule, FROM pinned docker/pf_sink.py on
# 30e9f9a0. Later grammar/token faults must precede earlier field faults.
# Bounded scalar cases must be decided natively; parser-capacity cases below
# also exercise production strict compatibility rather than manufacturing success.
FIELD_REFUSALS = (
    ("required-null", "docker/pf_sink.py:211", b'["E",null,1.0,0.0]\n'),
    ("i64-bool", "docker/pf_sink.py:217", b'["E",true,1.0,0.0]\n'),
    ("i64-float", "docker/pf_sink.py:217", b'["E",0.0,1.0,0.0]\n'),
    ("integer-bound", "docker/pf_sink.py:121", b'["E",9007199254740992,1.0,0.0]\n'),
    ("integer-digits", "docker/pf_sink.py:118", b'["E",10000000000000000,1.0,0.0]\n'),
    ("f64-integer", "docker/pf_sink.py:221", b'["E",0,1,0.0]\n'),
    ("f64-bool", "docker/pf_sink.py:221", b'["E",0,true,0.0]\n'),
    ("f64-repr", "docker/pf_sink.py:221", b'["E",0,1e4,0.0]\n'),
    ("float-overflow", "docker/pf_sink.py:128", b'["E",0,1e999,0.0]\n'),
    ("nan", "docker/pf_sink.py:133", b'["E",0,NaN,0.0]\n'),
    ("infinity", "docker/pf_sink.py:133", b'["E",0,Infinity,0.0]\n'),
    ("minus-infinity", "docker/pf_sink.py:133", b'["E",0,-Infinity,0.0]\n'),
    ("syntax-before-field", "docker/pf_sink.py:162", b'["E",true,1e999,0.0]\n'),
    ("record-kind", "docker/pf_sink.py:249", b'null\n'),
    ("record-empty", "docker/pf_sink.py:249", b'[]\n'),
    ("record-tag-type", "docker/pf_sink.py:249", b'[1,0,1.0,0.0]\n'),
    ("undeclared-tag", "docker/pf_sink.py:252", b'["Z",0,1.0,0.0]\n'),
    ("record-arity", "docker/pf_sink.py:255", b'["E",0,1.0]\n'),
    ("utf8", "docker/pf_sink.py:166", b'["E",0,"\xff",0.0]\n'),
)


def trade_bad(index, value):
    row = ["T", 1, "long", 0, 1, *([1.0] * 8), 0, 1, False, 1, 0, "", "", ""]
    row[index] = value
    return row


OTHER_FIELDS = (
    ("u64-negative", "docker/pf_sink.py:217", trade_bad(16, -1)),
    ("bool-column", "docker/pf_sink.py:225", trade_bad(15, 1)),
    ("string-kind", "docker/pf_sink.py:96", trade_bad(18, 5)),
    ("string-surrogate", "docker/pf_sink.py:96", trade_bad(18, "\ud800")),
    ("side-domain", "docker/pf_sink.py:258", trade_bad(2, "flat")),
)

T_REFUSALS = (
    tuple((f"t-required-null-{index}", "docker/pf_sink.py:214", trade_bad(index, None))
          for index in (1, 2, 3, 4, 13, 14, 15, 16, 17, 18, 19, 20))
    + tuple((f"t-i64-float-{index}", "docker/pf_sink.py:217", trade_bad(index, 1.0))
            for index in (1, 3, 4, 13, 14, 17))
    + tuple((f"t-f64-integer-{index}", "docker/pf_sink.py:221", trade_bad(index, 1))
            for index in range(5, 13))
    + (
        ("t-u64-bool", "docker/pf_sink.py:217", trade_bad(16, True)),
        ("t-u64-float", "docker/pf_sink.py:217", trade_bad(16, 1.0)),
        ("t-arity-short", "docker/pf_sink.py:255", trade_bad(1, 1)[:-1]),
        ("t-arity-long", "docker/pf_sink.py:255", trade_bad(1, 1) + ["extra"]),
        ("t-side-after-fields", "docker/pf_sink.py:257", trade_bad(2, "flat")[:-1] + [5]),
        ("t-token-before-field", "docker/pf_sink.py:162",
         wire(trade_bad(1, True)).replace(b",1.0,", b",1e999,", 1)),
        ("t-repr", "docker/pf_sink.py:221",
         TRADE_ONE.replace(b",100.0,", b",1e2,")),
        ("t-f64-signed-integer-zero", "docker/pf_sink.py:221",
         TRADE_ONE.replace(b",100.0,", b",-0,")),
        ("t-invalid-utf8-audit", "docker/pf_sink.py:166",
         TRADE_ONE.replace(b'"exit"', b'"\xff"')),
    )
)


@pytest.mark.parametrize("name,citation,row", FIELD_REFUSALS + OTHER_FIELDS + T_REFUSALS,
                         ids=[row[0] for row in FIELD_REFUSALS + OTHER_FIELDS + T_REFUSALS])
def test_native_refusal_field_rule(native_library, tmp_path, name, citation, row):
    path = tmp_path / (name + ".sink")
    path.write_bytes(packed([row]))
    _, _, function = native_library
    expected = refusal(lambda: strict(path))
    result, header, error = raw_call(function, path)
    assert not result.complete, (citation, result.status)
    assert result.status == 2, (citation, result.status)
    cold = refusal(lambda: run_json._raise_native_sink_json(result, header, error))
    assert cold == expected, (citation, cold, expected)
    # Exercise the production adapter too, including its receipt boundary,
    # rather than comparing only the cold translator to the oracle.
    actual = refusal(lambda: production_verify(function, path))
    assert actual == expected, (citation, actual, expected)


def wide_header(columns=1):
    header = fixtures.header(outputs=True)
    header["records"][2] = fixtures.descriptor(
        "O", ["row", "open_ms", "close_ms"] + [f"v{i}" for i in range(columns)],
        ["i64", "i64?", "i64?"] + ["f64?"] * columns,
        ["", "unix_ms", "unix_ms"] + [""] * columns)
    header["records"][4] = fixtures.descriptor(
        "K", [f"c{i}" for i in range(columns)], ["f64?"] * columns, [""] * columns)
    header["records"] += [
        fixtures.descriptor("X", ["timestamp", "bar_index", "name", "value"],
                            ["i64", "i64", "str", "f64?"], ["unix_ms", "", "", ""]),
        fixtures.descriptor("H", ["hash"], ["hex64"], [""])]
    return header


def optional_rows(columns=1):
    return [
        ["O", 0, None, -1] + [None] * columns,
        ["V", 1, 0, -1, None, -1, -1, -1, -1, None, "0123456789abcdef", None],
        ["K"] + [-0.0] * columns,
        ["X", -1, -1, 'quote"\n漢𝄞', None], ["H", "fedcba9876543210"]]


@pytest.mark.parametrize("columns", (0, 1, 25))
def test_native_all_optional_tags_and_wide_columns(native_library, tmp_path, columns):
    header = wide_header(columns)
    path = tmp_path / "optional.sink"
    path.write_bytes(packed([["E", 0, 1.0, 0.0], TRADE_ONE, *optional_rows(columns)],
                           header=header))
    with pf_sink.SinkReader(path) as reader:
        for _ in reader:
            pass
        expected_counts, expected_digest = reader.counts, reader.sha256
    result, raw_header, _ = raw_call(native_library[2], path)
    assert result.status == 0 and result.complete == 1, "optional tags must not select fallback"
    assert result.counts[:] == [expected_counts[tag] for tag in "ETOVKXH"]
    assert bytes(result.sha256_hex).decode() == expected_digest
    for tag in "TE":
        guard, fresh_header, _ = raw_call(native_library[2], path,
                                          verified=(result, raw_header), tag=tag)
        assert guard.status == 0 and guard.complete == 1
        assert fresh_header == raw_header


OPTIONAL_REFUSALS = (
    ("o-required", "docker/pf_sink.py:214", ["O", None, None, None, None]),
    ("o-time-bool", "docker/pf_sink.py:217", ["O", 0, True, None, None]),
    ("o-float", "docker/pf_sink.py:221", ["O", 0, None, None, 1]),
    ("v-sequence", "docker/pf_sink.py:217",
     ["V", -1, 0, 0, None, None, 0, 0, 0, None, "0123456789abcdef", None]),
    ("v-phase", "docker/pf_sink.py:217",
     ["V", 1, 0, 0, None, None, 0, True, 0, None, "0123456789abcdef", None]),
    ("v-hash", "docker/pf_sink.py:231",
     ["V", 1, 0, 0, None, None, 0, 0, 0, None, "ABCDEF0123456789", None]),
    ("v-message", "docker/pf_sink.py:96",
     ["V", 1, 0, 0, None, None, 0, 0, 0, None, "0123456789abcdef", "\ud800"]),
    ("k-float", "docker/pf_sink.py:221", ["K", 0]),
    ("x-name", "docker/pf_sink.py:96", ["X", 0, 0, "\udfff", None]),
    ("x-value", "docker/pf_sink.py:221", ["X", 0, 0, "x", False]),
    ("h-hash", "docker/pf_sink.py:231", ["H", "abcd"]),
    ("optional-syntax-before-fields", "docker/pf_sink.py:162", b'["O",true,null,null,1e999]\n'),
)


@pytest.mark.parametrize("name,citation,row", OPTIONAL_REFUSALS,
                         ids=[row[0] for row in OPTIONAL_REFUSALS])
def test_native_optional_tag_refusals(native_library, tmp_path, name, citation, row):
    path = tmp_path / "optional-refusal.sink"
    path.write_bytes(packed([TRADE_ONE, row], header=wide_header()))
    expected = refusal(lambda: strict(path))
    result, header, error = raw_call(native_library[2], path)
    assert result.status == 2 and not result.complete, (citation, result.status)
    assert refusal(lambda: run_json._raise_native_sink_json(result, header, error)) == expected
    assert refusal(lambda: production_verify(native_library[2], path)) == expected


def test_native_capacity_refusal_uses_production_strict_fallback(native_library, tmp_path):
    path = tmp_path / "capacity.sink"
    path.write_bytes(packed([b'["E",0,' + b"[" * 40 + b"0" + b"]" * 40 + b",0.0]\n"]))
    result, _, _ = raw_call(native_library[2], path)
    assert result.status == 1 and not result.complete
    expected = refusal(lambda: strict(path))
    assert refusal(lambda: production_verify(native_library[2], path)) == expected


SCHEMA_REFUSALS = (
    ("header-order", "docker/pf_sink.py:90"),
    ("schema", "docker/pf_sink.py:172"),
    ("version", "docker/pf_sink.py:101"),
    ("run-id-string", "docker/pf_sink.py:175"),
    ("engine-string", "docker/pf_sink.py:176"),
    ("records-array", "docker/pf_sink.py:177"),
    ("descriptor-order", "docker/pf_sink.py:182"),
    ("descriptor-tag-order", "docker/pf_sink.py:184"),
    ("descriptor-array", "docker/pf_sink.py:187"),
    ("fields", "docker/pf_sink.py:199"),
    ("types", "docker/pf_sink.py:199"),
    ("units", "docker/pf_sink.py:199"),
    ("output-fixed", "docker/pf_sink.py:191"),
    ("missing-et", "docker/pf_sink.py:202"),
    ("partial-ovk", "docker/pf_sink.py:204"),
    ("hex64", "docker/pf_sink.py:230"),
    ("trailer-order", "docker/pf_sink.py:342"),
    ("trailer-schema", "docker/pf_sink.py:343"),
    ("trailer-version", "docker/pf_sink.py:345"),
    ("count-keys", "docker/pf_sink.py:346"),
    ("count-type", "docker/pf_sink.py:348"),
    ("count-negative", "docker/pf_sink.py:348"),
    ("count-value", "docker/pf_sink.py:348"),
    ("digest", "docker/pf_sink.py:351"),
    ("trailing", "docker/pf_sink.py:353"),
    ("missing-trailer", "docker/pf_sink.py:337"),
    ("missing-lf", "docker/pf_sink.py:138"),
    ("line-cap", "docker/pf_sink.py:140"),
    ("blank", "docker/pf_sink.py:143"),
    ("bom", "docker/pf_sink.py:145"),
    ("whitespace", "docker/pf_sink.py:159"),
    ("duplicate-key", "docker/pf_sink.py:109"),
    ("deferred-e-vs-source", "docker/run_json.py:6076"),
)


def schema_payload(name):
    header = copy.deepcopy(fixtures.header())
    rows = [["E", 0, 1.0, 0.0]]
    if name == "header-order":
        header = dict(reversed(tuple(header.items())))
    elif name == "schema":
        header["schema"] = "wrong"
    elif name == "version":
        header["version"] = True
    elif name == "run-id-string":
        header["run_id"] = "\udfff"
    elif name == "engine-string":
        header["engine"] = 1
    elif name == "records-array":
        header["records"] = {}
    elif name == "descriptor-order":
        header["records"][0] = dict(reversed(tuple(header["records"][0].items())))
    elif name == "descriptor-tag-order":
        header["records"].reverse()
    elif name == "descriptor-array":
        header["records"][0]["fields"] = "time_ms"
    elif name in ("fields", "types", "units"):
        header["records"][0][name][0] = "wrong"
    elif name == "output-fixed":
        header = fixtures.header(outputs=True)
        header["records"][2]["fields"] = []
    elif name == "missing-et":
        header["records"].pop()
    elif name == "partial-ovk":
        header["records"].append(fixtures.header(outputs=True)["records"][2])
    elif name == "hex64":
        header["records"].append({"type": "H", "fields": ["hash"],
                                  "types": ["hex64"], "units": [""]})
        rows = [["H", "ABCDEF0123456789"]]
    payload = packed(rows, header=header)
    if name.startswith("trailer-") or name in ("count-keys", "count-type", "count-negative",
                                               "count-value", "digest"):
        def change(trailer):
            if name == "trailer-schema":
                trailer["trailer"] = "wrong"
            elif name == "trailer-version":
                trailer["version"] = 1.0
            elif name == "count-keys":
                trailer["counts"] = dict(reversed(tuple(trailer["counts"].items())))
            elif name.startswith("count-"):
                trailer["counts"]["E"] = {"count-type": True, "count-negative": -1,
                                          "count-value": 9}[name]
            elif name == "digest":
                trailer["sha256"] = "0" * 64
        payload = packed(rows, header=header, trailer_change=change)
        if name == "trailer-order":
            lines = payload.splitlines(keepends=True)
            lines[-1] = wire(dict(reversed(tuple(json.loads(lines[-1]).items()))))
            payload = b"".join(lines)
    elif name == "trailing":
        payload += b"x"
    elif name == "missing-trailer":
        payload = b"".join(payload.splitlines(keepends=True)[:-1])
    elif name == "missing-lf":
        payload = payload[:-1]
    elif name == "line-cap":
        payload = packed([b'["E",0,"' + b"x" * 1048576 + b'",0.0]\n'])
    elif name == "blank":
        payload = packed([b"\n"])
    elif name == "bom":
        payload = packed([b'\xef\xbb\xbf["E",0,1.0,0.0]\n'])
    elif name == "whitespace":
        payload = packed([b'["E", 0,1.0,0.0]\n'])
    elif name == "duplicate-key":
        payload = payload.replace(b'"version":1,', b'"version":1,"version":1,', 1)
    elif name == "deferred-e-vs-source":
        payload = packed([["E", 0, None, 0.0], b'["E",1,1e4,0.0]\n'])
    return payload


@pytest.mark.parametrize("name,citation", SCHEMA_REFUSALS,
                         ids=[row[0] for row in SCHEMA_REFUSALS])
def test_native_refusal_schema_framing_priority(native_library, tmp_path, name, citation):
    path = tmp_path / (name + ".sink")
    path.write_bytes(schema_payload(name))
    expected = refusal(lambda: strict(path))
    result, header, error = raw_call(native_library[2], path)
    assert not result.complete
    if result.status != 1:
        cold = refusal(lambda: run_json._raise_native_sink_json(result, header, error))
        assert cold == expected, (citation, cold, expected)
    actual = refusal(lambda: production_verify(native_library[2], path))
    assert actual == expected, (citation, actual, expected)


@pytest.mark.parametrize("tag", ("E", "T"))
@pytest.mark.parametrize("case", ["identity-before-malformed", "inode-before-malformed",
                                  "selected-before-digest", "digest-before-count", "count",
                                  "trailer-lf", "trailer-eof", "raw-trailer",
                                  "skipped-before-digest", "framing-before-digest"])
def test_native_guard_priority(native_library, tmp_path, case, tag):
    path = tmp_path / "guard.sink"
    path.write_bytes(packed([["E", 0, 1.0, 0.0], trade_bad(1, 1)]))
    before = path.read_bytes()
    result, header, _ = raw_call(native_library[2], path)
    assert result.status == 0 and result.complete
    sink = run_json._ReportSink.__new__(run_json._ReportSink)
    sink.path, sink.header = str(path), pf_sink._parse(header)
    sink.counts = {"E": 1, "T": 1}
    sink.sha256 = bytes(result.sha256_hex).decode()
    sink._identity = (result.source_ino, result.source_size)
    expected_count = None
    needle = b",1.0,0.0]" if tag == "E" else b",1.0,1.0,1.0,"
    bad_value = b",1e4,0.0]" if tag == "E" else b",1e4,1.0,1.0,"
    good_value = b",2.0,0.0]" if tag == "E" else b",2.0,1.0,1.0,"
    if case == "identity-before-malformed":
        path.write_bytes(b"bad\n")
    elif case == "inode-before-malformed":
        replacement = tmp_path / "replacement.sink"
        replacement.write_bytes(b"x" * (len(before) - 1) + b"\n")
        os.replace(replacement, path)
    elif case == "selected-before-digest":
        path.write_bytes(before.replace(needle, bad_value))
    elif case == "digest-before-count":
        path.write_bytes(before.replace(needle, good_value))
        sink.counts[tag] = expected_count = 2
    elif case == "count":
        sink.counts[tag] = expected_count = 2
    elif case == "trailer-lf":
        path.write_bytes(before[:-1] + b"x")
    elif case == "trailer-eof":
        path.write_bytes(before[:-2] + b"\nx")
    elif case == "raw-trailer":
        # Same-size raw trailer edit: #529 guards check LF/EOF, not its JSON.
        path.write_bytes(before.replace(b'"trailer":"pf-report-sink"',
                                        b'"trailer":"pf-report-sinX"'))
    elif case == "skipped-before-digest":
        path.write_bytes(before.replace(b'"long"', b'"flat"') if tag == "E"
                         else before.replace(b",1.0,0.0]", b",1e4,0.0]"))
    elif case == "framing-before-digest":
        path.write_bytes(before.replace(needle, good_value)[:-1] + b"x")
    native, fresh_header, error = raw_call(native_library[2], path, verified=(result, header),
                                         expected_count=expected_count, tag=tag)
    if case == "raw-trailer":
        assert list(sink.records(tag))
        assert native.status == 0 and native.complete
    else:
        expected = refusal(lambda: list(sink.records(tag)))
        actual = refusal(lambda: guard_refusal(native, header, fresh_header, error, tag))
        assert actual == expected


@pytest.mark.parametrize("tag", ("E", "T"))
def test_native_guard_retains_e_verified_vs_t_strict_lexical_rules(
        native_library, tmp_path, tag):
    header = fixtures.header()
    header["run_id"] = "padding"
    path = tmp_path / "guard-lexical.sink"
    path.write_bytes(packed([["E", 0, 1.0, 0.0], trade_bad(1, 1)], header=header))
    before = path.read_bytes()
    verified, raw_header, _ = raw_call(native_library[2], path)
    sink = configured_sink(None, path)
    sink.header, sink.counts = header, {"E": 1, "T": 1}
    sink.sha256 = bytes(verified.sha256_hex).decode()
    sink._identity = verified.source_ino, verified.source_size
    needle = b'["E",0,' if tag == "E" else b'["T",1,'
    replacement = b'["E", 0,' if tag == "E" else b'["T", 1,'
    changed = before.replace(needle, replacement, 1).replace(b'"padding"', b'"paddin"', 1)
    assert changed != before and len(changed) == len(before)
    path.write_bytes(changed)
    expected = refusal(lambda: list(sink.records(tag)))
    native, fresh_header, error = raw_call(native_library[2], path,
                                          verified=(verified, raw_header), tag=tag)
    assert refusal(lambda: guard_refusal(native, raw_header, fresh_header, error, tag)) == expected
    if tag == "E":
        assert expected[3].endswith("digest differs")
    else:
        assert expected[3].endswith("whitespace or control byte outside a JSON string")


def test_native_guard_trade_line_cap_precedes_missing_lf(native_library, tmp_path):
    row = trade_bad(20, "")
    row[20] = "x" * (pf_sink.MAX_LINE_BYTES - len(wire(row)))
    line = wire(row)
    assert len(line) == pf_sink.MAX_LINE_BYTES
    path = tmp_path / "guard-cap.sink"
    path.write_bytes(packed([line]))
    before = path.read_bytes()
    verified, raw_header, _ = raw_call(native_library[2], path)
    assert verified.status == 0 and verified.complete == 1
    sink = configured_sink(None, path)
    sink.header, sink.counts = pf_sink._parse(raw_header), {"E": 0, "T": 1}
    sink.sha256 = bytes(verified.sha256_hex).decode()
    sink._identity = verified.source_ino, verified.source_size
    # Valid twin goes through the complete native T guard first.
    valid, _, _ = raw_call(native_library[2], path, verified=(verified, raw_header), tag="T")
    assert valid.status == 0 and valid.complete == 1
    path.write_bytes(before.replace(line, line[:-1] + b"x", 1))
    expected = refusal(lambda: list(sink.records("T")))
    native, fresh_header, error = raw_call(native_library[2], path,
                                          verified=(verified, raw_header), tag="T")
    assert refusal(lambda: guard_refusal(native, raw_header, fresh_header, error, "T")) == expected
    assert expected[3].endswith("line exceeds 1 MiB including LF")


@pytest.mark.parametrize("outputs", (False, True))
def test_native_full_host_keeps_optional_output_passes(native_library, tmp_path, monkeypatch, outputs):
    documents, calls = [], []
    def native(request, result):
        status = native_library[2](request, result)
        calls.append((request._obj.operation, result._obj.status, request._obj.guard_tag))
        return status
    for enabled in (False, True):
        library = fixtures.SinkLibrary(outputs=outputs)
        library.pf_report_sink_json_v1 = native
        monkeypatch.setenv("PF_REPORT_SINK_JSON_SPIKE", "1" if enabled else "0")
        status, text, stderr, _ = fixtures.invoke(
            tmp_path, monkeypatch, library=library, forbid_output_arrays=outputs,
            extra=["--spill-dir", str(tmp_path)] + (["--outputs"] if outputs else []))
        assert status == 0 and stderr == "", (text, stderr)
        assert library.queried == 1 and library.freed == library.installed
        assert not Path(library.installed[0]).exists()
        documents.append(text)
    assert documents[0] == documents[1]
    assert calls == [(1, 0, 0), (2, 0, ord("T")), (2, 0, ord("E"))]
    assert ("outputs" in json.loads(documents[1])) == outputs


@pytest.mark.parametrize("case", ["header-before-selected", "valid-json-header",
                                  "deep-valid-header-before-selected"])
def test_native_guard_uses_the_captured_header_before_later_errors(
        native_library, tmp_path, case):
    base_header = fixtures.header()
    base_header["run_id"] = "x" * 256
    path = tmp_path / "guard-header.sink"
    path.write_bytes(packed([["E", 0, 1.0, 0.0]], header=base_header))
    before = path.read_bytes()
    verified, original_header, _ = raw_call(native_library[2], path)
    sink = configured_sink(None, path)
    sink.header, sink.counts = base_header, {"E": 1, "T": 0}
    sink.sha256 = bytes(verified.sha256_hex).decode()
    sink._identity = verified.source_ino, verified.source_size
    changed = before
    if case == "header-before-selected":
        changed = changed.replace(b'"version":1,', b'"version":?,', 1)
    elif case == "valid-json-header":
        # Header schema changes are not re-admitted by a raw guard.
        changed = changed.replace(b'"schema":', b'"xxxxxx":', 1)
    else:
        nested = b"[" * 40 + b"0" + b"]" * 40
        fresh = wire({"irrelevant": []})
        fresh = fresh.replace(b"[]", nested)
        # Keep raw size exactly fixed while using valid JSON outside the
        # native prepare subset. Whitespace INSIDE the padding string is legal.
        padding = len(original_header) - len(fresh) - len(b',"padding":""')
        assert padding >= 0
        fresh = fresh[:-2] + b',"padding":"' + b"x" * padding + b'"}\n'
        assert len(fresh) == len(original_header)
        changed = fresh + before[len(original_header):]
    if case != "valid-json-header":
        changed = changed.replace(b",1.0,0.0]", b",1e4,0.0]")
    assert len(changed) == len(before)
    path.write_bytes(changed)
    expected = refusal(lambda: list(sink.records("E")))
    native, fresh_header, error = raw_call(native_library[2], path,
                                          verified=(verified, original_header))
    actual = refusal(lambda: guard_refusal(native, original_header, fresh_header, error))
    assert actual == expected


@pytest.mark.parametrize("name,citation,row", OTHER_FIELDS + (
    ("valid-t-digest", "docker/run_json.py:6257", trade_bad(1, 1)),
    ("null-t-digest", "docker/run_json.py:6257", trade_bad(8, None)),
), ids=[row[0] for row in OTHER_FIELDS] + ["valid-t-digest", "null-t-digest"])
def test_native_empty_t_guard_preserves_t_parser_priority(
        native_library, tmp_path, name, citation, row):
    base_header = fixtures.header()
    base_header["run_id"] = "x" * 256
    path = tmp_path / "guard-trade.sink"
    path.write_bytes(packed([["E", 0, 1.0, 0.0]], header=base_header))
    before = path.read_bytes()
    verified, original_header, _ = raw_call(native_library[2], path)
    lines = before.splitlines(keepends=True)
    trade = wire(row)
    changed_header = copy.deepcopy(base_header)
    changed_header["run_id"] = "x" * (256 - (len(trade) - len(lines[1])))
    changed = wire(changed_header) + trade + lines[-1]
    assert len(changed) == len(before)
    path.write_bytes(changed)
    sink = configured_sink(None, path)
    sink.header, sink.counts = base_header, {"E": 1, "T": 0}
    sink.sha256 = bytes(verified.sha256_hex).decode()
    sink._identity = verified.source_ino, verified.source_size
    expected = refusal(lambda: list(sink.records("T")))
    native, fresh_header, error = raw_call(native_library[2], path,
                                          verified=(verified, original_header), tag="T")
    actual = refusal(lambda: guard_refusal(native, original_header, fresh_header, error, tag="T"))
    assert actual == expected, (citation, actual, expected)


@pytest.mark.parametrize("count,oscillating", [(2, False), (3, False), (257, True)])
def test_native_bulk_full_report_bytes_and_zero_python_equity_parse(
        native_library, tmp_path, monkeypatch, count, oscillating):
    module, library, real_native = native_library
    path = tmp_path / "bars.csv"
    prices = [100.0 + (8.0 * math.sin(i / 3.0) if oscillating else 0.0)
              for i in range(count)]
    path.write_text("timestamp,open,high,low,close,volume\n" + "".join(
        f"{1735776000000 + i * 60000},{p},{p + 1},{p - 1},{p},10\n"
        for i, p in enumerate(prices)), encoding="ascii")
    monkeypatch.setattr(run_json.time, "time", lambda: 100.0)
    monkeypatch.setattr(run_json.time, "perf_counter", lambda: 100.0)
    monkeypatch.setattr(run_json.time, "perf_counter_ns", lambda: 100000000000)
    after_engine = False
    native_mode = False
    in_record = False
    calls, record_dumps = [], []
    real_run = library.run_backtest_full

    def run(*args):
        nonlocal after_engine
        real_run(*args)
        after_engine = True

    def native(request, result):
        status = real_native(request, result)
        calls.append((request._obj.operation, result._obj.status,
                      result._obj.complete, request._obj.guard_tag))
        return status

    class Library:
        def __getattr__(self, name):
            return {"run_backtest_full": run, "pf_report_sink_json_v1": native}.get(
                name, getattr(library, name))

    monkeypatch.setattr(run_json, "load_strategy", lambda _: Library())
    original_parse, original_field = pf_sink._parse, pf_sink._field
    original_dumps = json.dumps

    def parse(line):
        if after_engine and native_mode and line.startswith((b'["E",', b'["T",')):
            pytest.fail("Python _parse visited E/T after the engine returned")
        return original_parse(line)

    def field(*args):
        if after_engine and native_mode and in_record:
            pytest.fail("Python _field visited E/T after the engine returned")
        return original_field(*args)

    def wrap_decoder(original):
        def decode(self, line, **kwargs):
            nonlocal in_record
            was = in_record
            in_record = line.startswith((b'["E",', b'["T",'))
            if after_engine and native_mode and in_record:
                pytest.fail("Python RecordParser visited E/T after the engine returned")
            try:
                return original(self, line, **kwargs)
            finally:
                in_record = was
        return decode

    def dumps(value, *args, **kwargs):
        if (after_engine and native_mode and isinstance(value, dict)
                and tuple(value) in (("time_ms", "equity", "open_profit"), fixtures.TRADE_FIELDS[:16])):
            record_dumps.append(value)
        return original_dumps(value, *args, **kwargs)

    monkeypatch.setattr(pf_sink, "_parse", parse)
    monkeypatch.setattr(pf_sink, "_field", field)
    monkeypatch.setattr(pf_sink.RecordParser, "parse", wrap_decoder(pf_sink.RecordParser.parse))
    monkeypatch.setattr(pf_sink.RecordParser, "parse_verified",
                        wrap_decoder(pf_sink.RecordParser.parse_verified))
    monkeypatch.setattr(json, "dumps", dumps)
    documents = []
    for enabled in (False, True):
        native_mode, after_engine = enabled, False
        monkeypatch.setenv("PF_REPORT_SINK_JSON_SPIKE", "1" if enabled else "0")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = run_json.main(["--so", module, "--ohlcv", str(path),
                                    "--input-tf", "1", "--script-tf", "1",
                                    "--spill-dir", str(tmp_path), "--run-id", "cr397",
                                    "--generated-cpp", str(ROOT / "tutorial/macd/generated.cpp")])
        assert status == 0 and stderr.getvalue() == "", (stdout.getvalue(), stderr.getvalue())
        documents.append(stdout.getvalue().encode())
    assert documents[0] == documents[1]  # full bytes, no timing/provenance masks
    document = json.loads(documents[1])
    assert len(document["equity_curve"]) == count
    assert bool(document["trades"]) == oscillating, "real generated T-bearing control required"
    assert document["diagnostics"]["run_profile"] == "unjailed"
    assert document["diagnostics"]["scratch_arena_jailed"] is False
    assert calls == [(1, 0, 1, 0), (2, 0, 1, ord("T")), (2, 0, 1, ord("E"))]
    assert not record_dumps
