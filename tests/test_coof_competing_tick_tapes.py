#!/usr/bin/env python3
"""TradingView's calc_on_order_fills competing-order tapes, replayed row for row.

tests/fixtures/coof_competing_tick holds independently authored synthetic
strategies on NYSE:F 15m, exported with `lab tv --no-note` (two exports per
script, byte-identical). Each script holds one lot for at most two bars per
event: a market entry fills at the open of the event's fill bar, and a
strategy.exit stop or limit at a level chosen from that bar's own extreme
works from the entry's fill recalculation (or, in the `g` and `w` families,
from the signal bar) until the bar after; then the script closes the
position at market. The book shape varies beside the exit: alone (`k1`), an
opposite-side entry stop never reached (`k2o`), a strategy.order never
reached (`k2r`), a global exit beside an opposite entry (`k2g`), a second
full exit of the entry called after (`k2x`) or before (`k2y`) it, two live
50% exits (`k2z`). TradingView books every exit as with no other order in
the book: there is no "competing chart tick" exclusion, for on-grid levels
(either binary spelling of the ladder point, touched or crossed, absolute or
relative) and off-grid levels (inside a cent extreme, or between a half-cent
raw extreme and its chart tick) alike. Every tape directory keeps the exact
Pine bytes, the export's List of Trades (UTC+8), the run configuration (whose
`bars` names the shared chart-feed rows in bars/) and the frozen generated
C++ (codegen d1220bb). CMake builds every tape into one module
(tests/coof_competing_tick_tapes.cpp), each tape's C entry points prefixed
coof_competing_tick_<tape>__.

For every tape the engine must book TradingView's trades exactly: the same
count and, row for row, the same side, quantity, entry and exit time, and
entry and exit price. TradingView's own tapes are checked too: every `k2o`,
`k2r`, `k2g` and `k2x` tape is byte-identical to its `k1` control, and the
competing calc_on_order_fills tapes cover every event class of
events.json on both sides, on limit and stop legs, and in both years. The
calc_on_order_fills tapes must be refused by a forward stream; every other
tape is replayed as a stream too, one stream per run of the feed between two
nights (a stream refuses a weekend or a holiday, because the lane's session
names no days), each over 1 and (in a run longer than that) 30 bars of
history, and must book the trades of a backtest over the same bars.

usage: test_coof_competing_tick_tapes.py <fixtures dir> <module> <scripts dir>
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
NIGHT_MS = 20 * 3600000
PAIRED_BOOKS = ("k2o", "k2r", "k2g", "k2x")
# The event classes of events.json each competing tape family must cover:
# on-grid levels (T/C: touched or crossed cent levels, m = k * 0.01 != k / 100,
# o = read on the grid either way, p = the k * syminfo.mintick spelling; Hm/Ho
# the outward tick of a half-cent raw extreme; Rm/Ro relative legs) and
# off-grid levels (Ou/Ol 0.3/0.7 tick inside a cent extreme; Hx between a
# half-cent raw extreme and its chart tick, Hi just inside it).
FAMILY_CLASSES = {
    "f": {"Tm", "Tp", "To", "Cm", "Co", "Ou", "Ol"},
    "g": {"Tm", "Tp", "To", "Cm", "Co", "Ou", "Ol"},
    "r": {"Rm", "Ro"},
    "h": {"Hx", "Hi", "Hm", "Ho"},
    "y": {"Tm", "Tp", "To", "Cm", "Co", "Ou", "Ol"},
    "z": {"Hx", "Hi", "Hm", "Ho"},
}


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
    their coof_competing_tick_<tape>__ prefix, the library's own exports by
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
    return "coof_competing_tick_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + "__"


def run_tape(fixtures, name, lib, strategy_class, inputs_run_kwargs, *args, **bounds):
    tape = fixtures / name
    conf = json.loads((tape / "configuration.json").read_text())
    bars, kwargs = inputs_run_kwargs(conf, tape, fixtures / conf["bars"])
    kwargs.update(bounds)
    result = strategy_class(lib, prefix_of(name), *args).run(bars, params=conf, **kwargs)
    if result.get("error"):
        raise AssertionError(f"{name}: run error {result['error']}")
    return result


def is_coof(fixtures, name):
    return "calc_on_order_fills=true" in (fixtures / name / "strategy.pine").read_text()


def control_of(name):
    family, side, leg, _book, calc = name.split("-")
    return f"{family}-{side}-{leg}-k1-{calc}"


def events_key(name):
    family, side, leg = name.split("-")[:3]
    if family in ("f", "g"):
        return f"{side.upper()}-{leg}"
    return f"{family.upper()}-{side.upper()}-{leg}"


def coverage_failures(fixtures, names):
    """Every competing calc_on_order_fills family covers its event classes on
    both sides, on limit and stop legs."""
    events = json.loads((fixtures / "events.json").read_text())
    covered = {}
    for name in names:
        family, side, leg, book, _calc = name.split("-")
        if book == "k1" or not is_coof(fixtures, name) or family not in FAMILY_CLASSES:
            continue
        classes = {event["cls"] for event in events[events_key(name)]}
        for cls in classes:
            covered.setdefault((family, cls), set()).add((side, leg))
    failures = []
    for family, classes in FAMILY_CLASSES.items():
        for cls in sorted(classes):
            cells = covered.get((family, cls), set())
            if {side for side, _ in cells} != {"l", "s"} or {leg for _, leg in cells} != {"lim", "stp"}:
                failures.append(f"family {family} class {cls}: competing tapes cover only {sorted(cells)}")
    print(f"  competing calc_on_order_fills tapes cover {sum(len(c) for c in FAMILY_CLASSES.values())} "
          f"family x class cells on both sides and both legs")
    return failures


def gap_free_runs(bars_csv):
    """The feed's runs of bars between two gaps longer than a night (a weekend
    or a holiday), as (first timestamp, last timestamp, bar count). The lane's
    session string names no days, so a stream counts every day as in session
    and refuses such a gap."""
    with bars_csv.open() as f:
        stamps = [int(row["timestamp"]) for row in csv.DictReader(f)]
    runs, start = [], 0
    for index in range(1, len(stamps) + 1):
        if index == len(stamps) or stamps[index] - stamps[index - 1] > NIGHT_MS:
            runs.append((stamps[start], stamps[index - 1], index - start))
            start = index
    return runs


def stream_departures(fixtures, names, lib, batch_class, stream_class, inputs_run_kwargs):
    failures = []
    for name in names:
        conf = json.loads((fixtures / name / "configuration.json").read_text())
        runs = gap_free_runs(fixtures / conf["bars"])
        trades = 0
        for first, last, count in runs:
            batch = run_tape(fixtures, name, lib, batch_class, inputs_run_kwargs,
                             ohlcv_start_ms=first, ohlcv_end_ms=last)
            for warm in (w for w in STREAM_WARMUPS if w < count):
                streamed = run_tape(fixtures, name, lib, stream_class, inputs_run_kwargs,
                                    warm, ohlcv_start_ms=first, ohlcv_end_ms=last)
                if full_trades(streamed) != full_trades(batch):
                    failures.append(f"{name} stream of the run from {first} ({warm} warm-up bars) "
                                    f"books other trades than the backtest")
            trades += len(batch["trades"])
        if not trades:
            failures.append(f"{name}: no trade to stream")
        print(f"  {name}: stream == backtest over {len(runs)} runs of the feed "
              f"({', '.join(str(w) for w in STREAM_WARMUPS)} warm-up bars where the run is "
              f"longer, {trades} trades)")
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
    print(f"  {len(names)} calc_on_order_fills tapes refused by a forward stream "
          f"({FORWARD_REFUSAL})")
    return failures


def main():
    fixtures, module, scripts = (Path(a).resolve() for a in sys.argv[1:4])
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
    lib.strategy_stream_push_bar.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.strategy_stream_push_bar.restype = ctypes.c_int
    lib.strategy_stream_order_actions_clear.argtypes = [ctypes.c_void_p]

    names = (fixtures / "tapes.txt").read_text().split()
    assert len(names) == len(set(names)), "tape names are unique"
    failures = []

    # TradingView: every single-quantity competing tape books as its control.
    paired = [n for n in names if n.split("-")[3] in PAIRED_BOOKS]
    for name in paired:
        control = control_of(name)
        assert control in names, f"{name}: its control {control} is a tape"
        if (fixtures / name / "tv_trades.csv").read_bytes() \
                != (fixtures / control / "tv_trades.csv").read_bytes():
            failures.append(f"{name}: TradingView's tape differs from its control {control}'s")
    failures += coverage_failures(fixtures, names)

    equal = 0
    rows = 0
    for name in names:
        result = run_tape(fixtures, name, lib, TapeStrategy, inputs_run_kwargs)
        tv = tv_trades(fixtures / name / "tv_trades.csv")
        assert tv, f"{name}: empty TradingView tape"
        departure = first_departure(tv, engine_trades(result))
        if departure is None:
            equal += 1
            rows += len(tv)
        else:
            failures.append(f"{name}: engine departs from TradingView at {departure}")
    print(f"coof competing-tick tapes: {equal} of {len(names)} equal to TradingView row for row "
          f"({rows} trades; {len(paired)} competing tapes byte-identical to their control on "
          f"TradingView)")

    coof = [n for n in names if is_coof(fixtures, n)]
    failures += forward_refusals(fixtures, coof, lib, TapeStrategy, _load_bars)
    failures += stream_departures(fixtures, [n for n in names if n not in coof], lib,
                                  TapeStrategy, StreamStrategy, inputs_run_kwargs)

    for failure in failures:
        print("FAIL", failure)
    return 1 if failures or equal != len(names) else 0


if __name__ == "__main__":
    sys.exit(main())
