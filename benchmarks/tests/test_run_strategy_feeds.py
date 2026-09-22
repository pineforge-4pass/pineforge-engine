#!/usr/bin/env python3
"""scripts/run_strategy.py derives the corpus feeds only for a run that reads one.

A benchmark slot runs on its own feed (benchmarks/assets/data/ETHUSDT_15.csv)
and never reads corpus/data/derived/, so a checkout with only the
benchmarks/assets submodule -- no corpus 1m feed to derive from -- must run
it. Each case drives run_strategy.main() on a scratch slot with
ensure_derived() replaced by a sentinel. The slot has no strategy library, so
a run that needs no derived feed ends at the library load, after every feed it
reads has been resolved; a run that needs one ends at the sentinel.

    python3 benchmarks/tests/test_run_strategy_feeds.py
"""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import run_strategy  # noqa: E402

BAR_MS = 900_000
START_MS = 1_735_689_600_000  # 2025-01-01 00:00 UTC
TAPE_HEADER = ("Trade number,Type,Date and time,Signal,Price USDT,Size (qty),"
               "Size (value),Net PnL USDT")
TAPE_ROWS = (  # two closed long trades inside the feed below
    "1,Exit long,2025-01-02 06:00,Exit,3400,1,3400,10",
    "1,Entry long,2025-01-02 00:45,Long,3390,1,3390,10",
    "2,Exit long,2025-01-03 21:30,Exit,3380,1,3380,-5",
    "2,Entry long,2025-01-03 07:45,Long,3385,1,3385,-5",
)


class DerivedFeedRead(Exception):
    """Raised by the stand-in for ensure_derived()."""


def _derive_sentinel() -> None:
    raise DerivedFeedRead


class DerivedFeedGate(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="bench-feeds-")
        self.addCleanup(tmp.cleanup)
        self.slot = Path(tmp.name) / "001-scratch-slot"
        self.slot.mkdir()
        self.feed = Path(tmp.name) / "ETHUSDT_15.csv"
        rows = ["timestamp,open,high,low,close,volume"]
        for i in range(400):
            rows.append(f"{START_MS + i * BAR_MS},3400,3410,3390,3400,1")
        self.feed.write_text("\n".join(rows) + "\n")

    def write_tape(self) -> None:
        (self.slot / "tv_trades.csv").write_text(
            "\n".join((TAPE_HEADER,) + TAPE_ROWS) + "\n")

    def write_inputs(self, inputs: dict) -> None:
        (self.slot / "inputs.json").write_text(json.dumps(inputs))

    def run_main(self, *argv: str) -> BaseException:
        """run_strategy.main() on the slot; the exception it ended with."""
        out = self.slot / "pineforge_trades.csv"
        with mock.patch.object(run_strategy, "ensure_derived", _derive_sentinel), \
             mock.patch.object(sys, "argv", ["run_strategy.py", str(self.slot),
                                             "--output", str(out), *argv]), \
             contextlib.redirect_stdout(io.StringIO()):
            try:
                run_strategy.main()
            except (DerivedFeedRead, OSError) as exc:
                return exc
        self.fail("run_strategy.main() returned without loading a strategy library")

    def assert_no_derived_feed(self, *argv: str) -> None:
        exc = self.run_main(*argv)
        self.assertNotIsInstance(
            exc, DerivedFeedRead,
            "run_strategy.py materialized corpus/data/derived/ for a run on its own feed")
        self.assertIn("strategy.so", str(exc))

    def assert_derived_feed(self, *argv: str) -> None:
        self.assertIsInstance(self.run_main(*argv), DerivedFeedRead)

    # A run on its own feed never touches the corpus.

    def test_bench_slot_on_its_own_feed(self) -> None:
        """run_all.sh's command: the slot's tape, the bench feed."""
        self.write_tape()
        self.assert_no_derived_feed("--ohlcv", str(self.feed))

    def test_own_feed_and_own_emit_window(self) -> None:
        self.assert_no_derived_feed("--ohlcv", str(self.feed),
                                    "--emit-window-ohlcv", str(self.feed))

    def test_own_feed_untrimmed(self) -> None:
        self.assert_no_derived_feed("--ohlcv", str(self.feed), "--no-trim-output")

    def test_inputs_json_names_its_own_feed(self) -> None:
        """inputs.json's ohlcv_csv replaces the (derived) default --ohlcv."""
        self.write_tape()
        self.write_inputs({"ohlcv_csv": str(self.feed)})
        self.assert_no_derived_feed()

    # A run that reads a derived feed still gets it materialized first.

    def test_default_feed_is_derived(self) -> None:
        self.write_tape()
        self.assert_derived_feed()

    def test_tapeless_run_reads_the_reference_window(self) -> None:
        """No tape and no --emit-window-ohlcv: the window is the reference feed's."""
        self.assert_derived_feed("--ohlcv", str(self.feed))

    def test_inputs_json_names_a_derived_chart_feed(self) -> None:
        self.write_tape()
        self.write_inputs({"ohlcv_csv": str(run_strategy.DERIVED_15M)})
        self.assert_derived_feed("--ohlcv", str(self.feed))

    def test_inputs_json_names_a_derived_auxiliary_feed(self) -> None:
        self.write_tape()
        self.write_inputs({"aux_security_ohlcv_csv": str(run_strategy.DERIVED_15M),
                           "aux_security_input_tf": "15"})
        self.assert_derived_feed("--ohlcv", str(self.feed))

    def test_derived_emit_window(self) -> None:
        self.assert_derived_feed("--ohlcv", str(self.feed),
                                 "--emit-window-ohlcv", str(run_strategy.DERIVED_15M_WINDOW))


if __name__ == "__main__":
    unittest.main()
