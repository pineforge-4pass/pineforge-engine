#!/usr/bin/env python3
"""Refuse a release without authenticated parity and full post-merge evidence.

Usage: ci_release_gate.py gate --repo OWNER/REPO --sha SHA --ref refs/heads/main
           [--phase bump|tag] [--dispatch-sha SHA] [--receipt FILE] [--dry-run true|false]
       ci_release_gate.py plan --sha SHA --phase bump|tag --github-output FILE
           [--bump patch|minor|major] [--override VERSION] [--dispatch-sha SHA]
       ci_release_gate.py rulesets-probe --repo OWNER/REPO

Every GitHub API call is a read (GET), using the release job's own GH_TOKEN.
Each gate endpoint/page is read once, except relied-on run metadata: one initial
read and one uncached validation immediately before admission. Failures refuse
without retry or sleep; a changed attempt or mismatched final metadata refuses.
The probe measures the inventory before an independent verdict read.
The explicit bump phase checks the dispatched main commit. The tag phase
requires an authenticated two-file bump on the named phase-1 dispatch,
that parent's authenticated parity and the bump's own fully green ci-post.
The fixed main-proof cut-over still governs parity admission, without fallback.
Calls without a phase retain the legacy read-only admission interface.
Docs-only releases walk back with the shared classifier to the actual built
proof, retaining each docs commit's own latest successful run.
pineforge/verify is advisory; its actual presence and state are recorded.
Tests inject API, git and history callables and patch the read_main_proof seam.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Callable
from urllib.parse import parse_qs, urlsplit

from ci_docs_only import is_documentation
import gen_run_failure_catalog_diff as catalog_diff
import release_version


CONTEXTS = ("pineforge/parity",)
# ci-post.yml's amd64 leg aggregates and arm shard names; the workflow pin test
# expands their displayed names independently and checks this required-name tuple.
CI_POST_BUILD_JOBS = (
    *(f"build (ubuntu-24.04, {build_type})" for build_type in ("Debug", "Release")),
    *(f"build-shard (ubuntu-24.04-arm, {build_type}, {shard}/4)"
      for build_type in ("Debug", "Release") for shard in range(1, 5)),
)
CI_POST_FAILURES = ("failure", "cancelled", "timed_out")
CI_POST_RUN_LIMIT = 10
ATTEMPT_LIMIT = 100
JOBS_LIMIT = 10000
DOCS_ONLY_WALK_LIMIT = 20
# Confirmed operator PAT; additions require a reviewed code change (docs/ci.md).
ALLOWED_STATUS_CREATORS = (("luisleo526", "User"),)
# release.yml pushes protected main with its PINEFORGE_APP_ID installation token.
# This is GitHub's authenticated push actor, never the commit's author/committer.
RELEASE_PUSH_ACTOR = "pineforge-app[bot]"
# Integration for release.yml's PINEFORGE_APP_ID installation token.
# The gate needs bypass_actors visible on every branch ruleset in the inventory.
# If the release App token cannot see them, the bump is refused;
# release-ruleset-probe.yml reports whether it can.
RELEASE_APP_ID = 4105147
RELEASE_BUMP_PATHS = frozenset(("VERSION", "docker/run_failure_codes_diff.json"))
MAIN_PROOF_WATCHER_FIRST_SHA: str | None = None
"""None = the watcher is not live yet. Otherwise its FIRST recorded main sha.

This cut-over is fixed in code and changed only by a reviewed PR, never by the
release job, an environment variable or a workflow input.
"""
API = Callable[..., str | bytes]
Git = Callable[[str, str], bool]
History = Callable[[str], tuple[str | None, list[str]]]
# gh may print the next page's headers directly after a JSON array or object.
_HTTP_RESPONSE = re.compile(
    r"(?:^|(?<=[}\]]))HTTP/\S+ ([0-9]{3})[^\r\n]*\r?\n"
    r"(?:[A-Za-z0-9-]+:[^\r\n]*\r?\n)*\r?\n", re.MULTILINE)
_HTTP_RESPONSE_BYTES = re.compile(_HTTP_RESPONSE.pattern.encode("ascii"), re.MULTILINE)


class ReleaseGateError(ValueError):
    """Missing, red or unreadable release evidence; never an advisory failure."""


def _fields(row: dict, names: tuple[str, ...]) -> dict:
    """Only documented public metadata enters a receipt, never arbitrary API keys."""
    return {key: row[key] if type(row[key]) in (str, int, bool, type(None)) else None
            for key in names if key in row}


def _actor_record(actor) -> dict | None:
    return _fields(actor, ("login", "id", "type")) if isinstance(actor, dict) else None


def _run_record(run: dict) -> dict:
    result = _fields(run, ("id", "run_attempt", "head_sha", "head_branch", "event",
                           "status", "conclusion", "created_at", "updated_at",
                           "run_started_at", "html_url"))
    result.update(actor=_actor_record(run.get("actor")),
                  triggering_actor=_actor_record(run.get("triggering_actor")))
    return result


def _job_record(job: dict) -> dict:
    result = _fields(job, ("id", "run_id", "head_sha", "name", "run_attempt",
                           "status", "conclusion", "created_at", "started_at",
                           "completed_at", "html_url"))
    if "steps" in job:
        result["steps"] = [
            _fields(step, ("name", "number", "status", "conclusion",
                           "started_at", "completed_at"))
            for step in job["steps"] if isinstance(step, dict)
        ] if isinstance(job["steps"], list) else None
    return result


def _new_receipt(repo: str, sha: str, phase: str | None, dispatch: str,
                 dry_run: bool = False) -> dict:
    return {
        "schema": "pineforge-release-gate/v1", "repository": repo, "phase": phase,
        "dry_run": dry_run, "created_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "requested": {"sha": sha, "tree": None}, "dispatch_sha": dispatch or None,
        "parent_sha": None, "parent_tree": None, "dispatch_tree": None,
        "observed_main": None, "observed_main_tree": None, "contexts": {},
        "required_contexts": list(CONTEXTS),
        "advisory_verify": {"context": "pineforge/verify", "presence": None,
                            "state": "unread", "complete": False, "sha": None, "tree": None,
                            "source": None, "status": None},
        "ci_post": {"proof": None, "selected_run_id": None, "runs": []},
        "docs_chain": [], "push_provenance": [],
        "response_inventory": [], "run_validations": [],
        "limits": {"ci_post_runs": CI_POST_RUN_LIMIT, "docs_walk": DOCS_ONLY_WALK_LIMIT,
                   "attempts": ATTEMPT_LIMIT, "jobs_per_inventory": JOBS_LIMIT,
                   "filtered_runs_refuse_at": 1000},
        "checkpoint": "input", "gate": {"outcome": "refused", "reasons": []},
    }


class _GateReads:
    """One-pass collection, with the original transport reserved for validation."""

    def __init__(self, transport: API, receipt: dict | None):
        self.transport = transport
        self.cache = {}
        self.runs = {}
        self.selected_run_id = None
        self.inventory = receipt["response_inventory"] if receipt is not None else []
        self.validations = receipt["run_validations"] if receipt is not None else []

    def __call__(self, endpoint: str, *, paginate=False, jq=None):
        key = endpoint, paginate, jq
        if key not in self.cache:
            self.cache[key] = self.transport(endpoint, paginate=paginate, jq=jq)
        return self.cache[key]


def _memo_api(api: API, *, receipt: dict | None = None) -> API:
    return _GateReads(api, receipt)


def _response_slot(api: API, endpoint: str, run_id: int, attempt: int | None,
                   purpose: str) -> dict:
    response = {
        "endpoint": endpoint, "run_id": run_id, "attempt": attempt, "purpose": purpose,
        "scope": "gh-api-stdout-bytes/v1", "sha256": None, "byte_length": None,
        "responses": [], "complete": False,
    }
    if isinstance(api, _GateReads):
        api.inventory.append(response)
    return response


def _capture_response(response: dict, received: bytes) -> None:
    """Hash exact stdout before decoding; never retain its headers or body.

    The whole digest covers all gh --include stdout, not wire HTTP bytes.
    Ordered response slices include headers, body and any separator up to the
    next HTTP header. The first slice includes any leading bytes. An unframed
    fixture is one slice. No strip, newline conversion or JSON encoding occurs.
    """
    response["sha256"] = hashlib.sha256(received).hexdigest()
    response["byte_length"] = len(received)
    starts = [match.start() for match in _HTTP_RESPONSE_BYTES.finditer(received)]
    boundaries = [0, *starts[1:], len(received)]
    response["responses"] = [
        {"number": number, "sha256": hashlib.sha256(received[start:end]).hexdigest(),
         "byte_length": end - start}
        for number, (start, end) in enumerate(zip(boundaries, boundaries[1:]), 1)]


def _bind_response_inventory(receipt: dict) -> None:
    """Bind the ordered digest manifest, explicitly distinct from raw digests."""
    inventory = receipt["response_inventory"]
    encoded = json.dumps(inventory, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("utf-8")
    receipt["gate"]["response_binding"] = {
        "scope": "canonical-response-inventory/v1",
        "sha256": hashlib.sha256(encoded).hexdigest(), "entries": len(inventory)}


def _run_identity(run) -> tuple:
    if (not isinstance(run, dict) or type(run.get("id")) is not int or run["id"] <= 0
            or type(run.get("run_attempt")) is not int
            or not 1 <= run["run_attempt"] <= ATTEMPT_LIMIT
            or not _is_sha(run.get("head_sha"))
            or not isinstance(run.get("head_branch"), str)
            or not isinstance(run.get("event"), str) or not run["event"]
            or not isinstance(run.get("status"), str) or not run["status"]
            or "conclusion" not in run
            or (run["conclusion"] is not None and not isinstance(run["conclusion"], str))):
        raise ReleaseGateError("ci-post: mismatched run metadata")
    return (*(run[key] for key in ("id", "run_attempt", "head_sha", "head_branch",
                                   "event", "status", "conclusion")),
            (_actor_login(run.get("actor")), _actor_record(run.get("actor"))),
            (_actor_login(run.get("triggering_actor")),
             _actor_record(run.get("triggering_actor"))),
            _fields(run, ("created_at", "updated_at", "run_started_at", "html_url")))


def _observe_run(api: API, repo: str, run: dict, record: dict) -> None:
    """Read current metadata before jobs; deduplicate by repository and run id."""
    identity = _run_identity(run)
    run_id, attempt = run["id"], run["run_attempt"]
    key = repo, run_id
    existing = api.runs.get(key) if isinstance(api, _GateReads) else record.get("run_validation")
    if existing is not None:
        record["run_validation"] = existing
        if _run_identity(existing["initial_run"]) != identity:
            existing["complete"] = False
            existing["outcome"] = "mismatched"
            raise ReleaseGateError("ci-post: mismatched repeated run observation")
        return
    endpoint = f"repos/{repo}/actions/runs/{run_id}"
    response = _response_slot(api, endpoint, run_id, attempt, "run_initial")
    validation = {
        "repository": repo, "run_id": run_id, "listed_attempt": attempt,
        "initial_attempt": None, "final_attempt": None,
        "initial_run": None, "final_run": None,
        "initial_response": response, "final_response": None,
        "complete": False, "outcome": "unreadable",
    }
    record["run_validation"] = validation
    if isinstance(api, _GateReads):
        api.runs[key] = validation
        api.validations.append(validation)
    metadata = _read_json(api, endpoint, response=response)
    if isinstance(metadata, dict):
        validation["initial_run"] = _run_record(metadata)
        validation["initial_attempt"] = _fields(metadata, ("run_attempt",)).get("run_attempt")
    validation["outcome"] = "mismatched"
    if _run_identity(metadata) != identity:
        raise ReleaseGateError("ci-post: mismatched initial run metadata")
    validation["outcome"] = "pending"


def _revalidate_runs(api: _GateReads) -> None:
    """Exactly one fresh read per relied-on run; the selected proof is last."""
    ordered = sorted(api.runs.values(),
                     key=lambda row: row["run_id"] == api.selected_run_id)
    for validation in ordered:
        run_id, attempt = validation["run_id"], validation["initial_attempt"]
        endpoint = f"repos/{validation['repository']}/actions/runs/{run_id}"
        response = _response_slot(api, endpoint, run_id, attempt, "run_final")
        validation["final_response"] = response
        validation["outcome"] = "unreadable"
        # Deliberately bypass _memo_api and its cache. This is validation,
        # not a retry of a failed read; no other input is read again here.
        metadata = _read_json(api.transport, endpoint, response=response)
        if isinstance(metadata, dict):
            validation["final_run"] = _run_record(metadata)
            validation["final_attempt"] = _fields(metadata, ("run_attempt",)).get("run_attempt")
        validation["outcome"] = "mismatched"
        identity = _run_identity(metadata)
        if metadata["run_attempt"] != attempt:
            validation["outcome"] = "changed"
            raise ReleaseGateError(f"ci-post: run attempt changed for run {run_id}")
        if identity != _run_identity(validation["initial_run"]):
            raise ReleaseGateError(f"ci-post: mismatched final run metadata for run {run_id}")
        validation["outcome"] = "stable"
        validation["complete"] = True


def _actor_login(actor) -> str:
    login = actor.get("login") if isinstance(actor, dict) else None
    if not isinstance(login, str) or re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9-]*(?:\[bot\])?", login) is None:
        raise ReleaseGateError("missing or invalid workflow actor login")
    return login


def _trigger_permission(api: API, repo: str, actor: str, trigger: str, attempt: int) -> dict:
    if trigger == actor and (attempt == 1 or actor == RELEASE_PUSH_ACTOR):
        return {"source": "original actor", "login": trigger, "permission": None}
    if attempt == 1:
        raise ReleaseGateError("first workflow attempt triggering_actor differs from actor")
    endpoint = f"repos/{repo}/collaborators/{trigger}/permission"
    permission = _read_json(api, endpoint)
    if (not isinstance(permission, dict)
            or not isinstance(permission.get("user"), dict)
            or permission["user"].get("login") != trigger
            or permission.get("permission") not in ("admin", "maintain", "write")):
        raise ReleaseGateError("workflow rerun triggering_actor requires repository write permission")
    return {"source": endpoint, "login": trigger, "permission": permission["permission"]}


def _collect_attempts(api: API, repo: str, run: dict, record: dict, *,
                      release_push: bool = False) -> None:
    """Attempt endpoints are audit history; latest jobs alone are proving."""
    record.update(run=_run_record(run), history_complete=False, expected_attempts=None,
                  attempt_history=[])
    run_id, current = run.get("id"), run.get("run_attempt")
    if (type(run_id) is not int or run_id <= 0 or type(current) is not int
            or not 1 <= current <= ATTEMPT_LIMIT):
        raise ReleaseGateError("ci-post: incomplete attempt history: invalid run id or attempt limit")
    record["expected_attempts"] = list(range(1, current + 1))
    original = _actor_login(run.get("actor"))
    if release_push and original != RELEASE_PUSH_ACTOR:
        raise ReleaseGateError(f"release bump push actor must be {RELEASE_PUSH_ACTOR}")
    try:
        _actor_login(run.get("triggering_actor"))
    except ReleaseGateError as exc:
        raise ReleaseGateError(
            "ci-post: incomplete attempt history: latest metadata changed") from exc
    _observe_run(api, repo, run, record)
    for attempt in record["expected_attempts"]:
        audit = {"requested_attempt": attempt, "run": None, "jobs": None,
                 "jobs_complete": False, "trigger_permission": None,
                 "metadata_response": None, "jobs_response": None}
        record["attempt_history"].append(audit)
        endpoint = f"repos/{repo}/actions/runs/{run_id}/attempts/{attempt}"
        audit["metadata_response"] = _response_slot(
            api, endpoint, run_id, attempt, "attempt_metadata")
        metadata = _read_json(api, endpoint, response=audit["metadata_response"])
        if not isinstance(metadata, dict):
            raise ReleaseGateError("ci-post: incomplete attempt history: invalid metadata")
        audit["run"] = _run_record(metadata)
        if (type(metadata.get("id")) is not int or metadata["id"] != run_id
                or type(metadata.get("run_attempt")) is not int
                or metadata["run_attempt"] != attempt
                or any(metadata.get(key) != run.get(key)
                       for key in ("head_sha", "head_branch", "event"))
                or _actor_login(metadata.get("actor")) != original
                or not isinstance(metadata.get("status"), str)
                or "conclusion" not in metadata
                or (metadata["conclusion"] is not None
                    and not isinstance(metadata["conclusion"], str))):
            raise ReleaseGateError("ci-post: incomplete attempt history: gapped or mismatched metadata")
        trigger = _actor_login(metadata.get("triggering_actor"))
        audit["trigger_permission"] = _trigger_permission(api, repo, original, trigger, attempt)
        if attempt == current and any(metadata.get(key) != run.get(key)
                                      for key in ("status", "conclusion", "triggering_actor")):
            raise ReleaseGateError("ci-post: incomplete attempt history: latest metadata changed")
        audit["jobs_response"] = _response_slot(
            api, endpoint + "/jobs?per_page=100", run_id, attempt, "attempt_jobs")
        jobs = _ci_post_jobs(api, repo, run_id, attempt=attempt,
                             response=audit["jobs_response"])
        if any(("run_id" in job and (type(job["run_id"]) is not int or job["run_id"] != run_id))
               or ("head_sha" in job and job["head_sha"] != run["head_sha"])
               or ("run_attempt" in job and (
                   type(job["run_attempt"]) is not int or job["run_attempt"] != attempt))
               for job in jobs):
            raise ReleaseGateError("ci-post: incomplete attempt history: mismatched job identity")
        audit["jobs"] = [_job_record(job) for job in jobs]
        audit["jobs_complete"] = True
    record["history_complete"] = True


def _carried_associations(proving: list[dict], history: list[dict]) -> list[dict]:
    """Describe matching evidence, without claiming a physical execution attempt."""
    result = []
    for job in proving:
        if (sum(other["name"] == job["name"] for other in proving) != 1
                or not job.get("started_at") or not job.get("completed_at")
                or not isinstance(job.get("steps"), list) or not job["steps"]):
            continue
        matches = []
        for attempt in history:
            rows = [row for row in attempt["jobs"] or [] if row["name"] == job["name"]]
            if (len(rows) == 1 and rows[0].get("started_at") == job["started_at"]
                    and rows[0].get("completed_at") == job["completed_at"]
                    and rows[0].get("steps") == job["steps"]):
                matches.append({"api_attempt": attempt["requested_attempt"],
                                "job_id": rows[0]["id"]})
        if len(matches) > 1:
            result.append({"proving_job_id": job["id"], "name": job["name"],
                           "earliest_matching_api_attempt": matches[0]["api_attempt"],
                           "matches": matches,
                           "basis": "unique name, identical non-null times and steps"})
    return result


def gh_api(endpoint: str, *, paginate: bool = False, jq: str | None = None) -> bytes:
    command = ["gh", "api", endpoint, "--include"]
    if paginate:
        command.append("--paginate")
    if jq is not None:
        command.extend(["--jq", jq])
    return subprocess.run(command, check=True, capture_output=True, text=False).stdout


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
               response: dict | None = None, **options) -> str:
    try:
        raw = api(endpoint, **options)
    except subprocess.CalledProcessError as exc:
        if response is not None and isinstance(exc.stdout, bytes):
            _capture_response(response, exc.stdout)

        def diagnostic(value):
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value
        detail = diagnostic(exc.stderr or exc.stdout or str(exc)).strip()
        reason = _http_failure(
            f"{diagnostic(exc.stdout or '')}\n{diagnostic(exc.stderr or '')}",
            exception=True) or "failed"
        raise ReleaseGateError(f"API {endpoint} {reason}: {detail}") from exc
    except Exception as exc:
        reason = _http_failure(str(exc), exception=True) or "failed"
        raise ReleaseGateError(f"API {endpoint} {reason}: {exc}") from exc
    if not isinstance(raw, (str, bytes)):
        raise ReleaseGateError(f"API {endpoint} returned a non-text payload")
    # A compatible injected transport may expose a string view plus the
    # original bytes. Plain text cannot be relabelled as raw-byte evidence.
    received = raw if isinstance(raw, bytes) else getattr(raw, "received_bytes", None)
    if response is not None:
        if not isinstance(received, bytes):
            raise ReleaseGateError(f"API {endpoint} has no original response bytes")
        _capture_response(response, received)
    if isinstance(received, bytes):
        raw = received.decode("utf-8")
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
               items_key: str | None = None, complete_rulesets: bool = False,
               response: dict | None = None):
    raw = _read_text(api, endpoint, paginate=paginate,
                    complete_pages="ruleset" if complete_rulesets else items_key,
                    response=response)
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
    if response is not None:
        response["complete"] = True
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


def _require_release_push(api: API, repo: str, sha: str, receipt: dict | None = None) -> None:
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
        record = {}
        if receipt is not None:
            receipt["push_provenance"].append(record)
        _collect_attempts(api, repo, row, record, release_push=True)
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
                               "(token cannot see this ruleset's bypass_actors)")
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


def _ruleset_inventory_report(api: API, repo: str) -> str:
    """Measure every branch ruleset without deciding the release verdict."""
    lines = [
        "branch ruleset inventory (includes_parents=true; branch targets only; tag/push omitted):"]
    try:
        endpoint = f"repos/{repo}"
        project = _read_json(api, endpoint)
        if not isinstance(project, dict) or project.get("default_branch") != "main":
            raise ReleaseGateError(f"API {endpoint} must identify main as the default branch")

        endpoint = f"repos/{repo}/rulesets?includes_parents=true&per_page=100"
        listed = _read_json(api, endpoint, complete_rulesets=True, paginate=True)
        identities, entries = set(), []
        # Match the verdict reader's complete inventory and summary validation.
        for item in listed:
            if (not isinstance(item, dict) or type(item.get("id")) is not int or item["id"] <= 0
                    or not isinstance(item.get("target"), str)
                    or item["target"] not in ("branch", "tag", "push")
                    or not isinstance(item.get("enforcement"), str)
                    or item["enforcement"] not in ("active", "disabled", "evaluate")):
                raise ReleaseGateError(
                    f"API {endpoint} returned invalid ruleset summary: {item!r}")
            if item["id"] in identities:
                raise ReleaseGateError(
                    f"API {endpoint} returned duplicate ruleset id {item['id']}")
            identities.add(item["id"])
            entries.append(item)
    except ReleaseGateError as exc:
        lines.append(f"inventory=ERROR {str(exc)[:240]!r}")
        return "\n".join(lines)

    for item in entries:
        if item["target"] != "branch":
            continue
        endpoint = f"repos/{repo}/rulesets/{item['id']}"
        display = item
        try:
            entry = _read_json(api, endpoint)
            if (not isinstance(entry, dict) or type(entry.get("id")) is not int
                    or any(entry.get(key) != item[key]
                           for key in ("id", "target", "enforcement"))):
                raise ReleaseGateError(f"API {endpoint} returned inconsistent ruleset detail")
            display = {**item, **entry}
            bypass_list = entry.get("bypass_actors")
            visibility = (f"visible actors={len(bypass_list)}"
                          if isinstance(bypass_list, list) else "hidden")
            try:
                applies = "yes" if _ruleset_applies_to_main(entry, endpoint) else "no"
            except ReleaseGateError:
                applies = "unreadable"
            rule_rows = entry.get("rules")
            pull_request = "unreadable"
            if isinstance(rule_rows, list):
                pull_request = "yes" if any(
                    isinstance(rule, dict) and rule.get("type") == "pull_request"
                    for rule in rule_rows) else "no"
            evidence = (f"bypass_actors={visibility} applies={applies} "
                        f"pull_request={pull_request}")
        except ReleaseGateError as exc:
            evidence = f"detail=ERROR {str(exc)[:240]!r}"
        lines.append(
            f"ruleset id={item['id']} name={display.get('name', '<unknown>')!r} "
            f"source_type={display.get('source_type', '<unknown>')!r} "
            f"source={display.get('source', '<unknown>')!r} "
            f"enforcement={display['enforcement']!r} "
            f"updated_at={display.get('updated_at', '<unknown>')!r} {evidence}")
    return "\n".join(lines)


def rulesets_probe(repo: str, *, api: API) -> str:
    """Print the complete measurement, then use the bump's unchanged verdict."""
    print(_ruleset_inventory_report(api, repo), flush=True)
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


def _release_bump(api: API, repo: str, sha: str, *, expected_parent: str | None = None,
                  git: Git | None = None, receipt: dict | None = None) -> str:
    _require_release_push(api, repo, sha, receipt)
    _require_release_rulesets(api, repo)
    candidate = _commit_info(api, repo, sha)
    parents = candidate.get("parents")
    if (not isinstance(parents, list) or len(parents) != 1
            or not isinstance(parents[0], dict) or not _is_sha(parents[0].get("sha"))
            or parents[0]["sha"] == sha):
        raise ReleaseGateError("release bump requires exactly one valid parent")
    parent = parents[0]["sha"]
    if receipt is not None:
        receipt["parent_sha"] = parent
        receipt["requested"]["tree"] = candidate["commit"]["tree"]["sha"]
    if expected_parent is not None and parent != expected_parent:
        raise ReleaseGateError("release bump parent does not equal the phase1 dispatch SHA")
    trees = {sha: candidate["commit"]["tree"]["sha"]}
    # The legacy path requires a merged PR, never another bump. Explicit tag
    # admission honors the parent's fixed receipt cut-over without fallback.
    # Cache commit trees so each endpoint is still read at most once.
    if expected_parent is not None and _uses_main_proof(parent, git or git_is_ancestor):
        evidence = _main_proof_evidence(repo, parent, receipt=receipt)
        trees[parent] = _commit_tree(api, repo, parent)
        if receipt is not None:
            receipt["advisory_verify"]["tree"] = trees[parent]
            for context in receipt["contexts"].values():
                context["tree"] = trees[parent]
    else:
        evidence = _phase_one(api, repo, parent, trees=trees, receipt=receipt)
    if receipt is not None:
        receipt["parent_tree"] = trees[parent]
        if receipt["dispatch_sha"] == parent:
            receipt["dispatch_tree"] = trees[parent]
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


def _history_git(*args: str) -> str:
    command = ["git", *args]
    try:
        result = subprocess.run(command, check=False, capture_output=True, text=True)
    except (OSError, UnicodeError) as exc:
        raise OSError(f"{' '.join(command)} failed: {exc}") from exc
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise OSError(f"{' '.join(command)} failed: {detail}")
    return result.stdout


def git_history(sha: str) -> tuple[str | None, list[str]]:
    """Read the first parent and every changed path in the current checkout."""
    revision = f"{sha}^"
    try:
        parent = _history_git("rev-parse", revision).strip()
    except OSError:
        # A missing parent is a root only if the commit itself is readable
        # and its parent list is empty. Other rev-parse failures stay errors.
        if _history_git("rev-list", "--parents", "--max-count=1", sha).split() == [sha]:
            return None, []
        raise
    if not _is_sha(parent):
        raise OSError(f"git rev-parse {revision} returned an invalid first parent: {parent!r}")
    # NUL framing preserves paths containing newlines or non-ASCII characters;
    # disabling renames exposes both the old and new path to the classifier.
    listed = _history_git("diff", "--name-only", "--no-renames", "--no-ext-diff",
                          "-z", revision, sha, "--")
    if listed and not listed.endswith("\0"):
        raise OSError(f"git diff {revision} {sha} returned an unreadable changed-path list")
    return parent, listed[:-1].split("\0") if listed else []


def _ci_post_proof(sha: str, history: History, *,
                    receipt: dict | None = None) -> tuple[str, list[str]]:
    """Walk documentation with the same classifier as merge admission."""
    proof, skipped = sha, []
    while True:
        try:
            parent, paths = history(proof)
        except (OSError, ValueError) as exc:
            raise ReleaseGateError(f"ci-post: walk-blocked {proof}: {exc}") from exc
        if parent is None:
            raise ReleaseGateError(f"ci-post: walk-blocked {proof}: missing first parent")
        if not _is_sha(parent) or parent == proof:
            raise ReleaseGateError(
                f"ci-post: walk-blocked {proof}: invalid first parent {parent!r}")
        if (not isinstance(paths, list)
                or any(not isinstance(path, str) or not path or "\0" in path for path in paths)):
            raise ReleaseGateError(
                f"ci-post: walk-blocked {proof}: unreadable changed-path list")
        if not paths or not all(is_documentation(path) for path in paths):
            return proof, skipped
        if receipt is not None:
            receipt["docs_chain"].append({
                "sha": proof, "parent": parent, "changed_paths": list(paths),
                "documentation_only": True, "run": None, "audit": None})
        skipped.append(proof)
        if len(skipped) > DOCS_ONLY_WALK_LIMIT:
            raise ReleaseGateError(
                f"ci-post: walk-too-long {sha}: more than "
                f"{DOCS_ONLY_WALK_LIMIT} docs-only commits")
        proof = parent


def _ci_post_runs(api: API, repo: str, sha: str) -> list[dict]:
    endpoint = f"repos/{repo}/actions/workflows/ci-post.yml/runs?head_sha={sha}&per_page=100"
    try:
        runs = _read_json(api, endpoint, paginate=True, items_key="workflow_runs")
        if len(runs) >= 1000:
            raise ReleaseGateError(
                f"API {endpoint} returned incomplete workflow_runs pagination: "
                "filtered search reached the 1000-run limit")
        matches, seen = [], set()
        for row in runs:
            if (not isinstance(row, dict) or not _is_sha(row.get("head_sha"))
                    or "head_branch" not in row
                    or (row["head_branch"] is not None
                        and not isinstance(row["head_branch"], str))):
                raise ReleaseGateError(
                    f"API {endpoint} returned invalid workflow run: {row!r}")
            if row["head_branch"] != "main" or row["head_sha"] != sha:
                continue
            if (type(row.get("id")) is not int or row["id"] <= 0
                    or not isinstance(row.get("event"), str) or not row["event"]
                    or row["id"] in seen):
                raise ReleaseGateError(
                    f"API {endpoint} returned invalid or duplicate workflow run: {row!r}")
            try:
                created = datetime.strptime(row.get("created_at"), "%Y-%m-%dT%H:%M:%SZ")
            except (TypeError, ValueError) as exc:
                raise ReleaseGateError(
                    f"API {endpoint} returned invalid created_at: {row.get('created_at')!r}") from exc
            if row["event"] not in ("push", "schedule", "workflow_dispatch"):
                continue
            seen.add(row["id"])
            matches.append((created, row["id"], row))
        return [row for _, _, row in sorted(matches, key=lambda item: item[:2], reverse=True)]
    except ReleaseGateError as exc:
        raise ReleaseGateError(f"ci-post: unreadable {exc}") from exc


def _ci_post_jobs(api: API, repo: str, run_id: int, *,
                   attempt: int | None = None, response: dict | None = None) -> list[dict]:
    scope = f"/attempts/{attempt}" if attempt is not None else ""
    endpoint = f"repos/{repo}/actions/runs/{run_id}{scope}/jobs?per_page=100"
    try:
        jobs = _read_json(api, endpoint, paginate=True, items_key="jobs", response=response)
        if len(jobs) > JOBS_LIMIT:
            raise ReleaseGateError(f"API {endpoint} exceeded the jobs inventory limit")
        if any(not isinstance(job, dict) or not isinstance(job.get("name"), str)
               or not job["name"] for job in jobs):
            raise ReleaseGateError(f"API {endpoint} returned an invalid job row")
        if any("steps" in job and (
                not isinstance(job["steps"], list)
                or any(not isinstance(step, dict) for step in job["steps"])) for job in jobs):
            raise ReleaseGateError(f"API {endpoint} returned invalid job steps")
        seen = set()
        for job in jobs:
            job_id = job.get("id")
            if type(job_id) is not int or job_id <= 0 or job_id in seen:
                raise ReleaseGateError(f"API {endpoint} returned invalid or duplicate job id")
            seen.add(job_id)
        return jobs
    except ReleaseGateError as exc:
        if response is not None:
            response["complete"] = False
        raise ReleaseGateError(f"ci-post: unreadable {exc}") from exc


def _require_ci_post(api: API, repo: str, sha: str, history: History, *,
                      receipt: dict | None = None) -> str:
    proof, docs_checked = _ci_post_proof(sha, history, receipt=receipt)
    if receipt is not None:
        receipt["ci_post"]["proof"] = {"sha": proof, "tree": None}
    runs = None
    for docs_sha in docs_checked:
        docs_runs = _ci_post_runs(api, repo, docs_sha)
        if not docs_runs:
            raise ReleaseGateError(f"ci-post: docs-commit-no-run sha {docs_sha}")
        docs_run = docs_runs[0]
        chain = next((row for row in receipt["docs_chain"] if row["sha"] == docs_sha),
                     None) if receipt is not None else None
        if chain is not None:
            chain["run"] = _run_record(docs_run)
        audit = {}
        if chain is not None:
            chain["audit"] = audit
        _collect_attempts(api, repo, docs_run, audit)
        if (docs_run.get("status") != "completed"
                or docs_run.get("conclusion") != "success"):
            raise ReleaseGateError(
                f"ci-post: docs-commit-red {docs_run['id']} sha {docs_sha}: "
                f"status {docs_run.get('status')} "
                f"conclusion {docs_run.get('conclusion')} event {docs_run['event']}")
        if docs_sha == proof:
            runs = docs_runs
    if runs is None:
        runs = _ci_post_runs(api, repo, proof)
    if not runs:
        raise ReleaseGateError(f"ci-post: no-run {proof}")
    reasons, newer_skipped = [], 0
    for index, run in enumerate(runs[:CI_POST_RUN_LIMIT]):
        identity = f"{run['id']} sha {proof} event {run['event']}"
        record = {"run": _run_record(run), "proving_source": "default latest jobs API",
                  "proving_jobs": None, "proving_jobs_complete": False,
                  "proving_response": None,
                  "history_complete": False, "attempt_history": [],
                  "expected_attempts": None,
                  "carried_associations": []}
        if receipt is not None:
            receipt["ci_post"]["runs"].append(record)
        if run.get("status") != "completed":
            raise ReleaseGateError(
                f"ci-post: run-in-progress {identity}: status {run.get('status')}")
        _observe_run(api, repo, run, record)
        required = {name: [] for name in CI_POST_BUILD_JOBS}
        record["proving_response"] = _response_slot(
            api, f"repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100",
            run["id"], run["run_attempt"], "proving_jobs")
        jobs = _ci_post_jobs(api, repo, run["id"], response=record["proving_response"])
        record["proving_jobs"] = [_job_record(job) for job in jobs]
        record["proving_jobs_complete"] = True
        _collect_attempts(api, repo, run, record)
        if any(("run_id" in job and (
                   type(job["run_id"]) is not int or job["run_id"] != run["id"]))
               or ("head_sha" in job and job["head_sha"] != proof)
               or ("run_attempt" in job and (
                   type(job["run_attempt"]) is not int or job["run_attempt"] != run["run_attempt"]))
               for job in jobs):
            raise ReleaseGateError("ci-post: mismatched proving job identity")
        record["carried_associations"] = _carried_associations(
            record["proving_jobs"], record["attempt_history"])
        for job in jobs:
            name, conclusion = job["name"], job.get("conclusion")
            # A failed leg anywhere in the returned inventory vetoes the run.
            # Keep failure diagnostics ahead of duplicate-name diagnostics.
            if conclusion in CI_POST_FAILURES:
                raise ReleaseGateError(
                    f"ci-post: newer-run-failed {identity}: job {name}={conclusion}")
        seen = set()
        for job in jobs:
            name = job["name"]
            if name in seen:
                raise ReleaseGateError(f"ci-post: duplicate-job {identity}: job {name}")
            seen.add(name)
            if name in required:
                required[name].append(job.get("conclusion"))
        full = all(values and all(value == "success" for value in values)
                   for values in required.values())
        conclusion = run.get("conclusion")
        # Fail closed for a run that does not prove (the newer-run table). Past the
        # failed-build check above, a non-proving run may be passed over for an
        # older proof only when it is a run-level success whose required jobs are all
        # present and each success or skipped, or a run-level skipped run whose
        # required jobs are all skipped. Every other conclusion refuses, and so does a
        # leg that is neutral, null, unknown or missing while an older run exists.
        leg_values = [value for rows in required.values() for value in rows]
        leg_problems = []
        for name, rows in required.items():
            if not rows:
                leg_problems.append(f"missing job {name}")
            else:
                leg_problems.extend(
                    f"job {name}={value}" for value in rows
                    if value not in ("success", "skipped"))
        has_older = index + 1 < len(runs[:CI_POST_RUN_LIMIT])
        if conclusion == "success":
            passes_over = not leg_problems
            refuse = bool(leg_problems) and has_older
            contradiction = leg_problems
        elif conclusion == "skipped":
            contradiction = leg_problems or [
                f"job {name}={value}" for name, rows in required.items()
                for value in rows if value != "skipped"]
            passes_over = not contradiction
            refuse = bool(contradiction)
        else:
            contradiction = leg_problems
            passes_over = False
            refuse = True
        if refuse:
            raise ReleaseGateError(
                f"ci-post: not-success {identity}: conclusion {conclusion}"
                + (f": {', '.join(contradiction)}" if contradiction else ""))
        if full and conclusion == "success":
            if isinstance(api, _GateReads):
                api.selected_run_id = run["id"]
            if receipt is not None:
                receipt["ci_post"]["selected_run_id"] = run["id"]
                receipt["ci_post"]["proof"]["tree"] = _commit_tree(api, repo, proof)
            evidence = f"ci-post run {run['id']} for {proof}"
            if docs_checked:
                evidence += f"; requested release {sha}"
                if receipt is not None:
                    evidence += (f" tree {receipt['requested']['tree']}; "
                                 f"proof tree {receipt['ci_post']['proof']['tree']}")
                evidence += f"; {len(docs_checked)} docs-only commit(s) checked"
            if newer_skipped:
                evidence += f"; {newer_skipped} newer skipped-only run(s) skipped"
            return evidence

        present = all(required.values())
        if passes_over:
            newer_skipped += 1
        if present and all(value == "skipped" for value in leg_values):
            detail = ["all four build jobs skipped"]
        else:
            detail = []
            for name, rows in required.items():
                if not rows:
                    detail.append(f"missing job {name}")
                else:
                    detail.extend(f"job {name}={value}" for value in rows if value != "success")
        if conclusion != "success":
            detail.append(f"run conclusion {conclusion}")
        reasons.append(f"run {identity}: {', '.join(detail)}")
    raise ReleaseGateError(
        f"ci-post: no-full-run {proof}: examined {len(reasons)} of {len(runs)} run(s); "
        + "; ".join(reasons))


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


def _context_evidence(latest: dict, *, sha: str, tree: str | None,
                       source: str, records: dict | None = None,
                       receipt: dict | None = None) -> str:
    if receipt is not None:
        receipt["contexts"] = {
            context: {"presence": context in latest, "state": latest.get(context, "missing"),
                      "sha": sha, "tree": tree, "source": source,
                      "status": (records or {}).get(context)}
            for context in (*CONTEXTS, "pineforge/verify")}
        receipt["advisory_verify"] = {
            "context": "pineforge/verify", "complete": True,
            **receipt["contexts"]["pineforge/verify"]}
    return f"pineforge/verify={latest.get('pineforge/verify', 'missing')} (advisory)"


def _main_proof_evidence(repo: str, sha: str, *, receipt: dict | None = None) -> str:
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
    advisory = _context_evidence(proof, sha=sha, tree=None, source="main-proof context map",
                                 receipt=receipt)
    return f"{subject}; {_require_full_contexts(proof, subject)}; {advisory}"


def _release_checkout(sha: str, release_git=None):
    snapshot = release_git if release_git is not None else catalog_diff.Git(
        Path(__file__).resolve().parents[1], sha)
    if snapshot.resolve("HEAD") != sha or snapshot.resolve(snapshot.ref) != sha:
        raise ReleaseGateError("release checkout is not the exact dispatch SHA")
    return snapshot


def gate(repo: str, sha: str, ref: str, *, api: API, git: Git = git_is_ancestor,
         history: History = git_history, phase: str | None = None,
         dispatch_sha: str = "", release_git=None, receipt: dict | None = None) -> str:
    api = _memo_api(api, receipt=receipt)
    if ref != "refs/heads/main":
        raise ReleaseGateError(f"release requires refs/heads/main, got {ref}")
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo) is None:
        raise ReleaseGateError("invalid --repo: expected OWNER/REPO")
    if not _is_sha(sha):
        raise ReleaseGateError(f"invalid release sha: {sha!r}")
    if phase not in (None, "bump", "tag"):
        raise ReleaseGateError("release phase must be bump or tag")
    if phase == "tag":
        if not _is_sha(dispatch_sha) or dispatch_sha == sha:
            raise ReleaseGateError("tag phase requires the phase1 dispatch SHA")
    elif dispatch_sha:
        raise ReleaseGateError("phase1 dispatch SHA is only an input to the tag phase")

    endpoint = f"repos/{repo}/branches/main"
    if receipt is not None:
        receipt["checkpoint"] = "current main"
    current = _read_text(api, endpoint, jq=".commit.sha")
    if receipt is not None:
        receipt["observed_main"] = current if _is_sha(current) else None
    if not _is_sha(current):
        raise ReleaseGateError(f"API {endpoint} returned invalid main sha: {current!r}")
    if current != sha:
        raise ReleaseGateError(f"release sha {sha} is not current main head {current}")

    if phase == "tag":
        if receipt is not None:
            receipt["checkpoint"] = "authenticated release bump and parent parity"
        evidence = _release_bump(api, repo, sha, expected_parent=dispatch_sha, git=git,
                                 receipt=receipt)
        snapshot = _release_checkout(sha, release_git)
        catalog_diff.validate_release_transition(
            catalog_diff.default_config(Path(__file__).resolve().parents[1]),
            snapshot, expected_parent=dispatch_sha, allow_tagged=True)
    elif _uses_main_proof(sha, git):
        if receipt is not None:
            receipt["checkpoint"] = "main-proof parity"
        evidence = _main_proof_evidence(repo, sha, receipt=receipt)
        if phase == "bump" and not _merged_pulls(api, repo, sha, allow_empty=True):
            raise ReleaseGateError(f"no merged pull request associated with {sha}; "
                                   "dispatch its existing bump with phase tag")
        if receipt is not None:
            tree = _commit_tree(api, repo, sha)
            receipt["requested"]["tree"] = tree
            receipt["advisory_verify"]["tree"] = tree
            for context in receipt["contexts"].values():
                context["tree"] = tree
    else:
        if receipt is not None:
            receipt["checkpoint"] = "PR head parity"
        evidence = _phase_one(api, repo, sha, allow_missing_pr=True, receipt=receipt)
        if evidence is None:
            if phase == "bump":
                raise ReleaseGateError(f"no merged pull request associated with {sha}; "
                                       "dispatch its existing bump with phase tag")
            try:
                evidence = _release_bump(api, repo, sha, receipt=receipt)
            except ReleaseGateError as exc:
                raise ReleaseGateError(
                    f"no merged pull request associated with {sha}; release bump refused: {exc}") from exc
    if receipt is not None:
        receipt["checkpoint"] = "ci-post and attempt history"
    post = _require_ci_post(api, repo, sha, history, receipt=receipt)
    if receipt is not None:
        receipt["checkpoint"] = "final uncached run validation"
    _revalidate_runs(api)
    if receipt is not None:
        receipt["observed_main_tree"] = receipt["requested"]["tree"]
        receipt["gate"] = {"outcome": "admitted", "reasons": []}
    return f"release gate green: {evidence}; {post}"


def release_plan(sha: str, phase: str, bump: str, override: str,
                 dispatch_sha: str, *, release_git=None) -> dict[str, str]:
    """Plan one phase; tag reads the committed VERSION and never increments it."""
    if not _is_sha(sha):
        raise ReleaseGateError("invalid dispatch SHA")
    snapshot = _release_checkout(sha, release_git)
    current = catalog_diff._snapshot_file(snapshot, sha, "VERSION").decode("utf-8").strip()
    tags = snapshot.tags()
    if phase == "bump":
        if dispatch_sha:
            raise ReleaseGateError("phase1 dispatch SHA is only an input to the tag phase")
        return release_version.plan(current, bump, override, tags)
    if phase != "tag" or not _is_sha(dispatch_sha) or dispatch_sha == sha:
        raise ReleaseGateError("tag phase requires the phase1 dispatch SHA")
    if override or bump != "patch":
        raise ReleaseGateError("override and non-default bump apply only to phase bump")
    _, version = catalog_diff.validate_release_transition(
        catalog_diff.default_config(Path(__file__).resolve().parents[1]),
        snapshot, expected_parent=dispatch_sha, allow_tagged=True)
    parsed = release_version.parse_version(version)
    return {"current": current, "next": version, "tag": "v" + version,
            "prerelease": "true" if parsed.prerelease else "false",
            "previous": release_version.previous_tag(parsed, release_version.parse_tags(tags))}


def _phase_one(api: API, repo: str, sha: str, *, allow_missing_pr: bool = False,
               trees: dict[str, str] | None = None, receipt: dict | None = None) -> str | None:
    merged = _merged_pulls(api, repo, sha, allow_empty=allow_missing_pr)
    if not merged:
        return None
    if trees is None:
        trees = {}
    if sha not in trees:
        trees[sha] = _commit_tree(api, repo, sha)
    tree = trees[sha]
    if receipt is not None and receipt["requested"]["sha"] == sha:
        receipt["requested"]["tree"] = tree
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
    latest, records = {}, {}
    for row in _read_json(api, endpoint, paginate=True):
        if (not isinstance(row, dict) or not isinstance(row.get("context"), str)
                or not row["context"]):
            raise ReleaseGateError(f"API {endpoint} returned invalid status row: {row!r}")
        context = row["context"]
        # GitHub returns newest first. Never let an older row override it.
        if context in latest:
            continue
        if context in (*CONTEXTS, "pineforge/verify"):
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
        if context in (*CONTEXTS, "pineforge/verify"):
            records[context] = {
                **_fields(row, ("id", "context", "state", "target_url", "created_at", "updated_at")),
                "creator": _actor_record(row.get("creator"))}
    advisory = _context_evidence(latest, sha=head, tree=tree, source="PR head statuses",
                                 records=records, receipt=receipt)
    states = _require_full_contexts(latest, f"PR #{number} head {head}")
    return f"PR #{number} head {head}; {states}; tree {tree}; {advisory}"


def main(argv=None, *, api: API | None = None, git: Git | None = None,
         history: History | None = None, release_git=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    check = modes.add_parser("gate", help="require full release proof for the current main sha")
    check.add_argument("--repo", required=True)
    check.add_argument("--sha", required=True)
    check.add_argument("--ref", required=True)
    check.add_argument("--phase", choices=("bump", "tag"))
    check.add_argument("--dispatch-sha", default="")
    check.add_argument("--receipt", type=Path, help="write allowlisted decision evidence, including refusal")
    check.add_argument("--dry-run", choices=("true", "false"), default="false")
    plan = modes.add_parser("plan", help="plan the bump or read the admitted tag-phase VERSION")
    plan.add_argument("--sha", required=True)
    plan.add_argument("--phase", choices=("bump", "tag"), required=True)
    plan.add_argument("--bump", choices=("patch", "minor", "major"), default="patch")
    plan.add_argument("--override", default="")
    plan.add_argument("--dispatch-sha", default="")
    plan.add_argument("--github-output", type=Path, required=True)
    probe = modes.add_parser("rulesets-probe", help="read the release bump's ruleset barrier only")
    probe.add_argument("--repo", required=True)
    args = parser.parse_args(argv)
    receipt = (_new_receipt(args.repo, args.sha, args.phase, args.dispatch_sha,
                            args.dry_run == "true") if args.mode == "gate" else None)
    code = 0
    try:
        if args.mode == "rulesets-probe":
            green = rulesets_probe(args.repo, api=api if api is not None else gh_api)
        elif args.mode == "plan":
            fields = release_plan(args.sha, args.phase, args.bump, args.override,
                                  args.dispatch_sha, release_git=release_git)
            green = "\n".join(f"{key}={value}" for key, value in fields.items())
            with args.github_output.open("a", encoding="utf-8") as output:
                output.write(green + "\n")
        else:
            green = gate(args.repo, args.sha, args.ref, api=api if api is not None else gh_api,
                         git=git if git is not None else git_is_ancestor,
                         history=history if history is not None else git_history,
                         phase=args.phase, dispatch_sha=args.dispatch_sha,
                         release_git=release_git, receipt=receipt)
    except (ReleaseGateError, catalog_diff.Finding, catalog_diff.InputError,
            release_version.ReleaseError, OSError, UnicodeError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        code = 1
        if receipt is not None:
            # Exception text may contain transport stderr, source, or credentials.
            # Record a safe reason and the failed checkpoint; retain typed evidence.
            receipt["gate"] = {
                "outcome": "refused",
                "reasons": [{"kind": type(exc).__name__, "check": receipt["checkpoint"]}]}
    if receipt is not None:
        _bind_response_inventory(receipt)
    if args.mode == "gate" and args.receipt is not None:
        try:
            args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                                    encoding="utf-8")
        except (OSError, UnicodeError):
            print("::error::cannot write release gate receipt", file=sys.stderr)
            return 1
    if code:
        return code
    print(green)
    return 0


if __name__ == "__main__":
    sys.exit(main())
