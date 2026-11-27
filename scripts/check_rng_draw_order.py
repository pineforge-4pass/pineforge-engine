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
  ``r.unit()``, ``s.below(3)``, ``rng_->percent(50)``, ``state.rng.below(3)``;
- a call of a *drawer*: a function, member function or named lambda whose
  body draws, found transitively over the translation unit (``long_id()``,
  ``units()``, ``opening(...)``, ``pick(rng, ...)``, ``walk(r, ...)``), or
  whose body applies a PRNG step to state it does not declare (a lambda over a
  captured ``state``, a function over a global ``mix_state``);
- a call that receives a generator as an argument (``dist(rng)``,
  ``random_text(rng, n)``, ``std::shuffle(b, e, rng)``): the callee may draw.

Draws in branch or loop bodies count. A fresh automatic generator is checked
inside its function; it does not by itself make calls of that function draw
from shared state. A lambda's body counts when the lambda is called.

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
from typing import Iterable, Optional, Tuple, Union

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
ANONYMOUS_NS = '(anonymous)'
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
        self.template_close: dict[int, int] = {}
        self.kind: dict[int, str] = {}
        self.parent = [-1] * len(toks)
        self._templates()
        self._brackets()
        self.class_heads = self._class_heads()
        self.body_params: dict[int, int] = {}  # definition brace -> parameter list or capture
        self.mem_initializers: set[int] = set()
        for i in sorted(self.close):
            self.kind[i] = self._classify(i)

    # -- template argument lists ------------------------------------------
    def _templates(self) -> None:
        toks = self.toks
        for i, t in enumerate(toks):
            if t.text != '<' or i == 0 or i in self.template_close:
                continue
            p = toks[i - 1]
            if p.kind != 'id' or (p.text in KEYWORDS and p.text not in (
                    'template', 'static_cast', 'dynamic_cast', 'const_cast', 'reinterpret_cast')):
                if not (p.text == '>' and (i - 1) in self.template_close):
                    continue
            j = self._template_end(i)
            if j is not None:
                self.template_open[i] = j
                self.template_close[j] = i

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
            if stack:
                self.parent[i] = stack[-1]
            x = t.text
            if t.kind != 'op':
                continue
            if x in '([{' and len(x) == 1:
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

    def name_before(self, end: int) -> int:
        """Last identifier of a possibly specialized name ending before end."""
        return self.template_close.get(end - 1, end) - 1

    def split_args(self, o: int) -> list[tuple[int, int]]:
        c = self.close[o]
        args = []
        start = k = o + 1
        while k < c:
            if self.toks[k].text == ',':
                args.append((start, k))
                start = k + 1
            k = self.close.get(k, self.template_open.get(k, k)) + 1
        if start < c or args:
            args.append((start, c))
        return args

    def _qualified_id_end(self, k: int) -> int:
        toks = self.toks
        if k < len(toks) and toks[k].text == '::':
            k += 1
        while k < len(toks) and toks[k].kind == 'id' and toks[k].text not in KEYWORDS:
            k += 1
            if k in self.template_open:
                k = self.template_open[k] + 1
            if k >= len(toks) or toks[k].text != '::':
                break
            k += 1
        return k

    def _class_heads(self) -> dict[int, tuple[int, str]]:
        """Match the class head forward. A template parameter has no class body."""
        toks = self.toks
        heads = {}
        for i, t in enumerate(toks):
            if t.text not in ('class', 'struct', 'union') or (i and toks[i - 1].text == 'enum'):
                continue
            k = i + 1
            while k in self.close and toks[k].text == '[':  # attributes
                k = self.close[k] + 1
            end = self._qualified_id_end(k)
            if end == k:
                continue
            name = toks[self.name_before(end)].text
            k = end
            if k < len(toks) and toks[k].text == 'final':
                k += 1
            if k < len(toks) and toks[k].text == ':':
                k += 1
                while k < len(toks) and toks[k].text not in ('{', '}', ';', ')', '>'):
                    k = self.template_open.get(k, self.close.get(k, k)) + 1
            if k in self.close and toks[k].text == '{':
                heads[k] = (i, name)
        return heads

    def _trailing_return(self, close_idx: int) -> bool:
        """Only a lambda declarator or an auto function can introduce -> type."""
        toks = self.toks
        o = self.open_of[close_idx]
        if toks[o].text == '[':
            return self.is_lambda_capture(o)
        k = o - 1
        if k in self.open_of and toks[k].text == ']':
            return self.is_lambda_capture(self.open_of[k])
        while k >= 0:
            if toks[k].text == 'auto':
                return True
            if k in self.template_close:
                k = self.template_close[k] - 1
            elif toks[k].kind == 'id' or toks[k].text in ('::', '~'):
                k -= 1
            else:
                break
        return False

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
            if x == '->' and self._trailing_return(close_idx):
                j += 1
                while j < len(toks) and toks[j].text not in ('{', '}', ';', '=', ',', ')'):
                    j = self.template_open.get(j, self.close.get(j, j)) + 1
                continue
            break
        return j

    def _constructor_body(self, colon: int) -> int | None:
        """Require a complete mem-initializer-list followed by the body. A
        conditional's colon and a case label do not introduce such a list."""
        toks = self.toks
        groups = []
        k = colon + 1
        while k < len(toks):
            end = self._qualified_id_end(k)
            if end == k or end not in self.close or toks[end].text not in ('(', '{'):
                break
            groups.append(end)
            k = self.close[end] + 1
            if k < len(toks) and toks[k].text == '...':
                k += 1
            if k in self.close and toks[k].text == '{':
                self.mem_initializers.update(groups)
                return k
            if k >= len(toks) or toks[k].text != ',':
                break
            k += 1
        return None

    def _classify(self, i: int) -> str:
        toks = self.toks
        x = toks[i].text
        p = self.prev(i)
        if i in self.mem_initializers:
            return CALL if x == '(' else INIT
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
            j = self.after_params(self.close[i])
            if j < len(toks) and toks[j].text == ':':
                body = self._constructor_body(j)
            else:
                body = j if j in self.close and toks[j].text == '{' else None
            if body is not None:
                self.body_params[body] = i
                return PARAMS
            return CALL
        if x == '[':
            if self.is_lambda_capture(i):
                j = self.after_params(self.close[i])
                if j in self.close and toks[j].text == '{':
                    self.body_params[j] = i
                return CAPTURE
            return SUBSCRIPT
        # '{'
        return INIT if self._brace_is_init(i) else BLOCK

    def _brace_is_expression(self, close_idx: int) -> bool:
        o = self.open_of.get(close_idx)
        if self.kind.get(o) == INIT:
            return True
        params = self.body_params.get(o)
        return params is not None and self.lambda_capture(params) is not None

    def lambda_capture(self, params: int) -> int | None:
        if self.toks[params].text == '[':
            return params
        prev = self.open_of.get(params - 1)
        return prev if self.kind.get(prev) == CAPTURE else None

    def _brace_is_init(self, i: int) -> bool:
        toks = self.toks
        p = self.prev(i)
        if i in self.body_params or i in self.class_heads or p is None:
            return False
        x = p.text
        if p.kind == 'str' and i >= 2 and toks[i - 2].text == 'extern':
            return False
        if x in ('=', 'return', ',', '(', '[', '?'):
            return True
        if x == '{':
            # {{...}} nested init inside an init; a block inside a block
            return self.kind.get(self.parent[i]) == INIT
        if x in (')', ';', '}', 'else', 'do', 'try', 'mutable', 'noexcept', 'const',
                 'override', 'final', 'volatile', '&', '&&'):
            return False
        if x == ']':
            o = self.open_of.get(i - 1)
            return not (o is not None and self.is_lambda_capture(o))
        if x == ':':
            par = self.parent[i]
            return par >= 0 and toks[par].text == '('     # range-for over a list
        if p.kind == 'id' or (x == '>' and (i - 1) in self.template_close):
            if p.kind == 'id' and x in KEYWORDS and x not in HEAD_WORDS:
                return x not in ('else', 'do', 'try')
            # A namespace, enum or anonymous class also introduces a block.
            k = i - 1
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
                    k = self.template_close[k] - 1
                    continue
                if t.kind == 'id' and t.text in HEAD_WORDS:
                    return False
                if t.text == ':' and k > 0 and toks[k - 1].kind == 'id' and \
                        toks[k - 1].text in ('public', 'private', 'protected'):
                    break
                k -= 1
            return True
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
    declarator: int                 # parameter list (or capture for a parameterless lambda)
    lambda_name: int | None = None  # declaration token, distinct even for same-named lambdas
    steps: set[str] = field(default_factory=set)    # non-locals a PRNG step advances
    mixes: set[str] = field(default_factory=set)    # non-locals updated from an input
    returns_value: bool = False
    qualifier: str | None = None    # Class of an out-of-line Class::name definition
    deduced: dict[int, Binding] = field(default_factory=dict)  # argument slot -> type parameter's object

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


@dataclass(frozen=True)
class Binding:
    type_name: str
    path: Path
    pos: int
    end: int
    automatic: bool   # a fresh value owned by this invocation, not a reference or static


@dataclass
class FileFacts:
    path: Path
    rel: str
    toks: list[Tok]
    st: Structure
    includes: list[Path]
    functions: list[Function]
    classes: list[ClassDef]
    locals: dict[str, list[Binding]]
    fields: dict[tuple[str, str], Binding]            # class, field
    globals: dict[tuple[tuple[str, ...], str], Binding]  # namespace, object
    namespaces: dict[int, tuple[str, ...]]            # opening brace -> qualified namespace
    references: dict[Binding, tuple[int, int]] = field(default_factory=dict)
    parameter_types: set[str] = field(default_factory=set)
    direct_globals: dict[Binding, int] = field(default_factory=dict)  # candidate initializer '('

    def namespace_at(self, pos: int) -> tuple[str, ...]:
        scope = self.st.parent[pos]
        while scope >= 0 and scope not in self.namespaces:
            scope = self.st.parent[scope]
        return self.namespaces.get(scope, ())


UPDATE_OPS = frozenset(('^=', '+=', '-=', '*=', '|=', '&=', '='))
GEN_NAME_RE = re.compile(r'(?i)rng|random|splitmix|xorshift|xoshiro|xoroshiro|pcg|lcg|prng')


def body_tokens(st: Structure, lo: int, hi: int) -> Iterable[int]:
    """An ordinary block executes here; nested definitions execute elsewhere."""
    k = lo
    while k < hi:
        if k in st.body_params or k in st.class_heads:
            k = st.close[k] + 1
        else:
            yield k
            k += 1


def body_updates(toks: list[Tok], st: Structure, lo: int, hi: int) -> tuple[set[str], set[str]]:
    """(stepped, mixed): identifiers a PRNG step advances from their own value
    alone (s ^= s << 13; s ^= s >> 7; state += 0x9e37...; s = s * K + C), and
    identifiers updated from anything else (h ^= byte; h = (h ^ v) * K)."""
    stepped: set[str] = set()
    mixed: set[str] = set()
    for k in body_tokens(st, lo, hi - 2):
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
                           for k in body_tokens(st, lo, hi - 1))


def collect_facts(path: Path, rel: str, raw: str) -> FileFacts:
    clean = strip_source(raw)
    toks = tokenize(clean)
    st = Structure(toks)
    includes = [path.parent / m.group(1) for m in INCLUDE_RE.finditer(raw)]
    functions: list[Function] = []
    classes: list[ClassDef] = []
    for o, (k, name) in st.class_heads.items():
        bases = base_names(toks, st, k, o)
        classes.append(ClassDef(name, (o + 1, st.close[o]), has_base=bool(bases), bases=bases))

    def owner_of(idx: int) -> str | None:
        best = None
        for cd in classes:
            if cd.body[0] <= idx < cd.body[1]:
                if best is None or cd.body[0] > best.body[0]:
                    best = cd
        return best.name if best else None

    # function definitions and named lambdas
    for j, o in st.body_params.items():
        body = (j + 1, st.close[j])
        capture = st.lambda_capture(o)
        name = '<lambda>'
        lambda_name = None
        p = st.name_before(o)
        if capture is not None:
            if capture >= 2 and toks[capture - 1].text == '=' and toks[capture - 2].kind == 'id':
                lambda_name = capture - 2
                name = toks[lambda_name].text
        elif toks[p].kind == 'id':
            name = toks[p].text
        elif toks[p].text == ')' and p in st.open_of and toks[st.open_of[p] - 1].text == 'operator':
            name = 'operator()'
        params = (o + 1, st.close[o]) if toks[o].text == '(' else None
        fn = Function(name, body, body_locals(toks, st, params, *body), owner_of(o), o, lambda_name)
        if capture is None and p >= 2 and toks[p - 1].text == '::':
            fn.qualifier = toks[st.name_before(p - 1)].text
        finish_function(fn, toks, st)
        functions.append(fn)
    for fn in functions:
        for cd in classes:
            if fn.owner == cd.name and st.lambda_capture(fn.declarator) is None:
                cd.methods.add(fn.name)

    ff = FileFacts(path, rel, toks, st, includes, functions, classes, {}, {}, {}, {})
    for i, t in enumerate(toks):
        if t.text == 'namespace':
            j = st._qualified_id_end(i + 1)
            if j in st.close and toks[j].text == '{':
                parts = tuple(t.text for t in toks[i + 1:j] if t.kind == 'id')
                ff.namespaces[j] = ff.namespace_at(i) + (parts or (ANONYMOUS_NS,))
        elif t.text == 'extern' and i + 2 in st.close and toks[i + 1].kind == 'str' \
                and toks[i + 2].text == '{':
            ff.namespaces[i + 2] = ff.namespace_at(i)  # linkage does not create a scope
    collect_bindings(ff)
    return ff


BUILTIN_TYPES = frozenset(('auto', 'bool', 'char', 'double', 'float', 'int', 'long', 'short',
                           'signed', 'unsigned', 'void'))
DECL_SPECIFIERS = frozenset(('const', 'volatile', 'constexpr', 'extern', 'inline', 'mutable',
                             'static', 'thread_local', 'typename'))


def parameter_type(st: Structure, a: int, b: int) -> tuple[int, int]:
    """The type-specifier in a parameter declaration, before its declarator."""
    toks = st.toks
    while a < b and toks[a].text in ('const', 'volatile', 'typename', 'class', 'struct', 'enum'):
        a += 1
    k = a
    while k < b and toks[k].text in BUILTIN_TYPES:
        k += 1
    return a, k if k > a else st._qualified_id_end(a)


def template_type_names(st: Structure, declarator: int) -> set[str]:
    """Type parameters of this function's template head, if it has one."""
    toks = st.toks
    k = st.name_before(declarator) - 1
    while k >= 0:
        if k in st.template_close:
            o = st.template_close[k]
            if o and toks[o - 1].text == 'template':
                return {toks[i + 1].text for i in range(o + 1, k - 1)
                        if toks[i].text in ('class', 'typename') and toks[i + 1].kind == 'id'}
            k = o - 1
        elif toks[k].kind == 'id' or toks[k].text in ('::', '*', '&', '&&'):
            k -= 1
        else:
            break
    return set()


def prototype(st: Structure, o: int, types: set[str]) -> bool:
    """Check parameter declarators, not just the first word inside the parens.
    In particular a qualified type followed by a parameter name is unambiguous.
    A lone identifier could instead be a value used to initialize an object."""
    toks = st.toks
    if st.parent[o] in st.class_heads:
        return True  # a nonstatic data member cannot have a parenthesized initializer here
    for a, b in st.split_args(o):
        start, k = parameter_type(st, a, b)
        if k <= start or k > b:
            return False
        known = toks[st.name_before(k)].text in types or start != a
        while k < b and toks[k].text in ('const', 'volatile', '&', '&&', '*', '...'):
            k += 1
        if k < b and toks[k].kind == 'id' and toks[k].text not in KEYWORDS:
            known = True
            k += 1
        while k < b and k in st.close and toks[k].text == '[':
            k = st.close[k] + 1
        if not known or (k != b and toks[k].text != '='):
            return False
    return True


def collect_bindings(ff: FileFacts) -> None:
    """Record declarators at their point of declaration. Parameters extend
    through their definition (including constructor initializers); prototype
    parameters end at the closing parenthesis."""
    toks, st = ff.toks, ff.st
    param_ends = {fn.declarator: fn.body[1] for fn in ff.functions}
    deduced = {}
    # Definitions supply type evidence for prototypes with unnamed parameters.
    for fn in ff.functions:
        if toks[fn.declarator].text == '(':
            generic = template_type_names(st, fn.declarator)
            for slot, (a, b) in enumerate(st.split_args(fn.declarator)):
                start, end = parameter_type(st, a, b)
                if start < end <= b:
                    ff.parameter_types.add(toks[st.name_before(end)].text)
                    if end == start + 1 and toks[start].text in generic:
                        deduced[start] = (fn, slot)
    type_names = ff.parameter_types | BUILTIN_TYPES | STD_ENGINES | {c.name for c in ff.classes}
    specifiers = DECL_SPECIFIERS | BUILTIN_TYPES
    for k in range(len(toks) - 1):
        t = toks[k]
        if t.kind != 'id' or (t.text in KEYWORDS and t.text not in BUILTIN_TYPES):
            continue
        j = k + 1
        if j in st.template_open:
            j = st.template_open[j] + 1
        while j < len(toks) and toks[j].text in ('&', '*', '&&', 'const', 'volatile'):
            j += 1
        ptr_group = j if (j + 3 in st.open_of and st.open_of[j + 3] == j
                          and toks[j + 1].text in ('*', '&') and j + 4 in st.close
                          and toks[j + 4].text == '(') else None
        if ptr_group is not None:
            j += 2  # int (*name)(...)
        if j + 1 >= len(toks) or toks[j].kind != 'id' or toks[j].text in KEYWORDS:
            continue
        after = toks[j + 1].text
        if after not in ('(', '{', '=', ';', ',', ')', '[', ':') or st.kind.get(j + 1) == PARAMS:
            continue
        if after == '(' and j + 1 in st.close and prototype(st, j + 1, type_names):
            continue
        start = k
        while start >= 2 and toks[start - 1].text == '::':
            start = st.name_before(start - 1)
        while start > 0 and toks[start - 1].text in specifiers:
            start -= 1
        if start > 0 and toks[start - 1].text not in ('(', ';', '{', '}', ',', ':', ')', '>', ']'):
            continue  # e.g. return x * y, not a declaration of y
        type_name = t.text
        indirect = any(t.text in ('&', '&&', '*') for t in toks[k + 1:j])
        if type_name == 'auto' and after == '=':
            m = j + 2
            if m < len(toks) and toks[m].text == '*':
                m += 1
            if m + 1 in st.template_open and toks[m].text == 'static_cast':
                m += 2  # auto& state = *static_cast<ns::State*>(...)
            end = st._qualified_id_end(m)
            if end > m and end < len(toks) and toks[end].text in ('(', '{', '*', '&', '>'):
                type_name = toks[st.name_before(end)].text
        scope = st.parent[ptr_group if ptr_group is not None else j]
        end = st.close.get(scope, len(toks))
        if scope in param_ends:
            end = param_ends[scope]
        elif st.kind.get(scope) == CONTROL and end + 1 in st.close and toks[end + 1].text == '{':
            end = st.close[end + 1]   # if/for/while initializer
            if end + 2 in st.close and toks[end + 1].text == 'else':
                end = st.close[end + 2]
        name = toks[j].text
        auto = not indirect and not any(t.text in ('static', 'thread_local', 'extern')
                                         for t in toks[start:k])
        decl = Binding(type_name, ff.path, j, end, auto)
        if k in deduced:
            fn, slot = deduced[k]
            fn.deduced[slot] = decl
        if t.text == 'auto' and after == '=' and any(
                tok.text in ('&', '&&') for tok in toks[k + 1:j]):
            stop = j + 2
            while stop < len(toks) and toks[stop].text not in (';', ',', ')', '}'):
                stop = st.template_open.get(stop, st.close.get(stop, stop)) + 1
            ff.references[decl] = (j + 2, stop)
        if scope in st.class_heads:
            ff.fields[st.class_heads[scope][1], name] = decl
        elif scope == -1 or scope in ff.namespaces:
            ff.globals[ff.namespaces.get(scope, ()), name] = decl
            if after == '(' and j + 1 in st.close:
                ff.direct_globals[decl] = j + 1
        else:
            ff.locals.setdefault(name, []).append(decl)


# ---------------------------------------------------------------------------
# Vocabulary of one translation unit
# ---------------------------------------------------------------------------

# Unlike postponed annotations, a type alias is evaluated at import time.
DrawerKey = Union[Tuple[Optional[str], str], Tuple[Path, int]]


@dataclass
class Vocabulary:
    gen_types: set[str] = field(default_factory=set)
    gen_objects: set[str] = field(default_factory=set)  # display/fast skip only, never lookup
    gen_methods: dict[str, set[str]] = field(default_factory=dict)   # class -> methods
    # For each drawer: the bindings whose storage can advance. None denotes
    # an unconditional effect (e.g. a PRNG step on shared scalar state).
    drawers: dict[DrawerKey, set[Binding | None]] = field(default_factory=dict)
    lambda_names: dict[tuple[Path, int], str] = field(default_factory=dict)
    classes: set[str] = field(default_factory=set)
    bases: dict[str, set[str]] = field(default_factory=dict)
    fields: dict[tuple[str, str], Binding] = field(default_factory=dict)
    globals: dict[tuple[tuple[str, ...], str], Binding] = field(default_factory=dict)
    references: dict[Binding, Binding] = field(default_factory=dict)
    generator_params: set[Binding] = field(default_factory=set)

    def is_generator(self, binding: Binding | None) -> bool:
        return binding is not None and (
            binding.type_name in self.gen_types or binding in self.generator_params)

    def lineage(self, cls: str | None) -> set[str]:
        if cls is None:
            return set()
        out = set()
        pending = [cls]
        while pending:
            c = pending.pop()
            if c in out:
                continue
            out.add(c)
            pending.extend(self.bases.get(c, ()))
        return out

    def names(self) -> list[str]:
        return sorted({self.lambda_names[(o, n)] if isinstance(n, int)
                       else ((o + '::' if o else '') + n) for o, n in self.drawers})


def resolved_owner(fn: Function, classes: set[str]) -> str | None:
    if fn.qualifier and fn.qualifier in classes:
        return fn.qualifier
    return fn.owner


def infer_generator_bindings(unit: list[FileFacts], v: Vocabulary,
                            checkers: dict[int, Checker]) -> None:
    """Resolve references and propagate generator arguments to the particular
    template declarations they instantiate, including calls through another template."""
    templates = {}
    for ff in unit:
        checker = checkers[id(ff)]
        for ref, expr in ff.references.items():
            pos = checker.bare_object(*expr)
            if pos is not None:
                source = checker.binding(pos)
                if source is not None and source != ref:
                    v.references[ref] = source
        for fn in ff.functions:
            if fn.deduced:
                scope = resolved_owner(fn, v.classes) or ff.namespace_at(fn.declarator)
                arity = len(ff.st.split_args(fn.declarator))
                signatures = templates.setdefault(fn.name, {})
                signatures.setdefault((scope, arity), []).extend(fn.deduced.items())

    # An edge describes storage passed as a bare argument, not an arbitrary
    # identifier with the same spelling. Known non-generator arguments seed no edges.
    edges: dict[Binding, set[Binding]] = {}
    for ff in unit:
        checker = checkers[id(ff)]
        checker._bindings.clear()  # receivers may now resolve through a reference
        if not templates:
            continue
        for o, kind in ff.st.kind.items():
            if kind != CALL:
                continue
            p = ff.st.name_before(o)
            signatures = templates.get(ff.toks[p].text) if p >= 0 else None
            if not signatures or checker.binding(p) is not None:
                continue
            args = ff.st.split_args(o)
            for scope in checker.call_scopes(p):
                matches = signatures.get((scope, len(args)))
                if matches:
                    for slot, formal in matches:
                        pos = checker.bare_object(*args[slot])
                        actual = checker.binding(pos) if pos is not None else None
                        if actual is not None:
                            edges.setdefault(actual, set()).add(formal)
                    break
    pending = [source for source in edges if v.is_generator(source)]
    while pending:
        for formal in edges.get(pending.pop(), ()):
            if formal not in v.generator_params:
                v.generator_params.add(formal)
                pending.append(formal)
    files = {ff.path: ff for ff in unit}
    for binding in v.generator_params | {r for r, d in v.references.items() if v.is_generator(d)}:
        v.gen_objects.add(files[binding.path].toks[binding.pos].text)


def build_vocabulary(unit: list[FileFacts]) -> Vocabulary:
    v = Vocabulary()
    v.gen_types |= STD_ENGINES
    # A generator class: one of its methods is a stepper, and it is named as
    # a generator or stands alone (no base class).  A host class with a
    # private draw() is not a generator; its draw() is a drawer below.
    for ff in unit:
        for cd in ff.classes:
            steppers = [fn for fn in ff.functions if fn.owner == cd.name and fn.stepper
                        and ff.st.lambda_capture(fn.declarator) is None]
            if steppers and (GEN_NAME_RE.search(cd.name) or not cd.has_base):
                v.gen_types.add(cd.name)
                v.gen_methods.setdefault(cd.name, set()).update(cd.methods)
    for ff in unit:
        for cd in ff.classes:
            v.classes.add(cd.name)
            v.bases.setdefault(cd.name, set()).update(cd.bases)
    types = v.classes | v.gen_types | BUILTIN_TYPES
    for ff in unit:
        types.update(ff.parameter_types)
    for ff in unit:
        v.fields.update(ff.fields)
        for key, decl in ff.globals.items():
            # A header alone may not distinguish an unknown type-id from a
            # value in parentheses. Revisit it with this unit's definitions.
            o = ff.direct_globals.get(decl)
            if o is None or not prototype(ff.st, o, types):
                v.globals[key] = decl
        v.gen_objects.update(name for name, decls in ff.locals.items()
                             if any(d.type_name in v.gen_types for d in decls))
        for scope in (ff.fields, ff.globals):
            v.gen_objects.update(name for (_, name), d in scope.items() if d.type_name in v.gen_types)
    # C++ implicitly imports an unnamed namespace into its enclosing namespace.
    # Keep the declaring scope and binding as well (including its source position).
    for (ns, name), obj in list(v.globals.items()):
        for i in reversed(range(len(ns))):
            if ns[i] == ANONYMOUS_NS:
                ns = ns[:i] + ns[i + 1:]
                v.globals.setdefault((ns, name), obj)
    # drawers: free functions, lambdas and non-generator methods whose body
    # draws, or which are steppers themselves; to a fixpoint.
    candidates = []
    for ff in unit:
        for fn in ff.functions:
            owner = resolved_owner(fn, v.classes)
            key: DrawerKey
            if fn.lambda_name is not None:
                key = (ff.path, fn.lambda_name)
                v.lambda_names[key] = fn.name
            elif fn.name == '<lambda>' or owner in v.gen_methods:
                continue
            else:
                key = (owner, fn.name)
            candidates.append((ff, fn, key))
            if fn.stepper:
                v.drawers.setdefault(key, set()).add(None)
    checkers = {id(ff): Checker(ff, v) for ff in unit}
    infer_generator_bindings(unit, v, checkers)
    changed = True
    while changed:
        changed = False
        for ff, fn, key in candidates:
            effects = v.drawers.get(key, set())
            if None in effects:
                continue
            checker = checkers[id(ff)]
            found = checker.function_effects(fn)
            if found - effects:
                v.drawers.setdefault(key, set()).update(found)
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
        self.scopes = {cd.body[0] - 1: cd.name for cd in ff.classes}
        self.scopes.update({fn.body[0] - 1: resolved_owner(fn, v.classes)
                            for fn in ff.functions if ff.st.lambda_capture(fn.declarator) is None})
        self.summarizing: Function | None = None
        self.effects: set[Binding | None] = set()
        self._bindings: dict[int, Binding | None] = {}

    # -- what is a draw ------------------------------------------------------
    def split_args(self, o: int) -> list[tuple[int, int]]:
        return self.st.split_args(o)

    def field(self, owner: str | None, name: str) -> Binding | None:
        if (owner, name) in self.v.fields:
            return self.v.fields[owner, name]
        for base in self.v.lineage(owner):
            if (base, name) in self.v.fields:
                return self.v.fields[base, name]
        return None

    def call_scopes(self, pos: int) -> Iterable[str | tuple[str, ...]]:
        """Candidate class or namespace scopes for a named template call."""
        toks = self.toks
        if pos >= 2 and toks[pos - 1].text in ('.', '->'):
            obj = self.binding(pos - 2)
            owner = self.owner_at(pos) if toks[pos - 2].text == 'this' else (
                obj.type_name if obj else None)
            yield from self.v.lineage(owner)
            return
        suffix = []
        k = pos
        while k >= 2 and toks[k - 1].text == '::':
            ident = self.st.name_before(k - 1)
            if ident < 0 or toks[ident].kind != 'id' or toks[ident].text in KEYWORDS:
                break
            suffix.append(toks[ident].text)
            k = ident
        if suffix and suffix[0] in self.v.classes:
            yield from self.v.lineage(suffix[0])
            return
        ns = self.ff.namespace_at(pos)
        if k and toks[k - 1].text == '::':
            ns = ()  # leading ::
        elif not suffix:
            yield from self.v.lineage(self.owner_at(pos))
        for n in range(len(ns), -1, -1):
            yield ns[:n] + tuple(reversed(suffix))

    def binding(self, pos: int) -> Binding | None:
        if pos not in self._bindings:
            self._bindings[pos] = self._binding(pos)
        d = self._bindings[pos]
        if d in self.v.references:
            seen = set()
            while d in self.v.references and d not in seen:
                seen.add(d)
                d = self.v.references[d]
        return d

    def _binding(self, pos: int) -> Binding | None:
        toks = self.toks
        name = toks[pos].text
        if pos >= 2 and toks[pos - 1].text in ('.', '->'):
            parent = pos - 2
            if toks[parent].text == 'this':
                return self.field(self.owner_at(pos), name)
            if toks[parent].kind == 'id':
                receiver = self.binding(parent)
                return self.field(receiver.type_name, name) if receiver else None
            return None
        if pos >= 1 and toks[pos - 1].text == '::':
            k = pos
            parts = []
            while k >= 2 and toks[k - 1].text == '::':
                k = self.st.name_before(k - 1)
                if k < 0 or toks[k].kind != 'id' or toks[k].text in KEYWORDS:
                    break
                parts.append(toks[k].text)
            if parts and parts[0] in self.v.classes:
                return self.field(parts[0], name)
            return self.v.globals.get((tuple(reversed(parts)), name))
        for d in reversed(self.ff.locals.get(name, ())):
            if d.pos <= pos < d.end:
                return d
        member = self.field(self.owner_at(pos), name)
        if member is not None:
            return member
        ns = self.ff.namespace_at(pos)
        for n in range(len(ns), -1, -1):
            d = self.v.globals.get((ns[:n], name))
            if d is not None and (d.path != self.ff.path or d.pos <= pos):
                return d
        return None

    def record(self, storage: Binding | None) -> bool:
        """A function summary only exports effects on storage it does not own."""
        fn = self.summarizing
        if fn is not None:
            if storage and storage.path == self.ff.path and storage.automatic \
                    and fn.declarator < storage.pos < fn.body[1]:
                return False
            # At a call site we already account for the actual generator
            # arguments. Do not export the callee's formal in their place.
            if storage and storage.path == self.ff.path and not storage.automatic \
                    and fn.declarator < storage.pos < self.st.close[fn.declarator] \
                    and self.v.is_generator(storage):
                return False
            self.effects.add(storage)
        return True

    def generator(self, d: Binding | None, pos: int) -> bool:
        if not self.v.is_generator(d):
            return False
        root = pos
        while root >= 2 and self.toks[root - 1].text in ('.', '->'):
            root -= 2
        return self.record(self.binding(root) or d)

    def bare_object(self, lo: int, hi: int) -> int | None:
        """Identifier denoting the argument's storage, without evaluating a call."""
        toks = self.toks
        if lo < hi and toks[lo].text in ('*', '&'):
            lo += 1
        if lo + 4 < hi and [t.text for t in toks[lo:lo + 4]] == ['std', '::', 'ref', '('] \
                and self.st.close.get(lo + 3) == hi - 1:
            return self.bare_object(lo + 4, hi - 1)
        if lo == hi or toks[hi - 1].kind != 'id':
            return None
        k = lo
        while k + 2 < hi and toks[k].kind == 'id' and toks[k + 1].text in ('.', '->', '::'):
            k += 2
        return k if k == hi - 1 else None

    def bare_generator(self, lo: int, hi: int) -> bool:
        pos = self.bare_object(lo, hi)
        return pos is not None and self.generator(self.binding(pos), pos)

    def applies(self, key: DrawerKey) -> bool:
        effects = self.v.drawers.get(key, ())
        if self.summarizing is None:
            return bool(effects)
        return any([self.record(d) for d in effects])  # collect every possible effect

    def drawer(self, name: str, owners: set[str | None] | None) -> bool:
        if owners is None:
            keys = [key for key in self.v.drawers if key[1] == name and key[0] in self.v.classes]
        else:
            keys = [(owner, name) for owner in owners]
        return any([self.applies(key) for key in keys])

    def is_draw_call(self, o: int) -> bool:
        if o in self._draw_cache:
            return self._draw_cache[o]
        result = self._is_draw_call(o)
        self._draw_cache[o] = result
        return result

    def _is_draw_call(self, o: int) -> bool:
        toks = self.toks
        # A direct-initialized copy reads but does not advance the generator.
        if (o >= 2 and toks[o - 1].kind == 'id' and toks[o - 2].kind == 'id'
                and toks[o - 2].text not in KEYWORDS):
            return False
        arguments_draw = any([self.bare_generator(a, b) for a, b in self.split_args(o)])
        # Visit both sources of effects: a callee can receive a fresh engine
        # and also draw from a global, or draw from two different arguments.
        return self.callee_draws(o) or arguments_draw

    def callee_draws(self, o: int) -> bool:
        toks = self.toks
        v = self.v
        p = self.st.name_before(o)
        if p < 0:
            return False
        pt = toks[p]
        if pt.text == ')' and p in self.st.open_of:
            return self.bare_generator(self.st.open_of[p] + 1, p)
        if pt.kind != 'id':
            return False
        name = pt.text
        q = toks[p - 1] if p >= 1 else None
        if q is not None and q.text in ('.', '->'):
            member = self.binding(p)  # state.rng() or this->callable()
            if member is not None:
                return self.object_draws(member, p)
            obj = toks[p - 2] if p >= 2 else None
            if obj is None or obj.kind != 'id':
                return self.drawer(name, None)         # a call result's member: any class
            if obj.text == 'this':
                here = self.owner_at(o)
                return self.drawer(name, v.lineage(here)) or (
                    name in v.gen_methods.get(here, ()) and self.record(None))
            receiver = self.binding(p - 2)
            if receiver is not None:
                if v.is_generator(receiver):
                    return self.generator(receiver, p - 2)
                if receiver.type_name in v.classes:
                    return self.drawer(name, v.lineage(receiver.type_name))
            return self.drawer(name, None)
        if q is not None and q.text == '::':
            member = self.binding(p)
            if member is not None:
                return self.object_draws(member, p)
            idx = self.st.name_before(p - 1)
            if idx >= 0 and toks[idx].text in v.classes:
                return self.drawer(name, v.lineage(toks[idx].text))
            if p - 2 in self.st.template_close or (idx >= 0 and toks[idx].text == 'std'):
                return False    # an external type's static member is not a free function
            return self.drawer(name, {None})           # a namespace's free function
        if q is not None and ((q.kind == 'id' and q.text not in KEYWORDS)
                              or (q.text in ('&', '*', '&&', '>') and not self.st.operand_end(p - 2)
                                  and q.text != '>')
                              or (q.text == '>' and (p - 1) in self.st.template_close)):
            return False        # a declaration: Rng rng(seed), Type name(args)
        obj = self.binding(p)
        if obj is not None:
            return self.object_draws(obj, p)
        here = self.owner_at(o)
        owners = v.lineage(here) | {None}
        return self.drawer(name, owners) or (
            name in v.gen_methods.get(here, ()) and self.record(None))

    def object_draws(self, obj: Binding, pos: int) -> bool:
        if self.v.is_generator(obj):
            return self.generator(obj, pos)
        key = (obj.path, obj.pos)
        if key in self.v.lambda_names:
            return self.applies(key)
        return self.drawer('operator()', self.v.lineage(obj.type_name))

    def owner_at(self, idx: int) -> str | None:
        """The class whose member function (or body) holds token idx."""
        scope = self.st.parent[idx]
        while scope >= 0 and scope not in self.scopes:
            scope = self.st.parent[scope]
        return self.scopes.get(scope)

    def function_effects(self, fn: Function) -> set[Binding | None]:
        self.summarizing = fn
        self.effects.clear()
        self._draw_cache.clear()
        self.region_draws(*fn.body)
        return self.effects

    def region_draws(self, lo: int, hi: int) -> bool:
        """Find an evaluated draw. A summary includes nested statement blocks,
        but defining a nested function/lambda/class never executes its body."""
        found = False
        k = lo
        st = self.st
        while k < hi:
            t = self.toks[k]
            if t.text in ('(', '[', '{') and k in st.close:
                kind = st.kind.get(k)
                c = st.close[k]
                if kind == CALL and self.is_draw_call(k):
                    found = True
                    if self.summarizing is None:
                        return True
                if kind in (CAPTURE, PARAMS, NAME) or (kind == BLOCK and (
                        self.summarizing is None or k in st.body_params or k in st.class_heads)):
                    k = c + 1
                    continue
                if self.region_draws(k + 1, c):
                    found = True
                    if self.summarizing is None:
                        return True
                k = c + 1
                continue
            k += 1
        return found

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
                p = self.st.template_close[p] - 1
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
