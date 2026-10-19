#!/usr/bin/env python3
"""Unit tests for scripts/ci_flat_peak_gate.py, the CR-150 flat-peak memory gate.

No build, no real ctest, no Valgrind: a fake runner plays every command the
gate issues and writes the result files a real run would leave, so each test
fixes one outcome -- a pass, a regression, zero rows, a skipped row, an absent
or incomplete report -- and checks the gate's verdict and exit status. The
wiring tests read the real workflow files and CMake labels, then mutate copies.
"""
from __future__ import annotations

import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_flat_peak_gate as gate  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

LISTING_ONE = 'Test project /b\n  Test #1: test_flat_memory_rss_macd\n\nTotal Tests: 1\n'
LISTING_ZERO = 'Test project /b\n\nTotal Tests: 0\n'
LISTING_TWO = ('Test project /b\n  Test #1: test_flat_memory_rss_macd\n'
               '  Test #2: test_flat_memory_rss_e2e\n\nTotal Tests: 2\n')
RESULT_PASSED = '1/1 Test #1: test_flat_memory_rss_macd ..............   Passed   31.20 sec\n'
CTEST_PASS = ('Test project /b\n    Start 1: test_flat_memory_rss_macd\n' + RESULT_PASSED
              + '\n100% tests passed, 0 tests failed out of 1\n\nTotal Test time (real) =  31.25 sec\n')
CTEST_ZERO = 'Test project /b\n\n100% tests passed, 0 tests failed out of 0\n'
CTEST_FAIL = ('Test project /b\n    Start 1: test_flat_memory_rss_macd\n'
              '1/1 Test #1: test_flat_memory_rss_macd ..............***Failed   31.20 sec\n'
              '\n0% tests passed, 1 tests failed out of 1\n\nThe following tests FAILED:\n'
              '\t  1 - test_flat_memory_rss_macd (Failed)\nErrors while running CTest\n')
CTEST_SKIP = ('Test project /b\n    Start 1: test_flat_memory_rss_macd\n'
              '1/1 Test #1: test_flat_memory_rss_macd ..............***Skipped   0.01 sec\n'
              '\n100% tests passed, 0 tests failed out of 1\n\nThe following tests did not run:\n'
              '\t  1 - test_flat_memory_rss_macd (Skipped)\n')


def run(function, *args, **kwargs):
    """The function's exit status and everything it printed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = function(*args, **kwargs)
    return status, out.getvalue()


class FakeRunner:
    """Plays every command the gate issues; a handler per kind answers (status, output)."""

    def __init__(self, **handlers):
        self.calls: list[list[str]] = []
        self.handlers = handlers

    @staticmethod
    def kind(argv: list[str]) -> str:
        if argv[0] == 'cmake':
            return 'build' if '--build' in argv else 'configure'
        if argv[0] == 'ctest':
            return 'list' if '-N' in argv else 'ctest'
        if len(argv) > 1 and argv[1].endswith('derive_corpus_feeds.py'):
            return 'derive'
        if len(argv) > 1 and argv[1].endswith('heap_census.py'):
            return 'census'
        return 'other'

    @property
    def kinds(self) -> list[str]:
        return [self.kind(argv) for argv in self.calls]

    def __call__(self, argv, cwd):
        self.calls.append(list(argv))
        handler = self.handlers.get(self.kind(argv))
        return (0, '') if handler is None else handler(argv)


def write_results(results: Path, *, cases=gate.MEM_CASES, red=None, rungs=None, complete=True, run_doc=None):
    """The files test_flat_memory_rss_macd's harness leaves in its results directory."""
    results.mkdir(parents=True)
    counts = dict(gate.MEM_RUNG_FLOORS)
    counts.update(rungs or {})
    for case in cases:
        document = {'case': case, 'verdict': 'PASS', 'reason': 'all case assertions passed',
                    'measured_rungs': counts[case]}
        if case == red:
            document.update(verdict='RED', reason=f'({case}) bars: rest is not flat: spread 40.00 MiB > 1 MiB')
        (results / f'case-{case}.json').write_text(json.dumps(document))
    document = {'selected': list(gate.MEM_CASES), 'executed': list(cases),
                'passed': len(cases) - (1 if red else 0), 'red': 1 if red else 0, 'setup': 0,
                'errors': 0, 'returncode': 1 if red else 0, 'report_sink': 'off',
                'judgement_deferred': False, 'verdict': 'RED' if red else 'PASS'}
    document.update(run_doc or {})
    (results / ('RUN.COMPLETE' if complete else 'RUN.INCOMPLETE')).write_text(json.dumps(document))


class TempCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='pf-flat-peak-test-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)


class MemGateTests(TempCase):
    def setUp(self):
        super().setUp()
        self.build = self.tmp / 'build'
        self.build.mkdir()
        self.results = self.build / gate.MEM_RESULTS

    def built(self, argv, *, skip=()):
        for path in (gate.MEM_MODULE, *gate.MEM_STAGED):
            if path.name in skip:
                continue
            target = self.build / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'built')
        return 0, ''

    def ran(self, argv):
        write_results(self.results)
        return 0, CTEST_PASS

    def gate(self, **handlers):
        defaults = dict(build=self.built, list=lambda argv: (0, LISTING_ONE), ctest=self.ran)
        defaults.update(handlers)
        runner = FakeRunner(**defaults)
        status, out = run(gate.gate_mem, ROOT, self.build, 4, None, runner)
        return status, out, runner

    def test_a_proven_pass(self):
        status, out, runner = self.gate()
        self.assertEqual(status, 0, out)
        self.assertIn('flat-peak mem: ctest ran 1 rows (floor 1)', out)
        self.assertIn('flat-peak mem: PASS', out)
        self.assertEqual(runner.kinds, ['configure', 'build', 'list', 'ctest'])
        self.assertIn('-DPINEFORGE_BUILD_FLAT_MEMORY_TESTS=ON', runner.calls[0])
        self.assertIn('-DCMAKE_BUILD_TYPE=Release', runner.calls[0])
        self.assertEqual(runner.calls[1][-3:], ['pineforge', 'flat_memory_macd', 'flat_memory_prefix'])
        self.assertEqual(runner.calls[2], ['ctest', '--test-dir', str(self.build), '-N', '-L', 'mem'])
        self.assertEqual(runner.calls[3], ['ctest', '--test-dir', str(self.build), '-L', 'mem',
                                           '--no-tests=error', '--output-on-failure', '--timeout', '0', '-V'])

    def test_zero_registered_rows_fail_before_the_run(self):
        status, out, runner = self.gate(list=lambda argv: (0, LISTING_ZERO))
        self.assertEqual(status, 1, out)
        self.assertIn('selects 0 rows', out)
        self.assertNotIn('ctest', runner.kinds[3:])
        self.assertEqual(runner.kinds, ['configure', 'build', 'list'])

    def test_a_row_outside_the_expected_set_fails_before_the_run(self):
        status, out, runner = self.gate(list=lambda argv: (0, LISTING_TWO))
        self.assertEqual(status, 1, out)
        self.assertIn('test_flat_memory_rss_e2e', out)
        self.assertEqual(runner.kinds, ['configure', 'build', 'list'])

    def test_a_regression_fails_and_names_the_harness_reason(self):
        def ran(argv):
            write_results(self.results, cases=('a',), red='a', complete=False)
            return 1, CTEST_FAIL
        status, out, _ = self.gate(ctest=ran)
        self.assertEqual(status, 1, out)
        self.assertIn('flat-peak mem: FAIL', out)
        self.assertIn('rest is not flat: spread 40.00 MiB', out)
        self.assertIn('ctest exited 1', out)
        self.assertNotIn('flat-peak mem: PASS', out)

    def test_a_skipped_row_is_not_a_pass(self):
        def ran(argv):
            write_results(self.results)
            return 0, CTEST_SKIP
        status, out, _ = self.gate(ctest=ran)
        self.assertEqual(status, 1, out)
        self.assertIn('skipped rows are not a pass', out)
        self.assertIn('ctest ran 0 rows (floor 1)', out)

    def test_zero_rows_run_fails(self):
        status, out, _ = self.gate(ctest=lambda argv: (0, CTEST_ZERO))
        self.assertEqual(status, 1, out)
        self.assertIn('ctest ran 0 rows (floor 1)', out)
        self.assertIn('below the floor of 1', out)

    def test_a_green_ctest_with_no_harness_verdict_is_not_a_pass(self):
        status, out, _ = self.gate(ctest=lambda argv: (0, CTEST_PASS))
        self.assertEqual(status, 1, out)
        self.assertIn('no readable', out)
        self.assertIn('RUN.COMPLETE', out)

    def test_an_incomplete_harness_run_is_not_a_pass(self):
        def ran(argv):
            write_results(self.results, complete=False)
            return 0, CTEST_PASS
        status, out, _ = self.gate(ctest=ran)
        self.assertEqual(status, 1, out)
        self.assertIn('RUN.INCOMPLETE', out)

    def test_a_short_ladder_is_not_a_pass(self):
        def ran(argv):
            write_results(self.results, rungs={'a': 1})
            return 0, CTEST_PASS
        status, out, _ = self.gate(ctest=ran)
        self.assertEqual(status, 1, out)
        self.assertIn('case a measured 1 rungs, below its floor of 4', out)

    def test_a_deferred_verdict_is_not_a_pass(self):
        def ran(argv):
            write_results(self.results, run_doc={'judgement_deferred': True})
            return 0, CTEST_PASS
        status, out, _ = self.gate(ctest=ran)
        self.assertEqual(status, 1, out)
        self.assertIn('judgement_deferred', out)

    def test_a_row_without_its_passed_line_is_not_a_pass(self):
        def ran(argv):
            write_results(self.results)
            return 0, CTEST_PASS.replace(RESULT_PASSED, '')
        status, out, _ = self.gate(ctest=ran)
        self.assertEqual(status, 1, out)
        self.assertIn('has no Passed result line', out)

    def test_a_stale_results_directory_is_refused_and_nothing_runs(self):
        self.results.mkdir()
        status, out, runner = self.gate()
        self.assertEqual(status, 2, out)
        self.assertIn('already exists', out)
        self.assertEqual(runner.calls, [])

    def test_a_failed_configure_stops_the_sequence(self):
        status, out, runner = self.gate(configure=lambda argv: (1, ''))
        self.assertEqual(status, 2, out)
        self.assertEqual(runner.kinds, ['configure'])

    def test_a_missing_module_stops_before_ctest(self):
        status, out, runner = self.gate(build=lambda argv: (0, ''))
        self.assertEqual(status, 2, out)
        self.assertIn('macd.so was not built or staged', out)
        self.assertEqual(runner.kinds, ['configure', 'build'])

    def test_a_missing_planner_library_stops_before_ctest(self):
        status, out, runner = self.gate(build=lambda argv: self.built(argv, skip=('libpineforge_window_plan.so',)))
        self.assertEqual(status, 2, out)
        self.assertIn('libpineforge_window_plan.so was not built or staged', out)
        self.assertNotIn('macd.so was not', out)
        self.assertEqual(runner.kinds, ['configure', 'build'])

    def test_a_missing_staged_run_json_stops_before_ctest(self):
        status, out, runner = self.gate(build=lambda argv: self.built(argv, skip=('run_json.py',)))
        self.assertEqual(status, 2, out)
        self.assertIn('run_json.py was not built or staged', out)
        self.assertEqual(runner.kinds, ['configure', 'build'])

    def test_the_launcher_reaches_the_configure_step(self):
        runner = FakeRunner(build=self.built, list=lambda argv: (0, LISTING_ONE), ctest=self.ran)
        run(gate.gate_mem, ROOT, self.build, 2, '/usr/bin/ccache', runner)
        self.assertIn('-DCMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache', runner.calls[0])
        self.assertEqual(runner.calls[1][runner.calls[1].index('--parallel') + 1], '2')


class CensusGateTests(TempCase):
    MODES = [f'mode{index}' for index in range(10)]
    POPULATION = ['validation/p1', 'validation/p2']

    def setUp(self):
        super().setUp()
        self.root = self.tmp / 'src'
        (self.root / 'corpus' / 'data').mkdir(parents=True)
        (self.root / 'corpus' / 'CMakeLists.txt').write_text('')
        (self.root / gate.CENSUS_FEED).write_bytes(b'0' * gate.CENSUS_MIN_FEED_BYTES)
        battery = self.root / 'tests' / 'fixtures' / 'flat_memory'
        battery.mkdir(parents=True)
        (battery / 'heap_census_battery.json').write_text(json.dumps(
            {'schema': 'pineforge-heap-census-battery/v1', 'native_modes': self.MODES}))
        self.build = self.tmp / 'build'
        self.build.mkdir()

    def report(self, **overrides):
        comparisons = ([{'name': mode, 'unit': 'bars', 'sites': []} for mode in self.MODES]
                       + [{'name': probe, 'unit': 'bars', 'sites': [
                           {'site': 'census_chart_ohlcv()', 'lane': 'caller OHLCV', 'admitted': True,
                            'delta_bytes': 192000, 'bytes_per_unit': 16.0}]}
                          for probe in self.POPULATION])
        document = {'schema': gate.CENSUS_SCHEMA, 'status': 'PASS', 'memory_verdict': 'FLAT',
                    'errors': [], 'corpus_requested': True,
                    'corpus_population': list(self.POPULATION), 'comparisons': comparisons}
        document.update(overrides)
        return document

    def census(self, report=None, status=0, write=True):
        def handler(argv):
            output = Path(argv[argv.index('--output') + 1])
            output.mkdir(parents=True)
            if write:
                (output / 'census.json').write_text(json.dumps(self.report() if report is None else report))
            return status, ''
        return handler

    def gate(self, which=None, platform='linux', **handlers):
        runner = FakeRunner(**handlers)
        status, out = run(gate.gate_census, self.root, self.build, 4, None, runner,
                          which=which or (lambda tool: f'/usr/bin/{tool}'), platform=platform)
        return status, out, runner

    def test_a_proven_pass(self):
        status, out, runner = self.gate(census=self.census())
        self.assertEqual(status, 0, out)
        self.assertIn('flat-peak census: PASS: 12 comparisons (floor 12), 2 corpus probes', out)
        self.assertEqual(runner.kinds, ['derive', 'configure', 'build', 'census'])
        self.assertIn('-DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON', runner.calls[1])
        self.assertIn('corpus_strategies', runner.calls[2])
        self.assertIn('test_heap_census_unique_ids', runner.calls[2])
        self.assertIn('--corpus', runner.calls[3])

    def test_a_growing_allocation_site_fails_and_is_named(self):
        grown = self.report(status='RED', memory_verdict='RED')
        grown['comparisons'][0]['sites'] = [{
            'site': 'operator new <- scratch::grow(unsigned long) <- run', 'lane': 'L19j',
            'admitted': False, 'delta_bytes': 48000, 'bytes_per_unit': 16.0}]
        status, out, _ = self.gate(census=self.census(report=grown, status=1))
        self.assertEqual(status, 1, out)
        self.assertIn('flat-peak census: FAIL', out)
        self.assertIn('1 growing allocation sites are not admitted', out)
        self.assertIn('mode0: L19j +48000 B', out)
        self.assertIn('scratch::grow', out)
        self.assertIn('exited 1', out)

    def test_an_open_proof_is_not_a_pass(self):
        opened = self.report(status='OPEN', errors=['valgrind: checkpoint timed out'])
        status, out, _ = self.gate(census=self.census(report=opened, status=2))
        self.assertEqual(status, 1, out)
        self.assertIn('open proof: valgrind: checkpoint timed out', out)

    def test_an_absent_report_is_not_a_pass(self):
        status, out, _ = self.gate(census=self.census(write=False))
        self.assertEqual(status, 1, out)
        self.assertIn('an absent report is never a pass', out)

    def test_a_battery_only_report_is_not_a_pass(self):
        battery_only = self.report(status='BATTERY_ONLY', corpus_requested=False)
        status, out, _ = self.gate(census=self.census(report=battery_only, status=2))
        self.assertEqual(status, 1, out)
        self.assertIn('BATTERY_ONLY', out)
        self.assertIn('battery-only report cannot PASS', out)

    def test_a_pass_report_with_a_failing_exit_status_is_not_a_pass(self):
        status, out, _ = self.gate(census=self.census(status=1))
        self.assertEqual(status, 1, out)
        self.assertIn('tests/heap_census.py exited 1', out)

    def test_zero_comparisons_is_not_a_pass(self):
        empty = self.report(comparisons=[], corpus_population=[])
        status, out, _ = self.gate(census=self.census(report=empty))
        self.assertEqual(status, 1, out)
        self.assertIn('zero comparisons', out)
        self.assertIn('zero corpus probes', out)

    def test_fewer_comparisons_than_the_floor_is_not_a_pass(self):
        short = self.report()
        short['comparisons'] = short['comparisons'][:5]
        status, out, _ = self.gate(census=self.census(report=short))
        self.assertEqual(status, 1, out)
        self.assertIn('below the floor of 12', out)

    def test_a_missing_valgrind_fails_and_runs_nothing(self):
        status, out, runner = self.gate(which=lambda tool: None if tool == 'valgrind' else '/usr/bin/x')
        self.assertEqual(status, 2, out)
        self.assertIn('valgrind is not installed', out)
        self.assertIn('never a skip', out)
        self.assertEqual(runner.calls, [])

    def test_a_non_linux_host_fails(self):
        status, out, runner = self.gate(platform='darwin')
        self.assertEqual(status, 2, out)
        self.assertEqual(runner.calls, [])

    def test_a_missing_corpus_fails(self):
        (self.root / 'corpus' / 'CMakeLists.txt').unlink()
        status, out, runner = self.gate()
        self.assertEqual(status, 2, out)
        self.assertEqual(runner.calls, [])

    def test_an_lfs_pointer_feed_fails(self):
        (self.root / gate.CENSUS_FEED).write_bytes(b'version https://git-lfs.github.com/spec/v1\n')
        status, out, runner = self.gate()
        self.assertEqual(status, 2, out)
        self.assertIn('Git-LFS pointer', out)
        self.assertEqual(runner.calls, [])

    def test_an_existing_output_directory_is_refused(self):
        (self.build / 'heap-census').mkdir()
        status, out, runner = self.gate()
        self.assertEqual(status, 2, out)
        self.assertEqual(runner.calls, [])

    def test_a_failed_build_stops_before_the_census(self):
        status, out, runner = self.gate(build=lambda argv: (2, ''))
        self.assertEqual(status, 2, out)
        self.assertEqual(runner.kinds, ['derive', 'configure', 'build'])


class DefaultRunnerTests(unittest.TestCase):
    def test_the_childs_exit_status_and_output_come_back(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status, text = gate.default_runner(
                [sys.executable, '-c', 'import sys; print("hello"); sys.exit(3)'], ROOT)
        self.assertEqual(status, 3)
        self.assertEqual(text, 'hello\n')
        self.assertIn('hello', out.getvalue())

    def test_a_missing_executable_is_a_failure_status(self):
        with contextlib.redirect_stdout(io.StringIO()):
            status, text = gate.default_runner(['/nonexistent/pineforge-gate-tool'], ROOT)
        self.assertEqual(status, 127)
        self.assertIn('pineforge-gate-tool', text)


class WiringTests(TempCase):
    FILES = ('.github/workflows/ci.yml', '.github/workflows/release.yml',
             '.github/workflows/flat-peak.yml', 'tests/CMakeLists.txt')

    def findings(self, **mutations):
        """check_wiring over copies of the real files with each (old, new) replaced once."""
        root = self.tmp / 'wired'
        for relative in self.FILES:
            text = (ROOT / relative).read_text()
            for old, new in mutations.get(relative, ()):
                self.assertIn(old, text, f'the mutation anchor is gone from {relative}: {old!r}')
                text = text.replace(old, new, 1)
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
        return gate.check_wiring(root)

    def test_the_tree_is_wired(self):
        self.assertEqual(gate.check_wiring(ROOT), [])
        self.assertEqual(self.findings(), [])

    def test_ci_without_the_gate_job_is_refused(self):
        found = self.findings(**{'.github/workflows/ci.yml': [('\n  flat-peak:\n', '\n  flat-peak-renamed:\n')]})
        self.assertIn('ci.yml must call the flat-peak gate (job flat-peak)', found)

    def test_ci_that_runs_the_gate_on_a_pull_request_is_refused(self):
        found = self.findings(**{'.github/workflows/ci.yml': [
            ("github.event_name != 'pull_request' && (needs.changes.result", '(needs.changes.result')]})
        self.assertTrue(any('excludes pull_request' in finding for finding in found), found)

    def test_a_release_job_that_stops_needing_the_gate_is_refused(self):
        found = self.findings(**{'.github/workflows/release.yml': [('    needs: flat-peak-gate\n', '')]})
        self.assertIn('release.yml job release must need exactly flat-peak-gate', found)

    def test_a_release_without_the_gate_job_is_refused(self):
        found = self.findings(**{'.github/workflows/release.yml': [('\n  flat-peak-gate:\n', '\n  flat-peak-skipped:\n')]})
        self.assertTrue(any('must call the flat-peak gate' in finding for finding in found), found)

    def test_a_publishing_job_that_escapes_the_chain_is_refused(self):
        found = self.findings(**{'.github/workflows/release.yml': [('  prebuilt:\n    needs: release\n', '  prebuilt:\n')]})
        self.assertIn('release.yml job prebuilt must need exactly release', found)

    def test_a_gate_step_that_continues_on_error_is_refused(self):
        found = self.findings(**{'.github/workflows/flat-peak.yml': [(
            '      - name: Flat-peak memory rows (ctest -L mem)\n',
            '      - name: Flat-peak memory rows (ctest -L mem)\n        continue-on-error: true\n')]})
        self.assertTrue(any('must not continue on error' in finding for finding in found), found)

    def test_a_gate_step_under_an_if_is_refused(self):
        found = self.findings(**{'.github/workflows/flat-peak.yml': [(
            '      - name: Heap census (growth gate)\n',
            '      - name: Heap census (growth gate)\n        if: always()\n')]})
        self.assertTrue(any('must not carry an `if`' in finding for finding in found), found)

    def test_a_gate_command_behind_a_pipe_is_refused(self):
        found = self.findings(**{'.github/workflows/flat-peak.yml': [(
            'python3 scripts/ci_flat_peak_gate.py mem --build-dir build-flat-peak',
            'python3 scripts/ci_flat_peak_gate.py mem --build-dir build-flat-peak | tee mem.log')]})
        self.assertTrue(any('must stand alone' in finding for finding in found), found)

    def test_a_failure_swallowed_by_true_is_refused(self):
        found = self.findings(**{'.github/workflows/flat-peak.yml': [(
            '--jobs "$(getconf _NPROCESSORS_ONLN)" --ccache\n\n      - name: Retain gate diagnostics\n        if: always()\n'
            '        uses: actions/upload-artifact@v4\n        with:\n          name: flat-peak-census',
            '--jobs "$(getconf _NPROCESSORS_ONLN)" --ccache || true\n\n      - name: Retain gate diagnostics\n'
            '        if: always()\n        uses: actions/upload-artifact@v4\n        with:\n          name: flat-peak-census')]})
        self.assertTrue(any('must not tolerate a failure' in finding for finding in found), found)

    def test_a_dropped_arm_is_refused(self):
        found = self.findings(**{'.github/workflows/flat-peak.yml': [(
            'python3 scripts/ci_flat_peak_gate.py census', 'python3 scripts/other.py census')]})
        self.assertTrue(any('in exactly one step' in finding for finding in found), found)

    def test_a_workflow_that_cannot_be_called_is_refused(self):
        found = self.findings(**{'.github/workflows/flat-peak.yml': [('\n  workflow_call:\n', '\n')]})
        self.assertIn('flat-peak.yml must be callable (workflow_call)', found)

    def test_a_mem_row_that_loses_its_label_is_refused(self):
        found = self.findings(**{'tests/CMakeLists.txt': [(
            'set_tests_properties(test_flat_memory_rss_macd PROPERTIES\n        LABELS "mem;slow;l4-pending")',
            'set_tests_properties(test_flat_memory_rss_macd PROPERTIES\n        LABELS "slow;l4-pending")')]})
        self.assertTrue(any('must carry the label mem' in finding for finding in found), found)

    def test_a_mem_row_that_runs_the_source_tree_run_json_is_refused(self):
        found = self.findings(**{'tests/CMakeLists.txt': [(
            '--run-json ${PINEFORGE_FLAT_MEMORY_PREFIX}/bin/run_json.py',
            '--run-json ${PROJECT_SOURCE_DIR}/docker/run_json.py')]})
        self.assertTrue(any('must run the staged run_json.py' in finding for finding in found), found)

    def test_a_label_that_ctest_l_mem_would_also_select_is_refused(self):
        found = self.findings(**{'tests/CMakeLists.txt': [(
            'LABELS "codegen-fixtures;slow;l4-pending"', 'LABELS "mem-codegen-fixtures;slow;l4-pending"')]})
        self.assertTrue(any('regular-expression search' in finding for finding in found), found)

    def test_a_missing_file_is_a_finding(self):
        self.assertEqual(gate.check_wiring(self.tmp / 'empty')[0].split(' ')[0], 'cannot')


if __name__ == '__main__':
    unittest.main()
