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

The ctest row passes its build tree's values for five CMake names (see
BUILD_TREE_NAMES): the trace controls need them to re-run a row whose own
registration passes such values. Run by hand without them, those controls
refuse each such row by name and print the options to pass; no row is skipped
and none is traced with made-up values.
"""
from __future__ import annotations

import argparse
import functools
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


# A registration may pass its script CMake values that only a build tree
# knows. The trace re-runs a row with the row's own arguments, so it needs
# those values from the build tree it runs in: this suite's ctest row
# (tests/CMakeLists.txt) passes them for exactly the five names below -- the
# five tests/lanes.d/private-marker-rows-registration.cmake uses -- and
# __main__ stores them in BUILD_TREE_VALUES. Run by hand without them, the
# table stays empty and row_argv refuses a row that names one of the five,
# printing the options to pass: such a row is never traced with made-up
# values and never skipped.
BUILD_TREE_NAMES = ('CMAKE_COMMAND', 'CMAKE_CTEST_COMMAND', 'CMAKE_GENERATOR',
                    'CMAKE_MAKE_PROGRAM', 'CMAKE_CURRENT_BINARY_DIR')
BUILD_TREE_REFERENCE = re.compile(r'\$\{(%s)\}' % '|'.join(BUILD_TREE_NAMES))
BUILD_TREE_VALUES = {}


def build_tree_option(name: str) -> str:
    """The command-line option that carries one build-tree value."""
    return '--' + name.lower().replace('_', '-')


@functools.lru_cache(maxsize=None)
def kernel_sweep():
    """scripts/check_kernel_python_private_reads.py as a module, loaded from
    its own file. The trace controls take the exempt rows from its table and
    its row parse, and row_argv reads a registration with its tokenizer and
    its pattern for a reference, so neither has a second copy here."""
    spec = importlib.util.spec_from_file_location(
        'check_kernel_python_private_reads', KERNEL_SWEEP)
    kernel = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kernel)
    return kernel


def expanded_argument(word, row: str, values: dict) -> str:
    """One registration argument with its build-tree names filled in.

    `word` is the argument as the kernel sweep's tokenizer read it: `text`
    is the argument without its quotes and with its escapes decoded, `raw`
    the registration's own spelling of it (empty for a bracket argument).
    """
    token = word.text
    # A "$" is read only in an argument the registration spells the way its
    # text reads, bare or between quotes. Spelled with a backslash escape or
    # as a bracket argument, the text is not what the registration wrote, and
    # what CMake makes of a "$" there is not worked out here: the argument is
    # refused under its own spelling, never filled in.
    if '$' in token and word.raw not in (token, '"' + token + '"'):
        raise AssertionError(
            f'{row}: unexpanded registration argument '
            f'{(word.raw or token)!r}: it holds a "$" and is spelled with a '
            'backslash escape or as a bracket argument, which is not read')
    # The odd pieces are the names BUILD_TREE_REFERENCE matched, the even
    # ones the argument's own text around them. Only that text is searched
    # for another reference, with the sweep's own pattern for one CMake has
    # yet to evaluate (${, $ENV{, $CACHE{ or the $< of a generator
    # expression): a value is used as it is, never read again.
    pieces = BUILD_TREE_REFERENCE.split(token)
    unexpanded = kernel_sweep().CMAKE_UNEXPANDED_REF
    if any(unexpanded.search(text) for text in pieces[0::2]):
        raise AssertionError(
            f'{row}: unexpanded registration argument {token!r}: only '
            + ', '.join(BUILD_TREE_NAMES) + ' are filled in')
    for index in range(1, len(pieces), 2):
        name = pieces[index]
        if name not in values:
            raise AssertionError(
                f'{row}: the registration argument {token!r} needs the build '
                f"tree's {name}, which this run was not given. The ctest row "
                'test_private_fixture_reads_controls passes it; by hand, pass '
                + ' '.join(f'{build_tree_option(each)}=<{each}>'
                           for each in BUILD_TREE_NAMES)
                + " with that build tree's values")
        pieces[index] = values[name]
    return ''.join(pieces)


def row_argv(statement: str, row: str, values=None) -> tuple:
    """The script arguments from this row's own registration.

    The registration is read with the kernel sweep's own tokenizer: a quoted
    argument is one argument, the spaces of its own text included. Each
    argument is then expanded on its own, so a value with a space stays
    inside its one argument, as it does when ctest runs the row. `values`
    maps a name of BUILD_TREE_NAMES to the build tree's value; no other name
    is expanded, whatever `values` holds.
    """
    # The statement is add_test(<body>): the body is tokenized the way the
    # sweep tokenizes it, and the arguments are the words behind the script.
    fields = list(kernel_sweep().words(statement[len('add_test('):-1], 1))
    script = next((index for index, word in enumerate(fields)
                   if word.text.endswith('.py')), None)
    if script is None:
        raise AssertionError(f'{row}: the registration names no .py script')
    return tuple(expanded_argument(word, row, values or {})
                 for word in fields[script + 1:])


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
        shutil.copytree(REPO / 'tests' / 'lanes.d',
                        self.root / 'tests' / 'lanes.d')
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
        # The report-channel rows name their drivers under three nested test
        # directories (each with the helpers its driver imports).
        for nested in ('report_channel', 'report_channel_deadlines', 'entrypoint_limits'):
            shutil.copytree(REPO / 'tests' / nested, self.root / 'tests' / nested,
                            ignore=shutil.ignore_patterns('__pycache__'))
        (self.root / 'benchmarks').mkdir()
        shutil.copy(REPO / 'benchmarks' / 'check_provenance.py',
                    self.root / 'benchmarks')
        # The lanes.d rows test_sink_finalize, test_sink_finalize_native,
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
        lane = self.tree.root / 'tests' / 'lanes.d' / 'e2e-path.cmake'
        private_path = f'{REMOVED_EXAMPLE}/bars.csv'
        relative_path = private_path[len('tests/'):]
        lane.write_text(f'''add_test(
    NAME test_e2e_cr150_lane_path
    COMMAND ${{CMAKE_COMMAND}} -E echo ${{CMAKE_CURRENT_SOURCE_DIR}}/{relative_path}
)
''')
        red = run_sweep(self.tree.root)
        self.assertEqual(1, red.returncode, red.stdout + red.stderr)
        self.assertIn('tests/lanes.d/e2e-path.cmake:1:', red.stdout)
        self.assertIn(f'test_e2e_cr150_lane_path names the removed path {private_path} '
                      'without being a pinned private-fixture row', red.stdout)

        lane.write_text(lane.read_text().replace(relative_path, 'cr150_public/bars.csv'))
        green = run_sweep(self.tree.root)
        self.assertEqual(0, green.returncode, green.stdout + green.stderr)
        self.assertIn('unpinned rows naming removed data: 0', green.stdout)

    def test_unreadable_cr150_lane_exits_2(self):
        lane = self.tree.root / 'tests' / 'lanes.d' / 'e2e-unreadable.cmake'
        # A dangling symlink is unreadable even when the lab runs as root.
        lane.symlink_to('missing-lane')
        result = run_sweep(self.tree.root)
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn('tests/lanes.d/e2e-unreadable.cmake', result.stdout)
        self.assertIn('cannot read', result.stdout)

    def test_cr150_lane_order_and_including_scope_at_include_site(self):
        glob = 'file(GLOB LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lanes.d/*.cmake")'
        private_path = f'{REMOVED_EXAMPLE}/bars.csv'
        self.tree.edit('tests/CMakeLists.txt', glob,
                       f'list(APPEND e2e_cr150_path {private_path})\n' + glob)
        self.tree.append('tests/CMakeLists.txt', f'''
list(REMOVE_ITEM e2e_cr150_path {private_path})
list(APPEND e2e_cr150_path tests/cr150_public/bars.csv)
''')
        # Create in reverse order; the walk and its file inventory must sort.
        for suffix in ('z-last', 'a-first'):
            lane = self.tree.root / 'tests' / 'lanes.d' / f'e2e-{suffix}.cmake'
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
        glob = 'file(GLOB LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lanes.d/*.cmake")'
        self.assertIn(glob, original)
        for replacement in ('# deliberately absent lane glob',
                            glob.replace(' CONFIGURE_DEPENDS', ''),
                            glob.replace('lanes.d/*.cmake', 'other.d/*.cmake')):
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
        for module in ('cmake_test_includes.py', 'check_lanes_cmake_d.py'):
            shutil.copy(REPO / 'scripts' / module, self.root / 'scripts')
        self.script = self.root / 'scripts' / 'check_kernel_python_private_reads.py'
        self.cmake = self.root / 'tests' / 'CMakeLists.txt'
        self.addCleanup(self._tmp.cleanup)

    def run_sweep(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [PY, '-B', str(self.script), '--cmake', str(self.cmake)],
            capture_output=True, text=True)

    def included_registration(self) -> Path:
        self.cmake.write_text(
            'file(GLOB LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lanes.d/*.cmake")\n'
            'list(SORT LANE_CMAKE)\nforeach(lane_file IN LISTS LANE_CMAKE)\n'
            '    include("${lane_file}")\nendforeach()\n')
        directory = self.cmake.parent / 'lanes.d'
        directory.mkdir()
        return directory / 'reader.cmake'

    def test_python_row_reading_absent_private_file_is_caught(self):
        fragment = self.included_registration()
        reader = self.cmake.parent / 'read_bars.py'
        reader.write_text(f'with open("{REMOVED_EXAMPLE}/bars.csv") as bars:\n'
                          '    bars.read()\n')
        for command in ('${PROJECT_SOURCE_DIR}/tests/read_bars.py -c "pass"',
                        '-B -c "import read_bars"'):
            with self.subTest(command=command):
                fragment.write_text(
                    'add_test(NAME test_read_bars COMMAND ${Python3_EXECUTABLE} '
                    + command + ' WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})\n')
                result = self.run_sweep()
                self.assertEqual(1, result.returncode, result.stdout + result.stderr)
                self.assertIn(f'test_read_bars: reads absent {REMOVED_EXAMPLE}/bars.csv '
                              f'({reader}:1)', result.stdout)

    def test_included_python_read_requires_its_own_skip(self):
        fragment = self.included_registration()
        fragment.write_text(
            'add_test(NAME test_read_bars COMMAND ${Python3_EXECUTABLE} '
            '${CMAKE_CURRENT_SOURCE_DIR}/read_bars.py)\n'
            'add_test(NAME test_forward_arguments COMMAND ${Python3_EXECUTABLE} -B '
            '-c "import os, sys; os.execv(sys.argv[1], sys.argv[1:])" '
            '${CMAKE_COMMAND} -E echo unused.py)\n')
        reader = self.cmake.parent / 'read_bars.py'
        reader.write_text('from pathlib import Path\n'
                          'missing = 77\n'
                          f'bars = Path("{REMOVED_EXAMPLE}/bars.csv")\n'
                          'if not bars.exists():\n'
                          '    print("SKIP private fixtures not present: " + str(bars))\n'
                          '    raise SystemExit(missing)\n'
                          'print(bars.read_text())\n')
        result = self.run_sweep()
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f'test_read_bars: reads absent {REMOVED_EXAMPLE}/bars.csv', result.stdout)
        with fragment.open('a') as out:
            out.write('set_tests_properties(test_read_bars PROPERTIES SKIP_RETURN_CODE 77)\n')
        result = self.run_sweep()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('kernel python rows reading an absent private file: 0', result.stdout)

    def test_unparseable_python_row_exits_2(self):
        fragment = self.included_registration()
        missing = f'{REMOVED_EXAMPLE}/bars.csv'
        program = ('from pathlib import Path\n'
                   f'bars = Path("{missing}")\n'
                   'if not bars.exists():\n'
                   '    print("SKIP private fixtures not present: " + str(bars))\n'
                   '    raise SystemExit(77)\n'
                   'bars.read_text()')
        command = 'add_test(NAME test_inline_reader COMMAND ${Python3_EXECUTABLE} -B -c '
        unreadable = 'add_test(NAME test_no_program COMMAND ${Python3_EXECUTABLE} -B -c)\n'
        quoted_reader = command + '"' + program.replace('"', r'\"') + '")\n'
        fragment.write_text(unreadable + quoted_reader)
        result = self.run_sweep()
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn(f'{fragment}:1: row test_no_program', result.stderr)
        self.assertIn('names no .py script', result.stderr)
        self.assertIn(f'test_inline_reader: reads absent {missing} ({fragment}:3)', result.stdout)

        fragment.write_text(command + '[=[\n' + program + ']=])\n')
        result = self.run_sweep()
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn(f'test_inline_reader: reads absent {missing} ({fragment}:3)', result.stdout)
        with fragment.open('a') as out:
            out.write('set_tests_properties(test_inline_reader PROPERTIES SKIP_RETURN_CODE 77)\n')
        result = self.run_sweep()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('kernel python rows reading an absent private file: 0', result.stdout)

    def test_real_tree_is_clean(self):
        result = subprocess.run([PY, '-B', str(KERNEL_SWEEP)], capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('kernel python rows reading an absent private file: 0',
                      result.stdout)


class RowArgvControls(unittest.TestCase):
    """row_argv: the arguments a row's ctest registration passes its script.

    A registration may pass CMake values that only a build tree knows. The
    trace can reproduce such a row only with the values of the build tree it
    runs in, and takes them for exactly five names: CMAKE_COMMAND,
    CMAKE_CTEST_COMMAND, CMAKE_GENERATOR, CMAKE_MAKE_PROGRAM and
    CMAKE_CURRENT_BINARY_DIR. Every registration and value below is invented;
    no row of the tree is read.
    """

    ROW = 'test_invented_row'
    VALUES = {
        'CMAKE_COMMAND': '/opt/invented/bin/cmake',
        'CMAKE_CTEST_COMMAND': '/opt/invented/bin/ctest',
        'CMAKE_GENERATOR': 'Invented',
        'CMAKE_MAKE_PROGRAM': '/opt/invented/bin/build-tool',
        'CMAKE_CURRENT_BINARY_DIR': '/srv/invented-build/tests',
    }

    @staticmethod
    def registration(*arguments: str) -> str:
        """An add_test() statement in the shape the registrations take: the
        script on the second line, then the arguments it passes."""
        return ('add_test(NAME test_invented_row COMMAND ${Python3_EXECUTABLE} -B\n'
                '    ${PROJECT_SOURCE_DIR}/tests/test_invented_row.py '
                + ' '.join(arguments) + ')')

    # an argument that names one of the five carries the build tree's value,
    # wherever the name sits in it and however many of them it holds
    def test_an_argument_naming_a_build_tree_value_is_expanded(self):
        statement = self.registration(
            '--plain',
            '"--tool=${CMAKE_COMMAND}"',
            '"--runner=${CMAKE_CTEST_COMMAND}"',
            '"--kind=${CMAKE_GENERATOR}"',
            '"--builder=${CMAKE_MAKE_PROGRAM}"',
            '"--scratch=${CMAKE_CURRENT_BINARY_DIR}/invented-work"',
            '"--pair=${CMAKE_COMMAND}:${CMAKE_CTEST_COMMAND}"')
        self.assertEqual(
            ('--plain',
             '--tool=/opt/invented/bin/cmake',
             '--runner=/opt/invented/bin/ctest',
             '--kind=Invented',
             '--builder=/opt/invented/bin/build-tool',
             '--scratch=/srv/invented-build/tests/invented-work',
             '--pair=/opt/invented/bin/cmake:/opt/invented/bin/ctest'),
            row_argv(statement, self.ROW, self.VALUES))

    # any other reference is still refused, and the refusal names the
    # argument, not the expanded one before it: a name close to one of the
    # five, a nested reference and a name whose value the caller offers all
    # stay outside the five, and so do the references that are no ${NAME} at
    # all -- an environment variable, a cache entry and a generator
    # expression, alone or right behind one of the five
    def test_any_other_reference_is_still_refused(self):
        offered = dict(self.VALUES, CMAKE_BINARY_DIR='/srv/invented-build',
                       INVENTED_VALUE='invented')
        for reference in ('${CMAKE_BINARY_DIR}', '${INVENTED_VALUE}',
                          '${CMAKE_COMMAND_LINE}', '${MY_CMAKE_COMMAND}',
                          '${INVENTED_${CMAKE_GENERATOR}}',
                          '$ENV{INVENTED}', '$CACHE{INVENTED}', '$<CONFIG>',
                          '${CMAKE_COMMAND}$ENV{INVENTED}'):
            with self.subTest(reference=reference):
                refused = '--other=' + reference
                statement = self.registration(
                    '"--tool=${CMAKE_COMMAND}"', '"' + refused + '"')
                with self.assertRaises(AssertionError) as caught:
                    row_argv(statement, self.ROW, offered)
                self.assertIn(
                    f'{self.ROW}: unexpanded registration argument {refused!r}',
                    str(caught.exception))

    # a reference outside the five is refused wherever it sits in an
    # argument that also names one of the five: the argument's own text is
    # read on every side of a filled-in name -- before the first, behind the
    # last and between two -- not only up to the first one
    def test_a_foreign_reference_beside_a_build_tree_name_is_refused(self):
        for refused in ('--pair=${INVENTED_OTHER}:${CMAKE_COMMAND}',
                        '--pair=${CMAKE_COMMAND}:${INVENTED_OTHER}',
                        '--trio=${CMAKE_COMMAND}:${INVENTED_OTHER}:${CMAKE_CTEST_COMMAND}'):
            with self.subTest(argument=refused):
                statement = self.registration('"' + refused + '"')
                with self.assertRaises(AssertionError) as caught:
                    row_argv(statement, self.ROW, self.VALUES)
                self.assertIn(
                    f'{self.ROW}: unexpanded registration argument {refused!r}',
                    str(caught.exception))

    # a reference whose name is itself a reference is refused, also when the
    # inner name is one of the five: what is left around that name is an
    # opened ${ with no name of its own, and it is the opening that is
    # refused, not only a whole ${NAME}
    def test_a_nested_reference_is_refused(self):
        for refused in ('--other=${${INVENTED}}', '--other=${${CMAKE_COMMAND}}'):
            with self.subTest(argument=refused):
                statement = self.registration('"' + refused + '"')
                with self.assertRaises(AssertionError) as caught:
                    row_argv(statement, self.ROW, self.VALUES)
                self.assertIn(
                    f'{self.ROW}: unexpanded registration argument {refused!r}',
                    str(caught.exception))

    # CMake reads no reference where the registration escapes one, \${NAME}
    # or $\{NAME}, and none inside a bracket argument. An argument that holds
    # a "$" and is spelled in one of those ways is refused, under the
    # registration's own spelling where it has one (a bracket argument is
    # named by its text): it is never filled in behind the backslash or
    # between the brackets, and never passed on as it stands
    def test_an_escaped_or_bracketed_reference_is_refused(self):
        for spelled, named in (
                (r'"--tool=\${CMAKE_COMMAND}"', r'"--tool=\${CMAKE_COMMAND}"'),
                (r'"--tool=$\{CMAKE_COMMAND}"', r'"--tool=$\{CMAKE_COMMAND}"'),
                ('[[--tool=${CMAKE_COMMAND}]]', '--tool=${CMAKE_COMMAND}')):
            with self.subTest(spelled=spelled):
                with self.assertRaises(AssertionError) as caught:
                    row_argv(self.registration(spelled), self.ROW, self.VALUES)
                self.assertIn(
                    f'{self.ROW}: unexpanded registration argument {named!r}',
                    str(caught.exception))

    # the registration is split into arguments first and each argument is
    # expanded on its own, so a value with a space -- a generator such as
    # "Unix Makefiles", a build directory -- stays inside its one argument
    def test_a_value_with_a_space_stays_one_argument(self):
        spaced = dict(self.VALUES, CMAKE_GENERATOR='Invented Makefiles',
                      CMAKE_CURRENT_BINARY_DIR='/srv/invented build/tests')
        statement = self.registration(
            '"--kind=${CMAKE_GENERATOR}"',
            '"--scratch=${CMAKE_CURRENT_BINARY_DIR}/invented-work"')
        self.assertEqual(
            ('--kind=Invented Makefiles',
             '--scratch=/srv/invented build/tests/invented-work'),
            row_argv(statement, self.ROW, spaced))

    # an argument is what CMake reads as one: a quoted argument stays whole,
    # the spaces of its own text included, with or without a name to fill in
    def test_a_quoted_argument_with_a_space_stays_one_argument(self):
        statement = self.registration(
            '"--name=two words"', '"--label=made with ${CMAKE_GENERATOR}"')
        self.assertEqual(
            ('--name=two words', '--label=made with Invented'),
            row_argv(statement, self.ROW, self.VALUES))

    # an empty value is a value: its name is filled in with nothing, and the
    # argument is not refused as one whose value was not given
    def test_an_empty_value_fills_its_name_with_nothing(self):
        emptied = dict(self.VALUES, CMAKE_MAKE_PROGRAM='')
        statement = self.registration(
            '"--builder=${CMAKE_MAKE_PROGRAM}"', '"--kind=${CMAKE_GENERATOR}"')
        self.assertEqual(('--builder=', '--kind=Invented'),
                         row_argv(statement, self.ROW, emptied))

    # a value is used as it is and never read again: a backslash in it is no
    # escape, a quote in it stays, and a ${...} in it is neither filled in,
    # though it spells one of the five, nor refused, though it spells
    # another name
    def test_a_value_is_used_as_it_is(self):
        odd = dict(self.VALUES,
                   CMAKE_COMMAND=r'C:\invented\cmake.exe',
                   CMAKE_GENERATOR='Invented "Quoted" Kind',
                   CMAKE_MAKE_PROGRAM='/opt/${CMAKE_COMMAND}/${INVENTED_OTHER}')
        statement = self.registration(
            '"--tool=${CMAKE_COMMAND}"', '"--kind=${CMAKE_GENERATOR}"',
            '"--builder=${CMAKE_MAKE_PROGRAM}"')
        self.assertEqual(
            (r'--tool=C:\invented\cmake.exe',
             '--kind=Invented "Quoted" Kind',
             '--builder=/opt/${CMAKE_COMMAND}/${INVENTED_OTHER}'),
            row_argv(statement, self.ROW, odd))

    # run by hand, the script has no build tree to ask: one of the five
    # without its value is refused -- a failure, never a skip -- and the
    # refusal names the row, the argument and the option that passes the value
    def test_a_name_without_its_value_is_refused_with_the_option_to_pass(self):
        statement = self.registration('"--kind=${CMAKE_GENERATOR}"')
        with self.assertRaises(AssertionError) as caught:
            row_argv(statement, self.ROW)
        message = str(caught.exception)
        self.assertIn(self.ROW, message)
        self.assertIn("'--kind=${CMAKE_GENERATOR}'", message)
        self.assertIn('--cmake-generator=', message)

    # the values are taken name by name, not all or none: with some of the
    # five given, a registration that names only those is filled in, and one
    # that also names another is refused for that name and that argument
    def test_some_values_fill_their_names_and_a_missing_one_is_refused(self):
        some = {name: self.VALUES[name]
                for name in ('CMAKE_COMMAND', 'CMAKE_CTEST_COMMAND')}
        given = self.registration(
            '"--tool=${CMAKE_COMMAND}"', '"--runner=${CMAKE_CTEST_COMMAND}"')
        self.assertEqual(
            ('--tool=/opt/invented/bin/cmake', '--runner=/opt/invented/bin/ctest'),
            row_argv(given, self.ROW, some))
        missing = self.registration(
            '"--tool=${CMAKE_COMMAND}"', '"--kind=${CMAKE_GENERATOR}"')
        with self.assertRaises(AssertionError) as caught:
            row_argv(missing, self.ROW, some)
        self.assertIn(
            self.ROW + ": the registration argument '--kind=${CMAKE_GENERATOR}' "
            "needs the build tree's CMAKE_GENERATOR",
            str(caught.exception))


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
    clean result. Where a registration passes CMake values, the argv holds
    this build tree's (BUILD_TREE_NAMES); a run that was not given them fails
    that row by name instead of tracing it some other way.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='e2e-fb2-trace-')
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        if os.environ.get(TRACE_MARKER):
            self.skipTest('a traced child run must not trace itself')

    def _exempt_scripts(self) -> dict:
        """row -> (its script, its own ctest registration)."""
        kernel = kernel_sweep()
        self.assertTrue(kernel.EXEMPT, 'no exempt row: the control is vacuous')
        scripts = {}
        for source, cmd in kernel.registration_statements(REPO / 'tests' / 'CMakeLists.txt'):
            if cmd.name.lower() == 'add_test':
                statement = f'add_test({cmd.body})'
                match = kernel.ADD_TEST.search(statement)
                if match and match[1] in kernel.EXEMPT:
                    row = kernel.python_row(source, cmd.line, statement)
                    if row is not None:
                        name, (script,) = row
                        scripts[name] = (script, statement)
        self.assertEqual(set(scripts), set(kernel.EXEMPT),
                         'every exemption must name a registered Python row')
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
        for row, (script, statement) in sorted(self._exempt_scripts().items()):
            with self.subTest(row=row):
                # The argv is built inside the row's own sub-test: a
                # registration this run cannot fill in (BUILD_TREE_NAMES)
                # fails that row alone, with the options to pass.
                argv = row_argv(statement, row, BUILD_TREE_VALUES)
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
        its own private_fixture_policy and prepare_settlement_cpp_abi_base,
        check_private_fixture_reads -- read off the import lines of the
        script and of ci_verify), plus `tamper`: a source
        snippet (one %r) inserted after the __future__ import, so the copy
        parses and really runs. The copy runs with --root REPO: its __file__
        is the scratch dir, so the REPO it computes would be the scratch
        tree, and the row's evaluation is about the real one."""
        siblings = self.tmp / 'scripts'
        siblings.mkdir(exist_ok=True)
        for sibling in ('ci_verify.py', 'private_fixture_policy.py',
                        'prepare_settlement_cpp_abi_base.py',
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

    # control 16: the row registered with CMake values is traced with them,
    # and its exemption keeps its subject.
    # tests/lanes.d/private-marker-rows-registration.cmake passes its script
    # five values of the build tree (cmake, ctest, the generator, the make
    # program and a work directory), so that row's exemption holds only while
    # the trace can run it the way the registration does. Control 12 runs
    # that trace, once, as it does for every exempt row. This control checks
    # what makes it the trace meant here: the row is on the trace's list, and
    # its registration does name build-tree values, which row_argv fills in
    # there. It also ties the exemption to what earned it: the script's text
    # still names both private paths it writes as stand-ins in its scratch
    # trees. A script that names neither gives the sweep nothing to exempt,
    # and the entry would go on covering whatever the script names later;
    # this control fails as soon as one of the two is gone.
    # Each path is written in two parts here, as tests/test_kernel_python_
    # population.py writes its own, so that the sweep reads no further
    # private path in this file on account of the two it only looks for.
    def test_a_row_registered_with_build_tree_values_is_traced_with_them(self):
        row = 'test_private_marker_rows_registration'
        scripts = self._exempt_scripts()
        self.assertIn(row, sorted(scripts),
                      f'{row} is not an exempt row, so no control traces it')
        script, statement = scripts[row]
        self.assertTrue(BUILD_TREE_REFERENCE.search(statement),
                        f'{row}: the registration names no build-tree value, '
                        'so this control has lost its subject')
        text = script.read_text()
        for payload in ('tests/' + 'fixtures/exit_binding/schedules.inc',
                        'tests/' + 'fixtures/cross_side_exit/README.md'):
            # assertTrue, not assertIn: a failure must not print the script.
            self.assertTrue(
                payload in text,
                f'{row}: {script} no longer names {payload}. The row was '
                'exempted as a script that names both stand-in paths: read '
                'its entry in scripts/check_kernel_python_private_reads.py '
                'against the script again, or take the row off EXEMPT')


if __name__ == '__main__':
    # The ctest row passes its build tree's value for each of
    # BUILD_TREE_NAMES; every other argument is unittest's own.
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    for build_tree_name in BUILD_TREE_NAMES:
        parser.add_argument(build_tree_option(build_tree_name), dest=build_tree_name)
    given, rest = parser.parse_known_args()
    BUILD_TREE_VALUES.update((name, value) for name, value in vars(given).items()
                             if value is not None)
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
