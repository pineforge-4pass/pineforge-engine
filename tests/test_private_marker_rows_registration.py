#!/usr/bin/env python3
"""The registration rule of the private rows that read no private fixture.

Why this exists
---------------
A pinned private-fixtures row normally needs a private file: its source, a
fixture or a quoted include that the public tree does not hold. The rows in
ROWS below need none. Each is built from a translation unit this tree holds
with and without the private fixtures, includes nothing the public tree
lacks, and opens no file when it runs. Its registration nevertheless keyed on
one private fixture file the row never reads. A tree that held the private
fixtures but not that one file registered the pinned SKIP placeholder, so the
unit test did not run there.

The rule this test pins
-----------------------
Each row stays a pinned private-fixtures row (tests/private_fixture_rows.txt),
and its registration block in tests/CMakeLists.txt answers two states:

* private fixtures absent: the pinned SKIP placeholder, as for every such row
  (the literal, the private_fixtures label, the skip expression);
* private fixtures present: the real unit test, registered through
  pineforge_add_unit_test, whichever other private files the tree holds.

"Private fixtures present" means that the marker file of
scripts/check_two_world_registration.py (its OVERLAY_MARKER) exists. This test
reads the marker out of that script, so the two cannot disagree. A profile
that does not build the source layer registers no row at all, in any state.

Every state is checked for every row of ROWS, one sub-test per row. A block of
tests/CMakeLists.txt that tests the marker must belong to a row of ROWS, so no
row follows the rule unchecked. test_publication_witness is the one exception:
it follows the same marker and has a third state of its own, so
tests/test_publication_witness_registration.py pins that row.

A block has no branch of its own for a missing input. A row's translation
unit and the headers it includes are part of this tree in both states.
pineforge_add_unit_test passes the translation unit to add_executable: with
the private fixtures present, a tree without it does not configure, and a
tree without one of the headers does not build.

How it is checked
-----------------
A row's block is cut out of tests/CMakeLists.txt as it stands and configured
by the real CMake inside a scratch project, once per state. The scratch tree
holds only the files the state names, as stand-ins: the test asks which row is
registered, and never builds or runs the unit test itself. The real CTest then
lists the row and runs it. pineforge_add_unit_test is replaced by a stand-in
that registers a row printing a known line, so "the real unit test was
registered" is read back from CTest rather than inferred.

This file writes out no private path. The marker comes from the checker
script, the other private files from the EXISTS guards of tests/CMakeLists.txt,
and the file a SKIP line names from the block itself.

The test never skips: without cmake and ctest it fails. Its CTest row passes
both on the command line, with the generator of the build tree; run by hand,
the two are taken from PATH.
"""
from __future__ import annotations

import argparse
import ast
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKELISTS = ROOT / 'tests' / 'CMakeLists.txt'
TWO_WORLD_CHECK = ROOT / 'scripts' / 'check_two_world_registration.py'
REMOVED_PATHS = ROOT / 'tests' / 'private_fixture_paths.txt'

# The rows this rule covers: pinned private-fixtures rows that read no private
# file. A row added here before its block follows the marker fails the first
# test below, for that row.
ROWS = (
    'test_carried_pooc_short_margin_state_l4a',
    'test_l4b_policy_regressions',
    'test_l9a_carried_pooc_competing_checkpoint',
    'test_trail_activation_tick_bar_l4c',
)
# A row that follows the marker too and has a registration test of its own.
ROWS_WITH_THEIR_OWN_TEST = ('test_publication_witness',)
SKIP_LITERAL = 'SKIP private fixtures not present: '
SKIP_EXPRESSION = '^' + SKIP_LITERAL
PRIVATE_LABEL = 'private_fixtures'
STAND_IN = 'pineforge_add_unit_test stand-in'
# A file an EXISTS guard of tests/CMakeLists.txt names, relative to tests/. A
# guard whose path carries a variable reference is not read.
GUARDED_FILE = re.compile(r'EXISTS "\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"$]+)"')
# The line that opens a private row's registration block, up to the file its
# guard tests: it names the row the block registers.
BLOCK_OPENING = re.compile(r'if\(NOT (\w+) IN_LIST _pf_kernel_absent_rows AND EXISTS "')
# A single-configuration generator ignores the choice; a multi-configuration
# one needs it to list and run a row.
CONFIGURATION = ('-C', 'Debug')

TOP_LEVEL = '''cmake_minimum_required(VERSION 3.16)
project(private_marker_rows_registration_probe NONE)
enable_testing()
add_subdirectory(tests)
'''
# What a block reads from the rest of tests/CMakeLists.txt, as stand-ins.
PRELUDE = '''set(_pf_kernel_absent_rows @KERNEL_ABSENT@)
function(pineforge_add_unit_test test_name)
    add_test(NAME ${test_name} COMMAND ${CMAKE_COMMAND} -E echo "@STAND_IN@: ${test_name}")
endfunction()
'''

TOOLS = argparse.Namespace(cmake=None, ctest=None, generator=None,
                           make_program=None, work_dir=None)


def run(command, cwd) -> subprocess.CompletedProcess:
    return subprocess.run([str(word) for word in command], cwd=str(cwd),
                          capture_output=True, text=True, errors='replace')


def overlay_marker() -> str:
    """The repo-relative file whose presence means the private fixtures are in
    the tree: OVERLAY_MARKER of scripts/check_two_world_registration.py, read
    from that script's own text."""
    module = ast.parse(TWO_WORLD_CHECK.read_text(encoding='utf-8'))
    values = [node.value for node in module.body
              if isinstance(node, ast.Assign)
              and any(isinstance(target, ast.Name) and target.id == 'OVERLAY_MARKER'
                      for target in node.targets)]
    if len(values) != 1:
        raise AssertionError(
            f'{TWO_WORLD_CHECK}: expected one OVERLAY_MARKER assignment, '
            f'found {len(values)}')
    marker = ast.literal_eval(values[0])
    if not isinstance(marker, str) or not marker.startswith('tests/'):
        raise AssertionError(
            f'{TWO_WORLD_CHECK}: OVERLAY_MARKER is not a path under tests/: {marker!r}')
    return marker


def removed_files() -> set[str]:
    """Every file the private-fixture removals took out of the public tree
    (the F lines of the pin): what a SKIP line may name."""
    files = {line[2:].strip()
             for line in REMOVED_PATHS.read_text(encoding='utf-8').splitlines()
             if line.startswith('F ')}
    if not files:
        raise AssertionError(f'{REMOVED_PATHS}: no removed file is pinned')
    return files


def guarded_private_files(removed: set[str]) -> set[str]:
    """Every removed file that an EXISTS guard of tests/CMakeLists.txt names:
    the private files a registration block of that file can key on."""
    text = CMAKELISTS.read_text(encoding='utf-8')
    return {'tests/' + path for path in GUARDED_FILE.findall(text)} & removed


def marker_guards(marker: str) -> list:
    """(line number, row) for every line of tests/CMakeLists.txt whose EXISTS
    guard tests the marker. The row is None where the line does not open a
    private row's registration block."""
    relative = marker[len('tests/'):]
    found = []
    lines = CMAKELISTS.read_text(encoding='utf-8').splitlines()
    for number, line in enumerate(lines, 1):
        if line.lstrip().startswith('#') or relative not in GUARDED_FILE.findall(line):
            continue
        opening = BLOCK_OPENING.match(line)
        found.append((number, opening.group(1) if opening else None))
    return found


def row_block(row: str) -> str:
    """The row's registration block, cut out of tests/CMakeLists.txt as it stands."""
    guard = 'if(NOT ' + row + ' IN_LIST _pf_kernel_absent_rows'
    lines = CMAKELISTS.read_text(encoding='utf-8').splitlines()
    starts = [index for index, line in enumerate(lines) if line.startswith(guard)]
    if len(starts) != 1:
        raise AssertionError(
            f'{CMAKELISTS}: expected one block opening with {guard!r}, '
            f'found {len(starts)}')
    depth = 0
    for index in range(starts[0], len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith('if('):
            depth += 1
        elif stripped.startswith('endif('):
            depth -= 1
            if depth == 0:
                return '\n'.join(lines[starts[0]:index + 1]) + '\n'
    raise AssertionError(f'{CMAKELISTS}:{starts[0] + 1}: the block never closes')


def own_input(row: str) -> str:
    """The row's own input, beside tests/CMakeLists.txt: its translation unit."""
    return 'tests/' + row + '.cpp'


def real_unit_test(row: str) -> list:
    """What the stand-in for pineforge_add_unit_test registers for the row,
    after the cmake executable."""
    return ['-E', 'echo', STAND_IN + ': ' + row]


def result_line(row: str):
    """CTest's result line: the row name, a run of dots, then the verdict."""
    return re.compile(
        re.escape(row) + r' \.+\s*(?:\*\*\*)?(Passed|Skipped|Failed|Not Run|Exception|Timeout)')


def as_list(value) -> list:
    if value is None:
        return []
    return list(value) if isinstance(value, (list, tuple)) else [value]


def labels(row: dict) -> list:
    return as_list(row['properties'].get('LABELS'))


def describe(row: dict, verdict: str, text: str) -> str:
    return (f'  command: {row["command"]}\n'
            f'  properties: {row["properties"]}\n'
            f'  CTest verdict: {verdict}\n'
            f'  CTest output:\n{text}')


class Probe:
    """One scratch tree in one state, configured once, and what CTest says of it."""

    def __init__(self, directory: Path, row: str, present=(), kernel_absent: bool = False):
        self.row = row
        self.source = directory / 'tree'
        self.build = directory / 'build'
        tests = self.source / 'tests'
        tests.mkdir(parents=True)
        (self.source / 'CMakeLists.txt').write_text(TOP_LEVEL)
        prelude = (PRELUDE.replace('@KERNEL_ABSENT@', row if kernel_absent else '')
                   .replace('@STAND_IN@', STAND_IN))
        (tests / 'CMakeLists.txt').write_text(prelude + row_block(row))
        for relative in present:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('stand-in: only the existence of this file is asked\n')
        command = [TOOLS.cmake, '-S', self.source, '-B', self.build]
        if TOOLS.generator:
            command += ['-G', TOOLS.generator]
        if TOOLS.make_program:
            command.append('-DCMAKE_MAKE_PROGRAM=' + TOOLS.make_program)
        configured = run(command, self.source)
        if configured.returncode != 0:
            raise AssertionError('the scratch project did not configure:\n'
                                 + configured.stdout + configured.stderr)

    def rows(self) -> dict:
        """name -> {'command': [...], 'properties': {...}} for every listed row."""
        shown = run([TOOLS.ctest, '--show-only=json-v1', *CONFIGURATION], self.build)
        if shown.returncode != 0:
            raise AssertionError('CTest could not list the scratch project:\n'
                                 + shown.stdout + shown.stderr)
        listed = {}
        # A tree that registers no row may print an empty listing.
        payload = shown.stdout.strip()
        for test in (json.loads(payload).get('tests', []) if payload else []):
            properties = {entry['name']: entry['value']
                          for entry in test.get('properties', [])}
            listed[test['name']] = {'command': list(test.get('command', [])),
                                    'properties': properties}
        return listed

    def verdict(self) -> tuple:
        """(CTest's verdict for the row, everything CTest printed)."""
        ran = run([TOOLS.ctest, '-V', *CONFIGURATION], self.build)
        text = ran.stdout + ran.stderr
        found = result_line(self.row).search(text)
        if found is None:
            raise AssertionError(f'CTest printed no result line for {self.row}:\n{text}')
        return found.group(1), text


class PrivateMarkerRowsRegistration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TOOLS.cmake = TOOLS.cmake or shutil.which('cmake')
        TOOLS.ctest = TOOLS.ctest or shutil.which('ctest')
        if not TOOLS.cmake or not TOOLS.ctest:
            raise AssertionError(
                'this test needs cmake and ctest: pass --cmake and --ctest, or '
                'put both on PATH. It never skips.')
        cls.marker = overlay_marker()
        cls.removed = removed_files()
        # Every other private file an EXISTS guard of tests/CMakeLists.txt names.
        cls.others = tuple(sorted(guarded_private_files(cls.removed) - {cls.marker}))

    def setUp(self):
        base = None
        if TOOLS.work_dir:
            base = Path(TOOLS.work_dir)
            base.mkdir(parents=True, exist_ok=True)
        self._scratch = tempfile.TemporaryDirectory(prefix='state-', dir=base)
        self.addCleanup(self._scratch.cleanup)
        self._probes = 0

    def probe(self, row: str, private=(), kernel_absent: bool = False) -> Probe:
        """A scratch tree that holds the row's translation unit and the named
        private files. Each private file is a removed file of the pin, so a
        stand-in is written only where the private fixtures would put a file,
        inside the scratch tree."""
        for path in private:
            # assertTrue, not assertIn: a failure must not print the whole pin.
            self.assertTrue(path in self.removed,
                            f'{path} is not a removed file of {REMOVED_PATHS.name}')
            self.assertFalse(Path(path).is_absolute() or '..' in Path(path).parts,
                             f'{path} would leave the scratch tree')
        self._probes += 1
        return Probe(Path(self._scratch.name) / f'probe-{self._probes}', row,
                     (own_input(row), *private), kernel_absent)

    def registered(self, probe: Probe) -> dict:
        row = probe.rows().get(probe.row)
        self.assertIsNotNone(row, f'{probe.row} is not registered at all')
        return row

    def test_the_private_fixture_marker_alone_registers_the_real_unit_test(self):
        # The state the earlier registration lost: the private fixtures are in
        # the tree (their marker is there) and no other private file is. The
        # row must be the real unit test, not the SKIP placeholder.
        for name in ROWS:
            with self.subTest(row=name):
                probe = self.probe(name, private=(self.marker,))
                row = self.registered(probe)
                verdict, text = probe.verdict()
                self.assertEqual(
                    real_unit_test(name), row['command'][1:],
                    'the private fixtures are present, so the block must '
                    f'register {name} through pineforge_add_unit_test; it '
                    'registered instead:\n' + describe(row, verdict, text))
                self.assertEqual('Passed', verdict, text)
                self.assertNotIn(PRIVATE_LABEL, labels(row),
                                 'the real unit test must not carry the placeholder label')

    def test_the_marker_with_every_other_guarded_private_file_registers_the_real_unit_test(self):
        # The same with every other private file an EXISTS guard of
        # tests/CMakeLists.txt names in the tree as well. Whichever private
        # file a block keys on, it is present here, so this state registers
        # the real unit test whether the block follows the marker or another
        # private file: more private files never turn a row into a placeholder.
        self.assertTrue(
            self.others,
            'no EXISTS guard of tests/CMakeLists.txt names a private file other '
            'than the marker, so this state would repeat the previous test')
        for name in ROWS:
            with self.subTest(row=name):
                probe = self.probe(name, private=(self.marker, *self.others))
                row = self.registered(probe)
                verdict, text = probe.verdict()
                self.assertEqual(
                    real_unit_test(name), row['command'][1:],
                    'the marker and every other guarded private file are '
                    f'present, yet {name} is not the real unit test:\n'
                    + describe(row, verdict, text))
                self.assertEqual('Passed', verdict, text)
                self.assertNotIn(PRIVATE_LABEL, labels(row),
                                 'the real unit test must not carry the placeholder label')

    def test_private_fixtures_absent_keep_the_pinned_skip_placeholder(self):
        # Public CI: no private file at all, the row's translation unit present.
        for name in ROWS:
            with self.subTest(row=name):
                probe = self.probe(name)
                row = self.registered(probe)
                verdict, text = probe.verdict()
                command = row['command']
                self.assertEqual(4, len(command), describe(row, verdict, text))
                self.assertEqual(['-E', 'echo'], command[1:3], describe(row, verdict, text))
                self.assertTrue(command[3].startswith(SKIP_LITERAL),
                                describe(row, verdict, text))
                self.assertTrue(
                    command[3][len(SKIP_LITERAL):] in self.removed,
                    'the SKIP line must name a file the private-fixture removals '
                    f'took out of the public tree ({REMOVED_PATHS.name}, F lines):\n'
                    + describe(row, verdict, text))
                self.assertEqual([PRIVATE_LABEL], labels(row))
                # An echo exits 0, so only the skip expression can make CTest
                # answer Skipped. Where the listing also prints the
                # expression, it is the pinned one.
                self.assertEqual('Skipped', verdict, text)
                expression = row['properties'].get('SKIP_REGULAR_EXPRESSION')
                if expression is not None:
                    self.assertEqual([SKIP_EXPRESSION], as_list(expression))

    def test_the_skip_line_names_the_file_whose_presence_registers_the_row(self):
        # The placeholder says which file is not present. Putting exactly that
        # file into the tree must register the real unit test: a SKIP line that
        # names one file while another decides the row tells the reader nothing.
        for name in ROWS:
            with self.subTest(row=name):
                absent = self.probe(name)
                command = self.registered(absent)['command']
                self.assertTrue(command and command[-1].startswith(SKIP_LITERAL), command)
                named = command[-1][len(SKIP_LITERAL):]
                present = self.probe(name, private=(named,))
                row = self.registered(present)
                verdict, text = present.verdict()
                self.assertEqual(
                    real_unit_test(name), row['command'][1:],
                    f'{named} is present, yet {name} is not the real unit test:\n'
                    + describe(row, verdict, text))

    def test_a_profile_without_the_source_layer_registers_no_row(self):
        # A profile that does not build the source layer lists a row whose
        # translation unit reaches that layer as absent: no target, no row and
        # no placeholder, whatever the tree holds.
        states = (
            ('no private file', ()),
            ('the marker alone', (self.marker,)),
            ('the marker and every other guarded private file',
             (self.marker, *self.others)),
        )
        for name in ROWS:
            for state, private in states:
                with self.subTest(row=name, state=state):
                    probe = self.probe(name, private=private, kernel_absent=True)
                    self.assertEqual({}, probe.rows())

    def test_every_block_that_tests_the_marker_belongs_to_a_listed_row(self):
        # A block that tests the marker claims this rule for its row. The row
        # is then one of ROWS, whose states the tests above check, or a row
        # with a registration test of its own. A row keyed on the marker and
        # listed nowhere would follow the rule unchecked.
        listed = set(ROWS) | set(ROWS_WITH_THEIR_OWN_TEST)
        for number, row in marker_guards(self.marker):
            with self.subTest(line=number, row=row):
                self.assertIsNotNone(
                    row,
                    f'{CMAKELISTS}:{number} tests the marker in a line that does '
                    'not open a private row\'s block, so this test cannot say '
                    'which row it registers')
                self.assertTrue(
                    row in listed,
                    f'{CMAKELISTS}:{number} registers {row} on the marker, and no '
                    'registration test lists that row. Once it is confirmed to '
                    'read no private file, add it to ROWS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    for option in ('--cmake', '--ctest', '--generator', '--make-program', '--work-dir'):
        parser.add_argument(option)
    known, rest = parser.parse_known_args()
    TOOLS.cmake, TOOLS.ctest = known.cmake, known.ctest
    TOOLS.generator, TOOLS.make_program = known.generator, known.make_program
    TOOLS.work_dir = known.work_dir
    unittest.main(argv=[sys.argv[0], *rest])
