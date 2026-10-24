#!/usr/bin/env python3
"""Release gate decisions and text pins; no subprocesses or network in tests."""
from __future__ import annotations

import ast
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
MAIN_API = f"repos/{REPO}/branches/main"
PULLS_API = f"repos/{REPO}/commits/{SHA}/pulls"
STATUSES_API = f"repos/{REPO}/commits/{HEAD}/statuses?per_page=100"
BUMP_PARENT = OTHER_HEAD
BUMP_API = f"repos/{REPO}/commits/{SHA}"
PARENT_PULLS_API = f"repos/{REPO}/commits/{BUMP_PARENT}/pulls"
RUNS_API = f"repos/{REPO}/actions/runs?head_sha={SHA}&per_page=100"
REPO_API = f"repos/{REPO}"
RULESETS_API = f"repos/{REPO}/rulesets?includes_parents=true&per_page=100"
DEFAULT_RULESET_API = f"repos/{REPO}/rulesets/101"
STRICT_RULESET_API = f"repos/{REPO}/rulesets/202"
BEFORE_TREE_API = f"repos/{REPO}/git/trees/{TREE}?recursive=1"
AFTER_TREE_API = f"repos/{REPO}/git/trees/{OTHER_TREE}?recursive=1"
GATE_NAME = "Refuse unless pineforge/verify and pineforge/parity are green"
RULESET_BARRIER_REFUSAL = (
    "main requires an active applicable pull_request ruleset with bypass_actors "
    "exactly Integration 4105147 with bypass_mode always")
SINGLE_DOCUMENT_REFUSAL = (
    "returned an HTTP response body with trailing data; "
    "expected exactly one JSON document")


def _without_comments(text):
    return "\n".join(line for line in text.splitlines()
                     if not line.strip().startswith("#"))


def commit(sha, tree):
    return {"sha": sha, "commit": {"tree": {"sha": tree}}}


def pull(head=HEAD, number=17, merged_at="2026-10-20T12:00:00Z", *,
         base_ref="main", merge_sha=SHA):
    return {"number": number, "merged_at": merged_at, "head": {"sha": head},
            "base": {"ref": base_ref}, "merge_commit_sha": merge_sha}


def status(context, state="success"):
    return {"context": context, "state": state,
            "creator": {"login": "luisleo526", "type": "User"}}


def successes():
    return [status("pineforge/verify"), status("pineforge/parity")]


def green_statuses():
    return successes() + [
        status("pineforge/verify", "failure"),
        status("pineforge/parity", "pending"),
    ]


def http_response(code, body, headers=None):
    fields = {"Content-Type": "application/json", **(headers or {})}
    return (f"HTTP/2.0 {code}\r\n"
            + "".join(f"{name}: {value}\r\n" for name, value in fields.items())
            + "\r\n" + body)


def push_run(*, actor="pineforge-app[bot]", triggering_actor="pineforge-app[bot]",
             event="push", branch="main", sha=SHA):
    return {"event": event, "head_branch": branch, "head_sha": sha,
            "actor": {"login": actor}, "triggering_actor": {"login": triggering_actor}}


def runs_page(rows, *, total=None, headers=None):
    return http_response(200, json.dumps({
        "total_count": len(rows) if total is None else total,
        "workflow_runs": rows}), headers)


def bypass(actor_id=4105147, actor_type="Integration", mode="always"):
    return {"actor_id": actor_id, "actor_type": actor_type, "bypass_mode": mode}


def ruleset(number=202, *, target="branch", enforcement="active", include=None,
            exclude=None, rules=("pull_request", "required_status_checks"), actors=None):
    rule_rows = [{"type": rule} for rule in rules]
    for row in rule_rows:
        if row["type"] == "required_status_checks":
            row["parameters"] = {
                "required_status_checks": [{"context": "lint"}],
                "strict_required_status_checks_policy": True,
            }
    return {
        "id": number, "name": f"fixture ruleset {number}", "target": target,
        "enforcement": enforcement,
        "conditions": {"ref_name": {
            "include": ["~DEFAULT_BRANCH"] if include is None else include,
            "exclude": [] if exclude is None else exclude}},
        "rules": rule_rows,
        "bypass_actors": [bypass()] if actors is None else actors,
    }


def release_rulesets():
    return [
        ruleset(101, rules=("deletion", "required_linear_history", "pull_request"),
                actors=[bypass(), bypass(None, "OrganizationAdmin"),
                        bypass(None, "DeployKey"), bypass(5, "RepositoryRole"),
                        bypass(5219872)]),
        ruleset(),
        # A tag-creation ruleset is never evidence about main pushes.
        ruleset(303, target="tag", include=["refs/tags/v*"], rules=("creation",)),
    ]


def ruleset_summaries(rows):
    return [{key: row[key] for key in ("id", "name", "target", "enforcement")}
            for row in rows]


def tree_entry(path, sha, *, mode="100644", kind="blob"):
    return {"path": path, "sha": sha, "mode": mode, "type": kind}


def bump_files(*, after=False):
    return [
        tree_entry("VERSION", ("4" if after else "1") * 40),
        tree_entry("docker", ("6" if after else "7") * 40, mode="040000", kind="tree"),
        tree_entry("docker/run_failure_codes_diff.json", ("5" if after else "2") * 40),
        tree_entry("src", "8" * 40, mode="040000", kind="tree"),
        tree_entry("src/unchanged.cpp", "3" * 40),
    ]


def git_tree(sha, entries, *, truncated=False):
    return json.dumps({"sha": sha, "truncated": truncated, "tree": entries})


class FakeAPI:
    """Raw gh responses keyed by exact endpoint, including jq/pagination flags."""

    def __init__(self):
        self.responses = {
            MAIN_API: SHA + "\n",
            f"repos/{REPO}/commits/{SHA}": json.dumps(commit(SHA, TREE)),
            PULLS_API: json.dumps([pull()]),
            f"repos/{REPO}/commits/{HEAD}": json.dumps(commit(HEAD, TREE)),
            STATUSES_API: json.dumps(successes()),
            RUNS_API: runs_page([]),
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

    def rulesets(self, rows):
        self.responses[RULESETS_API] = http_response(200, json.dumps(ruleset_summaries(rows)))
        self.responses.update({f"repos/{REPO}/rulesets/{row['id']}": json.dumps(row)
                               for row in rows})

    def release_bump(self):
        candidate = commit(SHA, OTHER_TREE)
        candidate["parents"] = [{"sha": BUMP_PARENT}]
        candidate["commit"]["verification"] = {"verified": False}
        self.responses.update({
            REPO_API: json.dumps({"default_branch": "main"}),
            PULLS_API: "[]",
            BUMP_API: json.dumps(candidate),
            RUNS_API: runs_page([push_run()]),
            PARENT_PULLS_API: json.dumps([pull(merge_sha=BUMP_PARENT)]),
            f"repos/{REPO}/commits/{BUMP_PARENT}": json.dumps(commit(BUMP_PARENT, TREE)),
            BEFORE_TREE_API: git_tree(TREE, bump_files()),
            AFTER_TREE_API: git_tree(OTHER_TREE, bump_files(after=True)),
        })
        self.rulesets(release_rulesets())


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
        self.assertEqual(self.api.calls, [(MAIN_API, False, ".commit.sha")],
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
        self.assertIn((MAIN_API, False, ".commit.sha"), self.api.calls)
        self.assertIn((PULLS_API, True, None), self.api.calls)
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_success_from_an_unknown_creator_or_wrong_type_refuses(self):
        for index, context in enumerate(("pineforge/verify", "pineforge/parity")):
            for login, creator_type in (("untrusted-operator", "User"), ("luisleo526", "Bot")):
                with self.subTest(context=context, login=login, creator_type=creator_type):
                    self.api = FakeAPI()
                    rows = successes()
                    rows[index]["creator"] = {"login": login, "type": creator_type}
                    self.api.statuses(rows)
                    self.assert_refused(
                        f"{context}: refused status creator (login={login!r}, type={creator_type!r})")

    def test_newest_untrusted_creator_cannot_be_rescued_by_an_older_allowed_success(self):
        for context in ("pineforge/verify", "pineforge/parity"):
            with self.subTest(context=context):
                self.api = FakeAPI()
                newest = status(context)
                newest["creator"] = {"login": "untrusted-operator", "type": "User"}
                self.api.responses[STATUSES_API] = (
                    json.dumps([newest]) + "\n" + json.dumps(successes()))
                self.assert_refused(
                    f"{context}: refused status creator (login='untrusted-operator', type='User')")

    def test_status_without_creator_refuses(self):
        for index, context in enumerate(("pineforge/verify", "pineforge/parity")):
            with self.subTest(context=context):
                self.api = FakeAPI()
                rows = successes()
                del rows[index]["creator"]
                self.api.statuses(rows)
                self.assert_refused(f"{context}: missing or malformed status creator: None")

    def test_malformed_creator_object_refuses(self):
        for creator in (None, "luisleo526", ["luisleo526", "User"]):
            with self.subTest(creator=creator):
                self.api = FakeAPI()
                rows = successes()
                rows[0]["creator"] = creator
                self.api.statuses(rows)
                self.assert_refused(
                    f"pineforge/verify: missing or malformed status creator: {creator!r}")

    def test_missing_or_malformed_creator_fields_refuse_without_inventing_an_identity(self):
        for creator in ({}, {"login": "luisleo526"}, {"type": "User"},
                        {"login": None, "type": "User"}, {"login": "luisleo526", "type": None},
                        {"login": 17, "type": "User"}, {"login": "luisleo526", "type": ["User"]}):
            with self.subTest(creator=creator):
                self.api = FakeAPI()
                rows = successes()
                rows[0]["creator"] = creator
                self.api.statuses(rows)
                self.assert_refused(
                    "pineforge/verify: refused status creator "
                    f"(login={creator.get('login')!r}, type={creator.get('type')!r})")

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
            status("pineforge/verify-lite"), successes()[1]])
        self.assert_refused(f"PR #17 head {HEAD}: missing pineforge/verify")

    def test_lite_verify_cannot_override_pending_full_verify(self):
        self.api.statuses([
            status("pineforge/verify-lite"),
            status("pineforge/verify", "pending"), successes()[1]])
        self.assert_refused(f"PR #17 head {HEAD}: pineforge/verify=pending (expected success)")

    def test_lite_parity_cannot_replace_full_parity(self):
        self.api.statuses([
            successes()[0], status("pineforge/parity-lite")])
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
        self.assert_refused(f"PR #17 head {HEAD}: missing pineforge/verify")
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
        self.assert_refused(f"no merged pull request associated with {SHA}")

    def test_main_merged_into_another_base_is_ignored(self):
        backport = pull(OTHER_HEAD, number=18, base_ref="release")
        self.api.responses[PULLS_API] = json.dumps([backport, pull()])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = json.dumps(
            commit(OTHER_HEAD, TREE))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"PR #17 head {HEAD}", stdout)
        self.assertNotIn((f"repos/{REPO}/commits/{OTHER_HEAD}", False, None), self.api.calls)

    def test_a_different_merge_commit_sha_is_ignored(self):
        other_merge = pull(OTHER_HEAD, number=18, merge_sha=OTHER_HEAD)
        self.api.responses[PULLS_API] = json.dumps([other_merge, pull()])
        self.api.responses[f"repos/{REPO}/commits/{OTHER_HEAD}"] = json.dumps(
            commit(OTHER_HEAD, TREE))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"PR #17 head {HEAD}", stdout)
        self.assertNotIn((f"repos/{REPO}/commits/{OTHER_HEAD}", False, None), self.api.calls)

    def test_only_unrelated_merged_pull_requests_refuse(self):
        self.api.responses[PULLS_API] = json.dumps([
            pull(base_ref="release"), pull(number=18, merge_sha=OTHER_HEAD)])
        self.assert_refused(f"no merged pull request associated with {SHA}")

    def test_missing_pull_request_base_or_merge_sha_refuses(self):
        for field, message in (("base", "invalid pull request base.ref"),
                               ("merge_commit_sha", "invalid merge_commit_sha")):
            with self.subTest(field=field):
                self.api = FakeAPI()
                row = pull()
                del row[field]
                self.api.responses[PULLS_API] = json.dumps([row])
                self.assert_refused(f"API {PULLS_API} returned {message}: None")

    def test_malformed_pull_request_base_ref_refuses(self):
        for base in (None, "main", [], {}, {"ref": None}, {"ref": 17}, {"ref": ""}):
            with self.subTest(base=base):
                self.api = FakeAPI()
                row = pull()
                row["base"] = base
                self.api.responses[PULLS_API] = json.dumps([row])
                self.assert_refused(f"API {PULLS_API} returned invalid pull request base.ref: {base!r}")

    def test_malformed_pull_request_merge_commit_sha_refuses(self):
        for merge_sha in (None, 17, [], {}, "", "a" * 7, "z" * 40, "A" * 40):
            with self.subTest(merge_sha=merge_sha):
                self.api = FakeAPI()
                self.api.responses[PULLS_API] = json.dumps([pull(merge_sha=merge_sha)])
                self.assert_refused(f"API {PULLS_API} returned invalid merge_commit_sha: {merge_sha!r}")

    def test_non_main_ref_refuses_before_any_api_call(self):
        self.assert_refused("release requires refs/heads/main, got refs/tags/v2.0.0",
                            ref="refs/tags/v2.0.0")
        self.assertEqual(self.api.calls, [])

    def test_main_backport_ref_refuses_before_any_api_call(self):
        self.assert_refused("release requires refs/heads/main, got refs/heads/main-backport",
                            ref="refs/heads/main-backport")
        self.assertEqual(self.api.calls, [])

    def test_main_ref_with_a_trailing_newline_refuses_before_any_api_call(self):
        self.assert_refused("release requires refs/heads/main, got refs/heads/main\n",
                            ref="refs/heads/main\n")
        self.assertEqual(self.api.calls, [])

    def test_sha_must_equal_the_live_main_head(self):
        self.api.responses[MAIN_API] = OTHER_HEAD + "\n"
        self.assert_refused(f"release sha {SHA} is not current main head {OTHER_HEAD}")
        self.assertEqual(self.api.calls, [(MAIN_API, False, ".commit.sha")])

    def test_a_shared_seven_hex_prefix_is_not_the_main_head(self):
        current = SHA[:7] + OTHER_HEAD[7:]
        self.api.responses[MAIN_API] = current + "\n"
        self.assert_refused(f"release sha {SHA} is not current main head {current}")
        self.assertEqual(self.api.calls, [(MAIN_API, False, ".commit.sha")])

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

    def test_paginated_pr_and_status_bodies_require_one_json_document(self):
        # RED on 72133e77e3: the headerless [] becomes another decoded page
        # without changing the merged PR or its successful statuses.
        for endpoint in (PULLS_API, STATUSES_API):
            with self.subTest(endpoint=endpoint):
                self.api = FakeAPI()
                self.api.responses[endpoint] = (
                    http_response(200, self.api.responses[endpoint]) + "[]")
                self.assert_refused(f"API {endpoint} {SINGLE_DOCUMENT_REFUSAL}")
                self.assertEqual(self.api.calls[-1], (endpoint, True, None))

    def test_http_headers_cannot_hide_an_empty_page(self):
        for pages in (("", json.dumps(successes())), (json.dumps(successes()), "")):
            with self.subTest(pages=pages):
                self.api = FakeAPI()
                self.api.responses[STATUSES_API] = "\n".join(
                    http_response(200, body) for body in pages)
                self.assert_refused(f"API {STATUSES_API} returned an empty payload")

    def test_empty_json_array_pages_can_surround_valid_list_evidence(self):
        for endpoint in (PULLS_API, STATUSES_API):
            for headers in (False, True):
                with self.subTest(endpoint=endpoint, headers=headers):
                    self.api = FakeAPI()
                    pages = ["[]", self.api.responses[endpoint], "[]"]
                    self.api.responses[endpoint] = "\n".join(
                        http_response(200, page) if headers else page for page in pages)
                    code, stdout, stderr = self.run_gate()
                    self.assertEqual(code, 0, (stdout, stderr))
                    self.assertEqual(stderr, "")

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
            status("pineforge/verify", "failure"),
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
                successes()[1], status("pineforge/verify", "failure")]))
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn("pineforge/verify=success; pineforge/parity=success", stdout)

    def test_failure_on_the_first_page_cannot_be_masked_by_later_success(self):
        self.api.responses[STATUSES_API] = (
            json.dumps([status("pineforge/verify", "failure")])
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
                (f"repos/{REPO}/commits/{HEAD}", "{}", "an empty payload"),
                (STATUSES_API, "null", "an empty payload"),
                (STATUSES_API, "[null]", "invalid status row"),
                (STATUSES_API, json.dumps([{key: value for key, value in successes()[0].items()
                                           if key != "state"}]), "invalid status state")):
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
            f"repos/{other_repo}/branches/main": other_sha,
            f"repos/{other_repo}/commits/{other_sha}": json.dumps(commit(other_sha, other_tree)),
            f"repos/{other_repo}/commits/{other_sha}/pulls": json.dumps([
                pull(other_head, number=902, merge_sha=other_sha)]),
            f"repos/{other_repo}/commits/{other_head}": json.dumps(commit(other_head, other_tree)),
            f"repos/{other_repo}/commits/{other_head}/statuses?per_page=100": json.dumps(successes()),
        }
        code, stdout, stderr = self.run_gate(repo=other_repo, sha=other_sha)
        self.assertEqual(code, 0, stderr)
        self.assertIn(f"PR #902 head {other_head}", stdout)
        self.assertIn(f"tree {other_tree}", stdout)


class ReleaseBumpTests(unittest.TestCase):
    # Reuse the real CLI harness without inheriting/rerunning GateTests' cases.
    run_gate = GateTests.run_gate
    assert_refused = GateTests.assert_refused
    enable_watcher = GateTests.enable_watcher

    def setUp(self):
        GateTests.setUp(self)
        self.api.release_bump()

    def reset_bump(self):
        self.api = FakeAPI()
        self.api.release_bump()

    def assert_bump_green(self):
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, (stdout, stderr))
        self.assertEqual(stderr, "")
        self.assertEqual(stdout, (
            f"release gate green: release bump {SHA}; parent {BUMP_PARENT}; "
            f"PR #17 head {HEAD}; pineforge/verify=success; "
            f"pineforge/parity=success; tree {TREE}\n"))

    def test_bump_on_a_gated_parent_passes(self):
        # RED on main: the exact empty associated-pulls input refuses there.
        self.assert_bump_green()
        self.assertIn((RUNS_API, True, None), self.api.calls)
        self.assertIn((PARENT_PULLS_API, True, None), self.api.calls)
        self.assertIn((STATUSES_API, True, None), self.api.calls)
        self.assertIn((BEFORE_TREE_API, False, None), self.api.calls)
        self.assertIn((AFTER_TREE_API, False, None), self.api.calls)
        self.assertTrue(all(count == 1 for count in
                            Counter(endpoint for endpoint, _, _ in self.api.calls).values()))
        self.git.assert_not_called()
        self.proof.assert_not_called()

    def test_bump_plus_one_code_file_refuses(self):
        # Condition (c) mutation control: all identity/parent evidence is green.
        entries = bump_files(after=True) + [tree_entry("src/extra.cpp", "9" * 40)]
        self.api.responses[AFTER_TREE_API] = git_tree(OTHER_TREE, entries)
        self.assert_refused("release bump must modify exactly VERSION and "
                            "docker/run_failure_codes_diff.json")

    def test_human_push_or_rerun_refuses_even_with_forged_commit_identity(self):
        candidate = json.loads(self.api.responses[BUMP_API])
        candidate.update(author={"login": "github-actions[bot]"},
                         committer={"login": "github-actions[bot]"})
        candidate["commit"].update(
            author={"name": "github-actions[bot]"},
            committer={"name": "github-actions[bot]"},
            verification={"verified": True})
        for actor, trigger in (("luisleo526", "luisleo526"),
                               ("luisleo526", "pineforge-app[bot]"),
                               ("pineforge-app[bot]", "luisleo526")):
            with self.subTest(actor=actor, triggering_actor=trigger):
                self.reset_bump()
                self.api.responses[BUMP_API] = json.dumps(candidate)
                self.api.responses[RUNS_API] = runs_page([
                    push_run(actor=actor, triggering_actor=trigger)])
                self.assert_refused("release bump push actor must be pineforge-app[bot] "
                                    "for both actor and triggering_actor")

    def test_zero_runs_refuses(self):
        self.api.responses[RUNS_API] = runs_page([])
        self.assert_refused(f"no exact-main push workflow run for release bump {SHA}")

    def test_unrelated_runs_cannot_supply_push_identity(self):
        for row in (push_run(event="workflow_dispatch"), push_run(branch="release"),
                    push_run(sha=BUMP_PARENT)):
            with self.subTest(row=row):
                self.reset_bump()
                self.api.responses[RUNS_API] = runs_page([row])
                self.assert_refused(f"no exact-main push workflow run for release bump {SHA}")

    def test_every_matching_push_run_must_have_both_app_actors(self):
        for rows in ([push_run(), push_run(actor="luisleo526")],
                     [push_run(triggering_actor="luisleo526"), push_run()]):
            with self.subTest(rows=rows):
                self.reset_bump()
                self.api.responses[RUNS_API] = runs_page(rows)
                self.assert_refused("release bump push actor must be pineforge-app[bot]")

    def test_runs_on_later_pages_are_all_read_with_adjacent_object_headers(self):
        for separator in ("", "\n"):
            with self.subTest(separator=separator):
                self.reset_bump()
                pages = [runs_page([push_run(event="workflow_dispatch") for _ in range(100)],
                                   total=101, headers={
                                       "Link": f'<https://api.github.com/{RUNS_API}&page=2>; '
                                       'rel="next"'}),
                         runs_page([push_run()], total=101, headers={
                             "Link": f'<https://api.github.com/{RUNS_API}&page=1>; rel="prev"'})]
                self.api.responses[RUNS_API] = separator.join(pages)
                self.assert_bump_green()
                self.assertIn((RUNS_API, True, None), self.api.calls)

    def test_human_push_on_a_later_page_refuses(self):
        self.api.responses[RUNS_API] = (
            runs_page([push_run() for _ in range(100)], total=101, headers={
                "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="next"'})
            + runs_page([push_run(actor="luisleo526")], total=101))
        self.assert_refused("release bump push actor must be pineforge-app[bot]")

    def test_later_runs_page_errors_cannot_hide_behind_a_good_page(self):
        for trailing, fragment in (
                (http_response(503, "unavailable"), "upstream error 503"),
                (http_response(200, ""), "returned an empty payload"),
                (http_response(200, "not JSON"), "returned invalid JSON")):
            with self.subTest(fragment=fragment):
                self.reset_bump()
                self.api.responses[RUNS_API] = (
                    runs_page([push_run()], total=2, headers={
                        "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="next"'})
                    + trailing)
                self.assert_refused(f"API {RUNS_API} {fragment}")
                self.assertEqual(self.api.calls[-1], (RUNS_API, True, None))

    def test_truncated_run_list_or_inconsistent_totals_refuse(self):
        for payload in (runs_page([push_run()], total=1001),
                        "".join(runs_page([push_run()] * 100, total=1000, headers={
                            "Link": f'<https://api.github.com/{RUNS_API}&page={index + 2}>; '
                            'rel="next"'} if index < 9 else None) for index in range(10)),
                        runs_page([push_run()], total=2, headers={
                            "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="next"'})
                        + runs_page([push_run()], total=3)):
            with self.subTest(payload=payload):
                self.reset_bump()
                self.api.responses[RUNS_API] = payload
                self.assert_refused(f"API {RUNS_API} returned incomplete workflow_runs pagination")

    def test_advertised_unread_runs_page_refuses_even_when_total_count_matches(self):
        # RED on a8b12768f1: one valid run equals total_count, so main ignores
        # the advertised second page and prints the exact green bump verdict.
        self.api.responses[RUNS_API] = runs_page([push_run()], headers={
            "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="next"'})
        self.assert_refused(
            f"API {RUNS_API} returned incomplete workflow_runs pagination: "
            "unread or unlinked next page")
        self.assertEqual(self.api.calls[-1], (RUNS_API, True, None))

    def test_missing_or_inconsistent_runs_page_links_refuse(self):
        body = json.dumps({"total_count": 1, "workflow_runs": [push_run()]})
        for raw in (body,
                    runs_page([push_run()], headers={
                        "Link": f'<https://api.github.com/{RUNS_API}&page=3>; rel="next"'})
                    + runs_page([], total=1),
                    runs_page([push_run()], total=2) + runs_page([push_run()], total=2),
                    runs_page([push_run()], headers={"Link": "unreadable"}),
                    runs_page([push_run()], headers={
                        "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="last"'})):
            with self.subTest(raw=raw):
                self.reset_bump()
                self.api.responses[RUNS_API] = raw
                self.assert_refused(
                    f"API {RUNS_API} returned incomplete workflow_runs pagination")
                self.assertEqual(self.api.calls[-1], (RUNS_API, True, None))

    def test_runs_body_cannot_hide_an_extra_json_document_without_headers(self):
        # Exact sol report input: one HTTP page, two JSON documents, one run.
        # RED on 72133e77e3: both totals are 1, so the bump prints green.
        self.api.responses[RUNS_API] = (
            runs_page([push_run()]) + '{"total_count":1,"workflow_runs":[]}')
        self.assert_refused(f"API {RUNS_API} {SINGLE_DOCUMENT_REFUSAL}")
        self.assertEqual(self.api.calls[-1], (RUNS_API, True, None))

    def test_ruleset_body_cannot_hide_an_extra_json_document_without_headers(self):
        # RED on 72133e77e3: the second document adds no inventory rows,
        # leaving the valid App-only barrier and exact green bump verdict.
        self.api.responses[RULESETS_API] += "[]"
        self.assert_refused(f"API {RULESETS_API} {SINGLE_DOCUMENT_REFUSAL}")
        self.assertEqual(self.api.calls[-1], (RULESETS_API, True, None))

    def test_second_http_page_cannot_contain_an_extra_json_document(self):
        summaries = json.dumps(ruleset_summaries(release_rulesets()))
        for endpoint, first, second, extra in (
                (RUNS_API,
                 json.dumps({"total_count": 2, "workflow_runs": [push_run()]}),
                 json.dumps({"total_count": 2, "workflow_runs": [push_run()]}),
                 '{"total_count":2,"workflow_runs":[]}'),
                (RULESETS_API, summaries, "[]", "[]")):
            for separator in ("", "\n"):
                with self.subTest(endpoint=endpoint, separator=separator):
                    self.reset_bump()
                    self.api.responses[endpoint] = (
                        http_response(200, first, {
                            "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
                        + separator + http_response(200, second, {
                            "Link": f'<https://api.github.com/{endpoint}&page=1>; rel="prev"'})
                        + extra)
                    # RED on 72133e77e3: two linked HTTP responses pass,
                    # then three documents decode with unchanged row counts.
                    self.assert_refused(f"API {endpoint} {SINGLE_DOCUMENT_REFUSAL}")
                    self.assertEqual(self.api.calls[-1], (endpoint, True, None))

    def test_trailing_garbage_is_named_as_an_invalid_http_body(self):
        for endpoint in (RUNS_API, RULESETS_API):
            with self.subTest(endpoint=endpoint):
                self.reset_bump()
                self.api.responses[endpoint] += "\ntrailing garbage"
                # RED on 72133e77e3: it instead reports a generic parse
                # error while decoding a nonexistent additional page.
                self.assert_refused(f"API {endpoint} {SINGLE_DOCUMENT_REFUSAL}")
                self.assertEqual(self.api.calls[-1], (endpoint, True, None))

    def test_partially_headerless_pages_refuse_before_link_validation(self):
        summaries = json.dumps(ruleset_summaries(release_rulesets()))
        for endpoint, first, second in (
                (RUNS_API,
                 json.dumps({"total_count": 1, "workflow_runs": [push_run()]}),
                 '{"total_count":1,"workflow_runs":[]}'),
                (RULESETS_API, summaries, "[]")):
            with self.subTest(endpoint=endpoint):
                self.reset_bump()
                self.api.responses[endpoint] = (
                    http_response(200, first, {
                        "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
                    + "\n" + second)
                # The second response alone lacks headers. RED on
                # 72133e77e3: Link validation reports an unread next page
                # before noticing that the first body has two documents.
                self.assert_refused(f"API {endpoint} {SINGLE_DOCUMENT_REFUSAL}")
                self.assertEqual(self.api.calls[-1], (endpoint, True, None))

    def test_empty_http_body_refuses_before_link_validation(self):
        for endpoint in (RUNS_API, RULESETS_API):
            with self.subTest(endpoint=endpoint):
                self.reset_bump()
                self.api.responses[endpoint] = http_response(200, " \r\n ", {
                    "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
                # RED on 72133e77e3: the unread Link diagnostic masks the
                # empty body; the body check must run before pagination.
                self.assert_refused(f"API {endpoint} returned an empty payload")
                self.assertEqual(self.api.calls[-1], (endpoint, True, None))

    def test_complete_runs_pages_may_end_with_an_empty_page(self):
        self.api.responses[RUNS_API] = (
            runs_page([push_run()], headers={
                "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="next"'})
            + runs_page([], total=1))
        self.assert_bump_green()

    def test_malformed_runs_or_actors_refuse(self):
        for payload, fragment in (
                (http_response(200, "[]"), "invalid workflow_runs page"),
                (http_response(200, '{"total_count":true,"workflow_runs":[]}'),
                 "invalid workflow_runs page"),
                (http_response(200, '{"total_count":1,"workflow_runs":{}}'),
                 "invalid workflow_runs page"),
                (runs_page([None]), "invalid workflow run"),
                (runs_page([{"event": "push"}]), "invalid workflow run"),
                (runs_page([{**push_run(), "actor": None}]), "push actor must be"),
                (runs_page([{**push_run(), "triggering_actor": {}}]), "push actor must be")):
            with self.subTest(payload=payload):
                self.reset_bump()
                self.api.responses[RUNS_API] = payload
                self.assert_refused(fragment)

    def test_bump_on_an_ungated_parent_refuses(self):
        # Condition (b) mutation control: identity and the two-file diff are green.
        for context in ("pineforge/verify", "pineforge/parity"):
            with self.subTest(context=context):
                self.reset_bump()
                self.api.statuses([row for row in successes() if row["context"] != context])
                self.assert_refused(f"PR #17 head {HEAD}: missing {context}")

    def test_parent_still_requires_trusted_newest_success_and_matching_tree(self):
        for variant, fragment in (
                ("red", "pineforge/verify=failure (expected success)"),
                ("creator", "refused status creator"),
                ("tree", f"no merged pull request head matches release tree {TREE}"),
                ("ambiguous", "multiple merged PR heads match (#17, #18)")):
            with self.subTest(variant=variant):
                self.reset_bump()
                if variant == "red":
                    self.api.statuses([status("pineforge/verify", "failure"), *successes()])
                elif variant == "creator":
                    rows = successes()
                    rows[0]["creator"] = {"login": "outsider", "type": "User"}
                    self.api.statuses(rows)
                elif variant == "tree":
                    self.api.responses[f"repos/{REPO}/commits/{HEAD}"] = json.dumps(
                        commit(HEAD, OTHER_TREE))
                else:
                    self.api.responses[PARENT_PULLS_API] = json.dumps([
                        pull(merge_sha=BUMP_PARENT), pull(number=18, merge_sha=BUMP_PARENT)])
                self.assert_refused(fragment)

    def test_bump_on_bump_refuses_despite_a_gated_grandparent(self):
        grandparent, grandtree = "9" * 40, "0" * 40
        parent = commit(BUMP_PARENT, TREE)
        parent["parents"] = [{"sha": grandparent}]
        self.api.responses.update({
            PARENT_PULLS_API: "[]",
            f"repos/{REPO}/commits/{BUMP_PARENT}": json.dumps(parent),
            f"repos/{REPO}/actions/runs?head_sha={BUMP_PARENT}&per_page=100":
                runs_page([push_run(sha=BUMP_PARENT)]),
            f"repos/{REPO}/commits/{grandparent}/pulls":
                json.dumps([pull(merge_sha=grandparent)]),
            f"repos/{REPO}/commits/{grandparent}": json.dumps(commit(grandparent, grandtree)),
            f"repos/{REPO}/commits/{HEAD}": json.dumps(commit(HEAD, grandtree)),
            f"repos/{REPO}/git/trees/{grandtree}?recursive=1":
                git_tree(grandtree, [
                    {**row, "sha": "a" * 40}
                    if row["path"] in ("VERSION", "docker/run_failure_codes_diff.json") else row
                    for row in bump_files()]),
        })
        self.assert_refused(f"no merged pull request associated with {BUMP_PARENT}")
        self.assertFalse(any(grandparent in endpoint for endpoint, _, _ in self.api.calls))

    def test_merge_root_and_malformed_parent_lists_refuse(self):
        for parents in ([], [{"sha": BUMP_PARENT}, {"sha": HEAD}],
                        None, [{"sha": "bad"}], [{}]):
            with self.subTest(parents=parents):
                self.reset_bump()
                candidate = commit(SHA, OTHER_TREE)
                candidate["parents"] = parents
                self.api.responses[BUMP_API] = json.dumps(candidate)
                self.assert_refused("release bump requires exactly one valid parent")

    def test_deleted_renamed_added_or_missing_allowlisted_file_refuses(self):
        for path in ("VERSION", "docker/run_failure_codes_diff.json"):
            for change in ("deleted", "renamed", "added", "unchanged"):
                with self.subTest(path=path, change=change):
                    self.reset_bump()
                    before, after = bump_files(), bump_files(after=True)
                    if change == "deleted":
                        after = [row for row in after if row["path"] != path]
                    elif change == "renamed":
                        after = [{**row, "path": path + ".renamed"}
                                 if row["path"] == path else row for row in after]
                    elif change == "added":
                        before = [row for row in before if row["path"] != path]
                    else:
                        original = next(row for row in before if row["path"] == path)
                        after = [original if row["path"] == path else row for row in after]
                    self.api.responses[BEFORE_TREE_API] = git_tree(TREE, before)
                    self.api.responses[AFTER_TREE_API] = git_tree(OTHER_TREE, after)
                    self.assert_refused("release bump must modify exactly VERSION and "
                                        "docker/run_failure_codes_diff.json")

    def test_mode_change_or_nonregular_file_on_either_side_refuses(self):
        for path in ("VERSION", "docker/run_failure_codes_diff.json"):
            for sides in (("before",), ("after",), ("before", "after")):
                for mode, kind in (("100755", "blob"), ("120000", "blob"),
                                   ("160000", "commit")):
                    with self.subTest(path=path, sides=sides, mode=mode):
                        self.reset_bump()
                        for side in sides:
                            entries = [{**row, "mode": mode, "type": kind}
                                       if row["path"] == path else row
                                       for row in bump_files(after=side == "after")]
                            endpoint, tree = ((BEFORE_TREE_API, TREE) if side == "before"
                                              else (AFTER_TREE_API, OTHER_TREE))
                            self.api.responses[endpoint] = git_tree(tree, entries)
                        self.assert_refused(f"release bump path {path} must stay mode 100644")

    def test_zero_changed_files_refuses(self):
        for same_tree in (False, True):
            with self.subTest(same_tree=same_tree):
                self.reset_bump()
                self.api.responses[AFTER_TREE_API] = git_tree(OTHER_TREE, bump_files())
                if same_tree:
                    candidate = json.loads(self.api.responses[BUMP_API])
                    candidate["commit"]["tree"]["sha"] = TREE
                    self.api.responses[BUMP_API] = json.dumps(candidate)
                self.assert_refused("release bump must modify exactly VERSION and "
                                    "docker/run_failure_codes_diff.json")
                self.assertEqual(Counter(endpoint for endpoint, _, _ in self.api.calls)[
                    BEFORE_TREE_API], 1)
                if same_tree:
                    self.assertNotIn((AFTER_TREE_API, False, None), self.api.calls)

    def test_truncated_or_malformed_git_trees_refuse(self):
        for endpoint, tree, after in ((BEFORE_TREE_API, TREE, False),
                                      (AFTER_TREE_API, OTHER_TREE, True)):
            for payload, fragment in (
                    (git_tree(tree, bump_files(after=after), truncated=True),
                     "invalid or truncated git tree"),
                    (git_tree("9" * 40, bump_files(after=after)), "invalid or truncated git tree"),
                    (json.dumps({"sha": tree, "tree": []}), "invalid or truncated git tree"),
                    (git_tree(tree, [None]), "invalid git tree entry"),
                    (git_tree(tree, [tree_entry("VERSION", "bad")]), "invalid git tree entry"),
                    (git_tree(tree, [*bump_files(after=after), bump_files(after=after)[0]]),
                     "duplicate git tree path")):
                with self.subTest(endpoint=endpoint, payload=payload):
                    self.reset_bump()
                    self.api.responses[endpoint] = payload
                    self.assert_refused(f"API {endpoint} returned {fragment}")

    def test_bump_evidence_read_errors_stop_without_retry_or_later_reads(self):
        for endpoint in (RUNS_API, BUMP_API, PARENT_PULLS_API,
                         f"repos/{REPO}/commits/{BUMP_PARENT}", STATUSES_API,
                         BEFORE_TREE_API, AFTER_TREE_API):
            for payload, fragment in ((http_response(503, "unavailable"), "upstream error 503"),
                                      ("", "returned an empty payload"),
                                      ("not JSON", "returned invalid JSON")):
                with self.subTest(endpoint=endpoint, payload=payload):
                    self.reset_bump()
                    self.api.responses[endpoint] = (
                        http_response(200, payload) if endpoint == RUNS_API
                        and not payload.startswith("HTTP/") else payload)
                    self.assert_refused(f"API {endpoint} {fragment}")
                    self.assertEqual(self.api.calls[-1][0], endpoint)
                    self.assertTrue(all(count == 1 for count in Counter(
                        called for called, _, _ in self.api.calls).values()))

    def test_phase_two_missing_receipt_cannot_use_a_valid_bump(self):
        self.enable_watcher()
        self.assert_refused(f"main-proof receipt is missing for {SHA}")
        self.assertEqual(self.api.calls, [(MAIN_API, False, ".commit.sha")])
        self.proof.assert_called_once_with(REPO, SHA)

    def test_failed_candidate_pr_check_cannot_use_an_otherwise_valid_bump(self):
        self.api.responses[PULLS_API] = json.dumps([pull()])
        self.assert_refused(f"no merged pull request head matches release tree {OTHER_TREE}")
        self.assertNotIn((RUNS_API, True, None), self.api.calls)


class ReleaseRulesetTests(unittest.TestCase):
    # Only transport is fake: every row goes through main() and the real gate().
    run_gate = GateTests.run_gate
    assert_refused = GateTests.assert_refused
    setUp = ReleaseBumpTests.setUp
    reset_bump = ReleaseBumpTests.reset_bump
    assert_bump_green = ReleaseBumpTests.assert_bump_green

    def test_overlapping_role_and_team_bypasses_are_not_an_app_only_barrier(self):
        # RED on a8b12768f1: descriptor intersection is App-only even though
        # a role-5 member of team 123 can bypass both pull-request rulesets.
        self.api.rulesets([
            ruleset(101, rules=("pull_request",),
                    actors=[bypass(), bypass(5, "RepositoryRole")]),
            ruleset(rules=("pull_request",), actors=[bypass(), bypass(123, "Team")]),
        ])
        self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_status_checks_cannot_complete_a_broader_pull_request_ruleset(self):
        # RED on a8b12768f1: a person can bypass the PR ruleset and satisfy
        # the status check normally, without bypassing the second ruleset.
        self.api.rulesets([
            ruleset(101, rules=("pull_request",),
                    actors=[bypass(), bypass(5, "RepositoryRole")]),
            ruleset(rules=("required_status_checks",)),
        ])
        self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_second_bypass_actor_cannot_loosen_the_app_only_barrier(self):
        # The Team row is RED on a8b12768f1: it is absent from the broad
        # ruleset, so the descriptor intersection still contains only the App.
        for actor in (bypass(5, "RepositoryRole"), bypass(5219872),
                      bypass(None, "OrganizationAdmin"), bypass(None, "DeployKey"),
                      bypass(123, "Team")):
            with self.subTest(actor=actor):
                self.reset_bump()
                rows = release_rulesets()
                rows[1]["bypass_actors"].append(actor)
                self.api.rulesets(rows)
                self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_disabled_or_evaluating_main_restriction_refuses(self):
        for enforcement in ("disabled", "evaluate"):
            with self.subTest(enforcement=enforcement):
                self.reset_bump()
                rows = release_rulesets()
                rows[1]["enforcement"] = enforcement
                self.api.rulesets(rows)
                self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_removed_strict_restriction_cannot_be_replaced_by_tag_protection(self):
        self.api.rulesets([release_rulesets()[0], release_rulesets()[2]])
        self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_at_least_one_applicable_pull_request_barrier_is_required(self):
        for rows in ([], [release_rulesets()[2]],
                     [ruleset(rules=("deletion", "required_linear_history"))],
                     [ruleset(include=["refs/heads/release"])],
                     [ruleset(exclude=["refs/heads/main"])]):
            with self.subTest(rows=rows):
                self.reset_bump()
                self.api.rulesets(rows)
                self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_barrier_bypasses_must_be_exactly_the_always_release_app(self):
        for actors in ([], [bypass(mode="pull_request")], [bypass(5219872)],
                       [bypass(4105147, "Team")]):
            with self.subTest(actors=actors):
                self.reset_bump()
                rows = release_rulesets()
                rows[1]["bypass_actors"] = actors
                self.api.rulesets(rows)
                self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_each_ruleset_api_error_stops_without_retry_or_later_reads(self):
        for endpoint in (REPO_API, RULESETS_API, DEFAULT_RULESET_API, STRICT_RULESET_API):
            for payload, fragment in (
                    (OSError("connection refused"), "failed: connection refused"),
                    (subprocess.CalledProcessError(
                        1, ["gh", "api", endpoint], stderr="gh: Forbidden (HTTP 403)"),
                     "failed with HTTP 403"),
                    (http_response(503, "unavailable"), "upstream error 503"),
                    ("", "returned an empty payload"),
                    ("not JSON",
                     "returned incomplete ruleset pagination: missing HTTP page headers"
                     if endpoint == RULESETS_API else "returned invalid JSON")):
                with self.subTest(endpoint=endpoint, fragment=fragment):
                    self.reset_bump()
                    # Keep the existing empty-body diagnostic reachable: the
                    # inventory already requires HTTP page headers on main.
                    self.api.responses[endpoint] = (
                        http_response(200, "") if endpoint == RULESETS_API and payload == ""
                        else payload)
                    self.assert_refused(f"API {endpoint} {fragment}")
                    self.assertEqual(self.api.calls[-1][0], endpoint)
                    self.assertTrue(all(count == 1 for count in Counter(
                        called for called, _, _ in self.api.calls).values()))

    def test_missing_bypass_actors_refuses_instead_of_assuming_token_visibility(self):
        for index in (0, 1):
            with self.subTest(index=index):
                self.reset_bump()
                rows = release_rulesets()
                del rows[index]["bypass_actors"]
                self.api.rulesets(rows)
                endpoint = DEFAULT_RULESET_API if index == 0 else STRICT_RULESET_API
                self.assert_refused(f"API {endpoint} returned missing or invalid bypass_actors")

    def test_malformed_ruleset_inventory_or_details_refuse(self):
        strict = ruleset()
        for endpoint, payload, fragment in (
                (RULESETS_API, http_response(200, "{}"), "an empty payload"),
                (RULESETS_API, http_response(200, "[null]"), "invalid ruleset summary"),
                (RULESETS_API, http_response(200, json.dumps([{"id": True}])),
                 "invalid ruleset summary"),
                (RULESETS_API, http_response(200, json.dumps(
                    ruleset_summaries([strict, strict]))), "duplicate ruleset id 202"),
                (STRICT_RULESET_API, json.dumps({**strict, "id": 999}),
                 "inconsistent ruleset detail"),
                (STRICT_RULESET_API, json.dumps({**strict, "enforcement": "disabled"}),
                 "inconsistent ruleset detail"),
                (STRICT_RULESET_API, json.dumps({**strict, "conditions": {}}),
                 "invalid ref_name conditions"),
                (STRICT_RULESET_API, json.dumps({**strict, "conditions": {
                    "ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": None}}}),
                 "invalid ref_name conditions"),
                (STRICT_RULESET_API, json.dumps({**strict, "rules": [None]}),
                 "invalid rules"),
                (STRICT_RULESET_API, json.dumps({**strict, "bypass_actors": None}),
                 "missing or invalid bypass_actors"),
                (STRICT_RULESET_API, json.dumps({**strict, "bypass_actors": [{}]}),
                 "invalid bypass actor"),
                (STRICT_RULESET_API, json.dumps({**strict, "bypass_actors": [
                    bypass(True), bypass()]}), "invalid bypass actor"),
                (STRICT_RULESET_API, json.dumps({**strict, "bypass_actors": [
                    bypass(mode="unknown")]}), "invalid bypass actor")):
            with self.subTest(endpoint=endpoint, fragment=fragment):
                self.reset_bump()
                self.api.responses[endpoint] = payload
                self.assert_refused(f"API {endpoint} returned {fragment}")

    def test_main_must_still_be_the_default_branch_for_default_branch_conditions(self):
        self.api.responses[REPO_API] = json.dumps({"default_branch": "release"})
        self.assert_refused(f"API {REPO_API} must identify main as the default branch")

    def test_app_only_ruleset_on_another_or_excluded_branch_cannot_hide_shared_human(self):
        for changes in ({"target": "tag"},
                        {"conditions": {"ref_name": {
                            "include": ["refs/heads/release"], "exclude": []}}},
                        {"conditions": {"ref_name": {
                            "include": ["~DEFAULT_BRANCH"], "exclude": ["refs/heads/m*"]}}}):
            with self.subTest(changes=changes):
                self.reset_bump()
                rows = release_rulesets()
                rows[1].update(changes)
                self.api.rulesets(rows)
                self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_other_bypass_lists_do_not_invalidate_an_existing_app_only_barrier(self):
        # The other-App and disabled/evaluate rows are RED on a8b12768f1:
        # it intersects unrelated bypasses or requires every blocker active.
        for other in (release_rulesets()[0], ruleset(404, actors=[bypass(5219872)]),
                      ruleset(404, enforcement="disabled"),
                      ruleset(404, enforcement="evaluate")):
            for barrier_first in (False, True):
                with self.subTest(other=other, barrier_first=barrier_first):
                    self.reset_bump()
                    rows = [ruleset(), other] if barrier_first else [other, ruleset()]
                    self.api.rulesets(rows)
                    self.assert_bump_green()
                    for row in rows:
                        self.assertIn((f"repos/{REPO}/rulesets/{row['id']}", False, None),
                                      self.api.calls)

    def test_incomplete_ruleset_pagination_refuses_even_with_an_app_only_first_page(self):
        summaries = json.dumps(ruleset_summaries(release_rulesets()))
        next_link = {"Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'}
        for raw in (summaries, http_response(200, summaries, next_link),
                    http_response(200, summaries, {
                        "Link": f'<https://api.github.com/{RULESETS_API}&page=3>; rel="next"'})
                    + http_response(200, "[]"),
                    http_response(200, summaries) + http_response(200, "[]"),
                    http_response(200, summaries, {"Link": "unreadable"}),
                    http_response(200, summaries, {
                        "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="last"'})):
            with self.subTest(raw=raw):
                self.reset_bump()
                self.api.responses[RULESETS_API] = raw
                self.assert_refused(f"API {RULESETS_API} returned incomplete ruleset pagination")

    def test_later_ruleset_pages_are_read_before_deciding(self):
        rows = [ruleset(), ruleset(404, actors=[bypass(5219872)])]
        del rows[1]["bypass_actors"]
        self.api.rulesets(rows)
        self.api.responses[RULESETS_API] = (
            http_response(200, json.dumps(ruleset_summaries(rows[:1])), {
                "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'})
            + http_response(200, json.dumps(ruleset_summaries(rows[1:]))))
        self.assert_refused(f"API repos/{REPO}/rulesets/404 returned "
                            "missing or invalid bypass_actors")
        self.assertIn((f"repos/{REPO}/rulesets/404", False, None), self.api.calls)

    def test_app_only_barrier_may_arrive_on_a_later_ruleset_page(self):
        rows = [release_rulesets()[0], ruleset()]
        self.api.rulesets(rows)
        self.api.responses[RULESETS_API] = (
            http_response(200, json.dumps(ruleset_summaries(rows[:1])), {
                "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'})
            + http_response(200, json.dumps(ruleset_summaries(rows[1:]))))
        self.assert_bump_green()
        self.assertIn((STRICT_RULESET_API, False, None), self.api.calls)

    def test_green_finds_the_barrier_without_pinning_ruleset_ids_or_names(self):
        rows = release_rulesets()
        for row in rows:
            row["id"] += 700
            row["name"] = "renamed protection"
        rows[1]["conditions"]["ref_name"]["include"] = ["refs/heads/main"]
        self.api.rulesets(rows)
        self.api.responses[RUNS_API] = runs_page([push_run(), push_run()])
        self.assert_bump_green()
        self.assertIn((RULESETS_API, True, None), self.api.calls)
        self.assertIn((f"repos/{REPO}/rulesets/902", False, None), self.api.calls)

    def test_green_complete_ruleset_pages_may_end_with_an_empty_page(self):
        summaries = json.dumps(ruleset_summaries(release_rulesets()))
        self.api.responses[RULESETS_API] = (
            http_response(200, summaries, {
                "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'})
            + http_response(200, "[]"))
        self.assert_bump_green()

    def test_pull_request_and_supported_main_conditions_can_supply_the_barrier(self):
        for include in ("~DEFAULT_BRANCH", "refs/heads/main", "~ALL", "refs/heads/m?in"):
            with self.subTest(include=include):
                self.reset_bump()
                self.api.rulesets([ruleset(rules=("pull_request",), include=[include])])
                self.assert_bump_green()

    def test_status_checks_alone_cannot_supply_the_barrier(self):
        # RED on a8b12768f1: required_status_checks is treated as a push
        # blocker and its App-only bypass intersection permits this bump.
        for include in ("~DEFAULT_BRANCH", "refs/heads/main", "~ALL", "refs/heads/m?in"):
            with self.subTest(include=include):
                self.reset_bump()
                self.api.rulesets([
                    ruleset(rules=("required_status_checks",), include=[include])])
                self.assert_refused(RULESET_BARRIER_REFUSAL)

    def test_unsupported_ref_patterns_refuse_instead_of_guessing_applicability(self):
        for pattern in ("~UNKNOWN", "refs/**/main", "refs/heads/[m]ain",
                        "refs/heads/ma\\in"):
            with self.subTest(pattern=pattern):
                self.reset_bump()
                self.api.rulesets([ruleset(include=[pattern])])
                self.assert_refused(f"API {STRICT_RULESET_API} returned invalid "
                                    "ref_name conditions: unsupported pattern")

    def test_failed_later_ruleset_page_cannot_hide_behind_a_green_first_page(self):
        summaries = json.dumps(ruleset_summaries(release_rulesets()))
        for body, code, fragment in (("unavailable", 503, "upstream error 503"),
                                     ("", 200, "returned an empty payload"),
                                     ("not JSON", 200, "returned invalid JSON")):
            with self.subTest(fragment=fragment):
                self.reset_bump()
                self.api.responses[RULESETS_API] = (
                    http_response(200, summaries, {
                        "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'})
                    + http_response(code, body))
                self.assert_refused(f"API {RULESETS_API} {fragment}")
                self.assertEqual(self.api.calls[-1], (RULESETS_API, True, None))

    def test_removing_each_new_condition_admits_its_unsafe_input(self):
        # Execute complete source mutants, never a stubbed gate/helper. The
        # ordinary gate must refuse each input before the mutant admits it.
        source = Path(ci_release_gate.__file__).read_text()
        strict = ruleset()
        missing = {key: value for key, value in strict.items() if key != "bypass_actors"}
        loose = {**strict, "bypass_actors": [bypass(), bypass(5, "RepositoryRole")]}

        def responses_for(rows):
            return {
                RULESETS_API: http_response(200, json.dumps(ruleset_summaries(rows))),
                **{f"repos/{REPO}/rulesets/{row['id']}": json.dumps(row) for row in rows},
            }

        applicability = 'if not applies or not blocking or payload["enforcement"] != "active":'
        exact_bypasses = 'if bypasses == {("Integration", RELEASE_APP_ID, "always")}:'
        pr_rule = 'blocking = any(rule["type"] == "pull_request" for rule in rules)'
        status_too = (
            'blocking = any(rule["type"] in ("pull_request", "required_status_checks") '
            'for rule in rules)')
        single_document = "if body[document_end:].strip():"
        cases = [
            ("reader call", "    _require_release_rulesets(api, repo)\n", "",
             responses_for([missing]), "missing or invalid bypass_actors"),
            ("nonempty barrier population", "if not barrier_found:", "if False:",
             responses_for([]), RULESET_BARRIER_REFUSAL),
            ("disabled enforcement", ' or payload["enforcement"] != "active"', "",
             responses_for([ruleset(enforcement="disabled")]), RULESET_BARRIER_REFUSAL),
            ("evaluate enforcement", ' or payload["enforcement"] != "active"', "",
             responses_for([ruleset(enforcement="evaluate")]), RULESET_BARRIER_REFUSAL),
            ("exact own bypass list", exact_bypasses, "if bypasses:",
             responses_for([loose]), RULESET_BARRIER_REFUSAL),
            ("overlapping role and team principals", exact_bypasses,
             'if ("Integration", RELEASE_APP_ID, "always") in bypasses:',
             responses_for([
                 ruleset(101, rules=("pull_request",),
                         actors=[bypass(), bypass(5, "RepositoryRole")]),
                 ruleset(rules=("pull_request",), actors=[bypass(), bypass(123, "Team")]),
             ]), RULESET_BARRIER_REFUSAL),
            ("all branch details read after finding a barrier",
             "            barrier_found = True", "            return",
             responses_for([ruleset(101), missing]), "missing or invalid bypass_actors"),
            ("actor type", "        identity = (kind, number, mode)",
             '        identity = ("Integration", number, mode)',
             responses_for([ruleset(actors=[bypass(4105147, "Team")])]),
             RULESET_BARRIER_REFUSAL),
            ("always mode", "        identity = (kind, number, mode)",
             '        identity = (kind, number, "always")',
             responses_for([ruleset(actors=[bypass(mode="pull_request")])]),
             RULESET_BARRIER_REFUSAL),
            ("App id", "RELEASE_APP_ID = 4105147", "RELEASE_APP_ID = 5219872",
             responses_for([ruleset(actors=[bypass(5219872)])]), RULESET_BARRIER_REFUSAL),
            ("visible bypass lists", 'actors = payload.get("bypass_actors")',
             'actors = payload.get("bypass_actors", [{"actor_type": "Integration", '
             '"actor_id": RELEASE_APP_ID, "bypass_mode": "always"}])',
             responses_for([missing]), "missing or invalid bypass_actors"),
            ("complete ruleset pagination", "paginate=True, complete_rulesets=True", "paginate=True",
             {RULESETS_API: http_response(
                 200, json.dumps(ruleset_summaries(release_rulesets())), {
                     "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'})},
             "incomplete ruleset pagination"),
            ("complete Actions pagination",
             'complete_pages="ruleset" if complete_rulesets else items_key',
             'complete_pages="ruleset" if complete_rulesets else None',
             {RUNS_API: runs_page([push_run()], headers={
                 "Link": f'<https://api.github.com/{RUNS_API}&page=2>; rel="next"'})},
             "incomplete workflow_runs pagination: unread or unlinked next page"),
            ("single Actions document per HTTP body", single_document, "if False:",
             {RUNS_API: runs_page([push_run()]) + '{"total_count":1,"workflow_runs":[]}'},
             f"API {RUNS_API} {SINGLE_DOCUMENT_REFUSAL}"),
            ("single ruleset document per HTTP body", single_document, "if False:",
             {RULESETS_API: http_response(
                 200, json.dumps(ruleset_summaries(release_rulesets()))) + "[]"},
             f"API {RULESETS_API} {SINGLE_DOCUMENT_REFUSAL}"),
            ("branch target", 'if summary["target"] != "branch":', "if False:",
             responses_for([ruleset(target="tag", include=["~ALL"])]), RULESET_BARRIER_REFUSAL),
            ("main inclusion", applicability,
             'if not blocking or payload["enforcement"] != "active":',
             responses_for([ruleset(include=["refs/heads/release"])]), RULESET_BARRIER_REFUSAL),
            ("main exclusion", applicability,
             'if not blocking or payload["enforcement"] != "active":',
             responses_for([ruleset(exclude=["refs/heads/main"])]), RULESET_BARRIER_REFUSAL),
            ("pull-request rule", applicability,
             'if not applies or payload["enforcement"] != "active":',
             responses_for([ruleset(rules=("deletion",))]), RULESET_BARRIER_REFUSAL),
            ("status-only ruleset is not a barrier", pr_rule, status_too,
             responses_for([ruleset(rules=("required_status_checks",))]), RULESET_BARRIER_REFUSAL),
            ("status checks cannot complete a broader PR ruleset", pr_rule, status_too,
             responses_for([
                 ruleset(101, rules=("pull_request",),
                         actors=[bypass(), bypass(5, "RepositoryRole")]),
                 ruleset(rules=("required_status_checks",)),
             ]), RULESET_BARRIER_REFUSAL),
            ("default branch", 'repository.get("default_branch") != "main"', "False",
             {REPO_API: json.dumps({"default_branch": "release"})},
             "must identify main as the default branch"),
        ]
        for name, before, after, responses, diagnostic in cases:
            with self.subTest(condition=name):
                self.reset_bump()
                self.api.responses.update(responses)
                self.assert_refused(diagnostic)
                self.assertEqual(source.count(before), 1, "mutation must have one source target")
                mutated = source.replace(before, after, 1)
                self.assertNotEqual(mutated, source)
                namespace = {"__name__": "ci_release_gate_condition_mutant"}
                exec(compile(mutated, ci_release_gate.__file__, "exec"), namespace)
                self.api.calls.clear()
                green = namespace["gate"](REPO, SHA, "refs/heads/main",
                                          api=self.api, git=self.git)
                self.assertEqual(green, (
                    f"release gate green: release bump {SHA}; parent {BUMP_PARENT}; "
                    f"PR #17 head {HEAD}; pineforge/verify=success; "
                    f"pineforge/parity=success; tree {TREE}"))


class APICommandTests(unittest.TestCase):
    def test_json_reader_accepts_an_empty_array_with_or_without_pagination(self):
        for paginate in (False, True):
            with self.subTest(paginate=paginate):
                api = mock.Mock(return_value="[]")
                self.assertEqual(ci_release_gate._read_json(api, PULLS_API, paginate=paginate), [])
                api.assert_called_once_with(PULLS_API, paginate=paginate)

    def test_gh_adapter_paginates_the_statuses_list(self):
        with mock.patch.object(ci_release_gate.subprocess, "run") as run:
            run.return_value.stdout = "[]\n"
            self.assertEqual(ci_release_gate.gh_api(STATUSES_API, paginate=True), "[]\n")
        run.assert_called_once_with(["gh", "api", STATUSES_API, "--include", "--paginate"],
                                    check=True, capture_output=True, text=True)

    def test_gh_adapter_reads_live_main_sha_with_jq(self):
        with mock.patch.object(ci_release_gate.subprocess, "run") as run:
            run.return_value.stdout = SHA + "\n"
            self.assertEqual(ci_release_gate.gh_api(MAIN_API, jq=".commit.sha"), SHA + "\n")
        run.assert_called_once_with(["gh", "api", MAIN_API, "--include", "--jq", ".commit.sha"],
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
    def test_checked_in_cut_over_is_a_literal_without_environment_or_cli_overrides(self):
        source = Path(ci_release_gate.__file__).read_text()
        self.assertRegex(source, r"(?m)^MAIN_PROOF_WATCHER_FIRST_SHA: str \| None = "
                         r"(?:None|\"[0-9a-f]{40}\"|'[0-9a-f]{40}')$")
        self.assertNotRegex(source, r"\b(?:environ|getenv)\b")
        module = ast.parse(source)
        writes = [node for node in ast.walk(module)
                  if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store)
                  and node.id == "MAIN_PROOF_WATCHER_FIRST_SHA"]
        self.assertEqual(len(writes), 1, "the cut-over must have only its checked-in assignment")
        main = next(node for node in module.body
                    if isinstance(node, ast.FunctionDef) and node.name == "main")
        flags = [ast.literal_eval(argument)
                 for node in ast.walk(main)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                 and node.func.attr == "add_argument"
                 for argument in node.args]
        self.assertEqual(flags, ["--repo", "--sha", "--ref"],
                         "main must offer no cut-over override flag")

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

    def assert_release_success_pin(self, workflow):
        jobs = _jobs(workflow)
        self.assertRegex(jobs["prebuilt"], r"(?m)^    needs: release$")
        for job in ("release", "prebuilt"):
            stripped = _without_comments(jobs[job])
            self.assertNotRegex(stripped, r"\balways\s*\(")
            self.assertNotRegex(stripped, r"!\s*cancelled\s*\(")
            self.assertNotRegex(stripped, r"(?m)^\s*continue-on-error\s*:")

    def test_ci_release_gate_is_unconditional_and_precedes_every_release_write(self):
        self.assert_gate_pin(self.workflow())

    def test_ruleset_reader_permission_dependency_is_documented_and_fixed_in_code(self):
        source = Path(ci_release_gate.__file__).read_text()
        docs = (ROOT / "docs/ci.md").read_text()
        self.assertRegex(source, r"(?m)^RELEASE_APP_ID = 4105147$")
        for text in (source, docs, self.workflow()):
            self.assertIn("administration: read", text)
            self.assertIn("bypass_actors", text)
        self.assertIn("# The gate relies on App permission administration: read to see bypass_actors;",
                      source)
        self.assertIn("Removing this App permission makes every release-bump exemption refuse.",
                      docs)

    def test_prebuilt_builds_the_emitted_sha_and_asserts_tag_identity(self):
        jobs = _jobs(self.workflow())
        self.assertRegex(jobs["release"],
                         r"(?m)^      sha:\s+\$\{\{ steps\.publish\.outputs\.sha \}\}$")
        push = next(step for step in _steps(jobs["release"])
                    if step.startswith("      - name: Write bumped VERSION, commit, tag, push\n"))
        self.assertIn("        id: publish\n", push)
        self.assertIn('          git push --atomic origin HEAD "${TAG}"\n'
                      '          echo "sha=$(git rev-parse HEAD)" >> "$GITHUB_OUTPUT"', push)

        steps = _steps(jobs["prebuilt"])
        checkout = next(step for step in steps if "uses: actions/checkout@v4" in step)
        self.assertIn("          ref: ${{ inputs.dry_run && github.sha || needs.release.outputs.sha }}\n",
                      checkout)
        self.assertIn("          fetch-depth: 0", checkout)
        assertion = next(step for step in steps if step.startswith(
            "      - name: Assert tag still names the emitted release commit\n"))
        self.assertLess(steps.index(checkout), steps.index(assertion))
        configure = next(step for step in steps if step.startswith("      - name: Configure\n"))
        self.assertLess(steps.index(assertion), steps.index(configure))
        self.assertIn("        if: ${{ !inputs.dry_run }}\n", assertion)
        self.assertIn("          TAG: ${{ needs.release.outputs.tag }}\n", assertion)
        self.assertIn("          SHA: ${{ needs.release.outputs.sha }}\n", assertion)
        script = assertion.split("        run: |\n", 1)[1]
        self.assertNotIn("${{", script)
        self.assertIn("          set -euo pipefail\n", script)
        self.assertIn('          if [[ ! "${SHA}" =~ ^[0-9a-f]{40}$ ]]; then\n'
                      '            echo "::error::invalid emitted release sha: ${SHA}" >&2\n'
                      '            exit 1\n'
                      '          fi\n', script)
        self.assertIn('          test "$(git rev-parse "refs/tags/${TAG}^{commit}")" = "${SHA}"',
                      script)

    def test_prebuilt_requires_release_success_without_failure_bypasses(self):
        self.assert_release_success_pin(self.workflow())

    def test_prebuilt_failure_bypass_mutations_are_refused(self):
        workflow = self.workflow()
        self.assertIn("no continue-on-error", _jobs(workflow)["prebuilt"])
        anchor = "  prebuilt:\n    needs: release\n"
        for bypass, message in (
            ("    continue-on-error: true\n",
             r"^Regex matched: '    continue-on-error:'"),
            ("    if: ${{ always() }}\n", r"^Regex matched: 'always\('"),
        ):
            with self.subTest(bypass=bypass):
                self.assert_release_success_pin(workflow)
                changed = workflow.replace(anchor, anchor + bypass, 1)
                self.assertNotEqual(changed, workflow, "the mutation must change the input")
                self.assertIn(bypass, _jobs(changed)["prebuilt"])
                with self.assertRaisesRegex(AssertionError, message):
                    self.assert_release_success_pin(changed)

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
