#!/usr/bin/env python3
"""Collect the open-report branch table and check its CTest partition."""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'test_open_report_rows_flat'
SOURCE_FILES = (
    ROOT / 'tests/test_open_report_rows_flat.cpp',
    ROOT / 'tests/CMakeLists.txt',
    ROOT / 'tests/cr150.d/core-cr150-o1w-tradeout.cmake',
)


def refuse_protected(path: Path) -> None:
    if (path.name.endswith('.env')
            or any(part in ('.ssh', '.aws', '.codex') for part in path.parts)
            or '/.config/zsh/keys/' in str(path) + '/'
            or '/.config/pineforge/' in str(path) + '/'):
        raise ValueError(f'refusing protected path: {path}')


def read_sources() -> tuple[str, str, str]:
    """Read only these fixed sources, announcing and checking every path."""
    print('Open-report cover sources:\n'
          + '\n'.join(map(str, SOURCE_FILES)), flush=True)
    paths = []
    for path in SOURCE_FILES:
        refuse_protected(path)
        resolved = path.resolve()
        refuse_protected(resolved)
        paths.append(resolved)
    print('Open-report cover reads:\n' + '\n'.join(map(str, paths)), flush=True)
    return tuple(path.read_text() for path in paths)


def without_comments(text: str) -> str:
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.DOTALL)


def collect_branches(cpp: str) -> tuple[int, list[tuple[str, int, int]]]:
    cpp = without_comments(cpp)
    counts = re.findall(r'constexpr int kSliceCount = (\d+);', cpp)
    tables = re.findall(
        r'constexpr PopulationBranch kPopulationBranches\[\] = \{(.*?)\n\};',
        cpp, re.DOTALL)
    if len(counts) != 1 or len(tables) != 1:
        raise AssertionError('expected one slice count and one executable branch table')
    branches = []
    for line in tables[0].splitlines():
        if not line.strip():
            continue
        row = re.fullmatch(
            r'\s*\{ReportBranch::(FULL|SUMMARY_ONLY|FULL_SUMMARY_BITS), '
            r'(\d+), (\d+)\},\s*', line)
        if not row:
            raise AssertionError(f'unrecognized branch table row: {line}')
        branches.append((row[1], int(row[2]), int(row[3])))
    return int(counts[0]), branches


def collect_rows(cmake: str) -> list[tuple[str, str]]:
    cmake = re.sub(r'#[^\n]*', '', cmake)
    return [
        (match[1], ' '.join(match[2].split()))
        for match in re.finditer(
            r'add_test\(NAME (test_open_report_rows_flat(?:_[A-Za-z0-9_]+)?)'
            r'\s+COMMAND\s+(.*?)\)', cmake, re.DOTALL)
    ]


def partition(k: int, branches: list[tuple[str, int, int]],
              rows: list[tuple[str, str]]) -> dict[int, list[tuple[str, int]]]:
    slice_rows = [(name, command) for name, command in rows
                  if re.fullmatch(PREFIX + r'_slice_\d+', name)]
    names = [name for name, _ in slice_rows]
    if len(names) != k or len(set(names)) != k:
        raise AssertionError('slice count does not match registered rows')
    selected: dict[int, list[tuple[str, int]]] = {i: [] for i in range(1, k + 1)}
    for i in selected:
        expected = (f'{PREFIX}_slice_{i}', f'{PREFIX} --slice {i}/{k}')
        if slice_rows.count(expected) != 1:
            raise AssertionError(f'missing or incorrect slice command: {expected}')
    for mode, lots, index in branches:
        if index not in selected or lots <= 0:
            raise AssertionError('branch has an invalid slice or population')
        selected[index].append((mode, lots))
    if any(not cases for cases in selected.values()):
        raise AssertionError('empty slice')
    full = Counter((mode, lots) for mode, lots, _ in branches)
    union = Counter(case for cases in selected.values() for case in cases)
    if any(count != 1 for count in union.values()) or union != full:
        raise AssertionError('branches overlap or do not cover the full enumeration')
    return selected


class OpenReportSlicesCover(unittest.TestCase):
    def test_registered_slices_cover_each_population_once(self):
        cpp, main_cmake, fragment = read_sources()
        cmake = main_cmake + '\n' + fragment
        rows = collect_rows(cmake)
        # This first assertion also applies to the unsplit registration:
        # four explicit slice rows and their cover must replace the serial row.
        expected_names = [f'{PREFIX}_slice_{i}' for i in range(1, 5)]
        expected_names.append(f'{PREFIX}_slices_cover')
        self.assertCountEqual([name for name, _ in rows], expected_names)
        self.assertCountEqual(
            [name for name, _ in collect_rows(fragment)], expected_names)

        k, branches = collect_branches(cpp)
        self.assertEqual(k, 4)
        selected = partition(k, branches, rows)
        # Independent population contract: deleting an executable table entry
        # must fail even if the remaining table is a perfect partition.
        expected_population = Counter(
            (mode, lots) for lots in (1, 1000, 100000)
            for mode in ('FULL', 'SUMMARY_ONLY'))
        expected_population[('FULL_SUMMARY_BITS', 64)] = 1
        self.assertEqual(Counter((mode, lots) for mode, lots, _ in branches),
                         expected_population)
        self.assertEqual(selected, {
            1: [('FULL', 1), ('FULL', 1000)],
            2: [('FULL', 100000)],
            3: [('SUMMARY_ONLY', 1), ('SUMMARY_ONLY', 1000),
                ('FULL_SUMMARY_BITS', 64)],
            4: [('SUMMARY_ONLY', 100000)],
        })

        cover_command = next(command for name, command in rows
                             if name == f'{PREFIX}_slices_cover')
        self.assertEqual(
            cover_command,
            '${Python3_EXECUTABLE} '
            '${PROJECT_SOURCE_DIR}/scripts/test_open_report_rows_flat_split.py')
        self.assertEqual(fragment.count(f'pineforge_add_unit_test({PREFIX})'), 1)
        helper = re.search(
            r'function\(pineforge_add_unit_test test_name\)(.*?)endfunction\(\)',
            main_cmake, re.DOTALL)
        self.assertIsNotNone(helper)
        self.assertIn(f'AND NOT test_name STREQUAL "{PREFIX}"', helper[1])
        self.assertNotRegex(cmake, rf'add_test\(NAME {PREFIX}\s')
        for name in expected_names[:-1]:
            self.assertRegex(fragment, rf'(?m)^    {re.escape(name)}$')
        properties = re.findall(
            r'set_tests_properties\((.*?)\s+PROPERTIES\s+(.*?)\)',
            cmake, re.DOTALL)
        for name in expected_names:
            matching = [values.strip() for names, values in properties
                        if name in names.split()]
            self.assertEqual(
                matching,
                [] if name == f'{PREFIX}_slices_cover' else ['LABELS "slow;mem"'],
                name)
        for label_list in ('PINEFORGE_PR_SLOW_TESTS', 'PINEFORGE_PR_FAST_TESTS'):
            blocks = re.findall(
                rf'^set\({label_list}\n(.*?)^\)', main_cmake,
                re.MULTILINE | re.DOTALL)
            self.assertEqual(len(blocks), 1, label_list)
            self.assertFalse(
                set(re.findall(r'\btest_[A-Za-z0-9_]+\b', blocks[0]))
                & {PREFIX, *expected_names},
                label_list)

        # The actual execution loop consumes the collected table once. These
        # checks tie collection to dispatch and keep invalid/empty inputs fatal.
        code = without_comments(cpp)
        self.assertIn('for (const auto& branch : kPopulationBranches)', code)
        self.assertIn('if (slice != 0 && branch.slice != slice) continue;', code)
        self.assertRegex(code, r'if \(selected == 0\) \{[^}]*return 2;\s*\}')
        self.assertIn('int slice = 0;', code)
        self.assertIn('if (argc != 1)', code)
        self.assertIn('argc != 3 || std::strcmp(argv[1], "--slice") != 0', code)
        self.assertIn('std::strlen(argv[2]) != 3', code)
        self.assertIn("argv[2][0] < '1' || argv[2][0] > '0' + kSliceCount", code)
        self.assertIn("argv[2][1] != '/'", code)
        self.assertIn("argv[2][2] != '0' + kSliceCount", code)
        self.assertIn("slice = argv[2][0] - '0';", code)
        for mode, call in (
                ('FULL', 'full_population(branch.lots);'),
                ('SUMMARY_ONLY', 'summary_population(branch.lots);'),
                ('FULL_SUMMARY_BITS', 'summary_matches_full_bits();')):
            self.assertRegex(
                code, rf'case ReportBranch::{mode}:\s*'
                + (r'CHECK\(branch.lots == 64\);\s*' if mode == 'FULL_SUMMARY_BITS' else '')
                + re.escape(call) + r'\s*break;')
        self.assertNotIn('compare_full', code)
        self.assertIn('check_row(report.trades[i], expected);', code)
        self.assertIn(
            'expected_net += summary.reference_row(static_cast<std::size_t>(i)).pnl;',
            code)
        self.assertIn('CHECK(same_bits(envelope.report.net_profit, expected_net));', code)
        self.assertIn('CHECK(peak.bytes <= kPeakReportBytes);', code)
        self.assertIn('constexpr std::size_t kPeakReportBytes = 256u * 1024u;', code)
        self.assertRegex(
            code, r'CHECK\(std::memcmp\(&envelope.report.metrics, &reference.metrics,'
            r'\s*sizeof\(pf_metrics_t\)\) == 0\);')

        # Mutation controls use the real collected table and registration.
        with self.assertRaisesRegex(AssertionError, 'registered rows'):
            partition(k, branches, [row for row in rows
                                    if row[0] != f'{PREFIX}_slice_2'])
        with self.assertRaisesRegex(AssertionError, 'overlap'):
            partition(k, branches + [branches[0]], rows)
        with self.assertRaisesRegex(AssertionError, 'registered rows'):
            partition(k - 1, branches, rows)
        empty_second = [(mode, lots, 1 if index == 2 else index)
                        for mode, lots, index in branches]
        with self.assertRaisesRegex(AssertionError, 'empty slice'):
            partition(k, empty_second, rows)

        print(f'Open-report cover: {len(branches)} branches, {k} nonempty slices',
              flush=True)
        for index, cases in selected.items():
            print(f'  slice {index}/{k}: {cases}', flush=True)


if __name__ == '__main__':
    unittest.main()
