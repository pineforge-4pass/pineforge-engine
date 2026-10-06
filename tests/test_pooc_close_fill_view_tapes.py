#!/usr/bin/env python3
"""TradingView's POOC same-pass position-view tapes, replayed row for row.

tests/fixtures/pooc_close_fill_view holds independently authored synthetic
strategies on BINANCE:ETHUSDT.P 15m, exported with `lab tv --no-note` (two
byte-identical exports each). Under process_orders_on_close and pyramiding=0,
each holds a one-lot short (a long in one mirror) and, on 2026-01-08 23:15
UTC, ends it with an order that fills at that bar's close: a strategy.exit
stop re-issued through the close, a first-issue one, strategy.close or
strategy.close_all. Right after that call the script reads
strategy.position_size or strategy.position_avg_price, or observes the exit
with strategy.position_size[1] < 0 and strategy.position_size == 0 behind an
exit cooldown, and the 23:30 entry encodes what it read. TradingView fills
the order at the close, after the pass: the pass keeps the pre-fill size and
average. Every tape directory keeps the exact Pine bytes, the export's List of
Trades (UTC+8), the run configuration (whose `bars` names the chart-feed rows
in bars/) and the frozen generated C++ (codegen c1df8a6). CMake builds every
tape into one module (tests/pooc_close_fill_view_tapes.cpp), each tape's C
entry points prefixed pooc_close_fill_view_<tape>__.

For every tape the engine must book TradingView's trades exactly: the same
count and, row for row, the same side, quantity, entry and exit time, entry
and exit price and entry id. known_divergences.json names the tapes whose
departure is a read this rule does not serve (each with its reason and first
departing row); for those the test requires exactly that first departure,
with the switches on and with each part cleared.

The rule has two switches (PoocCloseFillViewSwitches): the re-issued exit the
adapter fills at its call keeps the script's position view
(current_exit_keeps_position_view) and its average price
(current_exit_keeps_average_price). switch_off_departures.json records, per
setting, the tapes that move with a part cleared and their first departing
row; each must depart from TradingView, and no other tape may move. Every
tape is replayed as a forward stream too (strategy_stream_begin over 1 and 30
bars, then strategy_stream_push_bar per bar) and must book the trades of a
backtest over the same bars, with both parts on and with both off: the rule
has no stream-only branch.

The average part writes the pre-fill average into the flat book for the rest
of one pass; release_script_average_price() restores 0 right after the script,
before the bar's broker state is folded. position_entry_price_ is part of the
broker-state hash, so every tape is also run with the per-bar hash recorded,
the average part on and off: the hashes must be identical wherever the trades
are, and otherwise first differ after the exit bar's fold. A hold that outlived
its pass moves the exit bar's fold.

usage: test_pooc_close_fill_view_tapes.py <fixtures dir> <module> <scripts dir> [--record]
  --record rewrites switch_off_departures.json from this run instead of checking it.
"""
import csv
import ctypes
import datetime
import json
import re
import sys
from pathlib import Path

STREAM_WARMUPS = (1, 30)
# (current_exit_keeps_position_view, current_exit_keeps_average_price)
SETTINGS = {"average off": (1, 0), "view off": (0, 1), "both off": (0, 0)}
# The re-issued short exit through the close is the only call the adapter
# fills at the call without a freeze; the long mirror, strategy.close,
# strategy.close_all and a first-issue exit never move.
REISSUED_SHORT = {"short-reissued-exit-size", "short-reissued-exit-cooldown",
                  "atr-exit-cooldown-pyramiding-0", "short-reissued-exit-average",
                  "short-reissued-limit-exit"}
EXPECTED_MOVERS = {"average off": {"short-reissued-exit-average", "short-reissued-limit-exit"},
                   "view off": REISSUED_SHORT, "both off": REISSUED_SHORT}
# 2026-01-08 23:15 UTC: every tape's order fills at this bar's close.
EXIT_BAR_MS = 1767914100000


def tv_epoch_ms(text):
    value = datetime.datetime.fromisoformat(text).replace(tzinfo=datetime.timezone.utc)
    return int(value.timestamp() * 1000) - 8 * 3600000


def row_key(row):
    side, qty, entry_time, exit_time, entry_price, exit_price, entry_id = row
    return (entry_time, exit_time, side, qty, entry_price, exit_price, entry_id)


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
                     tv_epoch_ms(entry["Date and time"]), tv_epoch_ms(exit_["Date and time"]),
                     round(float(entry[price]), 6), round(float(exit_[price]), 6),
                     entry["Signal"]])
    return sorted(rows, key=row_key)


def engine_trades(result):
    rows = [["long" if t["is_long"] else "short", round(float(t["qty"]), 8),
             int(t["entry_time"]), int(t["exit_time"]),
             round(float(t["entry_price"]), 6), round(float(t["exit_price"]), 6),
             t.get("entry_id", "")]
            for t in result["trades"]]
    return sorted(rows, key=row_key)


def full_trades(result):
    return [(t["is_long"], t["qty"], t["entry_time"], t["exit_time"], t["entry_price"],
             t["exit_price"], t["pnl"], t.get("entry_id", "")) for t in result["trades"]]


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
    their pooc_close_fill_view_<tape>__ prefix, the library's own exports by
    name."""

    def __init__(self, lib, prefix):
        object.__setattr__(self, "_lib", lib)
        object.__setattr__(self, "_prefix", prefix)

    def __getattr__(self, name):
        try:
            return getattr(self._lib, self._prefix + name)
        except AttributeError:
            return getattr(self._lib, name)


class StreamEntryPoints(TapeEntryPoints):
    """The same entry points with the backtest replaced by a stream of the same
    bars: strategy_stream_begin with `warm` bars of history, one
    strategy_stream_push_bar per later bar, then strategy_stream_fill_report."""

    def __init__(self, lib, prefix, warm):
        super().__init__(lib, prefix)
        object.__setattr__(self, "_warm", warm)

    def __getattr__(self, name):
        if name != "run_backtest_full":
            return super().__getattr__(name)
        lib, warm = self._lib, self._warm

        def stream(state, bars, n, input_tf, script_tf, _mag_on, _mag_samples, _mag_dist, report):
            bar_type = bars._type_
            size = ctypes.sizeof(bar_type)
            base = ctypes.addressof(bars)

            def at(index):
                return ctypes.cast(ctypes.c_void_p(base + index * size), ctypes.POINTER(bar_type))

            if lib.strategy_stream_begin(state, at(0), warm, input_tf, script_tf or input_tf) != 0:
                raise AssertionError(f"stream begin refused: {lib.strategy_get_last_error(state)}")
            for index in range(warm, n):
                if lib.strategy_stream_push_bar(state, at(index)) != 0:
                    raise AssertionError(
                        f"stream push refused at bar {index}: {lib.strategy_get_last_error(state)}")
                lib.strategy_stream_order_actions_clear(state)
            lib.strategy_stream_fill_report(state, report)

        return stream


def prefix_of(name):
    return "pooc_close_fill_view_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + "__"


def set_switches(lib, view, average):
    assert lib.pooc_close_fill_view_tapes_set_switches(view, average) == view * 2 + average


def run_tape(fixtures, name, lib, strategy_class, inputs_run_kwargs, *args):
    tape = fixtures / name
    conf = json.loads((tape / "configuration.json").read_text())
    bars, kwargs = inputs_run_kwargs(conf, tape, fixtures / conf["bars"])
    result = strategy_class(lib, prefix_of(name), *args).run(bars, params=conf, **kwargs)
    if result.get("error"):
        raise AssertionError(f"{name}: run error {result['error']}")
    return result


def run_tapes(fixtures, names, lib, strategy_class, inputs_run_kwargs):
    outcomes = {}
    for name in names:
        result = run_tape(fixtures, name, lib, strategy_class, inputs_run_kwargs)
        tv = tv_trades(fixtures / name / "tv_trades.csv")
        assert tv, f"{name}: empty TradingView tape"
        outcomes[name] = first_departure(tv, engine_trades(result))
    return outcomes


def stream_departures(fixtures, names, lib, batch_class, stream_class, inputs_run_kwargs):
    failures = []
    for name in names:
        for view, average in ((1, 1), (0, 0)):
            set_switches(lib, view, average)
            try:
                batch = run_tape(fixtures, name, lib, batch_class, inputs_run_kwargs)
                for warm in STREAM_WARMUPS:
                    streamed = run_tape(fixtures, name, lib, stream_class, inputs_run_kwargs, warm)
                    if full_trades(streamed) != full_trades(batch):
                        failures.append(f"{name} stream ({warm} warm-up bars, switches "
                                        f"{'on' if view else 'off'}) books other trades than "
                                        f"the backtest")
            finally:
                set_switches(lib, 1, 1)
            if not batch["trades"]:
                failures.append(f"{name}: no trade to stream")
        print(f"  {name}: stream == backtest with both parts on and off "
              f"({', '.join(str(w) for w in STREAM_WARMUPS)} warm-up bars)")
    return failures


def exit_bar_index(bars_csv):
    with bars_csv.open(encoding="utf-8") as f:
        times = [int(row["timestamp"]) for row in csv.DictReader(f)]
    return times.index(EXIT_BAR_MS)


def hold_release_departures(fixtures, names, lib, strategy_class, inputs_run_kwargs):
    """Per tape, the per-bar broker-state hashes with the average part on and
    off (the view part on): identical where the trades are equal, and otherwise
    first different after the exit bar's fold."""
    failures = []
    equal = after = 0
    for name in names:
        tape = fixtures / name
        conf = json.loads((tape / "configuration.json").read_text())
        exit_bar = exit_bar_index(fixtures / conf["bars"])
        runs = {}
        for average in (1, 0):
            set_switches(lib, 1, average)
            try:
                bars, kwargs = inputs_run_kwargs(conf, tape, fixtures / conf["bars"])
                result = strategy_class(lib, prefix_of(name)).run(
                    bars, params=conf, broker_state_hash_recording=True, **kwargs)
            finally:
                set_switches(lib, 1, 1)
            if result.get("error"):
                raise AssertionError(f"{name}: run error {result['error']}")
            runs[average] = (result["broker_state_hash"], full_trades(result))
        (on_hashes, on_trades), (off_hashes, off_trades) = runs[1], runs[0]
        if not on_hashes or len(on_hashes) != len(off_hashes) or len(on_hashes) <= exit_bar:
            failures.append(f"{name}: per-bar broker-state hashes not recorded "
                            f"({len(on_hashes)} and {len(off_hashes)} folds)")
            continue
        first = next((i for i, (a, b) in enumerate(zip(on_hashes, off_hashes)) if a != b), None)
        if first is None:
            equal += 1
        elif on_trades == off_trades:
            failures.append(f"{name}: average part on and off book the same trades, but the "
                            f"broker-state hash differs from fold {first}: the held average "
                            f"outlived its pass")
        elif first <= exit_bar:
            failures.append(f"{name}: average part on and off: the broker-state hash differs "
                            f"from fold {first}, at or before the exit bar's fold {exit_bar}")
        else:
            after += 1
    print(f"  average part on and off: per-bar broker-state hashes equal on {equal} tapes; "
          f"on {after} they first differ after the exit bar's fold, with the trades")
    return failures


def main():
    fixtures, module, scripts = (Path(a).resolve() for a in sys.argv[1:4])
    record = "--record" in sys.argv[4:]
    sys.path.insert(0, str(scripts))
    from run_strategy import Strategy, _check_abi, inputs_run_kwargs  # noqa: E402

    class TapeStrategy(Strategy):
        def __init__(self, lib, prefix):  # noqa: super().__init__ loads a path
            self.lib = TapeEntryPoints(lib, prefix)
            _check_abi(self.lib)
            self._setup_signatures()

    class StreamStrategy(Strategy):
        def __init__(self, lib, prefix, warm):  # noqa: super().__init__ loads a path
            self.lib = StreamEntryPoints(lib, prefix, warm)
            _check_abi(self.lib)
            self._setup_signatures()

    lib = ctypes.CDLL(str(module))
    lib.pooc_close_fill_view_tapes_set_switches.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.pooc_close_fill_view_tapes_set_switches.restype = ctypes.c_int
    lib.strategy_stream_push_bar.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.strategy_stream_push_bar.restype = ctypes.c_int
    lib.strategy_stream_order_actions_clear.argtypes = [ctypes.c_void_p]

    names = (fixtures / "tapes.txt").read_text().split()
    assert len(names) == len(set(names)), "tape names are unique"
    failures = []

    known = json.loads((fixtures / "known_divergences.json").read_text())
    assert set(known) <= set(names), "known divergences name only tapes"
    assert not set(known) & set().union(*EXPECTED_MOVERS.values()), "a pinned tape is no known divergence"

    set_switches(lib, 1, 1)
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
    print(f"POOC close-fill view tapes: {equal} equal to TradingView row for row, "
          f"{len(known)} known divergences, {len(names)} tapes")

    failures += stream_departures(fixtures, names, lib, TapeStrategy, StreamStrategy,
                                  inputs_run_kwargs)

    recorded_path = fixtures / "switch_off_departures.json"
    moved_by_setting = {}
    for setting, (view, average) in SETTINGS.items():
        set_switches(lib, view, average)
        try:
            off = run_tapes(fixtures, names, lib, TapeStrategy, inputs_run_kwargs)
        finally:
            set_switches(lib, 1, 1)
        moved = {name: off[name] for name in names if off[name] != on[name]}
        print(f"  {setting}: {len(moved)} tapes move ({', '.join(sorted(moved))})")
        if set(moved) != EXPECTED_MOVERS[setting]:
            failures.append(f"{setting} moves {sorted(moved)}, expected "
                            f"{sorted(EXPECTED_MOVERS[setting])}")
        for name, departure in moved.items():
            if departure is None:
                failures.append(f"{setting}: {name} equals TradingView with the part cleared")
        moved_by_setting[setting] = moved
    if record:
        recorded_path.write_text(json.dumps(moved_by_setting, indent=1, sort_keys=True) + "\n")
        print(f"recorded {recorded_path}")
    else:
        recorded = json.loads(recorded_path.read_text())
        for setting in SETTINGS:
            if moved_by_setting[setting] != recorded.get(setting):
                failures.append(f"{setting}: recorded {recorded.get(setting)}, "
                                f"now {moved_by_setting[setting]}")

    failures += hold_release_departures(fixtures, names, lib, TapeStrategy, inputs_run_kwargs)

    for failure in failures:
        print("FAIL", failure)
    return 1 if failures or equal + len(known) != len(names) else 0


if __name__ == "__main__":
    sys.exit(main())
