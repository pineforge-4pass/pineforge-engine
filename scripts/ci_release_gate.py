#!/usr/bin/env python3
"""Refuse a release without full verification and parity evidence.

Usage: ci_release_gate.py gate --repo OWNER/REPO --sha SHA --ref refs/heads/main
       ci_release_gate.py rulesets-probe --repo OWNER/REPO

Every GitHub API call is a read (GET), using the release job's own GH_TOKEN.
Each endpoint/page is read at most once; failures refuse without retry or sleep.
Phase 1 checks a unique tree-matching merged PR head or one authenticated,
two-file release bump on that gated parent. Phase 2 requires the exact main
sha's proof receipt after the fixed watcher cut-over, with no PR/bump fallback.
Tests inject API and git callables and patch the read_main_proof seam.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import re
import subprocess
import sys
from typing import Callable
from urllib.parse import parse_qs, urlsplit


CONTEXTS = ("pineforge/verify", "pineforge/parity")
# Confirmed operator PAT; additions require a reviewed code change (docs/ci.md).
ALLOWED_STATUS_CREATORS = (("luisleo526", "User"),)
# release.yml pushes protected main with its PINEFORGE_APP_ID installation token.
# This is GitHub's authenticated push actor, never the commit's author/committer.
RELEASE_PUSH_ACTOR = "pineforge-app[bot]"
# Integration for release.yml's PINEFORGE_APP_ID installation token.
# The gate relies on App permission administration: read to see bypass_actors;
# removing it makes every release-bump exemption refuse, even with contents: write.
RELEASE_APP_ID = 4105147
RELEASE_BUMP_PATHS = frozenset(("VERSION", "docker/run_failure_codes_diff.json"))
MAIN_PROOF_WATCHER_FIRST_SHA: str | None = None
"""None = the watcher is not live yet. Otherwise its FIRST recorded main sha.

This cut-over is fixed in code and changed only by a reviewed PR, never by the
release job, an environment variable or a workflow input.
"""
API = Callable[..., str]
Git = Callable[[str, str], bool]
# gh may print the next page's headers directly after a JSON array or object.
_HTTP_RESPONSE = re.compile(
    r"(?:^|(?<=[}\]]))HTTP/\S+ ([0-9]{3})[^\r\n]*\r?\n"
    r"(?:[A-Za-z0-9-]+:[^\r\n]*\r?\n)*\r?\n", re.MULTILINE)


class ReleaseGateError(ValueError):
    """Missing, red or unreadable release evidence; never an advisory failure."""


def gh_api(endpoint: str, *, paginate: bool = False, jq: str | None = None) -> str:
    command = ["gh", "api", endpoint, "--include"]
    if paginate:
        command.append("--paginate")
    if jq is not None:
        command.extend(["--jq", jq])
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def _http_failure(raw: str, *, exception: bool = False) -> str | None:
    codes = [int(match[1]) for match in _HTTP_RESPONSE.finditer(raw)]
    if exception:
        # gh reports failed reads as "(HTTP 403)" on stderr; HTTPError uses
        # "HTTP Error 503". stdout may still contain the response headers.
        codes.extend(int(code) for code in re.findall(
            r"\bHTTP(?: Error)?[ \t]+([0-9]{3})\b", raw, re.IGNORECASE))
    for code in codes:
        if 500 <= code <= 599:
            return f"upstream error {code}"
    if (re.search(r"^X-RateLimit-Remaining:[ \t]*0[ \t]*\r?$", raw,
                  re.MULTILINE | re.IGNORECASE)
            or (403 in codes and re.search(r"rate[ -]+limit", raw, re.IGNORECASE))):
        return "rate limited"
    for code in codes:
        if not 200 <= code <= 299:
            return f"failed with HTTP {code}"
    return None


def _read_text(api: API, endpoint: str, *, complete_pages: str | None = None,
               **options) -> str:
    try:
        raw = api(endpoint, **options)
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or str(exc)).strip()
        reason = _http_failure(f"{exc.stdout or ''}\n{exc.stderr or ''}",
                               exception=True) or "failed"
        raise ReleaseGateError(f"API {endpoint} {reason}: {detail}") from exc
    except Exception as exc:
        reason = _http_failure(str(exc), exception=True) or "failed"
        raise ReleaseGateError(f"API {endpoint} {reason}: {exc}") from exc
    if not isinstance(raw, str):
        raise ReleaseGateError(f"API {endpoint} returned a non-text payload")
    reason = _http_failure(raw)
    if reason:
        raise ReleaseGateError(f"API {endpoint} {reason}")
    # --include emits one header block per page, also when --jq is used.
    # Preserve page boundaries so a good page cannot hide an empty response.
    responses = list(_HTTP_RESPONSE.finditer(raw))
    if responses:
        if raw[:responses[0].start()].strip() or (
                not options.get("paginate", False) and len(responses) != 1):
            raise ReleaseGateError(f"API {endpoint} returned unexpected HTTP responses")
        bodies = []
        for index, response in enumerate(responses):
            end = responses[index + 1].start() if index + 1 < len(responses) else len(raw)
            body = raw[response.end():end].strip()
            if not body:
                raise ReleaseGateError(f"API {endpoint} returned an empty payload")
            if options.get("jq") is None:
                # One HTTP body must be one JSON page before trusting Links
                # or joining bodies; a headerless document is not a page.
                try:
                    _, document_end = json.JSONDecoder().raw_decode(body)
                except json.JSONDecodeError as exc:
                    raise ReleaseGateError(
                        f"API {endpoint} returned invalid JSON: {exc}") from exc
                if body[document_end:].strip():
                    raise ReleaseGateError(
                        f"API {endpoint} returned an HTTP response body with trailing data; "
                        "expected exactly one JSON document")
            bodies.append(body)
        raw = "\n".join(bodies)
    if complete_pages is not None:
        # A matching Actions total_count cannot override an unread Link page.
        # Keep gh's HTTP evidence for both Actions and ruleset pagination.
        failure = f"API {endpoint} returned incomplete {complete_pages} pagination"
        if not responses or not options.get("paginate"):
            raise ReleaseGateError(f"{failure}: missing HTTP page headers")
        for index, response in enumerate(responses):
            links = re.findall(r"^Link:[ \t]*([^\r\n]*)", response[0],
                               re.MULTILINE | re.IGNORECASE)
            relations = {}
            for field in links:
                for link in field.split(","):
                    parsed = re.fullmatch(
                        r'\s*<(https://[^<>\s]+)>;\s*rel="(next|prev|first|last)"\s*', link)
                    if parsed is None or parsed[2] in relations:
                        raise ReleaseGateError(f"{failure}: malformed Link header")
                    try:
                        page = parse_qs(urlsplit(parsed[1]).query).get("page")
                    except ValueError as exc:
                        raise ReleaseGateError(f"{failure}: invalid page URL") from exc
                    if (page is None or len(page) != 1
                            or re.fullmatch(r"[1-9][0-9]*", page[0]) is None):
                        raise ReleaseGateError(f"{failure}: invalid page number")
                    relations[parsed[2]] = int(page[0])
            if ("next" in relations) != (index + 1 < len(responses)):
                raise ReleaseGateError(f"{failure}: unread or unlinked next page")
            expected = {"next": index + 2, "prev": index,
                        "first": 1, "last": len(responses)}
            if any(page != expected[relation] for relation, page in relations.items()):
                raise ReleaseGateError(f"{failure}: skipped, repeated or truncated pages")
    if not raw.strip():
        raise ReleaseGateError(f"API {endpoint} returned an empty payload")
    return raw.strip()


def _read_json(api: API, endpoint: str, *, paginate: bool = False,
               items_key: str | None = None, complete_rulesets: bool = False):
    raw = _read_text(api, endpoint, paginate=paginate,
                    complete_pages="ruleset" if complete_rulesets else items_key)
    try:
        if not paginate:
            result = json.loads(raw)
        else:
            # gh --paginate prints consecutive JSON documents, one per page.
            # Framed HTTP bodies were each checked before joining them.
            # Decode every page before deciding, including trailing bad data.
            result = []
            total = None
            decoder = json.JSONDecoder()
            while raw:
                page, end = decoder.raw_decode(raw)
                if items_key is not None:
                    if (not isinstance(page, dict)
                            or not isinstance(page.get(items_key), list)
                            or type(page.get("total_count")) is not int
                            or page["total_count"] < 0):
                        raise ReleaseGateError(
                            f"API {endpoint} returned invalid {items_key} page")
                    if total is not None and total != page["total_count"]:
                        raise ReleaseGateError(
                            f"API {endpoint} returned incomplete {items_key} pagination: "
                            "total_count changed between pages")
                    total = page["total_count"]
                    page = page[items_key]
                if not page and not isinstance(page, list):
                    raise ReleaseGateError(f"API {endpoint} returned an empty payload")
                if not isinstance(page, list):
                    raise ReleaseGateError(
                        f"API {endpoint} returned a non-array JSON page (expected a JSON array)")
                result.extend(page)
                raw = raw[end:].lstrip()
            if items_key is not None and len(result) != total:
                # The Actions API caps filtered searches; --paginate alone
                # cannot establish completeness when total_count exceeds it.
                raise ReleaseGateError(
                    f"API {endpoint} returned incomplete {items_key} pagination: "
                    f"expected {total} rows, got {len(result)}")
    except json.JSONDecodeError as exc:
        raise ReleaseGateError(f"API {endpoint} returned invalid JSON: {exc}") from exc
    # Empty arrays are valid list answers; callers decide if evidence is missing.
    if not result and not isinstance(result, list):
        raise ReleaseGateError(f"API {endpoint} returned an empty payload")
    return result


def _is_sha(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value) is not None


def _commit_info(api: API, repo: str, sha: str) -> dict:
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
    return payload


def _commit_tree(api: API, repo: str, sha: str) -> str:
    return _commit_info(api, repo, sha)["commit"]["tree"]["sha"]


def _merged_pulls(api: API, repo: str, sha: str, *,
                  allow_empty: bool = False) -> list[tuple[int, str]]:
    endpoint = f"repos/{repo}/commits/{sha}/pulls"
    merged = []
    for row in _read_json(api, endpoint, paginate=True):
        if not isinstance(row, dict) or "merged_at" not in row:
            raise ReleaseGateError(f"API {endpoint} returned invalid pull request: {row!r}")
        if row["merged_at"] is None:
            continue
        try:
            datetime.strptime(row["merged_at"], "%Y-%m-%dT%H:%M:%SZ")
        except (TypeError, ValueError) as exc:
            raise ReleaseGateError(
                f"API {endpoint} returned invalid merged_at: {row['merged_at']!r}") from exc
        base, merge_sha = row.get("base"), row.get("merge_commit_sha")
        if (not isinstance(base, dict) or not isinstance(base.get("ref"), str)
                or not base["ref"]):
            raise ReleaseGateError(f"API {endpoint} returned invalid pull request base.ref: {base!r}")
        if not _is_sha(merge_sha):
            raise ReleaseGateError(f"API {endpoint} returned invalid merge_commit_sha: {merge_sha!r}")
        number, head = row.get("number"), row.get("head")
        if (type(number) is not int or number <= 0 or not isinstance(head, dict)
                or not _is_sha(head.get("sha"))):
            raise ReleaseGateError(f"API {endpoint} returned invalid pull request: {row!r}")
        if base["ref"] != "main" or merge_sha != sha:
            continue
        if any(number == previous for previous, _ in merged):
            raise ReleaseGateError(f"API {endpoint} returned duplicate pull request #{number}")
        merged.append((number, head["sha"]))
    if not merged and not allow_empty:
        raise ReleaseGateError(f"no merged pull request associated with {sha}")
    return merged


def _require_release_push(api: API, repo: str, sha: str) -> None:
    endpoint = f"repos/{repo}/actions/runs?head_sha={sha}&per_page=100"
    found = False
    runs = _read_json(api, endpoint, paginate=True, items_key="workflow_runs")
    # Filtered Actions searches stop at 1000 results. Refuse at the boundary
    # even if total_count itself was capped, rather than infer completeness.
    if len(runs) >= 1000:
        raise ReleaseGateError(
            f"API {endpoint} returned incomplete workflow_runs pagination: "
            "filtered search reached the 1000-run limit")
    for row in runs:
        if (not isinstance(row, dict) or not isinstance(row.get("event"), str)
                or not row["event"] or "head_branch" not in row
                or (row["head_branch"] is not None
                    and not isinstance(row["head_branch"], str))
                or not _is_sha(row.get("head_sha"))):
            raise ReleaseGateError(f"API {endpoint} returned invalid workflow run: {row!r}")
        if (row["event"] != "push" or row["head_branch"] != "main"
                or row["head_sha"] != sha):
            continue
        found = True
        for field in ("actor", "triggering_actor"):
            actor = row.get(field)
            if not isinstance(actor, dict) or actor.get("login") != RELEASE_PUSH_ACTOR:
                raise ReleaseGateError(
                    f"release bump push actor must be {RELEASE_PUSH_ACTOR} "
                    f"for both actor and triggering_actor: {field}={actor!r}")
    if not found:
        raise ReleaseGateError(f"no exact-main push workflow run for release bump {sha}")


def _ruleset_applies_to_main(payload: dict, endpoint: str) -> bool:
    conditions = payload.get("conditions")
    refs = conditions.get("ref_name") if isinstance(conditions, dict) else None
    if (not isinstance(refs, dict)
            or any(not isinstance(refs.get(key), list)
                   or any(not isinstance(pattern, str) or not pattern
                          for pattern in refs[key]) for key in ("include", "exclude"))):
        raise ReleaseGateError(f"API {endpoint} returned invalid ref_name conditions")

    def matches(pattern: str) -> bool:
        if pattern in ("~DEFAULT_BRANCH", "~ALL"):
            return True
        # GitHub uses pathname-aware fnmatch: * and ? cannot cross '/'.
        # Refuse advanced/unknown syntax instead of guessing applicability.
        if (pattern.startswith("~") or "**" in pattern
                or any(char in pattern for char in ("[", "]", "{", "}", "\\"))):
            raise ReleaseGateError(f"API {endpoint} returned invalid ref_name conditions: "
                                   f"unsupported pattern {pattern!r}")
        expression = re.escape(pattern).replace(r"\*", "[^/]*").replace(r"\?", "[^/]")
        return re.fullmatch(expression, "refs/heads/main") is not None

    included = [matches(pattern) for pattern in refs["include"]]
    excluded = [matches(pattern) for pattern in refs["exclude"]]
    return any(included) and not any(excluded)


def _ruleset_bypasses(payload: dict, endpoint: str) -> set[tuple[str, int | None, str]]:
    actors = payload.get("bypass_actors")
    if not isinstance(actors, list):
        raise ReleaseGateError(f"API {endpoint} returned missing or invalid bypass_actors "
                               "(App requires administration: read)")
    bypasses = set()
    for actor in actors:
        if not isinstance(actor, dict):
            raise ReleaseGateError(f"API {endpoint} returned invalid bypass actor: {actor!r}")
        kind, number, mode = (actor.get(key) for key in
                              ("actor_type", "actor_id", "bypass_mode"))
        if (not isinstance(kind, str)
                or kind not in ("Integration", "OrganizationAdmin", "DeployKey",
                                "RepositoryRole", "Team")
                or "actor_id" not in actor
                or (number is None and kind not in ("OrganizationAdmin", "DeployKey"))
                or (number is not None and (type(number) is not int or number <= 0))
                or not isinstance(mode, str) or mode not in ("always", "pull_request")):
            raise ReleaseGateError(f"API {endpoint} returned invalid bypass actor: {actor!r}")
        identity = (kind, number, mode)
        if identity in bypasses:
            raise ReleaseGateError(f"API {endpoint} returned invalid bypass actor: duplicate")
        bypasses.add(identity)
    return bypasses


def _require_release_rulesets(api: API, repo: str) -> tuple[dict, list[dict]]:
    endpoint = f"repos/{repo}"
    repository = _read_json(api, endpoint)
    if not isinstance(repository, dict) or repository.get("default_branch") != "main":
        raise ReleaseGateError(f"API {endpoint} must identify main as the default branch")

    endpoint = f"repos/{repo}/rulesets?includes_parents=true&per_page=100"
    inventory = _read_json(api, endpoint, paginate=True, complete_rulesets=True)
    seen, summaries = set(), []
    for row in inventory:
        if (not isinstance(row, dict) or type(row.get("id")) is not int or row["id"] <= 0
                or not isinstance(row.get("target"), str)
                or row["target"] not in ("branch", "tag", "push")
                or not isinstance(row.get("enforcement"), str)
                or row["enforcement"] not in ("active", "disabled", "evaluate")):
            raise ReleaseGateError(f"API {endpoint} returned invalid ruleset summary: {row!r}")
        if row["id"] in seen:
            raise ReleaseGateError(f"API {endpoint} returned duplicate ruleset id {row['id']}")
        seen.add(row["id"])
        summaries.append(row)

    barrier_found = False
    barrier, branches = {}, []
    for summary in summaries:
        if summary["target"] != "branch":
            continue
        endpoint = f"repos/{repo}/rulesets/{summary['id']}"
        payload = _read_json(api, endpoint)
        if (not isinstance(payload, dict) or type(payload.get("id")) is not int
                or any(payload.get(key) != summary[key]
                       for key in ("id", "target", "enforcement"))):
            raise ReleaseGateError(f"API {endpoint} returned inconsistent ruleset detail")
        bypasses = _ruleset_bypasses(payload, endpoint)
        applies = _ruleset_applies_to_main(payload, endpoint)
        rules = payload.get("rules")
        if (not isinstance(rules, list)
                or any(not isinstance(rule, dict) or not isinstance(rule.get("type"), str)
                       or not rule["type"] for rule in rules)):
            raise ReleaseGateError(f"API {endpoint} returned invalid rules")
        # Display metadata is evidence, not an additional gate requirement.
        detail = {**summary, **payload}
        branches.append(detail)
        blocking = any(rule["type"] == "pull_request" for rule in rules)
        if not applies or not blocking or payload["enforcement"] != "active":
            continue
        # This one PR rule blocks every direct push except its own bypasses.
        # Descriptor intersections cannot account for overlapping role/team members.
        if bypasses == {("Integration", RELEASE_APP_ID, "always")}:
            if not barrier:
                barrier = detail
            barrier_found = True
        # Finish reading all branch details so later API/visibility errors refuse.

    if not barrier_found:
        raise ReleaseGateError("main requires an active applicable pull_request ruleset "
                               "with bypass_actors exactly "
                               f"Integration {RELEASE_APP_ID} with bypass_mode always")
    return barrier, branches


def rulesets_probe(repo: str, *, api: API) -> str:
    """Read exactly the bump's ruleset evidence, with the same refusal policy."""
    barrier, branches = _require_release_rulesets(api, repo)
    inventory = "; ".join(
        f"{row['id']} {row.get('name', '<unknown>')!r} {row['enforcement']} "
        f"source={row.get('source_type', '<unknown>')!r}:{row.get('source', '<unknown>')!r} "
        f"bypass_actors={'visible' if isinstance(row.get('bypass_actors'), list) else 'hidden'}"
        for row in branches)
    return (f"release ruleset barrier ok: ruleset {barrier['id']} "
            f"{barrier.get('name', '<unknown>')!r}\nbranch rulesets: {inventory}")


def _tree_files(api: API, repo: str, tree: str) -> dict[str, tuple[str, str, str]]:
    endpoint = f"repos/{repo}/git/trees/{tree}?recursive=1"
    payload = _read_json(api, endpoint)
    if (not isinstance(payload, dict) or payload.get("sha") != tree
            or payload.get("truncated") is not False
            or not isinstance(payload.get("tree"), list)):
        raise ReleaseGateError(f"API {endpoint} returned invalid or truncated git tree")
    files, seen = {}, set()
    modes = {"tree": ("040000",), "blob": ("100644", "100755", "120000"),
             "commit": ("160000",)}
    for row in payload["tree"]:
        if not isinstance(row, dict):
            raise ReleaseGateError(f"API {endpoint} returned invalid git tree entry: {row!r}")
        path, mode, kind, blob = (row.get(key) for key in ("path", "mode", "type", "sha"))
        if (not isinstance(path, str) or not path
                or any(part in ("", ".", "..") for part in path.split("/"))
                or not isinstance(kind, str) or kind not in modes
                or mode not in modes[kind] or not _is_sha(blob)):
            raise ReleaseGateError(f"API {endpoint} returned invalid git tree entry: {row!r}")
        if path in seen:
            raise ReleaseGateError(f"API {endpoint} returned duplicate git tree path: {path}")
        seen.add(path)
        if kind != "tree":
            files[path] = (mode, kind, blob)
    return files


def _release_bump(api: API, repo: str, sha: str) -> str:
    _require_release_push(api, repo, sha)
    _require_release_rulesets(api, repo)
    candidate = _commit_info(api, repo, sha)
    parents = candidate.get("parents")
    if (not isinstance(parents, list) or len(parents) != 1
            or not isinstance(parents[0], dict) or not _is_sha(parents[0].get("sha"))
            or parents[0]["sha"] == sha):
        raise ReleaseGateError("release bump requires exactly one valid parent")
    parent = parents[0]["sha"]
    trees = {sha: candidate["commit"]["tree"]["sha"]}
    # This helper requires a merged PR, never another bump or main-proof fallback.
    # It caches every commit tree so each endpoint is still read at most once.
    evidence = _phase_one(api, repo, parent, trees=trees)
    before = _tree_files(api, repo, trees[parent])
    after = before if trees[parent] == trees[sha] else _tree_files(api, repo, trees[sha])
    changed = {path for path in before.keys() | after.keys()
               if before.get(path) != after.get(path)}
    # M means the path exists on both sides and its blob changed. Comparing full
    # trees also rejects extra files, additions, deletions and renames.
    if (changed != RELEASE_BUMP_PATHS
            or any(path not in before or path not in after for path in changed)):
        raise ReleaseGateError(
            "release bump must modify exactly VERSION and docker/run_failure_codes_diff.json")
    for path in sorted(changed):
        if before[path][:2] != ("100644", "blob") or after[path][:2] != ("100644", "blob"):
            raise ReleaseGateError(f"release bump path {path} must stay mode 100644")
    return f"release bump {sha}; parent {parent}; {evidence}"


def git_is_ancestor(ancestor: str, descendant: str) -> bool:
    result = subprocess.run(["git", "merge-base", "--is-ancestor", ancestor, descendant],
                            check=False, capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise ReleaseGateError(
            f"git merge-base --is-ancestor failed: {result.stderr.strip()}")
    return result.returncode == 0


def read_main_proof(repo: str, sha: str) -> dict | None:
    """Return normalized context -> state evidence for this exact repo/main sha.

    This seam defines no receipt storage format; C2 supplies the reader that
    binds the receipt to the requested sha. None means no receipt was recorded.
    """
    raise NotImplementedError("phase 2 receipt reader lands with C2 main-watch #176")


def _uses_main_proof(sha: str, git: Git) -> bool:
    first = MAIN_PROOF_WATCHER_FIRST_SHA
    if first is None:
        return False
    if not _is_sha(first):
        raise ReleaseGateError("invalid MAIN_PROOF_WATCHER_FIRST_SHA cut-over")
    try:
        after = git(first, sha)
        if type(after) is not bool:
            raise ReleaseGateError("invalid git ancestry result for main-proof cut-over")
        if after:
            return True
        before = git(sha, first)
        if type(before) is not bool:
            raise ReleaseGateError("invalid git ancestry result for main-proof cut-over")
        if before:
            return False
    except Exception as exc:
        raise ReleaseGateError(f"cannot determine main-proof cut-over ancestry: {exc}") from exc
    raise ReleaseGateError(f"release sha {sha} is unrelated to main-proof cut-over {first}")


def _require_full_contexts(latest: dict, subject: str) -> str:
    # Exact names only: no prefix match and no lite-context fallback.
    for context in CONTEXTS:
        if context not in latest:
            raise ReleaseGateError(f"{subject}: missing {context}")
        if latest[context] != "success":
            raise ReleaseGateError(
                f"{subject}: {context}={latest[context]} (expected success)")
    return "; ".join(f"{context}={latest[context]}" for context in CONTEXTS)


def gate(repo: str, sha: str, ref: str, *, api: API, git: Git = git_is_ancestor) -> str:
    if ref != "refs/heads/main":
        raise ReleaseGateError(f"release requires refs/heads/main, got {ref}")
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo) is None:
        raise ReleaseGateError("invalid --repo: expected OWNER/REPO")
    if not _is_sha(sha):
        raise ReleaseGateError(f"invalid release sha: {sha!r}")

    endpoint = f"repos/{repo}/branches/main"
    current = _read_text(api, endpoint, jq=".commit.sha")
    if not _is_sha(current):
        raise ReleaseGateError(f"API {endpoint} returned invalid main sha: {current!r}")
    if current != sha:
        raise ReleaseGateError(f"release sha {sha} is not current main head {current}")

    if _uses_main_proof(sha, git):
        subject = f"main-proof receipt for {sha}"
        try:
            proof = read_main_proof(repo, sha)
        except Exception as exc:
            raise ReleaseGateError(f"{subject} unreadable: {exc}") from exc
        if proof is None:
            raise ReleaseGateError(f"main-proof receipt is missing for {sha}")
        if (not isinstance(proof, dict) or not proof
                or any(not isinstance(context, str) or not context
                       or not isinstance(state, str) or not state
                       for context, state in proof.items())):
            raise ReleaseGateError(f"{subject}: invalid or empty context map")
        states = _require_full_contexts(proof, subject)
        return f"release gate green: {subject}; {states}"

    evidence = _phase_one(api, repo, sha, allow_missing_pr=True)
    if evidence is None:
        try:
            evidence = _release_bump(api, repo, sha)
        except ReleaseGateError as exc:
            raise ReleaseGateError(
                f"no merged pull request associated with {sha}; release bump refused: {exc}") from exc
    return f"release gate green: {evidence}"


def _phase_one(api: API, repo: str, sha: str, *, allow_missing_pr: bool = False,
               trees: dict[str, str] | None = None) -> str | None:
    merged = _merged_pulls(api, repo, sha, allow_empty=allow_missing_pr)
    if not merged:
        return None
    if trees is None:
        trees = {}
    if sha not in trees:
        trees[sha] = _commit_tree(api, repo, sha)
    tree = trees[sha]
    matches = []
    for number, head in merged:
        if head not in trees:
            trees[head] = _commit_tree(api, repo, head)
        if trees[head] == tree:
            matches.append((number, head))
    if len(matches) > 1:
        numbers = ", ".join(f"#{number}" for number, _ in sorted(matches))
        raise ReleaseGateError(
            f"ambiguous release tree {tree}: multiple merged PR heads match ({numbers})")
    if not matches:
        raise ReleaseGateError(f"no merged pull request head matches release tree {tree}")
    number, head = matches[0]

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
        if context in CONTEXTS:
            creator = row.get("creator")
            if not isinstance(creator, dict):
                raise ReleaseGateError(
                    f"API {endpoint}: {context}: missing or malformed status creator: {creator!r}")
            login, creator_type = creator.get("login"), creator.get("type")
            if (not isinstance(login, str) or not isinstance(creator_type, str)
                    or (login, creator_type) not in ALLOWED_STATUS_CREATORS):
                raise ReleaseGateError(
                    f"API {endpoint}: {context}: refused status creator "
                    f"(login={login!r}, type={creator_type!r})")
        state = row.get("state")
        if not isinstance(state, str) or not state:
            raise ReleaseGateError(f"API {endpoint} returned invalid status state for {context}")
        latest[context] = state
    states = _require_full_contexts(latest, f"PR #{number} head {head}")
    return f"PR #{number} head {head}; {states}; tree {tree}"


def main(argv=None, *, api: API | None = None, git: Git | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    check = modes.add_parser("gate", help="require full release proof for the current main sha")
    check.add_argument("--repo", required=True)
    check.add_argument("--sha", required=True)
    check.add_argument("--ref", required=True)
    probe = modes.add_parser("rulesets-probe", help="read the release bump's ruleset barrier only")
    probe.add_argument("--repo", required=True)
    args = parser.parse_args(argv)
    try:
        if args.mode == "rulesets-probe":
            green = rulesets_probe(args.repo, api=api if api is not None else gh_api)
        else:
            green = gate(args.repo, args.sha, args.ref, api=api if api is not None else gh_api,
                         git=git if git is not None else git_is_ancestor)
    except ReleaseGateError as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1
    print(green)
    return 0


if __name__ == "__main__":
    sys.exit(main())
