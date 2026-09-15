#!/usr/bin/env python3
"""Mutation controls for real C++ ABI acceptance/rejection pair handling."""
from __future__ import annotations

import shutil
import unittest

from cpp_abi_pairing import PairingError, run_synthetic_pair


class SettlementAbi(unittest.TestCase):
    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        self.assertEqual(run_synthetic_pair(compiler), ["accept", "reject"])

    def test_unknown_pair_kind_is_refused(self) -> None:
        from cpp_abi_pairing import _source
        with self.assertRaises(PairingError):
            _source("engine_script_run_v17", "unknown")


if __name__ == "__main__":
    unittest.main()
