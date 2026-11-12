"""Optional run warnings through both Python runners and the native JSON host."""

from __future__ import annotations

import contextlib
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "scripts"))

import run_json
import run_strategy
import pf_release_run
from test_run_json_bin import T0, csv_file


ABSENT = object()
# Matches the single-site canonical-text row in
# test_security_requested_timeframe_warning.cpp and the native fixture below.
CANONICAL = b'[{"args":{"input_tf":"60","requested_tf":"75","sec_id":0},"code":"request_timeframe_not_multiple"}]'
WARNING = {
    "args": {"input_tf": "60", "requested_tf": "75", "sec_id": 0},
    "code": "request_timeframe_not_multiple",
}
FALLBACK = {"args": {}, "code": "run_warnings_unavailable"}


@pytest.fixture(autouse=True)
def ordinary_host_environment(monkeypatch):
    monkeypatch.setenv("PINEFORGE_RUN_JSON_PROFILE", "unjailed")
    monkeypatch.delenv("PINEFORGE_REPORT_SINK", raising=False)


def _library(module, payload):
    events = []

    def run(state, bars, count, input_tf, script_tf, magnifier, samples, dist, ref):
        assert state == 7
        report = ctypes.cast(ref, ctypes.POINTER(module.ReportC)).contents
        report.input_bars_processed = count
        report.script_bars_processed = count
        report.input_tf_seconds = report.script_tf_seconds = 3600
        report.script_tf_ratio = 1
        events.append("run")

    def warnings(state):
        assert state == 7
        assert events == ["run", "report"]
        events.append("warnings")
        return payload

    library = SimpleNamespace(
        strategy_create=lambda params: 7,
        strategy_set_input=lambda *args: None,
        strategy_set_override=lambda *args: None,
        run_backtest_full=run,
        report_free=lambda ref: events.append("report_free"),
        strategy_free=lambda state: events.append("strategy_free"),
    )
    if payload is not ABSENT:
        library.strategy_get_run_warnings = warnings
    return library, events


def _public_report(tmp_path, monkeypatch, runner, payload):
    module = run_json if runner == "json" else run_strategy
    library, events = _library(module, payload)
    source = csv_file(tmp_path, step=3_600_000)
    builder_name = "build_report_dict" if runner == "json" else "_report_to_dict"
    builder = getattr(module, builder_name)

    def build(*args, **kwargs):
        result = builder(*args, **kwargs)
        events.append("report")
        return result

    with monkeypatch.context() as patch:
        patch.setattr(module, builder_name, build)
        patch.setattr(module.time, "time", lambda: 1000.0)
        if runner == "json":
            patch.setattr(run_json, "load_strategy", lambda path: library)
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                status = run_json.main([
                    "--so", "unused.so", "--ohlcv", str(source),
                    "--input-tf", "60", "--script-tf", "60",
                ])
            assert (status, stderr.getvalue()) == (0, ""), stdout.getvalue()
            result = json.loads(stdout.getvalue())
        else:
            strategy = run_strategy.Strategy.__new__(run_strategy.Strategy)
            strategy.lib = library
            result = strategy.run(source, input_tf="60", script_tf="60")
    assert events[-2:] == ["report_free", "strategy_free"]
    return result, events


@pytest.mark.parametrize("runner", ["json", "strategy"])
@pytest.mark.parametrize("payload", [
    pytest.param(b"[]", id="empty"),
    pytest.param(b"not JSON", id="not-json"),
    pytest.param(b"{}", id="object"),
    pytest.param(None, id="null-pointer"),
    pytest.param(b"[\xff]", id="invalid-utf8"),
    pytest.param(b"null", id="json-null"),
    pytest.param(b'[{"args":{"value":NaN},"code":"future_runtime_warning"}]',
                 id="nonfinite-constant"),
    pytest.param(b'[{"args":{"value":1e999},"code":"future_runtime_warning"}]',
                 id="overflowing-number"),
    pytest.param(b'[null,1,[],{},{"code":3,"args":{}},{"code":"bad_args","args":[]}]',
                 id="only-malformed-elements"),
])
def test_silent_or_unreadable_warnings_leave_public_report_unchanged(
        tmp_path, monkeypatch, runner, payload):
    before, _ = _public_report(tmp_path, monkeypatch, runner, ABSENT)
    after, _ = _public_report(tmp_path, monkeypatch, runner, payload)
    assert "warnings" not in after.get("diagnostics", {})
    if runner == "json":
        assert after["summary"]["bars_processed"] == 5
    else:
        assert after["input_bars_processed"] == 5
        assert "diagnostics" not in after
    assert json.dumps(after, separators=(",", ":")) == json.dumps(
        before, separators=(",", ":"))


@pytest.mark.parametrize("runner", ["json", "strategy"])
@pytest.mark.parametrize(("payload", "expected"), [
    pytest.param(CANONICAL, [WARNING], id="canonical"),
    pytest.param(
        b'[{"args":{"input_tf":"60","requested_tf":"75","sec_id":0},"code":"request_timeframe_not_multiple"},'
        b'{"args":{"input_tf":"60","requested_tf":"90","sec_id":4},"code":"request_timeframe_not_multiple"}]',
        [WARNING, {"args": {"input_tf": "60", "requested_tf": "90", "sec_id": 4},
                   "code": "request_timeframe_not_multiple"}],
        id="two-in-order"),
    pytest.param(
        b'[null,1,[],{},{"code":3,"args":{}},{"code":"bad_args","args":[]},'
        b'{"args":{"input_tf":"60","requested_tf":"75","sec_id":0},"code":"request_timeframe_not_multiple"}]',
        [WARNING], id="drop-malformed"),
    pytest.param(
        b'[{"args":{"input_tf":"60","requested_tf":"75","sec_id":0},'
        b'"code":"request_timeframe_not_multiple","future_key":{"detail":true}}]',
        [WARNING], id="ignore-extra-key"),
    pytest.param(
        b'[{"args":{"nested":{"values":[true,null,7]},"text":"future"},'
        b'"code":"future_runtime_warning"}]',
        [{"args": {"nested": {"values": [True, None, 7]}, "text": "future"},
          "code": "future_runtime_warning"}], id="unknown-code"),
    pytest.param(b'[{"args":{},"code":"run_warnings_unavailable"}]',
                 [FALLBACK], id="unavailable"),
])
def test_public_report_preserves_warnings_after_assembly_before_cleanup(
        tmp_path, monkeypatch, runner, payload, expected):
    result, events = _public_report(tmp_path, monkeypatch, runner, payload)
    assert result["diagnostics"]["warnings"] == expected
    assert "warnings" not in result
    assert events == ["run", "report", "warnings", "report_free", "strategy_free"]


@pytest.mark.parametrize("payload", [
    pytest.param(ABSENT, id="no-symbol"),
    pytest.param(b"[]", id="empty"),
    pytest.param(CANONICAL, id="warning"),
])
def test_strategy_cli_summary_and_trace_include_only_nonempty_warnings(
        tmp_path, monkeypatch, payload):
    source = csv_file(tmp_path, step=3_600_000)
    library, events = _library(run_strategy, payload)
    strategy = run_strategy.Strategy.__new__(run_strategy.Strategy)
    strategy.lib = library
    builder = run_strategy._report_to_dict

    def build(report):
        result = builder(report)
        events.append("report")
        return result

    monkeypatch.setattr(run_strategy, "_report_to_dict", build)
    monkeypatch.setattr(run_strategy, "Strategy", lambda path: strategy)
    monkeypatch.setattr(run_strategy, "find_strategy_lib", lambda *args: source)
    monkeypatch.setattr(run_strategy, "load_probe_requests", lambda path: None)
    monkeypatch.setattr(run_strategy.time, "time", lambda: 1000.0)
    trace = tmp_path / "trace.json"
    output = tmp_path / "engine_trades.csv"
    monkeypatch.setattr(sys, "argv", [
        "run_strategy.py", str(tmp_path), "--ohlcv", str(source),
        "--no-trim-output", "--trace-json", str(trace), "--output", str(output),
    ])
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        assert run_strategy.main() == 0
    document = json.loads(trace.read_text(encoding="utf-8"))
    lines = stdout.getvalue().splitlines()
    warning_lines = [line for line in lines if line.startswith("diagnostics.warnings: ")]
    assert document["trace"] == []
    assert document["trace_names"] == []
    assert any("0 trades, net_profit=0.00, bars=5," in line for line in lines)
    assert output.read_text(encoding="utf-8").count("\n") == 1
    if payload is CANONICAL:
        assert document["diagnostics"]["warnings"] == [WARNING]
        assert warning_lines == ["diagnostics.warnings: " + CANONICAL.decode("utf-8")]
    else:
        assert "diagnostics" not in document
        assert warning_lines == []


@pytest.mark.parametrize("payload", [
    pytest.param(ABSENT, id="no-key"),
    pytest.param(b"[]", id="empty"),
    pytest.param(CANONICAL, id="warning"),
])
def test_strategy_docker_report_keeps_optional_warnings(tmp_path, monkeypatch, payload):
    source = csv_file(tmp_path, step=3_600_000)
    generated = tmp_path / "generated.cpp"
    generated.write_text("// Precompiled strategy input.\n", encoding="utf-8")
    raw = {"trades": [], "summary": {"net_pnl": 12.5},
           "diagnostics": {"input_bars_processed": 5}}
    if payload is not ABSENT:
        raw["diagnostics"]["warnings"] = json.loads(payload)
    calls = []

    def release(cpp, chart, **kwargs):
        calls.append((cpp, chart))
        return raw

    monkeypatch.setattr(pf_release_run, "run_release", release)
    result = run_strategy._run_via_docker(tmp_path, source, {}, {}, None, None)
    expected = {
        "trades": [], "net_profit": 12.5, "input_bars_processed": 5,
        "trace": [], "trace_names": [], "broker_state_hash": [], "equity_curve_time_ms": [],
    }
    if payload is CANONICAL:
        expected["diagnostics"] = {"warnings": [WARNING]}
    assert calls == [(generated, source)]
    assert result == expected


HOST = """
import functools
import os
import sys
sys.path.insert(0, sys.argv[1])
import run_json
import run_phase_transport
run_json.time.time = lambda: 1000.0
run_phase_transport.RunPhaseWriter = functools.partial(
    run_phase_transport.RunPhaseWriter, clock=lambda: 1000.0)
run_json.trusted_planner_path = lambda: os.environ["PINEFORGE_RUN_WARNINGS_PLANNER_SO"]
sys.exit(run_json._host_main(sys.argv[2:]))
"""


def _native_report(tmp_path, module, mode, selected):
    source = csv_file(tmp_path, step=3_600_000)
    inventory = tmp_path / "inventory.json"
    inventory.write_text(json.dumps({
        "schema": "pineforge-request-feed-inventory/v1",
        "source_sha256": "0" * 64,
        "artifact_sha256": hashlib.sha256(Path(module).read_bytes()).hexdigest(),
        "primary_chart_timeframe": "60",
        "entries": [],
    }), encoding="utf-8")
    extra = [
        "--report-policy", "selected-window/v1",
        "--window-start-ms", str(T0 + 2 * 3_600_000),
        "--window-end-ms", str(T0 + 5 * 3_600_000),
        "--preroll-bars", "1", "--fed-start-ms", str(T0),
        "--request-feed-inventory", str(inventory),
    ] if selected else []
    result = subprocess.run([
        sys.executable, "-B", "-c", HOST, str(ROOT / "docker"),
        "--so", str(module), "--ohlcv", str(source),
        "--input-tf", "60", "--script-tf", "60", *extra,
    ], capture_output=True, timeout=30,
        env={**os.environ, "PINEFORGE_RUN_WARNINGS_TEST_MODE": mode})
    observed = (result.returncode, result.stdout, result.stderr)
    assert result.returncode == 0, observed
    assert result.stderr == b"", observed
    assert result.stdout.count(b"\n") == 1, observed
    return result.stdout, json.loads(result.stdout)


@pytest.mark.parametrize("selected", [False, True], ids=["ordinary", "selected"])
@pytest.mark.parametrize(("mode", "expected"), [
    pytest.param("warning", [WARNING], id="canonical"),
    pytest.param("unavailable", [FALLBACK], id="unavailable"),
])
def test_native_host_reads_warnings_on_each_public_report_path(
        tmp_path, selected, mode, expected):
    module = os.environ["PINEFORGE_RUN_WARNINGS_STRATEGY_SO"]
    _, silent = _native_report(tmp_path, module, "empty", selected)
    _, warned = _native_report(tmp_path, module, mode, selected)
    assert warned["diagnostics"].pop("warnings") == expected
    assert warned == silent
    assert warned["summary"]["bars_processed"] == (4 if selected else 5)
    assert warned["trades"] == []
    if selected:
        assert warned["report_window"]["counts"]["window_script_bars"] == 3
        assert len(warned["equity_curve"]) == 4


@pytest.mark.parametrize("selected", [False, True], ids=["ordinary", "selected"])
def test_native_empty_warning_report_matches_library_without_symbol_byte_for_byte(
        tmp_path, selected):
    empty_bytes, empty = _native_report(
        tmp_path, os.environ["PINEFORGE_RUN_WARNINGS_STRATEGY_SO"], "empty", selected)
    old_bytes, old = _native_report(
        tmp_path, os.environ["PINEFORGE_NO_RUN_WARNINGS_STRATEGY_SO"], "warning", selected)
    assert "warnings" not in empty["diagnostics"]
    assert "warnings" not in old["diagnostics"]
    assert empty["summary"]["bars_processed"] == (4 if selected else 5)
    assert empty_bytes == old_bytes


def test_native_strategy_wrapper_binds_and_reads_warnings(tmp_path, monkeypatch):
    module = Path(os.environ["PINEFORGE_RUN_WARNINGS_STRATEGY_SO"])
    monkeypatch.setenv("PINEFORGE_RUN_WARNINGS_TEST_MODE", "warning")
    strategy = run_strategy.Strategy(module)
    result = strategy.run(
        csv_file(tmp_path, step=3_600_000), input_tf="60", script_tf="60")
    assert result["diagnostics"]["warnings"] == [WARNING]
    assert result["input_bars_processed"] == 5
    assert result["trades"] == []
    assert strategy.lib.strategy_get_run_warnings.argtypes == [ctypes.c_void_p]
    assert strategy.lib.strategy_get_run_warnings.restype is ctypes.c_char_p
