#!/usr/bin/env python3
"""Prove that every retained L0 oracle is compiled through its exact twin."""
from __future__ import annotations

from pathlib import Path
import json
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


# The oracle sources the private-fixture PR removes (their rows are pinned in
# tests/private_fixture_rows.txt: test_native_oracle_day_key_l2,
# test_native_oracle_reversal_same_bar_tx_l2, test_oracle_day_key,
# test_oracle_reversal_same_bar_tx, test_oracle_stop_snapshot and
# test_native_oracle_stop_snapshot_full_l2). They are allowed to be absent --
# named in the output -- and are censused exactly like the rest whenever they
# are present (an overlay carrying the private tree is verified too). Any other
# missing oracle, any unpinned oracle, a changed CHECK count or a twin that no
# longer includes exactly its oracle still fails.
def _private_fixture_oracles() -> tuple:
    """The pin's own privateFixtureAbsent list, as row names (no .cpp).

    The pin (tests/oracle.sha256) is the one source for which oracles the
    removals take; a hand-written second copy of it is exactly the class that
    drifts, so this reads the pin and dies loudly when it cannot."""
    pin = ROOT / "tests" / "oracle.sha256"
    try:
        names = json.loads(pin.read_text())["privateFixtureAbsent"]
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f"oracle twin census: {pin}: privateFixtureAbsent "
                         f"unreadable ({error})")
    return tuple(name[:-4] if name.endswith(".cpp") else name for name in names)


PRIVATE_FIXTURE_ORACLES = _private_fixture_oracles()


def main() -> int:
    actual = {path.stem: path for path in ORACLE.glob("test_oracle_*.cpp")}
    unexpected = sorted(set(actual) - set(TWINS) | set(actual) - set(EXPECTED_CHECKS))
    if unexpected:
        raise SystemExit("oracle twin census: oracle/twin inventory drift (unpinned oracle: "
                         + unexpected[0] + ")")
    missing = sorted(set(TWINS) - set(actual))
    undeclared = [name for name in missing if name not in PRIVATE_FIXTURE_ORACLES]
    if undeclared:
        raise SystemExit("oracle twin census: oracle/twin inventory drift (missing oracle: "
                         + undeclared[0] + ")")
    # The two static tables are one surface: a name in either alone is drift.
    # The inventory arms above read the files the tree HAS, so a name only
    # EXPECTED_CHECKS knows is invisible for an oracle the removals took; this
    # arm is what main's pre-removal `set(actual) - set(TWINS)` caught.
    static_drift = sorted(set(EXPECTED_CHECKS) ^ set(TWINS))
    if static_drift:
        where = ("EXPECTED_CHECKS" if static_drift[0] in EXPECTED_CHECKS else "TWINS")
        raise SystemExit("oracle twin census: the static tables disagree ("
                         + static_drift[0] + " is in " + where + " alone)")
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
    detail = f"{len(actual)} exact includes, {total} CHECK calls"
    if missing:
        detail += (", private fixtures not present: "
                   + ", ".join("tests/oracle/" + name + ".cpp" for name in missing))
    print(f"oracle twin census: {detail}, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
