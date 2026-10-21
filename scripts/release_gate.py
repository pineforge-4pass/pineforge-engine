#!/usr/bin/env python3
"""Refuse a release unless its tree matches a merged, gate-green PR head.

Usage: release_gate.py gate --repo OWNER/REPO --sha SHA --ref refs/heads/main

All evidence comes from the live GitHub API, including both commit trees and
the individual statuses on the matching PR head. ``main(api=callable)`` is the
only test seam; tests supply raw API responses without spawning commands.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import re
import subprocess
import sys
from typing import Callable


CONTEXTS = ("pineforge/verify", "pineforge/parity")
API = Callable[..., str]


class ReleaseGateError(ValueError):
    """Missing, red or unreadable release evidence; never an advisory failure."""


def gh_api(endpoint: str, *, paginate: bool = False, jq: str | None = None) -> str:
    command = ["gh", "api", endpoint]
    if paginate:
        command.append("--paginate")
    if jq is not None:
        command.extend(["--jq", jq])
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def _read_text(api: API, endpoint: str, **options) -> str:
    try:
        raw = api(endpoint, **options)
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or str(exc)).strip()
        raise ReleaseGateError(f"API {endpoint} failed: {detail}") from exc
    except Exception as exc:
        raise ReleaseGateError(f"API {endpoint} failed: {exc}") from exc
    if not isinstance(raw, str):
        raise ReleaseGateError(f"API {endpoint} returned a non-text payload")
    if not raw.strip():
        raise ReleaseGateError(f"API {endpoint} returned an empty payload")
    return raw.strip()


def _read_json(api: API, endpoint: str, *, paginate: bool = False):
    raw = _read_text(api, endpoint, paginate=paginate)
    try:
        if not paginate:
            result = json.loads(raw)
        else:
            # gh --paginate prints consecutive JSON arrays, one per page.
            # Decode every page before deciding, including trailing bad data.
            result = []
            decoder = json.JSONDecoder()
            while raw:
                page, end = decoder.raw_decode(raw)
                if not page:
                    raise ReleaseGateError(f"API {endpoint} returned an empty payload")
                if not isinstance(page, list):
                    raise ReleaseGateError(
                        f"API {endpoint} returned a non-array JSON page (expected a JSON array)")
                result.extend(page)
                raw = raw[end:].lstrip()
    except json.JSONDecodeError as exc:
        raise ReleaseGateError(f"API {endpoint} returned invalid JSON: {exc}") from exc
    if not result:
        raise ReleaseGateError(f"API {endpoint} returned an empty payload")
    return result


def _is_sha(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value) is not None


def _commit_tree(api: API, repo: str, sha: str) -> str:
    endpoint = f"repos/{repo}/commits/{sha}"
    payload = _read_json(api, endpoint)
    if not isinstance(payload, dict) or payload.get("sha") != sha:
        raise ReleaseGateError(f"API {endpoint} did not return commit {sha}")
    try:
        tree = payload["commit"]["tree"]["sha"]
    except (KeyError, TypeError):
        tree = None
    if not _is_sha(tree):
        raise ReleaseGateError(f"API {endpoint} returned missing or invalid commit.tree.sha")
    return tree


def _merged_pulls(api: API, repo: str, sha: str) -> list[tuple[datetime, int, str]]:
    endpoint = f"repos/{repo}/commits/{sha}/pulls"
    merged = []
    for row in _read_json(api, endpoint, paginate=True):
        if not isinstance(row, dict) or "merged_at" not in row:
            raise ReleaseGateError(f"API {endpoint} returned invalid pull request: {row!r}")
        if row["merged_at"] is None:
            continue
        try:
            merged_at = datetime.strptime(row["merged_at"], "%Y-%m-%dT%H:%M:%SZ")
        except (TypeError, ValueError) as exc:
            raise ReleaseGateError(
                f"API {endpoint} returned invalid merged_at: {row['merged_at']!r}") from exc
        number, head = row.get("number"), row.get("head")
        if (type(number) is not int or number <= 0 or not isinstance(head, dict)
                or not _is_sha(head.get("sha"))):
            raise ReleaseGateError(f"API {endpoint} returned invalid pull request: {row!r}")
        merged.append((merged_at, number, head["sha"]))
    if not merged:
        raise ReleaseGateError(f"no merged pull request associated with {sha}")
    return sorted(merged, reverse=True)


def gate(repo: str, sha: str, ref: str, *, api: API) -> str:
    if ref != "refs/heads/main":
        raise ReleaseGateError(f"release requires refs/heads/main, got {ref}")
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo) is None:
        raise ReleaseGateError("invalid --repo: expected OWNER/REPO")
    if not _is_sha(sha):
        raise ReleaseGateError(f"invalid release sha: {sha!r}")

    endpoint = f"repos/{repo}/commits/main"
    current = _read_text(api, endpoint, jq=".sha")
    if not _is_sha(current):
        raise ReleaseGateError(f"API {endpoint} returned invalid main sha: {current!r}")
    if current != sha:
        raise ReleaseGateError(f"release sha {sha} is not current main head {current}")

    merged = _merged_pulls(api, repo, sha)
    tree = _commit_tree(api, repo, sha)
    for _, number, head in merged:
        if _commit_tree(api, repo, head) == tree:
            break
    else:
        raise ReleaseGateError(f"no merged pull request head matches release tree {tree}")

    endpoint = f"repos/{repo}/commits/{head}/statuses?per_page=100"
    latest = {}
    for row in _read_json(api, endpoint, paginate=True):
        if (not isinstance(row, dict) or not isinstance(row.get("context"), str)
                or not row["context"]):
            raise ReleaseGateError(f"API {endpoint} returned invalid status row: {row!r}")
        context = row["context"]
        # GitHub returns newest first. Never let an older row override it.
        if context in latest:
            continue
        state = row.get("state")
        if not isinstance(state, str) or not state:
            raise ReleaseGateError(f"API {endpoint} returned invalid status state for {context}")
        latest[context] = state
    for context in CONTEXTS:
        if context not in latest:
            raise ReleaseGateError(f"PR #{number} head {head}: missing {context}")
        if latest[context] != "success":
            raise ReleaseGateError(
                f"PR #{number} head {head}: {context}={latest[context]} (expected success)")
    states = "; ".join(f"{context}={latest[context]}" for context in CONTEXTS)
    return f"release gate green: PR #{number} head {head}; {states}; tree {tree}"


def main(argv=None, *, api: API | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    check = modes.add_parser("gate", help="require a tree-matching, gate-green merged PR")
    check.add_argument("--repo", required=True)
    check.add_argument("--sha", required=True)
    check.add_argument("--ref", required=True)
    args = parser.parse_args(argv)
    try:
        green = gate(args.repo, args.sha, args.ref, api=api if api is not None else gh_api)
    except ReleaseGateError as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1
    print(green)
    return 0


if __name__ == "__main__":
    sys.exit(main())
