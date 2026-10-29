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

__all__ = ["I53_MAX", "MAX_LINE_BYTES", "Record", "RecordParser", "SinkFormatError", "SinkReader",
           "ReplaySinkReader", "ReplaySinkFormatError"]

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


REPLAY_RECUT_FIELDS = ["qty", "commission", "pnl", "pnl_pct", "max_runup", "max_drawdown"]
REPLAY_S = (
    ("initial_capital", "f64", "account_ccy"), ("pointvalue", "f64", "quote_ccy"),
    ("net_profit_sum", "f64", "account_ccy"),
)
REPLAY_Q = (
    ("point_index", "i64", ""), ("time_ms", "i64", "unix_ms"),
    ("settlement_count", "i64?", ""), ("net_profit_sum", "f64?", "account_ccy"),
    ("equity_basis", "str", ""), ("equity_term", "f64", "account_ccy"),
)
REPLAY_R = (
    ("n", "i64", ""), ("settlement_index", "i64?", ""), ("parent_id", "str?", ""),
    ("slice_index", "i64?", ""), ("slice_count", "i64?", ""), ("recut_fields", "str[]?", ""),
    ("child_index", "i64?", ""), ("transaction_qty", "f64?", "qty"),
    ("whole_qty", "f64?", "qty"), ("whole_pnl", "f64?", "account_ccy"),
    ("whole_commission", "f64?", "account_ccy"), ("whole_max_runup", "f64?", "account_ccy"),
    ("whole_max_drawdown", "f64?", "account_ccy"), ("whole_exit_fee", "f64?", "account_ccy"),
    ("transaction_fee", "f64?", "account_ccy"),
)
REPLAY_COLUMNS = {"S": REPLAY_S, "Q": REPLAY_Q, "R": REPLAY_R}


class ReplaySinkFormatError(ValueError):
    """Typed refusal of a companion or of a bound exact-replay claim."""
    def __init__(self, code, message=None, code_args=None):
        self.code = code
        self.code_args = code_args or {}
        super().__init__(message or code.replace("_", " "))


def _replay_bits(value):
    import struct
    return struct.pack(">d", float(value))


def _replay_double(value):
    import struct
    return struct.unpack(">d", value)[0]


def _replay_int(value, minimum=0):
    return type(value) is int and minimum <= value <= I53_MAX


def _replay_number(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def _replay_parse(line):
    # Preserve parsed binary64 bits, including -0.0. Nonfinite tokens are
    # judged by their record's validator, so the error keeps its S/R/Q type.
    if (not line.endswith(b"\n") or len(line) > MAX_LINE_BYTES
            or line.startswith(b"\xef\xbb\xbf") or line == b"\n"):
        raise ReplaySinkFormatError("framing_invalid")
    quoted = escaped = False
    for byte in line[:-1]:
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
            raise ReplaySinkFormatError("framing_invalid")
    try:
        return json.loads(line.decode("utf-8"), object_pairs_hook=_pairs,
                          parse_int=lambda text: -0.0 if text == "-0" else int(text))
    except (UnicodeError, ValueError, RecursionError) as error:
        raise ReplaySinkFormatError("json_invalid", str(error)) from error


def _replay_record(value, descriptors):
    if (type(value) is not list or not value or type(value[0]) is not str
            or value[0] not in descriptors):
        raise ReplaySinkFormatError("record_invalid")
    tag = value[0]
    columns = REPLAY_COLUMNS[tag]
    code = tag.lower() + "_record_invalid"

    def require(condition):
        if not condition:
            raise ReplaySinkFormatError(code)

    require(len(value) == len(columns) + 1)
    row = value[1:]
    for item, (_, kind, _) in zip(row, columns):
        if item is None and kind.endswith("?"):
            continue
        kind = kind.rstrip("?")
        require((_replay_int(item, -I53_MAX) if kind == "i64"
                 else _replay_number(item) if kind == "f64"
                 else type(item) is list and all(type(s) is str for s in item) if kind == "str[]"
                 else type(item) is str and not any(0xD800 <= ord(c) <= 0xDFFF for c in item)))
    if tag == "R":
        n, settlement, parent, index, count, fields, child = row[:7]
        require(_replay_int(n, 1))
        require(settlement is None or _replay_int(settlement))
        if parent is None:
            require(all(item is None for item in row[2:]))
        else:
            require(type(parent) is str and re.fullmatch(r"0|[1-9][0-9]{0,19}", parent) is not None)
            require(int(parent) <= (1 << 64) - 1 and settlement is not None)
            require(_replay_int(index) and _replay_int(count, 1))
            require(_replay_int(child))
            require(fields == [] or fields == REPLAY_RECUT_FIELDS)
            require(all(_replay_number(item) for item in row[7:13]))
            require(row[7] > 0 and row[8] > 0)
            if fields == []:
                require(row[13] is None and row[14] is None)
            else:
                require((row[13] is None) == (row[14] is None))
    elif tag == "Q":
        index, _, count, net, basis, term = row
        require(_replay_int(index))
        require(basis in ("kernel_open", "range_end", "terminal_quote", "anchor"))
        if basis == "anchor":
            require(count is None and net is None and _replay_bits(term) == _replay_bits(0.0))
        else:
            require(_replay_int(count) and _replay_number(net))
    return Record(tag, tuple(column[0] for column in columns), tuple(row))


def _replay_mapping(line):
    return _replay_record(_replay_parse(line), REPLAY_COLUMNS).as_dict()


class _ReplayPrimaryStream:
    """Retain one raw primary line only for a bound replay validator."""
    def __init__(self, path):
        self.file = open(path, "rb")
        self.line = b""

    def readline(self, limit):
        self.line = self.file.readline(limit)
        return self.line

    def read(self, limit):
        return self.file.read(limit)

    def close(self):
        self.file.close()


class ReplaySinkReader:
    """Independent companion iterator; bound exhaustion validates exact replay.

    Format-only reads keep a bounded line and counters. Bound reads use a
    temporary SQLite database with a 256 KiB cache, never a T/Q-sized RAM map.
    Raw engine lines and binary64 blobs preserve signed zero. No monetary
    value is re-encoded before its bit comparison.
    The database, streams and scratch are released on success or refusal.
    """
    def __init__(self, path=None, *, stream=None, primary_path=None):
        if (path is None) == (stream is None):
            raise TypeError("supply exactly one of path and stream")
        self.header, self.trailer = {}, None
        self.complete, self.sha256 = False, ""
        self._file = self._db = None
        self._failed = False
        self._io_errors = (OSError, ReplaySinkFormatError)
        self._counts, self._hash = {}, hashlib.sha256()
        self._scalars = None
        self._primary = primary_path
        try:
            self._file = stream if stream is not None else open(path, "rb")
            line = self._file.readline(MAX_LINE_BYTES + 1)
            value = _replay_parse(line)
            if (type(value) is not dict
                    or tuple(value) != ("schema", "version", "run_id", "engine", "records")
                    or value["schema"] != "pf-report-replay"
                    or type(value["version"]) is not int or value["version"] != 1
                    or type(value["run_id"]) is not str or type(value["engine"]) is not str
                    or type(value["records"]) is not list):
                raise ReplaySinkFormatError("header_invalid")
            for descriptor in value["records"]:
                if type(descriptor) is not dict or tuple(descriptor) != ("type", "fields", "types", "units"):
                    raise ReplaySinkFormatError("header_invalid")
                tag = descriptor["type"]
                if type(tag) is not str or tag not in REPLAY_COLUMNS or tag in self._counts:
                    raise ReplaySinkFormatError("header_invalid")
                for index, key in enumerate(("fields", "types", "units")):
                    if descriptor[key] != [column[index] for column in REPLAY_COLUMNS[tag]]:
                        raise ReplaySinkFormatError("header_invalid")
                self._counts[tag] = 0
            if "S" not in self._counts or (("R" in self._counts) != ("Q" in self._counts)):
                raise ReplaySinkFormatError("header_invalid")
            self.header = value
            self._hash.update(line)
            if primary_path is not None:
                import sqlite3
                self._io_errors += (sqlite3.Error,)
                self._db = sqlite3.connect("")  # private temporary disk database
                self._db.execute("PRAGMA cache_size=-256")
                self._db.execute("PRAGMA temp_store=FILE")
                self._db.execute("PRAGMA mmap_size=0")
                self._db.execute("PRAGMA journal_mode=OFF")
                self._db.executescript("""
                    CREATE TABLE rows(tag TEXT, seq INTEGER, payload BLOB, PRIMARY KEY(tag,seq));
                    CREATE TABLE base(tag TEXT, seq INTEGER, payload BLOB, PRIMARY KEY(tag,seq));
                    CREATE TABLE originals(sid INTEGER PRIMARY KEY, pnl BLOB, signature BLOB,
                                           marked INTEGER, parent TEXT);
                    CREATE TABLE prefixes(k INTEGER PRIMARY KEY, value BLOB);
                    CREATE TABLE parents(parent TEXT, slice INTEGER, slices INTEGER,
                                         fields TEXT, sid INTEGER, r BLOB, t BLOB);
                    CREATE INDEX parent_order ON parents(parent,slice);
                    CREATE TABLE children(parent TEXT, child INTEGER, sid INTEGER, qty BLOB,
                                          PRIMARY KEY(parent,child));
                    CREATE TABLE used_children(parent TEXT, child INTEGER, fee BLOB,
                                               PRIMARY KEY(parent,child));
                """)
        except FileNotFoundError as error:
            self._refuse(ReplaySinkFormatError("missing_companion", str(error)))
        except self._io_errors as error:
            self._refuse(error)

    @property
    def counts(self):
        return dict(self._counts)

    def _refuse(self, error):
        self._failed, self.complete = True, False
        self.sha256, self.trailer = "", None
        self.close()
        if not isinstance(error, ReplaySinkFormatError):
            error = ReplaySinkFormatError("sink_io", str(error))
        if self._primary is not None:
            reason = {
                "r_record_invalid": "r_coverage_invalid",
                "q_record_invalid": "q_coverage_invalid",
                "s_record_invalid": "replay_mismatch",
                "missing_s": "replay_mismatch",
                "duplicate_s": "replay_mismatch",
                "trailer_out_of_order": "missing_trailer",
                "framing_invalid": "missing_trailer",
                "json_invalid": "missing_trailer",
                "header_invalid": "missing_trailer",
                "trailer_invalid": "missing_trailer",
                "record_invalid": "missing_trailer",
            }.get(error.code, error.code)
            raise ReplaySinkFormatError(
                "exact_replay_unavailable", "exact replay unavailable: " + str(error),
                {"reason": reason}) from error
        raise error

    def __iter__(self):
        return self

    def __next__(self):
        if self.complete:
            raise StopIteration
        if self._failed:
            raise ReplaySinkFormatError("reader_failed")
        try:
            line = self._file.readline(MAX_LINE_BYTES + 1)
            if not line:
                raise ReplaySinkFormatError("missing_trailer")
            value = _replay_parse(line)
            if type(value) is dict:
                self._finish(value)
                raise StopIteration
            record = _replay_record(value, self._counts)
            tag = record.tag
            if tag == "S":
                if self._scalars is not None:
                    raise ReplaySinkFormatError("duplicate_s")
                self._scalars = record.values
            if self._counts[tag] == I53_MAX:
                raise ReplaySinkFormatError("count_mismatch")
            if self._db is not None and tag in ("R", "Q"):
                self._db.execute("INSERT INTO rows VALUES(?,?,?)",
                    (tag, self._counts[tag], line))
            self._counts[tag] += 1
            self._hash.update(line)
            return record
        except self._io_errors as error:
            self._refuse(error)

    def _finish(self, trailer):
        if self._scalars is None:
            code = "trailer_out_of_order" if self._file.read(1) else "missing_s"
            raise ReplaySinkFormatError(code)
        if (tuple(trailer) not in (
                ("trailer", "version", "counts", "sha256"),
                ("trailer", "version", "counts", "sha256", "base_sha256"))
                or trailer.get("trailer") != "pf-report-replay"
                or type(trailer.get("version")) is not int or trailer["version"] != 1):
            raise ReplaySinkFormatError("trailer_invalid")
        counts = trailer["counts"]
        if (type(counts) is not dict or set(counts) != set(self._counts)
                or any(not _replay_int(counts[tag]) or counts[tag] != self._counts[tag]
                       for tag in self._counts)):
            raise ReplaySinkFormatError("count_mismatch")
        digest = trailer["sha256"]
        if type(digest) is not str or _SHA256.fullmatch(digest) is None or digest != self._hash.hexdigest():
            raise ReplaySinkFormatError("sha256_mismatch")
        if "base_sha256" in trailer and (
                type(trailer["base_sha256"]) is not str or _SHA256.fullmatch(trailer["base_sha256"]) is None):
            raise ReplaySinkFormatError("base_sha256_mismatch")
        if self._file.read(1):
            raise ReplaySinkFormatError("trailing_bytes")
        if self._primary is not None:
            self._validate_bound(trailer)
        self.close()
        self.trailer, self.sha256, self.complete = trailer, digest, True

    def _validate_bound(self, trailer):
        db = self._db
        try:
            raw = _ReplayPrimaryStream(self._primary)
            with SinkReader(stream=raw) as primary:
                primary_parser = RecordParser(primary.header)
                counts = {"T": 0, "E": 0}
                for record in primary:
                    if record.tag in counts:
                        db.execute("INSERT INTO base VALUES(?,?,?)",
                            (record.tag, counts[record.tag], raw.line))
                        counts[record.tag] += 1
                primary_digest = primary.sha256
        except (OSError, SinkFormatError, ValueError) as error:
            raise ReplaySinkFormatError("primary_invalid", str(error)) from error
        # Foreign bindings must be refused before using their associations.
        if trailer.get("base_sha256") != primary_digest:
            raise ReplaySinkFormatError("base_sha256_mismatch")
        for tag, base_tag, reason in (("R", "T", "r_coverage_invalid"), ("Q", "E", "q_coverage_invalid")):
            if tag not in self._counts or self._counts[tag] != counts[base_tag]:
                raise ReplaySinkFormatError(reason)
        joined = db.execute(
            "SELECT r.seq,r.payload,b.payload FROM rows r JOIN base b ON b.seq=r.seq "
            "WHERE r.tag='R' AND b.tag='T' ORDER BY r.seq")
        for seq, r_text, t_text in joined:
            r, t = _replay_mapping(r_text), primary_parser.parse(t_text).as_dict()
            sid, parent = r["settlement_index"], r["parent_id"]
            if (r["n"] != seq + 1 or t["n"] != r["n"]
                    or (sid is None) != t["open_at_end"]
                    or (t["open_at_end"] and parent is not None)):
                raise ReplaySinkFormatError("r_coverage_invalid")
            if sid is None:
                continue
            marked = parent is not None
            operands = ([r[key] for key in ("whole_qty", "whole_pnl", "whole_commission",
                         "whole_max_runup", "whole_max_drawdown")] if marked else
                        [t[key] for key in ("qty", "pnl", "commission", "max_runup", "max_drawdown")])
            if not all(_replay_number(x) for x in operands):
                raise ReplaySinkFormatError("replay_mismatch")
            signature = b"".join(_replay_bits(x) for x in operands)
            if marked:
                signature += b"N" if r["whole_exit_fee"] is None else _replay_bits(r["whole_exit_fee"])
            previous = db.execute(
                "SELECT signature,marked,parent FROM originals WHERE sid=?", (sid,)).fetchone()
            if previous is None:
                db.execute("INSERT INTO originals VALUES(?,?,?,?,?)",
                    (sid, _replay_bits(operands[1]), signature, int(marked), parent))
            elif (previous[0] != signature or not marked or not previous[1] or previous[2] != parent):
                raise ReplaySinkFormatError("settlement_operand_mismatch")
            if marked:
                db.execute("INSERT INTO parents VALUES(?,?,?,?,?,?,?)",
                    (parent, r["slice_index"], r["slice_count"], json.dumps(r["recut_fields"]),
                     sid, r_text, t_text))
        for row in db.execute(
                "SELECT parent,count(*),min(slice),max(slice),count(DISTINCT slice),"
                "min(slices),max(slices),min(fields),max(fields) FROM parents GROUP BY parent"):
            _, count, first, last, distinct, lo, hi, fields_lo, fields_hi = row
            if first != 0 or last != count - 1 or distinct != count or lo != count or hi != count or fields_lo != fields_hi:
                raise ReplaySinkFormatError("parent_coverage_invalid")
        net, physical = 0.0, 0
        db.execute("INSERT INTO prefixes VALUES(0,?)", (_replay_bits(net),))
        for sid, pnl in db.execute("SELECT sid,pnl FROM originals ORDER BY sid"):
            if sid != physical:
                raise ReplaySinkFormatError("settlement_coverage_invalid")
            net = net + _replay_double(pnl)
            physical += 1
            db.execute("INSERT INTO prefixes VALUES(?,?)", (physical, _replay_bits(net)))
        capital, pointvalue, final = self._scalars
        if _replay_bits(net) != _replay_bits(final):
            raise ReplaySinkFormatError("replay_mismatch")
        for seq, q_text, e_text in db.execute(
                "SELECT q.seq,q.payload,b.payload FROM rows q JOIN base b ON b.seq=q.seq "
                "WHERE q.tag='Q' AND b.tag='E' ORDER BY q.seq"):
            q, e = _replay_mapping(q_text), primary_parser.parse(e_text).as_dict()
            if q["point_index"] != seq or q["time_ms"] != e["time_ms"]:
                raise ReplaySinkFormatError("q_coverage_invalid")
            if q["equity_basis"] == "anchor":
                if seq != 0:
                    raise ReplaySinkFormatError("q_coverage_invalid")
                expected = capital
            else:
                prefix = db.execute("SELECT value FROM prefixes WHERE k=?",
                                    (q["settlement_count"],)).fetchone()
                if prefix is None or prefix[0] != _replay_bits(q["net_profit_sum"]):
                    raise ReplaySinkFormatError("replay_mismatch")
                expected = (capital + q["equity_term"] if q["equity_basis"] == "terminal_quote"
                            else (capital + q["net_profit_sum"]) + q["equity_term"])
            if not _replay_number(e["equity"]) or _replay_bits(expected) != _replay_bits(e["equity"]):
                raise ReplaySinkFormatError("replay_mismatch")
        self._validate_slices(pointvalue, primary_parser)

    def _validate_slices(self, pointvalue, primary_parser):
        def same(a, b):
            if not _replay_number(a) or not _replay_number(b) or _replay_bits(a) != _replay_bits(b):
                raise ReplaySinkFormatError("replay_mismatch")

        def rounded(value):
            if value == 0 or not math.isfinite(value):
                return value
            scale = math.pow(10.0, 9.0 - math.floor(math.log10(abs(value))))
            result = math.floor(abs(value) * scale + 0.5) / scale
            return -result if value < 0 else result

        try:
            for (parent,) in self._db.execute("SELECT DISTINCT parent FROM parents ORDER BY parent"):
                for child, (sid, text) in enumerate(self._db.execute(
                        "SELECT sid,min(r) FROM parents WHERE parent=? GROUP BY sid ORDER BY sid", (parent,))):
                    whole = _replay_mapping(text)
                    self._db.execute("INSERT INTO children VALUES(?,?,?,?)",
                        (parent, child, sid, _replay_bits(whole["whole_qty"])))
                sources = iter(self._db.execute(
                    "SELECT sid,min(r) FROM parents WHERE parent=? GROUP BY sid ORDER BY sid", (parent,)))
                source = next(sources, None)
                left = _replay_mapping(source[1])["whole_qty"] if source else 0.0
                last_child, wanted = None, 0.0
                for r_text, t_text in self._db.execute(
                        "SELECT r,t FROM parents WHERE parent=? ORDER BY slice", (parent,)):
                    r, t = _replay_mapping(r_text), primary_parser.parse(t_text).as_dict()
                    child = r["child_index"]
                    if r["recut_fields"] == [] and child == last_child:
                        raise ReplaySinkFormatError("parent_coverage_invalid")
                    original_child = self._db.execute(
                        "SELECT sid,qty FROM children WHERE parent=? AND child=?", (parent, child)).fetchone()
                    if original_child is None or original_child[1] != _replay_bits(r["transaction_qty"]):
                        raise ReplaySinkFormatError("replay_mismatch")
                    fee_operand = b"N" if r["transaction_fee"] is None else _replay_bits(r["transaction_fee"])
                    if child != last_child:
                        if wanted > 1e-10:
                            raise ReplaySinkFormatError("replay_mismatch")
                        if self._db.execute("SELECT 1 FROM used_children WHERE parent=? AND child=?",
                                            (parent, child)).fetchone() is not None:
                            raise ReplaySinkFormatError("parent_coverage_invalid")
                        self._db.execute("INSERT INTO used_children VALUES(?,?,?)",
                                         (parent, child, fee_operand))
                        wanted, last_child = r["transaction_qty"], child
                    elif self._db.execute(
                            "SELECT fee FROM used_children WHERE parent=? AND child=?",
                            (parent, child)).fetchone()[0] != fee_operand:
                        raise ReplaySinkFormatError("replay_mismatch")
                    if r["recut_fields"] == []:
                        if original_child[0] != r["settlement_index"]:
                            raise ReplaySinkFormatError("replay_mismatch")
                        for name in ("qty", "pnl", "commission", "max_runup", "max_drawdown"):
                            same(t[name], r["whole_" + name])
                        same(r["transaction_qty"], r["whole_qty"])
                        cost = (t["entry_price"] * t["qty"]) * pointvalue
                        same(t["pnl_pct"], (t["pnl"] / cost) * 100.0 if cost > 0 else 0.0)
                        wanted = 0.0
                        continue
                    if source is None or source[0] != r["settlement_index"]:
                        raise ReplaySinkFormatError("replay_mismatch")
                    qty = min(left, wanted)
                    same(t["qty"], qty)
                    left, wanted = rounded(left - qty), rounded(wanted - qty)
                    if left <= 1e-10:
                        source = next(sources, None)
                        left = _replay_mapping(source[1])["whole_qty"] if source else 0.0
                    share = qty / r["whole_qty"]
                    fee = 0.0
                    if r["whole_exit_fee"] is not None:
                        a = rounded(max(0.0, r["whole_commission"] - r["whole_exit_fee"]) * share)
                        b = rounded((r["transaction_fee"] * qty) / r["transaction_qty"])
                        fee = rounded(a + b)
                    same(t["commission"], fee)
                    pnl = rounded((r["whole_pnl"] + r["whole_commission"]) * share) - fee
                    same(t["pnl"], pnl)
                    cost = (t["entry_price"] * qty) * pointvalue
                    same(t["pnl_pct"], (pnl / cost) * 100.0 if cost > 0 else 0.0)
                    same(t["max_runup"], r["whole_max_runup"] * share)
                    same(t["max_drawdown"], r["whole_max_drawdown"] * share)
                if wanted > 1e-10 or (r["recut_fields"] != [] and source is not None):
                    raise ReplaySinkFormatError("replay_mismatch")
        except (ArithmeticError, ValueError) as error:
            if isinstance(error, ReplaySinkFormatError):
                raise
            raise ReplaySinkFormatError("replay_mismatch", str(error)) from error

    def close(self):
        if self._file is not None:
            stream, self._file = self._file, None
            stream.close()
        if self._db is not None:
            database, self._db = self._db, None
            database.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
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
