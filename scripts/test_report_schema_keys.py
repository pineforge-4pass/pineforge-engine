#!/usr/bin/env python3
"""The JSON report keys of pf_equity_stats_t outlive its 1.0 field spellings.

Lane REL10 removed the pre-1.0 C spellings ``sharpe_tv`` / ``sortino_tv``;
the fields are ``sharpe_monthly`` / ``sortino_monthly``. The report schema is a
wire format and keeps its keys (ADR-0001, "Deprecated public spellings"): the
ctypes mirror in docker/run_json.py names the C fields, and ``_stats_dict``
writes those two under their report keys. These tests pin the equity key list
a report carried before the rename, the value behind each renamed key, and the
mirror's layout. No compiled engine is needed.
"""
from __future__ import annotations

import ctypes
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json  # noqa: E402

# metrics.equity's keys, in order, as docker/run_json.py wrote them at
# 1a0e7ea1, the commit before the rename.
EQUITY_KEYS = [
    "max_equity_drawdown", "max_equity_drawdown_pct",
    "max_equity_runup", "max_equity_runup_pct",
    "buy_hold_return", "buy_hold_return_pct",
    "sharpe_tv", "sortino_tv",
    "sharpe_bar", "sortino_bar",
    "cagr", "calmar",
    "recovery_factor", "time_in_market_pct",
    "open_pl",
]


class _Report:
    """The fields build_report_dict reads, as run_json_diagnostics_test.py
    fakes them: no trades, no equity curve, a real zero MetricsC."""
    trades_len = 0
    equity_curve = None
    net_profit = 0.0
    input_bars_processed = 0
    script_bars_processed = 0
    magnifier_sub_bars_total = 0
    magnifier_sample_ticks_total = 0
    bar_magnifier_enabled = 0

    def __init__(self) -> None:
        self.metrics = run_json.MetricsC()


class ReportSchemaKeyTests(unittest.TestCase):
    def test_equity_keys_are_the_pre_rename_keys(self) -> None:
        self.assertEqual(list(run_json._stats_dict(run_json.EquityStatsC())), EQUITY_KEYS)

    def test_renamed_fields_carry_their_values_under_the_report_keys(self) -> None:
        stats = run_json.EquityStatsC()
        stats.sharpe_monthly = 1.25
        stats.sortino_monthly = -0.5
        out = run_json._stats_dict(stats)
        self.assertEqual(out["sharpe_tv"], 1.25)
        self.assertEqual(out["sortino_tv"], -0.5)
        self.assertNotIn("sharpe_monthly", out)
        self.assertNotIn("sortino_monthly", out)

    def test_trade_stats_keep_their_field_names(self) -> None:
        names = [name for name, _ in run_json.TradeStatsC._fields_]
        self.assertEqual(list(run_json._stats_dict(run_json.TradeStatsC())), names)

    def test_mirror_layout_matches_the_c_header(self) -> None:
        # src/c_abi.cpp pins the same numbers with static_assert.
        self.assertEqual(ctypes.sizeof(run_json.EquityStatsC), 120)
        self.assertEqual(run_json.EquityStatsC.sharpe_monthly.offset, 48)
        self.assertEqual(run_json.EquityStatsC.sortino_monthly.offset, 56)
        self.assertEqual(run_json.EquityStatsC.sharpe_bar.offset, 64)
        self.assertEqual(ctypes.sizeof(run_json.MetricsC), 768)
        self.assertEqual(run_json.MetricsC.equity.offset, 648)

    def test_report_dict_writes_the_report_keys(self) -> None:
        report = _Report()
        report.metrics.equity.sharpe_monthly = 2.0
        rep = run_json.build_report_dict(
            report, ohlcv_path="x.csv", n_bars=0, first_ts=0, last_ts=0,
            elapsed=0.0, applied_inputs={}, applied_overrides={}, applied_runtime={})
        equity = rep["metrics"]["equity"]
        self.assertEqual(list(equity), EQUITY_KEYS)
        self.assertEqual(equity["sharpe_tv"], 2.0)


if __name__ == "__main__":
    unittest.main()
