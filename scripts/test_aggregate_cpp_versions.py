#!/usr/bin/env python3
"""Mutation controls for the aggregate C++ ownership guard."""
from pathlib import Path
import shutil
import tempfile
import unittest

import check_aggregate_cpp_versions as checker

ROOT = Path(__file__).resolve().parents[1]


class Versions(unittest.TestCase):
    def test_current_tree(self):
        checker.check(ROOT)

    def test_epoch_drift_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
                "build*", ".git", "corpus", "*.so", "*.a"))
            path = root / "include/pineforge/engine.hpp"
            path.write_text(path.read_text().replace("engine_script_run_v17", "engine_script_run_v0", 1))
            with self.assertRaises(ValueError):
                checker.check(root)

    def test_retired_header_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
                "build*", ".git", "corpus", "*.so", "*.a"))
            path = root / "include/pineforge/source/pine_pending_intent.hpp"
            path.write_text("#pragma once\n")
            with self.assertRaises(ValueError):
                checker.check(root)


if __name__ == "__main__":
    unittest.main()
