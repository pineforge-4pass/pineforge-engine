"""Tests for the corpus grader's source and export readers."""

import csv
import json
import unittest
from datetime import datetime, timezone
from io import StringIO
from pathlib import PurePosixPath
from posixpath import normpath
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from verify_corpus import (
    _ohlcv_span_ms, analyze_strategy, detect_parity_profile, distinct_entry_fill_keys,
    parse_dt, parse_trades, tv_tzinfo,
)


class MemoryPath:
    """Serve only in-memory probe files through the grader's path interface."""

    def __init__(self, path, files):
        self.path = path
        self.files = files

    def __truediv__(self, name):
        return MemoryPath(self.path / name, self.files)

    def resolve(self):
        return MemoryPath(PurePosixPath(normpath(self.path)), self.files)

    @property
    def name(self):
        return self.path.name

    @property
    def parent(self):
        return self.path.parent

    def is_file(self):
        return self.path in self.files

    exists = is_file

    def open(self, *, encoding):
        if not self.is_file():
            raise FileNotFoundError(str(self.path))
        return StringIO(self.files[self.path])


def probe(files):
    root = PurePosixPath("/probe")
    return MemoryPath(root, {root / name: contents for name, contents in files.items()})


def trade_tape(*trades):
    tape = StringIO()
    writer = csv.writer(tape)
    writer.writerow(("Trade #", "Type", "Date and time", "Price", "Qty",
                     "Net PnL", "Signal", "Engine entry incarnation"))
    for n, (entry, exit_time, qty, signal, identity) in enumerate(trades, 1):
        writer.writerow((n, "Entry long", entry, 100, qty, 0, signal, identity))
        writer.writerow((n, "Exit long", exit_time, 101, qty, qty, "close", ""))
    return tape.getvalue()


def feed(*timestamps):
    return "timestamp,open,high,low,close,volume\n" + "".join(
        f"{timestamp},100,101,100,101,1\n" for timestamp in timestamps
    )


class ReadingRulesTests(unittest.TestCase):
    def test_profile_scan_ignores_string_literals_and_comparisons(self):
        cases = (
            ('strategy.exit("ordinary", limit=101)', "strict"),
            ("// trail_points=1\n/* trail_offset=2 */", "strict"),
            ('// a comment\nstrategy.exit("trail", trail_price = 101)', "production"),
            ('strategy("trail_points=1")', "strict"),
            ("if trail_points == 1\n    strategy.close('L')", "strict"),
            ("int TRAIL_POINTS = 1", "strict"),
            (r'strategy("an escaped \" quote: trail_offset=1")', "strict"),
            ("""strategy('trail_price=1')""", "strict"),
            ('strategy("""a " quote: trail_points=1 " inside""")', "strict"),
            ('strategy.exit("https://x", trail_points=1)', "production"),
            ('strategy.exit("/*", trail_offset=1) // */', "production"),
        )
        for source, expected in cases:
            with self.subTest(source=source):
                self.assertEqual(detect_parity_profile(source), expected)

    def test_slashless_chart_zone_resolves_to_utc(self):
        for name in ("Japan", "Asia/Kolkata", "America/New_York"):
            try:
                ZoneInfo(name)
            except ZoneInfoNotFoundError:
                self.fail(f"NOT COMPARED: time zone data unavailable for {name}")
        midnight = datetime(2025, 1, 1, tzinfo=timezone.utc)
        cases = (
            ({}, "2025-01-01 08:00", midnight),
            ({"tv_trades_csv_tz": "  "}, "2025-01-01 08:00", midnight),
            ({"tv_trades_csv_tz": " utc "}, "2025-01-01 00:00", midnight),
            ({"tv_trades_csv_tz": "Asia/Kolkata"}, "2025-01-01 05:30", midnight),
            ({"tv_trades_csv_tz": "America/New_York"}, "2025-01-01 09:00",
             midnight.replace(hour=14)),
            ({"tv_trades_csv_tz": "America/New_York"}, "2025-07-01 09:00",
             midnight.replace(month=7, hour=13)),
            ({"tv_trades_csv_tz": "Japan"}, "2025-01-01 09:00", midnight),
        )
        for metadata, wall_time, utc in cases:
            with self.subTest(metadata=metadata, wall_time=wall_time):
                self.assertEqual(parse_dt(wall_time, tv_tzinfo(metadata)), int(utc.timestamp()))

        with self.subTest(case="same-bar pair"):
            result = analyze_strategy(probe({
                "inputs.json": '{"tv_trades_csv_tz":"Japan"}',
                "tv_trades.csv": trade_tape(
                    ("2025-01-01 09:00", "2025-01-01 09:15", 1, "L", "")),
                "engine_trades.csv": trade_tape(
                    ("2025-01-01 00:00", "2025-01-01 00:15", 1, "", "41")),
            }))
            self.assertEqual((result.tv_raw_count, result.eng_raw_count), (1, 1))
            self.assertEqual(result.matched_count, 1)
            utc_entry = int(midnight.timestamp())
            self.assertEqual([(t.entry_time, e.entry_time) for t, e in result.matched],
                             [(utc_entry, utc_entry)])

        with self.subTest(case="unknown name"):
            with self.assertRaisesRegex(ValueError, "tv_trades_csv_tz.*Asia/Taipeii"):
                tv_tzinfo({"tv_trades_csv_tz": "Asia/Taipeii"})

    def test_distinct_signals_preserve_trailing_space(self):
        entry, exit_time = "2025-01-01 00:00", "2025-01-01 00:15"
        entry_key = (int(datetime(2025, 1, 1, tzinfo=timezone.utc).timestamp()),
                     100, "long")
        engine = trade_tape((entry, exit_time, 1, "", "41"))
        for signal, keys, counts in (
            ("L", set(), (1, 1, 0, 0)),
            ("L ", {entry_key}, (2, 1, 1, 1)),
        ):
            with self.subTest(second_signal=signal):
                root = probe({
                    "inputs.json": '{"tv_trades_csv_tz":"utc"}',
                    "tv_trades.csv": trade_tape(
                        (entry, exit_time, 0.5, "L", ""),
                        (entry, exit_time, 0.5, signal, "")),
                    "engine_trades.csv": engine,
                })
                tv = parse_trades(root / "tv_trades.csv", tz=timezone.utc)
                self.assertEqual(len(tv), 2)
                result = analyze_strategy(root)
                self.assertEqual(result.matched_count, 1)
                self.assertEqual((result.tv_gate_count, result.eng_gate_count,
                                  result.count_abs_delta, result.distinct_entry_mismatches),
                                 counts)
                self.assertEqual(distinct_entry_fill_keys(tv), keys)

    def test_historical_epoch_milliseconds_are_not_scaled_as_seconds(self):
        interval = 15 * 60
        for year, units_per_second in (
            (2025, 1), (2025, 1000), (2000, 1), (1960, 1),
            (2000, 1000), (1960, 1000),
        ):
            with self.subTest(year=year, units_per_second=units_per_second):
                start = int(datetime(year, 1, 1, tzinfo=timezone.utc).timestamp())
                end = start + interval
                root = probe({"ohlcv.csv": feed(start * units_per_second,
                                               end * units_per_second)})
                self.assertEqual(_ohlcv_span_ms(root),
                                 (start * 1000, end * 1000, interval * 1000))

    def test_trim_uses_the_declared_chart_feed(self):
        day = int(datetime(2025, 1, 1, tzinfo=timezone.utc).timestamp()) * 1000
        hour = 60 * 60 * 1000
        next_day = day + 24 * hour
        files = {
            "tv_trades.csv": trade_tape(
                ("2025-01-01 01:00", "2025-01-01 02:00", 1, "L", "")),
            "engine_trades.csv": trade_tape(
                ("2025-01-01 01:00", "2025-01-01 02:00", 1, "", "41")),
            "ohlcv.csv": feed(*(next_day + n * hour for n in range(4))),
            "feeds/prices.csv": feed(*(day + n * hour for n in range(4))),
            "/feeds/prices.csv": feed(*(day + n * hour for n in range(5))),
        }
        cases = (
            ({}, (next_day + hour, next_day + 2 * hour), (0, 0)),
            ({"ohlcv_csv": "feeds/prices.csv"}, (day + hour, day + 2 * hour), (1, 1)),
            ({"ohlcv_csv": "/feeds/prices.csv"}, (day + hour, day + 3 * hour), (1, 1)),
        )
        for metadata, bounds, gated in cases:
            with self.subTest(metadata=metadata):
                files["inputs.json"] = json.dumps(
                    {"tv_trades_csv_tz": "utc", "trim_bars": 1, **metadata})
                result = analyze_strategy(probe(files))
                self.assertEqual((result.tv_raw_count, result.eng_raw_count,
                                  result.matched_count), (1, 1, 1))
                self.assertEqual(result.bounds, bounds)
                self.assertEqual((result.tv_gate_count, result.eng_gate_count), gated)


if __name__ == "__main__":
    unittest.main()
