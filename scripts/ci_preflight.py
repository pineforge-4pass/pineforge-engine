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
from collections.abc import Iterator
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
import unicodedata

from ci_verify import (ROOT, PRIVATE_FIXTURE_SKIP_LITERAL, private_fixtures_overlay,
                       source_guard_commands)
from ci_verifier_classes import verifier_class_tier

ACTIONLINT_VERSION = '1.7.12'
# Pin the exact ordered names extracted from the CMake slow-label list.
# Verifier and command-after slices inherit slow labels; their covers do not.
PR_SLOW_ROWS = (
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
PR_FAST_ROWS_SHA256 = '0509b2219786efb6878b193639344c299ff8075ba4532dbe09e80e6e97b60a9a'
# Linux PR jobs retain their fleet; secret-free post jobs use fixed GKE sizes.
# Release artifacts and baseline promotion retain their isolated -main policy.
PR_RUNNER = '["self-hosted","pf-gcp-pr"]'
POST_RUNNER = '["self-hosted","pf-gcp-post"]'
RELEASE_RUNNER = '["self-hosted","pf-gcp-main"]'
GKE_32_LABEL = 'gke-32cpu-64gb-amd64'
GKE_16_LABEL = 'gke-16cpu-32gb-amd64'
GKE_8_LABEL = 'gke-8cpu-16gb-amd64'
GKE_ARM_LABEL = 'gke-16cpu-32gb-arm64'
GKE_JOB_LABELS = {
    'ci-post.yml': {'preflight': GKE_8_LABEL, 'preflight-slow': GKE_8_LABEL,
                    'build': GKE_32_LABEL, 'build-debug': GKE_16_LABEL,
                    'sanitizers': GKE_32_LABEL, 'kernel-only': GKE_16_LABEL,
                    'build-arm': GKE_ARM_LABEL},
    'corpus-parity.yml': {'corpus-parity': GKE_16_LABEL, 'corpus-parity-subset': GKE_8_LABEL},
    'native-live.yml': {'native-live': GKE_16_LABEL, 'native-live-shards': GKE_8_LABEL},
}
GKE_JOB_RUNNERS = {
    name: {job: f'group: gke-runners labels: {label}' for job, label in jobs.items()}
    for name, jobs in GKE_JOB_LABELS.items()
}
CI_EVENTS = '''on:
  pull_request:
    # LB-387: suite jobs skip on a draft PR (the required aggregate then fails, never passes);
    # ready_for_review starts the full run when the PR is marked ready.
    # converted_to_draft re-runs the workflow so its failing aggregate supersedes the earlier ready run.
    types: [opened, synchronize, reopened, ready_for_review, converted_to_draft]
'''
CI_CONCURRENCY = '''concurrency:
  group: ${{ github.workflow }}-${{ github.event.pull_request.number || github.run_id }}-${{ github.event.pull_request.draft && 'draft' || 'ready' }}
  cancel-in-progress: ${{ github.event_name == 'pull_request' }}
'''
POST_CI_EVENTS = '''on:
  push:
    branches: [main]
  schedule:
    - cron: '30 1 * * *'
  workflow_dispatch:
    inputs:
      cold_native_cache:
        description: Run full native profiles without compiler or libcurl caches
        type: boolean
        default: false
'''
MATRIX_RUNNER = '${{ fromJSON(matrix.platform.runner) }}'
MATRIX_LINUX_RUNNER = f"'{PR_RUNNER}'"
HOSTED_MATRIX_RUNNER = '${{ matrix.runner }}'
# Strategies preserve existing check names without using events to route a job.
BUILD_STRATEGY = ('    strategy:\n'
                  '      fail-fast: false\n'
                  '      matrix:\n'
                  '        platform:\n'
                  '          - os: ubuntu-24.04\n'
                  '        build_type: [Release]\n')
POST_BUILD_STRATEGY = ('    strategy:\n'
                       '      fail-fast: false\n'
                       '      matrix:\n'
                       '        platform:\n'
                       '          - os: ubuntu-24.04\n'
                       '        build_type: [Release]\n'
                       '        shard: [1, 2, 3, 4]\n')
POST_DEBUG_BUILD_STRATEGY = POST_BUILD_STRATEGY.replace(
    'build_type: [Release]', 'build_type: [Debug]'
).replace('shard: [1, 2, 3, 4]', 'shard: [1, 2]')
POST_ARM_BUILD_STRATEGY = ('    strategy:\n'
                          '      fail-fast: false\n'
                          '      matrix:\n'
                          '        build_type: [Release, Debug]\n')
BUILD_NAME = 'build (${{ matrix.platform.os }}, ${{ matrix.build_type }})'
POST_ARM_BUILD_NAME = 'build (ubuntu-24.04-arm, ${{ matrix.build_type }})'
POST_SHARDS = {'build': 4, 'build-debug': 2, 'sanitizers': 4, 'kernel-only': 2}
POST_LEG_GATES = {
    'build-release-gate': ('build', 'build (ubuntu-24.04, Release)'),
    'build-debug-gate': ('build-debug', 'build (ubuntu-24.04, Debug)'),
    'sanitizers-gate': ('sanitizers', 'sanitizers'),
    'kernel-only-gate': ('kernel-only', 'kernel-only'),
}


def post_build_name(count: int) -> str:
    return ('build-shard (${{ matrix.platform.os }}, ${{ matrix.build_type }}, '
            '${{ matrix.shard }}/' + str(count) + ')')


RELEASE_STRATEGY = ('    strategy:\n'
                    '      fail-fast: false\n'
                    '      matrix:\n'
                    '        include:\n'
                    '          - triple: linux-x86_64\n'
                    f'            runner: {RELEASE_RUNNER}\n'
                    '          - triple: linux-aarch64\n'
                    '            runner: ubuntu-22.04-arm\n')
NATIVE_STRATEGY = '''    strategy:
      fail-fast: false
      matrix:
        include:
          - profile: live-sanitizers
            build_dir: build-live-sanitizers
          - profile: live-tsan
            build_dir: build-live-tsan
'''
NATIVE_SHARD_STRATEGY = '''    strategy:
      fail-fast: false
      matrix:
        shard: [1, 2, 3, 4]
        include:
          - profile: native
            build_dir: build-live
'''
NATIVE_SHARD_PROOF_STEP = '''      - name: Record native shard proof
        id: shard-proof
        shell: python
        env:
          SHARD_INDEX: ${{ matrix.shard }}
        run: |
          import json
          import os
          from pathlib import Path
          source = Path("build-live/ci-summary.json")
          print(f"Reading native shard proof: {source}", flush=True)
          summary = json.loads(source.read_text())
          index = int(os.environ["SHARD_INDEX"])
          if (summary.get("profile") != "native" or summary.get("status") != "passed"
                  or type(summary.get("exitCode")) is not int or summary["exitCode"] != 0
                  or summary.get("failures") != []
                  or summary.get("ctestShard") != {"index": index, "count": 4}):
              raise SystemExit("native shard summary did not pass")
          with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
              output.write(f"shard_{index}=success\\n")
'''
NATIVE_SHARD_GATE_STEP = '''      - name: Require every native shard to have passed
        env:
          AUTHZ_RESULT: ${{ needs.authz.result }}
          SHARDS_RESULT: ${{ needs.native-live-shards.result }}
          SHARD_1_RESULT: ${{ needs.native-live-shards.outputs.shard_1 }}
          SHARD_2_RESULT: ${{ needs.native-live-shards.outputs.shard_2 }}
          SHARD_3_RESULT: ${{ needs.native-live-shards.outputs.shard_3 }}
          SHARD_4_RESULT: ${{ needs.native-live-shards.outputs.shard_4 }}
        run: |
          printf 'native shards: %s\\n' "$SHARDS_RESULT"
          if test "$SHARDS_RESULT" = cancelled; then
            printf 'HANG (re-run): a native shard job was cancelled\\n'
          fi
          test "$AUTHZ_RESULT" = success
          test "$SHARDS_RESULT" = success
          test "$SHARD_1_RESULT" = success
          test "$SHARD_2_RESULT" = success
          test "$SHARD_3_RESULT" = success
          test "$SHARD_4_RESULT" = success
'''
NATIVE_GATE_STEP = '''      - name: Require every native profile to have passed
        env:
          AUTHZ_RESULT: ${{ needs.authz.result }}
          PROFILES_RESULT: ${{ needs.native-live.result }}
          NATIVE_RESULT: ${{ needs.native-live-native.result }}
        run: |
          printf 'native profiles: %s\\n' "$PROFILES_RESULT"
          printf 'native shard aggregate: %s\\n' "$NATIVE_RESULT"
          if test "$PROFILES_RESULT" = cancelled || test "$NATIVE_RESULT" = cancelled; then
            printf 'HANG (re-run): a native profile job was cancelled, which is how the platform stops a job at its 360-minute maximum\\n'
          fi
          test "$AUTHZ_RESULT" = success
          test "$PROFILES_RESULT" = success
          test "$NATIVE_RESULT" = success
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
# Every inline job in the workflows a CI run starts, and the command that
# must carry the core count. Fleet choice comes only from the event graph.
# No job or step may declare timeout-minutes: owner #827 removed every
# declaration on 2026-10-20.
# GitHub's own 6 h platform maximum is the only bound; docs/ci.md says so.
JOB_COMMANDS = {
    'ci.yml': {'authz': None, 'changes': None, 'preflight': None,
               'build': None, 'build-gate': None},
    'ci-post.yml': {'authz': None, 'changes': None, 'preflight': None,
                    'preflight-slow': None, 'build': VERIFY, 'build-arm': VERIFY,
                    'build-debug': VERIFY, 'sanitizers': VERIFY, 'kernel-only': VERIFY,
                    **dict.fromkeys(POST_LEG_GATES), 'build-gate': None},
    'native-live.yml': {'authz': None, 'native-live': VERIFY, 'native-live-shards': VERIFY,
                        'native-live-native': None, 'native-live-gate': None},
    'corpus-parity.yml': {'authz': None, 'corpus-parity': PARITY, 'corpus-parity-subset': PARITY},
    'corpus-parity-subset.yml': {'authz': None, 'corpus-parity-subset': PARITY},
    'docs.yml': {'authz': None, 'build': None},
    'docs-post.yml': {'authz': None, 'build': None},
}
# Exact hosted pins and GKE group/size mappings, checked per job.
# Historical name: this table also holds the exact GKE pins.
HOSTED_JOB_RUNNERS = {
    'ci.yml': dict.fromkeys(('authz', 'changes', 'build-gate'), 'ubuntu-24.04'),
    'ci-post.yml': {**dict.fromkeys(('authz', 'changes', 'build-gate'), 'ubuntu-24.04'),
                    **dict.fromkeys(POST_LEG_GATES, 'ubuntu-24.04'),
                    **GKE_JOB_RUNNERS['ci-post.yml']},
    **{name: {'authz': 'ubuntu-24.04'} for name in (
        'corpus-parity-subset.yml', 'docs-post.yml')},
    'native-live.yml': {'authz': 'ubuntu-24.04', **GKE_JOB_RUNNERS['native-live.yml'],
                        'native-live-native': 'ubuntu-24.04', 'native-live-gate': 'ubuntu-24.04'},
    'corpus-parity.yml': {'authz': 'ubuntu-24.04', **GKE_JOB_RUNNERS['corpus-parity.yml']},
    'docs.yml': {'authz': 'ubuntu-24.04', 'build': 'ubuntu-24.04'},
    'promote-baseline.yml': {'promote-guard': 'ubuntu-latest'},
}
# Release/probe credential pins and ARM strategies remain isolated.
RELEASE_JOB_RUNNERS = {
    'release.yml': {'release': RELEASE_RUNNER, 'prebuilt': HOSTED_MATRIX_RUNNER,
                    'notify-hub': RELEASE_RUNNER},
    'release-ruleset-probe.yml': {'authz': 'ubuntu-24.04', 'probe': POST_RUNNER},
    'promote-baseline.yml': {'promote': RELEASE_RUNNER},
}
# Credential-bearing jobs outside the release pins and gated docs build retain
# standard hosted labels. Existing hosted exceptions above are pinned separately.
STANDARD_RUNNERS = ('ubuntu-24.04',)
# Refuse paid labels everywhere, including pinned jobs, metadata and release matrices.
_LARGER_RUNNER_LABEL = re.compile(
    r'(?<![\w.-])(?:pf-linux-(?:x64|arm64)-\d+|[\w.-]+-(?:larger|xlarge|large))(?![\w.-])')
_LARGER_RUNNER_KEY = re.compile(r'(?<![\w.-])["\']?larger_runner["\']?[ \t]*:')
# Retired release labels remain refused by the runner pins as well. Neither
# spelling may grant an arbitrary workflow access to the isolated release pool.
_RELEASE_POOL_LABEL = re.compile(r'(?<![\w.-])(?:pf-ci-release|pf-gcp-main)(?![\w.-])')
# A docs-only change (scripts/ci_docs_only.py) skips the proof jobs. Each waits
# for authz and changes and skips only when changes succeeded and answered
# docs-only -- exactly the skip the build aggregate accepts -- so a failed or
# unanswered classification runs it.
PROOF_JOBS = {'build': 'BUILD_RESULT', 'build-arm': 'ARM_BUILD_RESULT',
              'build-debug': 'DEBUG_BUILD_RESULT',
              'build-release-gate': 'RELEASE_GATE_RESULT', 'build-debug-gate': 'DEBUG_GATE_RESULT',
              'sanitizers': 'SANITIZER_RESULT',
              'sanitizers-gate': 'SANITIZER_GATE_RESULT', 'kernel-only-gate': 'KERNEL_GATE_RESULT',
              'kernel-only': 'KERNEL_RESULT', 'native-live': 'NATIVE_RESULT',
              'corpus-parity-subset': 'PARITY_RESULT', 'corpus-parity': 'FULL_PARITY_RESULT'}
PR_PROOF_JOBS = {'build': 'BUILD_RESULT', 'corpus-parity-subset': 'PARITY_RESULT'}
HEAVY_JOBS = frozenset(('sanitizers', 'kernel-only', 'native-live', 'corpus-parity'))
AUTHZ_SUCCESS = "${{ needs.authz.result == 'success' }}"
AUTHZ_ALWAYS = "${{ always() && needs.authz.result == 'success' }}"
PREFLIGHT_IF = "${{ !github.event.pull_request.draft && (always() && needs.authz.result == 'success') }}"
AUTHZ_FULL_SUCCESS = "${{ needs.authz.result == 'success' && github.event_name != 'pull_request' }}"
DOCS_ONLY_SKIP = ("${{ always() && needs.authz.result == 'success' && (needs.changes.result != 'success' || "
                  "needs.changes.outputs.docs_only != 'true') }}")
# Only the scheduling hint comes from changes. The aggregate recomputes this
# class from the event-bound API diff inside its own job.
GITHUB_ONLY_SKIP = ("${{ !github.event.pull_request.draft && (always() && needs.authz.result == 'success' && "
                    "(needs.changes.result != 'success' || "
                    "(needs.changes.outputs.docs_only != 'true' && (github.event_name != 'pull_request' || "
                    "needs.changes.outputs.github_only != 'true')))) }}")
HEAVY_SKIP = DOCS_ONLY_SKIP
SLOW_PREFLIGHT_IF = AUTHZ_ALWAYS
BUILD_VERIFY_RUN = ' '.join(r'''|
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPINEFORGE_VERSION_SOURCE=FILE \
-DPython3_EXECUTABLE="$(command -v python3)" -DPINEFORGE_BUILD_TESTS=ON \
-DPINEFORGE_BUILD_SOURCE_LAYER=ON -DPINEFORGE_BUILD_TUTORIAL=OFF \
-DPINEFORGE_BUILD_LIVE_RUNNER=OFF -DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF \
-DPINEFORGE_BUILD_EXAMPLES=OFF -DPINEFORGE_REQUIRE_ABI_RECEIPTS=OFF \
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache
cmake --build build --target pineforge_pr_smoke --parallel "$(getconf _NPROCESSORS_ONLN)"
ctest --test-dir build -L '^fast$' --output-on-failure --no-tests=error \
--parallel "$(getconf _NPROCESSORS_ONLN)" --timeout 0 --output-junit build/ctest-junit.xml
'''.splitlines())
POST_BUILD_VERIFY_RUN = ('| python3 scripts/ci_verify.py '
                         "${{ matrix.build_type == 'Release' && 'release' || 'debug' }} "
                         f'--build-dir build --jobs {CORES} --ccache')
POST_PLATFORM_STEP = '''      - name: Require the requested platform
        if: ${{ needs.authz.result == 'success' }}
        env:
          REQUESTED_PLATFORM: ${{ matrix.platform.os }}
          ACTUAL_OS: ${{ runner.os }}
        run: |
          case "$REQUESTED_PLATFORM:$ACTUAL_OS" in
            ubuntu-24.04:Linux) ;;
            *) echo "::error::runner OS $ACTUAL_OS cannot verify $REQUESTED_PLATFORM"; exit 1 ;;
          esac
'''
POST_ARM_PLATFORM_STEP = POST_PLATFORM_STEP.replace(
    'REQUESTED_PLATFORM: ${{ matrix.platform.os }}', 'REQUESTED_PLATFORM: ubuntu-24.04-arm'
).replace('ubuntu-24.04:Linux', 'ubuntu-24.04-arm:Linux')
BUILD_CACHE_KEY = 'ccache-main-${{ runner.os }}-${{ runner.arch }}-${{ matrix.build_type }}-'
BUILD_CACHE_SAVE = ("${{ success() && github.ref == 'refs/heads/main' "
                    "&& steps.ccache-restore.outputs.cache-hit != 'true' }}")
SHARD_CACHE_SAVE = BUILD_CACHE_SAVE.replace(
    "&& steps.ccache-restore", "&& matrix.shard == 1 && steps.ccache-restore")
POST_SHARD_GATE_STEP = r'''      - name: Require every shard to have passed
        shell: bash
        env:
          SHARD_RESULT: ${{ needs.SHARD_JOB.result }}
        run: |
          set -euo pipefail
          printf '%s\n' "shards: $SHARD_RESULT"
          [[ "$SHARD_RESULT" == "success" ]]
'''
PREFLIGHT_LINTER_RUN = ' '.join(r'''|
set -euo pipefail
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y shellcheck
tools_dir="$RUNNER_TEMP/pineforge-actionlint"
mkdir -p "$tools_dir"
archive="$tools_dir/actionlint.tar.gz"
curl --fail --location --proto '=https' --tlsv1.2 \
"https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz" \
--output "$archive"
echo "${ACTIONLINT_SHA256}  ${archive}" | sha256sum -c -
tar -xzf "$archive" -C "$tools_dir" actionlint
echo "$tools_dir" >> "$GITHUB_PATH"'''.splitlines())
# These stages execute compiler/build controls or need the separately installed
# pytest dependency. The rest are source/documentation/policy checks. A local
# unfiltered invocation retains every stage; CI runs both disjoint tiers.
SLOW_PREFLIGHT_STAGES = frozenset((
    'verifier-tests', 'kernel-residuals-tests', 'unittest-shards-tests', 'run-json-harness-tests',
))
GITHUB_ONLY_PREFLIGHT_STAGES = frozenset((
    'shellcheck-version', 'actionlint-runner-labels', 'workflow-lint',
    'ci-workflow-contract', 'docs-workflow-contract', 'docs-workflow-contract-tests',
))
DOCS_PR_TRIGGER = "  pull_request:\n    paths-ignore: ['.github/**']\n"
FAST_TEST_REGISTRATION = '''foreach(_pf_fast_test IN LISTS PINEFORGE_PR_FAST_TESTS)
    if(PINEFORGE_BUILD_SOURCE_LAYER AND (NOT TEST ${_pf_fast_test} OR NOT TARGET ${_pf_fast_test}))
        message(FATAL_ERROR "PR fast row or executable is missing: ${_pf_fast_test}")
    endif()
    if(TEST ${_pf_fast_test})
        get_test_property(${_pf_fast_test} DISABLED _pf_fast_disabled)
        get_test_property(${_pf_fast_test} SKIP_RETURN_CODE _pf_fast_skip_code)
        get_test_property(${_pf_fast_test} SKIP_REGULAR_EXPRESSION _pf_fast_skip_regex)
        get_test_property(${_pf_fast_test} LABELS _pf_fast_labels)
        if(_pf_fast_disabled OR NOT _pf_fast_skip_code STREQUAL "NOTFOUND"
                OR NOT _pf_fast_skip_regex STREQUAL "NOTFOUND"
                OR "slow" IN_LIST _pf_fast_labels OR "private_fixtures" IN_LIST _pf_fast_labels)
            message(FATAL_ERROR "PR fast row must execute without skips: ${_pf_fast_test}")
        endif()
        set_property(TEST ${_pf_fast_test} APPEND PROPERTY LABELS fast)
    endif()
endforeach()
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_custom_target(pineforge_pr_smoke DEPENDS ${PINEFORGE_PR_FAST_TESTS})
endif()
'''
# The complete canon step, including its env, comments and shell, byte for byte.
# PR-only ALLOWED_BOTS is dependabot[bot] and pineforge-app[bot]. No local canon
# file is needed on CI runners. Changing the script requires a new canon pin.
AUTHZ_STEP_SHA256 = '94bf3afefe4588bc33dad5fd4d8fa3d2a104cc45743ad59286c756fa3a17baa6'
# Scoped CI workflows bind this canon header byte for byte, including its
# fleet comment and comma spacing. The release probe pins executable lines.
AUTHZ_JOB_HEADER = '''    name: authz
    # Fleet routing is fixed for this workflow; releases use pf-gcp-main.
    runs-on: ubuntu-24.04
    permissions: {}
'''
AUTHZ_REFUSE_STEP = '''      - name: Refuse an unauthorised run before anything else
        if: ${{ needs.authz.result != 'success' }}
        run: |
          echo "::error::authz did not pass; nothing of this run's code is checked out or run"
          exit 1
'''
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
PREFLIGHT_TIER = ("${{ github.event_name == 'pull_request' && needs.changes.result == 'success' && "
                  "needs.changes.outputs.github_only == 'true' && 'github-only' || 'fast' }}")
# This is the only non-authz token reader allowed by the CI contract. The same
# step schedules jobs in changes and independently proves skips in build-gate.
# It executes inline without checkout, validates both rename paths, every page
# and the live head/base/count before and after reading the PR diff.
GITHUB_ONLY_STEP = r'''      - name: Read the PR diff independently
        id: github-only
        shell: python3 {0}
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          import json
          import os
          from pathlib import Path
          import re
          from urllib.request import Request, urlopen

          github_only = False
          if os.environ["GITHUB_EVENT_NAME"] == "pull_request":
              event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
              pr = event["pull_request"]
              number = event["number"]
              repo = os.environ["GITHUB_REPOSITORY"]
              head, base = pr["head"]["sha"], pr["base"]["sha"]
              if (type(number) is not int or number <= 0 or pr["number"] != number
                      or event["repository"]["full_name"] != repo
                      or not re.fullmatch(r"[0-9a-f]{40}", head)
                      or not re.fullmatch(r"[0-9a-f]{40}", base)):
                  raise ValueError("missing or invalid PR identity")
              endpoint = os.environ["GITHUB_API_URL"].rstrip("/") + f"/repos/{repo}/pulls/{number}"

              def api(url):
                  request = Request(url, headers={
                      "Authorization": "Bearer " + os.environ["GH_TOKEN"],
                      "Accept": "application/vnd.github+json",
                      "X-GitHub-Api-Version": "2022-11-28",
                  })
                  with urlopen(request) as response:
                      return json.load(response)

              def snapshot():
                  current = api(endpoint)
                  if (current["number"] != number or current["head"]["sha"] != head
                          or current["base"]["sha"] != base):
                      raise ValueError("PR changed since this event; rerun CI")
                  count = current["changed_files"]
                  if type(count) is not int or not 0 < count <= 3000:
                      raise ValueError("empty or incomplete PR diff")
                  return count

              def path(value):
                  if (not isinstance(value, str) or not value
                          or any(ord(char) < 32 for char in value) or "\\" in value
                          or any(part in ("", ".", "..") for part in value.split("/"))):
                      raise ValueError("unreadable PR path")
                  return value

              count = snapshot()
              paths, filenames = [], set()
              for page in range(1, (count + 99) // 100 + 1):
                  rows = api(endpoint + f"/files?per_page=100&page={page}")
                  if not isinstance(rows, list) or len(rows) != min(100, count - len(filenames)):
                      raise ValueError("missing or truncated PR files page")
                  for row in rows:
                      filename = path(row["filename"])
                      if filename in filenames or row["status"] not in (
                              "added", "modified", "removed", "renamed", "copied", "changed", "unchanged"):
                          raise ValueError("duplicate or unreadable PR file")
                      filenames.add(filename)
                      paths.append(filename)
                      if row["status"] == "renamed" or "previous_filename" in row:
                          paths.append(path(row["previous_filename"]))
              if len(filenames) != count or snapshot() != count:
                  raise ValueError("PR diff changed while reading files")
              github_only = bool(paths) and all(value.startswith(".github/") for value in paths)
          with open(os.environ["GITHUB_OUTPUT"], "a") as output:
              output.write("github_only=" + str(github_only).lower() + "\n")
'''
_CONTINUE_ON_ERROR = re.compile(r'\s*(?:-\s+)?["\']?continue-on-error["\']?\s*:')
# SEC846: an independent literal pins the promotion-specific canon variant.
# Only the read scopes needed for API resolution are granted. No checkout,
# secrets, inherited OIDC, job skip or error suppression may be inserted.
# Include the header, comments and separator exactly;
# the codegen and campaign pins must carry this same literal, without repo edits.
PROMOTE_GUARD = r'''  promote-guard:
    name: promote-guard
    runs-on: ubuntu-latest
    # Resolution needs read access in private repos; no secrets or OIDC here.
    permissions:
      pull-requests: read
    outputs:
      ok: ${{ steps.authorize.outputs.ok }}
      merge_sha: ${{ steps.pr.outputs.merge_sha }}
      head_sha: ${{ steps.pr.outputs.head_sha }}
      pr_number: ${{ steps.pr.outputs.pr_number }}
      pr_url: ${{ steps.pr.outputs.pr_url }}
      merged_at: ${{ steps.pr.outputs.merged_at }}
    steps:
      - name: Resolve the merged PR
        id: pr
        env:
          GH_TOKEN: ${{ github.token }}
          EVENT_NAME: ${{ github.event_name }}
          REPO: ${{ github.repository }}
          REF: ${{ github.ref }}
          SHA: ${{ github.sha }}
          INPUT_MERGE_SHA: ${{ github.event.inputs.merge_commit }}
          INPUT_HEAD_SHA: ${{ github.event.inputs.head_sha }}
          INPUT_PR_NUMBER: ${{ github.event.inputs.pr_number }}
          INPUT_PR_URL: ${{ github.event.inputs.pr_url }}
          INPUT_MERGED_AT: ${{ github.event.inputs.merged_at }}
        run: |
          set -euo pipefail
          fail() { echo "::error title=authz::$1"; echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }
          [ "$REF" = refs/heads/main ] || fail "baseline promotion requires refs/heads/main"
          case "$EVENT_NAME" in
            workflow_dispatch)
              for value in "$INPUT_MERGE_SHA" "$INPUT_HEAD_SHA" "$INPUT_PR_NUMBER" "$INPUT_PR_URL" "$INPUT_MERGED_AT"; do
                [[ -n "${value//[[:space:]]/}" && "$value" != *$'\n'* && "$value" != *$'\r'* ]] || fail "empty or unreadable dispatch input"
              done
              printf '%s\n' "merge_sha=$INPUT_MERGE_SHA" "head_sha=$INPUT_HEAD_SHA" \
                "pr_number=$INPUT_PR_NUMBER" "pr_url=$INPUT_PR_URL" \
                "merged_at=$INPUT_MERGED_AT" "ok=true" >> "$GITHUB_OUTPUT"
              exit 0 ;;
            push) ;;
            *) fail "event $EVENT_NAME does not run baseline promotion (only push and workflow_dispatch are allowed)" ;;
          esac
          # Search every page: a second match must defer, never pick a PR.
          pulls=$(gh api --paginate --slurp \
            "repos/$REPO/commits/$SHA/pulls?per_page=100")
          python3 - 3<<<"$pulls" <<'PY'
          import json
          import os
          import re
          import subprocess

          def skip(reason):
              print(f"::notice::{reason}; skipping")
              with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                  output.write("ok=false\n")

          def fail(reason):
              print(f"::error title=authz::{reason}")
              with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                  summary.write(f"authz: REFUSED: {reason}\n")
              raise SystemExit(1)

          def reject_constant(value):
              raise ValueError(f"invalid JSON constant: {value}")

          def field(obj, *keys):
              for key in keys:
                  obj = obj.get(key) if isinstance(obj, dict) else None
              if (not isinstance(obj, str) or not obj.strip()
                      or any(ord(char) < 32 for char in obj)):
                  fail("empty or unreadable PR field: " + ".".join(keys))
              return obj

          sha = os.environ["SHA"]
          if not re.fullmatch(r"[0-9a-f]{40}", sha):
              fail("empty or unreadable pushed SHA")
          try:
              pages = json.loads(open(3, encoding="utf-8").read(), parse_constant=reject_constant)
          except (ValueError, RecursionError):
              skip("invalid merged PR response JSON")
              raise SystemExit(0)
          if (not isinstance(pages, list)
                  or any(not isinstance(page, list) for page in pages)
                  or any(not isinstance(pr, dict) for page in pages for pr in page)):
              skip("invalid merged PR response page array")
              raise SystemExit(0)
          matches = [pr for page in pages for pr in page
                     if pr.get("state") == "closed"
                     and pr.get("merged_at") is not None
                     and pr.get("merge_commit_sha") == sha
                     and isinstance(pr.get("base"), dict)
                     and pr["base"].get("ref") == "main"]
          if len(matches) != 1:
              skip(f"found {len(matches)} merged PRs with merge commit {sha} and base main")
              raise SystemExit(0)
          selected = matches[0]
          number = selected.get("number")
          if type(number) is not int or number <= 0:
              fail("empty or unreadable PR number")
          # The association endpoint returns a summary PR without merged_by.
          # Read that selected PR's full API response, binding it to the same
          # merge, base and head before using any identity or merge metadata.
          try:
              response = subprocess.run(
                  ["gh", "api", f"repos/{os.environ['REPO']}/pulls/{number}"],
                  check=True, capture_output=True, text=True)
              pr = json.loads(response.stdout, parse_constant=reject_constant)
          except (subprocess.CalledProcessError, OSError, ValueError, RecursionError):
              fail("unreadable resolved PR response")
          if (not isinstance(pr, dict) or type(pr.get("number")) is not int
                  or pr.get("number") != number
                  or pr.get("state") != "closed" or pr.get("merged") is not True
                  or pr.get("merge_commit_sha") != sha
                  or field(pr, "base", "ref") != "main"
                  or field(pr, "head", "sha") != field(selected, "head", "sha")
                  or field(pr, "merged_at") != field(selected, "merged_at")):
              fail("resolved PR no longer matches the pushed merge")
          head_sha = field(pr, "head", "sha")
          if not re.fullmatch(r"[0-9a-f]{40}", head_sha):
              fail("empty or unreadable PR head SHA")
          head = pr.get("head")
          repo = head.get("repo") if isinstance(head, dict) else None
          fork = repo.get("fork") if isinstance(repo, dict) else None
          if type(fork) is not bool:
              fail("empty or unreadable PR field: head.repo.fork")
          values = {
              "merge_sha": sha,
              "head_sha": head_sha,
              "pr_number": number,
              "pr_url": field(pr, "html_url"),
              "merged_at": field(pr, "merged_at"),
              "head_repo": field(pr, "head", "repo", "full_name"),
              "head_is_fork": str(fork).lower(),
              "association": field(pr, "author_association"),
              "pr_author": field(pr, "user", "login"),
              "merged_by": field(pr, "merged_by", "login"),
              "ok": "true",
          }
          with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
              for key, value in values.items():
                  output.write(f"{key}={value}\n")
          PY

      - name: Only our own merged pull requests and dispatches promote the baseline
        id: authorize
        if: steps.pr.outputs.ok == 'true'
        env:
          EVENT: ${{ github.event_name }}
          REPO: ${{ github.repository }}
          HEAD_REPO: ${{ steps.pr.outputs.head_repo }}
          HEAD_IS_FORK: ${{ steps.pr.outputs.head_is_fork }}
          ASSOCIATION: ${{ steps.pr.outputs.association }}
          PR_AUTHOR: ${{ steps.pr.outputs.pr_author }}
          MERGED_BY: ${{ steps.pr.outputs.merged_by }}
          GH_TOKEN: ${{ github.token }}   # read-only use: permission lookups for the PR author and merger
          # Bot logins whose same-repository pull requests count as ours: dependabot[bot] (CI-DEPBOT A)
          # and the org's own GitHub App bot logins by name (space-separated), nothing else.
          ALLOWED_BOTS: "dependabot[bot] pineforge-app[bot]"
        run: |
          set -euo pipefail
          fail() { echo "::error title=authz::$1"; echo "authz: REFUSED: $1" >> "$GITHUB_STEP_SUMMARY"; exit 1; }
          case "$EVENT" in
            workflow_dispatch)
              # Only someone with write access to this repository (or our App's token) can cause these
              # events; none of them runs pull-request code.
              ;;
            push)
              [ -n "$REPO" ] || fail "repository is empty"
              [ "$HEAD_REPO" = "$REPO" ] || fail "pull request from another repository (${HEAD_REPO:-unknown})"
              [ "$HEAD_IS_FORK" = "false" ] || fail "pull request from a fork"
              [ -n "$ASSOCIATION" ] || fail "pull request author association is empty"
              [ -n "$PR_AUTHOR" ] || fail "pull request author is empty"
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
              esac
              [ -n "$MERGED_BY" ] || fail "push merger is empty"
              case " $ALLOWED_BOTS " in
                *" $MERGED_BY "*) ;;
                *)
                  # No author_association exists for the merger: ask GitHub for the merger's permission on
                  # this repository; anything but admin/maintain/write (or a failed call) refuses.
                  perm=$(gh api "repos/$REPO/collaborators/$MERGED_BY/permission" --jq .permission 2>/dev/null || echo none)
                  case "$perm" in admin|maintain|write) ;; *) fail "push merger ${MERGED_BY:-unknown} has permission '$perm', not write" ;; esac ;;
              esac ;;
            *) fail "event $EVENT does not run baseline promotion (only push and workflow_dispatch are allowed)" ;;
          esac
          echo "authz: allowed ($EVENT${PR_AUTHOR:+ by $PR_AUTHOR})" >> "$GITHUB_STEP_SUMMARY"
          echo "ok=true" >> "$GITHUB_OUTPUT"

'''
PROMOTE_IF = ("${{ needs.promote-guard.result == 'success' && "
              "needs.promote-guard.outputs.ok == 'true' }}")
GATE_NEEDS = 'needs: [authz, changes, preflight, build, corpus-parity-subset]'
POST_GATE_NEEDS = ('needs: [authz, changes, preflight, build, build-arm, sanitizers, native-live, kernel-only, '
                   'corpus-parity-subset, preflight-slow, corpus-parity]')
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
          GITHUB_ONLY: ${{ steps.github-only.outputs.github_only }}
          PREFLIGHT_RESULT: ${{ needs.preflight.result }}
          BUILD_RESULT: ${{ needs.build.result }}
          PARITY_RESULT: ${{ needs.corpus-parity-subset.result }}
        run: |
          printf '%s\n' "authz: $AUTHZ_RESULT" "changes: $CHANGES_RESULT, docs only: ${DOCS_ONLY:-unanswered}" \
            "preflight: $PREFLIGHT_RESULT" "matrix: $BUILD_RESULT" \
            "corpus parity (subset): $PARITY_RESULT"
          # Every selected PR proof must pass.
          # GitHub-only skips use this job's own API read, never changes' output.
          # Classified docs-only skips retain their policy; preflight must pass.
          docs_only=false
          if [[ "$CHANGES_RESULT" == "success" && "$DOCS_ONLY" == "true" ]]; then docs_only=true; fi
          github_only=false
          if [[ "$GITHUB_EVENT_NAME" == "pull_request" && "${GITHUB_ONLY:-false}" == "true" ]]; then github_only=true; fi
          passed() { [[ "$1" == "success" || ( ( "$docs_only" == "true" || "$github_only" == "true" ) && "$1" == "skipped" ) ]]; }
          [[ "$AUTHZ_RESULT" == "success" && "$CHANGES_RESULT" == "success" ]] \
            && [[ "$PREFLIGHT_RESULT" == "success" ]] && passed "$BUILD_RESULT" \
            && passed "$PARITY_RESULT"
'''
POST_GATE_STEP = r'''      - name: Require every verification lane to have passed
        shell: bash
        env:
          AUTHZ_RESULT: ${{ needs.authz.result }}
          CHANGES_RESULT: ${{ needs.changes.result }}
          DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}
          PREFLIGHT_RESULT: ${{ needs.preflight.result }}
          BUILD_RESULT: ${{ needs.build.result }}
          DEBUG_BUILD_RESULT: ${{ needs.build-debug.result }}
          RELEASE_GATE_RESULT: ${{ needs.build-release-gate.result }}
          DEBUG_GATE_RESULT: ${{ needs.build-debug-gate.result }}
          ARM_BUILD_RESULT: ${{ needs.build-arm.result }}
          SANITIZER_RESULT: ${{ needs.sanitizers.result }}
          SANITIZER_GATE_RESULT: ${{ needs.sanitizers-gate.result }}
          NATIVE_RESULT: ${{ needs.native-live.result }}
          KERNEL_RESULT: ${{ needs.kernel-only.result }}
          KERNEL_GATE_RESULT: ${{ needs.kernel-only-gate.result }}
          PARITY_RESULT: ${{ needs.corpus-parity-subset.result }}
          SLOW_PREFLIGHT_RESULT: ${{ needs.preflight-slow.result }}
          FULL_PARITY_RESULT: ${{ needs.corpus-parity.result }}
        run: |
          printf '%s\n' "authz: $AUTHZ_RESULT" "changes: $CHANGES_RESULT, docs only: ${DOCS_ONLY:-unanswered}" \
            "preflight: $PREFLIGHT_RESULT" "matrix: $BUILD_RESULT" \
            "debug shards: $DEBUG_BUILD_RESULT" "release aggregate: $RELEASE_GATE_RESULT" "debug aggregate: $DEBUG_GATE_RESULT" \
            "arm64 matrix: $ARM_BUILD_RESULT" \
            "sanitizers: $SANITIZER_RESULT" "native: $NATIVE_RESULT" \
            "sanitizers aggregate: $SANITIZER_GATE_RESULT" "kernel aggregate: $KERNEL_GATE_RESULT" \
            "kernel: $KERNEL_RESULT" "corpus parity (subset): $PARITY_RESULT" \
            "preflight (slow): $SLOW_PREFLIGHT_RESULT" "corpus parity (full): $FULL_PARITY_RESULT"
          # Every shard matrix, leg aggregate and full-event proof must pass.
          # Only a classified docs-only push may skip proof jobs.
          # Both preflight tiers must pass even for a docs-only push.
          docs_only=false
          if [[ "$CHANGES_RESULT" == "success" && "$DOCS_ONLY" == "true" ]]; then docs_only=true; fi
          passed() { [[ "$1" == "success" || ( "$docs_only" == "true" && "$1" == "skipped" ) ]]; }
          [[ "$AUTHZ_RESULT" == "success" && "$CHANGES_RESULT" == "success" ]] \
            && [[ "$PREFLIGHT_RESULT" == "success" ]] && passed "$BUILD_RESULT" \
            && passed "$DEBUG_BUILD_RESULT" && passed "$RELEASE_GATE_RESULT" && passed "$DEBUG_GATE_RESULT" \
            && passed "$ARM_BUILD_RESULT" \
            && passed "$SANITIZER_RESULT" && passed "$NATIVE_RESULT" \
            && passed "$SANITIZER_GATE_RESULT" && passed "$KERNEL_GATE_RESULT" \
            && passed "$KERNEL_RESULT" && passed "$PARITY_RESULT" \
            && [[ "$SLOW_PREFLIGHT_RESULT" == "success" ]] && passed "$FULL_PARITY_RESULT"
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
# --jobs and every abbreviation argparse accepts for it in ci_verify.py, and a
# call of ci_verify itself, as a script or a module.
_JOBS_FLAG = re.compile(r'(?<![\w-])--j(?:o|ob|obs)?(?=[\s=]|$)')
_VERIFY_CALL = re.compile(r'\bci_verify\.py\b|-m\s+ci_verify\b')
_LITERAL_CACHE_HASH = re.compile(
    r'''\$\{\{\s*hashFiles\(\s*(?:'[^'\n]*'|"[^"\n]*")'''
    r'''(?:\s*,\s*(?:'[^'\n]*'|"[^"\n]*"))*\s*\)\s*\}\}-''')
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


ACTION_PIN_SHA = re.compile(r'\A[^@\s]+@[0-9a-f]{40}\Z')
ACTION_PIN_COMMENT = re.compile(
    r'\A#[ \t]+v[0-9]+(?:\.[0-9]+)*'
    r'(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?'
    r'(?:\+[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?[ \t]*\Z')
ACTION_PIN_EXEMPT_PREFIXES = ('./', 'docker://')
# This is a whitelist for our reviewed workflow files, not a YAML parser.
# Only plain block keys, scalar values, block scalars and scalar sequences
# are supported; literal {} / [] are allowed as a whole plain-key value.
_ACTION_YAML_KEY = re.compile(r'([A-Za-z_][A-Za-z0-9_-]*) *:(?: +(.*)|$)')
_ACTION_YAML_QUOTED = re.compile(r'"(?:\\.|[^"\\])*"' r"|'(?:''|[^'])*'")
_ACTION_BLOCK_SCALAR = re.compile(r'[|>](?:[+-][1-9]?|[1-9][+-]?)?')
_ACTION_EMPTY_COLLECTION = re.compile(r'\{[ \t]*\}|\[[ \t]*\]')
_ACTION_YAML_FORBIDDEN_CHARACTER = re.compile(
    r'\r(?!\n)|[\x00-\x08\x0b\x0c\x0e-\x1f\x85\u2028\u2029]')
_ACTION_YAML_PROPERTIES = {'&': 'anchors', '*': 'aliases', '!': 'tags',
                          '?': 'explicit keys', ':': 'explicit keys'}


def _action_yaml_visible(value: str, *,
                         sequence: list[str] | None = None) -> tuple[str, str, list[str]]:
    """Return code, masked code, quoted tokens; expressions/comments are opaque."""
    visible, quoted, index = [], [], 0
    depth = len(sequence) if sequence else 0
    flow = bool(depth) or value.startswith('[')
    element_start = bool(sequence and sequence[-1] in ('empty', 'comma'))
    while index < len(value):
        char = value[index]
        if char == '#' and (index == 0 or value[index - 1] == ' '):
            return value[:index].rstrip(' '), ''.join(visible), quoted
        if value.startswith('${{', index):
            end, in_string = index + 3, False
            # Every iteration advances; doubled quotes cannot be repartitioned
            # into adjacent strings when an expression has no closing }}.
            while end < len(value):
                if value[end] == "'":
                    if in_string and value.startswith("''", end):
                        end += 2
                        continue
                    in_string = not in_string
                elif not in_string and value.startswith('}}', end):
                    end += 2
                    break
                end += 1
            else:
                visible.append(value[index:])
                break
            token = value[index:end]
        elif char in ('"', "'") and (not flow or depth and element_start):
            end = index + 1
            while end < len(value):
                if char == '"' and value[end] == '\\':
                    end += 2
                    continue
                if value[end] == char:
                    if char == "'" and value.startswith("''", end):
                        end += 2
                        continue
                    end += 1
                    break
                end += 1
            else:
                # Keep the opening quote visible so the grammar refuses it.
                visible.append(value[index:])
                break
            token = value[index:end]
            quoted.append(token)
        else:
            visible.append(char)
            if flow:
                if char == '[':
                    depth += 1
                    element_start = True
                elif char == ']':
                    depth = max(0, depth - 1)
                    element_start = False
                elif char == ',':
                    element_start = True
                elif char != ' ':
                    # An embedded quote remains data, never a masking token.
                    element_start = False
            index += 1
            continue
        # Retain positions and a nonblank scalar placeholder, including for "".
        visible.append('v' + ' ' * (len(token) - 1))
        element_start = False
        index = end
    return value.rstrip(' '), ''.join(visible), quoted


def _action_yaml_value(value: str, *, whole_key: bool, sequence: list[str]) -> str | None:
    """Accept a scalar or scalar flow sequence; return a construct to refuse."""
    code, visible, quoted = _action_yaml_visible(value, sequence=sequence)
    if not code:
        return None
    if not sequence:
        if code[0] in _ACTION_YAML_PROPERTIES:
            return _ACTION_YAML_PROPERTIES[code[0]]
        if whole_key and _ACTION_EMPTY_COLLECTION.fullmatch(code):
            return None
        if code.startswith('{'):
            return 'flow mappings'
        if code.startswith(('"', "'")):
            if not _ACTION_YAML_QUOTED.fullmatch(code):
                return 'quoted scalars'
            return 'quoted escapes' if '\\' in code else None
        if _ACTION_BLOCK_SCALAR.fullmatch(code):
            return None
        if not code.startswith('['):
            # Quotes embedded in a plain scalar are data (e.g. shell printf),
            # while a colon separator would introduce another mapping key.
            if re.search(r':(?:[ \t]|$)', visible):
                return 'inline mapping keys'
            if code[0] in ']}|>%@`,' or re.match(r'-(?:[ \t]|$)', code):
                return 'block structure'
            return 'expressions' if '${{' in visible else None
    # Flow sequences may span lines, but their items must remain scalars or
    # nonempty sequences: even an implicit mapping without braces is refused.
    if re.search(r'[{}]|:(?=[ \t,\]}]|$)', visible):
        return 'flow mappings'
    property_token = re.search(r'(?:^|[ \t,\[])([&*!?:])', visible)
    if property_token:
        return _ACTION_YAML_PROPERTIES[property_token.group(1)]
    if any('\\' in token for token in quoted):
        return 'quoted escapes'
    if re.search(r'''["']''', visible):
        return 'quoted scalars'
    if '${{' in visible:
        return 'expressions'
    closed = False
    for char in visible:
        if char in ' \t':
            continue
        if closed:
            return 'flow sequences'
        if char == '[':
            if sequence:
                if sequence[-1] in ('value', 'closed'):
                    return 'flow sequences'
                sequence[-1] = 'value'
            sequence.append('empty')
        elif char == ']':
            if not sequence:
                return 'flow sequences'
            if sequence.pop() == 'empty':
                return 'empty collections'
            if sequence:
                sequence[-1] = 'closed'
            else:
                closed = True
        elif char == ',':
            if not sequence or sequence[-1] not in ('value', 'closed'):
                return 'flow sequences'
            sequence[-1] = 'comma'
        else:
            if not sequence or sequence[-1] == 'closed':
                return 'flow sequences'
            if sequence[-1] != 'value' and char in '|>%@`':
                return 'flow sequences'
            sequence[-1] = 'value'
    return None


def action_uses(sources: dict[str, str], *,
                findings: list[str]) -> Iterator[tuple[str, int, str, str]]:
    """Whitelist workflow YAML, then yield plain block uses scalars for pinning."""
    for name, text in sources.items():
        # Refuse the raw file before any scalar-body, comment or flow skip.
        # LF and CRLF are the only supported line endings, including in data.
        offense = _ACTION_YAML_FORBIDDEN_CHARACTER.search(text)
        if offense is not None:
            char = offense.group()
            number = text.count('\n', 0, offense.start()) + 1
            source_line = text.replace('\r\n', '\n').split('\n')[number - 1].strip(' \t')
            construct = ('unsupported line break' if char in '\r\x85\u2028\u2029\v\f'
                         else 'unsupported control character')
            findings.append(
                f'{name}:{number}: unsupported YAML construct in a workflow '
                f'({construct} U+{ord(char):04X}): '
                f'rewrite it in plain block style; {source_line!r}')
            continue
        scalar_depth = None
        sequence = []
        sequence_start = None
        refused_flow_depth = 0
        refused_flow_indent = 0
        for number, line in enumerate(text.replace('\r\n', '\n').split('\n'), 1):
            if scalar_depth is not None:
                if not line.strip(' ') or _indent(line) > scalar_depth:
                    continue
                scalar_depth = None
            unsupported_whitespace = any(
                char == '\ufeff' or char not in ' \t' and
                unicodedata.category(char) in {'Zs', 'Zl', 'Zp', 'Cc'}
                for char in line)
            if not unsupported_whitespace and (
                    not line.strip(' ') or line.lstrip(' ').startswith('#')):
                continue
            if refused_flow_depth:
                if _indent(line) <= refused_flow_indent:
                    refused_flow_depth = 0
                    # An aligned closing tail belongs to the refused flow;
                    # any other outdent is scanned again as a block line.
                    if line.lstrip(' ').startswith(('}', ']')):
                        continue
                else:
                    # This collection is already refused. Quotes are plain
                    # text here: they must never mask a closing delimiter.
                    for char in line:
                        if char in '{[':
                            refused_flow_depth += 1
                        elif char in '}]':
                            refused_flow_depth -= 1
                        if not refused_flow_depth:
                            break
                    continue
            stripped = line.strip(' ')
            step = re.match(r'-[ \t]+', stripped)
            key_line = stripped[step.end():] if step and not sequence else stripped
            key = _ACTION_YAML_KEY.fullmatch(key_line) if not sequence else None
            first = (key.group(2) or '').strip(' ') if key else None
            node = first if first is not None else key_line
            code, visible, _ = _action_yaml_visible(node, sequence=sequence)
            sequence_depth = len(sequence)
            # Recover only the extent of a refused property-prefixed block,
            # so an anchored script produces its anchor finding, not script
            # uses findings. This never authorizes the property itself.
            recovery = re.sub(r'^(?:[&*!][^ \t]+[ \t]*)+', '', code)
            if not sequence and _ACTION_BLOCK_SCALAR.fullmatch(recovery):
                # A bare "- |" owns its body from the dash column; "- run: |"
                # owns it from the mapping key's column after the dash.
                scalar_depth = _indent(line) + (step.end() if step and key else 0)
            quoted_key = _ACTION_YAML_QUOTED.match(key_line)
            if unsupported_whitespace:
                unsupported = 'unsupported whitespace'
            elif '\t' in re.match(r'[ \t]*', line).group() or (
                    step and '\t' in step.group()):
                unsupported = 'tab indentation'
            elif re.match(r'(?:---|\.\.\.)(?:[ \t]|$)', stripped):
                unsupported = 'document markers'
            elif not sequence and quoted_key and key_line[quoted_key.end():].lstrip(' ').startswith(':'):
                unsupported = 'quoted keys'
            elif not sequence and re.match(r'<<[ \t]*:', key_line):
                unsupported = 'merge keys'
            elif not sequence and not key and not step:
                unsupported = (_ACTION_YAML_PROPERTIES.get(key_line[0])
                               or 'block structure')
            else:
                unsupported = _action_yaml_value(node, whole_key=key is not None,
                                                 sequence=sequence)
                if unsupported is None and key and key.group(1) == 'uses' and (
                        not code or _ACTION_BLOCK_SCALAR.fullmatch(code)
                        or code.startswith(('{', '['))):
                    unsupported = 'uses keys'
            if sequence and not sequence_depth:
                sequence_start = number, line
            if unsupported is not None:
                # The finding refuses the entire unsupported collection;
                # its inner mapping lines cannot become accepted block keys.
                if sequence_depth or recovery.startswith(('{', '[')):
                    refused_flow_depth = max(0, sequence_depth
                                             + sum(1 if char in '{[' else -1
                                                   for char in visible if char in '{}[]'))
                    refused_flow_indent = _indent(sequence_start[1] if sequence_depth else line)
                sequence.clear()
                source_line = line.strip(' \t')
                findings.append(
                    f'{name}:{number}: unsupported YAML construct in a workflow ({unsupported}): '
                    f'rewrite it in plain block style; {source_line!r}')
                continue
            if not key or key.group(1) != 'uses':
                continue
            # Keep quotes separate from the trailing comment: a # inside a
            # quoted scalar is part of the reference, not a version comment.
            scalar = re.fullmatch(
                r"""(?:"([^"]*)"|'([^']*)'|([^\s'"#]+))(?:[ \t]+(#[^\r\n]*))?[ \t]*""",
                first)
            reference = (next(group for group in scalar.groups()[:3] if group is not None)
                         if scalar else first)
            comment = (scalar.group(4) or '') if scalar else ''
            yield name, number, reference, comment
        if sequence:
            number, line = sequence_start
            source_line = line.strip(' \t')
            findings.append(
                f'{name}:{number}: unsupported YAML construct in a workflow (flow sequences): '
                f'rewrite it in plain block style; {source_line!r}')


def action_pin_findings(sources: dict[str, str]) -> list[str]:
    """Refuse YAML outside the supported subset and check every uses scalar."""
    findings = []
    for name, number, reference, comment in action_uses(sources, findings=findings):
        if reference.startswith(ACTION_PIN_EXEMPT_PREFIXES):
            continue
        if ACTION_PIN_SHA.fullmatch(reference) and ACTION_PIN_COMMENT.fullmatch(comment):
            continue
        findings.append(
            f'{name}:{number}: third-party uses {reference!r} must be pinned to a '
            '40-character lowercase hex SHA with a trailing # v<version> comment')
    return findings


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


_PRIVATE_KEY_WORD = re.compile(r'private[-_]?key', re.IGNORECASE)
_SECRET_REFERENCE = re.compile(
    r'\bsecrets\s*(?:\.\s*([A-Za-z_]\w*)|\[([^\]]+)\])', re.IGNORECASE)
_APP_MINT_SCRIPT = re.compile(
    r'\bapp/installations/[^\s]*?/access_tokens\b|@octokit/auth-app'
    r'|\b(?:createAppAuth|GithubIntegration|(?:github\.)?Auth\.AppAuth)\s*\(',
    re.IGNORECASE)


def _literal_environment_name(value: str) -> bool:
    """Accept a nonempty string scalar, never an expression or YAML collection."""
    code, _, _ = _action_yaml_visible(value)
    if _ACTION_YAML_QUOTED.fullmatch(code):
        name = code[1:-1]
    else:
        if (not code or _action_yaml_value(code, whole_key=True, sequence=[]) is not None
                or code.startswith(('{', '[', '|', '>'))
                or code.casefold() in {'null', '~', 'true', 'false', 'yes', 'no', 'on', 'off'}
                or re.fullmatch(
                    r'[-+]?(?:0x[0-9a-f_]+|0o[0-7_]+|0b[01_]+'
                    r'|(?:[0-9][0-9_]*(?:\.[0-9_]*)?|\.[0-9_]+)'
                    r'(?:e[-+]?[0-9_]+)?|\.inf|\.nan)', code, re.IGNORECASE)):
            return False
        name = code
    return bool(name.strip()) and '${{' not in name


def _literal_job_environment(body: str) -> bool:
    values = _job_values(body, 'environment', unquote=False)
    if len(values) != 1:
        return False
    block = _job_block(body, 'environment')
    if block:
        names = _job_values('\n'.join(block[1:]), 'name', unquote=False)
        return len(names) == 1 and _literal_environment_name(names[0])
    return _literal_environment_name(values[0])


def _private_key_handoff(text: str) -> bool:
    """Recognize key-named secrets and secret-fed private-key inputs/arguments."""
    lines = [_action_yaml_visible(line)[0] for line in _code(text)]
    for match in _SECRET_REFERENCE.finditer('\n'.join(lines)):
        if _PRIVATE_KEY_WORD.search(match.group(1) or match.group(2)):
            return True
    for index, line in enumerate(lines):
        if not _PRIVATE_KEY_WORD.search(line):
            continue
        value = [line]
        # A private-key input/env may be a block scalar. Do not let an
        # unrelated sibling's secret turn a non-secret key into a handoff.
        for follow in lines[index + 1:]:
            if _indent(follow) <= _indent(line):
                break
            value.append(follow)
        if _SECRET_REFERENCE.search('\n'.join(value)):
            return True
    return False


def app_token_environment_findings(workflows: dict[str, str]) -> list[str]:
    """Require an environment for every visible App mint/private-key handoff.

    The contract's action-pin lint rejects YAML outside its reviewed block
    subset (including quoted keys and flow mappings); the runner contract
    refuses unreadable job mappings. Neither shape can silently bypass this
    job scan. Script/API signatures are conservative human-review indicators,
    not shell/Python/JavaScript interpretation or transitive action inspection.
    """
    findings = []
    for name, workflow in workflows.items():
        inherited_env = '\n'.join(_job_values(workflow, 'env'))
        for job, body in _jobs(workflow).items():
            references = (reference for _, _, reference, _ in
                          action_uses({name: body}, findings=[]))
            action_mint = any(reference.partition('@')[0].casefold()
                              == 'actions/create-github-app-token'
                              for reference in references)
            text = inherited_env + '\n' + body
            if not (action_mint or _private_key_handoff(text)
                    or _APP_MINT_SCRIPT.search('\n'.join(_code(text)))):
                continue
            if not _literal_job_environment(body):
                findings.append(
                    f'{name} job {job}: App-token minting/private-key use requires a '
                    'literal, non-empty environment; review other minting methods by hand')
    return findings


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


POST_EVENTS = frozenset(('push', 'schedule', 'workflow_dispatch'))


def workflow_event_routes(workflows: dict[str, str]) -> dict[str, set[str]]:
    """Follow local reusable calls to a fixed point; job ifs cannot excuse routing."""
    routes = {name: (_events(workflow) or set()) - {'workflow_call'}
              for name, workflow in workflows.items()}
    edges = [(name, value.removeprefix('./.github/workflows/'))
             for name, workflow in workflows.items()
             for body in _jobs(workflow).values()
             for value in _job_values(body, 'uses')
             if value.startswith('./.github/workflows/')]
    changed = True
    while changed:
        changed = False
        for caller, callee in edges:
            if callee in routes and not routes[caller] <= routes[callee]:
                routes[callee].update(routes[caller])
                changed = True
    return routes


def workflow_fleet_findings(workflows: dict[str, str]) -> list[str]:
    """Pin each event fleet, apart from the fixed hosted and release exceptions."""
    findings = []
    routes = workflow_event_routes(workflows)
    for name, workflow in workflows.items():
        # Per-job release/probe pins are checked by runner_findings; the probe
        # shares release.yml's App credential and uses the post fleet.
        if name in RELEASE_JOB_RUNNERS:
            continue
        events = _events(workflow)
        reachable = routes[name]
        if events is None:
            findings.append(f'{name} must use readable events for fleet routing')
        if 'pull_request' in reachable and reachable & POST_EVENTS:
            findings.append(f'{name} must split pull_request and post-merge events into separate workflows')
        runner = POST_RUNNER if reachable & POST_EVENTS else PR_RUNNER
        event_class = 'post-merge events' if reachable & POST_EVENTS else 'pull_request'
        fixed_hosted = {job: label for job, label in HOSTED_JOB_RUNNERS.get(name, {}).items()
                        if label == 'ubuntu-24.04' or (reachable & POST_EVENTS and 'pull_request' not in reachable)}
        if not reachable:
            findings.append(f'{name} must have a reachable event for fleet routing')
        for job, body in _jobs(workflow).items():
            # runner_findings pins this exact job's runner and metadata.
            # A PR caller disables the arm64/promotion exceptions through calls.
            if job in fixed_hosted:
                continue
            uses = _job_values(body, 'uses')
            if uses:
                for value in uses:
                    callee = value.removeprefix('./.github/workflows/')
                    if (not value.startswith('./.github/workflows/') or callee not in workflows
                            or 'workflow_call' not in (_events(workflows.get(callee, '')) or set())):
                        findings.append(f'{name} job {job} must call a readable local reusable workflow')
                if not _job_values(body, 'runs-on'):
                    continue
            values = _job_values(body, 'runs-on', unquote=False)
            if [_runner_value(value) for value in values] != [_runner_value(runner)]:
                findings.append(f'{name} job {job} must run on {runner} for {event_class}')
    return findings


def _workflow_runner(workflow: str) -> str:
    """The fixed route for a directly triggered, authz-pinned workflow."""
    return POST_RUNNER if (_events(workflow) or set()) & POST_EVENTS else PR_RUNNER


def _authz_header(workflow: str) -> str:
    # Light canon authz uses the same exact hosted image for every event.
    return AUTHZ_JOB_HEADER


_GKE_ROUTE = re.compile(r'(?<![\w.-])gke-[\w.-]+')


def _matrix_rows(body: str) -> list[dict[str, str]] | None:
    """Expand literal axes and include/exclude rows; expressions fail closed."""
    strategy = '\n'.join(_job_block(body, 'strategy')[1:])
    if not strategy:
        return None if _job_values(body, 'strategy') else [{}]
    if not _job_values(strategy, 'matrix'):
        return [{}]
    matrix = _job_block(strategy, 'matrix')[1:]
    if not matrix:
        return None
    axes: dict[str, list[str]] = {}
    includes: list[dict[str, str]] = []
    excludes: list[dict[str, str]] = []
    index = 0
    while index < len(matrix):
        header = re.fullmatch(r'        ([A-Za-z_]\w*):(?: (.*))?', matrix[index])
        if not header:
            return None
        key, value = header.group(1), header.group(2) or ''
        index += 1
        children = []
        while index < len(matrix) and _indent(matrix[index]) > 8:
            children.append(matrix[index])
            index += 1
        if key in axes or key in ('include', 'exclude') and value:
            return None
        if key in ('include', 'exclude'):
            rows = []
            for line in children:
                item = re.fullmatch(r'          - ([A-Za-z_]\w*): ([^{}\[\]]+)', line)
                follow = re.fullmatch(r'            ([A-Za-z_]\w*): ([^{}\[\]]+)', line)
                if item:
                    rows.append({item.group(1): item.group(2).strip().strip('\'"')})
                elif follow and rows and follow.group(1) not in rows[-1]:
                    rows[-1][follow.group(1)] = follow.group(2).strip().strip('\'"')
                else:
                    return None
            if not rows:
                return None
            (includes if key == 'include' else excludes).extend(rows)
        elif value:
            if children or not re.fullmatch(r'\[[^{}\[\]]+\]', value):
                return None
            values = [part.strip().strip('\'"') for part in value[1:-1].split(',')]
            if any(not part or '${{' in part for part in values):
                return None
            axes[key] = values
        else:
            # An object-valued axis (platform.os) is one value per block item.
            values = []
            for line in children:
                item = re.fullmatch(r'          - (.+)', line)
                if item:
                    values.append(item.group(1))
                elif not values or _indent(line) != 12:
                    return None
                else:
                    values[-1] += ' ' + line.strip()
            if not values or any('${{' in part for part in values):
                return None
            axes[key] = values
    originals = [{}] if axes else []
    for key, values in axes.items():
        originals = [dict(row, **{key: value}) for row in originals for value in values]
    originals = [row for row in originals
                 if not any(all(row.get(key) == value for key, value in excluded.items())
                            for excluded in excludes)]
    rows = [row.copy() for row in originals]
    for included in includes:
        matches = [index for index, row in enumerate(originals)
                   if all(key not in axes or row.get(key) == value
                          for key, value in included.items())]
        if matches:
            for index in matches:
                rows[index].update(included)
        else:
            rows.append(included.copy())
    return rows


def _input_condition_skips(body: str, inputs: dict[str, str]) -> bool:
    """Prune only a provably false literal input comparison in an AND condition."""
    conditions = _job_values(body, 'if')
    if len(conditions) != 1:
        return False
    condition = conditions[0].removeprefix('${{').removesuffix('}}').strip()
    match = re.fullmatch(
        r"""inputs\.([A-Za-z_]\w*)\s*(==|!=)\s*['"]([^'"]+)['"](?:\s*&&[^|]+)?""",
        condition)
    if not match or match.group(1) not in inputs:
        return False
    equal = inputs[match.group(1)] == match.group(3)
    return not equal if match.group(2) == '==' else equal


def _gke_cap_findings(workflows: dict[str, str]) -> list[str]:
    """Refuse credentials through GKE calls and count one post CI run's instances."""
    findings = []
    counts: dict[str, int] = {}
    workflow_jobs = {name: _jobs(workflow) for name, workflow in workflows.items()}
    calls = {(name, job): [value.removeprefix('./.github/workflows/')
                          for value in _job_values(body, 'uses')
                          if value.startswith('./.github/workflows/')]
             for name, jobs in workflow_jobs.items() for job, body in jobs.items()}
    gke_workflows = {name for name, jobs in workflow_jobs.items()
                     if any(_GKE_ROUTE.search(' '.join(_job_values(body, 'runs-on')))
                            for body in jobs.values())}
    # Credentials follow all calls, independent of input conditions or matrix rows.
    while True:
        callers = {name for (name, _), callees in calls.items()
                   if any(callee in gke_workflows for callee in callees)}
        if callers <= gke_workflows:
            break
        gke_workflows.update(callers)
    for (name, job), callees in calls.items():
        if any(callee in gke_workflows for callee in callees):
            workflow, body = workflows[name], workflow_jobs[name][job]
            permissions = _job_values(workflow, 'permissions') + _job_values(body, 'permissions')
            if (_holds_credentials(workflow, body)
                    or any(re.search(r'\bid-token\b', value) for value in permissions)):
                findings.append(f'{name} job {job} must not expose credentials on GKE')

    def visit(name: str, inputs: dict[str, str], multiplier: int, stack: tuple[str, ...]) -> None:
        if name in stack:
            findings.append(f'{name} must not recurse through reusable workflows for the GKE cap')
            return
        for job, body in workflow_jobs.get(name, {}).items():
            if _input_condition_skips(body, inputs):
                continue
            runners = ' '.join(_job_values(body, 'runs-on'))
            labels = _GKE_ROUTE.findall(runners)
            labels = [label for label in labels if label != 'gke-runners']
            callees = calls[(name, job)]
            if not labels and not callees:
                continue
            rows = _matrix_rows(body)
            if rows is None:
                findings.append(f'{name} job {job} must use a readable static matrix for the GKE cap')
                continue
            instances = multiplier * len(rows)
            for label in set(labels):
                counts[label] = counts.get(label, 0) + instances
            arguments = '\n'.join(_job_block(body, 'with')[1:])
            keys = re.findall(r'^\s+([A-Za-z_]\w*):', arguments, re.MULTILINE)
            passed = {key: values[0] for key in keys
                      if len(values := _job_values(arguments, key)) == 1
                      and '${{' not in values[0]}
            for callee in callees:
                visit(callee, passed, instances, (*stack, name))

    if 'ci-post.yml' in workflows:
        visit('ci-post.yml', {}, 1, ())
    findings.extend(f'ci-post.yml GKE label {label} expands to {count} jobs; cap is 8'
                    for label, count in sorted(counts.items()) if count > 8)
    return findings


def runner_findings(workflows: dict[str, str]) -> list[str]:
    """Enforce event-derived fleets, isolated releases and command/core pins."""
    findings = workflow_fleet_findings(workflows) + _gke_cap_findings(workflows)
    routes = workflow_event_routes(workflows)
    for name, workflow in workflows.items():
        code = '\n'.join(re.sub(r'\s+#.*$', '', line) for line in _code(workflow))
        if 'pull_request' in routes[name] and _GKE_ROUTE.search(code):
            findings.append(f'{name} is reachable from pull_request and must not contain GKE routing')
        if _LARGER_RUNNER_KEY.search(code):
            findings.append(f'{name} must not define a larger_runner key; use the GCP fleet or hosted exceptions')
        timed = JOB_COMMANDS.get(name, {})
        fixed_hosted = {job: label for job, label in HOSTED_JOB_RUNNERS.get(name, {}).items()
                        if label == 'ubuntu-24.04' or (routes[name] & POST_EVENTS and 'pull_request' not in routes[name])}
        if _LARGER_RUNNER_LABEL.search(code):
            findings.append(f'{name} must not use larger-runner labels; use the GCP fleet or hosted exceptions')
        fixed_release = RELEASE_JOB_RUNNERS.get(name, {})
        event_runner = POST_RUNNER if routes[name] & POST_EVENTS else PR_RUNNER
        pinned = {job: event_runner for job in timed}
        pinned.update(fixed_hosted)
        pinned.update(fixed_release)
        if (fixed_release and name != 'promote-baseline.yml'
                and _events(workflow) != {'workflow_dispatch'}):
            findings.append(f'{name} must retain workflow_dispatch only for pf-gcp-main jobs')
        if (name == 'promote-baseline.yml'
                and _events(workflow) != {'push', 'workflow_dispatch'}):
            findings.append(f'{name} must retain push and workflow_dispatch '
                            'only for pf-gcp-main promotion')
        if name == 'promote-baseline.yml':
            on_body = '\n'.join(_job_block(workflow, 'on')[1:])
            if _job_values(on_body, 'push') != ['branches: [main]']:
                findings.append(f'{name} must restrict push to exactly branches: [main]')
        release_events = ({'push', 'workflow_dispatch'}
                          if name == 'promote-baseline.yml' else {'workflow_dispatch'})
        if fixed_release and not routes[name] <= release_events:
            findings.append(f'{name} must not admit untrusted caller events to isolated release jobs')
        if _unparsed(workflow):
            findings.append(f'{name} has jobs lines the runner contract cannot read')
        jobs = _jobs(workflow)
        findings += [f'{name} is missing job {job}' for job in pinned if job not in jobs]
        for job, body in jobs.items():
            runs_on, uses = _job_values(body, 'runs-on'), _job_values(body, 'uses')
            gke_pin = GKE_JOB_RUNNERS.get(name, {}).get(job)
            has_gke = bool(_GKE_ROUTE.search(' '.join(runs_on)))
            if has_gke and runs_on != [gke_pin]:
                findings.append(f'{name} job {job} may use GKE routing only at its exact pinned job and label')
            if has_gke and _job_values(body, 'container'):
                findings.append(f'{name} job {job} must not declare a job container on GKE')
            if has_gke and _job_values(body, 'services'):
                findings.append(f'{name} job {job} must not declare job services on GKE')
            if has_gke:
                permissions = _job_values(workflow, 'permissions') + _job_values(body, 'permissions')
                if (_holds_credentials(workflow, body)
                        or any(re.search(r'\bid-token\b', value) for value in permissions)):
                    findings.append(f'{name} job {job} must not expose credentials on GKE')
            runner_text = ' '.join(runs_on + _job_block(body, 'strategy'))
            if _RELEASE_POOL_LABEL.search(runner_text) and job not in fixed_release:
                # No flat-peak.yml exception: event reachability alone cannot
                # establish a release-only caller, and no such job is pinned.
                findings.append(f'{name} job {job} may use release pool labels '
                                'only in a pinned isolated release job')
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
            hosted_platform = (len(runs_on) == 1 and
                               bool(re.fullmatch(r'ubuntu-(?:\d{2}\.\d{2}|latest)-arm', runs_on[0])))
            if job in fixed_release:
                # A quoted JSON scalar does not select a YAML label array.
                actual = _job_values(body, 'runs-on', unquote=False)
                expected = [fixed_release[job]]
                if fixed_release[job] == PR_RUNNER:
                    # Normalize the runner array here; the probe workflow pin below
                    # compares nonblank lines after removing comment-only lines.
                    actual = [_runner_value(value) for value in actual]
                    expected = [_runner_value(value) for value in expected]
                if actual != expected:
                    suffix = {PR_RUNNER: '', POST_RUNNER: ' (post fleet)', 'ubuntu-24.04': ' (hosted)'}.get(
                        fixed_release[job], ' (isolated release)')
                    findings.append(f'{name} job {job} must run on {fixed_release[job]}{suffix}')
                environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                    findings.append(f'{name} job {job} must not set PF_CI_LABELS '
                                    'on an isolated release job')
            elif gke_pin and job in fixed_hosted:
                if _job_values(body, 'runs-on', unquote=False) != [gke_pin]:
                    findings.append(f'{name} job {job} must run on {gke_pin}')
                if credentials:
                    findings.append(f'{name} job {job} must keep production credentials '
                                    'on a hosted standard runner')
                if (name == 'native-live.yml'
                        or name == 'ci-post.yml' and job not in ('preflight', 'preflight-slow')):
                    environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                    if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                        findings.append(f'{name} job {job} must not set PF_CI_LABELS on a GKE job')
            elif fixed_hosted.get(job) == POST_RUNNER:
                actual = _job_values(body, 'runs-on', unquote=False)
                if [_runner_value(value) for value in actual] != [_runner_value(POST_RUNNER)]:
                    findings.append(f'{name} job {job} must run on {POST_RUNNER}')
                if credentials:
                    findings.append(f'{name} job {job} must keep production credentials '
                                    'on a hosted standard runner')
                environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                    findings.append(f'{name} job {job} must not set PF_CI_LABELS '
                                    'on a post fleet job')
            elif (job in fixed_hosted
                  or (credentials and (name, job) != ('docs-post.yml', 'build'))
                  or (job not in pinned and hosted_platform)):
                # Exact hosted pins retain their credential/metadata checks.
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
                environments = ([] if name in ('ci.yml', 'ci-post.yml') and job in ('changes', 'build-gate')
                                else _job_values(workflow, 'env') + _job_values(body, 'env'))
                if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                    findings.append(f'{name} job {job} must not set PF_CI_LABELS on a hosted job')
            else:
                runner = pinned.get(job, POST_RUNNER if routes[name] & POST_EVENTS else PR_RUNNER)
                expected_runner = _runner_value(' '.join(runner.split()))
                # A quoted JSON-looking scalar is a runner label, not a YAML
                # array. Only the matrix's explicit fromJSON decodes a string.
                actual_runners = (_job_values(body, 'runs-on', unquote=False)
                                  if isinstance(expected_runner, list) else runs_on)
                if [_runner_value(value) for value in actual_runners] != [expected_runner]:
                    findings.append(f'{name} job {job} must run on {runner}')
                # Keep docs' existing ban on routing metadata after moving
                # its deployment from hosted runners to its pinned GCP job.
                if name in ('docs.yml', 'docs-post.yml') and job == 'build':
                    environments = _job_values(workflow, 'env') + _job_values(body, 'env')
                    if any(re.search(r'\bPF_CI_LABELS["\']?\s*:', value) for value in environments):
                        findings.append(f'{name} job {job} must not set PF_CI_LABELS '
                                        'on a gated docs job')
                # Legacy PF_CI_LABELS metadata never selects the GCP fleet.
                # Event reachability and literal runs-on arrays bind routing.
            if job not in timed:
                continue
            command = timed[job]
            if (_job_values(body, 'timeout-minutes')
                    or any(_step_values(step, 'timeout-minutes')
                           or _key_value(step.splitlines()[0].lstrip()[2:],
                                         'timeout-minutes') is not None
                           for step in _steps(body))):
                findings.append(f'{name} job {job} must declare no timeout-minutes (owner #827)')
            # Keep parallelism tied to the runner's available cores.
            if not _sized(_counted_text(body), command):
                findings.append(f'{name} job {job} must size its parallelism to the runner')
    for name, job, strategy, build_name, population in (
            ('ci.yml', 'build', BUILD_STRATEGY, BUILD_NAME, 'only Ubuntu Release on pull_request'),
            ('ci-post.yml', 'build', POST_BUILD_STRATEGY, post_build_name(4),
             'only Ubuntu Release shards on full events'),
            ('ci-post.yml', 'build-debug', POST_DEBUG_BUILD_STRATEGY, post_build_name(2),
             'only Ubuntu Debug shards on full events'),
            ('ci-post.yml', 'build-arm', POST_ARM_BUILD_STRATEGY, POST_ARM_BUILD_NAME,
             'only Linux arm64 Release/Debug cells on full events')):
        if name not in workflows:
            continue
        build = _jobs(workflows[name]).get(job, '')
        if _job_block(build, 'strategy') != _code(strategy):
            findings.append(f'{name} {job} must select {population}')
        if _job_values(build, 'name') != [build_name]:
            findings.append(f'{name} {job} legs must keep their names: {build_name}')
    if 'ci-post.yml' in workflows:
        jobs = _jobs(workflows['ci-post.yml'])
        for job, count in POST_SHARDS.items():
            body = jobs.get(job, '')
            strategy = (POST_BUILD_STRATEGY if job == 'build' else
                        POST_DEBUG_BUILD_STRATEGY if job == 'build-debug' else
                        '    strategy:\n      fail-fast: false\n      matrix:\n'
                        '        shard: [' + ', '.join(str(i) for i in range(1, count + 1)) + ']\n')
            if _job_block(body, 'strategy') != _code(strategy):
                findings.append(f'ci-post.yml {job} must select shards 1..{count} '
                                f'for --ctest-shard denominator {count}')
            flag = '--ctest-shard ${{ matrix.shard }}/' + str(count)
            commands = [value for step in _steps(body) for value in _verify_step_commands(step)
                        if _VERIFY_CALL.search(value)]
            if (len(commands) != 1 or _counted_text(body).count('--ctest-shard') != 1
                    or not re.search(re.escape(flag) + r'(?=\s|$)', commands[0])):
                findings.append(f'ci-post.yml {job} must pass {flag} exactly once to ci_verify')
            if job in ('sanitizers', 'kernel-only'):
                display = job + '-shard (${{ matrix.shard }}/' + str(count) + ')'
                if _job_values(body, 'name') != [display]:
                    findings.append(f'ci-post.yml {job} legs must keep their names: {display}')
    if 'release.yml' in workflows:
        prebuilt = _jobs(workflows['release.yml']).get('prebuilt', '')
        if _job_block(prebuilt, 'strategy') != _code(RELEASE_STRATEGY):
            findings.append('release.yml prebuilt must use exactly the isolated Linux x64 '
                            'and hosted ARM matrix')
        if any(re.search(r'macos|darwin|CMAKE_OSX_', '\n'.join(_code(step)), re.IGNORECASE)
               for step in _steps(prebuilt)):
            findings.append('release.yml prebuilt must not contain macOS build steps')
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


def _step_block(step: str, key: str) -> list[str]:
    return _job_block(re.sub(r'^( *)- ', r'\1  ', step, count=1), key)


def _normalized_verify_command(value: str) -> str:
    """Compare verifier arguments after harmless YAML and shell respellings."""
    value = re.sub(r'^[|>][+-]?(?:\s+|$)', '', value)
    value = re.sub(r'\\\s+', ' ', value)
    value = value.replace('--jobs=', '--jobs ').replace('--ctest-shard=', '--ctest-shard ')
    return ' '.join(value.split())


def _verify_step_commands(step: str) -> list[str]:
    """Read direct commands and the existing action wrapper's command input."""
    values = (_step_values(step, 'run')
              + _job_values('\n'.join(_step_block(step, 'with')[1:]), 'command'))
    return [_normalized_verify_command(value) for value in values]


def _compiler_cache_restore_with(step: str) -> list[str]:
    """Literal content hashes keep the same cache partition across shards."""
    return [_LITERAL_CACHE_HASH.sub('', line) if _key_value(line, 'key') is not None else line
            for line in _step_block(step, 'with')]


# ci_release_gate.py's phase-1 exemption is bounded by this exact writer.
RELEASE_BUMP_PUBLISH_STEP = r'''      - name: Write bumped VERSION, commit, tag, push
        id: publish
        if: ${{ !inputs.dry_run }}
        env:
          NEXT:  ${{ steps.bump.outputs.next }}
          TAG:   ${{ steps.bump.outputs.tag }}
          RESET: ${{ steps.catalog.outputs.reset }}
        run: |
          set -euo pipefail
          printf '%s\n' "${NEXT}" > VERSION
          cp "${RESET}" docker/run_failure_codes_diff.json
          git add VERSION docker/run_failure_codes_diff.json
          git commit -m "chore(release): ${TAG}"
          git tag -a "${TAG}" -m "Release ${TAG}"
          python3 scripts/gen_run_failure_catalog_diff.py --check
          git push --atomic origin HEAD "${TAG}"
          echo "sha=$(git rev-parse HEAD)" >> "$GITHUB_OUTPUT"
'''

# The release App token must see bypass_actors on every branch ruleset.
# release-ruleset-probe.yml reports that visibility with the same token mint.
# The gate requires one active main pull_request ruleset whose own bypass list
# is exactly Integration 4105147 with bypass_mode always.
# The workflow's contents: write GITHUB_TOKEN cannot replace that credential.
RELEASE_GATE_STEP = r'''      - name: Refuse unless pineforge/verify and pineforge/parity are green
        id: gate
        env:
          GH_TOKEN: ${{ steps.app.outputs.token }}
          GITHUB_REPOSITORY: ${{ github.repository }}
          GITHUB_SHA: ${{ github.sha }}
          GITHUB_REF: ${{ github.ref }}
        run: |
          python3 scripts/ci_release_gate.py gate \
            --repo "${GITHUB_REPOSITORY}" --sha "${GITHUB_SHA}" --ref "${GITHUB_REF}"
'''

# This tiny release-class workflow has no writer or release-gate phases.
# Pin the complete executable shape so no extra token consumer can be hidden
# in a workflow/job environment, an additional step or an error override.
RELEASE_RULESETS_PROBE_WORKFLOW = '''name: Release ruleset probe

on:
  workflow_dispatch:

permissions:
  contents: read

jobs:
  authz:
    name: authz
    # Preflight pins executable workflow lines after removing comment-only lines.
    # authz uses hosted ubuntu-24.04; the App-token probe uses pf-gcp-post.
    runs-on: ubuntu-24.04
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


def release_rulesets_probe_workflow_findings(workflow: str, release: str) -> list[str]:
    steps = _steps(_jobs(workflow).get('probe', ''))
    expected = _steps(_jobs(RELEASE_RULESETS_PROBE_WORKFLOW)['probe'])
    findings = []
    if not steps or _code(steps[0]) != _code(expected[0]):
        findings.append('release-ruleset-probe.yml probe must refuse a dispatch ref other than '
                        'refs/heads/main as its first step before minting the App token or checkout')
    probe_app = _step_named(steps, '      - name: Mint org App token')
    release_app = _step_named(_steps(_jobs(release).get('release', '')),
                             '      - name: Mint org App token')
    if (_code(workflow) != _code(RELEASE_RULESETS_PROBE_WORKFLOW)
            or _code(probe_app) != _code(release_app)):
        findings.append('release-ruleset-probe.yml must keep the dispatch-only read-only probe '
                        'with the same release App token')
    return findings


def release_bump_workflow_findings(workflow: str) -> list[str]:
    steps = _steps(_jobs(workflow).get('release', ''))
    publish = _step_named(steps, '      - name: Write bumped VERSION, commit, tag, push')
    findings = []
    if _code(publish) != _code(RELEASE_BUMP_PUBLISH_STEP):
        findings.append('release.yml must keep the exact two-file release bump writer, '
                        'commit and atomic push')
    gate = _step_named(steps, '      - name: Refuse unless pineforge/verify and pineforge/parity are green')
    if _code(gate) != _code(RELEASE_GATE_STEP):
        findings.append('release.yml must keep the unconditional release gate '
                        'with its release App token '
                        'for the App-only main pull_request barrier')
    app = _step_named(steps, '      - name: Mint org App token')
    checkout = _step_named(steps, '      - name: Checkout (full history)')
    if (_step_values(app, 'id') != ['app']
            or _step_values(app, 'uses') != ['actions/create-github-app-token@d72941d797fd3113feb6b93fd0dec494b13a2547']
            or _step_block(app, 'with') != [
                '        with:',
                '          app-id: ${{ secrets.PINEFORGE_APP_ID }}',
                '          private-key: ${{ secrets.PINEFORGE_APP_PRIVATE_KEY }}',
                '          owner: ${{ github.repository_owner }}',
                '          repositories: pineforge-engine']
            or _step_values(checkout, 'uses') != ['actions/checkout@11d5960a326750d5838078e36cf38b85af677262']
            or _step_block(checkout, 'with') != [
                '        with:', '          fetch-depth: 0',
                '          token: ${{ steps.app.outputs.token }}']):
        findings.append('release.yml bump push must use the release App installation token')
    return findings


def fast_rows(cmake: str) -> list[str]:
    """Read the single CMake population; no duplicate Python list of rows."""
    blocks = re.findall(r'^set\(PINEFORGE_PR_FAST_TESTS\n(.*?)^\)', cmake,
                        re.MULTILINE | re.DOTALL)
    if len(blocks) != 1:
        raise ValueError('expected one PINEFORGE_PR_FAST_TESTS list')
    rows = [line.strip() for line in blocks[0].splitlines() if line.strip()]
    if (not rows or len(rows) != len(set(rows))
            or any(not re.fullmatch(r'test_[A-Za-z0-9_]+', row) for row in rows)):
        raise ValueError('fast rows must be unique executable test names')
    return rows


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
            or not authz.startswith('\n' + _authz_header(workflow))
            or _job_values(authz, 'name') != ['authz']
            or _job_values(authz, 'permissions') != ['{}']
            or any(_job_values(authz, key) for key in ('needs', 'if', 'env', 'uses', 'strategy'))
            or hashlib.sha256(step.encode()).hexdigest() != AUTHZ_STEP_SHA256):
        findings.append(f'{name} must start with the verbatim canon authz job')
    events = _events(workflow)
    if events is None or 'pull_request_target' in events:
        findings.append(f'{name} must use readable events without pull_request_target')
    pr = re.search(r'(?m)^  pull_request:[^\n]*(?:\n(?:    [^\n]*|[ \t]*))*', workflow)
    if name == 'ci.yml' and (not pr or pr[0].strip() != (
            'pull_request:\n'
            '    # LB-387: suite jobs skip on a draft PR (the required aggregate then fails, never passes);\n'
            '    # ready_for_review starts the full run when the PR is marked ready.\n'
            '    # converted_to_draft re-runs the workflow so its failing aggregate supersedes the earlier ready run.\n'
            '    types: [opened, synchronize, reopened, ready_for_review, converted_to_draft]')):
        findings.append('ci.yml must run on every pull_request without event filters')
    if _job_values(workflow, 'permissions') != ['contents: read']:
        findings.append(f'{name} must retain contents: read workflow permissions')
    for job, body in jobs.items():
        if job != 'authz' and 'authz' not in _needs(body):
            findings.append(f'{name} job {job} must need authz')
        code = '\n'.join(_code(body))
        steps = _steps(body)
        api_reader = (name == 'ci.yml' and job in ('changes', 'build-gate')
                      and sum(_code(step) == _code(GITHUB_ONLY_STEP) for step in steps) == 1)
        # The exact inline reader gets a read-only PR token; no other step or
        # job gains a token exception from the new path class.
        credential_code = (code.replace('\n'.join(_code(GITHUB_ONLY_STEP)), '', 1)
                           if api_reader else code)
        inherited_env = '\n'.join(_job_values(workflow, 'env'))
        if (_holds_credentials(workflow, body)
                or _job_values(body, 'secrets')
                or re.search(r'\bsecrets\s*[.\[]', code + '\n' + inherited_env)
                or (job != 'authz' and re.search(r'\bgithub\s*\.\s*token\b', credential_code))):
            findings.append(f'{name} job {job} must carry no secrets, tokens or environment')
        allowed_permissions = (['contents: read pull-requests: read'],) if api_reader else ([], ['contents: read'])
        if job != 'authz' and _job_values(body, 'permissions') not in allowed_permissions:
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
            positive_guards = ([AUTHZ_SUCCESS], [AUTHZ_ALWAYS])
            if (name == 'ci.yml' and job == 'build'
                    and '      - name: Install the CTest pytest runner' in _code(step)):
                positive_guards += ([AUTHZ_FULL_SUCCESS],)
            if (status_function and repository_step
                    and _step_values(step, 'if') not in positive_guards):
                findings.append(f'{name} job {job} must guard repository steps on authz success')
    gate = jobs.get(aggregate, '')
    gate_steps = _steps(gate)
    if (_job_values(gate, 'if') not in (['always()'], ['${{ always() }}'])
            or len(gate_steps) != (3 if name == 'ci.yml' else 2)
            or _code(gate_steps[0]) != _code(AUTHZ_REFUSE_STEP)
            or _step_values(gate_steps[-1], 'if')):
        findings.append(f'{name} {aggregate} must always run and refuse authz first')
    return findings


# Corpus needs persisted checkout credentials for its HTTPS submodule clone,
# and docs deploys with Cloudflare secrets: the full authz contract forbids both.
def gated_workflow_findings(workflow: str, name: str) -> list[str]:
    """Pin canon authz and each hosted/fleet dependant without those restrictions."""
    findings = []
    jobs = _jobs(workflow)
    authz = jobs.get('authz', '')
    steps = _steps(authz)
    step = steps[0].rstrip('\n') + '\n' if len(steps) == 1 else ''
    if (not jobs or _unparsed(workflow) or next(iter(jobs)) != 'authz'
            or not authz.startswith('\n' + _authz_header(workflow))
            # Extra job keys such as continue-on-error can bypass refusal
            # without changing either the header or the single step's hash.
            or _code(authz) != _code(_authz_header(workflow)) + ['    steps:'] + _code(step)
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
        runner = HOSTED_JOB_RUNNERS.get(name, {}).get(job, _workflow_runner(workflow))
        if [_runner_value(value) for value in runners] != [_runner_value(runner)]:
            findings.append(f'{name} job {job} must run on {runner}')
    return findings


def corpus_parity_trigger_findings(parity: str) -> list[str]:
    """Keep full parity off pull requests, including reusable-workflow callers."""
    findings = []
    key = _ON_KEY.search(parity)
    events = parity[key.end():] if key else ''
    after = re.search(r'^[^\s#]', events, re.MULTILINE)
    events = events[:after.start()] if after else events
    # _events ignores comments; refuse commented-out trigger keys as well.
    if (_events(parity) != {'schedule', 'workflow_dispatch', 'workflow_call'}
            or re.search(r'(?m)^[ \t]*#[ \t]*[\'"]?[a-z_]+[\'"]?[ \t]*:', events)):
        findings.append('corpus-parity.yml must retain exactly schedule, workflow_dispatch '
                        'and workflow_call without commented-out triggers')
    # Compare canonically: a substring test would accept `... || true`.
    conditions = _job_values(_jobs(parity).get('corpus-parity', ''), 'if')
    canonical = [re.sub(r'\s+', '', condition) for condition in conditions]
    if canonical != ["inputs.mode!='subset'&&github.event_name!='pull_request'"]:
        findings.append('corpus-parity.yml job corpus-parity must exclude pull_request callers')
    return findings


def native_workflow_findings(native: str, curl_action: str | None = None) -> list[str]:
    findings = authz_workflow_findings(native, 'native-live.yml', 'native-live-gate')
    if _events(native) != {'workflow_call', 'workflow_dispatch'}:
        findings.append('native-live.yml must retain only workflow_call and workflow_dispatch')
    jobs = _jobs(native)
    job, gate = jobs.get('native-live', ''), jobs.get('native-live-gate', '')
    shards, aggregate = jobs.get('native-live-shards', ''), jobs.get('native-live-native', '')
    if (_job_block(job, 'strategy') != _code(NATIVE_STRATEGY)
            or _needs(job) != ['authz'] or _job_values(job, 'if')
            or _job_values(job, 'continue-on-error')
            or _job_values(job, 'name') != ['native-live (${{ matrix.profile }})']):
        findings.append('native-live must run both independent sanitizer profiles with fail-fast false, '
                        'the platform job maximum and no per-run budget')
    if (_job_block(shards, 'strategy') != _code(NATIVE_SHARD_STRATEGY)
            or _needs(shards) != ['authz'] or _job_values(shards, 'if')
            or _job_values(shards, 'continue-on-error')
            or _job_values(shards, 'name') != ['native-live-shard (${{ matrix.profile }}, ${{ matrix.shard }}/4)']):
        findings.append('native-live-shards must run exactly native shards 1..4 with fail-fast false')
    aggregate_steps = _steps(aggregate)
    if (_needs(aggregate) != ['authz', 'native-live-shards']
            or _job_values(aggregate, 'name') != ['native-live (native)']
            or _job_values(aggregate, 'if') != ['always()']
            or _job_values(aggregate, 'continue-on-error')
            or len(aggregate_steps) != 2
            or _code(aggregate_steps[0]) != _code(AUTHZ_REFUSE_STEP)
            or _code(aggregate_steps[-1]) != _code(NATIVE_SHARD_GATE_STEP)):
        findings.append('native-live-native must fail unless all four native shard proofs succeed')
    gate_steps = _steps(gate)
    if (set(jobs) != {'authz', 'native-live', 'native-live-shards', 'native-live-native', 'native-live-gate'}
            or _needs(gate) != ['authz', 'native-live', 'native-live-native']
            or _job_values(gate, 'if') != ['always()']
            or _job_values(gate, 'continue-on-error')
            or len(gate_steps) != 2
            or _code(gate_steps[-1]) != _code(NATIVE_GATE_STEP)):
        findings.append('native-live-gate must fail unless every profile succeeds')
    shard_steps = _steps(shards)
    proof = _step_named(shard_steps, '        id: shard-proof')
    outputs = ['    outputs:'] + [
        f'      shard_{index}: ${{{{ steps.shard-proof.outputs.shard_{index} }}}}' for index in range(1, 5)]
    if (_job_block(shards, 'outputs') != outputs
            or _code(proof) != _code(NATIVE_SHARD_PROOF_STEP)
            or not shard_steps or proof != shard_steps[-1]):
        findings.append('native-live-shards must publish each successful verifier summary under its own shard output')
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
    for name, sharded in (('native-live', False), ('native-live-shards', True)):
        findings += _native_profile_job_findings(jobs.get(name, ''), name, sharded=sharded)
    if curl_action is not None:
        action_steps = _steps(curl_action, 4)
        restored = _step_named(action_steps, '      id: curl-cache')
        built = _step_named(action_steps, '      id: curl-build')
        saved = _step_named(action_steps, '    - name: Save pinned WebSocket-enabled libcurl')
        if (_step_values(restored, 'if') != ["${{ inputs.cold-cache != 'true' }}"]
                or _step_values(restored, 'uses') != ['actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830']
                or 'key: curl-${{ inputs.cache-namespace }}-' not in restored
                or 'restore-keys:' in restored
                or any(token not in restored for token in (
                    'steps.dependencies.outputs.version', 'steps.dependencies.outputs.sha256',
                    'steps.dependencies.outputs.identity', "hashFiles('.github/actions/setup-live-curl/action.yml', 'scripts/build_live_curl.sh')"))
                or '  cold-cache:\n' not in curl_action or "    default: 'false'" not in curl_action):
            findings.append('pinned curl restore must include dependency identity and bypass every cold-cache restore')
        if (_step_values(saved, 'if') != [CURL_CACHE_SAVE]
                or _step_values(saved, 'uses') != ['actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830']
                or _step_values(built, 'run') != ['bash scripts/build_live_curl.sh build-native-deps 4']
                or '        key: ${{ steps.curl-cache.outputs.cache-primary-key }}' not in _code(saved)
                or not built or not saved or curl_action.index(saved) < curl_action.index(built)
                or 'uses: actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830' in curl_action):
            findings.append('pinned curl must save immediately after its successful build, independently of later tests')
    return findings


def post_shard_workflow_findings(jobs: dict[str, str]) -> list[str]:
    """Keep each leg independent, its cache single-writer and diagnostics distinct."""
    findings = []
    for aggregate, (job, display) in POST_LEG_GATES.items():
        body = jobs.get(aggregate, '')
        expected_steps = [_code(AUTHZ_REFUSE_STEP),
                          _code(POST_SHARD_GATE_STEP.replace('SHARD_JOB', job))]
        if (sorted(_needs(body)) != sorted(['authz', 'changes', job])
                or _job_values(body, 'name') != [display]
                or _job_values(body, 'if') != [DOCS_ONLY_SKIP]
                or _job_block(body, 'strategy') or _job_values(body, 'uses')
                or [_code(step) for step in _steps(body)] != expected_steps):
            findings.append(f'{aggregate} must require only its own complete shard matrix '
                            'and keep its docs-only scheduling policy')
    for job, count in POST_SHARDS.items():
        body = jobs.get(job, '')
        steps = _steps(body)
        restore = _step_named(steps, '      - name: Restore ccache')
        save = _step_named(steps, '      - name: Save main ccache')
        cache_key = (BUILD_CACHE_KEY if job in ('build', 'build-debug') else
                     'ccache-${{ runner.os }}-${{ runner.arch }}-'
                     + ('kernel' if job == 'kernel-only' else 'sanitizers') + '-')
        if (_step_values(restore, 'id') != ['ccache-restore']
                or _step_values(restore, 'uses') != [
                    'actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830']
                or _compiler_cache_restore_with(restore) != [
                    '        with:', '          path: ${{ env.CCACHE_DIR }}',
                    f'          key: {cache_key}${{{{ github.sha }}}}',
                    '          restore-keys: |', f'            {cache_key}']
                or _step_values(save, 'if') != [SHARD_CACHE_SAVE]
                or _step_values(save, 'uses') != [
                    'actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830']
                or _step_block(save, 'with') != [
                    '        with:', '          path: ${{ env.CCACHE_DIR }}',
                    '          key: ${{ steps.ccache-restore.outputs.cache-primary-key }}']
                or any(_step_values(step, 'uses') == [
                    'actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830'] for step in steps)):
            findings.append(f'{job} must restore one compiler cache per architecture/profile/SHA '
                            'and save from shard 1 only')
        upload = _step_named(steps, '      - name: Retain CI diagnostics')
        diagnostic = ('${{ matrix.platform.os }}-${{ matrix.build_type }}'
                      if job in ('build', 'build-debug') else
                      'kernel' if job == 'kernel-only' else 'sanitizers')
        if ('          name: ci-diagnostics-' + diagnostic
                + '-shard-${{ matrix.shard }}-of-' + str(count) not in _code(upload)
                or _step_values(upload, 'if') != ['always()']):
            findings.append(f'{job} must retain separate diagnostics for every shard')
    return findings


def _native_profile_job_findings(job: str, name: str, *, sharded: bool) -> list[str]:
    findings = []
    steps = _steps(job)
    restore = _step_named(steps, '        id: ccache-restore')
    verify = _step_named(steps, '        id: verify')
    curl = _step_named(steps, '        id: curl-deps')
    save = _step_named(steps, '      - name: Save ccache after verification')
    collect = _step_named(steps, '      - name: Stage and summarize diagnostics')
    upload = _step_named(steps, '      - name: Retain CI diagnostics')
    cache_key = 'ccache-${{ runner.os }}-${{ runner.arch }}-live-v2-${{ matrix.profile }}-'
    if sharded:
        cache_key += 'shard-${{ matrix.shard }}-'
    if (_step_values(restore, 'if') != ['${{ !inputs.cold_cache }}']
            or _step_values(restore, 'uses') != ['actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830']
            or f'          key: {cache_key}${{{{ github.sha }}}}' not in _code(restore)
            or f'            {cache_key}' not in _code(restore)
            or '      CCACHE_DIR: ${{ github.workspace }}/.ccache/${{ matrix.profile }}' not in _code(job)
            or '          ccache --zero-stats' not in _code(job)):
        findings.append(f'{name} compiler restore must be profile-isolated, cold-bypassable and reset statistics')
    if (_step_values(curl, 'uses') != ['./.github/actions/setup-live-curl']
            or '          cache-namespace: ${{ matrix.profile }}' not in _code(curl)
            or '          cold-cache: ${{ inputs.cold_cache }}' not in _code(curl)):
        findings.append('every native profile must use pinned curl with its namespace and cold-cache input')
    command = _verify_step_commands(verify)
    expected = ('python3 scripts/ci_verify.py ${{ matrix.profile }} '
                '--build-dir ${{ matrix.build_dir }} ' + f'--jobs {CORES} '
                '--generator Ninja --curl-dir "${{ steps.curl-deps.outputs.curl-dir }}" '
                '--ccache')
    if sharded:
        expected += ' --require-websocket ' + NATIVE_EXCLUSION + ' --ctest-shard ${{ matrix.shard }}/4'
    if (command != [expected]
            or _step_values(verify, 'if')
            or _step_values(verify, 'timeout-minutes')
            or job.count('--exclude-label') != int(sharded) or job.count('scripts/ci_verify.py ') != 1):
        findings.append(f'{name} must verify each full profile with no step deadline or timeout flag and '
                        'native-only PR exclusion')
    if (_step_values(save, 'if') != [NATIVE_CACHE_SAVE]
            or _step_values(save, 'uses') != ['actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830']
            or '          key: ${{ steps.ccache-restore.outputs.cache-primary-key }}' not in _code(save)
            or '          path: ${{ env.CCACHE_DIR }}' not in _code(save)
            or (save and upload and job.index(save) < job.index(upload))):
        findings.append(f'{name} must explicitly save compatible compiler objects after diagnostics on normal test failure')
    artifact = 'ci-diagnostics-native-live-${{ matrix.profile }}'
    if sharded:
        artifact += '-${{ matrix.shard }}of4'
    if (_step_values(collect, 'if') != ['always()']
            or _step_values(upload, 'if') != ['always()']
            or '--build-dir ${{ matrix.build_dir }} --profile ${{ matrix.profile }}' not in collect
            or f'          name: {artifact}' not in _code(upload)
            or '          COLD_CACHE: ${{ inputs.cold_cache }}' not in _code(collect)
            or '          fetch-depth: 0' not in _code(job)
            or '          persist-credentials: false' not in _code(job)):
        findings.append(f'{name} must retain per-profile diagnostics on failure and preserve safe full checkout')
    return findings


def pipeline_workflow_findings(workflow: str, name: str, *, pr: bool) -> list[str]:
    """Pin each pipeline's actual jobs, dependencies, commands and aggregate."""
    findings = authz_workflow_findings(workflow, name, 'build-gate')
    events = CI_EVENTS if pr else POST_CI_EVENTS
    if _job_block(workflow, 'on') != _code(events):
        findings.append(f'{name} must keep its exact {"unfiltered PR" if pr else "main push, nightly and manual"} triggers')
    if pr and _job_block(workflow, 'concurrency') != _code(CI_CONCURRENCY):
        findings.append('ci.yml must keep its exact draft and ready concurrency groups')
    jobs = _jobs(workflow)
    proofs = PR_PROOF_JOBS if pr else PROOF_JOBS
    expected_jobs = {'authz', 'changes', 'preflight', *proofs, 'build-gate'}
    if not pr:
        expected_jobs.add('preflight-slow')
    findings += [f'{name} is missing job {job}' for job in sorted(expected_jobs - jobs.keys())]
    if set(jobs) - expected_jobs:
        findings.append(f'{name} must keep only its pinned {"PR" if pr else "full-event"} jobs')
    named_jobs = {'authz', 'build', 'build-arm', 'build-gate'}
    if not pr:
        named_jobs.update(POST_SHARDS)
        named_jobs.update(POST_LEG_GATES)
    for job in expected_jobs - named_jobs:
        if _job_values(jobs.get(job, ''), 'name'):
            findings.append(f'{job} must keep its default check name')
    changes = jobs.get('changes', '')
    if _needs(changes) != ['authz'] or _job_values(changes, 'if'):
        findings.append('changes must start after authz on every run')
    if pr and ('      github_only: ${{ steps.github-only.outputs.github_only }}\n' not in changes
               or not any(_code(step) == _code(GITHUB_ONLY_STEP) for step in _steps(changes))):
        findings.append('changes must publish the event-bound GitHub-only PR scheduling hint')
    if ('      docs_only: ${{ steps.classify.outputs.docs_only }}\n' not in changes
            or CLASSIFY_STEP not in changes + '\n'):
        findings.append("changes must publish the base's scripts/ci_docs_only.py answer, exactly")
    if ('      verifier_slow: ${{ steps.verifier.outputs.verifier_slow }}\n' not in changes
            or VERIFIER_CLASSIFY_STEP not in changes + '\n'):
        findings.append("changes must publish the base's scripts/ci_verifier_paths.py answer, exactly")
    preflight = jobs.get('preflight', '')
    if (sorted(_needs(preflight)) != ['authz', 'changes']
            or _job_values(preflight, 'if') != [PREFLIGHT_IF if pr else AUTHZ_ALWAYS]):
        findings.append('preflight must follow authz and changes and run even if classification fails')
    if (f'      PINEFORGE_VERIFIER_SLOW: {PREFLIGHT_VERIFIER_SLOW}\n' not in preflight
            or preflight.count('PINEFORGE_VERIFIER_SLOW:') != 1):
        findings.append('preflight must consume verifier_slow and default to all classes')
    steps = _steps(preflight)
    verify = _step_named(steps, '      - name: Verify workflow wiring and source guards')
    tier = PREFLIGHT_TIER if pr else 'fast'
    if (_step_values(verify, 'run') != [
            f'python3 scripts/ci_preflight.py --output-dir build-ci-preflight --tier {tier}']
            or _step_values(verify, 'if') != [AUTHZ_SUCCESS]):
        findings.append('preflight must invoke only its fast stage plan')
    linters = _step_named(steps, '      - name: Install workflow linters')
    if (len(steps) != 5 or [_step_values(step, 'uses') for step in steps] != [
            [], ['actions/checkout@11d5960a326750d5838078e36cf38b85af677262'], [], [], ['actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02']]
            or _step_values(linters, 'run') != [PREFLIGHT_LINTER_RUN]
            or any(_step_values(step, 'run') for step in steps if _step_values(step, 'uses'))):
        findings.append('preflight must keep only checkout, linters, fast checks and diagnostics')
    if not pr:
        slow = jobs.get('preflight-slow', '')
        verify_slow = _step_named(_steps(slow), '      - name: Verify deferred preflight stages')
        if (sorted(_needs(slow)) != ['authz', 'changes']
                or _job_values(slow, 'if') != [AUTHZ_ALWAYS]
                or _step_values(verify_slow, 'if') != [AUTHZ_SUCCESS]
                or _step_values(verify_slow, 'run') != [
                    'python3 scripts/ci_preflight.py --output-dir build-ci-preflight-slow --tier slow']):
            findings.append('preflight-slow must run all deferred stages on full events only')
    for job in proofs.keys() - (set() if pr else POST_LEG_GATES.keys()):
        body = jobs.get(job, '')
        if (sorted(_needs(body)) != ['authz', 'changes']
                or _job_values(body, 'if') != [GITHUB_ONLY_SKIP if pr else DOCS_ONLY_SKIP]):
            findings.append(f'{job} must start alongside preflight, after authz and changes, '
                            'and skip only its classified path policy')
    gate = jobs.get('build-gate', '')
    dependencies = expected_jobs - {'build-gate'}
    if (set(_needs(gate)) != dependencies or len(_needs(gate)) != len(dependencies)
            or _job_values(gate, 'if') != ['always()'] or _job_values(gate, 'name') != ['build']):
        findings.append('build-gate must aggregate authz, every proof job, preflight and changes')
    for job in dependencies:
        variable = (proofs[job] if job in proofs else
                    {'authz': 'AUTHZ_RESULT', 'changes': 'CHANGES_RESULT',
                     'preflight': 'PREFLIGHT_RESULT', 'preflight-slow': 'SLOW_PREFLIGHT_RESULT'}[job])
        if f'{variable}: ${{{{ needs.{job}.result }}}}' not in gate:
            findings.append(f'build-gate must read the result of {job}')
    if 'DOCS_ONLY: ${{ needs.changes.outputs.docs_only }}' not in gate:
        findings.append("build-gate must read changes' docs_only answer")
    if '[[ "$AUTHZ_RESULT" == "success" && "$CHANGES_RESULT" == "success" ]]' not in gate:
        findings.append('build-gate must require authz and changes success')
    if '[[ "$PREFLIGHT_RESULT" == "success" ]]' not in gate or 'passed "$PREFLIGHT_RESULT"' in gate:
        findings.append('build-gate must require preflight success')
    for job, variable in proofs.items():
        if f'passed "${variable}"' not in gate:
            findings.append(f'build-gate must require {job} success or a docs-only skip')
    gate_steps = _steps(gate)
    if pr and (len(gate_steps) != 3 or _code(gate_steps[1]) != _code(GITHUB_ONLY_STEP)
               or 'needs.changes.outputs.github_only' in gate):
        findings.append('build-gate must independently read the event-bound PR diff before accepting skips')
    if not gate_steps or _code(gate_steps[-1]) != _code(GATE_STEP if pr else POST_GATE_STEP):
        findings.append('build-gate must keep the exact result policy: success or a classified docs-only skip')
    if not pr:
        findings += post_shard_workflow_findings(jobs)
    for job in (('build',) if pr else ('build', 'build-debug', 'build-arm')):
        build = jobs.get(job, '')
        build_step = _step_named(_steps(build), '      - name: Verify (${{ matrix.build_type }})')
        shard_flag = (' --ctest-shard ${{ matrix.shard }}/' + str(POST_SHARDS[job])
                      if not pr and job in POST_SHARDS else '')
        if (_step_values(build_step, 'run') != [
                BUILD_VERIFY_RUN if pr else POST_BUILD_VERIFY_RUN + shard_flag]
                or _step_values(build_step, 'if') != [AUTHZ_SUCCESS]
                or _counted_text(build).count('scripts/ci_verify.py') != (0 if pr else 1)
                or _counted_text(build).count('ctest --test-dir') != (1 if pr else 0)):
            findings.append(f'{job} must run the fast Release smoke only on PRs and full profiles otherwise')
        cache = _step_named(_steps(build), '      - name: Restore ccache')
        save = _step_named(_steps(build), '      - name: Save main ccache')
        if (_step_values(cache, 'uses') != ['actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830']
                or _step_values(cache, 'id') != ['ccache-restore']
                or _step_block(cache, 'with') != [
                    '        with:', '          path: ${{ env.CCACHE_DIR }}',
                    f'          key: {BUILD_CACHE_KEY}${{{{ github.sha }}}}',
                    '          restore-keys: |', f'            {BUILD_CACHE_KEY}']
                or _step_values(save, 'uses') != ['actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830']
                or _step_values(save, 'if') != [SHARD_CACHE_SAVE if shard_flag else BUILD_CACHE_SAVE]
                or _step_block(save, 'with') != [
                    '        with:', '          path: ${{ env.CCACHE_DIR }}',
                    '          key: ${{ steps.ccache-restore.outputs.cache-primary-key }}']):
            findings.append(f'{job} must restore the main compiler cache by platform and build type')
        pytest_step = _step_named(_steps(build), '      - name: Install the CTest pytest runner')
        if (pr and pytest_step) or (not pr and _step_values(pytest_step, 'if') != [AUTHZ_SUCCESS]):
            findings.append(f'{job} must install pytest only for the full profiles')
        if not pr:
            platform = _step_named(_steps(build), '      - name: Require the requested platform')
            expected_platform = POST_ARM_PLATFORM_STEP if job == 'build-arm' else POST_PLATFORM_STEP
            if _code(platform) != _code(expected_platform):
                findings.append(f'full {job} must refuse a runner whose OS differs from its named platform')
    if not pr:
        for job in ('build', 'build-debug'):
            count = POST_SHARDS[job]
            expected_steps = [
                _code(POST_ARM_PLATFORM_STEP)
                if _step_values(step, 'name') == ['Require the requested platform']
                else _code(step.replace('ci-diagnostics-${{ matrix.platform.os }}-',
                                        'ci-diagnostics-ubuntu-24.04-arm-')
                               .replace(' --ctest-shard ${{ matrix.shard }}/' + str(count), '')
                               .replace('-shard-${{ matrix.shard }}-of-' + str(count), '')
                               .replace(SHARD_CACHE_SAVE, BUILD_CACHE_SAVE))
                for step in _steps(jobs.get(job, ''))]
            if [_code(step) for step in _steps(jobs.get('build-arm', ''))] != expected_steps:
                findings.append(f'build-arm must keep the same verification steps as {job}')
        for job, profile, build_dir, extra in (
                ('sanitizers', 'sanitizers', 'build-asan', ''),
                ('kernel-only', 'kernel', 'build-kernel',
                 ' --curl-dir "${{ steps.curl-deps.outputs.curl-dir }}"')):
            verify_steps = [
                step for step in _steps(jobs.get(job, ''))
                if any(_VERIFY_CALL.search(value) for value in _verify_step_commands(step))]
            step = verify_steps[0] if len(verify_steps) == 1 else ''
            command = (f'python3 scripts/ci_verify.py {profile} --build-dir {build_dir} '
                       f'--jobs {CORES} --ccache' + extra
                       + ' --ctest-shard ${{ matrix.shard }}/' + str(POST_SHARDS[job]))
            if (_verify_step_commands(step) != [command]
                    or _step_values(step, 'if') != [AUTHZ_SUCCESS]):
                findings.append(f'{job} must run its full profile on every shard')
    for job, body in jobs.items():
        if any(flag in body for flag in ('--exclude-label', '--include-label', '--class-tier')):
            findings.append(f'{job} must keep its full population')
    if pr:
        subset = jobs.get('corpus-parity-subset', '')
        if (_job_values(subset, 'uses') != ['./.github/workflows/corpus-parity-subset.yml']
                or _job_values(subset, 'with')):
            findings.append('PR corpus-parity-subset must call only corpus-parity-subset.yml without mode inputs')
    else:
        if ('      exclude_slow: false' not in _code(jobs.get('native-live', ''))
                or _job_values(jobs.get('native-live', ''), 'uses') != ['./.github/workflows/native-live.yml']):
            findings.append('native-live must receive the full population input')
        if 'cold_cache: ${{ inputs.cold_native_cache || false }}' not in jobs.get('native-live', ''):
            findings.append('CI manual native cold-cache proof must be explicit and default off')
        for job, mode in (('corpus-parity-subset', 'subset'), ('corpus-parity', 'full')):
            body = jobs.get(job, '')
            if (_job_values(body, 'uses') != ['./.github/workflows/corpus-parity.yml']
                    or _job_block(body, 'with') != ['    with:', f'      mode: {mode}']):
                findings.append(f'{job} must call corpus-parity.yml with mode {mode}')
    return findings


def read_ci_text(path: Path, findings: list[str], root: Path | None = None) -> str:
    """A tracked CI file's text, every line break byte exactly as stored.

    Path.read_text() decodes in universal-newline mode: a bare CR reached every
    checker as LF, so the action-pin lint's refusal of it could never fire on a
    real file. This reads the bytes and decodes them as strict UTF-8, never the
    locale's codec. A file that is not UTF-8 is a finding (never a crash) and its
    text, bad bytes replaced, still goes on so the other findings print too. The
    finding names the file relative to root (the repository when None).
    """
    data = path.read_bytes()
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError as error:
        name = path.relative_to(ROOT if root is None else root).as_posix()
        line = data.count(b'\n', 0, error.start) + 1
        findings.append(f'{name}:{line}: not valid UTF-8 ({error.reason} at byte offset '
                        f'{error.start}): save the file as UTF-8')
        return data.decode('utf-8', 'replace')


def _universal_newlines(text: str) -> str:
    """The text as Path.read_text() always gave it: CRLF and a bare CR are LF.

    Every check except the action-pin lint reads this view, so a CRLF file is
    judged exactly as it was before the loaders kept the bytes.
    """
    return text.replace('\r\n', '\n').replace('\r', '\n')


def ci_workflow_findings(ci: str, native: str, promote: str, cmake: str,
                         parity: str, docs: str, post: str, subset: str, docs_post: str,
                         others: dict[str, str] | None = None,
                         *, curl_action: str | None = None,
                         actions: dict[str, str] | None = None) -> list[str]:
    """Check actual PR/post files together, including every reusable call edge.

    The texts are the files as stored (read_ci_text): the action-pin lint judges
    them so, and every other check gets the universal-newline view it always had.
    """
    workflows = {'ci.yml': ci, 'ci-post.yml': post, 'native-live.yml': native,
                 'corpus-parity.yml': parity, 'corpus-parity-subset.yml': subset,
                 'docs.yml': docs, 'docs-post.yml': docs_post,
                 'promote-baseline.yml': promote, **(others or {})}
    action_sources = {f'.github/workflows/{name}': text for name, text in workflows.items()}
    action_sources.update(actions or {})
    if curl_action is not None:
        action_sources.setdefault('.github/actions/setup-live-curl/action.yml', curl_action)
    workflows = {name: _universal_newlines(text) for name, text in workflows.items()}
    ci, native, promote, cmake, parity, docs, post, subset, docs_post = map(
        _universal_newlines, (ci, native, promote, cmake, parity, docs, post, subset, docs_post))
    curl_action = None if curl_action is None else _universal_newlines(curl_action)
    findings = runner_findings(workflows)
    findings += action_pin_findings(action_sources)
    findings += app_token_environment_findings(workflows)
    findings += pipeline_workflow_findings(ci, 'ci.yml', pr=True)
    findings += pipeline_workflow_findings(post, 'ci-post.yml', pr=False)
    findings += native_workflow_findings(native, curl_action)
    findings += corpus_parity_trigger_findings(parity)
    for name, workflow in (('corpus-parity.yml', parity), ('corpus-parity-subset.yml', subset),
                           ('docs.yml', docs), ('docs-post.yml', docs_post)):
        findings += gated_workflow_findings(workflow, name)
    if (_events(subset) != {'workflow_call'}
            or set(_jobs(subset)) != {'authz', 'corpus-parity-subset'}
            or _job_values(_jobs(subset).get('corpus-parity-subset', ''), 'if')):
        findings.append('PR parity workflow must retain only workflow_call, authz and an unconditional subset job')
    findings += docs_workflow_findings(docs, docs_post)
    if 'release.yml' in workflows:
        findings += release_bump_workflow_findings(workflows['release.yml'])
        findings += release_rulesets_probe_workflow_findings(
            workflows.get('release-ruleset-probe.yml', ''), workflows['release.yml'])
    for name, workflow in workflows.items():
        if name in RELEASE_JOB_RUNNERS:
            continue
        for job, body in _jobs(workflow).items():
            if any(_CONTINUE_ON_ERROR.match(line) for line in _code(body)):
                findings.append(f'{name} job {job} must not continue on error')
    # Baseline promotion is pineforge-workflow's campaign/ci/promote-baseline.yml,
    # installed unchanged. Main's own copy runs on push to main and resolves
    # exactly one merged PR in the guard before authorizing its API identities.
    # PR objects are fetched only for tree comparison. Both events retain
    # the newest two engine head statuses and the codegen check-run rule.
    promote_job = _jobs(promote).get('promote', '')
    guard_job = _jobs(promote).get('promote-guard', '')
    guard_steps = _steps(guard_job)
    promote_steps = _steps(promote_job)
    resolved = _step_named(guard_steps, '        id: pr')
    authorized = _step_named(guard_steps, '        id: authorize')
    exacttree = _step_named(promote_steps, '        id: exacttree')
    head_gate = _step_named(promote_steps, '        id: ci')
    if (_job_values(promote, 'concurrency') != [
            'group: promote-baseline cancel-in-progress: false']
            or _job_values(promote_job, 'concurrency')):
        findings.append('baseline promotion must queue in group promote-baseline without cancellation')
    resolver_env = (
        'GH_TOKEN: ${{ github.token }}',
        'EVENT_NAME: ${{ github.event_name }}',
        'REPO: ${{ github.repository }}',
        'REF: ${{ github.ref }}',
        'SHA: ${{ github.sha }}',
        'INPUT_MERGE_SHA: ${{ github.event.inputs.merge_commit }}',
        'INPUT_HEAD_SHA: ${{ github.event.inputs.head_sha }}',
        'INPUT_PR_NUMBER: ${{ github.event.inputs.pr_number }}',
        'INPUT_PR_URL: ${{ github.event.inputs.pr_url }}',
        'INPUT_MERGED_AT: ${{ github.event.inputs.merged_at }}',
    )
    resolver_selection = (
        'python3 - 3<<<"$pulls" <<\'PY\'',
        'sha = os.environ["SHA"]',
        'pages = json.loads(open(3, encoding="utf-8").read(), parse_constant=reject_constant)',
        ('matches = [pr for page in pages for pr in page '
         'if pr.get("state") == "closed" '
         'and pr.get("merged_at") is not None '
         'and pr.get("merge_commit_sha") == sha '
         'and isinstance(pr.get("base"), dict) '
         'and pr["base"].get("ref") == "main"]'),
        'if len(matches) != 1:',
    )
    resolver_code = ' '.join(line.strip() for line in _code(resolved))
    if (not resolved or _step_values(resolved, 'if')
            or any(value not in resolved for value in resolver_env)
            or any(value not in resolver_code for value in resolver_selection)
            or 'gh api --paginate --slurp' not in resolved
            or 'repos/$REPO/commits/$SHA/pulls?per_page=100' not in resolved
            or 'github.event.pull_request' in promote
            or 'pull_request_target' in promote):
        findings.append('baseline promotion must resolve the pushed SHA or dispatch inputs without a PR payload')
    # The guard resolves and authorizes before the promote job is admitted.
    # Its main checkout, tree and gate checks precede any release credentials.
    # No extra step before them may acquire credentials or bypass a refusal.
    if ([_step_values(step, 'id') for step in guard_steps] != [['pr'], ['authorize']]
            or _step_values(authorized, 'if') != ["steps.pr.outputs.ok == 'true'"]
            or [_step_values(step, 'id') for step in promote_steps[:4]]
            != [['axis'], [], ['exacttree'], ['ci']]
            or _job_values(promote_job, 'if') != [PROMOTE_IF]
            or len(promote_steps) < 4
            or not any(value.startswith('actions/checkout@')
                       for value in _step_values(promote_steps[1], 'uses'))
            or _step_values(promote_steps[1], 'with') != ['fetch-depth: 0']
            or _step_values(exacttree, 'if')
            or _step_values(head_gate, 'if') != ["steps.exacttree.outputs.ok == 'true'"]
            or any(_step_values(step, 'if') != ["steps.ci.outputs.green == 'true'"]
                   for step in promote_steps[4:])):
        findings.append('baseline promotion must guard resolution, tree and gate checks before credentials')
    if (_events(promote) != {'push', 'workflow_dispatch'}
            or re.search(r'^  pull_request(?:_target)?:', promote, re.MULTILINE)
            or '  statuses: read' not in promote
            or '  pull-requests: read' not in promote
            or '  id-token: write' not in promote
            or promote.count('needs.promote-guard.outputs.head_sha') != 3
            or promote.count('needs.promote-guard.outputs.pr_number') != 2
            or promote.count('refs/pull/') != 1
            or '/commits/$HEAD_SHA/statuses?per_page=100' not in head_gate
            or 'gh api --paginate' not in head_gate
            or 'reduce (.[][]) as $s' not in head_gate
            or 'if has($s.context) then . else .[$s.context] = $s.state end' not in head_gate
            or '.["pineforge/verify"] == "success"' not in head_gate
            or '.["pineforge/parity"] == "success"' not in head_gate
            or head_gate.count('echo "green=false" >> "$GITHUB_OUTPUT"; exit 0') != 2):
        findings.append("baseline promotion must run main's copy and require the latest two head statuses")
    # The job pin cannot see inherited workflow settings.
    for key in ('defaults', 'env'):
        if _job_values(promote, key):
            findings.append(f'baseline promotion must not define workflow-level {key}')
    promotion_jobs = _jobs(promote)
    if _job_values(promotion_jobs.get('promote-guard', ''), 'defaults'):
        findings.append('baseline promotion promote-guard must not define job-level defaults')
    promotion_text = _jobs_text(promote) or ''
    promotion_headers = list(_JOB_HEADER.finditer(promotion_text))
    if ([header.group(1) for header in promotion_headers] != ['promote-guard', 'promote']
            or _job_values(guard_job, 'permissions') != ['pull-requests: read']
            or promotion_text[promotion_headers[0].start():promotion_headers[1].start()]
            != PROMOTE_GUARD):
        findings.append('baseline promotion must start with the pinned read-only promote-guard')
    promotion = promotion_jobs.get('promote', '')
    if (_job_values(promotion, 'needs') != ['[promote-guard]']
            or _job_values(promotion, 'if') != [PROMOTE_IF]):
        findings.append('baseline promotion must need promote-guard and require successful authorization')
    # PRs are squash-merged, so the PR head is never an ancestor of the base
    # branch. The merge commit (the resolved merge_commit_sha, or the dispatch
    # input) must be on the base branch and carry the gated PR head's tree; the
    # campaign tool gets that tree and promotes only the pair the merge gate's
    # verdict measured for it. Every skip exits green before promotion.
    merge_env = 'MERGE_SHA: ${{ needs.promote-guard.outputs.merge_sha }}'
    head_env = 'HEAD_SHA: ${{ needs.promote-guard.outputs.head_sha }}'
    if (promote.count(merge_env) != 2 or promote.count(head_env) != 3
            or promote.count('PR_NUMBER: ${{ needs.promote-guard.outputs.pr_number }}') != 2
            or 'PR_URL: ${{ needs.promote-guard.outputs.pr_url }}' not in promote
            or 'MERGED_AT: ${{ needs.promote-guard.outputs.merged_at }}' not in promote
            or '      head_sha:\n' not in promote
            or 'BASE_REF: main' not in exacttree
            or 'git fetch --quiet origin "$BASE_REF" "+refs/pull/$PR_NUMBER/head:refs/remotes/origin/pr-head"'
            not in exacttree
            or 'git merge-base --is-ancestor "$MERGE_SHA" "refs/remotes/origin/$BASE_REF"' not in exacttree
            or '--is-ancestor "$HEAD_SHA"' in promote
            or 'merge_tree=$(git rev-parse "$MERGE_SHA^{tree}")' not in exacttree
            or 'head_tree=$(git rev-parse "$HEAD_SHA^{tree}")' not in exacttree
            or 'if [ "$merge_tree" != "$head_tree" ]; then' not in exacttree
            or exacttree.count('echo "ok=false" >> "$GITHUB_OUTPUT"; exit 0') != 2
            or 'MERGE_TREE: ${{ steps.exacttree.outputs.merge_tree }}' not in promote
            or '--merge-commit "$MERGE_SHA" --head-sha "$HEAD_SHA" --merge-tree "$MERGE_TREE"'
            not in promote
            or '--ci-head "$HEAD_SHA" --ci-green' not in promote
            or 'if [ "$code" = "2" ]; then' not in promote):
        findings.append('baseline promotion must admit a squash merge by tree equality only')
    blocks = re.findall(r'^set\(PINEFORGE_PR_SLOW_TESTS\n(.*?)^\)', cmake,
                        re.MULTILINE | re.DOTALL)
    names = re.findall(r'^    (test_[A-Za-z0-9_]+)$', blocks[0], re.MULTILINE) if len(blocks) == 1 else []
    if (len(blocks) != 1 or len(names) != 43 or len(set(names)) != 43
            or tuple(names) != PR_SLOW_ROWS
            or cmake.count('APPEND PROPERTY LABELS slow') != 1
            or 'set_property(TEST ${_pf_slow_test} APPEND PROPERTY LABELS slow)' not in cmake
            or 'if(PINEFORGE_BUILD_SOURCE_LAYER AND NOT TEST ${_pf_slow_test})' not in cmake):
        findings.append('measured slow rows must keep one pinned CMake label list')
    try:
        rows = fast_rows(cmake)
    except ValueError:
        rows = []
    if (hashlib.sha256(('\n'.join(rows) + '\n').encode()).hexdigest() != PR_FAST_ROWS_SHA256
            or cmake.count('APPEND PROPERTY LABELS fast') != 1
            or '\n'.join(_code(cmake)).count('\n'.join(_code(FAST_TEST_REGISTRATION))) != 1
            or _code(cmake)[-len(_code(FAST_TEST_REGISTRATION)):] != _code(FAST_TEST_REGISTRATION)):
        findings.append('PR fast rows must keep one pinned CMake label list and executable build target')
    return list(dict.fromkeys(findings))


def docs_workflow_findings(workflow: str, post: str | None = None) -> list[str]:
    """PR docs only build; the separate main/dispatch workflow also deploys."""
    findings = []
    events = workflow.split('\non:\n', 1)
    events = events[1].split('\npermissions:', 1)[0] if len(events) == 2 else ''
    pr = re.search(r'^  pull_request:[^\n]*(?:\n(?:    [^\n]*|[ \t]*))*', events, re.MULTILINE)
    if not pr:
        findings.append('docs.yml must build on pull_request')
    elif _code(pr[0]) != _code(DOCS_PR_TRIGGER):
        findings.append('docs.yml must exclude only .github-only PRs from documentation builds')
    if _events(workflow) != {'pull_request'}:
        findings.append('docs.yml must retain only pull_request')
    if ('cloudflare/wrangler-action@' in workflow
            or 'secrets.' in '\n'.join(_code(_jobs(workflow).get('build', '')))):
        findings.append('docs.yml must contain no deployment step or deployment secrets')
    for name, text in (('docs.yml', workflow), *([] if post is None else [('docs-post.yml', post)])):
        jobs = _jobs(text)
        if set(jobs) != {'authz', 'build'} or _needs(jobs.get('build', '')) != ['authz']:
            findings.append(f'{name} must retain authz and its dependent build job')
        build = _step_named(_steps(jobs.get('build', '')), '      - name: Build site')
        if _step_values(build, 'run') != ['| set -euo pipefail bash docs/build.sh --self-test-seo bash docs/build.sh']:
            findings.append(f'{name} must build documentation after its SEO self-test')
    if post is not None:
        expected = 'on:\n  push:\n    branches: [main]\n  workflow_dispatch: {}\n'
        if _job_block(post, 'on') != _code(expected):
            findings.append('docs-post.yml must retain only main pushes and manual dispatch')
        deploy = _step_named(_steps(_jobs(post).get('build', '')),
                             '      - name: Deploy to Cloudflare Pages')
        if (_step_values(deploy, 'uses') != ['cloudflare/wrangler-action@9acf94ace14e7dc412b076f2c5c20b8ce93c79cd']
                or _step_values(deploy, 'if')
                or 'apiToken: ${{ secrets.CLOUDFLARE_API_TOKEN }}' not in deploy
                or 'accountId: ${{ secrets.CLOUDFLARE_ACCOUNT_ID }}' not in deploy
                or 'pages deploy docs/site/html' not in deploy):
            findings.append('docs-post.yml must retain its Cloudflare deployment on every successful build')
    return findings


def docs_workflow_self_test(workflow: str, post: str) -> int:
    if docs_workflow_findings(workflow, post):
        print('docs workflow contract: current workflow fails')
        return 1
    no_pr = workflow.replace(DOCS_PR_TRIGGER, '', 1)
    deploy_on_pr = workflow.replace('      - name: Upload artifact\n',
        '      - uses: cloudflare/wrangler-action@9acf94ace14e7dc412b076f2c5c20b8ce93c79cd # v3.15.0\n      - name: Upload artifact\n', 1)
    no_deploy = post.replace('uses: cloudflare/wrangler-action@9acf94ace14e7dc412b076f2c5c20b8ce93c79cd # v3.15.0', 'run: "true"', 1)
    if no_pr == workflow or deploy_on_pr == workflow or no_deploy == post:
        print('docs workflow contract: could not form all three mutations')
        return 1
    if (not docs_workflow_findings(no_pr, post)
            or not docs_workflow_findings(deploy_on_pr, post)
            or not docs_workflow_findings(workflow, no_deploy)):
        print('docs workflow contract: a mutation escaped')
        return 1
    print('docs workflow contract: missing PR trigger, PR deploy and missing post deploy refused')
    return 0


_LITERAL_RUNNER_LABEL = r'''(?:"[^"\n]+"|'[^'\n]+'|[A-Za-z0-9_.-]+)'''


def actionlint_runner_label_findings(source: Path) -> list[str]:
    """Check the repo's simple block-list config without a YAML dependency.

    Unsupported config shapes fail closed; actionlint in workflow-lint also
    parses this same required file with its full YAML/schema parser.
    """
    config_name = '.github/actionlint.yaml'
    findings = []
    try:
        config = _universal_newlines(read_ci_text(source / config_name, findings, source))
    except OSError as error:
        return [f'{config_name}: cannot read config: {error}']
    lines = [re.sub(r'\s+#.*$', '', line).rstrip() for line in _code(config)]
    entries = [re.fullmatch(r'    - (' + _LITERAL_RUNNER_LABEL + r')', line)
               for line in lines[2:]]
    if (lines[:2] != ['self-hosted-runner:', '  labels:']
            or not entries or not all(entries)):
        return [*findings,
                f'{config_name}: cannot parse config: expected self-hosted-runner.labels '
                'as a block list of literal labels']
    declared = {entry.group(1).strip('\'"') for entry in entries}
    for path in sorted((source / '.github/workflows').glob('*')):
        if path.suffix not in ('.yml', '.yaml'):
            continue
        workflow = _universal_newlines(read_ci_text(path, findings, source))
        if _unparsed(workflow):
            findings.append(f'{path.name}: cannot parse jobs for literal runner labels')
            continue
        for job, body in _jobs(workflow).items():
            mapping = '\n'.join(_job_block(body, 'runs-on')[1:])
            values = _job_values(mapping, 'labels') if mapping else _job_values(body, 'runs-on')
            for value in values:
                if not value.startswith('['):
                    if value.startswith(('pf-', 'gke-')) and re.fullmatch(_LITERAL_RUNNER_LABEL, value):
                        if value not in declared:
                            findings.append(f'{path.name} job {job}: label "{value}" is not declared '
                                            f'in {config_name}')
                    continue
                if not re.fullmatch(r'\[\s*' + _LITERAL_RUNNER_LABEL
                                    + r'(?:\s*,\s*' + _LITERAL_RUNNER_LABEL
                                    + r')*\s*,?\s*\]', value):
                    findings.append(f'{path.name} job {job}: cannot parse literal runs-on array')
                    continue
                labels = {token.strip('\'"') for token in re.findall(_LITERAL_RUNNER_LABEL, value)
                          if '${{' not in token}
                if mapping or 'self-hosted' in labels:
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
                   *, env: dict[str, str] | None = None, tier: str = 'all') -> list[tuple]:
    if tier not in ('all', 'fast', 'slow', 'github-only'):
        raise ValueError(f'unknown preflight tier: {tier}')
    output_dir = (source / 'build-ci-preflight') if output is None else output
    workflows = sorted(str(path) for path in (source / '.github/workflows').glob('*')
                       if path.suffix in ('.yml', '.yaml'))
    class_args = (['--class-tier', 'fast']
                  if tier == 'all' and verifier_class_tier(os.environ if env is None else env) == 'fast'
                  else [])
    commands = [
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
        # CR-150 / C4: a new growable container in the scanned engine, inline
        # header or generated C++ needs a manifest entry whose bound names only
        # declared fixed inputs; the existing sites are listed OPEN, never PASS
        # (ci/cr150-source-bounds.yaml). Unconditional: a merge that adds a site
        # in a file this change never touched fails here as well. A missing
        # manifest, an empty file set or a scan with no sites exits 2 (fails).
        ('source-guard-cr150-source-bounds',
         [sys.executable, str(source / 'scripts/cr150_source_gate.py'),
          '--root', str(source)]),
        # The gate's own mutation controls: each class of unlisted site, bad
        # bound, empty scan and missing manifest must change the verdict.
        ('cr150-source-gate-tests',
         [sys.executable, str(source / 'scripts/test_cr150_source_gate.py')]),
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
        # Includes the single App-only main pull_request barrier, complete
        # Actions/ruleset pages, permission docs, bump identity/parent/tree diff.
        # Binding in both fast/all.
        ('release-gate-tests', [sys.executable, str(source / 'scripts/test_ci_release_gate.py')]),
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
        ('duplicate-test-names-tests',
         [sys.executable, str(source / 'scripts/test_check_duplicate_test_names.py')]),
        ('duplicate-test-names',
         [sys.executable, str(source / 'scripts/check_duplicate_test_names.py'),
          '--root', str(source)]),
    ]
    if tier == 'github-only':
        return [entry for entry in commands if entry[0] in GITHUB_ONLY_PREFLIGHT_STAGES]
    return [entry for entry in commands
            if tier == 'all' or (entry[0] in SLOW_PREFLIGHT_STAGES) == (tier == 'slow')]


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
    parser.add_argument('--tier', choices=('all', 'fast', 'slow', 'github-only'), default='all')
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
                 'docs.yml', 'ci-post.yml', 'corpus-parity-subset.yml', 'docs-post.yml')
        # Every file is read as stored: the lint must see a bare CR on disk.
        # A file that is not UTF-8 is a finding here, and the curl action is
        # read twice (as an action and as the native job's input): list it once.
        findings = []
        actions = {path.relative_to(ROOT).as_posix(): read_ci_text(path, findings)
                   for path in sorted((ROOT / '.github/actions').rglob('*'))
                   if path.name in ('action.yml', 'action.yaml') and path.is_file()}
        checked = ci_workflow_findings(
            read_ci_text(workflows / 'ci.yml', findings),
            read_ci_text(workflows / 'native-live.yml', findings),
            read_ci_text(workflows / 'promote-baseline.yml', findings),
            read_ci_text(ROOT / 'tests/CMakeLists.txt', findings),
            read_ci_text(workflows / 'corpus-parity.yml', findings),
            read_ci_text(workflows / 'docs.yml', findings),
            read_ci_text(workflows / 'ci-post.yml', findings),
            read_ci_text(workflows / 'corpus-parity-subset.yml', findings),
            read_ci_text(workflows / 'docs-post.yml', findings),
            {path.name: read_ci_text(path, findings) for path in sorted(workflows.iterdir())
             if path.suffix in ('.yml', '.yaml') and path.name not in named},
            curl_action=read_ci_text(ROOT / '.github/actions/setup-live-curl/action.yml', findings),
            actions=actions)
        findings = list(dict.fromkeys(findings + checked))
        for finding in findings:
            print(finding)
        if not findings:
            print('CI workflow contract: PR smoke, full events, docs-only skip, statuses, '
                  'fast/slow rows, runners and time limits OK')
        return 1 if findings else 0
    if args.check_docs_workflow or args.self_test_docs_workflow:
        findings = []
        workflow = _universal_newlines(read_ci_text(ROOT / '.github/workflows/docs.yml', findings))
        post = _universal_newlines(read_ci_text(ROOT / '.github/workflows/docs-post.yml', findings))
        if args.self_test_docs_workflow:
            code = docs_workflow_self_test(workflow, post)
            for finding in findings:
                print(finding)
            return 1 if findings else code
        findings += docs_workflow_findings(workflow, post)
        for finding in findings:
            print(finding)
        if not findings:
            print('docs workflow contract: PR builds and post-merge deployment ... OK')
        return 1 if findings else 0
    # Pin the linter contract as well as its CI download. No optional lint lane.
    version_check = [sys.executable, '-c',
                     'import subprocess,sys; '
                     'v=subprocess.check_output(["actionlint","-version"],text=True).splitlines()[0]; '
                     f'print("actionlint "+v); sys.exit(0 if v == {ACTIONLINT_VERSION!r} else 1)']
    output_dir = args.output_dir.resolve()
    return run_checks([*([('actionlint-version', version_check)] if args.tier != 'slow' else []),
                       *check_commands(ROOT, output_dir, tier=args.tier)],
                      output_dir)


if __name__ == '__main__':
    raise SystemExit(main())
