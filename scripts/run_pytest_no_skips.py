#!/usr/bin/env python3
"""Run a required pytest row; a JUnit skip makes the row fail (H2-6)."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def checked_path(value):
    path = Path(os.path.abspath(Path(value).expanduser()))
    protected = tuple(Path.home() / name for name in (
        ".config/zsh/keys", ".ssh", ".aws", ".codex", ".config/pineforge"))

    def refuse(candidate):
        if candidate.name.endswith(".env") or any(
                candidate == prefix or prefix in candidate.parents for prefix in protected):
            raise ValueError(f"refusing protected path: {candidate}")

    refuse(path)
    refuse(path.resolve())
    return path


def require_no_skips(root):
    suites = [node for node in root.iter() if node.tag in ("testsuite", "testsuites")]
    if not suites:
        raise ValueError("JUnit report has no test suite")
    for suite in suites:
        for key in ("skipped", "failures", "errors"):
            count = int(suite.get(key, "0"))
            if count < 0:
                raise ValueError(f"JUnit report has negative {key}")
            if count:
                raise ValueError(f"required pytest row reports {key}={count}")
    if any(node.tag == "skipped" for node in root.iter()):
        raise ValueError("required pytest row contains a skipped testcase")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("pytest_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    pytest_args = args.pytest_args
    if pytest_args[:1] == ["--"]:
        pytest_args = pytest_args[1:]
    try:
        artifacts = checked_path(args.artifacts)
        inputs = []
        for argument in pytest_args:
            if argument.startswith(("--junitxml", "--junit-xml")):
                raise ValueError("the wrapper owns the JUnit output path")
            value = argument.split("=", 1)[-1] if argument.startswith("--") else argument
            if value.startswith("-"):
                continue
            path = checked_path(value.split("::", 1)[0])
            if path.suffix == ".py" or path.is_file() or path.is_dir():
                inputs.append(path)
        print("pytest no-skip inputs:\n" + "\n".join(map(str, inputs)), flush=True)
        artifacts.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts))
        report = work / "junit.xml"
        print(f"pytest no-skip JUnit result: {report}", flush=True)
        result = subprocess.run(
            [sys.executable, "-B", "-m", "pytest", *pytest_args, "--junitxml", str(report)])
        if result.returncode:
            return result.returncode
        root = ET.parse(report).getroot()
        require_no_skips(root)
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"run_pytest_no_skips: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
