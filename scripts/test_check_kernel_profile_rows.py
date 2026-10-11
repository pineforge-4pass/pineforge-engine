#!/usr/bin/env python3
"""Controls for check_kernel_profile_rows.py, over the real tests/CMakeLists.txt
and over tampered copies: the sweep must close on the tree, fail loudly on a
contract it cannot read, and fail on every class it guards.

States exercised (each one asserts the exit code and the file:line message):
* GREEN, the real tree: 0 violations, the predicate list read by every guarded
  private block, the pinned per-profile counts derived, the 10 preparer/row
  pairs agreeing.
* RED, a source-layer row re-added to the kernel profile: the guard removed
  from one source-bound block -> exit 1 naming the block.
* RED, a source-bound row behind an if(EXISTS) block that ignores the predicate
  list, while its pin-list entry carries the list's closing paren (the shape
  that linked the kernel profile against the kernel-only archive): the block
  reverted to its pre-fix form -> exit 1 naming the row.
* RED, a pair disagreement: a receipt row's guard naming the wrong provider
  manifest -> exit 1 naming both lists.
* LOUD-FAIL, a construct the walk cannot read (an unterminated if(EXISTS)) ->
  exit 2 with file:line.
"""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import check_kernel_profile_rows as sweep

REAL = sweep.REPO / 'tests' / 'CMakeLists.txt'

DAY_KEY_GUARD = ('if(NOT test_native_oracle_day_key_l2 IN_LIST '
                 '_pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}'
                 '/oracle/test_oracle_day_key.cpp")')
COOF_GUARD = ('if(NOT test_adapter_coof_relower IN_LIST _pf_kernel_absent_rows '
              'AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnifier_intrabars'
              '/mi-ltf-f-1D/meta.json")')


def run_sweep(source: Path) -> tuple[int, str]:
    import contextlib
    import io
    out = io.StringIO()
    argv = ['check_kernel_profile_rows.py', '--cmakelists', str(source)]
    import sys
    saved = sys.argv
    sys.argv = argv
    try:
        with contextlib.redirect_stdout(out):
            code = sweep.main()
    finally:
        sys.argv = saved
    return code, out.getvalue()


class KernelProfileRows(unittest.TestCase):
    def copy(self, temporary: str) -> Path:
        target = Path(temporary) / 'CMakeLists.txt'
        target.write_text(REAL.read_text())
        return target

    def test_green_the_real_tree_closes(self):
        code, out = run_sweep(REAL)
        self.assertEqual(code, 0, out)
        self.assertIn('kernel targets linking without their prerequisites: 0', out)
        self.assertIn('blocks not reading the predicate list: 0', out)
        self.assertIn('preparer/row pairs checked: 10', out)
        self.assertNotIn('kernel-absent 0 ', out)

    def test_red_a_source_layer_row_re_added_to_kernel_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            self.assertEqual(text.count(DAY_KEY_GUARD), 1)
            target.write_text(text.replace(DAY_KEY_GUARD, 'if(EXISTS "'
                '${CMAKE_CURRENT_SOURCE_DIR}/oracle/test_oracle_day_key.cpp")'))
            code, out = run_sweep(target)
        self.assertEqual(code, 1, out)
        self.assertIn('test_native_oracle_day_key_l2 without reading '
                      '_pf_kernel_absent_rows', out)

    def test_red_a_paren_pinned_row_behind_an_unread_guard_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            fixed = ('if(NOT test_zero_trail_sibling_stop IN_LIST '
                     '_pf_kernel_absent_rows AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}'
                     '/test_zero_trail_sibling_stop.cpp")\n'
                     '    pineforge_add_unit_test(test_zero_trail_sibling_stop)\n'
                     'elseif(NOT test_zero_trail_sibling_stop IN_LIST '
                     '_pf_kernel_absent_rows)')
            self.assertEqual(text.count(fixed), 1)
            target.write_text(text.replace(fixed,
                'if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}'
                '/test_zero_trail_sibling_stop.cpp")\n'
                '    pineforge_add_unit_test(test_zero_trail_sibling_stop)\n'
                'else()'))
            code, out = run_sweep(target)
        self.assertEqual(code, 1, out)
        self.assertIn('test_zero_trail_sibling_stop without reading '
                      '_pf_kernel_absent_rows', out)

    def test_red_a_missing_placeholder_guard_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            marker = 'elseif(NOT test_adapter_coof_relower IN_LIST _pf_kernel_absent_rows)'
            self.assertIn(marker, text)
            target.write_text(text.replace(marker, 'else()'))
            code, out = run_sweep(target)
        self.assertEqual(code, 1, out)
        self.assertIn('the placeholder branch does not read _pf_kernel_absent_rows', out)

    def test_red_a_pair_disagreement_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            guard = ('if(EXISTS ${_pf_manifest_v18_frozen})\n'
                     '        add_test(\n'
                     '            NAME test_script_cpp_abi')
            self.assertEqual(text.count(guard), 1)
            target.write_text(text.replace(guard, guard.replace('v18', 'v16'), 1))
            code, out = run_sweep(target)
        self.assertEqual(code, 1, out)
        self.assertIn('test_script_cpp_abi: the guard names', out)
        self.assertIn('the preparer gives', out)

    def test_loud_fail_an_unterminated_block_exits_two(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            self.assertEqual(text.count(COOF_GUARD), 1)
            text = text.replace(COOF_GUARD, 'if(EXISTS "'
                '${CMAKE_CURRENT_SOURCE_DIR}/fixtures/magnifier_intrabars'
                '/mi-ltf-f-1D/meta.json")', 1)
            start = text.index('if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures'
                               '/magnifier_intrabars/mi-ltf-f-1D/meta.json")')
            end = text.index('\nendif()', start)
            target.write_text(text[:end] + text[end + len('\nendif()'):])
            code, out = run_sweep(target)
        self.assertEqual(code, 2, out)
        self.assertIn('if(EXISTS with no matching endif()', out)

    def test_red_a_pinned_row_no_guarded_block_registers_fails(self):
        # The pin names the row, the block that used to register it no longer
        # does: a bare registration (or none) would build against the
        # kernel-only archive, which is the gap the docstring promises to fail.
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            marker = '    pineforge_add_unit_test(test_native_oracle_day_key_l2)'
            self.assertIn(marker, text)
            target.write_text(text.replace(marker, '    message(STATUS "dropped")', 1))
            code, out = run_sweep(target)
        self.assertEqual(code, 1, out)
        self.assertIn('the private-row pin names test_native_oracle_day_key_l2, '
                      'and no guarded private block registers it', out)

    def test_a_missing_tu_is_counted_in_neither_profile(self):
        # A TU the tree does not have says nothing about the row: it is
        # named, and it is in neither the kernel nor the release count.
        # The missing-file case is built in a tampered tree (real directories
        # under TMPDIR), so the control does not change with the world: in the
        # overlay world the real tree HAS this TU, and only the real tree's
        # own answer (below) is what the world decides.
        code, out = run_sweep(REAL)
        self.assertEqual(code, 0, out)
        self.assertIn('whose TU the tree does not have (counted in neither profile)', out)
        with tempfile.TemporaryDirectory() as temporary:
            entry = Path(temporary) / 'tests' / 'test_trail_activation_tick_reach.cpp'
            entry.parent.mkdir()
            self.assertEqual('missing', sweep.source_bound(
                entry, 'test_trail_activation_tick_reach', set()))
            self.assertEqual('bound', sweep.source_bound(
                entry, 'test_trail_activation_tick_reach',
                {'test_trail_activation_tick_reach'}))
            entry.write_text('// e2e-x4: the TU exists and records nothing\n')
            self.assertEqual('clear', sweep.source_bound(
                entry, 'test_trail_activation_tick_reach', set()))
        real_tu = sweep.REPO / 'tests' / 'test_trail_activation_tick_reach.cpp'
        self.assertEqual('missing' if not real_tu.is_file() else 'bound',
                         sweep.source_bound(real_tu,
                                            'test_trail_activation_tick_reach',
                                            set()))

    def test_loud_fail_a_lost_predicate_definition_exits(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = self.copy(temporary)
            text = target.read_text()
            marker = 'set(_pf_kernel_absent_rows)'
            self.assertIn(marker, text)
            target.write_text(text.replace(marker, 'set(_pf_nothing)', 1))
            code, out = run_sweep(target)
        self.assertEqual(code, 1, out)
        self.assertIn('_pf_kernel_absent_rows is never set', out)


if __name__ == '__main__':
    unittest.main()
