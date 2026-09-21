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

HEAD = """## 1. Inventory

Columns: **Feature** · **Native today** · **Class** · **Lane** · **Sources** · **Notes**

### 1.1 Things

| ID | Feature | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
"""

TAIL = '\n## 2. Structural work\n\nnothing here.\n'


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
