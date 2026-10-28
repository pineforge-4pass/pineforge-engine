"""Candidate mutation controls using frozen bytes, without starting an engine."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import unittest
from unittest import mock

import test_report_sink_trade_identity as identity


def seal_sink(data: bytes) -> bytes:
    """Repair a test copy's trailer independently of the comparison helper."""
    prefix, trailer, end = data.rsplit(b"\n", 2)
    assert end == b""
    original = json.loads(trailer)["sha256"].encode("ascii")
    field = b'"sha256":"' + original + b'"'
    assert trailer.count(field) == 1
    replacement = b'"sha256":"' + hashlib.sha256(prefix + b"\n").hexdigest().encode("ascii") + b'"'
    return prefix + b"\n" + trailer.replace(field, replacement, 1) + b"\n"


def engine_version(data: bytes, version: str, *, seal: bool = True) -> bytes:
    header, newline, rest = data.partition(b"\n")
    assert newline
    original = json.loads(header)["engine"]
    field = b'"engine":' + json.dumps(original).encode("ascii")
    assert header.count(field) == 1
    replacement = b'"engine":' + json.dumps(version).encode("ascii")
    changed = header.replace(field, replacement, 1) + newline + rest
    assert changed != data
    return seal_sink(changed) if seal else changed


def trade_engine(data: bytes, version: str) -> bytes:
    """Put an engine key inside a real T record's string-valued comment."""
    trade = re.search(rb'(?m)^\["T",[^\n]*\n', data)
    assert trade is not None
    row = json.loads(trade[0])
    assert row[0] == "T" and len(row) == 21
    row[-1] = json.dumps({"engine": version}, separators=(",", ":"))
    line = json.dumps(row, separators=(",", ":")).encode("ascii") + b"\n"
    return seal_sink(data[:trade.start()] + line + data[trade.end():])


def record_body(data: bytes) -> bytes:
    """All record bytes, excluding only the header and trailer lines."""
    _, newline, rest = data.partition(b"\n")
    body, last_newline, _ = rest[:-1].rpartition(b"\n")
    assert newline and last_newline and data.endswith(b"\n")
    return body + last_newline


class CandidateComparison(unittest.TestCase):
    def setUp(self) -> None:
        self.reference, _ = identity.load_reference()

    def run_identity(self, replacements: dict[str, bytes]) -> None:
        # The real entry point still loads the independently frozen reference.
        # Stand-in producers copy that data and apply only the supplied mutants;
        # no output from the current library supplies an expected result.
        def produce(argv: list[str], *, check: bool) -> None:
            self.assertTrue(check)
            self.assertEqual(len(argv), 3)
            self.assertEqual(argv[1], "--artifacts")
            directory = Path(argv[2])
            written = 0
            for filename in sorted(identity.ARTIFACT_NAMES):
                data = replacements.get(filename)
                if data is None:
                    data = (self.reference / filename).read_bytes()
                (directory / filename).write_bytes(data)
                written += 1
            self.assertEqual(written, 12)

        with mock.patch.object(identity.sys, "argv", [
                identity.__file__, "retiring", "history", "legacy"]):
            with mock.patch.object(identity.subprocess, "run", side_effect=produce) as producer:
                try:
                    identity.main()
                finally:
                    self.assertEqual(producer.call_count, 3)

    def test_only_engine_version_compares_equal_on_both_sides(self) -> None:
        replacements = {}
        shapes = comparisons = 0
        for shape in identity.SHAPES:
            filename = f"{shape}.sink"
            expected = (self.reference / filename).read_bytes()
            version = json.loads(expected.partition(b"\n")[0])["engine"]
            versions = 0
            for candidate_version in (
                    version + ".next", version + "-release+metadata",
                    version + '-"quoted"\\version'):
                candidate = engine_version(expected, candidate_version)
                self.assertNotEqual(candidate, expected)
                self.assertEqual(record_body(candidate), record_body(expected))
                identity.compare_artifact(filename, expected, candidate)
                identity.compare_artifact(filename, candidate, expected)
                replacements[filename] = candidate
                versions += 1
                comparisons += 1
            self.assertEqual(versions, 3)
            shapes += 1
        self.assertEqual(shapes, 4)
        self.assertEqual(comparisons, 12)
        self.assertEqual(len(replacements), 4)
        self.run_identity(replacements)

    def test_one_other_byte_in_each_artifact_fails_and_names_file(self) -> None:
        mutations = 0
        for suffix in identity.SUFFIXES:
            filename = f"reversal.{suffix}"
            expected = (self.reference / filename).read_bytes()
            changed = bytearray(expected)
            if suffix == "sink":
                # A trailer count is outside the checksum's covered prefix.
                # This is exactly one byte, with a still-valid wire checksum,
                # so rejection must preserve the comparison of other fields.
                trailer_start = expected.rfind(b"\n", 0, len(expected) - 1) + 1
                count = re.search(rb'"T":([0-9]+)', expected[trailer_start:])
                self.assertIsNotNone(count)
                offset = trailer_start + count.end(1) - 1
                changed[offset] = ord("0") if expected[offset] != ord("0") else ord("1")
            else:
                offset = len(expected) // 2
                changed[offset] ^= 1
            candidate = bytes(changed)
            self.assertNotEqual(candidate[offset], expected[offset])
            self.assertEqual(candidate[:offset], expected[:offset])
            self.assertEqual(candidate[offset + 1:], expected[offset + 1:])
            with self.assertRaisesRegex(AssertionError, re.escape(filename)):
                identity.compare_artifact(filename, expected, candidate)
            with self.assertRaisesRegex(AssertionError, re.escape(filename)):
                self.run_identity({filename: candidate})
            mutations += 1
        self.assertEqual(mutations, 3)

    def test_engine_inside_trade_body_is_never_normalized(self) -> None:
        mutations = 0
        for shape in identity.SHAPES:
            filename = f"{shape}.sink"
            frozen = (self.reference / filename).read_bytes()
            version = json.loads(frozen.partition(b"\n")[0])["engine"]
            # Controlled copies isolate a second occurrence. They never replace
            # the reference loaded by the real identity entry point.
            expected = trade_engine(frozen, version)
            next_version = version + "-next"
            candidate = engine_version(trade_engine(expected, next_version), next_version)
            expected_body = record_body(expected)
            candidate_body = record_body(candidate)
            self.assertIn(b'engine', expected_body)
            self.assertIn(b'engine', candidate_body)
            self.assertNotEqual(expected_body, candidate_body)
            self.assertEqual(
                record_body(identity.comparison_bytes(filename, expected)), expected_body)
            self.assertEqual(
                record_body(identity.comparison_bytes(filename, candidate)), candidate_body)
            with self.assertRaisesRegex(AssertionError, re.escape(filename)):
                identity.compare_artifact(filename, expected, candidate)
            mutations += 1
        self.assertEqual(mutations, 4)

    def test_original_sink_digest_is_checked_before_normalization(self) -> None:
        filename = "reversal.sink"
        expected = (self.reference / filename).read_bytes()
        version = json.loads(expected.partition(b"\n")[0])["engine"]
        candidate = engine_version(expected, version + "-next", seal=False)
        with self.assertRaisesRegex(AssertionError, re.escape(filename) + ".*SHA-256"):
            identity.compare_artifact(filename, expected, candidate)


if __name__ == "__main__":
    unittest.main()
