#!/usr/bin/env python3
"""Derive the issue #716 public trade-list fixtures from the btc60 feed.

Usage:
    derive_item_csvs.py FEED_CSV OUTPUT_DIR

FEED_CSV is tests/fixtures/qty_step_mincontract/btc60/feed/bars.csv
(columns timestamp,open,high,low,close,volume; timestamp = bar open time in
integer milliseconds since the Unix epoch, UTC). OUTPUT_DIR must be absent or
an empty directory. Exactly two files are written: item1.tv_trades.csv and
item2.tv_trades.csv. Nothing is written unless every check passes.

Case definition (one long trade per item, open at the end of the feed):
    entry  = open of the bar with timestamp 1704074400000 (2024-01-01 02:00 UTC)
    exit   = close of the bar with timestamp 1709251200000 (2024-03-01 00:00 UTC)
    item 1 quantity = 100000 USDT equity / entry price (100% of equity)
    item 2 quantity = declared case quantity 0.12345 BTC

Columns (TradingView list-of-trades layout, money metrics omitted):
    Trade #        integer trade number (1)
    Type           "Entry long" or "Exit long"
    Date and time  "YYYY-MM-DD HH:MM", UTC (offset +00:00), entry row = entry
                   bar open time, exit row = exit bar open time
    Price USDT     feed price, quote currency, 2 decimals
    Size (qty)     base-currency quantity, 5 decimals
    Size (value)   notional = quantity * row price, USDT, 2 decimals
    Net P&L %      trade return = (exit / entry - 1) * 100, percent,
                   2 decimals, no commission; same value on both rows

All arithmetic uses decimal.Decimal (precision 50) on the feed text; each
column is quantized once to its stated precision with ROUND_HALF_UP. No
quote-currency PnL, excursion or commission columns are produced.
Encoding UTF-8 without BOM, LF newlines, fields quoted only when needed
(csv.QUOTE_MINIMAL), rows: header, Exit long, Entry long.
"""
import csv
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP, getcontext
import io
from pathlib import Path
import sys

getcontext().prec = 50

FEED_COLUMNS = ["timestamp", "open", "high", "low", "close", "volume"]
ENTRY_MS = 1704074400000
EXIT_MS = 1709251200000
EQUITY = Decimal("100000")
ITEM2_QTY = Decimal("0.12345")
PRICE_Q = Decimal("0.01")
QTY_Q = Decimal("0.00001")
VALUE_Q = Decimal("0.01")
PCT_Q = Decimal("0.01")
HEADER = ["Trade #", "Type", "Date and time", "Price USDT",
          "Size (qty)", "Size (value)", "Net P&L %"]
OUTPUTS = ("item1.tv_trades.csv", "item2.tv_trades.csv")


class FeedError(ValueError):
    pass


def number(text, label):
    try:
        value = Decimal(text)
    except (InvalidOperation, TypeError):
        raise FeedError(f"malformed {label}: {text!r}") from None
    if not value.is_finite():
        raise FeedError(f"non-finite {label}: {text!r}")
    return value


def load_feed(path):
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header != FEED_COLUMNS:
            raise FeedError(f"unexpected feed header: {header!r}")
        bars = {}
        previous = None
        for line, row in enumerate(reader, start=2):
            if len(row) != len(FEED_COLUMNS):
                raise FeedError(f"line {line}: expected 6 fields, got {len(row)}")
            if not row[0].isdigit():
                raise FeedError(f"line {line}: malformed timestamp {row[0]!r}")
            stamp = int(row[0])
            if stamp in bars:
                raise FeedError(f"line {line}: duplicate timestamp {stamp}")
            if previous is not None and stamp <= previous:
                raise FeedError(f"line {line}: timestamp out of order {stamp}")
            values = {key: number(text, f"line {line} {key}")
                      for key, text in zip(FEED_COLUMNS[1:], row[1:])}
            bars[stamp] = values
            previous = stamp
    return bars


def required_price(bars, stamp, key):
    if stamp not in bars:
        raise FeedError(f"required bar {stamp} is missing")
    price = bars[stamp][key]
    if price <= 0:
        raise FeedError(f"unusable {key} {price} at {stamp}")
    return price


def stamp_text(ms):
    moment = datetime.fromtimestamp(ms // 1000, tz=timezone.utc)
    return moment.strftime("%Y-%m-%d %H:%M")


def q(value, quantum):
    return value.quantize(quantum, rounding=ROUND_HALF_UP)


def render(quantity, entry, exit_price):
    if quantity <= 0:
        raise FeedError(f"unusable quantity {quantity}")
    percent = (exit_price / entry - 1) * 100
    rows = [
        ["1", "Exit long", stamp_text(EXIT_MS), exit_price, quantity,
         quantity * exit_price, percent],
        ["1", "Entry long", stamp_text(ENTRY_MS), entry, quantity,
         quantity * entry, percent],
    ]
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(HEADER)
    for number_, kind, when, price, qty, value, pct in rows:
        writer.writerow([number_, kind, when, str(q(price, PRICE_Q)),
                         str(q(qty, QTY_Q)), str(q(value, VALUE_Q)),
                         str(q(pct, PCT_Q))])
    return buffer.getvalue().encode("utf-8")


def main(argv):
    if len(argv) != 3:
        print("usage: derive_item_csvs.py FEED_CSV OUTPUT_DIR", file=sys.stderr)
        return 2
    feed, out = Path(argv[1]), Path(argv[2])
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        print(f"output directory is not fresh: {out}", file=sys.stderr)
        return 2
    try:
        bars = load_feed(feed)
        entry = required_price(bars, ENTRY_MS, "open")
        exit_price = required_price(bars, EXIT_MS, "close")
        quantity1 = q(EQUITY / entry, QTY_Q)
        blobs = {OUTPUTS[0]: render(quantity1, entry, exit_price),
                 OUTPUTS[1]: render(ITEM2_QTY, entry, exit_price)}
    except (FeedError, OSError, UnicodeDecodeError, csv.Error) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 1
    out.mkdir(parents=True, exist_ok=True)
    for name, blob in blobs.items():
        with open(out / name, "xb") as handle:
            handle.write(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
