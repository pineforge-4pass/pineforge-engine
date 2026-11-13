#!/usr/bin/env python3
"""Exercise both corpus admission shapes with independent workflow fixtures."""

from pathlib import Path
import shutil
import sys
import tempfile
import unittest

import ci_preflight
import test_ci_preflight as preflight_fixtures
from test_ci_preflight import in_job


ROOT = Path(__file__).resolve().parents[1]

# Literal reviewed workflow hunk. Never construct positive fixtures from the
# admission checker's templates, command tables or runner constants.
CORPUS_JOB = r'''  corpus-gitlink-result:
    needs: [admit]
    if: ${{ needs.admit.outputs.allowed == 'true' }}
    runs-on: ubuntu-24.04
    permissions: {contents: read}
    steps:
      - name: Checkout the base tree
        uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0
        with:
          repository: ${{ github.repository }}
          ref: ${{ needs.admit.outputs.base_sha }}
          path: base
          persist-credentials: false
      - name: Checkout the admitted head
        uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4.4.0
        with:
          repository: ${{ github.repository }}
          ref: ${{ needs.admit.outputs.head_sha }}
          path: head
          fetch-depth: 0
          persist-credentials: false
      - name: Set up Python
        uses: actions/setup-python@a26af69be951a213d495a4c3e4e4022e16d87065 # v5.6.0
        with:
          python-version: '3.12'
      - name: Require validation of the proposed corpus commit
        env:
          GH_TOKEN: ${{ github.token }}
          BASE_SHA: ${{ needs.admit.outputs.base_sha }}
          HEAD_SHA: ${{ needs.admit.outputs.head_sha }}
        run: python3 base/scripts/check_corpus_gitlink_result.py --repo head --base "$BASE_SHA" --head "$HEAD_SHA"

'''

# These files had no check_commands row before the corpus admission change.
# Some have separate CTest registrations; this census preserves that routing.
COMMAND_CENSUS_EXEMPTIONS = (
    ('scripts/test_abi_receipt_skips.py', 'unnamed before this change'),
    ('scripts/test_adapter_spec_shadowing.py', 'unnamed before this change'),
    ('scripts/test_aggregate_cpp_versions.py', 'unnamed before this change'),
    ('scripts/test_broker_state_hash_coverage.py', 'unnamed before this change'),
    ('scripts/test_check_lanes_cmake_d.py', 'unnamed before this change'),
    ('scripts/test_check_kernel_profile_rows.py', 'unnamed before this change'),
    ('scripts/test_check_replay_off_golden.py', 'unnamed before this change'),
    ('scripts/test_ci_preflight_split.py', 'unnamed before this change'),
    ('scripts/test_ci_verify_fetch_lock.py', 'unnamed before this change'),
    ('scripts/test_ci_verify_split.py', 'unnamed before this change'),
    ('scripts/test_cmake_version_source.py', 'unnamed before this change'),
    ('scripts/test_emit_abi_export_list.py', 'unnamed before this change'),
    ('scripts/test_example_runner.py', 'unnamed before this change'),
    ('scripts/test_market_admission_schema.py', 'unnamed before this change'),
    ('scripts/test_native_command_after_split.py', 'unnamed before this change'),
    ('scripts/test_native_cpp_versions.py', 'unnamed before this change'),
    ('scripts/test_native_feature_rulings.py', 'unnamed before this change'),
    ('scripts/test_native_include_independence.py', 'unnamed before this change'),
    ('scripts/test_native_live_equivalence_split.py', 'unnamed before this change'),
    ('scripts/test_open_report_rows_flat_split.py', 'unnamed before this change'),
    ('scripts/test_oracle_fixture_checks.py', 'unnamed before this change'),
    ('scripts/test_parity_packaged_paths.py', 'unnamed before this change'),
    ('scripts/test_pending_intent_view.py', 'unnamed before this change'),
    ('scripts/test_pending_intent_view_mutations.py', 'unnamed before this change'),
    ('scripts/test_private_fixture_guards.py', 'unnamed before this change'),
    ('scripts/test_private_fixture_reads.py', 'unnamed before this change'),
    ('scripts/test_regen_corpus_cpp_pin.py', 'unnamed before this change'),
    ('scripts/test_regen_validation_report.py', 'unnamed before this change'),
    ('scripts/test_run_json_outputs.py', 'unnamed before this change'),
    ('scripts/test_run_pytest_no_skips.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_aux_feed.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_dump_book.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_emit_window.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_identity.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_magnifier.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_mincontract.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_native_feed.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_range_end.py', 'unnamed before this change'),
    ('scripts/test_run_strategy_requests.py', 'unnamed before this change'),
    ('scripts/test_runtime_budget.py', 'unnamed before this change'),
    ('scripts/test_script_cpp_abi.py', 'unnamed before this change'),
    ('scripts/test_settlement_cpp_abi.py', 'unnamed before this change'),
    ('scripts/test_source_clone_volatile.py', 'unnamed before this change'),
    ('scripts/test_symbol_calendar.py', 'unnamed before this change'),
    ('scripts/test_twin_parity.py', 'unnamed before this change'),
    ('scripts/test_two_world_registration.py', 'unnamed before this change'),
    ('scripts/test_verify_corpus_count_rule.py', 'unnamed before this change'),
    ('scripts/test_verify_corpus_metrics.py', 'unnamed before this change'),
    ('scripts/test_verify_corpus_open_mark.py', 'unnamed before this change'),
    ('scripts/test_verify_corpus_trail_profile.py', 'unnamed before this change'),
)
NON_LINUX_COMMAND_CENSUS_EXEMPTIONS = (
    ('scripts/test_build_prebuilt_bookworm.py', 'existing command row requires Linux'),
    ('scripts/test_check_prebuilt_glibc.py', 'existing command row requires Linux'),
)


class GkeCorpusAdmissionVariantTests(unittest.TestCase):
    WORKFLOW = ROOT / '.github/workflows/gke-pr.yml'

    def setUp(self) -> None:
        self.actual = self.WORKFLOW.read_text(encoding='utf-8')
        self.original = self.without_corpus(self.actual)
        self.workflow = self.with_corpus(self.original)
        if 'corpus-gitlink-result' in ci_preflight._jobs(self.actual):
            self.assertEqual(self.workflow, self.actual)
        else:
            self.assertEqual(self.original, self.actual)

    def replace_job(self, workflow: str, job: str, before: str, after: str) -> str:
        body = ci_preflight._jobs(workflow)[job]
        self.assertEqual(body.count(before), 1, (job, before))
        changed = in_job(workflow, job, before, after)
        self.assertNotEqual(changed, workflow)
        self.assertEqual(ci_preflight._jobs(changed)[job], body.replace(before, after, 1))
        return changed

    def replace_step(self, name: str, before: str, after: str) -> str:
        body = ci_preflight._jobs(self.workflow)['corpus-gitlink-result']
        step = ci_preflight._step_named(ci_preflight._steps(body), '      - name: ' + name)
        self.assertEqual(step.count(before), 1, (name, before))
        return self.replace_job(self.workflow, 'corpus-gitlink-result', step,
                                step.replace(before, after, 1))

    def without_corpus(self, workflow: str) -> str:
        if 'corpus-gitlink-result' not in ci_preflight._jobs(workflow):
            return workflow
        # Recover OLD from the literal hunk after the workflow rollout too.
        self.assertEqual(workflow.count(CORPUS_JOB), 1)
        workflow = workflow.replace(CORPUS_JOB, '', 1)
        workflow = self.replace_job(
            workflow, 'result',
            '    needs: [admit, preflight, build-x86-gcc133, corpus-gitlink-result, legs]\n',
            '    needs: [admit, preflight, build-x86-gcc133, legs]\n')
        for addition in (
                '          CORPUS_RESULT: ${{ needs.corpus-gitlink-result.result }}\n',
                '          test "$CORPUS_RESULT" = success || fail "corpus validation did not pass"\n'):
            workflow = self.replace_job(workflow, 'result', addition, '')
        return workflow

    def with_corpus(self, workflow: str) -> str:
        self.assertNotIn('corpus-gitlink-result', ci_preflight._jobs(workflow))
        marker = '  legs:\n'
        self.assertEqual(workflow.count(marker), 1)
        changed = workflow.replace(marker, CORPUS_JOB + marker, 1)
        self.assertEqual(changed.replace(CORPUS_JOB, '', 1), workflow)
        changed = self.replace_job(
            changed, 'result',
            '    needs: [admit, preflight, build-x86-gcc133, legs]\n',
            '    needs: [admit, preflight, build-x86-gcc133, corpus-gitlink-result, legs]\n')
        # Literal result hunk; these anchors are independent of checker templates.
        for anchor, addition in (
                ('          X86_RESULT: ${{ needs.build-x86-gcc133.result }}\n',
                 '          CORPUS_RESULT: ${{ needs.corpus-gitlink-result.result }}\n'),
                ('          test "$X86_RESULT" = success || fail "native x86 GNU 13.3.0 build did not pass"\n',
                 '          test "$CORPUS_RESULT" = success || fail "corpus validation did not pass"\n')):
            changed = self.replace_job(changed, 'result', anchor, anchor + addition)
        return changed

    def admission_findings(self, workflow: str) -> list[str]:
        with tempfile.TemporaryDirectory() as fetched:
            directory = Path(fetched)
            copies = ('.github/actionlint.yaml', '.github/actions/setup-live-curl/action.yml')
            for relative in copies:
                target = directory / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, target)
            target_dir = directory / '.github/workflows'
            target_dir.mkdir(parents=True, exist_ok=True)
            for path in sorted((ROOT / '.github/workflows').iterdir()):
                if path.suffix in ('.yml', '.yaml'):
                    text = workflow if path.name == 'gke-pr.yml' else path.read_text(encoding='utf-8')
                    (target_dir / path.name).write_text(text, encoding='utf-8')
            return ci_preflight.admit_rule_findings(directory, run_actionlint=False)

    def assert_admitted(self, workflow: str) -> None:
        self.assertEqual(ci_preflight.gke_admit_workflow_findings(workflow), [])
        self.assertEqual(ci_preflight.runner_findings({'gke-pr.yml': workflow}), [])
        self.assertEqual(self.admission_findings(workflow), [])

    def assert_refused(self, changed: str, finding: str, *, baseline: str | None = None) -> None:
        self.assert_admitted(self.workflow if baseline is None else baseline)
        self.assertNotEqual(changed, self.workflow if baseline is None else baseline)
        self.assertIn(finding, ci_preflight.gke_admit_workflow_findings(changed))
        self.assertIn(finding, self.admission_findings(changed))

    def test_real_workflow_is_admitted(self) -> None:
        self.assert_admitted(self.actual)

    def test_without_corpus_job_is_still_admitted(self) -> None:
        self.assert_admitted(self.original)

    def test_reviewed_corpus_hunk_is_admitted(self) -> None:
        self.assert_admitted(self.workflow)

    def test_all_four_arm_and_corpus_combinations_are_admitted(self) -> None:
        # Reuse the independently reviewed arm fixture's OLD recovery and NEW
        # construction, without importing a TestCase into module discovery.
        arm = preflight_fixtures.GkeArmAdmissionVariantTests()
        arm.setUp()
        for has_arm, source in ((False, arm.original), (True, arm.workflow)):
            old = self.without_corpus(source)
            for has_corpus, workflow in ((False, old), (True, self.with_corpus(old))):
                with self.subTest(arm=has_arm, corpus=has_corpus):
                    jobs = ci_preflight._jobs(workflow)
                    self.assertEqual('build-arm' in jobs, has_arm)
                    self.assertEqual('corpus-gitlink-result' in jobs, has_corpus)
                    self.assert_admitted(workflow)

    def test_literal_hunk_recovers_old_before_and_after_rollout(self) -> None:
        for source in (self.original, self.workflow):
            with self.subTest(corpus='corpus-gitlink-result' in ci_preflight._jobs(source)):
                old = self.without_corpus(source)
                self.assertEqual(old, self.original)
                self.assertEqual(self.with_corpus(old), self.workflow)

    def test_corpus_selector_only_selects_its_workflow_and_defined_job(self) -> None:
        self.assertTrue(ci_preflight._gke_pr_corpus_variant(
            'gke-pr.yml', ci_preflight._jobs(self.workflow)))
        self.assertFalse(ci_preflight._gke_pr_corpus_variant(
            'gke-pr.yml', ci_preflight._jobs(self.original)))
        self.assertFalse(ci_preflight._gke_pr_corpus_variant(
            'ci-post.yml', ci_preflight._jobs(self.workflow)))

    def test_job_present_with_old_result_needs_is_refused(self) -> None:
        changed = self.replace_job(
            self.workflow, 'result',
            '    needs: [admit, preflight, build-x86-gcc133, corpus-gitlink-result, legs]\n',
            '    needs: [admit, preflight, build-x86-gcc133, legs]\n')
        self.assert_refused(
            changed, 'gke-pr.yml result must need admit, preflight, build-x86-gcc133, '
            'corpus-gitlink-result and legs')
        self.assertIn(
            'gke-pr.yml result must require admission, preflight, native x86, '
            'corpus validation and consistent proof results',
            ci_preflight.gke_admit_workflow_findings(changed))

    def test_job_present_with_old_result_template_is_refused(self) -> None:
        changed = self.workflow
        for line in (
                '          CORPUS_RESULT: ${{ needs.corpus-gitlink-result.result }}\n',
                '          test "$CORPUS_RESULT" = success || fail "corpus validation did not pass"\n'):
            changed = self.replace_job(changed, 'result', line, '')
        self.assert_refused(
            changed, 'gke-pr.yml result must require admission, preflight, native x86, '
            'corpus validation and consistent proof results')

    def test_job_absent_with_corpus_result_needs_is_refused(self) -> None:
        changed = self.replace_job(
            self.original, 'result',
            '    needs: [admit, preflight, build-x86-gcc133, legs]\n',
            '    needs: [admit, preflight, build-x86-gcc133, corpus-gitlink-result, legs]\n')
        self.assert_refused(
            changed, 'gke-pr.yml result must need admit, preflight, build-x86-gcc133 and legs',
            baseline=self.original)

    def test_job_absent_with_corpus_result_environment_is_refused(self) -> None:
        anchor = '          X86_RESULT: ${{ needs.build-x86-gcc133.result }}\n'
        changed = self.replace_job(
            self.original, 'result', anchor,
            anchor + '          CORPUS_RESULT: ${{ needs.corpus-gitlink-result.result }}\n')
        self.assert_refused(
            changed, 'gke-pr.yml result must require admission, preflight, native x86 '
            'and consistent proof results', baseline=self.original)

    def test_job_absent_with_corpus_result_check_is_refused(self) -> None:
        anchor = '          test "$X86_RESULT" = success || fail "native x86 GNU 13.3.0 build did not pass"\n'
        changed = self.replace_job(
            self.original, 'result', anchor,
            anchor + '          test "$CORPUS_RESULT" = success || fail "corpus validation did not pass"\n')
        self.assert_refused(
            changed, 'gke-pr.yml result must require admission, preflight, native x86 '
            'and consistent proof results', baseline=self.original)

    def test_corpus_result_environment_is_required(self) -> None:
        changed = self.replace_job(
            self.workflow, 'result',
            '          CORPUS_RESULT: ${{ needs.corpus-gitlink-result.result }}\n', '')
        self.assert_refused(
            changed, 'gke-pr.yml result must require admission, preflight, native x86, '
            'corpus validation and consistent proof results')

    def test_corpus_result_check_is_required(self) -> None:
        changed = self.replace_job(
            self.workflow, 'result',
            '          test "$CORPUS_RESULT" = success || fail "corpus validation did not pass"\n', '')
        self.assert_refused(
            changed, 'gke-pr.yml result must require admission, preflight, native x86, '
            'corpus validation and consistent proof results')

    def test_step_order_change_is_refused(self) -> None:
        steps = ci_preflight._steps(ci_preflight._jobs(self.workflow)['corpus-gitlink-result'])
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '\n'.join(steps), '\n'.join((steps[1], steps[0], *steps[2:])))
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its four ordered steps')

    def test_fifth_step_is_refused(self) -> None:
        changed = self.replace_job(
            self.workflow, 'corpus-gitlink-result',
            '      - name: Set up Python\n',
            '      - name: Extra command\n        run: "true"\n      - name: Set up Python\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its four ordered steps')

    def test_removed_step_is_refused(self) -> None:
        step = ci_preflight._step_named(
            ci_preflight._steps(ci_preflight._jobs(self.workflow)['corpus-gitlink-result']),
            '      - name: Set up Python')
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result', step, '')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its four ordered steps')

    def test_checker_from_head_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    'python3 base/scripts/', 'python3 head/scripts/')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_repository_dot_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit', '--repo head', '--repo .')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_api_override_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    '--head "$HEAD_SHA"', '--head "$HEAD_SHA" --api-url https://example.invalid')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_masked_checker_failure_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    '--head "$HEAD_SHA"', '--head "$HEAD_SHA" || true')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_checker_comparing_base_to_itself_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    '--head "$HEAD_SHA"', '--head "$BASE_SHA"')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_step_if_is_refused(self) -> None:
        changed = self.replace_step(
            'Require validation of the proposed corpus commit', '        env:\n',
            '        if: always()\n        env:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_job_continue_on_error_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    steps:\n', '    continue-on-error: true\n    steps:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep only its admitted job keys')

    def test_step_continue_on_error_is_refused(self) -> None:
        changed = self.replace_step(
            'Require validation of the proposed corpus commit', '        env:\n',
            '        continue-on-error: true\n        env:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_base_checkout_of_head_is_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree', 'outputs.base_sha', 'outputs.head_sha')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_head_checkout_of_base_is_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head', 'outputs.head_sha', 'outputs.base_sha')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_head_checkout_without_full_history_is_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head', '          fetch-depth: 0\n', '')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_shallow_head_checkout_is_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head', 'fetch-depth: 0', 'fetch-depth: 1')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_fetch_depth_on_base_checkout_is_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree', '          path: base\n',
                                    '          path: base\n          fetch-depth: 0\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_persisted_base_credentials_are_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree', 'persist-credentials: false', 'persist-credentials: true')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_persisted_head_credentials_are_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head', 'persist-credentials: false', 'persist-credentials: true')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_changed_base_path_is_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree', 'path: base', 'path: other')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_changed_head_path_is_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head', 'path: head', 'path: other')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_changed_base_repository_is_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree', 'repository: ${{ github.repository }}',
                                    'repository: example/other')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_changed_head_repository_is_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head', 'repository: ${{ github.repository }}',
                                    'repository: example/other')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_secret_token_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    'GH_TOKEN: ${{ github.token }}', 'GH_TOKEN: ${{ secrets.CORPUS_TOKEN }}')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_missing_token_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    '          GH_TOKEN: ${{ github.token }}\n', '')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_extra_environment_key_is_refused(self) -> None:
        changed = self.replace_step(
            'Require validation of the proposed corpus commit', '        env:\n',
            '        env:\n          API_URL: https://example.invalid\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_changed_base_environment_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    'BASE_SHA: ${{ needs.admit.outputs.base_sha }}',
                                    'BASE_SHA: ${{ needs.admit.outputs.head_sha }}')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_changed_head_environment_is_refused(self) -> None:
        changed = self.replace_step('Require validation of the proposed corpus commit',
                                    'HEAD_SHA: ${{ needs.admit.outputs.head_sha }}',
                                    'HEAD_SHA: ${{ needs.admit.outputs.base_sha }}')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must run only the base checker over head data')

    def test_changed_job_if_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   "    if: ${{ needs.admit.outputs.allowed == 'true' }}\n",
                                   '    if: always()\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must follow admission for every path class')

    def test_changed_job_needs_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    needs: [admit]\n', '    needs: [preflight]\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must follow admission for every path class')

    def test_changed_runner_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    runs-on: ubuntu-24.04\n', '    runs-on: ubuntu-latest\n')
        self.assert_refused(changed, 'gke-pr.yml job corpus-gitlink-result must use ubuntu-24.04')
        self.assertIn('gke-pr.yml job corpus-gitlink-result must run on ubuntu-24.04',
                      ci_preflight.runner_findings({'gke-pr.yml': changed}))

    def test_widened_permissions_are_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    permissions: {contents: read}\n', '    permissions: {contents: write}\n')
        self.assert_refused(changed, 'gke-pr.yml job corpus-gitlink-result must keep its read-only permissions')

    def test_changed_base_action_pin_is_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree',
                                    '11d5960a326750d5838078e36cf38b85af677262', 'v4')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_changed_head_action_pin_is_refused(self) -> None:
        changed = self.replace_step('Checkout the admitted head',
                                    '11d5960a326750d5838078e36cf38b85af677262', 'v4')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact head checkout')

    def test_changed_python_pin_is_refused(self) -> None:
        changed = self.replace_step('Set up Python', 'a26af69be951a213d495a4c3e4e4022e16d87065', 'v5')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must use the same pinned Python setup as build-debug')

    def test_changed_python_version_is_refused(self) -> None:
        changed = self.replace_step('Set up Python', "python-version: '3.12'", "python-version: '3.13'")
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must use the same pinned Python setup as build-debug')

    def test_matching_python_drift_is_refused(self) -> None:
        changed = self.replace_step('Set up Python', "python-version: '3.12'", "python-version: '3.13'")
        changed = self.replace_job(changed, 'build-debug',
                                   "python-version: '3.12'", "python-version: '3.13'")
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must use the same pinned Python setup as build-debug')

    def test_debug_python_disagreement_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'build-debug',
                                   "python-version: '3.12'", "python-version: '3.13'")
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must use the same pinned Python setup as build-debug')

    def test_step_if_on_checkout_is_refused(self) -> None:
        changed = self.replace_step('Checkout the base tree', '        with:\n',
                                    '        if: always()\n        with:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its exact base checkout')

    def test_step_if_on_python_is_refused(self) -> None:
        changed = self.replace_step('Set up Python', '        with:\n',
                                    '        if: always()\n        with:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must use the same pinned Python setup as build-debug')

    def test_extra_job_key_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    steps:\n', '    env: {PYTHONPATH: head}\n    steps:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep only its admitted job keys')

    def test_custom_check_name_is_refused(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    steps:\n', '    name: Different check\n    steps:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep its default check name')

    def test_corpus_command_table_enforces_the_job_timeout_rule(self) -> None:
        changed = self.replace_job(self.workflow, 'corpus-gitlink-result',
                                   '    steps:\n', '    timeout-minutes: 10\n    steps:\n')
        self.assert_refused(changed, 'gke-pr.yml corpus-gitlink-result must keep only its admitted job keys')
        self.assertIn('gke-pr.yml job corpus-gitlink-result must declare no timeout-minutes (owner #827)',
                      ci_preflight.runner_findings({'gke-pr.yml': changed}))


class CorpusCommandCensusTests(unittest.TestCase):
    def test_corpus_modules_have_executable_command_rows(self) -> None:
        modules = (
            'scripts/test_validate_corpus_sha.py',
            'scripts/test_check_corpus_gitlink_result.py',
            'scripts/test_write_corpus_file_digests.py',
            'scripts/test_ci_corpus_variant.py',
        )
        for tier in ('all', 'fast'):
            rows = ci_preflight.check_commands(ROOT, env={}, tier=tier)
            for relative in modules:
                with self.subTest(tier=tier, module=relative):
                    # A mention in prose or a stage label is not an executable row.
                    self.assertIn([sys.executable, str(ROOT / relative)],
                                  [list(row[1]) for row in rows])

    def test_every_script_test_is_named_or_explicitly_exempt(self) -> None:
        scripts = {path.relative_to(ROOT).as_posix()
                   for path in (ROOT / 'scripts').glob('test_*.py')}
        named = {
            relative for row in ci_preflight.check_commands(ROOT, env={})
            for relative in scripts if str(ROOT / relative) in row[1]
        }
        exemption_rows = COMMAND_CENSUS_EXEMPTIONS
        if not sys.platform.startswith('linux'):
            exemption_rows += NON_LINUX_COMMAND_CENSUS_EXEMPTIONS
        exemptions = dict(exemption_rows)
        self.assertEqual(len(exemptions), len(exemption_rows),
                         'duplicate census exemptions')
        self.assertTrue(all(reason.strip() for reason in exemptions.values()))
        self.assertEqual(set(exemptions) - scripts, set(), 'exemptions must name existing modules')
        self.assertEqual(set(exemptions) & named, set(), 'remove exemptions once a row names a module')
        self.assertEqual(scripts - named - set(exemptions), set(), 'script tests lack command rows')


if __name__ == '__main__':
    unittest.main()
