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
                            path of the last full anchor **earlier on the same
                            physical line** (```chart_day_key` :12052-12077``).
                            A continuation must start after a backtick, a space
                            or ``(`` so that clock times and URLs cannot match.

Any of the four may be wrapped in backticks as a whole
(```native_order.hpp:25```); that is a rendering choice, not a different
anchor.  Anchors inside fenced code blocks are **skipped**: those are pasted
tool output and compiler diagnostics, not citations the page is making.

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

Two elision conventions the pages already use are honoured rather than read
literally: a trailing ``*`` (``closed_trade_*``) matches any identifier with
that prefix, and a leading ``_`` (``… / _exit_id / _close_cause``, short for
``strategy_closed_trade_exit_id``) matches any identifier with that suffix.

Verdicts
--------
``OK``        file exists, line exists, and either the claimed symbol appears
              in the cited window or no symbol was claimed.
``NOFILE``    the path resolves nowhere.
``NOLINE``    the line (or range end) is past EOF, or the range is inverted.
``SYMMISS``   the claimed symbol does not appear in the cited window.

The cited window is ``[first, last + 1]`` - one line of slack after the range,
for a declaration whose name wraps onto the following line.

``--fix``
---------
Rewrites only the digits of a BAD anchor, and only when the file resolves and
the claimed symbol's occurrences in it are unambiguous.  Occurrences are
counted over the **code** of a C/C++ file - comments stripped - because a page
citing ``SYM`` at a line means where the code says ``SYM``, not a paragraph of
prose about it; if the symbol appears only in comments, the raw lines are used
instead.  Two mechanical rules then apply, in order:

1. the symbol occurs on exactly one line ``S``;
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
mechanical one, and belongs to whoever rewrites the sentence.

Exit status
-----------
0 only when no anchor is BAD on any scanned page.
"""
from __future__ import annotations

import argparse
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

SOURCE_SUFFIXES = ('cpp', 'hpp', 'h', 'c', 'py', 'md', 'txt', 'cmake', 'sh', 'yml', 'yaml', 'json')

_PATH = (r'(?:[A-Za-z0-9_][A-Za-z0-9_+.-]*/)*[A-Za-z0-9_][A-Za-z0-9_+.-]*'
         r'\.(?:' + '|'.join(SOURCE_SUFFIXES) + r')')
ANCHOR_RE = re.compile(r'(?P<path>' + _PATH + r'):(?P<a>\d+)(?:-(?P<b>\d+))?(?!\d)')
CONT_RE = re.compile(r'(?<=[ `(]):(?P<a>\d+)(?:-(?P<b>\d+))?(?!\d)')
FENCE_RE = re.compile(r'^\s*(```|~~~)', re.MULTILINE)
SPAN_RE = re.compile(r'`([^`\n]+)`')
SCOPE_BREAKS = ('|', '\n\n', '. ', '; ', '\n- ', '\n* ', '\n> ', ':\n')


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
        self.verdict = 'OK'
        self.detail = ''
        self.suggestion: str | None = None

    @property
    def text(self) -> str:
        return f'{self.path}:{self.first}' + (f'-{self.last}' if self.last != self.first else '')

    @property
    def length(self) -> int:
        return self.last - self.first + 1

    @property
    def bad(self) -> bool:
        return self.verdict != 'OK'


def fenced_spans(text: str) -> list[tuple[int, int]]:
    """Half-open offset ranges of fenced code blocks, which are not citations."""
    spans, open_at = [], None
    for match in FENCE_RE.finditer(text):
        if open_at is None:
            open_at = match.start()
        else:
            spans.append((open_at, text.find('\n', match.start()) + 1 or len(text)))
            open_at = None
    if open_at is not None:
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
    if len(parts) > 1 and parts[0] in PINE_ROOTS:
        return None, 'word'
    last = parts[-1]
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', last):
        return None, 'word'
    if mode == 'word' and last.startswith('_'):
        mode = 'suffix'
    return last, mode


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
        found.append(Anchor(page, match.start(), match.end(), doc_line(match.start()),
                            match.group('path'), first, last,
                            (digit_start, match.end() - digit_start), symbol, span, mode))

    # Continuations inherit the last full anchor's path from the same physical line.
    for match in CONT_RE.finditer(text):
        if fenced(match.start()) or any(lo <= match.start() < hi for lo, hi in taken):
            continue
        line = doc_line(match.start())
        prior = [a for a in found if a.doc_line == line and a.end <= match.start()]
        if not prior:
            continue
        first = int(match.group('a'))
        last = int(match.group('b') or match.group('a'))
        symbol, span, mode = claimed_symbol(text, match.start())
        found.append(Anchor(page, match.start(), match.end(), line,
                            prior[-1].path, first, last,
                            (match.start() + 1, match.end() - match.start() - 1),
                            symbol, span, mode))
    found.sort(key=lambda a: a.start)
    return found


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
    path = tree.resolve(anchor.path)
    if path is None:
        anchor.verdict, anchor.detail = 'NOFILE', 'no such file in the tree'
        return
    lines = tree.lines(path)
    if anchor.first < 1 or anchor.last < anchor.first or anchor.last > len(lines):
        anchor.verdict = 'NOLINE'
        anchor.detail = f'{path.relative_to(tree.root)} has {len(lines)} lines'
    if anchor.symbol is None:
        return                                     # no claim to check beyond existence
    raw_hits = occurrences(lines, anchor.symbol, anchor.mode)
    if anchor.verdict == 'OK':
        window = range(anchor.first, min(anchor.last + 1, len(lines)) + 1)
        if any(n in window for n in raw_hits):
            return
        anchor.verdict = 'SYMMISS'
        anchor.detail = f'`{anchor.symbol}` is not in lines {anchor.first}-{anchor.last + 1}'
    if not raw_hits:
        anchor.suggestion = f'{anchor.path}:NONE  (`{anchor.symbol}` is nowhere in the file)'
        return
    # Where should it point?  Code occurrences only, when the symbol has any.
    code = tree.code_lines(path)
    hits = occurrences(code, anchor.symbol, anchor.mode) or raw_hits

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
            anchor._fix = None
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
                claim = f'`{anchor.symbol}`' if anchor.symbol else '(no symbol)'
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
