#!/usr/bin/env python3
"""TradingView's callback and lifecycle tapes, replayed row for row.

tests/fixtures/callback_lifecycle holds independently authored synthetic
strategies, each exported by TradingView four times with identical results.
Every tape directory keeps the exact Pine bytes, the first export's List of
Trades (UTC+8), the chart-feed rows the script ran on, the run configuration
and the frozen generated C++ (codegen d9b92c7). CMake builds every tape into
one module (tests/callback_lifecycle_tapes.cpp), each tape's C entry points
prefixed callback_lifecycle_<tape>__.

For every tape the engine must book TradingView's trades exactly: the same
count and, row for row, the same side, quantity, entry and exit time, and
entry and exit price (quantities to 8 decimals: a lot-step product such as
16.032600000000002 is TradingView's 16.0326). Both sides are sorted by the
same key. PnL is not compared: TradingView's report rounds it through a
32-bit float.

known_divergences.json names the tapes whose rule this engine does not apply
yet, each with its reason and the first row where the engine departs. For
those the test requires exactly that first departure, so a change that moves
one, for better or worse, fails here until the file is updated.

Each rule has a switch (src/compat/pine/callback_lifecycle_rules.hpp). The test then
clears each switch in turn and requires exactly the departures
rule_off_departures.json records for it: the tapes that rule decides, and
nothing else. That pins the rule-to-tape map and keeps every OFF branch run.

The tapes the resting limit rule decides (rule_off_departures.json) are also
replayed forward: strategy_stream_begin over their first bar, as a live run
starts. Each is a calc_on_order_fills script, and a stream calculates on bar
close only, so it must refuse with its documented reason: the rule has no
forward half to depart from the backtest.

pair-hold-reissue (no quantity grid, so the engine takes the entry/close
pair-hold path) also has its book read after the pair's calculation: the
long's stop, re-issued after strategy.close in that calculation, must still
be held behind the pair's barrier. A re-issue that revived it would leave
the trades unchanged here (the gapped stop is deferred behind the admitted
reversal at the open either way), so the book is where the barrier shows.

usage: test_callback_lifecycle_tapes.py <fixtures dir> <module> <scripts dir> [--record]
  --record rewrites rule_off_departures.json from this run instead of
  checking it (the all-on run is still checked).
"""
import csv
import ctypes
import datetime
import json
import re
import sys
from pathlib import Path

RULES = (  # PineCallbackLifecycleRule values
    ("trail_points_mintick_tolerance", 0),
    ("declined_reversal_reissue_revives", 1),
    ("callback_limit_tick_reach", 2),
    ("resting_limit_tick_reach", 3),
)


def tv_epoch_ms(text, utc_plus_8):
    value = datetime.datetime.fromisoformat(text).replace(tzinfo=datetime.timezone.utc)
    return int(value.timestamp() * 1000) - (8 * 3600000 if utc_plus_8 else 0)


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
        rows.append([entry["Type"].split()[-1].lower(), round(float(entry["Size (qty)"]), 8),
                     tv_epoch_ms(entry["Date and time"], True),
                     tv_epoch_ms(exit_["Date and time"], True),
                     round(float(entry[price]), 6), round(float(exit_[price]), 6)])
    return sorted(rows, key=row_key)


def engine_trades(result):
    rows = [["long" if t["is_long"] else "short", round(float(t["qty"]), 8),
             int(t["entry_time"]), int(t["exit_time"]),
             round(float(t["entry_price"]), 6), round(float(t["exit_price"]), 6)]
            for t in result["trades"]]
    return sorted(rows, key=row_key)


def row_key(row):
    side, qty, entry_time, exit_time, entry_price, exit_price = row
    return (entry_time, exit_time, side, qty, entry_price, exit_price)


def first_departure(tv, engine):
    for number, (a, b) in enumerate(zip(tv, engine), start=1):
        if a != b:
            return {"trade": number, "tv": a, "engine": b}
    if len(tv) != len(engine):
        number = min(len(tv), len(engine)) + 1
        return {"trade": number,
                "tv": tv[number - 1] if len(tv) >= number else None,
                "engine": engine[number - 1] if len(engine) >= number else None}
    return None


class TapeEntryPoints:
    """One tape's view of the shared module: its own C entry points through
    their callback_lifecycle_<tape>__ prefix, the library's own exports by their names."""

    def __init__(self, lib, prefix):
        object.__setattr__(self, "_lib", lib)
        object.__setattr__(self, "_prefix", prefix)

    def __getattr__(self, name):
        try:
            return getattr(self._lib, self._prefix + name)
        except AttributeError:
            return getattr(self._lib, name)


def run_tapes(fixtures, names, lib, strategy_class, inputs_run_kwargs):
    outcomes = {}
    for name in names:
        tape = fixtures / name
        conf = json.loads((tape / "configuration.json").read_text())
        bars, kwargs = inputs_run_kwargs(conf, tape, tape / "bars.csv")
        prefix = "callback_lifecycle_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + "__"
        result = strategy_class(lib, prefix).run(bars, params=conf, **kwargs)
        if result.get("error"):
            raise AssertionError(f"{name}: run error {result['error']}")
        tv = tv_trades(tape / "tv_trades.csv")
        assert tv, f"{name}: empty TradingView tape"
        outcomes[name] = first_departure(tv, engine_trades(result))
    return outcomes


# The pair-hold barrier: tape, the pair's bar (UTC), the held exit's entry
# id and stop level (tests/fixtures/callback_lifecycle/pair-hold-reissue).
PAIR_HOLD = ("pair-hold-reissue", datetime.datetime(2025, 5, 14, 19, 45,
                                                    tzinfo=datetime.timezone.utc), "L", 10.62)


def pair_hold_barrier(fixtures, lib, strategy_class, inputs_run_kwargs):
    name, bar, from_entry, stop = PAIR_HOLD
    tape = fixtures / name
    conf = json.loads((tape / "configuration.json").read_text())
    assert "qty_step" not in conf["runtime_overrides"], "the pair-hold path needs no grid"
    bars, kwargs = inputs_run_kwargs(conf, tape, tape / "bars.csv")
    prefix = "callback_lifecycle_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + "__"
    result = strategy_class(lib, prefix).run(
        bars, params=conf, dump_book=True,
        ohlcv_end_ms=int(bar.timestamp() * 1000), **kwargs)
    rows = [o for o in result.get("pending_orders", [])
            if o.get("from_entry") == from_entry and o.get("stop_price") == stop]
    if len(rows) != 1:
        return f"{name}: expected one {from_entry} stop at {stop} after the pair, got {len(rows)}"
    row = rows[0]
    if not (row.get("dormant_bracket") == 1 and row.get("legs_suspension_hold_present") == 1):
        return (f"{name}: the stop re-issued in the pair's calculation left the barrier "
                f"(dormant_bracket {row.get('dormant_bracket')}, hold "
                f"{row.get('legs_suspension_hold_present')})")
    return None


# The rule whose tapes are replayed forward, and the reason a stream refuses them.
FORWARD_RULE = "resting_limit_tick_reach"
FORWARD_REFUSAL = "calc_on_order_fills is unsupported"


def forward_refusals(fixtures, names, lib, strategy_class, load_bars):
    failures = []
    for name in names:
        tape = fixtures / name
        if "calc_on_order_fills=true" not in (tape / "strategy.pine").read_text():
            failures.append(f"{name}: forward replay expects a calc_on_order_fills script")
            continue
        conf = json.loads((tape / "configuration.json").read_text())
        prefix = "callback_lifecycle_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + "__"
        api = strategy_class(lib, prefix).lib
        bars, _, _ = load_bars(tape / "bars.csv")
        state = api.strategy_create(json.dumps(conf).encode())
        try:
            begun = api.strategy_stream_begin(state, bars, 1, conf["input_tf"].encode(),
                                              conf["script_tf"].encode())
            error = (api.strategy_get_last_error(state) or b"").decode()
        finally:
            api.strategy_free(state)
        if begun != -1 or FORWARD_REFUSAL not in error:
            failures.append(f"{name}: forward replay began ({begun}, {error!r}); "
                            f"expected the stream to refuse it ({FORWARD_REFUSAL})")
    return failures


def main():
    fixtures, module, scripts = (Path(a).resolve() for a in sys.argv[1:4])
    record = "--record" in sys.argv[4:]
    sys.path.insert(0, str(scripts))
    from run_strategy import Strategy, _check_abi, _load_bars, inputs_run_kwargs  # noqa: E402

    class TapeStrategy(Strategy):
        def __init__(self, lib, prefix):  # noqa: super().__init__ loads a path
            self.lib = TapeEntryPoints(lib, prefix)
            _check_abi(self.lib)
            self._setup_signatures()

    lib = ctypes.CDLL(str(module))
    lib.callback_lifecycle_tapes_set_rule.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.callback_lifecycle_tapes_set_rule.restype = ctypes.c_int
    assert lib.callback_lifecycle_tapes_rule_count() == len(RULES), "the module and the test name the same rules"

    names = (fixtures / "tapes.txt").read_text().split()
    known = json.loads((fixtures / "known_divergences.json").read_text())
    assert len(names) == len(set(names)), "tape names are unique"
    assert set(known) <= set(names), "known divergences name only tapes"
    failures = []

    for _, index in RULES:
        assert lib.callback_lifecycle_tapes_set_rule(index, 1) == 1
    on = run_tapes(fixtures, names, lib, TapeStrategy, inputs_run_kwargs)
    equal = 0
    for name in names:
        expected = known.get(name)
        if expected is None:
            if on[name] is None:
                equal += 1
            else:
                failures.append(f"{name}: engine departs from TradingView at {on[name]}")
        elif on[name] != {k: expected[k] for k in ("trade", "tv", "engine")}:
            failures.append(f"{name}: known divergence moved, recorded "
                            f"{ {k: expected[k] for k in ('trade', 'tv', 'engine')} }, now {on[name]}")
    print(f"callback lifecycle tapes: {equal} equal to TradingView row for row, "
          f"{len(known)} known divergences, {len(names)} tapes")
    barrier = pair_hold_barrier(fixtures, lib, TapeStrategy, inputs_run_kwargs)
    if barrier:
        failures.append(barrier)
    else:
        print("  pair-hold-reissue: the re-issued stop stays behind the pair's barrier")

    forward = sorted(json.loads((fixtures / "rule_off_departures.json").read_text())
                     .get(FORWARD_RULE, {}))
    refused = forward_refusals(fixtures, forward, lib, TapeStrategy, _load_bars)
    failures += refused
    if forward and not refused:
        print(f"  {FORWARD_RULE}: {len(forward)} tapes replayed forward, each refused by the "
              f"stream ({FORWARD_REFUSAL})")
    elif not forward:
        failures.append(f"{FORWARD_RULE}: no tape to replay forward")

    recorded_path = fixtures / "rule_off_departures.json"
    recorded = {} if record else json.loads(recorded_path.read_text())
    observed = {}
    for rule, index in RULES:
        assert lib.callback_lifecycle_tapes_set_rule(index, 0) == 0
        try:
            off = run_tapes(fixtures, names, lib, TapeStrategy, inputs_run_kwargs)
        finally:
            assert lib.callback_lifecycle_tapes_set_rule(index, 1) == 1
        moved = {name: off[name] for name in names if off[name] != on[name]}
        observed[rule] = moved
        print(f"  {rule} off: {len(moved)} tapes move ({', '.join(sorted(moved))})")
        if not moved:
            failures.append(f"{rule} off moves no tape")
        if not record and moved != recorded.get(rule):
            failures.append(f"{rule} off: recorded {recorded.get(rule)}, now {moved}")
    if record:
        recorded_path.write_text(json.dumps(observed, indent=1, sort_keys=True) + "\n")
        print(f"recorded {recorded_path}")

    for failure in failures:
        print("FAIL", failure)
    return 1 if failures or equal + len(known) != len(names) else 0


if __name__ == "__main__":
    sys.exit(main())
