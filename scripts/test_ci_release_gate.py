#!/usr/bin/env python3
"""Release gate decisions and text pins; no subprocesses or network in tests."""
from __future__ import annotations

import ast
import contextlib
from collections import Counter
import errno
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unicodedata
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
RULESET_INVENTORY_HEADER = (
    "branch ruleset inventory (includes_parents=true; branch targets only; tag/push omitted):\n")


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


def ci_post_runs_api(sha, *, repo=REPO):
    return f"repos/{repo}/actions/workflows/ci-post.yml/runs?head_sha={sha}&per_page=100"


def ci_post_jobs_api(run_id, *, repo=REPO):
    return f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"


def ci_post_run(run_id, *, sha=SHA, branch="main", state="completed", conclusion="success",
                created_at="2026-10-25T12:00:00Z", event="push"):
    return {**push_run(event=event, branch=branch, sha=sha), "id": run_id, "status": state,
            "conclusion": conclusion, "created_at": created_at}


def ci_post_jobs():
    # One required-name constant drives the gate and its shared job fixtures.
    return [{"id": index, "name": name,
             "status": "completed", "conclusion": "success"}
            for index, name in enumerate(ci_release_gate.CI_POST_BUILD_JOBS, start=1)]


def linux_ci_post_jobs():
    # Keep these regression inputs Linux-only even against the previous gate.
    # Derive arm64 rows from amd64, without copying the required-name tuple.
    amd64 = [job for job in ci_post_jobs()
             if job["name"].startswith("build (ubuntu-24.04, ")]
    return amd64 + [
        {**job, "id": job["id"] + len(amd64),
         "name": job["name"].replace("ubuntu-24.04,", "ubuntu-24.04-arm,")}
        for job in amd64]


def jobs_page(rows, *, total=None):
    return http_response(200, json.dumps({
        "total_count": len(rows) if total is None else total, "jobs": rows}))


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

    def ci_post(self, row, jobs, *, repo=REPO):
        # Opt in only: old rows retain their exact responses and call lists.
        self.responses.update({
            ci_post_runs_api(row["head_sha"], repo=repo): runs_page([row]),
            ci_post_jobs_api(row["id"], repo=repo): jobs_page(jobs),
        })

    def ci_post_runs(self, sha, rows, jobs_by_run, *, repo=REPO):
        # Register only the exact runs/jobs endpoints this fixture supplies.
        self.responses[ci_post_runs_api(sha, repo=repo)] = runs_page(rows)
        self.responses.update({
            ci_post_jobs_api(run_id, repo=repo): jobs_page(jobs)
            for run_id, jobs in jobs_by_run.items()
        })

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


class FakeHistory:
    """First-parent diffs only; the real gate owns documentation classification."""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def __call__(self, sha):
        self.calls.append(sha)
        if sha not in self.responses:
            raise AssertionError(f"unexpected history sha: {sha}")
        response = self.responses[sha]
        if isinstance(response, Exception):
            raise response
        parent, paths = response
        return parent, list(paths)


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

    def assert_no_pr_fallback(self, *, ci_post=False):
        reads = [(MAIN_API, False, ".commit.sha")]
        if ci_post:
            # A green receipt now also needs ci-post; refusal rows still stop at main.
            reads.extend([
                (ci_post_runs_api(self.post_run["head_sha"]), True, None),
                (ci_post_jobs_api(self.post_run["id"]), False, None),
            ])
        self.assertEqual(self.api.calls, reads,
                         "phase 2 must never read PR heads or their statuses")

    def run_gate(self, *, repo=REPO, sha=SHA, ref="refs/heads/main"):
        # Opt in here so old evidence rows reach their original decision with
        # explicit green ci-post fixtures; the dedicated ci-post harness stays independent.
        self.post_run = ci_post_run(8341, sha=sha)
        self.api.ci_post(self.post_run, ci_post_jobs(), repo=repo)
        self.history = FakeHistory({sha: (OTHER_HEAD, ["src/old_gate_fixture.cpp"])})
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = ci_release_gate.main(
                ["gate", "--repo", repo, "--sha", sha, "--ref", ref],
                api=self.api, git=self.git, history=self.history)
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
        # Keep the original evidence text exact and append the newly required proof.
        self.assertEqual(stdout, (
            f"release gate green: PR #17 head {HEAD}; pineforge/verify=success; "
            f"pineforge/parity=success; tree {TREE}; "
            f"ci-post run {self.post_run['id']} for {self.post_run['head_sha']}\n"))
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
        # Phase two retains its receipt evidence and now names ci-post too.
        self.assertEqual(stdout, (
            f"release gate green: main-proof receipt for {SHA}; "
            "pineforge/verify=success; pineforge/parity=success; "
            f"ci-post run {self.post_run['id']} for {self.post_run['head_sha']}\n"))
        self.proof.assert_called_once_with(REPO, SHA)
        self.assert_no_pr_fallback(ci_post=True)

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
        # A valid release bump must now name its own ci-post proof after the parent.
        self.assertEqual(stdout, (
            f"release gate green: release bump {SHA}; parent {BUMP_PARENT}; "
            f"PR #17 head {HEAD}; pineforge/verify=success; "
            f"pineforge/parity=success; tree {TREE}; "
            f"ci-post run {self.post_run['id']} for {self.post_run['head_sha']}\n"))

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
                # The full-module mutant also receives the new history seam and proof.
                green = namespace["gate"](REPO, SHA, "refs/heads/main",
                                          api=self.api, git=self.git, history=self.history)
                self.assertEqual(green, (
                    f"release gate green: release bump {SHA}; parent {BUMP_PARENT}; "
                    f"PR #17 head {HEAD}; pineforge/verify=success; "
                    f"pineforge/parity=success; tree {TREE}; "
                    f"ci-post run {self.post_run['id']} for {self.post_run['head_sha']}"))


class RulesetsProbeTests(unittest.TestCase):
    """Exercise the real CLI and reader; only GitHub transport is fake."""

    def api_for(self, rows=None):
        api = FakeAPI()
        api.responses = {REPO_API: json.dumps({"default_branch": "main"})}
        api.rulesets(release_rulesets() if rows is None else rows)
        return api

    def measurement_rows(self):
        rows = release_rulesets()
        rows[0].update(id=13094758, name="default", source_type="Organization",
                       source="example", updated_at="2026-10-07T16:13:00Z")
        rows[1].update(id=21476386, name="PineForge strict CI base",
                       source_type="Repository", source=REPO,
                       updated_at="2026-09-26T02:40:00Z")
        return rows

    def measurement_listing(self, org_visibility, repo_visibility):
        return (
            RULESET_INVENTORY_HEADER
            + "ruleset id=13094758 name='default' source_type='Organization' "
            "source='example' enforcement='active' updated_at='2026-10-07T16:13:00Z' "
            f"bypass_actors={org_visibility} applies=yes pull_request=yes\n"
            "ruleset id=21476386 name='PineForge strict CI base' source_type='Repository' "
            f"source='{REPO}' enforcement='active' updated_at='2026-09-26T02:40:00Z' "
            f"bypass_actors={repo_visibility} applies=yes pull_request=yes\n")

    def run_probe(self, api, *, main=None, repo=REPO):
        stdout, stderr = io.StringIO(), io.StringIO()
        forbidden = mock.Mock(side_effect=AssertionError("probe must not run subprocesses"))
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), \
                mock.patch.object(ci_release_gate.subprocess, "run", forbidden):
            try:
                code = (ci_release_gate.main if main is None else main)(
                    ["rulesets-probe", "--repo", repo], api=api, git=forbidden)
            except SystemExit as exc:
                # An absent subcommand is an observable CLI refusal, not a
                # fixture/import failure, when tracing this test on the base.
                code = exc.code
        forbidden.assert_not_called()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_hidden_org_bypass_actors_refuses_with_the_reader_diagnostic(self):
        rows = self.measurement_rows()
        del rows[0]["bypass_actors"]
        api = self.api_for(rows)
        self.assertEqual(self.run_probe(api), (
            1, self.measurement_listing("hidden", "visible actors=1"),
            f"::error::API repos/{REPO}/rulesets/13094758 returned missing or invalid "
            "bypass_actors (token cannot see this ruleset's bypass_actors)\n"))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None),
            (f"repos/{REPO}/rulesets/21476386", False, None),
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None)])

    def test_two_hidden_rulesets_are_both_listed_in_one_dispatch_as_documented(self):
        rows = self.measurement_rows()
        visible_actors = rows[0].pop("bypass_actors")
        del rows[1]["bypass_actors"]
        first = self.api_for(rows)
        self.assertEqual(self.run_probe(first), (
            1, self.measurement_listing("hidden", "hidden"),
            f"::error::API repos/{REPO}/rulesets/13094758 returned missing or invalid "
            "bypass_actors (token cannot see this ruleset's bypass_actors)\n"))
        self.assertEqual(first.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None),
            (f"repos/{REPO}/rulesets/21476386", False, None),
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None)])

        rows[0]["bypass_actors"] = visible_actors
        second = self.api_for(rows)
        self.assertEqual(self.run_probe(second), (
            1, self.measurement_listing("visible actors=5", "hidden"),
            f"::error::API repos/{REPO}/rulesets/21476386 returned missing or invalid "
            "bypass_actors (token cannot see this ruleset's bypass_actors)\n"))
        self.assertEqual(second.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None),
            (f"repos/{REPO}/rulesets/21476386", False, None),
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None),
            (f"repos/{REPO}/rulesets/21476386", False, None)])

        docs = (ROOT / "docs/ci.md").read_text()
        limit = (
            "One dispatch lists every branch ruleset, including all hidden lists, even when\n"
            "the gate refuses on the first hidden ruleset.")
        message = "docs must explain the complete inventory despite the first refusal"
        self.assertIn(limit, docs, message)
        changed = docs.replace(limit, "", 1)
        self.assertNotEqual(changed, docs)
        with self.assertRaisesRegex(AssertionError, message):
            self.assertIn(limit, changed, message)

    def test_detail_http_error_is_listed_and_other_branch_details_are_still_read(self):
        rows = self.measurement_rows()
        api = self.api_for(rows)
        summaries = ruleset_summaries(rows)
        summaries[0].update(source_type="Organization", source="example",
                            updated_at="2026-10-07T16:13:00Z")
        api.responses[RULESETS_API] = http_response(200, json.dumps(summaries))
        endpoint = f"repos/{REPO}/rulesets/13094758"
        api.responses[endpoint] = http_response(503, "unavailable")
        self.assertEqual(self.run_probe(api), (
            1, RULESET_INVENTORY_HEADER
            + "ruleset id=13094758 name='default' source_type='Organization' "
            "source='example' enforcement='active' updated_at='2026-10-07T16:13:00Z' "
            f"detail=ERROR 'API {endpoint} upstream error 503'\n"
            "ruleset id=21476386 name='PineForge strict CI base' source_type='Repository' "
            f"source='{REPO}' enforcement='active' updated_at='2026-09-26T02:40:00Z' "
            "bypass_actors=visible actors=1 applies=yes pull_request=yes\n",
            f"::error::API {endpoint} upstream error 503\n"))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (endpoint, False, None),
            (f"repos/{REPO}/rulesets/21476386", False, None),
            (REPO_API, False, None), (RULESETS_API, True, None),
            (endpoint, False, None)])

    def test_measurement_visibility_is_only_a_list_not_a_valid_bypass_or_rule(self):
        rows = self.measurement_rows()
        rows[0]["bypass_actors"] = [{}]
        rows[0]["conditions"] = {}
        rows[1]["bypass_actors"] = None
        rows[1]["rules"] = [{"type": "deletion"}]
        api = self.api_for(rows)
        expected = self.measurement_listing("visible actors=1", "hidden")
        expected = expected.replace("applies=yes", "applies=unreadable", 1)
        expected = expected.rsplit("pull_request=yes", 1)[0] + "pull_request=no\n"
        self.assertEqual(self.run_probe(api), (
            1, expected,
            f"::error::API repos/{REPO}/rulesets/13094758 returned invalid bypass actor: {{}}\n"))

    def test_empty_visible_list_is_measured_without_counting_it_as_a_barrier(self):
        rows = self.measurement_rows()
        rows[0]["bypass_actors"] = []
        rows[1]["enforcement"] = "disabled"
        rows[1]["conditions"]["ref_name"]["exclude"] = ["refs/heads/main"]
        api = self.api_for(rows)
        listing = self.measurement_listing("visible actors=0", "visible actors=1")
        listing = listing.replace(
            "enforcement='active' updated_at='2026-09-26T02:40:00Z'",
            "enforcement='disabled' updated_at='2026-09-26T02:40:00Z'")
        listing = listing.rsplit("applies=yes", 1)[0] + "applies=no pull_request=yes\n"
        self.assertEqual(self.run_probe(api), (
            1, listing, f"::error::{RULESET_BARRIER_REFUSAL}\n"))

    def test_later_inventory_pages_and_disabled_branch_rulesets_are_all_listed(self):
        rows = self.measurement_rows()
        del rows[0]["bypass_actors"]
        rows.append(ruleset(19359220, enforcement="disabled", rules=("deletion",),
                            include=["refs/heads/release"]))
        api = self.api_for(rows)
        api.responses[RULESETS_API] = (
            http_response(200, json.dumps(ruleset_summaries(rows[:1])), {
                "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'})
            + http_response(200, json.dumps(ruleset_summaries(rows[1:]))))
        self.assertEqual(self.run_probe(api), (
            1, self.measurement_listing("hidden", "visible actors=1")
            + "ruleset id=19359220 name='fixture ruleset 19359220' source_type='<unknown>' "
            "source='<unknown>' enforcement='disabled' updated_at='<unknown>' "
            "bypass_actors=visible actors=1 applies=no pull_request=no\n",
            f"::error::API repos/{REPO}/rulesets/13094758 returned missing or invalid "
            "bypass_actors (token cannot see this ruleset's bypass_actors)\n"))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None),
            (f"repos/{REPO}/rulesets/21476386", False, None),
            (f"repos/{REPO}/rulesets/19359220", False, None),
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None)])

    def test_inventory_failures_are_reported_before_the_unchanged_reader_refusal(self):
        invalid = {"id": 707, "target": "branch", "enforcement": "unknown"}
        duplicate = ruleset_summaries([ruleset(), ruleset()])
        for endpoint, payload, diagnostic in (
                (REPO_API, json.dumps({"default_branch": "release"}),
                 f"API {REPO_API} must identify main as the default branch"),
                (RULESETS_API, http_response(200, json.dumps([invalid])),
                 f"API {RULESETS_API} returned invalid ruleset summary: {invalid!r}"),
                (RULESETS_API, http_response(200, json.dumps(duplicate)),
                 f"API {RULESETS_API} returned duplicate ruleset id 202"),
                (RULESETS_API, http_response(200, "[]", {
                    "Link": f'<https://api.github.com/{RULESETS_API}&page=2>; rel="next"'}),
                 f"API {RULESETS_API} returned incomplete ruleset pagination: "
                 "unread or unlinked next page")):
            with self.subTest(endpoint=endpoint, diagnostic=diagnostic):
                api = self.api_for()
                api.responses[endpoint] = payload
                self.assertEqual(self.run_probe(api), (
                    1, RULESET_INVENTORY_HEADER + f"inventory=ERROR {diagnostic!r}\n",
                    f"::error::{diagnostic}\n"))
                reads = [(REPO_API, False, None)]
                if endpoint == RULESETS_API:
                    reads.append((RULESETS_API, True, None))
                self.assertEqual(api.calls, reads * 2)

    def test_long_inventory_errors_are_escaped_shortened_and_keep_the_refusal(self):
        detail = "gh: Forbidden (HTTP 403)\n" + "x" * 300
        long_repo = "example/" + "r" * 300
        long_repo_api = f"repos/{long_repo}"
        invalid = {"id": 707, "target": "branch", "enforcement": "unknown" + "x" * 300}
        cases = (
            ("inventory transport", REPO, RULESETS_API, subprocess.CalledProcessError(
                1, ["gh", "api", RULESETS_API], stderr=detail),
             f"API {RULESETS_API} failed with HTTP 403: {detail}"),
            ("repository transport", REPO, REPO_API, subprocess.CalledProcessError(
                1, ["gh", "api", REPO_API], stderr=detail),
             f"API {REPO_API} failed with HTTP 403: {detail}"),
            ("default branch", long_repo, long_repo_api,
             json.dumps({"default_branch": "release"}),
             f"API {long_repo_api} must identify main as the default branch"),
            ("summary validation", REPO, RULESETS_API,
             http_response(200, json.dumps([invalid])),
             f"API {RULESETS_API} returned invalid ruleset summary: {invalid!r}"),
        )
        for scenario, repo, endpoint, payload, diagnostic in cases:
            with self.subTest(scenario=scenario):
                self.assertGreater(len(diagnostic), 240)
                api = self.api_for()
                api.responses[endpoint] = payload
                expected = (
                    1, RULESET_INVENTORY_HEADER + f"inventory=ERROR {diagnostic[:240]!r}\n",
                    f"::error::{diagnostic}\n")
                # RED on 5a866543573c: every row prints the entire diagnostic.
                self.assertEqual(self.run_probe(api, repo=repo), expected)
                reads = [(f"repos/{repo}", False, None)]
                if endpoint == RULESETS_API:
                    reads.append((RULESETS_API, True, None))
                self.assertEqual(api.calls, reads * 2)

                # Revert only the bound in the full module and require this
                # same exact-output assertion to catch every input row.
                source = Path(ci_release_gate.__file__).read_text()
                before = '        lines.append(f"inventory=ERROR {str(exc)[:240]!r}")\n'
                after = '        lines.append(f"inventory=ERROR {str(exc)!r}")\n'
                self.assertEqual(source.count(before), 1,
                                 "mutation must have one source target")
                mutated = source.replace(before, after, 1)
                self.assertNotEqual(mutated, source)
                namespace = {"__name__": "ci_release_inventory_error_bound_mutant"}
                exec(compile(mutated, ci_release_gate.__file__, "exec"), namespace)
                mutant_api = self.api_for()
                mutant_api.responses[endpoint] = payload
                result = self.run_probe(mutant_api, repo=repo, main=namespace["main"])
                self.assertEqual(result, (
                    1, RULESET_INVENTORY_HEADER + f"inventory=ERROR {diagnostic!r}\n",
                    f"::error::{diagnostic}\n"))
                with self.assertRaises(AssertionError):
                    self.assertEqual(result, expected)
                self.assertEqual(mutant_api.calls, reads * 2)

    def test_detail_transport_error_is_escaped_shortened_and_does_not_hide_next_row(self):
        rows = self.measurement_rows()
        api = self.api_for(rows)
        endpoint = f"repos/{REPO}/rulesets/13094758"
        detail = "gh: Forbidden (HTTP 403)\n::notice::" + "x" * 300
        api.responses[endpoint] = subprocess.CalledProcessError(
            1, ["gh", "api", endpoint], stderr=detail)
        diagnostic = f"API {endpoint} failed with HTTP 403: {detail}"
        repo_line = self.measurement_listing("hidden", "visible actors=1").splitlines()[2]
        self.assertEqual(self.run_probe(api), (
            1, RULESET_INVENTORY_HEADER
            + "ruleset id=13094758 name='default' source_type='<unknown>' "
            "source='<unknown>' enforcement='active' updated_at='<unknown>' "
            f"detail=ERROR {diagnostic[:240]!r}\n{repo_line}\n",
            f"::error::{diagnostic}\n"))
        self.assertIn((f"repos/{REPO}/rulesets/21476386", False, None), api.calls)

    def test_api_display_metadata_is_escaped_in_each_inventory_field(self):
        rows = self.measurement_rows()
        rows[0].update(name="org\n::notice::", source_type="Organization\rtest",
                       source="owner\nname", updated_at="time\tstamp\nnext")
        del rows[0]["bypass_actors"]
        listing = self.measurement_listing("hidden", "visible actors=1")
        listing = listing.replace("name='default'", "name='org\\n::notice::'")
        listing = listing.replace("source_type='Organization'", "source_type='Organization\\rtest'")
        listing = listing.replace("source='example'", "source='owner\\nname'")
        listing = listing.replace(
            "updated_at='2026-10-07T16:13:00Z'", "updated_at='time\\tstamp\\nnext'")
        self.assertEqual(self.run_probe(self.api_for(rows)), (
            1, listing,
            f"::error::API repos/{REPO}/rulesets/13094758 returned missing or invalid "
            "bypass_actors (token cannot see this ruleset's bypass_actors)\n"))

    def test_probe_dispatch_branch_and_fleets_are_documented(self):
        docs = (ROOT / "docs/ci.md").read_text()
        requirements = (
            "FLEET-POST=A keeps large verification jobs split at the file boundary:\n"
            'PR build/preflight and parity subset use `["self-hosted","pf-gcp-pr"]`; post-merge\n'
            'verification uses `["self-hosted","pf-gcp-post"]`. No job routes by event.',
            "The manual `release-ruleset-probe.yml` workflow runs `authz` on\n"
            'hosted `ubuntu-24.04` and `probe` on `["self-hosted","pf-gcp-post"]`.',
            "Release and promotion use the isolated release fleet:\n"
            "`release`, `notify-hub`, Linux x64 `prebuilt` and baseline `promote` use\n"
            '`["self-hosted","pf-gcp-main"]`.\n'
            'The release ruleset probe runs secret-free `authz` on hosted `ubuntu-24.04` and\n'
            '`probe` on `["self-hosted","pf-gcp-post"]`.',
            "Dispatch **Release ruleset probe** (`release-ruleset-probe.yml`) once from\n"
            "`main` to check actual ruleset visibility with the same App token mint as\n"
            "`release.yml`. The workflow file must be on the default branch, and the run\n"
            "must use `main`'s copy.\n"
            'The credentialed probe job runs on `["self-hosted","pf-gcp-post"]`, with\n'
            'secret-free `authz` on hosted `ubuntu-24.04`.',
            "`pf-gcp-main` admits only allowlisted workflows (an org runner-group restriction),\n"
            "so the one-time probe uses `pf-gcp-post`.",
            "The dispatch must run from `main`; the workflow itself refuses any other ref\n"
            "before minting the App token or checking out repository code.",
        )
        message = "docs must state the probe's main dispatch and actual fleet routes"
        for required in requirements:
            with self.subTest(required=required):
                self.assertIn(required, docs, message)
                changed = docs.replace(required, "", 1)
                self.assertNotEqual(changed, docs)
                with self.assertRaisesRegex(AssertionError, message):
                    self.assertIn(required, changed, message)

    def test_empty_inventory_refusal_and_documented_endpoint_scope(self):
        # A complete [] inventory has no barrier; its refusal has no endpoint.
        api = self.api_for([])
        self.assertEqual(self.run_probe(api), (
            1, RULESET_INVENTORY_HEADER, f"::error::{RULESET_BARRIER_REFUSAL}\n"))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (REPO_API, False, None), (RULESETS_API, True, None)])
        docs = (ROOT / "docs/ci.md").read_text()
        scoped = ("the reader's unchanged `::error::` diagnostic on stderr; "
                  "API/response failures name the endpoint.")
        unscoped = ("the reader's unchanged `::error::` diagnostic on stderr "
                    "naming the failing endpoint.")

        def assert_scope(text):
            self.assertIn(scoped, text, "only API/response failures promise an endpoint")
            self.assertNotIn(unscoped, text, "all-refusals endpoint claim must stay absent")

        assert_scope(docs)
        self.assertEqual(docs.count(scoped), 1)
        changed = docs.replace(scoped, unscoped, 1)
        self.assertNotEqual(changed, docs)
        with self.assertRaisesRegex(AssertionError,
                                    "only API/response failures promise an endpoint"):
            assert_scope(changed)

    def test_visible_barrier_prints_exact_evidence_and_only_reads_rulesets(self):
        rows = release_rulesets()
        rows[0].update(source_type="Organization", source="example")
        rows[1].update(source_type="Repository", source=REPO)
        rows.append(ruleset(404, target="push"))
        api = self.api_for(rows)
        self.assertEqual(self.run_probe(api), (0, (
            RULESET_INVENTORY_HEADER
            + "ruleset id=101 name='fixture ruleset 101' source_type='Organization' "
            "source='example' enforcement='active' updated_at='<unknown>' "
            "bypass_actors=visible actors=5 applies=yes pull_request=yes\n"
            "ruleset id=202 name='fixture ruleset 202' source_type='Repository' "
            f"source='{REPO}' enforcement='active' updated_at='<unknown>' "
            "bypass_actors=visible actors=1 applies=yes pull_request=yes\n"
            "release ruleset barrier ok: ruleset 202 'fixture ruleset 202'\n"
            "branch rulesets: 101 'fixture ruleset 101' active "
            "source='Organization':'example' bypass_actors=visible; "
            "202 'fixture ruleset 202' active "
            f"source='Repository':'{REPO}' bypass_actors=visible\n"), ""))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (DEFAULT_RULESET_API, False, None), (STRICT_RULESET_API, False, None)] * 2)

    def test_barrier_identity_and_inventory_are_not_fixture_constants(self):
        rows = [ruleset(907, enforcement="evaluate"), ruleset(809)]
        rows[0].update(name="other\nprotection", source_type="Organization", source="owner")
        rows[1].update(name="release barrier", source_type="Repository", source="owner/other")
        api = self.api_for(rows)
        repo = "owner/other"
        api.responses = {key.replace(REPO, repo): value for key, value in api.responses.items()}
        self.assertEqual(self.run_probe(api, repo=repo), (0, (
            RULESET_INVENTORY_HEADER
            + "ruleset id=907 name='other\\nprotection' source_type='Organization' "
            "source='owner' enforcement='evaluate' updated_at='<unknown>' "
            "bypass_actors=visible actors=1 applies=yes pull_request=yes\n"
            "ruleset id=809 name='release barrier' source_type='Repository' "
            "source='owner/other' enforcement='active' updated_at='<unknown>' "
            "bypass_actors=visible actors=1 applies=yes pull_request=yes\n"
            "release ruleset barrier ok: ruleset 809 'release barrier'\n"
            "branch rulesets: 907 'other\\nprotection' evaluate "
            "source='Organization':'owner' bypass_actors=visible; "
            "809 'release barrier' active "
            "source='Repository':'owner/other' bypass_actors=visible\n"), ""))
        self.assertEqual(api.calls, [
            (f"repos/{repo}", False, None),
            (f"repos/{repo}/rulesets?includes_parents=true&per_page=100", True, None),
            (f"repos/{repo}/rulesets/907", False, None),
            (f"repos/{repo}/rulesets/809", False, None)] * 2)

    def test_missing_display_metadata_does_not_change_the_gate_verdict(self):
        rows = [ruleset()]
        api = self.api_for(rows)
        summary = ruleset_summaries(rows)
        del summary[0]["name"]
        api.responses[RULESETS_API] = http_response(200, json.dumps(summary))
        detail = {key: value for key, value in rows[0].items() if key != "name"}
        api.responses[STRICT_RULESET_API] = json.dumps(detail)
        self.assertEqual(self.run_probe(api), (0, (
            RULESET_INVENTORY_HEADER
            + "ruleset id=202 name='<unknown>' source_type='<unknown>' "
            "source='<unknown>' enforcement='active' updated_at='<unknown>' "
            "bypass_actors=visible actors=1 applies=yes pull_request=yes\n"
            "release ruleset barrier ok: ruleset 202 '<unknown>'\n"
            "branch rulesets: 202 '<unknown>' active "
            "source='<unknown>':'<unknown>' bypass_actors=visible\n"), ""))

    def test_probe_matches_reader_for_every_existing_ruleset_fixture_row(self):
        # Replay every fixture/subtest from the existing class, including the
        # original full-source-mutant rows, without rewriting their inputs or
        # weakening their gate assertions. Each original gate call also runs
        # an independent real reader and the probe over identical responses.
        for name in unittest.defaultTestLoader.getTestCaseNames(ReleaseRulesetTests):
            with self.subTest(fixture_test=name):
                case = ReleaseRulesetTests(name)
                original = case.run_gate
                checked = []

                def compare_then_gate(**kwargs):
                    direct, probe = FakeAPI(), FakeAPI()
                    direct.responses = dict(case.api.responses)
                    probe.responses = dict(case.api.responses)
                    try:
                        ci_release_gate._require_release_rulesets(direct, REPO)
                    except ci_release_gate.ReleaseGateError as exc:
                        expected = (1, f"::error::{exc}\n")
                    else:
                        expected = None
                    result = self.run_probe(probe)
                    self.assertTrue(result[1].startswith(RULESET_INVENTORY_HEADER), result)
                    if expected is None:
                        self.assertEqual(result[0], 0, result)
                        self.assertEqual(result[2], "")
                        success_lines = result[1].splitlines()[-2:]
                        self.assertTrue(success_lines[0].startswith(
                            "release ruleset barrier ok: ruleset "), result)
                        self.assertTrue(success_lines[1].startswith("branch rulesets: "), result)
                    else:
                        self.assertEqual((result[0], result[2]), expected)
                        self.assertNotIn("release ruleset barrier ok:", result[1])
                    self.assertEqual(probe.calls[-len(direct.calls):], direct.calls)
                    checked.append(result[0])
                    return original(**kwargs)

                case.run_gate = compare_then_gate
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(result.errors + result.failures, [], name)
                self.assertTrue(checked, f"{name} exercised no ruleset fixture")

    def test_full_source_mutant_skipping_reader_admits_the_hidden_org_fixture(self):
        rows = release_rulesets()
        rows[0].update(id=13094758, name="default", source_type="Organization",
                       source="example")
        del rows[0]["bypass_actors"]
        listing = (
            RULESET_INVENTORY_HEADER
            + "ruleset id=13094758 name='default' source_type='Organization' "
            "source='example' enforcement='active' updated_at='<unknown>' "
            "bypass_actors=hidden applies=yes pull_request=yes\n"
            "ruleset id=202 name='fixture ruleset 202' source_type='<unknown>' "
            "source='<unknown>' enforcement='active' updated_at='<unknown>' "
            "bypass_actors=visible actors=1 applies=yes pull_request=yes\n")
        refusal = (1, listing, f"::error::API repos/{REPO}/rulesets/13094758 returned missing or "
                   "invalid bypass_actors (token cannot see this ruleset's bypass_actors)\n")
        self.assertEqual(self.run_probe(self.api_for(rows)), refusal)
        source = Path(ci_release_gate.__file__).read_text()
        before = "    barrier, branches = _require_release_rulesets(api, repo)\n"
        self.assertEqual(source.count(before), 1, "mutation must have one source target")
        mutated = source.replace(
            before, '    barrier, branches = {"id": 0, "name": "unverified"}, []\n', 1)
        self.assertNotEqual(mutated, source)
        namespace = {"__name__": "ci_release_rulesets_probe_mutant"}
        exec(compile(mutated, ci_release_gate.__file__, "exec"), namespace)
        api = self.api_for(rows)
        self.assertEqual(self.run_probe(api, main=namespace["main"]), (
            0, listing + "release ruleset barrier ok: ruleset 0 'unverified'\nbranch rulesets: \n",
            ""))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None),
            (STRICT_RULESET_API, False, None)],
            "the mutant must bypass the verdict reader, while retaining measurement")

    def test_full_source_mutant_dropping_listing_loses_only_the_measurement(self):
        rows = self.measurement_rows()
        del rows[0]["bypass_actors"]
        stderr = (f"::error::API repos/{REPO}/rulesets/13094758 returned missing or invalid "
                  "bypass_actors (token cannot see this ruleset's bypass_actors)\n")
        self.assertEqual(self.run_probe(self.api_for(rows)), (
            1, self.measurement_listing("hidden", "visible actors=1"), stderr))
        source = Path(ci_release_gate.__file__).read_text()
        before = "    print(_ruleset_inventory_report(api, repo), flush=True)\n"
        self.assertEqual(source.count(before), 1, "mutation must have one source target")
        mutated = source.replace(before, "", 1)
        self.assertNotEqual(mutated, source)
        namespace = {"__name__": "ci_release_rulesets_listing_mutant"}
        exec(compile(mutated, ci_release_gate.__file__, "exec"), namespace)
        api = self.api_for(rows)
        self.assertEqual(self.run_probe(api, main=namespace["main"]), (1, "", stderr))
        self.assertEqual(api.calls, [
            (REPO_API, False, None), (RULESETS_API, True, None),
            (f"repos/{REPO}/rulesets/13094758", False, None)])


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
        modes = [ast.literal_eval(node.args[0])
                 for node in ast.walk(main)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                 and node.func.attr == "add_parser"]
        self.assertEqual(modes, ["gate", "rulesets-probe"])
        flags = {}
        for node in ast.walk(main):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "add_argument"):
                flags.setdefault(node.func.value.id, []).extend(
                    ast.literal_eval(argument) for argument in node.args)
        self.assertEqual(flags, {"check": ["--repo", "--sha", "--ref"], "probe": ["--repo"]},
                         "neither subcommand may offer a cut-over override flag")

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

    def test_ci_post_required_build_jobs_match_workflow_matrices(self):
        """Tie the implementation's new name tuple and fixtures to both real matrices."""
        jobs = _jobs((ROOT / ".github/workflows/ci-post.yml").read_text())
        self.assertNotIn("build-macos", jobs, "post-merge verification must be Linux-only")
        expanded = []
        for job_id, axis in (("build", "platform.os"), ("build-arm", None)):
            with self.subTest(job=job_id):
                self.assertIn(job_id, jobs, "both Linux architectures must have a build job")
                body = _without_comments(jobs[job_id])
                matrix = re.search(
                    r"(?ms)^      matrix:\n(.*?)(?=^ {0,6}\S|\Z)", body)
                self.assertIsNotNone(matrix, "the build matrix must be readable")
                axes = re.findall(r"(?m)^        (\w+):", matrix[1])
                expected_axes = [axis.split(".")[0], "build_type"] if axis else ["build_type"]
                self.assertCountEqual(axes, expected_axes,
                                      "new axes/include/exclude need a gate review")
                types = re.search(r"(?m)^        build_type: \[([^\n]+)\]$", matrix[1])
                self.assertIsNotNone(types, "the build types must be explicit")
                build_types = [value.strip() for value in types[1].split(",")]
                if axis:
                    platforms = re.findall(r"(?m)^          - os: (\S+)$", matrix[1])
                else:
                    runner = re.search(
                        r"(?m)^    runs-on: (ubuntu-[0-9]{2}\.[0-9]{2}-arm)$", body)
                    self.assertIsNotNone(runner, "arm64 must use an explicit hosted Linux runner")
                    platforms = [runner[1]]
                self.assertTrue(platforms)
                name = re.search(r"(?m)^    name: (.+)$", body)
                self.assertIsNotNone(name, "the displayed job name must be explicit")
                for platform in platforms:
                    for build_type in build_types:
                        displayed = name[1]
                        if axis:
                            displayed = displayed.replace("${{ matrix." + axis + " }}", platform)
                        displayed = displayed.replace("${{ matrix.build_type }}", build_type)
                        self.assertNotIn("${{", displayed, "every name axis must be expanded")
                        self.assertEqual(displayed, f"build ({platform}, {build_type})",
                                         "the displayed platform must match its runner")
                        expanded.append(displayed)
        self.assertCountEqual(ci_release_gate.CI_POST_BUILD_JOBS, expanded)
        self.assertCountEqual([job["name"] for job in ci_post_jobs()], expanded)

    def test_prebuilt_matrix_and_release_notes_publish_only_linux(self):
        workflow = self.workflow()
        jobs = _jobs(workflow)
        prebuilt = jobs["prebuilt"]
        self.assertEqual(
            re.findall(r"(?m)^          - triple: (\S+)$", prebuilt),
            ["linux-x86_64", "linux-aarch64"])
        self.assertNotRegex(prebuilt, r"(?i)macos|darwin|CMAKE_OSX_ARCHITECTURES|\bbrew\b")
        self.assertNotIn("cmake_extra", prebuilt)
        self.assertNotIn("CMAKE_EXTRA", prebuilt)
        pack = next(step for step in _steps(prebuilt)
                    if step.startswith("      - name: Pack tarball + sha256\n"))
        self.assertIn('          sha256sum "${tarball}" > "${tarball}.sha256"\n', pack)
        self.assertNotIn("command -v sha256sum", pack)
        self.assertNotIn("shasum -a", pack)
        notes = next(step for step in _steps(jobs["release"])
                     if step.startswith("      - name: Generate release notes\n"))
        self.assertIn(
            '            echo "Linux amd64 and arm64 static-lib + headers tarballs '
            'are attached below (C ABI)."\n', notes)
        self.assertNotRegex(notes, r"(?i)macos|darwin")

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

    def assert_ruleset_visibility_documentation(self, source, docs, workflow):
        def constant_string(node):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                return node.value
            if isinstance(node, ast.Constant) and isinstance(node.value, bytes):
                return node.value.decode("utf-8", errors="replace")
            if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
                left = constant_string(node.left)
                right = constant_string(node.right)
                if left is not None and right is not None:
                    return left + right
            return None

        texts = [source, docs, workflow]
        # ast joins adjacent literals; walking also visits f-string literal parts.
        for node in ast.walk(ast.parse(source)):
            value = constant_string(node)
            if value is not None:
                texts.append(value)
        for text in texts:
            views = (text.replace("`", ""),
                     re.sub(r"""(?:\\\r?\n|#|//|['"`]|\s)+""", " ", text))
            # Compatibility normalization must only add matches to the original views.
            casefold_views = [view.casefold() for view in views]
            normalized_views = [unicodedata.normalize("NFKC", view).casefold()
                                for view in views]
            for view in casefold_views + normalized_views:
                self.assertNotRegex(view,
                                    r"\badministration\s*:\s*read\b|\bapp\s+requires\b",
                                    "unverified App permission claim must stay absent")
        self.assertIn(
            "# The gate needs bypass_actors visible on every branch ruleset in the inventory.\n"
            "# If the release App token cannot see them, the bump is refused;\n"
            "# release-ruleset-probe.yml reports whether it can.", source,
            "source must document the ruleset visibility requirement")
        self.assertIn(
            "The gate needs `bypass_actors` visible on every branch ruleset in the inventory.\n"
            "If the release App token cannot see them, the bump is refused.\n"
            "`release-ruleset-probe.yml` reports whether that token can see them.", docs,
            "docs must document the ruleset visibility requirement")
        self.assertIn(
            "      # The gate needs bypass_actors visible on every branch ruleset in the inventory.\n"
            "      # If the release App token cannot see them, the bump is refused.\n"
            "      # release-ruleset-probe.yml reports whether it can. "
            "contents: write scopes GITHUB_TOKEN.", workflow,
            "workflow must document the ruleset visibility requirement")
        self.assertNotIn("Removing this App permission makes every release-bump exemption refuse.",
                         docs, "the unverified permission dependency must stay absent")

    def test_ruleset_reader_visibility_requirement_is_documented_and_fixed_in_code(self):
        source = Path(ci_release_gate.__file__).read_text()
        docs = (ROOT / "docs/ci.md").read_text()
        self.assertRegex(source, r"(?m)^RELEASE_APP_ID = 4105147$")
        self.assert_ruleset_visibility_documentation(source, docs, self.workflow())

    def test_ruleset_visibility_prose_rejects_restored_permission_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        permission_message = "unverified App permission claim must stay absent"
        for index, claim, message in (
                (0, "\n# The gate relies on App permission administration: read to see bypass_actors;\n",
                 permission_message),
                (1, "\nThe gate relies on the release App permission `administration: read` to read\n"
                 "`bypass_actors`.\n"
                 "Removing this App permission makes every release-bump exemption refuse.\n",
                 permission_message),
                (2, "\n# Its App permission administration: read exposes bypass_actors; removing\n"
                 "# that permission makes the release-bump exemption refuse every release.\n",
                 permission_message),
                (1, "\nRemoving this App permission makes every release-bump exemption refuse.\n",
                 "the unverified permission dependency must stay absent")):
            with self.subTest(place=index, claim=claim):
                changed = original.copy()
                changed[index] += claim
                self.assertNotEqual(changed[index], original[index])
                with self.assertRaisesRegex(AssertionError, message):
                    self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_restored_refusal_and_spelling_variants(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        neutral = "(token cannot see this ruleset's bypass_actors)"
        self.assertEqual(original[0].count(neutral), 1,
                         "restoration must target the runtime refusal string")
        for claim in ("App requires administration: read", "administration: read",
                      "`administration: read`", "administration:read",
                      "`administration:read`", "`administration`: `read`",
                      "ADMINISTRATION : READ", "App requires"):
            with self.subTest(claim=claim):
                changed = original.copy()
                changed[0] = changed[0].replace(neutral, f"({claim})", 1)
                self.assertNotEqual(changed[0], original[0])
                with self.assertRaisesRegex(
                        AssertionError, "unverified App permission claim must stay absent"):
                    self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_concatenated_and_fstring_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        for expression in (
                '("App " "requires administration:" " read")',
                '"App " + ("requires administr" + "ation:") + " read"',
                '("administr" + ("ation:" + " re")) + "ad"',
                'f"App requi\\x72es administration: re\\x61d {RELEASE_APP_ID}"',
                '"administr\\x61tion: re\\x61d"'):
            with self.subTest(expression=expression):
                changed = original.copy()
                changed[0] += f"\nPERMISSION_CLAIM = {expression}\n"
                self.assertNotEqual(changed[0], original[0])
                ast.parse(changed[0])
                with self.assertRaisesRegex(
                        AssertionError, "unverified App permission claim must stay absent"):
                    self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_wrapped_and_quoted_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        for index in range(len(original)):
            for claim in (
                    "\n# Needs administration:\n# read\n",
                    "\n# App\n# requires\n",
                    '\n# Needs "administration":"read".\n',
                    "\n# // 'App'\n# // 'requires'\n"):
                with self.subTest(place=index, claim=claim):
                    changed = original.copy()
                    changed[index] += claim
                    self.assertNotEqual(changed[index], original[index])
                    with self.assertRaisesRegex(
                            AssertionError, "unverified App permission claim must stay absent"):
                        self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_backslash_wrapped_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        for index, claim in (
                (1, "\nNeeds administration:\\\nread\n"),
                (2, "\n# Needs administration:\\\n# read\n")):
            with self.subTest(place=index, claim=claim):
                changed = original.copy()
                changed[index] += claim
                self.assertNotEqual(changed[index], original[index])
                with self.assertRaisesRegex(
                        AssertionError, "unverified App permission claim must stay absent"):
                    self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_decoded_bytes_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        for expression in (
                'b"administr\\x61tion: re\\x61d"',
                'b"administr\\x61tion: re\\x61d".decode()',
                'b"administr\\x61tion:" + b" re\\x61d"',
                'b"\\xffadministr\\x61tion: re\\x61d".decode("utf-8", errors="replace")'):
            with self.subTest(expression=expression):
                changed = original.copy()
                changed[0] += f"\nPERMISSION_CLAIM = {expression}\n"
                self.assertNotEqual(changed[0], original[0])
                ast.parse(changed[0])
                with self.assertRaisesRegex(
                        AssertionError, "unverified App permission claim must stay absent"):
                    self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_fullwidth_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        for index in range(len(original)):
            for claim in ("\n# Ａｐｐ requires\n", "\n# Ａｐｐ\n# requires\n"):
                with self.subTest(place=index, claim=claim):
                    changed = original.copy()
                    changed[index] += claim
                    self.assertNotEqual(changed[index], original[index])
                    with self.assertRaisesRegex(
                            AssertionError, "unverified App permission claim must stay absent"):
                        self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_rejects_compatibility_suffix_claims(self):
        original = [Path(ci_release_gate.__file__).read_text(),
                    (ROOT / "docs/ci.md").read_text(), self.workflow()]
        self.assert_ruleset_visibility_documentation(*original)
        for index, claim in (
                (0, "\n# Needs administration: read\u2122\n"),
                (1, "\nNeeds administration: read\u2122\n"),
                (2, "\n# Needs administration: read\u2122\n"),
                (0, "\n# Needs administration:\n# read\u2122\n"),
                (1, "\nNeeds administration:\n# read\u2122\n"),
                (2, "\n# Needs administration:\n# read\u2122\n")):
            with self.subTest(place=index, claim=claim):
                changed = original.copy()
                changed[index] += claim
                self.assertNotEqual(changed[index], original[index])
                with self.assertRaisesRegex(
                        AssertionError, "unverified App permission claim must stay absent"):
                    self.assert_ruleset_visibility_documentation(*changed)

    def test_ruleset_visibility_guard_casefold_scan_removal_breaks_restoration_controls(self):
        regression = "test_ruleset_visibility_guard_rejects_compatibility_suffix_claims"
        original_result = unittest.TestResult()
        WorkflowPinTests(regression).run(original_result)
        self.assertEqual(original_result.testsRun, 1)
        self.assertEqual(original_result.errors, [])
        self.assertEqual(original_result.failures, [],
                         "restoration controls must pass before mutation")

        source = Path(__file__).read_text()
        before = "            for view in casefold_views + normalized_views:\n"
        self.assertEqual(source.count(before), 1, "mutation must have one source target")
        mutated = source.replace(before, "            for view in normalized_views:\n", 1)
        self.assertNotEqual(mutated, source)
        guard = next(node for node in ast.walk(ast.parse(mutated))
                     if isinstance(node, ast.FunctionDef)
                     and node.name == "assert_ruleset_visibility_documentation")
        namespace = {"ast": ast, "re": re, "unicodedata": unicodedata}
        exec(compile(ast.Module(body=[guard], type_ignores=[]), __file__, "exec"), namespace)
        case = WorkflowPinTests(regression)
        case.assert_ruleset_visibility_documentation = namespace[
            "assert_ruleset_visibility_documentation"].__get__(case)
        result = unittest.TestResult()
        case.run(result)
        self.assertEqual(result.testsRun, 1)
        self.assertEqual(result.errors, [])
        self.assertEqual(len(result.failures), 6,
                         "removing the casefold-only scan must break every restoration row")
        for _, traceback in result.failures:
            self.assertIn("AssertionError not raised", traceback)

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
        checkout = next(step for step in steps if "uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0" in step)
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


class GitHistorySeamTests(unittest.TestCase):
    """Exercise production git history in isolated repositories, without a fake."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tmp = Path(directory.name)
        self.path = self.tmp / "repo"
        self.path.mkdir(parents=True, exist_ok=True)
        self.git_env = {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_NAME": "ci", "GIT_AUTHOR_EMAIL": "ci@example.invalid",
            "GIT_COMMITTER_NAME": "ci", "GIT_COMMITTER_EMAIL": "ci@example.invalid",
            "GIT_AUTHOR_DATE": "2001-01-01T00:00:00Z",
            "GIT_COMMITTER_DATE": "2001-01-01T00:00:00Z",
            "GIT_CEILING_DIRECTORIES": str(self.tmp),
        }
        # Isolate git_history's subprocesses as well as the fixture's git calls.
        isolated = {key: value for key, value in os.environ.items()
                    if not key.startswith("GIT_")}
        environment = mock.patch.dict(
            os.environ, {**isolated, **self.git_env}, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.git("init", "--quiet", "--initial-branch=main")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "core.hooksPath", os.devnull)
        self.git("config", "core.precomposeunicode", "true")
        # Cleanups run in reverse order: leave the repo before deleting it.
        self.addCleanup(os.chdir, Path.cwd())
        os.chdir(self.path)
        self.root = self.commit("root", {
            "docs/history.md": "root documentation\n",
            "src/history.cpp": "int history = 0;\n",
        })

    def git(self, *args):
        return subprocess.run(
            ["git", "-C", str(self.path), *args], check=True,
            capture_output=True, text=True, env={**os.environ, **self.git_env}).stdout

    def commit(self, message, write=None):
        for path, text in (write or {}).items():
            target = self.path / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
        self.git("add", "--all")
        self.git("commit", "--quiet", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD").strip()

    def test_root_commit_returns_no_parent_or_paths(self):
        """R-B: prove root handling; block inventing a parent or diff for a root."""
        self.assertEqual(ci_release_gate.git_history(self.root), (None, []))

    def test_normal_commit_returns_parent_and_both_changed_paths(self):
        """R-B: prove the complete diff; block dropping a docs or source change."""
        changed = {
            "docs/history.md": "updated documentation\n",
            "src/history.cpp": "int history = 1;\n",
        }
        sha = self.commit("documentation and source", changed)
        parent, paths = ci_release_gate.git_history(sha)
        self.assertEqual(parent, self.root)
        self.assertCountEqual(paths, changed)

    def test_rename_returns_both_old_source_and_new_docs_paths(self):
        """R-B: prove both rename sides; block hiding source moved into docs."""
        old, new = "src/history.cpp", "docs/history.cpp"
        self.git("mv", old, new)
        sha = self.commit("move source into docs")
        parent, paths = ci_release_gate.git_history(sha)
        self.assertEqual(parent, self.root)
        self.assertCountEqual(paths, [old, new])

    def test_newline_and_non_ascii_paths_are_preserved_byte_for_byte(self):
        """R-B: prove NUL framing; block split-newline or quoted Unicode paths."""
        for path in ("docs/line\nbreak.md", "docs/café.md"):
            with self.subTest(path=path):
                expected_parent = self.git("rev-parse", "HEAD").strip()
                target = self.path / path
                target.parent.mkdir(parents=True, exist_ok=True)
                try:
                    target.write_text("unusual path\n", encoding="utf-8")
                except OSError as exc:
                    if "\n" in path and exc.errno == errno.EINVAL:
                        self.skipTest(
                            f"filesystem rejects a newline filename with EINVAL: {exc}")
                    raise
                sha = self.commit(f"add {path!r}")
                parent, paths = ci_release_gate.git_history(sha)
                self.assertEqual(parent, expected_parent)
                self.assertEqual([name.encode("utf-8") for name in paths],
                                 [path.encode("utf-8")])

    def test_merge_returns_first_parent_and_only_its_diff(self):
        """R-B: prove first-parent merge diff; block second-parent or combined diffs."""
        self.git("checkout", "--quiet", "-b", "topic")
        topic_paths = {"docs/topic.md": "topic documentation\n"}
        topic = self.commit("topic", topic_paths)
        self.git("checkout", "--quiet", "main")
        first_parent = self.commit("main advanced", {
            "src/main_only.cpp": "int main_only = 1;\n",
        })
        self.git("merge", "--quiet", "--no-ff", "--no-edit", "topic")
        sha = self.git("rev-parse", "HEAD").strip()
        self.assertEqual(self.git("rev-parse", f"{sha}^1").strip(), first_parent)
        self.assertEqual(self.git("rev-parse", f"{sha}^2").strip(), topic)
        parent, paths = ci_release_gate.git_history(sha)
        self.assertEqual(parent, first_parent)
        self.assertCountEqual(paths, topic_paths)

    def test_unknown_sha_raises_oserror(self):
        """R-B: prove unknown commits fail; block swallowing git errors as empty diffs."""
        unknown = "f" * 40
        with self.assertRaises(subprocess.CalledProcessError):
            self.git("cat-file", "-e", unknown)
        with self.assertRaises(OSError):
            ci_release_gate.git_history(unknown)

    def test_empty_commit_returns_parent_and_no_paths(self):
        """R-B: prove an empty commit keeps its parent; block treating it as a root."""
        sha = self.commit("empty")
        self.assertNotEqual(sha, self.root)
        self.assertEqual(ci_release_gate.git_history(sha), (self.root, []))

    def test_directory_without_git_raises_oserror(self):
        """R-B: prove absent git metadata fails; block an empty or root fallback."""
        outside = self.tmp / "without-git"
        outside.mkdir()
        self.assertFalse((outside / ".git").exists())
        os.chdir(outside)
        with self.assertRaises(OSError):
            ci_release_gate.git_history(self.root)


class CiPostGateTests(unittest.TestCase):
    """Real gate/CLI decisions over opt-in GitHub responses and first-parent diffs."""

    enable_watcher = GateTests.enable_watcher

    def setUp(self):
        GateTests.setUp(self)
        self.reset_ci_post()

    def reset_ci_post(self):
        self.api = FakeAPI()
        self.post_run = ci_post_run(8341)
        self.jobs = ci_post_jobs()
        self.api.ci_post(self.post_run, self.jobs)
        self.history = FakeHistory({
            self.post_run["head_sha"]: (OTHER_HEAD, ["src/ci_post_fixture.cpp"]),
        })

    def run_gate(self, *, repo=REPO, sha=SHA, ref="refs/heads/main"):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = ci_release_gate.main(
                ["gate", "--repo", repo, "--sha", sha, "--ref", ref],
                api=self.api, git=self.git, history=self.history)
        return code, stdout.getvalue(), stderr.getvalue()

    def post_calls(self):
        return [call for call in self.api.calls
                if "/actions/workflows/ci-post.yml/runs?" in call[0]
                or ("/actions/runs/" in call[0] and "/jobs?" in call[0])]

    def assert_post_reads(self, row, *, repo=REPO, newer=(), docs=()):
        # Each examined completed run needs its own jobs read, newest first.
        # Skipped docs commits need runs only, tip first, before any proof reads.
        self.assertEqual(self.post_calls(), [
            *[(ci_post_runs_api(sha, repo=repo), True, None) for sha in docs],
            (ci_post_runs_api(row["head_sha"], repo=repo), True, None),
            *[(ci_post_jobs_api(examined["id"], repo=repo), False, None)
              for examined in (*newer, row)],
        ], "docs runs, then proof runs and exactly the examined proof jobs, in order")

    def assert_refused(self, reason, *evidence, **kwargs):
        code, stdout, stderr = self.run_gate(**kwargs)
        self.assertIn(f"ci-post: {reason}", stderr, (stdout, stderr))
        self.assertEqual(code, 1, (stdout, stderr))
        self.assertIn("::error::", stderr)
        for fragment in evidence:
            self.assertIn(str(fragment), stderr)
        self.assertEqual(stdout, "", "a refusal must never print a green line")
        return stderr

    def assert_no_full_run(self, rows, *evidence, docs=()):
        stderr = self.assert_refused(
            "no-full-run", rows[-1]["id"], rows[-1]["head_sha"],
            f"examined {len(rows)}", *evidence)
        # Exhausting non-proving completed runs requires reading all their jobs.
        self.assert_post_reads(rows[-1], newer=rows[:-1], docs=docs)
        return stderr

    def assert_green_evidence(self, text, row, *, release_sha=SHA, skipped=(),
                              newer_skipped=()):
        self.assertTrue(text.startswith("release gate green:"), text)
        self.assertIn("ci-post", text)
        self.assertRegex(text, rf"\b{row['id']}\b")
        self.assertIn(row["head_sha"], text)
        if skipped:
            self.assertIn(release_sha, text)
            self.assertIn("docs-only", text)
            self.assertRegex(text, rf"{len(skipped)} docs-only commit")
        if newer_skipped:
            self.assertRegex(text, rf"\b{len(newer_skipped)} newer\b[^\n]*\bskipped\b")

    def assert_green(self, row, *, sha=SHA, repo=REPO, skipped=(), newer_skipped=()):
        code, stdout, stderr = self.run_gate(sha=sha, repo=repo)
        self.assertEqual(code, 0, (stdout, stderr))
        self.assertEqual(stderr, "")
        # Skipped-only newer runs are read before the first proving run's jobs.
        self.assert_post_reads(row, repo=repo, newer=newer_skipped, docs=skipped)
        self.assert_green_evidence(
            stdout, row, release_sha=sha, skipped=skipped, newer_skipped=newer_skipped)

    def built_ancestor(self, sha=OTHER_HEAD):
        row = ci_post_run(self.post_run["id"] + 37, sha=sha)
        self.api.ci_post(row, ci_post_jobs())
        return row

    def docs_chain(self, count):
        docs = [SHA] + [f"{index:040x}" for index in range(1, count)]
        ancestor = self.built_ancestor()
        self.docs_runs = [
            ci_post_run(self.post_run["id"] + index + 1, sha=sha)
            for index, sha in enumerate(docs)
        ]
        for row in self.docs_runs:
            self.api.ci_post_runs(row["head_sha"], [row], {})
        commits = [*docs, ancestor["head_sha"]]
        self.history = FakeHistory({
            current: (parent, [f"docs/release-note-{index}.md"])
            for index, (current, parent) in enumerate(zip(commits, commits[1:]))
        })
        self.history.responses[ancestor["head_sha"]] = (
            OTHER_TREE, ["src/built_ancestor.cpp"])
        return docs, ancestor

    def assert_docs_red(self, row, *, read_shas):
        stderr = self.assert_refused(
            f"docs-commit-red {row['id']} sha {row['head_sha']}",
            row["status"], row["event"])
        if row["conclusion"] is None:
            self.assertRegex(stderr, r"\b(?:None|null)\b")
        else:
            self.assertIn(row["conclusion"], stderr)
        self.assertEqual(self.post_calls(), [
            (ci_post_runs_api(sha), True, None) for sha in read_shas],
            "stop at the first refused docs run, without any docs or proof jobs")

    def test_plain_pass_reads_non_vacuous_ci_post_and_names_its_evidence(self):
        """R1/R2/R5/R6: block a PR-only green line with no ci-post reads."""
        self.assert_green(self.post_run)
        self.assertEqual(self.history.calls, [self.post_run["head_sha"]])
        self.git.assert_not_called()
        self.proof.assert_not_called()

    def test_four_green_linux_legs_without_macos_prove_the_release(self):
        jobs = linux_ci_post_jobs()
        self.assertEqual(len(jobs), 4)
        self.api.ci_post(self.post_run, jobs)
        self.assert_green(self.post_run)
        self.assertEqual(self.history.calls, [self.post_run["head_sha"]])

    def test_each_missing_linux_leg_is_the_only_missing_job_reported(self):
        complete = linux_ci_post_jobs()
        self.assertEqual(len(complete), 4)
        for original in complete:
            with self.subTest(job=original["name"]):
                self.reset_ci_post()
                jobs = [job for job in complete if job["name"] != original["name"]]
                self.assertEqual(len(jobs), 3)
                self.api.ci_post(self.post_run, jobs)
                stderr = self.assert_no_full_run(
                    [self.post_run], f"missing job {original['name']}")
                self.assertEqual(stderr, (
                    f"::error::ci-post: no-full-run {self.post_run['head_sha']}: "
                    f"examined 1 of 1 run(s); run {self.post_run['id']} "
                    f"sha {self.post_run['head_sha']} event {self.post_run['event']}: "
                    f"missing job {original['name']}\n"))

    def test_red_macos_jobs_follow_only_the_aggregate_run_verdict(self):
        for conclusion in ("success", "failure"):
            with self.subTest(run_conclusion=conclusion):
                self.reset_ci_post()
                row = {**self.post_run, "conclusion": conclusion}
                jobs = linux_ci_post_jobs()
                macos = [
                    {**job, "id": len(jobs) + job["id"], "conclusion": "failure",
                     "name": job["name"].replace("ubuntu-24.04,", "macos-26,")}
                    for job in jobs if job["name"].startswith("build (ubuntu-24.04, ")]
                self.assertEqual(len(macos), 2)
                self.api.ci_post(row, [*jobs, *macos])
                if conclusion == "success":
                    self.assert_green(row)
                else:
                    stderr = self.assert_refused(
                        "not-success", row["id"], row["head_sha"], row["event"],
                        f"conclusion {conclusion}")
                    self.assertEqual(stderr, (
                        f"::error::ci-post: not-success {row['id']} sha {row['head_sha']} "
                        f"event {row['event']}: conclusion {conclusion}\n"))
                    self.assert_post_reads(row)

    def test_gate_accepts_injected_history_and_returns_ci_post_evidence(self):
        """R1/R5/R6: block a CLI-only implementation or an unused history seam."""
        green = ci_release_gate.gate(
            REPO, SHA, "refs/heads/main", api=self.api, git=self.git, history=self.history)
        self.assert_post_reads(self.post_run)
        self.assert_green_evidence(green, self.post_run)
        self.assertEqual(self.history.calls, [self.post_run["head_sha"]])

    def test_pass_evidence_follows_changed_repo_sha_and_run_id(self):
        """R1/R5/R6/R7: block fixture-specific repo, proof sha or run-id answers."""
        repo = "another/release-project"
        sha, head, tree, parent = ("12" * 20, "34" * 20, "56" * 20, "78" * 20)
        row = ci_post_run(self.post_run["id"] + 912, sha=sha)
        self.api.responses = {
            f"repos/{repo}/branches/main": sha,
            f"repos/{repo}/commits/{sha}": json.dumps(commit(sha, tree)),
            f"repos/{repo}/commits/{sha}/pulls": json.dumps([
                pull(head, number=902, merge_sha=sha)]),
            f"repos/{repo}/commits/{head}": json.dumps(commit(head, tree)),
            f"repos/{repo}/commits/{head}/statuses?per_page=100": json.dumps(successes()),
        }
        self.api.ci_post(row, self.jobs, repo=repo)
        self.history = FakeHistory({sha: (parent, ["include/ci_post_fixture.h"])})
        self.assert_green(row, sha=sha, repo=repo)
        self.assertEqual(self.history.calls, [row["head_sha"]])

    def test_docs_only_tip_uses_built_parent_and_names_both_shas(self):
        """R1/R3/R5/R6: block judging only the docs tip or hiding the ancestor."""
        ancestor = self.built_ancestor()
        self.history = FakeHistory({
            SHA: (ancestor["head_sha"], ["docs/release-notes.md"]),
            ancestor["head_sha"]: (OTHER_TREE, ["src/built_ancestor.cpp"]),
        })
        self.api.responses[ci_post_jobs_api(self.post_run["id"])] = jobs_page([
            {**job, "conclusion": "skipped"} for job in self.jobs])
        self.assert_green(ancestor, skipped=[SHA])
        self.assertEqual(self.history.calls, [SHA, ancestor["head_sha"]])

    def test_multiple_docs_commits_use_the_shared_classifiers_full_path_rule(self):
        """R3/R5/R6: block an extension-only classifier or a one-commit walk."""
        ancestor = self.built_ancestor(OTHER_TREE)
        self.history = FakeHistory({
            SHA: (OTHER_HEAD, ["README.md"]),
            OTHER_HEAD: (ancestor["head_sha"], ["docs/assets/release-flow.svg"]),
            ancestor["head_sha"]: (TREE, ["src/built_ancestor.cpp"]),
        })
        self.api.ci_post_runs(
            OTHER_HEAD, [ci_post_run(self.post_run["id"] + 1, sha=OTHER_HEAD)], {})
        self.assert_green(ancestor, skipped=[SHA, OTHER_HEAD])
        self.assertEqual(self.history.calls, [SHA, OTHER_HEAD, ancestor["head_sha"]])

    def test_docs_tip_source_parent_red_cannot_reach_older_or_second_parent_green(self):
        """R1/R3/R4: block skipping a source commit or following a merge's second parent."""
        failed = ci_post_run(self.post_run["id"] + 1, sha=OTHER_HEAD, conclusion="failure")
        older = self.built_ancestor(OTHER_TREE)
        failed_job = self.jobs[2]
        failed_job["conclusion"] = "failure"
        self.api.ci_post(failed, self.jobs)
        candidate = json.loads(self.api.responses[BUMP_API])
        candidate["parents"] = [{"sha": failed["head_sha"]}, {"sha": older["head_sha"]}]
        self.api.responses[BUMP_API] = json.dumps(candidate)
        self.history = FakeHistory({
            SHA: (failed["head_sha"], ["docs/release-notes.md"]),
            failed["head_sha"]: (older["head_sha"], ["src/runtime_change.cpp"]),
            older["head_sha"]: (TREE, ["src/older_build.cpp"]),
        })
        self.assert_refused("newer-run-failed", failed["id"], failed["head_sha"],
                            failed["event"], failed_job["name"], failed_job["conclusion"])
        self.assertEqual(self.history.calls, [SHA, failed["head_sha"]])
        # The failed build is more specific than the run conclusion; jobs must be read.
        self.assert_post_reads(failed, docs=[SHA])

    def test_first_non_documentation_commit_without_run_cannot_use_older_green(self):
        """R1/R3/R4/R6: block skipping CI Markdown, CMake or proof inputs to older green."""
        for path in ("scripts/README.md", ".github/release-notes.md", "tests/README.md",
                     "benchmarks/README.md", "docs/CMakeLists.txt", "docs/toolchain.cmake",
                     "docs/.gitattributes", "docs/adr/0001-kernel-adapter-boundary.md",
                     "docs/pages/native-engine.md"):
            with self.subTest(path=path):
                self.reset_ci_post()
                older = self.built_ancestor(OTHER_TREE)
                self.history = FakeHistory({
                    SHA: (OTHER_HEAD, ["docs/release-notes.md"]),
                    OTHER_HEAD: (older["head_sha"], [path]),
                    older["head_sha"]: (TREE, ["src/older_build.cpp"]),
                })
                self.api.responses[ci_post_runs_api(OTHER_HEAD)] = runs_page([])
                self.assert_refused("no-run", OTHER_HEAD)
                self.assertEqual(self.history.calls, [SHA, OTHER_HEAD])
                self.assertEqual(self.post_calls(), [
                    (ci_post_runs_api(SHA), True, None),
                    (ci_post_runs_api(OTHER_HEAD), True, None)])

    def test_rename_old_non_documentation_side_selects_proof_commit_without_run(self):
        """R1/R3/R4/R6: block judging only a rename's docs destination or using older green."""
        for paths in (["scripts/release-notes.md", "docs/release-notes.md"],
                      ["docs/release-notes.md", "scripts/release-notes.md"]):
            with self.subTest(paths=paths):
                self.reset_ci_post()
                older = self.built_ancestor(OTHER_TREE)
                self.history = FakeHistory({
                    SHA: (OTHER_HEAD, ["README.md"]),
                    OTHER_HEAD: (older["head_sha"], paths),
                    older["head_sha"]: (TREE, ["src/older_build.cpp"]),
                })
                self.api.responses[ci_post_runs_api(OTHER_HEAD)] = runs_page([])
                self.assert_refused("no-run", OTHER_HEAD)
                self.assertEqual(self.history.calls, [SHA, OTHER_HEAD])
                self.assertEqual(self.post_calls(), [
                    (ci_post_runs_api(SHA), True, None),
                    (ci_post_runs_api(OTHER_HEAD), True, None)])

    def test_no_ci_post_run_refuses(self):
        """R1/R4: block accepting green PR-head statuses with no ci-post run."""
        self.api.responses[ci_post_runs_api(self.post_run["head_sha"])] = runs_page([])
        self.assert_refused("no-run", self.post_run["head_sha"])
        self.assertEqual(self.post_calls(), [
            (ci_post_runs_api(self.post_run["head_sha"]), True, None)])

    def test_gate_no_run_raises_release_gate_error_with_typed_reason(self):
        """R4/R6: block untyped exceptions that main cannot turn into an error annotation."""
        self.api.responses[ci_post_runs_api(self.post_run["head_sha"])] = runs_page([])
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                self.assertRaises(ci_release_gate.ReleaseGateError) as raised:
            ci_release_gate.gate(
                REPO, SHA, "refs/heads/main", api=self.api, git=self.git, history=self.history)
        self.assertIn("ci-post: no-run", str(raised.exception))
        self.assertIn(self.post_run["head_sha"], str(raised.exception))
        self.assertEqual(stdout.getvalue(), "", "a direct refusal must not print a green line")
        # Missing evidence stops after the runs read, with no jobs to examine.
        self.assertEqual(self.post_calls(), [
            (ci_post_runs_api(self.post_run["head_sha"]), True, None)])

    def test_running_or_queued_newest_run_is_not_rescued_by_older_green(self):
        """R1/R4: block using a stale success while the newest run has not completed."""
        for state in ("in_progress", "queued", "waiting"):
            with self.subTest(state=state):
                self.reset_ci_post()
                newest = {**self.post_run, "status": state, "conclusion": None}
                older = ci_post_run(self.post_run["id"] + 100,
                                    created_at="2026-10-24T12:00:00Z")
                self.api.ci_post(older, self.jobs)
                self.api.responses[ci_post_runs_api(newest["head_sha"])] = runs_page([
                    older, newest])
                self.assert_refused("run-in-progress", newest["id"], newest["head_sha"],
                                    newest["status"])
                # An unfinished newest run stops before any jobs read, including older green.
                self.assertEqual(self.post_calls(), [
                    (ci_post_runs_api(newest["head_sha"]), True, None)])

    def test_completed_failed_run_refuses(self):
        """R1/R4: block accepting completion without a successful run conclusion."""
        failed = {**self.post_run, "conclusion": "failure"}
        self.api.ci_post(failed, self.jobs)
        self.assert_refused("not-success", failed["id"], failed["head_sha"],
                            failed["event"], failed["conclusion"])
        # Read the four green builds before reporting the run-level failure.
        self.assert_post_reads(failed)

    def test_completed_cancelled_run_refuses(self):
        """R1/R4: block treating a cancelled run as completed verification."""
        cancelled = {**self.post_run, "conclusion": "cancelled"}
        self.api.ci_post(cancelled, self.jobs)
        self.assert_refused("not-success", cancelled["id"], cancelled["head_sha"],
                            cancelled["event"], cancelled["conclusion"])
        # Green build jobs cannot override a cancelled aggregate run.
        self.assert_post_reads(cancelled)

    def test_other_completed_run_conclusions_refuse(self):
        """Round 12/2/3: non-success runs refuse despite four green build legs."""
        for conclusion in ("neutral", "skipped", None, "action_required"):
            with self.subTest(conclusion=conclusion):
                self.reset_ci_post()
                row = {**self.post_run, "conclusion": conclusion}
                self.api.ci_post(row, self.jobs)
                stderr = self.assert_refused(
                    f"not-success {row['id']}", row["head_sha"], row["event"],
                    f"conclusion {row['conclusion']}")
                if conclusion == "skipped":
                    self.assertTrue(any(job["name"] in stderr for job in self.jobs), stderr)
                self.assert_post_reads(row)

    def test_newest_red_run_wins_over_older_green_regardless_of_response_order(self):
        """R1/R4: block first-row selection, largest-id selection or any-green rescue."""
        for newest_first in (False, True):
            with self.subTest(newest_first=newest_first):
                self.reset_ci_post()
                newest = {**self.post_run, "conclusion": "failure"}
                older = ci_post_run(newest["id"] + 100,
                                    created_at="2026-10-24T12:00:00Z")
                failed_jobs = ci_post_jobs()
                failed_job = failed_jobs[2]
                failed_job["conclusion"] = "failure"
                rows = [newest, older] if newest_first else [older, newest]
                self.api.ci_post_runs(newest["head_sha"], rows, {
                    newest["id"]: failed_jobs, older["id"]: self.jobs})
                self.assert_refused("newer-run-failed", newest["id"], newest["head_sha"],
                                    newest["event"], failed_job["name"], failed_job["conclusion"])
                # Only the newest failed run's jobs are read; older green cannot rescue it.
                self.assert_post_reads(newest)

    def test_equal_created_at_uses_higher_run_id(self):
        """R1/R4: block an arbitrary winner when red and green timestamps tie."""
        for newest_first in (False, True):
            with self.subTest(newest_first=newest_first):
                self.reset_ci_post()
                newest = {**self.post_run, "id": self.post_run["id"] + 1, "conclusion": "failure"}
                failed_jobs = ci_post_jobs()
                failed_job = failed_jobs[2]
                failed_job["conclusion"] = "failure"
                rows = [newest, self.post_run] if newest_first else [self.post_run, newest]
                self.api.ci_post_runs(newest["head_sha"], rows, {
                    newest["id"]: failed_jobs, self.post_run["id"]: self.jobs})
                self.assert_refused("newer-run-failed", newest["id"], newest["head_sha"],
                                    newest["event"], failed_job["name"], failed_job["conclusion"])
                # Equal timestamps still require only the higher-id run's jobs.
                self.assert_post_reads(newest)

    def test_run_for_different_sha_is_ignored(self):
        """R1/R4: block trusting the server's head_sha filter without checking each run."""
        unrelated = {**self.post_run, "head_sha": OTHER_HEAD}
        self.api.responses[ci_post_runs_api(SHA)] = runs_page([unrelated])
        self.assert_refused("no-run", SHA)
        self.assertEqual(self.post_calls(), [(ci_post_runs_api(SHA), True, None)])

    def test_run_on_another_branch_is_ignored(self):
        """R1/R4: block using a successful run for the same commit on another branch."""
        unrelated = {**self.post_run, "head_branch": "release"}
        self.api.responses[ci_post_runs_api(SHA)] = runs_page([unrelated])
        self.assert_refused("no-run", SHA)
        self.assertEqual(self.post_calls(), [(ci_post_runs_api(SHA), True, None)])

    def test_each_required_build_job_skipped_refuses(self):
        """R2/R4: block each of the four matrix cells becoming vacuous green."""
        for index, original in enumerate(ci_post_jobs()):
            with self.subTest(job=original["name"]):
                self.reset_ci_post()
                job = self.jobs[index]
                job["conclusion"] = "skipped"
                self.api.ci_post(self.post_run, self.jobs)
                self.assert_no_full_run([self.post_run], job["name"], job["conclusion"])

    def test_each_required_build_job_failed_refuses(self):
        """R2/R4: prove each failed build blocks; block partial-matrix or green-run rescue."""
        for index, original in enumerate(ci_post_jobs()):
            for conclusion in ("failure", "cancelled", "timed_out"):
                with self.subTest(job=original["name"], conclusion=conclusion):
                    self.reset_ci_post()
                    job = self.jobs[index]
                    job["conclusion"] = conclusion
                    self.api.ci_post(self.post_run, self.jobs)
                    self.assert_refused(
                        "newer-run-failed", self.post_run["id"], self.post_run["head_sha"],
                        self.post_run["event"], job["name"], job["conclusion"])
                    self.assert_post_reads(self.post_run)

    def test_other_non_success_job_conclusions_refuse_for_every_cell(self):
        """R2/R4: prove uncertain legs give no proof; block neutral/null/unknown green."""
        for index, original in enumerate(ci_post_jobs()):
            for conclusion in ("neutral", "action_required", None, "unknown"):
                with self.subTest(job=original["name"], conclusion=conclusion):
                    self.reset_ci_post()
                    job = self.jobs[index]
                    job["conclusion"] = conclusion
                    self.api.ci_post(self.post_run, self.jobs)
                    stderr = self.assert_no_full_run([self.post_run], job["name"])
                    if job["conclusion"] is None:
                        self.assertRegex(stderr, r"\b(?:None|null)\b")
                    else:
                        self.assertIn(job["conclusion"], stderr)

    def test_missing_required_job_cannot_be_replaced_by_green_aggregate(self):
        """R2/R4: block accepting job counts or the aggregate build instead of exact names."""
        for index, original in enumerate(ci_post_jobs()):
            with self.subTest(job=original["name"]):
                self.reset_ci_post()
                missing = self.jobs.pop(index)
                self.jobs.append({**missing, "name": "build"})
                self.api.ci_post(self.post_run, self.jobs)
                self.assert_no_full_run([self.post_run], "missing", missing["name"])

    def test_single_run_with_a_missing_leg_gives_no_full_run(self):
        """Round 13/2(b): a sole missing-leg run exhausts the proof search.

        Block a not-success refusal when no older run remains to protect.
        """
        for index, original in enumerate(ci_post_jobs()):
            with self.subTest(job=original["name"]):
                self.reset_ci_post()
                row = self.post_run
                missing = self.jobs.pop(index)
                self.api.ci_post_runs(row["head_sha"], [row], {row["id"]: self.jobs})
                self.assert_no_full_run(
                    [row], row["event"], "examined 1 of 1",
                    f"missing job {missing['name']}")

    def test_missing_leg_run_with_an_older_proof_refuses_not_success(self):
        """Round 13/2(b): a newer missing-leg run refuses before older proof.

        Block a defective newer success run hiding behind an older green run.
        """
        for index, original in enumerate(ci_post_jobs()):
            with self.subTest(job=original["name"]):
                self.reset_ci_post()
                newest = self.post_run
                missing = self.jobs.pop(index)
                older = ci_post_run(
                    newest["id"] - 1, created_at="2026-10-24T12:00:00Z")
                self.api.ci_post_runs(newest["head_sha"], [newest, older], {
                    newest["id"]: self.jobs, older["id"]: ci_post_jobs()})
                self.assert_refused(
                    f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                    f"conclusion {newest['conclusion']}", f"missing job {missing['name']}")
                self.assert_post_reads(newest)

    def test_jobs_total_larger_than_returned_rows_refuses(self):
        """R1/R4: block accepting four successes from an incomplete jobs response."""
        endpoint = ci_post_jobs_api(self.post_run["id"])
        self.api.responses[endpoint] = jobs_page(self.jobs, total=len(self.jobs) + 1)
        self.assert_refused("unreadable", endpoint)
        self.assert_post_reads(self.post_run)

    def test_ci_post_rate_limits_name_the_endpoint_and_are_not_retried(self):
        """R4: block retrying or ignoring rate limits on either ci-post read."""
        for jobs_read in (False, True):
            for raised in (False, True):
                with self.subTest(jobs_read=jobs_read, raised=raised):
                    self.reset_ci_post()
                    endpoint = (ci_post_jobs_api(self.post_run["id"]) if jobs_read
                                else ci_post_runs_api(self.post_run["head_sha"]))
                    self.api.responses[endpoint] = (
                        subprocess.CalledProcessError(
                            1, ["gh", "api", endpoint],
                            stderr="gh: API rate limit exceeded (HTTP 403)\n")
                        if raised else http_response(
                            403, '{"message":"API rate limit exceeded"}'))
                    self.assert_refused("unreadable", endpoint, "rate limited")
                    self.assertEqual(Counter(call[0] for call in self.api.calls)[endpoint], 1)
                    self.assertEqual(self.api.calls[-1], (endpoint, not jobs_read, None))

    def test_ci_post_5xx_errors_name_the_endpoint_and_are_not_retried(self):
        """R4: block upstream errors being swallowed or retried on either ci-post read."""
        for jobs_read in (False, True):
            for code in (500, 503, 599):
                with self.subTest(jobs_read=jobs_read, code=code):
                    self.reset_ci_post()
                    endpoint = (ci_post_jobs_api(self.post_run["id"]) if jobs_read
                                else ci_post_runs_api(self.post_run["head_sha"]))
                    self.api.responses[endpoint] = http_response(code, "unavailable")
                    self.assert_refused("unreadable", endpoint, f"upstream error {code}")
                    self.assertEqual(Counter(call[0] for call in self.api.calls)[endpoint], 1)
                    self.assertEqual(self.api.calls[-1], (endpoint, not jobs_read, None))

    def test_ci_post_transport_empty_and_parse_errors_fail_closed(self):
        """R4: block unreadable runs or jobs silently becoming successful evidence."""
        for jobs_read in (False, True):
            for response, diagnostic in (
                    (OSError("connection refused"), "failed: connection refused"),
                    (http_response(200, ""), "returned an empty payload"),
                    (http_response(200, "not JSON"), "returned invalid JSON")):
                with self.subTest(jobs_read=jobs_read, diagnostic=diagnostic):
                    self.reset_ci_post()
                    endpoint = (ci_post_jobs_api(self.post_run["id"]) if jobs_read
                                else ci_post_runs_api(self.post_run["head_sha"]))
                    self.api.responses[endpoint] = response
                    self.assert_refused("unreadable", endpoint, diagnostic)
                    self.assertEqual(Counter(call[0] for call in self.api.calls)[endpoint], 1)
                    self.assertEqual(self.api.calls[-1], (endpoint, not jobs_read, None))

    def test_runs_at_or_beyond_1000_limit_refuse_even_with_green_rows(self):
        """R1/R4: block trusting a capped Actions search with matching or larger totals."""
        for total in (1000, 1001):
            with self.subTest(total=total):
                self.reset_ci_post()
                endpoint = ci_post_runs_api(self.post_run["head_sha"])
                rows = [{**self.post_run, "id": self.post_run["id"] + index} for index in range(1000)]
                self.api.responses[endpoint] = "".join(
                    runs_page(rows[start:start + 100], total=total, headers={
                        "Link": f'<https://api.github.com/{endpoint}&page={start // 100 + 2}>; '
                        'rel="next"'} if start < 900 else None)
                    for start in range(0, len(rows), 100))
                self.assert_refused("unreadable", endpoint, "1000")
                self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_truncated_runs_total_refuses_before_jobs_are_read(self):
        """R1/R4: block accepting the visible green run when total_count promises more."""
        endpoint = ci_post_runs_api(self.post_run["head_sha"])
        self.api.responses[endpoint] = runs_page([self.post_run], total=2)
        self.assert_refused("unreadable", endpoint, "incomplete workflow_runs pagination")
        self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_unread_runs_link_refuses_even_when_total_matches(self):
        """R1/R4: block matching totals hiding an advertised unread runs page."""
        endpoint = ci_post_runs_api(self.post_run["head_sha"])
        self.api.responses[endpoint] = runs_page([self.post_run], headers={
            "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
        self.assert_refused("unreadable", endpoint, "incomplete workflow_runs pagination")
        self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_later_runs_page_error_cannot_hide_behind_a_green_first_page(self):
        """R1/R4: block trusting early success before reading all runs pages."""
        endpoint = ci_post_runs_api(self.post_run["head_sha"])
        self.api.responses[endpoint] = (
            runs_page([self.post_run], total=2, headers={
                "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
            + http_response(503, "unavailable"))
        self.assert_refused("unreadable", endpoint, "upstream error 503")
        self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_paginated_runs_choose_newest_matching_green_and_read_jobs_once(self):
        """R1/R5: block first-page selection, unrelated newer runs or per-page REST calls."""
        endpoint = ci_post_runs_api(self.post_run["head_sha"])
        older = ci_post_run(self.post_run["id"] + 200, conclusion="failure",
                            created_at="2026-10-24T12:00:00Z")
        unrelated = [{**self.post_run, "id": self.post_run["id"] + index + 1,
                      "head_branch": "release", "created_at": "2026-10-26T12:00:00Z"}
                     for index in range(99)]
        rows = [older, *unrelated, self.post_run]
        self.api.responses[endpoint] = (
            runs_page(rows[:100], total=len(rows), headers={
                "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
            + runs_page(rows[100:], total=len(rows), headers={
                "Link": f'<https://api.github.com/{endpoint}&page=1>; rel="prev"'}))
        self.assert_green(self.post_run)

    def test_empty_diff_is_not_docs_only_and_cannot_reach_parent_green(self):
        """R3/R4/R6: block vacuous all(paths) walking an empty commit to old evidence."""
        ancestor = self.built_ancestor()
        self.history = FakeHistory({
            SHA: (ancestor["head_sha"], []),
            ancestor["head_sha"]: (OTHER_TREE, ["src/older_build.cpp"]),
        })
        self.api.responses[ci_post_runs_api(SHA)] = runs_page([])
        self.assert_refused("no-run", SHA)
        self.assertEqual(self.history.calls, [SHA])
        self.assertEqual(self.post_calls(), [(ci_post_runs_api(SHA), True, None)])

    def test_twenty_docs_commits_can_reach_a_built_proof(self):
        """R3/R5/R6: block an off-by-one limit refusing the allowed twenty-docs walk."""
        docs, ancestor = self.docs_chain(20)
        self.assert_green(ancestor, skipped=docs)
        self.assertEqual(self.history.calls, [*docs, ancestor["head_sha"]])

    def test_twenty_one_docs_commits_refuse_before_reaching_older_green(self):
        """R3/R4/R6: block an unbounded walk despite a green build just beyond the limit."""
        docs, ancestor = self.docs_chain(21)
        self.assert_refused("walk-too-long", SHA, "20")
        self.assertNotIn(ancestor["head_sha"], self.history.calls)
        self.assertTrue(all(sha in docs for sha in self.history.calls))
        self.assertEqual(self.post_calls(), [])

    def test_root_commit_refuses_even_with_a_green_ci_post_fixture(self):
        """R3/R4/R6: block proving any root commit without its required first-parent diff."""
        for paths in (["docs/release-notes.md"], ["src/root_fixture.cpp"], []):
            with self.subTest(paths=paths):
                self.reset_ci_post()
                self.history = FakeHistory({SHA: (None, paths)})
                self.assert_refused("walk-blocked", SHA, "parent")
                self.assertEqual(self.history.calls, [SHA])
                self.assertEqual(self.post_calls(), [])

    def test_root_commit_during_docs_walk_refuses_even_with_green_ci_post_fixture(self):
        """R3/R4/R6: block accepting a root's green run after a docs-only step."""
        for paths in (["docs/release-notes.md"], ["src/root_fixture.cpp"], []):
            with self.subTest(paths=paths):
                self.reset_ci_post()
                root = self.built_ancestor()
                self.history = FakeHistory({
                    SHA: (root["head_sha"], ["docs/release-notes.md"]),
                    root["head_sha"]: (None, paths),
                })
                self.assert_refused("walk-blocked", root["head_sha"], "parent")
                self.assertEqual(self.history.calls, [SHA, root["head_sha"]])
                self.assertEqual(self.post_calls(), [])

    def test_unreadable_history_at_tip_or_during_walk_refuses(self):
        """R3/R4/R6: block a git failure authorizing a docs skip or an older green."""
        for failed_sha in (SHA, OTHER_HEAD):
            with self.subTest(failed_sha=failed_sha):
                self.reset_ci_post()
                ancestor = self.built_ancestor(OTHER_TREE)
                detail = f"git cannot read commit {failed_sha}"
                self.history = FakeHistory({
                    SHA: (OTHER_HEAD, ["docs/release-notes.md"]),
                    OTHER_HEAD: (ancestor["head_sha"], ["docs/another-note.md"]),
                    ancestor["head_sha"]: (TREE, ["src/older_build.cpp"]),
                })
                self.history.responses[failed_sha] = OSError(detail)
                self.assert_refused("walk-blocked", failed_sha, detail)
                expected = [SHA] if failed_sha == SHA else [SHA, failed_sha]
                self.assertEqual(self.history.calls, expected)
                self.assertEqual(self.post_calls(), [])

    def test_changed_path_read_failure_at_tip_or_during_walk_refuses(self):
        """R3/R4/R6: block using green evidence when git cannot list changed paths."""
        for failed_sha in (SHA, OTHER_HEAD):
            with self.subTest(failed_sha=failed_sha):
                self.reset_ci_post()
                ancestor = self.built_ancestor(OTHER_TREE)
                self.history = FakeHistory({
                    SHA: (OTHER_HEAD, ["docs/release-notes.md"]),
                    OTHER_HEAD: (ancestor["head_sha"], ["docs/another-note.md"]),
                    ancestor["head_sha"]: (TREE, ["src/older_build.cpp"]),
                })
                parent = self.history.responses[failed_sha][0]
                detail = (f"git diff --name-only --no-renames {parent} {failed_sha} failed: "
                          "unreadable changed-path list")
                self.history.responses[failed_sha] = OSError(detail)
                self.assert_refused("walk-blocked", failed_sha, detail)
                expected = [SHA] if failed_sha == SHA else [SHA, failed_sha]
                self.assertEqual(self.history.calls, expected)
                self.assertEqual(self.post_calls(), [])

    def test_phase_two_pass_also_reads_ci_post_without_pr_fallback(self):
        """R1/R5/R6/R7: block the main-proof early return bypassing non-vacuous ci-post."""
        self.enable_watcher({row["context"]: row["state"] for row in successes()})
        self.assert_green(self.post_run)
        self.proof.assert_called_once_with(REPO, SHA)
        self.assertEqual(self.api.calls, [
            (MAIN_API, False, ".commit.sha"),
            (ci_post_runs_api(self.post_run["head_sha"]), True, None),
            (ci_post_jobs_api(self.post_run["id"]), False, None),
        ])
        self.assertEqual(self.history.calls, [self.post_run["head_sha"]])

    def test_phase_two_green_receipt_cannot_rescue_red_ci_post(self):
        """R1/R4/R6/R7: block a valid main-proof receipt admitting a failed post-merge run."""
        self.enable_watcher({row["context"]: row["state"] for row in successes()})
        failed = {**self.post_run, "conclusion": "failure"}
        self.api.ci_post(failed, self.jobs)
        self.assert_refused("not-success", failed["id"], failed["head_sha"],
                            failed["event"], failed["conclusion"])
        self.proof.assert_called_once_with(REPO, SHA)
        # A phase-two receipt still needs the completed run's jobs to diagnose its failure.
        self.assertEqual(self.api.calls, [
            (MAIN_API, False, ".commit.sha"),
            (ci_post_runs_api(failed["head_sha"]), True, None),
            (ci_post_jobs_api(failed["id"]), False, None),
        ])

    def test_release_bump_pass_requires_its_own_ci_post_build(self):
        """R1/R5/R6/R7: block the release-bump exemption bypassing post-merge proof."""
        self.api.release_bump()
        self.history = FakeHistory({
            SHA: (BUMP_PARENT, ["VERSION", "docker/run_failure_codes_diff.json"]),
        })
        self.assert_green(self.post_run)
        self.assertEqual(self.history.calls, [self.post_run["head_sha"]])
        self.assertIn((RUNS_API, True, None), self.api.calls)
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_release_bump_cannot_use_parent_ci_post_when_its_own_run_is_missing(self):
        """R1/R3/R4/R6/R7: block reusing parent CI for a non-documentation release bump."""
        self.api.release_bump()
        ancestor = self.built_ancestor(BUMP_PARENT)
        self.history = FakeHistory({
            SHA: (ancestor["head_sha"], ["VERSION", "docker/run_failure_codes_diff.json"]),
            ancestor["head_sha"]: (OTHER_TREE, ["src/parent_build.cpp"]),
        })
        self.api.responses[ci_post_runs_api(SHA)] = runs_page([])
        self.assert_refused("no-run", SHA)
        self.assertEqual(self.history.calls, [SHA])
        self.assertEqual(self.post_calls(), [(ci_post_runs_api(SHA), True, None)])
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_completed_timed_out_run_with_four_green_builds_refuses(self):
        """Rule 3: prove aggregate timeout blocks; block four green builds rescuing it."""
        timed_out = {**self.post_run, "conclusion": "timed_out"}
        self.api.ci_post(timed_out, self.jobs)
        self.assert_refused("not-success", timed_out["id"], timed_out["head_sha"],
                            timed_out["event"], timed_out["conclusion"])
        self.assert_post_reads(timed_out)

    def test_newer_failed_run_with_four_green_builds_blocks_older_green(self):
        """Rule 3: prove non-build failure blocks; block an older full-green rescue."""
        newest = {**self.post_run, "conclusion": "failure"}
        older = ci_post_run(newest["id"] + 100, created_at="2026-10-24T12:00:00Z")
        self.api.ci_post_runs(newest["head_sha"], [older, newest], {
            newest["id"]: self.jobs, older["id"]: ci_post_jobs()})
        self.assert_refused("not-success", newest["id"], newest["head_sha"],
                            newest["event"], newest["conclusion"])
        self.assert_post_reads(newest)

    def test_newer_cancelled_run_with_four_green_builds_blocks_older_green(self):
        """Rule 3: prove aggregate cancellation blocks; block older-green rescue."""
        newest = {**self.post_run, "conclusion": "cancelled"}
        older = ci_post_run(newest["id"] + 100, created_at="2026-10-24T12:00:00Z")
        self.api.ci_post_runs(newest["head_sha"], [older, newest], {
            newest["id"]: self.jobs, older["id"]: ci_post_jobs()})
        self.assert_refused("not-success", newest["id"], newest["head_sha"],
                            newest["event"], newest["conclusion"])
        self.assert_post_reads(newest)

    def test_newer_timed_out_run_with_four_green_builds_blocks_older_green(self):
        """Rule 3: prove aggregate timeout blocks; block accepting older verification."""
        newest = {**self.post_run, "conclusion": "timed_out"}
        older = ci_post_run(newest["id"] + 100, created_at="2026-10-24T12:00:00Z")
        self.api.ci_post_runs(newest["head_sha"], [older, newest], {
            newest["id"]: self.jobs, older["id"]: ci_post_jobs()})
        self.assert_refused("not-success", newest["id"], newest["head_sha"],
                            newest["event"], newest["conclusion"])
        self.assert_post_reads(newest)

    def test_newest_red_run_with_all_skipped_builds_blocks_older_green(self):
        """R-A: prove skipped builds cannot hide run failure; block older-green rescue."""
        for conclusion in ("failure", "cancelled", "timed_out"):
            with self.subTest(conclusion=conclusion):
                self.reset_ci_post()
                newest = ci_post_run(
                    self.post_run["id"] + 1, conclusion=conclusion,
                    created_at="2026-10-25T12:01:00Z")
                skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
                self.api.ci_post_runs(newest["head_sha"], [newest, self.post_run], {
                    newest["id"]: skipped_jobs, self.post_run["id"]: self.jobs})
                self.assert_refused("not-success", newest["id"], newest["head_sha"],
                                    newest["event"], newest["conclusion"])
                self.assert_post_reads(newest)

    def test_newest_red_run_with_partly_skipped_builds_blocks_older_green(self):
        """R-A: prove partial skips cannot hide run failure; block older-green rescue."""
        for conclusion in ("failure", "cancelled", "timed_out"):
            for skipped_count in (2, 3):
                with self.subTest(conclusion=conclusion, skipped_count=skipped_count):
                    self.reset_ci_post()
                    newest = ci_post_run(
                        self.post_run["id"] + 1, conclusion=conclusion,
                        created_at="2026-10-25T12:01:00Z")
                    newer_jobs = ci_post_jobs()
                    for job in newer_jobs[:skipped_count]:
                        job["conclusion"] = "skipped"
                    self.api.ci_post_runs(newest["head_sha"], [newest, self.post_run], {
                        newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                    self.assert_refused("not-success", newest["id"], newest["head_sha"],
                                        newest["event"], newest["conclusion"])
                    self.assert_post_reads(newest)

    def test_newest_red_skipped_builds_run_is_sorted_before_skipped_only_and_green(self):
        """R-A: prove ordered run failure blocks; block list-order or skipped-only rescue."""
        middle = ci_post_run(self.post_run["id"] + 1, created_at="2026-10-25T12:01:00Z")
        newest = ci_post_run(
            self.post_run["id"] + 2, conclusion="failure", created_at="2026-10-25T12:02:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        # The newest red run is last in the response, behind skipped-only and green runs.
        self.api.ci_post_runs(newest["head_sha"], [middle, self.post_run, newest], {
            newest["id"]: skipped_jobs, middle["id"]: skipped_jobs,
            self.post_run["id"]: self.jobs})
        self.assert_refused("not-success", newest["id"], newest["head_sha"],
                            newest["event"], newest["conclusion"])
        self.assert_post_reads(newest)

    def test_red_run_with_skipped_builds_after_skipped_only_newest_blocks_older_green(self):
        """R-A: prove every examined run's failure blocks; block newest-only or green rescue."""
        failed = ci_post_run(
            self.post_run["id"] + 1, conclusion="failure", created_at="2026-10-25T12:01:00Z")
        newest = ci_post_run(self.post_run["id"] + 2, created_at="2026-10-25T12:02:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post_runs(failed["head_sha"], [newest, failed, self.post_run], {
            newest["id"]: skipped_jobs, failed["id"]: skipped_jobs,
            self.post_run["id"]: self.jobs})
        self.assert_refused("not-success", failed["id"], failed["head_sha"],
                            failed["event"], failed["conclusion"])
        self.assert_post_reads(failed, newer=[newest])

    def test_failed_build_with_skipped_others_overrides_run_failure(self):
        """R-A: prove build failure takes priority; block not-success or skipped-leg rescue."""
        newest = ci_post_run(
            self.post_run["id"] + 1, conclusion="failure", created_at="2026-10-25T12:01:00Z")
        failed_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        failed_job = failed_jobs[2]
        failed_job["conclusion"] = "failure"
        self.api.ci_post_runs(newest["head_sha"], [newest, self.post_run], {
            newest["id"]: failed_jobs, self.post_run["id"]: self.jobs})
        self.assert_refused("newer-run-failed", newest["id"], newest["head_sha"],
                            newest["event"], failed_job["name"], failed_job["conclusion"])
        self.assert_post_reads(newest)

    def test_failed_build_overrides_run_failure_and_names_first_failed_job(self):
        """Rule 3: prove build failure has priority; block aggregate-only or last-job reasons."""
        failed = {**self.post_run, "conclusion": "failure"}
        first_failed = self.jobs[0]
        first_failed["conclusion"] = "cancelled"
        self.jobs[2]["conclusion"] = "failure"
        self.api.ci_post(failed, self.jobs)
        self.assert_refused("newer-run-failed", failed["id"], failed["head_sha"],
                            failed["event"], first_failed["name"], first_failed["conclusion"])
        self.assert_post_reads(failed)

    def test_newer_schedule_or_dispatch_red_build_blocks_older_green_push(self):
        """Rules 3/event: prove any-event red builds block; block a push-only run filter."""
        for event in ("schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                self.reset_ci_post()
                newest = ci_post_run(
                    self.post_run["id"] + 1, event=event, conclusion="failure",
                    created_at="2026-10-26T12:00:00Z")
                failed_jobs = ci_post_jobs()
                failed_job = failed_jobs[2]
                failed_job["conclusion"] = "failure"
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                    newest["id"]: failed_jobs, self.post_run["id"]: self.jobs})
                self.assert_refused("newer-run-failed", newest["id"], newest["head_sha"],
                                    newest["event"], failed_job["name"], failed_job["conclusion"])
                self.assert_post_reads(newest)

    def test_schedule_or_dispatch_full_green_run_proves_without_a_push(self):
        """Rule 2/event: prove non-push green runs count; block requiring a push to prove."""
        for event in ("schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                self.reset_ci_post()
                proving = {**self.post_run, "event": event}
                self.api.ci_post(proving, self.jobs)
                self.assert_green(proving)

    def test_v1_7_0_skipped_push_and_newer_failed_schedule_refuse(self):
        """Rule 3/1.7.0: prove the red schedule blocks; block vacuous push-green release."""
        push = ci_post_run(40151975341, created_at="2026-10-25T01:47:00Z")
        scheduled = ci_post_run(
            40152213697, event="schedule", conclusion="failure",
            created_at="2026-10-25T01:51:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        failed_jobs = ci_post_jobs()
        failed_job = failed_jobs[2]
        failed_job["conclusion"] = "failure"
        self.api.ci_post_runs(scheduled["head_sha"], [push, scheduled], {
            push["id"]: skipped_jobs, scheduled["id"]: failed_jobs})
        self.assert_refused("newer-run-failed", scheduled["id"], scheduled["head_sha"],
                            scheduled["event"], failed_job["name"], failed_job["conclusion"])
        self.assert_post_reads(scheduled)

    def test_release_sha_docs_only_over_failed_push_refuses_through_walk(self):
        """Walk/rule 3: prove a failed source parent blocks; block docs-tip green rescue."""
        failed = ci_post_run(self.post_run["id"] + 1, sha=OTHER_HEAD, conclusion="failure")
        failed_job = self.jobs[2]
        failed_job["conclusion"] = "failure"
        self.api.ci_post(failed, self.jobs)
        self.history = FakeHistory({
            SHA: (failed["head_sha"], ["docs/release-notes.md"]),
            failed["head_sha"]: (OTHER_TREE, ["src/release_proof.cpp"]),
        })
        self.assert_refused("newer-run-failed", failed["id"], failed["head_sha"],
                            failed["event"], failed_job["name"], failed_job["conclusion"])
        self.assertEqual(self.history.calls, [SHA, failed["head_sha"]])
        self.assert_post_reads(failed, docs=[SHA])

    def test_only_skipped_runs_and_a_missing_matrix_name_give_no_full_run(self):
        """Rules 4/5: prove all non-proving runs exhaust; block skipped or missing-cell green."""
        newest = ci_post_run(self.post_run["id"] + 2, created_at="2026-10-25T12:02:00Z")
        middle = ci_post_run(self.post_run["id"] + 1, conclusion="skipped",
                             created_at="2026-10-25T12:01:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        missing_jobs = ci_post_jobs()
        missing = missing_jobs.pop(2)
        self.api.ci_post_runs(newest["head_sha"], [middle, self.post_run, newest], {
            newest["id"]: skipped_jobs, middle["id"]: skipped_jobs,
            self.post_run["id"]: missing_jobs})
        self.assert_no_full_run(
            [newest, middle, self.post_run],
            skipped_jobs[0]["conclusion"], "missing", missing["name"])

    def test_all_skipped_docs_tip_and_selected_source_proof_give_no_full_run(self):
        """Walk/rules 4/5: prove skipped proof is vacuous; block walking past source to green."""
        older = self.built_ancestor(OTHER_TREE)
        proof = ci_post_run(self.post_run["id"] + 1, sha=OTHER_HEAD)
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post(self.post_run, skipped_jobs)
        self.api.ci_post(proof, skipped_jobs)
        self.history = FakeHistory({
            SHA: (proof["head_sha"], ["README.md"]),
            proof["head_sha"]: (older["head_sha"], ["src/release_proof.cpp"]),
            older["head_sha"]: (TREE, ["src/older_build.cpp"]),
        })
        self.assert_no_full_run([proof], skipped_jobs[0]["conclusion"], docs=[SHA])
        self.assertEqual(self.history.calls, [SHA, proof["head_sha"]])

    def test_skipped_only_newer_run_allows_older_full_green(self):
        """Round 12/2/4: only success with skips or skipped with four skips falls through."""
        for conclusion, skipped_count in (
                ("success", 1), ("success", len(ci_post_jobs())),
                ("skipped", len(ci_post_jobs()))):
            with self.subTest(conclusion=conclusion, skipped_count=skipped_count):
                self.reset_ci_post()
                newest = ci_post_run(
                    self.post_run["id"] + 1, conclusion=conclusion,
                    created_at="2026-10-25T12:01:00Z")
                newer_jobs = ci_post_jobs()
                for job in newer_jobs[:skipped_count]:
                    job["conclusion"] = "skipped"
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                    newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                self.assert_green(self.post_run, newer_skipped=[newest])

    def test_uncertain_or_missing_newer_matrix_cell_refuses_older_full_green(self):
        """Round 12/4: neutral, action-required, null, unknown or missing legs block older green."""
        for index, original in enumerate(ci_post_jobs()):
            for conclusion in ("neutral", "action_required", None, "unknown", "missing"):
                with self.subTest(job=original["name"], conclusion=conclusion):
                    self.reset_ci_post()
                    newest = ci_post_run(
                        self.post_run["id"] + 1, created_at="2026-10-25T12:01:00Z")
                    newer_jobs = ci_post_jobs()
                    if conclusion == "missing":
                        newer_jobs.pop(index)
                    else:
                        newer_jobs[index]["conclusion"] = conclusion
                    self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                        newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                    evidence = (f"missing job {original['name']}" if conclusion == "missing"
                                else f"job {original['name']}={conclusion}")
                    self.assert_refused(
                        f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                        evidence)
                    self.assert_post_reads(newest)

    def test_newest_full_green_push_never_reads_older_red_jobs(self):
        """Rule 2: prove newest full green stops; block oldest-first selection or extra reads."""
        for newest_first in (False, True):
            with self.subTest(newest_first=newest_first):
                self.reset_ci_post()
                older = ci_post_run(
                    self.post_run["id"] + 100, conclusion="failure",
                    created_at="2026-10-24T12:00:00Z")
                failed_jobs = ci_post_jobs()
                failed_jobs[2]["conclusion"] = "failure"
                rows = [self.post_run, older] if newest_first else [older, self.post_run]
                self.api.ci_post_runs(self.post_run["head_sha"], rows, {
                    self.post_run["id"]: self.jobs, older["id"]: failed_jobs})
                self.assert_green(self.post_run)

    def test_newest_full_green_push_ignores_older_queued_or_in_progress_run(self):
        """P3: prove full green stops the walk; block older unfinished vetoes or extra reads."""
        for state in ("queued", "in_progress"):
            with self.subTest(state=state):
                self.reset_ci_post()
                older = ci_post_run(
                    self.post_run["id"] + 100, state=state, conclusion=None,
                    created_at="2026-10-24T12:00:00Z")
                self.api.ci_post_runs(self.post_run["head_sha"], [self.post_run, older], {
                    self.post_run["id"]: self.jobs, older["id"]: ci_post_jobs()})
                self.assert_green(self.post_run)

    def test_first_proving_run_stops_before_an_older_full_green_run(self):
        """Rule 2: prove the first full run wins; block oldest-green selection or extra jobs."""
        proving = ci_post_run(self.post_run["id"] + 1, created_at="2026-10-25T12:01:00Z")
        newest = ci_post_run(self.post_run["id"] + 2, created_at="2026-10-25T12:02:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post_runs(proving["head_sha"], [self.post_run, newest, proving], {
            newest["id"]: skipped_jobs, proving["id"]: ci_post_jobs(),
            self.post_run["id"]: self.jobs})
        self.assert_green(proving, newer_skipped=[newest])

    def test_skipped_newer_run_count_is_separate_from_docs_only_commit_count(self):
        """Green line: prove both counts follow fixtures; block a bare or reused number."""
        docs, ancestor = self.docs_chain(2)
        newest = [
            ci_post_run(
                ancestor["id"] + index + 1, sha=ancestor["head_sha"], event=event,
                conclusion="skipped" if event == "schedule" else "success",
                created_at=f"2026-10-25T12:0{3 - index}:00Z")
            for index, event in enumerate(("push", "schedule", "workflow_dispatch"))
        ]
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post_runs(ancestor["head_sha"], [ancestor, *reversed(newest)], {
            ancestor["id"]: ci_post_jobs(),
            **{row["id"]: skipped_jobs for row in newest},
        })
        self.assert_green(ancestor, skipped=docs, newer_skipped=newest)
        self.assertEqual(self.history.calls, [*docs, ancestor["head_sha"]])

    def test_running_run_after_skipped_newer_run_blocks_older_green_without_jobs(self):
        """Rules 1/4: prove an unfinished intermediate run blocks; block skipping to green."""
        for state in ("queued", "in_progress", "waiting"):
            with self.subTest(state=state):
                self.reset_ci_post()
                running = ci_post_run(
                    self.post_run["id"] + 1, state=state, conclusion=None,
                    created_at="2026-10-25T12:01:00Z")
                newest = ci_post_run(
                    self.post_run["id"] + 2, created_at="2026-10-25T12:02:00Z")
                skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, running, newest], {
                    newest["id"]: skipped_jobs, running["id"]: ci_post_jobs(),
                    self.post_run["id"]: self.jobs})
                self.assert_refused("run-in-progress", running["id"], running["head_sha"],
                                    running["status"])
                # Only the completed skipped run has jobs read before the unfinished blocker.
                self.assertEqual(self.post_calls(), [
                    (ci_post_runs_api(newest["head_sha"]), True, None),
                    (ci_post_jobs_api(newest["id"]), False, None),
                ])

    def test_red_build_after_skipped_newer_run_blocks_older_green(self):
        """Rules 3/4: prove an intermediate red build blocks; block newest-only failure checks."""
        failed = ci_post_run(
            self.post_run["id"] + 1, conclusion="failure", created_at="2026-10-25T12:01:00Z")
        newest = ci_post_run(self.post_run["id"] + 2, created_at="2026-10-25T12:02:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        failed_jobs = ci_post_jobs()
        failed_job = failed_jobs[2]
        failed_job["conclusion"] = "failure"
        self.api.ci_post_runs(failed["head_sha"], [self.post_run, failed, newest], {
            newest["id"]: skipped_jobs, failed["id"]: failed_jobs,
            self.post_run["id"]: self.jobs})
        self.assert_refused("newer-run-failed", failed["id"], failed["head_sha"],
                            failed["event"], failed_job["name"], failed_job["conclusion"])
        self.assert_post_reads(failed, newer=[newest])

    def test_unreadable_jobs_after_skipped_newer_run_cannot_reach_older_green(self):
        """Reads/rule 4: prove every jobs read fails closed; block skipping unreadable evidence."""
        for failed_read in ("upstream", "total"):
            with self.subTest(failed_read=failed_read):
                self.reset_ci_post()
                unreadable = ci_post_run(
                    self.post_run["id"] + 1, created_at="2026-10-25T12:01:00Z")
                newest = ci_post_run(
                    self.post_run["id"] + 2, created_at="2026-10-25T12:02:00Z")
                skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, unreadable, newest], {
                    newest["id"]: skipped_jobs, unreadable["id"]: ci_post_jobs(),
                    self.post_run["id"]: self.jobs})
                endpoint = ci_post_jobs_api(unreadable["id"])
                self.api.responses[endpoint] = (
                    http_response(503, "unavailable") if failed_read == "upstream"
                    else jobs_page(ci_post_jobs(), total=len(ci_post_jobs()) + 1))
                self.assert_refused("unreadable", endpoint, unreadable["id"])
                self.assert_post_reads(unreadable, newer=[newest])

    def test_ten_run_cap_refuses_when_eleventh_run_would_prove(self):
        """Rule 6: prove the ten-run cap stops; block reading or accepting the eleventh green."""
        rows = [
            ci_post_run(
                self.post_run["id"] + index + 1,
                created_at=f"2026-10-25T12:00:{10 - index:02d}Z")
            for index in range(11)
        ]
        examined = rows[:-1]
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post_runs(rows[0]["head_sha"], list(reversed(rows)), {
            **{row["id"]: skipped_jobs for row in examined},
            rows[-1]["id"]: self.jobs,
        })
        self.assert_no_full_run(
            examined, f"examined {len(examined)} of {len(rows)}", skipped_jobs[0]["conclusion"])

    def test_tenth_run_can_prove_without_reading_eleventh_red_run(self):
        """Rules 2/6: prove the tenth run is allowed; block a nine-run cap or eleventh read."""
        rows = [
            ci_post_run(
                self.post_run["id"] + index + 1,
                created_at=f"2026-10-25T12:00:{10 - index:02d}Z")
            for index in range(11)
        ]
        newer, proving, older = rows[:-2], rows[-2], rows[-1]
        older["conclusion"] = "failure"
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        failed_jobs = ci_post_jobs()
        failed_jobs[2]["conclusion"] = "failure"
        self.api.ci_post_runs(proving["head_sha"], list(reversed(rows)), {
            **{row["id"]: skipped_jobs for row in newer},
            proving["id"]: self.jobs, older["id"]: failed_jobs,
        })
        self.assert_green(proving, newer_skipped=newer)

    def test_duplicate_success_and_failure_for_same_matrix_name_refuses(self):
        """Rule 3: prove any duplicate failure blocks; block first-match or last-match success."""
        for index, original in enumerate(ci_post_jobs()):
            for failed_first in (False, True):
                with self.subTest(job=original["name"], failed_first=failed_first):
                    self.reset_ci_post()
                    failed_job = {
                        **self.jobs[index], "id": len(self.jobs) + 1, "conclusion": "failure"}
                    jobs = ([failed_job, *self.jobs] if failed_first
                            else [*self.jobs, failed_job])
                    self.api.ci_post(self.post_run, jobs)
                    self.assert_refused(
                        "newer-run-failed", self.post_run["id"], self.post_run["head_sha"],
                        self.post_run["event"], failed_job["name"], failed_job["conclusion"])
                    self.assert_post_reads(self.post_run)

    def test_docs_tip_red_run_blocks_built_green_ancestor(self):
        """F-1/a: prove docs run failure blocks; block release using a green ancestor."""
        docs, ancestor = self.docs_chain(1)
        failed = {**self.docs_runs[0], "conclusion": "failure"}
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post(failed, skipped_jobs)
        self.assert_docs_red(failed, read_shas=docs)

    def test_middle_docs_commit_red_run_stops_before_lower_commits(self):
        """F-1/b: prove tip-first checking stops at middle red; block tip-only approval."""
        docs, ancestor = self.docs_chain(3)
        failed = {**self.docs_runs[1], "conclusion": "failure"}
        self.api.ci_post_runs(failed["head_sha"], [failed], {})
        self.assert_docs_red(failed, read_shas=docs[:2])

    def test_docs_tip_without_run_cannot_use_built_green_ancestor(self):
        """F-1/c: prove missing docs evidence blocks; block inheriting ancestor success."""
        docs, ancestor = self.docs_chain(1)
        endpoint = ci_post_runs_api(docs[0])
        self.api.responses[endpoint] = runs_page([])
        self.assert_refused(f"docs-commit-no-run sha {docs[0]}")
        self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_docs_tip_only_wrong_branch_or_sha_runs_refuse(self):
        """F-1/c: prove exact main/sha filtering; block unrelated docs runs as evidence."""
        for field, value in (("head_branch", "release"), ("head_sha", OTHER_TREE)):
            with self.subTest(field=field):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                unrelated = {**self.docs_runs[0], field: value}
                self.api.ci_post_runs(docs[0], [unrelated], {})
                self.assert_refused(f"docs-commit-no-run sha {docs[0]}")
                self.assertEqual(self.post_calls(), [
                    (ci_post_runs_api(docs[0]), True, None)])

    def test_docs_tip_newest_unfinished_run_blocks_older_success(self):
        """F-1/d: prove newest docs status decides; block an older success rescuing it."""
        for state in ("queued", "in_progress", "waiting"):
            with self.subTest(state=state):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                newest = {**self.docs_runs[0], "status": state, "conclusion": None}
                older = ci_post_run(
                    newest["id"] + 100, sha=docs[0], created_at="2026-10-24T12:00:00Z")
                self.api.ci_post_runs(docs[0], [older, newest], {})
                self.assert_docs_red(newest, read_shas=docs)

    def test_docs_tip_completed_non_success_conclusions_refuse(self):
        """F-1/e: prove docs needs success; block neutral, skipped or absent conclusions."""
        for conclusion in ("cancelled", "timed_out", "neutral", "skipped", None,
                           "action_required"):
            with self.subTest(conclusion=conclusion):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                newest = {**self.docs_runs[0], "conclusion": conclusion}
                older = ci_post_run(
                    newest["id"] + 100, sha=docs[0], created_at="2026-10-24T12:00:00Z")
                self.api.ci_post_runs(docs[0], [older, newest], {})
                self.assert_docs_red(newest, read_shas=docs)

    def test_docs_tip_success_with_four_skipped_builds_needs_no_docs_jobs(self):
        """F-1/f: prove run-level docs success suffices; block reading its skipped jobs."""
        docs, ancestor = self.docs_chain(1)
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post(self.docs_runs[0], skipped_jobs)
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, (stdout, stderr))
        self.assertEqual(stderr, "")
        self.assert_post_reads(ancestor, docs=docs)
        self.assert_green_evidence(stdout, ancestor, skipped=docs)
        self.assertRegex(stdout, rf"\b{len(docs)} docs-only commit(?:\(s\)|s)? skipped\b")

    def test_docs_tip_newer_red_schedule_blocks_older_green_push(self):
        """F-1/g: prove all events decide docs health; block a push-only green rescue."""
        docs, ancestor = self.docs_chain(1)
        push = self.docs_runs[0]
        scheduled = ci_post_run(
            push["id"] + 1, sha=docs[0], event="schedule", conclusion="failure",
            created_at="2026-10-25T12:01:00Z")
        self.api.ci_post_runs(docs[0], [push, scheduled], {})
        self.assert_docs_red(scheduled, read_shas=docs)

    def test_docs_tip_unreadable_runs_name_endpoint_and_are_not_retried(self):
        """F-1/i: prove docs read errors fail closed; block retries or ancestor fallback."""
        endpoint = ci_post_runs_api(SHA)
        cases = [
            (OSError("connection refused"), "failed: connection refused"),
            (http_response(200, ""), "returned an empty payload"),
            (http_response(200, "not JSON"), "returned invalid JSON"),
            (http_response(403, '{"message":"API rate limit exceeded"}'), "rate limited"),
            (subprocess.CalledProcessError(
                1, ["gh", "api", endpoint],
                stderr="gh: API rate limit exceeded (HTTP 403)\n"), "rate limited"),
            *[(http_response(code, "unavailable"), f"upstream error {code}")
              for code in (500, 503, 599)],
        ]
        for response, diagnostic in cases:
            with self.subTest(response=repr(response), diagnostic=diagnostic):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                self.api.responses[endpoint] = response
                self.assert_refused("unreadable", endpoint, docs[0], diagnostic)
                self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_v1_7_0_docs_release_success_cannot_rescue_red_ancestor(self):
        """F-1/j: prove a healthy skipped-build tip leaves proof strict; block its rescue."""
        for build_failed in (False, True):
            with self.subTest(build_failed=build_failed):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
                self.api.ci_post(self.docs_runs[0], skipped_jobs)
                failed = {**ancestor, "conclusion": "failure", "event": "schedule"}
                failed_jobs = ci_post_jobs()
                evidence = [failed["id"], failed["head_sha"], failed["event"],
                            failed["conclusion"]]
                if build_failed:
                    failed_job = failed_jobs[2]
                    failed_job["conclusion"] = "failure"
                    evidence.extend([failed_job["name"], failed_job["conclusion"]])
                self.api.ci_post(failed, failed_jobs)
                self.assert_refused(
                    "newer-run-failed" if build_failed else "not-success", *evidence)
                self.assert_post_reads(failed, docs=docs)

    def test_phase_two_receipt_cannot_rescue_red_docs_tip(self):
        """F-1/k: prove phase two checks docs health; block receipt-only release approval."""
        self.enable_watcher({row["context"]: row["state"] for row in successes()})
        docs, ancestor = self.docs_chain(1)
        failed = {**self.docs_runs[0], "conclusion": "failure"}
        self.api.ci_post_runs(docs[0], [failed], {})
        self.assert_docs_red(failed, read_shas=docs)
        self.proof.assert_called_once_with(REPO, docs[0])
        self.assertEqual(self.api.calls, [
            (MAIN_API, False, ".commit.sha"),
            (ci_post_runs_api(docs[0]), True, None),
        ])

    def test_release_bump_path_cannot_rescue_docs_tip_without_run(self):
        """F-1/k: prove bump authorization still checks docs; block its no-run exemption."""
        self.api.release_bump()
        docs, ancestor = self.docs_chain(1)
        self.api.ci_post_runs(docs[0], [], {})
        self.assert_refused(f"docs-commit-no-run sha {docs[0]}")
        self.assertEqual(self.post_calls(), [
            (ci_post_runs_api(docs[0]), True, None)])
        self.assertIn((RUNS_API, True, None), self.api.calls)
        self.assertIn((STATUSES_API, True, None), self.api.calls)

    def test_docs_tip_newest_rerun_decides_in_both_response_orders(self):
        """F-1/l: prove newest docs rerun decides; block first-row, largest-id or any-green."""
        for newest_conclusion in ("success", "failure"):
            for newest_first in (False, True):
                with self.subTest(conclusion=newest_conclusion, newest_first=newest_first):
                    self.reset_ci_post()
                    docs, ancestor = self.docs_chain(1)
                    newest = {**self.docs_runs[0], "conclusion": newest_conclusion}
                    older = ci_post_run(
                        newest["id"] + 100, sha=docs[0],
                        conclusion="failure" if newest_conclusion == "success" else "success",
                        created_at="2026-10-24T12:00:00Z")
                    rows = [newest, older] if newest_first else [older, newest]
                    self.api.ci_post_runs(docs[0], rows, {})
                    if newest_conclusion == "success":
                        self.assert_green(ancestor, skipped=docs)
                    else:
                        self.assert_docs_red(newest, read_shas=docs)

    def test_docs_tip_equal_created_at_uses_higher_run_id_in_both_orders(self):
        """F-1/newest: prove id breaks timestamp ties; block list order or an older veto."""
        for newest_conclusion in ("success", "failure"):
            for newest_first in (False, True):
                with self.subTest(conclusion=newest_conclusion, newest_first=newest_first):
                    self.reset_ci_post()
                    docs, ancestor = self.docs_chain(1)
                    older = {**self.docs_runs[0],
                             "conclusion": ("failure" if newest_conclusion == "success"
                                            else "success")}
                    newest = {**older, "id": older["id"] + 1, "conclusion": newest_conclusion}
                    rows = [newest, older] if newest_first else [older, newest]
                    self.api.ci_post_runs(docs[0], rows, {})
                    if newest_conclusion == "success":
                        self.assert_green(ancestor, skipped=docs)
                    else:
                        self.assert_docs_red(newest, read_shas=docs)

    def test_lowest_docs_chain_commit_with_one_source_path_needs_full_builds(self):
        """F-1/m: prove one outside path ends docs walking; block skipped builds as proof."""
        docs, ancestor = self.docs_chain(3)
        proof = self.docs_runs[-1]
        parent, paths = self.history.responses[proof["head_sha"]]
        self.history.responses[proof["head_sha"]] = (
            parent, [*paths, "src/lowest_docs_boundary.cpp"])
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post(proof, skipped_jobs)
        self.assert_no_full_run(
            [proof], proof["event"], skipped_jobs[0]["conclusion"], docs=docs[:-1])
        self.assertEqual(self.history.calls, docs)

    def test_docs_tip_paginated_runs_choose_newest_matching_success_without_jobs(self):
        """F-1/runs: prove docs pages/filtering precede judgment; block first-page red veto."""
        docs, ancestor = self.docs_chain(1)
        newest = self.docs_runs[0]
        older = ci_post_run(
            newest["id"] + 200, sha=docs[0], conclusion="failure",
            created_at="2026-10-24T12:00:00Z")
        unrelated = [
            {**newest, "id": newest["id"] + index + 1, "head_branch": "release",
             "created_at": "2026-10-26T12:00:00Z"}
            for index in range(99)
        ]
        rows = [older, *unrelated, newest]
        endpoint = ci_post_runs_api(docs[0])
        self.api.responses[endpoint] = (
            runs_page(rows[:100], total=len(rows), headers={
                "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
            + runs_page(rows[100:], total=len(rows), headers={
                "Link": f'<https://api.github.com/{endpoint}&page=1>; rel="prev"'}))
        self.assert_green(ancestor, skipped=docs)

    def test_docs_tip_incomplete_runs_pages_refuse_before_proof_reads(self):
        """F-1/runs: prove complete docs pagination is required; block visible-green rescue."""
        for failure in ("total", "unread-link", "later-page"):
            with self.subTest(failure=failure):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                endpoint = ci_post_runs_api(docs[0])
                row = self.docs_runs[0]
                if failure == "total":
                    response = runs_page([row], total=2)
                else:
                    response = runs_page(
                        [row], total=2 if failure == "later-page" else 1, headers={
                            "Link": f'<https://api.github.com/{endpoint}&page=2>; rel="next"'})
                    if failure == "later-page":
                        response += http_response(503, "unavailable")
                diagnostic = ("upstream error 503" if failure == "later-page"
                              else "incomplete workflow_runs pagination")
                self.api.responses[endpoint] = response
                self.assert_refused("unreadable", endpoint, docs[0], diagnostic)
                self.assertEqual(self.post_calls(), [(endpoint, True, None)])

    def test_docs_tip_runs_at_or_beyond_1000_limit_refuse(self):
        """F-1/runs: prove docs shares the runs cap; block a capped green list as evidence."""
        for total in (1000, 1001):
            with self.subTest(total=total):
                self.reset_ci_post()
                docs, ancestor = self.docs_chain(1)
                endpoint = ci_post_runs_api(docs[0])
                rows = [{**self.docs_runs[0], "id": self.docs_runs[0]["id"] + index}
                        for index in range(1000)]
                self.api.responses[endpoint] = "".join(
                    runs_page(rows[start:start + 100], total=total, headers={
                        "Link": f'<https://api.github.com/{endpoint}&page={start // 100 + 2}>; '
                        'rel="next"'} if start < 900 else None)
                    for start in range(0, len(rows), 100))
                self.assert_refused("unreadable", endpoint, docs[0], "1000")
                self.assertEqual(self.post_calls(), [(endpoint, True, None)])


    def test_later_parent_failure_only_blocks_after_release_sha_flip(self):
        """Round 10/1/4/6a: prove SHA decides; block cross-commit vetoes or a dropped filter."""
        for run_sha in (OTHER_HEAD, SHA):
            with self.subTest(run_sha=run_sha):
                self.reset_ci_post()
                parent = ci_post_run(
                    self.post_run["id"] + 1, sha=OTHER_HEAD, conclusion="failure",
                    created_at="2026-10-25T13:00:00Z")
                failed_jobs = ci_post_jobs()
                failed_job = failed_jobs[2]
                failed_job["conclusion"] = "failure"
                self.api.ci_post(parent, failed_jobs)
                newer = {**parent, "head_sha": run_sha}
                self.api.ci_post_runs(SHA, [self.post_run, newer], {
                    self.post_run["id"]: self.jobs, newer["id"]: failed_jobs})
                if run_sha == OTHER_HEAD:
                    self.assert_green(self.post_run)
                else:
                    self.assert_refused(
                        "newer-run-failed", newer["id"], newer["head_sha"],
                        newer["event"], failed_job["name"], failed_job["conclusion"])
                    self.assert_post_reads(newer)
                self.assertNotIn(
                    (ci_post_runs_api(parent["head_sha"]), True, None), self.api.calls)
                self.assertEqual(self.history.calls, [self.post_run["head_sha"]])

    def test_later_parent_in_progress_only_blocks_after_release_sha_flip(self):
        """Round 10/2/6c: prove unfinished runs are SHA-scoped; block cross-commit vetoes."""
        for run_sha in (OTHER_HEAD, SHA):
            with self.subTest(run_sha=run_sha):
                self.reset_ci_post()
                parent = ci_post_run(
                    self.post_run["id"] + 1, sha=OTHER_HEAD,
                    state="in_progress", conclusion=None,
                    created_at="2026-10-25T13:00:00Z")
                newer = {**parent, "head_sha": run_sha}
                self.api.ci_post_runs(SHA, [newer, self.post_run], {
                    self.post_run["id"]: self.jobs})
                if run_sha == OTHER_HEAD:
                    self.assert_green(self.post_run)
                else:
                    self.assert_refused(
                        "run-in-progress", newer["id"], newer["head_sha"], newer["status"])
                    self.assertEqual(self.post_calls(), [
                        (ci_post_runs_api(self.post_run["head_sha"]), True, None)])
                self.assertEqual(self.history.calls, [self.post_run["head_sha"]])

    def test_docs_chain_later_parent_failure_only_blocks_after_proof_sha_flip(self):
        """Round 10/3/5: prove SHA flips both failure verdicts; block time ordering across commits."""
        for failed_build in (False, True):
            for on_proof in (False, True):
                with self.subTest(failed_build=failed_build, on_proof=on_proof):
                    self.reset_ci_post()
                    docs, ancestor = self.docs_chain(1)
                    parent_sha = self.history.responses[ancestor["head_sha"]][0]
                    parent = ci_post_run(
                        ancestor["id"] + 1, sha=parent_sha, conclusion="failure",
                        created_at="2026-10-25T13:00:00Z")
                    jobs = ci_post_jobs()
                    failed_job = jobs[2]
                    if failed_build:
                        failed_job["conclusion"] = "failure"
                    self.api.ci_post(parent, jobs)
                    newer = {**parent, "head_sha": (
                        ancestor["head_sha"] if on_proof else parent["head_sha"])}
                    self.api.ci_post_runs(docs[0], [newer, self.docs_runs[0]], {})
                    self.api.ci_post_runs(ancestor["head_sha"], [ancestor, newer], {
                        ancestor["id"]: ci_post_jobs(), newer["id"]: jobs})
                    if on_proof:
                        reason = "newer-run-failed" if failed_build else "not-success"
                        evidence = ((failed_job["name"], failed_job["conclusion"])
                                    if failed_build else (newer["conclusion"],))
                        self.assert_refused(
                            reason, newer["id"], newer["head_sha"], newer["event"], *evidence)
                        self.assert_post_reads(newer, docs=docs)
                    else:
                        self.assert_green(ancestor, skipped=docs)
                    self.assertNotIn(
                        (ci_post_runs_api(parent_sha), True, None), self.api.calls)
                    self.assertEqual(self.history.calls, [*docs, ancestor["head_sha"]])

    def test_later_run_only_vetoes_middle_docs_commit_after_sha_flip(self):
        """Round 10/6b/6c: prove middle-docs SHA decides; block outside vetoes or red-docs rescue."""
        for state, conclusion in (("completed", "failure"), ("in_progress", None)):
            for on_docs in (False, True):
                with self.subTest(state=state, on_docs=on_docs):
                    self.reset_ci_post()
                    docs, ancestor = self.docs_chain(3)
                    parent_sha = self.history.responses[ancestor["head_sha"]][0]
                    outside = ci_post_run(
                        ancestor["id"] + 1, sha=parent_sha, state=state,
                        conclusion=conclusion, created_at="2026-10-25T13:00:00Z")
                    newer = {**outside, "head_sha": (
                        docs[1] if on_docs else outside["head_sha"])}
                    self.api.ci_post_runs(docs[1], [self.docs_runs[1], newer], {})
                    if on_docs:
                        self.assert_docs_red(newer, read_shas=docs[:2])
                    else:
                        self.assert_green(ancestor, skipped=docs)
                        self.assertEqual(self.history.calls, [*docs, ancestor["head_sha"]])


    def test_newest_non_success_proof_conclusions_block_older_green_for_each_job_shape(self):
        """Round 12/2/3: non-success blocks unless both run and all four legs are skipped."""
        for conclusion in ("neutral", "action_required", "stale", "startup_failure", "skipped", None):
            job_shapes = (("success", "absent") if conclusion == "skipped"
                          else ("success", "skipped", "absent"))
            for job_shape in job_shapes:
                with self.subTest(conclusion=conclusion, job_shape=job_shape):
                    self.reset_ci_post()
                    newest = ci_post_run(
                        self.post_run["id"] + 1, conclusion=conclusion,
                        created_at="2026-10-25T12:01:00Z")
                    newer_jobs = ([] if job_shape == "absent" else [
                        {**job, "conclusion": job_shape} for job in ci_post_jobs()])
                    self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                        newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                    stderr = self.assert_refused(
                        f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                        f"conclusion {newest['conclusion']}")
                    if conclusion == "skipped":
                        contradictions = ([f"missing job {job['name']}" for job in ci_post_jobs()]
                                          if job_shape == "absent" else [
                                              job["name"] for job in newer_jobs])
                        self.assertTrue(any(name in stderr for name in contradictions), stderr)
                    self.assert_post_reads(newest)

    def test_non_success_proof_conclusions_after_skipped_newest_block_older_green(self):
        """Round 12/3: after success with skipped legs, each non-success middle run blocks."""
        for conclusion in ("neutral", "action_required", "stale", "startup_failure", None):
            with self.subTest(conclusion=conclusion):
                self.reset_ci_post()
                blocked = ci_post_run(
                    self.post_run["id"] + 1, conclusion=conclusion,
                    created_at="2026-10-25T12:01:00Z")
                newest = ci_post_run(
                    self.post_run["id"] + 2, conclusion="success",
                    created_at="2026-10-25T12:02:00Z")
                skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
                self.api.ci_post_runs(blocked["head_sha"], [self.post_run, blocked, newest], {
                    newest["id"]: skipped_jobs, blocked["id"]: skipped_jobs,
                    self.post_run["id"]: self.jobs})
                self.assert_refused(
                    f"not-success {blocked['id']}", blocked["head_sha"], blocked["event"],
                    f"conclusion {blocked['conclusion']}")
                self.assert_post_reads(blocked, newer=[newest])

    def test_neutral_run_refuses_and_failed_build_keeps_priority(self):
        """Round 11/3: block neutral fallthrough; a failed build must outrank not-success."""
        for failed_build in (False, True):
            with self.subTest(failed_build=failed_build):
                self.reset_ci_post()
                newest = ci_post_run(
                    self.post_run["id"] + 1, conclusion="neutral",
                    created_at="2026-10-25T12:01:00Z")
                newer_jobs = ci_post_jobs()
                failed_job = newer_jobs[2]
                if failed_build:
                    failed_job["conclusion"] = "failure"
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                    newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                if failed_build:
                    self.assert_refused(
                        f"newer-run-failed {newest['id']}", newest["head_sha"],
                        newest["event"], failed_job["name"], failed_job["conclusion"])
                else:
                    self.assert_refused(
                        f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                        f"conclusion {newest['conclusion']}")
                self.assert_post_reads(newest)

    def test_skipped_run_with_green_or_partly_green_legs_refuses_older_full_green(self):
        """Round 12/2: skipped with green legs is contradictory and cannot reach older green."""
        for skipped_count in (0, 1):
            with self.subTest(skipped_count=skipped_count):
                self.reset_ci_post()
                newest = ci_post_run(
                    self.post_run["id"] + 1, conclusion="skipped",
                    created_at="2026-10-25T12:01:00Z")
                newer_jobs = ci_post_jobs()
                for job in newer_jobs[:skipped_count]:
                    job["conclusion"] = "skipped"
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                    newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                stderr = self.assert_refused(
                    f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                    f"conclusion {newest['conclusion']}")
                self.assertTrue(
                    any(job["name"] in stderr for job in newer_jobs[skipped_count:]), stderr)
                self.assert_post_reads(newest)

    def test_skipped_run_with_uncertain_or_missing_leg_refuses_older_full_green(self):
        """Round 12/2: three skipped legs cannot hide a contradictory or absent fourth leg."""
        for index, original in enumerate(ci_post_jobs()):
            for conclusion in ("neutral", "action_required", None, "unknown", "missing"):
                with self.subTest(job=original["name"], conclusion=conclusion):
                    self.reset_ci_post()
                    newest = ci_post_run(
                        self.post_run["id"] + 1, conclusion="skipped",
                        created_at="2026-10-25T12:01:00Z")
                    newer_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
                    if conclusion == "missing":
                        newer_jobs.pop(index)
                    else:
                        newer_jobs[index]["conclusion"] = conclusion
                    self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                        newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                    evidence = (f"missing job {original['name']}" if conclusion == "missing"
                                else original["name"])
                    self.assert_refused(
                        f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                        f"conclusion {newest['conclusion']}", evidence)
                    self.assert_post_reads(newest)

    def test_failed_cancelled_or_timed_out_run_with_absent_legs_refuses_older_green(self):
        """Round 12/3: aggregate failure blocks older green even without any build legs."""
        for conclusion in ("failure", "cancelled", "timed_out"):
            with self.subTest(conclusion=conclusion):
                self.reset_ci_post()
                newest = ci_post_run(
                    self.post_run["id"] + 1, conclusion=conclusion,
                    created_at="2026-10-25T12:01:00Z")
                self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                    newest["id"]: [], self.post_run["id"]: self.jobs})
                self.assert_refused(
                    f"not-success {newest['id']}", newest["head_sha"], newest["event"],
                    f"conclusion {newest['conclusion']}")
                self.assert_post_reads(newest)

    def test_fully_skipped_middle_run_after_skipped_newest_allows_older_full_green(self):
        """Round 12/2: a skipped run with four skips falls through even as a middle run."""
        middle = ci_post_run(
            self.post_run["id"] + 1, conclusion="skipped",
            created_at="2026-10-25T12:01:00Z")
        newest = ci_post_run(
            self.post_run["id"] + 2, conclusion="success",
            created_at="2026-10-25T12:02:00Z")
        skipped_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        self.api.ci_post_runs(newest["head_sha"], [middle, self.post_run, newest], {
            newest["id"]: skipped_jobs, middle["id"]: skipped_jobs,
            self.post_run["id"]: self.jobs})
        self.assert_green(self.post_run, newer_skipped=[newest, middle])

    def assert_newer_success_with_invalid_leg_and_skips_refuses(self, conclusion):
        newest = ci_post_run(
            self.post_run["id"] + 1, conclusion="success",
            created_at="2026-10-25T12:01:00Z")
        newer_jobs = [{**job, "conclusion": "skipped"} for job in ci_post_jobs()]
        invalid = newer_jobs[2]
        if conclusion == "missing":
            newer_jobs.pop(2)
            evidence = f"missing job {invalid['name']}"
        else:
            invalid["conclusion"] = conclusion
            evidence = f"job {invalid['name']}={conclusion}"
        self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
            newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
        self.assert_refused(
            f"not-success {newest['id']}", newest["head_sha"], newest["event"], evidence)
        self.assert_post_reads(newest)

    def test_newer_success_run_with_neutral_leg_and_skips_refuses(self):
        """Round 12/4: a neutral leg blocks older green even beside three skipped legs."""
        self.assert_newer_success_with_invalid_leg_and_skips_refuses("neutral")

    def test_newer_success_run_with_null_leg_and_skips_refuses(self):
        """Round 12/4: a null leg blocks older green and is reported as None."""
        self.assert_newer_success_with_invalid_leg_and_skips_refuses(None)

    def test_newer_success_run_with_missing_leg_and_skips_refuses(self):
        """Round 12/4: three present skipped legs cannot hide a missing required build."""
        self.assert_newer_success_with_invalid_leg_and_skips_refuses("missing")

    def test_newer_success_run_with_unknown_leg_and_skips_refuses(self):
        """Round 12/4: an unknown leg cannot be treated as skipped to reach older green."""
        self.assert_newer_success_with_invalid_leg_and_skips_refuses("unknown")

    def test_failed_build_row_keeps_priority_over_every_run_conclusion(self):
        """Round 12/1: any failed build row outranks the run conclusion and older green."""
        for run_conclusion in (
                "success", "skipped", "neutral", "action_required", "stale",
                "startup_failure", None, "failure", "cancelled", "timed_out", "unknown"):
            for job_conclusion in ("failure", "cancelled", "timed_out"):
                for original in ci_post_jobs():
                    with self.subTest(run_conclusion=run_conclusion, job=original["name"],
                                      job_conclusion=job_conclusion):
                        self.reset_ci_post()
                        newest = ci_post_run(
                            self.post_run["id"] + 1, conclusion=run_conclusion,
                            created_at="2026-10-25T12:01:00Z")
                        newer_jobs = ci_post_jobs()
                        failed_job = {**original, "id": len(newer_jobs) + 1,
                                      "conclusion": job_conclusion}
                        # A later duplicate must still veto four earlier green build rows.
                        newer_jobs.append(failed_job)
                        self.api.ci_post_runs(newest["head_sha"], [self.post_run, newest], {
                            newest["id"]: newer_jobs, self.post_run["id"]: self.jobs})
                        self.assert_refused(
                            f"newer-run-failed {newest['id']}", newest["head_sha"],
                            newest["event"], failed_job["name"], failed_job["conclusion"])
                        self.assert_post_reads(newest)


if __name__ == "__main__":
    unittest.main()
