"""Entry bars, partner selection and the edges of the corpus comparison."""

import csv
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

from test_verify_corpus_count_rule import _checked_path
from verify_corpus import TradePair, align_by_time, analyze_strategy, trim_to_common_match_window


START = 1735689600  # 2025-01-01 00:00 UTC


def trade(minute, price=100.0, *, direction="long", mark=False):
    return TradePair(
        direction=direction, entry_time=START + 60 * minute, entry_price=price,
        exit_time=START + 60 * (120 if mark else minute + 15),
        exit_price=price + (1 if direction == "long" else -1),
        qty=1, pnl=1, open_mark=mark,
    )


def forty(shifts=None, *, short_prefix=0):
    # One entry per 15-minute bar. Prices distinguish the lots even when a
    # shifted entry lands on another lot's bar.
    shifts = shifts or {}
    return [trade(15 * i + shifts.get(i, 0), 100 + 10 * i,
                  direction="short" if i < short_prefix else "long") for i in range(40)]


def minutes(pairs):
    return [((t.entry_time - START) // 60, (e.entry_time - START) // 60) for t, e in pairs]


def write_tape(path, trades, *, tv):
    # The two tapes describe instants in different wall clocks, even in the equal-list control.
    zone = timezone(timedelta(hours=8)) if tv else timezone.utc

    def stamp(seconds):
        return datetime.fromtimestamp(seconds, zone).strftime("%Y-%m-%d %H:%M")

    with path.open("x", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("Trade #", "Type", "Date and time", "Price", "Qty", "Net PnL",
                         "Signal" if tv else "Engine range-end"))
        for n, t in enumerate(trades, start=1):
            writer.writerow((n, f"Entry {t.direction}", stamp(t.entry_time), t.entry_price,
                             t.qty, t.pnl, f"entry-{n}" if tv else ""))
            mark = ("Open" if tv else "open") if t.open_mark else ""
            writer.writerow((n, f"Exit {t.direction}", stamp(t.exit_time), t.exit_price,
                             t.qty, t.pnl, mark))


class SameBarTests(unittest.TestCase):
    def analyze(self, tv, eng):
        with tempfile.TemporaryDirectory(prefix="same-bar-", dir=_checked_path(Path.cwd())) as tmp:
            root = Path(tmp)
            files = {name: _checked_path(root / name) for name in (
                "tv_trades.csv", "engine_trades.csv", "inputs.json", "strategy.pine",
                "ohlcv.csv", "data.csv", "candles.csv",
            )}
            print("Grader fixture paths:", *files.values(), sep="\n", flush=True)
            files["inputs.json"].write_text('{"tv_trades_csv_tz":"utc_plus_8"}\n', encoding="utf-8")
            write_tape(files["tv_trades.csv"], tv, tv=True)
            write_tape(files["engine_trades.csv"], eng, tv=False)
            r = analyze_strategy(root)
        self.assertTrue(r.tv_exists and r.eng_exists)
        self.assertEqual((r.tv_raw_count, r.eng_raw_count), (len(tv), len(eng)))
        return r

    def assert_midlist_shift(self, shift):
        r = self.analyze(forty(), forty({20: shift}))
        self.assertEqual((r.matched_count, r.count_abs_delta, r.label), (39, 2, "strong"))
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.count_delta), (40, 40, 2 / 40))
        self.assertEqual((r.coverage, r.coverage_ok, r.count_ok), (39 / 40, True, False))
        self.assertEqual(minutes(r.matched), [(i * 15, i * 15) for i in range(40) if i != 20])

    def test_midlist_one_bar_late(self):
        self.assert_midlist_shift(15)

    def test_midlist_one_bar_early(self):
        self.assert_midlist_shift(-15)

    def test_midlist_sixty_minutes_late(self):
        self.assert_midlist_shift(60)

    def test_midlist_sixty_one_minutes_late(self):
        self.assert_midlist_shift(61)

    def test_neighbours_keep_their_exact_partners(self):
        # With just the two exact pairs the old matcher also succeeds. The
        # earlier TV-only row used to take minute 15, so minute 15 took 30.
        pairs = align_by_time([trade(0), trade(15), trade(30)], [trade(15), trade(30)])
        self.assertEqual(minutes(pairs), [(15, 15), (30, 30)])

    def assert_leading_engine_row(self, lead):
        r = self.analyze([trade(0), trade(15)], [trade(-lead), trade(0), trade(15)])
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.count_abs_delta, r.label),
                         (2, 2, 0, "excellent"))
        self.assertEqual(minutes(r.matched), [(0, 0), (15, 15)])

    def test_engine_only_forty_five_minutes_before(self):
        self.assert_leading_engine_row(45)

    def test_engine_only_seventy_five_minutes_before(self):
        self.assert_leading_engine_row(75)

    def test_next_exact_entry_starts_the_comparison(self):
        r = self.analyze([trade(0), trade(30)], [trade(15), trade(30)])
        self.assertEqual(minutes(r.matched), [(30, 30)])
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.count_abs_delta), (1, 1, 0))
        self.assertEqual((r.coverage_tv_count, r.coverage, r.coverage_ok, r.label),
                         (2, 1 / 2, True, "excellent"))
        self.assertEqual((r.unmatched_total, r.coverage_ok, r.label), (1, False, "weak"))

        for total, expected_tier in (
            (100, "excellent"), (99, "strong"), (20, "strong"), (19, "moderate"),
        ):
            with self.subTest(total=total):
                tv = [trade(15 * i) for i in range(total)]
                engine = [trade(15 * i) for i in range(1, total)]
                result = self.analyze(tv, engine)
                self.assertEqual((result.matched_count, result.tv_gate_count, result.eng_gate_count),
                                 (total - 1, total - 1, total - 1))
                self.assertEqual((result.count_abs_delta, result.coverage_tv_count,
                                  result.unmatched_total, result.coverage),
                                 (0, total, 1, (total - 1) / total))
                self.assertEqual((result.coverage_ok, result.label),
                                 (expected_tier == "excellent", expected_tier))

    def assert_tv_tail(self, delay, tv_count, gross, label):
        r = self.analyze([*forty(), trade(39 * 15 + delay, 500)], forty())
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.matched_count,
                          r.count_abs_delta, r.label), (tv_count, 40, 40, gross, label))
        self.assertEqual((r.coverage_tv_count, r.coverage), (41, 40 / 41))

    def test_tv_only_forty_five_minutes_after(self):
        self.assert_tv_tail(45, 41, 1, "strong")

    def test_tv_only_two_hours_after(self):
        self.assert_tv_tail(120, 40, 0, "excellent")

    def test_last_hour_includes_its_endpoint_on_both_sides(self):
        tv = [trade(0), trade(60), trade(61)]
        eng = [trade(0), trade(60, direction="short"), trade(61, direction="short")]
        self.assertEqual(trim_to_common_match_window(tv, eng, [(tv[0], eng[0])]),
                         (tv[:2], eng[:2]))

    def assert_prefix_coverage(self, eng):
        r = self.analyze(forty(), eng)
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.matched_count,
                          r.count_abs_delta, r.label), (30, 30, 30, 0, "moderate"))
        self.assertEqual((r.coverage_tv_count, r.coverage, r.unmatched_total, r.coverage_ok),
                         (40, 30 / 40, 10, False))
        self.assertEqual(minutes(r.matched), [(i * 15, i * 15) for i in range(10, 40)])

    def test_ten_entries_one_bar_early_still_cost_coverage(self):
        self.assert_prefix_coverage(forty({i: -15 for i in range(10)}))

    def test_ten_opposite_entries_still_cost_coverage(self):
        self.assert_prefix_coverage(forty(short_prefix=10))

    def test_same_bar_prices_in_reverse_order(self):
        tv = [trade(0, 100), trade(0, 100.001)]
        eng = [trade(0, 100.001), trade(0, 100)]
        self.assertEqual(align_by_time(tv, eng), [(tv[0], eng[1]), (tv[1], eng[0])])

    def test_nearest_price_then_list_order_for_equal_distances(self):
        tv = [trade(0, 1024), trade(0, 1024), trade(0, 2048)]
        # The two nearest longs are exactly equidistant in binary; the
        # higher price comes first. No remaining row is eligible at 2048.
        eng = [trade(0, 1024, direction="short"), trade(0, 1024 + 3 / 64),
               trade(0, 1024 + 1 / 64), trade(0, 1024 - 1 / 64)]
        self.assertEqual(align_by_time(tv, eng), [(tv[0], eng[2]), (tv[1], eng[3])])

        # Both prices are 0.01 from the TV entry, so the first row wins.
        tv = [trade(0, 100.01)]
        eng = [trade(0, 100.00), trade(0, 100.02)]
        self.assertEqual(align_by_time(tv, eng), [(tv[0], eng[0])])

    def test_open_marks_on_the_same_entry_bar(self):
        r = self.analyze([trade(0), trade(30, mark=True)], [trade(0), trade(30, mark=True)])
        self.assertEqual((r.open_mark_pairs, r.matched_count, r.count_abs_delta, r.label),
                         (1, 2, 0, "excellent"))
        self.assertEqual(minutes(r.open_mark_matched), [(30, 30)])
        self.assertEqual(minutes(r.matched), [(0, 0)])

    def test_open_marks_on_different_entry_bars(self):
        r = self.analyze([trade(0), trade(30, mark=True)], [trade(0), trade(45, mark=True)])
        self.assertEqual((r.open_mark_pairs, r.matched_count, r.count_abs_delta, r.label),
                         (0, 1, 2, "weak"))
        self.assertEqual(minutes(r.matched), [(0, 0)])
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.coverage_tv_count), (2, 2, 1))

    def test_equal_lists(self):
        r = self.analyze(forty(), forty())
        self.assertEqual(minutes(r.matched), [(i * 15, i * 15) for i in range(40)])
        self.assertEqual((r.matched_count, r.count_abs_delta, r.coverage, r.label),
                         (40, 0, 1.0, "excellent"))

    def test_empty_engine_list(self):
        r = self.analyze([trade(0), trade(15)], [])
        self.assertEqual((r.tv_count, r.eng_count, r.matched_count, r.count_abs_delta,
                          r.no_aligned_trades, r.label), (2, 0, 0, 2, True, "minimal"))

    def test_empty_tv_list(self):
        r = self.analyze([], [trade(0), trade(15)])
        self.assertEqual((r.tv_count, r.eng_count, r.matched_count, r.count_abs_delta,
                          r.no_aligned_trades, r.label), (0, 2, 0, 2, True, "minimal"))

    def test_one_pair_only(self):
        r = self.analyze([trade(15)], [trade(15)])
        self.assertEqual(minutes(r.matched), [(15, 15)])
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.count_abs_delta, r.label),
                         (1, 1, 0, "excellent"))

    def test_distinct_printed_minutes_in_one_chart_interval(self):
        pairs = align_by_time([trade(60), trade(75)], [trade(61), trade(75)])
        self.assertEqual(minutes(pairs), [(75, 75)])


if __name__ == "__main__":
    unittest.main()
