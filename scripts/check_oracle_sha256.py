#!/usr/bin/env python3
"""Check the recorded SHA-256 tree pin for the frozen L0 oracle sources."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "oracle"
PIN = ROOT / "tests" / "oracle.sha256"


def digest_tree() -> tuple[dict[str, str], str]:
    files = {
        str(path.relative_to(ORACLE)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(ORACLE.rglob("*")) if path.is_file()
    }
    material = "".join(name + "\0" + value + "\n" for name, value in files.items()).encode()
    return files, hashlib.sha256(material).hexdigest()


def main() -> int:
    try:
        pin = json.loads(PIN.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit("oracle SHA-256 pin: cannot read pin: " + str(error))
    files, tree = digest_tree()
    if pin.get("schema") != "pineforge-r4-d-oracle-tree/v1":
        raise SystemExit("oracle SHA-256 pin: unexpected schema")
    if pin.get("files") != files or pin.get("treeSha256") != tree:
        raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift")
    print(f"oracle SHA-256 pin: {len(files)} files, {tree}, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
