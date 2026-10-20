"""EN-133: exercise the real report builder, main(), writers and failure handler.

The in-process engine and planner are the existing selected-window stand-ins;
the reports are real ReportC / TradeC allocations, and no serializer or failure
classifier is replaced. Summary fault injection happens AFTER the real builder
because avg/best/worst cannot independently be poisoned through ReportC without
also poisoning an earlier canonical leaf. This is report-producer evidence,
not evidence that the native engine produces these values.

The process rows require an actual native host strategy module. Its
strategy_create must create a C-ABI host that enters one unit on the first bar,
flattens on the second, and supplies on_lot_excursion. The input excursion_value
selects finite (favorable=1, adverse=0), nan or infinity (favorable only). The
module must expose the ordinary run_json strategy exports and execution observer.
Supply PINEFORGE_REPORT_NONFINITE_HOST_LIBRARY, or put it at the default path
below. An absent default fixture is an explicit skip, never a fake native run;
an explicitly configured missing fixture fails. EN-134 will require changing
the process expectation if it guards this native seam before report assembly.
"""

import contextlib
import ctypes
import io
import json
import math
import os
import socket
import subprocess
import sys
import types
from decimal import Decimal, localcontext
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json  # noqa: E402
import run_phase_transport as rpt  # noqa: E402
import selected_window_plan as swp  # noqa: E402
import selected_window_report as swr  # noqa: E402
import test_run_json_selected_window as selected  # noqa: E402

# Pinned from the EN-133 brief BEFORE any execution; never derived from output,
# the catalog or the implementation's validator.
RESULT_INVALID = "report_result_invalid"
POST_EXECUTION_FAILED = "report_post_execution_failed"
NONFINITE_REASON = "nonfinite"
TRADE_FIELDS = (
    "trades.entry_price", "trades.exit_price", "trades.qty", "trades.pnl",
    "trades.pnl_pct", "trades.max_runup", "trades.max_drawdown", "trades.commission",
)
SUMMARY_FIELDS = (
    "summary.win_rate_pct", "summary.net_pnl", "summary.avg_trade", "summary.best_trade",
    "summary.worst_trade", "summary.max_drawdown",
)
BAD_VALUES = (
    pytest.param(float("nan"), id="nan"),
    pytest.param(float("inf"), id="infinity"),
    pytest.param(float("-inf"), id="negative-infinity"),
)
MODES = ("ordinary", "selected")
# Pinned from the revision 3.1 rule of proportional annualisation BEFORE any execution; never
# derived from output, the .def or the implementation. A curve's cagr is the simple proportional
# form (ratio - 1) * (1 / span_years) * 100 under one day, and from one day on the compounded
# form (pow(ratio, 1 / span_years) - 1) * 100 unless that is not finite, then the simple one.
# The wire names a simple form under diagnostics.annualization.cagr. calmar is cagr over the
# maximum drawdown percent, so it carries the SAME string under diagnostics.annualization.calmar
# exactly when the cagr string is present and calmar is not null (null at zero drawdown, and
# then no calmar key); sharpe_bar and sortino_bar never have a reason.
ONE_DAY_MS = 86_400_000
YEAR_MS = 365.25 * 86400.0 * 1000.0
SUBDAY = "simple_proportional_subday"
OVERFLOW = "simple_proportional_overflow"
# The frozen producer: docker/run_json.py on engine main just before EN-133 (the file is
# byte-identical at the commit the change was authored on, 9da4c984, and here).
BASE_SOURCE = "7da487ab2907c428bdfc755e10452811f95beaf2:docker/run_json.py"


def reject_constant(token):
    raise AssertionError(f"non-JSON number token: {token}")


def strict_line(stdout):
    lines = stdout.splitlines()
    assert len(lines) == 1, stdout
    assert stdout.endswith("\n"), stdout
    return json.loads(lines[0], parse_constant=reject_constant)


def simple_cagr(span_ms, capital, final_equity):
    """The simple proportional cagr (percent per year), in the kernel's operation order."""
    span_years = span_ms / YEAR_MS
    return (final_equity / capital - 1.0) * (1.0 / span_years) * 100.0


def compounded_cagr(span_ms, capital, final_equity):
    """The compounded cagr, for the curves whose compounded form is a finite double."""
    span_years = span_ms / YEAR_MS
    return ((final_equity / capital) ** (1.0 / span_years) - 1.0) * 100.0


def max_drawdown_pct(equities):
    """The kernel's max_equity_drawdown_pct over equity marks: the largest drop below the running
    peak, as a percent of the peak in effect when it was hit; 0.0 for marks that never fall."""
    peak, worst, percent = equities[0], 0.0, 0.0
    for equity in equities:
        peak = max(peak, equity)
        if peak - equity > worst:
            worst, percent = peak - equity, (peak - equity) / peak * 100.0
    return percent


def kernel_equity_metrics(form, span_ms, capital, final_equity, dip=None):
    """What the kernel publishes for a curve of two marks, or of three when `dip` is the middle
    mark, from the rule alone: `form` is the cagr form the rule gives the curve ("simple",
    "compounded") or None where the existing domain leaves it absent (span <= 0, capital <= 0,
    final equity <= 0). calmar is cagr over the drawdown percent, absent at zero drawdown (a
    curve that never falls) and with an absent cagr. These short curves have fewer than three
    returns, so both bar ratios stay in their existing absence: no span is involved."""
    nan = float("nan")
    if form is None:
        return {"cagr": nan, "calmar": nan, "sharpe_bar": nan, "sortino_bar": nan}
    cagr = (simple_cagr if form == "simple" else compounded_cagr)(span_ms, capital, final_equity)
    drawdown_pct = max_drawdown_pct(
        [capital] + ([] if dip is None else [dip]) + [final_equity])
    return {"cagr": cagr, "max_equity_drawdown_pct": drawdown_pct,
            "calmar": cagr / drawdown_pct if drawdown_pct > 0.0 else nan,
            "sharpe_bar": nan, "sortino_bar": nan}


def metrics_of(cagr, calmar=None):
    """The report's metric blocks as build_report_dict hands them to the annualization writer,
    reduced to what it may read: the kernel's net_profit_pct (a defined number under a positive
    capital), cagr and calmar (None: the defined absence of a curve that never falls)."""
    return {"all": {"net_profit_pct": 0.0}, "equity": {"cagr": cagr, "calmar": calmar}}


def wire_document(marks, equity_metrics, entry_bars=(1,), positive_capital=True, **options):
    """The strict JSON wire of the real report builder for a ReportC holding the given
    (time_ms, equity, open_profit) marks, the kernel's equity metrics and one zeroed trade row
    per entry bar index. compute_trade_stats' existing domain makes net_profit_pct null iff the
    initial capital is not positive (positive_capital). options reach build_report_dict."""
    report = run_json.ReportC()
    report.metrics.all.net_profit_pct = 0.0 if positive_capital else float("nan")
    curve = (run_json.EquityPointC * len(marks))(*(run_json.EquityPointC(*m) for m in marks))
    report.equity_curve = curve
    report.equity_curve_len = len(marks)
    trades = (run_json.TradeC * len(entry_bars))()
    for i, bar in enumerate(entry_bars):
        trades[i].entry_bar_index = bar
    report.trades = trades
    report.trades_len = report.total_trades = len(entry_bars)
    for name, value in equity_metrics.items():
        setattr(report.metrics.equity, name, value)
    # Calendar-month ratios are already absent with fewer than two monthly returns; that
    # domain has no span and never receives a reason.
    report.metrics.equity.sharpe_monthly = report.metrics.equity.sortino_monthly = float("nan")
    document = run_json.build_report_dict(
        report, Path("annualization.csv"), len(marks), marks[0][0], marks[-1][0],
        0.0, {}, {}, {"chart_tz": "America/New_York"}, **options)
    run_json._validate_report_result(document)
    return json.loads(json.dumps(document, allow_nan=False))


def exact_overflow_ratio(span_ms, factor):
    """`factor` times the ratio at which the compounded form first overflows a double, in exact
    arithmetic from the compounded formula. (pow - 1) * 100 rounds to infinity from
    2**1024 - 2**970 on (half an ulp above the largest double), and pow - 1 is pow at that
    size, so the threshold ratio is that limit over 100, raised to span_years."""
    with localcontext() as context:
        context.prec = 80
        exponent = 1.0 / (span_ms / YEAR_MS)
        limit = (Decimal(2) ** 1024 - Decimal(2) ** 970) / 100
        return float(limit ** (Decimal(1) / Decimal(exponent)) * Decimal(factor))


class ReportLib(selected.FakeLib):
    """Only the execution stand-in: owns the backing arrays until cleanup. equity_metrics are
    the kernel's published equity fields; last_mark (time_ms, equity) replaces the curve's
    last point after the stand-in run built it, and dip replaces the equity of point 1 (the
    anchor is point 0 and every later point rises above it): a curve that falls once, from the
    first mark to dip, then climbs, so its maximum drawdown is the one from the first mark."""

    def __init__(self, plan, events, *, trade_field=None, bad_value=None,
                 net_profit=3.0, pnls=(10.0, -7.0), metric=None, curve_value=None,
                 failure_field=None, equity_metrics=None, last_mark=None, dip=None):
        engine_failure = None
        if failure_field is not None:
            engine_failure = (
                RESULT_INVALID, "The report producer generated an invalid canonical result.")
        super().__init__(plan, events, engine_failure=engine_failure)
        if failure_field is not None:
            self.strategy_get_last_error_args = lambda state: json.dumps(
                {"field": failure_field, "reason": NONFINITE_REASON}).encode()
        self.trade_field = trade_field
        self.bad_value = bad_value
        self.net_profit = net_profit
        self.pnls = pnls
        self.metric = metric
        self.curve_value = curve_value
        self.equity_metrics = equity_metrics
        self.last_mark = last_mark
        self.dip = dip
        self._trades = None

    def _run(self, *args):
        super()._run(*args)  # real observer binding and phase handover
        report = args[-1]._obj
        self._trades = (run_json.TradeC * len(self.pnls))()
        for i, pnl in enumerate(self.pnls):
            trade = self._trades[i]
            trade.entry_time = selected.T + i * selected.MINUTE
            trade.exit_time = trade.entry_time + selected.MINUTE
            trade.entry_price = 100.0 + i
            trade.exit_price = 101.0 + i
            trade.qty = 2.0
            trade.pnl = pnl
            trade.pnl_pct = 5.0 if pnl > 0 else -3.5
            trade.max_runup = 12.0
            trade.max_drawdown = -8.0
            trade.commission = 0.5
            trade.is_long = int(i == 0)
            trade.entry_bar_index = i
            trade.exit_bar_index = i + 1
        if self.trade_field is not None:
            # Poison the LAST row: checking only the first trade must fail.
            setattr(self._trades[-1], self.trade_field, self.bad_value)
        report.trades = self._trades
        report.trades_len = report.total_trades = len(self.pnls)
        report.net_profit = self.net_profit
        report.metrics.all.profit_factor = 2.0
        report.metrics.equity.sharpe_monthly = 0.5
        if self.last_mark is not None:
            self._curve[-1].time_ms, self._curve[-1].equity = self.last_mark
        if self.dip is not None:
            self._curve[1].equity = self.dip
        for name, value in (self.equity_metrics or {}).items():
            setattr(report.metrics.equity, name, value)
        if self.metric is not None:
            block, name = self.metric
            setattr(getattr(report.metrics, block), name, self.bad_value)
        if self.curve_value is not None:
            # Preserve the selected anchor; poison an ordinary curve point.
            self._curve[-1].equity = self.curve_value
            self._curve[-1].open_profit = self.curve_value


class Harness:
    def __init__(self, path):
        self.ohlcv = path / "feed.csv"
        selected.write_ohlcv(self.ohlcv, selected.csv_rows(10))
        self.artifact, self.inventory = selected.write_inventory(path, selected.INSIDE)

    def run(self, mode, *, runner=run_json, phase_fd=True, inject=None,
            extra=(), document_patch=None, **report_options):
        events, built = [], []
        plan = (selected.full_plan() if mode == "selected" else selected.full_plan(
            fed_input_bars=10, fed_script_bars=10, window_script_bars=10))
        library = ReportLib(plan, events, **report_options)
        selected.FakePlanner.reset(plan)
        real_build = runner.build_report_dict

        def build(*args, **kwargs):
            assert isinstance(args[0], run_json.ReportC)
            document = real_build(*args, **kwargs)
            if inject is not None:
                inject(document)
            built.append(document)
            return document

        writer_type = rpt.RunPhaseWriter

        class FixedWriter(writer_type):
            def __init__(self, fd):
                super().__init__(fd, clock=lambda: 10.0)

            def advance(self, phase):
                events.append("advance:" + phase)
                return super().advance(phase)

        parent, child = socket.socketpair()
        try:
            if mode == "selected":
                argv = selected.selected_argv(self.artifact, self.ohlcv, self.inventory)
            else:
                argv = ["--so", str(self.artifact), "--ohlcv", str(self.ohlcv),
                        "--input-tf", "1", "--script-tf", "1"]
            if phase_fd:
                argv += ["--run-phase-fd", str(child.fileno())]
            argv += list(extra)
            stdout, stderr = selected.Stdout(events), io.StringIO()
            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(runner, "load_strategy", return_value=library))
                stack.enter_context(mock.patch.object(swp, "SelectedPrimaryPlanner", selected.FakePlanner))
                stack.enter_context(mock.patch.object(
                    runner, "normalize_release_provenance", lambda p, c, r, k: p))
                stack.enter_context(mock.patch.object(runner, "build_report_dict", side_effect=build))
                stack.enter_context(mock.patch.object(runner.time, "time", return_value=10.0))
                stack.enter_context(mock.patch.object(rpt, "RunPhaseWriter", FixedWriter))
                if document_patch is not None:
                    stack.enter_context(document_patch)
                stack.enter_context(contextlib.redirect_stdout(stdout))
                stack.enter_context(contextlib.redirect_stderr(stderr))
                status = runner.main(argv)
            records = selected.phase_records(parent)
        finally:
            child.close()
            parent.close()
        return types.SimpleNamespace(
            status=status, stdout=stdout.text(), stderr=stderr.getvalue(),
            records=records, events=events, library=library, built=built)


@pytest.fixture
def harness(tmp_path):
    return Harness(tmp_path)


def assert_failure(result, code, args, *, serialization=False, phase_fd=True, mode="ordinary"):
    document = strict_line(result.stdout)
    assert result.status == 1, document
    assert set(document) == {"engine", "error", "code", "args"}
    assert document["engine"] == "pineforge"
    assert (document["code"], document["args"]) == (code, args)
    assert result.stderr == ""
    if phase_fd:
        phases = [record["phase"] for record in result.records]
        assert phases == ["preflight", "execution", "result_assembly"] + (
            ["serialization"] if serialization else [])
    else:
        assert result.records == []
    assert "advance:completed" not in result.events
    assert result.events.count("stdout.write") == 1  # no part of a success report
    assert result.events.count("stdout.flush") == 1
    calls = [call[0] for call in result.library.calls]
    assert calls[-2:] == ["report_free", "strategy_free"]
    assert result.library.observer_descriptor is None
    if mode == "selected":
        assert result.library.window_config is None


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("field", TRADE_FIELDS)
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_trade_leaf_nonfinite(harness, mode, field, bad):
    result = harness.run(mode, trade_field=field.split(".")[1], bad_value=bad)
    # PnL is consumed during summary assembly and now fails before the real
    # builder can return. Other leaves fail in canonical-result validation.
    assert len(result.built) == (0 if field == "trades.pnl" else 1)
    assert_failure(result, RESULT_INVALID, {"field": field, "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("field", SUMMARY_FIELDS)
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_summary_leaf_nonfinite(harness, mode, field, bad):
    def poison(document):
        document["summary"][field.split(".")[1]] = bad

    result = harness.run(mode, inject=poison)
    assert len(result.built) == 1
    assert_failure(result, RESULT_INVALID, {"field": field, "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_native_net_profit_nonfinite(harness, mode, bad):
    result = harness.run(mode, net_profit=bad)
    assert_failure(result, RESULT_INVALID,
                   {"field": "summary.net_pnl", "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("mode", MODES)
def test_finite_pnls_overflow_summary_drawdown(harness, mode):
    result = harness.run(mode, pnls=(-1e308, -1e308), net_profit=0.0)
    assert_failure(result, RESULT_INVALID,
                   {"field": "summary.max_drawdown", "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("bad", BAD_VALUES)
def test_ordinary_typed_leaf_failure_without_phase_fd(harness, bad):
    result = harness.run("ordinary", phase_fd=False, trade_field="max_runup", bad_value=bad)
    assert_failure(result, RESULT_INVALID,
                   {"field": "trades.max_runup", "reason": NONFINITE_REASON}, phase_fd=False)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_writer_backstop(harness, mode, bad):
    def poison(document):
        document["elapsed_seconds"] = bad  # outside the canonical result leaves

    result = harness.run(mode, inject=poison)
    assert_failure(result, POST_EXECUTION_FAILED,
                   {"phase": "serialization", "reason": "exception"},
                   serialization=True, mode=mode)


def test_writer_backstop_without_phase_fd_is_report_result_invalid(harness):
    result = harness.run("ordinary", phase_fd=False,
                         inject=lambda document: document.update(elapsed_seconds=float("inf")))
    assert_failure(result, RESULT_INVALID,
                   {"field": "report.document", "reason": NONFINITE_REASON}, phase_fd=False)
    assert strict_line(result.stdout)["error"] == "the report document holds a non-finite number"


def test_writer_other_value_error_without_phase_fd_keeps_existing_classification(harness):
    def make_circular(document):
        document["diagnostics"]["cycle"] = document

    result = harness.run("ordinary", phase_fd=False, inject=make_circular)
    document = strict_line(result.stdout)
    assert result.status == 1
    assert (document["code"], document["args"]) == ("harness_internal_error", {})
    assert "ValueError: Circular reference detected" in document["error"]
    assert "ValueError: Circular reference detected" in result.stderr
    assert result.records == []
    assert result.events.count("stdout.write") == 1
    assert result.events.count("stdout.flush") == 1
    assert [call[0] for call in result.library.calls][-2:] == ["report_free", "strategy_free"]
    assert result.library.observer_descriptor is None


@pytest.mark.parametrize("bad", BAD_VALUES)
def test_capabilities_writer_backstop(harness, bad):
    real_record = swr.capabilities_record

    def poison(supported):
        document = real_record(supported)
        document["capabilities"]["injected_double"] = bad
        return document

    result = harness.run("ordinary", extra=("--capabilities-json",),
                         document_patch=mock.patch.object(swr, "capabilities_record", side_effect=poison))
    document = strict_line(result.stdout)
    assert result.status == 1
    assert (document["code"], document["args"]) == ("harness_internal_error", {})
    assert "ValueError" in result.stderr  # preflight document: no execution boundary
    assert result.records == []
    assert result.library.calls == []


@pytest.mark.parametrize("bad", BAD_VALUES)
def test_validation_document_writer_backstop(harness, bad):
    real_window = swr.build_report_window

    def poison(*args, **kwargs):
        window = real_window(*args, **kwargs)
        window["feed"]["injected_double"] = bad
        return window

    result = harness.run("selected", extra=("--validate-window-only",),
                         document_patch=mock.patch.object(swr, "build_report_window", side_effect=poison))
    document = strict_line(result.stdout)
    assert result.status == 1
    assert (document["code"], document["args"]) == ("harness_internal_error", {})
    assert "ValueError" in result.stderr
    assert result.records == []
    assert result.library.calls == []


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("metric,key", (
    (("all", "profit_factor"), "profit_factor"),
    (("longs", "profit_factor"), "profit_factor"),
    (("shorts", "profit_factor"), "profit_factor"),
    (("equity", "sharpe_monthly"), "sharpe_tv"),
), ids=("all", "longs", "shorts", "equity"))
def test_metric_defined_nan_stays_null(harness, mode, metric, key):
    # Only a native producer's defined absence can reach this representation.
    # Computed NaN and infinities are rejected by test_metrics_overflow.cpp.
    result = harness.run(mode, metric=metric, bad_value=float("nan"))
    document = strict_line(result.stdout)
    assert (result.status, result.stderr) == (0, "")
    assert document["metrics"][metric[0]][key] is None
    assert [record["phase"] for record in result.records][-1] == "completed"


# Exercise every floating report metric, including both monthly public aliases.
METRIC_FIELDS = [
    (block, name, {"sharpe_monthly": "sharpe_tv", "sortino_monthly": "sortino_tv"}.get(name, name))
    for block, struct in (("all", run_json.TradeStatsC), ("longs", run_json.TradeStatsC),
                          ("shorts", run_json.TradeStatsC), ("equity", run_json.EquityStatsC))
    for name, kind in struct._fields_ if kind is ctypes.c_double
]


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("block,name,key", METRIC_FIELDS)
@pytest.mark.parametrize("bad", (float("inf"), float("-inf")))
def test_metric_overflow_is_typed(harness, mode, block, name, key, bad):
    assert len(METRIC_FIELDS) == 87
    result = harness.run(mode, metric=(block, name), bad_value=bad)
    assert_failure(result, RESULT_INVALID,
                   {"field": f"metrics.{block}.{key}", "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("phase_fd", (True, False), ids=("phase-fd", "no-phase-fd"))
@pytest.mark.parametrize("field,key", (
    ("sharpe_monthly", "sharpe_tv"),
    ("sortino_monthly", "sortino_tv"),
    ("sharpe_tv", "sharpe_tv"),
    ("sortino_tv", "sortino_tv"),
), ids=("native-sharpe", "native-sortino", "public-sharpe", "public-sortino"))
def test_engine_monthly_overflow_field_uses_public_wire_key(harness, mode, phase_fd, field, key):
    # Model the typed kernel failure through the real getters and main()
    # forwarding path, rather than poisoning a returned report metric.
    result = harness.run(mode, phase_fd=phase_fd, failure_field=f"metrics.equity.{field}")
    assert result.built == []
    assert_failure(result, RESULT_INVALID,
                   {"field": f"metrics.equity.{key}", "reason": NONFINITE_REASON},
                   mode=mode, phase_fd=phase_fd)
    assert strict_line(result.stdout)["error"] == (
        "The report producer generated an invalid canonical result.")


@pytest.mark.parametrize("code,field", (
    (RESULT_INVALID, "metrics.equity.sharpe_bar"),
    (RESULT_INVALID, "metrics.equity.buy_hold_return"),
    (RESULT_INVALID, "metrics.all.sharpe_monthly"),
    (RESULT_INVALID, "metrics.equity.sharpe_monthly.extra"),
    (RESULT_INVALID, "sharpe_monthly"),
    ("engine_command_value_invalid", "metrics.equity.sharpe_monthly"),
))
def test_failure_line_preserves_fields_outside_monthly_report_aliases(code, field):
    args = {"field": field, "reason": NONFINITE_REASON}
    document = strict_line(run_json.failure_line("engine fault", code, args))
    assert document == {"engine": "pineforge", "error": "engine fault",
                        "code": code, "args": args}


def test_run_json_annualization_span_equals_the_native_span():
    # docker/report_annualization.def is what the C++ metric walk includes; run_json.py ships
    # alone and copies its one span bound. The bound is the whole policy: no field is named, so
    # no span makes any metric null (the .def holds nothing else, and run_json keeps no list).
    import re
    text = (Path(run_json.__file__).with_name("report_annualization.def")
            .read_text(encoding="utf-8"))
    spans = re.findall(r"^PF_ANNUALIZATION_SPAN_MS\((\d+)\)$", text, re.M)
    assert [int(s) for s in spans] == [run_json.MIN_ANNUALIZATION_SPAN_MS] == [ONE_DAY_MS]
    others = [line for line in text.splitlines()
              if line.strip() and not line.startswith("//")
              and not line.startswith("PF_ANNUALIZATION_SPAN_MS(")]
    assert others == []
    assert not hasattr(run_json, "SHORT_SPAN_ANNUALIZED_FIELDS")


def route_equity_metrics(cagr, final_equity, dip, **others):
    """The kernel's equity fields for the route harness curve, from the rule: it starts at the
    initial capital, falls once to `dip` when one is given (ReportLib's dip), and ends at
    final_equity. calmar is cagr over that drawdown percent, absent for a curve that never
    falls (the harness curve rises). `others` are published as given."""
    drawdown_pct = max_drawdown_pct(
        (selected.CAPITAL,) + (() if dip is None else (dip,)) + (final_equity,))
    return {"cagr": cagr, "max_equity_drawdown_pct": drawdown_pct,
            "calmar": cagr / drawdown_pct if drawdown_pct > 0.0 else float("nan"), **others}


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("dip,flags", (
    (None, {"cagr": SUBDAY}),
    (selected.CAPITAL * 0.98, {"cagr": SUBDAY, "calmar": SUBDAY}),
), ids=("zero-drawdown", "drawdown"))
def test_sub_day_annualization_completes_in_report_routes(harness, mode, dip, flags):
    # Ten minutes, +8%: the rule gives a finite simple proportional cagr, named under cagr.
    # calmar is cagr over the drawdown percent: null for a curve that never falls (no calmar
    # key), a finite number for a curve 2% below its first mark at the dip, which carries the
    # same string as cagr. The bar ratios keep their one formula (finite numbers, passed
    # through) and never have a reason.
    span_ms, final_equity = 10 * 60_000, selected.CAPITAL * 1.08
    cagr = simple_cagr(span_ms, selected.CAPITAL, final_equity)
    metrics = route_equity_metrics(cagr, final_equity, dip, sharpe_bar=1.5, sortino_bar=2.5)
    result = harness.run(mode, last_mark=(selected.T + span_ms, final_equity), dip=dip,
                         equity_metrics=metrics)
    document = strict_line(result.stdout)
    assert (result.status, result.stderr) == (0, "")
    equity = document["metrics"]["equity"]
    assert equity["cagr"] == cagr
    assert equity["calmar"] == (None if dip is None else metrics["calmar"])
    assert (equity["sharpe_bar"], equity["sortino_bar"]) == (1.5, 2.5)
    assert document["diagnostics"]["annualization"] == flags
    assert result.records[-1]["phase"] == "completed"


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("dip,flags", (
    (None, {"cagr": OVERFLOW}),
    (selected.CAPITAL * 0.5, {"cagr": OVERFLOW, "calmar": OVERFLOW}),
), ids=("zero-drawdown", "drawdown"))
def test_overflow_fallback_completes_in_report_routes(harness, mode, dip, flags):
    # One day, a 1e30 ratio: pow(1e30, 365.25) is not a double, so the kernel takes the simple
    # form, which is finite, and the wire names it under cagr. No trade, so the first mark (the
    # capital; in selected mode the anchor) is the initial capital the writer recomputes the
    # compounded form from. calmar (null at zero drawdown; a finite number after a fall to half
    # the capital) carries the same string as cagr when it is not null.
    final_equity = selected.CAPITAL * 1e30
    cagr = simple_cagr(ONE_DAY_MS, selected.CAPITAL, final_equity)
    assert math.isfinite(cagr)
    metrics = route_equity_metrics(cagr, final_equity, dip, sharpe_bar=1.5, sortino_bar=2.5)
    result = harness.run(
        mode, pnls=(), net_profit=0.0, last_mark=(selected.T + ONE_DAY_MS, final_equity),
        dip=dip, equity_metrics=metrics)
    document = strict_line(result.stdout)
    assert (result.status, result.stderr) == (0, "")
    equity = document["metrics"]["equity"]
    assert equity["cagr"] == cagr
    assert equity["calmar"] == (None if dip is None else metrics["calmar"])
    assert (equity["sharpe_bar"], equity["sortino_bar"]) == (1.5, 2.5)
    assert document["diagnostics"]["annualization"] == flags
    assert result.records[-1]["phase"] == "completed"


@pytest.mark.parametrize("dip,chosen_flags", (
    (None, {"cagr": OVERFLOW}),
    (selected.CAPITAL * 0.5, {"cagr": OVERFLOW, "calmar": OVERFLOW}),
), ids=("zero-drawdown", "drawdown"))
def test_the_selected_route_gives_the_writer_the_anchor_and_the_ordinary_route_does_not(
        harness, dip, chosen_flags):
    # ReportLib's first trade is entered at script bar 0. A selected run still knows its initial
    # capital (the anchor, passed by the selected route); an ordinary run, with a trade entered
    # at its first bar, has no proof of it and names nothing: calmar carries cagr's string, so
    # an ordinary run with a finite calmar still has no calmar key.
    final_equity = selected.CAPITAL * 1e30
    cagr = simple_cagr(ONE_DAY_MS, selected.CAPITAL, final_equity)
    options = {"last_mark": (selected.T + ONE_DAY_MS, final_equity), "dip": dip,
               "equity_metrics": route_equity_metrics(cagr, final_equity, dip)}
    chosen = strict_line(harness.run("selected", **options).stdout)
    assert chosen["diagnostics"]["annualization"] == chosen_flags
    plain = strict_line(harness.run("ordinary", **options).stdout)
    assert (plain["metrics"]["equity"]["calmar"] is None) == (dip is None)
    assert "annualization" not in plain["diagnostics"]


# (span_ms, capital, dip, final_equity, form, flags): the cagr form the rule gives a curve of two
# marks (three when `dip` is the middle one, a fall below the capital that gives the curve a
# drawdown) and the diagnostics.annualization object it carries on the wire, stated per row from
# the rule and not computed. Under one day cagr is always the simple form, named, even a flat
# curve's 0.0. From one day on it is the compounded form, not named; where that overflows a
# double (a 1e30 ratio over one day) it is the simple form, named. calmar carries cagr's string
# when it is not null and has no key when it is null (a curve that never falls: zero drawdown).
# A finite calmar beside a cagr that is not named (the one-day boundary rows, the losses) gets no
# key. The existing domain absences stay null with no object.
@pytest.mark.parametrize("span_ms,capital,dip,final_equity,form,flags", (
    pytest.param(7 * 60_000, 10_000.0, None, 10_800.0, "simple",
                 {"cagr": SUBDAY}, id="sub-day-rise-calmar-null"),
    pytest.param(7 * 60_000, 10_000.0, None, 10_000.0, "simple",
                 {"cagr": SUBDAY}, id="sub-day-flat-calmar-null"),
    pytest.param(7 * 60_000, 10_000.0, None, 9_000.0, "simple",
                 {"cagr": SUBDAY, "calmar": SUBDAY}, id="sub-day-loss"),
    pytest.param(7 * 60_000, 10_000.0, 9_800.0, 10_800.0, "simple",
                 {"cagr": SUBDAY, "calmar": SUBDAY}, id="sub-day-dip-then-gain"),
    pytest.param(86_399_999, 1.0, None, 1e300, "simple",
                 {"cagr": SUBDAY}, id="one-ms-under-a-day-1e300-calmar-null"),
    pytest.param(86_399_999, 100.0, 50.0, 120.0, "simple",
                 {"cagr": SUBDAY, "calmar": SUBDAY}, id="one-ms-under-a-day-dip"),
    pytest.param(86_400_000, 10_000.0, None, 10_800.0, "compounded",
                 None, id="exactly-a-day"),
    pytest.param(86_400_000, 10_000.0, 9_800.0, 10_800.0, "compounded",
                 None, id="exactly-a-day-dip-calmar-finite"),
    pytest.param(86_400_000, 10_000.0, None, 9_999.0, "compounded",
                 None, id="exactly-a-day-loss-calmar-finite"),
    pytest.param(86_400_001, 10_000.0, None, 10_800.0, "compounded",
                 None, id="one-ms-over-a-day"),
    pytest.param(30 * 86_400_000, 10_000.0, None, 9_000.0, "compounded",
                 None, id="30-days-loss-calmar-finite"),
    pytest.param(86_400_000, 1.0, None, 1e30, "simple",
                 {"cagr": OVERFLOW}, id="a-day-overflow-calmar-null"),
    pytest.param(3 * 86_400_000, 1.0, None, 1e30, "simple",
                 {"cagr": OVERFLOW}, id="3-days-overflow-calmar-null"),
    pytest.param(86_400_000, 1.0, 0.5, 1e30, "simple",
                 {"cagr": OVERFLOW, "calmar": OVERFLOW}, id="a-day-overflow-dip"),
    pytest.param(3 * 86_400_000, 1.0, 0.5, 1e30, "simple",
                 {"cagr": OVERFLOW, "calmar": OVERFLOW}, id="3-days-overflow-dip"),
    pytest.param(7 * 60_000, 0.0, None, 10_800.0, None, None, id="sub-day-capital-0"),
    pytest.param(7 * 60_000, -1.0, None, 10_800.0, None, None, id="sub-day-capital-negative"),
    pytest.param(7 * 60_000, 10_000.0, None, 0.0, None, None, id="sub-day-final-0"),
    pytest.param(86_400_000, 0.0, None, 10_800.0, None, None, id="a-day-capital-0"),
    pytest.param(86_400_000, 10_000.0, None, 0.0, None, None, id="a-day-final-0"),
    pytest.param(0, 10_000.0, None, 10_800.0, None, None, id="span-0"),
    pytest.param(-1, 10_000.0, None, 10_800.0, None, None, id="span-negative"),
))
def test_annualization_reason_on_wire(span_ms, capital, dip, final_equity, form, flags):
    # The kernel's values are supplied by the producer stand-in, as in the other wire tests.
    # The C++ rows separately prove the real metric computation.
    metrics = kernel_equity_metrics(form, span_ms, capital, final_equity, dip)
    marks = (((selected.T, capital, 0.0),)
             + (() if dip is None else ((selected.T + span_ms // 2, dip, 0.0),))
             + ((selected.T + span_ms, final_equity, 0.0),))
    wire = wire_document(marks, metrics, positive_capital=capital > 0)
    for name in ("cagr", "calmar", "sharpe_bar", "sortino_bar"):
        # A finite value reaches the wire as that number, an absence as null; none is
        # replaced or rounded by the writer, and only cagr and calmar have a reason.
        assert wire["metrics"]["equity"][name] == (
            None if math.isnan(metrics[name]) else metrics[name])
    assert wire["metrics"]["equity"]["sharpe_tv"] is None
    assert wire["metrics"]["equity"]["sortino_tv"] is None
    if flags is None:
        assert "annualization" not in wire["diagnostics"]
    else:
        # Row check on the published inputs: a calmar key is expected exactly for a finite calmar.
        assert ("calmar" in flags) == (not math.isnan(metrics["calmar"])), flags
        assert wire["diagnostics"]["annualization"] == flags
    # Appendix A's scalar leaf expansion stays exactly 135. This tests the
    # metric/summary names independently of the producer's descriptors.
    trade_keys = {
        "num_trades", "num_wins", "num_losses", "num_even", "percent_profitable",
        "net_profit", "net_profit_pct", "gross_profit", "gross_profit_pct",
        "gross_loss", "gross_loss_pct", "profit_factor", "avg_trade", "avg_trade_pct",
        "avg_win", "avg_win_pct", "avg_loss", "avg_loss_pct", "ratio_avg_win_avg_loss",
        "largest_win", "largest_win_pct", "largest_loss", "largest_loss_pct",
        "commission_paid", "max_consecutive_wins", "max_consecutive_losses",
        "expectancy", "avg_bars_in_trade", "avg_bars_in_wins", "avg_bars_in_losses",
    }
    equity_keys = {
        "max_equity_drawdown", "max_equity_drawdown_pct", "max_equity_runup",
        "max_equity_runup_pct", "buy_hold_return", "buy_hold_return_pct",
        "sharpe_tv", "sortino_tv", "sharpe_bar", "sortino_bar", "cagr", "calmar",
        "recovery_factor", "time_in_market_pct", "open_pl",
    }
    summary_keys = {
        "total_trades", "wins", "losses", "bars_processed", "win_rate_pct",
        "net_pnl", "avg_trade", "best_trade", "worst_trade", "max_drawdown",
    }
    assert set(wire["metrics"]) == {"all", "longs", "shorts", "equity"}
    for side in ("all", "longs", "shorts"):
        assert set(wire["metrics"][side]) == trade_keys
    assert set(wire["metrics"]["equity"]) == equity_keys
    assert set(wire["summary"]) == summary_keys
    trade_row_keys = {
        "n", "side", "entry_time", "exit_time", "entry_price", "exit_price",
        "qty", "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission",
        "entry_bar_index", "exit_bar_index", "open_at_end", "entry_incarnation",
    }
    assert set(wire["trades"][0]) == trade_row_keys
    assert all(set(point) == {"time_ms", "equity", "open_profit"}
               for point in wire["equity_curve"])
    # version + summary + 3 trade blocks + equity + trade-row fields + curve:
    assert (1 + len(summary_keys) + 3 * len(trade_keys) + len(equity_keys)
            + len(trade_row_keys) + 3) == 135


# The kernel's initial capital is 100 and the curve ends at 600 after exactly one day: a ratio of
# 6, whose compounded form is finite (6 ** 365.25 is about 1e284), so the kernel keeps the
# compounded form and nothing is flagged. A writer that took the first mark (75) for the capital
# would see a ratio of 8, whose compounded form overflows, and flag a form the kernel did not
# use. The first mark differs from the capital by open profit or a realized loss at it (the
# rows show a trade entered at the first bar); no row proves the capital, so nothing is named.
@pytest.mark.parametrize("first_mark,entry_bars", (
    ((selected.T, 75.0, -25.0), (0,)),
    ((selected.T, 75.0, 0.0), (0,)),
    ((selected.T, 75.0, -25.0), (3,)),
    ((selected.T, 75.0, 0.0), (-1,)),
), ids=("open-position", "realized-at-the-first-bar", "open-profit-alone", "unusable-bar-index"))
def test_a_first_mark_that_is_not_provably_the_capital_names_no_form(first_mark, entry_bars):
    capital, final_equity = 100.0, 600.0
    metrics = kernel_equity_metrics("compounded", ONE_DAY_MS, capital, final_equity)
    wire = wire_document((first_mark, (selected.T + ONE_DAY_MS, final_equity, 0.0)),
                         metrics, entry_bars=entry_bars)
    assert wire["metrics"]["equity"]["cagr"] == metrics["cagr"]
    assert "annualization" not in wire["diagnostics"]


def test_the_window_anchor_is_the_initial_capital_whatever_the_trades():
    # The selected-window anchor is the engine's own initial capital by construction, so a trade
    # entered at the first window bar (script bar 0 when no preroll precedes it) leaves it known.
    # The curve falls to half the capital on the way, so calmar is finite and carries cagr's
    # string; without the anchor and with that trade nothing is proved, and neither key is named.
    marks = ((selected.T, 1.0, 0.0), (selected.T + ONE_DAY_MS // 2, 0.5, 0.0),
             (selected.T + ONE_DAY_MS, 1e30, 0.0))
    metrics = kernel_equity_metrics("simple", ONE_DAY_MS, 1.0, 1e30, dip=0.5)
    anchored = wire_document(marks, metrics, entry_bars=(0,), window_anchor=True)
    assert anchored["metrics"]["equity"]["calmar"] == metrics["calmar"]
    assert anchored["diagnostics"]["annualization"] == {"cagr": OVERFLOW, "calmar": OVERFLOW}
    # The same ordinary curve and trade carry no proof of the capital.
    plain = wire_document(marks, metrics, entry_bars=(0,))
    assert plain["metrics"]["equity"]["calmar"] == metrics["calmar"]
    assert "annualization" not in plain["diagnostics"]


@pytest.mark.parametrize("days", (1, 2, 7, 30, 100, 300))
def test_overflow_decision_agrees_with_exact_arithmetic_at_the_threshold(days):
    # Ratios 1e-8 under and over the exact ratio at which (pow(ratio, 1 / span_years) - 1) * 100
    # first overflows a double, for several spans, from exact arithmetic and not from pow: a
    # ratio under it has a finite compounded form (nothing named, under cagr or calmar), a ratio
    # over it has none (the kernel takes the simple form: cagr named, and calmar with the same
    # string when it is not null; with a null calmar, zero drawdown, only the cagr key). The
    # margin moves the power by at least 1e-8, over 1e7 times the error of a double power, so
    # every working pow, C's included, decides these the same way.
    span_ms = days * ONE_DAY_MS
    for factor, with_null_calmar, with_finite_calmar in (
            ("0.99999999", {}, {}),
            ("1.00000001", {"cagr": OVERFLOW}, {"cagr": OVERFLOW, "calmar": OVERFLOW})):
        ratio = exact_overflow_ratio(span_ms, factor)
        curve = [{"time_ms": selected.T, "equity": 1.0, "open_profit": 0.0},
                 {"time_ms": selected.T + span_ms, "equity": ratio, "open_profit": 0.0}]
        assert run_json._annualization_diagnostics(
            curve, metrics_of(1.0)) == with_null_calmar, (days, factor, ratio)
        assert run_json._annualization_diagnostics(
            curve, metrics_of(1.0, calmar=7.0)) == with_finite_calmar, (days, factor, ratio)


@pytest.mark.parametrize("final_equity", (0.5, 1e-300, 5e-324))
@pytest.mark.parametrize("days", (1, 100))
def test_a_losing_curve_never_names_an_overflow(days, final_equity):
    # The power of a ratio under 1 underflows to 0.0 (cagr -100): an underflow is no overflow,
    # and a finite calmar (the loss is a drawdown) is named only beside a named cagr.
    curve = [{"time_ms": selected.T, "equity": 1.0, "open_profit": 0.0},
             {"time_ms": selected.T + days * ONE_DAY_MS, "equity": final_equity,
              "open_profit": 0.0}]
    assert run_json._annualization_diagnostics(curve, metrics_of(-100.0, calmar=-1.0)) == {}


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_executed_equity_nonfinite_is_typed(harness, mode, bad):
    result = harness.run(mode, curve_value=bad)
    assert_failure(result, RESULT_INVALID,
                   {"field": "equity_curve.equity", "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("pnls,net_profit", (
    ((1e308, 1e308, -1e308), 1e308),
    ((1e308, -1e308, -1e308), -1e308),
), ids=("sum", "peak-difference"))
def test_cumulative_summary_overflow_cannot_hide_in_min_max(harness, mode, pnls, net_profit):
    # Every trade and supplied net profit are finite. cum and cum-peak used
    # to overflow, then min/max silently discarded the resulting NaN.
    result = harness.run(mode, pnls=pnls, net_profit=net_profit)
    assert_failure(result, RESULT_INVALID,
                   {"field": "summary.max_drawdown", "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("mode", MODES)
def test_open_profit_nonfinite_is_typed(harness, mode):
    def poison(document):
        document["equity_curve"][-1]["open_profit"] = float("inf")

    result = harness.run(mode, inject=poison)
    assert_failure(result, RESULT_INVALID,
                   {"field": "equity_curve.open_profit", "reason": NONFINITE_REASON}, mode=mode)


@pytest.mark.parametrize("token", ("NaN", "Infinity", "-Infinity", "1e9999"))
def test_failure_line_engine_nonfinite_args_are_dropped(token):
    args = run_json._engine_args('{"field":' + token + '}')
    assert args == {}
    document = strict_line(run_json.failure_line("engine fault", "engine_unclassified_error", args))
    assert document == {"engine": "pineforge", "error": "engine fault",
                        "code": "engine_unclassified_error", "args": {}}


@pytest.fixture
def base_runner():
    # Run the frozen producer on the spot box, under the same deterministic
    # fixtures. No guessed digest or output computed by the changed producer.
    # The checkout must retain the campaign's pinned base commit.
    source = subprocess.run(["git", "show", BASE_SOURCE], cwd=ROOT,
                            capture_output=True, text=True, check=True).stdout
    module = types.ModuleType("en133_base_run_json")
    module.__file__ = str(ROOT / "docker" / "run_json.py")
    exec(compile(source, BASE_SOURCE, "exec"), module.__dict__)
    # These ABI layouts did not change. Share the real ctypes types so the same
    # backing allocations can be consumed by both versions of the producer.
    module.ReportC = run_json.ReportC
    module.EquityStatsC = run_json.EquityStatsC
    return module


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("phase_fd", (True, False), ids=("phase-fd", "no-phase-fd"))
@pytest.mark.parametrize("pnls", ((10.0, -7.0), ()), ids=("trades", "no-trades"))
def test_finite_report_bytes_match_base(harness, base_runner, mode, phase_fd, pnls):
    # These curves are minutes long, and a finite cagr on a sub-day span is flagged on the wire,
    # which the frozen producer cannot write: the stand-in publishes cagr as an absence (a null
    # on both wires), so no annualization flag applies and the bytes are the base's.
    options = {"pnls": pnls, "net_profit": 3.0 if pnls else 0.0,
               "equity_metrics": {"cagr": float("nan")}}
    before = harness.run(mode, runner=base_runner, phase_fd=phase_fd, **options)
    after = harness.run(mode, phase_fd=phase_fd, **options)
    assert (before.status, before.stderr, after.status, after.stderr) == (0, "", 0, "")
    document = strict_line(after.stdout)
    assert document["summary"] == {
        "total_trades": 2 if pnls else 0, "wins": 1 if pnls else 0,
        "losses": 1 if pnls else 0, "win_rate_pct": 50.0 if pnls else 0.0,
        "net_pnl": 3.0 if pnls else 0.0, "avg_trade": 1.5 if pnls else 0.0,
        "best_trade": 10.0 if pnls else 0.0, "worst_trade": -7.0 if pnls else 0.0,
        "max_drawdown": -7.0 if pnls else 0.0,
        "bars_processed": 9 if mode == "selected" else 10,
    }
    assert after.stdout.encode("utf-8") == before.stdout.encode("utf-8")
    assert after.records == before.records


@pytest.fixture
def native_host_library():
    configured = os.environ.get("PINEFORGE_REPORT_NONFINITE_HOST_LIBRARY")
    library = (Path(configured) if configured else
               ROOT / "build" / "lib" / "report_nonfinite_host.so")
    if not library.is_file():
        if configured:
            pytest.fail(f"configured native report fixture is missing: {library}")
        pytest.skip("SKIP private fixtures not present: native report excursion host library; "
                    "set PINEFORGE_REPORT_NONFINITE_HOST_LIBRARY")
    return library


@pytest.mark.parametrize("value", ("nan", "infinity"))
def test_native_process_excursion(harness, native_host_library, value):
    def run(value):
        parent, child = socket.socketpair()
        try:
            done = subprocess.run(
                [sys.executable, "-B", str(ROOT / "docker" / "run_json.py"),
                 "--so", str(native_host_library), "--ohlcv", str(harness.ohlcv),
                 "--input-tf", "1", "--script-tf", "1",
                 "--inputs", json.dumps({"excursion_value": value}),
                 "--run-phase-fd", str(child.fileno())],
                pass_fds=(child.fileno(),), capture_output=True, text=True, timeout=60)
            records = selected.phase_records(parent)
        finally:
            child.close()
            parent.close()
        return done, strict_line(done.stdout), [record["phase"] for record in records]

    control, finite, phases = run("finite")
    assert (control.returncode, control.stderr) == (0, "")
    assert len(finite["trades"]) == 1
    assert finite["trades"][0]["max_runup"] == 1.0
    assert finite["trades"][0]["max_drawdown"] == 0.0
    assert phases == ["preflight", "execution", "result_assembly", "serialization", "completed"]

    done, failure, phases = run(value)
    assert (done.returncode, done.stderr) == (1, "")
    assert set(failure) == {"engine", "error", "code", "args"}
    assert (failure["code"], failure["args"]) == (
        RESULT_INVALID, {"field": "trades.max_runup", "reason": NONFINITE_REASON})
    assert phases == ["preflight", "execution", "result_assembly"]
