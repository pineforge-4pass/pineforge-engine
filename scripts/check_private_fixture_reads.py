#!/usr/bin/env python3
"""Every registered row that names a path under the private-fixture removals.

Why this exists
---------------
The private-fixture PRs take whole fixture directories (and their test
sources) out of the public tree. A row that still names one of those paths --
through a target_compile_definitions value, a quoted #include, a C++ or
Python string literal composed with its fixture root, or an add_test argument
-- either RUNS against absent data on the fixture-less tree (the abort the
proof box found: the row was never pinned) or cannot be built at all. The
first guard rounds pinned the rows they found one at a time; this sweep is the
class: it resolves the paths every registered row names, from the row's
sources, the CMake arguments and the data headers, against the pinned removed
set (tests/private_fixture_paths.txt), and requires each hit to be either

  * a pinned private-fixture row (tests/private_fixture_rows.txt), or
  * a row whose every registration site is either the then-branch of an
    EXISTS guard naming removed data (present exactly when the data is) or
    the pinned SKIP placeholder the absent branch registers, or
  * reported as a violation.

It also checks the class the overlay-mode round found (A-release,
rj-20261011t185619-d9567d): a Python tape driver whose private-marker guard
composes the fixture directory onto an add_test argument that already IS that
directory -- a marker that can never exist, so the row prints the pinned SKIP
literal and exits 77 in every mode, and the overlay-mode refusal stage blames
the overlay. The marker a driver tests must resolve to a REMOVED path: present
exactly when the private fixtures are overlaid.

Every registration is walked, not pattern-matched per form: the two CMakeLists
are read command by command so that a row registered through add_test (also
multi-line, also inside foreach loops the file expands), pineforge_add_unit_
test, pineforge_add_twin_test or add_executable is swept, and the count of
add_test sites walked is compared with the count present (a site this sweep
could not read, or read twice, exits 2). Every row pinned in
tests/private_fixture_rows.txt must be swept: a pinned row no registration
matched exits 1.

The list() subcommands the two files use are modelled, and the model is
printed as a table (subcommand, uses, modelled). APPEND and REMOVE_ITEM edit
the tracked list; SORT orders a tracked list (default ascending or ORDER
DESCENDING) and refuses an input the walk never saw built. The exact CR-150
lane glob is tracked, including when empty. LENGTH writes the tracked length
into its out-var; GET, SUBLIST and
POP_FRONT write the element(s) they select, so a value a row gets through them
is swept like any other. POP_FRONT follows CMake: N out-vars take the first N
elements (the rest of the list is removed), no out-var removes exactly one
element, and out-vars with no element left are undefined. A subcommand outside
the modelled set is a construct the sweep cannot read:
exit 2, in or out of a function() body, never a silent skip. GET, SUBLIST and
POP_FRONT read only a list this walk saw built; a list it never saw is exit 2,
never a guessed value. LENGTH is the one soft case: its result is a count,
which cannot name a path, and the two files take its inputs from
file(STRINGS) (a file the fixture-less tree does not have), so an input the
walk did not see built prints an "input not tracked" line and writes nothing
-- the closing summary counts those lines. string(REPLACE "|" ";" <out>
<input>) is modelled in that one shape for the same reason: it is how the
foreach items the list(GET) sites split are produced.

Loud-fail: a construct it cannot parse (a missing pin file, a driver whose
marker expression it cannot read, a row whose add_test arguments it cannot
resolve) prints file:line and exits 2; it never counts what it cannot read as
clean.

Exit 0 when every hit is pinned or guard/placeholder-resolved and every marker
names a removed path, 1 on a violation, 2 on a construct it cannot parse.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

REMOVED_PIN = Path('tests/private_fixture_paths.txt')
ROW_PIN = Path('tests/private_fixture_rows.txt')
EVIDENCE = Path('tests/private_fixture_reads.evidence')
SKIP_LITERAL = 'SKIP private fixtures not present: '

FAILURES: list[str] = []


class Unparseable(Exception):
    pass


def fail(message: str) -> None:
    FAILURES.append(message)


def strip_comments(text: str) -> str:
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


def load_removed(root: Path) -> tuple[set[str], set[str]]:
    path = root / REMOVED_PIN
    if not path.exists():
        raise Unparseable(f'{path}: the removed-path pin is gone')
    dirs: set[str] = set()
    files: set[str] = set()
    for line in path.read_text().splitlines():
        if line.startswith('D '):
            dirs.add(line[2:].strip())
        elif line.startswith('F '):
            files.add(line[2:].strip())
    if not dirs or not files:
        raise Unparseable(f'{path}: the pin parsed empty')
    return dirs, files


def load_pinned_rows(root: Path) -> dict[str, int]:
    path = root / ROW_PIN
    if not path.exists():
        raise Unparseable(f'{path}: the private-row pin is gone')
    rows: dict[str, int] = {}
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        name = line.strip()
        if name and not name.startswith('//'):
            rows[name] = lineno
    if not rows:
        raise Unparseable(f'{path}: the row pin parsed empty')
    return rows


def load_evidence(root: Path) -> dict[str, tuple[str, str, str]]:
    """row -> (file:line, token, reason); every cited line is re-read."""
    path = root / EVIDENCE
    if not path.exists():
        raise Unparseable(f'{path}: the evidence file is gone')
    out: dict[str, tuple[str, str, str]] = {}
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.startswith('//'):
            continue
        fields = line.split('\t')
        if len(fields) != 4:
            raise Unparseable(f'{path}:{lineno}: not a row<TAB>file:line<TAB>token<TAB>reason record')
        row, cite, token, reason = (f.strip() for f in fields)
        if ':' not in cite:
            raise Unparseable(f'{path}:{lineno}: the citation {cite} has no line')
        rel, _, number = cite.rpartition(':')
        source = root / rel
        if not source.exists():
            raise Unparseable(f'{path}:{lineno}: {rel} does not exist')
        lines = source.read_text().splitlines()
        if not number.isdigit() or not (1 <= int(number) <= len(lines)):
            raise Unparseable(f'{path}:{lineno}: line {number} is out of range in {rel}')
        if token not in lines[int(number) - 1]:
            raise Unparseable(f'{path}:{lineno}: the token no longer sits on {cite}')
        out[row] = (cite, token, reason)
    return out


def is_removed(path: str, removed_dirs: set[str], removed_files: set[str]) -> bool:
    """True when the repo-relative path names removed data.

    A path still carrying a `${...}` reference is a construct this sweep did
    not resolve. The old form answered False for it, which silently cleared a
    row that might name removed data; it is a loud failure instead."""
    path = path.strip('"').strip('/')
    if '$' in path:
        raise Unparseable(f'a path this sweep never resolved: {path!r}')
    if not path:
        return False
    if path in removed_files:
        return True
    parts = path.split('/')
    for i in range(1, len(parts) + 1):
        if '/'.join(parts[:i]) in removed_dirs:
            return True
    return False


# Variables an argv can carry that name no repo path: a command or an
# interpreter. Bound to stated non-repo values, so a token like
# ${CMAKE_COMMAND} resolves instead of arriving unresolved at is_removed (a
# path this sweep could not resolve is a loud failure there).
NON_REPO_VARIABLES = {
    '${CMAKE_COMMAND}': '/usr/bin/cmake',
    '${Python3_EXECUTABLE}': '/usr/bin/python3',
    '${CMAKE_CURRENT_BINARY_DIR}': '/build/current',
    '${CMAKE_BINARY_DIR}': '/build',
    '${PROJECT_BINARY_DIR}': '/build',
    '${CMAKE_CXX_COMPILER}': '/usr/bin/c++',
}


def resolve_cmake_path(value: str, cmake_dir: str) -> str:
    value = value.replace('${CMAKE_CURRENT_SOURCE_DIR}', cmake_dir)
    value = value.replace('${PROJECT_SOURCE_DIR}', '')
    value = value.replace('${CMAKE_SOURCE_DIR}', '')
    for name, path in NON_REPO_VARIABLES.items():
        value = value.replace(name, path)
    return value.strip('"').lstrip('/')


def split_argument_elements(raw: str, table: 'Rows') -> list[str]:
    """A define value's or argv token's path candidates: the walk's own
    variables substituted (subst), CMake's list division applied, the
    source-dir variables resolved. An element still carrying a ${...} or
    $<...> reference is returned as it is -- the caller classifies it, so it
    is never silently treated as a path or silently dropped."""
    out = []
    for part in subst(raw, table.vars).split(';'):
        element = resolve_cmake_path(part.strip('"'), table.rel_dir)
        if element:
            out.append(element)
    return out


def subst(text: str, env: dict, depth: int = 0) -> str:
    """Substitute every ${var} the walk bound, one configured value each."""
    if depth > 10 or '${' not in text:
        return text
    def replace(m: re.Match) -> str:
        values = env.get(m.group(1))
        if not values:
            return m.group(0)
        return values[0]
    return subst(re.sub(r'\$\{(\w+)\}', replace, text), env, depth + 1)


def expand_names(raw: str, env: dict, limit: int = 64) -> set[str]:
    """Every literal name a registration's raw name token can resolve to."""
    names = {raw}
    for _ in range(10):
        nxt: set[str] = set()
        grew = False
        for name in names:
            m = re.search(r'\$\{(\w+)\}', name)
            if not m or m.group(1) not in env:
                nxt.add(name)
                continue
            for value in env[m.group(1)]:
                grown = name[:m.start()] + value + name[m.end():]
                if len(nxt) < limit:
                    nxt.add(grown)
                grew = True
        names = nxt
        if not grew:
            break
    resolved = {n for n in names if '${' not in n and not n.startswith('$<')}
    if len(resolved) != len(names):
        return set()
    return resolved


class Rows:
    """Registered ctest rows and what they name, per CMakeLists."""

    def __init__(self, path: Path, root: Path):
        self.path = path
        self.root = root
        self.rel_dir = str(path.parent.relative_to(root))
        self.text = strip_comments(path.read_text())
        self.lines = self.text.split('\n')
        self.line_starts = [0]
        for line in self.lines[:-1]:
            self.line_starts.append(self.line_starts[-1] + len(line) + 1)
        self.vars: dict[str, list[str]] = {}
        self.rows: dict[str, dict] = {}
        self.target_sources: dict[str, set] = {}
        self.site_lines: set[int] = set()
        self.add_test_sites_skipped = 0
        self.dynamic_names: list[tuple[int, str]] = []
        self.list_uses: dict[str, int] = {}
        self.unresolved_list_inputs: list[tuple[int, str, str, str]] = []
        self.cr150_lane_files: list[Path] | None = None
        self._check_sites_readable()
        self._scan_list_subcommands()
        top_env: dict[str, list[str]] = {}
        self._walk(0, len(self.lines), top_env, ())
        self.vars = top_env

    @property
    def add_test_sites(self) -> int:
        return len(self.site_lines)

    # ---- command location helpers -----------------------------------------
    def _line_of(self, index: int) -> int:
        line, lo, hi = 0, 0, len(self.line_starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.line_starts[mid] <= index:
                line = mid
                lo = mid + 1
            else:
                hi = mid
        return line

    @staticmethod
    def _paren_body(text: str, open_index: int) -> tuple[str, int]:
        """(body, end index) of the (...) that opens at open_index."""
        i = open_index
        depth = 0
        start = None
        while i < len(text):
            if text[i] == '(':
                depth += 1
                if depth == 1:
                    start = i + 1
            elif text[i] == ')':
                depth -= 1
                if depth == 0:
                    return text[start:i], i
            i += 1
        raise Unparseable('an unclosed ( while reading the CMake text')

    def _command_at(self, line_index: int):
        m = re.match(r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(', self.lines[line_index])
        if not m:
            return None
        open_index = self.line_starts[line_index] + m.end() - 1
        body, close = self._paren_body(self.text, open_index)
        return m.group(1), body, self._line_of(close)

    def _block_end(self, start_line: int, opener: str, closer: str) -> int:
        """Line index of the closer() that ends the block opening at start."""
        depth = 0
        for i in range(start_line, len(self.lines)):
            stripped = self.lines[i].strip()
            if re.match(rf'\s*{opener}\(', self.lines[i]) or stripped.startswith(f'{opener}('):
                depth += 1
            if re.match(rf'\s*{closer}\(', self.lines[i]) or stripped.startswith(f'{closer}('):
                depth -= 1
                if depth == 0:
                    return i
        raise Unparseable(f'{self.path}: {opener}( at line {start_line + 1} has no {closer}()')

    def _check_sites_readable(self) -> None:
        """Every add_test( site must name its row; one this sweep cannot read
        is exit 2, never a silent skip."""
        for m in re.finditer(r'add_test\(', self.text):
            lineno = self._line_of(m.start()) + 1
            if not re.match(r'add_test\(\s*NAME\s+[^\s)]+', self.text[m.start():]):
                raise Unparseable(
                    f'{self.path}:{lineno}: an add_test( whose NAME this sweep cannot read')

    # list() subcommands this sweep models, and what each does to the tracked
    # variables. Any other subcommand is a construct it cannot read.
    LIST_MODELLED = ('APPEND', 'GET', 'LENGTH', 'POP_FRONT', 'REMOVE_ITEM',
                     'SORT', 'SUBLIST')

    def _scan_list_subcommands(self) -> None:
        """Count every list(<subcommand>) the file has -- in or out of a
        function() body, since only the call sites of a template register --
        and refuse any form whose effect on the tracked lists is not modelled.
        The counts are what the printed table shows, so a subcommand that
        appeared since the model was written cannot hide behind the walk."""
        for m in re.finditer(r'\blist\s*\(', self.text):
            lineno = self._line_of(m.start()) + 1
            before = m.start() - 1
            while before >= 0 and self.text[before] in ' \t':
                before -= 1
            if before >= 0 and self.text[before] not in ')\n':
                raise Unparseable(
                    f'{self.path}:{lineno}: a list( this sweep cannot classify '
                    f'(not at a command position, or inside a string)')
            body, _ = self._paren_body(self.text, m.start() + len(m.group(0)) - 1)
            head = re.match(r'\s*(\w+)', body)
            subcommand = head.group(1) if head else ''
            if subcommand not in self.LIST_MODELLED:
                raise Unparseable(
                    f'{self.path}:{lineno}: list({subcommand}) is not a '
                    f'subcommand this sweep models: its effect on the tracked '
                    f'lists would be skipped, and skipped is not clean')
            self.list_uses[subcommand] = self.list_uses.get(subcommand, 0) + 1

    # ---- rows --------------------------------------------------------------
    def _entry(self, row: str, lineno: int, conditions: tuple, table) -> dict:
        entry = self.rows.get(row)
        if entry is None:
            entry = {'defines': {}, 'lineno': lineno, 'argvs': [],
                     'conditions': [], 'sites': [], 'table': table}
            self.rows[row] = entry
        elif not entry['lineno']:
            entry['lineno'] = lineno
        return entry

    def _site(self, row: str, lineno: int, conditions: tuple, placeholder: bool,
              table) -> dict:
        entry = self._entry(row, lineno, conditions, table)
        if conditions and conditions not in entry['conditions']:
            entry['conditions'].append(conditions)
        entry['sites'].append({'lineno': lineno, 'conditions': conditions,
                               'placeholder': placeholder})
        return entry

    def _register(self, raw: str, lineno: int, env: dict, conditions: tuple,
                  table, argv=None, placeholder: bool = False,
                  sources=None, twin_target: str | None = None) -> None:
        names = expand_names(raw.strip('"'), env)
        if not names:
            self.dynamic_names.append((lineno + 1, raw))
            names = {raw}
        for name in names:
            entry = self._site(name, lineno + 1, conditions, placeholder, table)
            if argv is not None:
                entry['argvs'].append([subst(t, env) for t in argv])
            if sources:
                entry.setdefault('sources', set()).update(sources)
            if twin_target:
                entry.setdefault('twin_targets', set()).add(twin_target)

    # ---- the walker --------------------------------------------------------
    def _walk(self, start: int, end: int, env: dict, conditions: tuple) -> None:
        i = start
        while i < end:
            command = self._command_at(i)
            if command is None:
                i += 1
                continue
            name, body, close_line = command
            if name == 'function':
                # A function() body is a template: the add_test sites in it
                # register nothing themselves; the call sites do.
                block_end = self._block_end(i, 'function', 'endfunction')
                block_text = '\n'.join(self.lines[close_line + 1:block_end])
                self.add_test_sites_skipped += len(re.findall(r'add_test\(', block_text))
                i = block_end + 1
                continue
            if name == 'if':
                i = self._walk_if(i, body, close_line, env, conditions)
                continue
            if name == 'foreach':
                block_end = self._block_end(i, 'foreach', 'endforeach')
                self._walk_foreach(body, close_line + 1, block_end, env,
                                   conditions)
                i = block_end + 1
                continue
            if name == 'while':
                block_end = self._block_end(i, 'while', 'endwhile')
                self._walk(close_line + 1, block_end, dict(env), conditions)
                i = block_end + 1
                continue
            if name == 'set':
                self._do_set(body, env)
            elif name == 'file':
                self._do_cr150_glob(body, i, env)
            elif name == 'list':
                self._do_list(body, i, env)
            elif name == 'string':
                self._do_string(body, i, env)
            elif name == 'include':
                pass
            elif name == 'add_test':
                self.site_lines.add(i)
                self._do_add_test(body, i, env, conditions)
            elif name == 'target_compile_definitions':
                self._do_defines(body, i, env, conditions)
            elif name in ('add_executable', 'pineforge_add_unit_test',
                          'pineforge_add_twin_test'):
                self._do_register_call(name, body, i, env, conditions)
            i = close_line + 1

    def _walk_if(self, line_i: int, body: str, close_line: int, env: dict,
                 conditions: tuple) -> int:
        """Walk every branch, each with its own condition polarity: the then
        branch of if(BODY) runs only when BODY holds, else()/elseif branches
        only when it does not. Variables a branch sets stay multi-valued."""
        block_end = self._block_end(line_i, 'if', 'endif')
        if_body = body
        segments: list[tuple[int, int, tuple]] = []   # (first, last, conds)
        segment_start, depth = close_line + 1, 1
        pending: list[tuple[str, bool]] = [(if_body, True)]
        for i in range(close_line + 1, block_end + 1):
            stripped = self.lines[i].strip()
            if re.match(r'if\(', stripped):
                depth += 1
            elif stripped.startswith('endif('):
                depth -= 1
                if depth == 0:
                    segments.append((segment_start, i, conditions + tuple(pending)))
                    break
            elif depth == 1 and stripped == 'else()':
                segments.append((segment_start, i, conditions + tuple(pending)))
                segment_start = i + 1
                pending = [(if_body, False)]
            elif depth == 1 and stripped.startswith('elseif('):
                segments.append((segment_start, i, conditions + tuple(pending)))
                segment_start = i + 1
                m = re.match(r'elseif\s*\((.*)\)\s*$', stripped)
                pending = [(if_body, False),
                           (m.group(1) if m else stripped, True)]
        merged: dict[str, list[str]] = {}
        for first, last, conds in segments:
            branch_env = dict(env)
            self._walk(first, last, branch_env, conds)
            for name, values in branch_env.items():
                if name in env and values == env[name]:
                    continue
                out = merged.setdefault(name, list(env.get(name, [])))
                for value in values:
                    if value not in out:
                        out.append(value)
        env.update(merged)
        return block_end + 1

    def _walk_foreach(self, body: str, first: int, last: int, env: dict,
                      conditions: tuple) -> None:
        head = re.match(r'\s*(\w+)\s+(.*)$', body, re.S)
        if not head:
            raise Unparseable(f'{self.path}:{first + 1}: a foreach header this sweep cannot read')
        var, rest = head.group(1), head.group(2).strip()
        m = re.match(r'IN\s+LISTS\s+(.*)$', rest, re.S)
        if m:
            items: list[str | None] = []
            for list_name in m.group(1).split():
                values = env.get(list_name)
                items.extend(values if values else [None])
        else:
            # bare foreach(<var> <items...>), IN ITEMS or IN ${list}
            m = re.match(r'IN\s+(?:ITEMS\s+)?(.*)$', rest, re.S)
            item_text = (m.group(1) if m else rest).strip()
            single = re.fullmatch(r'\$\{(\w+)\}', item_text)
            if single:
                items = env.get(single.group(1)) or [None]
            else:
                items = [subst(t, env) if '${' in t else t
                         for t in item_text.split()]
        for item in items:
            if item is None:
                item = '${' + var + '}'
            item_env = dict(env)
            item_env[var] = [subst(item, env)]
            self._walk(first, last, item_env, conditions)

    def _do_set(self, body: str, env: dict) -> None:
        m = re.match(r'\s*(\w+)\s+(.*)$', body, re.S)
        if not m:
            return
        name, rest = m.group(1), m.group(2)
        values = env.setdefault(name, [])
        for token in rest.split():
            value = token.strip('"')
            if value not in values:
                values.append(value)

    def _do_cr150_glob(self, body: str, line_i: int, env: dict) -> None:
        """Bind only the lane glob whose files this sweep will include."""
        if self.path != self.root / 'tests' / 'CMakeLists.txt':
            return
        if not re.fullmatch(
                r'\s*GLOB\s+CR150_LANE_CMAKE\s+CONFIGURE_DEPENDS\s+'
                r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/cr150\.d/\*\.cmake"\s*', body):
            return
        directory = self.root / 'tests' / 'cr150.d'
        try:
            paths = list(directory.iterdir())
        except FileNotFoundError:
            paths = []                         # CMake's empty-glob semantics
        except OSError as error:
            raise Unparseable(
                f'{self.path}:{line_i + 1}: cannot read {directory}: {error}') from error
        self.cr150_lane_files = sorted(path for path in paths
                                       if path.name.endswith('.cmake'))
        env['CR150_LANE_CMAKE'] = [str(path) for path in self.cr150_lane_files]

    def _do_list(self, body: str, line_i: int, env: dict) -> None:
        """Model the list() subcommands the two files use. What the walk does
        not model is exit 2 (see _scan_list_subcommands): a list() whose
        effect is skipped would let a value a row names go unswept."""
        head = re.match(r'\s*(\w+)\b(.*)$', body, re.S)
        if not head:
            raise Unparseable(
                f'{self.path}:{line_i + 1}: a list( whose subcommand this '
                f'sweep cannot read')
        action, rest = head.group(1), head.group(2).strip()
        if action not in self.LIST_MODELLED:
            raise Unparseable(
                f'{self.path}:{line_i + 1}: list({action}) is not a '
                f'subcommand this sweep models')
        if action in ('APPEND', 'REMOVE_ITEM'):
            m = re.match(r'\s*(\w+)\s+(.*)$', rest, re.S)
            if not m:
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: a list({action}) whose list '
                    f'name this sweep cannot read')
            name, tail = m.groups()
            values = env.setdefault(name, [])
            for token in tail.split():
                value = token.strip('"')
                splice = re.fullmatch(r'\$\{(\w+)\}', value)
                if splice:                  # list(APPEND X ${Y}) splices Y
                    items = env.get(splice.group(1), [])
                else:
                    items = [value]
                for item in items:
                    if action == 'APPEND':
                        if item not in values:
                            values.append(item)
                    elif item in values:
                        values.remove(item)
            return

        if action == 'SORT':
            tokens = rest.split()
            if not tokens or not re.fullmatch(r'\w+', tokens[0]):
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: a list(SORT) whose list name '
                    f'this sweep cannot read')
            name = tokens[0]
            values = env.get(name)
            if values is None:
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: list(SORT) is not a '
                    f'subcommand this sweep models: its effect on the tracked '
                    f'lists would be skipped, and skipped is not clean')
            options = tokens[1:]
            if options not in ([], ['ORDER', 'DESCENDING']):
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: list(SORT) option '
                    f'{" ".join(options)!r} is not modelled')
            env[name] = sorted(values, reverse=bool(options))
            return

        tokens = rest.split()
        unreadable = [
            token for token in tokens
            if not re.fullmatch(r'\w+', token) and not token.startswith('${')]
        if unreadable:
            raise Unparseable(
                f'{self.path}:{line_i + 1}: list({action}) names '
                f'{unreadable[0]}, a value this sweep cannot read')

        if action == 'LENGTH':
            if len(tokens) != 2:
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: a list(LENGTH) this sweep '
                    f'cannot read (needs a list and an out-var)')
            name, out = tokens
            values = env.get(name)
            if values is None:
                # A count, so it can never be the path a row names; the files
                # take these inputs from file(STRINGS), which this sweep does
                # not model. Recorded loudly instead of guessed.
                self.unresolved_list_inputs.append(
                    (line_i + 1, action, name, out))
                return
            env[out] = [str(len(values))]
            return

        if action == 'GET':
            if len(tokens) < 3 or not re.fullmatch(r'\w+', tokens[-1]):
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: a list(GET) this sweep cannot '
                    f'read (needs a list, an index and an out-var)')
            name, index_tokens, out = tokens[0], tokens[1:-1], tokens[-1]
            values = env.get(name)
            if values is None:
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: list(GET {name} ...) reads a '
                    f'list this sweep never saw built: {out} would keep a '
                    f'value the sweep cannot resolve')
            picked = []
            for token in index_tokens:
                index = self._list_index(token, env, line_i, name)
                if not -len(values) <= index < len(values):
                    raise Unparseable(
                        f'{self.path}:{line_i + 1}: list(GET {name} {token} '
                        f'...) is out of range: {len(values)} element(s)')
                picked.append(values[index])
            env[out] = picked
            return

        if action == 'SUBLIST':
            if len(tokens) != 4 or not re.fullmatch(r'\w+', tokens[3]):
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: a list(SUBLIST) this sweep '
                    f'cannot read (needs a list, a begin, a length and an '
                    f'out-var)')
            name, begin_token, length_token, out = tokens
            values = env.get(name)
            if values is None:
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: list(SUBLIST {name} ...) '
                    f'reads a list this sweep never saw built: {out} would '
                    f'keep a value the sweep cannot resolve')
            begin = self._list_index(begin_token, env, line_i, name)
            length = self._list_index(length_token, env, line_i, name)
            if begin < 0 or begin > len(values):
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: list(SUBLIST {name} '
                    f'{begin_token} ...) begins out of range: {len(values)} '
                    f'element(s)')
            if length < -1:
                raise Unparseable(
                    f'{self.path}:{line_i + 1}: list(SUBLIST {name} ... '
                    f'{length_token} {out}) has a length CMake refuses')
            if length == -1:
                length = len(values) - begin
            # CMake writes the empty string for an empty sublist.
            env[out] = values[begin:begin + length] or ['']
            return

        # POP_FRONT: N out-vars take the first N elements and CMake removes
        # them from the list; no out-var removes exactly one; an out-var with
        # no element left is left undefined, not emptied.
        if not tokens or not re.fullmatch(r'\w+', tokens[0]):
            raise Unparseable(
                f'{self.path}:{line_i + 1}: a list(POP_FRONT) whose list name '
                f'this sweep cannot read')
        name, outs = tokens[0], tokens[1:]
        values = env.get(name)
        if values is None:
            raise Unparseable(
                f'{self.path}:{line_i + 1}: list(POP_FRONT {name} ...) reads '
                f'a list this sweep never saw built: the popped value cannot '
                f'be resolved')
        count = len(outs) if outs else 1
        popped = values[:count]
        del values[:count]
        for position, out in enumerate(outs):
            if position < len(popped):
                env[out] = [popped[position]]
            else:
                env.pop(out, None)

    def _list_index(self, token: str, env: dict, line_i: int, name: str) -> int:
        """An index argument of list(GET)/list(SUBLIST): a literal or a ${var}
        this walk bound, never anything else."""
        text = subst(token, env)
        if not re.fullmatch(r'-?\d+', text):
            raise Unparseable(
                f'{self.path}:{line_i + 1}: a list( command on {name} uses '
                f'{token} as an index, which this sweep cannot resolve')
        return int(text)

    def _do_string(self, body: str, line_i: int, env: dict) -> None:
        """string(REPLACE <sep> <glue> <out> <input>...) in the one shape the
        two files use: splitting a '|'-joined foreach item into the fields
        the list(GET) sites then read. Inputs this walk did not see built
        leave <out> unknown -- the standing treatment of a value the sweep
        never saw -- and every other string() form is left alone."""
        m = re.match(r'\s*REPLACE\s+(\S+)\s+(\S+)\s+(\w+)\s+(.*)$', body, re.S)
        if not m:
            return
        separator = m.group(1).strip('"')
        out, tail = m.group(3), m.group(4)
        if not separator:
            return
        fields: list[str] = []
        for token in tail.split():
            text = subst(token, env)
            if '${' in text:
                return                      # input not tracked: out stays unknown
            fields.extend(text.strip('"').split(separator))
        env[out] = fields or ['']

    def _do_add_test(self, body: str, line_i: int, env: dict,
                     conditions: tuple) -> None:
        m = re.match(r'\s*NAME\s+([^\s)]+)', body)
        if not m:
            raise Unparseable(
                f'{self.path}:{line_i + 1}: an add_test( whose NAME this sweep cannot read')
        argv = body.split()
        for token in argv:
            for var in re.findall(r'\$\{(_pf_\w+)\}', token):
                if var not in env:
                    raise Unparseable(
                        f'{self.path}:{line_i + 1}: an add_test argument names '
                        f'${{{var}}}, which this sweep cannot resolve')
        placeholder = SKIP_LITERAL in ' '.join(argv)
        self._register(m.group(1), line_i, env, conditions, self, argv=argv,
                       placeholder=placeholder)

    def _do_defines(self, body: str, line_i: int, env: dict,
                    conditions: tuple) -> None:
        m = re.match(r'\s*(\S+?)\s+PRIVATE\s+(.*)$', body, re.S)
        if not m or not m.group(1).startswith('test_'):
            return
        target = subst(m.group(1), env)
        entry = self._site(target, line_i + 1, conditions, False, self)
        for dm in re.finditer(r'(\w+)\s*=\s*"([^"]+)"', m.group(2)):
            entry['defines'][dm.group(1)] = subst(dm.group(2), env)

    def _do_register_call(self, command: str, body: str, line_i: int, env: dict,
                          conditions: tuple) -> None:
        tokens = body.split()
        if not tokens:
            return
        raw = tokens[0]
        if command == 'add_executable':
            if not re.match(r'(test_|\$\{)', raw):
                return
            names = expand_names(subst(raw, env), env) or {raw}
            sources = {subst(t, env) for t in tokens[1:]
                       if not t.startswith('$') and t.endswith(('.cpp', '.c'))}
            for name in names:
                self.target_sources.setdefault(name, set()).update(sources)
                self._register(name, line_i, env, conditions, self,
                               sources=sources)
            return
        if command == 'pineforge_add_twin_test':
            if len(tokens) < 2:
                return
            self._register(raw, line_i, env, conditions, self,
                           twin_target=subst(tokens[1], env))
            return
        name = subst(raw, env)
        sources = {f'{name}.cpp'}
        self.target_sources.setdefault(name, set()).update(sources)
        self._register(raw, line_i, env, conditions, self, sources=sources)

    # ---- what the sweep asks of a row --------------------------------------
    def python_script(self, row: str) -> Path | None:
        """The row's Python driver, resolved the way CMake resolves it: the
        argument's path against the tree root with the source-dir variables
        substituted. The old form looked only beside the CMakeLists (and only
        at the bare file name), so a script under scripts/ was never found
        and the row's marker check silently did not run."""
        entry = self.rows.get(row)
        if not entry:
            return None
        for argv in entry['argvs']:
            for token in argv:
                if token.strip('"').endswith('.py'):
                    return self.root / resolve_cmake_path(token.strip('"'),
                                                          self.rel_dir)
        return None

    def fixtures_arg(self, row: str) -> str | None:
        entry = self.rows.get(row)
        if not entry:
            return None
        for argv in entry['argvs']:
            seen_script = False
            for token in argv:
                if token.strip('"').endswith('.py'):
                    seen_script = True
                    continue
                if seen_script:
                    return token
        return None


def cxx_literals(path: Path) -> list[tuple[int, str]]:
    out = []
    for lineno, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
        if line.lstrip().startswith('//'):
            continue
        for lit in re.findall(r'"([^"]*)"', line):
            out.append((lineno, lit))
    return out


def quoted_includes(path: Path) -> list[tuple[int, str]]:
    out = []
    for lineno, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
        m = re.search(r'#include\s+"([^"]+)"', line)
        if m:
            out.append((lineno, m.group(1)))
    return out


def row_sources(path: Path) -> list[tuple[Path, int, str]]:
    """(file, lineno, quoted include) closure of one TU, quoted includes only."""
    out: list[tuple[Path, int, str]] = []
    pending = [path]
    seen = set()
    while pending:
        current = pending.pop(0)
        if current in seen or not current.exists():
            continue
        seen.add(current)
        for lineno, inc in quoted_includes(current):
            target = (current.parent / inc)
            out.append((current, lineno, inc))
            if target.exists() and target.suffix in ('.hpp', '.h', '.inc'):
                pending.append(target.resolve())
    return out


def marker_expression(driver: Path) -> list[str] | None:
    """The driver's private marker in the pinned form -- an ASSIGNMENT at
    the start of a line: `_pf_private_marker = fixtures / \"...\" / \"...\"`.
    The identifier inside a message or a comment is not a marker (this sweep's
    own diagnostic text names the pinned form), so the search is anchored."""
    text = driver.read_text(errors='replace')
    m = re.search(r'^[ \t]*_pf_private_marker\s*=\s*fixtures\s*'
                  r'((?:/\s*\"[^\"]*\"\s*)+)', text, re.M)
    if not m:
        return None
    return re.findall(r'\"([^\"]*)\"', m.group(1))


def sweep(root: Path) -> int:
    removed_dirs, removed_files = load_removed(root)
    pinned = load_pinned_rows(root)
    pinned_total = len(pinned)
    evidence = load_evidence(root)
    cmake_files = [root / 'tests' / 'CMakeLists.txt',
                   root / 'runner' / 'CMakeLists.txt']

    rows: dict[str, dict] = {}
    add_test_sites = 0
    add_test_sites_skipped = 0
    dynamic_names: list[tuple[str, int, str]] = []
    list_uses: dict[str, int] = {}
    unresolved_list_inputs: list[tuple[str, int, str, str, str]] = []
    for cmake in cmake_files:
        if not cmake.exists():
            raise Unparseable(f'{cmake}: missing')
        table = Rows(cmake, root)
        add_test_sites += table.add_test_sites
        add_test_sites_skipped += table.add_test_sites_skipped
        for lineno, raw in table.dynamic_names:
            dynamic_names.append((str(cmake.relative_to(root)), lineno, raw))
        for subcommand, count in table.list_uses.items():
            list_uses[subcommand] = list_uses.get(subcommand, 0) + count
        for lineno, subcommand, name, out in table.unresolved_list_inputs:
            unresolved_list_inputs.append(
                (str(cmake.relative_to(root)), lineno, subcommand, name, out))
        for name, entry in table.rows.items():
            entry['cmake'] = cmake
            rows.setdefault(name, entry)

    # Every add_test( site the tree has must have been walked (sites inside a
    # function() are templates; their call sites register the rows).
    site_total = 0
    for cmake in cmake_files:
        text = strip_comments(cmake.read_text())
        site_total += len(re.findall(r'add_test\(', text))
    walked_total = site_total - add_test_sites_skipped
    if walked_total != add_test_sites:
        raise Unparseable(
            f'add_test sites walked: {add_test_sites} of {walked_total} '
            f'outside function() templates ({site_total} present): a site this '
            f'sweep did not walk')

    hits: dict[str, list[tuple[str, int, str]]] = {}
    markers: list[tuple[str, str, str]] = []
    missing_scripts: list[tuple[str, str]] = []
    unresolved_argv_tokens: list[tuple[str, str, int, str]] = []
    checked_paths = 0

    for row in sorted(rows):
        entry = rows[row]
        table: Rows = entry['table']
        define_values = []
        for raw in entry['defines'].values():
            for element in split_argument_elements(raw, table):
                if '$' in element:
                    # a reference the walk did not bind; the shape decides:
                    # path-shaped -> this sweep cannot resolve it (exit 2),
                    # not path-shaped -> named in the closing count
                    if '/' in element:
                        raise Unparseable(
                            f'{table.path}: {row}: a compile definition '
                            f'whose path the sweep never resolved: {element}')
                    unresolved_argv_tokens.append(
                        (row, str(table.path.relative_to(root)), 0, element))
                    continue
                define_values.append(element)
        named: list[tuple[str, int, str]] = []   # (where, lineno, path)

        for value in define_values:
            named.append((str(table.path.relative_to(root)), 0, value))

        src_names = set(entry.get('sources') or ())
        for target in entry.get('twin_targets') or ():
            src_names |= table.target_sources.get(target, set())
        if not src_names:
            src_names = {f'{row}.cpp'}
        files = []
        base = (root / table.rel_dir).resolve()
        for src_name in sorted(src_names):
            resolved = resolve_cmake_path(src_name, table.rel_dir)
            src = (base / resolved).resolve()
            if '..' in Path(resolved).parts or (
                    src != base and base not in src.parents):
                raise Unparseable(
                    f'{table.path}: {row} names a source outside the tree: '
                    f'{src_name}')
            if not src.exists() or src.suffix not in ('.cpp', '.c'):
                continue
            files.append(src)
            for holder, lineno, inc in row_sources(src):
                files.append(holder)
                named.append((str(holder.relative_to(root)), lineno,
                              str(holder.parent.relative_to(root) / inc)))
        for f in dict.fromkeys(files):
            for lineno, lit in cxx_literals(f):
                if not lit:
                    continue
                # a literal that already names a repo path
                if lit.startswith(('tests/', 'fixtures/')):
                    named.append((str(f.relative_to(root)), lineno, lit))
                # a literal composed with a fixture-dir define: root + lit
                for value in define_values:
                    joined = value.rstrip('/') + '/' + lit.lstrip('/')
                    named.append((str(f.relative_to(root)), lineno, joined))
        # the add_test command's own path arguments. A ${...} token is first
        # resolved through the walk's own variables and CMake's list division;
        # an element that still carries a reference and is path-shaped (it
        # holds a '/') is a construct this sweep cannot resolve -- exit 2 --
        # while a reference that names no path (a command or a non-path
        # variable, no '/') is counted and printed, never silently dropped.
        for argv in entry['argvs']:
            for token in argv:
                if token.startswith(('tests/', 'fixtures/', '${')):
                    where = str(table.path.relative_to(root))
                    for element in split_argument_elements(token, table):
                        if '$' in element:
                            if '/' in element:
                                raise Unparseable(
                                    f'{where}:{entry["lineno"]}: {row}: an '
                                    f'argument path the sweep never '
                                    f'resolved: {element}')
                            unresolved_argv_tokens.append(
                                (row, where, entry['lineno'], element))
                            continue
                        named.append((where, entry['lineno'], element))

        # Python driver: the private marker and the skip literal.
        script = table.python_script(row)
        if script is not None and not script.exists():
            # A row whose driver the tree does not have at the path its own
            # argv names: the marker check cannot run, and skipping it quietly
            # is how 127 rows went unchecked. Counted, then failed.
            missing_scripts.append((row, str(script.relative_to(root))))
        elif script:
            # A marker is an ASSIGNMENT to _pf_private_marker (the pinned form
            # is `_pf_private_marker = fixtures / "..." / "..."`): a file that
            # merely names the identifier -- a comment, a string the guard
            # suite reads back, this sweep's own probe -- does not carry a
            # marker, so the loud failure fires only on an assignment the
            # pinned form cannot be read out of.
            script_text = script.read_text(errors='replace')
            parts = marker_expression(script)
            assigned = re.search(r'^[^\S\n]*_pf_private_marker\s*=',
                                 script_text, re.M) is not None
            if assigned and parts is None:
                raise Unparseable(
                    f'{script}: {row} carries _pf_private_marker in a form '
                    f'this sweep cannot read (the pinned form is '
                    f'_pf_private_marker = fixtures / "..." / "...")')
            if parts is not None:
                fixtures_arg = table.fixtures_arg(row)
                if fixtures_arg is None:
                    raise Unparseable(
                        f'{script}: {row} has a marker but no add_test fixtures argument')
                arg = resolve_cmake_path(fixtures_arg, table.rel_dir).rstrip('/')
                marker = arg + '/' + '/'.join(parts)
                literal_lines = [ln for ln, line in
                                 enumerate(script.read_text(errors='replace').splitlines(), 1)
                                 if SKIP_LITERAL in line]
                markers.append((row, marker,
                                f'{script.relative_to(root)}:'
                                f'{literal_lines[0] if literal_lines else 0}'))

        for where, lineno, path in named:
            checked_paths += 1
            if is_removed(path, removed_dirs, removed_files):
                hits.setdefault(row, []).append((where, lineno, path))

    # ---- section 2: skip markers must name a removed file exactly (what the
    # private-fixtures overlay restores).
    for row, marker, where in markers:
        if marker not in removed_files:
            fail(f'{where}: {row}: the private skip marker {marker} is not a '
                 f'removed file: with the fixtures overlaid it stays absent, '
                 f'the row skips, and the overlay-mode stage refuses it')

    # ---- section 1: hits must be pinned rows, or register only under a
    # guard that names removed data (then they cannot run without it).
    resolved_guarded: set[str] = set()

    def condition_paths(condition: str, table: Rows) -> list[tuple[str, bool]]:
        """(path, from_exists) for every path a condition names."""
        out: list[tuple[str, bool]] = []
        env = table.vars
        for m in re.finditer(r'EXISTS\s+("(?:[^"]*)"|[^\s")]+)', condition):
            before = condition[:m.start()].rstrip()
            candidate = resolve_cmake_path(subst(m.group(1).strip('"'), env),
                                           table.rel_dir)
            out.append((candidate, not before.upper().endswith('NOT')))
        head = condition.strip()
        if not head or head.upper().startswith('NOT'):
            return out
        m = re.match(r'("(?:[^"]*)"|\$\{\w+\}[^\s")]*|[^\s")]+)', head)
        if m and not re.match(r'EXISTS\s', head):
            candidate = resolve_cmake_path(
                subst(m.group(1).strip('"'), env), table.rel_dir)
            if candidate:
                out.append((candidate, True))
        return out

    def site_guarded(site: dict, table: Rows) -> bool:
        """True when this registration site runs only with removed data
        present: a positive EXISTS guard naming it, and no enclosing condition
        that names removed data negatively (else()/NOT/elseif of it)."""
        guard = False
        for condition, positive in site['conditions']:
            for candidate, from_exists in condition_paths(condition, table):
                if not is_removed(candidate, removed_dirs, removed_files):
                    continue
                if positive and from_exists:
                    guard = True
                else:
                    return False
        return guard

    def guarded_entry(entry: dict) -> str | None:
        table = entry['table']
        sites = list(entry.get('sites') or [])
        # pineforge_add_twin_test registers the row through if(TARGET ...):
        # the row exists exactly when the target does, so the target's own
        # registration sites are the row's guard.
        for target in sorted(entry.get('twin_targets') or ()):
            target_entry = table.rows.get(target)
            if not target_entry or not target_entry.get('sites'):
                return None
            sites.extend(target_entry['sites'])
        if not sites:
            return None
        found = None
        for site in sites:
            if site['placeholder']:
                continue          # the pinned SKIP shape of the absent branch
            if not site['conditions'] and entry.get('twin_targets'):
                continue          # the twin call itself: the target's sites,
                # appended above, carry the guard the call inherits.
            if not site_guarded(site, table):
                return None       # a site that registers without the data
            found = found or site['conditions'][-1][0]
        return found

    for row in sorted(hits):
        if row in evidence:
            # Before the pin: the evidence record's whole point is that its
            # cited line IS re-read (load_evidence checked it once already),
            # so a record for a row the pin also covers is not dead weight.
            cite, token, reason = evidence[row]
            print(f'evidence hit: {row} names removed data ({hits[row][0][2]}); '
                  f'{cite} carries "{token}": {reason}')
            pinned.setdefault(row, 0)
            resolved_guarded.add(row)
            continue
        if row in pinned:
            continue
        guard = guarded_entry(rows[row])
        if guard:
            print(f'guarded hit: {row} registers only under {guard} '
                  f'and names {hits[row][0][2]}')
            pinned.setdefault(row, 0)   # counted as resolved below
            resolved_guarded.add(row)
            continue
        for where, lineno, path in hits[row][:4]:
            fail(f'{where}:{lineno}: {row} names the removed path {path} '
                 f'without being a pinned private-fixture row '
                 f'({ROW_PIN})')

    pinned_hits = sorted(row for row in hits if row in pinned)
    for row in pinned_hits:
        if row in resolved_guarded:
            continue
        where, lineno, path = hits[row][0]
        print(f'pinned hit: {row} ({ROW_PIN}:{pinned[row]}) reads {path} '
              f'({where}:{lineno})')

    # ---- section 3: every pinned row must have been swept: a pinned row no
    # registration matched means the sweep parsed fewer rows than are
    # registered.
    unswept = sorted(set(pinned) - set(rows) - resolved_guarded)
    for row in unswept:
        fail(f'{ROW_PIN}:{pinned[row]}: {row} is pinned but no registration '
             f'this sweep parsed names it: rows swept != rows registered')

    print(f'add_test sites walked: {add_test_sites} '
          f'(+{add_test_sites_skipped} function() templates) of {site_total}')
    print('list() subcommands (tests/CMakeLists.txt + runner/CMakeLists.txt):')
    unmodelled = 0
    for subcommand in sorted(Rows.LIST_MODELLED):
        print(f'  {subcommand:<12} {list_uses.get(subcommand, 0):>4} uses  '
              f'modelled yes')
    for subcommand in sorted(set(list_uses) - set(Rows.LIST_MODELLED)):
        unmodelled += 1
        print(f'  {subcommand:<12} {list_uses[subcommand]:>4} uses  '
              f'modelled no')
    print(f'list() subcommands not modelled: {unmodelled}')
    print(f'list() inputs not tracked: {len(unresolved_list_inputs)}')
    for where, lineno, subcommand, name, out in unresolved_list_inputs:
        print(f'  input not tracked: {where}:{lineno}: list({subcommand} '
              f'{name} {out}) -- {out} left unknown, never guessed')
    print(f'rows swept: {len(rows)}; pinned rows swept: '
          f'{pinned_total - len(unswept)} of {pinned_total}; '
          f'rows the sweep could not name: {len(dynamic_names)}')
    for where, lineno, raw in dynamic_names:
        print(f'  unnamed row: {where}:{lineno}: {raw} (swept under its raw '
              f'name; no registration resolves it)')
    print(f'named paths checked: {checked_paths}')
    print(f'rows naming removed data: {len(hits)} '
          f'(pinned {len(pinned_hits) - len(resolved_guarded)}, '
          f'guard-only {len(resolved_guarded)})')
    print(f'skip markers checked: {len(markers)}')
    print(f'row scripts not found: {len(missing_scripts)}')
    for row, where in missing_scripts:
        fail(f'{row} names the Python script {where} the tree does not have '
             f'at that path: its marker check cannot run')
    print(f'argv argument references that name no path: '
          f'{len(unresolved_argv_tokens)}')
    for row, where, lineno, element in unresolved_argv_tokens:
        print(f'  argument reference not a path: {where}:{lineno}: {row}: '
              f'{element}')
    for line in FAILURES:
        print(f'check_private_fixture_reads: {line}')
    if FAILURES:
        return 1
    unpinned_hits = [row for row in hits
                     if row not in pinned and row not in resolved_guarded]
    bad_markers = [marker for _, marker, _ in markers
                   if marker not in removed_files]
    print(f'unpinned rows naming removed data: {len(unpinned_hits)}')
    print(f'skip markers not naming a removed file: {len(bad_markers)}')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=REPO)
    args = parser.parse_args()
    try:
        return sweep(args.root)
    except Unparseable as error:
        print(f'check_private_fixture_reads: {error}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
