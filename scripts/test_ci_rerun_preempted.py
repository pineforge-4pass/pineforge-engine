#!/usr/bin/env python3
"""Runner-loss controller tests, written before the engine adaptation.

Real captured evidence is trimmed under fixtures/ci_rerun_preempted. Every
API request is fake; transport tests mock subprocess.run, never invoking gh.
"""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
from io import StringIO
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / 'ci'))
import rerun_preempted as controller  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / 'fixtures/ci_rerun_preempted'
CAPTURED = json.loads((FIXTURES / 'captured.json').read_text())
REPO = CAPTURED['run-41588378688.json']['repository']['full_name']
CUT_JOB = CAPTURED['run-41588378688-a1-jobs.json']['jobs'][0]
ORDINARY_JOB = CAPTURED['run-41515706477-jobs.json']['jobs'][0]
ESCAPE_FLAG = '--allow-escape-sequences'
FAKE_BODY = 'PRIVATE_RESPONSE_BODY'
FAKE_QUERY = '?signature=PRIVATE_QUERY'
FAKE_SIGNED_URL = 'https://signed.example.invalid/job.log' + FAKE_QUERY
UNTRUSTED_OUTPUT = '\n'.join((REPO, FAKE_QUERY, FAKE_SIGNED_URL, FAKE_BODY))


def completed_wave_run():
    run = deepcopy(CAPTURED['run-41588378688.json'])
    # The completed attempt-1 capture was unavailable. For positive process()
    # rows change ONLY status/conclusion; retain every captured identity field.
    run.update(status='completed', conclusion='failure')
    return run


class FakeAPI:
    """Strict fixture routes and fresh snapshots; record every GET and POST."""

    def __init__(self, *, run=None, job_pages=None, current=None):
        self.run = deepcopy(completed_wave_run() if run is None else run)
        self.current = deepcopy(self.run if current is None else current)
        self.endpoint = f'repos/{REPO}/actions/runs/{self.run["id"]}'
        self.jobs_path = f'{self.endpoint}/attempts/{self.run["run_attempt"]}/jobs?per_page=100'
        self.job_pages = deepcopy([{'jobs': [CUT_JOB]}] if job_pages is None else job_pages)
        self.calls = []
        self.reads = 0
        self.annotation_error = None
        self.log_error = None
        self.logs = {
            f'repos/{REPO}/actions/jobs/{job["id"]}/logs':
                (FIXTURES / f'job-{job["id"]}.log').read_text()
            for job in (CUT_JOB, ORDINARY_JOB)
        }
        self.annotations = {
            f'repos/{REPO}/check-runs/{job["id"]}/annotations?per_page=100':
                [CAPTURED[f'check-{job["id"]}-annotations.json']]
            for job in (CUT_JOB, ORDINARY_JOB)
        }

    def get(self, path):
        self.calls.append(('GET', path))
        if path != self.endpoint:
            raise AssertionError('unexpected run lookup')
        self.reads += 1
        return deepcopy(self.run if self.reads == 1 else self.current)

    def pages(self, path):
        self.calls.append(('PAGES', path))
        if path == self.jobs_path:
            return deepcopy(self.job_pages)
        if path not in self.annotations:
            raise AssertionError('unexpected paginated lookup')
        if self.annotation_error:
            raise self.annotation_error
        return deepcopy(self.annotations[path])

    def text(self, path):
        self.calls.append(('LOG', path))
        if self.log_error:
            raise self.log_error
        return self.logs[path]

    def post(self, path):
        self.calls.append(('POST', path))
        if path != f'{self.endpoint}/rerun-failed-jobs':
            raise AssertionError('unexpected mutation')


class LogTransportAPI(FakeAPI):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.transport = controller.GitHubAPI()

    def text(self, path):
        self.calls.append(('LOG', path))
        return self.transport.text(path)


class ControllerTests(unittest.TestCase):
    def decide(self, api, **kwargs):
        return controller.process(api, REPO, api.run['id'], **kwargs)

    def assert_no_post(self, api):
        self.assertEqual([call for call in api.calls if call[0] == 'POST'], [])

    def assert_refused(self, api, reason, **kwargs):
        result = self.decide(api, **kwargs)
        self.assertFalse(result['rerun'], result)
        self.assertEqual(result['reason'], reason, result)
        self.assert_no_post(api)
        return result

    def assert_diagnostic(self, stdout, stderr, *, method='GET',
                          route='repos/{repo}/actions/jobs/{id}/logs',
                          status='unknown', exit_code='unknown', reason):
        self.assertEqual(stdout.getvalue(), '')
        self.assertEqual(stderr.getvalue(),
                         f'GitHub API failure: method={method} path={route} '
                         f'http_status={status} gh_exit_code={exit_code} reason={reason}\n')
        self.assertEqual(len(stderr.getvalue().splitlines()), 1)
        for forbidden in (REPO, FAKE_QUERY, FAKE_SIGNED_URL, FAKE_BODY):
            self.assertNotIn(forbidden, stderr.getvalue())

    def test_wave6_cut_reruns_only_failed_jobs(self):
        api = FakeAPI()
        result = self.decide(api, expected_attempt=1)
        self.assertTrue(result['rerun'], result)
        self.assertEqual(result['reason'], 'all_failed_jobs_are_runner_loss')
        self.assertEqual(result['failed_job_ids'], [CUT_JOB['id']])
        self.assertEqual(api.calls, [
            ('GET', api.endpoint), ('PAGES', api.jobs_path),
            ('PAGES', next(iter(api.annotations))),
            ('LOG', next(iter(api.logs))), ('GET', api.endpoint),
            ('POST', f'{api.endpoint}/rerun-failed-jobs'),
        ])

    def test_plain_and_disk_sized_amd64_labels(self):
        for label in ('gke-8cpu-16gb-amd64', 'gke-32cpu-64gb-amd64',
                      'gke-32cpu-64gb-amd64-256gb', 'gke-16cpu-32gb-amd64-128gb'):
            with self.subTest(label=label):
                job = deepcopy(CUT_JOB)
                job['labels'] = [label]
                api = FakeAPI(job_pages=[{'jobs': [job]}])
                self.assertTrue(self.decide(api)['rerun'])

    def test_plain_failure_and_pr_run_refused(self):
        api = FakeAPI()
        self.assertEqual(controller._job_evidence(api, REPO, ORDINARY_JOB),
                         'independent_failure_diagnostic')
        self.assert_no_post(api)
        api = FakeAPI(run=CAPTURED['run-41515706477.json'])
        self.assert_refused(api, 'workflow_or_event_not_allowlisted')
        self.assertEqual(api.calls, [('GET', api.endpoint)])

    def test_mixed_failure_later_page_refused(self):
        for pages in ([{'jobs': [CUT_JOB, ORDINARY_JOB]}],
                      [{'jobs': [CUT_JOB]}, {'jobs': [ORDINARY_JOB]}]):
            with self.subTest(pages=len(pages)):
                api = FakeAPI(job_pages=pages)
                result = self.assert_refused(api, 'independent_failure_diagnostic')
                self.assertEqual(result['blocked_job_id'], ORDINARY_JOB['id'])

    def test_arm64_and_gkp_jobs_are_refused(self):
        for label in ('gke-16cpu-32gb-arm64', 'gke-32cpu-64gb-arm64-256gb',
                      'gkp-8cpu-16gb-amd64', 'gkp-32cpu-64gb-amd64-256gb',
                      'ubuntu-24.04', 'gke-32cpu-64gb-amd64-extra'):
            with self.subTest(label=label):
                job = deepcopy(CUT_JOB)
                job['labels'] = [label]
                self.assert_refused(FakeAPI(job_pages=[{'jobs': [job]}]),
                                    'not_a_fleet_amd64_job')

    def test_attempt_cap_stale_event_refused(self):
        self.assertEqual(controller.MAX_ATTEMPTS, 3)
        for attempt in (0, 3, 4):
            with self.subTest(attempt=attempt):
                run = completed_wave_run()
                run['run_attempt'] = attempt
                self.assert_refused(FakeAPI(run=run), 'attempt_limit')
        self.assert_refused(FakeAPI(), 'stale_completion_event', expected_attempt=2)

    def test_foreign_workflow_event_refused(self):
        mutations = [({'event': event}, 'workflow_or_event_not_allowlisted')
                     for event in ('workflow_dispatch', 'pull_request_target',
                                   'pull_request', 'workflow_call', 'repository_dispatch')]
        mutations += [({'path': f'.github/workflows/{name}.yml'},
                       'workflow_or_event_not_allowlisted')
                      for name in ('ci', 'native-live', 'gke-pr', 'release', 'signing',
                                   'publish', 'deploy', 'promote-baseline', 'docs-post')]
        mutations += [({'head_branch': 'topic'}, 'not_the_primary_branch'),
                      ({'repository': {'full_name': 'foreign/engine'}}, 'repository_mismatch'),
                      ({'status': 'in_progress', 'conclusion': None}, 'not_a_completed_failed_run'),
                      ({'conclusion': 'success'}, 'not_a_completed_failed_run')]
        for mutation, reason in mutations:
            with self.subTest(mutation=mutation):
                run = completed_wave_run()
                run.update(mutation)
                self.assert_refused(FakeAPI(run=run), reason)

    def test_changed_run_is_refused(self):
        for mutation in ({'run_attempt': 2}, {'status': 'in_progress'},
                         {'head_sha': '0' * 40}, {'event': 'schedule'},
                         {'id': 1}, {'head_branch': 'topic'}):
            with self.subTest(mutation=mutation):
                current = completed_wave_run()
                current.update(mutation)
                api = FakeAPI(current=current)
                self.assert_refused(api, 'run_changed_during_classification')
                self.assertEqual(api.reads, 2)

    def test_missing_loss_proof_refused(self):
        api = FakeAPI()
        api.logs[next(iter(api.logs))] = ''
        self.assert_refused(api, 'no_runner_loss_evidence')
        api = FakeAPI()
        api.logs[next(iter(api.logs))] += api.logs[list(api.logs)[1]]
        self.assert_refused(api, 'independent_failure_diagnostic')
        api = FakeAPI()
        api.log_error = RuntimeError('untrusted response body must stay private')
        result = self.assert_refused(api, 'lookup_or_request_failed')
        self.assertNotIn('untrusted response body', json.dumps(result))
        for mutation, reason in (
                ({'steps': [{'conclusion': 'failure'}, {'conclusion': 'failure'}]},
                 'multiple_failed_steps'),
                ({'check_run_url': 'https://api.github.com/repos/foreign/engine/check-runs/1'},
                 'invalid_check_run_identity')):
            with self.subTest(reason=reason):
                job = deepcopy(CUT_JOB)
                job.update(mutation)
                self.assert_refused(FakeAPI(job_pages=[{'jobs': [job]}]), reason)

    def test_incomplete_ambiguous_jobs_refused(self):
        self.assert_refused(FakeAPI(job_pages=[]), 'missing_or_incomplete_jobs')
        for mutation, reason in (
                ({'status': 'in_progress'}, 'missing_or_incomplete_jobs'),
                ({'conclusion': 'cancelled'}, 'ambiguous_unsuccessful_job'),
                ({'conclusion': 'timed_out'}, 'ambiguous_unsuccessful_job'),
                ({'conclusion': 'success'}, 'no_failed_jobs')):
            with self.subTest(mutation=mutation):
                job = deepcopy(CUT_JOB)
                job.update(mutation)
                self.assert_refused(FakeAPI(job_pages=[{'jobs': [job]}]), reason)

    def test_optional_annotations_and_dry_run(self):
        api = FakeAPI()
        api.annotation_error = PermissionError('checks not accessible')
        self.assertTrue(self.decide(api)['rerun'])
        api = FakeAPI()
        result = self.decide(api, dry_run=True)
        self.assertTrue(result['rerun'], result)
        self.assertTrue(result['dry_run'])
        self.assert_no_post(api)

    def test_escaped_logs_current_gh(self):
        api = LogTransportAPI()
        logs = next(iter(api.logs.values()))
        self.assertEqual(sum('\x1b' in line for line in logs.splitlines()), 5)

        def gh(argv, **kwargs):
            self.assertTrue(kwargs['capture_output'])
            self.assertTrue(kwargs['text'])
            if argv == ['gh', 'api', '--help']:
                return subprocess.CompletedProcess(argv, 0, 'Flags:\n  ' + ESCAPE_FLAG, '')
            if ESCAPE_FLAG not in argv:
                return subprocess.CompletedProcess(argv, 1, '',
                    'the response contains terminal escape sequences; pass '
                    '--allow-escape-sequences to output it anyway')
            return subprocess.CompletedProcess(argv, 0, logs, '')

        output = StringIO()
        with patch.object(controller.subprocess, 'run', side_effect=gh) as command, \
                redirect_stdout(output), redirect_stderr(output):
            result = self.decide(api)
        self.assertTrue(result['rerun'], result)
        self.assertEqual(command.call_count, 2)
        self.assertEqual(command.call_args_list[0].args[0], ['gh', 'api', '--help'])
        self.assertIn(ESCAPE_FLAG, command.call_args.args[0])
        self.assertEqual(output.getvalue(), '')

    def test_no_escape_flag_refuses_escapes(self):
        api = LogTransportAPI()
        logs = next(iter(api.logs.values()))

        def gh(argv, **kwargs):
            if argv == ['gh', 'api', '--help']:
                return subprocess.CompletedProcess(argv, 0, 'Flags:\n  --method string', '')
            return subprocess.CompletedProcess(argv, 1, logs + UNTRUSTED_OUTPUT,
                'the response contains terminal escape sequences; pass '
                '--allow-escape-sequences to output it anyway\n' + UNTRUSTED_OUTPUT)

        stdout, stderr = StringIO(), StringIO()
        with patch.object(controller.subprocess, 'run', side_effect=gh) as command, \
                redirect_stdout(stdout), redirect_stderr(stderr):
            result = self.assert_refused(api, 'lookup_or_request_failed')
        self.assertEqual(command.call_count, 2)
        for call in command.call_args_list:
            self.assertNotIn(ESCAPE_FLAG, call.args[0])
        self.assertEqual(result['error_type'], 'RuntimeError')
        self.assert_diagnostic(stdout, stderr, exit_code=1, reason='terminal_escape_sequences')
        self.assertNotIn('shutdown signal', json.dumps(result))

    def test_no_escape_flag_legacy_logs_ok(self):
        api = LogTransportAPI()
        logs = next(iter(api.logs.values()))

        def gh(argv, **kwargs):
            if argv == ['gh', 'api', '--help']:
                return subprocess.CompletedProcess(argv, 0, 'Flags:\n  --method string', '')
            return subprocess.CompletedProcess(argv, 0, logs, '')

        output = StringIO()
        with patch.object(controller.subprocess, 'run', side_effect=gh) as command, \
                redirect_stdout(output), redirect_stderr(output):
            result = self.decide(api)
        self.assertTrue(result['rerun'], result)
        self.assertEqual(command.call_count, 2)
        for call in command.call_args_list:
            self.assertNotIn(ESCAPE_FLAG, call.args[0])
        self.assertEqual(output.getvalue(), '')

    def test_failed_help_probe_refuses_early(self):
        for failure in (
                subprocess.CompletedProcess([], 2, UNTRUSTED_OUTPUT, 'HTTP 503\n' + UNTRUSTED_OUTPUT),
                subprocess.TimeoutExpired(['gh', 'api', '--help'], 60,
                                          output=UNTRUSTED_OUTPUT, stderr=UNTRUSTED_OUTPUT)):
            with self.subTest(failure=type(failure).__name__):
                api = LogTransportAPI()
                stdout, stderr = StringIO(), StringIO()
                mock_kwargs = ({'side_effect': failure} if isinstance(failure, Exception)
                               else {'return_value': failure})
                with patch.object(controller.subprocess, 'run', **mock_kwargs) as command, \
                        redirect_stdout(stdout), redirect_stderr(stderr):
                    result = self.assert_refused(api, 'lookup_or_request_failed')
                self.assertEqual(result['error_type'], 'RuntimeError')
                command.assert_called_once_with(['gh', 'api', '--help'],
                    capture_output=True, text=True, timeout=60, check=False)
                if isinstance(failure, subprocess.TimeoutExpired):
                    self.assert_diagnostic(stdout, stderr, reason='timeout')
                else:
                    self.assert_diagnostic(stdout, stderr, exit_code=2, reason='feature_detection_failed')

    def test_help_probe_cached_both_ways(self):
        fake = FakeAPI()
        paths = list(fake.logs)
        for supported in (True, False):
            with self.subTest(supported=supported):
                def gh(argv, **kwargs):
                    if argv == ['gh', 'api', '--help']:
                        help_text = 'Flags:\n  ' + (ESCAPE_FLAG if supported else '--method string')
                        return subprocess.CompletedProcess(argv, 0, help_text, '')
                    path = next(arg for arg in argv if arg.startswith('repos/'))
                    return subprocess.CompletedProcess(argv, 0, fake.logs[path.split('?', 1)[0]], '')

                api = controller.GitHubAPI()
                output = StringIO()
                with patch.object(controller.subprocess, 'run', side_effect=gh) as command, \
                        redirect_stdout(output), redirect_stderr(output):
                    for path in (paths[0], paths[1], paths[0] + FAKE_QUERY):
                        self.assertEqual(api.text(path), fake.logs[path.split('?', 1)[0]])
                argv_rows = [call.args[0] for call in command.call_args_list]
                self.assertEqual(argv_rows.count(['gh', 'api', '--help']), 1)
                self.assertEqual(len(argv_rows), 4)
                self.assertNotIn(ESCAPE_FLAG, argv_rows[0])
                for argv in argv_rows[1:]:
                    self.assertEqual(ESCAPE_FLAG in argv, supported)
                self.assertEqual(output.getvalue(), '')

    def test_escape_flag_scoped_to_logs(self):
        fake = FakeAPI()
        annotations_path = next(iter(fake.annotations))
        log_path = next(iter(fake.logs))
        pages = fake.annotations[annotations_path] + [[]]
        responses = {
            fake.endpoint: json.dumps(fake.run),
            annotations_path: '\n'.join(json.dumps(page) for page in pages),
            log_path: fake.logs[log_path],
            f'{fake.endpoint}/rerun-failed-jobs': '',
        }

        def gh(argv, **kwargs):
            if argv == ['gh', 'api', '--help']:
                return subprocess.CompletedProcess(argv, 0, 'Flags:\n  ' + ESCAPE_FLAG, '')
            path = next(arg for arg in argv if arg.startswith('repos/'))
            return subprocess.CompletedProcess(argv, 0, responses[path], '')

        api = controller.GitHubAPI()
        with patch.object(controller.subprocess, 'run', side_effect=gh) as command:
            self.assertEqual(api.get(fake.endpoint), fake.run)
            self.assertEqual(api.pages(annotations_path), pages)
            self.assertEqual(api.text(log_path), fake.logs[log_path])
            api.post(f'{fake.endpoint}/rerun-failed-jobs')
        for call in command.call_args_list:
            argv = call.args[0]
            self.assertEqual(ESCAPE_FLAG in argv, log_path in argv)
            self.assertTrue(call.kwargs['capture_output'])
        self.assertEqual(command.call_count, 5)
        self.assertEqual(command.call_args_list[2].args[0], ['gh', 'api', '--help'])
        self.assertIn('--paginate', command.call_args_list[1].args[0])

    def test_failed_calls_one_diagnostic_each(self):
        fake = FakeAPI()
        cases = (
            ('GET', fake.endpoint, 'repos/{repo}/actions/runs/{id}', 403, PermissionError),
            ('GET', fake.jobs_path, 'repos/{repo}/actions/runs/{id}/attempts/{id}/jobs', 502, RuntimeError),
            ('GET', next(iter(fake.logs)), 'repos/{repo}/actions/jobs/{id}/logs', 404, RuntimeError),
            ('GET', next(iter(fake.annotations)), 'repos/{repo}/check-runs/{id}/annotations', 404, PermissionError),
            ('POST', fake.endpoint + '/rerun-failed-jobs',
             'repos/{repo}/actions/runs/{id}/rerun-failed-jobs', 500, RuntimeError),
            ('PATCH', f'repos/{REPO}/unknown/1', 'unknown', 401, RuntimeError),
        )
        for method, path, template, status, exception in cases:
            with self.subTest(method=method, route=template):
                path = path.split('?', 1)[0] + FAKE_QUERY

                def gh(argv, **kwargs):
                    if argv == ['gh', 'api', '--help']:
                        return subprocess.CompletedProcess(argv, 0, 'Flags:\n  ' + ESCAPE_FLAG, '')
                    return subprocess.CompletedProcess(argv, 1, UNTRUSTED_OUTPUT,
                                                       f'HTTP {status}\n' + UNTRUSTED_OUTPUT)

                stdout, stderr = StringIO(), StringIO()
                with patch.object(controller.subprocess, 'run', side_effect=gh), \
                        redirect_stdout(stdout), redirect_stderr(stderr):
                    with self.assertRaises(exception):
                        controller.GitHubAPI()._call(path, method=method)
                self.assert_diagnostic(stdout, stderr,
                    method=method if method in ('GET', 'POST') else 'unknown',
                    route=template, status=status, exit_code=1, reason='request_failed')

    def test_timeout_missing_gh_one_diagnostic(self):
        for error, reason in (
                (subprocess.TimeoutExpired(['gh', 'api', FAKE_SIGNED_URL], 60,
                                           output=UNTRUSTED_OUTPUT, stderr=UNTRUSTED_OUTPUT), 'timeout'),
                (OSError(UNTRUSTED_OUTPUT), 'process_error')):
            with self.subTest(reason=reason):
                api = LogTransportAPI()
                # Cache successful detection so the fault is in the log call.
                results = [subprocess.CompletedProcess([], 0, 'Flags:\n  ' + ESCAPE_FLAG, ''), error]
                stdout, stderr = StringIO(), StringIO()
                with patch.object(controller.subprocess, 'run', side_effect=results) as command, \
                        redirect_stdout(stdout), redirect_stderr(stderr):
                    result = self.assert_refused(api, 'lookup_or_request_failed')
                self.assertEqual(result['error_type'], 'RuntimeError')
                self.assertEqual(command.call_count, 2)
                self.assert_diagnostic(stdout, stderr, reason=reason)

    def test_allowlist_and_trusted_checkout(self):
        self.assertEqual(controller.PRIMARY_BRANCH, 'main')
        self.assertEqual(controller.ALLOWED_WORKFLOWS, {
            '.github/workflows/ci-post.yml': frozenset({'push', 'schedule'}),
        })
        workflow = (ROOT / '.github/workflows/rerun-preempted.yml').read_text()
        trigger = workflow.split('\npermissions:', 1)[0]
        self.assertEqual(re.findall(r'^  (\w+):', trigger, re.M), ['workflow_run'])
        self.assertEqual(re.findall(r'^      - (.+)$', trigger, re.M), ['CI post-merge'])
        self.assertIn('types: [completed]', trigger)
        self.assertIn('runs-on: ubuntu-24.04', workflow)
        self.assertNotIn('pf-gcp-pr', workflow)
        self.assertIn('''    steps:
      - name: Require controller tools
        shell: bash
        run: |
          set -euo pipefail
          gh --version
          python3 --version
          gh_api_help=$(gh api --help)
          grep -q -- '--allow-escape-sequences' <<< "$gh_api_help" || {
            echo "gh api lacks --allow-escape-sequences; job logs cannot be read" >&2
            exit 1
          }
      - name: Load the controller from its trusted default-branch commit
''', workflow)
        self.assertIn('ref: ${{ github.workflow_sha }}', workflow)
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('--expected-attempt "$SOURCE_ATTEMPT"', workflow)
        self.assertIn('        run: |\n          gh --version\n'
                      '          python3 scripts/ci/rerun_preempted.py', workflow)
        self.assertIn('The controller must load this file from its own trusted default-branch commit.',
                      controller.__doc__)


if __name__ == '__main__':
    unittest.main()
