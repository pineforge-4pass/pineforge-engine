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
# A larger runner bills the organization even for a public repository, so only
# an event no fork can raise -- a push, the schedule, a maintainer's dispatch --
# or a pull_request whose head is a branch of this repository runs the heavy
# jobs on the organization's 16-core Linux runner or GitHub's M2 runner for the
# macos-26 image. Any other event -- a fork's pull request, any
# pull_request_target, a review or review comment (anyone can post one on a
# public repository's pull request), a comment, a workflow_run -- gets the
# standard runner. In a workflow_call the github context is the caller's.
TRUSTED_EVENT = ("github.event_name == 'push' || github.event_name == 'schedule' || "
                 "github.event_name == 'workflow_dispatch' || "
                 "(github.event_name == 'pull_request' && "
                 "github.event.pull_request.head.repo.full_name == github.repository)")
LINUX_RUNNER = '${{ (' + TRUSTED_EVENT + ") && 'pf-linux-x64-16' || 'ubuntu-24.04' }}"
MATRIX_RUNNER = '${{ (' + TRUSTED_EVENT + ') && matrix.larger_runner || matrix.os }}'
# The build job's whole strategy, exactly (comments and blank lines aside): an
# include entry more -- a leg whose os is a larger runner, or one that
# overrides a larger_runner -- is a finding.
BUILD_STRATEGY = ('    strategy:\n'
                  '      fail-fast: false\n'
                  '      matrix:\n'
                  '        os: [ubuntu-24.04, macos-26]\n'
                  '        build_type: [Release, Debug]\n'
                  '        include:\n'
                  '          - os: ubuntu-24.04\n'
                  '            larger_runner: pf-linux-x64-16\n'
                  '          - os: macos-26\n'
                  '            larger_runner: macos-26-xlarge\n')
BUILD_NAME = 'build (${{ matrix.os }}, ${{ matrix.build_type }})'
# Build and CTest parallelism is the core count of whichever runner took the job.
CORES = '"$(getconf _NPROCESSORS_ONLN)"'
VERIFY, PARITY = 'ci_verify.py', 'check_corpus_parity.sh'
PARITY_COMMAND = f'JOBS={CORES} ./scripts/{PARITY}'
# Every job with a runner in the workflows a CI run starts: its runs-on, its
# timeout-minutes, exactly, and the command that must carry the core count.
# docs/ci.md gives each limit's measured basis.
JOB_RUNNERS = {
    'ci.yml': {'preflight': (LINUX_RUNNER, 45, None),
               'build': (MATRIX_RUNNER, 75, VERIFY),
               'sanitizers': (LINUX_RUNNER, 120, VERIFY),
               'kernel-only': (LINUX_RUNNER, 60, VERIFY),
               'build-gate': ('ubuntu-24.04', 5, None)},
    'native-live.yml': {'native-live': (LINUX_RUNNER, 60, VERIFY)},
    'corpus-parity.yml': {'corpus-parity': (LINUX_RUNNER, 120, PARITY),
                          'corpus-parity-subset': (LINUX_RUNNER, 30, PARITY)},
}
# Every other workflow keeps its jobs on these. release.yml is exempt by name,
# while its on: block names only events no fork can raise (a push, the
# schedule, a dispatch; a dispatch alone today); any other workflow file is
# checked, whatever starts it.
STANDARD_RUNNERS = ('ubuntu-24.04', 'ubuntu-latest')
TRUSTED_ONLY_WORKFLOWS = ('release.yml',)
TRUSTED_ONLY_EVENTS = {'push', 'schedule', 'workflow_dispatch'}
# One stage's bound. The verifier self-tests (test_ci_verify.py) drive the real
# literal-aware parity, receipt and submodule guards in one serial process. On
# the maintainers' verification hosts they took 475 s at 0d76a099's tree (458 s
# in this change's own run, which leaves the suite alone), and those hosts ran
# them 1.93-1.95 times as fast as the standard hosted runner on the same trees
# (91d65ad6, 1a0e7ea1): about 925 s there. The stage starts within a minute of
# the preflight job, so its bound fits inside the job's limit and a stuck
# verifier-tests is logged here rather than cut off with the job.
STAGE_TIMEOUT_SECONDS = 2400
# A job's header -- any id GitHub accepts -- and the top-level jobs and on keys.
_JOB_HEADER = re.compile(r'^  ([A-Za-z_][A-Za-z0-9_-]*):[ \t]*(?:#.*)?$', re.MULTILINE)
_JOBS_KEY = re.compile(r'^jobs:[ \t]*(?:#.*)?$', re.MULTILINE)
_ON_KEY = re.compile(r'^(?:on|"on"|\'on\')[ \t]*:[ \t]*([^#\n]*)', re.MULTILINE)
# --jobs and every abbreviation argparse accepts for it in ci_verify.py, and a
# call of ci_verify itself, as a script or a module.
_JOBS_FLAG = re.compile(r'(?<![\w-])--j(?:o|ob|obs)?(?=[\s=]|$)')
_VERIFY_CALL = re.compile(r'\bci_verify\.py\b|-m\s+ci_verify\b')


def _jobs_text(workflow: str) -> str | None:
    """The jobs mapping: from the jobs key to the next top-level key."""
    key = _JOBS_KEY.search(workflow)
    if not key:
        return None
    text = workflow[key.end():]
    after = re.search(r'^[^\s#]', text, re.MULTILINE)
    return text[:after.start()] if after else text


def _jobs(workflow: str) -> dict[str, str]:
    text = _jobs_text(workflow) or ''
    matches = list(_JOB_HEADER.finditer(text))
    return {match.group(1): text[match.end():
                                 matches[index + 1].start() if index + 1 < len(matches)
                                 else len(text)]
            for index, match in enumerate(matches)}


def _code(text: str) -> list[str]:
    return [line for line in text.split('\n')
            if line.strip() and not line.lstrip().startswith('#')]


def _indent(line: str) -> int:
    return len(line) - len(line.lstrip(' '))


def _unparsed(workflow: str) -> list[str]:
    """Lines of the jobs mapping no job header accounts for: the parse fails closed."""
    text = _jobs_text(workflow)
    if text is None:
        return ['jobs:']
    first = _JOB_HEADER.search(text)
    stray = _code(text[:first.start()] if first else text)
    for body in _jobs(workflow).values():
        stray += [line for line in _code(body) if _indent(line) <= 2]
    return stray


def _key_value(line: str, key: str) -> str | None:
    """The value after `key:` on this line, the key bare or quoted; else None."""
    match = re.match(r'(["\']?)' + re.escape(key) + r'\1[ \t]*:(?:[ \t]+(.*)|$)', line.strip())
    if match is None:
        return None
    value = match.group(2) or ''
    return '' if value.startswith('#') else value


def _job_values(body: str, key: str) -> list[str]:
    """Every value of a job-level key, a block value's lines folded onto one."""
    lines = body.split('\n')
    code = _code(body)
    if not code:
        return []
    depth = _indent(code[0])
    values = []
    for index, line in enumerate(lines):
        first = _key_value(line, key) if _indent(line) == depth else None
        if first is None:
            continue
        parts = [first]
        for follow in lines[index + 1:]:
            if follow.strip() and _indent(follow) <= depth:
                break
            parts.append(follow)
        value = ' '.join(re.sub(r'\s+#.*$', '', part).strip() for part in parts
                         if part.strip() and not part.lstrip().startswith('#'))
        quoted = re.fullmatch(r'"([^"]*)"|\'([^\']*)\'', value)
        values.append(next(group for group in quoted.groups() if group is not None)
                      if quoted else value)
    return values


def _job_block(body: str, key: str) -> list[str]:
    """A job-level key's code lines, its own and every one under it."""
    lines = body.split('\n')
    code = _code(body)
    depth = _indent(code[0]) if code else 0
    for index, line in enumerate(lines):
        if _indent(line) == depth and _key_value(line, key) == '':
            block = [line]
            for follow in lines[index + 1:]:
                if follow.strip() and _indent(follow) <= depth:
                    break
                block.append(follow)
            return [re.sub(r'\s+#.*$', '', line) for line in _code('\n'.join(block))]
    return []


def _counted_text(body: str) -> str:
    """A job's code for the core-count rule: comments, step names and the
    hashFiles() arguments of cache keys -- names of files, not commands -- out."""
    lines = [re.sub(r'\s+#.*$', '', line) for line in _code(body)
             if not re.match(r'\s*(?:-\s+)?name\s*:', line)]
    return re.sub(r'hashFiles\([^)]*\)', '', '\n'.join(lines))


def _sized(text: str, command: str | None) -> bool:
    """Whether a job's work runs at the runner's core count.

    Counted over the whole job, in order, so no line, block or chaining form
    matters: every ci_verify call carries one core-count --jobs before the next
    call, no other --jobs spelling appears, and every JOBS and parity-script
    mention is part of the one parity command.
    """
    marks = sorted([(match.start(), 'C') for match in _VERIFY_CALL.finditer(text)]
                   + [(match.start(), 'F' if text.startswith(
                       (f'--jobs {CORES}', f'--jobs={CORES}'), match.start()) else 'U')
                      for match in _JOBS_FLAG.finditer(text)])
    calls = ''.join(kind for _, kind in marks)
    parity = text.count(PARITY_COMMAND)
    return (re.fullmatch(r'(?:CF)*', calls) is not None
            and len(re.findall(r'\bJOBS\s*[:=]', text)) == parity
            and text.count(PARITY) == parity
            and (command != VERIFY or bool(calls))
            and (command != PARITY or parity > 0))


def _events(workflow: str) -> set[str] | None:
    """The events that start a workflow, or None when the on key cannot be read."""
    matches = list(_ON_KEY.finditer(workflow))
    if len(matches) != 1:
        return None
    match = matches[0]
    inline = match.group(1).strip()
    if inline:
        if inline[0] in '>|&*!' or (inline[0] in '[{'
                                     and inline[-1] != {'[': ']', '{': '}'}[inline[0]]):
            return None
        return set(re.findall(r'[A-Za-z_]+', inline)) or None
    block = workflow[match.end():]
    after = re.search(r'^[^\s#]', block, re.MULTILINE)
    block = block[:after.start()] if after else block
    keys = [line for line in _code(block) if _indent(line) <= 2]
    events = [re.fullmatch(r'  ([A-Za-z_]+):(?:[ \t].*)?', line) for line in keys]
    if not keys or not all(events):
        return None
    return {event.group(1) for event in events}


def runner_findings(workflows: dict[str, str]) -> list[str]:
    """Pin every CI job's runner, time limit and parallelism; keep forks off larger runners."""
    findings = []
    for name, workflow in workflows.items():
        pinned = JOB_RUNNERS.get(name, {})
        events = _events(workflow)
        if (not pinned and name in TRUSTED_ONLY_WORKFLOWS and events is not None
                and events <= TRUSTED_ONLY_EVENTS):
            continue
        if _unparsed(workflow):
            findings.append(f'{name} has jobs lines the runner contract cannot read')
        jobs = _jobs(workflow)
        findings += [f'{name} is missing job {job}' for job in pinned if job not in jobs]
        for job, body in jobs.items():
            runs_on, uses = _job_values(body, 'runs-on'), _job_values(body, 'uses')
            if any(not value.startswith('./.github/workflows/') for value in uses):
                findings.append(f'{name} job {job} calls a workflow the runner contract '
                                'cannot see; call one under ./.github/workflows/')
            if job not in pinned:
                if pinned and runs_on:
                    findings.append(f'{name} job {job} needs a pinned runner and time limit')
                elif not uses and len(runs_on) != 1:
                    findings.append(f'{name} job {job} has a runs-on the runner contract '
                                    'cannot read')
                elif any(value not in STANDARD_RUNNERS for value in runs_on):
                    findings.append(f'{name} job {job} must stay on a standard runner '
                                    f'({" or ".join(STANDARD_RUNNERS)})')
                continue
            runner, minutes, command = pinned[job]
            if runs_on != [runner]:
                findings.append(f'{name} job {job} must run on {runner}')
            if _job_values(body, 'timeout-minutes') != [str(minutes)]:
                findings.append(f'{name} job {job} must allow {minutes} minutes')
            # A job on 16 cores that still asks for 4 wastes the runner it pays for.
            if not _sized(_counted_text(body), command):
                findings.append(f'{name} job {job} must size its parallelism to the runner')
    build = _jobs(workflows.get('ci.yml', '')).get('build', '')
    if _job_block(build, 'strategy') != _code(BUILD_STRATEGY):
        findings.append('ci.yml build must pair each image with its larger runner, exactly')
    if _job_values(build, 'name') != [BUILD_NAME]:
        findings.append(f'ci.yml build legs must keep their names: {BUILD_NAME}')
    return findings


def ci_workflow_findings(ci: str, native: str, promote: str, cmake: str,
                         parity: str, docs: str, others: dict[str, str] | None = None) -> list[str]:
    """Pin the PR-light/full-event split, parallel start, merge statuses, row home and runners.

    ``others`` holds every other workflow file by name, for the runner rule.
    """
    findings = runner_findings({'ci.yml': ci, 'native-live.yml': native,
                                'corpus-parity.yml': parity, 'docs.yml': docs,
                                'promote-baseline.yml': promote, **(others or {})})
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
    return list(dict.fromkeys(findings))


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
        workflows = ROOT / '.github/workflows'
        named = ('ci.yml', 'native-live.yml', 'promote-baseline.yml', 'corpus-parity.yml',
                 'docs.yml')
        findings = ci_workflow_findings(
            (workflows / 'ci.yml').read_text(),
            (workflows / 'native-live.yml').read_text(),
            (workflows / 'promote-baseline.yml').read_text(),
            (ROOT / 'tests/CMakeLists.txt').read_text(),
            (workflows / 'corpus-parity.yml').read_text(),
            (workflows / 'docs.yml').read_text(),
            {path.name: path.read_text() for path in sorted(workflows.iterdir())
             if path.suffix in ('.yml', '.yaml') and path.name not in named})
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
