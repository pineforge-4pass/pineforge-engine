#!/usr/bin/env python3
"""Mutation controls for the repo-local, literal-aware A29 parity guard."""
from __future__ import annotations

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

import check_twin_parity as checker


BASE = '''#define CHECK(x) do {} while (0)
void test() {
    CHECK(alpha(1));
    CHECK_NEAR(beta(2), 3, 0.1);
}
'''


class TwinParity(unittest.TestCase):
    def fixture(self, twin: str, ledger_rows: str,
                base: str = BASE) -> tuple[Path, Path, Path]:
        temporary = tempfile.TemporaryDirectory(prefix="pf-twin-parity-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name) / "repo"
        tests = root / "tests"
        tests.mkdir(parents=True)
        (tests / "test_case_l4d.cpp").write_text(twin)
        inventory = tests / "inventory.json"
        inventory.write_text(
            '{"base":"' + checker.BASE + '","removed":["test_case"],'
            '"families":{"fixture":["test_case"]}}\n')
        ledger = tests / "ledger.md"
        ledger.write_text(
            checker.APPENDIX_HEADING + "\n\n" + checker.TABLE_HEADING + "\n"
            + "| --- | --- | --- | --- |\n" + ledger_rows)
        return root, inventory, ledger

    def check(self, twin: str, ledger_rows: str = "", base: str = BASE):
        root, inventory, ledger = self.fixture(twin, ledger_rows, base)
        return checker.check_inventory(
            root=root, inventory=inventory, ledger=ledger,
            base_reader=lambda _: base, names=["test_case"])

    def test_literal_twin_and_exact_ledger_cover_base(self) -> None:
        result = self.check(
            '#define CHECK(x) do {} while (0)\n'
            'void test() { CHECK(alpha(1)); CHECK(extra_public_fact()); }\n',
            '| tests/test_case.cpp:4 | CHECK_NEAR(beta(2), 3, 0.1) | '
            'owner-private fixture | tests/test_case_l4d.cpp:2 public beta receipt |\n')
        self.assertEqual(result, {
            "tests": 1, "base": 2, "matched": 1, "ledgered": 1,
            "rewritten": 0, "extra": 1})

    def test_same_count_check_true_mutation_is_rejected(self) -> None:
        with self.assertRaisesRegex(checker.ParityError,
                                    r"tautological.*CHECK\(true\)"):
            self.check(
                '#define CHECK(x) do {} while (0)\n'
                'void test() { CHECK(true); CHECK_NEAR(beta(2), 3, 0.1); }\n')

    def test_require_and_assert_are_obligations(self) -> None:
        base = '''#include <cassert>
#define REQUIRE(x) do {} while (0)
void test() { REQUIRE(identity == 41); assert(created_seq == 7); }
'''
        result = self.check(
            '#include <cassert>\n#define REQUIRE(x) do {} while (0)\n'
            'void test() { REQUIRE(identity == 41); assert(created_seq == 7); }\n',
            base=base)
        self.assertEqual(result["base"], 2)
        self.assertEqual(result["matched"], 2)

    def test_dropped_require_is_rejected(self) -> None:
        base = '#define REQUIRE(x) do {} while (0)\nvoid test() { REQUIRE(identity == 41); }\n'
        with self.assertRaisesRegex(checker.ParityError,
                                    r"REQUIRE\(identity == 41\)"):
            self.check('#define REQUIRE(x) do {} while (0)\nvoid test() {}\n', base=base)

    def test_counted_owner_range_accounts_only_unmatched_rows(self) -> None:
        base = '''#define CHECK(x) do {} while (0)
void test() {
    CHECK(owner_private_a());
    CHECK(owner_private_b());
    CHECK(public_result());
}
'''
        result = self.check(
            '#define CHECK(x) do {} while (0)\n'
            'void test() { CHECK(public_result()); }\n',
            '| tests/test_case.cpp:3-4 | 2 CHECKs | retired owner helper | '
            'no public projection | tests/test_case_l4d.cpp:2 public result |\n',
            base=base)
        self.assertEqual(result, {
            "tests": 1, "base": 3, "matched": 1, "ledgered": 2,
            "rewritten": 0, "extra": 0})

    def test_range_count_cannot_exceed_source_rows(self) -> None:
        with self.assertRaisesRegex(checker.ParityError,
                                    "requires 3 unmatched.*found only 1"):
            self.check(
                '#define CHECK(x) do {} while (0)\n'
                'void test() { CHECK(alpha(1)); }\n',
                '| tests/test_case.cpp:3-4 | 3 CHECKs | retired owner helper | '
                'no public projection | tests/test_case_l4d.cpp:2 public result |\n')

    def test_scanner_ignores_strings_and_cpp_digit_separators(self) -> None:
        source = '''void test() {
            const char* message = "this is not CHECK(fake)";
            CHECK(value == 60'000LL);
            REQUIRE_EQ("CHECK(inside argument)", value);
        }\n'''
        assertions = checker.extract_assertions(source, "tests/test_case.cpp")
        self.assertEqual([item.text for item in assertions], [
            "CHECK(value == 60'000LL)",
            'REQUIRE_EQ("CHECK(inside argument)", value)',
        ])

    def test_macro_definitions_are_not_assertions(self) -> None:
        assertions = checker.extract_assertions(
            '#define CHECK(x) do { if (!(x)) abort(); } while (0)\n',
            "tests/test_case.cpp")
        self.assertEqual(assertions, [])

    def test_missing_covering_line_is_rejected(self) -> None:
        with self.assertRaisesRegex(checker.ParityError, "covering line has no assertion"):
            self.check(
                '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); }\n',
                '| tests/test_case.cpp:4 | CHECK_NEAR(beta(2), 3, 0.1) | '
                'owner-private | tests/test_case_l4d.cpp:99 public row |\n')

    def test_obvious_tautology_is_rejected_even_when_extra(self) -> None:
        with self.assertRaisesRegex(checker.ParityError, "tautological"):
            self.check(
                '#define CHECK(x) do {} while (0)\n'
                'void test() { CHECK(alpha(1)); CHECK_NEAR(beta(2), 3, 0.1); '
                'CHECK(value == value); }\n')

    def test_default_checker_runs_outside_repository_cwd(self) -> None:
        script = Path(__file__).with_name("check_twin_parity.py")
        with tempfile.TemporaryDirectory(prefix="pf-twin-cwd-") as directory:
            result = subprocess.run(
                [sys.executable, str(script), "--name", "test_aux_security_feed"],
                cwd=directory, text=True, capture_output=True,
                env={key: value for key, value in os.environ.items()
                     if key != "PINEFORGE_R4D_EV"}, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("not a git repository", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
