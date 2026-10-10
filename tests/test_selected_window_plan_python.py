"""Tests of docker/selected_window_plan.py against a FAKE ABI object.

Nothing here loads libpineforge_window_plan: a fake library stands in for
ctypes.CDLL, so these tests cover only the wrapper's struct layout, argument and
result mapping, error codes, lifetimes and validation.
"""

import ctypes
import gc
import inspect
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import selected_window_plan as swp  # noqa: E402

LIB_PATH = "/opt/pineforge/lib/libpineforge_window_plan.so"

# The frozen LP64 layout of the planner's C bridge, restated here on purpose.
BAR_LAYOUT = (("open", 0), ("high", 8), ("low", 16), ("close", 24), ("volume", 32),
              ("timestamp", 40))
REQUEST_LAYOUT = (
    ("struct_size", 0), ("version", 4), ("start_ms", 8), ("end_ms", 16),
    ("fed_start_ms", 24), ("preroll_bars", 32), ("feed_tolerant", 36), ("input_tf", 40),
    ("script_tf", 48), ("chart_timezone", 56), ("engine_timezone", 64), ("session", 72))
RESULT_LAYOUT = (
    ("struct_size", 0), ("version", 4), ("status", 8), ("bound", 12), ("value_ms", 16),
    ("previous_boundary_ms", 24), ("next_boundary_ms", 32), ("supplied_input_bars", 40),
    ("supplied_script_bars", 48), ("available_script_bars", 56), ("used_script_bars", 64),
    ("trimmed_script_bars", 72), ("trim_index", 80), ("fed_input_bars", 88),
    ("fed_script_bars", 96), ("preroll_input_bars", 104), ("window_input_bars", 112),
    ("window_script_bars", 120), ("trim_start_ms", 128), ("preroll_first_bar_ms", 136),
    ("preroll_last_bar_ms", 144), ("supplied_first_data_ms", 152),
    ("supplied_last_data_ms", 160), ("fed_first_data_ms", 168), ("fed_last_data_ms", 176),
    ("window_first_data_ms", 184), ("window_last_data_ms", 192), ("present_mask", 200),
    ("shortfall", 204), ("complete_pending_preroll_at_horizon", 208), ("reserved", 212),
    ("option", 216))
OPTIONALS = ("preroll_first_bar_ms", "preroll_last_bar_ms", "supplied_first_data_ms",
             "supplied_last_data_ms", "fed_first_data_ms", "fed_last_data_ms",
             "window_first_data_ms", "window_last_data_ms")
COUNTS = ("supplied_input_bars", "supplied_script_bars", "available_script_bars",
          "used_script_bars", "trimmed_script_bars", "trim_index", "fed_input_bars",
          "fed_script_bars", "preroll_input_bars", "window_input_bars", "window_script_bars")
PLAN_KEYS = (("status", "option", "bound", "value_ms", "previous_boundary_ms",
              "next_boundary_ms") + COUNTS + ("trim_start_ms",) + OPTIONALS
             + ("shortfall", "complete_pending_preroll_at_horizon"))
CAPABILITY_ARGS = {"capability": "selected_window_planner_v1"}


class ForeignBar(ctypes.Structure):
    """Another module's bar mirror with the documented layout (like run_json's)."""
    _fields_ = [("open", ctypes.c_double), ("high", ctypes.c_double),
                ("low", ctypes.c_double), ("close", ctypes.c_double),
                ("volume", ctypes.c_double), ("timestamp", ctypes.c_int64)]


class ReorderedBar(ctypes.Structure):
    _fields_ = [("timestamp", ctypes.c_int64), ("open", ctypes.c_double),
                ("high", ctypes.c_double), ("low", ctypes.c_double),
                ("close", ctypes.c_double), ("volume", ctypes.c_double)]


class ShortBar(ctypes.Structure):
    _fields_ = [("open", ctypes.c_double), ("timestamp", ctypes.c_int64)]


def must(condition, message):
    if not condition:
        raise AssertionError(message)


def churn():
    """Free and reuse small objects, so a request string that nothing kept alive
    would be overwritten before the stub reads it."""
    gc.collect()
    junk = [bytes([index % 251]) * 3 for index in range(5000)]
    del junk


def canned_result(option=b"", **fields):
    values = dict(struct_size=ctypes.sizeof(swp.ResultC), version=1, status=0, bound=-1)
    values.update(fields)
    return swp.ResultC(option=option, **values)


def full_result(mask, option=b"start_ms"):
    """Every field distinct and nonzero where present, zero where absent."""
    fields = {"status": 0, "bound": 0, "value_ms": 7, "previous_boundary_ms": -8,
              "next_boundary_ms": 9, "trim_start_ms": 10, "present_mask": mask,
              "shortfall": 1, "complete_pending_preroll_at_horizon": 1}
    for index, name in enumerate(COUNTS):
        fields[name] = 100 + index
    for bit, name in enumerate(OPTIONALS):
        fields[name] = 1000 + bit if (mask >> bit) & 1 else 0
    return canned_result(option=option, **fields)


def put_raw_option(result, raw):
    """Write exactly 32 raw bytes (raw zero-padded on the right) over option."""
    raw = raw.ljust(32, b"\0")
    must(len(raw) == 32, "an option is 32 bytes")
    ctypes.memmove(ctypes.addressof(result) + swp.ResultC.option.offset, raw, 32)


def write_result(result_pointer, canned):
    ctypes.memmove(ctypes.addressof(result_pointer.contents), ctypes.addressof(canned),
                   ctypes.sizeof(swp.ResultC))


def check_arguments(argtypes, args):
    """What ctypes would check against the bound argtypes."""
    must(len(argtypes) == len(args), f"{len(args)} arguments for {len(argtypes)} argtypes")
    for argtype, arg in zip(argtypes, args):
        if issubclass(argtype, ctypes._Pointer):
            must(arg is None or isinstance(arg, argtype), f"{type(arg)} for {argtype}")
        else:
            must(isinstance(arg, int) and not isinstance(arg, bool), f"{arg!r} for {argtype}")


class FakeFunction:
    def __init__(self, body):
        self.body = body
        self.argtypes = None
        self.restype = None
        self.calls = 0

    def __call__(self, *args):
        self.calls += 1
        if self.argtypes is not None:
            check_arguments(self.argtypes, args)
        return self.body(*args)


class FakeLibrary:
    """What ctypes.CDLL returns: an export is a function object, a missing one
    raises AttributeError like an undefined symbol."""

    def __init__(self, plan_body, version=1, exports=swp._EXPORTS):
        if "pf_selected_plan_version" in exports:
            self.pf_selected_plan_version = FakeFunction(lambda: version)
        if "pf_plan_selected_primary_v1" in exports:
            self.pf_plan_selected_primary_v1 = FakeFunction(plan_body)


class Recorder:
    """A plan body: records what the bridge was handed at call time, writes a
    canned result (write=False leaves the caller's result as preset) and returns
    a canned code."""

    def __init__(self, result=None, returns=0, write=True):
        self.result = result if result is not None else canned_result()
        self.returns = returns
        self.write = write
        self.seen = []

    def __call__(self, request_pointer, rows, count, result_pointer):
        request = request_pointer.contents
        churn()
        header = result_pointer.contents
        seen = {
            "request": {name: getattr(request, name) for name, _ in swp.RequestC._fields_},
            "request_address": ctypes.addressof(request),
            "result_before": ctypes.string_at(ctypes.addressof(header),
                                              ctypes.sizeof(swp.ResultC)),
            "rows": rows, "rows_address": ctypes.addressof(rows.contents) if rows else None,
            "count": count,
            "timestamps": [rows[index].timestamp for index in range(count)] if rows else [],
        }
        self.seen.append(seen)
        if self.write:
            write_result(result_pointer, self.result)
        return self.returns


def make_planner(body=None, version=1, exports=swp._EXPORTS):
    library = FakeLibrary(body or Recorder(), version, exports)
    with mock.patch.object(swp.ctypes, "CDLL", return_value=library) as loader:
        planner = swp.SelectedPrimaryPlanner(LIB_PATH)
    loader.assert_called_once_with(LIB_PATH)
    return planner, library


def request(**overrides):
    values = dict(start_ms=120000, end_ms=240000, fed_start_ms=60000, preroll_bars=3,
                  input_tf="1", script_tf="60")
    values.update(overrides)
    return values


def bar_array(timestamps, cls=swp.BarC):
    rows = (cls * len(timestamps))()
    for index, stamp in enumerate(timestamps):
        rows[index].timestamp = stamp
    return rows


class BridgeCase(unittest.TestCase):
    def assert_bridge_error(self, code, call, *args, **kwargs):
        with self.assertRaises(swp.PlanBridgeError) as caught:
            call(*args, **kwargs)
        error = caught.exception
        self.assertEqual(error.code, code)
        self.assertTrue(error.message)
        self.assertEqual(str(error), error.message)
        expected_args = CAPABILITY_ARGS if code == "window_mode_unsupported" else {}
        self.assertEqual(error.code_args, expected_args)
        return error

    def assert_internal(self, call, *args, **kwargs):
        return self.assert_bridge_error("harness_internal_error", call, *args, **kwargs)


class LayoutTests(BridgeCase):
    def test_sizes_and_offsets_are_the_frozen_lp64_layout(self):
        self.assertEqual(ctypes.sizeof(ctypes.c_void_p), 8)
        for struct, size, layout in ((swp.BarC, 48, BAR_LAYOUT),
                                     (swp.RequestC, 80, REQUEST_LAYOUT),
                                     (swp.ResultC, 248, RESULT_LAYOUT)):
            with self.subTest(struct=struct.__name__):
                self.assertEqual(ctypes.sizeof(struct), size)
                self.assertEqual(ctypes.alignment(struct), 8)
                self.assertEqual(tuple((name, getattr(struct, name).offset)
                                       for name, _ in struct._fields_), layout)
        self.assertEqual(swp.ResultC.option.size, 32)

    def test_field_types(self):
        self.assertEqual(dict(swp.RequestC._fields_)["feed_tolerant"], ctypes.c_uint32)
        self.assertEqual(dict(swp.RequestC._fields_)["input_tf"], ctypes.c_char_p)
        self.assertEqual(dict(swp.ResultC._fields_)["status"], ctypes.c_int32)
        self.assertEqual(dict(swp.ResultC._fields_)["supplied_input_bars"], ctypes.c_uint64)
        self.assertEqual(dict(swp.ResultC._fields_)["trim_start_ms"], ctypes.c_int64)
        self.assertEqual(dict(swp.ResultC._fields_)["option"], ctypes.c_char * 32)
        self.assertEqual(dict(swp.BarC._fields_)["timestamp"], ctypes.c_int64)

    def test_constructor_refuses_a_layout_that_is_not_the_frozen_one(self):
        wrong = ((swp.RequestC, 79, 8, swp._LAYOUTS[1][3]),)
        with mock.patch.object(swp, "_LAYOUTS", wrong), \
                mock.patch.object(swp.ctypes, "CDLL") as loader:
            self.assert_internal(swp.SelectedPrimaryPlanner, LIB_PATH)
        loader.assert_not_called()


class ConstructorTests(BridgeCase):
    def test_probes_the_version_and_binds_argtypes_and_restypes(self):
        planner, library = make_planner()
        version, plan = library.pf_selected_plan_version, library.pf_plan_selected_primary_v1
        self.assertEqual(version.argtypes, [])
        self.assertIs(version.restype, ctypes.c_uint32)
        self.assertEqual(plan.argtypes, [ctypes.POINTER(swp.RequestC),
                                         ctypes.POINTER(swp.BarC), ctypes.c_uint64,
                                         ctypes.POINTER(swp.ResultC)])
        self.assertIs(plan.restype, ctypes.c_int)
        self.assertEqual(version.calls, 1)
        self.assertEqual(plan.calls, 0)
        self.assertEqual(planner.library_path, LIB_PATH)

    def test_loads_the_one_explicit_absolute_path(self):
        library = FakeLibrary(Recorder())
        with mock.patch.object(swp.ctypes, "CDLL", return_value=library) as loader:
            swp.SelectedPrimaryPlanner(Path(LIB_PATH))
        loader.assert_called_once_with(LIB_PATH)

    def test_no_path_search_default_or_relative_path(self):
        self.assertRaises(TypeError, swp.SelectedPrimaryPlanner)
        for bad in (None, "", "libpineforge_window_plan.so", "lib/libx.so", "./libx.so",
                    b"/abs/libx.so", 7, LIB_PATH + "\0"):
            with self.subTest(path=bad), mock.patch.object(swp.ctypes, "CDLL") as loader:
                self.assert_internal(swp.SelectedPrimaryPlanner, bad)
                loader.assert_not_called()

    def test_a_missing_or_unloadable_library_is_window_mode_unsupported(self):
        with mock.patch.object(swp.ctypes, "CDLL", side_effect=OSError("no such file")):
            self.assert_bridge_error("window_mode_unsupported",
                                     swp.SelectedPrimaryPlanner, LIB_PATH)

    def test_a_missing_export_is_window_mode_unsupported(self):
        for exports in (("pf_selected_plan_version",), ("pf_plan_selected_primary_v1",), ()):
            with self.subTest(exports=exports):
                library = FakeLibrary(Recorder(), exports=exports)
                with mock.patch.object(swp.ctypes, "CDLL", return_value=library):
                    self.assert_bridge_error("window_mode_unsupported",
                                             swp.SelectedPrimaryPlanner, LIB_PATH)

    def test_a_version_other_than_1_is_window_mode_unsupported(self):
        for version in (0, 2, 7, 0xFFFFFFFF):
            with self.subTest(version=version):
                recorder = Recorder()
                library = FakeLibrary(recorder, version=version)
                with mock.patch.object(swp.ctypes, "CDLL", return_value=library):
                    self.assert_bridge_error("window_mode_unsupported",
                                             swp.SelectedPrimaryPlanner, LIB_PATH)
                self.assertEqual(recorder.seen, [])

    def test_module_has_no_library_search_or_environment_fallback(self):
        source = inspect.getsource(swp)
        for token in ("find_library", "environ", "getenv", "glob", "LD_LIBRARY_PATH"):
            self.assertNotIn(token, source)


class CallTests(BridgeCase):
    def test_the_request_and_the_result_are_preset_and_the_strings_alive(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        planner.plan(bar_array([60000]), 1, **request(
            chart_timezone="Asia/Taipei", engine_timezone="UTC", session="0930-1600",
            feed_tolerant=False))
        seen = recorder.seen[0]["request"]
        self.assertEqual(seen["struct_size"], 80)
        self.assertEqual(seen["version"], 1)
        self.assertEqual((seen["start_ms"], seen["end_ms"], seen["fed_start_ms"]),
                         (120000, 240000, 60000))
        self.assertEqual((seen["preroll_bars"], seen["feed_tolerant"]), (3, 0))
        self.assertEqual((seen["input_tf"], seen["script_tf"]), (b"1", b"60"))
        self.assertEqual((seen["chart_timezone"], seen["engine_timezone"], seen["session"]),
                         (b"Asia/Taipei", b"UTC", b"0930-1600"))
        before = recorder.seen[0]["result_before"]
        header = swp.ResultC.from_buffer_copy(before)
        self.assertEqual((header.struct_size, header.version), (248, 1))
        # status is the -1 sentinel set after size and version; no other byte is preset
        self.assertEqual(header.status, -1)
        self.assertEqual(before[12:], bytes(236))

    def test_feed_tolerant_defaults_to_true_and_is_0_or_1(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        planner.plan(None, 0, **request())
        planner.plan(None, 0, **request(feed_tolerant=True))
        planner.plan(None, 0, **request(feed_tolerant=False))
        self.assertEqual([call["request"]["feed_tolerant"] for call in recorder.seen],
                         [1, 1, 0])

    def test_empty_strings_are_nonnull_and_utf8_is_encoded(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        planner.plan(None, 0, **request(input_tf="", script_tf=""))
        planner.plan(None, 0, **request(chart_timezone="Zürich", session="é"))
        first, second = (call["request"] for call in recorder.seen)
        self.assertEqual((first["input_tf"], first["script_tf"], first["chart_timezone"],
                          first["engine_timezone"], first["session"]), (b"",) * 5)
        self.assertEqual(second["chart_timezone"], "Zürich".encode("utf-8"))
        self.assertEqual(second["session"], b"\xc3\xa9")

    def test_each_call_gets_fresh_structs_and_a_fresh_dictionary(self):
        recorder = Recorder(result=full_result(0xFF))
        planner, _ = make_planner(recorder)
        first = planner.plan(None, 0, **request())
        second = planner.plan(None, 0, **request())
        self.assertIsNot(first, second)
        first["status"] = 99
        self.assertEqual(second["status"], 0)
        before = [call["result_before"] for call in recorder.seen]
        self.assertEqual(before[0], before[1])
        self.assertNotEqual(recorder.seen[0]["request_address"], 0)

    def test_rows_are_read_in_place_not_copied(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        rows = bar_array([60000, 120000, 180000])
        planner.plan(rows, 3, **request())
        seen = recorder.seen[0]
        self.assertEqual(seen["rows_address"], ctypes.addressof(rows))
        self.assertEqual(seen["count"], 3)
        self.assertEqual(seen["timestamps"], [60000, 120000, 180000])

    def test_a_foreign_array_of_the_documented_layout_is_accepted(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        rows = bar_array([60000, 120000], cls=ForeignBar)
        planner.plan(rows, 2, **request())
        self.assertEqual(recorder.seen[0]["rows_address"], ctypes.addressof(rows))
        self.assertEqual(recorder.seen[0]["timestamps"], [60000, 120000])

    def test_a_pointer_with_an_explicit_count_is_accepted(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        rows = bar_array([60000, 120000, 180000], cls=ForeignBar)
        planner.plan(ctypes.cast(rows, ctypes.POINTER(ForeignBar)), 3, **request())
        planner.plan(ctypes.cast(rows, ctypes.POINTER(swp.BarC)), 2, **request())
        self.assertEqual([call["timestamps"] for call in recorder.seen],
                         [[60000, 120000, 180000], [60000, 120000]])
        self.assertTrue(all(call["rows_address"] == ctypes.addressof(rows)
                            for call in recorder.seen))

    def test_the_count_is_explicit_and_may_be_less_than_the_array(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        rows = bar_array([60000, 120000, 180000])
        planner.plan(rows, 2, **request())
        planner.plan(rows, 0, **request())
        self.assertEqual([call["count"] for call in recorder.seen], [2, 0])
        self.assert_internal(planner.plan, rows, 4, **request())
        self.assertEqual(len(recorder.seen), 2)

    def test_null_rows_are_accepted_only_with_count_zero(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        planner.plan(None, 0, **request())
        planner.plan(ctypes.POINTER(swp.BarC)(), 0, **request())
        planner.plan((swp.BarC * 0)(), 0, **request())
        self.assertEqual([call["count"] for call in recorder.seen], [0, 0, 0])
        self.assertIsNone(recorder.seen[0]["rows"])
        self.assertIsNone(recorder.seen[1]["rows_address"])
        self.assertIsNotNone(recorder.seen[2]["rows_address"])
        self.assert_internal(planner.plan, None, 1, **request())
        self.assert_internal(planner.plan, ctypes.POINTER(swp.BarC)(), 2, **request())
        self.assertEqual(len(recorder.seen), 3)

    def test_rows_that_are_not_the_documented_layout_are_refused_before_c(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        for rows in ([(60000, 1.0, 1.0, 1.0, 1.0, 1.0)], b"\0" * 48, (ctypes.c_double * 6)(),
                     bar_array([60000], cls=ReorderedBar), bar_array([60000], cls=ShortBar),
                     swp.BarC(), ctypes.c_void_p(0), "rows", 7):
            with self.subTest(rows=type(rows).__name__):
                self.assert_internal(planner.plan, rows, 1, **request())
        self.assertEqual(recorder.seen, [])

    def test_scalars_are_checked_before_c(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        cases = {
            "start_ms": (1.0, "1", None, True, 2 ** 63, -2 ** 63 - 1),
            "end_ms": (1.0, None, True, 2 ** 63),
            "fed_start_ms": (None, True, -2 ** 63 - 1),
            "preroll_bars": (-1, 2 ** 32, 1.5, None, True),
            "feed_tolerant": (1, 0, "yes", None),
        }
        for name, values in cases.items():
            for value in values:
                with self.subTest(name=name, value=value):
                    self.assert_internal(planner.plan, None, 0, **request(**{name: value}))
        for count in (-1, 1.0, None, True, 2 ** 64):
            with self.subTest(count=count):
                self.assert_internal(planner.plan, None, count, **request())
        self.assertEqual(recorder.seen, [])

    def test_scalar_edges_and_planner_domain_values_pass_through_to_c(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        planner.plan(None, 0, **request(start_ms=-2 ** 63, end_ms=2 ** 63 - 1,
                                        fed_start_ms=2 ** 62, preroll_bars=2 ** 32 - 1))
        planner.plan(None, 0, **request(start_ms=0, end_ms=0, fed_start_ms=0,
                                        preroll_bars=0))
        first, second = (call["request"] for call in recorder.seen)
        self.assertEqual((first["start_ms"], first["end_ms"], first["fed_start_ms"],
                          first["preroll_bars"]), (-2 ** 63, 2 ** 63 - 1, 2 ** 62, 2 ** 32 - 1))
        self.assertEqual((second["start_ms"], second["preroll_bars"]), (0, 0))

    def test_strings_are_checked_before_c(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        for name in ("input_tf", "script_tf", "chart_timezone", "engine_timezone", "session"):
            for value in ("a\0b", "\0", "\ud800", None, b"1", 5):
                with self.subTest(name=name, value=value):
                    self.assert_internal(planner.plan, None, 0, **request(**{name: value}))
        self.assertEqual(recorder.seen, [])

    def test_plan_takes_no_path_and_has_the_frozen_signature(self):
        parameters = inspect.signature(swp.SelectedPrimaryPlanner.plan).parameters
        self.assertEqual(list(parameters), [
            "self", "bars", "count", "start_ms", "end_ms", "fed_start_ms", "preroll_bars",
            "input_tf", "script_tf", "chart_timezone", "engine_timezone", "session",
            "feed_tolerant"])
        for name in list(parameters)[3:]:
            self.assertEqual(parameters[name].kind, inspect.Parameter.KEYWORD_ONLY)
        self.assertEqual({name: parameters[name].default for name in
                          ("chart_timezone", "engine_timezone", "session", "feed_tolerant")},
                         {"chart_timezone": "", "engine_timezone": "", "session": "",
                          "feed_tolerant": True})
        for name in ("start_ms", "end_ms", "fed_start_ms", "preroll_bars", "input_tf",
                     "script_tf"):
            self.assertIs(parameters[name].default, inspect.Parameter.empty)
        planner, _ = make_planner()
        for extra in ("feed_path", "ohlcv", "so", "library_path", "strategy_path"):
            with self.subTest(extra=extra):
                self.assertRaises(TypeError, planner.plan, None, 0, **request(**{extra: "/x"}))
        self.assertRaises(TypeError, planner.plan, None, 0, 1, **request())

    def test_the_strings_stay_alive_while_the_native_call_runs(self):
        recorder = Recorder()
        planner, _ = make_planner(recorder)
        rows = bar_array([60000])
        planner.plan(rows, 1, **request(input_tf="".join(["1", "5"]),
                                        script_tf="".join(["24", "0"]),
                                        chart_timezone="".join(["Europe/", "London"])))
        seen = recorder.seen[0]["request"]
        self.assertEqual((seen["input_tf"], seen["script_tf"], seen["chart_timezone"]),
                         (b"15", b"240", b"Europe/London"))
        planner.plan(bar_array([60000]), 1, **request())  # a row object only the call holds
        self.assertEqual(recorder.seen[1]["timestamps"], [60000])


class ResultTests(BridgeCase):
    def test_the_result_maps_to_the_cpp_field_names(self):
        planner, _ = make_planner(Recorder(result=full_result(0b10101010)))
        plan = planner.plan(None, 0, **request())
        self.assertEqual(tuple(plan), PLAN_KEYS)
        self.assertEqual(tuple(plan), swp.PLAN_FIELDS)
        self.assertEqual((plan["status"], plan["option"], plan["bound"]), (0, "start_ms", 0))
        self.assertEqual((plan["value_ms"], plan["previous_boundary_ms"],
                          plan["next_boundary_ms"], plan["trim_start_ms"]), (7, -8, 9, 10))
        for index, name in enumerate(COUNTS):
            self.assertEqual(plan[name], 100 + index)
        for bit, name in enumerate(OPTIONALS):
            self.assertEqual(plan[name], 1000 + bit if (0b10101010 >> bit) & 1 else None)
        self.assertIs(plan["shortfall"], True)
        self.assertIs(plan["complete_pending_preroll_at_horizon"], True)
        for name in PLAN_KEYS:
            if name in ("option",):
                self.assertIs(type(plan[name]), str)
            elif name in ("shortfall", "complete_pending_preroll_at_horizon"):
                self.assertIs(type(plan[name]), bool)
            elif plan[name] is not None:
                self.assertIs(type(plan[name]), int)

    def test_every_present_mask_maps_present_and_absent_timestamps(self):
        for mask in range(256):
            planner, _ = make_planner(Recorder(result=full_result(mask)))
            plan = planner.plan(None, 0, **request())
            for bit, name in enumerate(OPTIONALS):
                present = (mask >> bit) & 1
                self.assertEqual(plan[name], 1000 + bit if present else None,
                                 (mask, name))

    def test_a_present_timestamp_may_be_zero_or_negative(self):
        result = canned_result(present_mask=0b101, preroll_first_bar_ms=0,
                               supplied_first_data_ms=-60000)
        plan = make_planner(Recorder(result=result))[0].plan(None, 0, **request())
        self.assertEqual((plan["preroll_first_bar_ms"], plan["preroll_last_bar_ms"],
                          plan["supplied_first_data_ms"]), (0, None, -60000))

    def test_a_planner_refusal_is_returned_with_its_integer_status(self):
        for status in range(1, 7):
            with self.subTest(status=status):
                result = canned_result(option=b"end_ms", status=status, bound=1,
                                       value_ms=240000, previous_boundary_ms=180000,
                                       next_boundary_ms=300000)
                plan = make_planner(Recorder(result=result))[0].plan(None, 0, **request())
                self.assertIs(type(plan["status"]), int)
                self.assertEqual((plan["status"], plan["option"], plan["bound"]),
                                 (status, "end_ms", 1))
                self.assertEqual((plan["value_ms"], plan["previous_boundary_ms"],
                                  plan["next_boundary_ms"]), (240000, 180000, 300000))
                self.assertEqual(plan["supplied_input_bars"], 0)
                self.assertEqual(len(swp.STATUS_NAMES), 7)

    def test_planner_values_pass_through_without_python_count_logic(self):
        result = canned_result(supplied_input_bars=5, trim_index=9, fed_input_bars=1,
                               supplied_script_bars=2, available_script_bars=8,
                               window_script_bars=3, used_script_bars=11, bound=2)
        plan = make_planner(Recorder(result=result))[0].plan(None, 0, **request())
        self.assertEqual((plan["supplied_input_bars"], plan["trim_index"],
                          plan["fed_input_bars"], plan["available_script_bars"],
                          plan["window_script_bars"], plan["used_script_bars"]),
                         (5, 9, 1, 8, 3, 11))

    def test_an_option_of_31_characters_is_accepted(self):
        text = b"x" * 31
        plan = make_planner(Recorder(result=canned_result(option=text)))[0].plan(
            None, 0, **request())
        self.assertEqual(plan["option"], "x" * 31)

    def test_malformed_results_are_harness_internal_errors(self):
        no_nul = canned_result()
        put_raw_option(no_nul, b"a" * 32)
        dirty_padding = canned_result()
        put_raw_option(dirty_padding, b"ab\0cd" + b"\0" * 27)
        non_ascii = canned_result()
        put_raw_option(non_ascii, b"caf\xc3\xa9" + b"\0" * 26)
        cases = {
            "size low": canned_result(struct_size=247),
            "size zero": canned_result(struct_size=0),
            "size high": canned_result(struct_size=249),
            "version 0": canned_result(version=0),
            "version 2": canned_result(version=2),
            "status 7": canned_result(status=7),
            "status -1": canned_result(status=-1),
            "bound -2": canned_result(bound=-2),
            "bound 3": canned_result(bound=3),
            "mask bit 8": canned_result(present_mask=0x100),
            "mask all": canned_result(present_mask=0xFFFFFFFF),
            "shortfall 2": canned_result(shortfall=2),
            "horizon 2": canned_result(complete_pending_preroll_at_horizon=2),
            "horizon high": canned_result(complete_pending_preroll_at_horizon=0xFFFFFFFF),
            "reserved": canned_result(reserved=1),
            "option unterminated": no_nul,
            "option padding": dirty_padding,
            "option not ascii": non_ascii,
        }
        for bit, name in enumerate(OPTIONALS):
            cases[f"absent {name} set"] = canned_result(
                present_mask=0xFF & ~(1 << bit), **{name: 5})
            cases[f"absent {name} nonzero with no mask"] = canned_result(**{name: -1})
        for label, result in cases.items():
            with self.subTest(label):
                planner, _ = make_planner(Recorder(result=result))
                self.assert_internal(planner.plan, None, 0, **request())


class ReturnCodeTests(BridgeCase):
    def test_minus_2_is_out_of_memory(self):
        # Untouched output still holds the preset, sentinel included: unchanged.
        planner, _ = make_planner(Recorder(returns=-2, write=False))
        self.assert_bridge_error("out_of_memory", planner.plan, None, 0, **request())

    def test_minus_1_minus_3_and_unknown_returns_are_internal_errors(self):
        for code in (-1, -3, -4, -100, 1, 2, 7):
            with self.subTest(code=code):
                planner, _ = make_planner(Recorder(returns=code, write=False))
                error = self.assert_internal(planner.plan, None, 0, **request())
                self.assertIn(str(code), error.message)

    def test_a_negative_return_that_changed_the_result_is_an_internal_error(self):
        for code in (-1, -2, -3):
            with self.subTest(code=code):
                planner, _ = make_planner(Recorder(result=full_result(0xFF), returns=code))
                self.assert_internal(planner.plan, None, 0, **request())

    def test_the_negative_return_baseline_is_taken_after_the_sentinel(self):
        # canned_result(bound=0) is byte for byte the preset as it was before the
        # sentinel (size, version, zeros elsewhere). A bridge that writes it back on
        # a negative return changed the caller's output; a baseline taken before the
        # sentinel would call it unchanged and report out_of_memory for -2.
        old_preset = swp.ResultC(struct_size=248, version=1)
        written_back = canned_result(bound=0)
        self.assertEqual(bytes(written_back), bytes(old_preset))
        for code in (-1, -2, -3):
            with self.subTest(code=code):
                planner, _ = make_planner(Recorder(result=written_back, returns=code))
                error = self.assert_internal(planner.plan, None, 0, **request())
                self.assertIn("still changed", error.message)

    def test_return_0_that_wrote_no_result_is_an_internal_error(self):
        # The bridge says "a whole result was written" and wrote nothing: the preset
        # status -1 survives, is no planner status, and the call is refused. Without
        # the sentinel the untouched zeros would read as a valid all-zero Ok plan.
        for rows, count in ((None, 0), (bar_array([60000, 120000]), 2)):
            with self.subTest(count=count):
                recorder = Recorder(returns=0, write=False)
                planner, _ = make_planner(recorder)
                error = self.assert_internal(planner.plan, rows, count, **request())
                self.assertIn("status -1", error.message)
                self.assertEqual(len(recorder.seen), 1)
                preset = swp.ResultC.from_buffer_copy(recorder.seen[0]["result_before"])
                self.assertEqual((preset.struct_size, preset.version, preset.status),
                                 (248, 1, -1))

    def test_a_non_int_return_is_an_internal_error(self):
        for code in (None, True, False, 0.0, "0", b"\0"):
            with self.subTest(code=code):
                planner, _ = make_planner(Recorder(returns=code))
                self.assert_internal(planner.plan, None, 0, **request())

    def test_a_refused_argument_type_is_an_internal_error(self):
        def refuse(*args):
            raise ctypes.ArgumentError("argument 1: wrong type")
        planner, _ = make_planner(refuse)
        self.assert_internal(planner.plan, None, 0, **request())


class ErrorTests(unittest.TestCase):
    def test_plan_bridge_error_carries_code_message_and_code_args(self):
        arguments = {"capability": "x"}
        error = swp.PlanBridgeError("some_code", "some text", arguments)
        self.assertIsInstance(error, Exception)
        self.assertEqual((error.code, error.message, error.code_args),
                         ("some_code", "some text", {"capability": "x"}))
        self.assertIsNot(error.code_args, arguments)
        self.assertEqual(str(error), "some text")
        self.assertEqual(swp.PlanBridgeError("c", "m").code_args, {})
        self.assertEqual((swp.CODE_WINDOW_MODE_UNSUPPORTED, swp.CODE_OUT_OF_MEMORY,
                          swp.CODE_HARNESS_INTERNAL_ERROR, swp.CAPABILITY,
                          swp.PLAN_ABI_VERSION),
                         ("window_mode_unsupported", "out_of_memory",
                          "harness_internal_error", "selected_window_planner_v1", 1))


if __name__ == "__main__":
    unittest.main()
