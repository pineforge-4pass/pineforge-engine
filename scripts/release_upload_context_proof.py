#!/usr/bin/env python3
"""Exercise extracted release-upload shells with installed gh and loopback HTTPS.

Adapted from the admitted driver with SHA-256
92dc1e18467b5a1f562f4658580cd445a5c067313a08e9ffad6438c15716a6a1.
The negative input is the unmodified release.yml from revision 2167, Git blob
ae19f9e7b4573048b3b0b40bd2e567f6be4e5e98. Only that historical input is pinned.
UNIT_DUMMY_NOT_A_CREDENTIAL is synthetic loopback fixture data, as in that
driver; no existing credentials are read, copied or restored.
Runtime files and complete raw observations remain in the supplied output dir.
An execution here does not establish hosted acceptance: independently verify
the actual Actions job, final checkout head and logs.
"""
from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import ssl
import subprocess
import threading
from urllib.parse import parse_qs, urlsplit


BASELINE_SHA256 = "3cae61d4909db55623e69a349775c6703a9a500141ba72fc345379b01c614abd"
BASELINE_GIT_BLOB = "ae19f9e7b4573048b3b0b40bd2e567f6be4e5e98"
SYNTHETIC_TOKEN = "UNIT_DUMMY_NOT_A_CREDENTIAL"
REPOSITORIES = ("pineforge-4pass/pineforge-engine", "fixture-owner/other-repository")
TAG = "v0.0.0-context-fixture"
EXPRESSIONS = {
    "GH_TOKEN": "${{ secrets.GITHUB_TOKEN }}",
    "GH_REPO": "${{ github.repository }}",
    "TAG": "${{ needs.release.outputs.tag }}",
    "X86_TARBALL": "${{ needs.prebuilt-x86.outputs.tarball }}",
    "ARM_TARBALL": "${{ needs.prebuilt-arm.outputs.tarball }}",
}


class ProofError(RuntimeError):
    """A missing dependency or an unproved upload contract is a failure."""


def _require(condition, message):
    if not condition:
        raise ProofError(message)


def _save_json(path, value):
    text = json.dumps(value, sort_keys=True) + "\n"
    path.write_text(text, encoding="utf-8")
    # The existing preflight stage retains stdout; RUNNER_TEMP is outside it.
    print(path.name, text, end="", flush=True)


def _capture(argv, *, cwd, env, stem, timeout=30):
    timed_out = False
    try:
        result = subprocess.run(argv, cwd=cwd, env=env, capture_output=True,
                                check=False, timeout=timeout)
        code, stdout, stderr = result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as error:
        timed_out = True
        code, stdout, stderr = None, error.stdout or b"", error.stderr or b""
    stem.with_suffix(".stdout").write_bytes(stdout)
    stem.with_suffix(".stderr").write_bytes(stderr)
    record = {"argv": argv, "cwd": str(cwd), "returncode": code,
              "timed_out": timed_out, "stdout_hex": stdout.hex(),
              "stderr_hex": stderr.hex()}
    _save_json(stem.with_suffix(".json"), record)
    return record


def _source(path):
    data = path.read_bytes()
    return data, {
        "path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
        "git_blob": hashlib.sha1(
            b"blob " + str(len(data)).encode() + b"\0" + data,
            usedforsecurity=False).hexdigest(),
    }


def _upload_step(data):
    """Extract, without rewriting, the one reviewed literal upload step."""
    text = data.decode("utf-8")
    marker = "      - name: Upload to release\n"
    _require(text.count(marker) == 1, "expected exactly one Upload to release step")
    rows = []
    for row in text.split(marker, 1)[1].splitlines(True):
        if row.strip() and len(row) - len(row.lstrip()) < 8:
            break
        rows.append(row)
    block = "".join(rows)
    _require(block.count("        run: |\n") == 1, "upload needs one literal run block")
    header, run = block.split("        run: |\n", 1)
    bindings = {}
    in_env = False
    for row in header.splitlines():
        if not row.strip():
            continue
        if row == "        if: ${{ !inputs.dry_run }}":
            continue
        if row == "        env:":
            _require(not in_env, "duplicate upload environment")
            in_env = True
            continue
        match = re.fullmatch(r"          ([A-Z0-9_]+): (.+)", row)
        _require(in_env and match is not None, "unhandled upload step field: " + row)
        name, expression = match.groups()
        _require(name not in bindings and name in EXPRESSIONS,
                 "unhandled or duplicate upload binding: " + name)
        _require(expression == EXPRESSIONS[name], "changed upload binding: " + name)
        bindings[name] = expression
    _require(set(EXPRESSIONS) - {"GH_REPO"} <= bindings.keys(),
             "missing upload environment binding")
    _require(all(not row.strip() or row.startswith("          ")
                 for row in run.splitlines()), "unhandled run indentation")
    shell = "\n".join(row[10:] if row.startswith("          ") else ""
                      for row in run.splitlines()) + "\n"
    # Bound execution to release upload on GH_HOST. A future command/host change
    # must fail closed, rather than allowing an arbitrary shell to use a network.
    words = shlex.split(shell.replace("\\\n", ""))
    _require(words[:6] == ["set", "-euo", "pipefail", "gh", "release", "upload"],
             "upload shell is outside the loopback execution contract")
    for word in words[6:]:
        _require(word in ("${TAG}", "--clobber") or re.fullmatch(
            r"artifacts/(x86|arm)/\$\{(X86|ARM)_TARBALL\}(\.sha256)?", word),
            "unhandled upload argument: " + word)
    return marker + block, shell, bindings


def _tools(output, env):
    tools = {}
    for name, flag in (("gh", "--version"), ("openssl", "version"),
                       ("python3", "--version"), ("git", "--version"),
                       ("bash", "--version")):
        found = shutil.which(name, path=env["PATH"])
        _require(found is not None, "required installed tool is missing: " + name)
        binary = Path(found).resolve(strict=True)
        if name == "gh":
            data = binary.read_bytes()
            _require(data[:4] in (b"\x7fELF", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                                 b"\xca\xfe\xba\xbe") or data[:2] == b"MZ",
                     "gh must be the installed native CLI, not a script substitute")
            tools["gh_sha256"] = hashlib.sha256(data).hexdigest()
        record = _capture([str(binary), flag], cwd=output, env=env,
                          stem=output / ("tool-" + name))
        _require(record["returncode"] == 0 and record["stdout_hex"],
                 "required installed tool is broken: " + name)
        if name == "gh":
            _require(bytes.fromhex(record["stdout_hex"]).startswith(b"gh version "),
                     "installed gh did not identify itself")
        tools[name] = str(binary)
    _save_json(output / "tools.json", tools)
    return tools


def run_proof(*, root, baseline, candidate, output):
    """Run raw historical RED and current-source GREEN for two repo inputs."""
    root = Path(root).resolve(strict=True)
    baseline = (root / baseline).resolve(strict=True)
    candidate = (root / candidate).resolve(strict=True)
    output = (root / output).resolve()
    _require(output != root and root not in output.parents,
             "proof output must be outside the checkout")
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / "synthetic-loopback"
    fixture.mkdir()  # Refuse to overwrite any previous execution.
    home, config = fixture / "home", fixture / "tool-config"
    home.mkdir()
    config.mkdir()
    search_path = os.environ.get("PATH", os.defpath)
    _require(all(Path(part).is_absolute() for part in search_path.split(os.pathsep)),
             "PATH must contain only absolute tool directories")
    # A fresh whitelist isolates gh from user auth/config and drops inherited
    # Git redirects, proxies and shell startup hooks.
    env = {
        "PATH": search_path, "LC_ALL": "C.UTF-8", "HOME": str(home),
        "XDG_CONFIG_HOME": str(config), "GH_CONFIG_DIR": str(config),
        "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
        "GH_PROMPT_DISABLED": "1", "GH_NO_UPDATE_NOTIFIER": "1",
        "GH_NO_EXTENSION_UPDATE_NOTIFIER": "1", "NO_PROXY": "127.0.0.1",
    }
    old_data, old_input = _source(baseline)
    new_data, new_input = _source(candidate)
    old_input["source_revision"] = "2167"
    inputs = {"baseline": old_input, "candidate": new_input,
              "support": _source(Path(__file__).resolve())[1],
              "venue_metadata_only": {key: os.environ.get(key) for key in (
                  "GITHUB_ACTIONS", "RUNNER_ENVIRONMENT", "GITHUB_REPOSITORY",
                  "GITHUB_SHA", "GITHUB_JOB", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT")},
              "hosted_acceptance": "UNPROVEN_HOSTED: verify actual job/head/log independently"}
    (output / "baseline.source.yml").write_bytes(old_data)
    (output / "candidate.source.yml").write_bytes(new_data)
    _save_json(output / "inputs.json", inputs)
    _require(old_input["sha256"] == BASELINE_SHA256
             and old_input["git_blob"] == BASELINE_GIT_BLOB,
             "historical input is not the unmodified revision 2167 workflow")
    extracted = {}
    for label, data in (("baseline", old_data), ("candidate", new_data)):
        block, shell, bindings = _upload_step(data)
        (output / (label + ".step.yml")).write_text(block, encoding="utf-8")
        (output / (label + ".sh")).write_text(shell, encoding="utf-8")
        extracted[label] = (shell, bindings)
        inputs[label]["shell_sha256"] = hashlib.sha256(shell.encode()).hexdigest()
    _require("GH_REPO" not in extracted["baseline"][1],
             "historical negative must have no explicit repository")
    tools = _tools(output, env)
    for name, argument in (("head", "HEAD"), ("tree", "HEAD^{tree}")):
        record = _capture([tools["git"], "-C", str(root), "rev-parse", "--verify", argument],
                          cwd=output, env=env, stem=output / ("checkout-" + name))
        value = bytes.fromhex(record["stdout_hex"]).decode().strip()
        _require(record["returncode"] == 0 and re.fullmatch(r"[0-9a-f]{40}", value),
                 "cannot identify the actual checkout " + name)
        inputs["checkout_" + name] = value
    _save_json(output / "inputs.json", inputs)
    cert, key = fixture / "cert.pem", fixture / "fixture-key.pem"
    made_cert = _capture(
        [tools["openssl"], "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
         "-days", "1", "-keyout", str(key), "-out", str(cert)],
        cwd=fixture, env=env, stem=output / "certificate", timeout=20)
    _require(made_cert["returncode"] == 0, "openssl could not create loopback TLS fixture")
    requests = []
    lock = threading.Lock()
    active_repo = ""
    authority = ""

    class API(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def __getattr__(self, name):
            # Catalogue and refuse even unexpected HTTP verbs.
            if name.startswith("do_"):
                return self.dispatch_request
            raise AttributeError(name)

        def dispatch_request(self):
            parsed = urlsplit(self.path)
            payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            release_path = "/api/v3/repos/" + active_repo + "/releases/tags/" + TAG
            asset_path = "/api/uploads/repos/" + active_repo + "/releases/41/assets"
            route, code = "unknown-route", 404
            value = {"message": "unsupported fixture route"}
            if self.command == "DELETE":
                route, code = "forbidden-delete", 409
                value = {"message": "fixture forbids deletion"}
            elif self.command == "GET" and parsed.path == release_path and not parsed.query:
                route, code = "release-discovery", 200
                value = {"id": 41, "tag_name": TAG, "assets": [],
                         "upload_url": "https://" + authority + asset_path + "{?name,label}",
                         "url": "https://" + authority + "/api/v3/repos/"
                                + active_repo + "/releases/41"}
            elif self.command == "POST" and parsed.path == asset_path:
                route, code = "asset-upload", 201
                value = {"id": 42, "name": parse_qs(parsed.query).get("name", [""])[0],
                         "size": len(payload), "state": "uploaded",
                         "url": "https://" + authority + parsed.path}
            elif (self.command == "POST" and parsed.path == "/api/graphql"
                  and not parsed.query):
                try:
                    discovery = json.loads(payload)
                    query = discovery.get("query") if isinstance(discovery, dict) else None
                    if (isinstance(query, str) and re.match(r"^\s*query\b", query)
                            and not re.search(r"\b(mutation|subscription)\b", query)):
                        route = "read-only-graphql-discovery"
                except (ValueError, UnicodeError):
                    pass
                # Deliberate 404, like the admitted fixture: gh must fall back
                # to the REST release lookup. A mutation is never classified as read-only.
            record = {"method": self.command, "target": self.path,
                      "path": parsed.path, "query": parsed.query,
                      "payload_hex": payload.hex(), "route": route, "status": code}
            with lock:
                requests.append(record)
            data = json.dumps(value).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), API)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(cert), str(key))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    authority = "127.0.0.1:" + str(server.server_port)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    cases = []
    try:
        for index, repo in enumerate(REPOSITORIES):
            active_repo = repo
            for label in ("baseline", "candidate"):
                cwd = fixture / (str(index) + "-" + label)
                cwd.mkdir()
                cfg = cwd / "gh-config"
                cfg.mkdir()
                child_env = {**env, "HOME": str(cwd), "GH_CONFIG_DIR": str(cfg),
                             "XDG_CONFIG_HOME": str(cfg), "GH_HOST": authority,
                             "GH_ENTERPRISE_TOKEN": SYNTHETIC_TOKEN,
                             "SSL_CERT_FILE": str(cert), "GITHUB_ACTIONS": "true",
                             "GITHUB_REPOSITORY": repo}
                nongit = _capture([tools["git"], "rev-parse", "--is-inside-work-tree"],
                                  cwd=cwd, env=child_env, stem=cwd / "non-git")
                _require(nongit["returncode"] == 128 and b"not a git repository" in
                         bytes.fromhex(nongit["stderr_hex"]).lower(),
                         "working directory must genuinely be outside every Git repository")
                expected = {}
                for arch in ("x86", "arm"):
                    target = cwd / "artifacts" / arch
                    target.mkdir(parents=True)
                    payload = ("SYNTHETIC PAYLOAD ONLY " + repo + arch).encode()
                    expected[arch + ".tar.gz"] = payload
                    expected[arch + ".tar.gz.sha256"] = (
                        hashlib.sha256(payload).hexdigest() + "  " + arch + ".tar.gz\n").encode()
                    for name in (arch + ".tar.gz", arch + ".tar.gz.sha256"):
                        (target / name).write_bytes(expected[name])
                values = {"GH_TOKEN": SYNTHETIC_TOKEN, "GH_REPO": repo, "TAG": TAG,
                          "X86_TARBALL": "x86.tar.gz", "ARM_TARBALL": "arm.tar.gz"}
                shell, bindings = extracted[label]
                # Populate only fields actually present in this source step.
                child_env.update({name: values[name] for name in bindings})
                with lock:
                    start = len(requests)
                ran = _capture([tools["bash"], "-c", shell], cwd=cwd, env=child_env,
                               stem=cwd / "actual-gh")
                with lock:
                    seen = list(requests[start:])
                _save_json(cwd / "requests.json", seen)
                _save_json(output / "request-catalog.json", requests)
                raw = {"source": label, "repository": repo, "returncode": ran["returncode"],
                       "source_sha256": inputs[label]["sha256"],
                       "shell_sha256": inputs[label]["shell_sha256"],
                       "requests": seen, "expected_payloads_hex":
                           {name: data.hex() for name, data in expected.items()}}
                _save_json(cwd / "raw-outcome.json", raw)
                payload_matches = []
                if label == "baseline":
                    _require(ran["returncode"] not in (0, None) and not seen,
                             "raw historical shell must fail before making any API request")
                    _require(re.search(
                        r"not a git repository|failed to run git|could not determine.*repository",
                        bytes.fromhex(ran["stderr_hex"]).decode(errors="replace"), re.I),
                        "historical failure must identify missing repository context")
                    outcome, joins = "RED", 0
                else:
                    _require(ran["returncode"] == 0, "current-source upload shell failed")
                    _require(all(row["route"] in ("release-discovery", "asset-upload",
                                                 "read-only-graphql-discovery") for row in seen),
                             "unexpected API request or DELETE in complete catalogue")
                    _require(any(row["route"] == "release-discovery" for row in seen),
                             "gh did not discover the release for the supplied repository")
                    uploads = [row for row in seen if row["route"] == "asset-upload"]
                    actual = []
                    for row in uploads:
                        names = parse_qs(row["query"]).get("name", [])
                        _require(len(names) == 1, "upload needs exactly one asset filename")
                        actual.append((names[0], bytes.fromhex(row["payload_hex"])))
                    _require(len(uploads) == 4 and sorted(actual) == sorted(expected.items()),
                             "four exact repository/filename/payload joins did not match")
                    payload_matches = [
                        {"repository": repo, "filename": name,
                         "expected_payload_hex": expected[name].hex(),
                         "actual_payload_hex": payload.hex(),
                         "sha256": hashlib.sha256(payload).hexdigest()}
                        for name, payload in sorted(actual)]
                    outcome, joins = "GREEN", len(uploads)
                case = {**raw, "outcome": outcome, "payload_joins": joins,
                        "payload_matches": payload_matches}
                cases.append(case)
                _save_json(cwd / "checked-outcome.json", case)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        _save_json(output / "request-catalog.json", requests)
    result = {"inputs": inputs, "cases": cases,
              "scope": "synthetic loopback API only; no real GitHub publication",
              "hosted_acceptance": inputs["hosted_acceptance"]}
    _save_json(output / "observations.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "baseline", "candidate", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    run_proof(root=args.root, baseline=args.baseline, candidate=args.candidate,
              output=args.output)


if __name__ == "__main__":
    main()
