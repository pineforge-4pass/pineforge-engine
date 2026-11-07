#!/usr/bin/env python3
"""Retry runner loss and proven dependent gates; never execute logs or workflows.

Requires gh and GH_TOKEN (actions:write for execution, actions:read for --dry-run;
checks:read is required for gates and supplemental for shards;
contents:read reads workflow data).
The controller must load this file from its own trusted default-branch commit.
Adapted from the peer backend controller supplied for LB-1012.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime
from graphlib import TopologicalSorter
import itertools
import json
import os
import re
import subprocess
import sys
import textwrap
import time
from urllib.parse import quote

PRIMARY_BRANCH = 'main'
MAX_ATTEMPTS = 3
PR_WORKFLOW = '.github/workflows/gke-pr.yml'
PR_EVENT = 'pull_request_target'
# Let the jobs API converge after the workflow_run completion event.
JOB_POLL_INTERVAL = 30
JOB_POLL_BOUND = 10 * 60
# Exact entrypoints/events, independently checked in addition to workflow_run names.
ALLOWED_WORKFLOWS = {
    '.github/workflows/ci-post.yml': frozenset({'push', 'schedule'}),
    PR_WORKFLOW: frozenset({PR_EVENT}),
}
# The hosted x86 image the proof and post jobs run on, plus the legacy bare amd64
# GKE scale sets (optionally with the fleet's disk-size suffix) that attempts begun
# before the hosted migration and the captured evidence still carry.
# No arm64 (hosted or GKE), gkp pools or operator-dispatched entrypoints are admitted.
FLEET_LABEL = re.compile(r'ubuntu-24\.04|gke-(?:ondemand-)?(?:2|4|8|16|32)cpu-(?:4|8|16|32|64)gb-amd64(?:-(?:64|128|256|512)gb)?')
LOSS_PATTERNS = (
    re.compile(r'The runner has received a shutdown signal\..*', re.I),
    re.compile(r'(?:The runner[: ]+.*?)?lost communication with the server\b.*', re.I),
    re.compile(r'The self-hosted runner\b.*\blost\b.*', re.I),
)
LOSS_FALLOUT = re.compile(r'(?:The operation was cancel(?:ed|led)\.|Process completed with exit code (?:130|137|143)\.)', re.I)
GATE_EXIT = re.compile(r'Process completed with exit code [0-9]+\.')
TIMESTAMP = re.compile(r'^\d{4}-\d{2}-\d{2}T\S+\s+')


class LogUnavailable(RuntimeError):
    """The job log endpoint reports that its log is missing or expired."""


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
        if re.fullmatch(r'repos/[^/]+/[^/]+/contents/\.github/workflows/[\w.-]+\.ya?ml', route):
            safe_path = 'repos/{repo}/contents/{workflow}'
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
            if (method == 'GET' and status in {'404', '410'} and re.fullmatch(
                    r'repos/[^/]+/[^/]+/actions/jobs/[0-9]+/logs(?:\?.*)?', path)):
                raise LogUnavailable('GitHub job log unavailable')
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


def _clock():
    return time.monotonic()


def _sleep(seconds):
    time.sleep(seconds)


def _gate(run, repo, expected_attempt, max_attempts=MAX_ATTEMPTS):
    if type(max_attempts) is not int or max_attempts < 1:
        return 'invalid_max_attempts'
    if run['repository']['full_name'] != repo:
        return 'repository_mismatch'
    if run['status'] != 'completed' or run['conclusion'] != 'failure':
        return 'not_a_completed_failed_run'
    attempt = int(run['run_attempt'])
    if not 1 <= attempt < max_attempts:
        return 'attempt_limit'
    if expected_attempt is not None and attempt != expected_attempt:
        return 'stale_completion_event'
    if run['event'] not in ALLOWED_WORKFLOWS.get(run['path'], ()):
        return 'workflow_or_event_not_allowlisted'
    if run['path'] == '.github/workflows/ci-post.yml' and run['head_branch'] != PRIMARY_BRANCH:
        return 'not_the_primary_branch'
    return None


def _loss(message):
    return any(pattern.fullmatch(message) for pattern in LOSS_PATTERNS)


class MappingError(ValueError):
    def __init__(self, job_id=None):
        super().__init__('workflow mapping unavailable')
        self.job_id = job_id


class BlockScalar(str):
    """Inert script/condition text, never a job or matrix identity."""


def _flow_parts(source):
    """Split flow collections without treating quoted commas as separators."""
    parts, start, stack, quote = [], 0, [], None
    i = 0
    while i < len(source):
        char = source[i]
        if quote:
            if char == '\\' and quote == '"':
                i += 2
                continue
            if char == quote:
                if quote == "'" and source[i:i + 2] == "''":
                    i += 2
                    continue
                quote = None
        elif char in "\"'":
            quote = char
        elif char in '[{':
            stack.append(char)
        elif char in ']}':
            if not stack or stack.pop() != {']': '[', '}': '{'}[char]:
                raise MappingError()
        elif char == ',' and not stack:
            parts.append(source[start:i].strip())
            start = i + 1
        i += 1
    if quote or stack:
        raise MappingError()
    tail = source[start:].strip()
    if tail:
        parts.append(tail)
    if any(not part for part in parts):
        raise MappingError()
    return parts


def _pair(source):
    match = re.fullmatch(r'([A-Za-z_][A-Za-z0-9_-]*):(?:\s+(.*))?', source)
    if not match:
        raise MappingError()
    return match[1], match[2] or ''


def _scalar(source):
    if not source or source[0] in '&*!|>@`%':
        raise MappingError()
    if source[0] in '[{':
        close = ']' if source[0] == '[' else '}'
        if not source.endswith(close):
            raise MappingError()
        parts = _flow_parts(source[1:-1])
        if close == ']':
            return [_scalar(part) for part in parts]
        result = {}
        for part in parts:
            key, value = _pair(part)
            if key in result:
                raise MappingError()
            result[key] = _scalar(value)
        return result
    if source.startswith('"'):
        value = json.loads(source)
        if not isinstance(value, str):
            raise MappingError()
        return value
    if source.startswith("'"):
        if not re.fullmatch(r"'(?:[^']|'')*'", source):
            raise MappingError()
        return source[1:-1].replace("''", "'")
    if re.search(r':(?:\s|$)', source):
        raise MappingError()
    # Keep numeric/boolean matrix values in their display spelling.
    return None if source in ('null', '~') else source


def _uncomment(source):
    quote = None
    i = 0
    while i < len(source):
        char = source[i]
        if quote:
            if char == '\\' and quote == '"':
                i += 2
                continue
            if char == quote:
                if quote == "'" and source[i:i + 2] == "''":
                    i += 2
                    continue
                quote = None
        elif char in "\"'":
            quote = char
        elif char == '#' and (i == 0 or source[i - 1].isspace()):
            return source[:i].rstrip()
        i += 1
    return source.rstrip()


class WorkflowYAML:
    """Strict, bounded YAML subset; unsupported syntax refuses classification.

    Block/flow maps and lists, quoted scalars and literal/folded blocks cover
    these workflows. No aliases, merges, tags, multiline flow values or
    expression evaluation. Scripts stay inert strings, never Python/shell.
    """

    def __init__(self, source):
        self.rows, self.pos = [], 0
        lines = source.splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]
            i += 1
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            prefix = line[:len(line) - len(line.lstrip())]
            if '\t' in prefix:
                raise MappingError()
            indent = len(prefix)
            value = _uncomment(line[indent:])
            block = None
            if re.search(r': [|>][+-]?$', value):
                limit = indent + (2 if value.startswith('- ') else 0)
                content = []
                while i < len(lines):
                    next_line = lines[i]
                    if next_line.strip() and len(next_line) - len(next_line.lstrip()) <= limit:
                        break
                    content.append(next_line)
                    i += 1
                nonempty = [row for row in content if row.strip()]
                if nonempty:
                    margin = len(nonempty[0]) - len(nonempty[0].lstrip(' '))
                    if any(row[:margin] != ' ' * margin for row in nonempty):
                        raise MappingError()
                block = textwrap.dedent('\n'.join(content))
                if value.rsplit(' ', 1)[1].startswith('>'):
                    block = ' '.join(block.splitlines())
                block = BlockScalar(block)
            self.rows.append((indent, value, block))
        if len(self.rows) > 20000:
            raise MappingError()

    def _value(self, value, block, indent):
        if block is not None:
            return block
        if value:
            return _scalar(value)
        if self.pos < len(self.rows) and self.rows[self.pos][0] > indent:
            return self._node(self.rows[self.pos][0])
        return None

    def _node(self, indent):
        is_list = self.rows[self.pos][1].startswith('- ')
        result = [] if is_list else {}
        while self.pos < len(self.rows):
            level, value, block = self.rows[self.pos]
            if level < indent:
                break
            if level != indent or value.startswith('- ') != is_list:
                raise MappingError()
            self.pos += 1
            if is_list:
                value = value[2:]
                if re.match(r'[A-Za-z_][A-Za-z0-9_-]*:', value):
                    self.pos -= 1
                    self.rows[self.pos] = (indent + 2, value, block)
                    result.append(self._node(indent + 2))
                else:
                    result.append(self._value(value, block, indent))
            else:
                key, value = _pair(value)
                if key in result:
                    raise MappingError()
                result[key] = self._value(value, block, indent)
        return result

    def read(self):
        if not self.rows or self.rows[0][0] != 0:
            raise MappingError()
        result = self._node(0)
        if self.pos != len(self.rows) or not isinstance(result, dict):
            raise MappingError()
        return result


def _matrix_rows(spec):
    matrix = spec.get('strategy', {}).get('matrix', {})
    if not isinstance(matrix, dict):
        raise MappingError()
    axes = {key: value for key, value in matrix.items() if key not in ('include', 'exclude')}
    count = 1
    for values in axes.values():
        if not isinstance(values, list) or not values:
            raise MappingError()
        count *= len(values)
        if count > 256:
            raise MappingError()
    excluded, included = matrix.get('exclude', []), matrix.get('include', [])
    if any(not isinstance(items, list) or any(not isinstance(x, dict) for x in items)
           for items in (excluded, included)):
        raise MappingError()
    originals = [dict(zip(axes, values)) for values in itertools.product(*axes.values())]
    originals = [row for row in originals
                 if not any(all(row.get(k) == v for k, v in ex.items()) for ex in excluded)]
    if not axes and included:
        originals = []
    rows = [dict(row) for row in originals]
    for extra in included:
        matches = [i for i, row in enumerate(originals)
                   if all(k not in row or row[k] == v for k, v in extra.items())]
        for i in matches:
            rows[i].update(extra)
        if not matches:
            rows.append(dict(extra))
    if not rows or len(rows) > 256:
        raise MappingError()
    return rows


def _job_names(job_id, spec):
    names = []
    for row in _matrix_rows(spec):
        name = spec.get('name', job_id)
        if type(name) is not str:
            raise MappingError()

        def substitute(match):
            value = row
            for key in match[1].split('.'):
                value = value[key]
            if type(value) is not str:
                raise MappingError()
            return value

        name = re.sub(r'\$\{\{\s*matrix\.([A-Za-z0-9_.-]+)\s*\}\}', substitute, name)
        if 'name' not in spec and row:
            if any(type(value) is not str for value in row.values()):
                raise MappingError()
            name += ' (' + ', '.join(row.values()) + ')'
        if not name or '${{' in name:
            raise MappingError()
        names.append(name)
    return names


def _read_workflow(api, repo, sha, path):
    # Same-repository workflow files only, pinned to the admitted run's SHA.
    # Reusable references never become a checkout, command, or arbitrary URL.
    if not re.fullmatch(r'\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml', path):
        raise MappingError()
    data = api.get(f'repos/{repo}/contents/{path}?ref={sha}')
    if (data['type'] != 'file' or data['path'] != path or data['encoding'] != 'base64'
            or len(data['content']) > 2 * 1024 * 1024):
        raise MappingError()
    source = base64.b64decode(data['content'].replace('\n', ''), validate=True).decode('utf-8')
    specs = WorkflowYAML(source).read()['jobs']
    if not isinstance(specs, dict) or not specs:
        raise MappingError()
    for job_id, spec in specs.items():
        if not isinstance(spec, dict):
            raise MappingError()
        needs = spec.get('needs', [])
        needs = [needs] if type(needs) is str else needs
        if not isinstance(needs, list) or any(type(n) is not str or n not in specs or n == job_id
                                              for n in needs):
            raise MappingError()
        if 'uses' in spec and (type(spec['uses']) is not str or not re.fullmatch(
                r'\./\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml', spec['uses'])):
            raise MappingError()
        spec['needs'] = needs
        spec['names'] = _job_names(job_id, spec)
    visiting, visited = set(), set()

    def visit(job_id):
        if job_id in visiting:
            raise MappingError()
        if job_id not in visited:
            visiting.add(job_id)
            for need in specs[job_id]['needs']:
                visit(need)
            visiting.remove(job_id)
            visited.add(job_id)

    for job_id in specs:
        visit(job_id)
    return specs


def _workflow_jobs(api, repo, run):
    sha = run['head_sha']
    if not re.fullmatch(r'[0-9a-fA-F]{40}', sha):
        raise MappingError()
    cache, expanded = {}, {}

    def expand(path, scope='', display='', stack=()):
        if path in stack or len(stack) >= 10:
            raise MappingError()
        if path not in cache:
            if len(cache) >= 32:
                raise MappingError()
            cache[path] = _read_workflow(api, repo, sha, path)
        members = set()
        for job_id, spec in cache[path].items():
            key = scope + job_id
            if key in expanded or len(expanded) >= 20000:
                raise MappingError()
            node = dict(spec, needs=[scope + need for need in spec['needs']],
                        names=[display + name for name in spec['names']], members={key})
            expanded[key] = node
            if 'uses' in spec:
                # Caller needs stay on this node. Child needs resolve only
                # inside this invocation, including matrix caller instances.
                if len(set(node['names'])) != len(node['names']):
                    raise MappingError()
                for index, name in enumerate(node['names']):
                    instance = key if len(node['names']) == 1 else f'{key}[{index}]'
                    node['members'].update(expand(
                        spec['uses'][2:], instance + '/', name + ' / ', stack + (path,)))
            members.update(node['members'])
        return members

    expand(run['path'])  # Already admitted by _gate's exact workflow allowlist.
    return expanded


def _map_jobs(specs, jobs):
    mapped = {}
    for job in jobs:
        matches = set()
        for job_id, spec in specs.items():
            for name in spec['names']:
                if job['name'] == name:
                    matches.add(job_id)
        if len(matches) != 1:
            raise MappingError(int(job['id']))
        mapped[int(job['id'])] = matches.pop()
    return mapped


def _gate_step(job):
    steps = job.get('steps', [])
    failed = [step for step in steps if step.get('conclusion') == 'failure']
    if len(failed) != 1 or any(step.get('conclusion') not in ('success', 'skipped', 'failure', 'neutral')
                               for step in steps):
        return False
    name = failed[0].get('name', '')
    return isinstance(name, str) and name.startswith('Require every')


def _gate_kind(job, reason, messages):
    # Runner loss is classified first. Only ordinary gate exits may remain;
    # independent diagnostics from either logs or annotations still refuse.
    if (reason not in ('independent_failure_diagnostic', 'no_runner_loss_evidence') or not messages
            or any(not GATE_EXIT.fullmatch(message) for message in messages)):
        return None
    return 'step' if _gate_step(job) else None


def _loss_reason(messages):
    if any(not _loss(message) and not LOSS_FALLOUT.fullmatch(message) for message in messages):
        return 'independent_failure_diagnostic'
    if not any(_loss(message) for message in messages):
        return 'no_runner_loss_evidence'
    return None


def _job_evidence(api, repo, job, *, diagnostics=None):
    if not any(FLEET_LABEL.fullmatch(label) for label in job['labels']):
        return 'not_a_fleet_amd64_job'
    if sum(step.get('conclusion') == 'failure' for step in job.get('steps', [])) > 1:
        return 'multiple_failed_steps'
    check_url = job['check_run_url']
    prefix = f'https://api.github.com/repos/{repo}/check-runs/'
    if not check_url.startswith(prefix) or not check_url[len(prefix):].isdigit():
        return 'invalid_check_run_identity'
    gate = _gate_step(job)
    messages = []
    try:
        annotations = api.pages(check_url.removeprefix('https://api.github.com/') + '/annotations?per_page=100')
    except PermissionError:
        if gate:
            raise
        # Shards retain optional annotations and mandatory log inspection.
        annotations = []
    for page in annotations:
        for annotation in page:
            if annotation['annotation_level'] == 'failure':
                messages.extend(line.strip() for line in annotation['message'].splitlines() if line.strip())
    has_annotations = bool(messages)
    try:
        logs = api.text(f'repos/{repo}/actions/jobs/{int(job["id"])}/logs')
    except LogUnavailable:
        if not gate:
            raise
        # Only a missing/expired gate log permits annotation fallback.
        # Permission, rate-limit and transport failures still refuse.
        logs = None
    if logs is not None:
        if len(logs) > 32 * 1024 * 1024:
            return 'log_too_large_to_classify'
        for line in logs.splitlines():
            line = TIMESTAMP.sub('', line).strip()
            if line.startswith('##[error]'):
                messages.append(line.removeprefix('##[error]').strip())
            elif _loss(line):
                messages.append(line)
    if diagnostics is not None:
        diagnostics.extend(messages)
    reason = _loss_reason(messages)
    if reason is None:
        return None
    if gate and not has_annotations:
        return 'no_gate_annotation_evidence'
    return reason


def _jobs(api, endpoint, attempt):
    return [job for page in api.pages(f'{endpoint}/attempts/{attempt}/jobs?per_page=100')
            for job in page['jobs']]


def _run_changed(current, initial, repo, attempt, max_attempts=MAX_ATTEMPTS):
    identity = ('id', 'run_attempt', 'head_sha', 'path', 'event', 'head_branch')
    return _gate(current, repo, attempt, max_attempts=max_attempts) or any(
        current[key] != initial[key] for key in identity)


def _pr_identity(run, repo):
    repository = run.get('repository')
    head_repository = run.get('head_repository')
    if not isinstance(repository, dict) or not isinstance(head_repository, dict):
        return None, 'pull_request_metadata_unavailable'
    repository_name = repository.get('full_name')
    head_repository_name = head_repository.get('full_name')
    if not isinstance(repository_name, str) or not isinstance(head_repository_name, str):
        return None, 'pull_request_metadata_unavailable'
    if repository_name != repo or head_repository_name != repository_name:
        return None, 'pull_request_head_repository_mismatch'
    pull_requests = run.get('pull_requests')
    if (not isinstance(pull_requests, list) or len(pull_requests) != 1
            or not isinstance(pull_requests[0], dict)):
        return None, 'pull_request_metadata_unavailable'
    number = pull_requests[0].get('number')
    if type(number) is not int or number <= 0:
        return None, 'pull_request_metadata_unavailable'
    return number, None


def _same_run(left, right):
    left_id, right_id = left.get('id'), right.get('id')
    if left_id is None or right_id is None:
        return False
    if left_id == right_id:
        return True
    return (type(left_id) is int and type(right_id) is int
            and left_id > 0 and left_id == right_id)


def _created_at(value):
    if not isinstance(value, str):
        return None
    try:
        timestamp = datetime.fromisoformat(value.replace('Z', '+00:00'))
    except ValueError:
        return None
    return timestamp if timestamp.tzinfo is not None else None


def _newer_run(candidate, run):
    if _same_run(candidate, run):
        return False
    compared = False
    candidate_number, run_number = candidate.get('run_number'), run.get('run_number')
    if (type(candidate_number) is int and candidate_number > 0
            and type(run_number) is int and run_number > 0):
        compared = True
        if candidate_number > run_number:
            return True
    candidate_created, run_created = (_created_at(candidate.get('created_at')),
                                      _created_at(run.get('created_at')))
    if candidate_created is not None and run_created is not None:
        compared = True
        if candidate_created > run_created:
            return True
    return False if compared else None


def _pr_run_history(api, repo, run, number):
    head_branch = run.get('head_branch')
    if not isinstance(head_branch, str):
        return 'pull_request_metadata_unavailable'
    endpoint = (f'repos/{repo}/actions/workflows/gke-pr.yml/runs'
                f'?event={PR_EVENT}&branch={quote(head_branch, safe="")}&per_page=100')
    pages = api.pages(endpoint)
    if not isinstance(pages, list) or not pages:
        return 'pull_request_runs_unavailable'
    runs, total_count = [], None
    for page in pages:
        if not isinstance(page, dict) or not isinstance(page.get('workflow_runs'), list):
            return 'pull_request_runs_unavailable'
        page_total = page.get('total_count')
        if page_total is not None:
            if type(page_total) is not int or page_total < 0:
                return 'pull_request_runs_unavailable'
            total_count = page_total if total_count is None else max(total_count, page_total)
        runs.extend(page['workflow_runs'])
    if total_count is not None and len(runs) < total_count:
        return 'pull_request_runs_unavailable'
    found_current = False
    for candidate in runs:
        if not isinstance(candidate, dict):
            return 'pull_request_runs_unavailable'
        if _same_run(candidate, run):
            found_current = True
            continue
        pull_requests = candidate.get('pull_requests')
        if not isinstance(pull_requests, list):
            return 'pull_request_runs_unavailable'
        numbers = []
        for pull_request in pull_requests:
            if not isinstance(pull_request, dict):
                return 'pull_request_runs_unavailable'
            candidate_number = pull_request.get('number')
            if type(candidate_number) is not int or candidate_number <= 0:
                return 'pull_request_runs_unavailable'
            numbers.append(candidate_number)
        if number not in numbers:
            continue
        newer = _newer_run(candidate, run)
        if newer is None:
            return 'pull_request_runs_unavailable'
        if newer:
            return 'superseded_pull_request_head'
    return None if found_current else 'pull_request_runs_unavailable'


def _pr_guard(api, repo, run):
    number, reason = _pr_identity(run, repo)
    if reason:
        return reason
    head_sha = run.get('head_sha')
    if not isinstance(head_sha, str):
        return 'pull_request_metadata_unavailable'
    pull_request = api.get(f'repos/{repo}/pulls/{number}')
    if not isinstance(pull_request, dict) or pull_request.get('number') != number:
        return 'pull_request_metadata_unavailable'
    head = pull_request.get('head')
    head_repository = head.get('repo') if isinstance(head, dict) else None
    if not isinstance(head, dict) or not isinstance(head_repository, dict):
        return 'pull_request_metadata_unavailable'
    fork = head_repository.get('fork')
    if type(fork) is not bool:
        return 'pull_request_metadata_unavailable'
    if fork:
        return 'pull_request_is_fork'
    head_repository_name = head_repository.get('full_name')
    if not isinstance(head_repository_name, str):
        return 'pull_request_metadata_unavailable'
    if head_repository_name != repo:
        return 'pull_request_head_repository_mismatch'
    if pull_request.get('state') != 'open':
        return 'superseded_pull_request_head'
    current_head_sha = head.get('sha')
    if not isinstance(current_head_sha, str):
        return 'pull_request_metadata_unavailable'
    if current_head_sha != head_sha:
        return 'superseded_pull_request_head'
    return _pr_run_history(api, repo, run, number)


def _completed_jobs(api, endpoint, attempt, initial_run, repo, max_attempts=MAX_ATTEMPTS):
    jobs = _jobs(api, endpoint, attempt)
    if jobs and all(job['status'] == 'completed' for job in jobs):
        return jobs, None, None

    started = _clock()
    deadline = started + JOB_POLL_BOUND
    while True:
        remaining = deadline - _clock()
        if remaining <= 0:
            waited = round(max(0.0, _clock() - started), 3)
            return jobs, waited, 'missing_or_incomplete_jobs'
        _sleep(min(JOB_POLL_INTERVAL, remaining))
        waited = round(max(0.0, _clock() - started), 3)
        current = api.get(endpoint)
        if _run_changed(current, initial_run, repo, attempt, max_attempts=max_attempts):
            return jobs, waited, 'run_changed_during_classification'
        jobs = _jobs(api, endpoint, attempt)
        if jobs and all(job['status'] == 'completed' for job in jobs):
            return jobs, waited, None


def process(api, repo, run_id, *, dry_run=False, expected_attempt=None,
            max_attempts=MAX_ATTEMPTS):
    """Return a bounded decision; lookup ambiguity always refuses the POST."""
    result = {'run_id': run_id, 'dry_run': dry_run, 'rerun': False,
              'failed_job_ids': [], 'dependent_job_ids': []}
    try:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo) or int(run_id) <= 0:
            raise ValueError('invalid repository or run id')
        if type(max_attempts) is not int or max_attempts < 1:
            result['reason'] = 'invalid_max_attempts'
            return result
        endpoint = f'repos/{repo}/actions/runs/{int(run_id)}'
        run = api.get(endpoint)
        reason = _gate(run, repo, expected_attempt, max_attempts=max_attempts)
        result['run_attempt'] = run['run_attempt']
        if reason:
            result['reason'] = reason
            return result
        is_pr = run['path'] == PR_WORKFLOW
        if is_pr:
            reason = _pr_guard(api, repo, run)
            if reason:
                result['reason'] = reason
                return result
        attempt = int(run['run_attempt'])
        jobs, waited, reason = _completed_jobs(
            api, endpoint, attempt, run, repo, max_attempts=max_attempts)
        if waited is not None:
            result['waited_seconds'] = waited
        if reason:
            result['reason'] = reason
            return result
        if any(job['conclusion'] not in {'success', 'skipped', 'neutral', 'failure'} for job in jobs):
            result['reason'] = 'ambiguous_unsuccessful_job'
            return result
        failed = [job for job in jobs if job['conclusion'] == 'failure']
        result['failed_job_ids'] = [int(job['id']) for job in failed]
        if not failed:
            result['reason'] = 'no_failed_jobs'
            return result
        try:
            specs = _workflow_jobs(api, repo, run)
            # Map failures first so an unresolvable failure names its blocker.
            mapped = _map_jobs(specs, failed)
            # A skipped matrix row may collapse to an aggregate display name
            # when a docs-only condition is evaluated before expansion. It
            # cannot be a failed DAG node, so only non-skipped jobs need aliases.
            mapped.update(_map_jobs(specs, [j for j in jobs
                                           if j not in failed and j['conclusion'] != 'skipped']))
            failed_needs = {
                int(job['id']): {int(other['id']) for other in failed
                                if any(mapped[int(other['id'])] in specs[need]['members']
                                       for need in specs[mapped[int(job['id'])]]['needs'])}
                for job in failed
            }
            # Unknown needs or a cycle are mapping failures, before any job
            # can be excused. Matrix/caller dependencies come from the DAG.
            order = tuple(TopologicalSorter(failed_needs).static_order())
        except Exception as error:
            blocker = getattr(error, 'job_id', None) or int(failed[0]['id'])
            result.update(reason='aggregate_mapping_unavailable', blocked_job_id=blocker)
            return result
        reasons, kinds, lost = {}, {}, set()
        for job in failed:
            job_id = int(job['id'])
            messages = []
            reason = _job_evidence(api, repo, job, diagnostics=messages)
            reasons[job_id] = reason
            if reason is None:
                lost.add(job_id)
            else:
                kinds[job_id] = _gate_kind(job, reason, messages)
        # On this DAG, one topological pass reaches the fixed point: every
        # failed need has already been proved lost/dependent or left blocked.
        dependent = set()
        for job_id in order:
            if kinds.get(job_id) == 'step' and failed_needs[job_id] <= lost | dependent:
                dependent.add(job_id)
        result['dependent_job_ids'] = [int(j['id']) for j in failed if int(j['id']) in dependent]
        blocked = [int(j['id']) for j in failed if int(j['id']) not in lost | dependent]
        if blocked:
            # Report an own failure before a gate blocked by that failure,
            # independently of the API's pagination or completion order.
            own = [j for j in blocked if not kinds.get(j)]
            # Positive independent-error proof is more informative than a
            # missing runner-loss marker. Both still refuse the whole run.
            blocker = next((j for j in own if reasons[j] == 'independent_failure_diagnostic'),
                           (own or blocked)[0])
            result.update(reason=reasons[blocker], blocked_job_id=blocker)
            return result
        if not lost:
            result.update(reason='no_runner_loss_jobs', blocked_job_id=int(failed[0]['id']))
            return result
        # Re-read just before mutation; concurrency serializes controller runs too.
        current = api.get(endpoint)
        if _run_changed(current, run, repo, attempt, max_attempts=max_attempts):
            result['reason'] = 'run_changed_during_classification'
            return result
        if is_pr:
            reason = _pr_guard(api, repo, current)
            if reason:
                result['reason'] = reason
                return result
        if not dry_run:
            api.post(f'{endpoint}/rerun-failed-jobs')
        result.update(rerun=True, reason=('runner_loss_with_dependents' if dependent
                                         else 'all_failed_jobs_are_runner_loss'))
    except Exception as error:
        result.update(rerun=False, reason='lookup_or_request_failed', error_type=type(error).__name__)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'), required=not os.environ.get('GITHUB_REPOSITORY'))
    parser.add_argument('--run-id', required=True, type=int)
    parser.add_argument('--expected-attempt', type=int)
    parser.add_argument('--max-attempts', type=int, default=MAX_ATTEMPTS)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    decision = process(GitHubAPI(), args.repo, args.run_id, dry_run=args.dry_run,
                       expected_attempt=args.expected_attempt,
                       max_attempts=args.max_attempts)
    print(json.dumps(decision, sort_keys=True))
    # Refusals, including lookup failures, are successful controller decisions.
    # Their JSON diagnostics remain visible; missing proof never permits a POST.
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
