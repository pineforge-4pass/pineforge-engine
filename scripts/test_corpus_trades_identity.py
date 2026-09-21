#!/usr/bin/env python3
"""Unit tests for the corpus byte-identity checker's judgements."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from corpus_trades_identity import (  # noqa: E402
    ALLOWED_EXTRA_COLUMNS, first_differing_rows, header_problem, parse_subset,
    subset_paths,
)

ROOT = Path(__file__).resolve().parents[1]

TAPE = ('a,b,c\n'
        '1,2,3\n'
        '4,5,6\n')
EXTENDED = (f'a,b,c,{ALLOWED_EXTRA_COLUMNS[0]}\n'
            '1,2,3,\n'
            '4,5,6,open\n')


class HeaderRule(unittest.TestCase):
    def test_identical_header_is_no_problem(self):
        self.assertIsNone(header_problem('p', TAPE, TAPE))

    def test_declared_trailing_column_is_an_extension(self):
        self.assertIsNone(header_problem('p', TAPE, EXTENDED))

    def test_undeclared_trailing_column_is_drift(self):
        produced = 'a,b,c,Engine mood\n1,2,3,\n4,5,6,\n'
        problem = header_problem('p', TAPE, produced)
        self.assertIsNotNone(problem)
        self.assertIn('undeclared trailing column', problem)
        self.assertIn('Engine mood', problem)

    def test_a_renamed_recorded_column_is_drift(self):
        produced = 'a,b,C\n1,2,3\n4,5,6\n'
        problem = header_problem('p', TAPE, produced)
        self.assertIsNotNone(problem)
        self.assertIn('header changed', problem)

    def test_a_dropped_recorded_column_is_drift(self):
        produced = 'a,b\n1,2\n4,5\n'
        self.assertIn('header changed', header_problem('p', TAPE, produced))

    def test_an_empty_file_is_a_problem(self):
        self.assertIn('empty trades file', header_problem('p', TAPE, ''))


class Localisation(unittest.TestCase):
    def test_reports_the_first_row_that_moved_on_a_recorded_column(self):
        produced = 'a,b,c\n1,2,3\n4,5,7\n'
        lines = first_differing_rows('p', TAPE, produced, lines=20)
        self.assertIn('row 3', lines[0])
        self.assertIn('4,5,6', lines[1])
        self.assertIn('4,5,7', lines[2])

    def test_an_extension_only_difference_says_so(self):
        lines = first_differing_rows('p', TAPE, EXTENDED, lines=20)
        self.assertEqual(len(lines), 1)
        self.assertIn('trailing column', lines[0])

    def test_a_row_count_change_is_reported(self):
        produced = 'a,b,c\n1,2,3\n'
        lines = first_differing_rows('p', TAPE, produced, lines=20)
        self.assertIn('2 tape rows, 1 produced', lines[0])

    def test_the_line_budget_is_respected(self):
        produced = 'a,b,c\n9,9,9\n8,8,8\n'
        self.assertEqual(len(first_differing_rows('p', TAPE, produced, lines=2)), 2)


class SubsetFile(unittest.TestCase):
    def test_comments_blanks_and_trailing_slashes_are_ignored(self):
        text = ('# header\n'
                '\n'
                'validation/a-01   # the reason\n'
                'validation/b-01/\n'
                '   # indented comment\n')
        self.assertEqual(parse_subset(text), ['validation/a-01', 'validation/b-01'])

    def test_a_repeated_probe_is_named_once(self):
        self.assertEqual(parse_subset('validation/a-01\nvalidation/a-01\n'),
                         ['validation/a-01'])

    def test_an_empty_file_names_no_probe(self):
        self.assertEqual(parse_subset('# only a comment\n'), [])

    def test_paths_are_the_probes_trades_files(self):
        self.assertEqual(subset_paths('validation/a-01\n'),
                         ['validation/a-01/engine_trades.csv'])

    def test_the_shipped_subset_is_a_nonempty_slice_of_the_baseline(self):
        """The rows the pull-request gate judges must all be pinned."""
        pinned = {line.split(None, 1)[1].strip()
                  for line in (ROOT / 'scripts/corpus_parity_baseline.txt').read_text().splitlines()
                  if line and not line.startswith('#')}
        paths = subset_paths((ROOT / 'scripts/corpus_parity_subset.txt').read_text())
        self.assertGreaterEqual(len(paths), 20)
        self.assertLess(len(paths), len(pinned))
        self.assertEqual([path for path in paths if path not in pinned], [])


if __name__ == '__main__':
    unittest.main()
