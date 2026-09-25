#!/usr/bin/env python3
"""Every kernel seam the source layer implements, and every kernel member it
writes, has a row in ADR-0001 or the design record.

Why this exists
---------------
The fourth claimed-vs-actual audit (AUDIT4-opus §4, X10) found adapter-only
kernel state and seams with no ruling anywhere: `Trade::exit_from_bracket`,
`broker_fill_event_seq_` (hashed, advanced by the adapter alone),
`last_bar_index_` / `last_bar_time_`, the `source_aux_security_*` seams,
`source_stream_entry_comment` (a dead seam), and the Pine host's publication
of the presented clock. The residual gate cannot see them -- their names hold
no TradingView word -- and the ADR's own tables name only what a lane
remembered to write down. A seam or a member the adapter writes is the
boundary itself; this gate makes each one a row.

What it reads
-------------
* KERNEL SEAMS: every `virtual ... source_<name>(` declared in a kernel header
  -- `include/pineforge/**` and `src/*.hpp` outside the `source/` and
  `compat/` trees -- comments stripped.
* ADAPTER-WRITTEN KERNEL MEMBERS: the data members of `BacktestEngine` (the
  trailing-underscore fields of its body) and the fields of its row structs
  `Trade` and `PyramidEntry`, that source-layer code (`src/source/**`,
  `src/compat/**`, `include/pineforge/source/**`,
  `include/pineforge/compat/**`, comments stripped) WRITES: an assignment
  (`=`, a compound assignment, an indexed assignment), an increment or
  decrement, or a mutating container call (`push_back`, `clear`, `erase`,
  ...). A read is not a write. A `BacktestEngine` member is written
  unqualified from a source class's own code, or through an object whose
  nearest declaration makes it a `PineStrategyHost` or a `BacktestEngine`
  (`host.bar_index_ = index`). A member a source class declares again under
  the same name is that class's own and is skipped. A row-struct field is
  written through the kernel's rows only (see `inventory`). The member reader
  is this script's own: scripts/check_broker_state_hash_coverage.py splits a
  class body at `;` alone, so a member declared right after an inline
  function body is invisible to it (`current_bar_`,
  `bar_magnifier_enabled_`, `trace_buffer_` on the tree this gate landed on).

What it requires
----------------
Each name must appear inside a backticked span in the FIRST CELL of a table
row of docs/adr/0001-kernel-adapter-boundary.md -- the cell that says what the
row rules; the rest of the row states who writes it and its ruling. A mention
in prose, or in another cell (a design row that quotes `engine.current_bar_ =
open_view` is about the kernel's own write, not the adapter's), is not a row.
A row-struct field's name is often a plain word (`time`, `qty`), so it counts
only spelled qualified: `Trade::time`, `PyramidEntry::qty`.

Exit 0 when every seam and written member has its row, 1 when one has not,
2 when a file cannot be read. `--list` prints the inventory.

R5 lane H-DOCGATES (AUDIT4-opus X10).
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys

import check_broker_state_hash_coverage as coverage

ROOT = Path(__file__).resolve().parent.parent
ENGINE = "include/pineforge/engine.hpp"
ADR = "docs/adr/0001-kernel-adapter-boundary.md"
ROW_STRUCTS = ("Trade", "PyramidEntry")
SOURCE_TREES = ("src/source", "src/compat", "include/pineforge/source",
                "include/pineforge/compat")
CXX = (".cpp", ".hpp", ".h", ".cc")
SEAM = re.compile(r"\bvirtual\b[^;{}()]*?\b(source_\w+)\s*\(")
ASSIGN = r"(?:\[[^\]\n]*\]\s*)?(?:[-+*/%|&^]|<<|>>)?=(?!=)"
MUTATE = (r"(?:\.|->)\s*(?:push_back|emplace_back|emplace|insert|erase|clear|assign|"
          r"resize|swap|pop_back|pop_front|push_front|reserve)\s*\(")


class Unreadable(Exception):
    """A file the gate must read is missing; exit status 2."""


@dataclass
class Inventory:
    seams: dict[str, str] = field(default_factory=dict)          # name -> declaring header
    written: dict[str, list[str]] = field(default_factory=dict)  # name -> write sites


def read(root: Path, relative: str) -> str:
    path = root / relative
    if not path.is_file():
        raise Unreadable(f"{relative} is missing")
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(text: str) -> str:
    """Comments out and every newline kept, so a write's line is its line."""
    text = coverage.BLOCK_COMMENT.sub(lambda m: " " + "\n" * m.group(0).count("\n"), text)
    return coverage.LINE_COMMENT.sub("", text)


def declarations(body: str) -> list[str]:
    """The top-level declarations of a class body: split at a `;` and at the
    `}` that closes an inline function or a nested type."""
    found: list[str] = []
    current: list[str] = []
    depth = 0
    for char in body:
        current.append(char)
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                found.append("".join(current))
                current = []
        elif char == ";" and depth == 0:
            found.append("".join(current))
            current = []
    return found


def data_fields(body: str, *, trailing_underscore: bool) -> set[str]:
    result: set[str] = set()
    for statement in declarations(body):
        value = re.sub(r"\b(?:public|private|protected)\s*:\s*", "", statement).strip()
        value = value.rstrip(";").strip()
        if not value or value.startswith(("using ", "friend ", "static_assert", "return ",
                                          "template")):
            continue
        if value.endswith("}") and (re.match(r"(?:enum|struct|class|union)\b", value)
                                    or "(" in value.split("{", 1)[0]):
            continue                       # a nested type or an inline function body
        if "=" in value:
            value = value.split("=", 1)[0]
        value = re.sub(r"\{[^{};]*\}\s*$", "", value).strip()
        if "(" in value:
            continue                       # a function declaration
        match = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$", value)
        if match and (not trailing_underscore or match.group(1).endswith("_")):
            result.add(match.group(1))
    return result


def class_body(text: str, kind: str, name: str) -> str:
    return coverage._named_body(strip_comments(text), kind, name)


def source_class_members(root: Path) -> set[str]:
    result: set[str] = set()
    for relative, classes in coverage.SOURCE_CLASSES.items():
        text = read(root, relative)
        for name in classes:
            kind = "struct" if name == "PineLanguageState" else "class"
            result |= data_fields(class_body(text, kind, name), trailing_underscore=True)
    return result


def nearest_declaration(text: str, name: str, before: int) -> str | None:
    """The line, up to the name, of `name`'s nearest declaration before
    offset `before` (with the rest of that line), or None."""
    declared = None
    for decl in re.finditer(
            r"(?:\b[\w:<>]+\s*[&*]*\s+|\bauto\s*&?\s*|\(\s*(?:const\s+)?auto\s*&\s*)"
            + re.escape(name) + r"\s*(?=[=:;{(),])", text[:before]):
        declared = decl
    if declared is None:
        return None
    line_start = text.rfind("\n", 0, declared.start()) + 1
    line_end = text.find("\n", declared.end())
    return text[line_start:line_end if line_end >= 0 else len(text)]


def kernel_headers(root: Path) -> list[Path]:
    found: list[Path] = []
    for path in sorted((root / "include/pineforge").rglob("*")):
        if path.suffix in (".h", ".hpp") and not {"source", "compat"} & set(
                path.relative_to(root / "include/pineforge").parts[:-1]):
            found.append(path)
    found += sorted(p for p in (root / "src").glob("*.hpp"))
    return found


def source_files(root: Path) -> list[Path]:
    found: list[Path] = []
    for tree in SOURCE_TREES:
        base = root / tree
        if base.is_dir():
            found += sorted(p for p in base.rglob("*") if p.suffix in CXX)
    return found


def inventory(root: Path) -> Inventory:
    result = Inventory()
    for header in kernel_headers(root):
        text = strip_comments(header.read_text(encoding="utf-8", errors="replace"))
        for match in SEAM.finditer(text):
            result.seams.setdefault(match.group(1), header.relative_to(root).as_posix())

    engine = read(root, ENGINE)
    members = data_fields(class_body(engine, "class", "BacktestEngine"), trailing_underscore=True)
    members -= source_class_members(root)
    fields: set[str] = set()
    for struct in ROW_STRUCTS:
        fields |= data_fields(class_body(engine, "struct", struct), trailing_underscore=False)
    texts = {p.relative_to(root).as_posix(): strip_comments(
        p.read_text(encoding="utf-8", errors="replace")) for p in source_files(root)}

    def record(name: str, relative: str, text: str, offset: int) -> None:
        line = text.count("\n", 0, offset) + 1
        result.written.setdefault(name, []).append(f"{relative}:{line}")

    host_types = re.compile(r"\b(?:source::)?(?:PineStrategyHost|BacktestEngine)\b")
    for name in sorted(members):
        n = re.escape(name)
        plain = re.compile(
            r"(?<![\w.>])\b" + n + r"\s*" + ASSIGN
            + r"|(?:\+\+|--)\s*\b" + n + r"\b|(?<![\w.>])\b" + n + r"\s*(?:\+\+|--)"
            + r"|(?<![\w.>])\b" + n + r"\s*" + MUTATE)
        qualified = re.compile(
            r"(?P<obj>\b\w+)\s*(?:\.|->)\s*" + n + r"\s*(?:" + ASSIGN
            + r"|\+\+|--|(?=(?:\.|->))" + MUTATE + r")")
        for relative, text in texts.items():
            for match in plain.finditer(text):
                record(name, relative, text, match.start())
            for match in qualified.finditer(text):
                obj = match.group("obj")
                declared = None if obj == "this" else nearest_declaration(text, obj, match.start())
                if obj == "this" or (declared is not None and host_types.search(declared)):
                    record(name, relative, text, match.start())

    # A row-struct field is written through the kernel's rows only: an element
    # of `trades_` / `pyramid_entries_` (indexed or `.back()`), or a name whose
    # NEAREST preceding declaration in the file binds it to one -- a `Trade&` /
    # `PyramidEntry&` (or a `Trade` / `PyramidEntry` about to be booked), an
    # `auto&` taken from those containers, a range-for over them or over a
    # `std::vector<Trade>&` / `std::vector<PyramidEntry>&` parameter. A name
    # declared again in between (`sized.time` on a native_order::Sized, a
    # pending-order mirror `row`) is not a row.
    row_types = "|".join(ROW_STRUCTS)
    for relative, text in texts.items():
        containers = {"trades_", "pyramid_entries_"} | set(re.findall(
            r"vector\s*<\s*(?:" + row_types + r")\s*>\s*&\s*(\w+)", text))
        box = "(?:this->)?(?:" + "|".join(sorted(map(re.escape, containers))) + ")"
        direct = r"\b" + box + r"\s*(?:\[[^\]\n]*\]|\.\s*back\s*\(\s*\))"
        binding = re.compile(
            r"\b(?:const\s+)?(?:" + row_types + r")\s*&?\s*\w+\s*(?:[=;{(),].*)?$"
            r"|\bauto\s*&\s*\w+\s*=\s*" + box + r"\s*(?:\[|\.\s*back\b)"
            r"|\bfor\s*\(\s*(?:const\s+)?auto\s*&\s*\w+\s*:\s*" + box + r"\s*\)")
        for name in sorted(fields):
            n = re.escape(name)
            write = re.compile(
                r"(?P<obj>" + direct + r"|\b\w+\b)\s*(?:\.|->)\s*" + n + r"\s*(?:"
                + ASSIGN + r"|\+\+|--|(?=(?:\.|->))" + MUTATE + r")")
            for match in write.finditer(text):
                obj = match.group("obj")
                if not re.match(direct, obj):
                    declared = nearest_declaration(text, obj, match.start())
                    if declared is None or not binding.search(declared):
                        continue
                record(name, relative, text, match.start())
    return result


def first_cells(root: Path) -> str:
    """The first cell of every table row of the ADR, one per line."""
    cells: list[str] = []
    for line in read(root, ADR).splitlines():
        row = line.strip()
        if not row.startswith("|") or re.fullmatch(r"\|[\s|:-]*", row):
            continue
        # A cell ends at the first `|` outside a backticked span.
        cell, ticks = [], False
        for char in row[1:]:
            if char == "`":
                ticks = not ticks
            elif char == "|" and not ticks:
                break
            cell.append(char)
        cells.append("".join(cell))
    return "\n".join(cells)


def has_row(rows: str, name: str, *, row_field: bool = False) -> bool:
    """A backticked span of a row's first cell names it; a row-struct field
    only spelled qualified (`Trade::time`, `PyramidEntry::qty`)."""
    if row_field:
        qualified = "|".join(ROW_STRUCTS)
        return re.search(r"`[^`\n]*\b(?:" + qualified + r")::" + re.escape(name)
                         + r"(?![\w])[^`\n]*`", rows) is not None
    return re.search(r"`[^`\n]*(?<![\w])" + re.escape(name) + r"(?![\w])[^`\n]*`", rows) is not None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--list", action="store_true", help="print the whole inventory")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        found = inventory(root)
        rows = first_cells(root)
    except (Unreadable, ValueError) as error:
        print(f"check_kernel_seam_rows: {error}", file=sys.stderr)
        return 2
    missing: list[str] = []
    for name, header in sorted(found.seams.items()):
        ok = has_row(rows, name)
        if args.list:
            print(f"{'row ' if ok else 'NONE'} seam   {name} ({header})")
        if not ok:
            missing.append(f"kernel seam `{name}` ({header}) has no ADR-0001 row naming it in "
                           "its first cell: state who implements it and why it is the kernel's")
    for name, sites in sorted(found.written.items()):
        ok = has_row(rows, name, row_field=not name.endswith("_"))
        if args.list:
            print(f"{'row ' if ok else 'NONE'} member {name} (written at {sites[0]}"
                  + (f" and {len(sites) - 1} more" if len(sites) > 1 else "") + ")")
        if not ok:
            missing.append(f"kernel member `{name}` is written by the source layer "
                           f"({', '.join(sites[:3])}{', ...' if len(sites) > 3 else ''}) and "
                           "has no ADR-0001 row naming it in its first cell: state who writes "
                           "it, whether it is hashed, and its ruling")
    for message in missing:
        print("check_kernel_seam_rows: " + message, file=sys.stderr)
    print(f"kernel seam rows: {len(found.seams)} kernel source_* seams and "
          f"{len(found.written)} kernel members the source layer writes, "
          f"{len(missing)} without a row ... {'FAIL' if missing else 'OK'}")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
