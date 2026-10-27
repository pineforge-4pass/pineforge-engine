"""CR-397 review tests: Python owns expectations; the real codec owns answers.

Only fixture construction and ctypes setup are shared with the older tests.
No engine writer, native result, finalizer or converter supplies an oracle.
"""
from __future__ import annotations

from contextlib import contextmanager
import copy
from dataclasses import dataclass
import hashlib
import io
import json

import test_run_json_native_sink_json as existing


pf_sink = existing.pf_sink
run_json = existing.run_json
TAGS = "ETOVKXH"
WINDOW = 65536
CAP = 1048576


def compact(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii")


def row(tag, *values):
    return tag, compact([tag, *values]) + b"\n"


def trade(n=1, price=1.0, *, comment=""):
    # Arbitrary finite prices must not enter a cumulative-PnL overflow fold.
    return row("T", n, "long", 0, 1, price, price, 1.0, 0.0, 0.0,
               0.0, 0.0, 0.0, 0, 1, False, n, 0, "", "", comment)


def token_row(tag, token):
    values = (["E", 0, 1.0, 0.0] if tag == "E"
              else json.loads(trade()[1]))
    tokens = [compact(value) for value in values]
    tokens[2 if tag == "E" else 5] = token
    return tag, b"[" + b",".join(tokens) + b"]\n"


def pack(rows=(), *, header=None, trailer_edit=None):
    """Handwritten framing; explicit tags count even malformed record bytes."""
    header = existing.fixtures.header() if header is None else header
    counts = {descriptor["type"]: 0 for descriptor in header["records"]}
    lines = []
    for tag, line in rows:
        assert tag in counts and line.endswith(b"\n")
        counts[tag] += 1
        lines.append(line)
    prefix = compact(header) + b"\n" + b"".join(lines)
    trailer = {"trailer": "pf-report-sink", "version": 1, "counts": counts,
               "sha256": hashlib.sha256(prefix).hexdigest()}
    if trailer_edit is not None:
        trailer = trailer_edit(trailer)
    return prefix + compact(trailer) + b"\n"


@dataclass(frozen=True)
class Refusal:
    exception_type: type
    code: str
    code_args: dict
    message: str


def failure(error):
    return Refusal(type(error), error.code, copy.deepcopy(error.code_args), str(error))


def capture(call):
    try:
        return call()
    except run_json.RunFailure as error:
        return failure(error)


def project(record):
    width = 3 if record.tag == "E" else 16
    return dict(zip(record.fields[:width], record.values[:width]))


@dataclass(frozen=True)
class Source:
    raw_header: bytes
    header: dict
    counts: dict
    digest: str
    fragments: dict


def strict_source(payload):
    """Exhaust the pinned reader, including its trailer and EOF checks."""
    try:
        with pf_sink.SinkReader(stream=io.BytesIO(payload)) as reader:
            records = list(reader)
            assert reader.complete
            counts, digest, header = reader.counts, reader.sha256, reader.header
    except pf_sink.SinkFormatError as error:
        return failure(run_json._engine_invariant(
            "the engine's report sink is invalid: " + str(error)))
    assert counts == {tag: sum(record.tag == tag for record in records) for tag in counts}
    fragments = {
        tag: b",".join(compact(project(record)) for record in records if record.tag == tag)
        for tag in ("E", "T")
    }
    return Source(payload[:payload.index(b"\n") + 1], header, counts, digest, fragments)


def compare_prepare(function, path, payload, *, must_refuse=False, context=""):
    """Public C entry plus the host's final witness translation; no fallback."""
    expected = strict_source(payload)
    if must_refuse:
        assert isinstance(expected, Refusal), ("bad test witness", context, expected)
    path.write_bytes(payload)
    if isinstance(expected, Refusal):
        result, header, offending = existing.raw_call(function, path)
        assert (result.status, result.complete) == (2, 0), (
            "strict reader refused but native prepare did not return SOURCE_INVALID",
            context, result.status, result.complete)
        actual = capture(lambda: run_json._raise_native_sink_json(result, header, offending))
        assert isinstance(actual, Refusal), (context, actual)
        assert actual == expected, (context, actual, expected)
    else:
        # Separate files are required by the ABI. pwrite leaves offsets at 0.
        with (path.parent / "review-equity.fragment").open("w+b", buffering=0) as equity, (
                path.parent / "review-trades.fragment").open("w+b", buffering=0) as trades:
            result, header, _ = existing.raw_call(
                function, path, output=equity, trades_output=trades)
            assert (result.status, result.complete) == (0, 1), (context, result.status)
            assert header == expected.raw_header, context
            assert result.counts[:] == [expected.counts.get(tag, 0) for tag in TAGS], context
            assert bytes(result.sha256_hex).decode("ascii") == expected.digest, context
            for tag, file, size, digest in (
                    ("E", equity, result.equity_json_bytes, result.equity_json_sha256_hex),
                    ("T", trades, result.trades_json_bytes, result.trades_json_sha256_hex)):
                fragment = expected.fragments[tag]
                assert file.read() == fragment, (context, tag, "fragment bytes")
                assert size == len(fragment), (context, tag, "fragment length")
                assert bytes(digest).decode("ascii") == hashlib.sha256(fragment).hexdigest()
    return expected


def observed(function, calls):
    def invoke(request_pointer, result_pointer):
        returned = function(request_pointer, result_pointer)
        request, result = request_pointer._obj, result_pointer._obj
        calls.append((int(request.operation), int(request.guard_tag),
                      int(result.status), int(result.complete)))
        return returned
    return invoke


def source_refusal(function, path, payload):
    """Compare FINAL strict/native host refusals, including cold replay."""
    expected = compare_prepare(function, path, payload, must_refuse=True)
    strict = capture(lambda: existing.production_verify(None, path))
    calls = []
    native = capture(lambda: existing.production_verify(observed(function, calls), path))
    assert isinstance(strict, Refusal) and isinstance(native, Refusal)
    assert strict == native == expected
    assert calls == [(1, 0, 2, 0)], "source refusal bypassed the real native prepare"
    return expected


def stream_column(sink, tag, *, native):
    if native:
        column = run_json._NativeSinkJSONColumn(sink.native_json, sink, tag)
    else:
        def records():
            for record in sink.records(tag):
                yield record.as_dict() if tag == "E" else run_json._sink_trade(record)
        column = run_json._JSONRecordArray(records, sink.counts[tag])
    output = io.StringIO()
    run_json._stream_json(column, output)
    return output.getvalue().encode("ascii")


@contextmanager
def guarded_pair(function, path, payload):
    """Both real host prepare paths, then independently opened guard scans."""
    expected = strict_source(payload)
    assert isinstance(expected, Source), expected
    path.write_bytes(payload)
    calls = []
    strict = existing.configured_sink(None, path)
    native = existing.configured_sink(observed(function, calls), path)
    try:
        for sink in (strict, native):
            sink.named, sink.keep = False, True
            sink.run_id = expected.header["run_id"]
            receipt = existing.ReceiptFromStream(path)
            assert receipt.counts == expected.counts and receipt.digest == expected.digest
            report = run_json.ReportC()
            report.total_trades = expected.counts["T"]
            sink.verify(receipt, None, report)
            assert receipt.queries == 1
            assert sink.counts == expected.counts and sink.sha256 == expected.digest
        assert strict.native_json is None
        assert native.native_json is not None and native.native_json.accepted
        assert calls == [(1, 0, 0, 1)], "native prepare fell back"
        yield GuardPair(strict, native, calls, expected)
    finally:
        native.close()
        strict.close()


@dataclass
class GuardPair:
    strict: object
    native: object
    calls: list
    source: Source

    def compare(self, tag, *, must_refuse=False):
        expected = capture(lambda: stream_column(self.strict, tag, native=False))
        before = len(self.calls)
        actual = capture(lambda: stream_column(self.native, tag, native=True))
        assert len(self.calls) == before + 1, "guard was bypassed"
        operation, selected, status, complete = self.calls[-1]
        assert (operation, selected) == (2, ord(tag))
        assert status != 1, "native guard fell back to Python"
        if must_refuse:
            assert isinstance(expected, Refusal), ("bad refusal witness", expected)
        if isinstance(expected, Refusal):
            assert isinstance(actual, Refusal) and complete == 0, (actual, status)
            assert status in (2, 3, 4, 5, 6), status
        else:
            assert (status, complete) == (0, 1)
        assert actual == expected, (actual, expected)
        return expected


def host_fragments(function, path, payload):
    """Full adapter/serialization bytes, compared to pf_sink + json.dumps."""
    with guarded_pair(function, path, payload) as pair:
        for tag in ("T", "E"):
            actual = pair.compare(tag)
            assert actual == b"[" + pair.source.fragments[tag] + b"]", tag
        assert pair.calls == [(1, 0, 0, 1), (2, ord("T"), 0, 1), (2, ord("E"), 0, 1)]
        return pair.source
