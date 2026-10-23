#!/usr/bin/env python3
"""Replay a RETAINED pf-report-sink/v1 into today's compact report JSON.

  pineforge-sink-to-json --in-fd N --stdout-json PATH --out - --max-bytes B
      [--run-id ID]
  pineforge-sink-to-json --in PATH --stdout-json-fd M --out - --max-bytes B
  python3 -I -B -S docker/pineforge_sink_to_json.py <the same arguments>

Run AFTER the child and finalizer succeeded. N names a read-only regular
file, NOT the finalizer's live pipe. The wrapper must keep that retained
inode immutable for the entire conversion. M likewise names a read-only
regular stdout.json capture. Paths are opened read-only; inherited fds are
duplicated, never closed or repositioned in the caller. There is no implicit
stdin, scratch-arena access, spool, temporary, second sink copy or network.
Only fd 1 (JSON) and fd 2 (structured diagnostics) are written.

Pass 1 exhausts the SAME strict SinkReader as the finalizer, including its
EOF probe, counts, SHA and additional E/T/O/K ordering checks. It folds only
fixed-size scalars and one header-bounded K row. No stdout byte precedes this
verification. Replays use pread on the pinned inode and SinkReader again;
interleaving is legal. Trades and equity stream in report order. Each output
time/series column gets its own replay: no column transpose is stored.
An emission preflight walks every lazy array into a discard writer before
the actual emission. Sink defects and deterministic conversion failures
therefore precede stdout. Every replay must verify the original receipt.
An E-row null equity/open_profit is refused with event=sink_refused,
exit_code=3, code=report_result_invalid and
args={field: equity_curve.<field>, reason: nonfinite}.

stdout.json is read at most 2,097,153 bytes (2 MiB plus one overflow probe),
then decoded with duplicate-key and non-JSON-number refusal. Unknown/invalid
keys, an over-cap capture, bad JSON, and contradictions are DROPPED, with one
compact stderr JSON line per drop. Garbage metadata is NOT a run failure:
verified sink + garbage summary exits 0, billing=normal. Missing presentation
is omitted; missing output labels are null, never fabricated identifiers.
Trade numbers/counts, rows, constants and receipt values never come from it.
Metrics' non-count statistics, diagnostics, bar counts, report_window and
provenance are schema-checked PRESENTATION under APP ruling B, not receipts.

RAM is O(MAX_LINE_BYTES + STDOUT_JSON_MAX_BYTES), independent of sink length,
with Python's bounded decoded-object/encoder overhead. The 64 KiB I/O buffer
is NOT a 64 KiB whole-process RSS promise. No list grows with sink row count.
Manifest/slot/constant metadata is bounded by the input/header caps.

Exit codes (compatible with the finalizer's categories):
  0  verified report delivered; metadata drops still mean normal charging
  2  usage / retained-file descriptor contract
  3  malformed sink or a report row/order the verified v1 cannot supply
  4  receipt failure, changed retained input, or replay receipt disagreement
  5  input/output I/O failure, including stdout EPIPE
  6  incomplete sink / missing or truncated trailer
  7  converter exception; engine_fault and billing=refunded ONLY after verify
  8  retained sink exceeds the positive --max-bytes bound

Failure status goes ONLY to stderr; no error document follows report bytes.
OUTPUT COMMIT CONTRACT: the consumer MUST discard stdout on ANY nonzero exit
and publish only after exit 0. A pipe cannot retract an accepted prefix after
EPIPE, a late I/O error or a converter crash. Preflight does not make a pipe
transactional. Zero stdout is guaranteed for verification failures before
emission, not for arbitrary failures after emission starts. The lane report
returns that unavoidable limitation to CORE-TOP explicitly.
"""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import fcntl
import io
import json
import math
import os
from pathlib import Path
import re
import stat
from typing import NamedTuple

if sys.flags.isolated:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from pf_sink import I53_MAX, SinkFormatError
from pineforge_sink_finalize import (
    CHUNK_BYTES, _Failure, _FinalizerReader, _check_equity, _descriptor, _line_bound, _reader_code,
    _write_all,
)


STDOUT_JSON_MAX_BYTES = 2 * 1024 * 1024
_MISSING = object()
_SHA = re.compile(r"[0-9a-f]{64}\Z")


def _encoded(value):
    # Same defaults/float spelling/key order as run_json._stream_json.
    return json.dumps(value, separators=(",", ":"), allow_nan=False).encode("ascii")


class _Diagnostics:
    def __init__(self):
        self.drops = 0

    def line(self, event, **fields):
        _write_all(2, _encoded({"tool": "pineforge-sink-to-json", "event": event, **fields}) + b"\n")

    def drop(self, path, reason):
        self.drops += 1
        self.line("metadata_drop", path=path, reason=reason)


def _stamp(fd):
    value = os.fstat(fd)
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)


def _open_input(path, inherited):
    fd = (os.dup(inherited) if inherited is not None else
          os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK))
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise _Failure(2, "input must be a retained regular file, not a live stream")
        if fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE != os.O_RDONLY:
            raise _Failure(2, "inherited input must be opened read-only")
    except BaseException:
        os.close(fd)
        raise
    return fd


class _PreadInput(io.RawIOBase):
    """One independent, bounded replay cursor; never owns the retained fd."""
    def __init__(self, fd):
        super().__init__()
        self.fd, self.offset = fd, 0
        self.line_bytes = 0

    def readable(self):
        return True

    def readinto(self, buffer):
        data = os.pread(self.fd, min(len(buffer), CHUNK_BYTES), self.offset)
        # Match the finalizer's transport bound before SinkReader can treat
        # an overlong prefix without LF as an incomplete final fragment.
        self.line_bytes = _line_bound(data, self.line_bytes)
        self.offset += len(data)
        buffer[:len(data)] = data
        return len(data)


class _TradeCounts:
    """Only trade-derived counts; non-count metrics remain presentation."""
    def __init__(self):
        self.n = self.wins = self.losses = self.even = 0
        self.streak_w = self.streak_l = self.max_w = self.max_l = 0

    def fold(self, pnl):
        self.n += 1
        if pnl > 0:
            self.wins += 1
            self.streak_w += 1
            self.streak_l = 0
            self.max_w = max(self.max_w, self.streak_w)
        elif pnl < 0:
            self.losses += 1
            self.streak_l += 1
            self.streak_w = 0
            self.max_l = max(self.max_l, self.streak_l)
        else:
            self.even += 1
            self.streak_w = self.streak_l = 0

    def document(self):
        return dict(num_trades=self.n, num_wins=self.wins, num_losses=self.losses,
                    num_even=self.even, max_consecutive_wins=self.max_w,
                    max_consecutive_losses=self.max_l)


class _Sink:
    def __init__(self, fd, max_bytes, run_id):
        self.fd, self.run_id = fd, run_id
        self.stamp = _stamp(fd)
        if self.stamp[2] > max_bytes:
            raise _Failure(8, "retained sink exceeds --max-bytes")
        self.header = self.counts = None
        self.sha256 = ""
        self.verified = False
        self.constants = ()
        self.groups = {key: _TradeCounts() for key in ("all", "longs", "shorts")}
        self.cum = self.peak = self.max_dd = self.best = self.worst = 0.0

    def _unchanged(self):
        if _stamp(self.fd) != self.stamp:
            raise _Failure(4, "retained sink changed during conversion")

    def records(self, tag=None):
        self._unchanged()
        try:
            stream = io.BufferedReader(_PreadInput(self.fd), buffer_size=CHUNK_BYTES)
            with _FinalizerReader(stream=stream) as reader:
                if self.run_id is not None and reader.header["run_id"] != self.run_id:
                    raise _Failure(3, "header run_id differs from --run-id")
                last_time = None
                next_trade, next_output, constants = 1, 0, 0
                for row in reader:
                    if row.tag == "E":
                        _check_equity(row)
                        timestamp = row.values[0]
                        if last_time is not None and timestamp < last_time:
                            raise _Failure(3, "equity times are out of order")
                        last_time = timestamp
                    elif row.tag == "T":
                        if row.values[0] != next_trade:
                            raise _Failure(3, "trade rows are out of report order")
                        next_trade += 1
                    elif row.tag == "O":
                        if row.values[0] != next_output:
                            raise _Failure(3, "output rows are out of order")
                        next_output += 1
                    elif row.tag == "K":
                        constants += 1
                        if constants > 1:
                            raise _Failure(3, "constants record is repeated")
                    reader.line = b""
                    if tag is None or row.tag == tag:
                        yield row
                    del row
                if not reader.complete:
                    raise _Failure(4, "strict reader did not verify completion")
                if "O" in reader.counts and constants != 1:
                    raise _Failure(3, "output sink must supply exactly one K row")
                self._unchanged()
                if self.header is None:
                    self.header, self.counts, self.sha256 = reader.header, reader.counts, reader.sha256
                elif (reader.header != self.header or reader.counts != self.counts
                      or reader.sha256 != self.sha256):
                    raise _Failure(4, "replay differs from the verified sink receipt")
        except SinkFormatError as error:
            raise _Failure(_reader_code(error), str(error)) from error

    def verify(self):
        for row in self.records():
            if row.tag == "T":
                # Today's report rejects nullable T doubles; do not invent a
                # PnL/count classification for a null or unrepresentable sum.
                if any(value is None for value in row.values[4:12]):
                    raise _Failure(3, "sink cannot supply a finite canonical trade row")
                pnl = row.values[7]
                group = self.groups["all"]
                self.best = max(self.best, pnl) if group.n else pnl
                self.worst = min(self.worst, pnl) if group.n else pnl
                self.groups["longs" if row.values[1] == "long" else "shorts"].fold(pnl)
                group.fold(pnl)
                # Same sequential double folds and signed-zero tie rules as
                # run_json._sink_summary and native report net_profit.
                self.cum += pnl
                self.peak = max(self.peak, self.cum)
                self.max_dd = min(self.max_dd, self.cum - self.peak)
                if not all(math.isfinite(v) for v in (self.cum, self.max_dd)):
                    raise _Failure(3, "sink cannot supply a finite canonical summary")
            elif row.tag == "K":
                self.constants = row.values
        self.verified = True

    def summary(self):
        group = self.groups["all"]
        return dict(total_trades=self.counts["T"], wins=group.wins, losses=group.losses,
                    win_rate_pct=round(group.wins / group.n * 100.0 if group.n else 0.0, 4),
                    net_pnl=self.cum, avg_trade=self.cum / group.n if group.n else 0.0,
                    best_trade=self.best, worst_trade=self.worst, max_drawdown=self.max_dd)

    def slots(self):
        return next((len(entry["fields"]) - 3 for entry in self.header["records"]
                     if entry["type"] == "O"), 0)


# Presentation schema. Object keys are closed at EVERY level. Maps below
# have domain-defined data keys (input titles, symbol/timeframe identities),
# each with the indicated value schema; none is an arbitrary JSON escape.
class _Array(NamedTuple):
    item: object


class _Map(NamedTuple):
    item: object


class _Nullable(NamedTuple):
    item: object


def _text(value):
    return type(value) is str and not any(0xD800 <= ord(c) <= 0xDFFF for c in value)


def _integer(value):
    return type(value) is int and abs(value) <= I53_MAX


def _count(value):
    return _integer(value) and value >= 0


def _number(value):
    return type(value) in (int, float) and (abs(value) <= I53_MAX if type(value) is int
                                          else math.isfinite(value))


def _scalar(value):
    return value is None or type(value) is bool or _text(value) or _number(value)


def _boolean(value):
    return type(value) is bool


def _null(value):
    return value is None


def _sha(value):
    return _text(value) and _SHA.fullmatch(value) is not None


_N = _Nullable(_number)
_I = _Nullable(_integer)
_S = _Nullable(_text)
_RESOLUTION = {"status": _text, "reason": _text, "raw_default": _S,
               "distinct_native_inputs": _count}
_STRATEGY = {key: _Nullable(kind) for key, kind in (
    ("initial_capital", _number), ("process_orders_on_close", _boolean),
    ("default_qty_type", _text), ("default_qty_value", _number), ("pyramiding", _integer),
    ("commission_type", _text), ("commission_value", _number), ("slippage", _integer),
    ("calc_on_order_fills", _boolean), ("close_entries_rule", _text))}
_FEED_RECORD = {"bars": _count, "source_values_sha256": _sha, "first_ts": _integer,
                "last_ts": _integer, "format": _text, "schema_version": _integer,
                "symbol": _text, "tf": _text, "payload_sha256": _sha}
_WINDOW = {
    "wire_version": _count, "policy": _text, "applied": _boolean,
    "window": {"start_ms": _integer, "end_ms": _integer, "first_data_ms": _I,
               "last_data_ms": _I, "coverage": _text},
    "feed": {"requested_start_ms": _integer, "first_data_ms": _I, "last_data_ms": _I,
             "supplied_first_data_ms": _I, "supplied_last_data_ms": _I,
             "input_tf_seconds": _count, "script_tf": _text,
             "source_bytes_sha256": _sha, "source_values_sha256": _sha,
             "evaluated_source_values_sha256": _sha},
    "calendar": {"timezone": _text, "session": _text},
    "preroll": {**{key: _count for key in (
        "requested_script_bars", "available_script_bars", "used_script_bars",
        "trimmed_script_bars", "trimmed_input_bars")},
        "trim_start_ms": _I, "first_bar_ms": _I, "last_bar_ms": _I, "shortfall": _boolean},
    "lookback": {"declared_required_chart_bars": _Nullable(_count), "status": _text,
                 "has_unbounded_state": _Nullable(_boolean)},
    "counts": {key: _count for key in (
        "supplied_input_bars", "supplied_script_bars", "fed_input_bars", "fed_script_bars",
        "preroll_input_bars", "window_input_bars", "window_script_bars", "equity_points",
        "anchor_points")},
    "metering": {"billable_input_bars": _count, "preroll_billable_input_bars": _count,
                 "basis": _text},
    "indices": {"fed_script_index_of_window_first": _Nullable(_count),
                "supplied_script_index_of_window_first": _Nullable(_count)},
}
_RUNTIME = {
    **{key: _text for key in ("input_tf", "script_tf", "magnifier_dist", "chart_tz")},
    **{key: _count for key in ("input_tf_seconds", "script_tf_seconds", "script_tf_ratio",
                              "magnifier_samples")},
    **{key: _boolean for key in ("needs_aggregation", "bar_magnifier",
                                "magnifier_volume_weighted", "outputs")},
    "trade_start_ms": _I, "syminfo": {"qty_step": _number, "mincontract": _number},
    "symbol_feeds": {"canonicalization": _text, "symbols": _Map({
        "facts": {**{key: _text for key in ("canonical", "type", "timezone", "session", "currency")},
                  "mintick": _number}, "feeds": _Map(_FEED_RECORD)})},
    "report_window": _WINDOW,
}
_PROVENANCE = {
    "engine": {"version_string": _text, "major": _Nullable(_count),
               "minor": _Nullable(_count), "patch": _Nullable(_count), "commit_sha": _text},
    "feed": {"canonicalization": _text, "source_values_sha256": _sha},
    "codegen": {"version": _text, "generated_cpp_sha256": _Nullable(_sha),
                "transpiled_from_pine": _boolean},
    "strategy": _STRATEGY, "strategy_resolution": {key: _RESOLUTION for key in _STRATEGY},
    "inputs": _Map({"type": _text, "default": _scalar, "value": _scalar,
                    "resolution": _RESOLUTION}),
    "applied": {"inputs": _Map(_scalar), "overrides": _Map(_scalar)},
    "runtime": _RUNTIME, "schema_version": _count,
}
_TRADE_METRIC_KEYS = (
    "num_trades", "num_wins", "num_losses", "num_even", "percent_profitable",
    "net_profit", "net_profit_pct", "gross_profit", "gross_profit_pct", "gross_loss",
    "gross_loss_pct", "profit_factor", "avg_trade", "avg_trade_pct", "avg_win",
    "avg_win_pct", "avg_loss", "avg_loss_pct", "ratio_avg_win_avg_loss", "largest_win",
    "largest_win_pct", "largest_loss", "largest_loss_pct", "commission_paid", "expectancy",
    "max_consecutive_wins", "max_consecutive_losses", "avg_bars_in_trade",
    "avg_bars_in_wins", "avg_bars_in_losses",
)
_METRIC_COUNTS = frozenset(("num_trades", "num_wins", "num_losses", "num_even",
                            "max_consecutive_wins", "max_consecutive_losses"))
_EQUITY_METRICS = {key: _N for key in (
    "max_equity_drawdown", "max_equity_drawdown_pct", "max_equity_runup",
    "max_equity_runup_pct", "buy_hold_return", "buy_hold_return_pct", "sharpe_tv",
    "sortino_tv", "sharpe_bar", "sortino_bar", "cagr", "calmar", "recovery_factor",
    "time_in_market_pct", "open_pl")}
_REFERENCE = {"slot": _count, "constant": _count, "default": _scalar}
_MANIFEST_OUTPUT = {
    **{key: _text for key in ("id", "kind", "title", "events", "mark_rule", "freq",
                              "style", "text", "location", "size", "display", "format")},
    **{key: _integer for key in ("index", "line", "offset", "linewidth", "precision")},
    "series": _Array(_count), "price": _REFERENCE,
    "colors": {key: _REFERENCE for key in ("color", "textcolor", "wickcolor", "bordercolor")},
    "editable": _boolean, "show_last": _count, "trackprice": _boolean,
    "histbase": _scalar, "fillgaps": _boolean,
}
_MANIFEST = {
    "schema_version": _text, "message_format": _text, "source_sha256": _sha,
    "declaration": {"kind": _text, "overlay": _boolean, "title": _text,
                    "shorttitle": _text, "format": _text, "precision": _count},
    "outputs": _Array(_MANIFEST_OUTPUT),
    "series": _Array({"slot": _count, "output": _text, "role": _text, "encoding": _text}),
    "constants": _Array({"index": _count, "output": _text, "param": _text, "encoding": _text}),
    "changes_trading": _Array(_text), "not_outputs": _Array(_text),
}
_TRADE = {key: kind for key, kind in (
    ("n", _count), ("side", _text), ("entry_time", _integer), ("exit_time", _integer),
    ("entry_price", _number), ("exit_price", _number), ("qty", _number), ("pnl", _number),
    ("pnl_pct", _number), ("max_runup", _number), ("max_drawdown", _number),
    ("commission", _number), ("entry_bar_index", _integer), ("exit_bar_index", _integer),
    ("open_at_end", _boolean), ("entry_incarnation", _count))}
_EQUITY = {"time_ms": _integer, "equity": _N, "open_profit": _N}
_EVENT = {"sequence": _count, "output": _S, "bar_index": _integer,
          "bar_open_ms": _I, "bar_close_ms": _I, "ordinal_in_bar": _integer,
          "phase": _text, "value": _N, "message": _S, "freq": _S}
_SHAPE = {"version": _count, "name": _text, "curve_point_budget": _null,
          **{key: _count for key in ("canonical_equity_points", "emitted_equity_points",
                                    "canonical_trades", "emitted_trades")},
          "retained_point_indices": _null, "curve_decimated": _boolean}
_DIAGNOSTICS = {
    **{key: _count for key in ("input_bars_processed", "script_bars_processed",
        "magnifier_sub_bars_total", "magnifier_sample_ticks_total",
        "scratch_arena_high_water_bytes", "scratch_arena_bytes")},
    "bar_magnifier_enabled": _boolean,
    "annualization": {"cagr": _text, "calmar": _text},
    "phase_timing": {"version": _count, "execution_ms": _number,
                     "result_assembly_ms": _number, "results_digest_ms": _N},
    "timing": {"mode": _text, "warmup": _count, "repeats": _count,
               "samples_ns": _Array(_count), "magnifier": {"enabled": _boolean,
               "samples": _count, "dist": _text, "volume_weighted": _boolean}},
    "throughput": {"items_processed": _count, "samples_ns": _Array(_count),
                   "magnifier_mode": _text},
}
_SCHEMA = {
    "engine": _text,
    "input": {"ohlcv": _text, "bars": _count, "first_ts": _I, "last_ts": _I,
              "first_time": _S, "last_time": _S},
    "applied_inputs": _Map(_text), "applied_overrides": _Map(_text),
    "applied_runtime": _RUNTIME, "elapsed_seconds": _number,
    "summary": {**{key: _count for key in ("total_trades", "wins", "losses", "bars_processed")},
                **{key: _number for key in ("win_rate_pct", "net_pnl", "avg_trade",
                                           "best_trade", "worst_trade", "max_drawdown")}},
    "diagnostics": _DIAGNOSTICS, "trades": _Array(_TRADE),
    "metrics": {**{key: {name: _count if name in _METRIC_COUNTS else _N
                         for name in _TRADE_METRIC_KEYS} for key in ("all", "longs", "shorts")},
                "equity": _EQUITY_METRICS},
    "equity_curve": _Array(_EQUITY), "report_window": _WINDOW, "report_shape": _SHAPE,
    "outputs": {"schema_version": _text, "message_format": _text, "manifest_sha256": _sha,
                "manifest": _MANIFEST, "constants": _Array(_N),
                "hlines": _Array({"output": _S, "price": _N}),
                "bars": {"open_ms": _Array(_I), "close_ms": _Array(_I)},
                "series": _Array({"slot": _count, "output": _S, "values": _Array(_N)}),
                "events": _Array(_EVENT)},
    "fingerprint": _Nullable({"token": _text, "digest": _text, "provenance": _PROVENANCE,
                              "version": _count}),
    "report_sink": {"path": _text, "schema": _text, "version": _count, "run_id": _text,
                    "counts": {key: _count for key in "ETOVKXH"}, "sha256": _sha},
}


def _check(value, schema, path, diagnostics):
    if isinstance(schema, _Nullable):
        return None if value is None else _check(value, schema.item, path, diagnostics)
    if isinstance(schema, (_Array, _Map)) or type(schema) is dict:
        expected_type = list if isinstance(schema, _Array) else dict
        if type(value) is not expected_type:
            diagnostics.drop(path, "invalid_type")
            return _MISSING
        if isinstance(schema, _Array):
            result = []
            for index, item in enumerate(value):
                checked = _check(item, schema.item, f"{path}[{index}]", diagnostics)
                if checked is not _MISSING:
                    result.append(checked)
            return result
        result = {}
        for key, item in value.items():
            child = path + "." + key
            if not _text(key):
                diagnostics.drop(child, "invalid_key")
            elif type(schema) is dict and key not in schema:
                diagnostics.drop(child, "unknown_key")
            else:
                checked = _check(item, schema.item if isinstance(schema, _Map) else schema[key],
                                 child, diagnostics)
                if checked is not _MISSING:
                    result[key] = checked
        return result
    if not schema(value):
        diagnostics.drop(path, "invalid_value")
        return _MISSING
    return value


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _constant(text):
    raise ValueError("non-JSON numeric constant")


def _load_stdout(path, inherited, diagnostics):
    fd = None
    try:
        fd = _open_input(path, inherited)
        before = _stamp(fd)
        data = bytearray()
        # Stop at cap+1 even if a malicious capture grows during the read.
        while len(data) <= STDOUT_JSON_MAX_BYTES:
            chunk = os.pread(fd, min(CHUNK_BYTES, STDOUT_JSON_MAX_BYTES + 1 - len(data)), len(data))
            if not chunk:
                break
            data.extend(chunk)
        if len(data) > STDOUT_JSON_MAX_BYTES:
            diagnostics.drop("$", "stdout_json_over_cap")
            return {}
        if before != _stamp(fd):
            diagnostics.drop("$", "stdout_json_changed")
            return {}
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique, parse_constant=_constant)
    except (OSError, _Failure):
        diagnostics.drop("$", "stdout_json_unreadable")
        return {}
    except (ValueError, UnicodeError, RecursionError):
        diagnostics.drop("$", "stdout_json_invalid_json")
        return {}
    finally:
        if fd is not None:
            os.close(fd)
    result = _check(value, _SCHEMA, "$", diagnostics)
    return {} if result is _MISSING else result


class _Rows:
    def __init__(self, factory):
        self.factory = factory


class _Writer:
    def __init__(self, fd=None):
        self.fd = fd
        self.buffer = bytearray()

    def write(self, data):
        if self.fd is None:
            return  # preflight still serializes every value, retaining none
        if len(data) >= CHUNK_BYTES:
            self.flush()
            _write_all(self.fd, data)
        else:
            self.buffer.extend(data)
            if len(self.buffer) >= CHUNK_BYTES:
                self.flush()

    def flush(self):
        if self.buffer:
            _write_all(self.fd, self.buffer)
            self.buffer.clear()


def _emit(value, writer):
    if isinstance(value, _Rows):
        writer.write(b"[")
        first = True
        for row in value.factory():
            if not first:
                writer.write(b",")
            writer.write(_encoded(row))
            first = False
        writer.write(b"]")
    elif type(value) is dict:
        writer.write(b"{")
        for index, (key, item) in enumerate(value.items()):
            if index:
                writer.write(b",")
            writer.write(_encoded(key) + b":")
            _emit(item, writer)
        writer.write(b"}")
    elif type(value) in (list, tuple):
        writer.write(b"[")
        for index, item in enumerate(value):
            if index:
                writer.write(b",")
            _emit(item, writer)
        writer.write(b"]")
    else:
        writer.write(_encoded(value))


def _same(left, right):
    # Types and signed zero matter to report-byte parity (bool is not int).
    if isinstance(left, _Rows):
        if type(right) is not list:
            return False
        count = 0
        equal = True
        for count, item in enumerate(left.factory(), 1):
            if count > len(right) or not _same(item, right[count - 1]):
                equal = False
        return equal and count == len(right)
    if type(left) is dict:
        return (type(right) is dict and left.keys() == right.keys()
                and all(_same(item, right[key]) for key, item in left.items()))
    if type(left) in (list, tuple):
        return (type(right) is list and len(left) == len(right)
                and all(_same(a, b) for a, b in zip(left, right)))
    return type(left) is type(right) and _encoded(left) == _encoded(right)


def _provenance_equal(left, right):
    # JCS normalizes 1.0 to 1 and -0.0 to 0. Compare its JSON number model,
    # while keeping booleans distinct and refusing unknown token-side keys.
    if _number(left) and _number(right):
        return left == right
    if type(left) is dict:
        return (type(right) is dict and left.keys() == right.keys()
                and all(_provenance_equal(value, right[key]) for key, value in left.items()))
    if type(left) is list:
        return (type(right) is list and len(left) == len(right)
                and all(_provenance_equal(a, b) for a, b in zip(left, right)))
    return type(left) is type(right) and left == right


def _authoritative(source, key, value, path, diagnostics):
    if key in source and not _same(value, source[key]):
        diagnostics.drop(path + "." + key, "contradicts_verified_sink")
    return value


def _reconcile_window(window, sink, path, diagnostics):
    # Bar counts/planner geometry remain presentation. equity_points is a
    # record count and is always taken from the verified E population.
    counts = window.get("counts")
    if counts is not None and "equity_points" in counts:
        counts["equity_points"] = _authoritative(
            counts, "equity_points", sink.counts["E"], path + ".counts", diagnostics)


def _value(value, encoding):
    return int(value) if value is not None and encoding == "rgba-u32" else value


def _outputs(source, sink, diagnostics):
    manifest = source.get("manifest", {})
    # These are presentation identities/mappings, NEVER a source of row count.
    outputs = {}
    valid_outputs = []
    for index, entry in enumerate(manifest.get("outputs", [])):
        key = entry.get("index")
        if key is None or key < 0 or "id" not in entry or not entry["id"] or key in outputs:
            diagnostics.drop(f"$.outputs.manifest.outputs[{index}]", "invalid_output_identity")
        else:
            outputs[key] = entry
            valid_outputs.append(entry)
    if "outputs" in manifest:
        manifest["outputs"] = valid_outputs
    entries = {}
    valid_series = []
    for index, entry in enumerate(manifest.get("series", [])):
        slot = entry.get("slot")
        if slot is None or slot >= sink.slots() or slot in entries or "output" not in entry:
            diagnostics.drop(f"$.outputs.manifest.series[{index}]", "invalid_series_slot")
        else:
            entries[slot] = entry
            valid_series.append(entry)
    if "series" in manifest:
        manifest["series"] = valid_series
    encodings = {}
    valid_constants = []
    for index, entry in enumerate(manifest.get("constants", [])):
        key = entry.get("index")
        if key is None or key >= len(sink.constants) or key in encodings:
            diagnostics.drop(f"$.outputs.manifest.constants[{index}]", "invalid_constant_index")
        else:
            encodings[key] = entry.get("encoding")
            valid_constants.append(entry)
    if "constants" in manifest:
        manifest["constants"] = valid_constants
    constants = [_value(value, encodings.get(index)) for index, value in enumerate(sink.constants)]
    series = []
    # Slot count is from the strict header. Absent/invalid metadata cannot
    # remove a numeric column or supply a substitute value.
    for slot in range(sink.slots()):
        entry = entries.get(slot, {})
        if not entry:
            diagnostics.drop(f"$.outputs.series[{slot}].output", "presentation_unavailable")
        series.append({"slot": slot, "output": entry.get("output"), "values": _Rows(
            lambda slot=slot, encoding=entry.get("encoding"): (
                _value(row.values[3 + slot], encoding) for row in sink.records("O")))})
    hlines = []
    for entry in outputs.values():
        if entry.get("kind") == "hline":
            index = entry.get("price", {}).get("constant")
            hlines.append({"output": entry["id"], "price": (
                constants[index] if index is not None and index < len(constants) else None)})

    def events():
        for record in sink.records("V"):
            row = record.as_dict()
            output = outputs.get(row["output_index"], {})
            event = {
                "sequence": row["sequence"], "output": output.get("id"),
                "bar_index": row["bar_index"], "bar_open_ms": row["bar_open_ms"],
                "bar_close_ms": row["bar_close_ms"], "ordinal_in_bar": row["ordinal_in_bar"],
                "phase": {0: "batch", 1: "warmup", 2: "realtime"}.get(row["phase"], str(row["phase"])),
                "value": row["value"], "message": row["message"],
            }
            if output.get("kind") == "alert":
                event["freq"] = output.get("freq")
            yield event

    block = {"schema_version": "pineforge-outputs/v1",
             "message_format": manifest.get("message_format", "pineforge/v1")}
    if "manifest_sha256" in source:
        block["manifest_sha256"] = source["manifest_sha256"]
    if "manifest" in source:
        block["manifest"] = manifest
    block.update(bars={key: _Rows(lambda index=index: (
        row.values[index] for row in sink.records("O")))
        for key, index in (("open_ms", 1), ("close_ms", 2))},
        series=series, constants=constants, hlines=hlines, events=_Rows(events))
    for key, value in block.items():
        if key not in ("manifest", "manifest_sha256"):
            _authoritative(source, key, value, "$.outputs", diagnostics)
    return block


def _document(source, sink, diagnostics):
    provenance_engine = (source.get("fingerprint") or {}).get("provenance", {}).get("engine")
    if provenance_engine is not None and "version_string" in provenance_engine:
        provenance_engine["version_string"] = _authoritative(provenance_engine, "version_string",
            sink.header["engine"], "$.fingerprint.provenance.engine", diagnostics)
    for path, runtime in (("$.applied_runtime", source.get("applied_runtime", {})),
                          ("$.fingerprint.provenance.runtime",
                           (source.get("fingerprint") or {}).get("provenance", {}).get("runtime", {}))):
        if "outputs" in runtime:
            runtime["outputs"] = _authoritative(runtime, "outputs", "O" in sink.counts,
                                                 path, diagnostics)
        if "report_window" in runtime:
            _reconcile_window(runtime["report_window"], sink, path + ".report_window", diagnostics)
    result = {"engine": _authoritative(source, "engine", "pineforge", "$", diagnostics)}
    for key in ("input", "applied_inputs", "applied_overrides", "applied_runtime", "elapsed_seconds"):
        if key in source:
            result[key] = source[key]
    small_summary = source.get("summary", {})
    result["summary"] = {key: _authoritative(small_summary, key, value, "$.summary", diagnostics)
                         for key, value in sink.summary().items()}
    if "bars_processed" in small_summary:
        result["summary"]["bars_processed"] = small_summary["bars_processed"]
    if "diagnostics" in source:
        result["diagnostics"] = source["diagnostics"]
    result["trades"] = _authoritative(source, "trades", _Rows(lambda: (
        dict(zip(row.fields[:16], row.values[:16])) for row in sink.records("T"))), "$", diagnostics)
    metrics = source.get("metrics", {})
    result["metrics"] = {}
    for name, group in sink.groups.items():
        presented, counts = metrics.get(name, {}), group.document()
        result["metrics"][name] = {key: (
            _authoritative(presented, key, counts[key], "$.metrics." + name, diagnostics)
            if key in counts else presented[key]) for key in _TRADE_METRIC_KEYS
            if key in counts or key in presented}
    if "equity" in metrics:
        result["metrics"]["equity"] = metrics["equity"]
    result["equity_curve"] = _authoritative(source, "equity_curve", _Rows(lambda: (
        row.as_dict() for row in sink.records("E"))), "$", diagnostics)
    if "report_window" in source:
        _reconcile_window(source["report_window"], sink, "$.report_window", diagnostics)
        result["report_window"] = source["report_window"]
    if "report_shape" in source:
        shape = dict(version=1, name="full/v1", curve_point_budget=None,
                     canonical_equity_points=sink.counts["E"], emitted_equity_points=sink.counts["E"],
                     canonical_trades=sink.counts["T"], emitted_trades=sink.counts["T"],
                     retained_point_indices=None, curve_decimated=False)
        result["report_shape"] = {key: _authoritative(source["report_shape"], key, value,
            "$.report_shape", diagnostics) for key, value in shape.items()}
    if "O" in sink.counts:
        result["outputs"] = _outputs(source.get("outputs", {}), sink, diagnostics)
    elif "outputs" in source:
        diagnostics.drop("$.outputs", "contradicts_verified_sink")
    if "fingerprint" in source:
        fingerprint = source["fingerprint"]
        # A token/digest cannot continue to attest bytes whose invalid fields
        # were dropped or whose record counts were reconciled. Keep checked
        # provenance as presentation; drop inconsistent identity strings.
        if fingerprint is not None:
            provenance = fingerprint.get("provenance")
            token = fingerprint.get("token")
            if token is not None:
                import base64
                import hashlib
                try:
                    raw = base64.b64decode(token, validate=True)
                    decoded = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique,
                                         parse_int=float, parse_constant=_constant)
                    valid = (provenance is not None and _provenance_equal(provenance, decoded)
                             and fingerprint.get("digest") == "sha256:" + hashlib.sha256(raw).hexdigest())
                except (ValueError, UnicodeError, RecursionError):
                    valid = False
                if not valid:
                    for key in ("token", "digest"):
                        if key in fingerprint:
                            diagnostics.drop("$.fingerprint." + key, "inconsistent_provenance")
                            del fingerprint[key]
            elif "digest" in fingerprint:
                diagnostics.drop("$.fingerprint.digest", "token_unavailable")
                del fingerprint["digest"]
        result["fingerprint"] = fingerprint
    if "report_sink" in source:
        receipt = source["report_sink"]
        for key, value in (("schema", "pf-report-sink"), ("version", 1),
                           ("run_id", sink.header["run_id"]), ("sha256", sink.sha256),
                           ("counts", sink.counts)):
            _authoritative(receipt, key, value, "$.report_sink", diagnostics)
        diagnostics.drop("$.report_sink", "replaced_by_full_report")
    return result


def _positive(text):
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a positive integer") from error
    if not 0 < value <= I53_MAX:
        raise argparse.ArgumentTypeError("expected a positive exact JSON integer")
    return value


def _parser():
    parser = argparse.ArgumentParser(prog="pineforge-sink-to-json", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--in", dest="input_path", type=Path, metavar="PATH")
    source.add_argument("--in-fd", type=_descriptor, metavar="N")
    metadata = parser.add_mutually_exclusive_group(required=True)
    metadata.add_argument("--stdout-json", type=Path, metavar="PATH")
    metadata.add_argument("--stdout-json-fd", type=_descriptor, metavar="M")
    parser.add_argument("--out", required=True, choices=("-",))
    parser.add_argument("--max-bytes", required=True, type=_positive, metavar="B")
    parser.add_argument("--run-id", metavar="ID")
    return parser


def main(argv=None):
    parser = _parser()
    args = parser.parse_args(argv)
    if args.in_fd is not None and args.in_fd == args.stdout_json_fd:
        parser.error("sink and stdout.json must have different descriptors")
    if args.in_fd in (1, 2) or args.stdout_json_fd in (1, 2):
        parser.error("input descriptors must differ from stdout and stderr")
    diagnostics = _Diagnostics()
    fd, sink = None, None
    code, event, message = 0, "converter_complete", ""
    failure_fields = {}
    try:
        fd = _open_input(args.input_path, args.in_fd)
        sink = _Sink(fd, args.max_bytes, args.run_id)
        sink.verify()  # FIRST: metadata is not even read before sink acceptance.
        source = _load_stdout(args.stdout_json, args.stdout_json_fd, diagnostics)
        document = _document(source, sink, diagnostics)
        _emit(document, _Writer())  # deterministic conversion failures before stdout
        writer = _Writer(1)
        _emit(document, writer)
        writer.write(b"\n")
        writer.flush()  # direct write(2): no Python stdout buffer or exit-time EPIPE
    except _Failure as error:
        code, event, message = error.code, "sink_refused", str(error)
        failure_fields = error.diagnostic_fields()
    except OSError as error:
        code, event, message = 5, "converter_io", str(error)
    except (Exception, KeyboardInterrupt) as error:
        code, message = 7, type(error).__name__ + ": " + str(error)
        event = "engine_fault" if sink is not None and sink.verified else "converter_fault"
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError as error:
                code, event, message = 5, "converter_io", str(error)
                failure_fields = {}
    verified = sink is not None and sink.verified
    try:
        diagnostics.line(event, exit_code=code, sink_verified=verified,
                         billing="refunded" if event == "engine_fault" else "normal",
                         metadata_drops=diagnostics.drops, detail=message, **failure_fields)
    except OSError:
        return 5
    return code


if __name__ == "__main__":
    raise SystemExit(main())
