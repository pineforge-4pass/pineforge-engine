#!/usr/bin/env python3
"""The pull-request parity subset must witness every mutation the full sweep caught in the
recorded mutation battery (scripts/corpus_parity_mutation_battery.tsv, R5 lane H-MEASURE).

AUDIT3-opus2 H12 asked for the subset to be measured against mutations: 44 single-line kernel,
adapter and ta mutations plus 17 designed afterwards as a holdout, each built and run over the
whole 312-probe corpus. The battery file records the 39 the full sweep caught, each with the
probes that caught it; the original 30-row subset caught 19 of them. A subset row removed, or a
battery row added that no subset probe detects, fails here, in the source-only ci_preflight
stage, before any corpus build.
"""
import os
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(os.environ.get('PF_ROOT', Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(ROOT / 'scripts'))

from corpus_trades_identity import BASELINE, parse_subset  # noqa: E402

SUBSET = Path(os.environ.get('SUBSET_FILE', ROOT / 'scripts' / 'corpus_parity_subset.txt'))
BATTERY = Path(os.environ.get('BATTERY_FILE',
                              ROOT / 'scripts' / 'corpus_parity_mutation_battery.tsv'))


def battery_rows():
    rows = []
    for line in BATTERY.read_text().splitlines():
        if not line or line.startswith('#') or line.startswith('id\t'):
            continue
        mid, site, mutation, probes = line.split('\t')
        rows.append((mid, site, mutation, set(probes.split(','))))
    return rows


class SubsetCoversTheBattery(unittest.TestCase):
    def test_every_caught_mutation_has_a_subset_witness(self):
        subset = {p.split('/')[-1] for p in parse_subset(SUBSET.read_text())}
        escaped = [f'{mid} {site} ({mutation}): detected only by {", ".join(sorted(probes))}'
                   for mid, site, mutation, probes in battery_rows() if not probes & subset]
        self.assertEqual(escaped, [], f'{len(escaped)} recorded mutations escape {SUBSET.name}:\n  '
                         + '\n  '.join(escaped))

    def test_battery_names_only_pinned_probes(self):
        pinned = {line.split()[1].split('/')[1] for line in BASELINE.read_text().splitlines()
                  if line and not line.startswith('#')}
        unknown = sorted({p for *_, probes in battery_rows() for p in probes} - pinned)
        self.assertEqual(unknown, [])

    def test_the_battery_is_not_empty(self):
        self.assertGreaterEqual(len(battery_rows()), 20)


if __name__ == '__main__':
    unittest.main(verbosity=2)
