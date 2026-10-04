#!/usr/bin/env python3
"""Regenerate cases.tsv from the per-tape parameters.json files.

Usage: python3 build_cases.py [--check]

Every tape listed in provenance.json is read from <path>/parameters.json; the
sha256 of its tv_trades.csv and strategy.pine and of every bars/ file are first
verified against provenance.json. A tape whose provenance entry says
"asserted": false is written with asserted=0 (replayed and reported, not
checked); every other tape with asserted=1. Numbers are written with repr() so the C++
reader's strtod gets the identical double. --check exits 1 when the committed
cases.tsv differs from the regenerated one instead of rewriting it.
"""

import datetime
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent
COLUMNS = ['path', 'bars', 'tick', 'step', 'capital', 'direction', 'qty', 'slippage', 'fee', 'fee_type',
           'margin', 'pooc', 'coof', 'entry_ms', 'close_ms', 'to_ms', 'stop', 'prior', 'lots', 'pyramiding',
           'reversal', 'asserted']


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def epoch_ms(value):
    stamp = datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    if stamp.tzinfo is None:
        raise ValueError(f'{value}: no UTC offset')
    return int(stamp.timestamp() * 1000)


def bars_file(parameters, tape):
    # The two counter controls carry their own exported window (bars_file);
    # every other tape reads its feed trimmed to the chart's from..to window,
    # a tape whose provenance feed_set is "own-feed" from the feeds exported
    # with it.
    if parameters.get('bars_file'):
        return f"{parameters['feed']}-counter-{parameters['name']}.csv"
    if tape.get('feed_set') == 'own-feed':
        return f"{parameters['feed']}-own-{parameters['from']}-{parameters['to']}.csv"
    return f"{parameters['feed']}-{parameters['from']}-{parameters['to']}.csv"


def quantity(parameters):
    if parameters.get('qty') is not None:
        return repr(parameters['qty'])
    return 'source_raw' if parameters.get('sizing') == 'source_raw' else 'percent'


def row(tape, parameters):
    path = tape['path']
    prior = ';'.join(f"{epoch_ms(trade['entry'])}:{epoch_ms(trade['close'])}:{trade['direction']}:{trade['qty']!r}"
                     for trade in parameters.get('prior') or [])
    lots = parameters.get('lots') or []
    stop = parameters.get('stop')
    if parameters['direction'] not in ('long', 'short'):
        raise ValueError(f'{path}: direction {parameters["direction"]!r}')
    reversal = parameters.get('reversal')
    if reversal is not None and (parameters.get('prior') or parameters.get('lots')):
        raise ValueError(f'{path}: a reversal with a ledger or a batch')
    fee_type = parameters.get('fee_type', 'percent')
    if fee_type not in ('percent', 'cash_per_contract'):
        raise ValueError(f'{path}: fee_type {fee_type!r}')
    return [path, bars_file(parameters, tape), repr(parameters['tick']), repr(parameters['step']),
            repr(parameters['initial_capital']), parameters['direction'], quantity(parameters),
            str(int(parameters['slippage'])), repr(parameters['fee']), fee_type, repr(parameters['margin']),
            str(int(bool(parameters['pooc']))), str(int(bool(parameters['coof']))),
            str(epoch_ms(parameters['entry'])), str(epoch_ms(parameters['close'])),
            str(epoch_ms(parameters['to'] + 'T00:00:00+00:00')),
            '' if stop is None else repr(stop), prior, ';'.join(repr(lot) for lot in lots),
            '3' if lots else '1',
            '' if reversal is None else f"{epoch_ms(reversal['entry'])}:{reversal['qty']!r}",
            '1' if tape.get('asserted', True) else '0']


def build():
    provenance = json.loads((ROOT / 'provenance.json').read_text())
    errors = []
    for name, expected in provenance['bars'].items():
        if sha256(ROOT / 'bars' / name) != expected['sha256']:
            errors.append(f'bars/{name}: sha256 differs from provenance.json')
    lines = ['\t'.join(COLUMNS)]
    for tape in sorted(provenance['tapes'], key=lambda item: item['path']):
        directory = ROOT / tape['path']
        if sha256(directory / 'tv_trades.csv') != tape['tv_trades_sha256']:
            errors.append(f"{tape['path']}/tv_trades.csv: sha256 differs from provenance.json")
        if sha256(directory / 'strategy.pine') != tape['strategy_pine_sha256']:
            errors.append(f"{tape['path']}/strategy.pine: sha256 differs from provenance.json")
        parameters = json.loads((directory / 'parameters.json').read_text())
        cells = row(tape, parameters)
        if cells[1] not in provenance['bars']:
            errors.append(f"{tape['path']}: bars/{cells[1]} not listed in provenance.json")
        if any('\t' in cell or '\n' in cell for cell in cells):
            errors.append(f"{tape['path']}: a cell holds a tab or newline")
        lines.append('\t'.join(cells))
    # The order controls are replayed by handwritten hosts, not from cases.tsv:
    # only their files and bars are verified here.
    for control in provenance.get('order_controls', []):
        directory = ROOT / control['path']
        if sha256(directory / 'tv_trades.csv') != control['tv_trades_sha256']:
            errors.append(f"{control['path']}/tv_trades.csv: sha256 differs from provenance.json")
        if sha256(directory / 'strategy.pine') != control['strategy_pine_sha256']:
            errors.append(f"{control['path']}/strategy.pine: sha256 differs from provenance.json")
        if control['bars'] not in provenance['bars']:
            errors.append(f"{control['path']}: bars/{control['bars']} not listed in provenance.json")
    if len(lines) - 1 != provenance['tapes_total']:
        errors.append(f"{len(lines) - 1} tapes, provenance.json says {provenance['tapes_total']}")
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
