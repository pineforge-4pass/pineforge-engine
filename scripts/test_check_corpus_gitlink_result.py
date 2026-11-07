#!/usr/bin/env python3
"""Check the corpus gate against recorded GitHub response shapes."""
import copy
import json
from pathlib import Path
import unittest
from unittest import mock

import check_corpus_gitlink_result as gate


REAL = Path(__file__).parent / "fixtures" / "corpus_gate" / "real"
RECORDED = json.loads(
    (REAL / "check-runs-engine-14d7d9b3-realshape.json").read_text())
NEW = RECORDED["check_runs"][0]["head_sha"]
OLD = "8425ca820c4b7272db24d659a64d88e8e05b6407"


def success():
    row = copy.deepcopy(RECORDED["check_runs"][0])
    row["name"] = "corpus-validate"
    return {"total_count": 1, "check_runs": [row]}


class CorpusCheckResultTests(unittest.TestCase):
    def test_exact_success_and_single_field_refusals(self):
        good = success()
        self.assertEqual(gate.check_result(good, NEW)[0], 0)
        mutations = (
            ("head_sha", OLD, "head_sha"),
            ("name", "another-check", "missing"),
            ("status", "queued", "queued"),
            ("status", "in_progress", "in_progress"),
            ("conclusion", "failure", "failure"),
            ("conclusion", "cancelled", "cancelled"),
            ("conclusion", "neutral", "neutral"),
            ("conclusion", "skipped", "skipped"),
            ("conclusion", "timed_out", "timed_out"),
            ("conclusion", "action_required", "action_required"),
            ("conclusion", None, "None"),
            ("completed_at", None, "completed_at"),
        )
        for field, value, message in mutations:
            with self.subTest(field=field, value=value):
                response = copy.deepcopy(good)
                response["check_runs"][0][field] = value
                code, reason = gate.check_result(response, NEW)
                self.assertNotEqual(code, 0)
                self.assertIn(message, reason)
                print(f"{field}={value}: exit={code}: {reason}")

    def test_another_app_cannot_supply_the_check(self):
        response = success()
        response["check_runs"][0]["app"]["id"] = 1
        code, reason = gate.check_result(response, NEW)
        self.assertEqual(code, 1)
        self.assertIn("app", reason)

    def test_both_real_corpus_responses_are_missing(self):
        for filename in ("check-runs-corpus-adef1c63.json",
                         "check-runs-corpus-8425ca82.json"):
            with self.subTest(filename=filename):
                response = json.loads((REAL / filename).read_text())
                code, reason = gate.check_result(response, NEW)
                self.assertEqual(code, 1)
                self.assertIn("missing", reason)
                print(f"{filename}: exit={code}: {reason}")

    def test_newest_completion_decides_in_either_api_order(self):
        old = success()["check_runs"][0]
        newer = copy.deepcopy(old)
        newer["completed_at"] = "2099-01-01T00:00:00Z"
        newer["conclusion"] = "failure"
        for rows in ([old, newer], [newer, old]):
            code, reason = gate.check_result(
                {"total_count": 2, "check_runs": rows}, NEW)
            self.assertEqual(code, 1)
            self.assertIn("failure", reason)

    def test_pending_rerun_does_not_reuse_an_old_success(self):
        response = success()
        pending = copy.deepcopy(response["check_runs"][0])
        pending.update(status="in_progress", conclusion=None, completed_at=None)
        response["check_runs"].append(pending)
        response["total_count"] = 2
        self.assertEqual(gate.check_result(response, NEW)[0], 1)

    def test_unreadable_or_truncated_api_result_cannot_pass(self):
        for response in ({}, {"total_count": 1, "check_runs": []},
                         {"total_count": 101, "check_runs": []}):
            with self.subTest(response=response):
                self.assertEqual(gate.check_result(response, NEW)[0], 2)

    def test_unchanged_gitlink_never_calls_the_api(self):
        with mock.patch.object(gate, "gitlink", side_effect=[OLD, OLD]), \
                mock.patch.object(gate, "read_checks") as api:
            self.assertEqual(gate.main(["--base", OLD, "--head", NEW]), 0)
        api.assert_not_called()

    def test_changed_gitlink_reads_exactly_the_new_commit_once(self):
        with mock.patch.object(gate, "gitlink", side_effect=[OLD, NEW]), \
                mock.patch.object(gate, "read_checks", return_value=success()) as api:
            self.assertEqual(gate.main(["--base", OLD, "--head", NEW]), 0)
        api.assert_called_once_with("pineforge-4pass/pineforge-corpus", NEW)

    def test_gitlink_requires_a_submodule_entry(self):
        self.assertEqual(gate.parse_gitlink(
            f"160000 commit {OLD}\tcorpus\n"), OLD)
        for text in ("", f"100644 blob {OLD}\tcorpus\n",
                     f"160000 commit {OLD}\tother\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gate.parse_gitlink(text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
