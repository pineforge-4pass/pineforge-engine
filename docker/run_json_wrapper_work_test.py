"""Compare wrapper bytes and real interpreter work against independent originals.

The copied functions retain the original proofs and conversion order. Work
observers use interpreter events on real parsers, regexes and CSV conversion
frames; product functions contain no counters. No native module is required.
"""
from collections import Counter
from contextlib import contextmanager
import csv
import ctypes
import hashlib
import inspect
import io
from itertools import chain
import json
import math
from pathlib import Path
import re
import struct
import sys
import textwrap

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import run_json_wrapper_test_support as original
import run_json_fused_test_support as support
import test_run_json_sink as ordinary
from run_json_trade_scan_test import _event, _point, _trade
from run_json_wrapper_test_support import (
    _REFERENCE_E_LINE, _REFERENCE_E_RUN,
    _reference_bulk_equity, _reference_bulk_equity_ascii,
)


class _TemplatePattern:
    """Expose actual Match.expand calls for a string replacement on any CPython."""
    def __init__(self, pattern):
        self.pattern = pattern

    def __getattr__(self, name):
        return getattr(self.pattern, name)

    def sub(self, replacement, data, *args, **kwargs):
        if isinstance(replacement, (str, bytes)):
            # A string template expands once for every matching row. Delegate
            # each expansion to the actual regex engine, under the profiler.
            return self.pattern.sub(
                lambda match: match.expand(replacement), data, *args, **kwargs)
        return self.pattern.sub(replacement, data, *args, **kwargs)


class _EquityWork:
    def __init__(self, *, reference=False):
        self.reference = reference
        self.line = _REFERENCE_E_LINE if reference else run_json._BULK_E_LINE
        self.run = _REFERENCE_E_RUN if reference else run_json._BULK_E_RUN
        self.traversals = Counter()
        self.expansions = 0

    def _profile(self, frame, event, arg):
        if event != "c_call":
            return
        owner, name = getattr(arg, "__self__", None), getattr(arg, "__name__", "")
        if ((owner is self.line or owner is self.run)
                and name in ("match", "fullmatch", "findall", "finditer", "sub", "subn", "search")):
            self.traversals[name] += 1
        elif name == "expand" and isinstance(owner, re.Match) and owner.re is self.line:
            self.expansions += 1

    @contextmanager
    def install(self, monkeypatch):
        module = original if self.reference else run_json
        name = "_REFERENCE_E_LINE" if self.reference else "_BULK_E_LINE"
        previous = sys.getprofile()
        with monkeypatch.context() as patch:
            patch.setattr(module, name, _TemplatePattern(self.line))
            sys.setprofile(self._profile)
            try:
                yield self
            finally:
                sys.setprofile(previous)

    def assert_bounded(self, *, batches=1, non_equity_attempts=0):
        assert self.expansions == 0, (
            f"EQUITY_TEMPLATE_WORK: {self.expansions} real template expansions; budget is zero")
        expected = Counter(match=batches + non_equity_attempts, findall=batches)
        assert self.traversals == expected, (
            f"EQUITY_REGEX_WORK: {dict(self.traversals)}; expected exactly two complete "
            f"traversals per accepted batch (RUN.match and LINE.findall, {batches} batches), "
            f"plus {non_equity_attempts} other-tag lookaheads, and none in the encoder")


def _equity_batch(raw, monkeypatch, *, reference=False):
    prove = _reference_bulk_equity if reference else run_json._bulk_equity
    encode = _reference_bulk_equity_ascii if reference else run_json._bulk_equity_ascii
    pattern = _REFERENCE_E_RUN if reference else run_json._BULK_E_RUN
    work = _EquityWork(reference=reference)
    with work.install(monkeypatch):
        match = pattern.match(raw)
        assert match is not None and match.end() == len(raw)
        batch = prove(raw)
        assert batch is not None
        encoded = encode(batch)
    return encoded, work


@pytest.mark.parametrize("count", (1, 37, 128))
def test_equity_encoding_has_no_templates_and_only_two_regex_passes(monkeypatch, count):
    raw = b"".join(ordinary.encoded(_point(index)) for index in range(count))
    expected, control = _equity_batch(raw, monkeypatch, reference=True)
    assert control.expansions == count
    assert control.traversals == Counter(match=1, fullmatch=1, findall=1, sub=1)
    actual, work = _equity_batch(raw, monkeypatch)
    assert actual == expected
    work.assert_bounded()


def _template_encoder(batch):
    rows = run_json._BULK_E_LINE.sub(
        rb'{"time_ms":\1,"equity":\2,"open_profit":\3},' + b"\n",
        batch.raw).splitlines()
    return b"".join(rows)[:-1] if rows else b""


@pytest.mark.parametrize("kind", ("template", "fullmatch", "encode-rescan"))
def test_equity_counter_detects_reintroduced_work(monkeypatch, kind):
    raw = b"".join(ordinary.encoded(_point(index)) for index in range(37))
    expected, control = _equity_batch(raw, monkeypatch, reference=True)
    assert control.expansions == 37 and sum(control.traversals.values()) == 4
    prove, encode = run_json._bulk_equity, run_json._bulk_equity_ascii

    def repeated_proof(data):
        assert run_json._BULK_E_RUN.fullmatch(data) is not None
        return prove(data)

    def rescanned_encoding(batch):
        run_json._BULK_E_LINE.findall(batch.raw)
        return encode(batch)

    if kind == "template":
        monkeypatch.setattr(run_json, "_bulk_equity_ascii", _template_encoder)
    elif kind == "fullmatch":
        monkeypatch.setattr(run_json, "_bulk_equity", repeated_proof)
    else:
        monkeypatch.setattr(run_json, "_bulk_equity_ascii", rescanned_encoding)
    actual, work = _equity_batch(raw, monkeypatch)
    assert actual == expected
    with pytest.raises(AssertionError, match="EQUITY_(TEMPLATE|REGEX)_WORK"):
        work.assert_bounded()


def _batch_outcome(prove, encode, raw):
    batch = prove(raw)
    if batch is None:
        return None
    return (batch.raw, batch.count, ordinary.encoded(batch.first),
            ordinary.encoded(batch.last), encode(batch))


_I53 = (1 << 53) - 1
_EQUITY_WIRES = (
    b'["E",0,10000.0,0.0]\n',
    b'["E",1,-0.0,-0.0]\n',
    b'["E",1,1e-07,5e-324]\n',
    b'["E",1,1e+16,-1e+16]\n',
    b'["E",1,9999999999999998.0,0.0]\n',
    b'["E",1,10000000000000000.0,0.0]\n',
    b'["E",1,1.0000000000000002e+16,0.0]\n',
    b'["E",1,10000000000000002.0,0.0]\n',
    b'["E",1,1.7976931348623157e+308,0.0]\n',
    f'["E",{-_I53},-1.7976931348623157e+308,-1.7976931348623157e+308]\n'.encode("ascii"),
    b'["E",1,+1.0,0.0]\n',
    b'["E",1,1.00,0.0]\n',
    b'["E",1,1e0,0.0]\n',
    b'["E",1,1e+016,0.0]\n',
    b'["E",1,1e999,0.0]\n',
    b'["\\u0045",1,1.0,0.0]\n',
    b'["E",-0,1.0,0.0]\n',
    b'["E",1,1.0,0.0]',
    b'["E", 1,1.0,0.0]\n',
    b'\xef\xbb\xbf["E",1,1.0,0.0]\n',
    *(f'["E",{time},1.0,0.0]\n'.encode("ascii")
      for time in (-_I53 - 1, -_I53, _I53, _I53 + 1)),
)


@pytest.mark.parametrize("wire", _EQUITY_WIRES)
def test_equity_proof_and_encoding_match_original(wire):
    assert _batch_outcome(run_json._bulk_equity, run_json._bulk_equity_ascii, wire) == (
        _batch_outcome(_reference_bulk_equity, _reference_bulk_equity_ascii, wire))


def test_longest_equity_lines_fill_the_entire_batch():
    line = (
        f'["E",{-_I53},-1.7976931348623157e+308,-1.7976931348623157e+308]\n'
        .encode("ascii"))
    assert len(line) == 74
    raw = line * 128
    expected = _batch_outcome(_reference_bulk_equity, _reference_bulk_equity_ascii, raw)
    actual = _batch_outcome(run_json._bulk_equity, run_json._bulk_equity_ascii, raw)
    assert actual == expected and actual is not None and actual[1] == 128


@pytest.mark.parametrize("kind", (
    "empty", "canonical", "noncanonical", "above-batch-bound",
    "prefix", "middle", "suffix", "no-matches", "line-breaks",
))
def test_direct_equity_batches_preserve_original_substitution(kind):
    line = ordinary.encoded(_point(0))
    raw = {
        "empty": b"", "canonical": line * 37,
        "noncanonical": b'["E",1,1.00,0.0]\n',
        "above-batch-bound": line * 257, "prefix": b"x" + line,
        "middle": line + b"x\n" + line, "suffix": line + b"x",
        "no-matches": b"unmatched", "line-breaks": b"\r\nx\ry\n",
    }[kind]
    count = len(_REFERENCE_E_LINE.findall(raw))
    expected_batch = original._ReferenceEquity(raw, count, None, None)
    batch = run_json._BulkEquity(raw, count, None, None)
    assert (batch.raw, batch.count, batch.first, batch.last) == (raw, count, None, None)
    assert run_json._bulk_equity_ascii(batch) == _reference_bulk_equity_ascii(expected_batch)


@pytest.mark.parametrize("position", ("prefix", "middle", "suffix", "too-many"))
def test_equity_proof_does_not_skip_bytes_or_exceed_batch_bound(position):
    line = ordinary.encoded(_point(0))
    raw = {
        "prefix": b"x" + line,
        "middle": line + b"x" + line,
        "suffix": line + b"x",
        "too-many": line * 129,
    }[position]
    assert _reference_bulk_equity(raw) is None
    assert run_json._bulk_equity(raw) is None


def _equity_run_pair(pattern, raw):
    expected = _batch_outcome(
        lambda data: _reference_bulk_equity(data, pattern=pattern),
        _reference_bulk_equity_ascii, raw)
    actual = _batch_outcome(run_json._bulk_equity, run_json._bulk_equity_ascii, raw)
    assert actual == expected, "EQUITY_RUN_IDENTITY: proof differs from the run fullmatch"
    return actual


@pytest.mark.parametrize("repeat_text,lower,upper", (
    (b"{1,3}", 1, 3), (b"{2,37}", 2, 37),
    (b"{1,257}", 1, 257), (b"{1,1024}", 1, 1024),
    (b"{37}", 37, 37), (b"{1,257}?", 1, 257),
))
def test_equity_proof_follows_replaced_run_repeat(monkeypatch, repeat_text, lower, upper):
    pattern = re.compile(rb"(?:" + _REFERENCE_E_LINE.pattern + rb")" + repeat_text)
    monkeypatch.setattr(run_json, "_BULK_E_RUN", pattern)
    line = ordinary.encoded(_point(0))
    counts = {0, 1, lower - 1, lower, lower + 1, 127, 128, 129,
              upper - 1, upper, upper + 1}
    outcomes = Counter()
    for count in sorted(counts):
        raw = line * count
        result = _equity_run_pair(pattern, raw)
        outcomes["refused" if result is None else "accepted"] += 1
        assert (result is not None) == (bool(raw) and pattern.fullmatch(raw) is not None)
    valid = line * lower
    for raw in (b"x" + valid, valid + b"x", line + b"x" + valid, valid[:-1]):
        assert _equity_run_pair(pattern, raw) is None
    assert outcomes["accepted"] > 0 and outcomes["refused"] > 0


def _equity_run_guard_case(kind):
    line = b'["E",-1,1.0,0.0]\n'
    prefix = rb"(?:" + _REFERENCE_E_LINE.pattern + rb")"
    if kind == "prefix":
        narrowed = _REFERENCE_E_LINE.pattern.replace(b"(0|-?[1-9]", b"(0|[1-9]", 1)
        assert narrowed != _REFERENCE_E_LINE.pattern
        return re.compile(rb"(?:" + narrowed + rb"){1,128}"), line, 1
    assert kind == "closing-brace"
    return re.compile(prefix + b"{1,3x"), line * 2, 2


@pytest.mark.parametrize("kind", ("prefix", "closing-brace"))
def test_equity_proof_follows_replaced_run_shape(monkeypatch, kind):
    pattern, raw, count = _equity_run_guard_case(kind)
    monkeypatch.setattr(run_json, "_BULK_E_RUN", _REFERENCE_E_RUN)
    assert _equity_run_pair(_REFERENCE_E_RUN, raw)[1] == count
    assert pattern.fullmatch(raw) is None
    monkeypatch.setattr(run_json, "_BULK_E_RUN", pattern)
    assert _equity_run_pair(pattern, raw) is None


@pytest.mark.parametrize("kind", ("prefix", "closing-brace"))
def test_equity_run_identity_detects_a_missing_pattern_guard(monkeypatch, kind):
    pattern, raw, count = _equity_run_guard_case(kind)
    monkeypatch.setattr(run_json, "_BULK_E_RUN", _REFERENCE_E_RUN)
    assert _equity_run_pair(_REFERENCE_E_RUN, raw)[1] == count
    monkeypatch.setattr(run_json, "_BULK_E_RUN", pattern)
    assert pattern.fullmatch(raw) is None and _equity_run_pair(pattern, raw) is None
    prove = run_json._bulk_equity
    if "prefix == _BULK_E_RUN_PREFIX" in inspect.getsource(prove):
        guard = ("prefix == _BULK_E_RUN_PREFIX and " if kind == "prefix"
                 else ' and repeat_text.endswith(b"}")')
        changed = _mutate(prove, guard, "")
    else:
        changed = _mutate(prove, " or _BULK_E_RUN.fullmatch(raw) is None", "")
    monkeypatch.setattr(run_json, "_bulk_equity", changed)
    with pytest.raises(AssertionError, match="EQUITY_RUN_IDENTITY"):
        _equity_run_pair(pattern, raw)


@pytest.mark.parametrize("kind", ("rows", "bytes"))
def test_equity_repeat_identity_detects_a_fixed_cap(monkeypatch, kind):
    pattern = re.compile(rb"(?:" + _REFERENCE_E_LINE.pattern + rb"){1,1024}")
    monkeypatch.setattr(run_json, "_BULK_E_RUN", pattern)
    raw = b"".join(ordinary.encoded(_point(index)) for index in range(400))
    assert len(raw) > 128 * 80
    assert _equity_run_pair(pattern, raw)[1] == 400
    prove = run_json._bulk_equity

    def capped(data):
        if ((kind == "rows" and len(_REFERENCE_E_LINE.findall(data)) > 128)
                or (kind == "bytes" and len(data) > 128 * 80)):
            return None
        return prove(data)

    monkeypatch.setattr(run_json, "_bulk_equity", capped)
    with pytest.raises(AssertionError, match="EQUITY_RUN_IDENTITY"):
        _equity_run_pair(pattern, raw)


def _report_pair(sink, library, report, monkeypatch):
    raw = Path(sink.path).read_bytes()
    with monkeypatch.context() as patch:
        patch.setattr(run_json, "_bulk_equity", _reference_bulk_equity)
        patch.setattr(run_json, "_bulk_equity_ascii", _reference_bulk_equity_ascii)
        expected, reference_sha = original._assemble(
            sink, library, report, patch, reference=True)
    actual, actual_sha = original._assemble(sink, library, report, monkeypatch)
    support.assert_same(actual, expected)
    assert hashlib.sha256(actual.report).digest() == hashlib.sha256(expected.report).digest()
    assert Path(sink.path).read_bytes() == raw, "assembly must preserve every sink byte"
    if actual.failure is None:
        assert actual_sha == reference_sha == library.sha
    return actual


@pytest.mark.parametrize("count", (0, 1, 37, 257, 2201))
def test_complete_report_bytes_and_sink_digest_match_original(tmp_path, monkeypatch, count):
    rows = [_point(index) for index in range(count)]
    rows.extend(_trade(index + 1, pnl=float(index % 3 - 1)) for index in range(count))
    with original._sink_case(tmp_path, rows) as args:
        if count == 2201:
            assert sum(len(ordinary.encoded(row)) for row in rows if row[0] == "E") > 65536
        outcome = _report_pair(*args, monkeypatch)
        assert outcome.failure is None


@pytest.mark.parametrize("count", (1, 257, 2201))
def test_fused_equity_counter_observes_real_lookahead_and_encoding(tmp_path, monkeypatch, count):
    rows = [_point(index) for index in range(count)] + [_trade(1)]
    with original._sink_case(tmp_path, rows) as (sink, library, report):
        raw = Path(sink.path).read_bytes()
        control = _EquityWork(reference=True)
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_BULK_E_RUN", _REFERENCE_E_RUN)
            patch.setattr(run_json, "_bulk_equity", _reference_bulk_equity)
            patch.setattr(run_json, "_bulk_equity_ascii", _reference_bulk_equity_ascii)
            with control.install(patch):
                expected, reference_sha = original._assemble(
                    sink, library, report, patch, reference=True)
        assert expected.failure is None
        batches = control.traversals["findall"]
        assert (count + 127) // 128 <= batches <= count
        assert control.expansions == count
        # Each accepted E batch, then one refused lookahead at T and one at
        # the trailer. Header parsing precedes the bulk iterator.
        assert control.traversals == Counter(
            match=batches + 2, fullmatch=batches, findall=batches, sub=batches)
        work = _EquityWork()
        with work.install(monkeypatch):
            actual, actual_sha = original._assemble(sink, library, report, monkeypatch)
        assert actual.failure is None
        support.assert_same(actual, expected)
        assert hashlib.sha256(actual.report).digest() == hashlib.sha256(expected.report).digest()
        assert actual_sha == reference_sha == library.sha
        assert Path(sink.path).read_bytes() == raw
        work.assert_bounded(batches=batches, non_equity_attempts=2)


class _WriteChunks(io.StringIO):
    def __init__(self):
        super().__init__()
        self.chunks = []

    def write(self, text):
        self.chunks.append(text)
        return super().write(text)


@pytest.mark.parametrize("count", (1, 37, 128, 513))
def test_streamed_equity_keeps_write_boundaries_without_templates(tmp_path, monkeypatch, count):
    with original._sink_case(tmp_path, [_point(index) for index in range(count)]) as args:
        sink, library, report = args
        sink._fused_pass = False
        sink.verify(library, 1, report)
        value = run_json._JSONEquityArray(sink)
        expected, actual = _WriteChunks(), _WriteChunks()
        control = _EquityWork(reference=True)
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_BULK_E_RUN", _REFERENCE_E_RUN)
            patch.setattr(run_json, "_bulk_equity", _reference_bulk_equity)
            with control.install(patch):
                original._reference_stream_equity_array(value, expected)
        batches = control.traversals["findall"]
        lookaheads = control.traversals["match"] - batches
        assert batches >= (count + 127) // 128 and lookaheads >= 0
        assert control.expansions == count
        assert control.traversals == Counter(
            match=batches + lookaheads, fullmatch=batches, findall=batches, sub=batches)
        work = _EquityWork()
        with work.install(monkeypatch):
            run_json._stream_equity_array(value, actual)
        assert actual.chunks == expected.chunks
        assert actual.getvalue().encode("ascii") == expected.getvalue().encode("ascii")
        work.assert_bounded(batches=batches, non_equity_attempts=lookaheads)


@pytest.mark.parametrize("wire", _EQUITY_WIRES)
def test_noncanonical_equity_keeps_complete_output_or_refusal(tmp_path, monkeypatch, wire):
    original_line = ordinary.encoded(_point(0))
    with original._sink_case(tmp_path, [_point(0), _trade(1)]) as args:
        sink, library, report = args

        def replace(body):
            assert body.count(original_line) == 1
            return body.replace(original_line, wire, 1)

        original._rewrite_body(sink, library, replace)
        _report_pair(sink, library, report, monkeypatch)


def _mutate(function, old, new, *additional):
    """Remove or restore one operation in executable test-only source."""
    source = textwrap.dedent(inspect.getsource(function))
    for before, after in ((old, new), *additional):
        assert source.count(before) == 1, "the mutation must identify exactly one real check"
        source = source.replace(before, after)
    namespace = dict(function.__globals__)
    exec(compile(source, "<validation-mutation>", "exec"), namespace)
    return namespace[function.__name__]


@pytest.mark.parametrize("proof", ("repr", "timestamp"))
def test_equity_identity_detects_a_missing_numeric_proof(tmp_path, monkeypatch, proof):
    if proof == "repr":
        wire = b'["E",1,10000.00,0.0]\n'
        changed = _mutate(
            run_json._bulk_equity, " or list(map(repr, numbers)) != texts", "")
    else:
        wire = f'["E",{_I53 + 1},10000.0,0.0]\n'.encode("ascii")
        changed = _mutate(
            run_json._bulk_equity,
            "if min(timestamps) < -((1 << 53) - 1) or max(timestamps) > (1 << 53) - 1:",
            "if False:")
    with original._sink_case(tmp_path, [_point(0), _trade(1)]) as args:
        sink, library, report = args
        original._rewrite_body(sink, library, lambda body: body.replace(
            ordinary.encoded(_point(0)), wire, 1))
        # The intact candidate must first agree on the same corrupt sink.
        _report_pair(sink, library, report, monkeypatch)
        monkeypatch.setattr(run_json, "_bulk_equity", changed)
        with pytest.raises(AssertionError):
            _report_pair(sink, library, report, monkeypatch)


class _RecordWork:
    """Count dispatch calls, all descendant calls, and every parser LINE event.

    Resume events for Python generators count as calls. The dispatch count
    excludes JSON and token constructors; the total count includes them.
    Token hooks are also observed separately and must still run.
    """
    def __init__(self, parse):
        self.root = parse.__code__
        self.lexical_codes = {pf_sink._parse.__code__, original._reference_parse.__code__}
        self.int_codes = {pf_sink._int_token.__code__, original._reference_int_token.__code__}
        self.float_codes = {pf_sink._float_token.__code__, original._reference_float_token.__code__}
        self.hooks = self.int_codes | self.float_codes | {
            pf_sink._pairs.__code__, pf_sink._constant.__code__,
            original._reference_pairs.__code__, original._reference_constant.__code__}
        self.excluded = self.hooks | {function.__code__ for function in (
            json.loads, json.JSONDecoder.__init__, json.JSONDecoder.decode,
            json.JSONDecoder.raw_decode, pf_sink.Record.__new__,
            pf_sink._FloatToken.__new__, original._ReferenceFloatToken.__new__)}
        self.active = set()
        self.python_calls = self.record_lines = self.lexical_lines = self.rows = 0
        self.validation_lines = 0
        self.total_calls = 0
        self.int_tokens = self.float_tokens = 0

    def _trace(self, frame, event, arg):
        code = frame.f_code
        if event == "call":
            if code is self.root or (frame.f_back is not None and id(frame.f_back) in self.active):
                self.active.add(id(frame))
                self.total_calls += 1
                if code is self.root:
                    self.rows += 1
                if code in self.int_codes:
                    self.int_tokens += 1
                elif code in self.float_codes:
                    self.float_tokens += 1
                if code not in self.excluded:
                    self.python_calls += 1
        if id(frame) not in self.active:
            return None
        if event == "line":
            self.record_lines += code is self.root
            self.lexical_lines += code in self.lexical_codes
            self.validation_lines += code not in self.excluded
        elif event == "return":
            self.active.remove(id(frame))
        return self._trace

    @contextmanager
    def install(self):
        previous = sys.gettrace()
        sys.settrace(self._trace)
        try:
            yield self
        finally:
            sys.settrace(previous)

    def assert_bounded(self, count, *, trades=False):
        assert self.rows == count and not self.active
        assert self.python_calls <= 3 * count, (
            f"RECORD_FIELD_WORK: {self.python_calls} parser/validator Python calls; "
            f"budget is three per record ({count} records), independent of field count")
        assert self.record_lines <= 16 * count, (
            f"RECORD_FIELD_WORK: {self.record_lines} record-parser LINE events; budget is 16 per record")
        assert self.lexical_lines <= 16 * count, (
            f"RECORD_FIELD_WORK: {self.lexical_lines} lexical-parser LINE events; budget is 16 per record")
        if trades:
            # At most eight success-path lines per field, three loop setup/
            # exit lines, and the two parser budgets: 20*8 + 3 + 2*16.
            assert self.validation_lines <= 195 * count, (
                f"RECORD_FIELD_WORK: {self.validation_lines} parser/validator LINE events; "
                f"budget is 195 per trade ({count} trades), independent of string length")
            # Three parser/validator frames, seven int hooks, eight float
            # hooks and token constructors, one Record constructor, and
            # loads/JSONDecoder.__init__/decode/raw_decode: 31 calls in all.
            # Four frames of headroom allow JSON implementation changes; the
            # trade comparison also requires at least 40 fewer reference calls.
            assert self.total_calls <= (31 + 4) * count, (
                f"RECORD_FIELD_WORK: {self.total_calls} total Python calls including JSON "
                f"and constructors; budget is 31 + 4 per trade ({count} trades)")


def _record_result(parse, parser, line):
    try:
        record = parse(parser, line)
    except pf_sink.SinkFormatError as error:
        return "error", type(error).__qualname__, str(error), error.args
    encoded = json.dumps(
        (record.tag, record.fields, record.values), ensure_ascii=True,
        separators=(",", ":"), allow_nan=False).encode("ascii")
    return "ok", encoded


def _record_pair(parser, line):
    expected = _record_result(original._reference_record_parse, parser, line)
    actual = _record_result(pf_sink.RecordParser.parse, parser, line)
    assert actual == expected
    return actual


@pytest.mark.parametrize("count", (1, 37, 257))
def test_trade_validation_has_constant_python_call_count(count):
    parser = pf_sink.RecordParser(ordinary.header())
    lines = [ordinary.encoded(_trade(
        index + 1, entry_id="漢é\x7f", exit_id='q"\\', exit_comment="identifier" * 17))
             for index in range(count)]
    control = _RecordWork(original._reference_record_parse)
    with control.install():
        expected = [original._reference_record_parse(parser, line) for line in lines]
    # Root + lexical parser, 21 generator resumptions, 20 _field calls and
    # four _string calls already require 47 calls, before character walks.
    assert control.rows == count and control.python_calls >= 47 * count
    assert control.total_calls > 31 * count
    assert control.validation_lines > 195 * count
    assert control.lexical_lines > 16 * count
    assert (control.int_tokens, control.float_tokens) == (7 * count, 8 * count)
    work = _RecordWork(pf_sink.RecordParser.parse)
    with work.install():
        actual = [parser.parse(line) for line in lines]
    assert actual == expected
    assert (work.int_tokens, work.float_tokens) == (7 * count, 8 * count)
    work.assert_bounded(count, trades=True)
    assert control.total_calls - work.total_calls >= 40 * count, (
        "RECORD_FIELD_WORK: at least 40 Python calls per trade must be removed")


@pytest.mark.parametrize("width", (0, 2, 37, 257))
def test_schema_plan_call_budget_is_independent_of_column_count(width):
    header = ordinary.header(outputs=True)
    descriptor = next(item for item in header["records"] if item["type"] == "O")
    descriptor["fields"] = descriptor["fields"][:3] + [f"v{index}" for index in range(width)]
    descriptor["types"] = descriptor["types"][:3] + ["f64?"] * width
    descriptor["units"] = descriptor["units"][:3] + [""] * width
    parser = pf_sink.RecordParser(header)
    line = ordinary.encoded(["O", 0, ordinary.bars.T0, None, *([0.0] * width)])
    control = _RecordWork(original._reference_record_parse)
    with control.install():
        expected = original._reference_record_parse(parser, line)
    assert control.python_calls >= 2 * (3 + width) + 3
    work = _RecordWork(pf_sink.RecordParser.parse)
    with work.install():
        actual = parser.parse(line)
    assert actual == expected
    work.assert_bounded(1)


def _generic_record_parse(self, line, *, parsed=pf_sink._UNPARSED):
    value = pf_sink._parse(line) if parsed is pf_sink._UNPARSED else parsed
    if type(value) is not list or not value or type(value[0]) is not str:
        raise pf_sink.SinkFormatError("record must be a nonempty array with a string tag")
    tag = value[0]
    if tag not in self._descriptors:
        raise pf_sink.SinkFormatError("record tag is absent from the header")
    descriptor = self._descriptors[tag]
    if len(value) != len(descriptor.fields) + 1:
        raise pf_sink.SinkFormatError("record field count differs from its descriptor")
    values = tuple(pf_sink._field(item, kind) for item, kind in zip(value[1:], descriptor.types))
    if tag == "T" and values[1] not in ("long", "short"):
        raise pf_sink.SinkFormatError("trade side must be long or short")
    return pf_sink.Record(tag, descriptor.fields, values)


@pytest.mark.parametrize("count", (1, 37))
def test_trade_counter_detects_reintroduced_descriptor_dispatch(monkeypatch, count):
    parser = pf_sink.RecordParser(ordinary.header())
    lines = [ordinary.encoded(_trade(index + 1)) for index in range(count)]
    control = _RecordWork(original._reference_record_parse)
    with control.install():
        expected = [original._reference_record_parse(parser, line) for line in lines]
    assert control.python_calls >= 47 * count
    monkeypatch.setattr(pf_sink.RecordParser, "parse", _generic_record_parse)
    work = _RecordWork(pf_sink.RecordParser.parse)
    with work.install():
        assert [parser.parse(line) for line in lines] == expected
    assert work.python_calls >= 47 * count
    with pytest.raises(AssertionError, match="RECORD_FIELD_WORK"):
        work.assert_bounded(count, trades=True)


_FIELD_VALUES = (None, True, False, -1, 0, 1, _I53, _I53 + 1,
                 0.0, -0.0, 1e-7, "", "long", "bad", "\ud800", "漢é",
                 [], {}, "0123456789abcdef", *original._UNICODE_BOUNDARIES)


def _all_schema_header():
    header = ordinary.header(outputs=True)
    templates = {descriptor["type"]: descriptor for descriptor in header["records"]}
    records = []
    for tag in pf_sink._ORDER:
        if tag == "O":
            count = len(templates[tag]["fields"]) - len(pf_sink._O)
            columns = pf_sink._O + tuple((f"v{i}", "f64?", "") for i in range(count))
        elif tag == "K":
            columns = tuple((f"c{i}", "f64?", "")
                            for i in range(len(templates[tag]["fields"])))
        else:
            columns = pf_sink._FIXED[tag]
        records.append(ordinary.descriptor(
            tag, *([column[index] for column in columns] for index in range(3))))
    header["records"] = records
    return header


_SCHEMA_ROWS = (
    _point(0), _trade(1),
    ["O", 0, ordinary.bars.T0, None, 0.0, None],
    _event("text"), ["K", 0.0],
    ["X", ordinary.bars.T0, 0, "text", 0.0], ["H", "0123456789abcdef"],
)


@pytest.mark.parametrize("row", _SCHEMA_ROWS, ids=lambda row: row[0])
def test_every_schema_field_keeps_original_validation_and_order(row):
    parser = pf_sink.RecordParser(_all_schema_header())
    assert _record_pair(parser, ordinary.encoded(row))[0] == "ok"
    for index in range(1, len(row)):
        outcomes = Counter()
        for value in _FIELD_VALUES:
            changed = list(row)
            changed[index] = value
            # ASCII JSON preserves deliberate lone-surrogate escapes.
            line = json.dumps(changed, ensure_ascii=True, separators=(",", ":")).encode("ascii") + b"\n"
            outcomes[_record_pair(parser, line)[0]] += 1
        assert sum(outcomes.values()) == len(_FIELD_VALUES) == 19 + 8
        assert outcomes["ok"] > 0 and outcomes["error"] > 0, (row[0], index, outcomes)
        # With multiple bad fields, the first descriptor rejection still wins.
        changed = list(row)
        changed[index:] = [{}] * (len(row) - index)
        assert _record_pair(parser, ordinary.encoded(changed))[0] == "error"


@pytest.mark.parametrize("first,second", (
    (1, 2), (2, 5), (5, 15), (15, 16), (16, 18), (18, 19),
))
def test_trade_field_refusal_precedes_later_field_or_side_refusal(first, second):
    parser = pf_sink.RecordParser(ordinary.header())
    row = _trade(1)
    row[first], row[second] = None, {}
    _record_pair(parser, ordinary.encoded(row))
    row = _trade(1, side="sideways")
    row[second] = {}
    _record_pair(parser, ordinary.encoded(row))


@pytest.mark.parametrize("suffix", ("", "\\", " space"))
def test_trade_identity_detects_a_missing_surrogate_check(monkeypatch, suffix):
    parser = pf_sink.RecordParser(ordinary.header())
    line = json.dumps(_trade(1, entry_id="\ud800" + suffix), ensure_ascii=True,
                      separators=(",", ":")).encode("ascii") + b"\n"
    assert _record_pair(parser, line)[0] == "error"
    validate_string = pf_sink._string

    def unchecked_string(value):
        return value if type(value) is str else validate_string(value)

    monkeypatch.setattr(pf_sink, "_string", unchecked_string)
    monkeypatch.setattr(pf_sink, "_SURROGATE", re.compile(r"(?!)"), raising=False)
    with pytest.raises(AssertionError):
        _record_pair(parser, line)


def _string_result(validate, value):
    try:
        text = validate(value)
    except pf_sink.SinkFormatError as error:
        return "error", type(error).__qualname__, str(error), error.args
    return "ok", json.dumps(text, ensure_ascii=True).encode("ascii")


def _string_pair(value):
    expected = _string_result(original._reference_string, value)
    actual = _string_result(pf_sink._string, value)
    assert actual == expected, "STRING_IDENTITY: string validation differs from the original"
    return actual


@pytest.mark.parametrize("field", ("run_id", "engine", "type", "sha256"))
@pytest.mark.parametrize("value", (
    None, True, 0, "", "plain", "\ud800", *original._UNICODE_BOUNDARIES,
))
def test_header_and_trailer_strings_match_original_directly(field, value):
    # Shape valid text like each real call site, retaining non-string inputs.
    prefix = {"run_id": "run", "engine": "engine", "type": "E", "sha256": "a" * 64}[field]
    _string_pair(prefix + value if type(value) is str else value)


@pytest.mark.parametrize("pattern,text", (
    (r"[\ud800-\udbff]", "\udc00"),
    (r"[\ud800-\uffff]", "\ue000"),
), ids=("high-surrogates-only", "range-includes-private-use"))
def test_string_and_record_identity_detect_surrogate_range_changes(monkeypatch, pattern, text):
    parser = pf_sink.RecordParser(ordinary.header())
    line = json.dumps(_trade(1, entry_id=text), ensure_ascii=True,
                      separators=(",", ":")).encode("ascii") + b"\n"
    _string_pair("engine" + text)
    _record_pair(parser, line)
    changed_range = re.compile(pattern)

    def changed_string(value):
        if type(value) is not str or changed_range.search(value) is not None:
            raise pf_sink.SinkFormatError("expected a Unicode string without unpaired surrogates")
        return value

    monkeypatch.setattr(pf_sink, "_string", changed_string)
    monkeypatch.setattr(pf_sink, "_SURROGATE", changed_range, raising=False)
    with pytest.raises(AssertionError, match="STRING_IDENTITY"):
        _string_pair("engine" + text)
    with pytest.raises(AssertionError):
        _record_pair(parser, line)


@pytest.mark.parametrize("kind", ("surrogate", "backslash", "space", "bom", "too-long", "truncated"))
def test_trade_report_keeps_original_bytes_and_error_order(tmp_path, monkeypatch, kind):
    row = _trade(1, entry_id="marker", exit_id='q"\\')
    original_line = ordinary.encoded(row)
    if kind == "surrogate":
        wire = original_line.replace(b"marker", b"\\ud800")
    elif kind == "backslash":
        wire = original_line.replace(b"marker", b"escaped\\\\value")
    elif kind == "space":
        wire = original_line.replace(b'["T",', b'["T", ')
    elif kind == "bom":
        wire = b"\xef\xbb\xbf " + original_line
    elif kind == "too-long":
        wire = b" " + b"[" * pf_sink.MAX_LINE_BYTES + b"\n"
    else:
        wire = original_line[:-5] + b"\n"
    with original._sink_case(tmp_path, [_point(0), row]) as args:
        sink, library, report = args
        original._rewrite_body(sink, library, lambda body: body.replace(original_line, wire, 1))
        outcome = _report_pair(sink, library, report, monkeypatch)
        assert (outcome.failure is None) == (kind == "backslash")


@pytest.mark.parametrize("fault", ("trailer", "digest", "truncated"))
def test_deferred_trade_failure_keeps_sink_refusal_first(tmp_path, monkeypatch, fault):
    with original._sink_case(tmp_path, [_point(0), _trade(1, pnl=None)]) as args:
        sink, library, report = args
        raw = Path(sink.path).read_bytes()
        start = raw.rfind(b'\n{"trailer":') + 1
        assert start > 0
        if fault == "trailer":
            trailer = json.loads(raw[start:])
            trailer["counts"]["T"] += 1
            raw = raw[:start] + ordinary.encoded(trailer)
        elif fault == "digest":
            trailer = json.loads(raw[start:])
            trailer["sha256"] = "0" * 64
            raw = raw[:start] + ordinary.encoded(trailer)
        else:
            raw = raw[:start]
        Path(sink.path).write_bytes(raw)
        outcome = _report_pair(sink, library, report, monkeypatch)
        assert outcome.failure is not None and outcome.error.code == "engine_invariant"


_REFERENCE_BAR_HEADER = b"timestamp,open,high,low,close,volume\n"
_REFERENCE_BAR_FLOAT = (
    rb"[+-]?(?:(?:[0-9]+\.[0-9]*|\.[0-9]+)"
    rb"(?:[eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)"
)
_REFERENCE_BAR_LINE = re.compile(
    rb"([0-9]{13}),(" + _REFERENCE_BAR_FLOAT + rb"),(" + _REFERENCE_BAR_FLOAT
    + rb"),(" + _REFERENCE_BAR_FLOAT + rb"),(" + _REFERENCE_BAR_FLOAT + rb"),("
    + _REFERENCE_BAR_FLOAT + rb")\n")
_REFERENCE_BAR_SLAB_ROWS = 4096
_REFERENCE_FEED_RECORD = struct.Struct("<5dq")
_REFERENCE_FEED_PREFIX = b"pineforge:ohlcv:barc-le:v1\0"


def _reference_bar_layout():
    return (
        sys.byteorder == "little"
        and ctypes.sizeof(run_json.BarC) == _REFERENCE_FEED_RECORD.size
        and [getattr(run_json.BarC, name).offset for name in (
            "open", "high", "low", "close", "volume", "timestamp"
        )] == [0, 8, 16, 24, 32, 40]
    )


def _reference_feed_hasher():
    h = hashlib.sha256()
    h.update(_REFERENCE_FEED_PREFIX)
    return h


def _reference_update_feed_hash(h, row):
    h.update(_REFERENCE_FEED_RECORD.pack(*row))


def _reference_pack_bar_slab(matches):
    columns = tuple(zip(*matches))
    timestamps = list(map(int, columns[0]))
    float_columns = [list(map(float, values)) for values in columns[1:]]
    for raw_values, values in zip(columns[1:], float_columns):
        for raw, value in zip(raw_values, values):
            text = raw.decode("ascii")
            if (not math.isfinite(value)
                    or (value == 0.0 and text.startswith("-"))
                    or repr(value) != text):
                return None
    packed = bytearray(_REFERENCE_FEED_RECORD.size * len(timestamps))
    struct.pack_into(
        "<" + "5dq" * len(timestamps),
        packed,
        0,
        *chain.from_iterable(zip(*float_columns, timestamps)),
    )
    return packed


def _reference_load_bars_bulk(raw):
    if not _reference_bar_layout() or not raw.startswith(_REFERENCE_BAR_HEADER):
        return None
    body = raw[len(_REFERENCE_BAR_HEADER):]
    packed = bytearray()
    hasher = _reference_feed_hasher()
    count = 0
    batch = []
    cursor = 0
    for match in _REFERENCE_BAR_LINE.finditer(body):
        if match.start() != cursor:
            return None
        batch.append(match.groups())
        cursor = match.end()
        if len(batch) == _REFERENCE_BAR_SLAB_ROWS:
            slab = _reference_pack_bar_slab(batch)
            if slab is None:
                return None
            packed.extend(slab)
            hasher.update(slab)
            count += len(batch)
            batch.clear()
    if cursor != len(body):
        return None
    if batch:
        slab = _reference_pack_bar_slab(batch)
        if slab is None:
            return None
        packed.extend(slab)
        hasher.update(slab)
        count += len(batch)
    bars = (run_json.BarC * count)()
    if packed:
        source = (ctypes.c_char * len(packed)).from_buffer(packed)
        ctypes.memmove(ctypes.addressof(bars), ctypes.addressof(source), len(packed))
    return bars, count, hasher.hexdigest()


def _reference_load_bars_legacy(csv_path, raw=None):
    rows = []
    feed_hasher = _reference_feed_hasher()
    source = None
    try:
        source = (
            io.TextIOWrapper(io.BytesIO(raw), newline="", encoding="utf-8")
            if raw is not None else csv_path.open(newline="", encoding="utf-8")
        )
        with source as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    parsed = (
                        float(row["open"]),
                        float(row["high"]),
                        float(row["low"]),
                        float(row["close"]),
                        float(row["volume"]),
                        int(row["timestamp"]),
                    )
                except KeyError as e:
                    raise run_json._bars_unreadable(
                        f"--ohlcv: {csv_path}: no column {e.args[0]}", "columns") from None
                except (TypeError, ValueError):
                    raise run_json._bars_unreadable(
                        f"--ohlcv: {csv_path} line {reader.line_num}: not a number",
                        "value") from None
                _reference_update_feed_hash(feed_hasher, parsed)
                rows.append(parsed)
    except OSError as e:
        raise run_json._bars_unreadable(f"--ohlcv: {csv_path}: {e.strerror or e}", "io") from None
    except (UnicodeDecodeError, csv.Error) as e:
        raise run_json._bars_unreadable(
            f"--ohlcv: {csv_path}: not a UTF-8 CSV ({e})", "value") from None
    n = len(rows)
    bars = (run_json.BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open      = o
        bars[i].high      = h
        bars[i].low       = l
        bars[i].close     = c
        bars[i].volume    = v
        bars[i].timestamp = ts
    return bars, n, feed_hasher.hexdigest()


def _reference_load_bars(csv_path):
    raw = run_json._read_bar_bytes(csv_path)
    bulk = _reference_load_bars_bulk(raw)
    if bulk is not None:
        return bulk
    return _reference_load_bars_legacy(csv_path, raw)


class _BarsWork:
    """Observe successful DictReader conversions and actual bulk parser frames."""
    def __init__(self, *, reference=False):
        bulk = _reference_load_bars_bulk if reference else run_json._load_bars_bulk
        legacy = _reference_load_bars_legacy if reference else run_json._load_bars_legacy
        pack = _reference_pack_bar_slab if reference else run_json._pack_bar_bulk_slab
        update = _reference_update_feed_hash if reference else run_json._update_source_feed_hash
        self.bulk, self.legacy, self.pack, self.update = (
            function.__code__ for function in (bulk, legacy, pack, update))
        self.pattern = _REFERENCE_BAR_LINE if reference else run_json._BAR_CANONICAL_LINE
        source, start = inspect.getsourcelines(bulk)
        self.loop_lines = {start + offset for offset, line in enumerate(source)
                           if line.strip().startswith("for match in ")}
        assert len(self.loop_lines) == 1, "observe the actual bulk row iterator"
        self.converted = self.iterations = self.bulk_lines = self.slabs = self.regex_calls = 0
        self.packed_rows = 0

    def _trace(self, frame, event, arg):
        code = frame.f_code
        if event == "call":
            if code is self.update and frame.f_back.f_code is self.legacy:
                self.converted += 1
            elif code is self.pack or code is _reference_pack_bar_slab.__code__:
                self.slabs += 1
                frame.f_trace_lines = False
        elif event == "line" and code is self.bulk:
            self.bulk_lines += 1
            self.iterations += frame.f_lineno in self.loop_lines
        elif event == "return" and (code is self.pack or code is _reference_pack_bar_slab.__code__):
            if arg is not None:
                assert len(arg) % _REFERENCE_FEED_RECORD.size == 0
                self.packed_rows += len(arg) // _REFERENCE_FEED_RECORD.size
        return self._trace if code in (
            self.bulk, self.pack, _reference_pack_bar_slab.__code__) else None

    def _profile(self, frame, event, arg):
        owner = getattr(arg, "__self__", None)
        if (event == "c_call" and (owner is self.pattern or owner is _REFERENCE_BAR_LINE)
                and getattr(arg, "__name__", "") in (
                    "finditer", "findall", "match", "fullmatch", "search", "sub")):
            self.regex_calls += 1

    @contextmanager
    def install(self):
        previous_trace, previous_profile = sys.gettrace(), sys.getprofile()
        sys.settrace(self._trace)
        sys.setprofile(self._profile)
        try:
            yield self
        finally:
            sys.setprofile(previous_profile)
            sys.settrace(previous_trace)

    def assert_no_reparse(self):
        assert self.converted == 0, (
            f"BARS_DICTREADER_WORK: {self.converted} rows converted by DictReader; budget is zero")

    def assert_early_decline(self):
        assert self.iterations <= _REFERENCE_BAR_SLAB_ROWS, (
            f"BARS_DECLINE_WORK: {self.iterations} bulk iterator LINE events; "
            "budget is at most one slab before declining")
        assert self.bulk_lines <= 16 and self.regex_calls == self.slabs == 0, (
            f"BARS_DECLINE_WORK: {self.bulk_lines} bulk LINE events, "
            f"{self.regex_calls} regex scans and {self.slabs} packed slabs; "
            "a missing final LF must decline before scanning the body")

    def assert_late_decline(self, count):
        assert self.converted == count, (
            f"BARS_DICTREADER_WORK: {self.converted} fallback conversions; expected exactly {count}")
        assert self.packed_rows <= _REFERENCE_BAR_SLAB_ROWS and self.slabs <= 2, (
            f"BARS_DECLINE_WORK: {self.packed_rows} completed packed rows and {self.slabs} attempts; "
            "budget is one completed slab plus at most one refused slab")
        assert self.iterations <= 2 * _REFERENCE_BAR_SLAB_ROWS + 16 and self.regex_calls == 1, (
            f"BARS_DECLINE_WORK: {self.iterations} iterator LINE events and {self.regex_calls} scans; "
            "a refusal in the second slab must stop within that slab")


def _bar_tape(count, *, integer_volume=False, fixed_prices=False):
    rows = []
    for index in range(count):
        timestamp = 1704067200000 + index * 60000
        volume = str(1000 + index % 97) if integer_volume else repr(1000.0 + index % 97)
        prices = (100.0 + index / 1000, 102.0 + index / 1000,
                  99.0 + index / 1000, 101.0 + index / 1000)
        fields = tuple(format(value, ".2f") if fixed_prices else repr(value)
                       for value in prices) + (volume,)
        rows.append((str(timestamp) + "," + ",".join(fields)).encode("ascii") + b"\n")
    return _REFERENCE_BAR_HEADER + b"".join(rows)


def _bars_result(load, path):
    try:
        bars, count, digest = load(path)
    except Exception as error:
        return ("error", type(error).__qualname__, str(error), error.args,
                getattr(error, "code", None), getattr(error, "code_args", None),
                getattr(error, "exit_status", 1))
    raw = ctypes.string_at(ctypes.addressof(bars), ctypes.sizeof(bars))
    return "ok", raw, count, digest


def _bars_pair(path):
    expected = _bars_result(_reference_load_bars, path)
    actual = _bars_result(run_json.load_bars, path)
    assert actual == expected, "BARS_OUTPUT_IDENTITY: array bytes, count, hash or refusal changed"
    return actual


@pytest.mark.parametrize("count", (1, 37, 4097, 8193, 20000))
@pytest.mark.parametrize("fixed_prices", (False, True), ids=("shortest-prices", "fixed-prices"))
def test_integer_volume_tape_never_reparses_through_dictreader(tmp_path, count, fixed_prices):
    path = tmp_path / "bars.csv"
    path.write_bytes(_bar_tape(count, integer_volume=True, fixed_prices=fixed_prices))
    control = _BarsWork(reference=True)
    with control.install():
        expected = _bars_result(_reference_load_bars, path)
    assert control.converted == count, "the original must convert every input row through DictReader"
    work = _BarsWork()
    with work.install():
        actual = _bars_result(run_json.load_bars, path)
    assert actual == expected and actual[0] == "ok" and actual[2] == count
    work.assert_no_reparse()
    assert work.slabs == (count + 4095) // 4096
    assert work.packed_rows == count


def _replace_bar_cell(raw, row, column, value):
    lines = raw.splitlines(keepends=True)
    assert lines[0] == _REFERENCE_BAR_HEADER and lines[row + 1].endswith(b"\n")
    cells = lines[row + 1][:-1].split(b",")
    assert len(cells) == 6 and 0 <= column < 5
    cells[column + 1] = value.encode("ascii")
    lines[row + 1] = b",".join(cells) + b"\n"
    return b"".join(lines)


def _spelled_bar_tape(kind, count=37):
    raw = _bar_tape(count)
    if kind == "one-integer":
        return _replace_bar_cell(raw, count // 2, 4, "8627")
    rows = []
    for index in range(count):
        if kind == "fixed-decimals":
            cells = ("100.00", "3.50", "0.10", "99.50", "8627")
        elif kind == "one-to-eight-decimals":
            cells = tuple(format(100.0 + (index + column) / 100,
                                 f".{1 + (index + column) % 8}f")
                          for column in range(4)) + (str(504 + index),)
        elif kind == "exponents":
            cells = ("1e2", "1.00E+02", "+3.50e0", "1E-1", "8627e0")
        elif kind == "seventeen-digits":
            cells = ("1.2345678901234567", "1.7976931348623157e308",
                     "2.2250738585072014e-308", "-1.2345678901234567", "504")
        elif kind == "large-integers":
            cells = ("9007199254740993", "18014398509481985", "-9007199254740993",
                     "10000000000000001", "9007199254740995")
        elif kind == "leading-zeros":
            cells = ("007.5", "00099.50", "+0003.50", ".10", "0008627")
        else:
            raise AssertionError(kind)
        rows.append((str(1704067200000 + index * 60000) + "," + ",".join(cells)
                     + "\n").encode("ascii"))
    return _REFERENCE_BAR_HEADER + b"".join(rows)


def _assert_bar_spelling_admission(raw, *, packed=True, reference_packed=False):
    assert run_json._load_bars_bulk(_bar_tape(1)) is not None
    assert (_reference_load_bars_bulk(raw) is not None) == reference_packed
    if (run_json._BAR_FLOAT == _REFERENCE_BAR_FLOAT
            and "or repr(value) != text" in inspect.getsource(run_json._pack_bar_bulk_slab)):
        packed = reference_packed
    if packed:
        assert run_json._load_bars_bulk(raw) is not None, (
            "BARS_SPELLING_ADMISSION: an admitted spelling must use the packed path")
    else:
        assert run_json._load_bars_bulk(raw) is None, (
            "BARS_SPELLING_ADMISSION: a refused spelling must use DictReader")


@pytest.mark.parametrize("kind", (
    "fixed-decimals", "one-to-eight-decimals", "exponents", "seventeen-digits",
    "large-integers", "leading-zeros", "one-integer",
))
def test_numeric_bar_spellings_match_dictreader_bytes_and_feed_hash(tmp_path, kind):
    path = tmp_path / "bars.csv"
    raw = _spelled_bar_tape(kind)
    path.write_bytes(raw)
    _assert_bar_spelling_admission(raw)
    expected = _bars_result(_reference_load_bars_legacy, path)
    assert expected[0] == "ok" and expected[2] == 37
    assert _bars_pair(path) == expected


@pytest.mark.parametrize("column", (0, 4), ids=("price", "volume"))
@pytest.mark.parametrize("size", (24, 25, 131073))
def test_numeric_bar_length_envelope_keeps_dictreader_result(tmp_path, column, size):
    path = tmp_path / "bars.csv"
    raw = _replace_bar_cell(_bar_tape(1), 0, column, "0" * (size - 1) + "1")
    path.write_bytes(raw)
    expected = _bars_result(_reference_load_bars_legacy, path)
    if size > 24:
        assert run_json._load_bars_bulk(raw) is None
    else:
        _assert_bar_spelling_admission(raw)
    assert expected[0] == ("error" if size > csv.field_size_limit() else "ok")
    assert _bars_pair(path) == expected


@pytest.mark.parametrize("column", (0, 4), ids=("price", "volume"))
@pytest.mark.parametrize("cell,packed,reference_packed", (
    ("5.", True, False), ("5e-324", True, True),
    ("1e-400", True, False), ("-1e-400", False, False),
))
def test_numeric_bar_edge_spellings_pin_admission_and_dictreader_bytes(
        tmp_path, column, cell, packed, reference_packed):
    path = tmp_path / "bars.csv"
    raw = _replace_bar_cell(_bar_tape(37), 36, column, cell)
    assert run_json._BAR_CANONICAL_LINE.fullmatch(raw.splitlines(keepends=True)[-1]) is not None
    path.write_bytes(raw)
    _assert_bar_spelling_admission(raw, packed=packed, reference_packed=reference_packed)
    expected = _bars_result(_reference_load_bars_legacy, path)
    assert expected[0] == "ok" and expected[2] == 37
    assert _bars_pair(path) == expected


@pytest.mark.parametrize("column", (0, 4), ids=("price", "volume"))
def test_bar_identity_detects_a_missing_length_envelope(tmp_path, monkeypatch, column):
    path = tmp_path / "bars.csv"
    raw = _replace_bar_cell(_bar_tape(1), 0, column, "0" * 131072 + "1.0")
    path.write_bytes(raw)
    assert _bars_pair(path)[0] == "error"
    pack = run_json._pack_bar_bulk_slab
    if "len(raw) > 24 or " in inspect.getsource(pack):
        changed = _mutate(pack, "len(raw) > 24 or ", "")
    else:
        # The original repr test supplied the same length envelope.
        changed = _mutate(pack, "\n                    or repr(value) != text", "")
    monkeypatch.setattr(run_json, "_pack_bar_bulk_slab", changed)
    with pytest.raises(AssertionError, match="BARS_OUTPUT_IDENTITY"):
        _bars_pair(path)


def test_bar_counter_detects_a_restored_repr_check(tmp_path, monkeypatch):
    path = tmp_path / "bars.csv"
    count = 4097
    path.write_bytes(_bar_tape(count, fixed_prices=True))
    assert _bars_pair(path)[0] == "ok"
    control = _BarsWork(reference=True)
    with control.install():
        expected = _bars_result(_reference_load_bars, path)
    assert control.converted == count
    pack = run_json._pack_bar_bulk_slab

    def repr_checked(matches):
        for row in matches:
            for raw in row[1:]:
                if repr(float(raw)).encode("ascii") != raw:
                    return None
        return pack(matches)

    monkeypatch.setattr(run_json, "_pack_bar_bulk_slab", repr_checked)
    work = _BarsWork()
    with work.install():
        actual = _bars_result(run_json.load_bars, path)
    assert actual == expected and work.slabs == 1 and work.converted == count
    with pytest.raises(AssertionError, match="BARS_DICTREADER_WORK"):
        work.assert_no_reparse()


@pytest.mark.parametrize("value", ("nan", "inf", "-0.0", "1e999"))
def test_late_refused_bar_cell_bounds_packing_and_replays_csv_once(tmp_path, value):
    path = tmp_path / "bars.csv"
    count = 3 * _REFERENCE_BAR_SLAB_ROWS + 1
    raw = _replace_bar_cell(
        _bar_tape(count), _REFERENCE_BAR_SLAB_ROWS + 37, 4, value)
    path.write_bytes(raw)
    control = _BarsWork(reference=True)
    with control.install():
        expected = _bars_result(_reference_load_bars_legacy, path)
    assert control.converted == count and expected[0] == "ok"
    work = _BarsWork()
    with work.install():
        actual = _bars_result(run_json.load_bars, path)
    assert actual == expected and actual[2] == count
    work.assert_late_decline(count)
    assert work.packed_rows == _REFERENCE_BAR_SLAB_ROWS


@pytest.mark.parametrize("kind", ("repeated-packing", "continued-scan"))
def test_late_bar_counter_detects_repeated_packing(tmp_path, monkeypatch, kind):
    path = tmp_path / "bars.csv"
    count = 3 * _REFERENCE_BAR_SLAB_ROWS + 1
    path.write_bytes(_replace_bar_cell(
        _bar_tape(count), _REFERENCE_BAR_SLAB_ROWS + 37, 4, "-0.0"))
    expected = _bars_result(_reference_load_bars_legacy, path)
    control = _BarsWork()
    with control.install():
        assert _bars_result(run_json.load_bars, path) == expected
    control.assert_late_decline(count)
    pack = run_json._pack_bar_bulk_slab

    def repeated_pack(matches):
        _reference_pack_bar_slab(matches)
        return pack(matches)

    work = _BarsWork()
    if kind == "repeated-packing":
        monkeypatch.setattr(run_json, "_pack_bar_bulk_slab", repeated_pack)
        work.pack = repeated_pack.__code__
    else:
        load = run_json._load_bars_bulk
        replacements = (
            ("    for match in ", "    declined = False\n    for match in "),
            ("        batch.append(match.groups())",
             "        if declined:\n"
             "            cursor = match.end()\n"
             "            continue\n"
             "        batch.append(match.groups())"),
            ("            if slab is None:\n                return None",
             "            if slab is None:\n"
             "                declined = True\n"
             "                continue"),
            ("    if cursor != len(body):",
             "    if declined:\n        return None\n    if cursor != len(body):"),
        )
        changed = _mutate(load, *replacements[0], *replacements[1:])
        source = textwrap.dedent(inspect.getsource(load))
        for before, after in replacements:
            source = source.replace(before, after)
        work.bulk = changed.__code__
        work.loop_lines = {
            changed.__code__.co_firstlineno + offset
            for offset, line in enumerate(source.splitlines())
            if line.strip().startswith("for match in ")
        }
        assert len(work.loop_lines) == 1
        monkeypatch.setattr(run_json, "_load_bars_bulk", changed)
    with work.install():
        assert _bars_result(run_json.load_bars, path) == expected
    assert work.converted == count
    if kind == "repeated-packing":
        assert work.packed_rows > _REFERENCE_BAR_SLAB_ROWS
    else:
        assert work.packed_rows == _REFERENCE_BAR_SLAB_ROWS and work.slabs == 2
        assert work.regex_calls == 1
        assert work.iterations >= count
        assert work.iterations - (2 * _REFERENCE_BAR_SLAB_ROWS + 16) >= (
            _REFERENCE_BAR_SLAB_ROWS - 16)
    with pytest.raises(AssertionError, match="BARS_DECLINE_WORK"):
        work.assert_late_decline(count)


def _bar_admission_pair(raw):
    """Pin conservative bulk refusal independently of equivalent CSV bytes."""
    expected = _reference_load_bars_bulk(raw)
    actual = run_json._load_bars_bulk(raw)
    assert (actual is None) == (expected is None), (
        "BARS_ADMISSION_IDENTITY: a conservatively refused cell entered the packed path")


@pytest.mark.parametrize("guard,value", (
    ("finite", "1e999"), ("negative-zero", "-0.0"), ("negative-zero", "-1e-400"),
))
def test_bar_admission_identity_detects_missing_numeric_guards(tmp_path, monkeypatch, guard, value):
    raw = _replace_bar_cell(_bar_tape(37), 36, 4, value)
    path = tmp_path / "bars.csv"
    path.write_bytes(raw)
    assert _bars_pair(path)[0] == "ok"
    _bar_admission_pair(raw)
    pack = run_json._pack_bar_bulk_slab
    source = inspect.getsource(pack)
    if guard == "finite":
        old = "not math.isfinite(value)"
    else:
        old = ('(value == 0.0 and raw.startswith(b"-"))'
               if 'raw.startswith(b"-")' in source else
               '(value == 0.0 and text.startswith("-"))')
    # A repr check also refuses these values. Remove it when present so
    # this control isolates the finite or negative-zero guard alone.
    additional = ((("\n                    or repr(value) != text", ""),)
                  if "or repr(value) != text" in source else ())
    changed = _mutate(pack, old, "False", *additional)
    monkeypatch.setattr(run_json, "_pack_bar_bulk_slab", changed)
    with pytest.raises(AssertionError, match="BARS_ADMISSION_IDENTITY"):
        _bar_admission_pair(raw)


@pytest.mark.parametrize("count", (4097, 8193))
def test_missing_final_newline_declines_before_body_scan(tmp_path, count):
    path = tmp_path / "bars.csv"
    path.write_bytes(_bar_tape(count)[:-1])
    control = _BarsWork(reference=True)
    with control.install():
        expected = _bars_result(_reference_load_bars, path)
    assert control.converted == count and control.iterations >= count - 1
    assert control.regex_calls > 0 and control.slabs > 0
    work = _BarsWork()
    with work.install():
        actual = _bars_result(run_json.load_bars, path)
    assert actual == expected and actual[0] == "ok" and work.converted == count
    work.assert_early_decline()


@pytest.mark.parametrize("kind", ("dictreader", "late-decline"))
def test_bars_counters_detect_reintroduced_work(tmp_path, monkeypatch, kind):
    path = tmp_path / "bars.csv"
    count = 8193
    if kind == "dictreader":
        path.write_bytes(_bar_tape(count, integer_volume=True))
        load = run_json.load_bars

        def reparse(csv_path):
            result = load(csv_path)
            run_json._load_bars_legacy(csv_path, run_json._read_bar_bytes(csv_path))
            return result

        monkeypatch.setattr(run_json, "load_bars", reparse)
    else:
        path.write_bytes(_bar_tape(count)[:-1])
        monkeypatch.setattr(run_json, "_load_bars_bulk", _reference_load_bars_bulk)
    control = _BarsWork(reference=True)
    with control.install():
        expected = _bars_result(_reference_load_bars, path)
    assert control.converted == count
    work = _BarsWork()
    with work.install():
        actual = _bars_result(run_json.load_bars, path)
    assert actual == expected and actual[0] == "ok"
    with pytest.raises(AssertionError, match="BARS_(DICTREADER|DECLINE)_WORK"):
        if kind == "dictreader":
            work.assert_no_reparse()
        else:
            work.assert_early_decline()


def _bar_fixture(kind):
    raw = _bar_tape(37)
    if kind == "canonical":
        return raw
    if kind == "no-final-lf":
        return raw[:-1]
    if kind == "crlf":
        return raw.replace(b"\n", b"\r\n")
    if kind == "integer-volume":
        return _bar_tape(37, integer_volume=True)
    if kind == "non-ascii":
        return raw.replace(b",100.0,", b",\xff,", 1)
    if kind == "missing-column":
        return b"timestamp,open,high,low,close\n1704067200000,100,102,99,101\n"
    if kind == "exponent":
        return raw.replace(b",100.0,", b",1e2,", 1)
    if kind == "empty":
        return b""
    if kind == "header-only":
        return _REFERENCE_BAR_HEADER
    if kind == "negative-zero":
        return raw.replace(b",1000.0\n", b",-0.0\n", 1)
    if kind == "negative-integer-zero":
        return raw.replace(b",1000.0\n", b",-0\n", 1)
    if kind == "integer-columns":
        return _REFERENCE_BAR_HEADER + b"1704067200000,100,102,99,101,1000\n"
    if kind == "large-integer":
        return raw.replace(b",1000.0\n", b",9007199254740993\n", 1)
    if kind == "nonfinite":
        return raw.replace(b",1000.0\n", b",inf\n", 1)
    if kind == "trailing-junk":
        return raw + b"not,a,bar\n"
    raise AssertionError(kind)


@pytest.mark.parametrize("kind", (
    "canonical", "no-final-lf", "crlf", "integer-volume", "non-ascii",
    "missing-column", "exponent", "empty", "header-only", "negative-zero",
    "negative-integer-zero", "integer-columns", "large-integer", "nonfinite", "trailing-junk",
))
def test_bars_keep_original_array_bytes_hash_and_refusal(tmp_path, kind):
    path = tmp_path / "bars.csv"
    path.write_bytes(_bar_fixture(kind))
    _bars_pair(path)


@pytest.mark.parametrize("kind", ("trailing-junk", "last-invalid-row"))
def test_bars_identity_detects_an_omitted_tail_check(tmp_path, monkeypatch, kind):
    path = tmp_path / "bars.csv"
    raw = _bar_tape(37)
    path.write_bytes(raw + (b"garbage\n" if kind == "trailing-junk" else
                           b"1704069420000,not-a-number,102.0,99.0,101.0,1000.0\n"))
    assert _bars_pair(path)[0] == "error"
    changed = _mutate(
        run_json._load_bars_bulk, "    if cursor != len(body):\n        return None\n", "")
    monkeypatch.setattr(run_json, "_load_bars_bulk", changed)
    with pytest.raises(AssertionError):
        _bars_pair(path)
