"""Strict, streaming pf-report-sink version-1 reader.

Use ``with SinkReader(path) as reader:`` and consume the iterator to its end.
For a blocking binary file or pipe, use ``SinkReader(stream=file)`` instead;
no seek is required. The reader owns and closes either input on completion,
refusal or context exit. Supply exactly one of path and stream.
Each Record has a tag, an immutable values tuple and as_dict(). Records are
provisional until exhaustion verifies the trailer; complete is then True and
trailer/sha256 contain the verified receipt. A caller that stops early has no
verified sink. Retaining yielded records is the caller's choice; the reader
holds one bounded line, the header, seven counts and one SHA-256 state.

Running this file with a sink path verifies it and prints only its trailer.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from typing import Any, BinaryIO, NamedTuple

__all__ = ["I53_MAX", "MAX_LINE_BYTES", "Record", "RecordParser", "SinkFormatError", "SinkReader"]

I53_MAX = (1 << 53) - 1
MAX_LINE_BYTES = 1024 * 1024
_ORDER = "ETOVKXH"
_HEX64 = re.compile(r"[0-9a-f]{16}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_UNPARSED = object()


class SinkFormatError(ValueError):
    """The sink is malformed, incomplete, unreadable or fails its receipt."""


class Record(NamedTuple):
    tag: str
    fields: tuple[str, ...]
    values: tuple[Any, ...]

    def as_dict(self) -> dict[str, Any]:
        return dict(zip(self.fields, self.values))


class _FloatToken(NamedTuple):
    value: float
    text: str


class _Descriptor(NamedTuple):
    fields: tuple[str, ...]
    types: tuple[str, ...]


# Each triple is field, JSON wire type, unit. This is the fixed v1 schema;
# output slots and constants append only their consecutively numbered fields.
_E = (
    ("time_ms", "i64", "unix_ms"),
    ("equity", "f64?", "account_ccy"),
    ("open_profit", "f64?", "account_ccy"),
)
_T = (
    ("n", "i64", ""), ("side", "str", ""),
    ("entry_time", "i64", "unix_ms"), ("exit_time", "i64", "unix_ms"),
    ("entry_price", "f64?", "quote_ccy"), ("exit_price", "f64?", "quote_ccy"),
    ("qty", "f64?", "qty"), ("pnl", "f64?", "account_ccy"), ("pnl_pct", "f64?", "percent"),
    ("max_runup", "f64?", "account_ccy"), ("max_drawdown", "f64?", "account_ccy"),
    ("commission", "f64?", "account_ccy"),
    ("entry_bar_index", "i64", "script_bar"), ("exit_bar_index", "i64", "script_bar"),
    ("open_at_end", "bool", ""), ("entry_incarnation", "u64", ""), ("close_cause", "i64", ""),
    ("entry_id", "str", ""), ("exit_id", "str", ""), ("exit_comment", "str", ""),
)
_O = (("row", "i64", ""), ("open_ms", "i64?", "unix_ms"), ("close_ms", "i64?", "unix_ms"))
_V = (
    ("sequence", "u64", ""), ("output_index", "i64", ""), ("bar_index", "i64", ""),
    ("bar_open_ms", "i64?", "unix_ms"), ("bar_close_ms", "i64?", "unix_ms"),
    ("ordinal_in_bar", "i64", ""), ("phase", "i64", ""), ("confirmed", "i64", ""),
    ("value", "f64?", ""), ("message_hash64", "hex64", ""), ("message", "str?", ""),
)
_X = (("timestamp", "i64", "unix_ms"), ("bar_index", "i64", ""),
      ("name", "str", ""), ("value", "f64?", ""))
_H = (("hash", "hex64", ""),)
_FIXED = {"E": _E, "T": _T, "V": _V, "X": _X, "H": _H}


def _keys(value: Any, expected: tuple[str, ...], label: str) -> None:
    if type(value) is not dict or tuple(value) != expected:
        raise SinkFormatError(f"{label} keys are missing, extra or out of order")


def _string(value: Any) -> str:
    if type(value) is not str or any(0xD800 <= ord(char) <= 0xDFFF for char in value):
        raise SinkFormatError("expected a Unicode string without unpaired surrogates")
    return value


def _version(value: Any) -> None:
    if type(value) is not int or value != 1:
        raise SinkFormatError("expected integer version 1")


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise SinkFormatError("repeated JSON object key")
        result[key] = value
    return result


def _int_token(text: str) -> int:
    # Bound the text before int(), including on Python versions with a
    # configurable decimal-digit limit. Every integer in v1 fits this bound.
    if len(text.lstrip("-")) > 16:
        raise SinkFormatError("integer exceeds the exact JSON integer bound")
    value = int(text)
    if abs(value) > I53_MAX:
        raise SinkFormatError("integer exceeds the exact JSON integer bound")
    return value


def _float_token(text: str) -> _FloatToken:
    value = float(text)
    if not math.isfinite(value):
        raise SinkFormatError("non-finite JSON number")
    return _FloatToken(value, text)


def _constant(text: str) -> Any:
    raise SinkFormatError(f"non-JSON numeric constant {text}")


def _parse(line: bytes) -> Any:
    if not line.endswith(b"\n"):
        raise SinkFormatError("line does not end with LF")
    if len(line) > MAX_LINE_BYTES:
        raise SinkFormatError("line exceeds 1 MiB including LF")
    body = line[:-1]
    if not body:
        raise SinkFormatError("blank line")
    if body.startswith(b"\xef\xbb\xbf"):
        raise SinkFormatError("UTF-8 BOM is forbidden")
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
            raise SinkFormatError("whitespace or control byte outside a JSON string")
    try:
        return json.loads(body.decode("utf-8"), object_pairs_hook=_pairs,
                          parse_int=_int_token, parse_float=_float_token, parse_constant=_constant)
    except SinkFormatError:
        raise
    except (UnicodeError, ValueError, RecursionError) as error:
        raise SinkFormatError(f"invalid UTF-8 or JSON: {error}") from error


def _header(value: Any) -> dict[str, _Descriptor]:
    _keys(value, ("schema", "version", "run_id", "engine", "records"), "header")
    if value["schema"] != "pf-report-sink":
        raise SinkFormatError("unknown report sink schema")
    _version(value["version"])
    _string(value["run_id"])
    _string(value["engine"])
    if type(value["records"]) is not list:
        raise SinkFormatError("header records must be an array")
    descriptors: dict[str, _Descriptor] = {}
    previous = -1
    for descriptor in value["records"]:
        _keys(descriptor, ("type", "fields", "types", "units"), "descriptor")
        tag = _string(descriptor["type"])
        if len(tag) != 1 or tag not in _ORDER or _ORDER.index(tag) <= previous:
            raise SinkFormatError("unknown, repeated or out-of-order record type")
        previous = _ORDER.index(tag)
        if any(type(descriptor[key]) is not list for key in ("fields", "types", "units")):
            raise SinkFormatError("descriptor fields, types and units must be arrays")
        if tag == "O":
            count = len(descriptor["fields"]) - len(_O)
            if count < 0:
                raise SinkFormatError("output descriptor is missing its fixed fields")
            columns = _O + tuple((f"v{i}", "f64?", "") for i in range(count))
        elif tag == "K":
            columns = tuple((f"c{i}", "f64?", "") for i in range(len(descriptor["fields"])))
        else:
            columns = _FIXED[tag]
        for index, key in enumerate(("fields", "types", "units")):
            if descriptor[key] != [column[index] for column in columns]:
                raise SinkFormatError(f"record {tag} differs from the fixed v1 {key}")
        descriptors[tag] = _Descriptor(tuple(descriptor["fields"]), tuple(descriptor["types"]))
    if "E" not in descriptors or "T" not in descriptors:
        raise SinkFormatError("E and T descriptors are required")
    output_tags = sum(tag in descriptors for tag in "OVK")
    if output_tags not in (0, 3):
        raise SinkFormatError("O, V and K descriptors must be selected together")
    return descriptors


def _field(value: Any, wire_type: str) -> Any:
    if value is None:
        if wire_type.endswith("?"):
            return None
        raise SinkFormatError("null in a non-optional column")
    kind = wire_type.rstrip("?")
    if kind in ("i64", "u64"):
        if type(value) is not int or abs(value) > I53_MAX or (kind == "u64" and value < 0):
            raise SinkFormatError(f"invalid {kind} column")
        return value
    if kind == "f64":
        if not isinstance(value, _FloatToken) or repr(value.value) != value.text:
            raise SinkFormatError("f64 column is not the Python repr of its double")
        return value.value
    if kind == "bool":
        if type(value) is not bool:
            raise SinkFormatError("bool column requires true or false")
        return value
    if kind in ("str", "hex64"):
        text = _string(value)
        if kind == "hex64" and _HEX64.fullmatch(text) is None:
            raise SinkFormatError("hex64 requires 16 lowercase hex digits")
        return text
    raise SinkFormatError("unknown wire type")


class RecordParser:
    """Parse record lines against one header, without receipt or file state.

    ``parsed`` lets SinkReader forward the value it already decoded while
    checking for a trailer, preserving one JSON parse per line.
    """

    def __init__(self, header: dict[str, Any]):
        self._descriptors = _header(header)
        self._equity_descriptor = self._descriptors["E"]

    def parse(self, line: bytes, *, parsed: Any = _UNPARSED) -> Record:
        value = _parse(line) if parsed is _UNPARSED else parsed
        if type(value) is not list or not value or type(value[0]) is not str:
            raise SinkFormatError("record must be a nonempty array with a string tag")
        tag = value[0]
        if tag not in self._descriptors:
            raise SinkFormatError("record tag is absent from the header")
        descriptor = self._descriptors[tag]
        if len(value) != len(descriptor.fields) + 1:
            raise SinkFormatError("record field count differs from its descriptor")
        values = tuple(_field(item, kind) for item, kind in zip(value[1:], descriptor.types))
        if tag == "T" and values[1] not in ("long", "short"):
            raise SinkFormatError("trade side must be long or short")
        return Record(tag, descriptor.fields, values)

    def parse_verified(self, line: bytes) -> Record:
        """Decode E after strict sink verification, without the lexical walk.

        The caller must prove the scanned bytes still match that verified
        sink. Keep framing and field checks, including the shared token hooks.
        """
        if not line.endswith(b"\n"):
            raise SinkFormatError("line does not end with LF")
        if len(line) > MAX_LINE_BYTES:
            raise SinkFormatError("line exceeds 1 MiB including LF")
        body = line[:-1]
        if not body:
            raise SinkFormatError("blank line")
        if body.startswith(b"\xef\xbb\xbf"):
            raise SinkFormatError("UTF-8 BOM is forbidden")
        try:
            value = json.loads(body.decode("utf-8"), object_pairs_hook=_pairs,
                               parse_int=_int_token, parse_float=_float_token,
                               parse_constant=_constant)
        except SinkFormatError:
            raise
        except (UnicodeError, ValueError, RecursionError) as error:
            raise SinkFormatError(f"invalid UTF-8 or JSON: {error}") from error
        if type(value) is not list or not value or type(value[0]) is not str:
            raise SinkFormatError("record must be a nonempty array with a string tag")
        if value[0] != "E":
            raise SinkFormatError(f"record tag is {value[0]!r}, expected 'E'")
        descriptor = self._equity_descriptor
        if len(value) != len(descriptor.fields) + 1:
            raise SinkFormatError("record field count differs from its descriptor")
        values = tuple(_field(item, kind) for item, kind in zip(value[1:], descriptor.types))
        return Record("E", descriptor.fields, values)


class SinkReader:
    """One-pass, bounded-memory iterator. Exhaustion verifies the entire sink."""

    def __init__(self, path: str | Path | None = None, *,
                 stream: BinaryIO | None = None, keep_open: bool = False):
        if (path is None) == (stream is None):
            raise TypeError("supply exactly one of path and stream")
        self.header: dict[str, Any] = {}
        self.trailer: dict[str, Any] | None = None
        self.complete = False
        self.sha256 = ""
        self._line_number = 0
        self._hash = hashlib.sha256()
        self._file: BinaryIO | None = None
        self._failed = False
        self._keep_open = bool(keep_open)
        try:
            self._file = stream if stream is not None else open(path, "rb")
            value, line = self._readline()
            self._parser = RecordParser(value)
            self._counts = {descriptor["type"]: 0 for descriptor in value["records"]}
            self.header = value
            self._hash.update(line)
        except (OSError, SinkFormatError) as error:
            self._refuse(error)

    @property
    def counts(self) -> dict[str, int]:
        return dict(self._counts)

    def _refuse(self, error: Exception) -> Any:
        self._failed = True
        self.complete = False
        self.sha256 = ""
        self.trailer = None
        self.close()
        raise SinkFormatError(f"line {self._line_number}: {error}") from error

    def _readline(self) -> tuple[Any, bytes]:
        if self._file is None:
            raise SinkFormatError("reader is closed before verification")
        line = self._file.readline(MAX_LINE_BYTES + 1)
        self._line_number += 1
        if not line:
            raise SinkFormatError("missing header or trailer")
        return _parse(line), line

    def _finish(self, value: dict[str, Any]) -> None:
        _keys(value, ("trailer", "version", "counts", "sha256"), "trailer")
        if value["trailer"] != "pf-report-sink":
            raise SinkFormatError("unknown trailer")
        _version(value["version"])
        _keys(value["counts"], tuple(self._counts), "trailer counts")
        for tag, count in value["counts"].items():
            if type(count) is not int or count < 0 or count > I53_MAX or count != self._counts[tag]:
                raise SinkFormatError(f"trailer count mismatch for {tag}")
        digest = _string(value["sha256"])
        if _SHA256.fullmatch(digest) is None or digest != self._hash.hexdigest():
            raise SinkFormatError("trailer SHA-256 mismatch")
        if self._file is None or self._file.read(1):
            raise SinkFormatError("bytes after the trailer")
        if not self._keep_open:
            self.close()
        self.trailer = value
        self.sha256 = digest
        self.complete = True

    def __iter__(self) -> SinkReader:
        return self

    def __next__(self) -> Record:
        if self.complete:
            raise StopIteration
        if self._failed:
            raise SinkFormatError("reader previously refused this sink")
        try:
            value, line = self._readline()
            if type(value) is dict:
                self._finish(value)
                raise StopIteration
            record = self._parser.parse(line, parsed=value)
            tag = record.tag
            if self._counts[tag] == I53_MAX:
                raise SinkFormatError("record count exceeds the exact JSON integer bound")
            self._counts[tag] += 1
            self._hash.update(line)
            return record
        except (OSError, SinkFormatError) as error:
            self._refuse(error)

    def close(self) -> None:
        if self._file is not None:
            stream, self._file = self._file, None
            stream.close()

    def __enter__(self) -> SinkReader:
        return self

    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> None:
        self.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify a pf-report-sink/v1 file in bounded memory")
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        with SinkReader(args.path) as reader:
            for _ in reader:
                pass
            print(json.dumps(reader.trailer, separators=(",", ":")))
    except SinkFormatError as error:
        print(f"invalid report sink: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
