#!/usr/bin/env python3
"""Kernel-profile registration membership: a target the kernel profile builds
may link only the layers that profile builds.

Why this exists
---------------
The kernel profile configures with PINEFORGE_BUILD_SOURCE_LAYER=OFF, so
libpineforge.a holds the kernel alone. tests/CMakeLists.txt drops every
TEST_SOURCES row whose include closure reaches pineforge/source or
pineforge/compat/pine (the kernel-only filter) -- but the private-fixture
rows are removed from TEST_SOURCES in every profile and re-registered by
bespoke EXISTS-guarded blocks below, which the filter does not see. There
the same predicate is applied once, as the list `_pf_kernel_absent_rows`
(built by pineforge_test_is_source_bound, the one function the filter also
calls), and every guarded block reads that list: a source-layer-bound
private row registers NOTHING in a profile that does not build the source
layer -- no target, no row, no placeholder, no label -- and registers
through its private-fixture guard in every profile that does. With the
private fixtures overlaid, the guard restores a removed TU before configure
reads it, so the row that would link against the kernel archive alone is
exactly the row the predicate names.

What it checks
--------------
1. SINGLE SOURCE. `pineforge_test_is_source_bound` is defined once, the
   kernel-only filter calls it, and `set(_pf_kernel_absent_rows ...)` is
   built from it once.
2. EVERY GUARDED PRIVATE BLOCK reads the list: each if(EXISTS ...) block
   that registers a pinned private row carries `NOT <row> IN_LIST
   _pf_kernel_absent_rows` on BOTH the present branch and the placeholder
   branch. Closes `kernel targets linking without their prerequisites: 0`
   and `blocks not reading the predicate list: 0`.
3. PER-PROFILE COUNTS. For each profile the pinned private rows it
   registers are derived by the same predicate (source-bound rows: release
   only; the rest: release and kernel) and printed; the kernel count is the
   rows whose guard can register there, and zero of them are source-bound.
   The predicate reads the recorded `_pf_source_bound_private_rows` list
   first, so a private row whose TU the removals took is BOUND, not silent;
   a row whose TU is missing and is not recorded is counted in NEITHER
   profile, and named as such -- a missing TU is not "not source-bound".
   Every pinned row must be registered by one of the guarded blocks: a row
   the pin names that no block registers is a violation.
4. PREPARER/ROW AGREEMENT (d): every (row, provider-manifest) pair the
   guarded receipt blocks name is checked against the providers the
   preparer list gives that row, and both states are required in the block:
   absent -> the pinned SKIP placeholder with the literal; present -> the
   registration.

Loud-fail: any construct it cannot parse or classify (an if(EXISTS block
without a matching endif, an else() inside a guarded block it does not
model, a pinned row registered by a bare foreach, a missing predicate
definition, a receipt row whose guard names no binding) prints file:line
and exits non-zero. It never skips what it cannot read.

Exit 0 when every count closes, 1 on a violation, 2 on a construct it
cannot parse.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

REPO = Path(__file__).resolve().parent.parent

FAILURES: list[str] = []


def fail(source: Path, lineno: int, message: str, code: int = 1) -> None:
    FAILURES.append(f'{source}:{lineno}: {message}')


class Unparseable(Exception):
    pass


def strip_comments(text: str) -> str:
    """CMake #-comments off, honouring quotes (the T7 lesson)."""
    out = []
    for line in text.splitlines():
        quoted = False
        esc = False
        for i, ch in enumerate(line):
            if esc:
                esc = False
                continue
            if ch == '\\' and quoted:
                esc = True
                continue
            if ch == '"':
                quoted = not quoted
            if ch == '#' and not quoted:
                line = line[:i]
                break
        out.append(line)
    return '\n'.join(out)


def read_private_rows(text: str, source: Path) -> list[str]:
    try:
        start = text.index('set(_pf_private_fixture_rows')
        end = text.index('list(REMOVE_ITEM TEST_SOURCES ${_pf_private_fixture_rows})')
    except ValueError:
        raise Unparseable(f'{source}: the private-row pin list or its REMOVE_ITEM is gone')
    # The closing paren of a set()/list(APPEND) may sit on its last entry's
    # line -- `test_zero_trail_sibling_stop)` was, and the row it named left
    # the pinned set unseen, so its block was never checked (the kernel
    # profile then linked it against the kernel-only archive). Read the
    # optional paren as part of the entry.
    rows = re.findall(r'^\s*(test_\w+)\)?\s*$', text[start:end], re.M)
    if not rows:
        raise Unparseable(f'{source}: the private-row pin list parsed empty')
    return rows


def reaches_source_layer(entry: Path) -> bool:
    """The same closure pineforge_test_reaches_source_layer walks."""
    pending = [entry]
    seen = set()
    while pending:
        current = pending.pop(0)
        if current in seen:
            continue
        seen.add(current)
        if not current.exists():
            continue
        for line in current.read_text(errors='replace').splitlines():
            if not re.match(r'^[ \t]*#[ \t]*include', line):
                continue
            if re.search(r'pineforge/(source|compat/pine)/', line):
                return True
            match = re.search(r'"([^"]+)"', line)
            if match:
                pending.append(current.parent / match.group(1))
    return False


def read_recorded_source_bound(text: str, source: Path) -> set[str]:
    """The recorded source-bound private rows the predicate reads first.

    The predicate answers BOUND for a row in `_pf_source_bound_private_rows`
    before it walks anything, so this checker must read the SAME list: a
    private row whose public TU the removals took cannot be walked at all, and
    the walk's silence is not the predicate's answer."""
    start = text.find('set(_pf_source_bound_private_rows')
    if start < 0:
        raise Unparseable(f'{source}: _pf_source_bound_private_rows is gone')
    end = text.find('\n)', start)
    if end < 0:
        raise Unparseable(f'{source}: _pf_source_bound_private_rows never closes')
    recorded = set(re.findall(r'^\s*(test_\w+)\s*$', text[start:end], re.M))
    if not recorded:
        raise Unparseable(f'{source}: _pf_source_bound_private_rows parsed empty')
    return recorded


def source_bound(entry: Path, row: str, recorded: set[str]) -> str:
    """BOUND / CLEAR / MISSING for one private row, the predicate's own order.

    MISSING is not CLEAR: a translation unit the tree does not have says
    nothing about the row, and counting it as kernel-registered would report a
    profile that never configured it."""
    if row in recorded:
        return 'bound'
    if not entry.is_file():
        return 'missing'
    return 'bound' if reaches_source_layer(entry) else 'clear'


def blocks(lines: list[str], source: Path):
    """Every top-level if(EXISTS ...) block: (lineno, guard line, else lineno, end)."""
    blocks_found = []
    i = 0
    while i < len(lines):
        if (re.match(r'^if\(.*EXISTS "', lines[i])
                and not lines[i].startswith(' ')):
            depth = 1
            j = i + 1
            else_at = None
            while j < len(lines) and depth > 0:
                stripped = lines[j].strip()
                if stripped.startswith('if('):
                    depth += 1
                elif stripped.startswith('endif('):
                    depth -= 1
                    if depth == 0:
                        break
                elif depth == 1 and (stripped == 'else()'
                                     or stripped.startswith('elseif(')):
                    if else_at is not None:
                        raise Unparseable(
                            f'{source}:{j + 1}: a second else() inside the block that '
                            f'opens at :{i + 1}; the walker refuses rather than guessing')
                    else_at = j
                j += 1
            if depth != 0:
                raise Unparseable(f'{source}:{i + 1}: if(EXISTS with no matching endif()')
            blocks_found.append((i, else_at, j))
            i = j + 1
        else:
            i += 1
    return blocks_found


def check_single_source(text: str, source: Path) -> None:
    wrapper = len(re.findall(r'function\(pineforge_test_is_source_bound ', text))
    if wrapper != 1:
        fail(source, 0, f'pineforge_test_is_source_bound is defined {wrapper} times, want 1')
    if 'set(_pf_kernel_absent_rows)' not in text:
        fail(source, 0, 'the predicate list _pf_kernel_absent_rows is never set')
    if len(re.findall(r'pineforge_test_is_source_bound\(_pf_kernel_bound', text)) != 1:
        fail(source, 0, 'the predicate list is not built exactly once, by the one predicate')
    if len(re.findall(r'pineforge_test_is_source_bound\(_pf_reaches', text)) != 1:
        fail(source, 0, 'the kernel-only filter does not call pineforge_test_is_source_bound')


def check_blocks(lines: list[str], private: list[str], source: Path,
                 recorded: set[str]) -> dict:
    pinned = set(private)
    bound: dict[str, str] = {}
    counts = {'guarded_blocks': 0, 'kernel_registered': [], 'release_registered': [],
              'kernel_absent': [], 'missing_tu': [], 'unmatched': [],
              'blocks_without_predicate': 0, 'placeholder_without_predicate': 0}
    for i, else_at, end in blocks(lines, source):
        body = '\n'.join(lines[i:(else_at if else_at is not None else end)])
        absent_body = '\n'.join(lines[(else_at + 1):end]) if else_at is not None else ''
        registered = re.findall(r'pineforge_add_unit_test\((test_\w+)\)', body)
        registered += re.findall(r'add_executable\((test_\w+)[ )]', body)
        owners = [row for row in dict.fromkeys(registered) if row in pinned]
        if not owners:
            continue                      # not a private row block
        counts['guarded_blocks'] += 1
        guard_line = lines[i]
        wants = ' AND '.join(f'NOT {row} IN_LIST _pf_kernel_absent_rows' for row in owners)
        if wants not in guard_line:
            counts['blocks_without_predicate'] += 1
            fail(source, i + 1, f'the block registers {", ".join(owners)} without reading '
                                f'_pf_kernel_absent_rows: a source-bound row would build '
                                f'against the kernel-only archive')
        elif else_at is None or f'elseif({wants})' not in lines[else_at]:
            counts['placeholder_without_predicate'] += 1
            fail(source, (else_at or i) + 1,
                 f'the placeholder branch does not read _pf_kernel_absent_rows for '
                 f'{", ".join(owners)}: the row would keep a skip row in a profile that '
                 f'cannot link it')
        for row in owners:
            if row not in bound:
                bound[row] = source_bound(REPO / 'tests' / f'{row}.cpp', row,
                                          recorded)
        if not absent_body.strip():
            raise Unparseable(f'{source}:{i + 1}: no placeholder branch to read')
        if 'SKIP private fixtures not present: ' not in absent_body:
            fail(source, i + 1,
                 f'the absent branch of {", ".join(owners)} has no pinned SKIP literal')
        counts['release_registered'].extend(owners)
        for row in owners:
            if bound[row] == 'bound':
                counts['kernel_absent'].append(row)
            elif bound[row] == 'missing':
                counts['missing_tu'].append(row)
            else:
                counts['kernel_registered'].append(row)
    # Every pinned private row must be registered by one of those blocks: a
    # bare registration would build against the kernel-only archive.
    unmatched = sorted(pinned - set(counts['release_registered']))
    counts['unmatched'] = unmatched
    for row in unmatched:
        fail(source, 0, f'the private-row pin names {row}, and no guarded '
                        f'private block registers it: a bare registration '
                        f'would build against the kernel-only archive')
    return counts


def check_preparer_pairs(text: str, source: Path) -> int:
    """(d): every (row, provider-manifest) pair the guarded receipt blocks
    name agrees with the providers the preparer list gives that row."""
    bindings = dict(re.findall(
        r'set\((_pf_manifest_\w+)\s+\$\{PROJECT_SOURCE_DIR\}/(\S+)\)', text))
    if not bindings:
        raise Unparseable(f'{source}: no _pf_manifest_ bindings to read')
    try:
        import check_abi_receipt_skips as skips
        from test_abi_receipt_skips import PrivateFixtureManifests as receipt_rows
        from prepare_settlement_cpp_abi_base import PROVIDERS
    except ImportError as error:                      # pragma: no cover
        raise Unparseable(f'the preparer modules are unimportable: {error}')

    def manifest(role: str) -> str:
        return str(PROVIDERS[role]['manifest'].relative_to(skips.ROOT))

    pairs = 0
    for row, roles in receipt_rows.ROW_PROVIDERS.items():
        try:
            start = text.index('NAME ' + row)
        except ValueError:
            fail(source, 0, f'the receipt row {row} is not registered at all')
            continue
        guard = text.rindex('if(', 0, start)
        guarded = {bindings[name] for name in re.findall(
            r'EXISTS \$\{(_pf_manifest_\w+)\}', text[guard:start])}
        wanted = {manifest(role) for role in roles}
        if guarded != wanted:
            fail(source, text[:guard].count('\n') + 1,
                 f'{row}: the guard names {sorted(guarded)}, the preparer gives '
                 f'{sorted(wanted)}')
        pairs += len(wanted)
        placeholder = re.search(
            r'NAME ' + row + r' COMMAND \$\{CMAKE_COMMAND\} -E echo '
            r'"SKIP private fixtures not present: (\S+)"', text)
        if placeholder is None or placeholder.group(1) != manifest(roles[0]):
            fail(source, 0, f'{row}: the absent state does not name the provider manifest')
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cmakelists', type=Path,
                        default=REPO / 'tests' / 'CMakeLists.txt')
    args = parser.parse_args()
    source = args.cmakelists
    try:
        text = strip_comments(source.read_text())
        lines = text.splitlines()
        private = read_private_rows(text, source)
        recorded = read_recorded_source_bound(text, source)
        check_single_source(text, source)
        counts = check_blocks(lines, private, source, recorded)
        pairs = check_preparer_pairs(text, source)
    except Unparseable as error:
        print(f'check_kernel_profile_rows: {error}')
        return 2
    for line in FAILURES:
        print(f'check_kernel_profile_rows: {line}')
    if FAILURES:
        return 1
    print(f'private rows pinned: {len(private)}')
    print(f'guarded private blocks reading the predicate list: {counts["guarded_blocks"]}')
    print(f'profiles, derived by the same predicate: '
          f'kernel-absent {len(counts["kernel_absent"])} '
          f'(source-bound rows) -> release registers '
          f'{len(counts["release_registered"])} pinned rows, kernel registers '
          f'{len(counts["kernel_registered"])}, and '
          f'{len(counts["missing_tu"])} whose TU the tree does not have '
          f'(counted in neither profile)')
    print(f'pinned rows no guarded private block registers: '
          f'{len(counts["unmatched"])}')
    for row in sorted(counts['missing_tu']):
        print(f'  TU the tree does not have: {row} (not counted in either profile)')
    print(f'preparer/row pairs checked: {pairs}')
    # Both closing counts are COMPUTED from their own fail sites: a guard line
    # that does not name the predicate list (blocks not reading it), and a
    # placeholder branch that does not (the rows that would keep a skip row in
    # a profile that cannot link them).
    print(f'kernel targets linking without their prerequisites: '
          f'{counts["placeholder_without_predicate"]}')
    print(f'blocks not reading the predicate list: '
          f'{counts["blocks_without_predicate"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
