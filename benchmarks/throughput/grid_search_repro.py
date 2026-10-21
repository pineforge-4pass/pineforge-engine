#!/usr/bin/env python3
import argparse
import ctypes
import csv
from dataclasses import dataclass
import hashlib
import heapq
import itertools
import mmap
import os
from pathlib import Path
import struct
import sys

class pf_bar_t(ctypes.Structure):
    _fields_ = [
        ("open", ctypes.c_double),
        ("high", ctypes.c_double),
        ("low", ctypes.c_double),
        ("close", ctypes.c_double),
        ("volume", ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]

class pf_trade_t(ctypes.Structure):
    _fields_ = [
        ("entry_time", ctypes.c_int64),
        ("exit_time", ctypes.c_int64),
        ("entry_price", ctypes.c_double),
        ("exit_price", ctypes.c_double),
        ("pnl", ctypes.c_double),
        ("pnl_pct", ctypes.c_double),
        ("is_long", ctypes.c_int),
        ("max_runup", ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty", ctypes.c_double),
        ("commission", ctypes.c_double),
        ("entry_bar_index", ctypes.c_int32),
        ("exit_bar_index", ctypes.c_int32),
        ("open_at_end", ctypes.c_int32),   # ABI v3: range-end close row
    ]

class pf_trade_stats_t(ctypes.Structure):
    _fields_ = [
        ("num_trades", ctypes.c_int32), ("num_wins", ctypes.c_int32),
        ("num_losses", ctypes.c_int32), ("num_even", ctypes.c_int32),
        ("percent_profitable", ctypes.c_double),
        ("net_profit", ctypes.c_double), ("net_profit_pct", ctypes.c_double),
        ("gross_profit", ctypes.c_double), ("gross_profit_pct", ctypes.c_double),
        ("gross_loss", ctypes.c_double), ("gross_loss_pct", ctypes.c_double),
        ("profit_factor", ctypes.c_double),
        ("avg_trade", ctypes.c_double), ("avg_trade_pct", ctypes.c_double),
        ("avg_win", ctypes.c_double), ("avg_win_pct", ctypes.c_double),
        ("avg_loss", ctypes.c_double), ("avg_loss_pct", ctypes.c_double),
        ("ratio_avg_win_avg_loss", ctypes.c_double),
        ("largest_win", ctypes.c_double), ("largest_win_pct", ctypes.c_double),
        ("largest_loss", ctypes.c_double), ("largest_loss_pct", ctypes.c_double),
        ("commission_paid", ctypes.c_double),
        ("expectancy", ctypes.c_double),
        ("max_consecutive_wins", ctypes.c_int32), ("max_consecutive_losses", ctypes.c_int32),
        ("avg_bars_in_trade", ctypes.c_double), ("avg_bars_in_wins", ctypes.c_double),
        ("avg_bars_in_losses", ctypes.c_double),
    ]

class pf_equity_stats_t(ctypes.Structure):
    _fields_ = [
        ("max_equity_drawdown", ctypes.c_double), ("max_equity_drawdown_pct", ctypes.c_double),
        ("max_equity_runup", ctypes.c_double), ("max_equity_runup_pct", ctypes.c_double),
        ("buy_hold_return", ctypes.c_double), ("buy_hold_return_pct", ctypes.c_double),
        # The C field names. The JSON report keys of these two stay
        # sharpe_tv / sortino_tv (ADR-0001, "Deprecated public spellings").
        ("sharpe_monthly", ctypes.c_double), ("sortino_monthly", ctypes.c_double),
        ("sharpe_bar", ctypes.c_double), ("sortino_bar", ctypes.c_double),
        ("cagr", ctypes.c_double), ("calmar", ctypes.c_double),
        ("recovery_factor", ctypes.c_double), ("time_in_market_pct", ctypes.c_double),
        ("open_pl", ctypes.c_double),
    ]

class pf_metrics_t(ctypes.Structure):
    _fields_ = [("all", pf_trade_stats_t), ("longs", pf_trade_stats_t),
                ("shorts", pf_trade_stats_t), ("equity", pf_equity_stats_t)]

class pf_equity_point_t(ctypes.Structure):
    _fields_ = [("time_ms", ctypes.c_int64), ("equity", ctypes.c_double),
                ("open_profit", ctypes.c_double)]

class pf_security_diag_t(ctypes.Structure):
    _fields_ = [
        ("sec_id", ctypes.c_int),
        ("feed_count", ctypes.c_int64),
        ("complete_count", ctypes.c_int64),
        ("partial_count", ctypes.c_int64),
    ]

class pf_trace_entry_t(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id", ctypes.c_int32),
        ("value", ctypes.c_double),
    ]

class pf_report_t(ctypes.Structure):
    # Legacy layout retained for the remote reference comparison. HPO never
    # constructs one or binds report_free.
    _fields_ = [
        ("total_trades", ctypes.c_int),
        ("trades", ctypes.POINTER(pf_trade_t)),
        ("trades_len", ctypes.c_int),
        ("net_profit", ctypes.c_double),

        ("input_bars_processed", ctypes.c_int64),
        ("script_bars_processed", ctypes.c_int64),

        ("security_feeds_total", ctypes.c_int64),
        ("security_complete_total", ctypes.c_int64),
        ("security_partial_total", ctypes.c_int64),

        ("magnifier_sub_bars_total", ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),

        ("input_tf_seconds", ctypes.c_int),
        ("script_tf_seconds", ctypes.c_int),
        ("script_tf_ratio", ctypes.c_int),
        ("needs_aggregation", ctypes.c_int),
        ("bar_magnifier_enabled", ctypes.c_int),

        ("security_diag", ctypes.POINTER(pf_security_diag_t)),
        ("security_diag_len", ctypes.c_int),

        ("trace", ctypes.POINTER(pf_trace_entry_t)),
        ("trace_len", ctypes.c_int),
        ("trace_names", ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len", ctypes.c_int),

        ("metrics", pf_metrics_t),
        ("equity_curve", ctypes.POINTER(pf_equity_point_t)),
        ("equity_curve_len", ctypes.c_int64),  # int64, NOT c_int
        ("broker_state_hash", ctypes.POINTER(ctypes.c_uint64)),
        ("broker_state_hash_len", ctypes.c_int64),
    ]

# pf_report_t is caller-allocated; a layout mismatch means the runtime
# writes past this script's report buffer. Verify the ABI before running.
# v4 appended the live-runtime accessors and grew pf_report_t with the
# broker_state_hash array after equity_curve_len (ReportC above already
# carries both fields).
EXPECTED_PF_ABI = 4


class pf_report_summary_v1(ctypes.Structure):
    """Benchmark bridge projection of A1CTR SummaryV1; no owned sequences.

    The exact pending declarations are recorded in REPORT.md. This mirror is
    additive: pf_report_t and PF_ABI_VERSION retain their existing layouts.
    """
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("output_mode", ctypes.c_uint32),
        ("complete", ctypes.c_uint32),
        ("total_trades", ctypes.c_int64),
        ("equity_point_count", ctypes.c_int64),
        ("input_bars_processed", ctypes.c_int64),
        ("script_bars_processed", ctypes.c_int64),
        ("net_profit", ctypes.c_double),
        ("metrics", pf_metrics_t),
    ]


class pf_hpo_scratch_manifest_v1(ctypes.Structure):
    """Resident windows, not logical table capacities; see scratch_defaults.hpp."""
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("root_bytes", ctypes.c_uint64),
        ("entry_bytes", ctypes.c_uint64),
        ("cohort_bytes", ctypes.c_uint64),
        ("transaction_bytes", ctypes.c_uint64),
        ("resident_bytes", ctypes.c_uint64),
    ]


@dataclass(frozen=True)
class HpoScratchSpec:
    page_bytes: int = 4096
    root_pages: int = 16
    entry_pages: int = 16
    cohort_pages: int = 16
    transaction_pages: int = 16


def derive_hpo_scratch_manifest(spec):
    """Derive the C07 reservation from fixed cache geometry before bar 0."""
    geometry = (spec.page_bytes, spec.root_pages, spec.entry_pages,
                spec.cohort_pages, spec.transaction_pages)
    if any(type(value) is not int or value <= 0 for value in geometry):
        raise ValueError("HPO scratch windows must be positive integer sizes")
    sizes = tuple(spec.page_bytes * pages for pages in geometry[1:])
    maximum = (1 << 64) - 1
    if any(size > maximum for size in sizes) or sum(sizes) > maximum:
        raise OverflowError("HPO scratch resident byte overflow")
    return pf_hpo_scratch_manifest_v1(
        ctypes.sizeof(pf_hpo_scratch_manifest_v1), 1, *sizes, sum(sizes))


@dataclass(frozen=True)
class TrialResult:
    net_profit: float
    total_trades: int
    equity_point_count: int
    input_bars_processed: int
    script_bars_processed: int
    metrics: bytes


def load_csv_bars(path):
    """Allocate shared OHLCV once; fill it directly in a second CSV pass.

    A file with no data rows is refused with the typed `InputError` before an
    array is allocated: a zero-bar input would otherwise run every trial over
    nothing and still print a "Best" line with exit 0. This matches the
    `--ohlcv-bin` empty-file refusal in `load_bars_bin` (reason "empty").
    """
    def rows():
        with path.open("r", newline="") as stream:
            reader = csv.reader(stream)
            next(reader, None)
            for row in reader:
                if len(row) >= 6:
                    yield row

    count = sum(1 for _ in rows())
    if count == 0:
        raise InputError(
            f"OHLCV CSV file has no data rows (header only): {path}",
            "empty_ohlcv_csv")
    bars = (pf_bar_t * count)()
    seen = 0
    for row in rows():
        if seen == count:
            raise RuntimeError("OHLCV CSV changed while loading")
        bar = bars[seen]
        bar.timestamp = int(row[0])
        bar.open = float(row[1])
        bar.high = float(row[2])
        bar.low = float(row[3])
        bar.close = float(row[4])
        bar.volume = float(row[5])
        seen += 1
    if seen != count:
        raise RuntimeError("OHLCV CSV changed while loading")
    return bars


# --- pf_bar_t/v1 binary input (docs/design/BARS-BIN-INPUT.md section 1) ------
# Headerless 48-byte records, struct '<5dq': open, high, low, close, volume as
# little-endian binary64, then the bar-open timestamp as a little-endian int64
# Unix millisecond. There is no magic, version or count in the bar file itself;
# the sidecar (PATH.json, "format": "pf_bar_t/v1") carries the format id.
BAR_RECORD_SIZE = 48
# The engine's run entry takes a 32-bit bar count.
MAX_BIN_BARS = (1 << 31) - 1
_BAR_FIELD_OFFSETS = (0, 8, 16, 24, 32, 40)


class BarsBinError(ValueError):
    """Typed refusal for a --ohlcv-bin file this host cannot map safely."""

    def __init__(self, message, reason):
        super().__init__(message)
        self.reason = reason


class InputError(RuntimeError):
    """A required input is missing; the sweep must run zero trials.

    This is the typed missing-input path: `main` returns `exit_code` (never
    None), so `cli` passes a non-zero process code through instead of mapping
    the error to 0. `kind` names the failed check.
    """

    exit_code = 1

    def __init__(self, message, kind):
        super().__init__(message)
        self.kind = kind


def load_bars_bin(path):
    """Map a pf_bar_t/v1 file read-only and copy it into one ctypes array.

    Refuses before mapping: a host whose pf_bar_t is not the 48-byte
    little-endian wire record, a non-regular file, a size that is not a
    multiple of 48, an empty file, and a count the engine cannot accept.
    No per-row Python object is constructed; the CSV path is unchanged and
    both loaders return the same contiguous array shape.
    """
    path = Path(path)
    if sys.byteorder != "little" or ctypes.sizeof(pf_bar_t) != BAR_RECORD_SIZE:
        raise BarsBinError(
            "binary bars require a 48-byte little-endian pf_bar_t host",
            "host_layout")
    offsets = tuple(getattr(pf_bar_t, name).offset for name, _ in pf_bar_t._fields_)
    if offsets != _BAR_FIELD_OFFSETS:
        raise BarsBinError(
            f"pf_bar_t field offsets {offsets} are not the pf_bar_t/v1 layout",
            "host_layout")
    if not path.is_file():
        raise BarsBinError(f"binary bar input is not a regular file: {path}",
                           "not_a_file")
    size = path.stat().st_size
    if size % BAR_RECORD_SIZE:
        raise BarsBinError(
            f"binary bar file size {size} is not a multiple of {BAR_RECORD_SIZE}",
            "size_not_multiple_of_48")
    count = size // BAR_RECORD_SIZE
    if count == 0:
        raise BarsBinError("binary bar file is empty", "empty")
    if count > MAX_BIN_BARS:
        raise BarsBinError(
            f"binary bar count {count} exceeds the engine limit {MAX_BIN_BARS}",
            "too_many_bars")
    # map read-only: the input is never written, and a bad size was refused
    # before any mapping exists.
    with path.open("rb") as stream:
        mapping = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
    try:
        # from_buffer_copy on the read-only map builds the whole array in one
        # allocation, exactly as the engine's own load_bars_bin does.
        bars = (pf_bar_t * count).from_buffer_copy(mapping)
    finally:
        mapping.close()
    return bars


class HpoRuntime:
    """Synchronous summary trials over one caller-owned immutable bar array."""

    def __init__(self, lib, scratch_spec=HpoScratchSpec()):
        check_abi(lib)
        self.lib = lib
        # Resolve the complete summary contract before creating any handle.
        # A library lacking it cannot silently enter the legacy allocation path.
        declarations = (
            ("pf_report_output_version_v1", [], ctypes.c_uint32),
            ("strategy_create", [ctypes.c_char_p], ctypes.c_void_p),
            ("strategy_free", [ctypes.c_void_p], None),
            ("strategy_set_input", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p], None),
            ("strategy_set_report_output_mode_v1", [ctypes.c_void_p, ctypes.c_uint32], ctypes.c_int),
            ("strategy_set_hpo_scratch_manifest_v1",
             [ctypes.c_void_p, ctypes.POINTER(pf_hpo_scratch_manifest_v1)], ctypes.c_int),
            ("strategy_hpo_scratch_manifest_v1",
             [ctypes.c_void_p, ctypes.POINTER(pf_hpo_scratch_manifest_v1)], ctypes.c_int),
            ("strategy_report_summary_v1",
             [ctypes.c_void_p, ctypes.POINTER(pf_report_summary_v1)], ctypes.c_int),
            ("strategy_run_hpo_v1",
             [ctypes.c_void_p, ctypes.POINTER(pf_bar_t), ctypes.c_int], ctypes.c_int),
            ("strategy_get_last_error", [ctypes.c_void_p], ctypes.c_char_p),
            ("strategy_get_last_error_code", [ctypes.c_void_p], ctypes.c_char_p),
            ("strategy_last_run_status", [ctypes.c_void_p], ctypes.c_int),
        )
        for name, arguments, result in declarations:
            try:
                function = getattr(lib, name)
            except AttributeError as error:
                raise RuntimeError(f"HPO requires the summary contract: missing {name}") from error
            function.argtypes = arguments
            function.restype = result
        if lib.pf_report_output_version_v1() != 1:
            raise RuntimeError("HPO requires report output contract version 1")
        self.manifest = derive_hpo_scratch_manifest(scratch_spec)

    def trial(self, bars, inputs):
        handle = self.lib.strategy_create(None)
        if not handle:
            raise RuntimeError("Failed to create strategy instance")
        try:
            for key, value in inputs:
                self.lib.strategy_set_input(handle, key, str(value).encode())
            if self.lib.strategy_set_report_output_mode_v1(handle, 1) != 0:
                raise RuntimeError("HPO summary mode installation failed")
            if self.lib.strategy_set_hpo_scratch_manifest_v1(
                    handle, ctypes.byref(self.manifest)) != 0:
                raise RuntimeError("HPO resident-window manifest installation failed")
            self._check_manifest(handle)
            # The dedicated entry has no report argument or report producer.
            # Summary mode suppresses curve/trade/hash capture during execution.
            status = self.lib.strategy_run_hpo_v1(handle, bars, len(bars))
            self._check_run(handle)
            if status != 0:
                raise RuntimeError("HPO trial did not complete")
            self._check_manifest(handle)
            summary = pf_report_summary_v1()
            summary.struct_size = ctypes.sizeof(summary)
            summary.version = 1
            if self.lib.strategy_report_summary_v1(handle, ctypes.byref(summary)) != 0:
                raise RuntimeError("HPO frozen summary read failed")
            if (summary.struct_size != ctypes.sizeof(summary) or summary.version != 1
                    or summary.output_mode != 1 or summary.complete != 1):
                raise RuntimeError("HPO returned an incomplete or incompatible summary")
            if (summary.total_trades < 0 or summary.equity_point_count < 0
                    or summary.input_bars_processed != len(bars)
                    or summary.script_bars_processed < 0):
                raise RuntimeError("HPO summary completion counts are invalid")
            return TrialResult(
                summary.net_profit, summary.total_trades, summary.equity_point_count,
                summary.input_bars_processed, summary.script_bars_processed,
                bytes(summary.metrics))
        finally:
            self.lib.strategy_free(handle)

    def _check_run(self, handle):
        message = self.lib.strategy_get_last_error(handle)
        if self.lib.strategy_last_run_status(handle) != 0 or message:
            code = self.lib.strategy_get_last_error_code(handle) or b""
            text = (message or b"HPO trial failed").decode("utf-8", "replace")
            raise RuntimeError(f"{text} [{code.decode('utf-8', 'replace')}]")

    def _check_manifest(self, handle):
        receipt = pf_hpo_scratch_manifest_v1()
        receipt.struct_size = ctypes.sizeof(receipt)
        receipt.version = 1
        if self.lib.strategy_hpo_scratch_manifest_v1(handle, ctypes.byref(receipt)) != 0:
            raise RuntimeError("HPO resident-window receipt read failed")
        if bytes(receipt) != bytes(self.manifest):
            raise RuntimeError("HPO installed resident windows differ from the fixed spec")

def check_abi(lib):
    try:
        lib.pf_abi_version.restype = ctypes.c_int
        abi = lib.pf_abi_version()
    except AttributeError:
        raise RuntimeError(
            "strategy library predates pf_abi_version (ABI v1); rebuild it "
            "against the current pineforge runtime (pf_report_t grew).")
    if abi != EXPECTED_PF_ABI:
        raise RuntimeError(
            f"pineforge ABI mismatch: library reports {abi}, harness expects "
            f"{EXPECTED_PF_ABI}; rebuild.")


# --- CR-220: streamed TrialRecord retention ----------------------------------
# Every finished trial is appended to an append-only file as one fixed-size
# record and flushed before the next trial starts. Only the top-k records stay
# in memory, so retained host state is O(k) in the number of trials, not O(N).
#
# File layout (little-endian, fixed offsets, no on-disk padding):
#   64-byte header  '<8sIIII' + 40 zero bytes:
#     0  magic b"PFTRIAL\x01" | 8 version=1 | 12 record_size=128
#     16 param_slots=8 | 20 reserved=0
#   128-byte records '<IIdqII32s8d', in trial order:
#     0   uint32  trial_index   (0-based, strictly increasing)
#     4   uint32  status        (0 = ok, 1 = failed)
#     8   float64 net_profit    (IEEE bits kept; NaN on failure)
#     16  int64   total_trades  (-1 on failure)
#     24  uint32  param_count
#     28  uint32  reserved = 0
#     32  32      sha256 of the trial's 768-byte raw metrics block (zeros on failure)
#     64  64      8 x float64 parameter values; unused slots hold 0.0
#
# Disk cap and removal policy (caller-owned artifact):
# The log is CALLER-OWNED: the caller named the path and the sweep created it
# exclusively. The host never deletes it, on success and not on failure, and
# without --trial-log no file exists at all. This is deliberately different
# from the engine's CR-150 scratch files (include/pineforge/detail/
# scratch_file.hpp:29-70): those are run-owned, anonymous and unlinked at
# creation, so the run's own backing vanishes with the run. The trial log
# exists because the caller asked for it as the sweep's result, so it outlives
# the sweep and belongs to whoever named it.
# A per-run byte cap applies (--trial-log-max-bytes, default 1 GiB, the 64-byte
# header included). A record that would push the file past the cap is refused
# with a typed TrialLogCapError before any byte of it is written; records
# already written stay whole and readable. The log is never truncated, never
# rotated, and no record is dropped silently.
TRIAL_LOG_MAGIC = b"PFTRIAL\x01"
TRIAL_LOG_VERSION = 1
TRIAL_LOG_HEADER_SIZE = 64
TRIAL_LOG_RECORD_SIZE = 128
TRIAL_PARAM_SLOTS = 8
TRIAL_STATUS_OK = 0
TRIAL_STATUS_FAILED = 1
DEFAULT_TOP_K = 10
# 1 GiB, header included: (2**30 - 64) // 128 = 8,388,607 whole records after
# the header, then the cap refuses the next record.
DEFAULT_TRIAL_LOG_MAX_BYTES = 1 << 30
# The old best-1 scan initialised `best_profit = -float("inf")` and used a
# strict `>`; a NaN or an -inf net profit therefore never won.
STRICT_BEST_FLOOR = -float("inf")

_TRIAL_LOG_HEADER = struct.Struct("<8sIIII")
_TRIAL_LOG_RECORD = struct.Struct("<IIdqII32s8d")
if (_TRIAL_LOG_HEADER.size > TRIAL_LOG_HEADER_SIZE
        or _TRIAL_LOG_RECORD.size != TRIAL_LOG_RECORD_SIZE):
    raise RuntimeError("trial log struct geometry does not match the fixed layout")


class TrialLogError(ValueError):
    """The append-only trial log is truncated, mislabelled or out of order."""


class TrialLogCapError(TrialLogError):
    """The next record would push the caller-owned log past its byte cap.

    Subclass of TrialLogError, the host's trial-log I/O error family. Raised
    before any byte of the refused record is written; the bytes already on
    disk are flushed and every written record stays whole and readable.
    """

    def __init__(self, message, *, written_bytes, cap_bytes, trial_index):
        super().__init__(message)
        self.written_bytes = written_bytes
        self.cap_bytes = cap_bytes
        self.trial_index = trial_index


@dataclass(frozen=True)
class TrialRecord:
    """One finished trial; fixed fields, fixed size, no reference to the host."""

    trial_index: int
    status: int
    net_profit: float
    total_trades: int
    metrics_sha256: bytes
    params: tuple

    def pack(self):
        if len(self.metrics_sha256) != 32:
            raise ValueError("TrialRecord needs a 32-byte metrics digest")
        if len(self.params) > TRIAL_PARAM_SLOTS:
            raise ValueError(
                f"a TrialRecord carries at most {TRIAL_PARAM_SLOTS} parameters")
        slots = tuple(self.params) + (0.0,) * (TRIAL_PARAM_SLOTS - len(self.params))
        return _TRIAL_LOG_RECORD.pack(
            self.trial_index, self.status, self.net_profit, self.total_trades,
            len(self.params), 0, self.metrics_sha256, *slots)

    @classmethod
    def unpack(cls, payload):
        if len(payload) != TRIAL_LOG_RECORD_SIZE:
            raise TrialLogError(
                f"trial record is {len(payload)} bytes, not {TRIAL_LOG_RECORD_SIZE}")
        (index, status, profit, trades, count, _reserved, digest,
         *slots) = _TRIAL_LOG_RECORD.unpack(payload)
        if count > TRIAL_PARAM_SLOTS:
            raise TrialLogError(f"trial {index} claims {count} parameters")
        return cls(index, status, profit, trades, digest, tuple(slots[:count]))


class TrialLog:
    """Append-only writer: one flushed record per trial, no in-memory history.

    The file is created exclusively; an existing path is never truncated. It
    is caller-owned: the host never deletes it, on success or on failure. The
    64-byte header counts against `max_bytes`; a record that would cross the
    cap is refused (TrialLogCapError) before any byte of it is written.
    """

    def __init__(self, path, max_bytes=DEFAULT_TRIAL_LOG_MAX_BYTES):
        if type(max_bytes) is not int or max_bytes <= 0:
            raise ValueError("trial log max bytes must be a positive integer")
        if max_bytes < TRIAL_LOG_HEADER_SIZE:
            # Refuse before creating the file: no record could ever fit.
            raise TrialLogCapError(
                f"trial log cap {max_bytes} bytes cannot hold the "
                f"{TRIAL_LOG_HEADER_SIZE}-byte header",
                written_bytes=0, cap_bytes=max_bytes, trial_index=None)
        self.path = Path(path)
        self.max_bytes = max_bytes
        self.bytes_written = 0
        self.records_written = 0
        self._stream = self.path.open("xb")
        try:
            header = _TRIAL_LOG_HEADER.pack(
                TRIAL_LOG_MAGIC, TRIAL_LOG_VERSION, TRIAL_LOG_RECORD_SIZE,
                TRIAL_PARAM_SLOTS, 0)
            self._stream.write(header)
            self._stream.write(b"\0" * (TRIAL_LOG_HEADER_SIZE - len(header)))
            self._stream.flush()
            self.bytes_written = TRIAL_LOG_HEADER_SIZE
        except BaseException:
            self._stream.close()
            self._stream = None
            raise

    def write(self, record):
        if self._stream is None:
            raise TrialLogError("trial log is closed")
        # The size the file actually has: every earlier record was flushed, so
        # this is the on-disk size the cap is compared against.
        written = os.fstat(self._stream.fileno()).st_size
        projected = written + TRIAL_LOG_RECORD_SIZE
        if projected > self.max_bytes:
            # Cap refusal happens before any byte of this record is written.
            # Flush what is already on disk so the recorded prefix stays whole
            # and readable by iter_trial_log: never a truncation, never a
            # partial record, never a silent drop.
            self._stream.flush()
            raise TrialLogCapError(
                f"trial {record.trial_index} would grow the trial log to "
                f"{projected} bytes, past the {self.max_bytes}-byte cap",
                written_bytes=written,
                cap_bytes=self.max_bytes,
                trial_index=record.trial_index)
        self._stream.write(record.pack())
        self._stream.flush()
        self.bytes_written = projected
        self.records_written += 1

    def close(self):
        if self._stream is not None:
            self._stream.close()
            self._stream = None

    def __enter__(self):
        return self

    def __exit__(self, *_exception):
        self.close()
        return False


def iter_trial_log(path):
    """Read back what TrialLog wrote; a verification path, never the sweep's."""
    with Path(path).open("rb") as stream:
        header = stream.read(TRIAL_LOG_HEADER_SIZE)
        if len(header) != TRIAL_LOG_HEADER_SIZE:
            raise TrialLogError("trial log header is truncated")
        magic, version, record_size, slots, _reserved = _TRIAL_LOG_HEADER.unpack(
            header[:_TRIAL_LOG_HEADER.size])
        if magic != TRIAL_LOG_MAGIC or version != TRIAL_LOG_VERSION:
            raise TrialLogError("trial log has an unknown magic or version")
        if record_size != TRIAL_LOG_RECORD_SIZE or slots != TRIAL_PARAM_SLOTS:
            raise TrialLogError("trial log record geometry is not the fixed layout")
        expected = 0
        while True:
            payload = stream.read(TRIAL_LOG_RECORD_SIZE)
            if not payload:
                return
            if len(payload) != TRIAL_LOG_RECORD_SIZE:
                raise TrialLogError(f"trial log record {expected} is truncated")
            record = TrialRecord.unpack(payload)
            if record.trial_index != expected:
                raise TrialLogError(
                    f"trial log record {expected} has index {record.trial_index}")
            expected += 1
            yield record


class _RankedTrial:
    """Heap entry: higher net profit is better; on a tie the lower index wins."""

    __slots__ = ("record", "key")

    def __init__(self, record):
        self.record = record
        self.key = (record.net_profit, -record.trial_index)

    def __lt__(self, other):
        return self.key < other.key


class TopK:
    """Bounded ranking heap: at most k records are retained, whatever N is.

    A failed trial, a NaN net profit and an -inf net profit never enter the
    ranking, matching the old scan's strict `profit > best` comparison against
    its `-inf` initial best. `ranked()` returns the kept records best first:
    descending net profit, then ascending trial index, so k=1 reproduces the
    old first-wins best exactly (including "no best" when every trial is -inf).
    """

    def __init__(self, k=DEFAULT_TOP_K):
        if type(k) is not int or k < 1:
            raise ValueError("--top-k must be a positive integer")
        self.k = k
        self._heap = []

    def __len__(self):
        return len(self._heap)

    def offer(self, record):
        if record.status != TRIAL_STATUS_OK:
            return False
        profit = record.net_profit
        # The old scan initialised its best to -inf and compared strictly, so
        # neither NaN nor -inf can win. This keeps k=1 identical to it.
        if not profit > STRICT_BEST_FLOOR:
            return False
        entry = _RankedTrial(record)
        if len(self._heap) < self.k:
            heapq.heappush(self._heap, entry)
            return True
        if self._heap[0] < entry:
            heapq.heapreplace(self._heap, entry)
            return True
        return False

    def ranked(self):
        return tuple(entry.record for entry in sorted(self._heap, reverse=True))


PARAMETER_NAMES = (b"Fast EMA", b"Slow EMA", b"Take profit (ticks)")
FAST_EMA_RANGE = (3, 5, 7)
SLOW_EMA_RANGE = (11, 13, 15)
TAKE_PROFIT_RANGE = (10, 15, 20)


def default_grid():
    """The 27 default combinations, produced lazily (never materialized)."""
    return itertools.product(FAST_EMA_RANGE, SLOW_EMA_RANGE, TAKE_PROFIT_RANGE)


def _format_param(value):
    """Print a recorded parameter the way the old int-valued sweep printed it."""
    return f"{value:g}"


def _print_trial(_index, record):
    fast, slow, tp = record.params
    print(f"Params: Fast_EMA={_format_param(fast)}, Slow_EMA={_format_param(slow)}, "
          f"TP_ticks={_format_param(tp)} | Net Profit = {record.net_profit:.2f} | "
          f"Trades = {record.total_trades}")


@dataclass(frozen=True)
class SweepResult:
    """Fixed-size sweep outcome: the trial count and at most k records."""

    trials: int
    ranked: tuple


def run_sweep(runtime, bars, grid, trial_log=None, top_k=DEFAULT_TOP_K,
              observer=None, trial_log_max_bytes=DEFAULT_TRIAL_LOG_MAX_BYTES):
    """Run every grid trial, stream each record, keep only the top k.

    `grid` is consumed lazily. A failed trial is written with status 1 and
    then re-raised, so it can never enter the ranking. `observer(index,
    record)`, when given, sees each finished record once and must not retain
    it (the host itself keeps none beyond the k-sized heap). A record that
    would cross `trial_log_max_bytes` raises TrialLogCapError before it is
    written, aborting the sweep with the caller-owned log left whole.
    """
    ranking = TopK(top_k)
    log = (TrialLog(trial_log, trial_log_max_bytes)
           if trial_log is not None else None)
    trials = 0
    try:
        for index, params in enumerate(grid):
            if len(params) != len(PARAMETER_NAMES):
                raise ValueError(
                    f"grid combination {index} has {len(params)} parameters, "
                    f"the host passes {len(PARAMETER_NAMES)}")
            values = tuple(float(value) for value in params)
            try:
                result = runtime.trial(bars, tuple(
                    (name, value)
                    for name, value in zip(PARAMETER_NAMES, params)))
            except Exception as trial_error:
                record = TrialRecord(index, TRIAL_STATUS_FAILED, float("nan"), -1,
                                     bytes(32), values)
                if log is not None:
                    try:
                        log.write(record)
                    except TrialLogCapError as cap_error:
                        # The failed trial's own record crossed the cap. Do not
                        # let the cap refusal hide the real trial failure: the
                        # printed cap message names it and the original
                        # exception is chained as __cause__.
                        raise TrialLogCapError(
                            f"{cap_error}; the trial itself failed: {trial_error}",
                            written_bytes=cap_error.written_bytes,
                            cap_bytes=cap_error.cap_bytes,
                            trial_index=cap_error.trial_index) from trial_error
                raise
            record = TrialRecord(
                trial_index=index, status=TRIAL_STATUS_OK,
                net_profit=result.net_profit, total_trades=result.total_trades,
                metrics_sha256=hashlib.sha256(result.metrics).digest(),
                params=values)
            if log is not None:
                log.write(record)
            ranking.offer(record)
            trials += 1
            if observer is not None:
                observer(index, record)
    finally:
        if log is not None:
            log.close()
    return SweepResult(trials, ranking.ranked())


# The swept bench slot: the public analogue of the retired 19-scalping-wunder-bots
# (an EMA-cross scalper with tick-denominated take-profit / stop-loss exits).
SLUG = "021-composite-scalping-integration-01"

def main(library_path=None, csv_path=None, bin_path=None, trial_log=None,
         top_k=DEFAULT_TOP_K,
         trial_log_max_bytes=DEFAULT_TRIAL_LOG_MAX_BYTES):
    repo_root = Path(__file__).parent.parent.parent.resolve()
    dylib_path = (Path(library_path) if library_path is not None else
                  repo_root / "benchmarks/assets/strategies" / SLUG / "strategy.dylib")

    if not dylib_path.exists():
        # Fallback to shared library extension suffix on Linux
        dylib_path = dylib_path.with_suffix(".so")

    if not dylib_path.exists():
        error = InputError(
            f"compiled strategy library not found at: {dylib_path}; build the "
            "HPO bridge module as described in benchmarks/throughput/README.md",
            "missing_library")
        print(f"Error: {error}", file=sys.stderr)
        return error.exit_code

    csv_path = (Path(csv_path) if csv_path is not None else
                repo_root / "benchmarks/assets/data/ETHUSDT_15.csv")
    if bin_path is not None:
        bin_path = Path(bin_path)
        if not bin_path.is_file():
            error = InputError(
                f"OHLCV binary file not found at: {bin_path}",
                "missing_ohlcv_bin")
            print(f"Error: {error}", file=sys.stderr)
            return error.exit_code
    elif not csv_path.exists():
        error = InputError(
            f"OHLCV CSV file not found at: {csv_path}", "missing_ohlcv_csv")
        print(f"Error: {error}", file=sys.stderr)
        return error.exit_code

    # Load shared library
    lib = ctypes.CDLL(str(dylib_path))
    runtime = HpoRuntime(lib)

    # Load exactly one shared bar array, from the CSV or the binary input.
    if bin_path is not None:
        print(f"Loading binary OHLCV bars from {bin_path.name}...")
        bars_array = load_bars_bin(bin_path)
    else:
        print(f"Loading CSV data from {csv_path.name}...")
        bars_array = load_csv_bars(csv_path)
    n = len(bars_array)
    print(f"Loaded {n} bars successfully.")

    # Grid Search Parameters (around the script's defaults 5 / 13 / 15; the
    # "Stop loss (ticks)" input keeps its default of 7)
    combinations = len(FAST_EMA_RANGE) * len(SLOW_EMA_RANGE) * len(TAKE_PROFIT_RANGE)

    print(f"Starting parameter sweep on {SLUG} strategy across {combinations} combinations...")

    try:
        sweep = run_sweep(runtime, bars_array, default_grid(),
                          trial_log=trial_log, top_k=top_k,
                          observer=_print_trial,
                          trial_log_max_bytes=trial_log_max_bytes)
    except TrialLogCapError as error:
        # Abort the sweep with a non-zero process exit. The caller-owned log
        # is left on disk exactly as written; the host never deletes it.
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print("\n=== OPTIMIZATION COMPLETE ===")
    if not sweep.ranked:
        # The old best-1 scan kept its -inf sentinel and printed "Best Profit:
        # -inf" before failing to index the absent parameter tuple. k=1 keeps
        # that printed prefix identical; there is no parameter line to print.
        print(f"Best Profit: {STRICT_BEST_FLOOR:.2f}")
        raise RuntimeError("sweep produced no rankable trial")
    best = sweep.ranked[0]
    print(f"Best Profit: {best.net_profit:.2f}")
    print(f"Best Parameters: Fast EMA = {_format_param(best.params[0])}, "
          f"Slow EMA = {_format_param(best.params[1])}, "
          f"Take profit (ticks) = {_format_param(best.params[2])}")

    if top_k > 1:
        print(f"\nTop {len(sweep.ranked)} of {sweep.trials} trials "
              f"(ties: lower trial index wins):")
        for rank, record in enumerate(sweep.ranked, 1):
            print(f"  #{rank} trial {record.trial_index}: "
                  f"Net Profit = {record.net_profit:.2f} | "
                  f"Trades = {record.total_trades} | "
                  f"Fast EMA = {_format_param(record.params[0])}, "
                  f"Slow EMA = {_format_param(record.params[1])}, "
                  f"Take profit (ticks) = {_format_param(record.params[2])}")
    return 0


def positive_int(text):
    """argparse type: a decimal integer strictly greater than zero."""
    try:
        value = int(text)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError(
            f"{text!r} is not an integer") from None
    if value <= 0:
        raise argparse.ArgumentTypeError(
            f"{value} is not a positive integer")
    return value


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description="PineForge summary-mode HPO sweep")
    parser.add_argument("--library", type=Path, help="strategy module built with hpo_bridge.cpp")
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument("--ohlcv-csv", type=Path, help="shared input CSV")
    inputs.add_argument("--ohlcv-bin", type=Path,
                        help="pf_bar_t/v1 binary bars (headerless 48-byte records)")
    parser.add_argument("--trial-log", type=Path,
                        help="append-only TrialRecord log, created exclusively")
    parser.add_argument("--trial-log-max-bytes", type=positive_int,
                        default=DEFAULT_TRIAL_LOG_MAX_BYTES,
                        help="per-run byte cap for --trial-log, 64-byte header "
                             f"included (default {DEFAULT_TRIAL_LOG_MAX_BYTES})")
    parser.add_argument("--top-k", type=positive_int, default=DEFAULT_TOP_K,
                        help=f"records kept in memory for the ranking, a "
                             f"positive integer (default {DEFAULT_TOP_K})")
    return parser.parse_args(argv)


def cli(argv=None):
    """Parse argv and return the process exit code.

    0 ok; 1 for a capped sweep, a typed binary-bar or CSV input refusal, or a
    missing input (the int `main` returns is passed through unchanged); 2 from
    argparse. A missing or empty input never maps to 0.

    A `main` result that is not an int raises `TypeError` here rather than
    being coerced: `sys.exit(None)` exits 0, so no unset code may become a
    success.
    """
    arguments = parse_arguments(argv)
    try:
        code = main(arguments.library, arguments.ohlcv_csv, arguments.ohlcv_bin,
                    arguments.trial_log, arguments.top_k,
                    arguments.trial_log_max_bytes)
    except InputError as error:
        # A typed empty or missing input found while loading (for example a
        # header-only CSV) is the same refusal as `main`'s pre-checks.
        print(f"Error: {error}", file=sys.stderr)
        return error.exit_code
    except BarsBinError as error:
        # A typed malformed --ohlcv-bin input is an input error, not a crash.
        print(f"Error: {error}", file=sys.stderr)
        return 1
    if type(code) is not int:
        raise TypeError(
            f"main() returned {code!r}, not an int exit code; "
            "sys.exit(None) would exit 0")
    return code


if __name__ == "__main__":
    sys.exit(cli())
