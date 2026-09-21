#!/usr/bin/env python3
"""Hold the migration page's coverage table against the repository's own inventory.

Why this exists
---------------
``docs/pages/pine-to-native.md`` claims that every ``strategy.*`` builtin,
every ``strategy()`` declaration parameter, every ``request.*`` form and every
``barmerge.*`` constant has a row.  Both independent R5 audits measured that
claim and found it false — 43 of 96 ``strategy.*`` names had no row — so the
claim needs a gate, not a promise.  This script derives both sides of the
table and fails when they disagree:

*offered*
    The ``strategy.*``, ``request.*`` and ``barmerge.*`` rows of the
    repository's own Pine v6 inventory, ``docs/pine_v6_coverage_detail.md``,
    parsed out of its tables' first column — plus the named arguments of the
    ``strategy()`` call, which that inventory collapses into a single row and
    which are therefore listed here (``DECLARATION_PARAMETERS``).

*covered*
    The first column of every table row on the migration page whose cell is
    exactly one backticked token.  That is the page's own Pine column, so the
    page cannot claim coverage it does not print.

A token whose row says ``none`` in the C++ column is counted as covered and
reported separately: a ruled *none* is a decision with a reason, not a gap.
The page prints the same three numbers, so a reader can recount them here.

Exit status
-----------
0 when offered == covered and the page's printed totals match what is counted.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INVENTORY = 'docs/pine_v6_coverage_detail.md'
PAGE = 'docs/pages/pine-to-native.md'

#: The namespaces whose inventory rows this page must map.
NAMESPACES = ('strategy', 'request', 'barmerge')

#: The named arguments of the ``strategy()`` declaration.  The inventory has
#: one row for the whole call (``strategy()``), so its parameters are spelled
#: here.  This is the Pine v6 declaration surface a migrating author meets;
#: adding a parameter to it without adding a row to the page fails this check.
DECLARATION_PARAMETERS = (
    'pyramiding',
    'calc_on_order_fills',
    'calc_on_every_tick',
    'process_orders_on_close',
    'close_entries_rule',
    'default_qty_type',
    'default_qty_value',
    'initial_capital',
    'currency',
    'commission_type',
    'commission_value',
    'slippage',
    'margin_long',
    'margin_short',
    'backtest_fill_limits_assumption',
    'use_bar_magnifier',
    'fill_orders_on_standard_ohlc',
    'max_bars_back',
    'risk_free_rate',
)

INVENTORY_ROW = re.compile(r'^\|\s*`([A-Za-z_][A-Za-z0-9_.]*(?:\(\))?)`\s*\|', re.M)
#: A covered row: first cell exactly one backticked token, and at least one
#: further cell after it.  The C++ column is the second cell.
PAGE_ROW = re.compile(r'^\|\s*`([^`|]+)`\s*\|([^|]*)\|', re.M)
FENCE = re.compile(r'^\s*(```|~~~)')


def outside_fences(text: str) -> str:
    """The document with fenced code blocks removed: those are not tables."""
    kept, fenced = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        kept.append('' if fenced else line)
    return '\n'.join(kept)


def offered(root: Path) -> list[str]:
    text = outside_fences((root / INVENTORY).read_text())
    names = set()
    for token in INVENTORY_ROW.findall(text):
        if token.split('.')[0].removesuffix('()') in NAMESPACES:
            names.add(token)
    names.update(DECLARATION_PARAMETERS)
    return sorted(names)


def covered(root: Path) -> dict[str, str]:
    """Every Pine token the page maps, with its C++ cell."""
    text = outside_fences((root / PAGE).read_text())
    rows: dict[str, str] = {}
    for token, native in PAGE_ROW.findall(text):
        token = token.strip()
        if token in rows:
            continue
        rows[token] = native.strip()
    return rows


def ruled_none(native_cell: str) -> bool:
    """A row whose native counterpart is a ruling rather than a symbol."""
    return native_cell.startswith('none')


def main() -> int:
    parser = argparse.ArgumentParser(description='Recount the migration page coverage.')
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--list', action='store_true',
                        help='print every offered token with its verdict')
    args = parser.parse_args()
    root = args.root.resolve()

    want = offered(root)
    have = covered(root)

    missing = [token for token in want if token not in have]
    # A page row whose first cell is a Pine spelling this inventory does not
    # offer is not an error by itself (the page also tabulates concepts Pine
    # has no word for), so only the offered direction is enforced.
    mapped = [token for token in want if token in have]
    none_rows = [token for token in mapped if ruled_none(have[token])]

    if args.list:
        for token in want:
            if token not in have:
                print(f'MISSING  {token}')
            elif ruled_none(have[token]):
                print(f'RULED    {token}  -> {have[token]}')
            else:
                print(f'NATIVE   {token}  -> {have[token]}')

    print(f'offered={len(want)} covered={len(mapped)} '
          f'native={len(mapped) - len(none_rows)} ruled-none={len(none_rows)}')

    if missing:
        print(f'\n{len(missing)} offered Pine names have no row in {PAGE}:', file=sys.stderr)
        for token in missing:
            print(f'  {token}', file=sys.stderr)
        print('\nAdd one row each, or the page\'s coverage claim is false.', file=sys.stderr)
        return 1

    # The page prints the totals; they must be the counted ones.
    page_text = (root / PAGE).read_text()
    totals = re.search(r'\|\s*\*\*total\*\*\s*\|\s*\*\*(\d+)\*\*\s*\|\s*\*\*(\d+)\*\*\s*\|'
                       r'\s*\*\*(\d+)\*\*\s*\|\s*\*\*(\d+)\*\*\s*\|', page_text)
    if not totals:
        print(f'{PAGE} no longer prints a coverage total row', file=sys.stderr)
        return 1
    printed = tuple(int(value) for value in totals.groups())
    counted = (len(want), len(mapped), len(mapped) - len(none_rows), len(none_rows))
    if printed != counted:
        print(f'{PAGE} prints totals {printed}, this check counts {counted}', file=sys.stderr)
        return 1

    print('every offered Pine name has a row, and the page prints the counted totals')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
