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
import ctypes
from types import SimpleNamespace

import pytest
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


def test_trade_entry_incarnation_is_optional_and_positionally_aligned():
    report = _FakeReport()
    report.trades_len = 1
    report.trades = (run_json.TradeC * 1)()

    with_identity = run_json.build_report_dict(
        report,
        ohlcv_path="x.csv",
        n_bars=0,
        first_ts=0,
        last_ts=0,
        elapsed=0.0,
        applied_inputs={},
        applied_overrides={},
        applied_runtime={},
        trade_entry_incarnations=[41],
    )
    legacy = run_json.build_report_dict(
        report,
        ohlcv_path="x.csv",
        n_bars=0,
        first_ts=0,
        last_ts=0,
        elapsed=0.0,
        applied_inputs={},
        applied_overrides={},
        applied_runtime={},
    )

    assert with_identity["trades"][0]["entry_incarnation"] == 41
    assert legacy["trades"][0]["entry_incarnation"] == 0


class _CacheStatsGetter:
    """Wire-format fixture; real allocation refusal is exercised in C++."""
    def __init__(self, bypasses, status=0):
        self.bypasses, self.status, self.calls = bypasses, status, []

    def __call__(self, state, resident, bypasses):
        self.calls.append(state)
        ctypes.cast(resident, ctypes.POINTER(ctypes.c_uint64))[0] = 160_704
        ctypes.cast(bypasses, ctypes.POINTER(ctypes.c_uint64))[0] = self.bypasses
        return self.status


@pytest.mark.parametrize("count", [1, 3, (1 << 63) + 5])
def test_cache_bypass_diagnostics_serializes_the_engine_count(count):
    # test_command_scratch_spill and test_closed_trade_text_reads inject the
    # actual slab/directory/page refusals and call this export, including zero.
    getter = _CacheStatsGetter(count)
    lib = SimpleNamespace(pf_report_cache_stats=getter)
    run_json._bind_report_sink(lib)
    report = _call()
    run_json._add_cache_diagnostics(lib, 7, report["diagnostics"])
    assert getter.restype is ctypes.c_int
    assert getter.argtypes == [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
    assert getter.calls == [7]
    assert report["diagnostics"]["cache_resident_bytes"] == 160_704
    assert report["diagnostics"]["cache_bypass_reason"] == "alloc_failed"
    assert report["diagnostics"]["allocation_bypass_count"] == count


def test_cache_bypass_diagnostics_is_absent_without_a_failure():
    getter = _CacheStatsGetter(0)
    report = _call()
    run_json._add_cache_diagnostics(
        SimpleNamespace(pf_report_cache_stats=getter), 7, report["diagnostics"])
    assert getter.calls == [7]
    assert "cache_resident_bytes" not in report["diagnostics"]
    assert "cache_bypass_reason" not in report["diagnostics"]
    assert "allocation_bypass_count" not in report["diagnostics"]


def test_cache_diagnostics_skips_older_modules():
    report = _call()
    before = dict(report["diagnostics"])
    run_json._add_cache_diagnostics(SimpleNamespace(), 7, report["diagnostics"])
    assert report["diagnostics"] == before


def test_cache_diagnostics_getter_refusal_cannot_publish_a_partial_snapshot():
    report = _call()
    before = dict(report["diagnostics"])
    with pytest.raises(run_json.RunFailure):
        run_json._add_cache_diagnostics(
            SimpleNamespace(pf_report_cache_stats=_CacheStatsGetter(1, -1)),
            7, report["diagnostics"])
    assert report["diagnostics"] == before


# --- --bench diagnostics contract (timing + throughput) ---------------------
# These pure blocks back the raw-data benchmark drivers
# (benchmarks/speed/time_pineforge_docker.py,
#  benchmarks/throughput/time_throughput_docker.py).

def test_timing_block_shape():
    t = run_json._timing_block(
        [10, 20, 30], warmup=2, repeats=3, bar_magnifier=True,
        magnifier_samples=4, magnifier_dist="endpoints", volume_weighted=False)
    assert t["mode"] == "run_backtest_full"
    assert t["warmup"] == 2 and t["repeats"] == 3
    assert t["samples_ns"] == [10, 20, 30]
    assert t["magnifier"] == {
        "enabled": True, "samples": 4, "dist": "endpoints", "volume_weighted": False}


def test_throughput_magnifier_mode_mapping():
    on = run_json._throughput_block(525_600, [10, 20], bar_magnifier=True)
    off = run_json._throughput_block(525_600, [10, 20], bar_magnifier=False)
    assert on["magnifier_mode"] == "with_magnifier"
    assert off["magnifier_mode"] == "no_magnifier"
    assert on["items_processed"] == 525_600 and isinstance(on["items_processed"], int)
    assert on["samples_ns"] == [10, 20]


def test_timing_throughput_share_samples():
    """run_json wires throughput.samples_ns = timing.samples_ns (drivers rely on
    timing for speed and throughput for bars/s — they must be the same run)."""
    t = run_json._timing_block(
        [5, 6, 7], warmup=0, repeats=3, bar_magnifier=False,
        magnifier_samples=1, magnifier_dist="endpoints", volume_weighted=False)
    tp = run_json._throughput_block(100, t["samples_ns"], bar_magnifier=False)
    assert tp["samples_ns"] == t["samples_ns"]
