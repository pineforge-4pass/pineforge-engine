#!/usr/bin/env python3
"""Property controls for the real CR-150 lane CMake checker.

Each case copies the checker into a temporary repository and invokes its
normal entry point. PF_CR150_CMAKE_D_CHECKER can name the previous checker
for lab fail-before evidence; the refused-property cases must fail there.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
CHECKER = Path(os.environ.get(
    'PF_CR150_CMAKE_D_CHECKER', ROOT / 'scripts' / 'check_cr150_cmake_d.py'))
REGISTRATION = ('add_test(NAME test_lane_property COMMAND ${Python3_EXECUTABLE} '
                '-B ${PROJECT_SOURCE_DIR}/scripts/check_cr150_cmake_d.py)\n')


class LanePropertyControls(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix='cr150-cmake-properties-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        scripts = self.root / 'scripts'
        scripts.mkdir()
        self.checker = scripts / 'check_cr150_cmake_d.py'
        shutil.copyfile(CHECKER, self.checker)
        lanes = self.root / 'tests' / 'cr150.d'
        lanes.mkdir(parents=True)
        self.lane = lanes / 'property-control.cmake'
        shutil.copyfile(ROOT / 'tests' / 'cr150.d' / 'README.md', lanes / 'README.md')
        # The real final block provides a valid fixture independently of the
        # lane properties; no configure or CMake execution is needed.
        tail = (ROOT / 'tests' / 'CMakeLists.txt').read_text().splitlines(keepends=True)[-9:]
        (self.root / 'tests' / 'CMakeLists.txt').write_text(''.join(tail))

    def run_check(self, statement: str) -> subprocess.CompletedProcess:
        self.lane.write_text(REGISTRATION + statement + '\n')
        return subprocess.run([sys.executable, '-B', str(self.checker)],
                              cwd=self.root, capture_output=True, text=True)

    def assert_refused(self, statement: str, reason: str) -> None:
        result = self.run_check(statement)
        output = result.stdout + result.stderr
        self.assertEqual(1, result.returncode, output)
        self.assertIn(f'{self.lane}:2:', output)
        self.assertIn(reason, output)

    def test_set_target_properties_is_refused(self) -> None:
        self.assert_refused(
            'set_target_properties(pineforge PROPERTIES ENABLE_EXPORTS ON)',
            'set_target_properties')

    def test_environment_is_refused(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES ENVIRONMENT "INPUT=fixture")',
            'ENVIRONMENT')

    def test_working_directory_is_refused(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES WORKING_DIRECTORY fixture)',
            'WORKING_DIRECTORY')

    def test_environment_modification_is_refused(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES '
            'ENVIRONMENT_MODIFICATION "INPUT=set:fixture")',
            'ENVIRONMENT_MODIFICATION')

    def test_unknown_property_is_refused(self) -> None:
        self.assert_refused('set_tests_properties(test_lane_property PROPERTIES FOO bar)',
                            'FOO')

    def test_all_allowed_properties_pass(self) -> None:
        for keys in (('LABELS', 'SKIP_RETURN_CODE', 'TIMEOUT', 'RUN_SERIAL', 'DEPENDS'),
                     ('labels', 'skip_return_code', 'timeout', 'run_serial', 'depends')):
            with self.subTest(keys=keys):
                result = self.run_check(
                    'set_tests_properties(test_lane_property PROPERTIES '
                    f'{keys[0]} "ENVIRONMENT WORKING_DIRECTORY; cr150" '
                    f'{keys[1]} 77 {keys[2]} 30 {keys[3]} TRUE '
                    f'{keys[4]} [=[setup; teardown]=])')
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn('CR-150 CMake lane directory is valid', result.stdout)

    def test_missing_property_pair_is_refused(self) -> None:
        self.assert_refused('set_tests_properties(test_lane_property PROPERTIES)',
                            'needs at least one key/value pair')

    def test_odd_property_count_is_refused(self) -> None:
        self.assert_refused('set_tests_properties(test_lane_property PROPERTIES LABELS fast TIMEOUT)',
                            'key/value pairs')

    def test_refused_key_after_allowed_pairs_is_caught(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES '
            'LABELS fast TIMEOUT 30 ENVIRONMENT "INPUT=fixture")', 'ENVIRONMENT')

    def test_quoted_and_bracketed_keys_cannot_hide_environment(self) -> None:
        for key in ('"environment"', '[=[Environment]=]'):
            with self.subTest(key=key):
                self.assert_refused(
                    f'set_tests_properties(test_lane_property PROPERTIES {key} "INPUT=fixture")',
                    'ENVIRONMENT')

    def test_unquoted_list_cannot_hide_environment(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES LABELS fast;ENVIRONMENT;INPUT=fixture)',
            'ENVIRONMENT')

    def test_bracketed_value_is_one_literal_argument(self) -> None:
        result = self.run_check('''set_tests_properties(test_lane_property PROPERTIES LABELS [=[
ENVIRONMENT # literal label text, with a ) and an (
set_target_properties(literal PROPERTIES FOO bar)
]=] TIMEOUT 30)''')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('CR-150 CMake lane directory is valid', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
