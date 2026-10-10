"""The selected primary planner's standalone C bridge, seen from Python (frozen v1).

The bridge is the shared library built from the pinned tree (target
pineforge_window_plan, installed as lib/libpineforge_window_plan.so), which
links only the kernel and never strategy code. This module mirrors its two
structs and the engine's bar (RequestC, ResultC, BarC), loads the one library
the caller names by absolute path, calls pf_plan_selected_primary_v1 once per
plan() and returns the planner's own values as a dictionary of the C++
SelectedPrimaryPlan's field names. It carries no calendar, count, hash or phase
logic of its own, searches no path and never falls back to a strategy library.

A refusal of the planner is not an error here: it is a nonzero "status" (the
SelectedPlanStatus value) in the returned dictionary, for the caller to map to
its own catalog. PlanBridgeError is only the bridge failing: the capability
missing (window_mode_unsupported), the planner out of memory (out_of_memory), or
a call, return or result that breaks the frozen contract (harness_internal_error).
It does not import run_json; the caller that owns RunFailure maps its code,
message and code_args.
"""

import ctypes
import os

__all__ = [
    "BarC", "RequestC", "ResultC", "PlanBridgeError", "SelectedPrimaryPlanner",
    "PLAN_ABI_VERSION", "CAPABILITY", "PLAN_FIELDS", "STATUS_NAMES",
    "CODE_WINDOW_MODE_UNSUPPORTED", "CODE_OUT_OF_MEMORY", "CODE_HARNESS_INTERNAL_ERROR",
]

PLAN_ABI_VERSION = 1
CAPABILITY = "selected_window_planner_v1"

CODE_WINDOW_MODE_UNSUPPORTED = "window_mode_unsupported"
CODE_OUT_OF_MEMORY = "out_of_memory"
CODE_HARNESS_INTERNAL_ERROR = "harness_internal_error"

# pf_selected_plan_result_v1.status, by the C++ SelectedPlanStatus value. plan()
# returns the integer; the names are informational and their count bounds the
# status range check.
STATUS_NAMES = ("Ok", "RequestInvalid", "CalendarUnsupported", "BoundaryUnaligned",
                "FeedRangeInvalid", "RowsUnordered", "InternalError")

# No SelectedPlanStatus is negative. plan() presets the result's status to this
# value, after size and version and before the call, so a return 0 that never
# wrote the result reads as a status outside STATUS_NAMES and is refused, not
# passed as an all-zero Ok.
_UNWRITTEN_STATUS = -1

_BOUNDS = (-1, 0, 1, 2)  # -1 no boundary detail, 0 = T, 1 = E, 2 = F
_COUNT_FIELDS = (
    "supplied_input_bars", "supplied_script_bars", "available_script_bars",
    "used_script_bars", "trimmed_script_bars", "trim_index", "fed_input_bars",
    "fed_script_bars", "preroll_input_bars", "window_input_bars", "window_script_bars")
# present_mask bit i says the i-th of these eight timestamps is present; an
# absent one is zero in the struct and None in the dictionary.
_OPTIONAL_FIELDS = (
    "preroll_first_bar_ms", "preroll_last_bar_ms", "supplied_first_data_ms",
    "supplied_last_data_ms", "fed_first_data_ms", "fed_last_data_ms",
    "window_first_data_ms", "window_last_data_ms")
_FLAG_FIELDS = ("shortfall", "complete_pending_preroll_at_horizon")
_STRING_FIELDS = ("input_tf", "script_tf", "chart_timezone", "engine_timezone", "session")
_EXPORTS = ("pf_selected_plan_version", "pf_plan_selected_primary_v1")

# The keys of the dictionary plan() returns, in the C++ plan's field order.
PLAN_FIELDS = (("status", "option", "bound", "value_ms", "previous_boundary_ms",
                "next_boundary_ms") + _COUNT_FIELDS + ("trim_start_ms",)
               + _OPTIONAL_FIELDS + _FLAG_FIELDS)

_INT64_MIN, _INT64_MAX = -(1 << 63), (1 << 63) - 1
_UINT32_MAX = (1 << 32) - 1
# The count crosses as uint64_t and must also fit this platform's size_t.
_COUNT_MAX = min((1 << 64) - 1, (1 << (8 * ctypes.sizeof(ctypes.c_size_t))) - 1)


# --- ctypes mirror of <pineforge/selected_window_plan.h> and pf_bar_t -------

class BarC(ctypes.Structure):
    """pf_bar_t: five doubles then the int64 timestamp (48 bytes, alignment 8),
    the layout of run_json's BarC."""
    _fields_ = [
        ("open",      ctypes.c_double),
        ("high",      ctypes.c_double),
        ("low",       ctypes.c_double),
        ("close",     ctypes.c_double),
        ("volume",    ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]


class RequestC(ctypes.Structure):
    """pf_selected_plan_request_v1, natural alignment, no packing (LP64: 80 bytes).
    The caller presets struct_size to sizeof and version to 1."""
    _fields_ = [
        ("struct_size",     ctypes.c_uint32),
        ("version",         ctypes.c_uint32),
        ("start_ms",        ctypes.c_int64),
        ("end_ms",          ctypes.c_int64),
        ("fed_start_ms",    ctypes.c_int64),
        ("preroll_bars",    ctypes.c_uint32),
        ("feed_tolerant",   ctypes.c_uint32),
        ("input_tf",        ctypes.c_char_p),
        ("script_tf",       ctypes.c_char_p),
        ("chart_timezone",  ctypes.c_char_p),
        ("engine_timezone", ctypes.c_char_p),
        ("session",         ctypes.c_char_p),
    ]


class ResultC(ctypes.Structure):
    """pf_selected_plan_result_v1, natural alignment, no packing (LP64: 248
    bytes). The caller presets struct_size to sizeof and version to 1 (the frozen
    contract), then status to -1, which is no planner status; the bridge writes
    the whole struct on return 0 and, on a negative return, none of it."""
    _fields_ = [
        ("struct_size",           ctypes.c_uint32),
        ("version",               ctypes.c_uint32),
        ("status",                ctypes.c_int32),
        ("bound",                 ctypes.c_int32),
        ("value_ms",              ctypes.c_int64),
        ("previous_boundary_ms",  ctypes.c_int64),
        ("next_boundary_ms",      ctypes.c_int64),
        ("supplied_input_bars",   ctypes.c_uint64),
        ("supplied_script_bars",  ctypes.c_uint64),
        ("available_script_bars", ctypes.c_uint64),
        ("used_script_bars",      ctypes.c_uint64),
        ("trimmed_script_bars",   ctypes.c_uint64),
        ("trim_index",            ctypes.c_uint64),
        ("fed_input_bars",        ctypes.c_uint64),
        ("fed_script_bars",       ctypes.c_uint64),
        ("preroll_input_bars",    ctypes.c_uint64),
        ("window_input_bars",     ctypes.c_uint64),
        ("window_script_bars",    ctypes.c_uint64),
        ("trim_start_ms",         ctypes.c_int64),
        ("preroll_first_bar_ms",  ctypes.c_int64),
        ("preroll_last_bar_ms",   ctypes.c_int64),
        ("supplied_first_data_ms", ctypes.c_int64),
        ("supplied_last_data_ms", ctypes.c_int64),
        ("fed_first_data_ms",     ctypes.c_int64),
        ("fed_last_data_ms",      ctypes.c_int64),
        ("window_first_data_ms",  ctypes.c_int64),
        ("window_last_data_ms",   ctypes.c_int64),
        ("present_mask",          ctypes.c_uint32),
        ("shortfall",             ctypes.c_uint32),
        ("complete_pending_preroll_at_horizon", ctypes.c_uint32),
        ("reserved",              ctypes.c_uint32),
        ("option",                ctypes.c_char * 32),
    ]


_OPTION_START = ResultC.option.offset
_OPTION_END = _OPTION_START + ResultC.option.size

# (struct, sizeof, alignment, every field's offset): the frozen LP64 layout.
_LAYOUTS = (
    (BarC, 48, 8, (0, 8, 16, 24, 32, 40)),
    (RequestC, 80, 8, (0, 4, 8, 16, 24, 32, 36, 40, 48, 56, 64, 72)),
    (ResultC, 248, 8, (0, 4, 8, 12, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104,
                       112, 120, 128, 136, 144, 152, 160, 168, 176, 184, 192, 200,
                       204, 208, 212, 216)),
)


class PlanBridgeError(Exception):
    """The bridge failed; the planner did not refuse (a refusal is a status in
    the returned dictionary). code is window_mode_unsupported (code_args
    {"capability": "selected_window_planner_v1"}), out_of_memory or
    harness_internal_error (both with no code_args); message is the text."""

    def __init__(self, code, message, code_args=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.code_args = dict(code_args or {})


def _internal(message):
    return PlanBridgeError(CODE_HARNESS_INTERNAL_ERROR, message)


def _unsupported(message):
    return PlanBridgeError(CODE_WINDOW_MODE_UNSUPPORTED, message, {"capability": CAPABILITY})


def _check_layout():
    """The ctypes structs have exactly the frozen size, alignment and offsets."""
    for struct, size, alignment, offsets in _LAYOUTS:
        found = (ctypes.sizeof(struct), ctypes.alignment(struct),
                 tuple(getattr(struct, field[0]).offset for field in struct._fields_))
        if found != (size, alignment, offsets):
            raise _internal(
                f"the ctypes layout of {struct.__name__} is (size, alignment, offsets) "
                f"{found}, the frozen layout is {(size, alignment, offsets)}")


def _raw(struct):
    return ctypes.string_at(ctypes.addressof(struct), ctypes.sizeof(struct))


def _library_path(library_path):
    """The explicit absolute path, as text: no search, no default, no fallback."""
    try:
        path = os.fspath(library_path)
    except TypeError:
        raise _internal("library_path must be the absolute path of the planner library, "
                        f"got {type(library_path).__name__}") from None
    if not isinstance(path, str) or "\0" in path or not os.path.isabs(path):
        raise _internal("library_path must be the absolute path of the planner library, "
                        f"got {path!r}")
    return path


def _integer(name, value, low, high):
    if isinstance(value, bool) or not isinstance(value, int):
        raise _internal(f"{name} must be an int, got {type(value).__name__}")
    if not low <= value <= high:
        raise _internal(f"{name} {value} is outside {low}..{high}")
    return value


def _utf8(name, value):
    if not isinstance(value, str):
        raise _internal(f"{name} must be a str, got {type(value).__name__}")
    if "\0" in value:
        raise _internal(f"{name} holds an embedded NUL")
    try:
        return value.encode("utf-8")
    except UnicodeEncodeError:
        raise _internal(f"{name} is not encodable as UTF-8") from None


def _bar_layout(element):
    """element is BarC or a Structure of exactly its fields, types and offsets."""
    if element is BarC:
        return True
    if not (isinstance(element, type) and issubclass(element, ctypes.Structure)):
        return False
    fields = getattr(element, "_fields_", ())
    if len(fields) != len(BarC._fields_):
        return False
    for field, expected in zip(fields, BarC._fields_):
        if (len(field) != 2 or field[0] != expected[0] or field[1] is not expected[1]
                or getattr(element, field[0]).offset != getattr(BarC, expected[0]).offset):
            return False
    return (ctypes.sizeof(element) == ctypes.sizeof(BarC)
            and ctypes.alignment(element) == ctypes.alignment(BarC))


def _rows_argument(bars, count):
    """(the POINTER(BarC) the call takes, the validated count). bars is a ctypes
    array or pointer of BarC-layout rows (run_json's BarC array qualifies), used
    where it lies: nothing is copied. A null bars is accepted only with count 0."""
    count = _integer("count", count, 0, _COUNT_MAX)
    if bars is None:
        if count:
            raise _internal(f"bars is None but count is {count}: null rows need count 0")
        return None, 0
    if isinstance(bars, ctypes.Array):
        if count > len(bars):
            raise _internal(f"count {count} is more than the {len(bars)} rows of bars")
    elif isinstance(bars, ctypes._Pointer):
        if count and not bars:
            raise _internal(f"bars is a null pointer but count is {count}: null rows need count 0")
    else:
        raise _internal("bars must be a ctypes array or pointer of BarC rows, "
                        f"got {type(bars).__name__}")
    if not _bar_layout(bars._type_):
        raise _internal("the rows of bars are not the BarC layout (open, high, low, close, "
                        "volume as c_double, then timestamp as c_int64)")
    return ctypes.cast(bars, ctypes.POINTER(BarC)), count


def _request_struct(start_ms, end_ms, fed_start_ms, preroll_bars, input_tf, script_tf,
                    chart_timezone, engine_timezone, session, feed_tolerant):
    """(the RequestC, the encoded strings it points into). The strings must stay
    referenced until the call has returned."""
    values = {
        "start_ms": _integer("start_ms", start_ms, _INT64_MIN, _INT64_MAX),
        "end_ms": _integer("end_ms", end_ms, _INT64_MIN, _INT64_MAX),
        "fed_start_ms": _integer("fed_start_ms", fed_start_ms, _INT64_MIN, _INT64_MAX),
        "preroll_bars": _integer("preroll_bars", preroll_bars, 0, _UINT32_MAX),
    }
    if not isinstance(feed_tolerant, bool):
        raise _internal(f"feed_tolerant must be a bool, got {type(feed_tolerant).__name__}")
    texts = (input_tf, script_tf, chart_timezone, engine_timezone, session)
    strings = tuple(_utf8(name, text) for name, text in zip(_STRING_FIELDS, texts))
    request = RequestC(struct_size=ctypes.sizeof(RequestC), version=PLAN_ABI_VERSION,
                       feed_tolerant=1 if feed_tolerant else 0,
                       **values, **dict(zip(_STRING_FIELDS, strings)))
    return request, strings


def _option_text(raw):
    """The result's option: ASCII, NUL-terminated and zero-padded in its 32 bytes."""
    end = raw.find(b"\0")
    if end < 0:
        raise _internal("the result option is not NUL-terminated within its 32 bytes")
    if any(raw[end:]):
        raise _internal("the result option is not zero-padded after its NUL")
    if not raw[:end].isascii():
        raise _internal("the result option is not ASCII")
    return raw[:end].decode("ascii")


def _plan_from(result):
    """The result, validated, as the C++ plan's named fields. The planner's own
    values pass through unchanged: no count, calendar or hash is recomputed."""
    if result.struct_size != ctypes.sizeof(ResultC) or result.version != PLAN_ABI_VERSION:
        raise _internal(f"the result header is (size {result.struct_size}, version "
                        f"{result.version}), expected ({ctypes.sizeof(ResultC)}, "
                        f"{PLAN_ABI_VERSION})")
    if not 0 <= result.status < len(STATUS_NAMES):
        raise _internal(f"the result status {result.status} is not a planner status")
    if result.bound not in _BOUNDS:
        raise _internal(f"the result bound {result.bound} is not one of {_BOUNDS}")
    mask = result.present_mask
    if mask >> len(_OPTIONAL_FIELDS):
        raise _internal(f"the result present_mask {mask:#x} sets bits past bit "
                        f"{len(_OPTIONAL_FIELDS) - 1}")
    for bit, name in enumerate(_OPTIONAL_FIELDS):
        if not (mask >> bit) & 1 and getattr(result, name) != 0:
            raise _internal(f"the result {name} is {getattr(result, name)} but absent in "
                            f"present_mask {mask:#x}")
    for name in _FLAG_FIELDS:
        if getattr(result, name) not in (0, 1):
            raise _internal(f"the result {name} is {getattr(result, name)}, not 0 or 1")
    if result.reserved != 0:
        raise _internal(f"the result reserved word is {result.reserved}, not 0")
    option = _option_text(_raw(result)[_OPTION_START:_OPTION_END])
    plan = {"status": result.status, "option": option, "bound": result.bound}
    for name in (("value_ms", "previous_boundary_ms", "next_boundary_ms") + _COUNT_FIELDS
                 + ("trim_start_ms",)):
        plan[name] = getattr(result, name)
    for bit, name in enumerate(_OPTIONAL_FIELDS):
        plan[name] = getattr(result, name) if (mask >> bit) & 1 else None
    for name in _FLAG_FIELDS:
        plan[name] = bool(getattr(result, name))
    return plan


def _bridge_failure(code, output_changed):
    """The PlanBridgeError for a nonzero return of pf_plan_selected_primary_v1."""
    if code < 0 and output_changed:
        return _internal(f"pf_plan_selected_primary_v1 returned {code} and still changed "
                         "the caller's result")
    if code == -2:
        return PlanBridgeError(CODE_OUT_OF_MEMORY,
                               "the selected primary planner failed: std::bad_alloc")
    if code == -1:
        return _internal("pf_plan_selected_primary_v1 refused the call (return -1: a null "
                         "descriptor, a wrong size or version, a null string, a bad "
                         "feed_tolerant or an unrepresentable count)")
    if code == -3:
        return _internal("pf_plan_selected_primary_v1 failed on an exception or invariant "
                         "(return -3)")
    return _internal(f"pf_plan_selected_primary_v1 returned {code}, which is not 0, -1, -2 "
                     "or -3")


class SelectedPrimaryPlanner:
    """The trusted standalone planner library, loaded once from the explicit
    absolute path of the helper built with the pinned tree (never a strategy
    library). The constructor probes the version and binds both functions; the
    object keeps the library loaded for every later call."""

    def __init__(self, library_path):
        _check_layout()
        path = _library_path(library_path)
        try:
            library = ctypes.CDLL(path)
        except OSError as error:
            raise _unsupported(f"cannot load the selected primary planner library "
                               f"{path}: {error}") from None
        missing = [name for name in _EXPORTS if not hasattr(library, name)]
        if missing:
            raise _unsupported(f"the selected primary planner library {path} has no "
                               + ", ".join(missing))
        version_function = library.pf_selected_plan_version
        version_function.argtypes = []
        version_function.restype = ctypes.c_uint32
        plan_function = library.pf_plan_selected_primary_v1
        plan_function.argtypes = [ctypes.POINTER(RequestC), ctypes.POINTER(BarC),
                                  ctypes.c_uint64, ctypes.POINTER(ResultC)]
        plan_function.restype = ctypes.c_int
        version = version_function()
        if version != PLAN_ABI_VERSION:
            raise _unsupported(f"the selected primary planner library {path} reports "
                               f"version {version}, expected {PLAN_ABI_VERSION}")
        self._path = path
        self._library = library
        self._plan = plan_function

    @property
    def library_path(self):
        return self._path

    def plan(self, bars, count, *, start_ms, end_ms, fed_start_ms, preroll_bars,
             input_tf, script_tf, chart_timezone="", engine_timezone="", session="",
             feed_tolerant=True):
        """The planner's plan for the first count rows of bars and one selected
        window request, as a dict of the C++ SelectedPrimaryPlan's field names:
        status (the SelectedPlanStatus integer, 0 = Ok, a refusal otherwise),
        option as str, bound, value_ms, previous_boundary_ms, next_boundary_ms,
        the counts, trim_start_ms, the eight optional timestamps as int or None,
        shortfall and complete_pending_preroll_at_horizon as bool. bars is a
        ctypes array or pointer of BarC-layout rows (None with count 0), read in
        place. Raises PlanBridgeError when the bridge, not the planner, fails (a
        return 0 that wrote no result is such a failure)."""
        rows, row_count = _rows_argument(bars, count)
        request, strings = _request_struct(
            start_ms, end_ms, fed_start_ms, preroll_bars, input_tf, script_tf,
            chart_timezone, engine_timezone, session, feed_tolerant)
        result = ResultC(struct_size=ctypes.sizeof(ResultC), version=PLAN_ABI_VERSION)
        result.status = _UNWRITTEN_STATUS
        before = _raw(result)  # the baseline a negative return must leave unchanged
        # Everything the native call reads or writes stays referenced until it has
        # returned: the library, the caller's row object, the encoded strings and
        # both structs.
        held = (self._library, bars, rows, strings, request, result)
        try:
            code = self._plan(ctypes.pointer(request), rows, row_count, ctypes.pointer(result))
        except ctypes.ArgumentError as error:
            raise _internal("pf_plan_selected_primary_v1 refused its arguments: "
                            f"{error}") from None
        finally:
            del held
        if isinstance(code, bool) or not isinstance(code, int):
            raise _internal(f"pf_plan_selected_primary_v1 returned {code!r}, not an int")
        if code != 0:
            raise _bridge_failure(code, _raw(result) != before)
        return _plan_from(result)
