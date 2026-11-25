"""Check which logical trades contribute to the corpus grade."""

import csv
import json
import tempfile
import unittest
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path

from test_verify_corpus_count_rule import _checked_path
from test_verify_corpus_same_bar import START, trade
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


def analyze(tv, engine, *, trim=0, bars=()):
    with tempfile.TemporaryDirectory(dir=_checked_path(Path.cwd())) as tmp:
        files = {name: _checked_path(Path(tmp) / name) for name in (
            "inputs.json", "strategy.pine", "tv_trades.csv", "engine_trades.csv",
            "ohlcv.csv", "data.csv", "candles.csv",
        )}
        print("Grader fixture paths:", *files.values(), sep="\n", flush=True)
        meta = {"tv_trades_csv_tz": "utc", "trim_bars": trim}
        files["inputs.json"].write_text(json.dumps(meta), encoding="utf-8")
        if bars:
            with files["ohlcv.csv"].open("x", encoding="utf-8") as stream:
                stream.write("timestamp,open\n")
                for minute in bars:
                    stream.write(f"{START + 60 * minute},100\n")
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

    def test_no_alignment_reports_the_gated_pools(self):
        bars = range(0, 90, 15)
        tv = [trade(m) for m in bars]
        shorts = [trade(m, direction="short") for m in bars]
        for name, rows, engine, eng_inside, closed in (
            ("opposite directions", tv, shorts, 4, 4),
            ("one engine row inside", tv, [shorts[0], shorts[2], shorts[5]], 1, 4),
            ("only TV, one still open", [*tv[:2], trade(30, mark=True), *tv[3:]], [], 0, 3),
        ):
            with self.subTest(name=name):
                r = analyze(rows, engine, trim=1, bars=bars)  # Keep minutes 15 through 60.
                self.assertTrue(r.tv_exists and r.eng_exists and r.no_aligned_trades)
                self.assertEqual((r.tv_raw_count, r.eng_raw_count), (6, len(engine)))
                self.assertEqual((r.tv_count, r.eng_count, r.matched_count), (6, len(engine), 0))
                self.assertEqual((r.label, r.grading_policy), ("minimal", None))
                self.assertEqual((r.tv_gate_count, r.eng_gate_count), (4, eng_inside))
                self.assertEqual((r.count_abs_delta, r.gross_unmatched_count, r.net_count_abs_delta),
                                 (4 + eng_inside, 4 + eng_inside, 4 - eng_inside))
                self.assertEqual((r.count_delta, r.count_ok), ((4 + eng_inside) / 4, False))
                self.assertEqual((r.coverage_tv_count, r.unmatched_total, r.coverage),
                                 (closed, closed, 0.0))
                self.assertEqual((r.gating_matched_count, r.unmatched_in_window), (0, 4))
                self.assertEqual((r.trim_bars, r.warmup_bars), (1, 0))
                self.assertEqual(r.bounds, ((START + 15 * 60) * 1000, (START + 60 * 60) * 1000))

    def test_exterior_pairs_do_not_count_toward_interior_coverage(self):
        bars = range(0, 90, 15)
        tv = [trade(m) for m in bars]
        for interior_side, pairs, tier in (("long", 2, "excellent"), ("short", 0, "weak")):
            with self.subTest(interior_side=interior_side):
                engine = [trade(m, direction=interior_side if m in (30, 45) else "long")
                          for m in bars]
                r = analyze(tv, engine, trim=2, bars=bars)
                self.assertEqual((r.tv_gate_count, r.eng_gate_count, r.coverage_tv_count), (2, 2, 2))
                self.assertEqual(r.matched_count, 4 + pairs)
                self.assertEqual(r.count_abs_delta, 2 * (2 - pairs))
                self.assertEqual(r.gating_matched_count, pairs)
                self.assertEqual((r.coverage, r.unmatched_total, r.unmatched_in_window),
                                 (pairs / 2, 2 - pairs, 2 - pairs))
                self.assertEqual(r.label, tier)

    def test_mark_only_interior_excludes_exterior_close_metrics(self):
        bars = range(0, 105, 15)  # Trimming two bars keeps minutes 30 through 60.
        for name, mark in (("inside close", False), ("inside mark", True), ("exterior FIFO", True)):
            with self.subTest(name=name):
                tv = [trade(0), trade(15), trade(75), trade(90), trade(90, price=110)]
                engine = [replace(t, exit_price=t.exit_price + 4, pnl=5) for t in tv]
                if name == "exterior FIFO":
                    # The minute-zero entry splits its close; the second half
                    # closes with the minute-15 entry. Both tapes agree on it.
                    first = replace(tv[0], qty=0.5, pnl=0.5)
                    tv[:1] = [first, replace(first, exit_time=tv[1].exit_time)]
                    engine = [replace(t, entry_price=t.entry_price + 0.01) for t in tv]
                r = analyze([*tv, trade(45, mark=mark)], [*engine, trade(45, mark=mark)],
                            trim=2, bars=bars)
                self.assertEqual((r.matched_count, r.open_mark_pairs, r.gating_matched_count),
                                 (6, int(mark), 1))
                self.assertEqual((r.count_abs_delta, r.coverage_tv_count, r.coverage), (0, 1, 1.0))
                exterior = [(t.entry_price, e.entry_price, t.pnl, e.pnl) for t, e in r.matched
                            if t.entry_time == START + 75 * 60]
                self.assertEqual(exterior, [(100, 100.01, 1, 1)] if name == "exterior FIFO"
                                 else [(100, 100, 1, 5)])
                row = r.report_row()
                self.assertEqual(row["metric_observations"],
                                 {"entry": 1, "exit": int(not mark), "pnl": int(not mark)})
                self.assertEqual(row["entry_p100"], 0.0)
                self.assertEqual((row["exit_p100"], row["pnl_p100"]),
                                 (None, None) if mark else (0.0, 0.0))
                self.assertEqual(r.label, "excellent")


if __name__ == "__main__":
    unittest.main()
