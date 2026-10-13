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
import os
import socket
import subprocess
import sys
import types
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
    "summary.net_pnl", "summary.avg_trade", "summary.best_trade",
    "summary.worst_trade", "summary.max_drawdown",
)
BAD_VALUES = (
    pytest.param(float("nan"), id="nan"),
    pytest.param(float("inf"), id="infinity"),
    pytest.param(float("-inf"), id="negative-infinity"),
)
MODES = ("ordinary", "selected")
BASE_SOURCE = "26296bb8ce0ffb0b6fe113deae8853a375aaac37:docker/run_json.py"


def reject_constant(token):
    raise AssertionError(f"non-JSON number token: {token}")


def strict_line(stdout):
    lines = stdout.splitlines()
    assert len(lines) == 1, stdout
    assert stdout.endswith("\n"), stdout
    return json.loads(lines[0], parse_constant=reject_constant)


class ReportLib(selected.FakeLib):
    """Only the execution stand-in: owns the backing arrays until cleanup."""

    def __init__(self, plan, events, *, trade_field=None, bad_value=None,
                 net_profit=3.0, pnls=(10.0, -7.0), metric=None, curve_value=None):
        super().__init__(plan, events)
        self.trade_field = trade_field
        self.bad_value = bad_value
        self.net_profit = net_profit
        self.pnls = pnls
        self.metric = metric
        self.curve_value = curve_value
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
    assert len(result.built) == 1  # the REAL report builder ran
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
        document["elapsed_seconds"] = bad  # outside all 13 canonical leaves

    result = harness.run(mode, inject=poison)
    assert_failure(result, POST_EXECUTION_FAILED,
                   {"phase": "serialization", "reason": "exception"},
                   serialization=True, mode=mode)


def test_writer_backstop_without_phase_fd_keeps_existing_classification(harness):
    result = harness.run("ordinary", phase_fd=False,
                         inject=lambda document: document.update(elapsed_seconds=float("inf")))
    document = strict_line(result.stdout)
    assert result.status == 1
    # _post_boundary_failure explicitly has no boundary without this descriptor.
    assert (document["code"], document["args"]) == ("harness_internal_error", {})
    assert "ValueError" in document["error"]
    assert "ValueError" in result.stderr
    assert result.records == []
    assert result.events.count("stdout.write") == 1
    assert [call[0] for call in result.library.calls][-2:] == ["report_free", "strategy_free"]


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
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_metric_nonfinite_stays_null(harness, mode, metric, key, bad):
    result = harness.run(mode, metric=metric, bad_value=bad)
    document = strict_line(result.stdout)
    assert (result.status, result.stderr) == (0, "")
    assert document["metrics"][metric[0]][key] is None
    assert [record["phase"] for record in result.records][-1] == "completed"


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("bad", BAD_VALUES)
def test_equity_nonfinite_stays_null(harness, mode, bad):
    result = harness.run(mode, curve_value=bad)
    document = strict_line(result.stdout)
    assert (result.status, result.stderr) == (0, "")
    assert document["equity_curve"][-1]["equity"] is None
    assert document["equity_curve"][-1]["open_profit"] is None


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
    options = {"pnls": pnls, "net_profit": 3.0 if pnls else 0.0}
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
