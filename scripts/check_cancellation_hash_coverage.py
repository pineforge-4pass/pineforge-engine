#!/usr/bin/env python3
"""Check that cancellation-capable native receipts remain in adapter state."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
adapter = (ROOT / "include/pineforge/source/pine_adapter.hpp").read_text()
hash_source = (ROOT / "src/source/pine_state_hash.cpp").read_text()
projection = (ROOT / "src/source/pine_adapter.cpp").read_text()

for value in ("receipt_cursor_", "live_by_source_key_", "bracket_families_"):
    if value not in adapter or value not in hash_source:
        raise SystemExit("adapter receipt hash coverage missing: " + value)
if "int PendingIntentView::copy_v1(" not in projection:
    raise SystemExit("intent-view C projection is missing")
print("adapter cancellation receipt and projection coverage: OK")
