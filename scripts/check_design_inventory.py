#!/usr/bin/env python3
"""The design doc's §1 inventory must not contradict a passing kernel test.

Why this exists
---------------
``docs/design/native-feature-parity.md`` §1 is the campaign's inventory: one
row per adapter feature, with a **Native today** verdict of ``yes`` /
``partial`` / ``no`` read off the tree *when the row was written*. Lanes then
closed those gaps, and the rows did not move. The second claimed-vs-actual
audit found five rows reading ``partial`` / ``no`` for capabilities its own
bare-host program had just driven, while other rows had been given a closure
marker by the lane that closed them — so the same table carried two
conventions and a reader could not tell which ``partial`` was current.

The repair is the doc's own convention, not a verdict rewrite: a row keeps the
finding it recorded (that is the audit's record) and **appends a closure
marker** naming what landed and the test that proves it. This guard holds that
convention:

* a row whose verdict is ``no`` or ``partial`` and which names a test that runs
  in the **kernel profile** must carry a closure marker — the capability is
  demonstrably there, so the bare verdict is a contradiction;
* a **gap** row — one whose verdict is ``no`` or ``partial`` — that carries a
  closure marker must name at least one kernel-profile test: a marker with no
  source-free witness is a claim, not evidence. A row with no such verdict is
  not a gap row and is not asked for one; the price-grid waiver, whose subject
  is what the *adapter* cannot adopt, is the case that matters;
* every ``tests/…`` file any §1 row names must exist.

"Runs in the kernel profile" is not a heuristic
-----------------------------------------------
``tests/CMakeLists.txt`` decides it, and this guard transliterates that
decision rather than guessing at it. Two halves:

1. **Registration.** ``TEST_SOURCES`` is built by one ``set()`` plus a series
   of ``list(APPEND …)`` / ``list(REMOVE_ITEM …)`` calls. Those are replayed
   here for that one variable (and for the one helper list a ``REMOVE_ITEM``
   expands, ``L3A_LEGACY_OWNER_TEST_SOURCES``), so the result is the same set
   CMake computes.
2. **Reach.** ``pineforge_test_reaches_source_layer`` walks a TU's transitive
   *quoted* include closure and answers TRUE as soon as a line matches
   ``pineforge/(source|compat/pine)/``. With
   ``PINEFORGE_BUILD_SOURCE_LAYER=OFF`` those TUs are removed from
   ``TEST_SOURCES``; the rest are the kernel profile's rows. The walk below is
   that function, line for line: quoted includes are resolved against the
   including file's own directory, angled includes are only tested for the
   forbidden prefix, and a file that does not exist is skipped.

Both halves are readable from the tree alone, so the guard needs no configured
build. Where one exists, ``--ctest-list FILE`` takes the output of
``ctest -N`` from a kernel build directory and cross-checks the reimplemented
set against the rows CTest actually registered; a disagreement is reported as
an infrastructure finding, because it means this file has drifted from
``tests/CMakeLists.txt``.

Exit status
-----------
0 when every row is consistent. 2 when the document's shape cannot be read at
all (no §1, no rows, no kernel tests found) — that is a broken checker, not a
failing document, and it must not look like a pass.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from cmake_test_includes import registration_statements

ROOT = Path(__file__).resolve().parent.parent

DESIGN = 'docs/design/native-feature-parity.md'
TESTS_CMAKE = 'tests/CMakeLists.txt'

#: §1 runs from its own heading to the next top-level section.
SECTION_HEADING = '## 1. Inventory'

#: The verdicts a row's **Native today** cell can open with. The bold span may
#: carry a qualifier the table uses - ``**partial (inert)**``, ``**yes,
#: shared**`` - and ``n/a`` marks a pure TradingView-quirk row, which has no
#: native capability to close. A cell whose verdict is neither (the waiver row
#: reads ``**measured-infeasible for the Pine adapter**``) is not policed: the
#: guard's subject is a *contradiction* between a no/partial verdict and a
#: passing kernel test, not the table's grammar.
VERDICT_RE = re.compile(r'\*\*\s*(yes|partial|no)\b[^*]*\*\*|^n/a\b')

#: The closure-marker convention the table already uses: a bold lead-in whose
#: first word says the row has moved since its verdict was taken. ``Closed by
#: audit lane N6:`` and ``Status (audit lane N7):`` are the two the gap waves
#: wrote; the rest are the spellings the later lanes added.
MARKER_RE = re.compile(
    r'\*\*(?:Closed|Extended|Done|Status|Mostly done|Retained|Overtaken|Waiver|'
    r'Native-only by ruling|measured-infeasible)\b')

#: A test file a row cites, with or without its ``tests/`` prefix.
TEST_RE = re.compile(r'`?(?:tests/)?(test_[A-Za-z0-9_]+\.(?:cpp|c))`?')

FORBIDDEN_INCLUDE = re.compile(r'pineforge/(?:source|compat/pine)/')
ANY_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include')
QUOTED_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"')


class ShapeError(RuntimeError):
    """The document or the CMake list cannot be read; the guard is broken."""


# --------------------------------------------------------------------------
# Half 1: which test TUs tests/CMakeLists.txt registers
# --------------------------------------------------------------------------

def reaches_source_layer(entry: Path) -> bool:
    """``pineforge_test_reaches_source_layer`` over a TU's include closure."""
    pending = [entry]
    seen: set[Path] = set()
    while pending:
        current = pending.pop(0)
        if current in seen:
            continue
        seen.add(current)
        if not current.is_file():
            continue
        for line in current.read_text(errors='replace').splitlines():
            if not ANY_INCLUDE.match(line):
                continue
            if FORBIDDEN_INCLUDE.search(line):
                return True
            quoted = QUOTED_INCLUDE.match(line)
            if quoted:
                pending.append(current.parent / quoted.group(1))
    return False


def _fragment_enabled(condition: str, root: Path) -> bool:
    """Recognise the exclusions in the repository's registration fragments."""
    for term in re.split(r'\s+AND\s+', condition.strip()):
        if term in {'PINEFORGE_BUILD_SOURCE_LAYER', 'PINEFORGE_BUILD_CORPUS_STRATEGIES',
                    'CMAKE_BUILD_TYPE STREQUAL "Debug"', 'FALSE', 'OFF', '0'}:
            return False
        exists = re.fullmatch(r'EXISTS "\$\{PROJECT_SOURCE_DIR\}/([^"]+)"', term)
        if exists and not (root / exists[1]).exists():
            return False
    return True


def registrations(root: Path) -> tuple[set[str], dict[str, list[str]], dict[str, set[str]]]:
    """Source-list members, executable sources and literal CTest command targets."""
    cmake = root / TESTS_CMAKE
    if not cmake.is_file():
        raise ShapeError('no such file: ' + TESTS_CMAKE)
    lists: dict[str, list[str]] = {}
    direct: set[str] = set()
    targets: dict[str, list[str]] = {}
    commands: dict[str, set[str]] = {}
    active = [True]
    try:
        for source, cmd in registration_statements(cmake):
            name = cmd.name.lower()
            if source != cmake:
                if name == 'if':
                    active.append(active[-1] and _fragment_enabled(cmd.body, root))
                    continue
                if name == 'endif':
                    if len(active) == 1:
                        raise ShapeError(f'{source}:{cmd.line}: endif() without if()')
                    active.pop()
                    continue
                if not active[-1]:
                    continue
                if name == 'pineforge_add_unit_test':
                    # A direct registration can stand without a TEST_SOURCES entry.
                    direct.add(cmd.body.strip())
            words = cmd.body.split()
            if name == 'set' and words:
                lists[words[0]] = words[1:]
            elif name == 'list' and len(words) >= 2 and words[1].endswith('TEST_SOURCES'):
                operation, key = words[:2]
                items = []
                for word in words[2:]:
                    ref = re.fullmatch(r'\$\{(\w+)\}', word)
                    items.extend(lists.get(ref[1], []) if ref else [word])
                current = lists.setdefault(key, [])
                if operation.upper() == 'APPEND':
                    current.extend(items)
                elif operation.upper() == 'REMOVE_ITEM':
                    lists[key] = [entry for entry in current if entry not in items]
                else:
                    raise ShapeError(f'{source}:{cmd.line}: unhandled list({operation}) on {key}')
            elif name == 'add_executable' and words and words[0].startswith('test_'):
                targets[words[0]] = [word for word in words[1:] if word.endswith(('.cpp', '.c'))]
            elif name == 'add_test' and len(words) >= 4 and words[:3:2] == ['NAME', 'COMMAND']:
                row, target = words[1], words[3]
                if re.fullmatch(r'[\w.-]+', row):
                    commands.setdefault(target, set()).add(row)
    except ValueError as error:
        raise ShapeError(str(error)) from error
    if 'TEST_SOURCES' not in lists:
        raise ShapeError('TEST_SOURCES is not set in ' + TESTS_CMAKE)
    if len(active) != 1:
        raise ShapeError(f'{cmake}: unterminated condition in included test registrations')
    return direct.union(lists['TEST_SOURCES']), targets, commands


def kernel_units(root: Path, members: set[str], targets: dict[str, list[str]]) -> set[str]:
    out: set[str] = set()
    for name in members:
        for suffix in ('.cpp', '.c'):
            entry = root / 'tests' / (name + suffix)
            if entry.is_file():
                if not reaches_source_layer(entry):
                    out.add(name + suffix)
                break
    for sources in targets.values():
        present = [name for name in sources if (root / 'tests' / name).is_file()]
        if present and not any(reaches_source_layer(root / 'tests' / name)
                               for name in present):
            out.update(present)
    if not out:
        raise ShapeError('no kernel-profile test TU found; the reach walk is broken')
    return out


def kernel_profile_tests(root: Path) -> set[str]:
    members, targets, _ = registrations(root)
    return kernel_units(root, members, targets)


def ctest_rows(listing: str) -> set[str]:
    """Row names from ``ctest -N`` output (``  Test #12: name``)."""
    return set(re.findall(r'^\s*Test\s+#\d+:\s+(\S+)\s*$', listing, re.MULTILINE))


# --------------------------------------------------------------------------
# The document
# --------------------------------------------------------------------------

class Row:
    def __init__(self, line_number: int, identifier: str, verdict: str,
                 native_cell: str, tests: tuple[str, ...], marked: bool) -> None:
        self.line = line_number
        self.id = identifier
        self.verdict = verdict
        self.native = native_cell
        self.tests = tests
        self.marked = marked


def inventory_rows(text: str) -> list[Row]:
    start = text.find(SECTION_HEADING)
    if start < 0:
        raise ShapeError('section not found in ' + DESIGN + ': ' + SECTION_HEADING)
    body = text[start + len(SECTION_HEADING):]
    following = re.search(r'\n## ', body)
    if following:
        body = body[:following.start()]
    offset = text[:start + len(SECTION_HEADING)].count('\n') + 1
    rows: list[Row] = []
    for index, line in enumerate(body.splitlines(), start=offset):
        if not line.startswith('| '):
            continue
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if len(cells) < 3:
            continue
        identifier = cells[0]
        if identifier == 'ID' or not re.fullmatch(r'[A-Z]{2}[0-9]{0,2}[a-z]?', identifier):
            continue                       # the header row and its separator
        found = VERDICT_RE.search(cells[2])
        verdict = (found.group(1) or 'n/a') if found else 'other'
        rows.append(Row(index, identifier, verdict, cells[2],
                        tuple(dict.fromkeys(TEST_RE.findall(cells[2]))),
                        bool(MARKER_RE.search(cells[2]))))
    if not rows:
        raise ShapeError('no inventory rows parsed from ' + DESIGN + ' §1')
    return rows


def check(root: Path, listing: str | None = None) -> tuple[list[str], dict]:
    design = root / DESIGN
    if not design.is_file():
        raise ShapeError('no such file: ' + DESIGN)
    rows = inventory_rows(design.read_text())
    members, targets, commands = registrations(root)
    kernel = kernel_units(root, members, targets)
    findings: list[str] = []

    if listing is not None:
        registered = ctest_rows(listing)
        if not registered:
            raise ShapeError('--ctest-list parsed no rows; expected "Test #N: name" lines')
        owner = {name: target for target, sources in targets.items()
                 for name in sources}
        expected = set()
        missing = []
        for source in sorted(kernel):
            target = owner.get(source, source.rsplit('.', 1)[0])
            # Some units register only named slices of the executable.
            names = commands.get(target, {target})
            expected.update(names)
            for row in sorted(names - registered):
                missing.append(source if row == target else f'{source}: {row}')
        if missing:
            findings.append(
                f'{TESTS_CMAKE}: {len(missing)} kernel-profile test row(s) '
                f'that the kernel build did not register '
                f'({", ".join(missing[:6])}{", …" if len(missing) > 6 else ""}); '
                'this file has drifted from tests/CMakeLists.txt')
        # A listed row can name a compiled TU directly or run it under an alias.
        # A known script command is not a TU, even if a same-named file exists.
        def compiled_target(name):
            return (name in targets or name in members or
                    (re.fullmatch(r'[\w.-]+', name) and
                     any((root / 'tests' / (name + ext)).is_file() for ext in ('.cpp', '.c'))))

        named = set().union(*commands.values())
        native_rows = {row for target, names in commands.items() if compiled_target(target)
                       for row in names}
        compiled = {name for name in registered - expected
                    if name in native_rows or (name not in named and compiled_target(name))}
        wrong = sorted(compiled.intersection(members) | compiled.intersection(targets))
        if wrong:
            findings.append(
                f'{TESTS_CMAKE}: the kernel build registered {len(wrong)} row(s) '
                f'the reach walk calls source-bound '
                f'({", ".join(wrong[:6])}{", …" if len(wrong) > 6 else ""}); '
                'this file has drifted from tests/CMakeLists.txt')
        extra = sorted(compiled.difference(wrong))
        if extra:
            findings.append(f'{TESTS_CMAKE}: compiled CTest rows missing from the '
                            f'kernel inventory: {", ".join(extra)}')

    for row in rows:
        for name in row.tests:
            if not (root / 'tests' / name).is_file():
                findings.append(f'{DESIGN}:{row.line}: row {row.id} cites '
                                f'tests/{name}, which is not in the tree')
        witnesses = sorted(name for name in row.tests if name in kernel)
        if row.verdict in ('no', 'partial') and witnesses and not row.marked:
            findings.append(
                f'{DESIGN}:{row.line}: row {row.id} reads **{row.verdict}** while '
                f'{", ".join("tests/" + name for name in witnesses)} runs in the '
                'kernel profile. Append the closure marker the table already uses '
                '("**Closed:** <what landed> (`tests/…`)") instead of leaving the '
                'verdict to contradict a passing source-free test.')
        if row.marked and row.verdict in ('no', 'partial') and not witnesses:
            named = (', '.join('tests/' + name for name in row.tests)
                     if row.tests else 'no test at all')
            findings.append(
                f'{DESIGN}:{row.line}: row {row.id} carries a closure marker but '
                f'names {named} — no kernel-profile test. A closure marker must '
                'name a source-free witness; a capability only the adapter '
                'exercises is not closed for a bare host.')
    summary = {
        'rows': len(rows),
        'marked': sum(1 for row in rows if row.marked),
        'kernelTests': len(kernel),
        'findings': len(findings),
    }
    return findings, summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="The design doc's §1 inventory against the kernel-profile tests.")
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--ctest-list', type=Path, default=None,
                        help='output of `ctest -N` from a kernel build dir, to '
                             'cross-check the reimplemented kernel-profile set')
    parser.add_argument('--list-kernel-tests', action='store_true',
                        help='print the kernel-profile TU set and exit 0')
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        if args.list_kernel_tests:
            for name in sorted(kernel_profile_tests(root)):
                print(name)
            return 0
        listing = args.ctest_list.read_text() if args.ctest_list else None
        findings, summary = check(root, listing)
    except ShapeError as error:
        print(f'check_design_inventory: {error}', file=sys.stderr)
        return 2
    print(f'check_design_inventory: {summary["rows"]} inventory rows, '
          f'{summary["marked"]} carrying a closure marker, against '
          f'{summary["kernelTests"]} kernel-profile test units: '
          f'{summary["findings"]} findings'
          + ('' if findings else ' ... OK'))
    for finding in findings:
        print('  ' + finding)
    return 1 if findings else 0


if __name__ == '__main__':
    raise SystemExit(main())
