"""Test sequential trade readback against real sinks.

Run with files on disk. Fixtures, Record values and the byte oracle come from
the existing sink and stream-fastpath tests; this file implements neither a
sink serializer nor a trade parser. Counters and I/O fault injection are
test-only wrappers around the real implementation.
"""
from contextlib import contextmanager
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


WINDOW = 64 * 1024
DIGEST_DIFFERS = "the report sink changed after verification: digest differs"
IDENTITY_DIFFERS = "the report sink changed after verification: file identity differs"
COUNT_DIFFERS = "the report sink T scan disagrees with its verified count"
INVALID = "the engine's report sink is invalid: "


def _trade(number, **changes):
    return ["T", *oracle._trade(number, **changes).values]


def _point(index):
    return ["E", *oracle._equity(index, 10000. + index).values]


def _event(message):
    return ["V", 1, 1, 0, ordinary.bars.T0, ordinary.bars.T0 + 1, 0, 1, 1,
            None, "0123456789abcdef", message]


def _mixed():
    return [
        _trade(1, entry_id="trade-a", pnl=-0.),
        _point(0), _event("event-a"),
        ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0., 4294967295.],
        ["K", 50.],
        _trade(2, entry_id="trade-z", side="short", pnl=-7.),
    ]


def _strict(path):
    with pf_sink.SinkReader(path) as reader:
        records = list(reader)
        assert reader.complete
        return records, reader.header, reader.counts, reader.sha256, reader.trailer


def _trailer_start(raw):
    position = raw.rfind(b'\n{"trailer":')
    assert position >= 0
    return position + 1


def _identity(path):
    status = Path(path).stat()
    return status.st_ino, status.st_size


def _replace_once(raw, old, new):
    assert len(old) == len(new)
    assert raw.count(old) == 1
    return raw.replace(old, new, 1)


def _assert_engine_error(error, text):
    assert type(error) is run_json.RunFailure
    assert error.code == "engine_invariant"
    assert str(error) == text
    assert not str(error).startswith(INVALID)


def _assert_format_error(error):
    assert type(error) is run_json.RunFailure
    assert error.code == "engine_invariant"
    assert str(error).startswith(INVALID)
    assert "changed after verification" not in str(error)


@contextmanager
def _sink(tmp_path, rows, *, escaped_tags=(), verify=True):
    library = ordinary.SinkLibrary(outputs=True)
    sink = run_json._ReportSink(library, 1, directory=str(tmp_path))
    try:
        library.counts, library.sha = ordinary.write_sink(sink.path, rows, outputs=True)
        if escaped_tags:
            # The fixture writer spells tags literally. Change only their
            # wire spelling, then reuse its encoder and the real trailer.
            _, _, _, _, trailer = _strict(sink.path)
            raw = Path(sink.path).read_bytes()
            body = raw[:_trailer_start(raw)]
            for tag in escaped_tags:
                needle = f'\n["{tag}",'.encode("ascii")
                replacement = b'\n["\\u00' + f"{ord(tag):02x}".encode("ascii") + b'",'
                assert needle in body
                body = body.replace(needle, replacement)
            library.sha = hashlib.sha256(body).hexdigest()
            trailer = dict(trailer, sha256=library.sha)
            Path(sink.path).write_bytes(body + ordinary.encoded(trailer))
        report = run_json.ReportC()
        report.total_trades = library.counts["T"]
        if verify:
            sink.verify(library, 1, report)
            assert sink.counts == library.counts and sink.sha256 == library.sha
            assert sink.summary_failure is None
        yield sink, library, report
    finally:
        sink.close()


def _padding_before(position, following=()):
    """Size fixture rows with the existing encoder, never hand-built JSON."""
    size = len(ordinary.encoded(ordinary.header(outputs=True)))
    size += len(ordinary.encoded(_event("")))
    size += sum(len(ordinary.encoded(row)) for row in following)
    assert size <= position
    return _event("p" * (position - size))


def test_trade_pass_parses_header_and_trades_only(tmp_path, monkeypatch):
    rows = []
    for index in range(6001):
        rows.append(_point(index))
        if index in (0, 3000, 6000):
            rows.append(_trade(1 + index // 3000))
    rows.extend([_event("tail" + "x" * (WINDOW + 17)), ["K", 50.]])
    with _sink(tmp_path, rows) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        calls = 0
        original = pf_sink._parse

        def counted(line):
            nonlocal calls
            calls += 1
            return original(line)

        # Install only after A and the strict oracle have finished.
        monkeypatch.setattr(pf_sink, "_parse", counted)
        actual = list(sink.records("T"))
        assert actual == expected and len(actual) == sink.counts["T"] == 3
        assert len(rows) > 2000 * sink.counts["T"]
        assert calls == 1 + sink.counts["T"]


@pytest.mark.parametrize("where", ["trade", "nontrade"])
@pytest.mark.parametrize("wrong_count", [False, True], ids=["count-intact", "count-also-wrong"])
def test_same_size_byte_flip_gets_digest_refusal(tmp_path, where, wrong_count):
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        path = Path(sink.path)
        identity, raw = _identity(path), path.read_bytes()
        old, new = ((b"trade-a", b"trade-b") if where == "trade"
                    else (b"event-a", b"event-b"))
        path.write_bytes(_replace_once(raw, old, new))
        assert _identity(path) == identity
        count = sink.counts["T"]
        if wrong_count:
            sink.counts["T"] += 1
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("T"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        # Refusal is at exhaustion, after provisional rows were yielded.
        assert len(observed) == count
        assert observed[0].as_dict()["entry_id"] == (
            "trade-b" if where == "trade" else "trade-a")


@pytest.mark.parametrize("change", ["append", "shorten", "replace-same", "replace-different"])
def test_identity_changes_before_open_are_refused(tmp_path, change):
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
                rows[0] = _trade(1, entry_id="trade-b", pnl=-0.)
                ordinary.write_sink(replacement, rows, outputs=True)
            assert replacement.stat().st_size == size
            assert replacement.stat().st_ino != inode
            os.replace(replacement, path)
            assert _identity(path)[0] != inode
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("T"))
        _assert_engine_error(failure.value, IDENTITY_DIFFERS)
        assert observed == []  # these changes precede B's open/fstat


def test_same_inode_rewrite_with_valid_new_receipt_gets_digest_refusal(tmp_path):
    rows = _mixed()
    with _sink(tmp_path, rows) as (sink, _, _):
        identity, old_digest = _identity(sink.path), sink.sha256
        rows[0] = _trade(1, entry_id="trade-b", pnl=-0.)
        _, new_digest = ordinary.write_sink(sink.path, rows, outputs=True)
        assert _identity(sink.path) == identity and new_digest != old_digest
        # It is a valid sink in its own right, but differs from pass A.
        assert _strict(sink.path)[3] == new_digest
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("T"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        assert len(observed) == sink.counts["T"]


@pytest.mark.parametrize("delta", [-1, 1], ids=["count-low", "count-high"])
def test_verified_count_disagreement_is_not_a_file_change(tmp_path, delta):
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        raw, digest = Path(sink.path).read_bytes(), sink.sha256
        count = sink.counts["T"]
        sink.counts["T"] += delta
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("T"))
        _assert_engine_error(failure.value, COUNT_DIFFERS)
        assert "changed after verification" not in str(failure.value)
        assert len(observed) == count
        assert Path(sink.path).read_bytes() == raw and sink.sha256 == digest


@pytest.mark.parametrize("wrong_count", [False, True], ids=["count-intact", "count-also-wrong"])
def test_transient_trade_read_flip_is_hashed_with_the_parsed_bytes(
        tmp_path, monkeypatch, wrong_count):
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        raw, identity = Path(sink.path).read_bytes(), _identity(sink.path)
        original = run_json._UncachedSinkFile.read
        flips = []

        def altered(file, count=-1):
            chunk = original(file, count)
            if b"trade-a" in chunk:
                flips.append(file.fileno())
                return _replace_once(chunk, b"trade-a", b"trade-b")
            return chunk

        monkeypatch.setattr(run_json._UncachedSinkFile, "read", altered)
        count = sink.counts["T"]
        if wrong_count:
            sink.counts["T"] += 1
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("T"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        assert len(flips) == 1 and len(observed) == count
        assert observed[0].as_dict()["entry_id"] == "trade-b"
        assert Path(sink.path).read_bytes() == raw
        assert _identity(sink.path) == identity


def test_one_fresh_raw_sequential_stream_supplies_every_hashed_byte(tmp_path, monkeypatch):
    rows = [_trade(1), _event("x" * (3 * WINDOW + 17)), _point(0), _trade(2)]
    with _sink(tmp_path, rows) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        raw = Path(sink.path).read_bytes()
        opened, chunks, states = [], [], []
        original_init = run_json._UncachedSinkFile.__init__
        original_read = run_json._UncachedSinkFile.read
        original_hash = hashlib.sha256
        cursor = 0

        def initialize(file, stream):
            assert isinstance(stream, io.FileIO), "B must open an unbuffered raw fd"
            assert stream.tell() == 0, "B must include the original header bytes"
            original_init(file, stream)
            opened.append((file, file.fileno()))

        def read(file, count=-1):
            nonlocal cursor
            assert 0 < count <= WINDOW
            assert len(opened) == 1 and file is opened[0][0]
            assert file.position == cursor, "B must never rewind or skip bytes"
            chunk = original_read(file, count)
            cursor += len(chunk)
            chunks.append(chunk)
            return chunk

        def no_line_or_seek(*args, **kwargs):
            pytest.fail("B must use sequential raw reads, without readline or seek")

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

        monkeypatch.setattr(run_json._UncachedSinkFile, "__init__", initialize)
        monkeypatch.setattr(run_json._UncachedSinkFile, "read", read)
        monkeypatch.setattr(run_json._UncachedSinkFile, "readline", no_line_or_seek)
        monkeypatch.setattr(run_json._UncachedSinkFile, "seek", no_line_or_seek)
        monkeypatch.setattr(hashlib, "sha256", Digest)
        assert list(sink.records("T")) == expected
        assert len(opened) == len(states) == 1
        assert b"".join(chunks) == raw
        assert b"".join(states[0].parts) == raw[:_trailer_start(raw)]
        file, fd = opened[0]
        assert file.closed and file.buffer is None
        with pytest.raises(OSError):
            os.fstat(fd)


@pytest.mark.parametrize("start", [WINDOW - 5, WINDOW - 4, WINDOW - 3,
                                 WINDOW - 2, WINDOW - 1, WINDOW],
                         ids=["after-comma", "after-quote", "after-T",
                              "after-open-quote", "after-bracket", "at-boundary"])
def test_trade_prefix_and_line_cross_window_boundary(tmp_path, start):
    rows = [_padding_before(start), _trade(1), _point(0), _trade(2)]
    with _sink(tmp_path, rows) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        assert raw[start - 1:start + 5] == b'\n["T",'
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        assert list(sink.records("T")) == expected


def test_record_lf_ends_window_and_trailer_starts_next_window(tmp_path):
    trade = _trade(1)
    rows = [_padding_before(WINDOW, following=[trade]), trade]
    with _sink(tmp_path, rows) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        assert _trailer_start(raw) == WINDOW
        assert raw[WINDOW - 1:WINDOW + 1] == b"\n{"
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        assert list(sink.records("T")) == expected


@pytest.mark.parametrize("population", ["empty", "zero-trades", "first", "last",
                                      "first-and-last", "many-in-one-window"])
def test_trade_population_and_file_order(tmp_path, population):
    if population == "empty":
        rows, count = [], 0
    elif population == "zero-trades":
        rows, count = [_point(i) for i in range(2000)], 0
    elif population == "first":
        rows, count = [_trade(1), _point(0), ["K", 50.]], 1
    elif population == "last":
        rows, count = [_point(0), ["K", 50.], _trade(1)], 1
    elif population == "first-and-last":
        rows, count = [_trade(1), _point(0), _event("middle"), _trade(2)], 2
    else:
        rows, count = [_trade(i + 1, pnl=float(i)) for i in range(191)], 191
    with _sink(tmp_path, rows) as (sink, _, _):
        if population == "many-in-one-window":
            assert _trailer_start(Path(sink.path).read_bytes()) < WINDOW
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        actual = list(sink.records("T"))
        assert actual == expected and len(actual) == sink.counts["T"] == count
        assert [row.as_dict()["n"] for row in actual] == list(range(1, count + 1))


@pytest.mark.parametrize("tag", ["T", "V"])
def test_line_longer_than_window_and_string_delimiters_are_not_framing(tmp_path, tag):
    # Includes literal backslash+n, actual newline, quotes, a T-looking array
    # prefix and an opening brace, all inside a JSON string.
    text = "x" * (WINDOW + 17) + r'\n["T",{' + '\n["T",{' + 'quote"\\漢'
    row = _trade(1, exit_comment=text) if tag == "T" else _event(text)
    rows = [row, _point(0)] if tag == "T" else [_trade(1), row, _trade(2)]
    assert WINDOW < len(ordinary.encoded(row)) < pf_sink.MAX_LINE_BYTES
    with _sink(tmp_path, rows) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        assert list(sink.records("T")) == expected
        if tag == "T":
            assert expected[0].as_dict()["exit_comment"] == text


def test_trade_line_at_maximum_including_lf_is_accepted(tmp_path):
    size = pf_sink.MAX_LINE_BYTES - len(ordinary.encoded(_trade(1, exit_comment="")))
    trade = _trade(1, exit_comment="x" * size)
    assert len(ordinary.encoded(trade)) == pf_sink.MAX_LINE_BYTES
    with _sink(tmp_path, [trade, _event("tail")]) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        assert list(sink.records("T")) == expected


def test_oversized_trade_after_verification_is_a_format_refusal(tmp_path):
    size = pf_sink.MAX_LINE_BYTES - len(ordinary.encoded(_trade(1, exit_comment="")))
    trade = _trade(1, exit_comment="x" * size)
    with _sink(tmp_path, [trade, _event("tail")]) as (sink, _, _):
        path = Path(sink.path)
        identity, raw = _identity(path), path.read_bytes()
        start = raw.index(b'\n["T",') + 1
        lf = start + pf_sink.MAX_LINE_BYTES - 1
        assert raw[lf - 2:lf + 2] == b'"]\n['
        # Add one character to the T string and remove the next row's '[';
        # preserve inode/size so the T-line bound, rather than stat, refuses.
        path.write_bytes(raw[:lf - 2] + b'x"]\n' + raw[lf + 2:])
        assert _identity(path) == identity
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records("T"))
        _assert_format_error(failure.value)
        assert "line exceeds 1 MiB including LF" in str(failure.value)


def test_changed_nontrade_line_is_not_length_tracked_by_trade_scan(tmp_path):
    size = pf_sink.MAX_LINE_BYTES - len(ordinary.encoded(_event("")))
    event = _event("x" * size)
    assert len(ordinary.encoded(event)) == pf_sink.MAX_LINE_BYTES
    with _sink(tmp_path, [_trade(1), event, _point(0), _trade(2)]) as (sink, _, _):
        path = Path(sink.path)
        identity, raw = _identity(path), path.read_bytes()
        start = raw.index(b'\n["V",') + 1
        lf = start + pf_sink.MAX_LINE_BYTES - 1
        assert raw[lf:lf + 2] == b"\n["
        # Merge V and E into a skipped body longer than MAX_LINE_BYTES.
        # A verified the original lines; B proves this change by digest.
        path.write_bytes(raw[:lf] + b" " + raw[lf + 1:])
        assert _identity(path) == identity
        observed = []
        with pytest.raises(run_json.RunFailure) as failure:
            observed.extend(sink.records("T"))
        _assert_engine_error(failure.value, DIGEST_DIFFERS)
        assert [row.as_dict()["n"] for row in observed] == [1, 2]


@pytest.mark.parametrize("escaped_tags", [(), ("T",), ("E",), ("E", "T", "O", "V", "K")],
                         ids=["literal", "escaped-T", "escaped-E", "escaped-mixed"])
def test_escaped_tags_preserve_strict_reader_records(tmp_path, escaped_tags):
    with _sink(tmp_path, _mixed(), escaped_tags=escaped_tags) as (sink, _, _):
        strict, _, counts, digest, _ = _strict(sink.path)
        assert counts["E"] == 1 and counts["T"] == 2
        assert sink.counts == counts and sink.sha256 == digest
        expected = [row for row in strict if row.tag == "T"]
        assert list(sink.records("T")) == expected
        raw = Path(sink.path).read_bytes()
        for tag in escaped_tags:
            assert b'["\\u00' + f"{ord(tag):02x}".encode("ascii") + b'",' in raw


@pytest.mark.parametrize("tag", ["T", "E"])
def test_escaped_tag_token_crosses_window_boundary(tmp_path, tag):
    start = WINDOW - 4
    tail = [_trade(1), _point(0)] if tag == "T" else [_point(0), _trade(1)]
    with _sink(tmp_path, [_padding_before(start), *tail],
               escaped_tags=(tag,)) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        assert raw[start - 1:start + 4] == b'\n["\\u'
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        assert list(sink.records("T")) == expected


def test_escaped_nontrade_tag_decodes_only_its_first_token(tmp_path, monkeypatch):
    rows = [_point(0), _event('quoted "T",{' + "x" * (WINDOW + 17)), _trade(1)]
    with _sink(tmp_path, rows, escaped_tags=("E", "V", "T")) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        calls = 0
        original = pf_sink._parse
        original_loads = pf_sink.json.loads
        decoded = []

        def counted(line):
            nonlocal calls
            calls += 1
            return original(line)

        def loads(value, *args, **kwargs):
            result = original_loads(value, *args, **kwargs)
            decoded.append(result)
            return result

        monkeypatch.setattr(pf_sink, "_parse", counted)
        monkeypatch.setattr(pf_sink.json, "loads", loads)
        assert list(sink.records("T")) == expected
        assert sink.counts["E"] == sink.counts["V"] == sink.counts["T"] == 1
        assert calls == 1 + sink.counts["T"]
        assert all(not isinstance(value, list) or value[0] == "T" for value in decoded)


def test_public_record_parser_builds_descriptors_once_and_matches_strict(tmp_path, monkeypatch):
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        strict, header, _, _, _ = _strict(sink.path)
        parser_type = getattr(pf_sink, "RecordParser", None)
        assert parser_type is not None, "the trade scan requires public pf_sink.RecordParser(header)"
        assert "RecordParser" in pf_sink.__all__
        builds = []
        original = pf_sink._header

        def descriptors(value):
            builds.append(value)
            return original(value)

        monkeypatch.setattr(pf_sink, "_header", descriptors)
        parser = parser_type(header)
        for _ in range(2):
            actual = [parser.parse(ordinary.encoded(row)) for row in _mixed()]
            assert actual == strict
        assert builds == [header]
        assert actual[0].as_dict()["pnl"].hex() == "-0x0.0p+0"


@pytest.mark.parametrize("fault,text", [
    ("not-array", "record must be a nonempty array with a string tag"),
    ("empty-array", "record must be a nonempty array with a string tag"),
    ("tag-type", "record must be a nonempty array with a string tag"),
    ("absent-tag", "record tag is absent from the header"),
    ("field-count", "record field count differs from its descriptor"),
    ("field-type", "bool column requires true or false"),
    ("float-spelling", "f64 column is not the Python repr of its double"),
    ("side", "trade side must be long or short"),
])
def test_public_record_parser_owns_record_validation(tmp_path, fault, text):
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        parser_type = getattr(pf_sink, "RecordParser", None)
        assert parser_type is not None, "the trade scan requires public pf_sink.RecordParser(header)"
        parser = parser_type(sink.header)
        row = _trade(1)
        if fault == "not-array":
            row = None
        elif fault == "empty-array":
            row = []
        elif fault == "tag-type":
            row = [1]
        elif fault == "absent-tag":
            row[0] = "absent"
        elif fault == "field-count":
            row.pop()
        elif fault == "field-type":
            row[1 + ordinary.TRADE_FIELDS.index("open_at_end")] = 0
        elif fault == "float-spelling":
            row[1 + ordinary.TRADE_FIELDS.index("entry_price")] = 100
        else:
            row[2] = "oops"
        with pytest.raises(pf_sink.SinkFormatError) as failure:
            parser.parse(ordinary.encoded(row))
        assert str(failure.value) == text


def test_strict_reader_and_trade_pass_share_one_parser_each(tmp_path, monkeypatch):
    parser_type = getattr(pf_sink, "RecordParser", None)
    assert parser_type is not None, "the trade scan requires public pf_sink.RecordParser(header)"
    original_init, original_parse = parser_type.__init__, parser_type.parse
    parsers, parsed = [], []

    def initialize(parser, header):
        original_init(parser, header)
        parsers.append(parser)

    def parse(parser, *args, **kwargs):
        record = original_parse(parser, *args, **kwargs)
        parsed.append((parser, record.tag))
        return record

    monkeypatch.setattr(parser_type, "__init__", initialize)
    monkeypatch.setattr(parser_type, "parse", parse)
    rows = _mixed()
    with _sink(tmp_path, rows) as (sink, _, _):
        assert len(parsers) == 1
        assert parsed == [(parsers[0], row[0]) for row in rows]
        parsed.clear()
        actual = list(sink.records("T"))
        assert len(parsers) == 2
        assert parsed == [(parsers[1], "T")] * sink.counts["T"]
        assert len(actual) == sink.counts["T"]


@pytest.mark.parametrize("stage", ["verify", "trade-pass"])
@pytest.mark.parametrize("fault", ["malformed-header", "malformed-trade",
                                 "invalid-side", "malformed-final-line",
                                 "unterminated-trailer", "bytes-after-trailer"])
def test_format_errors_are_never_relabelled_as_file_changes(tmp_path, stage, fault):
    with _sink(tmp_path, _mixed(), verify=stage == "trade-pass") as (sink, library, report):
        path = Path(sink.path)
        raw, identity = path.read_bytes(), _identity(path)
        if fault == "malformed-header":
            changed = b"!" + raw[1:]
        elif fault == "malformed-trade":
            start = raw.index(b'\n["T",') + 1
            assert raw[start + 5:start + 6] == b"1"
            changed = raw[:start + 5] + b"?" + raw[start + 6:]
        elif fault == "invalid-side":
            changed = _replace_once(raw, b'"long"', b'"oops"')
        elif fault == "malformed-final-line":
            start = _trailer_start(raw)
            changed = raw[:start] + b"!" + raw[start + 1:]
        elif fault == "bytes-after-trailer":
            assert raw.endswith(b'"}\n')
            changed = raw[:-3] + b"\nx\n"
        else:
            assert raw.endswith(b"\n")
            changed = raw[:-1] + b" "
        path.write_bytes(changed)
        assert _identity(path) == identity
        with pytest.raises(pf_sink.SinkFormatError) as reference:
            with pf_sink.SinkReader(path) as reader:
                list(reader)
        with pytest.raises(run_json.RunFailure) as failure:
            if stage == "verify":
                sink.verify(library, 1, report)
            else:
                list(sink.records("T"))
        _assert_format_error(failure.value)
        if stage == "verify":
            assert str(failure.value) == INVALID + str(reference.value)
        elif fault == "invalid-side":
            assert "trade side must be long or short" in str(failure.value)
        elif fault == "unterminated-trailer":
            assert "line does not end with LF" in str(failure.value)
        elif fault in ("malformed-header", "malformed-trade"):
            assert "invalid UTF-8 or JSON" in str(failure.value)


def test_early_generator_close_skips_final_comparisons_and_releases_fd(tmp_path, monkeypatch):
    rows = [_trade(1), _event("x" * (3 * WINDOW)), _trade(2)]
    with _sink(tmp_path, rows) as (sink, _, _):
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        opened = []
        original = run_json._UncachedSinkFile.__init__

        def initialize(file, stream):
            original(file, stream)
            opened.append((file, file.fileno()))

        monkeypatch.setattr(run_json._UncachedSinkFile, "__init__", initialize)
        sink.sha256 = "0" * 64
        sink.counts["T"] += 1
        records = sink.records("T")
        try:
            assert next(records) == expected[0]
            assert len(opened) == 1 and not opened[0][0].closed
        finally:
            records.close()  # neither a digest nor a count comparison may run
        file, fd = opened[0]
        assert file.closed and file.buffer is None
        with pytest.raises(OSError):
            os.fstat(fd)


def test_interleaved_trades_array_bytes_match_strict_reader_and_existing_oracle(tmp_path):
    rows = _mixed()
    rows[-1] = _trade(2, side="short", pnl=-7., entry_price=5e-324,
                      exit_comment=r'\n["T",{' + 'quote"\n漢')
    with _sink(tmp_path, rows) as (sink, _, _):
        strict = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        expected = oracle._Destination()
        oracle._stream_json(
            run_json._JSONRecordArray(lambda: (run_json._sink_trade(row) for row in strict)),
            expected)
        actual = oracle._Destination()
        run_json._stream_json(run_json._JSONRecordArray(
            lambda: (run_json._sink_trade(row) for row in sink.records("T"))), actual)
        assert actual.bytes() == expected.bytes()
        assert b'"pnl":-0.0' in actual.bytes() and b"5e-324" in actual.bytes()
        assert [row.as_dict()["n"] for row in strict] == [1, 2]


@pytest.mark.parametrize("tag", [None, "E", "K"])
def test_other_filters_keep_strict_reader_format_errors(tmp_path, tag):
    with _sink(tmp_path, _mixed()) as (sink, _, _):
        path = Path(sink.path)
        path.write_bytes(_replace_once(path.read_bytes(), b"event-a", b"event-b"))
        with pytest.raises(pf_sink.SinkFormatError) as reference:
            with pf_sink.SinkReader(path) as reader:
                list(reader)
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records(tag))
        _assert_format_error(failure.value)
        assert str(failure.value) == INVALID + str(reference.value)


def test_digest_refusal_discards_provisional_report_bytes(tmp_path, monkeypatch):
    original = run_json._ReportSink.records
    changed = []

    def records(sink, tag=None):
        if tag == "T" and not changed:
            path = Path(sink.path)
            raw, identity = path.read_bytes(), _identity(path)
            start = raw.index(b'\n["T",') + 1
            position = raw.index(b",100.0,", start)
            assert position < raw.index(b"\n", start)
            path.write_bytes(raw[:position + 1] + b"9" + raw[position + 2:])
            assert _identity(path) == identity
            changed.append(sink.path)
        return original(sink, tag)

    monkeypatch.setattr(run_json._ReportSink, "records", records)
    status, text, stderr, library = ordinary.invoke(tmp_path, monkeypatch)
    assert len(changed) == 1
    assert status == 1 and stderr == ""
    assert text == run_json.failure_line(DIGEST_DIFFERS, "engine_invariant", {})
    assert library.freed == library.installed == changed
    assert all(not Path(path).exists() for path in changed)


@pytest.mark.parametrize("fault", ["eio", "truncate"],
                         ids=["injected-EIO", "mid-scan-truncation"])
def test_oserror_during_trade_scan_is_a_typed_refusal_and_releases_fd(
        tmp_path, monkeypatch, fault):
    rows = []
    for index in range(8001):
        rows.append(_point(index))
        if index in (0, 4000, 8000):
            rows.append(_trade(1 + index // 4000))
    with _sink(tmp_path, rows) as (sink, _, _):
        path = Path(sink.path)
        size = path.stat().st_size
        assert size > 3 * WINDOW and sink.counts["T"] == 3
        opened = []
        calls = 0
        original_init = run_json._UncachedSinkFile.__init__
        original_read = run_json._UncachedSinkFile.read

        def initialize(file, stream):
            original_init(file, stream)
            opened.append((file, file.fileno()))

        def read(file, count=-1):
            nonlocal calls
            calls += 1
            assert len(opened) == 1 and file is opened[0][0]
            if calls == 2:
                assert file.position > 0
                if fault == "eio":
                    raise OSError(errno.EIO, "injected")
                assert count < 0 or count > 17
                cutoff = file.position + 17
                assert cutoff < size
                # B has already opened and read once. Keep a partial window;
                # the real read, rather than this wrapper, detects the short.
                with path.open("r+b") as changed:
                    changed.truncate(cutoff)
            return original_read(file, count)

        # A verified the complete multi-window sink before either seam exists.
        monkeypatch.setattr(run_json._UncachedSinkFile, "__init__", initialize)
        monkeypatch.setattr(run_json._UncachedSinkFile, "read", read)
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records("T"))
        _assert_format_error(failure.value)
        assert calls == 2 and len(opened) == 1
        file, fd = opened[0]
        assert file.closed and file.buffer is None
        with pytest.raises(OSError):
            os.fstat(fd)


def test_trailer_crosses_window_boundary_without_hashing_trailer_bytes(
        tmp_path, monkeypatch):
    start = WINDOW - 20
    trade = _trade(1)
    rows = [_padding_before(start, following=[trade]), trade]
    with _sink(tmp_path, rows) as (sink, _, _):
        raw = Path(sink.path).read_bytes()
        assert _trailer_start(raw) == start
        assert len(raw[start:]) > 20 and len(raw) > WINDOW
        expected = [row for row in _strict(sink.path)[0] if row.tag == "T"]
        original_hash = hashlib.sha256
        states = []

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

        monkeypatch.setattr(hashlib, "sha256", Digest)
        assert list(sink.records("T")) == expected
        assert len(states) == 1
        # Neither the first 20 trailer bytes nor its next-window remainder
        # belongs to the hash, including when the trailer line is carried.
        assert b"".join(states[0].parts) == raw[:start]


def test_unterminated_trailer_crossing_window_boundary_is_a_format_refusal(tmp_path):
    start = WINDOW - 20
    trade = _trade(1)
    rows = [_padding_before(start, following=[trade]), trade]
    with _sink(tmp_path, rows) as (sink, _, _):
        path = Path(sink.path)
        raw, identity = path.read_bytes(), _identity(path)
        assert _trailer_start(raw) == start
        assert len(raw[start:]) > 20 and len(raw) > WINDOW
        assert raw.endswith(b"\n")
        # Preserve the verified inode/size and body; remove only trailer LF.
        path.write_bytes(raw[:-1] + b" ")
        assert _identity(path) == identity
        with pytest.raises(run_json.RunFailure) as failure:
            list(sink.records("T"))
        _assert_format_error(failure.value)


@pytest.mark.parametrize("edge", ["trade", "escaped", "trailer"])
@pytest.mark.parametrize("pad", range(WINDOW - 14, WINDOW + 15),
                         ids=lambda pad: f"pad={pad}")
def test_window_edge_sweep_matches_strict_trade_rows(tmp_path, edge, pad):
    case = f"{edge} pad={pad}"
    tail = [_trade(1), _point(0), _point(1), _trade(2), ["K", 50.]]
    replacements = []
    # The escaped edge escapes the FIRST T (at pad) and one E and keeps the
    # second T literal: swapping rows would reorder the trades and A refuses it.
    for row in ((tail[0], tail[2]) if edge == "escaped" else tail[2:4]):
        literal = ordinary.encoded(row)
        tag = f'["{row[0]}",'.encode("ascii")
        escaped_tag = b'["\\u00' + f"{ord(row[0]):02x}".encode("ascii") + b'",'
        replacements.append((literal, literal.replace(tag, escaped_tag, 1)))
    extra = sum(len(escaped) - len(literal) for literal, escaped in replacements)
    padding =(_padding_before(pad) if edge != "trailer" else
               _padding_before(pad - extra, following=tail))
    with _sink(tmp_path, [padding, *tail], verify=False) as (sink, library, report):
        # Escape the selected E and T tags, retaining one literal tag of each.
        # As in _sink, use the fixture encoder and the strict reader's trailer.
        trailer = _strict(sink.path)[4]
        path = Path(sink.path)
        raw = path.read_bytes()
        body = raw[:_trailer_start(raw)]
        for literal, escaped in replacements:
            assert body.count(b"\n" + literal) == 1, case
            body = body.replace(b"\n" + literal, b"\n" + escaped, 1)
        library.sha = hashlib.sha256(body).hexdigest()
        trailer = dict(trailer, sha256=library.sha)
        path.write_bytes(body + ordinary.encoded(trailer))
        sink.verify(library, 1, report)
        assert sink.counts == library.counts and sink.sha256 == library.sha, case
        assert sink.summary_failure is None, case

        raw = path.read_bytes()
        if edge == "trade":
            assert raw.index(b'\n["T",') + 1 == pad, case
        elif edge == "escaped":
            assert raw.index(b'\n["\\u0054",') + 1 == pad, case
        else:
            assert _trailer_start(raw) == pad, case
            assert len(raw) > WINDOW, case
        assert raw.count(b'\n["T",') == raw.count(b'\n["\\u0054",') == 1, case
        assert raw.count(b'\n["E",') == raw.count(b'\n["\\u0045",') == 1, case
        strict, _, counts, _, _ = _strict(sink.path)
        expected = [row for row in strict if row.tag == "T"]
        actual = list(sink.records("T"))
        assert actual == expected, case
        assert len(actual) == sink.counts["T"] == counts["T"] == report.total_trades == 2, case
