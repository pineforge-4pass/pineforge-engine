#!/usr/bin/env python3
"""Check the design doc's §1 closure markers against kernel test registrations.

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
marker** naming what landed and citing a source-free test. This guard holds that
convention:

* a row whose verdict is ``no`` or ``partial`` and which names a kernel-profile
  witness must carry a closure marker;
* a **gap** row — one whose verdict is ``no`` or ``partial`` — that carries a
  closure marker must name at least one kernel-profile test: a marker with no
  source-free witness is a claim, not evidence. A row with no such verdict is
  not a gap row and is not asked for one; the price-grid waiver, whose subject
  is what the *adapter* cannot adopt, is the case that matters;
* every ``tests/…`` file any §1 row names must exist.

How the guard identifies a kernel-profile witness
-------------------------------------------------
``tests/CMakeLists.txt`` decides it, and this guard transliterates that
decision rather than guessing at it. Two halves:

1. **Registration.** ``TEST_SOURCES`` is built by one ``set()`` plus a series
   of ``list(APPEND …)`` / ``list(REMOVE_ITEM …)`` calls. Those are replayed
   in source order, including writes to helper lists. An undecidable helper
   cannot supply a registration or a closure witness.
2. **Reach.** ``pineforge_test_reaches_source_layer`` walks a TU's transitive
   *quoted* include closure and answers TRUE as soon as a line matches
   ``pineforge/(source|compat/pine)/``. With
   ``PINEFORGE_BUILD_SOURCE_LAYER=OFF`` CMake removes those members from
   ``TEST_SOURCES`` once, before the unit registration loop. Later explicit
   targets do not go through that filter. Their registrations are compared
   even when their includes cannot supply a source-free document witness.
   A script that takes a selected module's target file has a registry row,
   but its fixture sources do not become document witnesses either.

Both halves are readable from the tree alone, so the guard needs no configured
build. Where one exists, ``--ctest-list FILE`` takes the output of
``ctest -N`` from a kernel build directory and cross-checks the reimplemented
set against the rows CTest actually registered; a disagreement is reported as
an infrastructure finding, because it means this file has drifted from
``tests/CMakeLists.txt``. Disabled rows still count as registrations. A source
whose listed rows are all disabled cannot witness a closure. Neither CMake
registration nor a CTest listing shows whether a test ran or passed.

Exit status
-----------
0 when every row checked is consistent; 1 when there are findings. 2 when a
required input cannot be read or interpreted.
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
#: cited kernel-profile witness, not the table's grammar.
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
        elif term == '_pf_source_bound_tests':
            value = bool(list_value(lists, term))
        elif m := re.fullmatch(r'TARGET ([\w.+-]+)', term):
            value = targets.get(m[1], False if m[1].startswith('test_') else None)
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


def require_reach_producer(statements, index):
    """The replay models a loop with one producer and one guarded append."""
    source, append, guards = statements[index]
    outer = guards[:-1]
    if index >= 2 and index + 1 < len(statements):
        before = statements[index - 2:index]
        after = statements[index + 1]
        (_, loop, _), (_, call, _) = before
        _, end, _ = after
        if (all(path == source and branch == outer for path, _, branch in (*before, after))
                and loop.name.lower() == 'foreach'
                and _words(loop.body) == ['test_name', 'IN', 'LISTS', 'TEST_SOURCES']
                and call.name.lower() == 'pineforge_test_is_source_bound'
                and _words(call.body) == ['_pf_reaches', '${CMAKE_CURRENT_SOURCE_DIR}/${test_name}.cpp']
                and end.name.lower() == 'endforeach'):
            return
    raise ShapeError(f'{source}:{append.line}: unsupported producer for _pf_reaches '
                     'in the source-layer filter')


def registrations(root: Path, *, modules=None):
    """Unit members, executable sources, selected rows, declarations and missing sources.

    For a registry comparison, also collect module providers, separately from
    executable witnesses. None means no selected definition; a ShapeError is
    propagated if a selected test depends on the undecidable provider.
    """
    cmake = root / TESTS_CMAKE
    if not cmake.is_file():
        raise ShapeError('no such file: ' + TESTS_CMAKE)
    lists: dict[str, list[str] | ShapeError] = {}
    direct: set[str] = set()
    filtered: set[str] = set()
    built: dict[str, bool | None] = {}
    targets: dict[str, list[str]] = {}
    commands: dict[str, set[str]] = {}
    declared: dict[str, set[str]] = {}
    missing_units: dict[str, str] = {}
    unit_parameter = None
    unit_rules = []
    function = None
    loops = []
    unit_loop_seen = False

    def register_unit(target, source, line):
        if not re.fullmatch(r'[\w.+-]+', target):
            raise ShapeError(f'{source}:{line}: unsupported unit target {target!r}')
        if target in direct:
            return
        filename = target + '.cpp'
        if not (root / 'tests' / filename).is_file():
            built[target] = False
            missing_units.setdefault(target, f'{source.relative_to(root)}:{line}: '
                                     f'unit target {target} is missing source tests/{filename}')
            return
        direct.add(target)
        built[target] = True
        targets[target] = [filename]
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
        statements = list(guarded(registration_statements(cmake)))
        if modules is not None:
            # add_test may refer to a module defined later in the directory.
            # Its kind is known in advance; TARGET becomes true at its definition.
            for _, cmd, _ in statements:
                if cmd.name.lower() == 'add_library':
                    words = _words(cmd.body)
                    if words[1:2] == ['MODULE'] and re.fullmatch(r'[\w.+-]+', words[0]):
                        modules[words[0]] = None
                        built[words[0]] = False
        for index, (source, cmd, guards) in enumerate(statements):
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
                if (operation == 'APPEND' and key == '_pf_source_bound_tests'
                        and loops and loops[-1] == ['test_name', 'IN', 'LISTS', 'TEST_SOURCES']
                        and values == ['${test_name}'] and guards
                        and guards[-1][1].body.strip() == '_pf_reaches' and guards[-1][2]):
                    if selected(guards[:-1], root, lists, built):
                        require_reach_producer(statements, index)
                        # Declarations after the REMOVE_ITEM do not go through this loop.
                        private = list_value(lists, '_pf_source_bound_private_rows') or ()
                        units = list_value(lists, 'TEST_SOURCES') or []
                        lists[key] = (list_value(lists, key) or []) + [
                            t for t in units if t in private or
                            reaches_source_layer(root / 'tests' / (t + '.cpp'))]
                    continue
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
                        if key == 'TEST_SOURCES':
                            if values == ['${_pf_source_bound_tests}']:
                                filtered.update(items)
                            else:
                                filtered.difference_update(items)
                except ShapeError as error:
                    # Only consumers need a decidable value. A later set() can replace it.
                    lists[key] = ShapeError(f'{source}:{cmd.line}: cannot determine {key}: {error}')
            elif name == 'pineforge_add_unit_test' and selected(guards, root, lists, built):
                if words == ['${test_name}']:  # The TEST_SOURCES registration loop.
                    if not loops or loops[-1] not in (['test_name', '${TEST_SOURCES}'],
                                                      ['test_name', 'IN', 'LISTS', 'TEST_SOURCES']):
                        raise ShapeError(f'{source}:{cmd.line}: unsupported unit loop')
                    unit_loop_seen = True
                    for target in list_value(lists, 'TEST_SOURCES') or []:
                        register_unit(target, source, cmd.line)
                elif len(words) == 1 and re.fullmatch(r'[\w.+-]+', words[0]):
                    register_unit(words[0], source, cmd.line)
                else:
                    raise ShapeError(f'{source}:{cmd.line}: unsupported unit target {cmd.body!r}')
            elif (name == 'add_library' and words[1:2] == ['MODULE']
                  and modules is not None and words[0] in modules):
                module = words[0]
                try:
                    enabled = selected(guards, root, lists, built)
                except ShapeError as error:
                    # An undecidable unused definition cannot affect a row.
                    modules[module] = error
                    built[module] = None
                    continue
                if not enabled:
                    continue
                sources = words[2:]
                if loops or 'IMPORTED' in sources:
                    raise ShapeError(f'{source}:{cmd.line}: unsupported module definition for {module}')
                if not sources or not all(re.fullmatch(r'[\w./+-]+\.(?:cpp|c)', s)
                                          and (root / 'tests' / s).is_file() for s in sources):
                    raise ShapeError(f'{source}:{cmd.line}: cannot resolve module sources for {module}')
                modules[module] = sources
                built[module] = True
            elif (name == 'add_executable' and words
                  and (words[0].startswith('test_') or any(w.startswith('test_') for w in words[1:]))):
                if selected(guards, root, lists, built):
                    built[words[0]] = True
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
                    available = built.get(target, False)
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
                script_only = not any(t in targets or t in direct or (modules is not None and t in modules)
                                      or t.startswith(('test_', 'tool_'))
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
    if 'TEST_SOURCES' not in lists:
        raise ShapeError('TEST_SOURCES is not set in ' + TESTS_CMAKE)
    members = direct | filtered
    if not unit_loop_seen and unit_parameter is None:
        # A standalone source-list fragment omits the helper and its loop.
        # A complete list file must use the values present at its calls.
        sources = list_value(lists, 'TEST_SOURCES')
        members.update(sources)
        for name in sources:
            if name in targets:
                continue
            for suffix in ('.cpp', '.c'):
                entry = root / 'tests' / (name + suffix)
                if entry.is_file():
                    if not reaches_source_layer(entry):
                        targets[name] = [entry.name]
                        commands.setdefault(name, {name})
                    break
    return members, targets, commands, declared, list(missing_units.values())


def kernel_unit_rows(root: Path, targets: dict[str, list[str]], commands) -> dict[str, set[str]]:
    """Map each source-free test unit to the registered names of its targets."""
    out: dict[str, set[str]] = {}
    for target, sources in targets.items():
        if not (names := commands.get(target)):
            continue
        present = [name for name in sources if (root / 'tests' / name).is_file()]
        if present and not any(reaches_source_layer(root / 'tests' / name)
                               for name in present):
            for source in present:
                out.setdefault(source, set()).update(names)
    return out


def kernel_profile_tests(root: Path) -> set[str]:
    _, targets, commands, _, missing = registrations(root)
    if missing:
        raise ShapeError('; '.join(missing))
    kernel = set(kernel_unit_rows(root, targets, commands))
    if not kernel:
        raise ShapeError('no kernel-profile test TU found; the reach walk is broken')
    return kernel


def ctest_rows(listing: str) -> dict[str, bool]:
    """Validate ``ctest -N`` output and map each registered name to its enabled status."""
    enabled: dict[str, bool] = {}
    totals = []
    lines = listing.rstrip().splitlines()
    for index, line in enumerate(lines):
        line = line.strip()
        if re.match(r'Test\s+#', line):
            row = re.fullmatch(r'Test\s+#([0-9]+):\s+(\S.*)', line)
            if not row:
                raise ShapeError(f'--ctest-list has a malformed test row: {line!r}')
            number, label = row.groups()
            expected = len(enabled) + 1
            if number != str(expected):
                raise ShapeError(f'--ctest-list expected test #{expected}, got #{number}')
            name = label.removesuffix(' (Disabled)')
            if name in enabled:
                raise ShapeError(f'--ctest-list has a duplicate test name: {name}')
            enabled[name] = name == label
        elif line.startswith('Total Tests'):
            totals.append(index)
    if not enabled:
        raise ShapeError('--ctest-list parsed no rows; expected "Test #N: name" lines')
    if totals != [len(lines) - 1] or not (total := re.fullmatch(
            r'Total Tests: ([0-9]+)', lines[-1].strip())):
        raise ShapeError('--ctest-list expected one final "Total Tests: N" line')
    if total[1] != str(len(enabled)):
        raise ShapeError(f'--ctest-list Total Tests: {total[1]} does not match {len(enabled)} test row(s)')
    return enabled


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
    registered = ctest_rows(listing) if listing is not None else None
    modules = {}
    members, targets, commands, declared, findings = registrations(
        root, modules=modules if listing is not None else None)
    missing_unit_sources = bool(findings)
    kernel = kernel_unit_rows(root, targets, commands)
    if not kernel and not missing_unit_sources:
        raise ShapeError('no kernel-profile test TU found; the reach walk is broken')

    if registered is not None and not missing_unit_sources:
        # Compare the selected targets' rows independently of whether their
        # sources can witness a closure in the design document.
        owners = dict(targets)
        for module, sources in modules.items():
            if module in commands:
                if isinstance(sources, ShapeError):
                    raise ShapeError(f'{sources} (module {module} for '
                                     + ', '.join(sorted(commands[module])) + ')')
                if sources is None:
                    raise ShapeError(f'{TESTS_CMAKE}: no selected module {module} for '
                                     + ', '.join(sorted(commands[module])))
                owners[module] = sources
        expected = set()
        missing = []
        for target, sources in sorted(owners.items()):
            if not (names := commands.get(target)):
                continue
            for row in sorted(names - registered.keys() - expected):
                missing.append(sources[0] if row == target else f'{sources[0]}: {row}')
            expected.update(names)
        if missing:
            findings.append(
                f'{TESTS_CMAKE}: {len(missing)} kernel-profile test row(s) '
                f'that the kernel build did not register '
                f'({", ".join(missing[:6])}{", …" if len(missing) > 6 else ""}); '
                'this file has drifted from tests/CMakeLists.txt')
        # A listed row can name a compiled TU directly or run it under an alias.
        # A known script command is not a TU, even if a same-named file exists.
        def compiled_target(name):
            return (name in targets or name in members or name in modules or
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
        compiled = {name for name in registered.keys() - expected
                    if name in native_rows or (name not in named and compiled_target(name))}
        wrong = sorted((compiled & members) - targets.keys())
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

    disabled_sources: dict[str, set[str]] = {}
    if registered is not None:
        for source, names in kernel.items():
            listed = names & registered.keys()
            if listed and not any(registered[name] for name in listed):
                disabled_sources[source] = listed

    for row in rows:
        for name in row.tests:
            if not (root / 'tests' / name).is_file():
                findings.append(f'{DESIGN}:{row.line}: row {row.id} cites '
                                f'tests/{name}, which is not in the tree')
        if not kernel:
            continue
        witnesses = sorted(name for name in row.tests
                           if name in kernel and name not in disabled_sources)
        if row.verdict in ('no', 'partial') and witnesses and not row.marked:
            findings.append(
                f'{DESIGN}:{row.line}: row {row.id} reads **{row.verdict}** while '
                f'{", ".join("tests/" + name for name in witnesses)} runs are registered '
                'for the kernel profile. Execution is not checked. Append the closure '
                'marker the table already uses ("**Closed:** <what landed> (`tests/…`)").')
        if row.marked and row.verdict in ('no', 'partial') and not witnesses:
            named = (', '.join('tests/' + name for name in row.tests)
                     if row.tests else 'no test at all')
            disabled = sorted({name for source in row.tests
                               for name in disabled_sources.get(source, ())})
            reason = (f'no enabled kernel-profile witness (disabled CTest rows: {", ".join(disabled)})'
                      if disabled else 'no kernel-profile test')
            findings.append(
                f'{DESIGN}:{row.line}: row {row.id} carries a closure marker but '
                f'names {named} — {reason}. A closure marker must name a source-free witness.')
    summary = {
        'rows': len(rows),
        'marked': sum(1 for row in rows if row.marked),
        'kernelTests': len(kernel),
        'findings': len(findings),
        'missingUnitSources': missing_unit_sources,
    }
    return findings, summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="The design doc's §1 inventory against the kernel-profile tests.")
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--ctest-list', type=Path, default=None,
                        help='output of `ctest -N` from a kernel build dir, to '
                             'check registrations and exclude disabled rows as witnesses')
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
    if summary['missingUnitSources']:
        print('CTest registry: NOT COMPARED (missing unit sources)')
    elif listing is None:
        print('CTest registry: NOT COMPARED (no --ctest-list supplied)')
    return 1 if findings else 0


if __name__ == '__main__':
    raise SystemExit(main())
