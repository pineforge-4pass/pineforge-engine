#!/usr/bin/env python3
"""Check added tracked content, new paths and commit messages for public wording.

The content comparison starts at the merge base; messages cover base..head.
GitHub change events supply the base; other runs share the documentation resolver.
Tree mode checks committed files and permits only byte-identical fixed fixtures.
E4 runs only with --digests supplied by the private path.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterator

from check_doc_reverts import GitError, default_base
from check_public_role_words import ROLE_WORD_RE

ROOT = Path(__file__).resolve().parent.parent
E1_RE = re.compile(r'\b[A-Z][A-Z0-9]*-(?:CX|TOP|NEXT)\b')
E2_RE = re.compile(
    r'\b(?:(?:CR|HO|LB|SMP|CG|TX|LIVE)-[0-9]+|CG'
    r'INT[0-9]+|S-[0-9]{3,}|K-[0-9]+)\b'
)
E3_RE = re.compile(
    r'\b(?:rj-20[0-9]{6}[a-z0-9]*(?:-[a-z0-9]+)*|tick-[0-9]{4})\b'
)
SUBTOKEN_RE = re.compile(r'[^0-9A-Za-z]+')
OID_RE = re.compile(r'(?:[0-9a-f]{40}|[0-9a-f]{64})')
HUNK_RE = re.compile(
    rb'@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@.*'
)
FROZEN_BLOBS = {
    'tests/fixtures/pf_sink_frozen_6b33c47f_rjone.py':
        '5ef5b865650031765d953f01c5c36200793994f067252832b8f89e58cf6e6fcb',
    'tests/fixtures/run_json_frozen_6b33c47f_rjone.py':
        '639435843b16fe9b6804b2165e9e16c585518b0c6ee66cba8b90147033394360',
    'tests/fixtures/run_json_frozen_pre_l9a.py':
        '743a1cb79e7d2aa24389ac58dcb2a0fc157e1fd4739addaf8cfbe2604b98dd65',
    'tests/oracle/test_oracle_coof.cpp':
        'e006f06456b5020f51034a24b3dd99732df53028c8105a32220ad48790f0a903',
    'tests/oracle/test_oracle_fx.cpp':
        '007e8bc47f4a442612d000b49e11474cccb2ac49b8b395f997333b23131f4ef5',
    'tests/ownership/probes/report/buy_hold_return_overflow/NOTES.md':
        'cd3f14bdab8ccc2e1cb1ed35688bfe1f4c4bd8d7fe81e136999fc36089a50bef',
    'tests/test_adapter_quiet_bar_pinned.inc':
        'c1c3673fe0b1affe9f5b6ffbbfffaac3720468e4a0cb22062dba50939e02ef9f',
    'tests/test_publication_witness_pinned.inc':
        '66d265cda7522e3e0b94af6296b1facade99740b0923d3b1b27ab2faa4b1d69d',
}


class Failure(Exception):
    """A named failure with no input text in its diagnostic."""


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise Failure('arguments')


def git(root: Path, *args: str) -> bytes:
    try:
        result = subprocess.run(
            ['git', '--no-replace-objects', '-C', str(root), *args],
            capture_output=True,
        )
    except OSError as error:
        raise Failure('git-' + args[0]) from error
    if result.returncode:
        raise Failure('git-' + args[0])
    return result.stdout


def resolve(root: Path, ref: str, kind: str) -> str:
    try:
        value = git(root, 'rev-parse', '--verify', '--end-of-options',
                    ref + '^{commit}').decode('ascii').strip()
    except (Failure, UnicodeError) as error:
        raise Failure(kind + '-resolution') from error
    if not OID_RE.fullmatch(value):
        raise Failure(kind + '-resolution')
    return value


def event_base() -> str | None:
    name = os.environ.get('GITHUB_EVENT_NAME')
    path = os.environ.get('GITHUB_EVENT_PATH')
    if name is None or path is None:
        return None
    try:
        data = Path(path).read_bytes()
    except (OSError, ValueError) as error:
        raise Failure('event-read') from error
    try:
        event = json.loads(data.decode('utf-8'))
    except (UnicodeError, ValueError) as error:
        raise Failure('event-format') from error
    if not isinstance(event, dict):
        raise Failure('event-format')
    try:
        if name in ('pull_request', 'pull_request_target'):
            base = event['pull_request']['base']['sha']
        elif name == 'push':
            base = event['before']
        else:
            return None
    except (KeyError, TypeError) as error:
        raise Failure('event-base') from error
    if not isinstance(base, str) or not re.fullmatch(r'[0-9a-f]{40}', base):
        raise Failure('event-base')
    if name == 'push' and base == '0' * 40:
        raise Failure('event-before-zero')
    return base


def load_digests(path: Path) -> set[str]:
    try:
        rows = path.read_bytes().decode('ascii').splitlines()
    except OSError as error:
        raise Failure('digest-read') from error
    except UnicodeError as error:
        raise Failure('digest-format') from error
    while rows and rows[0].startswith('#'):
        rows.pop(0)
    if (not rows or any(not re.fullmatch(r'[0-9a-f]{64}', row) for row in rows)
            or rows != sorted(set(rows))):
        raise Failure('digest-format')
    return set(rows)


def decode(data: bytes, kind: str = 'blob') -> str:
    try:
        return data.decode('latin-1' if b'\0' in data else 'utf-8')
    except UnicodeError as error:
        raise Failure(kind + '-decode') from error


def path_text(data: bytes) -> str:
    try:
        value = data.decode('utf-8')
    except UnicodeError as error:
        raise Failure('path-decode') from error
    if not value:
        raise Failure('path-format')
    return value


def display(path: str) -> str:
    return (path.replace('\\', '\\\\').replace('\n', '\\n')
            .replace('\r', '\\r').replace('\t', '\\t'))


def text_lines(text: str) -> list[str]:
    rows = text.split('\n')
    if rows[-1] == '':
        rows.pop()
    return rows


def classes(text: str, digests: set[str], *, path: bool = False) -> Iterator[str]:
    for kind, pattern in (('E1', E1_RE), ('E2', E2_RE), ('E3', E3_RE)):
        for _ in pattern.finditer(text):
            yield kind
    for match in ROLE_WORD_RE.finditer(text):
        if text[max(0, match.start() - 5):match.end()] != 'Asia/Taipei':
            yield 'E5'
    words = (text,) if path else text.split()
    for word in words:
        if digests:
            atoms = [atom.lower() for atom in SUBTOKEN_RE.split(word) if atom]
            for start in range(len(atoms)):
                for size in range(1, min(3, len(atoms) - start) + 1):
                    term = '-'.join(atoms[start:start + size])
                    if hashlib.sha256(term.encode('utf-8')).hexdigest() in digests:
                        yield 'E4'
        for segment in word.split('/'):
            if segment == '.' + 'lane':
                yield 'E6'


def scan(text: str, location: str, digests: set[str], *, path: bool = False) -> int:
    hits = 0
    for kind in classes(text, digests, path=path):
        print(f'{location}: {kind}')
        hits += 1
    return hits


def nul_fields(data: bytes) -> list[bytes]:
    if not data:
        return []
    if not data.endswith(b'\0'):
        raise Failure('listing-format')
    return data[:-1].split(b'\0')


@dataclass(frozen=True)
class Change:
    old_mode: str
    mode: str
    old_oid: str
    oid: str
    status: str
    path: str


def changes(root: Path, base: str, head: str) -> Iterator[Change]:
    fields = nul_fields(git(
        root, 'diff', '--raw', '-z', '--no-abbrev', '--no-ext-diff',
        '--no-textconv', '--no-relative', '--find-renames', '--rename-empty', '-l0',
        '--ignore-submodules=none', base, head, '--',
    ))
    index = 0
    while index < len(fields):
        try:
            header = fields[index].decode('ascii')
        except UnicodeError as error:
            raise Failure('listing-format') from error
        parts = header[1:].split() if header.startswith(':') else []
        if len(parts) != 5 or index + 1 >= len(fields):
            raise Failure('listing-format')
        old_mode, mode, old_oid, oid, status = parts
        if (not OID_RE.fullmatch(old_oid) or not OID_RE.fullmatch(oid)
                or not re.fullmatch(r'(?:[AMDT]|[RC][0-9]{1,3})', status)):
            raise Failure('listing-format')
        path = path_text(fields[index + 1])
        index += 2
        if status.startswith(('R', 'C')):
            if index >= len(fields):
                raise Failure('listing-format')
            path = path_text(fields[index])
            index += 1
        yield Change(old_mode, mode, old_oid, oid, status, path)


def entry_bytes(root: Path, mode: str, oid: str) -> bytes:
    if mode == '000000':
        return b''
    if mode == '160000':
        # A gitlink stores only the referenced commit identifier in this tree.
        return ('Subproject commit ' + oid + '\n').encode('ascii')
    if mode not in ('100644', '100755', '120000'):
        raise Failure('entry-type')
    return git(root, 'cat-file', 'blob', oid)


def added_lines(root: Path, change: Change, data: bytes) -> Iterator[tuple[int, str]]:
    rows = text_lines(decode(data))
    if (change.old_mode == '000000' or change.old_mode[:3] != change.mode[:3]
            or change.mode == '160000'):
        yield from enumerate(rows, 1)
        return
    if change.old_oid == change.oid:
        return
    old_data = entry_bytes(root, change.old_mode, change.old_oid)
    decode(old_data)
    patch = git(
        root, 'diff', '--no-ext-diff', '--no-textconv', '--text', '--no-color',
        '--no-renames', '--unified=0', '--output-indicator-new=+',
        '--output-indicator-old=-', '--output-indicator-context= ',
        change.old_oid, change.oid, '--',
    )
    if not patch.endswith(b'\n'):
        raise Failure('diff-format')
    in_hunk = False
    old_left = new_left = number = 0
    for line in patch.split(b'\n')[:-1]:
        if line.startswith(b'@@ '):
            hunk = HUNK_RE.fullmatch(line)
            if not hunk or old_left or new_left:
                raise Failure('diff-format')
            in_hunk = True
            old_left = int(hunk[2]) if hunk[2] is not None else 1
            number = int(hunk[3])
            new_left = int(hunk[4]) if hunk[4] is not None else 1
        elif in_hunk:
            if line == b'\\ No newline at end of file':
                continue
            if line.startswith(b'+'):
                if not new_left or not 1 <= number <= len(rows):
                    raise Failure('diff-format')
                encoding = 'latin-1' if b'\0' in data else 'utf-8'
                if line[1:].decode(encoding) != rows[number - 1]:
                    raise Failure('diff-coverage')
                yield number, rows[number - 1]
                number += 1
                new_left -= 1
            elif line.startswith(b'-') and old_left:
                old_left -= 1
            elif line.startswith(b' ') and old_left and new_left:
                old_left -= 1
                new_left -= 1
                number += 1
            else:
                raise Failure('diff-format')
    if not in_hunk or old_left or new_left:
        raise Failure('diff-coverage')


@dataclass
class Coverage:
    files: int = 0
    added_lines: int = 0
    paths: int = 0
    commits: int = 0
    exempt: int = 0


def scan_diff(root: Path, base: str, head: str, digests: set[str],
              coverage: Coverage) -> tuple[int, str]:
    if base == head:
        return 0, head
    commits = git(root, 'log', '--format=%H', f'{base}..{head}', '--').decode('ascii').split()
    if not commits:
        raise Failure('empty-range')
    if any(not OID_RE.fullmatch(commit) for commit in commits):
        raise Failure('commit-list')
    bases = git(root, 'merge-base', '--all', base, head).decode('ascii').split()
    if len(bases) != 1 or not OID_RE.fullmatch(bases[0]):
        raise Failure('merge-base')
    merge_base = bases[0]
    hits = 0
    for change in changes(root, merge_base, head):
        coverage.files += 1
        if change.status == 'D':
            continue
        location = display(change.path)
        if change.status.startswith(('A', 'R', 'C')):
            coverage.paths += 1
            hits += scan(change.path, location, digests, path=True)
        data = entry_bytes(root, change.mode, change.oid)
        for number, line in added_lines(root, change, data):
            coverage.added_lines += 1
            hits += scan(line, f'{location}:{number}', digests)
    for commit in commits:
        message = decode(git(root, 'log', '-1', '--encoding=none',
                             '--format=%B', commit, '--'), 'commit')
        hits += scan(message, commit[:8], digests)
        coverage.commits += 1
    return hits, merge_base


def scan_tree(root: Path, head: str, digests: set[str], coverage: Coverage) -> int:
    fields = nul_fields(git(root, 'ls-tree', '-r', '-z', '--full-tree', head))
    if not fields:
        raise Failure('empty-tree')
    hits = 0
    for field in fields:
        metadata, separator, raw_path = field.partition(b'\t')
        parts = metadata.decode('ascii').split()
        if not separator or len(parts) != 3:
            raise Failure('listing-format')
        mode, kind, oid = parts
        if (not OID_RE.fullmatch(oid)
                or kind != ('commit' if mode == '160000' else 'blob')):
            raise Failure('listing-format')
        path = path_text(raw_path)
        data = entry_bytes(root, mode, oid)
        coverage.files += 1
        if mode != '160000' and FROZEN_BLOBS.get(path) == hashlib.sha256(data).hexdigest():
            coverage.exempt += 1
            continue
        location = display(path)
        coverage.paths += 1
        hits += scan(path, location, digests, path=True)
        for number, line in enumerate(text_lines(decode(data)), 1):
            coverage.added_lines += 1
            hits += scan(line, f'{location}:{number}', digests)
    return hits


def main(argv: list[str] | None = None) -> int:
    parser = Parser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--base')
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--digests', type=Path,
                        help='private list supplied by the private path')
    parser.add_argument('--tree', action='store_true')
    try:
        args = parser.parse_args(argv)
        root = args.root.resolve()
        digests = load_digests(args.digests) if args.digests is not None else set()
        shallow = git(root, 'rev-parse', '--is-shallow-repository').strip()
        if shallow != b'false':
            raise Failure('shallow-history')
        head = resolve(root, args.head, 'head')
        coverage = Coverage()
        if args.tree:
            base = merge_base = head
            hits = scan_tree(root, head, digests, coverage)
        else:
            if args.base is not None:
                base = resolve(root, args.base, 'base')
            else:
                found = event_base()
                if found is None:
                    try:
                        found = default_base(root)
                    except (GitError, OSError, UnicodeError) as error:
                        raise Failure('base-resolution') from error
                if not found:
                    raise Failure('base-resolution')
                base = resolve(root, found, 'base')
            hits, merge_base = scan_diff(root, base, head, digests, coverage)
        e4 = 'enabled' if args.digests is not None else 'not-run (no private list)'
        print(f'public text added: coverage base={base} head={head} merge_base={merge_base} '
              f'files={coverage.files} added_lines={coverage.added_lines} '
              f'paths={coverage.paths} commits={coverage.commits} exempt={coverage.exempt} '
              f'e4={e4}')
        if not args.tree and base == head:
            print('public text added: no added range (base equals head)')
            return 0
        if hits:
            print(f'public text added: FAIL findings: {hits}')
            return 1
        print('public text added: OK')
        return 0
    except Failure as error:
        print(f'public text added: FAIL {error}: 1', file=sys.stderr)
        return 1
    except (OSError, UnicodeError, ValueError) as error:
        print(f'public text added: FAIL {type(error).__name__}: 1', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
