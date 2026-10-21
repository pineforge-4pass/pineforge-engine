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
                ".native-fx-introduced-*",
                # Nor is bytecode, which a parallel Python guard writes
                # through a temporary file that can vanish mid-copy.
                "__pycache__", "*.pyc"))
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

    def test_scratch_store_state_digest_prefix_is_pinned(self):
        # The old checker ignores this implementation, so every mutation
        # below returns success there and this test fails. No digest literal
        # is an oracle: mutate the live folds and derive the verdict from the
        # coverage check, just as for the other durable-state leaves.
        result, output = self.check()
        self.assertEqual(result, 0, output)
        self.assertIn("leaf=CommandScratchStore.state_digest prefix (24 bytes:", output)
        for before, after in (
            ("fold.le64(order_.id);", ""),
            ("fold.le64(order_.id);", "fold.le64(0);"),
            ("fold.le64(cur_.next_handle);", ""),
            ("fold.le64(cur_.next_handle);", "fold.le64(1);"),
            ("fold.le64(cur_.record_count);", "fold.le64(0);"),
            ("fold.le64(order_.id);\n            fold.le64(cur_.next_handle);",
             "fold.le64(cur_.next_handle);\n            fold.le64(order_.id);"),
            ("if (with_state) {\n            fold.le64(order_.id);",
             "if (false) {\n            fold.le64(order_.id);"),
            ("state_->digest(true, page, result)", "state_->digest(false, page, result)"),
            ("bytes(raw, 8);", "bytes(raw, 4);"),
            ("(value >> (8 * i)) & 0xFF", "(value >> (8 * (7 - i))) & 0xFF"),
        ):
            with self.subTest(before=before, after=after):
                result, output = self.check(((
                    "include/pineforge/detail/scratch_store_impl.hpp", before, after),))
                self.assertEqual(result, 1, output)
                self.assertIn("CommandScratchStore.state_digest", output)

    def test_native_request_state_folds_are_pinned(self):
        # R5 L7b: the anchor rounding and the arm visibility are request
        # state folded (only when set) into the continuation the generic
        # broker-state hash consumes; losing either fold fails closed.
        for before, after in (
            ("if (anchor->rounding != native_order::NativeAnchorRounding::Raw) {",
             "if (false) {"),
            ("f.u(static_cast<uint64_t>(anchor->rounding));", ""),
            ("if (value.visibility != native_order::NativeArmVisibility::Working) {",
             "if (false) {"),
            ("f.u(static_cast<uint64_t>(value.visibility));", ""),
            # R5 N13: likewise the first-match rule and the arm scope.
            ("if (value.first_match != native_order::NativeArmFirstMatch::AtArmPrint) {",
             "if (false) {"),
            ("f.u(static_cast<uint64_t>(value.first_match));", ""),
            ("if (value.scope != native_order::NativeArmScope::OwnerLot) {", "if (false) {"),
            ("f.u(static_cast<uint64_t>(value.scope));", ""),
        ):
            with self.subTest(before=before):
                result, output = self.check((
                    ("src/native_execution_consumer.cpp", before, after),))
                self.assertEqual(result, 1, output)

    def test_kernel_recorded_hash_row_is_pinned(self):
        # R5 N5 (RP9): the KernelRecorded recorder appends the per-bar
        # broker-state hash behind the recording switch, exactly once. A
        # dropped, dead, unconditional or relocated append fails closed.
        append = "engine.broker_state_hashes_.push_back(engine.broker_state_hash());"
        for before, after in (
            (append, ""),
            ("if (engine.broker_state_hash_recording_) {", "if (false) {"),
            ("if (engine.broker_state_hash_recording_) {", "{"),
            ("    engine.update_equity_extremes();\n",
             "    engine.update_equity_extremes();\n"
             "    engine.broker_state_hashes_.push_back(engine.broker_state_hash());\n"),
        ):
            with self.subTest(before=before, after=after):
                result, output = self.check((
                    ("src/native_execution_consumer.cpp", before, after),))
                self.assertEqual(result, 1, output)

    def test_generic_host_seam_is_pinned(self):
        # R5 N5 (RP10): the fold ends in hash_host_extension, its default
        # reaches the established marker through the deprecated spelling, and
        # the source host uses the generic hook.
        for relative, before, after in (
            ("src/engine_state_hash.cpp", "    hash_host_extension(f);\n", ""),
            ("src/engine_state_hash.cpp", "    hash_host_extension(f);\n",
             "    if (false) { hash_host_extension(f); }\n"),
            ("src/engine_state_hash.cpp", "    hash_host_extension(f);\n",
             "    hash_source_extension(f);\n"),
            ("src/engine_state_hash.cpp", "    hash_source_extension(sink);\n", ""),
            ("src/engine_state_hash.cpp", 'sink.s("source:none");', 'sink.s("");'),
            ("src/source/pine_state_hash.cpp",
             "void source::PineStrategyHost::hash_host_extension(",
             "void source::PineStrategyHost::hash_source_extension("),
        ):
            with self.subTest(relative=relative, before=before, after=after):
                result, output = self.check(((relative, before, after),))
                self.assertEqual(result, 1, output)

    def test_generic_domain_is_pinned(self):
        result, output = self.check((
            ("src/engine_state_hash.cpp", "pineforge-broker-state/v19",
             "pineforge-broker-state/v0"),))
        self.assertEqual(result, 1, output)

    def test_source_domain_is_pinned(self):
        result, output = self.check((
            ("include/pineforge/source/pine_adapter.hpp",
             "pineforge-source-adapter/v4", "pineforge-source-adapter/v0"),))
        self.assertEqual(result, 1, output)

    def test_security_domain_is_pinned(self):
        result, output = self.check((
            ("include/pineforge/source/pine_strategy_host.hpp",
             "pineforge-source-security/v6", "pineforge-source-security/v0"),))
        self.assertEqual(result, 1, output)

    def test_security_site_field_is_enumerated(self):
        result, output = self.check(((
            "src/source/pine_state_hash.cpp", "f.b(pine.lookahead_on);",
            "f.b(false);"),))
        self.assertEqual(result, 1, output)
        self.assertIn("lookahead_on", output)

    def test_adapter_fold_is_required(self):
        result, output = self.check((
            ("src/source/pine_state_hash.cpp", "adapter_.hash_state(f);",
             "adapter_.missing_hash(f);"),))
        self.assertEqual(result, 1, output)

    def test_exit_scratch_fold_is_required(self):
        result, output = self.check()
        self.assertEqual(result, 0, output)
        fold = "hash_exit_scratch(f, exit_scratch_.get());"
        for replacement in ("", "hash_exit_scratch(f, nullptr);"):
            with self.subTest(replacement=replacement):
                result, output = self.check(((
                    "src/source/pine_state_hash.cpp", fold, replacement),))
                self.assertEqual(result, 1, output)
                self.assertIn("exit_scratch_", output)

    def test_exit_scratch_context_fields_are_required(self):
        result, output = self.check()
        self.assertEqual(result, 0, output)
        self.assertIn("6 exit-context fields", output)
        for field, fold in checker.EXIT_SCRATCH_CONTEXT_FOLDS.items():
            for replacement in ("", "if (false) { " + fold + " }"):
                with self.subTest(field=field, replacement=replacement):
                    result, output = self.check(((
                        "src/source/pine_state_hash.cpp", fold, replacement),))
                    self.assertEqual(result, 1, output)
                    self.assertIn("ExitScratchContext." + field, output)
        for before, after in (
            ("hash_source_run_identity(f, scratch->run);",
             "hash_source_run_identity(f, {});"),
            ("f.u(scratch->next_scope);", "f.u(0);"),
            ("f.b(scratch->versions != nullptr);", "f.b(false);"),
            ("f.b(scratch->lots != nullptr);", "f.b(false);"),
            ("f.b(scratch->working != nullptr);", "f.b(false);"),
            ("hash_scratch_store(f, &scratch->store);", "hash_scratch_store(f, nullptr);"),
        ):
            with self.subTest(before=before):
                result, output = self.check(((
                    "src/source/pine_state_hash.cpp", before, after),))
                self.assertEqual(result, 1, output)
                self.assertIn("ExitScratchContext", output)

    def test_new_exit_scratch_context_field_is_not_silently_ignored(self):
        for declaration in (
            "    std::uint64_t injected_exit_state = 0;\n",
            "    bool (*injected_exit_reader)(void*) = nullptr;\n",
        ):
            with self.subTest(declaration=declaration):
                result, output = self.check(((
                    "include/pineforge/source/exit_scratch.hpp",
                    "struct ExitScratchContext {\n",
                    "struct ExitScratchContext {\n" + declaration),))
                self.assertEqual(result, 1, output)
                self.assertIn("ExitScratchContext fields: unclassified=", output)
                self.assertIn("injected_exit_", output)

    def test_exit_scratch_observation_waivers_cannot_go_stale(self):
        result, output = self.check(((
            "include/pineforge/source/exit_scratch.hpp",
            "    void* observation = nullptr;\n", ""),))
        self.assertEqual(result, 1, output)
        self.assertIn("ExitScratchContext fields:", output)
        self.assertIn("observation", output)

    def test_trade_scratch_fold_is_required(self):
        result, output = self.check()
        self.assertEqual(result, 0, output)
        fold = "hash_scratch_store(f, trade_scratch_);"
        for replacement in ("", "hash_scratch_store(f, nullptr);"):
            with self.subTest(replacement=replacement):
                result, output = self.check(((
                    "src/source/pine_state_hash.cpp", fold, replacement),))
                self.assertEqual(result, 1, output)
                self.assertIn("trade_scratch_", output)

    def test_unknown_waiver_is_rejected(self):
        result, output = self.check((
            ("scripts/broker_state_hash_waivers.txt",
             "trade_start_time_ # Configured execution-window boundary; native admission receives the projected boundary before requests exist.",
             "trade_start_time_ # Configured execution-window boundary; native admission receives the projected boundary before requests exist.\nunknown_state_ # invalid"),))
        self.assertEqual(result, 1, output)

    def test_void_cast_cannot_fake_a_fold(self):
        result, output = self.check(((
            "src/source/pine_state_hash.cpp", "f.u(cap_latest_fill_);",
            "(void)cap_latest_fill_;"),))
        self.assertEqual(result, 1, output)
        self.assertIn("cap_latest_fill_", output)

    def test_constant_cannot_replace_a_fold(self):
        result, output = self.check(((
            "src/source/pine_state_hash.cpp", "f.u(cap_latest_fill_);",
            "f.u(0);"),))
        self.assertEqual(result, 1, output)
        self.assertIn("cap_latest_fill_", output)

    def test_dead_branch_cannot_fake_a_fold(self):
        result, output = self.check(((
            "src/source/pine_state_hash.cpp", "f.u(cap_latest_fill_);",
            "if (false) { f.u(cap_latest_fill_); }"),))
        self.assertEqual(result, 1, output)
        self.assertIn("cap_latest_fill_", output)

    def test_member_above_marker_is_still_covered(self):
        result, output = self.check(((
            "include/pineforge/source/pine_adapter.hpp",
            "    // @source-state begin",
            "    std::uint64_t injected_unhashed_state_ = 0;\n"
            "    // @source-state begin"),))
        self.assertEqual(result, 1, output)
        self.assertIn("injected_unhashed_state_", output)

    def test_nested_struct_field_is_enumerated(self):
        result, output = self.check(((
            "src/source/pine_state_hash.cpp",
            "f.d(value.frozen_reversal_transaction);",
            "f.d(0.0);"),))
        self.assertEqual(result, 1, output)
        self.assertIn("frozen_reversal_transaction", output)

if __name__ == "__main__":
    unittest.main()
