#!/usr/bin/env python3
"""Check corpus decisions, real git histories, and local HTTP responses."""
from contextlib import contextmanager, ExitStack
import copy
from datetime import datetime, timedelta
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

import check_corpus_gitlink_result as gate


FIXTURES = Path(__file__).parent / "fixtures" / "corpus_gate"
REAL = FIXTURES / "real"
RECORDED = json.loads(
    (REAL / "check-runs-engine-14d7d9b3-realshape.json").read_text())
# No real corpus-validate run exists yet; the extra row is explicitly synthetic.
SYNTHETIC = json.loads(
    (FIXTURES / "check-runs-corpus-synthetic.json").read_text())
SCRIPT = Path(gate.__file__).resolve()
OLD = hashlib.sha1(b"previous corpus revision").hexdigest()


def corpus_row(response):
    return next(row for row in response["check_runs"]
                if row["name"] == "corpus-validate")


NEW = corpus_row(SYNTHETIC)["head_sha"]


def success():
    return copy.deepcopy(SYNTHETIC)


def later(row):
    completed = datetime.fromisoformat(row["completed_at"].replace("Z", "+00:00"))
    return (completed + timedelta(seconds=1)).isoformat()


def git(repo, *arguments):
    return subprocess.run(
        ["git", "-C", str(repo), "-c", "user.name=Corpus tests",
         "-c", "user.email=corpus-tests@example.invalid",
         "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
         *arguments], check=True, capture_output=True, text=True,
    ).stdout.strip()


def commit_pin(repo, pin, message):
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{pin},corpus")
    git(repo, "commit", "-q", "-m", message)
    return git(repo, "rev-parse", "HEAD")


def send_response(handler, response, status=200, headers=(), short=False):
    body = json.dumps(response).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body) + (len(body) if short else 0)))
    for name, value in headers:
        handler.send_header(name, value)
    handler.end_headers()
    handler.wfile.write(body)
    handler.close_connection = True


@contextmanager
def serve(reply):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append((self.path, self.headers))
            reply(self)

        def log_message(self, *arguments):
            pass

    with HTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever,
                                  kwargs={"poll_interval": 0.01}, daemon=True)
        thread.start()
        host, port = server.server_address
        try:
            yield f"http://{host}:{port}", requests
        finally:
            server.shutdown()
            thread.join()


@contextmanager
def closed_api():
    # Keep the port bound without listening so another process cannot take it.
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        host, port = reserved.getsockname()
        yield f"http://{host}:{port}"


class CorpusCheckResultTests(unittest.TestCase):
    def test_exact_success_and_single_field_refusals(self):
        good = success()
        self.assertEqual(good["check_runs"][:-1], RECORDED["check_runs"])
        self.assertEqual(good["total_count"], len(good["check_runs"]))
        self.assertEqual(set(corpus_row(good)), set(RECORDED["check_runs"][0]))
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
                corpus_row(response)[field] = value
                code, reason = gate.check_result(response, NEW)
                self.assertNotEqual(code, 0)
                self.assertIn(message, reason)
        response = success()
        del corpus_row(response)["completed_at"]
        code, reason = gate.check_result(response, NEW)
        self.assertEqual(code, 1)
        self.assertIn("completed_at", reason)

    def test_another_app_cannot_supply_the_check(self):
        response = success()
        corpus_row(response)["app"]["id"] += 1
        code, reason = gate.check_result(response, NEW)
        self.assertEqual(code, 1)
        self.assertIn("app", reason)

    def test_recorded_empty_answers_mean_missing(self):
        for filename in ("check-runs-corpus-adef1c63.json",
                         "check-runs-corpus-8425ca82.json"):
            with self.subTest(filename=filename):
                response = json.loads((REAL / filename).read_text())
                code, reason = gate.check_result(response, NEW)
                self.assertEqual(code, 1)
                self.assertIn("missing", reason)

    def test_newest_completion_decides_in_either_api_order(self):
        response = success()
        old = corpus_row(response)
        newer = copy.deepcopy(old)
        newer["completed_at"] = later(old)
        newer["conclusion"] = "failure"
        unrelated = [row for row in response["check_runs"] if row is not old]
        for rows in ([old, newer], [newer, old]):
            code, reason = gate.check_result(
                {"total_count": len(unrelated + rows),
                 "check_runs": unrelated + rows}, NEW)
            self.assertEqual(code, 1)
            self.assertIn("failure", reason)

    def test_newer_success_over_older_failure_in_either_api_order(self):
        response = success()
        old = corpus_row(response)
        newer = copy.deepcopy(old)
        newer["completed_at"] = later(old)
        old["conclusion"] = "failure"
        unrelated = [row for row in response["check_runs"] if row is not old]
        for rows in ([old, newer], [newer, old]):
            with self.subTest(order=[row["conclusion"] for row in rows]):
                code, reason = gate.check_result(
                    {"total_count": len(unrelated + rows),
                     "check_runs": unrelated + rows}, NEW)
                self.assertEqual(code, 0)
                self.assertIn("completed success", reason)

    def test_pending_rerun_does_not_reuse_an_old_success(self):
        response = success()
        pending = copy.deepcopy(corpus_row(response))
        pending.update(status="in_progress", conclusion=None, completed_at=None)
        response["check_runs"].append(pending)
        response["total_count"] = len(response["check_runs"])
        self.assertEqual(gate.check_result(response, NEW)[0], 1)

    def test_unreadable_or_truncated_api_result_cannot_pass(self):
        for response in ({}, {"total_count": 1, "check_runs": []},
                         {"total_count": 101, "check_runs": []}):
            with self.subTest(response=response):
                self.assertEqual(gate.check_result(response, NEW)[0], 2)

    def test_gitlink_requires_a_submodule_entry(self):
        self.assertEqual(gate.parse_gitlink(
            f"160000 commit {OLD}\tcorpus\n"), OLD)
        for text in ("", f"100644 blob {OLD}\tcorpus\n",
                     f"160000 commit {OLD}\tother\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gate.parse_gitlink(text)


class CorpusGitlinkIntegrationTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.directory = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="corpus-check-")))
        self.repo = self.directory / "repository"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        self.pin_a = hashlib.sha1(b"branch point corpus").hexdigest()
        self.pin_b = hashlib.sha1(b"base branch corpus").hexdigest()
        self.point = commit_pin(self.repo, self.pin_a, "Branch point")
        self.base = commit_pin(self.repo, self.pin_b, "Base moves corpus")
        git(self.repo, "checkout", "-q", "--detach", self.point)
        git(self.repo, "commit", "-q", "--allow-empty", "-m", "Branch keeps corpus")
        self.unchanged = git(self.repo, "rev-parse", "HEAD")
        self.head = commit_pin(self.repo, NEW, "Branch moves corpus")

    def invoke(self, api_url, *, head=None, cwd=None, explicit_repo=True):
        arguments = [
            sys.executable, "-B", str(SCRIPT),
            "--base", self.base, "--head", head or self.head,
        ]
        if api_url is not None:
            arguments.extend(["--api-url", api_url])
        if explicit_repo:
            arguments.extend(["--repo", str(self.repo)])
        return subprocess.run(arguments, cwd=cwd or self.repo,
                              capture_output=True, text=True)

    def assert_unreadable(self, result, exception=None):
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertTrue(result.stdout.startswith("corpus validation unreadable: "))
        self.assertNotIn("Traceback", result.stdout)
        if exception:
            self.assertIn(f"{exception}:", result.stdout)

    def test_unchanged_gitlink_never_calls_the_api(self):
        with closed_api() as api_url:
            result = self.invoke(api_url, head=self.unchanged)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"gitlink unchanged: {self.pin_a}", result.stdout)

    def test_changed_gitlink_reads_exactly_the_new_commit_once(self):
        with serve(lambda handler: send_response(handler, success())) as (url, requests):
            result = self.invoke(url)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([path for path, _ in requests], [
            f"/repos/{gate.CORPUS_REPOSITORY}/commits/{NEW}/check-runs"
            "?filter=all&check_name=corpus-validate&per_page=100"])
        self.assertIn(NEW, result.stdout)

    def test_unrelated_histories_are_unreadable(self):
        git(self.repo, "checkout", "-q", "--orphan", "unrelated")
        git(self.repo, "read-tree", "--empty")
        unrelated = commit_pin(self.repo, self.pin_b, "Unrelated history")
        result = self.invoke(None, head=unrelated, explicit_repo=False)
        self.assert_unreadable(result)
        self.assertIn("merge base", result.stdout)

    def test_explicit_repo_works_outside_any_repository(self):
        outside = self.directory / "plain-directory"
        outside.mkdir()
        with serve(lambda handler: send_response(handler, success())) as (url, requests):
            inside = self.invoke(url, explicit_repo=False)
            external = self.invoke(url, cwd=outside)
        self.assertEqual(inside.returncode, 0, inside.stdout + inside.stderr)
        self.assertEqual(external.returncode, inside.returncode,
                         external.stdout + external.stderr)
        self.assertEqual(external.stdout, inside.stdout)
        self.assertEqual(external.stderr, inside.stderr)
        self.assertEqual(len(requests), len((inside, external)))

    def test_request_path_and_headers_with_and_without_token(self):
        token = hashlib.sha256(b"local HTTP authorization fixture").hexdigest()
        with serve(lambda handler: send_response(handler, success())) as (url, requests):
            for present in (False, True):
                with self.subTest(token_present=present), mock.patch.dict(os.environ):
                    if present:
                        os.environ["GH_TOKEN"] = token
                    else:
                        os.environ.pop("GH_TOKEN", None)
                    response = gate.read_checks(
                        gate.CORPUS_REPOSITORY, NEW, api_url=url)
                    self.assertEqual(response, success())
                    path, headers = requests[-1]
                    self.assertEqual(path,
                                     f"/repos/{gate.CORPUS_REPOSITORY}/commits/{NEW}"
                                     "/check-runs?filter=all&check_name=corpus-validate"
                                     "&per_page=100")
                    self.assertEqual(headers.get("Accept"), "application/vnd.github+json")
                    self.assertEqual(headers.get("X-GitHub-Api-Version"), "2022-11-28")
                    self.assertEqual(headers.get("User-Agent"),
                                     "pineforge-corpus-gitlink-check")
                    if present:
                        self.assertTrue(headers.get("Authorization") == f"Bearer {token}",
                                        "authorization header differs")
                    else:
                        self.assertNotIn("Authorization", headers)
            self.assertEqual(len(requests), len((False, True)))

    def test_short_http_body_is_unreadable(self):
        with serve(lambda handler: send_response(
                handler, success(), short=True)) as (url, requests):
            result = self.invoke(url)
        self.assert_unreadable(result, "IncompleteRead")
        self.assertEqual(len(requests), 1)

    def test_next_page_is_unreadable(self):
        def reply(handler):
            send_response(handler, success(), headers=(
                ("Link", f'<http://{handler.headers["Host"]}/next>; rel="next"'),))

        with serve(reply) as (url, requests):
            result = self.invoke(url)
        self.assert_unreadable(result, "ValueError")
        self.assertIn("next page", result.stdout)
        self.assertEqual(len(requests), 1)

    def test_unexpected_http_statuses_are_unreadable(self):
        cases = (201, 403, 500)
        statuses = iter(cases)
        with serve(lambda handler: send_response(
                handler, success(), status=next(statuses))) as (url, requests):
            for status in cases:
                with self.subTest(status=status):
                    result = self.invoke(url)
                    self.assert_unreadable(result, "ValueError" if status == 201 else "HTTPError")
                    self.assertIn(str(status), result.stdout)
        self.assertEqual(len(requests), len(cases))

    def test_redirect_to_another_origin_never_sends_a_request(self):
        token = hashlib.sha256(b"redirect authorization fixture").hexdigest()
        with serve(lambda handler: send_response(handler, success())) as (other, received):
            with serve(lambda handler: send_response(
                    handler, {}, status=302,
                    headers=(("Location", other + "/redirected"),))) as (url, requests):
                with mock.patch.dict(os.environ, {"GH_TOKEN": token}):
                    result = self.invoke(url)
        self.assert_unreadable(result, "ValueError")
        self.assertIn("redirect", result.stdout)
        self.assertEqual(len(requests), 1)
        self.assertEqual(received, [])
        self.assertTrue(requests[0][1].get("Authorization") == f"Bearer {token}",
                        "initial authorization header differs")

    def test_same_origin_redirect_retains_authorization(self):
        token = hashlib.sha256(b"same origin authorization fixture").hexdigest()
        renamed = "/repos/renamed/corpus/check-runs"

        def reply(handler):
            if handler.path == renamed:
                send_response(handler, success())
            else:
                send_response(handler, {}, status=302, headers=(("Location", renamed),))

        with serve(reply) as (url, requests):
            with mock.patch.dict(os.environ, {"GH_TOKEN": token}):
                result = self.invoke(url)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([path for path, _ in requests], [
            f"/repos/{gate.CORPUS_REPOSITORY}/commits/{NEW}/check-runs"
            "?filter=all&check_name=corpus-validate&per_page=100", renamed])
        for _, headers in requests:
            self.assertTrue(headers.get("Authorization") == f"Bearer {token}",
                            "redirect authorization header differs")


if __name__ == "__main__":
    unittest.main(verbosity=2)
