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

Arguments follow CMake's own rules (cmake-language(7), "Quoted Argument" /
"Unquoted Argument"): a quoted argument is exactly one argument with its
quotes stripped, an unquoted one divides like a list into elements and each
non-empty element is an argument. Both files lean on that rule -- the
refusal-shard split at runner/CMakeLists.txt passes a family list through
`set(refusal_libraries ${...})` unquoted and appends quoted library entries
-- so a list that keeps its quotes, or one that stays a single joined
string, evaluates a DIFFERENT population than CMake's.

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
             tests/CMakeLists.txt) and the recorded refusal-shard list
             (`_pf_refusal_shard_rows` in runner/CMakeLists.txt) against the
             pinned private rows: every recorded row is pinned, each list is
             sorted, no duplicates.
  --overlay  the overlay half. Without the overlay in the tree (the marker
             file absent and PINEFORGE_PRIVATE_FIXTURES unset) it prints
             `overlay half not run: private fixtures not present: <marker>`
             and exits 0: this script has no skip path. With it (the tree
             holding the marker) it computes BOTH worlds for every profile
             and file, requires absent-only names: 0 and overlay-only names:
             0, and re-derives the source-bound set from the predicate over
             the live tree (a pinned row whose include closure reaches a
             pineforge/source or pineforge/compat/pine header). Two bounds
             hold on it: every row the record names must be one the predicate
             reaches, and every row whose registration the record changes must
             be named by it (or inherit a recorded base) -- the printed
             recorded-only and not-recorded counts are both 0 then. The
             refusal-shard list is re-derived the same
             way: the present branch's split over the live tree (the
             refused_*.cpp glob, the family regex, the 8-library batches)
             must produce exactly the recorded rows -- recorded-only: 0,
             derived-only: 0.
  --both     the ctest row: the absent half always runs and decides the exit
             code, then the overlay half by the --overlay rule. With
             PINEFORGE_PRIVATE_FIXTURES set and the marker absent the row
             FAILS (an overlay run whose overlay half cannot run is a
             failure, never a skip), and with neither it exits 0 and prints
             the not-run line.

Exit 0 when every invariant holds, 1 on a violation (including an overlay
half that cannot run although PINEFORGE_PRIVATE_FIXTURES says the fixtures
are present), 2 on a construct this evaluator cannot read. No path exits 77:
the row is registered without a SKIP_RETURN_CODE because it cannot skip.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import re
import sys
from pathlib import Path

import ci_verify
from check_private_fixture_reads import (Unparseable, is_removed, load_removed,
                                         strip_comments)

REPO = Path(__file__).resolve().parent.parent

PIN_PATHS = Path('tests/private_fixture_paths.txt')
PIN_ROWS = Path('tests/private_fixture_rows.txt')
# A removed file (an F line of the pin): present exactly when the
# private-fixture overlay is in the tree. The overlay half skips on it and
# fails when PINEFORGE_PRIVATE_FIXTURES is set without it.
OVERLAY_MARKER = 'tests/fixtures/whole_lot_tie/bars.inc'
CMAKE_FILES = (Path('tests/CMakeLists.txt'), Path('runner/CMakeLists.txt'))
# The absent world cannot glob files that do not exist, so the refusal-shard
# rows are a RECORDED list: runner/CMakeLists.txt pins the names the present
# branch's split produces, and the drift guard requires the two to agree.
# if(<single argument>) constants (cmake-language(7), "if"): everything else
# that is neither a defined variable nor a non-zero number is false.
TRUE_CONSTANTS = frozenset(('1', 'ON', 'YES', 'TRUE', 'Y'))
FALSE_CONSTANTS = frozenset(('0', 'OFF', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND'))

REFUSAL_SHARD_PREFIX = 'confirmed_capabilities_refusals_'
REFUSAL_SHARD_RECORD = '_pf_refusal_shard_rows'
REFUSAL_SHARD_MARKERS = '_pf_refusal_shard_first_library'

FAILURES: list[str] = []
# absent-only/overlay-only names summed over every profile and file the
# overlay half compared; the closing lines print these, never a literal zero.
TOTALS = {'absent_only': 0, 'overlay_only': 0}


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


# The profile table is DERIVED from scripts/ci_verify.py's own PROFILES -- the
# configure line each evidence job runs -- never hand-copied: every ci_verify
# profile is compared, in both worlds, so a row registered only under
# SOURCE_LAYER AND LIVE_RUNNER (the native and live-* profiles) is compared
# too. A profile ci_verify names without an entry, or an entry missing a field
# this table needs, is a loud failure, not a guess.
_PROFILE_FIELDS = ('build_type', 'sanitizers', 'live_runner', 'tutorial',
                   'source_layer', 'thread_sanitizers')


def profile_table() -> dict:
    table = {}
    for name in ci_verify.PROFILES:
        entry = ci_verify.PROFILE.get(name)
        if entry is None:
            raise Unreadable(f'scripts/ci_verify.py names the profile {name!r} '
                             f'without an entry in its PROFILE table')
        missing = [field for field in _PROFILE_FIELDS
                   if not hasattr(entry, field)]
        if missing:
            raise Unreadable(f'scripts/ci_verify.py PROFILE[{name!r}] is '
                             f'missing the fields the two-world sweep needs: '
                             f'{missing}')
        table[name] = {
            'CMAKE_BUILD_TYPE': entry.build_type,
            'PINEFORGE_BUILD_SOURCE_LAYER':
                'ON' if entry.source_layer else 'OFF',
            'PINEFORGE_ENABLE_SANITIZERS':
                'ON' if entry.sanitizers else 'OFF',
            'PINEFORGE_BUILD_LIVE_RUNNER':
                'ON' if entry.live_runner else 'OFF',
            'PINEFORGE_LIVE_TSAN':
                'ON' if entry.thread_sanitizers else 'OFF',
            'PINEFORGE_BUILD_TUTORIAL':
                'ON' if entry.tutorial else 'OFF',
            # ci_verify's profiles never turn coverage on; the two files test
            # the flag, so it is stated here rather than left unbound.
            'PINEFORGE_ENABLE_COVERAGE': 'OFF',
        }
    return table


PROFILES = profile_table()
PROFILE_NAMES = tuple(ci_verify.PROFILES)

# Flags the two files test that come from the configure line and are the same
# in every evidence job; a flag that is not here raises rather than guessing.
# PINEFORGE_BUILD_LIVE_RUNNER, PINEFORGE_LIVE_TSAN and
# PINEFORGE_BUILD_TUTORIAL are NOT here: they differ per profile and every
# derived profile entry states them, so a missing one is a loud failure.
CONSTANTS = {
    'UNIX': True, 'APPLE': False, 'WIN32': False,
    'PINEFORGE_BUILD_TESTS': True, 'PINEFORGE_BUILD_EXAMPLES': False,
    'PINEFORGE_BUILD_CORPUS_STRATEGIES': False,
    'PINEFORGE_STRICT_WARNINGS': False, 'PINEFORGE_REQUIRE_ABI_RECEIPTS': False,
    'PINEFORGE_BUILD_SPEED_BENCH': False,
    'PINEFORGE_BUILD_BENCH_STRATEGIES': False,
    'CMAKE_SYSTEM_NAME': 'Linux',
    'CMAKE_CXX_COMPILER_ID': 'GNU',
    # The interpreter and the cmake binary name COMMAND elements of add_test()
    # and of the list(APPEND)ed command lists (never a registration
    # condition): stated absolute paths, since a name is all the row needs.
    'Python3_EXECUTABLE': '/usr/bin/python3',
    'CMAKE_COMMAND': '/usr/bin/cmake',
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


class Machine:
    """One evaluation of one CMakeLists under one world and profile.

    `record` overrides the recorded source-bound list the predicate reads: the
    drift guard runs the absent world with it emptied, which is what that
    world would register without the record."""

    def __init__(self, path: Path, world: World, profile: dict, root: Path,
                 record: list | None = None, source_dir: Path | None = None):
        self.path = path
        self.text = strip_comments(path.read_text())
        self.wstatements = statements(self.text)
        self.world = world
        self.profile = profile
        self.root = root
        self.record = record
        # CMAKE_CURRENT_SOURCE_DIR: the directory being processed, which
        # include() does NOT change (CMAKE_CURRENT_LIST_DIR is the one it
        # changes; the evaluator does not model that variable).
        self.source_dir = Path(source_dir) if source_dir is not None else path.parent
        self.vars: dict[str, list[str]] = {}
        self.targets: set[str] = set()
        self.rows: set[str] = set()
        self.globals: dict[str, str] = {}
        self.twins: dict[str, str] = {}
        # The sequence list(POP_FRONT <list> ...) removed from each list, in
        # order: the consumer of a popped list (the runner's marker-per-row
        # pairing) reads it even though the list itself ends up empty.
        self.popped: dict[str, list[str]] = {}

    # ---- value handling -------------------------------------------------
    def substitute(self, token: str) -> str:
        def repl(match):
            name = match.group(1)
            if name == 'PROJECT_SOURCE_DIR':
                return str(self.root)
            if name == 'CMAKE_CURRENT_SOURCE_DIR':
                return str(self.source_dir)
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
                f'{self.path}:{getattr(self, "line", 0)}: cannot expand '
                f'${{{name}}}')
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

    def elements(self, token: str) -> list[str]:
        """One argument's contribution to a list, by CMake's own rule.

        cmake-language(7): a quoted argument is always exactly one argument
        (quotes stripped); an unquoted argument's value "is divided in the
        same way Lists divide into elements", each NON-EMPTY element being an
        argument -- so an empty expansion contributes none. Both files feed
        lists through unquoted `${list}` expansions and quote single
        elements, so collapsing either case evaluates a different
        population than CMake's."""
        resolved = self.substitute(token)
        if token.startswith('"'):
            return [resolved.strip('"')]
        return [part for part in resolved.split(';') if part]

    def number(self, token: str) -> int:
        if not token.startswith('"') and not token.lstrip('-').isdigit():
            values = self.value(token)
            if values:
                token = values[0]
            else:
                raise Unreadable(f'{self.path}: unbound number {token!r}')
        return int(token.strip('"'))

    def truthy(self, token: str) -> bool:
        """if(<single argument>), CMake's own rules: a defined variable
        answers with its value; the named constants (and non-zero numbers)
        answer; anything else -- an undefined variable or an arbitrary
        string -- is FALSE. The old form read an undefined variable as true,
        which is not what a configure does."""
        resolved = self.substitute(token)
        if resolved == token and not token.startswith('"'):
            values = self.value(token)
            if values is not None:
                resolved = values[0] if values else ''
            else:
                stripped = token.strip('"')
                if stripped.upper() in TRUE_CONSTANTS:
                    return True
                if re.fullmatch(r'-?\d+', stripped):
                    return int(stripped) != 0
                return False
        resolved = resolved.strip('"')
        if resolved.upper() in FALSE_CONSTANTS:
            return False
        if resolved.upper().endswith('-NOTFOUND'):
            return False
        return resolved != ''

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
        # A parenthesised group may itself hold top-level boolean operators
        # ('(NOT TEST a OR NOT TARGET a)'). Once its parentheses are gone the
        # group is an ordinary sub-expression, so evaluate it with the same
        # precedence instead of refusing it as one unmodelled atom.
        if len(self._split(text, 'OR')) > 1 or len(self._split(text, 'AND')) > 1:
            return self._or(text)
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
            raw = tokens[0]
            expanded = self.substitute(raw)
            if expanded == raw and not raw.startswith('"'):
                values = self.value(raw)
                subject = values[0] if values else raw
            else:
                # a quoted operand is one argument with its quotes stripped
                subject = expanded.strip('"')
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

    def parent_list(self, name: str):
        """A parent-scope list read from the tree's own top-level CMakeLists.

        `foreach(... IN LISTS <name>)` over a variable neither CMakeLists
        sets is the one shape whose value lives in the parent scope. When the
        top-level file sets it as a literal `set(<name> ...)`, the sweep
        reads that literal -- the real list, not a stand-in. The first such
        set() in the file wins; a name the top-level file does not set
        literally is a construct this sweep cannot bind and the walk refuses
        it (exit 2) rather than running the loop over an empty list."""
        top = self.root / 'CMakeLists.txt'
        if not top.exists():
            return None
        for _line, statement in statements(strip_comments(
                top.read_text(errors='replace'))):
            if head_of(statement) != 'set':
                continue
            values = split_values(arguments(statement))
            if values and values[0] == name:
                return [element for token in values[1:]
                        for element in self.elements(token)]
        return None

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
            self.line = line
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
                            values = self.parent_list(name)
                        if values is None:
                            # A parent-scope list this sweep cannot read. The
                            # loop is NOT run over an empty stand-in: a
                            # construct the evaluator cannot bind exits 2
                            # rather than quietly evaluating a smaller world.
                            raise Unreadable(
                                f'{self.path}:{line}: foreach over unbound '
                                f'{name}: a parent-scope list that is not a '
                                f'literal set() in the top-level CMakeLists.txt')
                        items.extend(values)
                elif len(header) >= 3 and header[1] == 'IN' and header[2] == 'ITEMS':
                    items = [element for token in header[3:]
                             for element in self.elements(token)]
                elif len(header) >= 3 and header[1] == 'RANGE':
                    bounds = [int(self.substitute(t).strip('"'))
                              for t in header[2:]]
                    if len(bounds) == 1:
                        start, stop, step = 0, bounds[0], 1
                    elif len(bounds) == 2:
                        start, stop, step = bounds[0], bounds[1], 1
                    elif len(bounds) == 3:
                        start, stop, step = bounds
                    else:
                        raise Unreadable(f'{self.path}:{line}: foreach RANGE '
                                         f'with {len(bounds)} bounds')
                    items = [str(n) for n in range(start, stop + 1, step)]
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
                tail = values[1:]
                if 'CACHE' in tail:
                    # set(<var> <value>... CACHE <type> <doc> [FORCE]): the
                    # value part is what is set; the cache metadata is not
                    tail = tail[:tail.index('CACHE')]
                if 'PARENT_SCOPE' in tail:
                    # the parent scope of a directory-level file (or of an
                    # included one) is the same scope this walk holds
                    tail = [token for token in tail if token != 'PARENT_SCOPE']
                rest = [element for token in tail for element in self.elements(token)]
                self.vars[name] = rest
                if name == '_pf_source_bound_private_rows':
                    self.globals[name] = rest
                index += 1
            elif head == 'list':
                values = split_values(arguments(statement))
                operation = values[0]
                if operation == 'APPEND':
                    target = values[1]
                    appended = self.vars.setdefault(target, [])
                    for token in values[2:]:
                        appended.extend(self.elements(token))
                elif operation == 'REMOVE_ITEM':
                    target = values[1]
                    remove = set()
                    for token in values[2:]:
                        remove.update(self.elements(token))
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
                    # CMake SETS each out-var (it does not append), unsetting
                    # a surplus one; with no out-var exactly one element is
                    # removed. The popped sequence is recorded either way.
                    target = values[1]
                    current = list(self.vars.get(target, []))
                    out_vars = values[2:]
                    popped = current[:len(out_vars) if out_vars else 1]
                    self.popped.setdefault(target, []).extend(popped)
                    for position, out in enumerate(out_vars):
                        if position < len(popped):
                            self.vars[out] = [popped[position]]
                        else:
                            self.vars.pop(out, None)
                    self.vars[target] = current[len(popped):]
                elif operation in ('SORT', 'REVERSE'):
                    target = values[1]
                    current = list(self.vars.get(target, []))
                    if operation == 'REVERSE':
                        self.vars[target] = list(reversed(current))
                    else:
                        # lexicographic by default; the ORDER option is
                        # modelled, COMPARE and anything else is refused
                        order = 'ASCENDING'
                        position = 2
                        while position < len(values):
                            if (values[position] == 'ORDER'
                                    and position + 1 < len(values)):
                                order = values[position + 1]
                                position += 2
                                continue
                            raise Unreadable(
                                f'{self.path}:{line}: list(SORT '
                                f'{values[position]!r}) is not modelled')
                        if order not in ('ASCENDING', 'DESCENDING'):
                            raise Unreadable(
                                f'{self.path}:{line}: list(SORT ORDER {order}) '
                                f'is not modelled')
                        self.vars[target] = sorted(
                            current, reverse=order == 'DESCENDING')
                else:
                    raise Unreadable(f'{self.path}:{line}: unmodelled list() {operation}')
                index += 1
            elif head == 'math':
                values = split_values(arguments(statement))
                expression = self.substitute(' '.join(values[2:]))
                try:
                    result = self._math(expression, line)
                except (ValueError, IndexError):
                    raise Unreadable(
                        f'{self.path}:{line}: math(EXPR) in a form this sweep '
                        f'cannot read: {expression!r}')
                self.vars[values[1]] = [str(result)]
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
                paths = self.elements(values[1])
                if len(paths) != 1:
                    raise Unreadable(
                        f'{self.path}:{line}: get_filename_component got '
                        f'{len(paths)} path arguments')
                path = paths[0]
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
                # include() runs the file in the including scope: the shared
                # containers carry the walk's state, `source_dir` stays the
                # including file's (only CMAKE_CURRENT_LIST_DIR changes, which
                # this evaluator does not model), and the record override rides
                # along -- an included file must read the same record the
                # including walk was given.
                sub = Machine(included, self.world, self.profile, self.root,
                              record=self.record, source_dir=self.source_dir)
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
                # The ONE probed command the tree runs: the compiler asked for
                # its own sanitizer runtime file name. This sweep runs no
                # command -- it is text over a tree -- so the probe is BOUND
                # with a stated value under which the guard it feeds HOLDS, the
                # state a real configure runs under when the compiler ships the
                # runtime: RESULT_VARIABLE 0 and an existing path in the
                # OUTPUT_VARIABLE. The sweep cannot know the host's runtime
                # path, so it binds the output to the runner listfile of the
                # world -- a file the world has -- which keeps the site's
                # `NOT EXISTS` arm (and with it the message(FATAL_ERROR) that
                # would stop a real configure) from firing. Neither variable
                # can reach a registration condition: the site's uses are that
                # FATAL_ERROR and a set_tests_properties ENVIRONMENT. Any other
                # execute_process, and any probe shape other than the
                # compiler's -print-file-name, is a construct this sweep cannot
                # bind.
                values = split_values(arguments(statement))
                command = []
                for position, value in enumerate(values):
                    if value == 'COMMAND':
                        command = [t for t in values[position + 1:]
                                   if t not in ('OUTPUT_VARIABLE',
                                                'OUTPUT_STRIP_TRAILING_WHITESPACE',
                                                'RESULT_VARIABLE')]
                        break
                probed = next((t.split('=', 1)[1].rstrip('"')
                               for t in command
                               if t.startswith('"-print-file-name=')
                               or t.startswith('-print-file-name=')), None)
                if probed is None:
                    raise Unreadable(
                        f'{self.path}:{line}: execute_process in a shape this '
                        f'sweep does not bind (only the compiler\'s '
                        f'-print-file-name probe is, with a stated value)')
                bound_runtime = self.world.root / 'runner' / 'CMakeLists.txt'
                for position, value in enumerate(values):
                    if value == 'OUTPUT_VARIABLE' and position + 1 < len(values):
                        self.vars[values[position + 1]] = [str(bound_runtime)]
                    elif value == 'RESULT_VARIABLE' and position + 1 < len(values):
                        self.vars[values[position + 1]] = ['0']
                index += 1
            elif head == 'message':
                # message(STATUS ...)/plain forms print and continue; a reached
                # FATAL_ERROR or SEND_ERROR stops a real configure, and this
                # sweep does not model a stopped configure -- so reaching one
                # is exit 2 with the file and line, never a quiet line.
                values = split_values(arguments(statement))
                mode = values[0].strip('"') if values else ''
                if mode in ('FATAL_ERROR', 'SEND_ERROR'):
                    raise Unreadable(
                        f'{self.path}:{line}: message({mode}) was reached: a '
                        f'real configure stops here and this sweep does not '
                        f'model a stopped configure')
                index += 1
            elif head == 'unset':
                values = split_values(arguments(statement))
                for token in values:
                    if token == 'PARENT_SCOPE':
                        continue
                    if token.startswith('CACHE{'):
                        raise Unreadable(
                            f'{self.path}:{line}: unset(CACHE{{...}}) is not '
                            f'modelled')
                    self.vars.pop(token.strip('"'), None)
                index += 1
            elif head == 'return':
                # At function/macro scope the bodies are skipped as text, so
                # a reached return() is at file scope: CMake stops processing
                # the file there and this sweep does not model a stopped walk.
                raise Unreadable(
                    f'{self.path}:{line}: return() at file scope stops a real '
                    f'configure and this sweep does not model that')
            elif head == 'option':
                values = split_values(arguments(statement))
                if not values:
                    raise Unreadable(f'{self.path}:{line}: option() without a name')
                name = values[0]
                initial = 'OFF'
                if len(values) >= 3:
                    initial = values[2].strip('"')
                elif len(values) == 2 and not values[1].startswith('"'):
                    initial = values[1]
                # option() defines the variable only when it is not defined
                # already (a -D cache entry wins), so a profile value stays
                if self.value(name) is None:
                    self.vars[name] = [initial]
                index += 1
            elif head in ('get_target_property',):
                values = split_values(arguments(statement))
                self.vars[values[1]] = ['NOTFOUND']
                index += 1
            elif head in ('pineforge_clear_test_labels', 'set_tests_properties',
                          'target_compile_definitions', 'target_compile_options',
                          'target_link_libraries', 'target_link_options',
                          'set_target_properties', 'target_include_directories',
                          'find_package', 'add_custom_command',
                          'add_custom_target', 'install', 'configure_file',
                          'add_dependencies',
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
            # string(JOIN <glue> <out-var> <input>...): the glue is first and
            # the output variable second (the old order named the glue as the
            # output and read a variable where the inputs are)
            glue = self.substitute(tokens[1]).strip('"')
            name = tokens[2]
            values = [element for token in tokens[3:]
                      for element in self.elements(token)]
            self.vars[name] = [glue.join(values)]
        elif operation == 'LENGTH':
            self.vars[tokens[2]] = [str(len(self.substitute(tokens[1])))]
        else:
            raise Unreadable(f'{self.path}: unmodelled string() {operation}')

    def _file(self, statement: str, line: int) -> None:
        tokens = split_values(arguments(statement))
        operation = tokens[0]
        if operation in ('GLOB', 'GLOB_RECURSE'):
            self._glob(operation, tokens, line)
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

    def _glob(self, operation: str, tokens: list[str], line: int) -> None:
        """file(GLOB/GLOB_RECURSE), CMake's own shape (file(1) docs).

        Every globbing expression is matched -- the old model read only the
        last one. A GLOB_RECURSE expression recurses into the subdirectories
        of the directory it names (CMP0009 NEW: a symlinked subdirectory is
        listed but not traversed, and FOLLOW_SYMLINKS -- which would traverse
        it -- is refused). LIST_DIRECTORIES and RELATIVE are modelled; a
        GLOB lists directories unless told otherwise while a GLOB_RECURSE
        omits them unless told otherwise; CONFIGURE_DEPENDS only affects
        build-system regeneration. The result is ordered lexicographically,
        as CMake documents for both operations."""
        name, position = None, 1
        list_directories = operation == 'GLOB'
        relative = None
        while position < len(tokens):
            token = tokens[position]
            if token == 'CONFIGURE_DEPENDS':
                position += 1
            elif token == 'LIST_DIRECTORIES':
                if position + 1 >= len(tokens):
                    raise Unreadable(f'{self.path}:{line}: file({operation}) '
                                     f'LIST_DIRECTORIES without a value')
                value = tokens[position + 1].strip('"').lower()
                if value not in ('true', 'false'):
                    raise Unreadable(f'{self.path}:{line}: file({operation}) '
                                     f'LIST_DIRECTORIES {tokens[position + 1]}: '
                                     f'expected true or false')
                list_directories = value == 'true'
                position += 2
            elif token == 'RELATIVE':
                if position + 1 >= len(tokens):
                    raise Unreadable(f'{self.path}:{line}: file({operation}) '
                                     f'RELATIVE without a path')
                relative = tokens[position + 1]
                position += 2
            elif token == 'FOLLOW_SYMLINKS':
                raise Unreadable(
                    f'{self.path}:{line}: file({operation}) FOLLOW_SYMLINKS is '
                    f'not modelled: it changes which subdirectories a '
                    f'recursive glob traverses')
            elif name is None:
                name = token
                position += 1
            else:
                break
        patterns = tokens[position:]
        if name is None or not patterns:
            raise Unreadable(f'{self.path}:{line}: file({operation}) without a '
                             f'variable and at least one globbing expression')
        matches: list[str] = []
        for pattern_token in patterns:
            pattern = self.substitute(pattern_token).strip('"')
            matches.extend(self._glob_matches(pattern, operation == 'GLOB_RECURSE',
                                              line))
        if not list_directories:
            matches = [m for m in matches
                       if (self.world.root / m).is_file()]
        if relative is not None:
            base = self._tree_relative(self.substitute(relative).strip('"'),
                                       line).rstrip('/')
            matches = [m[len(base) + 1:] if base and m.startswith(base + '/')
                       else m for m in matches]
        self.vars[name] = sorted(matches)

    def _glob_matches(self, pattern: str, recurse: bool, line: int) -> list[str]:
        """One globbing expression, tree-relative, over the world's files.

        A relative expression is relative to CMAKE_CURRENT_SOURCE_DIR; an
        expression whose directory part carries a wildcard is a construct this
        sweep does not model (none in the tree). Returns root-relative paths
        with the world's removed paths filtered, like every other read."""
        if not pattern:
            raise Unreadable(f'{self.path}:{line}: an empty globbing expression')
        head, _, tail = pattern.rpartition('/')
        base = head or '.'
        if not os.path.isabs(base):
            base = os.path.join(str(self.source_dir), base)
        base = os.path.normpath(base)
        for component in Path(base).parts:
            if any(ch in component for ch in '*?['):
                raise Unreadable(
                    f'{self.path}:{line}: the globbing expression {pattern!r} '
                    f'has a wildcard in its directory part, which this sweep '
                    f'does not model')
        rel = self.world.relative(base)
        if rel is None:
            raise Unreadable(f'{self.path}:{line}: the globbing expression '
                             f'{pattern!r} leaves the tree')
        base_path = self.world.root / rel
        if not base_path.is_dir():
            return []
        found = []
        if recurse:
            for dirpath, dirnames, filenames in os.walk(base_path,
                                                        followlinks=False):
                for entry in dirnames + filenames:
                    if fnmatch.fnmatchcase(entry, tail):
                        found.append(Path(dirpath) / entry)
        else:
            for entry in sorted(os.listdir(base_path)):
                if fnmatch.fnmatchcase(entry, tail):
                    found.append(base_path / entry)
        out = []
        for match in found:
            matchrel = self.world.relative(match)
            if matchrel is not None and not self.world.removed(match):
                out.append(matchrel)
        return out

    def _tree_relative(self, path: str, line: int) -> str:
        """A path argument's tree-relative form, else a refused construct."""
        if not os.path.isabs(path):
            path = os.path.join(str(self.source_dir), path)
        rel = self.world.relative(os.path.normpath(path))
        if rel is None:
            raise Unreadable(f'{self.path}:{line}: {path} leaves the tree')
        return rel

    @property
    def rel_dir(self) -> str:
        try:
            return str(self.source_dir.relative_to(self.root))
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
    TOTALS['absent_only'] += len(only_absent)
    TOTALS['overlay_only'] += len(only_overlay)
    print(f'  {rel} [{profile_name}] absent-only names: {len(only_absent)}')
    for name in only_absent:
        fail(f'{rel} [{profile_name}] registers {name} only with the fixtures absent')
    print(f'  {rel} [{profile_name}] overlay-only names: {len(only_overlay)}')
    for name in only_overlay:
        fail(f'{rel} [{profile_name}] registers {name} only with the overlay present')


def check_recorded_rows(root: Path, recorded: list[str], pinned: list[str]) -> None:
    unsorted = recorded != sorted(recorded)
    if unsorted:
        fail('the recorded source-bound list is not sorted')
    duplicates = sorted({name for name in recorded if recorded.count(name) > 1})
    for name in duplicates:
        fail(f'the recorded source-bound list names {name} twice')
    pinned_set = set(pinned)
    unpinned = [name for name in recorded if name not in pinned_set]
    for name in unpinned:
        fail(f'the recorded source-bound row {name} is not a pinned row ({PIN_ROWS})')
    print(f'the recorded source-bound rows: {len(recorded)}; not a pinned row: '
          f'{len(unpinned)}; unsorted: {1 if unsorted else 0}; '
          f'duplicated: {len(duplicates)}')


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
                      PROFILES['kernel'], root, record=[])
    overlay.run()
    without = Machine(root / 'tests' / 'CMakeLists.txt', absent_world,
                      PROFILES['kernel'], root, record=[])
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


def recorded_refusal_shards(root: Path, removed_dirs, removed_files):
    """The recorded refusal-shard rows and their first-library markers, read
    from the absent world's own walk of runner/CMakeLists.txt: the branch that
    registers the placeholders is the one that sets both records, and the
    marker sequence is the one POP_FRONT consumed, one per row, in the row
    list's order -- the positional pairing the row's SKIP literal relies on."""
    machine = registered(root / 'runner/CMakeLists.txt',
                         World(root, removed_dirs, removed_files, virtual=True),
                         PROFILES['release'], root)
    recorded = machine.vars.get(REFUSAL_SHARD_RECORD)
    if recorded is None:
        raise Unreadable(f'runner/CMakeLists.txt: {REFUSAL_SHARD_RECORD} was '
                         f'never set on the fixture-less branch')
    markers = machine.popped.get(REFUSAL_SHARD_MARKERS)
    if markers is None:
        raise Unreadable(f'runner/CMakeLists.txt: {REFUSAL_SHARD_MARKERS} was '
                         f'never popped on the fixture-less branch, so the '
                         f'shard rows have no first-library markers')
    return list(recorded), list(markers)


def derived_refusal_shard_markers(machine: Machine) -> dict[str, str]:
    """The first library of every shard row the present branch's split really
    produces: each family's library list, cut into the same 8-library batches
    the while() registers, each batch named by its family and shard number.
    The marker is the library's case name -- the left side of its `case=`."""
    markers = {}
    for family in ('requests', 'pooc'):
        libraries = machine.vars.get(f'_pf_confirmed_refusal_{family}')
        if libraries is None:
            raise Unreadable(f'runner/CMakeLists.txt: '
                             f'_pf_confirmed_refusal_{family} was never set on '
                             f'the present branch')
        for offset in range(0, len(libraries), 8):
            row = f'{REFUSAL_SHARD_PREFIX}{family}_{offset // 8 + 1}'
            markers[row] = libraries[offset].split('=', 1)[0]
    return markers


def check_recorded_refusal_shards(recorded: list[str], markers: list[str],
                                  pinned: list[str]) -> None:
    unsorted = recorded != sorted(recorded)
    if unsorted:
        fail('the recorded refusal-shard list is not sorted')
    duplicates = sorted({name for name in recorded if recorded.count(name) > 1})
    for name in duplicates:
        fail(f'the recorded refusal-shard list names {name} twice')
    pinned_set = set(pinned)
    unpinned = [name for name in recorded if name not in pinned_set]
    for name in unpinned:
        fail(f'the recorded refusal-shard row {name} is not a pinned row '
             f'({PIN_ROWS})')
    marker_duplicates = sorted({m for m in markers if markers.count(m) > 1})
    empty = [m for m in markers if not m]
    for name in marker_duplicates:
        fail(f'the recorded refusal-shard marker list names {name} twice')
    if len(markers) != len(recorded):
        fail(f'the recorded refusal-shard marker list has {len(markers)} '
             f'entries for {len(recorded)} rows: the positional pairing is '
             f'incomplete')
    print(f'the recorded refusal-shard rows: {len(recorded)}; not a pinned row: '
          f'{len(unpinned)}; unsorted: {1 if unsorted else 0}; '
          f'duplicated: {len(duplicates)}')
    print(f'the recorded refusal-shard first-library markers: {len(markers)} '
          f'for {len(recorded)} rows; duplicated: {len(marker_duplicates)}; '
          f'empty: {len(empty)}')


def absent_half(root: Path, removed_dirs, removed_files, pin_rows, profiles, rel_files) -> None:
    for rel in rel_files:
        for profile_name in profiles:
            world = World(root, removed_dirs, removed_files, virtual=True)
            machine = registered(root / rel, world, PROFILES[profile_name], root)
            print(f'  {rel} [{profile_name}] absent-world rows: {len(machine.rows)}')
    recorded = read_source_bound_recorded(registered(
        root / 'tests' / 'CMakeLists.txt',
        World(root, removed_dirs, removed_files, virtual=True),
        PROFILES['release'], root))
    check_recorded_rows(root, recorded, pin_rows)
    print(f'  (the recorded list was read from {CMAKE_FILES[0]}, release profile)')
    shard_rows, shard_markers = recorded_refusal_shards(
        root, removed_dirs, removed_files)
    check_recorded_refusal_shards(shard_rows, shard_markers, pin_rows)
    print(f'  (the recorded refusal-shard list was read from '
          f'{CMAKE_FILES[1]}, release profile, fixture-less branch)')


def overlay_half(root: Path, removed_dirs, removed_files, pin_rows, profiles,
                 rel_files) -> bool:
    """The overlay half; True when it ran, False when the overlay is absent.

    A missing overlay with PINEFORGE_PRIVATE_FIXTURES set is a failure: the
    environment says the fixtures are in the tree, so a half that cannot run
    is a broken input, never a skip (the ctest row carries no skip code)."""
    marker = os.environ.get('PINEFORGE_PRIVATE_FIXTURES', '').strip()
    present = (root / OVERLAY_MARKER).exists()
    if not present:
        if marker:
            fail(f'PINEFORGE_PRIVATE_FIXTURES is set but {OVERLAY_MARKER} is '
                 f'not in the tree: an overlay run whose overlay half cannot '
                 f'run is a failure, never a skip')
        else:
            print(f'overlay half not run: private fixtures not present: '
                  f'{OVERLAY_MARKER}')
        return False
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
    # The recorded refusal-shard list, the same way: the present branch's
    # split over the live tree (the refused_*.cpp glob, the family regex, the
    # 8-library batches) is run for real and its shard rows must be exactly
    # the recorded ones. Comparing the list with itself is impossible here --
    # the derived side is a registration the walk performed, not a literal.
    recorded_shards, recorded_markers = recorded_refusal_shards(
        root, removed_dirs, removed_files)
    overlay_runner = registered(root / 'runner/CMakeLists.txt', overlay_world,
                                PROFILES['release'], root)
    derived_shards = sorted(
        name for name in overlay_runner.rows
        if name.startswith(REFUSAL_SHARD_PREFIX))
    derived_markers = derived_refusal_shard_markers(overlay_runner)
    recorded_only = sorted(set(recorded_shards) - set(derived_shards))
    derived_only = sorted(set(derived_shards) - set(recorded_shards))
    print(f'  refusal-shard drift: recorded {len(recorded_shards)} rows; the '
          f'split over the live tree (runner/CMakeLists.txt, release profile, '
          f'present branch) produces {len(derived_shards)}')
    for name in recorded_only:
        fail(f'the recorded refusal-shard row {name} is not produced by the '
             f'split over the live tree')
    for name in derived_only:
        fail(f'the split over the live tree registers the refusal-shard row '
             f'{name}, which the recorded list does not name')
    print(f'  refusal-shard drift: recorded-only: {len(recorded_only)}, '
          f'derived-only: {len(derived_only)}')
    # Each marker (the first library of its shard) is paired with its row by
    # POSITION, so a mis-paired marker still names a removed file and only
    # this comparison can catch it: the derived side is the real split's own
    # first library for that row.
    mismatched = [(row, marker, derived_markers.get(row))
                  for row, marker in zip(recorded_shards, recorded_markers)
                  if derived_markers.get(row) != marker]
    print(f'  refusal-shard drift: marker pairs checked: '
          f'{len(recorded_shards)}; mismatched: {len(mismatched)}')
    for row, marker, expected in mismatched:
        fail(f'the recorded first-library marker of {row} is {marker}, but the '
             f'split over the live tree puts {expected} first in that shard')
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=REPO)
    parser.add_argument('--both', action='store_true',
                        help='the ctest row: the absent half always runs and '
                             'decides the exit code, then the overlay half '
                             'runs when the tree holds the overlay')
    parser.add_argument('--overlay', action='store_true',
                        help='the overlay half: both worlds and the drift guard')
    parser.add_argument('--profile', action='append', default=None,
                        choices=sorted(PROFILES))
    parser.add_argument('--cmakelists', action='append', default=None, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    profiles = args.profile or list(PROFILE_NAMES)
    rel_files = [Path(rel) for rel in (args.cmakelists or CMAKE_FILES)]
    overlay_ran = False
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
            overlay_ran = overlay_half(root, removed_dirs, removed_files,
                                       pin_rows, profiles, rel_files)
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
    if args.overlay or args.both:
        if overlay_ran:
            print(f'absent-only names: {TOTALS["absent_only"]}')
            print(f'overlay-only names: {TOTALS["overlay_only"]}')
    else:
        print('two-world registration: the absent world evaluated, the recorded '
              'list is pinned, sorted and duplicate-free')
    return 0


if __name__ == '__main__':
    sys.exit(main())
