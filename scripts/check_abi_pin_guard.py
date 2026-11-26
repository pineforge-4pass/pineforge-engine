#!/usr/bin/env python3
"""Hold the two strategy-module stamps to the layouts they promise.

A compiled strategy module carries two stamps that a loader reads before it
uses the module. Both are defined in include/pineforge/pineforge.h:

* PF_ABI_VERSION, returned by pf_abi_version(): the C structures a caller
  allocates or strides over.
* PF_STREAM_ENGINE_LAYOUT_VERSION, returned by
  strategy_stream_engine_layout_version(): the C++ layout the live runner reads
  through a strategy handle.

This check compares one change, base -> head, and fails when the change moves
one of those layouts without strictly raising its stamp:

(A) PF_ABI_VERSION must rise when the change adds, removes or alters the
    condition of a static_assert in src/c_abi.cpp that applies sizeof, offsetof
    or alignof to a pf_* type, or changes the members of a pf_* struct or union
    defined in include/pineforge/pineforge.h (a member added, removed, renamed,
    retyped or reordered, or a definition added or removed).
(B) PF_STREAM_ENGINE_LAYOUT_VERSION must rise when the change alters a
    layout-bearing declaration of BacktestEngine, of NativeExecutionConsumer
    or of a kernel type either reaches. The types are derived from the tree,
    never listed: the closure starts at the definitions of those two classes
    (include/pineforge/engine.hpp and src/native_execution_consumer.hpp) and
    follows every kernel type named by a base clause, a non-static data member
    or a virtual function of a covered type, every alias on the way, and every
    kernel class derived from a covered one. Kernel files are the C and C++
    files under include/pineforge/ and src/ outside the source and compat
    adapter directories. Layout-bearing declarations are a type's base clause,
    its non-static data members in order (types, names, array extents and
    bit-field widths; initializers are ignored), its virtual functions in
    order, and the underlying type of an enum.
(C) PF_STREAM_ENGINE_LAYOUT_VERSION must also rise when the change alters the
    measured pins of that layout, tests/stream_engine_layout_pins.inc: a size
    or member row added, removed or given another number. The pins carry the
    stamp they were measured for, and at the head that stamp must be the
    header's: pins left behind by a raised stamp fail, whatever else changed.
    This check reads the pins as text. The measurement itself is the CTest row
    test_stream_engine_layout, which compares them with the compiled layout.

Each stamp is judged alone: a change of both kinds needs both raised. A stamp
may rise without a layout change and may never fall. Comments, white space,
line breaks, assertion messages, function bodies, non-virtual member functions
and static members are not layout and need no stamp.

(B) is deliberately wider than what the live runner reads directly: the
runner also runs its own copy of kernel functions on a module's objects, and
those functions reach types this check cannot enumerate one by one. A change
that (B) flags and that moves no pinned member still needs the stamp raised;
its pins then change in their stamp row alone.

What the comparison cannot read fails; it never passes. A file whose brackets
do not pair is compared token by token, so every edit to it counts as a layout
change until it parses again. The first branch of a preprocessor conditional
is read as code; its condition and the text of its other branches are part of
the surrounding declaration, so moving a member or an assertion under a
conditional is a change.

The range
---------
The preflight stage passes no argument; the range comes from the run:

* a pull request event (pull_request, pull_request_target): the base and head
  commits of the event payload. The head must be the checked-out commit.
* a push event: before -> after of the payload, after being the checked-out
  commit.
* any other run: the commit named by PINEFORGE_DOC_REVERT_BASE (the base a
  build host is given), else the merge base of HEAD with origin/main or main.

The change judged is merge base -> head. When the base's tip is not an ancestor
of the head, a required stamp must also exceed the tip's. A pull request or a
push whose range is missing, unreadable or empty exits 2. Outside those two
events, a HEAD that is already contained in its base has nothing to compare:
the check says so and exits 77, which preflight records as skipped.

A base that holds neither this script nor the pins is the range that
introduces both: its pins are new, and no stamp is owed for them. A base that
holds this script and no pins, and a head without pins, cannot be compared.

--base REV --head REV judge one named change (both are required together).
--root DIR judges another checkout, so a copy of this script kept outside a
change can judge that change's commits as data.

Exit 0 when no stamp is owed, 1 when one is, 2 when the comparison cannot be
made, 77 for the skip above.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from check_doc_reverts import MAIN_TIP_NOTICE

ROOT = Path(__file__).resolve().parent.parent
HEADER = 'include/pineforge/pineforge.h'
RUNTIME = 'src/c_abi.cpp'
ENGINE_HEADER = 'include/pineforge/engine.hpp'
ENGINE_CLASS = 'BacktestEngine'
CONSUMER_HEADER = 'src/native_execution_consumer.hpp'
CONSUMER_CLASS = 'NativeExecutionConsumer'
# The two classes the live runner reads through a stamped module's handle.
STREAM_ROOTS = ((ENGINE_CLASS, ENGINE_HEADER), (CONSUMER_CLASS, CONSUMER_HEADER))
PINS = 'tests/stream_engine_layout_pins.inc'
SELF = 'scripts/check_abi_pin_guard.py'
ABI_STAMP = 'PF_ABI_VERSION'
STREAM_STAMP = 'PF_STREAM_ENGINE_LAYOUT_VERSION'
KERNEL_ROOTS = ('include/pineforge/', 'src/')
ADAPTER_ROOTS = ('include/pineforge/source/', 'include/pineforge/compat/',
                 'src/source/', 'src/compat/')
LAYOUT_SUFFIXES = ('.h', '.hpp', '.cpp')
BUILD_ROOTS = ('include/', 'src/', 'runner/', 'cmake/')
BUILD_SUFFIXES = ('.h', '.hpp', '.hh', '.c', '.cc', '.cpp', '.cxx', '.inc', '.in')
MAIN_REFS = ('refs/remotes/origin/main', 'refs/heads/main')
BASE_VARIABLE = 'PINEFORGE_DOC_REVERT_BASE'
PULL_REQUEST_EVENTS = ('pull_request', 'pull_request_target')
OID = re.compile(r'[0-9a-f]{40}(?:[0-9a-f]{24})?')
PREFIX = 'abi pin guard: '

CLASS_KEYS = ('class', 'struct', 'union')
ACCESS = ('public', 'protected', 'private')
ASSERTIONS = ('static_assert', '_Static_assert', 'PF_STATIC_ASSERT')
LAYOUT_OPERATORS = ('sizeof', 'offsetof', 'alignof', '_Alignof', '__alignof__')
# A parenthesis after one of these is part of a type or a specifier, never a
# parameter list.
SPECIFIER_CALLS = frozenset(('alignas', 'decltype', '__attribute__', '__declspec',
                             'noexcept', 'sizeof', 'alignof', 'typeof', '__typeof__'))
# A member declared with one of these is not part of an object of the class.
NOT_IN_OBJECT = frozenset(('static', 'constexpr', 'thread_local', 'extern', 'inline'))
# Qualifiers that leave the size, the alignment and the order alone.
QUALIFIERS = frozenset(('const', 'volatile', 'mutable'))
IF_MARK = '__pf_if__'
ALTERNATIVE_MARK = '__pf_alternative__'
ENDIF_MARK = '__pf_endif__'
PACK_MARK = '__pf_pragma_pack__'

# A quote that follows a letter or a digit is a digit separator (1'000), never
# the start of a character literal.
LEXEME = re.compile(
    r'(?P<comment>//[^\n]*|/\*.*?\*/)'
    r'|(?P<raw>(?:u8|[uUL])?R"(?P<delimiter>[^()\\\s"]{0,16})\(.*?\)(?P=delimiter)")'
    r'|(?P<string>(?:u8|[uUL])?"(?:\\.|[^"\\\n])*")'
    r"|(?P<character>(?<![0-9A-Za-z_])(?:u8|[uUL])?'(?:\\.|[^'\\\n])+')",
    re.S)
# '<' and '>' stay single tokens, and '>>' two: they open and close template
# argument lists. Comparisons and shifts that hold a '<' or a '>' are one token
# each, so they are never read as a bracket.
TOKEN = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*"
    r"|\.?[0-9](?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*"
    r"|<<=|>>=|<=>|<=|>=|<<|::|->|==|!=|&&|\|\||\"\"|''|\S")
DIRECTIVE = re.compile(r'[ \t]*#[ \t]*([A-Za-z_]*)(.*)', re.S)
TYPE_KEYWORD = re.compile(r'\b(?:class|struct|union|enum)\b')
INTEGER = re.compile(r'\(?[ \t]*([0-9]+)[uUlL]*[ \t]*\)?')
# The three row forms of the pin file.
PIN_ROW = re.compile(
    r'PF_STREAM_LAYOUT_STAMP\(\s*(?P<stamp>[0-9]+)[uU]?\s*\)'
    r'|PF_STREAM_LAYOUT_SIZE\(\s*(?P<sized>[A-Za-z_]\w*)\s*,\s*(?P<bytes>[0-9]+)\s*\)'
    r'|PF_STREAM_LAYOUT_MEMBER\(\s*(?P<type>[A-Za-z_]\w*)\s*,\s*(?P<member>[A-Za-z_][\w.]*)\s*,'
    r'\s*(?P<offset>[0-9]+)\s*,\s*(?P<size>[0-9]+)\s*\)')


class NotCompared(Exception):
    """The comparison cannot be made; exit status 2."""


class Unparsed(Exception):
    """One file does not have the shape the reader needs."""


def short_digest(text: str) -> str:
    return hashlib.sha256(' '.join(text.split()).encode('utf-8')).hexdigest()[:16]


def is_word(token: str) -> bool:
    return token[0] == '_' or token[0].isalpha()


def _neutral(match: re.Match) -> str:
    text = match.group(0)
    if match.group('comment') is not None:
        return ' ' + '\n' * text.count('\n')
    if match.group('raw') is not None or match.group('string') is not None:
        return '""' + '\n' * text.count('\n')
    if match.group('character') is not None:
        return "''"
    return text


def without_comments(text: str) -> str:
    """Comments become white space; string and character literals lose their text."""
    return LEXEME.sub(_neutral, text)


def without_directives(clean: str) -> str:
    """Drop preprocessor lines; keep what a conditional says as marker statements.

    The first branch of a conditional stays as code. Its condition, and the text
    of every later branch, become statements of their own, so a declaration that
    moves under a conditional, or whose other branch changes, reads differently.
    """
    kept: list[str] = []
    frames: list[dict] = []
    nested = 0
    lines = clean.split('\n')
    index = 0
    while index < len(lines):
        line = lines[index]
        index += 1
        dropped = bool(frames) and frames[-1]['dropped']
        if not line.lstrip().startswith('#'):
            if dropped:
                frames[-1]['other'].append(line)
            else:
                kept.append(line)
            continue
        while line.endswith('\\') and index < len(lines):
            line = line[:-1] + ' ' + lines[index]
            index += 1
        found = DIRECTIVE.match(line)
        name = found.group(1) if found else ''
        rest = found.group(2) if found else ''
        if dropped:
            if name in ('if', 'ifdef', 'ifndef'):
                nested += 1
            elif name == 'endif':
                if nested == 0:
                    other = '\n'.join(frames.pop()['other'])
                    # pfq: the unread branch holds a layout assertion on a pf_*
                    # type; pft: it may hold a type definition.
                    if 'pf_' in other and any(word in other for word in ASSERTIONS):
                        kind = 'pfq_'
                    elif TYPE_KEYWORD.search(other):
                        kind = 'pft_'
                    else:
                        kind = 'pfa_'
                    kept.append(f'{ALTERNATIVE_MARK} {kind}{short_digest(other)} ;')
                    kept.append(f'{ENDIF_MARK} ;')
                    continue
                nested -= 1
            frames[-1]['other'].append(line)
        elif name in ('if', 'ifdef', 'ifndef'):
            frames.append({'dropped': False, 'other': []})
            kept.append(f'{IF_MARK} pfc_{short_digest(name + " " + rest)} ;')
        elif name in ('elif', 'else'):
            if not frames:
                raise Unparsed('#' + name + ' outside a conditional')
            frames[-1]['dropped'] = True
            frames[-1]['other'].append(line)
        elif name == 'endif':
            if not frames:
                raise Unparsed('#endif outside a conditional')
            frames.pop()
            kept.append(f'{ENDIF_MARK} ;')
        elif name == 'pragma' and 'pack' in rest:
            kept.append(f'{PACK_MARK} pfp_{short_digest(rest)} ;')
    if frames:
        raise Unparsed('a preprocessor conditional is not closed')
    return '\n'.join(kept)


def paired_brackets(tokens: list[str]) -> dict[int, int]:
    pairs = {')': '(', ']': '[', '}': '{'}
    match: dict[int, int] = {}
    stack: list[int] = []
    for index, token in enumerate(tokens):
        if token in ('(', '[', '{'):
            stack.append(index)
        elif token in pairs:
            if not stack or tokens[stack[-1]] != pairs[token]:
                raise Unparsed(f'unpaired {token}')
            match[stack.pop()] = index
    if stack:
        raise Unparsed(f'unclosed {tokens[stack[-1]]}')
    return match


class Record:
    """One class, struct, union, enum or alias definition, as its layout reads."""

    def __init__(self, key: str, name: str, kind: str, path: str) -> None:
        self.key = key
        self.name = name
        self.kind = kind
        self.path = path
        self.names: list[str] = []
        self.head: list[str] = [kind]
        self.members: list[str] = []

    def text(self) -> str:
        if self.kind in ('enum', 'alias'):
            return ' '.join(self.head)
        return ' '.join(self.head) + ' { ' + ' ; '.join(self.members) + ' }'

    def words(self) -> list[str]:
        return ' '.join(self.head + self.members).split()


class Source:
    """The declarations of one file."""

    def __init__(self, path: str, text: str) -> None:
        self.path = path
        self.tokens: list[str] = TOKEN.findall(without_directives(without_comments(text)))
        self.match = paired_brackets(self.tokens)
        self.records: list[Record] = []
        # Unread conditional branches outside any class that may define a type.
        self.unread_types: list[str] = []
        self._scope(0, len(self.tokens), ())

    # -- helpers ---------------------------------------------------------
    def _after_angles(self, index: int, end: int) -> int:
        """tokens[index] is '<': the index after the '>' that closes it."""
        tokens, match = self.tokens, self.match
        depth = 0
        while index < end:
            token = tokens[index]
            if token in ('(', '[', '{'):
                index = match[index] + 1
                continue
            if token == '<':
                depth += 1
            elif token == '>':
                depth -= 1
                if depth == 0:
                    return index + 1
            elif token == ';':
                break
            index += 1
        raise Unparsed('a template argument list is not closed')

    def _after_statement(self, index: int, end: int) -> int:
        """The index after the statement at index: its ';' or its first block."""
        tokens, match = self.tokens, self.match
        while index < end:
            token = tokens[index]
            if token == ';':
                return index + 1
            if token == '{':
                return match[index] + 1
            if token in ('(', '['):
                index = match[index] + 1
            else:
                index += 1
        return end

    def _after_declarators(self, index: int, end: int) -> int:
        """The index of the ';' that ends the declarators starting at index."""
        tokens, match = self.tokens, self.match
        while index < end and tokens[index] != ';':
            index = match[index] + 1 if tokens[index] in ('(', '[', '{') else index + 1
        return index

    def _function_like(self, start: int, stop: int) -> bool:
        """Whether tokens[start:stop] declare a function rather than an object."""
        tokens, match = self.tokens, self.match
        depth = 0
        index = start
        while index < stop:
            token = tokens[index]
            if token == 'operator' and depth == 0:
                return True
            if token in ('[', '{'):
                index = match[index] + 1
                continue
            if token == '(':
                close = match[index]
                # Not a parameter list: the argument of a specifier, or the
                # parenthesised width of a bit-field.
                before = '' if index == start else tokens[index - 1]
                if depth == 0 and before not in SPECIFIER_CALLS and before != ':':
                    inner = tokens[index + 1:close]
                    pointer = bool(inner) and (inner[0] in ('*', '&') or any(
                        inner[at] == '::' and inner[at + 1] == '*'
                        for at in range(len(inner) - 1)))
                    return not pointer
                index = close + 1
                continue
            if depth == 0 and token == '=':
                return False
            if token == '<':
                depth += 1
            elif token == '>' and depth:
                depth -= 1
            index += 1
        return False

    def _object_text(self, start: int, stop: int) -> str | None:
        """The layout text of a data member, or None for a member outside the object."""
        tokens, match = self.tokens, self.match
        kept: list[str] = []
        depth = 0
        initializer = False
        index = start
        while index < stop:
            token = tokens[index]
            if token in ('(', '['):
                close = match[index]
                if not initializer:
                    kept.extend(tokens[index:close + 1])
                index = close + 1
                continue
            if token == '{':
                index = match[index] + 1
                continue
            if depth == 0 and token == '=':
                initializer = True
            elif depth == 0 and token == ',':
                initializer = False
                kept.append(token)
            elif not initializer:
                if token == '<':
                    depth += 1
                elif token == '>' and depth:
                    depth -= 1
                if depth == 0 and token in NOT_IN_OBJECT:
                    return None
                if token not in QUALIFIERS:
                    kept.append(token)
            index += 1
        return ' '.join(kept) if kept else None

    # -- scopes ----------------------------------------------------------
    def _scope(self, index: int, end: int, scope: tuple[str, ...]) -> None:
        tokens, match = self.tokens, self.match
        while index < end:
            token = tokens[index]
            if token == ';':
                index += 1
            elif token == '{':
                index = match[index] + 1
            elif token == 'namespace' or (token == 'inline' and index + 1 < end
                                          and tokens[index + 1] == 'namespace'):
                inline = token == 'inline'
                at = index + (2 if inline else 1)
                names: list[str] = []
                while at < end and tokens[at] not in ('{', ';', '='):
                    if is_word(tokens[at]) and tokens[at] != 'inline':
                        names.append(tokens[at])
                    at += 1
                if at < end and tokens[at] == '{':
                    self._scope(at + 1, match[at], scope if inline else scope + tuple(names))
                    index = match[at] + 1
                else:
                    index = self._after_statement(index, end)
            elif (token == 'extern' and index + 2 < end and tokens[index + 1] == '""'
                  and tokens[index + 2] == '{'):
                self._scope(index + 3, match[index + 2], scope)
                index = match[index + 2] + 1
            elif (token == ALTERNATIVE_MARK and index + 1 < end
                  and tokens[index + 1].startswith(('pft_', 'pfq_'))):
                self.unread_types.append(tokens[index + 1])
                index += 2
            else:
                index = self._declaration(index, end, scope, None)

    def _body(self, index: int, end: int, scope: tuple[str, ...], owner: Record) -> None:
        tokens, match = self.tokens, self.match
        while index < end:
            token = tokens[index]
            if token == ';':
                index += 1
            elif token == '{':
                index = match[index] + 1
            else:
                index = self._declaration(index, end, scope, owner)

    def _declaration(self, index: int, end: int, scope: tuple[str, ...],
                     owner: Record | None) -> int:
        tokens, match = self.tokens, self.match
        while index < end:
            token = tokens[index]
            if token == '[' and index + 1 < end and tokens[index + 1] == '[':
                index = match[index] + 1
            elif token == 'template' and index + 1 < end and tokens[index + 1] == '<':
                index = self._after_angles(index + 1, end)
            elif token == 'template':
                index += 1
            else:
                break
        if index >= end:
            return end
        token = tokens[index]
        if owner is not None and token in ACCESS and index + 1 < end and tokens[index + 1] == ':':
            return index + 2
        if token in ('friend', 'static_assert'):
            return self._after_statement(index, end)
        if token == 'using':
            return self._using(index, end, scope)
        typedef = token == 'typedef'
        at = index + 1 if typedef else index
        if at < end and tokens[at] in CLASS_KEYS:
            after = self._class(at, end, scope, owner, typedef)
            if after is not None:
                return after
        elif at < end and tokens[at] == 'enum':
            after = self._enum(at, end, scope, owner, typedef)
            if after is not None:
                return after
        if typedef:
            return self._typedef(index, end, scope)
        if (is_word(token) and index + 1 < end and tokens[index + 1] == '('
                and token not in SPECIFIER_CALLS and token != 'operator'
                and (owner is None or token != owner.name)):
            # A name called at declaration level is a macro. Inside a class it
            # may declare anything, so its spelling is part of the layout.
            close = match[index + 1]
            if owner is not None:
                owner.members.append('macro ' + ' '.join(tokens[index:close + 1]))
            after = close + 1
            return after + 1 if after < end and tokens[after] == ';' else after
        if owner is None:
            return self._after_statement(index, end)
        return self._member(index, end, owner)

    def _member(self, start: int, end: int, owner: Record) -> int:
        tokens, match = self.tokens, self.match
        index = start
        body = None
        while index < end:
            token = tokens[index]
            if token in ('(', '['):
                index = match[index] + 1
            elif token == '{':
                if self._function_like(start, index):
                    body = index
                    index = match[index] + 1
                    break
                index = match[index] + 1
            elif token == ';':
                break
            else:
                index += 1
        stop = index if body is None else body
        after = index if body is not None else min(index + 1, end)
        if stop <= start:
            return max(after, start + 1)
        if self._function_like(start, stop):
            call = start
            while call < stop and tokens[call] != '(':
                call += 1
            if 'virtual' in tokens[start:call]:
                owner.members.append(' '.join(tokens[start:stop]))
            return after
        text = self._object_text(start, stop)
        if text is not None:
            owner.members.append(text)
        return after

    def _class(self, index: int, end: int, scope: tuple[str, ...], owner: Record | None,
               typedef: bool) -> int | None:
        """tokens[index] is a class key. The index after a definition or a
        forward declaration, or None when the statement is neither."""
        tokens, match = self.tokens, self.match
        kind = 'union' if tokens[index] == 'union' else 'record'
        at = index + 1
        extra: list[str] = []
        name: list[str] = []
        chains = 0
        while at < end:
            token = tokens[at]
            if token == '[':
                at = match[at] + 1
            elif (token in ('alignas', '__attribute__', '__declspec') and at + 1 < end
                  and tokens[at + 1] == '('):
                extra.extend(tokens[at:match[at + 1] + 1])
                at = match[at + 1] + 1
            elif is_word(token) and token != 'final':
                name = [token]
                chains += 1
                at += 1
                while at + 1 < end and tokens[at] == '::' and is_word(tokens[at + 1]):
                    name.append(tokens[at + 1])
                    at += 2
            else:
                break
        if at < end and tokens[at] == '<':
            at = self._after_angles(at, end)
        if at < end and tokens[at] == 'final':
            at += 1
        bases: list[str] = []
        if at < end and tokens[at] == ':':
            brace = at + 1
            while brace < end and tokens[brace] not in ('{', ';'):
                brace = match[brace] + 1 if tokens[brace] in ('(', '[') else brace + 1
            if brace >= end or tokens[brace] != '{':
                return None
            bases = [token for token in tokens[at + 1:brace] if token not in ACCESS]
            at = brace
        if at >= end or tokens[at] != '{':
            if at < end and tokens[at] == ';' and chains == 1 and not typedef:
                return at + 1
            return None
        close = match[at]
        record = Record('::'.join(scope + tuple(name)), name[-1] if name else '', kind, self.path)
        record.head = [kind] + extra + bases
        self._body(at + 1, close, scope + tuple(name), record)
        semicolon = self._after_declarators(close + 1, end)
        declarators = tokens[close + 1:semicolon]
        if '=' in declarators:
            declarators = declarators[:declarators.index('=')]
        declarators = [token for token in declarators if token not in QUALIFIERS]
        after = min(semicolon + 1, end)
        aliases = [token for token in declarators if is_word(token)]
        if typedef and not name and aliases:
            record.key = '::'.join(scope + (aliases[0],))
            record.name = aliases[0]
            aliases = aliases[1:]
            name = [record.name]
        if name:
            self.records.append(record)
            if typedef:
                # typedef struct tag { ... } name_t: both names are the type.
                record.names = aliases
                for alias in aliases:
                    if alias != record.name:
                        self._alias(scope, alias, [record.name])
        if owner is not None and not typedef:
            if not name:
                owner.members.append(' '.join(
                    [kind, '{', ' ; '.join(record.members), '}'] + declarators))
            elif declarators:
                owner.members.append(' '.join([record.name] + declarators))
        return after

    def _enum(self, index: int, end: int, scope: tuple[str, ...], owner: Record | None,
              typedef: bool) -> int | None:
        tokens, match = self.tokens, self.match
        at = index + 1
        if at < end and tokens[at] in ('class', 'struct'):
            at += 1
        name: list[str] = []
        while at < end and (tokens[at] == '::' or is_word(tokens[at])):
            if tokens[at] != '::':
                name.append(tokens[at])
            at += 1
        underlying: list[str] = []
        if at < end and tokens[at] == ':':
            at += 1
            while at < end and tokens[at] not in ('{', ';'):
                underlying.append(tokens[at])
                at += 1
        if at >= end or len(name) > 1 and tokens[at] != '{':
            return None
        if tokens[at] == ';':
            if len(name) != 1 or typedef:
                return None
            after = at + 1
            declarators: list[str] = []
        elif tokens[at] == '{':
            semicolon = self._after_declarators(match[at] + 1, end)
            declarators = tokens[match[at] + 1:semicolon]
            if '=' in declarators:
                declarators = declarators[:declarators.index('=')]
            declarators = [token for token in declarators if token not in QUALIFIERS]
            after = min(semicolon + 1, end)
        else:
            return None
        if name:
            record = Record('::'.join(scope + tuple(name)), name[-1], 'enum', self.path)
            record.head = ['enum'] + [token for token in underlying if token not in QUALIFIERS]
            self.records.append(record)
            if typedef:
                record.names = [token for token in declarators if is_word(token)]
                for alias in record.names:
                    if alias != record.name:
                        self._alias(scope, alias, [record.name])
        if owner is not None and declarators and not typedef:
            owner.members.append(' '.join(['enum'] + name[-1:] + declarators))
        return after

    def _alias(self, scope: tuple[str, ...], name: str, target: list[str]) -> None:
        record = Record('::'.join(scope + (name,)), name, 'alias', self.path)
        record.head = ['alias'] + [token for token in target if token not in QUALIFIERS]
        self.records.append(record)

    def _using(self, index: int, end: int, scope: tuple[str, ...]) -> int:
        tokens = self.tokens
        after = self._after_statement(index, end)
        if index + 2 < end and is_word(tokens[index + 1]) and tokens[index + 2] == '=':
            self._alias(scope, tokens[index + 1], tokens[index + 3:after - 1])
        return after

    def _typedef(self, index: int, end: int, scope: tuple[str, ...]) -> int:
        tokens = self.tokens
        after = self._after_statement(index, end)
        body = tokens[index + 1:after - 1]
        words = [token for token in body if is_word(token)]
        if words:
            name = words[-1]
            for at in range(len(body) - 2):
                if body[at] == '(' and body[at + 1] == '*' and is_word(body[at + 2]):
                    name = body[at + 2]
                    break
            self._alias(scope, name, [token for token in body if token != name])
        return after


def qualified_names(words: list[str]) -> set[str]:
    """Every name in words, with the qualification written before it."""
    names: set[str] = set()
    index = 0
    while index < len(words):
        if is_word(words[index]):
            chain = [words[index]]
            index += 1
            while index + 1 < len(words) and words[index] == '::' and is_word(words[index + 1]):
                chain.append(words[index + 1])
                index += 2
            names.add('::'.join(chain))
        else:
            index += 1
    return names


class Layout:
    """The layout-bearing declarations one root class reaches in one commit."""

    def __init__(self) -> None:
        self.digests: dict[str, str] = {}
        self.places: dict[str, str] = {}
        self.covered: set[str] = set()
        self.opaque: dict[str, str] = {}
        self.reasons: dict[str, str] = {}
        self.files = 0


class Tree:
    """One commit: its stamps, and its files, read when a rule asks for them."""

    def __init__(self, root: Path, commit: str, cache: dict[tuple[str, str], object]) -> None:
        self.root = root
        self.commit = commit
        self.cache = cache
        self.tree = git_text(root, 'rev-parse', '--verify', '--quiet', commit + '^{tree}')
        self.entries = tree_entries(root, commit)
        self.texts: dict[str, str] = {}
        self.layouts: dict[tuple, Layout] = {}
        build = sorted(path for path in self.entries if stamp_scanned(path))
        self.read([HEADER, *build])
        self.stamps = {name: stamp_value(name, self.texts, build)
                       for name in sorted({rule.stamp for rule in RULES})}

    def read(self, paths: list[str]) -> None:
        missing = [path for path in paths if path not in self.texts]
        for path in missing:
            if path not in self.entries:
                raise NotCompared(f'{self.commit[:12]} has no regular file {path}: a boundary '
                                  'file moved; this check must move with it in the same change')
        blobs = read_blobs(self.root, [self.entries[path] for path in missing])
        for path in missing:
            self.texts[path] = blobs[self.entries[path]].decode('utf-8', 'replace')

    def parsed(self, path: str) -> Source | str:
        """The declarations of one file, or the reason they cannot be read."""
        self.read([path])
        key = (self.entries[path], path)
        if key not in self.cache:
            try:
                self.cache[key] = Source(path, self.texts[path])
            except (Unparsed, IndexError, KeyError, RecursionError) as error:
                self.cache[key] = (str(error) if isinstance(error, Unparsed)
                                   else f'{type(error).__name__} while reading declarations')
        return self.cache[key]

    def source(self, path: str) -> Source:
        """The declarations of a file the comparison cannot do without."""
        parsed = self.parsed(path)
        if not isinstance(parsed, Source):
            raise NotCompared(f'{path} at {self.commit[:12]} cannot be read: {parsed}')
        return parsed

    def layout(self, classes: tuple[tuple[str, str], ...], roots: tuple[str, ...],
               excluded: tuple[str, ...]) -> Layout:
        """What the (class, defining header) pairs reach in the files under roots."""
        wanted = (classes, roots, excluded)
        if wanted not in self.layouts:
            layout = Layout()
            for _, header in classes:
                self.source(header)
            paths = sorted(path for path in self.entries
                           if path.startswith(roots) and not path.startswith(excluded)
                           and path.endswith(LAYOUT_SUFFIXES))
            self.read(paths)
            table: dict[str, list[Record]] = {}
            unread: dict[str, str] = {}
            for path in paths:
                parsed = self.parsed(path)
                if isinstance(parsed, Source):
                    for record in parsed.records:
                        table.setdefault(record.key, []).append(record)
                    if parsed.unread_types:
                        unread['unread conditional branch of ' + path] = path
                        layout.digests['unread conditional branch of ' + path] = short_digest(
                            ' '.join(parsed.unread_types))
                else:
                    layout.opaque[path] = short_digest(' '.join(
                        TOKEN.findall(without_comments(self.texts[path]))))
                    layout.reasons[path] = parsed
            for key, records in table.items():
                layout.digests[key] = short_digest(' | '.join(record.text() for record in records))
                layout.places[key] = records[0].path
            layout.covered = covered_types(table, self.commit, classes)
            # A branch the reader does not follow may define a covered type.
            layout.covered.update(unread)
            layout.places.update(unread)
            layout.files = len(paths)
            self.layouts[wanted] = layout
        return self.layouts[wanted]


def git(root: Path, *args: str, data: bytes | None = None) -> bytes:
    try:
        result = subprocess.run(['git', '--no-replace-objects', '-C', str(root), *args],
                                input=data, capture_output=True)
    except OSError as error:
        raise NotCompared(f'git {args[0]} cannot run: {error}') from error
    if result.returncode:
        reason = result.stderr.decode('utf-8', 'replace').strip().splitlines()
        raise NotCompared(f'git {args[0]} failed' + (': ' + reason[-1] if reason else ''))
    return result.stdout


def git_text(root: Path, *args: str) -> str:
    return git(root, *args).decode('utf-8', 'replace').strip()


def commit_of(root: Path, revision: str, what: str) -> str:
    try:
        value = git_text(root, 'rev-parse', '--verify', '--quiet', '--end-of-options',
                         revision + '^{commit}')
    except NotCompared:
        value = ''
    if not OID.fullmatch(value):
        raise NotCompared(f'the {what} {revision!r} is not a commit in this repository')
    return value


def optional_commit(root: Path, revision: str) -> str | None:
    try:
        return commit_of(root, revision, 'ref')
    except NotCompared:
        return None


def tree_entries(root: Path, commit: str) -> dict[str, str]:
    """Path -> blob id for every regular file of the commit."""
    entries: dict[str, str] = {}
    listing = git(root, 'ls-tree', '-r', '-z', '--full-tree', commit)
    for row in listing.split(b'\0'):
        if not row:
            continue
        metadata, separator, raw = row.partition(b'\t')
        parts = metadata.decode('ascii', 'replace').split()
        if not separator or len(parts) != 3:
            raise NotCompared(f'git ls-tree gave an unreadable row for {commit[:12]}')
        mode, kind, oid = parts
        path = raw.decode('utf-8', 'replace')
        if kind == 'blob' and mode in ('100644', '100755'):
            entries[path] = oid
        elif path in (HEADER, RUNTIME, ENGINE_HEADER, CONSUMER_HEADER, PINS):
            raise NotCompared(f'{path} at {commit[:12]} is not a regular file')
    return entries


def read_blobs(root: Path, oids: list[str]) -> dict[str, bytes]:
    wanted = sorted(set(oids))
    blobs: dict[str, bytes] = {}
    if not wanted:
        return blobs
    output = git(root, 'cat-file', '--batch', data=('\n'.join(wanted) + '\n').encode('ascii'))
    position = 0
    for oid in wanted:
        line_end = output.find(b'\n', position)
        header = output[position:line_end].split() if line_end >= 0 else []
        if len(header) != 3 or header[0].decode('ascii', 'replace') != oid or header[1] != b'blob':
            raise NotCompared(f'git cat-file cannot read blob {oid}')
        size = int(header[2])
        body = output[line_end + 1:line_end + 1 + size]
        if len(body) != size:
            raise NotCompared(f'git cat-file returned a short blob {oid}')
        blobs[oid] = body
        position = line_end + 1 + size + 1
    return blobs


def stamp_scanned(path: str) -> bool:
    name = path.rsplit('/', 1)[-1]
    if path == 'CMakeLists.txt':
        return True
    if not path.startswith(BUILD_ROOTS):
        return False
    return name == 'CMakeLists.txt' or name.endswith('.cmake') or name.endswith(BUILD_SUFFIXES)


def stamp_value(name: str, texts: dict[str, str], build: list[str]) -> int:
    """The one value of a stamp, or NotCompared for none, several or a redefinition."""
    define = re.compile(r'^[ \t]*#[ \t]*define[ \t]+' + name + r'\b[ \t]*(.*)$', re.M)
    other = re.compile(r'^[ \t]*#[ \t]*(?:define|undef)[ \t]+' + name + r'\b', re.M)
    command_line = re.compile(r'-D[ \t]*' + name + r'\b|\b' + name + r'[ \t]*=')
    header = without_comments(texts[HEADER])
    found = define.findall(header)
    if len(found) != 1 or len(other.findall(header)) != 1:
        raise NotCompared(f'{HEADER} defines or undefines {name} {len(other.findall(header))} '
                          'times; exactly one definition is read')
    value = INTEGER.fullmatch(found[0].strip())
    if value is None:
        raise NotCompared(f'{HEADER} defines {name} as {found[0].strip()!r}, not one integer')
    for path in build:
        if path == HEADER or name not in texts[path]:
            continue
        name_of = path.rsplit('/', 1)[-1]
        if name_of == 'CMakeLists.txt' or name_of.endswith('.cmake'):
            if command_line.search(texts[path]):
                raise NotCompared(f'{path} sets {name} for the build; the stamp has one '
                                  f'definition, in {HEADER}')
        elif other.search(without_comments(texts[path])):
            raise NotCompared(f'{path} defines or undefines {name}; the stamp has one '
                              f'definition, in {HEADER}')
    return int(value.group(1))


def abi_pins(source: Source) -> Counter:
    """Every size, offset or alignment assertion on a pf_* type, with its conditionals."""
    tokens, match = source.tokens, source.match
    pins: Counter = Counter()
    conditions: list[str] = []
    for index, token in enumerate(tokens):
        if token == IF_MARK and index + 1 < len(tokens):
            conditions.append(tokens[index + 1])
        elif token == ALTERNATIVE_MARK and index + 1 < len(tokens):
            # The branch that is not read as code: its whole text is the pin.
            if tokens[index + 1].startswith('pfq_'):
                pins[' '.join(conditions + ['unread branch', tokens[index + 1]])] += 1
        elif token == ENDIF_MARK:
            if conditions:
                conditions.pop()
        elif token in ASSERTIONS and index + 1 < len(tokens) and tokens[index + 1] == '(':
            close = match[index + 1]
            stop = close
            while stop - 1 > index + 1 and tokens[stop - 1] == '""':
                stop -= 1
            if stop < close and tokens[stop - 1] == ',':
                stop -= 1
            else:
                stop = close
            pinned = False
            for at in range(index + 2, stop - 1):
                if tokens[at] in LAYOUT_OPERATORS and tokens[at + 1] == '(':
                    inner = tokens[at + 2:match[at + 1]]
                    if any(is_word(word) and word.startswith('pf_') for word in inner):
                        pinned = True
                        break
            if pinned:
                pins[' '.join(conditions + tokens[index + 2:stop])] += 1
    return pins


def abi_structs(source: Source) -> dict[str, str]:
    """Name -> member list for every pf_* struct or union the header defines."""
    structs: dict[str, str] = {}
    for record in source.records:
        if record.kind not in ('record', 'union'):
            continue
        for name in [record.name] + record.names:
            if name.startswith('pf_'):
                structs[name] = (structs[name] + ' | ' if name in structs else '') + record.text()
    return structs


def covered_types(table: dict[str, list[Record]], commit: str,
                  classes: tuple[tuple[str, str], ...]) -> set[str]:
    """The root classes and every type of the table their layouts reach."""
    by_name: dict[str, list[str]] = {}
    for key in table:
        by_name.setdefault(key.rsplit('::', 1)[-1], []).append(key)

    def resolve(name: str) -> list[str]:
        candidates = by_name.get(name.rsplit('::', 1)[-1], ())
        exact = [key for key in candidates if key == name or key.endswith('::' + name)]
        return exact if exact else list(candidates)

    roots: list[str] = []
    for root_class, root_header in classes:
        defined = [key for key in by_name.get(root_class, ())
                   if any(record.kind == 'record' and record.path == root_header
                          for record in table[key])]
        if not defined:
            raise NotCompared(f'{root_header} at {commit[:12]} does not define {root_class}')
        roots += defined
    derived: dict[str, set[str]] = {}
    reached: dict[str, set[str]] = {}
    for key, records in table.items():
        targets: set[str] = set()
        for record in records:
            for name in qualified_names(record.words()):
                targets.update(resolve(name))
            if record.kind in ('record', 'union'):
                for name in qualified_names(record.head):
                    for base in resolve(name):
                        derived.setdefault(base, set()).add(key)
        reached[key] = targets
    covered = set(roots)
    queue = list(roots)
    while queue:
        key = queue.pop()
        for target in reached[key] | derived.get(key, set()):
            if target not in covered:
                covered.add(target)
                queue.append(target)
    return covered


def event_commits() -> tuple[str, str, str] | None:
    """(base, head, label) of a pull request or push event; None outside both."""
    name = os.environ.get('GITHUB_EVENT_NAME') or ''
    if name not in PULL_REQUEST_EVENTS and name != 'push':
        return None
    path = os.environ.get('GITHUB_EVENT_PATH') or ''
    try:
        event = json.loads(Path(path).read_bytes().decode('utf-8'))
    except (OSError, UnicodeError, ValueError) as error:
        raise NotCompared(f'the {name} event payload cannot be read') from error
    try:
        if name == 'push':
            base, head = event['before'], event['after']
        else:
            base = event['pull_request']['base']['sha']
            head = event['pull_request']['head']['sha']
    except (KeyError, TypeError) as error:
        raise NotCompared(f'the {name} event payload names no base and head') from error
    for value in (base, head):
        if not isinstance(value, str) or not OID.fullmatch(value) or not value.strip('0'):
            raise NotCompared(f'the {name} event payload has an unusable commit id')
    return base, head, name + ' event'


class Range:
    def __init__(self, tip: str, head: str, merge_base: str, label: str, bound: bool) -> None:
        self.tip = tip
        self.head = head
        self.merge_base = merge_base
        self.label = label
        # A pull request, a push or a named pair must have something to compare.
        self.bound = bound


def find_range(root: Path, base: str | None, head: str | None) -> Range:
    if git_text(root, 'rev-parse', '--is-shallow-repository') != 'false':
        raise NotCompared('the checkout is shallow; the range needs its history')
    checked_out = commit_of(root, 'HEAD', 'checked-out commit')
    if (base is None) != (head is None):
        raise NotCompared('--base and --head are given together or not at all')
    if base is not None and head is not None:
        tip, top = commit_of(root, base, 'base'), commit_of(root, head, 'head')
        label, bound = 'arguments', True
    else:
        event = event_commits()
        if event is not None:
            tip = commit_of(root, event[0], 'event base')
            top = commit_of(root, event[1], 'event head')
            label, bound = event[2], True
            if top != checked_out:
                raise NotCompared(f'the event head {top[:12]} is not the checked-out commit '
                                  f'{checked_out[:12]}')
        else:
            top, bound = checked_out, False
            shipped = os.environ.get(BASE_VARIABLE) or ''
            if shipped:
                tip, label = commit_of(root, shipped, BASE_VARIABLE), BASE_VARIABLE
            else:
                for ref in MAIN_REFS:
                    found = optional_commit(root, ref)
                    if found is not None:
                        tip, label = found, 'merge base with ' + ref
                        break
                else:
                    raise NotCompared(
                        f'no pull request or push event, no {BASE_VARIABLE} and no main ref: '
                        'the range cannot be established')
    bases = git_text(root, 'merge-base', '--all', tip, top).split()
    if len(bases) != 1 or not OID.fullmatch(bases[0]):
        raise NotCompared(f'{tip[:12]} and {top[:12]} have {len(bases)} merge bases; one is needed')
    return Range(tip, top, bases[0], label, bound)


def owed(name: str, rule: str, changes: list[str], base: int, head: int,
         tip: int | None) -> list[str]:
    """The failures of one stamp."""
    failures = []
    if head < base:
        failures.append(f'FAIL {rule}: {name} fell from {base} to {head}; a stamp never decreases')
    elif changes and head == base:
        failures.append(f'FAIL {rule}: {len(changes)} layout change(s) need {name} raised above '
                        f'{base}; it is still {head}')
    elif changes and tip is not None and head <= tip:
        failures.append(f'FAIL {rule}: {len(changes)} layout change(s) need {name} raised above '
                        f"{tip}, the value at the base's tip; it is {head}")
    return failures


class Finding:
    """What one rule saw between two commits."""

    def __init__(self) -> None:
        # What needs the rule's stamp raised.
        self.changes: list[str] = []
        # What is wrong at the head whatever the stamp did.
        self.failures: list[str] = []
        self.facts = ''
        self.notes: list[str] = []
        self.covered: dict[str, str] = {}


def abi_changes(base: Tree, head: Tree) -> Finding:
    """Rule (A): the pins of src/c_abi.cpp and the pf_* members of pineforge.h."""
    finding = Finding()
    old_pins, new_pins = abi_pins(base.source(RUNTIME)), abi_pins(head.source(RUNTIME))
    old_structs = abi_structs(base.source(HEADER))
    new_structs = abi_structs(head.source(HEADER))
    for pin in sorted((old_pins - new_pins).elements()):
        finding.changes.append(f'{RUNTIME}: pin removed: {pin}')
    for pin in sorted((new_pins - old_pins).elements()):
        finding.changes.append(f'{RUNTIME}: pin added: {pin}')
    for name in sorted(set(old_structs) | set(new_structs)):
        if old_structs.get(name) != new_structs.get(name):
            state = ('added' if name not in old_structs
                     else 'removed' if name not in new_structs else 'members changed')
            finding.changes.append(f'{HEADER}: {name}: {state}')
    finding.facts = (f'pins base={sum(old_pins.values())} head={sum(new_pins.values())}; '
                     f'pf structs base={len(old_structs)} head={len(new_structs)}')
    return finding


def layout_changes(classes: tuple[tuple[str, str], ...], roots: tuple[str, ...],
                   excluded: tuple[str, ...]):
    """Rule (B) for some root classes: their layouts and those of every type they reach."""

    def compare(base: Tree, head: Tree) -> Finding:
        finding = Finding()
        old = base.layout(classes, roots, excluded)
        new = head.layout(classes, roots, excluded)
        for key in sorted(old.covered | new.covered):
            before, after = old.digests.get(key), new.digests.get(key)
            if before is None or before == after:
                continue
            place = (new.places if after is not None else old.places)[key]
            finding.changes.append(
                f'{place}: {key}: ' + ('removed' if after is None else 'layout changed'))
        for path in sorted(set(old.opaque) | set(new.opaque)):
            if old.opaque.get(path) != new.opaque.get(path):
                reason = new.reasons.get(path) or old.reasons.get(path)
                finding.changes.append(
                    f'{path}: changed, and not readable as declarations ({reason})')
        finding.facts = (f'covered types base={len(old.covered)} head={len(new.covered)}; '
                         f'files head={new.files} unreadable={len(new.opaque)}')
        finding.notes = [f'{path} is not readable as declarations ({new.reasons[path]}); '
                         'every edit to it counts as a layout change'
                         for path in sorted(new.opaque)]
        finding.covered = {key: new.places[key] for key in new.covered}
        return finding

    return compare


def read_pins(tree: Tree) -> tuple[list[int], dict[str, str]]:
    """The stamps and the rows (key -> numbers) of one commit's pin file."""
    tree.read([PINS])
    text = without_comments(tree.texts[PINS])
    if PIN_ROW.sub('', text).strip():
        raise NotCompared(f'{PINS} at {tree.commit[:12]} holds text that is not a pin row')
    stamps: list[int] = []
    rows: dict[str, str] = {}
    for found in PIN_ROW.finditer(text):
        if found.group('stamp') is not None:
            stamps.append(int(found.group('stamp')))
            continue
        if found.group('sized') is not None:
            key = f'sizeof({found.group("sized")})'
            value = f'size {int(found.group("bytes"))}'
        else:
            key = f'{found.group("type")}::{found.group("member")}'
            value = f'offset {int(found.group("offset"))} size {int(found.group("size"))}'
        if key in rows:
            raise NotCompared(f'{PINS} at {tree.commit[:12]} pins {key} twice')
        rows[key] = value
    return stamps, rows


def pin_changes(base: Tree, head: Tree) -> Finding:
    """Rule (C): the measured pins of the stream layout, read as text."""
    finding = Finding()
    if PINS not in head.entries:
        raise NotCompared(f'{head.commit[:12]} has no {PINS}: {STREAM_STAMP} has no '
                          'measured pins to hold it')
    stamps, rows = read_pins(head)
    if PINS in base.entries:
        old = read_pins(base)[1]
        finding.facts = f'pin rows base={len(old)} head={len(rows)}'
    elif SELF in base.entries:
        raise NotCompared(f'{base.commit[:12]} holds {SELF} and no {PINS}')
    else:
        old = rows
        finding.facts = f'pin rows base=none head={len(rows)}'
        finding.notes.append(f'{base.commit[:12]} holds neither {SELF} nor {PINS}: this range '
                             'introduces the pins, and no stamp is owed for them')
    for key in sorted(set(old) | set(rows)):
        if old.get(key) != rows.get(key):
            state = ('added' if key not in old else 'removed' if key not in rows
                     else f'{old[key]} -> {rows[key]}')
            finding.changes.append(f'{PINS}: {key}: {state}')
    defined = head.stamps[STREAM_STAMP]
    if stamps != [defined]:
        carried = ('no stamp row' if not stamps
                   else 'stamp ' + ' and '.join(str(stamp) for stamp in stamps))
        finding.failures.append(
            f'{PINS} carries {carried}; {HEADER} defines {STREAM_STAMP} {defined}: '
            'measure the layout for that stamp and pin it')
    if not rows:
        finding.failures.append(f'{PINS} holds no size or member row')
    return finding


class Rule:
    """One stamp and the comparison that says when it is owed.

    Another guarded type is another row of RULES: a name for the diagnostics,
    the stamp of pineforge.h that answers for it, and a comparison of two
    commits (layout_changes builds one for any root classes). The range, the
    stamp reader and the verdict below are shared by every row.
    """

    def __init__(self, name: str, stamp: str, compare) -> None:
        self.name = name
        self.stamp = stamp
        self.compare = compare


RULES = (
    Rule('c-abi', ABI_STAMP, abi_changes),
    Rule('stream-layout', STREAM_STAMP,
         layout_changes(STREAM_ROOTS, KERNEL_ROOTS, ADAPTER_ROOTS)),
    Rule('stream-pins', STREAM_STAMP, pin_changes),
)


def judge(root: Path, found: Range, list_covered: bool) -> int:
    cache: dict[tuple[str, str], object] = {}
    base = Tree(root, found.merge_base, cache)
    head = Tree(root, found.head, cache)
    tip = Tree(root, found.tip, cache) if found.tip != found.merge_base else None
    print(f'{PREFIX}range base={found.tip} head={found.head} merge_base={found.merge_base} '
          f'head_tree={head.tree} from={found.label}')
    findings = [(rule, rule.compare(base, head)) for rule in RULES]
    for rule, finding in findings:
        print(f'{PREFIX}{rule.name}: {rule.stamp} base={base.stamps[rule.stamp]} '
              f'head={head.stamps[rule.stamp]}; {finding.facts}; changes={len(finding.changes)}')
    for rule, finding in findings:
        for note in finding.notes:
            print(f'{PREFIX}note: {rule.name}: {note}')
        if list_covered:
            for key in sorted(finding.covered):
                print(f'{PREFIX}covered: {key} ({finding.covered[key]})')
    failures: list[str] = []
    for rule, finding in findings:
        for change in finding.changes:
            print(f'{PREFIX}{rule.name} change: {change}')
        failures += owed(rule.stamp, rule.name, finding.changes, base.stamps[rule.stamp],
                         head.stamps[rule.stamp], tip.stamps[rule.stamp] if tip else None)
        failures += [f'FAIL {rule.name}: {failure}' for failure in finding.failures]
    for failure in failures:
        print(PREFIX + failure)
    if failures:
        return 1
    print(f'{PREFIX}OK')
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--base', help='judge from this commit (with --head)')
    parser.add_argument('--head', help='judge up to this commit (with --base)')
    parser.add_argument('--list-covered', action='store_true',
                        help='also print every type the stream layout stamp covers at the head')
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        found = find_range(root, args.base, args.head)
        if found.merge_base == found.head:
            if found.bound:
                raise NotCompared(f'the range {found.tip[:12]}..{found.head[:12]} from '
                                  f'{found.label} holds no commit')
            print(MAIN_TIP_NOTICE)
            print(f'{PREFIX}NOT COMPARED: {found.head} is contained in its base '
                  f'({found.label}); no range to judge in this run')
            return 77
        return judge(root, found, args.list_covered)
    except NotCompared as error:
        print(f'{PREFIX}NOT COMPARED: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
