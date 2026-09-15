#!/usr/bin/env python3
"""Mutation controls for the aggregate v17 ownership and ABI pair guards."""
from __future__ import annotations

from pathlib import Path
import shutil
import tempfile
import unittest

import check_aggregate_cpp_versions as checker
from cpp_abi_pairing import run_synthetic_pair


ROOT = Path(__file__).resolve().parents[1]


class AggregateVersions(unittest.TestCase):
    def copied_root(self) -> Path:
        directory = tempfile.TemporaryDirectory(prefix="pf-aggregate-versions-")
        self.addCleanup(directory.cleanup)
        root = Path(directory.name) / "repo"
        shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
            "build*", ".git", "corpus", "*.so", "*.a", ".native-fx-introduced-*"))
        return root

    def test_current_tree(self) -> None:
        checker.check(ROOT)

    def test_epoch_mutation_rejects(self) -> None:
        root = self.copied_root()
        path = root / "include/pineforge/engine.hpp"
        path.write_text(path.read_text().replace("engine_script_run_v17", "engine_script_run_v0", 1))
        with self.assertRaises(ValueError):
            checker.check(root)

    def test_stream_fold_mutation_rejects(self) -> None:
        root = self.copied_root()
        path = root / "src/engine_stream.cpp"
        path.write_text(path.read_text().replace(
            "integer(17); integer(broker_state_hash());",
            "if (false) { integer(17); integer(broker_state_hash()); }", 1))
        with self.assertRaises(ValueError):
            checker.check(root)

    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        self.assertEqual(run_synthetic_pair(compiler), ["accept", "reject"])


if __name__ == "__main__":
    unittest.main()
