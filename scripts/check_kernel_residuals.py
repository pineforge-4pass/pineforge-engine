#!/usr/bin/env python3
"""The kernel-only archive names no TradingView vocabulary outside ADR-0001's table.

R5 gap lane P2. ADR-0001 ("Residual TradingView-named surface in the kernel-only
archive") rules every Pine / TradingView-shaped string the kernel archive still
carries, and says a name that is not in that table must not appear. Until this
lane nothing ran the measurement: the N14 probe was a documented grep, and it
was structurally blind to the pending-row names whose spelling holds no `pine`.
This guard runs `strings -a` and `nm -C` over libpineforge_kernel.a and matches
a fixed residual vocabulary against the ADR's own tables:

  * IDENTIFIER_PATTERNS -- an identifier (C/C++ name, field-name literal or
    demangled symbol part) that contains one of them must be listed in the
    first column of an ADR table, by its exact name;
  * PHRASE_PATTERNS -- a text (an error message, a label) that contains one
    of them must contain a phrase listed in that column;
  * and every listed name or phrase that the vocabulary matches must still
    be in the archive, so the table cannot go stale in either direction.

Exit 0 when every match is ruled and every ruling is live, 1 on a finding,
2 when the archive, the tools or the ADR section cannot be read.
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

ROOT = Path(__file__).resolve().parents[1]
ADR = ROOT / "docs" / "adr" / "0001-kernel-adapter-boundary.md"
DEFAULT_ARCHIVE = ROOT / "build-ci-kernel" / "lib" / "libpineforge_kernel.a"
SECTION_HEADING = "## Residual TradingView-named surface in the kernel-only archive"

# The residual vocabulary. Identifier patterns are matched against every
# identifier token of every `strings` line and every `nm -C` line; phrase
# patterns against the raw line. `pine` excludes the project's own name.
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
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
BACKTICKED = re.compile(r"`([^`]+)`")


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
        for identifier in sorted(set(IDENTIFIER.findall(line))):
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


def read_archive(archive: Path) -> tuple[list[str], list[str]]:
    if not archive.is_file():
        raise InfrastructureError("kernel archive is missing: " + str(archive))
    strings_text = run_tool(["strings", "-a", str(archive)])
    nm_text = run_tool(["nm", "-C", str(archive)])
    return strings_text.splitlines(), nm_text.splitlines()


def check(archive: Path, adr: Path = ADR, *, evidence_dir: Path | None = None) -> int:
    ruled = ruled_entries(adr.read_text(encoding="utf-8"))
    strings_lines, nm_lines = read_archive(archive)
    findings, summary = evaluate(strings_lines, nm_lines, ruled)
    if evidence_dir is not None:
        evidence_dir.mkdir(parents=True, exist_ok=True)
        (evidence_dir / "strings.txt").write_text("\n".join(strings_lines) + "\n")
        (evidence_dir / "nm.txt").write_text("\n".join(nm_lines) + "\n")
        (evidence_dir / "summary.json").write_text(json.dumps({
            "archive": str(archive), "adr": str(adr), "summary": summary,
            "findings": [finding.__dict__ for finding in findings]}, indent=2) + "\n")
    for finding in findings:
        print("check_kernel_residuals: " + str(finding), file=sys.stderr)
    verdict = "FAIL" if findings else "OK"
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
