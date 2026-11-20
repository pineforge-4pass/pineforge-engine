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
from contextlib import redirect_stdout
from datetime import datetime, timedelta, timezone
from io import StringIO
from pathlib import Path
from unittest.mock import patch

import verify_corpus
from verify_corpus import MATCH_WINDOW_SECONDS, analyze_strategy, main
from test_verify_corpus_trail_profile import (
    STRICT_STRATEGY, TRADE_COLUMNS, TRAIL_STRATEGY,
)


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
REFERENCE_STRATEGY_ID = "validation/mtf-triple-tf-macd-hist-confluence-01"


def _reference_floor_tapes(replacements=(10,)):
    start = datetime(2025, 1, 1, tzinfo=timezone.utc)
    tv_trades = [
        (start + timedelta(days=index),
         start + timedelta(days=index, hours=1), f"entry-{index + 1}")
        for index in range(24)
    ]
    engine_trades = [
        (entry + timedelta(hours=12), exit_time + timedelta(hours=12),
         identity + "-replacement")
        if index in replacements else (entry, exit_time, identity)
        for index, (entry, exit_time, identity) in enumerate(tv_trades)
    ]
    return tv_trades, engine_trades


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
    tv_trades: list, engine_trades: list,
    *, write_trades=_write_trades,
    strategy_source=TRAIL_STRATEGY, aggregate=False,
    omit_tv=False, omit_engine=False,
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
    for filename in ("verify_corpus.py", "parity_paths.py"):
        print(str(_checked_path(Path(__file__).with_name(filename))), flush=True)
    # The real analyzer probes these OHLCV candidates. They are deliberately
    # absent in the fresh fixture directory; no trim implementation is replaced.
    strategy.mkdir(parents=True)
    with files["strategy.pine"].open("x", encoding="utf-8") as handle:
        handle.write(strategy_source)
    with files["inputs.json"].open("x", encoding="utf-8") as handle:
        json.dump({"tv_trades_csv_tz": "utc_plus_8"}, handle)
    if not omit_tv:
        write_trades(files["tv_trades.csv"], tv_trades, tv_timezone=True)
    if not omit_engine:
        write_trades(files["engine_trades.csv"], engine_trades, tv_timezone=False)

    result = analyze_strategy(strategy)
    row = result.report_row()
    cli_output = StringIO()
    corpus_root = run_root if aggregate else strategy
    cli_argv = [
        "verify_corpus.py", "--all" if aggregate else str(strategy),
        "--corpus-root", str(corpus_root),
        "--derived-workspace", str(corpus_root),
        "--harness-dir", str(corpus_root),
    ]
    with patch.object(sys, "argv", cli_argv), redirect_stdout(cli_output):
        cli_exit = main()
    cli = {
        "execution": "in_process_main",
        "argv": cli_argv,
        "exit": cli_exit,
        "stdout": cli_output.getvalue(),
    }
    # Retain actual observations before assertions, including on a failure.
    observations = {
        "report_row": row,
        "cli": cli,
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
                "tv_entry_price": tv.entry_price, "engine_entry_price": eng.entry_price,
                "tv_exit_price": tv.exit_price, "engine_exit_price": eng.exit_price,
                "tv_qty": tv.qty, "engine_qty": eng.qty,
                "tv_pnl": tv.pnl, "engine_pnl": eng.pnl,
            }
            for tv, eng in result.matched
        ],
    }
    with files["actual-result.json"].open("x", encoding="utf-8") as handle:
        json.dump(observations, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(json.dumps(row, sort_keys=True), flush=True)
    return result, row, cli


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
        # Count every unmatched trade on both sides, including substitutions
        # that leave the two pool sizes equal.
        gross_count_delta = (tv_count + engine_count - 2 * shared_count) / max(
            tv_count, engine_count, 1
        )
        self.assertEqual(result.count_delta, gross_count_delta)
        self.assertEqual(row["count_delta"], gross_count_delta)
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
        control, control_row, control_cli = _analyze_tapes(
            run_root, "equal", shared, shared
        )
        self._assert_other_gates(control, control_row, shared, 200)
        self.assertEqual(control.count_abs_delta, 0)
        self.assertEqual(control.count_delta, 0.0)
        self.assertIs(control.count_ok, True)
        self.assertEqual(control.tier, "excellent")
        self.assertEqual(control_cli["exit"], 0)
        self.assertRegex(
            control_cli["stdout"], r"Count delta:\s+0\.0000%\s+\(OK;"
        )
        self.assertIn("-> excellent", control_cli["stdout"])

        # A failed equal-count control stops this method before unequal analysis.
        unequal, unequal_row, unequal_cli = _analyze_tapes(
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
        self.assertEqual(unequal_cli["exit"], 0)
        self.assertIn("abs=1", unequal_cli["stdout"])
        self.assertIn("-> strong", unequal_cli["stdout"])

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
        control, control_row, control_cli = _analyze_tapes(
            self.run_root, "balanced-matched-control", tv_trades, tv_trades
        )
        self._assert_other_gates(control, control_row, tv_trades, 201)
        self.assertEqual(control.count_abs_delta, 0)
        self.assertEqual(control.count_delta, 0.0)
        self.assertIs(control.count_ok, True)
        self.assertEqual(control.tier, "excellent")
        self.assertEqual(control_cli["exit"], 0)
        self.assertRegex(
            control_cli["stdout"], r"Count delta:\s+0\.0000%\s+\(OK;"
        )
        self.assertIn("-> excellent", control_cli["stdout"])

        balanced, balanced_row, balanced_cli = _analyze_tapes(
            self.run_root, "balanced-unmatched", tv_trades, engine_trades
        )
        self._assert_other_gates(
            balanced, balanced_row, shared, 201, tv_count=201
        )
        # expectation corrected: 0 -> 2, because each unmatched TV/engine
        # entry contributes one to the canonical absolute mismatch count.
        # This assertion must fail on both the net grader and a grader that
        # keeps net counting here while exposing a separate gross field.
        self.assertEqual(
            balanced.count_abs_delta, 2,
            "count_abs_delta must count one unmatched TV trade plus one "
            "unmatched engine trade",
        )
        self.assertEqual(balanced_row["count_abs_delta"], 2)
        self.assertIs(
            balanced.count_ok, False,
            f"balanced unmatched trades cancelled: "
            f"delta={balanced.count_delta}, tier={balanced.tier}",
        )
        self.assertEqual(balanced.gross_unmatched_count, 2)
        self.assertEqual(balanced_row["gross_unmatched_count"], 2)
        # expectation corrected: 0 -> 2 / 201, because the percentage uses
        # the same two gross unmatched trades as the absolute count.
        self.assertEqual(balanced.count_delta, 2 / 201)
        self.assertLess(
            balanced.count_delta,
            balanced_row["grading_policy"]["thresholds"]["count"],
        )
        self.assertEqual(balanced.tier, "strong")
        self.assertNotEqual(balanced.tier, "excellent")
        self.assertEqual(balanced_cli["exit"], 0)
        self.assertRegex(
            balanced_cli["stdout"], r"Count delta:\s+0\.9950%\s+\(X;"
        )
        self.assertIn("abs=2", balanced_cli["stdout"])
        self.assertIn("net=0", balanced_cli["stdout"])
        self.assertIn("-> strong", balanced_cli["stdout"])
        self.assertEqual(balanced.net_count_abs_delta, 0)
        self.assertEqual(balanced_row["net_count_abs_delta"], 0)
        self.assertEqual(
            balanced_row["grading_policy"]["count_basis"], "unmatched_both_sides"
        )
        self.assertEqual(control.gross_unmatched_count, 0)
        self.assertEqual(control_row["gross_unmatched_count"], 0)
        self.assertEqual(control.net_count_abs_delta, 0)
        self.assertIn("abs=0", control_cli["stdout"])

    def test_strong_rejects_balanced_unmatched_at_gross_limit(self):
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        tv_trades = [
            (start + timedelta(days=index),
             start + timedelta(days=index, hours=1), f"entry-{index + 1}")
            for index in range(100)
        ]
        results = []
        for substitutions, expected_label in ((2, "strong"), (3, "moderate")):
            moved = set((20, 40, 60)[:substitutions])
            engine_trades = [
                (entry + timedelta(hours=12),
                 exit_time + timedelta(hours=12), identity + "-replacement")
                if index in moved else (entry, exit_time, identity)
                for index, (entry, exit_time, identity) in enumerate(tv_trades)
            ]
            self.assertEqual(engine_trades[0], tv_trades[0])
            self.assertEqual(engine_trades[-1], tv_trades[-1])
            result, row, cli = _analyze_tapes(
                self.run_root, f"strong-{substitutions}", tv_trades, engine_trades
            )
            self.assertEqual((result.tv_gate_count, result.eng_gate_count), (100, 100))
            self.assertEqual(result.gating_matched_count, 100 - substitutions)
            self.assertEqual(result.count_abs_delta, 2 * substitutions)
            self.assertEqual(result.net_count_abs_delta, 0)
            self.assertEqual(result.coverage, (100 - substitutions) / 100)
            self.assertGreaterEqual(result.coverage, 0.95)
            self.assertFalse(result.coverage_ok)  # Excellent still requires 99%.
            self.assertTrue(result.distinct_entry_identity_ok)
            for metric in ("entry", "exit", "pnl"):
                self.assertTrue(getattr(result, metric + "_ok"))
                self.assertEqual(getattr(result, metric + "_p90"), 0.0)
            # On the previous grader this is the first failing assertion:
            # the 6% gross case incorrectly remains strong at 0% net.
            self.assertEqual(
                result.label, expected_label,
                "strong must reject gross unmatched count at the 6% limit",
            )
            self.assertEqual(cli["exit"], 0 if expected_label == "strong" else 1)
            results.append((result, row, substitutions))

        for result, row, substitutions in results:
            self.assertEqual(result.count_delta, 2 * substitutions / 100)
            self.assertEqual(row["count_delta"], result.count_delta)

    def test_strong_accepts_gross_count_just_below_limit(self):
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        tv_trades = [
            (start + timedelta(days=index),
             start + timedelta(days=index, hours=1), f"entry-{index + 1}")
            for index in range(101)
        ]
        moved = {20, 40, 60}
        engine_trades = [
            (entry + timedelta(hours=12),
             exit_time + timedelta(hours=12), identity + "-replacement")
            if index in moved else (entry, exit_time, identity)
            for index, (entry, exit_time, identity) in enumerate(tv_trades)
        ]
        result, row, cli = _analyze_tapes(
            self.run_root, "strong-below-limit", tv_trades, engine_trades
        )
        self.assertEqual((result.tv_gate_count, result.eng_gate_count), (101, 101))
        self.assertEqual(result.gating_matched_count, 98)
        self.assertEqual(result.count_abs_delta, 6)
        self.assertEqual(result.net_count_abs_delta, 0)
        self.assertEqual(result.count_delta, 6 / 101)
        self.assertLess(result.count_delta, verify_corpus.STRONG_COUNT_DELTA)
        self.assertEqual(result.label, "strong")
        self.assertEqual(cli["exit"], 0)

    def test_one_sided_unmatched_counts_once_per_side(self):
        start = datetime(2025, 1, 1, tzinfo=timezone.utc)
        shared = [
            (start + timedelta(days=index),
             start + timedelta(days=index, hours=1), f"entry-{index + 1}")
            for index in range(200)
        ]
        extra_entry = shared[99][0] + timedelta(hours=12)
        extra = (extra_entry, extra_entry + timedelta(hours=1), "one-sided")
        with_extra = sorted([*shared, extra])
        for name, tv_trades, engine_trades in (
            ("tv-only", with_extra, shared),
            ("engine-only", shared, with_extra),
        ):
            with self.subTest(side=name):
                result, row, cli = _analyze_tapes(
                    self.run_root, f"one-sided-{name}", tv_trades, engine_trades
                )
                self.assertEqual(
                    (result.tv_gate_count, result.eng_gate_count),
                    (len(tv_trades), len(engine_trades)),
                )
                self.assertEqual(result.gating_matched_count, 200)
                self.assertEqual(result.count_abs_delta, 1)
                self.assertEqual(result.net_count_abs_delta, 1)
                self.assertEqual(result.gross_unmatched_count, 1)
                self.assertEqual(row["count_abs_delta"], 1)
                self.assertEqual(row["net_count_abs_delta"], 1)
                self.assertEqual(result.count_delta, 1 / 201)
                self.assertIs(result.count_ok, False)
                self.assertNotEqual(result.label, "excellent")
                self.assertIn("abs=1", cli["stdout"])
                self.assertIn("net=1", cli["stdout"])

    def _assert_reference_moderate(self, result, row):
        self.assertEqual(result.label, "moderate")
        self.assertEqual(row["tier"], "moderate")
        self.assertEqual(result.profile, "strict")
        self.assertEqual((result.tv_raw_count, result.eng_raw_count), (24, 24))
        self.assertEqual((result.tv_gate_count, result.eng_gate_count), (24, 24))
        self.assertEqual(result.gating_matched_count, 23)
        self.assertEqual(result.count_abs_delta, 2)
        self.assertEqual(result.net_count_abs_delta, 0)
        self.assertEqual(result.count_delta, 2 / 24)
        self.assertEqual(result.coverage, 23 / 24)
        for metric in ("entry", "exit", "pnl"):
            self.assertEqual(getattr(result, metric + "_p90"), 0.0)

    def test_reference_strategy_at_moderate_fails_the_floor(self):
        """#836/#883: the measured Moderate state of the reference id fails the floor."""
        result, row, cli = _analyze_tapes(
            self.run_root, REFERENCE_STRATEGY_ID, *_reference_floor_tapes(),
            strategy_source=STRICT_STRATEGY, aggregate=True,
        )
        self._assert_reference_moderate(result, row)
        self.assertIn("-> moderate", cli["stdout"])
        self.assertIn(
            "Verified 1 strategies — excellent=0, strong=0, moderate=1, "
            "weak=0, minimal=0, anomaly=0, engine_only=0, missing=0",
            cli["stdout"],
        )
        self.assertEqual(
            cli["exit"], 1,
            "the aggregate floor has no exception for the reference id (#836/#883)",
        )
        self.assertIn(f"Below strong tier:\n  {REFERENCE_STRATEGY_ID}", cli["stdout"])

    def test_reference_floor_rejects_other_moderate_ids(self):
        """#836/#883: every other full id retains the Strong aggregate floor."""
        basename = REFERENCE_STRATEGY_ID.split("/", 1)[1]
        for strategy_id in (
            "validation/other-moderate", f"validation/{basename}-variant",
            f"validation/prefix-{basename}", f"basic/{basename}",
            f"community/{basename}",
        ):
            with self.subTest(strategy_id=strategy_id):
                result, row, cli = _analyze_tapes(
                    self.run_root / strategy_id.replace("/", "-"),
                    strategy_id, *_reference_floor_tapes(),
                    strategy_source=STRICT_STRATEGY, aggregate=True,
                )
                self._assert_reference_moderate(result, row)
                self.assertEqual(cli["exit"], 1)
                self.assertIn("moderate=1", cli["stdout"])
                self.assertIn(f"Below strong tier:\n  {strategy_id}", cli["stdout"])

    def test_reference_floor_rejects_missing_or_lower_tiers(self):
        """#836/#883: the exact id still fails with missing input or a lower tier."""
        for name, replacements, omit_tv, omit_engine, expected_tier in (
            ("weak", (3, 7, 12, 18), False, False, "weak"),
            ("minimal", tuple(range(24)), False, False, "minimal"),
            ("missing-tv", (10,), True, False, "missing"),
            ("missing-engine", (10,), False, True, "missing"),
        ):
            with self.subTest(name=name):
                result, row, cli = _analyze_tapes(
                    self.run_root / name, REFERENCE_STRATEGY_ID,
                    *_reference_floor_tapes(replacements),
                    strategy_source=STRICT_STRATEGY, aggregate=True,
                    omit_tv=omit_tv, omit_engine=omit_engine,
                )
                self.assertEqual(result.label, expected_tier)
                self.assertEqual(row["tier"], expected_tier)
                self.assertEqual(cli["exit"], 1)
                self.assertIn(f"{expected_tier}=1", cli["stdout"])
                self.assertIn(
                    f"Below strong tier:\n  {REFERENCE_STRATEGY_ID}", cli["stdout"]
                )

    def test_reference_floor_does_not_change_single_strategy_exit(self):
        """#836/#883: single-strategy verification of the reference id exits 1 at Moderate."""
        result, row, cli = _analyze_tapes(
            self.run_root, REFERENCE_STRATEGY_ID, *_reference_floor_tapes(),
            strategy_source=STRICT_STRATEGY,
        )
        self._assert_reference_moderate(result, row)
        self.assertEqual(cli["exit"], 1)
        self.assertIn("-> moderate", cli["stdout"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--retain-artifacts", type=Path, metavar="DIR")
    options, unittest_args = parser.parse_known_args()
    ExactCountBehaviorTests.retain_artifacts = options.retain_artifacts
    unittest.main(argv=[sys.argv[0], *unittest_args])
