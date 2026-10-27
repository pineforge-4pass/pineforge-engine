"""Trace admission's host boundary; native behavior is in test_trace_profile.

The injected native refusal tests transport, not a replacement trace guard.
The default-report byte oracle below is handwritten before the guard change.
"""
import contextlib
import io
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_bin as bars
import test_run_json_jailed_profile as profiles
import test_run_json_sink_fd as fd_sinks

REFUSAL = "error: bounded-memory execution refuses trace recording; use an unjailed debug run"
FAILURE_LINE = (
    '{"engine":"pineforge","error":"' + REFUSAL
    + '","code":"run_request_invalid","args":{"option":"arguments"}}\n'
)


def refused_library(mode):
    if mode == "hpo":
        library = profiles.HpoLibrary()
        library.setter_status = -1
    else:
        factory = (profiles.ProfileFdLibrary if mode == "ordinary"
                   else profiles.SelectedProfileFdLibrary)
        library = factory(setter_status=-1)
    for name, body in {
        "strategy_get_last_error": lambda state: REFUSAL.encode(),
        "strategy_get_last_error_code": lambda state: b"run_request_invalid",
        "strategy_get_last_error_args": lambda state: b'{"option":"arguments"}',
    }.items():
        setattr(library, name, body)
    return library


@pytest.fixture(autouse=True)
def clear_profile(monkeypatch):
    monkeypatch.delenv("PINEFORGE_RUN_JSON_PROFILE", raising=False)


@pytest.mark.parametrize("mode", ["ordinary", "selected"])
@pytest.mark.parametrize("transport", ["array", "fd"])
@pytest.mark.parametrize("selector", ["flag", "environment"])
def test_jailed_trace_refusal_precedes_all_report_output(
        tmp_path, monkeypatch, mode, transport, selector):
    library = refused_library(mode)
    options = ["--profile", "jailed"] if selector == "flag" else []
    if selector == "environment":
        monkeypatch.setenv("PINEFORGE_RUN_JSON_PROFILE", "jailed")
    with contextlib.ExitStack() as resources:
        stream = resources.enter_context(fd_sinks.Stream()) if transport == "fd" else None
        if stream:
            original = stream.writer
            stream.writer = fd_sinks.fcntl.fcntl(original, fd_sinks.fcntl.F_DUPFD_CLOEXEC, 17)
            fd_sinks.os.close(original)
            options += ["--report-sink-fd", str(stream.writer)]
        status, text, stderr = profiles.invoke_profile_sink(
            tmp_path, mode, library, options)
        if stream:
            fd_sinks.os.fstat(stream.writer)  # refusal keeps caller ownership
    assert (status, text, stderr) == (1, FAILURE_LINE, "")
    assert library.created == library.released and len(library.created) == 1
    assert library.arena_admissions == []
    assert not any(row[0] in ("sink", "run") for row in library.history)
    if stream:
        assert stream.data == b""


def test_hpo_admission_preserves_the_trace_request_failure():
    library = refused_library("ordinary")
    with pytest.raises(run_json.RunFailure) as caught:
        run_json.apply_scratch_tables(library, 7, run_json.default_scratch_manifest(2))
    failure = caught.value
    assert (str(failure), failure.code, failure.code_args, failure.exit_status) == (
        REFUSAL, "run_request_invalid", {"option": "arguments"}, 1)


def test_hpo_trial_trace_refusal_never_enters_ranking(tmp_path, monkeypatch, capsys):
    library = refused_library("hpo")
    module = profiles.grid_fixture(tmp_path, monkeypatch, library)
    with pytest.raises(run_json.RunFailure) as caught:
        module.main()
    assert (caught.value.code, caught.value.code_args) == (
        "run_request_invalid", {"option": "arguments"})
    assert library.created == library.released and len(library.created) == 1
    assert not any(row[0] == "run" for row in library.history)
    text = capsys.readouterr().out
    assert "Params:" not in text and "OPTIMIZATION COMPLETE" not in text


# Fixed ABI report from the call stand-in: every field is zero, no report
# arrays. Keep names/order/types independent of ReportC._fields_ and builders.
TRADE_STATS = (
    '{"num_trades":0,"num_wins":0,"num_losses":0,"num_even":0,'
    '"percent_profitable":0.0,"net_profit":0.0,"net_profit_pct":0.0,'
    '"gross_profit":0.0,"gross_profit_pct":0.0,"gross_loss":0.0,"gross_loss_pct":0.0,'
    '"profit_factor":0.0,"avg_trade":0.0,"avg_trade_pct":0.0,"avg_win":0.0,'
    '"avg_win_pct":0.0,"avg_loss":0.0,"avg_loss_pct":0.0,"ratio_avg_win_avg_loss":0.0,'
    '"largest_win":0.0,"largest_win_pct":0.0,"largest_loss":0.0,"largest_loss_pct":0.0,'
    '"commission_paid":0.0,"expectancy":0.0,"max_consecutive_wins":0,'
    '"max_consecutive_losses":0,"avg_bars_in_trade":0.0,"avg_bars_in_wins":0.0,'
    '"avg_bars_in_losses":0.0}'
)
EQUITY_STATS = (
    '{"max_equity_drawdown":0.0,"max_equity_drawdown_pct":0.0,'
    '"max_equity_runup":0.0,"max_equity_runup_pct":0.0,"buy_hold_return":0.0,'
    '"buy_hold_return_pct":0.0,"sharpe_tv":0.0,"sortino_tv":0.0,"sharpe_bar":0.0,'
    '"sortino_bar":0.0,"cagr":0.0,"calmar":0.0,"recovery_factor":0.0,'
    '"time_in_market_pct":0.0,"open_pl":0.0}'
)


def test_default_trace_off_report_bytes(tmp_path, monkeypatch):
    source = tmp_path / "chart.csv"
    source.write_text(
        "timestamp,open,high,low,close,volume\n"
        "1704067200000,100,110,95,102,50\n"
        "1704067260000,102,112,99,108,50\n", encoding="utf-8")
    library = bars.Library()
    monkeypatch.setattr(run_json, "load_strategy", lambda path: library)
    monkeypatch.setattr(run_json.time, "time", lambda: 10.)

    def no_provenance(*args, **kwargs):
        raise ValueError("fixture has no generated strategy provenance")

    monkeypatch.setattr(run_json, "build_provenance", no_provenance)
    output, errors = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
        status = run_json.main(["--so", "unused.so", "--ohlcv", str(source)])
    expected = (
        '{"engine":"pineforge","input":{"ohlcv":' + json.dumps(str(source))
        + ',"bars":2,"first_ts":1704067200000,"last_ts":1704067260000,'
        '"first_time":"2024-01-01 00:00 UTC","last_time":"2024-01-01 00:01 UTC"},'
        '"applied_inputs":{},"applied_overrides":{},"applied_runtime":{"input_tf":"",'
        '"script_tf":"","input_tf_seconds":0,"script_tf_seconds":0,"script_tf_ratio":0,'
        '"needs_aggregation":false,"bar_magnifier":false,"magnifier_samples":4,'
        '"magnifier_dist":"endpoints","magnifier_volume_weighted":false,'
        '"trade_start_ms":null,"chart_tz":""},"elapsed_seconds":0.0,'
        '"summary":{"total_trades":0,"wins":0,"losses":0,"win_rate_pct":0.0,'
        '"net_pnl":0.0,"avg_trade":0.0,"best_trade":0.0,"worst_trade":0.0,'
        '"max_drawdown":0.0,"bars_processed":0},"diagnostics":{"input_bars_processed":0,'
        '"script_bars_processed":0,"magnifier_sub_bars_total":0,'
        '"magnifier_sample_ticks_total":0,"bar_magnifier_enabled":false,'
        '"run_profile":"unjailed","scratch_arena_jailed":false},"trades":[],'
        '"metrics":{"all":' + TRADE_STATS + ',"longs":' + TRADE_STATS
        + ',"shorts":' + TRADE_STATS + ',"equity":' + EQUITY_STATS
        + '},"equity_curve":[],"fingerprint":null}\n'
    )
    assert (status, errors.getvalue()) == (0, "")
    assert output.getvalue().encode() == expected.encode()
    assert not any("trace" in call for call in library.calls)
