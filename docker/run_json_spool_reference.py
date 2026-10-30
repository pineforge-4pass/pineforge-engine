"""Independent copies of the immediate column writer and equity proof.

The copied bodies are from 3e34b9a02164ee5f619f9f3bcb8773da037e9d11.
Only unchanged dependencies are shared with the wrapper. This support module
does not match the test collection glob.
"""
from contextlib import contextmanager
import ctypes
from itertools import chain, repeat
import math
import mmap
import os
import stat
import sys

import run_json
from run_json import (
    RunFailure, ReportSinkReceiptC, _engine_invariant, _open_uncached_sink,
    _sink_summary, _sink_trade, _stream_json, _SinkReportDiagnostics,
    _filesystem_type, _MEMORY_FILESYSTEM_TYPES,
)

_BULK_E_LINE = run_json._BULK_E_LINE
_BULK_E_RUN = run_json._BULK_E_RUN
_BULK_E_RUN_PREFIX = run_json._BULK_E_RUN_PREFIX
_BULK_E_LITERAL_BYTES = run_json._BULK_E_LITERAL_BYTES


class _BulkEquity(run_json._BulkEquity):
    # Keep the copied constructor's plain endpoints independent of properties.
    first = last = None

    def __init__(self, raw, count, first, last, columns=None):
        self.raw, self.count, self.first, self.last = raw, count, first, last
        if columns is None:
            # Directly constructed batches need no proof or bound. Recover
            # captures only when they tile the input; columns remain absent
            # only on direct batches that need substitution over unmatched bytes.
            rows = _BULK_E_LINE.findall(raw)
            columns = tuple(zip(*rows)) if rows else ((), (), ())
            if (sum(map(len, chain.from_iterable(columns)))
                    + _BULK_E_LITERAL_BYTES * len(rows) != len(raw)):
                columns = None
        self.columns = columns


def _bulk_equity(raw):
    """Accept a strict subset; a failed proof never supplies an error.

    Product callers supply one complete _BULK_E_RUN match or, in the pending-line
    caller, one LF-terminated line containing at most one row. The repeat bound
    bounds captures retained for encoding independently of window and file sizes.
    Direct proof callers have the same bound as the run pattern's fullmatch,
    including when its repeat is replaced. Rebind _BULK_E_LINE only together with
    a _BULK_E_RUN built from it, which takes the fullmatch branch.
    """
    if not raw:
        return None
    rows = _BULK_E_LINE.findall(raw)
    if not rows:
        return None
    times, equities, profits = columns = tuple(zip(*rows))
    # findall returns disjoint complete lines. Their token lengths plus
    # the nine literal bytes per line must cover every input byte, so no
    # prefix, gap or suffix can have been skipped. No second match is needed.
    if (sum(map(len, times)) + sum(map(len, equities)) + sum(map(len, profits))
            + _BULK_E_LITERAL_BYTES * len(rows) != len(raw)):
        return None
    # Coverage proves a concatenation of complete lines. Read its permitted
    # count from the run pattern itself, keeping that pattern the sole batch
    # bound even when replaced. Other pattern shapes keep their fullmatch.
    prefix, _, repeat_text = _BULK_E_RUN.pattern.rpartition(b"{")
    bounds = repeat_text[:-1].split(b",")
    if (prefix == _BULK_E_RUN_PREFIX and repeat_text.endswith(b"}")
            and len(bounds) == 2 and all(map(bytes.isdigit, bounds))
            and _BULK_E_RUN.flags == 0):
        lower, upper = map(int, bounds)
        if not lower <= len(rows) <= upper:
            return None
    elif _BULK_E_RUN.fullmatch(raw) is None:
        return None
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
    return _BulkEquity(raw, count, first, last, columns)


def _bulk_equity_ascii(batch):
    columns = getattr(batch, "columns", None)
    if columns is None:
        rows = _BULK_E_LINE.sub(
            rb'{"time_ms":\1,"equity":\2,"open_profit":\3},' + b"\n",
            batch.raw).splitlines()
        return b"".join(rows)[:-1] if rows else b""
    times, equities, profits = columns
    # Reuse the proof's captured text. zip/chain assemble the fixed keys
    # without another regex traversal or a replacement expansion per row.
    return b"".join(chain.from_iterable(zip(
        repeat(b'{"time_ms":'), times, repeat(b',"equity":'), equities,
        repeat(b',"open_profit":'), profits, repeat(b"},"))))[:-1]


def _bulk_equity_rows(batch):
    """Keep individual row boundaries for the ordinary array writer."""
    columns = getattr(batch, "columns", None)
    if columns is None:
        return _BULK_E_LINE.sub(
            rb'{"time_ms":\1,"equity":\2,"open_profit":\3},' + b"\n",
            batch.raw).splitlines()
    times, equities, profits = columns
    return list(map(b"".join, zip(
        repeat(b'{"time_ms":'), times, repeat(b',"equity":'), equities,
        repeat(b',"open_profit":'), profits, repeat(b"},"))))


def _trades(self, reader, sink=None):
    # The existing strict reader exhausts its trailer in this same pass;
    # canonical E windows need only their count and decoded endpoints.
    # O/V/K/X/H do not enter the trade fold. A fused sink supplies the
    # optional sink argument so those records can be dispatched to their
    # bounded artifacts while the reader hashes every record.
    for record in run_json._bulk_equity_records(reader):
        if isinstance(record, _BulkEquity):
            if not self.count_e:
                self.first = record.first
            self.last = record.last
            self.count_e += record.count
            if sink is not None:
                sink._append_equity_batch(record)
        elif record.tag == "E":
            point = record.as_dict()
            if not self.count_e:
                self.first = point
            self.last = point
            self.count_e += 1
            if self.equity_failure is None:
                try:
                    run_json._validate_report_result({"equity_curve": (point,)}, equity_only=True)
                except RunFailure as error:
                    # Preserve the exception, but drop its fold traceback
                    # so it cannot retain the reader or per-row frames.
                    # Assembly raises it after trade/summary/metric checks.
                    self.equity_failure = error
                    error.__traceback__ = None
            if sink is not None:
                sink._append_equity_record(point)
        elif record.tag == "T":
            self.trade_at_start |= record.values[12] <= 0
            if sink is not None:
                sink._append_trade_record(record)
            yield record
        elif sink is not None:
            sink._append_output_record(record)


def _append_trade_record(self, record):
    if (self.named or self._trades_spool is None
            or self._trades_spool_failed):
        return
    try:
        self._trades_spool.append(_sink_trade(record))
    except Exception as error:
        self._trades_spool_failed = True
        self._remember_spool_failure(error)


def _verify_fused(self, lib, state, report):
    from pf_sink import SinkFormatError
    diagnostics = _SinkReportDiagnostics()
    summary = summary_failure = None
    try:
        with _open_uncached_sink(self.path) as reader:
            self.header, self.counts = reader.header, reader.counts
            self._prepare_fused_artifacts(lib, state)
            trades = diagnostics.trades(reader, sink=self)
            try:
                summary = _sink_summary(report, self, records=trades)
            except RunFailure as error:
                # Keep strict/trailer/receipt/identity refusals ahead of
                # deferred result checks, and do not retain fold frames.
                summary_failure = error
                error.__traceback__ = None
            for _ in trades:
                pass
            self.header, self.counts, self.sha256 = (
                reader.header, reader.counts, reader.sha256)
            if self.header["run_id"] != self.run_id:
                raise _engine_invariant(
                    "the report sink run id differs from the request")
            receipt = ReportSinkReceiptC()
            receipt.struct_size, receipt.version = ctypes.sizeof(receipt), 1
            if (lib.strategy_report_sink_receipt_v1(
                    state, ctypes.byref(receipt)) != 0
                    or receipt.complete != 1):
                raise _engine_invariant(
                    "the engine's report sink receipt is incomplete")
            counts = {
                tag: getattr(receipt, "count_" + tag.lower())
                for tag in "ETOVKXH"
            }
            if (
                any(counts[tag] != self.counts.get(tag, 0)
                    for tag in counts)
                or bytes(receipt.sha256_hex) != self.sha256.encode("ascii")
                or int(report.total_trades) != self.counts["T"]
            ):
                raise _engine_invariant(
                    "the report sink trailer differs from its engine receipt")
            self._check_fused_identity(reader, self.path, self.named)
            self._close_fused_reader(reader)
    except SinkFormatError as error:
        raise _engine_invariant(
            f"the engine's report sink is invalid: {error}") from error
    self._identity = (
        reader._open_identity.st_ino, reader._open_identity.st_size)
    self.summary, self.summary_failure, self.diagnostics = (
        summary, summary_failure, diagnostics)


class _File:
    def __init__(self, file):
        import fcntl
        self.file = file
        self.buffer = None
        self.used = self.base = self.size = self.position = 0
        self.read_base, self.read_size = -1, 0
        self.reading = False
        self._dirty = False
        self.binary = False
        self.libc = ctypes.CDLL(None, use_errno=True)
        fd = file.fileno()
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise OSError("report artifacts require a regular file")
        if sys.platform.startswith("linux"):
            kind = _filesystem_type(fd, self.libc)
            memory_backed = kind in _MEMORY_FILESYSTEM_TYPES
            if not memory_backed:
                fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_DIRECT)
        elif sys.platform == "darwin":
            fcntl.fcntl(fd, 48, 1)  # F_NOCACHE: synchronous uncached file I/O
        else:
            raise OSError("uncached report artifact I/O is unavailable on this platform")
        for name in ("pread", "pwrite"):
            function = getattr(self.libc, name)
            function.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int64)
            function.restype = ctypes.c_ssize_t
        self.buffer = mmap.mmap(-1, self.window)
        self.address = ctypes.addressof(ctypes.c_char.from_buffer(self.buffer))

    def write(self, text):
        if self.reading:
            raise ValueError("a prepared report artifact is read-only")
        for offset in range(0, len(text), self.window):
            self.write_ascii(text[offset:offset + self.window].encode("ascii"))
        return len(text)

    def write_ascii(self, data):
        """Append already encoded ASCII through the same aligned I/O window."""
        if self.reading:
            raise ValueError("a prepared report artifact is read-only")
        position = 0
        while position < len(data):
            count = min(self.window - self.used, len(data) - position)
            self.buffer[self.used:self.used + count] = data[position:position + count]
            self.used += count
            self.size += count
            self._dirty = True
            position += count
            if self.used == self.window:
                self._sync_window()
        return len(data)

    def flush(self):
        self.file.flush()  # retain checked flush/close failure behavior
        if not self.reading and self._dirty:
            self._sync_window()
            # A full last window has no buffered bytes, but still needs a
            # barrier. A following rewind must not sync unchanged bytes again.
            if hasattr(os, "fdatasync"):
                os.fdatasync(self.file.fileno())
            else:
                os.fsync(self.file.fileno())
            self._dirty = False

    def close(self):
        if self.buffer is not None:
            self.buffer.close()
            self.buffer = None
        self.file.close()


def _append_ascii(self, data, *, row_count=1):
    if not data and row_count == 0:
        return
    try:
        if not self.empty:
            self.file.write_ascii(b",")
        self.file.write_ascii(data)
    except OSError as error:
        raise RunFailure("report sink write failed", "report_sink_failed",
                         {"reason": "scratch_io"}) from error
    self.empty = False
    self.length += row_count


@contextmanager
def install(monkeypatch):
    """Replace every modified dependency with its independent copied body."""
    with monkeypatch.context() as patch:
        for name in ("_BulkEquity", "_bulk_equity", "_bulk_equity_ascii", "_bulk_equity_rows"):
            patch.setattr(run_json, name, globals()[name])
        patch.setattr(run_json._SinkReportDiagnostics, "trades", _trades)
        patch.setattr(run_json._ReportSink, "_append_trade_record", _append_trade_record)
        patch.setattr(run_json._ReportSink, "_verify_fused", _verify_fused)
        patch.setattr(run_json._JSONColumn, "append_ascii", _append_ascii)
        for name in ("__init__", "write", "write_ascii", "flush", "close"):
            patch.setattr(run_json._BoundedJSONFile, name, getattr(_File, name))
        yield
