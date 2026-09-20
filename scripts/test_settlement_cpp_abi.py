#!/usr/bin/env python3
"""Mutation controls for real C++ ABI acceptance/rejection pair handling."""
from __future__ import annotations

from pathlib import Path
import shutil
import tempfile
import unittest

import check_settlement_cpp_abi as checker
from cpp_abi_pairing import PairingError, run_synthetic_pair

ROOT = Path(__file__).resolve().parents[1]


class SettlementAbi(unittest.TestCase):
    def test_current_include_tree(self):
        result = checker.verify(ROOT / "include")
        self.assertEqual(result["transition"]["to"], "engine_script_run_v18")

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

    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        self.assertEqual(run_synthetic_pair(compiler), ["accept", "reject"])

    def test_unknown_pair_kind_is_refused(self) -> None:
        from cpp_abi_pairing import _source
        with self.assertRaises(PairingError):
            _source("engine_script_run_v18", "unknown")

    def test_missing_tick_hook_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            include = root / "include"
            shutil.copytree(ROOT / "include", include)
            path = include / "pineforge/native_host.hpp"
            path.write_text(path.read_text().replace(
                "virtual void on_native_tick", "virtual void missing_tick_hook", 1))
            with self.assertRaises(RuntimeError):
                checker.verify(include)


if __name__ == "__main__":
    unittest.main()
