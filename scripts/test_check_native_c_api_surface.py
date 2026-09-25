#!/usr/bin/env python3
"""Self-tests for the N8 C-surface guard: it must be able to FAIL.

A completeness guard that cannot fail is decoration. The first cases are the
ways the COVERAGE block in native_c_api.h can drift from the public surface
of NativeStrategyHost; the rest (R5 lane E15) are the ways its BASE-CLASS
SEAMS list can drift from the BacktestEngine members engine.hpp marks
`@host-seam`. Each is driven against copies of the real headers, so the guard
is exercised exactly as CI runs it.
"""
from __future__ import annotations

import contextlib
import io
import shutil
import tempfile
import unittest
from pathlib import Path

import check_native_c_api_surface as guard

ROOT = Path(__file__).resolve().parent.parent
HOST_TEXT = guard.HOST.read_text(encoding="utf-8")
C_API_TEXT = guard.C_API.read_text(encoding="utf-8")
ENGINE_TEXT = (ROOT / "include" / "pineforge" / "engine.hpp").read_text(encoding="utf-8")

MARK = "    // @host-seam (native_c_api.h, COVERAGE / BASE-CLASS SEAMS)\n"
SEAM_DECLARATION = "    void declare_opened_lot_entry_bar_mask("
SEAM_ROW = (
    " *   [C]  declare_opened_lot_entry_bar_mask strategy_native_declare_opened_lot_entry_bar_mask_v1"
    " -- legal\n"
    " *                                          inside on_applied alone; executed by the entry-bar mask\n"
    " *                                          scenario of tests/test_native_c_api.c\n")


class SurfaceGuardTests(unittest.TestCase):
    def setUp(self) -> None:
        self._saved = (guard.HOST, guard.C_API, getattr(guard, "ENGINE", None))
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.addCleanup(lambda: setattr(guard, "HOST", self._saved[0]))
        self.addCleanup(lambda: setattr(guard, "C_API", self._saved[1]))
        self.addCleanup(lambda: setattr(guard, "ENGINE", self._saved[2]))
        root = Path(directory.name)
        self.host = root / "native_host.hpp"
        self.c_api = root / "native_c_api.h"
        self.engine = root / "engine.hpp"
        self.host.write_text(HOST_TEXT, encoding="utf-8")
        self.c_api.write_text(C_API_TEXT, encoding="utf-8")
        self.engine.write_text(ENGINE_TEXT, encoding="utf-8")
        guard.HOST = self.host
        guard.C_API = self.c_api
        guard.ENGINE = self.engine

    def run_guard(self) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = guard.main()
        return code, err.getvalue()

    def test_the_real_tree_passes(self) -> None:
        code, err = self.run_guard()
        self.assertEqual(code, 0, err)

    def test_a_new_host_member_without_a_row_fails(self) -> None:
        self.host.write_text(
            HOST_TEXT.replace("    NativeStrategyHost();",
                              "    NativeStrategyHost();\n    int brand_new_capability() const;",
                              1),
            encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("brand_new_capability", err)

    def test_a_deleted_row_fails(self) -> None:
        self.c_api.write_text(
            C_API_TEXT.replace(" *   [C]  cancel_where", " *   [x]  cancel_where", 1),
            encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("cancel_where", err)

    def test_a_spelling_that_names_nothing_fails(self) -> None:
        self.c_api.write_text(
            C_API_TEXT.replace("strategy_native_cancel_where_v1",
                               "strategy_native_not_a_symbol_v1", 1),
            encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("names no declared symbol", err)

    def test_a_row_for_a_member_that_no_longer_exists_fails(self) -> None:
        self.c_api.write_text(
            C_API_TEXT.replace(
                " *   [C]  cancel_all  ",
                " *   [C]  cancel_gone                          strategy_native_cancel_all_v1\n"
                " *   [C]  cancel_all  ", 1),
            encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("cancel_gone", err)

    # --- the 1.0 C boundary (R5 lane H-DOCGATES) ---
    # Each case adds the spelling a 1.1.0 lane would most likely give a
    # capability the 1.0 C surface lacks. A pin that caught only the plain
    # spelling let the first four through (AUDIT4 cparity mutants M1, M3, M4
    # and M5): the gap would close and the boundary table would still list it.

    def assert_boundary_fails(self, key: str, anchor: str, addition: str) -> None:
        self.assertEqual(C_API_TEXT.count(anchor), 1, anchor)
        self.c_api.write_text(C_API_TEXT.replace(anchor, anchor + addition, 1), encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1, err)
        self.assertIn(f'C_V1_EXCLUSIONS["{key}"]', err)
        self.assertIn("is now declared", err)

    def test_a_suffixed_entry_comment_accessor_closes_the_gap(self) -> None:
        self.assert_boundary_fails(
            "closed_entry_comment", "PF_API int strategy_native_submit_v1(",
            "\n/* x */ const char* strategy_closed_trade_entry_comment_v1(pf_strategy_t s, int i);\n"
            "PF_API int strategy_native_submit_v1(")

    def test_a_replace_call_with_options_closes_the_gap(self) -> None:
        self.assert_boundary_fails(
            "replace_options", "PF_API int strategy_native_submit_v1(",
            "\nPF_API int strategy_native_replace_opts_v1(pf_strategy_t s, uint64_t incarnation,"
            " uint32_t keep_handle);\nPF_API int strategy_native_submit_v1(")

    def test_a_bracket_call_closes_the_gap(self) -> None:
        self.assert_boundary_fails(
            "toolkit", "PF_API int strategy_native_submit_v1(",
            "\nPF_API int strategy_native_submit_bracket_v1(pf_strategy_t s);\n"
            "PF_API int strategy_native_submit_v1(")

    def test_a_prefixed_applied_tail_closes_the_gap(self) -> None:
        self.assert_boundary_fails(
            "applied_event_tail",
            "    uint64_t opened_lot_incarnation; /**< The lot this fill opened, 0 when none. */",
            "\n    uint32_t request_origin;\n    const char* request_label;")

    def test_an_applied_struct_beside_v1_closes_the_gap(self) -> None:
        self.assert_boundary_fails(
            "applied_event_tail", "PF_API int strategy_native_submit_v1(",
            "\ntypedef struct pf_native_applied_ext_v1 { uint32_t origin; } "
            "pf_native_applied_ext_v1;\nPF_API int strategy_native_submit_v1(")

    def test_a_decision_input_interval_closes_the_gap(self) -> None:
        self.assert_boundary_fails(
            "callback_contexts", "    uint8_t  closes_session_day; /**< It closes its session day. */",
            "\n    int64_t input_interval_open_ms;")

    def test_a_cpp_capability_that_goes_retires_its_row(self) -> None:
        saved = guard.INCLUDE
        self.addCleanup(setattr, guard, "INCLUDE", saved)
        copy = Path(self.c_api.parent, "include")
        shutil.copytree(saved, copy)
        order = copy / "native_order.hpp"
        text = order.read_text(encoding="utf-8")
        self.assertEqual(text.count("struct ReplaceOptions"), 1)
        order.write_text(text.replace("struct ReplaceOptions", "struct ReplaceOptionsRenamed", 1),
                         encoding="utf-8")
        guard.INCLUDE = copy
        code, err = self.run_guard()
        self.assertEqual(code, 1, err)
        self.assertIn('C_V1_EXCLUSIONS["replace_options"]: the C++ declaration', err)

    def page_with(self, old: str, new: str) -> None:
        saved = guard.DOC_C_BOUNDARY
        self.addCleanup(setattr, guard, "DOC_C_BOUNDARY", saved)
        page = Path(self.c_api.parent, "native-engine.md")
        text = saved.read_text(encoding="utf-8")
        self.assertEqual(text.count(old), 1, old)
        page.write_text(text.replace(old, new, 1), encoding="utf-8")
        guard.DOC_C_BOUNDARY = page

    def test_a_boundary_row_cannot_disappear(self) -> None:
        self.page_with('`C_V1_EXCLUSIONS["replace_options"]`', "(gone)")
        code, err = self.run_guard()
        self.assertEqual(code, 1, err)
        self.assertIn('C_V1_EXCLUSIONS["replace_options"]: the boundary table', err)
        self.assertIn("cites it 0 times", err)

    def test_a_boundary_row_cannot_be_doubled(self) -> None:
        self.page_with('`C_V1_EXCLUSIONS["toolkit"]` |',
                       '`C_V1_EXCLUSIONS["toolkit"]`, `C_V1_EXCLUSIONS["toolkit"]` |')
        code, err = self.run_guard()
        self.assertEqual(code, 1, err)
        self.assertIn("cites it 2 times", err)

    def test_the_page_cannot_cite_a_row_the_checker_lacks(self) -> None:
        self.page_with('`C_V1_EXCLUSIONS["toolkit"]` |',
                       '`C_V1_EXCLUSIONS["toolkit"]`, `C_V1_EXCLUSIONS["made_up"]` |')
        code, err = self.run_guard()
        self.assertEqual(code, 1, err)
        self.assertIn('C_V1_EXCLUSIONS["made_up"]: the boundary table cites a row', err)

    # --- the BASE-CLASS SEAMS census (R5 lane E15) ---

    def test_a_marked_seam_without_a_row_fails(self) -> None:
        declaration = "    double marked_equity(double price) const;"
        self.assertIn(declaration, ENGINE_TEXT)
        self.engine.write_text(
            ENGINE_TEXT.replace(declaration, MARK + declaration, 1), encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("marked BacktestEngine seams with neither a C spelling nor an exclusion: "
                      "marked_equity", err)

    def test_a_seam_row_for_an_unmarked_member_fails(self) -> None:
        self.assertIn(MARK + SEAM_DECLARATION, ENGINE_TEXT)
        self.engine.write_text(
            ENGINE_TEXT.replace(MARK + SEAM_DECLARATION, SEAM_DECLARATION, 1), encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("BASE-CLASS SEAMS rows naming members engine.hpp does not mark "
                      "@host-seam: declare_opened_lot_entry_bar_mask", err)

    def test_a_seam_spelling_that_names_nothing_fails(self) -> None:
        self.assertIn(SEAM_ROW, C_API_TEXT)
        self.c_api.write_text(
            C_API_TEXT.replace(SEAM_ROW, SEAM_ROW.replace(
                "strategy_native_declare_opened_lot_entry_bar_mask_v1",
                "strategy_native_not_a_symbol_v1"), 1),
            encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("declare_opened_lot_entry_bar_mask: the C spelling", err)
        self.assertIn("names no declared symbol", err)

    def test_a_seam_row_among_the_host_rows_fails(self) -> None:
        # Lane E11's shape: a base-class member listed with NativeStrategyHost's
        # own members is a stale host row AND a marked seam without a row.
        self.assertIn(SEAM_ROW, C_API_TEXT)
        moved = C_API_TEXT.replace(SEAM_ROW, "", 1).replace(
            " *   [C]  on_bar  ", SEAM_ROW + " *   [C]  on_bar  ", 1)
        self.c_api.write_text(moved, encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("COVERAGE rows naming members that no longer exist: "
                      "declare_opened_lot_entry_bar_mask", err)
        self.assertIn("marked BacktestEngine seams with neither a C spelling nor an exclusion: "
                      "declare_opened_lot_entry_bar_mask", err)

    def test_a_marker_outside_the_class_fails(self) -> None:
        self.engine.write_text(
            ENGINE_TEXT.replace("class BacktestEngine {",
                                "// @host-seam\nclass BacktestEngine {", 1),
            encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("outside class BacktestEngine", err)

    def test_a_marker_on_a_data_member_fails(self) -> None:
        member = "    LotExcursionHook lot_excursion_hook_;"
        self.assertIn(member, ENGINE_TEXT)
        self.engine.write_text(ENGINE_TEXT.replace(member, MARK + member, 1), encoding="utf-8")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("is not followed by a member function declaration", err)



class EnumTwinGuardTests(unittest.TestCase):
    """R5 lane F4 (audit P4-b): every kernel enumeration a C word carries is
    parsed against its C twin, so an enumerator appended to the kernel without
    a C name fails -- the audit's R12 mutation compiled clean against the old
    last-value static_assert."""

    def setUp(self) -> None:
        self._saved = {name: getattr(guard, name)
                       for name in ("INCLUDE", "HOST", "C_API", "PUBLIC_C", "ENGINE")}
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        for name, value in self._saved.items():
            self.addCleanup(setattr, guard, name, value)
        self.include = Path(directory.name) / "pineforge"
        shutil.copytree(guard.INCLUDE, self.include)
        guard.INCLUDE = self.include
        guard.HOST = self.include / "native_host.hpp"
        guard.C_API = self.include / "native_c_api.h"
        guard.PUBLIC_C = self.include / "pineforge.h"
        guard.ENGINE = self.include / "engine.hpp"

    def edit(self, name: str, old: str, new: str) -> None:
        path = self.include / name
        text = path.read_text(encoding="utf-8")
        self.assertEqual(text.count(old), 1, f"{old!r} is not unique in {name}")
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def run_guard(self) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = guard.main()
        return code, err.getvalue()

    def test_the_copied_tree_passes(self) -> None:
        code, err = self.run_guard()
        self.assertEqual(code, 0, err)

    def test_an_appended_kernel_enumerator_without_a_c_name_fails(self) -> None:
        # The audit's R12 mutation: a kind appended after the last one (FxRoll
        # then, IntrabarSample since R5 lane PAR-MARGIN).
        self.edit("native_host.hpp", "    IntrabarSample = 4,\n};",
                  "    IntrabarSample = 4,\n    Unnamed = 5,\n};")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("NativeMarginCheckKind::Unnamed (5) has no C name in "
                      "pf_native_margin_check_kind_e", err)

    def test_an_implicit_enumerator_appended_without_a_c_name_fails(self) -> None:
        self.edit("native_order.hpp", "    TrailTrigger = 3,\n};", "    TrailTrigger = 3,\n    Later,\n};")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("ActivationKind::Later (4) has no C name in pf_native_activation_e", err)

    def test_an_inserted_kernel_enumerator_fails(self) -> None:
        # Inserted before the last two: the C names now spell shifted values,
        # and the last one's has no C name at all.
        self.edit("native_host.hpp", "    Calculation = 2,\n    FxRoll = 3,\n    IntrabarSample = 4,",
                  "    Calculation = 2,\n    Inserted = 3,\n    FxRoll = 4,\n    IntrabarSample = 5,")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("NativeMarginCheckKind::IntrabarSample (5) has no C name", err)

    def test_a_c_name_for_no_kernel_value_fails(self) -> None:
        self.edit("native_c_api.h", "    PF_NATIVE_ORIGIN_KERNEL_RISK        = 2  ",
                  "    PF_NATIVE_ORIGIN_KERNEL_RISK        = 2, PF_NATIVE_ORIGIN_BOGUS = 9  ")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("PF_NATIVE_ORIGIN_BOGUS (9) names no enumerator of RequestOrigin", err)

    def test_an_excluded_kernel_value_given_a_c_name_fails(self) -> None:
        self.edit("native_c_api.h", "    PF_NATIVE_REPORT_KERNEL_RECORDED = 1  ",
                  "    PF_NATIVE_REPORT_KERNEL_RECORDED = 1,\n    PF_NATIVE_REPORT_MARKS = 2  ")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("NativeReportPolicy::KernelRecordedAtHostMarks is excluded", err)

    def test_an_unclassified_c_enumeration_fails(self) -> None:
        self.edit("native_c_api.h", "/** @} */ /* end of pf_native_c_enums */",
                  "typedef enum pf_native_bogus_e { PF_NATIVE_BOGUS_A = 0 } pf_native_bogus_t;\n"
                  "/** @} */ /* end of pf_native_c_enums */")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("pf_native_bogus_e is neither twinned to a kernel enumeration nor "
                      "ruled C-only", err)

    def test_a_twin_whose_kernel_enumeration_is_gone_fails(self) -> None:
        self.edit("native_order.hpp", "enum class RequestOrigin", "enum class RequestProvenance")
        code, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("no `enum class RequestOrigin` in native_order.hpp", err)


if __name__ == "__main__":
    unittest.main()
