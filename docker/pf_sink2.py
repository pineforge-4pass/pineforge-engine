"""Strict, bounded random-access reader for report sink v2, spec sections 3-11.

``open(source)`` checks framing and metadata, NOT unseen records. ``verify()``
alone grants ``complete``. Column reads say ``verified_columns``; full block
reads say ``verified_block``. Iterator records remain provisional until verify.
Every data accessor also returns ``file_verification`` alongside its value:
COMPACT is UNVERIFIED until verify(), independently of per-read integrity.
FULL open checks metadata consistency; its stream digests bind records only
after verify(). Materialized states are snapshots; fetch again after verification.
Iterators report the current file state and stamp each yielded result.
Sources supply size, immutable generation and synchronous readRange(o,n,abort).
Each call finishes before the next; iterator consumption supplies backpressure.
No background fetch/decode work survives cancellation. Remote adapters must bind
every request to the captured generation (for example with If-Match).

Memory: two FULL index pages or one <=12 KiB COMPACT table, bounded metadata,
one block per cursor, and fixed hash states. Rank verification uses a <=64 KiB
position-window bitset and one key block, without a trade-sized scratch file.
Queries may return the explicitly requested P rows/B buckets.
"""
from __future__ import annotations

import argparse
from array import array
import bisect
import builtins
from collections import OrderedDict
import copy
from dataclasses import dataclass, fields, is_dataclass, replace
from functools import wraps
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
from typing import Any, BinaryIO, Callable, Iterator, Protocol
import zlib

import pf_sink as v1
import pf_sink2_layout as L

SinkFormatError = v1.SinkFormatError


class _FileResult:
    """File-level state, separate from a column/block's integrity scope."""

    file_verification: str = "provisional"


class _ScopedDict(dict, _FileResult):
    pass


class _ScopedBytes(bytes, _FileResult):
    pass


class _ScopedTuple(tuple, _FileResult):
    pass


class _ScopedInt(int, _FileResult):
    pass


class _ScopedStr(str, _FileResult):
    pass


class _ScopedValue(_FileResult):
    """An adapter's opaque generation token, with its original value retained."""

    def __init__(self, value: Any):
        self.value = value

    def __eq__(self, other: Any) -> bool:
        return self.value == (other.value if isinstance(other, _ScopedValue) else other)

    def __hash__(self) -> int:
        return hash(self.value)


class Record(v1.Record, _FileResult):
    """The v1 record interface plus an explicit file-level state."""

    def as_dict(self) -> dict[str, Any]:
        return _with_state(super().as_dict(), self.file_verification)


class _ScopedIterator(_FileResult):
    def __init__(self, reader: SinkReader, cursor: Iterator[Any]):
        self._reader, self._cursor = reader, cursor

    @property
    def file_verification(self) -> str:
        return self._reader.verification

    def __iter__(self) -> _ScopedIterator:
        return self

    def __next__(self) -> Any:
        try:
            return _with_state(next(self._cursor), self.file_verification)
        except (SinkFormatError, OSError):
            self._reader._complete = False
            self._reader._pages.clear()
            raise

    def close(self) -> None:
        self._cursor.close()


def _with_state(value: Any, state: str) -> Any:
    """Keep collection/scalar operations and bytes unchanged; add state."""
    if isinstance(value, Record):
        result = Record(*value)
    elif is_dataclass(value) and isinstance(value, _FileResult):
        nested = {item.name: _with_state(getattr(value, item.name), state)
                  for item in fields(value)
                  if isinstance(getattr(value, item.name), (_FileResult, dict, bytes, tuple))}
        result = replace(value, **nested)
    elif isinstance(value, dict):
        result = _ScopedDict(value)
    elif isinstance(value, bytes):
        result = _ScopedBytes(value)
    elif isinstance(value, tuple):
        # Payload scalars keep their exact types (especially bool). Only
        # structured results inside a tuple need their own state.
        result = _ScopedTuple(_with_state(item, state) if isinstance(item, _FileResult) else item
                              for item in value)
    elif isinstance(value, str):
        result = _ScopedStr(value)
    elif isinstance(value, int):
        result = _ScopedInt(value)
    else:
        result = _ScopedValue(value.value if isinstance(value, _ScopedValue) else value)
    object.__setattr__(result, "file_verification", state)
    return result


def _accessor(method: Callable[..., Any]) -> Callable[..., Any]:
    @wraps(method)
    def scoped(self: SinkReader, *args: Any, **kwargs: Any) -> Any:
        self._guard()
        # Public metadata scalars retain ordinary use as query arguments.
        plain = lambda value: (int(value) if isinstance(value, _ScopedInt) else
                               str(value) if isinstance(value, _ScopedStr) else value)
        result = method(self, *(plain(value) for value in args),
                        **{key: plain(value) for key, value in kwargs.items()})
        if isinstance(result, Iterator):
            return _ScopedIterator(self, result)
        return _with_state(result, self.verification)
    return scoped


class _FileField:
    """Root/tail/HEAD metadata attributes use the same state as methods."""

    def __set_name__(self, owner: type, name: str) -> None:
        self._name = "_" + name

    def __get__(self, reader: SinkReader | None, owner: type | None = None) -> Any:
        if reader is None:
            return self
        reader._guard()
        return _with_state(getattr(reader, self._name), reader.verification)

    def __set__(self, reader: SinkReader, value: Any) -> None:
        setattr(reader, self._name, value)


class SinkCancelled(SinkFormatError):
    """The operation is incomplete; no verified whole-file receipt exists."""


class UnverifiedLogicalIdentity(SinkFormatError):
    """The declared identity has not yet been authenticated against records."""

    code = "sink_logical_identity_unverified"


class ReportResultInvalid(SinkFormatError):
    """The existing v1 report adapter's typed nonfinite refusal (21.6)."""

    code = "report_result_invalid"

    def __init__(self, field: str):
        super().__init__(f"the report result {field} is not finite")
        self.code_args = {"field": field, "reason": "nonfinite"}


class SummaryLimit(SinkFormatError):
    """A SUMMARY decoded/stored length or frame window exceeds 3.7's bound."""

    code = "summary_limit"


class RandomAccessSource(Protocol):
    size: int
    generation: Any

    def readRange(self, offset: int, length: int,
                  abortSignal: Callable[[], bool] | None = None) -> bytes: ...


class LocalSource:
    def __init__(self, path: str | Path):
        self.locator = str(path)
        self._fd = os.open(path, os.O_RDONLY)
        stat = os.fstat(self._fd)
        self.size = stat.st_size
        self._initial = self._identity(stat)

    @staticmethod
    def _identity(stat: os.stat_result) -> tuple[int, ...]:
        return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns

    @property
    def generation(self) -> tuple[int, ...]:
        return self._identity(os.fstat(self._fd))

    def readRange(self, offset: int, length: int,
                  abortSignal: Callable[[], bool] | None = None) -> bytes:
        if self.generation != self._initial:
            raise SinkFormatError("local source generation changed")
        if abortSignal and abortSignal():
            raise SinkCancelled("cancelled before range read")
        return os.pread(self._fd, length, offset)

    def close(self) -> None:
        if self._fd >= 0:
            fd, self._fd = self._fd, -1
            os.close(fd)


class BytesSource:
    def __init__(self, data: bytes, locator: str = ""):
        self._data = bytes(data)
        self.size = len(data)
        self.generation = hashlib.sha256(data).digest()
        self.locator = locator

    def readRange(self, offset: int, length: int,
                  abortSignal: Callable[[], bool] | None = None) -> bytes:
        if abortSignal and abortSignal():
            raise SinkCancelled("cancelled before range read")
        return self._data[offset:offset + length]


def _need(ok: bool, message: str) -> None:
    if not ok:
        raise SinkFormatError(message)


def _safe(value: int) -> int:
    _need(0 <= value <= L.SAFE, "unsafe length, count or offset")
    return value


def _zero(data: bytes) -> None:
    _need(not any(data), "nonzero reserved bytes or padding")


def _u32(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int = 0) -> int:
    return _safe(struct.unpack_from("<Q", data, offset)[0])


def _bits(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def _float(word: int) -> float:
    value = struct.unpack("<d", struct.pack("<Q", word))[0]
    _need(math.isfinite(value), "non-finite f64 word")
    return value


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        _need(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def _reject_constant(text: str) -> Any:
    raise SinkFormatError(f"non-JSON numeric constant {text}")


def _json_integer(text: str) -> int | float:
    value = int(text)
    _need(abs(value) <= L.SAFE, "unsafe JSON integer token")
    return -0.0 if text == "-0" else value


def _json(data: bytes, *, compact: bool = False) -> Any:
    _need(not data.startswith(b"\xef\xbb\xbf"), "JSON BOM")
    quoted = escaped = False
    depth = 0
    for byte in data:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            depth += 1
            _need(depth <= L.MAX_DEPTH, "JSON nesting exceeds 64")
        elif byte in (93, 125):
            depth -= 1
        elif compact and byte <= 32:
            raise SinkFormatError("whitespace outside compact JSON strings")
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_pairs,
                           parse_constant=_reject_constant, parse_int=_json_integer)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise SinkFormatError(f"invalid JSON/UTF-8: {error}") from error

    def check(item: Any) -> None:
        if isinstance(item, str):
            _need(not any(0xD800 <= ord(c) <= 0xDFFF for c in item), "JSON surrogate")
        elif isinstance(item, float):
            _need(math.isfinite(item), "non-finite JSON number")
        elif isinstance(item, list):
            for child in item:
                check(child)
        elif isinstance(item, dict):
            for key, child in item.items():
                check(key)
                check(child)
    check(value)
    return value


def _keys(value: Any, keys: tuple[str, ...]) -> None:
    _need(type(value) is dict and tuple(value) == keys, "JSON keys missing, extra or out of order")


def _metadata(data: bytes, trades: int) -> dict[str, Any]:
    value = _json(data, compact=True)
    _need(type(value) is dict and set(L.SUMMARY_REQUIRED) <= value.keys(),
          "summary required fields missing/object required")
    _need(not set(L.SUMMARY_FORBIDDEN) & value.keys(), "summary payload/self-reference forbidden")
    _need(type(value["engine"]) is str, "summary engine must be a string")
    _need(type(value["elapsed_seconds"]) in (int, float), "summary elapsed_seconds must be finite")
    _need(value["fingerprint"] is None or type(value["fingerprint"]) is dict,
          "summary fingerprint must be object/null")
    for key in L.SUMMARY_REQUIRED + L.SUMMARY_OPTIONAL:
        if key in value and key not in ("engine", "elapsed_seconds", "fingerprint"):
            _need(type(value[key]) is dict, "summary member must be an object: " + key)
    if "outputs" in value:
        _need(set(value["outputs"]) == set(L.OUTPUT_METADATA), "summary outputs metadata fields")
    folds = value["sink_folds"]
    _keys(folds, L.SUMMARY_FOLDS)
    _need(all(type(folds[k]) is int and 0 <= folds[k] <= L.SAFE for k in L.SUMMARY_FOLDS[:2]),
          "summary fold counts must be nonnegative safe integers")
    _need(sum(folds[k] for k in L.SUMMARY_FOLDS[:2]) == trades, "summary fold counts mismatch")
    _need(type(folds["open_at_end_pnl"]) in (int, float), "summary fold pnl must be finite")
    return value


def _quote(text: str) -> str:
    # v1 uses \u00xx for ALL controls, including LF/TAB (json.dumps differs).
    return '"' + "".join('\\"' if c == '"' else "\\\\" if c == "\\" else
                         f"\\u{ord(c):04x}" if ord(c) < 32 else c for c in text) + '"'


def _json_text(value: Any) -> str:
    # State is API evidence, never an extra serialized member or changed token.
    if isinstance(value, _ScopedDict):
        value = dict(value)
    elif isinstance(value, _ScopedStr):
        value = str(value)
    elif isinstance(value, _ScopedInt):
        value = int(value)
    if value is None:
        return "null"
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) is str:
        return _quote(value)
    if type(value) is int:
        return str(value)
    if type(value) is float:
        _need(math.isfinite(value), "non-finite JSON value")
        return repr(value)
    if type(value) is list:
        return "[" + ",".join(_json_text(v) for v in value) + "]"
    if type(value) is dict:
        return "{" + ",".join(_quote(k) + ":" + _json_text(v) for k, v in value.items()) + "}"
    raise SinkFormatError("unsupported JSON value")


def _line(value: Any) -> bytes:
    data = (_json_text(value) + "\n").encode("utf-8")
    _need(len(data) <= L.MAX_LINE, "canonical v1 line exceeds 1 MiB")
    return data


def _canonical(value: Any, wire: str) -> bytes:
    nullable = wire.endswith("?")
    if value is None:
        _need(nullable, "null in nonnullable column")
        return b"\0"
    prefix = b"\1" if nullable else b""
    kind = wire.rstrip("?")
    if kind == "i64":
        body = struct.pack("<q", value)
    elif kind == "u64":
        body = struct.pack("<Q", value)
    elif kind == "f64":
        body = struct.pack("<d", value)
    elif kind in ("bool", "order"):
        body = bytes([value]) if kind == "order" else bytes([int(value)])
    elif kind == "hex64":
        body = struct.pack("<Q", int(value, 16))
    else:
        text = value.encode("utf-8")
        body = struct.pack("<I", len(text)) + text
    return prefix + body


def _varint(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    for k in range(8):
        _need(pos < len(data), "truncated varint")
        byte = data[pos]
        pos += 1
        value |= (byte & 127) << (7 * k)
        if not byte & 128:
            _need(not (k and byte == 0) and value <= L.SAFE, "nonminimal/unsafe varint")
            return value, pos
    raise SinkFormatError("varint exceeds eight bytes")


def _checkpoint(check: Callable[[], None] | None, index: int = 0) -> None:
    if check is not None and index % 1024 == 0:
        check()


def _column_budget(wire: str, n: int) -> int:
    kind = wire.rstrip("?")
    return 3 + 6 * n if kind == "str" else n if kind in ("bool", "order") else 9 * n


def _delta(data: bytes, pos: int, m: int, unsigned: bool = False, *,
           check: Callable[[], None] | None = None) -> tuple[list[int], int]:
    _need(m > 0, "empty DELTA/DECIMAL")
    z, pos = _varint(data, pos)
    value = -(z + 1) // 2 if z & 1 else z // 2
    g, pos = _varint(data, pos)
    _need(g >= 1, "zero delta GCD")
    out = []
    for i in range(m):
        _checkpoint(check, i)
        if i:
            z, pos = _varint(data, pos)
            q = -(z + 1) // 2 if z & 1 else z // 2
            _need(abs(q) < L.DELTA_LIMIT, "delta quotient out of range")
            value += g * q  # Python integers cannot overflow.
        _need(abs(value) < L.DELTA_LIMIT and (not unsigned or value >= 0),
              "delta value out of range")
        out.append(value)
    return out, pos


def _decode(raw: bytes, n: int, wire: str, enc: int, flags: int, *,
            check: Callable[[], None] | None = None,
            charge: Callable[[int], None] | None = None) -> list[Any]:
    _checkpoint(check)
    kind = wire.rstrip("?")
    _need(enc in L.ENCODINGS[kind], "encoding not allowed for column kind")
    _need(flags in (0, 1) and (not flags or wire.endswith("?")), "invalid null flags")
    decoded_budget = _column_budget(wire, n)
    _need(decoded_budget <= L.MAX_COLUMN, "decoded column budget exceeds 16 MiB")

    def string_bytes(amount: int) -> None:
        nonlocal decoded_budget
        decoded_budget += amount
        _need(decoded_budget <= L.MAX_COLUMN, "decoded column budget exceeds 16 MiB")
        if charge is not None:
            charge(amount)

    pos = 0
    nulls = b""
    if flags:
        pos = (n + 7) // 8
        _need(len(raw) >= pos, "truncated null bitmap")
        nulls = raw[:pos]
        _need(n % 8 == 0 or nulls[-1] >> (n % 8) == 0, "unused null bits")
    m = n - sum(byte.bit_count() for byte in nulls)
    vals: list[Any]
    if enc == L.ENC_PLAIN64:
        _need(len(raw) - pos == 8 * m, "PLAIN64 length")
        vals = []
        for i, (value,) in enumerate(struct.iter_unpack("<Q" if kind == "u64" else "<q", raw[pos:])):
            _checkpoint(check, i)
            _need(abs(value) <= L.SAFE and (kind != "u64" or value >= 0),
                  "PLAIN64 outside schema integer domain")
            vals.append(value)
        pos = len(raw)
    elif enc == L.ENC_DELTA:
        vals, pos = _delta(raw, pos, m, kind == "u64", check=check)
    elif enc == L.ENC_XORSHUF:
        _need(len(raw) - pos == 8 * m, "XORSHUF length")
        vals = []
        previous = 0
        for i in range(m):
            _checkpoint(check, i)
            word = previous
            for plane in range(8):
                word ^= raw[pos + plane * m + i] << (8 * plane)
            previous = word
            vals.append(f"{word:016x}" if kind == "hex64" else _float(word))
        pos = len(raw)
    elif enc == L.ENC_DECIMAL:
        _need(pos < len(raw) and raw[pos] < len(L.POW10), "DECIMAL scale")
        k = raw[pos]
        scaled, pos = _delta(raw, pos + 1, m, check=check)
        _need(all(abs(v) < L.DECIMAL_LIMIT for v in scaled), "DECIMAL scaled domain")
        vals = [float(v) / L.POW10[k] for v in scaled]  # division, never reciprocal
    elif enc == L.ENC_BYTES:
        _need(len(raw) - pos == m, "BYTES length")
        vals = list(raw[pos:])
        if kind == "bool":
            _need(all(v in (0, 1) for v in vals), "invalid bool byte")
            vals = [bool(v) for v in vals]
        pos = len(raw)
    else:
        count = m
        if enc == L.ENC_DICT:
            count, pos = _varint(raw, pos)
            _need(count <= m, "oversized dictionary")  # distinct values in this block
        lengths = []
        for i in range(count):
            _checkpoint(check, i)
            size, pos = _varint(raw, pos)
            _need(size <= L.MAX_STRING, "string exceeds limit")
            lengths.append(size)
        _need(sum(lengths) <= len(raw) - pos, "string overruns body")
        if enc == L.ENC_PLAINSTR:
            string_bytes(sum(lengths))
        strings = []
        for i, size in enumerate(lengths):
            _checkpoint(check, i)
            try:
                strings.append(raw[pos:pos + size].decode("utf-8", "strict"))
            except UnicodeError as error:
                raise SinkFormatError("invalid string UTF-8") from error
            pos += size
        vals = strings
        if enc == L.ENC_DICT:
            _need(len(set(strings)) == len(strings), "dictionary strings are not distinct")
            vals = []
            first_seen = 0
            for i in range(m):
                _checkpoint(check, i)
                index, pos = _varint(raw, pos)
                _need(index < count and index <= first_seen, "bad dictionary index/first occurrence")
                string_bytes(lengths[index])
                if index == first_seen:
                    first_seen += 1
                vals.append(strings[index])
            _need(first_seen == count, "unused dictionary entry")
    _need(pos == len(raw), "trailing column bytes")
    if not nulls:
        return vals
    out = []
    j = 0
    for i in range(n):
        _checkpoint(check, i)
        if nulls[i >> 3] & (1 << (i & 7)):
            out.append(None)
        else:
            out.append(vals[j])
            j += 1
    return out


def _check_zstd_header(data: bytes, raw_len: int, *, window_limit: int,
                       summary: bool = False) -> tuple[int, int]:
    """Only fixed framing bytes are needed; no stored payload or codec context."""
    _need(len(data) >= 6 and data[:4] == L.ZSTD_MAGIC, "invalid zstd magic/frame")
    fhd = data[4]
    _need(fhd & 0x1B == 0, "zstd dictionary/reserved header bits")
    single = bool(fhd & 32)
    pos = 5
    window = 0
    if not single:
        wd = data[pos]
        pos += 1
        base = 1 << (10 + (wd >> 3))
        window = base + (base >> 3) * (wd & 7)
    flag = fhd >> 6
    width = (1 if single else 0, 2, 4, 8)[flag]
    _need(width > 0 and pos + width <= len(data), "zstd content size absent/truncated")
    content = int.from_bytes(data[pos:pos + width], "little") + (256 if flag == 1 else 0)
    pos += width
    _need(content == raw_len, "zstd declared content size mismatch")
    window = content if single else window
    # 5.2/9.1: magic/dictionary/FCS precede window; the first failure sets
    # the refusal type even when an adversarial header has several faults.
    if window > window_limit:
        if summary:
            raise SummaryLimit("summary zstd window exceeds limit before allocation")
        raise SinkFormatError("zstd window exceeds limit before allocation")
    return pos, fhd


def _check_zstd_frame(data: bytes, raw_len: int, *,
                      check: Callable[[], None] | None = None,
                      window_limit: int = L.MAX_COLUMN, summary: bool = False) -> None:
    """Check one whole frame after its bounded header has admitted any allocation."""
    pos, fhd = _check_zstd_header(data, raw_len, window_limit=window_limit, summary=summary)
    blocks = 0
    while True:
        _checkpoint(check, blocks)
        blocks += 1
        _need(pos + 3 <= len(data), "truncated zstd block header")
        bh = int.from_bytes(data[pos:pos + 3], "little")
        pos += 3
        kind, size = (bh >> 1) & 3, bh >> 3
        _need(kind != 3 and size <= L.MAX_ZSTD_BLOCK, "invalid zstd block type/size")
        pos += 1 if kind == 1 else size
        _need(pos <= len(data), "truncated zstd block")
        if bh & 1:
            break
    pos += 4 if fhd & 4 else 0
    _need(pos == len(data), "trailing/truncated zstd frame bytes")


def _inflate(data: bytes, raw_len: int, *, check: Callable[[], None] | None = None,
             window_limit: int = L.MAX_COLUMN, summary: bool = False) -> bytes:
    _check_zstd_frame(data, raw_len, check=check, window_limit=window_limit, summary=summary)
    _checkpoint(check)
    if os.environ.get("PF_SINK_ZSTD_LIBRARY"):
        from pf_sink2_zstd import decompress
        try:
            raw = decompress(data, raw_len)
        except (ValueError, OSError) as error:
            raise SinkFormatError(f"pinned zstd decode failed: {error}") from error
        _checkpoint(check)
        return raw
    try:
        from compression import zstd
    except ImportError:
        try:
            import zstandard
        except ImportError as error:
            raise SinkFormatError("zstd frame needs Python 3.14 compression.zstd or PyPI zstandard") from error
        try:
            # The checked, mandatory frame content size is exactly raw_len, so
            # this API allocates that size even on bindings that ignore
            # max_output_size when a frame carries its size.
            raw = zstandard.ZstdDecompressor(max_window_size=window_limit // 1024).decompress(
                data, max_output_size=raw_len, allow_extra_data=False)
        except Exception as error:
            raise SinkFormatError(f"zstd decode failed: {error}") from error
    else:
        try:
            decoder = zstd.ZstdDecompressor()
            raw = decoder.decompress(data, max_length=raw_len)
            _need(decoder.eof and not decoder.unused_data, "incomplete/multiple zstd frames")
        except Exception as error:
            raise SinkFormatError(f"zstd decode failed: {error}") from error
    _need(len(raw) == raw_len, "zstd decoded length mismatch")
    _checkpoint(check)
    return raw


@dataclass(frozen=True)
class Table(_FileResult):
    offset: int
    pages: int
    width: int
    count: int
    last_page_bytes: int
    flags: int
    digest: bytes
    headerless: bool = False

    @property
    def capacity(self) -> int:
        return _with_state(L.PAGE_PAYLOAD // self.width, self.file_verification)

    @property
    def byte_length(self) -> int:
        if self.headerless:
            length = self.width * self.count
        else:
            length = (self.pages - 1) * L.PAGE_SIZE + self.last_page_bytes if self.pages else 0
        return _with_state(length, self.file_verification)


@dataclass(frozen=True)
class Witness(_FileResult):
    ordinal: int
    time_ms: int
    mtm: float | None
    book: float | None

    def pack(self) -> bytes:
        return struct.pack(L.WITNESS, self.ordinal, self.time_ms,
                           0 if self.mtm is None else _bits(self.mtm),
                           0 if self.book is None else _bits(self.book),
                           int(self.mtm is not None) | (int(self.book is not None) << 1), 0)


@dataclass(frozen=True)
class SeriesSummary(_FileResult):
    # ID18 does not store book finite_count. None means it is not derivable
    # from that bucket's witness union; descriptor/decoded counts are exact.
    finite_count: int | None
    first: Witness | None
    minimum: Witness | None
    maximum: Witness | None
    last: Witness | None

    def pack(self) -> bytes:
        points = (self.first, self.minimum, self.maximum, self.last)
        flags = sum(1 << i for i, point in enumerate(points) if point is not None)
        return struct.pack(L.SERIES_PREFIX, self.finite_count, flags) + b"".join(
            point.pack() if point is not None else bytes(L.WITNESS_SIZE) for point in points) + bytes(
                L.SERIES_SIZE - L.SERIES_PADDING)


_EMPTY_SERIES = SeriesSummary(0, None, None, None, None)


def _point(ordinal: int, values: tuple[Any, ...]) -> Witness:
    time_ms, mtm, profit = values
    book = None if mtm is None else mtm - (0.0 if profit is None else profit)
    if book is not None and not math.isfinite(book):
        book = None
    return Witness(ordinal, time_ms, mtm, book)


def _series(points: Iterator[Witness], which: str) -> SeriesSummary:
    first = minimum = maximum = last = None
    count = 0
    for point in points:
        if point.mtm is None:
            continue  # the population is finite equity, for BOTH series
        if first is None:
            first = point
        last = point
        value = getattr(point, which)
        if value is not None:
            count += 1
            if minimum is None or value < getattr(minimum, which):
                minimum = point
            if maximum is None or value > getattr(maximum, which):
                maximum = point
    return SeriesSummary(count, first, minimum, maximum, last)


def _merge(a: SeriesSummary, b: SeriesSummary, which: str) -> SeriesSummary:
    def extreme(x: Witness | None, y: Witness | None, minimum: bool) -> Witness | None:
        if x is None:
            return y
        if y is None:
            return x
        xv, yv = getattr(x, which), getattr(y, which)
        return y if (yv < xv if minimum else yv > xv) or (yv == xv and y.ordinal < x.ordinal) else x
    return SeriesSummary(a.finite_count + b.finite_count, a.first or b.first,
                         extreme(a.minimum, b.minimum, True), extreme(a.maximum, b.maximum, False),
                         b.last or a.last)


@dataclass(frozen=True)
class Descriptor(_FileResult):
    offset: int
    length: int
    tag: str
    flags: int
    n: int
    decoded_bytes: int
    rec0: int
    a_min: int
    a_max: int
    b_min: int
    b_max: int
    digest: bytes
    mtm: SeriesSummary
    book: SeriesSummary
    finite_prefix: int | None
    wire: bytes


@dataclass
class Columns(_FileResult):
    entry: Descriptor
    columns: dict[int, list[Any]]
    verification: str = "verified_columns"

    @property
    def verified_columns(self) -> tuple[int, ...]:
        return _with_state(tuple(self.columns), self.file_verification)

    def values(self, index: int) -> tuple[Any, ...]:
        return _with_state(tuple(column[index] for column in self.columns.values()),
                           self.file_verification)


@dataclass(frozen=True)
class TradePage(_FileResult):
    rows: tuple[Record, ...]
    total: int
    page_count: int
    effective_page: int
    verification: str = "verified_blocks"
    from_row: int = 0
    to_row: int = 0


@dataclass(frozen=True)
class EquityBucket(_FileResult):
    begin: int
    end: int
    mtm: SeriesSummary
    book: SeriesSummary
    verification: str = "verified_index_summaries_and_columns"

    @property
    def points(self) -> tuple[Witness, ...]:
        selected = {point.ordinal: point for series in (self.mtm, self.book)
                    for point in (series.first, series.minimum, series.maximum, series.last)
                    if point is not None}
        return _with_state(tuple(selected[key] for key in sorted(selected)), self.file_verification)


@dataclass(frozen=True)
class ColumnLayout(_FileResult):
    codec: int
    encoding: int
    flags: int
    raw_len: int
    stored_len: int
    crc: int
    sha256: bytes
    offset: int


@dataclass(frozen=True)
class _ReportArray:
    records: Callable[[], Iterator[Any]]


def _witness(data: bytes, rec0: int, n: int) -> Witness:
    ordinal, time_ms, mtm, book, mask, reserved = struct.unpack(L.WITNESS, data)
    _need(rec0 <= ordinal < rec0 + n and abs(time_ms) <= L.SAFE,
          "witness ordinal/time outside block domain")
    _need(not reserved and not mask & ~3, "invalid witness mask/reserved")
    _need(bool(mask & 1) or mtm == 0, "invalid MTM witness bits")
    _need(bool(mask & 2) or book == 0, "invalid book witness bits")
    return Witness(ordinal, time_ms, _float(mtm) if mask & 1 else None,
                   _float(book) if mask & 2 else None)


def _summary_series(data: bytes, rec0: int, n: int, which: str, *,
                    population: bool | None = None) -> SeriesSummary:
    count, flags = struct.unpack_from(L.SERIES_PREFIX, data)
    if population is None:
        population = bool(count)
    _need(count <= n and flags == (15 if count else 9 if population else 0),
          "invalid series count/flags")
    points = []
    for i in range(4):
        start = L.SERIES_WITNESSES + i * L.WITNESS_SIZE
        body = data[start:start + L.WITNESS_SIZE]
        if flags & (1 << i):
            points.append(_witness(body, rec0, n))
        else:
            _zero(body)
            points.append(None)
    _zero(data[L.SERIES_PADDING:])
    first, minimum, maximum, last = points
    if not population:
        _need(count == 0, "nonempty series outside finite population")
        return _EMPTY_SERIES
    _need(first.mtm is not None and last.mtm is not None and first.ordinal <= last.ordinal,
          "series endpoints outside finite population")
    if count:
        _need(getattr(minimum, which) is not None and getattr(maximum, which) is not None,
              "series extreme is null")
        _need(first.ordinal <= minimum.ordinal <= last.ordinal and
              first.ordinal <= maximum.ordinal <= last.ordinal and
              getattr(minimum, which) <= getattr(maximum, which), "series extrema reversed/outside endpoints")
    return SeriesSummary(count, first, minimum, maximum, last)


def _checked(method: Callable[..., Any]) -> Callable[..., Any]:
    """Any integrity/I/O refusal revokes this reader's whole-file claim."""
    @wraps(method)
    def checked(self: SinkReader, *args: Any, **kwargs: Any) -> Any:
        try:
            return method(self, *args, **kwargs)
        except (SinkFormatError, OSError):
            self._complete = False
            self._pages.clear()
            raise
    return checked


class SinkReader:
    """Synchronous range-source reader; only verify() sets complete.

    A remote adapter must enforce conditional reads against its generation,
    including when an old object remains addressable under the same locator.
    The reader checks generation and exact byte counts before/after every read.
    Accessor results carry file_verification without changing mapping members
    or serialized bytes. Lifecycle/source-adapter controls (complete, source,
    close) do not return decoded-file data. compact is a truth-valued
    integer flag with the same state as profile.
    """

    compact = _FileField()
    profile = _FileField()
    size = _FileField()
    generation = _FileField()
    root_offset = _FileField()
    root_length = _FileField()
    data_start = _FileField()
    data_end = _FileField()
    summary_offset = _FileField()
    summary_length = _FileField()
    summary_json_length = _FileField()
    summary_stored_length = _FileField()
    nblocks = _FileField()
    finite_e_count = _FileField()
    tables = _FileField()

    def __init__(self, source: RandomAccessSource | str | Path | bytes, *,
                 abortSignal: Callable[[], bool] | None = None,
                 scratch_dir: str | Path | None = None,
                 profile_hint: str | None = None, scratch_flags: int = 2):
        self._owned = isinstance(source, (str, Path))
        self.source = (LocalSource(source) if self._owned else
                       BytesSource(source) if isinstance(source, bytes) else source)
        self._closed = False
        self._complete = False
        self._abort = abortSignal
        self._scratch_dir = scratch_dir
        self._profile_hint = profile_hint
        self._scratch_flags = scratch_flags
        self._compact_table: bytes | None = None
        self._pages: OrderedDict[tuple[int, int], bytes] = OrderedDict()
        self.size = self.source.size
        self.generation = copy.deepcopy(self.source.generation)
        try:
            _need(type(self._size) is int and 0 <= self._size <= L.SAFE, "unsafe source size")
            _need(self._generation is not None, "source has no immutable generation")
            _need(profile_hint in (None, "FULL", "COMPACT"), "invalid trusted profile hint")
            _need(type(scratch_flags) is int and scratch_flags in (0, 1, 2), "invalid local scratch_flags")
            self._open()
        except Exception:
            self.close()
            raise

    def _guard(self) -> None:
        try:
            _need(not self._closed, "reader is closed")
            if self._abort and self._abort():
                raise SinkCancelled("cancelled; whole-file verification incomplete")
            _need(self.source.size == self._size and self.source.generation == self._generation,
                  "source generation/size changed")
        except Exception:
            self._complete = False
            self._pages.clear()
            raise

    @property
    def complete(self) -> bool:
        self._guard()
        return self._complete

    @property
    def verification(self) -> str:
        if self.complete:
            return "verified_whole_file"
        return "UNVERIFIED" if getattr(self, "_compact", False) else "provisional"

    @property
    def logical_identity_state(self) -> str:
        self._guard()
        return "VERIFIED" if self._complete else "UNVERIFIED"

    @property
    @_accessor
    def logical_sha256(self) -> str:
        """FULL can expose a consistent declaration, still unbound to records."""
        return self._logical_sha256 if not self.compact or self._complete else "UNVERIFIED"

    @_accessor
    def require_logical_sha256(self) -> str:
        """For cache keys/receipts that require an authenticated identity."""
        if self.logical_identity_state != "VERIFIED":
            raise UnverifiedLogicalIdentity("logical identity is UNVERIFIED; verify() is required")
        return self._logical_sha256

    @property
    def counts_verification(self) -> str:
        # A consistent root/table is not decoded-stream authentication.
        return self.verification

    def _read(self, offset: int, length: int) -> bytes:
        self._guard()
        if isinstance(offset, _ScopedInt):
            offset = int(offset)
        if isinstance(length, _ScopedInt):
            length = int(length)
        _need(type(offset) is int and type(length) is int and
              0 <= offset <= self.size and 0 <= length <= self.size - offset,
              "out-of-file range")
        try:
            data = self.source.readRange(offset, length, self._abort)
            self._guard()
            _need(isinstance(data, (bytes, bytearray, memoryview)) and len(data) == length,
                  "short range read")
            return bytes(data)
        except OSError as error:
            self._complete = False
            raise SinkFormatError(f"range read failed: {error}") from error

    def _logical(self, streams: tuple[bytes, ...]) -> bytes:
        digest = hashlib.sha256(L.LOGICAL_DOMAIN + self._v1_header_sha + self._summary_sha)
        for tag, count, stream in zip(L.TAGS, self._counts, streams):
            digest.update(struct.pack(L.LOGICAL_ITEM, ord(tag), count))
            digest.update(stream)
        return digest.digest()

    def _open(self) -> None:
        _need(self.size >= L.HEAD_FIXED + L.COMPACT_ROOT_SIZE + L.TAIL_SIZE, "truncated file/tail")
        wanted = (L.COMPACT_ROOT_SIZE if self._profile_hint == "COMPACT" else L.ROOT_SIZE) + L.TAIL_SIZE
        suffix_offset = max(0, self.size - wanted)
        suffix = self._read(suffix_offset, self.size - suffix_offset)
        tail = suffix[-L.TAIL_SIZE:]
        _need(tail[L.TAIL_MAGIC_OFFSET:] == L.TAIL_MAGIC, "bad tail magic/extra data")
        root_offset, root_length, flags, file_bytes = struct.unpack_from(L.TAIL_PREFIX, tail)
        _safe(root_offset)
        _safe(file_bytes)
        _need(root_length in (L.COMPACT_ROOT_SIZE, L.ROOT_SIZE) and not flags & ~1, "bad tail length/flags")
        _need(file_bytes == self.size == _safe(root_offset + root_length + L.TAIL_SIZE),
              "tail size/root range mismatch")
        _need(root_offset >= suffix_offset, "root disagrees with trusted profile hint")
        root = suffix[root_offset - suffix_offset:-L.TAIL_SIZE]
        _need(len(root) == root_length, "truncated adjacent root")
        _zero(tail[L.TAIL_PADDING:L.TAIL_MAGIC_OFFSET])
        self.root_offset = root_offset
        self.root_length = root_length
        self._logical_sha256 = tail[L.TAIL_LOGICAL_SHA:L.TAIL_STORED_SHA].hex()
        self._stored_sha = tail[L.TAIL_STORED_SHA:L.TAIL_PADDING] if flags & 1 else None
        if self._stored_sha is None:
            _zero(tail[L.TAIL_STORED_SHA:L.TAIL_PADDING])
        _need(hashlib.sha256(root).digest() == tail[L.TAIL_ROOT_SHA:L.TAIL_LOGICAL_SHA],
              "root SHA-256 mismatch")
        magic, version, rflags = struct.unpack_from(L.ROOT_COMMON_PREFIX, root)
        _need(magic == L.ROOT_MAGIC and version == L.VERSION and not rflags & ~L.COMPACT_FLAG,
              "bad root magic/version/flags")
        self.compact = bool(rflags & L.COMPACT_FLAG)
        self.profile = "COMPACT" if self.compact else "FULL"
        _need(root_length == (L.COMPACT_ROOT_SIZE if self.compact else L.ROOT_SIZE),
              "root profile flag/length mismatch")
        _need(self._profile_hint is None or self.profile == self._profile_hint, "trusted profile hint mismatch")
        crc_offset = L.COMPACT_ROOT_CRC if self.compact else L.ROOT_CRC
        _need(zlib.crc32(root[:crc_offset]) == _u32(root, crc_offset), "root CRC mismatch")
        if self.compact:
            _, _, _, data_end, summary_length, nblocks = struct.unpack_from(L.COMPACT_ROOT_PREFIX, root)
            summary_offset = data_end
            counts_offset, finite_offset = L.COMPACT_COUNTS, L.COMPACT_FINITE_E
            self._summary_sha = root[L.COMPACT_SUMMARY_SHA:L.COMPACT_V1_SHA]
            self._v1_header_sha = root[L.COMPACT_V1_SHA:L.COMPACT_TABLE_OFFSET]
            self._streams = None
            _zero(root[L.COMPACT_RESERVED:L.COMPACT_ROOT_CRC])
        else:
            _, _, _, data_end, summary_offset, summary_length, nblocks = struct.unpack_from(L.ROOT_PREFIX, root)
            counts_offset, finite_offset = L.ROOT_COUNTS, L.ROOT_FINITE_E
            self._summary_sha = root[L.ROOT_SUMMARY_SHA:L.ROOT_V1_SHA]
            self._v1_header_sha = root[L.ROOT_V1_SHA:L.ROOT_FINITE_E]
            self._streams = tuple(root[L.ROOT_STREAMS + i * L.HASH_SIZE:
                                       L.ROOT_STREAMS + (i + 1) * L.HASH_SIZE]
                                  for i in range(len(L.TAGS)))
            _zero(root[L.ROOT_RESERVED:L.ROOT_TABLES])
            _zero(root[L.ROOT_PADDING:L.ROOT_CRC])
            _need(_u32(root, L.ROOT_FIGURE_BUCKETS) == L.FIGURE_BUCKETS, "invalid root figure buckets")
        self.data_end = _safe(data_end)
        self.summary_offset = _safe(summary_offset)
        self.summary_length = summary_length
        self.nblocks = nblocks
        _need(summary_offset <= root_offset - L.SUMMARY_HEADER, "missing/invalid summary range")
        # 5.2: an oversized fixed SUM2 header refuses immediately, before any
        # further read, including the frame header or a table/HEAD allocation.
        summary_header = self._read(summary_offset, L.SUMMARY_HEADER)
        magic, stored_len, json_length, crc, sflags, sha = struct.unpack(L.SUMMARY_PREFIX, summary_header)
        _need(magic == L.SUMMARY_MAGIC and not sflags, "invalid summary header")
        if json_length > L.MAX_SUMMARY or stored_len > L.MAX_SUMMARY_STORED:
            raise SummaryLimit("summary stored/decoded length exceeds limit before allocation")
        _need(json_length >= 1 and stored_len >= 1 and
              summary_length == L.SUMMARY_HEADER + stored_len, "invalid summary header length")
        summary_end = _safe(summary_offset + summary_length)
        _need(data_end == summary_offset and summary_end <= root_offset, "invalid summary range")
        # Establish the exact next-region boundary from the fixed root before
        # reading a frame header or allocating a descriptor table (5.2 step 1).
        next_region = root_offset
        if self.compact:
            next_region = _safe(_u64(root, L.COMPACT_TABLE_OFFSET))
        else:
            for i in range(L.TABLES):
                locator = L.ROOT_TABLES + i * L.LOCATOR_SIZE
                if _u64(root, locator + 16):
                    next_region = _safe(_u64(root, locator))
                    break
        _need(summary_end == next_region, "summary range does not end at the next region")
        frame_offset = summary_offset + L.SUMMARY_HEADER
        _need(stored_len >= 6, "invalid zstd magic/frame")
        prefix = self._read(frame_offset, 5)
        _need(prefix[:4] == L.ZSTD_MAGIC, "invalid zstd magic/frame")
        fhd = prefix[4]
        _need(fhd & 0x1B == 0, "zstd dictionary/reserved header bits")
        single = bool(fhd & 32)
        width = (1 if single else 0, 2, 4, 8)[fhd >> 6]
        header_length = 5 + int(not single) + width
        _need(header_length <= stored_len, "zstd content size absent/truncated")
        frame_header = prefix + self._read(frame_offset + 5, header_length - 5)
        _check_zstd_header(frame_header, json_length, window_limit=L.MAX_SUMMARY, summary=True)
        self._counts = tuple(_u64(root, counts_offset + i * L.U64_SIZE)
                             for i in range(len(L.TAGS)))
        self.finite_e_count = _u64(root, finite_offset)
        _need(self.finite_e_count <= self._counts[0], "invalid root finite E count")
        _need(_safe(sum(self._counts[:-1])) == self._counts[-1] and self._counts[4] <= 1,
              "root I/K counts mismatch")
        if self.compact:
            self._open_compact_index(root, suffix, suffix_offset)
        else:
            self._open_full_index(root)
        prefix = self._read(0, L.HEAD_PREFIX_SIZE)
        magic, head_length = struct.unpack(L.HEAD_PREFIX, prefix)
        _need(magic == L.MAGIC and 1 <= head_length <= L.MAX_HEAD, "head magic/length")
        self.data_start = L.HEAD_FIXED + head_length
        _need(self.data_start <= data_end, "head overlaps data/summary")
        remainder = self._read(L.HEAD_PREFIX_SIZE, head_length + L.CRC_SIZE)
        _need(zlib.crc32(prefix + remainder[:-L.CRC_SIZE]) ==
              _u32(remainder, head_length), "head CRC mismatch")
        head = _json(remainder[:-L.CRC_SIZE], compact=True)
        _keys(head, L.HEAD_KEYS)
        _need(_json_text(head).encode("utf-8") == remainder[:-L.CRC_SIZE],
              "noncanonical HEAD escaping/spelling")
        _need(type(head["version"]) is int and head["version"] == L.VERSION, "unknown head version")
        # 9.5: format is omitted from the v1 header hash. Its mandatory cover
        # is head_crc; a changed cap can refuse reads, not authenticate data.
        h1 = {key: value for key, value in head.items() if key != "format"}
        h1["version"] = L.V1_VERSION
        self._schema = v1._header(h1)
        for tag, descriptor in self._schema.items():
            _need(len(descriptor.fields) <= L.MAX_COLUMNS, "too many columns")
            if tag == "O":
                _need(len(descriptor.fields) - 3 <= L.MAX_POSITIONS, "too many output slots")
            if tag == "K":
                _need(len(descriptor.fields) <= L.MAX_POSITIONS, "too many constants")
        self._v1_header_line = _line(h1)
        _need(hashlib.sha256(self._v1_header_line).digest() == self._v1_header_sha,
              "canonical v1 header SHA mismatch")
        fmt = head["format"]
        _keys(fmt, L.FORMAT_KEYS)
        _need(fmt["byte_order"] == "le" and fmt["codec"] == "zstd", "unknown byte order/codec")
        _need(type(fmt["level"]) is int and -7 <= fmt["level"] <= 22, "invalid zstd level")
        _keys(fmt["block_records"], tuple(self._schema) + ("I",))
        for tag, cap in fmt["block_records"].items():
            _need(type(cap) is int and 1 <= cap <= (L.MAX_POSITIONS if tag == "I" else L.MAX_RECORDS),
                  "invalid block record cap")
        _need("K" not in self._schema or fmt["block_records"]["K"] == 1, "K cap must be one")
        _need(type(fmt["block_bytes"]) is int and
              L.MIN_BLOCK_BYTES <= fmt["block_bytes"] <= L.MAX_COLUMN, "invalid block byte cap")
        self._head = head
        for i, tag in enumerate(L.TAGS):
            _need(tag in self._schema or tag == "I" or self._counts[i] == 0,
                  "records for absent schema type")
            if tag in fmt["block_records"]:
                _need(self._counts[i] <= self.tables[i].count * fmt["block_records"][tag],
                      "counts exceed descriptor capacities")
            if not self._counts[i] and self._streams is not None:
                _need(self._streams[i] == hashlib.sha256(L.STREAM_DOMAIN + tag.encode()).digest(),
                      "empty stream digest mismatch")
        if self.compact:
            ranges = []
            for i, tag in enumerate(L.TAGS):
                position = 0
                for ordinal in range(self.tables[i].count):
                    entry = self.descriptor(tag, ordinal)
                    _need(entry.rec0 == position, "COMPACT per-type rec0/order mismatch")
                    position += entry.n
                    ranges.append((entry.offset, entry.length))
                _need(position == self._counts[i], "COMPACT per-type record count mismatch")
            cursor = self.data_start
            for offset, length in sorted(ranges):
                _need(offset == cursor, "COMPACT block tiling mismatch")
                cursor += length
            _need(cursor == self.data_end, "COMPACT data_end/tiling mismatch")
        frame = self._read(frame_offset, stored_len)
        summary_bytes = _inflate(frame, json_length, check=self._guard, window_limit=L.MAX_SUMMARY, summary=True)
        _need(zlib.crc32(summary_bytes) == crc, "summary CRC mismatch")
        _need(hashlib.sha256(summary_bytes).digest() == sha == self._summary_sha,
              "summary SHA mismatch")
        self._metadata = _metadata(summary_bytes, self._counts[1])
        self._summary_bytes = summary_bytes
        self.summary_json_length = json_length
        self.summary_stored_length = stored_len
        if self._streams is not None:
            # Consistency of declared digests only. verify() binds them to records.
            _need(self._logical(self._streams).hex() == self._logical_sha256,
                  "tail/root/HEAD/SUMMARY logical SHA consistency mismatch")

    def _open_compact_index(self, root: bytes, suffix: bytes, suffix_offset: int) -> None:
        _need(self._counts[0] < L.COMPACT_E_LIMIT and self._counts[1] < L.COMPACT_T_LIMIT and
              self.nblocks <= L.COMPACT_BLOCK_LIMIT, "COMPACT population/block ceiling")
        offset = _u64(root, L.COMPACT_TABLE_OFFSET)
        length = _u64(root, L.COMPACT_TABLE_LENGTH)
        _need(offset == self.summary_offset + self.summary_length and
              length == L.COMPACT_DESCRIPTOR_SIZE * self.nblocks <= L.COMPACT_TABLE_LIMIT and
              offset + length == self.root_offset, "COMPACT descriptor table tiling/length")
        block_counts = tuple(_u32(root, L.COMPACT_BLOCK_COUNTS + i * L.CRC_SIZE) for i in range(len(L.TAGS)))
        _need(sum(block_counts) == self.nblocks, "COMPACT block count mismatch")
        data = (suffix[offset - suffix_offset:offset - suffix_offset + length] if offset >= suffix_offset
                else self._read(offset, length))
        _need(hashlib.sha256(L.COMPACT_INDEX_DOMAIN + data).digest() ==
              root[L.COMPACT_TABLE_SHA:L.COMPACT_BLOCK_COUNTS], "COMPACT descriptor table SHA mismatch")
        self._compact_table = data
        self._compact_offset = offset
        tables = []
        cursor = offset
        for i, count in enumerate(block_counts):
            _need((count == 0) == (self._counts[i] == 0) and count <= self._counts[i],
                  "COMPACT descriptor/record count mismatch")
            tables.append(Table(cursor if count else 0, 0, L.COMPACT_DESCRIPTOR_SIZE, count, 0, 0,
                                b"", headerless=True))
            cursor += count * L.COMPACT_DESCRIPTOR_SIZE
        # Logical absence only; index_page refuses COMPACT physical-page requests.
        tables.extend(Table(0, 0, 4 if i < L.FIGURE_TABLE else L.FIGURE_ENTRY, 0, 0, 0, b"")
                      for i in range(len(L.TAGS), L.TABLES))
        self.tables = tuple(tables)

    def _open_full_index(self, root: bytes) -> None:
        tables = []
        cursor = self.summary_offset + self.summary_length
        for i in range(L.TABLES):
            offset, pages, width, count, last_bytes, tflags, digest = struct.unpack_from(
                L.LOCATOR, root, L.ROOT_TABLES + i * L.LOCATOR_SIZE)
            _safe(offset)
            _safe(count)
            expected_width = L.DESCRIPTOR_SIZE if i < len(L.TAGS) else L.FIGURE_ENTRY if i == L.FIGURE_TABLE else (
                4 if self._counts[1] <= L.U32_POSITIONS else L.U64_SIZE)
            _need(width == expected_width, "invalid table width")
            capacity = L.PAGE_PAYLOAD // width
            _need(pages == (count + capacity - 1) // capacity, "table page/count mismatch")
            if not count:
                _need(offset == pages == last_bytes == tflags == 0, "nonzero empty-table locator/PRESENT")
                _zero(digest)
            else:
                _need(tflags == L.PRESENT and last_bytes ==
                      L.PAGE_HEADER + (count - (pages - 1) * capacity) * width,
                      "table final page length/PRESENT mismatch")
                table_bytes = (pages - 1) * L.PAGE_SIZE + last_bytes
                _need(offset == cursor and _safe(offset + table_bytes) <= self.root_offset,
                      "overlapping/out-of-file table range")
                cursor += table_bytes
            if i < len(L.TAGS):
                _need(count <= L.MAX_BLOCKS and (count == 0) == (self._counts[i] == 0) and
                      count <= self._counts[i], "descriptor count/record count mismatch")
            elif i < L.RANK_TABLE_END:
                _need(count == self._counts[1], "missing trade-rank permutation")
            else:
                _need(count == min(L.FIGURE_BUCKETS, self.finite_e_count), "default figure count mismatch")
            tables.append(Table(offset, pages, width, count, last_bytes, tflags, digest))
        _need(cursor == self.root_offset and sum(t.count for t in tables[:len(L.TAGS)]) == self.nblocks,
              "index tiling/block count mismatch")
        self.tables = tuple(tables)

    @_accessor
    def counts(self, *, include_interleave: bool = False) -> dict[str, int]:
        self._guard()
        tags = tuple(self._schema) + (("I",) if include_interleave else ())
        return {tag: self._counts[L.TAGS.index(tag)] for tag in tags}

    @_accessor
    def summary(self) -> dict[str, Any]:
        self._guard()
        value = copy.deepcopy(self._metadata)
        value["report_sink"] = {
            "path": getattr(self.source, "locator", ""), "schema": L.SCHEMA,
            "version": L.VERSION, "run_id": self._head["run_id"], "counts": self.counts(),
            "sha256": self.logical_sha256, "bytes": self.size, "scratch_flags": self._scratch_flags,
            "logical_identity_state": self.logical_identity_state,
            "counts_verification": self.counts_verification,
            "file_verification": self.verification,
        }
        return value

    @_accessor
    def summary_json_bytes(self) -> bytes:
        """The exact decoded canonical JSON, without LF (no reserialization)."""
        self._guard()
        return self._summary_bytes

    @_accessor
    def summary_frame_bytes(self) -> bytes:
        """Physical frame for same-build compression probes, distinct from JSON identity."""
        return self._read(self.summary_offset + L.SUMMARY_HEADER, self.summary_stored_length)

    @_accessor
    def head(self) -> dict[str, Any]:
        self._guard()
        return dict(copy.deepcopy(self._head), summary=self.summary())

    @staticmethod
    def _table_id(table: int | str) -> int:
        if type(table) is str:
            if table == "figure":
                return L.FIGURE_TABLE
            if len(table) == 1 and table in L.TAGS:
                return L.TAGS.index(table)
            key, _, direction = table.partition(":")
            _need(key in L.RANK_KEYS and direction in ("asc", "desc"), "unknown index table")
            return len(L.TAGS) + 2 * L.RANK_KEYS.index(key) + int(direction == "desc")
        _need(type(table) is int and 0 <= table < L.TABLES, "unknown index table")
        return table

    @_accessor
    @_checked
    def index_page(self, table: int | str, page: int) -> bytes:
        self._guard()
        _need(not self.compact, "COMPACT has no physical index pages")
        table_id = self._table_id(table)
        locator = self.tables[table_id]
        _need(type(page) is int and 0 <= page < locator.pages, "index page out of range")
        key = (table_id, page)
        if key in self._pages:
            self._pages.move_to_end(key)
            return self._pages[key]
        # Make room before fetching, so cache plus incoming page is at most two.
        if len(self._pages) == 2:
            self._pages.popitem(last=False)
        length = locator.last_page_bytes if page == locator.pages - 1 else L.PAGE_SIZE
        data = self._read(locator.offset + page * L.PAGE_SIZE, length)
        magic, tid, flags, depth, reserved, number, used, width, pages = struct.unpack_from(L.PAGE_PREFIX, data)
        expected_depth = (locator.pages - 1).bit_length()
        _need(magic == L.PAGE_MAGIC and tid == table_id and not flags and not reserved and
              number == page and width == locator.width and pages == locator.pages and
              depth == expected_depth <= L.MAX_MERKLE_DEPTH and
              used == min(locator.capacity, locator.count - page * locator.capacity),
              "index page header mismatch")
        _zero(data[L.PAGE_PROOFS + depth * L.HASH_SIZE:L.PAGE_CRC])
        _need(len(data) == L.PAGE_HEADER + used * width, "short non-final/invalid final index page")
        crc = zlib.crc32(data[L.PAGE_HEADER:], zlib.crc32(data[:L.PAGE_CRC]))
        _need(crc == _u32(data, L.PAGE_CRC), "index page CRC mismatch")
        digest = hashlib.sha256(L.INDEX_DOMAIN + data[:L.PAGE_LEAF] + data[L.PAGE_HEADER:]).digest()
        _need(digest == data[L.PAGE_LEAF:L.PAGE_PROOFS], "index leaf SHA mismatch")
        for level in range(depth):
            start = L.PAGE_PROOFS + level * L.HASH_SIZE
            sibling = data[start:start + L.HASH_SIZE]
            pair = sibling + digest if page & (1 << level) else digest + sibling
            digest = hashlib.sha256(L.NODE_DOMAIN + pair).digest()
        _need(digest == locator.digest, "index Merkle proof mismatch")
        self._pages[key] = data
        return data

    def _index_entry(self, table: int, ordinal: int) -> bytes:
        self._guard()
        locator = self.tables[table]
        _need(type(ordinal) is int and 0 <= ordinal < locator.count, "index ordinal out of range")
        if self.compact:
            _need(table < len(L.TAGS) and self._compact_table is not None,
                  "COMPACT has no stored rank/figure entries")
            start = locator.offset - self._compact_offset + ordinal * L.COMPACT_DESCRIPTOR_SIZE
            return self._compact_table[start:start + L.COMPACT_DESCRIPTOR_SIZE]
        page, slot = divmod(ordinal, locator.capacity)
        data = self.index_page(table, page)
        start = L.PAGE_HEADER + slot * locator.width
        return data[start:start + locator.width]

    @_accessor
    @_checked
    def descriptor(self, tag: str, ordinal: int) -> Descriptor:
        _need(tag in L.TAGS and len(tag) == 1, "unknown descriptor type")
        data = self._index_entry(L.TAGS.index(tag), ordinal)
        offset, length, t, flags, reserved, n, decoded, rec0, amin, amax, bmin, bmax = \
            struct.unpack_from(L.DESCRIPTOR_PREFIX, data)
        _safe(offset)
        _safe(rec0)
        _need(t == ord(tag) and not reserved and not flags & ~7, "descriptor tag/flags/reserved")
        _need(tag in self._schema or tag == "I", "descriptor for absent type")
        _need(1 <= n <= self._head["format"]["block_records"][tag] and
              rec0 + n <= self._counts[L.TAGS.index(tag)] and (tag != "K" or n == 1),
              "descriptor record range/cap")
        _need(self.data_start <= offset and L.BLOCK_HEADER <= length <= L.MAX_BLOCK and
              _safe(offset + length) <= self.data_end and L.BLOCK_HEADER <= decoded <= L.MAX_BLOCK,
              "descriptor block range/decoded budget")
        for key, lo, hi, bit in zip(L.TIME_COLUMNS[tag], (amin, bmin), (amax, bmax), (1, 2)):
            if flags & bit:
                _need(key is not None and -L.SAFE <= lo <= hi <= L.SAFE, "invalid descriptor time range")
            else:
                _need(lo == hi == 0, "nonzero absent time range")
                if key is not None and tag not in ("O", "V"):
                    raise SinkFormatError("missing nonnullable time range")
        _need(bool(flags & 4) == (tag == "E" and not self.compact), "descriptor E-summary flag mismatch")
        mtm = book = _EMPTY_SERIES
        if self.compact:
            return Descriptor(offset, length, tag, flags, n, decoded, rec0, amin, amax, bmin, bmax,
                              data[L.DESCRIPTOR_SHA:L.DESCRIPTOR_MTM], mtm, book, None, data)
        if tag == "E":
            mtm = _summary_series(data[L.DESCRIPTOR_MTM:L.DESCRIPTOR_BOOK], rec0, n, "mtm")
            book = _summary_series(data[L.DESCRIPTOR_BOOK:L.DESCRIPTOR_FINITE_PREFIX], rec0, n, "book",
                                   population=bool(mtm.finite_count))
            _need(book.finite_count <= mtm.finite_count and
                  (mtm.first is None and book.first is None or
                   mtm.first is not None and book.first is not None and
                   mtm.first.pack() == book.first.pack() and mtm.last.pack() == book.last.pack()),
                  "series endpoint companion mismatch")
        else:
            _zero(data[L.DESCRIPTOR_MTM:L.DESCRIPTOR_FINITE_PREFIX])
        finite_prefix = _u64(data, L.DESCRIPTOR_FINITE_PREFIX)
        _need(finite_prefix + mtm.finite_count <= self.finite_e_count if tag == "E" else finite_prefix == 0,
              "descriptor finite_prefix outside population")
        _zero(data[L.DESCRIPTOR_PADDING:])
        return Descriptor(offset, length, tag, flags, n, decoded, rec0, amin, amax, bmin, bmax,
                          data[L.DESCRIPTOR_SHA:L.DESCRIPTOR_MTM], mtm, book, finite_prefix, data)

    @_accessor
    def entries(self, tag: str) -> Iterator[Descriptor]:
        self._guard()
        table = self._table_id(tag)
        _need(table < len(L.TAGS), "entries requires a record type")
        for ordinal in range(self.tables[table].count):
            yield self.descriptor(tag, ordinal)

    def _find_record(self, tag: str, position: int) -> tuple[int, Descriptor]:
        _need(type(position) is int and 0 <= position < self._counts[L.TAGS.index(tag)],
              "record ordinal out of range")
        lo, hi = 0, self.tables[L.TAGS.index(tag)].count
        while lo < hi:
            mid = (lo + hi) // 2
            entry = self.descriptor(tag, mid)
            if entry.rec0 + entry.n <= position:
                lo = mid + 1
            else:
                hi = mid
        entry = self.descriptor(tag, lo)
        _need(entry.rec0 <= position < entry.rec0 + entry.n, "record not covered by descriptor")
        return lo, entry

    @_accessor
    @_checked
    def block_layout(self, entry: Descriptor) -> tuple[ColumnLayout, ...]:
        """Authenticate framing and the SHA table before fetching any payload."""
        _, trusted = self._find_record(entry.tag, entry.rec0)
        _need(trusted == entry and trusted.wire == entry.wire and
              trusted.mtm.pack() == entry.mtm.pack() and trusted.book.pack() == entry.book.pack(),
              "descriptor is not bound to this index")
        header = self._read(entry.offset, L.BLOCK_HEADER)
        magic, tag, flags, reserved, ncols, n, rec0, body_len = struct.unpack_from(L.BLOCK_PREFIX, header)
        wires = ("order",) if entry.tag == "I" else self._schema[entry.tag].types
        _need(magic == L.BLOCK_MAGIC and tag == ord(entry.tag) and not flags and not reserved and
              ncols == len(wires) <= L.MAX_COLUMNS and n == entry.n and rec0 == entry.rec0 and
              L.BLOCK_HEADER + body_len == entry.length and
              L.BLOCK_HEADER + ncols * L.COLUMN_FRAMING <= entry.length, "block/header descriptor mismatch")
        minimum_budgets = [_column_budget(wire, n) for wire in wires]
        _need(all(budget <= L.MAX_COLUMN for budget in minimum_budgets),
              "decoded column budget exceeds 16 MiB")
        _need(L.BLOCK_HEADER + ncols * L.COLUMN_FRAMING + sum(minimum_budgets) <= L.MAX_BLOCK,
              "decoded block budget exceeds 32 MiB")
        framing = self._read(entry.offset + L.BLOCK_HEADER, ncols * L.COLUMN_FRAMING)
        directory = framing[:ncols * L.DIRECTORY_ENTRY]
        hashes = framing[ncols * L.DIRECTORY_ENTRY:]
        _need(zlib.crc32(framing, zlib.crc32(header[:L.BLOCK_CRC])) ==
              _u32(header, L.BLOCK_CRC), "block directory CRC mismatch")
        _need(hashlib.sha256(L.BLOCK_DOMAIN + struct.pack(
            L.BLOCK_HASH_PREFIX, ord(entry.tag), entry.rec0, entry.n) + hashes).digest() == entry.digest,
            "block SHA table mismatch")
        offset = entry.offset + L.BLOCK_HEADER + len(framing)
        decoded = L.BLOCK_HEADER + len(framing)
        result = []
        for i, wire in enumerate(wires):
            codec, enc, cflags, rsv, raw_len, stored_len, crc = struct.unpack_from(
                L.DIRECTORY, directory, i * L.DIRECTORY_ENTRY)
            _need(codec in (0, 1) and enc in L.ENCODINGS[wire.rstrip("?")] and not rsv and
                  cflags in (0, 1) and (not cflags or wire.endswith("?")), "unknown codec/encoding/flags")
            _need(raw_len <= L.MAX_COLUMN and stored_len <= entry.offset + entry.length - offset and
                  (codec != 0 or stored_len == raw_len), "column length/budget mismatch")
            decoded += raw_len
            _need(decoded <= L.MAX_BLOCK, "decoded block budget exceeds 32 MiB")
            result.append(ColumnLayout(codec, enc, cflags, raw_len, stored_len, crc,
                                       hashes[i * L.HASH_SIZE:(i + 1) * L.HASH_SIZE], offset))
            offset += stored_len
        _need(offset == entry.offset + entry.length and decoded == entry.decoded_bytes,
              "block stored/decoded lengths mismatch")
        return tuple(result)

    @_accessor
    @_checked
    def read_columns(self, entry: Descriptor, ids: list[int] | tuple[int, ...]) -> Columns:
        layout = self.block_layout(entry)
        _need(all(type(i) is int and 0 <= i < len(layout) for i in ids) and len(set(ids)) == len(ids),
              "invalid/repeated column id")
        wires = ("order",) if entry.tag == "I" else self._schema[entry.tag].types
        columns = {}
        decoded_budget = L.BLOCK_HEADER + len(wires) * L.COLUMN_FRAMING + sum(
            _column_budget(wire, entry.n) for wire in wires)
        for i in sorted(ids):
            self._guard()
            meta = layout[i]
            stored = self._read(meta.offset, meta.stored_len)
            raw = stored if meta.codec == 0 else _inflate(stored, meta.raw_len, check=self._guard)

            def charge(amount: int) -> None:
                nonlocal decoded_budget
                decoded_budget += amount
                _need(decoded_budget <= L.MAX_BLOCK, "decoded block budget exceeds 32 MiB")

            values = _decode(raw, entry.n, wires[i], meta.encoding, meta.flags, check=self._guard, charge=charge)
            prefix = L.COL_DOMAIN + struct.pack(L.COL_PREFIX, ord(entry.tag), i, entry.rec0, entry.n)
            crc = zlib.crc32(prefix)
            digest = hashlib.sha256(prefix)
            for j, value in enumerate(values):
                _checkpoint(self._guard, j)
                canonical = _canonical(value, wires[i])
                crc = zlib.crc32(canonical, crc)
                digest.update(canonical)
            _need(crc == meta.crc, "decoded column CRC mismatch")
            _need(digest.digest() == meta.sha256, "decoded column SHA mismatch")
            if entry.tag == "I":
                _need(all(chr(v) in self._schema for v in values), "interleave tag absent from header")
            if entry.tag == "T" and i == 1:
                _need(all(v in ("long", "short") for v in values), "invalid trade side")
            columns[i] = values
        result = Columns(entry, columns)
        if len(columns) == len(wires):
            for j in range(entry.n):
                self._guard()
                values = result.values(j)
                if entry.tag != "I":
                    _line([entry.tag, *values])  # X2: every exported v1 row fits.
            self._check_facts(result)
            result.verification = "verified_block"
        return result

    @_accessor
    def read_block(self, entry: Descriptor) -> Columns:
        count = 1 if entry.tag == "I" else len(self._schema[entry.tag].fields)
        return self.read_columns(entry, tuple(range(count)))

    def _check_facts(self, block: Columns) -> None:
        entry = block.entry
        bounds = []
        flags = 4 if entry.tag == "E" and not self.compact else 0
        for i, column in enumerate(L.TIME_COLUMNS[entry.tag]):
            values = [] if column is None else (v for v in block.columns[column] if v is not None)
            low = high = None
            for value in values:
                low = value if low is None else min(low, value)
                high = value if high is None else max(high, value)
            if low is not None:
                flags |= 1 << i
            bounds.extend((0 if low is None else low, 0 if high is None else high))
        _need(flags == entry.flags and tuple(bounds) ==
              (entry.a_min, entry.a_max, entry.b_min, entry.b_max), "descriptor time bounds mismatch")
        if entry.tag == "E" and not self.compact:
            for which in ("mtm", "book"):
                series = _series((_point(entry.rec0 + j, block.values(j)) for j in range(entry.n)), which)
                _need(series.pack() == getattr(entry, which).pack(), "equity witness summary mismatch")

    def _record(self, block: Columns, j: int) -> Record:
        return Record(block.entry.tag, self._schema[block.entry.tag].fields, block.values(j))

    @_accessor
    def iter_type(self, tag: str) -> Iterator[Record]:
        self._guard()
        _need(tag in self._schema, "record type absent from header")
        position = 0
        for entry in self.entries(tag):
            _need(entry.rec0 == position, "bad per-type rec0/order")
            block = self.read_block(entry)
            for j in range(entry.n):
                self._guard()
                yield self._record(block, j)
            position += entry.n
            del block
        _need(position == self._counts[L.TAGS.index(tag)], "per-type count mismatch")

    @_accessor
    def iter_merged(self) -> Iterator[Record]:
        self._guard()
        cursors = {tag: self.iter_type(tag) for tag in self._schema}
        position = 0
        try:
            for entry in self.entries("I"):
                _need(entry.rec0 == position, "bad interleave rec0/order")
                block = self.read_block(entry)
                for tag in block.columns[0]:
                    self._guard()
                    row = next(cursors[chr(tag)], None)
                    _need(row is not None, "interleave exhausted a type cursor")
                    yield row
                position += entry.n
                del block
            _need(position == self._counts[-1], "interleave count mismatch")
            for cursor in cursors.values():
                _need(next(cursor, None) is None, "interleave omitted records")
        finally:
            for cursor in cursors.values():
                cursor.close()

    @_accessor
    def blocks_for_time_range(self, tag: str, key: str, t0: int, t1: int) -> Iterator[Descriptor]:
        self._guard()
        _need(tag in self._schema and key in ("A", "B") and
              L.TIME_COLUMNS[tag][int(key == "B")] is not None, "invalid time field")
        _need(type(t0) is int and type(t1) is int and -L.SAFE <= t0 <= t1 <= L.SAFE,
              "invalid half-open time range")
        if t0 == t1:
            return
        lo, total = 0, self.tables[L.TAGS.index(tag)].count
        if tag == "E":
            hi = total
            while lo < hi:
                mid = (lo + hi) // 2
                if self.descriptor(tag, mid).a_max < t0:
                    lo = mid + 1
                else:
                    hi = mid
        for ordinal in range(lo, total):
            entry = self.descriptor(tag, ordinal)
            low, high, bit = (entry.a_min, entry.a_max, 1) if key == "A" else (entry.b_min, entry.b_max, 2)
            if tag == "E" and low >= t1:
                break
            if entry.flags & bit and high >= t0 and low < t1:
                yield entry

    @_accessor
    def iter_time_range(self, tag: str, key: str, t0: int, t1: int, *,
                        after_ordinal: int = -1) -> Iterator[Record]:
        """Ordinal filter on this generation; keep both equal-time anchors.

        Persisted resume state must include this reader's logical identity and
        generation along with tag, field, time and exclusive ordinal (11.4).
        """
        self._guard()
        _need(type(after_ordinal) is int and after_ordinal >= -1, "invalid resume ordinal")
        for entry in self.blocks_for_time_range(tag, key, t0, t1):
            block = self.read_block(entry)
            column = L.TIME_COLUMNS[tag][int(key == "B")]
            for j, value in enumerate(block.columns[column]):
                self._guard()
                if entry.rec0 + j > after_ordinal and value is not None and t0 <= value < t1:
                    yield self._record(block, j)
            del block

    @_accessor
    def trade_slice(self, first: int, last: int) -> Iterator[Record]:
        """Inclusive 1-based n interval; a request past the last row is empty."""
        self._guard()
        _need(type(first) is int and type(last) is int and 1 <= first <= last <= L.SAFE,
              "invalid inclusive trade slice")
        end = min(last, self._counts[1])
        if first > end:
            return
        ordinal, _ = self._find_record("T", first - 1)
        while ordinal < self.tables[1].count:
            entry = self.descriptor("T", ordinal)
            if entry.rec0 >= end:
                break
            block = self.read_block(entry)
            for j in range(max(0, first - 1 - entry.rec0), min(entry.n, end - entry.rec0)):
                self._guard()
                yield self._record(block, j)
            del block
            ordinal += 1

    def _rank_position(self, table: int, rank: int) -> int:
        position = int.from_bytes(self._index_entry(table, rank), "little")
        _need(position < self._counts[1], "rank position out of range")
        return position

    @staticmethod
    def _rank_key(value: int | float | None, key: str, descending: bool, position: int) -> tuple[Any, ...]:
        if value is None:
            return True, 0.0, position
        if key == "dd":
            value = -abs(value) if value != 0.0 else 0.0
        return False, -value if descending else value, position

    def _compact_ranks(self, key: str, descending: bool) -> array:
        """Packed tuples and two u32 index arrays, <=25*T bytes (11.5.1)."""
        total = self._counts[1]
        item = struct.Struct("<BdQ")  # validity, exact safe-integer/f64 key, position
        tuples = bytearray(item.size * total)
        column = L.RANK_COLUMNS[L.RANK_KEYS.index(key)]
        position = 0
        for entry in self.entries("T"):
            _need(entry.rec0 == position, "bad per-type rec0/order")
            block = self.read_columns(entry, (column,))
            for value in block.columns[column]:
                item.pack_into(tuples, position * item.size, value is not None,
                               0.0 if value is None else value, position)
                position += 1
            del block
        _need(position == total, "compact trade key count mismatch")
        source = array("I", [0]) * total
        target = array("I", [0]) * total
        _need(source.itemsize == target.itemsize == 4, "u32 rank workspace unavailable")
        for position in range(total):
            source[position] = position

        def sort_key(p: int) -> tuple[Any, ...]:
            valid, value, canonical = item.unpack_from(tuples, p * item.size)
            return self._rank_key(value if valid else None, key, descending, canonical)

        run = 1
        while run < total:
            for left in range(0, total, 2 * run):
                self._guard()
                middle, right = min(left + run, total), min(left + 2 * run, total)
                a, b = left, middle
                for out in range(left, right):
                    if a < middle and (b == right or sort_key(source[a]) <= sort_key(source[b])):
                        target[out], a = source[a], a + 1
                    else:
                        target[out], b = source[b], b + 1
            source, target = target, source
            run *= 2
        return source

    @_accessor
    def trade_rank_page(self, key: str = "n", direction: str = "asc", *,
                        page: int = 1, size: int = 100) -> TradePage:
        self._guard()
        _need(key in ("n", *L.RANK_KEYS) and direction in ("asc", "desc"), "invalid trade sort")
        _need(type(page) is int and type(size) is int and 1 <= size <= L.SAFE, "invalid trade page/size")
        total = self._counts[1]
        pages = max(1, (total + size - 1) // size)
        effective = min(max(page, 1), pages)
        start, end = (effective - 1) * size, min(effective * size, total)
        table = None if key == "n" else self._table_id(f"{key}:{direction}")
        ranks = self._compact_ranks(key, direction == "desc") if self.compact and table is not None and total else None
        requests = sorted((rank if direction == "asc" else total - 1 - rank, rank - start)
                          if table is None else (ranks[rank] if ranks is not None else
                                                  self._rank_position(table, rank), rank - start)
                          for rank in range(start, end))
        del ranks
        rows: list[Record | None] = [None] * len(requests)
        i = 0
        while i < len(requests):
            _, entry = self._find_record("T", requests[i][0])
            block = self.read_block(entry)
            while i < len(requests) and requests[i][0] < entry.rec0 + entry.n:
                position, out = requests[i]
                rows[out] = self._record(block, position - entry.rec0)
                i += 1
            del block
        _need(all(row is not None for row in rows), "unresolved trade rank")
        return TradePage(tuple(rows), total, pages, effective,
                         from_row=start + 1 if rows else 0, to_row=end if rows else 0)

    def _finite_time_bound(self, time_ms: int) -> int:
        lo, hi = 0, self.tables[0].count
        while lo < hi:
            mid = (lo + hi) // 2
            if self.descriptor("E", mid).a_max < time_ms:
                lo = mid + 1
            else:
                hi = mid
        if lo == self.tables[0].count:
            return self.finite_e_count
        entry = self.descriptor("E", lo)
        columns = self.read_columns(entry, (0, 1)).columns
        stop = bisect.bisect_left(columns[0], time_ms)
        return entry.finite_prefix + sum(value is not None for value in columns[1][:stop])

    def _find_finite(self, position: int) -> tuple[int, Descriptor]:
        _need(0 <= position < self.finite_e_count, "finite E ordinal out of range")
        lo, hi = 0, self.tables[0].count
        while lo < hi:
            mid = (lo + hi) // 2
            entry = self.descriptor("E", mid)
            if entry.finite_prefix + entry.mtm.finite_count <= position:
                lo = mid + 1
            else:
                hi = mid
        entry = self.descriptor("E", lo)
        _need(entry.finite_prefix <= position < entry.finite_prefix + entry.mtm.finite_count,
              "finite E ordinal not covered")
        return lo, entry

    def _adjust_final(self, values: tuple[Any, ...], finite_ordinal: int) -> tuple[Any, ...]:
        folds = self._metadata["sink_folds"]
        profit = 0.0 if values[2] is None else values[2]
        if (finite_ordinal == self.finite_e_count - 1 and folds["open_at_end_count"] > 0 and
                abs(profit) <= L.FORCED_CLOSE_EPSILON):
            return values[0], values[1], float(folds["open_at_end_pnl"])
        return values[0], values[1], profit

    @_accessor
    def equity_page(self, begin: int, end: int) -> Iterator[Record]:
        """Finite-equity ordinal interval [begin,end), with APP final profit."""
        self._guard()
        _need(type(begin) is int and type(end) is int and 0 <= begin <= end <= L.SAFE,
              "invalid finite E page")
        end = min(end, self.finite_e_count)
        if begin >= end:
            return
        if self.compact:
            position = 0
            for record in self.iter_type("E"):
                if record.values[1] is not None:
                    if begin <= position < end:
                        yield Record("E", record.fields, self._adjust_final(record.values, position))
                    position += 1
                    if position == end:
                        return
            _need(position >= end, "finite E page count mismatch")
            return
        ordinal, _ = self._find_finite(begin)
        while ordinal < self.tables[0].count:
            entry = self.descriptor("E", ordinal)
            if entry.finite_prefix >= end:
                break
            if entry.mtm.finite_count:
                block = self.read_block(entry)
                position = entry.finite_prefix
                for j in range(entry.n):
                    values = block.values(j)
                    if values[1] is not None:
                        if begin <= position < end:
                            yield Record("E", self._schema["E"].fields, self._adjust_final(values, position))
                        position += 1
                del block
            ordinal += 1

    @_checked
    def _figure_bucket(self, ordinal: int) -> EquityBucket:
        data = self._index_entry(L.FIGURE_TABLE, ordinal)
        count = data[0]
        _need(1 <= count <= L.FIGURE_MAX_WITNESSES, "figure witness count")
        _zero(data[1:L.FIGURE_SELECTORS])
        points = []
        for i in range(count):
            start = L.FIGURE_WITNESSES + i * L.WITNESS_SIZE
            point = _witness(data[start:start + L.WITNESS_SIZE], 0, self._counts[0])
            _need(point.mtm is not None and (not points or points[-1].ordinal < point.ordinal),
                  "figure population/order")
            points.append(point)
        _zero(data[L.FIGURE_WITNESSES + count * L.WITNESS_SIZE:])
        selectors = data[L.FIGURE_SELECTORS:L.FIGURE_WITNESSES]
        _need(all(s < count or s == L.ABSENT_SELECTOR for s in selectors), "figure selector range")
        _need(set(s for s in selectors if s != L.ABSENT_SELECTOR) == set(range(count)),
              "unreferenced figure witness")
        _need(all(s < count for s in selectors[:4]) and
              selectors[0] == selectors[4] == 0 and selectors[3] == selectors[7] == count - 1 and
              (selectors[5] == L.ABSENT_SELECTOR) == (selectors[6] == L.ABSENT_SELECTOR),
              "figure endpoints/extreme presence")
        total, buckets = self.finite_e_count, self.tables[L.FIGURE_TABLE].count
        begin, end = ordinal * total // buckets, (ordinal + 1) * total // buckets
        series = []
        for index, which in enumerate(("mtm", "book")):
            chosen = tuple(None if s == L.ABSENT_SELECTOR else points[s]
                           for s in selectors[4 * index:4 * index + 4])
            reference = _series(iter(points), which)
            _need(chosen == (reference.first, reference.minimum, reference.maximum, reference.last),
                  "figure selectors/extrema mismatch")
            finite = end - begin if index == 0 else (
                0 if chosen[1] is None else reference.finite_count if end - begin == count else None)
            series.append(SeriesSummary(finite, *chosen))
        return EquityBucket(begin, end, *series, verification="verified_index_figure")

    @staticmethod
    def _figure_wire(bucket: EquityBucket) -> bytes:
        points = bucket.points
        _need(1 <= len(points) <= L.FIGURE_MAX_WITNESSES, "derived figure witness count")
        slots = {point.ordinal: i for i, point in enumerate(points)}
        selectors = bytes(L.ABSENT_SELECTOR if point is None else slots[point.ordinal]
                          for series in (bucket.mtm, bucket.book)
                          for point in (series.first, series.minimum, series.maximum, series.last))
        return (bytes((len(points),)) + bytes(L.FIGURE_SELECTORS - 1) + selectors +
                b"".join(point.pack() for point in points) +
                bytes((L.FIGURE_MAX_WITNESSES - len(points)) * L.WITNESS_SIZE))

    @_accessor
    def equity_summary(self, buckets: int, *, t0: int | None = None,
                       t1: int | None = None) -> tuple[EquityBucket, ...]:
        """Finite-equity floor buckets; FULL's default query uses ID18 only."""
        self._guard()
        _need(type(buckets) is int and 1 <= buckets <= L.SAFE, "invalid equity bucket count")
        _need((t0 is None) == (t1 is None), "both time bounds are required")
        if not self.compact and buckets == L.FIGURE_BUCKETS and t0 is None:
            return tuple(self._figure_bucket(j) for j in range(self.tables[L.FIGURE_TABLE].count))
        if self.compact:
            values = self._compact_equity_buckets(buckets, t0=t0, t1=t1)
            if buckets == L.FIGURE_BUCKETS and t0 is None:
                # ID18 cannot expose a book population count hidden by its
                # witness union. Keep that public result identical in COMPACT.
                return tuple(self._default_bucket_counts(value) for value in values)
            return tuple(values)
        return tuple(self._equity_buckets(buckets, t0=t0, t1=t1))

    @staticmethod
    def _default_bucket_counts(bucket: EquityBucket) -> EquityBucket:
        book = bucket.book
        points = bucket.points
        count = (0 if book.minimum is None else sum(point.book is not None for point in points)
                 if len(points) == bucket.end - bucket.begin else None)
        return EquityBucket(bucket.begin, bucket.end, bucket.mtm,
                            SeriesSummary(count, book.first, book.minimum, book.maximum, book.last),
                            bucket.verification)

    def _compact_equity_buckets(self, buckets: int, *, t0: int | None = None,
                               t1: int | None = None) -> Iterator[EquityBucket]:
        begin, end = 0, self.finite_e_count
        if t0 is not None:
            _need(type(t0) is int and type(t1) is int and -L.SAFE <= t0 <= t1 <= L.SAFE,
                  "invalid equity time range")
            begin = end = 0
            for entry in self.entries("E"):
                block = self.read_columns(entry, (0, 1))
                for time_ms, equity in zip(block.columns[0], block.columns[1]):
                    if equity is not None:
                        begin += int(time_ms < t0)
                        end += int(time_ms < t1)
                del block
        n = end - begin
        if not n:
            return
        count = min(buckets, n)
        j, position, left = 0, 0, begin
        right = begin + n // count
        mtm = book = _EMPTY_SERIES
        for entry in self.entries("E"):
            block = self.read_block(entry)
            for local in range(entry.n):
                self._guard()
                values = block.values(local)
                if values[1] is None:
                    continue
                if begin <= position < end:
                    point = _point(entry.rec0 + local, self._adjust_final(values, position))
                    mtm = _merge(mtm, _series(iter((point,)), "mtm"), "mtm")
                    book = _merge(book, _series(iter((point,)), "book"), "book")
                    if position + 1 == right:
                        yield EquityBucket(left, right, mtm, book, "verified_columns")
                        j, left = j + 1, right
                        right = begin + (j + 1) * n // count
                        mtm = book = _EMPTY_SERIES
                position += 1
                if position == end:
                    return
            del block
        _need(position == self.finite_e_count, "COMPACT finite E count mismatch")

    def _equity_buckets(self, buckets: int, *, t0: int | None = None,
                        t1: int | None = None) -> Iterator[EquityBucket]:
        """Reconstruct independently of ID18; one decoded block at a time."""
        begin, end = 0, self.finite_e_count
        if t0 is not None:
            _need(type(t0) is int and type(t1) is int and -L.SAFE <= t0 <= t1 <= L.SAFE,
                  "invalid equity time range")
            begin, end = self._finite_time_bound(t0), self._finite_time_bound(t1)
        n = end - begin
        if not n:
            return
        count = min(buckets, n)
        ordinal, _ = self._find_finite(begin)
        j = 0
        left = begin
        right = begin + n // count
        mtm = book = _EMPTY_SERIES
        while left < end:
            entry = self.descriptor("E", ordinal)
            block = None
            finite_end = entry.finite_prefix + entry.mtm.finite_count
            pos = max(begin, entry.finite_prefix)
            stop = min(end, finite_end)
            # A positive open count may require replacing a former extreme.
            adjust = (finite_end == self.finite_e_count and entry.mtm.finite_count > 0 and
                      self._metadata["sink_folds"]["open_at_end_count"] > 0)
            points = None
            while pos < stop:
                self._guard()
                cut = min(stop, right)
                if pos == entry.finite_prefix and cut == finite_end and not adjust:
                    a, b = entry.mtm, entry.book
                else:
                    if block is None:
                        block = self.read_block(entry)
                        points = []
                        finite_position = entry.finite_prefix
                        for local in range(entry.n):
                            values = block.values(local)
                            if values[1] is not None:
                                points.append(_point(entry.rec0 + local, self._adjust_final(values, finite_position)))
                                finite_position += 1
                    start, finish = pos - entry.finite_prefix, cut - entry.finite_prefix
                    a = _series(iter(points[start:finish]), "mtm")
                    b = _series(iter(points[start:finish]), "book")
                mtm, book = _merge(mtm, a, "mtm"), _merge(book, b, "book")
                pos = cut
                if pos == right:
                    yield EquityBucket(left, right, mtm, book)
                    j += 1
                    left = right
                    right = begin + (j + 1) * n // count
                    mtm = book = _EMPTY_SERIES
            del block
            del points
            ordinal += 1

    def _verify_rank(self, table_id: int) -> None:
        key_index, descending = divmod(table_id - len(L.TAGS), 2)
        key, column = L.RANK_KEYS[key_index], L.RANK_COLUMNS[key_index]
        total = self._counts[1]
        block = None
        seen = bytearray(min(L.PAGE_SIZE, (total + 7) // 8))
        # Exactly the fixed position-window algorithm required by 9.6.
        for begin in range(0, total, 8 * L.PAGE_SIZE):
            end = min(total, begin + 8 * L.PAGE_SIZE)
            if begin:
                for byte in range(len(seen)):
                    seen[byte] = 0
            found, previous = 0, None
            for rank in range(total):
                self._guard()
                position = self._rank_position(table_id, rank)
                if begin <= position < end:
                    local = position - begin
                    byte, bit = divmod(local, 8)
                    _need(not seen[byte] & (1 << bit), "duplicate rank position")
                    seen[byte] |= 1 << bit
                    found += 1
                if begin == 0:
                    if block is None or not block.entry.rec0 <= position < block.entry.rec0 + block.entry.n:
                        del block
                        _, entry = self._find_record("T", position)
                        block = self.read_columns(entry, (column,))
                    value = block.columns[column][position - block.entry.rec0]
                    current = self._rank_key(value, key, bool(descending), position)
                    _need(previous is None or previous < current, "invalid rank permutation/comparator")
                    previous = current
            _need(found == end - begin, "missing rank position")
        del block

    @_accessor
    @_checked
    def verify(self) -> dict[str, Any]:
        """Full physical/logical verification, with fixed-size rank workspace."""
        self._complete = False
        self._guard()
        streams = [hashlib.sha256(L.STREAM_DOMAIN + tag.encode()) for tag in L.TAGS]
        counts = [0] * len(L.TAGS)
        descriptors = [0] * len(L.TAGS)
        multiplicities = {tag: 0 for tag in self._schema}
        previous_e = None
        finite_e = closed = opened = 0
        open_pnl = 0.0
        cursor = self.data_start
        while cursor < self.data_end:
            header = self._read(cursor, L.BLOCK_HEADER)
            tag = chr(header[L.BLOCK_TAG])
            _need(tag in L.TAGS, "unknown physical block tag")
            i = L.TAGS.index(tag)
            entry = self.descriptor(tag, descriptors[i])
            _need(entry.offset == cursor and entry.rec0 == counts[i], "block tiling/rec0/order mismatch")
            if tag == "E" and not self.compact:
                _need(entry.finite_prefix == finite_e, "finite_prefix continuity mismatch")
            block = self.read_block(entry)
            wires = ("order",) if tag == "I" else self._schema[tag].types
            for j in range(entry.n):
                self._guard()
                values = block.values(j)
                for value, wire in zip(values, wires):
                    streams[i].update(_canonical(value, wire))
                if tag == "E":
                    _need(previous_e is None or values[0] >= previous_e, "E time decreases")
                    previous_e = values[0]
                    finite_e += int(values[1] is not None)
                elif tag == "T":
                    _need(values[0] == counts[i] + j + 1, "T.n is not canonical position + 1")
                    if values[14]:
                        opened += 1
                        if values[7] is not None:
                            open_pnl = open_pnl + values[7]
                            _need(math.isfinite(open_pnl), "nonfinite open_at_end pnl fold")
                    else:
                        closed += 1
                elif tag == "O":
                    _need(values[0] == counts[i] + j, "O.row is not canonical position")
                elif tag == "I":
                    multiplicities[chr(values[0])] += 1
            counts[i] += entry.n
            descriptors[i] += 1
            cursor += entry.length
            del block
        _need(cursor == self.data_end and tuple(counts) == self._counts and
              sum(descriptors) == self.nblocks, "physical counts/tiling mismatch")
        _need(all(descriptors[i] == self.tables[i].count for i in range(len(L.TAGS))),
              "unmatched descriptor")
        _need(multiplicities == self.counts(), "interleave multiplicities mismatch")
        _need(finite_e == self.finite_e_count, "root finite_e_count mismatch")
        folds = self._metadata["sink_folds"]
        _need(closed == folds["closed_count"] and opened == folds["open_at_end_count"] and
              _bits(open_pnl) == _bits(float(folds["open_at_end_pnl"])), "sink_folds mismatch")
        if not self.compact:
            for table_id, table in enumerate(self.tables):
                for page in range(table.pages):
                    self.index_page(table_id, page)
            for table_id in range(len(L.TAGS), L.RANK_TABLE_END):
                self._verify_rank(table_id)
            # Reconstruct without consulting any ID18 witnesses.
            for ordinal, bucket in enumerate(self._equity_buckets(L.FIGURE_BUCKETS)):
                _need(self._figure_wire(bucket) == self._index_entry(L.FIGURE_TABLE, ordinal),
                      "default figure table mismatch")
        digests = tuple(stream.digest() for stream in streams)
        if self._streams is not None:
            _need(digests == self._streams, "stream SHA mismatch")
        _need(self._logical(digests).hex() == self._logical_sha256, "decoded logical SHA mismatch")
        if self._stored_sha is not None:
            digest = hashlib.sha256()
            for offset in range(0, self.size - L.TAIL_SIZE, L.PAGE_SIZE):
                digest.update(self._read(offset, min(L.PAGE_SIZE, self.size - L.TAIL_SIZE - offset)))
            _need(digest.digest() == self._stored_sha, "stored-prefix SHA mismatch")
        self._guard()
        self._complete = True
        return {"complete": True, "verification": "verified_whole_file", "counts": self.counts(),
                "sha256": self.logical_sha256, "bytes": self.size}

    def _v1_lines(self) -> Iterator[bytes]:
        if not self.complete:
            self.verify()
        yield self._v1_header_line
        for record in self.iter_merged():
            yield _line([record.tag, *record.values])
        self._guard()

    @_accessor
    def content_sha256(self) -> str:
        """v1 trailer digest; excludes the trailer, unlike a served-file SHA."""
        digest = hashlib.sha256()
        for line in self._v1_lines():
            digest.update(line)
        return digest.hexdigest()

    def _report_column(self, tag: str, column: int, encoding: Any = None) -> Iterator[Any]:
        for entry in self.entries(tag):
            block = self.read_columns(entry, (column,))
            for value in block.columns[column]:
                self._guard()
                if value is not None and encoding == "rgba-u32":
                    value = int(value)  # v1 truncates toward zero, including signed values.
                yield value
            del block

    @staticmethod
    def _manifest_entries(manifest: dict[str, Any], key: str, fields: tuple[str, ...]) -> list[dict[str, Any]]:
        entries = manifest.get(key, [])
        _need(type(entries) is list and all(type(entry) is dict and all(f in entry for f in fields)
                                          for entry in entries), "invalid outputs manifest list: " + key)
        for entry in entries:
            for field in fields:
                value = entry[field]
                valid = (type(value) is int and value >= 0 if field in ("index", "slot")
                         else isinstance(value, str) and bool(value))
                _need(valid, "invalid outputs manifest " + key + "." + field)
        return entries

    def _report_outputs(self, saved: dict[str, Any]) -> dict[str, Any]:
        """Inverse of run_json.py:3710-3773,5512-5547,5592-5632 (spec 21.6)."""
        _need(all(tag in self._schema for tag in "OVK") and self._counts[4] == 1,
              "report outputs require O/V/K schemas and one K row")
        manifest = saved["manifest"]
        _need(type(manifest) is dict and saved["schema_version"] == L.OUTPUT_SCHEMA,
              "invalid report outputs schema/manifest")
        _need(saved["message_format"] == manifest.get("message_format", L.OUTPUT_MESSAGE_FORMAT),
              "report outputs message_format mismatch")
        sha = saved["manifest_sha256"]
        _need(type(sha) is str and len(sha) == 64 and all(c in "0123456789abcdef" for c in sha),
              "invalid saved raw-manifest SHA")
        outputs = self._manifest_entries(manifest, "outputs", ("index", "id"))
        series = self._manifest_entries(manifest, "series", ("slot", "output"))
        constants_meta = self._manifest_entries(manifest, "constants", ("index",))
        output_map = {entry["index"]: entry for entry in outputs}  # last entry wins, as v1
        encodings = {entry["index"]: entry.get("encoding") for entry in constants_meta}
        constants = []
        for constant_row in self.iter_type("K"):
            constants = [None if value is None else int(value) if encodings.get(i) == "rgba-u32" else value
                         for i, value in enumerate(constant_row.values)]
        hlines = []
        for entry in outputs:
            if entry.get("kind") == "hline":
                price = entry.get("price")
                index = price.get("constant") if isinstance(price, dict) else None
                # Deliberately includes bool, matching this particular v1 check.
                hlines.append({"output": entry.get("id"),
                               "price": constants[index] if isinstance(index, int) and
                               0 <= index < len(constants) else None})
        spelling = lambda value: json.dumps(value, separators=(",", ":"), allow_nan=False)
        _need(spelling(constants) == spelling(saved["constants"]) and spelling(hlines) == spelling(saved["hlines"]),
              "saved output constants/hlines differ from K/manifest")
        slots = len(self._schema["O"].fields) - 3
        series_objects = []
        for entry in sorted(series, key=lambda item: item["slot"]):
            slot, encoding = entry["slot"], entry.get("encoding")
            _need(0 <= slot < slots, "outputs manifest names an unrecorded slot")
            series_objects.append({"slot": slot, "output": entry["output"],
                                   "values": _ReportArray(lambda slot=slot, encoding=encoding:
                                                          self._report_column("O", 3 + slot, encoding))})

        def events() -> Iterator[dict[str, Any]]:
            for record in self.iter_type("V"):
                event = record.as_dict()
                index = event["output_index"]
                _need(index in output_map, "outputs manifest lists no output named by an event")
                output = output_map[index]
                phase = event["phase"]
                row = {"sequence": event["sequence"], "output": output.get("id"),
                       "bar_index": event["bar_index"], "bar_open_ms": event["bar_open_ms"],
                       "bar_close_ms": event["bar_close_ms"], "ordinal_in_bar": event["ordinal_in_bar"],
                       "phase": L.OUTPUT_PHASES.get(phase, str(phase)), "value": event["value"],
                       "message": event["message"]}
                if output.get("kind") == "alert":
                    row["freq"] = output.get("freq")
                yield row

        return {"schema_version": saved["schema_version"], "message_format": saved["message_format"],
                "manifest_sha256": sha, "manifest": manifest,
                "bars": {"open_ms": _ReportArray(lambda: self._report_column("O", 1)),
                         "close_ms": _ReportArray(lambda: self._report_column("O", 2))},
                "series": series_objects, "constants": constants, "hlines": hlines,
                "events": _ReportArray(events)}

    def _report_document(self) -> dict[str, Any]:
        """Captured metadata only; never recompute summary/metrics/timing."""
        if not self.complete:
            self.verify()
        # Match run_json.py:3919-3955,5458-5509, including first-field refusal.
        for record in self.iter_type("T"):
            for column in range(4, 12):
                value = record.values[column]
                if value is None or not math.isfinite(value):
                    raise ReportResultInvalid("trades." + L.REPORT_TRADE_FIELDS[column])
        for record in self.iter_type("E"):
            for column, name in ((1, "equity"), (2, "open_profit")):
                if record.values[column] is None:
                    raise ReportResultInvalid("equity_curve." + name)
        meta = self._metadata
        _need(set(L.REPORT_SUMMARY_FIELDS) <= meta["summary"].keys() and
              set(L.REPORT_DIAGNOSTICS) <= meta["diagnostics"].keys() and
              set(L.REPORT_INPUT_FIELDS) <= meta["input"].keys() and
              {"all", "longs", "shorts", "equity"} <= meta["metrics"].keys(),
              "missing complete v1 report metadata; framing-only SUMMARY cannot invent a report")
        for key in L.REPORT_SUMMARY_FINITE:
            value = meta["summary"][key]
            if value is None or type(value) not in (int, float) or not math.isfinite(value):
                raise ReportResultInvalid("summary." + key)
        out = {}
        for key, value in meta.items():
            if key == "sink_folds":
                continue
            out[key] = self._report_outputs(value) if key == "outputs" else value
            if key == "diagnostics":
                out["trades"] = _ReportArray(lambda: (
                    dict(zip(L.REPORT_TRADE_FIELDS, row.values[:16])) for row in self.iter_type("T")))
            elif key == "metrics":
                out["equity_curve"] = _ReportArray(lambda: (row.as_dict() for row in self.iter_type("E")))
        return out

    def _report_chunks(self, document: Any) -> Iterator[bytes]:
        self._guard()
        if isinstance(document, _ReportArray):
            yield b"["
            first = True
            for item in document.records():
                if not first:
                    yield b","
                yield from self._report_chunks(item)
                first = False
            yield b"]"
        elif isinstance(document, dict):
            yield b"{"
            for index, (key, value) in enumerate(document.items()):
                if index:
                    yield b","
                yield json.dumps(key, ensure_ascii=True).encode("ascii")
                yield b":"
                yield from self._report_chunks(value)
            yield b"}"
        elif isinstance(document, (list, tuple)):
            yield b"["
            for index, value in enumerate(document):
                if index:
                    yield b","
                yield from self._report_chunks(value)
            yield b"]"
        else:
            _need(type(document) is not int or abs(document) <= L.SAFE, "unsafe report JSON integer")
            try:
                yield json.dumps(document, ensure_ascii=True, sort_keys=False,
                                 separators=(",", ":"), allow_nan=False).encode("ascii")
            except (ValueError, TypeError, UnicodeError) as error:
                raise SinkFormatError("invalid report JSON scalar") from error

    def _report_pass(self, document: dict[str, Any], output: BinaryIO | None = None) -> dict[str, Any]:
        digest, size = hashlib.sha256(), 0

        def consume(data: bytes) -> None:
            nonlocal size
            self._guard()
            digest.update(data)
            size += len(data)
            if output is not None:
                view = memoryview(data)
                while view:
                    self._guard()
                    count = output.write(view)
                    _need(type(count) is int and 0 < count <= len(view), "short/failed report JSON write")
                    view = view[count:]

        for chunk in self._report_chunks(document):
            consume(chunk)
        consume(b"\n")  # J_report, content length and digest include exactly this LF.
        self._guard()
        return {"bytes": size, "content_length": size, "report_sha256": digest.hexdigest()}

    @_accessor
    @_checked
    def report_json_identity(self) -> dict[str, Any]:
        """Count before headers: content_length -> Content-Length;
        report_sha256 -> x-report-sha256. Both cover the terminal LF.
        """
        return self._report_pass(self._report_document())

    def _check_destination(self, destination: str | Path | BinaryIO) -> bool:
        owned = isinstance(destination, (str, Path))
        if owned and isinstance(self.source, LocalSource):
            source_stat = os.fstat(self.source._fd)
            target_stat = os.stat(destination) if os.path.exists(destination) else None
            same_inode = target_stat is not None and (source_stat.st_dev, source_stat.st_ino) == (
                target_stat.st_dev, target_stat.st_ino)
            _need(not same_inode and os.path.realpath(destination) != os.path.realpath(self.source.locator),
                  "conversion output would overwrite the source")
        return owned

    @_accessor
    @_checked
    def to_report_json(self, destination: str | Path | BinaryIO) -> dict[str, Any]:
        """Spec 21.6: count/validate first, then emit identical v1 report bytes."""
        document = self._report_document()
        expected = self._report_pass(document)
        owned = self._check_destination(destination)
        self._guard()
        output = builtins.open(destination, "wb") if owned else destination
        try:
            actual = self._report_pass(document, output)
            _need(actual == expected, "report JSON changed between counting and emitting")
            return actual
        finally:
            if owned:
                output.close()

    @_accessor
    def to_v1(self, destination: str | Path | BinaryIO) -> dict[str, Any]:
        if not self.complete:
            self.verify()
        owned = self._check_destination(destination)
        output = builtins.open(destination, "wb") if owned else destination
        digest = hashlib.sha256()
        size = 0

        def write(data: bytes) -> None:
            nonlocal size
            view = memoryview(data)
            while view:
                self._guard()
                n = output.write(view)
                _need(type(n) is int and 0 < n <= len(view), "short/failed v1 export write")
                size += n
                view = view[n:]

        try:
            for line in self._v1_lines():
                digest.update(line)
                write(line)
            trailer = {"trailer": L.SCHEMA, "version": L.V1_VERSION,
                       "counts": self.counts(), "sha256": digest.hexdigest()}
            self._guard()
            write(_line(trailer))  # only after immutable input verification
            return {"v1_sha256": digest.hexdigest(), "bytes": size}
        finally:
            if owned:
                output.close()

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._pages.clear()
            self._compact_table = None
            if self._owned:
                self.source.close()

    def __enter__(self) -> SinkReader:
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()


def open(source: RandomAccessSource | str | Path | bytes, **kwargs: Any) -> SinkReader:
    return SinkReader(source, **kwargs)


def to_report_json(source: RandomAccessSource | str | Path | bytes,
                   destination: str | Path | BinaryIO, **kwargs: Any) -> dict[str, Any]:
    with open(source, **kwargs) as reader:
        return reader.to_report_json(destination)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("verify", "head", "summary", "content-sha256"):
        command = commands.add_parser(name)
        command.add_argument("source", type=Path)
    for name in ("to-v1", "to-report-json"):
        convert = commands.add_parser(name)
        convert.add_argument("source", type=Path)
        convert.add_argument("destination", type=Path)
    encode = commands.add_parser("from-v1", help="independent test/tool encoder")
    encode.add_argument("source", type=Path)
    encode.add_argument("destination", type=Path)
    encode.add_argument("--summary-json", type=Path, required=True)
    encode.add_argument("--level", type=int, default=3)
    encode.add_argument("--block-records", default="")
    encode.add_argument("--stored", action="store_true", help="stored data-column fixtures; SUMMARY still uses zstd")
    encode.add_argument("--force-full", action="store_true", help="FULL layout below canonical thresholds")
    args = parser.parse_args(argv)
    try:
        if args.command == "from-v1":
            # The reference encoder imports layout literals and v1, NEVER this reader.
            from pf_sink2_reference import from_v1, parse_caps
            result = from_v1(args.source, args.destination, args.summary_json.read_bytes(),
                             level=args.level, block_records=parse_caps(args.block_records),
                             compress=not args.stored, force_full=args.force_full)
        else:
            with open(args.source) as reader:
                if args.command == "to-v1":
                    result = reader.to_v1(args.destination)
                elif args.command == "to-report-json":
                    result = reader.to_report_json(args.destination)
                elif args.command == "content-sha256":
                    result = {"v1_sha256": reader.content_sha256()}
                else:
                    result = getattr(reader, args.command)()
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    except ReportResultInvalid as error:
        print(json.dumps({"engine": "pineforge", "error": str(error), "code": error.code,
                          "args": error.code_args}, separators=(",", ":"), allow_nan=False), file=sys.stderr)
        return 1
    except (SinkFormatError, OSError) as error:
        print(f"invalid report sink: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
