#!/usr/bin/env python3
"""TradingView's cross-side exit tapes, replayed row for row.

tests/fixtures/cross_side_exit holds independently authored synthetic
strategies on NYSE:F 15m, exported with `lab tv --no-note`. Each `*-opp-*`
script holds a position and, on every bar it is held, places an entry order of
the OTHER side that rests and is never reached, then calls strategy.exit with
no from_entry; its `*-ctl-*` twin is the same script without that entry.
TradingView books every opp tape exactly as its control: a global exit called
while a position is held binds to that position, so its limit and stop rest on
the held side and a profit leg resolves against the held position. Every tape
directory keeps the exact Pine bytes, the export's List of Trades (UTC+8), the
run configuration (whose `bars` names the shared chart-feed rows in bars/) and
the frozen generated C++ (codegen 39545379). CMake builds every tape into one
module (tests/cross_side_exit_tapes.cpp), each tape's C entry points prefixed
cross_side_exit_<tape>__.

For every tape the engine must book TradingView's trades exactly: the same
count and, row for row, the same side, quantity, entry and exit time, and
entry and exit price. known_divergences.json names the tapes whose departure
is another rule's (each with its reason and first departing row, the same in
the opp tape and its control); for those the test requires exactly that first
departure.

The rule has a switch (ExitBindingRuleSwitches::global_exit_binds_held_position).
With it off, exactly the opp tapes rule_off_departures.json records must move,
each departing from TradingView, and no control may move. The calc_on_order_fills
opp tape must be refused by a forward stream; every other opp tape is replayed
as a stream too (strategy_stream_begin over 1 and 30 bars, then
strategy_stream_push_bar per bar, up to the feed's first weekend, which a stream
refuses because the lane's session names no days) and must book the trades of a backtest over the same bars, with
the rule on and with it off: the rule has no stream-only branch.

usage: test_cross_side_exit_tapes.py <fixtures dir> <module> <scripts dir> [--record]
  --record rewrites rule_off_departures.json from this run instead of checking it.
"""
import csv
import ctypes
import datetime
import json
import re
import sys
from pathlib import Path

STREAM_WARMUPS = (1, 30)
FORWARD_REFUSAL = "calc_on_order_fills is unsupported"


def tv_epoch_ms(text):
    value = datetime.datetime.fromisoformat(text).replace(tzinfo=datetime.timezone.utc)
    return int(value.timestamp() * 1000) - 8 * 3600000


def row_key(row):
    side, qty, entry_time, exit_time, entry_price, exit_price = row
    return (entry_time, exit_time, side, qty, entry_price, exit_price)


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
                     round(float(entry[price]), 6), round(float(exit_[price]), 6)])
    return sorted(rows, key=row_key)


def engine_trades(result):
    rows = [["long" if t["is_long"] else "short", round(float(t["qty"]), 8),
             int(t["entry_time"]), int(t["exit_time"]),
             round(float(t["entry_price"]), 6), round(float(t["exit_price"]), 6)]
            for t in result["trades"]]
    return sorted(rows, key=row_key)


def full_trades(result):
    return [(t["is_long"], t["qty"], t["entry_time"], t["exit_time"], t["entry_price"],
             t["exit_price"], t["pnl"]) for t in result["trades"]]


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
    their cross_side_exit_<tape>__ prefix, the library's own exports by name."""

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
    return "cross_side_exit_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + "__"


def run_tape(fixtures, name, lib, strategy_class, inputs_run_kwargs, *args, **extra):
    tape = fixtures / name
    conf = json.loads((tape / "configuration.json").read_text())
    bars, kwargs = inputs_run_kwargs(conf, tape, fixtures / conf["bars"])
    result = strategy_class(lib, prefix_of(name), *args).run(bars, params=conf, **kwargs, **extra)
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


def is_coof(fixtures, name):
    return "calc_on_order_fills=true" in (fixtures / name / "strategy.pine").read_text()


def gap_free_end_ms(bars_csv):
    """The last bar before the feed's first gap longer than a night (a weekend
    or a holiday). The lane's session string names no days, so a stream counts
    every day as in session and refuses that gap."""
    with bars_csv.open() as f:
        stamps = [int(row["timestamp"]) for row in csv.DictReader(f)]
    for before, after in zip(stamps, stamps[1:]):
        if after - before > 20 * 3600000:
            return before
    return stamps[-1]


def stream_departures(fixtures, names, lib, batch_class, stream_class, inputs_run_kwargs):
    failures = []
    for name in names:
        conf = json.loads((fixtures / name / "configuration.json").read_text())
        end = gap_free_end_ms(fixtures / conf["bars"])
        for on in (1, 0):
            assert lib.cross_side_exit_tapes_set_rule(on) == on
            try:
                batch = run_tape(fixtures, name, lib, batch_class, inputs_run_kwargs,
                                 ohlcv_end_ms=end)
                for warm in STREAM_WARMUPS:
                    streamed = run_tape(fixtures, name, lib, stream_class, inputs_run_kwargs, warm,
                                        ohlcv_end_ms=end)
                    if full_trades(streamed) != full_trades(batch):
                        failures.append(f"{name} stream ({warm} warm-up bars, rule "
                                        f"{'on' if on else 'off'}) books other trades than the backtest")
            finally:
                assert lib.cross_side_exit_tapes_set_rule(1) == 1
        if not batch["trades"]:
            failures.append(f"{name}: no trade before the first weekend to stream")
        print(f"  {name}: stream == backtest with the rule on and off "
              f"({', '.join(str(w) for w in STREAM_WARMUPS)} warm-up bars, {len(batch['trades'])} "
              f"trades up to the first weekend)")
    return failures


def forward_refusals(fixtures, names, lib, strategy_class, load_bars):
    failures = []
    for name in names:
        tape = fixtures / name
        conf = json.loads((tape / "configuration.json").read_text())
        api = strategy_class(lib, prefix_of(name)).lib
        bars, _, _ = load_bars(fixtures / conf["bars"])
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
        else:
            print(f"  {name}: refused by a forward stream ({FORWARD_REFUSAL})")
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

    class StreamStrategy(Strategy):
        def __init__(self, lib, prefix, warm):  # noqa: super().__init__ loads a path
            self.lib = StreamEntryPoints(lib, prefix, warm)
            _check_abi(self.lib)
            self._setup_signatures()

    lib = ctypes.CDLL(str(module))
    lib.cross_side_exit_tapes_set_rule.argtypes = [ctypes.c_int]
    lib.cross_side_exit_tapes_set_rule.restype = ctypes.c_int
    lib.strategy_stream_push_bar.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.strategy_stream_push_bar.restype = ctypes.c_int
    lib.strategy_stream_order_actions_clear.argtypes = [ctypes.c_void_p]

    names = (fixtures / "tapes.txt").read_text().split()
    known = json.loads((fixtures / "known_divergences.json").read_text())
    assert len(names) == len(set(names)), "tape names are unique"
    assert set(known) <= set(names), "known divergences name only tapes"
    opp = [n for n in names if "-opp-" in n]
    for name in opp:
        control = re.sub(r"-opp-", "-ctl-", name).replace("rlp-lim-ctl", "rlp-stp-ctl")
        assert control in names, f"{name}: its control {control} is a tape"
    failures = []

    assert lib.cross_side_exit_tapes_set_rule(1) == 1
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
    print(f"cross-side exit tapes: {equal} equal to TradingView row for row, "
          f"{len(known)} known divergences, {len(names)} tapes")

    failures += forward_refusals(fixtures, [n for n in opp if is_coof(fixtures, n)], lib,
                                 TapeStrategy, _load_bars)
    failures += stream_departures(fixtures, [n for n in opp if not is_coof(fixtures, n)], lib,
                                  TapeStrategy, StreamStrategy, inputs_run_kwargs)

    recorded_path = fixtures / "rule_off_departures.json"
    assert lib.cross_side_exit_tapes_set_rule(0) == 0
    try:
        off = run_tapes(fixtures, names, lib, TapeStrategy, inputs_run_kwargs)
    finally:
        assert lib.cross_side_exit_tapes_set_rule(1) == 1
    moved = {name: off[name] for name in names if off[name] != on[name]}
    print(f"  global_exit_binds_held_position off: {len(moved)} tapes move ({', '.join(sorted(moved))})")
    if set(moved) != set(opp):
        failures.append(f"rule off moves {sorted(moved)}, expected every opp tape {sorted(opp)}")
    for name, departure in moved.items():
        if departure is None:
            failures.append(f"{name}: equals TradingView with the rule off")
    if record:
        recorded_path.write_text(json.dumps(moved, indent=1, sort_keys=True) + "\n")
        print(f"recorded {recorded_path}")
    elif moved != json.loads(recorded_path.read_text()):
        failures.append(f"rule off: recorded {json.loads(recorded_path.read_text())}, now {moved}")

    for failure in failures:
        print("FAIL", failure)
    return 1 if failures or equal + len(known) != len(names) else 0


if __name__ == "__main__":
    sys.exit(main())
