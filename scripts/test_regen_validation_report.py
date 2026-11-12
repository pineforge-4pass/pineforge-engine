#!/usr/bin/env python3
"""Renderer regressions using the real analyzer's synthetic trade fixtures."""

from __future__ import annotations

import unittest
from copy import deepcopy
from unittest.mock import patch

import regen_validation_report as report
from test_verify_corpus_trail_profile import (
    STRICT_STRATEGY,
    TRAIL_STRATEGY,
    _analyze,
)


# TRAIL_STRATEGY declares the production policy. Keep this fixture oracle
# independent of both the resolver and the returned report-row snapshot.
PERCENTILE_FIXTURE_THRESHOLDS = {
    "count": 0.01, "entry": 0.0001, "exit": 0.0002, "pnl": 0.01,
}


class ValidationReportTests(unittest.TestCase):
    def table(self, markdown: str, section: str) -> list[dict[str, str]]:
        body = markdown.split(f"## {section}\n", 1)[1].split("\n## ", 1)[0]
        lines = [line for line in body.splitlines() if line.startswith("|")]
        self.assertGreaterEqual(len(lines), 3)
        headings = [cell.strip() for cell in lines[0].split("|")[1:-1]]
        rows = []
        for line in lines[2:]:
            cells = [cell.strip() for cell in line.split("|")[1:-1]]
            self.assertEqual(len(cells), len(headings))
            rows.append(dict(zip(headings, cells)))
        return rows

    def render(self, *results) -> str:
        return report._emit(
            [result.report_row() for result in results],
            "engine-receipt", "corpus-receipt", blob_url="",
        )

    def test_real_tail_metrics_and_counts_are_not_dropped_or_copied_from_p90(self):
        # Written before the consumer fix: the saved renderer has none of
        # these p100/count columns. Managed RED must use this same test file.
        for size, entry_price, exit_price, tv_pnl, engine_pnl in (
            (12, 100.04, 100.03, 20.0, 21.0),
            (23, 99.96, 99.95, -10.0, -7.5),
        ):
            with self.subTest(size=size):
                entries = [100.0] * size
                exits = [100.0] * size
                pnls = [tv_pnl] * size
                entries[2] = entry_price
                exits[5] = exit_price
                pnls[-1] = engine_pnl
                result = _analyze(
                    "synthetic-render-tail", TRAIL_STRATEGY,
                    tv_exit_prices=[100.0] * size,
                    engine_exit_prices=exits,
                    tv_pnls=[tv_pnl] * size,
                    engine_pnls=pnls,
                    engine_entry_prices=entries,
                )
                markdown = self.render(result)
                for column in (
                    "Entry p100", "Exit p100", "PnL p100",
                    "Entry observations", "Exit observations", "PnL observations",
                    "Applied grading policy",
                ):
                    self.assertIn(f"| {column} |", markdown)
                detail, = self.table(markdown, "Per-Strategy Detail")
                expected = {
                    "Entry": abs(100.0 - entry_price) / max(100.0, abs(entry_price)),
                    "Exit": abs(100.0 - exit_price) / max(100.0, abs(exit_price)),
                    "PnL": abs(tv_pnl - engine_pnl) / abs(tv_pnl),
                }
                for metric, worst in expected.items():
                    self.assertEqual(detail[f"{metric} p90"], f"{0.0:.4f}%")
                    self.assertEqual(detail[f"{metric} p100"], f"{worst * 100:.4f}%")
                    self.assertNotEqual(
                        detail[f"{metric} p90"], detail[f"{metric} p100"]
                    )
                    self.assertEqual(detail[f"{metric} observations"], str(size))
                slug = result.strategy_dir.name
                self.assertEqual(detail["Slug"], f"[`{slug}`](./validation/{slug}/)")
                self.assertEqual(detail["Category"], "`synthetic`")
                self.assertEqual(detail["Tier"], "excellent")
                self.assertEqual(detail["Profile"], "`production`")
                for column in ("TV", "Engine", "Matched"):
                    self.assertEqual(detail[column], str(size))
                self.assertEqual(detail["Count Δ"], f"{0.0:.4f}%")
                self.assertIn("**Total probes verified:** 1", markdown)
                self.assertIn("**Excellent:** 1 (100.0%)", markdown)
                self.assertIn("| `synthetic` | 1 | 1 |", markdown)
                self.assertIn("effective metric observations", markdown)
                self.assertNotIn("relative deltas across matched\ntrades", markdown)

    def test_nonzero_p90_and_larger_p100_follow_the_fixture_values(self):
        size = 13
        entry_base, entry_tail = 100.01, 100.04
        exit_base, exit_tail = 100.015, 100.06
        tv_pnl, pnl_base, pnl_tail = 20.0, 20.1, 21.0
        result = _analyze(
            "synthetic-render-nonzero", TRAIL_STRATEGY,
            tv_exit_prices=[100.0] * size,
            engine_exit_prices=[exit_base] * (size - 1) + [exit_tail],
            tv_pnls=[tv_pnl] * size,
            engine_pnls=[pnl_base] * (size - 1) + [pnl_tail],
            engine_entry_prices=[entry_base] * (size - 1) + [entry_tail],
        )
        detail, = self.table(self.render(result), "Per-Strategy Detail")
        # With twelve equal deltas followed by one larger delta, both
        # interpolation neighbours for p90 have the base value.
        expected = {
            "Entry": (
                abs(100.0 - entry_base) / max(100.0, entry_base),
                abs(100.0 - entry_tail) / max(100.0, entry_tail),
            ),
            "Exit": (
                abs(100.0 - exit_base) / max(100.0, exit_base),
                abs(100.0 - exit_tail) / max(100.0, exit_tail),
            ),
            "PnL": (
                abs(tv_pnl - pnl_base) / abs(tv_pnl),
                abs(tv_pnl - pnl_tail) / abs(tv_pnl),
            ),
        }
        for metric, (p90, p100) in expected.items():
            self.assertEqual(detail[f"{metric} p90"], f"{p90 * 100:.4f}%")
            self.assertEqual(detail[f"{metric} p100"], f"{p100 * 100:.4f}%")
            self.assertNotEqual(detail[f"{metric} p90"], detail[f"{metric} p100"])
            self.assertEqual(detail[f"{metric} observations"], str(size))

    def test_no_eligible_pnl_and_observed_zero_remain_distinct(self):
        for tv_pnls, engine_pnls, eligible in (
            ([0.0, 0.005], [-5.0, 1.0], 0),
            ([None], [None], 0),
            ([10.0], [10.0], 1),
        ):
            with self.subTest(tv_pnls=tv_pnls):
                size = len(tv_pnls)
                result = _analyze(
                    "synthetic-render-presence", TRAIL_STRATEGY,
                    tv_exit_prices=[100.0] * size,
                    engine_exit_prices=[100.0] * size,
                    tv_pnls=tv_pnls, engine_pnls=engine_pnls,
                )
                detail, = self.table(self.render(result), "Per-Strategy Detail")
                expected = "no eligible observations" if eligible == 0 else "0.0000%"
                self.assertEqual(detail["PnL p90"], expected)
                self.assertEqual(detail["PnL p100"], expected)
                self.assertEqual(detail["PnL observations"], str(eligible))
                for metric in ("Entry", "Exit"):
                    self.assertEqual(detail[f"{metric} p100"], "0.0000%")
                    self.assertEqual(detail[f"{metric} observations"], str(size))

    def test_nonexcellent_anomaly_uses_the_same_missing_observation_semantics(self):
        result = _analyze(
            "synthetic-render-anomaly", STRICT_STRATEGY,
            tv_exit_prices=[100.0], engine_exit_prices=[100.03],
            tv_pnls=[0.0], engine_pnls=[-5.0],
        )
        self.assertEqual(result.tier, "strong")
        markdown = self.render(result)
        anomaly, = self.table(markdown, "Anomalies & Non-Excellent")
        self.assertEqual(anomaly["PnL p90"], "no eligible observations")
        self.assertEqual(anomaly["Reason"], result.notes)
        self.assertIn("**Strong:** 1", markdown)

    def test_metric_counts_follow_eligibility_instead_of_matched_count(self):
        tv_pnls = [0.0, 10.0, 0.005, -20.0]
        engine_pnls = [-1.0, 11.0, 7.0, -18.0]
        exits = [100.0, 100.04, 100.0, 100.05]
        result = _analyze(
            "synthetic-render-filter", TRAIL_STRATEGY,
            tv_exit_prices=[100.0] * len(exits), engine_exit_prices=exits,
            tv_pnls=tv_pnls, engine_pnls=engine_pnls,
        )
        detail, = self.table(self.render(result), "Per-Strategy Detail")
        eligible = [i for i, pnl in enumerate(tv_pnls) if abs(pnl) >= 0.01]
        worst_pnl = max(
            abs(tv_pnls[i] - engine_pnls[i]) / abs(tv_pnls[i]) for i in eligible
        )
        worst_exit = max(abs(100.0 - price) / max(100.0, price) for price in exits)
        self.assertEqual(detail["Matched"], str(len(exits)))
        self.assertEqual(detail["Entry observations"], str(len(exits)))
        self.assertEqual(detail["Exit observations"], str(len(exits)))
        self.assertEqual(detail["PnL observations"], str(len(eligible)))
        self.assertEqual(detail["PnL p100"], f"{worst_pnl * 100:.4f}%")
        self.assertEqual(detail["Exit p100"], f"{worst_exit * 100:.4f}%")

    def test_each_probe_displays_its_recorded_custom_policy_and_forced_profile(self):
        narrow = {"count": 0.017, "entry": 0.0003, "exit": 0.0001, "pnl": 0.01}
        wider = {"count": 0.023, "entry": 0.0007, "exit": 0.00025, "pnl": 0.02}
        cases = (
            (STRICT_STRATEGY, "production", narrow, "strong"),
            (TRAIL_STRATEGY, "strict", wider, "excellent"),
            (STRICT_STRATEGY, "production", wider, "excellent"),
        )
        results = []
        expected = []
        for index, (source, profile, thresholds, tier) in enumerate(cases):
            applied = dict(thresholds)
            with patch("verify_corpus.parity_for_profile", return_value=applied) as resolve:
                result = _analyze(
                    f"synthetic-render-policy-{index}", source,
                    tv_exit_prices=[100.0], engine_exit_prices=[100.015],
                    tv_pnls=[10.0], engine_pnls=[10.15],
                    metadata={"parity_profile": profile},
                )
                resolve.assert_called_once_with(profile)
            self.assertEqual(result.tier, tier)
            results.append(result)
            expected.append((profile, dict(thresholds), tier))
            applied["exit"] = 0.5
            applied["pnl"] = 0.75
        # A fresh factory call cannot recover the actual applied snapshot.
        with patch(
            "verify_corpus.parity_for_profile",
            side_effect=AssertionError("renderer must consume recorded policy"),
        ):
            markdown = self.render(*results)
        details = self.table(markdown, "Per-Strategy Detail")
        self.assertEqual(len(details), len(cases))
        for detail, (profile, thresholds, tier) in zip(details, expected):
            self.assertEqual(detail["Profile"], f"`{profile}`")
            self.assertEqual(detail["Tier"], tier)
            policy = detail["Applied grading policy"]
            for key, value in (
                ("profile", profile), ("aggregation", "p90"),
                ("percentile", "90%"), ("scope", "per_probe"),
                ("count_rule", "exact"), ("comparison", "<"),
                ("count_basis", "unmatched_both_sides"),
            ):
                self.assertIn(f"{key}={value}", policy)
            for metric, fraction in thresholds.items():
                self.assertIn(f"{metric}={fraction * 100:.4f}%", policy)
        self.assertNotEqual(
            details[0]["Applied grading policy"], details[2]["Applied grading policy"]
        )
        self.assertNotIn("detected trailing", markdown.lower())

    def test_invalid_recorded_percentile_is_not_recorded(self):
        result = _analyze(
            "synthetic-render-percentile", TRAIL_STRATEGY,
            tv_exit_prices=[100.0], engine_exit_prices=[100.0],
            tv_pnls=[10.0], engine_pnls=[10.0],
        )
        original = result.report_row()
        thresholds = original["grading_policy"].get("thresholds")
        self.assertIsInstance(
            thresholds, dict, "percentile fixture thresholds must be a dict",
        )
        self.assertEqual(
            set(thresholds), {"count", "entry", "exit", "pnl"},
            "percentile fixture threshold keyset must be count, entry, exit, pnl",
        )
        self.assertEqual(
            thresholds, PERCENTILE_FIXTURE_THRESHOLDS,
            "percentile fixture threshold values must match declared production policy",
        )
        cases = [({}, "not recorded")]
        cases.extend(
            ({"percentile": value}, "not recorded")
            for value in (
                None, "0.9", "invalid", False, True,
                [], [0.9], {}, {"value": 0.9},
                float("nan"), float("inf"), float("-inf"), -0.01, 1.01,
            )
        )
        cases.extend(
            ({"percentile": value}, f"{value * 100:g}%")
            for value in (0, 0.37, 0.9, 1)
        )
        for metadata, expected in cases:
            with self.subTest(metadata=metadata):
                row = deepcopy(original)
                row["grading_policy"].pop("percentile")
                row["grading_policy"].update(metadata)
                markdown = report._emit(
                    [row], "engine-receipt", "corpus-receipt", blob_url="",
                )
                detail, = self.table(markdown, "Per-Strategy Detail")
                policy = detail["Applied grading policy"]
                fields = policy.split("; ")
                self.assertIn(f"percentile={expected}", fields)
                for key in (
                    "profile", "aggregation", "scope", "count_rule", "comparison",
                ):
                    self.assertIn(f"{key}={original['grading_policy'][key]}", fields)
                for metric, fraction in PERCENTILE_FIXTURE_THRESHOLDS.items():
                    self.assertIn(f"{metric}={fraction * 100:.4f}%", policy)

    def test_percentile_fixture_threshold_guard_controls(self):
        keyset_guard = (
            "percentile fixture threshold keyset must be count, entry, exit, pnl"
        )
        values_guard = (
            "percentile fixture threshold values must match declared production policy"
        )
        cases = [
            ("healthy", dict(PERCENTILE_FIXTURE_THRESHOLDS), None),
            ("empty", {}, keyset_guard),
            ("missing-field", None, "percentile fixture thresholds must be a dict"),
            (
                "extra-metric",
                {**PERCENTILE_FIXTURE_THRESHOLDS, "qty": 0.02},
                keyset_guard,
            ),
        ]
        for metric in ("count", "entry", "exit", "pnl"):
            missing = dict(PERCENTILE_FIXTURE_THRESHOLDS)
            del missing[metric]
            cases.append((f"missing-{metric}", missing, keyset_guard))
            changed = dict(PERCENTILE_FIXTURE_THRESHOLDS)
            changed[metric] *= 2
            cases.append((f"wrong-{metric}", changed, values_guard))

        report_row = report.vc.VerificationResult.report_row
        for name, planted_thresholds, expected_guard in cases:
            with self.subTest(control=name):
                planted = []

                def controlled_report_row(result):
                    row = report_row(result)
                    self.assertEqual(row["slug"], "synthetic-render-percentile")
                    self.assertEqual(
                        row["grading_policy"]["thresholds"],
                        PERCENTILE_FIXTURE_THRESHOLDS,
                    )
                    # Corrupt only the report-facing snapshot after healthy
                    # analysis, avoiding an unrelated grader KeyError.
                    if planted_thresholds is None:
                        row["grading_policy"].pop("thresholds")
                    else:
                        row["grading_policy"]["thresholds"] = dict(planted_thresholds)
                    planted.append((
                        row["slug"], deepcopy(row["grading_policy"].get("thresholds")),
                    ))
                    return row

                target = ValidationReportTests(
                    "test_invalid_recorded_percentile_is_not_recorded"
                )
                outcome = unittest.TestResult()
                with patch.object(
                    report.vc.VerificationResult, "report_row",
                    new=controlled_report_row,
                ):
                    target.run(outcome)
                self.assertEqual(
                    planted, [("synthetic-render-percentile", planted_thresholds)]
                )
                self.assertEqual(outcome.testsRun, 1)
                self.assertEqual(outcome.errors, [])
                self.assertEqual(outcome.skipped, [])
                self.assertEqual(outcome.expectedFailures, [])
                self.assertEqual(outcome.unexpectedSuccesses, [])
                if expected_guard is None:
                    self.assertEqual(outcome.failures, [])
                else:
                    self.assertEqual(len(outcome.failures), 1)
                    failed_target, traceback = outcome.failures[0]
                    self.assertIs(failed_target, target)
                    self.assertIn(expected_guard, traceback)

    def test_empty_analysis_has_no_recorded_policy_or_eligible_metrics(self):
        result = _analyze(
            "synthetic-render-empty", TRAIL_STRATEGY,
            tv_exit_prices=[], engine_exit_prices=[], tv_pnls=[], engine_pnls=[],
        )
        detail, = self.table(self.render(result), "Per-Strategy Detail")
        self.assertEqual(detail["Applied grading policy"], "not recorded")
        for metric in ("Entry", "Exit", "PnL"):
            self.assertEqual(detail[f"{metric} p90"], "no eligible observations")
            self.assertEqual(detail[f"{metric} p100"], "no eligible observations")
            self.assertEqual(detail[f"{metric} observations"], "0")

    def test_legacy_row_without_new_fields_is_not_reported_and_does_not_crash(self):
        result = _analyze(
            "synthetic-render-legacy", STRICT_STRATEGY,
            tv_exit_prices=[100.0], engine_exit_prices=[100.0],
            tv_pnls=[10.0], engine_pnls=[10.0],
        )
        row = result.report_row()
        new_keys = {
            "entry_p100", "exit_p100", "pnl_p100", "metric_observations",
            "metric_missing", "grading_policy", "tv_exists", "eng_exists",
            "no_aligned_trades",
        }
        legacy = {key: value for key, value in row.items() if key not in new_keys}
        markdown = report._emit([legacy], "engine-receipt", "corpus-receipt")
        detail, = self.table(markdown, "Per-Strategy Detail")
        self.assertEqual(detail["Applied grading policy"], "not recorded")
        for metric in ("Entry", "Exit", "PnL"):
            self.assertEqual(detail[f"{metric} p100"], "not reported")
            self.assertEqual(detail[f"{metric} observations"], "not reported")
            self.assertEqual(detail[f"{metric} p90"], "0.0000%")
        slug = result.strategy_dir.name
        self.assertEqual(
            detail["Slug"],
            f"[`{slug}`]({report.CORPUS_REPO_BLOB}/validation/{slug}/)",
        )


if __name__ == "__main__":
    unittest.main()
