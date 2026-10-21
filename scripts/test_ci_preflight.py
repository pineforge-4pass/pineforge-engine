#!/usr/bin/env python3
"""Exercise preflight failure propagation with real child processes."""
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import ci_preflight
import ci_verify
from ci_verifier_classes import select_test_ids, verifier_class_tier
from run_unittest_shards import discover_test_ids
from ci_preflight import (AUTHZ_ALWAYS, AUTHZ_REFUSE_STEP, CORES,
                          DOCS_ONLY_SKIP, FLAT_PEAK_SKIP, HOSTED_MATRIX_RUNNER, JOB_RUNNERS,
                          PR_RUNNER, MATRIX_LINUX_RUNNER, MATRIX_RUNNER,
                          NATIVE_EXCLUSION, NATIVE_CACHE_SAVE, CURL_CACHE_SAVE,
                          PROOF_JOBS, RELEASE_RUNNER, _jobs, check_commands,
                          ci_workflow_findings, gated_workflow_findings,
                          native_workflow_findings, run_checks)

ROOT = Path(__file__).resolve().parents[1]
PR_SLOW_FLAG = "${{ github.event_name == 'pull_request' && '--exclude-label slow' || '' }}"
KERNEL_VERIFY = ('run: python3 scripts/ci_verify.py kernel --build-dir build-kernel '
                 '--jobs "$(getconf _NPROCESSORS_ONLN)" --ccache '
                 '--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" '
                 + PR_SLOW_FLAG)
# ci_workflow_findings' arguments, in order.
CI_SOURCES = ('.github/workflows/ci.yml', '.github/workflows/native-live.yml',
              '.github/workflows/promote-baseline.yml', 'tests/CMakeLists.txt',
              '.github/workflows/corpus-parity.yml', '.github/workflows/docs.yml')
# Independent of the guard's tables: deleting a pin must not delete its test.
GCP_JOBS = {
    'ci.yml': ('pr', ('authz', 'changes', 'preflight', 'build', 'sanitizers',
                      'kernel-only', 'build-gate')),
    'native-live.yml': ('pr', ('authz', 'native-live', 'native-live-gate')),
    'corpus-parity.yml': ('pr', ('authz', 'corpus-parity', 'corpus-parity-subset')),
    'docs.yml': ('pr', ('authz', 'build')),
    'release.yml': ('main', ('release', 'prebuilt', 'notify-hub')),
    'promote-baseline.yml': ('main', ('promote',)),
}
BUILD_RUNNER_FINDING = ('ci.yml build must use exactly the pf-gcp-pr Linux and hosted macos-26 build types, '
                        'excluding only macOS Debug on pull_request')
RELEASE_MATRIX_FINDING = ('release.yml prebuilt must use exactly the isolated Linux x64 '
                          'and hosted ARM/macOS matrix')


def job_script(ci, job, step_name=None):
    """A job's run block, exactly as the runner receives it."""
    body = _jobs(ci)[job]
    if step_name is not None:
        body = ci_preflight._step_named(ci_preflight._steps(body), '      - name: ' + step_name)
    lines = body.split('\n')
    start = next(index for index, line in enumerate(lines) if line.strip() == 'run: |')
    depth = len(lines[start + 1]) - len(lines[start + 1].lstrip(' '))
    script = []
    for line in lines[start + 1:]:
        if line.strip() and len(line) - len(line.lstrip(' ')) < depth:
            break
        script.append(line[depth:])
    return '\n'.join(script).rstrip('\n') + '\n'


def in_job(workflow, job, before, after):
    """Replace the first `before` inside job `job` (anywhere when job is None)."""
    start = 0 if job is None else workflow.index(f'\n  {job}:')
    if job is not None and before not in _jobs(workflow)[job]:
        raise AssertionError(f'{before!r} is not in job {job}')
    at = workflow.index(before, start)
    return workflow[:at] + after + workflow[at + len(before):]


def routed_line(before, value):
    """Keep the CI matrix's JSON string shape when replacing a literal array."""
    if before.startswith("            runner: '") and value.startswith('['):
        value = "'" + value + "'"
    return before.split(': ', 1)[0] + ': ' + value + '\n'


class PreflightFailures(unittest.TestCase):
    def ci_sources(self):
        return [(ROOT / path).read_text() for path in CI_SOURCES]

    def other_sources(self):
        """Every other workflow file, by name: ci_workflow_findings' ``others``."""
        named = {Path(path).name for path in CI_SOURCES}
        return {path.name: path.read_text()
                for path in sorted((ROOT / '.github/workflows').iterdir())
                if path.suffix in ('.yml', '.yaml') and path.name not in named}

    def workflow_sources(self):
        return {Path(path).name: contents for path, contents in zip(CI_SOURCES, self.ci_sources())
                if path.endswith(('.yml', '.yaml'))} | self.other_sources()

    def gcp_sites(self):
        """All 19 fleet sites, including the two matrix values."""
        for name, (fleet, jobs) in GCP_JOBS.items():
            runner = PR_RUNNER if fleet == 'pr' else RELEASE_RUNNER
            for job in jobs:
                if (name, job) == ('ci.yml', 'build'):
                    yield name, job, fleet, f'            runner: {MATRIX_LINUX_RUNNER}\n', BUILD_RUNNER_FINDING
                elif (name, job) == ('release.yml', 'prebuilt'):
                    yield name, job, fleet, f'            runner: {RELEASE_RUNNER}\n', RELEASE_MATRIX_FINDING
                else:
                    value = ('["self-hosted", "pf-gcp-pr"]'
                             if job == 'authz' or name == 'docs.yml' else runner)
                    finding = f'{name} job {job} must run on {runner}'
                    if fleet == 'main':
                        finding += ' (isolated release)'
                    yield name, job, fleet, f'    runs-on: {value}\n', finding

    def routed_file_findings(self, name, job, before, after):
        workflows = self.workflow_sources()
        changed = in_job(workflows[name], job, before, after)
        self.assertNotEqual(changed, workflows[name], 'the mutation must change the input')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / name
            path.write_text(changed)
            workflows[name] = path.read_text()
            return ci_preflight.runner_findings(workflows)

    def mutated_file_findings(self, index, job, before, after):
        """Write a mutation, then let the contract consume the file read back."""
        sources = self.ci_sources()
        changed = in_job(sources[index], job, before, after)
        self.assertNotEqual(changed, sources[index], 'the mutation must change the input')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / Path(CI_SOURCES[index]).name
            path.write_text(changed)
            sources[index] = path.read_text()
            return ci_workflow_findings(*sources, others=self.other_sources())

    def assert_gated_mutation(self, index, job, before, after, finding):
        """The direct pin and CI's contract must both read and refuse the mutation."""
        sources = self.ci_sources()
        changed = in_job(sources[index], job, before, after)
        self.assertNotEqual(changed, sources[index], 'the mutation must change the input')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / Path(CI_SOURCES[index]).name
            path.write_text(changed)
            sources[index] = path.read_text()
            self.assertIn(finding, gated_workflow_findings(sources[index], path.name))
            self.assertIn(finding, ci_workflow_findings(*sources, others=self.other_sources()))

    def test_pool_jobs_reject_hosted_runners_and_fallbacks_from_mutated_files(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        for index in (0, 1, 4, 5):
            name = Path(CI_SOURCES[index]).name
            for job, (runner, _) in JOB_RUNNERS[name].items():
                # The matrix expression is pinned together with its include values.
                marker = '    runs-on: ' + ('["self-hosted", "pf-gcp-pr"]'
                                            if job == 'authz' or name == 'docs.yml' else runner)
                for replacement in ('ubuntu-24.04', 'ubuntu-latest', 'pf-linux-x64-16',
                                    "'" + (PR_RUNNER if job == 'build' else runner) + "'",
                                    "${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}"):
                    with self.subTest(workflow=name, job=job, runner=replacement):
                        findings = self.mutated_file_findings(index, job, marker,
                                                              '    runs-on: ' + replacement)
                        self.assertIn(f'{name} job {job} must run on {runner}', findings)
        for replacement in ("'ubuntu-24.04'", "'" + '["ubuntu-24.04"]' + "'",
                            "${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}"):
            with self.subTest(matrix_runner=replacement):
                findings = self.mutated_file_findings(0, 'build',
                    '            runner: ' + MATRIX_LINUX_RUNNER,
                    '            runner: ' + replacement)
                self.assertIn(BUILD_RUNNER_FINDING, findings)

    def test_authz_and_every_dependency_are_required_from_mutated_files(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original, others=self.other_sources()), [])
        for index in (0, 1):
            name = Path(CI_SOURCES[index]).name
            authz = '  authz:' + _jobs(original[index])['authz']
            findings = self.mutated_file_findings(index, None, authz, '')
            self.assertIn(f'{name} must start with the verbatim canon authz job', findings)
            # A no-op refusal changes the real canon script and must be caught.
            findings = self.mutated_file_findings(index, 'authz',
                'exit 1; }', ':; }')
            self.assertIn(f'{name} must start with the verbatim canon authz job', findings)
            findings = self.mutated_file_findings(index, 'authz',
                '    permissions: {}\n', '    permissions: {}\n    timeout-minutes: 360\n')
            self.assertIn(f'{name} job authz must declare no timeout-minutes (owner #827)', findings)
            for job, body in _jobs(original[index]).items():
                if job == 'authz':
                    continue
                needs = ci_preflight._job_values(body, 'needs')[0]
                without = [dependency for dependency in ci_preflight._needs(body) if dependency != 'authz']
                for after in ('', '    needs: [' + ', '.join(without) + ']\n'):
                    with self.subTest(workflow=name, job=job, needs=after):
                        findings = self.mutated_file_findings(index, job,
                                                              '    needs: ' + needs + '\n', after)
                        self.assertIn(f'{name} job {job} must need authz', findings)
        for after in ('  pull_request:\n    branches: [main]\n',
                      '  pull_request:\n    paths: [src/**]\n'):
            findings = self.mutated_file_findings(0, None, '  pull_request:\n', after)
            self.assertIn('ci.yml must run on every pull_request without event filters', findings)

    def test_gated_workflows_accept_current_text(self):
        sources = self.ci_sources()
        for index in (4, 5):
            name = Path(CI_SOURCES[index]).name
            with self.subTest(workflow=name):
                self.assertEqual(gated_workflow_findings(sources[index], name), [])
        self.assertEqual(ci_workflow_findings(*sources, others=self.other_sources()), [])

    def test_gated_workflows_refuse_a_removed_authz_job(self):
        sources = self.ci_sources()
        for index in (4, 5):
            name = Path(CI_SOURCES[index]).name
            authz = '  authz:' + _jobs(sources[index])['authz']
            with self.subTest(workflow=name):
                self.assert_gated_mutation(index, None, authz, '',
                    f'{name} must start with the verbatim canon authz job')

    def test_gated_workflows_require_authz_for_every_dependent_job(self):
        for index, job in ((4, 'corpus-parity'), (4, 'corpus-parity-subset'), (5, 'build')):
            name = Path(CI_SOURCES[index]).name
            for after in ('', '    needs: [wrong-authz]\n'):
                with self.subTest(workflow=name, job=job, dependency=after):
                    self.assert_gated_mutation(index, job, '    needs: [authz]\n', after,
                        f'{name} job {job} must need authz')

    def test_gated_workflows_refuse_a_one_character_authz_script_change(self):
        for index in (4, 5):
            name = Path(CI_SOURCES[index]).name
            with self.subTest(workflow=name):
                self.assert_gated_mutation(index, 'authz',
                    '          set -eu\n', '          set -ev\n',
                    f'{name} must start with the verbatim canon authz job')

    def test_gated_corpus_refuses_hosted_fallback_expressions(self):
        fallback = "${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}"
        for job in ('corpus-parity', 'corpus-parity-subset'):
            with self.subTest(job=job):
                self.assert_gated_mutation(4, job, f'    runs-on: {PR_RUNNER}\n',
                    f'    runs-on: {fallback}\n',
                    f'corpus-parity.yml job {job} must run on {PR_RUNNER}')

    def test_gated_docs_refuses_a_hosted_build_runner(self):
        self.assert_gated_mutation(5, 'build',
            '    runs-on: ["self-hosted", "pf-gcp-pr"]\n',
            '    runs-on: ubuntu-latest\n',
            f'docs.yml job build must run on {PR_RUNNER}')

    def test_gated_workflows_refuse_pull_request_target_and_unreadable_events(self):
        for index in (4, 5):
            name = Path(CI_SOURCES[index]).name
            for after in ('\non:\n  pull_request_target:\n', '\non: *unreadable\n'):
                with self.subTest(workflow=name, events=after):
                    self.assert_gated_mutation(index, None, '\non:\n', after,
                        f'{name} must use readable events without pull_request_target')

    def test_gated_workflows_require_authz_first(self):
        sources = self.ci_sources()
        for index in (4, 5):
            name = Path(CI_SOURCES[index]).name
            original = sources[index]
            authz = '  authz:' + _jobs(original)['authz']
            reordered = in_job(original, None, authz, '').rstrip('\n') + '\n' + authz
            unreadable = in_job(original, None, '\njobs:\n',
                '\njobs:\n  "pre-build":\n    runs-on: ["self-hosted", "pf-gcp-pr"]\n'
                '    steps:\n      - run: "true"\n')
            for after in (reordered, unreadable):
                with self.subTest(workflow=name, job_order=after):
                    self.assert_gated_mutation(index, None, original, after,
                        f'{name} must start with the verbatim canon authz job')

    def test_gated_workflows_refuse_authz_job_overrides_and_extra_steps(self):
        for index in (4, 5):
            name = Path(CI_SOURCES[index]).name
            mutations = [
                ('    name: authz\n', '    name: renamed\n'),
                ('    permissions: {}\n', '    permissions: read-all\n'),
                ('    steps:\n', '    steps:\n      - run: "true"\n'),
            ]
            mutations += [('    steps:\n', override + '    steps:\n') for override in (
                '    needs: [build]\n',
                '    if: always()\n',
                '    env: {}\n',
                '    uses: ./.github/workflows/ci.yml\n',
                '    strategy: {}\n',
                '    continue-on-error: true\n',
            )]
            for before, after in mutations:
                with self.subTest(workflow=name, override=after):
                    self.assert_gated_mutation(index, 'authz', before, after,
                        f'{name} must start with the verbatim canon authz job')

    def test_aggregates_refuse_first_even_when_authz_is_skipped_from_mutated_files(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        for index, job in ((0, 'build-gate'), (1, 'native-live-gate')):
            name = Path(CI_SOURCES[index]).name
            for after in ('', AUTHZ_REFUSE_STEP.replace('exit 1', 'true'),
                          AUTHZ_REFUSE_STEP.replace("!= 'success'", "== 'failure'"),
                          AUTHZ_REFUSE_STEP + '      - uses: actions/checkout@v4\n'):
                with self.subTest(workflow=name, mutation=after):
                    findings = self.mutated_file_findings(index, job, AUTHZ_REFUSE_STEP, after)
                    self.assertIn(f'{name} {job} must always run and refuse authz first', findings)
            # Keeping the refusal after checkout cannot satisfy refuse-first.
            findings = self.mutated_file_findings(index, job, AUTHZ_REFUSE_STEP,
                                                  '      - uses: actions/checkout@v4\n' + AUTHZ_REFUSE_STEP)
            self.assertIn(f'{name} {job} must always run and refuse authz first', findings)
            for condition in ('${{ !cancelled() }}', '${{ success() }}'):
                findings = self.mutated_file_findings(index, job, '    if: always()\n',
                                                      '    if: ' + condition + '\n')
                self.assertIn(f'{name} {job} must always run and refuse authz first', findings)

    def test_status_function_jobs_refuse_before_checkout_from_mutated_files(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        for job in ('preflight', 'build', 'sanitizers', 'kernel-only'):
            for after in ('', AUTHZ_REFUSE_STEP.replace('exit 1', 'true'),
                          '      - uses: actions/checkout@v4\n' + AUTHZ_REFUSE_STEP):
                with self.subTest(job=job, mutation=after):
                    findings = self.mutated_file_findings(0, job, AUTHZ_REFUSE_STEP, after)
                    self.assertIn(f'ci.yml job {job} with a status function must refuse authz first',
                                  findings)

    def test_all_status_functions_require_refusal_and_repository_guards_from_mutated_files(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original, others=self.other_sources()), [])
        body = _jobs(original[0])['preflight']
        steps = ci_preflight._steps(body)
        checkout = ci_preflight._step_named(steps, '      - uses: actions/checkout@v4')
        verify = ci_preflight._step_named(steps, '      - name: Verify workflow wiring and source guards')
        guard = f'        if: {ci_preflight.AUTHZ_SUCCESS}\n'
        unnamed_run = '      - run: python3 scripts/ci_preflight.py --output-dir build-ci-preflight\n'
        for function in ('always()', 'cancelled()', 'failure()', 'success()'):
            conditioned = in_job(body, None, f'    if: {AUTHZ_ALWAYS}\n',
                                 '    if: ${{ ' + function + " && needs.authz.result == 'success' }}\n")
            mutations = (
                (in_job(conditioned, None, AUTHZ_REFUSE_STEP, ''),
                 'ci.yml job preflight with a status function must refuse authz first'),
                (in_job(conditioned, None, checkout, in_job(checkout, None, guard, '')),
                 'ci.yml job preflight must guard repository steps on authz success'),
                (in_job(conditioned, None, verify, in_job(verify, None, guard, '')),
                 'ci.yml job preflight must guard repository steps on authz success'),
                (conditioned + '\n' + unnamed_run,
                 'ci.yml job preflight must guard repository steps on authz success'),
            )
            for after, finding in mutations:
                with self.subTest(function=function, mutation=after):
                    findings = self.mutated_file_findings(0, 'preflight', body, after)
                    self.assertIn(finding, findings)
        # An unnamed repository run is allowed with either canon positive guard.
        for condition in (ci_preflight.AUTHZ_SUCCESS, AUTHZ_ALWAYS):
            with self.subTest(positive_condition=condition):
                findings = self.mutated_file_findings(0, 'preflight', verify,
                    verify + '\n' + unnamed_run + f'        if: {condition}\n')
                self.assertEqual(findings, [])

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
            (0, '  flat-peak:\n', '  missing-flat-peak:\n'),
            (0, 'uses: ./.github/workflows/flat-peak.yml', 'uses: ./.github/workflows/native-live.yml'),
            (0, '  build-gate:\n', '  missing-gate:\n'),
            (0, 'passed "$SANITIZER_RESULT"', 'true'),
            (0, '[[ "$PREFLIGHT_RESULT" == "success" ]] && passed "$BUILD_RESULT"',
                'passed "$PREFLIGHT_RESULT" && passed "$BUILD_RESULT"'),
            (0, 'needs: [authz, changes, preflight, build,', 'needs: [authz, preflight, build,'),
            (0, '          CHANGES_RESULT: ${{ needs.changes.result }}\n', ''),
            (0, '          DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}\n',
                "          DOCS_ONLY: 'true'\n"),
            # The docs-only answer is the classifier's, never a constant.
            (0, '      docs_only: ${{ steps.classify.outputs.docs_only }}\n',
                "      docs_only: 'true'\n"),
            # The base's copy judges; the answer is the script's alone.
            (0, 'python3 "$RUNNER_TEMP/ci_docs_only.py" --root . --github-output "$GITHUB_OUTPUT"',
                'python3 scripts/ci_docs_only.py --github-output "$GITHUB_OUTPUT"'),
            (0, 'git show HEAD^1:scripts/ci_docs_only.py', 'git show HEAD:scripts/ci_docs_only.py'),
            (0, '--root . --github-output "$GITHUB_OUTPUT"\n',
                '--root . --github-output "$GITHUB_OUTPUT" || echo docs_only=true >> "$GITHUB_OUTPUT"\n'),
            (0, 'echo "docs_only=false" >> "$GITHUB_OUTPUT"', 'echo "docs_only=true" >> "$GITHUB_OUTPUT"'),
            (0, '          PUSH_FORCED: ${{ github.event.forced }}\n', ''),
            (0, '        id: classify\n', '        id: classify\n        continue-on-error: true\n'),
            (0, '  changes:\n', '  classify:\n'),
            (0, '  sanitizers:\n', '  sanitizers:\n    needs: preflight\n'),
            (0, "github.event_name == 'pull_request' && '--exclude-label slow'",
                "github.event_name == 'pull_request' && matrix.build_type == 'Debug' && '--exclude-label slow'"),
            (0, "github.event_name == 'pull_request' && '--exclude-label slow'", "'--exclude-label slow'"),
            (0, "exclude_slow: ${{ github.event_name == 'pull_request' }}", 'exclude_slow: true'),
            (1, '        default: false', '        default: true'),
            (1, NATIVE_EXCLUSION, ''),
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

    def test_build_matrix_excludes_only_macos_debug_on_pull_requests(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        condition = "github.event_name == 'pull_request'"
        excluded = '[{"os":"macos-26","build_type":"Debug"}]'
        exclusion = (f"        exclude: ${{{{ fromJSON({condition} && "
                     f"'{excluded}' || '[]') }}}}\n")
        self.assertIn(exclusion, _jobs(original[0])['build'])
        mutations = [
            # (a) The old four-leg PR matrix, or an empty exclusion, restores
            # macOS Debug on PRs.
            ('macOS Debug restored on PR', exclusion, ''),
            ('empty PR exclusion', exclusion, exclusion.replace(excluded, '[]')),
            # (b) An unconditional exclusion loses coverage on every full event.
            ('unconditional exclusion', exclusion,
             f"        exclude: ${{{{ fromJSON('{excluded}') }}}}\n"),
            ('inverted event condition', exclusion,
             exclusion.replace(condition, "github.event_name != 'pull_request'")),
            # Dropping an axis also loses full-event coverage.
            ('macOS axis removed', '        os: [ubuntu-24.04, macos-26]\n',
             '        os: [ubuntu-24.04]\n'),
            ('Debug axis removed', '        build_type: [Release, Debug]\n',
             '        build_type: [Release]\n'),
        ]
        for event in ('push', 'workflow_dispatch', 'schedule', 'release'):
            mutations.append((
                f'macOS Debug removed on {event}', exclusion,
                exclusion.replace(condition, f"({condition} || github.event_name == '{event}')")))
        # (c) Every other PR cell stays. (d) macOS Release in particular must
        # still provide the PR's own Release registration-floor proof.
        for os_name, build_type in (('ubuntu-24.04', 'Release'),
                                    ('ubuntu-24.04', 'Debug'),
                                    ('macos-26', 'Release')):
            removed = json.dumps([{'os': os_name, 'build_type': build_type}], separators=(',', ':'))
            mutations.extend((
                (f'{os_name} {build_type} excluded instead', exclusion,
                 exclusion.replace(excluded, removed)),
                (f'{os_name} {build_type} excluded too', exclusion,
                 exclusion.replace(excluded, excluded[:-1] + ',' + removed[1:])),
            ))
        include = '          - os: macos-26\n            runner: \'["macos-26"]\'\n'
        mutations.append((
            'macOS Debug reintroduced by include', include,
            include + '          - os: macos-26\n            build_type: Debug\n'
            '            runner: \'["macos-26"]\'\n'))
        for reason, before, after in mutations:
            with self.subTest(reason=reason):
                changed = original.copy()
                changed[0] = in_job(changed[0], 'build', before, after)
                self.assertNotEqual(changed[0], original[0])
                self.assertIn(BUILD_RUNNER_FINDING, ci_workflow_findings(*changed, others=others))

    def test_slow_exclusion_is_exactly_pr_matrix_kernel_and_sanitizers(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        flag = PR_SLOW_FLAG
        for job in ('build', 'kernel-only', 'sanitizers'):
            mutations = [
                '', flag + ' ' + flag,
                flag.replace('slow', 'slwo'),
                flag.replace('slow', 'other'),
                flag.replace("&& '--exclude-label", "&& matrix.build_type == 'Debug' && '--exclude-label"),
                flag.replace("&& '--exclude-label", "&& matrix.build_type == 'Release' && '--exclude-label"),
                "'--exclude-label slow'",
                # Keeping the text on an unrelated step does not prove that
                # the verifier itself receives the exclusion.
                f'\n      - run: echo "{flag}"',
            ]
            mutations += [flag.replace("'pull_request'", repr(event))
                          for event in ('push', 'workflow_dispatch', 'schedule', 'release')]
            for after in mutations:
                with self.subTest(job=job, after=after):
                    changed = original.copy()
                    changed[0] = in_job(changed[0], job, flag, after)
                    self.assertIn(f'{job} must exclude exactly slow on PRs only, once',
                                  ci_workflow_findings(*changed))
        # A fourth excluding job, or a parity subset with any exclusion, fails.
        # `preflight` is a steps job; `corpus-parity-subset` is a reusable-workflow
        # call (`uses:` + `with:`, no `steps:`), so the flag goes into its `with:`.
        for job, before, after in (
                ('preflight', '    steps:\n',
                 f'    steps:\n      - run: python3 scripts/ci_verify.py debug {flag}\n'),
                ('corpus-parity-subset', '      mode: subset\n',
                 f'      mode: subset\n      note: {flag}\n')):
            with self.subTest(job=job):
                changed = original.copy()
                changed[0] = in_job(changed[0], job, before, after)
                findings = ci_workflow_findings(*changed)
                self.assertIn(f'{job} must keep its full population', findings)
                self.assertIn('CI must carry exactly three PR-only slow exclusion flags', findings)

    def assert_full_event_populations(self, ci, release):
        jobs = _jobs(ci)
        # These exact PR-only expressions evaluate to empty on every other
        # event. Any additional flag, including a push-only expression, fails.
        self.assertEqual({job for job, body in jobs.items() if '--exclude-label' in body},
                         {'build', 'kernel-only', 'sanitizers'})
        for body in jobs.values():
            self.assertNotIn('--exclude-label', body.replace(PR_SLOW_FLAG, ''))
        self.assertNotIn('--exclude-label', release)
        self.assertNotIn('--class-tier', release)
        # Read the real workflow and CMake registration, rather than deriving
        # expectations from the same selector that constructs the commands.
        self.assertIn("exclude_slow: ${{ github.event_name == 'pull_request' }}",
                      jobs['native-live'])
        cmake = (ROOT / 'tests/CMakeLists.txt').read_text()
        row = re.search(r'add_test\(NAME test_ci_verify\s+(.*?)\)', cmake, re.DOTALL)
        self.assertIsNotNone(row)
        self.assertIn('scripts/test_ci_verify.py', row.group(1))
        self.assertNotIn('--class-tier', row.group(1))
        for profile in ('release', 'debug', 'kernel', 'sanitizers', 'native',
                        'live-sanitizers', 'live-tsan'):
            cfg = ci_verify.build_config([profile])
            self.assertEqual(ci_verify.ctest_command(cfg, cfg.build_dir, jobs=1, junit=False), [
                'ctest', '--test-dir', str(cfg.build_dir), '--output-on-failure',
                '--no-tests=error', '--parallel', '1', '--timeout', '0'])
        discovered = set(discover_test_ids(ROOT / 'scripts/test_ci_verify.py'))
        self.assertTrue(discovered)
        for event in ('push', 'workflow_dispatch', 'schedule', 'release', ''):
            for slow_answer in ('false', 'true', ''):
                with self.subTest(event=event, slow_answer=slow_answer):
                    env = {'GITHUB_EVENT_NAME': event, 'PINEFORGE_VERIFIER_SLOW': slow_answer}
                    self.assertEqual(verifier_class_tier(env), 'all')
                    self.assertEqual(set(select_test_ids(sorted(discovered),
                                                         verifier_class_tier(env))), discovered)
                    plan = {name: argv for name, argv in check_commands(ROOT, env=env)}
                    self.assertEqual(plan['verifier-tests'], [
                        sys.executable, str(ROOT / 'scripts/run_unittest_shards.py'),
                        str(ROOT / 'scripts/test_ci_verify.py'), '--log-dir',
                        str(ROOT / 'build-ci-preflight/verifier-tests-shards')])

    def test_main_and_release_keep_every_row_and_verifier_class(self):
        ci = self.ci_sources()[0]
        release = (ROOT / '.github/workflows/release.yml').read_text()
        self.assert_full_event_populations(ci, release)
        # A push path carrying an exclusion must break this coverage control.
        for job in ('build', 'kernel-only', 'sanitizers'):
            for flag in ("${{ github.event_name == 'push' && '--exclude-label slow' || '' }}",
                         '--exclude-label slow'):
                with self.subTest(job=job, flag=flag):
                    changed = in_job(ci, job, PR_SLOW_FLAG, PR_SLOW_FLAG + ' ' + flag)
                    with self.assertRaises(AssertionError):
                        self.assert_full_event_populations(changed, release)
                    self.assertNotEqual(ci_workflow_findings(
                        changed, *self.ci_sources()[1:]), [])
        with self.assertRaises(AssertionError):
            self.assert_full_event_populations(
                ci, in_job(release, 'release', '    steps:\n',
                           '    steps:\n      - run: python3 scripts/ci_verify.py release --exclude-label slow\n'))

    def test_verifier_stage_defers_slow_only_for_an_explicit_pr_answer(self):
        base = [sys.executable, str(ROOT / 'scripts/run_unittest_shards.py'),
                str(ROOT / 'scripts/test_ci_verify.py'), '--log-dir',
                str(ROOT / 'build-ci-preflight/verifier-tests-shards')]
        for answer, expected in (('false', base + ['--class-tier', 'fast']),
                                 ('true', base), ('', base), ('unknown', base)):
            with self.subTest(answer=answer):
                env = {'GITHUB_EVENT_NAME': 'pull_request', 'PINEFORGE_VERIFIER_SLOW': answer}
                plan = {name: argv for name, argv in check_commands(ROOT, env=env)}
                self.assertEqual(plan['verifier-tests'], expected)
        plan = {name: argv for name, argv in check_commands(
            ROOT, env={'GITHUB_EVENT_NAME': 'pull_request'})}
        self.assertEqual(plan['verifier-tests'], base)

    def test_verifier_classification_wiring_fails_closed(self):
        original, others = self.ci_sources(), self.other_sources()
        mutations = (
            ('changes', '      verifier_slow: ${{ steps.verifier.outputs.verifier_slow }}',
             "      verifier_slow: 'false'"),
            ('changes', 'git show HEAD^1:scripts/ci_verifier_paths.py',
             'git show HEAD:scripts/ci_verifier_paths.py'),
            ('changes', 'python3 "$RUNNER_TEMP/ci_verifier_paths.py"',
             'python3 scripts/ci_verifier_paths.py'),
            ('changes', 'echo "verifier_slow=true"', 'echo "verifier_slow=false"'),
            ('preflight', ci_preflight.PREFLIGHT_VERIFIER_SLOW, "'false'"),
            ('preflight', ci_preflight.PREFLIGHT_VERIFIER_SLOW,
             ci_preflight.PREFLIGHT_VERIFIER_SLOW.replace("|| 'true'", "|| 'false'")),
            ('preflight', '        run: python3 scripts/ci_preflight.py --output-dir build-ci-preflight',
             '        run: python3 scripts/ci_preflight.py --output-dir build-ci-preflight --class-tier fast'),
        )
        for job, before, after in mutations:
            with self.subTest(job=job, after=after):
                changed = original.copy()
                changed[0] = in_job(changed[0], job, before, after)
                self.assertNotEqual(ci_workflow_findings(*changed, others=others), [])

    def test_ci_contract_pins_spot_runners_time_limits_and_parallelism(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(PR_RUNNER, '["self-hosted","pf-gcp-pr"]')
        self.assertEqual(json.loads(PR_RUNNER), ['self-hosted', 'pf-gcp-pr'])
        self.assertEqual(MATRIX_LINUX_RUNNER, '\'["self-hosted","pf-gcp-pr"]\'')
        self.assertEqual(MATRIX_RUNNER, '${{ fromJSON(matrix.runner) }}')
        self.assertEqual(HOSTED_MATRIX_RUNNER, '${{ matrix.runner }}')
        self.assertEqual(RELEASE_RUNNER, '["self-hosted","pf-gcp-main"]')
        self.assertEqual(ci_preflight.HOSTED_JOB_RUNNERS, {})
        self.assertEqual(JOB_RUNNERS['corpus-parity.yml'], {
            'authz': (PR_RUNNER, None),
            'corpus-parity': (PR_RUNNER, ci_preflight.PARITY),
            'corpus-parity-subset': (PR_RUNNER, ci_preflight.PARITY),
        })
        self.assertEqual(JOB_RUNNERS['docs.yml'], {
            'authz': (PR_RUNNER, None), 'build': (PR_RUNNER, None),
        })
        self.assertEqual(ci_preflight.RELEASE_JOB_RUNNERS, {
            'release.yml': {
                'release': '["self-hosted","pf-gcp-main"]',
                'prebuilt': '${{ matrix.runner }}',
                'notify-hub': '["self-hosted","pf-gcp-main"]'},
            'promote-baseline.yml': {
                'promote': '["self-hosted","pf-gcp-main"]'},
        })
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        mutations = (
            # Each scoped pool job must keep its canon array without a hosted fallback.
            (0, 'preflight', PR_RUNNER, 'pf-linux-x64-16', f'ci.yml job preflight must run on {PR_RUNNER}'),
            (0, 'sanitizers', PR_RUNNER, 'macos-26-xlarge', f'ci.yml job sanitizers must run on {PR_RUNNER}'),
            (0, 'kernel-only', PR_RUNNER, 'ubuntu-latest', f'ci.yml job kernel-only must run on {PR_RUNNER}'),
            # Target runs-on exactly, never an earlier matrix reference or name.
            (0, 'build', f'    runs-on: {MATRIX_RUNNER}\n',
             '    runs-on: macos-26-large\n', f'ci.yml job build must run on {MATRIX_RUNNER}'),
            (0, 'build', f'    runs-on: {MATRIX_RUNNER}\n',
             '    runs-on: ${{ matrix.larger_runner }}\n', f'ci.yml job build must run on {MATRIX_RUNNER}'),
            (1, 'native-live', PR_RUNNER, 'pf-linux-x64-16', f'native-live.yml job native-live must run on {PR_RUNNER}'),
            (4, 'corpus-parity', PR_RUNNER, 'pf-linux-x64-16', f'corpus-parity.yml job corpus-parity must run on {PR_RUNNER}'),
            (4, 'corpus-parity-subset', PR_RUNNER, 'pf-linux-x64-16',
             f'corpus-parity.yml job corpus-parity-subset must run on {PR_RUNNER}'),
            # An event-based paid-runner route is forbidden, even for trusted events.
            (0, 'preflight', PR_RUNNER,
             "${{ github.event_name == 'push' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             f'ci.yml job preflight must run on {PR_RUNNER}'),
            (1, 'native-live', PR_RUNNER,
             "${{ github.event_name == 'workflow_dispatch' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             f'native-live.yml job native-live must run on {PR_RUNNER}'),
            (4, 'corpus-parity', PR_RUNNER,
             "${{ github.event_name == 'schedule' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             f'corpus-parity.yml job corpus-parity must run on {PR_RUNNER}'),
            (0, 'sanitizers', PR_RUNNER,
             "${{ github.event.pull_request.head.repo.full_name == github.repository "
             "&& 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             f'ci.yml job sanitizers must run on {PR_RUNNER}'),
            (0, 'native-live', 'uses: ./.github/workflows/native-live.yml',
             'uses: someone/else/.github/workflows/native-live.yml@main',
             'ci.yml job native-live calls a workflow the runner contract cannot see; '
             'call one under ./.github/workflows/'),
            # The routing includes and PR-only macOS Debug exclusion are exact.
            (0, 'build', 'os: [ubuntu-24.04, macos-26]', 'os: [ubuntu-24.04, macos-26-large]',
             BUILD_RUNNER_FINDING),
            (0, 'build', '        build_type: [Release, Debug]\n',
             '        build_type: [Release, Debug]\n        include:\n'
             '          - os: ubuntu-24.04\n            build_type: Debug\n',
             BUILD_RUNNER_FINDING),
            (0, 'build', '        build_type: [Release, Debug]\n',
             '        build_type: [Release, Debug]\n        include:\n'
             '          - os: ubuntu-24.04\n            larger_runner: ubuntu-24.04\n',
             'ci.yml must not define a larger_runner key; use the GCP fleet or hosted exceptions'),
            # Block forms of runs-on, and job ids the parser must still see.
            (5, 'build', '    runs-on: ["self-hosted", "pf-gcp-pr"]\n',
             '    runs-on:\n      group: pineforge-ci-large\n',
             f'docs.yml job build must run on {PR_RUNNER}'),
            (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n', '    runs-on:\n      - macos-26-xlarge\n',
             f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} (isolated release)'),
            (0, None, '  build-gate:\n', '  extra:\n    runs-on:\n      group: pineforge-ci-large\n'
             '    steps:\n      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner and time limit'),
            (0, None, '  build-gate:\n', '  extra:\n    "runs-on": pf-linux-x64-16\n'
             '    steps:\n      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner and time limit'),
            (5, 'build', '    runs-on: ["self-hosted", "pf-gcp-pr"]\n', '    "runs-on": pf-linux-x64-16\n',
             f'docs.yml job build must run on {PR_RUNNER}'),
            (5, 'build', '    runs-on: ["self-hosted", "pf-gcp-pr"]\n', '    runs-on : macos-26-xlarge\n',
             f'docs.yml job build must run on {PR_RUNNER}'),
            (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n', "    'runs-on': macos-26-xlarge\n",
             f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} (isolated release)'),
            (5, 'build', '    runs-on: ["self-hosted", "pf-gcp-pr"]\n', '',
             f'docs.yml job build must run on {PR_RUNNER}'),
            (5, None, '\njobs:\n', '\njobs:\n  pre_build:\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml job pre_build needs a pinned runner and time limit'),
            (5, None, '\njobs:\n', '\njobs:\n  "pre-build":\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml has jobs lines the runner contract cannot read'),
            (4, 'corpus-parity-subset', f'runs-on: {PR_RUNNER}', 'runs-on:\n      group: pineforge-ci-large',
             f'corpus-parity.yml job corpus-parity-subset must run on {PR_RUNNER}'),
            (0, 'build-gate', PR_RUNNER, 'pf-linux-x64-16',
             f'ci.yml job build-gate must run on {PR_RUNNER}'),
            (5, 'build', '    runs-on: ["self-hosted", "pf-gcp-pr"]\n', '    runs-on: pf-linux-x64-16\n',
             f'docs.yml job build must run on {PR_RUNNER}'),
            (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n', '    runs-on: macos-26-xlarge\n',
             f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} (isolated release)'),
            (0, None, '  build-gate:\n', '  extra:\n    runs-on: ubuntu-24.04\n    steps:\n'
             '      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner and time limit'),
            # No job or step may declare timeout-minutes (owner #827,
            # 2026-10-20). Seed the deleted 360 line before each timeout
            # mutation: changing its value still fails; removing it passes.
            (0, 'preflight', 'timeout-minutes: 360', 'timeout-minutes: 30',
             'ci.yml job preflight must declare no timeout-minutes (owner #827)'),
            (0, 'build', 'timeout-minutes: 360', 'timeout-minutes: 45',
             'ci.yml job build must declare no timeout-minutes (owner #827)'),
            (0, 'sanitizers', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'ci.yml job sanitizers must declare no timeout-minutes (owner #827)'),
            (0, 'kernel-only', 'timeout-minutes: 360', 'timeout-minutes: 45',
             'ci.yml job kernel-only must declare no timeout-minutes (owner #827)'),
            (0, 'build-gate', '    timeout-minutes: 360\n', '', None),
            (0, 'build-gate', f'    runs-on: {PR_RUNNER}\n',
             f'    runs-on: {PR_RUNNER}\n    timeout-minutes: 360\n',
             'ci.yml job build-gate must declare no timeout-minutes (owner #827)'),
            (0, 'changes', PR_RUNNER, 'pf-linux-x64-16',
             f'ci.yml job changes must run on {PR_RUNNER}'),
            (0, 'changes', '    timeout-minutes: 360\n', '    timeout-minutes: 300\n',
             'ci.yml job changes must declare no timeout-minutes (owner #827)'),
            (1, 'native-live', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'native-live.yml job native-live must declare no timeout-minutes (owner #827)'),
            (1, 'native-live-gate', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'native-live.yml job native-live-gate must declare no timeout-minutes (owner #827)'),
            (4, 'corpus-parity', 'timeout-minutes: 360', 'timeout-minutes: 30',
             'corpus-parity.yml job corpus-parity must declare no timeout-minutes (owner #827)'),
            (4, 'corpus-parity-subset', 'timeout-minutes: 360', 'timeout-minutes: 10',
             'corpus-parity.yml job corpus-parity-subset must declare no timeout-minutes (owner #827)'),
            # A fixed count cannot follow the runner's available cores.
            (0, 'build', f'--jobs {CORES}', '--jobs 4', 'ci.yml job build must size its parallelism to the runner'),
            (0, 'sanitizers', f'--jobs {CORES}', '--jobs 4', 'ci.yml job sanitizers must size its parallelism to the runner'),
            (0, 'kernel-only', f'--jobs {CORES}', '--jobs "$(nproc)"', 'ci.yml job kernel-only must size its parallelism to the runner'),
            (1, 'native-live', f'--jobs {CORES}', '--jobs 4', 'native-live.yml job native-live must size its parallelism to the runner'),
            (4, 'corpus-parity', f'JOBS={CORES} ./scripts', './scripts',
             'corpus-parity.yml job corpus-parity must size its parallelism to the runner'),
            (4, 'corpus-parity-subset', '          BUILD_DIR: build-corpus-parity-subset\n',
             '          BUILD_DIR: build-corpus-parity-subset\n          JOBS: "4"\n',
             'corpus-parity.yml job corpus-parity-subset must size its parallelism to the runner'),
            # ... through a multi-line run block, an = spelling, or a later flag.
            (0, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES}',
             'run: |\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'sanitizers', f'--jobs {CORES}', '--jobs=4', 'ci.yml job sanitizers must size its parallelism to the runner'),
            (0, 'build', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --jobs=4',
             'ci.yml job build must size its parallelism to the runner'),
            (0, 'kernel-only', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --job 4',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'sanitizers', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --jo=4',
             'ci.yml job sanitizers must size its parallelism to the runner'),
            (0, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
             'working-directory: scripts\n        run: python3 ci_verify.py kernel --build-dir ../build-kernel --ccache',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
             'run: $VERIFY', 'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n'
             '      - run: PYTHONPATH=scripts python3 -m ci_verify kernel --build-dir build-kernel --ccache\n',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            # A second, unsized call however it is chained, blocked or carried.
            (0, 'kernel-only', KERNEL_VERIFY,
             f'run: >-\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache'
             '\n\n          python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - uses: nick-fields/retry@v3\n        with:\n'
             '          command: python3 scripts/ci_verify.py release --build-dir build-rel --ccache\n',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY,
             KERNEL_VERIFY + ' && python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY,
             KERNEL_VERIFY + '; python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - run: python3 -X dev scripts/ci_verify.py release --build-dir build-rel --ccache\n',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - run: $pythonLocation/bin/python3 scripts/ci_verify.py release '
             '--build-dir build-rel --ccache\n',
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (0, 'kernel-only', KERNEL_VERIFY, KERNEL_VERIFY + " ${{ '--jobs 4' }}",
             'ci.yml job kernel-only must size its parallelism to the runner'),
            (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
             f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset'
             ' && JOBS=4 ./scripts/check_corpus_parity.sh --subset',
             'corpus-parity.yml job corpus-parity-subset must size its parallelism to the runner'),
            (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
             'run: |\n          JOBS=4 ./scripts/check_corpus_parity.sh --subset',
             'corpus-parity.yml job corpus-parity-subset must size its parallelism to the runner'),
            (4, 'corpus-parity', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh',
             'run: |\n          export JOBS=4\n          ./scripts/check_corpus_parity.sh',
             'corpus-parity.yml job corpus-parity must size its parallelism to the runner'),
            # The build legs keep their names: build (<image>, <type>).
            (0, 'build', '    name: build (${{ matrix.os }}, ${{ matrix.build_type }})\n', '',
             'ci.yml build legs must keep their names: build (${{ matrix.os }}, ${{ matrix.build_type }})'),
        )
        for index, job, before, after, finding in mutations:
            with self.subTest(file=CI_SOURCES[index], job=job, after=after):
                changed = original.copy()
                timeout_mutation = 'timeout-minutes:' in before
                if timeout_mutation:
                    self.assertNotIn('timeout-minutes:', _jobs(changed[index])[job])
                    changed[index] = in_job(changed[index], job, '    steps:\n',
                                            '    timeout-minutes: 360\n    steps:\n')
                    message = (f'{Path(CI_SOURCES[index]).name} job {job} '
                               'must declare no timeout-minutes (owner #827)')
                    self.assertEqual(ci_workflow_findings(*changed, others=others), [message])
                changed[index] = in_job(changed[index], job, before, after)
                findings = ci_workflow_findings(*changed, others=others)
                if timeout_mutation:
                    self.assertEqual(findings, [] if finding is None else [finding])
                else:
                    self.assertIn(finding, findings)

    def test_verifier_looking_step_name_cannot_hide_an_unsized_call_from_mutated_file(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original, others=self.other_sources()), [])
        before = ci_preflight._step_named(ci_preflight._steps(_jobs(original[0])['kernel-only']),
                                          '      - name: Verify (kernel)')
        # Match name and run independently, preserving any intervening if/env keys.
        after = in_job(before, None, '      - name: Verify (kernel)\n',
                       f'      - name: python3 scripts/ci_verify.py kernel --jobs {CORES}\n')
        after = in_job(after, None,
                       f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
                       'run: python3 -m ci_verify kernel --build-dir build-kernel --ccache')
        self.assertEqual(ci_preflight._step_values(after, 'if'), [ci_preflight.AUTHZ_SUCCESS])
        findings = self.mutated_file_findings(0, 'kernel-only', before, after)
        self.assertIn('ci.yml job kernel-only must size its parallelism to the runner', findings)

    def test_gcp_sites_reject_cross_class_routes(self):
        sites = list(self.gcp_sites())
        self.assertEqual(len(sites), 19)
        self.assertEqual(ci_preflight.runner_findings(self.workflow_sources()), [])
        for name, job, fleet, before, finding in sites:
            runner = RELEASE_RUNNER if fleet == 'pr' else PR_RUNNER
            with self.subTest(workflow=name, job=job, wrong_fleet=runner):
                findings = self.routed_file_findings(name, job, before, routed_line(before, runner))
                self.assertIn(finding, findings)

    def test_gcp_sites_reject_retired_and_void_labels(self):
        for name, job, fleet, before, finding in self.gcp_sites():
            runner = PR_RUNNER if fleet == 'pr' else RELEASE_RUNNER
            for label in ('pf-ci', 'pf-ci-x64', 'pf-ci-x64-16', 'pf-ci-release',
                          'pf-gcp-engine-pr', 'pf-gcp-engine-main', 'pf-gcp-small'):
                # Refuse the wrong class alone and extra labels on a valid class.
                for value in (f'["self-hosted","{label}"]', runner[:-1] + f',"{label}"]'):
                    with self.subTest(workflow=name, job=job, runner=value):
                        findings = self.routed_file_findings(name, job, before, routed_line(before, value))
                        self.assertIn(finding, findings)
            for value in ('["self-hosted","pf-ci","pf-ci-x64"]',
                          '["self-hosted","pf-ci","pf-ci-x64-16"]',
                          '["self-hosted","pf-ci","pf-ci-release"]',
                          '["self-hosted","pf-gcp-pr","pf-gcp-main"]'):
                with self.subTest(workflow=name, job=job, runner=value):
                    findings = self.routed_file_findings(name, job, before, routed_line(before, value))
                    self.assertIn(finding, findings)

    def test_gcp_sites_reject_hosted_fallbacks(self):
        for name, job, fleet, before, finding in self.gcp_sites():
            runner = PR_RUNNER if fleet == 'pr' else RELEASE_RUNNER
            for value in ('ubuntu-24.04', 'ubuntu-latest',
                          "${{ fromJSON('" + runner + "') || 'ubuntu-24.04' }}"):
                with self.subTest(workflow=name, job=job, runner=value):
                    findings = self.routed_file_findings(name, job, before, routed_line(before, value))
                    self.assertIn(finding, findings)

    def test_gcp_sites_require_the_literal_or_matrix_string_shape(self):
        for name, job, fleet, before, finding in self.gcp_sites():
            runner = PR_RUNNER if fleet == 'pr' else RELEASE_RUNNER
            # CI explicitly decodes its matrix string; every other fleet
            # site needs an array rather than a quoted JSON-looking scalar.
            values = (runner,) if (name, job) == ('ci.yml', 'build') else (
                "'" + runner + "'", json.dumps(runner))
            for value in values:
                with self.subTest(workflow=name, job=job, runner=value):
                    after = before.split(': ', 1)[0] + ': ' + value + '\n'
                    findings = self.routed_file_findings(name, job, before, after)
                    self.assertIn(finding, findings)

    def test_gcp_sites_require_every_routed_job(self):
        workflows = self.workflow_sources()
        for name, (_, jobs) in GCP_JOBS.items():
            for job in jobs:
                with self.subTest(workflow=name, missing_job=job):
                    before = '  ' + job + ':' + _jobs(workflows[name])[job]
                    findings = self.routed_file_findings(name, None, before, '')
                    self.assertIn(f'{name} is missing job {job}', findings)

    def test_authz_header_spacing_and_comment_are_verbatim(self):
        comment = ('    # GCP fleet (owner #1050): non-release jobs on pf-gcp-pr; '
                   'release-class jobs only on pf-gcp-main.\n')
        for index in (0, 1, 4, 5):
            name = Path(CI_SOURCES[index]).name
            for before, after in (
                    ('    runs-on: ["self-hosted", "pf-gcp-pr"]\n',
                     '    runs-on: ["self-hosted","pf-gcp-pr"]\n'),
                    (comment, comment.replace('#1050', '#1056')),
                    (comment, comment.replace('only on', 'on')),
                    (comment, '')):
                with self.subTest(workflow=name, replacement=after):
                    findings = self.mutated_file_findings(index, 'authz', before, after)
                    self.assertEqual(findings, [f'{name} must start with the verbatim canon authz job'])

    def test_corpus_routes_reject_the_legacy_conditional(self):
        legacy = '''>-
      ${{ github.event_name != 'pull_request_target' &&
          github.actor != 'dependabot[bot]' && github.triggering_actor != 'dependabot[bot]' &&
          contains(fromJSON('["push","pull_request","workflow_dispatch","schedule","release"]'), github.event_name) &&
          (github.event_name != 'pull_request' ||
           (github.event.pull_request.head.repo.full_name == github.repository && github.event.pull_request.head.repo.fork == false)) &&
          fromJSON('["self-hosted","pf-ci","pf-ci-x64-16"]') || 'ubuntu-24.04' }}'''
        # The whole expression and variations of its former event/bot/fork
        # guards are all invalid now: each corpus route must be a literal.
        replacements = (
            legacy,
            legacy.replace('["self-hosted","pf-ci","pf-ci-x64-16"]', PR_RUNNER),
            legacy.replace("          github.actor != 'dependabot[bot]' && "
                           "github.triggering_actor != 'dependabot[bot]' &&\n", ''),
            legacy.replace("github.actor != 'dependabot[bot]' && ", ''),
            legacy.replace(" && github.triggering_actor != 'dependabot[bot]'", ''),
            legacy.replace("'ubuntu-24.04'", "'ubuntu-latest'"),
            legacy.replace(' && github.event.pull_request.head.repo.fork == false', ''),
            legacy.replace("github.event_name != 'pull_request_target' &&", 'true &&'),
            legacy.replace('"schedule","release"', '"schedule","release","workflow_run"'),
        )
        for job in ('corpus-parity', 'corpus-parity-subset'):
            for value in replacements:
                with self.subTest(job=job, expression=value):
                    before = f'    runs-on: {PR_RUNNER}\n'
                    findings = self.routed_file_findings('corpus-parity.yml', job, before,
                                                         routed_line(before, value))
                    self.assertIn(f'corpus-parity.yml job {job} must run on {PR_RUNNER}', findings)

    def test_legacy_metadata_cannot_replace_runner_pins(self):
        workflows = self.workflow_sources()
        # Docs has no legacy PF_CI_LABELS declaration; the ten original
        # metadata-bearing jobs keep this independent population check.
        sites = [site for site in self.gcp_sites()
                 if site[2] == 'pr' and site[1] != 'authz' and site[0] != 'docs.yml']
        self.assertEqual(len(sites), 10)
        for name, job, _, before, finding in sites:
            env = ci_preflight._job_block(_jobs(workflows[name])[job], 'env')
            markers = [line + '\n' for line in env if ci_preflight._key_value(line, 'PF_CI_LABELS') is not None]
            self.assertEqual(len(markers), 1)
            # These old declarations are not scheduler inputs. Removing or
            # updating one cannot excuse a wrong literal or matrix runner.
            for replacement in ('', f"      PF_CI_LABELS: '{PR_RUNNER}'\n",
                                f"      PF_CI_LABELS: '{RELEASE_RUNNER}'\n"):
                with self.subTest(workflow=name, job=job, metadata=replacement):
                    changed = in_job(workflows[name], job, markers[0], replacement)
                    self.assertEqual(ci_preflight.runner_findings(dict(workflows, **{name: changed})), [])
                    changed = in_job(changed, job, before, routed_line(before, RELEASE_RUNNER))
                    findings = ci_preflight.runner_findings(dict(workflows, **{name: changed}))
                    self.assertIn(finding, findings)
        # A step-level declaration also cannot stand in for the actual runner.
        before = f'    runs-on: {PR_RUNNER}\n'
        changed = in_job(workflows['ci.yml'], 'preflight', before, routed_line(before, RELEASE_RUNNER))
        changed = in_job(changed, 'preflight', '      - uses: actions/checkout@v4\n',
                         '      - uses: actions/checkout@v4\n        env:\n'
                         f"          PF_CI_LABELS: '{PR_RUNNER}'\n")
        findings = ci_preflight.runner_findings(dict(workflows, **{'ci.yml': changed}))
        self.assertIn(f'ci.yml job preflight must run on {PR_RUNNER}', findings)

    def test_ci_contract_pins_the_docs_only_skip(self):
        """Each proof job waits for authz and changes and skips only its docs-only
        answer; preflight also waits for the class decision and always runs."""
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        skip = f'    if: {DOCS_ONLY_SKIP}\n'
        mutations = []
        for job in PROOF_JOBS:
            job_skip = f'    if: {FLAT_PEAK_SKIP if job == "flat-peak" else DOCS_ONLY_SKIP}\n'
            refused = f'{job} must start alongside preflight, after authz and changes'
            mutations += [
                (job, '    needs: [authz, changes]\n', '', refused),
                (job, '    needs: [authz, changes]\n', '    needs: [changes, preflight]\n', refused),
                (job, '    needs: [authz, changes]\n', '    needs: preflight\n', refused),
                (job, job_skip, '', refused),
                # A failed or unanswered classification must run the job.
                (job, job_skip, "    if: ${{ needs.changes.outputs.docs_only != 'true' }}\n", refused),
                (job, job_skip, "    if: ${{ !cancelled() && needs.changes.outputs.docs_only == 'false' }}\n",
                 refused),
                (job, job_skip, job_skip.replace("!= 'true'", "!= 'true' && github.event_name != 'push'"),
                 refused),
                # A skip the aggregate would not accept: a failed classification.
                (job, job_skip, "    if: ${{ !cancelled() && needs.changes.outputs.docs_only != 'true' }}\n",
                 refused)]
        mutations += [
            ('preflight', '    needs: [authz, changes]\n', '',
             'preflight must follow authz and changes'),
            ('preflight', '    needs: [authz, changes]\n', '    needs: build\n',
             'preflight must follow authz and changes'),
            ('preflight', f'    if: {AUTHZ_ALWAYS}\n', skip,
             'preflight must follow authz and changes'),
            ('preflight', f'    if: {AUTHZ_ALWAYS}\n', '',
             'preflight must follow authz and changes'),
            ('changes', '    steps:\n', '    needs: preflight\n    steps:\n',
             'changes must start after authz'),
            ('changes', '    steps:\n',
             "    if: github.event_name != 'pull_request'\n    steps:\n",
             'changes must start after authz'),
            # A lane that continues on error reports success however it ended.
            ('sanitizers', '    steps:\n', '    continue-on-error: true\n    steps:\n',
             'ci.yml job sanitizers must not continue on error'),
            ('build-gate', '        shell: bash\n', '        shell: bash\n        continue-on-error: true\n',
             'ci.yml job build-gate must not continue on error'),
            ('preflight', '        run: python3 scripts/ci_preflight.py --output-dir build-ci-preflight\n',
             '        run: python3 scripts/ci_preflight.py --output-dir build-ci-preflight\n'
             '        continue-on-error: true\n', 'ci.yml job preflight must not continue on error')]
        for job, before, after, finding in mutations:
            with self.subTest(job=job, after=after):
                changed = original.copy()
                changed[0] = in_job(changed[0], job, before, after)
                findings = ci_workflow_findings(*changed, others=others)
                self.assertTrue(any(finding in line for line in findings), findings)
        # The same wiring spelled another way is no finding.
        for job, before, after in (
                ('kernel-only', '    needs: [authz, changes]\n', '    needs: [changes, authz]\n'),
                ('sanitizers', skip, f'    if: "{DOCS_ONLY_SKIP}"\n'),
                ('changes', '    steps:\n', '    # classify first\n    steps:\n')):
            with self.subTest(job=job, harmless=after):
                changed = original.copy()
                changed[0] = in_job(changed[0], job, before, after)
                self.assertEqual(ci_workflow_findings(*changed, others=others), [])

    def test_build_gate_rejects_missing_matrix_exceptions_on_push(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        passed = ('          passed() { [[ "$1" == "success" || '
                  '( "$docs_only" == "true" && "$1" == "skipped" ) ]]; }\n')
        mutations = (
            # (e) Neither an empty result nor a skipped matrix on push is the
            # PR's absent cell; accepting either must fail the exact gate pin.
            (passed, passed.replace('|| (', '|| -z "$1" || (')),
            (passed, passed.replace('|| (', '|| "$1" == "skipped" || (')),
            ('passed "$BUILD_RESULT"',
             '( passed "$BUILD_RESULT" || [[ "$GITHUB_EVENT_NAME" == "push" ]] )'),
            ('          BUILD_RESULT: ${{ needs.build.result }}\n',
             "          BUILD_RESULT: ${{ needs.build.result || 'success' }}\n"),
            ('          docs_only=false\n', '          docs_only=true\n'),
            ('            && passed "$FLAT_PEAK_RESULT"\n',
             '            && passed "$FLAT_PEAK_RESULT"\n'
             '          true\n'),
        )
        for before, after in mutations:
            with self.subTest(after=after):
                changed = original.copy()
                changed[0] = in_job(changed[0], 'build-gate', before, after)
                self.assertNotEqual(changed[0], original[0])
                self.assertIn(
                    'build-gate must keep the exact result policy: success or a classified docs-only skip',
                    ci_workflow_findings(*changed, others=others))

    def test_build_aggregate_requires_matrix_success_on_pr_and_full_events(self):
        script = job_script(self.ci_sources()[0], 'build-gate',
                            'Require every verification lane to have passed')
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
               'AUTHZ_RESULT': 'success',
               'CHANGES_RESULT': 'success', 'DOCS_ONLY': 'false',
               'PREFLIGHT_RESULT': 'success',
               **{variable: 'success' for variable in PROOF_JOBS.values()}}
        for event in ('pull_request', 'push', 'workflow_dispatch', 'schedule', 'release'):
            for outcome in ('success', 'failure', 'cancelled', 'skipped', ''):
                with self.subTest(event=event, matrix=outcome):
                    result = subprocess.run(
                        ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
                        env={**env, 'GITHUB_EVENT_NAME': event, 'BUILD_RESULT': outcome},
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0 if outcome == 'success' else 1,
                                     result.stdout + result.stderr)
                    self.assertIn(f'matrix: {outcome}\n', result.stdout)

    def test_the_build_aggregate_passes_a_docs_only_skip_and_nothing_else(self):
        """Run the aggregate's own shell over every outcome of every lane: a
        proof lane passes by succeeding, or by being skipped after changes
        succeeded and answered docs-only; preflight always has to succeed."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        script = Path(directory.name) / 'gate.sh'
        script.write_text(job_script(self.ci_sources()[0], 'build-gate',
                                     'Require every verification lane to have passed'))
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'AUTHZ_RESULT': 'success'}
        lanes = list(PROOF_JOBS.values())
        vectors = set()
        for common in ('success', 'skipped'):
            for odd in range(len(lanes)):
                for outcome in ('success', 'skipped', 'failure', 'cancelled'):
                    results = [common] * len(lanes)
                    results[odd] = outcome
                    vectors.add(tuple(results))
        cases = [(changes, docs_only, preflight, *results)
                 for preflight in ('success', 'failure', 'skipped', 'cancelled')
                 for changes in ('success', 'failure', 'skipped')
                 for docs_only in ('true', 'false', '') for results in sorted(vectors)]
        # One bash reads every case; each runs the step in a subshell with the
        # options GitHub gives a bash step (-e, pipefail).
        names = ['CHANGES_RESULT', 'DOCS_ONLY', 'PREFLIGHT_RESULT', *lanes]
        # The harness's own variables share no name with the step's.
        columns = [f'case_{name.lower()}' for name in names]
        driver = ('while read -r ' + ' '.join(columns) + '; do\n'
                  '  [ "$case_docs_only" = - ] && case_docs_only=\n'
                  '  ( set -eo pipefail\n'
                  '    export ' + ' '.join(f'{name}="${column}"' for name, column in zip(names, columns))
                  + '\n'
                  '    unset ' + ' '.join(columns) + '\n'
                  '    . "$1" ) >/dev/null 2>&1\n'
                  '  echo "$?"\n'
                  'done\n')
        answers = subprocess.run(
            ['bash', '--noprofile', '--norc', '-c', driver, 'gate', str(script)], env=env,
            input=''.join(' '.join(value or '-' for value in case) + '\n' for case in cases),
            capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(len(answers), len(cases))
        for case, answer in zip(cases, answers):
            changes, docs_only, preflight, *results = case
            excused = changes == 'success' and docs_only == 'true'
            expected = changes == 'success' and preflight == 'success' and all(
                result == 'success' or (excused and result == 'skipped') for result in results)
            with self.subTest(case=dict(zip(names, case))):
                self.assertEqual(answer == '0', expected, answer)
        # And as the runner starts it: the docs-only pass, and a skip that is not.
        docs_only = dict(zip(names, ('success', 'true', 'success', *['skipped'] * len(lanes))))
        for override, code in (({}, 0), ({'DOCS_ONLY': 'false'}, 1), ({'CHANGES_RESULT': 'failure'}, 1),
                               *[({'AUTHZ_RESULT': result}, 1)
                                 for result in ('failure', 'cancelled', 'skipped', '')]):
            with self.subTest(override=override):
                result = subprocess.run(['bash', '--noprofile', '--norc', '-eo', 'pipefail', str(script)],
                                        env={**env, **docs_only, **override}, capture_output=True, text=True)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertIn('preflight: success', result.stdout)
                self.assertIn('matrix: skipped', result.stdout)

    def test_the_classification_is_the_base_copy_of_the_rule(self):
        """Run the changes job's own shell in throwaway repositories: the
        parent's scripts/ci_docs_only.py judges the change, so a change that
        breaks the rule cannot skip its own proof jobs, and a parent without
        the script runs every job."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        step = root / 'classify.sh'
        step.write_text(job_script(self.ci_sources()[0], 'changes'))
        git_env = {'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1',
                   'GIT_AUTHOR_NAME': 'ci', 'GIT_AUTHOR_EMAIL': 'ci@example.invalid',
                   'GIT_COMMITTER_NAME': 'ci', 'GIT_COMMITTER_EMAIL': 'ci@example.invalid'}
        rule = (ROOT / 'scripts/ci_docs_only.py').read_text()
        broken = rule.replace('def is_documentation(path: str) -> bool:\n',
                              'def is_documentation(path: str) -> bool:\n    return True\n', 1)
        self.assertNotEqual(broken, rule)

        def classify(name, base_files, change):
            repo = root / name
            repo.mkdir()
            run = lambda *args: subprocess.run(['git', '-C', str(repo), *args], check=True,
                                               capture_output=True, text=True,
                                               env={**os.environ, **git_env}).stdout
            run('init', '--quiet', '--initial-branch=main')
            run('config', 'commit.gpgsign', 'false')
            for files in (base_files, change):
                for path, text in files.items():
                    (repo / path).parent.mkdir(parents=True, exist_ok=True)
                    (repo / path).write_text(text)
                run('add', '--all')
                run('commit', '--quiet', '--allow-empty', '-m', 'commit')
            before = run('rev-parse', 'HEAD^1').strip()
            temp, output = root / f'{name}-temp', root / f'{name}-output'
            temp.mkdir()
            output.write_text('')
            result = subprocess.run(
                ['bash', '-e', str(step)], cwd=repo, capture_output=True, text=True,
                env={**os.environ, **git_env, 'GITHUB_EVENT_NAME': 'push', 'PUSH_BEFORE': before,
                     'PUSH_FORCED': 'false', 'RUNNER_TEMP': str(temp), 'GITHUB_OUTPUT': str(output)})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return output.read_text(), result.stdout

        page = {'docs/ci.md': 'new\n'}
        answer, _ = classify('docs', {'scripts/ci_docs_only.py': rule, 'docs/ci.md': 'old\n'}, page)
        self.assertEqual(answer, 'docs_only=true\n')
        # A change that breaks the rule is judged by the rule it breaks.
        answer, out = classify('rule', {'scripts/ci_docs_only.py': rule, 'docs/ci.md': 'old\n'},
                               {'scripts/ci_docs_only.py': broken, 'src/engine.cpp': 'int x;\n', **page})
        self.assertEqual(answer, 'docs_only=false\n', out)
        self.assertIn('scripts/ci_docs_only.py', out)
        # A base without the rule runs every job.
        answer, out = classify('bare', {'docs/ci.md': 'old\n'}, {'scripts/ci_docs_only.py': rule, **page})
        self.assertEqual(answer, 'docs_only=false\n', out)
        self.assertIn('the base holds no scripts/ci_docs_only.py', out)

    def test_harmless_spellings_are_not_findings(self):
        original, others = self.ci_sources(), self.other_sources()
        verify = KERNEL_VERIFY
        edits = (
                (0, 'kernel-only', '    steps:\n',
                 '    # Was --jobs 4, and JOBS: "4", on the standard runner.\n    steps:\n'),
                (0, 'build', '      matrix:\n', '      matrix:\n\n        # standard images only\n'),
                (0, 'build', '      matrix:\n',
                 '      matrix:\n        # Was larger_runner: macos-26-xlarge\n'),
                (0, 'build', "&& '--exclude-label slow' || '' }}\n",
                 "&& '--exclude-label slow' || '' }}  # was --jobs 4\n"),
                (0, 'kernel-only', verify,
                 'run: |\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel \\\n'
                 f'            --jobs {CORES} --ccache {PR_SLOW_FLAG}'),
                (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
                 f'run: |\n          JOBS={CORES} ./scripts/check_corpus_parity.sh --subset'),
                (5, 'build', '    runs-on: ["self-hosted", "pf-gcp-pr"]\n',
                 '    "runs-on": ["self-hosted", "pf-gcp-pr"]\n'),
                (0, 'preflight', '      PF_CI_LABELS:', '      "PF_CI_LABELS":'),
                (0, 'build', '        os: [ubuntu-24.04, macos-26]\n',
                 '        os: [ubuntu-24.04, macos-26]  # the images\n'),
                (0, 'build', '    strategy:\n', '    strategy:  # the legs\n'),
                (0, 'build', '-${{ matrix.build_type }}-${{ github.sha }}',
                 "-${{ matrix.build_type }}-${{ hashFiles('scripts/ci_verify.py') }}-${{ github.sha }}"),
                (0, 'kernel-only', '      - name: Verify (kernel)\n',
                 '      - name: Verify (kernel) with scripts/ci_verify.py\n'),
                (0, 'sanitizers', f'run: python3 scripts/ci_verify.py sanitizers --build-dir build-asan --jobs {CORES}',
                 'run: >-\n          python3 scripts/ci_verify.py sanitizers --build-dir build-asan\n'
                 f'          --jobs {CORES}'),
                (1, 'native-live', f'--jobs {CORES}', f'--jobs={CORES}'),
                (0, 'kernel-only', '      - name: Verify (kernel)\n',
                 '      - name: Verify (kernel), no longer --jobs 4\n'),
                (0, 'kernel-only', KERNEL_VERIFY,
                 'uses: nick-fields/retry@v3\n        with:\n          command: python3 scripts/ci_verify.py '
                 f'kernel --build-dir build-kernel --jobs {CORES} --ccache {PR_SLOW_FLAG}'),
                (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n',
                 f'    runs-on: {RELEASE_RUNNER}  # isolated release\n'),
                (0, None, '\n  preflight:\n', '\n  preflight:  # the fast gate\n'))
        # Each edit alone, then every one that composes (a line one edit
        # rewrites is left to the edit that came first).
        for index, job, before, after in edits:
            with self.subTest(file=CI_SOURCES[index], job=job, after=after):
                changed = original.copy()
                changed[index] = in_job(changed[index], job, before, after)
                self.assertEqual(ci_workflow_findings(*changed, others=others), [])
        changed = original.copy()
        for index, job, before, after in edits:
            if before in (changed[index] if job is None else _jobs(changed[index]).get(job, '')):
                changed[index] = in_job(changed[index], job, before, after)
        self.assertEqual(ci_workflow_findings(*changed, others=others), [])

    def test_a_finding_is_reported_once(self):
        changed = self.ci_sources()
        changed[0] = changed[0].replace('\n  kernel-only:\n', '\n  kernel-only-renamed:\n', 1)
        findings = ci_workflow_findings(*changed, others=self.other_sources())
        self.assertEqual(findings.count('ci.yml is missing job kernel-only'), 1, findings)

    def test_paid_labels_and_larger_runner_keys_are_refused_even_in_release(self):
        original, others = self.ci_sources(), self.other_sources()
        spot = f'    runs-on: {PR_RUNNER}\n'
        lint = ('name: lint\non: workflow_dispatch\njobs:\n  lint:\n' + spot
                + f"    env:\n      PF_CI_LABELS: '{PR_RUNNER}'\n"
                + '    steps:\n      - run: "true"\n')
        mutations = [(f'    runs-on: "{label}"\n', 'must not use larger-runner labels')
                     for label in ('pf-linux-x64-16', 'macos-26-xlarge', 'macos-26-large',
                                   'ubuntu-24.04-xlarge', 'windows-2025-large')]
        mutations += [(f'    strategy:\n      matrix: {{{key}: ubuntu-24.04}}\n' + spot,
                       'must not define a larger_runner key')
                      for key in ('larger_runner', '"larger_runner"', "'larger_runner'")]
        for name in ('lint.yml', 'release.yml'):
            workflow = others[name] if name == 'release.yml' else lint
            job = 'release' if name == 'release.yml' else 'lint'
            before = f'    runs-on: {RELEASE_RUNNER}\n' if name == 'release.yml' else spot
            self.assertEqual(ci_workflow_findings(
                *original, others=dict(others, **{name: workflow})), [])
            for replacement, finding in mutations:
                with self.subTest(name=name, replacement=replacement):
                    changed = in_job(workflow, job, before, replacement.replace(spot, before))
                    findings = ci_workflow_findings(
                        *original, others=dict(others, **{name: changed}))
                    self.assertIn(f'{name} {finding}; use the GCP fleet or hosted exceptions', findings)

    def test_release_jobs_require_isolation_and_reject_pr_triggers(self):
        original, others = self.ci_sources(), self.other_sources()
        release = others['release.yml']
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        for event in ('pull_request', 'pull_request_target', 'push'):
            for key in (event, f'"{event}"', f"'{event}'", event + ' '):
                spelling = f'on:\n  {key}:\n  workflow_dispatch:\n'
                with self.subTest(spelling=spelling):
                    opened = release.replace('on:\n  workflow_dispatch:\n', spelling, 1)
                    findings = ci_workflow_findings(
                        *original, others=dict(others, **{'release.yml': opened}))
                    self.assertIn('release.yml must retain workflow_dispatch only '
                                  'for pf-gcp-main jobs', findings)
        for spelling in ('on: >-\n  workflow_dispatch:\n',
                         'on:\n  workflow_dispatch:\non:\n  workflow_dispatch:\n',
                         'on:\n  push:\n'):
            with self.subTest(spelling=spelling):
                opened = release.replace('on:\n  workflow_dispatch:\n', spelling, 1)
                findings = ci_workflow_findings(*original, others=dict(others, **{'release.yml': opened}))
                self.assertIn('release.yml must retain workflow_dispatch only '
                              'for pf-gcp-main jobs', findings)
        for job in ('release', 'notify-hub'):
            for runner in ('ubuntu-latest', 'ubuntu-24.04', PR_RUNNER,
                           f"${{{{ fromJSON('{RELEASE_RUNNER}') || 'ubuntu-latest' }}}}"):
                with self.subTest(job=job, runner=runner):
                    changed = in_job(release, job, f'    runs-on: {RELEASE_RUNNER}\n',
                                     f'    runs-on: {runner}\n')
                    findings = ci_workflow_findings(
                        *original, others=dict(others, **{'release.yml': changed}))
                    self.assertIn(f'release.yml job {job} must run on {RELEASE_RUNNER} '
                                  '(isolated release)', findings)
        # Restoring the old x64 label must fail, as must moving a hosted leg
        # or changing anything else in the pinned three-leg matrix.
        for before, after in ((f'runner: {RELEASE_RUNNER}\n', 'runner: ubuntu-22.04\n'),
                              (f'runner: {RELEASE_RUNNER}\n', f'runner: {MATRIX_LINUX_RUNNER}\n'),
                              (f'runner: {RELEASE_RUNNER}\n',
                               f"runner: ${{{{ fromJSON('{RELEASE_RUNNER}') || 'ubuntu-22.04' }}}}\n"),
                              ('runner: ubuntu-22.04-arm', 'runner: ubuntu-24.04'),
                              ('runner: ubuntu-22.04-arm', f'runner: {RELEASE_RUNNER}'),
                              ('runner: macos-latest', f'runner: {RELEASE_RUNNER}'),
                              ('runner: macos-latest', 'runner: macos-26-xlarge'),
                              ('          - triple: linux-aarch64\n'
                               '            runner: ubuntu-22.04-arm\n'
                               '            cmake_extra: ""\n', '')):
            with self.subTest(release_runner=after):
                changed = in_job(release, 'prebuilt', before, after)
                findings = ci_workflow_findings(*original, others=dict(others, **{'release.yml': changed}))
                self.assertIn(RELEASE_MATRIX_FINDING, findings)

    def test_baseline_promotion_requires_isolation_and_its_existing_triggers(self):
        original, others = self.ci_sources(), self.other_sources()
        promote = original[2]
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        for runner in ('ubuntu-latest', 'ubuntu-24.04', PR_RUNNER,
                       f"${{{{ fromJSON('{RELEASE_RUNNER}') || 'ubuntu-latest' }}}}"):
            with self.subTest(runner=runner):
                changed = original.copy()
                changed[2] = in_job(promote, 'promote', f'    runs-on: {RELEASE_RUNNER}\n',
                                    f'    runs-on: {runner}\n')
                findings = ci_workflow_findings(*changed, others=others)
                self.assertIn(f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} '
                              '(isolated release)', findings)
        event_finding = ('promote-baseline.yml must retain pull_request_target and '
                         'workflow_dispatch only for pf-gcp-main promotion')
        for event in ('pull_request', 'push'):
            for key in (event, f'"{event}"', f"'{event}'", event + ' '):
                with self.subTest(event_key=key):
                    changed = original.copy()
                    changed[2] = in_job(promote, None, '\non:\n', f'\non:\n  {key}:\n')
                    findings = ci_workflow_findings(*changed, others=others)
                    self.assertIn(event_finding, findings)
        for before, after in (('  pull_request_target:\n', '  pull_request:\n'),
                              ('  workflow_dispatch:\n', '  push:\n'),
                              ('\non:\n', '\non: >-\n'),
                              ('\non:\n', '\non:\non:\n')):
            with self.subTest(trigger_replacement=after):
                changed = original.copy()
                changed[2] = in_job(promote, None, before, after)
                findings = ci_workflow_findings(*changed, others=others)
                self.assertIn(event_finding, findings)

    def test_new_workflows_require_spot_routing_with_hosted_exceptions(self):
        original, others = self.ci_sources(), self.other_sources()
        new = 'name: lint\non: {on}\njobs:\n  lint:\n    runs-on: {runner}\n    steps:\n      - run: "true"\n'
        for on, runner, refused in (
                ('[pull_request]', 'pf-linux-x64-16', True),
                ('\n  pull_request:\n', '\n      group: pineforge-ci-large', True),
                ('[push,\n  pull_request]', 'pf-linux-x64-16', True),
                ('>-\n  pull_request', 'macos-26-xlarge', True),
                ('[pull_request]', 'ubuntu-latest', True),
                ('[pull_request]', 'macos-26', False),
                ('pull_request_target', 'macos-26', False),
                ('pull_request_target', 'ubuntu-22.04-arm', False),
                ('[pull_request]', 'ubuntu-22.04-arm', False),
                ('[pull_request]', 'ubuntu-24.04-arm', False),
                # No dispatch-only exception, whether or not a fork can start it.
                ('\n  workflow_dispatch:\n', 'pf-linux-x64-16', True)):
            with self.subTest(on=on, runner=runner):
                findings = ci_workflow_findings(
                    *original, others=dict(others, **{'lint.yml': new.format(on=on, runner=runner)}))
                self.assertEqual(f'lint.yml job lint must run on {PR_RUNNER}' in findings, refused, findings)
        # A decoy on key inside a multi-line string is no exemption.
        decoy = ('name: "lint\non: push"\non:\n  pull_request:\njobs:\n  lint:\n'
                 '    runs-on: pf-linux-x64-16\n    steps:\n      - run: "true"\n')
        self.assertIn(f'lint.yml job lint must run on {PR_RUNNER}',
                      ci_workflow_findings(*original, others=dict(others, **{'lint.yml': decoy})))
        spot = new.format(on='[pull_request, push, workflow_dispatch]', runner=PR_RUNNER)
        spot = spot.replace('    steps:\n', f"    env:\n      PF_CI_LABELS: '{PR_RUNNER}'\n    steps:\n")
        self.assertEqual(ci_workflow_findings(*original, others=dict(others, **{'lint.yml': spot})), [])
        for on, condition in (('pull_request_target', ''),
                               ('[pull_request]', "    if: github.actor == 'dependabot[bot]'\n"),
                               ('[pull_request]', "    if: ${{ github.actor == 'dependabot[bot]' }}\n")):
            hosted = new.format(on=on, runner='ubuntu-latest')
            hosted = hosted.replace('  lint:\n', '  lint:\n' + condition)
            with self.subTest(hosted_event=on, condition=condition):
                findings = ci_workflow_findings(*original, others=dict(others, **{'lint.yml': hosted}))
                self.assertIn(f'lint.yml job lint must run on {PR_RUNNER}', findings)
                changed = hosted.replace('runs-on: ubuntu-latest', f'runs-on: {PR_RUNNER}', 1)
                changed = changed.replace('    steps:\n',
                                          f"    env:\n      PF_CI_LABELS: '{PR_RUNNER}'\n    steps:\n", 1)
                self.assertEqual(ci_workflow_findings(
                    *original, others=dict(others, **{'lint.yml': changed})), [])

    def test_credentials_require_hosted_runners_at_job_and_workflow_scope(self):
        original, others = self.ci_sources(), self.other_sources()
        base = ('name: deploy\non: workflow_dispatch\njobs:\n  deploy:\n'
                '    runs-on: ubuntu-24.04\n    steps:\n      - run: "true"\n')
        cases = (
            ('job secret', '', "    env:\n      API_KEY: ${{ secrets.DEPLOY_KEY }}\n"),
            ('bracket secret', '', "    env:\n      API_KEY: ${{ secrets['DEPLOY_KEY'] }}\n"),
            ('dynamic secret', '', '    env:\n      API_KEY: ${{ secrets[vars.KEY_NAME] }}\n'),
            ('workflow secret', 'env:\n  API_KEY: ${{ secrets.DEPLOY_KEY }}\n', ''),
            ('job OIDC', '', '    permissions:\n      id-token: write\n'),
            ('job flow OIDC', '', "    permissions: {'id-token': 'write', contents: read}\n"),
            ('workflow OIDC', 'permissions:\n  id-token: write\n', ''),
            ('workflow flow OIDC', 'permissions: {id-token: write}\n', ''),
            ('workflow OIDC despite override', 'permissions:\n  id-token: write\n',
             '    permissions: {id-token: none}\n'),
            ('all permissions', '', '    permissions: write-all\n'),
            ('environment name', '', '    environment: production\n'),
            ('environment mapping', '', '    environment:\n      name: production\n'),
            ('environment flow', '', '    environment: {name: production}\n'),
        )
        for reason, top, job in cases:
            hosted = base.replace('jobs:\n', top + 'jobs:\n', 1)
            hosted = in_job(hosted, 'deploy', '    steps:\n', job + '    steps:\n')
            with self.subTest(reason=reason, hosted=True):
                self.assertEqual(ci_workflow_findings(
                    *original, others=dict(others, **{'deploy.yml': hosted})), [])
            for runner in (PR_RUNNER, RELEASE_RUNNER):
                pool = in_job(hosted, 'deploy', '    runs-on: ubuntu-24.04\n',
                              f'    runs-on: {runner}\n')
                marker = f"      PF_CI_LABELS: '{runner}'\n"
                if '\n    env:\n' in _jobs(pool)['deploy']:
                    pool = in_job(pool, 'deploy', '\n    env:\n', '\n    env:\n' + marker)
                else:
                    pool = in_job(pool, 'deploy', '\n    steps:\n', '\n    env:\n' + marker + '    steps:\n')
                with self.subTest(reason=reason, pool=runner):
                    findings = ci_workflow_findings(*original, others=dict(others, **{'deploy.yml': pool}))
                    self.assertIn('deploy.yml job deploy must keep production credentials '
                                  'on a hosted standard runner', findings)

    def test_github_token_alone_and_credential_comments_keep_the_pool(self):
        original, others = self.ci_sources(), self.other_sources()
        for reference in ('secrets.GITHUB_TOKEN', "secrets['GITHUB_TOKEN']"):
            workflow = ('name: token\non: workflow_dispatch\npermissions:\n  contents: write\n'
                        'jobs:\n  token:\n'
                        f'    runs-on: {PR_RUNNER}\n'
                        '    # secrets.DEPLOY_KEY, id-token: write, environment: production\n'
                        '    env:\n'
                        f"      PF_CI_LABELS: '{PR_RUNNER}'\n"
                        f'      TOKEN: ${{{{ {reference} }}}}\n'
                        '    steps:\n      - run: "true"\n')
            with self.subTest(reference=reference):
                self.assertEqual(ci_workflow_findings(
                    *original, others=dict(others, **{'token.yml': workflow})), [])

    def test_today_release_and_deployment_jobs_pass_only_on_their_pinned_routes(self):
        original, others = self.ci_sources(), self.other_sources()
        workflows = {'docs.yml': original[5], 'promote-baseline.yml': original[2],
                     'release.yml': others['release.yml']}
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        self.assertNotIn('PF_CI_LABELS', workflows['release.yml'])
        for name, jobs in (('docs.yml', ('build',)), ('promote-baseline.yml', ('promote',)),
                           ('release.yml', ('release', 'prebuilt', 'notify-hub'))):
            for job in jobs:
                before = (HOSTED_MATRIX_RUNNER if job == 'prebuilt' else
                          '["self-hosted", "pf-gcp-pr"]' if name == 'docs.yml' else RELEASE_RUNNER)
                after = 'ubuntu-latest' if name == 'docs.yml' else PR_RUNNER
                pool = in_job(workflows[name], job, f'    runs-on: {before}\n',
                              f'    runs-on: {after}\n')
                findings = ci_preflight.runner_findings(dict(
                    {'ci.yml': original[0], **workflows}, **{name: pool}))
                finding = (f'{name} job {job} must run on {PR_RUNNER}' if name == 'docs.yml' else
                           f'{name} job {job} must run on {before} (isolated release)')
                self.assertIn(finding, findings)
                marker = f"      PF_CI_LABELS: '{PR_RUNNER}'\n"
                # Anchor job keys: four spaces alone also match the tail of
                # a step-level env key, which would leave the job env empty.
                if '\n    env:\n' in _jobs(workflows[name])[job]:
                    hosted = in_job(workflows[name], job, '\n    env:\n', '\n    env:\n' + marker)
                else:
                    hosted = in_job(workflows[name], job, '\n    steps:\n',
                                    '\n    env:\n' + marker + '    steps:\n')
                env = ci_preflight._job_block(_jobs(hosted)[job], 'env')
                self.assertEqual(ci_preflight._job_values('\n'.join(env[1:]), 'PF_CI_LABELS'),
                                 [PR_RUNNER], f'{name} job {job} fixture must set job-level PF_CI_LABELS')
                findings = ci_preflight.runner_findings(dict(
                    {'ci.yml': original[0], **workflows}, **{name: hosted}))
                route = 'a gated docs job' if name == 'docs.yml' else 'an isolated release job'
                self.assertIn(f'{name} job {job} must not set PF_CI_LABELS on {route}', findings)

    def test_release_dry_run_never_reaches_the_hub_dispatch(self):
        # Owner, the 1.6.2 dry cut: the hub's receivers are disabled, so a
        # rehearsal that reached notify-hub would dispatch into nothing. The
        # job-level guard is the only thing between a dry run and that call.
        original, others = self.ci_sources(), self.other_sources()
        release = others['release.yml']
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        finding = ('release.yml job notify-hub must carry '
                   'if: ${{ !inputs.dry_run }} so a dry run dispatches nothing')
        self.assertEqual(ci_preflight._jobs(release)['notify-hub'].count(
            '    if: ${{ !inputs.dry_run }}\n'), 1)
        for after in ('', '    if: ${{ always() }}\n',
                      '    if: ${{ inputs.dry_run }}\n',
                      '    if: ${{ !inputs.dry_run || inputs.dry_run }}\n',
                      "    if: '!inputs.dry_run'\n"):
            with self.subTest(after=after):
                changed = in_job(release, 'notify-hub',
                                 '    if: ${{ !inputs.dry_run }}\n', after)
                findings = ci_workflow_findings(
                    *original, others=dict(others, **{'release.yml': changed}))
                self.assertIn(finding, findings)
        # The guard reads a declared input: dropping or retyping it must fail.
        declaration = 'release.yml must declare dry_run as a boolean dispatch input'
        for before, after in (('      dry_run:\n', '      dry_run_disabled:\n'),
                              ('        type: boolean\n', '        type: string\n'),
                              ('        type: boolean\n', '        type: choice\n')):
            with self.subTest(declaration=after):
                changed = in_job(release, None, before, after)
                findings = ci_workflow_findings(
                    *original, others=dict(others, **{'release.yml': changed}))
                self.assertIn(declaration, findings)

    def test_no_stage_timeout_and_jobs_preserve_their_limit_policy(self):
        # Owner rule, 2026-10-12: no stage of this preflight is run under a
        # timeout of its own any more (its verifier-tests stage has taken
        # longer than the old 2400 s bound on slow hosts). Owner #827,
        # 2026-10-20, removed every job timeout declaration; GitHub's own
        # 6 h platform maximum is the only bound for jobs and their steps.
        self.assertFalse(hasattr(ci_preflight, 'STAGE_TIMEOUT_SECONDS'))
        source = (ROOT / 'scripts/ci_preflight.py').read_text()
        stage_call = re.search(r'result = subprocess\.run\(argv, cwd=source[^\n]*\)',
                               source).group()
        self.assertNotIn('timeout', stage_call)
        self.assertNotIn('TimeoutExpired', source)
        workflows = {name: (ROOT / '.github/workflows' / name).read_text()
                     for name in JOB_RUNNERS}
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for workflow, jobs in JOB_RUNNERS.items():
            for job in jobs:
                with self.subTest(workflow=workflow, job=job):
                    body = _jobs(workflows[workflow])[job]
                    self.assertNotRegex(body, r'(?m)^\s*(?:-\s+)?[\'"]?timeout-minutes[\'"]?\s*:')
                # Exercise both job and step keys on every pinned job, including
                # a step whose first key is timeout-minutes and a quoted key.
                for before, after in (
                        ('    steps:\n', '    timeout-minutes: 360\n    steps:\n'),
                        ('    steps:\n', '    timeout-minutes: 30\n    steps:\n'),
                        ('    steps:\n', '    "timeout-minutes":\n    steps:\n'),
                        ('    steps:\n', '    steps:\n      - run: "true"\n        timeout-minutes: 360\n'),
                        ('    steps:\n', '    steps:\n      - run: "true"\n        timeout-minutes: 30\n'),
                        ('    steps:\n', '    steps:\n      - timeout-minutes: 360\n        run: "true"\n'),
                        ('    steps:\n', '    steps:\n      - run: "true"\n        "timeout-minutes":\n')):
                    with self.subTest(workflow=workflow, job=job, declaration=after):
                        changed = dict(workflows)
                        changed[workflow] = in_job(changed[workflow], job, before, after)
                        self.assertEqual(ci_preflight.runner_findings(changed), [
                            f'{workflow} job {job} must declare no timeout-minutes (owner #827)'])

    def test_flat_peak_arms_pin_linux_runners_parallelism_and_no_time_limit(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        for job in ('flat-peak-mem', 'flat-peak-census'):
            for before, after, finding in (
                    (PR_RUNNER, 'ubuntu-24.04', f'job {job} must run on'),
                    (PR_RUNNER, 'pf-linux-x64-16', f'job {job} must run on'),
                    (PR_RUNNER, "${{ github.event_name == 'push' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
                     f'job {job} must run on'),
                    ('    steps:\n', '    timeout-minutes: 360\n    steps:\n',
                     f'job {job} must declare no timeout-minutes (owner #827)'),
                    ('        shell: bash\n', '        shell: bash\n        timeout-minutes: 360\n',
                     f'job {job} must declare no timeout-minutes (owner #827)'),
                    (f'--jobs {CORES}', '--jobs 4', f'job {job} must size its parallelism'),
                    ('        shell: bash\n', '        shell: bash\n        continue-on-error: true\n',
                     f'job {job} must not continue on error')):
                with self.subTest(job=job, after=after):
                    changed = in_job(others['flat-peak.yml'], job, before, after)
                    findings = ci_workflow_findings(*original, others=dict(others, **{'flat-peak.yml': changed}))
                    self.assertTrue(any(finding in line for line in findings), findings)

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
        shutil.copytree(ROOT / '.github/actions', tree / '.github/actions')
        (tree / 'tests').mkdir()
        shutil.copyfile(ROOT / 'tests/CMakeLists.txt', tree / 'tests/CMakeLists.txt')
        if mutation:
            ci = tree / '.github/workflows/ci.yml'
            ci.write_text(in_job(ci.read_text(), *mutation))
        result = subprocess.run(
            [sys.executable, str(tree / 'scripts/ci_preflight.py'), '--check-ci-workflow'],
            cwd=tree, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def test_the_preflight_stage_fails_bad_spot_labels_or_paid_runners(self):
        code, output = self.contract_stage(None)
        self.assertEqual(code, 0, output)
        self.assertIn('runners and time limits OK', output)
        for mutation, finding in (
                (('preflight', PR_RUNNER, 'ubuntu-latest'),
                 f'ci.yml job preflight must run on {PR_RUNNER}'),
                (('preflight', '    steps:\n',
                  '    environment: production\n    steps:\n'),
                 'ci.yml job preflight must keep production credentials on a hosted standard runner'),
                (('preflight', PR_RUNNER,
                  "${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}"),
                 f'ci.yml job preflight must run on {PR_RUNNER}'),
                (('preflight', PR_RUNNER, RELEASE_RUNNER),
                 f'ci.yml job preflight must run on {PR_RUNNER}'),
                (('preflight', PR_RUNNER, '["self-hosted","pf-gcp-pr","pf-gcp-main"]'),
                 f'ci.yml job preflight must run on {PR_RUNNER}'),
                (('sanitizers', PR_RUNNER, '["self-hosted","pf-gcp-small"]'),
                 f'ci.yml job sanitizers must run on {PR_RUNNER}'),
                (('sanitizers', PR_RUNNER, 'pf-linux-x64-16'),
                 f'ci.yml job sanitizers must run on {PR_RUNNER}'),
                (('build', 'os: [ubuntu-24.04, macos-26]', 'os: [ubuntu-24.04, macos-26-xlarge]'),
                 'ci.yml must not use larger-runner labels; use the GCP fleet or hosted exceptions'),
                (('build', '        build_type: [Release, Debug]\n',
                  '        build_type: [Release, Debug]\n        larger_runner: ubuntu-24.04\n'),
                 'ci.yml must not define a larger_runner key; use the GCP fleet or hosted exceptions')):
            with self.subTest(mutation=mutation):
                code, output = self.contract_stage(mutation)
                self.assertEqual(code, 1, output)
                self.assertIn(finding, output.splitlines())

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

    def guard_witness(self) -> list:
        """A stage that prints the pinned skip literal and exits 77."""
        return [sys.executable, '-c',
                'import sys; print("SKIP private fixtures not present: '
                'tests/fixtures/private/witness.csv"); sys.exit(77)']

    def test_a_guard_skip_is_a_skip_only_without_the_overlay(self):
        # R2-F3 (F7): the skip branch. Absent the overlay, exit 77 with the
        # pinned literal is a named skip; with PINEFORGE_PRIVATE_FIXTURES set
        # the same output is a failure, because the input the guard names is in
        # the tree then.
        # Each half states the variable itself: the job's own environment (the
        # overlay jobs export PINEFORGE_PRIVATE_FIXTURES) must not decide which
        # world this control asserts.
        with mock.patch.dict(os.environ) as own_env:
            own_env.pop('PINEFORGE_PRIVATE_FIXTURES', None)
            code, summary, logs = self.run_preflight([('guard', self.guard_witness())])
        self.assertEqual(code, 0)
        self.assertEqual(summary['stages'][0]['status'], 'skipped')
        self.assertEqual(summary['stages'][0]['exitCode'], 77)
        # The stage's log keeps the guard's own output, literal included.
        self.assertIn('SKIP private fixtures not present:',
                      (logs / 'guard.log').read_text())
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            code, summary, _ = self.run_preflight([('guard', self.guard_witness())])
        self.assertEqual(code, 1)
        self.assertEqual(summary['status'], 'failed')
        self.assertEqual(summary['stages'][0]['status'], 'failed')
        self.assertEqual(summary['stages'][0]['exitCode'], 77)

    def test_a_bare_77_is_a_failure_in_both_worlds(self):
        # The literal is required: 77 alone is never a skip.
        bare = [sys.executable, '-c', 'import sys; sys.exit(77)']
        for flag in ('', '1'):
            with self.subTest(overlay=flag), \
                    mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': flag}):
                code, summary, _ = self.run_preflight([('guard', bare)])
                self.assertEqual(code, 1)
                self.assertEqual(summary['stages'][0]['status'], 'failed')

    def test_the_skip_literal_has_one_home(self):
        # R2-F7: the branch reads ci_verify's literal, not a second copy.
        self.assertIs(ci_preflight.PRIVATE_FIXTURE_SKIP_LITERAL,
                      ci_verify.PRIVATE_FIXTURE_SKIP_LITERAL)
        source = (ROOT / 'scripts' / 'ci_preflight.py').read_text()
        self.assertEqual(source.count('SKIP private fixtures not present: '), 0)

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
                     'kernel-seam-rows', 'kernel-seam-rows-tests',
                     'run-json-harness-tests'):
            self.assertIn(name, plan)
            self.assertEqual(len(plan[name]), 2, name)

    def test_the_harness_tests_run_every_file_under_pytest(self):
        # docker/run_json_*_test.py need pytest: every one, found when the plan
        # is built, with nothing written into the checkout.
        argv = {entry[0]: entry[1] for entry in check_commands(ROOT)}['run-json-harness-tests']
        self.assertEqual(argv[:9], [sys.executable, '-B', '-m', 'pytest', '-q', '-p',
                                    'no:cacheprovider', '--rootdir', str(ROOT)])
        tests = sorted(str(path) for path in (ROOT / 'docker').glob('run_json_*_test.py'))
        self.assertGreaterEqual(len(tests), 4)
        self.assertEqual(argv[9:], tests)
        # A tree without them names the pattern, which pytest refuses: the stage
        # never falls back to collecting the whole tree.
        argv = {entry[0]: entry[1]
                for entry in check_commands(Path('/nonexistent-pineforge-src'))}[
                    'run-json-harness-tests']
        self.assertEqual(argv[9:], ['/nonexistent-pineforge-src/docker/run_json_*_test.py'])

    def test_no_stage_is_advisory_today(self):
        for entry in check_commands(Path('/src')):
            self.assertFalse(len(entry) > 2 and entry[2], entry[0])

    def test_success_is_explicitly_only_preflight(self):
        code, summary, _ = self.run_preflight([('ok', [sys.executable, '-c', 'pass'])])
        self.assertEqual(code, 0)
        self.assertEqual(summary['status'], 'passed')
        self.assertIn('full verification still required', summary['scope'])

    def test_summary_and_stages_record_started_at_and_seconds_while_preserving_existing_keys(self):
        iso_utc_second = re.compile(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$')
        observer_cmd = [
            sys.executable, '-c',
            'import json, pathlib; '
            'p = pathlib.Path("logs/preflight-summary.json"); '
            'pathlib.Path("logs/mid-run.json").write_text(p.read_text())'
        ]
        code, summary, logs = self.run_preflight([
            ('first', [sys.executable, '-c', 'pass']),
            ('second', observer_cmd),
        ])
        self.assertEqual(code, 0)
        mid = json.loads((logs / 'mid-run.json').read_text())

        # Mid-run summary: top-level startedAt is present, top-level seconds is not yet written.
        self.assertEqual(set(mid.keys()),
                         {'schemaVersion', 'status', 'scope', 'stages', 'startedAt'})
        self.assertEqual(mid['schemaVersion'], 'pineforge-ci-preflight/v1')
        self.assertEqual(mid['status'], 'incomplete')
        self.assertEqual(mid['scope'],
                         'workflow and source checks only; full verification still required')
        self.assertIsInstance(mid['startedAt'], str)
        self.assertRegex(mid['startedAt'], iso_utc_second)
        self.assertEqual(len(mid['stages']), 2)

        # While the second stage is running, the first stage already has its finished seconds
        # and the second stage has startedAt set and seconds null.
        finished_first = mid['stages'][0]
        self.assertIsInstance(finished_first['startedAt'], str)
        self.assertRegex(finished_first['startedAt'], iso_utc_second)
        self.assertIsInstance(finished_first['seconds'], float)
        self.assertNotIsInstance(finished_first['seconds'], bool)
        self.assertGreaterEqual(finished_first['seconds'], 0.0)
        self.assertEqual(round(finished_first['seconds'], 3), finished_first['seconds'])

        running = mid['stages'][1]
        self.assertEqual(set(running.keys()),
                         {'name', 'argv', 'status', 'exitCode', 'advisory', 'log',
                          'startedAt', 'seconds'})
        self.assertEqual(running['name'], 'second')
        self.assertEqual(running['argv'], observer_cmd)
        self.assertEqual(running['status'], 'running')
        self.assertIsNone(running['exitCode'])
        self.assertIs(running['advisory'], False)
        self.assertEqual(running['log'], 'second.log')
        self.assertIsInstance(running['startedAt'], str)
        self.assertRegex(running['startedAt'], iso_utc_second)
        self.assertIsNone(running['seconds'])

        # Finished summary: top-level startedAt and seconds are present alongside existing keys.
        self.assertEqual(set(summary.keys()),
                         {'schemaVersion', 'status', 'scope', 'stages', 'exitCode',
                          'startedAt', 'seconds'})
        self.assertEqual(summary['schemaVersion'], 'pineforge-ci-preflight/v1')
        self.assertEqual(summary['status'], 'passed')
        self.assertEqual(summary['scope'],
                         'workflow and source checks only; full verification still required')
        self.assertEqual(summary['exitCode'], 0)
        self.assertEqual(summary['startedAt'], mid['startedAt'])
        self.assertIsInstance(summary['seconds'], float)
        self.assertNotIsInstance(summary['seconds'], bool)
        self.assertGreaterEqual(summary['seconds'], 0.0)
        self.assertEqual(round(summary['seconds'], 3), summary['seconds'])

        for index, (expected_name, expected_argv) in enumerate((
                ('first', [sys.executable, '-c', 'pass']),
                ('second', observer_cmd))):
            stage = summary['stages'][index]
            self.assertEqual(set(stage.keys()),
                             {'name', 'argv', 'status', 'exitCode', 'advisory', 'log',
                              'startedAt', 'seconds'})
            self.assertEqual(stage['name'], expected_name)
            self.assertEqual(stage['argv'], expected_argv)
            self.assertEqual(stage['status'], 'passed')
            self.assertEqual(stage['exitCode'], 0)
            self.assertIs(stage['advisory'], False)
            self.assertEqual(stage['log'], f'{expected_name}.log')
            self.assertIsInstance(stage['startedAt'], str)
            self.assertRegex(stage['startedAt'], iso_utc_second)
            self.assertIsInstance(stage['seconds'], float)
            self.assertNotIsInstance(stage['seconds'], bool)
            self.assertGreaterEqual(stage['seconds'], 0.0)
            self.assertEqual(round(stage['seconds'], 3), stage['seconds'])
        self.assertEqual(summary['stages'][0]['startedAt'], finished_first['startedAt'])
        self.assertEqual(summary['stages'][0]['seconds'], finished_first['seconds'])
        self.assertEqual(summary['stages'][1]['startedAt'], running['startedAt'])


class CanonAuthz(unittest.TestCase):
    def test_real_workflow_scripts_admit_our_events_and_refuse_untrusted_identity(self):
        scripts = [job_script((ROOT / path).read_text(), 'authz') for path in CI_SOURCES[:2]]
        self.assertEqual(scripts[0], scripts[1])
        base = {'EVENT': 'pull_request', 'REPO': 'owner/engine', 'HEAD_REPO': 'owner/engine',
                'HEAD_IS_FORK': 'false', 'ASSOCIATION': 'MEMBER', 'PR_AUTHOR': 'member',
                'WR_HEAD_REPO': '', 'WR_ACTOR': '', 'PERMISSION': 'none', 'LOOKUP_EXIT': '0',
                'ALLOWED_BOTS': 'dependabot[bot] pineforge-app[bot]'}
        cases = [
            ({}, True, False),
            ({'PR_AUTHOR': 'dependabot[bot]', 'ASSOCIATION': 'NONE'}, True, False),
            ({'PR_AUTHOR': 'pineforge-app[bot]', 'ASSOCIATION': 'NONE'}, True, False),
            ({'HEAD_REPO': 'outside/engine'}, False, False),
            ({'HEAD_IS_FORK': 'true'}, False, False),
            ({'ASSOCIATION': 'NONE', 'PR_AUTHOR': 'outsider'}, False, True),
            ({'ASSOCIATION': 'NONE', 'PR_AUTHOR': 'other[bot]'}, False, True),
            ({'EVENT': 'pull_request_target'}, False, False),
            ({'EVENT': 'push'}, True, False),
        ]
        for permission in ('admin', 'maintain', 'write', 'read', 'triage', ''):
            cases.append(({'ASSOCIATION': 'CONTRIBUTOR', 'PERMISSION': permission},
                          permission in ('admin', 'maintain', 'write'), True))
        cases.append(({'ASSOCIATION': 'CONTRIBUTOR', 'LOOKUP_EXIT': '1'}, False, True))
        workflow_run = {'EVENT': 'workflow_run', 'WR_HEAD_REPO': 'owner/engine', 'WR_ACTOR': 'rerunner'}
        for change, allowed, lookup in (
                ({'WR_HEAD_REPO': 'outside/engine'}, False, False),
                ({'WR_HEAD_REPO': ''}, False, False),
                ({'WR_ACTOR': ''}, False, False),
                ({'WR_ACTOR': 'pineforge-app[bot]'}, True, False),
                ({'WR_ACTOR': 'pineforge-ci-dispatcher[bot]'}, True, False),
                ({'WR_ACTOR': 'dependabot[bot]', 'PERMISSION': 'write'}, False, False),
                ({'WR_ACTOR': 'github-actions[bot]', 'PERMISSION': 'write'}, False, False),
                ({'PERMISSION': 'admin'}, True, True),
                ({'PERMISSION': 'maintain'}, True, True),
                ({'PERMISSION': 'write'}, True, True),
                ({'PERMISSION': 'read'}, False, True),
                ({'PERMISSION': ''}, False, True),
                ({'PERMISSION': 'write', 'LOOKUP_EXIT': '1'}, False, True)):
            cases.append(({**workflow_run, **change}, allowed, lookup))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gh = root / 'gh'
            gh.write_text('#!' + sys.executable + '\n'
                'import os, pathlib, sys\n'
                'actor = os.environ["WR_ACTOR"] if os.environ["EVENT"] == "workflow_run" else os.environ["PR_AUTHOR"]\n'
                'expected = ["api", "repos/" + os.environ["REPO"] + "/collaborators/" + actor + "/permission", "--jq", ".permission"]\n'
                'if sys.argv[1:] != expected: raise SystemExit(2)\n'
                'with pathlib.Path(os.environ["LOOKUP_LOG"]).open("a") as log: log.write(actor + "\\n")\n'
                'print(os.environ["PERMISSION"])\n'
                'raise SystemExit(int(os.environ["LOOKUP_EXIT"]))\n')
            gh.chmod(0o755)
            for index, script in enumerate(scripts):
                for case, (change, allowed, lookup) in enumerate(cases):
                    with self.subTest(workflow=CI_SOURCES[index], identity=change):
                        log = root / f'lookup-{index}-{case}'
                        env = {**os.environ, **base, **change, 'LOOKUP_LOG': str(log),
                               'GITHUB_STEP_SUMMARY': str(root / f'summary-{index}-{case}'),
                               'PATH': str(root) + os.pathsep + os.environ.get('PATH', '')}
                        ran = subprocess.run(['bash', '-eu', '-c', script], env=env,
                                             text=True, capture_output=True)
                        self.assertEqual(ran.returncode == 0, allowed, ran.stdout + ran.stderr)
                        self.assertEqual(log.exists(), lookup, ran.stdout + ran.stderr)
                # Disabling fail must flip a refused fork verdict, proving that
                # the cases exercise the shell rather than just a source digest.
                mutant = script.replace('exit 1; }', ':; }', 1)
                self.assertNotEqual(mutant, script)
                env = {**os.environ, **base, 'HEAD_IS_FORK': 'true',
                       'GITHUB_STEP_SUMMARY': str(root / f'mutant-{index}')}
                before = subprocess.run(['bash', '-eu', '-c', script], env=env, capture_output=True)
                after = subprocess.run(['bash', '-eu', '-c', mutant], env=env, capture_output=True)
                self.assertNotEqual(before.returncode == 0, after.returncode == 0)


class ActionlintRunnerLabels(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tree = Path(directory.name)
        shutil.copytree(ROOT / 'scripts', self.tree / 'scripts',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        shutil.copytree(ROOT / '.github/workflows', self.tree / '.github/workflows')
        self.config = self.tree / '.github/actionlint.yaml'
        shutil.copyfile(ROOT / '.github/actionlint.yaml', self.config)

    def runner_labels(self, tree=None):
        tree = self.tree if tree is None else tree
        # Exercise the actual preflight stage, including config reads and
        # workflow discovery, without requiring actionlint in CTest hosts.
        argv = {entry[0]: entry[1] for entry in check_commands(tree)}['actionlint-runner-labels']
        result = subprocess.run(argv, cwd=tree, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def test_actionlint_runner_labels_accepts_real_tree(self):
        code, output = self.runner_labels(ROOT)
        self.assertEqual(code, 0, output)
        self.assertIn('all literal self-hosted labels declared', output)

    def test_actionlint_runner_labels_accepts_both_gcp_runner_labels(self):
        workflow = self.tree / '.github/workflows/gcp-labels.yml'
        workflow.write_text(
            'on: workflow_dispatch\njobs:\n  pr:\n'
            '    runs-on: ["self-hosted", "pf-gcp-pr"]\n'
            '    steps:\n      - run: "true"\n'
            '  main:\n'
            '    runs-on: ["self-hosted", "pf-gcp-main"]\n'
            '    steps:\n      - run: "true"\n')
        code, output = self.runner_labels()
        self.assertEqual(code, 0, output)

    def test_actionlint_runner_labels_requires_config(self):
        self.config.unlink()
        code, output = self.runner_labels()
        self.assertNotEqual(code, 0, output)
        self.assertIn('.github/actionlint.yaml: cannot read config:', output)
        self.assertIn('No such file or directory', output)

    def test_actionlint_runner_labels_rejects_removed_label(self):
        original = self.config.read_text()
        for label, sites in (
                ('pf-gcp-pr', (('ci.yml', 'authz'), ('native-live.yml', 'authz'),
                               ('corpus-parity.yml', 'authz'), ('corpus-parity.yml', 'corpus-parity'),
                               ('docs.yml', 'authz'), ('docs.yml', 'build'))),
                ('pf-gcp-main', (('release.yml', 'release'), ('promote-baseline.yml', 'promote')))):
            with self.subTest(label=label):
                changed = original.replace(f'    - {label}\n', '', 1)
                self.assertNotEqual(changed, original)
                self.config.write_text(changed)
                code, output = self.runner_labels()
                self.assertNotEqual(code, 0, output)
                for name, job in sites:
                    self.assertIn(f'{name} job {job}: label "{label}" is not declared '
                                  'in .github/actionlint.yaml', output.splitlines())
        self.config.write_text(original)

    def test_actionlint_runner_labels_rejects_new_literal_label(self):
        for suffix in ('.yml', '.yaml'):
            with self.subTest(suffix=suffix):
                workflow = self.tree / f'.github/workflows/undeclared{suffix}'
                workflow.write_text(
                    'on: workflow_dispatch\njobs:\n  undeclared:\n'
                    '    runs-on: ["self-hosted", "pf-gcp-undeclared"]\n'
                    '    steps:\n      - run: "true"\n')
                code, output = self.runner_labels()
                self.assertNotEqual(code, 0, output)
                self.assertIn(workflow.name + ' job undeclared: label "pf-gcp-undeclared" is not declared '
                              'in .github/actionlint.yaml', output.splitlines())
                workflow.unlink()

    def test_actionlint_runner_labels_rejects_unparseable_config(self):
        for config in ('self-hosted-runner: [\n',
                       'self-hosted-runner:\n  labels: pf-gcp-pr\n'):
            with self.subTest(config=config):
                self.config.write_text(config)
                code, output = self.runner_labels()
                self.assertNotEqual(code, 0, output)
                self.assertIn('.github/actionlint.yaml: cannot parse config: expected '
                              'self-hosted-runner.labels as a block list of literal labels',
                              output.splitlines())


class PinnedLiveCurl(unittest.TestCase):
    def test_all_live_profiles_use_the_shared_pinned_dependency(self):
        kernel = _jobs((ROOT / CI_SOURCES[0]).read_text())['kernel-only']
        native = _jobs((ROOT / CI_SOURCES[1]).read_text())['native-live']
        for job in (kernel, native):
            self.assertIn('uses: ./.github/actions/setup-live-curl', job)
            self.assertNotIn('libcurl4-openssl-dev', job)
            for line in job.splitlines():
                if 'run: python3 scripts/ci_verify.py ' in line:
                    self.assertIn('--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}"', line)
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        self.assertIn('bash scripts/build_live_curl.sh --metadata', action)
        self.assertIn('uses: actions/cache/restore@v4', action)
        self.assertIn('uses: actions/cache/save@v4', action)
        self.assertIn('bash scripts/build_live_curl.sh build-native-deps 4', action)

    def test_shared_curl_pin_and_websocket_build_flags(self):
        script = ROOT / 'scripts/build_live_curl.sh'
        result = subprocess.run(['bash', str(script), '--metadata'],
                                check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout, 'version=8.14.1\nsha256='
                         'f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd\n')
        build = script.read_text()
        self.assertIn('sha256sum -c -', build)
        self.assertIn('-DENABLE_WEBSOCKETS=ON', build)
        self.assertIn('-DBUILD_STATIC_LIBS=ON', build)
        self.assertIn('-DCURL_USE_OPENSSL=ON', build)


class IndependentNativeProfiles(unittest.TestCase):
    def test_native_contract_rejects_lost_profiles_budgets_caches_and_diagnostics(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        self.assertEqual(native_workflow_findings(native, action), [])
        mutations = [
            ('fail-fast: false', 'fail-fast: true'),
            ('    name: native-live (${{ matrix.profile }})',
             '    name: native-live (${{ matrix.profile }})\n    needs: prior'),
            ('needs: [authz, native-live]', 'needs: [authz]'),
            ('test "$PROFILES_RESULT" = success', 'true'),
            ('PROFILES_RESULT: ${{ needs.native-live.result }}', 'PROFILES_RESULT: success'),
            ('contents: read', 'contents: write'),
            ('persist-credentials: false', 'persist-credentials: true'),
            ('fetch-depth: 0', 'fetch-depth: 1'),
            ('PINEFORGE_REQUIRE_RELEASE_TAGS: "1"', 'PINEFORGE_REQUIRE_RELEASE_TAGS: "0"'),
            ('if: ${{ !inputs.cold_cache }}', 'if: always()'),
            ('cold-cache: ${{ inputs.cold_cache }}', "cold-cache: 'false'"),
            ('cache-namespace: ${{ matrix.profile }}', 'cache-namespace: shared'),
            ('live-v2-${{ matrix.profile }}-', 'live-v2-shared-'),
            ('ccache --zero-stats', 'ccache --show-stats'),
            (NATIVE_CACHE_SAVE, "${{ success() }}"),
            ('uses: actions/cache/save@v4', 'uses: actions/cache@v4'),
            ('name: ci-diagnostics-native-live-${{ matrix.profile }}',
             'name: ci-diagnostics-native-live'),
            ('--ccache ', '--ccache --test-timeout 900 '),
            ('--ccache ', '--ccache --ctest-timeout 1200 '),
            (NATIVE_EXCLUSION, NATIVE_EXCLUSION + ' --min-tests 1'),
            (NATIVE_EXCLUSION, NATIVE_EXCLUSION + ' || true'),
            ('--require-websocket', '--min-tests 1'),
            (NATIVE_EXCLUSION, "${{ inputs.exclude_slow && '--exclude-label slow' || '' }}"),
            ('id: verify\n', 'id: verify\n        if: success()\n'),
            ('id: verify\n', 'id: verify\n        timeout-minutes: 360\n'),
            ('      - name: Stage and summarize diagnostics\n        if: always()',
             '      - name: Stage and summarize diagnostics\n        if: success()'),
            ('      - name: Retain CI diagnostics\n        if: always()',
             '      - name: Retain CI diagnostics\n        if: success()'),
        ]
        for profile in ('native', 'live-sanitizers', 'live-tsan'):
            block = re.search(r'          - profile: ' + re.escape(profile)
                              + r'\n(?:            .*\n)+', native).group()
            mutations.append((block, ''))
            # A per-profile budget is not just refused when changed: with the
            # owner rule of 2026-10-12 (no time limit may fail a run for
            # slowness) its mere return must fail the contract, whatever its
            # value.
            for added in ('            job_minutes: 75\n', '            verify_minutes: 65\n',
                          '            test_timeout: 2700\n', '            ctest_timeout: 2820\n'):
                mutations.append((block, block + added))
        for before, after in mutations:
            with self.subTest(mutation=(before, after)):
                self.assertIn(before, native)
                self.assertTrue(native_workflow_findings(native.replace(before, after, 1), action))
        # Both entry points must keep the cold proof explicit and opt-in.
        for event in ('workflow_call', 'workflow_dispatch'):
            at = native.index('  ' + event + ':')
            cold = native.index('      cold_cache:', at)
            changed = native[:cold] + native[cold:].replace('default: false', 'default: true', 1)
            self.assertTrue(native_workflow_findings(changed, action))

    def test_curl_contract_rejects_post_only_save_and_unguarded_cold_restore(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        for before, after in (
                ("if: ${{ inputs.cold-cache != 'true' }}", 'if: always()'),
                (CURL_CACHE_SAVE, '${{ success() }}'),
                ('uses: actions/cache/restore@v4', 'uses: actions/cache@v4'),
                ('uses: actions/cache/save@v4', 'uses: actions/cache@v4'),
                ('curl-${{ inputs.cache-namespace }}-', 'curl-shared-'),
                ('${{ steps.dependencies.outputs.sha256 }}', 'unpinned'),
                ('${{ steps.dependencies.outputs.identity }}', 'shared-toolchain'),
                ('bash scripts/build_live_curl.sh build-native-deps 4', 'true')):
            with self.subTest(mutation=(before, after)):
                self.assertIn(before, action)
                self.assertTrue(native_workflow_findings(native, action.replace(before, after, 1)))

    def test_native_aggregate_rejects_failure_cancellation_and_skip(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        script = job_script(native, 'native-live-gate', 'Require every native profile to have passed')
        for authz, result in ((authz, profile)
                              for authz in ('success', 'failure', 'cancelled', 'skipped', '')
                              for profile in ('success', 'failure', 'cancelled', 'skipped', '')):
            with self.subTest(authz=authz, result=result):
                ran = subprocess.run(['bash', '-e', '-c', script], capture_output=True,
                                     env={**os.environ, 'AUTHZ_RESULT': authz, 'PROFILES_RESULT': result})
                self.assertEqual(ran.returncode == 0, authz == result == 'success', ran.stdout)
                if authz != 'success' or result != 'success':
                    self.assertEqual(ran.returncode, 1, ran.stdout)
                # A cancelled profile job is how the platform stops a hang at the job maximum: the
                # aggregate still fails, and the log names it as a HANG to re-run (EN-116).
                self.assertEqual(b'HANG (re-run)' in ran.stdout, result == 'cancelled', ran.stdout)

    def test_three_profiles_share_the_platform_maximum_and_no_fail_fast(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        job = _jobs(native)['native-live']
        self.assertIn('fail-fast: false', job)
        for profile in ('native', 'live-sanitizers', 'live-tsan'):
            self.assertIn(f'- profile: {profile}\n', job)
        # Owner #827, 2026-10-20: no declared job or step deadline;
        # GitHub's own 6 h platform maximum is the only bound.
        self.assertNotRegex(job, r'(?m)^    timeout-minutes:')
        self.assertNotRegex(job, r'(?m)^        timeout-minutes:')
        for budget in ('matrix.job_minutes', 'matrix.verify_minutes',
                       '--test-timeout', '--ctest-timeout'):
            self.assertNotIn(budget, job)
        self.assertIn('needs: [authz, native-live]', _jobs(native)['native-live-gate'])

    def test_cold_mode_bypasses_both_restores_and_dependencies_save_early(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        self.assertIn('if: ${{ !inputs.cold_cache }}', native)
        self.assertIn('cold-cache: ${{ inputs.cold_cache }}', native)
        self.assertIn("if: ${{ inputs.cold-cache != 'true' }}", action)
        self.assertIn('uses: actions/cache/restore@v4', action)
        self.assertIn('uses: actions/cache/save@v4', action)
        self.assertNotIn('uses: actions/cache@v4', action)


if __name__ == '__main__':
    unittest.main()
