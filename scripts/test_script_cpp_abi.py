#!/usr/bin/env python3
"""Mutation control: the script ABI checker must retain one accept/reject pair."""
from __future__ import annotations

import shutil
from pathlib import Path
import tempfile
import unittest

from cpp_abi_pairing import (PairingError, enforce_receipt_mode, epoch_from_headers,
                             run_synthetic_pair)


class ScriptAbi(unittest.TestCase):
    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        outcomes = run_synthetic_pair(compiler)
        self.assertIn("accept", outcomes)
        self.assertIn("reject", outcomes)

    def epoch_header(self, *namespaces: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        include = Path(directory.name)
        (include / "pineforge").mkdir()
        (include / "pineforge" / "engine.hpp").write_text("".join(
            "inline namespace " + name + " {\n}  // " + name + "\n" for name in namespaces))
        return include

    def test_epoch_header_accepts_a_reopened_epoch(self) -> None:
        self.assertEqual(epoch_from_headers(self.epoch_header(
            "engine_script_run_v19", "engine_script_run_v19")), "engine_script_run_v19")

    def test_epoch_header_refuses_two_different_epochs_or_none(self) -> None:
        with self.assertRaisesRegex(PairingError, "exactly one epoch"):
            epoch_from_headers(self.epoch_header("engine_script_run_v19", "engine_script_run_v18"))
        with self.assertRaisesRegex(PairingError, "exactly one epoch"):
            epoch_from_headers(self.epoch_header())

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
