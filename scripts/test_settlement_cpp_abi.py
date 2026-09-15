#!/usr/bin/env python3
"""Offline mutation controls for the v16-to-v17 ABI manifest guard."""
from pathlib import Path
import shutil
import tempfile
import unittest

import check_settlement_cpp_abi as checker

ROOT = Path(__file__).resolve().parents[1]


class SettlementAbi(unittest.TestCase):
    def test_current_include_tree(self):
        result = checker.verify(ROOT / "include")
        self.assertEqual(result["transition"]["to"], "engine_script_run_v17")

    def test_retired_header_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            include = root / "include"
            shutil.copytree(ROOT / "include", include)
            path = include / checker.RETIRED_HEADER
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n")
            with self.assertRaises(RuntimeError):
                checker.verify(include)

    def test_missing_hook_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            include = root / "include"
            shutil.copytree(ROOT / "include", include)
            path = include / "pineforge/native_host.hpp"
            path.write_text(path.read_text().replace(
                "virtual void prepare_native_begin", "virtual void missing_begin_hook", 1))
            with self.assertRaises(RuntimeError):
                checker.verify(include)


if __name__ == "__main__":
    unittest.main()
