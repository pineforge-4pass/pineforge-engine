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
SWEEP = Path(os.environ.get(
    'PF_SWEEP_SCRIPT', REPO / 'scripts' / 'check_private_fixture_reads.py'))
KERNEL_SWEEP = REPO / 'scripts' / 'check_kernel_python_private_reads.py'
PY = sys.executable

REMOVED_EXAMPLE = 'tests/fixtures/whole_lot_tie'
EQUIVALENCE_PARENT = 'native_live_equivalence_e2e'
EQUIVALENCE_PROBE = 'ta-rsi14-cross-50-01'

# ---- the audit-hook trace the kernel sweep's exemptions must earn ----------
# scripts/check_kernel_python_private_reads.py exempts a row when the absent
# private path its script names is a payload rather than a read.  That claim
# is re-earned here, not trusted: the exempt script is run under an
# "open" audit hook that records every path under the real tests/fixtures/ or
# tests/oracle/ and exits 3 when there is one.
#
# The hook is installed by the TRACER file below, which plays two roles: as
# __main__ it runs one target script and reports the log, and as the
# sitecustomize every child interpreter picks up from PYTHONPATH it installs
# the same hook in those children, because a child's opens are invisible to
# its parent's hook.  Coverage is therefore the script's own process plus
# every Python child that inherits the environment; a child run with -I and a
# scrubbed environment (not this suite's) is out of any in-process hook's
# reach, and the exemption reason says so.
#
# The trace runs the target the way its own ctest row does: the row's argv,
# and the script's directory first on sys.path exactly as `python <script>`
# would put it (runpy.run_path does not, and the two-world row stopped at
# `import ci_verify` under the old tracer, so its exemption was traced
# vacuously).  A script that raises instead of exiting is a FAILURE of the
# trace, never a clean log: its exit code is 4, distinct from the 3 of an
# open that named a missing private path.
TRACE_MARKER = 'PF_PRIVATE_TRACE_CHILD'

TRACER_SOURCE = '''\
"""Record every open() of a file the real private trees do NOT have.

Imported as sitecustomize (PYTHONPATH), the hook below installs itself in
this interpreter; run as __main__, this file runs one target script and
reports the log its processes wrote (PF_PRIVATE_TRACE_LOG), exiting 3 when an
open named a missing path under PF_PRIVATE_TRACE_ROOT/tests/fixtures/ or
.../tests/oracle/ -- a read of a KEPT file there is not what the exemption is
about.

The target runs the way `python <script> [args...]` would: its own directory
first on sys.path (runpy.run_path does not do that for a script FILE) and its
argv the caller gave.  The trace prints the script's own exit code, and a
script that RAISES instead is a failure of the trace -- exit 4, distinct from
the 3 of an open -- because a script that dies before its work opens nothing
and its clean log proves nothing.
"""
import os
import runpy
import sys

_PREFIXES = tuple(
    os.path.join(os.environ.get('PF_PRIVATE_TRACE_ROOT', ''), tree) + os.sep
    for tree in ('tests/fixtures', 'tests/oracle'))


def _record(path):
    log = os.environ.get('PF_PRIVATE_TRACE_LOG')
    if not log:
        return
    try:
        with open(log, 'a') as handle:
            handle.write(path + "\\n")
    except OSError:
        pass


def _hook(event, args):
    if event != 'open' or not _PREFIXES[0] or not _PREFIXES[0].startswith(os.sep):
        return
    path = args[0]
    if not isinstance(path, str):
        try:
            path = os.fspath(path)
        except TypeError:
            return
    if not isinstance(path, str):
        return
    full = os.path.abspath(path)
    if any(full.startswith(prefix) for prefix in _PREFIXES):
        # The exemption is about files the tree does NOT have: a read of a
        # KEPT file under a private tree is what the checkers do on purpose,
        # and copying tests/oracle/ for a control reads them all.
        if not os.path.exists(full):
            _record(full)


sys.addaudithook(_hook)

if __name__ == '__main__':
    target = sys.argv[2]
    os.environ['PF_PRIVATE_TRACE_CHILD'] = '1'
    sys.argv = [target] + sys.argv[3:]
    # `python <script>` puts the script's own directory first on sys.path;
    # runpy.run_path does NOT (it touches sys.path only for a directory or
    # zip target, never for a script file).  Without this the target's own
    # imports fail, the script dies before doing its work, and the trace is
    # vacuously clean -- which is how the two-world row's exemption passed.
    sys.path.insert(0, os.path.dirname(os.path.abspath(target)))
    exited = 0
    raised = None
    try:
        runpy.run_path(target, run_name='__main__')
    except SystemExit as exit_error:
        # The scripts end by exit codes; the code is reported, not judged
        # here -- the control compares it to the code the script is known
        # to produce in this tree.
        code = exit_error.code
        if code is None:
            exited = 0
        elif isinstance(code, int):
            exited = code
        else:
            print('trace: the traced script exited with the message %r' % (code,))
            exited = 1
    except BaseException as error:
        raised = error
    try:
        with open(os.environ['PF_PRIVATE_TRACE_LOG']) as handle:
            opened = sorted({line.strip() for line in handle if line.strip()})
    except OSError:
        opened = []
    for path in opened:
        print('trace: opened %s' % (path,))
    print('trace: opens under the real private trees: %d' % len(opened))
    if raised is not None:
        # The script died before finishing: no clean log can clear it, so
        # the trace fails with a code distinct from the 3 of an open.
        print('trace: the traced script raised %r' % (raised,))
        print('trace: the traced script did not run to the end')
        sys.exit(4)
    print('trace: the traced script exited %d' % exited)
    sys.exit(3 if opened else 0)
'''


def run_under_trace(root: Path, tmp: Path, target: Path, argv: tuple = ()):
    """Run one script under the audit-hook trace, children included, from a
    neutral cwd (the scratch dir): `argv` is the extra argv the script's own
    ctest row passes, so the trace reproduces the row's invocation, and a
    script that needs the repo as its cwd is caught here rather than passing
    on the runner's."""
    tracer = tmp / 'sitecustomize.py'
    tracer.write_text(TRACER_SOURCE)
    log = tmp / 'opens.log'
    log.unlink(missing_ok=True)
    env = dict(os.environ)
    env['PYTHONPATH'] = os.pathsep.join(
        [str(tmp)] + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
    env['PF_PRIVATE_TRACE_ROOT'] = str(root)
    env['PF_PRIVATE_TRACE_LOG'] = str(log)
    env[TRACE_MARKER] = '1'
    return subprocess.run(
        [PY, '-B', str(tracer), str(root), str(target), *argv],
        capture_output=True, text=True, env=env, cwd=str(tmp))


# TRACER_SOURCE's own exit codes: 3 names an open of a missing private path,
# 4 a traced script that raised -- a script that died before its work opened
# nothing, so a trace there is a failure of the trace, never a clean result.
TRACE_PRIVATE_OPEN_EXIT = 3
TRACE_RAISED_EXIT = 4


def row_argv(kernel, row: str) -> tuple:
    """The extra argv tests/CMakeLists.txt passes the row's script.

    The trace reproduces the row's own invocation, never a bare run: the
    two-world row registers `check_two_world_registration.py --both`, and the
    ROW is what an exemption is about.  Read from the same file the checker
    parses (its own statement split), so a registration that grows an
    argument is followed here without a second list to keep in step."""
    text = (REPO / 'tests' / 'CMakeLists.txt').read_text()
    for _line, statement in kernel.statements(text):
        head = kernel.CMAKE_HEAD.match(statement)
        if not head or head.group(1) != 'add_test':
            continue
        name = kernel.ADD_TEST.search(statement)
        if not name or name.group(1) != row:
            continue
        match = re.search(r'[\w${}/.-]+\.py\b', statement)
        if match is None:
            raise AssertionError(f'{row}: the registration names no .py script')
        argv = []
        for token in statement[match.end():].split():
            token = token.rstrip(')').strip('"')  # the closing paren rides it
            if not token:
                continue
            if '${' in token:
                raise AssertionError(
                    f'{row}: unexpanded registration argument {token!r}')
            argv.append(token)
        return tuple(argv)
    raise AssertionError(f'{row}: no add_test() registration names it')


def run_sweep(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [PY, '-B', str(SWEEP), '--root', str(root)],
        capture_output=True, text=True)


class TamperTree:
    """A minimal copy of the tree the private-fixture sweep reads."""

    def __init__(self, directory: Path):
        self.root = directory
        (self.root / 'tests').mkdir(parents=True)
        (self.root / 'runner').mkdir(parents=True)
        (self.root / 'scripts').mkdir(parents=True)
        shutil.copy(REPO / 'tests' / 'CMakeLists.txt', self.root / 'tests')
        shutil.copy(REPO / 'runner' / 'CMakeLists.txt', self.root / 'runner')
        shutil.copytree(REPO / 'tests' / 'cr150.d',
                        self.root / 'tests' / 'cr150.d')
        # the sweep resolves a row's Python driver against the tree root, so
        # the tamper tree carries the scripts a real row names (the sweep
        # fails when a named driver is absent -- that is what it now checks)
        for driver in sorted((REPO / 'scripts').glob('*.py')):
            shutil.copy(driver, self.root / 'scripts')
        for name in ('private_fixture_paths.txt', 'private_fixture_rows.txt',
                     'private_fixture_reads.evidence'):
            shutil.copy(REPO / 'tests' / name, self.root / 'tests')
        for driver in sorted((REPO / 'tests').glob('*.py')):
            shutil.copy(driver, self.root / 'tests')
        # The four ownership control shards and the D4 row share this nested
        # driver; one copy lets the sweep inspect every registration.
        (self.root / 'tests' / 'ownership').mkdir()
        shutil.copy(REPO / 'tests' / 'ownership' / 'test_manifest_checker.py',
                    self.root / 'tests' / 'ownership')
        (self.root / 'benchmarks').mkdir()
        shutil.copy(REPO / 'benchmarks' / 'check_provenance.py',
                    self.root / 'benchmarks')
        # The cr150.d rows test_sink_finalize, test_sink_finalize_native,
        # test_sink_to_json and test_run_json_bounded_transport name their
        # drivers under docker/; the sweep fails when a named driver is absent.
        (self.root / 'docker').mkdir()
        for driver in sorted((REPO / 'docker').glob('*.py')):
            shutil.copy(driver, self.root / 'docker')

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

    def test_missing_equivalence_probe_registration_is_caught(self):
        probe = EQUIVALENCE_PROBE
        child = f'{EQUIVALENCE_PARENT}_{probe}'
        self.tree.edit('runner/CMakeLists.txt', f'        {probe}\n', '')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f'{child} is pinned but no registration', result.stdout)

    def test_driver_probe_without_registration_is_caught(self):
        probe = 'unregistered-equivalence-probe-01'
        child = f'{EQUIVALENCE_PARENT}_{probe}'
        driver = self.tree.root / 'tests' / 'native_live_equivalence_e2e.py'
        original = driver.read_text()
        self.assertIn('PROBES = {\n', original)
        driver.write_text(original.replace(
            'PROBES = {\n',
            f'PROBES = {{\n    "{probe}": {{"market"}},\n',
            1))
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f'{child} is pinned but no registration', result.stdout)

    def test_duplicate_driver_probe_is_rejected(self):
        driver = self.tree.root / 'tests' / 'native_live_equivalence_e2e.py'
        original = driver.read_text()
        line = f'    "{EQUIVALENCE_PROBE}": {{"market"}},\n'
        self.assertEqual(1, original.count(line))
        driver.write_text(original.replace(line, line + line, 1))
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('distinct probe slugs', result.stdout + result.stderr)

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

    # a driver whose marker names a file the removal did NOT take: the marker
    # check must reject it (R3-P1-1's missing control -- the check needs a
    # control that fails when the marker is not a removed file)
    def test_a_marker_that_is_not_a_removed_file_is_caught(self):
        self.tree.append('tests/CMakeLists.txt', '''
add_test(NAME test_e2e_fm_marker_not_removed COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_e2e_fm_marker_not_removed.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
''')
        (self.tree.root / 'tests' / 'test_e2e_fm_marker_not_removed.py').write_text(
            'from pathlib import Path\n'
            'fixtures = Path(__file__).parent / "fixtures"\n'
            '    _pf_private_marker = fixtures / "e2e_fm_kept" / "kept.csv"\n'
            'if not _pf_private_marker.exists():\n'
            '    raise SystemExit(77)\n')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('tests/fixtures/e2e_fm_kept/kept.csv', result.stdout)
        self.assertIn('is not a removed file', result.stdout)

    # a row whose Python driver the tree does not have at the path its own
    # argv names: the marker check cannot run, and the sweep must fail rather
    # than skip it quietly (the old resolver looked only beside the CMakeLists
    # and answered 'not found' for scripts the tree has, then skipped them)
    def test_a_row_whose_driver_is_missing_is_caught(self):
        self.tree.append('tests/CMakeLists.txt', '''
add_test(NAME test_e2e_fm_missing_driver COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_e2e_fm_missing_driver.py)
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('row scripts not found: 1', result.stdout)
        self.assertIn('test_e2e_fm_missing_driver names the Python script',
                      result.stdout)

    # a path-shaped argument the sweep cannot resolve is exit 2, named -- the
    # old is_removed answered False for it, so the row was silently cleared
    def test_an_unresolved_path_argument_exits_2(self):
        self.tree.append('tests/CMakeLists.txt', '''
add_test(NAME test_e2e_fm_unresolved_path COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/e2e_fm_unresolved.py
    ${E2E_FM_UNRESOLVED_DIR}/fixtures/bars.csv)
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('an argument path the sweep never resolved',
                      result.stdout + result.stderr)
        self.assertIn('E2E_FM_UNRESOLVED_DIR', result.stdout + result.stderr)

    # a reference that names no path (no '/') is counted and printed, never a
    # quiet FALSE and never a failure
    def test_a_non_path_reference_is_counted_not_failed(self):
        self.tree.append('tests/CMakeLists.txt', '''
add_test(NAME test_e2e_fm_non_path_reference COMMAND ${E2E_FM_COMMAND}
    -E echo ok)
''')
        result = run_sweep(self.tree.root)
        self.assertNotIn('an argument path the sweep never resolved',
                         result.stdout + result.stderr)
        self.assertIn('argument reference not a path:', result.stdout)
        self.assertIn('${E2E_FM_COMMAND}', result.stdout)

    # control 8: the value list(POP_FRONT) pops must reach the row's command
    # line: a sweep that ignores the subcommand leaves ${..} unresolved and
    # exits 0, a sweep that models it names the popped removed path and exits 1
    def test_pop_front_popped_value_is_swept(self):
        self.tree.append('tests/CMakeLists.txt', f'''
set(e2e_fb2c_shards {REMOVED_EXAMPLE}/bars.csv filler)
list(POP_FRONT e2e_fb2c_shards e2e_fb2c_popped)
add_test(NAME e2e_fb2c_pop_front_flow
    COMMAND ${{CMAKE_COMMAND}} -E echo ${{e2e_fb2c_popped}})
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('e2e_fb2c_pop_front_flow', result.stdout)
        self.assertIn(f'{REMOVED_EXAMPLE}/bars.csv', result.stdout)

    # control 9: a list() subcommand outside the modelled set is a construct
    # the sweep cannot read: exit 2, never a walk that skips its effect
    def test_unmodelled_list_subcommand_exits_2(self):
        self.tree.append('tests/CMakeLists.txt',
                         '\nlist(SORT e2e_fb2c_unmodelled)\n')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('list(SORT)', result.stdout + result.stderr)

    # control 10: POP_FRONT reads a list the walk saw built; a list it never
    # saw is exit 2 rather than a guessed (or dropped) popped value
    def test_untracked_pop_front_input_exits_2(self):
        self.tree.append('tests/CMakeLists.txt',
                         '\nlist(POP_FRONT e2e_fb2c_never_set e2e_fb2c_out)\n')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('e2e_fb2c_never_set', result.stdout + result.stderr)

    # control 11: LENGTH is the soft case -- its result is a count that cannot
    # name a path, and the files take its inputs from file(STRINGS) -- so an
    # input the walk never saw is printed and left unknown, not guessed
    def test_untracked_length_input_is_printed(self):
        self.tree.append('tests/CMakeLists.txt',
                         '\nlist(LENGTH e2e_fb2c_never_set e2e_fb2c_len)\n')
        result = run_sweep(self.tree.root)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('list(LENGTH e2e_fb2c_never_set e2e_fb2c_len) -- '
                      'e2e_fb2c_len left unknown', result.stdout)
        self.assertIn('list() inputs not tracked: 1', result.stdout)

    # A lane registration is swept at its own file:line, with the including
    # tests/ source directory. The same registration with public data is clean.
    def test_cr150_lane_private_path_is_red_and_public_is_green(self):
        lane = self.tree.root / 'tests' / 'cr150.d' / 'e2e-path.cmake'
        private_path = f'{REMOVED_EXAMPLE}/bars.csv'
        relative_path = private_path[len('tests/'):]
        lane.write_text(f'''add_test(
    NAME test_e2e_cr150_lane_path
    COMMAND ${{CMAKE_COMMAND}} -E echo ${{CMAKE_CURRENT_SOURCE_DIR}}/{relative_path}
)
''')
        red = run_sweep(self.tree.root)
        self.assertEqual(1, red.returncode, red.stdout + red.stderr)
        self.assertIn('tests/cr150.d/e2e-path.cmake:1:', red.stdout)
        self.assertIn(f'test_e2e_cr150_lane_path names the removed path {private_path} '
                      'without being a pinned private-fixture row', red.stdout)

        lane.write_text(lane.read_text().replace(relative_path, 'cr150_public/bars.csv'))
        green = run_sweep(self.tree.root)
        self.assertEqual(0, green.returncode, green.stdout + green.stderr)
        self.assertIn('unpinned rows naming removed data: 0', green.stdout)

    def test_unreadable_cr150_lane_exits_2(self):
        lane = self.tree.root / 'tests' / 'cr150.d' / 'e2e-unreadable.cmake'
        # A dangling symlink is unreadable even when the lab runs as root.
        lane.symlink_to('missing-lane')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('tests/cr150.d/e2e-unreadable.cmake', result.stdout)
        self.assertIn('cannot read', result.stdout)

    def test_cr150_lane_order_and_including_scope_at_include_site(self):
        glob = 'file(GLOB CR150_LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake")'
        private_path = f'{REMOVED_EXAMPLE}/bars.csv'
        self.tree.edit('tests/CMakeLists.txt', glob,
                       f'list(APPEND e2e_cr150_path {private_path})\n' + glob)
        self.tree.append('tests/CMakeLists.txt', f'''
list(REMOVE_ITEM e2e_cr150_path {private_path})
list(APPEND e2e_cr150_path tests/cr150_public/bars.csv)
''')
        # Create in reverse order; the walk and its file inventory must sort.
        for suffix in ('z-last', 'a-first'):
            lane = self.tree.root / 'tests' / 'cr150.d' / f'e2e-{suffix}.cmake'
            lane.write_text(f'''add_test(NAME test_e2e_cr150_{suffix.replace('-', '_')}
    COMMAND ${{CMAKE_COMMAND}} -E echo ${{e2e_cr150_path}})
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        for suffix in ('a_first', 'z_last'):
            self.assertIn(f'test_e2e_cr150_{suffix} names the removed path {private_path}',
                          result.stdout)
        inventory = next(line for line in result.stdout.splitlines()
                         if line.startswith('CMake files read: '))
        self.assertLess(inventory.index('e2e-a-first.cmake'),
                        inventory.index('e2e-z-last.cmake'))

    def test_cr150_sort_requires_the_exact_configure_depends_lane_glob(self):
        cmake = self.tree.root / 'tests' / 'CMakeLists.txt'
        original = cmake.read_text()
        glob = 'file(GLOB CR150_LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake")'
        self.assertIn(glob, original)
        for replacement in ('# deliberately absent lane glob',
                            glob.replace(' CONFIGURE_DEPENDS', ''),
                            glob.replace('cr150.d/*.cmake', 'other.d/*.cmake')):
            with self.subTest(glob=replacement):
                cmake.write_text(original.replace(glob, replacement, 1))
                result = run_sweep(self.tree.root)
                self.assertEqual(2, result.returncode, result.stdout + result.stderr)
                self.assertIn('list(SORT) is not a subcommand this sweep models',
                              result.stdout)

    # GET makes the sort order observable: dropping SORT makes ascending
    # miss the private path, and makes descending report that path instead.
    def test_seen_sort_order_reaches_the_registered_row(self):
        cmake = self.tree.root / 'tests' / 'CMakeLists.txt'
        original = cmake.read_text()
        private_path = f'{REMOVED_EXAMPLE}/bars.csv'
        public_path = 'tests/zz_cr150_public/bars.csv'
        for option, first, second, expected in (
                ('', public_path, private_path, 1),
                (' ORDER DESCENDING', private_path, public_path, 0)):
            with self.subTest(order=option or 'default ascending'):
                cmake.write_text(original + f'''
list(APPEND e2e_cr150_sort {first})
list(APPEND e2e_cr150_sort {second})
list(SORT e2e_cr150_sort{option})
list(GET e2e_cr150_sort 0 e2e_cr150_first)
add_test(NAME test_e2e_cr150_sorted
    COMMAND ${{CMAKE_COMMAND}} -E echo ${{e2e_cr150_first}})
''')
                result = run_sweep(self.tree.root)
                self.assertEqual(expected, result.returncode, result.stdout + result.stderr)
                diagnostic = f'test_e2e_cr150_sorted names the removed path {private_path}'
                if expected == 1:
                    self.assertIn(diagnostic, result.stdout)
                else:
                    self.assertNotIn(diagnostic, result.stdout)
                    self.assertIn('unpinned rows naming removed data: 0', result.stdout)

    def test_seen_sort_rejects_options_outside_the_model(self):
        cmake = self.tree.root / 'tests' / 'CMakeLists.txt'
        original = cmake.read_text()
        for option in ('ORDER ASCENDING', 'ORDER SIDEWAYS', 'COMPARE NATURAL'):
            with self.subTest(option=option):
                cmake.write_text(original + f'''
list(APPEND e2e_cr150_sort tests/cr150_public/bars.csv)
list(SORT e2e_cr150_sort {option})
''')
                result = run_sweep(self.tree.root)
                self.assertEqual(2, result.returncode, result.stdout + result.stderr)
                self.assertIn('list(SORT)', result.stdout)

    def test_untracked_sort_in_a_function_template_exits_2(self):
        self.tree.append('tests/CMakeLists.txt', '''
function(e2e_cr150_template)
    list(SORT e2e_cr150_never_seen)
endfunction()
''')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('list(SORT) is not a subcommand this sweep models', result.stdout)


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
            capture_output=True, text=True)

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
        result = subprocess.run([PY, '-B', str(KERNEL_SWEEP)], capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('kernel python rows reading an absent private file: 0',
                      result.stdout)


TAMPER_READ = (
    'import pathlib\n'
    'try:\n'
    '    pathlib.Path(%r).read_text()\n'
    'except OSError:\n'
    '    pass\n')

TAMPER_CHILD_READ = (
    'import subprocess, sys\n'
    'subprocess.run([sys.executable, "-c",\n'
    '    "import pathlib; pathlib.Path(%r).read_text()"])\n')


class ExemptRowTraceControls(unittest.TestCase):
    """Z2: the kernel sweep's EXEMPT rows re-earn it under the audit trace.

    The rows and their scripts come from the real checker (its EXEMPT table
    and its own row parse), so an exemption added there is traced here without
    a second list to keep in step. The trace runs each script the way its own
    ctest row does -- the row's argv, the script's directory first on
    sys.path, a scratch cwd -- and records opens of paths the real private
    trees do NOT have: an exemption says the script never reads an absent
    private file, and a kept file's read is nobody's finding. A trace whose
    script raised (the tracer's exit 4) is a failure of the trace, never a
    clean result.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='e2e-fb2-trace-')
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        if os.environ.get(TRACE_MARKER):
            self.skipTest('a traced child run must not trace itself')

    def _exempt_scripts(self) -> dict:
        """row -> (its script, the argv its ctest registration passes)."""
        spec = importlib.util.spec_from_file_location(
            'check_kernel_python_private_reads', KERNEL_SWEEP)
        kernel = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(kernel)
        self.assertTrue(kernel.EXEMPT, 'no exempt row: the control is vacuous')
        rows = {name: script for name, script, _line in
                kernel.python_rows(REPO / 'tests' / 'CMakeLists.txt')}
        scripts = {}
        for row in kernel.EXEMPT:
            self.assertIn(row, rows,
                          f'{row} is exempt but registers no Python row')
            scripts[row] = (rows[row], row_argv(kernel, row))
        return scripts

    def assertTraceCompleted(self, result, where: str) -> None:
        """A trace whose script raised (the tracer's exit 4) is a failure of
        the trace, not an observation: the script did not run to the end, so
        it had done none of its work when the log was written."""
        self.assertNotEqual(
            TRACE_RAISED_EXIT, result.returncode,
            f'{where}: the traced script did not run to the end (trace exit '
            f'{TRACE_RAISED_EXIT}): ' + result.stdout + result.stderr)

    # control 12: every exempt row's script, traced the way its row runs it,
    # opens no file under the real tests/fixtures/ or tests/oracle/ and runs
    # to its own end -- the evidence its exemption cites, re-run in-tree
    # instead of trusted. Each script's exit code in the fixture-less tree is
    # 0, read from the scripts and registrations themselves, so 0 is the code
    # this control compares to (a script that died earlier records its own
    # code and fails here).
    def test_exempt_rows_open_no_real_private_file(self):
        for row, (script, argv) in sorted(self._exempt_scripts().items()):
            with self.subTest(row=row):
                result = run_under_trace(REPO, self.tmp, script, argv)
                self.assertTraceCompleted(result, row)
                self.assertIn('trace: the traced script exited 0',
                              result.stdout, result.stdout + result.stderr)
                self.assertEqual(0, result.returncode,
                                 result.stdout + result.stderr)
                self.assertIn('trace: opens under the real private trees: 0',
                              result.stdout)

    def _tampered_copy(self, name: str, tamper: str) -> Path:
        """The exempt script plus one real private-tree read: inserted after
        the __future__ import so the copy still parses and really runs."""
        source = (REPO / 'scripts' / 'test_private_fixture_reads.py').read_text()
        future = 'from __future__ import annotations\n'
        self.assertIn(future, source, 'the exempt script changed shape')
        script = self.tmp / name
        script.write_text(source.replace(
            future, future + tamper % str(REPO / REMOVED_EXAMPLE / 'bars.csv'), 1))
        return script

    # control 13: the trace can fail -- a copy of an exempt script that reads
    # a real private path is named and the trace exits 3. The copy's own exit
    # code is not compared here: it is the suite run from a scratch path, so
    # its REPO is the scratch parent and a non-zero code is that copy's
    # business -- the tracer records it in the output, and the failure this
    # control watches is the exit-4 raise.
    def test_a_tampered_copy_that_reads_a_fixture_is_failed(self):
        script = self._tampered_copy('tampered_in_process.py', TAMPER_READ)
        result = run_under_trace(REPO, self.tmp, script)
        self.assertTraceCompleted(result, 'the in-process tampered copy')
        self.assertEqual(TRACE_PRIVATE_OPEN_EXIT, result.returncode,
                         result.stdout + result.stderr)
        self.assertIn(f'trace: opened {REPO}/{REMOVED_EXAMPLE}/bars.csv',
                      result.stdout)

    # control 14: the trace follows the script's children -- the same read in
    # a child interpreter is still named, where a parent-only hook is blind
    def test_a_child_process_read_is_still_failed(self):
        script = self._tampered_copy('tampered_child.py', TAMPER_CHILD_READ)
        result = run_under_trace(REPO, self.tmp, script)
        self.assertTraceCompleted(result, 'the child-read tampered copy')
        self.assertEqual(TRACE_PRIVATE_OPEN_EXIT, result.returncode,
                         result.stdout + result.stderr)
        self.assertIn(f'trace: opened {REPO}/{REMOVED_EXAMPLE}/bars.csv',
                      result.stdout)

    def _two_world_copy(self, name: str, tamper: str) -> Path:
        """A copy of scripts/check_two_world_registration.py under the scratch
        dir's scripts/, with the local modules its imports need (ci_verify,
        its own prepare_settlement_cpp_abi_base, check_private_fixture_reads
        -- read off the script's import lines), plus `tamper`: a source
        snippet (one %r) inserted after the __future__ import, so the copy
        parses and really runs. The copy runs with --root REPO: its __file__
        is the scratch dir, so the REPO it computes would be the scratch
        tree, and the row's evaluation is about the real one."""
        siblings = self.tmp / 'scripts'
        siblings.mkdir(exist_ok=True)
        for sibling in ('ci_verify.py', 'prepare_settlement_cpp_abi_base.py',
                        'check_private_fixture_reads.py'):
            if not (siblings / sibling).exists():
                shutil.copy(REPO / 'scripts' / sibling, siblings / sibling)
        source = (REPO / 'scripts' / 'check_two_world_registration.py').read_text()
        future = 'from __future__ import annotations\n'
        self.assertIn(future, source, 'the two-world script changed shape')
        script = siblings / name
        script.write_text(source.replace(future, future + tamper, 1))
        return script

    # control 15: the two-world exemption's own RED and GREEN (lane FR made
    # it exempt with a trace that never ran it: the copy stopped at `import
    # ci_verify`). The RED copy is the real script plus one open of a private
    # path the tree does not have: the trace must name that path and exit
    # TRACE_PRIVATE_OPEN_EXIT while the copy still records its own exit 0, so
    # the RED fails for the OPEN, never for an import error. The GREEN copy,
    # the same shape without the open, must trace clean.
    def test_two_world_copy_open_is_red_and_clean_is_green(self):
        # tests/fixtures/whole_lot_tie/bars.csv is absent in BOTH worlds and
        # that is what the hook records: the pin empties the directory (`D
        # tests/fixtures/whole_lot_tie`) and bars.csv is not one of its F
        # lines, so the private overlay restores no such file; a present
        # path's read would not be recorded at all (a kept file is nobody's
        # finding), so a pinned removed path that the overlay DID restore
        # could not carry this control in the overlay world.
        probe = REPO / REMOVED_EXAMPLE / 'bars.csv'
        argv = ('--both', '--root', str(REPO))

        red = run_under_trace(REPO, self.tmp, self._two_world_copy(
            'two_world_tampered.py', TAMPER_READ % str(probe)), argv)
        self.assertTraceCompleted(red, 'the tampered two-world copy')
        self.assertIn('trace: the traced script exited 0',
                      red.stdout, red.stdout + red.stderr)
        self.assertEqual(TRACE_PRIVATE_OPEN_EXIT, red.returncode,
                         red.stdout + red.stderr)
        self.assertIn(f'trace: opened {probe}', red.stdout)

        green = run_under_trace(REPO, self.tmp, self._two_world_copy(
            'two_world_clean.py', ''), argv)
        self.assertTraceCompleted(green, 'the untampered two-world copy')
        self.assertIn('trace: the traced script exited 0',
                      green.stdout, green.stdout + green.stderr)
        self.assertEqual(0, green.returncode, green.stdout + green.stderr)
        self.assertIn('trace: opens under the real private trees: 0',
                      green.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
