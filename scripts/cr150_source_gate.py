#!/usr/bin/env python3
"""Source allocation-bound gate.

Scans the declared C++ file set (engine ``src/**``, the public inline headers
and the generated C++ present in the tree), indexes every allocation-bearing
construct (growth calls, new/malloc, by-value container declarations,
temporaries, map subscripts, container assignment, string append) and compares
the index with ``ci/cr150-source-bounds.yaml``.

Exit 0: no finding. Exit 1: a finding (an unlisted site, a stale entry, a
bounded entry without supported size-guard enforcement, a ceiling or pin drift,
an entry whose ``line:`` field is not the line of its first live site, a note
or a manifest comment citing a line that does not hold what its sentence
names, a number of UNCHECKED note or comment citations other than the one the
manifest pins) or,
with ``--strict``, any OPEN entry left. Exit 2: the gate cannot judge (missing
or malformed manifest/site inventory, an invalid fixed input or bound syntax,
zero files, zero sites, a broken pattern set). A bound naming a growth axis is
an exit-1 finding with its entry ID.

An entry is ``bounded`` only with a declared fixed input and supported
own-size-guarded growth; metadata or reserve alone never counts as PASS(bounded).
Concrete sites are pinned by lexical scope, operation/type and occurrence in a
reviewed companion inventory, independently of the (file, symbol) entry groups.
An entry whose flat-memory implementation
has not landed is ``pending-flat-memory`` (inventory row and growth axis
named) or ``unreviewed`` (nobody has established a bound). Both are OPEN: they
are listed and counted, never PASS, and ``--strict`` fails on them. A green
gate means nothing NEW and unbounded entered the scanned set; it is not a
flat-memory proof. The scan is a regex index over lexed C++ (comments and
string literals removed): not alias, type or call-path proof.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter
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
SITE_SCHEMA = 'cr150-source-sites/v1'
SITE_INVENTORY_REL = 'ci/cr150-source-sites.json'
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
                     'GenericMatrix', 'DrawingRecordStore', 'OwnedDeque')
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
SPLICE_RE = re.compile(r'\\[ \t\f\v\x00]*\n')
DEFINE_RE = re.compile(r'(?<!\w)define(?:\s|/\*.*?\*/)*([A-Za-z_]\w*)', re.S)


class GateError(Exception):
    """The gate cannot judge: exit 2."""


class GrowthAxisBound(GateError):
    """A bounded claim naming a growth axis is a finding, not a schema error."""


def _tokens(text: str) -> str:
    """Whitespace-independent spelling, without joining adjacent identifiers."""
    return ' '.join(re.findall(r'[A-Za-z_]\w*|\d+|::|->|&&|\|\||\S', text))


class Scopes:
    """Lexical brace paths, not C++ name lookup.

    Each component contains the tokenized brace header and its occurrence among
    identical siblings. Lines and byte offsets are diagnostics only. Repeated
    declarations/operations within one path get a separate ordinal in Scanner.
    """

    def __init__(self, code: str):
        self.starts = [0]
        self.paths: list[tuple[str, ...]] = [()]
        self.kinds: list[tuple[str, ...]] = [()]
        stack: list[tuple[str, str]] = []
        counts: Counter = Counter()
        boundary = 0
        for match in re.finditer(r'[{};]', code):
            ch, pos = match.group(), match.start()
            if ch == '{':
                header = re.sub(r'(?m)^\s*#.*$', '', code[boundary:pos]).strip()
                spelling = _tokens(header) or '<block>'
                parent = tuple(part for part, _ in stack)
                counts[parent, spelling] += 1
                kind = 'other'
                if re.fullmatch(r'(?:inline\s+)?namespace(?:\s+[\w:]+)?', header):
                    kind = 'namespace'
                elif re.fullmatch(r'(?:class|struct|union)\s+\w+(?:\s+final)?'
                                  r'(?:\s*:[^;{}()]*)?', header):
                    kind = 'class'
                elif (re.fullmatch(r'(?:[A-Za-z_]\w*(?:::\w+)*[\s*&]+)+'
                                   r'[A-Za-z_]\w*(?:::\w+)*\s*\([^;{}()]*\)'
                                   r'\s*(?:const\s*)?(?:noexcept\s*)?', header)
                      and not re.search(r'\b(?:class|struct|union|enum|namespace|extern|'
                                        r'if|else|for|while|switch|catch|return|throw|new|delete|'
                                        r'sizeof|alignas|decltype|using|typedef)\b', header)):
                    kind = 'function'
                elif not header or header in ('else', 'do', 'try'):
                    kind = 'block'
                elif ((control := re.match(
                        r'(?:if(?:\s+constexpr)?|else\s+if|while|switch|catch)\s*\(', header))
                      and _match_paren(header, control.end() - 1) == len(header) - 1):
                    kind = 'block'
                stack.append((f'{spelling} #{counts[parent, spelling]}', kind))
            elif ch == '}' and stack:
                stack.pop()
            if ch in '{}':
                self.starts.append(pos + 1)
                self.paths.append(tuple(part for part, _ in stack))
                self.kinds.append(tuple(kind for _, kind in stack))
            boundary = pos + 1

    def at(self, pos: int) -> tuple[str, ...]:
        return self.paths[bisect_right(self.starts, pos) - 1]

    def nonlocal_at(self, pos: int) -> bool:
        return all(kind in ('namespace', 'class')
                   for kind in self.kinds[bisect_right(self.starts, pos) - 1])

    def local_at(self, pos: int) -> bool:
        """Prove a function body, crossing only recognized statement blocks.

        Unknown headers (including decorated classes and linkage blocks) are
        barriers. Their spelling still participates in the unchanged site path.
        """
        for kind in reversed(self.kinds[bisect_right(self.starts, pos) - 1]):
            if kind == 'function':
                return True
            if kind != 'block':
                return False
        return False

    def class_at(self, pos: int) -> bool:
        kinds = self.kinds[bisect_right(self.starts, pos) - 1]
        return bool(kinds) and kinds[-1] == 'class'


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
            raise GrowthAxisBound(f'{who}: bound {expr!r} names {name!r}: forbidden growth axis '
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
    if (not isinstance(value, str) or not value.strip()
            or (not placeholder_ok and value.strip().lower() in PLACEHOLDERS)):
        raise GateError(f'{who}: field {field} is empty or a placeholder ({value!r})')
    return value


def _str_list(value, who: str, field: str, *, nonempty: bool = True) -> list[str]:
    if not isinstance(value, list) or (nonempty and not value) \
            or not all(isinstance(v, str) and v.strip() for v in value):
        raise GateError(f'{who}: field {field} must be a {"non-empty " if nonempty else ""}'
                        'list of strings')
    return value


class Manifest:
    def __init__(self, data: dict, root: Path):
        if not isinstance(data, dict):
            raise GateError('manifest must be a mapping')
        _need(data, ('schema', 'codegen', 'scan', 'ceilings', 'owners', 'fixed_inputs',
                     'entries'), 'manifest', ('site_inventory',))
        if data['schema'] != SCHEMA:
            raise GateError(f'manifest schema {data["schema"]!r} != {SCHEMA!r}')
        self.root = root
        self.data = data
        self.site_inventory = _text(data.get('site_inventory', SITE_INVENTORY_REL),
                                    'manifest', 'site_inventory')
        inventory_path = Path(self.site_inventory)
        if inventory_path.is_absolute() or '..' in inventory_path.parts:
            raise GateError('site_inventory must be a path inside the scanned root')
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
        # cited_unchecked pins the number of UNCHECKED note citations and
        # comment_unchecked that of UNCHECKED comment citations: each count
        # must equal its pin. A manifest that does not pin one allows none.
        _need(ceilings, ('unreviewed', 'pending-flat-memory'), 'ceilings',
              ('cited_unchecked', 'comment_unchecked'))
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
            _need(item, ('name', 'definition', 'value', 'fixed_before_bar0'), who, ('note',))
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
            value = item['value']
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                raise GateError(f'{who}: value must be a positive integer')
            body, _ = Scanner._lex(definition.read_text(errors='replace'))
            scopes = Scopes(body)
            declarations = list(re.finditer(
                r'(?<![\w:])(?P<storage>(?:(?:inline|static)\s+)*)(?:constexpr|const)\s+'
                r'(?:std::)?(?:size_t|u?int(?:8|16|32|64)_t|'
                r'(?:unsigned\s+|signed\s+)?(?:int|long(?:\s+long)?|short))\s+'
                rf'(?P<name>{re.escape(name)})\s*=\s*{value}\s*;', body))
            declarations = [m for m in declarations if scopes.nonlocal_at(m.start())
                            and (not scopes.class_at(m.start())
                                 or 'static' in m.group('storage').split())]
            if len(declarations) != 1:
                raise GateError(f'{who}: {item["definition"]} must have exactly one '
                                f'namespace/class-scope const/constexpr integer definition '
                                f'{name} = {value} (static at class scope; comments are not declarations)')
            self.fixed[name] = {**item, '_name_span': declarations[0].span('name')}

    def _entries(self, entries) -> None:
        if not isinstance(entries, list):
            raise GateError('entries must be a list')
        self.entries: list[dict] = []
        self.by_key: dict[tuple[str, str], dict] = {}
        self.bound_errors: dict[str, str] = {}
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
                try:
                    self._bounded(entry, who)
                except GrowthAxisBound as error:
                    self.bound_errors[entry['id']] = str(error)
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


def load_site_inventory(root: Path, manifest: Manifest) -> dict[tuple, dict]:
    path = root / manifest.site_inventory
    if not path.resolve().is_relative_to(root.resolve()):
        raise GateError('site inventory resolves outside the scanned root')
    if not path.is_file():
        raise GateError(f'site inventory {manifest.site_inventory} is missing; generate and '
                        'review it explicitly with --emit-site-inventory, never during verification')
    try:
        data = json.loads(path.read_text())
    except (ValueError, OSError) as error:
        raise GateError(f'site inventory {manifest.site_inventory}: {error}') from error
    if not isinstance(data, dict) or set(data) != {'schema', 'sites'} \
            or data['schema'] != SITE_SCHEMA or not isinstance(data['sites'], list):
        raise GateError(f'site inventory must have schema {SITE_SCHEMA} and a sites list')
    records = {}
    for record in data['sites']:
        if not isinstance(record, dict):
            raise GateError('site inventory record must be a map')
        _need(record, ('file', 'symbol', 'scope', 'cls', 'spelling', 'ordinal'),
              'site inventory record')
        for field in ('file', 'symbol'):
            _text(record[field], 'site inventory record', field, placeholder_ok=True)
        _text(record['spelling'], 'site inventory record', 'spelling')
        _str_list(record['scope'], 'site inventory record', 'scope', nonempty=False)
        if record['cls'] not in OP_CLASSES or not isinstance(record['ordinal'], int) \
                or isinstance(record['ordinal'], bool) or record['ordinal'] < 1:
            raise GateError('site inventory record has an invalid class or ordinal')
        key = (record['file'], record['symbol'], tuple(record['scope']),
               record['cls'], _tokens(record['spelling']), record['ordinal'])
        if key in records:
            raise GateError(f'duplicate identity in site inventory: {key}')
        records[key] = record
    return records


# --------------------------------------------------------------------------
# scanner
# --------------------------------------------------------------------------

class Site:
    __slots__ = ('file', 'line', 'cls', 'symbol', 'text', 'computed', 'container',
                 'pos', 'scope', 'ordinal')

    def __init__(self, file, line, cls, symbol, text, computed=False, container='', *, pos=0):
        self.file, self.line, self.cls, self.symbol = file, line, cls, symbol
        self.text, self.computed, self.container = text, computed, container
        self.pos, self.scope, self.ordinal = pos, (), 0

    @property
    def key(self):
        return (*self.group_key, self.scope, self.cls, _tokens(self.text), self.ordinal)

    @property
    def group_key(self):
        return (self.file, self.symbol)

    def identity(self) -> dict:
        return dict(scope=list(self.scope), cls=self.cls, spelling=_tokens(self.text),
                    ordinal=self.ordinal)


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
                sites.append(Site(rel, line, 'temp', f'<temp:{kind}>', text, pos=match.start()))
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
            sites.append(Site(rel, line, 'decl', symbol, text, container=text, pos=match.start()))
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
                                  f'{symbol}.{op}(...)', computed, pos=m.start()))
            for m in NEW_RE.finditer(code):
                before = code[max(0, m.start() - 12):m.start()]
                after = code[m.end():m.end() + 2].strip()
                if before.rstrip().endswith('<') or after.startswith('>'):
                    continue  # #include <new>, std::nothrow_t templates: not an allocation
                if re.search(r'operator\s*$', before):
                    # a user-defined allocation function (class-specific operator new)
                    sites.append(Site(rel, self._line(starts, m.start()), 'alloc', 'operator new',
                                      'operator new', pos=m.start()))
                    continue
                typename = re.match(r'\s*([\w:]+)', code[m.end():m.end() + 80])
                symbol = _assigned_name(code, m.start()) or f'new:{typename.group(1) if typename else "?"}'
                sites.append(Site(rel, self._line(starts, m.start()), 'alloc', symbol,
                                  f'new {typename.group(1) if typename else ""}', pos=m.start()))
            for m in CALLOC_RE.finditer(code):
                symbol = _assigned_name(code, m.start()) or m.group(1)
                sites.append(Site(rel, self._line(starts, m.start()), 'alloc', symbol, m.group(1),
                                  pos=m.start()))
            for m in MAKE_RE.finditer(code):
                symbol = _assigned_name(code, m.start()) or m.group(1)
                sites.append(Site(rel, self._line(starts, m.start()), 'alloc', symbol, m.group(1),
                                  pos=m.start()))
            for m in TO_STRING_RE.finditer(code):
                sites.append(Site(rel, self._line(starts, m.start()), 'temp', 'std::to_string',
                                  'std::to_string(...)', pos=m.start()))
            map_syms, str_syms, all_syms = maps | members_map, strings | members_str, \
                containers | members_all
            for m in OPERATOR_RE.finditer(code):
                name, op = m.group(1), m.group(2)
                line = self._line(starts, m.start(1))
                if op == '[' and name in map_syms:
                    sites.append(Site(rel, line, 'subscript', name, f'{name}[...]', pos=m.start()))
                elif op == '=' and name in all_syms and (line, name) not in decl_pos:
                    sites.append(Site(rel, line, 'assign', name, f'{name} = ...', pos=m.start()))
                elif op in ('+=', '<<') and name in str_syms:
                    sites.append(Site(rel, line, 'append', name, f'{name} {op} ...', pos=m.start()))
        sites.sort(key=lambda s: (s.file, s.pos, s.cls, s.symbol))
        scopes = {rel: Scopes(code) for rel, (code, _) in lexed.items()}
        ordinals: Counter = Counter()
        for site in sites:
            site.scope = scopes[site.file].at(site.pos)
            stem = (*site.group_key, site.scope, site.cls, _tokens(site.text))
            ordinals[stem] += 1
            site.ordinal = ordinals[stem]
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


def _guarded_push(site: Site, code: str) \
        -> tuple[str, tuple[int, int], tuple[int, int], tuple[int, int]] | str:
    """Recognize just one direct, scalar push guarded by its receiver's size.

    No intervening statements, loops, compound conditions, range insertion,
    member receivers or argument expressions are admitted. This intentionally
    does not try to prove arbitrary C++ control flow.
    """
    operation = GROW_RE.match(code, site.pos)
    if operation is None or operation.group(1) not in ('push_back', 'emplace_back'):
        return 'unsupported growth: reserve/resize/assign is not an enforced maximum'
    receiver = re.search(r'(?<![\w.>])([A-Za-z_]\w*)\s*$', code[:site.pos])
    if receiver is None or receiver.group(1) != site.symbol:
        return 'guarded admission supports a bare local vector receiver only'
    open_at = operation.end() - 1
    close = _match_paren(code, open_at)
    if close is None:
        return 'cannot delimit the growth call'
    argument = code[open_at + 1:close].strip()
    if not re.fullmatch(r'[-+]?\d+(?:\.\d+)?', argument):
        return 'guarded admission requires a numeric literal push argument (no conversion/call side effects)'
    guard = re.search(r'\bif\s*\((?P<condition>[^;{}]*)\)\s*(?P<brace>\{)?\s*$',
                      code[:receiver.start()])
    if guard is None:
        return 'growth has no direct own-size guard against the declared bound'
    condition = re.fullmatch(
        rf'\s*(?P<receiver>{re.escape(site.symbol)})\s*\.\s*size\s*\(\s*\)\s*<\s*'
        r'(?P<bound>[A-Za-z_]\w*|\d+)\s*', guard.group('condition'))
    if condition is None:
        return 'guard must compare this container\'s own size() < the declared bound identifier'
    tail = re.match(r'\s*;\s*', code[close + 1:])
    if tail is None:
        return 'guard body must be a standalone push statement'
    if guard.group('brace') and not code[close + 1 + tail.end():].startswith('}'):
        return 'guard block must contain exactly one push statement'
    start, end = condition.span('bound')
    base = guard.start('condition')
    size_start, size_end = condition.span('receiver')
    return (condition.group('bound'), (base + start, base + end), receiver.span(1),
            (base + size_start, base + size_end))


def bounded_issues(entry: dict, group: list[Site], sites: list[Site],
                   texts: dict[str, str], manifest: Manifest) -> list[tuple[str, int, str]]:
    """Fail closed outside the small enforced form; annotation alone never passes."""
    eid, bound = entry['id'], entry['bound']
    first = group[0]

    def finding(site: Site, reason: str):
        return (site.file, site.line, f'bounded entry {eid}: {reason}')

    if eid in manifest.bound_errors:
        return [finding(first, manifest.bound_errors[eid])]
    if Path(entry['file']).parts[0] == 'include':
        return [finding(first, 'header entries under include/ are outside guarded bounded '
                        'admission: prior includes and build flags are not proven')]
    if not IDENT_RE.fullmatch(bound) or bound not in manifest.fixed:
        return [finding(first, f'bound {bound!r}: guarded admission requires one fixed input identifier')]
    fixed = manifest.fixed[bound]
    if fixed['definition'] not in texts:
        return [finding(first, 'fixed-input definition is outside the scanned source files')]
    fixed_header = str(fixed['definition']).removeprefix('include/')
    proof_files = {entry['file'], fixed['definition']}
    macro_names = {'push_back', 'emplace_back', 'size'}
    for rel in sorted(proof_files):
        raw = texts[rel]
        # These are context-free raw-text refusals, including inside comments
        # and literals. Do not let a lexer/compiler disagreement admit a proof.
        if '\x00' in raw:
            return [finding(first, f'{rel}: NUL is outside guarded bounded admission')]
        if '\ufeff' in raw:
            return [finding(first, f'{rel}: BOM is outside guarded bounded admission')]
        if not raw.isascii():
            return [finding(first, f'{rel}: non-ASCII text is outside guarded bounded admission')]
        if re.search(r'(?:u8|[LuU])?R"', raw):
            return [finding(first, f'{rel}: raw string literals are outside guarded bounded admission')]
        if SPLICE_RE.search(raw):
            return [finding(first, f'{rel}: line continuation is outside guarded bounded admission')]
        if re.search(r'<%|%>', raw):
            return [finding(first, f'{rel}: brace digraphs are outside guarded bounded admission')]
        # Keywords and scalar/type names can be macro-rewritten just like the
        # receiver and bound. Include both proof files, not just five names.
        macro_names.update(IDENT_RE.findall(raw))
        for marker in re.finditer(r'#|%:', raw):
            directive = '#' + raw[marker.end():].split('\n', 1)[0]
            if re.fullmatch(r'#\s*pragma\s+once\s*', directive):
                continue
            if re.fullmatch(r'#\s*include\s*<(?:vector|cstddef|cstdint)>\s*', directive):
                continue
            if rel == entry['file'] and re.fullmatch(
                    rf'#\s*include\s*"{re.escape(fixed_header)}"\s*', directive):
                continue
            return [finding(first, f'{rel}: macro/conditional preprocessing or unsupported includes '
                            'are outside guarded bounded admission')]
    for rel, text in texts.items():
        # Raw strings can revert splices; comments can cross directive lines.
        # Intentionally ignore lexical context and even the directive marker.
        for definition in DEFINE_RE.finditer(SPLICE_RE.sub('', text)):
            if definition.group(1) in macro_names:
                return [finding(first, f'scanned file {rel} defines macro '
                                f'{definition.group(1)!r} used by guarded bounded admission')]
    lexed = {rel: Scanner._lex(text) for rel, text in texts.items()}
    declarations = [s for s in group if s.cls == 'decl']
    if len(declarations) != 1:
        return [finding(first, 'guarded admission requires exactly one local scalar-vector declaration')]
    declaration = declarations[0]
    code = lexed[declaration.file][0]
    scalar = (r'(?:bool|char|float|double|(?:std::)?u?int(?:8|16|32|64)_t|'
              r'(?:unsigned\s+|signed\s+)?(?:int|short|long(?:\s+long)?))')
    empty = re.match(rf'std::vector\s*<\s*{scalar}\s*>\s+(?P<symbol>{re.escape(entry["symbol"])})'
                     r'\s*(?:;|\{\s*\}\s*;)', code[declaration.pos:])
    prefix_start = max(code.rfind(ch, 0, declaration.pos) for ch in ';{}') + 1
    if (empty is None or not Scopes(code).local_at(declaration.pos)
            or re.search(r'\bextern\b', code[prefix_start:declaration.pos])):
        return [finding(declaration, 'guarded admission requires an initially empty local '
                        'std::vector of scalar values; member/global/initialized containers stay OPEN')]
    issues = []
    symbol_spans = {(declaration.pos + empty.start('symbol'),
                     declaration.pos + empty.end('symbol'))}
    growth = [s for s in group if s.cls == 'grow']
    if not growth:
        issues.append(finding(declaration, 'no guarded growth witnesses the declared bound'))
    for site in group:
        if site.cls == 'decl':
            continue
        if site.cls != 'grow':
            issues.append(finding(site, f'{site.cls} is outside guarded scalar-push admission'))
            continue
        if site.computed:
            issues.append(finding(site, f'{site.text} argument is computed, not an enforced bound'))
            continue
        if site.scope[:len(declaration.scope)] != declaration.scope:
            issues.append(finding(site, 'growth is outside the matched declaration scope'))
            continue
        guard = _guarded_push(site, lexed[site.file][0])
        if isinstance(guard, str):
            issues.append(finding(site, guard))
        else:
            symbol_spans.update(guard[2:])
            if guard[0] != bound:
                issues.append(finding(site, f'guard bound {guard[0]!r} does not match declared bound {bound!r}'))
    for rel, (body, starts) in lexed.items():
        for match in re.finditer(rf'\b{re.escape(entry["symbol"])}\b', body):
            if rel != declaration.file or match.span() not in symbol_spans:
                issues.append((rel, Scanner._line(starts, match.start()),
                               f'bounded entry {eid}: container has an unsupported use or shadow '
                               'outside its declaration/direct size-guarded pushes; aliases '
                               'and other mutation paths are not admitted'))
                break
    # Do not resolve shadowing with a new C++ parser. Refuse a fixed name used
    # anywhere except its immutable declaration and recognized push guards.
    allowed = {(fixed['definition'], *fixed['_name_span'])}
    for site in sites:
        if site.cls == 'grow':
            guard = _guarded_push(site, lexed[site.file][0])
            if not isinstance(guard, str) and guard[0] == bound:
                allowed.add((site.file, *guard[1]))
    for rel, (body, starts) in lexed.items():
        for match in re.finditer(rf'\b{re.escape(bound)}\b', body):
            if (rel, *match.span()) not in allowed:
                issues.append((rel, Scanner._line(starts, match.start()),
                               f'bounded entry {eid}: fixed bound {bound!r} has an unsupported '
                               'use or shadow declaration outside its definition/size guards'))
    return issues


# --------------------------------------------------------------------------
# note citations
# --------------------------------------------------------------------------

_CITE_NUM = r'\d+(?:-\d+)?'
# What joins the lines of one list: a comma, "and" or "or", with or without
# spaces (":4,6", ":4, 6", ":4 and 6", ":4, 6 and 9").
_CITE_JOIN = r'(?:\s*,\s*(?:(?:and|or)\s+)?|\s+(?:and|or)\s+)'
_CITE_SPEC = _CITE_NUM + r'(?:' + _CITE_JOIN + _CITE_NUM + r')*'
# The three written forms: ``path:N``, a bare ``:N`` and ``line N``. N may be a
# range N-M or a list of lines and ranges; every element of a list is read. A
# citation starts where no path character precedes it, so the clock time in
# "17:20" (a number on both sides of the colon) is not one.
CITE_RE = re.compile(
    r'(?<![\w./-])(?:'
    r'(?P<path>[A-Za-z_][\w./-]*):(?P<pspec>' + _CITE_SPEC + r')'
    r'|:(?P<bspec>' + _CITE_SPEC + r')'
    r'|(?P<word>[Ll]ines?)\s+(?P<wspec>' + _CITE_SPEC + r')'
    r')(?!\w)')
CITE_JOIN_RE = re.compile(_CITE_JOIN)
# The word that calls a cited line a site, written right before the citation.
SITE_WORD_RE = re.compile(r'(?<![\w-])[Ss]ite(?P<plural>s?)\s*\(?\s*$')
# Words that are never the construct a note names: C++ keywords and builtin
# type words (they sit on almost any line) and the unit words whose capitals
# look like a name.
NOTE_PROSE = KEYWORDS | {'std', 'auto', 'bool', 'char', 'double', 'else', 'false', 'float',
                         'int', 'long', 'nullptr', 'short', 'signed', 'this', 'true',
                         'unsigned', 'void', 'KiB', 'MiB', 'GiB'}


def note_sentences(note: str) -> list[str]:
    """The note cut after each period or semicolon that ends a clause: one that
    whitespace follows, outside parentheses. A parenthesis belongs to the
    sentence it stands in, so the citations of ``(:3; outer :6)`` stay with
    the words before it. Only a parenthesis that closes holds a sentence
    together: an opening one that never closes is plain text, so the sentences
    after it are still cut and none lends its names to a citation before it."""
    paired, opened = set(), []
    for index, ch in enumerate(note):
        if ch == '(':
            opened.append(index)
        elif ch == ')' and opened:
            paired.update((opened.pop(), index))
    sentences, start, depth = [], 0, 0
    for index, ch in enumerate(note):
        if index in paired:
            depth += 1 if ch == '(' else -1
        elif ch in '.;' and not depth and note[index + 1:index + 2].isspace():
            sentences.append(note[start:index + 1])
            start = index + 1
    sentences.append(note[start:])
    return sentences


def note_citations(note: str):
    """Yield (label, path, first, last, sentence, as_site, at) for every cited line.

    ``label`` is the citation as the note writes it, one per element of a
    list. ``path`` is None for ``line N`` and for a bare ``:N``, unless the
    bare form continues a ``path:N`` with nothing but a list join between them
    (``path:N and :M``, ``path:N, :M``): then it is a line of that path, and
    so is the next one joined to it. ``sentence`` is the citation's sentence
    (``note_sentences``). ``as_site`` is true when the word "site" introduces
    the citation or the citation joined before it, and for every citation
    that follows the word "sites" in its sentence: the name this inventory
    gives to what an entry records. ``at`` is the offset in ``note`` at which
    the citation is written, the same for every element of a list.
    """
    base = 0  # the sentences are consecutive slices of the note
    for sentence in note_sentences(note):
        sites_named = False
        previous = None  # (end, path, as_site) of the citation before this one
        for match in CITE_RE.finditer(sentence):
            path = match.group('path')
            spec = match.group('pspec') or match.group('bspec') or match.group('wspec')
            joined = previous is not None and CITE_JOIN_RE.fullmatch(
                sentence, previous[0], match.start()) is not None
            if joined and path is None and match.group('word') is None:
                path = previous[1]
            site_word = SITE_WORD_RE.search(sentence[:match.start()])
            if site_word is not None and site_word.group('plural'):
                sites_named = True
            as_site = (site_word is not None or sites_named
                       or (joined and previous[2]))
            previous = (match.end(), path, as_site)
            for first, last in re.findall(r'(\d+)(?:-(\d+))?', spec):
                element = f'{first}-{last}' if last else first
                if match.group('path'):
                    label = f'{path}:{element}'
                elif match.group('word'):
                    label = f'{match.group("word")} {element}'
                else:
                    label = f':{element}'
                yield (label, path, int(first), int(last or first), sentence, as_site,
                       base + match.start())
        base += len(sentence)


def stray_matches(note: str) -> list[re.Match]:
    """``:N`` text that no citation form reads (a clock time is not one), as
    matches on the note with its citations blanked: each offset is the one
    the text has in the note."""
    blank = CITE_RE.sub(lambda match: ' ' * len(match.group(0)), note)
    return list(re.finditer(r'(?<!\d):\d+', blank))


def note_strays(note: str) -> list[str]:
    """The text of every ``stray_matches`` hit."""
    return [match.group(0) for match in stray_matches(note)]


def note_constructs(sentence: str) -> set[str]:
    """Identifiers a sentence spells as code, never its prose words: backticked
    text, the parts of a ``::`` name, the parts of a member call
    (``buffer.substr(...)``, ``it->next()``), words with an underscore or an
    inner capital (snake_case, camelCase) and the growth operations this gate
    indexes (``emplace``, ``reserve``, ...). A capitalized or upper-case word
    (``Store``, ``NUL``) is prose unless the note backticks it."""
    names: set[str] = set()
    for quoted in re.findall(r'`([^`]*)`', sentence):
        names.update(IDENT_RE.findall(quoted))
    plain = CITE_RE.sub(' ', re.sub(r'`[^`]*`', ' ', sentence))
    for qualified in re.findall(r'[A-Za-z_]\w*(?:::[A-Za-z_]\w*)+', plain):
        names.update(qualified.split('::'))
    for call in re.findall(r'[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)+(?=\()', plain):
        names.update(re.split(r'\.|->', call))
    names.update(word for word in IDENT_RE.findall(plain)
                 if '_' in word or re.search(r'[a-z][A-Z]', word) or word in GROW_OPS)
    return names - NOTE_PROSE


def construct_end(code: list[str], first: int) -> int | None:
    """The line on which the block or statement that begins on line ``first``
    ends, read on lexed rows (comments and literals removed): the ``}`` that
    brings the brackets opened since that line back to none, or the first
    ``;`` outside every bracket. A function ends on its closing brace, a call
    written on two lines on the line of its ``;``.

    None when line ``first`` begins nothing (it is blank or a preprocessor
    directive, or it closes a bracket it did not open) and when the file ends
    first. Bracket matching, not C++ parsing: it finds where the text that
    starts on that line closes, and says nothing about what that text is.
    """
    if not 1 <= first <= len(code):
        return None
    head = code[first - 1].strip()
    if not head or head.startswith('#'):
        return None
    depth = 0
    for number in range(first, len(code) + 1):
        row = code[number - 1]
        if row.lstrip().startswith('#'):
            continue
        for ch in row:
            if ch in '([{':
                depth += 1
            elif ch in ')]}':
                depth -= 1
                if depth < 0:
                    return None
                if depth == 0 and ch == '}':
                    return number
            elif ch == ';' and depth == 0:
                return number
    return None


def comment_blocks(text: str) -> list[tuple[str, list[int], list[int]]]:
    """The comments of the manifest text, as blocks of prose.

    A comment is what the manifest parser drops from a line
    (``_strip_comment``). Consecutive lines that hold a comment and nothing
    else form one block, read as one text with a space for each line break,
    so a sentence written over several comment lines is one sentence. A
    comment with no words, a line without a comment and a line of data each
    end the block before them; a comment that follows data on its line is a
    block of its own.

    Returns one (text, starts, numbers) per block: ``starts[i]`` is the offset
    in ``text`` at which the comment of manifest line ``numbers[i]`` begins.
    """
    blocks: list[tuple[str, list[int], list[int]]] = []
    parts: list[str] = []
    numbers: list[int] = []

    def close() -> None:
        if parts:
            starts, offset = [], 0
            for part in parts:
                starts.append(offset)
                offset += len(part) + 1
            blocks.append((' '.join(parts), starts, list(numbers)))
            parts.clear()
            numbers.clear()

    for number, raw in enumerate(text.split('\n'), 1):
        data = _strip_comment(raw)
        words = raw[len(data) + 1:].strip()
        if not words:
            close()
            continue
        if data.strip():
            close()
        parts.append(words)
        numbers.append(number)
        if data.strip():
            close()
    close()
    return blocks


def check_citations(manifest: 'Manifest', by_key: dict, texts: dict[str, str], root: Path,
                    comments: list | tuple = (), manifest_rel: str = MANIFEST_REL):
    """Check every line an entry note or a manifest comment cites.

    A bare ``:N`` or ``line N`` is a line of the entry's own file; ``path:N``
    is a line of that path, written from the root, and so is a bare ``:M``
    joined to it (``path:N and :M``). A cited line holds what the note says
    when it carries

    * a site of the entry, when the file is the entry's own;
    * an identifier the note spells as code in the citation's own sentence.

    In another file the entry's symbol counts only as such an identifier: the
    entry records no site there, and the same spelling may be another object.
    A citation the note calls a site holds only on a site of the entry: a
    spelled construct does not stand in for that claim.

    A range ``N-M`` is judged at its two ends; a line inside it says nothing
    about where the range begins or ends. Its first line must hold. Its last
    line must hold too, or be the line on which the block or statement that
    begins on the first line ends (``construct_end``, C and C++ sources only):
    the closing brace of a function, the last line of a call written on
    several. A range whose first line does not hold never holds.

    A citation that does not hold is a finding when the note names something
    to look for there (a spelled construct, or the word "site"). A sentence of
    prose only names nothing a text check can look for: that citation is
    UNCHECKED, listed and counted, never taken as holding; the caller pins
    their number with the manifest ceiling. A cited path that is not a file
    under the root, or a line past its end, is a finding. Read-only: no file,
    count, id or bound changes.

    A manifest comment (``comment_blocks``) is judged by the same rules, as a
    note of no entry. It has no file of its own, so a line it cites is written
    with its path (``path:N``, or a bare ``:M`` joined to one): a bare ``:N``
    or ``line N`` that continues no path is a finding. It records no site, so
    a cited line holds only on an identifier its sentence spells as code, and
    a citation the comment calls a site never holds. Each finding names the
    manifest line on which the citation is written. Comment citations are
    counted apart from note citations, and the caller pins the UNCHECKED ones
    among them with a ceiling of their own.

    Returns (findings, entry ids with a finding, unchecked lines, cited, held)
    for the notes, then (findings, unchecked lines, cited, held) for the
    comments as a sixth member.
    """
    cache: dict[str, list[str] | None] = {}
    code_cache: dict[str, list[str] | None] = {}

    def code_of(rel: str) -> list[str] | None:
        """The lexed rows of a C or C++ source that ``rows_of`` has read, or
        None for any other file: only code has blocks and statements."""
        if rel not in code_cache:
            code_cache[rel] = None
            if Path(rel).suffix in SOURCE_SUFFIXES:
                text = texts.get(rel)
                if text is None:
                    text = (root / rel).read_text(errors='replace')
                code_cache[rel] = [part[0] for part in cxxlex.lex_lines(text)]
        return code_cache[rel]

    def rows_of(rel: str) -> list[str] | None:
        if rel not in cache:
            text = texts.get(rel)
            if text is None:
                path = Path(rel)
                full = root / path
                if (path.is_absolute() or '..' in path.parts or not full.is_file()
                        or not full.resolve().is_relative_to(root.resolve())):
                    cache[rel] = None
                    return None
                text = full.read_text(errors='replace')
            rows = text.split('\n')
            if rows and rows[-1] == '':
                rows.pop()
            cache[rel] = rows
        return cache[rel]

    def shown(row: str) -> str:
        text = ' '.join(row.split())
        return text[:137] + '...' if len(text) > 140 else text

    def judge(who: str, kind: str, label: str, rel: str, first: int, last: int,
              sentence: str, as_site: bool, site_lines: set[int], expected: str):
        """One citation of the note or comment that ``who`` names; ``kind`` is
        the word for it. ``site_lines`` are the lines that are sites for this
        citation (those of the entry when ``rel`` is its own file, none
        otherwise) and ``expected`` says what a line that does not hold is
        not. Returns ('held', None), ('finding', (file, line, message)) or
        ('unchecked', message)."""
        rows = rows_of(rel)
        if rows is None:
            return 'finding', (rel, first, f'{who} cites {label}: {rel!r} is not '
                               'a file under the scanned root (a cited path is written from '
                               'the root)')
        if not 1 <= first <= last <= len(rows):
            return 'finding', (rel, first, f'{who} cites {label}: {rel} has '
                               f'{len(rows)} line(s)')
        named = note_constructs(sentence)
        # A claimed site must be a site of the entry: a construct the
        # sentence spells does not stand in for it.
        spelled = set() if as_site else named
        # A range is judged at its two ends, a single line at itself.
        missing = [line for line in ((first,) if first == last else (first, last))
                   if line not in site_lines
                   and not spelled & set(IDENT_RE.findall(rows[line - 1]))]
        if missing == [last] and first != last:
            # The first line holds. The last one need not when it is where
            # the block or statement that begins on the first line ends.
            code = code_of(rel)
            if code is not None and construct_end(code, first) == last:
                missing = []
        if not missing:
            return 'held', None
        if first == last:
            holds = f'{rel}:{first} holds "{shown(rows[first - 1])}": {expected}'
        else:
            which = (f'line {missing[0]} does not' if len(missing) == 1
                     else 'neither line does')
            holds = (f'{rel}:{first}-{last} begins "{shown(rows[first - 1])}" and ends '
                     f'"{shown(rows[last - 1])}": a range holds only when its first line '
                     'does and its last line either does or ends the block or statement '
                     f'that begins on the first, and {which}: {expected}')
        if as_site:
            return 'finding', (rel, first, f'{who} cites {label}: {holds}, '
                               f'and the {kind} calls it a site')
        if named:
            return 'finding', (rel, first, f'{who} cites {label}: {holds} '
                               f'and none of the constructs the {kind} names in '
                               f'that sentence ({", ".join(sorted(named))})')
        return 'unchecked', (f'{who} cites {label}: {holds}, and '
                             'the sentence spells no construct to look for')

    findings: list[tuple[str, int, str]] = []
    failed: set[str] = set()
    unchecked: list[str] = []
    cited = held = 0
    for entry in manifest.entries:
        note = entry.get('note')
        if not isinstance(note, str):
            continue
        eid, own = entry['id'], entry['file']
        site_lines = {site.line for site in by_key.get((own, entry['symbol']), [])}
        for stray in note_strays(note):
            cited += 1
            unchecked.append(f'entry {eid}: note has {stray!r} in a form no citation rule reads')
        for label, path, first, last, sentence, as_site, _ in note_citations(note):
            cited += 1
            rel = path or own
            verdict, detail = judge(
                f'entry {eid}: note', 'note', label, rel, first, last, sentence, as_site,
                site_lines if rel == own else set(),
                f'not a site of the entry ({entry["symbol"]!r})' if rel == own
                else 'not in the entry\'s own file')
            if verdict == 'held':
                held += 1
            elif verdict == 'finding':
                findings.append(detail)
                failed.add(eid)
            else:
                unchecked.append(detail)
    # A comment is a note of no entry: no file of its own, no site.
    comment_findings: list[tuple[str, int, str]] = []
    comment_unchecked: list[str] = []
    comment_cited = comment_held = 0
    for text, starts, numbers in comments:
        for stray in stray_matches(text):
            comment_cited += 1
            written = numbers[bisect_right(starts, stray.start()) - 1]
            comment_unchecked.append(f'manifest line {written}: comment has '
                                     f'{stray.group(0)!r} in a form no citation rule reads')
        for label, path, first, last, sentence, as_site, at in note_citations(text):
            comment_cited += 1
            written = numbers[bisect_right(starts, at) - 1]
            who = f'manifest line {written}: comment'
            if path is None:
                comment_findings.append((
                    manifest_rel, written,
                    f'{who} cites {label}: a comment has no file of its own, so a line it '
                    'cites is written with its path (path:N)'))
                continue
            verdict, detail = judge(who, 'comment', label, path, first, last, sentence,
                                    as_site, set(), 'a comment records no site')
            if verdict == 'held':
                comment_held += 1
            elif verdict == 'finding':
                comment_findings.append(detail)
            else:
                comment_unchecked.append(detail)
    others = [(f'emitter template {template.get("id")}', template.get('note'))
              for template in manifest.templates]
    others += [(f'fixed input {name}', item.get('note')) for name, item in manifest.fixed.items()]
    for owner, note in others:
        if not isinstance(note, str):
            continue
        for label in [row[0] for row in note_citations(note)] + note_strays(note):
            cited += 1
            unchecked.append(f'{owner}: note cites {label}: only an entry note is resolved '
                             'against the tree')
    return (findings, failed, unchecked, cited, held,
            (comment_findings, comment_unchecked, comment_cited, comment_held))


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
    inventory = load_site_inventory(root, manifest)
    by_key: dict[tuple[str, str], list[Site]] = {}
    for site in sites:
        by_key.setdefault(site.group_key, []).append(site)
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
        for site in group:
            if site.key not in inventory:
                findings.append((site.file, site.line, f'NEW {site.cls} site on {site.symbol!r}: '
                                 f'identity not in reviewed site inventory for entry {entry["id"]}; '
                                 f'scope={list(site.scope)!r} ordinal={site.ordinal} '
                                 f'({site.text})'))
                entry_findings.add(entry['id'])
        seen_classes = {s.cls for s in group}
        for cls in sorted(seen_classes - set(entry['ops'])):
            first = next(s for s in group if s.cls == cls)
            findings.append((first.file, first.line, f"op class '{cls}' on {first.symbol!r} is "
                             f'not in the ops of entry {entry["id"]} ({first.text})'))
            entry_findings.add(entry['id'])
        if entry['state'] == 'bounded':
            issues = bounded_issues(entry, group, sites, texts, manifest)
            findings.extend(issues)
            if issues:
                entry_findings.add(entry['id'])
    live_keys = {site.key for site in sites}
    for key, record in inventory.items():
        if key not in live_keys:
            entry = manifest.by_key.get((record['file'], record['symbol']))
            findings.append((record['file'], int(entry.get('line') or 0) if entry else 0,
                             f'reviewed site identity for {record["symbol"]!r} '
                             f'({entry["id"] if entry else "no entry"}) matches no site: stale; '
                             'review the scope/type/multiplicity change'))
            if entry:
                entry_findings.add(entry['id'])
    # An entry's line: field is the line of its first live site: the number
    # --emit-seed writes and --json reports as live_lines[0]. A field the scan
    # does not give back is a finding. An entry with no live site has no line
    # to compare its field with: the field is counted as not compared, never
    # as holding, and the entry is a finding of its own (stale).
    line_fields = line_holds = line_differs = line_not_compared = 0
    for entry in manifest.entries:
        declared = entry.get('line')
        if declared is not None:
            line_fields += 1
        if entry['id'] not in matched:
            not_compared = ''
            if declared is not None:
                line_not_compared += 1
                not_compared = (f'; its line: {declared!r} is not compared, the entry has no '
                                'live site')
            findings.append((entry['file'], int(entry.get('line') or 0),
                             f'entry {entry["id"]} ({entry["symbol"]!r}) matches no site: stale; '
                             f'remove or update it{not_compared}'))
            continue
        if declared is None:
            continue
        first_live = min(site.line for site in by_key[entry['file'], entry['symbol']])
        if isinstance(declared, int) and not isinstance(declared, bool) and declared == first_live:
            line_holds += 1
            continue
        line_differs += 1
        findings.append((entry['file'], first_live,
                         f'entry {entry["id"]}: line: {declared!r} is not the first live site '
                         f'of the entry ({entry["symbol"]!r}), which is line {first_live}: '
                         f'write line: {first_live}, the number --emit-seed gives'))
        entry_findings.add(entry['id'])
    manifest_rel = (str(manifest_path.relative_to(root)) if manifest_path.is_relative_to(root)
                    else str(manifest_path))
    (cite_findings, cite_failed, unchecked, cited, cited_holds,
     (comment_findings, comment_unchecked, comment_cited, comment_holds)) = check_citations(
        manifest, by_key, texts, root, comment_blocks(manifest_path.read_text()), manifest_rel)
    findings.extend(cite_findings)
    findings.extend(comment_findings)
    entry_findings.update(cite_failed)

    def pinned(kind: str, key: str, count: int) -> int:
        """Hold the number of UNCHECKED citations of one kind to its manifest
        ceiling; returns that ceiling. An UNCHECKED citation is not a finding,
        so a reworded sentence could turn a failing citation into one. The
        reviewed number is pinned exactly: one more is refused, and so is one
        fewer until the pin follows it down, or the room left under it would
        take a later UNCHECKED citation unseen."""
        ceiling = manifest.ceilings.get(key, 0)
        if count > ceiling:
            unpinned = ('' if key in manifest.ceilings else
                        f' (the manifest pins no ceilings.{key}, so none is allowed)')
            findings.append((manifest_rel, 0,
                             f'UNCHECKED {kind} citations {count} exceed the manifest '
                             f'ceiling {ceiling}{unpinned}: name what the cited line '
                             f'holds so the citation is checked, or raise ceilings.{key} '
                             'in a reviewed change'))
        elif count < ceiling:
            findings.append((manifest_rel, 0,
                             f'UNCHECKED {kind} citations {count} are below the manifest '
                             f'ceiling {ceiling}: lower ceilings.{key} to '
                             f'{count} in the same change; the number is pinned exactly, '
                             'so that the room under it cannot take a later UNCHECKED citation '
                             'unseen'))
        return ceiling

    unchecked_ceiling = pinned('note', 'cited_unchecked', len(unchecked))
    comment_ceiling = pinned('comment', 'comment_unchecked', len(comment_unchecked))
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
            findings.append((manifest_rel, 0,
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
    for line in unchecked + comment_unchecked:
        out(f'{PREFIX} UNCHECKED {line}')
    pending = sum(1 for e in opened if e['state'] == 'pending-flat-memory')
    out(f'{PREFIX} counts PASS(bounded)={len(passed)} OPEN={len(opened)} '
        f'OPEN(templates)={len(open_templates)} findings={len(findings)}')
    out(f'{PREFIX} OPEN detail pending-flat-memory={pending} '
        f'unreviewed={len(opened) - pending}: listed, never counted as PASS')
    out(f'{PREFIX} note citations={cited} cited_holds={cited_holds} '
        f'cited_fails={len(cite_findings)} cited_unchecked={len(unchecked)} '
        f'cited_unchecked_ceiling={unchecked_ceiling}: '
        'an UNCHECKED citation is listed, never counted as holding')
    out(f'{PREFIX} comments: comment_citations={comment_cited} comment_holds={comment_holds} '
        f'comment_fails={len(comment_findings)} comment_unchecked={len(comment_unchecked)} '
        f'comment_unchecked_ceiling={comment_ceiling}: '
        'a citation in a manifest comment is judged as one in a note of no entry')
    out(f'{PREFIX} entry lines: line_fields={line_fields} line_holds={line_holds} '
        f'line_differs={line_differs} line_not_compared={line_not_compared}: '
        'a line: field is the first live site of its entry; one with no live site is '
        'not compared, never counted as holding')
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
        by_key.setdefault(site.group_key, []).append(site)
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


def emit_site_inventory(root: Path, manifest_path: Path) -> int:
    """Authoring only: print identities for an explicitly reviewed snapshot.

    Verification never calls this function and never rewrites the inventory.
    Existing OPEN entry rows can retain their IDs and spelling groups; every
    concrete occurrence is pinned separately in this companion inventory.
    """
    manifest = load_manifest(root, manifest_path)
    files = sorted(set(resolve_files(root, manifest.files))
                   | set(resolve_files(root, manifest.generated)))
    sites = Scanner(set(manifest.fixed)).scan(
        {rel: (root / rel).read_text(errors='replace') for rel in files})
    if not files or not sites:
        raise GateError('cannot author an inventory from zero files or zero sites')
    print(json.dumps({'schema': SITE_SCHEMA, 'sites': [
        dict(file=site.file, symbol=site.symbol, **site.identity()) for site in sites
    ]}, indent=1))
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
    parser.add_argument('--emit-site-inventory', action='store_true',
                        help='authoring only: print scoped site identities; never a tree verdict')
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
        if args.emit_site_inventory:
            return emit_site_inventory(root, manifest_path)
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
