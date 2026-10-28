"""Mutation controls for the identity row's frozen-reference verifier."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

import test_report_sink_trade_identity as identity


class ReferenceIntegrity(unittest.TestCase):
    def test_each_manifested_file_rejects_one_changed_byte(self) -> None:
        reference, _ = identity.load_reference()
        with tempfile.TemporaryDirectory(prefix="pineforge-reference-mutation-") as name:
            copy = Path(name) / "reference"
            shutil.copytree(reference, copy)
            # First prove the unmodified copy is accepted by the same verifier.
            identity.verify_reference(copy, committed=False)
            complete = (copy / "MANIFEST.sha256").is_file()
            covered = set(identity.ARTIFACT_NAMES)
            if complete:
                covered |= {"manifest.json", "BASE_COMMIT"}
            self.assertEqual(len(covered), 14 if complete else 12)
            for filename in sorted(covered):
                with self.subTest(filename=filename):
                    path = copy / filename
                    original = path.read_bytes()
                    self.assertTrue(original)
                    changed = bytearray(original)
                    changed[len(changed) // 2] ^= 1
                    path.write_bytes(changed)
                    try:
                        with self.assertRaisesRegex(identity.ReferenceError,
                                                    "reference SHA-256 mismatch"):
                            identity.verify_reference(copy, committed=False)
                        if complete:
                            with mock.patch.dict(os.environ, {}, clear=True):
                                with mock.patch.object(identity, "COMMITTED_REFERENCE", copy):
                                    with self.assertRaisesRegex(
                                            identity.ReferenceError,
                                            "reference SHA-256 mismatch"):
                                        identity.load_reference()
                    finally:
                        path.write_bytes(original)

            path = copy / "reversal.hashes"
            changed = bytearray(path.read_bytes())
            changed[0] ^= 1
            path.write_bytes(changed)
            with mock.patch.dict(os.environ, {"PINEFORGE_TRADE_HISTORY_REFERENCE": str(copy)}):
                with mock.patch.object(identity.sys, "argv", [
                        str(Path(identity.__file__)), "retiring", "history", "legacy"]):
                    with mock.patch.object(identity.subprocess, "run") as producer:
                        with self.assertRaisesRegex(identity.ReferenceError,
                                                    "reference SHA-256 mismatch"):
                            identity.main()
                        producer.assert_not_called()

    def test_missing_or_empty_default_fails_with_original_message(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pineforge-reference-absent-") as name:
            root = Path(name)
            empty = root / "empty"
            empty.mkdir()
            for directory in (root / "missing", empty):
                with self.subTest(directory=directory):
                    with mock.patch.dict(os.environ, {}, clear=True):
                        with mock.patch.object(identity, "COMMITTED_REFERENCE", directory):
                            with self.assertRaises(identity.ReferenceError) as error:
                                identity.load_reference()
                            self.assertEqual(str(error.exception), identity.MISSING_REFERENCE)


if __name__ == "__main__":
    unittest.main()
