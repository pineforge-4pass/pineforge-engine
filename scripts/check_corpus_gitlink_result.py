#!/usr/bin/env python3
"""Require successful corpus validation on the exact proposed corpus gitlink.

Exit 0: unchanged gitlink or completed success; 1: validation refused;
2: the gitlink or the complete check-runs response could not be read.
"""

import argparse
from datetime import datetime
import json
import os
import re
import subprocess
import urllib.error
import urllib.request


CORPUS_REPOSITORY = "pineforge-4pass/pineforge-corpus"
CHECK_NAME = "corpus-validate"
GITHUB_ACTIONS_APP_ID = 15368
SHA = re.compile(r"[0-9a-f]{40}")


def parse_gitlink(text: str) -> str:
    """Accept exactly the corpus submodule entry, never a blob or a tree."""
    match = re.fullmatch(r"160000 commit ([0-9a-f]{40})\tcorpus\n?", text)
    if match is None:
        raise ValueError("corpus: expected one 160000 commit gitlink")
    return match.group(1)


def gitlink(revision: str) -> str:
    if not SHA.fullmatch(revision):
        raise ValueError("engine revision must be a full commit SHA")
    result = subprocess.run(
        ["git", "--no-optional-locks", "ls-tree", revision, "--", "corpus"],
        check=True, capture_output=True, text=True,
    )
    return parse_gitlink(result.stdout)


def read_checks(repository: str, commit: str) -> dict:
    """Read one complete page, retaining reruns rather than only the latest."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("unreadable corpus repository")
    if not SHA.fullmatch(commit):
        raise ValueError("unreadable corpus commit SHA")
    url = (f"https://api.github.com/repos/{repository}/commits/{commit}/check-runs"
           f"?filter=all&check_name={CHECK_NAME}&per_page=100")
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "pineforge-corpus-gitlink-check",
    }
    token = os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=30) as response:
        if re.search(r'rel="?next"?', response.headers.get("Link", "")):
            raise ValueError("truncated check-runs response (next page exists)")
        return json.load(response)


def _completion(value: object) -> datetime:
    if not isinstance(value, str):
        raise ValueError("completed_at is missing or unreadable")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError(f"completed_at is unreadable: {value!r}") from error
    if parsed.tzinfo is None:
        raise ValueError(f"completed_at has no timezone: {value!r}")
    return parsed


def check_result(response: object, commit: str) -> tuple[int, str]:
    """Decide from the response bytes, with no fallback to another commit."""
    if not isinstance(response, dict):
        return 2, "unreadable check-runs response"
    total, rows = response.get("total_count"), response.get("check_runs")
    if (type(total) is not int or total < 0 or not isinstance(rows, list)
            or any(not isinstance(row, dict) for row in rows)):
        return 2, "unreadable total_count or check_runs"
    if total > 100 or len(rows) != total:
        return 2, "truncated or inconsistent check-runs response"
    named = [row for row in rows if row.get("name") == CHECK_NAME]
    if not named:
        return 1, f"{CHECK_NAME} missing on {commit}"
    # Refuse inconsistent identities rather than letting an unrelated success
    # hide a response that does not describe the requested commit and app.
    for row in named:
        if row.get("head_sha") != commit:
            return 1, f"{CHECK_NAME} head_sha {row.get('head_sha')!r} differs from {commit}"
        app = row.get("app")
        if (not isinstance(app, dict) or app.get("id") != GITHUB_ACTIONS_APP_ID
                or app.get("slug") != "github-actions"):
            return 1, f"{CHECK_NAME} app is not GitHub Actions"
    completed = []
    for row in named:
        status = row.get("status")
        # A pending run has no completion timestamp with which to prove that
        # it is older than a success. Never reuse a success while one exists.
        if status != "completed":
            return 1, f"{CHECK_NAME} status is {status!r} on {commit}"
        try:
            completed.append((_completion(row.get("completed_at")), row))
        except ValueError as error:
            return 1, f"{CHECK_NAME} {error}"
    newest = max(stamp for stamp, _ in completed)
    latest = [row for stamp, row in completed if stamp == newest]
    for row in latest:
        if row.get("conclusion") != "success":
            return 1, f"{CHECK_NAME} conclusion is {row.get('conclusion')!r} on {commit}"
    return 0, f"{CHECK_NAME} completed success on {commit}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Full engine base commit SHA.")
    parser.add_argument("--head", required=True, help="Full engine pull-request head SHA.")
    args = parser.parse_args(argv)
    try:
        old, new = gitlink(args.base), gitlink(args.head)
        if old == new:
            print(f"gitlink unchanged: {new}")
            return 0
        code, reason = check_result(read_checks(CORPUS_REPOSITORY, new), new)
    except (OSError, ValueError, subprocess.CalledProcessError,
            urllib.error.URLError) as error:
        print(f"corpus validation unreadable: {error}")
        return 2
    print(reason)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
