"""Compare the real SMA caller with externally approved reconstruction tapes.

The case JSON names source/bars and one TV CSV/metadata pair per captured
offset form by path and SHA256, plus their actually applied settings and an
approval reference. Paths are relative to that JSON. No tape, expected
quantity, or approval is generated.

All three offset forms are replayed. A form with a tape is compared with it
row by row. A form without one has NOT been measured against TradingView: it
is reported as a native control only (its lots against the exact floor where
the exact quantity is off every lot boundary) and is never listed as compared.

Exit 1 is a completed comparison with differences; exit 2 is incomplete input
or execution. The order comments carry TradingView's own average, previous
average and requested quantity as exact doubles: they are decoded and compared
with the native words bar for bar and reported as `signal`, a diagnostic that
localises a difference and never decides the result.
"""
import argparse
import csv
import datetime as dt
from decimal import Decimal, InvalidOperation, ROUND_HALF_EVEN, ROUND_HALF_UP
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys

SOURCE_SHA = "20aa1262e9b81ca07a94f168b6afbc2ab796aeb6626e6f2872b00438437d822c"
FEED_SHA = "6394e457aa1774f7ed8fecf047126136501460ea973312d5f73ea66288a486bf"
FIRST, HOUR, SAMPLES = 1704067200000, 3600000, 1441
STEP = Decimal("0.00001")
OFFSETS = ("0", "-0.25", "0.25")
NAMES = {"-0.25": "minus", "0": "zero", "0.25": "plus"}
SLOTS = tuple(i for i in range(SAMPLES) if i >= 2 and i % 3 == 2 and i + 2 < SAMPLES)
APPLIED = {
    "symbol": "BINANCE:BTCUSDT", "input_tf": "60", "script_tf": "60",
    "first_time": FIRST, "sample_count": SAMPLES, "feed_origin": FIRST,
    "sma_length": 3, "source_gate": "inWindow ? close : na",
    "qty_step": "0.00001", "quote_currency": "USDT", "account_fx": "1",
    "initial_capital": "1000000000", "pyramiding": 0,
    "default_qty_type": "fixed", "default_qty_value": "1",
    "commission_type": "percent", "commission_value": "0", "slippage": 0,
    "margin_long": "100", "margin_short": "100",
    "process_orders_on_close": False, "calc_on_order_fills": False,
    "calc_on_every_tick": False, "use_bar_magnifier": False,
}
# The script's order comment: command time and bar, then the average, the
# previous average and the requested quantity, each as `whole:fraction` with
# the value whole + fraction / 2**52, or `unsupported:<text>`.
SIGNAL = re.compile(r"i731\|t=(\d+)\|b=(\d+)\|s=([^|]+)\|s1=([^|]+)\|q=([^|]+)")
SIGNAL_WORDS = ("sma", "previous_sma", "request")


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bound_file(root, item):
    path = (root / item["path"]).resolve()
    require(re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) is not None,
            "invalid input digest")
    require(digest(path) == item["sha256"], f"input digest differs: {path}")
    return path


def decimal(text):
    value = Decimal(text)
    require(value.is_finite(), f"nonfinite decimal: {text}")
    return value


def binary(word):
    require(isinstance(word, str) and re.fullmatch(r"[0-9a-f]{16}", word) is not None,
            "invalid native binary64 word")
    return struct.unpack(">d", bytes.fromhex(word))[0]


def word_of(value):
    return struct.pack(">d", value).hex()


def native_decimal(word):
    value = binary(word)
    require(math.isfinite(value), "nonfinite native trade value")
    # Preserve the shortest round-trip decimal; do not snap either quantity.
    return Decimal(repr(value))


def lot_steps(quantity):
    """A quantity as whole lot steps and what is left of it beside the grid."""
    lots = int((quantity / STEP).to_integral_value(rounding=ROUND_HALF_EVEN))
    return lots, quantity - lots * STEP


def dyadic(text):
    """Decode one `whole:fraction` field to its exact double; NaN when the
    script wrote `unsupported:NaN`. Any other spelling is an input error."""
    if text == "unsupported:NaN":
        return math.nan
    match = re.fullmatch(r"(\d+):(\d+)", text)
    require(match is not None, f"unreadable signal value: {text}")
    whole, fraction = int(match[1]), int(match[2])
    require(whole >= 1 and fraction < 2 ** 52, f"signal value out of range: {text}")
    value = float(whole) + math.ldexp(float(fraction), -52)
    # The text must be this double's own encoding, or a digit was lost.
    require(int(value) == whole and int((value - whole) * 2.0 ** 52) == fraction,
            f"signal value is not an exact double: {text}")
    return value


def signal(text):
    match = SIGNAL.fullmatch(text)
    require(match is not None, f"unreadable signal: {text}")
    return {"time": int(match[1]), "bar": int(match[2]), "sma": dyadic(match[3]),
            "previous_sma": dyadic(match[4]), "request": dyadic(match[5])}


def same_word(native_word, value):
    """A native binary64 word against a decoded signal value, bit for bit;
    two NaNs are the same missing value."""
    native = binary(native_word)
    if math.isnan(value) or math.isnan(native):
        return math.isnan(value) and math.isnan(native)
    return native_word == word_of(value)


def timestamp(text):
    zone = dt.timezone(dt.timedelta(hours=8))
    value = dt.datetime.strptime(text, "%Y-%m-%d %H:%M").replace(tzinfo=zone)
    return int(value.timestamp()) * 1000


def feed(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == ["timestamp", "open", "high", "low", "close", "volume"],
                "feed columns differ")
        rows = list(reader)
    require(len(rows) == SAMPLES, "feed history length differs")
    for i, row in enumerate(rows):
        require(int(row["timestamp"]) == FIRST + i * HOUR, "feed history/gap differs")
        o, h, lo, c, v = (decimal(row[key]) for key in
                          ("open", "high", "low", "close", "volume"))
        require(0 < lo <= min(o, c) <= max(o, c) <= h and v >= 0, "invalid OHLCV")
    return rows


def tv_trades(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        required = {"Trade number", "Type", "Date and time", "Signal",
                    "Price USDT", "Size (qty)", "Net PnL USDT", "Commission USDT"}
        require(reader.fieldnames is not None and required <= set(reader.fieldnames),
                "TV quote-currency columns missing")
        rows = list(reader)
    require(len(rows) == 2 * len(SLOTS), "TV population incomplete or extra")
    pairs = {}
    for row in rows:
        require(None not in row and all(value is not None for value in row.values()),
                "malformed TV row")
        number = int(row["Trade number"])
        require(1 <= number <= len(SLOTS), "unexpected TV trade number")
        kind = row["Type"]
        require(kind in ("Entry long", "Exit long"), "unexpected/open TV row")
        pair = pairs.setdefault(number, {})
        require(kind not in pair, "duplicate TV leg")
        pair[kind] = row
    require(set(pairs) == set(range(1, len(SLOTS) + 1)), "missing TV trade number")
    for number, pair in pairs.items():
        require(set(pair) == {"Entry long", "Exit long"}, "missing TV leg")
        entry, exit_row = pair["Entry long"], pair["Exit long"]
        slot = SLOTS[number - 1]
        require(timestamp(entry["Date and time"]) == FIRST + (slot + 1) * HOUR
                and timestamp(exit_row["Date and time"]) == FIRST + (slot + 2) * HOUR,
                "TV schedule differs from source")
        quantity = decimal(entry["Size (qty)"])
        require(quantity > 0 and quantity == decimal(exit_row["Size (qty)"]),
                "invalid or inconsistent TV quantity")
        require(quantity % STEP == 0, "TV quantity is off the applied grid")
        for leg in (entry, exit_row):
            require(decimal(leg["Price USDT"]) > 0, "invalid TV price")
            require(decimal(leg["Commission USDT"]) == 0, "TV commission differs")
        require(decimal(entry["Net PnL USDT"]) == decimal(exit_row["Net PnL USDT"]),
                "TV leg PnL differs")
    return pairs


def offset_cases(items):
    """The captured forms: one to three, each offset once. A form that is not
    listed has no tape; it is replayed natively and never called measured."""
    forms = {}
    for item in items:
        offset = item["offset_steps"]
        require(offset in OFFSETS and offset not in forms, "unknown or duplicate offset form")
        require(item["applied"] == dict(APPLIED, offset_steps=offset),
                f"applied settings/history/NA gate differ: {offset}")
        forms[offset] = item
    require(forms, "at least one captured offset form is required")
    return forms


def load_case(path):
    case = json.loads(path.read_text(encoding="utf-8"))
    require(case["role"] == "RECONSTRUCTION", "case must be labelled RECONSTRUCTION")
    require(isinstance(case["approval_reference"], str) and case["approval_reference"].strip(),
            "missing external source/settings/TV approval reference")
    root = path.parent
    source = bound_file(root, case["source"])
    bars = bound_file(root, case["bars"])
    require(case["source"]["sha256"] == SOURCE_SHA, "reconstructed source identity differs")
    require(case["bars"]["sha256"] == FEED_SHA, "retained reconstruction history differs")
    require(digest(source) == SOURCE_SHA, "source changed")
    price_rows = feed(bars)
    tapes = {}
    for offset, item in offset_cases(case["tv"]).items():
        tape = bound_file(root, item["csv"])
        metadata_path = bound_file(root, item["metadata"])
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        require(metadata["pine_sha256"] == SOURCE_SHA
                and metadata["tv_trades_sha256"] == item["csv"]["sha256"],
                "TV metadata source/tape binding differs")
        require(metadata["tv_trades_rows"] == 2 * len(SLOTS), "metadata population differs")
        require(metadata["chart"]["symbol"] == "BINANCE:BTCUSDT"
                and metadata["chart"]["interval"] == "60", "metadata market differs")
        tapes[offset] = tv_trades(tape)
    return case, bars, price_rows, tapes


def native_rows(text, offset):
    rows = [json.loads(line) for line in text.splitlines()]
    require(all(row.get("kind") in {"bar", "trade", "event", "summary"} for row in rows),
            "unknown native record")
    summaries = [row for row in rows if row["kind"] == "summary"]
    require(len(summaries) == 1 and rows[-1] == summaries[0], "native summary incomplete")
    summary = summaries[0]
    require(summary["status"] == 0 and summary["error"] == "", "native run failed")
    require(binary(summary["grid"]) == float(STEP)
            and binary(summary["offset"]) == float(offset)
            and binary(summary["capital"]) == 1e9
            and binary(summary["account_fx"]) == 1
            and summary["quote_currency"] == "USDT", "native configuration differs")
    bars = [row for row in rows if row["kind"] == "bar"]
    trades = [row for row in rows if row["kind"] == "trade"]
    events = [row for row in rows if row["kind"] == "event"]
    require(len(bars) == SAMPLES and summary["bars"] == SAMPLES,
            "native bar population differs")
    for i, row in enumerate(bars):
        require((row["index"], row["local"], row["time"]) == (i, i, FIRST + i * HOUR)
                and row["in_window"] is True, "native history/window differs")
        for key in ("sma", "previous_sma", "request", "position", "placement_qty"):
            binary(row[key])
        require(math.isnan(binary(row["sma"])) if i < 2 else
                math.isfinite(binary(row["sma"])), "native SMA warmup differs")
    require(summary["trades"] == len(trades), "native trade export incomplete")
    require([row["index"] for row in trades] == list(range(len(trades))),
            "native trade indices differ")
    require(events and all(events[i]["ordinal"] < events[i + 1]["ordinal"]
                           for i in range(len(events) - 1)), "native event history incomplete")
    return bars, trades, summary, events


def exact_lots(prices, slot, offset):
    """The exact quantity of a command in lot steps, from the feed's decimal
    closes. A classification, never an observed TradingView value."""
    mean = sum(Fraction(prices[i]["close"]) for i in range(slot - 2, slot + 1)) / 3
    return (mean / 1000 + Fraction(offset) * Fraction(STEP)) / Fraction(STEP)


def rational_boundary(prices, slot, offset):
    # A classification from exact decimal feed inputs, not observed TV SMA,
    # and never a source of expected fill quantities.
    return exact_lots(prices, slot, offset).denominator == 1


def structure(bars, trades, summary, events):
    """Differences that need no tape: rejections, schedule, tails."""
    failures = []
    for event in events:
        if event["type"] in {"rejected", "replace_rejected", "match_rejected"}:
            failures.append({"reason": "native rejection", "event": event})
    by_entry = {}
    for trade in trades:
        key = trade["entry_time"]
        if key in by_entry:
            failures.append({"reason": "duplicate native entry", "native": trade})
        else:
            by_entry[key] = trade
    planned_entries = {FIRST + (slot + 1) * HOUR for slot in SLOTS}
    for key, trade in by_entry.items():
        if key not in planned_entries:
            failures.append({"reason": "unmatched native row", "native": trade})
    expected_closes = {slot + 1 for slot in SLOTS}
    for i, row in enumerate(bars):
        if row["entry"] is not (i in SLOTS) or row["close"] is not (i in expected_closes):
            failures.append({"reason": "command schedule differs/rejected order", "bar": row})
    if summary["pending"] != 0 or binary(summary["position"]) != 0:
        failures.append({"reason": "unaccounted pending/open tail", "summary": summary})
    return failures, by_entry


def identity_differs(native, slot):
    return (not native["long"] or native["open_at_end"] or native["entry_id"] != "L"
            or native["exit_time"] != FIRST + (slot + 2) * HOUR
            or native["entry_bar"] != slot + 1 or native["exit_bar"] != slot + 2)


def signal_words(entry, exit_row, bar, slot):
    """The diagnostic channel of one command: TradingView's own words beside
    the native ones. Reported, never a difference."""
    decoded = signal(entry["Signal"])
    require(exit_row["Signal"] == entry["Signal"], "TV entry/exit signals differ")
    require(decoded["time"] == FIRST + slot * HOUR and decoded["bar"] == slot,
            "TV signal is not this command's")
    return {key: {"tv": word_of(decoded[key]), "native": bar[key],
                  "equal": same_word(bar[key], decoded[key])} for key in SIGNAL_WORDS}


def compare(bars, trades, summary, tv, prices, offset, events):
    failures, by_entry = structure(bars, trades, summary, events)
    joined = []
    for number, slot in enumerate(SLOTS, 1):
        pair = tv[number]
        entry, exit_row = pair["Entry long"], pair["Exit long"]
        native = by_entry.get(FIRST + (slot + 1) * HOUR)
        row = {"number": number, "command": bars[slot], "native": native, "tv": pair,
               "rational_input_boundary": rational_boundary(prices, slot, offset),
               "signal": signal_words(entry, exit_row, bars[slot], slot)}
        if native is None:
            row["differences"] = ["missing native entry"]
        else:
            differences = []
            if identity_differs(native, slot):
                differences.append("native trade identity/time/open state")
            for key, leg in (("entry_price", entry), ("exit_price", exit_row)):
                if native_decimal(native[key]) != decimal(leg["Price USDT"]):
                    differences.append(key)
            actual, expected = native_decimal(native["qty"]), decimal(entry["Size (qty)"])
            row["quantity_delta_steps"] = str((actual - expected) / STEP)
            # The same delta in whole lot steps, apart from the last places of
            # the native double: a one-step row and a text-only row stay apart.
            row["quantity_lot_delta"] = lot_steps(actual)[0] - lot_steps(expected)[0]
            if actual != expected:
                differences.append("quantity")
            # Compare every PnL to the cent and separately check quote-price
            # arithmetic. Float32/export rounding disagreement stays a failure.
            pnl = native_decimal(native["pnl"])
            tv_pnl = decimal(exit_row["Net PnL USDT"])
            quote_pnl = (decimal(exit_row["Price USDT"]) - decimal(entry["Price USDT"])) * expected
            cent = Decimal("0.01")
            tv_cents = tv_pnl.quantize(cent, rounding=ROUND_HALF_UP)
            if pnl.quantize(cent, rounding=ROUND_HALF_UP) != tv_cents:
                differences.append("USDT PnL cents")
            if quote_pnl.quantize(cent, rounding=ROUND_HALF_UP) != tv_cents:
                differences.append("TV quote PnL/FX cents")
            row["quote_pnl"] = str(quote_pnl)
            row["differences"] = differences
        joined.append(row)
        if row["differences"]:
            failures.append(row)
    lot_deltas = [row["quantity_lot_delta"] for row in joined if "quantity_lot_delta" in row]
    return {"offset_steps": offset, "measured": "tradingview", "joined": joined,
            "failures": failures, "comparison_equal": not failures,
            "rational_boundary_rows": sum(row["rational_input_boundary"] for row in joined),
            "rational_nonboundary_rows": sum(not row["rational_input_boundary"] for row in joined),
            "quantity_differences": sum("quantity" in row["differences"] for row in joined),
            "one_step_differences": sum(abs(Decimal(row["quantity_delta_steps"])) == 1
                                        for row in joined if "quantity_delta_steps" in row),
            "lot_step_higher": sum(delta > 0 for delta in lot_deltas),
            "lot_step_lower": sum(delta < 0 for delta in lot_deltas),
            "signal": {"decides": False,
                       "rows_differing": {key: sum(not row["signal"][key]["equal"]
                                                   for row in joined)
                                          for key in SIGNAL_WORDS}}}


def control(bars, trades, summary, prices, offset, events):
    """A form without a tape: a native control, not a TradingView comparison.
    Off a lot boundary the fill must be the floor of the exact quantity; on a
    boundary the lot is only reported."""
    failures, by_entry = structure(bars, trades, summary, events)
    rows = []
    for number, slot in enumerate(SLOTS, 1):
        native = by_entry.get(FIRST + (slot + 1) * HOUR)
        exact = exact_lots(prices, slot, offset)
        row = {"number": number, "command": bars[slot], "native": native,
               "rational_input_boundary": exact.denominator == 1,
               "exact_lot_steps": str(exact)}
        if native is None:
            row["differences"] = ["missing native entry"]
        else:
            differences = []
            if identity_differs(native, slot):
                differences.append("native trade identity/time/open state")
            lots, residue = lot_steps(native_decimal(native["qty"]))
            row["native_lot_steps"] = lots
            row["lots_under_exact_floor"] = math.floor(exact) - lots
            # A fill sits on the lot grid up to the last places of its double.
            if abs(residue) >= STEP / 1000000:
                differences.append("native quantity off the lot grid")
            if exact.denominator != 1 and lots != math.floor(exact):
                differences.append("quantity off the exact floor")
            row["differences"] = differences
        rows.append(row)
        if row["differences"]:
            failures.append(row)
    return {"offset_steps": offset, "measured": "native control only; no TradingView tape",
            "rows": rows, "failures": failures, "comparison_equal": not failures,
            "rational_boundary_rows": sum(row["rational_input_boundary"] for row in rows)}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
                    encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # Refuse reuse, preserving every failed or completed collection.
    created = False
    try:
        args.output.mkdir(parents=True, exist_ok=False)
        created = True
        (args.output / "case.input.json").write_bytes(args.case.read_bytes())
        case, bars_path, prices, tapes = load_case(args.case.resolve())
        runner = args.runner.resolve(strict=True)
        write_json(args.output / "case.json", case)
        write_json(args.output / "execution.json",
                   {"runner": str(runner), "sha256": digest(runner),
                    "caller": "manually lowered real-engine fixture; no codegen agreement claim"})
        for offset in OFFSETS:
            result = subprocess.run([str(runner), "--bars", str(bars_path), "--offset", offset],
                                    capture_output=True, check=False)
            name = NAMES[offset]
            (args.output / (name + ".native.jsonl")).write_bytes(result.stdout)
            (args.output / (name + ".stderr")).write_bytes(result.stderr)
            write_json(args.output / (name + ".exit.json"), {"returncode": result.returncode})
            require(result.returncode == 0, f"{name}: native execution failed; sequence stopped")
            bars, trades, summary, events = native_rows(result.stdout.decode("utf-8"), offset)
            if offset in tapes:
                comparison = compare(bars, trades, summary, tapes[offset], prices, offset, events)
                differing = comparison["signal"]["rows_differing"]
                print(f"{name}: {len(comparison['joined'])} joined, "
                      f"{len(comparison['failures'])} differing records, "
                      f"{comparison['lot_step_higher']} lot steps higher, "
                      f"{comparison['lot_step_lower']} lower; signal rows differing: "
                      f"sma {differing['sma']}, previous {differing['previous_sma']}, "
                      f"request {differing['request']}", flush=True)
            else:
                comparison = control(bars, trades, summary, prices, offset, events)
                print(f"{name}: no TradingView tape; native control, "
                      f"{len(comparison['failures'])} differing records", flush=True)
            write_json(args.output / (name + ".comparison.json"), comparison)
            if not comparison["comparison_equal"]:
                # A failed comparison stops this sequence; no later form can
                # disguise its failure. All forms remain required for success.
                write_json(args.output / "result.json",
                           {"complete": False, "comparison_equal": False, "stopped_at": offset})
                return 1
        write_json(args.output / "result.json",
                   {"complete": True, "comparison_equal": True,
                    "tradingview_forms": [offset for offset in OFFSETS if offset in tapes],
                    "native_control_forms": [offset for offset in OFFSETS if offset not in tapes],
                    "approval_reference": case["approval_reference"],
                    "scope": "fixture comparison only; hosted CI/parity and paired codegen still required"})
        return 0
    except (ValueError, KeyError, OSError, UnicodeError, TypeError, InvalidOperation) as error:
        if created:
            write_json(args.output / "result.json", {"complete": False, "error": str(error)})
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
