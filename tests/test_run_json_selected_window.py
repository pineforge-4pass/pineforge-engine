"""Tests of the single-run selected-window orchestration of docker/run_json.py and of
docker/selected_window_report.py.

The orchestration is tested on fakes. The strategy library is a Python
stand-in (FakeLib) with the frozen export names, the planner is a stand-in object, and the
phase transport is a real AF_UNIX socketpair driven by the real RunPhaseWriter and the real
ExecutionObserverBinding (the fake engine calls the registered callback through its raw C
function pointer). The rows cover the order of work, what is admitted or refused before any feed,
library, planner or phase record exists, the trim / hash / R mapping, the comparison of native
counts with the plan, cleanup, and that an ordinary run is unchanged. The native engine itself
(the planner library, the selected-window ABI, the observer inside run_backtest_full, the report
of the selected generation and the socket boundaries) is not exercised here, and no fake result
stands in for it.

Poison counters: the entries a refused request must never reach (load_bars, load_symbol_feeds,
load_strategy, create_strategy, the planner constructor, ctypes.CDLL, a phase advance) are
patched with mocks that count and fail; every refusal row asserts all counts are zero and
the positive row asserts that work begins.
"""
import base64
import collections
import contextlib
import ctypes
import hashlib
import io
import json
import socket
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import request_feed_inventory as rfi  # noqa: E402
import run_execution_observer as reo  # noqa: E402
import run_json  # noqa: E402
import run_phase_transport as rpt  # noqa: E402
import selected_window_plan as swp  # noqa: E402
import selected_window_report as swr  # noqa: E402

MINUTE = 60_000
F = 1_759_536_000_000            # 2025-10-04T00:00:00Z, minute aligned: the frozen feed start
T = F + 4 * MINUTE               # window start
E = F + 10 * MINUTE              # window end
N = 3                            # requested pre-roll script bars
STATE = 7
CAPITAL = 1_000_000.0            # the effective initial capital of a provenance without overrides
# The lookback record of a build with NO lookback producer, written out from the documented
# contract and never read back from the module under test: status "unknown", a null declared
# count and a null has_unbounded_state. Null means "not produced by this build": it is neither
# true nor false.
NOT_PRODUCED_LOOKBACK = {"declared_required_chart_bars": None, "status": "unknown",
                         "has_unbounded_state": None}
INVENTORY_CAPABILITY = {"capability": "selected_window_request_feed_inventory"}
INSIDE = [{"kind": "token", "timeframe": "1"}, {"kind": "token", "timeframe": "D"}]


# --- fixtures ---------------------------------------------------------------

def csv_rows(count, start=F):
    return [(start + i * MINUTE, 100.0 + i, 102.0 + i, 99.0 + i, 101.0 + i, 10.0 + i)
            for i in range(count)]


def write_ohlcv(path, rows):
    lines = ["timestamp,open,high,low,close,volume"]
    lines += [f"{ts},{o},{h},{l},{c},{v}" for ts, o, h, l, c, v in rows]
    path.write_text("\n".join(lines) + "\n")


def values_hash(rows):
    """The existing primary hash domain, written out from its definition."""
    digest = hashlib.sha256()
    digest.update(b"pineforge:ohlcv:barc-le:v1\0")
    for ts, o, h, l, c, v in rows:
        digest.update(struct.pack("<5dq", o, h, l, c, v, ts))
    return digest.hexdigest()


def full_plan(**over):
    """The planner's plan for a 10 row, one minute feed: 4 rows before T, 6 in the window,
    N=3 kept, one pre-T row cut. Every wire equality holds."""
    plan = {
        "status": 0, "option": "", "bound": -1, "value_ms": 0, "previous_boundary_ms": 0,
        "next_boundary_ms": 0, "supplied_input_bars": 10, "supplied_script_bars": 10,
        "available_script_bars": 4, "used_script_bars": 3, "trimmed_script_bars": 1,
        "trim_index": 1, "fed_input_bars": 9, "fed_script_bars": 9, "preroll_input_bars": 3,
        "window_input_bars": 6, "window_script_bars": 6, "trim_start_ms": F + MINUTE,
        "preroll_first_bar_ms": F + MINUTE, "preroll_last_bar_ms": F + 3 * MINUTE,
        "supplied_first_data_ms": F, "supplied_last_data_ms": F + 9 * MINUTE,
        "fed_first_data_ms": F + MINUTE, "fed_last_data_ms": F + 9 * MINUTE,
        "window_first_data_ms": T, "window_last_data_ms": F + 9 * MINUTE,
        "shortfall": False, "complete_pending_preroll_at_horizon": False}
    plan.update(over)
    return plan


def empty_window_plan():
    """A 4 row feed that ends before T: pre-roll only, no window row."""
    return full_plan(
        supplied_input_bars=4, supplied_script_bars=4, available_script_bars=4,
        used_script_bars=3, trimmed_script_bars=1, trim_index=1, fed_input_bars=3,
        fed_script_bars=3, preroll_input_bars=3, window_input_bars=0, window_script_bars=0,
        supplied_last_data_ms=F + 3 * MINUTE, fed_last_data_ms=F + 3 * MINUTE,
        window_first_data_ms=None, window_last_data_ms=None,
        complete_pending_preroll_at_horizon=True)


def request():
    return swr.WindowRequest(T, E, N, F, "1", "1", 60)


def expected_window(plan, hashes, *, applied=True):
    """R written out field by field from the wire tables, not produced by the code under test."""
    window_script = plan["window_script_bars"]
    return {
        "wire_version": 1, "policy": "selected-window/v1", "applied": applied,
        "window": {"start_ms": T, "end_ms": E, "first_data_ms": plan["window_first_data_ms"],
                   "last_data_ms": plan["window_last_data_ms"],
                   "coverage": "nonempty" if plan["window_input_bars"] else "empty"},
        "feed": {"requested_start_ms": F, "first_data_ms": plan["fed_first_data_ms"],
                 "last_data_ms": plan["fed_last_data_ms"],
                 "supplied_first_data_ms": plan["supplied_first_data_ms"],
                 "supplied_last_data_ms": plan["supplied_last_data_ms"],
                 "input_tf_seconds": 60, "script_tf": "1",
                 "source_bytes_sha256": hashes[0], "source_values_sha256": hashes[1],
                 "evaluated_source_values_sha256": hashes[2]},
        "calendar": {"timezone": "UTC", "session": None},
        "preroll": {"requested_script_bars": N,
                    "available_script_bars": plan["available_script_bars"],
                    "used_script_bars": plan["used_script_bars"],
                    "trimmed_script_bars": plan["trimmed_script_bars"],
                    "trim_start_ms": plan["trim_start_ms"],
                    "trimmed_input_bars": plan["trim_index"],
                    "first_bar_ms": plan["preroll_first_bar_ms"],
                    "last_bar_ms": plan["preroll_last_bar_ms"], "shortfall": False},
        "lookback": dict(NOT_PRODUCED_LOOKBACK),
        "counts": {"supplied_input_bars": plan["supplied_input_bars"],
                   "supplied_script_bars": plan["supplied_script_bars"],
                   "fed_input_bars": plan["fed_input_bars"],
                   "fed_script_bars": plan["fed_script_bars"],
                   "preroll_input_bars": plan["preroll_input_bars"],
                   "window_input_bars": plan["window_input_bars"],
                   "window_script_bars": window_script,
                   "equity_points": window_script + 1 if applied else 0,
                   "anchor_points": 1 if applied else 0},
        "metering": {"billable_input_bars": plan["window_input_bars"],
                     "preroll_billable_input_bars": 0,
                     "basis": "window-primary-input-bars/v1"},
        "indices": {"fed_script_index_of_window_first":
                    plan["used_script_bars"] if window_script else None,
                    "supplied_script_index_of_window_first":
                    plan["available_script_bars"] if window_script else None},
    }


def selected_argv(so, ohlcv, inventory, *, script_tf="1", input_tf="1", extra=()):
    argv = ["--so", str(so), "--ohlcv", str(ohlcv), "--input-tf", input_tf,
            "--script-tf", script_tf, "--report-policy", "selected-window/v1",
            "--window-start-ms", str(T), "--window-end-ms", str(E),
            "--preroll-bars", str(N), "--fed-start-ms", str(F)]
    if inventory is not None:
        argv += ["--request-feed-inventory", str(inventory)]
    return argv + list(extra)


def write_inventory(directory, entries, *, chart=None, artifact_bytes=b"not a library\n",
                    artifact_sha=None):
    artifact = Path(directory) / "strategy.so"
    artifact.write_bytes(artifact_bytes)
    doc = {"schema": rfi.SCHEMA, "source_sha256": "0" * 64,
           "artifact_sha256": artifact_sha or hashlib.sha256(artifact_bytes).hexdigest(),
           "primary_chart_timeframe": chart, "entries": entries}
    inventory = Path(directory) / "inventory.json"
    inventory.write_text(json.dumps(doc))
    return artifact, inventory


UNRESOLVED_RESOLUTION = {"initial_capital": {
    "status": "unresolved", "reason": "unsupported_default", "raw_default": "capital_expr()"}}


def unresolved_capital_normalizer(seen=None):
    """A stand-in for run_json.normalize_release_provenance that leaves the initial capital the
    way the real one leaves a strategy() default it cannot certify (its legacy branch): null in
    provenance["strategy"], named in provenance["strategy_resolution"], nothing else changed.
    `seen`, when given, receives a JSON snapshot of the provenance as the normalizer handed it
    on, so a test can show that nothing after it wrote to the provenance (no capital guessed
    from the native anchor, no key added).

    It is a stand-in: it does NOT stand for the real normalizer on a real unresolved source."""
    def normalize(provenance, cpp_text, receipt, checked):
        provenance["strategy"]["initial_capital"] = None
        provenance["strategy_resolution"] = json.loads(json.dumps(UNRESOLVED_RESOLUTION))
        if seen is not None:
            seen["normalized"] = json.loads(json.dumps(provenance))
        return provenance
    return normalize


class PoisonTripped(Exception):
    pass


class Stdout:
    """A stdout that records its writes and flushes in the same event list as the run."""

    def __init__(self, events):
        self.events = events
        self.parts = []

    def write(self, text):
        self.events.append("stdout.write")
        self.parts.append(text)
        return len(text)

    def flush(self):
        self.events.append("stdout.flush")

    def text(self):
        return "".join(self.parts)


class PartialStdout(Stdout):
    """A stdout that takes the first half of the document and then fails."""

    def write(self, text):
        self.events.append("stdout.write")
        self.parts.append(text[:len(text) // 2])
        raise OSError(28, "No space left on device")


def run_main(argv, events=None, stdout_class=Stdout):
    stdout = stdout_class(events if events is not None else [])
    err = io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(err):
        status = run_json.main(argv)
    return status, stdout.text(), err.getvalue()


def last_line(text):
    return json.loads(text.strip().splitlines()[-1])


class FakeLib:
    """A strategy library stand-in: the exports run_json calls, a native-like run that
    calls the registered observer callback through its raw C function pointer and fills a
    ReportC, and the selected-window and observer ABIs. Only unit-level behaviour."""

    def __init__(self, plan, events=None, *, call_observer=True, counts=None,
                 observation=None, selected_abi=True, observer_abi=True,
                 anchor_equity=CAPITAL, raise_before_observer=None, engine_failure=None):
        self.plan = plan
        self.events = events if events is not None else []
        self.calls = []
        self.call_observer = call_observer
        self.anchor_equity = anchor_equity
        self.raise_before_observer = raise_before_observer
        if engine_failure is not None:  # (code, text): what run_failure reads after the run
            code, text = engine_failure
            self.strategy_get_last_error = lambda state: text.encode()
            self.strategy_get_last_error_code = lambda state: code.encode()
            self.strategy_last_run_status = lambda state: 1
        self.observer_descriptor = None
        self.observer_rc = None
        self.receipt = None
        self.run_args = None
        self.window_config = "unset"
        self._curve = None
        self.native_counts = {
            "run_generation": 5, "attempt_serial": 9, "attempt_generation": 5,
            "fed_input_bars": plan["fed_input_bars"], "fed_script_bars": plan["fed_script_bars"],
            "preroll_input_bars": plan["preroll_input_bars"],
            "preroll_script_bars": plan["used_script_bars"],
            "window_input_bars": plan["window_input_bars"],
            "window_script_bars": plan["window_script_bars"]}
        self.native_counts.update(counts or {})
        self.observation = {"run_generation": 5, "attempt_serial": 9, "attempt_generation": 5}
        self.observation.update(observation or {})
        lib = self

        def fn(name, result=0):
            def call(*args):
                lib.calls.append((name,) + args)
                return result
            return call

        self.strategy_create = fn("strategy_create", STATE)
        self.strategy_free = fn("strategy_free")
        self.report_free = fn("report_free")
        self.strategy_set_input = fn("strategy_set_input")
        self.strategy_set_override = fn("strategy_set_override")
        self.run_backtest_full = self._run
        if selected_abi:
            def set_selected(state, pointer):
                if pointer:
                    c = pointer.contents
                    lib.window_config = (c.struct_size, c.version, c.start_ms, c.end_ms)
                    lib.calls.append(("set_selected_window", c.start_ms, c.end_ms))
                else:
                    lib.window_config = None
                    lib.calls.append(("set_selected_window", None))
                return 0

            def selected_counts(state, ref):
                lib.calls.append(("selected_window_counts",))
                for name, value in lib.native_counts.items():
                    setattr(ref._obj, name, value)
                return 0

            self.pf_selected_window_version = lambda: 1
            self.strategy_set_selected_window_v1 = set_selected
            self.strategy_selected_window_counts_v1 = selected_counts
        if observer_abi:
            def set_observer(state, pointer):
                lib.observer_descriptor = pointer.contents if pointer else None
                lib.calls.append(("set_observer", "descriptor" if pointer else None))
                return 0

            def observation_snapshot(state, ref):
                for name, value in lib.observation.items():
                    setattr(ref._obj, name, value)
                return 0

            self.pf_execution_observer_version = lambda: 1
            self.strategy_set_execution_observer_v1 = set_observer
            self.strategy_execution_observation_v1 = observation_snapshot

    def _run(self, state, rows, count, input_tf, script_tf, magnifier, samples, dist, ref):
        self.events.append("native.start")
        self.calls.append(("run_backtest_full", state, count, input_tf, script_tf))
        self.run_args = (rows, count)
        if self.raise_before_observer is not None:
            raise self.raise_before_observer
        if self.call_observer and self.observer_descriptor is not None:
            boundary = reo.BoundaryC(ctypes.sizeof(reo.BoundaryC), 1, 5, 9)
            receipt = reo.ReceiptC(ctypes.sizeof(reo.ReceiptC), 1, 5, 0xFFFFFFFF, 0, 0, 0)
            self.observer_rc = self.observer_descriptor.before_results(
                None, ctypes.byref(boundary), ctypes.byref(receipt))
            self.receipt = (receipt.frame_bytes, receipt.handed_bytes, receipt.export_requested)
            if self.observer_rc != 0:
                self.events.append("native.end")
                return
        report = ref._obj
        plan = self.plan
        points = plan["window_script_bars"] + 1
        self._curve = (run_json.EquityPointC * points)()
        for i in range(points):
            self._curve[i].time_ms = T if i == 0 else T + (i - 1) * MINUTE
            self._curve[i].equity = self.anchor_equity if i == 0 else CAPITAL + i
            self._curve[i].open_profit = 0.0
        report.equity_curve = ctypes.cast(self._curve, ctypes.POINTER(run_json.EquityPointC))
        report.equity_curve_len = points
        report.input_bars_processed = plan["fed_input_bars"]
        report.script_bars_processed = plan["fed_script_bars"]
        report.input_tf_seconds = 60
        report.script_tf_seconds = 60
        report.script_tf_ratio = 1
        self.events.append("native.end")


class FakePlanner:
    """Stands in for SelectedPrimaryPlanner: records its construction and calls, returns
    the configured plan as a fresh dictionary."""
    constructed = []
    calls = []
    result = None

    def __init__(self, library_path):
        FakePlanner.constructed.append(library_path)

    def plan(self, bars, count, **kwargs):
        FakePlanner.calls.append((bars, count, kwargs))
        return dict(FakePlanner.result)

    @classmethod
    def reset(cls, result):
        cls.constructed, cls.calls, cls.result = [], [], result


def phase_records(parent):
    parent.setblocking(False)
    data = b""
    try:
        while True:
            chunk = parent.recv(65536)
            if not chunk:
                break
            data += chunk
    except BlockingIOError:
        pass
    return [json.loads(line) for line in data.split(b"\n") if line]


def recording_writer(events, fail_phase=None):
    class Writer(rpt.RunPhaseWriter):
        def advance(self, phase):
            events.append("advance:" + phase)
            if phase == fail_phase:
                raise rpt.PhaseTransportError("injected", reason="write_failed", phase=phase)
            super().advance(phase)
    return Writer


class Counters:
    """The entries a refused request must never reach, patched to count and fail. With
    allow=("load_bars",) the first feed read is the expected, failing end of a positive row."""

    ENTRIES = ((run_json, "load_bars"), (run_json, "load_symbol_feeds"),
               (run_json, "load_strategy"), (run_json, "create_strategy"),
               (swp, "SelectedPrimaryPlanner"), (ctypes, "CDLL"))

    def __init__(self):
        self.stack = contextlib.ExitStack()
        self.mocks = {}

    def __enter__(self):
        for owner, name in self.ENTRIES:
            mocked = mock.Mock(side_effect=PoisonTripped(name))
            self.stack.enter_context(mock.patch.object(owner, name, mocked))
            self.mocks[name] = mocked
        original = rpt.RunPhaseWriter.advance
        self.advance = mock.Mock(side_effect=lambda writer, phase: original(writer, phase))
        self.stack.enter_context(mock.patch.object(
            rpt.RunPhaseWriter, "advance", lambda writer, phase: self.advance(writer, phase)))
        self.reads = mock.Mock()
        for name in ("_read_inventory_bytes", "_artifact_digest"):
            real = getattr(rfi, name)
            self.stack.enter_context(mock.patch.object(
                rfi, name, lambda *a, _real=real, **k: (self.reads(), _real(*a, **k))[1]))
        return self

    def __exit__(self, *exc):
        self.stack.close()
        return False

    def counts(self):
        counts = {name: m.call_count for name, m in self.mocks.items()}
        counts["advance"] = self.advance.call_count
        counts["inventory_reads"] = self.reads.call_count
        return counts


# --- request validation, unsupported options, planner refusals --------------

class ValidationTests(unittest.TestCase):
    GOOD = dict(policy="selected-window/v1", window_start_ms=str(T), window_end_ms=str(E),
                preroll_bars=str(N), fed_start_ms=str(F), trade_start_ms=None,
                input_tf="1", script_tf="60")

    def refuse(self, code, args, **over):
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.validate_request(**{**self.GOOD, **over})
        self.assertEqual((caught.exception.code, caught.exception.code_args), (code, args))

    def test_a_good_request_validates_with_the_canonical_primary(self):
        got = swr.validate_request(**self.GOOD)
        self.assertEqual(tuple(got), (T, E, N, F, "1", "60", 60))

    def test_missing_or_wrong_policy_and_malformed_integers(self):
        self.refuse("window_request_invalid", {"option": "report_policy"}, policy=None)
        self.refuse("window_request_invalid", {"option": "report_policy"}, policy="selected-window/v2")
        for option in ("window_start_ms", "window_end_ms", "preroll_bars", "fed_start_ms"):
            for bad in (None, "", "1.5", "1e3", "01", "+5", " 5", "abc", str(2 ** 53)):
                with self.subTest(option=option, bad=bad):
                    self.refuse("window_request_invalid", {"option": option}, **{option: bad})

    def test_explicit_timeframes_are_required_and_must_be_tokens(self):
        self.refuse("window_request_invalid", {"option": "input_tf"}, input_tf="")
        self.refuse("window_request_invalid", {"option": "script_tf"}, script_tf="")
        self.refuse("window_request_invalid", {"option": "script_tf"}, script_tf="4h")
        self.refuse("window_request_invalid", {"option": "input_tf"}, input_tf="1H")
        self.refuse("window_request_invalid", {"option": "input_tf"}, input_tf="1M")

    def test_legacy_flag_range_and_preroll_refusals(self):
        self.refuse("window_legacy_flag_conflict", {"option": "trade_start_ms"}, trade_start_ms=5)
        self.refuse("window_preroll_out_of_range", {"requested": 5001}, preroll_bars="5001")
        self.refuse("window_preroll_out_of_range", {"requested": -1}, preroll_bars="-1")
        self.refuse("window_range_invalid", {"start_ms": E, "end_ms": E, "fed_start_ms": F},
                    window_start_ms=str(E))
        self.refuse("window_range_invalid", {"start_ms": T, "end_ms": E, "fed_start_ms": T + 1},
                    fed_start_ms=str(T + 1))

    def test_zero_and_the_maximum_preroll_are_accepted(self):
        self.assertEqual(swr.validate_request(**{**self.GOOD, "preroll_bars": "0"}).preroll_bars, 0)
        self.assertEqual(swr.validate_request(**{**self.GOOD, "preroll_bars": "5000"}).preroll_bars, 5000)

    def test_timeframe_seconds_is_a_unit_conversion_only(self):
        self.assertEqual([swr.timeframe_seconds(t) for t in ("1", "60", "D", "W", "30S", "2D")],
                         [60, 3600, 86400, 604800, 30, 172800])
        self.assertIsNone(swr.timeframe_seconds("1M"))
        self.assertIsNone(swr.timeframe_seconds("1H"))


class UnsupportedOptionTests(unittest.TestCase):
    def refuse(self, code, args, **kw):
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.check_unsupported_options(
                **{"selected": True, "report_shape": None, "curve_point_budget": None,
                   "results_digest": None, **kw})
        self.assertEqual((caught.exception.code, caught.exception.code_args), (code, args))

    def test_full_shape_alone_is_accepted_for_a_selected_request(self):
        swr.check_unsupported_options(selected=True, report_shape="full/v1",
                                      curve_point_budget=None, results_digest=None)
        swr.check_unsupported_options(selected=False, report_shape=None,
                                      curve_point_budget=None, results_digest=None)

    def test_compact_budget_and_digest_fail_typed(self):
        self.refuse("report_shape_unsupported", {"shape": "compact/v1"},
                    report_shape="compact/v1")
        self.refuse("report_shape_request_invalid", {"option": "report_shape", "value": "tiny"},
                    report_shape="tiny")
        self.refuse("report_shape_request_invalid",
                    {"option": "curve_point_budget", "value": "2500"}, curve_point_budget="2500")
        self.refuse("results_digest_unsupported", {"requested": "sha256/v1"},
                    results_digest="sha256/v1")

    def test_a_legacy_request_names_no_shape_in_this_build(self):
        self.refuse("report_shape_unsupported", {"shape": "full/v1"}, selected=False,
                    report_shape="full/v1")


class PlanRefusalTests(unittest.TestCase):
    def refusal(self, **over):
        return swr.refusal_for_plan(full_plan(**over), request(), timezone="UTC", session=None)

    def check(self, error, code, args):
        self.assertEqual((error.code, error.code_args), (code, args))

    def test_ok_is_none(self):
        self.assertIsNone(self.refusal())

    def test_request_invalid_by_option(self):
        self.check(self.refusal(status=1, option="script_tf"), "window_request_invalid",
                   {"option": "script_tf"})
        self.check(self.refusal(status=1, option="input_tf"), "window_request_invalid",
                   {"option": "input_tf"})
        self.check(self.refusal(status=1, option="fed_start_ms"), "window_range_invalid",
                   {"start_ms": T, "end_ms": E, "fed_start_ms": F})
        self.check(self.refusal(status=1, option="preroll_bars"), "window_preroll_out_of_range",
                   {"requested": N})
        self.assertEqual(self.refusal(status=1, option="rows").code, "harness_internal_error")

    def test_calendar_boundary_range_rows_and_internal(self):
        self.check(self.refusal(status=2, option="timezone"), "window_calendar_unsupported",
                   {"timezone": "UTC", "session": None})
        self.check(self.refusal(status=3, bound=0, value_ms=T + 1, previous_boundary_ms=T,
                                next_boundary_ms=T + MINUTE),
                   "window_boundary_unaligned",
                   {"bound": "start", "value_ms": T + 1, "previous_boundary_ms": T,
                    "next_boundary_ms": T + MINUTE})
        self.check(self.refusal(status=3, bound=1, value_ms=5), "window_boundary_unaligned",
                   {"bound": "end", "value_ms": 5, "previous_boundary_ms": 0, "next_boundary_ms": 0})
        self.check(self.refusal(status=3, bound=2, value_ms=5), "window_boundary_unaligned",
                   {"bound": "fed_start", "value_ms": 5, "previous_boundary_ms": 0,
                    "next_boundary_ms": 0})
        self.assertEqual(self.refusal(status=3, bound=-1).code, "harness_internal_error")
        self.check(self.refusal(status=4, value_ms=E + 1), "window_feed_range_invalid",
                   {"time_ms": E + 1, "fed_start_ms": F, "end_ms": E})
        self.check(self.refusal(status=5, value_ms=9, previous_boundary_ms=9),
                   "chart_bars_rejected", {"field": "timestamp", "reason": "not_increasing"})
        self.assertEqual(self.refusal(status=6, option="calendar").code, "engine_invariant")
        self.assertEqual(self.refusal(status=99).code, "harness_internal_error")


# --- trim by pointer offset, hash, R, ABI, native counts --------------------

class TrimAndHashTests(unittest.TestCase):
    def feed(self, count):
        bars = (run_json.BarC * count)()
        for i, (ts, o, h, l, c, v) in enumerate(csv_rows(count)):
            bars[i].open, bars[i].high, bars[i].low = o, h, l
            bars[i].close, bars[i].volume, bars[i].timestamp = c, v, ts
        return bars

    def test_the_view_shares_the_allocation_and_starts_at_the_offset(self):
        bars = self.feed(5)
        view, count = swr.retained_rows(bars, 5, 2, 3, run_json.BarC)
        self.assertEqual(count, 3)
        self.assertEqual(ctypes.addressof(view),
                         ctypes.addressof(bars) + 2 * ctypes.sizeof(run_json.BarC))
        self.assertEqual(view[0].timestamp, bars[2].timestamp)
        bars[3].close = 777.0  # a write to the original shows in the view: no copy
        self.assertEqual(view[1].close, 777.0)

    def test_nothing_cut_returns_the_original_and_nothing_left_returns_count_zero(self):
        bars = self.feed(4)
        self.assertIs(swr.retained_rows(bars, 4, 0, 4, run_json.BarC)[0], bars)
        self.assertEqual(swr.retained_rows(bars, 4, 4, 0, run_json.BarC), (bars, 0))

    def test_a_plan_that_breaks_supplied_equals_trimmed_plus_fed_is_an_invariant(self):
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.retained_rows(self.feed(4), 4, 1, 4, run_json.BarC)
        self.assertEqual(caught.exception.code, "engine_invariant")

    def test_evaluated_hash_restarts_the_existing_domain_over_the_retained_rows(self):
        rows = csv_rows(6)
        bars = self.feed(6)
        hasher = run_json._new_source_feed_hasher()
        swr.update_rows_hash(hasher, bars, 2, 4, run_json.BarC, run_json._SOURCE_FEED_RECORD)
        self.assertEqual(hasher.hexdigest(), values_hash(rows[2:]))
        whole = run_json._new_source_feed_hasher()
        swr.update_rows_hash(whole, bars, 0, 6, run_json.BarC, run_json._SOURCE_FEED_RECORD)
        self.assertEqual(whole.hexdigest(), values_hash(rows))

    def test_the_row_by_row_fallback_gives_the_same_digest(self):
        rows = csv_rows(5)
        bars = self.feed(5)
        hasher = run_json._new_source_feed_hasher()
        with mock.patch.object(swr.sys, "byteorder", "big"):
            swr.update_rows_hash(hasher, bars, 1, 4, run_json.BarC, run_json._SOURCE_FEED_RECORD)
        self.assertEqual(hasher.hexdigest(), values_hash(rows[1:]))


class ReportWindowTests(unittest.TestCase):
    HASHES = ("a" * 64, "b" * 64, "c" * 64)

    def build(self, plan, *, applied=True):
        return swr.build_report_window(
            request(), plan, applied=applied,
            feed={"input_tf_seconds": 60, "source_bytes_sha256": self.HASHES[0],
                  "source_values_sha256": self.HASHES[1],
                  "evaluated_source_values_sha256": self.HASHES[2]},
            calendar=swr.calendar_record("", ""))

    def test_r_maps_every_field_of_the_wire_table(self):
        self.assertEqual(self.build(full_plan()), expected_window(full_plan(), self.HASHES))

    def test_validation_only_counts_are_plans_with_no_curve_points(self):
        got = self.build(full_plan(), applied=False)
        self.assertEqual(got, expected_window(full_plan(), self.HASHES, applied=False))
        self.assertEqual((got["counts"]["equity_points"], got["counts"]["anchor_points"]), (0, 0))

    def test_an_empty_window_is_valid_with_null_extents_and_indices(self):
        got = self.build(empty_window_plan())
        self.assertEqual(got, expected_window(empty_window_plan(), self.HASHES))
        self.assertEqual(got["window"]["coverage"], "empty")
        self.assertEqual(got["counts"]["equity_points"], 1)
        self.assertIsNone(got["indices"]["fed_script_index_of_window_first"])

    def test_shortfall_follows_available_less_than_requested_only(self):
        plan = full_plan(available_script_bars=2, used_script_bars=2, trimmed_script_bars=0,
                         supplied_script_bars=8, fed_script_bars=8, shortfall=True,
                         trim_index=0, supplied_input_bars=9, fed_input_bars=9,
                         preroll_input_bars=3)
        got = self.build(plan)
        self.assertTrue(got["preroll"]["shortfall"])
        self.assertEqual(got["preroll"]["requested_script_bars"], N)

    def test_the_counting_equalities_are_checked(self):
        for broken in ({"fed_input_bars": 8}, {"trim_index": 2}, {"fed_script_bars": 10},
                       {"used_script_bars": 2}, {"shortfall": True}, {"window_script_bars": 5}):
            with self.subTest(broken=broken), self.assertRaises(swr.SelectedWindowError) as caught:
                self.build(full_plan(**broken))
            self.assertEqual(caught.exception.code, "engine_invariant")

    def test_lookback_is_not_produced_by_this_build_and_states_nothing(self):
        # This build has no lookback producer, so R says status "unknown", a null
        # declared count and a null has_unbounded_state ("not produced by this build", neither
        # true nor false). The real builder, every plan shape, a run and validation-only alike.
        shortfall = full_plan(available_script_bars=2, used_script_bars=2, trimmed_script_bars=0,
                              supplied_script_bars=8, fed_script_bars=8, shortfall=True,
                              trim_index=0, supplied_input_bars=9, fed_input_bars=9,
                              preroll_input_bars=3)
        for name, plan in (("full", full_plan()), ("empty window", empty_window_plan()),
                           ("shortfall", shortfall)):
            for applied in (True, False):
                with self.subTest(plan=name, applied=applied):
                    look = self.build(plan, applied=applied)["lookback"]
                    self.assertEqual(look, NOT_PRODUCED_LOOKBACK)
                    self.assertEqual(list(look), ["declared_required_chart_bars", "status",
                                                  "has_unbounded_state"])
                    self.assertEqual(look["status"], "unknown")
                    self.assertIsNone(look["declared_required_chart_bars"])
                    self.assertIsNone(look["has_unbounded_state"])  # not True and not False

    def test_every_record_gets_its_own_lookback_object(self):
        first = self.build(full_plan())["lookback"]
        second = self.build(full_plan())["lookback"]
        self.assertEqual(first, second)
        self.assertIsNot(first, second)
        first["status"] = "changed after the fact"
        self.assertEqual(second, NOT_PRODUCED_LOOKBACK)
        self.assertEqual(self.build(full_plan())["lookback"], NOT_PRODUCED_LOOKBACK)

    def test_calendar_defaults_and_report_shape(self):
        self.assertEqual(swr.calendar_record("", ""), {"timezone": "UTC", "session": None})
        self.assertEqual(swr.calendar_record("America/New_York", "24x7"),
                         {"timezone": "America/New_York", "session": None})
        self.assertEqual(swr.calendar_record("", "0930-1600"),
                         {"timezone": "UTC", "session": "0930-1600"})
        self.assertEqual(swr.report_shape_full(7, 0), {
            "version": 1, "name": "full/v1", "curve_point_budget": None,
            "canonical_equity_points": 7, "emitted_equity_points": 7, "canonical_trades": 0,
            "emitted_trades": 0, "retained_point_indices": None, "curve_decimated": False})

    # Capitals the provenance does not establish: null (how an unresolved strategy() default is
    # recorded), text and values that are no finite number. _established_number reads none of
    # them as a capital, so the anchor equity has nothing to be compared with.
    UNESTABLISHED_CAPITALS = (None, "lots", "", " ", True, False, float("nan"), float("inf"),
                              float("-inf"), "inf", "nan", "1e999", 10 ** 400, [], {}, [CAPITAL])

    @staticmethod
    def selected_curve(anchor_equity=CAPITAL):
        return [{"time_ms": T, "equity": anchor_equity, "open_profit": 0.0},
                {"time_ms": T, "equity": CAPITAL + 1.0, "open_profit": 0.5}]

    def refused_curve(self, curve, bars, capital, fragment):
        """check_selected_curve refuses with engine_invariant, and with the message of the check
        that fires (so a row proves the intended check and not another one)."""
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.check_selected_curve(curve, T, bars, capital)
        self.assertEqual(caught.exception.code, "engine_invariant")
        self.assertIn(fragment, caught.exception.message)

    def test_the_selected_curve_is_checked_not_rewritten(self):
        curve = self.selected_curve()
        before = json.loads(json.dumps(curve))
        swr.check_selected_curve(curve, T, 1, CAPITAL)
        swr.check_selected_curve(curve, T, 1, "1000000")  # text that spells the capital
        swr.check_selected_curve(curve, T, 1, 1000000)    # an integer capital
        self.assertEqual(curve, before)                   # nothing added, dropped or rewritten
        self.assertEqual(set(curve[0]), {"time_ms", "equity", "open_profit"})
        wrong_anchor = [{**curve[0], "equity": CAPITAL - 1.0}, curve[1]]
        extra_field = [{**curve[0], "point_kind": "window_anchor"}, curve[1]]
        for name, bad, bars, capital, fragment in (
                ("length", curve, 2, CAPITAL, "points, the plan says"),
                ("open profit", curve[1:], 0, CAPITAL + 1.0,
                 "does not start with the window anchor"),
                ("anchor time", [{**curve[0], "time_ms": T - 1}], 0, CAPITAL,
                 "does not start with the window anchor"),
                ("a per-point field", extra_field, 1, CAPITAL, "does not have exactly"),
                ("known capital, unequal anchor", wrong_anchor, 1, CAPITAL,
                 "is not the initial capital"),
                ("known text capital, unequal anchor", wrong_anchor, 1, "1000000",
                 "is not the initial capital"),
                ("known integer capital, unequal anchor", wrong_anchor, 1, 1000000,
                 "is not the initial capital")):
            with self.subTest(name=name):
                self.refused_curve(bad, bars, capital, fragment)

    def test_no_established_capital_skips_only_the_comparison_with_the_anchor(self):
        # An unresolved initial-capital provenance is not evidence that the native anchor is
        # wrong, and the selected path has no refusal ordinary mode lacks: a missing,
        # unreadable, boolean or non-finite capital is never a refusal.
        curve = self.selected_curve()
        before = json.loads(json.dumps(curve))
        for capital in self.UNESTABLISHED_CAPITALS:
            with self.subTest(capital=repr(capital)[:40]):
                self.assertIsNone(swr._established_number(capital))
                self.assertIsNone(swr.check_selected_curve(curve, T, 1, capital))
                self.assertEqual(curve, before)   # no capital was written back or guessed
        # With nothing to compare, ANY finite anchor value completes, and the very anchor that a
        # known capital refuses completes (so the equality alone was skipped).
        for equity in (CAPITAL - 1.0, 250_000.0, 0.0, -0.0, -5.0, 3, 1e300):
            with self.subTest(anchor=equity):
                swr.check_selected_curve(self.selected_curve(equity), T, 1, None)
        self.refused_curve(self.selected_curve(CAPITAL - 1.0), 1, CAPITAL,
                           "is not the initial capital")
        # An established capital is compared even when it spells a number as text, and a capital
        # the provenance establishes as zero or negative is a capital like any other.
        swr.check_selected_curve(self.selected_curve(0.0), T, 1, "0")
        swr.check_selected_curve(self.selected_curve(-5.0), T, 1, -5.0)
        self.refused_curve(self.selected_curve(1.0), 1, "0", "is not the initial capital")

    def test_every_other_check_still_applies_without_an_established_capital(self):
        curve = self.selected_curve()
        extra_field = [{**curve[0], "point_kind": "window_anchor"}, curve[1]]
        for capital in (None, "lots", True, float("inf")):
            for name, bad, bars, fragment in (
                    ("length", curve, 2, "points, the plan says"),
                    ("open profit", curve[1:], 0, "does not start with the window anchor"),
                    ("anchor time", [{**curve[0], "time_ms": T - 1}], 0,
                     "does not start with the window anchor"),
                    ("a per-point field", extra_field, 1, "does not have exactly"),
                    ("a missing field", [{"time_ms": T, "open_profit": 0.0}, curve[1]], 1,
                     "does not have exactly")):
                with self.subTest(capital=repr(capital), name=name):
                    self.refused_curve(bad, bars, capital, fragment)

    def test_a_malformed_actual_anchor_is_refused_whatever_the_capital(self):
        # The anchor the engine produced must be a finite number (the report writes a non-finite
        # double as null): not null, not a bool, not text, not NaN or an infinity, and not an int
        # beyond binary64. An unresolved capital never excuses it, and neither does an equal-
        # looking value: True == 1.0 must not pass for a capital of 1.
        malformed = (None, float("nan"), float("inf"), float("-inf"), True, False, "1000000",
                     10 ** 400, [CAPITAL], {"equity": CAPITAL})
        for equity in malformed:
            for capital in (CAPITAL, "1000000", None, "lots", 1, 0):
                with self.subTest(anchor=repr(equity)[:30], capital=repr(capital)):
                    self.refused_curve(self.selected_curve(equity), 1, capital,
                                       "not a finite number")

    def test_the_three_placements_the_version_and_the_hashed_bytes_are_checked(self):
        window = expected_window(full_plan(), ("a" * 64, "b" * 64, "c" * 64))
        provenance = {"schema_version": 2, "runtime": {"report_window": window}}
        raw = json.dumps(provenance, separators=(",", ":")).encode()
        fingerprint = {"token": base64.b64encode(raw).decode(),
                       "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
                       "provenance": provenance, "version": 2}

        def report(**over):
            doc = {"report_window": json.loads(json.dumps(window)),
                   "applied_runtime": {"report_window": json.loads(json.dumps(window))},
                   "fingerprint": json.loads(json.dumps(fingerprint))}
            doc.update(over)
            return doc

        swr.check_report_placements(report(), window)  # equal content in all three places
        other = {**window, "applied": False}
        broken = (
            ("missing fingerprint", report(fingerprint=None)),
            ("report_window", report(report_window=other)),
            ("applied_runtime", report(applied_runtime={"report_window": other})),
            ("no applied_runtime", report(applied_runtime=None)),
            ("provenance runtime", report(fingerprint={
                **fingerprint, "provenance": {**provenance, "runtime": {"report_window": other}}})),
            ("version", report(fingerprint={**fingerprint, "version": 1})),
            ("schema_version", report(fingerprint={
                **fingerprint, "provenance": {**provenance, "schema_version": 1}})),
            ("digest", report(fingerprint={**fingerprint, "digest": "sha256:" + "0" * 64})),
            ("token", report(fingerprint={**fingerprint, "token": "not base64 !"})),
            ("hashed R", report(fingerprint=self.refingerprint(
                {"schema_version": 2, "runtime": {"report_window": other}}, window))),
            ("hashed schema", report(fingerprint=self.refingerprint(
                {"schema_version": 1, "runtime": {"report_window": window}}, window))),
        )
        for name, doc in broken:
            with self.subTest(name=name), self.assertRaises(swr.SelectedWindowError) as caught:
                swr.check_report_placements(doc, window)
            self.assertEqual(caught.exception.code, "engine_invariant")

    @staticmethod
    def refingerprint(hashed, visible_window):
        """A fingerprint whose visible provenance is right (it holds `visible_window`) and whose
        hashed bytes, with a matching digest, are `hashed`."""
        raw = json.dumps(hashed, separators=(",", ":")).encode()
        return {"token": base64.b64encode(raw).decode(),
                "digest": "sha256:" + hashlib.sha256(raw).hexdigest(), "version": 2,
                "provenance": {"schema_version": 2,
                               "runtime": {"report_window": visible_window}}}

    @staticmethod
    def window_with_lookback(lookback):
        """R as the builder writes it, with its lookback member replaced by `lookback`."""
        record = expected_window(full_plan(), ("a" * 64, "b" * 64, "c" * 64))
        record["lookback"] = lookback
        return record

    @staticmethod
    def report_carrying(record):
        """A report that carries `record` as R in all three places AND in the hashed bytes, with
        a matching digest, fingerprint version 2 and schema_version 2: every equality row of
        check_report_placements holds, so only a rule about R's own content can refuse it."""
        provenance = {"schema_version": 2, "runtime": {"report_window": record}}
        raw = json.dumps(provenance, separators=(",", ":")).encode()
        return {"report_window": json.loads(json.dumps(record)),
                "applied_runtime": {"report_window": json.loads(json.dumps(record))},
                "fingerprint": {"token": base64.b64encode(raw).decode(),
                                "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
                                "provenance": json.loads(json.dumps(provenance)),
                                "version": 2}}

    def test_the_documented_null_triple_passes_the_report_checker(self):
        record = self.window_with_lookback(dict(NOT_PRODUCED_LOOKBACK))
        self.assertIsNone(
            swr.check_report_placements(self.report_carrying(record), record))

    def test_a_null_has_unbounded_state_is_valid_only_beside_unknown_and_a_null_count(self):
        # A null beside any other status or beside a number is an
        # invalid report. Every row carries the malformed R in all three places and in the
        # hashed bytes, so the equalities hold and only the pairing rule can be what fires; the
        # message proves it was.
        null = None
        invalid = (
            ("null beside sufficient_declared",
             {"declared_required_chart_bars": null, "status": "sufficient_declared",
              "has_unbounded_state": null}),
            ("null beside insufficient",
             {"declared_required_chart_bars": null, "status": "insufficient",
              "has_unbounded_state": null}),
            ("null beside a number",
             {"declared_required_chart_bars": 4800, "status": "unknown",
              "has_unbounded_state": null}),
            ("null beside zero",
             {"declared_required_chart_bars": 0, "status": "unknown",
              "has_unbounded_state": null}),
            ("null beside another status AND a number",
             {"declared_required_chart_bars": 4800, "status": "sufficient_declared",
              "has_unbounded_state": null}),
            ("null beside a status spelled in other case",
             {"declared_required_chart_bars": null, "status": "UNKNOWN",
              "has_unbounded_state": null}),
            ("null beside a null status",
             {"declared_required_chart_bars": null, "status": null,
              "has_unbounded_state": null}),
            ("null beside an absent count (null is not omission)",
             {"status": "unknown", "has_unbounded_state": null}),
            ("null beside an absent status",
             {"declared_required_chart_bars": null, "has_unbounded_state": null}),
        )
        for name, lookback in invalid:
            record = self.window_with_lookback(lookback)
            with self.subTest(name=name), self.assertRaises(swr.SelectedWindowError) as caught:
                swr.check_report_placements(self.report_carrying(record), record)
            self.assertEqual(caught.exception.code, "engine_invariant")
            self.assertIn("null is valid only beside status 'unknown' and a null "
                          "declared_required_chart_bars", caught.exception.message)

    def test_the_report_checker_asks_nothing_more_of_lookback_than_the_null_pairing(self):
        # A build WITH a lookback producer (this one is not) emits a boolean beside any status
        # and count. The checker does not invent a schema for them.
        producer_shapes = (
            {"declared_required_chart_bars": 4800, "status": "sufficient_declared",
             "has_unbounded_state": False},
            {"declared_required_chart_bars": 120, "status": "insufficient",
             "has_unbounded_state": True},
        )
        for lookback in producer_shapes:
            record = self.window_with_lookback(lookback)
            with self.subTest(lookback=lookback):
                self.assertIsNone(
                    swr.check_report_placements(self.report_carrying(record), record))


class AbiAndNativeCountsTests(unittest.TestCase):
    def test_struct_sizes_are_the_frozen_ones(self):
        self.assertEqual((ctypes.sizeof(swr.SelectedWindowConfigC),
                          ctypes.sizeof(swr.SelectedWindowCountsC)), (24, 80))

    def test_a_missing_export_or_version_is_window_mode_unsupported(self):
        lib = FakeLib(full_plan(), selected_abi=False)
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.SelectedWindowAbi(lib)
        self.assertEqual((caught.exception.code, caught.exception.code_args),
                         ("window_mode_unsupported", {"capability": "selected_window_v1"}))
        lib = FakeLib(full_plan())
        lib.pf_selected_window_version = lambda: 2
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.SelectedWindowAbi(lib)
        self.assertEqual(caught.exception.code, "window_mode_unsupported")

    def test_configure_sends_the_window_and_maps_statuses(self):
        lib = FakeLib(full_plan())
        abi = swr.SelectedWindowAbi(lib)
        abi.configure(STATE, T, E)
        self.assertEqual(lib.window_config, (24, 1, T, E))
        for status, code in ((-4, "out_of_memory"), (-2, "window_mode_unsupported"),
                             (-1, "harness_internal_error"), (-3, "harness_internal_error")):
            lib.strategy_set_selected_window_v1 = lambda state, pointer, _s=status: _s
            with self.subTest(status=status), self.assertRaises(swr.SelectedWindowError) as caught:
                swr.SelectedWindowAbi(lib).configure(STATE, T, E)
            self.assertEqual(caught.exception.code, code)

    def test_counts_read_back_and_a_missing_generation_is_an_invariant(self):
        lib = FakeLib(full_plan())
        abi = swr.SelectedWindowAbi(lib)
        self.assertEqual(abi.counts(STATE), lib.native_counts)
        lib.strategy_selected_window_counts_v1 = lambda state, ref: -2
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.SelectedWindowAbi(lib).counts(STATE)
        self.assertEqual(caught.exception.code, "engine_invariant")

    def test_native_counts_must_equal_the_plan_and_the_observed_attempt(self):
        plan = full_plan()
        native = FakeLib(plan).native_counts
        observation = {"run_generation": 5, "attempt_serial": 9, "attempt_generation": 5}
        swr.check_native_counts(plan, native, observation)
        for name in ("fed_input_bars", "fed_script_bars", "preroll_input_bars",
                     "preroll_script_bars", "window_input_bars", "window_script_bars"):
            with self.subTest(name=name), self.assertRaises(swr.SelectedWindowError) as caught:
                swr.check_native_counts(plan, {**native, name: native[name] + 1}, observation)
            self.assertEqual(caught.exception.code, "engine_invariant")
        for name in ("run_generation", "attempt_serial", "attempt_generation"):
            with self.subTest(name=name), self.assertRaises(swr.SelectedWindowError):
                swr.check_native_counts(plan, native, {**observation, name: 99})
            with self.assertRaises(swr.SelectedWindowError):
                swr.check_native_counts(plan, {**native, name: 0}, {**observation, name: 0})

    def test_capability_record_when_unsupported_claims_nothing(self):
        got = swr.capabilities_record(False)["capabilities"]
        self.assertEqual((got["selected_window_wire_version"], got["report_policy"],
                          got["fingerprint_version"], got["report_shapes"]), (0, None, None, []))
        on = swr.capabilities_record(True)["capabilities"]
        self.assertEqual((on["selected_window_wire_version"], on["report_policy"],
                          on["fingerprint_version"], on["report_shapes"],
                          on["curve_point_budget"], on["results_digest_version"],
                          on["results_digest_platform"]), (1, "selected-window/v1", 2,
                                                           ["full/v1"], None, None, None))

    def test_the_supported_record_declares_the_parity_revision_alone_never_the_full_one(self):
        # A parity build (a prerelease build, not full 1.3 coverage) emits
        # selected_window_contract_revisions == ["1.3-parity"]; a full build carries ["1.3"];
        # never both. The literals are written out here, not read from the module.
        supported = {
            "engine": "pineforge",
            "capabilities": {
                "selected_window_wire_version": 1,
                "selected_window_contract_revisions": ["1.3-parity"],
                "report_policy": "selected-window/v1",
                "fingerprint_version": 2,
                "report_shapes": ["full/v1"],
                "curve_point_budget": None,
                "results_digest_version": None,
                "results_digest_platform": None,
            },
        }
        unsupported = json.loads(json.dumps(supported))
        unsupported["capabilities"].update({
            "selected_window_wire_version": 0, "selected_window_contract_revisions": [],
            "report_policy": None, "fingerprint_version": None, "report_shapes": []})
        # Whole documents: wire version 1, policy, fingerprint 2 and every other member are what
        # they were, and the declaration added no member.
        self.assertEqual(swr.capabilities_record(True), supported)
        self.assertEqual(swr.capabilities_record(False), unsupported)
        revisions = (
            swr.capabilities_record(True)["capabilities"]["selected_window_contract_revisions"])
        self.assertEqual(revisions, ["1.3-parity"])
        self.assertEqual(len(revisions), 1)               # a singleton ...
        self.assertNotIn("1.3", revisions)                # ... that is never the full revision
        self.assertEqual(
            swr.capabilities_record(False)["capabilities"]["selected_window_contract_revisions"],
            [])                                           # unsupported lists nothing at all
        self.assertEqual(swr.CONTRACT_REVISIONS, ("1.3-parity",))   # the one constant

    def test_the_declared_revisions_are_a_fresh_list_of_the_one_constant(self):
        first = swr.capabilities_record(True)["capabilities"]["selected_window_contract_revisions"]
        first.append("1.3")      # a caller that edits its copy changes neither the constant ...
        self.assertEqual(swr.CONTRACT_REVISIONS, ("1.3-parity",))
        again = swr.capabilities_record(True)["capabilities"]["selected_window_contract_revisions"]
        self.assertEqual(again, ["1.3-parity"])           # ... nor the next record
        self.assertIsNot(first, again)


# --- admission: nothing is touched before it passes --------------------------

class AdmissionTests(unittest.TestCase):
    """Refusal rows through run_json.main: poisoned feed, library, planner, constructor
    and phase entries. Every refusal leaves every counter at zero."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.path = Path(self.dir.name)
        self.ohlcv = self.path / "feed.csv"
        write_ohlcv(self.ohlcv, csv_rows(10))

    def refused(self, argv, code, detail, *, reads=0):
        with Counters() as poison:
            status, out, _ = run_main(argv)
            counts = poison.counts()
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]), (code, detail))
        zero = {name: 0 for name in counts}
        zero["inventory_reads"] = reads
        self.assertEqual(counts, zero)

    def test_primary_daily_weekly_and_their_aliases_name_the_canonical_token(self):
        artifact, inventory = write_inventory(self.path, INSIDE)
        for spelling, token in (("D", "D"), ("1D", "D"), ("W", "W"), ("1W", "W")):
            with self.subTest(primary=spelling):
                self.refused(selected_argv(artifact, self.ohlcv, inventory, script_tf=spelling),
                             "window_mode_unsupported",
                             {"capability": "selected_window_chart_timeframe", "timeframe": token})

    def test_a_known_excluded_request_feed_names_its_token(self):
        artifact, inventory = write_inventory(
            self.path, [{"kind": "token", "timeframe": "1"}, {"kind": "token", "timeframe": "W"}])
        self.refused(selected_argv(artifact, self.ohlcv, inventory), "window_mode_unsupported",
                     {"capability": "selected_window_request_feed_timeframe", "timeframe": "W"},
                     reads=2)

    def test_an_incomplete_inventory_has_the_capability_only(self):
        artifact, inventory = write_inventory(self.path, [{"kind": "unknown"}])
        self.refused(selected_argv(artifact, self.ohlcv, inventory), "window_mode_unsupported",
                     INVENTORY_CAPABILITY, reads=2)  # the helper hashes the artifact first
        # no inventory given at all: the helper is asked for a path of None and refuses it
        self.refused(selected_argv(artifact, self.ohlcv, None), "window_mode_unsupported",
                     INVENTORY_CAPABILITY, reads=1)
        # an inventory bound to another artifact
        artifact, inventory = write_inventory(self.path, INSIDE, artifact_sha="f" * 64)
        self.refused(selected_argv(artifact, self.ohlcv, inventory), "window_mode_unsupported",
                     INVENTORY_CAPABILITY, reads=2)

    def test_an_input_override_that_resolves_outside_the_list_names_that_token(self):
        artifact, inventory = write_inventory(
            self.path, [{"kind": "input", "key": "HTF", "default": "1"}])
        self.refused(selected_argv(artifact, self.ohlcv, inventory,
                                   extra=["--inputs", json.dumps({"HTF": "1W"})]),
                     "window_mode_unsupported",
                     {"capability": "selected_window_request_feed_timeframe", "timeframe": "W"},
                     reads=2)

    def test_an_inventory_inside_the_list_proceeds_to_work(self):
        artifact, inventory = write_inventory(self.path, INSIDE)
        with Counters() as poison:
            status, out, _ = run_main(selected_argv(artifact, self.ohlcv, inventory))
            counts = poison.counts()
        self.assertEqual(status, 1)  # the poisoned feed read is the expected end of this row
        self.assertEqual(last_line(out)["code"], "harness_internal_error")
        self.assertEqual((counts["load_bars"], counts["advance"], counts["inventory_reads"]),
                         (1, 1, 2))
        for name in ("load_symbol_feeds", "load_strategy", "create_strategy",
                     "SelectedPrimaryPlanner", "CDLL"):
            self.assertEqual(counts[name], 0)

    def test_an_ordinary_run_never_reads_the_inventory_or_the_policy_or_the_planner(self):
        artifact, inventory = write_inventory(
            self.path, [{"kind": "token", "timeframe": "W"}])
        argv = ["--so", str(artifact), "--ohlcv", str(self.ohlcv), "--input-tf", "1",
                "--script-tf", "1", "--request-feed-inventory", str(inventory)]
        with Counters() as poison, mock.patch.object(rfi, "admit_selected_request") as admit:
            status, out, _ = run_main(argv)
            counts = poison.counts()
        self.assertEqual(status, 1)  # the ordinary body reached its (poisoned) feed read
        self.assertEqual(admit.call_count, 0)
        self.assertEqual((counts["load_bars"], counts["advance"], counts["inventory_reads"],
                          counts["SelectedPrimaryPlanner"], counts["CDLL"]), (1, 0, 0, 0, 0))

    def test_unsupported_options_and_bad_requests_fail_before_any_work(self):
        artifact, inventory = write_inventory(self.path, INSIDE)
        base = selected_argv(artifact, self.ohlcv, inventory)
        for extra, code, detail in (
                (["--report-shape", "compact/v1"], "report_shape_unsupported",
                 {"shape": "compact/v1"}),
                (["--curve-point-budget", "2500"], "report_shape_request_invalid",
                 {"option": "curve_point_budget", "value": "2500"}),
                (["--results-digest", "sha256/v1"], "results_digest_unsupported",
                 {"requested": "sha256/v1"})):
            with self.subTest(extra=extra):
                self.refused(base + extra, code, detail)
        self.refused(selected_argv(artifact, self.ohlcv, inventory, script_tf=""),
                     "window_request_invalid", {"option": "script_tf"})
        self.refused(base + ["--trade-start-ms", "5"], "window_legacy_flag_conflict",
                     {"option": "trade_start_ms"})
        self.refused([a for a in base if a != "--report-policy" and a != "selected-window/v1"],
                     "window_request_invalid", {"option": "report_policy"})
        self.refused(["--so", str(artifact), "--ohlcv", str(self.ohlcv), "--report-shape",
                      "full/v1"], "report_shape_unsupported", {"shape": "full/v1"})


# --- the selected run, end to end on fakes ----------------------------------

class SelectedRunTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.path = Path(self.dir.name)
        self.events = []
        self.parent, self.child = socket.socketpair()
        self.addCleanup(self.parent.close)
        self.addCleanup(self.child.close)
        self.rows = csv_rows(10)
        self.ohlcv = self.path / "feed.csv"
        write_ohlcv(self.ohlcv, self.rows)
        self.artifact, self.inventory = write_inventory(self.path, INSIDE)

    def fresh_phase_socket(self):
        """A new phase socketpair and an empty event list, for the next row of a loop."""
        self.events.clear()
        self.parent.close()
        self.child.close()
        self.parent, self.child = socket.socketpair()

    def run_selected(self, lib, plan, *, extra=(), fail_phase=None, rows=None, writer=True,
                     patches=(), stdout_class=Stdout):
        if rows is not None:
            write_ohlcv(self.ohlcv, rows)
        FakePlanner.reset(plan)
        self.captured = {}
        real_load = run_json.load_bars

        def load(path):
            result = real_load(path)
            self.captured["bars"] = result[0]
            return result

        argv = selected_argv(self.artifact, self.ohlcv, self.inventory,
                             extra=list(extra) + (["--run-phase-fd", str(self.child.fileno())]
                                                  if writer else []))
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(run_json, "load_strategy", lambda p: lib))
            stack.enter_context(mock.patch.object(run_json, "load_bars", load))
            stack.enter_context(mock.patch.object(swp, "SelectedPrimaryPlanner", FakePlanner))
            stack.enter_context(mock.patch.object(
                run_json, "normalize_release_provenance", lambda p, c, r, k: p))
            stack.enter_context(mock.patch.object(
                rpt, "RunPhaseWriter", recording_writer(self.events, fail_phase)))
            for patch in patches:
                stack.enter_context(patch)
            return run_main(argv, self.events, stdout_class)

    def test_a_selected_run_end_to_end(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        status, out, err = self.run_selected(lib, plan)
        self.assertEqual((status, err), (0, ""))
        report = json.loads(out)
        # --- the feed: planned once, trimmed by pointer offset, retained rows fed
        bars = self.captured["bars"]
        planner_bars, planner_count, planner_kwargs = FakePlanner.calls[0]
        self.assertIs(planner_bars, bars)
        self.assertEqual(planner_count, 10)
        self.assertEqual(
            {k: planner_kwargs[k] for k in ("start_ms", "end_ms", "fed_start_ms", "preroll_bars",
                                            "input_tf", "script_tf", "chart_timezone",
                                            "engine_timezone", "session", "feed_tolerant")},
            {"start_ms": T, "end_ms": E, "fed_start_ms": F, "preroll_bars": N, "input_tf": "1",
             "script_tf": "1", "chart_timezone": "", "engine_timezone": "", "session": "",
             "feed_tolerant": True})
        self.assertEqual(len(FakePlanner.constructed), 1)
        self.assertTrue(FakePlanner.constructed[0].endswith("/lib/libpineforge_window_plan.so"))
        self.assertTrue(Path(FakePlanner.constructed[0]).is_absolute())
        rows_seen, count_seen = lib.run_args
        self.assertEqual(count_seen, 9)
        self.assertEqual(ctypes.addressof(rows_seen),
                         ctypes.addressof(bars) + ctypes.sizeof(run_json.BarC))
        # --- R, in the three places, identical, with the expected hashes
        hashes = (hashlib.sha256(self.ohlcv.read_bytes()).hexdigest(), values_hash(self.rows),
                  values_hash(self.rows[1:]))
        window = expected_window(plan, hashes)
        self.assertEqual(report["report_window"], window)
        self.assertEqual(report["applied_runtime"]["report_window"], window)
        self.assertEqual(report["fingerprint"]["provenance"]["runtime"]["report_window"], window)
        # --- the lookback triple of a build with no lookback producer, the same
        #     in all three copies: unknown, null, null; never true, never false
        for copy in (report["report_window"], report["applied_runtime"]["report_window"],
                     report["fingerprint"]["provenance"]["runtime"]["report_window"]):
            self.assertEqual(copy["lookback"], NOT_PRODUCED_LOOKBACK)
            self.assertIsNone(copy["lookback"]["has_unbounded_state"])
        # --- fingerprint v2, report shape, timing, evaluated-fed legacy fields
        self.assertEqual(report["fingerprint"]["version"], 2)
        self.assertEqual(report["fingerprint"]["provenance"]["schema_version"], 2)
        raw_token = base64.b64decode(report["fingerprint"]["token"])
        token = raw_token.decode("utf-8")
        self.assertIn('"schema_version":2', token)
        # --- the hashed bytes keep the same triple, and the digest is the hash of those bytes
        hashed = json.loads(token)
        self.assertEqual(hashed["runtime"]["report_window"]["lookback"], NOT_PRODUCED_LOOKBACK)
        self.assertIn('"has_unbounded_state":null', token)
        self.assertNotIn('"has_unbounded_state":true', token)
        self.assertNotIn('"has_unbounded_state":false', token)
        self.assertEqual(report["fingerprint"]["digest"],
                         "sha256:" + hashlib.sha256(raw_token).hexdigest())
        # --- the capability revision is a declaration of the probe only: no literal of it is in
        #     the report or the hashed fingerprint bytes (the phase records are checked below)
        self.assertNotIn("1.3-parity", out)
        self.assertNotIn("1.3-parity", token)
        self.assertEqual(report["report_window"]["wire_version"], 1)
        self.assertEqual(report["report_shape"], swr.report_shape_full(7, 0))
        timing = report["diagnostics"]["phase_timing"]
        self.assertEqual((timing["version"], timing["results_digest_ms"]), (1, None))
        self.assertGreaterEqual(timing["execution_ms"], 0)
        self.assertEqual((report["input"]["bars"], report["input"]["first_ts"],
                          report["input"]["last_ts"]),
                         (9, F + MINUTE, F + 9 * MINUTE))
        self.assertEqual(report["summary"]["bars_processed"], 9)
        self.assertEqual(report["diagnostics"]["input_bars_processed"], 9)
        # --- the native curve is serialized as it is: three fields, anchor first
        self.assertEqual(len(report["equity_curve"]), 7)
        self.assertTrue(all(set(p) == {"time_ms", "equity", "open_profit"}
                            for p in report["equity_curve"]))
        self.assertEqual(report["equity_curve"][0]["time_ms"], T)
        # --- order: selection configured before the run and cleared after, observer detached
        names = [c[0] for c in lib.calls]
        self.assertEqual(lib.window_config, None)
        self.assertLess(names.index("set_selected_window"), names.index("run_backtest_full"))
        self.assertLess(names.index("set_observer"), names.index("run_backtest_full"))
        tail = names[names.index("run_backtest_full"):]
        self.assertEqual(tail[-4:], ["set_observer", "set_selected_window", "report_free",
                                     "strategy_free"])
        self.assertIsNone(lib.observer_descriptor)
        self.assertEqual(lib.calls[names.index("set_selected_window")][1:], (T, E))
        # --- phases: the observer announced result_assembly INSIDE the native call; completed
        #     follows the flush; the receipt names the whole record handed over
        records = phase_records(self.parent)
        self.assertEqual([r["phase"] for r in records],
                         ["preflight", "execution", "result_assembly", "serialization",
                          "completed"])
        self.assertEqual([r["sequence"] for r in records], [1, 2, 3, 4, 5])
        self.assertEqual({r["version"] for r in records}, {1})   # phase version 1, unchanged
        self.assertNotIn("1.3-parity", json.dumps(records))     # no revision literal in a phase
        self.assertEqual(lib.observer_rc, 0)
        self.assertEqual(lib.receipt[2], 1)
        self.assertEqual(lib.receipt[0], lib.receipt[1])
        ev = self.events
        self.assertLess(ev.index("advance:execution"), ev.index("native.start"))
        self.assertLess(ev.index("native.start"), ev.index("advance:result_assembly"))
        self.assertLess(ev.index("advance:result_assembly"), ev.index("native.end"))
        self.assertLess(ev.index("native.end"), ev.index("advance:serialization"))
        self.assertLess(ev.index("advance:serialization"), ev.index("stdout.write"))
        self.assertLess(max(i for i, e in enumerate(ev) if e == "stdout.flush"),
                        ev.index("advance:completed"))

    def test_an_empty_window_is_a_valid_anchor_only_run(self):
        plan = empty_window_plan()
        lib = FakeLib(plan, self.events)
        rows = csv_rows(4)
        status, out, _ = self.run_selected(lib, plan, rows=rows)
        self.assertEqual(status, 0)
        report = json.loads(out)
        hashes = (hashlib.sha256(self.ohlcv.read_bytes()).hexdigest(), values_hash(rows),
                  values_hash(rows[1:]))
        self.assertEqual(report["report_window"], expected_window(plan, hashes))
        for copy in (report["report_window"], report["applied_runtime"]["report_window"],
                     report["fingerprint"]["provenance"]["runtime"]["report_window"]):
            self.assertEqual(copy["lookback"], NOT_PRODUCED_LOOKBACK)  # an empty window too
        self.assertEqual(len(report["equity_curve"]), 1)
        self.assertEqual((report["trades"], report["input"]["bars"]), ([], 3))

    def test_validation_only_runs_nothing_and_reports_applied_false(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        loader = mock.Mock(side_effect=PoisonTripped("load_strategy"))
        writer = mock.Mock(side_effect=PoisonTripped("RunPhaseWriter"))
        status, out, _ = self.run_selected(
            lib, plan, extra=["--validate-window-only"], writer=False,
            patches=[mock.patch.object(run_json, "load_strategy", loader),
                     mock.patch.object(rpt, "RunPhaseWriter", writer)])
        self.assertEqual(status, 0)
        doc = json.loads(out)
        self.assertEqual(set(doc), {"engine", "validation_only", "report_window"})
        self.assertTrue(doc["validation_only"])
        self.assertFalse(doc["report_window"]["applied"])
        self.assertEqual((doc["report_window"]["counts"]["equity_points"],
                          doc["report_window"]["counts"]["anchor_points"]), (0, 0))
        self.assertEqual((loader.call_count, writer.call_count, lib.calls), (0, 0, []))
        # The validation-only R is the real builder's: the whole record, and in it the same
        # lookback triple as a run, not a different or an invented one.
        hashes = (hashlib.sha256(self.ohlcv.read_bytes()).hexdigest(), values_hash(self.rows),
                  values_hash(self.rows[1:]))
        self.assertEqual(doc["report_window"], expected_window(plan, hashes, applied=False))
        self.assertEqual(doc["report_window"]["lookback"], NOT_PRODUCED_LOOKBACK)
        self.assertIsNone(doc["report_window"]["lookback"]["has_unbounded_state"])

    def test_a_planner_refusal_ends_the_run_before_any_strategy_library(self):
        plan = full_plan(status=3, bound=0, value_ms=T + 1, previous_boundary_ms=T,
                         next_boundary_ms=T + MINUTE)
        loader = mock.Mock(side_effect=PoisonTripped("load_strategy"))
        status, out, _ = self.run_selected(
            FakeLib(plan, self.events), plan,
            patches=[mock.patch.object(run_json, "load_strategy", loader)])
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]["bound"]), ("window_boundary_unaligned", "start"))
        self.assertEqual(loader.call_count, 0)
        self.assertNotIn("advance:execution", self.events)

    def test_a_missing_planner_library_is_window_mode_unsupported(self):
        plan = full_plan()

        class Missing:
            def __init__(self, path):
                raise swp.PlanBridgeError("window_mode_unsupported", "no helper",
                                          {"capability": "selected_window_planner_v1"})

        status, out, _ = self.run_selected(
            FakeLib(plan, self.events), plan,
            patches=[mock.patch.object(swp, "SelectedPrimaryPlanner", Missing)])
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]),
                         ("window_mode_unsupported", {"capability": "selected_window_planner_v1"}))

    def test_the_callbacks_retained_failure_is_raised_before_any_report_access(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        spy = mock.Mock(side_effect=PoisonTripped("build_report_dict"))
        status, out, _ = self.run_selected(
            lib, plan, fail_phase="result_assembly",
            patches=[mock.patch.object(run_json, "build_report_dict", spy)])
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual(line["code"], "harness_internal_error")
        self.assertEqual((lib.observer_rc, spy.call_count), (-1, 0))
        names = [c[0] for c in lib.calls]
        self.assertEqual(names[-4:], ["set_observer", "set_selected_window", "report_free",
                                     "strategy_free"])
        self.assertNotIn("advance:completed", self.events)
        self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                         ["preflight", "execution"])

    def test_an_engine_that_never_announces_the_end_of_execution_is_a_harness_fault(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events, call_observer=False)
        status, out, _ = self.run_selected(lib, plan)
        self.assertEqual(status, 1)
        self.assertEqual(last_line(out)["code"], "harness_internal_error")
        self.assertNotIn("advance:result_assembly", self.events)  # never emulated afterwards

    def test_native_counts_that_differ_from_the_plan_are_an_engine_invariant(self):
        plan = full_plan()
        for counts, observation in (({"window_script_bars": 7}, {}),
                                    ({}, {"attempt_serial": 10})):
            with self.subTest(counts=counts, observation=observation):
                self.events.clear()
                self.parent.close()
                self.parent, self.child = socket.socketpair()
                lib = FakeLib(plan, self.events, counts=counts, observation=observation)
                status, out, _ = self.run_selected(lib, plan)
                self.assertEqual(status, 1)
                self.assertEqual(last_line(out)["code"], "engine_invariant")
                self.assertNotIn("report_window", out)
                self.assertEqual([c[0] for c in lib.calls][-2:], ["report_free", "strategy_free"])

    def test_a_native_curve_of_the_wrong_length_is_an_engine_invariant(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        lib.plan = {**plan, "window_script_bars": 5}  # the fake engine curve has 6 points
        status, out, _ = self.run_selected(lib, plan)
        self.assertEqual(status, 1)
        self.assertEqual(last_line(out)["code"], "engine_invariant")

    def test_a_library_without_the_selected_window_abi_is_refused_typed(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events, selected_abi=False)
        status, out, _ = self.run_selected(lib, plan)
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]),
                         ("window_mode_unsupported", {"capability": "selected_window_v1"}))
        self.assertEqual([c[0] for c in lib.calls if c[0] == "strategy_create"], [])

    def test_a_library_without_the_observer_is_refused_typed_and_still_cleaned_up(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events, observer_abi=False)
        status, out, _ = self.run_selected(lib, plan)
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]),
                         ("window_mode_unsupported", {"capability": "execution_observer_v1"}))
        self.assertEqual([c[0] for c in lib.calls][-2:], ["report_free", "strategy_free"])
        self.assertNotIn("run_backtest_full", [c[0] for c in lib.calls])

    def test_a_run_without_a_phase_descriptor_still_tracks_the_phases_internally(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        status, out, _ = self.run_selected(lib, plan, writer=False)
        self.assertEqual(status, 0)
        self.assertEqual(lib.receipt, (0, 0, 0))  # the receipt of a writer that exports nothing
        self.assertIn("phase_timing", json.loads(out)["diagnostics"])

    # --- fail-closed fingerprint, anchor equity, post-boundary classification --------------

    def test_a_provenance_failure_never_yields_a_report_or_a_null_fingerprint(self):
        plan = full_plan()
        for error, reason in ((ValueError("bad provenance"), "exception"),
                              (MemoryError(), "resource"), (OSError(5, "I/O error"), "io")):
            with self.subTest(error=type(error).__name__):
                self.fresh_phase_socket()
                lib = FakeLib(plan, self.events)
                status, out, _ = self.run_selected(lib, plan, patches=[
                    mock.patch.object(run_json, "build_fingerprint",
                                      mock.Mock(side_effect=error))])
                self.assertEqual(status, 1)
                line = last_line(out)
                self.assertEqual((line["code"], line["args"], line["error"]),
                                 ("report_post_execution_failed",
                                  {"phase": "result_assembly", "reason": reason},
                                  run_json.POST_EXECUTION_TEXT))
                self.assertEqual(len(out.strip().splitlines()), 1)  # no report, no 2nd document
                self.assertNotIn("fingerprint", out)
                self.assertNotIn("advance:serialization", self.events)
                self.assertNotIn("advance:completed", self.events)
                self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                                 ["preflight", "execution", "result_assembly"])
                self.assertEqual([c[0] for c in lib.calls][-2:], ["report_free", "strategy_free"])

    def test_a_typed_fault_in_the_provenance_keeps_its_own_code(self):
        plan = full_plan()
        typed = swr.SelectedWindowError("engine_invariant", "injected typed fault")
        lib = FakeLib(plan, self.events)
        status, out, _ = self.run_selected(lib, plan, patches=[
            mock.patch.object(run_json, "normalize_release_provenance",
                              mock.Mock(side_effect=typed))])
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["error"]),
                         ("engine_invariant", "injected typed fault"))

    def test_a_fingerprint_whose_digest_is_not_its_hash_is_refused_before_serialization(self):
        plan = full_plan()
        real = run_json.build_fingerprint

        def corrupted(provenance):
            fingerprint = real(provenance)
            fingerprint["digest"] = "sha256:" + "0" * 64
            return fingerprint

        lib = FakeLib(plan, self.events)
        status, out, _ = self.run_selected(lib, plan, patches=[
            mock.patch.object(run_json, "build_fingerprint", corrupted)])
        self.assertEqual((status, last_line(out)["code"]), (1, "engine_invariant"))
        self.assertNotIn("advance:serialization", self.events)

    def test_an_anchor_that_is_not_the_initial_capital_is_refused_before_serialization(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events, anchor_equity=CAPITAL - 1.0)
        status, out, _ = self.run_selected(lib, plan)
        self.assertEqual((status, last_line(out)["code"]), (1, "engine_invariant"))
        self.assertIn("is not the initial capital", last_line(out)["error"])
        self.assertNotIn("report_window", out)
        self.assertNotIn("advance:serialization", self.events)

    # --- an initial capital the provenance leaves unresolved ------------------------------
    #
    # The selected path keeps an unresolved initial-capital provenance unresolved, as ordinary
    # mode does, and skips only the comparison that cannot be made; there is no selected-only
    # refusal. The native anchor derives from the engine's own initial capital, so an
    # unresolved SOURCE provenance is not evidence that the anchor is wrong.
    #
    # The normalizer here is a stand-in and the engine a fake; ordinary mode against selected
    # mode on the real engine (same source, same feed bytes, same inputs) is not covered here.

    def test_an_unresolved_initial_capital_completes_and_stays_unresolved(self):
        plan = full_plan()
        seen = {}
        anchor = 250_000.0  # not the seed capital: a capital copied from the anchor would show
        lib = FakeLib(plan, self.events, anchor_equity=anchor)
        status, out, err = self.run_selected(lib, plan, patches=[
            mock.patch.object(run_json, "normalize_release_provenance",
                              unresolved_capital_normalizer(seen))])
        self.assertEqual((status, err), (0, ""))
        report = json.loads(out)
        # Completed, as a v2 report whose R is in all three places.
        hashes = (hashlib.sha256(self.ohlcv.read_bytes()).hexdigest(), values_hash(self.rows),
                  values_hash(self.rows[1:]))
        window = expected_window(plan, hashes)
        self.assertEqual(report["report_window"], window)
        self.assertEqual(report["applied_runtime"]["report_window"], window)
        provenance = report["fingerprint"]["provenance"]
        self.assertEqual(provenance["runtime"]["report_window"], window)
        self.assertEqual((report["fingerprint"]["version"], provenance["schema_version"]), (2, 2))
        # The provenance still says "unresolved", in the report and in the hashed bytes.
        self.assertIsNone(provenance["strategy"]["initial_capital"])
        self.assertEqual(provenance["strategy_resolution"], UNRESOLVED_RESOLUTION)
        hashed = json.loads(base64.b64decode(report["fingerprint"]["token"]).decode("utf-8"))
        self.assertIsNone(hashed["strategy"]["initial_capital"])
        self.assertEqual(hashed["strategy_resolution"], UNRESOLVED_RESOLUTION)
        self.assertEqual(hashed["schema_version"], 2)
        self.assertEqual(hashed["runtime"]["report_window"], window)
        # Not mutated and not invented: the provenance is what the normalizer returned plus the
        # version key run_json always adds, and the native anchor is untouched.
        self.assertEqual(provenance, {**seen["normalized"], "schema_version": 2})
        self.assertIsNone(seen["normalized"]["strategy"]["initial_capital"])
        self.assertEqual(len(report["equity_curve"]), 7)
        self.assertEqual(report["equity_curve"][0],
                         {"time_ms": T, "equity": anchor, "open_profit": 0.0})
        # Phases: serialization, and completed after the flush.
        self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                         ["preflight", "execution", "result_assembly", "serialization",
                          "completed"])

    def test_ordinary_mode_records_an_unresolved_initial_capital_the_same_way(self):
        # The representation the selected run keeps above is the ordinary run's: ordinary mode
        # never refuses on it either. Same stand-in normalizer, so this shows no more than that
        # both bodies hand the normalizer's result on unchanged.
        lib = FakeLib(full_plan(fed_input_bars=10, fed_script_bars=10, window_script_bars=10))
        lib.run_backtest_full = lambda *a: OrdinaryAndCapabilityTests._fill(a[-1]._obj)
        argv = ["--so", str(self.artifact), "--ohlcv", str(self.ohlcv), "--input-tf", "1",
                "--script-tf", "1"]
        with mock.patch.object(run_json, "load_strategy", lambda p: lib), \
                mock.patch.object(run_json, "normalize_release_provenance",
                                  unresolved_capital_normalizer()):
            status, out, _ = run_main(argv)
        self.assertEqual(status, 0)
        provenance = json.loads(out)["fingerprint"]["provenance"]
        self.assertIsNone(provenance["strategy"]["initial_capital"])
        self.assertEqual(provenance["strategy_resolution"], UNRESOLVED_RESOLUTION)
        self.assertNotIn("schema_version", provenance)

    def test_an_established_capital_is_still_compared_with_the_native_anchor(self):
        plan = full_plan()
        override = ["--overrides", json.dumps({"initial_capital": "250000"})]
        for anchor, completes in ((250_000.0, True), (CAPITAL, False), (250_000.5, False)):
            with self.subTest(anchor=anchor):
                self.fresh_phase_socket()
                lib = FakeLib(plan, self.events, anchor_equity=anchor)
                status, out, _ = self.run_selected(lib, plan, extra=override)
                if completes:
                    self.assertEqual(status, 0)
                    strategy = json.loads(out)["fingerprint"]["provenance"]["strategy"]
                    self.assertEqual(strategy["initial_capital"], "250000")  # as given, unchanged
                    continue
                self.assertEqual(status, 1)
                line = last_line(out)
                self.assertEqual(line["code"], "engine_invariant")
                self.assertIn("is not the initial capital", line["error"])
                self.assertEqual(len(out.strip().splitlines()), 1)
                self.assertNotIn("report_window", out)
                self.assertNotIn("advance:serialization", self.events)
                self.assertNotIn("advance:completed", self.events)
                self.assertEqual([c[0] for c in lib.calls][-2:], ["report_free", "strategy_free"])

    def test_a_malformed_actual_anchor_is_refused_even_when_the_capital_is_unresolved(self):
        plan = full_plan()
        # A non-finite native anchor reaches the report as null (build_report_dict writes every
        # non-finite double that way); it must not be written out as a valid-looking number.
        for anchor in (float("nan"), float("inf"), float("-inf")):
            for unresolved in (False, True):
                with self.subTest(anchor=anchor, unresolved=unresolved):
                    self.fresh_phase_socket()
                    lib = FakeLib(plan, self.events, anchor_equity=anchor)
                    patches = [mock.patch.object(run_json, "normalize_release_provenance",
                                                 unresolved_capital_normalizer())
                               ] if unresolved else []
                    status, out, _ = self.run_selected(lib, plan, patches=patches)
                    self.assertEqual(status, 1)
                    line = last_line(out)
                    self.assertEqual(line["code"], "engine_invariant")
                    self.assertIn("not a finite number", line["error"])
                    self.assertEqual(len(out.strip().splitlines()), 1)  # no report, no 2nd line
                    self.assertNotIn("report_window", out)
                    self.assertNotIn("advance:serialization", self.events)
                    self.assertNotIn("advance:completed", self.events)
                    self.assertEqual([c[0] for c in lib.calls][-2:],
                                     ["report_free", "strategy_free"])

    def test_unclassified_failures_after_the_boundary_are_report_post_execution_failed(self):
        plan = full_plan()
        for error, reason in ((RuntimeError("boom"), "exception"), (MemoryError(), "resource"),
                              (OSError(28, "No space left on device"), "io")):
            with self.subTest(error=type(error).__name__):
                self.fresh_phase_socket()
                lib = FakeLib(plan, self.events)
                status, out, _ = self.run_selected(lib, plan, patches=[
                    mock.patch.object(run_json, "build_report_dict",
                                      mock.Mock(side_effect=error))])
                self.assertEqual(status, 1)
                line = last_line(out)
                self.assertEqual((line["code"], line["args"]),
                                 ("report_post_execution_failed",
                                  {"phase": "result_assembly", "reason": reason}))
                self.assertEqual(len(out.strip().splitlines()), 1)
                self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                                 ["preflight", "execution", "result_assembly"])

    def test_a_failure_after_serialization_began_names_the_serialization_phase(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        status, out, _ = self.run_selected(lib, plan, patches=[
            mock.patch.object(swr, "report_shape_full", lambda points, trades: {"bad": {1, 2}})])
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]),
                         ("report_post_execution_failed",
                          {"phase": "serialization", "reason": "exception"}))
        self.assertEqual(len(out.strip().splitlines()), 1)  # nothing of the report was written
        self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                         ["preflight", "execution", "result_assembly", "serialization"])
        self.assertNotIn("advance:completed", self.events)

    def test_the_same_error_before_the_boundary_is_not_a_post_execution_failure(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events, raise_before_observer=RuntimeError("early"))
        status, out, _ = self.run_selected(lib, plan)
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual(line["code"], "harness_internal_error")
        self.assertIn("RuntimeError", line["error"])
        self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                         ["preflight", "execution"])
        self.assertEqual([c[0] for c in lib.calls][-2:], ["report_free", "strategy_free"])

    def test_engine_failures_are_classified_by_the_real_phase(self):
        plan = full_plan()
        after = {"phase": "result_assembly"}
        cases = (  # (engine code, observer called, expected code, expected args)
            ("out_of_memory", True, "report_post_execution_failed", {**after, "reason": "resource"}),
            ("", True, "report_post_execution_failed", {**after, "reason": "exception"}),
            ("engine_invariant", True, "engine_invariant", {}),
            ("out_of_memory", False, "out_of_memory", {}),
            ("strategy_runtime_error", False, "strategy_runtime_error", {}),
            ("", False, "engine_unclassified_error", {}),
        )
        for code, observed, expected_code, expected_args in cases:
            with self.subTest(code=code, observer=observed):
                self.fresh_phase_socket()
                lib = FakeLib(plan, self.events, call_observer=observed,
                              engine_failure=(code, "the engine said so"))
                status, out, _ = self.run_selected(lib, plan)
                self.assertEqual(status, 1)
                line = last_line(out)
                self.assertEqual((line["code"], line["args"]), (expected_code, expected_args))
                self.assertEqual(len(out.strip().splitlines()), 1)

    def test_partial_stdout_emits_no_second_document_and_keeps_the_terminal_phase(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        status, out, err = self.run_selected(lib, plan, stdout_class=PartialStdout)
        self.assertEqual(status, 1)
        self.assertNotIn('"engine":"pineforge","error"', out)  # no failure line follows
        with self.assertRaises(ValueError):
            json.loads(out)  # what reached stdout is half a document, and only that
        self.assertIn("report_post_execution_failed (phase serialization, reason io)", err)
        self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                         ["preflight", "execution", "result_assembly", "serialization"])
        self.assertNotIn("advance:completed", self.events)

    def test_a_completed_handoff_failure_is_a_harness_fault_and_never_a_success(self):
        plan = full_plan()
        lib = FakeLib(plan, self.events)
        status, out, err = self.run_selected(lib, plan, fail_phase="completed")
        self.assertEqual(status, 1)
        report = json.loads(out)  # one document only: the whole report, no failure line after it
        self.assertTrue(report["report_window"]["applied"])
        self.assertIn("harness_internal_error", err)
        self.assertEqual([r["phase"] for r in phase_records(self.parent)],
                         ["preflight", "execution", "result_assembly", "serialization"])

    def test_a_managed_ordinary_run_is_classified_alike_and_a_plain_one_is_not(self):
        plan = full_plan()
        argv = ["--so", str(self.artifact), "--ohlcv", str(self.ohlcv), "--input-tf", "1",
                "--script-tf", "1"]
        for managed in (True, False):
            with self.subTest(managed=managed):
                self.fresh_phase_socket()
                lib = FakeLib(plan, self.events)
                extra = ["--run-phase-fd", str(self.child.fileno())] if managed else []
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(run_json, "load_strategy",
                                                          lambda p, _l=lib: _l))
                    stack.enter_context(mock.patch.object(
                        rpt, "RunPhaseWriter", recording_writer(self.events)))
                    stack.enter_context(mock.patch.object(
                        run_json, "build_report_dict",
                        mock.Mock(side_effect=RuntimeError("boom"))))
                    status, out, _ = run_main(argv + extra, self.events)
                self.assertEqual(status, 1)
                line = last_line(out)
                if managed:
                    self.assertEqual((line["code"], line["args"]),
                                     ("report_post_execution_failed",
                                      {"phase": "result_assembly", "reason": "exception"}))
                else:
                    self.assertEqual(line["code"], "harness_internal_error")


class OrdinaryAndCapabilityTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.path = Path(self.dir.name)

    def test_the_ordinary_report_gains_no_selected_window_field(self):
        ohlcv = self.path / "feed.csv"
        write_ohlcv(ohlcv, csv_rows(10))
        plan = full_plan(fed_input_bars=10, fed_script_bars=10, window_script_bars=10)
        lib = FakeLib(plan)
        lib.run_backtest_full = lambda *a: self._fill(a[-1]._obj)
        with mock.patch.object(run_json, "load_strategy", lambda p: lib), \
                mock.patch.object(run_json, "normalize_release_provenance", lambda p, c, r, k: p):
            status, out, _ = run_main(["--so", "x.so", "--ohlcv", str(ohlcv), "--input-tf", "1",
                                       "--script-tf", "1"])
        self.assertEqual(status, 0)
        report = json.loads(out)
        for key in ("report_window", "report_shape"):
            self.assertNotIn(key, report)
        self.assertNotIn("report_window", report["applied_runtime"])
        self.assertNotIn("phase_timing", report["diagnostics"])
        self.assertNotIn("version", report["fingerprint"])
        self.assertNotIn("schema_version", report["fingerprint"]["provenance"])
        self.assertEqual((report["input"]["bars"], report["input"]["first_ts"]), (10, F))
        # An ordinary report gains no R and so no lookback record of any kind (the lookback
        # member belongs to R only): not in the report, not in the hashed bytes.
        self.assertNotIn("lookback", out)
        self.assertNotIn("has_unbounded_state", out)
        self.assertNotIn(b"lookback", base64.b64decode(report["fingerprint"]["token"]))

    @staticmethod
    def _fill(report):
        report.input_tf_seconds = 60
        report.script_tf_seconds = 60
        report.script_tf_ratio = 1

    def test_capabilities_probe_reports_a_supported_runtime_without_an_ohlcv(self):
        lib = FakeLib(full_plan())
        FakePlanner.reset(None)
        with mock.patch.object(run_json, "load_strategy", lambda p: lib), \
                mock.patch.object(swp, "SelectedPrimaryPlanner", FakePlanner):
            status, out, _ = run_main(["--so", "x.so", "--capabilities-json"])
        self.assertEqual((status, json.loads(out)), (0, swr.capabilities_record(True)))
        self.assertEqual(lib.calls, [])  # nothing was constructed or run
        # The probe's real stdout carries the parity declaration alone, not the full revision.
        printed = json.loads(out)["capabilities"]
        self.assertEqual(printed["selected_window_contract_revisions"], ["1.3-parity"])
        self.assertIn('"selected_window_contract_revisions":["1.3-parity"]', out)
        self.assertNotIn('"1.3"', out)
        self.assertEqual((printed["selected_window_wire_version"], printed["fingerprint_version"]),
                         (1, 2))

    def test_capabilities_probe_reports_zero_when_any_piece_is_missing(self):
        for lib in (FakeLib(full_plan(), selected_abi=False), FakeLib(full_plan(), observer_abi=False)):
            with mock.patch.object(run_json, "load_strategy", lambda p, _l=lib: _l), \
                    mock.patch.object(swp, "SelectedPrimaryPlanner", FakePlanner):
                status, out, _ = run_main(["--so", "x.so", "--capabilities-json"])
            self.assertEqual((status, json.loads(out)), (0, swr.capabilities_record(False)))
            # Not supported: no revision is listed at all, so neither "1.3-parity" nor "1.3".
            self.assertEqual(
                json.loads(out)["capabilities"]["selected_window_contract_revisions"], [])
            self.assertNotIn("1.3", out)

    def test_the_trusted_planner_path_is_absolute_under_the_installation_prefix(self):
        path = Path(run_json.trusted_planner_path())
        self.assertTrue(path.is_absolute())
        self.assertEqual((path.name, path.parent.name), ("libpineforge_window_plan.so", "lib"))
        self.assertEqual(path.parent.parent, Path(run_json.__file__).resolve().parent.parent)


class NumericBoundTests(unittest.TestCase):
    """Oversized decimal request numbers are typed request errors: the length is tested before
    any int(), so Python's integer digit-limit ValueError (a harness fault) is unreachable.
    Recognising a timeframe token stays unbounded; only the numeric conversion is bounded."""
    HUGE = "9" * 5000  # over the 4300 digits int() accepts by default

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.path = Path(self.dir.name)
        self.ohlcv = self.path / "feed.csv"
        write_ohlcv(self.ohlcv, csv_rows(10))
        self.artifact, self.inventory = write_inventory(self.path, INSIDE)

    def argv_with(self, flag, value):
        argv = selected_argv(self.artifact, self.ohlcv, self.inventory)
        argv[argv.index(flag) + 1] = value
        return argv

    def test_every_request_number_is_bounded_by_length_before_int(self):
        for option in ("window_start_ms", "window_end_ms", "preroll_bars", "fed_start_ms"):
            for text in (self.HUGE, "-" + self.HUGE, "9" * 17, "-" + "9" * 17):
                with self.subTest(option=option, length=len(text)), \
                        self.assertRaises(swr.SelectedWindowError) as caught:
                    swr.validate_request(**{**ValidationTests.GOOD, option: text})
                self.assertEqual((caught.exception.code, caught.exception.code_args),
                                 ("window_request_invalid", {"option": option}))

    def test_the_largest_i53_numbers_are_still_accepted(self):
        got = swr.validate_request(**{**ValidationTests.GOOD, "window_start_ms": str(2 ** 53 - 2),
                                      "window_end_ms": str(2 ** 53 - 1),
                                      "fed_start_ms": str(-(2 ** 53 - 1)), "preroll_bars": "0"})
        self.assertEqual((got.start_ms, got.end_ms, got.fed_start_ms),
                         (2 ** 53 - 2, 2 ** 53 - 1, -(2 ** 53 - 1)))

    def test_token_recognition_is_unbounded_and_its_conversion_is_bounded(self):
        token = "1" + "0" * 5000
        self.assertEqual(swr.canonical_timeframe(token), token)  # unchanged recognition
        self.assertIsNone(swr.timeframe_seconds(token))
        self.assertIsNone(swr.timeframe_seconds("9999999999999999"))  # over u53 seconds
        self.assertEqual(swr.timeframe_seconds("100"), 6000)
        with self.assertRaises(swr.SelectedWindowError) as caught:
            swr.validate_request(**{**ValidationTests.GOOD, "input_tf": token})
        self.assertEqual((caught.exception.code, caught.exception.code_args),
                         ("window_request_invalid", {"option": "input_tf"}))
        self.assertEqual(
            swr.validate_request(**{**ValidationTests.GOOD, "script_tf": token}).script_tf, token)

    def test_oversized_inputs_stop_the_run_before_any_feed_library_or_phase_work(self):
        for flag, option in (("--window-start-ms", "window_start_ms"),
                             ("--window-end-ms", "window_end_ms"),
                             ("--preroll-bars", "preroll_bars"),
                             ("--fed-start-ms", "fed_start_ms"),
                             ("--input-tf", "input_tf")):
            value = "1" + "0" * 5000 if flag == "--input-tf" else self.HUGE
            with self.subTest(flag=flag), Counters() as poison:
                status, out, _ = run_main(self.argv_with(flag, value))
                counts = poison.counts()
            self.assertEqual(status, 1)
            line = last_line(out)
            self.assertEqual((line["code"], line["args"]),
                             ("window_request_invalid", {"option": option}))
            self.assertEqual(set(counts.values()), {0})

    def test_an_oversized_primary_token_is_refused_by_admission_not_by_python(self):
        token = "1" + "0" * 5000
        with Counters() as poison:
            status, out, _ = run_main(self.argv_with("--script-tf", token))
            counts = poison.counts()
        self.assertEqual(status, 1)
        line = last_line(out)
        self.assertEqual((line["code"], line["args"]["capability"]),
                         ("window_mode_unsupported", "selected_window_chart_timeframe"))
        self.assertTrue(line["args"]["timeframe"].startswith("1000"))
        self.assertEqual(set(counts.values()), {0})

    def test_an_oversized_phase_descriptor_is_a_harness_fault_without_int(self):
        with Counters() as poison:
            status, out, _ = run_main(selected_argv(self.artifact, self.ohlcv, self.inventory,
                                                    extra=["--run-phase-fd", self.HUGE]))
            counts = poison.counts()
        self.assertEqual((status, last_line(out)["code"]), (1, "harness_internal_error"))
        self.assertEqual(set(counts.values()), {0})


ORIGINAL_LONG = (  # the long options of the command line before selected-window mode
    "--help", "--so", "--ohlcv", "--inputs", "--overrides", "--input-tf", "--script-tf",
    "--bar-magnifier", "--magnifier-samples", "--magnifier-dist", "--generated-cpp",
    "--transpiled", "--syminfo", "--trade-start-ms", "--chart-tz",
    "--magnifier-volume-weighted", "--bench", "--warmup", "--repeats", "--symbol-feeds",
    "--outputs")
NEW_LONG = (
    "--report-policy", "--window-start-ms", "--window-end-ms", "--preroll-bars",
    "--fed-start-ms", "--capabilities-json", "--validate-window-only", "--run-phase-fd",
    "--request-feed-inventory", "--report-shape", "--curve-point-budget", "--results-digest")


class AbbreviationTests(unittest.TestCase):
    """argparse accepts a prefix that exactly one long option begins with. The new flags made
    some formerly unique prefixes ambiguous (--r, --re, --rep for --repeats; --c for
    --chart-tz; --w for --warmup); the original command lines must keep working."""

    def canonical(self, *argv):
        return run_json._legacy_abbreviations(list(argv), ORIGINAL_LONG,
                                              ORIGINAL_LONG + NEW_LONG)

    def test_formerly_unique_prefixes_become_their_original_option(self):
        for prefix, option in (("--r", "--repeats"), ("--re", "--repeats"),
                               ("--rep", "--repeats"), ("--repe", "--repeats"),
                               ("--c", "--chart-tz"), ("--ch", "--chart-tz"),
                               ("--w", "--warmup"), ("--wa", "--warmup"),
                               ("--h", "--help"), ("--g", "--generated-cpp")):
            with self.subTest(prefix=prefix):
                self.assertEqual(self.canonical(prefix, "2"), [option, "2"])
                self.assertEqual(self.canonical(prefix + "=2"), [option + "=2"])  # = form

    def test_nothing_else_is_touched(self):
        untouched = (
            ["--repeats", "2"], ["--so", "x"], ["--chart-tz", "UTC"],            # full options
            ["--report-policy", "p"], ["--window-start-ms", "1"],                 # new full flags
            ["--capabilities-json"], ["--curve-point-budget=5"],
            ["--s", "1"], ["--i", "1"], ["--m", "1"], ["--o", "1"], ["--b", "1"],  # ambiguous
            ["--t", "1"], ["--input", "1"], ["--tra", "1"], ["--ma", "1"],         # before, still
            ["--report-p", "1"], ["--window-s", "1"], ["--va", "1"],               # new only
            ["--unknown", "1"], ["-r", "2"],                                        # not ours
            ["--inputs", '{"--r": 1}'], ["--inputs={\"--rep\": 1}"],                # values
            ["--overrides=--rep"],                                                   # after =
            ["--", "--r"],                                                           # after --
        )
        for argv in untouched:
            with self.subTest(argv=argv):
                self.assertEqual(self.canonical(*argv), argv)

    def test_a_value_after_the_equals_sign_and_a_second_token_are_left_alone(self):
        self.assertEqual(self.canonical("--rep=--re", "--c=--c"), ["--repeats=--re", "--chart-tz=--c"])
        self.assertEqual(self.canonical("--inputs", "--r x"), ["--inputs", "--r x"])

    def test_no_argv_means_the_process_argv(self):
        with mock.patch.object(sys, "argv", ["run_json.py", "--rep", "3"]):
            self.assertEqual(run_json._legacy_abbreviations(None, ORIGINAL_LONG,
                                                            ORIGINAL_LONG + NEW_LONG),
                             ["--repeats", "3"])

    def bench(self, *extra):
        ohlcv = Path(self.dir.name) / "feed.csv"
        write_ohlcv(ohlcv, csv_rows(10))
        lib = FakeLib(full_plan())
        runs = []

        def runner(*args):
            runs.append(args)
            OrdinaryAndCapabilityTests._fill(args[-1]._obj)

        lib.run_backtest_full = runner
        lib.strategy_set_chart_timezone = lambda state, tz: lib.calls.append(("chart_tz", tz))
        with mock.patch.object(run_json, "load_strategy", lambda p: lib), \
                mock.patch.object(run_json, "normalize_release_provenance", lambda p, c, r, k: p):
            status, out, _ = run_main(["--so", "x.so", "--ohlcv", str(ohlcv), "--input-tf", "1",
                                       "--script-tf", "1", "--bench", *extra])
        return status, json.loads(out), runs, lib

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def test_original_abbreviated_command_lines_still_run_as_they_did(self):
        for extra in (["--warmup", "0", "--repeats", "2"], ["--w", "0", "--rep", "2"],
                      ["--w=0", "--re=2"], ["--wa", "0", "--r", "2"]):
            with self.subTest(extra=extra):
                status, report, runs, _ = self.bench(*extra)
                self.assertEqual(status, 0)
                timing = report["diagnostics"]["timing"]
                self.assertEqual((timing["warmup"], timing["repeats"]), (0, 2))
                self.assertEqual(len(runs), 3)  # no warm-up, two timed runs, the body run

    def test_the_formerly_unique_chart_timezone_prefix_still_sets_the_timezone(self):
        for extra in (["--c", "UTC"], ["--c=UTC"], ["--ch", "UTC"], ["--chart-tz", "UTC"]):
            with self.subTest(extra=extra):
                status, report, _, lib = self.bench("--warmup", "0", "--repeats", "1", *extra)
                self.assertEqual(status, 0)
                self.assertIn(("chart_tz", b"UTC"), lib.calls)
                self.assertEqual(report["applied_runtime"]["chart_tz"], "UTC")

    def test_ambiguous_old_prefixes_are_still_refused_by_argparse(self):
        for prefix in ("--s", "--i", "--m", "--b", "--t", "--o", "--input", "--tra"):
            with self.subTest(prefix=prefix):
                out = io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                        self.assertRaises(SystemExit) as caught:
                    run_json.main(["--so", "x.so", "--ohlcv", "y.csv", prefix, "1"])
                self.assertEqual(caught.exception.code, 2)
                line = last_line(out.getvalue())
                self.assertEqual(line["code"], "run_request_invalid")
                self.assertIn("ambiguous", line["error"])

    def test_the_new_full_flags_work_normally_beside_the_old_prefixes(self):
        with Counters() as poison:
            status, out, _ = run_main(["--so", "x.so", "--ohlcv", "y.csv", "--rep", "2",
                                       "--results-digest", "sha256/v1"])
            counts = poison.counts()
        self.assertEqual(status, 1)
        self.assertEqual(last_line(out)["code"], "results_digest_unsupported")
        self.assertEqual(set(counts.values()), {0})


class SourceShapeTests(unittest.TestCase):
    """Static reads of the orchestration: an ordinary run imports none of the new modules, and
    run_json never opens the planner from a strategy path or a search."""

    def test_new_siblings_are_imported_only_inside_functions(self):
        import ast
        tree = ast.parse((ROOT / "docker" / "run_json.py").read_text())
        new = {"selected_window_report", "selected_window_plan", "request_feed_inventory",
               "run_phase_transport", "run_execution_observer"}
        top_level = {alias.name for node in tree.body if isinstance(node, ast.Import)
                     for alias in node.names}
        self.assertEqual(top_level & new, set())

    def test_the_report_module_does_not_import_run_json(self):
        import ast
        tree = ast.parse((ROOT / "docker" / "selected_window_report.py").read_text())
        imported = {alias.name for node in ast.walk(tree) if isinstance(node, ast.Import)
                    for alias in node.names}
        self.assertNotIn("run_json", imported)

    def test_the_lookback_triple_is_written_in_one_private_producer_the_builder_calls(self):
        import ast
        source = (ROOT / "docker" / "selected_window_report.py").read_text()
        tree = ast.parse(source)
        functions = {node.name: node for node in ast.walk(tree)
                     if isinstance(node, ast.FunctionDef)}

        def writes_the_member(node):
            return (isinstance(node, ast.Dict)
                    and any(isinstance(key, ast.Constant) and key.value == "has_unbounded_state"
                            for key in node.keys))

        literals = [node for node in ast.walk(tree) if writes_the_member(node)]
        self.assertEqual(len(literals), 1)  # the triple is written in exactly one place ...
        producer = functions["_lookback_not_produced"]  # ... a private function ...
        self.assertIn(literals[0], list(ast.walk(producer)))
        called = {node.func.id for node in ast.walk(functions["build_report_window"])
                  if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)}
        self.assertIn("_lookback_not_produced", called)  # ... which the builder calls
        # No boolean is ever written for the member by this module: it has no producer.
        self.assertNotIn('"has_unbounded_state": True', source)
        self.assertNotIn('"has_unbounded_state": False', source)

    def test_the_contract_revision_is_one_constant_with_one_literal_read_in_one_place(self):
        # The parity declaration ("1.3-parity") is the single switchable place
        # for a future full build ("1.3"): one tuple, one string literal, one reader, and no
        # revision literal in run_json (so none in a request, report, phase or fingerprint).
        import ast
        report_tree = ast.parse((ROOT / "docker" / "selected_window_report.py").read_text())

        def string_constants(tree):
            return [node.value for node in ast.walk(tree)
                    if isinstance(node, ast.Constant) and isinstance(node.value, str)]

        strings = string_constants(report_tree)
        self.assertEqual(strings.count("1.3-parity"), 1)     # written once ...
        self.assertNotIn("1.3", strings)                     # ... and never the full revision
        assignments = [node for node in report_tree.body if isinstance(node, ast.Assign)
                       and any(isinstance(target, ast.Name) and target.id == "CONTRACT_REVISIONS"
                               for target in node.targets)]
        self.assertEqual(len(assignments), 1)
        self.assertIsInstance(assignments[0].value, ast.Tuple)
        self.assertEqual(string_constants(assignments[0].value), ["1.3-parity"])
        readers = [function.name for function in ast.walk(report_tree)
                   if isinstance(function, ast.FunctionDef)
                   and any(isinstance(name, ast.Name) and name.id == "CONTRACT_REVISIONS"
                           for name in ast.walk(function))]
        self.assertEqual(readers, ["capabilities_record"])   # ... and read in one place
        run_json_strings = set(string_constants(
            ast.parse((ROOT / "docker" / "run_json.py").read_text())))
        self.assertEqual(run_json_strings & {"1.3", "1.3-parity"}, set())


if __name__ == "__main__":
    unittest.main()
