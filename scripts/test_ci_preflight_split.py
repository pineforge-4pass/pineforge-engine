#!/usr/bin/env python3
"""Collect preflight unittest slices and check their public CTest contract."""
from collections import Counter
from pathlib import Path
import re
import unittest

import test_ci_preflight as preflight


ROOT = Path(__file__).resolve().parents[1]
SLICE_COUNT = 8
SLICE_NAMES = tuple(f'test_ci_preflight_slice_{index}'
                    for index in range(1, SLICE_COUNT + 1))
COVER_NAME = 'test_ci_preflight_slices_cover'


def collected_ids(suite):
    pending = [suite]
    ids = []
    while pending:
        item = pending.pop()
        if isinstance(item, unittest.TestSuite):
            pending.extend(item)
        else:
            ids.append(item.id())
    return sorted(ids)


def registered_slices(cmake):
    code = '\n'.join(line.split('#', 1)[0] for line in cmake.splitlines())
    rows = re.findall(
        r'\badd_test\(\s*NAME\s+(test_ci_preflight(?:_[A-Za-z0-9_]+)?)\s+'
        r'COMMAND\s+([^)]*)\)', code)
    expected = set(SLICE_NAMES) | {COVER_NAME}
    if Counter(name for name, _ in rows) != Counter(expected):
        raise ValueError('preflight rows must be eight unique slices and one cover')
    commands = {name: command.split() for name, command in rows}
    for index, name in enumerate(SLICE_NAMES, 1):
        if commands[name] != [
                '${Python3_EXECUTABLE}',
                '${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight.py',
                '--slice', f'{index}/{SLICE_COUNT}']:
            raise ValueError(f'{name} must run its exact unittest slice')
    if commands[COVER_NAME] != [
            '${Python3_EXECUTABLE}',
            '${PROJECT_SOURCE_DIR}/scripts/test_ci_preflight_split.py']:
        raise ValueError('preflight cover must run its collection-only test')
    return tuple(commands[name][-1] for name in SLICE_NAMES)


def validate_partition(full_ids, slices, registered_count):
    if not full_ids or len(set(full_ids)) != len(full_ids):
        raise ValueError('full unittest population must be nonempty and unique')
    if len(slices) != registered_count or registered_count != SLICE_COUNT:
        raise ValueError('slice count must match the eight registered rows')
    if any(not selection for selection in slices):
        raise ValueError('every slice must collect at least one case')
    if Counter(case for selection in slices for case in selection) != Counter(full_ids):
        raise ValueError('slice union must equal discovery without duplicates')


class PreflightSlicesCover(unittest.TestCase):
    def test_slices_cover(self):
        cmake = (ROOT / 'tests' / 'CMakeLists.txt').read_text()
        # On the serial registration this fails on real row identities,
        # before accessing any newly added selector.
        specs = registered_slices(cmake)
        loader = unittest.TestLoader()
        full_suite = loader.loadTestsFromModule(preflight)
        self.assertEqual(loader.errors, [])
        full_ids = collected_ids(full_suite)
        self.assertEqual(len(full_ids), full_suite.countTestCases())
        slices = []
        for index, spec in enumerate(specs):
            sliced_loader = preflight.PreflightSliceLoader(spec)
            suite = sliced_loader.loadTestsFromModule(preflight)
            self.assertEqual(sliced_loader.errors, [])
            selected = collected_ids(suite)
            self.assertEqual(selected, full_ids[index::SLICE_COUNT])
            self.assertEqual(suite.countTestCases(), len(selected))
            slices.append(selected)
        validate_partition(full_ids, slices, len(specs))

        properties = re.findall(r'\bset_tests_properties\((.*?)\)', cmake, re.DOTALL)
        for name in (*SLICE_NAMES, COVER_NAME):
            matching = [args for args in properties if name in args.split()]
            self.assertEqual(len(matching), 1, name)
            names, values = matching[0].split('PROPERTIES', 1)
            self.assertIn(name, names.split())
            self.assertEqual(values.split(), [
                'ENVIRONMENT', '"TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp"'])
        for label in ('FAST', 'SLOW'):
            block = re.search(rf'set\(PINEFORGE_PR_{label}_TESTS\s+(.*?)\)',
                              cmake, re.DOTALL)
            self.assertIsNotNone(block)
            self.assertTrue(set(SLICE_NAMES).isdisjoint(block[1].split()))
            self.assertNotIn(COVER_NAME, block[1].split())

        # Mutate the same source/collections used by the real cover.
        row = re.search(r'add_test\(NAME test_ci_preflight_slice_8\s+'
                        r'COMMAND\s+[^)]*\)', cmake)[0]
        with self.assertRaisesRegex(ValueError, 'eight unique slices'):
            registered_slices(cmake.replace(row, '', 1))
        with self.assertRaisesRegex(ValueError, 'eight unique slices'):
            registered_slices(cmake + '\n' + row)
        with self.assertRaisesRegex(ValueError, 'exact unittest slice'):
            registered_slices(cmake.replace('--slice 8/8', '--slice 8/7', 1))
        with self.assertRaisesRegex(ValueError, 'eight registered rows'):
            validate_partition(full_ids, slices[:-1], len(specs))
        with self.assertRaisesRegex(ValueError, 'without duplicates'):
            validate_partition(full_ids, [slices[0] + [slices[1][0]], *slices[1:]],
                               len(specs))
        with self.assertRaisesRegex(ValueError, 'at least one case'):
            validate_partition(full_ids, [[], *slices[1:]], len(specs))
        for spec in ('0/8', '9/8', '-1/8', '1/0', '1/7', '1/9', '1', 'all'):
            with self.subTest(spec=spec), self.assertRaisesRegex(ValueError, 'slice'):
                preflight.PreflightSliceLoader(spec)
        for population, spec in (([], '1/8'), (full_ids[:1], '8/8')):
            with self.subTest(population=population, spec=spec):
                with self.assertRaisesRegex(ValueError, 'selects no tests'):
                    preflight.slice_test_ids(population, spec)
        for population in (list(reversed(full_ids)), full_ids[::2]):
            for index, spec in enumerate(specs):
                with self.subTest(population_size=len(population), spec=spec):
                    self.assertEqual(preflight.slice_test_ids(population, spec),
                                     sorted(population)[index::SLICE_COUNT])


if __name__ == '__main__':
    unittest.main()
