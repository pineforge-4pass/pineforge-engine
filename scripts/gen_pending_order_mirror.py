#!/usr/bin/env python3
"""Verify the frozen C mirror and its native intent-view projection.

The public POD is append-only and is intentionally not regenerated from a
runtime owner.  Its values are projected allocation-free by PendingIntentView
from native request definitions, live state, receipts, and placement facts.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from test_pending_intent_view import check as check_intent_projection

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "scripts" / "pending_intent_view.json"
HEADER = ROOT / "include" / "pineforge" / "pending_order_mirror.hpp"
ADAPTER_HEADER = ROOT / "include" / "pineforge" / "source" / "pine_adapter.hpp"
ADAPTER_SOURCE = ROOT / "src" / "source" / "pine_adapter.cpp"


def check() -> None:
    if not HEADER.is_file():
        raise SystemExit("mirror header is missing")
    if not ADAPTER_HEADER.is_file() or not ADAPTER_SOURCE.is_file():
        raise SystemExit("intent-view projection inputs are missing")
    schema = json.loads(SCHEMA.read_text())
    if schema.get("schema") != "pineforge-r4-d-pending-intent-view/v1":
        raise SystemExit("intent-view schema identity changed")
    if schema.get("open") not in (None, []):
        raise SystemExit("intent-view schema has unresolved fields")
    declaration = ADAPTER_HEADER.read_text()
    implementation = ADAPTER_SOURCE.read_text()
    for required in (
        "class PendingIntentView",
        "int copy_v1(int index, pf_pending_order_v1_t* out) const noexcept;",
    ):
        if required not in declaration:
            raise SystemExit("intent-view declaration is incomplete: " + required)
    if "int PendingIntentView::copy_v1(" not in implementation:
        raise SystemExit("intent-view C projection implementation is missing")
    # The generator/check entry point is a ci_verify source guard. Keep it
    # fail-closed over all 406 value projections, not merely the declaration.
    try:
        check_intent_projection(ROOT)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.parse_args()
    check()
    print("pending mirror: frozen POD projected by PendingIntentView")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
