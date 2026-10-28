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

Lane INT16b (PR #278, Linux kernel-only) adds the kernel-source surface: GCC 13
on x86-64 split `__kernel_liquidation__` into a vector constant and an
immediate, so its archive's `strings` never carries the label and the ruling
read stale on that platform alone. The cases reproduce that strings line: stale
without the source, live through the TU's literal, still stale when no surface
carries it, never live through a comment or an include path; the surface is
judged in both directions, and it is exactly the archive's own TU list.

R5 lane H-DOCGATES (AUDIT4-opus X12, kres K4/K5/K11) closes the reach gaps
the fourth audit probed: a ruled NAME never covers a TEXT (the bare-word
rulings `Pine` and `syminfo` passed "match Pine semantics exactly" and
"syminfo.tickerid must be set"), so a text is ruled by its exact words; the
vocabulary reads Pine's other dotted namespaces (`timeframe.`, `input.`,
`str.`, `math.`, `matrix.`, `line.`, `xloc.`, ...), the camel-case
`Tv`/`TV` spelling, the bare session-bar flags and the chart-type and
trade-date built-ins; a row whose second column says "no archive symbol" is
held to it; the printed counts are the rulings the vocabulary reads; and the
counts have floors that scripts/ci_verify.py passes.

CR-352 adds bare-line corroboration controls: the machine-code-shaped TvJO
line is ignored without independent evidence, while symbols, installed
headers and compiled escaped/adjacent literals still fail. The legacy
source-literal findings and ruled-row presence remain independently tested.

Compiler-free object/archive byte fixtures check that executable bytes stay
outside the strings surface; unknown data and nm names remain visible.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
    obj = directory / (source.name + ".o")
    archive = directory / "libresidual_probe.a"
    for argv in ([compiler, "-std=c++17", "-g", "-O0", "-c", str(source), "-o", str(obj)],
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
    "T pineforge::engine_script_run_v19::NativeStrategyHost::on_native_margin_call("
    "pineforge::native_order::native_order_v8::MarginCallEvent const&)",
    "libpineforge_kernel.a(pending_order_mirror.cpp.o):",
]
# The frozen C ABI's symbol-info ingress, which lane F6's `syminfo` word now
# reads, and which ADR-0001 rules by name (Mach-O and ELF spellings both).
RULED_SYMINFO_STRINGS = ["_strategy_set_syminfo_type", "strategy_set_syminfo_type"]
RULED_SYMINFO_NM = [
    "T pineforge::engine_script_run_v19::BacktestEngine::set_syminfo_metadata("
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
    """A synthetic archive holding exactly the ADR's ruled vocabulary -- all of
    it but the header-only rows (R5 lane H-DOCGATES), which table_headers()
    ships where the kernel profile ships them."""
    strings = (sorted(RULED.identifiers - RULED.header_only)
               + sorted(RULED.phrases - RULED.header_only - RULED.debug_only) + NOISE_STRINGS)
    return strings, list(NOISE_NM)


def table_headers() -> tuple[list[str], list[str], list[str], list[str]]:
    """The header-only rulings on the installed-header surface: each name as a
    code line, each text as a literal (read_headers' four lists)."""
    names = sorted(RULED.header_only & RULED.identifiers)
    texts = sorted(RULED.header_only & RULED.phrases)
    return (names, [f"pineforge/table.hpp:{n}" for n in range(1, len(names) + 1)],
            texts, [f"pineforge/table.hpp:{n}" for n in range(1, len(texts) + 1)])


def evaluate_table(strings: list[str], nm: list[str], **surfaces):
    """guard.evaluate with the header-only rulings shipped in the headers,
    unless the case brings its own header surface."""
    surfaces.setdefault("header_lines", table_headers())
    return guard.evaluate(strings, nm, RULED, **surfaces)


def ar_bytes(members: list[tuple[str, bytes]]) -> bytes:
    """Hand-build an ar archive, preserving repeated short member names."""
    output = bytearray(b"!<arch>\n")
    for name, contents in members:
        if len(name.encode("ascii")) > 15:
            raise ValueError("fixture requires a short ar member name")
        header = (f'{name + "/":<16}{0:<12}{0:<6}{0:<6}'
                  f'{"100644":<8}{len(contents):<10}`\n').encode("ascii")
        output.extend(header)
        output.extend(contents)
        if len(contents) % 2:
            output.extend(b"\n")
    return bytes(output)


def macho_object(sections: list[tuple[str, bytes, int, str]], *,
                 is_64: bool = True, endian: str = "<",
                 cputype: int | None = None) -> bytes:
    """Hand-built Mach-O: section name, bytes/zerofill size, flags, segment."""
    header_size = 32 if is_64 else 28
    command_size = (72 if is_64 else 56) + (80 if is_64 else 68) * len(sections)
    payload_offset = header_size + command_size
    payload = bytearray()
    section_table = bytearray()
    address = 0
    for name, contents, flags, segment in sections:
        zerofill = (flags & 0xFF) in (1, 0xC, 0x12)
        offset = 0 if zerofill else payload_offset + len(payload)
        fields = [
            name.encode("ascii").ljust(16, b"\0"),
            segment.encode("ascii").ljust(16, b"\0"),
            address, len(contents), offset, 0, 0, 0, flags, 0, 0,
        ]
        if is_64:
            fields.append(0)
        section_table.extend(struct.pack(
            endian + ("16s16sQQIIIIIIII" if is_64 else "16s16sIIIIIIIII"),
            *fields))
        if not zerofill:
            payload.extend(contents)
        address += len(contents)
    # MH_OBJECT's unnamed segment permits sections with different segnames.
    segment = struct.pack(
        endian + ("II16sQQQQiiII" if is_64 else "II16sIIIIiiII"),
        0x19 if is_64 else 0x1, command_size, b"\0" * 16,
        0, address, payload_offset, len(payload), 7, 5,
        len(sections), 0)
    if cputype is None:
        cputype = 0x0100000C if is_64 else 12
    subtype = 3 if cputype == 0x01000007 else 0
    header_fields = [
        0xfeedfacf if is_64 else 0xfeedface, cputype, subtype, 1, 1, command_size, 0,
    ]
    if is_64:
        header_fields.append(0)
    header = struct.pack(endian + ("IIIIIIII" if is_64 else "IIIIIII"),
                         *header_fields)
    return header + segment + bytes(section_table) + bytes(payload)


def macho64(sections: list[tuple[str, bytes]], *,
            cputype: int = 0x0100000C) -> bytes:
    """A compiler-free 64-bit little-endian Mach-O object fixture."""
    return macho_object([
        (name, contents,
         0x80000400 if name == "__text" else 0x2 if name == "__cstring" else 0,
         "__TEXT")
        for name, contents in sections
    ], cputype=cputype)


def fat_macho(slices: list[tuple[int, bytes]], *, is_64: bool = False,
              endian: str = ">") -> bytes:
    """Hand-build fat_arch or fat_arch_64 records around thin slices."""
    header_size = 8 + (32 if is_64 else 20) * len(slices)
    offset = header_size
    entries: list[bytes] = []
    locations: list[tuple[int, bytes]] = []
    for cputype, contents in slices:
        offset = (offset + 7) & ~7
        subtype = 3 if cputype == 0x01000007 else 0
        fields = [cputype, subtype, offset, len(contents), 3]
        if is_64:
            fields.append(0)
        entries.append(struct.pack(endian + ("IIQQII" if is_64 else "IIIII"),
                                   *fields))
        locations.append((offset, contents))
        offset += len(contents)
    output = bytearray(struct.pack(
        endian + "II", 0xcafebabf if is_64 else 0xcafebabe, len(slices)))
    output.extend(b"".join(entries))
    for location, contents in locations:
        output.extend(b"\0" * (location - len(output)))
        output.extend(contents)
    return bytes(output)


def elf_object(sections: list[tuple[str, bytes, int, int]], *,
               is_64: bool = True, endian: str = "<",
               extended_count: bool = False, extended_names: bool = False) -> bytes:
    """Hand-built ELF: section name, bytes/NOBITS size, flags, type.

    Small extended-count fixtures deliberately use section zero's escape
    fields without allocating tens of thousands of section headers.
    """
    names = b"\0" + b"".join(name.encode("ascii") + b"\0" for name, _, _, _ in sections)
    shstr_name_offset = len(names)
    names += b".shstrtab\0"
    name_offsets: dict[str, int] = {}
    cursor = 1
    for name, _, _, _ in sections:
        name_offsets[name] = cursor
        cursor += len(name) + 1
    name_offsets[".shstrtab"] = shstr_name_offset

    header_size = 64 if is_64 else 52
    section_size = 64 if is_64 else 40
    section_format = endian + ("IIQQQQIIQQ" if is_64 else "IIIIIIIIII")
    section_count = len(sections) + 2
    output = bytearray(b"\0" * header_size)
    section_headers: list[bytes] = [
        struct.pack(section_format, 0, 0, 0, 0, 0,
                    section_count if extended_count else 0,
                    section_count - 1 if extended_names else 0, 0, 0, 0)]
    for name, contents, flags, section_type in sections:
        entry_size = (1 if flags & 0x20 else 16) if flags & 0x10 else 0
        alignment = entry_size or 1
        output.extend(b"\0" * (-len(output) % alignment))
        if entry_size:
            contents += b"\0" * (-len(contents) % entry_size)
        offset = 0 if section_type == 8 else len(output)
        if section_type != 8:
            output.extend(contents)
        section_headers.append(struct.pack(
            section_format, name_offsets[name], section_type, flags, 0, offset, len(contents),
            0, 0, alignment, entry_size))
    shstr_offset = len(output)
    output.extend(names)
    section_headers.append(struct.pack(
        section_format, name_offsets[".shstrtab"], 3, 0, 0, shstr_offset, len(names),
        0, 0, 1, 0))
    output.extend(b"\0" * (-len(output) % 8))
    section_table_offset = len(output)
    output.extend(b"".join(section_headers))
    ident = b"\x7fELF" + bytes((2 if is_64 else 1, 1 if endian == "<" else 2, 1, 0)) + b"\0" * 8
    machine = (62 if endian == "<" else 21) if is_64 else (3 if endian == "<" else 20)
    output[:header_size] = struct.pack(
        endian + ("16sHHIQQQIHHHHHH" if is_64 else "16sHHIIIIIHHHHHH"),
        ident, 1, machine, 1, 0, 0, section_table_offset, 0,
        header_size, 0, 0, section_size, 0 if extended_count else section_count,
        0xFFFF if extended_names else section_count - 1)
    return bytes(output)


def elf64(sections: list[tuple[str, bytes, int]]) -> bytes:
    """A compiler-free 64-bit little-endian ELF relocatable fixture."""
    return elf_object([(name, contents, flags, 1) for name, contents, flags in sections])


class SanitizerAndDebugBuildTests(unittest.TestCase):
    """R5 lane H-DOCGATES: the sanitizers profile's first run of the dotted-
    namespace vocabulary (lab remote job rj-20260925t190344-64918a) found two
    strings a release archive does not carry: UBSan's quoted type name
    `'struct SymInfo'` and Eigen's assert text `matrix.cols() ==
    matrix.rows()`."""

    def test_a_sanitizer_type_name_is_judged_as_the_name(self) -> None:
        for line in ("'struct SymInfo'", "'pineforge::SymInfo'"):
            with self.subTest(line=line):
                strings, nm = table_archive()
                findings, _ = evaluate_table(strings + [line], nm)
                self.assertEqual(findings, [], [str(f) for f in findings])
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings + ["'struct PineBogusType'"], nm)
        self.assertEqual([(f.kind, f.token) for f in findings], [("unruled", "PineBogusType")])

    def test_a_quoted_text_is_still_a_text(self) -> None:
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings + ["'match Pine semantics exactly'"], nm)
        self.assertGreaterEqual(len(findings), 1)
        self.assertEqual({f.kind for f in findings}, {"unruled"})

    def test_the_debug_only_assert_text(self) -> None:
        text = "matrix.cols() == matrix.rows()"
        self.assertIn(text, RULED.debug_only)
        strings, nm = table_archive()
        self.assertNotIn(text, strings)                 # a release archive: not stale
        findings, _ = evaluate_table(strings, nm)
        self.assertEqual(findings, [], [str(f) for f in findings])
        findings, summary = evaluate_table(strings + [text], nm)   # a debug archive: covered
        self.assertEqual(findings, [], [str(f) for f in findings])
        self.assertEqual(summary["phraseHits"], 22)

    def test_only_a_marked_row_may_be_absent(self) -> None:
        adr = guard.ADR.read_text(encoding="utf-8")
        row = "so the text is " + guard.DEBUG_ONLY_MARK
        self.assertEqual(adr.count(row), 1)
        adr = adr.replace(row, "so the text is absent")
        ruled = guard.ruled_entries(adr)
        self.assertNotIn("matrix.cols() == matrix.rows()", ruled.debug_only)
        strings, nm = table_archive()
        findings, _ = guard.evaluate(strings, nm, ruled, header_lines=table_headers())
        self.assertEqual([(f.kind, f.token) for f in findings],
                         [("stale", "matrix.cols() == matrix.rows()")])

    def test_an_unruled_assert_text_still_fails(self) -> None:
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings + ["map.size() == map.capacity()"], nm)
        self.assertEqual([f.kind for f in findings], ["unruled"])


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
            sum(1 for p in RULED.phrases if "request.security" in p), 5)

    def test_citations_in_the_first_column_are_not_rulings(self) -> None:
        self.assertFalse(any("/" in token for token in RULED.identifiers | RULED.phrases))

    def test_a_missing_section_is_an_infrastructure_error(self) -> None:
        with self.assertRaises(guard.InfrastructureError):
            guard.ruled_entries("# ADR without the residual section\n")


class EvaluatorTests(unittest.TestCase):
    def evaluate(self, strings: list[str], nm: list[str], **surfaces):
        return evaluate_table(strings, nm, **surfaces)

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
        self.assertEqual(summary["phraseHits"], 21)

    def test_a_name_outside_the_table_fails(self) -> None:
        for intruder in ("market_admission_observation_bogus", "coof_bogus", "pooc_bogus",
                         "pine_bogus_field", "tv_bogus", "PineMatrixOps",
                         "barmerge_gaps", "calc_on_order_fills_flag",
                         "process_orders_on_close_mode", "tradingview_mode"):
            with self.subTest(intruder=intruder):
                strings, nm = table_archive()
                # A real one-name literal has independent source evidence.
                findings, _ = self.evaluate(
                    strings + [intruder], nm,
                    source_literals=([intruder], ["src/probe.cpp:1"]))
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
        # (expectation corrected: 3 -> every ruled text the vocabulary reads,
        # because R5 lane H-DOCGATES rules each text by its exact words: the
        # three feed texts, the deprecated aliases' texts, map.hpp's
        # static_assert texts and the runtime's dotted argument checks; the
        # table archive models a release archive, which carries no assert
        # text, so the debug-only row is not among them.)
        self.assertEqual(summary["ruledPhrasesCovered"],
                         sum(1 for p in RULED.phrases - RULED.debug_only
                             if guard.vocabulary_text(p)))

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


class BareStringsTests(unittest.TestCase):
    """Printable instructions need independent evidence of a name."""

    def test_an_uncorroborated_bare_identifier_is_not_a_finding(self) -> None:
        # RED on b7e16f56: each line is an unruled identifier via strings.
        # Two unrelated spellings prevent a token-specific workaround.
        for line in ("TvJO", "TvQZ", " \tTvJO "):
            with self.subTest(line=line):
                strings, nm = table_archive()
                findings, summary = evaluate_table(strings + [line], nm)
                self.assertEqual(findings, [], [str(f) for f in findings])
                self.assertEqual(summary["findings"], 0)
                _, baseline = evaluate_table(strings, nm)
                self.assertEqual(summary["identifierHits"], baseline["identifierHits"] + 1)

    def test_nm_corroborates_a_bare_string_after_symbol_normalization(self) -> None:
        for token in ("TvJO", "tv_foo", "tv_"):
            for decoration in ("", "_"):
                with self.subTest(token=token, decoration=decoration):
                    strings, nm = table_archive()
                    findings, _ = evaluate_table(
                        strings + [token], nm + ["T " + decoration + token])
                    self.assertEqual([(f.kind, f.token) for f in findings],
                                     [("unruled", token)])
                    # A surviving nm finding alone would not prove corroboration.
                    self.assertIn("via strings", findings[0].detail)

    def test_an_nm_only_residual_still_fails(self) -> None:
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings, nm + ["T tv_foo"])
        self.assertEqual([(f.kind, f.token) for f in findings], [("unruled", "tv_foo")])
        self.assertIn("via nm", findings[0].detail)

    def test_installed_code_and_literal_tokens_corroborate_archive_names(self) -> None:
        for surface in ("header", "header-literal"):
            with self.subTest(surface=surface):
                code, code_where, literals, literal_where = table_headers()
                if surface == "header":
                    code.append("int TvJO();")
                    code_where.append("pineforge/probe.hpp:1")
                else:
                    literals.append("TvJO")
                    literal_where.append("pineforge/probe.hpp:1")
                strings, nm = table_archive()
                findings, _ = evaluate_table(
                    strings + ["TvJO"], nm,
                    header_lines=(code, code_where, literals, literal_where))
                self.assertEqual([(f.kind, f.token) for f in findings],
                                 [("unruled", "TvJO")])
                self.assertIn("via strings", findings[0].detail)

    def test_texts_and_separators_do_not_need_corroboration(self) -> None:
        for text in ("xx TvJO yy", "xx TvJOabc yy", "TvJO.cold", "TvJO::probe"):
            with self.subTest(text=text):
                strings, nm = table_archive()
                findings, _ = evaluate_table(strings + [text], nm)
                token = "TvJOabc" if text == "xx TvJOabc yy" else "TvJO"
                self.assertEqual([(f.kind, f.token) for f in findings],
                                 [("unruled", token)])
                self.assertIn("via strings", findings[0].detail)

    def test_a_phrase_hit_on_a_bare_line_is_still_judged(self) -> None:
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings + ["Pine"], nm)
        phrase_findings = [f for f in findings
                           if "text matches residual vocabulary 'Pine prose'" in f.detail]
        self.assertEqual([(f.kind, f.token) for f in phrase_findings],
                         [("unruled", "Pine")])

    def test_a_ruled_bare_string_stays_present_without_corroboration(self) -> None:
        strings, nm = table_archive()
        self.assertIn("tv_carry_qty", strings)
        self.assertNotIn("tv_carry_qty",
                         {guard.symbol_name(h.token, h.source)
                          for h in guard.scan(nm, "nm")})
        self.assertNotIn("tv_carry_qty", table_headers()[0] + table_headers()[2])
        findings, summary = evaluate_table(strings, nm)
        self.assertEqual(findings, [], [str(f) for f in findings])
        strings.remove("tv_carry_qty")
        findings, absent = evaluate_table(strings, nm)
        self.assertEqual([(f.kind, f.token) for f in findings],
                         [("stale", "tv_carry_qty")])
        self.assertEqual(summary["ruledIdentifiersPresent"],
                         absent["ruledIdentifiersPresent"] + 1)


class ObjectSectionTests(unittest.TestCase):
    """The archive string surface is limited to literal-bearing sections."""

    @staticmethod
    def unruled_tokens(strings: list[str], nm: list[str]) -> set[str]:
        findings, _ = guard.evaluate(strings, nm, RULED)
        return {finding.token for finding in findings if finding.kind == "unruled"}

    def read_fixture(self, data: bytes, directory: Path, *,
                     symbols: str = "", code_control: bool = True
                     ) -> tuple[list[str], list[str]]:
        return self.read_archive_fixture(
            ar_bytes([("probe.o", data)]), directory,
            symbols=symbols, code_control=code_control)

    def read_archive_fixture(self, data: bytes, directory: Path, *,
                             symbols: str = "", code_control: bool = False,
                             stripped_data: bytes | None = None
                             ) -> tuple[list[str], list[str]]:
        """Exercise the old and new read_archive API without compiling.

        Only stripping (the fixture has no debug data) and nm (synthetic symbol
        output) are stubbed. Real ar/strings read a standard archive containing
        hand-built members, including every architecture of a fat member.
        """
        directory.mkdir()
        archive = directory / "probe.a"
        archive.write_bytes(data)
        run_tool = guard.run_tool
        if code_control:
            # Control: the old whole-archive operation sees the code word.
            old_lines = run_tool(["strings", "-a", str(archive)]).splitlines()
            self.assertIn("code TvJO bytes", old_lines)
            self.assertIn("TvJO", self.unruled_tokens(old_lines, []))

        def copy_fixture(source: Path, destination: Path) -> str:
            if stripped_data is None:
                shutil.copyfile(source, destination)
            else:
                destination.write_bytes(stripped_data)
            return "fixture-no-debug"

        def fixture_tool(argv: list[str]) -> str:
            if argv[0] == "nm":
                self.assertEqual(argv, ["nm", "-C", str(archive)])
                return symbols
            return run_tool(argv)

        with mock.patch.object(guard, "strip_debug_info", side_effect=copy_fixture), \
                mock.patch.object(guard, "run_tool", side_effect=fixture_tool):
            strings, nm, tool = guard.read_archive(archive, directory)
        self.assertEqual(tool, "fixture-no-debug")
        self.assertEqual(nm, symbols.splitlines())
        return strings, nm

    def test_machine_code_bytes_in_text_are_ignored(self) -> None:
        """Must fail on the old read_archive's whole-archive strings scan."""
        thin = macho64([("__text", b"\0code TvJO bytes\0")])
        fixtures = (
            ("Mach-O thin arm64", thin),
            ("Mach-O fat arm64", fat_macho([
                (0x01000007, macho64([("__text", b"neutral\0")],
                                    cputype=0x01000007)),
                (0x0100000C, thin),
            ])),
            ("ELF", elf64([
                (".text", b"\0code TvJO bytes\0", 0x6),
                (".text.hot", b"\0code TvJO bytes\0", 0x6),
                (".text.unlikely", b"\0code TvJO bytes\0", 0x6),
                (".rodata", b"neutral\0", 0x2),
            ])),
        )
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-sections-") as directory:
            root = Path(directory)
            for label, object_bytes in fixtures:
                with self.subTest(label=label):
                    strings, nm = self.read_fixture(object_bytes, root / label)
                    self.assertNotIn("TvJO", "\n".join(strings))
                    self.assertNotIn("TvJO", self.unruled_tokens(strings, nm))

    def test_data_sections_and_symbols_are_still_flagged(self) -> None:
        """Keep data/symbol findings while discarding code in the same object."""
        fixtures = (
            ("Mach-O __cstring", macho64([
                ("__text", b"\0code TvJO bytes\0"),
                ("__cstring", b"xx TvJO yy\0"),
            ]), ""),
            ("Mach-O __const", macho64([
                ("__text", b"\0code TvJO bytes\0"),
                ("__const", b"xx TvJO yy\0"),
            ]), ""),
            ("Mach-O fat __cstring arm64", fat_macho([
                (0x01000007, macho64([("__text", b"neutral\0")],
                                    cputype=0x01000007)),
                (0x0100000C, macho64([
                    ("__text", b"\0code TvJO bytes\0"),
                    ("__cstring", b"xx TvJO yy\0"),
                ])),
            ]), ""),
            *((f"ELF {section}", elf64([
                (".text", b"\0code TvJO bytes\0", 0x6),
                (section, b"xx TvJO yy\0", flags),
            ]), "") for section, flags in (
                (".rodata", 0x2), (".rodata.str1.1", 0x32),
                (".rodata.cst16", 0x12), (".data.rel.ro", 0x3),
                (".tdata", 0x403), (".gnu.linkonce.r.probe", 0x2),
            )),
            ("Mach-O symbol", macho64([
                ("__text", b"\0code TvJO bytes\0"),
            ]), "0000000000000000 T _TvJO\n"),
            ("ELF symbol", elf64([
                (".text", b"\0code TvJO bytes\0", 0x6),
            ]), "0000000000000000 T TvJO\n"),
        )
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-data-") as directory:
            root = Path(directory)
            for label, object_bytes, symbols in fixtures:
                with self.subTest(label=label):
                    strings, nm = self.read_fixture(
                        object_bytes, root / label, symbols=symbols)
                    if symbols:
                        findings, _ = guard.evaluate(strings, nm, RULED)
                        self.assertTrue(any(f.token == "TvJO" and "via nm" in f.detail
                                            for f in findings))
                    else:
                        self.assertIn("xx TvJO yy", strings)
                    self.assertIn("TvJO", self.unruled_tokens(strings, nm))
                    # This paired negative control makes this row red on the
                    # old checker too; retaining data alone already passed it.
                    self.assertNotIn("code TvJO bytes", strings)

    def test_duplicate_member_names_are_infrastructure_errors(self) -> None:
        """Both APIs must reject duplicates, not read the first object twice."""
        neutral = elf64([(".rodata", b"neutral\0", 2)])
        forbidden = elf64([(".rodata", b"xx TvJO yy\0", 2)])
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-duplicates-") as directory:
            root = Path(directory)
            for name in ("probe.o", "other.o"):
                data = ar_bytes([(name, neutral), (name, forbidden)])
                archive = root / (name + ".a")
                archive.write_bytes(data)
                message = "duplicate archive member.*" + re.escape(name)
                with self.subTest(name=name, entry="archive_members"):
                    with self.assertRaisesRegex(guard.InfrastructureError, message):
                        guard.archive_members(archive)
                with self.subTest(name=name, entry="read_archive"):
                    with self.assertRaisesRegex(guard.InfrastructureError, message):
                        self.read_archive_fixture(data, root / name)

    def test_stripped_duplicate_member_names_are_infrastructure_errors(self) -> None:
        """Validate the stripped member list as well as the original list."""
        neutral = elf64([(".rodata", b"neutral\0", 2)])
        original = ar_bytes([("probe.o", neutral)])
        stripped = ar_bytes([("shadow.o", neutral), ("shadow.o", neutral)])
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-strip-duplicates-") as directory:
            with self.assertRaisesRegex(guard.InfrastructureError,
                                        r"duplicate archive member.*shadow\.o"):
                self.read_archive_fixture(
                    original, Path(directory) / "read",
                    stripped_data=stripped)

    def test_unknown_non_executable_sections_are_flagged(self) -> None:
        """Unknown data, including misleading text/metadata names, is visible."""
        fixtures = [
            (f"Mach-O {name}", macho_object([
                ("__opcodes", b"\0code TvJO bytes\0", 0x80000400, "__TEXT"),
                (name, b"xx TvJO yy\0", 0, "__DATA"),
            ]))
            for name in ("__vendor_blob", "__foreign_data", "__text")
        ] + [
            (f"ELF {name}", elf_object([
                (".opcodes", b"\0code TvJO bytes\0", 0x6, 1),
                (name, b"xx TvJO yy\0", 0x2, 1),
            ]))
            for name in (".vendor.blob", ".unfamiliar", ".text.unflagged",
                         ".debuggerdata", ".symtab")
        ]
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-unknown-") as directory:
            for index, (label, data) in enumerate(fixtures):
                with self.subTest(label=label):
                    strings, nm = self.read_fixture(data, Path(directory) / str(index))
                    self.assertIn("xx TvJO yy", strings)
                    self.assertIn("TvJO", self.unruled_tokens(strings, nm))
                    self.assertNotIn("code TvJO bytes", strings)

    def test_metadata_deny_lists_are_ignored(self) -> None:
        """Known metadata stays out; nm remains the separate symbol surface."""
        code = b"\0code TvJO bytes\0"
        forbidden = b"xx TvJO yy\0"
        neutral = b"kept neutral data\0"
        elf_metadata = (
            (".symtab", 2), (".strtab", 3), (".rela.data", 4),
            (".rel.data", 9), (".dynsym", 11), (".symtab_shndx", 18),
            (".relr.dyn", 19),
        )
        metadata_rows = list(elf_metadata)
        # Types must work even with a data-like name.
        metadata_rows.extend((".rodata", kind) for _, kind in elf_metadata)
        metadata_rows.extend((name, 1) for name in (
            ".debug", ".zdebug", ".debug_info", ".debug_info.dwo",
            ".zdebug_str", ".gdb_index", ".gnu_debuglink", ".gnu_debugaltlink",
        ))
        fixtures = [
            (f"ELF {name} type {kind}", elf_object([
                (".text", code, 6, 1), (".data", neutral, 3, 1),
                (name, forbidden, 0, kind),
            ]))
            for name, kind in metadata_rows
        ] + [
            (label, macho_object([
                ("__text", code, 0x80000400, "__TEXT"),
                ("__data", neutral, 0, "__DATA"),
                (name, forbidden, flags, segment),
            ]))
            for label, name, flags, segment in (
                ("Mach-O debug segment", "__const", 0, "__DWARF"),
                ("Mach-O linker segment", "__const", 0, "__LINKEDIT"),
                ("Mach-O debug attribute", "__cstring", 0x02000002, "__DATA"),
            )
        ]
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-metadata-") as directory:
            for index, (label, data) in enumerate(fixtures):
                with self.subTest(label=label):
                    strings, nm = self.read_fixture(data, Path(directory) / str(index))
                    self.assertIn("kept neutral data", strings)
                    self.assertNotIn("xx TvJO yy", strings)
                    self.assertNotIn("code TvJO bytes", strings)
                    self.assertNotIn("TvJO", self.unruled_tokens(strings, nm))

    def test_executable_and_fileless_sections_are_ignored(self) -> None:
        """Flags and no-file-byte types apply independently of section names."""
        fixtures = [
            (f"Mach-O instruction flag {flags:#x}", macho_object([
                ("__data", b"kept neutral data\0", 0, "__DATA"),
                ("__unknown_code", b"\0code TvJO bytes\0", flags, "__TEXT"),
            ]))
            for flags in (0x80000000, 0x400)
        ] + [
            (f"Mach-O zerofill {kind:#x}", macho_object([
                ("__text", b"\0code TvJO bytes\0", 0x80000400, "__TEXT"),
                ("__data", b"kept neutral data\0", 0, "__DATA"),
                ("__const", b"xx TvJO yy\0", kind, "__DATA"),
                ("__empty", b"", 0, "__DATA"),
            ]))
            for kind in (1, 0xC, 0x12)
        ] + [
            ("ELF executable and NOBITS", elf_object([
                (".unknown_code", b"\0code TvJO bytes\0", 6, 1),
                (".data", b"kept neutral data\0", 3, 1),
                (".rodata", b"xx TvJO yy\0", 2, 8),
                (".empty", b"", 2, 1),
            ])),
        ]
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-fileless-") as directory:
            for index, (label, data) in enumerate(fixtures):
                with self.subTest(label=label):
                    strings, nm = self.read_fixture(data, Path(directory) / str(index))
                    self.assertIn("kept neutral data", strings)
                    self.assertNotIn("TvJO", "\n".join(strings))
                    self.assertNotIn("TvJO", self.unruled_tokens(strings, nm))

    def test_object_layout_variants_keep_data_and_skip_code(self) -> None:
        """Byte fixtures cover both widths/orders and both fat record layouts."""
        fixtures: list[tuple[str, bytes]] = []
        for is_64 in (False, True):
            for endian in ("<", ">"):
                label = f"{'64' if is_64 else '32'} {endian}"
                fixtures.extend((
                    ("Mach-O " + label, macho_object([
                        ("__text", b"\0code TvJO bytes\0", 0x80000400, "__TEXT"),
                        ("__cstring", b"xx TvJO yy\0", 2, "__TEXT"),
                    ], is_64=is_64, endian=endian)),
                    ("ELF " + label, elf_object([
                        (".text", b"\0code TvJO bytes\0", 6, 1),
                        (".rodata", b"xx TvJO yy\0", 2, 1),
                    ], is_64=is_64, endian=endian)),
                    ("fat Mach-O " + label, fat_macho([
                        (0x01000007, macho64([("__cstring", b"neutral\0")],
                                            cputype=0x01000007)),
                        (0x0100000C, macho64([
                            ("__text", b"\0code TvJO bytes\0"),
                            ("__cstring", b"xx TvJO yy\0"),
                        ])),
                    ], is_64=is_64, endian=endian)),
                ))
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-layouts-") as directory:
            for index, (label, data) in enumerate(fixtures):
                with self.subTest(label=label):
                    strings, nm = self.read_fixture(data, Path(directory) / str(index))
                    self.assertIn("xx TvJO yy", strings)
                    self.assertIn("TvJO", self.unruled_tokens(strings, nm))
                    self.assertNotIn("code TvJO bytes", strings)

    def test_elf_extended_section_counts_and_name_indexes(self) -> None:
        """Exercise each escape field independently and together, at both widths."""
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-extended-") as directory:
            root = Path(directory)
            for is_64 in (False, True):
                for endian in ("<", ">"):
                    for counts, names in ((True, False), (False, True), (True, True)):
                        label = f"{is_64}-{endian}-{counts}-{names}"
                        with self.subTest(label=label):
                            data = elf_object([
                                (".text", b"\0code TvJO bytes\0", 6, 1),
                                (".rodata", b"xx TvJO yy\0", 2, 1),
                            ], is_64=is_64, endian=endian,
                                extended_count=counts, extended_names=names)
                            strings, nm = self.read_fixture(data, root / label)
                            self.assertEqual(strings, ["xx TvJO yy"])
                            self.assertIn("TvJO", self.unruled_tokens(strings, nm))

    def test_malformed_object_members_are_infrastructure_errors(self) -> None:
        """Require named errors for specific unsupported/truncated byte layouts."""
        def changed(data: bytes, offset: int, fmt: str, value: int) -> bytes:
            output = bytearray(data)
            struct.pack_into(fmt, output, offset, value)
            return bytes(output)

        elf = elf64([(".rodata", b"xx TvJO yy\0", 2)])
        shoff = struct.unpack_from("<Q", elf, 40)[0]
        shnum = struct.unpack_from("<H", elf, 60)[0]
        names_header = shoff + (shnum - 1) * 64
        names_offset, names_size = struct.unpack_from("<QQ", elf, names_header + 24)
        macho = macho64([("__cstring", b"xx TvJO yy\0")])
        macho32 = macho_object([("__cstring", b"xx TvJO yy\0", 2, "__TEXT")],
                               is_64=False)
        fat = fat_macho([(0x0100000C, macho)])
        fat64 = fat_macho([(0x0100000C, macho)], is_64=True)
        fat_offset = struct.unpack_from(">I", fat, 16)[0]
        fixtures = (
            ("unknown magic", b"not-an-object", "unsupported object member"),
            ("ELF ident", b"\x7fELF", "not an ELF object"),
            ("ELF class", changed(elf, 4, "B", 3), "unsupported ELF class"),
            ("ELF order", changed(elf, 5, "B", 3), "unsupported ELF byte order"),
            ("ELF header", elf[:16], "truncated ELF header"),
            ("ELF missing table", changed(elf, 40, "<Q", 0), "section table is missing"),
            ("ELF short entries", changed(elf, 58, "<H", 1), "section table is missing"),
            ("ELF section zero", changed(elf, 40, "<Q", len(elf)), "section zero is outside"),
            ("ELF zero count", changed(elf, 60, "<H", 0), "section table has no names"),
            ("ELF name index", changed(elf, 62, "<H", shnum), "section table has no names"),
            ("ELF table bounds", changed(elf, 60, "<H", 0xFFFE), "section table is outside"),
            ("ELF names bounds", changed(elf, names_header + 24, "<Q", len(elf)),
             "section names is outside"),
            ("ELF name offset", changed(elf, shoff + 64, "<I", 0xFFFFFFFF),
             "section name is outside its table"),
            ("ELF unterminated name", changed(elf, names_offset + names_size - 1, "B", 88),
             "unterminated ELF section name"),
            ("ELF payload bounds", changed(elf, shoff + 64 + 24, "<Q", len(elf)),
             r"\.rodata is outside"),
            ("Mach-O header", macho[:4], "truncated Mach-O header"),
            ("Mach-O command bounds", changed(macho, 20, "<I", len(macho)),
             "load commands is outside"),
            ("Mach-O command count", changed(macho, 16, "<I", 2),
             "load command 1 is truncated"),
            ("Mach-O short command", changed(macho, 36, "<I", 4),
             "invalid Mach-O load command 0"),
            ("Mach-O long command", changed(macho, 36, "<I", len(macho)),
             "invalid Mach-O load command 0"),
            ("Mach-O 64 segment in 32", changed(macho32, 28, "<I", 0x19),
             "64-bit segment in 32-bit Mach-O"),
            ("Mach-O 32 segment in 64", changed(macho, 32, "<I", 0x1),
             "32-bit segment in 64-bit Mach-O"),
            ("Mach-O short segment", changed(macho, 36, "<I", 8),
             "truncated Mach-O segment"),
            ("Mach-O section count", changed(macho, 96, "<I", 2),
             "truncated Mach-O section table"),
            ("Mach-O payload bounds", changed(macho, 152, "<I", len(macho)),
             "__cstring is outside"),
            ("fat header", fat[:4], "truncated fat Mach-O header"),
            ("fat zero slices", changed(fat, 4, ">I", 0), "no architecture slices"),
            ("fat table bounds", fat[:8], "architecture table is outside"),
            ("fat slice bounds", changed(fat, 16, ">I", len(fat)), "architecture 0 is outside"),
            ("fat64 slice bounds", changed(fat64, 16, ">Q", len(fat64)),
             "architecture 0 is outside"),
            ("fat invalid slice", changed(fat, fat_offset, ">I", 0),
             "not a Mach-O object"),
        )
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-malformed-") as directory:
            for index, (label, data, message) in enumerate(fixtures):
                with self.subTest(label=label):
                    with self.assertRaisesRegex(guard.InfrastructureError, message) as raised:
                        self.read_fixture(data, Path(directory) / str(index), code_control=False)
                    self.assertIn("probe.o", str(raised.exception))


class DecodedLiteralCorroborationTests(unittest.TestCase):
    """Real archive + source probes, using check()'s unchanged public entry."""

    def check_literal(self, spelling: str, token: str, *, visible: bool = False) -> None:
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-decoded-") as directory:
            root = Path(directory)
            source_dir = root / "src"
            source_dir.mkdir()
            (source_dir / "empty.hpp").write_text("")
            # Keep all non-header ADR rows live; installed headers supply the
            # header-only rows, just as they do in the built-archive test.
            table = sorted((RULED.identifiers | RULED.phrases)
                           - RULED.header_only - RULED.debug_only)
            table_text = ",\n".join(json.dumps(text) for text in table)
            text = ('extern "C" const char* table_texts[] = {\n' + table_text + '\n};\n'
                    'extern "C" const char probe_text[] = "\\0" ' + spelling + ' "\\0";\n'
                    'extern "C" const char* probe_value() { return probe_text + 1; }\n')
            archive = build_probe_archive(source_dir, text)
            (root / "CMakeLists.txt").write_text(
                'set(PINEFORGE_KERNEL_SOURCES src/residual_probe.cpp)\n'
                'install(DIRECTORY include/pineforge DESTINATION include\n'
                '        PATTERN "source" EXCLUDE PATTERN "compat" EXCLUDE)\n')
            source = source_dir / "residual_probe.cpp"
            literals = guard.read_source_literals(root, [source])
            old_source_hits = guard.scan(literals[0], "source-literal")
            self.assertEqual(token in {h.token for h in old_source_hits}, visible)
            strings, nm, _ = guard.read_archive(archive, root)
            self.assertIn(token, [line.strip() for line in strings])
            self.assertNotIn(token, {guard.symbol_name(h.token, h.source)
                                     for h in guard.scan(nm, "nm")})
            evidence = root / "evidence"
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = guard.check(archive, source_root=root, evidence_dir=evidence)
            result = json.loads((evidence / "summary.json").read_text())
            self.assertEqual(code, 1, (out.getvalue(), err.getvalue()))
            self.assertEqual([(f["kind"], f["token"]) for f in result["findings"]],
                             [("unruled", token)])
            self.assertIn("via strings", result["findings"][0]["detail"])
            self.assertEqual(result["summary"]["sourceHits"], len(old_source_hits))
            self.assertIn("1 findings ... FAIL", out.getvalue())

    def test_an_ordinary_source_literal_corroborates_a_bare_string(self) -> None:
        for token in ("TvJO", "tv_foo"):
            with self.subTest(token=token):
                self.check_literal(json.dumps(token), token, visible=True)

    def test_escaped_literals_corroborate_their_decoded_identifiers(self) -> None:
        for spelling, token in (
                (r'"\x54\x76\x4a\x4f"', "TvJO"),
                (r'"\x00000054\x76\x4a\x4f"', "TvJO"),
                (r'"\x54vJO"', "TvJO"),
                (r'"\x74\x76\x5f\x66\x6f\x6f"', "tv_foo"),
                (r'"\124\166\112\117"', "TvJO"),
                (r'"\u0074\u0076\u005f\u0066\u006f\u006f"', "tv_foo"),
                (r'"\U00000054\U00000076\U0000004a\U0000004f"', "TvJO"),
                (r'"\t\x54\x76\x4a\x4f\n"', "TvJO")):
            with self.subTest(spelling=spelling):
                self.check_literal(spelling, token)

    def test_adjacent_literals_corroborate_their_joined_identifiers(self) -> None:
        for spelling, token in (
                ('"T" /* between tokens */ "vJO"', "TvJO"),
                ('"t"\n"v_foo"', "tv_foo"),
                ('u8"T" u8"vJO"', "TvJO"),
                ('R"first(T)first" R"second(vJO)second"', "TvJO"),
                ('"T" \\\n"vJO"', "TvJO"),
                ('\n#define UNUSED "prefix"\n"T" "vJO"', "TvJO"),
                ('\n#include "empty.hpp"\n"T" "vJO"', "TvJO")):
            with self.subTest(spelling=spelling):
                self.check_literal(spelling, token)


class AuditBlindSpotTests(unittest.TestCase):
    """The probes the third audit's two reviewers passed through the gate."""

    def evaluate(self, strings: list[str], nm: list[str], **surfaces):
        return evaluate_table(strings, nm, **surfaces)

    def unruled(self, strings: list[str], nm: list[str], **surfaces) -> set[str]:
        findings, _ = self.evaluate(strings, nm, **surfaces)
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
                self.assertIn(token, self.unruled(
                    strings + [line], nm,
                    source_literals=([line], ["src/blind_probe.cpp:1"])))
        strings, nm = table_archive()
        findings, _ = self.evaluate(strings + ["session.ismarket requires a session"], nm)
        self.assertEqual([f.kind for f in findings], ["unruled"])

    def test_pine_parameter_words_are_residue(self) -> None:
        for word in ("over_pyramiding_probe", "default_qty_type", "calc_on_every_tick",
                     "use_bar_magnifier", "backtest_fill_limits_assumption",
                     "close_entries_rule", "fill_orders_on_standard_ohlc", "max_bars_back_probe",
                     "calc_bars_count", "dynamic_requests", "syminfo_probe",
                     "strategy_position_entry_name", "strategy_closedtrades_probe",
                     "ta_highestbars_probe", "barstate_isconfirmed_probe"):
            with self.subTest(word=word):
                strings, nm = table_archive()
                self.assertEqual(self.unruled(
                    strings + [word], nm,
                    source_literals=([word], ["src/blind_probe.cpp:1"])), {word})

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
        # (expectation corrected: the text alone -> the text and its bare
        # `islastbar`, which R5 lane H-DOCGATES' vocabulary reads by itself.)
        self.assertEqual(self.unruled(strings + metadata + ["session.islastbar probe"], nm),
                         {"session.islastbar probe", "islastbar"})
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
        literals = guard.read_source_literals(workdir, [workdir / "residual_probe.cpp"])
        findings, _ = guard.evaluate(strings_lines, nm_lines, RULED, source_literals=literals)
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


# The x86-64 GCC 13.3 archive's own `strings -a` line for the kernel's
# liquidation label (lane INT16b, PR #278's kernel-only job): five 16-byte
# vector constants packed in .rodata.cst16, of which "__kernel_liquida" is one;
# the label's last bytes are the immediate of a `movabs`, so no line carries the
# whole `__kernel_liquidation__`.
GCC_X86_SPLIT_LINE = ("native-consumer/native-driver/v5native-calendar/"
                      "__kernel_liquidaMargin liquidati")
SPLIT_LABEL = "__kernel_liquidation__"


class KernelSourceSurfaceTests(unittest.TestCase):
    """Lane INT16b: the string literals of the kernel's own translation units
    are a third surface, so a literal the compiler materialized as constants
    and immediates is judged the same on every platform -- and a ruling that
    no surface carries is still stale."""

    def split_archive(self) -> tuple[list[str], list[str]]:
        strings, nm = table_archive()
        strings.remove(SPLIT_LABEL)
        return strings + [GCC_X86_SPLIT_LINE], nm

    def source_literals(self, text: str) -> tuple[list[str], list[str]]:
        with tempfile.TemporaryDirectory(prefix="pineforge-residual-source-") as directory:
            root = Path(directory)
            unit = root / "src" / "probe.cpp"
            unit.parent.mkdir()
            unit.write_text(text)
            return guard.read_source_literals(root, [unit])

    def test_without_the_source_the_split_label_reads_stale(self) -> None:
        """Fail-before: PR #278's Linux verdict, reproduced from its strings line."""
        findings, _ = evaluate_table(*self.split_archive())
        self.assertEqual([(f.kind, f.token) for f in findings], [("stale", SPLIT_LABEL)])

    def test_a_label_the_compiler_split_is_live_through_its_source_literal(self) -> None:
        literals = self.source_literals(
            'constexpr char kNativeLiquidationLabel[] = "__kernel_liquidation__";\n')
        findings, summary = evaluate_table(*self.split_archive(), source_literals=literals)
        self.assertEqual(findings, [], [str(f) for f in findings])
        self.assertEqual(summary["sourceHits"], 1)

    def test_a_ruling_no_surface_carries_is_still_stale(self) -> None:
        literals = self.source_literals(
            'constexpr char kNativeRiskLabel[] = "__kernel_risk__";\n'
            'constexpr char kComment[] = "Margin liquidation";\n')
        findings, _ = evaluate_table(*self.split_archive(), source_literals=literals)
        self.assertEqual([(f.kind, f.token) for f in findings], [("stale", SPLIT_LABEL)])

    def test_a_comment_or_an_include_path_is_not_a_source_literal(self) -> None:
        literals = self.source_literals(
            '#include "__kernel_liquidation__.hpp"\n'
            '// constexpr char kLabel[] = "__kernel_liquidation__";\n'
            '/* "__kernel_liquidation__" */\n'
            'constexpr char kOther[] = "Margin liquidation";\n')
        self.assertEqual(literals[0], ["Margin liquidation"])
        findings, _ = evaluate_table(*self.split_archive(), source_literals=literals)
        self.assertEqual([(f.kind, f.token) for f in findings], [("stale", SPLIT_LABEL)])

    def test_a_kernel_source_literal_is_judged_like_an_archive_text(self) -> None:
        """The surface is read in both directions: a vocabulary literal a kernel
        TU compiles needs a row even where no `strings` line shows it."""
        literals = self.source_literals('const char* reason = "strategy.entry rejected";\n'
                                        'const char* tag = "tv_probe_source";\n')
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings, nm, source_literals=literals)
        self.assertEqual(sorted((f.kind, f.token) for f in findings),
                         [("unruled", "strategy.entry rejected"),
                          ("unruled", "tv_probe_source")])

    def test_the_source_list_is_the_kernel_list_the_archive_was_built_from(self) -> None:
        cmake = guard.CMAKE_LISTS.read_text(encoding="utf-8")
        units = guard.kernel_translation_units(cmake)
        self.assertIn("src/native_execution_consumer.cpp", units)
        self.assertIn("src/reservation_expansion.cpp", units)
        self.assertNotIn("src/compat/pine/reservation_expansion.cpp", units)
        members = [Path(unit).name + ".o" for unit in units]
        files = [guard.repository_path(path, guard.ROOT)
                 for path in guard.kernel_source_files(guard.ROOT, cmake, members)]
        self.assertIn("src/native_execution_consumer.cpp", files)
        self.assertIn("src/native_execution_consumer.hpp", files)   # reached by its include
        self.assertFalse([f for f in files if f.startswith(("src/source/", "src/compat/"))],
                         files)
        for label, wrong in (("a member missing", members[1:]),
                             ("an extra member", members + ["pine_adapter.cpp.o"])):
            with self.subTest(label):
                with self.assertRaises(guard.InfrastructureError):
                    guard.kernel_source_files(guard.ROOT, cmake, wrong)

    def test_the_kernel_sources_carry_no_unruled_literal(self) -> None:
        cmake = guard.CMAKE_LISTS.read_text(encoding="utf-8")
        units = guard.kernel_translation_units(cmake)
        files = guard.kernel_source_files(guard.ROOT, cmake,
                                          [Path(unit).name + ".o" for unit in units])
        literals = guard.read_source_literals(guard.ROOT, files)
        strings, nm = table_archive()
        findings, summary = evaluate_table(strings, nm, source_literals=literals)
        self.assertEqual(findings, [], [str(f) for f in findings])
        self.assertGreater(summary["sourceHits"], 0)


class ReachTests(unittest.TestCase):
    """R5 lane H-DOCGATES (AUDIT4-opus X12, kres K4/K5/K11): the fourth
    audit's probes, each of which the lane's base checker (21bdc2e8) passed
    unflagged, and the two halves of the header-only and count rules."""

    def unruled(self, strings: list[str], nm: list[str], **surfaces) -> set[str]:
        findings, _ = evaluate_table(strings, nm, **surfaces)
        return {f.token for f in findings if f.kind == "unruled"}

    def test_a_ruled_name_never_covers_a_text(self) -> None:
        """kres gate_bareword_probe.txt: the bare-word rulings `Pine`
        (ADR-0001's PineMap row) and `syminfo` (the symbol-information row)
        passed these four texts, and any ruled name passed a text around it."""
        for text in ("match Pine semantics exactly", "Pine requires the chart symbol here",
                     "syminfo.tickerid must be set", "syminfo.mintick drives the tick",
                     "strategy_set_syminfo_type rejects this value"):
            for surface in ("strings", "source-literal"):
                with self.subTest(text=text, surface=surface):
                    strings, nm = table_archive()
                    if surface == "strings":
                        found = self.unruled(strings + [text], nm)
                    else:
                        found = self.unruled(strings, nm,
                                             source_literals=([text], ["src/probe.cpp:1"]))
                    self.assertIn(text, found)
        self.assertNotIn("Pine", RULED.identifiers)
        # The name alone -- a symbol, a code identifier -- stays ruled by name.
        strings, nm = table_archive()
        self.assertEqual(self.unruled(strings + ["strategy_set_syminfo_type"],
                                      nm + ["T _strategy_set_syminfo_type"]), set())

    def test_pine_dotted_namespaces_are_residue(self) -> None:
        for text in ("timeframe.change", "input.int", "str.format", "math.sum",
                     "matrix.new: negative size", "line.get_price requires xloc.bar_index",
                     "syminfo.tickerid", "format.mintick", "currency.USD", "dayofweek.monday",
                     "timeframe.isintraday", "array.get index out of range",
                     "ticker.heikinashi"):
            with self.subTest(text=text):
                strings, nm = table_archive()
                self.assertIn(text, self.unruled(strings + [text], nm))
                self.assertTrue(self.unruled(strings, nm + ["0000000000000000 T " + text]))
        # A compiler's switch-table label, a project header, a member of a
        # dotted run that is not a namespace: none is Pine's.
        for text in ("l_switch.table.strategy_native_cancel_v1", "include/pineforge/map.hpp",
                     "color.hpp", "matrix.hpp", "x.str.y", "data.exchange", "inline.cold"):
            with self.subTest(look_alike=text):
                strings, nm = table_archive()
                findings, _ = evaluate_table(strings + [text], nm + ["t " + text])
                self.assertEqual(findings, [], [str(f) for f in findings])

    def test_camel_case_platform_names_and_bare_flags_are_residue(self) -> None:
        for name in ("TvRound", "TVSession", "kTvStep", "islastbar", "isfirstbar",
                     "isfirstbar_regular", "heikinashi_bar", "renko_brick", "time_tradingday"):
            with self.subTest(name=name):
                strings, nm = table_archive()
                self.assertEqual(self.unruled(
                    strings + [name], nm,
                    source_literals=([name], ["src/probe.cpp:1"])), {name})
                self.assertEqual(self.unruled(strings, nm + ["T " + name]), {name})
        # Machine-code runs that only resemble them are not names.
        strings, nm = table_archive()
        findings, _ = evaluate_table(strings + ["TvV@", "TVh", 'C0"TV"', "xTVa", "ATVdisp"], nm)
        self.assertEqual(findings, [], [str(f) for f in findings])

    def test_a_compiler_split_symbol_is_still_its_name(self) -> None:
        """x86-64 GCC 13's kernel archive prints `strategy_set_syminfo_type.cold`
        (the cold half GCC splits off a function) in its string table: one
        symbol's name under a clone suffix, ruled with the export, not a text."""
        strings, nm = table_archive()
        clones = ["strategy_set_syminfo_type.cold", "_strategy_set_syminfo_type.cold.1",
                  "strategy_set_syminfo_mintick.part.0", "strategy_position_size.isra.0",
                  "strategy_set_syminfo_string.constprop.3", "strategy_set_syminfo_session.llvm.42"]
        findings, _ = evaluate_table(strings + clones, nm + ["t " + c for c in clones])
        self.assertEqual(findings, [], [str(f) for f in findings])
        # the suffix is a clone marker only at the end of a symbol-shaped line
        self.assertEqual(self.unruled(strings + ["pine_probe_fn.cold", "timeframe.change"], nm),
                         {"pine_probe_fn", "timeframe.change"})
        self.assertIn("syminfo.part: bad", self.unruled(strings + ["syminfo.part: bad"], nm))

    def test_a_header_only_ruling_is_held_to_its_row(self) -> None:
        """A row saying "no archive symbol" rules its names and texts for the
        installed headers alone: the same name in the archive or a kernel
        literal is a finding (PineMap, round 3's blind probe, now flags there)."""
        self.assertIn("PineMap", RULED.header_only)
        self.assertIn("map.size: result exceeds int range", RULED.header_only)
        self.assertNotIn("tv_carry_qty", RULED.header_only)
        for surface in ("strings", "nm", "source-literal"):
            with self.subTest(surface=surface):
                strings, nm = table_archive()
                if surface == "strings":
                    found = self.unruled(strings + ["PineMap"], nm)
                elif surface == "nm":
                    found = self.unruled(strings, nm + ["T pineforge::PineMap<int, int>::size()"])
                else:
                    found = self.unruled(strings, nm,
                                         source_literals=(["PineMap"], ["src/probe.cpp:1"]))
                self.assertIn("PineMap", found)
        strings, nm = table_archive()
        self.assertIn("map.size: result exceeds int range",
                      self.unruled(strings + ["map.size: result exceeds int range"], nm))
        # Where the row puts them, they pass.
        findings, _ = evaluate_table(*table_archive())
        self.assertEqual(findings, [], [str(f) for f in findings])

    def test_the_printed_counts_are_the_rulings_the_vocabulary_reads(self) -> None:
        """kres K5: the mechanism tables' first columns were counted as rulings
        (27 of "202" identifiers, 22 of "25" texts at 91d65ad6)."""
        self.assertIn("priority", RULED.identifiers)
        self.assertFalse(guard.vocabulary_name("priority"))
        _, summary = evaluate_table(*table_archive())
        self.assertEqual(summary["ruledIdentifiers"],
                         sum(1 for name in RULED.identifiers if guard.vocabulary_name(name)))
        self.assertLess(summary["ruledIdentifiers"], len(RULED.identifiers))
        self.assertEqual(summary["ruledPhrases"],
                         sum(1 for text in RULED.phrases if guard.vocabulary_text(text)))
        self.assertLess(summary["ruledPhrases"], len(RULED.phrases))

    def test_a_ruling_that_leaves_with_its_name_reads_below_the_floor(self) -> None:
        """docs-a N7: a row and its name removed together pass every other
        rule -- no unruled name, no stale row -- and only the floor sees it."""
        _, full = evaluate_table(*table_archive())
        row = next(line for line in ADR_TEXT.splitlines()
                   if line.startswith("| `tv_carry_qty` |"))
        reverted = guard.ruled_entries(ADR_TEXT.replace(row + "\n", ""))
        self.assertNotIn("tv_carry_qty", reverted.identifiers)
        strings, nm = table_archive()
        strings.remove("tv_carry_qty")
        findings, summary = guard.evaluate(strings, nm, reverted, header_lines=table_headers())
        self.assertEqual(findings, [], [str(f) for f in findings])
        below = guard.floor_findings(summary, full["ruledIdentifiers"], full["ruledPhrases"])
        self.assertEqual([(f.kind, f.token) for f in below], [("floor", "ruled identifiers")])
        self.assertEqual(guard.floor_findings(full, full["ruledIdentifiers"],
                                              full["ruledPhrases"]), [])


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
