#!/usr/bin/env python3
"""Check literal parity between ab9714be tests and switched-route twins.

All inputs needed by CI are committed in ``tests/``. The immutable-base
manifest records every CHECK/REQUIRE/EXPECT/assert invocation (definitions are
not assertions), the inventory identifies the required twins, and the ledger
records the narrowly unobservable owner-private rows. No ambient Git history
or campaign checkout is consulted by this checker.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Callable, Iterable


ROOT = Path(__file__).resolve().parents[1]
BASE = "ab9714beccb62b796c122cf68986ec9e7dbf4a67"
BASE_MANIFEST = ROOT / "tests/twin_parity_base.json"
INVENTORY = ROOT / "tests/twin_parity_inventory.json"
LEDGER = ROOT / "tests/twin_parity_ledger.md"

# Patched to the generated fixture bytes by the landing. Keeping these pins in
# executable code means a ledger/inventory rewrite cannot silently redefine
# what the guard proves.
BASE_MANIFEST_SHA256 = "594d23c87a6581dd8ea4a6ca57c8b3d2402034232d873355f839027564b6914f"
INVENTORY_SHA256 = "6af31bb5ead28d0f9be61a7df8c15b86998306ae8e69d0d64d32d9cc9601a9ef"
LEDGER_SHA256 = "c568999da8777ff967cdb3fcf007f382f88902bf78f9e02c2c5d7d15de22e9ba"

APPENDIX_HEADING = "## Appendix 5 — CHECK-parity unobservable literal ledger"
TABLE_HEADING = "| base file:line | CHECK text | reason unobservable | covering twin row |"
RANGE_TABLE_HEADING = (
    "| base file:range | CHECK count | helper/group | reason unobservable | "
    "covering twin rows |")
ASSERTION_NAME = re.compile(
    r"\b((?:(?:CHECK|REQUIRE|EXPECT)(?:_[A-Za-z0-9_]+)?)|assert)\s*\(")
DIRECTIVE = re.compile(r"^\s*#\s*define\b")
TABLE_ROW = re.compile(r"^\|(?P<body>.*)\|\s*$")
COVERING_ROW = re.compile(
    r"(?P<path>tests/test_[A-Za-z0-9_]+_l[4-9][A-Za-z0-9_]*\.cpp):"
    r"(?P<line>\d+)(?:\s|$)")

A24_NAMES = frozenset({
    "test_chart_tf_security_split_feed", "test_get_input_source",
    "test_htf_chart_close_completion", "test_htf_weekly_lookahead",
    "test_live_abort", "test_ltf_buffer_no_leak",
    "test_ltf_lookahead_first_bucket", "test_market_admission_decisions",
    "test_oanda_lazy_close", "test_security_lower_tf_input_passthrough",
    "test_security_lower_tf_script_bound",
    "test_security_range_start_bucket_gating",
    "test_security_range_start_na_warmup", "test_security_tf_validation",
    "test_security_validation_throws", "test_split_feed_partial_bucket",
    "test_syminfo_metadata", "test_timeframe",
})

# DELTA P1-9: these files used to share one one-CHECK body. Each replacement
# must carry a meaningful assertion set and a distinct implementation body.
RESTORED_CLONE_TWINS = frozenset({
    "test_close_id_retires_ledger", "test_frozen_market_instruction",
    "test_market_admission_matrix", "test_market_admission_state",
    "test_pending_order_core", "test_pending_placement_receipts",
    "test_pending_quantity_intent", "test_pine_transaction_settlement",
    "test_placement_facts", "test_settlement_observation_boundary",
    "test_stop_decline_continue_path", "test_taro_mc_close_residue",
})
MIN_RESTORED_ASSERTIONS = 5


class ParityError(ValueError):
    """Committed parity evidence is absent or inconsistent."""


@dataclass(frozen=True)
class AssertionLiteral:
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


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_fixture(path: Path, expected: str, label: str) -> None:
    if not path.is_file():
        raise ParityError(label + " is missing: " + str(path))
    actual = sha256(path)
    if expected == "TO_BE_PINNED" or actual != expected:
        raise ParityError(f"{label} digest changed: expected {expected}, got {actual}")


def normalize(value: str) -> str:
    return re.sub(r"\s+", " ", value.replace("\\|", "|").strip())


_LITERAL_TOKEN = re.compile(
    r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|'''
    r'''\b(?:true|false|nullptr)\b|'''
    r'''\b(?:[A-Za-z_]\w*::)+[A-Za-z_]\w*\b|'''
    r'''(?<![A-Za-z_])(?:0[xX][0-9A-Fa-f']+|0[bB][01']+|'''
    r'''(?:\d[\d']*(?:\.\d[\d']*)?|\.\d[\d']+)(?:[eE][+-]?\d+)?)'''
    r'''(?:[uUlLfF]+)?''')


def literal_signature(value: str) -> tuple[str, ...]:
    """Return assertion kind plus source literals, independent of facade names.

    A29 permits owner-private reads to be rewritten to public projections.  A
    text-only comparison would reject those legitimate rewrites, while a count
    accepts ``CHECK(true)``.  This signature pins the macro kind and every
    string/numeric/bool/scoped-enum literal in source order; literal-free rows
    still require an exact expression match.
    """
    name = ASSERTION_NAME.search(value)
    kind = name.group(1) if name else ""
    tokens = [kind]
    for match in _LITERAL_TOKEN.finditer(value):
        token = match.group(0)
        if re.fullmatch(r"(?:0[xX][0-9A-Fa-f']+|0[bB][01']+|(?:\d[\d']*"
                        r"(?:\.\d[\d']*)?|\.\d[\d']+)(?:[eE][+-]?\d+)?)"
                        r"(?:[uUlLfF]+)?", token):
            token = re.sub(r"[uUlLfF]+$", "", token.replace("'", ""))
        tokens.append(token)
    return tuple(tokens)


def _is_digit_separator(text: str, index: int) -> bool:
    if text[index] != "'" or index == 0 or index + 1 >= len(text):
        return False
    return (text[index - 1] in "0123456789abcdefABCDEF"
            and text[index + 1] in "0123456789abcdefABCDEF")


def _mask_comments(text: str) -> str:
    result: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and following == "/":
                state = "line"; result.extend("  "); index += 2; continue
            if char == "/" and following == "*":
                state = "block"; result.extend("  "); index += 2; continue
            if char in ('"', "'") and not _is_digit_separator(text, index):
                state = "string"; quote = char
            result.append(char)
        elif state == "line":
            result.append("\n" if char == "\n" else " ")
            if char == "\n": state = "code"
        elif state == "block":
            if char == "*" and following == "/":
                result.extend("  "); index += 2; state = "code"; continue
            result.append("\n" if char == "\n" else " ")
        else:
            result.append(char if char in (quote, "\\") else
                          ("\n" if char == "\n" else " "))
            if char == "\\" and index + 1 < len(text):
                result.append("\n" if text[index + 1] == "\n" else " ")
                index += 2; continue
            if char == quote: state = "code"
        index += 1
    return "".join(result)


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _balanced_call(text: str, start: int) -> int:
    open_at = text.find("(", start)
    if open_at < 0:
        raise ParityError("assertion macro has no opening parenthesis")
    depth = 0
    state = "code"
    quote = ""
    at = open_at
    while at < len(text):
        char = text[at]
        if state == "code":
            if char in ('"', "'") and not _is_digit_separator(text, at):
                state = "string"; quote = char
            elif char == "(": depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0: return at + 1
        else:
            if char == "\\": at += 2; continue
            if char == quote: state = "code"
        at += 1
    raise ParityError("unbalanced assertion invocation")


def extract_assertions(text: str, path: str) -> list[AssertionLiteral]:
    masked = _mask_comments(text)
    found: list[AssertionLiteral] = []
    for match in ASSERTION_NAME.finditer(masked):
        line_start = masked.rfind("\n", 0, match.start()) + 1
        if DIRECTIVE.match(masked[line_start:match.start()]):
            continue
        end = _balanced_call(masked, match.start())
        found.append(AssertionLiteral(path, _line_of(masked, match.start()),
                                      normalize(text[match.start():end])))
    return found


extract_checks = extract_assertions


def split_markdown_cells(body: str) -> list[str]:
    cells: list[str] = []
    value: list[str] = []
    escaped = False
    for char in body:
        if escaped:
            value.append(char); escaped = False
        elif char == "\\":
            escaped = True; value.append(char)
        elif char == "|":
            cells.append("".join(value).strip()); value = []
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
        raise ParityError("twin ledger lacks Appendix 5")
    section = text[start + len(APPENDIX_HEADING):]
    next_heading = re.search(r"^##\s+", section, re.M)
    if next_heading: section = section[:next_heading.start()]
    if TABLE_HEADING not in section:
        raise ParityError("Appendix 5 lacks the required exact-row heading")
    rows: dict[tuple[str, str], LedgerLiteral] = {}
    ranges: dict[tuple[str, int, int], RangeLedgerLiteral] = {}
    for raw in section.splitlines():
        match = TABLE_ROW.match(raw.strip())
        if not match: continue
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
            first, last = int(location_match["first"]), int(location_match["last"])
            count = int(count_match["count"])
            if first <= 0 or last < first or count <= 0 or not group or not reason or not covering:
                raise ParityError("Appendix 5 range row is incomplete: " + raw)
            key = (location_match["path"], first, last)
            if key in ranges: raise ParityError("Appendix 5 duplicates range: " + location)
            ranges[key] = RangeLedgerLiteral(key[0], first, last, count,
                                              group, reason, covering)
            continue
        if len(cells) != 4: continue
        location, assertion, reason, covering = cells
        if not re.fullmatch(r"tests/test_[A-Za-z0-9_]+\.cpp:\d+", location):
            raise ParityError("Appendix 5 has invalid base location: " + location)
        if not assertion or not reason or not covering:
            raise ParityError("Appendix 5 row is incomplete: " + raw)
        key = (location, normalize(assertion))
        if key in rows: raise ParityError("Appendix 5 duplicates literal: " + location)
        rows[key] = LedgerLiteral(location, key[1], reason, covering)
    return rows, ranges


def inventory_names(inventory: Path, *, families: Iterable[str] | None = None) -> list[str]:
    data = json.loads(inventory.read_text())
    if data.get("base") != BASE:
        raise ParityError("twin inventory does not pin " + BASE)
    if families is None:
        names = data.get("removed")
    else:
        declared = data.get("families")
        if not isinstance(declared, dict):
            raise ParityError("twin inventory has no family map")
        names = []
        for family in families:
            members = declared.get(family)
            if not isinstance(members, list) or not all(isinstance(n, str) for n in members):
                raise ParityError("twin inventory has no string family: " + family)
            names.extend(members)
    if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
        raise ParityError("twin inventory has no string removed list")
    if len(names) != len(set(names)):
        raise ParityError("twin inventory has duplicate names")
    return [name for name in names if name not in A24_NAMES]


def load_base_manifest(path: Path) -> dict[str, list[AssertionLiteral]]:
    data = json.loads(path.read_text())
    if data.get("schema") != "pineforge-r4-d-twin-base/v2" or data.get("base") != BASE:
        raise ParityError("unknown twin base manifest")
    tests = data.get("tests")
    if not isinstance(tests, dict): raise ParityError("twin base manifest has no tests")
    result: dict[str, list[AssertionLiteral]] = {}
    for name, row in tests.items():
        assertions = row.get("assertions") if isinstance(row, dict) else None
        if not isinstance(assertions, list):
            raise ParityError("base manifest row has no assertions: " + name)
        path_name = "tests/" + name + ".cpp"
        parsed: list[AssertionLiteral] = []
        for assertion in assertions:
            if (not isinstance(assertion, dict)
                    or not isinstance(assertion.get("line"), int)
                    or not isinstance(assertion.get("text"), str)):
                raise ParityError("invalid base assertion row: " + name)
            parsed.append(AssertionLiteral(path_name, assertion["line"],
                                           normalize(assertion["text"])))
        result[name] = parsed
    return result


def find_twin(tests: Path, name: str) -> Path:
    matches = sorted(tests.glob(name + "_l4*.cpp"))
    if len(matches) != 1:
        if not matches: raise ParityError("missing A29 twin: tests/" + name + "_l4*.cpp")
        raise ParityError("ambiguous A29 twins for " + name + ": "
                          + ", ".join(path.name for path in matches))
    return matches[0]


def validate_covering_row(root: Path, row: LedgerLiteral | RangeLedgerLiteral) -> None:
    matches = list(COVERING_ROW.finditer(row.covering))
    if not matches:
        raise ParityError("Appendix 5 covering twin row is invalid: " + row.covering)
    for match in matches:
        path = root / match["path"]
        if not path.is_file():
            raise ParityError("Appendix 5 covering twin is missing: " + match["path"])
        line = int(match["line"])
        calls = extract_assertions(path.read_text(), match["path"])
        if not any(call.line == line for call in calls):
            raise ParityError("Appendix 5 covering line has no assertion: "
                              + match.group(0).strip())


def _is_obvious_tautology(assertion: AssertionLiteral) -> bool:
    text = assertion.text
    open_at, close_at = text.find("("), text.rfind(")")
    body = text[open_at + 1:close_at].strip() if open_at >= 0 and close_at > open_at else ""
    if body in {"true", "1"}: return True
    return re.fullmatch(r"([A-Za-z_]\w*)\s*==\s*\1", body) is not None


def _body_fingerprint(path: Path) -> str:
    text = _mask_comments(path.read_text())
    text = re.sub(r"\s+", "", text)
    return hashlib.sha256(text.encode()).hexdigest()


def _assertion_digest(assertions: list[AssertionLiteral]) -> str:
    payload = "\n".join(row.text for row in assertions) + "\n"
    return hashlib.sha256(payload.encode()).hexdigest()


def check_inventory(*, root: Path = ROOT, inventory: Path = INVENTORY,
                    ledger: Path = LEDGER, base_manifest: Path = BASE_MANIFEST,
                    base_reader: Callable[[str], str] | None = None,
                    names: Iterable[str] | None = None,
                    families: Iterable[str] | None = None) -> dict[str, int]:
    if names is not None and families is not None:
        raise ParityError("choose names or families, not both")
    if base_reader is None and root == ROOT:
        require_fixture(base_manifest, BASE_MANIFEST_SHA256, "twin base manifest")
        require_fixture(inventory, INVENTORY_SHA256, "twin inventory")
        require_fixture(ledger, LEDGER_SHA256, "twin ledger")
    selected = list(names) if names is not None else inventory_names(inventory, families=families)
    appendix, range_appendix = read_appendix(ledger)
    manifest = None if base_reader is not None else load_base_manifest(base_manifest)
    inventory_data = json.loads(inventory.read_text())
    rewrites = inventory_data.get("observableRewrites", {})
    if not isinstance(rewrites, dict):
        raise ParityError("twin inventory observableRewrites must be an object")
    total_base = total_matched = total_ledgered = total_rewritten = total_extra = 0
    used_exact: set[tuple[str, str]] = set()
    used_ranges: set[tuple[str, int, int]] = set()
    restored_fingerprints: dict[str, str] = {}
    for name in selected:
        base_path = "tests/" + name + ".cpp"
        if base_reader is not None:
            base = extract_assertions(base_reader(name), base_path)
        else:
            if manifest is None or name not in manifest:
                raise ParityError("base manifest lacks " + name)
            base = manifest[name]
        twin = find_twin(root / "tests", name)
        twin_path = "tests/" + twin.name
        twin_assertions = extract_assertions(twin.read_text(), twin_path)
        if name in RESTORED_CLONE_TWINS:
            if len(twin_assertions) < MIN_RESTORED_ASSERTIONS:
                raise ParityError(
                    f"restored twin {name} has only {len(twin_assertions)} assertions; "
                    f"requires at least {MIN_RESTORED_ASSERTIONS}")
            fingerprint = _body_fingerprint(twin)
            duplicate = next((other for other, value in restored_fingerprints.items()
                              if value == fingerprint), None)
            if duplicate:
                raise ParityError(f"restored twins share one body: {duplicate}, {name}")
            restored_fingerprints[name] = fingerprint

        remaining = set(range(len(base)))
        relevant = {key: row for key, row in appendix.items()
                    if key[0].startswith(base_path + ":")}
        relevant_ranges = {key: row for key, row in range_appendix.items()
                           if key[0] == base_path}
        by_key: dict[tuple[str, str], list[int]] = {}
        for index, item in enumerate(base):
            by_key.setdefault((item.location, item.text), []).append(index)
        for key, row in relevant.items():
            matches = by_key.get(key, [])
            if len(matches) != 1:
                raise ParityError("Appendix 5 literal does not uniquely match base assertion: "
                                  + row.location + " " + row.text)
            remaining.discard(matches[0])
            validate_covering_row(root, row)
            used_exact.add(key)

        ordered_ranges = sorted(relevant_ranges.values(),
                                key=lambda row: (row.first_line, row.last_line))
        for number, row in enumerate(ordered_ranges):
            if number and row.first_line <= ordered_ranges[number - 1].last_line:
                raise ParityError("Appendix 5 ranges overlap: " + row.location)
            available = [index for index in sorted(remaining)
                         if row.first_line <= base[index].line <= row.last_line]
            # A whole-owner range (pending_order_identity and two A36 helpers)
            # is unobservable by definition; consume it before facade matching
            # so coincidental shared numeric literals cannot turn it into a
            # partly observable claim.
            if len(available) == row.count:
                remaining.difference_update(available)
                validate_covering_row(root, row)
                used_ranges.add((row.path, row.first_line, row.last_line))

        remaining_by_text: dict[str, list[int]] = {}
        for index in sorted(remaining):
            remaining_by_text.setdefault(base[index].text, []).append(index)
        remaining_by_signature: dict[tuple[str, ...], list[int]] = {}
        for index in sorted(remaining):
            signature = literal_signature(base[index].text)
            if len(signature) > 1:
                remaining_by_signature.setdefault(signature, []).append(index)
        matched = 0
        extra = 0
        for assertion in twin_assertions:
            candidates = remaining_by_text.get(assertion.text)
            if candidates:
                index = candidates.pop(0)
                remaining.discard(index)
                matched += 1
                signature = literal_signature(base[index].text)
                if len(signature) > 1 and index in remaining_by_signature.get(signature, []):
                    remaining_by_signature[signature].remove(index)
                continue
            signature = literal_signature(assertion.text)
            candidates = remaining_by_signature.get(signature) if len(signature) > 1 else None
            while candidates and candidates[0] not in remaining:
                candidates.pop(0)
            if candidates:
                index = candidates.pop(0)
                remaining.discard(index)
                if index in remaining_by_text.get(base[index].text, []):
                    remaining_by_text[base[index].text].remove(index)
                matched += 1
            else:
                if _is_obvious_tautology(assertion):
                    raise ParityError("tautological additional twin assertion: "
                                      + assertion.location + " " + assertion.text)
                extra += 1

        for row in ordered_ranges:
            key = (row.path, row.first_line, row.last_line)
            if key in used_ranges:
                continue
            candidates = [index for index in sorted(remaining)
                          if row.first_line <= base[index].line <= row.last_line]
            if len(candidates) < row.count:
                raise ParityError(
                    f"Appendix 5 range {row.location} requires {row.count} unmatched "
                    f"assertions, found only {len(candidates)}")
            remaining.difference_update(candidates[:row.count])
            validate_covering_row(root, row)
            used_ranges.add(key)

        if remaining:
            rewrite = rewrites.get(name)
            if not isinstance(rewrite, dict):
                first = base[min(remaining)]
                raise ParityError(
                    f"literal parity mismatch for {name}: uncovered {first.location} {first.text}; "
                    f"base={len(base)} matched={matched} ledgered="
                    f"{len(relevant) + sum(row.count for row in relevant_ranges.values())} "
                    f"extra={extra}")
            expected = {
                "twin": twin.name,
                "baseAssertions": len(base),
                "twinAssertions": len(twin_assertions),
                "twinAssertionSha256": _assertion_digest(twin_assertions),
            }
            for key, value in expected.items():
                if rewrite.get(key) != value:
                    raise ParityError(
                        f"observable rewrite evidence changed for {name}: "
                        f"{key} expected {rewrite.get(key)!r}, got {value!r}")
            reason = rewrite.get("reason")
            if not isinstance(reason, str) or not reason.strip():
                raise ParityError("observable rewrite lacks a reason: " + name)
            total_rewritten += len(remaining)
            remaining.clear()
        total_base += len(base)
        total_matched += matched
        total_ledgered += len(relevant) + sum(row.count for row in relevant_ranges.values())
        total_extra += extra

    selected_paths = {"tests/" + name + ".cpp" for name in selected}
    unused = {key for key in appendix if key[0].rsplit(":", 1)[0] in selected_paths} - used_exact
    if unused:
        first = next(iter(sorted(unused)))
        raise ParityError("Appendix 5 contains unused exact row: " + first[0])
    unused_ranges = {key for key in range_appendix if key[0] in selected_paths} - used_ranges
    if unused_ranges:
        first = next(iter(sorted(unused_ranges)))
        raise ParityError(f"Appendix 5 contains unused range: {first[0]}:{first[1]}-{first[2]}")
    return {"tests": len(selected), "base": total_base, "matched": total_matched,
            "ledgered": total_ledgered, "rewritten": total_rewritten,
            "extra": total_extra}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", action="append", default=[])
    parser.add_argument("--family", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        result = check_inventory(names=args.name or None, families=args.family or None)
    except (OSError, json.JSONDecodeError, ParityError) as error:
        print("check_twin_parity: " + str(error), file=sys.stderr)
        return 1
    print("check_twin_parity: {tests} tests, {base} base assertions, "
          "{matched} literal-matched twin assertions, {ledgered} ledgered, "
          "{rewritten} pinned observable rewrites, {extra} additional twin "
          "assertions, OK".format(**result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
