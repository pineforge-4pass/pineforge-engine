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


if __name__ == "__main__":
    unittest.main()
