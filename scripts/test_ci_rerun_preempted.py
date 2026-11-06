#!/usr/bin/env python3
"""Runner-loss controller tests, written before the engine adaptation.

Real captured evidence is trimmed under fixtures/ci_rerun_preempted. Every
API request is fake; transport tests mock subprocess.run, never invoking gh.
"""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
from io import StringIO
import base64
import json
from pathlib import Path
import re
import subprocess
import sys
import textwrap
from types import ModuleType
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
UNAVAILABLE_LOG = 'attempt-1 log unavailable after the attempt-2 re-run'


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
        self.job_pages_after = []
        self.job_reads = 0
        self.calls = []
        self.reads = 0
        self.annotation_error = None
        self.log_error = None
        # The older captures omitted their workflow. Keep their test bodies
        # and evidence unchanged; these two static names model that lookup.
        self.workflow = ('jobs:\n  cut:\n    name: ' + CUT_JOB['name']
                         + '\n  ordinary:\n    name: ' + ORDINARY_JOB['name'] + '\n')
        self.file_calls = []
        self.file_error = None
        self.workflows = {}
        self.file_path = (f'repos/{REPO}/contents/{self.run["path"]}'
                          f'?ref={self.run["head_sha"]}')
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

    def file_route(self, path):
        repo = self.run['repository']['full_name']
        return f'repos/{repo}/contents/{path}?ref={self.run["head_sha"]}'

    def get(self, path):
        files = {self.file_path: (self.run['path'], self.workflow)}
        files.update({self.file_route(name): (name, source)
                      for name, source in self.workflows.items()})
        if path in files:
            self.file_calls.append(path)
            if self.file_error:
                raise self.file_error
            name, source = files[path]
            if isinstance(source, Exception):
                raise source
            return {'type': 'file', 'path': name,
                    'encoding': 'base64',
                    'content': base64.b64encode(source.encode()).decode()}
        self.calls.append(('GET', path))
        if path != self.endpoint:
            raise AssertionError('unexpected run lookup')
        self.reads += 1
        return deepcopy(self.run if self.reads == 1 else self.current)

    def pages(self, path):
        self.calls.append(('PAGES', path))
        if path == self.jobs_path:
            self.job_reads += 1
            pages = (self.job_pages_after.pop(0)
                     if self.job_reads > 1 and self.job_pages_after else self.job_pages)
            return deepcopy(pages)
        if path not in self.annotations:
            raise AssertionError('unexpected paginated lookup')
        if self.annotation_error:
            raise self.annotation_error
        return deepcopy(self.annotations[path])

    def text(self, path):
        self.calls.append(('LOG', path))
        if self.log_error:
            raise self.log_error
        value = self.logs[path]
        if isinstance(value, Exception):
            raise value
        return value

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
                      'gke-32cpu-64gb-amd64-256gb',
                      'gke-ondemand-32cpu-64gb-amd64',
                      'gke-ondemand-32cpu-64gb-amd64-256gb',
                      'gke-16cpu-32gb-amd64-128gb'):
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
        # The PR workflow is now allowlisted; this old capture lacks PR identity fields.
        self.assert_refused(api, 'pull_request_metadata_unavailable')
        self.assertEqual(api.calls, [('GET', api.endpoint)])

    def test_pr_superseded_head_refused_without_post(self):
        class PRAPI(FakeAPI):
            def __init__(self, run, pull_request, history):
                super().__init__(run=run)
                self.pull_request = deepcopy(pull_request)
                self.history = deepcopy(history)

            def get(self, path):
                if path == f'{self.endpoint.rsplit("/actions", 1)[0]}/pulls/{self.pull_request["number"]}':
                    self.calls.append(('GET', path))
                    return deepcopy(self.pull_request)
                return super().get(path)

            def pages(self, path):
                if path == (f'{self.endpoint.rsplit("/actions", 1)[0]}/actions/workflows/'
                            f'gke-pr.yml/runs?event={controller.PR_EVENT}&branch=feature&per_page=100'):
                    self.calls.append(('PAGES', path))
                    return deepcopy(self.history)
                return super().pages(path)

        old_sha, new_sha = '1' * 40, '2' * 40
        run = completed_wave_run()
        run.update(path=controller.PR_WORKFLOW, event=controller.PR_EVENT,
                   head_branch='feature', head_repository={'full_name': REPO},
                   pull_requests=[{'number': 736}], head_sha=old_sha,
                   run_number=10, created_at='2026-11-05T00:00:00Z')
        current = deepcopy(run)
        newer = deepcopy(run)
        newer.update(id=run['id'] + 1, run_number=11, head_sha=new_sha,
                     created_at='2026-11-05T00:01:00Z')

        def pull_request(state, sha):
            return {'number': 736, 'state': state,
                    'head': {'sha': sha, 'repo': {'full_name': REPO, 'fork': False}}}

        cases = (
            ('closed', pull_request('closed', old_sha),
             [{'total_count': 1, 'workflow_runs': [current]}]),
            ('head_changed', pull_request('open', new_sha),
             [{'total_count': 1, 'workflow_runs': [current]}]),
            ('newer_run_head_match', pull_request('open', old_sha),
             [{'total_count': 2, 'workflow_runs': [newer, current]}]),
            ('newer_run_head_new', pull_request('open', new_sha),
             [{'total_count': 2, 'workflow_runs': [newer, current]}]),
        )
        for name, pr, history in cases:
            with self.subTest(case=name):
                api = PRAPI(run, pr, history)
                result = self.decide(api)
                self.assertFalse(result['rerun'], result)
                self.assertEqual(result['reason'], 'superseded_pull_request_head', result)
                self.assert_no_post(api)

    def test_mixed_failure_later_page_refused(self):
        for pages in ([{'jobs': [CUT_JOB, ORDINARY_JOB]}],
                      [{'jobs': [CUT_JOB]}, {'jobs': [ORDINARY_JOB]}]):
            with self.subTest(pages=len(pages)):
                api = FakeAPI(job_pages=pages)
                result = self.assert_refused(api, 'independent_failure_diagnostic')
                self.assertEqual(result['blocked_job_id'], ORDINARY_JOB['id'])

    def test_arm64_and_gkp_jobs_are_refused(self):
        for label in ('gke-16cpu-32gb-arm64', 'gke-32cpu-64gb-arm64-256gb',
                      'gke-ondemand-32cpu-64gb-arm64',
                      'gke-ondemand-32cpu-64gb-amd64-extra',
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

    def test_incomplete_jobs_poll_before_deciding(self):
        pending = deepcopy(CUT_JOB)
        pending['status'] = 'in_progress'
        api = FakeAPI(job_pages=[{'jobs': [pending]}])
        api.job_pages_after = [[{'jobs': [CUT_JOB]}]]
        with patch.object(controller, '_sleep'):
            result = self.decide(api)
        self.assertTrue(result['rerun'], result)
        self.assertIn('waited_seconds', result)
        for mutation, reason in (
                ({'status': 'in_progress'}, 'missing_or_incomplete_jobs'),
                ({'conclusion': 'cancelled'}, 'ambiguous_unsuccessful_job'),
                ({'conclusion': 'timed_out'}, 'ambiguous_unsuccessful_job'),
                ({'conclusion': 'success'}, 'no_failed_jobs')):
            with self.subTest(mutation=mutation):
                job = deepcopy(CUT_JOB)
                job.update(mutation)
                api = FakeAPI(job_pages=[{'jobs': [job]}])
                if mutation.get('status') == 'in_progress':
                    with patch.object(controller, '_clock',
                                      side_effect=(0, controller.JOB_POLL_BOUND,
                                                   controller.JOB_POLL_BOUND)):
                        result = self.assert_refused(api, reason)
                    self.assertEqual(result['waited_seconds'], controller.JOB_POLL_BOUND)
                else:
                    self.assert_refused(api, reason)

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
            '.github/workflows/gke-pr.yml': frozenset({'pull_request_target'}),
        })
        workflow = (ROOT / '.github/workflows/rerun-preempted.yml').read_text()
        trigger = workflow.split('\npermissions:', 1)[0]
        self.assertEqual(re.findall(r'^  (\w+):', trigger, re.M), ['workflow_run'])
        self.assertEqual(re.findall(r'^      - (.+)$', trigger, re.M),
                         ['CI post-merge', 'GKE PR'])
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


def api_logs(source, job_name):
    """Remove gh run view's two display columns, preserving API log bytes."""
    lines = []
    for line in source.splitlines(keepends=True):
        fields = line.split('\t', 2)
        if (len(fields) == 3 and fields[0] == job_name
                and re.match(r'^\d{4}-\d{2}-\d{2}T\S+\s+', fields[2])):
            line = fields[2]
        lines.append(line)
    return ''.join(lines)


class CaptureAPI(FakeAPI):
    def __init__(self, name):
        self.capture = json.loads((FIXTURES / f'{name}.json').read_text())
        run = deepcopy(self.capture['run.json'])
        # Two run snapshots were captured during/after attempt 2. Replay the
        # completed failed attempt 1 described by jobs-attempt1.json; retain
        # the original snapshot in the fixture, including its later state.
        run.update(status='completed', conclusion='failure', run_attempt=1)
        super().__init__(run=run,
                         job_pages=[self.capture['jobs-attempt1.json']])
        self.repo = run['repository']['full_name']
        self.endpoint = f'repos/{self.repo}/actions/runs/{run["id"]}'
        self.jobs_path = self.endpoint + '/attempts/1/jobs?per_page=100'
        self.file_path = (f'repos/{self.repo}/contents/{run["path"]}'
                          f'?ref={run["head_sha"]}')
        self.workflow = self.capture['workflow-at-head.yml']
        if name in ('e1', 'e2'):
            # These two called workflows are identical at both captured heads.
            self.workflows = json.loads((FIXTURES / 'reuse.json').read_text())
        self.logs, self.annotations = {}, {}
        for job in self.job_pages[0]['jobs']:
            if job['conclusion'] != 'failure':
                continue
            job_id = job['id']
            source = self.capture[f'job-{job_id}.log']
            self.logs[f'repos/{self.repo}/actions/jobs/{job_id}/logs'] = (
                controller.LogUnavailable() if source == UNAVAILABLE_LOG
                else api_logs(source, job['name']))
            route = job['check_run_url'].removeprefix('https://api.github.com/')
            self.annotations[route + '/annotations?per_page=100'] = [
                self.capture[f'annotations-{job_id}.json']]


ENGINE_CASES = (
    ('e1', 126202548075, [126202548075, 126203523633, 126204346774,
                        126219522021, 126229121477, 126229967614]),
    ('e2', 126342953893, [126342953893, 126346363870, 126359176509]),
)


def check_capture_logs(fixture, external):
    """Compare replay text with independent capture rows, without echoing it."""
    logs = {key: value for key, value in fixture.items() if key.endswith('.log')}
    refs = fixture.get('log_files', [])
    if (not isinstance(refs, list) or any(not isinstance(key, str)
            or not re.fullmatch(r'job-[0-9]+\.log', key) for key in refs)
            or len(set(refs)) != len(refs)):
        raise ValueError('invalid external log references')
    for key in refs:
        if key in logs or key not in external:
            raise ValueError('missing or duplicate external log')
        logs[key] = external[key]
    capture = fixture.get('capture', {})
    if not isinstance(capture, dict) or set(capture) != set(logs):
        raise ValueError('capture coverage differs from replay logs')
    for key, text in logs.items():
        if not isinstance(text, str):
            raise ValueError('invalid replay log type')
        if text == UNAVAILABLE_LOG:
            # A marker explicitly means there is no attempt-1 log capture.
            jobs = fixture.get('jobs-attempt1.json', {}).get('jobs', [])
            matches = [job for job in jobs if f'job-{job["id"]}.log' == key]
            if capture[key] is not None or len(matches) != 1:
                raise ValueError('unavailable log lacks its captured job')
            failed = [step for step in matches[0].get('steps', [])
                      if step.get('conclusion') == 'failure']
            if (matches[0]['conclusion'] != 'failure' or len(failed) != 1
                    or not failed[0].get('name', '').startswith('Require every')):
                raise ValueError('only a captured gate may lack its log')
            continue
        rows = capture[key]
        if (not isinstance(rows, list) or any(not isinstance(row, str) for row in rows)
                or text != ''.join(rows)):
            raise ValueError('replay log differs from captured lines')
    return set(refs)


class CaptureTests(unittest.TestCase):
    def test_all_fixture_logs(self):
        external = {path.name: path.read_bytes().decode('utf-8')
                    for path in FIXTURES.glob('*.log')}
        referenced = set()
        paths = sorted(FIXTURES.glob('*.json'))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(fixture=path.name):
                fixture = json.loads(path.read_text())
                referenced.update(check_capture_logs(fixture, external))
        self.assertEqual(referenced, set(external))

    def test_uncaptured_line_refused(self):
        # Synthetic corruption control; never materialized as a capture.
        fixture = {'job-1.log': 'captured line\nuncaptured line\n',
                   'capture': {'job-1.log': ['captured line\n']}}
        with self.assertRaises(ValueError):
            check_capture_logs(fixture, {})

    def test_unavailable_gate_logs(self):
        fixture = json.loads((FIXTURES / 'e2.json').read_text())
        keys = {key for key, value in fixture.items() if value == UNAVAILABLE_LOG}
        self.assertEqual(keys, {'job-126342953893.log', 'job-126346363870.log',
                                'job-126359176509.log'})
        for key in keys:
            with self.subTest(log=key):
                changed = deepcopy(fixture)
                changed[key] = 'uncaptured diagnostic\n'
                with self.assertRaises(ValueError):
                    check_capture_logs(changed, {})


class BeforeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Pin the actual unchanged controller, so this remains a red witness
        # after the fix is committed. This invokes git only, never gh/network.
        try:
            result = subprocess.run(
                ['git', 'show', 'cf68f156:scripts/ci/rerun_preempted.py'],
                cwd=ROOT, capture_output=True, text=True, check=False)
        except OSError as exc:
            reason = 'BeforeTests red witness skipped: git show is unavailable'
            print(reason)
            raise unittest.SkipTest(reason) from exc
        if result.returncode != 0:
            reason = 'BeforeTests red witness skipped: cf68f156 is absent from this checkout'
            print(reason)
            raise unittest.SkipTest(reason)
        source = result.stdout
        cls.before = ModuleType('before')
        exec(compile(source, '<before>', 'exec'), cls.before.__dict__)

    def test_engine_before(self):
        for name, blocker, _ in ENGINE_CASES:
            with self.subTest(capture=name):
                api = CaptureAPI(name)
                result = self.before.process(api, api.repo, api.run['id'])
                self.assertFalse(result['rerun'], result)
                if name == 'e2':
                    # The old controller requires the unavailable gate log.
                    # Do not invent a successful fetch to reproduce its blocker.
                    self.assertEqual(result['reason'], 'lookup_or_request_failed')
                    self.assertEqual(result['error_type'], 'LogUnavailable')
                    self.assertNotIn('blocked_job_id', result)
                    self.assertEqual(api.calls[-1],
                                     ('LOG', f'repos/{api.repo}/actions/jobs/{blocker}/logs'))
                else:
                    self.assertEqual(result['reason'],
                                     'independent_failure_diagnostic', result)
                    self.assertEqual(result['blocked_job_id'], blocker)
                self.assertFalse(any(c[0] == 'POST' for c in api.calls))


def case_api(workflow, *rows):
    """Explicit synthetic controls, separate from unmodified live evidence."""
    api = FakeAPI()
    api.workflow = textwrap.dedent(workflow).lstrip()
    api.logs, api.annotations = {}, {}
    jobs = []
    for job_id, (name, kind) in enumerate(rows, 100):
        job = deepcopy(CUT_JOB)
        job.update(id=job_id, name=name, conclusion='failure',
                   check_run_url=f'https://api.github.com/repos/{REPO}'
                                 f'/check-runs/{job_id}')
        step = {'loss': 'Compile', 'own': 'Test', 'gate': 'Require every lane passed',
                'needs': 'Summarize', 'ok': 'Compile'}[kind]
        job['steps'] = [{'name': step, 'conclusion': 'failure'}]
        if kind == 'ok':
            job['conclusion'] = 'success'
            job['steps'][0]['conclusion'] = 'success'
        logs = ('The runner has received a shutdown signal.\n' if kind == 'loss'
                else '##[error]Process completed with exit code 1.\n')
        if kind == 'own':
            logs += '##[error]Assertion failed in test\n'
        api.logs[f'repos/{REPO}/actions/jobs/{job_id}/logs'] = logs
        annotations = ([{'annotation_level': 'failure',
                         'message': 'Process completed with exit code 1.'}]
                       if kind == 'gate' else [])
        api.annotations[f'repos/{REPO}/check-runs/{job_id}'
                        '/annotations?per_page=100'] = [annotations]
        jobs.append(job)
    api.job_pages = [{'jobs': jobs}]
    return api


SMALL_FLOW = '''
jobs:
  work:
    name: Worker (${{ matrix.os }}, ${{ matrix.part }})
    strategy:
      matrix:
        os: [linux]
        part: [one, two]
  summary:
    name: Result
    needs: work
    steps:
      - name: Summarize
        env:
          RESULT: ${{ needs.work.result }}
        run: test "$RESULT" = success
'''


REUSE_PATH = '.github/workflows/reuse.yml'
REUSE_FLOW = '''
jobs:
  caller:
    name: Bundle
    uses: ./.github/workflows/reuse.yml
'''
CHILD_FLOW = '''
jobs:
  shard:
    strategy:
      matrix:
        part: [one, two]
  final:
    needs: [shard]
'''


class AggregateTests(unittest.TestCase):
    def decide(self, api, **kwargs):
        repo = api.run['repository']['full_name']
        return controller.process(api, repo, api.run['id'], **kwargs)

    def refused(self, api, reason, blocker=None):
        result = self.decide(api)
        self.assertFalse(result['rerun'], result)
        self.assertEqual(result['reason'], reason, result)
        if blocker is not None:
            self.assertEqual(result['blocked_job_id'], blocker, result)
        self.assertFalse(any(c[0] == 'POST' for c in api.calls))
        return result

    def accepted(self, api, dependent, **kwargs):
        result = self.decide(api, **kwargs)
        self.assertTrue(result['rerun'], result)
        self.assertEqual(result['reason'], 'runner_loss_with_dependents' if dependent
                         else 'all_failed_jobs_are_runner_loss')
        self.assertEqual(result['dependent_job_ids'], dependent)
        self.assertEqual(result['failed_job_ids'], [
            j['id'] for page in api.job_pages for j in page['jobs']
            if j['conclusion'] == 'failure'])
        self.assertEqual(api.file_calls, [api.file_path] + [
            api.file_route(path) for path in api.workflows])
        self.assertEqual(api.reads, 2)
        posts = [c for c in api.calls if c[0] == 'POST']
        self.assertEqual(posts, [] if kwargs.get('dry_run') else [
            ('POST', api.endpoint + '/rerun-failed-jobs')])
        return result

    def test_engine_after(self):
        # Both expected lists include the final build gate, reached through
        # failed shard aggregates under amendment 3's transitive rule.
        for name, _, dependent in ENGINE_CASES:
            with self.subTest(capture=name):
                api = CaptureAPI(name)
                self.accepted(api, dependent)
                for job_id in dependent:
                    self.assertIn(('LOG', f'repos/{api.repo}/actions/jobs/{job_id}/logs'),
                                  api.calls)

    def test_e2_fallback_evidence(self):
        gate_id = 126342953893
        for mutation in (None, 'annotations', 'steps'):
            with self.subTest(mutation=mutation):
                api = CaptureAPI('e2')
                gate_log = f'repos/{api.repo}/actions/jobs/{gate_id}/logs'
                with self.assertRaises(controller.LogUnavailable):
                    api.text(gate_log)
                if mutation == 'annotations':
                    route = f'repos/{api.repo}/check-runs/{gate_id}/annotations?per_page=100'
                    api.annotations[route] = [[]]
                    self.refused(api, 'no_gate_annotation_evidence', gate_id)
                elif mutation == 'steps':
                    # Synthetic negative control; the stored capture is unchanged.
                    job = next(j for j in api.job_pages[0]['jobs'] if j['id'] == gate_id)
                    next(s for s in job['steps'] if s['conclusion'] == 'failure')['name'] = 'Test'
                    self.refused(api, 'lookup_or_request_failed')
                else:
                    self.accepted(api, ENGINE_CASES[1][2])

    def test_full_head_workflow(self):
        # Exercise the parser on the untrimmed workflow as well. Read only
        # the committed data, without checkout or execution of its scripts.
        api = CaptureAPI('e2')
        ref = api.run['head_sha'] + ':.github/workflows/ci-post.yml'
        api.workflow = subprocess.run(
            ['git', 'show', ref], cwd=ROOT, capture_output=True,
            text=True, check=True).stdout
        for path in api.workflows:
            api.workflows[path] = subprocess.run(
                ['git', 'show', api.run['head_sha'] + ':' + path], cwd=ROOT,
                capture_output=True, text=True, check=True).stdout
        # Check identity and needs against the full committed workflows;
        # the live decisions above replay the trimmed captured evidence.
        specs = controller._workflow_jobs(api, api.repo, api.run)
        aliases = controller._map_jobs(specs, api.job_pages[0]['jobs'])
        self.assertEqual(len(aliases), len(api.job_pages[0]['jobs']))
        self.assertEqual(aliases[126331796695], 'build')
        self.assertEqual(aliases[126342953893], 'build-release-gate')
        self.assertEqual(aliases[126341075418], 'corpus-parity/corpus-parity')
        shard = 'native-live/native-live-shards'
        shard_id = next(j['id'] for j in api.job_pages[0]['jobs']
                        if j['name'] == 'native-live / native-live-shard (native, 2/4)')
        self.assertEqual(aliases[shard_id], shard)
        self.assertEqual(specs[shard]['needs'], ['native-live/authz'])
        self.assertEqual(specs['native-live/native-live-native']['needs'],
                         ['native-live/authz', shard])
        self.assertEqual(specs['build-release-gate']['needs'], ['authz', 'changes', 'build'])
        self.assertEqual(api.file_calls, [api.file_path] + [
            api.file_route(path) for path in api.workflows])
        self.assertFalse(any(c[0] == 'POST' for c in api.calls))

    def test_hpo_own_failure(self):
        self.refused(CaptureAPI('hpo'), 'independent_failure_diagnostic',
                     126311146425)

    def test_own_error_before_missing_loss(self):
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                       ('Worker (linux, two)', 'own'), ('Result', 'gate'))
        # An absent loss marker still refuses; it must not obscure a proven
        # independent failure in another job when naming the blocker.
        api.logs[next(iter(api.logs))] = '##[error]The operation was canceled.\n'
        self.refused(api, 'independent_failure_diagnostic', 101)

    def test_matrix_needs(self):
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                       ('Worker (linux, two)', 'ok'), ('Result', 'gate'))
        self.accepted(api, [102])

    def test_own_failed_need(self):
        api = case_api(SMALL_FLOW, ('Result', 'gate'),
                       ('Worker (linux, one)', 'loss'),
                       ('Worker (linux, two)', 'own'))
        result = self.refused(api, 'independent_failure_diagnostic', 102)
        self.assertNotIn(100, result['dependent_job_ids'])

    def test_no_own_error_excused(self):
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                       ('Result', 'own'))
        self.refused(api, 'independent_failure_diagnostic', 101)
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                       ('Result', 'gate'))
        api.logs[list(api.logs)[1]] += '##[error]SyntaxError in gate\n'
        api.annotations[list(api.annotations)[1]][0].append(
            {'annotation_level': 'failure', 'message': 'SyntaxError in gate'})
        self.refused(api, 'independent_failure_diagnostic', 101)

    def test_gate_without_log(self):
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
        gate_log = f'repos/{REPO}/actions/jobs/101/logs'
        api.logs[gate_log] = controller.LogUnavailable()
        self.accepted(api, [101])
        self.assertIn(('LOG', gate_log), api.calls)
        self.assertIn(('LOG', f'repos/{REPO}/actions/jobs/100/logs'), api.calls)

    def test_gate_own_diagnostics(self):
        # Reuse the existing synthetic assertion fault, never capture evidence.
        fault = case_api(SMALL_FLOW, ('Result', 'own'))
        assertion = next(iter(fault.logs.values())).splitlines()[-1]
        for source in ('log', 'annotations', 'unavailable'):
            with self.subTest(source=source):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
                gate_log = f'repos/{REPO}/actions/jobs/101/logs'
                if source == 'log':
                    api.logs[gate_log] += assertion + '\n'
                else:
                    route = f'repos/{REPO}/check-runs/101/annotations?per_page=100'
                    api.annotations[route][0].append(
                        {'annotation_level': 'failure', 'message': assertion})
                    if source == 'unavailable':
                        api.logs[gate_log] = controller.LogUnavailable()
                result = self.refused(api, 'independent_failure_diagnostic', 101)
                self.assertEqual(result['dependent_job_ids'], [])
                self.assertIn(('LOG', gate_log), api.calls)

    def test_gate_runner_loss(self):
        capture = CaptureAPI('e1')
        source_id = 126187307495
        source_log = f'repos/{capture.repo}/actions/jobs/{source_id}/logs'
        source_ann = f'repos/{capture.repo}/check-runs/{source_id}/annotations?per_page=100'
        for annotations in (capture.annotations[source_ann], [[]]):
            with self.subTest(has_annotations=bool(annotations[0])):
                # Synthetic gate identity, with captured exit-130/shutdown evidence.
                api = case_api(SMALL_FLOW, ('Result', 'gate'))
                gate_log = f'repos/{REPO}/actions/jobs/100/logs'
                api.logs[gate_log] = capture.logs[source_log]
                api.annotations[next(iter(api.annotations))] = deepcopy(annotations)
                self.accepted(api, [])
                self.assertIn(('LOG', gate_log), api.calls)

    def test_gate_loss_with_own_error(self):
        capture = CaptureAPI('e1')
        fault = case_api(SMALL_FLOW, ('Result', 'own'))
        assertion = next(iter(fault.logs.values())).splitlines()[-1]
        api = case_api(SMALL_FLOW, ('Result', 'gate'))
        source_log = f'repos/{capture.repo}/actions/jobs/126187307495/logs'
        api.logs[next(iter(api.logs))] = capture.logs[source_log] + assertion + '\n'
        api.annotations[next(iter(api.annotations))] = deepcopy(
            capture.annotations[f'repos/{capture.repo}/check-runs/126187307495/annotations?per_page=100'])
        self.refused(api, 'independent_failure_diagnostic', 100)

    def test_gate_exit_without_loss(self):
        capture = CaptureAPI('e1')
        annotations = capture.annotations[
            f'repos/{capture.repo}/check-runs/126187307495/annotations?per_page=100']
        for with_loss in (False, True):
            with self.subTest(with_loss=with_loss):
                api = case_api(SMALL_FLOW, ('Result', 'gate'),
                               ('Worker (linux, one)', 'loss' if with_loss else 'ok'))
                api.logs[f'repos/{REPO}/actions/jobs/100/logs'] = controller.LogUnavailable()
                api.annotations[next(iter(api.annotations))] = deepcopy(annotations)
                if with_loss:
                    self.accepted(api, [100])
                else:
                    result = self.refused(api, 'no_runner_loss_jobs', 100)
                    self.assertEqual(result['dependent_job_ids'], [100])

    def test_gate_log_transport(self):
        for status in (404, 410, 401, 403, 429, 500):
            with self.subTest(status=status):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
                gate_log = f'repos/{REPO}/actions/jobs/101/logs'
                transport = controller.GitHubAPI()
                transport._log_escape_flag = True
                read_log = api.text

                def read(path):
                    if path != gate_log:
                        return read_log(path)
                    api.calls.append(('LOG', path))
                    return transport.text(path)

                stdout, stderr = StringIO(), StringIO()
                response = subprocess.CompletedProcess(
                    [], 1, UNTRUSTED_OUTPUT, f'HTTP {status}\n' + UNTRUSTED_OUTPUT)
                with patch.object(api, 'text', side_effect=read), \
                        patch.object(controller.subprocess, 'run', return_value=response) as command, \
                        redirect_stdout(stdout), redirect_stderr(stderr):
                    if status in (404, 410):
                        self.accepted(api, [101])
                    else:
                        self.refused(api, 'lookup_or_request_failed')
                command.assert_called_once_with(
                    ['gh', 'api', '--method', 'GET', gate_log, ESCAPE_FLAG],
                    capture_output=True, text=True, timeout=60, check=False)
                self.assertEqual(stdout.getvalue(), '')
                for private in (REPO, FAKE_QUERY, FAKE_BODY):
                    self.assertNotIn(private, stderr.getvalue())

    def test_gate_needs_annotations(self):
        for page in ([], [{'annotation_level': 'warning', 'message': 'warning only'}]):
            with self.subTest(page_empty=not page):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
                api.annotations[list(api.annotations)[1]] = [page]
                self.refused(api, 'no_gate_annotation_evidence', 101)

    def test_gate_annotation_lookup(self):
        for error in (PermissionError('private'), RuntimeError('private')):
            with self.subTest(error=type(error).__name__):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
                gate_route = list(api.annotations)[1]
                pages = api.pages

                def lookup(path):
                    if path == gate_route:
                        raise error
                    return pages(path)

                with patch.object(api, 'pages', side_effect=lookup):
                    result = self.refused(api, 'lookup_or_request_failed')
                self.assertNotIn('private', json.dumps(result))

    def test_shard_log_still_required(self):
        for error in (RuntimeError('missing shard log'), controller.LogUnavailable()):
            with self.subTest(error=type(error).__name__):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
                api.annotations[next(iter(api.annotations))] = [[
                    {'annotation_level': 'failure',
                     'message': 'The runner has received a shutdown signal.'}]]
                api.log_error = error
                self.refused(api, 'lookup_or_request_failed')

    def test_gate_step_only(self):
        api = case_api(SMALL_FLOW, ('Result', 'gate'),
                       ('Worker (linux, one)', 'loss'))
        self.accepted(api, [100], dry_run=True)

    def test_gate_chain(self):
        # Put the outer gate first in both YAML and API order. Neither list
        # is a substitute for the workflow's dependency order.
        flow = SMALL_FLOW.replace('jobs:\n', 'jobs:\n  final:\n    needs: [summary]\n')
        api = case_api(flow, ('final', 'gate'), ('Result', 'gate'),
                       ('Worker (linux, one)', 'loss'))
        self.accepted(api, [100, 101])

    def test_gate_chain_own_failure(self):
        flow = SMALL_FLOW + '\n  final:\n    needs: [summary]\n'
        for extra_step in (False, True):
            with self.subTest(extra_step=extra_step):
                api = case_api(flow, ('final', 'gate'), ('Result', 'gate'),
                               ('Worker (linux, one)', 'loss'))
                steps = api.job_pages[0]['jobs'][1]['steps']
                if extra_step:
                    steps.append({'name': 'Compile', 'conclusion': 'failure'})
                    reason = 'multiple_failed_steps'
                else:
                    steps[0]['name'] = 'Summarize'
                    reason = 'independent_failure_diagnostic'
                result = self.refused(api, reason, 101)
                self.assertEqual(result['dependent_job_ids'], [])

    def test_gate_chain_without_loss(self):
        flow = SMALL_FLOW + '\n  final:\n    needs: [summary]\n'
        api = case_api(flow, ('final', 'gate'), ('Result', 'gate'),
                       ('Worker (linux, one)', 'ok'))
        result = self.refused(api, 'no_runner_loss_jobs', 100)
        self.assertEqual(result['dependent_job_ids'], [100, 101])

    def test_gate_graph_refuses(self):
        for source in (SMALL_FLOW.replace('  work:\n', '  work:\n    needs: summary\n'),
                       SMALL_FLOW.replace('needs: work', 'needs: absent')):
            with self.subTest(source=source):
                api = case_api(source, ('Result', 'gate'), ('Worker (linux, one)', 'loss'))
                result = self.refused(api, 'aggregate_mapping_unavailable', 100)
                self.assertEqual(result['dependent_job_ids'], [])

    def test_inline_own_failure(self):
        source = SMALL_FLOW.replace('    needs: work',
                                    '    needs: work\n    if: always()')
        source = source.replace('run: test "$RESULT" = success',
                                'run: |\n          # needs.work.result\n'
                                '          test -f /missing')
        api = case_api(source, ('Worker (linux, one)', 'loss'), ('Result', 'needs'))
        result = self.refused(api, 'independent_failure_diagnostic', 101)
        self.assertEqual(result['dependent_job_ids'], [])

    def test_gate_has_other_failure(self):
        for conclusion in ('failure', 'cancelled'):
            with self.subTest(conclusion=conclusion):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                               ('Result', 'gate'))
                api.job_pages[0]['jobs'][1]['steps'].append(
                    {'name': 'Compile', 'conclusion': conclusion})
                reason = ('multiple_failed_steps' if conclusion == 'failure'
                          else 'independent_failure_diagnostic')
                self.refused(api, reason, 101)

    def test_gate_admission_stays_strict(self):
        for mutation, reason in (
                ({'labels': ['gkp-2cpu-4gb-amd64']}, 'not_a_fleet_amd64_job'),
                ({'check_run_url': 'foreign'}, 'invalid_check_run_identity')):
            with self.subTest(reason=reason):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                               ('Result', 'gate'))
                api.job_pages[0]['jobs'][1].update(mutation)
                self.refused(api, reason, 101)

    def test_gate_without_loss(self):
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'ok'),
                       ('Result', 'gate'))
        self.refused(api, 'no_runner_loss_jobs', 101)

    def test_unmapped_failed_job(self):
        for name in ('Unknown', 'Worker (linux, three)', 'Result / pretend'):
            with self.subTest(name=name):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'),
                               (name, 'gate'))
                self.refused(api, 'aggregate_mapping_unavailable', 101)

    def test_workflow_read_failure(self):
        for error in (PermissionError('private'), RuntimeError('private')):
            with self.subTest(error=type(error).__name__):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'))
                api.file_error = error
                result = self.refused(api, 'aggregate_mapping_unavailable', 100)
                self.assertNotIn('private', json.dumps(result))

    def test_workflow_parse_failure(self):
        for source in ('jobs: [broken', 'jobs:\n  work: *alias\n',
                       'jobs:\n  work: {}\n  work: {}\n',
                       SMALL_FLOW.replace('needs: work', 'needs: absent'),
                       SMALL_FLOW.replace('part: [one, two]',
                                          'part: ${{ fromJSON(inputs.x) }}')):
            with self.subTest(source=source):
                api = case_api(source, ('Worker (linux, one)', 'loss'))
                self.refused(api, 'aggregate_mapping_unavailable', 100)

    def test_mapping_ambiguity(self):
        api = case_api(SMALL_FLOW + '\n  other:\n    name: Result\n',
                       ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
        self.refused(api, 'aggregate_mapping_unavailable', 101)
        flow = SMALL_FLOW + '''
  other:
    name: Hidden
  duplicate:
    name: Hidden
'''
        api = case_api(flow, ('Worker (linux, one)', 'loss'),
                       ('Result', 'gate'), ('Hidden', 'ok'))
        api.job_pages[0]['jobs'][2]['conclusion'] = 'skipped'
        self.accepted(api, [101])

    def test_needs_not_name_or_order(self):
        flow = SMALL_FLOW.replace('needs: work', 'needs: unrelated') + '''
  unrelated:
    name: Healthy
'''
        api = case_api(flow, ('Result', 'gate'), ('Healthy', 'own'),
                       ('Worker (linux, one)', 'loss'))
        result = self.refused(api, 'independent_failure_diagnostic', 101)
        self.assertEqual(result['dependent_job_ids'], [])

    def test_condition_is_not_gate_proof(self):
        source = SMALL_FLOW.replace('env:\n          RESULT: ${{ needs.work.result }}',
                                    "if: ${{ needs.work.result == 'failure' }}")
        api = case_api(source, ('Worker (linux, one)', 'loss'), ('Result', 'needs'))
        self.refused(api, 'independent_failure_diagnostic', 101)

    def test_own_build_not_an_aggregate(self):
        source = SMALL_FLOW.replace('    steps:\n',
                                    '    steps:\n      - uses: actions/checkout@v4\n')
        api = case_api(source, ('Worker (linux, one)', 'loss'), ('Result', 'needs'))
        self.refused(api, 'independent_failure_diagnostic', 101)

    def test_reusable_caller(self):
        api = case_api(REUSE_FLOW, ('Bundle / shard (one)', 'loss'),
                       ('Bundle / final', 'gate'))
        api.workflows[REUSE_PATH] = CHILD_FLOW
        self.accepted(api, [101])

    def test_reuse_own_failed_need(self):
        flow = REUSE_FLOW.replace('name: Bundle', 'name: native-live')
        api = case_api(flow,
                       ('native-live / native-live-shard (native, 1/4)', 'loss'),
                       ('native-live / native-live-shard (native, 2/4)', 'own'),
                       ('native-live / native-live (native)', 'gate'))
        sources = json.loads((FIXTURES / 'reuse.json').read_text())
        api.workflows[REUSE_PATH] = sources['.github/workflows/native-live.yml']
        result = self.refused(api, 'independent_failure_diagnostic', 101)
        self.assertEqual(result['dependent_job_ids'], [])

    def test_reuse_local_needs(self):
        flow = REUSE_FLOW.replace('    name: Bundle',
                                  '    name: Bundle\n    needs: outside')
        flow += '\n  outside:\n    name: Outside\n'
        api = case_api(flow, ('Outside', 'ok'),
                       ('Bundle / shard (one)', 'loss'), ('Bundle / final', 'gate'))
        api.workflows[REUSE_PATH] = CHILD_FLOW
        specs = controller._workflow_jobs(api, REPO, api.run)
        self.assertEqual(specs['caller']['needs'], ['outside'])
        self.assertEqual(specs['caller/final']['needs'], ['caller/shard'])
        self.assertEqual(specs['caller/shard']['needs'], [])
        api.file_calls.clear()
        self.accepted(api, [102])

    def test_reuse_outer_gate(self):
        flow = REUSE_FLOW + '\n  summary:\n    needs: caller\n'
        for kind in ('loss', 'own'):
            with self.subTest(kind=kind):
                api = case_api(flow, ('Bundle / shard (one)', 'loss'),
                               ('Bundle / shard (two)', kind), ('summary', 'gate'))
                api.workflows[REUSE_PATH] = CHILD_FLOW
                if kind == 'loss':
                    self.accepted(api, [102])
                else:
                    result = self.refused(api, 'independent_failure_diagnostic', 101)
                    self.assertEqual(result['dependent_job_ids'], [])

    def test_reuse_gate_chain(self):
        flow = REUSE_FLOW.replace('jobs:\n', 'jobs:\n  summary:\n    needs: caller\n')
        api = case_api(flow, ('summary', 'gate'), ('Bundle / final', 'gate'),
                       ('Bundle / shard (one)', 'loss'))
        api.workflows[REUSE_PATH] = CHILD_FLOW
        self.accepted(api, [100, 101])

    def test_reuse_matrix_scope(self):
        flow = REUSE_FLOW.replace('name: Bundle', 'name: Bundle (${{ matrix.os }})')
        flow += '''
    strategy:
      matrix:
        os: [linux, mac]
'''
        api = case_api(flow, ('Bundle (mac) / final', 'gate'),
                       ('Bundle (linux) / final', 'gate'),
                       ('Bundle (mac) / shard (one)', 'own'),
                       ('Bundle (linux) / shard (one)', 'loss'))
        api.workflows[REUSE_PATH] = CHILD_FLOW
        result = self.refused(api, 'independent_failure_diagnostic', 102)
        self.assertEqual(result['dependent_job_ids'], [101])
        self.assertEqual(api.file_calls, [api.file_path, api.file_route(REUSE_PATH)])

    def test_reuse_nested(self):
        api = case_api(REUSE_FLOW, ('Bundle / middle / shard (one)', 'loss'),
                       ('Bundle / middle / final', 'gate'))
        api.workflows[REUSE_PATH] = '''
jobs:
  middle:
    uses: ./.github/workflows/inner.yml
'''
        api.workflows['.github/workflows/inner.yml'] = CHILD_FLOW
        self.accepted(api, [101])

    def test_reuse_read_failure(self):
        for error in (PermissionError('private'), RuntimeError('private')):
            with self.subTest(error=type(error).__name__):
                api = case_api(REUSE_FLOW, ('Bundle / shard (one)', 'loss'))
                api.workflows[REUSE_PATH] = error
                result = self.refused(api, 'aggregate_mapping_unavailable', 100)
                self.assertEqual(api.file_calls, [api.file_path, api.file_route(REUSE_PATH)])
                self.assertNotIn('private', json.dumps(result))

    def test_reuse_parse_failure(self):
        for source in ('jobs: [broken', 'jobs:\n  shard: *alias\n',
                       CHILD_FLOW.replace('needs: [shard]', 'needs: [outside]'),
                       CHILD_FLOW.replace('  shard:\n', '  shard:\n    needs: final\n'),
                       'jobs:\n  cycle:\n    uses: ./.github/workflows/ci-post.yml\n'):
            with self.subTest(source=source):
                api = case_api(REUSE_FLOW, ('Bundle / shard (one)', 'loss'))
                api.workflows[REUSE_PATH] = source
                self.refused(api, 'aggregate_mapping_unavailable', 100)

    def test_reuse_exact_names(self):
        for name in ('Bundle / unknown', 'Bundle / shard (three)',
                     'Bundle / final / pretend'):
            with self.subTest(name=name):
                api = case_api(REUSE_FLOW, (name, 'loss'))
                api.workflows[REUSE_PATH] = CHILD_FLOW
                self.refused(api, 'aggregate_mapping_unavailable', 100)

    def test_reuse_path_admission(self):
        for target in ('other/repo/.github/workflows/x.yml@main',
                       './.github/workflows/../x.yml',
                       './.github/workflows/reuse.yml?ref=main',
                       './.github/workflows/${{ inputs.file }}.yml'):
            with self.subTest(target=target):
                flow = REUSE_FLOW.replace('./' + REUSE_PATH, target)
                api = case_api(flow, ('Bundle / shard (one)', 'loss'))
                self.refused(api, 'aggregate_mapping_unavailable', 100)
                self.assertEqual(api.file_calls, [api.file_path])

    def test_matrix_forms(self):
        flow = '''
        jobs:
          work:
            name: Unit (${{ matrix.platform.os }}, ${{ matrix.part }})
            strategy:
              matrix:
                platform:
                  - os: linux
                part: [one, two]
                exclude:
                  - part: two
                include:
                  - platform: {os: mac}
                    part: three
          summary:
            needs:
              - work
        '''
        api = case_api(flow, ('Unit (linux, one)', 'loss'),
                       ('Unit (mac, three)', 'ok'), ('summary', 'gate'))
        self.accepted(api, [102])
        api = case_api(flow, ('Unit (linux, two)', 'loss'))
        self.refused(api, 'aggregate_mapping_unavailable', 100)

    def test_default_matrix_name(self):
        flow = '''
        jobs:
          work:
            strategy:
              matrix:
                os: [linux]
                part: [one, two]
          summary:
            needs: [work]
        '''
        api = case_api(flow, ('work (linux, one)', 'loss'), ('summary', 'gate'))
        self.accepted(api, [101])

    def test_scripts_are_data(self):
        source = SMALL_FLOW.replace('run: test "$RESULT" = success', '''run: |
          jobs:
            pretender:
              name: Not a job
          raise RuntimeError("must remain data")''')
        api = case_api(source, ('Worker (linux, one)', 'loss'), ('Result', 'needs'))
        with patch.object(controller.subprocess, 'run') as command:
            self.refused(api, 'independent_failure_diagnostic', 101)
        command.assert_not_called()
        api = case_api(source, ('Not a job', 'loss'))
        self.refused(api, 'aggregate_mapping_unavailable', 100)

    def test_contents_transport(self):
        api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'), ('Result', 'gate'))
        body = api.get(api.file_path)
        api.file_calls.clear()
        fake_get = api.get

        def get(path):
            if path == api.file_path:
                api.file_calls.append(path)
                return controller.GitHubAPI().get(path)
            return fake_get(path)

        with patch.object(api, 'get', side_effect=get), \
                patch.object(controller.subprocess, 'run', return_value=
                             subprocess.CompletedProcess([], 0, json.dumps(body), '')) as command:
            self.accepted(api, [101])
        command.assert_called_once_with(
            ['gh', 'api', '--method', 'GET', api.file_path],
            capture_output=True, text=True, timeout=60, check=False)

    def test_bad_contents_refused(self):
        for change in ({'encoding': 'none'}, {'content': 'bad!'},
                       {'path': '.github/workflows/foreign.yml'}, {'type': 'symlink'}):
            with self.subTest(change=change):
                api = case_api(SMALL_FLOW, ('Worker (linux, one)', 'loss'))
                body = api.get(api.file_path)
                body.update(change)
                fake_get = api.get

                def get(path):
                    return body if path == api.file_path else fake_get(path)

                with patch.object(api, 'get', side_effect=get):
                    self.refused(api, 'aggregate_mapping_unavailable', 100)

    def test_contents_error_is_private(self):
        api = FakeAPI()
        stdout, stderr = StringIO(), StringIO()
        with patch.object(controller.subprocess, 'run', return_value=
                          subprocess.CompletedProcess([], 1, UNTRUSTED_OUTPUT,
                                                      'HTTP 403\n' + UNTRUSTED_OUTPUT)), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            with self.assertRaises(PermissionError):
                controller.GitHubAPI().get(api.file_path)
        self.assertEqual(stdout.getvalue(), '')
        self.assertEqual(stderr.getvalue(),
                         'GitHub API failure: method=GET '
                         'path=repos/{repo}/contents/{workflow} '
                         'http_status=403 gh_exit_code=1 reason=request_failed\n')
        for secret in (UNTRUSTED_OUTPUT, api.run['head_sha'], REPO):
            self.assertNotIn(secret, stderr.getvalue())


def load_tests(loader, suite, pattern):
    # Audit evidence, then run the old witnesses before the new behavior.
    return unittest.TestSuite(loader.loadTestsFromTestCase(cls) for cls in
                              (CaptureTests, BeforeTests, ControllerTests, AggregateTests))


if __name__ == '__main__':
    unittest.main()
