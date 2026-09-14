#!/usr/bin/env python3
"""Validate R4-D Appendix-C PendingIntentView coverage without changing ABI."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_pending_order_mirror as mirror  # noqa: E402

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


def by_name(rows: list[dict], key: str) -> dict[str, dict]:
    found: dict[str, dict] = {}
    for row in rows:
        name = row.get(key)
        if not isinstance(name, str) or not name:
            die(f"row has no {key}: {row!r}")
        if name in found:
            die(f"duplicate {key}: {name}")
        found[name] = row
    return found


def check_row(row: dict, name: str) -> None:
    if row.get("kind") not in KINDS:
        die(f"{name} has invalid kind {row.get('kind')!r}")
    source = row.get("source")
    if not isinstance(source, str) or not source or source.strip().lower() == "constant":
        die(f"{name} lacks a truthful source")
    if row.get("no_write") is not True:
        die(f"{name} must declare no_write=true")
    if row["kind"] == "derived" and not isinstance(row.get("derivation"), str):
        die(f"derived {name} lacks its derivation")


def main() -> int:
    schema = json.loads(SCHEMA.read_text())
    if schema.get("schema") != "pineforge-r4-d-pending-intent-view/v1":
        die("unknown schema")
    if schema.get("open") != []:
        die("OPEN fields require root disposition before L2")
    source = by_name(schema.get("source_pending_order_inventory", []), "member")
    expected_source = {name: typ for typ, name in mirror.members()}
    if set(source) != set(expected_source):
        die("source PendingOrder inventory does not cover exactly the current 65 members")
    for name, typ in expected_source.items():
        if source[name].get("cpp_type") != typ:
            die(f"source member type drift: {name}")
        check_row(source[name], name)

    prefix = by_name(schema.get("prefix_fields", []), "field")
    expected_prefix = {name: typ for typ, name in json.loads(PREFIX.read_text())["members"]}
    if set(prefix) != set(expected_prefix):
        die("pf_pending_order_v1_t prefix is not covered exactly")
    for name, typ in expected_prefix.items():
        if prefix[name].get("cpp_type") != typ:
            die(f"prefix field type drift: {name}")
        check_row(prefix[name], name)

    probes = by_name(schema.get("probes", []), "name")
    expected_probes = {
        "probe_fill_qty",
        "pending_order_level_resolved",
        "pending_order_effective_levels",
        "last_bar_dual_entry_path",
        "trail_best_price",
    }
    if set(probes) != expected_probes:
        die("probe coverage is incomplete")
    for name, row in probes.items():
        if row.get("kind") not in KINDS or not isinstance(row.get("source"), str):
            die(f"probe {name} lacks a truthful source")
        if not isinstance(row.get("derivation"), str) or not isinstance(row.get("failure"), str):
            die(f"probe {name} lacks derivation/failure convention")
    print("pending_intent_view: 65 source members, 98 prefix fields, 5 probes, 0 OPEN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
