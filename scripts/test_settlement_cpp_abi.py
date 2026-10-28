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

# The private-fixture PRs move tests/fixtures/native_cpp_abi/ (both relocation
# manifests checker.verify reads) out of the public tree.  Absent, every
# control that reads a manifest skips with the project's pinned literal
# instead of failing inside the checker; present, the suite runs exactly as
# before. Compiler and order-value controls read no private fixture and run
# in both worlds.
PRIVATE_FIXTURE_SKIP_LITERAL = "SKIP private fixtures not present: "
PRIVATE_FIXTURE_MANIFESTS = (checker.MANIFEST, checker.V16_V18_MANIFEST)
requires_private_fixtures = unittest.skipUnless(
    all(path.exists() for path in PRIVATE_FIXTURE_MANIFESTS),
    PRIVATE_FIXTURE_SKIP_LITERAL + "tests/fixtures/native_cpp_abi")


class SettlementAbi(unittest.TestCase):
    @requires_private_fixtures
    def test_current_include_tree(self):
        result = checker.verify(ROOT / "include")
        self.assertEqual(result["transition"]["to"], "engine_script_run_v19")

    @requires_private_fixtures
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

    @requires_private_fixtures
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
            _source("engine_script_run_v19", "unknown")

    @requires_private_fixtures
    def test_v18_capability_macro_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            include = root / "include"
            shutil.copytree(ROOT / "include", include)
            path = include / "pineforge/native_host.hpp"
            path.write_text(path.read_text().replace(
                "#define PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19 1",
                "#define PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18 1", 1))
            with self.assertRaises(RuntimeError):
                checker.verify(include)

    def test_current_order_epoch(self):
        self.assertEqual(checker.verify_order_epoch(ROOT / "include"), "native_order_v8")

    def test_stale_order_epochs_are_refused(self):
        for stale in ("native_order_v6", "native_order_v7"):
            with self.subTest(stale=stale), tempfile.TemporaryDirectory() as directory:
                include = Path(directory) / "include"
                shutil.copytree(ROOT / "include", include)
                path = include / "pineforge/native_order.hpp"
                before = "inline namespace native_order_v8 {"
                self.assertIn(before, path.read_text())
                path.write_text(path.read_text().replace(before, "inline namespace " + stale + " {", 1))
                with self.assertRaisesRegex(RuntimeError, "native_order_v8"):
                    checker.verify_order_epoch(include)

    def test_successful_stale_link_is_not_a_refusal(self):
        with self.assertRaisesRegex(PairingError, "unexpectedly linked"):
            checker.require_order_rejection(0, "")

    def test_unrelated_link_failure_is_not_a_refusal(self):
        for diagnostic in ("cannot find -lpineforge",
                           "undefined reference to `unrelated_symbol'",
                           "native_order_v7::WorkingRequestCore::WorkingRequestCore: syntax error",
                           "native_order_v7::WorkingRequestCore::WorkingRequestCore: syntax error\n"
                           "undefined reference to `unrelated_symbol'"):
            with self.subTest(diagnostic=diagnostic):
                with self.assertRaisesRegex(PairingError, "wrong reason"):
                    checker.require_order_rejection(1, diagnostic)

    def test_old_namespace_must_be_named_by_the_linker(self):
        checker.require_order_rejection(
            1, "undefined reference to `pineforge::native_order::native_order_v7"
               "::WorkingRequestCore::WorkingRequestCore(...)'")
        checker.require_order_rejection(
            1, 'Undefined symbols for architecture arm64:\n'
               '  "pineforge::native_order::native_order_v7::WorkingRequestCore'
               '::WorkingRequestCore(...)", referenced from:\n  _main in order_v7.o')
        with self.assertRaisesRegex(PairingError, "signal"):
            checker.require_order_rejection(
                -11, "undefined reference to `pineforge::native_order::native_order_v7"
                     "::WorkingRequestCore::WorkingRequestCore(...)'")
        with self.assertRaisesRegex(PairingError, "wrong reason"):
            checker.require_order_rejection(
                1, "undefined reference to `pineforge::native_order::native_order_v8"
                   "::WorkingRequestCore::WorkingRequestCore(...)'")

    def test_empty_or_banner_only_report_comparison_is_refused(self):
        for raw in (b"", b"test_native_settlement_row_finite: ok (0 rows)\n",
                    b"finite_closed_zero not-a-binary64-word\n"):
            with self.subTest(raw=raw):
                with self.assertRaises(PairingError):
                    checker.require_report_transcript(raw)

    @requires_private_fixtures
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

    @requires_private_fixtures
    def test_missing_host_hash_extension_is_refused(self):
        # N5: the generic hash extension and the deprecated spelling it
        # forwards to are both v19 BacktestEngine virtuals.
        for name in ("hash_host_extension", "hash_source_extension"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                include = root / "include"
                shutil.copytree(ROOT / "include", include)
                path = include / "pineforge/engine.hpp"
                path.write_text(path.read_text().replace(
                    "virtual void " + name + "(", "virtual void missing_" + name + "(", 1))
                with self.assertRaises(RuntimeError):
                    checker.verify(include)


if __name__ == "__main__":
    absent = [path for path in PRIVATE_FIXTURE_MANIFESTS if not path.exists()]
    if absent:
        print(PRIVATE_FIXTURE_SKIP_LITERAL + str(absent[0].relative_to(ROOT)))
    unittest.main()
