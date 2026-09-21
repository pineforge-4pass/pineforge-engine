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

Exit 0 when every match is ruled and every ruling is live, 1 on a finding,
2 when the archive, the tools (including the strip tool: the gate fails CLOSED
rather than read debug information) or the ADR section cannot be read.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ADR = ROOT / "docs" / "adr" / "0001-kernel-adapter-boundary.md"
DEFAULT_ARCHIVE = ROOT / "build-ci-kernel" / "lib" / "libpineforge_kernel.a"
SECTION_HEADING = "## Residual TradingView-named surface in the kernel-only archive"

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
)
PHRASE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("strategy.", re.compile(r"\bstrategy\.[a-z_]{2,}")),
    ("ta.", re.compile(r"\bta\.[a-z_]{2,}")),
    ("request.security", re.compile(r"request\.security")),
    ("barmerge.", re.compile(r"\bbarmerge\.[a-z_]{2,}")),
    ("__margin_call__", re.compile(r"__margin_call__")),
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
    source: str    # "strings" or "nm"
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


def scan(lines: list[str], source: str) -> list[Hit]:
    hits: list[Hit] = []
    for raw in lines:
        line = raw.rstrip("\n")
        for identifier in sorted(set(ARCHIVE_IDENTIFIER.findall(line))):
            for name, pattern in IDENTIFIER_PATTERNS:
                if pattern.search(identifier):
                    hits.append(Hit("identifier", identifier, name, source, line))
                    break
        for name, pattern in PHRASE_PATTERNS:
            if pattern.search(line):
                hits.append(Hit("phrase", line, name, source, line))
                break
    return hits


def evaluate(strings_lines: list[str], nm_lines: list[str], ruled: Ruled
             ) -> tuple[list[Finding], dict[str, int]]:
    hits = scan(strings_lines, "strings") + scan(nm_lines, "nm")
    findings: list[Finding] = []
    present: set[str] = set()
    covered: set[str] = set()
    reported: set[tuple[str, str]] = set()
    for hit in hits:
        if hit.kind == "identifier":
            if hit.token in ruled.identifiers:
                present.add(hit.token)
            elif (hit.kind, hit.token) not in reported:
                reported.add((hit.kind, hit.token))
                findings.append(Finding(
                    "unruled", hit.token,
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
    for identifier in sorted(ruled.identifiers):
        if identifier in present:
            continue
        if any(pattern.search(identifier) for _, pattern in IDENTIFIER_PATTERNS):
            findings.append(Finding(
                "stale", identifier,
                "ruled in ADR-0001 but no longer in the archive; drop its row"))
    for phrase in sorted(ruled.phrases):
        if phrase in covered:
            continue
        if any(pattern.search(phrase) for _, pattern in PHRASE_PATTERNS):
            findings.append(Finding(
                "stale", phrase,
                "ruled in ADR-0001 but no archive text contains it; drop its row"))
    summary = {
        "hits": len(hits),
        "identifierHits": sum(1 for hit in hits if hit.kind == "identifier"),
        "phraseHits": sum(1 for hit in hits if hit.kind == "phrase"),
        "ruledIdentifiersPresent": len(present),
        "ruledPhrasesCovered": len(covered),
        "ruledIdentifiers": len(ruled.identifiers),
        "ruledPhrases": len(ruled.phrases),
        "findings": len(findings),
    }
    return findings, summary


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


def check(archive: Path, adr: Path = ADR, *, evidence_dir: Path | None = None) -> int:
    ruled = ruled_entries(adr.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="pineforge-kernel-residuals-") as workdir:
        strings_lines, nm_lines, tool = read_archive(archive, Path(workdir))
    findings, summary = evaluate(strings_lines, nm_lines, ruled)
    if evidence_dir is not None:
        evidence_dir.mkdir(parents=True, exist_ok=True)
        (evidence_dir / "strings.txt").write_text("\n".join(strings_lines) + "\n")
        (evidence_dir / "nm.txt").write_text("\n".join(nm_lines) + "\n")
        (evidence_dir / "summary.json").write_text(json.dumps({
            "archive": str(archive), "adr": str(adr), "stripTool": tool,
            "summary": summary,
            "findings": [finding.__dict__ for finding in findings]}, indent=2) + "\n")
    for finding in findings:
        print("check_kernel_residuals: " + str(finding), file=sys.stderr)
    verdict = "FAIL" if findings else "OK"
    print(f"kernel residuals: {archive.name}: linkable surface = "
          f"{len(nm_lines)} nm -C lines and {len(strings_lines)} strings -a lines "
          f"of a {tool} debug-stripped copy")
    print(f"kernel residuals: {archive.name}: {summary['hits']} archive hits "
          f"({summary['identifierHits']} identifiers, {summary['phraseHits']} texts) "
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
                        help="write strings.txt, nm.txt and summary.json here")
    args = parser.parse_args(argv)
    try:
        return check(args.archive.resolve(), args.adr.resolve(),
                     evidence_dir=args.evidence_dir)
    except InfrastructureError as error:
        print("check_kernel_residuals: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
