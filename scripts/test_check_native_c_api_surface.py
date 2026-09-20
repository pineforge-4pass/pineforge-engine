#!/usr/bin/env python3
"""Self-tests for the N8 C-surface guard: it must be able to FAIL.

A completeness guard that cannot fail is decoration. These four cases are the
four ways the COVERAGE block in native_c_api.h can drift from the public
surface of NativeStrategyHost, each driven against copies of the real headers
so the guard is exercised exactly as CI runs it.
"""
from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import check_native_c_api_surface as guard

HOST_TEXT = guard.HOST.read_text(encoding="utf-8")
C_API_TEXT = guard.C_API.read_text(encoding="utf-8")


class SurfaceGuardTests(unittest.TestCase):
    def setUp(self) -> None:
        self._saved = (guard.HOST, guard.C_API)
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.addCleanup(lambda: setattr(guard, "HOST", self._saved[0]))
        self.addCleanup(lambda: setattr(guard, "C_API", self._saved[1]))
        root = Path(directory.name)
        self.host = root / "native_host.hpp"
        self.c_api = root / "native_c_api.h"
        self.host.write_text(HOST_TEXT, encoding="utf-8")
        self.c_api.write_text(C_API_TEXT, encoding="utf-8")
        guard.HOST = self.host
        guard.C_API = self.c_api

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


if __name__ == "__main__":
    unittest.main()
