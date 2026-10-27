#!/usr/bin/env python3
"""Mutation tests for gen_cap_table.py.

The fixture deliberately starts with an empty generated block. The tests then
drive the same command line used by preflight, proving that source changes,
manifest omissions, resource-vocabulary changes and page edits all fail.
"""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parent / "gen_cap_table.py"
ROOT = SCRIPT.parents[1]
BEGIN = "<!-- BEGIN generated cap table: python3 scripts/gen_cap_table.py --write -->"
END = "<!-- END generated cap table -->"

HEADER = """#pragma once
inline constexpr unsigned kAlpha = 4;
inline constexpr unsigned kBeta = 8;
enum class ScratchResource {
    layout = 0,
    cohort,
};
"""


def manifest() -> dict:
    row = {
        "header": "include/pineforge/detail/caps.hpp",
        "unit": "items",
        "enforced": "fixture",
        "refusal": {"code": "scratch_capacity_exceeded", "resource": "layout"},
        "unlimited": "no",
        "owner": "compile-time",
    }
    return {
        "resource_header": "include/pineforge/detail/caps.hpp",
        "rows": [
            {**row, "name": "alpha", "constant": "kAlpha", "resource": "layout"},
            {**row, "name": "beta", "constant": "kBeta", "resource": "cohort"},
        ],
    }


CATALOG = {
    "codes": {
        "scratch_capacity_exceeded": {
            "args": {"resource": {"values": ["layout", "cohort"]}}
        }
    }
}


class Fixture:
    def __init__(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="pf-cap-table-test-")
        self.root = Path(self.tmp.name)
        (self.root / "include/pineforge").mkdir(parents=True)
        (self.root / "scripts").mkdir()
        (self.root / "docker").mkdir()
        (self.root / "docs/pages").mkdir(parents=True)
        self.header = self.root / "include/pineforge/detail/caps.hpp"
        self.header.write_text(HEADER, encoding="utf-8")
        (self.root / "scripts/cap_table_rows.json").write_text(
            json.dumps(manifest(), indent=2) + "\n", encoding="utf-8"
        )
        (self.root / "docker/run_failure_codes.json").write_text(
            json.dumps(CATALOG, indent=2) + "\n", encoding="utf-8"
        )
        self.page = self.root / "docs/pages/caps-and-defaults.md"
        self.page.write_text(
            "# Caps\n\n" + BEGIN + "\n\n" + END + "\n", encoding="utf-8"
        )

    def close(self) -> None:
        self.tmp.cleanup()

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.root), *args],
            capture_output=True,
            text=True,
            check=False,
        )


class CapTableGenerator(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)

    def test_write_then_check_passes(self) -> None:
        written = self.fixture.run("--write")
        self.assertEqual(written.returncode, 0, written.stderr)
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_mutating_a_constant_fails_and_names_the_row(self) -> None:
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        self.fixture.header.write_text(HEADER.replace("kAlpha = 4", "kAlpha = 7"),
                                       encoding="utf-8")
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("alpha", checked.stderr)
        self.assertIn("4", checked.stderr)
        self.assertIn("7", checked.stderr)

    def test_removing_a_constant_fails_and_names_the_row(self) -> None:
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        self.fixture.header.write_text(HEADER.replace("inline constexpr unsigned kAlpha = 4;\n", ""),
                                       encoding="utf-8")
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("alpha", checked.stderr)
        self.assertIn("kAlpha", checked.stderr)

    def test_new_scratch_resource_requires_a_row(self) -> None:
        self.fixture.header.write_text(HEADER.replace("    cohort,\n", "    cohort,\n    extra,\n"),
                                       encoding="utf-8")
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("extra", checked.stderr)
        self.assertIn("missing manifest rows", checked.stderr)

    def test_hand_edit_of_generated_block_fails(self) -> None:
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        page = self.fixture.page.read_text(encoding="utf-8")
        self.fixture.page.write_text(page.replace("`4`", "`99`"), encoding="utf-8")
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("alpha", checked.stderr)
        self.assertIn("99", checked.stderr)
        self.assertIn("4", checked.stderr)

    def test_real_tree_check_is_exercised(self) -> None:
        checked = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(ROOT), "--check"],
            capture_output=True,
            text=True,
            check=False,
        )
        page = ROOT / "docs/pages/caps-and-defaults.md"
        if "| Row | Source value |" in page.read_text(encoding="utf-8"):
            self.assertEqual(checked.returncode, 0, checked.stderr)
        else:
            self.assertEqual(checked.returncode, 1)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
