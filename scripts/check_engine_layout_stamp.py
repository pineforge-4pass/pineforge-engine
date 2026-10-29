#!/usr/bin/env python3
"""Bind measured layout tables to an explicit stamp and a reviewed digest.

The lock lives beside the tables in engine_layout_fingerprint.hpp. Full Git
history supplies its previous measured version, so refreshing a digest cannot
silently reuse an issued stamp. Unmeasured historical tables were never valid
library builds and do not establish an issued version.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
FINGERPRINT = "src/engine_layout_fingerprint.hpp"
PROBE = "src/engine_layout_probe.hpp"
PUBLIC = "include/pineforge/pineforge.h"
TABLES = ("PINEFORGE_MEASURED_ENGINE_LAYOUT", "PINEFORGE_MEASURED_ENGINE_VIRTUALS")


class InvalidStamp(ValueError):
    pass


def literal(text: str, name: str) -> int:
    matches = re.findall(rf"\b{re.escape(name)}\s*=\s*(\d+)u?\s*;", text)
    if len(matches) != 1:
        raise InvalidStamp(f"expected one integer literal for {name}")
    return int(matches[0])


def macro_body(text: str, name: str, *, optional: bool = False) -> str:
    matches = list(re.finditer(rf"^#define {name}\(V\)[^\n]*", text, re.MULTILINE))
    if not matches and optional:
        return ""
    if len(matches) != 1:
        raise InvalidStamp(f"expected one table macro {name}")
    start = matches[0].end()
    body = []
    line = matches[0].group()
    while line.rstrip().endswith("\\"):
        end = text.find("\n", start + 1)
        if end < 0:
            end = len(text)
        line = text[start + 1:end]
        body.append(line.removesuffix("\\").strip())
        start = end
    return "\n".join(body)


@dataclass(frozen=True)
class Table:
    version: int
    rows: tuple[tuple[str, str, str, str], ...]

    @property
    def measured(self) -> bool:
        return bool(self.rows) and all("unmeasured" not in row[2:] for row in self.rows)


def read_table(text: str, *, historical: bool = False) -> Table:
    rows = []
    names = set()
    for kind, macro in zip(("layout", "virtual"), TABLES):
        body = macro_body(text, macro, optional=historical and kind == "virtual")
        for line in body.splitlines():
            row = re.fullmatch(
                r"V\((\w+),\s*(unmeasured|\d+),\s*(unmeasured|\d+)\)", line)
            if row is None or row[1] in names:
                raise InvalidStamp(f"invalid or duplicate measured row: {line}")
            names.add(row[1])
            rows.append((kind, row[1], row[2], row[3]))
    return Table(literal(text, "measured_version"), tuple(rows))


def digest(table: Table, previous: int) -> str:
    # Exact row order, names, both architecture values and both lock stamps.
    # Formatting outside those tokens does not require a compatibility bump.
    payload = f"version={table.version}\nprevious={previous}\n"
    payload += "".join(":".join(row) + "\n" for row in table.rows)
    return hashlib.sha256(payload.encode("ascii")).hexdigest()


def check_lock(table: Table, previous: int, recorded: str, public: int) -> None:
    if not table.measured:
        raise InvalidStamp("layout table is unmeasured")
    if public != table.version:
        raise InvalidStamp("public and measured layout version literals differ")
    if table.version <= previous:
        raise InvalidStamp("measured layout version must exceed previous_measured_version")
    if not re.fullmatch(r"[0-9a-f]{64}", recorded) or recorded != digest(table, previous):
        raise InvalidStamp("table_sha256 differs; review measurements and explicitly renew the lock")


def check_transition(table: Table, previous: int, baseline: Table | None) -> None:
    if baseline is None:
        # The first guarded table succeeds the pre-guard stamp 1. Only an
        # unmeasured history can reach this bootstrap case.
        if previous != 1:
            raise InvalidStamp("first measured table must record previous stamp 1")
        return
    if previous != baseline.version or table.version <= baseline.version:
        raise InvalidStamp("table changed without advancing its preceding issued layout stamp")


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(root), *args], text=True,
                            capture_output=True, check=False)
    if result.returncode:
        raise InvalidStamp(f"cannot read layout history: {result.stderr.strip()}")
    return result.stdout


def previous_table(root: Path, current: Table) -> Table | None:
    if git(root, "rev-parse", "--is-shallow-repository").strip() != "false":
        raise InvalidStamp("full Git history is required to validate the previous layout stamp")
    commits = git(root, "log", "--first-parent", "--format=%H", "HEAD", "--",
                  FINGERPRINT).splitlines()
    for commit in commits:
        text = git(root, "show", f"{commit}:{FINGERPRINT}")
        table = read_table(text, historical=True)
        if table.measured and table != current:
            return table
    return None


def check_probe(table: Table, probe: str) -> None:
    for kind, name in (("layout", "PINEFORGE_ENGINE_LAYOUT_VALUES"),
                       ("virtual", "PINEFORGE_ENGINE_LAYOUT_VIRTUALS")):
        actual = re.findall(r"^\s*V\((\w+),", macro_body(probe, name), re.MULTILINE)
        expected = [row[1] for row in table.rows if row[0] == kind]
        if not actual or actual != expected:
            raise InvalidStamp(f"{kind} probe and measured table names/order differ")


def check(root: Path, print_digest: bool) -> None:
    text = (root / FINGERPRINT).read_text()
    table = read_table(text)
    previous = literal(text, "previous_measured_version")
    check_probe(table, (root / PROBE).read_text())
    if print_digest:
        if not table.measured:
            raise InvalidStamp("measure both architecture columns before recording a digest")
        print(digest(table, previous))
        return
    hashes = re.findall(r'\btable_sha256\[\]\s*=\s*"([^"]*)"\s*;', text)
    if len(hashes) != 1:
        raise InvalidStamp("expected one literal table_sha256 lock")
    public = re.findall(r"^#define PF_STREAM_ENGINE_LAYOUT_VERSION\s+(\d+)u?\s*$",
                        (root / PUBLIC).read_text(), re.MULTILINE)
    if len(public) != 1:
        raise InvalidStamp("expected one public layout version literal")
    check_lock(table, previous, hashes[0], int(public[0]))
    check_transition(table, previous, previous_table(root, table))


class StampTests(unittest.TestCase):
    def setUp(self) -> None:
        self.old = Table(2, (("layout", "engine_size", "8", "16"),
                             ("virtual", "slot_present_report", "1", "2")))
        self.new = Table(3, (("layout", "engine_size", "16", "16"),
                             ("virtual", "slot_present_report", "1", "2")))

    def test_deliberate_renewal(self) -> None:
        check_lock(self.new, 2, digest(self.new, 2), 3)
        check_transition(self.new, 2, self.old)

    def test_table_edit_requires_bump_even_with_renewed_hash(self) -> None:
        same_stamp = Table(self.old.version, self.new.rows)
        check_lock(same_stamp, 1, digest(same_stamp, 1), 2)
        with self.assertRaisesRegex(InvalidStamp, "preceding issued"):
            check_transition(same_stamp, 1, self.old)

    def test_both_literals_changed_without_lock(self) -> None:
        with self.assertRaisesRegex(InvalidStamp, "table_sha256"):
            check_lock(self.new, 2, digest(self.old, 1), 3)

    def test_previous_cannot_be_skipped(self) -> None:
        with self.assertRaisesRegex(InvalidStamp, "preceding issued"):
            check_transition(self.new, 1, self.old)

    def test_public_must_match(self) -> None:
        with self.assertRaisesRegex(InvalidStamp, "literals differ"):
            check_lock(self.new, 2, digest(self.new, 2), 2)

    def test_slots_and_both_architectures_enter_digest(self) -> None:
        for row_index in range(len(self.old.rows)):
            for column in (2, 3):
                rows = [list(row) for row in self.old.rows]
                rows[row_index][column] = str(int(rows[row_index][column]) + 1)
                changed = Table(2, tuple(tuple(row) for row in rows))
                with self.subTest(row=row_index, column=column):
                    with self.assertRaisesRegex(InvalidStamp, "table_sha256"):
                        check_lock(changed, 1, digest(self.old, 1), 2)

    def test_no_unmeasured_acceptance(self) -> None:
        empty = Table(2, (("layout", "engine_size", "unmeasured", "8"),))
        with self.assertRaisesRegex(InvalidStamp, "unmeasured"):
            check_lock(empty, 1, digest(empty, 1), 2)

    def test_parse_and_probe_inventory(self) -> None:
        text = ("measured_version = 2u;\n"
                "#define PINEFORGE_MEASURED_ENGINE_LAYOUT(V) \\\n"
                "    V(engine_size, 8, 16)\n"
                "#define PINEFORGE_MEASURED_ENGINE_VIRTUALS(V) \\\n"
                "    V(slot_present_report, 1, 2)\n")
        self.assertEqual(read_table(text), self.old)
        probe = ("#define PINEFORGE_ENGINE_LAYOUT_VALUES(V) \\\n"
                 "    V(engine_size, sizeof(BacktestEngine))\n"
                 "#define PINEFORGE_ENGINE_LAYOUT_VIRTUALS(V) \\\n"
                 "    V(slot_present_report, &BacktestEngine::present_report)\n")
        check_probe(self.old, probe)
        with self.assertRaisesRegex(InvalidStamp, "names/order"):
            check_probe(self.old, probe.replace("slot_present_report", "other_slot"))
        with self.assertRaisesRegex(InvalidStamp, "invalid or duplicate"):
            read_table(text.replace("8, 16", "sizeof(BacktestEngine), 16"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--print-digest", action="store_true",
                        help="print a proposed lock digest; never edit files")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner().run(
            unittest.defaultTestLoader.loadTestsFromTestCase(StampTests))
        return 0 if result.wasSuccessful() else 1
    try:
        check(args.root, args.print_digest)
    except (InvalidStamp, OSError) as error:
        print(f"check_engine_layout_stamp: {error}", file=sys.stderr)
        return 1
    if not args.print_digest:
        print("check_engine_layout_stamp: measured table, digest and stamp history agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
