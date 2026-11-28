#!/usr/bin/env python3
"""Baseline promotion guard: the pull-request lookup retry and the release-commit skip.

Every row takes the `run:` script of the "Resolve the merged PR" step out of
.github/workflows/promote-baseline.yml by step name and runs it with bash.
Only the transport is fake: a `gh` that answers from a fixed, ordered reply
list and a `sleep` that records its arguments and returns at once, both first
on PATH. Nothing here reaches GitHub or waits. The rows need pytest (tmp_path).
"""
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / '.github/workflows/promote-baseline.yml'
STEP = 'Resolve the merged PR'
REPO = 'fixture/project'
SHA = 'a' * 40
HEAD = 'b' * 40
RELEASE_APP = 'pineforge-app[bot]'
MAINTAINER = 'fixture-maintainer'
RELEASE_FILES = ('VERSION', 'docker/run_failure_codes_diff.json')
RELEASE_SKIP = ('release commit: baseline is recorded by the release_tag route, '
                'not post-merge promote\n')
DEFERRED = f'DEFERRED_NOT_FOUND: no merged PR found for {SHA} after 3 attempts; baseline unchanged'
REFUSAL_OUTPUT = f'::error title=baseline-promotion::{DEFERRED}\n'
REFUSAL_SUMMARY = f'baseline promotion: {DEFERRED}\n'
NO_MATCH_SKIP = '::notice::found {count} merged PRs with merge commit ' + SHA + ' and base main; skipping\n'

# Each request consumes the next reply; another request, or one too many,
# exits 93 instead of reaching a real gh. The replies are part of this file:
# it follows no path given by a command or by the environment.
GH_BODY = '''import json, pathlib, sys
replies = json.loads(REPLIES)
log = pathlib.Path(__file__).with_name("gh-calls")
done = len(log.read_text().splitlines()) if log.exists() else 0
args = sys.argv[1:]
with log.open("a") as stream:
    stream.write(json.dumps(args) + "\\n")
if done >= len(replies) or replies[done]["args"] != args:
    sys.stderr.write("fixture: unexpected gh request: " + " ".join(args) + "\\n")
    raise SystemExit(93)
sys.stdout.write(replies[done]["stdout"])
raise SystemExit(replies[done]["status"])
'''
SLEEP_BODY = '''import json, pathlib, sys
with pathlib.Path(__file__).with_name("sleep-calls").open("a") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
'''


def step_script(name=STEP):
    """The named step's `run:` block, as the runner hands it to bash."""
    lines = WORKFLOW.read_text(encoding='utf-8').split('\n')
    heads = [index for index, line in enumerate(lines) if line == f'      - name: {name}']
    assert len(heads) == 1, f'the workflow must name the step {name!r} exactly once'
    body = lines[heads[0] + 1:]
    # The step ends where the next step or job begins.
    end = next((index for index, line in enumerate(body)
                if line.strip() and len(line) - len(line.lstrip(' ')) <= 6), len(body))
    body = body[:end]
    assert body.count('        run: |') == 1, 'the step must have one literal run block'
    block = body[body.index('        run: |') + 1:]
    assert all(not line.strip() or line.startswith(' ' * 10) for line in block), (
        'the run block must be the last key of the step')
    script = '\n'.join(line[10:] for line in block).rstrip('\n') + '\n'
    assert '${{' not in script, 'the run block must read its inputs from env'
    return script


def merged_pr(number=701):
    return {'number': number, 'state': 'closed', 'merged': True, 'merge_commit_sha': SHA,
            'base': {'ref': 'main'},
            'head': {'sha': HEAD, 'repo': {'full_name': REPO, 'fork': False}},
            'merged_at': '2026-11-04T09:10:00Z',
            'html_url': f'https://github.com/{REPO}/pull/{number}',
            'author_association': 'MEMBER', 'user': {'login': 'fixture-author'},
            'merged_by': {'login': 'fixture-merger'}}


def summary_of(pr):
    """The association answer lists a summary: no `merged`, no `merged_by`."""
    return {key: pr[key] for key in
            ('number', 'state', 'merge_commit_sha', 'base', 'head', 'merged_at')}


def lookup(pages):
    return {'args': ['api', '--paginate', '--slurp',
                     f'repos/{REPO}/commits/{SHA}/pulls?per_page=100'],
            'stdout': json.dumps(pages), 'status': 0}


def full_read(pr):
    return {'args': ['api', f'repos/{REPO}/pulls/{pr["number"]}'],
            'stdout': json.dumps(pr), 'status': 0}


def commit_read(paths):
    """The pushed commit's own answer. Its subject is release-shaped in every
    row, so a refused row also shows that the subject alone never counts."""
    commit = {'sha': SHA, 'commit': {'message': 'chore(release): v9.9.9'},
              'files': [{'filename': path, 'status': 'modified'} for path in paths]}
    return {'args': ['api', '--paginate', '--slurp',
                     f'repos/{REPO}/commits/{SHA}?per_page=100'],
            'stdout': json.dumps([commit]), 'status': 0}


def args_of(replies):
    return [reply['args'] for reply in replies]


def retry_notice(attempt):
    return (f'::notice::No merged PR found for {SHA} (attempt {attempt}/3); '
            'retrying in 20 seconds\n')


def resolved_output(pr):
    return ''.join(f'{key}={value}\n' for key, value in (
        ('merge_sha', SHA), ('head_sha', HEAD), ('pr_number', pr['number']),
        ('pr_url', pr['html_url']), ('merged_at', pr['merged_at']),
        ('head_repo', REPO), ('head_is_fork', 'false'), ('association', 'MEMBER'),
        ('pr_author', 'fixture-author'), ('merged_by', 'fixture-merger'), ('ok', 'true')))


def write_tool(path, body):
    path.write_text('#!' + sys.executable + ' -I\n' + body, encoding='utf-8')
    path.chmod(0o755)


def run_step(tmp_path, replies, *, actor=MAINTAINER):
    """Run the step as a push to main by `actor`, with only the fake transport."""
    tools = tmp_path / 'tools'
    tools.mkdir()
    write_tool(tools / 'gh', f'REPLIES = {json.dumps(replies)!r}\n' + GH_BODY)
    write_tool(tools / 'sleep', SLEEP_BODY)
    output, summary = tmp_path / 'github-output', tmp_path / 'step-summary'
    # A closed environment: the fakes come first on PATH, and nothing of the
    # caller's environment reaches the step.
    env = {
        'PATH': os.pathsep.join([str(tools), str(Path(sys.executable).parent), '/usr/bin', '/bin']),
        'LC_ALL': 'C', 'GH_TOKEN': 'fixture-token',
        'EVENT_NAME': 'push', 'REF': 'refs/heads/main', 'REPO': REPO, 'SHA': SHA,
        'PUSH_ACTOR': actor,
        'INPUT_MERGE_SHA': '', 'INPUT_HEAD_SHA': '', 'INPUT_PR_NUMBER': '',
        'INPUT_PR_URL': '', 'INPUT_MERGED_AT': '',
        'GITHUB_OUTPUT': str(output), 'GITHUB_STEP_SUMMARY': str(summary),
    }
    ran = subprocess.run(
        ['bash', '--noprofile', '--norc', '-eo', 'pipefail', '-c', step_script()],
        env=env, cwd=tmp_path, text=True, capture_output=True, timeout=300)

    def text(path):
        return path.read_text(encoding='utf-8') if path.exists() else ''

    def recorded(name):
        return [json.loads(line) for line in text(tools / name).splitlines()]

    return SimpleNamespace(code=ran.returncode, stdout=ran.stdout, stderr=ran.stderr,
                           output=text(output), summary=text(summary),
                           calls=recorded('gh-calls'), sleeps=recorded('sleep-calls'))


def test_empty_first_answer_is_retried_and_the_merged_pr_is_found(tmp_path):
    pr = merged_pr()
    replies = [lookup([[]]), lookup([[summary_of(pr)]]), full_read(pr)]
    ran = run_step(tmp_path, replies)
    assert ran.stderr == ''
    assert ran.calls == args_of(replies)
    assert ran.sleeps == [['20']]
    assert ran.stdout == retry_notice(1)
    assert ran.output == resolved_output(pr)
    assert ran.summary == ''
    assert ran.code == 0


def test_three_empty_answers_refuse_with_deferred_not_found(tmp_path):
    replies = [lookup([[]]), lookup([[]]), lookup([[]])]
    ran = run_step(tmp_path, replies)
    assert ran.stderr == ''
    assert ran.calls == args_of(replies)
    assert ran.sleeps == [['20'], ['20']]
    assert ran.stdout == retry_notice(1) + retry_notice(2) + REFUSAL_OUTPUT
    assert ran.output == 'ok=false\n'
    assert ran.summary == REFUSAL_SUMMARY
    assert ran.code == 1


def test_non_empty_answer_with_no_match_skips_once_without_a_retry(tmp_path):
    pr = merged_pr()
    other = summary_of(dict(pr, merge_commit_sha='c' * 40))
    # The later replies would resolve the PR: a retry would consume them.
    replies = [lookup([[other]]), lookup([[summary_of(pr)]]), full_read(pr)]
    ran = run_step(tmp_path, replies)
    assert ran.stderr == ''
    assert ran.calls == args_of(replies[:1])
    assert ran.sleeps == []
    assert ran.stdout == NO_MATCH_SKIP.format(count=0)
    assert ran.output == 'ok=false\n'
    assert ran.summary == ''
    assert ran.code == 0


def test_two_matching_prs_still_defer(tmp_path):
    first, second = merged_pr(), merged_pr(number=702)
    replies = [lookup([[summary_of(first)], [summary_of(second)]]), full_read(first)]
    ran = run_step(tmp_path, replies)
    assert ran.stderr == ''
    assert ran.calls == args_of(replies[:1])
    assert ran.sleeps == []
    assert ran.stdout == NO_MATCH_SKIP.format(count=2)
    assert ran.output == 'ok=false\n'
    assert ran.summary == ''
    assert ran.code == 0


def test_release_shaped_commit_from_the_release_app_skips_green(tmp_path):
    replies = [lookup([[]]), lookup([[]]), lookup([[]]), commit_read(RELEASE_FILES)]
    ran = run_step(tmp_path, replies, actor=RELEASE_APP)
    assert ran.stderr == ''
    assert ran.calls == args_of(replies)
    assert ran.sleeps == [['20'], ['20']]
    assert ran.stdout == retry_notice(1) + retry_notice(2) + RELEASE_SKIP
    assert ran.output == 'ok=false\n'
    assert ran.summary == ''
    assert ran.code == 0


def test_release_files_pushed_by_another_actor_are_refused(tmp_path):
    # The commit answer is release-shaped: only the push actor differs.
    replies = [lookup([[]]), lookup([[]]), lookup([[]]), commit_read(RELEASE_FILES)]
    ran = run_step(tmp_path, replies, actor=MAINTAINER)
    assert ran.stderr == ''
    assert ran.calls[:3] == args_of(replies[:3]) and len(ran.calls) <= len(replies)
    assert ran.sleeps == [['20'], ['20']]
    assert ran.stdout == retry_notice(1) + retry_notice(2) + REFUSAL_OUTPUT
    assert ran.output == 'ok=false\n'
    assert ran.summary == REFUSAL_SUMMARY
    assert ran.code == 1


def test_release_app_commit_with_one_extra_code_file_is_refused(tmp_path):
    replies = [lookup([[]]), lookup([[]]), lookup([[]]),
               commit_read((*RELEASE_FILES, 'src/engine_run.cpp'))]
    ran = run_step(tmp_path, replies, actor=RELEASE_APP)
    assert ran.stderr == ''
    assert ran.calls == args_of(replies)
    assert ran.sleeps == [['20'], ['20']]
    assert ran.stdout == retry_notice(1) + retry_notice(2) + REFUSAL_OUTPUT
    assert ran.output == 'ok=false\n'
    assert ran.summary == REFUSAL_SUMMARY
    assert ran.code == 1


if __name__ == '__main__':
    raise SystemExit(pytest.main(['-q', '-p', 'no:cacheprovider', __file__]))
