#!/usr/bin/env python3
"""Regression tests for exact trade counts in the real corpus analyzer.

Synthetic CSV fixtures and actual analyzer observations are temporary by
default. To retain them, invoke this script with --retain-artifacts DIR,
where DIR is an existing owned directory. Run through unittest discovery
from scripts; no private corpus or external capture is needed.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from verify_corpus import MATCH_WINDOW_SECONDS, analyze_strategy
from test_verify_corpus_trail_profile import TRADE_COLUMNS, TRAIL_STRATEGY


def _checked_path(path: Path) -> Path:
    """Refuse sensitive names and symlinks before following a fixture path."""
    path = path.absolute()
    parts = path.parts
    forbidden = {".ssh", ".aws", ".codex"}
    if any(part in forbidden or part.endswith(".env") for part in parts):
        raise ValueError(f"refused sensitive path: {path}")
    if any(
        parts[index:index + len(blocked)] == blocked
        for blocked in ((".config", "zsh", "keys"), (".config", "pineforge"))
        for index in range(len(parts))
    ):
        raise ValueError(f"refused sensitive path: {path}")
    if ".." in parts:
        raise ValueError(f"refused parent traversal: {path}")
    for component in (*reversed(path.parents), path):
        if component.is_symlink():
            raise ValueError(f"refused symlink: {component}")
    return path


ClosedTrade = tuple[datetime, datetime, str]


def _write_trades(
    path: Path, trades: list[ClosedTrade], *, tv_timezone: bool
) -> None:
    """Write explicit closed-trade timestamps, preserving shared identities."""
    columns = list(TRADE_COLUMNS)
    if not tv_timezone:
        columns.append("Engine entry ID")
    with _checked_path(path).open("x", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(columns)
        for number, (entry, exit_time, identity) in enumerate(
            sorted(trades), start=1
        ):
            # Same bare-wall-clock UTC+8 convention as the trail fixture.
            offset = timedelta(hours=8 if tv_timezone else 0)
            entry_text = (entry + offset).strftime("%Y-%m-%d %H:%M")
            exit_text = (exit_time + offset).strftime("%Y-%m-%d %H:%M")
            entry_row = [
                number, "Entry long", entry_text,
                identity if tv_timezone else "", 100.0, 1.0, 0.0, 0.0,
            ]
            exit_row = [
                number, "Exit long", exit_text, "", 110.0, 1.0, 10.0, 10.0,
            ]
            if not tv_timezone:
                entry_row.append("engine-" + identity)
                exit_row.append("")
            writer.writerow(entry_row)
            writer.writerow(exit_row)


def _analyze_tapes(
    run_root: Path, name: str,
    tv_trades: list[ClosedTrade], engine_trades: list[ClosedTrade],
):
    strategy = _checked_path(run_root / name)
    files = {
        filename: _checked_path(strategy / filename)
        for filename in (
            "strategy.pine", "inputs.json", "tv_trades.csv",
            "engine_trades.csv", "ohlcv.csv", "data.csv", "candles.csv",
            "actual-result.json",
        )
    }
    print(f"Count-rule fixture files to follow ({name}):", flush=True)
    for path in files.values():
        print(str(path), flush=True)
    # The real analyzer probes these OHLCV candidates. They are deliberately
    # absent in the fresh fixture directory; no trim implementation is replaced.
    strategy.mkdir()
    with files["strategy.pine"].open("x", encoding="utf-8") as handle:
        handle.write(TRAIL_STRATEGY)
    with files["inputs.json"].open("x", encoding="utf-8") as handle:
        json.dump({"tv_trades_csv_tz": "utc_plus_8"}, handle)
    _write_trades(files["tv_trades.csv"], tv_trades, tv_timezone=True)
    _write_trades(files["engine_trades.csv"], engine_trades, tv_timezone=False)

    result = analyze_strategy(strategy)
    row = result.report_row()
    # Retain actual observations before assertions, including on a failure.
    observations = {
        "report_row": row,
        "result_fields": {
            field: getattr(result, field)
            for field in (
                "tv_raw_count", "eng_raw_count", "tv_count", "eng_count",
                "tv_gate_count", "eng_gate_count", "matched_count",
                "gating_matched_count", "count_delta", "count_abs_delta",
                "count_ok", "entry_ok", "exit_ok", "pnl_ok", "coverage_ok",
                "distinct_entry_identity_ok", "distinct_entry_mismatches",
                "coverage", "coverage_tv_count", "unmatched_total",
                "unmatched_in_window", "trim_bars", "warmup_bars", "bounds",
                "open_mark_pairs", "no_aligned_trades", "entry_p100",
                "exit_p100", "pnl_p100", "qty_p100",
            )
        },
        "matched_entries": [
            {
                "tv_time": tv.entry_time, "engine_time": eng.entry_time,
                "tv_signal": tv.entry_signal,
                "engine_identity": eng.entry_identity,
                "tv_exit_time": tv.exit_time, "engine_exit_time": eng.exit_time,
            }
            for tv, eng in result.matched
        ],
    }
    with files["actual-result.json"].open("x", encoding="utf-8") as handle:
        json.dump(observations, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(json.dumps(row, sort_keys=True), flush=True)
    return result, row


class ExactCountBehaviorTests(unittest.TestCase):
    retain_artifacts: Path | None = None

    def setUp(self):
        if self.retain_artifacts is None:
            temporary = tempfile.TemporaryDirectory(prefix="verify-corpus-count-")
            # unittest runs cleanups even if setup or an assertion raises.
            self.addCleanup(temporary.cleanup)
            self.run_root = _checked_path(Path(temporary.name))
            print(f"Count-rule temporary run directory: {self.run_root}", flush=True)
        else:
            parent = _checked_path(self.retain_artifacts)
            self.run_root = _checked_path(Path(tempfile.mkdtemp(
                prefix="verify-corpus-count-", dir=parent
            )))
            print(f"Count-rule retained run directory: {self.run_root}", flush=True)

    def _assert_other_gates(
        self, result, row, shared, engine_count, *, tv_count=None
    ):
        shared_count = len(shared)
        if tv_count is None:
            tv_count = shared_count
        self.assertTrue(result.tv_exists)
        self.assertTrue(result.eng_exists)
        self.assertFalse(result.no_aligned_trades)
        self.assertEqual(result.profile, "production")
        self.assertEqual(
            (result.tv_raw_count, result.eng_raw_count), (tv_count, engine_count)
        )
        self.assertEqual((result.tv_count, result.eng_count), (tv_count, engine_count))
        self.assertEqual(
            (result.tv_gate_count, result.eng_gate_count), (tv_count, engine_count)
        )
        self.assertEqual((result.matched_count, result.gating_matched_count),
                         (shared_count, shared_count))
        self.assertEqual(len(result.matched), shared_count)
        self.assertEqual((result.trim_bars, result.warmup_bars), (0, 0))
        self.assertIsNone(result.bounds)
        self.assertEqual(result.open_mark_pairs, 0)
        self.assertEqual(result.coverage_tv_count, tv_count)
        self.assertEqual(result.coverage, shared_count / tv_count)
        self.assertEqual(result.unmatched_total, tv_count - shared_count)
        self.assertEqual(
            result.unmatched_in_window, max(tv_count, engine_count) - shared_count
        )
        for field in (
            "entry_ok", "exit_ok", "pnl_ok", "coverage_ok",
            "distinct_entry_identity_ok",
        ):
            self.assertIs(getattr(result, field), True, field)
        self.assertEqual(result.distinct_entry_mismatches, 0)
        self.assertEqual(
            row["metric_observations"],
            {"entry": shared_count, "exit": shared_count, "pnl": shared_count},
        )
        policy = row["grading_policy"]
        self.assertEqual(policy["count_rule"], "exact")
        self.assertEqual(policy["profile"], result.profile)
        self.assertGreater(policy["thresholds"]["count"], 0.0)
        for metric in ("entry", "exit", "pnl"):
            self.assertEqual(len(getattr(result, metric + "_deltas")), shared_count)
            self.assertEqual(getattr(result, metric + "_p90"), 0.0)
            self.assertEqual(getattr(result, metric + "_p100"), 0.0)
            self.assertEqual(row[metric + "_p90"], getattr(result, metric + "_p90"))
            self.assertLess(row[metric + "_p90"], policy["thresholds"][metric])
        self.assertEqual(result.qty_p100, 0.0)
        self.assertEqual((row["tv"], row["eng"], row["matched"]),
                         (result.tv_count, result.eng_count, result.matched_count))
        self.assertEqual(row["count_delta"], result.count_delta)
        self.assertEqual(row["count_abs_delta"], result.count_abs_delta)
        self.assertIs(row["distinct_entry_identity_ok"], True)
        self.assertEqual(row["distinct_entry_mismatches"], 0)
        self.assertEqual(row["tier"], result.tier)

        # Verify all real matches, including unchanged first/last anchors.
        self.assertEqual(
            [(tv.entry_time, eng.entry_time) for tv, eng in result.matched],
            [(int(entry.timestamp()), int(entry.timestamp()))
             for entry, _, _ in shared],
        )
        self.assertEqual(
            [(tv.entry_signal, eng.entry_identity) for tv, eng in result.matched],
            [(identity, "engine-" + identity) for _, _, identity in shared],
        )
        for tv, eng in result.matched:
            self.assertFalse(tv.open_mark)
            self.assertFalse(eng.open_mark)
            self.assertEqual(tv.exit_time, eng.exit_time)
            self.assertEqual(tv.exit_time - tv.entry_time, 3600)
            self.assertEqual((tv.entry_price, eng.entry_price), (100.0, 100.0))
            self.assertEqual((tv.exit_price, eng.exit_price), (110.0, 110.0))
            self.assertEqual((tv.qty, eng.qty), (1.0, 1.0))
            self.assertEqual((tv.pnl, eng.pnl), (10.0, 10.0))

    def test_exact_rule_rejects_one_interior_extra_within_nominal_fraction(self):
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        shared = [
            (start + timedelta(days=index),
             start + timedelta(days=index, hours=1), f"entry-{index + 1}")
            for index in range(200)
        ]
        extra_entry = shared[99][0] + timedelta(hours=12)
        self.assertLess(shared[99][0], extra_entry)
        self.assertLess(extra_entry, shared[100][0])
        self.assertGreater((extra_entry - shared[99][0]).total_seconds(),
                           MATCH_WINDOW_SECONDS)
        self.assertGreater((shared[100][0] - extra_entry).total_seconds(),
                           MATCH_WINDOW_SECONDS)
        extra = (extra_entry, extra_entry + timedelta(hours=1), "interior-extra")
        engine_extra = sorted([*shared, extra])
        self.assertEqual(engine_extra[0], shared[0])
        self.assertEqual(engine_extra[-1], shared[-1])
        self.assertEqual(len({entry for entry, _, _ in engine_extra}), 201)

        run_root = self.run_root
        control, control_row = _analyze_tapes(run_root, "equal", shared, shared)
        self._assert_other_gates(control, control_row, shared, 200)
        self.assertEqual(control.count_abs_delta, 0)
        self.assertEqual(control.count_delta, 0.0)
        self.assertIs(control.count_ok, True)
        self.assertEqual(control.tier, "excellent")

        # A failed equal-count control stops this method before unequal analysis.
        unequal, unequal_row = _analyze_tapes(
            run_root, "interior-extra", shared, engine_extra
        )
        self._assert_other_gates(unequal, unequal_row, shared, 201)
        self.assertEqual(unequal.count_abs_delta, 1)
        self.assertAlmostEqual(unequal.count_delta, 1 / 201, places=15)
        self.assertGreater(unequal.count_delta, 0.0)
        self.assertLess(
            unequal.count_delta,
            unequal_row["grading_policy"]["thresholds"]["count"],
        )
        # A nominal-fraction count gate must fail this real analyzer assertion.
        self.assertIs(unequal.count_ok, False)
        self.assertEqual(unequal.tier, "strong")

    def test_balanced_unmatched_trades_do_not_cancel(self):
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        shared = [
            (start + timedelta(days=index),
             start + timedelta(days=index, hours=1), f"entry-{index + 1}")
            for index in range(200)
        ]
        engine_entry = shared[49][0] + timedelta(hours=12)
        tv_entry = shared[149][0] + timedelta(hours=12)
        for entry in (engine_entry, tv_entry):
            for shared_entry, _, _ in shared:
                self.assertGreater(
                    abs((entry - shared_entry).total_seconds()),
                    MATCH_WINDOW_SECONDS,
                )
        self.assertGreater(
            (tv_entry - engine_entry).total_seconds(), MATCH_WINDOW_SECONDS
        )
        engine_only = (
            engine_entry, engine_entry + timedelta(hours=1), "engine-only"
        )
        tv_only = (tv_entry, tv_entry + timedelta(hours=1), "tv-only")
        tv_trades = sorted([*shared, tv_only])
        engine_trades = sorted([*shared, engine_only])
        for trades in (tv_trades, engine_trades):
            self.assertEqual((trades[0], trades[-1]), (shared[0], shared[-1]))
            self.assertEqual(len({entry for entry, _, _ in trades}), 201)

        # Same row counts as the regression, but all 201 entries really match.
        # Moving the engine-only input to the TV-only entry must restore Excellent.
        control, control_row = _analyze_tapes(
            self.run_root, "balanced-matched-control", tv_trades, tv_trades
        )
        self._assert_other_gates(control, control_row, tv_trades, 201)
        self.assertEqual(control.count_abs_delta, 0)
        self.assertEqual(control.count_delta, 0.0)
        self.assertIs(control.count_ok, True)
        self.assertEqual(control.tier, "excellent")

        balanced, balanced_row = _analyze_tapes(
            self.run_root, "balanced-unmatched", tv_trades, engine_trades
        )
        self._assert_other_gates(
            balanced, balanced_row, shared, 201, tv_count=201
        )
        self.assertEqual(balanced.count_abs_delta, 0)
        # This assertion fails on the net-count grader with delta=0 / Excellent,
        # before reading the new gross-count field: setup failure is not the red.
        self.assertIs(
            balanced.count_ok, False,
            f"balanced unmatched trades cancelled: "
            f"delta={balanced.count_delta}, tier={balanced.tier}",
        )
        self.assertEqual(balanced.gross_unmatched_count, 2)
        self.assertEqual(balanced_row["gross_unmatched_count"], 2)
        self.assertAlmostEqual(balanced.count_delta, 2 / 201, places=15)
        self.assertLess(
            balanced.count_delta,
            balanced_row["grading_policy"]["thresholds"]["count"],
        )
        self.assertEqual(balanced.tier, "strong")
        self.assertEqual(control.gross_unmatched_count, 0)
        self.assertEqual(control_row["gross_unmatched_count"], 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--retain-artifacts", type=Path, metavar="DIR")
    options, unittest_args = parser.parse_known_args()
    ExactCountBehaviorTests.retain_artifacts = options.retain_artifacts
    unittest.main(argv=[sys.argv[0], *unittest_args])
