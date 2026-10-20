#!/usr/bin/env python3
"""Decide whether a PR must run SLOW verifier self-test classes.

Only an established, nonempty PR diff entirely in DEFERRED_DIRS or root prose
may defer SLOW until the next push to main. All other events and every doubt
run ALL classes. Use the base's copy, alongside its ci_docs_only.py, with
--root pointing at the tested checkout. Both sides of a rename count.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import sys

from ci_docs_only import Doubt, changed_paths, compared

ROOT = Path(__file__).resolve().parents[1]
DEFERRED_DIRS = ('tools/', 'tutorial/', 'benchmarks/', '.claude/')
# Audit inventory, deliberately wider than direct ROOT reads: source guards
# recursively read src/include; provider receipts read tests/fixtures; helpers
# read docker catalogs, examples, docs, CMake inputs and release tags.
# Unknown paths ALSO trigger SLOW. This tuple documents the known readers;
# it is not an allowlist that could silently omit a new input.
VERIFIER_INPUT_DIRS = (
    '.github/', 'scripts/', 'tests/', 'include/', 'src/', 'runner/',
    'examples/', 'docs/', 'docker/', 'cmake/',
)
VERIFIER_INPUT_FILES = frozenset({
    'VERSION', 'CONTRIBUTING.md', 'CMakeLists.txt', '.gitmodules', 'corpus',
})


def may_defer(path: str) -> bool:
    """A narrow deferring set; malformed and unknown paths always run SLOW."""
    parts = PurePosixPath(path).parts
    if (not path or path.startswith('/') or '\\' in path
            or any(part in ('', '.', '..') for part in path.split('/'))
            or path in VERIFIER_INPUT_FILES or path.startswith(VERIFIER_INPUT_DIRS)):
        return False
    if path.startswith(DEFERRED_DIRS):
        return True
    return len(parts) == 1 and (path.endswith('.md') or path in ('LICENSE', 'NOTICE'))


def needs_slow(paths: list[str] | None) -> bool:
    """An empty or unavailable diff cannot authorize deferral."""
    return not paths or any(not may_defer(path) for path in paths)


def decide(env: dict[str, str], root: Path = ROOT) -> tuple[bool, list[str]]:
    if env.get('GITHUB_EVENT_NAME') != 'pull_request':
        return True, ['verifier SLOW: non-PR events and local verification run all classes']
    try:
        base, head, what = compared(env, root)
        paths = changed_paths(root, base, head)
    except (Doubt, OSError, UnicodeError) as error:
        return True, [f'verifier SLOW: unreadable or unknown diff: {error}']
    slow = needs_slow(paths)
    return slow, [f'{what}: {base}..{head}, {len(paths)} changed path(s)',
                  'verifier SLOW: run' if slow else
                  'verifier SLOW: deferred until push to main; FAST still runs']


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--github-output', type=Path)
    args = parser.parse_args(argv)
    slow, lines = decide(dict(os.environ), args.root)
    for line in lines:
        print(line)
    if args.github_output is not None:
        with args.github_output.open('a', encoding='utf-8') as output:
            output.write(f'verifier_slow={"true" if slow else "false"}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
