#!/usr/bin/env python3
"""CR-150 / C4 source allocation-bound gate.

Scans the declared C++ file set (engine ``src/**``, the public inline headers
and the generated C++ present in the tree), indexes every allocation-bearing
construct (growth calls, new/malloc, by-value container declarations,
temporaries, map subscripts, container assignment, string append) and compares
the index with ``ci/cr150-source-bounds.yaml``.

Exit 0: no finding. Exit 1: a finding (an unlisted site, a stale entry, a
bounded entry covering a computed reserve/resize, a ceiling or pin drift) or,
with ``--strict``, any OPEN entry left. Exit 2: the gate cannot judge (missing
or malformed manifest, an entry whose bound is not a fixed pre-bar-0 constant
expression, zero files, zero sites, a broken pattern set).

An entry is ``bounded`` only with a bound expression over declared fixed
inputs; it counts as PASS(bounded). An entry whose flat-memory implementation
has not landed is ``pending-flat-memory`` (inventory row and growth axis
named) or ``unreviewed`` (nobody has established a bound). Both are OPEN: they
are listed and counted, never PASS, and ``--strict`` fails on them. A green
gate means nothing NEW and unbounded entered the scanned set; it is not a
flat-memory proof. The scan is a regex index over lexed C++ (comments and
string literals removed): not alias, type or call-path proof.
"""
from __future__ import annotations

import argparse
import fnmatch
import glob
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cxxlex  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_REL = 'ci/cr150-source-bounds.yaml'
SCHEMA = 'cr150-source-bounds/v1'
PREFIX = 'cr150_source_gate:'

OP_CLASSES = ('grow', 'alloc', 'decl', 'temp', 'subscript', 'assign', 'append')
PHASES = ('setup', 'bar', 'report', 'teardown', 'unclassified')
STATES = ('bounded', 'pending-flat-memory', 'unreviewed')
OPEN_STATES = ('pending-flat-memory', 'unreviewed')
# A bound may not name a growth axis. Substring match, case-insensitive, on
# every identifier of a bound expression and on every fixed-input name.
FORBIDDEN_FRAGMENTS = ('bar', 'trade', 'entr', 'decision', 'histor', 'event', 'tick', 'candle')
PLACEHOLDERS = {'', 'todo', 'tbd', 'fixme', '?', '-', 'n/a', 'na', 'none', 'unknown', 'xxx'}
# The engine tree the manifest may not narrow: every source file under these
# globs must be in the scanned set, or editing scan.files could switch the gate
# off for a file.
ENGINE_FLOOR = ('src/**/*.cpp', 'src/**/*.hpp', 'src/**/*.inc', 'include/pineforge/**/*.hpp',
                'include/pineforge/**/*.h')
SOURCE_SUFFIXES = {'.cpp', '.cc', '.cxx', '.c', '.hpp', '.hh', '.h', '.inc', '.ipp', '.tpp'}

GROW_OPS = ('push_back', 'emplace_back', 'push_front', 'emplace_front', 'emplace',
            'emplace_hint', 'insert', 'insert_or_assign', 'try_emplace', 'resize',
            'reserve', 'assign', 'append')
SIZED_OPS = ('reserve', 'resize', 'assign')
STD_CONTAINERS = ('vector', 'deque', 'list', 'forward_list', 'map', 'multimap',
                  'unordered_map', 'unordered_multimap', 'set', 'multiset', 'unordered_set',
                  'unordered_multiset', 'string', 'wstring', 'u8string', 'u16string',
                  'u32string', 'basic_string', 'ostringstream', 'stringstream',
                  'istringstream', 'basic_ostringstream', 'queue', 'stack',
                  'priority_queue', 'valarray')
STD_WRAPPERS = ('optional', 'pair', 'tuple', 'unique_ptr', 'shared_ptr', 'variant', 'array')
MAP_TYPES = ('map', 'multimap', 'unordered_map', 'unordered_multimap')
STRING_TYPES = ('string', 'wstring', 'u8string', 'u16string', 'u32string', 'basic_string',
                'ostringstream', 'stringstream', 'basic_ostringstream')
# Growable wrappers this repository defines (include/pineforge).
CUSTOM_CONTAINERS = ('Series', 'DynamicRingBuffer', 'PineMap', 'NumericMatrix',
                     'GenericMatrix', 'DrawingRecordStore')
CUSTOM_MAPS = ('PineMap',)

GROW_RE = re.compile(r'(?:\.|->)\s*(' + '|'.join(GROW_OPS) + r')\s*(?:<[^<>;]*>)?\s*\(')
NEW_RE = re.compile(r'(?<![\w.])new\b')
CALLOC_RE = re.compile(r'(?<![\w.>])(?:std::)?(malloc|calloc|realloc|aligned_alloc|'
                       r'posix_memalign|strdup|strndup)\s*\(')
MAKE_RE = re.compile(r'\bstd::(make_unique|make_shared|allocate_shared)\b')
TO_STRING_RE = re.compile(r'\bstd::to_string\s*\(')
OPERATOR_RE = re.compile(r'(?<!\w)([A-Za-z_]\w*)\s*(\[|\+=|<<(?!=)|=(?!=))')
USING_RE = re.compile(r'\busing\s+(\w+)\s*=\s*(?:typename\s+)?(?:const\s+)?'
                      r'(?:(?:std::)?(?:' + '|'.join(STD_CONTAINERS) + r')\b|(?:\w+::)*(?:'
                      + '|'.join(CUSTOM_CONTAINERS) + r')\b)')
IDENT_RE = re.compile(r'[A-Za-z_]\w*')
KEYWORDS = {'operator', 'return', 'const', 'static', 'if', 'for', 'while', 'new', 'delete',
            'noexcept', 'override', 'final', 'throw', 'sizeof', 'typename', 'template'}

BLANK_CHARS = ' \t\r\n'


class GateError(Exception):
    """The gate cannot judge: exit 2."""


# --------------------------------------------------------------------------
# restricted YAML (maps, lists, flow maps/lists, quoted/plain scalars)
# --------------------------------------------------------------------------

def _strip_comment(line: str) -> str:
    quote = ''
    for i, ch in enumerate(line):
        if quote:
            if ch == '\\' and quote == '"':
                continue
            if ch == quote:
                quote = ''
        elif ch in '"\'':
            quote = ch
        elif ch == '#' and (i == 0 or line[i - 1] in ' \t'):
            return line[:i]
    return line


def _split_top(text: str, sep: str = ',') -> list[str]:
    parts, depth, quote, cur, i = [], 0, '', [], 0
    while i < len(text):
        ch = text[i]
        if quote:
            cur.append(ch)
            if ch == '\\' and quote == '"' and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 1
            elif ch == quote:
                quote = ''
        elif ch in '"\'':
            quote = ch
            cur.append(ch)
        elif ch in '[{':
            depth += 1
            cur.append(ch)
        elif ch in ']}':
            depth -= 1
            cur.append(ch)
        elif ch == sep and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
        i += 1
    if quote or depth:
        raise GateError(f'unbalanced quote or bracket in manifest text: {text[:80]!r}')
    parts.append(''.join(cur))
    return [p.strip() for p in parts]


def _scalar(text: str):
    text = text.strip()
    if text.startswith('"'):
        if len(text) < 2 or not text.endswith('"'):
            raise GateError(f'bad quoted scalar: {text[:80]!r}')
        return re.sub(r'\\(.)', r'\1', text[1:-1])
    if text.startswith("'"):
        if len(text) < 2 or not text.endswith("'"):
            raise GateError(f'bad quoted scalar: {text[:80]!r}')
        return text[1:-1]
    if text.startswith('['):
        if not text.endswith(']'):
            raise GateError(f'bad flow list: {text[:80]!r}')
        inner = text[1:-1].strip()
        return [] if not inner else [_scalar(p) for p in _split_top(inner)]
    if text.startswith('{'):
        return _flow_map(text)
    if text in ('true', 'false'):
        return text == 'true'
    if text in ('null', '~', ''):
        return None
    if re.fullmatch(r'-?\d+', text):
        return int(text)
    return text


def _flow_map(text: str) -> dict:
    text = text.strip()
    if not (text.startswith('{') and text.endswith('}')):
        raise GateError(f'bad flow map: {text[:80]!r}')
    inner = text[1:-1].strip()
    result: dict = {}
    for part in ([] if not inner else _split_top(inner)):
        match = re.match(r'([A-Za-z_][\w\-]*)\s*:\s*(.*)$', part, re.S)
        if not match:
            raise GateError(f'bad flow-map member: {part[:80]!r}')
        if match.group(1) in result:
            raise GateError(f'duplicate key {match.group(1)!r} in flow map')
        result[match.group(1)] = _scalar(match.group(2))
    return result


def parse_yaml(text: str):
    rows = []
    for number, raw in enumerate(text.split('\n'), 1):
        if '\t' in raw[:len(raw) - len(raw.lstrip())]:
            raise GateError(f'manifest line {number}: tab indentation')
        line = _strip_comment(raw).rstrip()
        if line.strip():
            rows.append((len(line) - len(line.lstrip(' ')), line.strip(), number))
    if not rows:
        raise GateError('manifest is empty')

    def block(i: int, indent: int):
        if rows[i][1].startswith('- ') or rows[i][1] == '-':
            return lst(i, indent)
        return mapping(i, indent)

    def lst(i: int, indent: int):
        items = []
        while i < len(rows) and rows[i][0] == indent and (rows[i][1].startswith('- ')
                                                          or rows[i][1] == '-'):
            body = rows[i][1][1:].strip()
            if not body:
                raise GateError(f'manifest line {rows[i][2]}: empty list item')
            if body.startswith('{'):
                items.append(_flow_map(body))
            elif re.match(r'[A-Za-z_][\w\-]*\s*:(\s|$)', body):
                raise GateError(f'manifest line {rows[i][2]}: block-map list items are '
                                'not supported; use a flow map {k: v, ...}')
            else:
                items.append(_scalar(body))
            i += 1
        if i < len(rows) and rows[i][0] > indent:
            raise GateError(f'manifest line {rows[i][2]}: unexpected indentation')
        return items, i

    def mapping(i: int, indent: int):
        result: dict = {}
        while i < len(rows) and rows[i][0] == indent:
            number, body = rows[i][2], rows[i][1]
            match = re.match(r'([A-Za-z_][\w\-]*)\s*:(?:\s+(.*))?$', body)
            if not match:
                raise GateError(f'manifest line {number}: expected "key: value", got {body[:60]!r}')
            key, rest = match.group(1), match.group(2)
            if key in result:
                raise GateError(f'manifest line {number}: duplicate key {key!r}')
            i += 1
            if rest is not None and rest != '':
                result[key] = _scalar(rest)
            elif i < len(rows) and rows[i][0] > indent:
                inner = rows[i][1]
                if inner in ('[]', '{}'):
                    result[key] = [] if inner == '[]' else {}
                    i += 1
                else:
                    result[key], i = block(i, rows[i][0])
            else:
                result[key] = None
        if i < len(rows) and rows[i][0] > indent:
            raise GateError(f'manifest line {rows[i][2]}: unexpected indentation')
        return result, i

    value, end = block(0, rows[0][0])
    if end != len(rows):
        raise GateError(f'manifest line {rows[end][2]}: trailing content')
    return value


# --------------------------------------------------------------------------
# bound expressions
# --------------------------------------------------------------------------

def _forbidden(identifier: str) -> str | None:
    low = identifier.lower()
    for fragment in FORBIDDEN_FRAGMENTS:
        if fragment in low:
            return fragment
    return None


def bound_identifiers(expr: str, symbol: str, who: str) -> set[str]:
    """Parse ``expr``; return its identifiers. Only integer literals, names,
    + - * / ( ) and min/max are allowed: member access or any other call is a
    tautology risk (a container's own size) and is refused."""
    if re.search(r'\.|->|\[|\]', expr):
        raise GateError(f'{who}: bound {expr!r} uses member access or a subscript: a container\'s '
                        'own size/capacity is a tautology, not a bound')
    tokens = re.findall(r'\s*(0[xX][0-9a-fA-F]+|\d+|[A-Za-z_]\w*|[-+*/(),]|\S)', expr)
    if not tokens or ''.join(tokens) != re.sub(r'\s+', '', expr):
        raise GateError(f'{who}: bound {expr!r} is empty or has unsupported characters')
    names: set[str] = set()
    pos = 0

    def peek():
        return tokens[pos] if pos < len(tokens) else None

    def take(expected=None):
        nonlocal pos
        token = peek()
        if token is None or (expected is not None and token != expected):
            raise GateError(f'{who}: bound {expr!r}: expected {expected or "a term"}')
        pos += 1
        return token

    def expression():
        term()
        while peek() in ('+', '-', '*', '/'):
            take()
            term()

    def term():
        token = take()
        if token == '(':
            expression()
            take(')')
        elif re.fullmatch(r'0[xX][0-9a-fA-F]+|\d+', token):
            return
        elif IDENT_RE.fullmatch(token):
            if peek() == '(':
                if token not in ('min', 'max'):
                    raise GateError(f'{who}: bound {expr!r} calls {token}(): call/member '
                                    'syntax can only be a size tautology; only min/max are allowed')
                take('(')
                expression()
                take(',')
                expression()
                take(')')
            else:
                names.add(token)
        else:
            raise GateError(f'{who}: bound {expr!r}: unexpected {token!r}')

    expression()
    if pos != len(tokens):
        raise GateError(f'{who}: bound {expr!r}: trailing {tokens[pos]!r}')
    names -= {'min', 'max'}
    for name in sorted(names):
        axis = _forbidden(name)
        if axis:
            raise GateError(f'{who}: bound {expr!r} names {name!r}: forbidden growth axis '
                            f'({axis}); a bound must be independent of bars/trades/entries/'
                            'decisions/history')
        if name == symbol:
            raise GateError(f'{who}: bound {expr!r} names the container itself ({symbol!r}): '
                            'tautology, not a bound')
    return names


# --------------------------------------------------------------------------
# manifest validation
# --------------------------------------------------------------------------

def _need(mapping: dict, keys: tuple, who: str, allowed: tuple = ()) -> None:
    missing = [k for k in keys if k not in mapping or mapping[k] is None]
    if missing:
        raise GateError(f'{who}: missing required field(s) {", ".join(missing)}')
    extra = sorted(set(mapping) - set(keys) - set(allowed))
    if extra:
        raise GateError(f'{who}: unknown field(s) {", ".join(extra)}')


def _text(value, who: str, field: str, *, placeholder_ok: bool = False) -> str:
    if not isinstance(value, str) or (not placeholder_ok and value.strip().lower() in PLACEHOLDERS):
        raise GateError(f'{who}: field {field} is empty or a placeholder ({value!r})')
    return value


def _str_list(value, who: str, field: str, *, nonempty: bool = True) -> list[str]:
    if not isinstance(value, list) or (nonempty and not value) \
            or not all(isinstance(v, str) and v for v in value):
        raise GateError(f'{who}: field {field} must be a {"non-empty " if nonempty else ""}'
                        'list of strings')
    return value


class Manifest:
    def __init__(self, data: dict, root: Path):
        if not isinstance(data, dict):
            raise GateError('manifest must be a mapping')
        _need(data, ('schema', 'codegen', 'scan', 'ceilings', 'owners', 'fixed_inputs',
                     'entries'), 'manifest')
        if data['schema'] != SCHEMA:
            raise GateError(f'manifest schema {data["schema"]!r} != {SCHEMA!r}')
        self.root = root
        self.data = data
        self._codegen(data['codegen'])
        scan = data['scan']
        if not isinstance(scan, dict):
            raise GateError('scan must be a mapping')
        _need(scan, ('files', 'generated_files'), 'scan')
        self.files = _str_list(scan['files'], 'scan', 'files')
        self.generated = _str_list(scan['generated_files'], 'scan', 'generated_files',
                                   nonempty=False)
        ceilings = data['ceilings']
        if not isinstance(ceilings, dict):
            raise GateError('ceilings must be a mapping')
        _need(ceilings, ('unreviewed', 'pending-flat-memory'), 'ceilings')
        for key, value in ceilings.items():
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise GateError(f'ceilings.{key} must be a non-negative integer')
        self.ceilings = ceilings
        self._owners(data['owners'])
        self._fixed(data['fixed_inputs'])
        self._entries(data['entries'])

    def _codegen(self, codegen) -> None:
        if not isinstance(codegen, dict):
            raise GateError('codegen must be a mapping')
        _need(codegen, ('pin', 'pin_source', 'tree', 'emitter_templates'), 'codegen')
        if not re.fullmatch(r'[0-9a-f]{40}', str(codegen['pin'])):
            raise GateError('codegen.pin must be a 40-hex commit')
        if codegen['tree'] != 'absent':
            raise GateError('codegen.tree: only "absent" is supported by this gate version '
                            '(no codegen checkout is scanned); declaring a tree without scan '
                            'support would silently skip it')
        source = self.root / str(codegen['pin_source'])
        if not source.is_file():
            raise GateError(f'codegen.pin_source {codegen["pin_source"]!r} is not a file')
        self.pin, self.pin_source = codegen['pin'], str(codegen['pin_source'])
        templates = codegen['emitter_templates']
        if not isinstance(templates, list) or not templates:
            raise GateError('codegen.tree is absent, so a declared emitter template list is '
                            'required and it is empty')
        self.templates = []
        for template in templates:
            who = f'emitter template {template.get("id", "?") if isinstance(template, dict) else "?"}'
            if not isinstance(template, dict):
                raise GateError(f'{who}: must be a flow map')
            state = template.get('state')
            if state not in OPEN_STATES:
                raise GateError(f'{who}: state must be one of {OPEN_STATES} (a template '
                                'list without a codegen tree can prove no bound)')
            required = ['id', 'emits', 'owner', 'phase', 'state']
            if state == 'pending-flat-memory':
                required += ['grows_with', 'inventory']
            _need(template, tuple(required), who, ('inventory', 'grows_with', 'note'))
            self.templates.append(template)

    def _owners(self, owners) -> None:
        if not isinstance(owners, list) or not owners:
            raise GateError('owners must be a non-empty list')
        self.owner_globs: list[tuple[str, str]] = []
        self.owner_names: set[str] = set()
        for owner in owners:
            if not isinstance(owner, dict):
                raise GateError('owners entries must be flow maps')
            _need(owner, ('name', 'globs'), 'owner')
            if owner['name'] in self.owner_names:
                raise GateError(f'duplicate owner {owner["name"]!r}')
            self.owner_names.add(_text(owner['name'], 'owner', 'name'))
            for pattern in _str_list(owner['globs'], f'owner {owner["name"]}', 'globs'):
                self.owner_globs.append((pattern, owner['name']))

    def owner_of(self, rel: str) -> str | None:
        for pattern, name in self.owner_globs:
            if fnmatch.fnmatchcase(rel, pattern):
                return name
        return None

    def _fixed(self, fixed) -> None:
        if not isinstance(fixed, list):
            raise GateError('fixed_inputs must be a list')
        self.fixed: dict[str, dict] = {}
        for item in fixed:
            who = f'fixed input {item.get("name", "?") if isinstance(item, dict) else "?"}'
            if not isinstance(item, dict):
                raise GateError(f'{who}: must be a flow map')
            _need(item, ('name', 'definition', 'fixed_before_bar0'), who, ('value', 'note'))
            name = item['name']
            if not isinstance(name, str) or not IDENT_RE.fullmatch(name):
                raise GateError(f'{who}: name is not an identifier')
            if name in self.fixed:
                raise GateError(f'{who}: declared twice')
            if item['fixed_before_bar0'] is not True:
                raise GateError(f'{who}: fixed_before_bar0 must be true')
            axis = _forbidden(name)
            if axis:
                raise GateError(f'{who}: name carries the growth axis {axis!r}; a fixed input '
                                'must not be named after bars/trades/entries/decisions/history')
            definition = self.root / str(item['definition'])
            if not definition.is_file():
                raise GateError(f'{who}: definition file {item["definition"]!r} not found')
            body = definition.read_text(errors='replace')
            if not re.search(rf'\b{re.escape(name)}\b', body):
                raise GateError(f'{who}: {name!r} does not occur in {item["definition"]}')
            if 'value' in item:
                if not isinstance(item['value'], int):
                    raise GateError(f'{who}: value must be an integer')
                if not re.search(rf'\b{re.escape(name)}\b[^;=]*=\s*{item["value"]}\b', body):
                    raise GateError(f'{who}: {item["definition"]} does not define '
                                    f'{name} = {item["value"]}')
            self.fixed[name] = item

    def _entries(self, entries) -> None:
        if not isinstance(entries, list):
            raise GateError('entries must be a list')
        self.entries: list[dict] = []
        self.by_key: dict[tuple[str, str], dict] = {}
        ids: set[str] = set()
        for entry in entries:
            who = f'entry {entry.get("id", "?") if isinstance(entry, dict) else "?"}'
            if not isinstance(entry, dict):
                raise GateError(f'{who}: must be a flow map')
            state = entry.get('state')
            if state not in STATES:
                raise GateError(f'{who}: state {state!r} not in {STATES}')
            required = ['id', 'file', 'symbol', 'ops', 'container', 'owner', 'phase', 'state']
            optional = ['line', 'note', 'inventory', 'grows_with']
            if state == 'bounded':
                required += ['bound', 'bound_inputs', 'lifetime', 'rollback', 'hash_reader',
                             'tests']
                optional.remove('grows_with')
                optional.remove('inventory')
                optional.append('inventory')
            elif state == 'pending-flat-memory':
                required += ['grows_with', 'inventory']
            _need(entry, tuple(required), who, tuple(optional))
            if entry['id'] in ids:
                raise GateError(f'{who}: duplicate id')
            ids.add(entry['id'])
            for field in ('file', 'symbol', 'container'):
                _text(entry[field], who, field, placeholder_ok=True)
            _text(entry['owner'], who, 'owner')
            if entry['owner'] not in self.owner_names:
                raise GateError(f'{who}: owner {entry["owner"]!r} is not in the owners list')
            ops = _str_list(entry['ops'], who, 'ops')
            if not set(ops) <= set(OP_CLASSES):
                raise GateError(f'{who}: ops {ops} not within {OP_CLASSES}')
            if entry['phase'] not in PHASES:
                raise GateError(f'{who}: phase {entry["phase"]!r} not in {PHASES}')
            if (entry['phase'] == 'unclassified') != (state == 'unreviewed'):
                raise GateError(f'{who}: phase "unclassified" is for state "unreviewed" only '
                                'and every unreviewed entry carries it')
            if state != 'bounded' and entry.get('bound') not in (None, 'none'):
                raise GateError(f'{who}: only a bounded entry may carry a bound')
            if state == 'pending-flat-memory':
                _str_list(entry['grows_with'], who, 'grows_with')
                if not re.fullmatch(r'[A-Z]\d{2}', str(entry['inventory'])):
                    raise GateError(f'{who}: inventory must be an A-section row id (e.g. R01)')
            if state == 'bounded':
                self._bounded(entry, who)
            key = (entry['file'], entry['symbol'])
            if key in self.by_key:
                raise GateError(f'{who}: duplicate (file, symbol) key {key}; '
                                f'already {self.by_key[key]["id"]}')
            self.by_key[key] = entry
            self.entries.append(entry)

    def _bounded(self, entry: dict, who: str) -> None:
        for field in ('lifetime', 'rollback', 'hash_reader', 'tests'):
            _text(entry[field], who, field)
        expr = _text(entry['bound'], who, 'bound')
        inputs = _str_list(entry['bound_inputs'], who, 'bound_inputs')
        names = bound_identifiers(expr, entry['symbol'], who)
        for name in sorted(set(inputs) | names):
            if name not in self.fixed:
                raise GateError(f'{who}: bound input {name!r} is not a declared fixed input '
                                '(fixed_inputs)')
        if names != set(inputs):
            raise GateError(f'{who}: bound_inputs {sorted(inputs)} != identifiers of the '
                            f'bound expression {sorted(names)}')
        if not names:
            raise GateError(f'{who}: a bound must name at least one declared fixed input')


def load_manifest(root: Path, path: Path) -> Manifest:
    if not path.is_file():
        raise GateError(f'manifest {path} is missing (the gate fails closed)')
    return Manifest(parse_yaml(path.read_text()), root)


# --------------------------------------------------------------------------
# scanner
# --------------------------------------------------------------------------

class Site:
    __slots__ = ('file', 'line', 'cls', 'symbol', 'text', 'computed', 'container')

    def __init__(self, file, line, cls, symbol, text, computed=False, container=''):
        self.file, self.line, self.cls, self.symbol = file, line, cls, symbol
        self.text, self.computed, self.container = text, computed, container

    @property
    def key(self):
        return (self.file, self.symbol)


def _match_angle(code: str, start: int) -> int | None:
    """Index just past the ``>`` matching the ``<`` at ``start``."""
    depth = 0
    for i in range(start, min(len(code), start + 600)):
        ch = code[i]
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
            if depth == 0:
                return i + 1
        elif ch in ';{}':
            return None
    return None


def _match_paren(code: str, start: int) -> int | None:
    depth = 0
    for i in range(start, min(len(code), start + 400)):
        if code[i] == '(':
            depth += 1
        elif code[i] == ')':
            depth -= 1
            if depth == 0:
                return i
    return None


def _receiver(code: str, pos: int) -> str:
    j = pos
    for _ in range(8):
        while j > 0 and code[j - 1] in BLANK_CHARS:
            j -= 1
        if j > 0 and code[j - 1] in ')]':
            close = code[j - 1]
            opener = '(' if close == ')' else '['
            depth, k = 0, j - 1
            while k >= 0 and j - k < 400:
                if code[k] == close:
                    depth += 1
                elif code[k] == opener:
                    depth -= 1
                    if depth == 0:
                        break
                k -= 1
            if k < 0 or depth:
                return '<expr>'
            j = k
            continue
        break
    end = j
    while j > 0 and (code[j - 1].isalnum() or code[j - 1] == '_'):
        j -= 1
    return code[j:end] or '<expr>'


def _assigned_name(code: str, pos: int) -> str | None:
    window = code[max(0, pos - 120):pos]
    match = re.search(r'([A-Za-z_]\w*)\s*(?:=|\()\s*(?:\([^()]*\)\s*)?(?:std::)?$', window)
    return match.group(1) if match and match.group(1) not in KEYWORDS else None


class Scanner:
    def __init__(self, fixed_names: set[str]):
        self.fixed_names = fixed_names

    def _type_re(self, aliases: set[str]) -> re.Pattern:
        std = '|'.join(STD_CONTAINERS + STD_WRAPPERS)
        custom = '|'.join(CUSTOM_CONTAINERS)
        alias = ('|' + '|'.join(sorted(map(re.escape, aliases)))) if aliases else ''
        return re.compile(r'(?<![\w])(?:std::(?P<std>' + std + r')|(?:\w+::)*(?P<custom>'
                          + custom + r')' + (r'|(?P<alias>' + alias[1:] + r')' if aliases else '')
                          + r')\b(?!_)')

    @staticmethod
    def _lex(text: str) -> tuple[str, list[int]]:
        code = '\n'.join(part[0] for part in cxxlex.lex_lines(text))
        starts, offset = [0], 0
        for line in code.split('\n'):
            offset += len(line) + 1
            starts.append(offset)
        return code, starts

    @staticmethod
    def _line(starts: list[int], pos: int) -> int:
        lo, hi = 0, len(starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if starts[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def _is_fixed_arg(self, arg: str) -> bool:
        stripped = re.sub(r'\bsizeof\b', '', arg)
        names = set(IDENT_RE.findall(re.sub(r'\b\d[\w]*', '', stripped)))
        return names <= self.fixed_names and bool(re.sub(r'\s', '', stripped))

    def decls(self, rel: str, code: str, starts: list[int], type_re: re.Pattern):
        """Container declarations and temporaries; (sites, map syms, string syms,
        container syms). Template arguments are consumed, never rescanned."""
        sites, maps, strings, containers = [], set(), set(), set()
        pos = 0
        while True:
            match = type_re.search(code, pos)
            if not match:
                break
            end = match.end()
            kind = match.group('std') or match.group('custom') or match.group('alias') or ''
            inner = ''
            probe = end
            while probe < len(code) and code[probe] in BLANK_CHARS:
                probe += 1
            if probe < len(code) and code[probe] == '<':
                close = _match_angle(code, probe)
                if close is None:
                    pos = end
                    continue
                inner = code[probe:close]
                end = close
            pos = end
            if kind in STD_WRAPPERS and not re.search(
                    r'\b(?:' + '|'.join(STD_CONTAINERS) + '|' + '|'.join(CUSTOM_CONTAINERS)
                    + r')\b', inner):
                continue
            rest = end
            while rest < len(code) and code[rest] in BLANK_CHARS:
                rest += 1
            suffix_start = rest
            owning = True
            while True:
                word = re.match(r'(?:const|volatile)\b', code[rest:rest + 9])
                if word:
                    rest += word.end()
                    while rest < len(code) and code[rest] in BLANK_CHARS:
                        rest += 1
                    continue
                if rest < len(code) and code[rest] in '&*':
                    owning = False
                    break
                break
            if not owning or rest >= len(code):
                continue
            head = code[max(0, match.start() - 12):match.start()]
            line = self._line(starts, match.start())
            text = code[match.start():end].replace('\n', ' ').strip()
            if code[rest] in '({' and suffix_start == rest:
                sites.append(Site(rel, line, 'temp', f'<temp:{kind}>', text))
                continue
            ident = IDENT_RE.match(code, rest)
            if not ident or ident.group(0) in KEYWORDS:
                continue
            if re.search(r'\btypedef\s*$', head):
                continue
            after = ident.end()
            while after < len(code) and code[after] in BLANK_CHARS:
                after += 1
            if after < len(code) and code[after] == ':' and code[after:after + 2] == '::':
                continue
            if after >= len(code) or code[after] not in ';=({,)[':
                continue
            symbol = ident.group(0)
            sites.append(Site(rel, line, 'decl', symbol, text, container=text))
            containers.add(symbol)
            if kind in MAP_TYPES or kind in CUSTOM_MAPS:
                maps.add(symbol)
            if kind in STRING_TYPES or 'string' in inner:
                strings.add(symbol)
        return sites, maps, strings, containers

    def scan(self, texts: dict[str, str]):
        lexed = {rel: self._lex(text) for rel, text in sorted(texts.items())}
        aliases = set()
        for code, _ in lexed.values():
            aliases.update(m.group(1) for m in USING_RE.finditer(code))
        type_re = self._type_re(aliases)
        sites: list[Site] = []
        per_file = {}
        members_map, members_str, members_all = set(), set(), set()
        for rel, (code, starts) in lexed.items():
            found, maps, strings, containers = self.decls(rel, code, starts, type_re)
            per_file[rel] = (found, maps, strings, containers)
            sites.extend(found)
            members_map |= {s for s in maps if s.endswith('_')}
            members_str |= {s for s in strings if s.endswith('_')}
            members_all |= {s for s in containers if s.endswith('_')}
        for rel, (code, starts) in lexed.items():
            found, maps, strings, containers = per_file[rel]
            decl_pos = {(s.line, s.symbol) for s in found if s.cls == 'decl'}
            for m in GROW_RE.finditer(code):
                op = m.group(1)
                symbol = _receiver(code, m.start())
                computed = False
                if op in SIZED_OPS:
                    open_at = m.end() - 1
                    close = _match_paren(code, open_at)
                    arg = code[open_at + 1:close] if close else code[open_at + 1:open_at + 80]
                    first = _split_top(arg)[0] if arg.strip() else ''
                    computed = not self._is_fixed_arg(first)
                sites.append(Site(rel, self._line(starts, m.start()), 'grow', symbol,
                                  f'{symbol}.{op}(...)', computed))
            for m in NEW_RE.finditer(code):
                before = code[max(0, m.start() - 12):m.start()]
                after = code[m.end():m.end() + 2].strip()
                if before.rstrip().endswith('<') or after.startswith('>'):
                    continue  # #include <new>, std::nothrow_t templates: not an allocation
                if re.search(r'operator\s*$', before):
                    # a user-defined allocation function (class-specific operator new)
                    sites.append(Site(rel, self._line(starts, m.start()), 'alloc', 'operator new',
                                      'operator new'))
                    continue
                typename = re.match(r'\s*([\w:]+)', code[m.end():m.end() + 80])
                symbol = _assigned_name(code, m.start()) or f'new:{typename.group(1) if typename else "?"}'
                sites.append(Site(rel, self._line(starts, m.start()), 'alloc', symbol,
                                  f'new {typename.group(1) if typename else ""}'))
            for m in CALLOC_RE.finditer(code):
                symbol = _assigned_name(code, m.start()) or m.group(1)
                sites.append(Site(rel, self._line(starts, m.start()), 'alloc', symbol, m.group(1)))
            for m in MAKE_RE.finditer(code):
                symbol = _assigned_name(code, m.start()) or m.group(1)
                sites.append(Site(rel, self._line(starts, m.start()), 'alloc', symbol, m.group(1)))
            for m in TO_STRING_RE.finditer(code):
                sites.append(Site(rel, self._line(starts, m.start()), 'temp', 'std::to_string',
                                  'std::to_string(...)'))
            map_syms, str_syms, all_syms = maps | members_map, strings | members_str, \
                containers | members_all
            for m in OPERATOR_RE.finditer(code):
                name, op = m.group(1), m.group(2)
                line = self._line(starts, m.start(1))
                if op == '[' and name in map_syms:
                    sites.append(Site(rel, line, 'subscript', name, f'{name}[...]'))
                elif op == '=' and name in all_syms and (line, name) not in decl_pos:
                    sites.append(Site(rel, line, 'assign', name, f'{name} = ...'))
                elif op in ('+=', '<<') and name in str_syms:
                    sites.append(Site(rel, line, 'append', name, f'{name} {op} ...'))
        sites.sort(key=lambda s: (s.file, s.line, s.cls, s.symbol))
        return sites


CANARY = '''\
struct Canary { std::vector<int> rows_; std::map<int, int> by_key_; std::string text_; };
void canary(Canary& c, int n) {
    c.rows_.push_back(n);
    int* p = new int[n];
    auto t = std::vector<int>(n);
    c.by_key_[n] = 1;
    c.text_ = std::to_string(n);
    c.text_ += "x";
}
'''


def canary_classes() -> set[str]:
    return {s.cls for s in Scanner(set()).scan({'canary.cpp': CANARY})}


# --------------------------------------------------------------------------
# gate
# --------------------------------------------------------------------------

def resolve_files(root: Path, patterns: list[str]) -> list[str]:
    found: set[str] = set()
    for pattern in patterns:
        for hit in glob.glob(pattern, root_dir=root, recursive=True):
            path = root / hit
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                found.add(hit.replace('\\', '/'))
    return sorted(found)


def run(root: Path, manifest_path: Path, strict: bool, json_out: Path | None):
    classes = canary_classes()
    if classes != set(OP_CLASSES):
        raise GateError(f'scanner canary: site classes {sorted(set(OP_CLASSES) - classes)} '
                        'matched nothing on the built-in snippet; the pattern set is broken')
    manifest = load_manifest(root, manifest_path)
    files = resolve_files(root, manifest.files)
    if not files:
        raise GateError(f'the declared file set {manifest.files} resolves to zero files '
                        f'under {root}')
    omitted = sorted(set(resolve_files(root, list(ENGINE_FLOOR))) - set(files))
    if omitted:
        raise GateError(f'manifest scan.files omits {len(omitted)} engine source file(s) under '
                        f'{", ".join(ENGINE_FLOOR)}, first: {omitted[0]}; the manifest may not '
                        'narrow the scanned engine tree')
    generated = resolve_files(root, manifest.generated)
    if manifest.generated and not generated:
        raise GateError(f'declared generated_files {manifest.generated} resolve to zero files')
    files = sorted(set(files) | set(generated))
    unowned = [rel for rel in files if manifest.owner_of(rel) is None]
    if unowned:
        raise GateError(f'{len(unowned)} scanned file(s) match no owners glob, first: {unowned[0]}')
    texts = {rel: (root / rel).read_text(errors='replace') for rel in files}
    sites = Scanner(set(manifest.fixed)).scan(texts)
    if not sites:
        raise GateError(f'the scan of {len(files)} file(s) found zero sites; a gate that '
                        'matches nothing is not a pass')
    by_key: dict[tuple[str, str], list[Site]] = {}
    for site in sites:
        by_key.setdefault(site.key, []).append(site)
    findings: list[tuple[str, int, str]] = []
    matched: dict[str, dict] = {}
    entry_findings: set[str] = set()
    for key, group in sorted(by_key.items()):
        entry = manifest.by_key.get(key)
        if entry is None:
            for site in group:
                findings.append((site.file, site.line, f'no manifest entry for {site.cls} site '
                                 f'on {site.symbol!r} ({site.text})'))
            continue
        matched[entry['id']] = entry
        seen_classes = {s.cls for s in group}
        for cls in sorted(seen_classes - set(entry['ops'])):
            first = next(s for s in group if s.cls == cls)
            findings.append((first.file, first.line, f"op class '{cls}' on {first.symbol!r} is "
                             f'not in the ops of entry {entry["id"]} ({first.text})'))
            entry_findings.add(entry['id'])
        if entry['state'] == 'bounded':
            for site in group:
                if site.computed:
                    findings.append((site.file, site.line,
                                     f'bounded entry {entry["id"]} covers a {site.text} whose '
                                     'argument is computed, not a declared fixed input'))
                    entry_findings.add(entry['id'])
    for entry in manifest.entries:
        if entry['id'] not in matched:
            findings.append((entry['file'], int(entry.get('line') or 0),
                             f'entry {entry["id"]} ({entry["symbol"]!r}) matches no site: stale; '
                             'remove or update it'))
    pin_text = (root / manifest.pin_source).read_text(errors='replace')
    pin_match = re.search(r'CODEGEN_COMMIT="?([0-9a-f]{40})', pin_text)
    if not pin_match or pin_match.group(1) != manifest.pin:
        findings.append((manifest.pin_source, 0,
                         f'codegen pin {manifest.pin} in the manifest != '
                         f'{pin_match.group(1) if pin_match else "(none)"} in '
                         f'{manifest.pin_source}: re-review the emitter template list and update '
                         'the manifest pin'))
    states = {s: sum(1 for e in manifest.entries if e['state'] == s) for s in STATES}
    for state in OPEN_STATES:
        if states[state] > manifest.ceilings[state]:
            findings.append((str(manifest_path.relative_to(root)) if manifest_path.is_relative_to(root)
                             else str(manifest_path), 0,
                             f'{state} entries {states[state]} exceed the manifest ceiling '
                             f'{manifest.ceilings[state]}: a new OPEN entry needs a reviewed '
                             'ceiling change'))
    passed = [e for e in matched.values() if e['state'] == 'bounded' and e['id'] not in entry_findings]
    opened = [e for e in matched.values() if e['state'] in OPEN_STATES]
    open_templates = [t for t in manifest.templates]
    out = print
    out(f'{PREFIX} root={root}')
    out(f'{PREFIX} manifest={manifest_path.name} schema={SCHEMA} '
        f'codegen_tree=absent codegen_pin={manifest.pin[:12]} '
        f'emitter_coverage=declared-template-list')
    out(f'{PREFIX} files_scanned={len(files)} generated_files={len(generated)} '
        f'sites_found={len(sites)} containers={len(by_key)}')
    out(f'{PREFIX} entries={len(manifest.entries)} entries_matched={len(matched)} '
        f'templates_declared={len(manifest.templates)} '
        'states: ' + ' '.join(f'{s}={states[s]}' for s in STATES))
    for file, line, message in sorted(findings):
        out(f'{PREFIX} FINDING {file}:{line}: {message}')
    pending = sum(1 for e in opened if e['state'] == 'pending-flat-memory')
    out(f'{PREFIX} counts PASS(bounded)={len(passed)} OPEN={len(opened)} '
        f'OPEN(templates)={len(open_templates)} findings={len(findings)}')
    out(f'{PREFIX} OPEN detail pending-flat-memory={pending} '
        f'unreviewed={len(opened) - pending}: listed, never counted as PASS')
    total_open = len(opened) + len(open_templates)
    if findings:
        verdict, code = 'FINDINGS', 1
    elif total_open:
        verdict, code = 'OPEN-ONLY', 1 if strict else 0
    else:
        verdict, code = 'CLEAN', 0
    note = {'FINDINGS': 'fix every finding above',
            'OPEN-ONLY': f'{total_open} OPEN item(s): no new unbounded site entered the scanned '
                         'set, which is NOT a flat-memory proof' + ('; --strict refuses OPEN' if strict else ''),
            'CLEAN': 'every entry bounded by a declared fixed input'}[verdict]
    out(f'{PREFIX} result: {verdict} ({note})')
    if json_out:
        rows = []
        for entry in manifest.entries:
            live = by_key.get((entry['file'], entry['symbol']), [])
            rows.append({**entry, 'live_lines': sorted({s.line for s in live}),
                         'live_sites': len(live), 'matched': entry['id'] in matched})
        json_out.write_text(json.dumps({
            'files_scanned': len(files), 'sites_found': len(sites), 'containers': len(by_key),
            'findings': [{'file': f, 'line': l, 'message': m} for f, l, m in sorted(findings)],
            'states': states, 'templates': manifest.templates, 'entries': rows,
            'verdict': verdict}, indent=1) + '\n')
    return code, sites, by_key, manifest


def emit_seed(root: Path, manifest_path: Path) -> int:
    """Print ``unreviewed`` entry lines for every scanned key that lacks an
    entry. The manifest must already declare the scan, owners and ceilings; the
    ceilings still have to be raised by a reviewed edit for the gate to pass."""
    manifest = load_manifest(root, manifest_path)
    files = sorted(set(resolve_files(root, manifest.files)) | set(resolve_files(root, manifest.generated)))
    sites = Scanner(set(manifest.fixed)).scan({r: (root / r).read_text(errors='replace') for r in files})
    by_key: dict[tuple[str, str], list[Site]] = {}
    for site in sites:
        by_key.setdefault(site.key, []).append(site)
    top = max([int(m.group(1)) for e in manifest.entries
               if (m := re.fullmatch(r'U(\d+)', e['id']))] or [0])
    for key, group in sorted(by_key.items()):
        if key in manifest.by_key:
            continue
        top += 1
        decl = next((s.container for s in group if s.container), '') \
            or f'unresolved by the index: {group[0].text}'
        fields = {'id': f'U{top:04d}', 'file': key[0], 'symbol': key[1],
                  'ops': sorted({s.cls for s in group}), 'container': decl[:80],
                  'owner': manifest.owner_of(key[0]), 'phase': 'unclassified',
                  'state': 'unreviewed', 'line': group[0].line}
        parts = []
        for k, v in fields.items():
            if isinstance(v, int):
                parts.append(f'{k}: {v}')
            elif isinstance(v, list):
                parts.append(f'{k}: [' + ', '.join(json.dumps(x) for x in v) + ']')
            else:
                parts.append(f'{k}: {json.dumps(v)}')
        print('  - {' + ', '.join(parts) + '}')
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--manifest', type=Path, default=None,
                        help=f'default: <root>/{MANIFEST_REL}')
    parser.add_argument('--strict', action='store_true',
                        help='exit 1 while any OPEN entry remains (release gate for flat memory)')
    parser.add_argument('--json', type=Path, default=None, help='write the machine-readable result')
    parser.add_argument('--self-check', action='store_true',
                        help='run only the built-in scanner canary')
    parser.add_argument('--emit-seed', action='store_true',
                        help='print unreviewed entry lines for scanned keys without an entry')
    args = parser.parse_args(argv)
    try:
        if args.self_check:
            hit = canary_classes()
            print(f'{PREFIX} canary: {len(hit & set(OP_CLASSES))}/{len(OP_CLASSES)} '
                  f'site classes matched ({", ".join(sorted(hit))})')
            return 0 if hit == set(OP_CLASSES) else 2
        root = args.root.resolve()
        manifest_path = (args.manifest or root / MANIFEST_REL).resolve()
        if args.emit_seed:
            return emit_seed(root, manifest_path)
        return run(root, manifest_path, args.strict, args.json)[0]
    except GateError as error:
        print(f'{PREFIX} ERROR: {error}')
        print(f'{PREFIX} result: ERROR (the gate could not judge; this is never a pass)')
        return 2
    except Exception as error:  # a scanner or parser crash must not read as a verdict
        print(f'{PREFIX} ERROR: internal {type(error).__name__}: {error}')
        print(f'{PREFIX} result: ERROR (the gate could not judge; this is never a pass)')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
