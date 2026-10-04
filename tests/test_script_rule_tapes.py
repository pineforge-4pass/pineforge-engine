#!/usr/bin/env python3
"""TradingView's explicit-quantity and close-time bar-fill tapes, replayed row for row
through the generated strategies.

tests/fixtures/explicit_qty_floor and tests/fixtures/pooc_close_bar_fills hold
independently authored synthetic strategies, each exported by TradingView (twice,
byte-identical, but for the four earlier scheduled-stop tapes, exported once).
Every tape directory keeps the exact Pine bytes, the List of Trades (UTC+8), the
chart-feed rows the script ran on (bars.csv), the run configuration
(configuration.json) and the frozen generated C++ (generated.cpp, codegen
ee04fc6). CMake builds every tape into one module (tests/script_rule_tapes.cpp),
each tape's C entry points prefixed script_rule_<root>_<tape>__, so this test runs
the product path: generated code, library and scripts/run_strategy.py.

For every tape the engine must book TradingView's trades exactly: the same count
and, row for row, the same side, quantity (8 decimals), entry and exit time, and
entry and exit price (6 decimals), and the PnL within the report's float32 print,
max(1e-4, 1.2e-7 |pnl|). Both sides are sorted by the same key.

Each rule has a switch (ScriptRuleSwitches, include/pineforge/source/pine_adapter.hpp).
The test then turns each switch off in turn and requires exactly the departures
switch_off_departures.json records for it in each root: the tapes that rule decides,
and nothing else. That pins the rule-to-tape map and keeps every OFF branch run.

The hand-ported hosts in test_explicit_qty_floor_tapes.cpp and
test_pooc_close_bar_fills_tapes.cpp add what trades cannot show: the
strategy.equity / openprofit / netprofit / position_size readouts at the read bar
(to 10 decimals) and the guarded re-arm's branch with its position readouts.

usage: test_script_rule_tapes.py <fixtures dir> <module> <scripts dir> [--root NAME]... [--record]
  --root limits the run to the named fixture roots (default: both); ctest runs one
  row per root. --record rewrites each root's switch_off_departures.json from this
  run instead of checking it (the all-on run is still checked).
"""
import csv
import ctypes
import datetime
import json
import re
import sys
from pathlib import Path

ROOTS = ("explicit_qty_floor", "pooc_close_bar_fills")
SWITCHES = (  # script_rule_tapes_set_switch values
    ("explicit_qty_decimal_floor", 0),
    ("equity_tick_mark", 1),
    ("pooc_bracket_skips_inert_exits", 2),
)


def tv_epoch_ms(text):
    value = datetime.datetime.fromisoformat(text).replace(tzinfo=datetime.timezone.utc)
    return int(value.timestamp() * 1000) - 8 * 3600000


def tv_trades(path):
    entries, exits = {}, {}
    with path.open(encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            number = int(row["Trade number"])
            (entries if row["Type"].startswith("Entry") else exits)[number] = row
    rows = []
    for number in sorted(exits):
        entry, exit_ = entries[number], exits[number]
        price = next(k for k in entry if k.startswith("Price"))
        pnl = next(k for k in exit_ if k.startswith("Net PnL"))
        rows.append([entry["Type"].split()[-1].lower(), round(float(entry["Size (qty)"]), 8),
                     tv_epoch_ms(entry["Date and time"]), tv_epoch_ms(exit_["Date and time"]),
                     round(float(entry[price]), 6), round(float(exit_[price]), 6),
                     float(exit_[pnl])])
    return sorted(rows, key=row_key)


def engine_trades(result):
    rows = [["long" if t["is_long"] else "short", round(float(t["qty"]), 8),
             int(t["entry_time"]), int(t["exit_time"]),
             round(float(t["entry_price"]), 6), round(float(t["exit_price"]), 6),
             float(t["pnl"])]
            for t in result["trades"]]
    return sorted(rows, key=row_key)


def row_key(row):
    return (row[2], row[3], row[0], row[1], row[4], row[5])


def same_row(tv, engine):
    return (tv[:6] == engine[:6]
            and abs(tv[6] - engine[6]) <= max(1e-4, 1.2e-7 * abs(tv[6])))


def first_departure(tv, engine):
    for number, (a, b) in enumerate(zip(tv, engine), start=1):
        if not same_row(a, b):
            return {"trade": number, "tv": a[:6], "engine": b[:6]}
    if len(tv) != len(engine):
        number = min(len(tv), len(engine)) + 1
        return {"trade": number,
                "tv": tv[number - 1][:6] if len(tv) >= number else None,
                "engine": engine[number - 1][:6] if len(engine) >= number else None}
    return None


class TapeEntryPoints:
    """One tape's view of the shared module: its own C entry points through their
    script_rule_<root>_<tape>__ prefix, the library's own exports by their names."""

    def __init__(self, lib, prefix):
        object.__setattr__(self, "_lib", lib)
        object.__setattr__(self, "_prefix", prefix)

    def __getattr__(self, name):
        try:
            return getattr(self._lib, self._prefix + name)
        except AttributeError:
            return getattr(self._lib, name)


def prefix_for(root, name):
    return "script_rule_" + re.sub(r"[^A-Za-z0-9_]", "_", f"{root}_{name}") + "__"


def run_tapes(fixtures, root, names, lib, strategy_class, inputs_run_kwargs):
    outcomes = {}
    for name in names:
        tape = fixtures / root / name
        conf = json.loads((tape / "configuration.json").read_text())
        bars, kwargs = inputs_run_kwargs(conf, tape, tape / "bars.csv")
        result = strategy_class(lib, prefix_for(root, name)).run(bars, params=conf, **kwargs)
        if result.get("error"):
            raise AssertionError(f"{root}/{name}: run error {result['error']}")
        tv = tv_trades(tape / "tv_trades.csv")
        assert tv, f"{root}/{name}: empty TradingView tape"
        outcomes[name] = first_departure(tv, engine_trades(result))
    return outcomes


def main():
    fixtures, module, scripts = (Path(a).resolve() for a in sys.argv[1:4])
    options = sys.argv[4:]
    record = "--record" in options
    roots = [options[i + 1] for i, a in enumerate(options) if a == "--root"] or list(ROOTS)
    assert set(roots) <= set(ROOTS), f"unknown root in {roots}"
    sys.path.insert(0, str(scripts))
    from run_strategy import Strategy, _check_abi, inputs_run_kwargs  # noqa: E402

    class TapeStrategy(Strategy):
        def __init__(self, lib, prefix):  # noqa: super().__init__ loads a path
            self.lib = TapeEntryPoints(lib, prefix)
            _check_abi(self.lib)
            self._setup_signatures()

    lib = ctypes.CDLL(str(module))
    lib.script_rule_tapes_set_switch.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.script_rule_tapes_set_switch.restype = ctypes.c_int
    assert lib.script_rule_tapes_switch_count() == len(SWITCHES), \
        "the module and the test name the same switches"

    failures = []
    for root in roots:
        names = (fixtures / root / "tapes.txt").read_text().split()
        assert names and len(names) == len(set(names)), f"{root}: tape names are unique"
        for _, index in SWITCHES:
            assert lib.script_rule_tapes_set_switch(index, 1) == 1
        on = run_tapes(fixtures, root, names, lib, TapeStrategy, inputs_run_kwargs)
        equal = sum(1 for name in names if on[name] is None)
        for name in names:
            if on[name] is not None:
                failures.append(f"{root}/{name}: engine departs from TradingView at {on[name]}")
        print(f"{root}: {equal} of {len(names)} tapes equal to TradingView row for row")

        recorded_path = fixtures / root / "switch_off_departures.json"
        recorded = {} if record else json.loads(recorded_path.read_text())
        observed = {}
        for switch, index in SWITCHES:
            assert lib.script_rule_tapes_set_switch(index, 0) == 0
            try:
                off = run_tapes(fixtures, root, names, lib, TapeStrategy, inputs_run_kwargs)
            finally:
                assert lib.script_rule_tapes_set_switch(index, 1) == 1
            moved = {name: off[name] for name in names if off[name] != on[name]}
            observed[switch] = moved
            print(f"  {switch} off: {len(moved)} tapes move ({', '.join(sorted(moved))})")
            if not record and moved != recorded.get(switch):
                failures.append(f"{root}: {switch} off: recorded {recorded.get(switch)}, now {moved}")
        if not any(observed.values()):
            failures.append(f"{root}: no switch moves any tape")
        if record:
            recorded_path.write_text(json.dumps(observed, indent=1, sort_keys=True) + "\n")
            print(f"recorded {recorded_path}")

    for failure in failures:
        print("FAIL", failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
