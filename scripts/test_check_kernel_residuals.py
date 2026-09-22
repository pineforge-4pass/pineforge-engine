#!/usr/bin/env python3
"""Self-tests for the P2 kernel-residual gate: it must be able to FAIL.

The gate holds ADR-0001's residual table against the kernel-only archive. A
gate that cannot fail is decoration, so these cases drive the evaluator with
synthetic `strings` / `nm -C` output built from the real ADR: the table alone
passes, a name outside the table fails, a stale ruling fails, and the project's
own name and the ruled broker terms are not residue. With --archive PATH the
suite also runs the real check over a built libpineforge_kernel.a, which is how
the CTest row uses it.

Gap lane P2b adds the cases that make the verdict profile-independent: a `-g`
archive carrying one Pine-named function, one TradingView-named string literal
and one TradingView-named block-scope local answers with the two linkable names
and NOT the local (with the local proven to be in the debug info the strip
removes, so the case cannot pass by the compiler having dropped it); machine
code that `strings` prints as `C0"TV"` is not a name; and a host with no
stripping tool fails CLOSED at exit 2 instead of reading the debug info.

R5 lane F6 (AUDIT3-opus2 H11, AUDIT3-opus kres §1.1) widens what the gate
reads, and these cases are its must-fail half: the audits' blind-spot probes
(`security_lower_tf_probe`, `calc_on_every_tick`, `strategy_entry`,
`syminfo_mintick_probe_fn`, `ta_ema_probe_fn`, the `__close__` sentinel,
`barstate.islast`, `session.ismarket`, `gaps_on`, `lookahead_on`) are caught
as text, as symbols and compiled into a `-g` archive; Pine's parameter words
are residue; the generic look-alikes the kernel really spells
(`strategy_native_*`, `pf_native_lookahead_e`, `ta_misc`, `request_is_buy`,
`session_in_premarket`) are not; a mangled symbol is judged by its demangled
name and a Mach-O underscore is not a second name; and the kernel profile's
INSTALLED headers are read too -- code identifiers and string literals,
never comments -- over exactly the header set the CMake install rule ships.
"""
from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import check_kernel_residuals as guard

ADR_TEXT = guard.ADR.read_text(encoding="utf-8")
RULED = guard.ruled_entries(ADR_TEXT)
ARCHIVE: Path | None = None
CXX: str | None = None

# One translation unit holding exactly three residual-vocabulary names, one per
# surface: a function (a defined symbol), a string literal (rodata), and a
# block-scope local (debug information and nothing else). Its globals are
# neutrally spelled so each finding names one surface only.
PROBE_SOURCE = """
extern "C" const char* pine_probe_symbol();
const char* probe_text = "tv_probe_literal";
extern "C" const char* pine_probe_symbol() {
    int tv_dow = 3;
    return probe_text + (tv_dow - 3);
}
"""


def find_cxx() -> str | None:
    if CXX:
        return CXX
    for candidate in (os.environ.get("CXX"), "c++", "g++", "clang++"):
        if candidate and shutil.which(candidate):
            return candidate
    return None


def build_probe_archive(directory: Path, text: str = PROBE_SOURCE) -> Path:
    """`-g` archive of `text` (PROBE_SOURCE), or a skip when the host cannot build one."""
    compiler = find_cxx()
    if compiler is None:
        raise unittest.SkipTest("no C++ compiler on PATH (pass --cxx)")
    if shutil.which("ar") is None:
        raise unittest.SkipTest("ar is not on PATH")
    source = directory / "residual_probe.cpp"
    source.write_text(text)
    obj = directory / "residual_probe.o"
    archive = directory / "libresidual_probe.a"
    for argv in ([compiler, "-g", "-O0", "-c", str(source), "-o", str(obj)],
                 ["ar", "rcs", str(archive), str(obj)]):
        result = subprocess.run(argv, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(" ".join(argv) + " failed:\n" + result.stderr)
    return archive

# Lines a real archive carries that the vocabulary must NOT flag.
# (expectation corrected: `_strategy_set_syminfo_type` and
# `BacktestEngine::set_syminfo_metadata(...)` were noise -> they are ruled
# vocabulary, because lane F6 reads `syminfo`; they moved to
# RULED_SYMINFO_STRINGS / RULED_SYMINFO_NM below.)
NOISE_STRINGS = [
    "__TEXT", "__cstring", "pending_order_mirror.cpp.o/", "engine_run.cpp.o/",
    "_ZN9pineforge2ta18ema_na_warmup_flagEv",
    "__ZZN9pineforge2ta18ema_na_warmup_flagEvE4flag$tlv$init",
    "_strategy_pending_order_layout",
    "[pineforge] WARNING: session template unknown",
    "delta.x", "data.exchange", "splinelike", "actv_", "cooperative",
]
NOISE_NM = [
    "T pineforge::ta::ema_na_warmup_flag()",
    "s pineforge::ta::ema_na_warmup_flag()::flag",
    "T pineforge::engine_script_run_v18::NativeStrategyHost::on_native_margin_call("
    "pineforge::native_order::native_order_v6::MarginCallEvent const&)",
    "libpineforge_kernel.a(pending_order_mirror.cpp.o):",
]
# The frozen C ABI's symbol-info ingress, which lane F6's `syminfo` word now
# reads, and which ADR-0001 rules by name (Mach-O and ELF spellings both).
RULED_SYMINFO_STRINGS = ["_strategy_set_syminfo_type", "strategy_set_syminfo_type"]
RULED_SYMINFO_NM = [
    "T pineforge::engine_script_run_v18::BacktestEngine::set_syminfo_metadata("
    "std::__1::basic_string<char> const&, double)",
    "T _strategy_set_syminfo_type",
]

# Names and texts the kernel really spells that share a word with Pine but
# are the kernel's own vocabulary: the gate must stay silent on every one.
GENERIC_LOOK_ALIKES = [
    "_strategy_native_cancel_v1", "strategy_closed_trade_exit_id",
    "_strategy_request_abort", "pf_native_request_v1", "request_is_buy",
    "ta_misc", "ta_moving_averages", "ta_bar_index", "pf_native_lookahead_e",
    "PF_NATIVE_LOOKAHEAD_AT_COMPLETION", "PF_NATIVE_GAPS_CLEAR", "pf_native_gaps_e",
    "session_in_premarket", "session_day_index", "chart_bar_ismarket",
    "__GLOBAL__sub_I_timezone.cpp", "l_switch.table.strategy_native_cancel_v1",
    "session.hpp", "include/pineforge/session_time.hpp",
]

# AUDIT3-opus2's r2kernel/inject2/blind_probe.c, verbatim: Pine vocabulary
# the gate's fixed pattern list did not contain until lane F6.
BLIND_PROBE_SOURCE = """
extern "C" const char *pf_blind_text(void) { return "security_lower_tf_probe calc_on_every_tick strategy_entry"; }
extern "C" int syminfo_mintick_probe_fn(int x) { return x + 2; }
extern "C" int ta_ema_probe_fn(int x) { return x + 3; }
"""
BLIND_PROBE_NAMES = ("security_lower_tf_probe", "calc_on_every_tick", "strategy_entry",
                     "syminfo_mintick_probe_fn", "ta_ema_probe_fn")


def table_archive() -> tuple[list[str], list[str]]:
    """A synthetic archive holding exactly the ADR's ruled vocabulary."""
    strings = sorted(RULED.identifiers) + sorted(RULED.phrases) + NOISE_STRINGS
    return strings, list(NOISE_NM)


class RuledTableTests(unittest.TestCase):
    def test_the_adr_table_rules_every_family_the_audit_named(self) -> None:
        names = RULED.identifiers
        # (expectation corrected: every `pine_*` ruled name 17 -> the 17 of
        # the frozen POD, counted by their prefixes, because lane F6's header
        # scan rules the generated-code `pine_*` forwarders beside them.)
        self.assertEqual(sum(1 for n in names if n.startswith(
            ("pine_exit_activation_", "pine_frozen_market_instruction_", "pine_birth_reach"))), 17)
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
        # (expectation corrected: the names IDENTIFIER_PATTERNS match -> those
        # plus the `__name__` labels, because lane F6's sentinel vocabulary
        # reads `__kernel_liquidation__` / `__kernel_risk__` too.)
        self.assertEqual(summary["ruledIdentifiersPresent"],
                         sum(1 for n in RULED.identifiers
                             if any(p.search(n) for _, p in guard.IDENTIFIER_PATTERNS)
                             or guard.SENTINEL[1].match(n)))
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

    def test_the_syminfo_ingress_is_ruled_vocabulary(self) -> None:
        strings, nm = table_archive()
        findings, summary = self.evaluate(strings + RULED_SYMINFO_STRINGS,
                                          nm + RULED_SYMINFO_NM)
        self.assertEqual([f for f in findings if f.kind == "unruled"], [])
        _, before = self.evaluate(*table_archive())
        self.assertEqual(summary["identifierHits"] - before["identifierHits"], 4)

    def test_generic_look_alikes_are_not_residue(self) -> None:
        findings, summary = self.evaluate(list(GENERIC_LOOK_ALIKES),
                                          ["T " + name for name in GENERIC_LOOK_ALIKES])
        self.assertEqual(findings and [str(f) for f in findings if f.kind == "unruled"], [])
        self.assertEqual(summary["hits"], 0, [str(f) for f in findings])


    def test_binary_fragments_are_not_identifiers(self) -> None:
        """Machine code `strings` prints is never a name, but a name still is.

        The Debug archive that failed lane P2's gate answered `TV` out of
        `C0"TV"` and `tV` out of `3)"tV$`. A name is a whole identifier of at
        least guard.MIN_IDENTIFIER_LENGTH bytes, delimited on both sides; the
        floor must not disarm the vocabulary, so the same lines carrying a real
        residual name still fail."""
        fragments = ['C0"TV"', '3)"tV$', 'v."v.$v.6v.Cv.tv.vv.', "0tv", "ptv%"]
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings + fragments, nm)
        self.assertEqual(findings, [], [str(f) for f in findings])
        for fragment in fragments:
            with self.subTest(fragment=fragment):
                findings, _ = self.evaluate(strings + [fragment + " tv_bogus"], nm)
                self.assertEqual([(f.kind, f.token) for f in findings],
                                 [("unruled", "tv_bogus")])

    def test_a_source_file_name_is_not_a_pine_text(self) -> None:
        """`ta.hpp` is this project's header; `ta.ema` is a Pine call.

        The strip does not settle this one: a sanitizer build writes every
        source path into rodata as a real string literal (ASan's global
        descriptors), which is why the ubuntu-24.04 sanitizers job would still
        have answered `ta.hpp` with the debug information already gone."""
        paths = ["include/pineforge/ta.hpp", "src/ta.cpp:148", "strategy.hpp",
                 "/home/runner/work/pineforge-engine/pineforge-engine/"
                 "include/pineforge/ta.hpp", "ta.ipp"]
        # (no `barmerge.*` path: `barmerge` is a residual identifier in its own
        # right, so a file of that stem would fail on the identifier rule and
        # would be a finding, not a carve-out.)
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings + paths, nm)
        self.assertEqual(findings, [], [str(f) for f in findings])
        # The carve-out is the file suffix and nothing else: a Pine call on a
        # line that also names the header still fails, and so does every Pine
        # call whose member merely starts with one of those letters.
        for text in ("include/pineforge/ta.hpp: ta.ema length must be positive",
                     "ta.highest", "strategy.close rejected", "ta.change",
                     "barmerge.gaps_on is not supported here"):
            with self.subTest(text=text):
                findings, _ = self.evaluate(strings + [text], nm)
                self.assertGreaterEqual(len(findings), 1, [str(f) for f in findings])
                self.assertEqual({f.kind for f in findings}, {"unruled"})

    def test_a_finding_is_reported_once_per_name(self) -> None:
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings + ["pine_dup", "pine_dup"],
                                    nm + ["T pineforge::pine_dup()"])
        self.assertEqual(len(findings), 1)


class AuditBlindSpotTests(unittest.TestCase):
    """The probes the third audit's two reviewers passed through the gate."""

    def evaluate(self, strings: list[str], nm: list[str]):
        return guard.evaluate(strings, nm, RULED)

    def unruled(self, strings: list[str], nm: list[str]) -> set[str]:
        findings, _ = self.evaluate(strings, nm)
        return {f.token for f in findings if f.kind == "unruled"}

    def test_opus2_blind_probes_are_caught_as_text_and_as_symbols(self) -> None:
        strings, nm = table_archive()
        # blind_probe.c's literal and its two C symbols, Mach-O and ELF.
        for decoration in ("_", ""):
            with self.subTest(decoration=decoration or "elf"):
                hits = self.unruled(
                    strings + ["security_lower_tf_probe calc_on_every_tick strategy_entry"],
                    nm + ["T " + decoration + "syminfo_mintick_probe_fn",
                          "T " + decoration + "ta_ema_probe_fn"])
                self.assertEqual(hits, set(BLIND_PROBE_NAMES))

    def test_opus_probes_are_caught(self) -> None:
        for line, token in (("__close__", "__close__"),
                            ("__margin_probe__", "__margin_probe__"),
                            ("barstate.islast is not a kernel flag", "barstate"),
                            ("gaps_on", "gaps_on"), ("LOOKAHEAD_ON", "LOOKAHEAD_ON"),
                            ("request_security_probe", "request_security_probe"),
                            ("session_ismarket_probe", "session_ismarket_probe")):
            with self.subTest(line=line):
                strings, nm = table_archive()
                self.assertIn(token, self.unruled(strings + [line], nm))
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings + ["session.ismarket requires a session"], nm)
        self.assertEqual([f.kind for f in findings], ["unruled"])

    def test_pine_parameter_words_are_residue(self) -> None:
        for word in ("over_pyramiding_probe", "default_qty_type", "calc_on_every_tick",
                     "use_bar_magnifier", "backtest_fill_limits_assumption",
                     "close_entries_rule", "fill_orders_on_standard_ohlc", "max_bars_back",
                     "calc_bars_count", "dynamic_requests", "syminfo_probe",
                     "strategy_position_entry_name", "strategy_closedtrades_probe",
                     "ta_highestbars_probe", "barstate_isconfirmed_probe"):
            with self.subTest(word=word):
                strings, nm = table_archive()
                self.assertEqual(self.unruled(strings + [word], nm), {word})

    def test_a_mangled_symbol_is_judged_by_its_demangled_name(self) -> None:
        """`strings` reads the symbol table's mangled spelling, `nm -C` the
        demangled one; one symbol must be one finding, under its own name."""
        strings, nm = table_archive()
        findings, _ = self.evaluate(
            strings + ["__ZN9pineforge2ta14pine_ema_probeEv", "_ZN9pineforge2ta14pine_ema_probeEv"],
            nm + ["T pineforge::ta::pine_ema_probe()"])
        self.assertEqual([(f.kind, f.token) for f in findings], [("unruled", "pine_ema_probe")])

    def test_sanitizer_metadata_is_not_surface(self) -> None:
        """An AddressSanitizer archive names locals in its stack-frame
        descriptions and every `__func__` array in its global descriptors;
        neither is a residual surface, and a real residue beside them is
        still caught."""
        strings, nm = table_archive()
        metadata = ["3 32 8 6 s.addr 64 8 12 session.addr 96 16 11 ref.tmp:701",
                    "2 32 8 15 pyramiding.addr 48 8 10 syminfo.addr:88",
                    "__func__._ZN5Eigen8internal14aligned_mallocEm"]
        findings, summary = self.evaluate(strings + metadata, nm)
        self.assertEqual(findings, [], [str(f) for f in findings])
        self.assertEqual(self.unruled(strings + metadata + ["session.islastbar probe"], nm),
                         {"session.islastbar probe"})
        self.assertEqual(self.unruled(strings + ["session.start must precede its end"], nm),
                         set())

    def test_a_macho_underscore_is_not_a_second_name(self) -> None:
        """Mach-O prefixes every C symbol with `_`, ELF does not: the ruled
        `tv_carry_qty` covers `_tv_carry_qty` and is present through it."""
        strings, nm = table_archive()
        strings.remove("tv_carry_qty")
        findings, summary = self.evaluate(strings + ["_tv_carry_qty"], nm)
        self.assertEqual(findings, [], [str(f) for f in findings])
        _, reference = self.evaluate(*table_archive())
        self.assertEqual(summary["ruledIdentifiersPresent"],
                         reference["ruledIdentifiersPresent"])


class HeaderSurfaceTests(unittest.TestCase):
    """The kernel profile's installed headers are residual surface too."""

    def write_tree(self, root: Path, files: dict[str, str]) -> Path:
        for relative, text in files.items():
            path = root / "pineforge" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        return root

    def header_findings(self, files: dict[str, str]) -> set[str]:
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-headers-") as directory:
            root = self.write_tree(Path(directory), files)
            headers = guard.kernel_profile_headers(root)
            lines = guard.read_headers(root, headers)
        strings, nm = table_archive()
        findings, _ = guard.evaluate(strings, nm, RULED, header_lines=lines)
        return {f.token for f in findings if f.kind == "unruled"}

    def test_the_header_set_is_the_kernel_install_rule(self) -> None:
        excluded = guard.kernel_install_exclusions(guard.CMAKE_LISTS.read_text(encoding="utf-8"))
        self.assertEqual(excluded, frozenset({"source", "compat"}))
        headers = {h.relative_to(guard.ROOT / "include").as_posix()
                   for h in guard.kernel_profile_headers(guard.ROOT / "include")}
        self.assertIn("pineforge/engine.hpp", headers)
        self.assertIn("pineforge/pineforge.h", headers)
        self.assertIn("pineforge/map.hpp", headers)
        self.assertFalse([h for h in headers
                          if h.startswith(("pineforge/source/", "pineforge/compat/"))])

    def test_a_missing_install_rule_is_an_infrastructure_error(self) -> None:
        with self.assertRaises(guard.InfrastructureError):
            guard.kernel_install_exclusions("install(TARGETS pineforge)\n")

    def test_a_pine_named_declaration_fails_and_a_comment_does_not(self) -> None:
        hits = self.header_findings({"probe.hpp": (
            "#pragma once\n"
            "// pine_comment_only strategy_entry_in_a_comment\n"
            "/* tradingview_block_comment */\n"
            "inline int strategy_entry_probe() { return 0; }  // syminfo_trailing\n")})
        self.assertEqual(hits, {"strategy_entry_probe"})

    def test_a_literal_is_read_and_a_compiler_keyword_is_not_a_sentinel(self) -> None:
        hits = self.header_findings({"probe.hpp": (
            "__attribute__((unused)) static const char* kProbe = \"__close_probe__\";\n"
            "static const char* kText = \"session.ismarket needs a session\";\n"
            "#if defined(__clang__)\n#endif\n")})
        self.assertEqual(hits, {"__close_probe__", "session.ismarket needs a session"})

    def test_a_header_the_kernel_profile_does_not_install_is_not_surface(self) -> None:
        hits = self.header_findings({"source/adapter.hpp": "int pine_probe_in_source();\n",
                                     "compat/pine/rule.hpp": "int pine_probe_in_compat();\n"})
        self.assertEqual(hits, set())

    def test_the_installed_headers_carry_no_unruled_name(self) -> None:
        """What the kernel profile installs, read from this tree: every name
        the vocabulary matches has its row (stale rows need the archive and
        are the built-archive case's business)."""
        headers = guard.kernel_profile_headers(guard.ROOT / "include")
        lines = guard.read_headers(guard.ROOT / "include", headers)
        findings, summary = guard.evaluate([], [], RULED, header_lines=lines)
        unruled = [str(f) for f in findings if f.kind == "unruled"]
        self.assertEqual(unruled, [])
        self.assertGreater(summary["headerHits"], 0)


class ArchiveSurfaceTests(unittest.TestCase):
    """What the gate reads out of a real `-g` archive, and what it must not."""

    def probe_hits(self, archive: Path, workdir: Path) -> set[str]:
        strings_lines, nm_lines, tool = guard.read_archive(archive, workdir)
        self.assertIn(tool, {name for name, _ in guard.STRIP_TOOLS})
        findings, _ = guard.evaluate(strings_lines, nm_lines, RULED)
        # Only the unruled half is this archive's business: it holds none of
        # the kernel's ruled names, so every ADR row is legitimately stale here.
        return {f.token for f in findings if f.kind == "unruled"}

    def test_a_debug_local_is_not_linkable_surface_but_a_symbol_and_a_literal_are(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-probe-") as directory:
            work = Path(directory)
            archive = build_probe_archive(work)
            hits = self.probe_hits(archive, work)
        # A defined symbol, whatever decoration the platform gives it: Mach-O
        # prefixes an underscore, ELF does not.
        self.assertTrue([h for h in hits if "pine_probe_symbol" in h], hits)
        self.assertIn("tv_probe_literal", hits)      # a string literal
        self.assertFalse([h for h in hits if "tv_dow" in h],  # debug info only
                         "a block-scope local is not linkable surface")
        self.assertFalse([h for h in hits if "pine_probe_symbol" not in h
                          and "tv_probe_literal" not in h], hits)

    def test_the_audit_blind_probe_archive_fails(self) -> None:
        """opus2's inject2 method: the blind probe compiled into an archive
        (here `-g`, so the strip is exercised too) answers all five names."""
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-probe-") as directory:
            work = Path(directory)
            archive = build_probe_archive(work, BLIND_PROBE_SOURCE)
            hits = self.probe_hits(archive, work)
        for name in BLIND_PROBE_NAMES:
            self.assertTrue([h for h in hits if h.lstrip("_") == name], (name, hits))

    def test_the_local_is_in_the_debug_info_the_strip_removes(self) -> None:
        """Fail-before for the case above: the local IS in the `-g` archive.

        Without this the previous case would pass on an archive whose local the
        compiler never recorded, and the gate's profile-independence would be an
        accident rather than the strip."""
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-probe-") as directory:
            work = Path(directory)
            archive = build_probe_archive(work)
            raw = subprocess.run(["strings", "-a", str(archive)],
                                 capture_output=True, text=True, check=True).stdout
            stripped_lines, _, _ = guard.read_archive(archive, work)
        self.assertIn("tv_dow", raw.split(), "the -g archive must record the local")
        self.assertNotIn("tv_dow", "\n".join(stripped_lines).split())
        self.assertIn("tv_probe_literal", "\n".join(stripped_lines).split())


class StripToolTests(unittest.TestCase):
    def test_a_host_without_a_stripping_tool_fails_closed(self) -> None:
        """No strip tool must be exit 2 with a message, never a debug-info read."""
        original = guard.STRIP_TOOLS
        guard.STRIP_TOOLS = (
            ("absent-objcopy", ("pineforge-absent-objcopy", "--strip-debug",
                                "{input}", "{output}")),)
        try:
            with tempfile.TemporaryDirectory(prefix="pineforge-residual-strip-") as directory:
                archive = Path(directory) / "libprobe.a"
                archive.write_bytes(b"!<arch>\n")
                err, out = io.StringIO(), io.StringIO()
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                    code = guard.main(["--archive", str(archive)])
        finally:
            guard.STRIP_TOOLS = original
        self.assertEqual(code, 2, err.getvalue())
        self.assertIn("no tool to strip debug information", err.getvalue())
        self.assertIn("pineforge-absent-objcopy", err.getvalue())
        self.assertEqual(out.getvalue(), "")


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
    global ARCHIVE, CXX
    argv = list(sys.argv)
    if "--archive" in argv:
        index = argv.index("--archive")
        ARCHIVE = Path(argv[index + 1]).resolve()
        del argv[index:index + 2]
        if not ARCHIVE.is_file():
            raise SystemExit("kernel archive is missing: " + str(ARCHIVE))
    if "--cxx" in argv:
        index = argv.index("--cxx")
        CXX = argv[index + 1]
        del argv[index:index + 2]
    unittest.main(argv=argv)


if __name__ == "__main__":
    main()
