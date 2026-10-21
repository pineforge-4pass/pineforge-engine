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
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

from ci_verify import (ROOT, PRIVATE_FIXTURE_SKIP_LITERAL, private_fixtures_overlay,
                       source_guard_commands)
from ci_verifier_classes import verifier_class_tier

ACTIONLINT_VERSION = '1.7.12'
# The names live only in tests/CMakeLists.txt. A digest pins that measured
# population without maintaining a second row list in this Python guard.
PR_SLOW_ROWS_SHA256 = 'e8bddea797abc46ca0deb671d5d35db3491b67a82e1413d08278530d12f7b67b'
# GCP fleet routing (owner #1056): all non-release jobs use -pr on every ref;
# release, Linux x64 release artifacts and baseline promotion use isolated -main.
# CI build matrix values are JSON strings decoded to arrays, including macOS.
PR_RUNNER = '["self-hosted","pf-gcp-pr"]'
RELEASE_RUNNER = '["self-hosted","pf-gcp-main"]'
MATRIX_RUNNER = '${{ fromJSON(matrix.runner) }}'
MATRIX_LINUX_RUNNER = f"'{PR_RUNNER}'"
HOSTED_MATRIX_RUNNER = '${{ matrix.runner }}'
# The build job's whole strategy, exactly (comments and blank lines aside):
# D3 excludes only macOS Debug on PRs; every other event keeps all four legs.
# Linux x64 legs use pf-gcp-pr on every ref; macOS stays on macos-26.
BUILD_STRATEGY = ('    strategy:\n'
                  '      fail-fast: false\n'
                  '      matrix:\n'
                  '        os: [ubuntu-24.04, macos-26]\n'
                  '        build_type: [Release, Debug]\n'
                  '        exclude: ${{ fromJSON(github.event_name == \'pull_request\' && '
                  '\'[{"os":"macos-26","build_type":"Debug"}]\' || \'[]\') }}\n'
                  '        include:\n'
                  '          - os: ubuntu-24.04\n'
                  f'            runner: {MATRIX_LINUX_RUNNER}\n'
                  '          - os: macos-26\n'
                  '            runner: \'["macos-26"]\'\n')
BUILD_NAME = 'build (${{ matrix.os }}, ${{ matrix.build_type }})'
RELEASE_STRATEGY = ('    strategy:\n'
                    '      fail-fast: false\n'
                    '      matrix:\n'
                    '        include:\n'
                    '          - triple: linux-x86_64\n'
                    f'            runner: {RELEASE_RUNNER}\n'
                    '            cmake_extra: ""\n'
                    '          - triple: linux-aarch64\n'
                    '            runner: ubuntu-22.04-arm\n'
                    '            cmake_extra: ""\n'
                    '          - triple: macos-universal\n'
                    '            runner: macos-latest\n'
                    '            cmake_extra: "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"\n')
NATIVE_STRATEGY = '''    strategy:
      fail-fast: false
      matrix:
        include:
          - profile: native
            build_dir: build-live
          - profile: live-sanitizers
            build_dir: build-live-sanitizers
          - profile: live-tsan
            build_dir: build-live-tsan
'''
NATIVE_EXCLUSION = "${{ matrix.profile == 'native' && inputs.exclude_slow && '--exclude-label slow' || '' }}"
NATIVE_CACHE_SAVE = ("${{ !cancelled() && !inputs.cold_cache && steps.verify.outcome != 'skipped' "
                     "&& steps.ccache-restore.outputs.cache-hit != 'true' }}")
CURL_CACHE_SAVE = ("${{ inputs.cold-cache != 'true' && steps.curl-build.outcome == 'success' "
                   "&& steps.curl-cache.outputs.cache-hit != 'true' }}")
# Build and CTest parallelism is the core count of whichever runner took the job.
CORES = '"$(getconf _NPROCESSORS_ONLN)"'
VERIFY, PARITY, FLAT_PEAK = 'ci_verify.py', 'check_corpus_parity.sh', 'ci_flat_peak_gate.py'
PARITY_COMMAND = f'JOBS={CORES} ./scripts/{PARITY}'
# Every job with a runner in the workflows a CI run starts: its runs-on and
# the command that must carry the core count. No job or step may declare
# timeout-minutes: owner #827 removed every declaration on 2026-10-20.
# GitHub's own 6 h platform maximum is the only bound; docs/ci.md says so.
JOB_RUNNERS = {
    'ci.yml': {'authz': (PR_RUNNER, None),
               'changes': (PR_RUNNER, None),
               'preflight': (PR_RUNNER, None),
               'build': (MATRIX_RUNNER, VERIFY),
               'sanitizers': (PR_RUNNER, VERIFY),
               'kernel-only': (PR_RUNNER, VERIFY),
               'build-gate': (PR_RUNNER, None)},
    'native-live.yml': {'authz': (PR_RUNNER, None),
                         'native-live': (PR_RUNNER, VERIFY),
                         'native-live-gate': (PR_RUNNER, None)},
    'corpus-parity.yml': {'authz': (PR_RUNNER, None),
                          'corpus-parity': (PR_RUNNER, PARITY),
                          'corpus-parity-subset': (PR_RUNNER, PARITY)},
    'docs.yml': {'authz': (PR_RUNNER, None), 'build': (PR_RUNNER, None)},
    'flat-peak.yml': {'authz': (PR_RUNNER, None),
                      'flat-peak-mem': (PR_RUNNER, FLAT_PEAK),
                      'flat-peak-census': (PR_RUNNER, FLAT_PEAK)},
}
# Keep the mechanism for any future workflow with a pinned hosted exception.
HOSTED_JOB_RUNNERS = {}
# LB-435 plus addendum 2: isolate releases and the named promotion exception.
# The strategy pin keeps the existing macOS and Linux ARM hosted legs.
RELEASE_JOB_RUNNERS = {
    'release.yml': {'release': RELEASE_RUNNER, 'prebuilt': HOSTED_MATRIX_RUNNER,
                    'notify-hub': RELEASE_RUNNER},
    'promote-baseline.yml': {'promote': RELEASE_RUNNER},
}
# Credential-bearing jobs outside the release pins and gated docs build retain
# standard hosted labels. Existing hosted exceptions above are pinned separately.
STANDARD_RUNNERS = ('ubuntu-24.04', 'macos-26')
# Refuse paid labels and the old matrix key in every workflow, including the
# release matrix. Scan code so nested/flow mappings count too.
_LARGER_RUNNER_LABEL = re.compile(
    r'(?<![\w.-])(?:pf-linux-x64-16|[\w.-]+-(?:xlarge|large))(?![\w.-])')
_LARGER_RUNNER_KEY = re.compile(r'(?<![\w.-])["\']?larger_runner["\']?[ \t]*:')
# A docs-only change (scripts/ci_docs_only.py) skips the proof jobs. Each waits
# for authz and changes and skips only when changes succeeded and answered
# docs-only -- exactly the skip the build aggregate accepts -- so a failed or
# unanswered classification runs it.
PROOF_JOBS = {'build': 'BUILD_RESULT', 'sanitizers': 'SANITIZER_RESULT',
              'kernel-only': 'KERNEL_RESULT', 'native-live': 'NATIVE_RESULT',
              'corpus-parity-subset': 'PARITY_RESULT', 'flat-peak': 'FLAT_PEAK_RESULT'}
AUTHZ_SUCCESS = "${{ needs.authz.result == 'success' }}"
AUTHZ_ALWAYS = "${{ always() && needs.authz.result == 'success' }}"
DOCS_ONLY_SKIP = ("${{ always() && needs.authz.result == 'success' && (needs.changes.result != 'success' || "
                  "needs.changes.outputs.docs_only != 'true') }}")
# The complete canon step, including its env, comments and shell, byte for byte.
# PR-only ALLOWED_BOTS is dependabot[bot] and pineforge-app[bot]. No local canon
# file is needed on CI runners. Changing the script requires a new canon pin.
AUTHZ_STEP_SHA256 = '94bf3afefe4588bc33dad5fd4d8fa3d2a104cc45743ad59286c756fa3a17baa6'
# The canon header is text, not a decoded array: the fleet comment and the
# space after the comma bind every authz job byte for byte.
AUTHZ_JOB_HEADER = '''    name: authz
    # GCP fleet (owner #1050): non-release jobs on pf-gcp-pr; release-class jobs only on pf-gcp-main.
    runs-on: ["self-hosted", "pf-gcp-pr"]
    permissions: {}
'''
AUTHZ_REFUSE_STEP = '''      - name: Refuse an unauthorised run before anything else
        if: ${{ needs.authz.result != 'success' }}
        run: |
          echo "::error::authz did not pass; nothing of this run's code is checked out or run"
          exit 1
'''
# Retain the growth lane's fork exclusion in addition to canon authz.
FLAT_PEAK_SKIP = ("${{ always() && needs.authz.result == 'success' && "
                  "(github.event_name != 'pull_request' || "
                  "github.event.pull_request.head.repo.full_name == github.repository) && "
                  "(needs.changes.result != 'success' || "
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
VERIFIER_CLASSIFY_STEP = """      - name: Classify verifier self-tests
        id: verifier
        env:
          PR_HEAD_SHA: ${{ github.event.pull_request.head.sha }}
        run: |
          if git show HEAD^1:scripts/ci_verifier_paths.py > "$RUNNER_TEMP/ci_verifier_paths.py"; then
            python3 "$RUNNER_TEMP/ci_verifier_paths.py" --root . --github-output "$GITHUB_OUTPUT"
          else
            echo "verifier SLOW: the base holds no scripts/ci_verifier_paths.py"
            echo "verifier_slow=true" >> "$GITHUB_OUTPUT"
          fi
"""
PREFLIGHT_VERIFIER_SLOW = ("${{ needs.changes.result == 'success' && "
                          "needs.changes.outputs.verifier_slow || 'true' }}")
_CONTINUE_ON_ERROR = re.compile(r'\s*(?:-\s+)?["\']?continue-on-error["\']?\s*:')
GATE_NEEDS = ('needs: [authz, changes, preflight, build, sanitizers, native-live, kernel-only, '
              'corpus-parity-subset, flat-peak]')
# The matrix dependency already aggregates the selected cells. Its result must
# never be excused for the PR-only exclusion: no job was created for that cell.
# Pin the whole step so a widened passed(), a later override or a missing-result
# fallback cannot keep the required snippets while accepting a failed proof.
GATE_STEP = r'''      - name: Require every verification lane to have passed
        shell: bash
        env:
          AUTHZ_RESULT: ${{ needs.authz.result }}
          CHANGES_RESULT: ${{ needs.changes.result }}
          DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}
          PREFLIGHT_RESULT: ${{ needs.preflight.result }}
          BUILD_RESULT: ${{ needs.build.result }}
          SANITIZER_RESULT: ${{ needs.sanitizers.result }}
          NATIVE_RESULT: ${{ needs.native-live.result }}
          KERNEL_RESULT: ${{ needs.kernel-only.result }}
          PARITY_RESULT: ${{ needs.corpus-parity-subset.result }}
          FLAT_PEAK_RESULT: ${{ needs.flat-peak.result }}
        run: |
          printf '%s\n' "authz: $AUTHZ_RESULT" "changes: $CHANGES_RESULT, docs only: ${DOCS_ONLY:-unanswered}" \
            "preflight: $PREFLIGHT_RESULT" "matrix: $BUILD_RESULT" \
            "sanitizers: $SANITIZER_RESULT" "native: $NATIVE_RESULT" \
            "kernel: $KERNEL_RESULT" "corpus parity (subset): $PARITY_RESULT" \
            "flat-peak: $FLAT_PEAK_RESULT"
          # The matrix result covers only the selected cells: the PR exclusion
          # creates no skipped cell to excuse. Every existing cell must pass.
          # A proof lane passes by succeeding, or by having been skipped after
          # the changes job succeeded and answered docs-only. A lane skipped
          # for any other reason fails, and preflight must always succeed.
          docs_only=false
          if [[ "$CHANGES_RESULT" == "success" && "$DOCS_ONLY" == "true" ]]; then docs_only=true; fi
          passed() { [[ "$1" == "success" || ( "$docs_only" == "true" && "$1" == "skipped" ) ]]; }
          [[ "$AUTHZ_RESULT" == "success" && "$CHANGES_RESULT" == "success" ]] \
            && [[ "$PREFLIGHT_RESULT" == "success" ]] && passed "$BUILD_RESULT" \
            && passed "$SANITIZER_RESULT" && passed "$NATIVE_RESULT" \
            && passed "$KERNEL_RESULT" && passed "$PARITY_RESULT" \
            && passed "$FLAT_PEAK_RESULT"
'''
# No stage of this preflight carries a timeout: the verifier self-tests
# (test_ci_verify.py) drive the real literal-aware parity, receipt and
# submodule guards across parallel unittest shards (run_unittest_shards.py),
# and no bound of this file's may fail a run for slowness (owner rule,
# 2026-10-12). A stage that hangs is caught only by the job's own platform
# maximum, 360 minutes.
# A job's header -- any id GitHub accepts -- and the top-level jobs and on keys.
_JOB_HEADER = re.compile(r'^  ([A-Za-z_][A-Za-z0-9_-]*):[ \t]*(?:#.*)?$', re.MULTILINE)
_JOBS_KEY = re.compile(r'^jobs:[ \t]*(?:#.*)?$', re.MULTILINE)
_ON_KEY = re.compile(r'^(?:on|"on"|\'on\')[ \t]*:[ \t]*([^#\n]*)', re.MULTILINE)
# --jobs and every argparse abbreviation, and a verifier or growth-gate call,
# as a script or a module.
_JOBS_FLAG = re.compile(r'(?<![\w-])--j(?:o|ob|obs)?(?=[\s=]|$)')
_VERIFY_CALL = re.compile(r'\b(?:ci_verify|ci_flat_peak_gate)\.py\b|-m\s+(?:ci_verify|ci_flat_peak_gate)\b')
_STATUS_FUNCTION = re.compile(r'\b(?:always|cancelled|failure|success)\s*\(', re.IGNORECASE)


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


def _job_values(body: str, key: str, *, unquote: bool = True) -> list[str]:
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
                      if quoted and unquote else value)
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
    matters: every verifier/growth call carries one core-count --jobs before the next
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
            and (command not in (VERIFY, FLAT_PEAK) or bool(calls))
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


def _holds_credentials(workflow: str, body: str) -> bool:
    """L6: non-GITHUB_TOKEN secrets, OIDC writes or a deployment environment.

    Include inherited workflow env and permissions. Bracket/dynamic secret
    access and inherited reusable-workflow secrets must not bypass the rule.
    """
    if _job_values(body, 'environment') or _job_values(body, 'secrets') == ['inherit']:
        return True
    permissions = _job_values(workflow, 'permissions') + _job_values(body, 'permissions')
    oidc_write = r'''(?<![\w-])["']?id-token["']?\s*:\s*(?:"write"|'write'|write)(?=$|[\s,}])'''
    if any(value == 'write-all' or re.search(oidc_write, value, re.IGNORECASE)
           for value in permissions):
        return True
    text = '\n'.join(re.sub(r'\s+#.*$', '', line) for line in _code(body))
    text += '\n' + '\n'.join(_job_values(workflow, 'env'))
    for match in re.finditer(r'\bsecrets\s*(?:\.\s*([A-Za-z_]\w*)|\[([^\]]+)\])',
                             text, re.IGNORECASE):
        if match.group(1) is not None:
            if match.group(1).upper() != 'GITHUB_TOKEN':
                return True
        elif match.group(2).strip().upper() not in ("'GITHUB_TOKEN'", '"GITHUB_TOKEN"'):
            return True
    return False


def _runner_value(value: str) -> str | list[str]:
    """Compare literal runner arrays as arrays; expressions remain exact pins."""
    try:
        labels = json.loads(value)
    except ValueError:
        return value
    return labels if isinstance(labels, list) and all(isinstance(x, str) for x in labels) else value


def runner_findings(workflows: dict[str, str]) -> list[str]:
    """Pin GCP non-release/release routes and the existing hosted exceptions."""
    findings = []
    for name, workflow in workflows.items():
        code = '\n'.join(re.sub(r'\s+#.*$', '', line) for line in _code(workflow))
        if _LARGER_RUNNER_LABEL.search(code):
            findings.append(f'{name} must not use larger-runner labels; use the GCP fleet or hosted exceptions')
        if _LARGER_RUNNER_KEY.search(code):
            findings.append(f'{name} must not define a larger_runner key; use the GCP fleet or hosted exceptions')
        timed = JOB_RUNNERS.get(name, {})
        fixed_hosted = HOSTED_JOB_RUNNERS.get(name, {})
        fixed_release = RELEASE_JOB_RUNNERS.get(name, {})
        pinned = {job: values[0] for job, values in timed.items()}
        pinned.update(fixed_hosted)
        pinned.update(fixed_release)
        if (fixed_release and name != 'promote-baseline.yml'
                and _events(workflow) != {'workflow_dispatch'}):
            findings.append(f'{name} must retain workflow_dispatch only for pf-gcp-main jobs')
        if (name == 'promote-baseline.yml'
                and _events(workflow) != {'pull_request_target', 'workflow_dispatch'}):
            findings.append(f'{name} must retain pull_request_target and workflow_dispatch '
                            'only for pf-gcp-main promotion')
        if _unparsed(workflow):
            findings.append(f'{name} has jobs lines the runner contract cannot read')
        jobs = _jobs(workflow)
        findings += [f'{name} is missing job {job}' for job in pinned if job not in jobs]
        for job, body in jobs.items():
            runs_on, uses = _job_values(body, 'runs-on'), _job_values(body, 'uses')
            if any(not value.startswith('./.github/workflows/') for value in uses):
                findings.append(f'{name} job {job} calls a workflow the runner contract '
                                'cannot see; call one under ./.github/workflows/')
            credentials = _holds_credentials(workflow, body)
            if job not in pinned:
                if pinned and runs_on:
                    findings.append(f'{name} job {job} needs a pinned runner and time limit')
                if uses and not runs_on:
                    if credentials:
                        findings.append(f'{name} job {job} must prove credential-bearing '
                                        'reusable workflow calls stay hosted')
                    continue
                if len(runs_on) != 1:
                    findings.append(f'{name} job {job} has a runs-on the runner contract '
                                    'cannot read')
                    continue
            hosted_platform = (len(runs_on) == 1 and (runs_on[0] == 'macos-26' or
                               bool(re.fullmatch(r'ubuntu-(?:\d{2}\.\d{2}|latest)-arm', runs_on[0]))))
            if job in fixed_release:
                # A quoted JSON scalar does not select a YAML label array.
                if _job_values(body, 'runs-on', unquote=False) != [fixed_release[job]]:
                    findings.append(f'{name} job {job} must run on {fixed_release[job]} '
                                    '(isolated release)')
                environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                    findings.append(f'{name} job {job} must not set PF_CI_LABELS '
                                    'on an isolated release job')
            elif (job in fixed_hosted
                  or (credentials and (name, job) != ('docs.yml', 'build'))
                  or (job not in pinned and hosted_platform)):
                # Authz-gated docs deployment is pinned to the fleet below;
                # other credential jobs retain their hosted routing.
                if job in fixed_hosted:
                    hosted_ok = runs_on == [fixed_hosted[job]]
                    if not hosted_ok:
                        findings.append(f'{name} job {job} must run on {fixed_hosted[job]} (hosted)')
                else:
                    hosted_ok = (len(runs_on) == 1 and
                                 (runs_on[0] in STANDARD_RUNNERS or hosted_platform))
                if credentials and not hosted_ok:
                    findings.append(f'{name} job {job} must keep production credentials '
                                    'on a hosted standard runner')
                environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                    findings.append(f'{name} job {job} must not set PF_CI_LABELS on a hosted job')
            else:
                runner = pinned.get(job, PR_RUNNER)
                expected_runner = _runner_value(' '.join(runner.split()))
                # A quoted JSON-looking scalar is a runner label, not a YAML
                # array. Only the matrix's explicit fromJSON decodes a string.
                actual_runners = (_job_values(body, 'runs-on', unquote=False)
                                  if isinstance(expected_runner, list) else runs_on)
                if [_runner_value(value) for value in actual_runners] != [expected_runner]:
                    findings.append(f'{name} job {job} must run on {runner}')
                # Keep docs' existing ban on routing metadata after moving
                # its deployment from hosted runners to its pinned GCP job.
                if (name, job) == ('docs.yml', 'build'):
                    environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                    if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                        findings.append(f'{name} job {job} must not set PF_CI_LABELS '
                                        'on a gated docs job')
                # PF_CI_LABELS no longer supplies a routing pin: the six ci.yml,
                # two native-live.yml and two corpus-parity.yml declarations
                # still contain pre-migration light/heavy metadata at this HEAD.
                # No workflow interpolates it and no repository code consumes
                # it. The old pool's matching-metadata requirement does not
                # apply to GCP routing; runs-on and the matrix pins below do.
            if job not in timed:
                continue
            _, command = timed[job]
            if (_job_values(body, 'timeout-minutes')
                    or any(_step_values(step, 'timeout-minutes')
                           or _key_value(step.splitlines()[0].lstrip()[2:],
                                         'timeout-minutes') is not None
                           for step in _steps(body))):
                findings.append(f'{name} job {job} must declare no timeout-minutes (owner #827)')
            # Keep parallelism tied to the runner's available cores.
            if not _sized(_counted_text(body), command):
                findings.append(f'{name} job {job} must size its parallelism to the runner')
    build = _jobs(workflows.get('ci.yml', '')).get('build', '')
    if _job_block(build, 'strategy') != _code(BUILD_STRATEGY):
        findings.append('ci.yml build must use exactly the pf-gcp-pr Linux and hosted macos-26 build types, '
                        'excluding only macOS Debug on pull_request')
    if _job_values(build, 'name') != [BUILD_NAME]:
        findings.append(f'ci.yml build legs must keep their names: {BUILD_NAME}')
    if 'release.yml' in workflows:
        prebuilt = _jobs(workflows['release.yml']).get('prebuilt', '')
        if _job_block(prebuilt, 'strategy') != _code(RELEASE_STRATEGY):
            findings.append('release.yml prebuilt must use exactly the isolated Linux x64 '
                            'and hosted ARM/macOS matrix')
        # The workflow's only fan-out is notify-hub's dispatch to the release
        # hub; a rehearsal must never reach it (owner, the 1.6.2 dry cut: the
        # hub's receivers are disabled, so a real dispatch would be lost).
        notify = _jobs(workflows['release.yml']).get('notify-hub', '')
        if _job_values(notify, 'if') != ['${{ !inputs.dry_run }}']:
            findings.append('release.yml job notify-hub must carry '
                            'if: ${{ !inputs.dry_run }} so a dry run dispatches nothing')
        # The guard reads a dispatch input: a workflow that dropped or
        # retyped that declaration would not be the one this contract pins.
        dispatch = workflows['release.yml'].split('\n  workflow_dispatch:\n', 1)
        dispatch = dispatch[1].split('\npermissions:', 1)[0] if len(dispatch) == 2 else ''
        lines = dispatch.split('\n')
        start = next((index for index, line in enumerate(lines) if line == '      dry_run:'), None)
        block = []
        for follow in (lines[start + 1:] if start is not None else []):
            if follow.strip() and _indent(follow) <= 6:
                break
            block.append(follow)
        if not any(line.strip() == 'type: boolean' for line in block):
            findings.append('release.yml must declare dry_run as a boolean dispatch input')
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
    # Keep a leading uses:/run: key on unnamed steps at the other keys' depth.
    return _job_values(re.sub(r'^( *)- ', r'\1  ', step, count=1), key)


def _needs(body: str) -> list[str]:
    values = _job_values(body, 'needs')
    if len(values) != 1:
        return []
    value = values[0]
    if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_-]*', value):
        return [value]
    if re.fullmatch(r'\[\s*[A-Za-z_][A-Za-z0-9_-]*(?:\s*,\s*[A-Za-z_][A-Za-z0-9_-]*)*\s*\]', value):
        return [part.strip() for part in value[1:-1].split(',')]
    return []


def authz_workflow_findings(workflow: str, name: str, aggregate: str) -> list[str]:
    """Bind scoped workflows to the frozen canon before any repository code."""
    findings = []
    jobs = _jobs(workflow)
    authz = jobs.get('authz', '')
    steps = _steps(authz)
    step = steps[0].rstrip('\n') + '\n' if len(steps) == 1 else ''
    if (not jobs or next(iter(jobs)) != 'authz'
            or not authz.startswith('\n' + AUTHZ_JOB_HEADER)
            or _job_values(authz, 'name') != ['authz']
            or _job_values(authz, 'permissions') != ['{}']
            or any(_job_values(authz, key) for key in ('needs', 'if', 'env', 'uses', 'strategy'))
            or hashlib.sha256(step.encode()).hexdigest() != AUTHZ_STEP_SHA256):
        findings.append(f'{name} must start with the verbatim canon authz job')
    events = _events(workflow)
    if events is None or 'pull_request_target' in events:
        findings.append(f'{name} must use readable events without pull_request_target')
    pr = re.search(r'(?m)^  pull_request:[^\n]*(?:\n(?:    [^\n]*|[ \t]*))*', workflow)
    if name == 'ci.yml' and (not pr or pr[0].strip() != 'pull_request:'):
        findings.append('ci.yml must run on every pull_request without event filters')
    if _job_values(workflow, 'permissions') != ['contents: read']:
        findings.append(f'{name} must retain contents: read workflow permissions')
    for job, body in jobs.items():
        if job != 'authz' and 'authz' not in _needs(body):
            findings.append(f'{name} job {job} must need authz')
        code = '\n'.join(_code(body))
        inherited_env = '\n'.join(_job_values(workflow, 'env'))
        if (_holds_credentials(workflow, body)
                or _job_values(body, 'secrets')
                or re.search(r'\bsecrets\s*[.\[]', code + '\n' + inherited_env)
                or (job != 'authz' and re.search(r'\bgithub\s*\.\s*token\b', code))):
            findings.append(f'{name} job {job} must carry no secrets, tokens or environment')
        if job != 'authz' and _job_values(body, 'permissions') not in ([], ['contents: read']):
            findings.append(f'{name} job {job} must inherit or set only contents: read')
        steps = _steps(body)
        status_function = job != 'authz' and any(
            _STATUS_FUNCTION.search(value) for value in _job_values(body, 'if'))
        # Reusable workflow callers cannot declare steps; their job conditions
        # are pinned separately. Every inline job with a status function refuses first.
        if (status_function and not _job_values(body, 'uses')
                and (not steps or _code(steps[0]) != _code(AUTHZ_REFUSE_STEP))):
            findings.append(f'{name} job {job} with a status function must refuse authz first')
        for step in steps:
            uses = _step_values(step, 'uses')
            checkout = any(value.startswith('actions/checkout@') for value in uses)
            if checkout and '          persist-credentials: false' not in _code(step):
                findings.append(f'{name} job {job} must disable checkout credential persistence')
            # Explicit status functions bypass GitHub's implicit success() guard.
            # Repository steps need their own positive authz condition as well.
            repository_step = (checkout or any(value.startswith('./') for value in uses)
                               or any('scripts/' in value for value in _step_values(step, 'run')))
            if (status_function and repository_step
                    and _step_values(step, 'if') not in ([AUTHZ_SUCCESS], [AUTHZ_ALWAYS])):
                findings.append(f'{name} job {job} must guard repository steps on authz success')
    gate = jobs.get(aggregate, '')
    gate_steps = _steps(gate)
    if (_job_values(gate, 'if') not in (['always()'], ['${{ always() }}'])
            or len(gate_steps) != 2 or _code(gate_steps[0]) != _code(AUTHZ_REFUSE_STEP)
            or _step_values(gate_steps[-1], 'if')):
        findings.append(f'{name} {aggregate} must always run and refuse authz first')
    return findings


# Corpus needs persisted checkout credentials for its HTTPS submodule clone,
# and docs deploys with Cloudflare secrets: the full authz contract forbids both.
def gated_workflow_findings(workflow: str, name: str) -> list[str]:
    """Pin canon authz and its fleet-only dependants without those restrictions."""
    findings = []
    jobs = _jobs(workflow)
    authz = jobs.get('authz', '')
    steps = _steps(authz)
    step = steps[0].rstrip('\n') + '\n' if len(steps) == 1 else ''
    if (not jobs or _unparsed(workflow) or next(iter(jobs)) != 'authz'
            or not authz.startswith('\n' + AUTHZ_JOB_HEADER)
            # Extra job keys such as continue-on-error can bypass refusal
            # without changing either the header or the single step's hash.
            or _code(authz) != _code(AUTHZ_JOB_HEADER) + ['    steps:'] + _code(step)
            or _job_values(authz, 'name') != ['authz']
            or _job_values(authz, 'permissions') != ['{}']
            or any(_job_values(authz, key) for key in ('needs', 'if', 'env', 'uses', 'strategy'))
            or hashlib.sha256(step.encode()).hexdigest() != AUTHZ_STEP_SHA256):
        findings.append(f'{name} must start with the verbatim canon authz job')
    events = _events(workflow)
    if events is None or 'pull_request_target' in events:
        findings.append(f'{name} must use readable events without pull_request_target')
    for job, body in jobs.items():
        if job != 'authz' and 'authz' not in _needs(body):
            findings.append(f'{name} job {job} must need authz')
        # Literal arrays may differ in whitespace; quoted JSON scalars and
        # expressions cannot stand in for the fleet's two scheduler labels.
        runners = _job_values(body, 'runs-on', unquote=False)
        if [_runner_value(value) for value in runners] != [_runner_value(PR_RUNNER)]:
            findings.append(f'{name} job {job} must run on {PR_RUNNER}')
    return findings


def native_workflow_findings(native: str, curl_action: str | None = None) -> list[str]:
    findings = authz_workflow_findings(native, 'native-live.yml', 'native-live-gate')
    jobs = _jobs(native)
    job, gate = jobs.get('native-live', ''), jobs.get('native-live-gate', '')
    if (_job_block(job, 'strategy') != _code(NATIVE_STRATEGY)
            or _needs(job) != ['authz'] or _job_values(job, 'if')
            or _job_values(job, 'name') != ['native-live (${{ matrix.profile }})']):
        findings.append('native-live must run all three independent profiles with fail-fast false, '
                        'the platform job maximum and no per-run budget')
    if (set(jobs) != {'authz', 'native-live', 'native-live-gate'}
            or _needs(gate) != ['authz', 'native-live']
            or _job_values(gate, 'if') != ['always()']
            or '          AUTHZ_RESULT: ${{ needs.authz.result }}' not in _code(gate)
            or '          test "$AUTHZ_RESULT" = success' not in _code(gate)
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
                '--ccache '
                "${{ matrix.profile == 'native' && '--require-websocket' || '' }} " + NATIVE_EXCLUSION)
    if (len(command) != 1 or command[0].replace('--jobs=', '--jobs ') != expected
            or _step_values(verify, 'if')
            or _step_values(verify, 'timeout-minutes')
            or job.count('--exclude-label') != 1 or job.count('scripts/ci_verify.py ') != 1):
        findings.append('native-live must verify each full profile with no step deadline or timeout flag and '
                        'native-only PR exclusion')
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
    findings += authz_workflow_findings(ci, 'ci.yml', 'build-gate')
    findings += native_workflow_findings(native, curl_action)
    findings += gated_workflow_findings(parity, 'corpus-parity.yml')
    findings += gated_workflow_findings(docs, 'docs.yml')
    if 'flat-peak.yml' in (others or {}):
        findings += gated_workflow_findings(others['flat-peak.yml'], 'flat-peak.yml')
    events = ci.split('\non:\n', 1)
    events = events[1].split('\npermissions:', 1)[0] if len(events) == 2 else ''
    for trigger in ('push:\n    branches: [main]', 'pull_request:', 'workflow_dispatch:'):
        if '  ' + trigger not in events:
            findings.append(f'ci.yml must retain {trigger.split(":", 1)[0]}')
    jobs = _jobs(ci)
    for job in ('authz', 'changes', 'preflight', *PROOF_JOBS, 'build-gate'):
        if job not in jobs:
            findings.append(f'ci.yml is missing job {job}')
    # All repository work follows authz. Classification and preflight remain
    # separate: failed classification still runs proof, and fails the aggregate.
    if (_needs(jobs.get('changes', '')) != ['authz']
            or _job_values(jobs.get('changes', ''), 'if')):
        findings.append('changes must start after authz on every run')
    preflight = jobs.get('preflight', '')
    if (set(_needs(preflight)) != {'authz', 'changes'} or len(_needs(preflight)) != 2
            or _job_values(preflight, 'if') != [AUTHZ_ALWAYS]):
        findings.append('preflight must follow authz and changes and run even if classification fails')
    if (f'      PINEFORGE_VERIFIER_SLOW: {PREFLIGHT_VERIFIER_SLOW}\n' not in preflight
            or preflight.count('PINEFORGE_VERIFIER_SLOW:') != 1):
        findings.append('preflight must consume verifier_slow and default to all classes')
    verifier_step = _step_named(_steps(preflight), '      - name: Verify workflow wiring and source guards')
    if _step_values(verifier_step, 'run') != [
            'python3 scripts/ci_preflight.py --output-dir build-ci-preflight']:
        findings.append('preflight must invoke its unfiltered event-aware stage plan')
    for job in PROOF_JOBS:
        condition = FLAT_PEAK_SKIP if job == 'flat-peak' else DOCS_ONLY_SKIP
        if (set(_needs(jobs.get(job, ''))) != {'authz', 'changes'}
                or len(_needs(jobs.get(job, ''))) != 2
                or _job_values(jobs.get(job, ''), 'if') != [condition]):
            findings.append(f'{job} must start alongside preflight, after authz and changes, '
                            + ('and preserve its docs-only/fork condition' if job == 'flat-peak'
                               else 'and skip only a docs-only change'))
    if _job_values(jobs.get('flat-peak', ''), 'uses') != ['./.github/workflows/flat-peak.yml']:
        findings.append('flat-peak must call ./.github/workflows/flat-peak.yml')
    changes = jobs.get('changes', '')
    if ('      docs_only: ${{ steps.classify.outputs.docs_only }}\n' not in changes
            or CLASSIFY_STEP not in changes + '\n'):
        findings.append("changes must publish the base's scripts/ci_docs_only.py answer, exactly")
    if ('      verifier_slow: ${{ steps.verifier.outputs.verifier_slow }}\n' not in changes
            or VERIFIER_CLASSIFY_STEP not in changes + '\n'):
        findings.append("changes must publish the base's scripts/ci_verifier_paths.py answer, exactly")
    # A job or step that continues on error reports success to the aggregate
    # however it ended.
    for name, workflow in (('ci.yml', ci), ('native-live.yml', native),
                           ('corpus-parity.yml', parity),
                           ('flat-peak.yml', (others or {}).get('flat-peak.yml', ''))):
        for job, body in _jobs(workflow).items():
            if any(_CONTINUE_ON_ERROR.match(line) for line in _code(body)):
                findings.append(f'{name} job {job} must not continue on error')
    gate = jobs.get('build-gate', '')
    if GATE_NEEDS not in gate or '    if: always()' not in gate or '    name: build' not in gate:
        findings.append('build-gate must aggregate authz, every proof job, preflight and changes')
    for job, variable in (('authz', 'AUTHZ_RESULT'), ('changes', 'CHANGES_RESULT'), ('preflight', 'PREFLIGHT_RESULT'),
                          *PROOF_JOBS.items()):
        if f'{variable}: ${{{{ needs.{job}.result }}}}' not in gate:
            findings.append(f'build-gate must read the result of {job}')
    if 'DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}' not in gate:
        findings.append("build-gate must read changes' docs_only answer")
    if '[[ "$AUTHZ_RESULT" == "success" && "$CHANGES_RESULT" == "success" ]]' not in gate:
        findings.append('build-gate must require authz and changes success')
    # The shell itself is run over every outcome by scripts/test_ci_preflight.py.
    if '[[ "$PREFLIGHT_RESULT" == "success" ]]' not in gate or 'passed "$PREFLIGHT_RESULT"' in gate:
        findings.append('build-gate must require preflight success')
    for job, variable in PROOF_JOBS.items():
        if f'passed "${variable}"' not in gate:
            findings.append(f'build-gate must require {job} success or a docs-only skip')
    gate_steps = _steps(gate)
    if not gate_steps or _code(gate_steps[-1]) != _code(GATE_STEP):
        findings.append('build-gate must keep the exact result policy: success or a classified docs-only skip')
    pr_flag = "${{ github.event_name == 'pull_request' && '--exclude-label slow' || '' }}"
    # D1 reverses the full-on-PR Release/kernel rule. Each allowed job has
    # exactly one PR-only flag; every other event keeps the full population.
    for job in ('build', 'kernel-only', 'sanitizers'):
        original = jobs.get(job, '')
        body = '\n'.join(_code(original))
        verify_steps = [step for step in _steps(original)
                        if _VERIFY_CALL.search(_counted_text(step))]
        if (body.count(pr_flag) != 1 or body.count('--exclude-label') != 1
                or len(verify_steps) != 1 or pr_flag not in _counted_text(verify_steps[0])):
            findings.append(f'{job} must exclude exactly slow on PRs only, once')
    for job, body in jobs.items():
        if job not in ('build', 'kernel-only', 'sanitizers') and '--exclude-label' in body:
            findings.append(f'{job} must keep its full population')
    if ci.count('--exclude-label') != 3:
        findings.append('CI must carry exactly three PR-only slow exclusion flags')
    if ('exclude_slow: ${{ github.event_name == \'pull_request\' }}' not in jobs.get('native-live', '')
            or _job_values(jobs.get('native-live', ''), 'uses') != ['./.github/workflows/native-live.yml']):
        findings.append('native-live must receive the PR-only exclusion input')
    if ('cold_cache: ${{ inputs.cold_native_cache || false }}' not in jobs.get('native-live', '')
            or '      cold_native_cache:\n' not in events
            or '        type: boolean\n        default: false' not in events):
        findings.append('CI manual native cold-cache proof must be explicit and default off')
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


_LITERAL_RUNNER_LABEL = r'''(?:"[^"\n]+"|'[^'\n]+'|[A-Za-z0-9_.-]+)'''


def actionlint_runner_label_findings(source: Path) -> list[str]:
    """Check the repo's simple block-list config without a YAML dependency.

    Unsupported config shapes fail closed; actionlint in workflow-lint also
    parses this same required file with its full YAML/schema parser.
    """
    config_name = '.github/actionlint.yaml'
    try:
        config = (source / config_name).read_text()
    except (OSError, UnicodeError) as error:
        return [f'{config_name}: cannot read config: {error}']
    lines = [re.sub(r'\s+#.*$', '', line).rstrip() for line in _code(config)]
    entries = [re.fullmatch(r'    - (' + _LITERAL_RUNNER_LABEL + r')', line)
               for line in lines[2:]]
    if (lines[:2] != ['self-hosted-runner:', '  labels:']
            or not entries or not all(entries)):
        return [f'{config_name}: cannot parse config: expected self-hosted-runner.labels '
                'as a block list of literal labels']
    declared = {entry.group(1).strip('\'"') for entry in entries}
    findings = []
    for path in sorted((source / '.github/workflows').glob('*')):
        if path.suffix not in ('.yml', '.yaml'):
            continue
        workflow = path.read_text()
        if _unparsed(workflow):
            findings.append(f'{path.name}: cannot parse jobs for literal runner labels')
            continue
        for job, body in _jobs(workflow).items():
            for value in _job_values(body, 'runs-on'):
                if not value.startswith('['):
                    continue
                if not re.fullmatch(r'\[\s*' + _LITERAL_RUNNER_LABEL
                                    + r'(?:\s*,\s*' + _LITERAL_RUNNER_LABEL
                                    + r')*\s*,?\s*\]', value):
                    findings.append(f'{path.name} job {job}: cannot parse literal runs-on array')
                    continue
                labels = {token.strip('\'"') for token in re.findall(_LITERAL_RUNNER_LABEL, value)
                          if '${{' not in token}
                if 'self-hosted' in labels:
                    for label in sorted(labels - declared):
                        findings.append(f'{path.name} job {job}: label "{label}" is not declared '
                                        f'in {config_name}')
    return findings


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


def check_commands(source: Path, output: Path | None = None,
                   *, env: dict[str, str] | None = None) -> list[tuple]:
    output_dir = (source / 'build-ci-preflight') if output is None else output
    workflows = sorted(str(path) for path in (source / '.github/workflows').glob('*')
                       if path.suffix in ('.yml', '.yaml'))
    class_args = (['--class-tier', 'fast']
                  if verifier_class_tier(os.environ if env is None else env) == 'fast'
                  else [])
    return [
        ('shellcheck-version', ['shellcheck', '--version']),
        ('actionlint-runner-labels',
         [sys.executable, str(source / 'scripts/ci_preflight.py'),
          '--check-actionlint-runner-labels']),
        # Require the config explicitly: absent/malformed config and undeclared
        # literal self-hosted labels are actionlint findings, including in new
        # workflows. A missing workflows directory must not lint another tree.
        ('workflow-lint', ['actionlint', '-color',
                           '-config-file', str(source / '.github/actionlint.yaml'),
                           *(workflows or [str(source / '.github/workflows/*.yml')])]),
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
        ('verifier-tests',
         [sys.executable, str(source / 'scripts/run_unittest_shards.py'),
          str(source / 'scripts/test_ci_verify.py'),
          '--log-dir', str(output_dir / 'verifier-tests-shards'), *class_args]),
        ('preflight-tests', [sys.executable, str(source / 'scripts/test_ci_preflight.py')]),
        ('unittest-shards-tests',
         [sys.executable, str(source / 'scripts/test_run_unittest_shards.py')]),
        ('verifier-paths-tests',
         [sys.executable, str(source / 'scripts/test_ci_verifier_paths.py')]),
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
        # EN-132: the checker itself comes from source_guard_commands above;
        # its negative controls and independent D4 route need no engine.
        ('selected-window-ownership-tests',
         [sys.executable, '-I', '-B', str(source / 'tests/ownership/test_manifest_checker.py')]),
        # Recorded outputs: the keys, order and value rules of run_json's
        # "outputs" block, with no engine (docs/outputs.md).
        ('report-outputs-key-tests',
         [sys.executable, str(source / 'scripts/test_report_outputs_keys.py')]),
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
        # CR-150 flat-peak memory gate (docs/ci.md): the driver's own rules -- zero
        # rows, a skipped row, an absent or RED report each fail -- and the static
        # wiring of ci.yml, release.yml, flat-peak.yml and the mem labels.
        ('flat-peak-gate-tests',
         [sys.executable, str(source / 'scripts/test_ci_flat_peak_gate.py')]),
        ('flat-peak-wiring',
         [sys.executable, str(source / 'scripts/ci_flat_peak_gate.py'), 'check-wiring']),
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


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def run_checks(commands: list[tuple], output: Path, *, source: Path = ROOT) -> int:
    """Run every stage, log it, and answer 0 only when every binding one passed.

    A stage may be ``(name, argv)`` or ``(name, argv, advisory)``. An advisory
    stage still runs and still prints its findings, but its failure is recorded
    as ``reported`` and does not fail the preflight.
    """
    output.mkdir(parents=True, exist_ok=True)
    run_t0 = time.monotonic()
    summary = {'schemaVersion': 'pineforge-ci-preflight/v1', 'status': 'incomplete',
               'scope': 'workflow and source checks only; full verification still required',
               'startedAt': _utc_now(), 'stages': []}
    summary_path = output / 'preflight-summary.json'

    def record() -> None:
        summary_path.write_text(json.dumps(summary, indent=2) + '\n')

    record()
    for entry in commands:
        name, argv = entry[0], entry[1]
        advisory = bool(entry[2]) if len(entry) > 2 else False
        print(f'ci_preflight: {name}: {shlex.join(argv)}'
              + (' [advisory]' if advisory else ''), flush=True)
        stage_t0 = time.monotonic()
        stage = {'name': name, 'argv': argv, 'status': 'running', 'exitCode': None,
                 'advisory': advisory, 'log': name + '.log',
                 'startedAt': _utc_now(), 'seconds': None}
        summary['stages'].append(stage)
        record()
        try:
            result = subprocess.run(argv, cwd=source, capture_output=True)
            code, log = result.returncode, result.stdout + result.stderr
        except OSError as error:
            code, log = 127, str(error).encode()
        stage['seconds'] = round(time.monotonic() - stage_t0, 3)
        (output / stage['log']).write_bytes(log)
        if log:
            print(log.decode('utf-8', 'replace'), end='', flush=True)
        if (code == 77 and not advisory and not private_fixtures_overlay()
                and PRIVATE_FIXTURE_SKIP_LITERAL.encode() in log):
            # A source guard whose private fixture input the tree lacks exits
            # 77 with the pinned literal naming the missing path.  The literal
            # is required: a bare 77 without it stays a failure -- and so does
            # the literal itself while PINEFORGE_PRIVATE_FIXTURES is set, when
            # the input the guard names is in the tree (R2-F3).
            print(f'ci_preflight: {name}: skipped (exit 77, private fixtures absent)',
                  flush=True)
            stage.update(status='skipped', exitCode=code)
        elif code and advisory:
            print(f'ci_preflight: {name}: exit {code}, reported only '
                  '(this stage is declared advisory)', flush=True)
            stage.update(status='reported', exitCode=code)
        else:
            stage.update(status='passed' if code == 0 else 'failed', exitCode=code)
        record()
    passed = bool(commands) and all(stage['exitCode'] == 0 or stage['advisory']
                                    or stage['status'] == 'skipped'
                                    for stage in summary['stages'])
    summary.update(status='passed' if passed else 'failed', exitCode=0 if passed else 1,
                   seconds=round(time.monotonic() - run_t0, 3))
    record()
    return summary['exitCode']


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build-ci-preflight')
    parser.add_argument('--check-docs-workflow', action='store_true')
    parser.add_argument('--self-test-docs-workflow', action='store_true')
    parser.add_argument('--check-ci-workflow', action='store_true')
    parser.add_argument('--check-actionlint-runner-labels', action='store_true')
    args = parser.parse_args()
    if args.check_actionlint_runner_labels:
        findings = actionlint_runner_label_findings(ROOT)
        for finding in findings:
            print(finding)
        if not findings:
            print('actionlint runner labels: all literal self-hosted labels declared')
        return 1 if findings else 0
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
    output_dir = args.output_dir.resolve()
    return run_checks([('actionlint-version', version_check),
                       *check_commands(ROOT, output_dir)],
                      output_dir)


if __name__ == '__main__':
    raise SystemExit(main())
