#!/usr/bin/env python3
"""Mutation controls for the admission reflection guard."""
from pathlib import Path
import shutil
import tempfile
import unittest

import check_market_admission_schema as checker

ROOT = Path(__file__).resolve().parents[1]


class AdmissionSchema(unittest.TestCase):
    def clone(self):
        directory = tempfile.TemporaryDirectory()
        root = Path(directory.name) / "repo"
        # __pycache__ / *.pyc: a Python guard CTest runs in parallel writes its
        # bytecode through a temporary file that can vanish between copytree's
        # listing and its copy; bytecode is never source input to the clone.
        shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
            "build*", ".git", "corpus", "*.so", "*.a",
            ".native-fx-introduced-*", "__pycache__", "*.pyc"))
        return directory, root

    def test_current(self):
        self.assertEqual(checker.check(ROOT), 14)

    def test_missing_reflection_is_rejected(self):
        directory, root = self.clone()
        try:
            path = root / "src/source/market_admission.cpp"
            path.write_text(path.read_text().replace("void reflect(const Event& value", "void missing_reflect(const Event& value", 1))
            with self.assertRaises(ValueError):
                checker.check(root)
        finally:
            directory.cleanup()

    def test_missing_adapter_hash_is_rejected(self):
        directory, root = self.clone()
        try:
            path = root / "src/source/pine_state_hash.cpp"
            path.write_text(path.read_text().replace(
                "admission_journal.reflect(\"journal\"",
                "missing_journal.reflect(\"journal\"", 1))
            with self.assertRaises(ValueError):
                checker.check(root)
        finally:
            directory.cleanup()


if __name__ == "__main__":
    unittest.main()
