#!/usr/bin/env python3
"""Maintainer tool: re-record named tests in the twin base manifest from the working tree.

The manifest pins the CHECK/REQUIRE/EXPECT literals of the ab9714be tests that the switched-route twins must match. When a
base test is rewritten on purpose (for example its recorded market data replaced by synthetic data), the twins are
rewritten with it and this tool re-records that test's assertions, line numbers and source digest from the file now in
``tests/``. Every other row of the manifest stays as it was, and ``BASE_MANIFEST_SHA256`` in check_twin_parity.py is
re-pinned to the new bytes.

A name is refused unless it is a row of the manifest AND its tests/<name>.cpp differs from the same file at the --since
commit, so a test that was not rewritten can never have its recorded literals replaced by this tool.

usage: python3 scripts/refresh_twin_parity_base.py --since <commit> test_name [test_name ...]
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import check_twin_parity as parity


ROOT = Path(__file__).resolve().parents[1]
PIN = re.compile(r'^(BASE_MANIFEST_SHA256 = ")[0-9a-f]{64}(")$', re.M)


def main(args: list[str]) -> int:
    if len(args) < 3 or args[0] != "--since":
        raise SystemExit(__doc__)
    since, names = args[1], args[2:]
    if len(set(names)) != len(names):
        raise SystemExit("duplicate test name")
    path = ROOT / "tests/twin_parity_base.json"
    manifest = json.loads(path.read_text())
    for name in names:
        if name not in manifest["tests"]:
            raise SystemExit("not in the twin base manifest: " + name)
        if not (ROOT / "tests" / (name + ".cpp")).is_file():
            raise SystemExit("tests/" + name + ".cpp does not exist: its row is not re-recorded")
        shown = subprocess.run(["git", "-C", str(ROOT), "show", f"{since}:tests/{name}.cpp"],
                               capture_output=True, text=True)
        if shown.returncode == 0 and shown.stdout == (ROOT / "tests" / (name + ".cpp")).read_text():
            raise SystemExit(name + " is unchanged since " + since + ": its row is not re-recorded")
        source = (ROOT / "tests" / (name + ".cpp")).read_text()
        rows = parity.extract_assertions(source, "tests/" + name + ".cpp")
        manifest["tests"][name] = {
            "sourceSha256": hashlib.sha256(source.encode()).hexdigest(),
            "assertions": [{"line": row.line, "text": row.text} for row in rows],
        }
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    checker = ROOT / "scripts/check_twin_parity.py"
    text = checker.read_text()
    if len(PIN.findall(text)) != 1:
        raise SystemExit("check_twin_parity.py has no single BASE_MANIFEST_SHA256 pin")
    checker.write_text(PIN.sub(lambda m: m[1] + digest + m[2], text))
    print("re-recorded", len(names), "tests; BASE_MANIFEST_SHA256 =", digest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
