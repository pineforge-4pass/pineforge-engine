#!/usr/bin/env python3
"""Mutation controls for the all-field pending-intent projection checker."""
from __future__ import annotations

from pathlib import Path
import shutil
import tempfile
import unittest

import test_pending_intent_view as checker


ROOT = Path(__file__).resolve().parents[1]


class ProjectionCoverage(unittest.TestCase):
    def check(self, mutations=()):
        temporary = tempfile.TemporaryDirectory(prefix="pf-intent-view-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name) / "repo"
        # __pycache__ / *.pyc: a Python guard CTest runs in parallel writes its
        # bytecode through a temporary file that can vanish between copytree's
        # listing and its copy; bytecode is never source input to the clone.
        shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
            ".git", "build*", "corpus", "benchmarks", "*.a", "*.so", ".native-fx-introduced-*", ".ccache",
            "__pycache__", "*.pyc"))
        for relative, before, after in mutations:
            path = root / relative
            text = path.read_text()
            self.assertIn(before, text)
            path.write_text(text.replace(before, after, 1))
        try:
            result = checker.check(root)
            return result, ""
        except ValueError as error:
            return None, str(error)

    def test_current_tree_passes_with_exact_named_debt(self):
        result, diagnostic = self.check()
        self.assertIsNotNone(result, diagnostic)
        self.assertEqual(result["mirror"], 406)
        self.assertEqual(result["dynamic"], 406)
        self.assertEqual(result["debt"], 0)

    def test_arbitrary_field_cannot_be_folded_to_zero(self):
        result, diagnostic = self.check(((
            "src/source/pine_adapter.cpp",
            "out->created_bar = snapshot.projection_created_bar;",
            "out->created_bar = 0U;"),))
        self.assertIsNone(result)
        self.assertIn("created_bar", diagnostic)

    def test_named_constant_fold_is_rejected(self):
        result, diagnostic = self.check(((
            "src/source/pine_adapter.cpp",
            "constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();",
            "constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();\n"
            "constexpr std::uint32_t kZero = 0U;"),
            ("src/source/pine_adapter.cpp",
             "out->created_bar = snapshot.projection_created_bar;",
             "out->created_bar = kZero;"),))
        self.assertIsNone(result)
        self.assertIn("created_bar", diagnostic)

    def test_dead_branch_does_not_count_as_a_projection(self):
        result, diagnostic = self.check(((
            "src/source/pine_adapter.cpp",
            "out->created_bar = snapshot.projection_created_bar;",
            "if (false) { out->created_bar = snapshot.projection_created_bar; }"),))
        self.assertIsNone(result)
        self.assertIn("created_bar", diagnostic)

    def test_memset_only_debt_cannot_grow(self):
        result, diagnostic = self.check(((
            "src/source/pine_adapter.cpp",
            "out->created_seq = static_cast<std::int64_t>(snapshot.source_sequence);",
            ""),))
        self.assertIsNone(result)
        self.assertIn("created_seq", diagnostic)

    def test_stale_debt_row_is_rejected_after_live_projection_lands(self):
        # Integrated tree: every field is projected live, so a re-added debt
        # row for a live field must be rejected as stale.
        result, diagnostic = self.check(((
            "scripts/pending_intent_constant_debt.txt",
            "# (empty since MERGE-L8",
            "legs_last_bind_owner\n# (empty since MERGE-L8"),))
        self.assertIsNone(result)
        self.assertIn("legs_last_bind_owner", diagnostic)

    def test_nonexistent_schema_provenance_is_rejected(self):
        result, diagnostic = self.check(((
            "scripts/pending_intent_view.json",
            "PlacementSnapshot::projection_position_side",
            "PlacementSnapshot::NonexistentProjectionFact"),))
        self.assertIsNone(result)
        self.assertIn("NonexistentProjectionFact", diagnostic)


if __name__ == "__main__":
    unittest.main()
