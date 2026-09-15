#!/usr/bin/env python3
"""Validate the approved intent-view schema without changing the C ABI."""
from __future__ import annotations

import json
import re
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

# Fields that the final review found populated by unconditional literals after
# the legacy book was deleted.  They must remain expressions over live native
# events or adapter facts; padding is the only part of the POD zeroed by
# copy_v1 before field projection begins.
DYNAMIC_FIELDS = {
    "stop_limit_activated",
    "coof_cascade_seg_i",
    "dormant_bracket",
    "dormant_reissue_pending",
    "dormant_original_stop_price",
    "dormant_hold_bar",
    "dormant_reversal_kill_bar",
    "dormant_trail_best",
    "dormant_trail_best_start",
    "dormant_trail_leg_dead",
    "paired_flat_market_candidate",
    "paired_flat_market_own_qty",
    "paired_flat_market_signal_close",
    "paired_flat_market_signal_equity",
    "paired_flat_market_signal_margin_pct",
    "paired_flat_market_signal_pointvalue",
    "paired_flat_market_signal_fx",
    "paired_flat_market_peer_seq",
    "paired_flat_market_transaction_qty",
    "signal_close_mc_bar",
    "signal_close_mc_entry_incarnation",
    "signal_close_mc_fill_seq",
    "signal_close_mc_remaining_qty",
    "pooc_global_full_exit_dynamic_qty",
    "pooc_global_full_exit_tracks_bound_adds",
    "pooc_global_full_exit_bound_add",
    "suppressed_close_consumed_ledger_qty",
    "suppressed_close_retired_ledger_qty",
    "birth_cause",
    "cancellation_cause",
    "cancellation_state",
    "cancellation_close_claim_release",
}

CONSTANT_ASSIGNMENT = re.compile(
    r"out->(?P<field>[A-Za-z0-9_]+)\s*=\s*"
    r"(?:0(?:U|ULL|L)?|-1|kNaN|std::numeric_limits<double>::quiet_NaN\(\))\s*;"
)


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
    projection = (ROOT / "src/source/pine_adapter.cpp").read_text()
    for name in expected_prefix:
        if f"out->{name}" not in projection:
            die(f"public prefix field lacks an explicit PendingIntentView projection: {name}")
    constants = {match.group("field") for match in CONSTANT_ASSIGNMENT.finditer(projection)}
    static = sorted(DYNAMIC_FIELDS & constants)
    if static:
        die("constant compatibility projection(s): " + ", ".join(static))

    # The four L0 owner-private TUs are represented by public native twins.
    # Check the registrations and all review-spot-checked literals together so
    # an inventory edit cannot silently orphan a constant-sensitive witness.
    cmake = (ROOT / "tests/CMakeLists.txt").read_text()
    for target in ("test_live_pending_order_mirror", "test_oracle_coof_first_open",
                   "test_oracle_reversal"):
        if target not in cmake:
            die(f"missing public mirror/oracle twin target: {target}")
    coof_twin = (ROOT / "tests/test_native_l4c_coof_literals.cpp").read_text()
    reversal_twin = (ROOT / "tests/test_native_l4c_oracle_reversal_literals.cpp").read_text()
    for literal in ("lot_count == 6", "lot_price(1), 108.0", "lot_count == 5"):
        if literal not in coof_twin:
            die(f"missing public COOF literal: {literal}")
    for literal in ("0x3fb999999999999a", "0x3fb99999999999a0",
                    "4.7000000000000002", ".68965517241379315",
                    "1037.2413793103448"):
        if literal not in reversal_twin:
            die(f"missing reversal literal: {literal}")

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
    print("pending_intent_view: 65 captured members, 98 prefix fields, 32 live projections, 5 probes, 0 OPEN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
