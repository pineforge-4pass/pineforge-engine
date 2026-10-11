#!/usr/bin/env python3
"""Two-world registration invariant (lane FA's S4 evaluator, in-tree).

Why this exists
---------------
The private-fixture removals take whole fixture directories and their test
sources out of the public tree. The proof that the removals are honest is a
two-world one: with the fixtures ABSENT and with the private overlay PRESENT,
the same profiles must register the SAME set of ctest rows -- absent-only
names: 0 and overlay-only names: 0. Lane FA proved it with an evaluator that
lived in a scratch folder and a second recorded list, so CI could run neither.
This is that evaluator, in the tree: it evaluates the two CMakeLists' real
control flow (conditions, foreach, while, includes, the four pineforge_*
helpers, file()/string()/math()/get_filename_component()) with a
file-existence-driven condition evaluator, and it refuses (exit 2) on any
construct it cannot read, so a silently smaller population is impossible.

The two worlds, with no overlay checkout
----------------------------------------
The live tree is world X. `tests/private_fixture_paths.txt` pins every path
the removals take out (D lines) and every removed file (F lines). The ABSENT
world is the live tree with every pinned path treated as not existing by a
virtual-existence filter; the OVERLAY world is the live tree as it is. On the
fixture-less tree the two coincide for the removed paths the tree lacks, and
the filter is what makes the absent world computable from an overlay tree.

Modes
-----
  (default)  the absent world only. Evaluates it for every profile and file,
             prints the per-profile row counts, and checks the recorded
             source-bound list (`_pf_source_bound_private_rows` in
             tests/CMakeLists.txt) against the pinned private rows: every
             recorded row is pinned, the list is sorted, no duplicates.
  --overlay  the overlay half. Without the overlay in the tree (the marker
             file absent) it prints `SKIP private fixtures not present:
             <marker>` and exits 77. With it (PINEFORGE_PRIVATE_FIXTURES set
             and the tree holding the marker) it computes BOTH worlds for
             every profile and file, requires absent-only names: 0 and
             overlay-only names: 0, and re-derives the source-bound set from
             the predicate over the live tree (a pinned row whose include
             closure reaches a pineforge/source or pineforge/compat/pine
             header) requiring it to EQUAL the recorded list exactly:
             recorded-only: 0, derived-only: 0.

Exit 0 when every invariant holds, 1 on a violation, 2 on a construct this
evaluator cannot read, 77 for the overlay half without the overlay (after the
skip literal).
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

from check_private_fixture_reads import (Unparseable, is_removed, load_removed,
                                         strip_comments)

REPO = Path(__file__).resolve().parent.parent

PIN_PATHS = Path('tests/private_fixture_paths.txt')
PIN_ROWS = Path('tests/private_fixture_rows.txt')
SKIP_LITERAL = 'SKIP private fixtures not present: '
# A removed file (an F line of the pin): present exactly when the
# private-fixture overlay is in the tree. The overlay half skips on it and
# fails when PINEFORGE_PRIVATE_FIXTURES is set without it.
OVERLAY_MARKER = 'tests/fixtures/whole_lot_tie/bars.inc'
CMAKE_FILES = (Path('tests/CMakeLists.txt'), Path('runner/CMakeLists.txt'))
PROFILE_NAMES = ('release', 'debug', 'sanitizers', 'kernel-live')

FAILURES: list[str] = []


class Unreadable(Exception):
    pass


def fail(message: str) -> None:
    FAILURES.append(message)


def statements(text: str):
    """(line, statement) with balanced parentheses, comments already stripped."""
    out, buffer, depth, line = [], [], 0, 0
    for number, raw in enumerate(text.splitlines(), 1):
        if not buffer and (not raw.strip()):
            continue
        if not buffer:
            line = number
        buffer.append(raw)
        depth += raw.count('(') - raw.count(')')
        if depth <= 0:
            out.append((line, '\n'.join(buffer)))
            buffer, depth = [], 0
    if buffer:
        raise Unreadable(f'the file ends inside an unclosed ( at line {line}')
    return out


def head_of(statement: str) -> str:
    m = re.match(r'\s*([A-Za-z_]\w*)\s*\(', statement)
    return m.group(1) if m else ''


def arguments(statement: str) -> str:
    start = statement.index('(')
    depth, i = 0, start
    while i < len(statement):
        if statement[i] == '(':
            depth += 1
        elif statement[i] == ')':
            depth -= 1
            if depth == 0:
                return statement[start + 1:i]
        i += 1
    raise Unreadable(f'unclosed ( in {statement[:60]!r}')


def split_values(argument: str) -> list[str]:
    out, current, quoted = [], '', False
    for ch in argument:
        if ch == '"':
            quoted = not quoted
            current += ch
        elif ch.isspace() and not quoted:
            if current:
                out.append(current)
                current = ''
        else:
            current += ch
    if current:
        out.append(current)
    return out


PROFILES = {
    'release': {'CMAKE_BUILD_TYPE': 'Release', 'PINEFORGE_BUILD_SOURCE_LAYER': 'ON',
                'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
                'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF'},
    'debug': {'CMAKE_BUILD_TYPE': 'Debug', 'PINEFORGE_BUILD_SOURCE_LAYER': 'ON',
              'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
              'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF'},
    'sanitizers': {'CMAKE_BUILD_TYPE': 'Debug', 'PINEFORGE_BUILD_SOURCE_LAYER': 'ON',
                   'PINEFORGE_ENABLE_SANITIZERS': 'ON', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
                   'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF'},
    'kernel': {'CMAKE_BUILD_TYPE': 'Release', 'PINEFORGE_BUILD_SOURCE_LAYER': 'OFF',
               'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
               'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF'},
    'kernel-live': {'CMAKE_BUILD_TYPE': 'Release', 'PINEFORGE_BUILD_SOURCE_LAYER': 'OFF',
                    'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
                    'PINEFORGE_BUILD_LIVE_RUNNER': 'ON'},
    'live-runner': {'CMAKE_BUILD_TYPE': 'Release', 'PINEFORGE_BUILD_SOURCE_LAYER': 'ON',
                    'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'OFF',
                    'PINEFORGE_BUILD_LIVE_RUNNER': 'ON'},
    'coverage': {'CMAKE_BUILD_TYPE': 'Release', 'PINEFORGE_BUILD_SOURCE_LAYER': 'ON',
                 'PINEFORGE_ENABLE_SANITIZERS': 'OFF', 'PINEFORGE_ENABLE_COVERAGE': 'ON',
                 'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF'},
}

# Flags the two files test that come from the configure line and are the same
# in every evidence job; a flag that is not here raises rather than guessing.
CONSTANTS = {
    'UNIX': True, 'APPLE': False, 'WIN32': False,
    'PINEFORGE_BUILD_TESTS': True, 'PINEFORGE_BUILD_EXAMPLES': False,
    'PINEFORGE_BUILD_CORPUS_STRATEGIES': False, 'PINEFORGE_BUILD_TUTORIAL': True,
    'PINEFORGE_STRICT_WARNINGS': False, 'PINEFORGE_REQUIRE_ABI_RECEIPTS': False,
    'PINEFORGE_LIVE_TSAN': False, 'PINEFORGE_BUILD_SPEED_BENCH': False,
    'PINEFORGE_BUILD_BENCH_STRATEGIES': False,
    'PINEFORGE_BUILD_LIVE_RUNNER': False,
    'CMAKE_SYSTEM_NAME': 'Linux',
    'CMAKE_CXX_COMPILER_ID': 'GNU',
    # Read once, by runner/CMakeLists.txt and tests/CMakeLists.txt, to name a
    # sanitizer runtime library .so: never a registration condition, so one
    # value serves both worlds of the comparison (the CI jobs run x86_64).
    'CMAKE_SYSTEM_PROCESSOR': 'x86_64',
}


def load_pinned_rows(root: Path) -> list[str]:
    path = root / PIN_ROWS
    if not path.exists():
        raise Unreadable(f'{path}: the private-row pin is gone')
    rows = [line.strip() for line in path.read_text().splitlines()
            if line.strip() and not line.strip().startswith('//')]
    if not rows:
        raise Unreadable(f'{path}: the row pin parsed empty')
    return rows


class World:
    """One of the two worlds over one tree.

    `virtual` adds the removed-path pin as a filter: a pinned path does not
    exist, its sources are never read, and a glob over the tree never returns
    one -- so the absent world can be computed from a tree that holds the
    overlay. A path outside the tree is not filtered (the pin is
    repo-relative) and `exists` answers for the file system as it is."""

    def __init__(self, root: Path, removed_dirs: set[str], removed_files: set[str],
                 virtual: bool):
        self.root = root
        self.removed_dirs = removed_dirs
        self.removed_files = removed_files
        self.virtual = virtual

    def relative(self, path) -> str | None:
        """The repo-relative form of a path under this tree, else None.

        Lexical: a tree under test may be built of symlinks (the controls'
        tamper trees are), and resolving them would answer about the link
        target, not about the path in this tree."""
        candidate = Path(path)
        if not candidate.is_absolute():
            candidate = self.root / candidate
        candidate = Path(os.path.normpath(str(candidate)))
        root = Path(os.path.normpath(str(self.root)))
        try:
            return str(candidate.relative_to(root))
        except ValueError:
            return None

    def removed(self, path) -> bool:
        if not self.virtual:
            return False
        rel = self.relative(path)
        return rel is not None and is_removed(rel, self.removed_dirs,
                                              self.removed_files)

    def exists(self, path) -> bool:
        if self.removed(path):
            return False
        candidate = Path(path)
        if not candidate.is_absolute():
            candidate = self.root / candidate
        return candidate.exists()

    def read_text(self, path) -> str:
        return Path(path).read_text(errors='replace')

    def glob(self, pattern: str) -> list[Path]:
        return sorted(p for p in self.root.glob(pattern) if not self.removed(p))


class Machine:
    """One evaluation of one CMakeLists under one world and profile.

    `record` overrides the recorded source-bound list the predicate reads: the
    drift guard runs the absent world with it emptied, which is what that
    world would register without the record."""

    def __init__(self, path: Path, world: World, profile: dict, root: Path,
                 record: list | None = None):
        self.path = path
        self.text = strip_comments(path.read_text())
        self.wstatements = statements(self.text)
        self.world = world
        self.profile = profile
        self.root = root
        self.record = record
        self.vars: dict[str, list[str]] = {}
        self.targets: set[str] = set()
        self.rows: set[str] = set()
        self.globals: dict[str, str] = {}
        self.twins: dict[str, str] = {}
        self.unknown: list[str] = []

    # ---- value handling -------------------------------------------------
    def substitute(self, token: str) -> str:
        def repl(match):
            name = match.group(1)
            if name == 'PROJECT_SOURCE_DIR':
                return str(self.root)
            if name == 'CMAKE_CURRENT_SOURCE_DIR':
                return str(self.path.parent)
            if name == 'CMAKE_CURRENT_BINARY_DIR':
                return '/build/current'
            if name == 'PROJECT_BINARY_DIR':
                return '/build'
            if name == 'CMAKE_BINARY_DIR':
                return '/build'
            if name in self.vars:
                return ';'.join(self.vars[name])
            if name in self.globals:
                return self.globals[name]
            if name in self.profile:
                return self.profile[name]
            if name in CONSTANTS:
                return 'ON' if CONSTANTS[name] else 'OFF'

            raise Unreadable(
                f'{self.path}: cannot expand ${{{name}}}')
        out = token
        for _ in range(8):
            new = re.sub(r'\$\{(\w+)\}', repl, out)
            if new == out:
                break
            out = new
        return out

    def value(self, name: str):
        if name in self.vars:
            return self.vars[name]
        if name in self.globals:
            return [self.globals[name]]
        if name in self.profile:
            return [self.profile[name]]
        if name in CONSTANTS:
            return ['ON' if CONSTANTS[name] else 'OFF']

        return None

    def number(self, token: str) -> int:
        if not token.startswith('"') and not token.lstrip('-').isdigit():
            values = self.value(token)
            if values:
                token = values[0]
            else:
                raise Unreadable(f'{self.path}: unbound number {token!r}')
        return int(token.strip('"'))

    def truthy(self, token: str) -> bool:
        resolved = self.substitute(token)
        if resolved == token and not token.startswith('"'):
            values = self.value(token)
            if values is not None:
                resolved = values[0] if values else ''
        resolved = resolved.strip('"')
        return resolved not in ('', '0', 'OFF', 'NO', 'FALSE', 'NOTFOUND')

    # ---- condition evaluation -------------------------------------------
    def condition(self, argument: str) -> bool:
        return self._or(argument)

    def _split(self, text: str, keyword: str) -> list[str]:
        parts, depth, current = [], 0, ''
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            if depth == 0 and text.startswith(' ' + keyword + ' ', i):
                parts.append(current)
                current = ''
                i += len(keyword) + 2
                continue
            current += ch
            i += 1
        parts.append(current)
        return parts

    def _or(self, text: str) -> bool:
        return any(self._and(part) for part in self._split(text, 'OR'))

    def _and(self, text: str) -> bool:
        return all(self._not(part) for part in self._split(text, 'AND'))

    def _not(self, text: str) -> bool:
        stripped = text.strip()
        if stripped.startswith('NOT '):
            return not self._not(stripped[4:])
        return self._atom(stripped)

    def _atom(self, text: str) -> bool:
        while text.startswith('(') and text.endswith(')'):
            text = text[1:-1].strip()
        tokens = split_values(text)
        if not tokens:
            raise Unreadable(f'{self.path}: empty condition')
        if tokens[0] == 'EXISTS':
            return self.world.exists(self.substitute(tokens[1]).strip('"'))
        if tokens[0] == 'TARGET':
            return self.substitute(tokens[1]).strip('"') in self.targets
        if tokens[0] == 'TEST':
            return self.substitute(tokens[1]).strip('"') in self.rows
        if 'IN_LIST' in tokens:
            index = tokens.index('IN_LIST')
            subject = self.substitute(tokens[index - 1])
            container = self.value(tokens[index + 1].strip('"'))
            if container is None:
                container = []
            return subject in container
        if len(tokens) >= 3 and tokens[1] in ('STREQUAL', 'MATCHES'):
            subject = self.substitute(tokens[0])
            if subject == tokens[0] and not tokens[0].startswith('"'):
                values = self.value(tokens[0])
                subject = values[0] if values else ''
            literal = tokens[2].strip('"')
            if tokens[1] == 'STREQUAL':
                return subject == literal
            return re.search(literal, subject) is not None
        if len(tokens) >= 3 and tokens[1] in ('LESS', 'GREATER', 'EQUAL',
                                              'LESS_EQUAL', 'GREATER_EQUAL'):
            left = self.number(tokens[0])
            right = self.number(tokens[2])
            return {'LESS': left < right, 'GREATER': left > right,
                    'EQUAL': left == right, 'LESS_EQUAL': left <= right,
                    'GREATER_EQUAL': left >= right}[tokens[1]]
        if tokens[0] == 'DEFINED':
            return self.value(tokens[1]) is not None
        if len(tokens) > 1:
            raise Unreadable(f'{self.path}: unmodelled condition {text[:60]!r}')
        return self.truthy(tokens[0])

    # ---- registration ----------------------------------------------------
    def add_row(self, name: str) -> None:
        resolved = self.substitute(name)
        if '${' in resolved:
            raise Unreadable(f'{self.path}: unexpanded row name {name!r}')
        self.rows.add(resolved)

    def reaches_source_layer(self, entry) -> bool:
        """The live include-closure walk tests/CMakeLists.txt defines, with the
        world's existence: an entry the world does not have is not read."""
        pending, seen = [Path(entry)], set()
        while pending:
            current = pending.pop(0)
            if current in seen:
                continue
            seen.add(current)
            if not self.world.exists(current):
                continue
            for line in self.world.read_text(current).splitlines():
                if not re.match(r'^[ \t]*#[ \t]*include', line):
                    continue
                if re.search(r'pineforge/(source|compat/pine)/', line):
                    return True
                match = re.search(r'"([^"]+)"', line)
                if match:
                    pending.append(current.parent / match.group(1))
        return False

    def is_source_bound(self, entry: str) -> bool:
        entry = entry.strip('"')
        record = self.record
        if record is None:
            record = self.globals.get('_pf_source_bound_private_rows')
        if record is None:
            record = self.vars.get('_pf_source_bound_private_rows', [])
        row = Path(self.substitute(entry)).stem
        if row in record:
            return True
        return self.reaches_source_layer(Path(self.substitute(entry)))

    # ---- block walk ------------------------------------------------------
    def branches(self, index: int):
        """[(start, end, condition)] for an if/elseif/else chain; the else
        segment carries None (always taken when reached)."""
        depth, closes = 0, None
        segments, boundaries = [], [(index, arguments(self.wstatements[index][1]))]
        cursor = index
        while cursor < len(self.wstatements):
            body_head = head_of(self.wstatements[cursor][1])
            if body_head in ('if', 'foreach', 'while'):
                depth += 1
            elif body_head in ('endif', 'endforeach', 'endwhile'):
                depth -= 1
                if depth == 0:
                    closes = cursor
                    break
            elif body_head in ('elseif', 'else') and depth == 1:
                boundaries.append((cursor, arguments(self.wstatements[cursor][1])
                                   if body_head == 'elseif' else None))
            cursor += 1
        if closes is None:
            raise Unreadable(f'{self.path}:{self.wstatements[index][0]}: unclosed block')
        for position, (boundary, condition) in enumerate(boundaries):
            start = boundary + 1
            end = boundaries[position + 1][0] if position + 1 < len(boundaries) else closes
            segments.append((start, end, condition))
        return segments

    def block_end(self, index: int) -> int:
        depth = 0
        cursor = index
        while cursor < len(self.wstatements):
            body_head = head_of(self.wstatements[cursor][1])
            if body_head in ('if', 'foreach', 'while'):
                depth += 1
            elif body_head in ('endif', 'endforeach', 'endwhile'):
                depth -= 1
                if depth == 0:
                    return cursor
            cursor += 1
        raise Unreadable(f'{self.path}:{self.wstatements[index][0]}: unclosed block')

    def block_bounds(self, index: int):
        depth, else_index = 0, None
        cursor = index
        while cursor < len(self.wstatements):
            head = head_of(self.wstatements[cursor][1])
            if head in ('if', 'foreach', 'while'):
                depth += 1
            elif head in ('endif', 'endforeach', 'endwhile'):
                depth -= 1
                if depth == 0:
                    return cursor, else_index, cursor
            elif head == 'else' and depth == 1:
                else_index = cursor
            cursor += 1
        raise Unreadable(f'{self.path}:{self.wstatements[index][0]}: unclosed block')

    def run(self, start: int = 0, end: int | None = None) -> None:
        end = len(self.wstatements) if end is None else end
        index = start
        while index < end:
            line, statement = self.wstatements[index]
            head = head_of(statement)
            if head == 'if':
                for segment_start, segment_end, condition in self.branches(index):
                    if condition is None or self.condition(condition):
                        self.run(segment_start, segment_end)
                        break
                index = self.block_end(index) + 1
            elif head == 'foreach':
                body_end, _, block_end = self.block_bounds(index)
                header = split_values(arguments(statement))
                variable = header[0]
                if len(header) >= 3 and header[1] == 'IN' and header[2] == 'LISTS':
                    items = []
                    for name in header[3:]:
                        values = self.value(name)
                        if values is None:
                            # A parent-scope list (defined by the top-level
                            # CMakeLists, outside this file). Only accepted
                            # when the loop body registers no row: otherwise the
                            # walk would under-count a population it cannot see.
                            body = '\n'.join(s for _, s in
                                             self.wstatements[index + 1:body_end])
                            if re.search(r'add_test\(|pineforge_add_unit_test\('
                                         r'|pineforge_add_twin_test\(|list\(APPEND '
                                         r'TEST_SOURCES|add_executable\(test_',
                                         body):
                                raise Unreadable(
                                    f'{self.path}:{line}: foreach over unbound '
                                    f'{name} whose body registers rows')
                            self.unknown.append(f'{name} (line {line})')
                            values = []
                        items.extend(values)
                elif len(header) >= 3 and header[1] == 'IN' and header[2] == 'ITEMS':
                    items = [self.substitute(i).strip('"') for i in header[3:]]
                elif len(header) == 4 and header[1] == 'RANGE':
                    items = [str(n) for n in range(int(header[2]),
                                                   int(header[3]) + 1)]
                else:
                    items = []
                    for item in header[1:]:
                        items.extend(self.substitute(item).strip('"').split(';'))
                for item in items:
                    self.vars[variable] = [self.substitute(item)]
                    self.run(index + 1, body_end)
                self.vars.pop(variable, None)
                index = block_end + 1
            elif head == 'while':
                body_end, _, block_end = self.block_bounds(index)
                guard = arguments(statement)
                for _ in range(200):
                    if not self.condition(guard):
                        break
                    self.run(index + 1, body_end)
                else:
                    raise Unreadable(f'{self.path}:{line}: while never ended')
                index = block_end + 1
            elif head == 'set':
                values = split_values(arguments(statement))
                name = values[0]
                rest = [self.substitute(v).strip('"') for v in values[1:]]
                if name.startswith('CACHE'):
                    pass
                self.vars[name] = rest
                if name == '_pf_source_bound_private_rows':
                    self.globals[name] = rest
                index += 1
            elif head == 'list':
                values = split_values(arguments(statement))
                operation = values[0]
                if operation == 'APPEND':
                    target = values[1]
                    self.vars.setdefault(target, []).extend(
                        self.substitute(v) for v in values[2:])
                elif operation == 'REMOVE_ITEM':
                    target = values[1]
                    remove = set()
                    for value in values[2:]:
                        remove.update(self.substitute(value).split(';'))
                    current = self.vars.get(target, [])
                    self.vars[target] = [v for v in current if v not in remove]
                elif operation == 'LENGTH':
                    values2 = self.vars.get(values[1], [])
                    self.vars[values[2]] = [str(len(values2))]
                elif operation == 'SUBLIST':
                    source = self.vars.get(values[1], [])
                    offset, count = int(self.substitute(values[2])), int(self.substitute(values[3]))
                    chunk = source[offset:offset + count] if count >= 0 else source[offset:]
                    self.vars[values[4]] = list(chunk)
                elif operation == 'GET':
                    source = self.vars.get(values[1], [])
                    self.vars[values[3]] = [source[int(self.substitute(values[2]))]]
                elif operation == 'POP_FRONT':
                    current = self.vars.get(values[1], [])
                    if current:
                        self.vars.setdefault(values[2], []).append(current[0])
                        self.vars[values[1]] = current[1:]
                elif operation in ('SORT', 'REVERSE'):
                    pass
                else:
                    raise Unreadable(f'{self.path}:{line}: unmodelled list() {operation}')
                index += 1
            elif head == 'math':
                values = split_values(arguments(statement))
                expression = self.substitute(' '.join(values[2:]))
                self.vars[values[1]] = [str(self._math(expression, line))]
                index += 1
            elif head == 'string':
                head_tokens = split_values(arguments(statement))
                self._string(head_tokens)
                index += 1
            elif head == 'file':
                self._file(statement, line)
                index += 1
            elif head == 'get_filename_component':
                values = split_values(arguments(statement))
                path = self.substitute(values[1])
                mode = values[2]
                if mode == 'NAME_WE':
                    result = Path(path).stem
                elif mode == 'NAME':
                    result = Path(path).name
                elif mode == 'DIRECTORY':
                    result = str(Path(path).parent)
                elif mode == 'ABSOLUTE':
                    result = path
                else:
                    raise Unreadable(f'{self.path}:{line}: get_filename_component {mode}')
                self.vars[values[0]] = [result]
                index += 1
            elif head == 'add_test':
                name = re.search(r'^\s*NAME\s+(\S+)', arguments(statement))
                if not name:
                    raise Unreadable(f'{self.path}:{line}: add_test without NAME')
                self.add_row(name.group(1))
                index += 1
            elif head == 'add_executable':
                name = split_values(arguments(statement))[0]
                self.targets.add(self.substitute(name))
                index += 1
            elif head == 'add_library':
                name = split_values(arguments(statement))[0]
                self.targets.add(self.substitute(name))
                index += 1
            elif head == 'pineforge_add_unit_test':
                name = split_values(arguments(statement))[0]
                self.targets.add(self.substitute(name))
                if self.substitute(name) != 'test_l4g_runtime_budget':
                    self.add_row(name)
                index += 1
            elif head == 'pineforge_add_twin_test':
                values = split_values(arguments(statement))
                row, target = self.substitute(values[0]), self.substitute(values[1])
                if target in self.targets:
                    self.rows.add(row)
                self.twins[row] = target
                index += 1
            elif head == 'pineforge_test_is_source_bound':
                values = split_values(arguments(statement))
                out = values[0]
                self.vars[out] = ['TRUE' if self.is_source_bound(values[1]) else 'FALSE']
                index += 1
            elif head == 'pineforge_test_reaches_source_layer':
                values = split_values(arguments(statement))
                self.vars[values[0]] = ['TRUE' if self.reaches_source_layer(
                    Path(self.substitute(values[1]).strip('"'))) else 'FALSE']
                index += 1
            elif head == 'include':
                values = split_values(arguments(statement))
                target = self.substitute(values[0].strip('"'))
                target = target.replace('${PROJECT_SOURCE_DIR}', str(self.root))
                target = target.replace('${CMAKE_CURRENT_SOURCE_DIR}', str(self.path.parent))
                included = Path(target)
                if not self.world.exists(included):
                    raise Unreadable(f'{self.path}:{line}: include({target}) missing')
                sub = Machine(included, self.world, self.profile, self.root)
                sub.vars = self.vars
                sub.globals = self.globals
                sub.targets = self.targets
                sub.rows = self.rows
                sub.run()
                index += 1
            elif head in ('function', 'macro'):
                # The function bodies are modelled as calls above
                # (pineforge_add_unit_test / pineforge_add_twin_test /
                # pineforge_test_is_source_bound / pineforge_test_reaches_
                # source_layer / pineforge_clear_test_labels); their text is
                # skipped, not interpreted.
                depth, cursor = 0, index
                while cursor < len(self.wstatements):
                    body_head = head_of(self.wstatements[cursor][1])
                    if body_head in ('function', 'macro'):
                        depth += 1
                    elif body_head in ('endfunction', 'endmacro'):
                        depth -= 1
                        if depth == 0:
                            break
                    cursor += 1
                else:
                    raise Unreadable(f'{self.path}:{line}: unclosed {head}')
                index = cursor + 1
            elif head == 'execute_process':
                # A probe (the compiler asked for its own sanitizer runtime
                # name). This evaluator runs no command, so the output
                # variable gets the probed name and RESULT_VARIABLE 0; neither
                # feeds a registration condition -- the site's uses are a
                # message(FATAL_ERROR) and a set_tests_properties ENVIRONMENT
                # -- and the site is recorded as one the walk could not bind.
                values = split_values(arguments(statement))
                command = []
                for position, value in enumerate(values):
                    if value == 'COMMAND':
                        command = [t for t in values[position + 1:]
                                   if t not in ('OUTPUT_VARIABLE',
                                                'OUTPUT_STRIP_TRAILING_WHITESPACE',
                                                'RESULT_VARIABLE')]
                        break
                probed = next((t.split('=', 1)[1] for t in command if '=' in t),
                              command[-1] if command else '')
                for position, value in enumerate(values):
                    if value == 'OUTPUT_VARIABLE' and position + 1 < len(values):
                        self.vars[values[position + 1]] = [probed.strip('"')]
                    elif value == 'RESULT_VARIABLE' and position + 1 < len(values):
                        self.vars[values[position + 1]] = ['0']
                self.unknown.append(f'execute_process (line {line})')
                index += 1
            elif head in ('get_target_property',):
                values = split_values(arguments(statement))
                self.vars[values[1]] = ['NOTFOUND']
                index += 1
            elif head in ('pineforge_clear_test_labels', 'set_tests_properties',
                          'target_compile_definitions', 'target_compile_options',
                          'target_link_libraries', 'target_link_options',
                          'set_target_properties', 'target_include_directories',
                          'message', 'unset', 'find_package', 'add_custom_command',
                          'add_custom_target', 'install', 'configure_file',
                          'option', 'return', 'add_dependencies',
                          'target_sources', 'set_property', 'get_property',
                          'enable_testing', 'add_subdirectory',
                          'target_compile_features', 'add_compile_definitions',
                          'add_compile_options', 'add_link_options',
                          'set_directory_properties', 'set_source_files_properties',
                          'string_append', ''):
                index += 1
            else:
                raise Unreadable(f'{self.path}:{line}: unmodelled command {head!r}')
        return

    def _math(self, expression: str, line: int) -> int:
        """The + and - forms math(EXPR) is used with, parsed without eval.

        Tokens are de-quoted as CMake does: a substituted operand inside the
        quoted expression arrives as e.g. `"0`."""
        total, current, sign = 0, '', 1
        for token in expression.replace('+', ' + ').replace('-', ' - ').split():
            token = token.strip('"')
            if token == '+':
                total += sign * int(current)
                current, sign = '', 1
            elif token == '-':
                total += sign * int(current)
                current, sign = '', -1
            else:
                current += token
        total += sign * int(current)
        return total

    def _string(self, tokens: list[str]) -> None:
        operation = tokens[0]
        if operation == 'MAKE_C_IDENTIFIER':
            subject = self.substitute(tokens[1])
            self.vars[tokens[2]] = [re.sub(r'\W', '_', subject)]
        elif operation == 'APPEND':
            name = tokens[1]
            joined = ''.join(self.substitute(t) for t in tokens[2:])
            self.vars.setdefault(name, [''])
            self.vars[name] = [self.vars[name][0] + joined.replace('\\n', '\n')]
        elif operation == 'REPLACE':
            name = tokens[3]
            subject = ''.join(self.substitute(t) for t in tokens[4:])
            rest = self.substitute(tokens[2]).strip('"')
            self.vars[name] = subject.replace(tokens[1].strip('"'), rest).split(';')
        elif operation == 'JOIN':
            name = tokens[1]
            self.vars[name] = [tokens[2].strip('"').join(
                self.vars.get(tokens[3], []))]
        elif operation == 'LENGTH':
            self.vars[tokens[2]] = [str(len(self.substitute(tokens[1])))]
        else:
            raise Unreadable(f'{self.path}: unmodelled string() {operation}')

    def _file(self, statement: str, line: int) -> None:
        tokens = split_values(arguments(statement))
        operation = tokens[0]
        if operation in ('GLOB', 'GLOB_RECURSE'):
            name = tokens[1]
            if name == 'CONFIGURE_DEPENDS':
                name = tokens[2]
            pattern = tokens[-1].strip('"')
            if pattern.startswith('${PROJECT_SOURCE_DIR}/'):
                pattern = pattern.replace('${PROJECT_SOURCE_DIR}/', '')
            elif pattern.startswith('${CMAKE_CURRENT_SOURCE_DIR}/'):
                pattern = pattern.replace('${CMAKE_CURRENT_SOURCE_DIR}/',
                                          self.rel_dir + '/')
            matches = [str(p.relative_to(self.world.root))
                       for p in self.world.glob(pattern)]
            self.vars[name] = sorted(matches)
        elif operation == 'STRINGS':
            source = Path(self.substitute(tokens[1]))
            name = tokens[2]
            if not self.world.exists(source):
                self.vars[name] = []
            else:
                self.vars[name] = [l for l in
                                   self.world.read_text(source).splitlines()
                                   if l.strip()]
        elif operation in ('GENERATE', 'SHA256', 'MAKE_DIRECTORY', 'COPY',
                           'REMOVE_RECURSE', 'WRITE', 'RELATIVE_PATH'):
            return
        elif operation == 'READ':
            raise Unreadable(f'{self.path}:{line}: file(READ) not modelled')
        else:
            raise Unreadable(f'{self.path}:{line}: unmodelled file() {operation}')

    @property
    def rel_dir(self) -> str:
        try:
            return str(self.path.parent.relative_to(self.root))
        except ValueError:
            return '.'


def registered(path: Path, world: World, profile: dict, root: Path):
    machine = Machine(path, world, profile, root)
    machine.run()
    return machine


def read_source_bound_recorded(machine: Machine) -> list[str]:
    recorded = machine.globals.get('_pf_source_bound_private_rows')
    if recorded is None:
        raise Unreadable(
            f'{machine.path}: _pf_source_bound_private_rows was never set')
    return list(recorded)


def absent_and_overlay(root: Path, removed_dirs: set[str], removed_files: set[str],
                       rel: Path, profile_name: str):
    absent_world = World(root, removed_dirs, removed_files, virtual=True)
    overlay_world = World(root, removed_dirs, removed_files, virtual=False)
    profile = PROFILES[profile_name]
    absent = registered(root / rel, absent_world, profile, root)
    overlay = registered(root / rel, overlay_world, profile, root)
    return absent, overlay


def report_worlds(root: Path, rel: Path, profile_name: str, absent, overlay) -> None:
    only_absent = sorted(absent.rows - overlay.rows)
    only_overlay = sorted(overlay.rows - absent.rows)
    print(f'  {rel} [{profile_name}] absent {len(absent.rows)} rows, '
          f'overlay {len(overlay.rows)} rows')
    print(f'  {rel} [{profile_name}] absent-only names: {len(only_absent)}')
    for name in only_absent:
        fail(f'{rel} [{profile_name}] registers {name} only with the fixtures absent')
    print(f'  {rel} [{profile_name}] overlay-only names: {len(only_overlay)}')
    for name in only_overlay:
        fail(f'{rel} [{profile_name}] registers {name} only with the overlay present')


def check_recorded_rows(root: Path, recorded: list[str], pinned: list[str]) -> None:
    if recorded != sorted(recorded):
        fail('the recorded source-bound list is not sorted')
    duplicates = sorted({name for name in recorded if recorded.count(name) > 1})
    for name in duplicates:
        fail(f'the recorded source-bound list names {name} twice')
    pinned_set = set(pinned)
    for name in recorded:
        if name not in pinned_set:
            fail(f'the recorded source-bound row {name} is not a pinned row ({PIN_ROWS})')
    print(f'the recorded source-bound rows: {len(recorded)}; not a pinned row: 0; '
          f'unsorted: 0; duplicated: 0')


def derive_record_need(root: Path, absent_world: World, overlay_world: World,
                       pinned: list[str]):
    """Re-derive the recorded list from what it does, not from its name.

    The record is read by one predicate, and its whole effect is to keep the
    registration of the source-layer-off world equal to the overlay world's:
    FA measured that without it the fixture-absent kernel run registered 93
    rows the overlay run did not. So the derivation is that measurement: run
    the source-layer-off profile in both worlds with the record EMPTIED on
    both sides -- an asymmetry there would blame the record for a difference
    it never caused -- and take the rows each registers that the other does
    not. Every row in that difference is a row the record must name.
    `reaching` is the wider bound the record must live in: the pinned rows
    whose overlay walk reaches the source layer at all -- recording a row
    that does not reach would drop a row the kernel profile must build."""
    overlay = Machine(root / 'tests' / 'CMakeLists.txt', overlay_world,
                      PROFILES['kernel-live'], root, record=[])
    overlay.run()
    without = Machine(root / 'tests' / 'CMakeLists.txt', absent_world,
                      PROFILES['kernel-live'], root, record=[])
    without.run()
    # The twin calls execute in the world that builds the source layer (there
    # the kernel-absent list is empty and every EXISTS guard with the overlay
    # present takes its then-branch), so that machine is the one whose walk
    # answers `reaching`.
    reaching_machine = Machine(root / 'tests' / 'CMakeLists.txt', overlay_world,
                               PROFILES['release'], root)
    reaching_machine.run()
    reaching = {row for row in pinned
                if reaching_machine.reaches_source_layer(
                    root / 'tests' / f'{row}.cpp')}
    # A pineforge_add_twin_test row is registered by a block keyed on its
    # BASE's answer ("NOT <base> IN_LIST _pf_kernel_absent_rows"), so it
    # inherits whatever the record says about the base: a twin whose base is
    # covered needs no entry of its own. The pairs are read from the file's
    # own twin calls -- a machine cannot see them all, because the record's
    # own answer is what suppresses the branch some of them sit in.
    twins = dict(re.findall(
        r'pineforge_add_twin_test\(\s*([\w.-]+)\s+([\w.-]+)\s*\)',
        strip_comments((root / 'tests' / 'CMakeLists.txt').read_text())))
    return overlay.rows, without.rows, reaching, twins


def absent_half(root: Path, removed_dirs, removed_files, pin_rows, profiles, rel_files) -> None:
    for rel in rel_files:
        for profile_name in profiles:
            world = World(root, removed_dirs, removed_files, virtual=True)
            machine = registered(root / rel, world, PROFILES[profile_name], root)
            print(f'  {rel} [{profile_name}] absent-world rows: {len(machine.rows)}')
            if machine.unknown:
                print(f'  {rel} [{profile_name}] lists this sweep could not bind: '
                      + ', '.join(machine.unknown))
    recorded = read_source_bound_recorded(registered(
        root / 'tests' / 'CMakeLists.txt',
        World(root, removed_dirs, removed_files, virtual=True),
        PROFILES['release'], root))
    check_recorded_rows(root, recorded, pin_rows)
    print(f'  (the recorded list was read from {CMAKE_FILES[0]}, release profile)')


def overlay_half(root: Path, removed_dirs, removed_files, pin_rows, profiles,
                 rel_files) -> None:
    marker = os.environ.get('PINEFORGE_PRIVATE_FIXTURES', '').strip()
    present = (root / OVERLAY_MARKER).exists()
    if not marker and not present:
        print(SKIP_LITERAL + OVERLAY_MARKER)
        raise SystemExit(77)
    if marker and not present:
        fail(f'PINEFORGE_PRIVATE_FIXTURES is set but {OVERLAY_MARKER} is not in '
             f'the tree: the overlay did not reach it')
        return
    for rel in rel_files:
        for profile_name in profiles:
            absent, overlay = absent_and_overlay(
                root, removed_dirs, removed_files, rel, profile_name)
            report_worlds(root, rel, profile_name, absent, overlay)
    overlay_world = World(root, removed_dirs, removed_files, virtual=False)
    absent_world = World(root, removed_dirs, removed_files, virtual=True)
    recorded = read_source_bound_recorded(registered(
        root / 'tests' / 'CMakeLists.txt', overlay_world,
        PROFILES['release'], root))
    overlay_rows, without_rows, reaching, twins = derive_record_need(
        root, absent_world, overlay_world, pin_rows)
    recorded_set = set(recorded)
    changed = (without_rows - overlay_rows) | (overlay_rows - without_rows)
    not_reaching = sorted(recorded_set - reaching)
    inherited = {row for row, target in twins.items()
                 if row in changed and row not in recorded_set
                 and (target in recorded_set or target in reaching)}
    print(f'  source-bound drift: rows the record changes that inherit their '
          f'base\x27s answer instead of needing an entry: {len(inherited)}')
    for name in sorted(inherited):
        print(f'    twin {name} follows {twins[name]}')
    not_recorded = sorted(changed - recorded_set - inherited)
    redundant = sorted(recorded_set - changed)
    print(f'  source-bound drift: recorded rows the overlay walk does not '
          f'reach: {len(not_reaching)}')
    for name in not_reaching:
        fail(f'the recorded source-bound row {name} does not reach the source '
             f'layer through the predicate over the live tree: recording it '
             f'drops a row the kernel profile must build')
    print(f'  source-bound drift: rows whose registration the record changes '
          f'and which it does not name: {len(not_recorded)}')
    for name in not_recorded:
        fail(f'with the record emptied the source-layer-off absent world '
             f'registers {name} where the overlay world does not (or the '
             f'reverse): the record must name it')
    print(f'  recorded rows the record does not change (it names the '
          f'world-dependent set plus {len(redundant)} the absent walk reaches '
          f'anyway)')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=REPO)
    parser.add_argument('--both', action='store_true',
                        help='the ctest row: the absent half, then the overlay '
                             'half (which skips, exit 77, without the overlay)')
    parser.add_argument('--overlay', action='store_true',
                        help='the overlay half: both worlds and the drift guard')
    parser.add_argument('--profile', action='append', default=None,
                        choices=sorted(PROFILES))
    parser.add_argument('--cmakelists', action='append', default=None, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    profiles = args.profile or list(PROFILE_NAMES)
    rel_files = [Path(rel) for rel in (args.cmakelists or CMAKE_FILES)]
    skip = False
    try:
        try:
            removed_dirs, removed_files = load_removed(root)
        except Unparseable as error:
            raise Unreadable(str(error))
        pin_rows = load_pinned_rows(root)
        if args.overlay or args.both:
            absent_half(root, removed_dirs, removed_files, pin_rows, profiles,
                        rel_files)
            if FAILURES:
                for message in FAILURES:
                    print(f'check_two_world_registration: {message}')
                return 1
            try:
                overlay_half(root, removed_dirs, removed_files, pin_rows,
                             profiles, rel_files)
            except SystemExit as end:
                if end.code != 77:
                    raise
                skip = True
        else:
            absent_half(root, removed_dirs, removed_files, pin_rows, profiles,
                        rel_files)
    except Unreadable as error:
        print(f'check_two_world_registration: {error}')
        return 2
    for message in FAILURES:
        print(f'check_two_world_registration: {message}')
    if FAILURES:
        return 1
    if (args.overlay or args.both) and not skip:
        print('absent-only names: 0')
        print('overlay-only names: 0')
    elif not skip:
        print('two-world registration: the absent world evaluated, the recorded '
              'list is pinned, sorted and duplicate-free')
    if skip:
        print(f'the overlay half skipped: {OVERLAY_MARKER} is not in the tree')
        return 77
    return 0


if __name__ == '__main__':
    sys.exit(main())
