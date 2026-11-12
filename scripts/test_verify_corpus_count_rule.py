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


def _analyze_configured_tapes(
    run_root: Path, name: str,
    tv_trades: list[ClosedTrade], engine_trades: list[ClosedTrade],
    *, short_engine: bool = False, trim_bars: int = 0,
    bar_times: list[datetime] | None = None,
):
    """Extend the CSV helpers with direction and a real, known bars span."""
    strategy = _checked_path(run_root / name)
    strategy.mkdir()
    with _checked_path(strategy / "strategy.pine").open(
        "x", encoding="utf-8"
    ) as handle:
        handle.write(TRAIL_STRATEGY)
    with _checked_path(strategy / "inputs.json").open(
        "x", encoding="utf-8"
    ) as handle:
        json.dump({
            "tv_trades_csv_tz": "utc_plus_8",
            "trim_bars": trim_bars,
        }, handle)
    _write_trades(strategy / "tv_trades.csv", tv_trades, tv_timezone=True)
    engine_path = _checked_path(strategy / "engine_trades.csv")
    _write_trades(engine_path, engine_trades, tv_timezone=False)
    if short_engine:
        with engine_path.open(encoding="utf-8", newline="") as handle:
            rows = list(csv.reader(handle))
        for trade_row in rows[1:]:
            # These are the Type and price columns written by _write_trades.
            trade_row[1] = trade_row[1].replace("long", "short")
            if trade_row[1] == "Exit short":
                trade_row[4] = "90.0"
        with engine_path.open("w", encoding="utf-8", newline="") as handle:
            csv.writer(handle).writerows(rows)
    if bar_times is not None:
        with _checked_path(strategy / "ohlcv.csv").open(
            "x", encoding="utf-8", newline=""
        ) as handle:
            writer = csv.writer(handle)
            writer.writerow(["timestamp", "open", "high", "low", "close", "volume"])
            for bar_time in bar_times:
                writer.writerow([int(bar_time.timestamp()), 100, 110, 90, 100, 1])

    result = analyze_strategy(strategy)
    row = result.report_row()
    observations = {
        "report_row": row,
        "result_fields": {
            field: getattr(result, field)
            for field in (
                "tv_gate_count", "eng_gate_count", "count_abs_delta", "count_ok",
                "trim_bars", "warmup_bars", "bounds", "open_mark_pairs",
            )
        },
        "matched_entries": [
            {"tv_time": tv.entry_time, "engine_time": eng.entry_time,
             "tv_direction": tv.direction, "engine_direction": eng.direction}
            for tv, eng in result.matched
        ],
    }
    with _checked_path(strategy / "actual-result.json").open(
        "x", encoding="utf-8"
    ) as handle:
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

    def _assert_other_gates(self, result, row, shared, engine_count):
        self.assertTrue(result.tv_exists)
        self.assertTrue(result.eng_exists)
        self.assertFalse(result.no_aligned_trades)
        self.assertEqual(result.profile, "production")
        self.assertEqual(
            (result.tv_raw_count, result.eng_raw_count), (200, engine_count)
        )
        self.assertEqual((result.tv_count, result.eng_count), (200, engine_count))
        self.assertEqual(
            (result.tv_gate_count, result.eng_gate_count), (200, engine_count)
        )
        self.assertEqual((result.matched_count, result.gating_matched_count),
                         (200, 200))
        self.assertEqual(len(result.matched), 200)
        self.assertEqual((result.trim_bars, result.warmup_bars), (0, 0))
        self.assertIsNone(result.bounds)
        self.assertEqual(result.open_mark_pairs, 0)
        self.assertEqual(result.coverage_tv_count, 200)
        self.assertEqual(result.coverage, 1.0)
        self.assertEqual(result.unmatched_total, 0)
        self.assertEqual(result.unmatched_in_window, engine_count - 200)
        for field in (
            "entry_ok", "exit_ok", "pnl_ok", "coverage_ok",
            "distinct_entry_identity_ok",
        ):
            self.assertIs(getattr(result, field), True, field)
        self.assertEqual(result.distinct_entry_mismatches, 0)
        self.assertEqual(
            row["metric_observations"], {"entry": 200, "exit": 200, "pnl": 200}
        )
        policy = row["grading_policy"]
        self.assertEqual(policy["count_rule"], "exact")
        self.assertEqual(policy["profile"], result.profile)
        self.assertGreater(policy["thresholds"]["count"], 0.0)
        for metric in ("entry", "exit", "pnl"):
            self.assertEqual(len(getattr(result, metric + "_deltas")), 200)
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

    def test_gross_count_rejects_cancelling_unmatched_entries(self):
        """Gross unmatched counts cannot cancel across the two sides.

        Today's net rule gives count_abs_delta=0, count_ok=True, excellent.
        """
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        trades = [
            (start + timedelta(days=index),
             start + timedelta(days=index, hours=1), identity)
            for index, identity in enumerate(("A", "B", "C", "D"))
        ]
        a, b, c, d = trades
        self.assertLess(a[0], b[0])
        self.assertLess(b[0], c[0])
        self.assertLess(c[0], d[0])
        for earlier, later in zip(trades, trades[1:]):
            self.assertGreater((later[0] - earlier[0]).total_seconds(),
                               MATCH_WINDOW_SECONDS)

        result, row = _analyze_tapes(
            self.run_root, "cancelling-unmatched", [a, b, d], [a, c, d]
        )
        self.assertIsNone(result.bounds)
        self.assertEqual((result.trim_bars, result.warmup_bars), (0, 0))
        self.assertFalse(result.no_aligned_trades)
        self.assertEqual(result.open_mark_pairs, 0)
        self.assertEqual((result.tv_count, result.eng_count), (3, 3))
        self.assertEqual(result.tv_gate_count, result.eng_gate_count)
        self.assertEqual(result.tv_gate_count, 3)
        self.assertEqual((result.matched_count, result.gating_matched_count), (2, 2))
        self.assertEqual(
            [(tv.entry_signal, eng.entry_identity) for tv, eng in result.matched],
            [("A", "engine-A"), ("D", "engine-D")],
        )
        self.assertEqual(result.unmatched_total, 1)
        self.assertAlmostEqual(result.coverage, 2 / 3)
        for field in (
            "entry_ok", "exit_ok", "pnl_ok", "coverage_ok",
            "distinct_entry_identity_ok",
        ):
            self.assertIs(getattr(result, field), True, field)
        self.assertEqual(result.count_delta, 0.0)
        self.assertEqual(result.count_abs_delta, 2)
        self.assertEqual(row["count_abs_delta"], 2)
        self.assertIs(result.count_ok, False)
        self.assertEqual(result.tier, "strong")
        self.assertEqual(row["tier"], "strong")

    def test_gross_count_zero_alignment_sums_both_compared_lists(self):
        """With zero alignment, every compared trade is unmatched.

        Today's net rule gives count_abs_delta=0, count_ok=True, minimal.
        """
        entry = datetime(2025, 1, 1, tzinfo=timezone.utc)
        trade = (entry, entry + timedelta(hours=1), "opposite-direction")
        result, row = _analyze_configured_tapes(
            self.run_root, "zero-alignment", [trade], [trade], short_engine=True
        )
        self.assertTrue(result.no_aligned_trades)
        self.assertEqual(result.matched, [])
        self.assertEqual(result.matched_count, 0)
        self.assertEqual(result.open_mark_pairs, 0)
        self.assertEqual((result.tv_raw_count, result.eng_raw_count), (1, 1))
        self.assertEqual((result.tv_count, result.eng_count), (1, 1))
        self.assertEqual(result.count_delta, 0.0)
        self.assertEqual(result.tier, "minimal")
        self.assertEqual(row["tier"], "minimal")
        self.assertEqual(result.count_abs_delta, 2)
        self.assertEqual(row["count_abs_delta"], 2)
        self.assertIs(result.count_ok, False)

    def test_gross_count_keeps_pair_straddling_interior_bound_matched(self):
        """Any alignment pair stays matched across the interior boundary.

        Today's net rule gives count_abs_delta=1, count_ok=False, moderate.
        """
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        tv_trades = [
            (start + timedelta(hours=hour),
             start + timedelta(hours=hour + 1), identity)
            for hour, identity in ((1, "boundary"), (3, "middle"), (5, "last"))
        ]
        engine_boundary = start + timedelta(minutes=30)
        engine_trades = [
            (engine_boundary, tv_trades[0][1], "boundary"),
            *tv_trades[1:],
        ]
        self.assertLessEqual(
            (tv_trades[0][0] - engine_boundary).total_seconds(),
            MATCH_WINDOW_SECONDS,
        )
        result, row = _analyze_configured_tapes(
            self.run_root, "straddling-interior", tv_trades, engine_trades,
            trim_bars=1,
            bar_times=[start + timedelta(hours=hour) for hour in range(8)],
        )
        lo = int((start + timedelta(hours=1)).timestamp()) * 1000
        hi = int((start + timedelta(hours=6)).timestamp()) * 1000
        self.assertEqual(result.bounds, (lo, hi))
        self.assertEqual((result.trim_bars, result.warmup_bars), (1, 0))
        self.assertEqual((result.tv_count, result.eng_count), (3, 3))
        self.assertEqual((result.tv_gate_count, result.eng_gate_count), (3, 2))
        self.assertEqual((result.matched_count, result.gating_matched_count), (3, 3))
        self.assertEqual(result.open_mark_pairs, 0)
        self.assertFalse(result.no_aligned_trades)
        self.assertEqual(
            [(tv.entry_time, eng.entry_time) for tv, eng in result.matched],
            [(int(tv[0].timestamp()), int(eng[0].timestamp()))
             for tv, eng in zip(tv_trades, engine_trades)],
        )
        boundary_tv, boundary_engine = result.matched[0]
        self.assertLess(boundary_engine.entry_time * 1000, lo)
        self.assertGreaterEqual(boundary_tv.entry_time * 1000, lo)
        self.assertLessEqual(boundary_tv.entry_time * 1000, hi)
        self.assertEqual(result.coverage, 1.0)
        self.assertEqual(result.unmatched_total, 0)
        for field in (
            "entry_ok", "exit_ok", "pnl_ok", "coverage_ok",
            "distinct_entry_identity_ok",
        ):
            self.assertIs(getattr(result, field), True, field)
        self.assertAlmostEqual(result.count_delta, 1 / 3, places=15)
        self.assertEqual(result.count_abs_delta, 0)
        self.assertEqual(row["count_abs_delta"], 0)
        self.assertIs(result.count_ok, True)
        self.assertEqual(result.tier, "excellent")
        self.assertEqual(row["tier"], "excellent")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--retain-artifacts", type=Path, metavar="DIR")
    options, unittest_args = parser.parse_known_args()
    ExactCountBehaviorTests.retain_artifacts = options.retain_artifacts
    unittest.main(argv=[sys.argv[0], *unittest_args])
