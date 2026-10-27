"""Integer, record-count and window-boundary controls for both sink readers.

The oracle always consumes pf_sink.SinkReader to exhaustion. The native route
requires the real library, successful native prepare and both native guards;
malformed sources must also be refused by the raw codec without fallback.
"""
from __future__ import annotations

import copy
from dataclasses import dataclass, replace
import hashlib
import io
import json

import pytest

import test_run_json_native_sink_json as support
from test_run_json_native_sink_json import native_library


pf_sink = support.pf_sink
run_json = support.run_json
WINDOW = 64 * 1024
TAGS = ("E", "T")


@dataclass(frozen=True)
class Refusal:
    exception_type: type
    code: str
    code_args: dict
    message: str


@dataclass(frozen=True)
class View:
    counts: dict
    first: dict
    last: dict
    fragments: dict
    digest: str
    deferred: Refusal | None


@pytest.fixture(params=("strict", "native"), ids=("strict-python", "native"))
def route(request):
    return request.param


def _failure(error):
    return Refusal(type(error), error.code, copy.deepcopy(error.code_args), str(error))


def _refusal(call):
    try:
        call()
    except run_json.RunFailure as error:
        return _failure(error)
    raise AssertionError("a malformed source reached successful verification")


def _project(record):
    # Fixed-v1 report projection; the strict reader supplies fields and values.
    width = 3 if record.tag == "E" else 16
    return dict(zip(record.fields[:width], record.values[:width]))


def _compact(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii")


def _report(counts):
    report = run_json.ReportC()
    report.total_trades = counts["T"]
    return report


def _strict_outcome(path):
    """Independent source/count/fragment oracle, never a native result."""
    points = {tag: [] for tag in TAGS}
    trades = []
    try:
        with pf_sink.SinkReader(path) as reader:
            for record in reader:
                points[record.tag].append(_project(record))
                if record.tag == "T":
                    trades.append(record)
            assert reader.complete
            counts, digest = reader.counts, reader.sha256
    except pf_sink.SinkFormatError as error:
        return _failure(run_json._engine_invariant(
            "the engine's report sink is invalid: " + str(error)))
    # Count independently of the trailer and of SinkReader's own accumulator.
    assert counts == {tag: len(points[tag]) for tag in TAGS}
    deferred = None
    try:
        # Wire-valid T.n need not be report numbering from 1. Preserve that
        # separate host refusal instead of narrowing the reader's language.
        run_json._sink_summary(_report(counts), None, records=iter(trades))
    except run_json.RunFailure as error:
        deferred = _failure(error)
    return View(
        counts=counts,
        first={tag: points[tag][0] if points[tag] else None for tag in TAGS},
        last={tag: points[tag][-1] if points[tag] else None for tag in TAGS},
        fragments={tag: b",".join(_compact(point) for point in points[tag])
                   for tag in TAGS},
        digest=digest,
        deferred=deferred,
    )


def _assert_same(actual, expected):
    assert type(actual) is type(expected), (actual, expected)
    if isinstance(expected, Refusal):
        assert actual.exception_type is expected.exception_type
        assert actual.code == expected.code
        assert actual.code_args == expected.code_args
        assert actual.message == expected.message
    else:
        assert actual.counts == expected.counts
        assert actual.first == expected.first
        assert actual.last == expected.last
        assert actual.fragments == expected.fragments
        assert actual.digest == expected.digest
        assert actual.deferred == expected.deferred


def _mutation_self_check(actual, expected, tag):
    """The same checker must catch a wrong count/byte answer or refusal."""
    if isinstance(expected, Refusal):
        wrong = replace(expected, message=expected.message + " (wrong expectation)")
        with pytest.raises(AssertionError):
            _assert_same(actual, wrong)
    else:
        counts = dict(expected.counts)
        counts[tag] += 1
        with pytest.raises(AssertionError):
            _assert_same(actual, replace(expected, counts=counts))
        fragments = dict(expected.fragments)
        fragments[tag] += b" "
        with pytest.raises(AssertionError):
            _assert_same(actual, replace(expected, fragments=fragments))


def _observed_function(function, calls):
    def observed(request_pointer, result_pointer):
        status = function(request_pointer, result_pointer)
        request, result = request_pointer._obj, result_pointer._obj
        calls.append((int(request.operation), int(request.guard_tag),
                      int(result.status), int(result.complete)))
        return status
    return observed


def _host_view(path, function, expected):
    """Use the real adapter and its actual T/E serialization checkpoints."""
    calls = []
    selected = _observed_function(function, calls) if function is not None else None
    sink = support.configured_sink(selected, path)
    sink.named, sink.keep = False, True
    receipt = support.ReceiptFromStream(path)  # strict-reader receipt, not codec
    try:
        sink.verify(receipt, None, _report(expected.counts))
        assert receipt.queries == 1
        if sink.summary_failure is None:
            assert sink.summary["total_trades"] == sink.counts["T"]
        if function is not None:
            assert sink.native_json is not None and sink.native_json.accepted
            assert calls == [(1, 0, 0, 1)], "native prepare fell back or was bypassed"
            result = sink.native_json.result
            assert result.diagnostics.complete == result.summary.complete == 1
            assert result.diagnostics.count_e == sink.counts["E"]
            assert result.summary.count_t == sink.counts["T"]
        else:
            assert sink.native_json is None
        fragments, points = {}, {}
        # Match report serialization order, including guards of empty arrays.
        for tag in ("T", "E"):
            if function is not None:
                column = run_json._NativeSinkJSONColumn(sink.native_json, sink, tag)
            else:
                def records(tag=tag):
                    for record in sink.records(tag):
                        yield record.as_dict() if tag == "E" else run_json._sink_trade(record)
                column = run_json._JSONRecordArray(records, sink.counts[tag])
            output = io.StringIO()
            run_json._stream_json(column, output)
            encoded = output.getvalue().encode("ascii")
            assert encoded.startswith(b"[") and encoded.endswith(b"]")
            fragments[tag] = encoded[1:-1]
            points[tag] = json.loads(encoded)
            assert len(points[tag]) == sink.counts[tag]
        if function is not None:
            assert calls == [(1, 0, 0, 1), (2, ord("T"), 0, 1), (2, ord("E"), 0, 1)]
        diagnostics = sink.diagnostics
        assert diagnostics.count_e == len(points["E"])
        first = {tag: points[tag][0] if points[tag] else None for tag in TAGS}
        last = {tag: points[tag][-1] if points[tag] else None for tag in TAGS}
        assert diagnostics.first == first["E"]
        assert diagnostics.last == last["E"]
        deferred = _failure(sink.summary_failure) if sink.summary_failure is not None else None
        return View(dict(sink.counts), first, last, fragments, sink.sha256, deferred)
    finally:
        sink.close()


def _route_outcome(route, function, path, expected):
    selected = function if route == "native" else None
    if isinstance(expected, View):
        return _host_view(path, selected, expected)
    if selected is not None:
        # A production fallback cannot conceal over-acceptance by prepare.
        result, header, error = support.raw_call(selected, path)
        assert result.status == 2 and result.complete == 0, (
            "malformed bounded source was not refused natively", result.status)
        captured = _refusal(lambda: run_json._raise_native_sink_json(result, header, error))
        _assert_same(captured, expected)
    calls = []
    selected = _observed_function(selected, calls) if selected is not None else None
    # This helper forbids reaching engine-receipt admission on a bad source.
    actual = _refusal(lambda: support.production_verify(selected, path))
    if route == "native":
        assert calls == [(1, 0, 2, 0)], "native source refusal used a fallback"
    return actual


def _exercise(route, function, path, tag):
    expected = _strict_outcome(path)
    actual = _route_outcome(route, function, path, expected)
    _assert_same(actual, expected)
    _mutation_self_check(actual, expected, tag)
    return expected


def _trade(n=1):
    return ["T", n, "long", 1000000000000000, 1000000000000001,
            100.0, 101.0, 1.0, 1.0, 1.0, 2.0, -1.0, 0.0,
            0, 1, False, 1, 0, "entry", "exit", ""]


INTEGER_COLUMNS = tuple(
    pytest.param(descriptor["type"], index + 1, field, kind,
                 id=descriptor["type"] + "-" + field)
    for descriptor in support.fixtures.header()["records"]
    for index, (field, kind) in enumerate(zip(descriptor["fields"], descriptor["types"]))
    if kind in ("i64", "u64")
)
INTEGER_TOKENS = (
    b"9007199254740990", b"9007199254740991",
    b"9007199254740992", b"9007199254740993",
    b"-9007199254740990", b"-9007199254740991",
    b"-9007199254740992", b"-9007199254740993",
    b"9007199254740993.0", b"-9007199254740993.0",
    b"-0", b"-0.0", b"1e3", b"-1e3",
)


def _integer_row(tag, index, token, *, competing_field=False):
    row = ["E", 0, 10000.0, -0.0] if tag == "E" else _trade()
    fields = [_compact(value) for value in row]
    fields[index] = token  # retain the wire spelling; json.dumps would lose -0
    if competing_field:
        if tag == "E":
            fields[2] = b"1"  # an integer token in an f64 column
        elif index == 1:
            fields[2] = b"0"  # a non-string side after T.n
        else:
            fields[1] = b"null"  # an earlier invalid T.n column
    return b"[" + b",".join(fields) + b"]\n"


def _integer_rule(token, kind):
    # These are the pinned reader's token and column checks, before any codec
    # call: pf_sink.py:115-130,210-219. In particular, float != exact int type.
    try:
        value = pf_sink._parse(b"[" + token + b"]\n")[0]
        return pf_sink._field(value, kind), None
    except pf_sink.SinkFormatError as error:
        return None, str(error)


@pytest.mark.parametrize("tag,index,field,kind", INTEGER_COLUMNS)
@pytest.mark.parametrize("token", INTEGER_TOKENS, ids=lambda token: token.decode("ascii"))
def test_i53_range_control(route, native_library, tmp_path, tag, index, field, kind, token):
    row = _integer_row(tag, index, token)
    path = tmp_path / "integer.sink"
    path.write_bytes(support.packed([row]))
    value, reason = _integer_rule(token, kind)
    expected = _exercise(route, native_library[2], path, tag)
    if reason is None:
        assert isinstance(expected, View)
        assert expected.counts[tag] == 1
        # close_cause is validated but deliberately absent from T projection.
        if field in expected.first[tag]:
            assert type(expected.first[tag][field]) is int
            assert expected.first[tag][field] == value
    else:
        assert isinstance(expected, Refusal)
        assert expected.message == "the engine's report sink is invalid: line 2: " + reason
        # Source failure wins even with a different request run ID, two bad
        # counts and a bad digest. The receipt is forbidden by production_verify.
        header = support.fixtures.header()
        header["run_id"] = "different-from-the-request"
        def competing(trailer):
            trailer["counts"]["E"] += 1
            trailer["counts"]["T"] += 1
            trailer["sha256"] = "0" * 64
        path.write_bytes(support.packed([row], header=header, trailer_change=competing))
        priority = _exercise(route, native_library[2], path, tag)
        _assert_same(priority, expected)
        # An out-of-range integer token precedes even an earlier field error.
        # An in-range float token instead reaches ordered column validation.
        # Let the strict reader choose the winning refusal in each case.
        row = _integer_row(tag, index, token, competing_field=True)
        path.write_bytes(support.packed([row]))
        priority = _exercise(route, native_library[2], path, tag)
        assert isinstance(priority, Refusal)


COUNT_SIZES = (0, 1, 2, 3, 63, 64, 65, 1023, 1024, 1025,
               "window-1", "window", "window+1")


def _count_row(tag, index):
    if tag == "E":
        return ["E", 1000000000000000 + index, 10000.0, -0.0]
    row = _trade(index + 1)
    row[3], row[4] = 1000000000000000 + 2 * index, 1000000000000001 + 2 * index
    # Keep T wire rows equally wide while retaining genuine consecutive n.
    row[-1] = "x" * (16 - len(str(index + 1)))
    return row


def _count_fixture(tag, size):
    header = support.fixtures.header()
    row_bytes = len(support.wire(_count_row(tag, 0)))
    available = WINDOW - len(support.wire(header))
    full_window, padding = divmod(available, row_bytes)
    assert full_window > 1
    # Only harmless header metadata supplies the remainder. The final target
    # row's LF is exactly byte 65535 when N == full_window.
    header["engine"] += "x" * padding
    assert len(support.wire(header)) + full_window * row_bytes == WINDOW
    count = ({"window-1": full_window - 1, "window": full_window,
              "window+1": full_window + 1}[size] if isinstance(size, str) else size)
    rows = [_count_row(tag, index) for index in range(count)]
    assert all(len(support.wire(row)) == row_bytes for row in rows)
    if size == "window":
        assert len(support.wire(header)) + sum(len(support.wire(row)) for row in rows) == WINDOW
    other = "T" if tag == "E" else "E"
    # Different per-tag populations expose shared E/T counters. These rows
    # follow the target block so they do not move its window boundary.
    rows += [_count_row(other, index) for index in range(3)]
    return header, rows, count


@pytest.mark.parametrize("tag", TAGS)
@pytest.mark.parametrize("size", COUNT_SIZES, ids=str)
def test_e_and_t_count_increment_control(route, native_library, tmp_path, tag, size):
    header, rows, count = _count_fixture(tag, size)
    path = tmp_path / "counts.sink"
    path.write_bytes(support.packed(rows, header=header))
    expected = _exercise(route, native_library[2], path, tag)
    assert isinstance(expected, View)
    assert expected.counts[tag] == count
    other = "T" if tag == "E" else "E"
    assert expected.counts[other] == 3
    assert (expected.first[tag] is None) == (count == 0)
    assert (expected.last[tag] is None) == (count == 0)
    for delta in (1, -1):
        for wrong_digest in (False, True):
            def mismatch(trailer):
                trailer["counts"][tag] += delta
                if wrong_digest:
                    trailer["sha256"] = "0" * 64
            path.write_bytes(support.packed(rows, header=header, trailer_change=mismatch))
            refused = _exercise(route, native_library[2], path, tag)
            assert isinstance(refused, Refusal)
            assert refused.message == (
                f"the engine's report sink is invalid: line {len(rows) + 2}: "
                f"trailer count mismatch for {tag}")


def _boundary_fixture(tag, window_end):
    header = support.fixtures.header()
    filler = _trade()
    target = (["E", 1000000000000000, 10000.0, -0.0]
              if tag == "E" else _trade(2))
    target_line = support.wire(target)
    integer_at = target_line.index(b"1000000000000000")
    padding = (window_end - 7 - integer_at
               - len(support.wire(header)) - len(support.wire(filler)))
    assert padding > 0
    filler[-1] = "x" * padding
    target_start = len(support.wire(header)) + len(support.wire(filler))
    assert target_start + integer_at == window_end - 7
    assert target_start < window_end - 1 < window_end + 1 < target_start + len(target_line)
    assert len(support.wire(filler)) < pf_sink.MAX_LINE_BYTES
    payload = support.packed([filler, target], header=header)
    assert payload[window_end - 1:window_end + 2] == b"000"
    return payload


def _valid_digit_change(payload, offset):
    # A different, still-valid integer at the SAME boundary must also render
    # correctly. Rehash the input, not an expected native output.
    trailer_at = payload.rfind(b"\n{") + 1
    assert 0 < offset < trailer_at
    assert payload[offset:offset + 1] == b"0"
    prefix = payload[:offset] + b"1" + payload[offset + 1:trailer_at]
    trailer = json.loads(payload[trailer_at:])
    trailer["sha256"] = hashlib.sha256(prefix).hexdigest()
    return prefix + support.wire(trailer)


@pytest.mark.parametrize("tag", TAGS)
@pytest.mark.parametrize("window_end", (WINDOW, 2 * WINDOW),
                         ids=("first-window", "second-window"))
@pytest.mark.parametrize("relative", (-1, 0, 1), ids=("last-byte", "end", "next-byte"))
@pytest.mark.parametrize("fault", ("valid", "valid-digit", "truncate", "nul", "utf8"))
def test_fault_at_64k_boundary_control(
        route, native_library, tmp_path, tag, window_end, relative, fault):
    offset = window_end + relative
    payload = _boundary_fixture(tag, window_end)
    if fault == "valid-digit":
        payload = _valid_digit_change(payload, offset)
    elif fault == "truncate":
        payload = payload[:offset]
        assert len(payload) == offset
    elif fault in ("nul", "utf8"):
        replacement = b"\x00" if fault == "nul" else b"\xff"
        payload = payload[:offset] + replacement + payload[offset + 1:]
        assert payload[offset:offset + 1] == replacement
    path = tmp_path / "boundary.sink"
    path.write_bytes(payload)
    expected = _exercise(route, native_library[2], path, tag)
    if fault in ("valid", "valid-digit"):
        assert isinstance(expected, View)
        assert expected.deferred is None
    else:
        assert isinstance(expected, Refusal)
        assert "line 3:" in expected.message
        if fault == "truncate":
            assert expected.message.endswith("line does not end with LF")
        elif fault == "nul":
            assert expected.message.endswith("whitespace or control byte outside a JSON string")
        else:
            assert "invalid UTF-8 or JSON:" in expected.message
