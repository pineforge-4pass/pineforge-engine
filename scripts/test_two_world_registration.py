#!/usr/bin/env python3
"""Controls for scripts/check_two_world_registration.py (the two-world check).

Checker controls run the REAL check script against a tamper tree: the worktree
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
 18. an undefined variable in if() is FALSE (a row behind it is not
     registered), and a quoted STREQUAL pair matches with its quotes stripped
 19. a math(EXPR) in a form the sweep cannot read -> 2, named
 20. the ctest row's TMPDIR shape (an untracked build dir inside the source
     tree) is a legal write area; tracked paths stay refused -> no raise / raise
 21. parenthesised boolean groups register exactly zero or one extra row;
     an unmodelled parenthesised predicate still exits 2, named
 22. get_test_property binds its output to the property-read NOTFOUND value;
     IN_LIST walks cleanly, and a registration behind that false value is absent
 23. a public lane row is evaluated in both worlds from a neutral cwd
 24. a pinned private lane row registered in only one world is drift
 25. a glob-fed file(STRINGS) reads relative paths against the tree root
 26. the lane include fixture excludes a fenced block after the include loop
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
from unittest import mock

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
LANE_FILE = 'tests/cr150.d/e2e-two-world-lane.cmake'
SHARD_ROW = 'confirmed_capabilities_refusals_pooc_5'
# A pinned private row that is no refusal shard: added to the list it is
# pinned and sorted, so only the drift guard can reject it.
NOT_A_SHARD_ROW = 'test_pooc_close_fill_view_tapes'


def lane_include_block(text: str) -> str:
    """Extract the unique cr150.d glob through its loop's matching end."""
    lines = text.splitlines(keepends=True)
    starts = [index for index, line in enumerate(lines)
              if re.match(r'\s*file\s*\(\s*GLOB\b', line, re.IGNORECASE)
              and '/cr150.d/*.cmake' in line]
    if len(starts) != 1:
        raise AssertionError(
            f'{TREES_DIR}: expected one cr150.d file(GLOB) statement, '
            f'found {len(starts)}')
    start = starts[0]
    depth = 0
    for index in range(start + 1, len(lines)):
        command = re.match(r'\s*(foreach|endforeach)\s*\(',
                           lines[index], re.IGNORECASE)
        if command is None:
            continue
        if command.group(1).lower() == 'foreach':
            depth += 1
        else:
            if depth == 0:
                raise AssertionError(
                    f'{TREES_DIR}:{index + 1}: endforeach() before the lane loop')
            depth -= 1
            if depth == 0:
                return ''.join(lines[start:index + 1])
    raise AssertionError(
        f'{TREES_DIR}:{start + 1}: no matching endforeach() for the lane include block')


class TamperTree:
    """The worktree by symlink plus the overlay, with real CMakeLists copies.

    Every directory a control can write into is REAL. A symlinked directory
    would carry the write through into the worktree -- a first version of this
    class did exactly that -- so `_mirror` builds real directories for the two
    private trees and `_write_target` refuses any path that resolves into the
    repository's own (tracked) content; the one in-tree path it lets through
    is the resolved TMPDIR's untracked build tree, the ctest row's shape."""

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
        """Refuse a write that could reach the worktree's own content.

        Two shapes are legal: a path outside the worktree, and a path under
        the resolved TMPDIR whose worktree-relative top-level name git does
        NOT track. The second is the CI shape this class must survive: the
        ctest row pins TMPDIR to ``<build>/tests/tmp`` (tests/CMakeLists.txt,
        ENVIRONMENT) and the build dir sits inside the source tree, so the
        temp area resolves into the worktree while being no part of it. Every
        other path into the worktree is refused -- tracked content above all,
        including a TMPDIR that points into a tracked top-level path.
        """
        resolved = path.resolve()
        if resolved != REPO and REPO not in resolved.parents:
            return
        tmpdir = _resolved_tmpdir()
        if tmpdir is not None and (resolved == tmpdir or tmpdir in resolved.parents):
            relative = resolved.relative_to(REPO)
            if relative.parts and relative.parts[0] not in _tracked_top_level():
                return
            reason = (f'the TMPDIR area ({tmpdir}) lies inside the tracked '
                      f'top-level path {relative.parts[0] + "/" if relative.parts else REPO}')
        elif tmpdir is None:
            reason = 'no TMPDIR is set, so it is no temporary area'
        else:
            reason = f'the TMPDIR area ({tmpdir}) is not one it lies under'
        raise AssertionError(
            f'{path} resolves into the worktree ({resolved}): refusing to '
            f'write there because {reason}')

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


_TRACKED_TOP_LEVEL: 'frozenset[str] | None' = None


def _tracked_top_level() -> frozenset[str]:
    """The worktree's tracked top-level names, read from git once.

    git is the only source of truth for what the worktree's own content is; a
    path inside REPO whose first component is one of these names is tree
    content (or a new file beside it), never a temporary area. When git cannot
    answer at all, every entry of REPO is treated as tracked, so the guard
    fails closed instead of opening the tree.
    """
    global _TRACKED_TOP_LEVEL
    if _TRACKED_TOP_LEVEL is None:
        result = subprocess.run(['git', '-C', str(REPO), 'ls-files', '-z'],
                                capture_output=True, text=True)
        if result.returncode != 0:
            names = {entry.name for entry in REPO.iterdir()}
        else:
            names = {path.split('/', 1)[0]
                     for path in result.stdout.split('\0') if path}
        _TRACKED_TOP_LEVEL = frozenset(names)
    return _TRACKED_TOP_LEVEL


def _resolved_tmpdir() -> 'Path | None':
    raw = os.environ.get('TMPDIR', '').strip()
    return Path(raw).resolve() if raw else None

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

    def run_check(self, *args: str, overlay: bool = False, cwd: Path | None = None):
        env = dict(os.environ)
        if overlay:
            env['PINEFORGE_PRIVATE_FIXTURES'] = '1'
        return subprocess.run(
            [PY, '-B', str(CHECK), '--root', str(self.root), *args],
            capture_output=True, text=True, env=env, cwd=cwd)

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
        # No construct is left unbound: a clean run prints no refusal line
        # (every refusal and failure line carries the script's own prefix).
        self.assertNotIn('check_two_world_registration:', result.stdout)

    def test_unit_test_helper_follows_argument_branches_and_literal_rows(self):
        path = self.tree._write_target('tests/helper-registration.cmake')
        path.write_text('''
function(pineforge_add_unit_test program)
    add_executable(${program} ${program}.cpp)
    set(helper_local changed)
    if(program STREQUAL "packed_program")
        add_test(NAME packed_program_slice_1 COMMAND ${program} --slice 1/2)
        add_test(NAME packed_program_slice_2 COMMAND ${program} --slice 2/2)
        add_test(NAME packed_program_cover COMMAND ${program} --list)
    else()
        add_test(NAME ${program} COMMAND ${program})
    endif()
endfunction()
set(program parent_value)
set(helper_local parent_value)
pineforge_add_unit_test(packed_program)
pineforge_add_unit_test(plain_program)
add_test(NAME literal_public_row COMMAND plain_program)
''')
        world = CHECK_MODULE.World(self.root, set(), set(), virtual=True)
        machine = CHECK_MODULE.Machine(
            path, world, CHECK_MODULE.PROFILES['release'], self.root)
        machine.run()
        self.assertEqual({
            'packed_program_slice_1', 'packed_program_slice_2',
            'packed_program_cover', 'plain_program', 'literal_public_row',
        }, machine.rows)
        self.assertEqual({'packed_program', 'plain_program'}, machine.targets)
        self.assertEqual(['parent_value'], machine.vars['program'])
        self.assertEqual(['parent_value'], machine.vars['helper_local'])

    def test_deleted_unit_slice_registration_names_the_missing_row(self):
        row = 'test_native_command_after_slice_4'
        registration = (f'        add_test(NAME {row}\n'
                        '            COMMAND ${test_name} --slice 4/8)\n')
        self.tree.edit(TREES_DIR, registration, '')
        result = self.run_check('--cmakelists', TREES_DIR, '--profile', 'debug')
        output = result.stdout + result.stderr
        self.assertEqual(2, result.returncode, output)
        self.assertIn(f'PR slow row is no longer registered: {row}', output)
        self.assertIn('message(FATAL_ERROR) was reached', output)
        self.assertNotIn('Traceback', output)

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
        self.assertNotIn('check_two_world_registration:', result.stdout)

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

    # control 12: the ctest row's TMPDIR shape is a legal temp area while
    # tracked content stays refused (N9). RED on the old guard: the first
    # _refuse_repo call below raised, which is exactly how the row failed in
    # every profile (the build dir sits inside the source tree in CI).
    def test_the_ctest_tmpdir_shape_is_legal_and_tracked_content_is_refused(self):
        with tempfile.TemporaryDirectory(prefix='build-e2e-two-world-',
                                         dir=REPO) as build:
            tmp = Path(build) / 'tests' / 'tmp'
            tmp.mkdir(parents=True)
            with mock.patch.dict(os.environ, {'TMPDIR': str(tmp)}):
                # The exact shape: <build>/tests/tmp/e2e-two-world-*/tree/tests.
                TamperTree._refuse_repo(tmp / 'e2e-two-world-abc' / 'tree' / 'tests')
                TamperTree._refuse_repo(tmp)
                for tracked in (REPO / 'tests', REPO / 'runner' / 'CMakeLists.txt',
                                REPO / 'tests' / 'tmp', REPO / 'build-e2e-two-world-x'):
                    with self.subTest(tracked=tracked.name), \
                            self.assertRaises(AssertionError):
                        TamperTree._refuse_repo(tracked)
            # A TMPDIR pointed into a tracked path opens nothing there.
            with mock.patch.dict(os.environ, {'TMPDIR': str(REPO / 'tests')}):
                with self.assertRaises(AssertionError):
                    TamperTree._refuse_repo(REPO / 'tests' / 'e2e-two-world-new')
        # No TMPDIR: no path into the worktree is a temporary area.
        with mock.patch.dict(os.environ, {'TMPDIR': ''}):
            with self.assertRaises(AssertionError):
                TamperTree._refuse_repo(REPO / 'build-e2e-two-world-x')

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

    # control 18: an undefined variable is FALSE in CMake (a row behind it is
    # not registered), and a quoted STREQUAL pair matches after the quotes
    # are stripped (CMake: a quoted argument is one argument, de-quoted)
    def test_conditions_follow_cmakes_rules(self):
        baseline = self.row_count(RUNNER)
        self.tree.append(RUNNER, '''
if(E2E_TWO_WORLD_UNBOUND_FLAG)
    add_test(NAME e2e_two_world_unbound_flag COMMAND ${CMAKE_COMMAND} -E echo ok)
endif()
''')
        # an undefined variable registers NOTHING (each half counted on its
        # own, so the two rules cannot cancel out)
        self.assertEqual(baseline, self.row_count(RUNNER))
        self.tree.append(RUNNER, '''
if("yes" STREQUAL "yes")
    add_test(NAME e2e_two_world_quoted_strequal COMMAND ${CMAKE_COMMAND} -E echo ok)
endif()
''')
        self.assertEqual(baseline + 1, self.row_count(RUNNER))

    # control 22: a property read overwrites its output, rather than ignoring
    # the command or binding the test/property argument. Like other property
    # reads, NOTFOUND is false and has no ordinary label in its list.
    def test_get_test_property_binds_unknown_values_and_controls_registration(self):
        baseline = self.row_count(RUNNER)
        self.tree.append(RUNNER, '''
add_test(NAME e2e_two_world_property_probe COMMAND ${CMAKE_COMMAND} -E echo ok)
foreach(_e2e_two_world_property_name IN ITEMS
        DISABLED SKIP_RETURN_CODE SKIP_REGULAR_EXPRESSION LABELS)
    set(_e2e_two_world_property_value e2e_stale_label)
    get_test_property(e2e_two_world_property_probe ${_e2e_two_world_property_name} _e2e_two_world_property_value)
    if(e2e_stale_label IN_LIST _e2e_two_world_property_value)
        message(FATAL_ERROR "get_test_property left its output unchanged")
    endif()
    if(_e2e_two_world_property_value OR NOT _e2e_two_world_property_value STREQUAL "NOTFOUND")
        message(FATAL_ERROR "get_test_property must use the property-read unknown value")
    endif()
endforeach()
''')
        self.assertEqual(baseline + 1, self.row_count(RUNNER))
        registration = ('add_test(NAME e2e_two_world_property_guarded '
                        'COMMAND ${CMAKE_COMMAND} -E echo ok)\n')
        self.tree.append(RUNNER, '\n' + registration)
        self.assertEqual(baseline + 2, self.row_count(RUNNER))
        for condition in ('_e2e_two_world_property_value',
                          'e2e_stale_label IN_LIST _e2e_two_world_property_value'):
            with self.subTest(condition=condition):
                guarded = f'if({condition})\n    {registration}endif()\n'
                self.tree.edit(RUNNER, registration, guarded)
                # Unknown property values follow the existing false-condition
                # rule: the guarded registration is not walked.
                self.assertEqual(baseline + 1, self.row_count(RUNNER))
                known = 'set(_e2e_two_world_property_value e2e_stale_label)\n' + guarded
                self.tree.edit(RUNNER, guarded, known)
                self.assertEqual(baseline + 2, self.row_count(RUNNER))
                self.tree.edit(RUNNER, known, registration)

    # control 19: a math(EXPR) form the sweep cannot read is exit 2, named
    def test_an_unreadable_math_expression_exits_2(self):
        self.tree.append(RUNNER, '''
math(EXPR _e2e_two_world_math "1 +")
''')
        result = self.run_check('--cmakelists', RUNNER, '--profile', 'release')
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('math(EXPR) in a form this sweep cannot read',
                      result.stdout + result.stderr)

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

    # control 21: reached parenthesised groups are FALSE or TRUE according to
    # their predicates; an unmodelled predicate stays exit 2, named
    def test_parenthesised_boolean_groups_and_unmodelled_predicates(self):
        # The checker reads tests/ for its records even with --cmakelists
        # runner/. Flatten its existing guard in the tamper copy only, so
        # 64137d89 reaches the synthetic group instead of failing the baseline.
        self.tree.edit(
            TREES_DIR,
            'if(PINEFORGE_BUILD_SOURCE_LAYER AND (NOT TEST ${_pf_fast_test} '
            'OR NOT TARGET ${_pf_fast_test}))',
            'if(PINEFORGE_BUILD_SOURCE_LAYER AND NOT TEST ${_pf_fast_test} '
            'OR PINEFORGE_BUILD_SOURCE_LAYER AND NOT TARGET ${_pf_fast_test})')
        baseline = self.row_count(RUNNER)
        self.tree.append(RUNNER, '''
if((TEST e2e_two_world_ct21_missing AND NOT TEST e2e_two_world_ct21_missing))
    add_test(NAME e2e_two_world_ct21_false COMMAND ${CMAKE_COMMAND} -E echo bad)
endif()
''')
        # Count the FALSE group before adding the TRUE group so they cannot
        # cancel each other's registration errors.
        self.assertEqual(baseline, self.row_count(RUNNER))
        self.tree.append(RUNNER, '''
if((NOT TEST e2e_two_world_ct21_missing OR NOT TARGET e2e_two_world_ct21_missing))
    add_test(NAME e2e_two_world_ct21_true COMMAND ${CMAKE_COMMAND} -E echo ok)
endif()
''')
        self.assertEqual(baseline + 1, self.row_count(RUNNER))
        # VERSION_LESS is not among _atom's modelled comparisons.
        self.tree.append(RUNNER, '''
if((1 VERSION_LESS 2))
    add_test(NAME e2e_two_world_ct21_unmodelled COMMAND ${CMAKE_COMMAND} -E echo bad)
endif()
''')
        result = self.run_check('--cmakelists', RUNNER, '--profile', 'release')
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn("unmodelled condition '1 VERSION_LESS 2'",
                      result.stdout + result.stderr)
        self.assertIn(RUNNER, result.stdout + result.stderr)

    def lane_tree(self, body: str) -> Path:
        """An isolated include probe over the copied tree and real pin set."""
        lanes = self.root / 'tests' / 'cr150.d'
        self.tree._refuse_repo(lanes.parent)
        self.assertTrue(lanes.is_symlink(), 'the copied lane directory changed shape')
        lanes.unlink()                       # the scratch-tree link, never its target
        shutil.copytree(REPO / 'tests' / 'cr150.d', lanes)
        self.tree.append(LANE_FILE, body)
        # Keep the real lane include block, with empty but fully modelled
        # records around it. Both probes use this same registration fixture.
        cmake = (REPO / TREES_DIR).read_text()
        block = lane_include_block(cmake)
        helper = re.search(
            r'(?ms)^function\(pineforge_add_unit_test\b.*?^endfunction\(\)',
            cmake)
        self.assertIsNotNone(helper, 'the unit-test registration helper is gone')
        self.tree._write_target(TREES_DIR).write_text(
            'set(_pf_source_bound_private_rows)\n' + helper.group(0) + '\n' + block)
        self.tree._write_target(RUNNER).write_text(
            'set(_pf_refusal_shard_rows)\n'
            'set(_pf_refusal_shard_first_library)\n'
            'list(POP_FRONT _pf_refusal_shard_first_library)\n'
            'set(_pf_confirmed_refusal_requests)\n'
            'set(_pf_confirmed_refusal_pooc)\n')
        # Exercise both worlds without requiring the private overlay: only
        # marker existence is consumed by this fixture, not its file contents.
        marker = self.root / CHECK_MODULE.OVERLAY_MARKER
        if not marker.exists():
            self.tree._refuse_repo(marker.parent)
            marker.parent.mkdir(parents=True, exist_ok=True)
            self.tree.append(str(CHECK_MODULE.OVERLAY_MARKER), '// registry probe marker\n')
        cwd = Path(self._tmp.name) / 'outside-tree'
        cwd.mkdir()
        self.assertNotEqual(cwd, self.root)
        return cwd

    def test_lane_include_block_ignores_a_following_fenced_block(self):
        cmake = (REPO / TREES_DIR).read_text()
        tail = (REPO / 'scripts' / 'fixtures' / 'cr150_cmake_d'
                / 'tail-l6b2.cmake').read_text()
        tail = tail.replace('core-cr150-l6b2', Path(LANE_FILE).stem)
        expected = (
            'file(GLOB CR150_LANE_CMAKE CONFIGURE_DEPENDS '
            '"${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake")\n'
            'list(SORT CR150_LANE_CMAKE)\n'
            'foreach(cr150_lane_file IN LISTS CR150_LANE_CMAKE)\n'
            '    include("${cr150_lane_file}")\n'
            'endforeach()\n')
        self.assertEqual(expected, lane_include_block(cmake + tail))

    def test_lane_public_row_is_read_from_a_different_cwd(self):
        row = 'e2e_two_world_lane_public'
        cwd = self.lane_tree(
            f'add_test(NAME {row} COMMAND ${{CMAKE_COMMAND}} -E echo public)\n')
        self.assertNotIn(row, CHECK_MODULE.load_pinned_rows(self.root))
        with mock.patch.dict(os.environ, {'PF_TWO_WORLD_SHOW_ROWS': '1'}):
            result = self.run_check('--both', '--cmakelists', TREES_DIR,
                                    '--profile', 'release', cwd=cwd)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for world in ('absent', 'overlay'):
            self.assertIn(f'{TREES_DIR} [release] {world}-world row: {row}', result.stdout)
        self.assertIn('absent-only names: 0', result.stdout)
        self.assertIn('overlay-only names: 0', result.stdout)

    def test_lane_pinned_private_row_is_drift_from_a_different_cwd(self):
        row = NOT_A_SHARD_ROW
        self.assertIn(row, CHECK_MODULE.load_pinned_rows(self.root))
        cwd = self.lane_tree(f'''if(EXISTS "${{PROJECT_SOURCE_DIR}}/{CHECK_MODULE.OVERLAY_MARKER}")
    add_test(NAME {row} COMMAND ${{CMAKE_COMMAND}} -E echo private)
endif()
''')
        result = self.run_check('--both', '--cmakelists', TREES_DIR,
                                '--profile', 'release', cwd=cwd)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f'{TREES_DIR} [release] registers {row} only with the overlay present',
                      result.stdout)
        self.assertIn('overlay-only names: 1', result.stdout)
        self.assertNotIn('FileNotFoundError', result.stdout + result.stderr)

    def test_glob_fed_strings_are_read_from_a_different_cwd(self):
        row = 'e2e_two_world_glob_strings'
        cwd = self.lane_tree('')
        directory = self.root / 'tests' / 'e2e-glob-strings'
        self.tree._refuse_repo(directory)
        directory.mkdir()
        self.tree._write_target('tests/e2e-glob-strings/rows.txt').write_text(row + '\n')
        self.tree.append(TREES_DIR, '''
file(GLOB _e2e_strings_files "${CMAKE_CURRENT_SOURCE_DIR}/e2e-glob-strings/*.txt")
list(GET _e2e_strings_files 0 _e2e_strings_file)
file(STRINGS ${_e2e_strings_file} _e2e_strings_rows)
foreach(_e2e_strings_row IN LISTS _e2e_strings_rows)
    add_test(NAME ${_e2e_strings_row} COMMAND ${CMAKE_COMMAND} -E echo public)
endforeach()
''')
        with mock.patch.dict(os.environ, {'PF_TWO_WORLD_SHOW_ROWS': '1'}):
            result = self.run_check('--both', '--cmakelists', TREES_DIR,
                                    '--profile', 'release', cwd=cwd)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for world in ('absent', 'overlay'):
            self.assertIn(f'{TREES_DIR} [release] {world}-world row: {row}', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
