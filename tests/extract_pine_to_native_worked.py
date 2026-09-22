#!/usr/bin/env python3
"""Extract the worked migration of docs/pages/pine-to-native.md for its CTest row.

The page's section "A worked migration, end to end" ports one Pine strategy in
six C++ blocks and says everything the port needs is on the page. The row
``test_pine_to_native_worked`` holds it to that: this script copies the blocks
out of the page VERBATIM at build time, ``tests/test_pine_to_native_worked.cpp``
compiles them against ``PineForge::kernel`` and runs them, and the row fails
when the page's code does not configure, does not trade, or does not charge
what the page's own Pine declaration says.

What is taken from the page, and how it is found:

* the one ```pine block: the ``strategy()`` arguments the row's expectations
  are computed from (``default_qty_value``, ``commission_value`` and the
  ``strategy.exit`` distances) -- so a page whose Pine and C++ disagree fails;
* the six ```cpp blocks, each claimed by the ``### <n>.`` heading above it:
  steps 1 and 2 build ``spec``, step 3 is ``on_native_run_begin``, step 6 is
  ``on_native_bar``, and steps 4 and 5 (the entry and the bracket) replace the
  ``/* the entry and the three legs above */`` placeholder of step 6, which is
  where the page says they run.

Nothing is retyped. Every block is preceded by a ``#line`` directive naming
its line on the page, so a compile error in the page's code is reported
against ``docs/pages/pine-to-native.md``, not against a generated file.

Anything the script cannot find exactly once -- the section, a step's block,
the placeholder, a Pine argument -- is an error (exit 1): a page that moved
its example must not leave the row compiling something else.

Outputs, all in ``--out-dir``:

* ``pine_to_native_worked_page.hpp`` -- the page's path, byte length and
  FNV-1a-64 digest (the binary re-reads the page and refuses to run stale),
  and the Pine arguments;
* ``pine_to_native_worked_spec.inc`` -- steps 1 and 2, statements;
* ``pine_to_native_worked_members.inc`` -- steps 3 and 6 (4 and 5 spliced in),
  member function definitions.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SECTION = '## A worked migration, end to end'
PLACEHOLDER = re.compile(r'/\*\s*the entry and the three legs above\s*\*/')
STEP_HEADING = re.compile(r'^###\s+([0-9]+)\.\s')
FENCE_OPEN = re.compile(r'^```([A-Za-z0-9_+-]*)\s*$')
PINE_NUMBERS = {
    # strategy(): the cash the entry sizes with, and the commission percent.
    'default_qty_value': 'PF_P2N_PINE_CASH',
    'commission_value': 'PF_P2N_PINE_COMMISSION_PERCENT',
    'initial_capital': 'PF_P2N_PINE_INITIAL_CAPITAL',
    # strategy.exit(): the three distances, in ticks.
    'profit': 'PF_P2N_PINE_PROFIT_TICKS',
    'loss': 'PF_P2N_PINE_LOSS_TICKS',
    'trail_points': 'PF_P2N_PINE_TRAIL_POINTS_TICKS',
    'trail_offset': 'PF_P2N_PINE_TRAIL_OFFSET_TICKS',
}
PINE_WORDS = {
    'default_qty_type': 'strategy.cash',
    'commission_type': 'strategy.commission.percent',
}


class ExtractError(Exception):
    pass


def fnv1a64(data: bytes) -> int:
    value = 0xcbf29ce484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return value


def section_lines(lines: list[str]) -> tuple[int, int]:
    """0-based [start, end) of the worked-migration section."""
    starts = [i for i, line in enumerate(lines) if line.startswith(SECTION)]
    if len(starts) != 1:
        raise ExtractError(f'expected exactly one "{SECTION}" heading, found {len(starts)}')
    start = starts[0]
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].startswith('## ')), len(lines))
    return start, end


def fenced_blocks(lines: list[str], start: int, end: int):
    """(language, 1-based first body line, body lines, owning step or None)."""
    blocks, step, i = [], None, start
    while i < end:
        heading = STEP_HEADING.match(lines[i])
        if heading:
            step = int(heading.group(1))
        opened = FENCE_OPEN.match(lines[i])
        if opened:
            body, j = [], i + 1
            while j < end and not lines[j].startswith('```'):
                body.append(lines[j])
                j += 1
            if j >= end:
                raise ExtractError(f'unterminated fence opened at page line {i + 1}')
            blocks.append((opened.group(1), i + 2, body, step))
            i = j + 1
            continue
        i += 1
    return blocks


def pine_arguments(body: list[str]) -> dict[str, str]:
    text = '\n'.join(body)
    found: dict[str, str] = {}
    for name in list(PINE_NUMBERS) + list(PINE_WORDS):
        hits = re.findall(r'\b' + re.escape(name) + r'\s*=\s*([A-Za-z0-9_.]+)', text)
        if len(hits) != 1:
            raise ExtractError(f'the Pine block must set `{name}` exactly once, found {len(hits)}')
        found[name] = hits[0]
    for name, value in PINE_NUMBERS.items():
        if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)?', found[name]):
            raise ExtractError(f'the Pine argument `{name}` is not a plain number: {found[name]}')
    for name, word in PINE_WORDS.items():
        if found[name] != word:
            raise ExtractError(f'the row assumes `{name} = {word}`; the page says {found[name]}')
    return found


def line_directive(number: int, page_label: str) -> str:
    return f'#line {number} "{page_label}"'


def extract(page: Path, page_label: str) -> dict[str, str]:
    raw = page.read_bytes()
    lines = raw.decode('utf-8').splitlines()
    start, end = section_lines(lines)
    blocks = fenced_blocks(lines, start, end)

    pine = [b for b in blocks if b[0] == 'pine']
    if len(pine) != 1:
        raise ExtractError(f'expected exactly one ```pine block in the section, found {len(pine)}')
    arguments = pine_arguments(pine[0][2])

    steps: dict[int, tuple[int, list[str]]] = {}
    for language, first, body, step in blocks:
        if language != 'cpp':
            continue
        if step is None:
            raise ExtractError(f'a ```cpp block at page line {first - 1} sits under no "### <n>." step')
        if step in steps:
            raise ExtractError(f'step {step} has more than one ```cpp block (page line {first - 1})')
        steps[step] = (first, body)
    missing = [n for n in range(1, 7) if n not in steps]
    extra = sorted(n for n in steps if n not in range(1, 7))
    if missing or extra:
        raise ExtractError(f'expected one ```cpp block for each of steps 1-6; '
                           f'missing {missing or "none"}, unexpected {extra or "none"}')

    def block(n: int) -> str:
        first, body = steps[n]
        return line_directive(first, page_label) + '\n' + '\n'.join(body) + '\n'

    first6, body6 = steps[6]
    hits = [(k, m) for k, line in enumerate(body6) for m in PLACEHOLDER.finditer(line)]
    if len(hits) != 1:
        raise ExtractError('step 6 must carry the placeholder '
                           '"/* the entry and the three legs above */" exactly once, '
                           f'found {len(hits)}')
    k, match = hits[0]
    before = body6[:k] + [body6[k][:match.start()]]
    after = [body6[k][match.end():]] + body6[k + 1:]
    member6 = (line_directive(first6, page_label) + '\n' + '\n'.join(before) + '\n'
               + block(4) + block(5)
               + line_directive(first6 + k, page_label) + '\n' + '\n'.join(after) + '\n')

    banner = ('// GENERATED by tests/extract_pine_to_native_worked.py from '
              f'{page_label} -- do not edit.\n')
    header = [banner, '#pragma once', '',
              f'#define PF_P2N_PAGE_PATH "{page.resolve().as_posix()}"',
              f'#define PF_P2N_PAGE_BYTES {len(raw)}ULL',
              f'#define PF_P2N_PAGE_FNV1A64 0x{fnv1a64(raw):016x}ULL',
              '// The page\'s Pine declaration, which the six blocks translate.']
    for name, macro in PINE_NUMBERS.items():
        header.append(f'#define {macro} {float(arguments[name])!r}  // {name} = {arguments[name]}')
    header.append('')
    return {
        'pine_to_native_worked_page.hpp': '\n'.join(header),
        'pine_to_native_worked_spec.inc': banner + block(1) + block(2),
        'pine_to_native_worked_members.inc': banner + block(3) + member6,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--page', type=Path, required=True)
    parser.add_argument('--page-label', default='docs/pages/pine-to-native.md',
                        help='the name #line directives report the page under')
    parser.add_argument('--out-dir', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        outputs = extract(args.page, args.page_label)
    except (ExtractError, OSError, UnicodeDecodeError) as error:
        print(f'extract_pine_to_native_worked: {args.page}: {error}', file=sys.stderr)
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for name, text in outputs.items():
        (args.out_dir / name).write_text(text)
    print(f'extract_pine_to_native_worked: {len(outputs)} files from {args.page_label} '
          f'into {args.out_dir}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
