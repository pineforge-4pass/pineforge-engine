#!/usr/bin/env python3
"""Fast CI wiring and source checks. Does not build or verify the engine.

Run this before the complete ci_verify.py profiles. Both local use and GitHub
Actions require actionlint 1.7.12; missing tools and failed checks fail closed.

The documentation guards ``doc-anchors`` and ``doc-lint`` (R5 lane L14-A) are
ordinary fail-closed stages. They ran ADVISORY for exactly as long as their
offenders existed - the stale ``file:line`` anchors and the stale prose the two
page-rewriting lanes L14-B and L14-C were written to remove - and the
``--strict-docs`` flag that promoted them is gone with that state: the pages
are clean, so a guard that can be asked not to bind is decoration. The advisory
*mechanism* stays (a stage may still be declared ``(name, argv, True)``), for
the next guard that lands before the drift it names is gone.

``design-inventory`` and ``doc-pine-coverage`` were fail-closed from the day
they landed: each holds a convention its document already follows on every row,
so neither had drift to report or anything to wait for.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

from ci_verify import ROOT, source_guard_commands

ACTIONLINT_VERSION = '1.7.12'
# The names live only in tests/CMakeLists.txt. A digest pins that measured
# population without maintaining a second row list in this Python guard.
PR_SLOW_ROWS_SHA256 = 'e8bddea797abc46ca0deb671d5d35db3491b67a82e1413d08278530d12f7b67b'
# A larger runner bills the organization even for a public repository, so a
# pull request from a fork runs on the free standard runner. Every other event
# -- a push, a dispatch, the schedule, a same-repository pull request -- runs
# the heavy jobs on the organization's 16-core Linux runner or on GitHub's M2
# runner for the macos-26 image.
TRUSTED_EVENT = ("github.event_name != 'pull_request' || "
                 "github.event.pull_request.head.repo.full_name == github.repository")
LINUX_RUNNER = '${{ (' + TRUSTED_EVENT + ") && 'pf-linux-x64-16' || 'ubuntu-24.04' }}"
MATRIX_RUNNER = '${{ (' + TRUSTED_EVENT + ') && matrix.larger_runner || matrix.os }}'
BUILD_MATRIX = ('      matrix:\n'
                '        os: [ubuntu-24.04, macos-26]\n'
                '        build_type: [Release, Debug]\n'
                '        include:\n'
                '          - os: ubuntu-24.04\n'
                '            larger_runner: pf-linux-x64-16\n'
                '          - os: macos-26\n'
                '            larger_runner: macos-26-xlarge\n')
# Build and CTest parallelism is the core count of whichever runner took the job.
CORES = '"$(getconf _NPROCESSORS_ONLN)"'
# Every job with a runner in the workflows a CI run starts: its runs-on and its
# timeout-minutes, exactly. docs/ci.md gives each limit's measured basis.
JOB_RUNNERS = {
    'ci.yml': {'preflight': (LINUX_RUNNER, 30), 'build': (MATRIX_RUNNER, 75),
               'sanitizers': (LINUX_RUNNER, 120), 'kernel-only': (LINUX_RUNNER, 60),
               'build-gate': ('ubuntu-24.04', 5)},
    'native-live.yml': {'native-live': (LINUX_RUNNER, 60)},
    'corpus-parity.yml': {'corpus-parity': (LINUX_RUNNER, 120),
                          'corpus-parity-subset': (LINUX_RUNNER, 30)},
}
# The other workflows a pull request (a fork's included) can start.
STANDARD_RUNNERS = ('ubuntu-24.04', 'ubuntu-latest')
# One stage's bound. The verifier self-tests (test_ci_verify.py) drive the real
# literal-aware parity, receipt and submodule guards (L8d) and took 394-543 s
# on the standard hosted runner, past 570 s at 0d76a099. The bound stays inside
# the preflight job's time limit, so a stuck stage is logged here rather than
# cut off with the job.
STAGE_TIMEOUT_SECONDS = 1500


def _jobs(workflow: str) -> dict[str, str]:
    body = workflow.split('\njobs:\n', 1)
    if len(body) != 2:
        return {}
    matches = list(re.finditer(r'^  ([a-z][a-z0-9-]*):\s*$', body[1], re.MULTILINE))
    return {match.group(1): body[1][match.end():
                                    matches[index + 1].start() if index + 1 < len(matches)
                                    else len(body[1])]
            for index, match in enumerate(matches)}


def runner_findings(workflows: dict[str, str]) -> list[str]:
    """Pin every CI job's runner, time limit and parallelism; keep forks off larger runners."""
    findings = []
    for name, workflow in workflows.items():
        pinned = JOB_RUNNERS.get(name, {})
        jobs = _jobs(workflow)
        findings += [f'{name} is missing job {job}' for job in pinned if job not in jobs]
        for job, body in jobs.items():
            runs_on = re.findall(r'^    runs-on: (.*)$', body, re.MULTILINE)
            limit = re.findall(r'^    timeout-minutes: (.*)$', body, re.MULTILINE)
            if job not in pinned:
                if pinned and runs_on:
                    findings.append(f'{name} job {job} needs a pinned runner and time limit')
                elif any(value not in STANDARD_RUNNERS for value in runs_on):
                    findings.append(f'{name} job {job} must stay on a standard runner')
                continue
            runner, minutes = pinned[job]
            if runs_on != [runner]:
                findings.append(f'{name} job {job} must run on {runner}')
            if limit != [str(minutes)]:
                findings.append(f'{name} job {job} must allow {minutes} minutes')
            # A job on 16 cores that still asks for 4 wastes the runner it pays for.
            runs = re.findall(r'^ +run: (.*)$', body, re.MULTILINE)
            if (re.search(r'--jobs [0-9]|^ +JOBS: ', body, re.MULTILINE)
                    or any(f'--jobs {CORES}' not in run
                           for run in runs if 'scripts/ci_verify.py' in run)
                    or any(not run.startswith(f'JOBS={CORES} ./scripts/check_corpus_parity.sh')
                           for run in runs if 'check_corpus_parity.sh' in run)):
                findings.append(f'{name} job {job} must size its parallelism to the runner')
    build = _jobs(workflows.get('ci.yml', '')).get('build', '')
    if (BUILD_MATRIX not in build
            or '    name: build (${{ matrix.os }}, ${{ matrix.build_type }})\n' not in build):
        findings.append('ci.yml build must pair each image with its larger runner')
    return findings


def ci_workflow_findings(ci: str, native: str, promote: str, cmake: str,
                         parity: str, docs: str) -> list[str]:
    """Pin the PR-light/full-event split, parallel start, merge statuses, row home and runners."""
    findings = runner_findings({'ci.yml': ci, 'native-live.yml': native,
                                'corpus-parity.yml': parity, 'docs.yml': docs,
                                'promote-baseline.yml': promote})
    events = ci.split('\non:\n', 1)
    events = events[1].split('\npermissions:', 1)[0] if len(events) == 2 else ''
    for trigger in ('push:\n    branches: [main]',
                    'pull_request:\n    branches: [main]', 'workflow_dispatch:'):
        if '  ' + trigger not in events:
            findings.append(f'ci.yml must retain {trigger.split(":", 1)[0]}')
    jobs = _jobs(ci)
    proof_jobs = ('build', 'sanitizers', 'kernel-only', 'native-live',
                  'corpus-parity-subset')
    for job in ('preflight', *proof_jobs, 'build-gate'):
        if job not in jobs:
            findings.append(f'ci.yml is missing job {job}')
    for job in proof_jobs:
        if re.search(r'^    needs:', jobs.get(job, ''), re.MULTILINE):
            findings.append(f'{job} must start alongside preflight')
    gate = jobs.get('build-gate', '')
    if ('needs: [preflight, build, sanitizers, native-live, kernel-only, '
            'corpus-parity-subset]' not in gate or '    if: always()' not in gate
            or '    name: build' not in gate):
        findings.append('build-gate must aggregate every proof job and preflight')
    for job, variable in (('preflight', 'PREFLIGHT_RESULT'), ('build', 'BUILD_RESULT'),
                          ('sanitizers', 'SANITIZER_RESULT'), ('native-live', 'NATIVE_RESULT'),
                          ('kernel-only', 'KERNEL_RESULT'),
                          ('corpus-parity-subset', 'PARITY_RESULT')):
        if (f'${{{{ needs.{job}.result }}}}' not in gate
                or f'"${variable}" == "success"' not in gate):
            findings.append(f'build-gate must require {job} success')
    debug_flag = "${{ github.event_name == 'pull_request' && matrix.build_type == 'Debug' && '--exclude-label slow' || '' }}"
    pr_flag = "${{ github.event_name == 'pull_request' && '--exclude-label slow' || '' }}"
    if debug_flag not in jobs.get('build', '') or jobs.get('build', '').count('--exclude-label slow') != 1:
        findings.append('only PR Debug matrix jobs may exclude slow rows')
    if pr_flag not in jobs.get('sanitizers', '') or jobs.get('sanitizers', '').count('--exclude-label slow') != 1:
        findings.append('only PR sanitizers may exclude slow rows')
    if 'exclude_slow: ${{ github.event_name == \'pull_request\' }}' not in jobs.get('native-live', ''):
        findings.append('native-live must receive the PR-only exclusion input')
    for job in ('kernel-only', 'corpus-parity-subset'):
        if '--exclude-label' in jobs.get(job, ''):
            findings.append(f'{job} must keep its full population')
    if ('  workflow_call:\n    inputs:\n      exclude_slow:' not in native
            or '        type: boolean\n        default: false' not in native
            or '  workflow_dispatch:' not in native
            or "${{ inputs.exclude_slow && '--exclude-label slow' || '' }}" not in native):
        findings.append('native-live must default to full rows for dispatch and push')
    # Baseline promotion is pineforge-workflow's campaign/ci/promote-baseline.yml,
    # installed unchanged. Main's own copy runs on the closed PR
    # (pull_request_target, never the PR's copy) and runs no PR code; the
    # engine axis needs the newest pineforge/verify and pineforge/parity
    # statuses on the PR head.
    if ('  pull_request_target:\n    types: [closed]' not in promote
            or re.search(r'^  pull_request:', promote, re.MULTILINE)
            or '  statuses: read' not in promote
            or '  id-token: write' not in promote
            or 'ref: ${{ github.event.pull_request.head' in promote
            or '/commits/$HEAD_SHA/statuses?per_page=100' not in promote
            or 'gh api --paginate' not in promote
            or 'reduce (.[][]) as $s' not in promote
            or 'if has($s.context) then . else .[$s.context] = $s.state end' not in promote
            or '.["pineforge/verify"] == "success"' not in promote
            or '.["pineforge/parity"] == "success"' not in promote
            or promote.count('echo "green=false" >> "$GITHUB_OUTPUT"; exit 0') != 2):
        findings.append("baseline promotion must run main's copy and require the latest two head statuses")
    # PRs are squash-merged, so the PR head is never an ancestor of the base
    # branch. The merge commit (the event's merge_commit_sha, or the dispatch
    # input) must be on the base branch and carry the gated PR head's tree; the
    # campaign tool gets that tree and promotes only the pair the merge gate's
    # verdict measured for it. Every skip exits green before promotion.
    merge_env = ('MERGE_SHA: ${{ github.event.pull_request.merge_commit_sha'
                 ' || github.event.inputs.merge_commit }}')
    head_env = 'HEAD_SHA: ${{ github.event.pull_request.head.sha || github.event.inputs.head_sha }}'
    if (promote.count(merge_env) != 2 or promote.count(head_env) != 3
            or 'github.event.pull_request.head.sha || github.event.inputs.merge_commit' in promote
            or '      head_sha:\n' not in promote
            or 'git merge-base --is-ancestor "$MERGE_SHA" "origin/$BASE_REF"' not in promote
            or '--is-ancestor "$HEAD_SHA"' in promote
            or 'merge_tree=$(git rev-parse "$MERGE_SHA^{tree}")' not in promote
            or 'head_tree=$(git rev-parse "$HEAD_SHA^{tree}")' not in promote
            or 'if [ "$merge_tree" != "$head_tree" ]; then' not in promote
            or promote.count('echo "ok=false" >> "$GITHUB_OUTPUT"; exit 0') != 2
            or 'MERGE_TREE: ${{ steps.exacttree.outputs.merge_tree }}' not in promote
            or '--merge-commit "$MERGE_SHA" --head-sha "$HEAD_SHA" --merge-tree "$MERGE_TREE"'
            not in promote
            or '--ci-head "$HEAD_SHA" --ci-green' not in promote
            or 'if [ "$code" = "2" ]; then' not in promote):
        findings.append('baseline promotion must admit a squash merge by tree equality only')
    blocks = re.findall(r'^set\(PINEFORGE_PR_SLOW_TESTS\n(.*?)^\)', cmake,
                        re.MULTILINE | re.DOTALL)
    names = re.findall(r'^    (test_[A-Za-z0-9_]+)$', blocks[0], re.MULTILINE) if len(blocks) == 1 else []
    canonical = '\n'.join(names) + '\n'
    if (len(blocks) != 1 or len(names) != 29 or len(set(names)) != 29
            or hashlib.sha256(canonical.encode()).hexdigest() != PR_SLOW_ROWS_SHA256
            or cmake.count('APPEND PROPERTY LABELS slow') != 1
            or 'set_property(TEST ${_pf_slow_test} APPEND PROPERTY LABELS slow)' not in cmake
            or 'if(PINEFORGE_BUILD_SOURCE_LAYER AND NOT TEST ${_pf_slow_test})' not in cmake):
        findings.append('measured slow rows must keep one pinned CMake label list')
    return findings


def docs_workflow_findings(workflow: str) -> list[str]:
    """Pin the PR build event and the deploy exclusion beside actionlint."""
    findings = []
    events = workflow.split('\non:\n', 1)
    events = events[1].split('\npermissions:', 1)[0] if len(events) == 2 else ''
    if not re.search(r'^  pull_request:\s*(?:\{\})?\s*$', events, re.MULTILINE):
        findings.append('docs.yml must build on pull_request')
    deploy = workflow.split('      - name: Deploy to Cloudflare Pages\n', 1)
    deploy = deploy[1].split('\n      - name:', 1)[0] if len(deploy) == 2 else ''
    if not re.search(r"^        if: github\.event_name != 'pull_request'$",
                     deploy, re.MULTILINE):
        findings.append('docs.yml must skip deployment on pull_request')
    return findings


def docs_workflow_self_test(workflow: str) -> int:
    if docs_workflow_findings(workflow):
        print('docs workflow contract: current workflow fails')
        return 1
    no_pr = workflow.replace('  pull_request: {}\n', '', 1)
    no_skip = workflow.replace("        if: github.event_name != 'pull_request'\n", '', 1)
    if no_pr == workflow or no_skip == workflow:
        print('docs workflow contract: could not form both mutations')
        return 1
    if not docs_workflow_findings(no_pr) or not docs_workflow_findings(no_skip):
        print('docs workflow contract: a mutation escaped')
        return 1
    print('docs workflow contract: missing PR trigger and unguarded deploy both refused')
    return 0


def check_commands(source: Path) -> list[tuple]:
    return [
        ('shellcheck-version', ['shellcheck', '--version']),
        ('workflow-lint', ['actionlint', '-color',
                           str(source / '.github/workflows/ci.yml'),
                           str(source / '.github/workflows/native-live.yml'),
                           str(source / '.github/workflows/corpus-parity.yml'),
                           str(source / '.github/workflows/docs.yml'),
                           str(source / '.github/workflows/promote-baseline.yml'),
                           str(source / '.github/workflows/release.yml')]),
        ('ci-workflow-contract', [sys.executable, str(source / 'scripts/ci_preflight.py'),
                                  '--check-ci-workflow']),
        ('docs-workflow-contract', [sys.executable, str(source / 'scripts/ci_preflight.py'),
                                    '--check-docs-workflow']),
        ('docs-workflow-contract-tests',
         [sys.executable, str(source / 'scripts/ci_preflight.py'),
          '--self-test-docs-workflow']),
        *source_guard_commands(source),
        ('source-guard-market-admission',
         [sys.executable, str(source / 'scripts/check_market_admission_schema.py')]),
        ('source-guard-cancellation',
         [sys.executable, str(source / 'scripts/check_cancellation_hash_coverage.py')]),
        # R5 lane H-DOCGATES (AUDIT4-opus X17, perf N-2): two random draws whose
        # order C++ leaves to the compiler build a different battery on x86-64
        # GCC than on AppleClang -- XPLAT1's bug class, back at ten sites.
        ('rng-draw-order-tests',
         [sys.executable, str(source / 'scripts/test_check_rng_draw_order.py')]),
        ('rng-draw-order',
         [sys.executable, str(source / 'scripts/check_rng_draw_order.py')]),
        ('verifier-tests', [sys.executable, str(source / 'scripts/test_ci_verify.py')]),
        ('preflight-tests', [sys.executable, str(source / 'scripts/test_ci_preflight.py')]),
        ('native-c-surface-tests',
         [sys.executable, str(source / 'scripts/test_check_native_c_api_surface.py')]),
        ('kernel-residuals-tests',
         [sys.executable, str(source / 'scripts/test_check_kernel_residuals.py')]),
        ('corpus-parity-identity-tests',
         [sys.executable, str(source / 'scripts/test_corpus_trades_identity.py')]),
        # R5 lane H-MEASURE (AUDIT3 H12): the pull-request subset witnesses
        # every mutation of the recorded battery the full sweep caught.
        ('corpus-parity-subset-cover-tests',
         [sys.executable, str(source / 'scripts/test_corpus_parity_subset_cover.py')]),
        # Lane REL10: the JSON report keys outlive the removed C spellings, and
        # release.yml's version arithmetic is a tested script.
        ('report-schema-key-tests',
         [sys.executable, str(source / 'scripts/test_report_schema_keys.py')]),
        ('release-version-tests',
         [sys.executable, str(source / 'scripts/test_release_version.py')]),
        ('design-inventory-tests',
         [sys.executable, str(source / 'scripts/test_check_design_inventory.py')]),
        ('design-inventory',
         [sys.executable, str(source / 'scripts/check_design_inventory.py')]),
        ('detached-comments-tests',
         [sys.executable, str(source / 'scripts/measure_detached_comments.py'),
          '--self-test']),
        ('detached-comments',
         [sys.executable, str(source / 'scripts/measure_detached_comments.py'),
          '--check-ceiling']),
        ('native-tempdir-tests',
         [sys.executable, str(source / 'scripts/check_native_cpp_versions.py'),
          '--self-test-tempdir']),
        ('dangling-comment-names-tests',
         [sys.executable, str(source / 'scripts/test_check_dangling_comment_names.py')]),
        ('doc-anchors-tests',
         [sys.executable, str(source / 'scripts/test_check_doc_anchors.py')]),
        ('doc-anchors',
         [sys.executable, str(source / 'scripts/check_doc_anchors.py')]),
        ('doc-lint-tests',
         [sys.executable, str(source / 'scripts/test_check_doc_lint.py')]),
        ('doc-lint',
         [sys.executable, str(source / 'scripts/check_doc_lint.py')]),
        # R5 lane H-DOCGATES (AUDIT4-opus X13): a published sentence leaves only
        # when its commit's message names it -- by the introducing lane or commit,
        # or by its words -- and older text never silently replaces newer text.
        ('doc-reverts-tests',
         [sys.executable, str(source / 'scripts/test_check_doc_reverts.py')]),
        ('doc-reverts',
         [sys.executable, str(source / 'scripts/check_doc_reverts.py')]),
        # R5 lane H-DOCGATES (AUDIT4-opus X10): every kernel `source_*` seam and
        # every kernel member or row field the source layer writes is named by
        # an ADR-0001 row, which states who writes it and its ruling.
        ('kernel-seam-rows-tests',
         [sys.executable, str(source / 'scripts/test_check_kernel_seam_rows.py')]),
        ('kernel-seam-rows',
         [sys.executable, str(source / 'scripts/check_kernel_seam_rows.py')]),
        # NOT advisory: the migration page's coverage claim is L14-B's own, and
        # it is true on this tree. A `strategy.*` name added to the Pine v6
        # inventory without a row on that page fails here immediately.
        ('doc-pine-coverage',
         [sys.executable, str(source / 'scripts/check_pine_to_native_coverage.py')]),
        ('doc-pine-coverage-tests',
         [sys.executable, str(source / 'scripts/test_check_pine_to_native_coverage.py')]),
    ]


def run_checks(commands: list[tuple], output: Path, *, source: Path = ROOT) -> int:
    """Run every stage, log it, and answer 0 only when every binding one passed.

    A stage may be ``(name, argv)`` or ``(name, argv, advisory)``. An advisory
    stage still runs and still prints its findings, but its failure is recorded
    as ``reported`` and does not fail the preflight.
    """
    output.mkdir(parents=True, exist_ok=True)
    summary = {'schemaVersion': 'pineforge-ci-preflight/v1', 'status': 'incomplete',
               'scope': 'workflow and source checks only; full verification still required',
               'stages': []}
    summary_path = output / 'preflight-summary.json'

    def record() -> None:
        summary_path.write_text(json.dumps(summary, indent=2) + '\n')

    record()
    for entry in commands:
        name, argv = entry[0], entry[1]
        advisory = bool(entry[2]) if len(entry) > 2 else False
        print(f'ci_preflight: {name}: {shlex.join(argv)}'
              + (' [advisory]' if advisory else ''), flush=True)
        stage = {'name': name, 'argv': argv, 'status': 'running', 'exitCode': None,
                 'advisory': advisory, 'log': name + '.log'}
        summary['stages'].append(stage)
        record()
        try:
            result = subprocess.run(argv, cwd=source, capture_output=True,
                                    timeout=STAGE_TIMEOUT_SECONDS)
            code, log = result.returncode, result.stdout + result.stderr
        except subprocess.TimeoutExpired as error:
            code = 124
            log = (error.stdout or b'') + (error.stderr or b'') + b'\ncheck timed out\n'
        except OSError as error:
            code, log = 127, str(error).encode()
        (output / stage['log']).write_bytes(log)
        if log:
            print(log.decode('utf-8', 'replace'), end='', flush=True)
        if code and advisory:
            print(f'ci_preflight: {name}: exit {code}, reported only '
                  '(this stage is declared advisory)', flush=True)
            stage.update(status='reported', exitCode=code)
        else:
            stage.update(status='passed' if code == 0 else 'failed', exitCode=code)
        record()
    passed = bool(commands) and all(stage['exitCode'] == 0 or stage['advisory']
                                    for stage in summary['stages'])
    summary.update(status='passed' if passed else 'failed', exitCode=0 if passed else 1)
    record()
    return summary['exitCode']


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build-ci-preflight')
    parser.add_argument('--check-docs-workflow', action='store_true')
    parser.add_argument('--self-test-docs-workflow', action='store_true')
    parser.add_argument('--check-ci-workflow', action='store_true')
    args = parser.parse_args()
    if args.check_ci_workflow:
        findings = ci_workflow_findings(
            (ROOT / '.github/workflows/ci.yml').read_text(),
            (ROOT / '.github/workflows/native-live.yml').read_text(),
            (ROOT / '.github/workflows/promote-baseline.yml').read_text(),
            (ROOT / 'tests/CMakeLists.txt').read_text(),
            (ROOT / '.github/workflows/corpus-parity.yml').read_text(),
            (ROOT / '.github/workflows/docs.yml').read_text())
        for finding in findings:
            print(finding)
        if not findings:
            print('CI workflow contract: PR exclusions, full events, statuses, slow rows, '
                  'runners and time limits OK')
        return 1 if findings else 0
    if args.check_docs_workflow or args.self_test_docs_workflow:
        workflow = (ROOT / '.github/workflows/docs.yml').read_text()
        if args.self_test_docs_workflow:
            return docs_workflow_self_test(workflow)
        findings = docs_workflow_findings(workflow)
        for finding in findings:
            print(finding)
        if not findings:
            print('docs workflow contract: PR builds and deploy exclusion ... OK')
        return 1 if findings else 0
    # Pin the linter contract as well as its CI download. No optional lint lane.
    version_check = [sys.executable, '-c',
                     'import subprocess,sys; '
                     'v=subprocess.check_output(["actionlint","-version"],text=True).splitlines()[0]; '
                     f'print("actionlint "+v); sys.exit(0 if v == {ACTIONLINT_VERSION!r} else 1)']
    return run_checks([('actionlint-version', version_check), *check_commands(ROOT)],
                      args.output_dir.resolve())


if __name__ == '__main__':
    raise SystemExit(main())
