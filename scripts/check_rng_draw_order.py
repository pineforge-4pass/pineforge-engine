#!/usr/bin/env python3
"""Refuse two random draws whose relative order C++17 leaves to the compiler.

Why this exists
---------------
C++ leaves the order in which a call's arguments are evaluated unspecified,
and the two operands of most binary operators unsequenced.  GCC on x86-64
evaluates a call's arguments right to left; AppleClang, and GCC on aarch64,
left to right (``take(draw(1), draw(2), draw(3))`` prints ``321`` with GCC 13.3
on x86-64 and ``123`` with AppleClang 17).  A test that takes two draws of one
generator inside one argument list therefore builds a different random
battery on each compiler.  XPLAT1 (e8bc3237) met exactly that:
test_adapter_quiet_bar matched 92 of its 272 pins on x86-64 and all 272 on
arm64, because three call sites of its random scenario drew two to four values
inside one call's arguments.  The fix was one draw per statement, taken left
to right; the pins were harvested with that order.  Nothing kept the class
out, and ten call sites came back.  This checker is that guard.

What counts as a draw
---------------------
A *generator* is a ``std::`` random engine (``mt19937``, ``mt19937_64``,
``minstd_rand`` ...), or a class the scanned code defines whose member
functions advance a data member with a PRNG step (``s ^= s << 13``,
``s ^= s >> 12``, ``state += 0x9e3779b97f4a7c15``, ``s = s * K + C``) -- the
tests' ``Rng``, ``Random`` and ``SplitMix``.  A *draw* is a call expression
that advances one:

- a call on a generator object: ``rng()``, ``(*rng)()``, ``rng.next()``,
  ``r.unit()``, ``s.below(3)``, ``rng_->percent(50)``;
- a call of a *drawer*: a function, member function or named lambda whose
  body draws, found transitively over the translation unit (``long_id()``,
  ``units()``, ``opening(...)``, ``pick(rng, ...)``, ``walk(r, ...)``), or
  whose body applies a PRNG step to state it does not declare (a lambda over a
  captured ``state``, a function over a global ``mix_state``);
- a call that receives a generator as an argument (``dist(rng)``,
  ``random_text(rng, n)``, ``std::shuffle(b, e, rng)``): the callee may draw.

The translation unit is the file plus the quoted headers it includes,
transitively; a header is checked under every unit that includes it.

What is reported
----------------
``ARG``      a call (function, member function, constructor, macro) with two
             or more arguments that each contain a draw.  A generator passed
             bare (``walk(r, 40 + r.below(20))``) is not a draw of that
             argument: the callee draws only after every argument is
             evaluated, so the order is fixed.
``OPERAND``  two operands of one unsequenced binary operator
             (``+ - * / % & | ^ < > <= >= == !=``) that each contain a draw,
             e.g. ``(r.percent(50) ? 1 : -1) * r.between(4, 16)``.
``BYVALUE``  a function or lambda parameter of generator type taken by value:
             its copy would be made while the other arguments are evaluated.

What is sequenced, and never reported
-------------------------------------
- one draw per statement, and one per declarator
  (``const int a = r.below(3), b = r.below(4);``);
- the elements of a braced-init-list, ``{a(), b()}`` and ``T{a(), b()}``,
  which are evaluated left to right even when a constructor is called
  ([dcl.init.list]/4);
- ``&&``, ``||``, ``?:`` and the built-in comma (left before right);
- C++17's P0145 orders: ``<<`` and ``>>`` (left before right), ``a[b]``, the
  postfix of ``a.f(b)`` before ``b``, and ``=`` / compound assignment (right
  before left);
- a nested call's arguments before its body
  (``rng.quarter(rng.percent(3) ? -4 : 0, 12)``);
- draws inside a lambda body: they run when the lambda is called.

Limits (by design)
------------------
Draws hidden behind a macro, a virtual callback or a function defined in
another translation unit are not seen, and neither are two draws of two
different generators (their order does not change either battery).  Preprocessor
lines are blanked before scanning.

Usage
-----
    check_rng_draw_order.py                 # scan the default directories
    check_rng_draw_order.py PATH...         # files or directories
    check_rng_draw_order.py --root DIR      # another checkout
    check_rng_draw_order.py --vocabulary    # print what each unit counts as a draw

Exit 0 when nothing is found, 1 on findings, 2 on a usage error.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIRS = ('tests', 'examples', 'benchmarks', 'tools', 'src', 'include', 'runner',
                'tutorial')
# Frozen previous-epoch header snapshots: compiled only as ABI witnesses, never
# as tests, and never holding a generator.
EXCLUDED_PREFIXES = ('tests/fixtures/',)
SUFFIXES = frozenset(('.cpp', '.cc', '.cxx', '.hpp', '.h', '.hh', '.hxx', '.inc', '.ipp'))

STD_ENGINES = frozenset((
    'mt19937', 'mt19937_64', 'minstd_rand', 'minstd_rand0', 'ranlux24', 'ranlux48',
    'ranlux24_base', 'ranlux48_base', 'knuth_b', 'default_random_engine', 'random_device',
    'linear_congruential_engine', 'mersenne_twister_engine', 'subtract_with_carry_engine',
    'discard_block_engine', 'independent_bits_engine', 'shuffle_order_engine'))
# A unit that spells none of these cannot draw; it is skipped unread.
HINT = re.compile(
    r'\b(?:' + '|'.join(sorted(STD_ENGINES)) + r')\b'
    r'|(\w+)\s*\^=\s*\(?\s*\1\s*(?:<<|>>)'
    r'|\+=\s*0x9[eE]3779[bB]97[fF]4[aA]7[cC]15'
    r'|(\w+)\s*=\s*\2\s*\*\s*\d{6,}')

KEYWORDS = frozenset((
    'alignas', 'alignof', 'and', 'asm', 'auto', 'bool', 'break', 'case', 'catch', 'char',
    'class', 'const', 'const_cast', 'constexpr', 'continue', 'decltype', 'default', 'delete',
    'do', 'double', 'dynamic_cast', 'else', 'enum', 'explicit', 'extern', 'false', 'float',
    'for', 'friend', 'goto', 'if', 'inline', 'int', 'long', 'mutable', 'namespace', 'new',
    'noexcept', 'not', 'nullptr', 'operator', 'or', 'private', 'protected', 'public',
    'reinterpret_cast', 'return', 'short', 'signed', 'sizeof', 'static', 'static_assert',
    'static_cast', 'struct', 'switch', 'template', 'this', 'throw', 'true', 'try',
    'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using', 'virtual', 'void',
    'volatile', 'while', 'override', 'final', 'co_await', 'co_return', 'co_yield'))
VALUE_WORDS = frozenset(('this', 'true', 'false', 'nullptr'))
# A '(' after one of these is a condition or an operand, never a call.
CONTROL_WORDS = frozenset((
    'if', 'while', 'switch', 'for', 'catch', 'sizeof', 'alignof', 'alignas', 'decltype',
    'noexcept', 'static_assert', 'typeid', 'return', 'throw', 'case', 'co_return',
    'co_yield', 'co_await', 'defined', 'and', 'or', 'not', 'else', 'do', 'new', 'delete'))
# Keywords that end one piece of an expression region and start the next.
SEPARATOR_WORDS = frozenset((
    'return', 'if', 'else', 'while', 'for', 'do', 'switch', 'case', 'default', 'throw',
    'goto', 'break', 'continue', 'co_return', 'co_yield', 'co_await', 'and', 'or', 'try',
    'catch'))
# Sequenced (or statement-level) punctuators: a piece of a region ends here.
SEPARATOR_OPS = frozenset((
    ';', ',', '?', ':', '&&', '||', '=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
    '<<=', '>>=', '<<', '>>', '->*', '.*'))
UNSEQUENCED_BINARY = frozenset((
    '+', '-', '*', '/', '%', '&', '|', '^', '<', '>', '<=', '>=', '==', '!=', '<=>'))
DECL_TRAILERS = frozenset(('const', 'volatile', 'override', 'final', 'mutable', 'constexpr',
                           '&', '&&', 'noexcept', 'throw', 'try'))
HEAD_WORDS = frozenset(('struct', 'class', 'union', 'enum', 'namespace', 'extern', 'concept'))

TOKEN_RE = re.compile(r'''
    (?P<str>(?:u8|u|U|L)?R?"[^"]*"|(?:u8|u|U|L)?'[^']*')
  | (?P<id>[A-Za-z_]\w*)
  | (?P<num>\.?\d(?:[\w.']|[eEpP][+-])*)
  | (?P<op>\.\.\.|->\*|<=>|<<=|>>=|::|->|\+\+|--|<<|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=
          |&=|\|=|\^=|\.\*|>>|[{}\[\]()<>;:,.?~!%^&*+\-=/|])
''', re.X)
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"\n]+)"', re.M)
BIG_LITERAL = re.compile(r'^(?:0[xX][0-9a-fA-F\']{8,}|\d[\d\']{5,})[uUlL]*$')


# ---------------------------------------------------------------------------
# Lexing
# ---------------------------------------------------------------------------

def strip_source(text: str) -> str:
    """Blank comments, the contents of string and character literals, and
    preprocessor directives, keeping every offset and newline."""
    out = list(text)
    n = len(text)

    def blank(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            if out[k] != '\n':
                out[k] = ' '

    i = 0
    line_start = True
    while i < n:
        c = text[i]
        if c == '\n':
            line_start = True
            i += 1
            continue
        if c in ' \t\r\f\v':
            i += 1
            continue
        if line_start and c == '#':
            j = i
            while j < n:
                if text.startswith('/*', j):
                    end = text.find('*/', j + 2)
                    j = n if end < 0 else end + 2
                    continue
                if text[j] == '\n':
                    back = j - 1
                    while back > i and text[back] in ' \t\r':
                        back -= 1
                    if text[back] == '\\':
                        j += 1
                        continue
                    break
                j += 1
            blank(i, j)
            i = j
            continue
        line_start = False
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == '"':
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] == '_'):
                k -= 1
            prefix = text[k + 1:i]
            if prefix.endswith('R') and prefix in ('R', 'u8R', 'uR', 'UR', 'LR'):
                m = re.match(r'"([^()\\\s]{0,16})\(', text[i:])
                if m:
                    closing = ')' + m.group(1) + '"'
                    end = text.find(closing, i + m.end())
                    end = n if end < 0 else end + len(closing)
                    blank(i + 1, end - 1)
                    i = end
                    continue
            j = i + 1
            while j < n and text[j] != '"' and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            blank(i + 1, j)
            i = j + 1
        elif c == "'":
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] in "_'."):
                k -= 1
            run = text[k + 1:i]
            if run and run[0].isdigit():      # a digit separator: 1'000'000
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            blank(i + 1, j)
            i = j + 1
        else:
            i += 1
    return ''.join(out)


@dataclass
class Tok:
    kind: str       # 'id', 'num', 'str', 'op'
    text: str
    line: int
    joined: bool = False    # the second '>' of a split '>>'


def tokenize(clean: str) -> list[Tok]:
    toks: list[Tok] = []
    line = 1
    last = 0
    for m in TOKEN_RE.finditer(clean):
        line += clean.count('\n', last, m.start())
        last = m.start()
        kind = m.lastgroup
        text = m.group(0)
        if kind == 'op' and text == '>>':
            toks.append(Tok('op', '>', line))
            toks.append(Tok('op', '>', line, joined=True))
            continue
        toks.append(Tok(kind, text, line))
    return toks


# ---------------------------------------------------------------------------
# Structure: brackets, template arguments, group kinds
# ---------------------------------------------------------------------------

CALL, GROUP, CONTROL, PARAMS, BLOCK, INIT, SUBSCRIPT, CAPTURE, NAME = (
    'call', 'group', 'control', 'params', 'block', 'init', 'subscript', 'capture', 'name')


class Structure:
    """Bracket pairs and what each group is, for one file's tokens."""

    def __init__(self, toks: list[Tok]):
        self.toks = toks
        self.close: dict[int, int] = {}
        self.open_of: dict[int, int] = {}
        self.template_open: dict[int, int] = {}
        self.template_close: set[int] = set()
        self.kind: dict[int, str] = {}
        self.parent: dict[int, int] = {}
        self._templates()
        self._brackets()
        for i in sorted(self.close):
            self.kind[i] = self._classify(i)

    # -- template argument lists ------------------------------------------
    def _templates(self) -> None:
        toks = self.toks
        for i, t in enumerate(toks):
            if t.text != '<' or i == 0 or i in self.template_close:
                continue
            p = toks[i - 1]
            if p.kind != 'id' or (p.text in KEYWORDS and p.text not in ('template',)):
                if not (p.text == '>' and (i - 1) in self.template_close):
                    continue
            j = self._template_end(i)
            if j is not None:
                self.template_open[i] = j
                self.template_close.add(j)

    def _template_end(self, i: int) -> int | None:
        toks = self.toks
        depth = 1
        j = i + 1
        while j < len(toks) and j - i < 80:
            t = toks[j]
            x = t.text
            if x == '<':
                if toks[j - 1].kind != 'id':
                    return None
                depth += 1
            elif x == '>':
                depth -= 1
                if depth == 0:
                    return j
            elif t.kind in ('id', 'num'):
                if t.kind == 'id' and x in ('return', 'if', 'while', 'for', 'case', 'new',
                                            'delete', 'throw', 'else'):
                    return None
            elif x in ('::', ',', '*', '&', '...'):
                pass
            elif x == '(':
                # A function type, std::function<void(int)>: its own parens,
                # holding type words only.
                k = j + 1
                while k < len(toks) and toks[k].text != ')':
                    if toks[k].kind not in ('id',) and toks[k].text not in (
                            '::', ',', '*', '&', '<', '>', '...'):
                        return None
                    k += 1
                j = k
            else:
                return None
            j += 1
        return None

    # -- (), [], {} pairs ------------------------------------------------
    def _brackets(self) -> None:
        stack: list[int] = []
        pairs = {')': '(', ']': '[', '}': '{'}
        for i, t in enumerate(self.toks):
            x = t.text
            if t.kind != 'op':
                continue
            if x in '([{' and len(x) == 1:
                if stack:
                    self.parent[i] = stack[-1]
                stack.append(i)
            elif x in pairs and len(x) == 1:
                while stack and self.toks[stack[-1]].text != pairs[x]:
                    stack.pop()         # tolerate an unbalanced macro spelling
                if stack:
                    o = stack.pop()
                    self.close[o] = i
                    self.open_of[i] = o

    # -- helpers -----------------------------------------------------------
    def prev(self, i: int) -> Tok | None:
        return self.toks[i - 1] if i > 0 else None

    def operand_end(self, i: int) -> bool:
        """Whether toks[i] can end an operand (so a following '-' is binary)."""
        if i < 0:
            return False
        t = self.toks[i]
        if t.kind in ('num', 'str'):
            return True
        if t.kind == 'id':
            return t.text not in KEYWORDS or t.text in VALUE_WORDS
        if t.text in (')', ']', '}'):
            return True
        if t.text == '>' and i in self.template_close:
            return True
        if t.text in ('++', '--'):
            return self.operand_end(i - 1)
        return False

    def is_lambda_capture(self, i: int) -> bool:
        if i + 1 < len(self.toks) and self.toks[i + 1].text == '[':
            return False            # [[attribute]]
        return not self.operand_end(i - 1)

    def after_params(self, close_idx: int) -> int:
        """Index of the first token after a parameter list's trailers."""
        toks = self.toks
        j = close_idx + 1
        while j < len(toks):
            x = toks[j].text
            if x in DECL_TRAILERS:
                if x in ('noexcept', 'throw') and j + 1 < len(toks) and toks[j + 1].text == '(':
                    j = self.close.get(j + 1, j + 1) + 1
                    continue
                j += 1
                continue
            if x == '->':
                j += 1
                while j < len(toks) and toks[j].text not in ('{', ';', '=') and not (
                        toks[j].text == ',' or toks[j].text == ')'):
                    if toks[j].text == '<' and j in self.template_open:
                        j = self.template_open[j]
                    j += 1
                continue
            break
        return j

    def _classify(self, i: int) -> str:
        toks = self.toks
        x = toks[i].text
        p = self.prev(i)
        if x == '(':
            if p is None:
                return GROUP
            if p.kind == 'id' and p.text in CONTROL_WORDS:
                return CONTROL
            if p.kind == 'id' and p.text == 'operator':
                return NAME
            callee_like = (
                (p.kind == 'id' and (p.text not in KEYWORDS or p.text in (
                    'this', 'static_cast', 'const_cast', 'reinterpret_cast', 'dynamic_cast')))
                or p.text in (')', ']')
                or (p.text == '>' and (i - 1) in self.template_close)
                or (p.text == '}' and self._brace_is_expression(i - 1)))
            if not callee_like:
                return GROUP
            if p.text == ']':
                o = self.open_of.get(i - 1)
                if o is not None and self.is_lambda_capture(o):
                    return PARAMS
            j = self.after_params(self.close[i])
            nxt = toks[j].text if j < len(toks) else ''
            if nxt == '{':
                return PARAMS
            if nxt == ':' and j + 1 < len(toks) and toks[j + 1].kind == 'id' \
                    and toks[j + 1].text not in KEYWORDS:
                return PARAMS           # a constructor's mem-initializer list follows
            return CALL
        if x == '[':
            if self.is_lambda_capture(i):
                if i + 1 < len(toks) and toks[i + 1].text == '[':
                    return NAME         # attribute
                return CAPTURE
            return SUBSCRIPT
        # '{'
        return INIT if self._brace_is_init(i) else BLOCK

    def _brace_is_expression(self, close_idx: int) -> bool:
        o = self.open_of.get(close_idx)
        if o is None:
            return False
        if self._brace_is_init(o):
            return True
        # a lambda body: [..](..) {..} or [..] {..}
        p = o - 1
        while p >= 0 and self.toks[p].text in DECL_TRAILERS:
            p -= 1
        if p >= 0 and self.toks[p].text == ')':
            q = self.open_of.get(p)
            if q is not None and q > 0 and self.toks[q - 1].text == ']':
                return True
        if p >= 0 and self.toks[p].text == ']':
            q = self.open_of.get(p)
            return q is not None and self.is_lambda_capture(q)
        return False

    def _brace_is_init(self, i: int) -> bool:
        toks = self.toks
        p = self.prev(i)
        if p is None:
            return False
        x = p.text
        if x in ('=', 'return', ',', '(', '[', '?', '{') and x != '{':
            return True
        if x == '{':
            # {{...}} nested init inside an init; a block inside a block
            par = self.parent.get(i)
            return par is not None and self._brace_is_init(par)
        if x in (')', ';', '}', 'else', 'do', 'try', 'mutable', 'noexcept', 'const',
                 'override', 'final', 'volatile', '&', '&&'):
            return False
        if x == ']':
            o = self.open_of.get(i - 1)
            return not (o is not None and self.is_lambda_capture(o))
        if x == ':':
            par = self.parent.get(i)
            return par is not None and toks[par].text == '('     # range-for over a list
        if p.kind == 'id' or (x == '>' and (i - 1) in self.template_close):
            if p.kind == 'id' and x in KEYWORDS and x not in HEAD_WORDS:
                return x not in ('else', 'do', 'try')
            # scan the statement head for struct/class/namespace/enum or a
            # trailing return type
            k = i - 1
            saw_arrow = False
            while k >= 0:
                t = toks[k]
                if t.text in (';', '{', '}') and k not in self.close and k not in self.open_of:
                    break
                if t.text in (')', ']', '}') and k in self.open_of:
                    k = self.open_of[k] - 1
                    continue
                if t.text in ('(', '[', '{') and k in self.close:
                    break           # the head is inside an enclosing group
                if t.text == '>' and k in self.template_close:
                    # skip back over the template argument list
                    for o, c in self.template_open.items():
                        if c == k:
                            k = o - 1
                            break
                    else:
                        k -= 1
                    continue
                if t.kind == 'id' and t.text in HEAD_WORDS:
                    return False
                if t.text == '->':
                    saw_arrow = True
                if t.text == ':' and k > 0 and toks[k - 1].kind == 'id' and \
                        toks[k - 1].text in ('public', 'private', 'protected'):
                    break
                k -= 1
            return not saw_arrow
        return True


# ---------------------------------------------------------------------------
# Per-file facts
# ---------------------------------------------------------------------------

@dataclass
class Function:
    name: str
    body: tuple[int, int]           # token range inside the braces
    locals: set[str]                # parameters by value and local variables
    owner: str | None               # enclosing class, if any
    steps: set[str] = field(default_factory=set)    # non-locals a PRNG step advances
    mixes: set[str] = field(default_factory=set)    # non-locals updated from an input
    returns_value: bool = False
    qualifier: str | None = None    # Class of an out-of-line Class::name definition

    @property
    def stepper(self) -> bool:
        """Advances state it does not own, from that state alone, and returns
        a value: a generator's next(), a lambda over a captured seed.  A hash
        fold (h = (h ^ v) * K) mixes an input and is not one."""
        return bool(self.steps - self.mixes) and self.returns_value


@dataclass
class ClassDef:
    name: str
    body: tuple[int, int]
    has_base: bool = False
    bases: set[str] = field(default_factory=set)
    methods: set[str] = field(default_factory=set)


@dataclass
class FileFacts:
    path: Path
    rel: str
    toks: list[Tok]
    st: Structure
    includes: list[Path]
    functions: list[Function]
    classes: list[ClassDef]
    declared: list[tuple[str, str, int]]    # (type name, variable name, token index)
    hinted: bool


UPDATE_OPS = frozenset(('^=', '+=', '-=', '*=', '|=', '&=', '='))
GEN_NAME_RE = re.compile(r'(?i)rng|random|splitmix|xorshift|xoshiro|xoroshiro|pcg|lcg|prng')


def body_updates(toks: list[Tok], st: Structure, lo: int, hi: int) -> tuple[set[str], set[str]]:
    """(stepped, mixed): identifiers a PRNG step advances from their own value
    alone (s ^= s << 13; s ^= s >> 7; state += 0x9e37...; s = s * K + C), and
    identifiers updated from anything else (h ^= byte; h = (h ^ v) * K)."""
    stepped: set[str] = set()
    mixed: set[str] = set()
    for k in range(lo, hi - 2):
        t = toks[k]
        if t.kind != 'id' or t.text in KEYWORDS:
            continue
        if k > lo and toks[k - 1].text in ('.', '::', '->') and not (
                toks[k - 1].text == '->' and toks[k - 2].text == 'this'):
            continue
        op = toks[k + 1].text
        if op not in UPDATE_OPS:
            continue
        if op == '=' and k > lo and (toks[k - 1].kind == 'id' and toks[k - 1].text not in (
                'return',) or toks[k - 1].text in ('*', '&', '>')):
            continue        # a declaration's initializer, not an update
        rhs: list[Tok] = []
        j = k + 2
        depth = 0
        while j < hi:
            x = toks[j].text
            if x == '<' and j in st.template_open:
                j = st.template_open[j] + 1
                continue
            if x in ('(', '[', '{'):
                depth += 1
            elif x in (')', ']', '}'):
                if depth == 0:
                    break
                depth -= 1
            elif x in (';', ',') and depth == 0:
                break
            rhs.append(toks[j])
            j += 1
        name = t.text
        ids = {r.text for n, r in enumerate(rhs)
               if r.kind == 'id' and r.text not in KEYWORDS
               and not (n + 1 < len(rhs) and rhs[n + 1].text == '::')}
        texts = [r.text for r in rhs]
        shift = '<<' in texts or any(
            texts[m] == '>' and m + 1 < len(texts) and rhs[m + 1].joined
            for m in range(len(texts)))
        big = any(r.kind == 'num' and BIG_LITERAL.match(r.text) for r in rhs)
        self_only = ids <= {name}
        prng_like = ((op == '^=' and name in ids and shift)
                     or (op == '+=' and big and len(rhs) == 1)
                     or (op == '*=' and big and len(rhs) == 1)
                     or (op == '=' and name in ids and big and '*' in texts))
        if self_only and prng_like:
            stepped.add(name)
        elif ids - {name} or (op == '=' and name not in ids):
            mixed.add(name)
    return stepped, mixed


def body_locals(toks: list[Tok], st: Structure, params: tuple[int, int] | None,
                lo: int, hi: int) -> set[str]:
    """Names a function owns: its by-value parameters and its local variables.
    A reference parameter is the caller's state (splitmix(std::uint64_t& s))."""
    out: set[str] = set()

    def scan(a: int, b: int, is_params: bool) -> None:
        for k in range(a, b):
            t = toks[k]
            if t.kind != 'id' or t.text in KEYWORDS:
                continue
            if k + 1 >= len(toks):
                continue
            nxt = toks[k + 1].text
            p = toks[k - 1] if k > 0 else None
            if p is None:
                continue
            typeish = (p.kind == 'id' and (p.text not in KEYWORDS or p.text in (
                'auto', 'int', 'long', 'double', 'float', 'bool', 'char', 'unsigned',
                'signed', 'short', 'const'))) or p.text in ('*', '&', '&&') or (
                p.text == '>' and (k - 1) in st.template_close)
            if not typeish:
                continue
            if is_params and p.text in ('&', '&&'):
                continue
            if nxt in ('=', ';', ',', '(', '{', '[', ')', ':') or (is_params and nxt == ')'):
                out.add(t.text)

    if params:
        scan(params[0], params[1], True)
    scan(lo, hi, False)
    return out


def base_names(toks: list[Tok], st: Structure, lo: int, hi: int) -> set[str]:
    """The base classes a class head toks[lo:hi] names (last component of each)."""
    out: set[str] = set()
    k = lo
    while k < hi and toks[k].text != ':':
        k += 1
    last = None
    k += 1
    while k <= hi:
        t = toks[k] if k < hi else Tok('op', ',', 0)
        if t.text == '<' and k in st.template_open:
            k = st.template_open[k] + 1
            continue
        if t.text == ',':
            if last:
                out.add(last)
            last = None
        elif t.kind == 'id' and t.text not in KEYWORDS:
            last = t.text
        k += 1
    return out


def finish_function(fn: Function, toks: list[Tok], st: Structure) -> None:
    lo, hi = fn.body
    stepped, mixed = body_updates(toks, st, lo, hi)
    fn.steps = stepped - fn.locals
    fn.mixes = mixed - fn.locals
    fn.returns_value = any(toks[k].text == 'return' and toks[k + 1].text != ';'
                           for k in range(lo, hi - 1))


def collect_facts(path: Path, rel: str, raw: str) -> FileFacts:
    clean = strip_source(raw)
    toks = tokenize(clean)
    st = Structure(toks)
    includes = [path.parent / m.group(1) for m in INCLUDE_RE.finditer(raw)]
    functions: list[Function] = []
    classes: list[ClassDef] = []
    declared: list[tuple[str, str, int]] = []

    # class / struct bodies
    for o, c in st.close.items():
        if toks[o].text != '{' or st.kind.get(o) != BLOCK:
            continue
        k = o - 1
        name = None
        while k >= 0 and toks[k].text not in (';', '{', '}'):
            if toks[k].kind == 'id' and toks[k].text in ('struct', 'class', 'union'):
                if k + 1 < o and toks[k + 1].kind == 'id':
                    name = toks[k + 1].text
                break
            if toks[k].text in (')', ']') and k in st.open_of:
                k = st.open_of[k]
            k -= 1
        if name:
            texts = [t.text for t in toks[k:o]]
            classes.append(ClassDef(name, (o + 1, c), has_base=':' in texts,
                                    bases=base_names(toks, st, k, o)))

    def owner_of(idx: int) -> str | None:
        best = None
        for cd in classes:
            if cd.body[0] <= idx < cd.body[1]:
                if best is None or cd.body[0] > best.body[0]:
                    best = cd
        return best.name if best else None

    # function definitions and named lambdas
    for o, kind in st.kind.items():
        if kind != PARAMS:
            continue
        c = st.close[o]
        j = st.after_params(c)
        if j < len(toks) and toks[j].text == ':':
            # constructor: skip the mem-initializer list to the body
            k = j + 1
            while k < len(toks):
                if toks[k].text == '{' and st.kind.get(k) == BLOCK:
                    break
                if toks[k].text in ('(', '{') and k in st.close:
                    k = st.close[k] + 1
                    continue
                k += 1
            j = k
        if j >= len(toks) or toks[j].text != '{' or j not in st.close:
            continue
        body = (j + 1, st.close[j])
        name = None
        p = o - 1
        if toks[p].text == ']':
            # lambda: auto name = [caps](params) {...}
            q = st.open_of.get(p)
            if q is not None and q >= 2 and toks[q - 1].text == '=' and toks[q - 2].kind == 'id':
                name = toks[q - 2].text
        elif toks[p].kind == 'id':
            name = toks[p].text
        elif toks[p].text == ')' and p in st.open_of and toks[st.open_of[p] - 1].text == 'operator':
            name = 'operator()'
        if not name:
            name = '<lambda>'
        fn = Function(name, body, body_locals(toks, st, (o + 1, c), body[0], body[1]),
                      owner_of(o))
        if toks[p].kind == 'id' and p >= 2 and toks[p - 1].text == '::' \
                and toks[p - 2].kind == 'id':
            fn.qualifier = toks[p - 2].text
        finish_function(fn, toks, st)
        functions.append(fn)
    # lambdas without a parameter list: auto name = [caps] {...}
    for o, kind in st.kind.items():
        if kind != CAPTURE:
            continue
        c = st.close[o]
        j = st.after_params(c)
        if j < len(toks) and toks[j].text == '{' and j in st.close:
            name = None
            if o >= 2 and toks[o - 1].text == '=' and toks[o - 2].kind == 'id':
                name = toks[o - 2].text
            body = (j + 1, st.close[j])
            fn = Function(name or '<lambda>', body,
                          body_locals(toks, st, None, body[0], body[1]), owner_of(o))
            finish_function(fn, toks, st)
            functions.append(fn)
    for fn in functions:
        for cd in classes:
            if fn.owner == cd.name:
                cd.methods.add(fn.name)

    # declarations: TYPE [&*] NAME followed by ( { = ; , ) [
    for k in range(len(toks) - 1):
        t = toks[k]
        if t.kind != 'id' or t.text in KEYWORDS:
            continue
        if k > 0 and toks[k - 1].text in ('struct', 'class', 'union', 'enum'):
            continue
        j = k + 1
        if j < len(toks) and toks[j].text == '<' and j in st.template_open:
            j = st.template_open[j] + 1     # Type<Args> name
        while j < len(toks) and toks[j].text in ('&', '*', '&&', 'const'):
            j += 1
        if j + 1 >= len(toks) or toks[j].kind != 'id' or toks[j].text in KEYWORDS:
            continue
        if toks[j + 1].text in ('(', '{', '=', ';', ',', ')', '['):
            declared.append((t.text, toks[j].text, j))
    # auto NAME = TYPE(...) / TYPE{...}
    for k in range(len(toks) - 4):
        if toks[k].text == 'auto' and toks[k + 1].text in ('&', '&&'):
            continue
        if toks[k].text == 'auto' and toks[k + 1].kind == 'id' and toks[k + 2].text == '=':
            m = k + 3
            while m + 1 < len(toks) and toks[m + 1].text == '::':
                m += 2
            if m + 1 < len(toks) and toks[m].kind == 'id' and toks[m + 1].text in ('(', '{'):
                declared.append((toks[m].text, toks[k + 1].text, k + 1))
    return FileFacts(path, rel, toks, st, includes, functions, classes, declared,
                     bool(HINT.search(raw)))


# ---------------------------------------------------------------------------
# Vocabulary of one translation unit
# ---------------------------------------------------------------------------

@dataclass
class Vocabulary:
    gen_types: set[str] = field(default_factory=set)
    gen_objects: set[str] = field(default_factory=set)
    gen_methods: dict[str, set[str]] = field(default_factory=dict)   # class -> methods
    # (owner class or None, name): free functions and named lambdas have no owner
    drawers: set[tuple[str | None, str]] = field(default_factory=set)
    classes: set[str] = field(default_factory=set)
    bases: dict[str, set[str]] = field(default_factory=dict)
    obj_types: dict[str, set[str]] = field(default_factory=dict)

    def lineage(self, cls: str | None) -> set[str | None]:
        if cls is None:
            return set()
        out: set[str | None] = set()
        pending = [cls]
        while pending:
            c = pending.pop()
            if c in out:
                continue
            out.add(c)
            pending.extend(self.bases.get(c, ()))
        return out

    def drawer(self, name: str, owners: set[str | None] | None) -> bool:
        """Whether `name` draws when owned by one of `owners` (None: any class)."""
        if owners is None:
            return any(n == name and o is not None for o, n in self.drawers)
        return any((o, name) in self.drawers for o in owners)

    def names(self) -> list[str]:
        return sorted({(o + '::' if o else '') + n for o, n in self.drawers})


def resolved_owner(fn: Function, classes: set[str]) -> str | None:
    if fn.qualifier and fn.qualifier in classes:
        return fn.qualifier
    return fn.owner


def build_vocabulary(unit: list[FileFacts]) -> Vocabulary:
    v = Vocabulary()
    v.gen_types |= STD_ENGINES
    # A generator class: one of its methods is a stepper, and it is named as
    # a generator or stands alone (no base class).  A host class with a
    # private draw() is not a generator; its draw() is a drawer below.
    for ff in unit:
        for cd in ff.classes:
            steppers = [fn for fn in ff.functions if fn.owner == cd.name and fn.stepper]
            if steppers and (GEN_NAME_RE.search(cd.name) or not cd.has_base):
                v.gen_types.add(cd.name)
                v.gen_methods.setdefault(cd.name, set()).update(cd.methods)
    for ff in unit:
        for cd in ff.classes:
            v.classes.add(cd.name)
            v.bases.setdefault(cd.name, set()).update(cd.bases)
    for ff in unit:
        for type_name, var, _ in ff.declared:
            if type_name in v.gen_types:
                v.gen_objects.add(var)
            elif type_name in v.classes:
                v.obj_types.setdefault(var, set()).add(type_name)
    # drawers: free functions, lambdas and non-generator methods whose body
    # draws, or which are steppers themselves; to a fixpoint.
    candidates = [(ff, fn, resolved_owner(fn, v.classes)) for ff in unit for fn in ff.functions
                  if fn.name != '<lambda>']
    candidates = [(ff, fn, owner) for ff, fn, owner in candidates if owner not in v.gen_methods]
    for ff, fn, owner in candidates:
        if fn.stepper:
            v.drawers.add((owner, fn.name))
    checkers = {id(ff): Checker(ff, v) for ff in unit}
    changed = True
    while changed:
        changed = False
        for ff, fn, owner in candidates:
            if (owner, fn.name) in v.drawers:
                continue
            checker = checkers[id(ff)]
            checker._draw_cache.clear()
            lo, hi = fn.body
            if checker.region_draws(lo, hi):
                v.drawers.add((owner, fn.name))
                changed = True
    return v


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------

@dataclass(frozen=True, order=True)
class Finding:
    rel: str
    line: int
    kind: str
    detail: str


class Checker:
    def __init__(self, ff: FileFacts, v: Vocabulary):
        self.ff = ff
        self.toks = ff.toks
        self.st = ff.st
        self.v = v
        self.findings: set[Finding] = set()
        self._draw_cache: dict[int, bool] = {}
        self.gen_class_ranges = [(cd.body, cd.name) for cd in ff.classes
                                 if cd.name in v.gen_methods]
        self.scopes = [(cd.body[0], cd.body[1], cd.name) for cd in ff.classes]
        self.scopes += [(fn.body[0], fn.body[1], resolved_owner(fn, v.classes))
                        for fn in ff.functions if fn.name != '<lambda>']

    # -- what is a draw ------------------------------------------------------
    def split_args(self, o: int) -> list[tuple[int, int]]:
        c = self.st.close[o]
        args = []
        start = o + 1
        k = o + 1
        while k < c:
            x = self.toks[k].text
            if x in ('(', '[', '{') and k in self.st.close:
                k = self.st.close[k] + 1
                continue
            if x == '<' and k in self.st.template_open:
                k = self.st.template_open[k] + 1
                continue
            if x == ',':
                args.append((start, k))
                start = k + 1
            k += 1
        if start < c or args:
            args.append((start, c))
        return args

    def bare_generator(self, lo: int, hi: int) -> bool:
        texts = [t.text for t in self.toks[lo:hi]]
        if texts and texts[0] in ('*', '&'):
            texts = texts[1:]
        if len(texts) >= 2 and texts[0] == 'this' and texts[1] == '->':
            texts = texts[2:]
        if len(texts) >= 4 and texts[:3] == ['std', '::', 'ref'] and texts[3] == '(':
            texts = [x for x in texts[4:] if x != ')']
        return len(texts) == 1 and texts[0] in self.v.gen_objects

    def in_gen_class(self, idx: int) -> str | None:
        for (lo, hi), name in self.gen_class_ranges:
            if lo <= idx < hi:
                return name
        return None

    def is_draw_call(self, o: int) -> bool:
        if o in self._draw_cache:
            return self._draw_cache[o]
        result = self._is_draw_call(o)
        self._draw_cache[o] = result
        return result

    def _is_draw_call(self, o: int) -> bool:
        toks = self.toks
        v = self.v
        args = self.split_args(o)
        if any(self.bare_generator(a, b) for a, b in args):
            # the callee may draw from it -- unless this is a declaration
            # copying it (Rng copy(rng)), which reads but does not advance
            if not (o >= 2 and toks[o - 1].kind == 'id' and toks[o - 2].kind == 'id'
                    and toks[o - 2].text not in KEYWORDS):
                return True
        p = o - 1
        if p < 0:
            return False
        pt = toks[p]
        if pt.text == ')' and p in self.st.open_of:
            inner = [t.text for t in toks[self.st.open_of[p] + 1:p]]
            if inner and inner[0] == '*':
                inner = inner[1:]
            return len(inner) == 1 and inner[0] in v.gen_objects
        if pt.kind != 'id':
            return False
        name = pt.text
        q = toks[p - 1] if p >= 1 else None
        here = self.owner_at(o)
        if q is not None and q.text in ('.', '->'):
            obj = toks[p - 2] if p >= 2 else None
            if obj is None or obj.kind != 'id':
                return v.drawer(name, None)         # a call result's member: any class
            if obj.text == 'this':
                return v.drawer(name, v.lineage(here)) or self._gen_method(name, o)
            chained = p >= 3 and toks[p - 3].text in ('.', '->') and not (
                p >= 4 and toks[p - 4].text == 'this')
            if obj.text in v.gen_objects and not chained:
                return True
            types = None if chained else v.obj_types.get(obj.text)
            if types:
                owners: set[str | None] = set()
                for t in types:
                    owners |= v.lineage(t)
                return v.drawer(name, owners)
            return v.drawer(name, None)
        if q is not None and q.text == '::':
            qual = toks[p - 2] if p >= 2 else None
            if qual is None or qual.text == 'std':
                return False
            if qual.text in v.classes:
                return v.drawer(name, v.lineage(qual.text))
            return v.drawer(name, {None})           # a namespace's free function
        if q is not None and ((q.kind == 'id' and q.text not in KEYWORDS)
                              or (q.text in ('&', '*', '&&', '>') and not self.st.operand_end(p - 2)
                                  and q.text != '>')
                              or (q.text == '>' and (p - 1) in self.st.template_close)):
            return False        # a declaration: Rng rng(seed), Type name(args)
        if name in v.gen_objects:
            return True         # rng()
        owners = v.lineage(here) | {None}
        return v.drawer(name, owners) or self._gen_method(name, o)

    def owner_at(self, idx: int) -> str | None:
        """The class whose member function (or body) holds token idx."""
        best = None
        for lo, hi, owner in self.scopes:
            if lo <= idx < hi and (best is None or lo >= best[0]):
                best = (lo, hi, owner)
        return best[2] if best else None

    def _gen_method(self, name: str, o: int) -> bool:
        cls = self.in_gen_class(o)
        return cls is not None and name in self.v.gen_methods.get(cls, set())

    def region_draws(self, lo: int, hi: int) -> bool:
        """Whether toks[lo:hi] evaluates a draw (lambda and block bodies aside)."""
        k = lo
        st = self.st
        while k < hi:
            t = self.toks[k]
            if t.text in ('(', '[', '{') and k in st.close:
                kind = st.kind.get(k)
                c = st.close[k]
                if kind == CALL and self.is_draw_call(k):
                    return True
                if kind in (CAPTURE, PARAMS, BLOCK, NAME):
                    k = c + 1
                    continue
                if self.region_draws(k + 1, c):
                    return True
                k = c + 1
                continue
            k += 1
        return False

    # -- walking -------------------------------------------------------------
    def check_file(self) -> None:
        self.walk(0, len(self.toks))

    def walk(self, lo: int, hi: int) -> None:
        """Check one region: its unsequenced operands, then every nested group."""
        self.check_operands(lo, hi)
        k = lo
        st = self.st
        while k < hi:
            t = self.toks[k]
            if t.text in ('(', '[', '{') and k in st.close:
                c = st.close[k]
                kind = st.kind.get(k)
                if kind == CALL:
                    self.check_call(k)
                    for a, b in self.split_args(k):
                        self.walk(a, b)
                elif kind == PARAMS:
                    self.check_params(k)
                elif kind in (CAPTURE, NAME):
                    pass
                elif kind == CONTROL and k > 0 and self.toks[k - 1].text == 'for':
                    self.walk(k + 1, c)
                else:
                    self.walk(k + 1, c)
                k = c + 1
                continue
            k += 1

    def check_call(self, o: int) -> None:
        args = self.split_args(o)
        if len(args) < 2:
            return
        drawing = [n + 1 for n, (a, b) in enumerate(args)
                   if not self.bare_generator(a, b) and self.region_draws(a, b)]
        if len(drawing) >= 2:
            callee = self.callee_text(o)
            self.findings.add(Finding(
                self.ff.rel, self.toks[o].line, 'ARG',
                f'{callee}(...): arguments {", ".join(map(str, drawing))} each draw; '
                f'the compiler picks their order'))

    def callee_text(self, o: int) -> str:
        toks = self.toks
        p = o - 1
        parts = []
        while p >= 0 and len(parts) < 5:
            t = toks[p]
            if (t.kind == 'id' and t.text not in KEYWORDS) or t.text in ('.', '->', '::') \
                    or t.text == 'this':
                parts.append(t.text)
                p -= 1
                continue
            if t.text == '>' and p in self.st.template_close:
                parts.append('<>')
                for op_, cl in self.st.template_open.items():
                    if cl == p:
                        p = op_ - 1
                        break
                else:
                    p -= 1
                continue
            break
        return ''.join(reversed(parts)) or '(expression)'

    def check_params(self, o: int) -> None:
        for a, b in self.split_args(o):
            texts = []
            k = a
            while k < b:
                x = self.toks[k].text
                if x == '<' and k in self.st.template_open:
                    k = self.st.template_open[k] + 1
                    continue
                if x == '=':
                    break
                texts.append(x)
                k += 1
            if any(x in self.v.gen_types for x in texts) and not any(
                    x in ('&', '&&', '*') for x in texts):
                name = next((x for x in texts if x in self.v.gen_types), '?')
                self.findings.add(Finding(
                    self.ff.rel, self.toks[a].line if a < len(self.toks) else 0, 'BYVALUE',
                    f'parameter of generator type {name} taken by value; pass it by '
                    f'reference'))

    def pieces(self, lo: int, hi: int) -> Iterable[list[int]]:
        """Top-level token indices of toks[lo:hi], cut at every sequenced
        punctuator, statement keyword, block and condition."""
        toks = self.toks
        st = self.st
        piece: list[int] = []
        k = lo
        while k < hi:
            t = toks[k]
            x = t.text
            if x in ('(', '[', '{') and k in st.close:
                kind = st.kind.get(k)
                c = st.close[k]
                if kind in (BLOCK, CONTROL, PARAMS, NAME):
                    if piece:
                        yield piece
                    piece = []
                    k = c + 1
                    continue
                if kind == CAPTURE:
                    # a lambda expression is one atom that draws nothing now
                    j = st.after_params(c)
                    if j < len(toks) and toks[j].text == '(' and st.kind.get(j) == PARAMS:
                        j = st.after_params(st.close[j])
                    if j < len(toks) and toks[j].text == '{' and j in st.close:
                        piece.append(-1)
                        k = st.close[j] + 1
                        continue
                    k = c + 1
                    continue
                piece.append(k)
                k = c + 1
                continue
            if x == '<' and k in st.template_open:
                k = st.template_open[k] + 1
                continue
            if t.kind == 'op' and x == '>' and k not in st.template_close and \
                    k + 1 < hi and toks[k + 1].joined and (k + 1) not in st.template_close:
                if piece:
                    yield piece
                piece = []
                k += 2          # '>>': sequenced in C++17
                continue
            if (t.kind == 'op' and x in SEPARATOR_OPS) or (t.kind == 'id' and x in SEPARATOR_WORDS):
                if piece:
                    yield piece
                piece = []
                k += 1
                continue
            piece.append(k)
            k += 1
        if piece:
            yield piece

    def check_operands(self, lo: int, hi: int) -> None:
        toks = self.toks
        st = self.st
        for piece in self.pieces(lo, hi):
            operands: list[list[int]] = [[]]
            ops: list[int] = []
            for pos, k in enumerate(piece):
                if k < 0:
                    operands[-1].append(k)
                    continue
                t = toks[k]
                if t.kind == 'op' and t.text in UNSEQUENCED_BINARY and k not in st.template_close:
                    prev = piece[pos - 1] if pos > 0 else None
                    binary = prev is not None and prev >= 0 and self._ends_operand(prev)
                    if prev is not None and prev < 0:
                        binary = True       # after a lambda atom
                    if binary:
                        ops.append(k)
                        operands.append([])
                        continue
                operands[-1].append(k)
            if len(operands) < 2:
                continue
            drawing = [n for n, ks in enumerate(operands) if self._operand_draws(ks)]
            if len(drawing) >= 2:
                op_idx = ops[drawing[1] - 1]
                self.findings.add(Finding(
                    self.ff.rel, toks[op_idx].line, 'OPERAND',
                    f"both sides of '{toks[op_idx].text}' draw; the compiler picks their "
                    f"order"))

    def _ends_operand(self, k: int) -> bool:
        t = self.toks[k]
        if t.text in ('(', '[', '{') and k in self.st.close:
            return True     # a group atom stands for its closing bracket
        return self.st.operand_end(k)

    def _operand_draws(self, ks: list[int]) -> bool:
        st = self.st
        for k in ks:
            if k < 0:
                continue
            t = self.toks[k]
            if t.text in ('(', '[', '{') and k in st.close:
                kind = st.kind.get(k)
                if kind == CALL and self.is_draw_call(k):
                    return True
                if self.region_draws(k + 1, st.close[k]):
                    return True
        return False


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def iter_sources(root: Path, targets: list[str]) -> list[Path]:
    out: list[Path] = []
    for target in targets:
        base = (root / target) if not Path(target).is_absolute() else Path(target)
        if base.is_file():
            out.append(base)
            continue
        if not base.is_dir():
            continue
        for p in sorted(base.rglob('*')):
            if p.suffix in SUFFIXES and p.is_file():
                out.append(p)
    uniq = []
    seen = set()
    for p in out:
        rp = p.resolve()
        if rp in seen:
            continue
        seen.add(rp)
        try:
            rel = rp.relative_to(root.resolve()).as_posix()
        except ValueError:
            rel = rp.as_posix()
        if any(rel.startswith(prefix) for prefix in EXCLUDED_PREFIXES):
            continue
        uniq.append(rp)
    return uniq


def scan(root: Path, targets: list[str], vocabulary_out=None) -> list[Finding]:
    root = root.resolve()
    files = iter_sources(root, targets)
    raw_cache: dict[Path, str] = {}
    facts: dict[Path, FileFacts] = {}

    def rel_of(p: Path) -> str:
        try:
            return p.relative_to(root).as_posix()
        except ValueError:
            return p.as_posix()

    def raw(p: Path) -> str | None:
        if p not in raw_cache:
            try:
                raw_cache[p] = p.read_text(encoding='utf-8', errors='replace')
            except OSError:
                return None
        return raw_cache[p]

    def closure(p: Path) -> list[Path]:
        order: list[Path] = []
        pending = [p]
        seen: set[Path] = set()
        while pending:
            cur = pending.pop()
            cur = cur.resolve()
            if cur in seen:
                continue
            seen.add(cur)
            text = raw(cur)
            if text is None:
                continue
            order.append(cur)
            for m in INCLUDE_RE.finditer(text):
                pending.append(cur.parent / m.group(1))
        return order

    def fact(p: Path) -> FileFacts:
        if p not in facts:
            facts[p] = collect_facts(p, rel_of(p), raw(p) or '')
        return facts[p]

    # units: every scanned file with its include closure
    units = {p: closure(p) for p in files}
    containing: dict[Path, list[Path]] = {}
    for p, members in units.items():
        for m in members:
            containing.setdefault(m, []).append(p)
    hinted_unit = {p: any(HINT.search(raw(m) or '') for m in members)
                   for p, members in units.items()}
    vocab: dict[Path, Vocabulary] = {}
    for p, members in units.items():
        if hinted_unit[p]:
            vocab[p] = build_vocabulary([fact(m) for m in members])
    findings: set[Finding] = set()
    scanned = set(files)
    for f in files:
        for unit in containing.get(f, [f]):
            v = vocab.get(unit)
            if v is None:
                continue
            if not v.gen_objects and not v.drawers:
                continue
            checker = Checker(fact(f), v)
            checker.check_file()
            findings |= checker.findings
    if vocabulary_out is not None:
        for p in sorted(vocab):
            v = vocab[p]
            if not v.gen_objects and not v.drawers:
                continue
            vocabulary_out.append(
                f'{rel_of(p)}: generator types {sorted(v.gen_types - STD_ENGINES)}; '
                f'objects {sorted(v.gen_objects)}; drawers {v.names()}')
    del scanned
    return sorted(findings)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('paths', nargs='*', help='files or directories (default: %s)' %
                    ', '.join(DEFAULT_DIRS))
    ap.add_argument('--root', type=Path, default=ROOT, help='checkout to scan (default: this one)')
    ap.add_argument('--vocabulary', action='store_true',
                    help='print what each translation unit counts as a draw')
    args = ap.parse_args(argv)
    root = args.root
    if not root.is_dir():
        print(f'check_rng_draw_order: no such directory {root}', file=sys.stderr)
        return 2
    targets = args.paths or list(DEFAULT_DIRS)
    vocab_lines: list[str] | None = [] if args.vocabulary else None
    findings = scan(root, targets, vocab_lines)
    if vocab_lines is not None:
        for line in vocab_lines:
            print(line)
    lines_cache: dict[str, list[str]] = {}
    for f in findings:
        if f.rel not in lines_cache:
            try:
                lines_cache[f.rel] = (root / f.rel).read_text(
                    encoding='utf-8', errors='replace').splitlines()
            except OSError:
                lines_cache[f.rel] = []
        src = lines_cache[f.rel]
        text = src[f.line - 1].strip() if 0 < f.line <= len(src) else ''
        print(f'{f.rel}:{f.line}: {f.kind} {f.detail}\n    {text}')
    if findings:
        print(f'check_rng_draw_order: {len(findings)} finding(s). Take each draw in a '
              f'statement of its own, into a named local, in the order the arguments are '
              f'written (left to right).', file=sys.stderr)
        return 1
    print('check_rng_draw_order: no draw left to the compiler\'s evaluation order')
    return 0


if __name__ == '__main__':
    sys.exit(main())
