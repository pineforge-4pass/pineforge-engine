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
from ci_preflight import (CORES, DOCS_ONLY_SKIP, JOB_RUNNERS, LINUX_RUNNER, MATRIX_RUNNER, NATIVE_EXCLUSION, NATIVE_CACHE_SAVE, CURL_CACHE_SAVE,
                          PROOF_JOBS, _jobs, check_commands,
                          ci_workflow_findings, native_workflow_findings, run_checks)

ROOT = Path(__file__).resolve().parents[1]
KERNEL_VERIFY = ('run: python3 scripts/ci_verify.py kernel --build-dir build-kernel '
                 '--jobs "$(getconf _NPROCESSORS_ONLN)" --ccache '
                 '--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}"')
# ci_workflow_findings' arguments, in order.
CI_SOURCES = ('.github/workflows/ci.yml', '.github/workflows/native-live.yml',
              '.github/workflows/promote-baseline.yml', 'tests/CMakeLists.txt',
              '.github/workflows/corpus-parity.yml', '.github/workflows/docs.yml')


def job_script(ci, job):
    """A job's run block, exactly as the runner receives it."""
    lines = _jobs(ci)[job].split('\n')
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
            (0, 'passed "$SANITIZER_RESULT"', 'true'),
            (0, '[[ "$PREFLIGHT_RESULT" == "success" ]] && passed "$BUILD_RESULT"',
                'passed "$PREFLIGHT_RESULT" && passed "$BUILD_RESULT"'),
            (0, 'needs: [changes, preflight, build,', 'needs: [preflight, build,'),
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
            (0, "matrix.build_type == 'Debug' && '--exclude-label slow'", "'--exclude-label slow'"),
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

    def test_ci_contract_pins_standard_runners_time_limits_and_parallelism(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(LINUX_RUNNER, 'ubuntu-24.04')
        self.assertEqual(MATRIX_RUNNER, '${{ matrix.os }}')
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        mutations = (
            # Every heavy job stays on standard runners, whatever starts it.
            (0, 'preflight', LINUX_RUNNER, 'pf-linux-x64-16', 'ci.yml job preflight must run on'),
            (0, 'sanitizers', LINUX_RUNNER, 'macos-26-xlarge', 'ci.yml job sanitizers must run on'),
            (0, 'kernel-only', LINUX_RUNNER, 'ubuntu-latest', 'ci.yml job kernel-only must run on'),
            # Target runs-on exactly: matrix.os also occurs earlier in the leg name.
            (0, 'build', f'    runs-on: {MATRIX_RUNNER}\n',
             '    runs-on: macos-26-large\n', 'ci.yml job build must run on'),
            (0, 'build', f'    runs-on: {MATRIX_RUNNER}\n',
             '    runs-on: ${{ matrix.larger_runner }}\n', 'ci.yml job build must run on'),
            (1, 'native-live', LINUX_RUNNER, 'pf-linux-x64-16', 'native-live.yml job native-live must run on'),
            (4, 'corpus-parity', LINUX_RUNNER, 'pf-linux-x64-16', 'corpus-parity.yml job corpus-parity must run on'),
            (4, 'corpus-parity-subset', LINUX_RUNNER, 'pf-linux-x64-16',
             'corpus-parity.yml job corpus-parity-subset must run on'),
            # An event-based paid-runner route is forbidden, even for trusted events.
            (0, 'preflight', LINUX_RUNNER,
             "${{ github.event_name == 'push' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             'ci.yml job preflight must run on'),
            (1, 'native-live', LINUX_RUNNER,
             "${{ github.event_name == 'workflow_dispatch' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             'native-live.yml job native-live must run on'),
            (4, 'corpus-parity', LINUX_RUNNER,
             "${{ github.event_name == 'schedule' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             'corpus-parity.yml job corpus-parity must run on'),
            (0, 'sanitizers', LINUX_RUNNER,
             "${{ github.event.pull_request.head.repo.full_name == github.repository "
             "&& 'pf-linux-x64-16' || 'ubuntu-24.04' }}",
             'ci.yml job sanitizers must run on'),
            (0, 'native-live', 'uses: ./.github/workflows/native-live.yml',
             'uses: someone/else/.github/workflows/native-live.yml@main',
             'ci.yml job native-live calls a workflow the runner contract cannot see'),
            # No paid image or include override, even one using a standard image.
            (0, 'build', 'os: [ubuntu-24.04, macos-26]', 'os: [ubuntu-24.04, macos-26-large]',
             'ci.yml build must use exactly the standard images'),
            (0, 'build', '        build_type: [Release, Debug]\n',
             '        build_type: [Release, Debug]\n        include:\n'
             '          - os: ubuntu-24.04\n            build_type: Debug\n',
             'ci.yml build must use exactly the standard images'),
            (0, 'build', '        build_type: [Release, Debug]\n',
             '        build_type: [Release, Debug]\n        include:\n'
             '          - os: ubuntu-24.04\n            larger_runner: ubuntu-24.04\n',
             'ci.yml must not define a larger_runner key'),
            # Block forms of runs-on, and job ids the parser must still see.
            (5, 'build', '    runs-on: ubuntu-latest\n', '    runs-on:\n      group: pineforge-ci-large\n',
             'docs.yml job build must stay on a standard runner'),
            (2, 'promote', '    runs-on: ubuntu-latest\n', '    runs-on:\n      - macos-26-xlarge\n',
             'promote-baseline.yml job promote must stay on a standard runner'),
            (0, None, '  build-gate:\n', '  extra:\n    runs-on:\n      group: pineforge-ci-large\n'
             '    steps:\n      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner'),
            (0, None, '  build-gate:\n', '  extra:\n    "runs-on": pf-linux-x64-16\n'
             '    steps:\n      - run: "true"\n\n  build-gate:\n', 'ci.yml job extra needs a pinned runner'),
            (5, 'build', '    runs-on: ubuntu-latest\n', '    "runs-on": pf-linux-x64-16\n',
             'docs.yml job build must stay on a standard runner'),
            (5, 'build', '    runs-on: ubuntu-latest\n', '    runs-on : macos-26-xlarge\n',
             'docs.yml job build must stay on a standard runner'),
            (2, 'promote', '    runs-on: ubuntu-latest\n', "    'runs-on': macos-26-xlarge\n",
             'promote-baseline.yml job promote must stay on a standard runner'),
            (5, 'build', '    runs-on: ubuntu-latest\n', '',
             'docs.yml job build has a runs-on the runner contract cannot read'),
            (5, None, '\njobs:\n', '\njobs:\n  pre_build:\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml job pre_build must stay on a standard runner'),
            (5, None, '\njobs:\n', '\njobs:\n  "pre-build":\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml has jobs lines the runner contract cannot read'),
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
            # Any time limit other than the platform maximum: every job must
            # keep GitHub's own 360-minute cap as its only bound, so no bound
            # of this repository's can fail a run for slowness (owner rule,
            # 2026-10-12).
            (0, 'preflight', 'timeout-minutes: 360', 'timeout-minutes: 30',
             'ci.yml job preflight must allow 360 minutes'),
            (0, 'build', 'timeout-minutes: 360', 'timeout-minutes: 45',
             'ci.yml job build must allow 360'),
            (0, 'sanitizers', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'ci.yml job sanitizers must allow 360'),
            (0, 'kernel-only', 'timeout-minutes: 360', 'timeout-minutes: 45',
             'ci.yml job kernel-only must allow 360'),
            (0, 'build-gate', '    timeout-minutes: 360\n', '', 'ci.yml job build-gate must allow 360'),
            (0, 'changes', 'runs-on: ubuntu-24.04', 'runs-on: pf-linux-x64-16',
             'ci.yml job changes must run on ubuntu-24.04'),
            (0, 'changes', '    timeout-minutes: 360\n', '    timeout-minutes: 300\n',
             'ci.yml job changes must allow 360'),
            (1, 'native-live', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'native-live.yml job native-live must allow 360 minutes'),
            (4, 'corpus-parity', 'timeout-minutes: 360', 'timeout-minutes: 30',
             'corpus-parity.yml job corpus-parity must allow 360'),
            (4, 'corpus-parity-subset', 'timeout-minutes: 360', 'timeout-minutes: 10',
             'corpus-parity.yml job corpus-parity-subset must allow 360'),
            # A fixed count cannot follow the runner's available cores.
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
            (0, 'kernel-only', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --job 4',
             'ci.yml job kernel-only must size'),
            (0, 'sanitizers', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --jo=4',
             'ci.yml job sanitizers must size'),
            (0, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
             'working-directory: scripts\n        run: python3 ci_verify.py kernel --build-dir ../build-kernel --ccache',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
             'run: $VERIFY', 'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n'
             '      - run: PYTHONPATH=scripts python3 -m ci_verify kernel --build-dir build-kernel --ccache\n',
             'ci.yml job kernel-only must size'),
            # A second, unsized call however it is chained, blocked or carried.
            (0, 'kernel-only', KERNEL_VERIFY,
             f'run: >-\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache'
             '\n\n          python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - uses: nick-fields/retry@v3\n        with:\n'
             '          command: python3 scripts/ci_verify.py release --build-dir build-rel --ccache\n',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY,
             KERNEL_VERIFY + ' && python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY,
             KERNEL_VERIFY + '; python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - run: python3 -X dev scripts/ci_verify.py release --build-dir build-rel --ccache\n',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - run: $pythonLocation/bin/python3 scripts/ci_verify.py release '
             '--build-dir build-rel --ccache\n',
             'ci.yml job kernel-only must size'),
            (0, 'kernel-only', KERNEL_VERIFY, KERNEL_VERIFY + " ${{ '--jobs 4' }}",
             'ci.yml job kernel-only must size'),
            (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
             f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset'
             ' && JOBS=4 ./scripts/check_corpus_parity.sh --subset',
             'corpus-parity.yml job corpus-parity-subset must size'),
            (0, 'kernel-only', f'      - name: Verify (kernel)\n        run: python3 scripts/ci_verify.py kernel '
             f'--build-dir build-kernel --jobs {CORES} --ccache',
             f'      - name: python3 scripts/ci_verify.py kernel --jobs {CORES}\n'
             '        run: python3 -m ci_verify kernel --build-dir build-kernel --ccache',
             'ci.yml job kernel-only must size'),
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

    def test_ci_contract_pins_the_docs_only_skip(self):
        """Each proof job waits for changes alone and skips only its docs-only
        answer; changes and preflight start at once on every run."""
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        skip = f'    if: {DOCS_ONLY_SKIP}\n'
        mutations = []
        for job in PROOF_JOBS:
            refused = f'{job} must start alongside preflight, after changes alone'
            mutations += [
                (job, '    needs: changes\n', '', refused),
                (job, '    needs: changes\n', '    needs: [changes, preflight]\n', refused),
                (job, '    needs: changes\n', '    needs: preflight\n', refused),
                (job, skip, '', refused),
                # A failed or unanswered classification must run the job.
                (job, skip, "    if: ${{ needs.changes.outputs.docs_only != 'true' }}\n", refused),
                (job, skip, "    if: ${{ !cancelled() && needs.changes.outputs.docs_only == 'false' }}\n",
                 refused),
                (job, skip, skip.replace("!= 'true'", "!= 'true' && github.event_name != 'push'"),
                 refused),
                # A skip the aggregate would not accept: a failed classification.
                (job, skip, "    if: ${{ !cancelled() && needs.changes.outputs.docs_only != 'true' }}\n",
                 refused)]
        mutations += [
            ('preflight', '    timeout-minutes: 360\n', '    timeout-minutes: 360\n    needs: changes\n',
             'preflight must start at once'),
            ('preflight', '    timeout-minutes: 360\n', '    timeout-minutes: 360\n' + skip,
             'preflight must start at once'),
            ('changes', '    timeout-minutes: 360\n', '    timeout-minutes: 360\n    needs: preflight\n',
             'changes must start at once'),
            ('changes', '    timeout-minutes: 360\n',
             "    timeout-minutes: 360\n    if: github.event_name != 'pull_request'\n",
             'changes must start at once'),
            # A lane that continues on error reports success however it ended.
            ('sanitizers', '    timeout-minutes: 360\n', '    timeout-minutes: 360\n    continue-on-error: true\n',
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
                ('kernel-only', '    needs: changes\n', '    needs: [changes]\n'),
                ('sanitizers', skip, f'    if: "{DOCS_ONLY_SKIP}"\n'),
                ('changes', '    steps:\n', '    # classify first\n    steps:\n')):
            with self.subTest(job=job, harmless=after):
                changed = original.copy()
                changed[0] = in_job(changed[0], job, before, after)
                self.assertEqual(ci_workflow_findings(*changed, others=others), [])

    def test_the_build_aggregate_passes_a_docs_only_skip_and_nothing_else(self):
        """Run the aggregate's own shell over every outcome of every lane: a
        proof lane passes by succeeding, or by being skipped after changes
        succeeded and answered docs-only; preflight always has to succeed."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        script = Path(directory.name) / 'gate.sh'
        script.write_text(job_script(self.ci_sources()[0], 'build-gate'))
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin')}
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
            expected = preflight == 'success' and all(
                result == 'success' or (excused and result == 'skipped') for result in results)
            with self.subTest(case=dict(zip(names, case))):
                self.assertEqual(answer == '0', expected, answer)
        # And as the runner starts it: the docs-only pass, and a skip that is not.
        docs_only = dict(zip(names, ('success', 'true', 'success', *['skipped'] * len(lanes))))
        for override, code in (({}, 0), ({'DOCS_ONLY': 'false'}, 1), ({'CHANGES_RESULT': 'failure'}, 1)):
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
                 f'            --jobs {CORES} --ccache'),
                (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
                 f'run: |\n          JOBS={CORES} ./scripts/check_corpus_parity.sh --subset'),
                (5, 'build', '    runs-on: ubuntu-latest\n', '    runs-on: "ubuntu-latest"\n'),
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
                 f'kernel --build-dir build-kernel --jobs {CORES} --ccache'),
                (2, 'promote', '    runs-on: ubuntu-latest\n', '    runs-on:\n      ubuntu-latest\n'),
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
        standard = '    runs-on: ubuntu-24.04\n'
        workflow = ('name: lint\non: workflow_dispatch\njobs:\n  lint:\n' + standard
                    + '    steps:\n      - run: "true"\n')
        mutations = [(f'    runs-on: "{label}"\n', 'must not use larger-runner labels')
                     for label in ('pf-linux-x64-16', 'macos-26-xlarge', 'macos-26-large',
                                   'ubuntu-24.04-xlarge', 'windows-2025-large')]
        mutations += [(f'    strategy:\n      matrix: {{{key}: ubuntu-24.04}}\n' + standard,
                       'must not define a larger_runner key')
                      for key in ('larger_runner', '"larger_runner"', "'larger_runner'")]
        for name in ('lint.yml', 'release.yml'):
            self.assertEqual(ci_workflow_findings(
                *original, others=dict(others, **{name: workflow})), [])
            for replacement, finding in mutations:
                with self.subTest(name=name, replacement=replacement):
                    changed = workflow.replace(standard, replacement, 1)
                    findings = ci_workflow_findings(
                        *original, others=dict(others, **{name: changed}))
                    self.assertTrue(any(f'{name} {finding}' in line for line in findings), findings)

    def test_every_workflow_a_pull_request_can_start_stays_on_standard_runners(self):
        original, others = self.ci_sources(), self.other_sources()
        # release.yml starts only on a dispatch, so no pull request reaches its runners.
        self.assertIn('release.yml', others)
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        for spelling in ('on:\n  pull_request:\n  workflow_dispatch:\n',
                         'on:\n  "pull_request":\n  workflow_dispatch:\n',
                         'on:\n  pull_request :\n  workflow_dispatch:\n'):
            with self.subTest(spelling=spelling):
                opened = others['release.yml'].replace('on:\n  workflow_dispatch:\n', spelling, 1)
                findings = ci_workflow_findings(*original, others=dict(others, **{'release.yml': opened}))
                self.assertTrue(any('release.yml job prebuilt must stay on a standard runner' in line
                                    for line in findings), findings)
        new = 'name: lint\non: {on}\njobs:\n  lint:\n    runs-on: {runner}\n    steps:\n      - run: "true"\n'
        for on, runner, refused in (
                ('[pull_request]', 'pf-linux-x64-16', True),
                ('pull_request_target', 'macos-26-xlarge', True),
                ('\n  pull_request:\n', '\n      group: pineforge-ci-large', True),
                ('[push,\n  pull_request]', 'pf-linux-x64-16', True),
                ('>-\n  pull_request', 'macos-26-xlarge', True),
                ('[pull_request]', 'ubuntu-latest', False),
                # Only release.yml is exempt, by name: a new workflow is checked
                # whatever starts it, so no on: spelling can exempt one.
                ('\n  workflow_dispatch:\n', 'pf-linux-x64-16', True)):
            with self.subTest(on=on, runner=runner):
                findings = ci_workflow_findings(
                    *original, others=dict(others, **{'lint.yml': new.format(on=on, runner=runner)}))
                self.assertEqual(any('lint.yml job lint must stay on a standard runner' in line
                                     for line in findings), refused, findings)
        # A decoy on key inside a multi-line string is no exemption.
        decoy = ('name: "lint\non: push"\non:\n  pull_request:\njobs:\n  lint:\n'
                 '    runs-on: pf-linux-x64-16\n    steps:\n      - run: "true"\n')
        self.assertTrue(any('lint.yml job lint must stay on a standard runner' in line for line in
                            ci_workflow_findings(*original, others=dict(others, **{'lint.yml': decoy}))))

    def test_no_stage_runs_under_a_timeout_and_jobs_keep_the_platform_maximum(self):
        # Owner rule, 2026-10-12: no stage of this preflight is run under a
        # timeout of its own any more (its verifier-tests stage has taken
        # longer than the old 2400 s bound on slow hosts), and every pinned
        # job limit is GitHub's own 360-minute maximum -- the only hang
        # catcher left, and deliberately the same for a five-second job and
        # the full verifier.
        self.assertFalse(hasattr(ci_preflight, 'STAGE_TIMEOUT_SECONDS'))
        source = (ROOT / 'scripts/ci_preflight.py').read_text()
        stage_call = re.search(r'result = subprocess\.run\(argv, cwd=source[^\n]*\)',
                               source).group()
        self.assertNotIn('timeout', stage_call)
        self.assertNotIn('TimeoutExpired', source)
        for workflow, jobs in JOB_RUNNERS.items():
            for job, (_, minutes, _) in jobs.items():
                with self.subTest(workflow=workflow, job=job):
                    self.assertEqual(minutes, 360)

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

    def test_the_preflight_stage_fails_a_paid_runner_or_larger_runner_key(self):
        code, output = self.contract_stage(None)
        self.assertEqual(code, 0, output)
        self.assertIn('runners and time limits OK', output)
        for mutation, finding in (
                (('sanitizers', LINUX_RUNNER, 'pf-linux-x64-16'), 'ci.yml job sanitizers must run on'),
                (('build', 'os: [ubuntu-24.04, macos-26]', 'os: [ubuntu-24.04, macos-26-xlarge]'),
                 'ci.yml must not use larger-runner labels'),
                (('build', '        build_type: [Release, Debug]\n',
                  '        build_type: [Release, Debug]\n        larger_runner: ubuntu-24.04\n'),
                 'ci.yml must not define a larger_runner key')):
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
            ('needs: native-live', 'needs: []'),
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
        script = job_script(native, 'native-live-gate')
        for result in ('success', 'failure', 'cancelled', 'skipped', ''):
            with self.subTest(result=result):
                ran = subprocess.run(['bash', '-e', '-c', script], capture_output=True,
                                     env={**os.environ, 'PROFILES_RESULT': result})
                self.assertEqual(ran.returncode == 0, result == 'success', ran.stdout)
                if result != 'success':
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
        # 360 minutes: GitHub's own job maximum, the only hang catcher left.
        self.assertEqual(job.count('timeout-minutes: 360'), 1)
        self.assertNotRegex(job, r'(?m)^        timeout-minutes:')
        for budget in ('matrix.job_minutes', 'matrix.verify_minutes',
                       '--test-timeout', '--ctest-timeout'):
            self.assertNotIn(budget, job)
        self.assertIn('needs: native-live', _jobs(native)['native-live-gate'])

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
