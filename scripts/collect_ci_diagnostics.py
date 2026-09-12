#!/usr/bin/env python3
"""Retain compact CI diagnostics without copying binaries or dependency caches."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ARTIFACTS = {
    "ci-summary.json": "ci-summary.json",
    "ci-logs": "ci-logs",
    "ctest-junit.xml": "ctest-junit.xml",
    "Testing/Temporary/LastTest.log": "LastTest.log",
    "settlement-abi-base/receipt.json": "settlement-abi-base-receipt.json",
    "settlement-abi-base/configure.log": "settlement-abi-base-configure.log",
    "settlement-abi-base/build.log": "settlement-abi-base-build.log",
    "settlement-abi-receipt.json": "settlement-abi-receipt.json",
}
ENV_KEYS = ("GITHUB_EVENT_NAME", "GITHUB_REF", "GITHUB_SHA", "GITHUB_RUN_ID",
            "GITHUB_RUN_ATTEMPT", "GITHUB_JOB", "RUNNER_OS", "RUNNER_ARCH", "CURL_CACHE_HIT")


def copy_regular(source: Path, target: Path) -> None:
    if source.is_symlink():
        return
    if source.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    elif source.is_dir():
        for item in source.iterdir():
            copy_regular(item, target / item.name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--profile", choices=("release", "debug", "sanitizers", "native"), required=True)
    parser.add_argument("--output", type=Path, default=Path("ci-diagnostics"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    if output == build or build in output.parents:
        parser.error("--output must be outside --build-dir to avoid copying diagnostics into themselves")
    args.output.mkdir(parents=True, exist_ok=True)
    info = {key: os.environ.get(key, "") for key in ENV_KEYS}
    info.update(buildDir=str(args.build_dir), profile=args.profile)
    (args.output / "job-info.json").write_text(json.dumps(info, indent=2) + "\n")
    missing = []
    for source, target in ARTIFACTS.items():
        path = args.build_dir / source
        if path.exists() and not path.is_symlink():
            copy_regular(path, args.output / target)
        else:
            missing.append(source)
    for directory in args.build_dir.glob("settlement-abi-receipt.artifacts-*"):
        if directory.is_symlink() or not directory.is_dir():
            continue
        for item in directory.rglob("*"):
            if item.suffix in (".log", ".json", ".cpp", ".hpp"):
                copy_regular(item, args.output / directory.name / item.relative_to(directory))
    (args.output / "missing.json").write_text(json.dumps(missing, indent=2) + "\n")
    if shutil.which("ccache"):
        result = subprocess.run(["ccache", "--show-stats"], capture_output=True, text=True)
        (args.output / "ccache.log").write_text(result.stdout + result.stderr)
    summary = {}
    try:
        summary = json.loads((args.build_dir / "ci-summary.json").read_text())
    except (OSError, ValueError):
        pass
    print(json.dumps({"profile": args.profile, "status": summary.get("status", "unavailable"),
                      "failures": summary.get("failures", []), "diagnostics": str(args.output)}, indent=2))
    destination = os.environ.get("GITHUB_STEP_SUMMARY")
    if destination:
        status = summary.get("status", "unavailable")
        stages = summary.get("stages", [])
        lines = [f"### Verification: {args.profile}", "", f"Status: **{status}**", "",
                 "| Stage | Result |", "| --- | --- |"]
        lines += [f"| {stage.get('name', '?')} | {stage.get('status', '?')} |" for stage in stages]
        lines += ["", "Full logs and ABI/CTest receipts are attached as CI diagnostics.", ""]
        with Path(destination).open("a") as output:
            output.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
