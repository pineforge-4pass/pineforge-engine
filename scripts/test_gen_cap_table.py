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

SCRATCH_TABLES = """struct ScratchManifest {
    std::uint64_t K_q = 0;
    std::uint64_t B_q = 0;
    std::uint64_t K_e = 0;
    std::uint64_t B_e = 0;
    std::uint64_t K_c = 0;
    std::uint64_t B_c = 0;
    std::uint64_t U = 0;
    std::uint64_t B_U = 0;
    std::uint64_t V = 0;
    std::uint64_t B_V = 0;
    std::uint64_t R = 0;
};
"""

SCRATCH_DEFAULTS = """#pragma once
inline ScratchManifest default_scratch_manifest(std::uint64_t bars) {
    return {
        0, 0, // Reserved descriptor fields, not an initializer.
        262144, 25165824,
        32768, 4227072,
        65536, 4194304,
        27, 6912,
        2 * bars,
    };
}

inline ScratchManifest default_scratch_manifest() {
    auto manifest = default_scratch_manifest(0);
    manifest.R = 65536;
    return manifest;
}
"""


def write_fixture_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


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
        self.header = self.root / "include/pineforge/detail/caps.hpp"
        write_fixture_file(self.header, HEADER)
        write_fixture_file(
            self.root / "scripts/cap_table_rows.json", json.dumps(manifest(), indent=2) + "\n"
        )
        write_fixture_file(
            self.root / "docker/run_failure_codes.json", json.dumps(CATALOG, indent=2) + "\n"
        )
        self.page = self.root / "docs/pages/caps-and-defaults.md"
        write_fixture_file(self.page, "# Caps\n\n" + BEGIN + "\n\n" + END + "\n")

    def add_rows(self, header: str, rows: dict[str, str]) -> None:
        path = self.root / "scripts/cap_table_rows.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        for name, constant in rows.items():
            value["rows"].append({
                **value["rows"][0], "name": name, "constant": constant, "header": header,
            })
        write_fixture_file(path, json.dumps(value, indent=2) + "\n")

    def add_scratch_manifest(self) -> Path:
        header = "include/pineforge/detail/scratch_defaults.hpp"
        write_fixture_file(self.root / header, SCRATCH_DEFAULTS)
        write_fixture_file(self.root / "include/pineforge/detail/scratch_tables.hpp",
                           SCRATCH_TABLES)
        self.add_rows(header, {
            **{f"manifest-{field}": f"default_scratch_manifest(bars).{field}"
               for field in ("K_q", "B_q", "K_e", "B_e", "K_c", "B_c",
                             "U", "B_U", "V", "B_V", "R")},
            "stream-R": "default_scratch_manifest().R",
        })
        return self.root / header

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
        write_fixture_file(self.fixture.header, HEADER.replace("kAlpha = 4", "kAlpha = 7"))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("alpha", checked.stderr)
        self.assertIn("4", checked.stderr)
        self.assertIn("7", checked.stderr)

    def test_removing_a_constant_fails_and_names_the_row(self) -> None:
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        write_fixture_file(
            self.fixture.header, HEADER.replace("inline constexpr unsigned kAlpha = 4;\n", "")
        )
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("alpha", checked.stderr)
        self.assertIn("kAlpha", checked.stderr)

    def test_new_scratch_resource_requires_a_row(self) -> None:
        write_fixture_file(
            self.fixture.header, HEADER.replace("    cohort,\n", "    cohort,\n    extra,\n")
        )
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("extra", checked.stderr)
        self.assertIn("missing manifest rows", checked.stderr)

    def test_hand_edit_of_generated_block_fails(self) -> None:
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        page = self.fixture.page.read_text(encoding="utf-8")
        write_fixture_file(self.fixture.page, page.replace("`4`", "`99`"))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("alpha", checked.stderr)
        self.assertIn("99", checked.stderr)
        self.assertIn("4", checked.stderr)

    def test_aggregate_values_and_source_lines(self) -> None:
        self.fixture.add_scratch_manifest()
        written = self.fixture.run("--write")
        self.assertEqual(written.returncode, 0, written.stderr)
        page = self.fixture.page.read_text(encoding="utf-8")
        expected = {
            "K_q": ("0", 4), "B_q": ("0", 4),
            "K_e": ("262144", 5), "B_e": ("25165824", 5),
            "K_c": ("32768", 6), "B_c": ("4227072", 6),
            "U": ("65536", 7), "B_U": ("4194304", 7),
            "V": ("27", 8), "B_V": ("6912", 8), "R": ("2 * bars", 9),
        }
        for field, (value, line) in expected.items():
            with self.subTest(field=field):
                self.assertIn(
                    f"| `manifest-{field}` | `{value}` "
                    f"(include/pineforge/detail/scratch_defaults.hpp:{line})", page
                )
        self.assertIn("| `stream-R` | `65536` "
                      "(include/pineforge/detail/scratch_defaults.hpp:15)", page)
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_mutating_an_aggregate_value_fails(self) -> None:
        header = self.fixture.add_scratch_manifest()
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        write_fixture_file(header, SCRATCH_DEFAULTS.replace("262144", "262145"))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("manifest-K_e", checked.stderr)
        self.assertIn("262144", checked.stderr)
        self.assertIn("262145", checked.stderr)

    def test_mutating_the_stream_reserve_fails(self) -> None:
        header = self.fixture.add_scratch_manifest()
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        write_fixture_file(header, SCRATCH_DEFAULTS.replace("manifest.R = 65536",
                                                          "manifest.R = 65537"))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("stream-R", checked.stderr)
        self.assertIn("65536", checked.stderr)
        self.assertIn("65537", checked.stderr)

    def test_aggregate_uses_source_field_order(self) -> None:
        self.fixture.add_scratch_manifest()
        self.assertEqual(self.fixture.run("--write").returncode, 0)
        write_fixture_file(
            self.fixture.root / "include/pineforge/detail/scratch_tables.hpp",
            SCRATCH_TABLES.replace(
                "    std::uint64_t K_e = 0;\n    std::uint64_t B_e = 0;",
                "    std::uint64_t B_e = 0;\n    std::uint64_t K_e = 0;",
            ),
        )
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("manifest-K_e", checked.stderr)
        self.assertIn("262144", checked.stderr)
        self.assertIn("25165824", checked.stderr)

    def test_missing_aggregate_field_fails(self) -> None:
        self.fixture.add_scratch_manifest()
        path = self.fixture.root / "scripts/cap_table_rows.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value["rows"][2]["constant"] = "default_scratch_manifest(bars).missing"
        write_fixture_file(path, json.dumps(value))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("manifest-K_q", checked.stderr)
        self.assertIn("missing source constant", checked.stderr)

    def test_incomplete_aggregate_fails(self) -> None:
        header = self.fixture.add_scratch_manifest()
        write_fixture_file(header, SCRATCH_DEFAULTS.replace("        2 * bars,\n", ""))
        written = self.fixture.run("--write")
        self.assertEqual(written.returncode, 1)
        self.assertIn("10 values for 11 fields", written.stderr)

    def test_struct_defaults_and_scoped_literal(self) -> None:
        header = "include/pineforge/detail/sizing.hpp"
        text = """struct Shape {
    std::uint32_t page_bytes = 4096;
    std::uint64_t max_live_objects = 0;
};
struct ScratchStoreConfig {
    std::size_t page_count = 0;
    ScratchKeyOrder order{};
    ScratchOpenMode mode = ScratchOpenMode::reopen;
};
inline bool valid() {
    if (first > 64 || second > 64) return false;
    return count <= (std::size_t{1} << 24);
}
"""
        write_fixture_file(self.fixture.root / header, text)
        self.fixture.add_rows(header, {
            "shape-page": "Shape::page_bytes",
            "shape-limit": "Shape::max_live_objects",
            "config-count": "ScratchStoreConfig::page_count",
            "config-mode": "ScratchStoreConfig::mode",
            "page-ceiling": "literal:std::size_t{1} << 24",
            "pair-ceiling": "literal:first > 64 || second > 64",
        })
        written = self.fixture.run("--write")
        self.assertEqual(written.returncode, 0, written.stderr)
        page = self.fixture.page.read_text(encoding="utf-8")
        for name, value in {
            "shape-page": "4096", "shape-limit": "0", "config-count": "0",
            "config-mode": "ScratchOpenMode::reopen", "page-ceiling": "std::size_t{1} << 24",
            "pair-ceiling": r"first > 64 \|\| second > 64",
        }.items():
            with self.subTest(row=name):
                self.assertIn(f"| `{name}` | `{value}` ", page)
        self.assertEqual(self.fixture.run("--check").returncode, 0)
        write_fixture_file(self.fixture.root / header, text.replace("page_bytes = 4096",
                                                                  "page_bytes = 8192"))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("shape-page", checked.stderr)
        self.assertIn("4096", checked.stderr)
        self.assertIn("8192", checked.stderr)

    def test_missing_struct_field_fails(self) -> None:
        header = "include/pineforge/detail/sizing.hpp"
        write_fixture_file(self.fixture.root / header,
                           "struct Shape {\n    std::uint32_t page_bytes = 4096;\n};\n")
        self.fixture.add_rows(header, {"shape-page": "Shape::missing"})
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("shape-page", checked.stderr)
        self.assertIn("Shape::missing", checked.stderr)
        self.assertIn("missing source constant", checked.stderr)

    def test_separate_resource_header(self) -> None:
        header = "include/pineforge/detail/scratch_errors.hpp"
        write_fixture_file(self.fixture.root / header, HEADER)
        path = self.fixture.root / "scripts/cap_table_rows.json"
        value = manifest()
        value["resource_header"] = header
        write_fixture_file(path, json.dumps(value))
        written = self.fixture.run("--write")
        self.assertEqual(written.returncode, 0, written.stderr)
        self.assertEqual(self.fixture.run("--check").returncode, 0)
        write_fixture_file(self.fixture.root / header,
                           HEADER.replace("    cohort,\n", "    cohort,\n    extra,\n"))
        checked = self.fixture.run("--check")
        self.assertEqual(checked.returncode, 1)
        self.assertIn("extra", checked.stderr)
        self.assertIn("missing manifest rows", checked.stderr)

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
