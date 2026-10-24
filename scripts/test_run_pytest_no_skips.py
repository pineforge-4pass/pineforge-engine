#!/usr/bin/env python3
"""H2-6 controls using the actual L9A CMake row's pytest driver.

On BASE the row invokes plain pytest: the skip control obtains a JUnit skip
and exit 0, then fails its required exit-1 assertion. No missing new wrapper
is used as fail-before evidence. Fixture runs and XML are retained.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "scratch-s4e" / "pytest-no-skips-controls"


def declared_inputs(paths):
    paths = [Path(os.path.abspath(path)) for path in paths]
    print("pytest no-skip control inputs:\n" + "\n".join(map(str, paths)), flush=True)
    protected = tuple(Path.home() / name for name in (
        ".config/zsh/keys", ".ssh", ".aws", ".codex", ".config/pineforge"))

    def refuse(path):
        if path.name.endswith(".env") or any(
                path == prefix or prefix in path.parents for prefix in protected):
            raise AssertionError(f"refusing protected input: {path}")

    for path in paths:
        refuse(path)
        refuse(path.resolve())


class NoSkipControls(unittest.TestCase):
    def setUp(self):
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="control-", dir=ARTIFACTS))

    def run_row_driver(self, body):
        cmake = ROOT / "tests" / "CMakeLists.txt"
        declared_inputs((cmake, ROOT / "scripts" / "check_cr150_cmake_d.py"))
        import check_cr150_cmake_d as checker
        statements = checker._statements(cmake, cmake.read_text(), mask_quotes=True)
        registrations = [checker._fence_words(statement) for statement in statements
                         if statement.name.lower() == "add_test"
                         and checker._fence_words(statement, limit=2)
                         == ["NAME", "test_run_json_sink_l9a"]]
        self.assertEqual(1, len(registrations), "L9A must have exactly one control row")
        words = registrations[0]
        source = words.index("${PROJECT_SOURCE_DIR}/tests/test_run_json_sink.py")
        driver = words[3:source]
        replacements = {
            "${Python3_EXECUTABLE}": sys.executable,
            "${PROJECT_SOURCE_DIR}": str(ROOT),
            "${CMAKE_CURRENT_BINARY_DIR}": str(self.work),
            "${CMAKE_BINARY_DIR}": str(self.work),
        }
        for old, new in replacements.items():
            driver = [word.replace(old, new) for word in driver]
        if "-m" in driver and "pytest" in driver:
            driver += ["--junitxml", str(self.work / "junit.xml")]
        fixture = self.work / "test_fixture.py"
        fixture.write_text(body)
        configuration = self.work / "pytest.ini"
        configuration.write_text("[pytest]\n")
        declared_inputs([fixture, configuration]
                        + [word for word in driver if word.endswith(".py")])
        environment = dict(os.environ, PYTEST_DISABLE_PLUGIN_AUTOLOAD="1")
        return subprocess.run(
            [*driver, str(fixture), "-c", str(configuration),
             "--confcutdir", str(self.work)],
            cwd=self.work, env=environment, capture_output=True, text=True)

    def test_a_skip_cannot_be_a_green_ctest_row(self):
        result = self.run_row_driver(
            "import pytest\n\ndef test_optional():\n    pytest.skip('missing native fixture')\n")
        reports = list(self.work.rglob("junit.xml"))
        self.assertEqual(1, len(reports), result.stdout + result.stderr)
        declared_inputs(reports)
        root = ET.parse(reports[0]).getroot()
        self.assertEqual(1, sum(int(suite.get("skipped", "0"))
                                for suite in root.iter("testsuite")))
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)

    def test_a_passing_row_stays_green(self):
        result = self.run_row_driver("def test_present():\n    assert 2 + 2 == 4\n")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_a_failed_assertion_keeps_the_pytest_exit(self):
        result = self.run_row_driver("def test_failure():\n    assert False, 'control failure'\n")
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)

    def test_deliberate_crash_keeps_the_work_exit(self):
        # A new run script must prove that an interrupted pytest cannot turn
        # into a successful wrapper exit or reuse a previous JUnit report.
        result = self.run_row_driver("import os\n\ndef test_crash():\n    os._exit(23)\n")
        self.assertEqual(23, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, default=ARTIFACTS)
    arguments, remaining = parser.parse_known_args()
    ARTIFACTS = arguments.artifacts
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
