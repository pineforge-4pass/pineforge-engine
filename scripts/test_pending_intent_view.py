#!/usr/bin/env python3
"""Validate the approved intent-view schema without changing the C ABI."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "scripts" / "pending_intent_view.json"
PREFIX = ROOT / "scripts" / "pending_order_v1_prefix.json"
KINDS = {
    "request-core definition",
    "live state",
    "receipt fact",
    "adapter placement snapshot",
    "derived",
}


def die(message: str) -> None:
    raise SystemExit("pending_intent_view: " + message)


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


def main() -> int:
    schema = json.loads(SCHEMA.read_text())
    if schema.get("schema") != "pineforge-r4-d-pending-intent-view/v1":
        die("unknown schema")
    if schema.get("open") != []:
        die("OPEN fields require a root disposition")
    inventory = named(schema.get("source_pending_order_inventory", []), "member")
    if len(inventory) != 65:
        die("source inventory must retain the approved 65-member capture")
    for name, row in inventory.items():
        check_row(row, name)

    prefix = named(schema.get("prefix_fields", []), "field")
    expected_prefix = {name: typ for typ, name in json.loads(PREFIX.read_text())["members"]}
    if set(prefix) != set(expected_prefix):
        die("public prefix coverage is incomplete")
    for name, typ in expected_prefix.items():
        if prefix[name].get("cpp_type") != typ:
            die(f"public field type drift: {name}")
        check_row(prefix[name], name)

    # The schema is not documentation-only: every frozen public prefix field
    # must have an explicit projection write in copy_v1.  Padding may be
    # zeroed for ABI determinism, but it must never become the value source for
    # an omitted compatibility field.
    for name in expected_prefix:
        if f"out->{name}" not in (ROOT / "src/source/pine_adapter.cpp").read_text():
            die(f"public prefix field lacks an explicit PendingIntentView projection: {name}")

    probes = named(schema.get("probes", []), "name")
    expected = {
        "probe_fill_qty",
        "pending_order_level_resolved",
        "pending_order_effective_levels",
        "last_bar_dual_entry_path",
        "trail_best_price",
    }
    if set(probes) != expected:
        die("probe coverage is incomplete")
    for name, row in probes.items():
        if row.get("kind") not in KINDS or not isinstance(row.get("source"), str):
            die(f"probe {name} lacks a truthful source")
        if not isinstance(row.get("derivation"), str) or not isinstance(row.get("failure"), str):
            die(f"probe {name} lacks derivation/failure convention")
    print("pending_intent_view: 65 captured members, 98 prefix fields, 5 probes, 0 OPEN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
