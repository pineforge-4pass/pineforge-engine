#!/usr/bin/env python3
"""Reject code-shaped names in installed/kernel comments that name no code.

This is a lexical guard, not a C++ name resolver. It checks the identifier
parts of qualified names against comment-free source and allows explicit
historical wording. The small exemption set below contains documented prose
labels and intentionally incomplete field prefixes, not callable symbols.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from cxxlex import lex_lines

ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".cc", ".inc"}
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CITED = re.compile(
    r"`([^`]+)`|([A-Za-z_][A-Za-z0-9_:]*)\s*\(|"
    r"([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)+)|"
    r"\b([a-z][a-z0-9]*(?:_[a-z0-9]+){2,})\b")
HISTORY = re.compile(r"\b(?:deleted|removed|retired|gone|historical|formerly|"
                     r"deprecated|superseded|no longer|before v[0-9]+|"
                     r"[0-9a-f]{8})\b", re.I)
# These are prose labels or examples, never promised declarations. A new
# exemption requires the reviewer to name why it is not an API/code name.
PROSE_LABELS = {
    "RTH_open", "last_bar_time", "last_traded_close", "next_period_open",
    "mean_sub_bar_volume", "source_price_time", "precommit_pod",
    "validation_typed_matrix", "pf_native_c_api", "pf_native_c_status",
    "pf_native_c_types", "pf_native_c_enums", "currency_fx", "k_best",
    "source_price_time", "Ambient",  # Ambient*Scope is a wildcard pattern.
}


def source_paths(root: Path):
    # Deliberately stay out of corpus and benchmark asset trees.
    for folder in ("src", "include"):
        base = root / folder
        if base.exists():
            yield from (p for p in base.rglob("*")
                        if p.is_file() and p.suffix in SOURCE_SUFFIXES)


def comment_paths(root: Path):
    include = root / "include/pineforge"
    if include.exists():
        yield from (p for p in include.rglob("*")
                    if p.is_file() and p.suffix in SOURCE_SUFFIXES
                    and "source" not in p.relative_to(include).parts)
    source = root / "src"
    if source.exists():
        yield from (p for p in source.iterdir()
                    if p.is_file() and p.suffix in SOURCE_SUFFIXES)


def findings(root: Path) -> list[str]:
    code_ids: set[str] = set()
    artefact_names: set[str] = set()
    for path in source_paths(root):
        artefact_names.add(path.stem)
        for code, _, literal in lex_lines(path.read_text(encoding="utf-8", errors="replace")):
            code_ids.update(IDENT.findall(code))
            code_ids.update(IDENT.findall(literal))
    for folder in ("tests", "examples"):
        base = root / folder
        if base.exists():
            artefact_names.update(p.stem for p in base.rglob("*")
                                  if p.is_file() and p.suffix in SOURCE_SUFFIXES)
    scripts = root / "scripts"
    if scripts.exists():
        artefact_names.update(p.stem for p in scripts.glob("*.py"))
        artefact_names.update(p.stem for p in scripts.glob("*.sh"))
    missing: list[tuple[Path, int, str]] = []
    for path in comment_paths(root):
        relative = path.relative_to(root)
        previous_comment = ""
        for line_no, (_, comment, _) in enumerate(
                lex_lines(path.read_text(encoding="utf-8", errors="replace")), 1):
            historical = HISTORY.search(comment) or (
                previous_comment and HISTORY.search(previous_comment))
            previous_comment = comment
            if not comment or historical:
                continue
            found_here: set[str] = set()
            for match in CITED.finditer(comment):
                if re.match(r"\.(?:cpp|hpp|h|c|cc)\b", comment[match.end():]):
                    continue  # a file citation, judged by documentation gates
                chunk = next(group for group in match.groups() if group)
                if "*" in chunk:
                    continue  # an intentionally incomplete name pattern
                names = IDENT.findall(chunk)
                for name in names:
                    if (name in found_here or name in code_ids
                            or name in artefact_names or name in PROSE_LABELS):
                        continue
                    if len(name) < 6 or name.startswith("_") or name.endswith("_"):
                        continue
                    if name.isupper():
                        continue  # prose enum examples and macro families
                    if "_" not in name and "::" not in chunk:
                        continue
                    found_here.add(name)
                    missing.append((relative, line_no, name))

    # A few kernel comments cite a test helper or fixture field. Resolve only
    # the still-missing names there, so the guard does not lex the whole test
    # corpus on every ci_verify source-check invocation.
    needed = {name for _, _, name in missing}
    external_ids: set[str] = set()
    for folder in ("tests", "examples"):
        base = root / folder
        if not base.exists() or not needed:
            continue
        paths = (p for p in base.rglob("*")
                 if p.is_file() and p.suffix in SOURCE_SUFFIXES)
        for path in sorted(paths, key=lambda p: ("fixtures" in p.parts, str(p))):
            if not needed:
                break
            raw = path.read_bytes()
            if not any(name.encode() in raw for name in needed):
                continue
            for code, _, literal in lex_lines(raw.decode("utf-8", errors="replace")):
                external_ids.update(IDENT.findall(code))
                external_ids.update(IDENT.findall(literal))
            needed.difference_update(external_ids)
    return [f"{relative}:{line_no}: dangling comment name {name}"
            for relative, line_no, name in missing if name not in external_ids]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    result = findings(args.root)
    for item in result:
        print(item, file=sys.stderr)
    print(f"dangling comment names: {len(result)} findings")
    return 1 if result else 0


if __name__ == "__main__":
    raise SystemExit(main())
