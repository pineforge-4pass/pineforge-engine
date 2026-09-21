#!/usr/bin/env python3
"""Count the DETACHED comment lines in the kernel's own compile closure.

A comment is documentation when a reader can name the declaration it
documents. The one structural test for that in C++ is position: a comment
block that sits immediately above a code line documents that line. So, inside
one gap between two code lines, the LAST comment block is attached and every
earlier block in the same gap is DETACHED -- separated from the next code by a
blank line and another block, it documents nothing the compiler can see.

That is the measure R5 lane E6 reported (its finding 3) and the one this
script makes reproducible. It is deliberately structural: it does not judge
whether a detached block is still TRUE, only that nothing in the file says
what it is about. Reading each block is the lane's work; this is its census.

One class of comment is excluded, because its position is not a claim about
the code under it: a DIRECTIVE block, whose text a tool consumes. Doxygen
grouping commands (@defgroup / @addtogroup / @name and the @{ @} that open and
close a group) bracket a run of declarations, so the closer necessarily
follows the last of them; the @broker-state / @source-state region markers
scripts/check_broker_state_hash_coverage.py reads must sit exactly around the
members they enclose. Moving either would break the tool that reads it.

Scope: the kernel compile closure -- the translation units CMakeLists.txt
lists in PINEFORGE_KERNEL_SOURCES, plus every in-tree header reachable from
them through #include. The source-adapter layer is out of scope: the kernel is
what has to stand alone.

Usage:
    measure_detached_comments.py                 # census, per file
    measure_detached_comments.py --blocks        # every detached block's span
    measure_detached_comments.py --json
    measure_detached_comments.py --self-test     # the classifier's own pins
    measure_detached_comments.py FILE [FILE...]  # an explicit population
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL_SOURCE_BLOCK = re.compile(
    r"set\(PINEFORGE_KERNEL_SOURCES\s*(.*?)\)", re.DOTALL)
INCLUDE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)')

CODE, COMMENT, BLANK = "code", "comment", "blank"

# A comment whose position a tool owns. See the module docstring.
DIRECTIVE = re.compile(
    r"@(?:def|add)group\b|@name\b|@\{|@\}|@(?:broker|source)-state\b")


def classify(lines: list[str]) -> list[str]:
    """One kind per line: code, comment or blank.

    A line that carries any code is CODE even when it also carries a comment;
    a line is COMMENT only when every byte outside whitespace is commentary.
    Quotes and raw strings are tracked so that "//" or "/*" inside a literal
    never opens a comment.
    """
    kinds: list[str] = []
    in_block = False        # inside /* ... */
    raw_terminator = ""     # inside R"delim( ... )delim"
    for line in lines:
        saw_code = False
        saw_comment = in_block or bool(raw_terminator)
        at = 0
        end = len(line)
        while at < end:
            if raw_terminator:
                found = line.find(raw_terminator, at)
                if found < 0:
                    saw_code = True
                    break
                saw_code = True
                at = found + len(raw_terminator)
                raw_terminator = ""
                continue
            if in_block:
                found = line.find("*/", at)
                if found < 0:
                    at = end
                    break
                in_block = False
                at = found + 2
                continue
            char = line[at]
            if char in " \t\r\n":
                at += 1
                continue
            if line.startswith("//", at):
                saw_comment = True
                break
            if line.startswith("/*", at):
                saw_comment = True
                in_block = True
                at += 2
                continue
            raw = re.match(r'(?:u8|u|U|L)?R"([^(]*)\(', line[at:])
            if raw:
                raw_terminator = ")" + raw.group(1) + '"'
                saw_code = True
                at += raw.end()
                continue
            if char in "\"'":
                saw_code = True
                at += 1
                while at < end:
                    if line[at] == "\\":
                        at += 2
                        continue
                    if line[at] == char:
                        at += 1
                        break
                    at += 1
                continue
            saw_code = True
            at += 1
        if saw_code:
            kinds.append(CODE)
        elif saw_comment:
            kinds.append(COMMENT)
        else:
            kinds.append(BLANK)
    return kinds


def detached_blocks(kinds: list[str], lines: list[str] | None = None
                    ) -> list[tuple[int, int]]:
    """1-based [start, end] spans of every detached comment block.

    A gap is the run of non-code lines between two CODE lines. Inside a gap,
    blank lines separate comment blocks; every block but the last is detached.
    The region before the first code line (the file banner) and the region
    after the last one are NOT gaps: they bound no declaration, so this census
    leaves them out exactly as lane E6's did.
    """
    kinds_text = lines if lines is not None else [""] * len(kinds)
    code_at = [index for index, kind in enumerate(kinds) if kind == CODE]
    spans: list[tuple[int, int]] = []
    for left, right in zip(code_at, code_at[1:]):
        blocks: list[tuple[int, int]] = []
        start = None
        for index in range(left + 1, right):
            if kinds[index] == COMMENT:
                if start is None:
                    start = index
            elif start is not None:
                blocks.append((start, index - 1))
                start = None
        if start is not None:
            blocks.append((start, right - 1))
        for block in blocks[:-1]:
            if any(DIRECTIVE.search(kinds_text[index])
                   for index in range(block[0], block[1] + 1)):
                continue
            spans.append((block[0] + 1, block[1] + 1))
    return spans


def kernel_closure(root: Path) -> list[Path]:
    """The kernel TUs CMake names, plus every in-tree header they reach."""
    text = (root / "CMakeLists.txt").read_text()
    match = KERNEL_SOURCE_BLOCK.search(text)
    if not match:
        raise SystemExit("CMakeLists.txt: PINEFORGE_KERNEL_SOURCES not found")
    pending = [root / line.strip() for line in match.group(1).splitlines()
               if line.strip() and not line.strip().startswith("#")]
    for path in pending:
        if not path.is_file():
            raise SystemExit("kernel source is missing: " + str(path))
    seen: list[Path] = []
    queue = list(pending)
    while queue:
        path = queue.pop(0)
        if path in seen:
            continue
        seen.append(path)
        for line in path.read_text(errors="replace").splitlines():
            found = INCLUDE.match(line)
            if not found:
                continue
            name = found.group(1) or found.group(2)
            for candidate in (path.parent / name, root / "include" / name,
                              root / "src" / name):
                resolved = candidate.resolve()
                if resolved.is_file() and ROOT in resolved.parents:
                    queue.append(resolved)
                    break
    return sorted(seen)


def measure(paths: list[Path]) -> list[dict]:
    rows = []
    for path in paths:
        lines = path.read_text(errors="replace").splitlines()
        spans = detached_blocks(classify(lines), lines)
        rows.append({
            "file": str(path.relative_to(ROOT)) if ROOT in path.parents
                    or path.parent == ROOT else str(path),
            "lines": sum(end - start + 1 for start, end in spans),
            "blocks": [{"start": start, "end": end} for start, end in spans],
        })
    rows.sort(key=lambda row: (-row["lines"], row["file"]))
    return rows


SELF_TESTS: list[tuple[str, str, list[tuple[int, int]]]] = [
    ("one block above code is attached",
     "int a;\n// doc\nint b;\n", []),
    ("two blocks in one gap: the first is detached",
     "int a;\n// orphan\n\n// doc\nint b;\n", [(2, 2)]),
    ("three blocks: only the last is attached",
     "int a;\n// one\n\n// two\n\n// doc\nint b;\n", [(2, 2), (4, 4)]),
    ("a trailing comment does not make its line a comment",
     "int a;  // note\n// orphan\n\n// doc\nint b;\n", [(2, 2)]),
    ("the file banner bounds no declaration",
     "// banner\n\n// doc\nint a;\n", []),
    ("the region after the last code line is not a gap",
     "int a;\n// tail\n\n// more tail\n", []),
    ("a block comment spanning lines is one block",
     "int a;\n/* one\n   two */\n\n// doc\nint b;\n", [(2, 3)]),
    ("// inside a string literal is code",
     'int a;\nconst char* u = "http://x";\n// doc\nint b;\n', []),
    ("/* inside a string literal opens nothing",
     'int a;\nconst char* u = "/*";\n// orphan\n\n// doc\nint b;\n', [(3, 3)]),
    ("a raw string hides its delimiters",
     'int a;\nauto r = R"(// not a comment\n/* nor this */)";\n'
     "// orphan\n\n// doc\nint b;\n", [(4, 4)]),
    ("code after a block comment closes on the same line is code",
     "int a;\n/* lead */ int b;\n// doc\nint c;\n", []),
    # If the #include were not code, this gap would run 2..6 and its first
    # block would be detached; because it is code, both blocks are attached.
    ("a preprocessor line is code",
     "int a;\n// orphan\n\n#include <x>\n\n// doc\nint b;\n", []),
    ("a doc comment directly under code, then blank, then doc",
     "int a;\n/// tail of a\n\n/// doc of b\nint b;\n", [(2, 2)]),
    ("adjacent comment styles with no blank between are one block",
     "int a;\n// one\n/// two\nint b;\n", []),
    ("a doxygen group closer is a directive, not a detached comment",
     "int a;\n/** @} */\n\n// doc\nint b;\n", []),
    ("a doxygen group opener is a directive wherever it sits",
     "int a;\n/** @defgroup g Title\n *  @{\n */\n\n// doc\nint b;\n", []),
    ("a broker-state region marker is a directive",
     "int a_;\n// @broker-state end\n\n// doc\nint b;\n", []),
    ("a directive block does not shield a real orphan beside it",
     "int a;\n// orphan\n\n/** @} */\n\n// doc\nint b;\n", [(2, 2)]),
]


def self_test() -> int:
    failures = 0
    for name, text, want in SELF_TESTS:
        got = detached_blocks(classify(text.splitlines()),
                              text.splitlines())
        status = "ok  " if got == want else "FAIL"
        if got != want:
            failures += 1
        print(f"  {status} {name}"
              + ("" if got == want else f"\n       want {want} got {got}"))
    print(f"measure_detached_comments self-test: {len(SELF_TESTS) - failures}"
          f" passed, {failures} failed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--blocks", action="store_true",
                        help="print every detached block's line span")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    paths = [path.resolve() for path in args.paths] or kernel_closure(ROOT)
    rows = measure(paths)
    if args.json:
        print(json.dumps({"files": rows,
                          "total": sum(row["lines"] for row in rows)},
                         indent=2))
        return 0
    total = 0
    for row in rows:
        if not row["lines"]:
            continue
        total += row["lines"]
        print(f"{row['lines']:6d}  {row['file']}")
        if args.blocks:
            for block in row["blocks"]:
                print(f"          :{block['start']}-{block['end']}"
                      f"  ({block['end'] - block['start'] + 1})")
    print(f"{total:6d}  TOTAL detached comment lines"
          f" in {len(paths)} files of the kernel compile closure")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
