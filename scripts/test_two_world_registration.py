#!/usr/bin/env python3
"""Controls for scripts/check_two_world_registration.py (the two-world check).

Every control runs the REAL check script against a tamper tree: the worktree
by symlink, with the two CMakeLists as real copies a control edits, and -- when
PF_PRIVATE_OVERLAY names the read-only private overlay (the pf-private clone) --
the overlay's files linked into place, so the overlay half has the world it
needs. The overlay is a CI input, not a repo path, so without that variable the
three controls that need both worlds skip themselves with the reason printed.

Controls:
  1. a recorded row removed from the record -> the drift guard names it, 1
  2. a name added to the record that is not source-bound -> 1
  3. a row's absent-world placeholder deleted -> overlay-only names: 1 -> 1
  4. a construct the evaluator cannot read -> 2
  5. the tree as it is -> the absent world evaluated, 0, per-profile counts
  6. a refusal-shard row removed from the recorded list -> 1, named
  7. a refusal-shard name the split does not produce, unpinned -> 1, named
  8. a pinned row that is not a refusal shard added to the list -> 1, named
  9. the tree as it is, overlay present -> 0, every profile 0/0, drift 0/0
"""
from __future__ import annotations

import os
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


def needs_overlay(test):
    def wrapped(self):
        if not OVERLAY:
            self.skipTest('PF_PRIVATE_OVERLAY is not set: no overlay tree to '
                          'link in, and this control needs both worlds')
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

    # control 2: a name in the record that is not source-bound fails the guard
    @needs_overlay
    def test_a_non_source_bound_record_entry_is_drift(self):
        anchor = 'set(_pf_source_bound_private_rows\n'
        self.assertIn(anchor, (self.root / TREES_DIR).read_text(),
                      'the recorded list changed shape')
        self.tree.edit(TREES_DIR, anchor,
                       anchor + '    e2e_two_world_not_bound\n')
        result = self.run_check('--overlay', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('e2e_two_world_not_bound', result.stdout)

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
        self.assertIn('runner/CMakeLists.txt [kernel-live] absent-world rows:',
                      result.stdout)
        self.assertIn('the recorded list is pinned, sorted and duplicate-free',
                      result.stdout)
        self.assertIn('the recorded refusal-shard rows: 12; not a pinned row: '
                      '0; unsorted: 0; duplicated: 0', result.stdout)

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

    # control 7: a name the split does not produce, and the pin does not name
    @needs_overlay
    def test_an_unpinned_shard_row_is_drift(self):
        tail = ('            confirmed_capabilities_refusals_requests_3\n'
                '        )')
        self.assertIn(tail, (self.root / RUNNER).read_text(),
                      'the recorded shard list changed shape')
        self.tree.edit(RUNNER, tail,
                       '            confirmed_capabilities_refusals_requests_3\n'
                       '            confirmed_capabilities_refusals_requests_9\n'
                       '        )')
        result = self.run_check('--overlay', '--cmakelists', RUNNER,
                                '--profile', 'release', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('confirmed_capabilities_refusals_requests_9', result.stdout)
        self.assertIn('is not a pinned row', result.stdout)

    # control 8: a pinned row that is not a refusal shard, added to the list:
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
        result = self.run_check('--overlay', '--cmakelists', RUNNER,
                                '--profile', 'release', overlay=True)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(NOT_A_SHARD_ROW, result.stdout)
        self.assertIn('recorded-only: 1', result.stdout)

    # control 9: the tree as it is, overlay present: both worlds and the split
    # over the live tree agree, in every profile
    @needs_overlay
    def test_the_tree_as_is_matches_in_both_worlds(self):
        result = self.run_check('--overlay', '--cmakelists', RUNNER, overlay=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for profile in ('release', 'debug', 'sanitizers', 'kernel-live'):
            self.assertIn(f'runner/CMakeLists.txt [{profile}] absent-only names: 0',
                          result.stdout)
            self.assertIn(f'runner/CMakeLists.txt [{profile}] overlay-only names: 0',
                          result.stdout)
        self.assertIn('refusal-shard drift: recorded-only: 0, derived-only: 0',
                      result.stdout)
        self.assertNotIn('could not bind', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
