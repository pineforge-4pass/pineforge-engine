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
  * reported as a violation.

It also checks the class the overlay-mode round found (A-release,
rj-20261011t185619-d9567d): a Python tape driver whose private-marker guard
composes the fixture directory onto an add_test argument that already IS that
directory -- a marker that can never exist, so the row prints the pinned SKIP
literal and exits 77 in every mode, and the overlay-mode refusal stage blames
the overlay. The marker a driver tests must resolve to a REMOVED path: present
exactly when the private fixtures are overlaid.

Loud-fail: a construct it cannot parse (a missing pin file, a driver whose
marker expression it cannot read, a row whose add_test arguments it cannot
resolve) prints file:line and exits 2; it never counts what it cannot read as
clean.

Exit 0 when every hit is pinned and every marker names a removed path, 1 on a
violation, 2 on a construct it cannot parse.
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
    """True when the repo-relative path names removed data."""
    path = path.strip('/')
    if not path:
        return False
    if path in removed_files:
        return True
    parts = path.split('/')
    for i in range(1, len(parts) + 1):
        if '/'.join(parts[:i]) in removed_dirs:
            return True
    return False


def resolve_cmake_path(value: str, cmake_dir: str) -> str:
    return value.replace('${CMAKE_CURRENT_SOURCE_DIR}', cmake_dir).lstrip('/')


class Rows:
    """Registered ctest rows and what they name, per CMakeLists."""

    def __init__(self, path: Path, root: Path):
        self.path = path
        self.rel_dir = str(path.parent.relative_to(root))
        raw = path.read_text()
        self.text = strip_comments(raw)
        self.lines = self.text.splitlines()
        self.vars: dict[str, str] = {}
        self.rows: dict[str, dict] = {}
        self._parse()

    def resolve(self, token: str) -> str:
        token = token.strip('"')
        m = re.fullmatch(r'\$\{(_pf_\w+)\}', token)
        if m:
            if m.group(1) not in self.vars:
                raise Unparseable(f'{self.path}: ${{{m.group(1)}}} is not set')
            return self.vars[m.group(1)]
        return token

    def _entry(self, row: str, lineno: int, conditions: tuple = ()) -> dict:
        entry = self.rows.get(row)
        if entry is None:
            entry = {'defines': {}, 'lineno': lineno, 'argvs': [],
                     'conditions': []}
            self.rows[row] = entry
        elif not entry['lineno']:
            entry['lineno'] = lineno
        if conditions and conditions not in entry['conditions']:
            entry['conditions'].append(conditions)
        return entry

    def _condition_stack(self) -> dict[int, tuple]:
        """line index -> the enclosing top-level if() conditions."""
        stack: list[str] = []
        conditions: dict[int, tuple] = {}
        for index, line in enumerate(self.lines):
            stripped = line.strip()
            if stripped.startswith('endif('):
                if stack:
                    stack.pop()
                conditions[index] = tuple(stack)
                continue
            if stripped == 'else()' or stripped.startswith('elseif('):
                conditions[index] = tuple(stack)
                continue
            if not line.startswith((' ', '\t')) and re.match(r'^if\(', line):
                conditions[index] = tuple(stack)
                stack.append(line.strip())
                continue
            conditions[index] = tuple(stack)
        return conditions

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

    def _scan(self, text: str, base: int, cond_of: dict[int, tuple] | None = None) -> None:
        def lineno(index: int) -> int:
            return base + text[:index].count('\n')

        def conds(index: int) -> tuple:
            if cond_of is None:
                return ()
            return cond_of.get(lineno(index) - 1, ())

        for m in re.finditer(r'add_test\(NAME\s+(test_\w+)\s+COMMAND', text):
            row = m.group(1)
            body, _ = self._paren_body(text, text.index('(', m.start()))
            entry = self._entry(row, lineno(m.start()), conds(m.start()))
            entry['argvs'].append(body.split())
        for m in re.finditer(r'(?:pineforge_add_unit_test|add_executable)\((test_\w+)',
                             text):
            self._entry(m.group(1), lineno(m.start()), conds(m.start()))
        for launch in re.finditer(r'list\(APPEND TEST_SOURCES\b', text):
            body, _ = self._paren_body(text, text.index('(', launch.start()))
            for name in re.findall(r'(test_\w+)', body):
                self._entry(name, lineno(launch.start()), conds(launch.start()))
        for m in re.finditer(r'target_compile_definitions\((\S+?)\s+PRIVATE', text):
            target = m.group(1)
            row = target if target.startswith('test_') else None
            body, _ = self._paren_body(text, text.index('(', m.start()))
            if row is None:
                continue
            entry = self._entry(row, lineno(m.start()), conds(m.start()))
            for dm in re.finditer(r'(\w+)\s*=\s*"([^"]+)"', body):
                entry['defines'][dm.group(1)] = dm.group(2)

    def _parse(self) -> None:
        text = self.text
        for m in re.finditer(r'set\((_pf_\w+)\s+(\S+)\)', text):
            value = m.group(2).replace('${CMAKE_CURRENT_SOURCE_DIR}',
                                       self.rel_dir)
            self.vars.setdefault(m.group(1), value)
        cond_of = self._condition_stack()
        self._scan(text, 0, cond_of)
        # foreach(<var> IN ITEMS <items>) blocks register and define per item;
        # expand each item's substituted body and scan it too.
        for m in re.finditer(r'foreach\((\w+)\s+IN\s+ITEMS\s+([^)]*)\)', text):
            var, items = m.group(1), m.group(2).split()
            body_start = m.end()
            body_end = text.find('endforeach()', body_start)
            if body_end < 0:
                raise Unparseable('a bare foreach( with no endforeach()')
            body = text[body_start:body_end]
            base = text[:body_start].count('\n') + 1
            outer = cond_of.get(text[:m.start()].count('\n'), ())
            item_conds = {line: outer
                          for line in range(base - 1, base + body.count('\n') + 1)}
            for item in items:
                self._scan(re.sub(r'\$\{' + var + r'\}', item, body), base,
                           item_conds)

    def python_script(self, row: str) -> Path | None:
        entry = self.rows.get(row)
        if not entry:
            return None
        for argv in entry['argvs']:
            for token in argv:
                if token.strip('"').endswith('.py'):
                    return (self.path.parent / Path(token.strip('"')).name)
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
                    return self.resolve(token)
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
    text = driver.read_text(errors='replace')
    m = re.search(r'_pf_private_marker\s*=\s*fixtures\s*((?:/\s*"[^"]*"\s*)+)',
                  text)
    if not m:
        return None
    return re.findall(r'"([^"]*)"', m.group(1))


def sweep(root: Path) -> int:
    removed_dirs, removed_files = load_removed(root)
    pinned = load_pinned_rows(root)
    evidence = load_evidence(root)
    cmake_files = [root / 'tests' / 'CMakeLists.txt',
                   root / 'runner' / 'CMakeLists.txt']

    rows: dict[str, dict] = {}
    for cmake in cmake_files:
        if not cmake.exists():
            raise Unparseable(f'{cmake}: missing')
        table = Rows(cmake, root)
        for name, entry in table.rows.items():
            entry['cmake'] = cmake
            entry['table'] = table
            rows.setdefault(name, entry)

    hits: dict[str, list[tuple[str, int, str]]] = {}
    markers: list[tuple[str, str, str]] = []
    checked_paths = 0

    for row in sorted(rows):
        entry = rows[row]
        table: Rows = entry['table']
        define_values = [resolve_cmake_path(v, table.rel_dir)
                         for v in entry['defines'].values()]
        named: list[tuple[str, int, str]] = []   # (where, lineno, path)

        for value in define_values:
            named.append((str(table.path.relative_to(root)), 0, value))

        src = root / table.rel_dir / f'{row}.cpp'
        files = []
        if src.exists():
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
        # the add_test command's own path arguments
        for argv in entry['argvs']:
            for token in argv:
                if token.startswith(('tests/', 'fixtures/', '${')):
                    try:
                        value = table.resolve(token)
                    except Unparseable:
                        continue
                    named.append((str(table.path.relative_to(root)), entry['lineno'],
                                  resolve_cmake_path(value, table.rel_dir)))

        # Python driver: the private marker and the skip literal.
        script = table.python_script(row)
        if script and script.exists():
            parts = marker_expression(script)
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

    def condition_paths(condition: str, table: Rows) -> list[str]:
        out = []
        for m in re.finditer(r'\$\{(\w+)\}([^" )\n]*)', condition):
            var, suffix = m.group(1), m.group(2)
            if var == 'CMAKE_CURRENT_SOURCE_DIR':
                value = table.rel_dir
            else:
                value = table.vars.get(var)
            if value is None:
                continue
            out.append(resolve_cmake_path(value, table.rel_dir).rstrip('/') + suffix)
        for m in re.finditer(r'"([^"]+)"', condition):
            cand = re.sub(r'\$\{(\w+)\}',
                          lambda mm: table.vars.get(mm.group(1), mm.group(0)),
                          m.group(1))
            out.append(resolve_cmake_path(cand, table.rel_dir))
        return out

    def guarded_entry(entry: dict) -> str | None:
        sites = entry.get('conditions') or []
        if not sites:
            return None
        table = entry['table']
        found = None
        for site in sites:
            guard = None
            for condition in site:
                for cand in condition_paths(condition, table):
                    if is_removed(cand, removed_dirs, removed_files):
                        guard = condition
                        break
                if guard:
                    break
            if guard is None:
                return None          # a site without the removed-data guard
            found = found or guard
        return found

    for row in sorted(hits):
        if row in pinned:
            continue
        guard = guarded_entry(rows[row])
        if guard:
            print(f'guarded hit: {row} registers only under {guard} '
                  f'and names {hits[row][0][2]}')
            pinned.setdefault(row, 0)   # counted as resolved below
            resolved_guarded.add(row)
            continue
        if row in evidence:
            cite, token, reason = evidence[row]
            print(f'evidence hit: {row} names removed data ({hits[row][0][2]}); '
                  f'{cite} carries "{token}": {reason}')
            pinned.setdefault(row, 0)
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

    print(f'rows swept: {len(rows)}')
    print(f'named paths checked: {checked_paths}')
    print(f'rows naming removed data: {len(hits)} '
          f'(pinned {len(pinned_hits) - len(resolved_guarded)}, '
          f'guard-only {len(resolved_guarded)})')
    print(f'skip markers checked: {len(markers)}')
    for line in FAILURES:
        print(f'check_private_fixture_reads: {line}')
    if FAILURES:
        return 1
    print('unpinned rows naming removed data: 0')
    print('skip markers not naming a removed file: 0')
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
