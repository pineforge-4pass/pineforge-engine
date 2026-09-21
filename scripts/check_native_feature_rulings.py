#!/usr/bin/env python3
"""Every NativeRunSpec field the Pine adapter never declares carries a ruling and a native consumer.

R5 audit lane P6. The kernel's run-spec features are opt-in, so a field the
adapter's project() never assigns is either a deliberate boundary decision or
dead weight nobody decided about. This check makes the difference mechanical:

  * the FIELDS are read from `struct NativeRunSpec`
    (include/pineforge/native_run_spec.hpp);
  * the adapter DECLARES a field when PineExecutionAdapter::project()
    (src/source/pine_adapter.cpp) assigns it -- project() is the source layer's
    one spec construction site, and a second one fails this check until it is
    taught about it;
  * every other field must have a row in ADR-0001's ruling table (between the
    `native-feature-rulings` markers), and the row must name executed native
    consumers that exist and spell the field:
        native-only     the adapter will not declare it, by ruling; needs at
                        least one examples/native/ host and one tests/ unit
        adapter-policy  the adapter keeps its own rule on top of the default;
                        needs at least one native consumer
        adapter-hook    the adapter reaches the same kernel feature through a
                        begin-time hook instead of the spec field; the row's
                        third cell names that hook first, and the source layer
                        must call it
  * a row for a field project() does assign is a stale ruling, and a row for a
    name that is not a field is a typo: both fail. So does a member of a ruled
    name assigned ANYWHERE in the source layer through any object (a helper
    that fills the spec by reference would otherwise hide a declaration):
    fail closed, then drop the row or teach this check the unrelated member.

Stdlib only. Exit 0 when clean, 1 on a finding or when a source no longer has
the shape this check reads, 2 on usage errors.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]
HEADER = "include/pineforge/native_run_spec.hpp"
ADAPTER = "src/source/pine_adapter.cpp"
ADR = "docs/adr/0001-kernel-adapter-boundary.md"
SOURCE_LAYER_DIRS = ("src/source", "src/compat/pine",
                     "include/pineforge/source", "include/pineforge/compat/pine")
BEGIN_MARKER = "<!-- native-feature-rulings:begin -->"
END_MARKER = "<!-- native-feature-rulings:end -->"
RULING_KINDS = ("native-only", "adapter-policy", "adapter-hook")
EXAMPLES_PREFIX = "examples/native/"
TESTS_PREFIX = "tests/"


class ShapeError(RuntimeError):
    """A source does not have the shape this check reads."""


@dataclass(frozen=True)
class Ruling:
    fields: tuple[str, ...]
    kind: str
    hook: str | None
    consumers: tuple[str, ...]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def balanced_body(text: str, signature: str) -> str:
    """The brace-balanced body of the first definition whose signature matches."""
    match = re.search(signature, text)
    if not match:
        raise ShapeError("definition not found: " + signature)
    start = text.find("{", match.end())
    if start < 0:
        raise ShapeError("definition has no body: " + signature)
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    raise ShapeError("unbalanced body: " + signature)


def spec_fields(header_text: str) -> list[str]:
    """The data members of `struct NativeRunSpec`, in declaration order."""
    body = strip_comments(balanced_body(strip_comments(header_text), r"\bstruct\s+NativeRunSpec\s*(?={)"))
    fields: list[str] = []
    for statement in body.split(";"):
        statement = statement.strip()
        if not statement or "(" in statement.split("=")[0]:
            continue
        declarator = re.split(r"=|\{", statement, maxsplit=1)[0].strip()
        match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", declarator)
        if not match:
            raise ShapeError("unreadable NativeRunSpec member: " + statement)
        fields.append(match.group(1))
    if len(fields) < 10 or len(set(fields)) != len(fields):
        raise ShapeError("NativeRunSpec members not recognised")
    return fields


def adapter_declared(adapter_text: str, fields: list[str]) -> set[str]:
    """The fields project() assigns, directly or through a member of theirs."""
    project = strip_comments(balanced_body(
        adapter_text, r"NativeRunSpec\s+PineExecutionAdapter::project\s*\("))
    if not re.search(r"\bNativeRunSpec\s+spec\s*;", project):
        raise ShapeError("project() no longer builds a local `NativeRunSpec spec;`")
    declared: set[str] = set()
    for field in fields:
        target = r"\bspec\." + re.escape(field) + r"\b(?:\.[A-Za-z_][A-Za-z0-9_]*)*"
        if re.search(target + r"\s*=(?!=)", project) \
                or re.search(target + r"\.(?:emplace|emplace_back|push_back|assign)\s*\(", project):
            declared.add(field)
    return declared


def assigned_through_any_object(source_layer_text: str, field: str) -> bool:
    """`<object>.<field> = ...` (or a container write) anywhere in the source layer."""
    target = r"(?:\.|->)" + re.escape(field) + r"\b(?:\.[A-Za-z_][A-Za-z0-9_]*)*"
    code = strip_comments(source_layer_text)
    return bool(re.search(target + r"\s*=(?!=)", code)
                or re.search(target + r"\.(?:emplace|emplace_back|push_back|assign)\s*\(", code))


def spec_construction_sites(source_layer_text: str) -> int:
    """How many default-constructed NativeRunSpec locals the source layer holds."""
    return len(re.findall(r"\bNativeRunSpec\s+[A-Za-z_][A-Za-z0-9_]*\s*;", strip_comments(source_layer_text)))


def parse_rulings(adr_text: str) -> list[Ruling]:
    if adr_text.count(BEGIN_MARKER) != 1 or adr_text.count(END_MARKER) != 1:
        raise ShapeError("ADR ruling table markers missing or repeated: " + BEGIN_MARKER)
    block = adr_text.split(BEGIN_MARKER, 1)[1].split(END_MARKER, 1)[0]
    rulings: list[Ruling] = []
    rows = [line.strip() for line in block.splitlines() if line.strip().startswith("|")]
    for index, line in enumerate(rows):
        separator = re.compile(r"\|[\s|:-]+\|")
        if separator.fullmatch(line):
            continue
        if index + 1 < len(rows) and separator.fullmatch(rows[index + 1]):
            continue  # the header row: the one a separator follows
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if len(cells) != 5:
            raise ShapeError("ruling row needs five cells: " + line)
        fields = tuple(re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", cells[0]))
        if not fields:
            raise ShapeError("ruling row names no field in its first cell: " + line[:60])
        kinds = [kind for kind in RULING_KINDS if re.search(r"\*\*" + re.escape(kind) + r"\*\*", cells[1])]
        if len(kinds) != 1:
            raise ShapeError("ruling row needs exactly one bold kind of " + "/".join(RULING_KINDS)
                             + ": " + ", ".join(fields))
        hook = None
        if kinds[0] == "adapter-hook":
            named = re.findall(r"`([A-Za-z_][A-Za-z0-9_:]*)`", cells[2])
            if not named:
                raise ShapeError("adapter-hook row names no hook in its third cell: " + ", ".join(fields))
            hook = named[0].split("::")[-1]
        consumers = tuple(re.findall(r"`([A-Za-z0-9_./-]+\.(?:cpp|c|hpp|h))`", cells[4]))
        rulings.append(Ruling(fields, kinds[0], hook, consumers))
    return rulings


def check(header_text: str, adapter_text: str, adr_text: str, source_layer_text: str,
          read_file: Callable[[str], str | None]) -> list[str]:
    """Findings for one tree; shape errors propagate."""
    fields = spec_fields(header_text)
    declared = adapter_declared(adapter_text, fields)
    findings: list[str] = []
    sites = spec_construction_sites(source_layer_text)
    if sites != 1:
        findings.append(f"the source layer builds {sites} NativeRunSpec locals; this check reads "
                        "exactly one (project()): teach it the new construction site")
    rulings = parse_rulings(adr_text)
    ruled: dict[str, Ruling] = {}
    for ruling in rulings:
        for field in ruling.fields:
            if field not in fields:
                findings.append(f"{field}: the ruling table names it, but NativeRunSpec has no such field")
            elif field in ruled:
                findings.append(f"{field}: ruled twice")
            ruled[field] = ruling
    for field in fields:
        if field in declared:
            if field in ruled:
                findings.append(f"{field}: stale ruling -- project() assigns it, so the adapter "
                                "is a consumer; drop the row")
            continue
        if field in ruled and assigned_through_any_object(source_layer_text, field):
            findings.append(f"{field}: stale ruling -- the source layer assigns a member of that "
                            "name outside project(); drop the row, or teach this check the "
                            "unrelated member")
        if field not in ruled:
            findings.append(f"{field}: the adapter never declares it and no ruling row covers it "
                            f"({ADR}, between the native-feature-rulings markers)")
    for ruling in rulings:
        name = ", ".join(ruling.fields)
        if not ruling.consumers:
            findings.append(f"{name}: the ruling names no native consumer")
        for path in ruling.consumers:
            text = read_file(path)
            if text is None:
                findings.append(f"{name}: consumer {path} does not exist")
                continue
            for field in ruling.fields:
                if not re.search(r"\b" + re.escape(field) + r"\b", text):
                    findings.append(f"{field}: consumer {path} never spells it")
        if ruling.kind == "native-only":
            if not any(path.startswith(EXAMPLES_PREFIX) for path in ruling.consumers):
                findings.append(f"{name}: a native-only ruling needs an {EXAMPLES_PREFIX} host that exercises it")
            if not any(path.startswith(TESTS_PREFIX) for path in ruling.consumers):
                findings.append(f"{name}: a native-only ruling needs a {TESTS_PREFIX} unit that pins it")
        if ruling.kind == "adapter-hook":
            if not re.search(r"\b" + re.escape(ruling.hook or "") + r"\s*\(", strip_comments(source_layer_text)):
                findings.append(f"{name}: the source layer never calls the hook `{ruling.hook}` the ruling names")
    return findings


def read_source_layer(root: Path) -> str:
    parts: list[str] = []
    for directory in SOURCE_LAYER_DIRS:
        for pattern in ("*.cpp", "*.hpp", "*.inc"):
            for path in sorted((root / directory).glob(pattern)):
                parts.append(path.read_text(errors="replace"))
    if not parts:
        raise ShapeError("no source-layer translation unit found under " + ", ".join(SOURCE_LAYER_DIRS))
    return "\n".join(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--list", action="store_true", help="print the inventory, one field per line")
    args = parser.parse_args(argv)
    root = args.root.resolve()

    def read_file(relative: str) -> str | None:
        path = root / relative
        return path.read_text(errors="replace") if path.is_file() else None

    try:
        header_text = (root / HEADER).read_text()
        adapter_text = (root / ADAPTER).read_text()
        adr_text = (root / ADR).read_text()
    except OSError as error:
        print(f"check_native_feature_rulings: cannot read an input: {error}", file=sys.stderr)
        return 2
    try:
        source_layer_text = read_source_layer(root)
        findings = check(header_text, adapter_text, adr_text, source_layer_text, read_file)
        fields = spec_fields(header_text)
        declared = adapter_declared(adapter_text, fields)
        rulings = parse_rulings(adr_text)
    except ShapeError as error:
        print(f"check_native_feature_rulings: {error}", file=sys.stderr)
        return 1
    if args.list:
        kinds = {field: ruling.kind for ruling in rulings for field in ruling.fields}
        for field in fields:
            print(f"{field:32s} {'adapter declares (project())' if field in declared else kinds.get(field, 'UNRULED')}")
    for finding in findings:
        print("check_native_feature_rulings: " + finding)
    if findings:
        return 1
    counts = {kind: sum(len(r.fields) for r in rulings if r.kind == kind) for kind in RULING_KINDS}
    print(f"check_native_feature_rulings: {len(fields)} NativeRunSpec fields, {len(declared)} declared by "
          f"the adapter's project(), {len(fields) - len(declared)} ruled ("
          + ", ".join(f"{counts[kind]} {kind}" for kind in RULING_KINDS)
          + "), every ruling backed by an executed native consumer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
