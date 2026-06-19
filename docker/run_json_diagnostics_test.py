"""build_report_dict must surface the billable counters in a `diagnostics` block.

Mirrors the harness style of run_json_syminfo_test.py: load run_json (docker/ is
on sys.path in the engine test env) and call build_report_dict with a minimal
stand-in report. The fake exposes EVERY attribute build_report_dict dereferences
so the call succeeds without a compiled engine .so:
  - trades_len == 0          -> skips the trades[i] loop
  - equity_curve is None      -> falsy pointer, skips the equity-curve loop
  - metrics is a real MetricsC -> _stats_dict iterates its _fields_
  - net_profit + the diagnostics counters are read directly
"""
import run_json  # docker/ is on sys.path in the engine test env


class _FakeReport:
    """Minimal stand-in exposing only the fields build_report_dict reads."""
    trades_len = 0
    equity_curve = None  # falsy POINTER -> equity-curve loop skipped
    net_profit = 0.0
    # Billable counters (the point of this test):
    input_bars_processed = 525_600
    script_bars_processed = 8_760
    magnifier_sub_bars_total = 0
    magnifier_sample_ticks_total = 0
    bar_magnifier_enabled = 0

    def __init__(self):
        # _stats_dict iterates metrics.<field>._fields_; a real zero-initialized
        # MetricsC satisfies that without touching the native engine.
        self.metrics = run_json.MetricsC()


def _call():
    return run_json.build_report_dict(
        _FakeReport(),
        ohlcv_path="x.csv",
        n_bars=0,
        first_ts=0,
        last_ts=0,
        elapsed=0.0,
        applied_inputs={},
        applied_overrides={},
        applied_runtime={},
    )


def test_diagnostics_block_present():
    rep = _call()
    d = rep["diagnostics"]
    assert d["script_bars_processed"] == 8_760
    assert d["input_bars_processed"] == 525_600
    assert d["bar_magnifier_enabled"] is False
    assert d["magnifier_sub_bars_total"] == 0
    assert d["magnifier_sample_ticks_total"] == 0


def test_diagnostics_counter_types():
    d = _call()["diagnostics"]
    assert isinstance(d["input_bars_processed"], int)
    assert isinstance(d["script_bars_processed"], int)
    assert isinstance(d["magnifier_sub_bars_total"], int)
    assert isinstance(d["magnifier_sample_ticks_total"], int)
    assert isinstance(d["bar_magnifier_enabled"], bool)


def test_summary_bars_processed_unchanged():
    """summary.bars_processed stays = input_bars_processed (display = data scanned)."""
    rep = _call()
    assert rep["summary"]["bars_processed"] == 525_600
