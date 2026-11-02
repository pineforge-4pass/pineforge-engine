#!/usr/bin/env python3
"""Whether a CI run's change is documentation only, for .github/workflows/ci.yml.

The workflow's ``changes`` job runs this script before anything else. On a
docs-only change the proof jobs -- the four ``build`` legs, ``sanitizers``,
``kernel-only``, ``native-live`` and ``corpus-parity-subset`` -- are skipped,
and the ``build`` aggregate accepts exactly that skip. ``preflight`` still runs,
and page checks outside PROOF_READS are among its stages
(doc-anchors, doc-lint, doc-reverts, doc-pine-coverage, design-inventory,
kernel-seam-rows, the native C surface table), as are the workflow wiring
guards. PROOF_READS lists documentation inputs with checks outside preflight,
in a proof job's CTest row or the docs workflow. ``docs.yml``, the Doxygen
build, runs as before.

A changed path is documentation when it is a Markdown file (``*.md``) or lies
under ``docs/``, unless

* it lies under ``.github/``, ``benchmarks/``, ``scripts/`` or ``tests/``: a CI
  change runs CI, and the Markdown there is read by the proof jobs' own rows
  (the benchmark provenance README, the twin-parity ledger, fixture READMEs);
* it is a CMake file or a git control file (``.gitattributes`` decides how a
  checkout writes the files beside it); or
* it is listed in PROOF_READS (each with its check's home).

A change is docs-only when it changes at least one path and every path it
changes, both sides of a rename, is documentation. Whatever the script cannot
establish answers "not docs-only", and every proof job runs: an event other
than a pull request or a push (a manual dispatch runs everything), a push that
created the branch, was forced or carried more than one commit, a pull request
whose checked-out merge is not the merge of its head, a commit git cannot read,
an empty change.

The change is HEAD, checked out with its parents, against its first parent:
for a pull request the merge GitHub tests against the base it merged into,
exactly what the tested tree changes; for a push the pushed commit against
``before``, which must be that parent.

The workflow runs the copy of this script that the first parent holds, with
``--root`` naming the checkout, so a change to the rule is judged by the rule
it changes and never by itself; a base without the script runs everything.

    python3 ci_docs_only.py [--root DIR] [--github-output FILE]
    python3 ci_docs_only.py --paths FILE [--github-output FILE]

reads GITHUB_EVENT_NAME, and PR_HEAD_SHA (pull request) or PUSH_BEFORE and
PUSH_FORCED (push), from the environment. It prints its reasons, appends
``docs_only=true|false``, ``github_only=true|false`` and
``arm_relevant=true|false`` to FILE, and exits 0
whatever it decides. ``--paths`` reads a complete NUL-separated list as data;
an unreadable or unterminated list is neither class. The classifier is held by
scripts/test_ci_docs_only.py (a ci_preflight stage).

Arm proof is required for source/build inputs and test scripts, and for any
path the rule cannot classify. Missing or empty change data also requires it.
An established docs-only or GitHub-only PR never requires arm proof.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
# CI itself, and Markdown the proof jobs' own rows read: never documentation.
NOT_DOCUMENTATION_DIRS = ('.github/', 'benchmarks/', 'scripts/', 'tests/')
ARM_SOURCE_DIRS = ('tests/', 'runner/', 'include/', 'src/', 'docker/')
ARM_SOURCE_EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.cmake')
# Documentation inputs whose checks run outside preflight, in a proof job's
# CTest row or the docs workflow, with each input's check home. These inputs
# remain excluded from docs-only skips.
PROOF_READS = {
    'docs/adr/0001-kernel-adapter-boundary.md':
        'test_kernel_residuals and test_native_feature_rulings hold the kernel against '
        "its tables, and so does the kernel profile's kernel-residuals stage",
    'docs/build.sh':
        'test_docs_doxygen_retry runs its --self-test-retry and '
        'the docs.yml build job runs its --self-test-seo before building the site',
    'docs/pages/native-engine.md':
        'test_native_engine_complete_host compiles and runs its Complete host block',
    'docs/pages/pine-to-native.md':
        'test_pine_to_native_worked compiles and runs its worked migration',
    'docs/seo_postprocess.py':
        'the docs.yml build job executes it on synthetic pages through '
        'docs/build.sh --self-test-seo before building the site',
}
SHA = re.compile(r'[0-9a-f]{40}')
NO_COMMIT = '0' * 40
SHOWN = 40


class Doubt(Exception):
    """The change cannot be established as docs-only."""


def is_documentation(path: str) -> bool:
    name = path.rsplit('/', 1)[-1]
    if (path.startswith(NOT_DOCUMENTATION_DIRS) or path in PROOF_READS
            or name == 'CMakeLists.txt' or name.endswith('.cmake') or name.startswith('.git')):
        return False
    return path.startswith('docs/') or name.endswith('.md')


def is_arm_relevant(path: str) -> bool:
    """Recognize source/test inputs; unknown paths require arm proof too."""
    name = path.rsplit('/', 1)[-1]
    if (path.startswith(ARM_SOURCE_DIRS) or path.startswith('scripts/test_')
            or name == 'CMakeLists.txt' or name.endswith(ARM_SOURCE_EXTENSIONS)):
        return True
    return not (is_documentation(path) or path.startswith('.github/'))


def classify_paths(paths: list[str], *, pull_request: bool = True) -> tuple[bool, bool, bool]:
    """Return ``(docs_only, github_only, arm_relevant)`` for collected paths.

    The caller supplies both sides of a rename. No path is normalized: names
    containing newlines, quotes or ``..`` are still names, and only the exact
    directory prefixes below classify them.
    """
    if not paths:
        return False, False, True
    docs_only = all(is_documentation(path) for path in paths)
    github_only = pull_request and all(path.startswith('.github/') for path in paths)
    arm_relevant = not (docs_only or github_only) and any(is_arm_relevant(path) for path in paths)
    return docs_only, github_only, arm_relevant


def read_paths_file(path: Path) -> tuple[list[str] | None, str]:
    """Read one complete NUL-separated path file without treating names as code."""
    try:
        data = path.read_bytes()
    except OSError as error:
        return None, f'path list is unreadable: {error}'
    if not data:
        return [], 'path list is empty'
    if not data.endswith(b'\0'):
        return None, 'path list is truncated (missing its final NUL)'
    try:
        values = data[:-1].split(b'\0')
        paths = [value.decode('utf-8') for value in values]
    except (UnicodeDecodeError, ValueError) as error:
        return None, f'path list is unreadable: {error}'
    if any(not value for value in paths):
        return None, 'path list contains an empty path'
    return paths, f'path list contains {len(paths)} path(s)'


def git(root: Path, *args: str) -> str:
    try:
        result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise Doubt(f'git {" ".join(args)} failed: {error}') from error
    if result.returncode:
        raise Doubt(f'git {" ".join(args)} failed: '
                    f'{result.stderr.strip() or f"exit {result.returncode}"}')
    return result.stdout


def compared(env: dict[str, str], root: Path) -> tuple[str, str, str]:
    """The two commits whose trees the change is, and what they are."""
    event = env.get('GITHUB_EVENT_NAME', '')
    if event not in ('pull_request', 'push'):
        raise Doubt(f'{event or "an unnamed event"} runs every job')
    commits = git(root, 'rev-list', '--parents', '--max-count=1', 'HEAD').split()
    if event == 'pull_request':
        pr_head = env.get('PR_HEAD_SHA', '')
        if len(commits) != 3 or not SHA.fullmatch(pr_head) or commits[2] != pr_head:
            raise Doubt(f'HEAD is not a merge of the pull request head {pr_head or "(none)"}: '
                        f'HEAD and its parents are {" ".join(commits) or "(none)"}')
        return commits[1], commits[0], 'the tested merge against its base'
    before, forced = env.get('PUSH_BEFORE', ''), env.get('PUSH_FORCED', '')
    if forced != 'false':
        raise Doubt('the push was forced' if forced == 'true' else f'push forced={forced!r}')
    if before == NO_COMMIT:
        raise Doubt('the push created the branch')
    if not SHA.fullmatch(before):
        raise Doubt(f'push before={before!r} names no commit')
    if len(commits) != 2 or commits[1] != before:
        raise Doubt(f'the push carried more than its one commit: before {before} is not the parent '
                    f'of HEAD, whose parents are {" ".join(commits[1:]) or "(none)"}')
    return before, commits[0], 'the pushed commit against the previous tip'


def changed_paths(root: Path, base: str, head: str) -> list[str]:
    """Every path the change touches: renames split, so both sides count."""
    listed = git(root, 'diff', '--no-renames', '--no-ext-diff', '--name-only', '-z', base, head, '--')
    return [path for path in listed.split('\0') if path]


def classify(env: dict[str, str], root: Path = ROOT) -> tuple[bool, bool, bool, list[str]]:
    """(docs_only, github_only, arm_relevant, reasons printed for them)."""
    try:
        base, head, what = compared(env, root)
        paths = changed_paths(root, base, head)
    except Doubt as doubt:
        return False, False, True, [f'not docs-only: {doubt}']
    lines = [f'{what}: {base}..{head}, {len(paths)} changed path(s)']
    docs_only, github_only, arm_relevant = classify_paths(
        paths, pull_request=env.get('GITHUB_EVENT_NAME') == 'pull_request')
    if not paths:
        return False, False, True, lines + ['not docs-only: the change is empty']
    other = [path for path in paths if not is_documentation(path)]
    if other:
        lines.append(f'not docs-only: {len(other)} changed path(s) are not documentation')
        lines += [f'  {path}' + (f' ({PROOF_READS[path]})' if path in PROOF_READS else '')
                  for path in other[:SHOWN]]
        lines += ([f'  ... and {len(other) - SHOWN} more'] if len(other) > SHOWN else [])
    else:
        lines.append('docs-only: every changed path is documentation')
        lines += [f'  {path}' for path in paths[:SHOWN]]
        lines += ([f'  ... and {len(paths) - SHOWN} more']
                  if len(paths) > SHOWN else [])
    if github_only:
        lines.append('github-only: every changed path is under .github/')
    return docs_only, github_only, arm_relevant, lines


def decide(env: dict[str, str], root: Path = ROOT) -> tuple[bool, list[str]]:
    """(docs_only, the reasons printed for it), preserving the old API."""
    docs_only, _, _, lines = classify(env, root)
    return docs_only, lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT,
                        help='the checkout to judge (default: the one holding this script)')
    parser.add_argument('--paths', type=Path,
                        help='read a complete NUL-separated compare-file list')
    parser.add_argument('--github-output', type=Path,
                        help='append docs_only, github_only and arm_relevant flags to this file')
    args = parser.parse_args(argv)
    if args.paths is not None:
        paths, reason = read_paths_file(args.paths)
        if paths is None:
            docs_only, github_only, arm_relevant = False, False, True
            lines = [f'not docs-only/github-only: {reason}']
        else:
            docs_only, github_only, arm_relevant = classify_paths(paths)
            lines = [reason]
            lines.append('docs-only: every changed path is documentation'
                         if docs_only else 'not docs-only: a changed path is not documentation')
            lines.append('github-only: every changed path is under .github/'
                         if github_only else 'not github-only: a changed path is outside .github/')
    else:
        docs_only, github_only, arm_relevant, lines = classify(dict(os.environ), args.root)
    for line in lines:
        print(line)
    if args.github_output is not None:
        with args.github_output.open('a', encoding='utf-8') as output:
            output.write(f'docs_only={"true" if docs_only else "false"}\n')
            output.write(f'github_only={"true" if github_only else "false"}\n')
            output.write(f'arm_relevant={"true" if arm_relevant else "false"}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
