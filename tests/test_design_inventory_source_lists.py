"""Which values of a CMake source list can witness an inventory closure."""
import argparse
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import check_design_inventory as inventory
from test_check_design_inventory import CMAKE, KERNEL_TU, design, tree

CTEST = 'ctest'
BUILD = None


class SourceLists(unittest.TestCase):
    def inspect(self, assignments, consume='list(APPEND TEST_SOURCES ${extra})'):
        row = '| AB | clock | **no** **Closed:** (`tests/test_clock.cpp`) |'
        with tree(design(row), cmake=CMAKE + assignments + '\n' + consume + '\n',
                  extra={'tests/test_clock.cpp': KERNEL_TU}) as made:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code, out = made.run()
        return code, out + err.getvalue()

    def test_only_selected_writes_supply_a_source_list(self):
        for writes, witness in (
                ('if(OFF)\nset(extra test_clock)\nendif()', False),
                ('if(ON)\nset(extra test_clock)\nendif()', True),
                ('set(extra test_clock)\nif(OFF)\nset(extra test_adapter_one)\nendif()', True),
                ('set(extra test_clock)\nif(ON)\nset(extra test_adapter_one)\nendif()', False),
                ('set(extra)\nif(OFF)\nlist(APPEND extra test_clock)\nendif()', False),
                ('set(extra)\nif(OFF)\nelse()\nlist(APPEND extra test_clock)\nendif()', True)):
            with self.subTest(writes=writes):
                code, out = self.inspect(writes)
                self.assertEqual(code, 0 if witness else 1, out)
                self.assertIn('0 findings ... OK' if witness else
                              'row AB carries a closure marker but names tests/test_clock.cpp', out)

    def test_an_undecidable_source_list_is_named_at_its_use(self):
        writes = 'if(UNMODELLED_OPTION)\nset(extra test_clock)\nendif()'
        for consume in ('list(APPEND TEST_SOURCES ${extra})',
                        'set(alias ${extra})\nlist(APPEND TEST_SOURCES ${alias})'):
            with self.subTest(consume=consume):
                code, out = self.inspect(writes, consume)
                self.assertEqual(code, 2, out)
                self.assertRegex(out, r'NOT COMPARED: [^\n]*\bextra\b')
                self.assertIn('UNMODELLED_OPTION', out)
                self.assertNotIn('0 findings', out)
        for assignments in (writes + '\nset(extra test_clock)',
                            writes.replace('extra', 'unused') + '\nset(extra test_clock)'):
            with self.subTest(assignments=assignments):
                code, out = self.inspect(assignments)
                self.assertEqual(code, 0, out)

    def test_private_row_removal_in_the_repository_registry(self):
        members, _, commands, _, _ = inventory.registrations(inventory.ROOT)
        primary = 'test_native_report_truth'
        # Removed from the registration loop by the appended private-row list;
        # its own registration is also disabled in the kernel profile.
        removed = 'test_dropped_reversal_mc_first_l4c'
        self.assertIn(primary, commands[primary])
        self.assertFalse(removed in members, f'{removed} survived its private-row removal')
        self.assertFalse(commands.get(removed), f'{removed} still has a compiled registration')
        self.assertIsNotNone(BUILD, 'the repository control requires --build-dir')
        cache = (BUILD / 'CMakeCache.txt').read_text().splitlines()
        self.assertTrue(f'CMAKE_HOME_DIRECTORY:INTERNAL={inventory.ROOT}' in cache,
                        'the CMake cache belongs to a different source tree')
        listing = subprocess.run([CTEST, '--test-dir', str(BUILD), '-N'],
                                 capture_output=True, text=True, check=True).stdout
        listed = inventory.ctest_rows(listing)
        self.assertIn(primary, listed)
        layer = [line.split('=', 1)[-1] for line in cache
                 if line.startswith('PINEFORGE_BUILD_SOURCE_LAYER:BOOL=')]
        self.assertIn(layer, (['ON'], ['OFF']))
        if layer == ['OFF']:
            self.assertNotIn(removed, listed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ctest', required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    options, rest = parser.parse_known_args()
    CTEST, BUILD = options.ctest, options.build_dir
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
