#!/usr/bin/env python3
"""Release gate decisions and text pins; no subprocesses or network in tests."""
from __future__ import annotations

import contextlib
from collections import Counter
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from ci_preflight import _jobs, _steps
import ci_release_gate


ROOT = Path(__file__).resolve().parents[1]
REPO = "example/engine"
SHA = "a" * 40
HEAD = "b" * 40
TREE = "c" * 40
OTHER_HEAD = "d" * 40
OTHER_TREE = "e" * 40
WATCHER_FIRST_SHA = "f" * 40
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


def http_response(code, body, headers=None):
    fields = {"Content-Type": "application/json", **(headers or {})}
    return (f"HTTP/2.0 {code}\r\n"
            + "".join(f"{name}: {value}\r\n" for name, value in fields.items())
            + "\r\n" + body)


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
        self.git = mock.Mock(side_effect=AssertionError("unexpected git ancestry read"))
        first = mock.patch.object(ci_release_gate, "MAIN_PROOF_WATCHER_FIRST_SHA", None)
        first.start()
        self.addCleanup(first.stop)
        reader = mock.patch.object(ci_release_gate, "read_main_proof",
                                   side_effect=AssertionError("unexpected main-proof read"))
        self.proof = reader.start()
        self.addCleanup(reader.stop)

    def enable_watcher(self, receipt=None, *, first_sha=WATCHER_FIRST_SHA):
        first = mock.patch.object(ci_release_gate, "MAIN_PROOF_WATCHER_FIRST_SHA", first_sha)
        first.start()
        self.addCleanup(first.stop)
        self.git.side_effect = None
        self.git.return_value = True
        self.proof.side_effect = None
        self.proof.return_value = receipt

    def assert_no_pr_fallback(self):
        self.assertEqual(self.api.calls, [(MAIN_API, False, ".sha")],
                         "phase 2 must never read PR heads or their statuses")

    def run_gate(self, *, repo=REPO, sha=SHA, ref="refs/heads/main"):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = ci_release_gate.main(
                ["gate", "--repo", repo, "--sha", sha, "--ref", ref],
                api=self.api, git=self.git)
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

    def test_lite_verify_cannot_replace_missing_full_verify(self):
        self.api.statuses([
            {"context": "pineforge/verify-lite", "state": "success"}, successes()[1]])
        self.assert_refused(f"PR #17 head {HEAD}: missing pineforge/verify")

    def test_lite_verify_cannot_override_pending_full_verify(self):
        self.api.statuses([
            {"context": "pineforge/verify-lite", "state": "success"},
            {"context": "pineforge/verify", "state": "pending"}, successes()[1]])
        self.assert_refused(f"PR #17 head {HEAD}: pineforge/verify=pending (expected success)")

    def test_lite_parity_cannot_replace_full_parity(self):
        self.api.statuses([
            successes()[0], {"context": "pineforge/parity-lite", "state": "success"}])
        self.assert_refused(f"PR #17 head {HEAD}: missing pineforge/parity")

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
        self.assertFalse(any("/statuses" in endpoint for endpoint, _, _ in self.api.calls))

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

    def test_raised_rate_limit_is_named_and_preserves_the_stderr(self):
        self.api.responses[PULLS_API] = subprocess.CalledProcessError(
            1, ["gh", "api", PULLS_API], stderr="gh: API rate limit exceeded (HTTP 403)\n")
        self.assert_refused(
            f"API {PULLS_API} rate limited: gh: API rate limit exceeded (HTTP 403)")
        self.assertEqual(Counter(endpoint for endpoint, _, _ in self.api.calls)[PULLS_API], 1)

    def test_returned_rate_limit_is_named_and_not_retried(self):
        for payload in (
                http_response(403, '{"message":"API rate limit exceeded"}'),
                http_response(403, '{"message":"Forbidden"}',
                              {"X-RateLimit-Remaining": "0"}),
                http_response(200, json.dumps(successes()),
                              {"x-ratelimit-remaining": "0"})):
            with self.subTest(payload=payload):
                self.api = FakeAPI()
                self.api.responses[STATUSES_API] = payload
                self.assert_refused(f"API {STATUSES_API} rate limited")
                counts = Counter(endpoint for endpoint, _, _ in self.api.calls)
                self.assertEqual(counts[STATUSES_API], 1)

    def test_returned_5xx_is_named_and_not_retried(self):
        for code in (500, 502, 503, 599):
            with self.subTest(code=code):
                self.api = FakeAPI()
                self.api.responses[STATUSES_API] = http_response(
                    code, '{"message":"Service Unavailable"}')
                self.assert_refused(f"API {STATUSES_API} upstream error {code}")
                counts = Counter(endpoint for endpoint, _, _ in self.api.calls)
                self.assertEqual(counts[STATUSES_API], 1)

    def test_raised_503_is_named_and_not_retried(self):
        self.api.responses[STATUSES_API] = subprocess.CalledProcessError(
            1, ["gh", "api", STATUSES_API], stderr="gh: Service Unavailable (HTTP 503)\n")
        self.assert_refused(f"API {STATUSES_API} upstream error 503")
        self.assertEqual(Counter(endpoint for endpoint, _, _ in self.api.calls)[STATUSES_API], 1)

    def test_each_api_read_is_attempted_at_most_once(self):
        endpoints = [MAIN_API, PULLS_API, f"repos/{REPO}/commits/{SHA}",
                     f"repos/{REPO}/commits/{HEAD}", STATUSES_API]
        for failed in [None, *endpoints]:
            with self.subTest(failed=failed):
                self.api = FakeAPI()
                if failed is not None:
                    self.api.responses[failed] = http_response(503, "unavailable")
                    self.assert_refused(f"API {failed} upstream error 503")
                    self.assertEqual(self.api.calls[-1][0], failed,
                                     "a failed API read must stop the check immediately")
                else:
                    code, stdout, stderr = self.run_gate()
                    self.assertEqual(code, 0, (stdout, stderr))
                counts = Counter(endpoint for endpoint, _, _ in self.api.calls)
                self.assertTrue(counts)
                self.assertTrue(all(count == 1 for count in counts.values()), counts)

    def test_http_headers_are_stripped_for_main_and_all_json_pages(self):
        self.api.responses = {endpoint: http_response(200, raw)
                              for endpoint, raw in self.api.responses.items()}
        self.api.responses[STATUSES_API] = (
            http_response(200, json.dumps([successes()[0]])) + "\n"
            + http_response(200, json.dumps([successes()[1]])))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, (stdout, stderr))
        self.assertEqual(stderr, "")

    def test_paginated_headers_need_no_extra_separator_after_a_json_array(self):
        self.api.responses[STATUSES_API] = (
            http_response(200, json.dumps([successes()[0]]))
            + http_response(200, json.dumps([successes()[1]])))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, (stdout, stderr))

    def test_http_headers_cannot_hide_an_empty_page(self):
        for pages in (("", json.dumps(successes())), (json.dumps(successes()), "")):
            with self.subTest(pages=pages):
                self.api = FakeAPI()
                self.api.responses[STATUSES_API] = "\n".join(
                    http_response(200, body) for body in pages)
                self.assert_refused(f"API {STATUSES_API} returned an empty payload")

    def test_a_later_page_api_failure_refuses_without_retry(self):
        for response, message in (
                (http_response(403, '{"message":"rate limit exceeded"}'), "rate limited"),
                (http_response(503, "unavailable"), "upstream error 503")):
            with self.subTest(message=message):
                self.api = FakeAPI()
                self.api.responses[STATUSES_API] = (
                    http_response(200, json.dumps(successes())) + "\n" + response)
                self.assert_refused(f"API {STATUSES_API} {message}")
                counts = Counter(endpoint for endpoint, _, _ in self.api.calls)
                self.assertEqual(counts[STATUSES_API], 1)

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

    def test_multiple_tree_matching_prs_refuse_and_name_both_numbers(self):
        newer = pull(OTHER_HEAD, number=18, merged_at="2026-10-21T12:00:00Z")
        self.api.responses[PULLS_API] = json.dumps([newer, pull()])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = json.dumps(
            commit(OTHER_HEAD, TREE))
        newer_api = f"repos/{REPO}/commits/{OTHER_HEAD}/statuses?per_page=100"
        self.api.responses[newer_api] = json.dumps(successes())
        self.assert_refused(
            f"ambiguous release tree {TREE}: multiple merged PR heads match (#17, #18)")
        self.assertFalse(any("/statuses" in endpoint for endpoint, _, _ in self.api.calls))

    def test_shared_head_is_read_once_before_ambiguity_refusal(self):
        self.api.responses[PULLS_API] = (
            json.dumps([pull()]) + "\n" + json.dumps([pull(number=18)]))
        self.assert_refused("multiple merged PR heads match (#17, #18)")
        counts = Counter(endpoint for endpoint, _, _ in self.api.calls)
        self.assertTrue(all(count == 1 for count in counts.values()), counts)

    def test_newer_nonmatching_pr_does_not_hide_a_matching_pr(self):
        newer = pull(OTHER_HEAD, number=18, merged_at="2026-10-21T12:00:00Z")
        self.api.responses[PULLS_API] = json.dumps([newer, pull()])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = json.dumps(
            commit(OTHER_HEAD, OTHER_TREE))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"PR #17 head {HEAD}", stdout)
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_older_nonmatching_pr_is_still_checked_after_a_match(self):
        older = pull(OTHER_HEAD, number=16, merged_at="2026-10-19T12:00:00Z")
        self.api.responses[PULLS_API] = json.dumps([pull(), older])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = http_response(
            503, "unavailable")
        self.assert_refused(f"API repos/{REPO}/commits/{OTHER_HEAD} upstream error 503")
        self.assertFalse(any("/statuses" in endpoint for endpoint, _, _ in self.api.calls))

    def test_watcher_not_live_uses_only_the_transitional_pr_path(self):
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, (stdout, stderr))
        self.git.assert_not_called()
        self.proof.assert_not_called()

    def test_at_or_after_cut_over_missing_main_proof_refuses_without_pr_fallback(self):
        for first in (SHA, WATCHER_FIRST_SHA):
            with self.subTest(first=first):
                self.api = FakeAPI()  # A fully green PR fallback is available.
                self.git.reset_mock()
                self.proof.reset_mock()
                self.enable_watcher(first_sha=first)
                self.assert_refused(f"main-proof receipt is missing for {SHA}")
                self.git.assert_called_once_with(first, SHA)
                self.proof.assert_called_once_with(REPO, SHA)
                self.assert_no_pr_fallback()

    def test_main_proof_with_full_contexts_green_passes_without_pr_reads(self):
        self.enable_watcher({row["context"]: row["state"] for row in successes()})
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertEqual(stderr, "")
        self.assertEqual(stdout, (
            f"release gate green: main-proof receipt for {SHA}; "
            "pineforge/verify=success; pineforge/parity=success\n"))
        self.proof.assert_called_once_with(REPO, SHA)
        self.assert_no_pr_fallback()

    def test_main_proof_lite_context_cannot_replace_either_full_context(self):
        for missing in ("pineforge/verify", "pineforge/parity"):
            with self.subTest(missing=missing):
                self.api = FakeAPI()
                receipt = {row["context"]: row["state"] for row in successes()
                           if row["context"] != missing}
                receipt[missing + "-lite"] = "success"
                self.enable_watcher(receipt)
                self.assert_refused(f"main-proof receipt for {SHA}: missing {missing}")
                self.assert_no_pr_fallback()

    def test_main_proof_pending_full_context_is_not_rescued_by_lite_success(self):
        self.enable_watcher({"pineforge/verify": "pending", "pineforge/verify-lite": "success",
                             "pineforge/parity": "success"})
        self.assert_refused(
            f"main-proof receipt for {SHA}: pineforge/verify=pending (expected success)")
        self.assert_no_pr_fallback()

    def test_unreadable_main_proof_never_falls_back(self):
        self.enable_watcher()
        self.proof.side_effect = NotImplementedError(
            "phase 2 receipt reader lands with C2 main-watch #176")
        self.assert_refused(f"main-proof receipt for {SHA} unreadable: "
                            "phase 2 receipt reader lands with C2 main-watch #176")
        self.assert_no_pr_fallback()

    def test_empty_or_malformed_main_proof_refuses_without_fallback(self):
        for receipt in ({}, [], "", "not JSON", {"pineforge/verify": None}):
            with self.subTest(receipt=receipt):
                self.api = FakeAPI()
                self.enable_watcher(receipt)
                self.assert_refused(f"main-proof receipt for {SHA}: invalid or empty context map")
                self.assert_no_pr_fallback()

    def test_pre_watcher_commit_uses_the_transitional_pr_path(self):
        self.enable_watcher()
        self.git.side_effect = [False, True]
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, (stdout, stderr))
        self.assertIn(f"PR #17 head {HEAD}", stdout)
        self.assertEqual(self.git.call_args_list,
                         [mock.call(WATCHER_FIRST_SHA, SHA), mock.call(SHA, WATCHER_FIRST_SHA)])
        self.proof.assert_not_called()
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_unrelated_commit_cannot_use_the_transitional_pr_path(self):
        self.enable_watcher()
        self.git.return_value = False
        self.assert_refused(f"release sha {SHA} is unrelated to main-proof cut-over")
        self.proof.assert_not_called()
        self.assert_no_pr_fallback()

    def test_unknown_cut_over_ancestry_refuses_without_fallback(self):
        for result in (OSError("git unavailable"), None, 0, "true"):
            with self.subTest(result=result):
                self.api = FakeAPI()
                self.enable_watcher()
                if isinstance(result, Exception):
                    self.git.side_effect = result
                else:
                    self.git.return_value = result
                self.assert_refused("cannot determine main-proof cut-over ancestry")
                self.proof.assert_not_called()
                self.assert_no_pr_fallback()

    def test_invalid_cut_over_constant_refuses(self):
        self.enable_watcher(first_sha="not a sha")
        self.assert_refused("invalid MAIN_PROOF_WATCHER_FIRST_SHA cut-over")
        self.git.assert_not_called()
        self.proof.assert_not_called()
        self.assert_no_pr_fallback()

    def test_unexpected_api_shapes_refuse(self):
        for endpoint, payload, message in (
                (PULLS_API, "{}", "an empty payload"),
                (PULLS_API, '{"message": "Not Found"}',
                 "a non-array JSON page (expected a JSON array)"),
                (PULLS_API, '[{"number": 17}]', "invalid pull request"),
                (PULLS_API, json.dumps([pull(merged_at="not a date")]), "invalid merged_at"),
                (PULLS_API, json.dumps([pull(), pull()]), "duplicate pull request #17"),
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
        with mock.patch.object(ci_release_gate.subprocess, "run") as run:
            run.return_value.stdout = "[]\n"
            self.assertEqual(ci_release_gate.gh_api(STATUSES_API, paginate=True), "[]\n")
        run.assert_called_once_with(["gh", "api", STATUSES_API, "--include", "--paginate"],
                                    check=True, capture_output=True, text=True)

    def test_gh_adapter_reads_live_main_sha_with_jq(self):
        with mock.patch.object(ci_release_gate.subprocess, "run") as run:
            run.return_value.stdout = SHA + "\n"
            self.assertEqual(ci_release_gate.gh_api(MAIN_API, jq=".sha"), SHA + "\n")
        run.assert_called_once_with(["gh", "api", MAIN_API, "--include", "--jq", ".sha"],
                                    check=True, capture_output=True, text=True)

    def test_gh_adapter_does_not_retry_a_failed_read(self):
        failure = subprocess.CalledProcessError(
            1, ["gh", "api", STATUSES_API], stderr="gh: Service Unavailable (HTTP 503)")
        with mock.patch.object(ci_release_gate.subprocess, "run", side_effect=failure) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                ci_release_gate.gh_api(STATUSES_API, paginate=True)
        run.assert_called_once_with(["gh", "api", STATUSES_API, "--include", "--paginate"],
                                    check=True, capture_output=True, text=True)


class MainProofSeamTests(unittest.TestCase):
    def test_reader_is_not_live_and_defines_no_storage_format(self):
        with self.assertRaisesRegex(NotImplementedError,
                                    "^phase 2 receipt reader lands with C2 main-watch #176$"):
            ci_release_gate.read_main_proof(REPO, SHA)

    def test_git_adapter_uses_merge_base_ancestor_exit_semantics(self):
        for code, expected in ((0, True), (1, False)):
            with self.subTest(code=code):
                with mock.patch.object(ci_release_gate.subprocess, "run") as run:
                    run.return_value.returncode = code
                    self.assertIs(ci_release_gate.git_is_ancestor(WATCHER_FIRST_SHA, SHA), expected)
                run.assert_called_once_with(
                    ["git", "merge-base", "--is-ancestor", WATCHER_FIRST_SHA, SHA],
                    check=False, capture_output=True, text=True)

    def test_git_adapter_does_not_treat_an_error_as_a_pre_watcher_commit(self):
        with mock.patch.object(ci_release_gate.subprocess, "run") as run:
            run.return_value.returncode = 128
            run.return_value.stderr = "fatal: invalid commit"
            with self.assertRaisesRegex(ci_release_gate.ReleaseGateError,
                                        "git merge-base --is-ancestor failed: fatal: invalid commit"):
                ci_release_gate.git_is_ancestor(WATCHER_FIRST_SHA, SHA)
        run.assert_called_once()


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
        self.assertIn("python3 scripts/ci_release_gate.py gate \\\n", gate)
        self.assertIn("          GH_TOKEN: ${{ steps.app.outputs.token }}\n", gate,
                      "release gate must use the release job's own App token")
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

    def test_ci_release_gate_is_unconditional_and_precedes_every_release_write(self):
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

    def test_pin_fails_when_gate_uses_a_second_token(self):
        workflow = self.workflow()
        gate = self.gate_block(workflow)
        changed = workflow.replace(gate, gate.replace(
            "GH_TOKEN: ${{ steps.app.outputs.token }}", "GH_TOKEN: ${{ github.token }}", 1), 1)
        self.assert_mutation_refused(
            workflow, changed, "release gate must use the release job's own App token")


if __name__ == "__main__":
    unittest.main()
