"""Opt-in scalar documents, optional ABI binding and whole-run recovery.

Reuse the sink tests' native-call stand-in and real main/reader/writer helpers.
These rows prove Python control flow, not native admission or RSS reduction.
"""
import contextlib
import ctypes
import io
import json
import math
from pathlib import Path
import struct
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_sink as ordinary
from test_run_json_sink import frozen  # noqa: F401


SETTER = "strategy_set_metrics_streaming_v1"
GETTER = "strategy_metrics_extent_receipt_v1"


class MetricsLibrary(ordinary.SinkLibrary):
    """Extend the existing fake library with independently controlled receipts."""
    def __init__(self, *, routes=(2,), receipt_fault=None, receipt_edit=None,
                 failures=(), no_curve=False, cagr=0.25, after_run=None,
                 bad_report=None, arena_stats=None, initial_capital=None,
                 curve_from_feed=False, calmar=-0.):
        super().__init__(arena_stats=arena_stats)
        self.routes, self.receipt_fault, self.receipt_edit = routes, receipt_fault, receipt_edit
        self.failures, self.no_curve, self.cagr = failures, no_curve, cagr
        self.after_run, self.bad_report = after_run, bad_report
        self.curve_from_feed = curve_from_feed
        self.calmar = calmar
        self.last_equities = {}
        self.created, self.live, self.events = [], set(), []
        self.preferences, self.config, self.extents, self.errors = [], {}, {}, {}
        self.executions, self.receipts, self.settings = [], [], []
        self.report_states, self.report_freed = {}, set()
        self.strategy_create = self.export("strategy_create", self.create)
        self.strategy_free = self.export("strategy_free", self.free)
        self.report_free = self.export("report_free", self.free_report)
        setattr(self, SETTER, self.export(SETTER, self.preference))
        setattr(self, GETTER, self.export(GETTER, self.metrics_receipt))
        if initial_capital is not None:
            self.strategy_initial_capital = self.export(
                "strategy_initial_capital", lambda st: initial_capital)
        self.strategy_get_last_error = self.export(
            "strategy_get_last_error", lambda st: self.errors.get(st, ("", "", {}))[0].encode())
        self.strategy_get_last_error_code = self.export(
            "strategy_get_last_error_code", lambda st: self.errors.get(st, ("", "", {}))[1].encode())
        self.strategy_get_last_error_args = self.export(
            "strategy_get_last_error_args", lambda st: json.dumps(self.errors.get(st, ("", "", {}))[2]).encode())
        self.strategy_last_run_status = self.export(
            "strategy_last_run_status", lambda st: int(st in self.errors))
        for name in ("strategy_set_input", "strategy_set_override",
                     "strategy_set_syminfo_mintick", "strategy_set_syminfo_pointvalue",
                     "strategy_set_syminfo_timezone", "strategy_set_syminfo_session",
                     "strategy_set_syminfo_metadata", "strategy_set_trade_start_time",
                     "strategy_set_chart_timezone", "strategy_set_magnifier_volume_weighted"):
            setattr(self, name, self.export(
                name, lambda st, *values, name=name: self.settings.append((st, name, values))))
        self.strategy_set_scratch_tables_v1 = self.export(
            "strategy_set_scratch_tables_v1", lambda *args: 0)
        self.strategy_require_report_scratch_arena_v1 = self.export(
            "strategy_require_report_scratch_arena_v1", lambda *args: 0)

    def create(self, _params):
        assert not self.live, "recovery must release the first strategy before creating another"
        state = 100 + len(self.created)
        self.created.append(state)
        self.live.add(state)
        self.events.append(("create", state))
        return state

    def preference(self, state, mode, mask):
        self.preferences.append((state, mode, mask))
        self.config[state] = mode, mask
        return 0

    def run(self, state, feed, count, *args):
        super().run(state, feed, count, *args)
        report = args[-1]._obj
        self.report_states[id(report)] = state
        self.extents[state] = count, int(feed[0].timestamp), int(feed[count - 1].timestamp)
        if self.curve_from_feed:
            for i in range(report.equity_curve_len):
                report.equity_curve[i] = run_json.EquityPointC(
                    int(feed[i].timestamp), float(feed[i].close), -0.)
        self.last_equities[state] = (
            report.equity_curve[report.equity_curve_len - 1].equity
            if report.equity_curve_len else None)
        self.executions.append((state, ctypes.addressof(feed), id(report)))
        self.events.append(("run", state))
        report.metrics.equity.cagr, report.metrics.equity.calmar = self.cagr, self.calmar
        report.metrics.equity.sharpe_bar, report.metrics.equity.sortino_bar = 1.125, 2.75
        report.metrics.equity.sharpe_monthly, report.metrics.equity.sortino_monthly = 3.25, 4.5
        report.metrics.all.num_trades = len(self.pnls)
        report.metrics.all.net_profit = sum(self.pnls)
        report.metrics.longs.num_trades, report.metrics.shorts.num_trades = 2, 1
        attempt = len(self.executions) - 1
        if attempt < len(self.failures) and self.failures[attempt] is not None:
            self.errors[state] = self.failures[attempt]
        if self.routes[min(attempt, len(self.routes) - 1)] == 3:
            # The native contract deliberately supplies no authoritative report.
            ctypes.memset(ctypes.byref(report), 0, ctypes.sizeof(report))
        elif self.no_curve and self.config.get(state, (0, 0))[0] == 1:
            report.equity_curve = ctypes.POINTER(run_json.EquityPointC)()
            report.equity_curve_len = 0
        if self.bad_report == "trade":
            report.trades[0].entry_price = float("inf")
        elif self.bad_report == "equity":
            report.equity_curve[0].equity = float("inf")
        if self.after_run is not None:
            self.after_run(self, attempt)

    def metrics_receipt(self, state, pointer):
        out = pointer._obj
        assert (out.struct_size, out.version) == (248, 1)
        assert ctypes.string_at(ctypes.addressof(out) + 8, 240) == bytes(240)
        self.receipts.append(state)
        attempt = self.created.index(state)
        if attempt == 0 and self.receipt_fault == "status":
            return -2
        if attempt == 0 and self.receipt_fault == "exception":
            raise RuntimeError("receipt transport failed")
        count, first, last = self.extents[state]
        plan = out.extent_plan
        plan.extent = run_json.MetricsExtentV1(count, first, last, int(count == 0), 0)
        out.actual_extent = plan.extent
        if self.last_equities[state] is not None:
            out.actual_last_equity = self.last_equities[state]
            out.actual_last_equity_valid = 1
        plan.present = 1
        for i, name in enumerate(("attempt", "input_generation", "admitted_view", "spec_identity")):
            value = (1 << 40) + 17 * (i + 1) + (attempt if name == "attempt" else 0)
            setattr(plan, name, value)
            setattr(out, "actual_" + name, value)
        out.route = self.routes[min(attempt, len(self.routes) - 1)]
        out.route_reason = {1: 11, 2: 1, 3: 3}[out.route]
        out.streaming_sharpe, out.metrics_needed = self.config.get(state, (0, 0x7f))
        out.plain_subset, out.witness = 1, 1
        out.replay_attempts = int(out.route == 3)
        out.authoritative_attempt = 0 if out.route == 3 else plan.attempt
        out.stop_boundary.completed_marks = count
        out.stop_boundary.input_index, out.stop_boundary.kind = count - 1, 1
        if attempt == 0 and self.receipt_fault == "version":
            out.version = 2
        if attempt == 0 and self.receipt_fault == "size":
            out.struct_size -= 8
        if self.receipt_edit is not None:
            self.receipt_edit(out, attempt)
        return 0

    def free_report(self, pointer):
        state = self.report_states[id(pointer._obj)]
        assert state not in self.report_freed
        self.report_freed.add(state)
        self.events.append(("report_free", state))
        self.backing = []

    def free(self, state):
        if any(execution[0] == state for execution in self.executions):
            assert state in self.report_freed, "report cleanup must precede strategy cleanup"
        self.events.append(("strategy_free", state))
        self.live.remove(state)
        super().free(state)


def success(result):
    status, text, error, library = result
    assert (status, error) == (0, ""), text
    assert len(text.splitlines()) == 1
    return text, json.loads(text), library


def metrics_shape(document):
    # Independent contract projection of an executed ordinary producer.
    out = {key: value for key, value in document.items()
           if key not in ("trades", "equity_curve", "elapsed_seconds")}
    out["diagnostics"] = {key: value for key, value in document["diagnostics"].items()
                          if key not in ordinary.ARENA_DIAGNOSTICS}
    return out


def document_bytes(document):
    return (json.dumps(document, separators=(",", ":"), allow_nan=False) + "\n").encode()


@pytest.mark.parametrize("option,preference", [
    (["--metrics-only"], (1, 0x37)),
    (["--metrics-only=auto"], (1, 0x37)),
    (["--metrics-only", "auto"], (1, 0x37)),
    (["--metrics-only=legacy"], (0, 0x7f)),
    (["--metrics-only", "legacy"], (0, 0x7f)),
])
def test_option_parsing(tmp_path, monkeypatch, option, preference):
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    text, document, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(), extra=option))
    assert [row[1:] for row in library.preferences] == [preference]
    assert "trades" not in document and "equity_curve" not in document
    assert library.installed == []


@pytest.mark.parametrize("value", ["", "on", "AUTO", "false"])
def test_invalid_option_value_is_typed(tmp_path, monkeypatch, value):
    library = MetricsLibrary()
    status, document, error, _ = ordinary.bars.invoke(
        monkeypatch, "--ohlcv", "unread.csv", "--metrics-only=" + value, library=library)
    assert status == 2 and document["code"] == "run_request_invalid"
    assert document["args"] == {"option": "arguments"}
    assert "invalid choice" in error and library.calls == []


REFUSED = [
    ["--report-sink", "unread.sink"], ["--report-sink-fd", "4"],
    ["--report-sink-fd", "5"], ["--run-phase-fd", "4"],
    ["--report-policy", "selected-window/v1"], ["--window-start-ms", "0"],
    ["--window-end-ms", "60000"], ["--preroll-bars", "0"], ["--fed-start-ms", "0"],
    ["--request-feed-inventory", "unread.json"], ["--report-shape", "full/v1"],
    ["--curve-point-budget", "1"], ["--results-digest", "sha256"],
    ["--validate-window-only"], ["--capabilities-json"],
    ["--outputs"], ["--bench"], ["--symbol-feeds", "unread.json"],
    ["--report-sink", "unread.sink", "--report-sink-fd", "4"],
    ["--report-sink-fd", "4", "--run-phase-fd", "4"],
    ["--report-sink", "unread.sink", "--report-mode", "SUMMARY_ONLY"],
    ["--report-policy", "selected-window/v1", "--window-start-ms", "0",
     "--window-end-ms", "60000", "--preroll-bars", "0", "--fed-start-ms", "0"],
]


@pytest.mark.parametrize("mode", ["auto", "legacy"])
@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
@pytest.mark.parametrize("incompatible", REFUSED)
def test_incompatible_options_refused_before_loading(tmp_path, monkeypatch, mode, profile, incompatible):
    def no_feed(*args, **kwargs):
        pytest.fail("an incompatible request read its feed")
    monkeypatch.setattr(run_json, "load_bars", no_feed)
    status, text, error, library = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(),
        extra=["--metrics-only=" + mode, "--profile", profile, *incompatible])
    refusal = json.loads(text)
    assert (status, error) == (2, ""), text
    assert refusal["code"] == "run_request_invalid"
    assert refusal["args"] == {"option": "arguments"}
    assert "--metrics-only cannot be combined with " + incompatible[0] in refusal["error"]
    assert len(text.splitlines()) == 1 and library.calls == []


@pytest.mark.parametrize("mode", ["auto", "legacy"])
@pytest.mark.parametrize("sink_api", [False, True])
@pytest.mark.parametrize("profile", ["unjailed", "jailed"])
@pytest.mark.parametrize("stats", [None, (1 << 33, 1 << 35)])
def test_document_shape_order_and_shared_bytes(tmp_path, monkeypatch, mode, sink_api, profile, stats):
    def library():
        lib = MetricsLibrary(arena_stats=stats)
        return lib if sink_api else ordinary.without_report_sink(lib)
    _, ordinary_document, original = success(ordinary.invoke(
        tmp_path, monkeypatch, library=library(), extra=["--profile", profile]))
    text, document, scalar = success(ordinary.invoke(
        tmp_path, monkeypatch, library=library(),
        extra=["--profile", profile, "--metrics-only=" + mode]))
    expected = metrics_shape(ordinary_document)
    assert list(document) == list(expected)
    assert list(document["diagnostics"]) == list(expected["diagnostics"])
    assert text.encode() == document_bytes(expected)
    assert document["diagnostics"]["annualization"] == ordinary_document["diagnostics"]["annualization"]
    assert document["fingerprint"] is not None
    assert scalar.installed == [] and scalar.queried == 0
    assert bool(original.installed) == (sink_api and profile == "unjailed")
    assert scalar.live == set()


def test_auto_requires_module_capability(tmp_path, monkeypatch):
    # Named mutation control: ignoring this false probe must turn the row red.
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: False)
    _, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(1,)),
        extra=["--metrics-only=auto"]))
    assert [row[1:] for row in library.preferences] == [(0, 0x7f)]
    assert library.receipts == [] and len(library.executions) == 1


@pytest.mark.parametrize("status", [-1, -2, -3])
def test_rejected_streaming_preference_reverts_to_actual_curve(tmp_path, monkeypatch, status):
    expected, _, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(), extra=["--metrics-only=legacy"]))
    library = MetricsLibrary()
    def prefer(state, mode, mask):
        library.preference(state, mode, mask)
        return status if mode == 1 else 0
    setattr(library, SETTER, library.export(SETTER, prefer))
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=library, extra=["--metrics-only"]))
    assert actual.encode() == expected.encode()
    assert [row[1:] for row in library.preferences] == [(1, 0x37), (0, 0x7f)]
    assert len(library.executions) == 1 and library.receipts == []


@pytest.mark.parametrize("missing", [(), (SETTER,), (GETTER,), (SETTER, GETTER)])
def test_optional_binders_registered_by_load_strategy(monkeypatch, missing):
    library = MetricsLibrary()
    for name in missing:
        delattr(library, name)
    monkeypatch.setattr(run_json.ctypes, "CDLL", lambda path: library)
    loaded = run_json.load_strategy(Path("unused.so"))
    assert loaded is library
    expected = {
        SETTER: [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64],
        GETTER: [ctypes.c_void_p, ctypes.POINTER(run_json.MetricsExtentReceiptV1)],
    }
    for name, args in expected.items():
        if name not in missing:
            assert getattr(loaded, name).argtypes == args
            assert getattr(loaded, name).restype is ctypes.c_int32


@pytest.mark.parametrize("missing", [(SETTER,), (GETTER,), (SETTER, GETTER)])
def test_auto_missing_binding_keeps_legacy_curve(tmp_path, monkeypatch, missing):
    library = MetricsLibrary()
    for name in missing:
        delattr(library, name)
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    _, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=library, extra=["--metrics-only"]))
    assert all(mode == 0 and mask == 0x7f for _, mode, mask in library.preferences)
    assert len(library.executions) == 1 and library.receipts == []


def test_native_receipt_layout_and_fixed_width_words():
    assert [ctypes.sizeof(pod) for pod in (
        run_json.MetricsExtentV1, run_json.MetricsExtentPlanV1,
        run_json.MetricsStopBoundaryV1, run_json.MetricsExtentReceiptV1)] == [32, 72, 24, 248]
    payload = bytearray(248)
    struct.pack_into("=II", payload, 0, 248, 1)
    struct.pack_into("=qqqII", payload, 8, 9, -17, 111, 0, 0)
    struct.pack_into("=QQQQII", payload, 40, 2**40 + 1, 2**41 + 2, 2**42 + 3, 2**43 + 4, 1, 0)
    struct.pack_into("=qqqII", payload, 80, 8, -19, 109, 0, 0)
    struct.pack_into("=8Q", payload, 112, *(2**44 + i for i in range(8)))
    struct.pack_into("=qqII", payload, 176, 7, -3, 2, 0)
    struct.pack_into("=6IQ", payload, 200, 1, 3, 11, 1, 1, 2, 2**52 + 7)
    struct.pack_into("=dI", payload, 232, -12345.625, 1)
    receipt = run_json.MetricsExtentReceiptV1.from_buffer(payload)
    assert (receipt.extent_plan.extent.count, receipt.extent_plan.extent.first_time_ms) == (9, -17)
    assert receipt.extent_plan.spec_identity == 2**43 + 4
    assert receipt.actual_extent.last_time_ms == 109
    assert receipt.metrics_needed == 2**44 + 7
    assert (receipt.stop_boundary.input_index, receipt.route, receipt.replay_attempts) == (-3, 3, 2)
    assert receipt.authoritative_attempt == 2**52 + 7
    assert receipt.actual_last_equity == struct.unpack_from("=d", payload, 232)[0]
    assert receipt.actual_last_equity_valid == 1
    assert run_json.MetricsExtentReceiptV1.actual_last_equity.offset == 232
    assert run_json.MetricsExtentReceiptV1.actual_last_equity_valid.offset == 240
    receipt.actual_last_equity_valid = 2**32 - 1
    assert struct.unpack_from("=I", payload, 240)[0] == 2**32 - 1
    receipt.actual_attempt = 2**60 + 13
    assert struct.unpack_from("=Q", payload, 112)[0] == 2**60 + 13


def test_recovery_replays_whole_run_once_and_cleans_first(tmp_path, monkeypatch):
    control, _, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(), extra=["--metrics-only=legacy"]))
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(3, 2)), extra=["--metrics-only"]))
    assert actual.encode() == control.encode()
    assert len(library.executions) == 2 and len(library.receipts) == 2
    first, second = library.created
    assert first != second
    assert library.events == [
        ("create", first), ("run", first), ("report_free", first), ("strategy_free", first),
        ("create", second), ("run", second), ("report_free", second), ("strategy_free", second)]
    assert [row[1:] for row in library.preferences] == [(1, 0x37), (0, 0x7f)]
    assert library.rows[0] == library.rows[1] and library.rows[0]
    assert library.executions[0][1] == library.executions[1][1]
    assert library.executions[0][2] != library.executions[1][2]
    assert library.live == set() and library.installed == []


@pytest.mark.parametrize("fault", ["status", "exception", "version", "size"])
def test_unavailable_receipt_recovers_instead_of_publishing_partial(tmp_path, monkeypatch, fault):
    expected, _, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(), extra=["--metrics-only=legacy"]))
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(3, 2), receipt_fault=fault),
        extra=["--metrics-only"]))
    assert actual.encode() == expected.encode() and len(library.executions) == 2


@pytest.mark.parametrize("capital", [None, 0., -1., float("nan"), float("inf"), -float("inf")])
def test_missing_or_invalid_annualization_capital_uses_actual_curve(tmp_path, monkeypatch, capital):
    expected, document, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(initial_capital=capital),
        extra=["--metrics-only=legacy"]))
    assert "annualization" in document["diagnostics"]
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(
            routes=(1, 2), no_curve=True, initial_capital=capital),
        extra=["--metrics-only"]))
    assert actual.encode() == expected.encode() and len(library.executions) == 2


@pytest.mark.parametrize("field,value", [
    ("actual_last_equity_valid", 0), ("actual_last_equity_valid", 2),
    ("actual_last_equity_valid", 2**32 - 1),
    ("actual_last_equity", float("nan")), ("actual_last_equity", float("inf")),
])
def test_invalid_final_mark_uses_actual_curve(tmp_path, monkeypatch, field, value):
    capital = 10000.
    expected, _, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(initial_capital=capital),
        extra=["--metrics-only=legacy"]))
    def corrupt(receipt, attempt):
        if attempt == 0:
            setattr(receipt, field, value)
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(
            routes=(1, 2), no_curve=True, initial_capital=capital, receipt_edit=corrupt),
        extra=["--metrics-only"]))
    assert actual.encode() == expected.encode()
    assert len(library.executions) == 2
    assert [row[1:] for row in library.preferences] == [(1, 0x37), (0, 0x7f)]


@pytest.mark.parametrize("span,last_equity,overflow", [
    (60000, 10005., False), (2 * 86400000, 10005., False), (86400000, 80000., True),
])
@pytest.mark.parametrize("calmar", [None, -0.])
def test_finite_annualization_uses_observed_endpoints_without_replay(
        tmp_path, monkeypatch, span, last_equity, overflow, calmar):
    capital = 10000.
    def chart(directory):
        path = directory / "annualization.csv"
        times = [ordinary.bars.T0, ordinary.bars.T0 + span // 2, ordinary.bars.T0 + span]
        prices = [capital, capital - 1., last_equity]
        path.write_text("timestamp,open,high,low,close,volume\n" + "".join(
            f"{timestamp},{price},{price},{price},{price},1\n"
            for timestamp, price in zip(times, prices)))
        return path
    monkeypatch.setattr(ordinary.bars, "csv_file", chart)
    def library(**kwargs):
        return MetricsLibrary(initial_capital=capital, curve_from_feed=True,
                              calmar=float("nan") if calmar is None else calmar, **kwargs)
    legacy_text, legacy, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=library(), extra=["--metrics-only=legacy"]))
    # Also observe the actual curve in an ordinary independent execution.
    _, ordinary_document, control = success(ordinary.invoke(
        tmp_path, monkeypatch, library=ordinary.without_report_sink(library())))
    curve = ordinary_document["equity_curve"]
    assert curve[-1]["equity"] == control.last_equities[control.created[0]]
    assert curve[-1]["equity"] != capital + ordinary_document["summary"]["net_pnl"]
    if overflow:
        # This input really overflows the compounded form; no math helper is mocked.
        years = float(curve[-1]["time_ms"] - curve[0]["time_ms"]) / (365.25 * 86400. * 1000.)
        with pytest.raises(OverflowError):
            math.pow(curve[-1]["equity"] / capital, 1. / years)
        assert "annualization" in legacy["diagnostics"]
    elif span < run_json.MIN_ANNUALIZATION_SPAN_MS:
        assert "annualization" in legacy["diagnostics"]
    else:
        assert "annualization" not in legacy["diagnostics"]
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual_text, actual, streamed = success(ordinary.invoke(
        tmp_path, monkeypatch, library=library(routes=(1,), no_curve=True),
        extra=["--metrics-only=auto"]))
    assert math.isfinite(actual["metrics"]["equity"]["cagr"])
    assert actual["diagnostics"].get("annualization") == legacy["diagnostics"].get("annualization")
    assert actual_text.encode() == legacy_text.encode()
    assert actual_text.encode() == document_bytes(metrics_shape(ordinary_document))
    assert len(streamed.executions) == 1 and streamed.receipts == streamed.created
    assert streamed.last_equities[streamed.created[0]] == curve[-1]["equity"]


def test_valid_streaming_receipt_without_endpoint_need_does_not_replay(tmp_path, monkeypatch):
    expected, _, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(cagr=float("nan")),
        extra=["--metrics-only=legacy"]))
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(1,), no_curve=True, cagr=float("nan")),
        extra=["--metrics-only"]))
    assert actual.encode() == expected.encode()
    assert len(library.executions) == 1 and library.receipts == library.created


@pytest.mark.parametrize("field", ["metrics_needed", "plain_subset", "witness",
                                  "failed_plain_inputs", "actual_attempt", "mismatch_fields"])
def test_unproven_streaming_receipt_is_not_published(tmp_path, monkeypatch, field):
    def corrupt(receipt, attempt):
        if attempt == 0:
            setattr(receipt, field, getattr(receipt, field) ^ 1)
    expected, _, _ = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(cagr=float("nan")),
        extra=["--metrics-only=legacy"]))
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(
            routes=(1, 2), no_curve=True, cagr=float("nan"), receipt_edit=corrupt),
        extra=["--metrics-only"]))
    assert actual.encode() == expected.encode() and len(library.executions) == 2


def test_recovery_reuses_frozen_settings_syminfo_source_and_chart(tmp_path, monkeypatch):
    syminfo, cpp = tmp_path / "syminfo.json", tmp_path / "generated.cpp"
    syminfo.write_text(json.dumps({"mincontract": 0.25, "mintick": 0.5,
                                  "pointvalue": 2, "timezone": "UTC", "session": "24x7"}))
    cpp.write_bytes(b"// fixed source\r\n")
    options = ["--syminfo", str(syminfo), "--generated-cpp", str(cpp),
               "--inputs", '{"Length":"8"}', "--overrides", '{"initial_capital":"10000"}',
               "--trade-start-ms", str(ordinary.bars.T0), "--chart-tz", "UTC",
               "--bar-magnifier", "true", "--magnifier-volume-weighted"]
    expected, _, control = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(),
        extra=[*options, "--metrics-only=legacy"]))
    def mutate_paths(library, attempt):
        if attempt == 0:
            syminfo.write_text("not JSON anymore")
            cpp.write_text("// replaced source\n")
            (tmp_path / "chart.csv").write_text("not CSV anymore")
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    actual, document, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(3, 2), after_run=mutate_paths),
        extra=[*options, "--metrics-only"]))
    assert actual.encode() == expected.encode()
    expected_settings = [(name, values) for _, name, values in control.settings]
    for state in library.created:
        assert [(name, values) for st, name, values in library.settings if st == state] == expected_settings
    assert library.rows == [control.rows[0], control.rows[0]]
    assert document["applied_runtime"]["syminfo"] == {"qty_step": 0.25, "mincontract": 0.25}


def test_recovery_is_bounded_to_one_retry(tmp_path, monkeypatch):
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    status, text, error, library = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(3, 3)), extra=["--metrics-only"])
    assert (status, error) == (1, "")
    assert json.loads(text)["code"] == "engine_invariant"
    assert len(text.splitlines()) == 1 and len(library.executions) == 2
    assert library.live == set()


@pytest.mark.parametrize("second", ["same", "success", "different", "unwitnessed"])
def test_failed_attempt_never_turns_into_success(tmp_path, monkeypatch, second):
    original = ("fixture deterministic stop", "strategy_runtime_error", {})
    expected_status, expected, _, _ = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(failures=(original,)),
        extra=["--metrics-only=legacy"])
    replay = {"same": original, "success": None,
              "different": ("different stop", "strategy_runtime_error", {}),
              "unwitnessed": original}[second]
    def boundary(receipt, attempt):
        if second == "unwitnessed" and attempt == 1:
            receipt.stop_boundary.kind = 0
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    status, actual, error, library = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(
            routes=(3, 2), failures=(original, replay), receipt_edit=boundary),
        extra=["--metrics-only"])
    assert status == expected_status == 1 and error == ""
    assert actual.encode() == expected.encode()
    assert len(library.executions) == 2 and library.live == set()


@pytest.mark.parametrize("failure", [
    ("scratch failed", "report_sink_failed", {"reason": "arena_exhausted",
                                             "high_water_bytes": 1024, "arena_bytes": 1024}),
    ("native run aborted", "run_aborted", {"bars_count": 3, "strategy_started": True}),
])
def test_resource_failure_and_cancellation_do_not_retry_or_finalize(tmp_path, monkeypatch, failure):
    expected_status, expected, _, _ = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(failures=(failure,)),
        extra=["--metrics-only=legacy"])
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    status, actual, error, library = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(3, 2), failures=(failure,)),
        extra=["--metrics-only"])
    assert status == expected_status == 1 and error == ""
    assert actual.encode() == expected.encode()
    assert library.receipts == [] and len(library.executions) == 1


def test_receipt_finalization_resource_failure_keeps_native_error(tmp_path, monkeypatch):
    failure = ("scratch finalization failed", "report_sink_failed", {"reason": "scratch_io"})
    expected_status, expected, _, _ = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(failures=(failure,)),
        extra=["--metrics-only=legacy"])
    library = MetricsLibrary(routes=(3, 2))
    def fail_receipt(receipt, attempt):
        library.errors[library.created[attempt]] = failure
    library.receipt_edit = fail_receipt
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    status, actual, error, library = ordinary.invoke(
        tmp_path, monkeypatch, library=library, extra=["--metrics-only"])
    assert status == expected_status == 1 and error == ""
    assert actual.encode() == expected.encode() and len(library.executions) == 1
    assert len(library.receipts) == 1 and library.live == set()


def test_authoritative_legacy_failure_uses_ordinary_writer(tmp_path, monkeypatch):
    failure = ("legacy execution failed", "strategy_runtime_error", {})
    expected_status, expected, _, _ = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(failures=(failure,)),
        extra=["--metrics-only=legacy"])
    monkeypatch.setattr(run_json, "_metrics_only_supported", lambda lib: True)
    status, actual, error, library = ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(routes=(3, 2), failures=(None, failure)),
        extra=["--metrics-only"])
    assert status == expected_status == 1 and error == ""
    assert actual.encode() == expected.encode() and len(library.executions) == 2
    assert library.live == set()


@pytest.mark.parametrize("mode", ["auto", "legacy"])
@pytest.mark.parametrize("bad_report", ["trade", "equity"])
def test_omitted_arrays_are_validated_before_projection(tmp_path, monkeypatch, mode, bad_report):
    def library():
        return ordinary.without_report_sink(MetricsLibrary(bad_report=bad_report))
    baseline = ordinary.invoke(tmp_path, monkeypatch, library=library())
    actual = ordinary.invoke(
        tmp_path, monkeypatch, library=library(), extra=["--metrics-only=" + mode])
    assert baseline[0] == actual[0] == 1
    assert baseline[1].encode() == actual[1].encode()
    assert json.loads(actual[1])["code"] == "report_result_invalid"


@pytest.mark.parametrize("sink_api", [False, True])
def test_without_option_matches_frozen_ordinary_bytes(tmp_path, monkeypatch, frozen, sink_api):
    # The existing immutable oracle predates the two profile markers.
    # Supply only those already-established markers; leave its output intact.
    builder = frozen.build_report_dict
    def reference_builder(*args, **kwargs):
        document = builder(*args, **kwargs)
        document["diagnostics"]["run_profile"] = "unjailed"
        document["diagnostics"]["scratch_arena_jailed"] = False
        return document
    with monkeypatch.context() as patch:
        patch.setattr(frozen, "build_report_dict", reference_builder)
        expected, _, _ = success(ordinary.invoke(
            tmp_path, patch, runner=frozen,
            library=ordinary.without_report_sink(MetricsLibrary(cagr=float("nan")))))
    library = MetricsLibrary(cagr=float("nan"))
    if not sink_api:
        ordinary.without_report_sink(library)
    actual, document, library = success(ordinary.invoke(tmp_path, monkeypatch, library=library))
    assert actual.encode() == expected.encode()
    assert library.preferences == [] and library.receipts == []
    assert "trades" in document and "equity_curve" in document and "elapsed_seconds" in document


def test_existing_report_mode_prefix_is_preserved(tmp_path, monkeypatch):
    expected, _, _ = success(ordinary.invoke(tmp_path, monkeypatch, library=MetricsLibrary()))
    actual, _, library = success(ordinary.invoke(
        tmp_path, monkeypatch, library=MetricsLibrary(), extra=["--report-m", "FULL"]))
    assert actual.encode() == expected.encode() and library.preferences == []


def test_existing_ambiguous_prefix_keeps_its_error(tmp_path, monkeypatch, frozen):
    results = []
    for runner in (frozen, run_json):
        with monkeypatch.context() as patch:
            patch.setattr(ordinary.bars, "run_json", runner)
            status, document, error, library = ordinary.bars.invoke(
                patch, "--ohlcv", "unread.csv", "--m", "4", library=MetricsLibrary())
        assert status == 2 and library.calls == []
        results.append(document)
    # The two limit options added after the frozen runner also begin with "--m",
    # so they join the candidate list; everything else is unchanged.
    expected = dict(results[0])
    expected["error"] += ", --max-symbol-feeds, --max-inventory-bytes"
    assert results[1] == expected


@pytest.mark.parametrize("arguments", [["--m", "4"], ["--magnifier-samples", "invalid"]])
def test_absent_option_preserves_parser_error_bytes(arguments):
    observed = []
    for parser_type in (run_json._ArgumentParser, run_json._MetricsArgumentParser):
        parser = parser_type(prog="runner")
        parser.add_argument("--magnifier-samples", type=int, default=4)
        parser.add_argument("--magnifier-dist", default="endpoints")
        parser.add_argument("--magnifier-volume-weighted", action="store_true")
        if parser_type is run_json._MetricsArgumentParser:
            parser._legacy_usage = parser.format_usage()
            parser.add_argument("--metrics-only", nargs="?", const="auto", choices=("auto", "legacy"))
        out, error = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(error):
            with pytest.raises(SystemExit) as refused:
                parser.parse_args(arguments)
        assert refused.value.code == 2
        observed.append((out.getvalue().encode(), error.getvalue().encode()))
    assert observed[0] == observed[1]


@pytest.mark.parametrize("mode", ["auto", "legacy"])
def test_sink_factory_validates_explicit_requests_before_bypass(mode):
    for sink in ({"report_sink": Path("caller.sink")}, {"report_sink_fd": 4}):
        args = SimpleNamespace(metrics_only=mode, profile="unjailed", **sink)
        with pytest.raises(run_json.RunFailure) as refused:
            run_json._new_report_sink(MetricsLibrary(), 7, args)
        assert refused.value.code == "run_request_invalid" and refused.value.exit_status == 2


class _CapabilityExport:
    """A ctypes-like foreign function: records the declared types and returns a status."""

    def __init__(self, status=0, failure=None):
        self.status, self.failure = status, failure
        self.restype, self.argtypes = None, None

    def __call__(self):
        if self.failure is not None:
            raise self.failure
        return self.status


@pytest.mark.parametrize("status,expected", [(0, True), (-1, False), (1, False), (-2, False)])
def test_capability_probe_accepts_only_status_ok(status, expected):
    export = _CapabilityExport(status)
    lib = SimpleNamespace(strategy_metrics_only_capability_v1=export)
    assert run_json._metrics_only_supported(lib) is expected
    assert export.restype is ctypes.c_int32 and export.argtypes == []


def test_capability_probe_refuses_a_missing_or_failing_export():
    assert run_json._metrics_only_supported(SimpleNamespace()) is False
    failing = SimpleNamespace(
        strategy_metrics_only_capability_v1=_CapabilityExport(failure=OSError("probe failed")))
    assert run_json._metrics_only_supported(failing) is False
