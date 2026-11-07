#!/usr/bin/env python3
"""Keep internal working names out of public text.

The bare numbered design stages in docs/design/native-feature-parity.md
describe engine stages and are deliberately absent from the expression.
Only tracked files in PUBLIC_PATHS are read. Text must be valid UTF-8;
NUL-containing assets are also searched, using reversible byte decoding.
Exit 0 for a clean scope, 1 for findings or any listing or reading failure.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
# Retain the two previously cleaned files while covering all public roots.
PUBLIC_PATHS = (
    'docs/',
    'README.md',
    'CHANGELOG.md',
    'src/',
    'include/',
    'scripts/arena_bound.py',
    'tests/test_arena_bound.py',
)

# The original alternatives keep their boundaries. The added alternatives
# preserve substring matching, except for the two-digit integration names.
ROLE_WORD_RE = re.compile(
    r'\b(?:[A-Z]+-TOP|[A-Z0-9]+-CX|CR-[0-9]{3}|LB-[0-9]+|'
    r'rj-20[0-9]{6}[a-z0-9]*(?:-[a-z0-9]+)*|CX-\w+-\w+)\b|'
    r'R[0-9]+ lane|PERF-D[0-9]+|\bINT[0-9]{2}\b|TRIAGE[0-9]+|AUDIT[0-9]+|'
    r'supervisor call|owner call|lane brief|OWNER DECISION|Taipei'
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    root = parser.parse_args(argv).root.resolve()
    try:
        listed = subprocess.run(
            ['git', '-C', str(root), 'ls-files', '-z', '--', *PUBLIC_PATHS],
            capture_output=True,
        )
    except OSError as error:
        print(f'check_public_role_words: git ls-files failed: {error}', file=sys.stderr)
        return 1
    if listed.returncode:
        reason = listed.stderr.decode('utf-8', errors='replace').strip()
        print('check_public_role_words: git ls-files failed: '
              + (reason or f'exit {listed.returncode}'), file=sys.stderr)
        return 1
    paths = sorted({os.fsdecode(path) for path in listed.stdout.split(b'\0') if path})
    if not paths:
        print('check_public_role_words: no tracked files in the public scope',
              file=sys.stderr)
        return 1

    hits = 0
    scanned = 0
    read_failed = False
    for path in paths:
        try:
            data = (root / path).read_bytes()
        except OSError as error:
            print(f'{path}: cannot read tracked file: {error}', file=sys.stderr)
            read_failed = True
            continue
        try:
            body = data.decode('latin-1' if b'\0' in data else 'utf-8')
        except UnicodeDecodeError as error:
            print(f'{path}: cannot decode tracked text as UTF-8: {error}', file=sys.stderr)
            read_failed = True
            continue
        scanned += 1
        for line_number, line in enumerate(body.splitlines(), 1):
            for match in ROLE_WORD_RE.finditer(line):
                print(f'{path}:{line_number}: {match.group(0)}')
                hits += 1

    if hits:
        print('say it in plain public words; see docs/ci.md')
    if hits or read_failed:
        return 1
    print(f'check_public_role_words: no internal names found '
          f'({scanned} tracked files checked)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
