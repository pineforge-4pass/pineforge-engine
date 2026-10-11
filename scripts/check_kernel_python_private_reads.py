#!/usr/bin/env python3
"""Class sweep: a Python ctest row may not read a private file that is gone.

L6's S-73 two-mode round 4 (kernel profile, fixtures absent) found Python rows
that read files the private-fixture removals take out of the public tree and
failed on read_text()/open(): test_native_cpp_abi (its headers.tar),
test_native_cpp_versions, test_settlement_cpp_abi_tooling,
test_verify_corpus_metrics and the two pending_order_v1 prefix rows. Each row
now either reads no private path or has the skip path the fix round gave it:
the pinned literal ("SKIP private fixtures not present: <path>") with the
row's SKIP_RETURN_CODE 77 / SKIP_REGULAR_EXPRESSION, or unittest.SkipTest
(skipUnless) around the private read.

This sweep reads the tree, names every offender with file:line, and closes
with

    kernel python rows reading an absent private file: 0

LOUD-FAIL (shared with the other private-fixture sweeps): a construct this
sweep cannot parse is an ERROR, never a silent skip --

* an add_test() whose command runs ${Python3_EXECUTABLE} but names no .py
  script, or names one the tree lacks,
* a local module a row's script imports that the sweep cannot read,
* a row whose script text cannot be decoded.

The population is every Python ctest row registered by tests/CMakeLists.txt
(a superset of any one profile's rows, so the kernel profile cannot be missed
by a row that registers only elsewhere). A "private path read" is a path under
tests/fixtures/ or tests/oracle/ named by the script's own text -- as one
literal ("tests/fixtures/...") or as the composition the tree writes them with
("tests" / "fixtures" / "...") -- that the tree does not have. A path the tree
HAS is not a private read: the sweep never flags a read it cannot prove is
missing.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKIP_LITERAL = 'SKIP private fixtures not present: '
PRIVATE_PREFIXES = ('tests/fixtures/', 'tests/oracle/')
CMAKE_HEAD = re.compile(r'^\s*([A-Za-z_]\w*)\s*\(')
ADD_TEST = re.compile(r'add_test\(\s*NAME\s+([A-Za-z0-9_.-]+)', re.MULTILINE)
LITERAL_PRIVATE = re.compile(r'["\']((?:[\w.-]+/)?tests/(?:fixtures|oracle)/[\w./-]+)["\']')
COMPOSED_PRIVATE = re.compile(
    r'["\']tests["\']\s*/\s*["\'](fixtures|oracle)["\']\s*/\s*["\']([\w./-]+)["\']')
LOCAL_IMPORT = re.compile(r'^\s*(?:from\s+([A-Za-z_]\w*)\s+import|import\s+([A-Za-z_]\w*))',
                          re.MULTILINE)


class SweepError(Exception):
    """A construct the sweep cannot parse; reported and exits non-zero."""


def statements(text: str):
    """(line, statement) pairs, split at balanced parentheses."""
    out, buffer, depth, line = [], [], 0, 0
    for number, raw in enumerate(text.splitlines(), 1):
        if not buffer and (not raw.strip() or raw.strip().startswith('#')):
            continue
        if not buffer:
            line = number
        buffer.append(raw)
        depth += raw.count('(') - raw.count(')')
        if depth <= 0:
            out.append((line, '\n'.join(buffer)))
            buffer, depth = [], 0
    if buffer:
        out.append((line, '\n'.join(buffer)))
    return out


def python_rows(cmake_path: Path):
    """(row name, script path, row line) for every Python ctest row."""
    text = cmake_path.read_text()
    rows = []
    for line, statement in statements(text):
        head = CMAKE_HEAD.match(statement)
        if not head or head.group(1) != 'add_test':
            continue
        name = ADD_TEST.search(statement)
        if not name:
            # A function template (pineforge_add_twin_test) registers with a
            # variable NAME. It cannot hide a Python row unless its command
            # runs Python; give up loudly in that case only.
            if 'Python3_EXECUTABLE' in statement:
                raise SweepError(
                    f'{cmake_path}:{line}: add_test() with an unreadable NAME runs '
                    'Python3_EXECUTABLE; the sweep refuses to skip it: '
                    + repr(statement))
            continue
        if 'Python3_EXECUTABLE' not in statement:
            continue
        scripts = [token.strip('"') for token in
                   re.findall(r"[\w${}/.-]+\.py\b", statement)]
        if not scripts:
            # The one modelled non-file form is unittest discovery, whose
            # population is the discovered test files, not one script.
            discovered = re.search(r"-m\s+unittest\s+discover\s+-s\s+([\w./-]+)", statement)
            if discovered:
                directory = ROOT / discovered.group(1)
                if not directory.is_dir():
                    raise SweepError(
                        f'{cmake_path}:{line}: row {name.group(1)} discovers '
                        f'{discovered.group(1)!r}, which is not a directory')
                scripts = sorted(str(path) for path in directory.glob('test_*.py'))
            if not scripts:
                raise SweepError(
                    f'{cmake_path}:{line}: row {name.group(1)} runs Python3_EXECUTABLE '
                    f'but names no .py script; the sweep cannot read it and refuses to skip it')
        script = scripts[0]
        script = (script.replace('${PROJECT_SOURCE_DIR}', str(ROOT))
                        .replace('${CMAKE_CURRENT_SOURCE_DIR}', str(ROOT / 'tests')))
        if '${' in script:
            raise SweepError(
                f'{cmake_path}:{line}: row {name.group(1)} names an unexpanded script '
                f'{script!r}; the sweep refuses to skip it')
        resolved = Path(script)
        rows.append((name.group(1),
                     resolved if resolved.is_absolute() else ROOT / resolved, line))
    return rows


def module_closure(script: Path, seen=None):
    """The script plus every local module it imports, transitively (read-only)."""
    seen = seen if seen is not None else {}
    if script in seen:
        return seen
    try:
        text = script.read_text()
    except OSError as error:
        raise SweepError(f'{script}: unreadable: {error}') from error
    seen[script] = text
    for match in LOCAL_IMPORT.finditer(text):
        name = match.group(1) or match.group(2)
        for directory in (script.parent, ROOT / 'scripts'):
            candidate = directory / f'{name}.py'
            if candidate.is_file() and candidate not in seen:
                module_closure(candidate, seen)
    return seen


def private_reads(closure):
    """Absent tests/fixtures|oracle paths named by the closure's text."""
    missing = []
    for path, text in closure.items():
        for match in LITERAL_PRIVATE.finditer(text):
            candidate = ROOT / match.group(1)
            if not candidate.exists():
                line = text.count('\n', 0, match.start()) + 1
                missing.append((path, line, match.group(1)))
        for match in COMPOSED_PRIVATE.finditer(text):
            reference = f'tests/{match.group(1)}/{match.group(2)}'
            if not (ROOT / reference).exists():
                line = text.count('\n', 0, match.start()) + 1
                missing.append((path, line, reference))
    return missing


def row_properties(cmake_text: str, row: str) -> str:
    """Every set_tests_properties call whose row list names `row`."""
    found = []
    for _, statement in statements(cmake_text):
        head = CMAKE_HEAD.match(statement)
        if head and head.group(1) == 'set_tests_properties' and re.search(
                r'(?<![\w-])' + re.escape(row) + r'(?![\w-])', statement):
            found.append(statement)
    return '\n'.join(found)


def has_skip_path(cmake_text: str, row: str, closure) -> bool:
    """The row's own skip path: the pinned literal plus a ctest skip property,
    or a unittest SkipTest/skipUnless around the private read."""
    texts = [text for path, text in closure.items() if path.suffix == '.py']
    if not any(SKIP_LITERAL in text for text in texts):
        return False
    properties = row_properties(cmake_text, row)
    if re.search(r'SKIP_RETURN_CODE\s+77', properties):
        return True
    if 'SKIP_REGULAR_EXPRESSION' in properties and 'private fixtures not present' in properties:
        return True
    return any(('SkipTest' in text or 'skipUnless' in text) for text in texts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', type=Path, default=ROOT / 'tests' / 'CMakeLists.txt')
    args = parser.parse_args()
    cmake_path = args.cmake
    cmake_text = cmake_path.read_text()
    offenders = []
    try:
        rows = python_rows(cmake_path)
        checked = 0
        for name, script, line in rows:
            if not script.is_file():
                raise SweepError(
                    f'{cmake_path}:{line}: row {name} runs a missing script {script}')
            closure = module_closure(script)
            reads = private_reads(closure)
            if not reads:
                checked += 1
                continue
            if not has_skip_path(cmake_text, name, closure):
                offenders.append((name, reads[0]))
            checked += 1
    except SweepError as error:
        print(f'check_kernel_python_private_reads: {error}', file=sys.stderr)
        return 2
    for name, (path, line, reference) in offenders:
        print(f'{name}: reads absent {reference} ({path}:{line}) '
              f'without printing {SKIP_LITERAL!r} and skipping')
    print(f'kernel python rows reading an absent private file: {len(offenders)}')
    return 1 if offenders else 0


if __name__ == '__main__':
    raise SystemExit(main())
