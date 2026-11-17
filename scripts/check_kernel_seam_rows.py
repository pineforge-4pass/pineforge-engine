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

The excursion writers
---------------------
A row's two excursion fields, `max_runup` and `max_drawdown`, are held to more
than a row. ADR-0001 rules that the kernel's sampler books every lot's run-up
and drawdown, with two narrow exceptions in the source layer: the re-cut of a
global exit's `Trade` rows, and one margin-call booking that sets one
`PyramidEntry`'s two extremes. Both write a field of the same name, and a
row's first cell matches a field by its name alone, so a third writer would
pass under the rows the first two have. EXCURSION_WRITERS therefore names the
one function per row struct that may write them, and the gate fails on

* a write of either field, in the forms read below, through one of the
  kernel's rows anywhere else in the source layer, the other struct's writer
  included;
* a write of either field through an object it cannot type, because it then
  cannot tell a kernel row from the source layer's own state;
* a named writer that writes neither field: the table and the ADR row would
  name an exception the tree no longer has.

What this check reads. A write is an assignment, a compound assignment or a
suffix `++` or `--` of the field through `.` or `->`, in a `.cpp`, `.hpp`,
`.h` or `.cc` file of the source trees, read from the code alone (comments
and the contents of literals out). The written object is the name before the
`.` or `->`, or the container an element is taken from with `[i]`, `.back()`,
`.front()` or `.at(i)`. It is one of the kernel's rows when that name is

* `trades_` or `pyramid_entries_`, however the container is reached (`pine->`,
  `host.`);
* a `std::vector<Row>&` parameter or reference declared in that file, or a
  source-class member declared with a row type;
* a name whose nearest declaration above it spells a row struct in its type
  (`PyramidEntry&`, `PyramidEntry*`, `PyramidEntry* const`,
  `PyramidEntry const&`), or deduces its type with `auto` on a line that reads
  a row struct or one of those containers (`auto* const lot =
  &pine->pyramid_entries_.front()`, `auto const lot = ...`). A qualifier
  between the type and the name, before or after the `*` or `&`, belongs to the
  declaration and is never its type.

It is the source layer's own when it is any other data member of a source
class (`risk_.max_drawdown` is the adapter's risk limit) or a name declared
with any other explicit type. The function a write sits in is the nearest
enclosing brace whose header reads as a function definition, so a write inside
a lambda belongs to the function the lambda is written in.

What this check does not read. It reads spellings and compiles nothing, so
each of these passes it unseen:

* a prefix `++` or `--` of the field;
* a reference or a pointer taken to the field itself, a library call that
  takes the field by reference included: `std::exchange(lot.max_runup, v)`,
  `std::swap`, `std::tie(...) =`, `memcpy`;
* a whole-row assignment or copy (`lot = other`);
* a pointer-to-member write (`lot.*field = v`) and a structured binding;
* for a `Trade`, a write into the row's serialized image: the `header[13]` and
  `header[14]` words of the re-cut, by which its rows reach the kernel's
  ledger (`scratch_trade_scalars` in src/source/pine_strategy_host.cpp writes
  a row's two extremes from them, `scratch_scalar(header[13], row.max_runup)`,
  a by-reference write this check does not see);
* a macro defined outside the source trees;
* a file of any other suffix: `.inc`, `.ipp`, `.inl`
  (`src/source/exit_payload_codec.inc` is one);
* a row typed through an alias: an explicit type that does not spell `Trade`
  or `PyramidEntry` is read as the source layer's own;
* a declaration it does not parse: an explicit type with its `*` or `&` set
  against the name (`PyramidEntry *lot`; a `std::vector<Row> &lots` is the
  exception, the reader of row-vector references reads it), `auto *lot`, a type
  it cannot spell as one word (a `decltype` type,
  `std::remove_reference_t<...>* const lot`). The name is then typed by an
  older declaration of the same name anywhere above it in the file, because the
  nearest declaration is looked up in the file's text and not in the write's
  scope; it fails as an object the gate cannot type only when there is none;
* a template type with more than one argument: the pattern reads its last
  fragment as the type (`std::array<PyramidEntry, 2> lots` gives `2>`,
  `std::vector<PyramidEntry, A>& lots` gives `A>`), so, unless that fragment
  itself names a row struct, the name is the source layer's own whether or not
  an older declaration stands above it;
* an overload that has a named writer's name in the same file: a writer is
  named by its file and its qualified name.

Nor does it read ADR-0001 for the function names: EXCURSION_WRITERS and the
ADR rows that name its functions are kept in step by hand. And it witnesses
who writes, not what is written or for how long: the booking itself, and that
a lot's own extremes are back when it ends, are
tests/test_margin_call_long_tick_excursion.cpp's to pin. A tree whose rows
declare neither field has nothing to hold.

Exit 0 when every seam and written member has its row and every excursion
write is its named writer's, 1 when one is not, 2 when a file cannot be read.
`--list` prints the inventory.

R5 lane H-DOCGATES (AUDIT4-opus X10).
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys

import check_broker_state_hash_coverage as coverage
import cxxlex

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

#: A row's excursion fields, and the one source-layer function per row struct
#: that may write them: the two narrow exceptions ADR-0001 rules (the module
#: docstring, "The excursion writers"). An entry and the ADR-0001 row that
#: names its function change together, by hand: nothing here reads the ADR for
#: these names.
EXCURSION_FIELDS = ("max_runup", "max_drawdown")
EXCURSION_WRITERS = {
    "PyramidEntry": ("src/source/pine_adapter.cpp",
                     "PineExecutionAdapter::submit_long_tick_margin_units"),
    "Trade": ("src/source/pine_strategy_host.cpp",
              "source::PineStrategyHost::order_global_exit_children"),
}
#: The kernel's own containers of its rows, by the row struct each holds.
ROW_CONTAINERS = {"trades_": "Trade", "pyramid_entries_": "PyramidEntry"}
#: What `written_row` answers for an object that is the source layer's own.
NOT_A_ROW = ""
#: The object a field is written through, read backwards from the `.` or `->`:
#: a name, or an element of a named container.
WRITTEN_OBJECT = re.compile(
    r"\b(?P<obj>\w+)\s*"
    r"(?:\[[^\]\n]*\]|\.\s*(?:back|front)\s*\(\s*\)|\.\s*at\s*\([^()\n]*\))?\s*$")
#: A brace header that is a function definition: `name(parameters)`, then
#: nothing but qualifiers. A lambda has no name before its parameters, and a
#: call a lambda is an argument of has not closed its own parenthesis yet.
FUNCTION_HEAD = re.compile(
    r"([A-Za-z_~]\w*(?:\s*::\s*[A-Za-z_~]\w*)*)\s*\((?:[^()]|\([^()]*\))*\)\s*"
    r"(?:(?:const|override|final|noexcept(?:\s*\([^()]*\))?)\s*)*"
    r"(?:->[\w\s:<>*&,]+)?$")
#: Words a parenthesis follows in a brace header that is no function's:
#: `if (...)`, `if constexpr (...)`, `catch (...)`.
CONTROL_WORDS = frozenset(("if", "constexpr", "for", "while", "switch", "catch", "return",
                           "sizeof", "alignof", "alignas", "decltype", "noexcept", "requires",
                           "throw", "static_assert", "defined", "__attribute__"))
#: Words that can stand where a declaration's type would without declaring
#: anything: `return lot;`, `else lot = ...`.
STATEMENT_WORDS = frozenset(("return", "else", "case", "throw", "delete", "new", "goto",
                             "co_return", "co_yield", "typename", "using", "do"))
#: Qualifiers a declaration takes between its type and its name: after the type
#: word (`auto const lot`) or between the declarator's `*` or `&` and the name
#: (`auto* const lot`). One of them is never a declaration's type: the name it
#: stands before is typed by the declaration it qualifies, and where that type
#: cannot be read as one word (`std::remove_reference_t<...>* const lot`) the
#: name has no declaration here, so a write through it is a name the gate
#: cannot type, unless an older declaration of the same name stands above it in
#: the file (the module docstring says so).
QUALIFIER_WORDS = frozenset(("const", "volatile"))
BLOCK_MARK = re.compile(r"[{};]")


class Unreadable(Exception):
    """A file the gate must read is missing; exit status 2."""


@dataclass
class Inventory:
    seams: dict[str, str] = field(default_factory=dict)          # name -> declaring header
    written: dict[str, list[str]] = field(default_factory=dict)  # name -> write sites


@dataclass
class ExcursionWrite:
    struct: str | None      # the row struct written; None when the gate cannot tell
    name: str               # the excursion field
    relative: str
    line: int
    function: str | None    # the function definition the write sits in, if it reads as one

    @property
    def site(self) -> str:
        return f"{self.relative}:{self.line}"


@dataclass
class Excursions:
    guarded: list[str] = field(default_factory=list)            # row structs with such a field
    writes: list[ExcursionWrite] = field(default_factory=list)  # through the kernel's rows


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


def code_only(text: str) -> str:
    """The code of a C/C++ text: comments and the contents of string and
    character literals out, every newline kept, so that a brace, a semicolon or
    a field name inside either is never read as code."""
    return "\n".join(part[0] for part in cxxlex.lex_lines(text))


def enclosing_function(code: str, offset: int) -> str | None:
    """The function definition the code at `offset` sits in, by its name as
    the definition spells it: the nearest enclosing brace whose header reads
    as one (FUNCTION_HEAD). None outside a function, and for a header this
    does not read; the gate then fails closed, because no name is a named
    writer's."""
    headers: list[str] = []
    boundary = 0
    for mark in BLOCK_MARK.finditer(code, 0, offset):
        if mark.group() == "{":
            headers.append(code[boundary:mark.start()])
        elif mark.group() == "}" and headers:
            headers.pop()
        boundary = mark.end()
    for header in reversed(headers):
        head = FUNCTION_HEAD.search(" ".join(header.split()))
        if head is None:
            continue
        name = re.sub(r"\s*::\s*", "::", head.group(1))
        if name.rsplit("::", 1)[-1] not in CONTROL_WORDS:
            return name
    return None


def declaration_of(code: str, name: str, before: int) -> tuple[str | None, str] | None:
    """`name`'s nearest declaration before offset `before`: its type word
    (None when the type is deduced with `auto`) and the declaration's line.
    The reading of nearest_declaration, except that a statement keyword
    standing where a type would declares nothing (`return lot;`), that a
    `&&` with a space before it is an operator (`ready && lot`), and that a
    qualifier after the type word (`auto const lot`) or after the declarator's
    `*` or `&` (`auto* const lot`, `PyramidEntry* const lot`) belongs to the
    declaration and is never its type."""
    pattern = re.compile(
        r"(?:\b(?P<type>[\w:<>]+)(?:\s+(?:const|volatile)\b)*"
        r"(?:\s*(?:&|\*+)(?:\s*(?:const|volatile)\b)*|&&)?\s+"
        r"|\(\s*(?:const\s+)?auto\s*&{0,2}\s*|\bauto\s*&{0,2}\s*)"
        + re.escape(name) + r"\s*(?=[=:;{(),])")
    declared = None
    for found in pattern.finditer(code, 0, before):
        if found.group("type") not in STATEMENT_WORDS | QUALIFIER_WORDS:
            declared = found
    if declared is None:
        return None
    start = code.rfind("\n", 0, declared.start()) + 1
    end = code.find("\n", declared.end())
    kind = declared.group("type")
    return (None if kind in (None, "auto") else kind,
            code[start:end if end >= 0 else len(code)])


def row_named(text: str, vectors: dict[str, str]) -> str | None:
    """The row struct a type or a declaration's line names: by the struct
    itself, or by the container of rows it reads."""
    for struct in ROW_STRUCTS:
        if re.search(r"\b" + struct + r"\b", text):
            return struct
    for container, struct in (*ROW_CONTAINERS.items(), *vectors.items()):
        if re.search(r"\b" + re.escape(container) + r"\b", text):
            return struct
    return None


def written_row(code: str, start: int, own: set[str], own_rows: dict[str, str],
                vectors: dict[str, str]) -> str | None:
    """What the object written at `start` (the `.` or `->` before the field)
    is: a row struct's name, NOT_A_ROW for an object of the source layer's
    own, None when this cannot tell."""
    tail = WRITTEN_OBJECT.search(code, max(0, start - 240), start)
    if tail is None:
        return None
    obj = tail.group("obj")
    for known in (ROW_CONTAINERS, vectors, own_rows):
        if obj in known:
            return known[obj]
    if obj in own:
        return NOT_A_ROW
    declared = declaration_of(code, obj, start)
    if declared is None:
        return None
    kind, line = declared
    if kind is not None:
        return row_named(kind, {}) or NOT_A_ROW
    return row_named(line, vectors)


def excursion_writes(root: Path) -> Excursions:
    """Every source-layer write of a row's excursion field through one of the
    kernel's rows, with the row struct it writes and the function it sits in
    (the module docstring, "The excursion writers")."""
    engine = read(root, ENGINE)
    result = Excursions()
    names: set[str] = set()
    for struct in ROW_STRUCTS:
        declared = set(EXCURSION_FIELDS) & data_fields(
            class_body(engine, "struct", struct), trailing_underscore=False)
        if declared:
            result.guarded.append(struct)
            names |= declared
    if not names:
        return result
    own = source_class_members(root)
    row_types = "|".join(ROW_STRUCTS)
    own_rows = {held.group(2): held.group(1)
                for relative in coverage.SOURCE_CLASSES
                for held in re.finditer(
                    r"\b(" + row_types + r")\b[^;(){}=]*?\b(\w+_)\s*[;={]",
                    code_only(read(root, relative)))}
    write = re.compile(r"(?:\.|->)\s*(?P<name>" + "|".join(sorted(names)) + r")\b\s*(?:"
                       + ASSIGN + r"|\+\+|--)")
    for path in source_files(root):
        raw = path.read_text(encoding="utf-8", errors="replace")
        if not any(name in raw for name in names):
            continue
        relative = path.relative_to(root).as_posix()
        code = code_only(raw)
        vectors = {name: struct for struct, name in re.findall(
            r"vector\s*<\s*(" + row_types + r")\s*>\s*&\s*(\w+)", code)}
        for match in write.finditer(code):
            struct = written_row(code, match.start(), own, own_rows, vectors)
            if struct == NOT_A_ROW:
                continue
            result.writes.append(ExcursionWrite(
                struct, match.group("name"), relative,
                code.count("\n", 0, match.start()) + 1,
                enclosing_function(code, match.start())))
    return result


def excursion_findings(excursions: Excursions) -> list[str]:
    """One message per excursion write that is not its row struct's named
    writer's, per write the gate cannot type, and per named writer of a
    guarded struct that writes nothing."""
    findings: list[str] = []
    writing: set[str] = set()
    for write in excursions.writes:
        place = f"`{write.function}`" if write.function else "no function the gate can name"
        if write.struct is None:
            findings.append(
                f"`{write.name}` is written at {write.site}, in {place}, through an object the "
                "gate cannot type: spell the object's type, so a kernel row can be told from the "
                "source layer's own state")
            continue
        relative, function = EXCURSION_WRITERS[write.struct]
        if (write.relative, write.function) == (relative, function):
            writing.add(write.struct)
            continue
        findings.append(
            f"a row's excursion `{write.struct}::{write.name}` is written at {write.site}, in "
            f"{place}: ADR-0001 names one source-layer writer of a `{write.struct}`'s run-up and "
            f"drawdown, `{function}` ({relative}). Book the excursion there, or rule the new "
            "writer in ADR-0001 and name it in EXCURSION_WRITERS")
    for struct in excursions.guarded:
        relative, function = EXCURSION_WRITERS[struct]
        if struct not in writing:
            findings.append(
                f"EXCURSION_WRITERS names `{function}` ({relative}) as the writer of a "
                f"`{struct}`'s excursion, and the gate sees it write neither field: remove the "
                "entry and its ADR-0001 row, or restore the write")
    return findings


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
        excursions = excursion_writes(root)
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
    stray = excursion_findings(excursions)
    if args.list:
        for write in excursions.writes:
            print(f"     excursion {write.struct or '?'}::{write.name} (written at {write.site}, "
                  f"in {write.function or '?'})")
    for message in missing + stray:
        print("check_kernel_seam_rows: " + message, file=sys.stderr)
    print(f"kernel seam rows: {len(found.seams)} kernel source_* seams and "
          f"{len(found.written)} kernel members the source layer writes, "
          f"{len(missing)} without a row ... {'FAIL' if missing else 'OK'}")
    if excursions.guarded:
        print(f"row excursions: {len(excursions.writes)} source-layer writes of a row's run-up "
              f"or drawdown, {len(stray)} findings against the writers ADR-0001 names ... "
              f"{'FAIL' if stray else 'OK'}")
    return 1 if missing or stray else 0


if __name__ == "__main__":
    raise SystemExit(main())
