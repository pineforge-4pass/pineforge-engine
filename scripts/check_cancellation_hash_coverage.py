#!/usr/bin/env python3
"""Source-level one-to-one coverage for the cancellation receipt leaves."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
hash_source = (ROOT / "src/source/pine_state_hash.cpp").read_text()
mirror_source = (ROOT / "src/source/pine_pending_mirror.cpp").read_text()
leaves = [
    "cause", "state", "close_claim_release", "source_incarnation",
    "source_sequence", "target_incarnation", "target_owner", "target_revision",
    "close_claim_consumed", "close_claim_retired",
]
for leaf in leaves:
    hash_count = len(re.findall(rf"o\.cancellation\.{re.escape(leaf)}\(\)", hash_source))
    mirror_count = len(re.findall(
        rf"out->cancellation_{re.escape(leaf)}\s*=.*?src\.cancellation\.{re.escape(leaf)}\(\)",
        mirror_source,
    ))
    if hash_count != 1:
        raise SystemExit(f"cancellation hash leaf {leaf}: expected one fold, got {hash_count}")
    if mirror_count != 1:
        raise SystemExit(f"cancellation mirror leaf {leaf}: expected one projection, got {mirror_count}")
print(f"cancellation hash/mirror coverage: {len(leaves)} leaves each folded and projected once")
