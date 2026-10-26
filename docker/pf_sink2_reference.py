"""Independent sink-v2 reference ENCODER for tests and offline tools.

This module must NEVER import pf_sink2 or its decoding/checksum/summary logic.
Only published literals in pf_sink2_layout and the frozen v1 schema/reader are
shared. Builders are capped; descriptors and Merkle levels use scratch files.
Five trade keys are sorted on disk by stdlib SQLite with a bounded page cache.
This deliberately simple implementation is not the engine's BlockWriter.
"""
from __future__ import annotations

from contextlib import ExitStack
import hashlib
import io
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import tempfile
from typing import Any, BinaryIO, Callable, Iterable
import zlib

import pf_sink as v1
import pf_sink2_layout as L

SinkFormatError = v1.SinkFormatError
FrameFactory = Callable[[str, int, bytes], bytes | None]
ColumnFixture = Callable[[str, int, int, int, bytes, list[Any]],
                         tuple[int, int, bytes, list[Any]] | None]


class SummaryLimit(SinkFormatError):
    """Independent reference writer's typed SUMMARY admission refusal."""

    code = "summary_limit"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SinkFormatError(message)


def text_json(value: Any) -> str:
    """v1 escaping and Python-repr numbers, independently implemented."""
    if isinstance(value, str):
        escapes = {34: '\\"', 92: "\\\\"}
        pieces = ['"']
        for char in value:
            code = ord(char)
            require(not 0xD800 <= code <= 0xDFFF, "reference input has a surrogate")
            pieces.append(escapes.get(code, "\\u%04x" % code if code < 32 else char))
        pieces.append('"')
        return "".join(pieces)
    if value is None:
        return "null"
    if type(value) is bool:
        return str(value).lower()
    if type(value) in (int, float):
        require(type(value) is int or math.isfinite(value), "nonfinite metadata")
        return repr(value)
    if type(value) in (tuple, list):
        return "[" + ",".join(map(text_json, value)) + "]"
    if type(value) is dict:
        require(all(type(key) is str for key in value), "JSON keys must be strings")
        return "{" + ",".join(text_json(key) + ":" + text_json(item) for key, item in value.items()) + "}"
    raise SinkFormatError("unsupported reference JSON value")


def text_line(value: Any) -> bytes:
    data = (text_json(value) + "\n").encode("utf-8")
    require(len(data) <= L.MAX_LINE, "v1 line exceeds 1 MiB")
    return data


def sanitize_utf8(value: bytes) -> str:
    """One replacement per byte that cannot start a complete valid sequence."""
    chars = []
    pos = 0
    while pos < len(value):
        lead = value[pos]
        width = (1 if lead < 0x80 else 2 if 0xC2 <= lead <= 0xDF else
                 3 if 0xE0 <= lead <= 0xEF else 4 if 0xF0 <= lead <= 0xF4 else 0)
        try:
            require(width > 0 and pos + width <= len(value), "bad UTF-8 input")
            char = value[pos:pos + width].decode("utf-8", "strict")
        except (UnicodeError, SinkFormatError):
            char, width = "\ufffd", 1
        chars.append(char)
        pos += width
    return "".join(chars)


def normalize_row(tag: str, values: Iterable[Any], wires: tuple[str, ...]) -> tuple[Any, ...]:
    values = tuple(values)
    require(len(values) == len(wires), "reference record width differs from header")
    result = []
    for value, wire in zip(values, wires):
        kind = wire.removesuffix("?")
        if kind == "f64" and value is not None:
            require(type(value) in (int, float), "reference f64 requires a number")
            try:
                value = float(value)
            except OverflowError as error:
                raise SinkFormatError("reference f64 outside binary64") from error
            if not math.isfinite(value):
                value = None
        if tag in ("O", "V") and wire == "i64?" and value == L.INT64_MIN:
            value = None  # v1's absent-clock sentinel
        if value is None:
            require(wire.endswith("?"), "reference null in nonnullable field")
        elif kind in ("i64", "u64"):
            require(type(value) is int and abs(value) <= L.SAFE and (kind != "u64" or value >= 0),
                    "reference integer outside v1 safe domain")
        elif kind == "bool":
            require(type(value) is bool, "reference bool requires bool")
        elif kind in ("str", "hex64"):
            if kind == "hex64" and type(value) is int:
                require(0 <= value <= L.UINT64_MAX, "reference hex64 word outside u64")
                value = format(value, "016x")
            if isinstance(value, bytes) and kind == "str":
                value = sanitize_utf8(value)
            require(type(value) is str and not any(0xD800 <= ord(c) <= 0xDFFF for c in value),
                    "reference string is not Unicode scalar text")
            require(len(value.encode("utf-8")) <= L.MAX_STRING, "reference string exceeds limit")
            if kind == "hex64":
                require(len(value) == 16 and all(c in "0123456789abcdef" for c in value),
                        "reference hex64 requires 16 lowercase digits")
        result.append(value)
    require(tag != "T" or result[1] in ("long", "short"), "reference invalid trade side")
    return tuple(result)


def canonical_cell(value: Any, wire: str) -> bytes:
    """Section 9.5 canonical value; independent of the reader's CRC path."""
    prefix = bytearray()
    if wire.endswith("?"):
        prefix.append(0 if value is None else 1)
        if value is None:
            return bytes(prefix)
    kind = wire.removesuffix("?")
    if kind == "i64":
        prefix.extend(int(value).to_bytes(L.U64_SIZE, "little", signed=True))
    elif kind == "u64":
        prefix.extend(int(value).to_bytes(L.U64_SIZE, "little"))
    elif kind == "f64":
        prefix.extend(struct.pack("<d", value))
    elif kind == "hex64":
        prefix.extend(int(value, 16).to_bytes(L.U64_SIZE, "little"))
    elif kind in ("bool", "order"):
        prefix.append(int(value))
    else:
        data = value.encode("utf-8")
        prefix.extend(len(data).to_bytes(L.CRC_SIZE, "little"))
        prefix.extend(data)
    return bytes(prefix)


def unsigned_varint(value: int) -> bytes:
    require(0 <= value <= L.SAFE, "reference varint outside safe domain")
    result = bytearray()
    while value >= 128:
        result.append((value % 128) + 128)
        value //= 128
    result.append(value)
    return bytes(result)


def zz(value: int) -> int:
    return 2 * value if value >= 0 else -2 * value - 1


def delta_encoding(values: list[int]) -> bytes | None:
    if not values or any(abs(value) >= L.DELTA_LIMIT for value in values):
        return None
    gcd = 0
    for i in range(1, len(values)):
        gcd = math.gcd(gcd, abs(values[i] - values[i - 1]))
    gcd = gcd or 1
    quotients = [(values[i] - values[i - 1]) // gcd for i in range(1, len(values))]
    if gcd > L.SAFE or any(abs(q) >= L.DELTA_LIMIT for q in quotients):
        return None
    return unsigned_varint(zz(values[0])) + unsigned_varint(gcd) + b"".join(
        unsigned_varint(zz(q)) for q in quotients)


def scaled_integer(value: float, scale: int) -> int | None:
    product = value * L.POW10[scale]
    if not math.isfinite(product) or abs(product) >= L.DECIMAL_LIMIT:
        return None
    integer = round(product)
    if abs(integer) >= L.DECIMAL_LIMIT:
        return None
    reconstructed = float(integer) / L.POW10[scale]
    return integer if struct.pack("<d", reconstructed) == struct.pack("<d", value) else None


def decimal_encoding(values: list[float]) -> bytes | None:
    if not values:
        return None
    scale = 0
    for value in values:
        for candidate in range(len(L.POW10)):
            if scaled_integer(value, candidate) is not None:
                scale = max(scale, candidate)
                break
        else:
            return None
    integers = []
    for value in values:
        integer = scaled_integer(value, scale)
        if integer is None:
            return None
        integers.append(integer)
    delta = delta_encoding(integers)
    return None if delta is None else bytes((scale,)) + delta


def shuffled_words(words: list[int]) -> bytes:
    output = bytearray(L.U64_SIZE * len(words))
    previous = 0
    for i, word in enumerate(words):
        difference = word ^ previous
        for plane in range(L.U64_SIZE):
            output[plane * len(words) + i] = (difference >> (plane * 8)) & 255
        previous = word
    return bytes(output)


def encode_column(values: list[Any], wire: str) -> tuple[int, int, bytes]:
    present = [value for value in values if value is not None]
    nullable = len(present) != len(values)
    bitmap = bytearray((len(values) + 7) // 8 if nullable else 0)
    if nullable:
        for i, value in enumerate(values):
            if value is None:
                bitmap[i // 8] |= 1 << (i % 8)
    kind = wire.removesuffix("?")
    if kind in ("i64", "u64"):
        delta = delta_encoding(present)
        if delta is not None and len(delta) < L.U64_SIZE * len(present):
            encoding, body = L.ENC_DELTA, delta
        else:
            encoding = L.ENC_PLAIN64
            body = b"".join(v.to_bytes(L.U64_SIZE, "little", signed=kind == "i64") for v in present)
    elif kind in ("f64", "hex64"):
        decimal = decimal_encoding(present) if kind == "f64" else None
        if decimal is not None and len(decimal) < L.U64_SIZE * len(present):
            encoding, body = L.ENC_DECIMAL, decimal
        else:
            words = [int(v, 16) if kind == "hex64" else int.from_bytes(struct.pack("<d", v), "little")
                     for v in present]
            encoding, body = L.ENC_XORSHUF, shuffled_words(words)
    elif kind in ("bool", "order"):
        encoding, body = L.ENC_BYTES, bytes(map(int, present))
    else:
        strings = [v.encode("utf-8") for v in present]
        dictionary = dict.fromkeys(strings)
        if 2 * len(dictionary) <= len(strings):
            lookup = {value: i for i, value in enumerate(dictionary)}
            body = (unsigned_varint(len(dictionary)) +
                    b"".join(unsigned_varint(len(v)) for v in dictionary) +
                    b"".join(dictionary) + b"".join(unsigned_varint(lookup[v]) for v in strings))
            encoding = L.ENC_DICT
        else:
            body = b"".join(unsigned_varint(len(v)) for v in strings) + b"".join(strings)
            encoding = L.ENC_PLAINSTR
    return encoding, int(nullable), bytes(bitmap) + body


def compress_column(raw: bytes, level: int) -> bytes:
    """One pledged-size, single-threaded frame; also used for SUMMARY."""
    if os.environ.get("PF_SINK_ZSTD_LIBRARY"):
        from pf_sink2_zstd import compress
        return compress(raw, level)
    window_log = min(24, max(10, (len(raw) - 1).bit_length()))
    try:
        from compression import zstd
    except ImportError:
        try:
            import zstandard
        except ImportError as error:
            raise SinkFormatError("reference compression needs Python 3.14 compression.zstd or PyPI zstandard") from error
        parameters = zstandard.ZstdCompressionParameters.from_level(
            level, source_size=len(raw), window_log=window_log, write_content_size=1, write_checksum=0,
            write_dict_id=0, threads=0)
        return zstandard.ZstdCompressor(compression_params=parameters).compress(raw)
    options = {zstd.CompressionParameter.window_log: window_log,
               zstd.CompressionParameter.content_size_flag: 1,
               zstd.CompressionParameter.checksum_flag: 0,
               zstd.CompressionParameter.dict_id_flag: 0,
               zstd.CompressionParameter.nb_workers: 0}
    return zstd.compress(raw, level=level, options=options)


def raw_summary_frame(data: bytes) -> bytes:
    """Bounded standard raw-block frame (9.1); also a decode-only fixture."""
    size = len(data)
    if size > L.MAX_SUMMARY:
        raise SummaryLimit("reference summary decoded length exceeds ceiling")
    require(size >= 1, "reference summary frame size")
    if size < 256:
        prefix = L.ZSTD_MAGIC + b"\x20" + bytes((size,))
    elif size < 65_792:
        prefix = L.ZSTD_MAGIC + b"\x60" + (size - 256).to_bytes(2, "little")
    else:
        prefix = L.ZSTD_MAGIC + b"\xa0" + size.to_bytes(4, "little")
    blocks = (size + L.MAX_ZSTD_BLOCK - 1) // L.MAX_ZSTD_BLOCK
    if L.SUMMARY_HEADER + len(prefix) + 3 * blocks + size > L.MAX_SUMMARY_LENGTH:
        raise SummaryLimit("reference raw summary stored length exceeds ceiling")
    frame = bytearray(prefix)
    for start in range(0, size, L.MAX_ZSTD_BLOCK):
        part = data[start:start + L.MAX_ZSTD_BLOCK]
        last = start + len(part) == size
        frame.extend(((len(part) << 3) | int(last)).to_bytes(3, "little"))
        frame.extend(part)
    return bytes(frame)


def compress_summary(data: bytes, level: int) -> bytes:
    """9.1: always frame; fall back when the library's 52+C exceeds the bound."""
    if len(data) > L.MAX_SUMMARY:
        raise SummaryLimit("reference summary decoded length exceeds ceiling")
    frame = compress_column(data, level)
    if L.SUMMARY_HEADER + len(frame) > L.MAX_SUMMARY_LENGTH:
        # Release the oversize candidate before constructing the fallback.
        del frame
        frame = raw_summary_frame(data)
    if L.SUMMARY_HEADER + len(frame) > L.MAX_SUMMARY_LENGTH:
        raise SummaryLimit("reference summary stored length exceeds ceiling")
    return frame


def metadata_bytes(summary: dict[str, Any] | bytes, trades: int | None = None) -> bytes:
    try:
        data = summary if isinstance(summary, bytes) else json.dumps(
            summary, separators=(",", ":"), allow_nan=False).encode("ascii")
    except (ValueError, UnicodeError, RecursionError) as error:
        raise SinkFormatError("reference invalid metadata value") from error
    if len(data) > L.MAX_SUMMARY:
        raise SummaryLimit("reference summary decoded length exceeds ceiling")
    require(len(data) >= 1 and not data.startswith(b"\xef\xbb\xbf"), "reference metadata size/BOM")

    def pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
        result = {}
        for key, value in items:
            require(key not in result, "reference duplicate metadata key")
            result[key] = value
        return result

    def constant(value: str) -> Any:
        raise SinkFormatError("reference non-JSON metadata constant " + value)

    def integer(token: str) -> int | float:
        value = int(token)
        require(abs(value) <= L.SAFE, "reference unsafe metadata integer")
        return -0.0 if token == "-0" else value

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
            require(depth <= L.MAX_DEPTH, "reference metadata nesting exceeds 64")
        elif byte in (93, 125):
            depth -= 1
        else:
            require(byte > 32, "reference whitespace outside metadata strings")
    try:
        value = json.loads(data.decode("utf-8", "strict"), object_pairs_hook=pairs,
                           parse_constant=constant, parse_int=integer)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise SinkFormatError("reference invalid metadata JSON") from error

    def check(item: Any, depth: int) -> None:
        require(depth <= L.MAX_DEPTH, "reference metadata nesting exceeds 64")
        if isinstance(item, str):
            require(not any(0xD800 <= ord(char) <= 0xDFFF for char in item), "reference metadata surrogate")
        elif type(item) is float:
            require(math.isfinite(item), "reference nonfinite metadata number")
        elif isinstance(item, (list, dict)):
            for child in (item.values() if isinstance(item, dict) else item):
                check(child, depth + int(isinstance(child, (list, dict))))
            if isinstance(item, dict):
                for key in item:
                    check(key, depth)

    check(value, int(isinstance(value, (dict, list))))
    require(type(value) is dict and set(L.SUMMARY_REQUIRED) <= value.keys(), "reference metadata fields")
    require(not set(L.SUMMARY_FORBIDDEN) & value.keys(), "reference metadata payload/self-reference")
    require(type(value["engine"]) is str and type(value["elapsed_seconds"]) in (int, float),
            "reference metadata engine/elapsed type")
    require(value["fingerprint"] is None or type(value["fingerprint"]) is dict,
            "reference metadata fingerprint type")
    for key in L.SUMMARY_REQUIRED + L.SUMMARY_OPTIONAL:
        if key in value and key not in ("engine", "elapsed_seconds", "fingerprint"):
            require(type(value[key]) is dict, "reference metadata object required: " + key)
    if "outputs" in value:
        require(set(value["outputs"]) == set(L.OUTPUT_METADATA),
                "reference outputs must contain bounded metadata only")
    folds = value["sink_folds"]
    require(tuple(folds) == L.SUMMARY_FOLDS and
            all(type(folds[key]) is int and 0 <= folds[key] <= L.SAFE for key in L.SUMMARY_FOLDS[:2]) and
            type(folds["open_at_end_pnl"]) in (int, float), "reference sink_folds fields/types")
    if trades is not None:
        require(folds["closed_count"] + folds["open_at_end_count"] == trades,
                "reference sink_folds trade count")
    return data


def witness_bytes(point: tuple[Any, ...] | None) -> bytes:
    if point is None:
        return bytes(L.WITNESS_SIZE)
    ordinal, time_ms, mtm, book = point
    mask = int(mtm is not None) + 2 * int(book is not None)
    mtm_word = 0 if mtm is None else int.from_bytes(struct.pack("<d", mtm), "little")
    book_word = 0 if book is None else int.from_bytes(struct.pack("<d", book), "little")
    return struct.pack(L.WITNESS, ordinal, time_ms, mtm_word, book_word, mask, 0)


def equity_point(ordinal: int, time_ms: int, equity: float, profit: float | None) -> tuple[Any, ...]:
    book = equity - (0.0 if profit is None else profit)
    return ordinal, time_ms, equity, book if math.isfinite(book) else None


def series_bytes(rows: list[tuple[Any, ...]], rec0: int) -> bytes:
    """Build both witness series independently, with min/max over (value,index)."""
    points = []
    for j, (time_ms, equity, profit) in enumerate(rows):
        if equity is not None:
            points.append(equity_point(rec0 + j, time_ms, equity, profit))

    output = bytearray()
    for index in (2, 3):
        finite = [point for point in points if point[index] is not None]
        minimum = min(finite, key=lambda point: (point[index], point[0])) if finite else None
        maximum = min(finite, key=lambda point: (-point[index], point[0])) if finite else None
        output.extend(struct.pack(L.SERIES_PREFIX, len(finite), 15 if finite else 9 if points else 0))
        for point in (points[0] if points else None, minimum, maximum, points[-1] if points else None):
            output.extend(witness_bytes(point))
        output.extend(bytes(L.SERIES_SIZE - L.SERIES_PADDING))
    return bytes(output)


def parse_caps(text: str) -> dict[str, int]:
    caps = {}
    if text:
        for item in text.split(","):
            tag, sep, value = item.partition("=")
            require(tag in L.TAGS and len(tag) == 1 and sep == "=" and value.isdigit(),
                    "invalid block caps; use E=2,T=1,I=4")
            require(tag not in caps, "repeated block cap")
            caps[tag] = int(value)
    return caps


class _Encoder:
    def __init__(self, output: BinaryIO, stack: ExitStack, scratch: Path, header: dict[str, Any],
                 summary: dict[str, Any] | bytes, level: int, block_records: dict[str, int] | None,
                 block_bytes: int, compress: bool, stored_digest: bool, frame_factory: FrameFactory | None,
                 force_full: bool, column_fixture: ColumnFixture | None,
                 summary_frame_factory: Callable[[bytes], bytes] | None):
        self.output, self.stack, self.scratch = output, stack, scratch
        self.offset = 0
        self.stored_hash = hashlib.sha256()
        self.compress, self.level, self.stored_digest = compress, level, stored_digest
        self.frame_factory = frame_factory
        self.summary_frame_factory = summary_frame_factory
        self.column_fixture = column_fixture
        require(type(force_full) is bool, "reference force_full must be bool")
        self.force_full = force_full
        self.frame_manifest: BinaryIO | None = None
        self.schema = v1._header(header)
        self.header_line = text_line(header)
        self.header_sha = hashlib.sha256(self.header_line).digest()
        self.summary = metadata_bytes(summary)
        self.summary_sha = hashlib.sha256(self.summary).digest()
        require(type(level) is int and 1 <= level <= 19, "reference encoder level must be 1..19")
        require(type(block_bytes) is int and L.MIN_BLOCK_BYTES <= block_bytes <= L.MAX_COLUMN,
                "reference block_bytes outside limits")
        self.block_bytes = block_bytes
        self.caps = {tag: L.DEFAULT_CAPS[tag] for tag in (*self.schema, "I")}
        for tag, cap in (block_records or {}).items():
            require(tag in self.caps and type(cap) is int and
                    1 <= cap <= (L.MAX_POSITIONS if tag == "I" else L.MAX_RECORDS), "reference invalid block cap")
            self.caps[tag] = cap
        require("K" not in self.caps or self.caps["K"] == 1, "reference K cap must be one")
        for tag, descriptor in self.schema.items():
            require(len(descriptor.fields) <= L.MAX_COLUMNS, "reference too many columns")
            if tag == "O":
                require(len(descriptor.fields) - 3 <= L.MAX_POSITIONS, "reference too many output slots")
            if tag == "K":
                require(len(descriptor.fields) <= L.MAX_POSITIONS, "reference too many constants")
        self.rows = {tag: [] for tag in self.caps}
        self.soft = dict.fromkeys(self.caps, 0)
        self.string_sizes = {tag: [0] * len(self.wires(tag)) for tag in self.caps}
        self.counts = dict.fromkeys(L.TAGS, 0)
        self.closed_counts = dict.fromkeys(L.TAGS, 0)
        self.block_counts = dict.fromkeys(L.TAGS, 0)
        self.streams = {tag: hashlib.sha256(L.STREAM_DOMAIN + tag.encode()) for tag in L.TAGS}
        self.descriptors = {tag: self.temp() for tag in L.TAGS}
        self.previous_e = None
        self.finite_e_count = self.closed_trades = self.open_trades = 0
        self.open_pnl = 0.0
        # Store E binary words, since SQLite REAL does not preserve -0.0.
        self.equity_rows = self.temp()
        database = sqlite3.connect(str(scratch / "ranks.sqlite"))
        self.database = database
        stack.callback(database.close)
        database.execute("PRAGMA cache_size=-1024")
        database.execute("PRAGMA temp_store=FILE")
        database.execute("PRAGMA journal_mode=OFF")
        database.execute("PRAGMA synchronous=OFF")
        database.execute("CREATE TABLE keys(p INTEGER PRIMARY KEY, exit INTEGER, pnl REAL, pct REAL, ru REAL, dd REAL)")
        head = dict(header)
        head["version"] = L.VERSION
        head["format"] = {"byte_order": "le", "codec": "zstd", "level": level,
                          "block_records": self.caps, "block_bytes": block_bytes}
        data = text_json(head).encode("utf-8")
        require(1 <= len(data) <= L.MAX_HEAD, "reference head exceeds limit")
        prefix = struct.pack(L.HEAD_PREFIX, L.MAGIC, len(data)) + data
        self.write(prefix + struct.pack("<I", zlib.crc32(prefix)))

    def temp(self) -> BinaryIO:
        return self.stack.enter_context(tempfile.TemporaryFile(dir=self.scratch))

    def wires(self, tag: str) -> tuple[str, ...]:
        return ("order",) if tag == "I" else self.schema[tag].types

    def write(self, data: bytes | bytearray) -> None:
        require(self.offset + len(data) <= L.SAFE, "reference output exceeds safe offsets")
        remaining = memoryview(data)
        while remaining:
            n = self.output.write(remaining)
            require(type(n) is int and 0 < n <= len(remaining), "reference short output write")
            remaining = remaining[n:]
        self.stored_hash.update(data)
        self.offset += len(data)

    def budget(self, tag: str, count: int, string_sizes: list[int]) -> bool:
        budgets = []
        for wire, strings in zip(self.wires(tag), string_sizes):
            kind = wire.removesuffix("?")
            budgets.append(3 + 6 * count + strings if kind == "str" else
                           count if kind in ("bool", "order") else 9 * count)
        return (all(size <= L.MAX_COLUMN for size in budgets) and
                L.BLOCK_HEADER + L.COLUMN_FRAMING * len(budgets) + sum(budgets) <= L.MAX_BLOCK)

    def add(self, tag: str, values: Iterable[Any]) -> None:
        require(tag in self.schema, "reference record absent from header")
        row = normalize_row(tag, values, self.wires(tag))
        text_line([tag, *row])  # X2 applies even though no JSON is stored.
        require(self.counts[tag] < L.SAFE, "reference count outside safe domain")
        if tag == "E":
            require(self.previous_e is None or row[0] >= self.previous_e, "reference E time decreases")
            self.previous_e = row[0]
            if row[1] is not None:
                self.equity_rows.write(struct.pack(
                    "<QqdBd", self.counts[tag], row[0], row[1], row[2] is not None,
                    0.0 if row[2] is None else row[2]))
        if tag == "T":
            require(row[0] == self.counts[tag] + 1, "reference T.n out of order")
            rank_values = [row[index] for index in L.RANK_COLUMNS]
            if rank_values[-1] is not None:
                rank_values[-1] = -abs(rank_values[-1]) if rank_values[-1] != 0.0 else 0.0
            self.database.execute("INSERT INTO keys VALUES(?,?,?,?,?,?)", (self.counts[tag], *rank_values))
            if row[14]:
                self.open_trades += 1
                if row[7] is not None:
                    self.open_pnl = self.open_pnl + row[7]
                    require(math.isfinite(self.open_pnl), "reference nonfinite open pnl fold")
            else:
                self.closed_trades += 1
        if tag == "O":
            require(row[0] == self.counts[tag], "reference O.row out of order")
        require(tag != "K" or self.counts[tag] == 0, "reference duplicate K")
        sizes = [len(value.encode("utf-8")) if wire.removesuffix("?") == "str" and value is not None else 0
                 for value, wire in zip(row, self.wires(tag))]
        nonstrings = sum(wire.removesuffix("?") != "str" for wire in self.wires(tag))
        cost = 8 * nonstrings + sum(sizes)
        proposed = [a + b for a, b in zip(self.string_sizes[tag], sizes)]
        n = len(self.rows[tag])
        if n and (n == self.caps[tag] or self.soft[tag] + cost > self.block_bytes or
                  not self.budget(tag, n + 1, proposed)):
            self.flush(tag)
            proposed = sizes
            n = 0
        require(self.budget(tag, n + 1, proposed), "reference single record exceeds decoded budget")
        self.rows[tag].append(row)
        self.string_sizes[tag] = proposed
        self.soft[tag] += cost
        self.counts[tag] += 1
        self.rows["I"].append((ord(tag),))
        self.counts["I"] += 1
        require(self.counts["I"] <= L.SAFE, "reference I count outside safe domain")
        if len(self.rows[tag]) == self.caps[tag]:
            self.flush(tag)
        if len(self.rows["I"]) == self.caps["I"]:
            self.flush("I")

    def flush(self, tag: str) -> None:
        rows = self.rows[tag]
        if not rows:
            return
        require(sum(self.block_counts.values()) < L.MAX_BLOCKS, "reference too many blocks")
        n, rec0, wires = len(rows), self.closed_counts[tag], self.wires(tag)
        directory, hashes, bodies = bytearray(), bytearray(), []
        raw_total = 0
        frame_columns = []
        for column, wire in enumerate(wires):
            values = [row[column] for row in rows]
            encoding, flags, raw = encode_column(values, wire)
            if self.column_fixture is not None:
                replacement = self.column_fixture(tag, column, encoding, flags, raw, values)
                if replacement is not None:
                    encoding, flags, raw, values = replacement
                    require(len(values) == n, "reference negative-column fixture width")
            require(len(raw) <= L.MAX_COLUMN, "reference raw column exceeds 16 MiB")
            raw_total += len(raw)
            stored, codec = raw, 0
            forced = self.frame_factory(tag, column, raw) if self.frame_factory else None
            if forced is not None:
                stored, codec = forced, 1
            elif self.compress and len(raw) >= L.MIN_ZSTD:
                compressed = compress_column(raw, self.level)
                if len(compressed) < len(raw):
                    stored, codec = compressed, 1
            domain = L.COL_DOMAIN + struct.pack(L.COL_PREFIX, ord(tag), column, rec0, n)
            crc = zlib.crc32(domain)
            column_hash = hashlib.sha256(domain)
            for value in values:
                cell = canonical_cell(value, wire)
                crc = zlib.crc32(cell, crc)
                column_hash.update(cell)
            hashes.extend(column_hash.digest())
            directory.extend(struct.pack(L.DIRECTORY, codec, encoding, flags, 0, len(raw), len(stored), crc))
            if codec:
                frame_columns.append((column, sum(len(body) for body in bodies), len(stored)))
            bodies.append(stored)
        decoded = L.BLOCK_HEADER + len(directory) + len(hashes) + raw_total
        length = L.BLOCK_HEADER + len(directory) + len(hashes) + sum(map(len, bodies))
        require(decoded <= L.MAX_BLOCK and length <= L.MAX_BLOCK, "reference block exceeds 32 MiB")
        prefix = struct.pack(L.BLOCK_PREFIX, L.BLOCK_MAGIC, ord(tag), 0, 0, len(wires), n, rec0,
                             length - L.BLOCK_HEADER)
        crc = zlib.crc32(hashes, zlib.crc32(directory, zlib.crc32(prefix)))
        start = self.offset
        self.write(prefix + struct.pack("<I", crc) + directory + hashes)
        for column, relative, size in frame_columns:
            if self.frame_manifest is not None:
                self.frame_manifest.write(text_line({"offset": self.offset + relative, "length": size,
                                                     "tag": tag, "column": column}))
        for body in bodies:
            self.write(body)
        block_hash = hashlib.sha256(L.BLOCK_DOMAIN + struct.pack(L.BLOCK_HASH_PREFIX, ord(tag), rec0, n) + hashes)
        for row in rows:
            for value, wire in zip(row, wires):
                canonical = canonical_cell(value, wire)
                self.streams[tag].update(canonical)
        flags = 4 if tag == "E" else 0
        ranges = []
        for bit, key in enumerate(L.TIME_COLUMNS[tag]):
            clocks = [] if key is None else [row[key] for row in rows if row[key] is not None]
            if clocks:
                flags |= 1 << bit
                ranges.extend((min(clocks), max(clocks)))
            else:
                ranges.extend((0, 0))
        descriptor = bytearray(struct.pack(L.DESCRIPTOR_PREFIX, start, length, ord(tag), flags, 0, n,
                                           decoded, rec0, *ranges))
        descriptor.extend(block_hash.digest())
        descriptor.extend(series_bytes(rows, rec0) if tag == "E" else bytes(2 * L.SERIES_SIZE))
        descriptor.extend(struct.pack("<Q", self.finite_e_count if tag == "E" else 0))
        descriptor.extend(bytes(L.DESCRIPTOR_SIZE - L.DESCRIPTOR_PADDING))
        require(len(descriptor) == L.DESCRIPTOR_SIZE, "reference descriptor layout bug")
        self.descriptors[tag].write(descriptor)
        self.block_counts[tag] += 1
        self.closed_counts[tag] += n
        if tag == "E":
            self.finite_e_count += sum(row[1] is not None for row in rows)
        self.rows[tag] = []
        self.string_sizes[tag] = [0] * len(wires)
        self.soft[tag] = 0

    def table(self, table_id: int, entries: BinaryIO, width: int, count: int) -> bytes:
        """Two-pass payload spool plus on-disk Merkle levels, no full table list."""
        if not count:
            return struct.pack(L.LOCATOR, 0, 0, width, 0, 0, 0, bytes(L.HASH_SIZE))
        capacity = L.PAGE_PAYLOAD // width
        pages = (count + capacity - 1) // capacity
        require(pages <= L.MAX_BLOCKS, "reference table page count exceeds u32")
        depth = (pages - 1).bit_length()
        offset = self.offset

        def page_parts(number: int) -> tuple[bytes, bytes]:
            used = min(capacity, count - number * capacity)
            entries.seek(number * capacity * width)
            payload = entries.read(used * width)
            require(len(payload) == used * width, "reference table spool truncated")
            header = struct.pack(L.PAGE_PREFIX, L.PAGE_MAGIC, table_id, 0, depth, 0, number,
                                 used, width, pages)
            return header, payload

        # Close all levels when this table is done; at most depth+1 files.
        with ExitStack() as levels_stack:
            def level_file() -> BinaryIO:
                return levels_stack.enter_context(tempfile.TemporaryFile(dir=self.scratch))

            leaves = level_file()
            levels = [leaves]
            last = b""
            for page in range(pages):
                prefix, payload = page_parts(page)
                last = hashlib.sha256(L.INDEX_DOMAIN + prefix + payload).digest()
                leaves.write(last)
            for _ in range((1 << depth) - pages):
                leaves.write(last)
            size = 1 << depth
            while size > 1:
                source = levels[-1]
                source.seek(0)
                destination = level_file()
                for _ in range(size // 2):
                    destination.write(hashlib.sha256(L.NODE_DOMAIN + source.read(2 * L.HASH_SIZE)).digest())
                levels.append(destination)
                size //= 2
            levels[-1].seek(0)
            root_hash = levels[-1].read(L.HASH_SIZE)
            for page in range(pages):
                prefix, payload = page_parts(page)
                header = bytearray(L.PAGE_HEADER)
                header[:L.PAGE_LEAF] = prefix
                leaves.seek(page * L.HASH_SIZE)
                header[L.PAGE_LEAF:L.PAGE_PROOFS] = leaves.read(L.HASH_SIZE)
                for level in range(depth):
                    levels[level].seek(((page >> level) ^ 1) * L.HASH_SIZE)
                    begin = L.PAGE_PROOFS + level * L.HASH_SIZE
                    header[begin:begin + L.HASH_SIZE] = levels[level].read(L.HASH_SIZE)
                crc = zlib.crc32(payload, zlib.crc32(header[:L.PAGE_CRC]))
                struct.pack_into("<I", header, L.PAGE_CRC, crc)
                self.write(header)
                self.write(payload)
        last_bytes = L.PAGE_HEADER + width * (count - (pages - 1) * capacity)
        return struct.pack(L.LOCATOR, offset, pages, width, count, last_bytes, L.PRESENT, root_hash)

    def figure_entries(self) -> BinaryIO:
        """Independent one-bucket reduction of the finite E input spool."""
        output = self.temp()
        population = self.finite_e_count
        buckets = min(population, L.FIGURE_BUCKETS)
        self.equity_rows.seek(0)
        sample = struct.Struct("<QqdBd")
        folds = json.loads(self.summary)["sink_folds"]
        position = 0
        for bucket in range(buckets):
            end = (bucket + 1) * population // buckets
            first = last = None
            minima, maxima = [None, None], [None, None]
            while position < end:
                raw = self.equity_rows.read(sample.size)
                require(len(raw) == sample.size, "reference finite E spool truncated")
                ordinal, time_ms, equity, valid, profit = sample.unpack(raw)
                if not valid:
                    profit = 0.0
                if (position == population - 1 and folds["open_at_end_count"] > 0 and
                        abs(profit) <= L.FORCED_CLOSE_EPSILON):
                    profit = float(folds["open_at_end_pnl"])
                point = equity_point(ordinal, time_ms, equity, profit)
                if first is None:
                    first = point
                last = point
                for series, column in enumerate((2, 3)):
                    value = point[column]
                    if value is not None:
                        if minima[series] is None or value < minima[series][column]:
                            minima[series] = point
                        if maxima[series] is None or value > maxima[series][column]:
                            maxima[series] = point
                position += 1
            selected = (first, minima[0], maxima[0], last, first, minima[1], maxima[1], last)
            union = {point[0]: point for point in selected if point is not None}
            points = [union[index] for index in sorted(union)]
            require(1 <= len(points) <= L.FIGURE_MAX_WITNESSES, "reference figure union")
            slots = {point[0]: index for index, point in enumerate(points)}
            selectors = bytes(L.ABSENT_SELECTOR if point is None else slots[point[0]] for point in selected)
            output.write(bytes((len(points),)) + bytes(L.FIGURE_SELECTORS - 1) + selectors +
                         b"".join(witness_bytes(point) for point in points) +
                         bytes((L.FIGURE_MAX_WITNESSES - len(points)) * L.WITNESS_SIZE))
        require(position == population and not self.equity_rows.read(1), "reference finite E spool count")
        return output

    def compact_index(self) -> tuple[int, int, bytes]:
        start = self.offset
        digest = hashlib.sha256(L.COMPACT_INDEX_DOMAIN)
        for tag in L.TAGS:
            entries = self.descriptors[tag]
            entries.seek(0)
            for _ in range(self.block_counts[tag]):
                full = entries.read(L.DESCRIPTOR_SIZE)
                require(len(full) == L.DESCRIPTOR_SIZE, "reference descriptor spool truncated")
                entry = bytearray(full[:L.COMPACT_DESCRIPTOR_SIZE])
                entry[L.DESCRIPTOR_FLAGS] &= ~4
                self.write(entry)
                digest.update(entry)
        length = self.offset - start
        require(length <= L.COMPACT_TABLE_LIMIT, "reference compact table ceiling")
        return start, length, digest.digest()

    def full_indexes(self) -> list[bytes]:
        locators = [self.table(index, self.descriptors[tag], L.DESCRIPTOR_SIZE, self.block_counts[tag])
                    for index, tag in enumerate(L.TAGS)]
        self.database.commit()
        width = 4 if self.counts["T"] <= L.U32_POSITIONS else L.U64_SIZE
        for key_index, key in enumerate(L.RANK_KEYS):
            self.database.execute(f"CREATE INDEX rank_{key} ON keys({key},p)")
            for descending in (False, True):
                with tempfile.TemporaryFile(dir=self.scratch) as positions:
                    direction = "DESC" if descending else "ASC"
                    cursor = self.database.execute(
                        f"SELECT p FROM keys ORDER BY ({key} IS NULL), {key} {direction}, p ASC")
                    for (position,) in cursor:
                        positions.write(position.to_bytes(width, "little"))
                    cursor.close()
                    table_id = len(L.TAGS) + 2 * key_index + int(descending)
                    locators.append(self.table(table_id, positions, width, self.counts["T"]))
            self.database.execute(f"DROP INDEX rank_{key}")
        figure = self.figure_entries()
        locators.append(self.table(L.FIGURE_TABLE, figure, L.FIGURE_ENTRY,
                                   min(L.FIGURE_BUCKETS, self.finite_e_count)))
        return locators

    def finish(self) -> dict[str, Any]:
        for tag in L.TAGS:
            if tag in self.rows:
                self.flush(tag)
        metadata_bytes(self.summary, self.counts["T"])
        folds = json.loads(self.summary)["sink_folds"]
        require(folds["closed_count"] == self.closed_trades and folds["open_at_end_count"] == self.open_trades and
                struct.pack("<d", folds["open_at_end_pnl"]) == struct.pack("<d", self.open_pnl),
                "reference sink_folds differ from canonical T fold")
        nblocks = sum(self.block_counts.values())
        compact = (not self.force_full and self.counts["E"] < L.COMPACT_E_LIMIT and
                   self.counts["T"] < L.COMPACT_T_LIMIT and nblocks <= L.COMPACT_BLOCK_LIMIT)
        data_end = self.offset
        frame = (self.summary_frame_factory(self.summary) if self.summary_frame_factory is not None
                 else compress_summary(self.summary, self.level))
        summary_length = L.SUMMARY_HEADER + len(frame)
        if len(self.summary) > L.MAX_SUMMARY or summary_length > L.MAX_SUMMARY_LENGTH:
            raise SummaryLimit("reference summary stored/decoded length exceeds ceiling")
        require(len(frame) >= 1, "reference empty summary frame")
        self.write(struct.pack(L.SUMMARY_PREFIX, L.SUMMARY_MAGIC, len(frame), len(self.summary),
                               zlib.crc32(self.summary), 0, self.summary_sha))
        if self.frame_manifest is not None and self.summary_frame_factory is None:
            self.frame_manifest.write(text_line({"tag": "SUMMARY", "offset": self.offset, "length": len(frame)}))
        self.write(frame)
        del frame  # 10.7: release SUMMARY storage before index/rank work.
        compact_table = self.compact_index() if compact else None
        locators = [] if compact else self.full_indexes()
        root_offset = self.offset
        root_length = L.COMPACT_ROOT_SIZE if compact else L.ROOT_SIZE
        root = bytearray(root_length)
        if compact:
            struct.pack_into(L.COMPACT_ROOT_PREFIX, root, 0, L.ROOT_MAGIC, L.VERSION, L.COMPACT_FLAG,
                             data_end, summary_length, nblocks)
            count_offset = L.COMPACT_COUNTS
            struct.pack_into("<Q", root, L.COMPACT_FINITE_E, self.finite_e_count)
            root[L.COMPACT_SUMMARY_SHA:L.COMPACT_V1_SHA] = self.summary_sha
            root[L.COMPACT_V1_SHA:L.COMPACT_TABLE_OFFSET] = self.header_sha
            table_offset, table_length, table_sha = compact_table
            struct.pack_into("<QQ", root, L.COMPACT_TABLE_OFFSET, table_offset, table_length)
            root[L.COMPACT_TABLE_SHA:L.COMPACT_BLOCK_COUNTS] = table_sha
            for i, tag in enumerate(L.TAGS):
                struct.pack_into("<I", root, L.COMPACT_BLOCK_COUNTS + i * L.CRC_SIZE, self.block_counts[tag])
        else:
            struct.pack_into(L.ROOT_PREFIX, root, 0, L.ROOT_MAGIC, L.VERSION, 0, data_end, data_end,
                             summary_length, nblocks)
            count_offset = L.ROOT_COUNTS
            root[L.ROOT_SUMMARY_SHA:L.ROOT_V1_SHA] = self.summary_sha
            root[L.ROOT_V1_SHA:L.ROOT_FINITE_E] = self.header_sha
            struct.pack_into("<QI", root, L.ROOT_FINITE_E, self.finite_e_count, L.FIGURE_BUCKETS)
            for i, locator in enumerate(locators):
                start = L.ROOT_TABLES + i * L.LOCATOR_SIZE
                root[start:start + L.LOCATOR_SIZE] = locator
        logical = hashlib.sha256(L.LOGICAL_DOMAIN + self.header_sha + self.summary_sha)
        for i, tag in enumerate(L.TAGS):
            count = self.counts[tag]
            digest = self.streams[tag].digest()
            struct.pack_into("<Q", root, count_offset + i * L.U64_SIZE, count)
            if not compact:
                start = L.ROOT_STREAMS + i * L.HASH_SIZE
                root[start:start + L.HASH_SIZE] = digest
            logical.update(struct.pack(L.LOGICAL_ITEM, ord(tag), count))
            logical.update(digest)
        root_crc = L.COMPACT_ROOT_CRC if compact else L.ROOT_CRC
        struct.pack_into("<I", root, root_crc, zlib.crc32(root[:root_crc]))
        self.write(root)
        stored_sha = self.stored_hash.digest()
        tail = bytearray(L.TAIL_SIZE)
        struct.pack_into(L.TAIL_PREFIX, tail, 0, root_offset, root_length, int(self.stored_digest),
                         self.offset + L.TAIL_SIZE)
        tail[L.TAIL_ROOT_SHA:L.TAIL_LOGICAL_SHA] = hashlib.sha256(root).digest()
        tail[L.TAIL_LOGICAL_SHA:L.TAIL_STORED_SHA] = logical.digest()
        if self.stored_digest:
            tail[L.TAIL_STORED_SHA:L.TAIL_PADDING] = stored_sha
        tail[L.TAIL_MAGIC_OFFSET:] = L.TAIL_MAGIC
        self.write(tail)
        return {"sha256": logical.hexdigest(), "bytes": self.offset,
                "counts": {tag: self.counts[tag] for tag in self.schema},
                "block_count": nblocks, "profile": "COMPACT" if compact else "FULL"}


def encode(header: dict[str, Any], records: Iterable[v1.Record | list[Any] | tuple[Any, ...]],
           summary: dict[str, Any] | bytes, output: BinaryIO, *, level: int = 3,
           block_records: dict[str, int] | None = None, block_bytes: int = L.DEFAULT_BYTES,
           compress: bool = True, stored_digest: bool = True, frame_factory: FrameFactory | None = None,
           frame_manifest: BinaryIO | None = None, scratch_dir: str | Path | None = None,
           force_full: bool = False, column_fixture: ColumnFixture | None = None,
           summary_frame_factory: Callable[[bytes], bytes] | None = None) -> dict[str, Any]:
    """Encode input rows; tail is written only after the input iterator succeeds.

    frame_factory is exclusively for decode/refusal fixtures (including raw/RLE
    zstd frames below the canonical size threshold). It is not a writer mode.
    frame_manifest streams frame byte ranges, avoiding a block-sized mask list.
    summary_frame_factory is exclusively a decode/refusal fixture hook. Normal
    encoding always uses HEAD's level, even with compress=False (stored columns).
    column_fixture is ONLY for named negative controls: it overrides encoded
    bytes and their canonical hash values so a reader reaches the intended
    domain/X2 refusal rather than an unrelated CRC. Never use it for successful
    vectors or writer-vs-encoder conformance.
    """
    with tempfile.TemporaryDirectory(dir=scratch_dir) as scratch, ExitStack() as stack:
        encoder = _Encoder(output, stack, Path(scratch), header, summary, level, block_records,
                           block_bytes, compress, stored_digest, frame_factory, force_full, column_fixture,
                           summary_frame_factory)
        encoder.frame_manifest = frame_manifest
        for record in records:
            if isinstance(record, v1.Record):
                encoder.add(record.tag, record.values)
            else:
                encoder.add(record[0], record[1:])
        return encoder.finish()


def encode_bytes(header: dict[str, Any], records: Iterable[Any], summary: dict[str, Any] | bytes,
                 **kwargs: Any) -> bytes:
    """Small-fixture convenience; production/offline conversion uses encode()."""
    output = io.BytesIO()
    encode(header, records, summary, output, **kwargs)
    return output.getvalue()


def from_v1(source: str | Path, destination: str | Path, summary: dict[str, Any] | bytes,
            **kwargs: Any) -> dict[str, Any]:
    source_path, destination_path = Path(source), Path(destination)
    same_inode = destination_path.exists() and source_path.samefile(destination_path)
    require(not same_inode and source_path.resolve() != destination_path.resolve(),
            "reference output would overwrite v1 input")
    with v1.SinkReader(source) as reader, open(destination, "wb") as output:
        # SinkReader raises before iterator exhaustion on an invalid receipt.
        # Therefore encode() cannot finish/seal an unverified v1 input.
        return encode(reader.header, reader, summary, output, **kwargs)
