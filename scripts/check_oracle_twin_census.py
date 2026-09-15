#!/usr/bin/env python3
"""Prove that every retained L0 oracle is compiled through its exact twin."""
from __future__ import annotations

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "oracle"
TWINS = {
    "test_oracle_coof": "test_native_oracle_coof_l2.cpp",
    "test_oracle_day_key": "test_native_oracle_day_key_l2.cpp",
    "test_oracle_deferred_any_witnesses": "test_native_oracle_deferred_any_witnesses_l2.cpp",
    "test_oracle_deferred_birth": "test_native_oracle_deferred_birth_l2.cpp",
    "test_oracle_frozen_size": "test_native_oracle_frozen_size_full_l2.cpp",
    "test_oracle_fx": "test_native_oracle_fx_l2.cpp",
    "test_oracle_magnifier_barstate": "test_native_oracle_magnifier_barstate_l2.cpp",
    "test_oracle_magnifier_distribution": "test_native_oracle_magnifier_distribution_l2.cpp",
    "test_oracle_more_than_64_fills": "test_native_oracle_more_than_64_fills_l2.cpp",
    "test_oracle_pooc_freeze": "test_native_oracle_pooc_freeze_l2.cpp",
    "test_oracle_pooc_immediate": "test_native_oracle_pooc_immediate_l2.cpp",
    "test_oracle_relative_exit": "test_native_oracle_relative_exit_l2.cpp",
    "test_oracle_reversal_close_only": "test_native_oracle_reversal_close_only_l2.cpp",
    "test_oracle_reversal_later_tick": "test_native_oracle_reversal_later_tick_l2.cpp",
    "test_oracle_reversal_replaced_percent": "test_native_oracle_reversal_replaced_percent_l2.cpp",
    "test_oracle_reversal_same_bar_tx": "test_native_oracle_reversal_same_bar_tx_l2.cpp",
    "test_oracle_short_seed": "test_native_oracle_short_seed_full_l2.cpp",
    "test_oracle_short_seed_percent": "test_native_oracle_short_seed_percent_full_l2.cpp",
    "test_oracle_stop_snapshot": "test_native_oracle_stop_snapshot_full_l2.cpp",
}

# Count calls with Python re rather than accepting a handwritten claim. Macro
# definitions are not assertions, so they are excluded line-by-line.
CHECK = re.compile(r"\bCHECK\s*\(")
DEFINE = re.compile(r"^\s*#\s*define\s+CHECK\b")
EXPECTED_CHECKS = {
    "test_oracle_coof": 34,
    "test_oracle_day_key": 84,
    "test_oracle_deferred_any_witnesses": 18,
    "test_oracle_deferred_birth": 36,
    "test_oracle_frozen_size": 23,
    "test_oracle_fx": 127,
    "test_oracle_magnifier_barstate": 9,
    "test_oracle_magnifier_distribution": 14,
    "test_oracle_more_than_64_fills": 4,
    "test_oracle_pooc_freeze": 22,
    "test_oracle_pooc_immediate": 13,
    "test_oracle_relative_exit": 31,
    "test_oracle_reversal_close_only": 35,
    "test_oracle_reversal_later_tick": 47,
    "test_oracle_reversal_replaced_percent": 50,
    "test_oracle_reversal_same_bar_tx": 3,
    "test_oracle_short_seed": 105,
    "test_oracle_short_seed_percent": 86,
    "test_oracle_stop_snapshot": 50,
}


def check_count(path: Path) -> int:
    return sum(len(CHECK.findall(line)) for line in path.read_text().splitlines()
               if not DEFINE.match(line))


def main() -> int:
    actual = {path.stem: path for path in ORACLE.glob("test_oracle_*.cpp")}
    if set(actual) != set(TWINS) or set(actual) != set(EXPECTED_CHECKS):
        raise SystemExit("oracle twin census: oracle/twin inventory drift")
    total = 0
    for name, oracle in sorted(actual.items()):
        count = check_count(oracle)
        if count != EXPECTED_CHECKS[name]:
            raise SystemExit(f"oracle twin census: {name} CHECK count {count}, expected {EXPECTED_CHECKS[name]}")
        twin = ROOT / "tests" / TWINS[name]
        if not twin.is_file():
            raise SystemExit("oracle twin census: missing twin " + str(twin))
        text = twin.read_text()
        include = '#include "oracle/' + oracle.name + '"'
        if text.count(include) != 1:
            raise SystemExit("oracle twin census: " + twin.name + " must include exactly " + include)
        if re.search(r"\bint\s+main\s*\(|\bCHECK\s*\(", text):
            raise SystemExit("oracle twin census: hand-copied body remains in " + twin.name)
        total += count
    print(f"oracle twin census: {len(actual)} exact includes, {total} CHECK calls, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
