#!/usr/bin/env python3
"""Tests for check_abi_receipt_skips.py, with a real ctest over hand-written rows.

The receipt-gated rows are stand-ins shaped like the real ones (the same flags,
SKIP_RETURN_CODE 77, a command that exits 77 when an input receipt is missing),
so the gap the script exists for is reproduced here: ctest prints "100% tests
passed" for a run in which a gated row skipped.
"""
from __future__ import annotations

import argparse
import contextlib
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import check_abi_receipt_skips as skips
from ci_verify import ctest_rows
from prepare_settlement_cpp_abi_base import PROVIDERS

# The ctest the CTest row passes with --ctest (CMAKE_CTEST_COMMAND), else PATH's.
CTEST = shutil.which('ctest')
# A stand-in for a gated checker: exit 77 when a named input receipt is absent.
CHECKER = ('import sys, os; a = sys.argv[1:]; '
           'sys.exit(77 if any(f.endswith("-receipt") and f != "--receipt" '
           'and not os.path.exists(v) for f, v in zip(a, a[1:])) else 0)')


def cmake_quote(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


class GatedRows(unittest.TestCase):
    def test_a_row_is_gated_by_its_receipt_mode_and_reads_only_its_input_receipts(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            present = build / 'native-abi-v16-frozen' / 'receipt.json'
            present.parent.mkdir()
            present.write_text('{}')
            absent = build / 'settlement-abi-base' / 'receipt.json'
            tests = [
                {'name': 'plain', 'command': ['python3', 'check.py', '--receipt', str(absent)]},
                {'name': 'skipping', 'command': [
                    'python3', 'check.py', '--v16-frozen-receipt', str(present),
                    '--base-receipt', str(absent),
                    '--receipt', str(build / 'own-output-receipt.json'),
                    '--skip-if-receipt-missing']},
                {'name': 'required', 'command': [
                    'python3', 'budget.py', '--compile-commands',
                    str(build / 'compile_commands.json'),
                    '--v16-frozen-receipt', str(present), '--require-receipts']},
            ]
            rows = skips.gated_rows(tests)
        self.assertEqual([row.name for row in rows], ['skipping', 'required'])
        skipping, required = rows
        # --receipt is the row's own output, never an input.
        self.assertEqual((skipping.mode, skipping.missing, skipping.database),
                         ('skip', (absent,), None))
        self.assertEqual(skipping.verdict, 'skips')
        self.assertEqual((required.mode, required.missing), ('require', ()))
        self.assertEqual(required.database, build / 'compile_commands.json')
        self.assertEqual(required.verdict, 'fails')

    def test_a_missing_receipt_fails_a_row_registered_to_require_it(self):
        row = skips.GatedRow('required', 'require', (Path('/nowhere/receipt.json'),))
        self.assertEqual(row.verdict, 'fails')
        self.assertEqual(skips.GatedRow('ready', 'skip', ()).verdict, 'runs')


class AgainstRealCTest(unittest.TestCase):
    """Rows written to a CTestTestfile.cmake and listed by the real ctest."""

    # The gated rows build_tree registers, in the order ctest lists them.
    GATED = ('test_gated_pair', 'test_gated_settlement', 'test_gated_budget')

    @classmethod
    def setUpClass(cls):
        if not CTEST:
            raise unittest.SkipTest('ctest not found')

    def build_tree(self, *, export_compile_commands: bool) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        build = Path(temporary.name).resolve()
        (build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=/nowhere\n')
        if export_compile_commands:
            (build / 'compile_commands.json').write_text('[]\n')
        v16 = build / 'native-abi-v16-frozen' / 'receipt.json'
        base = build / 'settlement-abi-base' / 'receipt.json'
        rows = {
            'test_plain_python_row': [sys.executable, '-c', 'pass'],
            'test_gated_pair': [sys.executable, '-c', CHECKER, '--v16-frozen-receipt', str(v16),
                                '--receipt', str(build / 'pair-receipt.json'),
                                '--skip-if-receipt-missing'],
            'test_gated_settlement': [sys.executable, '-c', CHECKER, '--base-receipt', str(base),
                                      '--v16-frozen-receipt', str(v16),
                                      '--skip-if-receipt-missing'],
            'test_gated_budget': [sys.executable, '-c', CHECKER,
                                  '--compile-commands', str(build / 'compile_commands.json'),
                                  '--v16-frozen-receipt', str(v16), '--skip-if-receipt-missing'],
        }
        lines = []
        for name, command in rows.items():
            lines.append(f'add_test({name} ' + ' '.join(map(cmake_quote, command)) + ')')
            if name != 'test_plain_python_row':
                lines.append(f'set_tests_properties({name} PROPERTIES SKIP_RETURN_CODE "77")')
        (build / 'CTestTestfile.cmake').write_text('\n'.join(lines) + '\n')
        return build

    @staticmethod
    def receipts(build: Path) -> None:
        for directory in ('native-abi-v16-frozen', 'settlement-abi-base'):
            (build / directory).mkdir(exist_ok=True)
            (build / directory / 'receipt.json').write_text('{}\n')

    @staticmethod
    def check(build: Path, *extra: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = skips.main(['--build-dir', str(build), '--ctest', CTEST, *extra])
        return code, out.getvalue()

    def test_ctest_passes_a_run_in_which_gated_rows_skipped_and_the_check_names_them(self):
        build = self.build_tree(export_compile_commands=True)
        run = subprocess.run([CTEST, '--test-dir', str(build)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout)
        # The summary wording is CMake's, not this tree's: 3.28 always spells
        # the failed count, 4.x drops the clause when nothing failed (see
        # CTestSummarySpelling below). Read the counts with ci_verify's parser,
        # the one the CI floor reads the same line with, rather than matching
        # one host's wording.
        # expectation corrected: literal '100% tests passed out of 4' ->
        # ctest_rows() counts, because that string is the CMake >= 4 wording
        # only and ubuntu-24.04 CI runs CMake 3.28.
        rows = ctest_rows(run.stdout.encode())
        self.assertEqual((rows.total, rows.ran), (4, 1), run.stdout)
        self.assertEqual(rows.skipped, self.GATED, run.stdout)
        self.assertIn('test_gated_pair (Skipped)', run.stdout)
        code, out = self.check(build)
        self.assertEqual(code, 1, out)
        self.assertIn('registers 3 receipt-gated CTest rows', out)
        for name in ('test_gated_pair', 'test_gated_settlement', 'test_gated_budget'):
            self.assertRegex(out, rf'\n  skips +{name} +receipts? missing: ')
        self.assertIn('settlement-abi-base/receipt.json, native-abi-v16-frozen/receipt.json', out)
        self.assertNotIn('test_plain_python_row', out)
        self.assertNotIn('pair-receipt.json', out)
        self.assertIn('3 of 3 receipt-gated rows do not run', out)
        self.assertIn('--prepare', out)

    def test_with_every_receipt_present_every_gated_row_runs(self):
        build = self.build_tree(export_compile_commands=True)
        self.receipts(build)
        code, out = self.check(build)
        self.assertEqual(code, 0, out)
        self.assertIn('every receipt they read is present: all 3 run', out)
        run = subprocess.run([CTEST, '--test-dir', str(build)], capture_output=True, text=True)
        self.assertNotIn('Skipped', run.stdout)

    def test_a_tree_without_a_compile_database_fails_the_budget_row_once_it_runs(self):
        # test_l4g_runtime_budget reads compile_commands.json after its receipt
        # check: preparing the providers in a tree configured without
        # CMAKE_EXPORT_COMPILE_COMMANDS=ON turns its skip into a failure.
        build = self.build_tree(export_compile_commands=False)
        self.receipts(build)
        code, out = self.check(build)
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r'\n  fails +test_gated_budget +compile database missing: '
                              r'compile_commands\.json')
        self.assertIn('-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', out)
        self.assertNotIn('--prepare', out)

    def test_a_tree_without_gated_rows_passes(self):
        build = self.build_tree(export_compile_commands=True)
        (build / 'CTestTestfile.cmake').write_text(
            f'add_test(test_plain_python_row {cmake_quote(sys.executable)} "-c" "pass")\n')
        code, out = self.check(build)
        self.assertEqual(code, 0, out)
        self.assertIn('registers no receipt-gated CTest row', out)

    def test_a_directory_that_is_no_build_tree_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = skips.main(['--build-dir', temporary, '--ctest', CTEST])
        self.assertEqual(code, 2)
        self.assertIn('not a configured CMake build tree', err.getvalue())


class CTestSummarySpelling(unittest.TestCase):
    """Both wordings CTest prints for the summary the row above reads.

    Verbatim runs of the four rows build_tree registers, three of them
    skipped, on the two CMake generations in use: 3.28.3 is ubuntu-24.04's
    apt cmake, which the kernel-only CI job runs; 4.4.0 is the local macOS
    one. Only the summary line differs, and a row that matched one of them
    literally passed on that host alone -- so the fixtures keep both readable
    from either host.
    """

    ROWS = ('Test project BUILD\n'
            '    Start 1: test_plain_python_row\n'
            '1/4 Test #1: test_plain_python_row ............   Passed    0.01 sec\n'
            '    Start 2: test_gated_pair\n'
            '2/4 Test #2: test_gated_pair ..................***Skipped   0.01 sec\n'
            '    Start 3: test_gated_settlement\n'
            '3/4 Test #3: test_gated_settlement ............***Skipped   0.01 sec\n'
            '    Start 4: test_gated_budget\n'
            '4/4 Test #4: test_gated_budget ................***Skipped   0.01 sec\n')
    DID_NOT_RUN = ('\nTotal Test time (real) =   0.04 sec\n'
                   '\nThe following tests did not run:\n'
                   '\t  2 - test_gated_pair (Skipped)\n'
                   '\t  3 - test_gated_settlement (Skipped)\n'
                   '\t  4 - test_gated_budget (Skipped)\n')
    SUMMARIES = {
        'cmake 3.28.3 (ubuntu-24.04)': '\n100% tests passed, 0 tests failed out of 4\n',
        'cmake 4.4.0 (macOS)': '\n100% tests passed out of 4\n',
    }

    def test_either_wording_reads_as_four_rows_of_which_the_three_gated_skipped(self):
        for spelling, summary in self.SUMMARIES.items():
            with self.subTest(spelling):
                rows = ctest_rows((self.ROWS + summary + self.DID_NOT_RUN).encode())
                self.assertEqual((rows.total, rows.ran), (4, 1))
                self.assertEqual(rows.skipped, AgainstRealCTest.GATED)
                self.assertEqual((rows.not_run, rows.disabled), ((), ()))


class Preparation(unittest.TestCase):
    """--prepare: the six providers, in ci_verify's order, never over an existing one."""

    def plan(self, build: Path, *, missing_objects=()) -> tuple[bool, list[list[str]], str]:
        calls = []

        def run(argv, **kwargs):
            calls.append(list(argv))
            if argv[:3] == ['git', '-C', str(skips.ROOT)] and 'cat-file' in argv:
                missing = any(commit + '^{commit}' in argv for commit in missing_objects)
                return subprocess.CompletedProcess(argv, 1 if missing else 0)
            return subprocess.CompletedProcess(argv, 0)

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ok = skips.prepare(skips.ROOT, build, 3, run=run)
        return ok, calls, out.getvalue()

    def test_every_missing_provider_is_prepared_with_ci_verify_argv(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            ok, calls, _ = self.plan(build)
        self.assertTrue(ok)
        prepares = [argv for argv in calls if 'cat-file' not in argv]
        self.assertEqual(prepares, [skips.prepare_argv(skips.ROOT, build, role, 3)
                                    for role in skips.PROVIDER_ROLES])
        self.assertEqual([Path(argv[argv.index('--output') + 1]).name for argv in prepares],
                         ['settlement-abi-base', 'settlement-abi-prior', 'native-abi-v13',
                          'native-abi-v14', 'native-abi-v15-frozen', 'native-abi-v16-frozen'])

    def test_a_missing_pinned_object_is_fetched_first(self):
        commit = PROVIDERS['v14']['commit']
        with tempfile.TemporaryDirectory() as temporary:
            ok, calls, _ = self.plan(Path(temporary), missing_objects=(commit,))
        self.assertTrue(ok)
        fetch = ['git', '-C', str(skips.ROOT), 'fetch', '--no-tags', '--depth=1', 'origin', commit]
        self.assertIn(fetch, calls)
        self.assertLess(calls.index(fetch),
                        next(index for index, argv in enumerate(calls) if commit in argv
                             and '--tree' in argv))

    def test_an_existing_provider_that_does_not_match_is_refused_and_kept(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            existing = build / PROVIDERS['v13']['default_output']
            existing.mkdir()
            (existing / 'sentinel').write_text('keep\n')
            ok, calls, out = self.plan(build)
            self.assertTrue((existing / 'sentinel').is_file())
        self.assertFalse(ok)
        self.assertIn('v13: refused', out)
        self.assertFalse(any(PROVIDERS['v13']['commit'] in argv and '--tree' in argv
                             for argv in calls))
        self.assertEqual(sum('--output' in argv for argv in calls), 5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--ctest')
    known, rest = parser.parse_known_args()
    CTEST = known.ctest or CTEST
    unittest.main(argv=[sys.argv[0], *rest])
