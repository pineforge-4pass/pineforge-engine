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
NATIVE_STRATEGY = '''    strategy:
      fail-fast: false
      matrix:
        include:
          - profile: native
            build_dir: build-live
            job_minutes: 75
            verify_minutes: 65
            test_timeout: 2700
            ctest_timeout: 2820
          - profile: live-sanitizers
            build_dir: build-live-sanitizers
            job_minutes: 45
            verify_minutes: 35
            test_timeout: 900
            ctest_timeout: 1200
          - profile: live-tsan
            build_dir: build-live-tsan
            job_minutes: 45
            verify_minutes: 35
            test_timeout: 900
            ctest_timeout: 1200
'''
NATIVE_EXCLUSION = "${{ matrix.profile == 'native' && inputs.exclude_slow && '--exclude-label slow' || '' }}"
NATIVE_CACHE_SAVE = ("${{ !cancelled() && !inputs.cold_cache && steps.verify.outcome != 'skipped' "
                     "&& steps.ccache-restore.outputs.cache-hit != 'true' }}")
CURL_CACHE_SAVE = ("${{ inputs.cold-cache != 'true' && steps.curl-build.outcome == 'success' "
                   "&& steps.curl-cache.outputs.cache-hit != 'true' }}")
# Build and CTest parallelism is the core count of whichever runner took the job.
CORES = '"$(getconf _NPROCESSORS_ONLN)"'
VERIFY, PARITY = 'ci_verify.py', 'check_corpus_parity.sh'
PARITY_COMMAND = f'JOBS={CORES} ./scripts/{PARITY}'
# Every job with a runner in the workflows a CI run starts: its runs-on, its
# timeout-minutes, exactly, and the command that must carry the core count.
# docs/ci.md gives each limit's measured basis.
JOB_RUNNERS = {
    'ci.yml': {'changes': ('ubuntu-24.04', 5, None),
               'preflight': (LINUX_RUNNER, 45, None),
               'build': (MATRIX_RUNNER, 75, VERIFY),
               'sanitizers': (LINUX_RUNNER, 120, VERIFY),
               'kernel-only': (LINUX_RUNNER, 60, VERIFY),
               'build-gate': ('ubuntu-24.04', 5, None)},
    'native-live.yml': {'native-live': (LINUX_RUNNER, '${{ matrix.job_minutes }}', VERIFY),
                         'native-live-gate': ('ubuntu-24.04', 5, None)},
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
# A docs-only change (scripts/ci_docs_only.py) skips the proof jobs. Each waits
# for the changes job alone and skips only when it succeeded and answered
# docs-only -- exactly the skip the build aggregate accepts -- so a failed or
# unanswered classification runs it.
PROOF_JOBS = {'build': 'BUILD_RESULT', 'sanitizers': 'SANITIZER_RESULT',
              'kernel-only': 'KERNEL_RESULT', 'native-live': 'NATIVE_RESULT',
              'corpus-parity-subset': 'PARITY_RESULT'}
DOCS_ONLY_SKIP = ("${{ !cancelled() && (needs.changes.result != 'success' || "
                  "needs.changes.outputs.docs_only != 'true') }}")
# The classification step, exactly: the base's copy of the rule judges the
# change, a base without one runs every job, and the answer is the script's.
CLASSIFY_STEP = """      - name: Classify the change
        id: classify
        env:
          PR_HEAD_SHA: ${{ github.event.pull_request.head.sha }}
          PUSH_BEFORE: ${{ github.event.before }}
          PUSH_FORCED: ${{ github.event.forced }}
        run: |
          if git show HEAD^1:scripts/ci_docs_only.py > "$RUNNER_TEMP/ci_docs_only.py"; then
            python3 "$RUNNER_TEMP/ci_docs_only.py" --root . --github-output "$GITHUB_OUTPUT"
          else
            echo "not docs-only: the base holds no scripts/ci_docs_only.py"
            echo "docs_only=false" >> "$GITHUB_OUTPUT"
          fi
"""
_CONTINUE_ON_ERROR = re.compile(r'\s*(?:-\s+)?["\']?continue-on-error["\']?\s*:')
GATE_NEEDS = ('needs: [changes, preflight, build, sanitizers, native-live, kernel-only, '
              'corpus-parity-subset]')
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


def _steps(text: str, depth: int = 6) -> list[str]:
    """Step blocks at one known depth; never match a nested shell/YAML value."""
    lines = text.splitlines()
    result = []
    for index, line in enumerate(lines):
        if not line.startswith(' ' * depth + '- '):
            continue
        block = [line]
        for follow in lines[index + 1:]:
            if follow.strip() and _indent(follow) <= depth:
                break
            block.append(follow)
        result.append('\n'.join(block))
    return result


def _step_named(steps: list[str], marker: str) -> str:
    matches = [step for step in steps if marker in _code(step)]
    # Duplicate ids/names cannot satisfy a contract by hiding a second step.
    return matches[0] if len(matches) == 1 else ''


def _step_values(step: str, key: str) -> list[str]:
    return _job_values('\n'.join(step.splitlines()[1:]), key)


def native_workflow_findings(native: str, curl_action: str | None = None) -> list[str]:
    findings = []
    jobs = _jobs(native)
    job, gate = jobs.get('native-live', ''), jobs.get('native-live-gate', '')
    if (_job_block(job, 'strategy') != _code(NATIVE_STRATEGY)
            or _job_values(job, 'needs') or _job_values(job, 'if')
            or _job_values(job, 'name') != ['native-live (${{ matrix.profile }})']):
        findings.append('native-live must run all three independent profiles with pinned budgets and fail-fast false')
    if (set(jobs) != {'native-live', 'native-live-gate'}
            or _job_values(gate, 'needs') != ['native-live']
            or _job_values(gate, 'if') != ['always()']
            or '          PROFILES_RESULT: ${{ needs.native-live.result }}' not in _code(gate)
            or '          test "$PROFILES_RESULT" = success' not in _code(gate)):
        findings.append('native-live-gate must fail unless every profile succeeds')
    for event in ('workflow_call', 'workflow_dispatch'):
        block = native.split(f'  {event}:\n', 1)[-1]
        block = re.split(r'(?m)^  [a-z_]+:|^\S', block, maxsplit=1)[0]
        cold = re.search(r'(?m)^      cold_cache:\n(.*?)(?=^      \w|\Z)', block, re.DOTALL)
        if (not cold or '        type: boolean' not in _code(cold[1])
                or '        default: false' not in _code(cold[1])):
            findings.append(f'native-live {event} must default cold_cache to false')
    exclusion_input = re.search(r'(?m)^      exclude_slow:\n(.*?)(?=^      \w|^  \w|\Z)',
                                native, re.DOTALL)
    if (not exclusion_input or '        type: boolean' not in _code(exclusion_input[1])
            or '        default: false' not in _code(exclusion_input[1])):
        findings.append('native-live exclude_slow must default to the full row set')
    if ('permissions:\n  contents: read\n' not in native
            or '  PINEFORGE_REQUIRE_RELEASE_TAGS: "1"' not in _code(native)):
        findings.append('native-live must retain read-only permissions and required release tags')
    steps = _steps(job)
    restore = _step_named(steps, '        id: ccache-restore')
    verify = _step_named(steps, '        id: verify')
    curl = _step_named(steps, '        id: curl-deps')
    save = _step_named(steps, '      - name: Save ccache after verification')
    collect = _step_named(steps, '      - name: Stage and summarize diagnostics')
    upload = _step_named(steps, '      - name: Retain CI diagnostics')
    cache_key = 'ccache-${{ runner.os }}-${{ runner.arch }}-live-v2-${{ matrix.profile }}-'
    if (_step_values(restore, 'if') != ['${{ !inputs.cold_cache }}']
            or _step_values(restore, 'uses') != ['actions/cache/restore@v4']
            or f'          key: {cache_key}${{{{ github.sha }}}}' not in _code(restore)
            or f'            {cache_key}' not in _code(restore)
            or '      CCACHE_DIR: ${{ github.workspace }}/.ccache/${{ matrix.profile }}' not in _code(job)
            or '          ccache --zero-stats' not in _code(job)):
        findings.append('native-live compiler restore must be profile-isolated, cold-bypassable and reset statistics')
    if (_step_values(curl, 'uses') != ['./.github/actions/setup-live-curl']
            or '          cache-namespace: ${{ matrix.profile }}' not in _code(curl)
            or '          cold-cache: ${{ inputs.cold_cache }}' not in _code(curl)):
        findings.append('every native profile must use pinned curl with its namespace and cold-cache input')
    command = _step_values(verify, 'run')
    expected = ('python3 scripts/ci_verify.py ${{ matrix.profile }} '
                '--build-dir ${{ matrix.build_dir }} ' + f'--jobs {CORES} '
                '--generator Ninja --curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" '
                '--ccache --test-timeout ${{ matrix.test_timeout }} '
                '--ctest-timeout ${{ matrix.ctest_timeout }} '
                "${{ matrix.profile == 'native' && '--require-websocket' || '' }} " + NATIVE_EXCLUSION)
    if (len(command) != 1 or command[0].replace('--jobs=', '--jobs ') != expected
            or _step_values(verify, 'if')
            or _step_values(verify, 'timeout-minutes') != ['${{ matrix.verify_minutes }}']
            or job.count('--exclude-label') != 1 or job.count('scripts/ci_verify.py ') != 1):
        findings.append('native-live must verify each full profile with bounded CTest and native-only PR exclusion')
    if (_step_values(save, 'if') != [NATIVE_CACHE_SAVE]
            or _step_values(save, 'uses') != ['actions/cache/save@v4']
            or '          key: ${{ steps.ccache-restore.outputs.cache-primary-key }}' not in _code(save)
            or '          path: ${{ env.CCACHE_DIR }}' not in _code(save)
            or (save and upload and job.index(save) < job.index(upload))):
        findings.append('native-live must explicitly save compatible compiler objects after diagnostics on normal test failure')
    if (_step_values(collect, 'if') != ['always()']
            or _step_values(upload, 'if') != ['always()']
            or '--build-dir ${{ matrix.build_dir }} --profile ${{ matrix.profile }}' not in collect
            or '          name: ci-diagnostics-native-live-${{ matrix.profile }}' not in _code(upload)
            or '          COLD_CACHE: ${{ inputs.cold_cache }}' not in _code(collect)
            or '          fetch-depth: 0' not in _code(job)
            or '          persist-credentials: false' not in _code(job)):
        findings.append('native-live must retain per-profile diagnostics on failure and preserve safe full checkout')
    if curl_action is not None:
        action_steps = _steps(curl_action, 4)
        restored = _step_named(action_steps, '      id: curl-cache')
        built = _step_named(action_steps, '      id: curl-build')
        saved = _step_named(action_steps, '    - name: Save pinned WebSocket-enabled libcurl')
        if (_step_values(restored, 'if') != ["${{ inputs.cold-cache != 'true' }}"]
                or _step_values(restored, 'uses') != ['actions/cache/restore@v4']
                or 'key: curl-${{ inputs.cache-namespace }}-' not in restored
                or 'restore-keys:' in restored
                or any(token not in restored for token in (
                    'steps.dependencies.outputs.version', 'steps.dependencies.outputs.sha256',
                    'steps.dependencies.outputs.identity', "hashFiles('.github/actions/setup-live-curl/action.yml', 'scripts/build_live_curl.sh')"))
                or '  cold-cache:\n' not in curl_action or "    default: 'false'" not in curl_action):
            findings.append('pinned curl restore must include dependency identity and bypass every cold-cache restore')
        if (_step_values(saved, 'if') != [CURL_CACHE_SAVE]
                or _step_values(saved, 'uses') != ['actions/cache/save@v4']
                or _step_values(built, 'run') != ['bash scripts/build_live_curl.sh build-native-deps 4']
                or '        key: ${{ steps.curl-cache.outputs.cache-primary-key }}' not in _code(saved)
                or not built or not saved or curl_action.index(saved) < curl_action.index(built)
                or 'uses: actions/cache@v4' in curl_action):
            findings.append('pinned curl must save immediately after its successful build, independently of later tests')
    return findings


def ci_workflow_findings(ci: str, native: str, promote: str, cmake: str,
                         parity: str, docs: str, others: dict[str, str] | None = None,
                         *, curl_action: str | None = None) -> list[str]:
    """Pin the PR-light/full-event split, the docs-only skip, parallel start, merge statuses,
    row home and runners.

    ``others`` holds every other workflow file by name, for the runner rule.
    """
    findings = runner_findings({'ci.yml': ci, 'native-live.yml': native,
                                'corpus-parity.yml': parity, 'docs.yml': docs,
                                'promote-baseline.yml': promote, **(others or {})})
    findings += native_workflow_findings(native, curl_action)
    events = ci.split('\non:\n', 1)
    events = events[1].split('\npermissions:', 1)[0] if len(events) == 2 else ''
    for trigger in ('push:\n    branches: [main]',
                    'pull_request:\n    branches: [main]', 'workflow_dispatch:'):
        if '  ' + trigger not in events:
            findings.append(f'ci.yml must retain {trigger.split(":", 1)[0]}')
    jobs = _jobs(ci)
    for job in ('changes', 'preflight', *PROOF_JOBS, 'build-gate'):
        if job not in jobs:
            findings.append(f'ci.yml is missing job {job}')
    # Preflight holds the documentation guards: it runs on every change, and
    # neither it nor the classification waits for anything.
    for job in ('changes', 'preflight'):
        if _job_values(jobs.get(job, ''), 'needs') or _job_values(jobs.get(job, ''), 'if'):
            findings.append(f'{job} must start at once on every run')
    for job in PROOF_JOBS:
        if (_job_values(jobs.get(job, ''), 'needs') not in (['changes'], ['[changes]'])
                or _job_values(jobs.get(job, ''), 'if') != [DOCS_ONLY_SKIP]):
            findings.append(f'{job} must start alongside preflight, after changes alone, '
                            'and skip only a docs-only change')
    changes = jobs.get('changes', '')
    if ('      docs_only: ${{ steps.classify.outputs.docs_only }}\n' not in changes
            or CLASSIFY_STEP not in changes + '\n'):
        findings.append("changes must publish the base's scripts/ci_docs_only.py answer, exactly")
    # A job or step that continues on error reports success to the aggregate
    # however it ended.
    for name, workflow in (('ci.yml', ci), ('native-live.yml', native),
                           ('corpus-parity.yml', parity)):
        for job, body in _jobs(workflow).items():
            if any(_CONTINUE_ON_ERROR.match(line) for line in _code(body)):
                findings.append(f'{name} job {job} must not continue on error')
    gate = jobs.get('build-gate', '')
    if GATE_NEEDS not in gate or '    if: always()' not in gate or '    name: build' not in gate:
        findings.append('build-gate must aggregate every proof job, preflight and changes')
    for job, variable in (('changes', 'CHANGES_RESULT'), ('preflight', 'PREFLIGHT_RESULT'),
                          *PROOF_JOBS.items()):
        if f'{variable}: ${{{{ needs.{job}.result }}}}' not in gate:
            findings.append(f'build-gate must read the result of {job}')
    if 'DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}' not in gate:
        findings.append("build-gate must read changes' docs_only answer")
    # The shell itself is run over every outcome by scripts/test_ci_preflight.py.
    if '[[ "$PREFLIGHT_RESULT" == "success" ]]' not in gate or 'passed "$PREFLIGHT_RESULT"' in gate:
        findings.append('build-gate must require preflight success')
    for job, variable in PROOF_JOBS.items():
        if f'passed "${variable}"' not in gate:
            findings.append(f'build-gate must require {job} success or a docs-only skip')
    debug_flag = "${{ github.event_name == 'pull_request' && matrix.build_type == 'Debug' && '--exclude-label slow' || '' }}"
    pr_flag = "${{ github.event_name == 'pull_request' && '--exclude-label slow' || '' }}"
    if debug_flag not in jobs.get('build', '') or jobs.get('build', '').count('--exclude-label slow') != 1:
        findings.append('only PR Debug matrix jobs may exclude slow rows')
    if pr_flag not in jobs.get('sanitizers', '') or jobs.get('sanitizers', '').count('--exclude-label slow') != 1:
        findings.append('only PR sanitizers may exclude slow rows')
    if ('exclude_slow: ${{ github.event_name == \'pull_request\' }}' not in jobs.get('native-live', '')
            or _job_values(jobs.get('native-live', ''), 'uses') != ['./.github/workflows/native-live.yml']):
        findings.append('native-live must receive the PR-only exclusion input')
    if ('cold_cache: ${{ inputs.cold_native_cache || false }}' not in jobs.get('native-live', '')
            or '      cold_native_cache:\n' not in events
            or '        type: boolean\n        default: false' not in events):
        findings.append('CI manual native cold-cache proof must be explicit and default off')
    for job in ('kernel-only', 'corpus-parity-subset'):
        if '--exclude-label' in jobs.get(job, ''):
            findings.append(f'{job} must keep its full population')
    if ('  workflow_call:\n    inputs:\n      exclude_slow:' not in native
            or '        type: boolean\n        default: false' not in native
            or '  workflow_dispatch:' not in native
            or NATIVE_EXCLUSION not in native):
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


RUN_JSON_TESTS = 'run_json_*_test.py'


def run_json_harness_command(source: Path) -> list[str]:
    """pytest over every docker/run_json_*_test.py, found when the plan is built
    (a new one is never forgotten); -B and no cacheprovider leave the checkout
    clean. A missing pytest is an import error, so the stage fails, never
    skips. With no file found the argv names the pattern itself, which pytest
    refuses (file not found), instead of collecting the whole tree."""
    tests = sorted(str(path) for path in (source / 'docker').glob(RUN_JSON_TESTS))
    return [sys.executable, '-B', '-m', 'pytest', '-q', '-p', 'no:cacheprovider',
            '--rootdir', str(source), *(tests or [str(source / 'docker' / RUN_JSON_TESTS)])]


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
        # R5 run-failure codes: the catalog diff release.yml stamps is a tested
        # script, and the code tables of docs/pages/run-failure-codes.md are
        # generated from the catalog it diffs.
        ('run-failure-diff-tests',
         [sys.executable, str(source / 'scripts/test_gen_run_failure_catalog_diff.py')]),
        # R5 run-failure codes: docker/run_json.py's failure line, its own codes
        # held to docker/run_failure_codes.json, and the syminfo, symbol-feed
        # and diagnostics harness paths. The tests need pytest: CI installs
        # scripts/requirements-preflight.txt (pytest 9.1.1, the remote
        # verifier's), and without it the stage fails.
        ('run-json-harness-tests', run_json_harness_command(source)),
        # Lane pf-ci-docs: ci.yml skips its proof jobs on a documentation-only
        # change, and scripts/ci_docs_only.py's rule decides which that is.
        ('docs-only-tests',
         [sys.executable, str(source / 'scripts/test_ci_docs_only.py')]),
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
             if path.suffix in ('.yml', '.yaml') and path.name not in named},
            curl_action=(ROOT / '.github/actions/setup-live-curl/action.yml').read_text())
        for finding in findings:
            print(finding)
        if not findings:
            print('CI workflow contract: PR exclusions, full events, docs-only skip, statuses, '
                  'slow rows, runners and time limits OK')
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
