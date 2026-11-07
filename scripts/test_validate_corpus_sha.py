#!/usr/bin/env python3
"""Unit tests for corpus records; no engine parity oracle is used."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import validate_corpus_sha as validator


class CorpusRecordsTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="corpus-records-"))
        probe = self.root / "validation" / "sample"
        probe.mkdir(parents=True)
        (self.root / "data").mkdir()
        (self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv").write_bytes(
            b"timestamp,open,high,low,close,volume\n0,1,1,1,1,1\n")
        for filename in validator.ARTIFACT_FILES.values():
            (probe / filename).write_bytes(b"recorded bytes\n")
        self.write_records()

    def write_records(self):
        probe = self.root / "validation" / "sample"
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        self.manifest = {
            "kind": "pineforge-corpus-run-manifest",
            "schemaVersion": 2,
            "feedSha256": digest(
                self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv"),
            "tapes": {"validation/sample": digest(probe / "engine_trades.csv")},
        }
        self.owned = {
            "schema_version": "pineforge-owned-corpus-manifest-v1",
            "entries": [{
                "corpus_id": "sample",
                "artifacts": {key: digest(probe / filename)
                              for key, filename in validator.ARTIFACT_FILES.items()},
            }],
        }
        self.save()

    def save(self):
        (self.root / "run_manifest.json").write_text(json.dumps(self.manifest))
        (self.root / "owned_strategies.json").write_text(json.dumps(self.owned))

    def test_records_bind_each_artifact_and_feed_to_bytes(self):
        self.assertEqual(validator.check_records(self.root), [])
        for filename in (*validator.ARTIFACT_FILES.values(),
                         "../../data/ohlcv_ETH-USDT-USDT_1m.csv"):
            with self.subTest(filename=filename):
                path = self.root / "validation" / "sample" / filename
                previous = path.read_bytes()
                path.write_bytes(previous + b"x")
                errors = validator.check_records(self.root)
                self.assertTrue(errors)
                self.assertIn(path.resolve().name, "\n".join(errors))
                path.write_bytes(previous)

    def test_renamed_directory_reports_missing_and_unrecorded(self):
        (self.root / "validation" / "sample").rename(
            self.root / "validation" / "renamed")
        errors = "\n".join(validator.check_records(self.root))
        self.assertIn("validation/sample", errors)
        self.assertIn("validation/renamed", errors)

    def test_empty_population_and_wrong_schemas_are_invalid(self):
        for kind in ("tapes", "kind", "schemaVersion"):
            previous = self.manifest[kind]
            self.manifest[kind] = {} if kind == "tapes" else "wrong"
            self.save()
            self.assertTrue(validator.check_records(self.root))
            self.manifest[kind] = previous
        self.owned["entries"] = []
        self.save()
        self.assertTrue(validator.check_records(self.root))

    def test_lfs_pointer_is_checked_against_materialized_bytes(self):
        feed = self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv"
        oid = hashlib.sha256(feed.read_bytes()).hexdigest()
        pointer = (f"version https://git-lfs.github.com/spec/v1\n"
                   f"oid sha256:{oid}\nsize {feed.stat().st_size}\n").encode()
        self.assertIsNone(validator.lfs_problem(feed, pointer))
        self.assertIn(str(feed), validator.lfs_problem(
            feed, pointer.replace(oid.encode(), b"0" * 64)))
        feed.write_bytes(pointer)
        self.assertIn("materialized", validator.lfs_problem(feed, pointer))

    def test_subset_rejects_empty_duplicate_and_unknown_probes(self):
        path = self.root / "subset.txt"
        for text in ("# empty\n", "validation/sample\nvalidation/sample\n",
                     "validation/missing\n", "../sample\n"):
            with self.subTest(text=text):
                path.write_text(text)
                with self.assertRaises(ValueError):
                    validator.read_subset(path, {"validation/sample"})
        path.write_text("validation/sample # mechanism\n")
        self.assertEqual(validator.read_subset(
            path, {"validation/sample"}), ["validation/sample"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
