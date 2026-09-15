#!/usr/bin/env python3
"""Mutation controls for generic and source-adapter hash coverage."""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import shutil
import tempfile
import unittest

import check_broker_state_hash_coverage as checker

ROOT = Path(__file__).resolve().parents[1]


class Coverage(unittest.TestCase):
    def check(self, mutations=()):
        with tempfile.TemporaryDirectory(prefix="pf-hash-coverage-") as directory:
            root = Path(directory) / "repo"
            shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
                "build*", ".git", "corpus", "*.so", "*.a",
                # The ABI checker creates this short-lived root sentinel
                # while CTest runs guards in parallel. It is not source
                # input to this isolated mutation clone.
                ".native-fx-introduced-*"))
            for relative, before, after in mutations:
                path = root / relative
                text = path.read_text()
                self.assertIn(before, text)
                path.write_text(text.replace(before, after, 1))
            output = StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                result = checker.main(root)
            return result, output.getvalue()

    def test_current_tree_passes(self):
        self.assertEqual(self.check()[0], 0)

    def test_generic_domain_is_pinned(self):
        result, output = self.check((
            ("src/engine_state_hash.cpp", "pineforge-broker-state/v17",
             "pineforge-broker-state/v0"),))
        self.assertEqual(result, 1, output)

    def test_source_domain_is_pinned(self):
        result, output = self.check((
            ("include/pineforge/source/pine_adapter.hpp",
             "pineforge-source-adapter/v2", "pineforge-source-adapter/v0"),))
        self.assertEqual(result, 1, output)

    def test_adapter_fold_is_required(self):
        result, output = self.check((
            ("src/source/pine_state_hash.cpp", "adapter_.hash_state(f);",
             "adapter_.missing_hash(f);"),))
        self.assertEqual(result, 1, output)

    def test_unknown_waiver_is_rejected(self):
        result, output = self.check((
            ("scripts/broker_state_hash_waivers.txt",
             "trade_start_time_ # configured execution window",
             "trade_start_time_ # configured execution window\nunknown_state_ # invalid"),))
        self.assertEqual(result, 1, output)


if __name__ == "__main__":
    unittest.main()
