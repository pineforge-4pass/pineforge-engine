#!/usr/bin/env python3
"""Require authenticated recorded native output when a PR changes its headline.

Execute this file from the trusted PR base. Candidate Git objects and PR body
attachments are data. No shell text, candidate modules, or artifact code runs.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPOSITORY = "pineforge-4pass/pineforge-engine"
CORPUS_REPOSITORY = "pineforge-4pass/pineforge-corpus"
PARITY = "scripts/check_corpus_parity.sh"
WORKFLOW = ".github/workflows/corpus-parity.yml"
SCHEMA = "pineforge-corpus-headline-v1"
SHA = re.compile(r"[0-9a-f]{40}")
DIGEST = re.compile(r"[0-9a-f]{64}")
PRODUCER_PATHS = (
    WORKFLOW, "scripts/record_corpus_headline_evidence.py",
    "scripts/check_corpus_headline_evidence.py", PARITY,
    "scripts/run_corpus.sh", "scripts/run_strategy.py", "scripts/verify_corpus.py",
    "scripts/parity_paths.py", "scripts/derive_corpus_feeds.py",
)
# Unknown tracked inputs are INCLUDED. Only named guard/CI files and prose are
# excluded; the pin alone is normalized below. This includes build files,
# native/runtime code, grader dependencies, corpus gitlink and feed derivation.
NONSCIENTIFIC = frozenset((
    "AGENTS.md", "CLAUDE.md", "LICENSE", "NOTICE",
    "scripts/check_corpus_headline_evidence.py",
    "scripts/test_check_corpus_headline_evidence.py",
    "scripts/record_corpus_headline_evidence.py",
    "scripts/test_record_corpus_headline_evidence.py",
    "scripts/ci_preflight.py", "scripts/test_ci_preflight.py",
))
NATIVE_SUMMARY = re.compile(rb"ran ([1-9][0-9]*) strategies in [0-9]+s -- ok=([0-9]+) fail=([0-9]+)\n")
HEADLINE = re.compile(
    rb"Verified ([1-9][0-9]*) strategies \xe2\x80\x94 excellent=([0-9]+), "
    rb"strong=([0-9]+), moderate=([0-9]+), weak=([0-9]+), minimal=([0-9]+), "
    rb"anomaly=([0-9]+), engine_only=([0-9]+), missing=([0-9]+)")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def json_object(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result
    def constant(value):
        raise ValueError("invalid JSON constant")
    value = json.loads(data, object_pairs_hook=pairs, parse_constant=constant)
    require(isinstance(value, dict), "expected JSON object")
    return value


def git(repo, *args):
    return subprocess.run(["git", "--no-optional-locks", "-C", str(repo), *args],
                          check=True, capture_output=True).stdout


def tree(repo, revision):
    require(bool(SHA.fullmatch(revision)), "revision must be a full commit SHA")
    result = {}
    for row in git(repo, "ls-tree", "-rz", "--full-tree", revision).split(b"\0"):
        if not row:
            continue
        info, name = row.split(b"\t", 1)
        mode, kind, oid = info.decode("ascii").split()
        path = name.decode("utf-8")
        require(not path.startswith("/") and all(p not in ("", ".", "..")
                                                for p in path.split("/")), "unsafe tree path")
        result[path] = (mode, kind, oid)
    return result


def blob(repo, revision, path):
    entry = tree(repo, revision).get(path)
    require(entry is not None and entry[0] in ("100644", "100755")
            and entry[1] == "blob", f"not a regular tracked file: {path}")
    return git(repo, "cat-file", "blob", entry[2])


def assignment(script):
    """One supported, literal, top-level assignment; never evaluate shell."""
    require(b"\r" not in script and b"\0" not in script, "unparseable shell text")
    matches = []
    lines = script.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if line.lstrip().startswith(b"#"):
            continue
        if line.startswith(b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-'):
            match = re.fullmatch(rb'EXPECTED_VERIFY="\$\{EXPECTED_VERIFY:-([^"$`\\{}\r\n]*)\}"\n?', line)
        else:
            match = re.fullmatch(rb'EXPECTED_VERIFY=(?:"([^"$`\\{}\r\n]*)"|\'([^\'\r\n]*)\')\n?', line)
        if match:
            value = next(group for group in match.groups() if group is not None)
            require(bool(value), "empty headline default")
            matches.append((index, value))
            continue
        references_removed = re.sub(rb"\$\{EXPECTED_VERIFY\}|\$EXPECTED_VERIFY\b", b"", line)
        require(not re.search(rb"\bEXPECTED_VERIFY\b", references_removed),
                "dynamic or ambiguous EXPECTED_VERIFY assignment/use")
    require(len(matches) == 1, "expected exactly one literal headline assignment")
    index, value = matches[0]
    lines[index] = b"EXPECTED_VERIFY=<headline-default>\n"
    return value, b"".join(lines)


def compared(repo, base, head):
    require(bool(SHA.fullmatch(base)) and bool(SHA.fullmatch(head)), "full PR SHAs required")
    ancestor = git(repo, "merge-base", "--all", base, head).decode().strip()
    require(bool(SHA.fullmatch(ancestor)), "ambiguous or missing PR merge-base")
    require(ancestor != head, "empty or reversed PR comparison")
    return ancestor, assignment(blob(repo, ancestor, PARITY))[0], assignment(blob(repo, head, PARITY))[0]


def excluded(path):
    return (path in NONSCIENTIFIC or path.startswith((".github/", "operator/"))
            or (path.startswith("docs/") and Path(path).suffix in (".md", ".rst", ".svg", ".png"))
            or ("/" not in path and path.endswith(".md")))


def engine_identity(repo, revision):
    rows = []
    for path, (mode, kind, oid) in sorted(tree(repo, revision).items()):
        if excluded(path):
            continue
        if path == PARITY:
            oid = "sha256:" + sha256(assignment(blob(repo, revision, path))[1])
        rows.append([path, mode, kind, oid])
    require(bool(rows), "empty scientific source")
    return sha256(json_bytes(rows))


def scientific(repo, revision):
    entries = tree(repo, revision)
    corpus = entries.get("corpus")
    grader = entries.get("scripts/verify_corpus.py")
    require(corpus is not None and corpus[:2] == ("160000", "commit"), "missing corpus gitlink")
    require(grader is not None and grader[0] in ("100644", "100755")
            and grader[1] == "blob", "missing grader")
    return {"engine_sha256": engine_identity(repo, revision),
            "grader_git_blob": grader[2], "corpus_commit": corpus[2]}


def population(entries):
    """Mirror verifier discovery, not its grading or outcomes."""
    paths = set(entries)
    probes = set()
    for path in paths:
        parts = path.split("/")
        if len(parts) < 3 or parts[0] not in ("basic", "community", "validation"):
            continue
        probe = "/".join(parts[:2])
        if parts[1] == "symbol-specified" and probe + "/strategy.pine" not in paths:
            continue
        probes.add(probe)
    require(bool(probes), "empty expected verifier population")
    require(all(re.fullmatch(r"(basic|community|validation)/[A-Za-z0-9][A-Za-z0-9._-]*", p)
                for p in probes), "unsupported probe name")
    return sorted(probes)


def headline(raw):
    rows = [row for row in raw.split(b"\n") if row.startswith(b"Verified ")]
    require(len(rows) == 1, "missing or ambiguous native verifier headline")
    match = HEADLINE.fullmatch(rows[0])
    require(match is not None, "unparseable native verifier headline")
    counts = [int(value) for value in match.groups()]
    require(sum(counts[1:]) == counts[0] and counts[-1] == 0,
            "inconsistent or missing measured population")
    return rows[0], counts[0]


PYTHON_TOOLCACHE = "/opt/hostedtoolcache/"
PYTHON_CACHE_ID = re.compile(r"Python/([0-9]+\.[0-9]+\.[0-9]+)/(x64|arm64)")


def loader_environment(env):
    """Describe only the permitted loader binding, without absolute paths."""
    if "LD_LIBRARY_PATH" not in env:
        return {"LD_LIBRARY_PATH": "absent"}
    library = env["LD_LIBRARY_PATH"]
    require(isinstance(library, str), "invalid loader environment value")
    if library == "":
        return {"LD_LIBRARY_PATH": "empty"}
    location = env.get("pythonLocation")
    require(isinstance(location, str) and location.startswith(PYTHON_TOOLCACHE),
            "loader environment requires a standard pythonLocation")
    relative = location[len(PYTHON_TOOLCACHE):]
    require(PYTHON_CACHE_ID.fullmatch(relative) is not None
            and library == location + "/lib",
            "loader environment must be exactly pythonLocation/lib")
    return {
        "LD_LIBRARY_PATH": "pythonLocation/lib", "pythonLocation": relative,
        "pythonLocation_sha256": sha256(location.encode()),
        "LD_LIBRARY_PATH_sha256": sha256(library.encode()),
    }


def validate_loader_environment(value, python_version):
    require(isinstance(value, dict), "missing loader environment")
    mode = value.get("LD_LIBRARY_PATH")
    if mode in ("absent", "empty"):
        require(value == {"LD_LIBRARY_PATH": mode}, "unbound loader environment fields")
        return
    require(mode == "pythonLocation/lib", "unsupported loader environment")
    relative = value.get("pythonLocation")
    match = PYTHON_CACHE_ID.fullmatch(relative) if isinstance(relative, str) else None
    require(match is not None and match[1] == python_version,
            "loader environment location/version mismatch")
    location = PYTHON_TOOLCACHE + relative
    require(value == loader_environment({
        "pythonLocation": location, "LD_LIBRARY_PATH": location + "/lib",
    }), "loader environment binding/digest mismatch")


def validate_record(record, raw, summary, expected, inputs, probes, source, output_digest):
    require(record.get("schema") == SCHEMA, "unrecorded evidence schema")
    require(record.get("source_commit") == source, "stale measured source")
    require(record.get("scientific") == inputs, "wrong scientific inputs")
    require(record.get("population") == probes and bool(probes), "wrong expected population")
    require(bool(DIGEST.fullmatch(record.get("inputs_sha256", ""))), "missing input digest")
    verifier = record.get("verifier", {})
    require(type(verifier.get("exit")) is int and verifier["exit"] == 0, "failed/unexecuted verifier")
    require(sha256(raw) == output_digest == verifier.get("sha256"), "raw output digest mismatch")
    actual, total = headline(raw)
    require(actual == expected, "measured headline bytes differ from candidate default")
    require(total == len(probes) > 0, "partial measured verifier population")
    require(record.get("native_summary_sha256") == sha256(summary), "native control digest mismatch")
    match = NATIVE_SUMMARY.fullmatch(summary)
    require(match is not None, "missing native completion control")
    require(tuple(map(int, match.groups())) == (total, total, 0), "partial/failed native control")
    native = record.get("native", {})
    require(all(type(native.get(key)) is int and native[key] == value
                for key, value in (("exit", 0), ("total", total), ("ok", total), ("fail", 0))),
            "failed or partial native run")
    require(native.get("probes") == probes, "partial native probe set")
    for key in ("outputs", "libraries"):
        files = native.get(key)
        require(isinstance(files, dict) and set(files) == set(probes)
                and all(isinstance(d, str) and DIGEST.fullmatch(d) for d in files.values()),
                f"incomplete native {key}")
    require(bool(DIGEST.fullmatch(native.get("runtime_sha256", ""))), "missing native runtime")
    derived = record.get("derived_inputs")
    require(isinstance(derived, dict) and set(derived) == {
        "ohlcv_ETH-USDT-USDT_15m.csv", "ohlcv_ETH-USDT-USDT_15m_window.csv"}
        and all(isinstance(d, str) and DIGEST.fullmatch(d) for d in derived.values()),
        "missing derived scientific input identity")
    build = record.get("build", {})
    require(build.get("type") == "Release" and build.get("runner") == "ctypes"
            and isinstance(build.get("cache_sha256"), str) and DIGEST.fullmatch(build["cache_sha256"])
            and all(isinstance(build.get(k), str) and re.fullmatch(r"[0-9][0-9.]+", build[k])
                    for k in ("compiler", "cmake", "python")), "unsupported recording build")
    validate_loader_environment(record.get("environment"), build["python"])
    return total


def attachment(value):
    require(set(value) == {"run_id", "artifact_id", "artifact_sha256", "output_sha256"},
            "attachment must name run/artifact IDs and both SHA-256 digests")
    require(all(type(value[k]) is int and value[k] > 0 for k in ("run_id", "artifact_id")),
            "invalid run/artifact ID")
    require(all(isinstance(value[k], str) and DIGEST.fullmatch(value[k])
                for k in ("artifact_sha256", "output_sha256")), "invalid attachment digest")
    return value


def service_identity(run, artifact, attached, workflow_id):
    require(run.get("id") == attached["run_id"] and run.get("workflow_id") == workflow_id
            and run.get("path") == WORKFLOW and run.get("event") == "workflow_dispatch"
            and run.get("status") == "completed" and run.get("conclusion") in ("success", "failure"),
            "untrusted or incomplete producer run")
    require(run.get("repository", {}).get("full_name") == REPOSITORY
            and run.get("head_repository", {}).get("full_name") == REPOSITORY,
            "wrong producer repository")
    source, attempt = run.get("head_sha", ""), run.get("run_attempt")
    require(bool(SHA.fullmatch(source)) and type(attempt) is int and attempt > 0,
            "unreadable producer source/attempt")
    association = artifact.get("workflow_run", {})
    require(artifact.get("id") == attached["artifact_id"] and artifact.get("expired") is False
            and artifact.get("name") == f"corpus-headline-evidence-{attempt}"
            and artifact.get("digest") == "sha256:" + attached["artifact_sha256"]
            and association.get("id") == attached["run_id"]
            and association.get("head_sha") == source
            and type(run["repository"].get("id")) is int
            and association.get("repository_id") == run["repository"]["id"]
            and association.get("head_repository_id") == run["head_repository"].get("id"),
            "wrong/unrecorded artifact association or service digest")
    return source


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class Service:
    """Fixed GitHub authority; bearer token never follows an artifact redirect."""
    def request(self, path):
        require(path.startswith("/repos/") and not path.startswith("//"), "invalid service path")
        headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28",
                   "User-Agent": "pineforge-corpus-headline"}
        if os.environ.get("GH_TOKEN"):
            headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
        return urllib.request.Request("https://api.github.com" + path, headers=headers)

    def read(self, path):
        with urllib.request.build_opener(NoRedirect()).open(self.request(path), timeout=30) as response:
            require(re.search(r'rel="?next"?', response.headers.get("Link", "")) is None,
                    "truncated service response")
            data = response.read(8 * 1024 * 1024 + 1)
        require(len(data) <= 8 * 1024 * 1024, "service response too large")
        return json_object(data)

    def download(self, artifact_id):
        path = f"/repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip"
        try:
            response = urllib.request.build_opener(NoRedirect()).open(self.request(path), timeout=30)
        except urllib.error.HTTPError as error:
            require(error.code == 302, "artifact download refused")
            url = urllib.parse.urlsplit(error.headers.get("Location", ""))
            require(url.scheme == "https" and url.hostname and url.port in (None, 443)
                    and url.username is None and url.password is None
                    and (url.hostname.endswith(".blob.core.windows.net")
                         or url.hostname.endswith(".actions.githubusercontent.com")),
                    "untrusted artifact redirect")
            # New request, no API authorization header, and no further redirects.
            response = urllib.request.build_opener(NoRedirect()).open(url.geturl(), timeout=30)
        with response:
            data = response.read(8 * 1024 * 1024 + 1)
        require(0 < len(data) <= 8 * 1024 * 1024, "empty/oversized artifact")
        return data

    def source(self, path, revision):
        data = self.read(f"/repos/{REPOSITORY}/contents/{path}?ref={revision}")
        require(data.get("type") == "file" and data.get("encoding") == "base64", "missing producer source")
        raw = base64.b64decode("".join(data["content"].split()), validate=True)
        require(hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest() == data.get("sha"),
                "producer source blob mismatch")
        return raw


def unpack(data, digest):
    require(sha256(data) == digest, "service artifact digest mismatch")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = ["record.json", "verify_corpus.log", "native-summary.log"]
        require(sorted(archive.namelist()) == sorted(names), "unexpected/duplicate artifact members")
        for entry in archive.infolist():
            mode = entry.external_attr >> 16
            require(not stat.S_ISLNK(mode) and entry.file_size <= 4 * 1024 * 1024
                    and not entry.flag_bits & 1, "unsafe/oversized artifact member")
        return (json_object(archive.read(names[0])), archive.read(names[1]), archive.read(names[2]))


def check_service(service, attached, repo, base, head, expected):
    attached = attachment(attached)
    prefix = f"/repos/{REPOSITORY}"
    workflow = service.read(prefix + "/actions/workflows/corpus-parity.yml")
    require(workflow.get("path") == WORKFLOW and type(workflow.get("id")) is int, "wrong producer workflow")
    run = service.read(prefix + f"/actions/runs/{attached['run_id']}")
    artifact = service.read(prefix + f"/actions/artifacts/{attached['artifact_id']}")
    source = service_identity(run, artifact, attached, workflow["id"])
    for path in PRODUCER_PATHS:
        trusted, actual = blob(repo, base, path), service.source(path, source)
        if path == PARITY:
            trusted, actual = assignment(trusted)[1], assignment(actual)[1]
        require(actual == trusted, f"producer differs from accepted base: {path}")
    jobs = service.read(prefix + f"/actions/runs/{attached['run_id']}/attempts/{run['run_attempt']}/jobs?per_page=100")
    rows = jobs.get("jobs")
    require(isinstance(rows, list) and type(jobs.get("total_count")) is int
            and len(rows) == jobs["total_count"] <= 100, "incomplete producer jobs")
    selected = [j for j in rows if j.get("name") == "corpus-parity"]
    require(len(selected) == 1 and selected[0].get("status") == "completed"
            and selected[0].get("conclusion") in ("success", "failure"), "native producer did not run")
    steps = selected[0].get("steps", [])
    for name, verdicts in (
        ("Check TradingView parity against the committed oracle", ("success", "failure")),
        ("Retain recorded headline evidence", ("success",)),
    ):
        found = [s for s in steps if s.get("name") == name]
        require(len(found) == 1 and found[0].get("status") == "completed"
                and found[0].get("conclusion") in verdicts, "missing/failed producer step: " + name)
    record, raw, summary = unpack(service.download(attached["artifact_id"]), attached["artifact_sha256"])
    inputs = scientific(repo, head)
    corpus_tree = service.read(f"/repos/{CORPUS_REPOSITORY}/git/trees/{inputs['corpus_commit']}?recursive=1")
    require(corpus_tree.get("sha") == inputs["corpus_commit"]
            and corpus_tree.get("truncated") is False
            and isinstance(corpus_tree.get("tree"), list), "incomplete corpus input tree")
    probes = population({entry["path"]: entry for entry in corpus_tree["tree"]
                         if entry.get("type") == "blob"})
    total = validate_record(record, raw, summary, expected, inputs, probes, source, attached["output_sha256"])
    latest = service.read(prefix + f"/actions/runs/{attached['run_id']}")
    latest_artifact = service.read(prefix + f"/actions/artifacts/{attached['artifact_id']}")
    require(latest.get("run_attempt") == run["run_attempt"]
            and service_identity(latest, latest_artifact, attached, workflow["id"]) == source,
            "recording changed while evidence was read")
    return total


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--pr", type=int)
    source.add_argument("--attachment", type=Path)
    args = parser.parse_args(argv)
    try:
        ancestor, old, new = compared(args.repo, args.base, args.head)
        if old == new:
            print(f"corpus headline unchanged: merge-base {ancestor}; no new evidence required")
            return 0
        require(args.pr is not None or args.attachment is not None,
                "changed headline requires a recorded run/artifact attachment")
        service = Service()
        if args.pr is not None:
            require(args.pr > 0, "invalid PR number")
            pr = service.read(f"/repos/{REPOSITORY}/pulls/{args.pr}")
            require(pr.get("head", {}).get("sha") == args.head
                    and pr.get("base", {}).get("sha") == args.base
                    and pr.get("head", {}).get("repo", {}).get("full_name") == REPOSITORY
                    and pr.get("head", {}).get("repo", {}).get("fork") is False,
                    "live PR no longer matches admitted base/head")
            body = pr.get("body") or ""
            require(body.count("```corpus-headline-evidence") == 1, "ambiguous PR attachment")
            blocks = re.findall(r"(?m)^```corpus-headline-evidence\n([^\n]+)\n```[ \t]*$", body)
            require(len(blocks) == 1, "expected one corpus-headline-evidence PR attachment")
            attached = json_object(blocks[0])
        else:
            attached = json_object(args.attachment.read_bytes())
        total = check_service(service, attached, args.repo, args.base, args.head, new)
        print(f"corpus headline evidence: {total}/{total} native probes; exact measured bytes and scientific inputs")
        print("Coverage: this recorded full native verifier run; no baseline/parity or review approval implied")
        return 0
    except Exception as error:
        print("corpus headline evidence refused: " + " ".join(str(error).splitlines()))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
