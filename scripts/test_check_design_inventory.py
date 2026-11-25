#!/usr/bin/env python3
"""Self-tests for the §1 inventory guard: it must be able to FAIL.

Each case builds a throwaway tree — a ``tests/CMakeLists.txt`` that registers
a handful of test TUs, the TUs themselves (one of which reaches a source-layer
header), and a design document with a §1 table — and drives the real
``check_design_inventory.main`` over it with ``--root``.

The must-fail half is one case per rule: a ``no`` row whose kernel-profile test
is passing, a ``partial`` row likewise, a gap row whose closure marker names
only a source-bound test, a row citing a test that is not in the tree, and a
``--ctest-list`` that disagrees with the reach walk in either direction.

The must-not-fire half is what keeps the guard usable: a closure marker in the
form the real table uses, a ``no`` row whose only witness is source-bound (the
capability really is adapter-only), an ``n/a`` quirk row, a row with no test at
all, and a waiver row whose verdict is neither yes/partial/no.

The shape half proves the guard exits 2 — not 0 — when it cannot read the
document or find any kernel-profile TU, because a checker that cannot see must
not look like a pass.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import check_design_inventory as guard

# A TU that includes only kernel headers, and one that reaches the source layer
# through a quoted local header - the transitive case the CMake function walks.
KERNEL_TU = '#include <pineforge/native_host.hpp>\nint main() { return 0; }\n'
SOURCE_TU = '#include <pineforge/source/pine_strategy_host.hpp>\nint main() { return 0; }\n'
VIA_LOCAL = '#include "shared_fixture.hpp"\nint main() { return 0; }\n'
LOCAL_FIXTURE = '#include <pineforge/compat/pine/order_birth.hpp>\n'

CMAKE = """set(TEST_SOURCES
    test_kernel_one
    test_kernel_two
    test_adapter_one
    test_via_local
)
list(APPEND TEST_SOURCES test_kernel_three)
set(LEGACY_TEST_SOURCES test_kernel_two)
list(REMOVE_ITEM TEST_SOURCES ${LEGACY_TEST_SOURCES})
add_executable(test_pure_c test_pure_c.c)
"""

INCLUDES = """file(GLOB LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lanes.d/*.cmake")
list(SORT LANE_CMAKE)
foreach(lane_file IN LISTS LANE_CMAKE)
    include("${lane_file}")
endforeach()
"""

HEAD = """## 1. Inventory

Columns: **Feature** · **Native today** · **Class** · **Lane** · **Sources** · **Notes**

### 1.1 Things

| ID | Feature | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
"""

TAIL = '\n## 2. Structural work\n\nnothing here.\n'


UNIT_HELPER = """function(pineforge_add_unit_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()
"""

UNIT_LOOP = """foreach(test_name ${TEST_SOURCES})
    pineforge_add_unit_test(${test_name})
endforeach()
"""


def design(*rows: str) -> str:
    return HEAD + ''.join(row.rstrip('\n') + '\n' for row in rows) + TAIL


class Tree:
    """A throwaway repository: tests/, a CMake list, and the design document."""

    def __init__(self, design_text: str, *, cmake: str = CMAKE,
                 extra: dict[str, str] | None = None) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / 'tests').mkdir(parents=True)
        (self.root / 'docs/design').mkdir(parents=True)
        (self.root / 'tests/CMakeLists.txt').write_text(cmake)
        for name in ('test_kernel_one', 'test_kernel_two', 'test_kernel_three'):
            (self.root / 'tests' / (name + '.cpp')).write_text(KERNEL_TU)
        (self.root / 'tests/test_adapter_one.cpp').write_text(SOURCE_TU)
        (self.root / 'tests/test_via_local.cpp').write_text(VIA_LOCAL)
        (self.root / 'tests/shared_fixture.hpp').write_text(LOCAL_FIXTURE)
        (self.root / 'tests/test_pure_c.c').write_text(KERNEL_TU)
        (self.root / 'docs/design/native-feature-parity.md').write_text(design_text)
        for name, body in (extra or {}).items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)

    def run(self, *extra_argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = guard.main(['--root', str(self.root), *extra_argv])
        return code, out.getvalue()

    def close(self) -> None:
        self.dir.cleanup()


@contextlib.contextmanager
def tree(design_text: str, **kwargs):
    made = Tree(design_text, **kwargs)
    try:
        yield made
    finally:
        made.close()


class MustFail(unittest.TestCase):
    def test_no_row_with_a_kernel_test(self):
        row = ('| OL1 | a feature | **no** — nothing native '
               '(`tests/test_kernel_one.cpp`) | K | L7 | F:A1 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 1, out)
        self.assertIn('row OL1 reads **no**', out)
        self.assertIn('tests/test_kernel_one.cpp', out)

    def test_partial_row_with_a_kernel_test(self):
        row = ('| TR2 | a feature | **partial** — half of it '
               '(`tests/test_kernel_three.cpp`) | K | L7 | F:B2 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 1, out)
        self.assertIn('row TR2 reads **partial**', out)

    def test_partial_with_a_qualifier_is_still_a_gap_row(self):
        row = ('| HT2 | a feature | **partial (inert)** — staged but unused '
               '(`tests/test_kernel_one.cpp`) | K | L6 | F:F2 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 1, out)
        self.assertIn('row HT2 reads **partial**', out)

    def test_marker_whose_only_witness_is_source_bound(self):
        row = ('| SZ3 | a feature | **no** — not native. **Closed:** the adapter '
               'does it (`tests/test_adapter_one.cpp`) | K | L3 | F:C1 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 1, out)
        self.assertIn('carries a closure marker', out)
        self.assertIn('no kernel-profile test', out)

    def test_row_citing_a_test_that_is_not_in_the_tree(self):
        row = ('| RP9 | a feature | **yes** — done (`tests/test_absent.cpp`) '
               '| K | L2 | F:I2 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 1, out)
        self.assertIn('tests/test_absent.cpp, which is not in the tree', out)

    def test_ctest_listing_missing_a_claimed_kernel_row(self):
        row = ('| OL1 | a feature | **yes** — done '
               '(`tests/test_kernel_one.cpp`) | K | — | F:A1 | — |')
        listing = '  Test #1: test_kernel_three\n  Test #2: test_pure_c\n\nTotal Tests: 2\n'
        with tree(design(row), extra={'listing.txt': listing}) as made:
            code, out = made.run('--ctest-list', str(made.root / 'listing.txt'))
        self.assertEqual(code, 1, out)
        self.assertIn('kernel build did not register', out)
        self.assertIn('test_kernel_one.cpp', out)

    def test_ctest_listing_registering_a_row_the_walk_calls_source_bound(self):
        row = ('| OL1 | a feature | **yes** — done '
               '(`tests/test_kernel_one.cpp`) | K | — | F:A1 | — |')
        listing = ('  Test #1: test_kernel_one\n  Test #2: test_kernel_three\n'
                   '  Test #3: test_pure_c\n  Test #4: test_adapter_one\n'
                   '\nTotal Tests: 4\n')
        with tree(design(row), extra={'listing.txt': listing}) as made:
            code, out = made.run('--ctest-list', str(made.root / 'listing.txt'))
        self.assertEqual(code, 1, out)
        self.assertIn('the reach walk calls source-bound', out)
        self.assertIn('test_adapter_one', out)

    def test_included_registration_is_an_inventory_witness(self):
        row = '| AB | a feature | **no** (`tests/test_clock.cpp`) |'
        extra = {'tests/test_clock.cpp': KERNEL_TU, 'tests/test_display.cpp': KERNEL_TU,
                 'tests/python_row.cpp': KERNEL_TU,
                 'tests/clock.py': "from pathlib import Path\n"
                                   "assert Path(__file__).with_name('test_clock.cpp').is_file()\n",
                 'tests/lanes.d/clock.cmake': 'list(APPEND TEST_SOURCES test_clock)\n'
                                              'pineforge_add_unit_test(test_clock)\n'
                                              'if(PINEFORGE_BUILD_SOURCE_LAYER)\n'
                                              '    list(APPEND TEST_SOURCES test_display)\n'
                                              '    pineforge_add_unit_test(test_display)\nendif()\n'}
        with tree(design(row), cmake=CMAKE + INCLUDES, extra=extra) as made:
            code, out = made.run()
            self.assertEqual(code, 1, out)
            self.assertIn('row AB reads **no** while tests/test_clock.cpp runs', out)
            (made.root / 'tests/lanes.d/clock.cmake').write_text(
                'pineforge_add_unit_test(test_clock)\n'
                'if(PINEFORGE_BUILD_SOURCE_LAYER)\n'
                '    pineforge_add_unit_test(test_display)\nendif()\n'
                'add_test(NAME python_row COMMAND ${Python3_EXECUTABLE} '
                '${CMAKE_CURRENT_SOURCE_DIR}/clock.py)\n')
            (made.root / guard.DESIGN).write_text(design(row.replace('**no**', '**no** **Closed:**')))
            listing = made.root / 'listing.txt'
            listing.write_text('Test #1: test_kernel_one\nTest #2: test_kernel_three\n'
                               'Test #3: test_pure_c\nTest #4: python_row\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('kernel build did not register (test_clock.cpp)', out)
            with listing.open('a') as out_file:
                out_file.write('Test #5: test_clock\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 0, out)

    def test_ctest_lists_a_compiled_row_missing_from_the_inventory(self):
        row = '| AB | a feature | **yes** |'
        listing = ('Test #1: test_kernel_one\nTest #2: test_kernel_three\n'
                   'Test #3: test_pure_c\nTest #4: test_unmodelled\nTest #5: other_clock\n')
        extra = {'tests/lanes.d/clock.cmake':
                 'add_test(NAME other_clock COMMAND test_unmodelled --clock)\n',
                 'listing.txt': listing, 'tests/test_unmodelled.cpp': KERNEL_TU}
        with tree(design(row), cmake=CMAKE + INCLUDES, extra=extra) as made:
            code, out = made.run('--ctest-list', str(made.root / 'listing.txt'))
        self.assertEqual(code, 1, out)
        self.assertIn('test_unmodelled', out)
        self.assertIn('other_clock', out)
        self.assertIn('missing from the', out)

    def test_listing_checks_each_included_executable_alias(self):
        extra = {
            'tests/test_clock.cpp': KERNEL_TU,
            'tests/lanes.d/clock.cmake': (
                'list(APPEND TEST_SOURCES test_clock)\n'
                'add_test(NAME test_clock_first COMMAND test_clock --slice 1/2)\n'
                'add_test(NAME test_clock_second COMMAND test_clock --slice 2/2)\n'),
        }
        with tree(design('| AB | a feature | **yes** |'), cmake=CMAKE + INCLUDES,
                  extra=extra) as made:
            listing = made.root / 'listing.txt'
            listing.write_text('Test #1: test_kernel_one\nTest #2: test_kernel_three\n'
                               'Test #3: test_pure_c\nTest #4: test_clock_first\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('test_clock_second', out)
            with listing.open('a') as out_file:
                out_file.write('Test #5: test_clock_second\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 0, out)


class MustNotFire(unittest.TestCase):
    def test_gap_row_with_the_tables_own_closure_marker(self):
        row = ('| SZ3 | a feature | **no** — `HostSized` only. **Closed:** '
               '`Sized{EquityFraction}` is the sixth intent '
               '(`tests/test_kernel_one.cpp`) | K | L3 | F:C1 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_the_gap_waves_own_spelling_of_the_marker(self):
        row = ('| MG9 | a feature | **no** — no re-check. **Closed by audit lane '
               'N6:** the FX roll is a check point (`tests/test_kernel_two.cpp`, '
               '`tests/test_kernel_three.cpp`) | K | L4 | O:D8 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_gap_row_whose_only_witness_is_source_bound(self):
        row = ('| OL13 | a quirk | **no** — the adapter owns it '
               '(`tests/test_adapter_one.cpp`) | A | — | F:A15 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_transitive_reach_through_a_quoted_local_header(self):
        row = ('| OL13 | a quirk | **no** — the adapter owns it '
               '(`tests/test_via_local.cpp`) | A | — | F:A15 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_quirk_row_and_a_row_with_no_test(self):
        rows = ('| OL14 | a quirk | n/a | A | — | S:R4 | — |',
                '| OT1 | a feature | **partial** — staging only | K | — | F:J1 | — |')
        with tree(design(*rows)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_waiver_row_is_not_asked_for_a_witness(self):
        row = ('| PG | the adapter on the kernel grid | '
               '**measured-infeasible for the Pine adapter** — 30 pinned checks '
               'move | A | waived | — | **Waiver.** one rule for the run |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_pure_c_target_counts_as_kernel_profile(self):
        row = ('| OT6 | C order submission | **no** — none of the symbols. '
               '**Closed:** `strategy_native_submit_v1` '
               '(`tests/test_pure_c.c`) | K | L13 | F:J6 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)

    def test_removed_member_is_still_kernel_profile_through_its_own_target(self):
        # test_kernel_two is REMOVE_ITEM'd from TEST_SOURCES, so it is not a
        # registered row at all and cannot be demanded as a witness.
        row = ('| OL1 | a feature | **no** — nothing native '
               '(`tests/test_kernel_two.cpp`) | K | — | F:A1 | — |')
        with tree(design(row)) as made:
            code, out = made.run()
        self.assertEqual(code, 0, out)


class SelectedRegistrations(unittest.TestCase):
    def test_explicit_unit_registration_after_the_source_list_loop(self):
        cmake = UNIT_HELPER + """set(TEST_SOURCES test_kernel_one test_via_local test_adapter_one)
if(NOT PINEFORGE_BUILD_SOURCE_LAYER)
    set(_pf_source_bound_tests test_via_local test_adapter_one)
    if(_pf_source_bound_tests)
        list(REMOVE_ITEM TEST_SOURCES ${_pf_source_bound_tests})
    endif()
endif()
""" + UNIT_LOOP
        fragment = """list(APPEND TEST_SOURCES test_via_local)
pineforge_add_unit_test(test_via_local)
if(TARGET test_via_local)
    add_test(NAME read_clock COMMAND test_via_local --clock)
endif()
"""
        # The local header reaches the adapter. Registration alone cannot
        # make this file a source-free witness for the document.
        row = '| AB | clock | **no** (`tests/test_via_local.cpp`) |'
        primary = 'Test #1: test_kernel_one\nTest #2: test_via_local\n'
        header = '#ifdef WITH_ADAPTER\n' + LOCAL_FIXTURE + '#endif\n'
        with tree(design(row), cmake=cmake + INCLUDES, extra={
                'tests/lanes.d/clock.cmake': fragment,
                'tests/shared_fixture.hpp': header}) as made:
            listing = made.root / 'listing.txt'
            listing.write_text(primary + 'Test #3: read_clock\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 0, out)

            listing.write_text(primary)
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('kernel build did not register (test_via_local.cpp: read_clock)', out)

            # Appending a TU after the loop without calling the helper does
            # not register it. A supplied listing must still expose that row.
            (made.root / 'tests/lanes.d/clock.cmake').write_text(
                fragment + 'list(APPEND TEST_SOURCES test_kernel_two)\n')
            listing.write_text(primary + 'Test #3: read_clock\nTest #4: test_kernel_two\n'
                               'Test #5: test_adapter_one\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('compiled CTest rows missing from the kernel inventory: test_kernel_two', out)
            self.assertIn('the reach walk calls source-bound (test_adapter_one)', out)

            (made.root / guard.DESIGN).write_text(design(
                '| AB | clock | **no** **Closed:** (`tests/test_kernel_two.cpp`) |'))
            code, out = made.run()
            self.assertEqual(code, 1, out)
            self.assertIn('no kernel-profile test', out)

    def test_script_registration_uses_the_selected_module_provider(self):
        command = ('add_test(NAME clock_read COMMAND ${CMAKE_COMMAND} -E env '
                   '"CLOCK=$<TARGET_FILE:clock_plugin>" ${Python3_EXECUTABLE} '
                   '${CMAKE_CURRENT_SOURCE_DIR}/read_clock.py)\n')
        # The provider can be defined after add_test. Its sources are not a
        # test executable, and the library has no CTest row of its own.
        provider = 'add_library(clock_plugin MODULE clock_plugin.cpp test_clock_step.cpp)\n'
        row = '| AB | clock | **no** (`tests/test_clock_step.cpp`) |'
        cmake = UNIT_HELPER + 'set(TEST_SOURCES test_kernel_one)\n' + UNIT_LOOP
        baseline = 'Test #1: test_kernel_one\n'
        with tree(design(row), cmake=cmake + command + provider, extra={
                'tests/clock_plugin.cpp': 'int clock_step();\n'
                                          'extern "C" int clock_read() { return 16 + clock_step(); }\n',
                'tests/test_clock_step.cpp': 'int clock_step() { return 1; }\n',
                'tests/read_clock.py': 'import ctypes, os\n'
                                       'assert ctypes.CDLL(os.environ["CLOCK"]).clock_read() == 17\n'
        }) as made:
            listing = made.root / 'listing.txt'
            listing.write_text(baseline + 'Test #2: clock_read\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 0, out)

            listing.write_text(baseline)
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('kernel build did not register (clock_plugin.cpp: clock_read)', out)

            (made.root / guard.TESTS_CMAKE).write_text(
                cmake + 'if(PINEFORGE_BUILD_SOURCE_LAYER)\n' + command + provider + 'endif()\n')
            listing.write_text(baseline + 'Test #2: clock_read\n')
            code, out = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 1, out)
            self.assertIn('compiled CTest rows missing from the kernel inventory: clock_read', out)

            (made.root / guard.TESTS_CMAKE).write_text(cmake + command + 'if(UNKNOWN)\n'
                                                      + provider + 'endif()\n')
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code, _ = made.run('--ctest-list', str(listing))
            self.assertEqual(code, 2)
            self.assertIn('NOT COMPARED:', err.getvalue())
            self.assertIn('UNKNOWN', err.getvalue())

            for bad_provider in (provider.replace('test_clock_step.cpp', 'missing.cpp'),
                                 'if(OFF)\n' + provider + 'endif()\n'):
                with self.subTest(provider=bad_provider):
                    (made.root / guard.TESTS_CMAKE).write_text(cmake + command + bad_provider)
                    err = io.StringIO()
                    with contextlib.redirect_stderr(err):
                        code, _ = made.run('--ctest-list', str(listing))
                    self.assertEqual(code, 2)
                    self.assertIn('NOT COMPARED:', err.getvalue())
                    self.assertIn('clock_plugin', err.getvalue())


class Shape(unittest.TestCase):
    def test_missing_section_exits_two(self):
        with tree('# a document with no inventory\n') as made:
            code, _ = made.run()
        self.assertEqual(code, 2)

    def test_no_rows_exits_two(self):
        with tree(HEAD + TAIL) as made:
            code, _ = made.run()
        self.assertEqual(code, 2)

    def test_no_kernel_profile_tu_exits_two(self):
        cmake = 'set(TEST_SOURCES\n    test_adapter_one\n)\n'
        row = '| OL1 | a feature | **yes** — done | K | — | F:A1 | — |'
        with tree(design(row), cmake=cmake) as made:
            code, _ = made.run()
        self.assertEqual(code, 2)

    def test_unreadable_ctest_listing_exits_two(self):
        row = '| OL1 | a feature | **yes** — done | K | — | F:A1 | — |'
        with tree(design(row), extra={'listing.txt': 'no rows here\n'}) as made:
            code, _ = made.run('--ctest-list', str(made.root / 'listing.txt'))
        self.assertEqual(code, 2)

    def test_list_kernel_tests_prints_the_set(self):
        row = '| OL1 | a feature | **yes** — done | K | — | F:A1 | — |'
        with tree(design(row)) as made:
            code, out = made.run('--list-kernel-tests')
        self.assertEqual(code, 0, out)
        self.assertIn('test_kernel_one.cpp', out)
        self.assertIn('test_pure_c.c', out)
        self.assertNotIn('test_adapter_one.cpp', out)
        self.assertNotIn('test_via_local.cpp', out)


class RealTree(unittest.TestCase):
    """The guard must pass on this repository, and see a real population."""

    def test_repository_is_consistent(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = guard.main([])
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn('findings', out.getvalue())

    def test_repository_has_a_plausible_kernel_set(self):
        kernel = guard.kernel_profile_tests(guard.ROOT)
        self.assertGreater(len(kernel), 80, 'the reach walk found almost nothing')
        self.assertIn('test_native_report_truth.cpp', kernel)
        self.assertIn('test_native_c_api.c', kernel)
        self.assertNotIn('test_adapter_risk_relower.cpp', kernel)


if __name__ == '__main__':
    unittest.main(verbosity=2)
