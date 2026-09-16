#!/usr/bin/env python3
"""Validate every frozen pending-row projection and its approved provenance."""
from __future__ import annotations

import json
from pathlib import Path
import re

from check_pending_order_prefix import _struct_fields


ROOT = Path(__file__).resolve().parents[1]
KINDS = {
    "request-core definition", "live state", "receipt fact",
    "adapter placement snapshot", "derived",
}
CONSTANT = re.compile(
    r"^\s*(?:0(?:\.0)?(?:U|ULL|L)?|-1|kNaN|"
    r"std::numeric_limits<double>::quiet_NaN\(\)|false|true|nullptr|\{\})\s*$")
IDENT = re.compile(r"^(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)$")
CONSTEXPR_NAME = re.compile(
    r"\bconstexpr\b[^;{=]*\b([A-Za-z_]\w*)\s*[={;]")
CONST_OBJECT = re.compile(
    r"\b(?:static\s+)?const\b(?!\s*expr\b)([^;=]*?)\b([A-Za-z_]\w*)\s*=\s*([^;]+);")
ENUM_BLOCK = re.compile(
    r"\benum\b(?:\s+class|\s+struct)?(?:\s+\w+)?\s*(?::[^{]+)?\{([^}]*)\}", re.S)
ENUM_MEMBER = re.compile(r"(?:^|,)\s*([A-Za-z_]\w*)\b")
STATIC_CAST = re.compile(r"^static_cast\s*<[^>]+>\s*\((.*)\)$")
SOURCE_TOKEN = re.compile(r"(?:::)?([A-Za-z_]\w+)(?=::|\b)")


def die(message: str) -> None:
    raise ValueError("pending_intent_view: " + message)


def named(rows: list[dict], key: str) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for row in rows:
        name = row.get(key)
        if not isinstance(name, str) or not name or name in result:
            die(f"invalid or duplicate {key}: {row!r}")
        result[name] = row
    return result


def check_row(row: dict, name: str) -> None:
    if row.get("kind") not in KINDS:
        die(f"{name} has invalid kind")
    source = row.get("source")
    if not isinstance(source, str) or not source or source.strip().lower() == "constant":
        die(f"{name} lacks a truthful source")
    if row.get("no_write") is not True:
        die(f"{name} must be read-only")
    if row["kind"] == "derived" and not isinstance(row.get("derivation"), str):
        die(f"derived {name} lacks its derivation")


def function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0: die("copy_v1 implementation is missing")
    opening = text.find("{", start)
    depth = 1
    index = opening + 1
    while index < len(text) and depth:
        depth += (text[index] == "{") - (text[index] == "}")
        index += 1
    if depth: die("copy_v1 implementation is unclosed")
    return text[opening + 1:index - 1]


def remove_false_blocks(text: str) -> str:
    result = list(text)
    for match in reversed(list(re.finditer(r"\bif\s*\(\s*false\s*\)\s*\{", text))):
        depth = 1
        index = match.end()
        while index < len(text) and depth:
            depth += (text[index] == "{") - (text[index] == "}")
            index += 1
        for position in range(match.start(), index):
            if result[position] != "\n": result[position] = " "
    return "".join(result)


def named_constants(text: str) -> set[str]:
    names = set(CONSTEXPR_NAME.findall(text))
    for decl, name, initial in CONST_OBJECT.findall(text):
        if "&" in decl:
            continue
        initial = initial.strip()
        if CONSTANT.fullmatch(initial) or IDENT.fullmatch(initial):
            names.add(name)
    for block in ENUM_BLOCK.findall(text):
        names.update(ENUM_MEMBER.findall(block))
    return names


def rhs_is_compile_time_constant(rhs: str, constants: set[str]) -> bool:
    value = rhs.strip()
    while True:
        cast = STATIC_CAST.match(value)
        if not cast:
            break
        value = cast.group(1).strip()
    if CONSTANT.fullmatch(value):
        return True
    ident = IDENT.fullmatch(value)
    return bool(ident and ident.group(1) in constants)


def projection_kind(body: str, field: str, constants: set[str]) -> str:
    occurrences = list(re.finditer(r"out->" + re.escape(field) + r"\b", body))
    dynamic = False
    for match in occurrences:
        tail = body[match.end():]
        assignment = re.match(r"\s*=\s*", tail)
        if not assignment:
            # Passed by reference/pointer to a projection helper.
            dynamic = True
            continue
        rhs_start = match.end() + assignment.end()
        semicolon = body.find(";", rhs_start)
        if semicolon < 0: die("unterminated projection assignment for " + field)
        rhs = body[rhs_start:semicolon].strip()
        if re.match(r"^false\s*\?", rhs):
            continue
        if not rhs_is_compile_time_constant(rhs, constants):
            dynamic = True
    return "dynamic" if dynamic else "constant"


def load_debt(root: Path) -> set[str]:
    result = set()
    path = root / "scripts/pending_intent_constant_debt.txt"
    for raw in path.read_text().splitlines():
        value = raw.strip()
        if value and not value.startswith("#"):
            if value in result: die("duplicate constant-debt row: " + value)
            result.add(value)
    return result


def source_corpus(root: Path) -> str:
    values = []
    for directory in (root / "include", root / "src"):
        for path in directory.rglob("*"):
            if path.is_file():
                values.append(path.read_text(errors="ignore"))
    return "\n".join(values)


def validate_provenance_tokens(schema: dict, corpus: str) -> None:
    for group, key in (("source_pending_order_inventory", "member"),
                       ("prefix_fields", "field"), ("probes", "name")):
        for row in schema[group]:
            source = row.get("source", "")
            candidates = [
                token for token in SOURCE_TOKEN.findall(source)
                if ("_" in token or (token[:1].isupper() and len(token) > 2))
                and token != "PineExecutionAdapter"
            ]
            missing = sorted({token for token in candidates if token not in corpus})
            if missing:
                die(f"{group} {row[key]} names absent provenance: {', '.join(missing)}")


def check(root: Path = ROOT) -> dict[str, int]:
    schema_path = root / "scripts/pending_intent_view.json"
    prefix_path = root / "scripts/pending_order_v1_prefix.json"
    schema = json.loads(schema_path.read_text())
    if schema.get("schema") != "pineforge-r4-d-pending-intent-view/v1":
        die("unknown schema")
    if schema.get("open") != []:
        die("OPEN fields require a root disposition")
    inventory = named(schema.get("source_pending_order_inventory", []), "member")
    if len(inventory) != 65:
        die("source inventory must retain the approved 65-member capture")
    for name, row in inventory.items(): check_row(row, name)

    prefix = named(schema.get("prefix_fields", []), "field")
    expected_prefix = {
        name: typ for typ, name in json.loads(prefix_path.read_text())["members"]}
    if set(prefix) != set(expected_prefix): die("public prefix coverage is incomplete")
    for name, typ in expected_prefix.items():
        if prefix[name].get("cpp_type") != typ: die("public field type drift: " + name)
        check_row(prefix[name], name)

    projection_text = (root / "src/source/pine_adapter.cpp").read_text()
    body = remove_false_blocks(function_body(
        projection_text, "int PendingIntentView::copy_v1("))
    body = re.sub(r"\(\s*void\s*\)\s*out->[A-Za-z_]\w*\s*;", "", body)
    constants = named_constants(projection_text)
    mirror = (root / "include/pineforge/pending_order_mirror.hpp").read_text()
    fields = [row[0] for row in _struct_fields(mirror)]
    if len(fields) != 406: die(f"frozen mirror field count changed: {len(fields)}")
    constant = {field for field in fields
                if projection_kind(body, field, constants) == "constant"}
    debt = load_debt(root)
    unknown = sorted(debt - set(fields))
    unexpected = sorted(constant - debt)
    stale = sorted(debt - constant)
    if unknown or unexpected or stale:
        die(f"constant projection mismatch: unexpected={unexpected}, "
            f"stale_debt={stale}, unknown_debt={unknown}")

    validate_provenance_tokens(schema, source_corpus(root))

    probes = named(schema.get("probes", []), "name")
    expected_probes = {
        "probe_fill_qty", "pending_order_level_resolved",
        "pending_order_effective_levels", "last_bar_dual_entry_path",
        "trail_best_price",
    }
    if set(probes) != expected_probes: die("probe coverage is incomplete")
    for name, row in probes.items():
        if row.get("kind") not in KINDS or not isinstance(row.get("source"), str):
            die(f"probe {name} lacks a truthful source")
        if not isinstance(row.get("derivation"), str) or not isinstance(row.get("failure"), str):
            die(f"probe {name} lacks derivation/failure convention")

    return {"captured": len(inventory), "prefix": len(prefix),
            "mirror": len(fields), "dynamic": len(fields) - len(constant),
            "debt": len(debt), "probes": len(probes)}


def main() -> int:
    try:
        result = check()
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise SystemExit(str(error))
    print("pending_intent_view: {captured} captured members, {prefix} schema fields, "
          "{mirror} C fields, {dynamic} live projections, {debt} pinned sibling-lane "
          "debts, {probes} probes, 0 OPEN".format(**result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
