"""The inventory follows the selected branch and the rows a test target owns."""
import contextlib
import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from test_check_design_inventory import CMAKE, INCLUDES, KERNEL_TU, design, tree


class InventoryRegistry(unittest.TestCase):
    def test_only_the_selected_arm_can_witness_a_gap(self):
        for condition, middle, marker in (
                ('PINEFORGE_BUILD_SOURCE_LAYER', 'else()', ''),
                ('OFF', 'elseif(NOT PINEFORGE_BUILD_SOURCE_LAYER)', ''),
                ('ON', 'else()', ' **Closed:**')):
            with self.subTest(condition=condition):
                registrations = (f'if({condition})\n'
                                 '    if(OFF)\n    endif()\n'
                                 f'{middle}\n    pineforge_add_unit_test(test_clock)\nendif()\n')
                row = f'| AB | clock | **no**{marker} (`tests/test_clock.cpp`) |'
                with tree(design(row), cmake=CMAKE + INCLUDES, extra={
                        'tests/test_clock.cpp': KERNEL_TU,
                        'tests/lanes.d/clock.cmake': registrations}) as made:
                    code, out = made.run()
                self.assertEqual(code, 1, out)
                self.assertIn('no kernel-profile test' if marker else
                              'row AB reads **no** while tests/test_clock.cpp runs', out)

    def test_an_unknown_witness_condition_names_the_comparison_it_prevents(self):
        with tree(design('| AB | clock | **yes** |'), cmake=CMAKE + INCLUDES, extra={
                'tests/lanes.d/clock.cmake': 'if(UNMODELLED_OPTION)\n'
                                             '    pineforge_add_unit_test(test_clock)\nendif()\n',
                'tests/test_clock.cpp': KERNEL_TU}) as made:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code, _ = made.run()
        self.assertEqual(code, 2)
        self.assertIn('clock.cmake:1', err.getvalue())
        self.assertIn('UNMODELLED_OPTION', err.getvalue())

    def test_a_disabled_accompanying_target_cannot_close_a_gap(self):
        row = '| AB | clock | **no** **Closed:** (`tests/test_clock.cpp`) |'
        cmake = (CMAKE + 'if(PINEFORGE_BUILD_SOURCE_LAYER)\n'
                 '    add_executable(test_clock test_clock.cpp)\n'
                 '    add_test(NAME test_clock COMMAND test_clock)\nendif()\n')
        with tree(design(row), cmake=cmake, extra={'tests/test_clock.cpp': KERNEL_TU}) as made:
            code, out = made.run()
        self.assertEqual(code, 1, out)
        self.assertIn('row AB carries a closure marker', out)

    def test_a_slice_cannot_replace_a_separately_registered_primary(self):
        with tree(design('| AB | clock | **yes** |'), cmake=CMAKE + INCLUDES, extra={
                'tests/test_clock.cpp': KERNEL_TU,
                'tests/lanes.d/clock.cmake': 'pineforge_add_unit_test(test_clock)\n'
                                             'add_test(NAME clock_slice COMMAND test_clock 1)\n'
        }) as made:
            listing = made.root / 'listing.txt'
            listing.write_text('Test #1: test_kernel_one\nTest #2: test_kernel_three\n'
                               'Test #3: test_pure_c\nTest #4: clock_slice\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('kernel build did not register (test_clock.cpp)', out)
            with listing.open('a') as output:
                output.write('Test #5: test_clock\n')
            self.assertEqual(made.run('--ctest-list', str(listing))[0], 0)

    def test_a_python_launcher_is_a_compiled_alias_in_both_directions(self):
        fragment = ('add_test(NAME clock_alias COMMAND ${Python3_EXECUTABLE} -B -c '
                    '"import os, sys; os.execv(sys.argv[1], sys.argv[1:])" '
                    '"$<TARGET_FILE:test_clock>")\n')
        with tree(design('| AB | clock | **yes** |'), cmake=CMAKE + INCLUDES, extra={
                'tests/test_clock.cpp': KERNEL_TU, 'tests/lanes.d/clock.cmake': fragment
        }) as made:
            listing = made.root / 'listing.txt'
            listing.write_text('Test #1: test_kernel_one\nTest #2: test_kernel_three\n'
                               'Test #3: test_pure_c\nTest #4: clock_alias\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('compiled CTest rows missing from the kernel inventory: clock_alias', out)
            (made.root / 'tests/lanes.d/clock.cmake').write_text(
                'pineforge_add_unit_test(test_clock)\n' + fragment)
            listing.write_text(listing.read_text().replace('clock_alias', 'test_clock'))
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('test_clock.cpp: clock_alias', out)

    def test_a_source_comparison_identifies_the_missing_registry(self):
        with tree(design('| AB | clock | **yes** |')) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)
        self.assertIn('CTest registry: NOT COMPARED', out)

    def test_a_computed_launcher_target_is_named_instead_of_ignored(self):
        with tree(design('| AB | clock | **yes** |'), cmake=CMAKE + INCLUDES, extra={
                'tests/lanes.d/clock.cmake': 'add_test(NAME unreadable COMMAND ${Python3_EXECUTABLE} '
                                             '-c "pass" "$<TARGET_FILE:${selected}>")\n'}) as made:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code, _ = made.run()
        self.assertEqual(code, 2)
        self.assertIn('clock.cmake:1', err.getvalue())
        self.assertIn('$<TARGET_FILE:${selected}>', err.getvalue())

    def test_a_compiled_case_registered_by_a_literal_loop_is_required(self):
        extra = {
            'tests/test_clock.cpp': KERNEL_TU,
            'listing.txt': 'Test #1: test_kernel_one\nTest #2: test_kernel_three\n'
                           'Test #3: test_pure_c\nTest #4: test_clock\nTest #5: clock_read\n'}
        cmake = (CMAKE + 'list(APPEND TEST_SOURCES test_clock)\n'
                 'pineforge_add_unit_test(test_clock)\nforeach(mode IN ITEMS read write)\n'
                 '    add_test(NAME clock_${mode} COMMAND test_clock ${mode})\nendforeach()\n')
        with tree(design('| AB | clock | **yes** |'), cmake=cmake, extra=extra) as made:
            listing = made.root / 'listing.txt'
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('test_clock.cpp: clock_write', out)
            with listing.open('a') as output:
                output.write('Test #6: clock_write\n')
            self.assertEqual(made.run('--ctest-list', str(listing))[0], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
