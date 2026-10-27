#!/usr/bin/env python3
"""Drive the registered l22v pytest command and inspect its JUnit report."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).absolute().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import check_cr150_cmake_d as checker
from test_run_pytest_no_skips import declared_inputs

ARTIFACTS = None


class NativeSkipAccountingControls(unittest.TestCase):
    def setUp(self):
        artifacts = ARTIFACTS
        if artifacts is None:
            artifacts = Path(tempfile.mkdtemp(prefix="pineforge-l22v-skip-accounting-"))
        declared_inputs((artifacts,))
        artifacts.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="control-", dir=artifacts))

    def run_registered_driver(self, body):
        cmake = ROOT / "tests/cr150.d/core-cr150-l22v.cmake"
        declared_inputs((cmake, ROOT / "scripts/check_cr150_cmake_d.py"))
        # Coupled to the registered L22V command and the checker's private CMake parsers.
        statements = checker._statements(cmake, cmake.read_text(), mask_quotes=True)
        registrations = [checker._fence_words(statement) for statement in statements
                         if statement.name.lower() == "add_test"
                         and checker._fence_words(statement, limit=2)
                         == ["NAME", "test_run_json_sink_fd_native_l22v"]]
        self.assertEqual(1, len(registrations), "L22V must have exactly one native row")
        words = registrations[0]
        source = words.index("${PROJECT_SOURCE_DIR}/tests/test_run_json_sink_fd.py")
        # The module environment belongs to the engine tests. Exercise the
        # registered Python driver with controlled pytest input, as H2-6 does.
        driver = words[words.index("${Python3_EXECUTABLE}"):source]
        selection = words[source + 1:]
        for old, new in {
                "${Python3_EXECUTABLE}": sys.executable,
                "${PROJECT_SOURCE_DIR}": str(ROOT),
                "${CMAKE_CURRENT_BINARY_DIR}": str(self.work),
                "${CMAKE_BINARY_DIR}": str(self.work)}.items():
            driver = [word.replace(old, new) for word in driver]
        # Plain pytest on the base must also produce its own evidence. The
        # fixed wrapper owns its report path and refuses a caller-supplied one.
        if "-m" in driver and "pytest" in driver:
            driver += ["--junitxml", str(self.work / "junit.xml")]
        fixture = self.work / "test_fixture.py"
        configuration = self.work / "pytest.ini"
        declared_inputs((fixture, configuration))
        fixture.write_text(body)
        configuration.write_text("[pytest]\n")
        declared_inputs(word for word in driver if word.endswith(".py"))
        result = subprocess.run(
            [*driver, str(fixture), *selection, "-c", str(configuration),
             "--confcutdir", str(self.work)],
            cwd=self.work,
            env=dict(os.environ, PYTEST_DISABLE_PLUGIN_AUTOLOAD="1"),
            capture_output=True, text=True)
        (self.work / "stdout.txt").write_text(result.stdout)
        (self.work / "stderr.txt").write_text(result.stderr)
        reports = list(self.work.rglob("junit.xml"))
        self.assertEqual(1, len(reports), result.stdout + result.stderr)
        declared_inputs(reports)
        root = ET.parse(reports[0]).getroot()
        return result, list(root.iter("testcase"))

    def test_permission_skips_cannot_make_the_native_row_green(self):
        result, cases = self.run_registered_driver(
            "import pytest\n\n"
            "def test_native_tmpdir_precondition():\n"
            "    pytest.skip('mode-0555 TMPDIR permits create: root or DAC capabilities')\n\n"
            "def test_native_work_precondition():\n"
            "    pytest.skip('mode-0555 work permits create: root or DAC capabilities')\n\n"
            "def test_native_present():\n"
            "    assert 2 + 3 == 5\n")
        # Oracle: two explicit skips plus one executed assertion = three
        # selected cases, two skipped cases, and no failed/error cases.
        self.assertEqual(3, len(cases), result.stdout + result.stderr)
        self.assertEqual(2, sum(case.find("skipped") is not None for case in cases))
        self.assertFalse(any(case.find("failure") is not None
                             or case.find("error") is not None for case in cases))
        self.assertEqual(
            1, result.returncode,
            "two JUnit permission skips were hidden in a green native_l22v row\n"
            + result.stdout + result.stderr)

    def test_an_executed_native_row_stays_green(self):
        result, cases = self.run_registered_driver(
            "def test_native_present():\n    assert 2 + 3 == 5\n")
        self.assertEqual(1, len(cases), result.stdout + result.stderr)
        self.assertFalse(list(cases[0]), result.stdout + result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", type=Path, default=ARTIFACTS)
    arguments, remaining = parser.parse_known_args()
    ARTIFACTS = arguments.artifacts
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
