#!/usr/bin/env python3
"""The kernel-only archive names no TradingView vocabulary outside ADR-0001's table.

R5 gap lane P2, profile-independent since gap lane P2b. ADR-0001 ("Residual
TradingView-named surface in the kernel-only archive") rules every Pine /
TradingView-shaped name the kernel archive still carries, and says a name that
is not in that table must not appear. Until lane P2 nothing ran the
measurement: the N14 probe was a documented grep, and it was structurally blind
to the pending-row names whose spelling holds no `pine`.

What the gate judges is what a consumer LINKS, and only that:

  * the SYMBOL TABLE -- `nm -C` over the archive, defined and undefined,
    demangled: every name the linker can resolve or demand;
  * the STRING LITERALS -- `strings -a` over a copy of the archive whose
    debug information has been stripped (see STRIP_TOOLS).

The strip is what makes the verdict a property of the code instead of a
property of the build type. A `-g` archive carries DWARF, and `strings -a` over
DWARF reads the debug info's own name tables: every block-scope local, every
struct member, every `struct timeval` field of every system header, every
source path. A Debug archive of this tree answered 831 hits to the Release
archive's 109 and named thirteen "residuals" that no consumer can link (lane P2
shipped without the strip and failed exactly there, on hosted
`ubuntu-24.04 Debug`). A local variable is not a residual surface; a symbol and
a literal are.

Against those two sources the checker matches a fixed residual vocabulary:

  * IDENTIFIER_PATTERNS -- a whole identifier (ARCHIVE_IDENTIFIER: at least
    MIN_IDENTIFIER_LENGTH characters, delimited by non-identifier bytes on both
    sides) that contains one of them must be listed in the first column of an
    ADR table, by its exact name. The whole-identifier rule is also what keeps
    binary bytes out of the verdict: `strings` happily prints a run like
    `C0"TV"` out of machine code, and a two-letter fragment is never a name.
  * PHRASE_PATTERNS -- a text (an error message, a label) that contains one of
    them must contain a phrase listed in that column;
  * and every listed name or phrase that the vocabulary matches must still be
    in the archive, so the table cannot go stale in either direction.

R5 lane F6 (AUDIT3-opus2 H11, AUDIT3-opus kres) widened both halves. The
vocabulary now also reads the Pine words that carry no `pine`/`tv` token: the
`strategy()` declaration parameters (`pyramiding`, `default_qty`,
`calc_on_every_tick`, ...), the namespaces with no generic reading
(`syminfo`, `barstate`), the underscore spellings of Pine calls whose
namespace word IS generic (`strategy_entry`, `ta_ema`, `request_security`,
`security_lower_tf`, `session_ismarket`, `gaps_on`, `lookahead_off` -- the
Pine member names are listed, so `strategy_native_*`, `ta_misc` or
`pf_native_lookahead_e` stay the kernel's own), `__name__` sentinel labels,
and the dotted `session.` / `barstate.` texts. A mangled symbol is judged by
the demangled name `nm -C` prints for it, and the platform's C-symbol
underscore (Mach-O `_name`, ELF `name`) is not a second name. And the gate
reads a second surface:

  * the INSTALLED HEADERS -- every header the kernel profile installs (the
    CMake install rule's own exclusions, read from CMakeLists.txt), comments
    stripped: code identifiers and string literals are what a consumer
    compiles against, a comment is not.

R5 INT16b adds a third surface, because whether a string literal's bytes stay
contiguous in the archive is the compiler's decision, not the code's. GCC 13
on x86-64 (the hosted ubuntu-24.04 runner) builds the std::string the kernel
books its own liquidation under from a 16-byte vector constant and an
overlapping 8-byte immediate -- `movdqa .LC168(%rip),%xmm0` ("__kernel_liquida",
packed into .rodata.cst16 beside other constants) and `movabs
$0x5f5f6e6f69746164,%rcx` ("dation__"), in
NativeExecutionConsumer::kernel_submit_liquidation -- because the constexpr
array's address is never taken; clang (macOS) and GCC on aarch64 keep the
array. `strings -a` finds no `__kernel_liquidation__` in that one archive, so a
gate that read only the archive called the ruling stale on Linux x86-64 and
live everywhere else. So the gate also reads

  * the KERNEL SOURCES -- the string literals of the translation units the
    archive was built from (CMakeLists.txt's `set(PINEFORGE_KERNEL_SOURCES
    ...)`, which must be exactly the archive's members) and of the src/
    headers they include, comments stripped and include paths skipped. They
    are judged in both directions, like the other two surfaces: a ruling
    stays live while any surface carries it, and a vocabulary literal a
    kernel TU compiles must have a row even where the compiler split it.

Exit 0 when every match is ruled and every ruling is live, 1 on a finding,
2 when the archive, the tools (including the strip tool: the gate fails CLOSED
rather than read debug information), the install rule or the ADR section
cannot be read.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import fnmatch
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ADR = ROOT / "docs" / "adr" / "0001-kernel-adapter-boundary.md"
CMAKE_LISTS = ROOT / "CMakeLists.txt"
INCLUDE_ROOT = ROOT / "include"
HEADER_SUFFIXES = (".h", ".hpp")
DEFAULT_ARCHIVE = ROOT / "build-ci-kernel" / "lib" / "libpineforge_kernel.a"
SECTION_HEADING = "## Residual TradingView-named surface in the kernel-only archive"
# The kernel's translation units as CMakeLists.txt lists them -- the one list
# both archives compile -- and the private headers they reach: a quoted
# include resolved beside the including file or under src/.
KERNEL_SOURCES = re.compile(r"set\(\s*PINEFORGE_KERNEL_SOURCES\s+([^)]*)\)")
INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\b")

# Pine v6 member names, for the underscore spellings of calls whose namespace
# word the kernel shares (see IDENTIFIER_PATTERNS). A closed vocabulary, like
# the rest: the language's own, not this tree's.
STRATEGY_MEMBERS = (
    "entry exit close close_all cancel cancel_all order convert_to_account "
    "convert_to_symbol default_entry_qty closedtrades opentrades risk account_currency "
    "avg_losing_trade avg_losing_trade_percent avg_trade avg_trade_percent "
    "avg_winning_trade avg_winning_trade_percent equity eventrades grossloss "
    "grossloss_percent grossprofit grossprofit_percent initial_capital losstrades "
    "margin_liquidation_price max_contracts_held_all max_contracts_held_long "
    "max_contracts_held_short max_drawdown max_drawdown_percent max_runup "
    "max_runup_percent netprofit netprofit_percent openprofit openprofit_percent "
    "position_avg_price position_entry_name position_size wintrades cash fixed "
    "percent_of_equity long short commission direction oca").split()
TA_FUNCTIONS = (
    "accdist alma atr barssince bb bbw cci change cmo cog correlation cross crossover "
    "crossunder cum dev dmi ema falling highest highestbars hma iii kc kcw linreg "
    "lowest lowestbars macd max median mfi min mode mom nvi obv "
    "percentile_linear_interpolation percentile_nearest_rank percentrank "
    "pivot_point_levels pivothigh pivotlow pvi pvt range rci rising rma roc rsi sar "
    "sma stdev stoch supertrend swma tr tsi valuewhen variance vwap vwma wad wma wpr "
    "wvad").split()
REQUEST_FUNCTIONS = (
    "security security_lower_tf financial quandl dividends earnings splits economic "
    "seed currency_rate footprint").split()
SESSION_MEMBERS = (
    "ismarket ispremarket ispostmarket isfirstbar islastbar isfirstbar_regular "
    "islastbar_regular regular extended").split()
BARSTATE_MEMBERS = (
    "isfirst islast ishistory isrealtime isnew isconfirmed islastconfirmedhistory").split()


def _any_of(words: list[str]) -> str:
    """One alternation, longest first so `close_all` wins over `close`."""
    return "(?:" + "|".join(sorted(words, key=len, reverse=True)) + ")"


# The residual vocabulary. Identifier patterns are matched against every whole
# identifier of every `strings` line and every `nm -C` line; phrase patterns
# against the raw line. `pine` excludes the project's own name.
IDENTIFIER_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("pine", re.compile(r"pine(?!forge)", re.IGNORECASE)),
    ("tradingview", re.compile(r"tradingview", re.IGNORECASE)),
    ("barmerge", re.compile(r"barmerge", re.IGNORECASE)),
    ("coof", re.compile(r"coof", re.IGNORECASE)),
    ("pooc", re.compile(r"pooc", re.IGNORECASE)),
    ("market_admission", re.compile(r"market_admission", re.IGNORECASE)),
    ("calc_on_order_fills", re.compile(r"calc_on_order_fills", re.IGNORECASE)),
    ("process_orders_on_close", re.compile(r"process_orders_on_close", re.IGNORECASE)),
    ("tv", re.compile(r"(?:^|_)tv(?:_|$)", re.IGNORECASE)),
    # R5 lane F6: Pine words with no `pine`/`tv` token. The strategy()
    # declaration parameters whose spelling has no generic reading ...
    ("pyramiding", re.compile(r"pyramiding", re.IGNORECASE)),
    ("default_qty", re.compile(r"default_qty", re.IGNORECASE)),
    ("calc_on_every_tick", re.compile(r"calc_on_every_tick", re.IGNORECASE)),
    ("strategy() parameter", re.compile(
        r"backtest_fill_limits_assumption|close_entries_rule|fill_orders_on_standard_ohlc"
        r"|use_bar_magnifier|max_bars_back|calc_bars_count|dynamic_requests",
        re.IGNORECASE)),
    # ... the Pine namespaces with no generic reading ...
    ("syminfo", re.compile(r"syminfo", re.IGNORECASE)),
    ("barstate", re.compile(r"barstate", re.IGNORECASE)),
    # ... and the underscore spelling of a Pine call whose namespace word is
    # also the kernel's own (a strategy, a TA library, a request, a session):
    # only the Pine member names count, so strategy_native_* (the C ABI),
    # ta_misc (a translation unit) or request_is_buy stay generic.
    ("strategy_<member>", re.compile(
        r"(?:^|_)strategy_" + _any_of(STRATEGY_MEMBERS) + r"(?:_|$)", re.IGNORECASE)),
    ("ta_<function>", re.compile(
        r"(?:^|_)ta_" + _any_of(TA_FUNCTIONS) + r"(?:_|$)", re.IGNORECASE)),
    ("request_<function>", re.compile(
        r"(?:^|_)(?:request_" + _any_of(REQUEST_FUNCTIONS) + r"|security_lower_tf)(?:_|$)",
        re.IGNORECASE)),
    ("session_<member>", re.compile(
        r"(?:^|_)session_" + _any_of(SESSION_MEMBERS) + r"(?:_|$)", re.IGNORECASE)),
    ("barmerge constant", re.compile(r"(?:^|_)(?:gaps|lookahead)_o(?:n|ff)(?:_|$)",
                                     re.IGNORECASE)),
)
# A label spelled `__name__` -- the adapter's `__close__` / `__margin_call__`
# family -- is residue wherever the archive holds it as a text. Read in texts
# only: in header CODE the same shape is the compiler's own vocabulary
# (`__attribute__`, `__clang__`).
SENTINEL = ("__sentinel__", re.compile(r"^__[a-z][a-z0-9_]*[a-z0-9]__$"))
# The languages' own identifiers of that shape are not labels: C99/C++11's
# predefined `__func__` (a sanitizer build names every function's `__func__`
# array in its global descriptors) and the GNU alternate keyword spellings.
LANGUAGE_DUNDERS = frozenset((
    "__func__ __attribute__ __asm__ __inline__ __restrict__ __typeof__ __volatile__ "
    "__extension__ __alignof__ __const__ __signed__ __label__ __real__ __imag__ "
    "__complex__").split())
# AddressSanitizer's stack-frame description, one rodata string per
# instrumented frame: a local count, then offset / size / name-length / name
# for each local ("3 32 8 6 s.addr 64 8 12 session.addr 96 16 11 ref.tmp:701").
# It names block-scope locals, exactly what the debug information does, and a
# local is not a residual surface (lane P2b), so the line is not read.
SANITIZER_FRAME = re.compile(r"^\d+(?: \d+ \d+ \d+ \S+)+$")
# An Itanium-mangled symbol as the symbol table spells it (ELF `_Z...`,
# Mach-O `__Z...`). `nm -C` prints the same symbol demangled, and that line
# is the one judged: one symbol is one finding, under its own name.
MANGLED = re.compile(r"^_{1,2}Z[0-9A-Z]")
# A dotted Pine namespace call is a text; the project's own source file of the
# same stem is not. `include/pineforge/ta.hpp` is a header, and an ASan build
# writes every such path into rodata as a real string literal (the global
# descriptors), so the strip alone does not settle it -- the suffix does. This
# is the same carve-out `pine(?!forge)` makes for the project's own name, and
# it hides no Pine built-in: none is spelled `h`, `hpp`, `cpp`, ...
NOT_A_SOURCE_FILE = r"(?!(?:h|hpp|hh|hxx|c|cc|cpp|cxx|inc|ipp)(?![A-Za-z0-9_]))"
PHRASE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("strategy.", re.compile(r"\bstrategy\." + NOT_A_SOURCE_FILE + r"[a-z_]{2,}")),
    ("ta.", re.compile(r"\bta\." + NOT_A_SOURCE_FILE + r"[a-z_]{2,}")),
    ("request.security", re.compile(r"request\.security")),
    ("barmerge.", re.compile(r"\bbarmerge\." + NOT_A_SOURCE_FILE + r"[a-z_]{2,}")),
    ("__margin_call__", re.compile(r"__margin_call__")),
    # R5 lane F6: the two dotted namespaces lane F6 reads name Pine members
    # only -- `session` is also the kernel's own word, and a text such as
    # "session.start" is not Pine's.
    ("session.", re.compile(r"\bsession\." + _any_of(SESSION_MEMBERS) + r"\b")),
    ("barstate.", re.compile(r"\bbarstate\." + _any_of(BARSTATE_MEMBERS) + r"\b")),
)
# A C/C++ name as the ADR spells it, used to read the ruling tables.
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# A name as it appears in the archive: a maximal identifier run, delimited by a
# non-identifier byte on both sides, at least MIN_IDENTIFIER_LENGTH long. The
# left-hand lookbehind is what makes it maximal (`0abc` is not the name `abc`),
# and the length floor is what makes machine-code bytes unreadable as names:
# the shortest vocabulary hit needs three characters anyway (`_tv`, `tv_`), so
# the floor costs the gate no reach.
MIN_IDENTIFIER_LENGTH = 3
ARCHIVE_IDENTIFIER = re.compile(
    r"(?<![A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]{%d,}(?![A-Za-z0-9_])"
    % (MIN_IDENTIFIER_LENGTH - 1))
BACKTICKED = re.compile(r"`([^`]+)`")

# How to write a copy of the archive without its debug information, in the
# order the checker tries them. All three keep the archive an archive and keep
# every symbol and every literal; they differ only in which toolchain ships
# them. `objcopy` is binutils, installed on the hosted ubuntu-24.04 image with
# the toolchain; `strip -S -o` is Apple's strip on the hosted macOS image,
# where no objcopy of any spelling exists (`xcrun --find llvm-objcopy` fails).
# GNU strip understands the same `-S -o` spelling, so the last row alone would
# already cover both runners; the objcopy rows are preferred because they say
# "copy without debug" instead of "strip into a copy".
STRIP_TOOLS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("llvm-objcopy", ("llvm-objcopy", "--strip-debug", "{input}", "{output}")),
    ("objcopy", ("objcopy", "--strip-debug", "{input}", "{output}")),
    ("strip", ("strip", "-S", "-o", "{output}", "{input}")),
)


class InfrastructureError(Exception):
    """The measurement could not be taken; exit status 2."""


@dataclass(frozen=True)
class Hit:
    kind: str      # "identifier" or "phrase"
    token: str     # the identifier, or the whole text for a phrase hit
    pattern: str
    source: str    # "strings", "nm", "header" (code) or "header-literal"
    line: str


@dataclass(frozen=True)
class Finding:
    kind: str      # "unruled" or "stale"
    token: str
    detail: str

    def __str__(self) -> str:
        return f"{self.kind} {self.token!r}: {self.detail}"


@dataclass(frozen=True)
class Ruled:
    identifiers: frozenset[str]
    phrases: frozenset[str]


def ruled_entries(adr_text: str) -> Ruled:
    """The first column of every table row in the ADR's residual section.

    A backticked token that is a plain identifier is ruled by name; a token with
    spaces or punctuation is a ruled phrase (a text is covered when it contains
    it); a token with a path separator is a citation, not a ruling."""
    start = adr_text.find(SECTION_HEADING)
    if start < 0:
        raise InfrastructureError("ADR section not found: " + SECTION_HEADING)
    section = adr_text[start + len(SECTION_HEADING):]
    following = re.search(r"\n## ", section)
    if following:
        section = section[:following.start()]
    identifiers: set[str] = set()
    phrases: set[str] = set()
    for line in section.splitlines():
        if not line.startswith("|"):
            continue
        first = line.split("|")[1]
        for token in BACKTICKED.findall(first):
            token = token.strip()
            if "/" in token:
                continue
            if IDENTIFIER.fullmatch(token):
                identifiers.add(token)
            else:
                phrases.add(token)
    if not identifiers:
        raise InfrastructureError("ADR residual section lists no ruled identifier")
    return Ruled(frozenset(identifiers), frozenset(phrases))


def scan(lines: list[str], source: str, *, texts: bool = True,
         locations: list[str] | None = None) -> list[Hit]:
    """The vocabulary's hits in `lines`.

    `texts` says whether the lines are texts -- archive strings, symbol lines,
    header string literals -- which the sentinel and phrase patterns read; a
    header's code (literals blanked) is not. A mangled token is skipped: its
    symbol is judged by the demangled line `nm -C` prints for it."""
    hits: list[Hit] = []
    for index, raw in enumerate(lines):
        line = raw.rstrip("\n")
        if source == "strings" and SANITIZER_FRAME.match(line):
            continue
        where = f"{locations[index]}: {line}" if locations is not None else line
        for identifier in sorted(set(ARCHIVE_IDENTIFIER.findall(line))):
            if MANGLED.match(identifier) or identifier in LANGUAGE_DUNDERS:
                continue
            if SENTINEL[1].match(identifier):
                if texts:
                    hits.append(Hit("identifier", identifier, SENTINEL[0], source, where))
                continue
            for name, pattern in IDENTIFIER_PATTERNS:
                if pattern.search(identifier):
                    hits.append(Hit("identifier", identifier, name, source, where))
                    break
        if texts:
            for name, pattern in PHRASE_PATTERNS:
                if pattern.search(line):
                    hits.append(Hit("phrase", line, name, source, where))
                    break
    return hits


def symbol_name(token: str, source: str) -> str:
    """A symbol's name as the ADR spells it: Mach-O prefixes every C symbol
    with one `_` that ELF does not, so `_strategy_position_size` and
    `strategy_position_size` are one name. Header text carries no decoration."""
    if source in ("strings", "nm") and token.startswith("_") and not token.startswith("__"):
        return token[1:]
    return token


def evaluate(strings_lines: list[str], nm_lines: list[str], ruled: Ruled,
             header_lines: tuple[list[str], list[str], list[str], list[str]] | None = None,
             source_literals: tuple[list[str], list[str]] | None = None
             ) -> tuple[list[Finding], dict[str, int]]:
    """Judge the archive's two surfaces and, when given, the installed headers'
    (code lines and their locations, literal texts and theirs; see
    read_headers) and the kernel sources' string literals (texts and their
    locations; see read_source_literals)."""
    hits = scan(strings_lines, "strings") + scan(nm_lines, "nm")
    header_hits: list[Hit] = []
    if header_lines is not None:
        code, code_where, literals, literal_where = header_lines
        header_hits = (scan(code, "header", texts=False, locations=code_where)
                       + scan(literals, "header-literal", locations=literal_where))
    source_hits: list[Hit] = []
    if source_literals is not None:
        texts, where = source_literals
        source_hits = scan(texts, "source-literal", locations=where)
    findings: list[Finding] = []
    present: set[str] = set()
    covered: set[str] = set()
    reported: set[tuple[str, str]] = set()
    for hit in hits + header_hits + source_hits:
        if hit.kind == "identifier":
            name = symbol_name(hit.token, hit.source)
            ruled_as = next((candidate for candidate in (hit.token, name)
                             if candidate in ruled.identifiers), None)
            if ruled_as is not None:
                present.add(ruled_as)
            elif (hit.kind, name) not in reported:
                reported.add((hit.kind, name))
                findings.append(Finding(
                    "unruled", name,
                    f"identifier matches residual vocabulary {hit.pattern!r} via {hit.source} "
                    f"and has no ADR-0001 row: {hit.line[:160]}"))
        else:
            covering = [phrase for phrase in ruled.phrases if phrase in hit.line]
            if covering:
                covered.update(covering)
            elif (hit.kind, hit.token) not in reported:
                reported.add((hit.kind, hit.token))
                findings.append(Finding(
                    "unruled", hit.token[:160],
                    f"text matches residual vocabulary {hit.pattern!r} via {hit.source} "
                    "and contains no ADR-0001 phrase"))
    def matched(identifier: str) -> bool:
        return (any(pattern.search(identifier) for _, pattern in IDENTIFIER_PATTERNS)
                or bool(SENTINEL[1].match(identifier)))
    for identifier in sorted(ruled.identifiers):
        if identifier in present:
            continue
        if matched(identifier):
            findings.append(Finding(
                "stale", identifier,
                "ruled in ADR-0001 but no longer in the archive, the installed headers or "
                "a kernel source literal; drop its row"))
    for phrase in sorted(ruled.phrases):
        if phrase in covered:
            continue
        if any(pattern.search(phrase) for _, pattern in PHRASE_PATTERNS):
            findings.append(Finding(
                "stale", phrase,
                "ruled in ADR-0001 but no archive, header or kernel source text contains it; "
                "drop its row"))
    summary = {
        "hits": len(hits),
        "identifierHits": sum(1 for hit in hits if hit.kind == "identifier"),
        "phraseHits": sum(1 for hit in hits if hit.kind == "phrase"),
        "headerHits": len(header_hits),
        "sourceHits": len(source_hits),
        "ruledIdentifiersPresent": len(present),
        "ruledPhrasesCovered": len(covered),
        "ruledIdentifiers": len(ruled.identifiers),
        "ruledPhrases": len(ruled.phrases),
        "findings": len(findings),
    }
    return findings, summary


def kernel_install_exclusions(cmake_text: str) -> frozenset[str]:
    """The directory patterns the kernel-only header install excludes.

    CMakeLists.txt installs include/pineforge in both profiles; the kernel-only
    rule is the one call carrying `PATTERN "<name>" EXCLUDE` clauses. Reading
    it here keeps one source of truth for what the kernel profile ships."""
    calls = [match.group(0) for match in re.finditer(
        r"install\(\s*DIRECTORY\s+include/pineforge\b[^)]*\)", cmake_text)]
    excluding = [call for call in calls if re.search(r"\bEXCLUDE\b", call)]
    if len(excluding) != 1:
        raise InfrastructureError(
            "CMakeLists.txt: expected exactly one kernel-only install(DIRECTORY "
            f"include/pineforge ... EXCLUDE) rule, found {len(excluding)}")
    names = re.findall(r'PATTERN\s+"([^"]+)"\s+EXCLUDE', excluding[0])
    if not names:
        raise InfrastructureError("CMakeLists.txt: the kernel-only install rule names no "
                                  "excluded pattern")
    return frozenset(names)


def kernel_profile_headers(include_root: Path, cmake_text: str | None = None) -> list[Path]:
    """Every header under `include_root`/pineforge the kernel profile installs.

    `include_root` is this tree's include/ or an installed prefix's include/;
    either way the install rule's exclusions apply, component by component,
    as CMake's PATTERN ... EXCLUDE does."""
    excluded = kernel_install_exclusions(
        CMAKE_LISTS.read_text(encoding="utf-8") if cmake_text is None else cmake_text)
    base = include_root / "pineforge"
    if not base.is_dir():
        raise InfrastructureError("no pineforge/ header tree under " + str(include_root))
    headers = []
    for path in sorted(base.rglob("*")):
        if not path.is_file() or path.suffix not in HEADER_SUFFIXES:
            continue
        parts = path.relative_to(base).parts
        if any(fnmatch.fnmatchcase(part, pattern) for part in parts for pattern in excluded):
            continue
        headers.append(path)
    return headers


_IDENT_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")


def split_code_and_literals(source: str) -> tuple[str, list[tuple[int, str]]]:
    """C/C++ text without its comments, string literals blanked to "", and the
    literals' contents (between the quotes) with their 1-based line numbers.

    Lexical, not regex: a `//` or `/*` inside a literal never opens a comment,
    raw strings R"d(...)d" are literals, a digit separator is not a character
    literal, and a backslash-newline continues a line comment. Newlines are
    kept, so every code line keeps its number."""
    out: list[str] = []
    literals: list[tuple[int, str]] = []
    i, n, line = 0, len(source), 1
    while i < n:
        c = source[i]
        if source.startswith("//", i):
            j = i + 2
            while j < n and not (source[j] == "\n" and source[j - 1] != "\\"):
                if source[j] == "\n":
                    line += 1
                    out.append("\n")
                j += 1
            i = j
            continue
        if source.startswith("/*", i):
            j = source.find("*/", i + 2)
            if j < 0:
                raise InfrastructureError("unterminated block comment")
            newlines = source.count("\n", i, j)
            out.append(" " + "\n" * newlines)
            line += newlines
            i = j + 2
            continue
        if c == '"':
            k = i - 1
            while k >= 0 and source[k] in _IDENT_CHARS:
                k -= 1
            if source[k + 1:i] in ("R", "u8R", "uR", "UR", "LR"):
                open_paren = source.find("(", i + 1)
                close = source.find(")" + source[i + 1:open_paren] + '"', open_paren + 1)
                if open_paren < 0 or close < 0:
                    raise InfrastructureError("unterminated raw string")
                end = close + (open_paren - i - 1) + 2
            else:
                end = i + 1
                while end < n and source[end] != '"':
                    if source[end] == "\\":
                        end += 1
                    elif source[end] == "\n":
                        raise InfrastructureError("newline in a string literal")
                    end += 1
                end += 1
            literals.append((line, source[i + 1:end - 1]))
            newlines = source.count("\n", i, end)
            out.append('""' + "\n" * newlines)
            line += newlines
            i = end
            continue
        if c == "'":
            k = i - 1
            while k >= 0 and source[k] in _IDENT_CHARS:
                k -= 1
            if source[k + 1:i][:1].isdigit():
                out.append(c)
                i += 1
                continue
            end = i + 1
            while end < n and source[end] != "'":
                if source[end] == "\\":
                    end += 1
                end += 1
            out.append(source[i:end + 1])
            i = end + 1
            continue
        if c == "\n":
            line += 1
        out.append(c)
        i += 1
    return "".join(out), literals


def read_headers(include_root: Path, headers: list[Path]
                 ) -> tuple[list[str], list[str], list[str], list[str]]:
    """The installed headers as the gate reads them: code lines (comments
    stripped, literals blanked) with their `file:line` locations, and string
    literals with theirs."""
    code: list[str] = []
    code_where: list[str] = []
    literals: list[str] = []
    literal_where: list[str] = []
    for header in headers:
        relative = header.relative_to(include_root).as_posix()
        text, found = split_code_and_literals(header.read_text(encoding="utf-8"))
        for number, content in enumerate(text.split("\n"), start=1):
            if content.strip():
                code.append(content)
                code_where.append(f"{relative}:{number}")
        for number, literal in found:
            literals.append(literal)
            literal_where.append(f"{relative}:{number}")
    return code, code_where, literals, literal_where


def kernel_translation_units(cmake_text: str) -> list[str]:
    """The kernel TUs, repository-relative, as `set(PINEFORGE_KERNEL_SOURCES ...)`
    lists them in CMakeLists.txt -- the one list both archives compile."""
    calls = KERNEL_SOURCES.findall(cmake_text)
    if len(calls) != 1:
        raise InfrastructureError("CMakeLists.txt: expected exactly one "
                                  "set(PINEFORGE_KERNEL_SOURCES ...), found " + str(len(calls)))
    units = calls[0].split()
    if not units:
        raise InfrastructureError("CMakeLists.txt: PINEFORGE_KERNEL_SOURCES lists no "
                                  "translation unit")
    return units


def archive_members(archive: Path) -> list[str]:
    """The object members `ar t` lists; an archive's symbol index is not one."""
    return [line.strip() for line in run_tool(["ar", "t", str(archive)]).splitlines()
            if line.strip().endswith(".o")]


def kernel_source_files(root: Path, cmake_text: str, members: list[str]) -> list[Path]:
    """The translation units `members` were compiled from, then the src/ headers
    they include.

    The archive must be exactly CMakeLists.txt's kernel list -- CMake names
    each member `<file>.o` -- so the literals read are the ones this archive
    compiled, never a source-layer TU's (src/compat/pine/ has a
    reservation_expansion.cpp too). A header outside src/ is either an
    installed one, already its own surface, or a system header."""
    units = kernel_translation_units(cmake_text)
    expected = [Path(unit).name + ".o" for unit in units]
    if len(set(expected)) != len(expected):
        raise InfrastructureError("PINEFORGE_KERNEL_SOURCES lists two translation units "
                                  "of one file name")
    missing = sorted(set(expected) - set(members))
    extra = sorted(set(members) - set(expected))
    if missing or extra:
        raise InfrastructureError(
            "the archive's members are not CMakeLists.txt's PINEFORGE_KERNEL_SOURCES ("
            + "; ".join(part for part in ("missing " + " ".join(missing) if missing else "",
                                          "extra " + " ".join(extra) if extra else "")
                        if part) + ")")
    source_root = (root / "src").resolve()
    files: list[Path] = []
    seen: set[Path] = set()
    pending = [root / unit for unit in units]
    while pending:
        path = pending.pop(0)
        resolved = path.resolve()
        if resolved in seen:
            continue
        if not path.is_file():
            raise InfrastructureError("kernel source is missing: " + str(path))
        seen.add(resolved)
        files.append(path)
        text = path.read_text(encoding="utf-8")
        lines = text.split("\n")
        for number, literal in split_code_and_literals(text)[1]:
            if not INCLUDE_DIRECTIVE.match(lines[number - 1]):
                continue
            for base in (path.parent, root / "src"):
                candidate = (base / literal).resolve()
                if candidate.is_file():
                    if candidate.is_relative_to(source_root) and candidate not in seen:
                        pending.append(candidate)
                    break
    return files


def repository_path(path: Path, root: Path) -> str:
    """`path` as a location under `root`, or as given when it lies elsewhere."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def read_source_literals(root: Path, files: list[Path]) -> tuple[list[str], list[str]]:
    """The string literals of `files` with their `file:line` locations: comments
    stripped (split_code_and_literals), an include directive's path skipped."""
    literals: list[str] = []
    where: list[str] = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        lines = text.split("\n")
        relative = repository_path(path, root)
        for number, literal in split_code_and_literals(text)[1]:
            if INCLUDE_DIRECTIVE.match(lines[number - 1]):
                continue
            literals.append(literal)
            where.append(f"{relative}:{number}")
    return literals, where


def run_tool(argv: list[str]) -> str:
    if shutil.which(argv[0]) is None:
        raise InfrastructureError(argv[0] + " is not on PATH")
    try:
        result = subprocess.run(argv, text=True, capture_output=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise InfrastructureError(" ".join(argv) + " could not run: " + str(error)) from error
    if result.returncode:
        raise InfrastructureError(" ".join(argv) + " failed:\n" + result.stdout + result.stderr)
    return result.stdout


def strip_debug_info(archive: Path, destination: Path) -> str:
    """Write `archive` without its debug information to `destination`.

    Returns the name of the tool that did it. Fails CLOSED: with no stripping
    tool the gate cannot tell a linkable name from a DWARF one, and reading the
    debug information anyway is exactly the profile-dependent verdict lane P2b
    removed."""
    tried: list[str] = []
    for name, template in STRIP_TOOLS:
        if shutil.which(template[0]) is None:
            tried.append(template[0])
            continue
        argv = [part.format(input=str(archive), output=str(destination))
                for part in template]
        run_tool(argv)
        if not destination.is_file() or destination.stat().st_size == 0:
            raise InfrastructureError(
                " ".join(argv) + " produced no archive at " + str(destination))
        return name
    raise InfrastructureError(
        "no tool to strip debug information from " + str(archive) + "; tried " +
        ", ".join(tried) + ". The gate reads the symbol table and the string "
        "literals, never the debug info, so it cannot run without one "
        "(install binutils for objcopy, or use the platform strip).")


def read_archive(archive: Path, workdir: Path) -> tuple[list[str], list[str], str]:
    """`strings -a` over a debug-stripped copy, `nm -C` over the archive itself.

    `nm` reads the symbol table and never the debug information, so it is
    already profile-independent and is taken from the archive a consumer
    actually links; only the literal scan needs the stripped copy."""
    if not archive.is_file():
        raise InfrastructureError("kernel archive is missing: " + str(archive))
    stripped = workdir / ("stripped-" + archive.name)
    tool = strip_debug_info(archive, stripped)
    strings_text = run_tool(["strings", "-a", str(stripped)])
    nm_text = run_tool(["nm", "-C", str(archive)])
    return strings_text.splitlines(), nm_text.splitlines(), tool


def check(archive: Path, adr: Path = ADR, *, evidence_dir: Path | None = None,
          include_root: Path = INCLUDE_ROOT, source_root: Path = ROOT) -> int:
    ruled = ruled_entries(adr.read_text(encoding="utf-8"))
    cmake_text = (source_root / "CMakeLists.txt").read_text(encoding="utf-8")
    headers = kernel_profile_headers(include_root, cmake_text)
    header_lines = read_headers(include_root, headers)
    with tempfile.TemporaryDirectory(prefix="pineforge-kernel-residuals-") as workdir:
        strings_lines, nm_lines, tool = read_archive(archive, Path(workdir))
    sources = kernel_source_files(source_root, cmake_text, archive_members(archive))
    source_literals = read_source_literals(source_root, sources)
    units = len(kernel_translation_units(cmake_text))
    findings, summary = evaluate(strings_lines, nm_lines, ruled, header_lines=header_lines,
                                 source_literals=source_literals)
    if evidence_dir is not None:
        evidence_dir.mkdir(parents=True, exist_ok=True)
        (evidence_dir / "strings.txt").write_text("\n".join(strings_lines) + "\n")
        (evidence_dir / "nm.txt").write_text("\n".join(nm_lines) + "\n")
        (evidence_dir / "headers.txt").write_text(
            "\n".join(header.relative_to(include_root).as_posix() for header in headers) + "\n")
        (evidence_dir / "sources.txt").write_text(
            "\n".join(repository_path(path, source_root) for path in sources) + "\n")
        (evidence_dir / "summary.json").write_text(json.dumps({
            "archive": str(archive), "adr": str(adr), "stripTool": tool,
            "includeRoot": str(include_root), "headers": len(headers),
            "sources": len(sources), "sourceLiterals": len(source_literals[0]),
            "summary": summary,
            "findings": [finding.__dict__ for finding in findings]}, indent=2) + "\n")
    for finding in findings:
        print("check_kernel_residuals: " + str(finding), file=sys.stderr)
    verdict = "FAIL" if findings else "OK"
    print(f"kernel residuals: {archive.name}: linkable surface = "
          f"{len(nm_lines)} nm -C lines and {len(strings_lines)} strings -a lines "
          f"of a {tool} debug-stripped copy; installed surface = {len(headers)} "
          f"kernel-profile headers under {include_root}; source surface = "
          f"{len(source_literals[0])} string literals of the archive's {units} "
          f"translation units and the {len(sources) - units} src/ headers they include")
    print(f"kernel residuals: {archive.name}: {summary['hits']} archive hits "
          f"({summary['identifierHits']} identifiers, {summary['phraseHits']} texts), "
          f"{summary['headerHits']} installed-header hits and "
          f"{summary['sourceHits']} kernel-source hits "
          f"against {summary['ruledIdentifiers']} ruled identifiers and "
          f"{summary['ruledPhrases']} ruled texts in {adr.name}: "
          f"{summary['findings']} findings ... {verdict}")
    return 1 if findings else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE,
                        help="libpineforge_kernel.a to measure (default: %(default)s)")
    parser.add_argument("--adr", type=Path, default=ADR,
                        help="the ADR whose residual tables are the ruling (default: %(default)s)")
    parser.add_argument("--evidence-dir", type=Path, default=None,
                        help="write strings.txt, nm.txt, headers.txt and summary.json here")
    parser.add_argument("--headers", type=Path, default=INCLUDE_ROOT,
                        help="include root whose pineforge/ tree is read as the kernel "
                             "profile installs it -- this tree's include/ or an installed "
                             "prefix's (default: %(default)s)")
    args = parser.parse_args(argv)
    try:
        return check(args.archive.resolve(), args.adr.resolve(),
                     evidence_dir=args.evidence_dir, include_root=args.headers.resolve())
    except InfrastructureError as error:
        print("check_kernel_residuals: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
