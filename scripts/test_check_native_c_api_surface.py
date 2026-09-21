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


if __name__ == "__main__":
    unittest.main()
