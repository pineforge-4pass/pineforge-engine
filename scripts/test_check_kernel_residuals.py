#!/usr/bin/env python3
"""Self-tests for the P2 kernel-residual gate: it must be able to FAIL.

The gate holds ADR-0001's residual table against the kernel-only archive. A
gate that cannot fail is decoration, so these cases drive the evaluator with
synthetic `strings` / `nm -C` output built from the real ADR: the table alone
passes, a name outside the table fails, a stale ruling fails, and the project's
own name and the ruled broker terms are not residue. With --archive PATH the
suite also runs the real check over a built libpineforge_kernel.a, which is how
the CTest row uses it.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import sys
import unittest

import check_kernel_residuals as guard

ADR_TEXT = guard.ADR.read_text(encoding="utf-8")
RULED = guard.ruled_entries(ADR_TEXT)
ARCHIVE: Path | None = None

# Lines a real archive carries that the vocabulary must NOT flag.
NOISE_STRINGS = [
    "__TEXT", "__cstring", "pending_order_mirror.cpp.o/", "engine_run.cpp.o/",
    "_ZN9pineforge2ta18ema_na_warmup_flagEv",
    "__ZZN9pineforge2ta18ema_na_warmup_flagEvE4flag$tlv$init",
    "_strategy_set_syminfo_type", "_strategy_pending_order_layout",
    "[pineforge] WARNING: session template unknown",
    "delta.x", "data.exchange", "splinelike", "actv_", "cooperative",
]
NOISE_NM = [
    "T pineforge::ta::ema_na_warmup_flag()",
    "s pineforge::ta::ema_na_warmup_flag()::flag",
    "T pineforge::engine_script_run_v18::BacktestEngine::set_syminfo_metadata("
    "std::__1::basic_string<char> const&, double)",
    "T pineforge::engine_script_run_v18::NativeStrategyHost::on_native_margin_call("
    "pineforge::native_order::native_order_v6::MarginCallEvent const&)",
    "T _strategy_set_syminfo_type",
    "libpineforge_kernel.a(pending_order_mirror.cpp.o):",
]


def table_archive() -> tuple[list[str], list[str]]:
    """A synthetic archive holding exactly the ADR's ruled vocabulary."""
    strings = sorted(RULED.identifiers) + sorted(RULED.phrases) + NOISE_STRINGS
    return strings, list(NOISE_NM)


class RuledTableTests(unittest.TestCase):
    def test_the_adr_table_rules_every_family_the_audit_named(self) -> None:
        names = RULED.identifiers
        self.assertEqual(sum(1 for n in names if n.startswith("pine_")), 17)
        self.assertEqual(sum(1 for n in names if "coof" in n), 7)
        self.assertEqual(sum(1 for n in names if "pooc" in n), 3)
        self.assertEqual(
            sum(1 for n in names if n.startswith("market_admission_observation_")), 68)
        self.assertEqual(sum(1 for n in names if n.startswith("market_admission_review_")), 5)
        self.assertEqual(
            sum(1 for n in names if n.startswith("market_admission_sizing_revision_")), 5)
        self.assertIn("tv_carry_qty", names)
        self.assertEqual(
            sum(1 for p in RULED.phrases if "request.security" in p), 3)

    def test_citations_in_the_first_column_are_not_rulings(self) -> None:
        self.assertFalse(any("/" in token for token in RULED.identifiers | RULED.phrases))

    def test_a_missing_section_is_an_infrastructure_error(self) -> None:
        with self.assertRaises(guard.InfrastructureError):
            guard.ruled_entries("# ADR without the residual section\n")


class EvaluatorTests(unittest.TestCase):
    def evaluate(self, strings: list[str], nm: list[str]):
        return guard.evaluate(strings, nm, RULED)

    def test_the_table_alone_passes(self) -> None:
        findings, summary = self.evaluate(*table_archive())
        self.assertEqual(findings, [], [str(f) for f in findings])
        self.assertEqual(summary["ruledIdentifiersPresent"],
                         sum(1 for n in RULED.identifiers
                             if any(p.search(n) for _, p in guard.IDENTIFIER_PATTERNS)))
        self.assertEqual(summary["phraseHits"], 3)

    def test_a_name_outside_the_table_fails(self) -> None:
        for intruder in ("market_admission_observation_bogus", "coof_bogus", "pooc_bogus",
                         "pine_bogus_field", "tv_bogus", "PineMatrixOps",
                         "barmerge_gaps", "calc_on_order_fills_flag",
                         "process_orders_on_close_mode", "tradingview_mode"):
            with self.subTest(intruder=intruder):
                strings, nm = table_archive()
                findings, _ = self.evaluate(strings + [intruder], nm)
                self.assertEqual([f.kind for f in findings], ["unruled"])
                self.assertEqual(findings[0].token, intruder)

    def test_a_symbol_outside_the_table_fails(self) -> None:
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings, nm + ["T pineforge::ta::pine_ema_seed()"])
        self.assertEqual([(f.kind, f.token) for f in findings], [("unruled", "pine_ema_seed")])

    def test_a_text_with_pine_vocabulary_fails(self) -> None:
        for text in ("strategy.entry rejected: qty <= 0", "ta.ema length must be positive",
                     "barmerge.gaps_on is not supported here", "__margin_call__",
                     "native request.security_lower_tf feed is unsupported"):
            with self.subTest(text=text):
                strings, nm = table_archive()
                findings, _ = self.evaluate(strings + [text], nm)
                # A dotted spelling is caught as a text; when its stem is also
                # an identifier of the vocabulary (barmerge.) both fire.
                self.assertGreaterEqual(len(findings), 1, [str(f) for f in findings])
                self.assertEqual({f.kind for f in findings}, {"unruled"})

    def test_the_ruled_texts_pass_by_phrase(self) -> None:
        strings, nm = table_archive()
        findings, summary = self.evaluate(strings, nm)
        self.assertEqual(findings, [])
        self.assertEqual(summary["ruledPhrasesCovered"], 3)

    def test_a_stale_ruling_fails(self) -> None:
        strings, nm = table_archive()
        strings.remove("pine_birth_reach")
        findings, _ = self.evaluate(strings, nm)
        self.assertEqual([(f.kind, f.token) for f in findings], [("stale", "pine_birth_reach")])
        strings, nm = table_archive()
        gone = next(p for p in RULED.phrases if "strictly increasing" in p)
        strings.remove(gone)
        findings, _ = self.evaluate(strings, nm)
        self.assertEqual([(f.kind, f.token) for f in findings], [("stale", gone)])

    def test_project_name_and_broker_terms_are_not_residue(self) -> None:
        findings, summary = self.evaluate(list(NOISE_STRINGS), list(NOISE_NM))
        self.assertEqual([f for f in findings if f.kind == "unruled"], [])
        self.assertEqual(summary["hits"], 0)

    def test_a_finding_is_reported_once_per_name(self) -> None:
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings + ["pine_dup", "pine_dup"],
                                    nm + ["T pineforge::pine_dup()"])
        self.assertEqual(len(findings), 1)


class BuiltArchiveTests(unittest.TestCase):
    def test_the_built_kernel_archive_passes(self) -> None:
        if ARCHIVE is None:
            self.skipTest("no --archive given")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = guard.check(ARCHIVE, guard.ADR)
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn("0 findings ... OK", out.getvalue())


def main() -> None:
    global ARCHIVE
    argv = list(sys.argv)
    if "--archive" in argv:
        index = argv.index("--archive")
        ARCHIVE = Path(argv[index + 1]).resolve()
        del argv[index:index + 2]
        if not ARCHIVE.is_file():
            raise SystemExit("kernel archive is missing: " + str(ARCHIVE))
    unittest.main(argv=argv)


if __name__ == "__main__":
    main()
