"""Equity scan regressions for verified report sinks.

Use real sink fixtures, the strict reader, RecordParser.parse, and the frozen
writer as independent references. Cover selective decoding, sequential raw
reads, digest and count checks, error ordering, and publication cleanup.

Run on the lab's disk-backed workspace.
"""
import errno
import hashlib
import io
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_sink as ordinary
import test_run_json_stream_fastpath as oracle
from run_json_trade_scan_test import (
    DIGEST_DIFFERS, IDENTITY_DIFFERS, INVALID, WINDOW,
    _assert_engine_error, _assert_format_error, _event, _identity, _mixed,
    _padding_before, _point, _replace_once, _sink, _strict, _trade, _trailer_start,
)


COUNT_DIFFERS = "the report sink E scan disagrees with its verified count"
I53 = (1 << 53) - 1


# Accepted float tokens come from repr(value), never a hand-spelled variant.
# Each alternate denotes the same double but violates the strict repr rule.
_EQUITY_FLOAT_SPELLINGS = (
    (0.0, ("0.00", "0e+00")),
    (-0.0, ("-0.00", "-0e+00")),
    (1.0, ("1.00", "1e+00")),
    (-1.0, ("-1.00", "-1e+00")),
    (1.5, ("1.500",)),
    (0.1, ("0.10000000000000001",)),
    (0.1 + 0.2, ("0.300000000000000040",)),
    (1.2345678901234567, ("1.23456789012345670",)),
    (0.0001, ("0.00010", "1e-04")),
    (1e-05, ("0.00001", "1e-5")),
    (1e-06, ("1e-6",)),
    (1e-07, ("1e-7", "1.0e-07")),
    (1000000000000000.0, ("1e+15",)),
    (1e+16, ("1.0e+16", "1e16", "1E+16", "1e+016",
             "10000000000000000.0")),
    (1e+22, ("1.0e+22",)),
    (1.25e+16, ("1.250e+16",)),
    (1.25e-07, ("1.250e-07",)),
    (1e+100, ("1e100", "1e+0100")),
    (1e-100, ("1e-0100",)),
    (1.25e+100, ("1.250e+100",)),
    (1.25e-100, ("1.250e-100",)),
    (5e-324, ("4.9406564584124654e-324",)),
    (2.2250738585072014e-308, ("2.22507385850720140e-308",)),
    (2.225073858507201e-308, ("2.2250738585072010e-308",)),
    (1.7976931348623157e+308, ("1.79769313486231570e+308",)),
)
_NON_REPR_FLOAT_TOKENS = tuple(
    text.encode("ascii") for _, alternates in _EQUITY_FLOAT_SPELLINGS
    for text in alternates
)


def _verified_decoder(parser):
    method = getattr(parser, "parse_verified", None)
    assert callable(method), "the equity scan requires RecordParser.parse_verified(line)"
    return method


def _assert_record(actual, expected):
    assert type(actual) is type(expected) is pf_sink.Record
    assert tuple(actual) == tuple(expected)
    for left, right in zip(actual, expected):
        assert type(left) is type(right)
    assert type(actual.fields) is type(actual.values) is tuple
    assert len(actual.fields) == len(expected.fields)
    assert len(actual.values) == len(expected.values)
    for left, right in zip(actual.fields + actual.values,
                           expected.fields + expected.values):
        assert type(left) is type(right)
        if type(right) is float:
            assert left.hex() == right.hex()
        else:
            assert left == right


def _array_bytes(records, writer=oracle._stream_json):
    destination = oracle._Destination()
    writer(run_json._JSONRecordArray(lambda: (row.as_dict() for row in records)),
           destination)
    return destination.bytes()


def _equity_flip(raw):
    start = raw.index(b'\n["E",') + 1
    position = raw.index(b",10000.0,", start) + 1
    assert position < raw.index(b"\n", start)
    return raw[:position] + b"9" + raw[position + 1:], position


def _write_same_identity(path, changed):
    path = Path(path)
    identity = _identity(path)
    assert len(changed) == identity[1]
    path.write_bytes(changed)
    assert _identity(path) == identity


def _assert_closed(opened):
    assert len(opened) == 1
    file, fd = opened[0]
    assert file.closed and file.buffer is None
    with pytest.raises(OSError):
        os.fstat(fd)


def _invoke_after_equity_tamper(tmp_path, monkeypatch, mutate, *, library=None):
    """Use real output staging; alter the file when equity records are requested."""
    original = run_json._ReportSink._tagged_records
    original_init = run_json._BoundedJSONFile.__init__
    changed, scratch = [], []

    def initialize(file, stream):
        original_init(file, stream)
        if not isinstance(file, run_json._UncachedSinkFile):
            scratch.append((file, file.fileno()))

    def records(sink, tag, **kwargs):
        if tag == "E" and not changed:
            assert sink._identity == _identity(sink.path)
            assert sink.sha256 and sink.summary_failure is None  # A finished
            mutate(sink)
            changed.append(sink.path)
        return original(sink, tag, **kwargs)

    with monkeypatch.context() as patch:
        patch.setattr(run_json._BoundedJSONFile, "__init__", initialize)
        patch.setattr(run_json._ReportSink, "_tagged_records", records)
        result = ordinary.invoke(tmp_path, patch, library=library)
    status, text, stderr, library = result
    assert len(changed) == 1
    assert status == 1 and stderr == ""
    assert "Traceback" not in text + stderr
    assert library.freed == library.installed == changed
    assert all(not Path(path).exists() for path in changed)
    assert scratch, "the real private report staging file must have been opened"
    for file, fd in scratch:
        assert file.closed and file.buffer is None
        with pytest.raises(OSError):
            os.fstat(fd)
    # One error document, without any provisional report prefix/suffix.
    document = ordinary.json.loads(text)
    assert tuple(document) == ("engine", "error", "code", "args")
    assert document["engine"] == "pineforge"
    assert document["code"] == "engine_invariant" and document["args"] == {}
    return text, document["error"]


def test_equity_pass_parses_only_one_raw_header_at_two_sizes(tmp_path, monkeypatch):
    """The equity scan parses one raw header at both fixture sizes."""
    results = []
    original = pf_sink._parse
    for count in (10, 5000):
        rows = [_point(index) for index in range(count)]
        rows[1:1] = [_trade(1), _event("event-a"), ["K", 50.]]
        with _sink(tmp_path, rows) as (sink, _, _):
            strict = [row for row in _strict(sink.path)[0] if row.tag == "E"]
            raw = Path(sink.path).read_bytes()
            parsed = []

            def parse(line):
                parsed.append(line)
                return original(line)

            # Verification and the independent strict oracle have both finished.
            with monkeypatch.context() as patch:
                patch.setattr(pf_sink, "_parse", parse)
                actual = list(sink.records("E"))
            assert actual == strict and len(actual) == sink.counts["E"] == count
            results.append((count, len(raw), parsed, raw[:raw.index(b"\n") + 1]))
    assert results[0][0] != results[1][0]
    assert results[1][1] > results[0][1] + WINDOW
    # Measure both populations before comparing parser calls, so a regression
    # reports both sizes even when the small population already differs.
    assert [len(parsed) for _, _, parsed, _ in results] == [1, 1]
    for _, _, parsed, header in results:
        assert parsed == [header]


@pytest.mark.parametrize("bulk", [False, True], ids=["strict", "bulk"])
def test_verify_still_strictly_parses_every_line(tmp_path, monkeypatch, bulk):
    """Only canonical E may bypass strict parsing; all other lines retain it."""
    rows = [_point(0), *_mixed(), _point(1)]
    with _sink(tmp_path, rows, verify=False) as (sink, library, report):
        raw = Path(sink.path).read_bytes()
        calls = []
        original = pf_sink._parse

        def parse(line):
            calls.append(line)
            return original(line)

        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_BULK_E_ENABLED", bulk, raising=False)
            patch.setattr(pf_sink, "_parse", parse)
            sink.verify(library, 1, report)
        lines = raw.splitlines(keepends=True)
        expected = ([line for line in lines if not line.startswith(b'["E",')]
                    if bulk else lines)
        assert calls == expected
        assert len(calls) + (sink.counts["E"] if bulk else 0) == 2 + len(rows)
        assert sink.sha256 == library.sha


def test_equity_records_require_a_before_yielding(tmp_path):
    """Equity records require verification before the first yield."""
    with _sink(tmp_path, [_point(0)], verify=False) as (sink, _, _):
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("E"))
        assert type(failure.value) is run_json.RunFailure
        assert failure.value.code == "engine_invariant"
        assert observed == []


@pytest.mark.parametrize("escaped_tags", [(), ("E",)], ids=["literal", "escaped-E"])
def test_verified_equity_decoder_matches_strict_types_hex_and_frozen_bytes(
        tmp_path, escaped_tags):
    """Verified decoding preserves strict types, float bits, and frozen bytes."""
    rows = [
        ["E", -I53, -0.0, 0.0],
        ["E", I53, 5e-324, -5e-324],
        ["E", 0, 1e+16, 1e-07],
        ["E", 1, None, -0.0],
        ["E", 2, 1.5, None],
        ["E", 3, None, None],
        ["E", 4, 1.7976931348623157e308, 0.1 + 0.2],
    ]
    for index, (value, alternates) in enumerate(_EQUITY_FLOAT_SPELLINGS, start=5):
        text = repr(value)
        assert repr(float(text)) == text
        assert ordinary.encoded(value) == text.encode("ascii") + b"\n"
        for alternate in alternates:
            assert alternate != text
            assert repr(float(alternate)) == text
            assert float(alternate).hex() == value.hex()
        # Both signs, with the original seven rows retained above.
        rows.append(["E", index, value, -value])
    with _sink(tmp_path, rows, escaped_tags=escaped_tags) as (sink, _, _):
        strict, header, counts, digest, _ = _strict(sink.path)
        raw = Path(sink.path).read_bytes()
        lines = raw[:_trailer_start(raw)].splitlines(keepends=True)[1:]
        assert counts["E"] == len(rows) == len(lines) == len(strict)
        assert digest == sink.sha256
        if escaped_tags:
            assert all(line.startswith(b'["\\u0045",') for line in lines)
        assert b"-0.0" in raw and b"5e-324" in raw
        assert b"1e+16" in raw and b"1e-07" in raw
        parser = pf_sink.RecordParser(header)
        decode = _verified_decoder(parser)
        decoded, parsed = [], []
        rows_compared = 0
        for line, record in zip(lines, strict):
            reference = parser.parse(line)
            _assert_record(reference, record)
            actual = decode(line)
            _assert_record(actual, reference)
            decoded.append(actual)
            parsed.append(reference)
            rows_compared += 1
        assert rows_compared == len(rows) and rows_compared > 0
        frozen = _array_bytes(strict)
        assert _array_bytes(parsed) == _array_bytes(decoded) == frozen
        assert _array_bytes(decoded, run_json._stream_json) == frozen
        assert _array_bytes(sink.records("E"), run_json._stream_json) == frozen
        assert b'"equity":-0.0' in frozen and b'"open_profit":-0.0' in frozen


@pytest.mark.parametrize("line,text", [
    (b'["E",9007199254740992,1.0,0.0]\n',
     "integer exceeds the exact JSON integer bound"),
    (b'["E",-9007199254740992,1.0,0.0]\n',
     "integer exceeds the exact JSON integer bound"),
    (b'["E",null,1.0,0.0]\n', "null in a non-optional column"),
    (b'["E",true,1.0,0.0]\n', "invalid i64 column"),
    (b'["E",false,1.0,0.0]\n', "invalid i64 column"),
    (b'["E",0,1,0.0]\n', "f64 column is not the Python repr of its double"),
    (b'["E",0,1.0,0]\n', "f64 column is not the Python repr of its double"),
    (b'["E",0,1.50,0.0]\n', "f64 column is not the Python repr of its double"),
    (b'["E",0,1E5,0.0]\n', "f64 column is not the Python repr of its double"),
] + [
    ((b'["E",0,' + token + b',0.0]\n' if column == "equity" else
      b'["E",0,1.0,' + token + b']\n'),
     "f64 column is not the Python repr of its double")
    for column in ("equity", "open-profit") for token in _NON_REPR_FLOAT_TOKENS
], ids=["above-I53", "below-minus-I53", "null-time", "bool-true-time",
        "bool-false-time", "int-equity", "int-open-profit", "1.50", "1E5"] + [
    column + "-" + token.decode("ascii")
    for column in ("equity", "open-profit") for token in _NON_REPR_FLOAT_TOKENS
])
def test_a_and_verified_decoder_both_refuse_invalid_equity_tokens(tmp_path, line, text):
    """Verification and the verified decoder reject invalid equity tokens."""
    with _sink(tmp_path, [_point(0)], verify=False) as (sink, library, report):
        path = Path(sink.path)
        # Before verification, no identity or receipt check can mask the line's
        # lexical or field refusal.
        path.write_bytes(_replace_line(path.read_bytes(), ordinary.encoded(_point(0)), line))
        with pytest.raises(pf_sink.SinkFormatError) as reference:
            _strict(path)
        assert str(reference.value) == "line 2: " + text
        with pytest.raises(run_json.RunFailure) as failure:
            sink.verify(library, 1, report)
        _assert_format_error(failure.value)
        assert str(failure.value) == INVALID + str(reference.value)
        assert sink._identity is None
        parser = pf_sink.RecordParser(ordinary.header(outputs=True))
        with pytest.raises(pf_sink.SinkFormatError) as strict:
            parser.parse(line)
        assert str(strict.value) == text
        with pytest.raises(pf_sink.SinkFormatError) as decoded:
            _verified_decoder(parser)(line)
        assert str(decoded.value) == text


def _replace_line(raw, old, new):
    """Replace fixture bytes, allowing length changes only before verification."""
    assert raw.count(b"\n" + old) == 1
    return raw.replace(b"\n" + old, b"\n" + new, 1)


@pytest.mark.parametrize("space", [b" ", b"\t", b"\r"], ids=["space", "tab", "control-CR"])
def test_a_refuses_whitespace_but_c_proves_the_same_changed_bytes_by_digest(
        tmp_path, monkeypatch, space):
    """Verification rejects whitespace; later edits fail the raw digest."""
    point = ["E", ordinary.bars.T0, 10000.0, -0.0]
    line = ordinary.encoded(point)
    position = line.index(b",") + 1
    assert line[position:position + 1] in b"123456789"
    changed_line = line[:position] + space + line[position + 1:]
    assert len(changed_line) == len(line)
    with _sink(tmp_path, [point], verify=False) as (sink, library, report):
        _write_same_identity(sink.path, _replace_once(
            Path(sink.path).read_bytes(), line, changed_line))
        with pytest.raises(pf_sink.SinkFormatError) as reference:
            _strict(sink.path)
        assert str(reference.value) == (
            "line 2: whitespace or control byte outside a JSON string")
        with pytest.raises(run_json.RunFailure) as failure:
            sink.verify(library, 1, report)
        assert str(failure.value) == INVALID + str(reference.value)
        _assert_format_error(failure.value)

    def mutate(sink):
        _write_same_identity(sink.path, _replace_once(
            Path(sink.path).read_bytes(), line, changed_line))

    text, error = _invoke_after_equity_tamper(tmp_path, monkeypatch, mutate)
    assert error == DIGEST_DIFFERS
    assert text == run_json.failure_line(DIGEST_DIFFERS, "engine_invariant", {})


@pytest.mark.parametrize("where", ["equity", "non-E"])
@pytest.mark.parametrize("wrong_count", [False, True], ids=["count-intact", "count-also-wrong"])
def test_same_size_equity_or_non_e_flip_has_digest_priority(tmp_path, where, wrong_count):
    """The raw digest detects changed equity or skipped bytes before count checks."""
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        changed = (_equity_flip(raw)[0] if where == "equity" else
                   _replace_once(raw, b"event-a", b"event-b"))
        _write_same_identity(sink.path, changed)
        count = sink.counts["E"]
        if wrong_count:
            sink.counts["E"] += 1
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("E"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        assert len(observed) == count
        assert observed[0].as_dict()["equity"] == (
            90000.0 if where == "equity" else 10000.0)


def test_non_e_tamper_before_a_keeps_strict_reader_text(tmp_path):
    """Verification checks the body digest even when only other tags change."""
    with _sink(tmp_path, _mixed(), verify=False) as (sink, library, report):
        _write_same_identity(sink.path, _replace_once(
            Path(sink.path).read_bytes(), b"event-a", b"event-b"))
        with pytest.raises(pf_sink.SinkFormatError) as reference:
            _strict(sink.path)
        assert str(reference.value).endswith("trailer SHA-256 mismatch")
        with pytest.raises(run_json.RunFailure) as failure:
            sink.verify(library, 1, report)
        _assert_format_error(failure.value)
        assert str(failure.value) == INVALID + str(reference.value)


@pytest.mark.parametrize("delta", [-1, 1], ids=["count-low", "count-high"])
def test_equity_verified_count_alone_is_not_a_file_change(tmp_path, delta):
    """A verified equity count mismatch has its own refusal after a matching digest."""
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        raw, digest = Path(sink.path).read_bytes(), sink.sha256
        count = sink.counts["E"]
        sink.counts["E"] += delta
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("E"))
        _assert_engine_error(failure.value, COUNT_DIFFERS)
        assert "changed after verification" not in str(failure.value)
        assert len(observed) == count
        assert Path(sink.path).read_bytes() == raw and sink.sha256 == digest


@pytest.mark.parametrize("change", ["append", "shorten", "replace-same", "replace-different"])
def test_equity_identity_is_an_early_refusal_only(tmp_path, change):
    """An inode or size change is refused before yielding equity records."""
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        path = Path(sink.path)
        inode, size = _identity(path)
        if change == "append":
            with path.open("ab") as file:
                file.write(b"x")
            assert _identity(path) == (inode, size + 1)
        elif change == "shorten":
            with path.open("r+b") as file:
                file.truncate(size - 1)
            assert _identity(path) == (inode, size - 1)
        else:
            replacement = tmp_path / "replacement.sink"
            if change == "replace-same":
                replacement.write_bytes(path.read_bytes())
            else:
                rows = _mixed()
                rows[1][2] = 90000.0
                ordinary.write_sink(replacement, rows, outputs=True)
            assert replacement.stat().st_size == size
            assert replacement.stat().st_ino != inode
            os.replace(replacement, path)
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("E"))
        _assert_engine_error(failure.value, IDENTITY_DIFFERS)
        assert observed == []


def test_equity_same_inode_valid_new_receipt_still_differs_from_a(tmp_path):
    """A rewritten valid receipt still differs from the original verified digest."""
    rows = _mixed()
    with _sink(tmp_path, rows) as (sink, _, _):
        identity, digest = _identity(sink.path), sink.sha256
        rows[1][2] = 90000.0
        _, new_digest = ordinary.write_sink(sink.path, rows, outputs=True)
        assert _identity(sink.path) == identity and new_digest != digest
        assert _strict(sink.path)[3] == new_digest
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("E"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        assert len(observed) == sink.counts["E"]


def test_equity_digest_refusal_discards_provisional_report_bytes(tmp_path, monkeypatch):
    """A digest refusal discards provisional report bytes before publication."""
    def mutate(sink):
        changed, _ = _equity_flip(Path(sink.path).read_bytes())
        _write_same_identity(sink.path, changed)

    text, error = _invoke_after_equity_tamper(tmp_path, monkeypatch, mutate)
    assert error == DIGEST_DIFFERS
    assert text == run_json.failure_line(DIGEST_DIFFERS, "engine_invariant", {})


@pytest.mark.parametrize("fault,line,detail", [
    ("non-list", b'{}\n', "record must be a nonempty array with a string tag"),
    ("wrong-tag", ordinary.encoded(_trade(1)), "record tag is 'T', expected 'E'"),
], ids=["non-list", "wrong-tag"])
def test_verified_equity_decoder_outer_shape_refusals_are_typed(fault, line, detail):
    """Direct decoding gives typed errors for non-array records and wrong tags."""
    parser = pf_sink.RecordParser(ordinary.header(outputs=True))
    if fault == "wrong-tag":
        # A valid T record isolates the E-only decoder's tag refusal.
        assert parser.parse(line).tag == "T"
    with pytest.raises(pf_sink.SinkFormatError) as failure:
        _verified_decoder(parser)(line)
    assert type(failure.value) is pf_sink.SinkFormatError
    assert str(failure.value) == detail


def _decoder_fault_line(fault):
    if fault == "recursion":
        prefix, suffix = b'["E",0,', b',0.0]\n'
        depth = (pf_sink.MAX_LINE_BYTES - len(prefix) - len(suffix)) // 2
        line = prefix + b"[" * depth + b"]" * depth + suffix
        assert pf_sink.MAX_LINE_BYTES - 1 <= len(line) <= pf_sink.MAX_LINE_BYTES
        return line
    return {
        "invalid-UTF8": b'["E",0,"\xff",0.0]\n',
        "malformed-JSON": b'["E",?,1.0,0.0]\n',
        "wrong-arity": b'["E",0,1.0]\n',
        "nested-time": b'["E",[],1.0,0.0]\n',
        "nested-equity": b'["E",0,{},0.0]\n',
        "nested-open-profit": b'["E",0,1.0,[]]\n',
    }[fault]


@pytest.mark.parametrize("fault", [
    "recursion", "invalid-UTF8", "malformed-JSON",
    "wrong-arity", "nested-time", "nested-equity", "nested-open-profit",
])
def test_equity_decoder_exceptions_after_a_are_typed_and_never_published(
        tmp_path, monkeypatch, fault):
    """Malformed equity reaches the verified decoder before any digest verdict."""
    payload = _decoder_fault_line(fault)
    # Reserve a valid event before verification. Its later replacement is the
    # decoder's exact input at the same inode and size, before digest comparison.
    size = max(len(payload), len(ordinary.encoded(_event(""))) + 32)
    reserve = _event("p" * (size - len(ordinary.encoded(_event("")))))
    reserve_line = ordinary.encoded(reserve)
    assert len(reserve_line) == size
    damaged = payload[:-1].ljust(size - 1, b" ") + b"\n"
    assert len(damaged) == size
    original_write = ordinary.write_sink
    written = []
    tail = _event("tail" + "x" * (2 * WINDOW))

    def write(path, rows, **kwargs):
        written.append(path)
        return original_write(path, [*rows, reserve, tail], **kwargs)

    monkeypatch.setattr(ordinary, "write_sink", write)
    library = ordinary.SinkLibrary(outputs=True)
    seen, loads_seen, recursive, fields, scanned = [], [], [], [], []
    active = False

    def mutate(sink):
        raw = Path(sink.path).read_bytes()
        assert sink.counts["V"] >= 1
        _write_same_identity(sink.path, _replace_once(raw, reserve_line, damaged))
        original_decode = _verified_decoder(pf_sink.RecordParser(sink.header)).__func__
        original_loads, original_field = pf_sink.json.loads, pf_sink._field
        original_read = run_json._UncachedSinkFile.read

        def read(file, count=-1):
            chunk = original_read(file, count)
            scanned.append((file.position, file.size))
            return chunk

        def decode(parser, line):
            nonlocal active
            target = line == damaged
            if target:
                # A two-window tail follows the reserved line. At least a
                # window is still unread: this failure precedes EOF and
                # hashing the whole verified prefix.
                assert scanned and scanned[-1][0] + WINDOW < scanned[-1][1]
                seen.append(line)
                active = True
            try:
                return original_decode(parser, line)
            finally:
                if target:
                    active = False

        def loads(value, *args, **kwargs):
            if active:
                loads_seen.append(value)
            try:
                result = original_loads(value, *args, **kwargs)
            except RecursionError:
                if active:
                    recursive.append(True)
                raise
            return result

        def field(value, kind):
            if active:
                fields.append((value, kind))
            return original_field(value, kind)

        monkeypatch.setattr(pf_sink.RecordParser, "parse_verified", decode)
        monkeypatch.setattr(pf_sink.json, "loads", loads)
        monkeypatch.setattr(pf_sink, "_field", field)
        monkeypatch.setattr(run_json._UncachedSinkFile, "read", read)

    _, error = _invoke_after_equity_tamper(tmp_path, monkeypatch, mutate, library=library)
    assert len(written) == 1
    assert seen == [damaged]
    # UTF-8 decoding fails before json.loads can be called.
    assert len(loads_seen) == (0 if fault == "invalid-UTF8" else 1)
    if fault.startswith("nested-"):
        assert fields, "the verified decoder must look up the module-global _field"
    assert error.startswith(INVALID)
    if fault in ("recursion", "invalid-UTF8", "malformed-JSON"):
        assert error.startswith(INVALID + "invalid UTF-8 or JSON: ")
        if fault == "recursion":
            assert recursive == [True]
            assert "recursion" in error
    elif fault == "wrong-arity":
        assert error == INVALID + "record field count differs from its descriptor"
    elif fault == "nested-time":
        assert error == INVALID + "invalid i64 column"
        assert any(type(value) is list and kind == "i64" for value, kind in fields)
    else:
        assert error == INVALID + "f64 column is not the Python repr of its double"
        assert any(type(value) in (dict, list) and kind == "f64?"
                   for value, kind in fields)


@pytest.mark.parametrize("fault,detail", [
    ("no-LF", "line does not end with LF"),
    ("zero-byte-body", "blank line"),
], ids=["line-without-LF", "zero-byte-body"])
def test_equity_framing_exceptions_after_a_are_typed_and_never_published(
        tmp_path, monkeypatch, fault, detail):
    """Malformed equity framing gives typed errors and prevents publication."""
    with _sink(tmp_path, [_point(0)]) as (sink, _, _):
        path = Path(sink.path)
        raw = path.read_bytes()
        start = raw.index(b'\n["E",') + 1
        end = raw.index(b"\n", start) + 1
        if fault == "no-LF":
            # Write an unterminated equity record after verification. This direct
            # decoder call bypasses the early identity check.
            path.write_bytes(raw[:end - 1])
            damaged = path.read_bytes()[start:]
            assert damaged == raw[start:end - 1] and not damaged.endswith(b"\n")
        else:
            path.write_bytes(raw[:start] + b"\n" + raw[end:])
            damaged = path.read_bytes()[start:start + 1]
            assert damaged == b"\n"  # LF present, zero bytes in its body
        parser = pf_sink.RecordParser(sink.header)
        with pytest.raises(pf_sink.SinkFormatError) as reference:
            parser.parse(damaged)
        assert str(reference.value) == detail
        with pytest.raises(pf_sink.SinkFormatError) as decoded:
            _verified_decoder(parser)(damaged)
        assert str(decoded.value) == detail

    # Preserve inode/size for the pipeline check. No LF is a framing error;
    # blank lines have no E needle, so their changed bytes fail the digest.
    def mutate(sink):
        raw = Path(sink.path).read_bytes()
        if fault == "no-LF":
            assert raw.endswith(b"\n")
            changed = raw[:-1] + b" "
        else:
            start = raw.index(b'\n["E",') + 1
            end = raw.index(b"\n", start) + 1
            # Empty record bodies, retaining the verified inode and size.
            changed = raw[:start] + b"\n" * (end - start) + raw[end:]
        _write_same_identity(sink.path, changed)

    text, error = _invoke_after_equity_tamper(tmp_path, monkeypatch, mutate)
    if fault == "no-LF":
        assert error.startswith(INVALID) and error.endswith(detail)
        assert "changed after verification" not in error
    else:
        assert error == DIGEST_DIFFERS
        assert text == run_json.failure_line(DIGEST_DIFFERS, "engine_invariant", {})


@pytest.mark.parametrize("fault", ["header", "equity", "trailer-LF", "after-trailer"])
def test_equity_format_and_eof_refusals_precede_digest_and_count(tmp_path, fault):
    """Format and EOF errors take priority over digest and count mismatches."""
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        if fault == "header":
            changed = b"!" + raw[1:]
        elif fault == "equity":
            start = raw.index(b'\n["E",') + 1
            position = raw.index(b",", start) + 1
            changed = raw[:position] + b"?" + raw[position + 1:]
        elif fault == "trailer-LF":
            changed = raw[:-1] + b" "
        else:
            assert raw.endswith(b'"}\n')
            changed = raw[:-3] + b"\nx\n"
        _write_same_identity(sink.path, changed)
        sink.sha256 = "0" * 64
        sink.counts["E"] += 1
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records("E"))
        _assert_format_error(failure.value)
        if fault in ("header", "equity"):
            assert "invalid UTF-8 or JSON" in str(failure.value)
        elif fault == "trailer-LF":
            assert str(failure.value).endswith("line does not end with LF")
        # Bytes after the trailer must cause a format refusal before the final
        # digest and count comparisons.


def test_one_fresh_raw_equity_stream_supplies_every_hashed_byte(tmp_path, monkeypatch):
    """One fresh raw equity stream supplies the decoded and hashed bytes."""
    rows = [_point(0), _trade(1), _event("x" * (3 * WINDOW + 17)), _point(1)]
    with _sink(tmp_path, rows) as (sink, _, _):
        strict = [row for row in _strict(sink.path)[0] if row.tag == "E"]
        raw = Path(sink.path).read_bytes()
        opened, opens, chunks, states = [], [], [], []
        original_open = run_json._open_uncached_sink
        original_init = run_json._UncachedSinkFile.__init__
        original_read, original_hash = run_json._UncachedSinkFile.read, hashlib.sha256
        cursor = 0

        def open_sink(path, *, raw=False):
            opens.append((path, raw))
            assert raw, "C needs a fresh raw stream from byte zero"
            return original_open(path, raw=raw)

        def initialize(file, stream):
            assert isinstance(stream, io.FileIO)
            assert stream.tell() == 0
            original_init(file, stream)
            opened.append((file, file.fileno()))

        def read(file, count=-1):
            nonlocal cursor
            assert 0 < count <= WINDOW
            assert len(opened) == 1 and file is opened[0][0]
            assert file.position == cursor
            chunk = original_read(file, count)
            chunks.append(chunk)
            cursor += len(chunk)
            return chunk

        def no_line_or_seek(*args, **kwargs):
            pytest.fail("C must use raw sequential reads, without readline or seek")

        class Digest:
            def __init__(self, data=b"", **kwargs):
                self.inner = original_hash(data, **kwargs)
                self.parts = [bytes(data)] if data else []
                states.append(self)

            def update(self, data):
                self.parts.append(bytes(data))
                return self.inner.update(data)

            def __getattr__(self, name):
                return getattr(self.inner, name)

        monkeypatch.setattr(run_json, "_open_uncached_sink", open_sink)
        monkeypatch.setattr(run_json._UncachedSinkFile, "__init__", initialize)
        monkeypatch.setattr(run_json._UncachedSinkFile, "read", read)
        monkeypatch.setattr(run_json._UncachedSinkFile, "readline", no_line_or_seek)
        monkeypatch.setattr(run_json._UncachedSinkFile, "seek", no_line_or_seek)
        monkeypatch.setattr(hashlib, "sha256", Digest)
        assert list(sink.records("E")) == strict
        assert opens == [(sink.path, True)]
        assert len(states) == 1 and len(chunks) > 3
        assert b"".join(chunks) == raw
        assert b"".join(states[0].parts) == raw[:_trailer_start(raw)]
        _assert_closed(opened)


@pytest.mark.parametrize("wrong_count", [False, True], ids=["count-intact", "count-also-wrong"])
def test_transient_equity_read_flip_is_hashed_with_decoded_bytes(
        tmp_path, monkeypatch, wrong_count):
    """The digest hashes the same transiently changed bytes that equity decoding sees."""
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        raw, identity = Path(sink.path).read_bytes(), _identity(sink.path)
        original = run_json._UncachedSinkFile.read
        flips = []

        def read(file, count=-1):
            chunk = original(file, count)
            if b",10000.0," in chunk:
                flips.append(file.fileno())
                return _replace_once(chunk, b",10000.0,", b",90000.0,")
            return chunk

        monkeypatch.setattr(run_json._UncachedSinkFile, "read", read)
        count = sink.counts["E"]
        if wrong_count:
            sink.counts["E"] += 1
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("E"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        assert len(flips) == 1 and len(observed) == count
        assert observed[0].as_dict()["equity"] == 90000.0
        assert Path(sink.path).read_bytes() == raw and _identity(sink.path) == identity


def test_equity_body_flip_in_the_trailer_window_is_still_hashed(tmp_path):
    """Equity bytes sharing a window with the trailer remain inside the digest."""
    point = _point(0)
    start = WINDOW - 20
    with _sink(tmp_path, [_padding_before(start, following=[point]), point]) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        assert _trailer_start(raw) == start
        changed, position = _equity_flip(raw)
        assert position // WINDOW == start // WINDOW
        _write_same_identity(sink.path, changed)
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records("E"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)


def test_equity_early_close_releases_fd_and_skips_final_comparisons(tmp_path, monkeypatch):
    """Early close releases the descriptor without final digest or count checks."""
    with _sink(tmp_path, [_point(0), _event("x" * (3 * WINDOW)), _point(1)]) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "E"]
        opened = []
        original = run_json._UncachedSinkFile.__init__

        def initialize(file, stream):
            original(file, stream)
            opened.append((file, file.fileno()))

        monkeypatch.setattr(run_json._UncachedSinkFile, "__init__", initialize)
        sink.sha256 = "0" * 64
        sink.counts["E"] += 1
        records = sink.records("E")
        try:
            assert next(records) == expected[0]
            assert len(opened) == 1 and not opened[0][0].closed
        finally:
            records.close()
        _assert_closed(opened)


@pytest.mark.parametrize("fault", ["eio", "truncate"],
                         ids=["injected-EIO", "mid-scan-truncation"])
def test_oserror_during_equity_scan_is_typed_and_releases_fd(tmp_path, monkeypatch, fault):
    """Read errors and truncation give typed format refusals and close the descriptor."""
    rows = [_point(0), _event("x" * (4 * WINDOW)), _point(1)]
    with _sink(tmp_path, rows) as (sink, _, _):
        path = Path(sink.path)
        size = path.stat().st_size
        assert size > 3 * WINDOW
        opened, calls = [], []
        original_init = run_json._UncachedSinkFile.__init__
        original_read = run_json._UncachedSinkFile.read

        def initialize(file, stream):
            original_init(file, stream)
            opened.append((file, file.fileno()))

        def read(file, count=-1):
            calls.append(count)
            if len(calls) == 2:
                assert file.position > 0
                if fault == "eio":
                    raise OSError(errno.EIO, "injected")
                assert count > 17
                cutoff = file.position + 17
                assert cutoff < size
                with path.open("r+b") as changed:
                    changed.truncate(cutoff)
            return original_read(file, count)

        monkeypatch.setattr(run_json._UncachedSinkFile, "__init__", initialize)
        monkeypatch.setattr(run_json._UncachedSinkFile, "read", read)
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records("E"))
        _assert_format_error(failure.value)
        # A real read must detect an I/O failure or a truncation after the first
        # window; the raw scan must close its descriptor on either refusal.
        assert len(calls) >= 2
        _assert_closed(opened)


@pytest.mark.parametrize("population", ["empty", "zero-E", "first", "last",
                                      "first-and-last", "many-in-one-window"])
def test_equity_population_and_file_order(tmp_path, population):
    """Equity scans preserve order across empty, absent, and populated cases."""
    if population == "empty":
        rows, count = [], 0
    elif population == "zero-E":
        rows, count = [_trade(1), _event("event-a"), ["K", 50.]], 0
    elif population == "first":
        rows, count = [_point(0), _trade(1), ["K", 50.]], 1
    elif population == "last":
        rows, count = [_trade(1), ["K", 50.], _point(0)], 1
    elif population == "first-and-last":
        rows, count = [_point(0), _trade(1), _event("middle"), _point(1)], 2
    else:
        rows, count = [_point(index) for index in range(191)], 191
    with _sink(tmp_path, rows) as (sink, _, _):
        if population == "many-in-one-window":
            assert _trailer_start(Path(sink.path).read_bytes()) < WINDOW
        expected = [row for row in _strict(sink.path)[0] if row.tag == "E"]
        actual = list(sink.records("E"))
        assert actual == expected and len(actual) == sink.counts["E"] == count
        assert _array_bytes(actual, run_json._stream_json) == _array_bytes(expected)
        for left, right in zip(actual, expected):
            _assert_record(left, right)


@pytest.mark.parametrize("escaped_tags", [(), ("E",)], ids=["literal-E", "escaped-E"])
@pytest.mark.parametrize("edge", ["equity", "trailer"])
@pytest.mark.parametrize("pad", range(WINDOW - 14, WINDOW + 15),
                         ids=lambda pad: f"pad={pad}")
def test_equity_window_edge_sweep_matches_strict_rows(tmp_path, escaped_tags, edge, pad):
    """Literal and escaped equity rows stay exact across record and trailer edges."""
    tail = [_point(0), _trade(1), _event("middle"),
            ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0.0, 4294967295.0],
            _point(1), _trade(2), ["K", 50.]]
    extra = 0
    if escaped_tags:
        for point in (tail[0], tail[4]):
            literal = ordinary.encoded(point)
            escaped = literal.replace(b'["E",', b'["\\u0045",', 1)
            extra += len(escaped) - len(literal)
    padding = (_padding_before(pad) if edge == "equity" else
               _padding_before(pad - extra, following=tail))
    with _sink(tmp_path, [padding, *tail], escaped_tags=escaped_tags) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        needle = b'\n["\\u0045",' if escaped_tags else b'\n["E",'
        assert raw.count(needle) == 2
        if edge == "equity":
            assert raw.index(needle) + 1 == pad
            if pad < WINDOW:
                assert pad < WINDOW < raw.index(b"\n", pad) + 1
        else:
            assert _trailer_start(raw) == pad
            assert len(raw) > WINDOW
            if pad == WINDOW:
                assert raw[WINDOW - 1:WINDOW + 1] == b"\n{"
        strict, _, counts, digest, _ = _strict(sink.path)
        expected = [row for row in strict if row.tag == "E"]
        actual = list(sink.records("E"))
        assert len(actual) == counts["E"] == sink.counts["E"] == 2
        assert digest == sink.sha256
        assert actual == expected
        assert _array_bytes(actual, run_json._stream_json) == _array_bytes(expected)
        for left, right in zip(actual, expected):
            _assert_record(left, right)


def test_escaped_equity_token_crosses_window_and_non_e_decodes_only_tag(
        tmp_path, monkeypatch):
    """Escaped equity spans windows while other record bodies remain undecoded."""
    start = WINDOW - 4
    rows = [_padding_before(start - 5), _point(0),
            _event('quoted "E",{' + "x" * (WINDOW + 17)), _trade(1), _point(1)]
    with _sink(tmp_path, rows, escaped_tags=("E", "T", "V")) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        # The padding V tag is also escaped, adding five bytes before E.
        assert raw.index(b'\n["\\u0045",') + 1 == start
        assert raw[start - 1:start + 4] == b'\n["\\u'
        expected = [row for row in _strict(sink.path)[0] if row.tag == "E"]
        parsed, decoded = [], []
        original_parse, original_loads = pf_sink._parse, pf_sink.json.loads

        def parse(line):
            parsed.append(line)
            return original_parse(line)

        def loads(value, *args, **kwargs):
            result = original_loads(value, *args, **kwargs)
            decoded.append(result)
            return result

        monkeypatch.setattr(pf_sink, "_parse", parse)
        monkeypatch.setattr(pf_sink.json, "loads", loads)
        assert list(sink.records("E")) == expected
        assert parsed == [raw[:raw.index(b"\n") + 1]]
        assert decoded, "the scan must look up json.loads through the module global"
        equity_decodes = [value for value in decoded if isinstance(value, list)]
        assert len(equity_decodes) == len(expected) == sink.counts["E"] == 2
        assert all(not isinstance(value, list) or value[0] == "E" for value in decoded)
