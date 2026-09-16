#!/usr/bin/env python3
"""Check A29 CHECK-parity between removed base tests and L4 native twins.

The base sources are deliberately read from the immutable ``ab9714be`` tree.
For every inventory row, a current ``tests/<name>_l4*.cpp`` twin must retain
each CHECK-family invocation unless the exact base literal has an Appendix 5
row in the R4-D deletion ledger.  A ledger entry is deliberately narrow: it
names one base source line, its normalized CHECK text, why that read is no
longer observable without reviving the deleted owner, and the twin row that
asserts the public behaviour instead.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Callable, Iterable


ROOT = Path(__file__).resolve().parents[1]
BASE = "ab9714be"
DEFAULT_EV = Path("/Users/haoliangwen/.pineforge/parity/native-engine-refactor-roadmap-20260912")
APPENDIX_HEADING = "## Appendix 5 — CHECK-parity unobservable literal ledger"
TABLE_HEADING = "| base file:line | CHECK text | reason unobservable | covering twin row |"
CHECK_NAME = re.compile(r"\b(CHECK(?:_[A-Za-z0-9_]+)?)\s*\(")
DIRECTIVE = re.compile(r"^\s*#\s*define\b")
TABLE_ROW = re.compile(r"^\|(?P<body>.*)\|\s*$")
COVERING_ROW = re.compile(
    r"(?P<path>tests/test_[A-Za-z0-9_]+_l4[A-Za-z0-9_]*\.cpp):(?P<line>\d+)(?:\s|$)")

# A24 keeps these behavioural tests registered unchanged in the ordinary
# switched-route inventory. They are intentionally outside the removed-twin
# population; every other JSON entry is an A29 parity obligation.
A24_NAMES = frozenset({
    "test_chart_tf_security_split_feed",
    "test_get_input_source",
    "test_htf_chart_close_completion",
    "test_htf_weekly_lookahead",
    "test_live_abort",
    "test_ltf_buffer_no_leak",
    "test_ltf_lookahead_first_bucket",
    "test_market_admission_decisions",
    "test_oanda_lazy_close",
    "test_security_lower_tf_input_passthrough",
    "test_security_lower_tf_script_bound",
    "test_security_range_start_bucket_gating",
    "test_security_range_start_na_warmup",
    "test_security_tf_validation",
    "test_security_validation_throws",
    "test_split_feed_partial_bucket",
    "test_syminfo_metadata",
    "test_timeframe",
})


class ParityError(ValueError):
    """Inventory, twin, or Appendix 5 evidence is malformed."""


@dataclass(frozen=True)
class CheckLiteral:
    path: str
    line: int
    text: str

    @property
    def location(self) -> str:
        return self.path + ":" + str(self.line)


@dataclass(frozen=True)
class LedgerLiteral:
    location: str
    text: str
    reason: str
    covering: str


@dataclass(frozen=True)
class RangeLedgerLiteral:
    path: str
    first_line: int
    last_line: int
    count: int
    group: str
    reason: str
    covering: str

    @property
    def location(self) -> str:
        return f"{self.path}:{self.first_line}-{self.last_line}"


def normalize(value: str) -> str:
    """Canonical form used both for extracted and ledgered CHECK text."""
    return re.sub(r"\s+", " ", value.replace("\\|", "|").strip())


def _is_digit_separator(text: str, index: int) -> bool:
    """Whether a C++ apostrophe is a numeric separator rather than a quote."""
    if text[index] != "'" or index == 0 or index + 1 >= len(text):
        return False
    # C++14 digit separators occur inside decimal, hexadecimal, binary, and
    # digit-suffixed literals (for example ``60'000LL``).  Treating one as a
    # character delimiter makes the scanner consume the rest of the TU and
    # miss every subsequent CHECK.  A real character literal cannot have a
    # digit/hex character on both sides of its opening quote.
    return text[index - 1] in "0123456789abcdefABCDEF" and text[index + 1] in "0123456789abcdefABCDEF"


def _mask_comments(text: str) -> str:
    """Preserve source offsets while masking comments and quoted contents.

    The original text is used to retain the exact CHECK expression.  This
    scanner only supplies safe locations: strings/comments must not contribute
    a spurious ``CHECK(`` token, while their quote positions remain so the
    balanced-call scanner can still skip parentheses within string arguments.
    """
    result: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                state = "line"
                result.extend("  ")
                index += 2
                continue
            if char == "/" and next_char == "*":
                state = "block"
                result.extend("  ")
                index += 2
                continue
            if char in ('\"', "'") and not _is_digit_separator(text, index):
                quote = char
                state = "string"
            result.append(char)
        elif state == "line":
            result.append("\n" if char == "\n" else " ")
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and next_char == "/":
                result.extend("  ")
                index += 2
                state = "code"
                continue
            result.append("\n" if char == "\n" else " ")
        else:  # string / character literal
            # Keep only the delimiters and escape markers.  The spaces retain
            # offsets while ensuring a message such as "CHECK(foo)" cannot be
            # mistaken for a test assertion.
            result.append(char if char in (quote, "\\") else ("\n" if char == "\n" else " "))
            if char == "\\" and index + 1 < len(text):
                result.append("\n" if text[index + 1] == "\n" else " ")
                index += 2
                continue
            if char == quote:
                state = "code"
        index += 1
    return "".join(result)


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _balanced_call(text: str, start: int) -> int:
    """Return the exclusive end of one macro invocation beginning at ``start``."""
    open_at = text.find("(", start)
    if open_at < 0:
        raise ParityError("CHECK-family macro has no opening parenthesis")
    depth = 0
    state = "code"
    quote = ""
    at = open_at
    while at < len(text):
        char = text[at]
        if state == "code":
            if char in ('\"', "'") and not _is_digit_separator(text, at):
                state = "string"
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    return at + 1
        else:
            if char == "\\":
                at += 2
                continue
            if char == quote:
                state = "code"
        at += 1
    raise ParityError("unbalanced CHECK-family invocation")


def extract_checks(text: str, path: str, *, include_definitions: bool = False) -> list[CheckLiteral]:
    """Extract CHECK/CHECK_* occurrences in source order.

    A29's original census counted each test-local ``#define CHECK(...)`` as a
    CHECK-family occurrence (the L4d population is 3,369 rather than 3,299).
    Callers use ``include_definitions=True`` for that mechanical census, but
    retain the default here for the Appendix 5 literal matcher: a macro
    definition is never an unobservable behavioural literal.
    """
    masked = _mask_comments(text)
    found: list[CheckLiteral] = []
    for match in CHECK_NAME.finditer(masked):
        line_start = masked.rfind("\n", 0, match.start()) + 1
        if not include_definitions and DIRECTIVE.match(masked[line_start:match.start()]):
            continue
        end = _balanced_call(masked, match.start())
        found.append(CheckLiteral(path, _line_of(masked, match.start()),
                                  normalize(text[match.start():end])))
    return found


def split_markdown_cells(body: str) -> list[str]:
    cells: list[str] = []
    value: list[str] = []
    escaped = False
    for char in body:
        if escaped:
            value.append(char)
            escaped = False
        elif char == "\\":
            escaped = True
            value.append(char)
        elif char == "|":
            cells.append("".join(value).strip())
            value = []
        else:
            value.append(char)
    cells.append("".join(value).strip())
    return cells


def read_appendix(ledger: Path) -> tuple[
        dict[tuple[str, str], LedgerLiteral],
        dict[tuple[str, int, int], RangeLedgerLiteral]]:
    text = ledger.read_text()
    start = text.find(APPENDIX_HEADING)
    if start < 0:
        raise ParityError("deletion ledger lacks Appendix 5")
    section = text[start + len(APPENDIX_HEADING):]
    next_heading = re.search(r"^##\s+", section, re.M)
    if next_heading:
        section = section[:next_heading.start()]
    if TABLE_HEADING not in section:
        raise ParityError("Appendix 5 lacks the required column heading")
    rows: dict[tuple[str, str], LedgerLiteral] = {}
    ranges: dict[tuple[str, int, int], RangeLedgerLiteral] = {}
    for raw in section.splitlines():
        match = TABLE_ROW.match(raw.strip())
        if not match:
            continue
        cells = split_markdown_cells(match.group("body"))
        if cells[0].lower() in ("base file:line", "base file:range") \
                or cells[0].startswith("---"):
            continue
        if len(cells) == 5:
            location, count_text, group, reason, covering = cells
            location_match = re.fullmatch(
                r"(?P<path>tests/test_[A-Za-z0-9_]+\.cpp):"
                r"(?P<first>\d+)-(?P<last>\d+)", location)
            count_match = re.fullmatch(r"(?P<count>\d+) CHECKs?", count_text)
            if not location_match or not count_match:
                raise ParityError("Appendix 5 has invalid range row: " + raw)
            first = int(location_match.group("first"))
            last = int(location_match.group("last"))
            count = int(count_match.group("count"))
            if first <= 0 or last < first or count <= 0 \
                    or not group or not reason or not covering:
                raise ParityError("Appendix 5 range row is incomplete: " + raw)
            key = (location_match.group("path"), first, last)
            if key in ranges:
                raise ParityError("Appendix 5 duplicates range: " + location)
            ranges[key] = RangeLedgerLiteral(
                key[0], first, last, count, group, reason, covering)
            continue
        if len(cells) != 4:
            continue
        location, check, reason, covering = cells
        if not re.fullmatch(r"tests/test_[A-Za-z0-9_]+\.cpp:\d+", location):
            raise ParityError("Appendix 5 has invalid base location: " + location)
        if not check or not reason or not covering:
            raise ParityError("Appendix 5 row is incomplete: " + raw)
        key = (location, normalize(check))
        if key in rows:
            raise ParityError("Appendix 5 duplicates literal: " + location)
        rows[key] = LedgerLiteral(location, key[1], reason, covering)
    return rows, ranges


def inventory_names(inventory: Path, *, families: Iterable[str] | None = None) -> list[str]:
    data = json.loads(inventory.read_text())
    if families is None:
        names = data.get("removed")
    else:
        declared = data.get("families")
        if not isinstance(declared, dict):
            raise ParityError("removed-test inventory has no family map")
        names = []
        for family in families:
            members = declared.get(family)
            if not isinstance(members, list) or not all(isinstance(name, str) for name in members):
                raise ParityError("removed-test inventory has no string family: " + family)
            names.extend(members)
    if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
        raise ParityError("removed-test inventory has no string removed list")
    if len(names) != len(set(names)):
        raise ParityError("removed-test inventory has duplicate names")
    return [name for name in names if name not in A24_NAMES]


def git_base_source(name: str, *, git: str = "git", base: str = BASE) -> str:
    result = subprocess.run([git, "show", f"{base}:tests/{name}.cpp"], text=True,
                            capture_output=True, timeout=60)
    if result.returncode:
        raise ParityError("cannot read base test tests/" + name + ".cpp: " + result.stderr.strip())
    return result.stdout


def find_twin(tests: Path, name: str) -> Path:
    matches = sorted(tests.glob(name + "_l4*.cpp"))
    if len(matches) != 1:
        if not matches:
            raise ParityError("missing A29 twin: tests/" + name + "_l4*.cpp")
        raise ParityError("ambiguous A29 twins for " + name + ": "
                          + ", ".join(path.name for path in matches))
    return matches[0]


def validate_covering_row(root: Path, row: LedgerLiteral | RangeLedgerLiteral) -> None:
    """Require Appendix 5 to name a real CHECK-family row in a real twin."""
    matches = list(COVERING_ROW.finditer(row.covering))
    if not matches:
        raise ParityError("Appendix 5 covering twin row is invalid: " + row.covering)
    for match in matches:
        path = root / match.group("path")
        if not path.is_file():
            raise ParityError("Appendix 5 covering twin is missing: " + match.group("path"))
        line = int(match.group("line"))
        calls = extract_checks(path.read_text(), match.group("path"), include_definitions=True)
        if not any(call.line == line for call in calls):
            raise ParityError(
                "Appendix 5 covering line has no CHECK-family macro: "
                + match.group(0).strip())


def check_inventory(*, root: Path = ROOT, ev: Path = DEFAULT_EV,
                    base_reader: Callable[[str], str] | None = None,
                    names: Iterable[str] | None = None,
                    families: Iterable[str] | None = None) -> dict[str, int]:
    """Return parity counts or raise ``ParityError`` on the first mismatch."""
    inventory = ev / "tasks/r4-d/REMOVED-TESTS-ab9714be-d1a0862.json"
    ledger = ev / "tasks/r4-d/DELETION-LEDGER.md"
    if names is not None and families is not None:
        raise ParityError("choose names or families, not both")
    selected = (list(names) if names is not None
                else inventory_names(inventory, families=families))
    appendix, range_appendix = read_appendix(ledger)
    reader = base_reader or git_base_source
    total_base = total_twin = total_ledgered = 0
    used_ledger: set[tuple[str, str]] = set()
    used_ranges: set[tuple[str, int, int]] = set()
    for name in selected:
        base_path = "tests/" + name + ".cpp"
        base_source = reader(name)
        base_checks = extract_checks(base_source, base_path, include_definitions=True)
        base_literals = extract_checks(base_source, base_path)
        twin = find_twin(root / "tests", name)
        twin_checks = extract_checks(twin.read_text(), "tests/" + twin.name,
                                     include_definitions=True)
        base_keys = {(item.location, item.text) for item in base_literals}
        relevant = {key: row for key, row in appendix.items()
                    if key[0].startswith(base_path + ":")}
        relevant_ranges = {key: row for key, row in range_appendix.items()
                           if key[0] == base_path}
        for key, row in relevant.items():
            if key not in base_keys:
                raise ParityError("Appendix 5 literal does not match base CHECK: "
                                  + row.location + " " + row.text)
            validate_covering_row(root, row)
        ordered_ranges = sorted(relevant_ranges.values(),
                                key=lambda row: (row.first_line, row.last_line))
        for index, row in enumerate(ordered_ranges):
            if index and row.first_line <= ordered_ranges[index - 1].last_line:
                raise ParityError("Appendix 5 ranges overlap: " + row.location)
            available = sum(row.first_line <= check.line <= row.last_line
                            for check in base_checks)
            if row.count > available:
                raise ParityError(
                    f"Appendix 5 range count exceeds CHECKs in {row.location}: "
                    f"declared={row.count} available={available}")
            if any(row.first_line <= literal.line <= row.last_line
                   for literal in base_literals
                   if (literal.location, literal.text) in relevant):
                raise ParityError(
                    "Appendix 5 exact literal overlaps range: " + row.location)
            validate_covering_row(root, row)
        # A native twin may rewrite an owner-private read to a public
        # projection. The mechanical gate therefore checks the required count,
        # while Appendix 5 supplies exact-text evidence for any omitted row.
        base_texts = [item.text for item in base_checks]
        twin_texts = [item.text for item in twin_checks]
        ledger_texts = [row.text for row in relevant.values()]
        range_count = sum(row.count for row in relevant_ranges.values())
        for text in ledger_texts:
            if text not in base_texts:
                raise ParityError("Appendix 5 CHECK text is absent from base: " + base_path)
        if len(twin_texts) + len(ledger_texts) + range_count != len(base_texts):
            raise ParityError(
                f"CHECK parity mismatch for {name}: base={len(base_texts)} "
                f"twin={len(twin_texts)} "
                f"ledgered={len(ledger_texts) + range_count}")
        used_ledger.update(relevant)
        used_ranges.update(relevant_ranges)
        total_base += len(base_checks)
        total_twin += len(twin_checks)
        total_ledgered += len(relevant) + range_count
    # ``--name`` is a targeted development aid.  It must validate every row
    # for its selected test without rejecting Appendix 5 evidence belonging to
    # another selected-at-CI twin.  A full/default inventory still rejects any
    # row outside its A29 population.
    selected_paths = {"tests/" + name + ".cpp" for name in selected}
    scoped_ledger = {key for key in appendix if key[0].rsplit(":", 1)[0] in selected_paths}
    unused = scoped_ledger - used_ledger
    if unused:
        first = next(iter(sorted(unused)))
        raise ParityError("Appendix 5 contains a literal outside the checked inventory: " + first[0])
    scoped_ranges = {key for key in range_appendix if key[0] in selected_paths}
    unused_ranges = scoped_ranges - used_ranges
    if unused_ranges:
        first = next(iter(sorted(unused_ranges)))
        raise ParityError(
            "Appendix 5 contains a range outside the checked inventory: "
            + f"{first[0]}:{first[1]}-{first[2]}")
    return {"tests": len(selected), "base": total_base, "twin": total_twin,
            "ledgered": total_ledgered}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ev", type=Path,
                        default=Path(os.environ.get("PINEFORGE_R4D_EV", DEFAULT_EV)))
    parser.add_argument("--name", action="append", default=[],
                        help="check one inventory name (test mutation helper only)")
    parser.add_argument("--family", action="append", default=[],
                        help="check one named inventory family (landing-local CI scope)")
    args = parser.parse_args(argv)
    try:
        result = check_inventory(ev=args.ev, names=args.name or None,
                                 families=args.family or None)
    except (OSError, json.JSONDecodeError, ParityError) as error:
        print("check_twin_parity: " + str(error), file=sys.stderr)
        return 1
    print("check_twin_parity: {tests} tests, {base} base CHECKs, "
          "{twin} twin CHECKs, {ledgered} ledgered unobservable literals, OK".format(**result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
