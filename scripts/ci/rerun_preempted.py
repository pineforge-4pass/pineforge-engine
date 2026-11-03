#!/usr/bin/env python3
"""Retry only failed CI jobs when all failures are runner loss; never execute logs.

Requires gh and GH_TOKEN (actions:write for execution, actions:read for --dry-run;
checks:read permits supplemental annotations).
The controller must load this file from its own trusted default-branch commit.
Adapted from the peer backend controller supplied for LB-1012.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

PRIMARY_BRANCH = 'main'
MAX_ATTEMPTS = 3
# Exact entrypoints/events, independently checked in addition to workflow_run names.
ALLOWED_WORKFLOWS = {
    '.github/workflows/ci-post.yml': frozenset({'push', 'schedule'}),
}
# Bare amd64 GKE scale sets, optionally with the fleet's disk-size suffix.
# No arm64, gkp pools or operator-dispatched entrypoints are admitted.
FLEET_LABEL = re.compile(r'gke-(?:2|4|8|16|32)cpu-(?:4|8|16|32|64)gb-amd64(?:-(?:64|128|256|512)gb)?')
LOSS_PATTERNS = (
    re.compile(r'The runner has received a shutdown signal\..*', re.I),
    re.compile(r'(?:The runner[: ]+.*?)?lost communication with the server\b.*', re.I),
    re.compile(r'The self-hosted runner\b.*\blost\b.*', re.I),
)
LOSS_FALLOUT = re.compile(r'(?:The operation was cancel(?:ed|led)\.|Process completed with exit code (?:130|137|143)\.)', re.I)
TIMESTAMP = re.compile(r'^\d{4}-\d{2}-\d{2}T\S+\s+')


class GitHubAPI:
    """gh handles authentication and signed log redirects without exposing tokens."""

    def __init__(self):
        self._log_escape_flag = None

    @staticmethod
    def _diagnostic(path, method, *, status='unknown', exit_code='unknown', reason='request_failed'):
        # Only fixed route templates enter logs: no repo names, query strings,
        # signed redirect URLs, response bodies, or raw gh stderr.
        route = path.split('?', 1)[0]
        safe_path = 'unknown'
        for suffix in (
            r'actions/runs/[0-9]+',
            r'actions/runs/[0-9]+/attempts/[0-9]+/jobs',
            r'actions/jobs/[0-9]+/logs',
            r'actions/runs/[0-9]+/rerun-failed-jobs',
            r'check-runs/[0-9]+/annotations',
        ):
            if re.fullmatch(r'repos/[^/]+/[^/]+/' + suffix, route):
                safe_path = 'repos/{repo}/' + re.sub(r'[0-9]+', '{id}', route.split('/', 3)[3])
                break
        safe_method = method if method in {'GET', 'POST'} else 'unknown'
        print(f'GitHub API failure: method={safe_method} path={safe_path} '
              f'http_status={status} gh_exit_code={exit_code} reason={reason}', file=sys.stderr)

    def _execute(self, args, path, method):
        try:
            return subprocess.run(args, capture_output=True, text=True, timeout=60, check=False)
        except (subprocess.TimeoutExpired, OSError) as error:
            reason = 'timeout' if isinstance(error, subprocess.TimeoutExpired) else 'process_error'
            self._diagnostic(path, method, reason=reason)
            raise RuntimeError('GitHub CLI execution failed') from None

    def _call(self, path, *, method='GET', paginate=False):
        args = ['gh', 'api', '--method', method, path]
        if paginate:
            # The fleet's Ubuntu gh 2.45 predates --slurp.
            args.append('--paginate')
        if method == 'GET' and re.fullmatch(r'repos/[^/]+/[^/]+/actions/jobs/[0-9]+/logs(?:\?.*)?', path):
            if self._log_escape_flag is None:
                help_result = self._execute(['gh', 'api', '--help'], path, method)
                if help_result.returncode:
                    self._diagnostic(path, method, exit_code=help_result.returncode, reason='feature_detection_failed')
                    raise RuntimeError('GitHub CLI feature detection failed')
                self._log_escape_flag = '--allow-escape-sequences' in help_result.stdout
            if self._log_escape_flag:
                # New gh refuses ESC in logs even with captured/non-TTY stdout.
                # Treat logs strictly as data; never print or execute this output.
                args.append('--allow-escape-sequences')
        result = self._execute(args, path, method)
        if result.returncode:
            status_match = re.search(r'\bHTTP ([1-5][0-9]{2})\b', result.stderr)
            status = status_match[1] if status_match else 'unknown'
            reason = ('terminal_escape_sequences' if 'the response contains terminal escape sequences' in result.stderr
                      else 'request_failed')
            self._diagnostic(path, method, status=status, exit_code=result.returncode, reason=reason)
            optional_annotations = method == 'GET' and re.fullmatch(
                r'repos/[^/]+/[^/]+/check-runs/[0-9]+/annotations(?:\?.*)?', path
            )
            if status == '403' or (optional_annotations and status == '404'):
                raise PermissionError('GitHub API scope refused')
            raise RuntimeError('GitHub API request failed')
        return result.stdout

    def get(self, path):
        return json.loads(self._call(path))

    def pages(self, path):
        source = self._call(path, paginate=True).lstrip()
        decoder = json.JSONDecoder()
        pages = []
        while source:
            page, end = decoder.raw_decode(source)
            pages.append(page)
            source = source[end:].lstrip()
        return pages

    def text(self, path):
        return self._call(path)

    def post(self, path):
        self._call(path, method='POST')


def _gate(run, repo, expected_attempt):
    if run['repository']['full_name'] != repo:
        return 'repository_mismatch'
    if run['status'] != 'completed' or run['conclusion'] != 'failure':
        return 'not_a_completed_failed_run'
    attempt = int(run['run_attempt'])
    if not 1 <= attempt < MAX_ATTEMPTS:
        return 'attempt_limit'
    if expected_attempt is not None and attempt != expected_attempt:
        return 'stale_completion_event'
    if run['event'] not in ALLOWED_WORKFLOWS.get(run['path'], ()):
        return 'workflow_or_event_not_allowlisted'
    if run['head_branch'] != PRIMARY_BRANCH:
        return 'not_the_primary_branch'
    return None


def _loss(message):
    return any(pattern.fullmatch(message) for pattern in LOSS_PATTERNS)


def _job_evidence(api, repo, job):
    if not any(FLEET_LABEL.fullmatch(label) for label in job['labels']):
        return 'not_a_fleet_amd64_job'
    if sum(step.get('conclusion') == 'failure' for step in job.get('steps', [])) > 1:
        return 'multiple_failed_steps'
    check_url = job['check_run_url']
    prefix = f'https://api.github.com/repos/{repo}/check-runs/'
    if not check_url.startswith(prefix) or not check_url[len(prefix):].isdigit():
        return 'invalid_check_run_identity'
    messages = []
    try:
        annotations = api.pages(check_url.removeprefix('https://api.github.com/') + '/annotations?per_page=100')
    except PermissionError:
        # A token can lack Checks read, or GitHub can hide annotations with 404.
        # Actions logs remain mandatory and are inspected for independent errors.
        annotations = []
    for page in annotations:
        for annotation in page:
            if annotation['annotation_level'] == 'failure':
                messages.extend(line.strip() for line in annotation['message'].splitlines() if line.strip())
    logs = api.text(f'repos/{repo}/actions/jobs/{int(job["id"])}/logs')
    if len(logs) > 32 * 1024 * 1024:
        return 'log_too_large_to_classify'
    for line in logs.splitlines():
        line = TIMESTAMP.sub('', line).strip()
        if line.startswith('##[error]'):
            messages.append(line.removeprefix('##[error]').strip())
        elif _loss(line):
            messages.append(line)
    if any(not _loss(message) and not LOSS_FALLOUT.fullmatch(message) for message in messages):
        return 'independent_failure_diagnostic'
    if not any(_loss(message) for message in messages):
        return 'no_runner_loss_evidence'
    return None


def process(api, repo, run_id, *, dry_run=False, expected_attempt=None):
    """Return a bounded decision; lookup ambiguity always refuses the POST."""
    result = {'run_id': run_id, 'dry_run': dry_run, 'rerun': False, 'failed_job_ids': []}
    try:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo) or int(run_id) <= 0:
            raise ValueError('invalid repository or run id')
        endpoint = f'repos/{repo}/actions/runs/{int(run_id)}'
        run = api.get(endpoint)
        reason = _gate(run, repo, expected_attempt)
        result['run_attempt'] = run['run_attempt']
        if reason:
            result['reason'] = reason
            return result
        attempt = int(run['run_attempt'])
        jobs = [job for page in api.pages(f'{endpoint}/attempts/{attempt}/jobs?per_page=100') for job in page['jobs']]
        if not jobs or any(job['status'] != 'completed' for job in jobs):
            result['reason'] = 'missing_or_incomplete_jobs'
            return result
        if any(job['conclusion'] not in {'success', 'skipped', 'neutral', 'failure'} for job in jobs):
            result['reason'] = 'ambiguous_unsuccessful_job'
            return result
        failed = [job for job in jobs if job['conclusion'] == 'failure']
        result['failed_job_ids'] = [int(job['id']) for job in failed]
        if not failed:
            result['reason'] = 'no_failed_jobs'
            return result
        for job in failed:
            reason = _job_evidence(api, repo, job)
            if reason:
                result['reason'] = reason
                result['blocked_job_id'] = int(job['id'])
                return result
        # Re-read just before mutation; concurrency serializes controller runs too.
        current = api.get(endpoint)
        identity = ('id', 'run_attempt', 'head_sha', 'path', 'event', 'head_branch')
        if _gate(current, repo, attempt) or any(current[key] != run[key] for key in identity):
            result['reason'] = 'run_changed_during_classification'
            return result
        if not dry_run:
            api.post(f'{endpoint}/rerun-failed-jobs')
        result.update(rerun=True, reason='all_failed_jobs_are_runner_loss')
    except Exception as error:
        result.update(rerun=False, reason='lookup_or_request_failed', error_type=type(error).__name__)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'), required=not os.environ.get('GITHUB_REPOSITORY'))
    parser.add_argument('--run-id', required=True, type=int)
    parser.add_argument('--expected-attempt', type=int)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    decision = process(GitHubAPI(), args.repo, args.run_id, dry_run=args.dry_run, expected_attempt=args.expected_attempt)
    print(json.dumps(decision, sort_keys=True))
    # Refusals, including lookup failures, are successful controller decisions.
    # Their JSON diagnostics remain visible; missing proof never permits a POST.
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
