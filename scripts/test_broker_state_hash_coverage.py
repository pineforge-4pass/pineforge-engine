#!/usr/bin/env python3
"""Mutation controls for the split v16 broker/source hash coverage gate."""
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import shutil
import tempfile
import unittest

import check_broker_state_hash_coverage as checker
from gen_pending_order_mirror import members

ROOT = Path(__file__).resolve().parents[1]


class SourceHashCoverage(unittest.TestCase):
    def check(self, mutations=()):
        with tempfile.TemporaryDirectory(prefix="pf-source-hash-") as temporary:
            root = Path(temporary) / "repo"
            shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
                "build*", ".git", "corpus", "*.so", "*.a", ".native-fx-introduced-*"))
            for relative, before, after in mutations:
                target = root / relative
                text = target.read_text()
                self.assertIn(before, text, relative)
                target.write_text(text.replace(before, after, 1))
            output = StringIO()
            with redirect_stdout(output), redirect_stderr(output):
                result = checker.main(root)
            return result, output.getvalue()

    def test_current_v16_split_passes(self):
        self.assertEqual(self.check()[0], 0)

    def test_generic_domain_cannot_drift(self):
        result, output = self.check((
            ("src/engine_state_hash.cpp", "pineforge-broker-state/v16",
             "pineforge-broker-state/v12"),))
        self.assertEqual(result, 1, output)

    def test_source_domain_cannot_drift(self):
        result, output = self.check((
            ("include/pineforge/source/pine_adapter.hpp",
             "pineforge-source-adapter/v1", "pineforge-source-adapter/v0"),))
        self.assertEqual(result, 1, output)

    def test_stream_fold_is_v16_and_unconditional(self):
        for replacement in (
            "integer(12); integer(broker_state_hash());",
            "if (false) { integer(16); integer(broker_state_hash()); }",
        ):
            result, output = self.check((
                ("src/engine_stream.cpp", "integer(16); integer(broker_state_hash());",
                 replacement),))
            self.assertEqual(result, 1, output)

    def test_pending_order_loop_must_remain_in_source_hash(self):
        result, output = self.check((
            ("src/source/pine_state_hash.cpp", "f.s(o.id);", "f.s(o.missing_id);"),))
        self.assertEqual(result, 1, output)

    def test_source_marker_members_need_folds(self):
        result, output = self.check((
            ("src/source/pine_state_hash.cpp", "f.b(risk_halted_);",
             "f.b(missing_risk_halted_);"),))
        self.assertEqual(result, 1, output)

    def test_waiver_cannot_name_source_hashed_field(self):
        result, output = self.check((
            ("scripts/broker_state_hash_waivers.txt",
             "calc_on_order_fills_          # config: strategy(calc_on_order_fills=...)",
             "calc_on_order_fills_          # config: strategy(calc_on_order_fills=...)\n"
             "risk_direction_ # test source fold must not be waived"),))
        self.assertEqual(result, 1, output)
        self.assertIn("hashed by the source extension", output)

    def test_waiver_cannot_name_generic_hashed_field(self):
        result, output = self.check((
            ("scripts/broker_state_hash_waivers.txt",
             "calc_on_order_fills_          # config: strategy(calc_on_order_fills=...)",
             "calc_on_order_fills_          # config: strategy(calc_on_order_fills=...)\n"
             "fold_exit_path_extremes_ # test generic fold must not be waived"),))
        self.assertEqual(result, 1, output)
        self.assertIn("already hashed field", output)

    def test_pending_order_waiver_cannot_duplicate_fold(self):
        result, output = self.check((
            ("scripts/broker_state_hash_waivers.txt",
             "pending_order.comment         # trade-report label only: copied into the Trade row's entry/exit comment at fill and never read by any fill, admission, sizing or eligibility path -- it cannot change a future fill",
             "pending_order.comment         # trade-report label only: copied into the Trade row's entry/exit comment at fill and never read by any fill, admission, sizing or eligibility path -- it cannot change a future fill\n"
             "pending_order.id # test duplicate pending fold"),))
        self.assertEqual(result, 1, output)
        self.assertIn("redundant_waivers", output)

    def test_pending_parser_uses_source_intent(self):
        pending = ROOT / "include/pineforge/source/pine_pending_intent.hpp"
        self.assertGreater(len(members(pending.read_text())), 0)
        result, output = self.check((
            ("include/pineforge/source/pine_pending_intent.hpp",
             "double qty;", "double qty; double hidden;"),))
        self.assertEqual(result, 1, output)


if __name__ == "__main__":
    unittest.main()
