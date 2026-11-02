#!/usr/bin/env python3
"""Mutation controls for the admission reflection guard."""
from pathlib import Path
import tempfile
import unittest

import check_market_admission_schema as checker
import source_clone

ROOT = Path(__file__).resolve().parents[1]


class AdmissionSchema(unittest.TestCase):
    def clone(self):
        directory = tempfile.TemporaryDirectory()
        root = Path(directory.name) / "repo"
        # source_clone.py excludes volatile bytecode and pytest cache names.
        source_clone.clone_source_tree(ROOT, root, extra_ignore=(
            "build*", ".git", "corpus", "*.so", "*.a",
            ".native-fx-introduced-*"))
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
            for relative, token in (
                ("src/source/market_admission.cpp",
                 "fold_validated_journal_record(source,page,summary,result.digest,"),
                ("src/source/pine_state_hash.cpp",
                 "const auto folded = admission_journal.folded_window();"),
                ("src/source/pine_state_hash.cpp",
                 "admission_events_folded_ = folded.count;"),
                ("src/source/pine_state_hash.cpp",
                 "admission_events_digest_ = folded.digest;"),
                ("src/source/pine_state_hash.cpp",
                 "admission_events_last_ = folded.last_sequence;"),
            ):
                with self.subTest(token=token):
                    path = root / relative
                    original = path.read_text()
                    self.assertIn(token, original)
                    try:
                        path.write_text(original.replace(token, "", 1))
                        with self.assertRaises(ValueError):
                            checker.check(root)
                    finally:
                        path.write_text(original)
        finally:
            directory.cleanup()


if __name__ == "__main__":
    unittest.main()
