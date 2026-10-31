"""CLOUD-only capture of main's OFF bytes; never used by the acceptance row.

Invoke this helper from the lane checkout, pointing --main-root at a separate,
unmodified checkout of the pinned engine/corpus revisions and --requests at
the supervisor's fixed requests.json. --output is a new artifact directory.
The produced manifest is an artifact for the supervisor to inspect; this
helper never updates the checked-in empty-digest manifest.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

from host import ROOT, clean_environment
from test_off_identity import CLOCK_RUNNER, CLOCKS, MANIFEST


ENGINE = "fc0a2df5deca07f8a7e59995a17e71203fb19f34"
CORPUS = "8425ca820c4b7272db24d659a64d88e8e05b6407"


def git(root, *arguments):
    return subprocess.run(
        ["git", "-C", str(root), *arguments], check=True, capture_output=True, text=True).stdout.strip()


def write_new(path, data):
    with path.open("xb") as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--main-root", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.main_root.resolve()
    if root == ROOT.resolve():
        raise ValueError("main capture requires a separate unmodified checkout")
    if git(root, "rev-parse", "HEAD") != ENGINE:
        raise ValueError("main capture engine revision is not pinned")
    if git(root / "corpus", "rev-parse", "HEAD") != CORPUS:
        raise ValueError("main capture corpus revision is not pinned")
    for checkout in (root, root / "corpus"):
        git(checkout, "diff", "--exit-code", "HEAD", "--")
        if git(checkout, "status", "--porcelain", "--untracked-files=normal"):
            raise ValueError("main capture requires an unmodified source checkout")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    subset = (root / manifest["scenario_list"]).read_bytes()
    ids = [line.partition("#")[0].strip() for line in subset.decode().splitlines()]
    ids = [name for name in ids if name]
    requests_raw = args.requests.read_bytes()
    requests = json.loads(requests_raw)
    if (requests.get("engine_revision") != "fc0a2df5d"
            or [case["id"] for case in requests["cases"]] != ids
            or len(set(ids)) != len(ids)):
        raise ValueError("requests must name the exact pinned corpus subset, in order")
    rows = []
    for case in requests["cases"]:
        if (not case["argv"] or set(case["fixed_clock"]) != CLOCKS
                or not all(type(value) in (int, float) and math.isfinite(value)
                           for value in case["fixed_clock"].values())):
            raise ValueError("each main request needs fixed finite clocks and exact arguments")
        if any(token.partition("=")[0] in (
                "--report-channel", "--terminal-status-fd", "--report-max-bytes",
                "--diagnostic-max-bytes", "--channel-wall-ms", "--channel-stall-ms")
               for token in case["argv"]):
            raise ValueError("main capture must be OFF")
        if any(key.startswith(("PINEFORGE_REPORT_", "PINEFORGE_TERMINAL_",
                               "PINEFORGE_CHANNEL_", "PINEFORGE_DIAGNOSTIC_"))
               for key in case["environment"]):
            raise ValueError("main capture must not supply channel environment controls")
        environment = clean_environment()
        environment.update(case["environment"])
        result = subprocess.run(
            [sys.executable, "-B", "-c", CLOCK_RUNNER, str(root), json.dumps(case)],
            cwd=root, env=environment, capture_output=True, check=False, timeout=600)
        document = json.loads(result.stdout)
        if not isinstance(document, dict):
            raise ValueError("main request did not produce a report/failure object")
        rows.append({
            "id": case["id"], "report_base64": base64.b64encode(result.stdout).decode("ascii"),
            "report_sha256": hashlib.sha256(result.stdout).hexdigest(),
            "failure_code": document.get("code"), "exit_status": result.returncode,
        })
    reports_raw = json.dumps({"cases": rows}, separators=(",", ":")).encode() + b"\n"
    manifest.update(
        scenario_list_sha256=hashlib.sha256(subset).hexdigest(),
        requests_sha256=hashlib.sha256(requests_raw).hexdigest(),
        reports_sha256=hashlib.sha256(reports_raw).hexdigest())
    args.output.mkdir(parents=True, exist_ok=False)
    write_new(args.output / "requests.json", requests_raw)
    write_new(args.output / "reports.json", reports_raw)
    write_new(args.output / "off_identity_golden.json",
              json.dumps(manifest, indent=2).encode() + b"\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
