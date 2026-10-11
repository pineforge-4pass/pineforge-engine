#!/usr/bin/env python3
"""Controls for scripts/check_two_world_registration.py (the two-world check).

Every control runs the REAL check script against a tamper tree: the worktree
by symlink, with the two CMakeLists as real copies a control edits, and -- when
the overlay is available (PF_PRIVATE_OVERLAY names the read-only private
clone, or the tree itself holds the overlay's marker file, which is the CI
overlay job's shape) -- the overlay's files linked into place, so the overlay
half has the world it needs. Where neither holds, the controls that need both
worlds skip themselves with the reason printed -- unless
PINEFORGE_PRIVATE_FIXTURES is set, which says the fixtures ARE in the tree, so
a control that cannot run is then a FAILURE, never a skip. Controls 2 and 7
fail through the absent half alone and carry no gate.

Controls:
  1. a recorded row removed from the record -> the drift guard names it, 1
  2. a name added to the record that is not source-bound -> 1 (no overlay)
  3. a row's absent-world placeholder deleted -> overlay-only names: 1 -> 1
  4. a construct the evaluator cannot read -> 2
  5. the tree as it is -> the absent world evaluated, 0, per-profile counts
  6. a refusal-shard row removed from the recorded list -> 1, named
  7. a refusal-shard name the split does not produce, unpinned -> 1 (no
     overlay)
  8. a pinned row that is not a refusal shard added to the list -> 1, named
  9. the tree as it is, overlay present -> 0, every profile 0/0, drift 0/0
 10. an execute_process the sweep does not bind -> 2, named
 11. a foreach over a parent list it cannot read -> 2, named
 12. a reached message(FATAL_ERROR) -> 2, naming file and line
 13. a two-pattern file(GLOB) -> both patterns matched (a row appears only
     when the evaluator sees both files)
 14. a file(GLOB_RECURSE) -> the subdirectory is traversed (a row appears
     only when the nested file is seen)
 15. a glob form the sweep does not model (FOLLOW_SYMLINKS) -> 2, named
 16. the profile table is ci_verify's own; a copy with a profile removed from
     the table is detected by the comparison
 17. two first-library markers swapped -> the positional pairing is drift -> 1
"""
from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CHECK = REPO / 'scripts' / 'check_two_world_registration.py'
PY = sys.executable
OVERLAY = os.environ.get('PF_PRIVATE_OVERLAY')


def _load_check_module():
    """The real check script as a module: its marker path and its tables are
    read from the script itself, never copied into this file."""
    spec = importlib.util.spec_from_file_location('e2e_two_world_check', CHECK)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CHECK_MODULE = _load_check_module()
OVERLAY_MARKER = REPO / CHECK_MODULE.OVERLAY_MARKER
# A pinned row whose public TU the removals take: without its own record entry
# the layer-off world walks no TU and the worlds' registrations differ.
REMOVED_TU_ROW = 'test_trail_activation_tick_reach'
TREES_DIR = 'tests/CMakeLists.txt'
RUNNER = 'runner/CMakeLists.txt'
SHARD_ROW = 'confirmed_capabilities_refusals_pooc_5'
# A pinned private row that is no refusal shard: added to the list it is
# pinned and sorted, so only the drift guard can reject it.
NOT_A_SHARD_ROW = 'test_pooc_close_fill_view_tapes'


class TamperTree:
    """The worktree by symlink plus the overlay, with real CMakeLists copies.

    Every directory a control can write into is REAL. A symlinked directory
    would carry the write through into the worktree -- a first version of this
    class did exactly that -- so `_mirror` builds real directories for the two
    private trees and `_write_target` refuses any path that resolves into the
    repository."""

    def __init__(self, dest: Path):
        self.root = dest
        dest.mkdir(parents=True)
        for entry in sorted(REPO.iterdir()):
            if entry.name in ('.git', 'build'):
                continue
            link = dest / entry.name
            if entry.name in ('tests', 'runner'):
                link.mkdir()
                for sub in sorted(entry.iterdir()):
                    if sub.name in ('fixtures', 'oracle') and sub.is_dir():
                        self._mirror(sub, link / sub.name)
                    else:
                        (link / sub.name).symlink_to(sub)
            else:
                link.symlink_to(entry)
        if OVERLAY:
            self._overlay(Path(OVERLAY), dest)
        for rel in (TREES_DIR, 'runner/CMakeLists.txt'):
            copy = self.root / rel
            self._refuse_repo(copy.parent)
            if copy.is_symlink() or copy.exists():
                copy.unlink()
            shutil.copy(REPO / rel, copy)

    @staticmethod
    def _mirror(src: Path, dst: Path) -> None:
        """A real directory tree whose files are links to `src`'s."""
        dst.mkdir(parents=True, exist_ok=True)
        for entry in sorted(src.iterdir()):
            if entry.is_dir():
                TamperTree._mirror(entry, dst / entry.name)
            elif entry.is_file():
                (dst / entry.name).symlink_to(entry)

    @staticmethod
    def _refuse_repo(path: Path) -> None:
        resolved = path.resolve()
        if resolved == REPO or REPO in resolved.parents:
            raise AssertionError(
                f'{path} resolves into the worktree ({resolved}): refusing to '
                f'write through a symlink')

    def _write_target(self, rel: str) -> Path:
        path = self.root / rel
        self._refuse_repo(path.parent)
        if path.is_symlink():
            raise AssertionError(
                f'{rel} is a symlink: refusing to write through it')
        return path

    @classmethod
    def _overlay(cls, overlay: Path, dest: Path) -> None:
        for root, _dirs, files in os.walk(overlay):
            rel = Path(root).relative_to(overlay)
            if rel.parts[:1] != ('tests',):
                continue
            for name in files:
                link = dest / rel / name
                link.parent.mkdir(parents=True, exist_ok=True)
                cls._refuse_repo(link.parent)
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(Path(root) / name)

    def edit(self, rel: str, old: str, new: str) -> None:
        path = self._write_target(rel)
        text = path.read_text()
        if old not in text:
            raise AssertionError(f'{rel}: template text {old!r} not found')
        path.write_text(text.replace(old, new, 1))

    def append(self, rel: str, text: str) -> None:
        with self._write_target(rel).open('a') as handle:
            handle.write(text)


def overlay_available() -> bool:
    """The overlay this control set needs: PF_PRIVATE_OVERLAY names the
    read-only clone for local runs, or the tree itself holds the overlay's
    marker file (the CI overlay jobs put the fixtures in the tree)."""
    return bool(OVERLAY) or OVERLAY_MARKER.is_file()


def needs_overlay(test):
    def wrapped(self):
        if not overlay_available():
            if os.environ.get('PINEFORGE_PRIVATE_FIXTURES', '').strip():
                self.fail(
                    'PINEFORGE_PRIVATE_FIXTURES is set but the overlay is not '
                    f'in the tree ({CHECK_MODULE.OVERLAY_MARKER} absent, '
                    'PF_PRIVATE_OVERLAY unset): this control cannot run, and '
                    'an overlay run that cannot run its overlay controls is a '
                    'failure, never a skip')
            self.skipTest(
                'no overlay to link in (PF_PRIVATE_OVERLAY unset and '
                f'{CHECK_MODULE.OVERLAY_MARKER} not in the tree): this '
                'control needs both worlds')
        return test(self)
    return wrapped


class TwoWorldControls(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='e2e-two-world-')
        self.root = Path(self._tmp.name) / 'tree'
        self.tree = TamperTree(self.root)
        self.addCleanup(self._tmp.cleanup)

    def run_check(self, *args: str, overlay: bool = False):
        env = dict(os.environ)
        if overlay:
            env['PINEFORGE_PRIVATE_FIXTURES'] = '1'
        return subprocess.run(
            [PY, '-B', str(CHECK), '--root', str(self.root), *args],
            capture_output=True, text=True, timeout=1800, env=env)

    # control 1: the drift guard names a recorded row whose record was removed
    @needs_overlay
    def test_a_removed_record_entry_is_drift(self):
        path = self.root / TREES_DIR
        text = path.read_text()
        row_line = f'    {REMOVED_TU_ROW}\n'
        self.assertIn(row_line, text, 'the recorded list changed shape')
        self.tree.edit(TREES_DIR, row_line, '')
        result = self.run_check('--overlay', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(REMOVED_TU_ROW, result.stdout)
        self.assertIn('the record must name it', result.stdout)

    # control 2: a name in the record that is not source-bound fails the guard.
    # Ungated: the record is checked by the absent half alone, so this needs no
    # overlay (it must run in every CI job).
    def test_a_non_source_bound_record_entry_is_drift(self):
        anchor = 'set(_pf_source_bound_private_rows\n'
        self.assertIn(anchor, (self.root / TREES_DIR).read_text(),
                      'the recorded list changed shape')
        self.tree.edit(TREES_DIR, anchor,
                       anchor + '    e2e_two_world_not_bound\n')
        result = self.run_check()
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('e2e_two_world_not_bound', result.stdout)
        self.assertIn('is not a pinned row', result.stdout)

    # control 3: a row registered in the overlay world only is drift (a
    # registration whose absent-world half is missing, the else-branch
    # placeholder removed)
    @needs_overlay
    def test_a_one_world_row_is_drift(self):
        self.tree.append(TREES_DIR, '''
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/whole_lot_tie/bars.inc")
    add_test(NAME e2e_two_world_one_world COMMAND ${CMAKE_COMMAND} -E echo present)
endif()
''')
        result = self.run_check('--overlay', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('overlay-only names: 1', result.stdout)
        self.assertIn('e2e_two_world_one_world', result.stdout)

    # control 4: a construct the evaluator cannot read is exit 2, never a skip
    def test_an_unreadable_construct_exits_2(self):
        self.tree.append(TREES_DIR,
                         '\nlist(TRANSFORM e2e_two_world REPLACE "a" "b")\n')
        result = self.run_check()
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('unmodelled list()', result.stdout + result.stderr)

    # control 5: the tree as it is evaluates the absent world, exit 0
    def test_the_tree_as_is_is_clean(self):
        result = self.run_check()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('tests/CMakeLists.txt [release] absent-world rows:', result.stdout)
        self.assertIn('runner/CMakeLists.txt [kernel] absent-world rows:',
                      result.stdout)
        self.assertIn('the recorded list is pinned, sorted and duplicate-free',
                      result.stdout)
        self.assertIn('the recorded refusal-shard rows: 12; not a pinned row: '
                      '0; unsorted: 0; duplicated: 0', result.stdout)
        # No construct is left unbound: the sweep either reads it or exits 2.
        self.assertNotIn('could not bind', result.stdout)

    # control 6: a refusal-shard row removed from the recorded list is drift
    @needs_overlay
    def test_a_removed_shard_row_is_drift(self):
        row_line = f'            {SHARD_ROW}\n'
        self.assertIn(row_line, (self.root / RUNNER).read_text(),
                      'the recorded shard list changed shape')
        self.tree.edit(RUNNER, row_line, '')
        result = self.run_check('--overlay', '--cmakelists', RUNNER,
                                '--profile', 'release', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(SHARD_ROW, result.stdout)
        self.assertIn('derived-only: 1', result.stdout)

    # control 7: a name the split does not produce, and the pin does not name.
    # Ungated: the recorded shard list is checked by the absent half alone, so
    # this needs no overlay (it must run in every CI job).
    def test_an_unpinned_shard_row_is_drift(self):
        tail = ('            confirmed_capabilities_refusals_requests_3\n'
                '        )')
        self.assertIn(tail, (self.root / RUNNER).read_text(),
                      'the recorded shard list changed shape')
        self.tree.edit(RUNNER, tail,
                       '            confirmed_capabilities_refusals_requests_3\n'
                       '            confirmed_capabilities_refusals_requests_9\n'
                       '        )')
        result = self.run_check('--cmakelists', RUNNER,
                                '--profile', 'release')
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('confirmed_capabilities_refusals_requests_9', result.stdout)
        self.assertIn('is not a pinned row', result.stdout)

    # control 8: a pinned row that is not a refusal shard, added to the list
    # with a marker of its own (the positional pairing stays complete):
    # pinned, sorted and duplicate-free, so only the derived side rejects it
    @needs_overlay
    def test_a_pinned_non_shard_row_is_drift(self):
        tail = ('            confirmed_capabilities_refusals_requests_3\n'
                '        )')
        self.assertIn(tail, (self.root / RUNNER).read_text(),
                      'the recorded shard list changed shape')
        self.tree.edit(RUNNER, tail,
                       '            confirmed_capabilities_refusals_requests_3\n'
                       f'            {NOT_A_SHARD_ROW}\n'
                       '        )')
        marker_tail = ('            refused_clock_W\n'
                       '        )')
        self.assertIn(marker_tail, (self.root / RUNNER).read_text(),
                      'the marker list changed shape')
        self.tree.edit(RUNNER, marker_tail,
                       '            refused_clock_W\n'
                       '            pooc_close_fill_view\n'
                       '        )')
        result = self.run_check('--overlay', '--cmakelists', RUNNER,
                                '--profile', 'release', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(NOT_A_SHARD_ROW, result.stdout)
        self.assertIn('recorded-only: 1', result.stdout)

    # control 9: the tree as it is, overlay present: both worlds and the split
    # over the live tree agree, in every profile ci_verify runs
    @needs_overlay
    def test_the_tree_as_is_matches_in_both_worlds(self):
        import ci_verify
        result = self.run_check('--overlay', '--cmakelists', RUNNER, overlay=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for profile in ci_verify.PROFILES:
            self.assertIn(f'runner/CMakeLists.txt [{profile}] absent-only names: 0',
                          result.stdout)
            self.assertIn(f'runner/CMakeLists.txt [{profile}] overlay-only names: 0',
                          result.stdout)
        self.assertIn('refusal-shard drift: recorded-only: 0, derived-only: 0',
                      result.stdout)
        self.assertNotIn('could not bind', result.stdout)

    # control 10: an execute_process the sweep does not bind is exit 2, never
    # a quiet line in a passing run
    def test_an_unbound_probe_exits_2(self):
        self.tree.append(RUNNER, '''

execute_process(COMMAND echo hi
    OUTPUT_VARIABLE _e2e_two_world_probe)
''')
        result = self.run_check('--cmakelists', RUNNER, '--profile', 'release')
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('execute_process in a shape this sweep does not bind',
                      result.stdout + result.stderr)
        self.assertIn(RUNNER, result.stdout + result.stderr)

    # control 11: a foreach over a parent list the sweep cannot read is exit 2
    def test_an_unreadable_parent_list_exits_2(self):
        self.tree.append(RUNNER, '''

foreach(_e2e_two_world_item IN LISTS E2E_TWO_WORLD_PARENT_LIST)
    set(_e2e_two_world_seen 1)
endforeach()
''')
        result = self.run_check('--cmakelists', RUNNER, '--profile', 'release')
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('E2E_TWO_WORLD_PARENT_LIST', result.stdout + result.stderr)

    def row_count(self, rel: str) -> int:
        """The absent-world row count the sweep prints for one file."""
        result = self.run_check('--cmakelists', rel, '--profile', 'release')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        match = re.search(r'\[release\] absent-world rows: (\d+)', result.stdout)
        if match is None:
            raise AssertionError(f'no row count in:\n{result.stdout}')
        return int(match.group(1))

    # control 12: a reached message(FATAL_ERROR) stops a real configure; the
    # sweep must say so (exit 2, file:line), never walk past it
    def test_a_reached_fatal_error_exits_2(self):
        self.tree.append(TREES_DIR, '''
message(FATAL_ERROR "e2e-two-world: the tampered tree reached a fatal error")
''')
        result = self.run_check('--cmakelists', TREES_DIR, '--profile', 'release')
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('message(FATAL_ERROR) was reached',
                      result.stdout + result.stderr)
        self.assertIn(TREES_DIR, result.stdout + result.stderr)

    # control 13: a two-pattern glob; the registered row appears only when
    # BOTH patterns are matched (one file each, in two directories)
    def test_a_two_pattern_glob_matches_both_patterns(self):
        for name in ('e2e-two-world-a', 'e2e-two-world-b'):
            directory = self.root / 'tests' / name
            directory.mkdir()
            (directory / 'present.txt').write_text('e2e-two-world\n')
        baseline = self.row_count(TREES_DIR)
        self.tree.append(TREES_DIR, '''
file(GLOB _e2e_two_world_two_patterns
     "${CMAKE_CURRENT_SOURCE_DIR}/e2e-two-world-a/*.txt"
     "${CMAKE_CURRENT_SOURCE_DIR}/e2e-two-world-b/*.txt")
list(LENGTH _e2e_two_world_two_patterns _e2e_two_world_two_count)
if(_e2e_two_world_two_count EQUAL 2)
    add_test(NAME e2e_two_world_two_pattern_glob COMMAND ${CMAKE_COMMAND} -E echo ok)
endif()
''')
        self.assertEqual(baseline + 1, self.row_count(TREES_DIR))

    # control 14: a recursive glob; the registered row appears only when the
    # nested file (one directory deeper) is matched too
    def test_a_recursive_glob_traverses_subdirectories(self):
        base = self.root / 'tests' / 'e2e-two-world-r'
        (base / 'sub').mkdir(parents=True)
        (base / 'present.txt').write_text('e2e-two-world\n')
        (base / 'sub' / 'nested.txt').write_text('e2e-two-world\n')
        baseline = self.row_count(TREES_DIR)
        self.tree.append(TREES_DIR, '''
file(GLOB_RECURSE _e2e_two_world_recursive
     "${CMAKE_CURRENT_SOURCE_DIR}/e2e-two-world-r/*.txt")
list(LENGTH _e2e_two_world_recursive _e2e_two_world_recursive_count)
if(_e2e_two_world_recursive_count EQUAL 2)
    add_test(NAME e2e_two_world_recursive_glob COMMAND ${CMAKE_COMMAND} -E echo ok)
endif()
''')
        self.assertEqual(baseline + 1, self.row_count(TREES_DIR))

    # control 15: a glob form the sweep does not model is exit 2, named
    def test_an_unmodelled_glob_form_exits_2(self):
        self.tree.append(RUNNER, '''
file(GLOB_RECURSE _e2e_two_world_symlinked FOLLOW_SYMLINKS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.py")
''')
        result = self.run_check('--cmakelists', RUNNER, '--profile', 'release')
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('FOLLOW_SYMLINKS', result.stdout + result.stderr)
        self.assertIn(RUNNER, result.stdout + result.stderr)

    # control 17: two first-library markers swapped. Both still name removed
    # files, so only the derived side (the real split's own first library per
    # row) can catch the mis-paired markers
    @needs_overlay
    def test_a_swapped_marker_pair_is_drift(self):
        head = ('            htf15_sma_pooc\n'
                '            pooc_close_short\n')
        self.assertIn(head, (self.root / RUNNER).read_text(),
                      'the marker list changed shape')
        self.tree.edit(RUNNER, head,
                       '            pooc_close_short\n'
                       '            htf15_sma_pooc\n')
        result = self.run_check('--overlay', '--cmakelists', RUNNER,
                                '--profile', 'release', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('confirmed_capabilities_refusals_pooc_1', result.stdout)
        self.assertIn('confirmed_capabilities_refusals_pooc_2', result.stdout)
        self.assertIn('mismatched: 2', result.stdout)

    # the profile table is scripts/ci_verify.py's own: the compared profiles
    # are printed for every ci_verify profile, the per-profile flags agree
    # with its entries, and a copy with a profile removed from the table is
    # detected by this comparison
    def test_the_profile_table_is_derived_from_ci_verify(self):
        import ci_verify
        self.assertEqual(set(ci_verify.PROFILES), set(CHECK_MODULE.PROFILES))
        for name in ci_verify.PROFILES:
            entry = ci_verify.PROFILE[name]
            table = CHECK_MODULE.PROFILES[name]
            self.assertEqual(entry.build_type, table['CMAKE_BUILD_TYPE'], name)
            self.assertEqual('ON' if entry.source_layer else 'OFF',
                             table['PINEFORGE_BUILD_SOURCE_LAYER'], name)
            self.assertEqual('ON' if entry.live_runner else 'OFF',
                             table['PINEFORGE_BUILD_LIVE_RUNNER'], name)
            self.assertEqual('ON' if entry.sanitizers else 'OFF',
                             table['PINEFORGE_ENABLE_SANITIZERS'], name)
        result = self.run_check('--cmakelists', RUNNER)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for name in ci_verify.PROFILES:
            self.assertIn(f'runner/CMakeLists.txt [{name}] absent-world rows:',
                          result.stdout)
        with tempfile.TemporaryDirectory() as temporary:
            tampered = Path(temporary) / 'check_two_world_registration.py'
            tampered.write_text(CHECK.read_text()
                                + "\nPROFILES.pop('native', None)\n")
            spec = importlib.util.spec_from_file_location(
                'e2e_two_world_check_tampered', tampered)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            self.assertEqual(set(ci_verify.PROFILES) - {'native'},
                             set(module.PROFILES),
                             'the tampered copy must lose exactly one profile')
            self.assertNotEqual(set(ci_verify.PROFILES), set(module.PROFILES),
                                'a table an entry short must not compare equal')


if __name__ == '__main__':
    unittest.main(verbosity=2)
