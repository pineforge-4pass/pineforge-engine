#!/usr/bin/env python3
"""Must-fail controls for the two oracle tree checks (private-fixture aware).

check_oracle_sha256.py and check_oracle_twin_census.py both run against a tree
where the private-fixture PR removed three oracle sources. Each control here
builds a synthetic repository root from this tree's real inputs -- the two
scripts, tests/oracle/, tests/oracle.sha256 and the present twins -- then
breaks exactly one thing and runs the real script in that root, so the
checker's own code path is what answers:

  * green: the fixture-absent tree passes and both scripts name the three
    private oracles;
  * red: a mutated oracle file, an unpinned oracle file, a missing
    non-private oracle, a pin that stops allowing one of the named private
    files, a twin that lost its exact include, and an oracle whose CHECK count
    moved each fail with their own finding.

Run directly: python3 -B scripts/test_oracle_fixture_checks.py
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_oracle_twin_census as census  # noqa: E402

SCRIPTS = ("check_oracle_sha256.py", "check_oracle_twin_census.py")


class OracleFixtureChecks(unittest.TestCase):
    def fixture(self) -> Path:
        temporary = tempfile.TemporaryDirectory(prefix="pf-oracle-checks-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name) / "repo"
        (root / "scripts").mkdir(parents=True)
        (root / "tests").mkdir()
        for name in SCRIPTS:
            shutil.copy2(ROOT / "scripts" / name, root / "scripts" / name)
        shutil.copytree(ROOT / "tests" / "oracle", root / "tests" / "oracle")
        shutil.copy2(ROOT / "tests" / "oracle.sha256", root / "tests" / "oracle.sha256")
        for twin in census.TWINS.values():
            source = ROOT / "tests" / twin
            if source.is_file():
                shutil.copy2(source, root / "tests" / twin)
        return root

    def run_check(self, root: Path, script: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(root / "scripts" / script)],
                              capture_output=True, text=True, timeout=60)

    def assert_fails(self, root: Path, script: str, needle: str) -> None:
        result = self.run_check(root, script)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(needle, result.stdout + result.stderr)

    def test_the_fixture_absent_tree_passes_and_names_the_private_oracles(self) -> None:
        root = self.fixture()
        for script in SCRIPTS:
            result = self.run_check(root, script)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("private fixtures not present", result.stdout)
            for private in census.PRIVATE_FIXTURE_ORACLES:
                self.assertIn(private + ".cpp", result.stdout)
        # A private oracle restored with the wrong bytes is drift, not a skip:
        # the pinned hash is enforced whenever the file is present.
        (root / "tests" / "oracle" / "test_oracle_stop_snapshot.cpp").write_text(
            "// restored private oracle placeholder\n")
        self.assert_fails(root, "check_oracle_sha256.py",
                          "content changed: test_oracle_stop_snapshot.cpp")

    def test_a_mutated_oracle_file_fails_the_sha256_pin(self) -> None:
        root = self.fixture()
        target = root / "tests" / "oracle" / "test_oracle_coof.cpp"
        target.write_text(target.read_text() + "\n")
        self.assert_fails(root, "check_oracle_sha256.py", "content changed: test_oracle_coof.cpp")

    def test_an_unpinned_oracle_file_fails_both_checks(self) -> None:
        root = self.fixture()
        (root / "tests" / "oracle" / "test_oracle_extra.cpp").write_text("// new\n")
        self.assert_fails(root, "check_oracle_sha256.py", "unpinned file: test_oracle_extra.cpp")
        self.assert_fails(root, "check_oracle_twin_census.py", "unpinned oracle: test_oracle_extra")

    def test_a_missing_non_private_oracle_fails_both_checks(self) -> None:
        root = self.fixture()
        (root / "tests" / "oracle" / "test_oracle_coof.cpp").unlink()
        self.assert_fails(root, "check_oracle_sha256.py", "missing pinned file: test_oracle_coof.cpp")
        self.assert_fails(root, "check_oracle_twin_census.py", "missing oracle: test_oracle_coof")

    def test_a_pin_that_stops_allowing_a_private_oracle_fails(self) -> None:
        root = self.fixture()
        pin = json.loads((root / "tests" / "oracle.sha256").read_text())
        pin["privateFixtureAbsent"] = [name for name in pin["privateFixtureAbsent"]
                                       if name != "test_oracle_day_key.cpp"]
        (root / "tests" / "oracle.sha256").write_text(json.dumps(pin))
        self.assert_fails(root, "check_oracle_sha256.py",
                          "missing pinned file: test_oracle_day_key.cpp")

    def test_a_pin_allowing_a_file_outside_the_removal_set_fails(self) -> None:
        root = self.fixture()
        pin = json.loads((root / "tests" / "oracle.sha256").read_text())
        pin["privateFixtureAbsent"] = pin["privateFixtureAbsent"] + ["test_oracle_ghost.cpp"]
        (root / "tests" / "oracle.sha256").write_text(json.dumps(pin))
        self.assert_fails(root, "check_oracle_sha256.py", "partial private fixture removal")

    def test_a_twin_that_lost_its_include_fails_the_census(self) -> None:
        root = self.fixture()
        twin = root / "tests" / census.TWINS["test_oracle_coof"]
        twin.write_text(twin.read_text().replace('#include "oracle/test_oracle_coof.cpp"',
                                                 "// include dropped"))
        self.assert_fails(root, "check_oracle_twin_census.py", "must include exactly")

    def test_an_oracle_whose_check_count_moved_fails_the_census(self) -> None:
        root = self.fixture()
        target = root / "tests" / "oracle" / "test_oracle_coof.cpp"
        target.write_text(target.read_text() + "\nvoid extra() { CHECK(true); }\n")
        self.assert_fails(root, "check_oracle_twin_census.py",
                          "test_oracle_coof CHECK count 35, expected 34")


if __name__ == "__main__":
    unittest.main()
