"""Independent copies of the original sink validators and equity encoder.

The public version-1 reader with an unconditional lexical walk supplies the
oracle. Shared fixtures exercise real verification, serialization and cleanup;
this module is deliberately outside the run_json_*_test.py collection glob.
"""
from contextlib import contextmanager
import hashlib
import io
import json
import math
from pathlib import Path
import re
import sys
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import run_json_fused_test_support as support
import test_run_json_sink as ordinary


_REFERENCE_I53 = (1 << 53) - 1
_REFERENCE_MAX_LINE_BYTES = 1024 * 1024
_REFERENCE_HEX64 = re.compile(r"[0-9a-f]{16}\Z")
_REFERENCE_UNPARSED = object()
_UNICODE_BOUNDARIES = (
    "\ud7ff", "\udbff", "\udc00", "\udfff", "\ue000", "\uffff",
    "\U0001f600", "\udc00\ud800",
)


class _ReferenceFloatToken(NamedTuple):
    value: float
    text: str


def _reference_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise pf_sink.SinkFormatError("repeated JSON object key")
        result[key] = value
    return result


def _reference_int_token(text):
    # Bound the text before int(), including on Python versions with a
    # configurable decimal-digit limit. Every integer in v1 fits this bound.
    if len(text.lstrip("-")) > 16:
        raise pf_sink.SinkFormatError("integer exceeds the exact JSON integer bound")
    value = int(text)
    if abs(value) > _REFERENCE_I53:
        raise pf_sink.SinkFormatError("integer exceeds the exact JSON integer bound")
    return value


def _reference_float_token(text):
    value = float(text)
    if not math.isfinite(value):
        raise pf_sink.SinkFormatError("non-finite JSON number")
    return _ReferenceFloatToken(value, text)


def _reference_constant(text):
    raise pf_sink.SinkFormatError(f"non-JSON numeric constant {text}")


def _reference_string(value):
    if type(value) is not str or any(0xD800 <= ord(char) <= 0xDFFF for char in value):
        raise pf_sink.SinkFormatError("expected a Unicode string without unpaired surrogates")
    return value


def _reference_field(value, wire_type):
    if value is None:
        if wire_type.endswith("?"):
            return None
        raise pf_sink.SinkFormatError("null in a non-optional column")
    kind = wire_type.rstrip("?")
    if kind in ("i64", "u64"):
        if type(value) is not int or abs(value) > _REFERENCE_I53 or (kind == "u64" and value < 0):
            raise pf_sink.SinkFormatError(f"invalid {kind} column")
        return value
    if kind == "f64":
        if not isinstance(value, _ReferenceFloatToken) or repr(value.value) != value.text:
            raise pf_sink.SinkFormatError("f64 column is not the Python repr of its double")
        return value.value
    if kind == "bool":
        if type(value) is not bool:
            raise pf_sink.SinkFormatError("bool column requires true or false")
        return value
    if kind in ("str", "hex64"):
        text = _reference_string(value)
        if kind == "hex64" and _REFERENCE_HEX64.fullmatch(text) is None:
            raise pf_sink.SinkFormatError("hex64 requires 16 lowercase hex digits")
        return text
    raise pf_sink.SinkFormatError("unknown wire type")


def _reference_parse(line):
    """Original version-1 parser, including its unconditional lexical walk."""
    if not line.endswith(b"\n"):
        raise pf_sink.SinkFormatError("line does not end with LF")
    if len(line) > _REFERENCE_MAX_LINE_BYTES:
        raise pf_sink.SinkFormatError("line exceeds 1 MiB including LF")
    body = line[:-1]
    if not body:
        raise pf_sink.SinkFormatError("blank line")
    if body.startswith(b"\xef\xbb\xbf"):
        raise pf_sink.SinkFormatError("UTF-8 BOM is forbidden")
    quoted = False
    escaped = False
    for byte in body:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 0x5C:
                escaped = True
            elif byte == 0x22:
                quoted = False
        elif byte == 0x22:
            quoted = True
        elif byte <= 0x20:
            raise pf_sink.SinkFormatError("whitespace or control byte outside a JSON string")
    try:
        return json.loads(body.decode("utf-8"), object_pairs_hook=_reference_pairs,
                          parse_int=_reference_int_token, parse_float=_reference_float_token,
                          parse_constant=_reference_constant)
    except pf_sink.SinkFormatError:
        raise
    except (UnicodeError, ValueError, RecursionError) as error:
        raise pf_sink.SinkFormatError(f"invalid UTF-8 or JSON: {error}") from error


def _reference_record_parse(self, line, *, parsed=_REFERENCE_UNPARSED):
    value = _reference_parse(line) if parsed is _REFERENCE_UNPARSED else parsed
    if type(value) is not list or not value or type(value[0]) is not str:
        raise pf_sink.SinkFormatError("record must be a nonempty array with a string tag")
    tag = value[0]
    if tag not in self._descriptors:
        raise pf_sink.SinkFormatError("record tag is absent from the header")
    descriptor = self._descriptors[tag]
    if len(value) != len(descriptor.fields) + 1:
        raise pf_sink.SinkFormatError("record field count differs from its descriptor")
    values = tuple(_reference_field(item, kind) for item, kind in zip(value[1:], descriptor.types))
    if tag == "T" and values[1] not in ("long", "short"):
        raise pf_sink.SinkFormatError("trade side must be long or short")
    return pf_sink.Record(tag, descriptor.fields, values)


_REFERENCE_E_FLOAT = (
    rb"(?=[-+0-9.e]{1,24}[,\]])-?(?:0|[1-9][0-9]*)"
    rb"(?:\.[0-9]+(?:e[+-]?[0-9]+)?|e[+-]?[0-9]+)"
)
_REFERENCE_E_LINE = re.compile(
    rb'\["E",(0|-?[1-9][0-9]{0,15}),(' + _REFERENCE_E_FLOAT
    + rb"),(" + _REFERENCE_E_FLOAT + rb")\]\n")
_REFERENCE_E_RUN = re.compile(rb"(?:" + _REFERENCE_E_LINE.pattern + rb"){1,128}")


class _ReferenceEquity(run_json._BulkEquity):
    def __init__(self, raw, count, first, last):
        self.raw, self.count, self.first, self.last = raw, count, first, last


def _reference_bulk_equity(raw, *, pattern=None):
    if pattern is None:
        pattern = _REFERENCE_E_RUN
    if not raw or pattern.fullmatch(raw) is None:
        return None
    times, equities, profits = zip(*_REFERENCE_E_LINE.findall(raw))
    timestamps = list(map(int, times))
    if min(timestamps) < -((1 << 53) - 1) or max(timestamps) > (1 << 53) - 1:
        return None
    texts = list(map(bytes.decode, equities + profits))
    numbers = list(map(float, texts))
    if not all(map(math.isfinite, numbers)) or list(map(repr, numbers)) != texts:
        return None
    count = len(timestamps)
    first = dict(time_ms=timestamps[0], equity=numbers[0], open_profit=numbers[count])
    last = dict(time_ms=timestamps[-1], equity=numbers[count - 1], open_profit=numbers[-1])
    return _ReferenceEquity(raw, count, first, last)


def _reference_bulk_equity_ascii(batch):
    rows = _REFERENCE_E_LINE.sub(
        rb'{"time_ms":\1,"equity":\2,"open_profit":\3},' + b"\n",
        batch.raw).splitlines()
    return b"".join(rows)[:-1] if rows else b""


def _reference_stream_equity_array(value, destination):
    destination.write("[")
    pending, count, first = [], 0, True
    try:
        for record in value.sink._tagged_records("E", bulk=True):
            if isinstance(record, run_json._BulkEquity):
                rows = _REFERENCE_E_LINE.sub(
                    rb'{"time_ms":\1,"equity":\2,"open_profit":\3},' + b"\n",
                    record.raw).splitlines()
            else:
                rows = [json.dumps(record.as_dict(), separators=(",", ":"),
                                   allow_nan=False).encode("ascii") + b","]
            offset = 0
            while offset < len(rows):
                take = min(run_json._STREAM_BATCH_RECORDS - count, len(rows) - offset)
                chunk = b"".join(rows[offset:offset + take])[:-1]
                pending.append(chunk if first else b"," + chunk)
                first = False
                count += take
                offset += take
                if count == run_json._STREAM_BATCH_RECORDS:
                    chunk = b"".join(pending)
                    pending.clear()  # never retry a failed write
                    count = 0
                    run_json._write_json_ascii(destination, chunk)
    finally:
        # Match the ordinary array writer's completed prefix on producer error.
        if pending:
            run_json._write_json_ascii(destination, b"".join(pending))
    destination.write("]")


@contextmanager
def _sink_case(tmp_path, rows):
    library = ordinary.SinkLibrary()
    sink = run_json._ReportSink(library, 1, directory=str(tmp_path))
    try:
        library.counts, library.sha = ordinary.write_sink(sink.path, rows)
        report = run_json.ReportC()
        support.set_report(report, rows)
        yield sink, library, report
    finally:
        sink.close()


def _assemble(sink, library, report, monkeypatch, *, reference=False):
    """Compare complete stdout, including LF, after real preparation/cleanup."""
    target = support.reference_sink(run_json, sink)
    stdout = io.StringIO()

    def publish(artifact, document):
        assert run_json._write_report(artifact) == 0

    try:
        with monkeypatch.context() as patch:
            if reference:
                patch.setattr(pf_sink, "_parse", _reference_parse)
                patch.setattr(pf_sink, "_string", _reference_string)
                patch.setattr(pf_sink.RecordParser, "parse", _reference_record_parse)
                patch.setattr(run_json, "_BULK_E_RUN", _REFERENCE_E_RUN)
                patch.setattr(run_json, "_bulk_equity", _reference_bulk_equity)
                patch.setattr(run_json, "_bulk_equity_ascii", _reference_bulk_equity_ascii)
            patch.setattr(run_json, "_REPORT_OUTPUT", None)
            patch.setattr(run_json, "_LIBRARY_REPORT_RUN", None)
            patch.setattr(sys, "stdout", stdout)
            outcome = support.assemble(
                run_json, target, library, report, consume=publish)
        outcome.report = stdout.getvalue().encode("ascii")
        return outcome, target.sha256
    finally:
        target.close()


def _rewrite_body(sink, library, edit):
    raw = Path(sink.path).read_bytes()
    cut = raw.rfind(b'\n{"trailer":') + 1
    assert cut > 0
    body, trailer = edit(raw[:cut]), json.loads(raw[cut:])
    library.sha = trailer["sha256"] = hashlib.sha256(body).hexdigest()
    Path(sink.path).write_bytes(body + ordinary.encoded(trailer))
