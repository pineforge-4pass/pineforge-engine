"""CR-150 pf_bar_t/v1 file contract and streaming CSV writer.

No header: each record is <5dq (OHLCV, open Unix ms), 48 bytes. A feed
also has <q closes. The canonical JSON sidecar is the commit record. Only
volume may be NaN; float()'s zero signs and NaN bits are never rewritten.

convert_csv uses two fixed 1 MiB buffers at most. Contract checks are also
exposed for the mapped reader; mapping and engine ownership belong to L2.
"""
from __future__ import annotations

import calendar
import csv
import hashlib
import json
import math
import os
import re
import stat
import struct
from contextlib import ExitStack, closing
from datetime import datetime, timezone
from itertools import chain
from pathlib import Path


FORMAT = "pf_bar_t/v1"
BAR_RECORD = struct.Struct("<5dq")
CLOSE_RECORD = struct.Struct("<q")
MAX_COUNT = 2**31 - 1
STAMP_MAX = 2**53 - 1
BUFFER_SIZE = 1024 * 1024
# A sidecar is one small metadata object; reuse the existing 1 MiB bounded
# I/O unit as its hard input limit.
SIDECAR_MAX_BYTES = BUFFER_SIZE
SOURCE_VALUES_PREFIX = b"pineforge:ohlcv:barc-le:v1\0"
SYMBOL_VALUES_PREFIX = b"pineforge:symbol-feed:barc-close-le:v1\0"
CHART_REASONS = frozenset({
    "io", "columns", "value", "empty", "size", "count", "sidecar", "order", "timeframe_invalid",
})
FEED_REASONS = frozenset({
    "feed_columns_missing", "feed_value_not_number", "feed_value_invalid",
    "feed_time_out_of_range", "feed_not_increasing", "feed_close_time_invalid",
    "feed_unreadable", "feed_not_utf8_csv", "size", "count", "sidecar", "close_file",
    "empty", "io", "timeframe_invalid",
})
_TF_RE = re.compile(r"(?:[1-9][0-9]{0,4}|[1-9][0-9]{0,3}[DWMS])")
_SHA_RE = re.compile(r"[0-9a-f]{64}")


class BarsError(ValueError):
    """A catalog-named refusal, without a dependency on run_json or ctypes."""

    def __init__(self, text, reason, *, feed=False):
        super().__init__(text)
        self.code = "symbol_feeds_refused" if feed else "chart_bars_unreadable"
        self.code_args = {"reason": reason}


def canonical_timeframe(tf, *, feed=False):
    """symbol_timeframe's spelling and limits, with the selected mode's code."""
    if isinstance(tf, str) and tf in ("D", "W", "M", "S"):
        tf = "1" + tf
    if not (isinstance(tf, str) and _TF_RE.fullmatch(tf)):
        raise BarsError(f"csv2bin: invalid timeframe {tf!r}", "timeframe_invalid", feed=feed)
    return tf


def conventional_name(symbol, tf, *, feed=False):
    """Human-facing convention only; explicit output paths remain authoritative."""
    token = re.sub(rb"[^A-Za-z0-9._-]", b"_", symbol.encode("utf-8")).decode("ascii")
    return f"{'feed' if feed else 'chart'}.{token}.{canonical_timeframe(tf, feed=feed)}.bin"


def sidecar_path(path):
    return Path(str(path) + ".json")


def close_path(path):
    return Path(path).with_suffix(".close.bin")


def bar_close_ms(open_ms, tf):
    """CSV fallback: integer units, or UTC calendar months with day clamping.

    tf is canonical. A session-bound CSV should supply time_close. There is
    deliberately no session guess here. None means outside years 1..9999.
    """
    n = int(tf) if tf.isdigit() else int(tf[:-1])
    unit = "" if tf.isdigit() else tf[-1]
    if unit != "M":
        return open_ms + n * {"": 60000, "S": 1000, "D": 86400000, "W": 604800000}[unit]
    seconds, ms = divmod(open_ms, 1000)
    try:
        opened = datetime.fromtimestamp(seconds, tz=timezone.utc)
        shifted = opened.month - 1 + n
        year, month = opened.year + shifted // 12, shifted % 12 + 1
        day = min(opened.day, calendar.monthrange(year, month)[1])
        return int(opened.replace(year=year, month=month, day=day).timestamp()) * 1000 + ms
    except (ValueError, OverflowError, OSError):
        return None


def bar_count(size, *, feed=False):
    """Validate an fstat size, including the absence of zero-record binaries."""
    if size % BAR_RECORD.size:
        raise BarsError(f"bar file size {size} is not a multiple of 48", "size", feed=feed)
    count = size // BAR_RECORD.size
    if count == 0:
        raise BarsError("bar file has no records", "empty", feed=feed)
    if count > MAX_COUNT:
        raise BarsError(f"bar count {count} exceeds {MAX_COUNT}", "count", feed=feed)
    return count


def check_close_size(size, count):
    if size != CLOSE_RECORD.size * count:
        raise BarsError(f"close file size {size} does not match {count} records",
                        "close_file", feed=True)


def check_order(previous, timestamp, *, feed=False):
    """For writers and the loader's existing hash pass (QC9a)."""
    if previous is not None and timestamp <= previous:
        raise BarsError("timestamps must increase",
                        "feed_not_increasing" if feed else "order", feed=feed)


def read_sidecar(path, *, feed=False):
    fd = None
    try:
        flags = (os.O_RDONLY | getattr(os, "O_NONBLOCK", 0)
                 | getattr(os, "O_CLOEXEC", 0))
        fd = os.open(path, flags)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise BarsError(f"sidecar {path} is not a regular file", "sidecar", feed=feed)
        if info.st_size > SIDECAR_MAX_BYTES:
            raise BarsError(f"sidecar {path} exceeds {SIDECAR_MAX_BYTES} bytes",
                            "sidecar", feed=feed)
        with os.fdopen(fd, "rb") as stream:
            fd = None
            raw = stream.read(SIDECAR_MAX_BYTES + 1)
        if len(raw) > SIDECAR_MAX_BYTES:
            raise BarsError(f"sidecar {path} exceeds {SIDECAR_MAX_BYTES} bytes",
                            "sidecar", feed=feed)
        document = json.loads(raw)
    except BarsError:
        raise
    except (OSError, UnicodeError, ValueError, RecursionError) as error:
        raise BarsError(f"cannot read sidecar {path}: {error}", "sidecar", feed=feed) from None
    finally:
        if fd is not None:
            os.close(fd)
    if not isinstance(document, dict):
        raise BarsError(f"sidecar {path} must be an object", "sidecar", feed=feed)
    return document


def check_sidecar(document, *, count, first_ts, last_ts, sha256=None,
                  close_sha256=None, feed=False):
    """Check metadata; pass computed digests only for --verify-bin full.

    Symbol is descriptive and is never compared to the chart or feed index.
    Omitting digest arguments implements sidecar-only verification. The L2
    caller still checks file sizes and scans order in its hashing pass.
    """
    valid = isinstance(document, dict)
    if valid:
        valid = (document.get("format") == FORMAT
                 and type(document.get("record_size")) is int
                 and document["record_size"] == BAR_RECORD.size
                 and isinstance(document.get("symbol"), str)
                 and isinstance(document.get("tf"), str)
                 and _TF_RE.fullmatch(document["tf"]) is not None
                 and all(type(document.get(key)) is int and document[key] == value
                         for key, value in (("count", count), ("first_ts", first_ts),
                                            ("last_ts", last_ts)))
                 and isinstance(document.get("sha256"), str)
                 and _SHA_RE.fullmatch(document["sha256"]) is not None)
    if valid and feed:
        valid = (isinstance(document.get("close_sha256"), str)
                 and _SHA_RE.fullmatch(document["close_sha256"]) is not None)
    if valid and sha256 is not None:
        valid = document["sha256"] == sha256
    if valid and feed and close_sha256 is not None:
        valid = document["close_sha256"] == close_sha256
    if not valid:
        raise BarsError("sidecar does not match the bar/close files", "sidecar", feed=feed)


def sidecar_bytes(document):
    return (json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
            + "\n").encode("ascii")


def _csv_rows(path, tf, feed):
    """First-pass checks only; input I/O must not become an output-io refusal."""
    where = f"csv2bin: {path}"
    try:
        with path.open(newline="", encoding="utf-8-sig" if feed else "utf-8") as stream:
            reader = csv.DictReader(stream)
            if feed:
                columns = reader.fieldnames or []
                missing = [k for k in ("timestamp", "open", "high", "low", "close")
                           if k not in columns]
                if missing:
                    raise BarsError(f"{where}: no column {', '.join(missing)}",
                                    "feed_columns_missing", feed=True)
            for row in reader:
                line = reader.line_num
                try:
                    if feed:
                        # Keep run_json's parse order: even an invalid price is
                        # checked only after time_close has been parsed.
                        timestamp = int(row["timestamp"])
                        o, h, l, c = (float(row[k]) for k in ("open", "high", "low", "close"))
                        cell = (row.get("volume") or "").strip()
                        volume = float(cell) if cell else math.nan
                        cell = (row.get("time_close") or "").strip()
                        close = int(cell) if cell else None
                        values = (o, h, l, c, volume, timestamp)
                    else:
                        values = (float(row["open"]), float(row["high"]), float(row["low"]),
                                  float(row["close"]), float(row["volume"]), int(row["timestamp"]))
                        close = None
                except KeyError as error:
                    raise BarsError(f"{where}: no column {error.args[0]}",
                                    "feed_columns_missing" if feed else "columns", feed=feed) from None
                except (TypeError, ValueError):
                    raise BarsError(f"{where} line {line}: not a number",
                                    "feed_value_not_number" if feed else "value", feed=feed) from None
                if not feed and not -(2**63) <= values[5] < 2**63:
                    raise BarsError(f"{where} line {line}: timestamp is outside int64", "value")
                if (not all(math.isfinite(x) for x in values[:4])
                        or values[4] < 0 or math.isinf(values[4])):
                    raise BarsError(f"{where} line {line}: prices must be finite and volume nonnegative or NaN",
                                    "feed_value_invalid" if feed else "value", feed=feed)
                if feed:
                    if close is None:
                        close = bar_close_ms(timestamp, tf)
                    if not all(x is not None and abs(x) <= STAMP_MAX for x in (timestamp, close)):
                        raise BarsError(f"{where} line {line}: a time must be within +-{STAMP_MAX}",
                                        "feed_time_out_of_range", feed=True)
                yield values, close, line
    except OSError as error:
        raise BarsError(f"{where}: {error}", "feed_unreadable" if feed else "io", feed=feed) from None
    except (UnicodeDecodeError, csv.Error) as error:
        raise BarsError(f"{where}: not a UTF-8 CSV ({error})",
                        "feed_not_utf8_csv" if feed else "value", feed=feed) from None


class _PackedWriter:
    def __init__(self, stream, record):
        self.stream, self.record = stream, record
        self.buffer = bytearray(BUFFER_SIZE)
        self.used = 0
        self.hasher = hashlib.sha256()

    def append(self, values):
        if self.used + self.record.size > len(self.buffer):
            self._flush_buffer()
        self.record.pack_into(self.buffer, self.used, *values)
        self.used += self.record.size

    def _flush_buffer(self):
        view = memoryview(self.buffer)[:self.used]
        self.stream.write(view)
        self.hasher.update(view)
        self.used = 0

    def sync(self):
        self._flush_buffer()
        self.stream.flush()
        os.fsync(self.stream.fileno())


def _precheck(paths, feed, fresh):
    for path in paths:
        try:
            mode = path.lstat().st_mode
        except FileNotFoundError:
            continue
        if not stat.S_ISREG(mode) or fresh:
            raise BarsError(f"csv2bin: output name is {'already present' if fresh else 'not a regular file'}: {path}",
                            "io", feed=feed)


class _Directory:
    def __init__(self, path, fresh):
        self.path, self.fresh, self.fd = path, fresh, None

    def __enter__(self):
        if not self.fresh:
            self.fd = os.open(self.path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        return self

    def sync(self):
        if self.fd is not None:
            os.fsync(self.fd)

    def __exit__(self, *exc):
        if self.fd is not None:
            os.close(self.fd)


def _open_temporary(path):
    flags = (os.O_WRONLY | os.O_CREAT | os.O_EXCL
             | getattr(os, "O_NOFOLLOW", 0)
             | getattr(os, "O_CLOEXEC", 0))
    fd = os.open(path, flags, 0o600)
    try:
        return os.fdopen(fd, "wb")
    except BaseException:
        os.close(fd)
        raise


def _feed_pair_error(previous, current):
    values, close, line = previous
    next_values, _, next_line = current
    if next_values[5] <= values[5]:
        return BarsError(f"csv2bin line {next_line}: timestamps must increase",
                         "feed_not_increasing", feed=True)
    if close <= values[5] or close > next_values[5]:
        return BarsError(f"csv2bin line {line}: close must be after open and at or before the next open",
                         "feed_close_time_invalid", feed=True)
    return None


def convert_csv(csv_path, output_path, *, symbol, tf, feed=False, fresh_private_directory=False):
    """Write bars, optional closes, then the sidecar; return its metadata.

    Existing regular outputs are replaced. All three final names (including
    the unused chart close name) are pre-checked before any write. In an
    explicitly fresh private directory, existing names are refused and
    directory fsync is omitted (QC5a). The CLI always uses durable overwrite.

    Temporary names are fixed, per the contract; one writer owns a generation.
    On refusal no sidecar is published. Partial .tmp files are retained.
    """
    tf = canonical_timeframe(tf, feed=feed)
    csv_path, output_path = Path(csv_path), Path(output_path)
    close_file, sidecar = close_path(output_path), sidecar_path(output_path)
    paths = (output_path, close_file, sidecar)
    try:
        _precheck(paths, feed, fresh_private_directory)
        with closing(_csv_rows(csv_path, tf, feed)) as rows:
            first = next(rows, None)
            if first is None:
                raise BarsError(f"csv2bin: {csv_path}: no bars", "empty", feed=feed)
            with _Directory(output_path.parent, fresh_private_directory) as directory:
                # The old commit record must disappear durably before any
                # member of its generation can be replaced.
                if not fresh_private_directory:
                    try:
                        os.unlink(sidecar)
                    except FileNotFoundError:
                        pass
                    else:
                        directory.sync()
                with ExitStack() as stack:
                    bar_tmp = Path(str(output_path) + ".tmp")
                    bars = _PackedWriter(stack.enter_context(_open_temporary(bar_tmp)), BAR_RECORD)
                    closes = None
                    if feed:
                        close_tmp = Path(str(close_file) + ".tmp")
                        closes = _PackedWriter(stack.enter_context(_open_temporary(close_tmp)),
                                               CLOSE_RECORD)
                    count, previous, pending = 0, None, None
                    first_ts = first[0][5]
                    for current in chain((first,), rows):
                        values, close, _ = current
                        if feed:
                            # Defer the first second-pass defect. Any later
                            # first-pass defect still wins, as in run_json.
                            if previous is not None and pending is None:
                                pending = _feed_pair_error(previous, current)
                        else:
                            check_order(previous[0][5] if previous is not None else None, values[5])
                        count += 1
                        if count > MAX_COUNT:
                            raise BarsError(f"csv2bin: bar count exceeds {MAX_COUNT}", "count", feed=feed)
                        bars.append(values)
                        if closes is not None:
                            closes.append((close,))
                        previous = current
                    if pending is not None:
                        raise pending
                    if feed and previous[1] <= previous[0][5]:
                        raise BarsError(f"csv2bin line {previous[2]}: close must be after open",
                                        "feed_close_time_invalid", feed=True)
                    bars.sync()
                    os.replace(bar_tmp, output_path)
                    directory.sync()
                    if closes is not None:
                        closes.sync()
                        os.replace(close_tmp, close_file)
                        directory.sync()
                    metadata = {"format": FORMAT, "record_size": BAR_RECORD.size, "symbol": symbol,
                                "tf": tf, "count": count, "first_ts": first_ts,
                                "last_ts": previous[0][5], "sha256": bars.hasher.hexdigest()}
                    if closes is not None:
                        metadata["close_sha256"] = closes.hasher.hexdigest()
                sidecar_tmp = Path(str(sidecar) + ".tmp")
                with _open_temporary(sidecar_tmp) as stream:
                    stream.write(sidecar_bytes(metadata))
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(sidecar_tmp, sidecar)
                directory.sync()
                return metadata
    except OSError as error:
        raise BarsError(f"csv2bin: cannot write {output_path}: {error}", "io", feed=feed) from None
