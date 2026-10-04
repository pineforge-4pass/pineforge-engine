#!/usr/bin/env python3
"""Regenerate cases.tsv from the per-probe plan.json files.

Usage: python3 build_cases.py [--check]

Every tape listed in provenance.json becomes one row, read with its probe's
<name>/plan.json; the sha256 of its tv_trades.csv, of the probe's
strategy.pine and of every bars/ file are first verified against
provenance.json. Numbers are written with repr() so the C++ reader's strtod
gets the identical double. --check exits 1 when the committed cases.tsv
differs from the regenerated one instead of rewriting it.
"""

import datetime
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent
COLUMNS = ['name', 'tape', 'bars', 'symbol', 'timeframe', 'tick', 'step', 'capital', 'margin_long',
           'margin_short', 'after_close', 'events']
OPS = ('entry', 'close', 'close_all')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def day_ms(value):
    stamp = datetime.datetime.fromisoformat(value).replace(tzinfo=datetime.timezone.utc)
    return int(stamp.timestamp() * 1000)


def feed(plan):
    # The chart's feed: the daily NYSE:F feed for the 1D probes, else the
    # symbol's 15-minute feed.
    if plan.get('feed'):
        return plan['feed']
    if plan['timeframe'] == '1D':
        return 'f-roi'
    return {'NASDAQ:AAPL': 'aapl', 'BINANCE:ETHUSDT.P': 'eth-proozac'}.get(plan['symbol'], 'f-antoni')


def bars_file(plan):
    return f"{feed(plan)}-{plan['from']}-{plan['to']}.csv"


def event_cell(name, event):
    # time:op:id:side:qty:boundary -- side 1 long, -1 short, 0 for a close;
    # qty empty for the default (100 % of equity); boundary 0/1.
    op = event['op']
    if op not in OPS:
        raise ValueError(f'{name}: op {op!r}')
    identifier = event.get('id') or ''
    if op == 'close_all' and identifier:
        raise ValueError(f'{name}: close_all with id {identifier!r}')
    if op != 'close_all' and not identifier:
        raise ValueError(f'{name}: {op} without id')
    if any(mark in identifier for mark in ':;\t\n'):
        raise ValueError(f'{name}: id {identifier!r} holds a separator')
    side = event.get('side') if op == 'entry' else 0
    if op == 'entry' and side not in (1, -1):
        raise ValueError(f'{name}: side {side!r}')
    quantity = event.get('qty') if op == 'entry' else None
    return ':'.join([str(int(event['time'])), op, identifier, str(side),
                     '' if quantity is None else repr(float(quantity)),
                     str(int(bool(event.get('boundary'))))])


def row(tape, plan):
    name = plan['name']
    if plan['timeframe'] not in ('15', '1D'):
        raise ValueError(f'{name}: timeframe {plan["timeframe"]!r}')
    return [name, tape['path'] + '/tv_trades.csv', bars_file(plan), plan['symbol'], plan['timeframe'],
            repr(plan['tick']), repr(plan['step']), repr(plan['capital']), repr(plan['margin_long']),
            repr(plan['margin_short']), str(int(bool(plan['after_close']))),
            ';'.join(event_cell(name, event) for event in plan['events'])]


def bar_times(name):
    lines = (ROOT / 'bars' / name).read_text().splitlines()
    return {int(line.split(',')[0]) for line in lines[1:]}


def build():
    provenance = json.loads((ROOT / 'provenance.json').read_text())
    errors = []
    for name, expected in provenance['bars'].items():
        if sha256(ROOT / 'bars' / name) != expected['sha256']:
            errors.append(f'bars/{name}: sha256 differs from provenance.json')
    plans = {}
    for entry in provenance['plans']:
        directory = ROOT / entry['name']
        if sha256(directory / 'strategy.pine') != entry['strategy_pine_sha256']:
            errors.append(f"{entry['name']}/strategy.pine: sha256 differs from provenance.json")
        plan = json.loads((directory / 'plan.json').read_text())
        if plan['name'] != entry['name']:
            errors.append(f"{entry['name']}/plan.json names {plan['name']!r}")
        plans[entry['name']] = plan
    times = {}
    lines = ['\t'.join(COLUMNS)]
    tv_rows = 0
    for tape in sorted(provenance['tapes'], key=lambda item: item['path']):
        name = tape['path'].split('/')[0]
        if name not in plans:
            errors.append(f"{tape['path']}: no plan {name!r} in provenance.json")
            continue
        if sha256(ROOT / tape['path'] / 'tv_trades.csv') != tape['tv_trades_sha256']:
            errors.append(f"{tape['path']}/tv_trades.csv: sha256 differs from provenance.json")
        tv_rows += tape['tv_rows']
        plan = plans[name]
        cells = row(tape, plan)
        if cells[2] not in provenance['bars']:
            errors.append(f"{tape['path']}: bars/{cells[2]} not listed in provenance.json")
            continue
        # The scripts fire an event on the bar opening at its time: every
        # event time must be a bar of the window.
        if cells[2] not in times:
            times[cells[2]] = bar_times(cells[2])
        start, end = day_ms(plan['from']), day_ms(plan['to'])
        for event in plan['events']:
            if event['time'] not in times[cells[2]] or not start <= event['time'] <= end:
                errors.append(f"{tape['path']}: event time {event['time']} is not a bar of bars/{cells[2]}")
        if any('\t' in cell or '\n' in cell for cell in cells):
            errors.append(f"{tape['path']}: a cell holds a tab or newline")
        lines.append('\t'.join(cells))
    if len(lines) - 1 != provenance['tapes_total']:
        errors.append(f"{len(lines) - 1} tapes, provenance.json says {provenance['tapes_total']}")
    if tv_rows != provenance['tv_rows_total']:
        errors.append(f"{tv_rows} TV rows, provenance.json says {provenance['tv_rows_total']}")
    if len(plans) != provenance['plans_total']:
        errors.append(f"{len(plans)} plans, provenance.json says {provenance['plans_total']}")
    return '\n'.join(lines) + '\n', errors


def main():
    text, errors = build()
    for error in errors:
        print('ERROR', error, file=sys.stderr)
    if errors:
        return 1
    target = ROOT / 'cases.tsv'
    if '--check' in sys.argv[1:]:
        if not target.exists() or target.read_text() != text:
            print('cases.tsv is stale: run python3 build_cases.py', file=sys.stderr)
            return 1
        print(f'cases.tsv up to date ({text.count(chr(10)) - 1} tapes)')
        return 0
    target.write_text(text)
    print(f'wrote cases.tsv ({text.count(chr(10)) - 1} tapes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
