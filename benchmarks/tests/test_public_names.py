#!/usr/bin/env python3
"""No committed result names a closed slot by its directory, and the gate column is the rubric's.

A closed slot's directory is ``NNN-<author>-<title>``: the TradingView
author's handle is in it. Every harness step that writes a committed file
prints the slot as ``NNN-closed`` instead (``paths.public_name``, the Google
Benchmark harness, ``select_population.public_manifest``), and the committed
results must hold no other spelling of a closed slot.

compare.py prints a non-excellent row's failing gates from the rubric's gate
booleans: ``analyze_strategy``'s ``notes`` carry a probe's declared note when
its inputs.json has one, which is prose about PineForge's history on the
probe, not the graded engine's gates. A slot without ``strategy_pyne.py``
reads its PyneSys compile error from the request ledger or, without one,
from the committed request log.

    python3 benchmarks/tests/test_public_names.py
"""
from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "benchmarks"))
import compare  # noqa: E402
import paths  # noqa: E402
import select_population  # noqa: E402

RESULTS = REPO_ROOT / "benchmarks" / "results"
CLOSED_NAME = re.compile(r"^(1\d\d|20\d)-closed$")
# A slot key of a committed file: three digits that start a token (not the
# "240" inside a probe name such as ta-tf-240-rising-01), a dash, and a name
# with a letter in it (a closed name may start with a digit; a range such as
# 101-201 is not a key).
SLOT_KEY = re.compile(r"(?<![\w-])(\d{3})-(?=[\w.-]*[A-Za-z])([\w.-]+)")


def closed_keys(text: str) -> set[str]:
    """Every slot key 101..299 in ``text`` that is not ``NNN-closed``."""
    return {m[0] for m in SLOT_KEY.finditer(text)
            if 101 <= int(m[1]) <= 299 and not m[2].startswith("closed")}


class PublicName(unittest.TestCase):
    def test_closed_slot_prints_its_number_only(self) -> None:
        slot = paths.CLOSED_STRATEGIES / "123-some-author-some-title"
        self.assertEqual(paths.public_name(slot), "123-closed")

    def test_public_slot_keeps_its_directory_name(self) -> None:
        slot = paths.STRATEGIES / "001-analyzer-anvil-percent-costs-01"
        self.assertEqual(paths.public_name(slot), slot.name)

    def test_public_manifest_drops_the_handles(self) -> None:
        full = {"slots": [
            {"slot": "001-probe", "source": "corpus", "probeId": "corpus:x", "replacements": ["y"]},
            {"slot": "150-author-title", "source": "closed", "probeId": "scrapper:data/standard/author-title",
             "replacements": ["other-author-title"], "lost": {"reason": "r"}},
            {"slot": "201-author2-title2", "source": "closed", "probeId": "scrapper:data/standard/author2-title2",
             "replaces": "150-author-title", "replacements": []},
        ]}
        public = select_population.public_manifest(full)
        self.assertEqual(public["slots"][0], full["slots"][0])
        self.assertEqual(public["slots"][1], {"slot": "150-closed", "source": "closed",
                                              "lost": {"reason": "r"}})
        self.assertEqual(public["slots"][2], {"slot": "201-closed", "source": "closed",
                                              "replaces": "150-closed"})
        self.assertIn("author", json.dumps(full))  # the input is not mutated


class CommittedResults(unittest.TestCase):
    def test_selection_names_closed_slots_by_number(self) -> None:
        selection = json.loads((RESULTS / "selection.json").read_text(encoding="utf-8"))
        closed = [s for s in selection["slots"] if s["source"] == "closed"]
        self.assertTrue(closed)
        for s in closed:
            self.assertRegex(s["slot"], CLOSED_NAME)
            self.assertNotIn("probeId", s)
            self.assertNotIn("replacements", s)
            if "replaces" in s:
                self.assertRegex(s["replaces"], CLOSED_NAME)

    def test_no_committed_result_spells_a_closed_slot_otherwise(self) -> None:
        files = [p for p in RESULTS.rglob("*") if p.is_file() and p.suffix in (".md", ".json", ".tsv")]
        files += [REPO_ROOT / "benchmarks" / "throughput" / "benchmark_results.json"]
        for path in files:
            with self.subTest(path=str(path.relative_to(REPO_ROOT))):
                self.assertEqual(closed_keys(path.read_text(encoding="utf-8")), set())


class GateColumn(unittest.TestCase):
    def result(self, **kw) -> "compare.verify_corpus.VerificationResult":
        base = dict(strategy_dir=Path("x"), rel="x", label="strong", count_ok=True, entry_ok=True,
                    exit_ok=True, pnl_ok=True, coverage_ok=True)
        base.update(kw)
        return compare.verify_corpus.VerificationResult(**base)

    def test_failing_gates_come_from_the_gate_booleans(self) -> None:
        r = self.result(coverage_ok=False, coverage=0.982,
                        notes="Excellent (224/224 matched). A declared note about the probe.")
        self.assertEqual(compare.failing_gates(r), "coverage 98.2%")

    def test_every_failing_gate_is_named_in_rubric_order(self) -> None:
        r = self.result(label="weak", count_ok=False, count_delta=0.1665, pnl_ok=False, pnl_p90=0.5,
                        distinct_entry_identity_ok=False, distinct_entry_mismatches=3)
        self.assertEqual(compare.failing_gates(r),
                         "count Δ 16.65%; pnl p90 50.0000%; distinct-entry multiplicity Δ 3")

    def test_no_aligned_trades(self) -> None:
        r = self.result(label="minimal", no_aligned_trades=True, notes="declared")
        self.assertEqual(compare.failing_gates(r), "no aligned trades")


class CompileErrors(unittest.TestCase):
    LOG = """\
| # | UTC | Batch | Kind | Slot | Outcome | Message (first line) | Seconds |
|---:|---|---|---|---|---|---|---:|
| 1 | 2026-09-21T13:36:47Z | 0-usage | usage |  | ok | API Usage | 1.2 |
| 2 | 2026-09-21T13:36:57Z | 1 | compile | 150-closed | compile-error | {"detail":"bad"} | 1.3 |
| 3 | 2026-09-21T13:37:02Z | 1 | compile | 151-closed | compile-error | {"detail":"worse"} | 1.3 |
| 4 | 2026-09-21T13:37:07Z | 2 | compile | 151-closed | ok |  | 1.1 |
"""

    def test_committed_log_serves_without_the_ledger(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "pynesys-compile-log.md"
            log.write_text(self.LOG, encoding="utf-8")
            saved = compare.COMPILE_LEDGER, compare.COMPILE_LOG
            compare.COMPILE_LEDGER, compare.COMPILE_LOG = Path(tmp) / "absent.jsonl", log
            try:
                self.assertEqual(compare.compile_errors(), {150: '{"detail":"bad"}'})
            finally:
                compare.COMPILE_LEDGER, compare.COMPILE_LOG = saved

    def test_the_committed_log_names_the_rejected_slot(self) -> None:
        saved = compare.COMPILE_LEDGER
        compare.COMPILE_LEDGER = RESULTS / "absent.jsonl"
        try:
            errors = compare.compile_errors()
        finally:
            compare.COMPILE_LEDGER = saved
        self.assertEqual(sorted(errors), [192])
        self.assertIn("Empty document.", errors[192])


if __name__ == "__main__":
    unittest.main()
