#!/usr/bin/env python3
"""check_provenance.py passes the README and fails each way a number can lose its source.

Every must-fail case edits a copy of the README text or tampers with what the
repository serves, and asserts the error that names the failure.

    python3 benchmarks/tests/test_check_provenance.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "benchmarks"))
import check_provenance as cp  # noqa: E402

README = (REPO_ROOT / cp.README).read_text(encoding="utf-8")
THROUGHPUT_BULLET = "and is the median of five quiet runs."


class Tampered(cp.Tree):
    """The working tree with one raw file's bytes changed, or one file added."""

    def __init__(self, *, change: str | None = None, extra: str | None = None):
        super().__init__()
        self.change, self.extra = change, extra

    def bytes(self, path: str) -> bytes:
        data = super().bytes(path)
        return data + b"\n" if path == self.change else data

    def files(self, directory: str) -> list[str]:
        return super().files(directory) + ([self.extra] if self.extra else [])


class CheckProvenance(unittest.TestCase):
    def errors(self, readme: str = README, tree: cp.Tree | None = None) -> list[str]:
        return cp.check(readme, tree or cp.Tree())

    def assert_error(self, errors: list[str], text: str) -> None:
        self.assertTrue(any(text in e for e in errors),
                        f"no error mentions {text!r}; errors: {errors}")

    def test_readme_traces_to_committed_sources(self) -> None:
        self.assertEqual(self.errors(), [])

    def test_number_without_a_claim_fails(self) -> None:
        readme = README.replace(THROUGHPUT_BULLET, THROUGHPUT_BULLET + " A cold start adds 7 ms.")
        self.assertNotEqual(readme, README)
        self.assert_error(self.errors(readme), "'7' has no source")

    def test_number_its_source_contradicts_fails(self) -> None:
        readme = README.replace("**603k**", "**650k**")
        self.assertNotEqual(readme, README)
        self.assert_error(self.errors(readme), "pineforge_row: the README prints median = 650")

    def test_rounding_outside_the_printed_precision_fails(self) -> None:
        readme = README.replace("0.94× the time", "0.95× the time")
        self.assertNotEqual(readme, README)
        self.assert_error(self.errors(readme), "sweep_loads_and_ab: the README prints ratio = 0.95")

    def test_reworded_phrase_fails(self) -> None:
        readme = README.replace(THROUGHPUT_BULLET, "and is the median of 5 runs.")
        self.assertNotEqual(readme, README)
        errors = self.errors(readme)
        self.assert_error(errors, "throughput: its phrase is not in the headline any more")
        self.assert_error(errors, "'0.64' has no source")

    def test_raw_file_differing_from_its_manifest_sha_fails(self) -> None:
        path = f"{cp.BENCH3}/throughput/r3.json"
        self.assert_error(self.errors(tree=Tampered(change=path)),
                          f"{path}: sha256 differs from manifest.json")

    def test_raw_file_missing_from_the_manifest_fails(self) -> None:
        path = f"{cp.RAW}/2026-09-22-063e4460/throughput/r6.json"
        self.assert_error(self.errors(tree=Tampered(extra=path)),
                          f"{path} is not listed in manifest.json")

    def test_published_throughput_run_must_be_the_median_run(self) -> None:
        tree = Tampered(change="benchmarks/throughput/benchmark_results.json")
        self.assert_error(self.errors(tree=tree),
                          "throughput/benchmark_results.json is not the median run's raw file")


if __name__ == "__main__":
    unittest.main()
