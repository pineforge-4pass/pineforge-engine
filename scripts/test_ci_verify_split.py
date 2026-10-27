#!/usr/bin/env python3
"""Collect-only proof and mutation controls for the registered ci_verify slices."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest

import test_ci_verify


ROOT = Path(__file__).resolve().parents[1]


def iter_test_cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from iter_test_cases(item)
        else:
            yield item


def require_exact_cover(full, slices):
    """Compare actual collected identities, including their multiplicities."""
    expected = Counter(full)
    actual = Counter(name for part in slices for name in part)
    missing = sorted((expected - actual).elements())
    extra = sorted((actual - expected).elements())
    duplicates = sorted(name for name, count in actual.items() if count > 1)
    empty = [index for index, part in enumerate(slices, 1) if not part]
    if (not expected or any(count != 1 for count in expected.values())
            or not slices or missing or extra or duplicates or empty):
        raise ValueError(
            f'invalid slice cover: missing={missing}, extra={extra}, '
            f'duplicates={duplicates}, empty={empty}')


class CiVerifySlicesCover(unittest.TestCase):
    slice_count = None

    def collect_command(self, *arguments):
        return subprocess.run(
            [sys.executable, '-B', str(ROOT / 'scripts' / 'test_ci_verify.py'),
             *arguments, '--collect-only'],
            capture_output=True, text=True)

    def collected(self, *arguments):
        result = self.collect_command(*arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        names = json.loads(result.stdout)
        self.assertIsInstance(names, list)
        self.assertTrue(names, 'a collection command must not accept an empty slice')
        self.assertTrue(all(isinstance(name, str) and name for name in names))
        return names

    def test_registered_slices_cover_real_collection(self):
        cmake = (ROOT / 'tests' / 'CMakeLists.txt').read_text()
        statements = test_ci_verify._cmake_statements(cmake)
        registrations = [statement.split() for _, statement in statements
                         if statement.lstrip().startswith('add_test(')
                         and '/scripts/test_ci_verify.py' in statement]
        # Main registers the unsplit command with no selector. This assertion
        # is red there before any new selector or helper is needed.
        self.assertEqual(registrations, [[
            'add_test(NAME', 'test_ci_verify_slice_${_pf_ci_verify_slice}',
            'COMMAND', '${Python3_EXECUTABLE}',
            '${PROJECT_SOURCE_DIR}/scripts/test_ci_verify.py', '--slice',
            '${_pf_ci_verify_slice}/${PINEFORGE_CI_VERIFY_SLICE_COUNT})',
        ]])
        count = re.search(
            r'set\(PINEFORGE_CI_VERIFY_SLICE_COUNT\s+([1-9][0-9]*)\)', cmake)
        self.assertIsNotNone(count)
        count = int(count.group(1))
        self.assertGreater(count, 1)
        if self.slice_count is not None:
            self.assertEqual(count, self.slice_count)
        self.assertIn(
            'foreach(_pf_ci_verify_slice RANGE 1 ${PINEFORGE_CI_VERIFY_SLICE_COUNT})',
            cmake)
        self.assertRegex(
            cmake,
            r'add_test\(NAME test_ci_verify_slices_cover\s+COMMAND '
            r'\$\{Python3_EXECUTABLE\}\s+'
            r'\$\{PROJECT_SOURCE_DIR\}/scripts/test_ci_verify_split\.py\s+'
            r'--slice-count \$\{PINEFORGE_CI_VERIFY_SLICE_COUNT\}\)')
        rows = {f'test_ci_verify_slice_{index}' for index in range(1, count + 1)}
        self.assertEqual(
            {name for name in test_ci_verify.ci_verify.PINNED_SLOW_TEST_ROWS
             if name.startswith('test_ci_verify')}, rows)

        # Independent real unittest collection: no literal population and no
        # execution of any of the original test bodies or module fixtures.
        loader = unittest.TestLoader()
        suite = loader.loadTestsFromModule(test_ci_verify)
        self.assertEqual(loader.errors, [])
        full = sorted(case.id().removeprefix(test_ci_verify.__name__ + '.')
                      for case in iter_test_cases(suite))
        self.assertEqual(self.collected(), full)
        slices = [self.collected('--slice', f'{index}/{count}')
                  for index in range(1, count + 1)]
        require_exact_cover(full, slices)
        for index, part in enumerate(slices):
            self.assertEqual(part, full[index::count])

        # Mutate the actual collected plan, not a miniature substitute.
        for mutation in ('missing', 'duplicate'):
            with self.subTest(mutation=mutation):
                damaged = [list(part) for part in slices]
                name = damaged[0][0]
                if mutation == 'missing':
                    damaged[0].remove(name)
                else:
                    damaged[1].append(name)
                with self.assertRaises(ValueError) as error:
                    require_exact_cover(full, damaged)
                self.assertIn(mutation, str(error.exception))
                self.assertIn(name, str(error.exception))
        with self.subTest(mutation='empty'):
            damaged = [list(part) for part in slices]
            damaged.append([])
            with self.assertRaisesRegex(ValueError, 'empty='):
                require_exact_cover(full, damaged)

        # More slices than real cases must fail, including collect-only mode.
        empty = self.collect_command('--slice', f'{len(full) + 1}/{len(full) + 1}')
        self.assertNotEqual(empty.returncode, 0, empty.stdout + empty.stderr)
        self.assertIn('empty slice', empty.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--slice-count', type=int)
    options, remaining = parser.parse_known_args()
    CiVerifySlicesCover.slice_count = options.slice_count
    unittest.main(argv=[sys.argv[0], *remaining])
