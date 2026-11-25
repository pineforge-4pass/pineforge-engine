"""Money and price gates on synthetic trade lists."""

import csv
import unittest
from dataclasses import replace
from datetime import datetime, timezone
from io import BytesIO, StringIO
from zipfile import Path, ZipFile

from verify_corpus import (
    TradePair, analyze_strategy, has_cross_entry_fifo_allocation,
)


def lot(entry, **fields):
    return replace(TradePair("long", entry, 100, entry + 900, 101, 1, 1), **fields)


def grade(tv, eng):
    # All paths the grader sees are members of this in-memory archive.
    with ZipFile(BytesIO(), "w") as archive:
        archive.filename = "trades.zip"
        archive.writestr("probe/inputs.json", '{"tv_trades_csv_tz":"utc"}')
        for name, trades in (("tv_trades.csv", tv), ("engine_trades.csv", eng)):
            is_tv = name == "tv_trades.csv"
            data = StringIO()
            writer = csv.writer(data)
            writer.writerow(("Trade #", "Type", "Date and time", "Price", "Qty", "Net PnL",
                             "Signal", "Engine entry incarnation", "Engine range-end"))
            for n, t in enumerate(trades, 1):
                for leg, time, price in (("Entry", t.entry_time, t.entry_price),
                                         ("Exit", t.exit_time, t.exit_price)):
                    stamp = datetime.fromtimestamp(time, timezone.utc).strftime("%Y-%m-%d %H:%M")
                    mark = "Open" if t.open_mark and leg == "Exit" else ""
                    signal = t.entry_signal if leg == "Entry" else (mark if is_tv else "")
                    writer.writerow((n, f"{leg} {t.direction}", stamp, price, t.qty, t.pnl,
                                     signal, t.entry_identity if leg == "Entry" else "",
                                     "" if is_tv else mark))
            archive.writestr("probe/" + name, data.getvalue())
        return analyze_strategy(Path(archive, "probe/"))


class MoneyTests(unittest.TestCase):
    def assert_pairs(self, r, n):
        self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.matched_count,
                          r.gating_matched_count, r.count_abs_delta, r.coverage_tv_count),
                         (n, n, n, n, 0, n))

    def test_fifo_uses_proven_same_tick_entry_owners(self):
        def rows(spec, tv):
            return [lot(entry, exit_time=close, qty=qty, pnl=qty,
                        entry_signal=tag if tv else "comment",
                        entry_identity="unused" if tv else tag)
                    for entry, close, tag, qty in spec]

        a = (0, 900, "A", 1)
        b = (0, 1800, "B", 1)
        c = (900, 1800, "C", 1)
        single = (900, 2700, "C", 1)
        # Each reference has the same logical fills but no shared, split owner.
        cases = (
            ([(0, 900, "A", .5), (0, 1800, "A", .5), b], [a, b], 2, 1),
            ([a, b, c], [a, (0, 900, "B", 1), c], 3, 3),
            ([a, (0, 1800, "A", 1), (0, 1800, "", 1), single],
             [(0, 900, "A", 3), single], 2, 2),
            ([a, b, (0, 900, "", 1), (0, 1800, "", 1)],
             [a, (0, 900, "B", 1), (0, 900, "", 1), (0, 900, "", 1)], 4, 4),
        )
        for shape, reference, deals, observations in cases:
            for tv, eng in ((shape, reference), (reference, shape)):
                with self.subTest(tv=tv, eng=eng):
                    r = grade(rows(tv, True), rows(eng, False))
                    self.assert_pairs(r, deals)
                    self.assertEqual(r.pnl_deltas, [0.] * observations)
            for field in ("entry_signal", "entry_identity"):
                with self.subTest(field=field, shape=shape):
                    tape = [lot(entry, exit_time=close, qty=qty, **{field: tag})
                            for entry, close, tag, qty in shape]
                    self.assertEqual(has_cross_entry_fifo_allocation(tape), observations == 1)

    def test_quantity_normalization_includes_both_two_percent_edges(self):
        for tv_qty, eng_qty, tv_pnl, eng_pnl, residual in (
            (98, 100, 98, 100, 0), (102, 100, 102, 100, 0),
            (.98, 1, 98, 100, 0), (1.02, 1, 102, 100, 0),
            (103, 100, 103, 100, 3), (97, 100, 97, 100, 3),
            (101, 100, 100, 100, 0), (0, 100, 98, 100, 2),
        ):
            with self.subTest(tv_qty=tv_qty, eng_qty=eng_qty):
                tv = [lot(i * 900, qty=tv_qty, pnl=tv_pnl) for i in range(6)]
                eng = [replace(t, qty=eng_qty, pnl=eng_pnl) for t in tv]
                r = grade(tv, eng)
                self.assert_pairs(r, 6)
                self.assertEqual(len(r.pnl_deltas), 6)
                self.assertAlmostEqual(r.pnl_p90, residual / tv_pnl)
                self.assertEqual(r.label, "strong" if residual else "excellent")

    def test_exit_drift_explains_only_the_signed_money_change(self):
        for direction, side in (("long", 1), ("short", -1)):
            tv = [lot(i * 900, direction=direction, entry_price=1000 - side * .125,
                      exit_price=1000, pnl=.125) for i in range(6)]
            for proceeds, residual in ((.0625, .0625), (.140625, .015625),
                                       (.1875, 0), (.190625, 0), (.125, 0)):
                with self.subTest(direction=direction, proceeds=proceeds):
                    eng = [replace(t, exit_price=1000 + side * .0625, pnl=proceeds) for t in tv]
                    r = grade(tv, eng)
                    self.assert_pairs(r, 6)
                    self.assertTrue(r.exit_ok)
                    self.assertEqual(r.pnl_deltas, [residual / .125] * 6)
                    self.assertEqual(r.label, "strong" if residual else "excellent")

        # With 101 engine units, only the comparison scaled to 100 TV units
        # explains the three-dollar improvement from a three-cent better exit.
        tv = [lot(i * 900, entry_price=1001, exit_price=1000, qty=100, pnl=-100)
              for i in range(6)]
        eng = [replace(t, qty=101, exit_price=1000.03, pnl=-97.97) for t in tv]
        r = grade(tv, eng)
        self.assert_pairs(r, 6)
        self.assertEqual((r.pnl_deltas, r.label), ([0.] * 6, "excellent"))

        # Outside the sizing band only the eight actual engine units can
        # explain an improvement: eight sixteenths of a dollar, not ten.
        tv = [lot(i * 900, entry_price=999.875, exit_price=1000, qty=10, pnl=1.25)
              for i in range(6)]
        for proceeds, delta in ((1.75, 0), (1.875, .625 / 1.25)):
            with self.subTest(proceeds=proceeds):
                eng = [replace(t, qty=8, exit_price=1000.0625, pnl=proceeds) for t in tv]
                r = grade(tv, eng)
                self.assert_pairs(r, 6)
                self.assertEqual(r.pnl_deltas, [delta] * 6)


if __name__ == "__main__":
    unittest.main()
