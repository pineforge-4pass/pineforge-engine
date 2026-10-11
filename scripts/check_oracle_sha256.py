#!/usr/bin/env python3
"""Check the recorded SHA-256 tree pin for the frozen L0 oracle sources.

The private-fixture PR removes three oracle sources (their rows are pinned in
tests/private_fixture_rows.txt): test_oracle_day_key.cpp,
test_oracle_reversal_same_bar_tx.cpp and test_oracle_stop_snapshot.cpp. The
pin, tests/oracle.sha256, names them under ``privateFixtureAbsent`` and pins a
second aggregate hash, ``treeSha256WithoutPrivate``, computed over exactly the
files that remain when they are absent. Either shape is enforced exactly:

  * every present file must be pinned and match its hash (a mutated oracle
    fails), an unpinned file present fails, and a removed pinned file that is
    not one of the three named private ones fails;
  * the aggregate tree hash must match the full ``treeSha256`` when every
    pinned file is present (an overlay carrying the private tree is verified
    too), or ``treeSha256WithoutPrivate`` when exactly the named private files
    are absent. Any other subset is drift.

The three private files are still verified hash-for-hash whenever they are
present, so the fixture-less and overlay shapes are both pinned, never
weakened to an always-pass.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "oracle"
PIN = ROOT / "tests" / "oracle.sha256"
SCHEMAS = ("pineforge-r4-d-oracle-tree/v1", "pineforge-r4-d-oracle-tree/v2")


def digest_files(files: dict[str, str]) -> str:
    material = "".join(name + "\0" + value + "\n" for name, value in files.items()).encode()
    return hashlib.sha256(material).hexdigest()


def digest_tree() -> dict[str, str]:
    return {
        str(path.relative_to(ORACLE)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(ORACLE.rglob("*")) if path.is_file()
    }


def main() -> int:
    try:
        pin = json.loads(PIN.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit("oracle SHA-256 pin: cannot read pin: " + str(error))
    if pin.get("schema") not in SCHEMAS:
        raise SystemExit("oracle SHA-256 pin: unexpected schema")
    pinned = pin.get("files")
    if not isinstance(pinned, dict) or not pinned:
        raise SystemExit("oracle SHA-256 pin: pin has no files")
    private_absent = pin.get("privateFixtureAbsent", [])
    if not isinstance(private_absent, list) or not all(isinstance(name, str) for name in private_absent):
        raise SystemExit("oracle SHA-256 pin: privateFixtureAbsent must be a list of names")
    private_absent = sorted(private_absent)
    files = digest_tree()
    unexpected = sorted(set(files) - set(pinned))
    if unexpected:
        raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift (unpinned file: "
                         + unexpected[0] + ")")
    missing = sorted(set(pinned) - set(files))
    undeclared = [name for name in missing if name not in private_absent]
    if undeclared:
        raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift (missing pinned file: "
                         + undeclared[0] + ")")
    for name, digest in files.items():
        if digest != pinned[name]:
            raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift (content changed: "
                             + name + ")")
    if not missing:
        tree = digest_files(files)
        if pin.get("treeSha256") != tree:
            raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift")
        detail = f"{len(files)} files, {tree}"
    elif missing == private_absent:
        tree = digest_files(files)
        if pin.get("treeSha256WithoutPrivate") != tree:
            raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift "
                             "(private-fixture-absent tree hash changed)")
        detail = (f"private fixtures not present: "
                  + ", ".join("tests/oracle/" + name for name in missing)
                  + f"; {len(files)} files, {tree}")
    else:
        raise SystemExit("oracle SHA-256 pin: tests/oracle tree drift (partial private "
                         "fixture removal: missing " + ", ".join(missing) + ")")
    print(f"oracle SHA-256 pin: {detail}, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
