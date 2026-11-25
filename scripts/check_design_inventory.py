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
   in source order, including writes to helper lists. An undecidable helper
   cannot supply a registration or a closure witness.
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
import os
import re
import struct
import sys
from pathlib import Path

from check_lanes_cmake_d import _words
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


def guarded(commands):
    """Yield each statement with the conditions selecting its arm at every depth."""
    stack = []
    for source, cmd in commands:
        name = cmd.name.lower()
        if name == 'if':
            stack.append([(source, cmd)])
        elif name in ('else', 'elseif', 'endif'):
            if not stack or stack[-1][0][0] != source:
                raise ShapeError(f'{source}:{cmd.line}: {name}() without if()')
            if name == 'endif':
                stack.pop()
            elif stack[-1][-1][1].name.lower() == 'else':
                raise ShapeError(f'{source}:{cmd.line}: {name}() after else()')
            else:
                stack[-1].append((source, cmd))
        else:
            # An elseif or else requires every preceding arm to have been false.
            yield source, cmd, [(path, arm, arm is chain[-1][1])
                                for chain in stack for path, arm in chain
                                if arm.name.lower() != 'else']
    if stack:
        source, start = stack[-1][0]
        raise ShapeError(f'{source}:{start.line}: unterminated if()')


def list_value(lists, name):
    value = lists.get(name)
    if isinstance(value, ShapeError):
        raise value
    return value


def _condition(body, root, lists, targets, unit=None):
    """Kernel Release configuration. None means this condition is not modelled."""
    body = ' '.join(body.split())
    if body == 'CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE':
        return sys.platform.startswith('linux') or sys.platform == 'darwin'
    if re.search(r'\bOR\b|[()]', body):
        return None
    terms = []
    for term in body.split(' AND '):
        negate = term.startswith('NOT ')
        term = term.removeprefix('NOT ')
        if term in {'TRUE', 'ON', '1', 'PINEFORGE_BUILD_LIVE_RUNNER',
                    'CMAKE_BUILD_TYPE STREQUAL "Release"'}:
            value = True
        elif term in {'FALSE', 'OFF', '0', 'PINEFORGE_BUILD_SOURCE_LAYER',
                      'PINEFORGE_BUILD_CORPUS_STRATEGIES', 'PINEFORGE_ENABLE_COVERAGE',
                      'PINEFORGE_ENABLE_SANITIZERS', 'PINEFORGE_BUILD_TUTORIAL',
                      'PINEFORGE_LAB_ONLY_TESTS', 'PINEFORGE_VALUE_INVALID_FIXTURES',
                      'CMAKE_BUILD_TYPE STREQUAL "Debug"'}:
            value = False
        elif term == 'UNIX':
            value = os.name == 'posix'
        elif term == 'CMAKE_SYSTEM_NAME STREQUAL "Linux"':
            value = sys.platform.startswith('linux')
        elif term == 'CMAKE_SIZEOF_VOID_P EQUAL 8':
            value = struct.calcsize('P') == 8
        elif (unit and (m := re.fullmatch(r'(\w+) STREQUAL "([\w.-]+)"', term))
              and m[1] == unit[0]):
            value = unit[1] == m[2]
        elif m := re.fullmatch(r'EXISTS "\$\{(PROJECT_SOURCE_DIR|CMAKE_CURRENT_SOURCE_DIR)\}/([^"$]+)"', term):
            value = ((root if m[1] == 'PROJECT_SOURCE_DIR' else root / 'tests') / m[2]).exists()
        elif m := re.fullmatch(r'EXISTS \$\{(\w+)\}', term):
            paths = list_value(lists, m[1]) or []
            value = (_condition(f'EXISTS "{paths[0]}"', root, lists, targets)
                     if len(paths) == 1 and paths[0].startswith(
                         ('${PROJECT_SOURCE_DIR}/', '${CMAKE_CURRENT_SOURCE_DIR}/')) else None)
        elif m := re.fullmatch(r'([\w.-]+) IN_LIST _pf_kernel_absent_rows', term):
            value = (m[1] in (list_value(lists, '_pf_source_bound_private_rows') or ()) or
                     reaches_source_layer(root / 'tests' / (m[1] + '.cpp')))
        elif m := re.fullmatch(r'TARGET (test_[\w.-]+)', term):
            value = m[1] in targets
        else:
            value = None
        if value is not None and value == negate:
            return False
        terms.append(value)
    return None if None in terms else True


def selected(guards, root, lists, targets, *, unit=None, script_only=False):
    unknown = None
    for source, arm, wanted in guards:
        result = _condition(arm.body, root, lists, targets, unit)
        if result is None:
            unknown = f'{source}:{arm.line}: unsupported condition {arm.body.strip()!r}'
        elif result != wanted:
            return False
    if unknown and not script_only:
        raise ShapeError(unknown)
    return True


TARGET_FILE = re.compile(r'\$<TARGET_FILE:([\w.+-]+)>')


def command_targets(words, source, line):
    """Keep the executable and any literal target files passed to a launcher."""
    target, *args = words
    if '$<' in target:
        match = TARGET_FILE.fullmatch(target)
        if not match:
            raise ShapeError(f'{source}:{line}: unsupported test command {target!r}')
        target = match[1]
    result = {target}
    for arg in args:
        if '$<TARGET_FILE:' in arg:
            if '$<TARGET_FILE:' in TARGET_FILE.sub('', arg):
                raise ShapeError(f'{source}:{line}: unsupported target file {arg!r}')
            result.update(TARGET_FILE.findall(arg))
    return result


def registrations(root: Path):
    """Source-list members, executable sources, selected rows and declared commands."""
    cmake = root / TESTS_CMAKE
    if not cmake.is_file():
        raise ShapeError('no such file: ' + TESTS_CMAKE)
    lists: dict[str, list[str] | ShapeError] = {}
    direct: set[str] = set()
    built: set[str] = set()
    targets: dict[str, list[str]] = {}
    commands: dict[str, set[str]] = {}
    declared: dict[str, set[str]] = {}
    unit_parameter = None
    unit_rules = []
    function = None
    loops = []

    def register_unit(target):
        if not re.fullmatch(r'[\w.+-]+', target):
            raise ShapeError(f'{cmake}: unsupported unit target {target!r}')
        if target in direct:
            return
        direct.add(target)
        entry = root / 'tests' / (target + '.cpp')
        if entry.is_file() and not reaches_source_layer(entry):
            built.add(target)
        rows = commands.setdefault(target, set())
        if unit_parameter is None:  # Standalone fragments use the usual primary.
            rows.add(target)
        for conditions, row in unit_rules:
            if selected(conditions, root, lists, built, unit=(unit_parameter, target)):
                row = row.replace('${' + unit_parameter + '}', target)
                if not re.fullmatch(r'[\w.+-]+', row):
                    raise ShapeError(f'unsupported unit row {row!r} for {target}')
                rows.add(row)

    def row_names(name, source, line):
        if re.fullmatch(r'[\w.+-]+', name):
            return [name]
        for variable, *items in reversed(loops):
            token = '${' + variable + '}'
            if token not in name:
                continue
            if items[:2] == ['IN', 'ITEMS']:
                items = items[2:]
            if items and items[0] not in ('IN', 'RANGE') and all(
                    re.fullmatch(r'[\w.+-]+', name.replace(token, item)) for item in items):
                return [name.replace(token, item) for item in items]
        raise ShapeError(f'{source}:{line}: unsupported compiled row name {name!r}')

    try:
        for source, cmd, guards in guarded(registration_statements(cmake)):
            name = cmd.name.lower()
            words = _words(cmd.body)
            if name in ('function', 'macro'):
                function = words[0]
                if function == 'pineforge_add_unit_test':
                    if len(words) != 2:
                        raise ShapeError(f'{source}:{cmd.line}: unsupported unit helper parameters')
                    unit_parameter = words[1]
                continue
            if name in ('endfunction', 'endmacro'):
                function = None
                continue
            if function:
                if (function == 'pineforge_add_unit_test' and name == 'add_test'
                        and len(words) >= 4 and words[:3:2] == ['NAME', 'COMMAND']
                        and words[3] == '${' + unit_parameter + '}'):
                    unit_rules.append((guards, words[1]))
                continue
            if name == 'foreach':
                loops.append(words)
                continue
            if name == 'endforeach':
                if not loops:
                    raise ShapeError(f'{source}:{cmd.line}: endforeach() without foreach()')
                loops.pop()
                continue
            if (name in ('set', 'unset') and words) or (name == 'list' and len(words) >= 2):
                if name == 'list':
                    operation, key, *values = words
                    operation = operation.upper()
                    if operation in ('LENGTH', 'GET', 'JOIN', 'SUBLIST', 'FIND'):
                        key = words[-1]  # These write an output, not the source list.
                else:
                    operation = name.upper()
                    key, *values = words
                if (operation == 'REMOVE_ITEM' and key == 'TEST_SOURCES'
                        and values == ['${_pf_source_bound_tests}']):
                    continue  # kernel_units applies the include walk.
                try:
                    if not selected(guards, root, lists, built):
                        lists.setdefault(key, [])  # A disabled write supplies no value.
                        continue
                    if loops or operation not in ('SET', 'UNSET', 'APPEND', 'REMOVE_ITEM'):
                        raise ShapeError(f'unmodelled {name}({cmd.body.strip()})')
                    if ('CACHE' in values or 'PARENT_SCOPE' in values
                            or (operation == 'UNSET' and values)):
                        raise ShapeError(f'unmodelled variable scope for {key}')
                    items = []
                    for word in values:
                        ref = re.fullmatch(r'\$\{(\w+)\}', word)
                        if ref and ref[1] not in ('CMAKE_COMMAND', 'Python3_EXECUTABLE'):
                            value = list_value(lists, ref[1])
                            if value is None:
                                raise ShapeError(f'unknown source list {word}')
                            items.extend(value)
                        else:
                            items.append(word)
                    if key.endswith('TEST_SOURCES'):
                        for item in items:
                            if not re.fullmatch(r'[\w.+-]+', item):
                                raise ShapeError(f'unsupported source name {item!r}')
                    if operation in ('SET', 'UNSET'):
                        lists[key] = items
                    elif operation == 'APPEND':
                        lists[key] = (list_value(lists, key) or []) + items
                    else:
                        lists[key] = [v for v in (list_value(lists, key) or []) if v not in items]
                except ShapeError as error:
                    # Only consumers need a decidable value. A later set() can replace it.
                    lists[key] = ShapeError(f'{source}:{cmd.line}: cannot determine {key}: {error}')
            elif name == 'pineforge_add_unit_test' and selected(guards, root, lists, built):
                if words == ['${test_name}']:  # The TEST_SOURCES registration loop.
                    for target in list_value(lists, 'TEST_SOURCES') or []:
                        register_unit(target)
                elif len(words) == 1 and re.fullmatch(r'[\w.+-]+', words[0]):
                    register_unit(words[0])
                else:
                    raise ShapeError(f'{source}:{cmd.line}: unsupported unit target {cmd.body!r}')
            elif (name == 'add_executable' and words
                  and (words[0].startswith('test_') or any(w.startswith('test_') for w in words[1:]))):
                if selected(guards, root, lists, built):
                    built.add(words[0])
                    sources = []
                    for word in words[1:]:
                        var = re.fullmatch(r'\$\{(\w+)\}', word)
                        if var:
                            value = list_value(lists, var[1])
                            if value is None:
                                raise ShapeError(f'{source}:{cmd.line}: unknown executable sources {word}')
                            sources.extend(value)
                        else:
                            sources.append(word)
                    targets[words[0]] = [s for s in sources if s.endswith(('.cpp', '.c'))]
                    if not targets[words[0]]:
                        raise ShapeError(f'{source}:{cmd.line}: unmodelled sources of {words[0]}')
            elif name in ('add_test', 'pineforge_add_twin_test'):
                if name == 'pineforge_add_twin_test':
                    row, target = words[:2]
                    owners = {target}
                    available = target in built
                else:
                    if len(words) < 4 or words[:3:2] != ['NAME', 'COMMAND']:
                        raise ShapeError(f'{source}:{cmd.line}: unsupported add_test({cmd.body})')
                    row = words[1]
                    args = words[3:]
                    ref = re.fullmatch(r'\$\{(\w+)\}', args[0])
                    if ref and ref[1] not in ('Python3_EXECUTABLE', 'CMAKE_COMMAND'):
                        if not selected(guards, root, lists, built):
                            continue
                        launcher = list_value(lists, ref[1])
                        if not launcher:
                            raise ShapeError(f'{source}:{cmd.line}: unsupported command for {row}: {args[0]}')
                        args = [*launcher, *args[1:]]
                    owners = command_targets(args, source, cmd.line)
                    available = True
                # An optional script-only registration cannot supply a TU witness.
                script_only = not any(t in targets or t in direct or t.startswith(('test_', 'tool_'))
                                      for t in owners)
                enabled = available and selected(guards, root, lists, built, script_only=script_only)
                if '$' in row and (not enabled or script_only):
                    continue
                names = row_names(row, source, cmd.line)
                for owner in owners:
                    declared.setdefault(owner, set()).update(names)
                    if enabled:
                        commands.setdefault(owner, set()).update(names)
    except ValueError as error:
        raise ShapeError(str(error)) from error
    if loops or function:
        raise ShapeError(f'{cmake}: unterminated registration loop or function')
    sources = list_value(lists, 'TEST_SOURCES')
    if sources is None:
        raise ShapeError('TEST_SOURCES is not set in ' + TESTS_CMAKE)
    return direct.union(sources), targets, commands, declared


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
    members, targets, _, _ = registrations(root)
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
    members, targets, commands, declared = registrations(root)
    kernel = kernel_units(root, members, targets)
    findings: list[str] = []

    if listing is not None:
        registered = ctest_rows(listing)
        if not registered:
            raise ShapeError('--ctest-list parsed no rows; expected "Test #N: name" lines')
        # An explicit add_executable in a selected arm exists even when its TU
        # has a conditional adapter include. Only TEST_SOURCES goes through the
        # include filter. Keep TU witnesses conservative, but compare its rows.
        owners = dict(targets)
        for source in kernel:
            target = source.rsplit('.', 1)[0]
            if target in members:
                owners.setdefault(target, [source])
        expected = set()
        missing = []
        for target, sources in sorted(owners.items()):
            names = commands.get(target, {target})
            expected.update(names)
            for row in sorted(names - registered):
                missing.append(sources[0] if row == target else f'{sources[0]}: {row}')
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

        named = set().union(*declared.values())
        native_rows = {row for target, names in declared.items() if compiled_target(target)
                       for row in names}
        # An active script (including a missing-fixture placeholder) takes
        # precedence over an alternative compiled registration of the same name.
        active_names = set().union(*commands.values())
        active_compiled = {row for target, names in commands.items() if compiled_target(target)
                           for row in names}
        native_rows -= active_names - active_compiled
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
            print('CTest registry: NOT COMPARED (source list only)', file=sys.stderr)
            return 0
        listing = args.ctest_list.read_text() if args.ctest_list else None
        findings, summary = check(root, listing)
    except (OSError, ValueError, ShapeError) as error:
        print(f'check_design_inventory: NOT COMPARED: {error}', file=sys.stderr)
        return 2
    print(f'check_design_inventory: {summary["rows"]} inventory rows, '
          f'{summary["marked"]} carrying a closure marker, against '
          f'{summary["kernelTests"]} kernel-profile test units: '
          f'{summary["findings"]} findings'
          + ('' if findings else ' ... OK'))
    for finding in findings:
        print('  ' + finding)
    if listing is None:
        print('CTest registry: NOT COMPARED (no --ctest-list supplied)')
    return 1 if findings else 0


if __name__ == '__main__':
    raise SystemExit(main())
