#!/usr/bin/env python3
"""Mutation controls for the A29 CHECK-parity checker."""
from __future__ import annotations

from pathlib import Path
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
    def fixture(self, twin: str, appendix_rows: str) -> tuple[Path, Path]:
        temporary = tempfile.TemporaryDirectory(prefix="pf-twin-parity-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name) / "repo"
        tests = root / "tests"
        tests.mkdir(parents=True)
        (tests / "test_case_l4d.cpp").write_text(twin)
        ev = Path(temporary.name) / "ev"
        task = ev / "tasks/r4-d"
        task.mkdir(parents=True)
        (task / "REMOVED-TESTS-ab9714be-d1a0862.json").write_text('{"removed":["test_case"]}\n')
        (task / "DELETION-LEDGER.md").write_text(
            checker.APPENDIX_HEADING + "\n\n" + checker.TABLE_HEADING + "\n"
            + "| --- | --- | --- | --- |\n" + appendix_rows)
        return root, ev

    def test_twin_and_ledger_sum_to_base(self) -> None:
        root, ev = self.fixture(
            '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); }\n',
            '| tests/test_case.cpp:4 | CHECK_NEAR(beta(2), 3, 0.1) | owner-private test fixture only | tests/test_case_l4d.cpp:1 public beta receipt |\n')
        self.assertEqual(checker.check_inventory(
            root=root, ev=ev, base_reader=lambda _: BASE),
            {"tests": 1, "base": 3, "twin": 2, "ledgered": 1})

    def test_missing_literal_is_rejected(self) -> None:
        root, ev = self.fixture(
            '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); }\n', "")
        with self.assertRaisesRegex(checker.ParityError, "CHECK parity mismatch"):
            checker.check_inventory(root=root, ev=ev, base_reader=lambda _: BASE)

    def test_extra_twin_literal_is_rejected(self) -> None:
        root, ev = self.fixture(
            '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); CHECK(gamma(1)); }\n',
            '| tests/test_case.cpp:4 | CHECK_NEAR(beta(2), 3, 0.1) | owner-private test fixture only | tests/test_case_l4d.cpp:1 public beta receipt |\n')
        with self.assertRaisesRegex(checker.ParityError, "CHECK parity mismatch"):
            checker.check_inventory(root=root, ev=ev, base_reader=lambda _: BASE)

    def test_scanner_ignores_strings_and_accepts_cpp_digit_separators(self) -> None:
        source = '''void test() {
            const char* message = "this is not CHECK(fake)";
            CHECK(value == 60'000LL);
            CHECK_EQ("CHECK(inside argument)", value);
        }\n'''
        checks = checker.extract_checks(source, "tests/test_case.cpp")
        self.assertEqual([item.text for item in checks], [
            "CHECK(value == 60'000LL)",
            'CHECK_EQ("CHECK(inside argument)", value)',
        ])

    def test_unknown_family_is_rejected(self) -> None:
        root, ev = self.fixture(
            '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); }\n',
            "")
        with self.assertRaisesRegex(checker.ParityError, "no family"):
            checker.check_inventory(root=root, ev=ev, base_reader=lambda _: BASE,
                                    families=["not-a-family"])

    def test_targeted_check_ignores_other_appendix_rows(self) -> None:
        root, ev = self.fixture(
            '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); }\n',
            '| tests/test_case.cpp:4 | CHECK_NEAR(beta(2), 3, 0.1) | owner-private only | tests/test_case_l4d.cpp:2 public row |\n'
            '| tests/test_other.cpp:9 | CHECK(other()) | owner-private only | tests/test_other_l4d.cpp:3 public row |\n')
        self.assertEqual(checker.check_inventory(
            root=root, ev=ev, base_reader=lambda _: BASE, names=["test_case"]),
            {"tests": 1, "base": 3, "twin": 2, "ledgered": 1})

    def test_missing_covering_row_is_rejected(self) -> None:
        root, ev = self.fixture(
            '#define CHECK(x) do {} while (0)\nvoid test() { CHECK(alpha(1)); }\n',
            '| tests/test_case.cpp:4 | CHECK_NEAR(beta(2), 3, 0.1) | owner-private only | tests/test_missing_l4d.cpp:2 public row |\n')
        with self.assertRaisesRegex(checker.ParityError, "covering twin is missing"):
            checker.check_inventory(root=root, ev=ev, base_reader=lambda _: BASE)


if __name__ == "__main__":
    unittest.main()
