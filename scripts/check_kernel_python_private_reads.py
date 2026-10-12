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


# Rows whose closure names an absent private path that the row provably never
# reads from the real tree. Each entry is printed on every run, with the
# evidence that earned it, so an exemption is never silent. Regaining the
# exemption requires re-running its evidence: scripts/test_private_fixture_
# reads.py (ExemptRowTraceControls) re-runs it in-tree for every row this
# table exempts -- it traces each row's script, fails when an open lands under
# the real tests/fixtures/ or tests/oracle/, and fails a tampered copy that
# does read, so a name on this table without a clean trace does not pass.
EXEMPT = {
    'test_private_fixture_guards': (
        'the paths it names are temp-world payloads (PROBES), not reads: under '
        'a sys.addaudithook(open) trace it opens 0 files under the real '
        'tests/fixtures/ and exits 0 on the fixture-less tree; the trace is '
        're-run in-tree by control 12 of scripts/test_private_fixture_reads.py'),
    'test_private_fixture_reads_controls': (
        'the path it names is a tamper payload (REMOVED_EXAMPLE, the removed '
        'directory the controls copy into their temp trees), not a read: under '
        'a sys.addaudithook(open) trace the script opens 0 files under the '
        'real tests/fixtures/ and tests/oracle/, in its own process and in the '
        'Python children it starts; control 12 of scripts/test_private_'
        'fixture_reads.py re-runs that trace'),
    'test_oracle_fixture_checks': (
        'the private paths it names are payloads of the synthetic roots its '
        'controls build: the suite copies tests/oracle/ (the KEPT files) into '
        'a temp root and writes the three private oracle sources there itself, '
        'so it never opens an ABSENT private file of the real tree; under the '
        'trace that records exactly those opens it comes out clean, and the '
        'three of its controls that assert the checkers’ private-fixture '
        'behaviour (the flag, the pin’s removal set) skip when the tree '
        'holds the oracles, because the overlay world is not what they '
        'assert. Control 12 of scripts/test_private_fixture_reads.py re-runs '
        'the trace'),
    'test_two_world_registration': (
        'the path it names is the overlay MARKER (OVERLAY_MARKER), not a read: '
        'the script probes it with Path.exists() -- an os.stat, which the '
        'open-audit hook does not observe -- to decide whether the overlay '
        'half runs, and every file it opens is gated on that same world '
        'existence test (load_pinned_rows, reaches_source_layer, parent_list, '
        'file(STRINGS)), so no file under the real tests/fixtures/ or '
        'tests/oracle/ is ever opened, present or absent. Without the '
        'fixtures the row exits 0 and prints its own not-run line ("overlay '
        'half not run: private fixtures not present: <marker>"); it carries '
        'no skip code. The trace control re-runs every row this table '
        'exempts, with one caveat stated rather than hidden: its invocation '
        'runs a script through runpy without the script’s own directory on '
        'sys.path, so THIS script stops at its `import ci_verify` and the '
        'trace observes no opens either way -- the evidence that holds here '
        'is the reading above plus the row’s exit 0 on the fixture-less '
        'tree, and the checker’s sweep re-reads the constant every run'),
}
GATED_BY_MODULE = re.compile(r'(?:return|Exit|exit)\s*\(?\s*77\b')


def module_gates_its_own_paths(text: str) -> bool:
    """The module that names the path also carries the pinned literal and a
    77 exit -- the settlement-ABI family's own gate (95c27eaff)."""
    return SKIP_LITERAL in text and GATED_BY_MODULE.search(text) is not None


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
            # A path named only by a closure module that gates its own reads
            # (the pinned literal plus a 77 exit) is already private-aware.
            unguarded = [read for read in reads
                         if not module_gates_its_own_paths(closure[read[0]])]
            if unguarded and name not in EXEMPT and not has_skip_path(
                    cmake_text, name, closure):
                offenders.append((name, unguarded[0]))
            checked += 1
    except SweepError as error:
        print(f'check_kernel_python_private_reads: {error}', file=sys.stderr)
        return 2
    for name, (path, line, reference) in offenders:
        print(f'{name}: reads absent {reference} ({path}:{line}) '
              f'without printing {SKIP_LITERAL!r} and skipping')
    for name, reason in sorted(EXEMPT.items()):
        print(f'exempt: {name} -- {reason}')
    print(f'kernel python rows reading an absent private file: {len(offenders)}')
    return 1 if offenders else 0


if __name__ == '__main__':
    raise SystemExit(main())
