#!/usr/bin/env python3
"""Mutation control: the script ABI checker must retain one accept/reject pair."""
from __future__ import annotations

import shutil
import unittest

from cpp_abi_pairing import run_synthetic_pair


class ScriptAbi(unittest.TestCase):
    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        outcomes = run_synthetic_pair(compiler)
        self.assertIn("accept", outcomes)
        self.assertIn("reject", outcomes)


if __name__ == "__main__":
    unittest.main()
