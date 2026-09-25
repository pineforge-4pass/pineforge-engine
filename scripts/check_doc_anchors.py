#!/usr/bin/env python3
"""Check every ``file:line`` anchor the published documentation cites.

Why this exists
---------------
The pages cite the tree by line number.  Every header edit moves those lines,
so the citations rot silently: the second claimed-vs-actual audit found 108-115
of ~304 anchors wrong after a single campaign.  A reader who follows a stale
anchor lands on an unrelated declaration and concludes the page is lying.  The
only durable repair is a gate, so this checker runs as a ``ci_preflight``
source guard and ``--fix`` mechanically re-anchors what it can prove.

The anchor grammar the pages already use
----------------------------------------
An *anchor* is a path plus a line or line range.  Four spellings occur, all
accepted here:

1. bare file name           ``native_run_spec.hpp:562``
2. explicit path            ``src/native_execution_consumer.cpp:6670``
3. range                    ``engine.hpp:2395-2396``
4. continuation             ``:1487-1492`` - a bare ``:line`` that inherits the
                            path of the last full anchor in the same citation
                            list, even across a physical line break.
                            A continuation must start after a backtick, a space
                            or ``(`` so that clock times and URLs cannot match.

Any of the four may be wrapped in backticks as a whole
(```native_order.hpp:25```); that is a rendering choice, not a different
anchor. Unlabelled and output-labelled fences are skipped as pasted output.
Source-labelled fences are scanned: a code example's citation is a claim.

Path resolution
---------------
A path that contains a directory separator is taken as-is from the repository
root.  A bare file name is resolved against this search order, first hit wins::

    .                              (repository root: CMakeLists.txt, README.md)
    include/pineforge/             (the kernel / native API headers)
    include/pineforge/source/      (the Pine adapter headers)
    include/pineforge/compat/pine/ (the adapter's TradingView-quirk headers)
    src/
    src/source/
    src/compat/pine/
    scripts/
    examples/native/
    tests/
    runner/
    docs/  docs/pages/  docs/design/  docs/adr/   (a page citing a sibling page)

The repository root is searched first - the lane brief's order starts at
``include/pineforge/``, but a bare ``CMakeLists.txt`` must mean the top-level
one that defines ``pineforge_kernel``, not ``examples/native/CMakeLists.txt``.
The adapter, runner and docs directories are appended for the same reason: the
pages cite ``intraday_cap.hpp``, ``order_birth.cpp``, ``main.cpp`` and
``native-feature-parity.md`` by bare name, and a resolution gap would be
reported as NOFILE drift that is not drift.  First hit wins; the order is the
only tie-break, so ``market_admission.hpp`` means the source-layer one.

The symbol claim
----------------
When a backticked span precedes an anchor *in the same scope*, the page is
claiming that symbol lives at that line.  The scope ends at the nearest
preceding table-cell bar ``|``, blank line, list-item bullet, ``. `` or ``; ``,
so a Pine column never lends its name to a Native column's anchor and a
neighbouring sentence never lends its name across a full stop.

The claimed symbol is the *last path component* of that span, after stripping
an argument list, a brace initialiser or template arguments:

    ``NativeRunSpec::calculation``               -> calculation
    ``cancel_where(text, NativeRequestField::L)`` -> cancel_where
    ``native_toolkit::OrderBook<Key>``            -> OrderBook
    ``Reduce{OwnerOpenedUnits{}}``                -> Reduce

A span that is not identifier-shaped after that (``ticks * price_tick``, a
path, a command line) yields **no** symbol claim; the anchor is then only
checked for file and line existence.  Neither does a span rooted in a Pine
namespace (``strategy.risk.max_position_size``, ``request.security``): those
are the migration table's left column, spellings TradingView owns and the tree
never declares.  Only the *nearest* preceding span is consulted - falling back
to an earlier one would invent a claim the page did not make.

A continuation list (```sym` x.cpp:10, `:20`, `:30```) shares the claim that
heads it: when a continuation's nearest span is itself only an anchor (the
list's previous citation), the continuation takes its head anchor's claim.

Two elision conventions the pages already use are honoured rather than read
literally: a trailing ``*`` (``closed_trade_*``) matches any identifier with
that prefix, and a leading ``_`` (``… / _exit_id / _close_cause``, short for
``strategy_closed_trade_exit_id``) matches any identifier with that suffix.

A qualified span claims its scope as well: ``NativeRiskLimits::max_fills_per_day``
says the field is a member of ``NativeRiskLimits``, so where the cited file
declares ``NativeRiskLimits`` with braces (``struct`` / ``class`` / ``union`` /
``enum`` / ``namespace``) the occurrence must sit inside that brace scope.
``GroupEffect::Reduce`` cited at ``struct Reduce {`` names the wrong ``Reduce``
even though the word is on the line.  A qualifier the file declares no scope
for (a namespace alias, a type declared elsewhere) is not checked.

Ruling tables
-------------
ADR-0001's ruling sections and the design record's inventory, coupling and
native-only ruling sections (``RULING_SECTIONS``) are tables of *rulings*, and a
ruling's citation is the evidence for it.  An anchor in one of their table rows
must therefore carry a claim the gate can verify: a symbol, as everywhere, or -
only here - a backticked code fragment (``engine.current_bar_ = bar``,
``"pineforge-broker-state/v18"``), which must appear verbatim, whitespace
aside, in the cited window.  A row anchor with neither is ``NOCLAIM``: without a
claim, a citation that drifts onto unrelated code still passes, which is how
design rows CT4 and RP5 and ADR rows 492, 493 and 563 came to point at
unrelated lines while this gate said OK.

Verdicts
--------
``OK``        file exists, line exists, and either the claimed symbol appears
              in the cited window or no symbol was claimed.
``NOFILE``    the path resolves nowhere.
``NOLINE``    the line (or range end) is past EOF, or the range is inverted.
``SYMMISS``   the claimed symbol (or fragment) does not appear in the cited window.
``SCOPE``     the symbol is in the window, but outside the brace scope of the
              qualifier its span names.
``NOCLAIM``   a ruling-table anchor, or a symbol-less range anywhere, carries
              no verifiable claim.
``COMMENTONLY`` the complete cited C/C++ window is comment-only.

The cited window is exactly ``[first, last]``. A symbol on the next line is
outside the citation. Ranges without a symbol use a backticked content hash,
and the hash covers the cited source lines joined with ``\n``.

``--fix``
---------
Rewrites only the digits of a BAD anchor, and only when the file resolves and
the claimed symbol's occurrences in it are unambiguous.  Occurrences are
counted over the **code** of a C/C++ file - comments stripped - because a page
citing ``SYM`` at a line means where the code says ``SYM``, not a paragraph of
prose about it; if the symbol appears only in comments, the raw lines are used
instead.  Two mechanical rules then apply, in order:

1. the symbol occurs on exactly one line ``S`` (inside the qualifier's scope,
   when the span names one the file declares);
2. or exactly one occurrence declares a type: ``struct``/``class``/``enum``/
   ``enum class``/``union``/``namespace``/``using SYM =``.  A ``struct SYM``
   line *is* the declaration of ``SYM``; picking it is reading, not guessing.

Given that single line ``S``: a single-line anchor becomes ``S``, and an
``a-b`` range becomes ``S-(S+len-1)`` with the cited span length preserved,
provided that stays inside the file.  A symbol whose occurrence lines form
exactly one contiguous run of the cited span's length becomes that run.

Anything else (no symbol claimed, symbol absent, symbol on several equally
plausible lines, unresolvable file) is left alone and reported with every
candidate line: choosing among them is an editorial judgement, not a
mechanical one, and belongs to whoever rewrites the sentence.  A ``NOCLAIM``
anchor is never fixed: its missing claim is an edit to the sentence.

Exit status
-----------
0 only when no anchor is BAD on any scanned page.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: The published set.  These are the pages a reader is pointed at, and the only
#: files ``--fix`` may rewrite.
PAGE_GLOBS = (
    'README.md',
    'CONTRIBUTING.md',
    'docs/README.md',
    'docs/pages/*.md',
    'docs/adr/*.md',
    'docs/design/*.md',
)

SEARCH_DIRS = (
    '.',
    'include/pineforge',
    'include/pineforge/source',
    'include/pineforge/compat/pine',
    'src',
    'src/source',
    'src/compat/pine',
    'scripts',
    'examples/native',
    'tests',
    'runner',
    'docs',
    'docs/pages',
    'docs/design',
    'docs/adr',
)

CXX_SUFFIXES = {'.cpp', '.hpp', '.h', '.c'}

#: The ruling sections, by page: an anchor in a table row under one of these
#: headings (at any depth below it) must carry a claim.  Matched against the
#: heading text with its ``{#id}`` removed, as a prefix.
RULING_SECTIONS = {
    'docs/adr/0001-kernel-adapter-boundary.md': (
        'Residual TradingView-named surface',
        'Deprecated public spellings',
        'Kernel capabilities the Pine adapter does not declare',
        'TradingView-calibrated kernel mechanisms',
    ),
    'docs/design/native-feature-parity.md': (
        '1. Inventory',
        '2.ii Coupling extraction',
        '3.6 Kernel features the Pine adapter never declares',
        '3.7 Audit lane Q6',
    ),
}
HEADING_RE = re.compile(r'^(#{1,6})\s+(.*?)\s*(?:\{#[^}]*\})?\s*$')

SOURCE_SUFFIXES = ('cpp', 'hpp', 'h', 'c', 'py', 'md', 'txt', 'cmake', 'sh', 'yml', 'yaml', 'json')

_PATH = (r'(?:[A-Za-z0-9_][A-Za-z0-9_+.-]*/)*[A-Za-z0-9_][A-Za-z0-9_+.-]*'
         r'\.(?:' + '|'.join(SOURCE_SUFFIXES) + r')')
ANCHOR_RE = re.compile(r'(?P<path>' + _PATH + r'):(?P<a>\d+)(?:-(?P<b>\d+))?(?!\d)')
CONT_RE = re.compile(r'(?<=[ `(]):(?P<a>\d{1,6})(?:-(?P<b>\d{1,6}))?(?!\d)')
FENCE_RE = re.compile(r'^\s*(```|~~~)([^\n]*)$', re.MULTILINE)
SPAN_RE = re.compile(r'`([^`\n]+)`')
OUTPUT_FENCE_LABELS = frozenset({'text', 'console', 'output', 'terminal', 'log',
                                 'shell-session', 'ansi'})
HASH_RE = re.compile(r'(?i)^sha256:[0-9a-f]{64}$')
SCOPE_BREAKS = ('|', '\n\n', '. ', '; ', '\n- ', '\n* ', '\n> ', ':\n')
#: A backticked span that is nothing but an anchor: a citation, never a claim.
ANCHOR_ONLY_RE = re.compile(r'^\s*(?:' + _PATH + r')?:\d+(?:-\d+)?\s*$')


class Anchor:
    """One citation: where it is written, what it points at, what it claims."""

    def __init__(self, page: Path, start: int, end: int, doc_line: int,
                 path: str, first: int, last: int, digits: tuple[int, int],
                 symbol: str | None, span: str | None, mode: str = 'word') -> None:
        self.page = page
        self.start, self.end, self.doc_line = start, end, doc_line
        self.path, self.first, self.last = path, first, last
        self.digits = digits          # (offset, length) of the ``line`` or ``a-b`` text
        self.symbol, self.span, self.mode = symbol, span, mode
        self.qualifier = qualifier_of(span) if symbol and span else None
        self.content_hash = None
        if symbol is None and span:
            match = re.fullmatch(r'sha256:([0-9a-f]{64})', span.strip(), re.I)
            if match:
                self.content_hash = match.group(1).lower()
        self.orphan = False
        self.ruling = False           # set by collect(): a row of a ruling table
        self.fragment: str | None = None  # a ruling row's code-fragment claim
        self.verdict = 'OK'
        self.detail = ''
        self.suggestion: str | None = None
        self._fix = None

    @property
    def text(self) -> str:
        return f'{self.path + ":" if self.path else ":"}{self.first}' + (f'-{self.last}' if self.last != self.first else '')

    @property
    def length(self) -> int:
        return self.last - self.first + 1

    @property
    def bad(self) -> bool:
        return self.verdict != 'OK'


def fenced_spans(text: str) -> list[tuple[int, int]]:
    """Half-open ranges of pasted-output fences, not source-example fences."""
    spans, open_at, skip = [], None, False
    for match in FENCE_RE.finditer(text):
        if open_at is None:
            open_at = match.start()
            info = match.group(2).strip().split()
            skip = not info or info[0].lower() in OUTPUT_FENCE_LABELS
        else:
            if skip:
                end = text.find('\n', match.start())
                spans.append((open_at, len(text) if end < 0 else end + 1))
            open_at = None
            skip = False
    if open_at is not None and skip:
        spans.append((open_at, len(text)))
    return spans


#: Pine namespaces.  ``strategy.risk.max_position_size`` is the migration
#: table's left column - a TradingView spelling, never a symbol the tree
#: declares - so it lends its name to no anchor.
PINE_ROOTS = frozenset({'strategy', 'request', 'ta', 'math', 'syminfo', 'timeframe',
                        'input', 'array', 'matrix', 'map', 'str', 'color', 'label',
                        'line', 'box', 'table', 'barstate', 'session', 'chart',
                        'currency', 'dayofweek', 'plot', 'alert', 'runtime', 'std'})


def symbol_of(span: str) -> tuple[str | None, str]:
    """The identifier a backticked span claims, and how it must be matched.

    The mode is ``word`` normally; ``prefix`` for the pages' ``closed_trade_*``
    wildcard and ``suffix`` for their ``/ _exit_id / _close_cause`` elision of
    a shared prefix, so neither convention is read as a literal identifier.
    """
    text = span.strip()
    if '/' in text or re.search(r'\.(?:' + '|'.join(SOURCE_SUFFIXES) + r')\b', text):
        return None, 'word'                          # a path is not a symbol claim
    for cut in '({<[':
        index = text.find(cut)
        if index > 0:
            text = text[:index]
    text = text.strip().rstrip('&;,: ')
    mode = 'word'
    if text.endswith('*'):
        text, mode = text.rstrip('*'), 'prefix'
    text = text.rstrip('&;,: ')
    if not text:
        return None, 'word'
    parts = re.split(r'::|\.', text)
    # A Pine spelling is dotted (`ta.ema`, `strategy.risk.x`); a C++ qualified
    # name is not one (`ta::ATR` is pineforge::ta::ATR), except under `std::`.
    root = re.split(r'::|\.', text, maxsplit=1)[0]
    if len(parts) > 1 and (root == 'std' or ('.' in text and root in PINE_ROOTS)):
        return None, 'word'
    last = parts[-1]
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', last):
        return None, 'word'
    if mode == 'word' and last.startswith('_'):
        mode = 'suffix'
    return last, mode


def qualifier_of(span: str) -> str | None:
    """The scope a qualified span claims: ``NativeRiskLimits`` of
    ``NativeRiskLimits::max_fills_per_day``, ``Domain`` of
    ``exit_legs::Domain::Coof``; None for an unqualified span."""
    text = span.strip()
    for cut in '({<[':
        index = text.find(cut)
        if index > 0:
            text = text[:index]
    parts = [part for part in text.strip().rstrip('&;,:* ').split('::') if part]
    if len(parts) < 2 or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', parts[-2]):
        return None
    return parts[-2]


ANCHOR_SPAN_RE = re.compile(r'^\s*(?:' + _PATH + r')?:\d+(?:-\d+)?\s*$')


def fragment_of(span: str | None) -> str | None:
    """A ruling row's code fragment claim, or None when the span is no claim.

    A path, a bare ``:line`` continuation and a span with no identifier
    character claim nothing; anything else a ruling row puts in backticks
    before an anchor is a fragment of the cited code."""
    if not span:
        return None
    text = ' '.join(span.split())
    if HASH_RE.fullmatch(text):
        return None
    if not re.search(r'[A-Za-z_]', text) or ANCHOR_SPAN_RE.match(text):
        return None
    if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+(?:\(\))?', text) \
            and text.split('.')[0] in PINE_ROOTS:
        return None                                   # a Pine spelling: the table's other column
    if '/' in text and not re.search(r'[=*+<>"]', text):
        return None                                   # a path, not code
    if re.search(r'\.(?:' + '|'.join(SOURCE_SUFFIXES) + r')\b', text) and ' ' not in text:
        return None
    return text


def matcher(symbol: str, mode: str) -> re.Pattern[str]:
    escaped = re.escape(symbol)
    if mode == 'prefix':
        return re.compile(r'\b' + escaped + r'[A-Za-z0-9_]*')
    if mode == 'suffix':
        return re.compile(r'[A-Za-z0-9_]*' + escaped + r'\b')
    return re.compile(r'\b' + escaped + r'\b')


def scope_start(text: str, start: int) -> int:
    """Offset where the anchor's sentence or table cell begins."""
    window_start = max(0, start - 600)
    window = text[window_start:start]
    cut = 0
    for token in SCOPE_BREAKS:
        index = window.rfind(token)
        if index >= 0:
            cut = max(cut, index + len(token))
    return window_start + cut


def claimed_symbol(text: str, anchor_start: int) -> tuple[str | None, str | None, str]:
    """The nearest backticked span before the anchor, and the symbol it claims."""
    begin = scope_start(text, anchor_start)
    nearest = None
    for match in SPAN_RE.finditer(text, begin, anchor_start + 1):
        if match.end() <= anchor_start:
            nearest = match
    if nearest is None:
        return None, None, 'word'
    symbol, mode = symbol_of(nearest.group(1))
    return symbol, nearest.group(1), mode


def collect(page: Path, text: str) -> list[Anchor]:
    """Every anchor on the page, in source order, with its symbol claim."""
    skip = fenced_spans(text)

    def fenced(offset: int) -> bool:
        return any(lo <= offset < hi for lo, hi in skip)

    line_starts = [0] + [m.end() for m in re.finditer('\n', text)]

    def doc_line(offset: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    found: list[Anchor] = []
    taken: list[tuple[int, int]] = []
    for match in ANCHOR_RE.finditer(text):
        if fenced(match.start()):
            continue
        taken.append((match.start(), match.end()))
        first = int(match.group('a'))
        last = int(match.group('b') or match.group('a'))
        digit_start = match.start() + len(match.group('path')) + 1
        symbol, span, mode = claimed_symbol(text, match.start())
        immediate = re.search(r'`(sha256:[0-9a-f]{64})`\s*$', text[:match.start()], re.I)
        if immediate:
            symbol, span, mode = None, immediate.group(1), 'word'
        elif span and re.fullmatch(r'sha256:[0-9a-f]{64}', span.strip(), re.I):
            symbol, span, mode = None, None, 'word'
        found.append(Anchor(page, match.start(), match.end(), doc_line(match.start()),
                            match.group('path'), first, last,
                            (digit_start, match.end() - digit_start), symbol, span, mode))

    # Continuations inherit the last full anchor in the same paragraph or table
    # row. A physical wrap is harmless; a blank line, heading or next table row
    # is a boundary. A true orphan is a BAD anchor instead of silently ignored.
    full = list(found)
    for match in CONT_RE.finditer(text):
        if fenced(match.start()) or any(lo <= match.start() < hi for lo, hi in taken):
            continue
        line = doc_line(match.start())
        prior = [a for a in full if a.end <= match.start() and not re.search(
            r'\n\s*\n|\n\s*#{1,6}\s', text[a.end:match.start()])]
        prior.sort(key=lambda a: a.end)
        first = int(match.group('a'))
        last = int(match.group('b') or match.group('a'))
        symbol, span, mode = claimed_symbol(text, match.start())
        immediate = re.search(r'`(sha256:[0-9a-f]{64})`\s*$', text[:match.start()], re.I)
        if immediate:
            symbol, span, mode = None, immediate.group(1), 'word'
        elif span and re.fullmatch(r'sha256:[0-9a-f]{64}', span.strip(), re.I):
            symbol, span, mode = None, None, 'word'
        if prior and span is not None and ANCHOR_ONLY_RE.match(span):
            # "`sym` x.cpp:1730, `:1755`, `:7695`": the nearest span is the
            # list's previous citation, not a claim; the list shares its head's.
            symbol, span, mode = prior[-1].symbol, prior[-1].span, prior[-1].mode
        anchor = Anchor(page, match.start(), match.end(), line,
                        prior[-1].path if prior else '', first, last,
                        (match.start() + 1, match.end() - match.start() - 1),
                        symbol, span, mode)
        anchor.orphan = not bool(prior)
        found.append(anchor)
    found.sort(key=lambda a: a.start)
    ruling_lines = ruling_table_lines(page, text)
    for anchor in found:
        if anchor.doc_line in ruling_lines:
            anchor.ruling = True
            if anchor.symbol is None:
                anchor.fragment = fragment_of(anchor.span)
    return found


def ruling_table_lines(page: Path, text: str) -> set[int]:
    """1-based doc lines that are table rows inside one of the page's ruling sections."""
    prefixes = ()
    for rel, names in RULING_SECTIONS.items():
        if page.as_posix().endswith(rel):
            prefixes = names
    if not prefixes:
        return set()
    out: set[int] = set()
    stack: list[tuple[int, str]] = []
    fenced = False
    for number, line in enumerate(text.splitlines(), 1):
        if FENCE_RE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        heading = HEADING_RE.match(line)
        if heading:
            level = len(heading.group(1))
            while stack and stack[-1][0] >= level:
                stack.pop()
            stack.append((level, heading.group(2).strip()))
            continue
        if line.lstrip().startswith('|') and any(
                title.startswith(prefix) for _, title in stack for prefix in prefixes):
            out.add(number)
    return out


class Tree:
    """Resolved files and their lines, read once."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self._paths: dict[str, Path | None] = {}
        self._lines: dict[Path, list[str]] = {}
        self._code: dict[Path, list[str]] = {}

    def resolve(self, cited: str) -> Path | None:
        if cited not in self._paths:
            found = None
            if '/' in cited:
                candidate = self.root / cited
                found = candidate if candidate.is_file() else None
            else:
                for directory in SEARCH_DIRS:
                    candidate = self.root / directory / cited
                    if candidate.is_file():
                        found = candidate
                        break
            self._paths[cited] = found
        return self._paths[cited]

    def lines(self, path: Path) -> list[str]:
        if path not in self._lines:
            self._lines[path] = path.read_text(errors='replace').splitlines()
        return self._lines[path]

    def code_lines(self, path: Path) -> list[str]:
        """The file with C/C++ comments blanked out; other suffixes unchanged."""
        if path not in self._code:
            raw = self.lines(path)
            self._code[path] = strip_comments(raw) if path.suffix in CXX_SUFFIXES else raw
        return self._code[path]

    def scopes(self, path: Path, name: str) -> list[tuple[int, int]]:
        """Brace scopes (first line, last line) of every braced declaration of
        ``name`` in a C/C++ file; empty when the file declares none."""
        if path.suffix not in CXX_SUFFIXES:
            return []
        code = self.code_lines(path)
        decl = re.compile(r'\b(?:struct|class|union|namespace|enum(?:\s+class|\s+struct)?)\s+'
                          + re.escape(name) + r'\b(?![^;{]*;)')
        out: list[tuple[int, int]] = []
        for index, line in enumerate(code):
            if not decl.search(line):
                continue
            depth, opened = 0, False
            for end in range(index, len(code)):
                segment = code[end] if end != index else code[end][decl.search(line).start():]
                for char in segment:
                    if char == '{':
                        depth, opened = depth + 1, True
                    elif char == '}':
                        depth -= 1
                if opened and depth <= 0:
                    out.append((index + 1, end + 1))
                    break
                if not opened and ';' in segment:
                    break                              # a forward declaration
        return out


def strip_comments(lines: list[str]) -> list[str]:
    """Blank every ``//`` tail and ``/* */`` body, keeping the line numbering."""
    out, in_block = [], False
    for line in lines:
        kept, index = [], 0
        while index < len(line):
            if in_block:
                end = line.find('*/', index)
                if end < 0:
                    index = len(line)
                else:
                    in_block, index = False, end + 2
                continue
            start_block = line.find('/*', index)
            start_line = line.find('//', index)
            if start_line >= 0 and (start_block < 0 or start_line < start_block):
                kept.append(line[index:start_line])
                break
            if start_block >= 0:
                kept.append(line[index:start_block])
                in_block, index = True, start_block + 2
                continue
            kept.append(line[index:])
            break
        out.append(''.join(kept))
    return out


DECLARES = r'\b(?:struct|class|union|namespace|enum|enum\s+class)\s+{sym}\b|\busing\s+{sym}\s*='


def occurrences(lines: list[str], symbol: str, mode: str = 'word') -> list[int]:
    word = matcher(symbol, mode)
    return [n for n, line in enumerate(lines, 1) if word.search(line)]


def declarations(lines: list[str], symbol: str) -> list[int]:
    decl = re.compile(DECLARES.format(sym=re.escape(symbol)))
    return [n for n, line in enumerate(lines, 1) if decl.search(line)]


def runs(numbers: list[int]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for value in numbers:
        if out and value == out[-1][1] + 1:
            out[-1] = (out[-1][0], value)
        else:
            out.append((value, value))
    return out


def judge(anchor: Anchor, tree: Tree) -> None:
    if anchor.orphan:
        anchor.verdict, anchor.detail = 'ORPHAN', 'continuation has no full anchor in this paragraph or table row'
        return
    path = tree.resolve(anchor.path)
    if path is None:
        anchor.verdict, anchor.detail = 'NOFILE', 'no such file in the tree'
        return
    lines = tree.lines(path)
    if anchor.first < 1 or anchor.last < anchor.first or anchor.last > len(lines):
        anchor.verdict = 'NOLINE'
        anchor.detail = f'{path.relative_to(tree.root)} has {len(lines)} lines'
    if anchor.verdict == 'OK' and path.suffix in CXX_SUFFIXES and anchor.content_hash is None:
        code = tree.code_lines(path)
        if not any(line.strip() for line in code[anchor.first - 1:anchor.last]):
            anchor.verdict = 'COMMENTONLY'
            anchor.detail = f'lines {anchor.first}-{anchor.last} contain no C/C++ code'
            return
    if anchor.symbol is None:
        if anchor.content_hash is not None and anchor.verdict == 'OK':
            content = '\n'.join(lines[anchor.first - 1:anchor.last]).encode()
            actual = hashlib.sha256(content).hexdigest()
            if actual != anchor.content_hash:
                anchor.verdict, anchor.detail = 'HASHMISS', f'content hash is {actual}'
        elif anchor.fragment is not None:
            judge_fragment(anchor, tree, path)
        elif (anchor.length > 1 and anchor.span is None and anchor.verdict == 'OK'):
            anchor.verdict = 'NOCLAIM'
            anchor.detail = 'a symbol-less range needs a backticked symbol or sha256 content hash'
        elif anchor.ruling and anchor.verdict == 'OK':
            anchor.verdict = 'NOCLAIM'
            anchor.detail = ('a ruling-table anchor names nothing to verify: put the symbol '
                             '(or the code fragment) it cites in backticks before it')
        return                                     # no claim to check beyond existence
    all_hits = occurrences(lines, anchor.symbol, anchor.mode)
    scopes = tree.scopes(path, anchor.qualifier) if anchor.qualifier else []

    def scoped(numbers: list[int]) -> list[int]:
        return [n for n in numbers if any(a <= n <= b for a, b in scopes)] if scopes else numbers

    raw_hits = scoped(all_hits) or all_hits
    if anchor.verdict == 'OK':
        window = range(anchor.first, anchor.last + 1)
        if any(n in window for n in scoped(all_hits)):
            return
        stray = [n for n in all_hits if n in window]
        if scopes and stray:
            anchor.verdict = 'SCOPE'
            shown = ', '.join(f'{a}-{b}' for a, b in scopes)
            anchor.detail = (f'`{anchor.symbol}` at line {stray[0]} is outside '
                             f'`{anchor.qualifier}` (lines {shown})')
        else:
            anchor.verdict = 'SYMMISS'
            anchor.detail = f'`{anchor.symbol}` is not in lines {anchor.first}-{anchor.last}'
    if not raw_hits:
        anchor.suggestion = f'{anchor.path}:NONE  (`{anchor.symbol}` is nowhere in the file)'
        return
    # Where should it point?  Code occurrences only, when the symbol has any,
    # and only inside the qualifier's scope when the span names one.
    code = tree.code_lines(path)
    hits = occurrences(code, anchor.symbol, anchor.mode) or occurrences(lines, anchor.symbol,
                                                                         anchor.mode)
    hits = scoped(hits) or hits

    def aim(target: int) -> None:
        end = target + anchor.length - 1
        fixable = end <= len(lines)
        anchor.suggestion = (f'{anchor.path}:{target}'
                             + (f'-{end}' if anchor.length > 1 else '')
                             + ('' if fixable else '  (span would pass EOF; not fixed)'))
        anchor._fix = (target, end) if fixable else None

    if len(hits) == 1:
        aim(hits[0])
        return
    blocks = runs(hits)
    if len(blocks) == 1 and blocks[0][1] - blocks[0][0] + 1 == anchor.length:
        anchor.suggestion = f'{anchor.path}:{blocks[0][0]}-{blocks[0][1]}'
        anchor._fix = blocks[0]
        return
    declared = declarations(code, anchor.symbol)
    if len(declared) == 1:
        aim(declared[0])
        anchor.suggestion += f'  (the one declaration of `{anchor.symbol}`)'
        return
    nearest = min(hits, key=lambda n: abs(n - anchor.first))
    shown = ', '.join(str(n) for n in hits[:6]) + (', ...' if len(hits) > 6 else '')
    anchor.suggestion = (f'{anchor.path}:{nearest}?  (ambiguous: `{anchor.symbol}` '
                         f'on {len(hits)} lines: {shown}; not fixed)')


def judge_fragment(anchor: Anchor, tree: Tree, path: Path) -> None:
    """A ruling row's code fragment must appear, whitespace aside, in the window."""
    lines = tree.lines(path)
    if anchor.verdict != 'OK':
        return
    needle = ' '.join(anchor.fragment.split())

    def holds(first: int, last: int) -> bool:
        return needle in ' '.join(' '.join(lines[first - 1:last]).split())

    if holds(anchor.first, anchor.last):
        return
    anchor.verdict = 'SYMMISS'
    anchor.detail = f'`{needle}` is not in lines {anchor.first}-{anchor.last}'
    hits = [n for n in range(1, len(lines) + 1) if holds(n, n)]
    if len(hits) == 1:
        end = hits[0] + anchor.length - 1
        if end <= len(lines):
            anchor._fix = (hits[0], end)
            anchor.suggestion = (f'{anchor.path}:{hits[0]}'
                                 + (f'-{end}' if anchor.length > 1 else ''))
            return
    shown = ', '.join(str(n) for n in hits[:6]) + (', ...' if len(hits) > 6 else '')
    anchor.suggestion = (f'{anchor.path}:NONE  (the fragment is on no single line)' if not hits
                         else f'{anchor.path}:{hits[0]}?  (ambiguous: on {len(hits)} lines: '
                              f'{shown}; not fixed)')


def pages(root: Path, selected: list[str]) -> list[Path]:
    if selected:
        return [Path(name) if Path(name).is_absolute() else root / name for name in selected]
    out: list[Path] = []
    for glob in PAGE_GLOBS:
        out.extend(sorted(root.glob(glob)))
    return out


def apply_fixes(path: Path, text: str, fixed: list[Anchor]) -> str:
    for anchor in sorted(fixed, key=lambda a: a.digits[0], reverse=True):
        start, length = anchor.digits
        first, last = anchor._fix
        replacement = str(first) + (f'-{last}' if last != first else '')
        text = text[:start] + replacement + text[start + length:]
    path.write_text(text)
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Check the documentation\'s file:line anchors.')
    parser.add_argument('pages', nargs='*', help='pages to check (default: the published set)')
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--fix', action='store_true',
                        help='re-anchor the unambiguous line numbers in place')
    parser.add_argument('--list', action='store_true', dest='list_all',
                        help='dump every anchor with its verdict')
    args = parser.parse_args(argv)

    root = args.root.resolve()
    tree = Tree(root)
    totals = {'anchors': 0, 'ok': 0, 'bad': 0, 'nofile': 0, 'noline': 0}
    rows, bad_all, fixed_all = [], [], []

    for page in pages(root, args.pages):
        if not page.is_file():
            print(f'check_doc_anchors: no such page: {page}', file=sys.stderr)
            return 2
        text = page.read_text()
        anchors = collect(page, text)
        for anchor in anchors:
            judge(anchor, tree)
        bad = [a for a in anchors if a.bad]
        if args.fix:
            fixable = [a for a in bad if a._fix]
            if fixable:
                apply_fixes(page, text, fixable)
                fixed_all.extend(fixable)
                for anchor in fixable:
                    anchor.verdict, anchor.detail = 'FIXED', anchor.detail
                bad = [a for a in bad if not a._fix]
        rel = str(page.relative_to(root))
        counts = {'anchors': len(anchors), 'bad': len(bad),
                  'nofile': sum(1 for a in bad if a.verdict == 'NOFILE'),
                  'noline': sum(1 for a in bad if a.verdict == 'NOLINE')}
        counts['ok'] = counts['anchors'] - counts['bad']
        rows.append((rel, counts))
        for key in totals:
            totals[key] += counts[key]
        bad_all.extend(bad)
        if args.list_all:
            for anchor in anchors:
                claim = (f'`{anchor.symbol}`' if anchor.symbol
                         else f'`{anchor.fragment}` (fragment)' if anchor.fragment
                         else '(no symbol)')
                print(f'{rel}:{anchor.doc_line}  {anchor.verdict:<8} {anchor.text:<52} '
                      f'{claim:<34} {anchor.suggestion or ""}')

    width = max((len(r) for r, _ in rows), default=4)
    print(f'{"page":<{width}} {"anchors":>8} {"ok":>5} {"bad":>5} {"nofile":>7} {"noline":>7}')
    for rel, counts in rows:
        if counts['anchors']:
            print(f'{rel:<{width}} {counts["anchors"]:>8} {counts["ok"]:>5} {counts["bad"]:>5} '
                  f'{counts["nofile"]:>7} {counts["noline"]:>7}')
    print(f'{"TOTAL":<{width}} {totals["anchors"]:>8} {totals["ok"]:>5} {totals["bad"]:>5} '
          f'{totals["nofile"]:>7} {totals["noline"]:>7}')

    if fixed_all:
        print(f'\nre-anchored {len(fixed_all)}:')
        for anchor in fixed_all:
            print(f'  {anchor.page.relative_to(root)}:{anchor.doc_line}  {anchor.text} '
                  f'-> {anchor.path}:{anchor._fix[0]}'
                  + (f'-{anchor._fix[1]}' if anchor._fix[1] != anchor._fix[0] else ''))

    if bad_all:
        print(f'\n{len(bad_all)} bad anchors:')
        for anchor in bad_all:
            print(f'  {anchor.page.relative_to(root)}:{anchor.doc_line}  {anchor.text}  '
                  f'[{anchor.verdict}] {anchor.detail}'
                  + (f'  -> {anchor.suggestion}' if anchor.suggestion else ''))
        return 1
    print('\nevery documentation anchor resolves to its symbol')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
