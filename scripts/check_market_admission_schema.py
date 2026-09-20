#!/usr/bin/env python3
"""Validate canonical admission reflection and the adapter hash fold."""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(root: Path = ROOT) -> int:
    header = (root / "include/pineforge/source/market_admission.hpp").read_text()
    source = (root / "src/source/market_admission.cpp").read_text()
    adapter_hash = (root / "src/source/pine_state_hash.cpp").read_text()
    schema = json.loads((root / "scripts/market_admission_schema.json").read_text())
    required = {
        "Configuration", "PriceRequest", "CurrentPrices", "SizingObservation",
        "CommandObservation", "ReviewReceipt", "SizingRevision", "Draft",
        "BookObservation", "CommandEvent", "InstructionResolution", "ReviewEvent",
        "SizingEvent", "Journal",
    }
    if set(schema) != required:
        raise ValueError("canonical admission schema changed")
    for name in required:
        if not re.search(r"\b(?:class|struct)\s+" + re.escape(name) + r"\s*\{", header):
            raise ValueError(name + " storage is missing")
    for token in (
        "void reflect(const Draft& value", "void reflect(const Event& value",
        "void Journal::reflect", "admission_journal.reflect(\"journal\"",
    ):
        if token not in (source + "\n" + adapter_hash):
            raise ValueError("admission reflection/hash fold is missing: " + token)
    waivers = (root / "scripts/broker_state_hash_waivers.txt").read_text()
    if "market_admission" in waivers:
        raise ValueError("admission state cannot be waived")
    return len(required)


if __name__ == "__main__":
    try:
        print("market admission: " + str(check()) + " canonical owners reflected and hashed")
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
