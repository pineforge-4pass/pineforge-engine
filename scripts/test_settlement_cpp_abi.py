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
# before.  test_real_link_accept_and_reject_pair is NOT one of them: it
# compiles a synthetic pair through cpp_abi_pairing.run_synthetic_pair, reads
# no fixture, and runs in both worlds.
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

    @requires_private_fixtures
    def test_v6_order_epoch_is_refused(self):
        # V19-B: the relocation manifest pins native_order_v6 -> v7, so a
        # tree whose request values are still v6 fails the surface audit.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            include = root / "include"
            shutil.copytree(ROOT / "include", include)
            path = include / "pineforge/native_order.hpp"
            path.write_text(path.read_text().replace(
                "inline namespace native_order_v7 {", "inline namespace native_order_v6 {", 1))
            with self.assertRaises(RuntimeError):
                checker.verify(include)

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
