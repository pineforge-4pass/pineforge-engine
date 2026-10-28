#!/usr/bin/env python3
"""Exercise preflight failure propagation with real child processes."""
import argparse
import contextlib
import io
import inspect
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
                          DOCS_ONLY_SKIP, GITHUB_ONLY_SKIP, HEAVY_SKIP, HOSTED_MATRIX_RUNNER, JOB_COMMANDS,
                          PR_RUNNER, POST_RUNNER, MATRIX_LINUX_RUNNER, MATRIX_RUNNER,
                          NATIVE_EXCLUSION,
                          PREFLIGHT_IF, PROOF_JOBS, PR_PROOF_JOBS, RELEASE_RUNNER, _jobs, check_commands,
                          ADMIT_MAX_DEPTH, ADMIT_MAX_FILE_BYTES, ADMIT_MAX_LINE, ADMIT_MAX_LINES,
                          ADMIT_MAX_TOTAL_BYTES, admit_rule_findings, admit_shape_findings,
                          admit_yaml_findings, gke_admit_workflow_findings,
                          ci_workflow_findings, gated_workflow_findings,
                          native_workflow_findings, run_checks)

ROOT = Path(__file__).resolve().parents[1]
GKE_32 = 'gke-32cpu-64gb-amd64'
GKE_16 = 'gke-16cpu-32gb-amd64'
GKE_8 = 'gke-8cpu-16gb-amd64'
GKE_ARM = 'gke-16cpu-32gb-arm64'
GKE_PR_32 = 'gke-32cpu-64gb-amd64'
GKE_PR_16 = 'gke-16cpu-32gb-amd64'
GKE_PR_8 = 'gke-8cpu-16gb-amd64'
GKE_2 = 'gke-2cpu-4gb-amd64'
GKE_4 = 'gke-4cpu-8gb-amd64'
EXPECTED_LIGHT_GKE_JOBS = {
    'ci-post.yml': dict.fromkeys(
        ('authz', 'changes', 'build-release-gate', 'build-debug-gate',
         'sanitizers-gate', 'kernel-only-gate', 'build-gate'), GKE_2),
    'native-live.yml': dict.fromkeys(
        ('authz', 'native-live-native', 'native-live-gate'), GKE_4),
    'corpus-parity.yml': {'authz': GKE_4},
    'docs-post.yml': {'authz': GKE_2},
    'release-ruleset-probe.yml': {'authz': GKE_2},
    'promote-baseline.yml': {'promote-guard': GKE_2},
}
EXPECTED_GKE_JOBS = {
    'gke-pr.yml': {'preflight': GKE_PR_8, 'build': GKE_PR_32, 'build-debug': GKE_PR_16,
                   'corpus-parity-subset': GKE_PR_8},
    'ci-post.yml': {'preflight': GKE_8, 'preflight-slow': GKE_8,
                    'build': GKE_32, 'build-debug': GKE_16,
                    'sanitizers': GKE_32, 'kernel-only': GKE_16, 'build-arm': GKE_ARM},
    'corpus-parity.yml': {'corpus-parity': GKE_16, 'corpus-parity-subset': GKE_8},
    'native-live.yml': {'native-live': GKE_16, 'native-live-shards': GKE_8},
}
PR_SLOW_FLAG = "${{ github.event_name == 'pull_request' && '--exclude-label slow' || '' }}"
EXPECTED_PR_SLOW_ROWS = (
    'test_adapter_live_state_equivalence',
    'test_adapter_live_state_scaling',
    'test_adapter_reissue_binding',
    'test_aggregate_cpp_versions',
    'test_broker_state_hash_coverage_mutations',
    'test_chart_day_memo',
    'test_ci_verify_slice_1',
    'test_ci_verify_slice_2',
    'test_ci_verify_slice_3',
    'test_ci_verify_slice_4',
    'test_ci_verify_slice_5',
    'test_ci_verify_slice_6',
    'test_ci_verify_slice_7',
    'test_ci_verify_slice_8',
    'test_l10recon1_merge_fill_liveness',
    'test_local_time_fields',
    'test_magnifier_endpoints4',
    'test_native_bar_open_hook',
    'test_native_command_after_slice_1',
    'test_native_command_after_slice_2',
    'test_native_command_after_slice_3',
    'test_native_command_after_slice_4',
    'test_native_command_after_slice_5',
    'test_native_command_after_slice_6',
    'test_native_command_after_slice_7',
    'test_native_command_after_slice_8',
    'test_native_cpp_abi',
    'test_native_cpp_versions',
    'test_native_definition_index',
    'test_native_direct_mutation',
    'test_native_event_retention',
    'test_native_group_absorption',
    'test_native_group_keep_handle',
    'test_native_handle_stable_replace',
    'test_native_host_reads',
    'test_native_lookup_memos',
    'test_native_match_band_precheck',
    'test_native_match_row_reuse',
    'test_native_precommit_hook',
    'test_native_quiet_point',
    'test_native_runtime_ambient',
    'test_native_script_bucket_completions',
    'test_native_settlement_carry',
)
PR_SLOW_ROWS_FINDING = 'measured slow rows must keep one pinned CMake label list'
KERNEL_VERIFY = ('run: python3 scripts/ci_verify.py kernel --build-dir build-kernel '
                 '--jobs "$(getconf _NPROCESSORS_ONLN)" --ccache '
                 '--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" '
                 '--ctest-shard ${{ matrix.shard }}/2')
POST_BUILD_VERIFY = (
    'run: |\n          python3 scripts/ci_verify.py '
    "${{ matrix.build_type == 'Release' && 'release' || 'debug' }} "
    '--build-dir build --jobs "$(getconf _NPROCESSORS_ONLN)" --ccache')
SHARDED_VERIFY_RUNS = (
    (6, 'build', POST_BUILD_VERIFY + ' --ctest-shard ${{ matrix.shard }}/4'),
    (6, 'build-debug', POST_BUILD_VERIFY + ' --ctest-shard ${{ matrix.shard }}/2'),
    (6, 'sanitizers', 'run: python3 scripts/ci_verify.py sanitizers --build-dir build-asan '
     '--jobs "$(getconf _NPROCESSORS_ONLN)" --ccache --ctest-shard ${{ matrix.shard }}/4'),
    (6, 'kernel-only', KERNEL_VERIFY),
    (1, 'native-live-shards',
     'run: python3 scripts/ci_verify.py ${{ matrix.profile }} --build-dir ${{ matrix.build_dir }} '
     '--jobs "$(getconf _NPROCESSORS_ONLN)" --generator Ninja '
     '--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" --ccache --require-websocket '
     + NATIVE_EXCLUSION + ' --ctest-shard ${{ matrix.shard }}/4'),
)
# ci_workflow_findings' arguments, in order.
CI_SOURCES = ('.github/workflows/ci.yml', '.github/workflows/native-live.yml',
              '.github/workflows/promote-baseline.yml', 'tests/CMakeLists.txt',
              '.github/workflows/corpus-parity.yml', '.github/workflows/docs.yml',
              '.github/workflows/ci-post.yml', '.github/workflows/docs-post.yml')
POST_BUILD_RUNNER_FINDING = 'ci-post.yml build must select only Ubuntu Release shards on full events'
POST_DEBUG_BUILD_RUNNER_FINDING = 'ci-post.yml build-debug must select only Ubuntu Debug shards on full events'
POST_ARM_BUILD_RUNNER_FINDING = ('ci-post.yml build-arm must select '
                                'only Linux arm64 Release/Debug cells on full events')
RELEASE_MATRIX_FINDING = ('release.yml prebuilt must use exactly the isolated Linux x64 '
                          'and hosted ARM matrix')
CI_JOB_IDS = ('authz',)
GKE_PR_JOB_IDS = ('admit', 'preflight', 'build', 'build-debug',
                  'corpus-parity-subset', 'legs', 'result')
GKE_PROOF_IF = ("${{ needs.admit.outputs.allowed == 'true' && "
                "needs.admit.outputs.docs_only != 'true' && "
                "needs.admit.outputs.github_only != 'true' }}")
POST_CI_JOB_IDS = ('authz', 'changes', 'preflight', 'preflight-slow', 'build',
                  'build-debug', 'build-release-gate', 'build-debug-gate',
                  'sanitizers-gate', 'kernel-only-gate',
                  'build-arm', 'sanitizers', 'kernel-only', 'native-live', 'corpus-parity-subset',
                  'corpus-parity', 'build-gate')
NATIVE_HOSTED_RUNNERS = {'authz': GKE_4, 'native-live': GKE_16,
                         'native-live-shards': GKE_8,
                         'native-live-native': GKE_4, 'native-live-gate': GKE_4}
FLEET_JOB_IDS = {
    'ci.yml': ('authz',),
    'gke-pr.yml': GKE_PR_JOB_IDS,
    'ci-post.yml': ('authz', 'changes', 'preflight', 'preflight-slow', 'build',
                    'build-debug', 'build-arm', 'sanitizers', 'kernel-only', 'build-gate',
                    'build-release-gate', 'build-debug-gate', 'sanitizers-gate', 'kernel-only-gate'),
    'native-live.yml': ('authz', 'native-live', 'native-live-shards', 'native-live-native', 'native-live-gate'),
    'corpus-parity.yml': ('authz', 'corpus-parity', 'corpus-parity-subset'),
    'docs.yml': ('authz', 'build'),
    'docs-post.yml': ('authz', 'build'),
    'release.yml': ('release', 'prebuilt', 'notify-hub'),
    'release-ruleset-probe.yml': ('authz', 'probe'),
    'promote-baseline.yml': ('promote',),
}
LIGHT_JOB_IDS = {
    'ci.yml': ('authz',),
    'gke-pr.yml': ('admit', 'legs', 'result'),
    'ci-post.yml': ('authz', 'changes', 'build-gate', 'build-release-gate',
                    'build-debug-gate', 'sanitizers-gate', 'kernel-only-gate'),
    'corpus-parity.yml': ('authz',),
    'docs.yml': ('authz', 'build'),
    'docs-post.yml': ('authz',),
    'native-live.yml': ('authz', 'native-live-native', 'native-live-gate'),
    'release-ruleset-probe.yml': ('authz',),
}
HOSTED_SHARD_JOB_LABELS = {
    'build': GKE_32, 'build-debug': GKE_16,
    'sanitizers': GKE_32, 'kernel-only': GKE_16,
}
HEAVY_CI_JOBS = ('preflight-slow', 'sanitizers', 'kernel-only', 'native-live', 'corpus-parity')
PREFLIGHT_RUN = 'python3 scripts/ci_preflight.py --output-dir build-ci-preflight --tier fast'
PROMOTION_AUTHORIZATION_STEP = 'Only our own merged pull requests and dispatches promote the baseline'
RULESETS_PROBE_FINDING = (
    'release-ruleset-probe.yml must keep the dispatch-only read-only probe '
    'with the same release App token')
RULESETS_PROBE_REF_FINDING = (
    'release-ruleset-probe.yml probe must refuse a dispatch ref other than '
    'refs/heads/main as its first step before minting the App token or checkout')
RULESETS_PROBE_WORKFLOW = '''name: Release ruleset probe

on:
  workflow_dispatch:

permissions:
  contents: read

jobs:
  authz:
    name: authz
    # Preflight pins executable workflow lines after removing comment-only lines.
    # authz uses gke-2cpu-4gb-amd64; the App-token probe uses pf-gcp-post.
    runs-on: gke-2cpu-4gb-amd64
    permissions: {}
    # No timeout-minutes: the owner's no-time-limits rule (check-no-time-limits refuses new ones).
    steps:
      - name: Only our own pull requests and our own events run CI
        env:
          EVENT: ${{ github.event_name }}
          REPO: ${{ github.repository }}
          HEAD_REPO: ${{ github.event.pull_request.head.repo.full_name }}
          HEAD_IS_FORK: ${{ github.event.pull_request.head.repo.fork }}
          ASSOCIATION: ${{ github.event.pull_request.author_association }}
          PR_AUTHOR: ${{ github.event.pull_request.user.login }}
          WR_HEAD_REPO: ${{ github.event.workflow_run.head_repository.full_name }}
          WR_ACTOR: ${{ github.event.workflow_run.triggering_actor.login }}
          GH_TOKEN: ${{ github.token }}   # read-only use: one permission lookup for workflow_run
          # Bot logins whose same-repository pull requests count as ours: dependabot[bot] (CI-DEPBOT A)
          # and the org's own GitHub App bot logins by name (space-separated), nothing else.
          ALLOWED_BOTS: "dependabot[bot] pineforge-app[bot]"
        run: |
          set -eu
          fail() { echo "::error title=authz::$1"; echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }
          case "$EVENT" in
            push|workflow_dispatch|schedule|merge_group|release|workflow_call|repository_dispatch)
              # Only someone with write access to this repository (or our App's token) can cause these
              # events; none of them runs pull-request code.
              ;;
            workflow_run)
              # LB-437: bind identity to the triggering run and its re-runner.
              # These two App bots are fixed canon policy; ALLOWED_BOTS is PR-only.
              [ -n "${WR_HEAD_REPO:-}" ] || fail "workflow_run head repository is missing"
              [ -n "${WR_ACTOR:-}" ] || fail "workflow_run triggering actor is missing"
              [ "$WR_HEAD_REPO" = "$REPO" ] || fail "workflow_run head repository does not match this repository"
              case "$WR_ACTOR" in
                "pineforge-app[bot]"|"pineforge-ci-dispatcher[bot]") ;;
                *"[bot]") fail "workflow_run bot is not on the canon allowlist" ;;
                *)
                  perm=$(gh api "repos/$REPO/collaborators/$WR_ACTOR/permission" --jq .permission 2>/dev/null) || fail "workflow_run permission lookup failed"
                  case "$perm" in admin|maintain|write) ;; *) fail "workflow_run triggering actor does not have write permission" ;; esac ;;
              esac ;;
            pull_request)
              [ "$HEAD_REPO" = "$REPO" ] || fail "pull request from another repository (${HEAD_REPO:-unknown})"
              [ "$HEAD_IS_FORK" = "false" ] || fail "pull request from a fork"
              case "$ASSOCIATION" in
                OWNER|MEMBER|COLLABORATOR) ;;
                *)
                  case " $ALLOWED_BOTS " in
                    *" $PR_AUTHOR "*) ;;
                    *)
                      # A member whose org membership is private shows as CONTRIBUTOR: ask GitHub for the
                      # author's permission on this repository; anything but admin/maintain/write (or a
                      # failed call) refuses.
                      perm=$(gh api "repos/$REPO/collaborators/$PR_AUTHOR/permission" --jq .permission 2>/dev/null || echo none)
                      case "$perm" in admin|maintain|write) ;; *) fail "author ${PR_AUTHOR:-unknown} (${ASSOCIATION:-NONE}) has permission '$perm', not write, and is not an allowlisted bot" ;; esac ;;
                  esac ;;
              esac ;;
            *) fail "event $EVENT does not run CI (pull_request_target and other events are not allowed)" ;;
          esac
          echo "authz: allowed ($EVENT${PR_AUTHOR:+ by $PR_AUTHOR})" >> "$GITHUB_STEP_SUMMARY"

  probe:
    needs: [authz]
    runs-on: ["self-hosted","pf-gcp-post"]
    environment: release
    steps:
      - name: Refuse a dispatch ref other than main
        env:
          REF: ${{ github.ref }}
        run: |
          set -euo pipefail
          fail() { echo "::error title=rulesets-probe::$1"; echo "rulesets-probe: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }
          [ "$REF" = refs/heads/main ] || fail "release ruleset probe requires refs/heads/main"

      - name: Mint org App token
        id: app
        uses: actions/create-github-app-token@d72941d797fd3113feb6b93fd0dec494b13a2547 # v1.12.0
        with:
          app-id: ${{ secrets.PINEFORGE_APP_ID }}
          private-key: ${{ secrets.PINEFORGE_APP_PRIVATE_KEY }}
          owner: ${{ github.repository_owner }}
          repositories: pineforge-engine

      - name: Checkout
        if: ${{ needs.authz.result == 'success' }}
        uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0
        with:
          persist-credentials: false

      - name: Probe release ruleset barrier
        if: ${{ needs.authz.result == 'success' }}
        env:
          GH_TOKEN: ${{ steps.app.outputs.token }}
        run: python3 scripts/ci_release_gate.py rulesets-probe --repo "${GITHUB_REPOSITORY}"
'''


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


def ready_pr(condition):
    """Keep the draft guard when mutating another PR job condition."""
    if not condition.startswith('${{ ') or not condition.endswith(' }}'):
        raise AssertionError(f'not a workflow condition: {condition!r}')
    return '${{ !github.event.pull_request.draft && (' + condition[4:-3] + ') }}'


def in_step(workflow, job, name, before, after):
    """Mutate the named real step, even when another step shares a line."""
    step = ci_preflight._step_named(ci_preflight._steps(_jobs(workflow)[job]),
                                    '      - name: ' + name)
    if not step or before not in step:
        raise AssertionError(f'{before!r} is not in step {name}')
    return workflow.replace(step, step.replace(before, after, 1), 1)


def routed_line(before, value):
    """Keep the CI matrix's JSON string shape when replacing a literal array."""
    if before.startswith('    runs-on:'):
        return runner_declaration(value)
    if before.startswith("            runner: '") and value.startswith('['):
        value = "'" + value + "'"
    return before.split(': ', 1)[0] + ': ' + value + '\n'


def site_runner(name, job, fleet):
    if fleet == 'gke':
        return EXPECTED_GKE_JOBS[name][job]
    return {'pr': PR_RUNNER, 'post': POST_RUNNER, 'main': RELEASE_RUNNER}[fleet]


class WorkflowActionPins(unittest.TestCase):
    """Check the actual workflow and composite-action references."""

    def action_sources(self):
        paths = sorted(path for path in (ROOT / '.github/workflows').iterdir()
                       if path.suffix in ('.yml', '.yaml'))
        for name in ('action.yml', 'action.yaml'):
            paths.extend(sorted((ROOT / '.github/actions').rglob(name)))
        # The bytes as stored, decoded as preflight decodes them: read_text()
        # would turn a bare CR into LF before the lint ever saw it.
        return {str(path.relative_to(ROOT)): path.read_bytes().decode('utf-8')
                for path in paths}

    def third_party_uses(self, sources=None):
        if sources is None:
            sources = self.action_sources()
        findings = []
        references = [(Path(name), number, reference, comment)
                      for name, number, reference, comment in
                      ci_preflight.action_uses(sources, findings=findings)
                      if not reference.startswith(ci_preflight.ACTION_PIN_EXEMPT_PREFIXES)]
        self.assertEqual(findings, [], '\n'.join(findings))
        self.assertTrue(references, 'no third-party action references were checked')
        return references

    def assert_full_commit_sha(self, reference):
        self.assertRegex(reference, ci_preflight.ACTION_PIN_SHA)

    def assert_version_comment(self, comment):
        self.assertRegex(comment, ci_preflight.ACTION_PIN_COMMENT)

    def test_third_party_actions_use_full_commit_shas(self):
        for path, number, reference, _ in self.third_party_uses():
            with self.subTest(path=str(path), line=number):
                self.assert_full_commit_sha(reference)

    def test_third_party_actions_have_version_comments(self):
        for path, number, _, comment in self.third_party_uses():
            with self.subTest(path=str(path), line=number):
                self.assert_version_comment(comment)

    def test_real_tree_helper_accepts_lint_scalar_grammar(self):
        reference = 'actions/checkout@11d5960a326750d5838078e36cf38b85af677262'
        for value in (f'"{reference}" # v4.4.0', f"'{reference}' # v4.4.0",
                      'docker://alpine:3.20', f'{reference} # v4.4.0-rc.1'):
            with self.subTest(value=value):
                # Retain a third-party row when the second row is exempt.
                sources = {'.github/workflows/grammar.yml':
                           f'- uses: {reference} # v4.4.0\n- uses: {value}\n'}
                self.assertEqual(ci_preflight.action_pin_findings(sources), [])
                rows = self.third_party_uses(sources)
                expected = [reference] if value.startswith('docker://') else [reference, reference]
                self.assertEqual([row[2] for row in rows], expected)
                for _, _, parsed, comment in rows:
                    self.assert_full_commit_sha(parsed)
                    self.assert_version_comment(comment)
        # The shared real-tree assertions must still reject these mutations.
        path = '.github/workflows/grammar.yml'
        for _, _, reference, _ in self.third_party_uses(
                {path: '- uses: actions/checkout@main # v4.4.0\n'}):
            with self.assertRaises(AssertionError):
                self.assert_full_commit_sha(reference)
        for _, _, _, comment in self.third_party_uses(
                {path: '- uses: actions/checkout@0123456789abcdef0123456789abcdef01234567\n'}):
            with self.assertRaises(AssertionError):
                self.assert_version_comment(comment)

    def test_real_tree_helper_refuses_unsupported_uses_shapes(self):
        path = '.github/workflows/grammar.yml'
        text = ('- uses: actions/checkout@0123456789abcdef0123456789abcdef01234567 # v4.4.0\n'
                '- {uses: actions/checkout@main}\n')
        expected = (f'{path}:2: unsupported YAML construct in a workflow (flow mappings): '
                    "rewrite it in plain block style; '- {uses: actions/checkout@main}'")
        with self.assertRaises(AssertionError) as failure:
            self.third_party_uses({path: text})
        self.assertIn(expected, str(failure.exception))

    def test_every_real_action_source_has_zero_findings(self):
        sources = self.action_sources()
        self.assertTrue(any(name.startswith('.github/workflows/') for name in sources))
        self.assertTrue(any(name.startswith('.github/actions/') for name in sources))
        self.assertEqual(ci_preflight.action_pin_findings(sources), [])
        # Keep the complete population and a valid pin while introducing a
        # hidden key: zero findings must mean the grammar was checked too.
        path = '.github/workflows/grammar-control.yml'
        line = r'- "\u0075ses": actions/checkout@main'
        changed = {**sources, path: (
            '- uses: actions/checkout@0123456789abcdef0123456789abcdef01234567 # v4.4.0\n'
            + line + '\n')}
        self.assertEqual(ci_preflight.action_pin_findings(changed), [
            f'{path}:2: unsupported YAML construct in a workflow (quoted keys): '
            f'rewrite it in plain block style; {line!r}'])


class AppTokenEnvironments(unittest.TestCase):
    """Exercise the existing contract entrypoint, including its file loader."""

    WORKFLOW = 'app-token-guard.yml'
    # CiFileLoading.make_tree reads these two names from the test it is lent to.
    CONFIG = '.github/actionlint.yaml'
    CMAKE = 'tests/CMakeLists.txt'
    SHA = '0123456789abcdef0123456789abcdef01234567'
    STEP = ('      - uses: actions/create-github-app-token@' + SHA + ' # v1.12.0\n')

    @classmethod
    def workflow(cls, environment='', steps=None):
        return ('on: workflow_dispatch\njobs:\n  mint:\n'
                f'    runs-on: {POST_RUNNER}\n' + environment
                + '    steps:\n' + (cls.STEP if steps is None else steps))

    @staticmethod
    def finding(name, job='mint'):
        return (f'{name} job {job}: App-token minting/private-key use requires a '
                'literal, non-empty environment; review other minting methods by hand')

    def app_findings(self, workflows, *, checker=None):
        # This entrypoint exists on main. Missing policy must yield a
        # missing finding, never an import/setup error for a new helper.
        sources = [(ROOT / path).read_text() for path in CI_SOURCES]
        checker = ci_workflow_findings if checker is None else checker
        findings = checker(*sources, others=workflows)
        return [finding for finding in findings if ': App-token ' in finding
                and any(finding.startswith(f'{name} job ') for name in workflows)]

    def assert_refused(self, workflow, *, checker=None):
        self.assertEqual(self.app_findings({self.WORKFLOW: workflow}, checker=checker),
                         [self.finding(self.WORKFLOW)])

    def test_mint_action_requires_environment_at_every_ref(self):
        for ref in (self.SHA, 'v1', 'v2', 'main'):
            for quote in ('', '"', "'"):
                with self.subTest(ref=ref, quote=quote):
                    steps = ('      - name: Obtain installation credential\n'
                             f'        uses: {quote}actions/create-github-app-token@{ref}'
                             f'{quote} # v1.12.0\n')
                    self.assert_refused(self.workflow(steps=steps))

    def test_tibdex_mint_action_requires_environment_whatever_feeds_its_key(self):
        for ref in (self.SHA, 'v1', 'main'):
            for quote in ('', '"', "'"):
                for key_source in ('vars.APP_KEY', 'inputs.key', 'secrets.RELEASE_PEM'):
                    with self.subTest(ref=ref, quote=quote, key_source=key_source):
                        steps = ('      - name: Obtain installation credential\n'
                                 f'        uses: {quote}tibdex/github-app-token@{ref}{quote}\n'
                                 '        with:\n'
                                 f'          app_id: 1\n          key: ${{{{ {key_source} }}}}\n')
                        self.assert_refused(self.workflow(steps=steps))
                        self.assertEqual(self.app_findings({self.WORKFLOW: self.workflow(
                            '    environment: release\n', steps=steps)}), [])

    def test_literal_names_and_mapping_names_have_removal_controls(self):
        for environment in (
                '    environment: release\n',
                '    environment: 1a\n',
                '    environment: "release-bot" # automatic\n',
                "    environment: 'release'\n",
                '    environment:\n      name: release\n',
                '    environment:\n      name: "release-bot"\n'
                '      url: ${{ steps.deploy.outputs.url }}\n'):
            with self.subTest(environment=environment):
                self.assertEqual(self.app_findings(
                    {self.WORKFLOW: self.workflow(environment)}), [])
                self.assert_refused(self.workflow())

    def test_empty_expression_and_non_name_environments_are_refused(self):
        for environment in (
                '    environment:\n',
                '    environment: ""\n',
                "    environment: ''\n",
                '    environment: "   "\n',
                '    environment: null\n',
                '    environment: ~\n',
                '    environment: false\n',
                '    environment: 123\n',
                '    environment: .5\n',
                '    environment: 0x10\n',
                '    environment: {}\n',
                '    environment: []\n',
                '    environment: ${{ inputs.environment }}\n',
                '    environment: "release-${{ github.ref_name }}"\n',
                "    environment: ${{ 'release' }}\n",
                '    environment:\n      name:\n',
                '    environment:\n      name: ""\n',
                '    environment:\n      name: ${{ inputs.environment }}\n',
                '    environment:\n      url: https://example.com\n',
                '    environment:\n      name:\n        nested: release\n',
                '    environment: release\n    environment: release-bot\n',
                '    environment:\n      name: release\n      name: release-bot\n',
                '    environment: {name: release}\n'):
            with self.subTest(environment=environment):
                self.assert_refused(self.workflow(environment))

    def test_each_job_and_each_workflow_needs_its_own_environment(self):
        safe = self.workflow('    environment: release\n')
        second = self.workflow().replace('  mint:', '  later:', 1)
        second = second.replace('jobs:\n', 'jobs:\n  ordinary:\n'
                                f'    runs-on: {POST_RUNNER}\n'
                                '    steps:\n      - run: "true"\n', 1)
        workflows = {self.WORKFLOW: safe, 'second-app-token.yaml': second}
        self.assertEqual(self.app_findings(workflows),
                         [self.finding('second-app-token.yaml', 'later')])
        same_file = safe + second.split('jobs:\n', 1)[1]
        self.assertEqual(self.app_findings({self.WORKFLOW: same_file}),
                         [self.finding(self.WORKFLOW, 'later')])

    def test_quoted_environment_keys_do_not_hide_empty_names(self):
        for key in ('"environment"', "'environment'"):
            with self.subTest(key=key):
                text = self.workflow(f'    {key}: ""\n')
                self.assert_refused(text)
                # Existing YAML policy explicitly refuses even a quoted key
                # with a literal value; it cannot silently skip that job.
                accepted_name = self.workflow(f'    {key}: release\n')
                path = f'.github/workflows/{self.WORKFLOW}'
                self.assertIn(ActionPins.unsupported_finding(
                    path, 5, f'{key}: release', 'quoted keys'),
                    ci_preflight.action_pin_findings({path: accepted_name}))

    def test_flow_jobs_and_steps_are_explicit_yaml_refusals(self):
        path = f'.github/workflows/{self.WORKFLOW}'
        for text, number, source in (
                ('on: workflow_dispatch\njobs: {mint: {steps: ['
                 '{uses: actions/create-github-app-token@v2}]}}\n', 2,
                 'jobs: {mint: {steps: [{uses: actions/create-github-app-token@v2}]}}'),
                (self.workflow(steps='      - {uses: actions/create-github-app-token@v2}\n'),
                 6, '- {uses: actions/create-github-app-token@v2}')):
            with self.subTest(source=source):
                self.assertIn(ActionPins.unsupported_finding(path, number, source),
                              ci_preflight.action_pin_findings({path: text}))
        # The supported spelling must also refuse the missing environment.
        self.assert_refused(self.workflow())

    def test_private_key_handoffs_to_other_actions_and_scripts_are_refused(self):
        for steps in (
                '      - uses: example/installation-token@' + self.SHA + ' # v1.0.0\n'
                '        with:\n          private-key: ${{ secrets.RELEASE_PEM }}\n',
                '      - uses: example/installation-token@' + self.SHA + ' # v1.0.0\n'
                '        with:\n          private_key: ${{ secrets.RELEASE_PEM }}\n',
                '      - run: python3 mint.py\n'
                '        env:\n          APP_KEY: ${{ secrets.PINEFORGE_APP_PRIVATE_KEY }}\n',
                '      - run: python3 mint.py\n'
                "        env:\n          APP_KEY: ${{ secrets['PINEFORGE_APP_PRIVATE_KEY'] }}\n",
                '      - run: python3 mint.py\n'
                '        env:\n          APP_KEY: ${{ secrets . PINEFORGE_APP_PRIVATE_KEY }}\n',
                '      - run: python3 mint.py\n'
                '        env:\n          PRIVATE_KEY: ${{ secrets.RELEASE_PEM }}\n',
                '      - run: |\n'
                '          python3 mint.py --private-key "${{ secrets.RELEASE_PEM }}"\n',
                '      - run: python3 mint.py\n'
                '        env:\n          PRIVATE_KEY: |\n'
                '            ${{ secrets.RELEASE_PEM }}\n'):
            with self.subTest(steps=steps):
                self.assert_refused(self.workflow(steps=steps))
                self.assertEqual(self.app_findings({self.WORKFLOW: self.workflow(
                    '    environment: release\n', steps=steps)}), [])

    def test_inherited_private_key_and_script_mint_indicators_are_refused(self):
        inherited = self.workflow(steps='      - run: python3 mint.py\n').replace(
            'jobs:\n', 'env:\n  APP_KEY: ${{ secrets.PINEFORGE_APP_PRIVATE_KEY }}\njobs:\n', 1)
        self.assert_refused(inherited)
        for command in (
                'gh api --method POST "app/installations/$APP_INSTALLATION/access_tokens"',
                'curl -X POST "https://api.github.com/app/installations/123/access_tokens"',
                "const auth = createAppAuth({appId, privateKey});",
                'integration = GithubIntegration(app_id, private_key)',
                'auth = github.Auth.AppAuth(app_id, private_key)'):
            with self.subTest(command=command):
                self.assert_refused(self.workflow(steps='      - run: |\n          '
                                                 + command + '\n'))

    def test_ordinary_jobs_and_commented_action_names_do_not_need_environment(self):
        ordinary = self.workflow(steps=(
            '      # - uses: actions/create-github-app-token@v2\n'
            '      - run: |\n'
            '          echo "uses: actions/create-github-app-token@v2"\n'
            '        env:\n          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}\n'))
        self.assertEqual(self.app_findings({self.WORKFLOW: ordinary}), [])
        self.assert_refused(self.workflow())

    def test_second_workflow_files_are_discovered_by_the_ci_entrypoint(self):
        tree = CiFileLoading.make_tree(self)
        for name in ('second-app-token.yml', 'second-app-token.yaml'):
            with self.subTest(name=name):
                (tree / '.github/workflows' / name).write_text(self.workflow())
                code, output = CiFileLoading.run_stage(tree, '--check-ci-workflow')
                self.assertIn(self.finding(name), output.splitlines())
                self.assertEqual(code, 1, output)

    def test_disabling_or_loosening_the_guard_breaks_the_regression_assertion(self):
        self.assert_refused(self.workflow())
        source = inspect.getsource(ci_workflow_findings)
        guard = '    findings += app_token_environment_findings(workflows)\n'
        self.assertEqual(source.count(guard), 1)
        namespace = dict(ci_workflow_findings.__globals__)
        exec(compile(source.replace(guard, '', 1),
                     '<app-token-environment-check-removed>', 'exec'), namespace)
        with self.assertRaises(AssertionError):
            self.assert_refused(self.workflow(), checker=namespace['ci_workflow_findings'])
        source = inspect.getsource(ci_preflight.app_token_environment_findings)
        guard = '            if not _literal_job_environment(body):\n'
        self.assertEqual(source.count(guard), 1)
        mutant = source.replace(guard,
                                "            if not _job_values(body, 'environment'):\n", 1)
        exec(compile(mutant, '<app-token-environment-check-loosened>', 'exec'), namespace)
        exec(compile(inspect.getsource(ci_workflow_findings),
                     '<contract-with-loosened-environment-check>', 'exec'), namespace)
        with self.assertRaises(AssertionError):
            self.assert_refused(self.workflow('    environment: ${{ inputs.environment }}\n'),
                                checker=namespace['ci_workflow_findings'])

    def test_real_minting_jobs_have_exactly_the_inventoried_environments(self):
        expected = {
            ('release.yml', 'release'): ['release'],
            ('release.yml', 'notify-hub'): ['release'],
            ('release-ruleset-probe.yml', 'probe'): ['release'],
            ('promote-baseline.yml', 'promote'): ['release-bot'],
        }
        actual = {(path.name, job): ci_preflight._job_values(body, 'environment')
                  for path in sorted((ROOT / '.github/workflows').iterdir())
                  if path.suffix in ('.yml', '.yaml')
                  for job, body in _jobs(path.read_text()).items()
                  if ci_preflight._job_values(body, 'environment')}
        self.assertEqual(actual, expected)


class WorkflowEventFleets(unittest.TestCase):
    """Fleet expectations come from events and call edges, never file names."""

    def workflows(self):
        return {path.name: path.read_text()
                for path in sorted((ROOT / '.github/workflows').iterdir())
                if path.suffix in ('.yml', '.yaml')}

    def test_workflow_inventories_match_live_files_and_orphans_still_fail(self):
        workflows = self.workflows()
        for name, inventory in (
                ('commands', JOB_COMMANDS),
                ('hosted and GKE pins', ci_preflight.HOSTED_JOB_RUNNERS),
                ('GKE pins', ci_preflight.GKE_JOB_RUNNERS),
                ('release pins', ci_preflight.RELEASE_JOB_RUNNERS)):
            with self.subTest(inventory=name):
                self.assertEqual(set(inventory) - workflows.keys(), set())
        action_sources = {
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / '.github/actions').rglob('*')
            if path.is_file() and path.name in ('action.yml', 'action.yaml')}
        self.assertEqual(ci_preflight.TRUSTED_CACHE_FILES - workflows.keys() - action_sources,
                         set())
        self.assertEqual(ci_preflight.workflow_fleet_findings(workflows), [])
        orphan = ('on: workflow_call\njobs:\n  inspect:\n'
                  f'    runs-on: {PR_RUNNER}\n'
                  '    steps:\n      - run: "true"\n')
        self.assertEqual(ci_preflight.workflow_fleet_findings({'orphan.yml': orphan}),
                         ['orphan.yml must have a reachable event for fleet routing'])

    def test_post_events_require_post_fleet_except_the_fixed_hosted_job(self):
        workflows = self.workflows()
        post_events = {'push', 'schedule', 'workflow_dispatch'}
        checked = 0
        for name, workflow in workflows.items():
            events = ci_preflight._events(workflow)
            self.assertIsNotNone(events, name)
            # The isolated release/promotion policy has its own negative controls.
            if name in ci_preflight.RELEASE_JOB_RUNNERS:
                continue
            if not events & post_events:
                continue
            self.assertNotIn('pull_request', events, name)
            for job, body in _jobs(workflow).items():
                if ci_preflight._job_values(body, 'uses'):
                    continue  # The graph check below follows every local call.
                if name == 'native-live.yml':
                    self.assertEqual(ci_preflight._job_values(body, 'runs-on'),
                                     [NATIVE_HOSTED_RUNNERS[job]])
                    self.assertNotIn('PF_CI_LABELS', body)
                    continue
                if job in EXPECTED_GKE_JOBS.get(name, {}):
                    self.assertEqual(ci_preflight._job_values(body, 'runs-on'),
                                     [EXPECTED_GKE_JOBS[name][job]])
                    if name == 'ci-post.yml' and job not in ('preflight', 'preflight-slow'):
                        self.assertNotIn('PF_CI_LABELS', body)
                    continue
                if job in LIGHT_JOB_IDS.get(name, ()):
                    runner = EXPECTED_LIGHT_GKE_JOBS.get(name, {}).get(job, 'ubuntu-24.04')
                    self.assertEqual(ci_preflight._job_values(body, 'runs-on'), [runner])
                    continue
                checked += 1
                self.assertEqual([json.loads(value) for value in ci_preflight._job_values(
                                     body, 'runs-on', unquote=False)],
                                 [['self-hosted', 'pf-gcp-post']],
                                 f'{name} job {job}')
        self.assertGreater(checked, 0)
        self.assertEqual(ci_preflight.workflow_fleet_findings(workflows), [])
        for event in ('push', 'schedule', 'workflow_dispatch'):
            name = 'event-contract-' + event + '.yml'
            valid = (f'on:\n  {event}:\njobs:\n  inspect:\n'
                     '    runs-on: ["self-hosted","pf-gcp-post"]\n'
                     '    steps:\n      - run: "true"\n')
            self.assertEqual(ci_preflight.runner_findings({name: valid}), [])
            wrong = valid.replace('"pf-gcp-post"', '"pf-gcp-pr"', 1)
            self.assertNotEqual(wrong, valid)
            self.assertIn(f'{name} job inspect must run on ["self-hosted","pf-gcp-post"] for post-merge events',
                          ci_preflight.runner_findings({name: wrong}))

    def test_pull_request_workflows_never_name_post_fleet(self):
        workflows = self.workflows()
        checked = 0
        for name, workflow in workflows.items():
            events = ci_preflight._events(workflow) or set()
            if 'pull_request' not in events:
                continue
            checked += 1
            self.assertFalse(events & {'push', 'schedule', 'workflow_dispatch'}, name)
            self.assertNotIn('pf-gcp-post', workflow, name)
        self.assertGreater(checked, 0)
        # Reusable workflows inherit the caller's event, including nested calls.
        self.assertEqual(ci_preflight.workflow_fleet_findings(workflows), [])

    def test_post_fleet_mutation_on_pr_job_is_refused(self):
        for suffix in ('.yml', '.yaml'):
            name = 'arbitrary-pr-name' + suffix
            pr = ('on:\n  pull_request:\njobs:\n  inspect:\n'
                  '    runs-on: ["self-hosted","pf-gcp-pr"]\n'
                  '    steps:\n      - run: "true"\n')
            self.assertEqual(ci_preflight.runner_findings({name: pr}), [])
            valid_post = pr.replace('  pull_request:', '  push:', 1).replace('"pf-gcp-pr"', '"pf-gcp-post"', 1)
            # Red on the old "all non-release events use -pr" policy before
            # checking that the same post label is refused on a PR job.
            self.assertEqual(ci_preflight.runner_findings({name: valid_post}), [])
            post = pr.replace('"pf-gcp-pr"', '"pf-gcp-post"', 1)
            self.assertNotEqual(post, pr)
            finding = f'{name} job inspect must run on ["self-hosted","pf-gcp-pr"] for pull_request'
            self.assertIn(finding, ci_preflight.runner_findings({name: post}))
            # Renaming a file, adding an if, or hiding the job behind a local
            # workflow call cannot turn a PR into a post-merge run.
            guarded = post.replace('    runs-on:', '    if: false\n    runs-on:', 1)
            self.assertIn(finding, ci_preflight.runner_findings({name: guarded}))
            caller = ('on:\n  pull_request:\njobs:\n  call:\n'
                      '    uses: ./.github/workflows/nested.yml\n')
            nested = ('on:\n  workflow_call:\njobs:\n  call:\n'
                      '    uses: ./.github/workflows/leaf.yml\n')
            leaf = post.replace('  pull_request:', '  workflow_call:', 1)
            self.assertIn('leaf.yml job inspect must run on ["self-hosted","pf-gcp-pr"] for pull_request',
                          ci_preflight.runner_findings({name: caller, 'nested.yml': nested,
                                                        'leaf.yml': leaf}))

    def test_release_pool_labels_are_refused_on_real_pull_request_jobs(self):
        workflows = self.workflows()
        original = workflows['ci.yml']
        self.assertEqual(ci_preflight._events(original), {'pull_request'})
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for runner in ('["self-hosted","pf-ci","pf-ci-release"]',
                       '["self-hosted","pf-gcp-main"]'):
            with self.subTest(runner=runner):
                changed = in_job(original, 'authz',
                                 '    runs-on: ubuntu-24.04\n',
                                 f'    runs-on: {runner}\n')
                self.assertNotEqual(changed, original)
                self.assertIn('ci.yml job authz may use release pool labels '
                              'only in a pinned isolated release job',
                              ci_preflight.runner_findings({**workflows, 'ci.yml': changed}))
                # A new filename has no job pin to rely on.
                self.assertIn('arbitrary-pr.yml job authz may use release pool labels '
                              'only in a pinned isolated release job',
                              ci_preflight.runner_findings({'arbitrary-pr.yml': changed}))

    def test_release_keeps_its_current_pool_and_refuses_the_retired_label(self):
        release = self.workflows()['release.yml']
        self.assertEqual(ci_preflight._events(release), {'workflow_dispatch'})
        self.assertEqual(ci_preflight._job_values(_jobs(release)['release'], 'runs-on'),
                         ['["self-hosted","pf-gcp-main"]'])
        self.assertEqual(ci_preflight.runner_findings({'release.yml': release}), [])
        changed = in_job(release, 'release',
                         '    runs-on: ["self-hosted","pf-gcp-main"]\n',
                         '    runs-on: ["self-hosted","pf-ci","pf-ci-release"]\n')
        self.assertNotEqual(changed, release)
        self.assertIn('release.yml job release must run on ["self-hosted","pf-gcp-main"] '
                      '(isolated release)', ci_preflight.runner_findings({'release.yml': changed}))

    def test_flat_peak_release_pool_has_no_unproven_reusable_exception(self):
        workflows = self.workflows()
        original = ('on: workflow_call\njobs:\n  corpus-parity-subset:'
                    + _jobs(workflows['gke-pr.yml'])['corpus-parity-subset'])
        self.assertEqual(ci_preflight._events(original), {'workflow_call'})
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for label in ('pf-ci-release', 'pf-gcp-main'):
            flat = in_job(original, 'corpus-parity-subset', runner_declaration(GKE_PR_8),
                          runner_declaration(f'["self-hosted","{label}"]'))
            self.assertNotEqual(flat, original)
            finding = ('flat-peak.yml job corpus-parity-subset may use release pool labels '
                       'only in a pinned isolated release job')
            with self.subTest(label=label, caller='none'):
                self.assertIn(finding, ci_preflight.runner_findings({'flat-peak.yml': flat}))
            # A PR caller and a trusted post-merge caller both fail: this
            # event-only graph does not prove that a caller is release.yml.
            for name, job, callee in (
                    ('ci.yml', 'authz', None),
                    ('ci-post.yml', 'corpus-parity-subset', 'corpus-parity.yml')):
                with self.subTest(label=label, caller=name):
                    caller = (in_job(workflows[name], job,
                                     f'    uses: ./.github/workflows/{callee}\n',
                                     '    uses: ./.github/workflows/flat-peak.yml\n')
                              if callee else workflows[name] +
                              '\n  call:\n    uses: ./.github/workflows/flat-peak.yml\n')
                    self.assertNotEqual(caller, workflows[name])
                    self.assertIn(finding, ci_preflight.runner_findings(
                        {**workflows, name: caller, 'flat-peak.yml': flat}))

    def test_release_pins_cannot_hide_pull_request_callers(self):
        workflows = self.workflows()
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for callee in ('release.yml', 'promote-baseline.yml'):
            with self.subTest(callee=callee):
                caller = (workflows['ci.yml'] +
                          f'\n  call:\n    uses: ./.github/workflows/{callee}\n')
                self.assertNotEqual(caller, workflows['ci.yml'])
                self.assertIn(f'{callee} must not admit untrusted caller events to isolated release jobs',
                              ci_preflight.runner_findings({**workflows, 'ci.yml': caller}))

    def test_post_arm_build_rejects_the_amd64_post_fleet(self):
        workflows = self.workflows()
        post = workflows['ci-post.yml']
        self.assertIn('build-arm', _jobs(post))
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        changed = in_job(post, 'build-arm', runner_declaration(GKE_ARM),
                         f'    runs-on: {POST_RUNNER}\n')
        self.assertNotEqual(changed, post)
        findings = ci_preflight.runner_findings({**workflows, 'ci-post.yml': changed})
        self.assertIn(f'ci-post.yml job build-arm must run on {GKE_ARM}', findings)
        for scope, before, after in (
                (None, '\nenv:\n', f"\nenv:\n  PF_CI_LABELS: '{POST_RUNNER}'\n"),
                ('build-arm', '    steps:\n',
                 f"    env:\n      PF_CI_LABELS: '{POST_RUNNER}'\n    steps:\n")):
            with self.subTest(metadata_scope=scope):
                changed = in_job(post, scope, before, after)
                self.assertNotEqual(changed, post)
                self.assertIn('ci-post.yml job build-arm must not set PF_CI_LABELS on a GKE job',
                              ci_preflight.runner_findings({**workflows, 'ci-post.yml': changed}))

    def test_post_split_preserves_all_four_matrix_check_names(self):
        # Expand the actual matrix and name expressions so a missing cell or
        # a changed display name cannot silently change the required checks.
        def names(body):
            template, = ci_preflight._job_values(body, 'name')
            platforms = re.findall(r'^          - os: (\S+)$', body, re.MULTILINE)
            if not platforms:
                platform = re.search(r'\(([^,]+),', template)
                self.assertIsNotNone(platform)
                platforms = [platform.group(1)]
            types = re.search(r'^        build_type: \[([^\]]+)\]$', body, re.MULTILINE)
            if types is None:
                self.assertNotIn('${{', template)
                return [template]
            rendered = []
            for platform in platforms:
                for build_type in (value.strip() for value in types.group(1).split(',')):
                    values = {'platform.os': platform, 'build_type': build_type}
                    name = re.sub(r'\$\{\{\s*matrix\.(platform\.os|build_type)\s*\}\}',
                                  lambda match: values[match.group(1)], template)
                    self.assertNotIn('${{', name)
                    rendered.append(name)
            return rendered

        workflows = self.workflows()
        post = workflows['ci-post.yml']
        jobs = _jobs(post)
        self.assertIn('build-arm', jobs)
        self.assertNotIn('build-macos', jobs)
        expected = ['build (ubuntu-24.04, Release)', 'build (ubuntu-24.04, Debug)',
                    'build (ubuntu-24.04-arm, Release)', 'build (ubuntu-24.04-arm, Debug)']
        self.assertEqual(len(expected), 4)
        self.assertCountEqual(names(jobs['build-release-gate']) + names(jobs['build-debug-gate'])
                              + names(jobs['build-arm']), expected)
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        changed = in_job(post, 'build-arm', '    name: build (',
                         '    name: build arm64 (')
        self.assertNotEqual(changed, post)
        mutated = _jobs(changed)
        self.assertNotEqual(sorted(names(mutated['build-release-gate'])
                                   + names(mutated['build-debug-gate']) + names(mutated['build-arm'])),
                            sorted(expected))
        self.assertIn('ci-post.yml build-arm legs must keep their names: '
                      'build (ubuntu-24.04-arm, ${{ matrix.build_type }})',
                      ci_preflight.runner_findings({**workflows, 'ci-post.yml': changed}))

    def test_post_hosted_exception_refuses_jobs_outside_the_fixed_allowlist(self):
        post = ('on: workflow_dispatch\njobs:\n  build-arm:\n'
                + runner_declaration(GKE_ARM) + '    steps:\n      - run: "true"\n')
        # This positive control is red under the old blanket post-fleet rule.
        self.assertEqual(ci_preflight.workflow_fleet_findings({'ci-post.yml': post}), [])
        for runner in ('ubuntu-latest', 'ubuntu-24.04-arm', 'macos-26'):
            changed = post + ('\n  unexpected:\n    runs-on: ' + runner
                              + '\n    steps:\n      - run: "true"\n')
            self.assertNotEqual(changed, post)
            self.assertIn(f'ci-post.yml job unexpected must run on {POST_RUNNER} for post-merge events',
                          ci_preflight.runner_findings({'ci-post.yml': changed}))
        workflows = self.workflows()
        original = workflows['ci-post.yml']
        changed = in_job(original, 'build-arm', runner_declaration(GKE_ARM),
                         '    runs-on: ubuntu-latest\n')
        self.assertNotEqual(changed, original)
        self.assertIn(f'ci-post.yml job build-arm must run on {GKE_ARM}',
                      ci_preflight.runner_findings({**workflows, 'ci-post.yml': changed}))

    def test_post_hosted_exception_never_applies_to_pull_request(self):
        post = ('on: workflow_dispatch\njobs:\n  build-arm:\n'
                + runner_declaration(GKE_ARM) + '    steps:\n      - run: "true"\n')
        self.assertEqual(ci_preflight.workflow_fleet_findings({'ci-post.yml': post}), [])
        pr = post.replace('on: workflow_dispatch', 'on: pull_request', 1)
        self.assertNotEqual(pr, post)
        finding = f'ci-post.yml job build-arm must run on {PR_RUNNER} for pull_request'
        self.assertIn(finding, ci_preflight.runner_findings({'ci-post.yml': pr}))
        changed = pr.replace(runner_declaration(GKE_ARM), f'    runs-on: {POST_RUNNER}\n', 1)
        self.assertNotEqual(changed, pr)
        self.assertIn(finding, ci_preflight.runner_findings({'ci-post.yml': changed}))
        # Reusable calls must not make the fixed hosted exception PR-reachable.
        caller = ('on: pull_request\njobs:\n  call:\n'
                  '    uses: ./.github/workflows/ci-post.yml\n')
        callee = changed.replace('on: pull_request', 'on: workflow_call', 1)
        self.assertNotEqual(callee, changed)
        self.assertIn(finding, ci_preflight.runner_findings(
            {'pr.yml': caller, 'ci-post.yml': callee}))


POST_FLEET_CASES = (
    ('ci-post.yml', 'build', 'Release'),
    ('ci-post.yml', 'build-debug', 'Debug'),
    ('ci-post.yml', 'sanitizers', 'sanitizers'),
    ('ci-post.yml', 'kernel-only', 'kernel'),
    ('native-live.yml', 'native-live-shards', 'native'),
    ('native-live.yml', 'native-live', 'live-sanitizers'),
    ('native-live.yml', 'native-live', 'live-tsan'),
)
PAID_RUNNER_LABELS = (
    'pf-linux-x64-16', 'pf-linux-x64-32', 'pf-linux-x64-64', 'pf-linux-x64-128',
    'pf-linux-arm64-2', 'pf-linux-arm64-16', 'pf-linux-arm64-64', 'pf-linux-arm64-128',
    'ubuntu-24.04-larger', 'ubuntu-24.04-xlarge', 'macos-26-large',
)

def runner_declaration(runner):
    """Render scalar pins and legacy mappings used by refusal controls."""
    if runner.startswith('group: '):
        group, label = runner.removeprefix('group: ').split(' labels: ')
        return f'    runs-on:\n      group: {group}\n      labels: {label}\n'
    return f'    runs-on: {runner}\n'


class LightGkeRunnerPolicy(unittest.TestCase):
    def workflows(self):
        workflows = {path.name: path.read_text()
                     for path in sorted((ROOT / '.github/workflows').iterdir())
                     if path.suffix in ('.yml', '.yaml')}
        # Supply the requested routes even before the real workflows are moved.
        for name, pins in EXPECTED_LIGHT_GKE_JOBS.items():
            for job, runner in pins.items():
                before = ci_preflight._job_values(
                    _jobs(workflows[name])[job], 'runs-on', unquote=False)
                self.assertEqual(len(before), 1)
                workflows[name] = in_job(
                    workflows[name], job, runner_declaration(before[0]),
                    runner_declaration(runner))
        return workflows

    def test_light_scalar_routes_and_unchanged_proof_routes_are_accepted(self):
        workflows = self.workflows()
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for name, pins in EXPECTED_LIGHT_GKE_JOBS.items():
            for job, runner in pins.items():
                with self.subTest(workflow=name, job=job):
                    body = _jobs(workflows[name])[job]
                    self.assertEqual(ci_preflight._job_values(
                        body, 'runs-on', unquote=False), [runner])
                    for wrong in ('ubuntu-24.04', 'ubuntu-latest', PR_RUNNER,
                                  f'["{runner}"]', f'"{runner}"',
                                  f'group: gke-runners labels: {runner}',
                                  "${{ 'gke-2cpu-4gb-amd64' }}",
                                  GKE_4 if runner == GKE_2 else GKE_2):
                        changed = in_job(workflows[name], job,
                                         runner_declaration(runner),
                                         runner_declaration(wrong))
                        self.assertNotEqual(changed, workflows[name])
                        self.assertIn(f'{name} job {job} must run on {runner}',
                                      ci_preflight.runner_findings(
                                          {**workflows, name: changed}))

    def test_scalar_authz_headers_keep_the_verbatim_canon_and_refusal(self):
        workflows = self.workflows()
        for name, aggregate in (('ci-post.yml', 'build-gate'),
                                ('native-live.yml', 'native-live-gate')):
            self.assertEqual(ci_preflight.authz_workflow_findings(
                workflows[name], name, aggregate), [])
        for name in ('corpus-parity.yml', 'docs-post.yml'):
            self.assertEqual(ci_preflight.gated_workflow_findings(workflows[name], name), [])
        for name in ('ci-post.yml', 'native-live.yml', 'corpus-parity.yml', 'docs-post.yml'):
            changed = in_job(workflows[name], 'authz', '          set -eu\n',
                             '          set -u\n')
            validator = (ci_preflight.gated_workflow_findings(changed, name)
                         if name in ('corpus-parity.yml', 'docs-post.yml') else
                         ci_preflight.authz_workflow_findings(
                             changed, name, 'build-gate' if name == 'ci-post.yml'
                             else 'native-live-gate'))
            self.assertIn(f'{name} must start with the verbatim canon authz job', validator)

    def test_probe_and_promotion_guard_pins_accept_only_the_new_runner(self):
        workflows = self.workflows()
        self.assertEqual(ci_preflight.release_rulesets_probe_workflow_findings(
            workflows['release-ruleset-probe.yml'], workflows['release.yml']), [])
        guard = '  promote-guard:' + _jobs(workflows['promote-baseline.yml'])['promote-guard']
        self.assertEqual(ci_preflight._code(guard), ci_preflight._code(ci_preflight.PROMOTE_GUARD))
        old_probe = in_job(workflows['release-ruleset-probe.yml'], 'authz',
                           runner_declaration(GKE_2), runner_declaration('ubuntu-24.04'))
        self.assertIn(RULESETS_PROBE_FINDING,
                      ci_preflight.release_rulesets_probe_workflow_findings(
                          old_probe, workflows['release.yml']))
        old_guard = in_job(workflows['promote-baseline.yml'], 'promote-guard',
                           runner_declaration(GKE_2), runner_declaration('ubuntu-latest'))
        self.assertNotEqual(ci_preflight._code(
            '  promote-guard:' + _jobs(old_guard)['promote-guard']),
                            ci_preflight._code(ci_preflight.PROMOTE_GUARD))

    def test_scalar_routes_still_refuse_pr_reachability_and_credentials(self):
        workflows = self.workflows()
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for name, pins in EXPECTED_LIGHT_GKE_JOBS.items():
            for job in pins:
                for extra in ('    environment: production\n',
                              '    env:\n      KEY: ${{ secrets.DEPLOY_KEY }}\n',
                              '    permissions:\n      id-token: write\n'):
                    body = _jobs(workflows[name])[job]
                    before, after = '    steps:\n', extra + '    steps:\n'
                    if extra.startswith('    permissions:'):
                        permission = ci_preflight._job_block(body, 'permissions')
                        if permission:
                            before, after = '\n'.join(permission) + '\n', extra
                    elif extra.startswith('    env:') and ci_preflight._job_values(body, 'env'):
                        before, after = '    env:\n', extra
                    changed = in_job(workflows[name], job, before, after)
                    self.assertNotEqual(changed, workflows[name])
                    self.assertIn(f'{name} job {job} must not expose credentials on GKE',
                                  ci_preflight.runner_findings({**workflows, name: changed}))
            direct = workflows[name].replace('on:\n', 'on:\n  pull_request:\n', 1)
            self.assertNotEqual(direct, workflows[name])
            self.assertIn(f'{name} is reachable from pull_request and must not contain GKE routing',
                          ci_preflight.runner_findings({**workflows, name: direct}))
            if 'workflow_call' in ci_preflight._events(workflows[name]):
                caller = ('on: pull_request\njobs:\n  call:\n'
                          f'    uses: ./.github/workflows/{name}\n')
                self.assertIn(f'{name} is reachable from pull_request and must not contain GKE routing',
                              ci_preflight.runner_findings({**workflows, 'pr-caller.yml': caller}))

    def test_guard_permission_override_and_inherited_oidc_are_distinct(self):
        workflows = self.workflows()
        name, job = 'promote-baseline.yml', 'promote-guard'
        self.assertNotIn(f'{name} job {job} must not expose credentials on GKE',
                         ci_preflight.runner_findings(workflows))
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        permission = '    permissions:\n      pull-requests: read\n'
        for replacement in ('', '    permissions: write-all\n',
                            '    permissions:\n      id-token: write\n'):
            changed = in_job(workflows[name], job, permission, replacement)
            self.assertNotEqual(changed, workflows[name])
            self.assertIn(f'{name} job {job} must not expose credentials on GKE',
                          ci_preflight.runner_findings({**workflows, name: changed}))

    def test_real_post_light_split_pins_seven_and_five_instances(self):
        names = ('ci-post.yml', 'native-live.yml', 'corpus-parity.yml')
        workflows = {name: (ROOT / '.github/workflows' / name).read_text() for name in names}
        calls = [value.removeprefix('./.github/workflows/')
                 for body in _jobs(workflows['ci-post.yml']).values()
                 for value in ci_preflight._job_values(body, 'uses')]
        self.assertCountEqual(calls, ['native-live.yml', 'corpus-parity.yml', 'corpus-parity.yml'])
        counts = {GKE_2: 0, GKE_4: 0}
        for name in names:
            for job, runner in EXPECTED_LIGHT_GKE_JOBS[name].items():
                body = _jobs(workflows[name])[job]
                self.assertEqual(ci_preflight._job_values(body, 'runs-on', unquote=False), [runner])
                self.assertEqual(ci_preflight._matrix_rows(body), [{}])
                counts[runner] += 1 if name == 'ci-post.yml' else calls.count(name)
        self.assertEqual(counts, {GKE_2: 7, GKE_4: 5})
        self.assertEqual(ci_preflight._gke_cap_findings(workflows), [])
        for name, label, boundary, overflow in (('ci-post.yml', GKE_2, 2, 3),
                                                 ('native-live.yml', GKE_4, 4, 5)):
            for replicas in (boundary, overflow):
                matrix = ('    strategy:\n      matrix:\n        replica: ['
                          + ', '.join(str(i) for i in range(replicas)) + ']\n')
                changed = in_job(workflows[name], 'authz', '    steps:\n',
                                 matrix + '    steps:\n')
                findings = ci_preflight._gke_cap_findings({**workflows, name: changed})
                expected = ([] if replicas == boundary else
                            [f'ci-post.yml GKE label {label} expands to 9 jobs; cap is 8'])
                self.assertEqual(findings, expected)

    def test_cap_counts_scalar_and_mapping_instances_together(self):
        workflows = self.workflows()
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for count in (7, 8):
            matrix = ('    strategy:\n      matrix:\n        replica: ['
                      + ', '.join(str(i) for i in range(count)) + ']\n')
            post = ('on: workflow_dispatch\njobs:\n  scalar:\n'
                    + runner_declaration(GKE_2) + matrix
                    + '    steps:\n      - run: git --version\n  mapping:\n'
                    + runner_declaration(f'group: gke-runners labels: {GKE_2}')
                    + '    steps:\n      - run: git --version\n')
            self.assertEqual(ci_preflight._gke_cap_findings({'ci-post.yml': post}),
                             [] if count == 7 else
                             [f'ci-post.yml GKE label {GKE_2} expands to 9 jobs; cap is 8'])


class GkeRunnerPolicy(unittest.TestCase):
    CALLER_CREDENTIALS = (
        ('inherited secrets', '    secrets: inherit\n'),
        ('mapped secrets', '    secrets:\n      DEPLOY_KEY: ${{ secrets.DEPLOY_KEY }}\n'),
        ('id-token write', '    permissions:\n      id-token: write\n'),
        ('id-token read', '    permissions:\n      id-token: read\n'),
        ('write-all', '    permissions: write-all\n'),
        ('environment', '    environment: release\n'),
    )

    def fixture(self, *, job='preflight', runner=GKE_8, extra='', matrix=''):
        return ('on: workflow_dispatch\njobs:\n' + f'  {job}:\n'
                + runner_declaration(runner) + extra + matrix
                + '    steps:\n      - run: git --version\n')

    def caller_fixture(self, extra=''):
        caller = ('on: workflow_dispatch\njobs:\n  parity:\n'
                  '    uses: ./.github/workflows/corpus-parity.yml\n' + extra)
        callee = self.fixture(job='corpus-parity-subset').replace(
            'on: workflow_dispatch',
            'on:\n  workflow_call:\n    secrets:\n      DEPLOY_KEY:\n        required: false')
        return {'ci-post.yml': caller, 'corpus-parity.yml': callee}

    def test_exact_gke_route_is_accepted(self):
        self.assertEqual(ci_preflight.workflow_fleet_findings(
            {'ci-post.yml': self.fixture()}), [])

    def assert_gke_shape_refused(self, runner):
        findings = ci_preflight.runner_findings(
            {'ci-post.yml': self.fixture(runner=runner)})
        self.assertIn('ci-post.yml job preflight must use a bare scalar GKE scale-set name',
                      findings)
        self.assertIn('ci-post.yml job preflight must run on gke-8cpu-16gb-amd64',
                      findings)

    def test_gke_group_mapping_is_refused(self):
        for runner in (
                'group: gke-runners labels: gke-8cpu-16gb-amd64',
                '{group: gke-runners, labels: gke-8cpu-16gb-amd64}',
                '{labels: gke-8cpu-16gb-amd64}',
                '\n      labels: gke-8cpu-16gb-amd64'):
            with self.subTest(runner=runner):
                self.assert_gke_shape_refused(runner)

    def test_gke_single_label_array_is_refused(self):
        for runner in ('[gke-8cpu-16gb-amd64]', '["gke-8cpu-16gb-amd64"]'):
            with self.subTest(runner=runner):
                self.assert_gke_shape_refused(runner)

    def test_gke_self_hosted_array_is_refused(self):
        self.assert_gke_shape_refused('["self-hosted","gke-8cpu-16gb-amd64"]')

    def test_gke_expression_is_refused(self):
        for runner in ("${{ 'gke-8cpu-16gb-amd64' }}",
                       "${{ inputs.runner || 'gke-8cpu-16gb-amd64' }}"):
            with self.subTest(runner=runner):
                self.assert_gke_shape_refused(runner)

    def test_gke_wrong_case_is_refused(self):
        for runner in ('GKE-8CPU-16GB-AMD64', 'gKe-8cpu-16gb-amd64'):
            with self.subTest(runner=runner):
                self.assert_gke_shape_refused(runner)

    def test_gke_wrong_size_is_refused(self):
        findings = ci_preflight.runner_findings(
            {'ci-post.yml': self.fixture(runner='gke-16cpu-32gb-amd64')})
        self.assertIn('ci-post.yml job preflight may use GKE routing only at its exact pinned job and label',
                      findings)
        self.assertIn('ci-post.yml job preflight must run on gke-8cpu-16gb-amd64',
                      findings)

    def test_every_pinned_gke_job_requires_bare_scalar_runner(self):
        for name, pins in EXPECTED_GKE_JOBS.items():
            workflow = (ROOT / '.github/workflows' / name).read_text()
            for job, label in pins.items():
                marker = re.search(
                    r'(?m)^    runs-on:[^\n]*(?:\n      (?:group|labels):[^\n]*)*\n',
                    _jobs(workflow)[job])
                self.assertIsNotNone(marker)
                bare = in_job(workflow, job, marker.group(), runner_declaration(label))
                prefix = f'{name} job {job} must run on '
                with self.subTest(workflow=name, job=job, runner=label):
                    self.assertEqual([
                        finding for finding in ci_preflight.runner_findings({name: bare})
                        if finding.startswith(prefix)
                    ], [])
                arch = label.rsplit('-', 1)[1]
                wrong_size = (f'gke-16cpu-32gb-{arch}' if label.startswith('gke-32cpu-')
                              else f'gke-32cpu-64gb-{arch}')
                for runner in (
                        f'group: gke-runners labels: {label}',
                        f'{{group: gke-runners, labels: {label}}}',
                        f'{{labels: {label}}}',
                        f'[{label}]', f'["{label}"]',
                        f'["self-hosted","{label}"]',
                        f"${{{{ '{label}' }}}}",
                        f"${{{{ inputs.runner || '{label}' }}}}",
                        f"'{label}'", f'"{label}"',
                        label.upper(), wrong_size):
                    with self.subTest(workflow=name, job=job, runner=runner):
                        changed = in_job(bare, job, runner_declaration(label),
                                         runner_declaration(runner))
                        self.assertNotEqual(changed, bare)
                        self.assertIn(prefix + label,
                                      ci_preflight.runner_findings({name: changed}))

    def test_pull_request_graph_refuses_wrong_case_gke_routes(self):
        runner = 'GKE-8CPU-16GB-AMD64'
        pr = self.fixture(runner=runner).replace('on: workflow_dispatch', 'on: pull_request')
        self.assertIn('ci.yml is reachable from pull_request and must not contain GKE routing',
                      ci_preflight.runner_findings({'ci.yml': pr}))
        caller = ('on: pull_request\njobs:\n  call:\n'
                  '    uses: ./.github/workflows/nested.yml\n')
        nested = ('on: workflow_call\njobs:\n  call:\n'
                  '    uses: ./.github/workflows/ci-post.yml\n')
        leaf = self.fixture(runner=runner).replace('on: workflow_dispatch', 'on: workflow_call')
        self.assertIn('ci-post.yml is reachable from pull_request and must not contain GKE routing',
                      ci_preflight.runner_findings(
                          {'ci.yml': caller, 'nested.yml': nested, 'ci-post.yml': leaf}))

    def test_wrong_case_gke_routes_cannot_hide_credentials(self):
        runner = 'GKE-8CPU-16GB-AMD64'
        workflow = self.fixture(runner=runner, extra='    environment: production\n')
        self.assertIn('ci-post.yml job preflight must not expose credentials on GKE',
                      ci_preflight.runner_findings({'ci-post.yml': workflow}))
        caller = ('on: workflow_dispatch\njobs:\n  parity:\n'
                  '    uses: ./.github/workflows/corpus-parity.yml\n'
                  '    secrets: inherit\n')
        callee = self.fixture(job='corpus-parity-subset', runner=runner).replace(
            'on: workflow_dispatch', 'on: workflow_call')
        self.assertIn('ci-post.yml job parity must not expose credentials on GKE',
                      ci_preflight.runner_findings(
                          {'ci-post.yml': caller, 'corpus-parity.yml': callee}))

    def test_gke_cap_cannot_split_a_scale_set_by_case(self):
        def matrix(count):
            return ('    strategy:\n      matrix:\n        shard: ['
                    + ', '.join(str(i) for i in range(1, count + 1)) + ']\n')

        post = self.fixture(runner='gke-8cpu-16gb-amd64', matrix=matrix(5))
        leaf = self.fixture(job='corpus-parity-subset', runner='GKE-8CPU-16GB-AMD64',
                            matrix=matrix(4)).replace(
                                'on: workflow_dispatch', 'on: workflow_call')
        post += '  parity:\n    uses: ./.github/workflows/corpus-parity.yml\n'
        self.assertIn('ci-post.yml GKE label gke-8cpu-16gb-amd64 expands to 9 jobs; cap is 8',
                      ci_preflight.runner_findings(
                          {'ci-post.yml': post, 'corpus-parity.yml': leaf}))

    def test_real_jobs_have_fixed_gke_routes_without_job_containers(self):
        for name, pins in EXPECTED_GKE_JOBS.items():
            jobs = _jobs((ROOT / '.github/workflows' / name).read_text())
            for job, runner in pins.items():
                with self.subTest(workflow=name, job=job):
                    body = jobs[job]
                    self.assertEqual(ci_preflight._job_values(body, 'runs-on'), [runner])
                    self.assertNotRegex(body, r'''(?m)^    ['"]?container['"]?\s*:''')
                    self.assertNotRegex(body, r'''(?m)^    ['"]?services['"]?\s*:''')
                    self.assertNotIn('      - name: Prepare the job container', body)

    def assert_original_apt_steps(self, body, dependencies):
        commands = [line for line in body.splitlines()
                    if re.search(r'\bapt-get\b', line)]
        expected = [] if dependencies is None else [
            '          sudo apt-get update',
            '          sudo DEBIAN_FRONTEND=noninteractive apt-get install -y ' + dependencies,
        ]
        self.assertEqual(commands, expected, 'apt-get lines must stay pinned')

    def test_apt_step_pin_refuses_single_line_and_chained_installs(self):
        workflow = (ROOT / '.github/workflows/ci-post.yml').read_text()
        self.assert_original_apt_steps(_jobs(workflow)['preflight'], 'shellcheck')
        for step in (
                '      - run: sudo apt-get install -y evil\n',
                '      - run: |\n          true && apt-get install -y evil\n'):
            with self.subTest(step=step):
                changed = in_job(workflow, 'preflight', '    steps:\n',
                                 '    steps:\n' + step)
                self.assertNotEqual(changed, workflow)
                with self.assertRaisesRegex(AssertionError, 'apt-get lines must stay pinned'):
                    self.assert_original_apt_steps(_jobs(changed)['preflight'], 'shellcheck')

    def test_real_gke_jobs_allow_original_sudo_apt_steps(self):
        packages = {
            'gke-pr.yml': {
                'preflight': 'shellcheck',
                'build': 'libeigen3-dev tzdata-legacy ccache',
                'build-debug': 'libeigen3-dev tzdata-legacy ccache',
                'corpus-parity-subset': 'libeigen3-dev ccache git-lfs',
            },
            'ci-post.yml': {
                'preflight': 'shellcheck', 'preflight-slow': None,
                'build': 'libeigen3-dev tzdata-legacy ccache strace',
                'build-debug': 'libeigen3-dev tzdata-legacy ccache strace',
                'build-arm': 'libeigen3-dev tzdata-legacy ccache strace',
                'sanitizers': 'libeigen3-dev tzdata-legacy ccache',
                'kernel-only': 'cmake ninja-build libeigen3-dev tzdata-legacy libsqlite3-dev libssl-dev zlib1g-dev ccache',
            },
            'corpus-parity.yml': {
                'corpus-parity': 'libeigen3-dev ccache git-lfs',
                'corpus-parity-subset': 'libeigen3-dev ccache git-lfs',
            },
            'native-live.yml': {
                'native-live': 'cmake ninja-build libeigen3-dev tzdata-legacy libsqlite3-dev libssl-dev zlib1g-dev ccache git-lfs strace',
                'native-live-shards': 'cmake ninja-build libeigen3-dev tzdata-legacy libsqlite3-dev libssl-dev zlib1g-dev ccache git-lfs strace',
            },
        }
        workflows = {path.name: path.read_text()
                     for path in sorted((ROOT / '.github/workflows').iterdir())
                     if path.suffix in ('.yml', '.yaml')}
        self.assertEqual(set(packages), set(EXPECTED_GKE_JOBS))
        for name, installs in packages.items():
            self.assertEqual(set(installs), set(EXPECTED_GKE_JOBS[name]))
            jobs = _jobs(workflows[name])
            for job, dependencies in installs.items():
                with self.subTest(workflow=name, job=job):
                    self.assert_original_apt_steps(jobs[job], dependencies)
        self.assertEqual(ci_preflight.runner_findings(workflows), [])

    def test_preflight_linter_constant_matches_real_post_step(self):
        workflow = (ROOT / '.github/workflows/ci-post.yml').read_text()
        linters = ci_preflight._step_named(
            ci_preflight._steps(_jobs(workflow)['preflight']),
            '      - name: Install workflow linters')
        self.assertTrue(linters)
        # This commit leaves the linter step byte-identical to main.
        self.assertEqual(ci_preflight._step_values(linters, 'run'),
                         [ci_preflight.PREFLIGHT_LINTER_RUN])

    def test_gke_jobs_refuse_paid_runner_reversion(self):
        for name, pins in EXPECTED_GKE_JOBS.items():
            for job, runner in pins.items():
                changed = self.fixture(job=job, runner='pf-linux-x64-16')
                with self.subTest(workflow=name, job=job):
                    findings = ci_preflight.runner_findings({name: changed})
                    self.assertIn(f'{name} job {job} must run on {runner}', findings)
                    self.assertIn(f'{name} must not use larger-runner labels; '
                                  'use the GCP fleet or hosted exceptions', findings)

    def test_gke_jobs_refuse_job_container_declarations(self):
        for name, pins in EXPECTED_GKE_JOBS.items():
            workflow = (ROOT / '.github/workflows' / name).read_text()
            for job in pins:
                for declaration in (
                        '    container:\n      image: ubuntu:24.04\n      options: --user root\n',
                        '    container: ubuntu:24.04\n',
                        '    container: {image: ubuntu:24.04}\n',
                        '    container: {}\n',
                        '    container:\n',
                        '    "container": ubuntu:24.04\n'):
                    with self.subTest(workflow=name, job=job, declaration=declaration):
                        changed = in_job(workflow, job, '    steps:\n',
                                         declaration + '    steps:\n')
                        self.assertNotEqual(changed, workflow)
                        self.assertIn(f'{name} job {job} must not declare a job container on GKE',
                                      ci_preflight.runner_findings({name: changed}))

    def test_gke_jobs_refuse_job_services_declarations(self):
        for name, pins in EXPECTED_GKE_JOBS.items():
            workflow = (ROOT / '.github/workflows' / name).read_text()
            for job in pins:
                for declaration in (
                        '    services:\n      cache:\n        image: redis:7\n',
                        '    services: {cache: {image: redis:7}}\n',
                        '    services: {}\n',
                        '    services:\n',
                        '    "services":\n      cache:\n        image: redis:7\n',
                        "    'services': {}\n"):
                    with self.subTest(workflow=name, job=job, declaration=declaration):
                        changed = in_job(workflow, job, '    steps:\n',
                                         declaration + '    steps:\n')
                        self.assertNotEqual(changed, workflow)
                        self.assertIn(f'{name} job {job} must not declare job services on GKE',
                                      ci_preflight.runner_findings({name: changed}))

    def test_gke_scale_sets_are_confined_to_exact_jobs(self):
        for name, job, runner in (
                ('extra.yml', 'preflight', GKE_8),
                ('ci-post.yml', 'unexpected', GKE_8),
                ('ci-post.yml', 'preflight', GKE_16),
                ('ci-post.yml', 'preflight',
                 'group: gke-runners labels: gke-8cpu-16gb-amd64'),
                ('ci-post.yml', 'preflight',
                 'group: other-runners labels: gke-8cpu-16gb-amd64')):
            with self.subTest(workflow=name, job=job, runner=runner):
                self.assertIn(f'{name} job {job} may use GKE routing only at its exact pinned job and label',
                              ci_preflight.runner_findings(
                                  {name: self.fixture(job=job, runner=runner)}))

    def test_pull_request_graph_refuses_gke_routes(self):
        for runner in (GKE_8, 'group: gke-runners labels: gke-8cpu-16gb-amd64',
                       'group: gke-runners labels: linux'):
            pr = self.fixture(runner=runner).replace('on: workflow_dispatch', 'on: pull_request')
            with self.subTest(runner=runner, route='direct'):
                self.assertIn('ci.yml is reachable from pull_request and must not contain GKE routing',
                              ci_preflight.runner_findings({'ci.yml': pr}))
            caller = ('on: pull_request\njobs:\n  call:\n'
                      '    uses: ./.github/workflows/nested.yml\n')
            nested = ('on: workflow_call\njobs:\n  call:\n'
                      '    uses: ./.github/workflows/ci-post.yml\n')
            leaf = self.fixture(runner=runner).replace('on: workflow_dispatch', 'on: workflow_call')
            with self.subTest(runner=runner, route='nested'):
                self.assertIn('ci-post.yml is reachable from pull_request and must not contain GKE routing',
                              ci_preflight.runner_findings(
                                  {'ci.yml': caller, 'nested.yml': nested, 'ci-post.yml': leaf}))

    def test_gke_credentials_are_refused_including_inherited_context(self):
        finding = 'ci-post.yml job preflight must not expose credentials on GKE'
        for extra in (
                '    env:\n      KEY: ${{ secrets.DEPLOY_KEY }}\n',
                "    env:\n      KEY: ${{ secrets['DEPLOY_KEY'] }}\n",
                '    env:\n      KEY: ${{ secrets[inputs.key] }}\n',
                '    permissions:\n      id-token: write\n',
                '    permissions:\n      id-token: read\n',
                '    permissions: write-all\n',
                '    environment: production\n',
                '    secrets: inherit\n'):
            with self.subTest(context='job', extra=extra):
                self.assertIn(finding, ci_preflight.runner_findings(
                    {'ci-post.yml': self.fixture(extra=extra)}))
        for context in ('env:\n  KEY: ${{ secrets.DEPLOY_KEY }}\n',
                        'permissions:\n  id-token: write\n'):
            workflow = self.fixture().replace('jobs:\n', context + 'jobs:\n', 1)
            with self.subTest(context=context):
                self.assertIn(finding, ci_preflight.runner_findings({'ci-post.yml': workflow}))
        for token in ('secrets.GITHUB_TOKEN', "secrets['GITHUB_TOKEN']", 'github.token'):
            workflow = self.fixture(extra=f'    env:\n      GH_TOKEN: ${{{{ {token} }}}}\n')
            self.assertNotIn(finding, ci_preflight.runner_findings({'ci-post.yml': workflow}))

    def test_gke_reusable_callers_refuse_credentials(self):
        for mutation, extra in self.CALLER_CREDENTIALS:
            workflows = self.caller_fixture(extra)
            body = _jobs(workflows['ci-post.yml'])['parity']
            with self.subTest(mutation=mutation):
                self.assertEqual(ci_preflight._job_values(body, 'runs-on'), [])
                self.assertEqual(ci_preflight._job_values(body, 'uses'),
                                 ['./.github/workflows/corpus-parity.yml'])
                self.assertIn('ci-post.yml job parity must not expose credentials on GKE',
                              ci_preflight.runner_findings(workflows))

    def test_gke_reusable_callers_accept_read_only_contents_without_secrets(self):
        finding = 'ci-post.yml job parity must not expose credentials on GKE'
        for extra in ('', '    permissions:\n      contents: read\n'):
            workflows = self.caller_fixture(extra)
            with self.subTest(extra=extra):
                self.assertEqual(ci_preflight._job_values(
                    _jobs(workflows['ci-post.yml'])['parity'], 'secrets'), [])
                self.assertNotIn(finding, ci_preflight.runner_findings(workflows))

    def test_gke_reusable_callers_refuse_credentials_through_nested_calls(self):
        workflows = self.caller_fixture()
        workflows['ci-post.yml'] = workflows['ci-post.yml'].replace(
            'uses: ./.github/workflows/corpus-parity.yml',
            'uses: ./.github/workflows/relay.yml')
        workflows['relay.yml'] = (
            'on: workflow_call\njobs:\n  forward:\n'
            '    uses: ./.github/workflows/corpus-parity.yml\n')
        for name, job, callee in (('ci-post.yml', 'parity', 'relay.yml'),
                                   ('relay.yml', 'forward', 'corpus-parity.yml')):
            before = f'    uses: ./.github/workflows/{callee}\n'
            changed = in_job(workflows[name], job, before, before + '    secrets: inherit\n')
            with self.subTest(workflow=name, job=job):
                self.assertNotEqual(changed, workflows[name])
                self.assertIn(f'{name} job {job} must not expose credentials on GKE',
                              ci_preflight.runner_findings({**workflows, name: changed}))

    def test_gke_reusable_caller_rule_is_not_limited_to_post_ci(self):
        graph = self.caller_fixture('    secrets: inherit\n')
        workflows = {'dispatch.yml': graph['ci-post.yml'],
                     'corpus-parity.yml': graph['corpus-parity.yml']}
        self.assertIn('dispatch.yml job parity must not expose credentials on GKE',
                      ci_preflight.runner_findings(workflows))

    def test_gke_reusable_caller_rule_does_not_flag_hosted_callees(self):
        caller = self.caller_fixture('    secrets: inherit\n')['ci-post.yml'].replace(
            'uses: ./.github/workflows/corpus-parity.yml',
            'uses: ./.github/workflows/hosted.yml')
        hosted = ('on: workflow_call\njobs:\n  inspect:\n'
                  '    runs-on: ubuntu-24.04\n    steps:\n      - run: git --version\n')
        self.assertNotIn('dispatch.yml job parity must not expose credentials on GKE',
                         ci_preflight.runner_findings(
                             {'dispatch.yml': caller, 'hosted.yml': hosted}))

    def test_real_gke_reusable_callers_refuse_credentials(self):
        workflows = {path.name: path.read_text()
                     for path in sorted((ROOT / '.github/workflows').iterdir())
                     if path.suffix in ('.yml', '.yaml')}
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        post = workflows['ci-post.yml']
        for job, callee in (('native-live', 'native-live.yml'),
                            ('corpus-parity-subset', 'corpus-parity.yml'),
                            ('corpus-parity', 'corpus-parity.yml')):
            body = _jobs(post)[job]
            self.assertEqual(ci_preflight._job_values(body, 'runs-on'), [])
            self.assertEqual(ci_preflight._job_values(body, 'uses'),
                             [f'./.github/workflows/{callee}'])
            self.assertEqual(ci_preflight._job_values(body, 'secrets'), [])
            before = f'    uses: ./.github/workflows/{callee}\n'
            for mutation, extra in self.CALLER_CREDENTIALS:
                changed = in_job(post, job, before, before + extra)
                with self.subTest(job=job, mutation=mutation):
                    self.assertNotEqual(changed, post)
                    self.assertIn(f'ci-post.yml job {job} must not expose credentials on GKE',
                                  ci_preflight.runner_findings(
                                      {**workflows, 'ci-post.yml': changed}))

    def test_gke_cap_expands_axes_and_reusable_calls(self):
        def graph(count):
            post = ('on: workflow_dispatch\njobs:\n  parity:\n'
                    '    uses: ./.github/workflows/corpus-parity.yml\n')
            matrix = ('    strategy:\n      matrix:\n'
                      '        profile: [one]\n        shard: ['
                      + ', '.join(str(i) for i in range(1, count + 1)) + ']\n')
            callee = self.fixture(job='corpus-parity-subset', matrix=matrix).replace(
                'on: workflow_dispatch', 'on: workflow_call')
            return {'ci-post.yml': post, 'corpus-parity.yml': callee}

        prefix = 'ci-post.yml GKE label gke-8cpu-16gb-amd64 expands to '
        self.assertEqual([finding for finding in ci_preflight.runner_findings(graph(8))
                          if finding.startswith(prefix)], [])
        self.assertIn(prefix + '9 jobs; cap is 8',
                      ci_preflight.runner_findings(graph(9)))
        # Calls are instances: calling the same reusable workflow twice doubles its rows.
        twice = graph(5)
        twice['ci-post.yml'] += ('  parity-again:\n'
                                 '    uses: ./.github/workflows/corpus-parity.yml\n')
        self.assertIn(prefix + '10 jobs; cap is 8', ci_preflight.runner_findings(twice))

    def test_gke_cap_expands_include_exclude_and_caller_mode(self):
        post = ('on: workflow_dispatch\njobs:\n'
                '  full:\n    uses: ./.github/workflows/corpus-parity.yml\n'
                '    with:\n      mode: full\n'
                '  subset:\n    uses: ./.github/workflows/corpus-parity.yml\n'
                '    with:\n      mode: subset\n')
        matrix = ('    strategy:\n      matrix:\n        shard: [1, 2, 3, 4, 5, 6, 7, 8]\n'
                  '        exclude:\n          - shard: 8\n'
                  '        include:\n          - shard: 9\n')
        leaf = self.fixture(job='corpus-parity-subset', matrix=matrix,
                            extra="    if: inputs.mode == 'subset'\n").replace(
                                'on: workflow_dispatch', 'on: workflow_call')
        prefix = 'ci-post.yml GKE label gke-8cpu-16gb-amd64 expands to '
        bounded = ci_preflight.runner_findings({'ci-post.yml': post, 'corpus-parity.yml': leaf})
        self.assertEqual([finding for finding in bounded if finding.startswith(prefix)], [])
        leaf = leaf.replace('          - shard: 9\n',
                            '          - shard: 9\n          - shard: 10\n')
        self.assertIn(prefix + '9 jobs; cap is 8',
                      ci_preflight.runner_findings({'ci-post.yml': post, 'corpus-parity.yml': leaf}))

    def test_gke_cap_refuses_unreadable_matrix(self):
        workflow = self.fixture(matrix='    strategy:\n      matrix: ${{ inputs.matrix }}\n')
        self.assertIn('ci-post.yml job preflight must use a readable static matrix for the GKE cap',
                      ci_preflight.runner_findings({'ci-post.yml': workflow}))

    def test_gke_cap_does_not_prune_dynamic_caller_inputs(self):
        post = ('on: workflow_dispatch\njobs:\n  parity:\n'
                '    uses: ./.github/workflows/corpus-parity.yml\n'
                '    with:\n      mode: ${{ inputs.mode }}\n')
        matrix = ('    strategy:\n      matrix:\n'
                  '        shard: [1, 2, 3, 4, 5, 6, 7, 8, 9]\n')
        leaf = self.fixture(job='corpus-parity-subset', matrix=matrix,
                            extra="    if: inputs.mode == 'subset'\n").replace(
                                'on: workflow_dispatch', 'on: workflow_call')
        self.assertIn('ci-post.yml GKE label gke-8cpu-16gb-amd64 expands to 9 jobs; cap is 8',
                      ci_preflight.runner_findings({'ci-post.yml': post, 'corpus-parity.yml': leaf}))

    def test_gke_cap_multiplies_caller_matrices(self):
        post = ('on: workflow_dispatch\njobs:\n  parity:\n'
                '    strategy:\n      matrix:\n        replica: [1, 2, 3]\n'
                '    uses: ./.github/workflows/corpus-parity.yml\n')
        leaf = self.fixture(
            job='corpus-parity-subset',
            matrix='    strategy:\n      matrix:\n        shard: [1, 2, 3]\n').replace(
                'on: workflow_dispatch', 'on: workflow_call')
        self.assertIn('ci-post.yml GKE label gke-8cpu-16gb-amd64 expands to 9 jobs; cap is 8',
                      ci_preflight.runner_findings({'ci-post.yml': post, 'corpus-parity.yml': leaf}))


class PostFleetRunnerPolicy(unittest.TestCase):
    def post_workflows(self):
        return {path.name: path.read_text()
                for path in sorted((ROOT / '.github/workflows').iterdir())
                if path.suffix in ('.yml', '.yaml')}

    def test_real_post_jobs_use_exact_post_fleet(self):
        for name, job, profile in POST_FLEET_CASES:
            with self.subTest(workflow=name, job=job, profile=profile):
                body = _jobs((ROOT / '.github/workflows' / name).read_text())[job]
                self.assertEqual(ci_preflight._job_values(body, 'runs-on', unquote=False),
                                 [EXPECTED_GKE_JOBS[name][job]])
                if job == 'native-live':
                    self.assertIn(f'          - profile: {profile}\n', body)

    def test_checker_accepts_post_fleet_at_every_migrated_job(self):
        self.assertEqual(ci_preflight.runner_findings(self.post_workflows()), [])

    def test_no_workflow_contains_paid_runner_labels(self):
        paid = re.compile(r'(?<![\w.-])(?:pf-linux-(?:x64|arm64)-\d+'
                          r'|[\w.-]+-(?:larger|xlarge|large))(?![\w.-])')
        paths = sorted(path for path in (ROOT / '.github/workflows').iterdir()
                       if path.suffix in ('.yml', '.yaml'))
        self.assertTrue(paths, 'the workflow inventory must not be empty')
        for path in paths:
            with self.subTest(workflow=path.name):
                self.assertIsNone(paid.search(path.read_text()), path.name)

    def test_paid_runner_labels_have_no_pinned_job_exemptions(self):
        workflows = self.post_workflows()
        for name, job, profile in POST_FLEET_CASES:
            for label in PAID_RUNNER_LABELS:
                with self.subTest(workflow=name, job=job, profile=profile, label=label):
                    runner = EXPECTED_GKE_JOBS[name][job]
                    changed = in_job(workflows[name], job, runner_declaration(runner),
                                     f'    runs-on: {label}\n')
                    self.assertNotEqual(changed, workflows[name])
                    findings = ci_preflight.runner_findings({**workflows, name: changed})
                    self.assertIn(f'{name} must not use larger-runner labels; '
                                  'use the GCP fleet or hosted exceptions', findings)
                    self.assertIn(f'{name} job {job} must run on {runner}', findings)

    def test_paid_runner_labels_are_refused_at_arbitrary_jobs(self):
        for label in PAID_RUNNER_LABELS:
            with self.subTest(label=label):
                workflow = ('on: workflow_dispatch\njobs:\n  inspect:\n'
                            f'    runs-on: {label}\n    steps:\n      - run: "true"\n')
                findings = ci_preflight.runner_findings({'unlisted.yml': workflow})
                self.assertIn('unlisted.yml must not use larger-runner labels; '
                              'use the GCP fleet or hosted exceptions', findings)
                self.assertIn(f'unlisted.yml job inspect must run on {POST_RUNNER}', findings)

    def test_post_jobs_refuse_other_fleets_scalars_and_reordered_labels(self):
        workflows = self.post_workflows()
        for name, job, profile in POST_FLEET_CASES:
            expected = EXPECTED_GKE_JOBS[name][job]
            for runner in (PR_RUNNER, RELEASE_RUNNER, 'pf-gcp-post',
                           '["pf-gcp-post","self-hosted"]',
                           '["self-hosted","pf-gcp-post","extra"]',
                           "'" + POST_RUNNER + "'", json.dumps(POST_RUNNER)):
                with self.subTest(workflow=name, job=job, profile=profile, runner=runner):
                    changed = in_job(workflows[name], job, runner_declaration(expected),
                                     f'    runs-on: {runner}\n')
                    self.assertNotEqual(changed, workflows[name])
                    self.assertIn(f'{name} job {job} must run on {expected}',
                                  ci_preflight.runner_findings({**workflows, name: changed}))
            for declaration in ('    environment: production\n',
                                '    env:\n      API_KEY: ${{ secrets.DEPLOY_KEY }}\n'):
                with self.subTest(workflow=name, job=job, profile=profile, credentials=declaration):
                    before, after = '    steps:\n', declaration + '    steps:\n'
                    if declaration.startswith('    env:') and '    env:\n' in _jobs(workflows[name])[job]:
                        before, after = '    env:\n', declaration
                    changed = in_job(workflows[name], job, before, after)
                    self.assertIn(f'{name} job {job} must keep production credentials '
                                  'on a hosted standard runner',
                                  ci_preflight.runner_findings({**workflows, name: changed}))
                    changed = in_job(changed, job, runner_declaration(expected),
                                     '    runs-on: ubuntu-24.04\n')
                    self.assertIn(f'{name} job {job} must run on {expected}',
                                  ci_preflight.runner_findings({**workflows, name: changed}))

    def test_post_fleet_docs_match_runner_policy(self):
        docs = (ROOT / 'docs/ci.md').read_text()
        self.assertEqual(len(docs.splitlines()), 2016)
        requirements = (
            'Post Release/Debug/kernel and native shards, post sanitizers and both native-live sanitizer\n'
            'profiles use fixed bare GKE scale-set names directly in runner pods carrying the engine toolchain image and sudo. No job routes by event.',
            "Hosted `ubuntu-24.04` runs PR `authz`, admission, `legs` and `result` jobs, and `docs.yml`'s `build`; "
            'secret-free post guards and aggregates use bare scalar GKE names. '
            'These light, secret-free jobs need no VM-only tools, mounts or caches.',
            'The seven `ci-post.yml` light jobs use `gke-2cpu-4gb-amd64`; called parity/native light jobs use '
            '`gke-4cpu-8gb-amd64` to satisfy the per-label cap, without a weight claim.',
            'Local reusable calls cannot mix PR and GKE jobs. The full post call graph expands to 36 GKE job instances: '
            "amd64 32/16/8/4/2 CPU labels carry 8/7/7/5/7 jobs and arm64 16 CPU carries 2; each label's static cap is 8.",
            'The amd64 post build shards and the two arm64 post cells use bare scalar',
            'Hosted exceptions cover PR light jobs and the existing hosted release cells; post light jobs use bare scalar GKE names.',
            '`native-live.yml` runs four `native` shards on `gke-8cpu-16gb-amd64`;\n'
            '`live-sanitizers` and `live-tsan` each build once on `gke-16cpu-32gb-amd64`.',
            "paid 16-core runners' medians over five runs were 1815, 1812, 1770, 2050",
            'The four amd64 post legs, arm64 cells, preflight, parity and native verification use bare GKE scale-set names; '
            'their aggregates use the light image. '
            "Promotion's `promote-guard` and docs-post/probe `authz` each use one "
            '`gke-2cpu-4gb-amd64` instance in separate roots; their image must include `gh` for permission lookups.',
            '| `ci-post.yml` | Push to `main`, 01:30 UTC schedule, dispatch | '
            'Seven light jobs: scalar `gke-2cpu-4gb-amd64`; bare GKE scalars: '
            'Release/sanitizers `gke-32cpu-64gb-amd64`, Debug/kernel `gke-16cpu-32gb-amd64`, '
            'preflight tiers `gke-8cpu-16gb-amd64`, arm64 `gke-16cpu-32gb-arm64` |',
            '| `native-live.yml` | Dispatch and post CI callers on `main` | '
            '`authz` and both aggregates: scalar `gke-4cpu-8gb-amd64`; bare GKE scalars: '
            'native shards `gke-8cpu-16gb-amd64`, sanitizer profiles `gke-16cpu-32gb-amd64` |',
            'GKE jobs carry no deployment secrets, effective id-token permissions or environment; '
            'pull_request workflows cannot name GKE routing. '
            'Runner-group workflow/ref allowlists must admit the post-only workflows '
            'at refs/heads/main before they can acquire pods. '
            'GKE jobs start only from gke-pr.yml, ci-post.yml, corpus-parity.yml, native-live.yml, '
            'docs-post.yml, release-ruleset-probe.yml and promote-baseline.yml '
            'at refs/heads/main (the runner group lists those workflow refs); '
            'a dispatch from another ref queues without a runner.',
            'Rollback may restore an earlier GCP fleet pin; paid larger-runner labels remain refused.',
            '`build-debug` shards, and kernel two shards; sanitizers uses four, '
            'all on their fixed GKE sizes.',
            '`HOSTED_JOB_RUNNERS` pins hosted jobs and exact bare GKE scalars; every GKE job runs directly '
            'in a runner pod carrying the engine toolchain image and sudo. PR checks are',
            '`build`, `build-debug`, `corpus-parity-subset`, `legs` and `result` in `gke-pr.yml`.',
            'Both matrices keep `fail-fast: false` and build pinned WebSocket curl cold.',
            '| `gke-pr / admit` | Hosted admission and main-tree rule check | Hosted admission and main-tree rule check | Not triggered |',
            'Every paid larger-runner label is refused in every workflow, including pinned jobs;',
        )
        for required in requirements:
            with self.subTest(required=required):
                self.assertIn(required, docs)


POST_SHARD_CASES = (
    ('build', 'release', 4, GKE_32, 'build-release-gate',
     'build (ubuntu-24.04, Release)'),
    ('build-debug', 'debug', 2, GKE_16, 'build-debug-gate',
     'build (ubuntu-24.04, Debug)'),
    ('sanitizers', 'sanitizers', 4, GKE_32, 'sanitizers-gate', 'sanitizers'),
    ('kernel-only', 'kernel', 2, GKE_16, 'kernel-only-gate', 'kernel-only'),
)


class PostCTestShardPolicy(unittest.TestCase):
    """Use explicit inputs so the new refusals can be traced on the old checker."""

    def shard_workflow(self, job, profile, count, label):
        build_type = 'Release' if profile == 'release' else 'Debug'
        axes = (f'        platform:\n          - os: ubuntu-24.04\n'
                f'        build_type: [{build_type}]\n') if job.startswith('build') else ''
        return ('on: workflow_dispatch\njobs:\n'
                f'  {job}:\n' + runner_declaration(label) +
                '    strategy:\n      fail-fast: false\n      matrix:\n'
                + axes + '        shard: [' + ', '.join(str(i) for i in range(1, count + 1))
                + ']\n    steps:\n'
                f'      - run: python3 scripts/ci_verify.py {profile} --build-dir build '
                f'--jobs {CORES} --ccache --ctest-shard ${{{{ matrix.shard }}}}/{count}\n')

    def test_larger_shard_labels_have_exact_post_job_exceptions(self):
        for job, profile, count, label, _, _ in POST_SHARD_CASES:
            with self.subTest(job=job):
                post = self.shard_workflow(job, profile, count, label)
                self.assertEqual(ci_preflight.workflow_fleet_findings({'ci-post.yml': post}), [])
                pr = post.replace('on: workflow_dispatch', 'on: pull_request', 1)
                self.assertIn(f'ci-post.yml job {job} must run on {PR_RUNNER} for pull_request',
                              ci_preflight.runner_findings({'ci-post.yml': pr}))
                for wrong in (PR_RUNNER, RELEASE_RUNNER, 'ubuntu-24.04',
                              'pf-linux-x64-16', 'pf-linux-x64-32', 'pf-linux-x64-64'):
                    changed = in_job(post, job, runner_declaration(label), runner_declaration(wrong))
                    self.assertIn(f'ci-post.yml job {job} must run on {label}',
                                  ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_all_three_x64_labels_are_refused_outside_the_exact_jobs(self):
        for label in ('pf-linux-x64-16', 'pf-linux-x64-32', 'pf-linux-x64-64'):
            for name, job in (('ci-post.yml', 'preflight'), ('ci.yml', 'build'),
                              ('unlisted.yml', 'build')):
                with self.subTest(label=label, workflow=name, job=job):
                    workflow = self.shard_workflow(job, 'release', 4, label)
                    self.assertIn(f'{name} must not use larger-runner labels; '
                                  'use the GCP fleet or hosted exceptions',
                                  ci_preflight.runner_findings({name: workflow}))
        # A workflow-level reference is not an exception just because a
        # correctly pinned job also mentions the same label.
        post = self.shard_workflow('build', 'release', 4, 'pf-linux-x64-16')
        post = post.replace('jobs:\n', 'env:\n  OTHER_RUNNER: pf-linux-x64-16\njobs:\n', 1)
        self.assertIn('ci-post.yml must not use larger-runner labels; '
                      'use the GCP fleet or hosted exceptions',
                      ci_preflight.runner_findings({'ci-post.yml': post}))

    def test_shard_hosted_jobs_refuse_legacy_fleet_metadata(self):
        for job, profile, count, label, _, _ in POST_SHARD_CASES:
            post = self.shard_workflow(job, profile, count, label)
            for scope in ('job', 'workflow'):
                with self.subTest(job=job, scope=scope):
                    changed = (post.replace('    steps:\n',
                               f"    env:\n      PF_CI_LABELS: '{POST_RUNNER}'\n    steps:\n", 1)
                               if scope == 'job' else post.replace('jobs:\n',
                               f"env:\n  PF_CI_LABELS: '{POST_RUNNER}'\njobs:\n", 1))
                    self.assertIn(f'ci-post.yml job {job} must not set PF_CI_LABELS on a GKE job',
                                  ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_shard_matrices_refuse_missing_duplicate_or_extra_indices(self):
        for job, profile, count, label, _, _ in POST_SHARD_CASES:
            post = self.shard_workflow(job, profile, count, label)
            indices = ', '.join(str(i) for i in range(1, count + 1))
            for replacement in (indices.rsplit(', ', 1)[0], indices + ', 1',
                                indices + f', {count + 1}', '0, ' + indices):
                with self.subTest(job=job, indices=replacement):
                    changed = in_job(post, job, f'shard: [{indices}]', f'shard: [{replacement}]')
                    self.assertIn(f'ci-post.yml {job} must select shards 1..{count} '
                                  f'for --ctest-shard denominator {count}',
                                  ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_shard_denominators_must_match_the_pinned_matrix(self):
        for job, profile, count, label, _, _ in POST_SHARD_CASES:
            post = self.shard_workflow(job, profile, count, label)
            flag = f'--ctest-shard ${{{{ matrix.shard }}}}/{count}'
            for spelling in (flag, flag.replace(' ', '=', 1),
                             flag.replace(' ', ' \\\n            ', 1)):
                source = (in_job(post, job, '      - run: ', '      - run: |\n          ')
                          if '\n' in spelling else post)
                spelled = in_job(source, job, flag, spelling)
                self.assertNotIn(f'ci-post.yml {job} must pass {flag} exactly once to ci_verify',
                                 ci_preflight.runner_findings({'ci-post.yml': spelled}))
                for replacement in ('', spelling.replace(f'/{count}', '/8'),
                                    '--ctest-shard 1/1', spelling + ' ' + spelling):
                    with self.subTest(job=job, spelling=spelling, flag=replacement):
                        changed = in_job(spelled, job, spelling, replacement)
                        self.assertIn(f'ci-post.yml {job} must pass {flag} exactly once to ci_verify',
                                      ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_new_shard_jobs_and_aggregates_refuse_timeout_keys(self):
        # The existing no-timeout control covers old job ids. These new ids
        # were previously outside JOB_COMMANDS, so main misses this finding.
        for job in ('build-debug', 'build-release-gate', 'build-debug-gate',
                    'sanitizers-gate', 'kernel-only-gate'):
            post = self.shard_workflow(job, 'debug', 2, 'ubuntu-24.04')
            for before, after in (
                    ('    steps:\n', '    timeout-minutes: 15\n    steps:\n'),
                    ('      - run:', '      - timeout-minutes: 15\n        run:')):
                with self.subTest(job=job, declaration=after):
                    changed = in_job(post, job, before, after)
                    self.assertIn(f'ci-post.yml job {job} must declare no timeout-minutes (owner #827)',
                                  ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_real_shard_matrices_equal_the_command_denominators(self):
        jobs = _jobs((ROOT / '.github/workflows/ci-post.yml').read_text())
        for job, _, count, label, aggregate, display in POST_SHARD_CASES:
            with self.subTest(job=job):
                self.assertIn(job, jobs, 'each profile must have a shard job')
                body = jobs[job]
                matrix = re.findall(r'^        shard: \[([0-9, ]+)\]$', body, re.MULTILINE)
                self.assertEqual(len(matrix), 1, f'{job} must have one explicit shard axis')
                indices = [int(value.strip()) for value in matrix[0].split(',')]
                denominators = re.findall(r'--ctest-shard \$\{\{ matrix.shard \}\}/(\d+)', body)
                self.assertEqual(denominators, [str(count)])
                self.assertEqual(indices, list(range(1, int(denominators[0]) + 1)))
                self.assertEqual(ci_preflight._job_values(body, 'runs-on'), [label])
                self.assertEqual(ci_preflight._job_values(jobs[aggregate], 'name'), [display])

    def test_leg_aggregate_contract_refuses_wrong_dependencies_or_green_skips(self):
        for job, _, _, _, aggregate, display in POST_SHARD_CASES:
            post = ('on: workflow_dispatch\njobs:\n'
                    f'  {aggregate}:\n    name: {display}\n'
                    f'    needs: [authz, changes, {job}]\n'
                    f'    if: {DOCS_ONLY_SKIP}\n'
                    '    runs-on: ubuntu-24.04\n    steps:\n'
                    + AUTHZ_REFUSE_STEP
                    + '      - name: Require every shard to have passed\n'
                    '        shell: bash\n        env:\n'
                    f'          SHARD_RESULT: ${{{{ needs.{job}.result }}}}\n'
                    '        run: |\n          set -euo pipefail\n'
                    '          printf \'%s\\n\' "shards: $SHARD_RESULT"\n'
                    '          [[ "$SHARD_RESULT" == "success" ]]\n')
            for before, after in (
                    (f'needs: [authz, changes, {job}]', 'needs: [authz, changes]'),
                    (f'needs.{job}.result', 'needs.other-leg.result'),
                    ('[[ "$SHARD_RESULT" == "success" ]]',
                     '[[ "$SHARD_RESULT" == "success" || "$SHARD_RESULT" == "skipped" ]]'),
                    (f'    if: {DOCS_ONLY_SKIP}\n', '    if: always()\n')):
                with self.subTest(aggregate=aggregate, replacement=after):
                    changed = in_job(post, aggregate, before, after)
                    self.assertIn(f'{aggregate} must require only its own complete shard matrix '
                                  'and keep its docs-only scheduling policy',
                                  ci_preflight.pipeline_workflow_findings(
                                      changed, 'ci-post.yml', pr=False))

    def test_real_leg_aggregates_refuse_every_non_success_result(self):
        post = (ROOT / '.github/workflows/ci-post.yml').read_text()
        for _, _, _, _, aggregate, _ in POST_SHARD_CASES:
            self.assertIn(aggregate, _jobs(post), 'each leg needs its own aggregate')
            script = job_script(post, aggregate, 'Require every shard to have passed')
            for outcome in ('success', 'failure', 'cancelled', 'skipped', ''):
                with self.subTest(aggregate=aggregate, result=outcome):
                    result = subprocess.run(
                        ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
                        env={'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'SHARD_RESULT': outcome},
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0 if outcome == 'success' else 1,
                                     result.stdout + result.stderr)
            # An absent dependency result is also a refusal under nounset.
            result = subprocess.run(
                ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
                env={'PATH': os.environ.get('PATH', '/usr/bin:/bin')},
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shards_build_cold_and_keep_unique_diagnostics(self):
        post = (ROOT / '.github/workflows/ci-post.yml').read_text()
        for job, profile, count, _, _, _ in POST_SHARD_CASES:
            with self.subTest(job=job):
                self.assertIn(job, _jobs(post), 'each profile must have a shard job')
                steps = ci_preflight._steps(_jobs(post)[job])
                self.assertNotIn('actions/cache', _jobs(post)[job])
                self.assertEqual(ci_preflight._step_named(steps, '      - name: Restore ccache'), '')
                self.assertEqual(ci_preflight._step_named(steps, '      - name: Save main ccache'), '')
                self.assertIn(f'shard-${{{{ matrix.shard }}}}-of-{count}', _jobs(post)[job])
                for reference in ('actions/cache/restore', 'actions/cache/save', 'actions/cache'):
                    changed = in_job(post, job, '    steps:\n', '    steps:\n'
                                     f'      - uses: {reference}@0057852bfaa89a56745cba8c7296529d2fc39830\n'
                                     '        with:\n          path: .ccache\n'
                                     f'          key: ccache-${{{{ runner.arch }}}}-{profile}\n')
                    self.assertIn(f'{job} must build cold without an Actions cache',
                                  ci_preflight.pipeline_workflow_findings(
                                      changed, 'ci-post.yml', pr=False))
                changed = in_job(post, job, f'-shard-${{{{ matrix.shard }}}}-of-{count}', '')
                self.assertIn(f'{job} must retain separate diagnostics for every shard',
                              ci_preflight.pipeline_workflow_findings(
                                  changed, 'ci-post.yml', pr=False))
                if job in ('sanitizers', 'kernel-only'):
                    for key in ('ccache-main', 'ccache-shared', '${{ github.sha }}',
                                '${{ hashFiles(matrix.shard) }}',
                                "${{ hashFiles('include/**', 'src/**') }}"):
                        with self.subTest(key=key):
                            changed = in_job(post, job, '    steps:\n', '    steps:\n'
                                             '      - uses: actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830\n'
                                             '        with:\n          path: .ccache\n'
                                             f'          key: {key}\n')
                            self.assertIn(f'{job} must build cold without an Actions cache',
                                          ci_preflight.pipeline_workflow_findings(
                                              changed, 'ci-post.yml', pr=False))


class PostLinuxBuildPolicy(unittest.TestCase):
    """Policy regressions whose inputs can also be constructed from main."""

    def post(self):
        return (ROOT / '.github/workflows/ci-post.yml').read_text()

    def arm_post(self):
        post = self.post()
        if 'build-arm' in _jobs(post):
            return post
        # Build a real arm job from the amd64 steps when testing the old tree.
        # This keeps the negative rows red at their policy assertion, rather
        # than failing to find a job that main did not yet have.
        arm = _jobs(post)['build']
        arm = arm.replace('name: build (${{ matrix.platform.os }},',
                          'name: build (ubuntu-24.04-arm,', 1)
        arm = arm.replace('        platform:\n          - os: ubuntu-24.04\n', '', 1)
        arm = arm.replace(f'    runs-on: {POST_RUNNER}\n',
                          '    runs-on: ubuntu-24.04-arm\n', 1)
        arm = arm.replace("    env:\n      PF_CI_LABELS: "
                          "'[\"self-hosted\",\"pf-ci\",\"pf-ci-x64-16\"]'\n", '', 1)
        arm = arm.replace('REQUESTED_PLATFORM: ${{ matrix.platform.os }}',
                          'REQUESTED_PLATFORM: ubuntu-24.04-arm', 1)
        arm = arm.replace('ubuntu-24.04:Linux|macos-26:macOS',
                          'ubuntu-24.04-arm:Linux', 1)
        arm = arm.replace('post fleet runner OS', 'runner OS', 1)
        arm = arm.replace('ci-diagnostics-${{ matrix.platform.os }}-',
                          'ci-diagnostics-ubuntu-24.04-arm-', 1)
        return post + '\n  build-arm:\n' + arm + '\n'

    def test_post_refuses_a_retired_macos_job(self):
        post = self.post()
        if 'build-macos' not in _jobs(post):
            post += ('\n  build-macos:\n    runs-on: macos-26\n'
                     '    steps:\n      - run: cmake --version\n')
        self.assertIn('ci-post.yml must keep only its pinned full-event jobs',
                      ci_preflight.pipeline_workflow_findings(post, 'ci-post.yml', pr=False))

    def test_post_gate_requires_the_arm_job(self):
        post = self.post().replace(', build-arm,', ',', 1)
        self.assertNotIn('build-arm', ci_preflight._needs(_jobs(post)['build-gate']))
        self.assertIn('build-gate must aggregate authz, every proof job, preflight and changes',
                      ci_preflight.pipeline_workflow_findings(post, 'ci-post.yml', pr=False))

    def test_post_arm_refuses_a_non_arm_runner(self):
        post = self.arm_post()
        for runner in ('ubuntu-24.04', POST_RUNNER, 'ubuntu-latest', 'ubuntu-22.04-arm'):
            with self.subTest(runner=runner):
                changed = in_job(post, 'build-arm', runner_declaration(GKE_ARM),
                                 f'    runs-on: {runner}\n')
                self.assertNotEqual(changed, post)
                self.assertIn(f'ci-post.yml job build-arm must run on {GKE_ARM}',
                              ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_post_arm_requires_both_build_types(self):
        post = self.arm_post()
        for build_type in ('Release', 'Debug'):
            with self.subTest(remaining=build_type):
                changed = in_job(post, 'build-arm', '        build_type: [Release, Debug]\n',
                                 f'        build_type: [{build_type}]\n')
                self.assertNotEqual(changed, post)
                self.assertIn('ci-post.yml build-arm must select only Linux arm64 '
                              'Release/Debug cells on full events',
                              ci_preflight.runner_findings({'ci-post.yml': changed}))

    def test_post_arm_keeps_the_amd64_steps(self):
        post = self.arm_post()
        for before, after in (('libeigen3-dev tzdata-legacy ccache strace',
                               'libeigen3-dev ccache strace'),
                              ('--require-hashes', '--no-deps'),
                              ('PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY: \'0\'',
                               'PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY: \'1\''),
                              ('ci-diagnostics-ubuntu-24.04-arm-', 'ci-diagnostics-ubuntu-24.04-')):
            with self.subTest(replacement=after):
                changed = in_job(post, 'build-arm', before, after)
                self.assertNotEqual(changed, post)
                self.assertIn('build-arm must keep the same verification steps as build',
                              ci_preflight.pipeline_workflow_findings(changed, 'ci-post.yml', pr=False))

    def test_release_prebuilt_refuses_retired_macos_steps(self):
        release = (ROOT / '.github/workflows/release.yml').read_text()
        for step in ("      - name: Install Eigen3 (macOS)\n"
                     "        if: runner.os == 'macOS'\n"
                     "        run: brew install eigen\n",
                     "      - name: Configure universal archive\n"
                     "        run: cmake -B build -DCMAKE_OSX_ARCHITECTURES=arm64\n"):
            with self.subTest(step=step):
                changed = in_job(release, 'prebuilt', '    steps:\n', '    steps:\n' + step)
                self.assertNotEqual(changed, release)
                self.assertIn('release.yml prebuilt must not contain macOS build steps',
                              ci_preflight.runner_findings({'release.yml': changed}))


class ActionPins(unittest.TestCase):
    SHA = '0123456789abcdef0123456789abcdef01234567'
    WORKFLOW = '.github/workflows/pinlint.yml'
    ACTION = '.github/actions/setup-live-curl/action.yml'

    @staticmethod
    def workflow(value, *, named=False):
        steps = ('      - name: External action\n'
                 f'        uses: {value}\n' if named else f'      - uses: {value}\n')
        return ('on: workflow_dispatch\njobs:\n  inspect:\n'
                '    runs-on: ["self-hosted","pf-gcp-post"]\n'
                '    steps:\n' + steps)

    @staticmethod
    def finding(path, line, reference):
        return (f'{path}:{line}: third-party uses {reference!r} must be pinned to a '
                '40-character lowercase hex SHA with a trailing # v<version> comment')

    def pin_findings(self, text, *, composite=False, checker=None):
        # Use the existing contract entrypoint, so main fails for a missing
        # finding, not an import error for a newly introduced helper.
        sources = [(ROOT / path).read_text() for path in CI_SOURCES]
        checker = ci_workflow_findings if checker is None else checker
        if composite:
            findings = checker(*sources, curl_action=text)
            path = self.ACTION
        else:
            findings = checker(*sources, others={Path(self.WORKFLOW).name: text})
            path = self.WORKFLOW
        return [finding for finding in findings if finding.startswith(path + ':')]

    def assert_refused(self, value, reference, *, named=False, checker=None):
        self.assertEqual(
            self.pin_findings(self.workflow(value, named=named), checker=checker),
            [self.finding(self.WORKFLOW, 7 if named else 6, reference)])

    def test_mutable_tag_is_refused(self):
        self.assert_refused('actions/checkout@v4 # v4.2.2', 'actions/checkout@v4')

    def test_branch_ref_is_refused(self):
        self.assert_refused('actions/checkout@main # v4.2.2', 'actions/checkout@main',
                            named=True)

    def test_short_sha_is_refused(self):
        self.assert_refused('actions/checkout@0123456 # v4.2.2', 'actions/checkout@0123456')

    def test_sha_without_same_line_version_is_refused(self):
        reference = 'actions/checkout@' + self.SHA
        for value in (reference, reference + '\n        # v4.2.2'):
            with self.subTest(value=value):
                self.assert_refused(value, reference)

    def test_malformed_version_comments_are_refused(self):
        reference = 'actions/checkout@' + self.SHA
        for comment in ('# 4.2.2', '# vfour', '# v4..2', '# v4.2.',
                        '# v4.2.2 extra', '# v4.2.2-', '# v4.2.2+'):
            with self.subTest(comment=comment):
                self.assert_refused(reference + ' ' + comment, reference)

    def test_uppercase_sha_is_refused(self):
        reference = 'actions/checkout@' + self.SHA.upper()
        self.assert_refused(reference + ' # v4.2.2', reference)

    def test_quoted_mutable_values_are_refused(self):
        for quote in ('"', "'"):
            for named in (False, True):
                with self.subTest(quote=quote, named=named):
                    self.assert_refused(f'{quote}actions/checkout@v4{quote} # v4.2.2',
                                        'actions/checkout@v4', named=named)

    def test_reference_without_ref_is_refused(self):
        self.assert_refused('actions/checkout # v4.2.2', 'actions/checkout')

    def test_remote_reusable_workflow_is_refused(self):
        reference = 'example/shared/.github/workflows/build.yml@main'
        text = ('on: workflow_dispatch\njobs:\n  call:\n'
                f'    uses: {reference} # v1.2.3\n')
        self.assertEqual(self.pin_findings(text),
                         [self.finding(self.WORKFLOW, 4, reference)])

    def test_composite_step_is_refused(self):
        reference = 'actions/cache/restore@v4'
        text = ('name: Example\nruns:\n  using: composite\n  steps:\n'
                f'    - uses: {reference} # v4.2.2\n')
        self.assertEqual(self.pin_findings(text, composite=True),
                         [self.finding(self.ACTION, 5, reference)])

    @staticmethod
    def unsupported_finding(path, line, text, construct='flow mappings'):
        return (f'{path}:{line}: unsupported YAML construct in a workflow ({construct}): '
                f'rewrite it in plain block style; {text!r}')

    def assert_unsupported_step(self, step, *, composite=False, checker=None,
                                construct='flow mappings'):
        if composite:
            original = (ROOT / self.ACTION).read_text()
            self.assertTrue(original.endswith('\n'))
            number = len(original.splitlines()) + 1
            text = original + '    ' + step + '\n'
            path = self.ACTION
        else:
            original = self.workflow('actions/checkout@main')
            text = original.replace('- uses: actions/checkout@main', step, 1)
            self.assertNotEqual(text, original)
            number, path = 6, self.WORKFLOW
        self.assertEqual(self.pin_findings(text, composite=composite, checker=checker),
                         [self.unsupported_finding(path, number, step, construct)])

    def assert_yaml_refusals(self, text, rows):
        # The public checker already exists on f4a03dd712: a red result
        # is a behavioral mismatch, never a missing-helper/import failure.
        for path in (self.WORKFLOW, self.ACTION):
            with self.subTest(path=path):
                expected = [self.unsupported_finding(path, number, line, construct)
                            for number, construct, line in rows]
                self.assertEqual(ci_preflight.action_pin_findings({path: text}), expected)

    def test_escaped_quoted_keys_are_refused(self):
        for line in (r'- "\u0075ses": actions/checkout@main',
                     r'- "\x75ses": actions/checkout@main',
                     r'- "\U00000075ses": actions/checkout@main'):
            with self.subTest(line=line):
                self.assert_yaml_refusals(line + '\n', [(1, 'quoted keys', line)])

    def test_all_quoted_keys_are_refused(self):
        for key in ('"uses"', "'uses'", '"steps"', "'permissions'"):
            line = f'- {key}: actions/checkout@{self.SHA} # v4.4.0'
            with self.subTest(key=key):
                self.assert_yaml_refusals(line + '\n', [(1, 'quoted keys', line)])

    def test_explicit_keys_are_refused(self):
        text = '- ? uses\n  : actions/checkout@main\n'
        self.assert_yaml_refusals(text, [
            (1, 'explicit keys', '- ? uses'),
            (2, 'explicit keys', ': actions/checkout@main')])

    def test_anchors_are_refused_without_a_uses_key(self):
        line = 'steps: &checkout'
        self.assert_yaml_refusals(line + '\n', [(1, 'anchors', line)])

    def test_aliases_are_refused_without_a_uses_key(self):
        line = '- *checkout'
        self.assert_yaml_refusals(line + '\n', [(1, 'aliases', line)])

    def test_merge_keys_are_refused(self):
        line = '<<: *checkout'
        self.assert_yaml_refusals(line + '\n', [(1, 'merge keys', line)])

    def test_tags_are_refused(self):
        for tag in ('!!map', '!custom', '!<tag:yaml.org,2002:map>'):
            line = 'steps: ' + tag
            with self.subTest(tag=tag):
                self.assert_yaml_refusals(line + '\n', [(1, 'tags', line)])

    def test_anchored_multiline_mapping_is_refused(self):
        text = ('steps:\n'
                '  - &checkout {\n'
                f'      uses: actions/checkout@{self.SHA} # v4.4.0\n'
                '    }\n'
                '  - *checkout\n')
        self.assert_yaml_refusals(text, [
            (2, 'anchors', '- &checkout {'),
            (5, 'aliases', '- *checkout')])

    def test_anchored_run_scalar_is_refused_without_scanning_its_body(self):
        text = ("steps:\n  - run: &script |\n"
                "      printf '%s\\n' '\n"
                "      uses: actions/checkout@main\n"
                "      '\n")
        self.assert_yaml_refusals(text, [(2, 'anchors', '- run: &script |')])

    def test_flow_mappings_without_uses_are_refused(self):
        for line in ('env: {CHECKOUT: main}', '- {name: Checkout}', '- {}',
                     'env: {} trailing', '- { }', 'labels: [{}]',
                     'labels: [name: Checkout]'):
            with self.subTest(line=line):
                self.assert_yaml_refusals(line + '\n', [(1, 'flow mappings', line)])
        self.assert_yaml_refusals('env: {\n  CHECKOUT: main\n}\n',
                                 [(1, 'flow mappings', 'env: {')])

    def test_reported_flow_continuation_resumes_before_mutable_action(self):
        path = '.github/workflows/ci.yml'
        text = ('env: {\n'
                '  A: b", } # "\n'
                'steps:\n'
                '  - uses: actions/checkout@main\n')
        self.assertEqual(ci_preflight.action_pin_findings({path: text}), [
            self.unsupported_finding(path, 1, 'env: {'),
            self.finding(path, 4, 'actions/checkout@main')])

    def test_refused_flow_continuations_count_delimiters_despite_plain_quotes(self):
        cases = (
            ('env: {', 'A: b", } # "'),
            ('env: {', "A: b', } # '"),
            ('env: {', 'A: [b", linux]} # "'),
            ('env: {', "A: [b', {B: value}]} # '"),
            ('options: [{', 'A: b", }] # "'),
            ('options: [[{', "A: b', }]] # '"),
        )
        for opening, continuation in cases:
            text = ('steps:\n  - ' + opening + '\n'
                    '      ' + continuation + '\n'
                    '    uses: actions/checkout@main\n')
            for path in (self.WORKFLOW, self.ACTION):
                with self.subTest(opening=opening, continuation=continuation, path=path):
                    # The uses key is deeper than the opening line: outdent
                    # recovery alone cannot make this assertion pass.
                    self.assertEqual(ci_preflight.action_pin_findings({path: text}), [
                        self.unsupported_finding(path, 2, '- ' + opening),
                        self.finding(path, 4, 'actions/checkout@main')])

    def test_refused_flow_continuations_end_at_outdent_even_with_unbalanced_depth(self):
        for prefix, indent in (('', ''), ('options:\n', '  ')):
            opening = indent + 'env: {'
            # The quoted { adds depth in the conservative scan; the embedded
            # quote after b masks the real closer on the old implementation.
            continuation = indent + '  A: "{", B: b", } # "'
            for following, uses_line in (
                    (indent + 'uses: actions/checkout@main\n', 3),
                    (indent + 'uses: actions/checkout@main # }\n', 3),
                    ('steps:\n  - uses: actions/checkout@main\n', 4)):
                text = prefix + opening + '\n' + continuation + '\n' + following
                offset = 1 if prefix else 0
                for path in (self.WORKFLOW, self.ACTION):
                    with self.subTest(prefix=prefix, following=following, path=path):
                        self.assertEqual(ci_preflight.action_pin_findings({path: text}), [
                            self.unsupported_finding(path, 1 + offset, opening.strip(' ')),
                            self.finding(path, uses_line + offset, 'actions/checkout@main')])

    def test_empty_sequences_require_a_whole_plain_key_value(self):
        for line in ('- []', 'labels: [[]]', 'labels: [] trailing'):
            with self.subTest(line=line):
                self.assert_yaml_refusals(line + '\n', [(1, 'empty collections', line)])

    def test_properties_inside_flow_sequences_are_refused(self):
        for value, construct in (('&runner linux', 'anchors'), ('*runner', 'aliases'),
                                 ('!!str linux', 'tags'), ('? uses', 'explicit keys')):
            line = 'runs-on: [' + value + ']'
            with self.subTest(value=value):
                self.assert_yaml_refusals(line + '\n', [(1, construct, line)])

    def test_flow_sequence_quotes_cannot_mask_mappings_or_comments(self):
        for quote in ('"', "'"):
            line = (f'options: [a{quote}, {{uses: actions/checkout@main}}] '
                    f'# {quote}]')
            with self.subTest(quote=quote):
                self.assert_yaml_refusals(line + '\n', [(1, 'flow mappings', line)])
                code, visible, _ = ci_preflight._action_yaml_visible(
                    line.split(': ', 1)[1])
                self.assertEqual(code, line.split(': ', 1)[1].split(' # ', 1)[0])
                self.assertIn('{uses: actions/checkout@main}', visible)

    def test_quotes_in_flow_sequences_must_start_an_element(self):
        for value in ('[a"b"]', "[a'b']", '["a" "b"]', "['a' 'b']",
                      '[linux, [a"b"]]'):
            line = 'options: ' + value
            with self.subTest(value=value):
                self.assert_yaml_refusals(line + '\n', [(1, 'quoted scalars', line)])
        for first in ('options: [', 'options: [linux,', 'options: [linux'):
            line = 'a"b"]'
            with self.subTest(first=first):
                self.assert_yaml_refusals(first + '\n  ' + line + '\n',
                                         [(2, 'quoted scalars', line)])

    def test_flow_quotes_at_element_start_preserve_data_and_scan_resumption(self):
        text = ('runs-on: ["self-hosted", "pf-gcp-pr"]\n'
                'branches: [main]\n'
                'needs: [a, b]\n'
                'options: ["a # {uses: actions/checkout@main}",\n'
                "  ['it''s # data', linux]] # actual comment\n")
        self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [])
        text += '- uses: actions/upload-artifact@main\n'
        self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [
            self.finding(self.WORKFLOW, 6, 'actions/upload-artifact@main')])

    def test_unicode_whitespace_at_line_start_is_refused(self):
        spaces = ('\u0085', '\u00a0', '\u1680', '\u2000', '\u2001', '\u2002',
                  '\u2003', '\u2004', '\u2005', '\u2006', '\u2007', '\u2008',
                  '\u2009', '\u200a', '\u2028', '\u2029', '\u202f', '\u205f',
                  '\u3000', '\ufeff')
        for space in spaces:
            line = space + '#hidden: {uses: actions/checkout@main}'
            construct = (f'unsupported line break U+{ord(space):04X}'
                         if space in '\u0085\u2028\u2029' else 'unsupported whitespace')
            for prefix in ('', 'name: Example\n'):
                with self.subTest(space=repr(space), prefix=prefix):
                    self.assert_yaml_refusals(
                        prefix + line + '\n',
                        [(1 if not prefix else 2, construct, line)])

    def test_unicode_whitespace_around_separators_is_refused(self):
        for space in ('\u0085', '\u00a0', '\u2028', '\u2029', '\ufeff'):
            construct = (f'unsupported line break U+{ord(space):04X}'
                         if space in '\u0085\u2028\u2029' else 'unsupported whitespace')
            for line in (f'name{space}: Example', f'name:{space}Example',
                         f'name: Example{space}#hidden: {{uses: actions/checkout@main}}',
                         f'name: Example #{space}hidden: {{uses: actions/checkout@main}}',
                         f'name: Example{space}: hidden', f'name: Example:{space}hidden'):
                with self.subTest(space=repr(space), line=line):
                    self.assert_yaml_refusals(line + '\n', [(1, construct, line)])

    def test_unsupported_line_breaks_in_block_scalars_refuse_the_whole_file(self):
        for separator in ('\r', '\u0085', '\u2028', '\u2029', '\v', '\f'):
            for prefix, body, following, number in (
                    ('steps:\n  - run: |\n', '      echo ok',
                     '  - uses: actions/checkout@main', 3),
                    ('steps:\n  - run: |\n', '     ',
                     '  - uses: actions/checkout@main', 3),
                    ('name: Example\nruns:\n  using: composite\n  steps:\n'
                     '    - shell: bash\n      run: |\n', '        echo ok',
                     '    - uses: actions/checkout@main', 7)):
                line = body + separator + following
                with self.subTest(separator=repr(separator), prefix=prefix, body=body):
                    self.assert_yaml_refusals(prefix + line + '\n', [
                        (number, f'unsupported line break U+{ord(separator):04X}',
                         line.strip(' \t'))])

    def test_unsupported_line_breaks_in_comments_refuse_the_whole_file(self):
        for separator in ('\r', '\u0085', '\u2028', '\u2029', '\v', '\f'):
            line = '  # comment' + separator + '  - uses: actions/checkout@main'
            with self.subTest(separator=repr(separator)):
                self.assert_yaml_refusals('steps:\n' + line + '\n', [
                    (2, f'unsupported line break U+{ord(separator):04X}',
                     line.strip(' \t'))])

    def test_c0_controls_in_block_scalars_and_comments_refuse_the_whole_file(self):
        for codepoint in range(0x20):
            if codepoint in (0x09, 0x0a, 0x0b, 0x0c, 0x0d):
                continue  # TAB/LF/CRLF controls and the six line breaks have separate rows.
            control = chr(codepoint)
            for prefix, line, number in (
                    ('run: |\n', '  echo ' + control + 'ok', 2),
                    ('name: Example\n', '# comment ' + control + 'text', 2)):
                with self.subTest(codepoint=codepoint, prefix=prefix):
                    self.assert_yaml_refusals(prefix + line + '\n', [
                        (number, f'unsupported control character U+{codepoint:04X}',
                         line.strip(' \t'))])

    def test_file_character_refusal_reports_only_the_first_offense_before_scanning(self):
        first = '  echo \x01ok'
        text = ('uses: actions/checkout@main\r\n'
                'run: |\r\n' + first + '\r\n'
                '# later \u2028\n')
        for path, other in ((self.WORKFLOW, self.ACTION), (self.ACTION, self.WORKFLOW)):
            with self.subTest(path=path):
                self.assertEqual(ci_preflight.action_pin_findings({
                    path: text, other: 'uses: actions/upload-artifact@main\n'}), [
                        self.unsupported_finding(
                            path, 3, first.strip(' '), 'unsupported control character U+0001'),
                        self.finding(other, 1, 'actions/upload-artifact@main')])

    def test_crlf_block_scalar_resumption_and_non_ascii_text_remain_supported(self):
        text = ('steps:\n'
                '  - run: |\n'
                '      echo café — →\tdata\n'
                '      uses: actions/checkout@main\n'
                '  # café — →\n'
                f'  - uses: actions/upload-artifact@{self.SHA} # v4.4.0\n')
        for newline in ('\n', '\r\n'):
            for path in (self.WORKFLOW, self.ACTION):
                with self.subTest(newline=newline, path=path):
                    sources = {path: text.replace('\n', newline)}
                    self.assertEqual(ci_preflight.action_pin_findings(sources), [])
                    changed = sources[path].replace(
                        f'actions/upload-artifact@{self.SHA}', 'actions/upload-artifact@main', 1)
                    self.assertNotEqual(changed, sources[path])
                    self.assertEqual(ci_preflight.action_pin_findings({path: changed}), [
                        self.finding(path, 6, 'actions/upload-artifact@main')])

    def test_unterminated_expressions_with_many_quotes_are_refused(self):
        # Thread-free, with no timing threshold: the large input cannot finish
        # in normal test flow with the old exponentially backtracking regex.
        for count in (100, 5000, 200000):
            line = 'name: ${{' + "'" * count
            with self.subTest(count=count):
                self.assert_yaml_refusals(line + '\n', [(1, 'expressions', line)])

    def test_expression_strings_keep_delimiters_and_doubled_quotes_opaque(self):
        text = ("name: ${{ format('it''s # }} uses: {0}', 'ignored') }}\n"
                "options: [${{ format('}} # uses: {0}', 'ignored') }}, linux]\n")
        self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [])
        text += '- uses: actions/checkout@main\n'
        self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [
            self.finding(self.WORKFLOW, 3, 'actions/checkout@main')])

    def test_backslashes_in_yaml_quoted_values_are_refused(self):
        for line in (r'name: "line\nbreak"', r"name: 'line\nbreak'",
                     r'runs-on: ["self\u002dhosted", linux]'):
            with self.subTest(line=line):
                self.assert_yaml_refusals(line + '\n', [(1, 'quoted escapes', line)])

    def test_quoted_values_must_end_on_their_line(self):
        for line in ('name: "unfinished', "name: 'finished' trailing", 'labels: ["unfinished'):
            with self.subTest(line=line):
                self.assert_yaml_refusals(line + '\n', [(1, 'quoted scalars', line)])

    def test_flow_sequence_boundaries_are_refused(self):
        for line in ('labels: [linux][windows]', 'labels: [linux] [windows]',
                     'labels: [linux] trailing', 'labels: [[linux] windows]',
                     'labels: [linux,'):
            with self.subTest(line=line):
                self.assert_yaml_refusals(line + '\n', [(1, 'flow sequences', line)])

    def test_tab_indentation_is_refused(self):
        for prefix in ('\t', ' \t', '\t  '):
            line = prefix + f'- uses: actions/checkout@{self.SHA} # v4.4.0'
            with self.subTest(prefix=prefix):
                self.assert_yaml_refusals(line + '\n',
                                         [(1, 'tab indentation', line.strip())])
        line = f'-\tuses: actions/checkout@{self.SHA} # v4.4.0'
        self.assert_yaml_refusals(line + '\n', [(1, 'tab indentation', line)])
        self.assert_yaml_refusals('\t\n', [(1, 'tab indentation', '')])

    def test_workflow_document_markers_are_refused(self):
        for marker in ('---', '...', '--- # another document', '... # end'):
            text = 'name: Example\n' + marker + '\n'
            with self.subTest(marker=marker):
                self.assert_yaml_refusals(text, [(2, 'document markers', marker)])

    def test_uses_requires_a_same_line_scalar(self):
        for value in ('', '|', '>-', '{}', '[]', '[./local]'):
            line = '- uses:' + (' ' + value if value else '')
            with self.subTest(value=value):
                self.assert_yaml_refusals(line + '\n', [(1, 'uses keys', line)])

    def test_unrecognised_block_lines_are_refused(self):
        line = '%YAML 1.2'
        self.assert_yaml_refusals(line + '\n', [(1, 'block structure', line)])

    def test_supported_yaml_values_and_collections_remain_accepted(self):
        for value in ('{}', '{ }', '{ \t}', '[]', '[ ]', '[\t ]',
                      '[linux, [windows, macos]]'):
            with self.subTest(value=value):
                self.assertEqual(ci_preflight.action_pin_findings(
                    {self.WORKFLOW: 'options: ' + value + ' # supported value\n'}), [])
        text = ('permissions: {} # no permissions\n'
                'options: [] # no options\n'
                'branches: [main]\n'
                'runs-on: ["self-hosted", "pf-gcp-pr"]\n'
                "cron: '30 1 * * *'\n"
                "paths-ignore: ['.github/**']\n"
                'shell: python3 {0}\n'
                "run: printf '%s\\n' \"${NEXT}\" > VERSION\n"
                'group: prefix-${{ github.workflow }}-${{ github.run_id }}\n'
                "if: ${{ !cancelled() && contains('uses: & ! {', 'x') }}\n"
                'labels: [linux,\n'
                '  "self-hosted"]\n'
                'steps:\n'
                f'  - uses: "actions/checkout@{self.SHA}" # v4.4.0\n')
        for newline in ('\n', '\r\n'):
            with self.subTest(newline=newline):
                sources = {self.WORKFLOW: text.replace('\n', newline)}
                self.assertEqual(ci_preflight.action_pin_findings(sources), [])
                changed = sources[self.WORKFLOW].replace(
                    f'actions/checkout@{self.SHA}', 'actions/checkout@main', 1)
                self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: changed}), [
                    self.finding(self.WORKFLOW, 14, 'actions/checkout@main')])

    def test_script_and_other_block_scalar_bodies_are_excluded(self):
        for key in ('run', 'script', 'path', 'restore-keys'):
            for indicator in ('|+', '|-', '>2-', '|2+', '>+2'):
                with self.subTest(key=key, indicator=indicator):
                    text = (f'with:\n  {key}: {indicator}\n'
                            '    - "\\u0075ses": actions/checkout@main\n'
                            '    ? uses\n'
                            '    <<: *checkout\n'
                            '    steps: &checkout {uses: actions/checkout@main}\n'
                            '    ---\n'
                            '    \t!!str uses: actions/checkout@main\n'
                            '  next: value\n')
                    self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [])
                    text += '- uses: actions/upload-artifact@main\n'
                    self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [
                        self.finding(self.WORKFLOW, 10, 'actions/upload-artifact@main')])

    def test_sequence_item_block_scalar_bodies_are_excluded(self):
        for indicator in ('|', '>-', '|2+', '|+2', '>2-'):
            text = (f'args:\n  - {indicator}\n'
                    '    uses: actions/checkout@main\n')
            with self.subTest(indicator=indicator):
                self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [])
                text += '  - uses: actions/upload-artifact@main\n'
                self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [
                    self.finding(self.WORKFLOW, 4, 'actions/upload-artifact@main')])

    def test_sequence_mapping_block_scalar_resumes_at_key_indent(self):
        for key in ('run', 'script'):
            for indicator in ('|', '>-', '|2+'):
                text = (f'args:\n  - {key}: {indicator}\n'
                        '      uses: actions/checkout@main\n'
                        '    uses: actions/upload-artifact@main\n')
                with self.subTest(key=key, indicator=indicator):
                    self.assertEqual(ci_preflight.action_pin_findings({self.WORKFLOW: text}), [
                        self.finding(self.WORKFLOW, 4, 'actions/upload-artifact@main')])

    def test_flow_mapping_mutable_steps_are_refused(self):
        steps = (
            '- {uses: actions/checkout@main}',
            '- { uses : actions/checkout@main }',
            '- {"uses": "actions/checkout@main"}',
            "- { 'uses' : 'actions/checkout@main' }",
            '- {"uses":"actions/checkout@main"}',
            '- {name: External action, uses: actions/checkout@main}',
            '- {name: External action, "uses" : actions/checkout@main, with: {fetch-depth: 0}}',
            "- {with: {fetch-depth: 0}, 'uses': actions/checkout@main}",
            '- name: External action, uses: actions/checkout@main',
            "- name: External action, 'uses' : actions/checkout@main",
            '- uses: ./.github/actions/local, uses: actions/checkout@main',
            '- uses: docker://alpine:3.20, "uses": actions/checkout@main',
            '- {&key "uses": actions/checkout@main}',
            '- {!!str uses: actions/checkout@main}',
        )
        for composite in (False, True):
            for step in steps:
                with self.subTest(composite=composite, step=step):
                    construct = ('flow mappings' if step.startswith('- {')
                                 else 'inline mapping keys')
                    self.assert_unsupported_step(step, composite=composite, construct=construct)

    def test_pinned_flow_mapping_is_refused_as_unsupported(self):
        step = f'- {{uses: actions/checkout@{self.SHA}}} # v4.4.0'
        for composite in (False, True):
            with self.subTest(composite=composite):
                self.assert_unsupported_step(step, composite=composite)

    def test_multiline_flow_mapping_is_refused_as_unsupported(self):
        line = f'uses: actions/checkout@{self.SHA} # v4.4.0'
        original = self.workflow('actions/checkout@main')
        text = original.replace('- uses: actions/checkout@main',
                                '- {\n          ' + line + '\n        }', 1)
        self.assertNotEqual(text, original)
        self.assertEqual(self.pin_findings(text),
                         [self.unsupported_finding(self.WORKFLOW, 6, '- {')])

    def test_flow_mapping_reusable_workflow_is_refused(self):
        line = 'call: {uses: example/shared/.github/workflows/build.yml@main}'
        text = 'on: workflow_dispatch\njobs:\n  ' + line + '\n'
        self.assertEqual(self.pin_findings(text),
                         [self.unsupported_finding(self.WORKFLOW, 3, line)])

    def test_run_script_uses_text_is_not_a_yaml_key(self):
        for indicator in ('|', '|-', '|+', '|2', '>', '>-', '>+', '>2-'):
            with self.subTest(indicator=indicator):
                script = (f'      - run: {indicator}\n'
                          "          printf '%s\\n' '\n"
                          '          uses: actions/checkout@main\n'
                          "          '\n"
                          '          # {uses: actions/checkout@main}\n'
                          '          echo "uses: actions/checkout@main"\n')
                text = self.workflow('actions/checkout@main').replace(
                    '      - uses: actions/checkout@main\n', script, 1)
                self.assertEqual(self.pin_findings(text), [])
                number = len(text.splitlines()) + 1
                text += '      - uses: actions/upload-artifact@main\n'
                self.assertEqual(self.pin_findings(text), [
                    self.finding(self.WORKFLOW, number, 'actions/upload-artifact@main')])
        # Quoted YAML values and comments must not expose apparent flow keys.
        text = ('on: workflow_dispatch\njobs:\n  inspect:\n'
                '    runs-on: ["self-hosted","pf-gcp-post"]\n    steps:\n'
                '      - name: "Example, uses: actions/checkout@main"\n'
                '        run: \'echo "{uses: actions/checkout@main}"\'\n'
                '      - name: A plain scalar mentioning uses:literal\n'
                '        run: "true"\n'
                '      # - {"uses": actions/checkout@main}\n')
        self.assertEqual(self.pin_findings(text), [])

    def test_removing_unsupported_shape_branch_is_detected(self):
        step = '- {uses: actions/checkout@main}'
        sources = {self.WORKFLOW: self.workflow('actions/checkout@main').replace(
            '- uses: actions/checkout@main', step, 1)}
        expected = [self.unsupported_finding(self.WORKFLOW, 6, step)]
        self.assertEqual(ci_preflight.action_pin_findings(sources), expected)
        source = inspect.getsource(ci_preflight.action_uses)
        start = source.index('            if unsupported is not None:\n')
        end = source.index("            if not key or key.group(1) != 'uses':\n", start)
        guard = source[start:end]
        self.assertEqual(source.count(guard), 1)
        mutant = source.replace(guard, '', 1)
        namespace = dict(ci_preflight.action_pin_findings.__globals__)
        exec(compile(mutant, '<unsupported-action-shape-check-removed>', 'exec'), namespace)
        exec(compile(inspect.getsource(ci_preflight.action_pin_findings),
                     '<action-pin-check-with-mutant-extractor>', 'exec'), namespace)
        # Exercise real extraction and validation with the refusal branch removed.
        with self.assertRaises(AssertionError):
            self.assertEqual(namespace['action_pin_findings'](sources), expected)

    def test_every_offending_line_is_reported(self):
        text = self.workflow('actions/checkout@main # v4.2.2')
        text += '      - uses: actions/upload-artifact@v4 # v4.6.2\n'
        self.assertEqual(self.pin_findings(text), [
            self.finding(self.WORKFLOW, 6, 'actions/checkout@main'),
            self.finding(self.WORKFLOW, 7, 'actions/upload-artifact@v4'),
        ])

    def test_pinned_quoted_and_local_shapes_are_accepted(self):
        for named in (False, True):
            for quote in ('', '"', "'"):
                for version in ('v4', 'v4.2.2', 'v1.12.0', 'v4.2.2-rc.1',
                                'v4.2.2+build.7', 'v4.2.2-rc.1+build.7'):
                    value = f'{quote}actions/checkout@{self.SHA}{quote} # {version}'
                    with self.subTest(value=value, named=named):
                        self.assertEqual(self.pin_findings(self.workflow(value, named=named)), [])
                for reference in ('./.github/actions/x', './.github/workflows/y.yml',
                                  'docker://alpine:3.20'):
                    with self.subTest(reference=reference, quote=quote, named=named):
                        value = quote + reference + quote
                        text = self.workflow(value, named=named)
                        text += '      # - uses: actions/checkout@main\n'
                        self.assertEqual(self.pin_findings(text), [])

    @staticmethod
    def run_contract(tree):
        output = io.StringIO()
        with mock.patch.object(ci_preflight, 'ROOT', tree), \
                mock.patch.object(sys, 'argv', ['ci_preflight.py', '--check-ci-workflow']), \
                contextlib.redirect_stdout(output):
            code = ci_preflight.main()
        return code, output.getvalue()

    def test_nested_action_yml_and_yaml_are_discovered(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            # Copy data inputs only; the real, already imported checker runs.
            shutil.copytree(ROOT / '.github/workflows', tree / '.github/workflows')
            shutil.copytree(ROOT / '.github/actions', tree / '.github/actions')
            (tree / 'tests').mkdir()
            shutil.copyfile(ROOT / 'tests/CMakeLists.txt', tree / 'tests/CMakeLists.txt')
            paths = ('.github/actions/nested/alpha/action.yml',
                     '.github/actions/nested/beta/action.yaml')
            reference = 'actions/cache/save@main'
            for name in paths:
                path = tree / name
                path.parent.mkdir(parents=True)
                path.write_text('name: Example\nruns:\n  using: composite\n  steps:\n'
                                f'    - uses: {reference} # v4.2.2\n')
            code, output = self.run_contract(tree)
            self.assertEqual(
                [line for line in output.splitlines()
                 if any(line.startswith(name + ':') for name in paths)],
                [self.finding(name, 5, reference) for name in paths])
            self.assertEqual(code, 1, output)

    def test_removing_pin_check_is_detected(self):
        value, reference = 'actions/checkout@main # v4.2.2', 'actions/checkout@main'
        self.assert_refused(value, reference)
        source = inspect.getsource(ci_workflow_findings)
        guard = '    findings += action_pin_findings(action_sources)\n'
        self.assertEqual(source.count(guard), 1)
        mutant = source.replace(guard, '', 1)
        namespace = dict(ci_workflow_findings.__globals__)
        exec(compile(mutant, '<action-pin-check-removed>', 'exec'), namespace)
        # Delete the actual integration call, then apply the same regression
        # assertion to that function. No checker result is stubbed.
        with self.assertRaises(AssertionError):
            self.assert_refused(value, reference, checker=namespace['ci_workflow_findings'])

    # Expected to fail with the new check on base c82fd00e until the separate
    # action-pins lane lands. Keep this a normal test, never an xfail or skip.
    def test_real_tree_has_only_versioned_action_pins(self):
        self.assert_refused('actions/checkout@main # v4.2.2', 'actions/checkout@main')
        for tier in ('all', 'fast', 'github-only'):
            plan = {entry[0]: entry for entry in check_commands(ROOT, tier=tier)}
            self.assertEqual(plan['ci-workflow-contract'], (
                'ci-workflow-contract',
                [sys.executable, str(ROOT / 'scripts/ci_preflight.py'), '--check-ci-workflow']))
        code, output = self.run_contract(ROOT)
        self.assertEqual(code, 0, output)


class CiFileLoading(unittest.TestCase):
    """The stages read tracked CI files as stored: bytes, strict UTF-8, no newline translation.

    Every test writes real files into a temporary tree laid out like the
    repository and runs main(), the entry point CI runs, over it. A checker
    handed a string never passes through the loaders, and the loaders are where
    a bare CR used to vanish: Path.read_text() decodes in universal-newline mode.
    """
    DOCS = '.github/workflows/docs.yml'
    DOCS_POST = '.github/workflows/docs-post.yml'
    CONFIG = '.github/actionlint.yaml'
    CMAKE = 'tests/CMakeLists.txt'

    @staticmethod
    def action_sources():
        """Every file the action-pin lint reads, relative to the repository root."""
        paths = sorted(path for path in (ROOT / '.github/workflows').iterdir()
                       if path.suffix in ('.yml', '.yaml'))
        for name in ('action.yml', 'action.yaml'):
            paths.extend(sorted((ROOT / '.github/actions').rglob(name)))
        return [path.relative_to(ROOT).as_posix() for path in paths]

    def workflow_sources(self):
        return [name for name in self.action_sources() if name.startswith('.github/workflows/')]

    def make_tree(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        tree = Path(directory.name)
        shutil.copytree(ROOT / '.github/workflows', tree / '.github/workflows')
        shutil.copytree(ROOT / '.github/actions', tree / '.github/actions')
        shutil.copyfile(ROOT / self.CONFIG, tree / self.CONFIG)
        (tree / 'tests').mkdir()
        shutil.copyfile(ROOT / self.CMAKE, tree / self.CMAKE)
        return tree

    def crlf_tree(self, names):
        tree = self.make_tree()
        for name in names:
            original = (ROOT / name).read_bytes()
            self.assertNotIn(b'\r', original, name)
            (tree / name).write_bytes(original.replace(b'\n', b'\r\n'))
        return tree

    @staticmethod
    def run_stage(tree, flag):
        output = io.StringIO()
        with mock.patch.object(ci_preflight, 'ROOT', tree), \
                mock.patch.object(sys, 'argv', ['ci_preflight.py', flag]), \
                contextlib.redirect_stdout(output):
            code = ci_preflight.main()
        return code, output.getvalue()

    @staticmethod
    def about(output, name):
        """The stage's lines about one file."""
        return [line for line in output.splitlines() if line.startswith(name + ':')]

    @staticmethod
    def not_utf8(name, line, offset):
        return (f'{name}:{line}: not valid UTF-8 (invalid start byte at byte offset {offset}): '
                'save the file as UTF-8')

    @staticmethod
    def bare_cr_finding(name, changed):
        """The finding for the first bare CR of `changed`: its LF-delimited line and text."""
        index = changed.index(b'\r')
        number = changed.count(b'\n', 0, index) + 1
        source = changed.decode('utf-8').split('\n')[number - 1].strip(' \t')
        return ActionPins.unsupported_finding(
            name, number, source, 'unsupported line break U+000D')

    def assert_bare_cr_refused(self, name, changed):
        """main() over a tree holding `changed` as `name` must name its first bare CR."""
        tree = self.make_tree()
        (tree / name).write_bytes(changed)
        code, output = self.run_stage(tree, '--check-ci-workflow')
        self.assertEqual(self.about(output, name), [self.bare_cr_finding(name, changed)], output)
        self.assertEqual(code, 1, output)

    def assert_undecodable_is_a_finding(self, name, flag):
        """An invalid byte appended to a real file is a finding that names it, never a crash."""
        original = (ROOT / name).read_bytes()
        self.assertTrue(original.endswith(b'\n'), name)
        tree = self.make_tree()
        (tree / name).write_bytes(original + b'# \xff\n')
        try:
            code, output = self.run_stage(tree, flag)
        except UnicodeError as error:
            self.fail(f'{flag} crashed on an undecodable {name}: {error}')
        self.assertEqual(self.about(output, name), [
            self.not_utf8(name, original.count(b'\n') + 1, len(original) + 2)], output)
        self.assertEqual(code, 1, output)

    def test_a_bare_cr_in_place_of_the_final_lf_of_any_real_source_is_refused(self):
        sources = self.action_sources()
        self.assertTrue(any(name.startswith('.github/workflows/') for name in sources))
        self.assertTrue(any(name.startswith('.github/actions/') for name in sources))
        for name in sources:
            with self.subTest(name=name):
                original = (ROOT / name).read_bytes()
                self.assertTrue(original.endswith(b'\n') and b'\r' not in original)
                self.assert_bare_cr_refused(name, original[:-1] + b'\r')

    def test_a_bare_cr_between_two_lines_of_a_real_workflow_is_refused(self):
        original = (ROOT / self.DOCS).read_bytes()
        self.assertEqual(original.count(b'\non:\n'), 1)
        self.assert_bare_cr_refused(self.DOCS, original.replace(b'\non:\n', b'\non:\r', 1))

    def test_the_stage_argv_ci_runs_refuses_the_reported_input(self):
        # No mock at all: the stage's own argv in a child process, over a copy
        # of the tree whose docs.yml has its final LF replaced by a CR.
        changed = (ROOT / self.DOCS).read_bytes()[:-1] + b'\r'
        tree = self.make_tree()
        shutil.copytree(ROOT / 'scripts', tree / 'scripts',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        (tree / self.DOCS).write_bytes(changed)
        argv = {entry[0]: entry[1] for entry in check_commands(tree)}['ci-workflow-contract']
        result = subprocess.run(argv, cwd=tree, capture_output=True, text=True)
        output = result.stdout + result.stderr
        self.assertEqual(self.about(output, self.DOCS), [self.bare_cr_finding(self.DOCS, changed)],
                         output)
        self.assertEqual(result.returncode, 1, output)

    def test_crlf_copies_of_every_stage_input_are_accepted_and_still_scanned(self):
        tree = self.crlf_tree([*self.action_sources(), self.CMAKE])
        code, output = self.run_stage(tree, '--check-ci-workflow')
        self.assertEqual(code, 0, output)
        self.assertIn('runners and time limits OK', output)
        # Zero findings must mean the grammar was checked: a mutable action
        # after the last block scalar of a CRLF file is found on its own line.
        original = (ROOT / self.DOCS).read_bytes()
        (tree / self.DOCS).write_bytes(original.replace(b'\n', b'\r\n')
                                       + b'      - uses: actions/checkout@main\r\n')
        code, output = self.run_stage(tree, '--check-ci-workflow')
        self.assertEqual(self.about(output, self.DOCS), [ActionPins.finding(
            self.DOCS, original.count(b'\n') + 1, 'actions/checkout@main')], output)
        self.assertEqual(code, 1, output)

    def test_the_combined_checker_takes_the_file_texts_as_stored(self):
        sources = [(ROOT / path).read_bytes().decode('utf-8') for path in CI_SOURCES]
        self.assertEqual(ci_workflow_findings(*sources), [])
        # The loaders now hand it CRLF files unchanged: every check but the
        # action-pin lint still has to read them as the LF text they always did.
        self.assertEqual(ci_workflow_findings(*(text.replace('\n', '\r\n') for text in sources)), [])

    def test_a_file_that_is_not_utf8_is_a_finding_not_a_crash(self):
        for name in (self.DOCS, '.github/workflows/release.yml',
                     '.github/actions/setup-live-curl/action.yml', self.CMAKE):
            with self.subTest(name=name):
                self.assert_undecodable_is_a_finding(name, '--check-ci-workflow')

    def test_the_docs_stages_read_the_same_way(self):
        for flag in ('--check-docs-workflow', '--self-test-docs-workflow'):
            with self.subTest(flag=flag, kind='crlf'):
                code, output = self.run_stage(self.crlf_tree((self.DOCS, self.DOCS_POST)), flag)
                self.assertEqual(code, 0, output)
            for name in (self.DOCS, self.DOCS_POST):
                with self.subTest(flag=flag, name=name):
                    self.assert_undecodable_is_a_finding(name, flag)

    def test_the_runner_label_stage_reads_the_same_way(self):
        flag = '--check-actionlint-runner-labels'
        tree = self.crlf_tree([self.CONFIG, *self.workflow_sources()])
        code, output = self.run_stage(tree, flag)
        self.assertEqual(code, 0, output)
        self.assertIn('all literal self-hosted labels declared', output)
        for name in ('.github/workflows/ci.yml', self.CONFIG):
            with self.subTest(name=name):
                self.assert_undecodable_is_a_finding(name, flag)

    def test_reverting_the_loader_to_read_text_is_detected(self):
        changed = (ROOT / self.DOCS).read_bytes()[:-1] + b'\r'
        self.assert_bare_cr_refused(self.DOCS, changed)
        source = inspect.getsource(ci_preflight.read_ci_text)
        guard = 'data = path.read_bytes()\n'
        self.assertEqual(source.count(guard), 1)
        mutant = source.replace(
            guard, "data = path.read_text(encoding='utf-8').encode('utf-8')\n", 1)
        namespace = {}
        exec(compile(mutant, '<loader-reverted-to-read-text>', 'exec'),
             vars(ci_preflight), namespace)
        with mock.patch.object(ci_preflight, 'read_ci_text', namespace['read_ci_text']):
            with self.assertRaises(AssertionError):
                self.assert_bare_cr_refused(self.DOCS, changed)

    def test_dropping_the_lf_view_of_the_other_checks_is_detected(self):
        for flag, names in (('--check-ci-workflow', (self.CMAKE,)),
                            ('--check-docs-workflow', (self.DOCS, self.DOCS_POST)),
                            ('--check-actionlint-runner-labels',
                             (self.CONFIG, *self.workflow_sources()))):
            with self.subTest(flag=flag):
                code, output = self.run_stage(self.crlf_tree(names), flag)
                self.assertEqual(code, 0, output)
                with mock.patch.object(ci_preflight, '_universal_newlines', lambda text: text):
                    code, output = self.run_stage(self.crlf_tree(names), flag)
                self.assertEqual(code, 1, output)


class PreflightFailures(unittest.TestCase):
    def gke(self):
        return (ROOT / '.github/workflows/gke-pr.yml').read_text()

    def test_inline_pr_subset_keeps_its_real_unconditional_subset_command(self):
        original = self.gke()
        self.assertEqual(gke_admit_workflow_findings(original), [])
        job = 'corpus-parity-subset'
        step = 'Check TradingView parity over the pull-request subset'
        command = f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset'
        finding = ('gke-pr.yml corpus-parity-subset must run its unconditional '
                   'inline subset command')
        for before, after in (
                (command, command.removesuffix(' --subset')),
                (command, 'run: echo parity'),
                (command, command.replace(f'JOBS={CORES}', 'JOBS=4')),
                ('        run:', '        if: false\n        run:'),
                ('        run:', '        continue-on-error: true\n        run:')):
            with self.subTest(mutation=after):
                changed = in_step(original, job, step, before, after)
                self.assertNotEqual(changed, original)
                self.assertIn(finding, gke_admit_workflow_findings(changed))

    def gke_mutated_findings(self, job, before, after):
        original = self.gke()
        changed = in_job(original, job, before, after)
        self.assertNotEqual(changed, original, 'the mutation must change the input')
        others = {**self.other_sources(), 'gke-pr.yml': changed}
        return ci_workflow_findings(*self.ci_sources(), others=others)

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

    def light_workflows(self):
        """The requested routing fixture, also constructible from main's workflows."""
        workflows = self.workflow_sources()
        for name, jobs in LIGHT_JOB_IDS.items():
            for job in jobs:
                actual, = ci_preflight._job_values(_jobs(workflows[name])[job],
                                                   'runs-on', unquote=False)
                workflows[name] = in_job(workflows[name], job, f'    runs-on: {actual}\n',
                                         runner_declaration(EXPECTED_LIGHT_GKE_JOBS.get(
                                             name, {}).get(job, 'ubuntu-24.04')))
        return workflows

    def test_light_jobs_accept_exactly_the_requested_routing(self):
        workflows = self.light_workflows()
        # CI 1 + admission 3 + post 7 + parity 1 + docs 3 + native 3 + probe 1.
        self.assertEqual(sum(map(len, LIGHT_JOB_IDS.values())), 19)
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        self.assertEqual(ci_preflight.workflow_fleet_findings(workflows), [])
        for name in ('corpus-parity.yml', 'docs.yml', 'docs-post.yml'):
            self.assertEqual(gated_workflow_findings(workflows[name], name), [])
        for name, jobs in LIGHT_JOB_IDS.items():
            for job in jobs:
                self.assertEqual(ci_preflight._job_values(_jobs(self.workflow_sources()[name])[job],
                                                         'runs-on'),
                                 [EXPECTED_LIGHT_GKE_JOBS.get(name, {}).get(job, 'ubuntu-24.04')])

    def test_light_jobs_refuse_vm_labels_unlisted_images_arrays_and_fallbacks(self):
        workflows = self.light_workflows()
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for name, jobs in LIGHT_JOB_IDS.items():
            for job in jobs:
                expected = EXPECTED_LIGHT_GKE_JOBS.get(name, {}).get(job, 'ubuntu-24.04')
                for runner in (PR_RUNNER, POST_RUNNER, RELEASE_RUNNER, 'ubuntu-latest',
                               'ubuntu-22.04', 'macos-26', 'ubuntu-22.04-arm', 'pf-linux-x64-16',
                               '["ubuntu-24.04"]', "'[\"ubuntu-24.04\"]'",
                               '["self-hosted","pf-ci","pf-ci-x64"]',
                               '["self-hosted","pf-ci","pf-ci-x64-16"]',
                               '["self-hosted","pf-ci","pf-ci-release"]',
                               "${{ github.event_name == 'pull_request' && 'ubuntu-24.04' || 'ubuntu-latest' }}"):
                    with self.subTest(workflow=name, job=job, runner=runner):
                        changed = in_job(workflows[name], job, f'    runs-on: {expected}\n',
                                         f'    runs-on: {runner}\n')
                        self.assertNotEqual(changed, workflows[name])
                        findings = ci_preflight.runner_findings({**workflows, name: changed})
                        suffix = ' (hosted)' if expected == 'ubuntu-24.04' else ''
                        self.assertIn(f'{name} job {job} must run on {expected}{suffix}', findings)

    def test_light_job_metadata_cannot_replace_the_hosted_pin(self):
        workflows = self.light_workflows()
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for name in ('ci-post.yml',):
            for job in ('changes', 'build-gate'):
                expected = EXPECTED_LIGHT_GKE_JOBS.get(name, {}).get(job, 'ubuntu-24.04')
                env = ci_preflight._job_block(_jobs(workflows[name])[job], 'env')
                marker, = [line + '\n' for line in env if 'PF_CI_LABELS:' in line]
                for replacement in ('', f"      PF_CI_LABELS: '{PR_RUNNER}'\n",
                                    f"      PF_CI_LABELS: '{RELEASE_RUNNER}'\n"):
                    with self.subTest(workflow=name, job=job, metadata=replacement):
                        changed = in_job(workflows[name], job, marker, replacement)
                        self.assertEqual(ci_preflight.runner_findings({**workflows, name: changed}), [])
                        changed = in_job(changed, job, f'    runs-on: {expected}\n',
                                         f'    runs-on: {PR_RUNNER}\n')
                        suffix = ' (hosted)' if expected == 'ubuntu-24.04' else ''
                        self.assertIn(f'{name} job {job} must run on {expected}{suffix}',
                                      ci_preflight.runner_findings({**workflows, name: changed}))

    def gcp_sites(self):
        """Mutate every real fleet site; derive the class from events/callers."""
        workflows = self.workflow_sources()
        routes = ci_preflight.workflow_event_routes(workflows)
        for name, workflow in workflows.items():
            workflow_fleet = ('main' if name in ci_preflight.RELEASE_JOB_RUNNERS else
                              'post' if routes[name] & {'push', 'schedule', 'workflow_dispatch'} else 'pr')
            for job, body in _jobs(workflow).items():
                if ci_preflight._job_values(body, 'uses'):
                    continue
                gke = EXPECTED_GKE_JOBS.get(name, {}).get(job)
                if (not gke and ci_preflight.HOSTED_JOB_RUNNERS.get(name, {}).get(job) not in (None, POST_RUNNER)
                        or ci_preflight.RELEASE_JOB_RUNNERS.get(name, {}).get(job) == 'ubuntu-24.04'):
                    continue  # Hosted jobs have independent literal pins and mutation controls.
                # Per-job pins retain the canon PR fleet or the dispatch post fleet.
                fixed_runner = ci_preflight.RELEASE_JOB_RUNNERS.get(name, {}).get(job)
                fleet = 'gke' if gke else {PR_RUNNER: 'pr', POST_RUNNER: 'post'}.get(fixed_runner, workflow_fleet)
                runner = site_runner(name, job, fleet)
                if (name, job) == ('release.yml', 'prebuilt'):
                    yield name, job, fleet, f'            runner: {RELEASE_RUNNER}\n', RELEASE_MATRIX_FINDING
                    continue
                value, = ci_preflight._job_values(body, 'runs-on', unquote=False)
                finding = f'{name} job {job} must run on {runner}'
                if fixed_runner == POST_RUNNER:
                    finding += ' (post fleet)'
                elif fixed_runner is not None and fixed_runner != PR_RUNNER:
                    finding += ' (isolated release)'
                yield name, job, fleet, runner_declaration(value), finding

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
        for index in (0, 1, 4, 5, 6, 7):
            name = Path(CI_SOURCES[index]).name
            routes = ci_preflight.workflow_event_routes(self.workflow_sources())
            runner = POST_RUNNER if routes[name] & {'push', 'schedule', 'workflow_dispatch'} else PR_RUNNER
            for job in JOB_COMMANDS[name]:
                if job in LIGHT_JOB_IDS.get(name, ()):
                    self.assertEqual(ci_preflight._job_values(_jobs(self.ci_sources()[index])[job],
                                                             'runs-on'),
                                     [EXPECTED_LIGHT_GKE_JOBS.get(name, {}).get(job, 'ubuntu-24.04')])
                    continue
                if (name, job) == ('ci-post.yml', 'build-arm'):
                    continue
                actual, = ci_preflight._job_values(_jobs(self.ci_sources()[index])[job],
                                                   'runs-on', unquote=False)
                marker = runner_declaration(actual)
                pinned_runner = EXPECTED_GKE_JOBS.get(name, {}).get(job, runner)
                finding = f'{name} job {job} must run on {pinned_runner}'
                if name == 'native-live.yml' and not pinned_runner.startswith('gke-'):
                    finding += ' (hosted)'
                for replacement in ('ubuntu-24.04', 'ubuntu-latest',
                                    'pf-linux-x64-64' if actual == 'pf-linux-x64-16' else 'pf-linux-x64-16',
                                    "'" + (PR_RUNNER if job == 'build' else runner) + "'",
                                    "${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}"):
                    with self.subTest(workflow=name, job=job, runner=replacement):
                        findings = self.mutated_file_findings(index, job, marker,
                                                              runner_declaration(replacement))
                        self.assertIn(finding, findings)
        for replacement in ("'ubuntu-24.04'", "'" + '["ubuntu-24.04"]' + "'",
                            "${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}"):
            with self.subTest(matrix_runner=replacement):
                findings = self.gke_mutated_findings(
                    'build', runner_declaration(GKE_PR_32), runner_declaration(replacement))
                self.assertIn(f'gke-pr.yml job build must run on {GKE_PR_32}', findings)

    def test_authz_and_every_dependency_are_required_from_mutated_files(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original, others=self.other_sources()), [])
        for index in (0, 1, 6):
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
        for index in (4, 5, 7):
            name = Path(CI_SOURCES[index]).name
            with self.subTest(workflow=name):
                self.assertEqual(gated_workflow_findings(sources[index], name), [])
        self.assertEqual(ci_workflow_findings(*sources, others=self.other_sources()), [])

    def test_gated_workflows_refuse_a_removed_authz_job(self):
        sources = self.ci_sources()
        for index in (4, 5, 7):
            name = Path(CI_SOURCES[index]).name
            authz = '  authz:' + _jobs(sources[index])['authz']
            with self.subTest(workflow=name):
                self.assert_gated_mutation(index, None, authz, '',
                    f'{name} must start with the verbatim canon authz job')

    def test_gated_workflows_require_authz_for_every_dependent_job(self):
        for index, job in ((4, 'corpus-parity'), (4, 'corpus-parity-subset'), (5, 'build'),
                           (7, 'build')):
            name = Path(CI_SOURCES[index]).name
            for after in ('', '    needs: [wrong-authz]\n'):
                with self.subTest(workflow=name, job=job, dependency=after):
                    self.assert_gated_mutation(index, job, '    needs: [authz]\n', after,
                        f'{name} job {job} must need authz')

    def test_gated_workflows_refuse_a_one_character_authz_script_change(self):
        for index in (4, 5, 7):
            name = Path(CI_SOURCES[index]).name
            with self.subTest(workflow=name):
                self.assert_gated_mutation(index, 'authz',
                    '          set -eu\n', '          set -ev\n',
                    f'{name} must start with the verbatim canon authz job')

    def test_gated_corpus_refuses_hosted_fallback_expressions(self):
        fallback = "${{ fromJSON('" + POST_RUNNER + "') || 'ubuntu-24.04' }}"
        for job in ('corpus-parity', 'corpus-parity-subset'):
            with self.subTest(job=job):
                expected = EXPECTED_GKE_JOBS['corpus-parity.yml'][job]
                self.assert_gated_mutation(4, job, runner_declaration(expected),
                    f'    runs-on: {fallback}\n',
                    f'corpus-parity.yml job {job} must run on {expected}')

    def test_gated_docs_requires_the_pinned_hosted_build_runner(self):
        self.assertEqual(ci_preflight._job_values(_jobs(self.ci_sources()[5])['build'],
                                                 'runs-on'), ['ubuntu-24.04'])
        self.assert_gated_mutation(5, 'build',
            '    runs-on: ubuntu-24.04\n',
            '    runs-on: ubuntu-latest\n',
            'docs.yml job build must run on ubuntu-24.04')

    def test_gated_workflows_refuse_pull_request_target_and_unreadable_events(self):
        for index in (4, 5, 7):
            name = Path(CI_SOURCES[index]).name
            for after in ('\non:\n  pull_request_target:\n', '\non: *unreadable\n'):
                with self.subTest(workflow=name, events=after):
                    self.assert_gated_mutation(index, None, '\non:\n', after,
                        f'{name} must use readable events without pull_request_target')

    def test_gated_workflows_require_authz_first(self):
        sources = self.ci_sources()
        for index in (4, 5, 7):
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
        for index in (4, 5, 7):
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
        for index, job in ((1, 'native-live-gate'), (6, 'build-gate')):
            name = Path(CI_SOURCES[index]).name
            for after in ('', AUTHZ_REFUSE_STEP.replace('exit 1', 'true'),
                          AUTHZ_REFUSE_STEP.replace("!= 'success'", "== 'failure'"),
                          AUTHZ_REFUSE_STEP + '      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0\n'):
                with self.subTest(workflow=name, mutation=after):
                    findings = self.mutated_file_findings(index, job, AUTHZ_REFUSE_STEP, after)
                    self.assertIn(f'{name} {job} must always run and refuse authz first', findings)
            # Keeping the refusal after checkout cannot satisfy refuse-first.
            findings = self.mutated_file_findings(index, job, AUTHZ_REFUSE_STEP,
                                                  '      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0\n' + AUTHZ_REFUSE_STEP)
            self.assertIn(f'{name} {job} must always run and refuse authz first', findings)
            for condition in ('${{ !cancelled() }}', '${{ success() }}'):
                findings = self.mutated_file_findings(index, job, '    if: always()\n',
                                                      '    if: ' + condition + '\n')
                self.assertIn(f'{name} {job} must always run and refuse authz first', findings)
        for before, after in (
                ('    if: always()\n', '    if: success()\n'),
                ('test "$ADMIT_RESULT" = success', 'true'),
                ('test "$ALLOWED" = true', 'true')):
            self.assertIn('gke-pr.yml result must require admission, preflight and consistent proof results',
                          self.gke_mutated_findings('result', before, after))

    def test_status_function_jobs_refuse_before_checkout_from_mutated_files(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        for index, job in ((6, 'preflight'), (6, 'build'), (6, 'build-arm'),
                           (6, 'sanitizers'), (6, 'kernel-only')):
            for after in ('', AUTHZ_REFUSE_STEP.replace('exit 1', 'true'),
                          '      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0\n' + AUTHZ_REFUSE_STEP):
                with self.subTest(job=job, mutation=after):
                    findings = self.mutated_file_findings(index, job, AUTHZ_REFUSE_STEP, after)
                    self.assertIn(f'{Path(CI_SOURCES[index]).name} job {job} with a status function must refuse authz first',
                                  findings)

    def test_all_status_functions_require_refusal_and_repository_guards_from_mutated_files(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original, others=self.other_sources()), [])
        body = _jobs(original[6])['preflight']
        steps = ci_preflight._steps(body)
        checkout = ci_preflight._step_named(steps, '      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0')
        verify = ci_preflight._step_named(steps, '      - name: Verify workflow wiring and source guards')
        guard = f'        if: {ci_preflight.AUTHZ_SUCCESS}\n'
        unnamed_run = '      - run: python3 scripts/ci_preflight.py --output-dir build-ci-preflight\n'
        for function in ('always()', 'cancelled()', 'failure()', 'success()'):
            condition = '${{ ' + function + " && needs.authz.result == 'success' }}"
            conditioned = in_job(body, None, f'    if: {AUTHZ_ALWAYS}\n',
                                 f'    if: {condition}\n')
            mutations = (
                (in_job(conditioned, None, AUTHZ_REFUSE_STEP, ''),
                 'ci-post.yml job preflight with a status function must refuse authz first'),
                (in_job(conditioned, None, checkout, in_job(checkout, None, guard, '')),
                 'ci-post.yml job preflight must guard repository steps on authz success'),
                (in_job(conditioned, None, verify, in_job(verify, None, guard, '')),
                 'ci-post.yml job preflight must guard repository steps on authz success'),
                (conditioned + '\n' + unnamed_run,
                 'ci-post.yml job preflight must guard repository steps on authz success'),
            )
            for after, finding in mutations:
                with self.subTest(function=function, mutation=after):
                    findings = self.mutated_file_findings(6, 'preflight', body, after)
                    self.assertIn(finding, findings)
        # An unnamed repository run is allowed with either canon positive guard.
        for condition in (ci_preflight.AUTHZ_SUCCESS, AUTHZ_ALWAYS):
            with self.subTest(positive_condition=condition):
                after = verify + '\n' + unnamed_run + f'        if: {condition}\n'
                changed = in_job(original[6], 'preflight', verify, after)
                self.assertNotEqual(changed, original[6])
                self.assertEqual(ci_preflight.authz_workflow_findings(changed, 'ci-post.yml', 'build-gate'), [])
                findings = self.mutated_file_findings(6, 'preflight', verify,
                                                       after)
                self.assertEqual(findings, [
                    'preflight must keep only checkout, linters, fast checks and diagnostics'])

    def test_ci_contract_pins_parallel_pr_light_and_full_events(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        mutations = (
            (6, '  workflow_dispatch:\n', ''),
            (6, '  native-live:\n', '  missing-native-live:\n'),
            (6, '  sanitizers:\n', '  missing-sanitizers:\n'),
            (6, '  build:\n', '  missing-build:\n'),
            (6, '  kernel-only:\n', '  missing-kernel:\n'),
            (6, '  corpus-parity-subset:\n', '  missing-subset:\n'),
            (6, '  build-gate:\n', '  missing-gate:\n'),
            (6, 'passed "$SANITIZER_RESULT"', 'true'),
            (6, '[[ "$PREFLIGHT_RESULT" == "success" ]] && passed "$BUILD_RESULT"',
                'passed "$PREFLIGHT_RESULT" && passed "$BUILD_RESULT"'),
            (6, 'needs: [authz, changes, preflight,', 'needs: [authz, preflight,'),
            (6, '          CHANGES_RESULT: ${{ needs.changes.result }}\n', ''),
            (6, '          DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}\n',
                "          DOCS_ONLY: 'true'\n"),
            # The docs-only answer is the classifier's, never a constant.
            (6, '      docs_only: ${{ steps.classify.outputs.docs_only }}\n',
                "      docs_only: 'true'\n"),
            # The base's copy judges; the answer is the script's alone.
            (6, 'python3 "$RUNNER_TEMP/ci_docs_only.py" --root . --github-output "$GITHUB_OUTPUT"',
                'python3 scripts/ci_docs_only.py --github-output "$GITHUB_OUTPUT"'),
            (6, 'git show HEAD^1:scripts/ci_docs_only.py', 'git show HEAD:scripts/ci_docs_only.py'),
            (6, '--root . --github-output "$GITHUB_OUTPUT"\n',
                '--root . --github-output "$GITHUB_OUTPUT" || echo docs_only=true >> "$GITHUB_OUTPUT"\n'),
            (6, 'echo "docs_only=false" >> "$GITHUB_OUTPUT"', 'echo "docs_only=true" >> "$GITHUB_OUTPUT"'),
            (6, '          PUSH_FORCED: ${{ github.event.forced }}\n', ''),
            (6, '        id: classify\n', '        id: classify\n        continue-on-error: true\n'),
            (6, '  changes:\n', '  classify:\n'),
            (6, '  sanitizers:\n', '  sanitizers:\n    needs: preflight\n'),
            (6, '--ccache\n',
                "--ccache ${{ github.event_name == 'pull_request' && matrix.build_type == 'Debug' "
                "&& '--exclude-label slow' || '' }}\n"),
            (6, '--ccache\n', '--ccache --exclude-label slow\n'),
            (6, 'exclude_slow: false', 'exclude_slow: true'),
            (1, '        default: false', '        default: true'),
            (1, NATIVE_EXCLUSION, ''),
            # Baseline promotion starts from main's push and runs no PR code;
            # it needs the newest two statuses on the resolved PR head.
            (2, '  push:\n    branches: [main]\n', '  pull_request:\n'),
            (2, '  statuses: read', '  checks: read'),
            (2, '  id-token: write', '  id-token: none'),
            (2, '        with:\n          fetch-depth: 0',
                '        with:\n          ref: ${{ needs.promote-guard.outputs.head_sha }}\n'
                '          fetch-depth: 0'),
            (2, '/commits/$HEAD_SHA/statuses?per_page=100', '/commits/$HEAD_SHA/check-runs'),
            (2, '.["pineforge/parity"] == "success"', 'true'),
            (2, 'if has($s.context) then . else .[$s.context] = $s.state end',
                '.[$s.context] = $s.state'),
            (2, 'echo "green=false" >> "$GITHUB_OUTPUT"; exit 0', 'echo "green=false" >> "$GITHUB_OUTPUT"'),
            # Squash merges: the merge commit is merge_commit_sha, admitted by
            # tree equality with the verified PR head, never by its ancestry,
            # and the campaign tool gets that tree.
            (2, 'MERGE_SHA: ${{ needs.promote-guard.outputs.merge_sha }}',
                'MERGE_SHA: ${{ needs.promote-guard.outputs.head_sha }}'),
            (2, 'HEAD_SHA: ${{ needs.promote-guard.outputs.head_sha }}',
                'HEAD_SHA: ${{ needs.promote-guard.outputs.merge_sha }}'),
            (2, '      head_sha:\n', '      pr_commit:\n'),
            (2, 'git merge-base --is-ancestor "$MERGE_SHA"', 'git merge-base --is-ancestor "$HEAD_SHA"'),
            (2, 'if [ "$merge_tree" != "$head_tree" ]; then', 'if false; then'),
            (2, 'head_tree=$(git rev-parse "$HEAD_SHA^{tree}")',
                'head_tree=$(git rev-parse "$MERGE_SHA^{tree}")'),
            (2, 'echo "ok=false" >> "$GITHUB_OUTPUT"; exit 0\n          fi\n          merge_tree=',
                'echo "ok=false" >> "$GITHUB_OUTPUT"\n          fi\n          merge_tree='),
            (2, 'MERGE_TREE: ${{ steps.exacttree.outputs.merge_tree }}',
                'MERGE_TREE: ${{ needs.promote-guard.outputs.head_sha }}'),
            (2, '--merge-commit "$MERGE_SHA"', '--merge-commit "$HEAD_SHA"'),
            (2, ' --merge-tree "$MERGE_TREE"', ''),
            (2, '--ci-head "$HEAD_SHA"', '--ci-head "$MERGE_SHA"'),
            (2, 'if [ "$code" = "2" ]; then', 'if false; then'),
            (3, '    test_chart_day_memo\n    test_ci_verify_slice_1\n',
                '    test_chart_day_memo\n'),
            (3, 'APPEND PROPERTY LABELS slow', 'APPEND PROPERTY LABELS other'),
        )
        for index, before, after in mutations:
            with self.subTest(index=index, before=before):
                changed = original.copy()
                self.assertIn(before, changed[index])
                changed[index] = changed[index].replace(before, after, 1)
                self.assertNotEqual(changed[index], original[index])
                self.assertNotEqual(ci_workflow_findings(*changed), [])

    def literal_slow_sources(self, names=EXPECTED_PR_SLOW_ROWS):
        sources = self.ci_sources()
        prefix, body = sources[3].split('set(PINEFORGE_PR_SLOW_TESTS\n', 1)
        _, suffix = body.split('\n)\n', 1)
        sources[3] = (prefix + 'set(PINEFORGE_PR_SLOW_TESTS\n'
                      + '\n'.join('    ' + name for name in names) + '\n)\n' + suffix)
        return sources

    def assert_slow_population_refused(self, names):
        # Judge the valid and changed inputs together so rejecting every list
        # cannot satisfy a negative control. Neither checker result is stubbed.
        actual = [
            ci_workflow_findings(*self.literal_slow_sources()),
            ci_workflow_findings(*self.literal_slow_sources(names)),
        ]
        self.assertEqual(actual, [[], [PR_SLOW_ROWS_FINDING]])

    def test_measured_slow_rows_accept_exact_literal_population(self):
        self.assertEqual(len(EXPECTED_PR_SLOW_ROWS), 43)
        self.assertEqual(EXPECTED_PR_SLOW_ROWS, tuple(sorted(EXPECTED_PR_SLOW_ROWS)))
        self.assertEqual(ci_preflight.PR_SLOW_ROWS, EXPECTED_PR_SLOW_ROWS)
        actual = [
            ci_workflow_findings(*self.ci_sources()),
            ci_workflow_findings(*self.literal_slow_sources()),
        ]
        self.assertEqual(actual, [[], []])

    def test_measured_slow_rows_refuse_each_missing_slice(self):
        for family in ('test_ci_verify', 'test_native_command_after'):
            for index in range(1, 9):
                name = f'{family}_slice_{index}'
                with self.subTest(missing=name):
                    names = tuple(row for row in EXPECTED_PR_SLOW_ROWS if row != name)
                    self.assert_slow_population_refused(names)

    def test_measured_slow_rows_refuse_added_name(self):
        # The covers are already registered; adding one changes only the slow set.
        for family in ('test_ci_verify', 'test_native_command_after'):
            with self.subTest(family=family):
                names = tuple(sorted((*EXPECTED_PR_SLOW_ROWS, f'{family}_slices_cover')))
                self.assert_slow_population_refused(names)

    def test_measured_slow_rows_refuse_reordering_duplicates_and_substitution(self):
        for family in ('test_ci_verify', 'test_native_command_after'):
            index = EXPECTED_PR_SLOW_ROWS.index(f'{family}_slice_1')
            reordered = list(EXPECTED_PR_SLOW_ROWS)
            reordered[index:index + 2] = reversed(reordered[index:index + 2])
            duplicated = list(EXPECTED_PR_SLOW_ROWS)
            duplicated[index + 1] = duplicated[index]
            substituted = list(EXPECTED_PR_SLOW_ROWS)
            substituted[index] = family
            for mutation, names in (('reordered', reordered), ('duplicated', duplicated),
                                    ('substituted', substituted)):
                with self.subTest(family=family, mutation=mutation):
                    self.assert_slow_population_refused(names)

    def test_measured_slow_rows_refuse_variable_in_place_of_literal(self):
        sources = self.literal_slow_sources()
        literal = '    test_native_command_after_slice_1\n'
        self.assertEqual(sources[3].count(literal), 1)
        changed = sources.copy()
        changed[3] = sources[3].replace(literal, '    ${PINEFORGE_COMMAND_AFTER_SLICES}\n', 1)
        self.assertEqual(
            [ci_workflow_findings(*sources), ci_workflow_findings(*changed)],
            [[], [PR_SLOW_ROWS_FINDING]])

    def test_documented_slow_row_counts_match_literal_population(self):
        text = (ROOT / 'docs/ci.md').read_text()
        self.assertEqual(len(text.splitlines()), 2016)
        counts = re.findall(
            r'^the (\d+) CTest rows labelled `slow` in `tests/CMakeLists.txt`$'
            r'|^The label `slow` (?:is|includes) `PINEFORGE_PR_SLOW_TESTS` '
            r'in `tests/CMakeLists.txt`: (\d+)$', text, re.MULTILINE)
        self.assertEqual([int(first or second) for first, second in counts],
                         [len(EXPECTED_PR_SLOW_ROWS)] * 2)
        inventory = re.findall(
            r'pins all (\d+) source-labelled names: (\d+) from the\n'
            r'`PINEFORGE_PR_SLOW_TESTS` loop and (\d+) direct CMake', text)
        self.assertEqual(inventory, [('62', '43', '19')])
        self.assertIn('The timing list contains 43 names, and 19 additional names', text)
        self.assertIn('making 62 distinct pinned slow names.', text)

    def test_floor_table_documentation_anchors_point_to_definition(self):
        source = (ROOT / 'scripts/ci_verify.py').read_text().splitlines()
        for path, count in (('CONTRIBUTING.md', 1), ('docs/pages/contributing-llm.md', 2),
                            ('docs/pages/native-engine.md', 1)):
            text = (ROOT / path).read_text()
            anchors = re.findall(r'`PRIVATE_FIXTURE_PROFILE_FLOORS`\s+'
                                 r'ci_verify\.py:(\d+)\s+table', text)
            self.assertEqual(len(anchors), count, path)
            for number in anchors:
                with self.subTest(path=path, line=number):
                    self.assertEqual(source[int(number) - 1],
                                     'PRIVATE_FIXTURE_PROFILE_FLOORS = {')

    def test_command_after_slices_keep_the_exact_slow_pin(self):
        sources = self.ci_sources()
        cmake = sources[3]
        block = re.search(r'^set\(PINEFORGE_PR_SLOW_TESTS\n(.*?)^\)',
                          cmake, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(block)
        names = re.findall(r'^    (test_[A-Za-z0-9_]+)$', block[1], re.MULTILINE)
        family = tuple(name for name in names if name.startswith('test_native_command_after'))
        expected = tuple(f'test_native_command_after_slice_{index}' for index in range(1, 9))
        self.assertEqual(family, expected)
        self.assertEqual(tuple(names), ci_preflight.PR_SLOW_ROWS)
        for name in expected:
            with self.subTest(missing=name):
                changed = sources.copy()
                changed[3] = cmake.replace(f'    {name}\n', '', 1)
                self.assertEqual(
                    [ci_workflow_findings(*sources), ci_workflow_findings(*changed)],
                    [[], [PR_SLOW_ROWS_FINDING]])

    def test_baseline_promotion_accepts_qualified_base_ref(self):
        sources = self.ci_sources()
        short = 'git merge-base --is-ancestor "$MERGE_SHA" "origin/$BASE_REF"'
        qualified = ('git merge-base --is-ancestor "$MERGE_SHA" '
                     '"refs/remotes/origin/$BASE_REF"')
        # Supply the same qualified input before and after the workflow fix.
        sources[2] = sources[2].replace(short, qualified)
        self.assertEqual(sources[2].count(qualified), 1)
        self.assertEqual(ci_workflow_findings(*sources, others=self.other_sources()), [])

    def test_baseline_promotion_rejects_short_base_ref(self):
        sources = self.ci_sources()
        short = 'git merge-base --is-ancestor "$MERGE_SHA" "origin/$BASE_REF"'
        qualified = ('git merge-base --is-ancestor "$MERGE_SHA" '
                     '"refs/remotes/origin/$BASE_REF"')
        # Supply the old command even when the workflow already uses the fix.
        sources[2] = sources[2].replace(qualified, short)
        self.assertEqual(sources[2].count(short), 1)
        self.assertIn('baseline promotion must admit a squash merge by tree equality only',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_baseline_promotion_rejects_later_pr_head_checkout(self):
        finding = ("baseline promotion must run main's copy and require "
                   "the latest two head statuses")
        self.assertNotIn(finding, ci_workflow_findings(*self.ci_sources(),
                                                      others=self.other_sources()))
        anchor = '      - name: Mint an org App token to read the campaign tooling\n'
        checkout = ('      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1\n'
                    "        if: steps.ci.outputs.green == 'true'\n"
                    '        with:\n'
                    '          ref: ${{ needs.promote-guard.outputs.head_sha }}\n')
        findings = self.mutated_file_findings(2, 'promote', anchor, checkout + anchor)
        self.assertIn(finding, findings)

    def test_promotion_guard_rejects_added_contents_read_permission(self):
        finding = 'baseline promotion must start with the pinned read-only promote-guard'
        findings = self.mutated_file_findings(
            2, 'promote-guard',
            '    permissions:\n      pull-requests: read\n',
            '    permissions:\n      contents: read\n      pull-requests: read\n')
        self.assertIn(finding, findings)

    def test_promotion_guard_rejects_missing_pull_requests_read_permission(self):
        finding = 'baseline promotion must start with the pinned read-only promote-guard'
        findings = self.mutated_file_findings(
            2, 'promote-guard', '      pull-requests: read\n', '')
        self.assertIn(finding, findings)

    def test_promotion_guard_rejects_mutations_of_the_real_workflow(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        jobs = _jobs(original[2])
        self.assertEqual(ci_preflight._job_values(jobs['promote-guard'], 'permissions'),
                         ['pull-requests: read'])
        for forbidden in ('secrets.', 'checkout', 'id-token'):
            self.assertNotIn(forbidden, jobs['promote-guard'])
        self.assertEqual(ci_preflight._job_values(jobs['promote'], 'needs'), ['[promote-guard]'])
        guard_finding = 'baseline promotion must start with the pinned read-only promote-guard'
        needs_finding = ('baseline promotion must need promote-guard and require '
                         'successful authorization')
        run_block = '        run: |\n' + ''.join(
            '          ' + line + '\n'
            for line in job_script(original[2], 'promote-guard',
                                    PROMOTION_AUTHORIZATION_STEP).splitlines())
        fail = ('          fail() { echo "::error title=authz::$1"; '
                'echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }\n')
        lookup = ('                  perm=$(gh api "repos/$REPO/collaborators/$MERGED_BY/permission" '
                  '--jq .permission 2>/dev/null || echo none)\n')
        author_lookup = ('                      perm=$(gh api "repos/$REPO/collaborators/$PR_AUTHOR/permission" '
                         '--jq .permission 2>/dev/null || echo none)\n')
        default = ('            *) fail "event $EVENT does not run baseline promotion '
                   '(only push and workflow_dispatch are allowed)" ;;\n')
        permissions = '    permissions:\n      pull-requests: read\n'
        mutations = [
            ('promote-guard', run_block, '        run: |\n          exit 0\n', guard_finding),
            ('promote-guard', fail, '          fail() { :; }\n', guard_finding),
            ('promote-guard',
             '              [ "$HEAD_REPO" = "$REPO" ] || fail "pull request from another repository (${HEAD_REPO:-unknown})"\n',
             '', guard_finding),
            ('promote-guard',
             '              [ "$HEAD_IS_FORK" = "false" ] || fail "pull request from a fork"\n',
             '', guard_finding),
            ('promote-guard', '                OWNER|MEMBER|COLLABORATOR) ;;\n',
             '                *) ;;\n', guard_finding),
            ('promote-guard', '"dependabot[bot] pineforge-app[bot]"',
             '"dependabot[bot] pineforge-app[bot] another[bot]"', guard_finding),
            ('promote-guard', author_lookup, '', guard_finding),
            ('promote-guard', author_lookup,
             author_lookup.replace('|| echo none)', '|| echo write)'), guard_finding),
            ('promote-guard', lookup, '', guard_finding),
            ('promote-guard',
             '              [ -n "$MERGED_BY" ] || fail "push merger is empty"\n',
             '', guard_finding),
            ('promote-guard', lookup,
             lookup.replace('|| echo none)', '|| echo write)'), guard_finding),
            ('promote-guard', 'admin|maintain|write)', 'admin|maintain|write|read)', guard_finding),
            ('promote-guard',
             'case "$perm" in admin|maintain|write) ;; *) fail "push merger ',
             'case "$perm" in admin|maintain|write|read) ;; *) fail "push merger ',
             guard_finding),
            ('promote-guard', default, '', guard_finding),
            ('promote-guard', '            workflow_dispatch)\n',
             '            push|workflow_dispatch)\n', guard_finding),
            ('promote-guard', '            push)\n',
             '            push)\n              exit 0\n', guard_finding),
            ('promote-guard', permissions, '', guard_finding),
            ('promote-guard', permissions,
             '    permissions: {id-token: write}\n', guard_finding),
            ('promote-guard', permissions, '    permissions: {}\n', guard_finding),
            ('promote-guard', '    steps:\n',
             '    env:\n      KEY: ${{ secrets.PINEFORGE_APP_PRIVATE_KEY }}\n'
             '    steps:\n', guard_finding),
            ('promote-guard', '    steps:\n',
             '    steps:\n      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0\n', guard_finding),
            ('promote-guard', '    steps:\n',
             '    if: false\n    steps:\n', guard_finding),
            ('promote-guard', '    steps:\n',
             '    continue-on-error: true\n    steps:\n', guard_finding),
            ('promote-guard', '        run: |\n',
             '        continue-on-error: true\n        run: |\n', guard_finding),
            ('promote-guard', '    runs-on: gke-2cpu-4gb-amd64\n',
             '    runs-on: ubuntu-24.04\n', guard_finding),
            ('promote-guard',
             '          # and the org\'s own GitHub App bot logins by name (space-separated), nothing else.\n',
             '', guard_finding),
            ('promote-guard',
             '      - name: Only our own merged pull requests and dispatches promote the baseline\n',
             '      - name: Changed promotion guard step\n', guard_finding),
            ('promote-guard', '\n\n', '\n', guard_finding),
            ('promote-guard', '      ok: ${{ steps.authorize.outputs.ok }}\n',
             '      ok: ${{ steps.pr.outputs.ok }}\n', guard_finding),
            ('promote-guard', '        id: authorize\n',
             '        id: authorize\n        continue-on-error: true\n', guard_finding),
            ('promote-guard', "        if: steps.pr.outputs.ok == 'true'\n",
             '', guard_finding),
            ('promote', '    needs: [promote-guard]\n', '', needs_finding),
            ('promote', '    needs: [promote-guard]\n',
             '    needs: []\n', needs_finding),
            ('promote', '    needs: [promote-guard]\n',
             '    needs: [promote-guard, another-job]\n', needs_finding),
            ('promote',
             f'    if: {ci_preflight.PROMOTE_IF}\n',
             '    if: ${{ always() }}\n', needs_finding),
            ('promote', f'    if: {ci_preflight.PROMOTE_IF}\n', '', needs_finding),
            ('promote', f'    if: {ci_preflight.PROMOTE_IF}\n',
             "    if: ${{ needs.promote-guard.outputs.ok == 'true' }}\n", needs_finding),
            ('promote', f'    if: {ci_preflight.PROMOTE_IF}\n',
             "    if: ${{ needs.promote-guard.result == 'success' }}\n", needs_finding),
        ]
        # Each resolved API binding is independently required, not merely a word
        # elsewhere in the file. In particular HEAD_IS_FORK must not disappear.
        for key, value in (
                ('EVENT', '${{ github.event_name }}'),
                ('REPO', '${{ github.repository }}'),
                ('HEAD_REPO', '${{ steps.pr.outputs.head_repo }}'),
                ('HEAD_IS_FORK', '${{ steps.pr.outputs.head_is_fork }}'),
                ('ASSOCIATION', '${{ steps.pr.outputs.association }}'),
                ('PR_AUTHOR', '${{ steps.pr.outputs.pr_author }}'),
                ('MERGED_BY', '${{ steps.pr.outputs.merged_by }}'),
                ('GH_TOKEN', '${{ github.token }}   # read-only use: permission lookups for the PR author and merger'),
                ('ALLOWED_BOTS', '"dependabot[bot] pineforge-app[bot]"')):
            mutations.append(('promote-guard', f'          {key}: {value}\n', '', guard_finding))
        for job, before, after, finding in mutations:
            with self.subTest(job=job, before=before, after=after):
                changed = original.copy()
                changed[2] = in_job(changed[2], job, before, after)
                self.assertNotEqual(changed[2], original[2])
                self.assertIn(finding, ci_workflow_findings(*changed))
        for before, after in (
                ('              [ -n "$ASSOCIATION" ] || fail "pull request author association is empty"\n', ''),
                ('              [ -n "$PR_AUTHOR" ] || fail "pull request author is empty"\n', ''),
                ('              [ -n "$REPO" ] || fail "repository is empty"\n', ''),
                ('          HEAD_IS_FORK: ${{ steps.pr.outputs.head_is_fork }}\n',
                 '          HEAD_IS_FORK: ${{ github.event.pull_request.head.repo.fork }}\n'),
                ('            push)\n', '            pull_request_target)\n')):
            with self.subTest(authorization=before):
                changed = original.copy()
                changed[2] = in_step(changed[2], 'promote-guard',
                                     PROMOTION_AUTHORIZATION_STEP, before, after)
                self.assertIn(guard_finding, ci_workflow_findings(*changed))
        for before, after in (
                ('jobs:\n  promote-guard:\n', 'jobs:\n  unexpected:\n    runs-on: ubuntu-latest\n  promote-guard:\n'),
                ('  promote-guard:\n', '  promote-guard: # changed header\n'),
                ('  promote-guard:\n', '  renamed-guard:\n'),
                ('  promote:\n', '  promote-guard:\n'),
                ('          echo "ok=true" >> "$GITHUB_OUTPUT"\n\n  promote:\n',
                 '          echo "ok=true" >> "$GITHUB_OUTPUT"\n  promote:\n')):
            with self.subTest(header=after):
                changed = original.copy()
                self.assertIn(before, changed[2])
                changed[2] = changed[2].replace(before, after, 1)
                self.assertIn(guard_finding, ci_workflow_findings(*changed))
        # Keeping both bodies but moving the guard after promote still
        # violates the first-job contract.
        guard_at = original[2].index('\n  promote-guard:')
        promote_at = original[2].index('\n  promote:')
        changed = original.copy()
        changed[2] = original[2][:guard_at] + original[2][promote_at:]
        self.assertIn(guard_finding, ci_workflow_findings(*changed))
        changed = original.copy()
        changed[2] = (original[2][:guard_at] + original[2][promote_at:]
                      + original[2][guard_at:promote_at])
        self.assertIn(guard_finding, ci_workflow_findings(*changed))
        for before, after in (
                ('  workflow_dispatch:\n', '  push:\n'),
                ('  push:\n    branches: [main]\n',
                 '  pull_request_target:\n    types: [closed]\n')):
            with self.subTest(trigger=after):
                changed = original.copy()
                self.assertIn(before, changed[2])
                changed[2] = changed[2].replace(before, after, 1)
                self.assertIn("baseline promotion must run main's copy and require the latest two head statuses",
                              ci_workflow_findings(*changed))

    @staticmethod
    def quoted_key_lint_findings(workflow, setting):
        """The action-pin lint's refusal of an injected setting whose key is quoted.

        The promotion guard reads a quoted key like the plain one, so its own
        findings do not change; the YAML whitelist refuses the quoted key too,
        first, at the line the setting starts on in the changed workflow.
        """
        if not setting.lstrip(' ').startswith(('"', "'")):
            return []
        number = workflow.count('\n', 0, workflow.index(setting)) + 1
        return [ActionPins.unsupported_finding(
            CI_SOURCES[2], number, setting.strip(), 'quoted keys')]

    def test_promotion_guard_rejects_workflow_defaults_and_env(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        self.assertEqual(original[2].count('\njobs:\n'), 1)
        settings = (
            ('defaults', 'defaults:\n  run:\n    shell: bash -c "exit 0" -- {0}\n'),
            ('env', 'env:\n  KEY: ${{ secrets.PINEFORGE_APP_PRIVATE_KEY }}\n'),
            ('defaults', 'defaults: {}\n'),
            ('env', 'env: {}\n'),
            ('defaults', "'defaults': {run: {shell: bash}}\n"),
            ('env', '"env": {PUBLIC_VALUE: harmless}\n'),
        )
        for key, setting in settings:
            with self.subTest(setting=setting):
                changed = original.copy()
                changed[2] = changed[2].replace('\njobs:\n', '\n' + setting + 'jobs:\n', 1)
                self.assertNotEqual(changed[2], original[2])
                self.assertEqual(_jobs(changed[2]), _jobs(original[2]))
                # promote-guard runs on a GKE pod: a workflow-level secret reaches it too.
                credentials = [] if 'secrets.' not in setting else [
                    'promote-baseline.yml job promote-guard must not expose credentials on GKE',
                    'promote-baseline.yml job promote-guard must keep production credentials '
                    'on a hosted standard runner',
                ]
                # ci_workflow_findings reports runner findings first, then the
                # action-pin lint, then the App-token environment finding, then
                # the promotion findings.
                app_token = []
                if key == 'env' and 'secrets.PINEFORGE_APP_PRIVATE_KEY' in setting:
                    # The inherited key also reaches the unbound guard job;
                    # retain the original workflow-level refusal below it.
                    app_token = [AppTokenEnvironments.finding(
                        'promote-baseline.yml', 'promote-guard')]
                expected = (credentials + self.quoted_key_lint_findings(changed[2], setting)
                            + app_token + [
                                f'baseline promotion must not define workflow-level {key}',
                            ])
                self.assertEqual(ci_workflow_findings(*changed), expected)

    def test_promotion_guard_rejects_job_defaults(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        for setting in (
                '    defaults:\n      run:\n        shell: bash -c "exit 0" -- {0}\n',
                '    defaults: {}\n',
                '    "defaults": {run: {shell: bash}}\n'):
            with self.subTest(setting=setting):
                changed = original.copy()
                changed[2] = in_job(changed[2], 'promote-guard', '    steps:\n',
                                    setting + '    steps:\n')
                self.assertNotEqual(changed[2], original[2])
                expected = self.quoted_key_lint_findings(changed[2], setting) + [
                    'baseline promotion promote-guard must not define job-level defaults',
                    'baseline promotion must start with the pinned read-only promote-guard',
                ]
                self.assertEqual(ci_workflow_findings(*changed), expected)

    def promotion_guard_result(self, *, workflow=None, **overrides):
        """Run the real workflow shell; only the external gh CLI is stubbed."""
        bash = shutil.which('bash')
        if bash is None:
            self.fail('NOT RUN: bash is required to execute the promote-guard decision cases')
        if workflow is None:
            workflow = self.ci_sources()[2]
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / 'summary'
            lookup = Path(directory) / 'lookup'
            output = Path(directory) / 'outputs'
            gh = Path(directory) / 'gh'
            # Put an executable stub first on PATH. The shell under test is
            # extracted unchanged from the workflow, with no driver prepended.
            gh.write_text(f'#!{bash}\n' + r'''printf '%s\n' "$*" >> "$LOOKUP_LOG"
[ "$#" -eq 4 ] && [ "$1" = api ] &&
  [ "$3" = --jq ] && [ "$4" = .permission ] || exit 2
case "$2" in
  "repos/$REPO/collaborators/$PR_AUTHOR/permission")
    [ -n "$PR_AUTHOR" ] || exit 2
    lookup_permission=$AUTHOR_PERMISSION
    lookup_exit=$AUTHOR_LOOKUP_EXIT ;;
  "repos/$REPO/collaborators/$MERGED_BY/permission")
    [ -n "$MERGED_BY" ] || exit 2
    lookup_permission=$MERGER_PERMISSION
    lookup_exit=$MERGER_LOOKUP_EXIT ;;
  *) exit 2 ;;
esac
if [ -n "$lookup_permission" ]; then printf '%s\n' "$lookup_permission"; fi
exit "$lookup_exit"
''')
            gh.chmod(0o755)
            env = {
                'PATH': directory + os.pathsep + os.environ.get('PATH', '/usr/bin:/bin'),
                'GITHUB_STEP_SUMMARY': str(summary), 'LOOKUP_LOG': str(lookup),
                'GITHUB_OUTPUT': str(output),
                'EVENT': 'push', 'REPO': 'pineforge-4pass/pineforge-engine',
                'HEAD_REPO': 'pineforge-4pass/pineforge-engine', 'HEAD_IS_FORK': 'false',
                'ASSOCIATION': 'MEMBER', 'PR_AUTHOR': 'author',
                'MERGED_BY': 'merger',
                'ALLOWED_BOTS': 'dependabot[bot] pineforge-app[bot]',
                'AUTHOR_PERMISSION': 'none', 'AUTHOR_LOOKUP_EXIT': '0',
                'MERGER_PERMISSION': 'write', 'MERGER_LOOKUP_EXIT': '0',
            }
            env.update(overrides)
            result = subprocess.run(
                [bash, '--noprofile', '--norc', '-eo', 'pipefail', '-c',
                 job_script(workflow, 'promote-guard', PROMOTION_AUTHORIZATION_STEP)],
                env=env, capture_output=True, text=True)
            return (result, summary.read_text() if summary.exists() else '',
                    lookup.read_text() if lookup.exists() else '',
                    output.read_text() if output.exists() else '')

    def test_promotion_guard_accepts_only_authorised_merges_and_dispatches(self):
        cases = [({'ASSOCIATION': association}, ('merger',))
                 for association in ('OWNER', 'MEMBER', 'COLLABORATOR')]
        cases += [({'ASSOCIATION': 'NONE', 'PR_AUTHOR': bot}, ('merger',))
                  for bot in ('dependabot[bot]', 'pineforge-app[bot]')]
        cases += [({'MERGER_PERMISSION': permission}, ('merger',))
                  for permission in ('admin', 'maintain', 'write')]
        cases += [({'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': permission},
                   ('author', 'merger'))
                  for permission in ('admin', 'maintain', 'write')]
        cases += [({'MERGED_BY': bot, 'MERGER_LOOKUP_EXIT': '1'}, ())
                  for bot in ('dependabot[bot]', 'pineforge-app[bot]')]
        cases.append(({'EVENT': 'workflow_dispatch', 'HEAD_REPO': 'outside/fork',
                       'HEAD_IS_FORK': 'true', 'ASSOCIATION': 'NONE', 'PR_AUTHOR': 'outsider',
                       'MERGED_BY': '', 'AUTHOR_LOOKUP_EXIT': '1',
                       'MERGER_LOOKUP_EXIT': '1'}, ()))
        for overrides, usernames in cases:
            with self.subTest(overrides=overrides):
                result, summary, lookup, output = self.promotion_guard_result(**overrides)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('authz: allowed (', summary)
                self.assertNotIn('REFUSED', summary)
                self.assertEqual(output, 'ok=true\n')
                self.assertEqual(lookup, ''.join(
                    f'api repos/pineforge-4pass/pineforge-engine/collaborators/{username}/permission '
                    '--jq .permission\n' for username in usernames))
        # No merger may be omitted after a push resolved one merged PR.
        # Unmerged PRs now defer in resolution, before this authorization step.
        for author, usernames in (({}, ()), ({'ASSOCIATION': 'CONTRIBUTOR',
                                             'AUTHOR_PERMISSION': 'write'}, ('author',))):
            with self.subTest(unmerged_author=author):
                result, summary, lookup, output = self.promotion_guard_result(
                    MERGED_BY='', MERGER_LOOKUP_EXIT='1', **author)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(summary, 'authz: REFUSED: push merger is empty\n')
                self.assertEqual(output, '')
                self.assertEqual(lookup, ''.join(
                    f'api repos/pineforge-4pass/pineforge-engine/collaborators/{username}/permission '
                    '--jq .permission\n' for username in usernames))

    def test_promotion_guard_refuses_foreign_heads_authors_mergers_and_lookup_errors(self):
        cases = [
            ({'HEAD_REPO': 'outside/fork'}, 'pull request from another repository (outside/fork)', ()),
            ({'HEAD_REPO': 'outside/fork', 'HEAD_IS_FORK': 'true', 'ASSOCIATION': 'OWNER'},
             'pull request from another repository (outside/fork)', ()),
            ({'HEAD_REPO': 'outside/fork', 'HEAD_IS_FORK': 'true', 'ASSOCIATION': 'COLLABORATOR'},
             'pull request from another repository (outside/fork)', ()),
            ({'HEAD_REPO': 'outside/fork', 'PR_AUTHOR': 'dependabot[bot]', 'ASSOCIATION': 'NONE'},
             'pull request from another repository (outside/fork)', ()),
            ({'HEAD_REPO': ''}, 'pull request from another repository (unknown)', ()),
            ({'HEAD_IS_FORK': 'true'}, 'pull request from a fork', ()),
            ({'HEAD_IS_FORK': 'true', 'MERGED_BY': 'pineforge-app[bot]'}, 'pull request from a fork', ()),
            ({'HEAD_IS_FORK': ''}, 'pull request from a fork', ()),
            ({'HEAD_IS_FORK': 'true', 'PR_AUTHOR': 'pineforge-app[bot]', 'ASSOCIATION': 'NONE'},
             'pull request from a fork', ()),
            ({'HEAD_REPO': 'outside/fork', 'HEAD_IS_FORK': 'true',
              'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': 'write'},
             'pull request from another repository (outside/fork)', ()),
            ({'HEAD_IS_FORK': 'true', 'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': 'write'},
             'pull request from a fork', ()),
            ({'ASSOCIATION': 'NONE', 'PR_AUTHOR': 'outsider'},
             "author outsider (NONE) has permission 'none', not write, and is not an allowlisted bot",
             ('outsider',)),
            ({'ASSOCIATION': 'NONE', 'PR_AUTHOR': 'outsider', 'MERGED_BY': 'pineforge-app[bot]'},
             "author outsider (NONE) has permission 'none', not write, and is not an allowlisted bot",
             ('outsider',)),
            ({'ASSOCIATION': 'UNKNOWN', 'PR_AUTHOR': 'outsider'},
             "author outsider (UNKNOWN) has permission 'none', not write, and is not an allowlisted bot",
             ('outsider',)),
            ({'ASSOCIATION': 'CONTRIBUTOR', 'PR_AUTHOR': 'another[bot]'},
             "author another[bot] (CONTRIBUTOR) has permission 'none', not write, and is not an allowlisted bot",
             ('another[bot]',)),
            ({'ASSOCIATION': '', 'PR_AUTHOR': ''}, 'pull request author association is empty', ()),
            ({'ASSOCIATION': '', 'AUTHOR_PERMISSION': 'write'},
             'pull request author association is empty', ()),
            ({'PR_AUTHOR': ''}, 'pull request author is empty', ()),
            ({'REPO': '', 'HEAD_REPO': ''}, 'repository is empty', ()),
            ({'MERGED_BY': ''}, 'push merger is empty', ()),
            ({'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': 'write', 'MERGER_PERMISSION': 'read'},
             "push merger merger has permission 'read', not write", ('author', 'merger')),
        ]
        cases += [({'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': permission},
                   f"author author (CONTRIBUTOR) has permission '{permission}', not write, "
                   "and is not an allowlisted bot", ('author',))
                  for permission in ('read', 'triage', 'none', '')]
        cases.append(({'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': '', 'AUTHOR_LOOKUP_EXIT': '1'},
                      "author author (CONTRIBUTOR) has permission 'none', not write, "
                      "and is not an allowlisted bot", ('author',)))
        cases.append(({'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': 'write', 'AUTHOR_LOOKUP_EXIT': '1'},
                      "author author (CONTRIBUTOR) has permission 'write\nnone', not write, "
                      "and is not an allowlisted bot", ('author',)))
        cases += [({'MERGER_PERMISSION': permission},
                   f"push merger merger has permission '{permission}', not write", ('merger',))
                  for permission in ('read', 'triage', 'none', '')]
        cases.append(({'MERGER_PERMISSION': '', 'MERGER_LOOKUP_EXIT': '1'},
                      "push merger merger has permission 'none', not write", ('merger',)))
        # Even a failed call that printed 'write' must refuse.
        cases.append(({'MERGER_PERMISSION': 'write', 'MERGER_LOOKUP_EXIT': '1'},
                      "push merger merger has permission 'write\nnone', not write", ('merger',)))
        cases += [({'EVENT': event},
                   f'event {event} does not run baseline promotion '
                   '(only push and workflow_dispatch are allowed)', ())
                  for event in ('pull_request', 'pull_request_target', 'workflow_run', 'schedule', '')]
        for overrides, message, usernames in cases:
            with self.subTest(overrides=overrides):
                result, summary, lookup, output = self.promotion_guard_result(**overrides)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('::error title=authz::' + message, result.stdout)
                self.assertIn('authz: REFUSED: ' + message, summary)
                self.assertNotIn('authz: allowed', summary)
                self.assertEqual(output, '')
                self.assertEqual(lookup, ''.join(
                    f'api repos/pineforge-4pass/pineforge-engine/collaborators/{username}/permission '
                    '--jq .permission\n' for username in usernames))

    def test_promotion_guard_decisions_detect_disabled_refusals_in_real_workflow(self):
        original = self.ci_sources()
        run_block = '        run: |\n' + ''.join(
            '          ' + line + '\n'
            for line in job_script(original[2], 'promote-guard',
                                    PROMOTION_AUTHORIZATION_STEP).splitlines())
        fail = ('          fail() { echo "::error title=authz::$1"; '
                'echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }\n')
        cases = (
            ('guard script replaced by exit 0', run_block,
             '        run: |\n          exit 0\n',
             {'HEAD_REPO': 'outside/fork', 'HEAD_IS_FORK': 'true'}),
            ('fail made a no-op', fail, '          fail() { :; }\n',
             {'HEAD_IS_FORK': 'true'}),
            ('fork check deleted',
             '              [ "$HEAD_IS_FORK" = "false" ] || fail "pull request from a fork"\n',
             '', {'HEAD_IS_FORK': 'true'}),
            ('head repository check deleted',
             '              [ "$HEAD_REPO" = "$REPO" ] || fail "pull request from another repository (${HEAD_REPO:-unknown})"\n',
             '', {'HEAD_REPO': 'outside/fork', 'HEAD_IS_FORK': 'false'}),
            ('green exit before head and author checks', '            push)\n',
             '            push)\n              exit 0\n',
             {'HEAD_REPO': 'outside/fork', 'HEAD_IS_FORK': 'true'}),
        )
        for name, before, after, overrides in cases:
            with self.subTest(mutation=name):
                baseline, _, _, _ = self.promotion_guard_result(**overrides)
                self.assertEqual(baseline.returncode, 1, baseline.stdout + baseline.stderr)
                changed = original.copy()
                changed[2] = in_step(changed[2], 'promote-guard',
                                     PROMOTION_AUTHORIZATION_STEP, before, after)
                self.assertNotEqual(changed[2], original[2])
                self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                              ci_workflow_findings(*changed))
                result, _, _, _ = self.promotion_guard_result(
                    workflow=changed[2], **overrides)
                # Each disabled refusal must change a required exit 1 into
                # exit 0; an unrelated shell error is not evidence.
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotEqual(result.returncode, baseline.returncode)

    def test_promotion_guard_decisions_detect_author_lookup_mutations(self):
        original = self.ci_sources()
        author_lookup = ('                      perm=$(gh api "repos/$REPO/collaborators/$PR_AUTHOR/permission" '
                         '--jq .permission 2>/dev/null || echo none)\n')
        cases = (
            ('author lookup deleted', '', {'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': 'write'},
             0, 1),
            ('failed author lookup passes', author_lookup.replace('|| echo none)', '|| echo write)'),
             {'ASSOCIATION': 'CONTRIBUTOR', 'AUTHOR_PERMISSION': '', 'AUTHOR_LOOKUP_EXIT': '1'},
             1, 0),
        )
        for name, replacement, overrides, expected, mutated_expected in cases:
            with self.subTest(mutation=name):
                baseline, _, _, _ = self.promotion_guard_result(**overrides)
                self.assertEqual(baseline.returncode, expected, baseline.stdout + baseline.stderr)
                changed = original.copy()
                changed[2] = in_job(changed[2], 'promote-guard', author_lookup, replacement)
                self.assertNotEqual(changed[2], original[2])
                self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                              ci_workflow_findings(*changed))
                result, _, _, _ = self.promotion_guard_result(workflow=changed[2], **overrides)
                self.assertEqual(result.returncode, mutated_expected, result.stdout + result.stderr)
                self.assertNotEqual(result.returncode, baseline.returncode)

    def test_promotion_guard_decisions_detect_merger_lookup_mutations(self):
        original = self.ci_sources()
        lookup = ('                  perm=$(gh api "repos/$REPO/collaborators/$MERGED_BY/permission" '
                  '--jq .permission 2>/dev/null || echo none)\n')
        cases = (
            ('merger lookup deleted', '', {'MERGER_PERMISSION': 'write'}, 0, 1),
            ('failed merger lookup passes', lookup.replace('|| echo none)', '|| echo write)'),
             {'MERGER_PERMISSION': '', 'MERGER_LOOKUP_EXIT': '1'}, 1, 0),
        )
        for name, replacement, overrides, expected, mutated_expected in cases:
            with self.subTest(mutation=name):
                baseline, _, _, _ = self.promotion_guard_result(**overrides)
                self.assertEqual(baseline.returncode, expected, baseline.stdout + baseline.stderr)
                changed = original.copy()
                changed[2] = in_step(changed[2], 'promote-guard',
                                     PROMOTION_AUTHORIZATION_STEP, lookup, replacement)
                self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                              ci_workflow_findings(*changed))
                result, _, _, _ = self.promotion_guard_result(workflow=changed[2], **overrides)
                self.assertEqual(result.returncode, mutated_expected, result.stdout + result.stderr)
                self.assertNotEqual(result.returncode, baseline.returncode)

    def test_promotion_guard_crash_cannot_report_authorization(self):
        original = self.ci_sources()
        changed = original.copy()
        changed[2] = in_step(changed[2], 'promote-guard', PROMOTION_AUTHORIZATION_STEP,
                             '          set -euo pipefail\n',
                             '          set -euo pipefail\n          exit 37\n')
        self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                      ci_workflow_findings(*changed))
        result, summary, lookup, output = self.promotion_guard_result(workflow=changed[2])
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertNotIn('authz: allowed', summary)
        self.assertEqual(lookup, '')
        self.assertEqual(output, '')

    def test_promotion_guard_pins_api_facts_refusals_and_job_outputs(self):
        original = self.ci_sources()
        guard = _jobs(original[2])['promote-guard']
        expected_if = ("${{ needs.promote-guard.result == 'success' && "
                       "needs.promote-guard.outputs.ok == 'true' }}")
        self.assertEqual(ci_preflight._job_values(_jobs(original[2])['promote'], 'if'), [expected_if])
        expected_outputs = ['ok: ${{ steps.authorize.outputs.ok }}']
        expected_outputs += [f'{key}: ${{{{ steps.pr.outputs.{key} }}}}'
                             for key in ('merge_sha', 'head_sha', 'pr_number', 'pr_url', 'merged_at')]
        self.assertEqual(ci_preflight._job_values(guard, 'outputs'), [' '.join(expected_outputs)])
        finding = 'baseline promotion must start with the pinned read-only promote-guard'
        for before, after in (
                ('          [ "$REF" = refs/heads/main ] || fail "baseline promotion requires refs/heads/main"\n', ''),
                ('[[ -n "${value//[[:space:]]/}" && "$value" != *$\'\\n\'* && "$value" != *$\'\\r\'* ]]',
                 'true'),
                ('except (ValueError, RecursionError):', 'except ValueError:'),
                ('or any(ord(char) < 32 for char in obj)', 'or False'),
                ('not isinstance(obj, str) or not obj.strip()', 'False'),
                ('if type(number) is not int or number <= 0:', 'if False:'),
                ('if not re.fullmatch(r"[0-9a-f]{40}", sha):', 'if False:'),
                ('["gh", "api", f"repos/{os.environ[\'REPO\']}/pulls/{number}"]',
                 '["gh", "api", f"repos/{os.environ[\'REPO\']}/pulls/1"]'),
                ('check=True, capture_output=True, text=True', 'check=False, capture_output=True, text=True'),
                ('except (subprocess.CalledProcessError, OSError, ValueError, RecursionError):',
                 'except ValueError:'),
                ('pr.get("number") != number', 'False'),
                ('type(pr.get("number")) is not int', 'False'),
                ('pr.get("state") != "closed"', 'False'),
                ('pr.get("merged") is not True', 'False'),
                ('pr.get("merge_commit_sha") != sha', 'False'),
                ('field(pr, "base", "ref") != "main"', 'False'),
                ('field(pr, "head", "sha") != field(selected, "head", "sha")', 'False'),
                ('field(pr, "merged_at") != field(selected, "merged_at")', 'False'),
                ('if not re.fullmatch(r"[0-9a-f]{40}", head_sha):', 'if False:'),
                ('if type(fork) is not bool:', 'if False:'),
                ('"merged_by": field(pr, "merged_by", "login")',
                 '"merged_by": field(pr, "user", "login")'),
                ('              raise SystemExit(1)\n', '              raise SystemExit(0)\n')):
            with self.subTest(api_refusal=before):
                changed = original.copy()
                changed[2] = in_step(changed[2], 'promote-guard', 'Resolve the merged PR', before, after)
                self.assertIn(finding, ci_workflow_findings(*changed))
        for key in ('merge_sha', 'head_sha', 'pr_number', 'pr_url', 'merged_at'):
            with self.subTest(job_output=key):
                changed = original.copy()
                changed[2] = in_job(changed[2], 'promote-guard',
                                    f'      {key}: ${{{{ steps.pr.outputs.{key} }}}}\n', '')
                self.assertIn(finding, ci_workflow_findings(*changed))

    def test_build_matrix_keeps_one_pr_smoke_and_every_full_event_leg(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        for job, build_type in (('build', 'Release'), ('build-debug', 'Debug')):
            body = _jobs(self.gke())[job]
            self.assertEqual(ci_preflight._job_block(body, 'strategy'), [])
            self.assertIn(f'-DCMAKE_BUILD_TYPE={build_type}', body)
            for axes in ('        build_type: [Release, Debug]\n',
                         '        os: [macos-26]\n',
                         '        exclude: []\n', '        include: []\n',
                         '        event: ["${{ github.event_name }}"]\n'):
                self.assertIn(f'gke-pr.yml {job} must remain one fixed proof job',
                              self.gke_mutated_findings(job, '    steps:\n',
                                  '    strategy:\n      matrix:\n' + axes + '    steps:\n'))
        mutations = [
            (6, 'build', '        build_type: [Release]\n', '        build_type: [Release, Debug]\n'),
            (6, 'build', '          - os: ubuntu-24.04\n', '          - os: macos-26\n'),
            (6, 'build-debug', '          - os: ubuntu-24.04\n', ''),
            (6, 'build', '          - os: ubuntu-24.04\n', ''),
            (6, 'build', '          - os: ubuntu-24.04\n',
             '          - os: ubuntu-24.04\n          - os: macos-26\n'),
            (6, 'build-arm', '        build_type: [Release, Debug]\n', ''),
            (6, 'build-arm', '      matrix:\n', '      matrix:\n        os: [ubuntu-24.04]\n'),
            (6, 'build-arm', '      matrix:\n',
             '      matrix:\n        os: [ubuntu-24.04-arm, ubuntu-24.04]\n'),
        ]
        for index, job in ((6, 'build'), (6, 'build-debug'), (6, 'build-arm')):
            if index == 6 and job == 'build-arm':
                for build_type in ('Release', 'Debug'):
                    mutations.append((index, job, '        build_type: [Release, Debug]\n',
                                      f'        build_type: [{build_type}]\n'))
            elif index == 6:
                build_type = 'Release' if job == 'build' else 'Debug'
                mutations.append((index, job, f'        build_type: [{build_type}]\n',
                                  '        build_type: [Release, Debug]\n'))
            for extra in ('        event: ["${{ github.event_name }}"]\n',
                          '        exclude: []\n', '        include: []\n'):
                mutations.append((index, job, '      matrix:\n', '      matrix:\n' + extra))
            for os_name in ('ubuntu-24.04', 'ubuntu-24.04-arm', 'macos-26'):
                for build_type in ('Release', 'Debug'):
                    axis = f'platform: {{os: {os_name}}}' if job in ('build', 'build-debug') else f'os: {os_name}'
                    mutations.append((index, job, '      matrix:\n',
                        '      matrix:\n        exclude:\n'
                        f'          - {axis}\n            build_type: {build_type}\n'))
        for index, job, before, after in mutations:
            with self.subTest(workflow=CI_SOURCES[index], job=job, before=before, after=after):
                changed = original.copy()
                changed[index] = in_job(changed[index], job, before, after)
                self.assertNotEqual(changed[index], original[index])
                finding = (POST_ARM_BUILD_RUNNER_FINDING if job == 'build-arm' else
                           POST_DEBUG_BUILD_RUNNER_FINDING if job == 'build-debug' else
                           POST_BUILD_RUNNER_FINDING)
                self.assertIn(finding, ci_workflow_findings(*changed, others=others))

    def test_post_build_refuses_a_runner_with_the_wrong_os(self):
        post = self.ci_sources()[6]
        for job, platform_step in (('build', ci_preflight.POST_PLATFORM_STEP),
                                   ('build-debug', ci_preflight.POST_PLATFORM_STEP),
                                   ('build-arm', ci_preflight.POST_ARM_PLATFORM_STEP)):
            script = job_script(post, job, 'Require the requested platform')
            for requested in ('ubuntu-24.04', 'ubuntu-24.04-arm', 'macos-26'):
                for actual in ('Linux', 'macOS', 'Windows', ''):
                    with self.subTest(job=job, requested=requested, actual=actual):
                        result = subprocess.run(['bash', '-e', '-c', script], capture_output=True, text=True,
                            env={'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                                 'REQUESTED_PLATFORM': requested, 'ACTUAL_OS': actual})
                        self.assertEqual(result.returncode == 0,
                                         (requested, actual) == (
                                             'ubuntu-24.04-arm' if job == 'build-arm' else 'ubuntu-24.04',
                                             'Linux'),
                                         result.stdout + result.stderr)
            for before, after in ((platform_step, ''),
                                  ('ACTUAL_OS: ${{ runner.os }}', 'ACTUAL_OS: macOS'),
                                  ('exit 1 ;;', 'true ;;')):
                self.assertIn(f'full {job} must refuse a runner whose OS differs from its named platform',
                              self.mutated_file_findings(6, job, before, after))

    def test_ci_split_requires_every_home_trigger_without_path_filters(self):
        ci = self.ci_sources()[0]
        self.assertEqual(ci_preflight._events(ci), {'pull_request'})
        mutations = [
            (6, '  push:\n    branches: [main]\n', ''),
            (0, '  pull_request:\n', ''),
            (6, "  schedule:\n    - cron: '30 1 * * *'\n", ''),
            (6, '  workflow_dispatch:\n    inputs:\n      cold_native_cache:\n'
             '        description: Run full native profiles without compiler or libcurl caches\n'
             '        type: boolean\n        default: false\n', ''),
            (6, '    branches: [main]\n', '    branches: [other]\n'),
            (6, '    branches: [main]\n', '    branches: [main]\n    paths: [src/**]\n'),
            (6, '    branches: [main]\n', '    branches: [main]\n    paths-ignore: [scripts/**]\n'),
            (6, "    - cron: '30 1 * * *'\n", "    - cron: '30 1 * * 0'\n"),
            (0, '  pull_request:\n', '  pull_request:\n    paths: [src/**]\n'),
            (0, '  pull_request:\n', '  pull_request:\n  release:\n'),
            (0, '  pull_request:\n', '  pull_request:\n  push:\n'),
            (6, 'on:\n', 'on:\n  pull_request:\n'),
        ]
        for index, before, after in mutations:
            with self.subTest(replacement=after, removed=before):
                findings = self.mutated_file_findings(index, None, before, after)
                self.assertIn(f'{Path(CI_SOURCES[index]).name} must keep its exact '
                              + ('unfiltered PR' if index == 0 else 'main push, nightly and manual') + ' triggers',
                              findings)

    def test_ci_split_requires_ready_for_review(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        findings = self.mutated_file_findings(0, None,
            '    types: [opened, synchronize, reopened, ready_for_review, converted_to_draft]\n',
            '    types: [opened, synchronize, reopened, converted_to_draft]\n')
        self.assertIn('ci.yml must keep its exact unfiltered PR triggers', findings)
        self.assertIn('ci.yml must run on every pull_request without event filters', findings)

    def test_ci_split_requires_converted_to_draft(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        findings = self.mutated_file_findings(0, None,
            '    types: [opened, synchronize, reopened, ready_for_review, converted_to_draft]\n',
            '    types: [opened, synchronize, reopened, ready_for_review]\n')
        self.assertIn('ci.yml must keep its exact unfiltered PR triggers', findings)
        self.assertIn('ci.yml must run on every pull_request without event filters', findings)

    def test_ci_split_requires_separate_draft_and_ready_concurrency_groups(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        findings = self.mutated_file_findings(0, None,
            '  cancel-in-progress: true\n', '  cancel-in-progress: false\n')
        self.assertIn('ci.yml must retain its PR concurrency group and cancellation', findings)
        self.assertIn('gke-pr.yml must cancel superseded runs for the same PR',
                      self.gke_mutated_findings(
                          None, '  group: gke-pr-${{ github.event.pull_request.number }}\n',
                          '  group: gke-pr\n'))
        for before, after in (
                ('test "$DRAFT" = false', 'true'),
                ('test "$draft" = false', 'true')):
            self.assertIn('gke-pr.yml admit must refuse drafts and stale or unauthorized identities',
                          self.gke_mutated_findings('admit', before, after))

    def test_ci_split_requires_draft_guards_on_every_pr_suite_job(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        for job in ('preflight', 'build', 'build-debug', 'corpus-parity-subset'):
            with self.subTest(job=job):
                self.assertIn(f'gke-pr.yml {job} must follow admission and keep its path policy',
                              self.gke_mutated_findings(
                                  job, "needs.admit.outputs.allowed == 'true'", 'true'))

    def test_admission_preserves_false_payload_booleans_and_identity_refusals(self):
        identity = ci_preflight._step_named(ci_preflight._steps(_jobs(self.gke())['admit']),
                                             '        id: identity')
        env = '\n'.join(ci_preflight._step_block(identity, 'env')[1:])
        self.assertEqual(ci_preflight._job_values(env, 'DRAFT'),
                         ['${{ toJSON(github.event.pull_request.draft) }}'])
        self.assertEqual(ci_preflight._job_values(env, 'HEAD_IS_FORK'),
                         ['${{ toJSON(github.event.pull_request.head.repo.fork) }}'])
        for before, after in (
                ('exit 1; }', ':; }'),
                ('for login in "$PR_AUTHOR" "$TRIGGERING_ACTOR" "$ACTOR"; do',
                 'for login in "$PR_AUTHOR"; do'),
                ('admin|maintain|write) ;;', 'admin|maintain|write|read) ;;'),
                ('test "$live_head" = "$HEAD_SHA"', 'true'),
                ('test "$state" = open', 'true'),
                ('test "$live_base" = main', 'true'),
                ('test "$live_repo" = "$REPO"', 'true')):
            with self.subTest(before=before):
                self.assertIn('gke-pr.yml admit must refuse drafts and stale or unauthorized identities',
                              self.gke_mutated_findings('admit', before, after))

    def test_ci_split_refuses_deleting_or_renaming_every_job(self):
        ci = self.ci_sources()[0]
        self.assertEqual(set(_jobs(ci)), set(CI_JOB_IDS))
        for job in CI_JOB_IDS:
            block = '  ' + job + ':' + _jobs(ci)[job]
            for before, after in ((block, ''), (f'  {job}:\n', f'  {job}-renamed:\n')):
                with self.subTest(job=job, replacement=after):
                    findings = self.mutated_file_findings(0, None, before, after)
                    self.assertIn(f'ci.yml is missing job {job}', findings)
        self.assertIn('ci.yml must start with the verbatim canon authz job',
                      self.mutated_file_findings(0, 'authz', '    name: authz\n',
                                                  '    name: renamed\n'))
        admission = self.gke()
        self.assertEqual(set(_jobs(admission)), set(GKE_PR_JOB_IDS))
        for job in GKE_PR_JOB_IDS:
            block = '  ' + job + ':' + _jobs(admission)[job]
            for before, after in ((block, ''), (f'  {job}:\n', f'  {job}-renamed:\n')):
                with self.subTest(job=job, replacement=after):
                    self.assertIn(f'gke-pr.yml is missing job {job}',
                                  self.gke_mutated_findings(None, before, after))
            self.assertIn(f'gke-pr.yml {job} must keep its default check name',
                          self.gke_mutated_findings(
                              job, '    runs-on:', '    name: renamed\n    runs-on:'))

    def test_ci_split_refuses_heavy_pr_reentry_and_lost_full_events(self):
        ci = self.ci_sources()[6]
        self.assertEqual(set(_jobs(ci)), set(POST_CI_JOB_IDS))
        for job in POST_CI_JOB_IDS:
            block = '  ' + job + ':' + _jobs(ci)[job]
            for before, after in ((block, ''), (f'  {job}:\n', f'  {job}-renamed:\n')):
                with self.subTest(job=job, replacement=after):
                    self.assertIn(f'ci-post.yml is missing job {job}',
                                  self.mutated_file_findings(6, None, before, after))
        for job in HEAVY_CI_JOBS:
            condition, = ci_preflight._job_values(_jobs(ci)[job], 'if')
            before = f'    if: {condition}\n'
            self.assertNotIn('github.event_name', condition)
            finding = ('preflight-slow must run all deferred stages on full events only'
                       if job == 'preflight-slow' else
                       f'{job} must start alongside preflight, after authz and changes, '
                       'and skip only its classified path policy')
            mutations = [
                '', f'    if: {ci_preflight.AUTHZ_SUCCESS}\n', '    if: false\n',
            ]
            for event in ('push', 'schedule', 'workflow_dispatch'):
                mutations += [
                    before.replace("needs.authz.result == 'success'",
                                   f"needs.authz.result == 'success' && github.event_name == '{event}'"),
                    before.replace("needs.authz.result == 'success'",
                                   f"needs.authz.result == 'success' && github.event_name != '{event}'"),
                ]
            for after in mutations:
                with self.subTest(job=job, condition=after):
                    self.assertIn(finding, self.mutated_file_findings(6, job, before, after))
        # A second reusable call must not smuggle a heavy profile onto PRs.
        extra = ('  heavy-again:\n    needs: [authz]\n'
                 '    uses: ./.github/workflows/native-live.yml\n\n')
        findings = self.mutated_file_findings(0, None, '  authz:\n', extra + '  authz:\n')
        self.assertIn('ci.yml must retain only its authz job after PR legs move to gke-pr', findings)
        self.assertIn('gke-pr.yml: reusable workflow calls are refused',
                      self.gke_mutated_findings(None, '  result:\n', extra + '  result:\n'))

    def test_preflight_tiers_partition_every_existing_stage(self):
        # Expectations are independent of SLOW_PREFLIGHT_STAGES: losing a
        # classification must not also remove its control.
        slow_names = {'verifier-tests', 'kernel-residuals-tests',
                      'unittest-shards-tests', 'run-json-harness-tests'}
        for event in ('pull_request', 'push', 'schedule', 'workflow_dispatch'):
            for answer in ('false', 'true', ''):
                with self.subTest(event=event, answer=answer):
                    env = {'GITHUB_EVENT_NAME': event, 'PINEFORGE_VERIFIER_SLOW': answer}
                    all_stages = check_commands(ROOT, env={})
                    fast = check_commands(ROOT, env=env, tier='fast')
                    slow = check_commands(ROOT, env=env, tier='slow')
                    self.assertEqual({entry[0] for entry in slow}, slow_names)
                    self.assertTrue(fast)
                    self.assertFalse({entry[0] for entry in fast} & slow_names)
                    self.assertEqual(sorted(fast + slow), sorted(all_stages))
                    self.assertEqual(len({entry[0] for entry in fast + slow}), len(all_stages))
                    self.assertTrue({'doc-lint', 'doc-anchors', 'doc-reverts',
                                     'ci-workflow-contract', 'preflight-tests'} <=
                                    {entry[0] for entry in fast})
                    self.assertTrue({name for name, _ in ci_verify.source_guard_commands(ROOT)} <=
                                    {entry[0] for entry in fast})
                    self.assertNotIn('--class-tier', dict(slow)['verifier-tests'])
        with self.assertRaises(ValueError):
            check_commands(ROOT, tier='missing')

    def test_github_only_preflight_runs_only_workflow_contract_stages(self):
        expected = {'shellcheck-version', 'actionlint-runner-labels', 'workflow-lint',
                    'ci-workflow-contract', 'docs-workflow-contract', 'docs-workflow-contract-tests'}
        plan = check_commands(ROOT, tier='github-only')
        self.assertEqual({name for name, _ in plan}, expected)
        self.assertEqual(len(plan), len(expected))
        self.assertTrue({plan_name for plan_name, _ in plan} <=
                        {name for name, _ in check_commands(ROOT, tier='fast')})
        original = self.ci_sources()[5]
        self.assertEqual(ci_preflight.docs_workflow_findings(original), [])
        for after in ('  pull_request: {}\n',
                      "  pull_request:\n    paths-ignore: ['.github/**', 'scripts/**']\n"):
            changed = original.replace(ci_preflight.DOCS_PR_TRIGGER, after, 1)
            self.assertNotEqual(changed, original, 'the mutation must change the trigger')
            self.assertIn('docs.yml must exclude only .github-only PRs from documentation builds',
                          ci_preflight.docs_workflow_findings(changed))

    def test_preflight_split_refuses_slow_stages_on_pr_and_missing_deferred_stages(self):
        for tier in ('all', 'fast', 'slow', 'github-only', ''):
            findings = self.gke_mutated_findings(
                'preflight', '--tier "$TIER"', '--tier ' + tier)
            self.assertIn('gke-pr.yml preflight must run the selected fast or github-only tier', findings)
        for tier in ('all', 'fast', ''):
            findings = self.mutated_file_findings(6, 'preflight-slow', '--tier slow',
                                                  f'--tier {tier}' if tier else '')
            self.assertIn('preflight-slow must run all deferred stages on full events only', findings)
        for after in (
                '          sudo apt-get update\n          cmake --build build\n',
                '          sudo apt-get update\n          python3 -m pip install pytest\n',
                '          sudo apt-get update\n          rustup toolchain install stable\n'):
            findings = self.gke_mutated_findings(
                'preflight', '          sudo apt-get update\n', after)
            self.assertIn('gke-pr.yml preflight must contain only checkout, linters and source checks', findings)
        for step in (
                '      - run: cmake --build build\n',
                '      - uses: actions/setup-python@a26af69be951a213d495a4c3e4e4022e16d87065 # v5.6.0\n',
                '      - run: python3 scripts/ci_verify.py release\n'):
            findings = self.gke_mutated_findings('preflight', '    steps:\n',
                                                 '    steps:\n' + step)
            self.assertIn('gke-pr.yml preflight must contain only checkout, linters and source checks', findings)

    def test_ci_smoke_refuses_missing_builds_or_wrong_ctest_selection(self):
        mutations = [
            ('-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_BUILD_TYPE=Debug'),
            ('--target pineforge_pr_smoke', '--target pineforge'),
            (f'          cmake --build build --target pineforge_pr_smoke --parallel {CORES}\n', ''),
            ("-L '^fast$'", "-L '^slow$'"),
            ("-L '^fast$'", ''),
            ('--no-tests=error', ''),
            ('--parallel ' + CORES, '--parallel 4'),
            ('ctest --test-dir build', 'ctest --test-dir build --include-label fast'),
        ]
        for before, after in mutations:
            with self.subTest(replacement=after):
                findings = self.gke_mutated_findings('build', before, after)
                self.assertIn('gke-pr.yml build must run its complete fast Release smoke', findings)
        findings = self.gke_mutated_findings(
            'build', '    steps:\n',
            '    steps:\n      - name: Install the CTest pytest runner\n        run: python3 -m pip install pytest\n')
        self.assertIn('gke-pr.yml build must install only its smoke dependencies', findings)

    def test_verification_steps_cannot_skip_their_home_events(self):
        for index, job, step_name, finding in (
                (6, 'preflight', 'Verify workflow wiring and source guards',
                 'preflight must invoke only its fast stage plan'),
                (6, 'preflight-slow', 'Verify deferred preflight stages',
                 'preflight-slow must run all deferred stages on full events only'),
                (6, 'build-debug', 'Verify (${{ matrix.build_type }})',
                 'build-debug must run the fast Release smoke only on PRs and full profiles otherwise'),
                (6, 'build', 'Verify (${{ matrix.build_type }})',
                 'build must run the fast Release smoke only on PRs and full profiles otherwise'),
                (6, 'build-arm', 'Verify (${{ matrix.build_type }})',
                 'build-arm must run the fast Release smoke only on PRs and full profiles otherwise')):
            ci = self.ci_sources()[index]
            step = ci_preflight._step_named(ci_preflight._steps(_jobs(ci)[job]),
                                            '      - name: ' + step_name)
            self.assertTrue(step)
            for condition in (ci_preflight.AUTHZ_FULL_SUCCESS, '${{ false }}', ''):
                after = step.replace(f'        if: {ci_preflight.AUTHZ_SUCCESS}\n',
                                     f'        if: {condition}\n' if condition else '', 1)
                self.assertNotEqual(after, step)
                with self.subTest(job=job, condition=condition):
                    findings = self.mutated_file_findings(index, job, step, after)
                    self.assertIn(finding, findings)

    def test_ci_smoke_and_full_builds_refuse_all_actions_cache_readers_and_writers(self):
        for name, jobs in (('gke-pr.yml', ('build', 'build-debug')),
                           ('ci-post.yml', ('build', 'build-debug', 'build-arm'))):
            workflow = self.workflow_sources()[name]
            for job in jobs:
                self.assertNotIn('actions/cache', _jobs(workflow)[job])
                for reference in ('actions/cache/restore', 'actions/cache/save', 'actions/cache'):
                    for key in ('ccache-main', 'ccache-pr', '${{ steps.ccache-restore.outputs.cache-primary-key }}'):
                        with self.subTest(workflow=name, job=job, reference=reference, key=key):
                            step = (f'      - uses: {reference}@0057852bfaa89a56745cba8c7296529d2fc39830\n'
                                    '        with:\n          path: .ccache\n'
                                    f'          key: {key}\n          restore-keys: ccache-main-\n')
                            findings = (self.gke_mutated_findings(job, '    steps:\n', '    steps:\n' + step)
                                        if name == 'gke-pr.yml' else
                                        self.mutated_file_findings(6, job, '    steps:\n', '    steps:\n' + step))
                            finding = ('gke-pr.yml: admission legs must not use Actions cache'
                                       if name == 'gke-pr.yml' else
                                       f'{job} must build cold without an Actions cache')
                            self.assertIn(finding, findings)

    def test_fast_label_rows_and_build_dependencies_are_pinned(self):
        cmake = self.ci_sources()[3]
        rows = ci_preflight.fast_rows(cmake)
        self.assertEqual(rows, [
            'test_ringbuffer', 'test_timeframe', 'test_path_at', 'test_na_runtime',
            'test_math_sum', 'test_str_utils', 'test_ta', 'test_matrix',
            'test_native_run_spec', 'test_native_bare_host_contracts',
            'test_native_kernel_edge'])
        mutations = [
            ('    test_ringbuffer\n', ''),
            ('    test_ringbuffer\n', '    test_ringbuffer\n    test_ringbuffer\n'),
            ('    test_ringbuffer\n', '    test_missing\n'),
            ('APPEND PROPERTY LABELS fast', 'APPEND PROPERTY LABELS slow'),
            ('add_custom_target(pineforge_pr_smoke DEPENDS ${PINEFORGE_PR_FAST_TESTS})',
             'add_custom_target(pineforge_pr_smoke DEPENDS pineforge)'),
            ('OR NOT TARGET ${_pf_fast_test}', 'OR FALSE'),
            ('if(_pf_fast_disabled OR NOT _pf_fast_skip_code STREQUAL "NOTFOUND"',
             'if(FALSE'),
        ]
        fast_block = cmake[cmake.index('set(PINEFORGE_PR_FAST_TESTS\n'):]
        for before, after in mutations:
            with self.subTest(replacement=after):
                changed_block = fast_block.replace(before, after, 1)
                self.assertNotEqual(changed_block, fast_block)
                findings = self.mutated_file_findings(3, None, fast_block, changed_block)
                self.assertIn('PR fast rows must keep one pinned CMake label list and executable build target',
                              findings)

    def test_fast_rows_refuse_property_writes_after_the_registration_block(self):
        sources = self.ci_sources()
        cmake = sources[3]
        finding = 'PR fast rows must keep one pinned CMake label list and executable build target'
        self.assertIn('test_ta', ci_preflight.fast_rows(cmake))
        self.assertNotIn(finding, ci_workflow_findings(*sources, others=self.other_sources()))
        self.assertNotIn(
            finding, self.mutated_file_findings(
                3, None, cmake, cmake + '\n# A comment after registration is harmless.\n'))
        for statement in (
                'set_tests_properties(test_ta PROPERTIES LABELS "cr999")',
                'set_tests_properties(test_ta PROPERTIES SKIP_RETURN_CODE 77)',
                'set_property(TEST test_ta APPEND PROPERTY LABELS "cr999")',
                'add_test(NAME test_ta_x COMMAND true)'):
            with self.subTest(statement=statement):
                findings = self.mutated_file_findings(
                    3, None, cmake, cmake + '\n' + statement + '\n')
                self.assertIn(finding, findings)

    def test_fast_rows_cannot_be_rewritten_after_validation(self):
        sources = self.ci_sources()
        block = ci_preflight.FAST_TEST_REGISTRATION
        finding = 'PR fast rows must keep one pinned CMake label list and executable build target'
        self.assertEqual(sources[3].count(block), 1)
        # A terminal block is the positive control even on the BASE tree,
        # whose real block precedes the lane includes.
        sources[3] = sources[3].replace(block, '', 1).rstrip() + '\n' + block
        self.assertNotIn(finding, ci_workflow_findings(*sources))
        for properties in ('SKIP_RETURN_CODE 1', 'LABELS "slow"'):
            with self.subTest(properties=properties):
                changed = list(sources)
                changed[3] += f'set_tests_properties(test_ta PROPERTIES {properties})\n'
                self.assertIn(finding, ci_workflow_findings(*changed))

    def test_fast_validation_cannot_precede_a_lane_include(self):
        sources = self.ci_sources()
        block = ci_preflight.FAST_TEST_REGISTRATION
        finding = 'PR fast rows must keep one pinned CMake label list and executable build target'
        self.assertEqual(sources[3].count(block), 1)
        sources[3] = sources[3].replace(block, '', 1).rstrip() + '\n' + block
        self.assertNotIn(finding, ci_workflow_findings(*sources))
        sources[3] += 'include("${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/late-properties.cmake")\n'
        self.assertIn(finding, ci_workflow_findings(*sources))

    def test_reusable_heavy_profiles_keep_full_populations_and_dispatch(self):
        for before, after in (
                ('  workflow_dispatch:\n', ''),
                ('  workflow_call:\n', '  pull_request:\n  workflow_call:\n')):
            findings = self.mutated_file_findings(1, None, before, after)
            self.assertIn('native-live.yml must retain only workflow_call and workflow_dispatch', findings)
        for profile, build_dir in (('native', 'build-live'),
                                   ('live-sanitizers', 'build-live-sanitizers'),
                                   ('live-tsan', 'build-live-tsan')):
            block = f'          - profile: {profile}\n            build_dir: {build_dir}\n'
            job = 'native-live-shards' if profile == 'native' else 'native-live'
            findings = self.mutated_file_findings(1, job, block, '')
            finding = ('native-live-shards must run exactly native shards 1..4 with fail-fast false'
                       if profile == 'native' else
                       'native-live must run both independent sanitizer profiles with fail-fast false, '
                       'the platform job maximum and no per-run budget')
            self.assertIn(finding, findings)
        for after in ('true', "${{ github.event_name == 'pull_request' }}"):
            findings = self.mutated_file_findings(6, 'native-live', 'exclude_slow: false',
                                                  f'exclude_slow: {after}')
            self.assertIn('native-live must receive the full population input', findings)
        for job, mode, wrong in (('corpus-parity', 'full', 'subset'),
                                 ('corpus-parity-subset', 'subset', 'full')):
            findings = self.mutated_file_findings(6, job, f'mode: {mode}', f'mode: {wrong}')
            self.assertIn(f'{job} must call corpus-parity.yml with mode {mode}', findings)

    def test_corpus_parity_accepts_unmodified_triggers_and_full_job_guard(self):
        sources = self.ci_sources()
        self.assertEqual(ci_preflight.corpus_parity_trigger_findings(sources[4]), [])
        self.assertEqual(ci_workflow_findings(*sources, others=self.other_sources()), [])

    def test_corpus_parity_rejects_readded_pull_request_trigger(self):
        sources = self.ci_sources()
        original = sources[4]
        trigger = ("  pull_request:\n"
                   "    branches: [main]\n"
                   "    paths:\n"
                   "      - 'corpus'\n")
        sources[4] = original.replace('on:\n', 'on:\n' + trigger, 1)
        self.assertNotEqual(sources[4], original)
        self.assertIn(trigger, sources[4])
        self.assertIn('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                      'and workflow_call without commented-out triggers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_removed_full_job_pull_request_guard(self):
        sources = self.ci_sources()
        original = sources[4]
        sources[4] = in_job(original, 'corpus-parity',
                            " && github.event_name != 'pull_request'", '')
        self.assertNotEqual(sources[4], original)
        self.assertIn('corpus-parity.yml job corpus-parity must exclude pull_request callers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_true_short_circuit_in_full_job_guard(self):
        sources = self.ci_sources()
        original = sources[4]
        guard = " && github.event_name != 'pull_request'"
        sources[4] = in_job(original, 'corpus-parity', guard, guard + ' || true')
        self.assertNotEqual(sources[4], original)
        self.assertIn(guard + ' || true', sources[4])
        self.assertIn('corpus-parity.yml job corpus-parity must exclude pull_request callers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_added_pull_request_target_trigger(self):
        sources = self.ci_sources()
        original = sources[4]
        sources[4] = original.replace('on:\n', 'on:\n  pull_request_target:\n', 1)
        self.assertNotEqual(sources[4], original)
        self.assertIn('  pull_request_target:\n', sources[4])
        self.assertIn('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                      'and workflow_call without commented-out triggers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_empty_pull_request_trigger(self):
        sources = self.ci_sources()
        original = sources[4]
        sources[4] = original.replace('on:\n', 'on:\n  pull_request:\n', 1)
        self.assertNotEqual(sources[4], original)
        self.assertIn('  pull_request:\n', sources[4])
        self.assertIn('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                      'and workflow_call without commented-out triggers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_commented_out_trigger(self):
        for event in ('pull_request', 'pull_request_target', 'push', 'schedule',
                      'workflow_dispatch', 'workflow_call'):
            with self.subTest(event=event):
                sources = self.ci_sources()
                original = sources[4]
                trigger = f'  # {event}:\n'
                sources[4] = original.replace('on:\n', 'on:\n' + trigger, 1)
                self.assertNotEqual(sources[4], original)
                self.assertIn(trigger, sources[4])
                self.assertIn('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                              'and workflow_call without commented-out triggers',
                              ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_removed_workflow_dispatch_trigger(self):
        sources = self.ci_sources()
        original = sources[4]
        sources[4] = original.replace('  workflow_dispatch:\n', '', 1)
        self.assertNotEqual(sources[4], original)
        self.assertIn('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                      'and workflow_call without commented-out triggers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_corpus_parity_rejects_added_push_trigger(self):
        sources = self.ci_sources()
        original = sources[4]
        sources[4] = original.replace('on:\n', 'on:\n  push:\n', 1)
        self.assertNotEqual(sources[4], original)
        self.assertIn('  push:\n', sources[4])
        self.assertIn('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                      'and workflow_call without commented-out triggers',
                      ci_workflow_findings(*sources, others=self.other_sources()))

    def test_full_profiles_refuse_every_label_or_class_exclusion(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original), [])
        flag = PR_SLOW_FLAG
        for job in ('build', 'kernel-only', 'sanitizers'):
            command = ' --ccache'
            mutations = [
                flag, flag + ' ' + flag,
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
                    findings = self.mutated_file_findings(6, job, command, command + ' ' + after)
                    self.assertIn(f'{job} must keep its full population', findings)
            for after in ('--include-label fast', '--class-tier fast'):
                with self.subTest(job=job, after=after):
                    findings = self.mutated_file_findings(6, job, command, command + ' ' + after)
                    self.assertIn(f'{job} must keep its full population', findings)
        # A fast preflight or parity subset with any exclusion also fails.
        # Both admitted jobs run inline steps; inject the flag into just one
        # job and require that job's full-population refusal.
        for job in ('preflight', 'corpus-parity-subset'):
            with self.subTest(job=job):
                after = f'    steps:\n      - run: python3 scripts/ci_verify.py debug {flag}\n'
                changed = in_job(self.gke(), job, '    steps:\n', after)
                findings = self.gke_mutated_findings(job, '    steps:\n', after)
                self.assertIn(f'{job} must keep its full population', findings)
                self.assertEqual(sum('--exclude-label' in body for body in _jobs(changed).values()), 1)

    def assert_release_kernel_native_full_debug_sanitizers_exclude_slow(self, ci, release):
        jobs = _jobs(ci)
        # Full verification no longer carries a label exclusion on any event.
        self.assertEqual({job for job, body in jobs.items() if '--exclude-label' in body},
                         set())
        for body in jobs.values():
            self.assertNotIn('--exclude-label', body)
        self.assertNotIn('--exclude-label', release)
        self.assertNotIn('--class-tier', release)
        # Read the real workflow and CMake registration, rather than deriving
        # expectations from the same selector that constructs the commands.
        self.assertIn('exclude_slow: false', jobs['native-live'])
        cmake = (ROOT / 'tests/CMakeLists.txt').read_text()
        self.assertIn('set(PINEFORGE_CI_VERIFY_SLICE_COUNT 8)', cmake)
        self.assertIn('foreach(_pf_ci_verify_slice RANGE 1 ${PINEFORGE_CI_VERIFY_SLICE_COUNT})',
                      cmake)
        row = re.search(r'add_test\(NAME test_ci_verify_slice_\$\{_pf_ci_verify_slice\}'
                        r'\s+(.*?)\)', cmake, re.DOTALL)
        self.assertIsNotNone(row)
        self.assertIn('scripts/test_ci_verify.py', row.group(1))
        self.assertIn('--slice ${_pf_ci_verify_slice}/${PINEFORGE_CI_VERIFY_SLICE_COUNT}',
                      row.group(1))
        self.assertNotIn('--class-tier', row.group(1))
        for profile in ('release', 'debug', 'kernel', 'sanitizers', 'native',
                        'live-sanitizers', 'live-tsan'):
            cfg = ci_verify.build_config([profile])
            # L6 rule 2026-10-24 02:12: the Debug builds of the full registry
            # (debug, sanitizers) exclude label slow by default; every other
            # profile, including the runner-only Debug ones, runs every row.
            default_exclusion = ['-LE', 'slow'] if profile in ('debug', 'sanitizers') else []
            self.assertEqual(ci_verify.ctest_command(cfg, cfg.build_dir, jobs=1, junit=False), [
                'ctest', '--test-dir', str(cfg.build_dir), '--output-on-failure',
                '--no-tests=error', '--parallel', '1', '--timeout', '0'] + default_exclusion)
        discovered = set(discover_test_ids(ROOT / 'scripts/test_ci_verify.py'))
        self.assertTrue(discovered)
        for event in ('push', 'workflow_dispatch', 'schedule', 'release', ''):
            for slow_answer in ('false', 'true', ''):
                with self.subTest(event=event, slow_answer=slow_answer):
                    env = {'GITHUB_EVENT_NAME': event, 'PINEFORGE_VERIFIER_SLOW': slow_answer}
                    self.assertEqual(verifier_class_tier(env), 'all')
                    self.assertEqual(set(select_test_ids(sorted(discovered),
                                                         verifier_class_tier(env))), discovered)
                    plan = {name: argv for name, argv in check_commands(ROOT, env=env, tier='slow')}
                    self.assertEqual(plan['verifier-tests'], [
                        sys.executable, str(ROOT / 'scripts/run_unittest_shards.py'),
                        str(ROOT / 'scripts/test_ci_verify.py'), '--log-dir',
                        str(ROOT / 'build-ci-preflight/verifier-tests-shards')])

    def test_release_kernel_native_keep_rows_debug_sanitizers_exclude_slow_all_classes(self):
        # CR-233 keeps the full-event profiles in ci-post.yml (CI_SOURCES[6]).
        ci = self.ci_sources()[6]
        release = (ROOT / '.github/workflows/release.yml').read_text()
        self.assert_release_kernel_native_full_debug_sanitizers_exclude_slow(ci, release)
        # A push path carrying an exclusion must break this coverage control.
        for job in ('build', 'kernel-only', 'sanitizers'):
            for flag in ("${{ github.event_name == 'push' && '--exclude-label slow' || '' }}",
                         '--exclude-label slow'):
                with self.subTest(job=job, flag=flag):
                    changed = in_job(ci, job, ' --ccache', ' --ccache ' + flag)
                    self.assertNotEqual(changed, ci)
                    with self.assertRaises(AssertionError):
                        self.assert_release_kernel_native_full_debug_sanitizers_exclude_slow(
                            changed, release)
                    sources = self.ci_sources()
                    sources[6] = changed
                    self.assertNotEqual(ci_workflow_findings(*sources), [])
        with self.assertRaises(AssertionError):
            self.assert_release_kernel_native_full_debug_sanitizers_exclude_slow(
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
                changed[6] = in_job(changed[6], job, before, after)
                self.assertNotEqual(changed[6], original[6])
                self.assertNotEqual(ci_workflow_findings(*changed, others=others), [])

    def test_ci_contract_pins_spot_runners_time_limits_and_parallelism(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(PR_RUNNER, '["self-hosted","pf-gcp-pr"]')
        self.assertEqual(json.loads(PR_RUNNER), ['self-hosted', 'pf-gcp-pr'])
        self.assertEqual(MATRIX_LINUX_RUNNER, '\'["self-hosted","pf-gcp-pr"]\'')
        self.assertEqual(MATRIX_RUNNER, '${{ fromJSON(matrix.platform.runner) }}')
        self.assertEqual(HOSTED_MATRIX_RUNNER, '${{ matrix.runner }}')
        self.assertEqual(RELEASE_RUNNER, '["self-hosted","pf-gcp-main"]')
        self.assertEqual(ci_preflight.HOSTED_JOB_RUNNERS,
                         {'ci.yml': {'authz': 'ubuntu-24.04'},
                          'gke-pr.yml': {'admit': 'ubuntu-24.04', 'legs': 'ubuntu-24.04',
                                         'result': 'ubuntu-24.04', **EXPECTED_GKE_JOBS['gke-pr.yml']},
                          'ci-post.yml': {'authz': GKE_2, 'changes': GKE_2,
                                          'build-release-gate': GKE_2,
                                          'build-debug-gate': GKE_2,
                                          'sanitizers-gate': GKE_2,
                                          'kernel-only-gate': GKE_2,
                                          **EXPECTED_GKE_JOBS['ci-post.yml'],
                                          'build-gate': GKE_2},
                          'corpus-parity.yml': {'authz': GKE_4,
                                                 **EXPECTED_GKE_JOBS['corpus-parity.yml']},
                          'docs.yml': {'authz': 'ubuntu-24.04', 'build': 'ubuntu-24.04'},
                          'docs-post.yml': {'authz': GKE_2},
                          'native-live.yml': {'authz': GKE_4,
                                              **EXPECTED_GKE_JOBS['native-live.yml'],
                                              'native-live-native': GKE_4,
                                              'native-live-gate': GKE_4},
                          'promote-baseline.yml': {'promote-guard': GKE_2},
                          'release-ruleset-probe.yml': {'authz': GKE_2}})
        self.assertEqual(JOB_COMMANDS['corpus-parity.yml'], {
            'authz': None,
            'corpus-parity': ci_preflight.PARITY,
            'corpus-parity-subset': ci_preflight.PARITY,
        })
        self.assertEqual(JOB_COMMANDS['docs.yml'], {
            'authz': None, 'build': None,
        })
        self.assertEqual(ci_preflight.RELEASE_JOB_RUNNERS, {
            'release.yml': {
                'release': '["self-hosted","pf-gcp-main"]',
                'prebuilt': '${{ matrix.runner }}',
                'notify-hub': '["self-hosted","pf-gcp-main"]'},
            'release-ruleset-probe.yml': {
                'probe': '["self-hosted","pf-gcp-post"]'},
            'promote-baseline.yml': {
                'promote': '["self-hosted","pf-gcp-main"]'},
        })
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        mutations = (
            # Each scoped pool job must keep its canon array without a hosted fallback.
            (6, 'preflight', runner_declaration(GKE_8), runner_declaration('pf-linux-x64-16'),
             f'ci-post.yml job preflight must run on {GKE_8}'),
            (6, 'sanitizers', runner_declaration(GKE_32), runner_declaration('macos-26-xlarge'),
             f'ci-post.yml job sanitizers must run on {GKE_32}'),
            (6, 'kernel-only', runner_declaration(GKE_16), runner_declaration('ubuntu-latest'),
             f'ci-post.yml job kernel-only must run on {GKE_16}'),
            # Target runs-on exactly, never an earlier matrix reference or name.
            (6, 'build', runner_declaration(GKE_32),
             '    runs-on: macos-26-large\n', f'ci-post.yml job build must run on {GKE_32}'),
            (6, 'build', runner_declaration(GKE_32),
             '    runs-on: ${{ matrix.larger_runner }}\n', f'ci-post.yml job build must run on {GKE_32}'),
            (1, 'native-live', runner_declaration(GKE_16), runner_declaration('pf-linux-x64-16'),
             f'native-live.yml job native-live must run on {GKE_16}'),
            (4, 'corpus-parity', runner_declaration(GKE_16), runner_declaration('pf-linux-x64-16'),
             f'corpus-parity.yml job corpus-parity must run on {GKE_16}'),
            (4, 'corpus-parity-subset', runner_declaration(GKE_8), runner_declaration('pf-linux-x64-16'),
             f'corpus-parity.yml job corpus-parity-subset must run on {GKE_8}'),
            # An event-based paid-runner route is forbidden, even for trusted events.
            (6, 'preflight', runner_declaration(GKE_8),
             runner_declaration("${{ github.event_name == 'push' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}"),
             f'ci-post.yml job preflight must run on {GKE_8}'),
            (1, 'native-live', runner_declaration(GKE_16),
             runner_declaration("${{ github.event_name == 'workflow_dispatch' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}"),
             f'native-live.yml job native-live must run on {GKE_16}'),
            (4, 'corpus-parity', runner_declaration(GKE_16),
             runner_declaration("${{ github.event_name == 'schedule' && 'pf-linux-x64-16' || 'ubuntu-24.04' }}"),
             f'corpus-parity.yml job corpus-parity must run on {GKE_16}'),
            (6, 'sanitizers', runner_declaration(GKE_32),
             runner_declaration("${{ github.event.pull_request.head.repo.full_name == github.repository "
                                "&& 'pf-linux-x64-16' || 'ubuntu-24.04' }}"),
             f'ci-post.yml job sanitizers must run on {GKE_32}'),
            (6, 'native-live', 'uses: ./.github/workflows/native-live.yml',
             'uses: someone/else/.github/workflows/native-live.yml@main',
             'ci-post.yml job native-live calls a workflow the runner contract cannot see; '
             'call one under ./.github/workflows/'),
            # Both split matrices and the PR's one-cell population are exact.
            (6, 'build-arm', '      matrix:\n', '      matrix:\n        os: [ubuntu-24.04-arm]\n',
             POST_ARM_BUILD_RUNNER_FINDING),
            (6, 'build', '        build_type: [Release]\n',
             '        build_type: [Release]\n        include:\n'
             '          - os: ubuntu-24.04\n            build_type: Debug\n',
             POST_BUILD_RUNNER_FINDING),
            (6, 'build', '        build_type: [Release]\n',
             '        build_type: [Release]\n        include:\n'
             '          - os: ubuntu-24.04\n            larger_runner: ubuntu-24.04\n',
             'ci-post.yml must not define a larger_runner key; use the GCP fleet or hosted exceptions'),
            # Block forms of runs-on, and job ids the parser must still see.
            (5, 'build', '    runs-on: ubuntu-24.04\n',
             '    runs-on:\n      group: pineforge-ci-large\n',
             'docs.yml job build must run on ubuntu-24.04'),
            (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n', '    runs-on:\n      - macos-26-xlarge\n',
             f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} (isolated release)'),
            (0, None, '  authz:\n', '  extra:\n    runs-on:\n      group: pineforge-ci-large\n'
             '    steps:\n      - run: "true"\n\n  authz:\n', 'ci.yml job extra needs a pinned runner and time limit'),
            (0, None, '  authz:\n', '  extra:\n    "runs-on": pf-linux-x64-16\n'
             '    steps:\n      - run: "true"\n\n  authz:\n', 'ci.yml job extra needs a pinned runner and time limit'),
            (5, 'build', '    runs-on: ubuntu-24.04\n', '    "runs-on": pf-linux-x64-16\n',
             'docs.yml job build must run on ubuntu-24.04'),
            (5, 'build', '    runs-on: ubuntu-24.04\n', '    runs-on : macos-26-xlarge\n',
             'docs.yml job build must run on ubuntu-24.04'),
            (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n', "    'runs-on': macos-26-xlarge\n",
             f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} (isolated release)'),
            (5, 'build', '    runs-on: ubuntu-24.04\n', '',
             'docs.yml job build must run on ubuntu-24.04'),
            (5, None, '\njobs:\n', '\njobs:\n  pre_build:\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml job pre_build needs a pinned runner and time limit'),
            (5, None, '\njobs:\n', '\njobs:\n  "pre-build":\n    runs-on: macos-26-xlarge\n    steps:\n'
             '      - run: "true"\n\n', 'docs.yml has jobs lines the runner contract cannot read'),
            (4, 'corpus-parity-subset', runner_declaration(GKE_8),
             '    runs-on:\n      group: pineforge-ci-large\n',
             f'corpus-parity.yml job corpus-parity-subset must run on {GKE_8}'),
            (0, 'authz', 'ubuntu-24.04', 'pf-linux-x64-16',
             'ci.yml job authz must run on ubuntu-24.04 (hosted)'),
            (5, 'build', '    runs-on: ubuntu-24.04\n', '    runs-on: pf-linux-x64-16\n',
             'docs.yml job build must run on ubuntu-24.04'),
            (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n', '    runs-on: macos-26-xlarge\n',
             f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} (isolated release)'),
            (0, None, '  authz:\n', '  extra:\n    runs-on: ubuntu-24.04\n    steps:\n'
             '      - run: "true"\n\n  authz:\n', 'ci.yml job extra needs a pinned runner and time limit'),
            # No job or step may declare timeout-minutes (owner #827,
            # 2026-10-20). Seed the deleted 360 line before each timeout
            # mutation: changing its value still fails; removing it passes.
            (6, 'preflight', 'timeout-minutes: 360', 'timeout-minutes: 30',
             'ci-post.yml job preflight must declare no timeout-minutes (owner #827)'),
            (6, 'build', 'timeout-minutes: 360', 'timeout-minutes: 45',
             'ci-post.yml job build must declare no timeout-minutes (owner #827)'),
            (6, 'sanitizers', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'ci-post.yml job sanitizers must declare no timeout-minutes (owner #827)'),
            (6, 'kernel-only', 'timeout-minutes: 360', 'timeout-minutes: 45',
             'ci-post.yml job kernel-only must declare no timeout-minutes (owner #827)'),
            (6, 'build-gate', '    timeout-minutes: 360\n', '', None),
            (6, 'changes', GKE_2, 'pf-linux-x64-16',
             f'ci-post.yml job changes must run on {GKE_2}'),
            (6, 'changes', '    timeout-minutes: 360\n', '    timeout-minutes: 300\n',
             'ci-post.yml job changes must declare no timeout-minutes (owner #827)'),
            (1, 'native-live', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'native-live.yml job native-live must declare no timeout-minutes (owner #827)'),
            (1, 'native-live-gate', 'timeout-minutes: 360', 'timeout-minutes: 60',
             'native-live.yml job native-live-gate must declare no timeout-minutes (owner #827)'),
            (4, 'corpus-parity', 'timeout-minutes: 360', 'timeout-minutes: 30',
             'corpus-parity.yml job corpus-parity must declare no timeout-minutes (owner #827)'),
            (4, 'corpus-parity-subset', 'timeout-minutes: 360', 'timeout-minutes: 10',
             'corpus-parity.yml job corpus-parity-subset must declare no timeout-minutes (owner #827)'),
            # A fixed count cannot follow the runner's available cores.
            (6, 'build', f'--jobs {CORES}', '--jobs 4', 'ci-post.yml job build must size its parallelism to the runner'),
            (6, 'sanitizers', 'scripts/ci_verify.py sanitizers', 'scripts/ci_verify.py release',
             'sanitizers must run its full profile on every shard'),
            (6, 'kernel-only', 'scripts/ci_verify.py kernel', 'scripts/ci_verify.py release',
             'kernel-only must run its full profile on every shard'),
            (6, 'sanitizers', f'--jobs {CORES}', '--jobs 4', 'ci-post.yml job sanitizers must size its parallelism to the runner'),
            (6, 'kernel-only', f'--jobs {CORES}', '--jobs "$(nproc)"', 'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (1, 'native-live', f'--jobs {CORES}', '--jobs 4', 'native-live.yml job native-live must size its parallelism to the runner'),
            (4, 'corpus-parity', f'JOBS={CORES} ./scripts', './scripts',
             'corpus-parity.yml job corpus-parity must size its parallelism to the runner'),
            (4, 'corpus-parity-subset', '          BUILD_DIR: build-corpus-parity-subset\n',
             '          BUILD_DIR: build-corpus-parity-subset\n          JOBS: "4"\n',
             'corpus-parity.yml job corpus-parity-subset must size its parallelism to the runner'),
            # ... through a multi-line run block, an = spelling, or a later flag.
            (6, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES}',
             'run: |\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'sanitizers', f'--jobs {CORES}', '--jobs=4', 'ci-post.yml job sanitizers must size its parallelism to the runner'),
            (6, 'build', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --jobs=4',
             'ci-post.yml job build must size its parallelism to the runner'),
            (6, 'kernel-only', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --job 4',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'sanitizers', f'--jobs {CORES} --ccache', f'--jobs {CORES} --ccache --jo=4',
             'ci-post.yml job sanitizers must size its parallelism to the runner'),
            (6, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
             'working-directory: scripts\n        run: python3 ci_verify.py kernel --build-dir ../build-kernel --ccache',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
             'run: $VERIFY', 'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n'
             '      - run: PYTHONPATH=scripts python3 -m ci_verify kernel --build-dir build-kernel --ccache\n',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            # A second, unsized call however it is chained, blocked or carried.
            (6, 'kernel-only', KERNEL_VERIFY,
             f'run: >-\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache'
             '\n\n          python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - uses: nick-fields/retry@v3\n        with:\n'
             '          command: python3 scripts/ci_verify.py release --build-dir build-rel --ccache\n',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY,
             KERNEL_VERIFY + ' && python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY,
             KERNEL_VERIFY + '; python3 scripts/ci_verify.py release --build-dir build-rel --ccache',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - run: python3 -X dev scripts/ci_verify.py release --build-dir build-rel --ccache\n',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY + '\n',
             KERNEL_VERIFY + '\n\n      - run: $pythonLocation/bin/python3 scripts/ci_verify.py release '
             '--build-dir build-rel --ccache\n',
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
            (6, 'kernel-only', KERNEL_VERIFY, KERNEL_VERIFY + " ${{ '--jobs 4' }}",
             'ci-post.yml job kernel-only must size its parallelism to the runner'),
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
            (6, 'build', '    name: build-shard (${{ matrix.platform.os }}, ${{ matrix.build_type }}, ${{ matrix.shard }}/4)\n', '',
             'ci-post.yml build legs must keep their names: build-shard (${{ matrix.platform.os }}, ${{ matrix.build_type }}, ${{ matrix.shard }}/4)'),
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

        for index, job, command in SHARDED_VERIFY_RUNS:
            with self.subTest(file=CI_SOURCES[index], job=job, mutation='parallelism'):
                changed = original.copy()
                self.assertIn(command + '\n', _jobs(changed[index])[job])
                changed[index] = in_job(changed[index], job, command + '\n',
                                        command.replace(f'--jobs {CORES}', '--jobs 4') + '\n')
                self.assertIn(f'{Path(CI_SOURCES[index]).name} job {job} '
                              'must size its parallelism to the runner',
                              ci_workflow_findings(*changed, others=others))

    def test_verifier_looking_step_name_cannot_hide_an_unsized_call_from_mutated_file(self):
        original = self.ci_sources()
        self.assertEqual(ci_workflow_findings(*original, others=self.other_sources()), [])
        before = ci_preflight._step_named(ci_preflight._steps(_jobs(original[6])['kernel-only']),
                                          '      - name: Verify (kernel)')
        # Match name and run independently, preserving any intervening if/env keys.
        after = in_job(before, None, '      - name: Verify (kernel)\n',
                       f'      - name: python3 scripts/ci_verify.py kernel --jobs {CORES}\n')
        after = in_job(after, None,
                       f'run: python3 scripts/ci_verify.py kernel --build-dir build-kernel --jobs {CORES} --ccache',
                       'run: python3 -m ci_verify kernel --build-dir build-kernel --ccache')
        self.assertEqual(ci_preflight._step_values(after, 'if'), [ci_preflight.AUTHZ_SUCCESS])
        findings = self.mutated_file_findings(6, 'kernel-only', before, after)
        self.assertIn('ci-post.yml job kernel-only must size its parallelism to the runner', findings)

    def test_gcp_sites_reject_cross_class_routes(self):
        sites = list(self.gcp_sites())
        # Admitted PR 4 + post preflight/shards/arm 7 + native 2 + parity 2 + docs 1 + release 3
        # + probe 1 + promotion 1; the dispatch canary is a separate synthetic input.
        self.assertEqual(len(sites), 21)
        self.assertCountEqual([(name, job) for name, job, *_ in sites],
                              [(name, job) for name, jobs in FLEET_JOB_IDS.items()
                               for job in jobs if job not in LIGHT_JOB_IDS.get(name, ())
                               and (ci_preflight.HOSTED_JOB_RUNNERS.get(name, {}).get(job)
                                    in (None, POST_RUNNER) or job in EXPECTED_GKE_JOBS.get(name, {}))])
        self.assertEqual(ci_preflight.runner_findings(self.workflow_sources()), [])
        for name, job, fleet, before, finding in sites:
            correct = site_runner(name, job, fleet)
            for runner in {PR_RUNNER, POST_RUNNER, RELEASE_RUNNER} - {correct}:
                with self.subTest(workflow=name, job=job, wrong_fleet=runner):
                    findings = self.routed_file_findings(name, job, before, routed_line(before, runner))
                    self.assertIn(finding, findings)

    def test_gcp_sites_pin_probe_to_the_dispatch_fleet(self):
        sites = [site for site in self.gcp_sites() if site[0] == 'release-ruleset-probe.yml']
        self.assertEqual([(job, fleet) for _, job, fleet, *_ in sites],
                         [('probe', 'post')])
        self.assertEqual(sites[0][-1],
                         f'release-ruleset-probe.yml job probe must run on {POST_RUNNER} '
                         '(post fleet)')

    def test_dispatch_only_job_keeps_the_post_fleet(self):
        name, job = 'dispatch-inspection.yml', 'inspect'
        workflow = ('on: workflow_dispatch\njobs:\n  inspect:\n'
                    + runner_declaration(POST_RUNNER)
                    + '    steps:\n      - run: git --version\n')
        self.assertEqual(ci_preflight.runner_findings({name: workflow}), [])
        changed = in_job(workflow, job, runner_declaration(POST_RUNNER),
                         runner_declaration(PR_RUNNER))
        self.assertNotEqual(changed, workflow)
        self.assertIn(f'{name} job {job} must run on {POST_RUNNER} for post-merge events',
                      ci_preflight.runner_findings({name: changed}))

    def test_gcp_sites_reject_retired_and_void_labels(self):
        for name, job, fleet, before, finding in self.gcp_sites():
            runner = site_runner(name, job, fleet)
            for label in ('pf-ci', 'pf-ci-x64', 'pf-ci-x64-16', 'pf-ci-release',
                          'pf-gcp-engine-pr', 'pf-gcp-engine-main', 'pf-gcp-small'):
                # Refuse the wrong class alone and extra labels on a valid class.
                extra = (f'["{runner}","{label}"]'
                         if fleet == 'gke' else runner[:-1] + f',"{label}"]')
                for value in (f'["self-hosted","{label}"]', extra):
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
            runner = site_runner(name, job, fleet)
            for value in ('ubuntu-24.04', 'ubuntu-latest',
                          "${{ fromJSON('" + runner + "') || 'ubuntu-24.04' }}"):
                with self.subTest(workflow=name, job=job, runner=value):
                    findings = self.routed_file_findings(name, job, before, routed_line(before, value))
                    self.assertIn(finding, findings)

    def test_gcp_sites_require_the_literal_or_matrix_string_shape(self):
        for name, job, fleet, before, finding in self.gcp_sites():
            runner = site_runner(name, job, fleet)
            values = ("'" + runner + "'", json.dumps(runner))
            for value in values:
                with self.subTest(workflow=name, job=job, runner=value):
                    after = routed_line(before, value)
                    findings = self.routed_file_findings(name, job, before, after)
                    self.assertIn(finding, findings)

    def test_gcp_sites_require_every_routed_job(self):
        workflows = self.workflow_sources()
        for name, jobs in FLEET_JOB_IDS.items():
            self.assertIn(name, workflows, f'{name} required fleet workflow is missing')
            for job in jobs:
                with self.subTest(workflow=name, missing_job=job):
                    before = '  ' + job + ':' + _jobs(workflows[name])[job]
                    findings = self.routed_file_findings(name, None, before, '')
                    self.assertIn(f'{name} is missing job {job}', findings)

    def test_authz_header_spacing_and_comment_are_verbatim(self):
        self.assertEqual(ci_preflight.runner_findings(self.light_workflows()), [])
        comment = '    # Fleet routing is fixed for this workflow; releases use pf-gcp-main.\n'
        for index in (0, 1, 4, 5, 6, 7):
            name = Path(CI_SOURCES[index]).name
            runner, = ci_preflight._job_values(_jobs(self.ci_sources()[index])['authz'],
                                               'runs-on', unquote=False)
            mutations = [(f'    runs-on: {runner}\n', f'    runs-on:  {runner}\n')]
            if index == 0:
                mutations += [('          set -eu\n', '          set -ev\n'),
                              ('          set -eu\n', '          set +eu\n'),
                              ('          set -eu\n', '')]
            else:
                mutations += [(comment, comment.replace('fixed', 'dynamic')),
                              (comment, comment.replace('releases', 'jobs')),
                              (comment, '')]
            for before, after in mutations:
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
                    expected = EXPECTED_GKE_JOBS['corpus-parity.yml'][job]
                    before = runner_declaration(expected)
                    findings = self.routed_file_findings('corpus-parity.yml', job, before,
                                                         routed_line(before, value))
                    self.assertIn(f'corpus-parity.yml job {job} must run on {expected}', findings)

    def test_legacy_metadata_cannot_replace_runner_pins(self):
        workflows = self.workflow_sources()
        # Metadata is never a scheduler input, on either event class.
        sites = [site for site in self.gcp_sites()
                 if 'PF_CI_LABELS:' in _jobs(workflows[site[0]])[site[1]]]
        # Post preflight 1 + post parity 2; admitted jobs have no legacy metadata.
        self.assertEqual(len(sites), 3)
        self.assertCountEqual([(name, job) for name, job, *_ in sites], [
            ('ci-post.yml', 'preflight'),
            ('corpus-parity.yml', 'corpus-parity'),
            ('corpus-parity.yml', 'corpus-parity-subset'),
        ])
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
        before = runner_declaration(GKE_PR_8)
        for job in ('preflight', 'corpus-parity-subset'):
            with self.subTest(inline_job=job):
                changed = in_job(workflows['gke-pr.yml'], job, before,
                                 routed_line(before, RELEASE_RUNNER))
                changed = in_job(changed, job, '      - name: Checkout the admitted head\n',
                                 '      - name: Checkout the admitted head\n        env:\n'
                                 f"          PF_CI_LABELS: '{PR_RUNNER}'\n")
                findings = ci_preflight.runner_findings(dict(workflows, **{'gke-pr.yml': changed}))
                self.assertIn(f'gke-pr.yml job {job} must run on {GKE_PR_8}', findings)

    def test_ci_contract_pins_the_docs_only_skip(self):
        """Keep each pipeline's path policy and dependencies; PR suite jobs
        also require a ready PR, and post-merge preflight always runs."""
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        docs_skip = f'    if: {DOCS_ONLY_SKIP}\n'
        mutations = []
        for index, proofs, policy in ((6, PROOF_JOBS, DOCS_ONLY_SKIP),):
            wrong_skips = [f'    if: {ready_pr(condition) if index == 0 else condition}\n'
                           for condition in (
                               "${{ needs.changes.outputs.docs_only != 'true' }}",
                               "${{ !cancelled() && needs.changes.outputs.docs_only == 'false' }}",
                               "${{ !cancelled() && needs.changes.outputs.docs_only != 'true' }}",
                           )]
            for job in proofs:
                if index == 6 and job in ci_preflight.POST_LEG_GATES:
                    continue  # Per-leg dependencies and skips have independent exact controls.
                skip = f'    if: {policy}\n'
                refused = f'{job} must start alongside preflight, after authz and changes'
                mutations += [
                    (index, job, '    needs: [authz, changes]\n', '', refused),
                    (index, job, '    needs: [authz, changes]\n', '    needs: [changes, preflight]\n', refused),
                    (index, job, '    needs: [authz, changes]\n', '    needs: preflight\n', refused),
                    (index, job, skip, '', refused),
                    (index, job, skip, wrong_skips[0], refused),
                    (index, job, skip, wrong_skips[1], refused),
                    (index, job, skip, skip.replace("!= 'true'", "!= 'true' && github.event_name != 'push'"),
                     refused),
                    (index, job, skip, wrong_skips[2], refused)]
        mutations += [
            (6, 'preflight', '    needs: [authz, changes]\n', '',
             'preflight must follow authz and changes'),
            (6, 'preflight', '    needs: [authz, changes]\n', '    needs: build\n',
             'preflight must follow authz and changes'),
            (6, 'preflight', f'    if: {AUTHZ_ALWAYS}\n', docs_skip,
             'preflight must follow authz and changes'),
            (6, 'preflight', f'    if: {AUTHZ_ALWAYS}\n', '',
             'preflight must follow authz and changes'),
            (6, 'changes', '    steps:\n', '    needs: preflight\n    steps:\n',
             'changes must start after authz'),
            (6, 'changes', '    steps:\n',
             "    if: github.event_name != 'pull_request'\n    steps:\n",
             'changes must start after authz'),
            # A lane that continues on error reports success however it ended.
            (6, 'sanitizers', '    steps:\n', '    continue-on-error: true\n    steps:\n',
             'ci-post.yml job sanitizers must not continue on error'),
            (6, 'build-gate', '        shell: bash\n', '        shell: bash\n        continue-on-error: true\n',
             'ci-post.yml job build-gate must not continue on error'),
            (6, 'preflight', f'        run: {PREFLIGHT_RUN}\n',
             f'        run: {PREFLIGHT_RUN}\n'
             '        continue-on-error: true\n', 'ci-post.yml job preflight must not continue on error')]
        for index, job, before, after, finding in mutations:
            with self.subTest(job=job, after=after):
                changed = original.copy()
                changed[index] = in_job(changed[index], job, before, after)
                self.assertNotEqual(changed[index], original[index])
                findings = ci_workflow_findings(*changed, others=others)
                self.assertTrue(any(finding in line for line in findings), findings)
        # The same wiring spelled another way is no finding.
        for index, job, before, after in (
                (6, 'kernel-only', '    needs: [authz, changes]\n', '    needs: [changes, authz]\n'),
                (6, 'sanitizers', f'    if: {HEAVY_SKIP}\n', f'    if: "{HEAVY_SKIP}"\n'),
                (6, 'changes', '    steps:\n', '    # classify first\n    steps:\n')):
            with self.subTest(job=job, harmless=after):
                changed = original.copy()
                changed[index] = in_job(changed[index], job, before, after)
                self.assertEqual(ci_workflow_findings(*changed, others=others), [])
        for job in ('preflight', 'build', 'build-debug', 'corpus-parity-subset'):
            body = _jobs(self.gke())[job]
            condition, = ci_preflight._job_values(body, 'if')
            for before, after in (
                    ('    needs: [admit]\n', ''),
                    ('    needs: [admit]\n', '    needs: preflight\n'),
                    ('    needs: [admit]\n', '    needs: [admit, preflight]\n'),
                    (f'    if: {condition}\n', ''),
                    (f'    if: {condition}\n', '    if: always()\n'),
                    (f'    if: {condition}\n', f'    if: {DOCS_ONLY_SKIP}\n')):
                with self.subTest(job=job, after=after):
                    self.assertIn(f'gke-pr.yml {job} must follow admission and keep its path policy',
                                  self.gke_mutated_findings(job, before, after))
        for job in ('admit', 'preflight', 'legs', 'result'):
            self.assertIn(f'gke-pr.yml job {job} must not continue on error',
                          self.gke_mutated_findings(
                              job, '    steps:\n', '    continue-on-error: true\n    steps:\n'))

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
            ('            && [[ "$SLOW_PREFLIGHT_RESULT" == "success" ]] && passed "$FULL_PARITY_RESULT"\n',
             '            && [[ "$SLOW_PREFLIGHT_RESULT" == "success" ]] && passed "$FULL_PARITY_RESULT"\n'
             '          true\n'),
        )
        for before, after in mutations:
            with self.subTest(after=after):
                changed = original.copy()
                changed[6] = in_job(changed[6], 'build-gate', before, after)
                self.assertNotEqual(changed[6], original[6])
                self.assertIn(
                    'build-gate must keep the exact result policy: success or a classified docs-only skip',
                    ci_workflow_findings(*changed, others=others))

    def test_build_aggregate_requires_matrix_success_on_pr_and_full_events(self):
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
               'AUTHZ_RESULT': 'success',
               'CHANGES_RESULT': 'success', 'DOCS_ONLY': 'false',
               'PREFLIGHT_RESULT': 'success',
               'SLOW_PREFLIGHT_RESULT': 'success',
               **{variable: 'success' for variable in PROOF_JOBS.values()}}
        pr_script = job_script(self.gke(), 'legs', 'Require every proof leg')
        for variable in ('BUILD_RESULT', 'DEBUG_RESULT', 'PARITY_RESULT'):
            for outcome in ('success', 'failure', 'cancelled', 'skipped', ''):
                env_pr = dict.fromkeys(('BUILD_RESULT', 'DEBUG_RESULT', 'PARITY_RESULT'), 'success')
                result = subprocess.run(
                    ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', pr_script],
                    env={'PATH': env['PATH'], **env_pr, variable: outcome},
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0 if outcome == 'success' else 1,
                                 result.stdout + result.stderr)
        for event in ('push', 'workflow_dispatch', 'schedule', 'release'):
            script = job_script(self.ci_sources()[6], 'build-gate',
                                'Require every verification lane to have passed')
            matrices = [('BUILD_RESULT', 'matrix')]
            if event != 'pull_request':
                matrices.append(('ARM_BUILD_RESULT', 'arm64 matrix'))
            for variable, label in matrices:
                for outcome in ('success', 'failure', 'cancelled', 'skipped', ''):
                    with self.subTest(event=event, lane=variable, matrix=outcome):
                        result = subprocess.run(
                            ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
                            env={**env, 'GITHUB_EVENT_NAME': event, variable: outcome},
                            capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0 if outcome == 'success' else 1,
                                         result.stdout + result.stderr)
                        self.assertIn(f'{label}: {outcome}\n', result.stdout)

    def test_the_build_aggregate_passes_a_docs_only_skip_and_nothing_else(self):
        """Run the aggregate's own shell over every outcome of every lane: a
        proof lane passes by succeeding, or by being skipped after changes
        succeeded and answered docs-only; preflight always has to succeed."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        script = Path(directory.name) / 'gate.sh'
        script.write_text(job_script(self.ci_sources()[6], 'build-gate',
                                     'Require every verification lane to have passed'))
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'AUTHZ_RESULT': 'success',
               'GITHUB_EVENT_NAME': 'push', 'SLOW_PREFLIGHT_RESULT': 'success'}
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

    def github_only_aggregate(self, rows, *, files_response='fixture',
                              second_rows=None, results=None):
        """Run both compare readers, the real classifier, veto and result.

        Only gh API transport is replaced; classifier subprocesses and workflow
        scripts execute unchanged. A reader failure stops the remaining work.
        """
        ci = self.gke()
        readers = (
            job_script(ci, 'admit', 'Read the payload-bound compare file list'),
            job_script(ci, 'result', 'Re-read the payload-bound compare list'),
        )
        veto = job_script(ci, 'result', 'Apply the one-way code-extension veto')
        gate = job_script(ci, 'result', 'Require an independently consistent successful result')
        endpoint = 'repos/pfrel/pineforge-engine/compare/' + 'b' * 40 + '...' + 'a' * 40
        driver = r'''
import json
from pathlib import Path
import subprocess
import sys
from unittest import mock

fixture = json.loads(Path(sys.argv[1]).read_text())
requests = []
run = subprocess.run

def transport(argv, **kwargs):
    if argv[:2] != ["gh", "api"]:
        return run(argv, **kwargs)
    url = argv[2]
    requests.append(url)
    assert url.startswith(fixture["endpoint"] + "?per_page=100&page="), "compare binding mismatch"
    page = int(url.rsplit("=", 1)[1])
    if fixture["files_response"] is None:
        raise subprocess.CalledProcessError(1, argv, stderr="compare API unavailable")
    text = (json.dumps({"files": fixture["rows"][(page - 1) * 100:page * 100]})
            if fixture["files_response"] == "fixture" else fixture["files_response"])
    return subprocess.CompletedProcess(argv, 0, text, "")

try:
    with mock.patch("subprocess.run", side_effect=transport):
        exec(compile(Path(sys.argv[2]).read_text(), "gke-pr compare step", "exec"), {})
finally:
    Path(sys.argv[3]).write_text(json.dumps(requests))
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                   'REPO': 'pfrel/pineforge-engine', 'BASE_SHA': 'b' * 40, 'HEAD_SHA': 'a' * 40,
                   'PATH_FILE': str(root / 'paths'), 'GH_TOKEN': 'fixture-token',
                   'ADMIT_RESULT': 'success', 'ALLOWED': 'true',
                   'PREFLIGHT_RESULT': 'success', 'LEGS_RESULT': 'skipped',
                   **(results or {})}
            requests = []
            for index, reader in enumerate(readers):
                output = root / f'class-{index}'
                env['GITHUB_OUTPUT'] = str(output)
                (root / 'reader.py').write_text(reader)
                (root / 'transport.json').write_text(json.dumps({
                    'endpoint': endpoint,
                    'rows': second_rows if index and second_rows is not None else rows,
                    'files_response': files_response}))
                read_result = subprocess.run(
                    [sys.executable, '-c', driver, str(root / 'transport.json'),
                     str(root / 'reader.py'), str(root / 'requests.json')],
                    cwd=ROOT, env=env, capture_output=True, text=True)
                requests += json.loads((root / 'requests.json').read_text())
                self.assertNotIn('AssertionError', read_result.stderr, read_result.stderr)
                if read_result.returncode:
                    self.assertFalse(output.exists(), 'a failed reader must never publish a skip')
                    return read_result, requests
                if not index:
                    classified = subprocess.run(
                        [sys.executable, str(ROOT / 'scripts/ci_docs_only.py'),
                         '--paths', env['PATH_FILE'], '--github-output', str(output)],
                        cwd=ROOT, env=env, capture_output=True, text=True)
                    if classified.returncode:
                        return classified, requests
                values = dict(line.split('=', 1) for line in output.read_text().splitlines())
                env[('SECOND' if index else 'ADMIT') + '_DOCS_ONLY'] = values['docs_only']
                env[('SECOND' if index else 'ADMIT') + '_GITHUB_ONLY'] = values['github_only']
            env['GITHUB_OUTPUT'] = str(root / 'veto-output')
            checked = subprocess.run([sys.executable, '-c', veto], env=env,
                                     capture_output=True, text=True)
            if checked.returncode:
                return checked, requests
            env['CODE_EXTENSION'] = (root / 'veto-output').read_text().strip().split('=', 1)[1]
            result = subprocess.run(['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', gate],
                                    env=env, capture_output=True, text=True)
            return result, requests

    def test_github_only_aggregate_src_skip_fails(self):
        original = [{'filename': '.github/workflows/ci.yml', 'status': 'modified'}]
        changed = [dict(original[0], filename='src/c_abi.cpp')]
        self.assertNotEqual(changed, original, 'the mutation must change the API diff input')
        baseline, _ = self.github_only_aggregate(original)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        result, requests = self.github_only_aggregate(changed)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(any('/compare/' in url for url in requests))
        self.assertIn('proof legs did not pass', result.stdout)

    def test_github_only_aggregate_workflow_skip_passes(self):
        original = [{'filename': 'src/c_abi.cpp', 'status': 'modified'}]
        changed = [dict(original[0], filename='.github/workflows/ci.yml')]
        self.assertNotEqual(changed, original, 'the mutation must change the API diff input')
        baseline, _ = self.github_only_aggregate(original)
        self.assertNotEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        result, _ = self.github_only_aggregate(changed)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # The path exception excuses only skipped PR proofs, never a failed or
        # cancelled PR proof, or an unsuccessful authz/classification/preflight.
        # Full-event jobs are absent from this workflow (PR_PROOF_JOBS).
        baseline_results = {'ADMIT_RESULT': 'success', 'ALLOWED': 'true',
                            'PREFLIGHT_RESULT': 'success', 'LEGS_RESULT': 'skipped'}
        for variable in baseline_results:
            for outcome in ('failure', 'cancelled', ''):
                with self.subTest(lane=variable, result=outcome):
                    mutated = {**baseline_results, variable: outcome}
                    self.assertNotEqual(mutated, baseline_results, 'the mutation must change a lane result')
                    failed, _ = self.github_only_aggregate(changed, results=mutated)
                    self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
        for event in ('push', 'schedule', 'workflow_dispatch'):
            self.assertIn('gke-pr.yml: trigger must be exactly pull_request_target',
                          self.gke_mutated_findings(None, '  pull_request_target:\n',
                                                     f'  {event}:\n'))

    def test_github_only_aggregate_defers_only_full_event_lanes(self):
        # Pin the admitted PR proofs independently of the post-only set.
        self.assertEqual(PR_PROOF_JOBS,
                         {'build': 'BUILD_RESULT', 'build-debug': 'DEBUG_RESULT',
                          'corpus-parity-subset': 'PARITY_RESULT'})
        deferred = {'preflight-slow': 'SLOW_PREFLIGHT_RESULT',
                    **{job: variable for job, variable in PROOF_JOBS.items()
                       if job not in PR_PROOF_JOBS}}
        self.assertEqual(deferred, {
            'preflight-slow': 'SLOW_PREFLIGHT_RESULT', 'build-arm': 'ARM_BUILD_RESULT',
            'build-release-gate': 'RELEASE_GATE_RESULT',
            'build-debug-gate': 'DEBUG_GATE_RESULT', 'sanitizers-gate': 'SANITIZER_GATE_RESULT',
            'kernel-only-gate': 'KERNEL_GATE_RESULT',
            'sanitizers': 'SANITIZER_RESULT', 'kernel-only': 'KERNEL_RESULT',
            'native-live': 'NATIVE_RESULT', 'corpus-parity': 'FULL_PARITY_RESULT'})
        sources = self.ci_sources()
        pr_jobs, post_jobs = _jobs(self.gke()), _jobs(sources[6])
        pr_gate, post_gate = pr_jobs['legs'], post_jobs['build-gate']
        self.assertEqual(set(ci_preflight._needs(pr_gate)),
                         {'admit', *PR_PROOF_JOBS})
        self.assertEqual(ci_preflight._needs(pr_jobs['result']), ['admit', 'preflight', 'legs'])
        rows = [{'filename': '.github/workflows/ci.yml', 'status': 'modified'}]
        for job, variable in deferred.items():
            with self.subTest(job=job):
                self.assertNotIn(job, pr_jobs)
                self.assertNotIn(variable, pr_gate)
                self.assertIn(job, ci_preflight._needs(post_gate))
                self.assertIn(f'{variable}: ${{{{ needs.{job}.result }}}}', post_gate)
            for outcome in ('failure', 'cancelled', ''):
                with self.subTest(job=job, result=outcome):
                    result, _ = self.github_only_aggregate(rows, results={variable: outcome})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_github_only_aggregate_mixed_ci_script_skip_fails(self):
        original = [{'filename': '.github/workflows/ci.yml', 'status': 'modified'}]
        baseline, _ = self.github_only_aggregate(original)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        for filename in ('scripts/ci_preflight.py', 'scripts/ci_verify.py', 'scripts/ci_docs_only.py'):
            with self.subTest(filename=filename):
                changed = original + [{'filename': filename, 'status': 'modified'}]
                self.assertNotEqual(changed, original, 'the mutation must change the API diff input')
                result, _ = self.github_only_aggregate(changed)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('proof legs did not pass', result.stdout)

    def test_github_only_aggregate_empty_or_unreadable_diff_fails_closed(self):
        original = [{'filename': '.github/workflows/ci.yml', 'status': 'modified'}]
        all_success = {'LEGS_RESULT': 'success'}
        baseline, _ = self.github_only_aggregate(original)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        # Exercise empty, missing paths, malformed paths, and missing rename
        # origins through the real reader rather than a precomputed boolean.
        for changed in ([], [{}], [{'filename': '', 'status': 'modified'}],
                        [{'filename': '.github/../src/c_abi.cpp', 'status': 'modified'}],
                        [{'filename': '.github/workflows/ci.yml', 'status': 'renamed'}]):
            with self.subTest(rows=changed):
                self.assertNotEqual(changed, original, 'the mutation must change the API diff input')
                result, _ = self.github_only_aggregate(changed, results=all_success)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, '', 'the result shell must not run after reader failure')
        for response in ('', '{', '{}', '[]', None):
            with self.subTest(response=response):
                self.assertNotEqual(response, json.dumps(original),
                                    'the mutation must change the API response bytes')
                result, _ = self.github_only_aggregate(original, files_response=response,
                                                     results=all_success)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, '')
        for response in ('{"files": null}', '{"files": {}}', '{"files": [null]}'):
            with self.subTest(response=response):
                result, _ = self.github_only_aggregate(original, files_response=response,
                                                     results=all_success)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, '')

    def test_github_only_diff_reads_every_page_and_both_rename_paths(self):
        original = [{'filename': f'.github/workflows/check-{index}.yml', 'status': 'modified'}
                    for index in range(101)]
        baseline, requests = self.github_only_aggregate(original)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        self.assertTrue(any(url.endswith('page=2') for url in requests))
        changed = original[:-1] + [dict(original[-1], filename='scripts/ci_preflight.py')]
        self.assertNotEqual(changed, original, 'the mutation must change the second API page')
        result, requests = self.github_only_aggregate(changed)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(any(url.endswith('page=2') for url in requests))
        rename = [{'filename': '.github/workflows/new.yml', 'status': 'renamed',
                   'previous_filename': '.github/workflows/old.yml'}]
        baseline, _ = self.github_only_aggregate(rename)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        for key in ('filename', 'previous_filename'):
            changed = [dict(rename[0], **{key: 'scripts/ci_preflight.py'})]
            self.assertNotEqual(changed, rename, 'the mutation must change a rename side')
            result, _ = self.github_only_aggregate(changed)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        for changed in ([{'filename': 'src/c_abi.cpp'}], [{'filename': 'docs/ci.md'}]):
            result, requests = self.github_only_aggregate(rename, second_rows=changed)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(all('/compare/' + 'b' * 40 + '...' + 'a' * 40 in url
                                for url in requests))
            self.assertIn('classification changed', result.stdout)
        for before in ('test "$live_head" = "$HEAD_SHA"', 'test "$live_base" = main',
                       'test "$state" = open'):
            self.assertIn('gke-pr.yml admit must refuse drafts and stale or unauthorized identities',
                          self.gke_mutated_findings('admit', before, 'true'))

    def test_github_only_contract_refuses_previous_job_trust_and_reader_bypasses(self):
        original = self.gke()
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        reader = ci_preflight._step_named(ci_preflight._steps(_jobs(original)['result']),
                                           '        id: second-classify')
        for job, marker in (('admit', '      - name: Read the payload-bound compare file list'),
                            ('result', '        id: second-classify')):
            step = ci_preflight._step_named(ci_preflight._steps(_jobs(original)[job]), marker)
            env = '\n'.join(ci_preflight._step_block(step, 'env')[1:])
            self.assertEqual(ci_preflight._job_values(env, 'GH_TOKEN'), ['${{ github.token }}'])
            self.assertEqual(ci_preflight._job_values(env, 'REPO'), ['${{ github.repository }}'])
        for job, before, after, finding in (
                ('result', 'steps.second-classify.outputs.github_only', 'needs.admit.outputs.github_only',
                 'gke-pr.yml result must require admission, preflight and consistent proof results'),
                ('result', reader, '', 'gke-pr.yml result must read the immutable compare list independently'),
                ('admit', '      github_only: ${{ steps.classify.outputs.github_only }}',
                 '      github_only: true', 'gke-pr.yml admit must publish only the base classifier outputs'),
                ('admit', 'run: python3 scripts/ci_docs_only.py --paths "$PATH_FILE" --github-output "$GITHUB_OUTPUT"',
                 'run: echo github_only=true >> "$GITHUB_OUTPUT"',
                 'gke-pr.yml admit must classify the complete path file with the base rule'),
                ('result', 'if len(paths) >= 3000:', 'if False:',
                 'gke-pr.yml result must read the immutable compare list independently'),
                ('result', 'BASE_SHA: ${{ needs.admit.outputs.base_sha }}',
                 'BASE_SHA: ${{ github.event.pull_request.base.ref }}',
                 'gke-pr.yml result must read the immutable compare list independently'),
                ('build', GKE_PROOF_IF, "${{ needs.admit.outputs.docs_only != 'true' }}",
                 'gke-pr.yml build must follow admission and keep its path policy'),
                ('corpus-parity-subset', GKE_PROOF_IF, "${{ needs.admit.outputs.docs_only != 'true' }}",
                 'gke-pr.yml corpus-parity-subset must follow admission and keep its path policy'),
                ('preflight', '--tier "$TIER"', '--tier github-only',
                 'gke-pr.yml preflight must run the selected fast or github-only tier'),
                ('admit', 'permissions: {contents: read}', 'permissions: {contents: write}',
                 'gke-pr.yml job admit must keep its read-only permissions'),
                ('result', 'permissions: {contents: read}', 'permissions: {contents: write}',
                 'gke-pr.yml job result must keep its read-only permissions')):
            with self.subTest(job=job, mutation=after):
                changed = in_job(original, job, before, after)
                self.assertNotEqual(changed, original, 'the mutation must change the workflow input')
                self.assertIn(finding, ci_workflow_findings(
                    *self.ci_sources(), others={**self.other_sources(), 'gke-pr.yml': changed}))

    def test_build_aggregate_excuses_only_pr_deferrals_and_classified_docs_skips(self):
        pr_script = job_script(self.gke(), 'result',
                               'Require an independently consistent successful result')
        for class_name in ('docs', 'github', 'code'):
            base = {'ADMIT_RESULT': 'success', 'ALLOWED': 'true', 'PREFLIGHT_RESULT': 'success',
                    'LEGS_RESULT': 'success' if class_name == 'code' else 'skipped',
                    'ADMIT_DOCS_ONLY': 'true' if class_name == 'docs' else 'false',
                    'SECOND_DOCS_ONLY': 'true' if class_name == 'docs' else 'false',
                    'ADMIT_GITHUB_ONLY': 'true' if class_name == 'github' else 'false',
                    'SECOND_GITHUB_ONLY': 'true' if class_name == 'github' else 'false',
                    'CODE_EXTENSION': 'true' if class_name == 'code' else 'false'}
            for field in base:
                for outcome in dict.fromkeys((base[field], 'success', 'skipped', 'failure', 'cancelled', '')):
                    values = {**base, field: outcome}
                    expected = (values['ADMIT_RESULT'] == values['PREFLIGHT_RESULT'] == 'success'
                                and values['ALLOWED'] == 'true'
                                and values['ADMIT_DOCS_ONLY'] == values['SECOND_DOCS_ONLY']
                                and values['ADMIT_GITHUB_ONLY'] == values['SECOND_GITHUB_ONLY']
                                and (values['CODE_EXTENSION'] != 'true'
                                     or (values['ADMIT_DOCS_ONLY'] != 'true'
                                         and values['ADMIT_GITHUB_ONLY'] != 'true'))
                                and values['LEGS_RESULT'] == (
                                    'skipped' if 'true' in (
                                        values['ADMIT_DOCS_ONLY'], values['ADMIT_GITHUB_ONLY'])
                                    else 'success'))
                    result = subprocess.run(
                        ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', pr_script],
                        env={'PATH': os.environ.get('PATH', '/usr/bin:/bin'), **values},
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, expected, result.stdout + result.stderr)
        for event in ('push', 'schedule', 'workflow_dispatch'):
            script = job_script(self.ci_sources()[6], 'build-gate',
                                'Require every verification lane to have passed')
            required = {'AUTHZ_RESULT', 'CHANGES_RESULT', 'PREFLIGHT_RESULT', 'SLOW_PREFLIGHT_RESULT'}
            proof = {'BUILD_RESULT', 'PARITY_RESULT', 'ARM_BUILD_RESULT', 'SANITIZER_RESULT', 'NATIVE_RESULT',
                          'KERNEL_RESULT', 'FULL_PARITY_RESULT', 'DEBUG_BUILD_RESULT',
                          'RELEASE_GATE_RESULT', 'DEBUG_GATE_RESULT',
                          'SANITIZER_GATE_RESULT', 'KERNEL_GATE_RESULT'}
            for docs_only in ('false', 'true'):
                base = {name: 'success' for name in required | proof}
                if docs_only == 'true':
                    base.update({name: 'skipped' for name in proof})
                for name in sorted(base):
                    for outcome in ('success', 'skipped', 'failure', 'cancelled', ''):
                        results = {**base, name: outcome}
                        expected = all(results[key] == 'success' for key in required)
                        expected = expected and all(
                            results[key] == 'success'
                            or (results[key] == 'skipped'
                                and docs_only == 'true')
                            for key in proof)
                        with self.subTest(event=event, docs_only=docs_only, lane=name, result=outcome):
                            result = subprocess.run(
                                ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
                                env={'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                                     'GITHUB_EVENT_NAME': event, 'DOCS_ONLY': docs_only, **results},
                                capture_output=True, text=True)
                            self.assertEqual(result.returncode == 0, expected,
                                             result.stdout + result.stderr)
                            self.assertIn('preflight (slow): ', result.stdout)
                            self.assertIn('corpus parity (full): ', result.stdout)

    def test_build_gate_refuses_widened_deferrals_and_lost_heavy_dependencies(self):
        mutations = [
            ('"$docs_only" == "true" && "$1" == "skipped"', '"$1" == "skipped"'),
            ('passed "$DEBUG_BUILD_RESULT"', 'true'),
            ('passed "$RELEASE_GATE_RESULT"', 'true'),
            ('passed "$DEBUG_GATE_RESULT"', 'true'),
            ('passed "$SANITIZER_GATE_RESULT"', 'true'),
            ('passed "$KERNEL_GATE_RESULT"', 'true'),
            ('passed "$ARM_BUILD_RESULT"', 'true'),
            ('passed "$SANITIZER_RESULT"', 'true'),
            ('&& [[ "$SLOW_PREFLIGHT_RESULT" == "success" ]] && passed "$FULL_PARITY_RESULT"',
             '&& true && passed "$FULL_PARITY_RESULT"'),
            ('passed "$FULL_PARITY_RESULT"', 'true'),
            ('PARITY_RESULT: ${{ needs.corpus-parity-subset.result }}',
             "PARITY_RESULT: ${{ needs.corpus-parity-subset.result || 'success' }}"),
            ('SLOW_PREFLIGHT_RESULT: ${{ needs.preflight-slow.result }}',
             "SLOW_PREFLIGHT_RESULT: ${{ needs.preflight-slow.result || 'success' }}"),
            ('ARM_BUILD_RESULT: ${{ needs.build-arm.result }}',
             "ARM_BUILD_RESULT: ${{ needs.build-arm.result || 'success' }}"),
        ]
        for before, after in mutations:
            with self.subTest(replacement=after):
                findings = self.mutated_file_findings(6, 'build-gate', before, after)
                self.assertIn(
                    'build-gate must keep the exact result policy: success or a classified docs-only skip',
                    findings)
        ci = self.ci_sources()[6]
        needs, = ci_preflight._job_values(_jobs(ci)['build-gate'], 'needs')
        for job in ('build-arm', 'preflight-slow', 'corpus-parity', 'native-live', 'sanitizers',
                    'kernel-only', 'build-debug', 'build-release-gate', 'build-debug-gate',
                    'sanitizers-gate', 'kernel-only-gate'):
            replacement = ', '.join(item for item in ci_preflight._needs(_jobs(ci)['build-gate'])
                                    if item != job)
            findings = self.mutated_file_findings(6, 'build-gate', '    needs: ' + needs,
                                                  '    needs: [' + replacement + ']')
            self.assertIn('build-gate must aggregate authz, every proof job, preflight and changes', findings)

    def test_the_classification_is_the_base_copy_of_the_rule(self):
        """Run the changes job's own shell in throwaway repositories: the
        parent's scripts/ci_docs_only.py judges the change, so a change that
        breaks the rule cannot skip its own proof jobs, and a parent without
        the script runs every job."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        step = root / 'classify.sh'
        step.write_text(job_script(self.ci_sources()[6], 'changes', 'Classify the change'))
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
        self.assertEqual(answer, 'docs_only=true\ngithub_only=false\n')
        # A change that breaks the rule is judged by the rule it breaks.
        answer, out = classify('rule', {'scripts/ci_docs_only.py': rule, 'docs/ci.md': 'old\n'},
                               {'scripts/ci_docs_only.py': broken, 'src/engine.cpp': 'int x;\n', **page})
        self.assertEqual(answer, 'docs_only=false\ngithub_only=false\n', out)
        self.assertIn('scripts/ci_docs_only.py', out)
        # A base without the rule runs every job.
        answer, out = classify('bare', {'docs/ci.md': 'old\n'}, {'scripts/ci_docs_only.py': rule, **page})
        self.assertEqual(answer, 'docs_only=false\n', out)
        self.assertIn('the base holds no scripts/ci_docs_only.py', out)

    def test_harmless_spellings_are_not_findings(self):
        self.assertEqual(ci_preflight.runner_findings(self.light_workflows()), [])
        original, others = self.ci_sources(), self.other_sources()

        def quoted_key_findings(sources):
            # These two existing spellings remain harmless to the runner and
            # sizing policies, but the YAML whitelist now explicitly refuses
            # quoted keys. Keep both inputs and require exactly that refusal.
            findings = []
            # Match ci_workflow_findings' documented YAML/action-pin order:
            # ci-post.yml precedes docs.yml.
            for index in (6, 5):
                for number, line in enumerate(sources[index].splitlines(), 1):
                    if re.match(r' *(?:"PF_CI_LABELS"|"runs-on"):', line):
                        findings.append(ActionPins.unsupported_finding(
                            CI_SOURCES[index], number, line.strip(), 'quoted keys'))
            return findings

        quoted_edits = (
            (5, 'build', '    runs-on: ubuntu-24.04\n',
             '    "runs-on": ubuntu-24.04\n'),
            (6, 'preflight', '      PF_CI_LABELS:', '      "PF_CI_LABELS":'))
        verify = KERNEL_VERIFY
        edits = (
                (6, 'kernel-only', '    steps:\n',
                 '    # Was --jobs 4, and JOBS: "4", on the standard runner.\n    steps:\n'),
                (6, 'build', '      matrix:\n', '      matrix:\n\n        # standard images only\n'),
                (6, 'build', '      matrix:\n',
                 '      matrix:\n        # Was larger_runner: macos-26-xlarge\n'),
                (6, 'build', '          python3 scripts/ci_verify.py',
                 '          # Was --jobs 4 on the standard runner.\n          python3 scripts/ci_verify.py'),
                (6, 'kernel-only', verify,
                 'run: |\n          python3 scripts/ci_verify.py kernel --build-dir build-kernel \\\n'
                 f'            --jobs {CORES} --ccache '
                 '--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" '
                 '--ctest-shard ${{ matrix.shard }}/2'),
                (6, 'kernel-only', verify,
                 verify.replace('run: ', 'run: >-\n          ', 1).replace(
                     '--ctest-shard ', '--ctest-shard \n            ', 1)),
                (4, 'corpus-parity-subset', f'run: JOBS={CORES} ./scripts/check_corpus_parity.sh --subset',
                 f'run: |\n          JOBS={CORES} ./scripts/check_corpus_parity.sh --subset'),
                *quoted_edits,
                (6, 'build', '        platform:\n',
                 '        platform:  # the images\n'),
                (6, 'build', '    strategy:\n', '    strategy:  # the legs\n'),
                (6, 'sanitizers', '      - name: Verify (sanitizers)\n',
                 "      # hashFiles('scripts/ci_verify.py') is not a compiler-cache key.\n"
                 '      - name: Verify (sanitizers)\n'),
                (6, 'kernel-only', '      - name: Verify (kernel)\n',
                 '      - name: Verify (kernel) with scripts/ci_verify.py\n'),
                (6, 'sanitizers', f'run: python3 scripts/ci_verify.py sanitizers --build-dir build-asan --jobs {CORES}',
                 'run: >-\n          python3 scripts/ci_verify.py sanitizers --build-dir build-asan\n'
                 f'          --jobs {CORES}'),
                (1, 'native-live', f'--jobs {CORES}', f'--jobs={CORES}'),
                (1, 'native-live-shards', f'--jobs {CORES}', f'--jobs={CORES}'),
                (6, 'kernel-only', '      - name: Verify (kernel)\n',
                 '      - name: Verify (kernel), no longer --jobs 4\n'),
                (6, 'kernel-only', KERNEL_VERIFY,
                 # A wrapper action spelled the way the ORG-ACTIONPINS lint accepts: a (synthetic) 40-hex sha
                 # and a version comment, so only the harmless spelling of the command is under test here.
                 'uses: nick-fields/retry@0123456789abcdef0123456789abcdef01234567 # v3.2.0\n        with:\n'
                 '          command: python3 scripts/ci_verify.py '
                 f'kernel --build-dir build-kernel --jobs {CORES} --ccache '
                 '--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" '
                 '--ctest-shard ${{ matrix.shard }}/2'),
                (2, 'promote', f'    runs-on: {RELEASE_RUNNER}\n',
                 f'    runs-on: {RELEASE_RUNNER}  # isolated release\n'),
                (6, None, '\n  preflight:\n', '\n  preflight:  # the fast gate\n'))
        # Each edit alone, then every one that composes (a line one edit
        # rewrites is left to the edit that came first).
        for index, job, before, after in edits:
            with self.subTest(file=CI_SOURCES[index], job=job, after=after):
                changed = original.copy()
                changed[index] = in_job(changed[index], job, before, after)
                self.assertEqual(ci_workflow_findings(*changed, others=others),
                                 quoted_key_findings(changed))
        # Isolate both quoted keys: diagnostic order follows the checker,
        # regardless of which file's edit is applied first.
        for order in (quoted_edits, quoted_edits[::-1]):
            with self.subTest(quoted_order=tuple(CI_SOURCES[index] for index, *_ in order)):
                changed = original.copy()
                for index, job, before, after in order:
                    changed[index] = in_job(changed[index], job, before, after)
                    self.assertNotEqual(changed[index], original[index])
                self.assertEqual(ci_workflow_findings(*changed, others=others),
                                 quoted_key_findings(changed))
        changed = original.copy()
        for index, job, before, after in edits:
            if before in (changed[index] if job is None else _jobs(changed[index]).get(job, '')):
                changed[index] = in_job(changed[index], job, before, after)
        self.assertEqual(ci_workflow_findings(*changed, others=others),
                         quoted_key_findings(changed))

    def test_a_finding_is_reported_once(self):
        changed = self.ci_sources()
        changed[6] = changed[6].replace('\n  kernel-only:\n', '\n  kernel-only-renamed:\n', 1)
        findings = ci_workflow_findings(*changed, others=self.other_sources())
        self.assertEqual(findings.count('ci-post.yml is missing job kernel-only'), 1, findings)

    def test_paid_labels_and_larger_runner_keys_are_refused_even_in_release(self):
        original, others = self.ci_sources(), self.other_sources()
        spot = f'    runs-on: {POST_RUNNER}\n'
        lint = ('name: lint\non: workflow_dispatch\njobs:\n  lint:\n' + spot
                + f"    env:\n      PF_CI_LABELS: '{POST_RUNNER}'\n"
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
        # or changing anything else in the pinned two-leg Linux matrix.
        for before, after in ((f'runner: {RELEASE_RUNNER}\n', 'runner: ubuntu-22.04\n'),
                              (f'runner: {RELEASE_RUNNER}\n', f'runner: {MATRIX_LINUX_RUNNER}\n'),
                              (f'runner: {RELEASE_RUNNER}\n',
                               f"runner: ${{{{ fromJSON('{RELEASE_RUNNER}') || 'ubuntu-22.04' }}}}\n"),
                              ('runner: ubuntu-22.04-arm', 'runner: ubuntu-24.04'),
                              ('runner: ubuntu-22.04-arm', f'runner: {RELEASE_RUNNER}'),
                              ('runner: ubuntu-22.04-arm', 'runner: macos-latest'),
                              ('runner: ubuntu-22.04-arm', 'runner: macos-26-xlarge'),
                              ('        include:\n', '        include:\n'
                               '          - triple: macos-universal\n'
                               '            runner: macos-latest\n'
                               '            cmake_extra: "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"\n'),
                              ('          - triple: linux-aarch64\n'
                               '            runner: ubuntu-22.04-arm\n', '')):
            with self.subTest(release_runner=after):
                changed = in_job(release, 'prebuilt', before, after)
                findings = ci_workflow_findings(*original, others=dict(others, **{'release.yml': changed}))
                self.assertIn(RELEASE_MATRIX_FINDING, findings)

    def test_baseline_promotion_requires_isolation_and_push_dispatch_only(self):
        original, others = self.ci_sources(), self.other_sources()
        promote = original[2]
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        for runner in ('ubuntu-latest', 'ubuntu-24.04', PR_RUNNER,
                       '["self-hosted","pf-ci","pf-ci-release"]',
                       f"${{{{ fromJSON('{RELEASE_RUNNER}') || 'ubuntu-latest' }}}}"):
            with self.subTest(runner=runner):
                changed = original.copy()
                changed[2] = in_job(promote, 'promote', f'    runs-on: {RELEASE_RUNNER}\n',
                                    f'    runs-on: {runner}\n')
                findings = ci_workflow_findings(*changed, others=others)
                self.assertIn(f'promote-baseline.yml job promote must run on {RELEASE_RUNNER} '
                              '(isolated release)', findings)
        event_finding = ('promote-baseline.yml must retain push and '
                         'workflow_dispatch only for pf-gcp-main promotion')
        for event in ('pull_request', 'pull_request_target', 'workflow_run'):
            for key in (event, f'"{event}"', f"'{event}'", event + ' '):
                with self.subTest(event_key=key):
                    changed = original.copy()
                    changed[2] = in_job(promote, None, '\non:\n', f'\non:\n  {key}:\n')
                    findings = ci_workflow_findings(*changed, others=others)
                    self.assertIn(event_finding, findings)
        for before, after in (('  push:\n    branches: [main]\n', ''),
                              ('  workflow_dispatch:\n', '  workflow_call:\n'),
                              ('\non:\n', '\non: >-\n'),
                              ('\non:\n', '\non:\non:\n')):
            with self.subTest(trigger_replacement=after):
                changed = original.copy()
                changed[2] = in_job(promote, None, before, after)
                findings = ci_workflow_findings(*changed, others=others)
                self.assertIn(event_finding, findings)

    def test_baseline_promotion_requires_exact_main_push_filter_and_queue(self):
        branch_finding = 'promote-baseline.yml must restrict push to exactly branches: [main]'
        for replacement in ('', '    branches: [develop]\n',
                            '    branches: [main, develop]\n', '    branches: ["*"]\n',
                            '    branches-ignore: [develop]\n',
                            '    branches: [main]\n    tags: ["*"]\n',
                            '    branches: [main]\n    paths: ["scripts/**"]\n'):
            with self.subTest(push_filter=replacement):
                findings = self.mutated_file_findings(
                    2, None, '    branches: [main]\n', replacement)
                self.assertIn(branch_finding, findings)
        concurrency = ('concurrency:\n  group: promote-baseline\n'
                       '  cancel-in-progress: false\n')
        for before, after in (
                (concurrency, ''),
                ('  group: promote-baseline\n', '  group: promote-${{ github.sha }}\n'),
                ('  cancel-in-progress: false\n', '  cancel-in-progress: true\n'),
                ('  cancel-in-progress: false\n', ''),
                ('    steps:\n', '    concurrency: other-group\n    steps:\n')):
            with self.subTest(concurrency=after):
                job = 'promote' if before == '    steps:\n' else None
                findings = self.mutated_file_findings(2, job, before, after)
                self.assertIn('baseline promotion must queue in group promote-baseline '
                              'without cancellation', findings)

    def test_baseline_promotion_keeps_resolution_and_guards_before_credentials(self):
        for before, after in (
                ('SHA: ${{ github.sha }}', 'SHA: ${{ github.event.before }}'),
                ('gh api --paginate --slurp', 'gh api'),
                ('/commits/$SHA/pulls?per_page=100', '/commits/main/pulls?per_page=100'),
                ('        id: pr\n', '        id: unresolved\n'),
                ('        id: pr\n', "        id: pr\n        if: github.event_name == 'workflow_dispatch'\n"),
                ('REF: ${{ github.ref }}', "REF: refs/heads/main")):
            with self.subTest(resolver=after):
                findings = self.mutated_file_findings(2, 'promote-guard', before, after)
                self.assertIn('baseline promotion must resolve the pushed SHA or dispatch '
                              'inputs without a PR payload', findings)
        for key, value in (('MERGE_SHA', 'merge_commit'), ('HEAD_SHA', 'head_sha'),
                           ('PR_NUMBER', 'pr_number'), ('PR_URL', 'pr_url'),
                           ('MERGED_AT', 'merged_at')):
            with self.subTest(dispatch_input=key):
                findings = self.mutated_file_findings(2, 'promote-guard',
                    f'          INPUT_{key}: ${{{{ github.event.inputs.{value} }}}}\n', '')
                self.assertIn('baseline promotion must resolve the pushed SHA or dispatch '
                              'inputs without a PR payload', findings)
        for before, after in (
                ('        id: exacttree\n',
                 "        id: exacttree\n        if: github.event_name == 'workflow_dispatch'\n"),
                ("        if: steps.exacttree.outputs.ok == 'true'\n",
                 "        if: steps.pr.outputs.ok == 'true'\n"),
                ("        if: steps.ci.outputs.green == 'true'\n", ''),
                ('    steps:\n', "    if: github.event_name == 'workflow_dispatch'\n    steps:\n"),
                ('      - name: Verify the merged tree is the gated tree\n',
                 '      - run: echo unchecked-step\n'
                 '      - name: Verify the merged tree is the gated tree\n')):
            with self.subTest(guard=after):
                findings = self.mutated_file_findings(2, 'promote', before, after)
                self.assertIn('baseline promotion must guard resolution, tree and gate '
                              'checks before credentials', findings)
        for before, after in (
                ("        if: steps.pr.outputs.ok == 'true'\n", ''),
                ('        id: authorize\n', '        id: unchecked\n'),
                ('      - name: Only our own merged pull requests and dispatches promote the baseline\n',
                 '      - run: echo unchecked-step\n'
                 '      - name: Only our own merged pull requests and dispatches promote the baseline\n')):
            with self.subTest(authorization_order=after):
                findings = self.mutated_file_findings(2, 'promote-guard', before, after)
                self.assertIn('baseline promotion must guard resolution, tree and gate '
                              'checks before credentials', findings)
        findings = self.mutated_file_findings(2, 'promote',
            'PR_URL: ${{ needs.promote-guard.outputs.pr_url }}',
            'PR_URL: ${{ github.event.pull_request.html_url }}')
        self.assertIn('baseline promotion must resolve the pushed SHA or dispatch '
                      'inputs without a PR payload', findings)
        findings = self.mutated_file_findings(2, 'promote',
            '"+refs/pull/$PR_NUMBER/head:refs/remotes/origin/pr-head"',
            '"$HEAD_SHA:refs/remotes/origin/pr-head"')
        self.assertIn('baseline promotion must admit a squash merge by tree equality only', findings)

    def test_baseline_promotion_pins_python_selection_and_unique_match(self):
        for before, after in (
                ('python3 - 3<<<"$pulls" <<\'PY\'', "python3 - <<'PY'"),
                ('sha = os.environ["SHA"]', 'sha = "a" * 40'),
                ('pages = json.loads(open(3, encoding="utf-8").read(), parse_constant=reject_constant)',
                 'pages = [[]]'),
                ('matches = [pr for page in pages for pr in page',
                 'matches = [pr for page in pages[:1] for pr in page'),
                ('if pr.get("state") == "closed"', 'if True'),
                ('and pr.get("merged_at") is not None', 'and True'),
                ('and pr.get("merge_commit_sha") == sha', 'and True'),
                ('and isinstance(pr.get("base"), dict)', 'and True'),
                ('and pr["base"].get("ref") == "main"]', 'and True]'),
                ('if len(matches) != 1:', 'if not matches:')):
            with self.subTest(selection=after):
                findings = self.mutated_file_findings(2, 'promote-guard', before, after)
                self.assertIn('baseline promotion must resolve the pushed SHA or dispatch '
                              'inputs without a PR payload', findings)

    def test_new_workflows_require_event_fleets_without_conditional_routes(self):
        original, others = self.ci_sources(), self.other_sources()
        new = 'name: lint\non: {on}\njobs:\n  lint:\n    runs-on: {runner}\n    steps:\n      - run: "true"\n'
        for on, runner, refused in (
                ('[pull_request]', 'pf-linux-x64-16', True),
                ('\n  pull_request:\n', '\n      group: pineforge-ci-large', True),
                ('[push,\n  pull_request]', 'pf-linux-x64-16', True),
                ('>-\n  pull_request', 'macos-26-xlarge', True),
                ('[pull_request]', 'ubuntu-latest', True),
                ('[pull_request]', 'macos-26', True),
                ('pull_request_target', 'macos-26', True),
                ('pull_request_target', 'ubuntu-22.04-arm', True),
                ('[pull_request]', 'ubuntu-22.04-arm', True),
                ('[pull_request]', 'ubuntu-24.04-arm', True),
                # No dispatch-only exception, whether or not a fork can start it.
                ('\n  workflow_dispatch:\n', 'pf-linux-x64-16', True)):
            with self.subTest(on=on, runner=runner):
                findings = ci_workflow_findings(
                    *original, others=dict(others, **{'lint.yml': new.format(on=on, runner=runner)}))
                expected = POST_RUNNER if 'workflow_dispatch' in on else PR_RUNNER
                self.assertEqual(any(line.startswith(f'lint.yml job lint must run on {expected}')
                                     for line in findings), refused, findings)
        # A decoy on key inside a multi-line string is no exemption.
        decoy = ('name: "lint\non: push"\non:\n  pull_request:\njobs:\n  lint:\n'
                 '    runs-on: pf-linux-x64-16\n    steps:\n      - run: "true"\n')
        self.assertIn(f'lint.yml job lint must run on {PR_RUNNER}',
                      ci_workflow_findings(*original, others=dict(others, **{'lint.yml': decoy})))
        spot = new.format(on='[pull_request]', runner=PR_RUNNER)
        spot = spot.replace('    steps:\n', f"    env:\n      PF_CI_LABELS: '{PR_RUNNER}'\n    steps:\n")
        self.assertEqual(ci_workflow_findings(*original, others=dict(others, **{'lint.yml': spot})), [])
        mixed = spot.replace('on: [pull_request]', 'on: [pull_request, push, workflow_dispatch]', 1)
        self.assertIn('lint.yml must split pull_request and post-merge events into separate workflows',
                      ci_workflow_findings(*original, others=dict(others, **{'lint.yml': mixed})))
        for event in ('push', 'schedule', 'workflow_dispatch'):
            post = new.format(on='[' + event + ']', runner=POST_RUNNER)
            self.assertEqual(ci_workflow_findings(*original, others=dict(others, **{'lint.yml': post})), [])
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
                findings = ci_workflow_findings(*original, others=dict(others, **{'deploy.yml': hosted}))
                self.assertIn(f'deploy.yml job deploy must run on {POST_RUNNER} for post-merge events', findings)
                self.assertNotIn('deploy.yml job deploy must keep production credentials on a hosted standard runner',
                                 findings)
            for runner in (PR_RUNNER, POST_RUNNER, RELEASE_RUNNER):
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
                        f'    runs-on: {POST_RUNNER}\n'
                        '    # secrets.DEPLOY_KEY, id-token: write, environment: production\n'
                        '    env:\n'
                        f"      PF_CI_LABELS: '{POST_RUNNER}'\n"
                        f'      TOKEN: ${{{{ {reference} }}}}\n'
                        '    steps:\n      - run: "true"\n')
            with self.subTest(reference=reference):
                self.assertEqual(ci_workflow_findings(
                    *original, others=dict(others, **{'token.yml': workflow})), [])

    def test_today_release_and_deployment_jobs_pass_only_on_their_pinned_routes(self):
        self.assertEqual(ci_preflight.runner_findings(self.light_workflows()), [])
        original, others = self.ci_sources(), self.other_sources()
        workflows = {'docs.yml': original[5], 'promote-baseline.yml': original[2],
                     'release.yml': others['release.yml']}
        self.assertEqual(ci_workflow_findings(*original, others=others), [])
        self.assertNotIn('PF_CI_LABELS', workflows['release.yml'])
        for name, jobs in (('docs.yml', ('build',)), ('promote-baseline.yml', ('promote-guard', 'promote')),
                           ('release.yml', ('release', 'prebuilt', 'notify-hub'))):
            for job in jobs:
                before = (HOSTED_MATRIX_RUNNER if job == 'prebuilt' else
                          GKE_2 if job == 'promote-guard' else
                          'ubuntu-24.04' if name == 'docs.yml' else RELEASE_RUNNER)
                after = 'ubuntu-latest' if name == 'docs.yml' else PR_RUNNER
                pool = in_job(workflows[name], job, f'    runs-on: {before}\n',
                              f'    runs-on: {after}\n')
                findings = ci_preflight.runner_findings(dict(
                    {'ci.yml': original[0], **workflows}, **{name: pool}))
                finding = (f'{name} job {job} must run on ubuntu-24.04 (hosted)' if name == 'docs.yml' else
                           f'{name} job {job} must run on {GKE_2}' if job == 'promote-guard' else
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
                route = ('a hosted job' if name == 'docs.yml' else
                         'a GKE job' if job == 'promote-guard' else 'an isolated release job')
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

    def test_no_stage_runs_under_a_timeout_and_jobs_keep_the_platform_maximum(self):
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
                     for name in JOB_COMMANDS}
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        for workflow, jobs in JOB_COMMANDS.items():
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
                        timeout = f'{workflow} job {job} must declare no timeout-minutes (owner #827)'
                        self.assertEqual(ci_preflight.runner_findings(changed), [timeout])

    def contract_stage(self, mutation, workflow_name='ci.yml'):
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
            ci = tree / '.github/workflows' / workflow_name
            ci.write_text(in_job(ci.read_text(), *mutation))
        result = subprocess.run(
            [sys.executable, str(tree / 'scripts/ci_preflight.py'), '--check-ci-workflow'],
            cwd=tree, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def test_the_preflight_stage_fails_bad_spot_labels_or_paid_runners(self):
        code, output = self.contract_stage(None)
        self.assertEqual(code, 0, output)
        self.assertIn('runners and time limits OK', output)
        for name, mutation, finding in (
                ('gke-pr.yml', ('preflight', runner_declaration(GKE_PR_8), runner_declaration('ubuntu-latest')),
                 f'gke-pr.yml job preflight must run on {GKE_PR_8}'),
                ('gke-pr.yml', ('preflight', '    steps:\n',
                  '    environment: production\n    steps:\n'),
                 'gke-pr.yml job preflight must keep production credentials on a hosted standard runner'),
                ('gke-pr.yml', ('preflight', runner_declaration(GKE_PR_8),
                  runner_declaration("${{ fromJSON('" + PR_RUNNER + "') || 'ubuntu-24.04' }}")),
                 f'gke-pr.yml job preflight must run on {GKE_PR_8}'),
                ('gke-pr.yml', ('preflight', runner_declaration(GKE_PR_8), runner_declaration(RELEASE_RUNNER)),
                 f'gke-pr.yml job preflight must run on {GKE_PR_8}'),
                ('gke-pr.yml', ('preflight', runner_declaration(GKE_PR_8),
                                runner_declaration('["self-hosted","pf-gcp-pr","pf-gcp-main"]')),
                 f'gke-pr.yml job preflight must run on {GKE_PR_8}'),
                ('ci-post.yml', ('sanitizers', runner_declaration(GKE_32),
                                runner_declaration('["self-hosted","pf-gcp-small"]')),
                 f'ci-post.yml job sanitizers must run on {GKE_32}'),
                ('ci-post.yml', ('sanitizers', runner_declaration(GKE_32),
                                runner_declaration('pf-linux-x64-16')),
                 f'ci-post.yml job sanitizers must run on {GKE_32}'),
                ('ci-post.yml', ('build-arm', runner_declaration(GKE_ARM),
                                '    runs-on: macos-26-xlarge\n'),
                 'ci-post.yml must not use larger-runner labels; use the GCP fleet or hosted exceptions'),
                ('ci-post.yml', ('build', '        build_type: [Release]\n',
                  '        build_type: [Release]\n        larger_runner: ubuntu-24.04\n'),
                 'ci-post.yml must not define a larger_runner key; use the GCP fleet or hosted exceptions')):
            with self.subTest(mutation=mutation):
                code, output = self.contract_stage(mutation, workflow_name=name)
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

    def test_release_bump_contract_and_gate_tests_remain_binding(self):
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])
        for tier in ('all', 'fast'):
            plan = {entry[0]: entry for entry in check_commands(ROOT, tier=tier)}
            self.assertEqual(plan['release-gate-tests'], (
                'release-gate-tests', [sys.executable, str(ROOT / 'scripts/test_ci_release_gate.py')]))

    def test_release_bump_publication_mutations_refuse(self):
        original, others = self.ci_sources(), self.other_sources()
        release = others['release.yml']
        finding = ('release.yml must keep the exact two-file release bump writer, '
                   'commit and atomic push')
        for before, after in (
                ('git add VERSION docker/run_failure_codes_diff.json',
                 'git add VERSION docker/run_failure_codes_diff.json README.md'),
                ('git add VERSION docker/run_failure_codes_diff.json', 'git add -A'),
                ('cp "${RESET}" docker/run_failure_codes_diff.json',
                 'cp "${RESET}" docker/run_failure_codes.json'),
                ("printf '%s\\n' \"${NEXT}\" > VERSION",
                 "printf '%s\\n' \"${NEXT}\" > VERSION\n          chmod +x VERSION"),
                ('git commit -m "chore(release): ${TAG}"',
                 'git commit --amend -m "chore(release): ${TAG}"'),
                ('git push --atomic origin HEAD "${TAG}"', 'git push origin HEAD "${TAG}"')):
            with self.subTest(after=after):
                changed = in_step(release, 'release', 'Write bumped VERSION, commit, tag, push',
                                  before, after)
                self.assertNotEqual(changed, release)
                self.assertIn(finding, ci_workflow_findings(
                    *original, others={**others, 'release.yml': changed}))

    def test_release_bump_app_token_mutations_refuse(self):
        original, others = self.ci_sources(), self.other_sources()
        release = others['release.yml']
        for name, before, after in (
                ('Checkout (full history)', 'token: ${{ steps.app.outputs.token }}',
                 'token: ${{ github.token }}'),
                ('Mint org App token', '        id: app', '        id: another-app'),
                ('Mint org App token', 'app-id: ${{ secrets.PINEFORGE_APP_ID }}',
                 'app-id: ${{ secrets.OTHER_APP_ID }}')):
            with self.subTest(name=name, after=after):
                changed = in_step(release, 'release', name, before, after)
                self.assertNotEqual(changed, release)
                self.assertIn('release.yml bump push must use the release App installation token',
                              ci_workflow_findings(
                                  *original, others={**others, 'release.yml': changed}))

    def test_release_ruleset_reader_token_and_invocation_mutations_refuse(self):
        original, others = self.ci_sources(), self.other_sources()
        release = others['release.yml']
        gate_name = 'Refuse unless pineforge/verify and pineforge/parity are green'
        for before, after in (
                ('GH_TOKEN: ${{ steps.app.outputs.token }}', 'GH_TOKEN: ${{ github.token }}'),
                ('        id: gate', '        id: gate\n        if: inputs.dry_run'),
                ('python3 scripts/ci_release_gate.py gate', 'echo release gate green'),
                ('--ref "${GITHUB_REF}"', '--ref "${GITHUB_REF}" || true'),
                ('        run: |', '        continue-on-error: true\n        run: |')):
            with self.subTest(after=after):
                changed = in_step(release, 'release', gate_name, before, after)
                self.assertNotEqual(changed, release)
                self.assertIn('release.yml must keep the unconditional release gate '
                              'with its release App token '
                              'for the App-only main pull_request barrier',
                              ci_workflow_findings(
                                  *original, others={**others, 'release.yml': changed}))

    def test_release_ruleset_probe_workflow_is_installed_and_pinned(self):
        path = ROOT / '.github/workflows/release-ruleset-probe.yml'
        self.assertTrue(path.is_file(), 'the dispatch-only ruleset probe workflow is missing')
        self.assertEqual(ci_preflight._code(path.read_text()),
                         ci_preflight._code(RULESETS_PROBE_WORKFLOW))
        self.assertEqual(ci_preflight.RELEASE_JOB_RUNNERS.get(path.name),
                         {'probe': POST_RUNNER})
        self.assertEqual(ci_preflight.HOSTED_JOB_RUNNERS.get(path.name), {'authz': GKE_2})
        probe_app = ci_preflight._step_named(ci_preflight._steps(_jobs(path.read_text())['probe']),
                                             '      - name: Mint org App token')
        release_app = ci_preflight._step_named(
            ci_preflight._steps(_jobs(self.other_sources()['release.yml'])['release']),
            '      - name: Mint org App token')
        self.assertEqual(probe_app, release_app, 'the mint steps must stay byte-identical')
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=self.other_sources()), [])

    def test_release_ruleset_probe_post_fleet_is_accepted(self):
        workflows = {'release-ruleset-probe.yml': RULESETS_PROBE_WORKFLOW}
        self.assertEqual(ci_preflight.runner_findings(workflows), [])
        self.assertEqual(ci_preflight.release_rulesets_probe_workflow_findings(
            RULESETS_PROBE_WORKFLOW, self.other_sources()['release.yml']), [])

    def test_release_ruleset_probe_refuses_release_and_pr_fleets(self):
        original, others = self.ci_sources(), self.other_sources()
        finding = (f'release-ruleset-probe.yml job probe must run on {POST_RUNNER} '
                   '(post fleet)')
        for runner in (RELEASE_RUNNER, PR_RUNNER):
            with self.subTest(runner=runner):
                changed = in_job(RULESETS_PROBE_WORKFLOW, 'probe',
                                 f'    runs-on: {POST_RUNNER}\n',
                                 f'    runs-on: {runner}\n')
                self.assertNotEqual(changed, RULESETS_PROBE_WORKFLOW)
                self.assertIn(finding, ci_preflight.runner_findings(
                    {'release-ruleset-probe.yml': changed}))
                self.assertIn(finding, ci_workflow_findings(
                    *original, others={**others, 'release-ruleset-probe.yml': changed}))
                self.assertIn(RULESETS_PROBE_FINDING,
                              ci_preflight.release_rulesets_probe_workflow_findings(
                                  changed, others['release.yml']))

    def test_release_ruleset_probe_main_ref_guard_precedes_credentials(self):
        marker = '      - name: Refuse a dispatch ref other than main'
        for workflow in (
                (ROOT / '.github/workflows/release-ruleset-probe.yml').read_text(),
                ci_preflight.RELEASE_RULESETS_PROBE_WORKFLOW,
                RULESETS_PROBE_WORKFLOW):
            with self.subTest(workflow=workflow):
                steps = ci_preflight._steps(_jobs(workflow)['probe'])
                self.assertEqual(steps[0].splitlines()[0], marker)
                self.assertEqual(ci_preflight._step_values(steps[0], 'if'), [])
                self.assertEqual(ci_preflight._step_values(steps[0], 'continue-on-error'), [])
                self.assertEqual(ci_preflight._step_values(steps[0], 'env'),
                                 ['REF: ${{ github.ref }}'])
                self.assertNotIn('secrets.', steps[0])
                self.assertNotIn('token', steps[0])
                self.assertEqual(steps[1].splitlines()[0], '      - name: Mint org App token')
                self.assertEqual(steps[2].splitlines()[0], '      - name: Checkout')

    def test_release_ruleset_probe_topic_dispatch_stops_before_mint(self):
        workflow = (ROOT / '.github/workflows/release-ruleset-probe.yml').read_text()
        steps = ci_preflight._steps(_jobs(workflow)['probe'])
        mint = next(index for index, step in enumerate(steps)
                    if step.splitlines()[0] == '      - name: Mint org App token')
        prefix = []
        for step in steps[:mint]:
            self.assertEqual(ci_preflight._step_values(step, 'uses'), [])
            self.assertEqual(ci_preflight._step_values(step, 'if'), [])
            self.assertEqual(ci_preflight._step_values(step, 'continue-on-error'), [])
            self.assertEqual(ci_preflight._step_values(step, 'env'),
                             ['REF: ${{ github.ref }}'])
            name, = ci_preflight._step_values(step, 'name')
            prefix.append(job_script(workflow, 'probe', name))
        # Execute the real pre-mint shell; the echo marks reaching the action boundary.
        script = ''.join(prefix) + 'echo "mint step reached"\n'
        for ref, expected in (
                ('refs/heads/topic', 1), ('refs/tags/main', 1),
                ('refs/heads/main/topic', 1), ('refs/heads/Main', 1),
                ('', 1), ('refs/heads/main', 0)):
            with self.subTest(ref=ref), tempfile.TemporaryDirectory() as directory:
                summary = Path(directory) / 'summary'
                result = subprocess.run(
                    ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script],
                    env={'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                         'REF': ref, 'GITHUB_STEP_SUMMARY': str(summary)},
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertEqual(result.stderr, '')
                if expected:
                    self.assertEqual(result.stdout, '::error title=rulesets-probe::'
                                     'release ruleset probe requires refs/heads/main\n')
                    self.assertEqual(summary.read_text(), 'rulesets-probe: REFUSED: '
                                     'release ruleset probe requires refs/heads/main\n')
                    self.assertNotIn('mint step reached', result.stdout)
                else:
                    self.assertEqual(result.stdout, 'mint step reached\n')
                    self.assertFalse(summary.exists())

    def test_release_ruleset_probe_main_ref_guard_mutations_refuse(self):
        original, others = self.ci_sources(), self.other_sources()
        steps = ci_preflight._steps(_jobs(RULESETS_PROBE_WORKFLOW)['probe'])
        guard, mint, checkout = (step + '\n' for step in steps[:3])
        no_guard = RULESETS_PROBE_WORKFLOW.replace(guard, '', 1)
        comparison = ('          [ "$REF" = refs/heads/main ] || fail '
                      '"release ruleset probe requires refs/heads/main"')
        guard_name = '      - name: Refuse a dispatch ref other than main\n'
        for name, before, after in (
                ('missing main ref guard', guard, ''),
                ('main ref guard after mint', guard + mint, mint + guard),
                ('main ref guard after checkout', guard + mint + checkout, mint + checkout + guard),
                ('dispatch input ref', '          REF: ${{ github.ref }}',
                 '          REF: ${{ github.event.inputs.ref }}'),
                ('head ref', '          REF: ${{ github.ref }}',
                 '          REF: ${{ github.head_ref }}'),
                ('different ref comparison', comparison,
                 comparison.replace('[ "$REF" = refs/heads/main ]', '[ "$REF" = refs/heads/topic ]')),
                ('prefix ref comparison', comparison,
                 comparison.replace('[ "$REF" = refs/heads/main ]', '[[ "$REF" == refs/heads/main* ]]')),
                ('masked main ref refusal', comparison,
                 '          [ "$REF" = refs/heads/main ] || true'),
                ('neutralized main ref exit', guard, guard.replace('exit 1; }', 'return 0; }')),
                ('main ref continue-on-error', guard_name,
                 guard_name + '        continue-on-error: true\n'),
                ('main ref if false', guard_name, guard_name + '        if: false\n'),
                ('main ref if always', guard_name, guard_name + '        if: ${{ always() }}\n'),
                ('main ref guard only in authz', RULESETS_PROBE_WORKFLOW,
                 in_job(no_guard, 'authz', '    steps:\n', '    steps:\n' + guard))):
            with self.subTest(invariant=name):
                self.assertEqual(RULESETS_PROBE_WORKFLOW.count(before), 1, name)
                changed = RULESETS_PROBE_WORKFLOW.replace(before, after, 1)
                self.assertNotEqual(changed, RULESETS_PROBE_WORKFLOW)
                direct = ci_preflight.release_rulesets_probe_workflow_findings(
                    changed, others['release.yml'])
                combined = ci_workflow_findings(
                    *original, others={**others, 'release-ruleset-probe.yml': changed})
                for findings in (direct, combined):
                    self.assertIn(RULESETS_PROBE_REF_FINDING, findings)
                    self.assertIn(RULESETS_PROBE_FINDING, findings)

    def test_release_ruleset_probe_comments_describe_the_actual_pin_scope(self):
        source = (ROOT / 'scripts/ci_preflight.py').read_text()
        workflow = (ROOT / '.github/workflows/release-ruleset-probe.yml').read_text()
        header = (
            '    # Preflight pins executable workflow lines after removing comment-only lines.\n'
            '    # authz uses gke-2cpu-4gb-amd64; the App-token probe uses pf-gcp-post.\n')
        requirements = (
            (source,
             '# Scoped CI workflows bind this canon header byte for byte, including its\n'
             '# fleet comment and comma spacing. The release probe pins executable lines.\n'),
            (source,
             '                    # Normalize the runner array here; the probe workflow pin below\n'
             '                    # compares nonblank lines after removing comment-only lines.\n'),
            (source, header),
            (RULESETS_PROBE_WORKFLOW, header),
            (workflow, header),
        )
        message = 'comments must describe the probe pin without claiming its header is bound'
        for text, required in requirements:
            self.assertIn(required, text, message)
            changed = text.replace(required, '', 1)
            self.assertNotEqual(changed, text)
            with self.assertRaisesRegex(AssertionError, message):
                self.assertIn(required, changed, message)

        changed = workflow.replace(header, '    # A different header comment.\n', 1)
        self.assertNotEqual(changed, workflow)
        self.assertEqual(ci_workflow_findings(
            *self.ci_sources(), others={**self.other_sources(),
                                       'release-ruleset-probe.yml': changed}), [])

    def test_release_ruleset_probe_refuses_the_original_unauthorized_workflow(self):
        # The concrete main input has neither authz nor needs/step guards.
        # Main's whole-workflow pin accepts it, so this is red for that omission.
        workflow = RULESETS_PROBE_WORKFLOW
        authz = '  authz:' + _jobs(workflow)['authz']
        self.assertEqual(workflow.count(authz), 1)
        unguarded = workflow.replace(authz, '', 1)
        unguarded = unguarded.replace('    needs: [authz]\n', '', 1)
        guard = "        if: ${{ needs.authz.result == 'success' }}\n"
        self.assertEqual(unguarded.count(guard), 2)
        unguarded = unguarded.replace(guard, '')
        self.assertEqual(list(_jobs(unguarded)), ['probe'])
        self.assertEqual(ci_preflight._needs(_jobs(unguarded)['probe']), [])
        self.assertIn(RULESETS_PROBE_FINDING, ci_workflow_findings(
            *self.ci_sources(), others={**self.other_sources(),
                                       'release-ruleset-probe.yml': unguarded}))

    def test_release_ruleset_probe_contract_accepts_only_the_requested_workflow(self):
        others = {**self.other_sources(), 'release-ruleset-probe.yml': RULESETS_PROBE_WORKFLOW}
        self.assertEqual(ci_workflow_findings(*self.ci_sources(), others=others), [])

    def test_release_ruleset_probe_workflow_mutations_refuse(self):
        original, others = self.ci_sources(), self.other_sources()
        self.assertEqual(ci_preflight.release_rulesets_probe_workflow_findings(
            RULESETS_PROBE_WORKFLOW, others['release.yml']), [])
        # Each row changes an independent invariant of the authorized dispatch
        # fixture and must reach the real whole-workflow contract.
        authz = '  authz:' + _jobs(RULESETS_PROBE_WORKFLOW)['authz']
        for name, before, after in (
                ('missing authz', authz, ''),
                ('missing needs', '    needs: [authz]\n', ''),
                ('disabled authz', '    name: authz\n', '    name: authz\n    if: false\n'),
                ('authz refusal',
                 'fail() { echo "::error title=authz::$1"; echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }',
                 'fail() { echo "::error title=authz::$1"; echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; :; }'),
                ('authz permissions', '    permissions: {}', '    permissions: {contents: read}'),
                ('authz runner', '    runs-on: gke-2cpu-4gb-amd64',
                 '    runs-on: ["self-hosted", "pf-gcp-post"]'),
                ('checkout guard', '      - name: Checkout\n'
                 "        if: ${{ needs.authz.result == 'success' }}\n",
                 '      - name: Checkout\n'),
                ('probe guard', '      - name: Probe release ruleset barrier\n'
                 "        if: ${{ needs.authz.result == 'success' }}\n",
                 '      - name: Probe release ruleset barrier\n'),
                ('trigger', '  workflow_dispatch:', '  push:'),
                ('extra trigger', '  workflow_dispatch:', '  workflow_dispatch:\n  pull_request:'),
                ('inputs', '  workflow_dispatch:', '  workflow_dispatch:\n    inputs:\n      token:\n        type: string'),
                ('permissions', '  contents: read', '  contents: write'),
                ('extra scope', '  contents: read', '  contents: read\n  actions: read'),
                ('job permissions', '  probe:\n', '  probe:\n    permissions:\n      contents: write\n'),
                ('runner', POST_RUNNER, RELEASE_RUNNER),
                ('PR runner', POST_RUNNER, PR_RUNNER),
                ('app action', 'actions/create-github-app-token@d72941d797fd3113feb6b93fd0dec494b13a2547',
                 'actions/create-github-app-token@v2'),
                ('app output', '        id: app', '        id: other'),
                ('app id', 'secrets.PINEFORGE_APP_ID', 'secrets.OTHER_APP_ID'),
                ('private key', 'secrets.PINEFORGE_APP_PRIVATE_KEY', 'secrets.OTHER_PRIVATE_KEY'),
                ('owner', '${{ github.repository_owner }}', 'another-owner'),
                ('repository', 'repositories: pineforge-engine', 'repositories: other'),
                ('token permission set', '          repositories: pineforge-engine',
                 '          repositories: pineforge-engine\n          permission-administration: read'),
                ('token consumer', 'GH_TOKEN: ${{ steps.app.outputs.token }}',
                 'GH_TOKEN: ${{ github.token }}'),
                ('job token', '  probe:\n',
                 '  probe:\n    env:\n      GH_TOKEN: ${{ steps.app.outputs.token }}\n'),
                ('workflow token', 'jobs:', 'env:\n  GH_TOKEN: ${{ steps.app.outputs.token }}\n\njobs:'),
                ('checkout persistence', 'persist-credentials: false', 'persist-credentials: true'),
                ('checkout token', '          persist-credentials: false',
                 '          persist-credentials: false\n          token: ${{ steps.app.outputs.token }}'),
                ('another token consumer', '      - name: Checkout',
                 '      - run: echo "${GH_TOKEN}"\n'
                 '        env:\n          GH_TOKEN: ${{ steps.app.outputs.token }}\n'
                 '      - name: Checkout'),
                ('skip', '      - name: Probe release ruleset barrier\n'
                 "        if: ${{ needs.authz.result == 'success' }}",
                 '      - name: Probe release ruleset barrier\n        if: false'),
                ('ignore error', '        run: python3',
                 '        continue-on-error: true\n        run: python3'),
                ('subcommand', ' rulesets-probe --repo', ' gate --repo'),
                ('masked exit', '--repo "${GITHUB_REPOSITORY}"',
                 '--repo "${GITHUB_REPOSITORY}" || true'),
                ('extra command', '--repo "${GITHUB_REPOSITORY}"',
                 '--repo "${GITHUB_REPOSITORY}"; gh api repos/example/engine'),
                ('token logging', '        run: python3', '        run: echo "$GH_TOKEN"; python3')):
            with self.subTest(invariant=name):
                self.assertEqual(RULESETS_PROBE_WORKFLOW.count(before), 1, name)
                changed = RULESETS_PROBE_WORKFLOW.replace(before, after, 1)
                self.assertNotEqual(changed, RULESETS_PROBE_WORKFLOW)
                self.assertIn(RULESETS_PROBE_FINDING, ci_workflow_findings(
                    *original, others={**others, 'release-ruleset-probe.yml': changed}))

    def test_release_ruleset_probe_refuses_missing_workflow_or_different_release_token(self):
        original, others = self.ci_sources(), self.other_sources()
        missing = {name: body for name, body in others.items()
                   if name != 'release-ruleset-probe.yml'}
        self.assertIn(RULESETS_PROBE_FINDING, ci_workflow_findings(*original, others=missing))
        for before, after in (
                ('actions/create-github-app-token@d72941d797fd3113feb6b93fd0dec494b13a2547', 'actions/create-github-app-token@v2'),
                ('secrets.PINEFORGE_APP_ID', 'secrets.OTHER_APP_ID'),
                ('secrets.PINEFORGE_APP_PRIVATE_KEY', 'secrets.OTHER_PRIVATE_KEY'),
                ('          repositories: pineforge-engine',
                 '          repositories: pineforge-engine\n          permission-contents: read')):
            with self.subTest(release_token=after):
                changed = in_step(others['release.yml'], 'release', 'Mint org App token', before, after)
                self.assertIn(RULESETS_PROBE_FINDING, ci_workflow_findings(
                    *original, others={**others, 'release.yml': changed,
                                      'release-ruleset-probe.yml': RULESETS_PROBE_WORKFLOW}))

    def test_duplicate_test_name_stages_are_binding_and_pin_the_source_root(self):
        source = Path('/src')
        commands = check_commands(source)
        plan = {entry[0]: entry for entry in commands}
        tests = 'duplicate-test-names-tests'
        check = 'duplicate-test-names'
        self.assertEqual(plan[tests], (
            tests, [sys.executable, str(source / 'scripts/test_check_duplicate_test_names.py')],
        ))
        self.assertEqual(plan[check], (
            check, [sys.executable, str(source / 'scripts/check_duplicate_test_names.py'),
                    '--root', str(source)],
        ))
        names = [entry[0] for entry in commands]
        self.assertLess(names.index(tests), names.index(check))
        fast_names = {entry[0] for entry in check_commands(source, tier='fast')}
        self.assertTrue({tests, check} <= fast_names)

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


class BaselinePromotionResolution(unittest.TestCase):
    SHA = 'a' * 40
    HEAD = 'b' * 40
    IDENTITIES = {'head_repo': 'owner/engine', 'head_is_fork': 'false',
                  'association': 'MEMBER', 'pr_author': 'author', 'merged_by': 'merger'}

    def merged_pr(self, *, sha=None, head=None, number=231,
                  merged_at='2026-10-21T08:15:30Z'):
        return {'state': 'closed', 'merged': True,
                'merge_commit_sha': self.SHA if sha is None else sha,
                'head': {'sha': self.HEAD if head is None else head,
                         'repo': {'full_name': 'owner/engine', 'fork': False}}, 'number': number,
                'html_url': f'https://github.com/owner/engine/pull/{number}',
                'merged_at': merged_at, 'base': {'ref': 'main'},
                'author_association': 'MEMBER', 'user': {'login': 'author'},
                'merged_by': {'login': 'merger'}}

    def resolver_record(self, pages, *, sha=None, event='push', inputs=None, raw_json=None,
                        details=None, raw_details=None, detail_number=None, detail_exit=0, api_exit=0,
                        ref='refs/heads/main', workflow=None, expected_code=0,
                        authorize=False, permissions=None):
        if workflow is None:
            workflow = (ROOT / CI_SOURCES[2]).read_text()
        script = job_script(workflow, 'promote-guard', 'Resolve the merged PR')
        self.assertNotIn('${{', script)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gh = root / 'gh'
            # Only the API transport is fake; bash and python3 run the workflow's
            # actual selection and output formatting against the fixture JSON.
            # Responses are embedded in this executable; it follows no fixture
            # path supplied by a command or environment variable.
            details = self.merged_pr(sha=sha) if details is None else details
            pages_text = json.dumps(pages) if raw_json is None else raw_json
            details_text = json.dumps(details) if raw_details is None else raw_details
            if detail_number is None:
                detail_number = details.get('number', 231) if isinstance(details, dict) else 231
            permissions = {'author': ('write', 0), 'merger': ('write', 0)} if permissions is None else permissions
            gh.write_text('#!' + sys.executable + ' -I\n'
                'import os, pathlib, sys\n'
                f'pages = {pages_text!r}\ndetails = {details_text!r}\n'
                f'api_exit = {api_exit!r}\ndetail_exit = {detail_exit!r}\n'
                f'number = {detail_number!r}\npermissions = {permissions!r}\n'
                'args = sys.argv[1:]\n'
                'with pathlib.Path(__file__).with_name("lookup").open("a") as log:\n'
                '    log.write(" ".join(args) + "\\n")\n'
                'if args == ["api", "--paginate", "--slurp", '
                '"repos/owner/engine/commits/" + os.environ["SHA"] + "/pulls?per_page=100"]:\n'
                '    sys.stdout.write(pages)\n'
                '    raise SystemExit(api_exit)\n'
                'if args == ["api", "repos/owner/engine/pulls/" + str(number)]:\n'
                '    sys.stdout.write(details)\n'
                '    raise SystemExit(detail_exit)\n'
                'prefix = "repos/owner/engine/collaborators/"\n'
                'if (len(args) == 4 and args[0] == "api" and args[2:] == ["--jq", ".permission"]\n'
                '        and args[1].startswith(prefix) and args[1].endswith("/permission")):\n'
                '    user = args[1][len(prefix):-len("/permission")]\n'
                '    permission, code = permissions.get(user, ("none", 0))\n'
                '    if permission: print(permission)\n'
                '    raise SystemExit(code)\n'
                'raise SystemExit(2)\n')
            gh.chmod(0o755)
            output = root / 'outputs'
            log = root / 'lookup'
            summary = root / 'summary'
            env = {**os.environ, 'EVENT_NAME': event, 'SHA': self.SHA if sha is None else sha,
                   'GH_TOKEN': 'fixture-token', 'REPO': 'owner/engine', 'REF': ref,
                   'INPUT_MERGE_SHA': '', 'INPUT_HEAD_SHA': '', 'INPUT_PR_NUMBER': '',
                   'INPUT_PR_URL': '', 'INPUT_MERGED_AT': '', **(inputs or {}),
                   'GITHUB_STEP_SUMMARY': str(summary), 'GITHUB_OUTPUT': str(output),
                   'PATH': str(root) + os.pathsep + os.environ.get('PATH', '')}
            ran = subprocess.run(['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', script], env=env,
                                 text=True, capture_output=True)
            self.assertEqual(ran.returncode, expected_code, ran.stdout + ran.stderr)
            if expected_code:
                self.assertNotIn('authz: allowed', summary.read_text() if summary.exists() else '')
            lines = output.read_text().splitlines() if output.exists() else []
            values = dict(line.split('=', 1) for line in lines)
            self.assertEqual(len(values), len(lines), 'duplicate step outputs')
            authorization = None
            allowed = root / 'authorized'
            if authorize and ran.returncode == 0 and values.get('ok') == 'true':
                step = ci_preflight._step_named(ci_preflight._steps(_jobs(workflow)['promote-guard']),
                                                '      - name: ' + PROMOTION_AUTHORIZATION_STEP)
                self.assertEqual(ci_preflight._step_values(step, 'if'), ["steps.pr.outputs.ok == 'true'"])
                bindings = ci_preflight._step_block(step, 'env')
                self.assertTrue(bindings)
                context = {'github.event_name': event, 'github.repository': 'owner/engine',
                           'github.token': 'fixture-token'}
                auth_env = dict(env, GITHUB_OUTPUT=str(allowed))
                for binding in bindings[1:]:
                    key, value = binding.strip().split(': ', 1)
                    expression = re.fullmatch(r'\$\{\{ (.+) \}\}', value)
                    if expression:
                        source = expression.group(1)
                        if source.startswith('steps.pr.outputs.'):
                            auth_env[key] = values.get(source.removeprefix('steps.pr.outputs.'), '')
                        else:
                            self.assertIn(source, context, 'unexpected workflow binding')
                            auth_env[key] = context[source]
                    else:
                        auth_env[key] = json.loads(value)
                authorization = subprocess.run(
                    ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c',
                     job_script(workflow, 'promote-guard', PROMOTION_AUTHORIZATION_STEP)],
                    env=auth_env, text=True, capture_output=True)
            return {'resolver': ran, 'values': values,
                    'authorization': authorization,
                    'authorized': allowed.read_text() if allowed.exists() else '',
                    'summary': summary.read_text() if summary.exists() else '',
                    'lookup': log.read_text() if log.exists() else ''}

    def run_resolver(self, pages, **kwargs):
        record = self.resolver_record(pages, **kwargs)
        return record['values'], record['resolver'].stdout, bool(record['lookup'])

    def test_push_returns_exact_five_values_for_one_merged_pr(self):
        for sha, head, number, merged_at in (
                (self.SHA, self.HEAD, 231, '2026-10-21T08:15:30Z'),
                ('c' * 40, 'd' * 40, 466, '2026-10-22T03:04:05Z')):
            with self.subTest(sha=sha, pr=number):
                pr = self.merged_pr(sha=sha, head=head, number=number, merged_at=merged_at)
                values, stdout, called = self.run_resolver([[pr]], sha=sha, details=pr)
                self.assertEqual(values, {
                    'merge_sha': sha, 'head_sha': head, 'pr_number': str(number),
                    'pr_url': f'https://github.com/owner/engine/pull/{number}',
                    'merged_at': merged_at, 'ok': 'true', **self.IDENTITIES})
                self.assertNotIn('::notice::', stdout)
                self.assertTrue(called)

    def test_push_reads_response_larger_than_linux_environment_limit(self):
        """The 131,072-byte limit per environment string exists only on Linux.

        This catches the transport regression on the CLOUD host, not on macOS.
        """
        pr = dict(self.merged_pr(), body='界' * 50_000)
        raw_json = json.dumps([[pr]], ensure_ascii=False)
        self.assertGreater(len(raw_json.encode('utf-8')), 131_072)
        values, stdout, called = self.run_resolver(None, raw_json=raw_json)
        self.assertEqual(values, {
            'merge_sha': self.SHA, 'head_sha': self.HEAD, 'pr_number': '231',
            'pr_url': 'https://github.com/owner/engine/pull/231',
            'merged_at': '2026-10-21T08:15:30Z', 'ok': 'true', **self.IDENTITIES})
        self.assertNotIn('::notice::', stdout)
        self.assertTrue(called)

    def test_push_defers_with_notice_and_no_metadata_without_one_match(self):
        pr = self.merged_pr()
        second = self.merged_pr(head='c' * 40, number=466)
        missing_merged_at = {key: value for key, value in pr.items() if key != 'merged_at'}
        cases = (
            ('another merge commit', [[dict(pr, merge_commit_sha='d' * 40)]]),
            ('open with merge time', [[dict(pr, state='open')]]),
            ('closed unmerged', [[dict(pr, merged_at=None)]]),
            ('missing merge time', [[missing_merged_at]]),
            ('two matches', [[pr, second]]),
            ('two matches across pages', [[pr], [second]]),
            ('empty list', [[]]),
            ('no pages', []),
            ('another base', [[dict(pr, base={'ref': 'release'})]]),
            ('null base', [[dict(pr, base=None)]]),
            ('missing base', [[{key: value for key, value in pr.items() if key != 'base'}]]),
        )
        for name, pages in cases:
            with self.subTest(case=name):
                values, stdout, called = self.run_resolver(pages)
                self.assertEqual(values, {'ok': 'false'})
                count = 2 if name.startswith('two matches') else 0
                self.assertEqual(stdout, f'::notice::found {count} merged PRs with merge '
                                 f'commit {self.SHA} and base main; skipping\n')
                self.assertTrue(called)

    def test_push_defers_on_malformed_json_without_metadata(self):
        valid_pages = json.dumps([[self.merged_pr()]])
        invalid_constants = [
            json.dumps([[dict(self.merged_pr(), merged_at=None)]]).replace('null', constant)
            for constant in ('NaN', 'Infinity', '-Infinity')]
        for raw_json in ('', '{', valid_pages[:-1], valid_pages + '\ntrailing garbage',
                         *invalid_constants):
            with self.subTest(raw_json=raw_json):
                values, stdout, called = self.run_resolver(None, raw_json=raw_json)
                self.assertEqual(values, {'ok': 'false'})
                self.assertEqual(stdout, '::notice::invalid merged PR response JSON; skipping\n')
                self.assertTrue(called)

    def test_push_defers_on_deeply_nested_json_without_metadata(self):
        values, stdout, called = self.run_resolver(None, raw_json='[' * 100_000)
        self.assertEqual(values, {'ok': 'false'})
        self.assertEqual(stdout, '::notice::invalid merged PR response JSON; skipping\n')
        self.assertTrue(called)

    def test_push_requires_an_array_of_page_arrays_of_pr_objects(self):
        pr = self.merged_pr()
        for pages in (None, {}, {'pages': [[pr]]}, pr, 231, True, 'not an array',
                      [pr], [[pr], None], [[pr], {'other': pr}],
                      [[pr], [None]], [[pr], [231]], [[pr], ['not a PR']]):
            with self.subTest(pages=pages):
                values, stdout, called = self.run_resolver(pages)
                self.assertEqual(values, {'ok': 'false'})
                self.assertEqual(stdout,
                                 '::notice::invalid merged PR response page array; skipping\n')
                self.assertTrue(called)

    def test_push_selects_matching_pr_across_pages_among_unrelated_prs(self):
        pr = self.merged_pr()
        pages = [[dict(pr, merge_commit_sha='c' * 40)],
                 [dict(pr, state='closed', merged_at=None), pr],
                 [dict(pr, base={'ref': 'release'})]]
        values, stdout, called = self.run_resolver(pages)
        self.assertEqual(values, {
            'merge_sha': self.SHA, 'head_sha': self.HEAD, 'pr_number': '231',
            'pr_url': 'https://github.com/owner/engine/pull/231',
            'merged_at': '2026-10-21T08:15:30Z', 'ok': 'true', **self.IDENTITIES})
        self.assertNotIn('::notice::', stdout)
        self.assertTrue(called)

    def test_push_treats_json_quotes_and_shell_text_as_data(self):
        pr = self.merged_pr()
        pr['title'] = '\nPY\nraise SystemExit(99)\n\'"$()`\n'
        pr['html_url'] += '?literal=$(printf injected)`printf injected`\'";'
        values, stdout, called = self.run_resolver([[pr]], details=pr)
        self.assertEqual(values, {
            'merge_sha': self.SHA, 'head_sha': self.HEAD, 'pr_number': '231',
            'pr_url': pr['html_url'], 'merged_at': pr['merged_at'], 'ok': 'true', **self.IDENTITIES})
        self.assertNotIn('::notice::', stdout)
        self.assertTrue(called)

    def test_dispatch_returns_exact_inputs_without_a_pr_lookup(self):
        values, stdout, called = self.run_resolver(
            [[self.merged_pr()]], sha='f' * 40, event='workflow_dispatch', inputs={
                'INPUT_MERGE_SHA': 'd' * 40, 'INPUT_HEAD_SHA': 'e' * 40,
                'INPUT_PR_NUMBER': '712', 'INPUT_PR_URL': 'https://github.com/owner/engine/pull/712',
                'INPUT_MERGED_AT': '2026-10-20T01:02:03Z'})
        self.assertEqual(values, {
            'merge_sha': 'd' * 40, 'head_sha': 'e' * 40, 'pr_number': '712',
            'pr_url': 'https://github.com/owner/engine/pull/712',
            'merged_at': '2026-10-20T01:02:03Z', 'ok': 'true'})
        self.assertNotIn('::notice::', stdout)
        self.assertFalse(called)

    def dispatch_inputs(self):
        return {'INPUT_MERGE_SHA': 'd' * 40, 'INPUT_HEAD_SHA': 'e' * 40,
                'INPUT_PR_NUMBER': '712', 'INPUT_PR_URL': 'https://github.com/owner/engine/pull/712',
                'INPUT_MERGED_AT': '2026-10-20T01:02:03Z'}

    def changed_pr(self, changes):
        pr = self.merged_pr()
        for path, value in changes:
            target = pr
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
        return pr

    def test_push_authorizes_the_selected_full_pr_response_without_payload_fields(self):
        pr = self.merged_pr()
        summary_pr = {key: value for key, value in pr.items() if key not in ('merged', 'merged_by')}
        record = self.resolver_record([[summary_pr]], details=pr, authorize=True)
        self.assertEqual(record['resolver'].returncode, 0)
        self.assertEqual(record['values']['merged_by'], 'merger')
        self.assertIsNotNone(record['authorization'])
        self.assertEqual(record['authorization'].returncode, 0,
                         record['authorization'].stdout + record['authorization'].stderr)
        self.assertEqual(record['authorized'], 'ok=true\n')
        self.assertIn('authz: allowed (push by author)', record['summary'])
        self.assertEqual(record['lookup'].splitlines(), [
            f'api --paginate --slurp repos/owner/engine/commits/{self.SHA}/pulls?per_page=100',
            'api repos/owner/engine/pulls/231',
            'api repos/owner/engine/collaborators/merger/permission --jq .permission'])

    def test_push_resolved_foreign_heads_and_untrusted_identities_never_authorize(self):
        cases = (
            ([(('head', 'repo', 'full_name'), 'outside/fork')], None, 'another repository'),
            ([(('head', 'repo', 'fork'), True)], None, 'from a fork'),
            ([(('author_association',), 'NONE'), (('user', 'login'), 'outsider')],
             {'outsider': ('read', 0)}, 'author outsider'),
            ([(('author_association',), 'CONTRIBUTOR')],
             {'author': ('write', 1)}, 'author author'),
            ([], {'merger': ('read', 0)}, 'push merger merger'),
            ([], {'merger': ('write', 1)}, 'push merger merger'),
            ([], {'merger': ('', 1)}, 'push merger merger'),
        )
        for changes, permissions, message in cases:
            with self.subTest(changes=changes, permissions=permissions):
                details = self.changed_pr(changes)
                record = self.resolver_record([[self.merged_pr()]], details=details,
                                               authorize=True, permissions=permissions)
                self.assertEqual(record['resolver'].returncode, 0)
                self.assertIsNotNone(record['authorization'])
                self.assertEqual(record['authorization'].returncode, 1,
                                 record['authorization'].stdout + record['authorization'].stderr)
                self.assertIn('authz: REFUSED:', record['summary'])
                self.assertIn(message, record['summary'])
                self.assertNotIn('authz: allowed', record['summary'])
                self.assertEqual(record['authorized'], '')

    def test_push_allows_api_bot_identities_and_private_member_permission_fallback(self):
        cases = [
            ([(('user', 'login'), bot), (('author_association',), 'NONE')], {})
            for bot in ('dependabot[bot]', 'pineforge-app[bot]')]
        # The merger still needs a permission when the author is a bot.
        cases = [(changes, {'merger': ('write', 0)}) for changes, _ in cases]
        cases += [([(('merged_by', 'login'), bot)], {})
                  for bot in ('dependabot[bot]', 'pineforge-app[bot]')]
        cases += [([(('author_association',), 'CONTRIBUTOR')],
                   {'author': (permission, 0), 'merger': ('write', 0)})
                  for permission in ('admin', 'maintain', 'write')]
        for changes, permissions in cases:
            with self.subTest(changes=changes, permissions=permissions):
                record = self.resolver_record([[self.merged_pr()]], details=self.changed_pr(changes),
                                               authorize=True, permissions=permissions)
                self.assertEqual(record['authorization'].returncode, 0,
                                 record['authorization'].stdout + record['authorization'].stderr)
                self.assertEqual(record['authorized'], 'ok=true\n')
                self.assertNotIn('REFUSED', record['summary'])

    def test_push_unmatched_or_ambiguous_prs_never_reach_authorization(self):
        pr = self.merged_pr()
        for pages in ([], [[dict(pr, merged_at=None)]], [[pr, self.merged_pr(number=466)]]):
            with self.subTest(pages=pages):
                record = self.resolver_record(pages, authorize=True)
                self.assertEqual(record['values'], {'ok': 'false'})
                self.assertIsNone(record['authorization'])
                self.assertEqual(record['authorized'], '')
                self.assertIn('::notice::', record['resolver'].stdout)
                self.assertEqual(len(record['lookup'].splitlines()), 1)

    def test_push_refuses_unreadable_full_pr_responses_and_api_failures(self):
        for raw in ('', '{', 'null', '[]', '231', 'NaN', '[' * 100_000):
            with self.subTest(raw=raw[:30]):
                record = self.resolver_record([[self.merged_pr()]], raw_details=raw,
                                               authorize=True, expected_code=1)
                self.assertEqual(record['values'], {})
                self.assertIsNone(record['authorization'])
                self.assertEqual(record['authorized'], '')
                self.assertIn('authz: REFUSED:', record['summary'])
        for kwargs in ({'api_exit': 1}, {'detail_exit': 1}):
            with self.subTest(api_failure=kwargs):
                record = self.resolver_record([[self.merged_pr()]], authorize=True,
                                               expected_code=1, **kwargs)
                self.assertEqual(record['values'], {})
                self.assertIsNone(record['authorization'])
                self.assertEqual(record['authorized'], '')
                self.assertNotIn('authz: allowed', record['summary'])

    def test_push_refuses_changed_full_pr_merge_base_head_or_number(self):
        for path, value in (
                (('number',), 466), (('state',), 'open'), (('merged',), False),
                (('merged',), None), (('merged',), 'true'),
                (('merge_commit_sha',), 'c' * 40), (('base', 'ref'), 'release'),
                (('head', 'sha'), 'd' * 40), (('merged_at',), '2026-10-20T01:02:03Z')):
            with self.subTest(path=path, value=value):
                record = self.resolver_record([[self.merged_pr()]],
                                               details=self.changed_pr([(path, value)]),
                                               detail_number=231, authorize=True, expected_code=1)
                self.assertEqual(record['values'], {})
                self.assertIsNone(record['authorization'])
                self.assertEqual(record['authorized'], '')
                self.assertIn('resolved PR no longer matches the pushed merge', record['summary'])

    def test_push_refuses_empty_unreadable_or_multiline_required_pr_fields(self):
        paths = (('html_url',), ('head', 'sha'), ('base', 'ref'), ('merged_at',),
                 ('head', 'repo', 'full_name'), ('author_association',),
                 ('user', 'login'), ('merged_by', 'login'))
        for path in paths:
            for value in ('', '   ', None, [], {}, True, 'bad\nok=true', 'bad\rvalue', 'bad\x00value'):
                with self.subTest(path=path, value=value):
                    record = self.resolver_record([[self.merged_pr()]],
                                                   details=self.changed_pr([(path, value)]),
                                                   authorize=True, expected_code=1)
                    self.assertEqual(record['values'], {})
                    self.assertEqual(record['authorized'], '')
                    self.assertIsNone(record['authorization'])
                    self.assertIn('empty or unreadable PR field', record['summary'])
        for value in ('', None, 'false', 0, 1):
            with self.subTest(fork=value):
                values, stdout, _ = self.run_resolver(
                    [[self.merged_pr()]], details=self.changed_pr([(('head', 'repo', 'fork'), value)]),
                    expected_code=1)
                self.assertEqual(values, {})
                self.assertIn('empty or unreadable PR field: head.repo.fork', stdout)
        for number in (None, '', True, 0, -1, '231'):
            with self.subTest(number=number):
                values, stdout, _ = self.run_resolver(
                    [[dict(self.merged_pr(), number=number)]], expected_code=1)
                self.assertEqual(values, {})
                self.assertIn('empty or unreadable PR number', stdout)

    def test_full_pr_number_type_control_refuses_boolean_one(self):
        workflow = (ROOT / CI_SOURCES[2]).read_text()
        selected = self.merged_pr(number=1)
        details = dict(selected, number=True)
        values, stdout, _ = self.run_resolver(
            [[selected]], details=details, detail_number=1, expected_code=1)
        self.assertEqual(values, {})
        self.assertIn('resolved PR no longer matches the pushed merge', stdout)
        changed = in_step(workflow, 'promote-guard', 'Resolve the merged PR',
                          'type(pr.get("number")) is not int', 'False')
        sources = [(ROOT / path).read_text() for path in CI_SOURCES]
        sources[2] = changed
        self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                      ci_workflow_findings(*sources))
        values, _, _ = self.run_resolver(
            [[selected]], details=details, detail_number=1, workflow=changed)
        self.assertEqual(values['pr_number'], '1')
        self.assertEqual(values['ok'], 'true')

    def test_resolver_sha_controls_refuse_invalid_pushed_and_head_hashes(self):
        workflow = (ROOT / CI_SOURCES[2]).read_text()
        for name, value, kwargs in (
                ('sha', '', {'sha': ''}),
                ('sha', 'x' * 40, {'sha': 'x' * 40}),
                ('head_sha', 'short', {}),
                ('head_sha', 'x' * 40, {})):
            with self.subTest(field=name, value=value):
                pr = self.merged_pr(sha=value) if name == 'sha' else self.merged_pr(head=value)
                values, stdout, _ = self.run_resolver([[pr]], details=pr, expected_code=1, **kwargs)
                self.assertEqual(values, {})
                self.assertIn('empty or unreadable', stdout)
                changed = in_step(workflow, 'promote-guard', 'Resolve the merged PR',
                                  f'if not re.fullmatch(r"[0-9a-f]{{40}}", {name}):', 'if False:')
                sources = [(ROOT / path).read_text() for path in CI_SOURCES]
                sources[2] = changed
                self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                              ci_workflow_findings(*sources))
                values, _, _ = self.run_resolver([[pr]], details=pr, workflow=changed, **kwargs)
                self.assertEqual(values['ok'], 'true')

    def test_dispatch_keeps_its_guard_arm_and_all_five_inputs(self):
        record = self.resolver_record([], event='workflow_dispatch', inputs=self.dispatch_inputs(),
                                       authorize=True)
        self.assertEqual(record['authorization'].returncode, 0,
                         record['authorization'].stdout + record['authorization'].stderr)
        self.assertEqual(record['authorized'], 'ok=true\n')
        self.assertIn('authz: allowed (workflow_dispatch)', record['summary'])
        self.assertEqual(record['lookup'], '')
        for key in self.dispatch_inputs():
            for value in ('', '   ', 'bad\nok=true', 'bad\rvalue'):
                with self.subTest(input=key, value=value):
                    inputs = dict(self.dispatch_inputs(), **{key: value})
                    values, stdout, called = self.run_resolver(
                        [], event='workflow_dispatch', inputs=inputs, expected_code=1)
                    self.assertEqual(values, {})
                    self.assertFalse(called)
                    self.assertIn('empty or unreadable dispatch input', stdout)

    def test_push_and_dispatch_refuse_unsupported_refs_before_api_or_checkout(self):
        for event in ('push', 'workflow_dispatch'):
            for ref in ('', 'refs/heads/develop', 'refs/tags/v1'):
                with self.subTest(event=event, ref=ref):
                    values, stdout, called = self.run_resolver(
                        [[self.merged_pr()]], event=event, ref=ref,
                        inputs=self.dispatch_inputs(), expected_code=1)
                    self.assertEqual(values, {})
                    self.assertFalse(called)
                    self.assertIn('baseline promotion requires refs/heads/main', stdout)

    def test_resolver_refusal_mutations_change_real_results_and_report_findings(self):
        workflow = (ROOT / CI_SOURCES[2]).read_text()
        cases = (
            ('          [ "$REF" = refs/heads/main ] || fail "baseline promotion requires refs/heads/main"\n',
             '', {'event': 'workflow_dispatch', 'ref': 'refs/heads/develop',
                   'inputs': self.dispatch_inputs()}),
            ('pr.get("merged") is not True', 'False',
             {'details': dict(self.merged_pr(), merged=False)}),
            ('field(pr, "head", "sha") != field(selected, "head", "sha")', 'False',
             {'details': self.changed_pr([(('head', 'sha'), 'c' * 40)])}),
            ('field(pr, "merged_at") != field(selected, "merged_at")', 'False',
             {'details': dict(self.merged_pr(), merged_at='2026-10-20T01:02:03Z')}),
        )
        for before, after, kwargs in cases:
            with self.subTest(refusal=before):
                baseline, _, _ = self.run_resolver([[self.merged_pr()]], expected_code=1, **kwargs)
                self.assertEqual(baseline, {})
                changed = in_step(workflow, 'promote-guard', 'Resolve the merged PR', before, after)
                self.assertNotEqual(changed, workflow)
                sources = [(ROOT / path).read_text() for path in CI_SOURCES]
                sources[2] = changed
                self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                              ci_workflow_findings(*sources))
                values, _, _ = self.run_resolver([[self.merged_pr()]], workflow=changed, **kwargs)
                self.assertEqual(values['ok'], 'true')

    def test_resolver_nesting_control_detects_a_removed_recursion_skip(self):
        workflow = (ROOT / CI_SOURCES[2]).read_text()
        changed = in_step(workflow, 'promote-guard', 'Resolve the merged PR',
                          'except (ValueError, RecursionError):', 'except ValueError:')
        sources = [(ROOT / path).read_text() for path in CI_SOURCES]
        sources[2] = changed
        self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                      ci_workflow_findings(*sources))
        values, _, called = self.run_resolver(None, raw_json='[' * 100_000,
                                              workflow=changed, expected_code=1)
        self.assertEqual(values, {})
        self.assertTrue(called)

    def test_resolver_crash_propagates_its_own_exit_without_authorization(self):
        workflow = (ROOT / CI_SOURCES[2]).read_text()
        changed = in_step(workflow, 'promote-guard', 'Resolve the merged PR',
                          '          set -euo pipefail\n',
                          '          set -euo pipefail\n          exit 37\n')
        sources = [(ROOT / path).read_text() for path in CI_SOURCES]
        sources[2] = changed
        self.assertIn('baseline promotion must start with the pinned read-only promote-guard',
                      ci_workflow_findings(*sources))
        record = self.resolver_record([[self.merged_pr()]], workflow=changed,
                                       authorize=True, expected_code=37)
        self.assertEqual(record['resolver'].returncode, 37)
        self.assertEqual(record['values'], {})
        self.assertEqual(record['authorized'], '')
        self.assertIsNone(record['authorization'])
        self.assertEqual(record['lookup'], '')


class CanonAuthz(unittest.TestCase):
    def test_real_workflow_scripts_admit_our_events_and_refuse_untrusted_identity(self):
        paths = (*CI_SOURCES[:2], '.github/workflows/release-ruleset-probe.yml')
        scripts = []
        for path in paths:
            workflow = (ROOT / path).read_text()
            self.assertIn('authz', _jobs(workflow), f'{path} must authorize before repository code')
            scripts.append(job_script(workflow, 'authz'))
        for script in scripts[1:]:
            self.assertEqual(scripts[0], script)
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
            ({'EVENT': 'workflow_dispatch'}, True, False),
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
                    with self.subTest(workflow=paths[index], identity=change):
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
    def test_actionlint_runner_labels_reads_gke_scalar_label(self):
        workflow = self.tree / '.github/workflows/gke-label.yml'
        workflow.write_text(
            'on: workflow_dispatch\njobs:\n  inspect:\n'
            '    runs-on: gke-unknown\n'
            '    steps:\n      - run: "true"\n')
        code, output = self.runner_labels()
        self.assertEqual(code, 1, output)
        self.assertIn('gke-label.yml job inspect: label "gke-unknown" is not declared '
                      'in .github/actionlint.yaml', output.splitlines())

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

    def test_light_scalar_runner_labels_are_declared_and_required(self):
        original = self.config.read_text()
        for label in (GKE_2, GKE_4):
            with self.subTest(label=label):
                self.assertIn(f'    - {label}\n', original)
                workflow = self.tree / '.github/workflows/light-label.yml'
                workflow.write_text(
                    'on: workflow_dispatch\njobs:\n  inspect:\n'
                    + runner_declaration(label) + '    steps:\n      - run: git --version\n')
                self.config.write_text(original)
                code, output = self.runner_labels()
                self.assertEqual(code, 0, output)
                self.config.write_text(original.replace(f'    - {label}\n', '', 1))
                code, output = self.runner_labels()
                self.assertEqual(code, 1, output)
                self.assertIn(f'light-label.yml job inspect: label "{label}" is not declared '
                              'in .github/actionlint.yaml', output.splitlines())
        self.config.write_text(original)

    def test_actionlint_runner_labels_accepts_retained_fleets_and_refuses_retired_pr_route(self):
        workflow = self.tree / '.github/workflows/gcp-labels.yml'
        workflow.write_text(
            'on: workflow_dispatch\njobs:\n  post:\n'
            '    runs-on: ["self-hosted", "pf-gcp-post"]\n'
            '    steps:\n      - run: "true"\n'
            '  main:\n'
            '    runs-on: ["self-hosted", "pf-gcp-main"]\n'
            '    steps:\n      - run: "true"\n')
        code, output = self.runner_labels()
        self.assertEqual(code, 0, output)
        admitted = self.tree / '.github/workflows/gke-pr.yml'
        original = admitted.read_text()
        changed = in_job(original, 'corpus-parity-subset', runner_declaration(GKE_PR_8),
                         runner_declaration(PR_RUNNER))
        self.assertNotEqual(changed, original)
        admitted.write_text(changed)
        code, output = self.runner_labels()
        self.assertEqual(code, 1, output)
        self.assertIn('gke-pr.yml job corpus-parity-subset: label "pf-gcp-pr" is not declared '
                      'in .github/actionlint.yaml', output.splitlines())

    def test_actionlint_runner_labels_requires_each_larger_hosted_label(self):
        original = self.config.read_text()
        for label in ('pf-linux-x64-16', 'pf-linux-x64-32', 'pf-linux-x64-64'):
            with self.subTest(label=label):
                workflow = self.tree / '.github/workflows/larger-label.yml'
                workflow.write_text(
                    'on: workflow_dispatch\njobs:\n  inspect:\n'
                    f'    runs-on: {label}\n    steps:\n      - run: "true"\n')
                self.assertNotIn(f'    - {label}\n', original)
                self.config.write_text(original)
                code, output = self.runner_labels()
                self.assertEqual(code, 1, output)
                self.assertIn(f'larger-label.yml job inspect: label "{label}" is not declared '
                              'in .github/actionlint.yaml', output.splitlines())
                # Config declaration still governs the linter's label inventory;
                # runner_findings independently refuses paid labels everywhere.
                self.config.write_text(original + f'    - {label}\n')
                code, output = self.runner_labels()
                self.assertEqual(code, 0, output)
        self.config.write_text(original)

    def test_actionlint_runner_labels_requires_config(self):
        self.config.unlink()
        code, output = self.runner_labels()
        self.assertNotEqual(code, 0, output)
        self.assertIn('.github/actionlint.yaml: cannot read config:', output)
        self.assertIn('No such file or directory', output)

    def test_actionlint_runner_labels_rejects_removed_label(self):
        original = self.config.read_text()
        for label, sites in (
                ('pf-gcp-post', (('docs-post.yml', 'build'), ('release-ruleset-probe.yml', 'probe'))),
                ('gke-32cpu-64gb-amd64', (('gke-pr.yml', 'build'),
                                         ('ci-post.yml', 'build'), ('ci-post.yml', 'sanitizers'))),
                ('gke-16cpu-32gb-amd64', (('gke-pr.yml', 'build-debug'),
                                         ('ci-post.yml', 'build-debug'), ('ci-post.yml', 'kernel-only'),
                                         ('native-live.yml', 'native-live'), ('corpus-parity.yml', 'corpus-parity'))),
                ('gke-8cpu-16gb-amd64', (('gke-pr.yml', 'preflight'), ('gke-pr.yml', 'corpus-parity-subset'),
                                        ('ci-post.yml', 'preflight'), ('ci-post.yml', 'preflight-slow'),
                                        ('native-live.yml', 'native-live-shards'), ('corpus-parity.yml', 'corpus-parity-subset'))),
                ('gke-16cpu-32gb-arm64', (('ci-post.yml', 'build-arm'),)),
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
                for name, jobs in LIGHT_JOB_IDS.items():
                    for job in jobs:
                        self.assertNotIn(f'{name} job {job}: label "{label}" is not declared '
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
    def test_trusted_cache_rule_reads_fetched_paths_and_both_step_forms(self):
        for name in ('ci-post.yml', 'native-live.yml'):
            original = (ROOT / '.github/workflows' / name).read_text()
            self.assertEqual(ci_preflight.trusted_cache_findings({name: original}), [])
            for reference in ('actions/cache/restore', 'actions/cache/save', 'actions/cache'):
                for step in (f'      - uses: {reference}@0057852bfaa89a56745cba8c7296529d2fc39830\n',
                             '      - name: Reintroduced cache\n'
                             f'        uses: {reference}@0057852bfaa89a56745cba8c7296529d2fc39830\n'):
                    changed = original.replace('    steps:\n', '    steps:\n' + step, 1)
                    self.assertNotEqual(changed, original)
                    expected = [name + ': trusted workflow must not restore or save an Actions cache']
                    self.assertEqual(ci_preflight.trusted_cache_findings({name: changed}), expected)
                    self.assertEqual(ci_preflight.trusted_cache_findings(
                        {'.github/workflows/' + name: changed}), expected)

    def test_all_live_profiles_use_the_shared_pinned_dependency(self):
        kernel = _jobs((ROOT / CI_SOURCES[6]).read_text())['kernel-only']
        native_jobs = _jobs((ROOT / CI_SOURCES[1]).read_text())
        for job in (kernel, native_jobs['native-live'], native_jobs['native-live-shards']):
            self.assertIn('uses: ./.github/actions/setup-live-curl', job)
            self.assertNotIn('steps.curl-deps.outputs.cache-hit', job)
            self.assertNotIn('libcurl4-openssl-dev', job)
            for line in job.splitlines():
                if 'run: python3 scripts/ci_verify.py ' in line:
                    self.assertIn('--curl-dir "${{ steps.curl-deps.outputs.curl-dir }}"', line)
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        self.assertIn('bash scripts/build_live_curl.sh --metadata', action)
        self.assertNotIn('actions/cache', action)
        self.assertNotIn('cache-hit:', action)
        self.assertNotIn('steps.curl-cache', action)
        self.assertIn('CURL_CACHE_HIT: "false"', action)
        self.assertIn('value: ${{ steps.dependencies.outputs.curl-dir }}', action)
        self.assertIn('bash scripts/build_live_curl.sh build-native-deps 4', action)

    def test_curl_cache_is_refused_by_both_contract_entries(self):
        native = (ROOT / CI_SOURCES[1]).read_text()
        action_path = '.github/actions/setup-live-curl/action.yml'
        action = (ROOT / action_path).read_text()
        self.assertEqual(native_workflow_findings(native, action), [])
        mutations = {}
        for reference in ('actions/cache/restore', 'actions/cache/save', 'actions/cache'):
            changed = action.replace(
                '    - name: Build pinned WebSocket-enabled libcurl\n',
                '    - name: Reintroduced dependency cache\n'
                f'      uses: {reference}@0057852bfaa89a56745cba8c7296529d2fc39830\n'
                '      with:\n        path: build-native-deps/curl-install\n'
                '        key: curl-shared\n'
                '    - name: Build pinned WebSocket-enabled libcurl\n', 1)
            self.assertNotEqual(changed, action)
            self.assertIn('pinned curl must build cold without an Actions cache',
                          native_workflow_findings(native, changed))
            mutations[reference] = changed
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            for relative in ('.github/workflows/gke-pr.yml', action_path):
                target = tree / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text((ROOT / relative).read_text())
            self.assertEqual(admit_rule_findings(tree, run_actionlint=False), [])
            for reference, changed in mutations.items():
                with self.subTest(reference=reference):
                    (tree / action_path).write_text(changed)
                    self.assertIn(
                        action_path + ': trusted workflow must not restore or save an Actions cache',
                        admit_rule_findings(tree, run_actionlint=False))

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


class HostedNativeProfiles(unittest.TestCase):
    def workflow(self):
        return (ROOT / '.github/workflows/native-live.yml').read_text()

    def test_native_jobs_have_exact_hosted_pins_without_fleet_metadata(self):
        native = self.workflow()
        jobs = _jobs(native)
        pins = NATIVE_HOSTED_RUNNERS
        self.assertEqual(ci_preflight.HOSTED_JOB_RUNNERS['native-live.yml'], pins)
        self.assertEqual(ci_preflight.runner_findings({'native-live.yml': native}), [])
        for job, label in pins.items():
            with self.subTest(job=job):
                self.assertEqual(ci_preflight._job_values(jobs.get(job, ''), 'runs-on'), [label])
                self.assertNotIn('PF_CI_LABELS', jobs.get(job, ''))
                suffix = '' if label.startswith('gke-') else ' (hosted)'
                metadata_class = 'GKE' if label.startswith('gke-') else 'hosted'
                for replacement in (PR_RUNNER, RELEASE_RUNNER, 'pf-linux-x64-16',
                                    'pf-linux-x64-32', 'pf-linux-x64-64',
                                    'ubuntu-24.04' if label.startswith('gke-') else POST_RUNNER):
                    changed = in_job(native, job, runner_declaration(label),
                                     f'    runs-on: {replacement}\n')
                    self.assertNotEqual(changed, native)
                    self.assertIn(f'native-live.yml job {job} must run on {label}{suffix}',
                                  ci_preflight.runner_findings({'native-live.yml': changed}))
                for scope in (None, job):
                    before = '\nenv:\n' if scope is None else '\n    env:\n'
                    if scope is None:
                        after = before + f"  PF_CI_LABELS: '{POST_RUNNER}'\n"
                        changed = native.replace(before, after, 1)
                    elif before in jobs[job]:
                        changed = in_job(native, job, before,
                                         before + f"      PF_CI_LABELS: '{POST_RUNNER}'\n")
                    else:
                        changed = in_job(native, job, '    steps:\n',
                                         f"    env:\n      PF_CI_LABELS: '{POST_RUNNER}'\n    steps:\n")
                    self.assertNotEqual(changed, native)
                    self.assertIn(f'native-live.yml job {job} must not set PF_CI_LABELS '
                                  f'on a {metadata_class} job',
                                  ci_preflight.runner_findings({'native-live.yml': changed}))

    def test_native_shard_matrix_matches_the_command_denominator(self):
        native = self.workflow()
        jobs = _jobs(native)
        self.assertIn('native-live-shards', jobs, 'native needs four explicit shard cells')
        shard_job = jobs['native-live-shards']
        shards = re.search(r'(?m)^        shard: \[([0-9, ]+)\]$', shard_job)
        self.assertIsNotNone(shards, 'the native shard inventory must be readable')
        indices = [int(value.strip()) for value in shards[1].split(',')]
        denominator = re.search(r'--ctest-shard \$\{\{ matrix.shard \}\}/([0-9]+)', shard_job)
        self.assertIsNotNone(denominator, 'native must pass its matrix index to ci_verify')
        self.assertEqual(int(denominator[1]), 4)
        self.assertEqual(indices, list(range(1, int(denominator[1]) + 1)))
        self.assertNotIn('--ctest-shard', jobs['native-live'])
        finding = 'native-live-shards must run exactly native shards 1..4 with fail-fast false'
        for before, after in (
                ('shard: [1, 2, 3, 4]', 'shard: [1, 2, 3]'),
                ('shard: [1, 2, 3, 4]', 'shard: [1, 2, 3, 3]'),
                ('shard: [1, 2, 3, 4]', 'shard: [1, 2, 3, 4, 5]'),
                ('fail-fast: false', 'fail-fast: true')):
            with self.subTest(after=after):
                changed = in_job(native, 'native-live-shards', before, after)
                self.assertNotEqual(changed, native)
                self.assertIn(finding, native_workflow_findings(changed))
        changed = in_job(native, 'native-live-shards',
                         '--ctest-shard ${{ matrix.shard }}/4',
                         '--ctest-shard ${{ matrix.shard }}/3')
        self.assertIn('native-live-shards must verify each full profile with no step deadline or timeout flag and '
                      'native-only PR exclusion', native_workflow_findings(changed))

    def test_native_larger_labels_are_not_workflow_wide_exceptions(self):
        native = self.workflow()
        finding = 'native-live.yml must not use larger-runner labels; use the GCP fleet or hosted exceptions'
        for label in ('pf-linux-x64-16', 'pf-linux-x64-32', 'pf-linux-x64-64'):
            with self.subTest(label=label):
                changed = native + f'\n  unrelated:\n    runs-on: {label}\n    steps:\n      - run: "true"\n'
                self.assertIn(finding, ci_preflight.runner_findings({'native-live.yml': changed}))
                changed = native.replace('\nenv:\n', f'\nenv:\n  UNPINNED_RUNNER: {label}\n', 1)
                self.assertIn(finding, ci_preflight.runner_findings({'native-live.yml': changed}))
        swapped = in_job(native, 'native-live', runner_declaration(GKE_16),
                         '    runs-on: pf-linux-x64-16\n')
        self.assertNotEqual(swapped, native)
        self.assertIn(finding, ci_preflight.runner_findings({'native-live.yml': swapped}))
        for job, label in (('native-live', 'pf-linux-x64-64'),
                           ('native-live-shards', 'pf-linux-x64-16')):
            changed = in_job(native, job, '\n    env:\n',
                             f'\n    env:\n      UNPINNED_RUNNER: {label}\n')
            self.assertIn(finding, ci_preflight.runner_findings({'native-live.yml': changed}))
        caller = ('on: pull_request\njobs:\n  call:\n'
                  '    uses: ./.github/workflows/native-live.yml\n')
        findings = ci_preflight.runner_findings({'caller.yml': caller, 'native-live.yml': swapped})
        self.assertIn(finding, findings)
        self.assertIn('native-live.yml must split pull_request and post-merge events '
                      'into separate workflows', findings)

    def test_actionlint_declares_all_three_larger_x64_labels(self):
        config = (ROOT / '.github/actionlint.yaml').read_text()
        for label in ('pf-linux-x64-16', 'pf-linux-x64-32', 'pf-linux-x64-64'):
            with self.subTest(label=label):
                self.assertEqual(config.splitlines().count(f'    - {label}'), 0)

    def test_native_aggregate_requires_each_shard_result_and_keeps_its_check_name(self):
        native = self.workflow()
        aggregate = _jobs(native).get('native-live-native', '')
        self.assertEqual(ci_preflight._job_values(aggregate, 'name'), ['native-live (native)'])
        self.assertEqual(ci_preflight._needs(aggregate), ['authz', 'native-live-shards'])
        self.assertEqual(ci_preflight._job_values(aggregate, 'if'), ['always()'])
        script = job_script(native, 'native-live-native', 'Require every native shard to have passed')
        valid = {'AUTHZ_RESULT': 'success', 'SHARDS_RESULT': 'success',
                 **{f'SHARD_{index}_RESULT': 'success' for index in range(1, 5)}}
        for field in valid:
            for result in ('success', 'failure', 'cancelled', 'skipped', ''):
                with self.subTest(field=field, result=result):
                    ran = subprocess.run(['bash', '-e', '-c', script], capture_output=True,
                                         env={**os.environ, **valid, field: result})
                    self.assertEqual(ran.returncode, 0 if result == 'success' else 1, ran.stdout)
        finding = 'native-live-native must fail unless all four native shard proofs succeed'
        for index in range(1, 5):
            for before, after in (
                    (f'          SHARD_{index}_RESULT: ${{{{ needs.native-live-shards.outputs.shard_{index} }}}}',
                     f'          SHARD_{index}_RESULT: success'),
                    (f'          test "$SHARD_{index}_RESULT" = success', '          true')):
                changed = in_job(native, 'native-live-native', before, after)
                self.assertNotEqual(changed, native)
                self.assertIn(finding, native_workflow_findings(changed))
        for job, before, after, expected in (
                ('native-live-native', '    if: always()', '    if: success()', finding),
                ('native-live-native', '    needs: [authz, native-live-shards]', '    needs: [authz]', finding),
                ('native-live-gate', '    needs: [authz, native-live, native-live-native]',
                 '    needs: [authz, native-live]', 'native-live-gate must fail unless every profile succeeds'),
                ('native-live-gate', '          test "$NATIVE_RESULT" = success', '          true',
                 'native-live-gate must fail unless every profile succeeds')):
            changed = in_job(native, job, before, after)
            self.assertNotEqual(changed, native)
            self.assertIn(expected, native_workflow_findings(changed))

    def test_native_shard_proofs_read_the_verifiers_own_summary(self):
        native = self.workflow()
        shard_job = _jobs(native).get('native-live-shards', '')
        self.assertIn('id: shard-proof', shard_job, 'each shard needs its own verifier proof')
        script = job_script(native, 'native-live-shards', 'Record native shard proof')
        valid = {'profile': 'native', 'status': 'passed', 'exitCode': 0, 'failures': [],
                 'ctestShard': {'index': 1, 'count': 4}}
        cases = [(valid, True), ({**valid, 'status': 'failed'}, False),
                 ({**valid, 'exitCode': 1}, False), ({**valid, 'exitCode': False}, False),
                 ({**valid, 'profile': 'release'}, False), ({**valid, 'failures': ['ctest']}, False),
                 ({**valid, 'ctestShard': {'index': 2, 'count': 4}}, False),
                 ({**valid, 'ctestShard': {'index': 1, 'count': 3}}, False),
                 ({}, False), ('deliberately malformed JSON', False), (None, False)]
        for summary, passes in cases:
            with self.subTest(summary=summary), tempfile.TemporaryDirectory() as directory:
                tree = Path(directory)
                (tree / 'build-live').mkdir()
                if summary is not None:
                    (tree / 'build-live/ci-summary.json').write_text(
                        summary if isinstance(summary, str) else json.dumps(summary))
                output = tree / 'github-output'
                ran = subprocess.run([sys.executable, '-c', script], capture_output=True, cwd=tree,
                                     env={**os.environ, 'SHARD_INDEX': '1', 'GITHUB_OUTPUT': str(output)})
                self.assertEqual(ran.returncode == 0, passes, ran.stderr)
                self.assertEqual(output.read_text() if output.exists() else '',
                                 'shard_1=success\n' if passes else '')
        finding = ('native-live-shards must publish each successful verifier summary '
                   'under its own shard output')
        for index in range(1, 5):
            before = f'      shard_{index}: ${{{{ steps.shard-proof.outputs.shard_{index} }}}}\n'
            changed = in_job(native, 'native-live-shards', before, '')
            self.assertIn(finding, native_workflow_findings(changed))
        for before, after in (
                ('shell: python', 'shell: bash'),
                ('summary = json.loads(source.read_text())', 'summary = {}'),
                ('summary.get("status") != "passed"', 'False'),
                ('raise SystemExit("native shard summary did not pass")', 'pass')):
            changed = in_job(native, 'native-live-shards', before, after)
            self.assertNotEqual(changed, native)
            self.assertIn(finding, native_workflow_findings(changed))

    def test_native_shard_builds_cold_and_diagnostic_names_are_distinct(self):
        native = self.workflow()
        shard_job = _jobs(native).get('native-live-shards', '')
        self.assertNotIn('actions/cache', shard_job)
        self.assertIn('name: ci-diagnostics-native-live-${{ matrix.profile }}-${{ matrix.shard }}of4', shard_job)
        for before, after, finding in (
                ('    steps:\n', '    steps:\n'
                 '      - uses: actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830\n',
                 'native-live-shards must build cold without an Actions cache'),
                ('-${{ matrix.shard }}of4', '', 'native-live-shards must retain per-profile diagnostics '
                 'on failure and preserve safe full checkout')):
            changed = in_job(native, 'native-live-shards', before, after)
            self.assertNotEqual(changed, native)
            self.assertIn(finding, native_workflow_findings(changed))


class IndependentNativeProfiles(unittest.TestCase):
    def test_native_contract_rejects_lost_profiles_budgets_caches_and_diagnostics(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        self.assertEqual(native_workflow_findings(native, action), [])
        mutations = [
            ('fail-fast: false', 'fail-fast: true'),
            ('    name: native-live (${{ matrix.profile }})',
             '    name: native-live (${{ matrix.profile }})\n    needs: prior'),
            ('needs: [authz, native-live, native-live-native]', 'needs: [authz]'),
            ('test "$PROFILES_RESULT" = success', 'true'),
            ('PROFILES_RESULT: ${{ needs.native-live.result }}', 'PROFILES_RESULT: success'),
            ('contents: read', 'contents: write'),
            ('persist-credentials: false', 'persist-credentials: true'),
            ('fetch-depth: 0', 'fetch-depth: 1'),
            ('PINEFORGE_REQUIRE_RELEASE_TAGS: "1"', 'PINEFORGE_REQUIRE_RELEASE_TAGS: "0"'),
            ('uses: ./.github/actions/setup-live-curl', 'uses: ./unpinned-curl'),
            ('ccache --zero-stats', 'ccache --show-stats'),
            ('    steps:\n', '    steps:\n'
             '      - uses: actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830\n'),
            ('    steps:\n', '    steps:\n'
             '      - uses: actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830\n'),
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

    def test_curl_contract_rejects_cache_reentry_and_skipped_dependency_builds(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        cases = [
            ('CURL_CACHE_HIT: "false"', 'CURL_CACHE_HIT: "true"',
             'pinned curl must always build the checksum-pinned dependency and publish curl-dir'),
            ('bash scripts/build_live_curl.sh --metadata', 'true',
             'pinned curl must always build the checksum-pinned dependency and publish curl-dir'),
            ('bash scripts/build_live_curl.sh build-native-deps 4', 'true',
             'pinned curl must always build the checksum-pinned dependency and publish curl-dir'),
            ('      id: curl-build\n', '      id: curl-build\n      if: success()\n',
             'pinned curl must always build the checksum-pinned dependency and publish curl-dir'),
            ('value: ${{ steps.dependencies.outputs.curl-dir }}', 'value: unpinned',
             'pinned curl must always build the checksum-pinned dependency and publish curl-dir'),
        ]
        for reference in ('actions/cache/restore', 'actions/cache/save', 'actions/cache'):
            cases.append(('  steps:\n', '  steps:\n'
                          f'    - uses: {reference}@0057852bfaa89a56745cba8c7296529d2fc39830\n',
                          'pinned curl must build cold without an Actions cache'))
        for before, after, finding in cases:
            with self.subTest(mutation=(before, after)):
                self.assertIn(before, action)
                self.assertIn(finding, native_workflow_findings(
                    native, action.replace(before, after, 1)))

    def test_native_aggregate_rejects_failure_cancellation_and_skip(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        script = job_script(native, 'native-live-gate', 'Require every native profile to have passed')
        for authz, result, shards in ((authz, profile, shards)
                                      for authz in ('success', 'failure', 'cancelled', 'skipped', '')
                                      for profile in ('success', 'failure', 'cancelled', 'skipped', '')
                                      for shards in ('success', 'failure', 'cancelled', 'skipped', '')):
            with self.subTest(authz=authz, result=result, shards=shards):
                ran = subprocess.run(['bash', '-e', '-c', script], capture_output=True,
                                     env={**os.environ, 'AUTHZ_RESULT': authz, 'PROFILES_RESULT': result,
                                          'NATIVE_RESULT': shards})
                self.assertEqual(ran.returncode == 0, authz == result == shards == 'success', ran.stdout)
                if authz != 'success' or result != 'success' or shards != 'success':
                    self.assertEqual(ran.returncode, 1, ran.stdout)
                # A cancelled profile job is how the platform stops a hang at the job maximum: the
                # aggregate still fails, and the log names it as a HANG to re-run (EN-116).
                self.assertEqual(b'HANG (re-run)' in ran.stdout,
                                 'cancelled' in (result, shards), ran.stdout)

    def test_three_profiles_share_the_platform_maximum_and_no_fail_fast(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        jobs = _jobs(native)
        for job_name, profiles in (('native-live', ('live-sanitizers', 'live-tsan')),
                                   ('native-live-shards', ('native',))):
            job = jobs[job_name]
            self.assertIn('fail-fast: false', job)
            for profile in profiles:
                self.assertIn(f'- profile: {profile}\n', job)
            # GitHub's platform maximum is the only bound on either matrix.
            self.assertNotRegex(job, r'(?m)^    timeout-minutes:')
            self.assertNotRegex(job, r'(?m)^        timeout-minutes:')
            for budget in ('matrix.job_minutes', 'matrix.verify_minutes',
                           '--test-timeout', '--ctest-timeout'):
                self.assertNotIn(budget, job)
        self.assertIn('needs: [authz, native-live, native-live-native]', jobs['native-live-gate'])

    def test_both_native_entry_points_and_dependencies_always_build_cold(self):
        native = (ROOT / '.github/workflows/native-live.yml').read_text()
        action = (ROOT / '.github/actions/setup-live-curl/action.yml').read_text()
        self.assertNotIn('actions/cache', native)
        self.assertNotIn('actions/cache', action)
        self.assertNotIn('steps.curl-cache', action)
        self.assertIn('CURL_CACHE_HIT: "false"', action)
        for job in ('native-live', 'native-live-shards'):
            setup = ci_preflight._step_named(ci_preflight._steps(_jobs(native)[job]),
                                             '        id: curl-deps')
            self.assertEqual(ci_preflight._step_values(setup, 'uses'),
                             ['./.github/actions/setup-live-curl'])
            self.assertEqual(ci_preflight._step_values(setup, 'if'), [])
            self.assertEqual(ci_preflight._step_block(setup, 'with'), [])


PREFLIGHT_SLICE_COUNT = 8


def slice_index(spec):
    if not re.fullmatch(r'[0-9]+/[0-9]+', spec):
        raise ValueError('slice must use I/8 with 1 <= I <= 8')
    index, count = map(int, spec.split('/'))
    if count != PREFLIGHT_SLICE_COUNT or not 1 <= index <= count:
        raise ValueError('slice must use I/8 with 1 <= I <= 8')
    return index - 1


def slice_test_ids(test_ids, spec):
    selected = sorted(test_ids)[slice_index(spec)::PREFLIGHT_SLICE_COUNT]
    if not selected:
        raise ValueError(f'slice {spec} selects no tests')
    return selected


class PreflightSliceLoader(unittest.TestLoader):
    """Keep whole test methods while slicing the loader's sorted full IDs."""

    def __init__(self, spec):
        super().__init__()
        slice_index(spec)
        self.spec = spec

    def loadTestsFromModule(self, module, *, pattern=None):
        pending = [super().loadTestsFromModule(module, pattern=pattern)]
        cases = {}
        while pending:
            item = pending.pop()
            if isinstance(item, unittest.TestSuite):
                pending.extend(item)
            else:
                test_id = item.id()
                if test_id in cases:
                    raise ValueError(f'duplicate unittest ID: {test_id}')
                cases[test_id] = item
        if self.errors:
            raise ValueError('cannot slice a failed unittest discovery')
        selected = slice_test_ids(cases, self.spec)
        return self.suiteClass(cases[test_id] for test_id in selected)


class GkeAdmissionRuleTests(unittest.TestCase):
    """Red-first controls for the hosted admission and its fetched data rules."""

    WORKFLOW = ROOT / '.github/workflows/gke-pr.yml'

    def workflow(self) -> str:
        return self.WORKFLOW.read_text(encoding='utf-8')

    def assert_workflow_mutation(self, before: str, after: str, finding: str) -> None:
        self.assertNotEqual(before, after)
        self.assertIn(finding, gke_admit_workflow_findings(after))

    def test_real_admission_shape_is_accepted(self) -> None:
        self.assertEqual(gke_admit_workflow_findings(self.workflow()), [])

    def test_preflight_linter_prologue_is_used_once(self) -> None:
        original = self.workflow()
        finding = 'gke-pr.yml preflight must contain only checkout, linters and source checks'
        self.assertNotIn(finding, gke_admit_workflow_findings(original))
        for prologue in ('', '          set -euo pipefail\n' * 2):
            with self.subTest(prologue=prologue):
                changed = in_step(original, 'preflight', 'Install workflow linters',
                                  '          set -euo pipefail\n', prologue)
                self.assert_workflow_mutation(original, changed, finding)

    def test_proof_runner_forms_are_refused(self) -> None:
        original = self.workflow()
        for job, label in EXPECTED_GKE_JOBS['gke-pr.yml'].items():
            marker = re.search(
                r'(?m)^    runs-on:[^\n]*(?:\n      (?:group|labels):[^\n]*)*\n',
                _jobs(original)[job])
            self.assertIsNotNone(marker)
            runners = (
                f'group: gke-runners labels: {label}',
                f'{{group: gke-runners, labels: {label}}}',
                f'{{labels: {label}}}',
                f'[{label}]',
                f'["{label}"]',
                f'["self-hosted","{label}"]',
                f"${{{{ '{label}' }}}}",
                f"${{{{ inputs.runner || '{label}' }}}}",
                f"'{label}'",
                label.upper(),
                label.replace('amd64', 'arm64'),
                GKE_PR_16 if label != GKE_PR_16 else GKE_PR_32,
            )
            for runner in runners:
                with self.subTest(job=job, runner=runner):
                    changed = in_job(original, job, marker.group(), runner_declaration(runner))
                    self.assertIn(f'gke-pr.yml job {job} must use {label}',
                                  gke_admit_workflow_findings(changed))
                    self.assertIn(f'gke-pr.yml job {job} must run on {label}',
                                  ci_preflight.runner_findings({'gke-pr.yml': changed}))

    def test_trigger_mutation_is_red(self) -> None:
        original = self.workflow()
        changed = original.replace('pull_request_target:', 'pull_request:', 1)
        self.assert_workflow_mutation(original, changed,
                                      'gke-pr.yml: trigger must be exactly pull_request_target')

    def test_cache_secret_environment_and_local_workflow_mutations_are_red(self) -> None:
        original = self.workflow()
        mutations = (
            ('    permissions: {contents: read}\n',
             '    permissions: {contents: read}\n      environment: production\n',
             'gke-pr.yml: secrets, id-token and environment are refused'),
            ('      - name: Require an independently consistent successful result\n',
             '      - uses: actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830 # v4.3.0\n'
             '      - name: Require an independently consistent successful result\n',
             'gke-pr.yml: admission legs must not use Actions cache'),
            ('          name: gke-pr-corpus-parity-subset-diagnostics\n',
             '          name: gke-pr-corpus-parity-subset-diagnostics\n'
             '      - uses: ./.github/workflows/ci.yml\n',
             'gke-pr.yml: reusable workflow calls are refused'),
        )
        for before, after, finding in mutations:
            with self.subTest(finding=finding):
                self.assert_workflow_mutation(original, original.replace(before, after, 1), finding)

    def test_result_and_checkout_mutations_are_red(self) -> None:
        original = self.workflow()
        cases = (
            ('    if: always()\n', '    if: success()\n',
             'gke-pr.yml result must run always'),
            ('    needs: [admit, preflight, legs]\n',
             '    needs: [admit, preflight]\n',
             'gke-pr.yml result must need admit, preflight and legs'),
            ('          ref: ${{ needs.admit.outputs.head_sha }}\n',
             '          ref: ${{ github.event.pull_request.head.ref }}\n',
             'gke-pr.yml preflight checkout must use the admitted immutable head'),
            ('          ref: ${{ needs.admit.outputs.base_sha }}\n',
             '          ref: ${{ needs.admit.outputs.head_sha }}\n',
             'gke-pr.yml result checkout must use the base tree'),
            ('          ref: ${{ github.sha }}\n',
             '          ref: ${{ github.event.pull_request.head.sha }}\n',
             'gke-pr.yml admit checkout must use the base tree'),
        )
        for before, after, finding in cases:
            with self.subTest(finding=finding):
                self.assert_workflow_mutation(original, original.replace(before, after, 1), finding)

    def test_run_shell_interpolation_and_timeout_mutations_are_red(self) -> None:
        original = self.workflow()
        run_mutation = original.replace(
            '          set -euo pipefail\n',
            '          echo "${{ github.event.pull_request.head.ref }}"\n          set -euo pipefail\n',
            1)
        self.assert_workflow_mutation(original, run_mutation,
                                      'gke-pr.yml job admit must pass PR values through env')
        timeout_mutation = original.replace(
            '          set -euo pipefail\n',
            '          timeout 10 true\n          set -euo pipefail\n',
            1)
        self.assert_workflow_mutation(original, timeout_mutation,
                                      'gke-pr.yml admit must not declare a timeout or continue on error')

    def test_shape_bounds_are_real_independent_refusals(self) -> None:
        cases = (
            ('long-line.yml', 'x' * (ADMIT_MAX_LINE + 1) + '\n',
             f'long-line.yml:1: line length exceeds {ADMIT_MAX_LINE}'),
            ('many-lines.yml', ('x\n' * (ADMIT_MAX_LINES + 1)),
             f'many-lines.yml: line count exceeds {ADMIT_MAX_LINES}'),
            ('deep.yml', ('  ' * (ADMIT_MAX_DEPTH + 1)) + 'key: value\n',
             f'deep.yml:1: structure depth exceeds {ADMIT_MAX_DEPTH}'),
            ('large.yml', 'x' * (ADMIT_MAX_FILE_BYTES + 1),
             f'large.yml: fetched size exceeds {ADMIT_MAX_FILE_BYTES} bytes'),
        )
        for name, text, finding in cases:
            with self.subTest(name=name):
                self.assertIn(finding, admit_shape_findings({name: text}))
        total_findings = admit_shape_findings({
            f'file-{index}.yml': 'x' * (ADMIT_MAX_FILE_BYTES - 1)
            for index in range(5)})
        self.assertIn(f'admit: fetched total size exceeds {ADMIT_MAX_TOTAL_BYTES} bytes', total_findings)
        self.assertIn('admit: fetched file count 101 exceeds 100',
                      admit_shape_findings({f'count-{index}.yml': 'x\n' for index in range(101)}))
        self.assertIn('invalid.yml: fetched blob is not valid UTF-8',
                      admit_shape_findings({'invalid.yml': b'\xff'}))

    def test_parser_differential_rows_and_block_scalar_positive_control(self) -> None:
        positive = {
            'workflow.yml': (
                'permissions: {contents: read}\n'
                'runs-on: ["self-hosted", "pf-gcp-post"]\n'
                'run: |\n'
                '  echo "&& *.yml ${{ github.event_name }}"\n')
        }
        self.assertEqual(admit_yaml_findings(positive), [])
        negative = (
            'runs-on: [*alias]\n',
            'permissions: {<<: *x}\n',
            'uses: [x]\n',
            'runs-on: {group: gke-runners, labels: [x]}\n',
            'runs-on: ${{ fromJSON(matrix.runner) }}\n',
            'runs-on: [linux]\nruns-on: [windows]\n',
            '---\n',
            '? key\n',
            '"runs\\x2Don": linux\n',
            '\ufeffname: x\n',
            'run:\rvalue\n',
            'run: \x00\n',
            '  \tname: x\n',
        )
        for text in negative:
            with self.subTest(text=repr(text)):
                self.assertTrue(admit_yaml_findings({'negative.yml': text}))

    def test_bare_cr_in_scalar_values_is_refused(self) -> None:
        cases = (
            ('run:\rvalue\n', 1),
            ("name: 'before\rafter'\n", 1),
            ('name: "before\rafter"\n', 1),
            ('run: |\n  echo before\rafter\n', 2),
        )
        for text, line in cases:
            with self.subTest(text=repr(text)):
                self.assertIn(f'negative.yml:{line}: bare CR is not valid workflow YAML',
                              admit_yaml_findings({'negative.yml': text}))
        self.assertEqual(admit_yaml_findings({
            'positive.yml': 'name: "ordinary"\r\nrun: |\r\n  echo "ordinary"\r\n',
        }), [])

    def test_rules_only_reads_the_fetched_directory_as_data(self) -> None:
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second, \
                tempfile.TemporaryDirectory() as fetched:
            directory = Path(fetched)
            for relative in ('.github/workflows/gke-pr.yml', '.github/actionlint.yaml',
                             '.github/actions/setup-live-curl/action.yml'):
                target = directory / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, target)
            for relative in ('docs/ci.md', 'tests/CMakeLists.txt', 'scripts/ci_docs_only.py'):
                for tree, text in ((Path(first), (ROOT / relative).read_text()),
                                   (Path(second), 'contradictory source\n')):
                    target = tree / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(text)
            with mock.patch.object(ci_preflight, 'ROOT', Path(first)):
                normal = admit_rule_findings(directory, run_actionlint=False)
            self.assertEqual(normal, [])
            with mock.patch.object(ci_preflight, 'ROOT', Path(second)):
                contradictory = admit_rule_findings(directory, run_actionlint=False)
            self.assertEqual(contradictory, normal)


if __name__ == '__main__':
    if any(arg == '--slice' or arg.startswith('--slice=') for arg in sys.argv[1:]):
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument('--slice', required=True, metavar='I/8')
        args = parser.parse_args()
        try:
            program = unittest.main(argv=[sys.argv[0]],
                                    testLoader=PreflightSliceLoader(args.slice), exit=False)
        except ValueError as error:
            parser.error(str(error))
        if program.result.testsRun == 0:
            parser.exit(1, f'slice {args.slice} ran no tests\n')
        sys.exit(not program.result.wasSuccessful())
    else:
        unittest.main()
