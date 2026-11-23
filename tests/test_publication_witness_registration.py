#!/usr/bin/env python3
"""The registration rule of test_publication_witness, read back through CMake and CTest.

Why this exists
---------------
test_publication_witness reads no private fixture. It is built from two files
of this tree: its translation unit and the pinned table that unit includes.
Its registration nevertheless keyed on one private fixture file the witness
never reads. A tree that held the private fixtures but not that one file
registered the pinned SKIP placeholder, so the witness did not run and nothing
said so.

The rule this test pins
-----------------------
The row stays a pinned private-fixtures row (tests/private_fixture_rows.txt),
and its registration block in tests/CMakeLists.txt answers three states:

* private fixtures absent: the pinned SKIP placeholder, as for every such row
  (the literal, the private_fixtures label, the skip expression);
* private fixtures present and both of the witness's own inputs present: the
  real witness, registered through pineforge_add_unit_test;
* private fixtures present and one of its own inputs missing: a row that
  FAILS and prints PF_PUBLICATION_WITNESS_INPUT_MISSING with the missing
  file. It is never a skip and never a pass.

"Private fixtures present" means that the marker file of
scripts/check_two_world_registration.py (its OVERLAY_MARKER) exists. This test
reads the marker out of that script, so the two cannot disagree. A profile
that does not build the source layer registers no row at all, in any state.

How it is checked
-----------------
The row's block is cut out of tests/CMakeLists.txt as it stands and configured
by the real CMake inside a scratch project, once per state. The scratch tree
holds only the files the state names, as stand-ins: the test asks which row is
registered, and never builds or runs the witness itself. The real CTest then
lists the row and runs it. pineforge_add_unit_test is replaced by a stand-in
that registers a row printing a known line, so "the real witness was
registered" is read back from CTest rather than inferred.

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

ROW = 'test_publication_witness'
# The witness's own inputs, beside tests/CMakeLists.txt: its translation unit
# and the pinned table that unit includes.
OWN_INPUTS = ('test_publication_witness.cpp', 'test_publication_witness_pinned.inc')
# The driver the failing row runs. The scratch tree needs its own copy.
DRIVER = 'publication_witness_input_missing.cmake'
CODE = 'PF_PUBLICATION_WITNESS_INPUT_MISSING'
SKIP_LITERAL = 'SKIP private fixtures not present: '
SKIP_EXPRESSION = '^' + SKIP_LITERAL
PRIVATE_LABEL = 'private_fixtures'
GUARD = 'if(NOT ' + ROW + ' IN_LIST _pf_kernel_absent_rows'
STAND_IN = 'pineforge_add_unit_test stand-in'
# Properties that replace or invert the exit status of a row's own command.
OVERRIDING = ('SKIP_REGULAR_EXPRESSION', 'SKIP_RETURN_CODE',
              'PASS_REGULAR_EXPRESSION', 'WILL_FAIL', 'DISABLED')
# CTest's result line: the row name, a run of dots, then the verdict.
RESULT_LINE = re.compile(
    re.escape(ROW) + r' \.+\s*(?:\*\*\*)?(Passed|Skipped|Failed|Not Run|Exception|Timeout)')
# A single-configuration generator ignores the choice; a multi-configuration
# one needs it to list and run a row.
CONFIGURATION = ('-C', 'Debug')

TOP_LEVEL = '''cmake_minimum_required(VERSION 3.16)
project(publication_witness_registration_probe NONE)
enable_testing()
add_subdirectory(tests)
'''
# What the block reads from the rest of tests/CMakeLists.txt, as stand-ins.
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


def witness_block() -> str:
    """The row's registration block, cut out of tests/CMakeLists.txt as it stands."""
    lines = CMAKELISTS.read_text(encoding='utf-8').splitlines()
    starts = [index for index, line in enumerate(lines) if line.startswith(GUARD)]
    if len(starts) != 1:
        raise AssertionError(
            f'{CMAKELISTS}: expected one block opening with {GUARD!r}, '
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

    def __init__(self, directory: Path, present=(), kernel_absent: bool = False):
        self.source = directory / 'tree'
        self.build = directory / 'build'
        tests = self.source / 'tests'
        tests.mkdir(parents=True)
        (self.source / 'CMakeLists.txt').write_text(TOP_LEVEL)
        prelude = (PRELUDE.replace('@KERNEL_ABSENT@', ROW if kernel_absent else '')
                   .replace('@STAND_IN@', STAND_IN))
        (tests / 'CMakeLists.txt').write_text(prelude + witness_block())
        driver = ROOT / 'tests' / DRIVER
        if driver.is_file():
            shutil.copyfile(driver, tests / DRIVER)
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
        found = RESULT_LINE.search(text)
        if found is None:
            raise AssertionError(f'CTest printed no result line for {ROW}:\n{text}')
        return found.group(1), text


class PublicationWitnessRegistration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TOOLS.cmake = TOOLS.cmake or shutil.which('cmake')
        TOOLS.ctest = TOOLS.ctest or shutil.which('ctest')
        if not TOOLS.cmake or not TOOLS.ctest:
            raise AssertionError(
                'this test needs cmake and ctest: pass --cmake and --ctest, or '
                'put both on PATH. It never skips.')
        cls.marker = overlay_marker()
        cls.own_inputs = tuple('tests/' + name for name in OWN_INPUTS)

    def setUp(self):
        base = None
        if TOOLS.work_dir:
            base = Path(TOOLS.work_dir)
            base.mkdir(parents=True, exist_ok=True)
        self._scratch = tempfile.TemporaryDirectory(prefix='state-', dir=base)
        self.addCleanup(self._scratch.cleanup)
        self._probes = 0

    def probe(self, present=(), kernel_absent: bool = False) -> Probe:
        self._probes += 1
        return Probe(Path(self._scratch.name) / f'probe-{self._probes}',
                     present, kernel_absent)

    def registered(self, probe: Probe) -> dict:
        row = probe.rows().get(ROW)
        self.assertIsNotNone(row, f'{ROW} is not registered at all')
        return row

    def test_private_fixtures_and_own_inputs_present_register_the_real_witness(self):
        # The state the earlier registration lost: the private fixtures are in
        # the tree (their marker is there), both inputs of the witness are
        # there, and no other private file is. The row must be the real unit
        # test, not the SKIP placeholder.
        probe = self.probe(present=(self.marker, *self.own_inputs))
        row = self.registered(probe)
        verdict, text = probe.verdict()
        self.assertEqual(
            ['-E', 'echo', STAND_IN + ': ' + ROW], row['command'][1:],
            'the private fixtures and both inputs of the witness are present, '
            'so the block must register the witness through '
            'pineforge_add_unit_test; it registered instead:\n'
            + describe(row, verdict, text))
        self.assertEqual('Passed', verdict, text)
        self.assertNotIn(PRIVATE_LABEL, labels(row),
                         'the real witness must not carry the placeholder label')

    def test_private_fixtures_present_and_an_own_input_missing_register_a_failing_row(self):
        cases = (
            ('the pinned table', (self.own_inputs[1],)),
            ('the translation unit', (self.own_inputs[0],)),
            ('both inputs', self.own_inputs),
        )
        for name, missing in cases:
            with self.subTest(missing=name):
                present = (self.marker, *(path for path in self.own_inputs
                                          if path not in missing))
                probe = self.probe(present=present)
                row = self.registered(probe)
                verdict, text = probe.verdict()
                self.assertEqual(
                    'Failed', verdict,
                    f'the private fixtures are present and {name} of the witness '
                    f'is missing, so the row must fail with {CODE}; instead:\n'
                    + describe(row, verdict, text))
                named = set(re.findall(re.escape(CODE) + r': (\S+)', text))
                self.assertTrue(named, f'the failing row did not print {CODE}:\n{text}')
                self.assertLessEqual(
                    named, set(missing),
                    f'{CODE} named a file that is not missing:\n{text}')
                if len(missing) == 1:
                    self.assertEqual(set(missing), named, text)
                for overriding in OVERRIDING:
                    self.assertFalse(
                        row['properties'].get(overriding),
                        f'{overriding} would replace the failing row\'s verdict')
                self.assertNotIn(PRIVATE_LABEL, labels(row),
                                 'a failing row must not carry the placeholder label')
                # The command itself fails: the verdict rests on its exit
                # status, not on a CTest property.
                direct = run(row['command'], probe.build)
                self.assertNotEqual(
                    0, direct.returncode,
                    'the failing row\'s own command exited 0:\n'
                    + direct.stdout + direct.stderr)

    def test_private_fixtures_absent_keep_the_pinned_skip_placeholder(self):
        # Public CI: no private file at all, the witness's own inputs present.
        probe = self.probe(present=self.own_inputs)
        row = self.registered(probe)
        verdict, text = probe.verdict()
        command = row['command']
        self.assertEqual(4, len(command), describe(row, verdict, text))
        self.assertEqual(['-E', 'echo'], command[1:3], describe(row, verdict, text))
        self.assertTrue(command[3].startswith(SKIP_LITERAL), describe(row, verdict, text))
        self.assertIn(
            command[3][len(SKIP_LITERAL):], removed_files(),
            'the SKIP line must name a file the private-fixture removals took '
            f'out of the public tree ({REMOVED_PATHS.name}, F lines)')
        self.assertEqual([PRIVATE_LABEL], labels(row))
        # An echo exits 0, so only the skip expression can make CTest answer
        # Skipped. Where the listing also prints the expression, it is the
        # pinned one.
        self.assertEqual('Skipped', verdict, text)
        expression = row['properties'].get('SKIP_REGULAR_EXPRESSION')
        if expression is not None:
            self.assertEqual([SKIP_EXPRESSION], as_list(expression))

    def test_the_skip_line_names_the_file_whose_presence_registers_the_witness(self):
        # The placeholder says which file is not present. Putting exactly that
        # file into the tree must register the real witness: a SKIP line that
        # names one file while another decides the row tells the reader nothing.
        absent = self.probe(present=self.own_inputs)
        command = self.registered(absent)['command']
        self.assertTrue(command and command[-1].startswith(SKIP_LITERAL), command)
        named = command[-1][len(SKIP_LITERAL):]
        present = self.probe(present=(named, *self.own_inputs))
        row = self.registered(present)
        verdict, text = present.verdict()
        self.assertEqual(
            ['-E', 'echo', STAND_IN + ': ' + ROW], row['command'][1:],
            f'{named} is present and so are the witness\'s inputs, yet the row is '
            'not the real witness:\n' + describe(row, verdict, text))

    def test_a_profile_without_the_source_layer_registers_no_row(self):
        # The witness's translation unit reaches the source layer, so a profile
        # that does not build that layer lists the row as absent: no target, no
        # row, no placeholder and no failing row, whatever the tree holds.
        states = (
            ('no private file', self.own_inputs),
            ('private fixtures and both inputs', (self.marker, *self.own_inputs)),
            ('private fixtures and no input', (self.marker,)),
        )
        for name, present in states:
            with self.subTest(state=name):
                probe = self.probe(present=present, kernel_absent=True)
                self.assertEqual({}, probe.rows())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    for option in ('--cmake', '--ctest', '--generator', '--make-program', '--work-dir'):
        parser.add_argument(option)
    known, rest = parser.parse_known_args()
    TOOLS.cmake, TOOLS.ctest = known.cmake, known.ctest
    TOOLS.generator, TOOLS.make_program = known.generator, known.make_program
    TOOLS.work_dir = known.work_dir
    unittest.main(argv=[sys.argv[0], *rest])
