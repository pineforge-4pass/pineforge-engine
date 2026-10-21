#!/usr/bin/env python3
"""Release gate decisions and text pins; no subprocesses or network in tests."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from ci_preflight import _jobs, _steps
import release_gate


ROOT = Path(__file__).resolve().parents[1]
REPO = "example/engine"
SHA = "a" * 40
HEAD = "b" * 40
TREE = "c" * 40
OTHER_HEAD = "d" * 40
OTHER_TREE = "e" * 40
MAIN_API = f"repos/{REPO}/commits/main"
PULLS_API = f"repos/{REPO}/commits/{SHA}/pulls"
STATUSES_API = f"repos/{REPO}/commits/{HEAD}/statuses?per_page=100"
GATE_NAME = "Refuse unless pineforge/verify and pineforge/parity are green"


def commit(sha, tree):
    return {"sha": sha, "commit": {"tree": {"sha": tree}}}


def pull(head=HEAD, number=17, merged_at="2026-10-20T12:00:00Z"):
    return {"number": number, "merged_at": merged_at, "head": {"sha": head}}


def successes():
    return [{"context": "pineforge/verify", "state": "success"},
            {"context": "pineforge/parity", "state": "success"}]


def green_statuses():
    return successes() + [
        {"context": "pineforge/verify", "state": "failure"},
        {"context": "pineforge/parity", "state": "pending"},
    ]


class FakeAPI:
    """Raw gh responses keyed by exact endpoint, including jq/pagination flags."""

    def __init__(self):
        self.responses = {
            MAIN_API: SHA + "\n",
            f"repos/{REPO}/commits/{SHA}": json.dumps(commit(SHA, TREE)),
            PULLS_API: json.dumps([pull()]),
            f"repos/{REPO}/commits/{HEAD}": json.dumps(commit(HEAD, TREE)),
            STATUSES_API: json.dumps(successes()),
        }
        self.calls = []

    def __call__(self, endpoint, *, paginate=False, jq=None):
        self.calls.append((endpoint, paginate, jq))
        if endpoint not in self.responses:
            raise AssertionError(f"unexpected API endpoint: {endpoint}")
        response = self.responses[endpoint]
        if isinstance(response, Exception):
            raise response
        return response

    def statuses(self, rows):
        self.responses[STATUSES_API] = json.dumps(rows)


class GateTests(unittest.TestCase):
    def setUp(self):
        self.api = FakeAPI()

    def run_gate(self, *, repo=REPO, sha=SHA, ref="refs/heads/main"):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = release_gate.main(
                ["gate", "--repo", repo, "--sha", sha, "--ref", ref], api=self.api)
        return code, stdout.getvalue(), stderr.getvalue()

    def assert_refused(self, fragment, **kwargs):
        code, stdout, stderr = self.run_gate(**kwargs)
        self.assertEqual(code, 1, (stdout, stderr))
        self.assertIn("::error::", stderr)
        self.assertIn(fragment, stderr)
        self.assertEqual(stdout, "", "a refusal must never print a green line")

    def test_green_reads_both_newest_statuses_and_names_the_reviewed_tree(self):
        self.api.statuses(green_statuses())
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertEqual(stderr, "")
        self.assertEqual(stdout, (
            f"release gate green: PR #17 head {HEAD}; pineforge/verify=success; "
            f"pineforge/parity=success; tree {TREE}\n"))
        self.assertIn((MAIN_API, False, ".sha"), self.api.calls)
        self.assertIn((PULLS_API, True, None), self.api.calls)
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_missing_verify_refuses(self):
        self.api.statuses([successes()[1]])
        self.assert_refused("missing pineforge/verify")

    def test_missing_parity_refuses(self):
        self.api.statuses([successes()[0]])
        self.assert_refused("missing pineforge/parity")

    def test_pending_refuses(self):
        rows = successes()
        rows[0]["state"] = "pending"
        self.api.statuses(rows)
        self.assert_refused("pineforge/verify=pending (expected success)")

    def test_failure_refuses(self):
        rows = successes()
        rows[1]["state"] = "failure"
        self.api.statuses(rows)
        self.assert_refused("pineforge/parity=failure (expected success)")

    def test_error_state_refuses(self):
        rows = successes()
        rows[0]["state"] = "error"
        self.api.statuses(rows)
        self.assert_refused("pineforge/verify=error (expected success)")

    def test_statuses_on_a_different_sha_cannot_green_the_matching_head(self):
        wrong_api = f"repos/{REPO}/commits/{OTHER_HEAD}/statuses?per_page=100"
        self.api.responses[wrong_api] = json.dumps(successes())
        self.api.statuses([])
        self.assert_refused(f"API {STATUSES_API} returned an empty payload")
        self.assertIn((STATUSES_API, True, None), self.api.calls)
        self.assertFalse(any(endpoint == wrong_api for endpoint, _, _ in self.api.calls))

    def test_merged_head_with_a_different_tree_refuses(self):
        self.api.responses[f"repos/{REPO}/commits/{HEAD}"] = json.dumps(
            commit(HEAD, OTHER_TREE))
        self.assert_refused(f"no merged pull request head matches release tree {TREE}")

    def test_no_merged_pull_request_refuses(self):
        self.api.responses[PULLS_API] = json.dumps([pull(merged_at=None)])
        self.assert_refused(f"no merged pull request associated with {SHA}")

    def test_no_associated_pull_requests_refuses(self):
        self.api.responses[PULLS_API] = "[]"
        self.assert_refused(f"API {PULLS_API} returned an empty payload")

    def test_non_main_ref_refuses_before_any_api_call(self):
        self.assert_refused("release requires refs/heads/main, got refs/tags/v2.0.0",
                            ref="refs/tags/v2.0.0")
        self.assertEqual(self.api.calls, [])

    def test_sha_must_equal_the_live_main_head(self):
        self.api.responses[MAIN_API] = OTHER_HEAD + "\n"
        self.assert_refused(f"release sha {SHA} is not current main head {OTHER_HEAD}")
        self.assertEqual(self.api.calls, [(MAIN_API, False, ".sha")])

    def test_api_error_preserves_the_exact_stderr(self):
        self.api.responses[PULLS_API] = subprocess.CalledProcessError(
            1, ["gh", "api", PULLS_API], stderr="gh: API rate limit exceeded (HTTP 403)\n")
        self.assert_refused(
            f"API {PULLS_API} failed: gh: API rate limit exceeded (HTTP 403)")

    def test_non_json_payload_refuses_with_the_parse_error(self):
        self.api.responses[STATUSES_API] = "not JSON"
        self.assert_refused(
            f"API {STATUSES_API} returned invalid JSON: "
            "Expecting value: line 1 column 1 (char 0)")

    def test_empty_payload_refuses(self):
        self.api.responses[MAIN_API] = " \n"
        self.assert_refused(f"API {MAIN_API} returned an empty payload")

    def test_newest_failure_cannot_be_masked_by_an_older_success(self):
        self.api.statuses([
            {"context": "pineforge/verify", "state": "failure"},
            *successes(),
        ])
        self.assert_refused("pineforge/verify=failure (expected success)")

    def test_green_fixture_fails_when_either_context_is_removed(self):
        for removed in ("pineforge/verify", "pineforge/parity"):
            with self.subTest(removed=removed):
                self.api = FakeAPI()
                self.api.statuses([row for row in green_statuses() if row["context"] != removed])
                self.assert_refused(f"missing {removed}")

    def test_status_pages_preserve_first_occurrence_across_page_boundaries(self):
        self.api.responses[STATUSES_API] = (
            json.dumps([successes()[0]]) + "\n" + json.dumps([
                successes()[1], {"context": "pineforge/verify", "state": "failure"}]))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn("pineforge/verify=success; pineforge/parity=success", stdout)

    def test_failure_on_the_first_page_cannot_be_masked_by_later_success(self):
        self.api.responses[STATUSES_API] = (
            json.dumps([{"context": "pineforge/verify", "state": "failure"}])
            + "\n" + json.dumps(successes()))
        self.assert_refused("pineforge/verify=failure (expected success)")

    def test_valid_first_page_cannot_hide_a_non_json_later_page(self):
        self.api.responses[STATUSES_API] = json.dumps(successes()) + "\nnot JSON"
        self.assert_refused(
            f"API {STATUSES_API} returned invalid JSON: "
            "Expecting value: line 1 column 1 (char 0)")

    def test_newest_matching_pr_wins_over_older_green_pr(self):
        newer = pull(OTHER_HEAD, number=18, merged_at="2026-10-21T12:00:00Z")
        self.api.responses[PULLS_API] = json.dumps([pull(), newer])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = json.dumps(
            commit(OTHER_HEAD, TREE))
        newer_api = f"repos/{REPO}/commits/{OTHER_HEAD}/statuses?per_page=100"
        self.api.responses[newer_api] = json.dumps([
            {"context": "pineforge/verify", "state": "pending"}, successes()[1]])
        self.assert_refused(
            f"PR #18 head {OTHER_HEAD}: pineforge/verify=pending (expected success)")
        self.assertFalse(any(endpoint == STATUSES_API for endpoint, _, _ in self.api.calls))

    def test_newer_nonmatching_pr_does_not_hide_a_matching_pr(self):
        newer = pull(OTHER_HEAD, number=18, merged_at="2026-10-21T12:00:00Z")
        self.api.responses[PULLS_API] = json.dumps([newer, pull()])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = json.dumps(
            commit(OTHER_HEAD, OTHER_TREE))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"PR #17 head {HEAD}", stdout)

    def test_unexpected_api_shapes_refuse(self):
        for endpoint, payload, message in (
                (PULLS_API, "{}", "an empty payload"),
                (PULLS_API, '{"message": "Not Found"}',
                 "a non-array JSON page (expected a JSON array)"),
                (PULLS_API, '[{"number": 17}]', "invalid pull request"),
                (PULLS_API, json.dumps([pull(merged_at="not a date")]), "invalid merged_at"),
                (f"repos/{REPO}/commits/{HEAD}", json.dumps({"sha": HEAD}),
                 "missing or invalid commit.tree.sha"),
                (STATUSES_API, "null", "an empty payload"),
                (STATUSES_API, "[null]", "invalid status row"),
                (STATUSES_API, '[{"context": "pineforge/verify"}]', "invalid status state")):
            with self.subTest(endpoint=endpoint, payload=payload):
                self.api = FakeAPI()
                self.api.responses[endpoint] = payload
                self.assert_refused(f"API {endpoint} returned {message}")

    def test_each_api_read_fails_closed_on_transport_parse_and_empty_errors(self):
        endpoints = [MAIN_API, PULLS_API, f"repos/{REPO}/commits/{SHA}",
                     f"repos/{REPO}/commits/{HEAD}", STATUSES_API]
        for endpoint in endpoints:
            for payload, message in ((OSError("connection refused"), "failed: connection refused"),
                                     ("", "returned an empty payload"),
                                     ("not JSON", "returned invalid main sha" if endpoint == MAIN_API
                                      else "returned invalid JSON")):
                with self.subTest(endpoint=endpoint, payload=payload):
                    self.api = FakeAPI()
                    self.api.responses[endpoint] = payload
                    self.assert_refused(f"API {endpoint} {message}")

    def test_repo_and_sha_are_not_special_cased(self):
        other_repo = "another/project"
        other_sha, other_head, other_tree = "1" * 40, "2" * 40, "3" * 40
        self.api.responses = {
            f"repos/{other_repo}/commits/main": other_sha,
            f"repos/{other_repo}/commits/{other_sha}": json.dumps(commit(other_sha, other_tree)),
            f"repos/{other_repo}/commits/{other_sha}/pulls": json.dumps([pull(other_head, number=902)]),
            f"repos/{other_repo}/commits/{other_head}": json.dumps(commit(other_head, other_tree)),
            f"repos/{other_repo}/commits/{other_head}/statuses?per_page=100": json.dumps(successes()),
        }
        code, stdout, stderr = self.run_gate(repo=other_repo, sha=other_sha)
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"PR #902 head {other_head}", stdout)
        self.assertIn(f"tree {other_tree}", stdout)


class APICommandTests(unittest.TestCase):
    def test_gh_adapter_paginates_the_statuses_list(self):
        with mock.patch.object(release_gate.subprocess, "run") as run:
            run.return_value.stdout = "[]\n"
            self.assertEqual(release_gate.gh_api(STATUSES_API, paginate=True), "[]\n")
        run.assert_called_once_with(["gh", "api", STATUSES_API, "--paginate"],
                                    check=True, capture_output=True, text=True)

    def test_gh_adapter_reads_live_main_sha_with_jq(self):
        with mock.patch.object(release_gate.subprocess, "run") as run:
            run.return_value.stdout = SHA + "\n"
            self.assertEqual(release_gate.gh_api(MAIN_API, jq=".sha"), SHA + "\n")
        run.assert_called_once_with(["gh", "api", MAIN_API, "--jq", ".sha"],
                                    check=True, capture_output=True, text=True)


class WorkflowPinTests(unittest.TestCase):
    def workflow(self):
        return (ROOT / ".github/workflows/release.yml").read_text()

    def assert_gate_pin(self, workflow):
        steps = _steps(_jobs(workflow).get("release", ""))
        names = [step.splitlines()[0].removeprefix("      - name: ") for step in steps]
        self.assertEqual(names.count(GATE_NAME), 1, "release job must contain one gate step")
        index = names.index(GATE_NAME)
        self.assertGreater(index, names.index("Configure git identity"),
                           "release gate must follow Configure git identity")
        self.assertLess(index, names.index("Write bumped VERSION, commit, tag, push"),
                        "release gate must precede commit, tag, push")
        self.assertLess(index, names.index("Compute next version"),
                        "release gate must precede Compute next version")
        gate = steps[index]
        self.assertRegex(gate, r"(?m)^        id: gate$", "release gate id must be gate")
        self.assertNotRegex(gate, r"(?m)^        if\s*:",
                            "release gate must run for dry runs too")
        self.assertIn("python3 scripts/release_gate.py gate \\\n", gate)
        for name in ("GH_TOKEN", "GITHUB_REPOSITORY", "GITHUB_SHA", "GITHUB_REF"):
            self.assertRegex(gate, rf"(?m)^          {name}:\s+\$\{{\{{ .+ \}}\}}$",
                             f"release gate must pass {name} through env")
        for flag, name in (("repo", "GITHUB_REPOSITORY"), ("sha", "GITHUB_SHA"),
                           ("ref", "GITHUB_REF")):
            self.assertIn(f'--{flag} "${{{name}}}"', gate)
        self.assertNotIn("${{", gate.split("        run: |\n", 1)[1],
                         "run script must receive interpolations through env")

    def gate_block(self, workflow):
        return next(step for step in _steps(_jobs(workflow)["release"])
                    if step.splitlines()[0] == "      - name: " + GATE_NAME)

    def assert_mutation_refused(self, workflow, changed, message):
        self.assertNotEqual(changed, workflow, "the mutation must change the input")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "release.yml"
            path.write_text(changed)
            with self.assertRaisesRegex(AssertionError, message):
                self.assert_gate_pin(path.read_text())

    def test_release_gate_is_unconditional_and_precedes_every_release_write(self):
        self.assert_gate_pin(self.workflow())

    def test_pin_fails_when_gate_is_deleted(self):
        workflow = self.workflow()
        changed = workflow.replace(self.gate_block(workflow), "", 1)
        self.assert_mutation_refused(workflow, changed, "release job must contain one gate step")

    def test_pin_fails_when_gate_is_moved_after_push(self):
        workflow = self.workflow()
        gate = self.gate_block(workflow)
        changed = workflow.replace(gate, "", 1)
        push = next(step for step in _steps(_jobs(changed)["release"])
                    if step.startswith("      - name: Write bumped VERSION, commit, tag, push\n"))
        changed = changed.replace(push, push + "\n" + gate, 1)
        self.assert_mutation_refused(workflow, changed, "release gate must precede commit, tag, push")

    def test_pin_fails_when_gate_skips_dry_runs(self):
        workflow = self.workflow()
        gate = self.gate_block(workflow)
        changed = workflow.replace(gate, gate.replace(
            "        id: gate\n", "        id: gate\n        if: inputs.dry_run\n", 1), 1)
        self.assert_mutation_refused(workflow, changed, "release gate must run for dry runs too")


if __name__ == "__main__":
    unittest.main()
