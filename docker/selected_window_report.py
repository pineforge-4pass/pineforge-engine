"""Selected-window/v1 helpers of docker/run_json.py (single run, full/v1 report, no digest).

run_json owns the orchestration (what happens in which order); this module holds the pieces
that are pure data or a thin native wrapper, so each can be controlled on its own:

  * request validation (the documented flags, explicit timeframes, the legacy-flag conflict) and the
    typed refusal of the options this build does not offer (compact shape, curve budget, digest);
  * the candidate support constants handed to request_feed_inventory.admit_selected_request;
  * the mapping of the native primary planner's refusal statuses to the window catalog codes;
  * the in-place trim view and the hash of the retained rows in the existing primary hash domain;
  * the exact report_window record R (its lookback member is the one triple of a build with no
    lookback producer), the full/v1 report_shape and the capability record (its
    contract revision is this prerelease build's parity declaration, CONTRACT_REVISIONS);
  * the frozen selected_window ABI as a ctypes wrapper, and
    the exact comparison of the engine's native counts with the planned counts.

It never imports run_json (a script run as __main__ would be loaded twice), never loads a strategy
library and carries no calendar, count or hash-domain logic of its own: calendar and count planning
belong to the native planner, the hash domain to run_json's frozen helpers (passed in).

Errors are SelectedWindowError: `code` is a catalog code, `code_args` its typed arguments and
`message` the documented fixed English text or an invariant description. The
caller (run_json) maps them to RunFailure.
"""
from __future__ import annotations

import base64
import binascii
import ctypes
import hashlib
import json
import math
import re
import sys
from typing import Any, Dict, List, Mapping, NamedTuple, Optional, Sequence, Tuple

import request_feed_inventory as _inventory

__all__ = [
    "CAPABILITY", "CONTRACT_REVISIONS", "FINGERPRINT_VERSION", "I53_MAX", "MAX_PREROLL_BARS",
    "METERING_BASIS", "POLICY", "REPORT_SHAPE", "SelectedWindowAbi", "SelectedWindowConfigC",
    "SelectedWindowCountsC", "SelectedWindowError", "WIRE_VERSION", "WindowRequest",
    "build_report_window", "calendar_record", "calendar_session", "calendar_timezone",
    "canonical_timeframe", "capabilities_record", "check_native_counts",
    "check_report_placements", "check_selected_curve", "check_unsupported_options",
    "refusal_for_plan", "report_shape_full",
    "retained_rows", "support_policy", "timeframe_seconds", "update_rows_hash",
    "validate_request",
]

POLICY = "selected-window/v1"
WIRE_VERSION = 1
FINGERPRINT_VERSION = 2
# The contract revision this build declares in its capability record (the member
# selected_window_contract_revisions of --capabilities-json). This is the ONE place it is
# written; capabilities_record is the code that reads it. It is a PRERELEASE PARITY capability:
# the selected window without the value-ownership producers of the full contract, and NOT a
# claim of full 1.3 coverage. A parity build lists ("1.3-parity",) and a full build lists
# ("1.3",), never both: a future full build changes this one tuple and nothing else (no
# feature switch, environment knob or second constant exists). It is a declaration only: no
# request, report_window, phase record or fingerprint member carries it, and the wire version
# (1), the phase version (1) and the fingerprint/schema version (2) are unchanged.
CONTRACT_REVISIONS = ("1.3-parity",)
REPORT_SHAPE = "full/v1"
METERING_BASIS = "window-primary-input-bars/v1"
MAX_PREROLL_BARS = 5000
I53_MAX = (1 << 53) - 1
# A request number is at most this many decimal digits (I53_MAX has 16). Every int() of request
# text is preceded by a length test against it, so a number of thousands of digits is a typed
# request error and never Python's integer digit-limit ValueError.
_MAX_INTEGER_DIGITS = len(str(I53_MAX))
CAPABILITY = "selected_window_v1"  # the one argument of window_mode_unsupported for the native ABI

# The candidate support constants this build hands to admit_selected_request: the eight
# intraday primary tokens, pre-roll supported, request feeds 1 and D. They are the internal
# implementation target, NOT a release declaration: the release policy comes from the release's
# acceptance alone, and run_json never reads or writes one.
_PRIMARY_TIMEFRAMES = ("1", "3", "5", "15", "30", "60", "120", "240")
_REQUEST_FEED_TIMEFRAMES = ("1", "D")


def support_policy() -> Dict[str, Any]:
    """A fresh copy of the candidate support constants."""
    return {"primary_chart_timeframes": list(_PRIMARY_TIMEFRAMES),
            "preroll": "supported",
            "request_feed_timeframes": list(_REQUEST_FEED_TIMEFRAMES)}


# --- typed errors -----------------------------------------------------------

_FIXED_TEXT = {
    "window_mode_unsupported": "This runner does not support the requested window mode.",
    "window_request_invalid": "The selected-window request is invalid.",
    "window_range_invalid": "The selected-window range is invalid.",
    "window_preroll_out_of_range": "Pre-roll must be between zero and 5000 script bars.",
    "window_legacy_flag_conflict":
        "The legacy trade-start option cannot be combined with selected-window mode.",
    "window_calendar_unsupported": "The requested window calendar is unsupported.",
    "window_boundary_unaligned": "A window boundary is not aligned to the script calendar.",
    "window_feed_range_invalid": "The primary feed lies outside the frozen range.",
    "report_shape_unsupported": "This runner does not support the requested report shape.",
    "report_shape_request_invalid": "The report-shape request is invalid.",
    "results_digest_unsupported": "This runner does not support the requested results digest.",
}


class SelectedWindowError(Exception):
    """A typed refusal or fault of the selected-window path. `code_args` (not
    BaseException.args) holds the code's typed arguments."""

    def __init__(self, code: str, message: str, code_args: Optional[Mapping[str, Any]] = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.code_args = dict(code_args or {})


def _error(code: str, **code_args: Any) -> SelectedWindowError:
    return SelectedWindowError(code, _FIXED_TEXT[code], code_args)


def _invalid(option: str) -> SelectedWindowError:
    return _error("window_request_invalid", option=option)


def _internal(text: str) -> SelectedWindowError:
    return SelectedWindowError("harness_internal_error", text)


def _invariant(text: str) -> SelectedWindowError:
    return SelectedWindowError("engine_invariant", text)


# --- timeframes -------------------------------------------------------------

_TOKEN_UNITS = {"S": 1, "D": 86400, "W": 7 * 86400}
_TOKEN_RE = re.compile(r"([1-9][0-9]*)([DWMS]?)")


def canonical_timeframe(value: Any) -> Optional[str]:
    """The wire's canonical spelling of value, or None; the common canonicalizer of the
    inventory helper, so one spelling rule serves the policy, the inventory and this request."""
    return _inventory._canonical_timeframe(value)


def timeframe_seconds(token: Any) -> Optional[int]:
    """The nominal length in seconds of a CANONICAL token: n minutes, nS, nD or nW; None for
    months, for anything that is not a canonical token and for a length that is not a u53
    number of seconds. Only the report's input_tf_seconds unit conversion: no calendar.
    Recognising a token stays unbounded (canonical_timeframe, unchanged); only this numeric
    conversion is bounded, by the decimal length of the count before int() is called, so a token
    of thousands of digits gives None here and never Python's integer digit-limit error."""
    if not isinstance(token, str):
        return None
    if token in ("D", "W"):
        return _TOKEN_UNITS[token]
    match = _TOKEN_RE.fullmatch(token)
    if match is None or match.group(2) == "M" or len(match.group(1)) > _MAX_INTEGER_DIGITS:
        return None
    count, unit = int(match.group(1)), match.group(2)
    seconds = count * (60 if unit == "" else _TOKEN_UNITS[unit])
    return seconds if seconds <= I53_MAX else None


# --- the request ------------------------------------------------------------

class WindowRequest(NamedTuple):
    """A validated selected-window request: T, E, N, F and the two explicit timeframes."""
    start_ms: int
    end_ms: int
    preroll_bars: int
    fed_start_ms: int
    input_tf: str
    script_tf: str            # the canonical primary chart token
    input_tf_seconds: int


_INTEGER_RE = re.compile(r"-?(?:0|[1-9][0-9]*)")


def _integer(text: Any, option: str) -> int:
    """A decimal integer written as such: no sign other than '-', no leading zeros, no
    fraction or exponent, and within the JSON safe-integer range (i53). The text is bounded by
    its length (an optional '-' and at most 16 digits) BEFORE the pattern or int() sees it, so
    an oversized number is window_request_invalid{option} at once and Python's integer
    digit-limit ValueError is unreachable."""
    if (not isinstance(text, str) or len(text) > _MAX_INTEGER_DIGITS + 1
            or _INTEGER_RE.fullmatch(text) is None):
        raise _invalid(option)
    value = int(text)
    if abs(value) > I53_MAX:
        raise _invalid(option)
    return value


def validate_request(*, policy: Any, window_start_ms: Any, window_end_ms: Any,
                     preroll_bars: Any, fed_start_ms: Any, trade_start_ms: Any,
                     input_tf: str, script_tf: str) -> WindowRequest:
    """Validate the documented flags of a selected-window request, in this order: the policy, the
    four window arguments (present, written as integers, i53), the two explicit timeframes
    (present and timeframe tokens), the legacy trade-start conflict, the pre-roll range, the
    ranges T < E and F <= T. Raises SelectedWindowError with the window catalog code; the
    caller refuses before any feed, library or phase work."""
    if policy != POLICY:
        raise _invalid("report_policy")
    start = _integer(window_start_ms, "window_start_ms")
    end = _integer(window_end_ms, "window_end_ms")
    preroll = _integer(preroll_bars, "preroll_bars")
    fed_start = _integer(fed_start_ms, "fed_start_ms")
    if not input_tf:
        raise _invalid("input_tf")
    if not script_tf:
        raise _invalid("script_tf")
    primary = canonical_timeframe(script_tf)
    if primary is None:
        raise _invalid("script_tf")
    chart = canonical_timeframe(input_tf)
    seconds = timeframe_seconds(chart)
    if chart is None or seconds is None:
        raise _invalid("input_tf")
    if trade_start_ms is not None:
        raise _error("window_legacy_flag_conflict", option="trade_start_ms")
    if not 0 <= preroll <= MAX_PREROLL_BARS:
        raise _error("window_preroll_out_of_range", requested=preroll)
    if start >= end or fed_start > start:
        raise _error("window_range_invalid", start_ms=start, end_ms=end, fed_start_ms=fed_start)
    return WindowRequest(start, end, preroll, fed_start, input_tf, primary, seconds)


def check_unsupported_options(*, selected: bool, report_shape: Any, curve_point_budget: Any,
                              results_digest: Any) -> None:
    """Refuse, typed and before any work, what this build does not offer: the compact shape,
    an explicit curve point budget (only compact takes one) and any results digest. A selected
    request may name full/v1. A legacy request names no shape in this build: with
    legacy accounting every shape request is refused (not settled by the documented contract; the
    refusal keeps the ordinary report byte for byte)."""
    if report_shape is not None:
        if selected and report_shape == REPORT_SHAPE:
            pass
        elif selected and report_shape != "compact/v1":
            raise _error("report_shape_request_invalid", option="report_shape",
                         value=str(report_shape))
        else:
            raise _error("report_shape_unsupported", shape=str(report_shape))
    if curve_point_budget is not None:
        raise _error("report_shape_request_invalid", option="curve_point_budget",
                     value=str(curve_point_budget))
    if results_digest is not None:
        raise _error("results_digest_unsupported", requested=str(results_digest))


# --- calendar data ----------------------------------------------------------

def calendar_timezone(chart_tz: str) -> str:
    """R.calendar.timezone: the alignment timezone, --chart-tz, UTC when absent or empty."""
    return chart_tz if chart_tz else "UTC"


def calendar_session(session: str) -> Optional[str]:
    """R.calendar.session: the resolved session, null for 24x7 (absent, empty or 24x7)."""
    return None if session in ("", "24x7") else session


def calendar_record(chart_tz: str, session: str) -> Dict[str, Any]:
    return {"timezone": calendar_timezone(chart_tz), "session": calendar_session(session)}


# --- the native planner's refusals ------------------------------------------

_BOUND_NAMES = ("start", "end", "fed_start")


def refusal_for_plan(plan: Mapping[str, Any], request: WindowRequest, *, timezone: str,
                     session: Optional[str]) -> Optional[SelectedWindowError]:
    """None for planner status 0 (Ok), else the typed error for the planner's refusal:

      1 RequestInvalid        script_tf / input_tf  -> window_request_invalid{option}
                              start_ms / end_ms / fed_start_ms -> window_range_invalid
                              preroll_bars          -> window_preroll_out_of_range{requested}
                              any other option (rows, ...) -> harness_internal_error
      2 CalendarUnsupported   -> window_calendar_unsupported{timezone, session}
      3 BoundaryUnaligned     -> window_boundary_unaligned{bound, value_ms, previous, next}
      4 FeedRangeInvalid      -> window_feed_range_invalid{time_ms, fed_start_ms, end_ms}
      5 RowsUnordered         -> chart_bars_rejected{field: timestamp, reason: not_increasing}
      6 InternalError         -> engine_invariant
      anything else           -> harness_internal_error

    The option strings are the planner's own. The mapping is this module's reading of them:
    the documented contract names the codes, not the planner's options."""
    status = plan["status"]
    if status == 0:
        return None
    option = plan.get("option")
    if status == 1:
        if option in ("script_tf", "input_tf"):
            return _invalid(option)
        if option in ("start_ms", "end_ms", "fed_start_ms"):
            return _error("window_range_invalid", start_ms=request.start_ms,
                          end_ms=request.end_ms, fed_start_ms=request.fed_start_ms)
        if option == "preroll_bars":
            return _error("window_preroll_out_of_range", requested=request.preroll_bars)
        return _internal(f"the selected primary planner refused the call as invalid ({option!r})")
    if status == 2:
        return _error("window_calendar_unsupported", timezone=timezone, session=session)
    if status == 3:
        bound = plan.get("bound")
        if bound in (0, 1, 2):
            return _error("window_boundary_unaligned", bound=_BOUND_NAMES[bound],
                          value_ms=plan["value_ms"],
                          previous_boundary_ms=plan["previous_boundary_ms"],
                          next_boundary_ms=plan["next_boundary_ms"])
        return _internal(f"the selected primary planner reported an unaligned boundary with "
                         f"bound {bound!r}")
    if status == 4:
        return _error("window_feed_range_invalid", time_ms=plan["value_ms"],
                      fed_start_ms=request.fed_start_ms, end_ms=request.end_ms)
    if status == 5:
        return SelectedWindowError(
            "chart_bars_rejected",
            f"bar timestamps are not increasing (row at {plan.get('value_ms')} after "
            f"{plan.get('previous_boundary_ms')})",
            {"field": "timestamp", "reason": "not_increasing"})
    if status == 6:
        return _invariant(f"the selected primary planner failed on an internal error ({option!r})")
    return _internal(f"the selected primary planner returned status {status!r}")


# --- trim by pointer offset, hash of the retained rows ----------------------

_ROW_FIELDS = ("open", "high", "low", "close", "volume", "timestamp")
_ROW_OFFSETS = (0, 8, 16, 24, 32, 40)
_HASH_CHUNK_ROWS = 1 << 16


def retained_rows(bars: Any, row_count: int, trim_index: int, fed_count: int,
                  row_type: Any) -> Tuple[Any, int]:
    """(rows, count) of the rows[trim_index:] the engine is fed, WITHOUT copying the feed.

    trim_index 0 returns the original array; otherwise a view of the same allocation is
    returned (ctypes from_buffer at the byte offset), which holds a reference to the original,
    so the original stays alive as long as the view does. fed_count must be exactly
    row_count - trim_index (the planner's equality supplied = trimmed + fed); a mismatch is an
    engine invariant. With nothing to feed the original array is returned with count 0."""
    for name, value in (("row_count", row_count), ("trim_index", trim_index),
                        ("fed_count", fed_count)):
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise _internal(f"{name} must be a nonnegative int, got {value!r}")
    if trim_index > row_count or fed_count != row_count - trim_index:
        raise _invariant(f"the plan keeps {fed_count} rows after trimming {trim_index} of "
                         f"{row_count} supplied rows")
    if trim_index == 0:
        return bars, fed_count
    if fed_count == 0:
        return bars, 0
    view = (row_type * fed_count).from_buffer(bars, trim_index * ctypes.sizeof(row_type))
    return view, fed_count


def _row_layout_is_record(row_type: Any, record_size: int) -> bool:
    try:
        offsets = tuple(getattr(row_type, name).offset for name in _ROW_FIELDS)
    except AttributeError:
        return False
    return offsets == _ROW_OFFSETS and ctypes.sizeof(row_type) == record_size


def update_rows_hash(hasher: Any, bars: Any, start: int, count: int, row_type: Any,
                     record: Any) -> None:
    """Feed rows[start:start+count] into hasher in the existing primary hash domain (the
    caller's hasher already holds the domain prefix; `record` is the frozen "<5dq" Struct).
    On a little-endian host whose rows are exactly those 48 bytes the memory itself is the
    record sequence and is hashed in bounded chunks; otherwise each row is packed."""
    if count <= 0:
        return
    size = ctypes.sizeof(row_type)
    if sys.byteorder == "little" and _row_layout_is_record(row_type, record.size):
        base = ctypes.addressof(bars)
        done = 0
        while done < count:
            take = min(_HASH_CHUNK_ROWS, count - done)
            hasher.update(ctypes.string_at(base + (start + done) * size, take * size))
            done += take
        return
    for index in range(start, start + count):
        row = bars[index]
        hasher.update(record.pack(row.open, row.high, row.low, row.close, row.volume,
                                  row.timestamp))


# --- R = $.report_window ----------------------------------------------------

_COUNT_KEYS = ("supplied_input_bars", "supplied_script_bars", "available_script_bars",
               "used_script_bars", "trimmed_script_bars", "trim_index", "fed_input_bars",
               "fed_script_bars", "preroll_input_bars", "window_input_bars",
               "window_script_bars")
_SHA256_RE = re.compile(r"[0-9a-f]{64}")


def _u53(plan: Mapping[str, Any], key: str) -> int:
    value = plan[key]
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= I53_MAX:
        raise _invariant(f"the plan's {key} is {value!r}, not a u53")
    return value


def _optional_i53(plan: Mapping[str, Any], key: str) -> Optional[int]:
    value = plan[key]
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or abs(value) > I53_MAX:
        raise _invariant(f"the plan's {key} is {value!r}, not an i53 or null")
    return value


def _lookback_not_produced() -> Dict[str, Any]:
    """The lookback record of R in a build that has NO producer for the lookback analysis:
    the one triple this build emits, for a run and for validation-only output alike. status
    is "unknown", declared_required_chart_bars is null and has_unbounded_state is null, which
    means "not produced by this build": it is neither true nor false, and nothing is guessed
    or defaulted to either. This build never emits sufficient_declared, insufficient or a
    number here, and draws on no lookback analysis, timeframe conversion or input metadata.
    A build that has the producer emits a boolean.

    A fresh dict on every call, in the wire table's member order. build_report_window is the
    only caller, so this is the only place the triple is written."""
    return {
        "declared_required_chart_bars": None,
        "status": "unknown",
        "has_unbounded_state": None,
    }


def _lookback_null_pairing_fault(lookback: Any) -> Optional[str]:
    """The text of the fault when `lookback` holds a null has_unbounded_state in a pairing that
    the documented contract forbids, else None. Null is valid ONLY together with status
    "unknown" and a null declared_required_chart_bars, both present (the wire writes null and
    never omits a member of an opted-in report): a null beside any other status, or beside a
    number, zero included, is an invalid report.

    That is the whole rule. It asks nothing else of the record: a boolean has_unbounded_state
    (the shape of a build WITH the producer), the set of statuses, the range of a count, a
    member that is absent next to a boolean and a record that is not an object are other
    lookback schema questions, which this function does not invent."""
    if (not isinstance(lookback, dict) or "has_unbounded_state" not in lookback
            or lookback["has_unbounded_state"] is not None):
        return None
    if (lookback.get("status") == "unknown" and "declared_required_chart_bars" in lookback
            and lookback["declared_required_chart_bars"] is None):
        return None
    return ("report_window.lookback has has_unbounded_state null beside status "
            f"{lookback.get('status')!r} and declared_required_chart_bars "
            f"{lookback.get('declared_required_chart_bars', '<absent>')!r}: null is valid only "
            "beside status 'unknown' and a null declared_required_chart_bars")


def build_report_window(request: WindowRequest, plan: Mapping[str, Any], *, applied: bool,
                        feed: Mapping[str, Any], calendar: Mapping[str, Any]) -> Dict[str, Any]:
    """The exact record R (the documented wire table) from the planner's plan.

    `feed` holds input_tf_seconds, source_bytes_sha256, source_values_sha256 and
    evaluated_source_values_sha256; `calendar` holds timezone and session. applied is True for
    a run, False for validation only (then equity_points and anchor_points are 0, and the counts
    are plans, not execution evidence). Every count is the planner's; the wire's counting
    equalities are checked here and a failure is an engine invariant. Lookback is not produced
    by this build: the record is _lookback_not_produced()'s triple, status
    unknown with a null declared count and a null has_unbounded_state, in a run and in
    validation-only output alike."""
    counts = {key: _u53(plan, key) for key in _COUNT_KEYS}
    window_first = _optional_i53(plan, "window_first_data_ms")
    window_last = _optional_i53(plan, "window_last_data_ms")
    fed_first = _optional_i53(plan, "fed_first_data_ms")
    fed_last = _optional_i53(plan, "fed_last_data_ms")
    supplied_first = _optional_i53(plan, "supplied_first_data_ms")
    supplied_last = _optional_i53(plan, "supplied_last_data_ms")
    preroll_first = _optional_i53(plan, "preroll_first_bar_ms")
    preroll_last = _optional_i53(plan, "preroll_last_bar_ms")
    trim_start = plan["trim_start_ms"]
    if isinstance(trim_start, bool) or not isinstance(trim_start, int) or abs(trim_start) > I53_MAX:
        raise _invariant(f"the plan's trim_start_ms is {trim_start!r}, not an i53")
    available, used = counts["available_script_bars"], counts["used_script_bars"]
    window_script, window_input = counts["window_script_bars"], counts["window_input_bars"]
    if (counts["supplied_input_bars"] != counts["trim_index"] + counts["fed_input_bars"]
            or counts["supplied_script_bars"] != available + window_script
            or counts["fed_input_bars"] != counts["preroll_input_bars"] + window_input
            or counts["fed_script_bars"] != used + window_script
            or used != min(request.preroll_bars, available)
            or counts["trimmed_script_bars"] != available - used
            or bool(plan["shortfall"]) != (available < request.preroll_bars)):
        raise _invariant("the plan breaks the wire's counting equalities")
    for key in ("source_bytes_sha256", "source_values_sha256", "evaluated_source_values_sha256"):
        value = feed[key]
        if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
            raise _internal(f"{key} is not a lowercase SHA-256 digest")
    seconds = feed["input_tf_seconds"]
    if isinstance(seconds, bool) or not isinstance(seconds, int) or not 0 < seconds <= I53_MAX:
        raise _internal("input_tf_seconds is not a positive u53")
    nonempty_window = window_script > 0
    return {
        "wire_version": WIRE_VERSION,
        "policy": POLICY,
        "applied": bool(applied),
        "window": {
            "start_ms": request.start_ms,
            "end_ms": request.end_ms,
            "first_data_ms": window_first,
            "last_data_ms": window_last,
            "coverage": "nonempty" if window_input > 0 else "empty",
        },
        "feed": {
            "requested_start_ms": request.fed_start_ms,
            "first_data_ms": fed_first,
            "last_data_ms": fed_last,
            "supplied_first_data_ms": supplied_first,
            "supplied_last_data_ms": supplied_last,
            "input_tf_seconds": seconds,
            "script_tf": request.script_tf,
            "source_bytes_sha256": feed["source_bytes_sha256"],
            "source_values_sha256": feed["source_values_sha256"],
            "evaluated_source_values_sha256": feed["evaluated_source_values_sha256"],
        },
        "calendar": {"timezone": calendar["timezone"], "session": calendar["session"]},
        "preroll": {
            "requested_script_bars": request.preroll_bars,
            "available_script_bars": available,
            "used_script_bars": used,
            "trimmed_script_bars": counts["trimmed_script_bars"],
            "trim_start_ms": trim_start,
            "trimmed_input_bars": counts["trim_index"],
            "first_bar_ms": preroll_first,
            "last_bar_ms": preroll_last,
            "shortfall": bool(plan["shortfall"]),
        },
        "lookback": _lookback_not_produced(),
        "counts": {
            "supplied_input_bars": counts["supplied_input_bars"],
            "supplied_script_bars": counts["supplied_script_bars"],
            "fed_input_bars": counts["fed_input_bars"],
            "fed_script_bars": counts["fed_script_bars"],
            "preroll_input_bars": counts["preroll_input_bars"],
            "window_input_bars": window_input,
            "window_script_bars": window_script,
            "equity_points": window_script + 1 if applied else 0,
            "anchor_points": 1 if applied else 0,
        },
        "metering": {
            "billable_input_bars": window_input,
            "preroll_billable_input_bars": 0,
            "basis": METERING_BASIS,
        },
        "indices": {
            "fed_script_index_of_window_first": used if nonempty_window else None,
            "supplied_script_index_of_window_first": available if nonempty_window else None,
        },
    }


def report_shape_full(canonical_equity_points: int, canonical_trades: int) -> Dict[str, Any]:
    """$.report_shape of a full/v1 report: nothing is projected away."""
    return {
        "version": 1,
        "name": REPORT_SHAPE,
        "curve_point_budget": None,
        "canonical_equity_points": canonical_equity_points,
        "emitted_equity_points": canonical_equity_points,
        "canonical_trades": canonical_trades,
        "emitted_trades": canonical_trades,
        "retained_point_indices": None,
        "curve_decimated": False,
    }


def _established_number(value: Any) -> Optional[float]:
    """value as a finite float when it is a number (not a bool) or text that spells one, else
    None: the only forms of an effective initial capital the provenance can hold. None means
    the provenance establishes no capital (an unresolved one is recorded as null there); it is
    no verdict on the run."""
    if isinstance(value, bool):
        return None
    if isinstance(value, str):
        try:
            value = float(value.strip())
        except ValueError:
            return None
    if not isinstance(value, (int, float)):
        return None
    try:
        number = float(value)
    except OverflowError:
        return None
    return number if math.isfinite(number) else None


def _finite_actual(value: Any) -> bool:
    """Whether value, as the engine produced it, is a finite number: an int or a float that is
    not a bool, not None (the report writes a non-finite double as null), not text, and not NaN
    or an infinity. Nothing is converted or read from text."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(float(value))
    except OverflowError:  # an int beyond binary64
        return False


def check_selected_curve(curve: Sequence[Mapping[str, Any]], start_ms: int,
                         window_script_bars: int, initial_capital: Any) -> None:
    """The serialized selected curve is the native selected curve, unchanged: exactly three
    fields per point (time_ms, equity, open_profit) and no other, M + 1 points, position 0 the
    anchor at T with open profit 0 and an equity that is a finite number, position i >= 1 the
    window bar i - 1. Nothing is added, dropped or recomputed.

    `initial_capital` is the effective strategy setting of the run's provenance, read as the
    provenance holds it. The native anchor derives from the engine's own initial capital, so a
    provenance that does not establish one (an unresolved capital is null there, as in ordinary
    mode) is no evidence that the anchor is wrong: then ONLY the comparison of the anchor equity
    with the capital is skipped. No capital is guessed or written back, and every other check
    above, the finiteness of the actual anchor equity included, is made in both cases. When the
    provenance does establish a finite capital (a number or text spelling one), the anchor
    equity must equal it, or the run is an engine invariant failure."""
    if len(curve) != window_script_bars + 1:
        raise _invariant(f"the selected curve has {len(curve)} points, the plan says "
                         f"{window_script_bars + 1}")
    for position, point in enumerate(curve):
        if set(point) != {"time_ms", "equity", "open_profit"}:
            raise _invariant(f"point {position} of the selected curve does not have exactly "
                             "time_ms, equity and open_profit")
    anchor = curve[0]
    if anchor["time_ms"] != start_ms or anchor["open_profit"] != 0:
        raise _invariant("the selected curve does not start with the window anchor at T with "
                         "open profit 0")
    if not _finite_actual(anchor["equity"]):
        raise _invariant(f"the anchor equity {anchor['equity']!r} of the selected curve is "
                         "not a finite number")
    capital = _established_number(initial_capital)
    if capital is not None and anchor["equity"] != capital:
        raise _invariant(f"the anchor equity {anchor['equity']!r} is not the initial capital "
                         f"{capital!r}")


def check_report_placements(report: Mapping[str, Any], window: Mapping[str, Any]) -> None:
    """The selected report carries R in all three required places and a version 2
    fingerprint, or it is not a report: $.report_window, $.applied_runtime.report_window and
    $.fingerprint.provenance.runtime.report_window must each equal `window`; the fingerprint has
    version 2 and provenance.schema_version 2; and the HASHED bytes (the base64 token, whose
    SHA-256 must be the digest) hold schema_version 2 and the same R. R's lookback record is
    held to the documented null pairing (a null has_unbounded_state only beside status
    "unknown" and a null declared_required_chart_bars) and to nothing else about lookback; the
    copies equal `window`, so they carry the same triple. Any failure is an engine invariant
    raised before anything is serialized."""
    fault = _lookback_null_pairing_fault(window.get("lookback"))
    if fault is not None:
        raise _invariant(fault)
    fingerprint = report.get("fingerprint")
    provenance = fingerprint.get("provenance") if isinstance(fingerprint, dict) else None
    if not isinstance(fingerprint, dict) or not isinstance(provenance, dict):
        raise _invariant("the selected report has no fingerprint")
    applied = report.get("applied_runtime")
    runtime = provenance.get("runtime")
    places = (
        ("$.report_window", report.get("report_window")),
        ("$.applied_runtime.report_window",
         applied.get("report_window") if isinstance(applied, dict) else None),
        ("$.fingerprint.provenance.runtime.report_window",
         runtime.get("report_window") if isinstance(runtime, dict) else None),
    )
    for path, placed in places:
        if placed != window:
            raise _invariant(f"{path} is not the report_window record of the run")
    if fingerprint.get("version") != FINGERPRINT_VERSION:
        raise _invariant("the selected fingerprint is not version 2")
    if provenance.get("schema_version") != FINGERPRINT_VERSION:
        raise _invariant("the selected provenance is not schema version 2")
    token, digest = fingerprint.get("token"), fingerprint.get("digest")
    try:
        raw = base64.b64decode(token, validate=True)
        hashed = json.loads(raw.decode("utf-8"))
    except (TypeError, ValueError, binascii.Error, RecursionError):
        raise _invariant("the selected fingerprint token cannot be read back") from None
    if digest != "sha256:" + hashlib.sha256(raw).hexdigest():
        raise _invariant("the selected fingerprint digest is not the hash of its token")
    hashed_runtime = hashed.get("runtime") if isinstance(hashed, dict) else None
    if (not isinstance(hashed, dict) or hashed.get("schema_version") != FINGERPRINT_VERSION
            or not isinstance(hashed_runtime, dict)
            or hashed_runtime.get("report_window") != window):
        raise _invariant("the hashed bytes of the selected fingerprint do not hold schema "
                         "version 2 and the run's report_window")


def capabilities_record(supported: bool) -> Dict[str, Any]:
    """The --capabilities-json document. A runtime that cannot establish the native ABIs, the
    observer and the planner reports wire version 0 and the other members null or empty; the
    members of features this build does not offer (compact shape, digest) are always absent.

    selected_window_contract_revisions is CONTRACT_REVISIONS as a fresh list when supported:
    exactly ["1.3-parity"], the prerelease parity declaration (not full 1.3 coverage), and
    never "1.3", which only a full build carries. It is [] when the runtime is not supported,
    so an unsupported runtime lists no revision at all. No other member of this document
    changes with the declaration."""
    return {
        "engine": "pineforge",
        "capabilities": {
            "selected_window_wire_version": WIRE_VERSION if supported else 0,
            "selected_window_contract_revisions": list(CONTRACT_REVISIONS) if supported else [],
            "report_policy": POLICY if supported else None,
            "fingerprint_version": FINGERPRINT_VERSION if supported else None,
            "report_shapes": [REPORT_SHAPE] if supported else [],
            "curve_point_budget": None,
            "results_digest_version": None,
            "results_digest_platform": None,
        },
    }


# --- the frozen selected_window ABI ---

ABI_VERSION = 1
_SET_OK = 0
_SET_STATUS_MEANING = {
    -1: "invalid handle, size, version or bounds",
    -2: "unsupported execution contract",
    -3: "not quiescent",
    -4: "out of memory",
}
_COUNTS_NOT_OWNED = -2


class SelectedWindowConfigC(ctypes.Structure):
    """pf_selected_window_config_v1 (LP64: 24 bytes): the window [T, E)."""
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("start_ms", ctypes.c_int64),
        ("end_ms", ctypes.c_int64),
    ]


class SelectedWindowCountsC(ctypes.Structure):
    """pf_selected_window_counts_v1 (LP64: 80 bytes): the engine's own counts of one successful
    selected generation, with the identities that own it."""
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("run_generation", ctypes.c_uint64),
        ("attempt_serial", ctypes.c_uint64),
        ("attempt_generation", ctypes.c_uint64),
        ("fed_input_bars", ctypes.c_uint64),
        ("fed_script_bars", ctypes.c_uint64),
        ("preroll_input_bars", ctypes.c_uint64),
        ("preroll_script_bars", ctypes.c_uint64),
        ("window_input_bars", ctypes.c_uint64),
        ("window_script_bars", ctypes.c_uint64),
    ]


_ABI_EXPORTS = ("pf_selected_window_version", "strategy_set_selected_window_v1",
                "strategy_selected_window_counts_v1")


def _unsupported_abi(text: str) -> SelectedWindowError:
    return SelectedWindowError("window_mode_unsupported", text, {"capability": CAPABILITY})


class SelectedWindowAbi:
    """The three exports of the selected-window ABI on one loaded strategy library: probed at
    construction (a missing export or a version other than 1 is window_mode_unsupported with
    capability selected_window_v1), then configure() before the run, counts() after it and
    clear() in cleanup. `library` is the loaded strategy library or anything with the exports."""

    def __init__(self, library: Any) -> None:
        if (ctypes.sizeof(SelectedWindowConfigC) != 24
                or ctypes.sizeof(SelectedWindowCountsC) != 80):
            raise _internal("the ctypes layout of the selected-window structs is not the frozen "
                            "24 and 80 bytes")
        try:
            version = library.pf_selected_window_version
            self._set = library.strategy_set_selected_window_v1
            self._counts = library.strategy_selected_window_counts_v1
        except AttributeError as error:
            raise _unsupported_abi(
                f"the strategy library lacks a selected-window export: {error}") from error
        version.argtypes = []
        version.restype = ctypes.c_uint32
        self._set.argtypes = [ctypes.c_void_p, ctypes.POINTER(SelectedWindowConfigC)]
        self._set.restype = ctypes.c_int
        self._counts.argtypes = [ctypes.c_void_p, ctypes.POINTER(SelectedWindowCountsC)]
        self._counts.restype = ctypes.c_int
        found = version()
        if found != ABI_VERSION:
            raise _unsupported_abi(
                f"the strategy library reports selected-window version {found}, "
                f"not {ABI_VERSION}")

    def configure(self, state: Any, start_ms: int, end_ms: int) -> None:
        """Select [start_ms, end_ms) for the NEXT admitted generation of `state`."""
        config = SelectedWindowConfigC(ctypes.sizeof(SelectedWindowConfigC), ABI_VERSION,
                                       start_ms, end_ms)
        status = self._set(state, ctypes.pointer(config))
        if status == _SET_OK:
            return
        meaning = _SET_STATUS_MEANING.get(status, "unknown status")
        text = f"the selected window was refused with status {status} ({meaning})"
        if status == -4:
            raise SelectedWindowError("out_of_memory", text)
        if status == -2:
            raise SelectedWindowError("window_mode_unsupported", text,
                                      {"capability": CAPABILITY})
        raise _internal(text)

    def clear(self, state: Any) -> int:
        """Remove the configuration (a NULL descriptor) for cleanup; the status is returned
        and is never an error here, since the state is about to be freed."""
        return self._set(state, None)

    def counts(self, state: Any) -> Dict[str, int]:
        """The engine's counts of the latest attempt, which must own a successful selected
        generation: anything else is an engine invariant (the run reported success)."""
        counts = SelectedWindowCountsC(ctypes.sizeof(SelectedWindowCountsC), ABI_VERSION)
        status = self._counts(state, ctypes.byref(counts))
        if status == _COUNTS_NOT_OWNED:
            raise _invariant("the engine reports no successful selected generation for the "
                             "latest attempt")
        if status != 0:
            raise _internal(f"strategy_selected_window_counts_v1 returned {status}")
        if counts.struct_size != ctypes.sizeof(SelectedWindowCountsC) or counts.version != ABI_VERSION:
            raise _internal("the selected-window counts came back with a changed header")
        return {name: int(getattr(counts, name)) for name, _ in SelectedWindowCountsC._fields_
                if name not in ("struct_size", "version")}


def check_native_counts(plan: Mapping[str, Any], native: Mapping[str, int],
                        observation: Mapping[str, int]) -> None:
    """The engine's counts must equal the planned ones exactly, and belong to the observed
    attempt. A mismatch is an engine invariant: the plan's counts are never replaced by the
    engine's, and the engine's never by the plan's.

    planned fed_input / fed_script / preroll_input / preroll_script (= used pre-roll bars) /
    window_input / window_script  versus  the same six native counts; the three identities
    (run_generation, attempt_serial, attempt_generation) of the counts must equal those of the
    execution observation and be nonzero."""
    expected = {
        "fed_input_bars": plan["fed_input_bars"],
        "fed_script_bars": plan["fed_script_bars"],
        "preroll_input_bars": plan["preroll_input_bars"],
        "preroll_script_bars": plan["used_script_bars"],
        "window_input_bars": plan["window_input_bars"],
        "window_script_bars": plan["window_script_bars"],
    }
    for name, planned in expected.items():
        if native[name] != planned:
            raise _invariant(f"the engine counted {native[name]} for {name}, the plan "
                             f"says {planned}")
    for name in ("run_generation", "attempt_serial", "attempt_generation"):
        if native[name] == 0 or native[name] != observation[name]:
            raise _invariant(f"the selected-window counts name {name} {native[name]}, the "
                             f"execution observation names {observation[name]}")
