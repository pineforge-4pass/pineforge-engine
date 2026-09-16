#!/usr/bin/env python3
"""Mutation control: the script ABI checker must retain one accept/reject pair."""
from __future__ import annotations

import shutil
from pathlib import Path
import tempfile
import unittest

from cpp_abi_pairing import PairingError, enforce_receipt_mode, run_synthetic_pair


class ScriptAbi(unittest.TestCase):
    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        outcomes = run_synthetic_pair(compiler)
        self.assertIn("accept", outcomes)
        self.assertIn("reject", outcomes)

    def test_manual_missing_receipt_skips_but_ci_mode_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing.json"
            self.assertEqual(enforce_receipt_mode(
                [missing], skip=True, require=False, label="fixture"), 77)
            with self.assertRaisesRegex(PairingError, "required receipt missing"):
                enforce_receipt_mode(
                    [missing], skip=False, require=True, label="fixture")


if __name__ == "__main__":
    unittest.main()
