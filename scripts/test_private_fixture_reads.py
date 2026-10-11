#!/usr/bin/env python3
"""Controls for the two class sweeps nothing else ran (R3-P1-1, R3-P1-2).

Every test runs the REAL scripts -- scripts/check_private_fixture_reads.py and
scripts/check_kernel_python_private_reads.py -- against a tampered copy of the
tree under a temporary root, or against the real tree. None of them
re-implements a sweep.

The tamper copy holds exactly what check_private_fixture_reads.py reads: the
two CMakeLists, the pinned removed-path set, the pinned row set, the evidence
file and the Python tape drivers. A control that needs a row's source subtree
adds it itself.

RED evidence for CI-visible regressions: set PF_SWEEP_SCRIPT to an older copy
of the sweep (e.g. `git show 6d7b2bf4e:scripts/check_private_fixture_reads.py`)
to watch every control below fail against it.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SWEEP = Path(__import__('os').environ.get(
    'PF_SWEEP_SCRIPT', REPO / 'scripts' / 'check_private_fixture_reads.py'))
KERNEL_SWEEP = REPO / 'scripts' / 'check_kernel_python_private_reads.py'
PY = sys.executable

REMOVED_EXAMPLE = 'tests/fixtures/whole_lot_tie'


def run_sweep(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [PY, '-B', str(SWEEP), '--root', str(root)],
        capture_output=True, text=True, timeout=600)


class TamperTree:
    """A minimal copy of the tree the private-fixture sweep reads."""

    def __init__(self, directory: Path):
        self.root = directory
        (self.root / 'tests').mkdir(parents=True)
        (self.root / 'runner').mkdir(parents=True)
        shutil.copy(REPO / 'tests' / 'CMakeLists.txt', self.root / 'tests')
        shutil.copy(REPO / 'runner' / 'CMakeLists.txt', self.root / 'runner')
        for name in ('private_fixture_paths.txt', 'private_fixture_rows.txt',
                     'private_fixture_reads.evidence'):
            shutil.copy(REPO / 'tests' / name, self.root / 'tests')
        for driver in sorted((REPO / 'tests').glob('*_tapes.py')):
            shutil.copy(driver, self.root / 'tests')

    def append(self, rel: str, text: str) -> None:
        with (self.root / rel).open('a') as handle:
            handle.write(text)

    def edit(self, rel: str, old: str, new: str) -> None:
        path = self.root / rel
        text = path.read_text()
        self.assertIn(old, text, rel)
        path.write_text(text.replace(old, new, 1))

    @staticmethod
    def assertIn(needle: str, haystack: str, where: str) -> None:
        if needle not in haystack:
            raise AssertionError(f'{where}: template text {needle!r} not found')


class PrivateFixtureReadsControls(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='e2e-fb2-')
        self.tree = TamperTree(Path(self._tmp.name) / 'tamper')
        self.addCleanup(self._tmp.cleanup)

    # control 0: the real tree in the absent world is clean
    def test_real_tree_is_clean(self):
        result = run_sweep(REPO)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('unpinned rows naming removed data: 0', result.stdout)
        self.assertIn('skip markers not naming a removed file: 0', result.stdout)

    # control 1: an unpinned multi-line add_test( NAME ... row naming removed
    # data
    def test_multiline_unpinned_row_is_caught(self):
        self.tree.append('tests/CMakeLists.txt', f'''
add_test(
    NAME test_e2e_fb2_multiline_row
    COMMAND ${{CMAKE_COMMAND}} -E echo {REMOVED_EXAMPLE}/bars.csv
)
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('test_e2e_fb2_multiline_row', result.stdout)

    # control 2: an unpinned runner row naming removed data through
    # ${PROJECT_SOURCE_DIR}
    def test_runner_row_via_project_source_dir_is_caught(self):
        self.tree.append('runner/CMakeLists.txt', f'''
add_test(NAME test_e2e_fb2_runner_row_proj
    COMMAND ${{Python3_EXECUTABLE}} ${{PROJECT_SOURCE_DIR}}/{REMOVED_EXAMPLE}/e2e.py)
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('test_e2e_fb2_runner_row_proj', result.stdout)

    # control 3: a row registered in the else() branch of an EXISTS guard
    def test_else_branch_row_is_caught(self):
        self.tree.append('tests/CMakeLists.txt', f'''
if(EXISTS "${{CMAKE_CURRENT_SOURCE_DIR}}/fixtures/whole_lot_tie")
    add_test(NAME e2e_fb2_guard_branch COMMAND ${{CMAKE_COMMAND}} -E echo present)
else()
    add_test(NAME test_e2e_fb2_else_branch_row
        COMMAND test_e2e_fb2_else_reader {REMOVED_EXAMPLE}/bars.csv)
endif()
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('test_e2e_fb2_else_branch_row', result.stdout)

    # control 4: a row under if(NOT EXISTS <removed>)
    def test_not_exists_row_is_caught(self):
        self.tree.append('tests/CMakeLists.txt', f'''
if(NOT EXISTS "${{CMAKE_CURRENT_SOURCE_DIR}}/fixtures/whole_lot_tie")

    add_test(NAME test_e2e_fb2_not_exists_row COMMAND test_e2e_fb2_not_exists_reader {REMOVED_EXAMPLE}/bars.csv)
endif()
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('test_e2e_fb2_not_exists_row', result.stdout)

    # control 5: an add_test argument naming an unset ${_pf_*} variable
    def test_unset_pf_variable_argument_exits_2(self):
        self.tree.append('tests/CMakeLists.txt', '''
add_test(NAME test_e2e_fb2_unset_pf_arg
    COMMAND ${CMAKE_COMMAND} -E echo ${_pf_fb2_never_set})
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('_pf_fb2_never_set', result.stdout + result.stderr)

    # control 6: a tape driver whose private marker takes another form
    def test_unreadable_driver_marker_exits_2(self):
        driver = self.tree.root / 'tests' / 'test_callback_lifecycle_tapes.py'
        self.assertTrue(driver.exists(), 'the tape driver template is gone')
        text = driver.read_text()
        marker_line = [line for line in text.splitlines()
                       if '_pf_private_marker' in line and '=' in line]
        self.assertTrue(marker_line, 'the driver template carries no marker')
        old = marker_line[0]
        driver.write_text(text.replace(
            old, '    _pf_private_marker = os.path.join(fixtures, "meta.json")', 1))
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('test_callback_lifecycle_tapes', result.stdout + result.stderr)

    # control 7: a pinned row no registration names (swept != registered)
    def test_pinned_row_without_registration_is_caught(self):
        self.tree.append('tests/private_fixture_rows.txt',
                         'e2e_fb2_pinned_row_that_registers_nowhere\n')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('e2e_fb2_pinned_row_that_registers_nowhere', result.stdout)


class KernelPythonSweepControls(unittest.TestCase):
    """Controls for check_kernel_python_private_reads.py: its population is a
    CMakeLists, so the tamper tree carries a minimal one plus the scripts."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='e2e-fb2-kernel-')
        self.root = Path(self._tmp.name) / 'tamper'
        (self.root / 'tests').mkdir(parents=True)
        (self.root / 'scripts').mkdir(parents=True)
        shutil.copy(KERNEL_SWEEP, self.root / 'scripts')
        self.script = self.root / 'scripts' / 'check_kernel_python_private_reads.py'
        self.cmake = self.root / 'tests' / 'CMakeLists.txt'
        self.addCleanup(self._tmp.cleanup)

    def run_sweep(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [PY, '-B', str(self.script), '--cmake', str(self.cmake)],
            capture_output=True, text=True, timeout=600)

    def test_python_row_reading_absent_private_file_is_caught(self):
        (self.root / 'tests' / 'e2e_fb2_reader.py').write_text(
            f'PATH = "{REMOVED_EXAMPLE}/bars.csv"\n'
            'def main():\n'
            '    return open(PATH).read()\n')
        self.cmake.write_text(
            'add_test(NAME e2e_fb2_python_reader\n'
            '    COMMAND ${Python3_EXECUTABLE} '
            '${PROJECT_SOURCE_DIR}/tests/e2e_fb2_reader.py)\n')
        result = self.run_sweep()
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('e2e_fb2_python_reader', result.stdout)

    def test_unparseable_python_row_exits_2(self):
        self.cmake.write_text(
            'add_test(NAME e2e_fb2_python_unparseable\n'
            '    COMMAND ${Python3_EXECUTABLE} -c "print(1)")\n')
        result = self.run_sweep()
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('names no .py script', result.stdout + result.stderr)

    def test_real_tree_is_clean(self):
        result = subprocess.run(
            [PY, '-B', str(KERNEL_SWEEP)], capture_output=True, text=True,
            timeout=600)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('kernel python rows reading an absent private file: 0',
                      result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
