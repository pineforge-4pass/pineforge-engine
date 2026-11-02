"""Observe lexical work without counters in the reader being optimized.

The references preserve the original lexical, token and field validators.
Both report arms use the real fused verifier, spools, serializer and stdout
writer; the reference arm substitutes the shared original validators.
No native library, generated strategy or corpus is needed.
"""
from contextlib import contextmanager
import hashlib
import inspect
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import run_json_fused_test_support as support
import test_run_json_sink as ordinary
from run_json_trade_scan_test import _point, _trade
from run_json_wrapper_test_support import (
    _REFERENCE_MAX_LINE_BYTES, _UNICODE_BOUNDARIES, _assemble, _reference_constant,
    _reference_float_token, _reference_int_token, _reference_pairs,
    _reference_parse, _reference_record_parse, _reference_string,
    _rewrite_body, _sink_case,
)


class _LexicalWork:
    """Observe all parser lines and non-JSON Python descendants, including generators."""
    def __init__(self, parse):
        source, start = inspect.getsourcelines(parse)
        self.loop_lines = {start + offset for offset, text in enumerate(source)
                           if text.strip() == "for byte in body:"}
        self.code = parse.__code__
        self.json_codes = {function.__code__ for function in (
            json.loads, pf_sink._pairs, pf_sink._int_token, pf_sink._float_token,
            pf_sink._constant, _reference_pairs, _reference_int_token,
            _reference_float_token, _reference_constant)}
        self.lines = []
        self.loop_events = 0
        self.line_events = 0
        self.python_calls = 0
        self.helper_lines = 0
        self.helpers = set()

    @property
    def body_bytes(self):
        return sum(len(line) - 1 for line in self.lines)

    def _trace(self, frame, event, arg):
        if frame.f_code is self.code:
            if event == "call":
                self.lines.append(frame.f_locals["line"])
            elif event == "line":
                self.line_events += 1
                self.loop_events += frame.f_lineno in self.loop_lines
            return self._trace
        if event == "call" and frame.f_back is not None:
            parent = frame.f_back
            if ((parent.f_code is self.code or id(parent) in self.helpers)
                    and frame.f_code not in self.json_codes):
                self.python_calls += 1
                self.helpers.add(id(frame))
        if id(frame) in self.helpers:
            if event == "line":
                self.helper_lines += 1
            elif event == "return":
                self.helpers.remove(id(frame))
            return self._trace
        return None

    def assert_compact(self, lines):
        assert self.lines == lines
        assert self.body_bytes == sum(len(line) - 1 for line in lines)
        assert self.line_events <= 16 * len(lines), (
            f"SINK_LEXICAL_RESCAN: {self.line_events} parser LINE events; "
            f"budget is 16 per compact line ({len(lines)} lines)")
        assert self.python_calls == self.helper_lines == 0, (
            f"SINK_LEXICAL_RESCAN: {self.python_calls} non-JSON Python calls and "
            f"{self.helper_lines} helper LINE events; both budgets are zero")
        assert self.loop_events == 0, (
            f"SINK_LEXICAL_RESCAN: {self.loop_events} lexical-loop LINE events; budget is zero")

    @contextmanager
    def install(self):
        previous = sys.gettrace()
        sys.settrace(self._trace)
        try:
            yield self
        finally:
            sys.settrace(previous)


class _ColumnCopyWork:
    """Observe class read/write seams inside the existing column-copy frame.

    This is an accounting guard, not a claim to remove the text round trip.
    Read-return bytes and destination-write bytes must reconcile separately
    with each real column's size and digest, including copies across windows.
    """
    def __init__(self):
        self.columns = {}
        self.pending = {}

    def _entry(self, file):
        if file not in self.columns:
            self.columns[file] = dict(
                reads=0, writes=0, read_bytes=0, write_bytes=0,
                read_sha=hashlib.sha256(), write_sha=hashlib.sha256(),
                read_bases=set())
        return self.columns[file]

    @contextmanager
    def install(self, monkeypatch):
        read, write = run_json._BoundedJSONFile.read, run_json._BoundedJSONFile.write
        copy_code = run_json._stream_json_value.__code__

        def observed_read(file, count=-1):
            caller = sys._getframe(1)
            if caller.f_code is copy_code:
                # The column serializer copies in 64 KiB requests.
                assert count == 64 * 1024
            value = read(file, count)
            if caller.f_code is copy_code:
                assert type(value) is str, "column reads must supply text to the copy seam"
                entry = self._entry(file)
                if value:
                    data = value.encode("ascii")
                    assert len(data) == min(count, file.size - entry["read_bytes"])
                    assert id(caller) not in self.pending
                    self.pending[id(caller)] = file, data
                    entry["reads"] += 1
                    entry["read_bytes"] += len(data)
                    entry["read_sha"].update(data)
                    entry["read_bases"].add(file.read_base)
            return value

        def observed_write(file, text):
            written = write(file, text)
            caller = sys._getframe(1)
            if caller.f_code is copy_code and id(caller) in self.pending:
                source, expected = self.pending.pop(id(caller))
                assert type(text) is str, "column chunks still use the fault-injectable write seam"
                entry = self._entry(source)
                data = text.encode("ascii")
                assert data == expected, "each read chunk must be copied without byte changes"
                entry["writes"] += 1
                entry["write_bytes"] += len(data)
                entry["write_sha"].update(data)
            return written

        with monkeypatch.context() as patch:
            patch.setattr(run_json._BoundedJSONFile, "read", observed_read)
            patch.setattr(run_json._BoundedJSONFile, "write", observed_write)
            yield self

    def assert_one_copy(self, *, across_windows=False):
        assert not self.pending, "every nonempty column read must have a matching write"
        assert len(self.columns) == 2, "both E and T spools must enter real column copying"
        for file, entry in self.columns.items():
            assert entry["read_bytes"] == entry["write_bytes"] == file.size > 0
            assert file.size < 32 * 1024 * 1024
            # Copy requests are independent of the backing artifact window.
            read_size = 64 * 1024
            assert entry["reads"] == entry["writes"] == (
                file.size + read_size - 1) // read_size
            assert entry["read_sha"].digest() == entry["write_sha"].digest()
            if across_windows:
                assert file.size > file.window
                assert len(entry["read_bases"]) > 1


def _compact_rows(count):
    rows = []
    for index in range(count):
        rows.extend((_point(index), _trade(index + 1, pnl=float(index % 3 - 1))))
    return rows


def _reference_observer_control():
    line = ordinary.encoded(_trade(1, entry_id="identifier" * 97))
    work = _LexicalWork(_reference_parse)
    with work.install():
        _reference_parse(line)
    assert work.lines == [line]
    assert work.line_events >= work.body_bytes > 16
    assert work.body_bytes <= work.loop_events <= work.body_bytes + 1


def _fallback_observer_control(work):
    line = ordinary.encoded(_trade(1, entry_id='space "\\ trailing\\'))
    previous = work.line_events
    with work.install():
        assert pf_sink._parse(line) == _reference_parse(line)
    assert work.lines[-1] == line
    assert work.line_events - previous >= len(line) - 1


@pytest.mark.parametrize("count", (1, 37, 257))
def test_compact_parse_has_no_python_lexical_work(count):
    _reference_observer_control()
    lines = [ordinary.encoded(ordinary.header())]
    lines.extend(ordinary.encoded(row) for row in _compact_rows(count))
    lines.append(ordinary.encoded(_trade(
        count + 1, entry_id="漢é\x7f", exit_id='q"\\',
        exit_comment="identifier" * 9000)))
    work = _LexicalWork(pf_sink._parse)
    with work.install():
        actual = [pf_sink._parse(line) for line in lines]
    assert actual == [_reference_parse(line) for line in lines]
    work.assert_compact(lines)
    _fallback_observer_control(work)


@pytest.mark.parametrize("count", (1, 257))
def test_fused_report_has_bounded_python_lexical_work(tmp_path, monkeypatch, count):
    _reference_observer_control()
    with _sink_case(tmp_path, _compact_rows(count)) as (sink, library, report):
        expected, reference_sha = _assemble(sink, library, report, monkeypatch, reference=True)
        raw = Path(sink.path).read_bytes()
        strict_lines = [line for line in raw.splitlines(keepends=True)
                        if not line.startswith(b'["E",')]
        lexical = _LexicalWork(pf_sink._parse)
        with lexical.install():
            actual, actual_sha = _assemble(sink, library, report, monkeypatch)
        assert expected.failure is actual.failure is None
        support.assert_same(actual, expected)
        assert hashlib.sha256(actual.report).digest() == hashlib.sha256(expected.report).digest()
        assert reference_sha == actual_sha == library.sha
        assert len(strict_lines) == count + 2
        lexical.assert_compact(strict_lines)
        _fallback_observer_control(lexical)


@pytest.mark.parametrize("count", (1, 257))
def test_fused_columns_copy_each_byte_once(tmp_path, monkeypatch, count):
    across_windows = count > 1
    if across_windows:
        window = support.spool_windows(tmp_path)["E"]
        point = dict(zip(("time_ms", "equity", "open_profit"), _point(0)[1:]))
        row_width = len(json.dumps(point, separators=(",", ":"), allow_nan=False)) + 1
        count = max(count, window // row_width + 2)
    with _sink_case(tmp_path, _compact_rows(count)) as (sink, library, report):
        assert Path(sink.path).stat().st_size < 32 * 1024 * 1024
        control = _ColumnCopyWork()
        with control.install(monkeypatch):
            expected, reference_sha = _assemble(
                sink, library, report, monkeypatch, reference=True)
        assert expected.failure is None and reference_sha == library.sha
        assert len(expected.report) < 32 * 1024 * 1024
        control.assert_one_copy(across_windows=across_windows)
        columns = _ColumnCopyWork()
        with columns.install(monkeypatch):
            actual, actual_sha = _assemble(sink, library, report, monkeypatch)
        assert actual.failure is None and actual_sha == library.sha
        assert len(actual.report) < 32 * 1024 * 1024
        support.assert_same(actual, expected)
        columns.assert_one_copy(across_windows=across_windows)


def test_loop_observer_sees_reference_and_quoted_space_fallback():
    compact = ordinary.encoded(_trade(1))
    fallback = ordinary.encoded(_trade(1, entry_id='space "\\ trailing\\'))
    reference = _LexicalWork(_reference_parse)
    with reference.install():
        _reference_parse(compact)
    assert reference.lines == [compact]
    assert reference.line_events >= reference.body_bytes
    assert reference.body_bytes <= reference.loop_events <= reference.body_bytes + 1
    work = _LexicalWork(pf_sink._parse)
    with work.install():
        actual = pf_sink._parse(fallback)
    assert actual == _reference_parse(fallback)
    assert work.lines == [fallback]
    assert work.line_events >= work.body_bytes
    assert work.body_bytes <= work.loop_events <= work.body_bytes + 1


def _python_byte_search(body):
    for byte in body:
        if byte <= 0x20:
            return True
    return None


@pytest.mark.parametrize("kind", ("generator", "nested-helper"))
def test_lexical_counter_detects_other_python_byte_walks(monkeypatch, kind):
    class PythonSearch:
        def search(self, body):
            if kind == "generator":
                return True if any(byte <= 0x20 for byte in body) else None
            return _python_byte_search(body)

    _reference_observer_control()
    # On a parser without the compiled search, the original walk still runs
    # and still trips the counter.
    monkeypatch.setattr(pf_sink, "_SPACE_OR_CONTROL", PythonSearch(), raising=False)
    line = ordinary.encoded(_trade(1, entry_id="identifier" * 97))
    work = _LexicalWork(pf_sink._parse)
    with work.install():
        assert pf_sink._parse(line) == _reference_parse(line)
    with pytest.raises(AssertionError, match="SINK_LEXICAL_RESCAN"):
        work.assert_compact([line])


def _identity_rows(kind):
    if kind == "empty":
        return []
    if kind == "one-trade":
        return [_point(0), _trade(1, pnl=-0.0)]
    if kind == "many-trades":
        return _compact_rows(257)
    if kind == "long-ids":
        return [_point(0), _trade(
            1, entry_id="identifier" * 9000, exit_id='quote"\\漢' * 3000,
            exit_comment="comment with spaces\tand\nescapes" * 1000)]
    if kind == "odd-floats":
        pnls = (-0.0, 5e-324, -5e-324, 1e-7, 1e16, -1e16)
        rows = [["E", 0, 1.25e100, -0.0], ["E", 1, 10000.0, 5e-324]]
        rows.extend(_trade(
            index + 1, pnl=pnl, pnl_pct=-0.0, qty=1e-7,
            entry_price=1.7976931348623157e308, max_runup=5e-324)
                    for index, pnl in enumerate(pnls))
        return rows
    if kind == "equity-gaps":
        return [_point(0), _trade(1), _point(7), _point(999),
                _trade(2, pnl=-7.0), _point(100000)]
    if kind == "escaped-tags":
        return [_point(0), _trade(1), _point(13), _trade(2)]
    raise AssertionError(kind)


@pytest.mark.parametrize("kind", (
    "empty", "one-trade", "many-trades", "long-ids", "odd-floats",
    "equity-gaps", "escaped-tags",
))
def test_complete_report_bytes_match_pre_change_parser(tmp_path, monkeypatch, kind):
    with _sink_case(tmp_path, _identity_rows(kind)) as (sink, library, report):
        if kind == "escaped-tags":
            _rewrite_body(sink, library, lambda body: body.replace(
                b'["E",', b'["\\u0045",').replace(b'["T",', b'["\\u0054",'))
        expected, reference_sha = _assemble(sink, library, report, monkeypatch, reference=True)
        actual, actual_sha = _assemble(sink, library, report, monkeypatch)
        assert actual.failure is expected.failure is None
        support.assert_same(actual, expected)
        assert hashlib.sha256(actual.report).digest() == hashlib.sha256(expected.report).digest()
        assert reference_sha == actual_sha == library.sha


def _refusal(parse, line):
    with pytest.raises(pf_sink.SinkFormatError) as error:
        parse(line)
    return type(error.value), str(error.value), error.value.args


@pytest.mark.parametrize("byte", range(0x21), ids=lambda byte: f"byte-{byte:02x}")
@pytest.mark.parametrize("prefix,suffix", (
    (b"", b'["E",1,1.0,0.0]'),
    (b'["E",', b"1,1.0,0.0]"),
    (b'["E",1,1.0,0.0]', b""),
    (b'["quote\\"tail",', b"1.0]"),
    (b'["slash\\\\",', b"1.0]"),
    (b'["quoted space",', b"1.0]"),
), ids=("leading", "between-fields", "trailing", "escaped-quote", "escaped-slash", "quoted-space"))
def test_every_outside_space_or_control_keeps_lexical_refusal(byte, prefix, suffix):
    line = prefix + bytes((byte,)) + suffix + b"\n"
    expected = _refusal(_reference_parse, line)
    assert expected[1] == "whitespace or control byte outside a JSON string"
    assert _refusal(pf_sink._parse, line) == expected


@pytest.mark.parametrize("byte", (b" ", b"\x01"), ids=("space", "control"))
def test_outside_byte_after_long_trade_text_keeps_lexical_refusal(byte):
    compact = ordinary.encoded(_trade(1, exit_comment="identifier" * 1000))
    assert pf_sink._parse(compact) == _reference_parse(compact)
    assert compact.endswith(b"]\n") and len(compact) > 10000
    line = compact[:-2] + byte + compact[-2:]
    expected = _refusal(_reference_parse, line)
    assert expected[1] == "whitespace or control byte outside a JSON string"
    assert _refusal(pf_sink._parse, line) == expected


@pytest.mark.parametrize("line", (
    b'["E",1,1.0,0.0]', b"\n", b'\xef\xbb\xbf["E",1,1.0,0.0]\n',
    b"[" * pf_sink.MAX_LINE_BYTES + b"\n",
    b'["\xff"]\n', b'["unterminated]\n', b'["dangling\\"]\n',
    b'{"x":1,"x":2}\n', b"[NaN]\n", b"[Infinity]\n", b"[-Infinity]\n",
    b"[1e999]\n", b"[9007199254740992]\n", b"[10000000000000000]\n",
    b"[\x7f]\n", b'["raw\x01control"]\n', b'["E",1,1.0,0.0]\r\n',
    b'\xff ["E",1,1.0,0.0]\n',
    b'\xef\xbb\xbf ["E",1,1.0,0.0]\n',
    b" " + b"[" * _REFERENCE_MAX_LINE_BYTES + b"\n",
), ids=(
    "no-lf", "blank", "bom", "too-long", "utf8", "quote", "escape", "duplicate-key",
    "nan", "infinity", "negative-infinity", "overflow-float", "inexact-int",
    "long-int", "del-outside", "raw-string-control", "crlf", "lexical-before-utf8",
    "bom-before-lexical", "length-before-lexical",
))
def test_framing_json_and_token_refusals_are_identical(line):
    assert _refusal(pf_sink._parse, line) == _refusal(_reference_parse, line)


@pytest.mark.parametrize("text", (
    "", "plain", 'quote"slash\\', "with space", "\t\n\r\x00\x1f",
    "\x7f", "漢é", '\\" space after escape', "\\", *_UNICODE_BOUNDARIES,
))
def test_quoted_text_keeps_pre_change_acceptance(text):
    line = json.dumps(_trade(1, entry_id=text), ensure_ascii=True,
                      separators=(",", ":")).encode("ascii") + b"\n"
    assert pf_sink._parse(line) == _reference_parse(line)
    parser = pf_sink.RecordParser(ordinary.header())
    try:
        _reference_string(text)
    except pf_sink.SinkFormatError:
        expected = _refusal(lambda wire: _reference_record_parse(parser, wire), line)
        assert _refusal(parser.parse, line) == expected
    else:
        expected = _reference_record_parse(parser, line)
        assert parser.parse(line) == expected
        assert expected.values[17] == text


@pytest.mark.parametrize("old,new", (
    (b'["T",1,', b'["T", 1,'),
    (b'["T",1,', b'["T",\t1,'),
    (b'["T",1,', b'["T",\r1,'),
    (b'["T",1,', b'["T",\x001,'),
    (b'["T",1,', b'["T",\x1f1,'),
    (b"fixture-id", b"bad\x01id"),
    (b"fixture-id", b"\xff"),
    (b"fixture-id", b"\\ud800"),
    (b",100.0,", b",1e2,"),
    (b",100.0,", b",NaN,"),
    (b",100.0,", b",1e999,"),
    (b'["T",1,', b'["T",9007199254740992,'),
    (b'"long"', b'"sideways"'),
), ids=(
    "space", "tab", "cr", "nul", "unit-separator", "raw-string-control",
    "invalid-utf8", "surrogate", "noncanonical-f64", "nan", "overflow",
    "inexact-int", "trade-side",
))
def test_corrupt_sink_keeps_complete_failure_envelope(tmp_path, monkeypatch, old, new):
    with _sink_case(tmp_path, [_point(0), _trade(1, entry_id="fixture-id")]) as args:
        sink, library, report = args

        def edit(body):
            assert body.count(old) == 1
            return body.replace(old, new, 1)

        # Recompute both receipt hashes, so a digest refusal cannot hide a
        # weakened lexical, UTF-8, token, string or schema rejection.
        _rewrite_body(sink, library, edit)
        expected, _ = _assemble(sink, library, report, monkeypatch, reference=True)
        actual, _ = _assemble(sink, library, report, monkeypatch)
        assert actual.failure is not None and expected.failure is not None
        support.assert_same(actual, expected)
        assert actual.error.code == "engine_invariant"
        assert "line 3:" in str(actual.error)
        assert actual.report == expected.report == b""
