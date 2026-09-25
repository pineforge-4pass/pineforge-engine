#!/usr/bin/env python3
"""Exercise preflight failure propagation with real child processes."""
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from ci_preflight import (CORES, JOB_RUNNERS, LINUX_RUNNER, MATRIX_RUNNER,
                          STAGE_TIMEOUT_SECONDS, _jobs, check_commands,
                          ci_workflow_findings, run_checks)

ROOT = Path(__file__).resolve().parents[1]
# ci_workflow_findings' arguments, in order.
CI_SOURCES = ('.github/workflows/ci.yml', '.github/workflows/native-live.yml',
              '.github/workflows/promote-baseline.yml', 'tests/CMakeLists.txt',
              '.github/workflows/corpus-parity.yml', '.github/workflows/docs.yml')


def in_job(workflow, job, before, after):
    """Replace the first `before` inside job `job` (anywhere when job is None)."""
    start = 0 if job is None else workflow.index(f'\n  {job}:')
    if job is not None and before not in _jobs(workflow)[job]:
        raise AssertionError(f'{before!r} is not in job {job}')
    at = workflow.index(before, start)
    return workflow[:at] + after + workflow[at + len(before):]


class PreflightFailures(unittest.TestCase):
    def ci_sources(self):
        return [(ROOT / path).read_text() for path in CI_SOURCES]

    def other_sources(self):
        """Every other workflow file, by name: ci_workflow_findings' ``others``."""
        named = {Path(path).name for path in CI_SOURCES}
        return {path.name: path.read_text()
                for path in sorted((ROOT / '.github/workflows').iterdir())
                if path.suffix in ('.yml', '.yaml') and path.name not in named}

    def test_ci_contract_pins_parallel_pr_light_and_full_events(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        mutations = (
            (0, '  workflow_dispatch:\n', ''),
            (0, '  native-live:\n', '  missing-native-live:\n'),
            (0, '  sanitizers:\n', '  missing-sanitizers:\n'),
            (0, '  build:\n', '  missing-build:\n'),
            (0, '  kernel-only:\n', '  missing-kernel:\n'),
            (0, '  corpus-parity-subset:\n', '  missing-subset:\n'),
            (0, '  build-gate:\n', '  missing-gate:\n'),
            (0, '"$SANITIZER_RESULT" == "success"', '"$SANITIZER_RESULT" != "success"'),
            (0, '  sanitizers:\n', '  sanitizers:\n    needs: preflight\n'),
            (0, "matrix.build_type == 'Debug' && '--exclude-label slow'", "'--exclude-label slow'"),
            (0, "github.event_name == 'pull_request' && '--exclude-label slow'", "'--exclude-label slow'"),
            (0, "exclude_slow: ${{ github.event_name == 'pull_request' }}", 'exclude_slow: true'),
            (1, '        default: false', '        default: true'),
            (1, "${{ inputs.exclude_slow && '--exclude-label slow' || '' }}", ''),
            # Baseline promotion runs main's copy of the workflow and no PR
            # code, and needs the newest two statuses on the PR head.
            (2, '  pull_request_target:\n', '  pull_request:\n'),
            (2, '  statuses: read', '  checks: read'),
            (2, '  id-token: write', '  id-token: none'),
            (2, '        with:\n          fetch-depth: 0',
                '        with:\n          ref: ${{ github.event.pull_request.head.sha }}\n'
                '          fetch-depth: 0'),
            (2, '/commits/$HEAD_SHA/statuses?per_page=100', '/commits/$HEAD_SHA/check-runs'),
            (2, '.["pineforge/parity"] == "success"', 'true'),
            (2, 'if has($s.context) then . else .[$s.context] = $s.state end',
                '.[$s.context] = $s.state'),
            (2, 'echo "green=false" >> "$GITHUB_OUTPUT"; exit 0', 'echo "green=false" >> "$GITHUB_OUTPUT"'),
            # Squash merges: the merge commit is merge_commit_sha, admitted by
            # tree equality with the verified PR head, never by its ancestry,
            # and the campaign tool gets that tree.
            (2, 'github.event.pull_request.merge_commit_sha || github.event.inputs.merge_commit',
                'github.event.pull_request.head.sha || github.event.inputs.merge_commit'),
            (2, 'HEAD_SHA: ${{ github.event.pull_request.head.sha || github.event.inputs.head_sha }}',
                'HEAD_SHA: ${{ github.event.pull_request.merge_commit_sha || github.event.inputs.merge_commit }}'),
            (2, '      head_sha:\n', '      pr_commit:\n'),
            (2, 'git merge-base --is-ancestor "$MERGE_SHA"', 'git merge-base --is-ancestor "$HEAD_SHA"'),
            (2, 'if [ "$merge_tree" != "$head_tree" ]; then', 'if false; then'),
            (2, 'head_tree=$(git rev-parse "$HEAD_SHA^{tree}")',
                'head_tree=$(git rev-parse "$MERGE_SHA^{tree}")'),
            (2, 'echo "ok=false" >> "$GITHUB_OUTPUT"; exit 0', 'echo "ok=false" >> "$GITHUB_OUTPUT"'),
            (2, 'MERGE_TREE: ${{ steps.exacttree.outputs.merge_tree }}',
                'MERGE_TREE: ${{ github.event.pull_request.head.sha }}'),
            (2, '--merge-commit "$MERGE_SHA"', '--merge-commit "$HEAD_SHA"'),
            (2, ' --merge-tree "$MERGE_TREE"', ''),
            (2, '--ci-head "$HEAD_SHA"', '--ci-head "$MERGE_SHA"'),
            (2, 'if [ "$code" = "2" ]; then', 'if false; then'),
            (3, '    test_chart_day_memo\n    test_ci_verify\n',
                '    test_chart_day_memo\n'),
            (3, 'APPEND PROPERTY LABELS slow', 'APPEND PROPERTY LABELS other'),
        )
        for index, before, after in mutations:
            with self.subTest(index=index, before=before):
                changed = original.copy()
                self.assertIn(before, changed[index])
                changed[index] = changed[index].replace(before, after, 1)
                self.assertNotEqual(ci_workflow_findings(*changed), [])

    def test_ci_contract_pins_runners_time_limits_and_the_fork_guard(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        fork_head = 'github.event.pull_request.head.repo.full_name'
        mutations = (
            # A heavy job moved back to the slow standard runner.
            (0, 'preflight', LINUX_RUNNER, 'ubuntu-24.04', 'ci.yml job preflight must run on'),
            (0, 'sanitizers', LINUX_RUNNER, 'ubuntu-24.04', 'ci.yml job sanitizers must run on'),
            (0, 'kernel-only', LINUX_RUNNER, 'ubuntu-24.04', 'ci.yml job kernel-only must run on'),
            (0, 'build', MATRIX_RUNNER, '${{ matrix.os }}', 'ci.yml job build must run on'),
            (0, 'build', 'larger_runner: macos-26-xlarge', 'larger_runner: macos-26',
             'ci.yml build must pair each image'),
            (0, 'build', 'larger_runner: pf-linux-x64-16', 'larger_runner: ubuntu-24.04',
             'ci.yml build must pair each image'),
            # A later include entry overrides one leg's larger runner.
            (0, 'build', '            larger_runner: macos-26-xlarge\n',
             '            larger_runner: macos-26-xlarge\n          - os: ubuntu-24.04\n'
             '            build_type: Debug\n            larger_runner: ubuntu-24.04\n',
             'ci.yml build must pair each image'),
            (1, 'native-live', LINUX_RUNNER, 'ubuntu-24.04', 'native-live.yml job native-live must run on'),
            (4, 'corpus-parity', LINUX_RUNNER, 'ubuntu-24.04', 'corpus-parity.yml job corpus-parity must run on'),
            (4, 'corpus-parity-subset', LINUX_RUNNER, 'ubuntu-24.04',
             'corpus-parity.yml job corpus-parity-subset must run on'),
            # A fork's pull request let onto a larger runner, which bills the org.
            (0, 'preflight', LINUX_RUNNER, 'pf-linux-x64-16', 'ci.yml job preflight must run on'),
            (0, 'sanitizers', fork_head, 'github.event.pull_request.base.repo.full_name',
             'ci.yml job sanitizers must run on'),
            (0, 'kernel-only', f'{fork_head} == github.repository', f"{fork_head} != ''",
             'ci.yml job kernel-only must run on'),
            (0, 'build', MATRIX_RUNNER, '${{ matrix.larger_runner }}', 'ci.yml job build must run on'),
            (0, 'build', 'larger_runner: macos-26-xlarge', 'larger_runner: macos-26-large',
             'ci.yml build must pair each image'),
            # A leg whose os is a larger runner: the fork's fallback is the larger runner.
            (0, 'build', '            larger_runner: macos-26-xlarge\n',
             '            larger_runner: macos-26-xlarge\n          - os: pf-linux-x64-16\n'
             '            build_type: Release\n', 'ci.yml build must pair each image'),
            # Block forms of runs-on, and job ids the parser must still see.
            (5, 'build', '    runs-on: ubuntu-latest\n', '    runs-on:\n      group: pineforge-ci-large\n',
             'docs.yml job build must stay on a standard runner'),
            (2, 'promote', '    runs-on: ubuntu-latest\n', '    runs-on:\n      - macos-26-xlarge\n',
             'promote-baseline.yml job promote must stay on a standard runner'),
            (0, None, '  build-gate:\n', '  extra:\n    runs-on:\n      group: pineforge-ci-large\n'
             '    steps:\n      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner'),
            (5, None, '\njobs:\n', '\njobs:\n  pre_build:\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml job pre_build must stay on a standard runner'),
            (5, None, '\njobs:\n', '\njobs:\n  "pre-build":\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml has jobs lines the runner contract cannot read'),
            (1, 'native-live', "|| 'ubuntu-24.04'", "|| 'pf-linux-x64-16'",
             'native-live.yml job native-live must run on'),
            (4, 'corpus-parity-subset', f'runs-on: {LINUX_RUNNER}', 'runs-on:\n      group: pineforge-ci-large',
             'corpus-parity.yml job corpus-parity-subset must run on'),
            (0, 'build-gate', 'runs-on: ubuntu-24.04', 'runs-on: pf-linux-x64-16',
             'ci.yml job build-gate must run on ubuntu-24.04'),
            (5, 'build', 'runs-on: ubuntu-latest', 'runs-on: pf-linux-x64-16',
             'docs.yml job build must stay on a standard runner'),
            (2, 'promote', 'runs-on: ubuntu-latest', 'runs-on: macos-26-xlarge',
             'promote-baseline.yml job promote must stay on a standard runner'),
            (0, None, '  build-gate:\n', '  extra:\n    runs-on: ubuntu-24.04\n    steps:\n'
             '      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner'),
            # A time limit changed away from its measured basis.
            (0, 'preflight', 'timeout-minutes: 45', 'timeout-minutes: 30',
             'ci.yml job preflight must allow 45 minutes'),
            (0, 'build', 'timeout-minutes: 75', 'timeout-minutes: 45', 'ci.yml job build must allow 75'),
            (0, 'sanitizers', 'timeout-minutes: 120', 'timeout-minutes: 60',
             'ci.yml job sanitizers must allow 120'),
            (0, 'kernel-only', 'timeout-minutes: 60', 'timeout-minutes: 45',
             'ci.yml job kernel-only must allow 60'),
            (0, 'build-gate', '    timeout-minutes: 5\n', '', 'ci.yml job build-gate must allow 5'),
            (1, 'native-live', 'timeout-minutes: 60', 'timeout-minutes: 45',
             'native-live.yml job native-live must allow 60'),
            (4, 'corpus-parity', 'timeout-minutes: 120', 'timeout-minutes: 30',
             'corpus-parity.yml job corpus-parity must allow 120'),
            (4, 'corpus-parity-subset', 'timeout-minutes: 30', 'timeout-minutes: 10',
             'corpus-parity.yml job corpus-parity-subset must allow 30'),
            # Four jobs asked of a 16-core runner.
            (0, 'build', f'--jobs {CORES}', '--jobs 4', 'ci.yml job build must size'),
            (0, 'sanitizers', f'--jobs {CORES}', '--jobs 4', 'ci.yml job sanitizers must size'),
            (0, 'kernel-only', f'--jobs {CORES}', '--jobs "$(nproc)"', 'ci.yml job kernel-only must size'),
            (1, 'native-live', f'--jobs {CORES}', '--jobs 4', 'native-live.yml job native-live must size'),
            (4, 'corpus-parity', f'JOBS={CORES} ./scripts', './scripts',
             'corpus-parity.yml job corpus-parity must size'),
            (4, 'corpus-parity-subset', '          BUILD_DIR: build-corpus-parity-subset\n',
             '          BUILD_DIR: build-corpus-parity-subset\n          JOBS: "4"\n',
             'corpus-parity.yml job corpus-parity-subset must size'),
            # ... through a multi-line run block, an = spelling, or a later flag.
            (0, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES}',
             'run: |\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel',
             'ci.yml job kernel-only must size'),
            (0, 'sanitizers', f'--jobs {CORES}', '--jobs=4', 'ci.yml job sanitizers must size'),
            (0, 'build', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --jobs=4',
             'ci.yml job build must size'),
            (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
             'run: |\n          JOBS=4 ./scripts/check_corpus_parity.sh --subset',
             'corpus-parity.yml job corpus-parity-subset must size'),
            (4, 'corpus-parity', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh',
             'run: |\n          export JOBS=4\n          ./scripts/check_corpus_parity.sh',
             'corpus-parity.yml job corpus-parity must size'),
            # The build legs keep their names: build (<image>, <type>).
            (0, 'build', '    name: build (${{ matrix.os }}, ${{ matrix.build_type }})\n', '',
             'ci.yml build legs must keep their names'),
        )
        for index, job, before, after, finding in mutations:
            with self.subTest(file=CI_SOURCES[index], job=job, after=after):
                changed = original.copy()
                changed[index] = in_job(changed[index], job, before, after)
                findings = ci_workflow_findings(*changed, others=others)
                self.assertTrue(any(finding in line for line in findings), findings)

    def test_comments_and_commented_headers_are_not_findings(self):
        changed, others = self.ci_sources(), self.other_sources()
        changed[0] = in_job(changed[0], 'kernel-only', '    steps:\n',
                            '    # Was --jobs 4, and JOBS: "4", on the standard runner.\n'
                            '    steps:\n')
        changed[0] = changed[0].replace('\n  preflight:\n', '\n  preflight:  # the fast gate\n', 1)
        self.assertEqual(ci_workflow_findings(*changed, others=others), [])

    def test_every_workflow_a_pull_request_can_start_stays_on_standard_runners(self):
        original, others = self.ci_sources(), self.other_sources()
        # release.yml starts only on a dispatch, so no pull request reaches its runners.
        self.assertIn('release.yml', others)
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        opened = others['release.yml'].replace('on:\n  workflow_dispatch:\n',
                                               'on:\n  pull_request:\n  workflow_dispatch:\n', 1)
        self.assertNotEqual(opened, others['release.yml'])
        self.assertIn('release.yml job prebuilt must stay on a standard runner',
                      ci_workflow_findings(*original, others=dict(others, **{'release.yml': opened})))
        new = 'name: lint\non: {on}\njobs:\n  lint:\n    runs-on: {runner}\n    steps:\n      - run: "true"\n'
        for on, runner, refused in (
                ('[pull_request]', 'pf-linux-x64-16', True),
                ('pull_request_target', 'macos-26-xlarge', True),
                ('\n  pull_request:\n', '\n      group: pineforge-ci-large', True),
                ('[pull_request]', 'ubuntu-latest', False),
                ('\n  workflow_dispatch:\n', 'pf-linux-x64-16', False)):
            with self.subTest(on=on, runner=runner):
                findings = ci_workflow_findings(
                    *original, others=dict(others, **{'lint.yml': new.format(on=on, runner=runner)}))
                self.assertEqual('lint.yml job lint must stay on a standard runner' in findings,
                                 refused, findings)

    def test_a_stage_times_out_inside_the_preflight_job(self):
        # Five minutes stay for the runner setup and the other stages, so the
        # stage's own timeout, with its log, fires before the job's.
        job_seconds = JOB_RUNNERS['ci.yml']['preflight'][1] * 60
        self.assertLessEqual(STAGE_TIMEOUT_SECONDS + 300, job_seconds)
        # And at least twice the stage's standard-runner time at 0d76a099's
        # tree: 475 s on the verification hosts, 1.94 times as fast.
        self.assertGreaterEqual(STAGE_TIMEOUT_SECONDS, 2 * 475 * 1.94)

    def contract_stage(self, mutation):
        """Run the ci-workflow-contract stage's argv in a copy of the tree."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        tree = Path(directory.name)
        # A Python row CTest runs beside this one writes its bytecode through
        # a temporary file that can vanish mid-copy: never copy bytecode.
        shutil.copytree(ROOT / 'scripts', tree / 'scripts',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        shutil.copytree(ROOT / '.github/workflows', tree / '.github/workflows')
        (tree / 'tests').mkdir()
        shutil.copyfile(ROOT / 'tests/CMakeLists.txt', tree / 'tests/CMakeLists.txt')
        if mutation:
            ci = tree / '.github/workflows/ci.yml'
            ci.write_text(in_job(ci.read_text(), *mutation))
        result = subprocess.run(
            [sys.executable, str(tree / 'scripts/ci_preflight.py'), '--check-ci-workflow'],
            cwd=tree, capture_output=True, text=True, timeout=120)
        return result.returncode, result.stdout + result.stderr

    def test_the_preflight_stage_fails_a_slow_or_fork_reachable_runner(self):
        code, output = self.contract_stage(None)
        self.assertEqual(code, 0, output)
        self.assertIn('runners and time limits OK', output)
        for mutation, finding in (
                (('sanitizers', LINUX_RUNNER, 'ubuntu-24.04'), 'ci.yml job sanitizers must run on'),
                (('sanitizers', 'pull_request.head.repo', 'pull_request.base.repo'),
                 'ci.yml job sanitizers must run on')):
            with self.subTest(mutation=mutation):
                code, output = self.contract_stage(mutation)
                self.assertEqual(code, 1, output)
                self.assertIn(finding, output)

    def run_preflight(self, commands):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        with contextlib.redirect_stdout(io.StringIO()):
            code = run_checks(commands, root / 'logs', source=root)
        return code, json.loads((root / 'logs/preflight-summary.json').read_text()), root / 'logs'

    def test_failure_keeps_exit_code_logs_and_runs_independent_checks(self):
        code, summary, logs = self.run_preflight([
            ('fail', [sys.executable, '-c', 'import sys; print("failure witness"); sys.exit(7)']),
            ('next', [sys.executable, '-c', 'print("next check ran")']),
        ])
        self.assertEqual(code, 1)
        self.assertEqual(summary['status'], 'failed')
        self.assertEqual([s['exitCode'] for s in summary['stages']], [7, 0])
        self.assertIn('failure witness', (logs / 'fail.log').read_text())
        self.assertIn('next check ran', (logs / 'next.log').read_text())

    def test_missing_tool_is_a_failure(self):
        code, summary, _ = self.run_preflight([('missing', ['/nonexistent/pineforge-ci-tool'])])
        self.assertEqual(code, 1)
        self.assertEqual(summary['stages'][0]['exitCode'], 127)

    def test_empty_check_plan_cannot_pass(self):
        code, summary, _ = self.run_preflight([])
        self.assertEqual(code, 1)
        self.assertEqual(summary['status'], 'failed')

    def test_advisory_failure_is_reported_without_failing_the_run(self):
        # R5 L14-A: the documentation guards report today's drift until the
        # page-rewriting lanes land. Reported, logged, and not binding.
        code, summary, logs = self.run_preflight([
            ('docs', [sys.executable, '-c', 'import sys; print("drift"); sys.exit(1)'], True),
            ('next', [sys.executable, '-c', 'print("next check ran")']),
        ])
        self.assertEqual(code, 0)
        self.assertEqual(summary['status'], 'passed')
        self.assertEqual(summary['stages'][0]['status'], 'reported')
        self.assertEqual(summary['stages'][0]['exitCode'], 1)
        self.assertIn('drift', (logs / 'docs.log').read_text())

    def test_every_documentation_stage_is_binding(self):
        # R5 int12: the page lanes landed, so the guards and their self-tests
        # are ordinary fail-closed stages and --strict-docs is gone. A stage
        # declares advisory by carrying a third element; none of these may.
        plan = {entry[0]: entry for entry in check_commands(Path('/src'))}
        for name in ('doc-anchors', 'doc-lint', 'doc-anchors-tests', 'doc-lint-tests',
                     'doc-pine-coverage', 'design-inventory', 'design-inventory-tests',
                     'doc-reverts', 'doc-reverts-tests',
                     'kernel-seam-rows', 'kernel-seam-rows-tests'):
            self.assertIn(name, plan)
            self.assertEqual(len(plan[name]), 2, name)

    def test_no_stage_is_advisory_today(self):
        for entry in check_commands(Path('/src')):
            self.assertFalse(len(entry) > 2 and entry[2], entry[0])

    def test_success_is_explicitly_only_preflight(self):
        code, summary, _ = self.run_preflight([('ok', [sys.executable, '-c', 'pass'])])
        self.assertEqual(code, 0)
        self.assertEqual(summary['status'], 'passed')
        self.assertIn('full verification still required', summary['scope'])


if __name__ == '__main__':
    unittest.main()
