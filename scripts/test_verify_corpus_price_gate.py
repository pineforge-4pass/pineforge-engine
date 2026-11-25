#!/usr/bin/env python3
"""Check which trades the corpus grader pairs at different price levels."""

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

from verify_corpus import TradePair, align_by_time, analyze_strategy, pair_range_end_marks


def _trades(prices, *, same_bar=False, open_mark=False):
    trades = []
    for index, price in enumerate(prices):
        # Separate independent cases by three hours.
        entry_time = 0 if same_bar else index * 10_800
        trades.append(TradePair("long", entry_time, price, 86_400,
                                1.0, 1.0, 0.0, index + 1, open_mark=open_mark))
    return trades


def _entry_prices(pairs):
    return [(tv.entry_price, engine.entry_price) for tv, engine in pairs]


def _write_trades(path, prices):
    with path.open("x", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["Trade #", "Type", "Date and time", "Price", "Qty"])
        for index, price in enumerate(prices):
            hour = index * 3
            writer.writerow([index + 1, "Entry long", f"2025-01-01 {hour:02d}:00", price, 1])
            writer.writerow([index + 1, "Exit long", f"2025-01-01 {hour + 1:02d}:00", 101_000, 1])


def _analyze(tv_prices, engine_prices):
    # CTest runs in its build directory. Keep fixtures there and use only
    # fixed file names inside a new directory.
    parent = Path.cwd()
    parts = parent.parts
    if (any(part in {".ssh", ".aws", ".codex"} or part.endswith(".env") for part in parts)
            or any(parts[i:i + len(name)] == name
                   for name in ((".config", "zsh", "keys"), (".config", "pineforge"))
                   for i in range(len(parts)))):
        raise ValueError(f"refused sensitive fixture directory: {parent}")

    with tempfile.TemporaryDirectory(prefix="verify-corpus-price-", dir=parent) as directory:
        strategy = Path(directory)
        files = ("inputs.json", "tv_trades.csv", "engine_trades.csv", "strategy.pine",
                 "ohlcv.csv", "data.csv", "candles.csv")
        print("Analyzer fixture paths: " + ", ".join(str(strategy / name) for name in files),
              flush=True)
        (strategy / "inputs.json").write_text('{"tv_trades_csv_tz": "utc"}', encoding="utf-8")
        _write_trades(strategy / "tv_trades.csv", tv_prices)
        _write_trades(strategy / "engine_trades.csv", engine_prices)
        return analyze_strategy(strategy)


class EntryPriceGateTests(unittest.TestCase):
    def test_analyzer_pairs_high_price_trades_within_the_limit(self):
        # The 200-unit miss lies between the other two entries in time.
        result = _analyze([100_000, 100_000, 100_000], [100_010, 100_200, 100_100])
        self.assertEqual((result.tv_raw_count, result.eng_raw_count), (3, 3))
        self.assertEqual(_entry_prices(result.matched), [(100_000, 100_010), (100_000, 100_100)])

    def test_low_price_trades_pair_only_inside_the_limit(self):
        tv = _trades([1.1, 1.1])
        engine = _trades([1.15, 1.101])
        self.assertEqual(_entry_prices(align_by_time(tv, engine)), [(1.1, 1.101)])

    def test_price_limits_for_quarter_point_and_share_prices(self):
        cases = [
            # A 37.50 gap is exactly on the limit, even below the TV price.
            (25_000, [25_037.25, 25_037.75, 24_962.50],
             [(25_000, 25_037.25), (25_000, 24_962.50)]),
            # The 0.01759 gap fits only if scaled from the engine price.
            # The downward gap of 0.017578125 is exactly on the TV limit.
            (11.71875, [11.73575, 11.73634, 11.701171875],
             [(11.71875, 11.73575), (11.71875, 11.701171875)]),
            # At 100 the inclusive limit is 0.15 in either direction.
            (100, [100.149999, 100.15, 100.150001, 99.85],
             [(100, 100.149999), (100, 100.15), (100, 99.85)]),
            # A 0.15 gap is just outside the limit below 100.
            (99.99999999999982, [99.99999999999982, 99.84999999999982],
             [(99.99999999999982, 99.99999999999982)]),
        ]
        for tv_price, engine_prices, expected in cases:
            with self.subTest(tv_price=tv_price):
                tv = _trades([tv_price] * len(engine_prices))
                engine = _trades(engine_prices)
                self.assertEqual(_entry_prices(align_by_time(tv, engine)), expected)

    def test_two_thousand_price_keeps_the_inclusive_three_unit_limit(self):
        tv = _trades([2_000, 2_000, 2_000])
        engine = _trades([2_002.9, 2_003.1, 2_003.0])
        self.assertEqual(_entry_prices(align_by_time(tv, engine)),
                         [(2_000, 2_002.9), (2_000, 2_003.0)])

    def test_range_end_marks_use_the_entry_price_limit(self):
        tv = _trades([100_000, 100_000], open_mark=True)
        engine = _trades([100_100, 100_200], open_mark=True)
        self.assertEqual(pair_range_end_marks(tv, engine),
                         ([(tv[0], engine[0])], [tv[1]], [engine[1]]))

    def test_same_bar_entries_pair_with_candidates_near_their_own_price(self):
        tv = _trades([1.1, 1.15], same_bar=True)
        engine = _trades([1.1505, 1.0995, 1.101, 1.149], same_bar=True)
        self.assertEqual(_entry_prices(align_by_time(tv, engine)),
                         [(1.1, 1.0995), (1.15, 1.1505)])


if __name__ == "__main__":
    unittest.main()
