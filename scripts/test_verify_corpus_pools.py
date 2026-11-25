"""Check which logical trades contribute to the corpus grade."""

import csv
import json
import tempfile
import unittest
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path

from test_verify_corpus_count_rule import _checked_path
from test_verify_corpus_same_bar import trade
from verify_corpus import analyze_strategy


def write_trades(path, rows, *, tv):
    def stamp(seconds):
        return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%d %H:%M")

    with path.open("x", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("Trade #", "Type", "Date and time", "Price", "Qty", "Net PnL",
                         "Signal" if tv else "Engine range-end"))
        for n, row in enumerate(rows, 1):
            writer.writerow((n, "Entry " + row.direction, stamp(row.entry_time), row.entry_price,
                             row.qty, row.pnl, row.entry_signal if tv else ""))
            mark = ("Open" if tv else "open") if row.open_mark else ""
            writer.writerow((n, "Exit " + row.direction, stamp(row.exit_time), row.exit_price,
                             row.qty, row.pnl, mark))


def analyze(tv, engine):
    with tempfile.TemporaryDirectory(dir=_checked_path(Path.cwd())) as tmp:
        files = {name: _checked_path(Path(tmp) / name) for name in (
            "inputs.json", "strategy.pine", "tv_trades.csv", "engine_trades.csv",
            "ohlcv.csv", "data.csv", "candles.csv",
        )}
        print("Grader fixture paths:", *files.values(), sep="\n", flush=True)
        files["inputs.json"].write_text(json.dumps({"tv_trades_csv_tz": "utc"}), encoding="utf-8")
        write_trades(files["tv_trades.csv"], tv, tv=True)
        write_trades(files["engine_trades.csv"], engine, tv=False)
        return analyze_strategy(Path(tmp))


class TradePoolTests(unittest.TestCase):
    def test_closed_sibling_remains_in_coverage(self):
        closed = replace(trade(0), qty=0.5, entry_signal="A")
        mark = replace(trade(0, mark=True), qty=0.5, exit_price=102, entry_signal="A")
        for name, fragments, deals, missed in (
            ("wholly open", [mark, mark], 1, 0),
            ("closed sibling", [closed, replace(mark, entry_signal="B")], 2, 1),
            ("partly closed", [closed, mark], 1, 1),
        ):
            with self.subTest(name=name):
                tv = [*fragments, *(trade(15 * i) for i in range(1, 7))]
                r = analyze(tv, [trade(15 * i) for i in range(1, 7)])
                self.assertEqual((r.tv_raw_count, r.eng_raw_count), (6 + deals, 6))
                self.assertEqual((r.matched_count, r.open_mark_pairs, r.count_abs_delta), (6, 0, 0))
                self.assertEqual((r.coverage_tv_count, r.unmatched_total, r.coverage),
                                 (6 + missed, missed, 6 / (6 + missed)))


if __name__ == "__main__":
    unittest.main()
