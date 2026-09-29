#!/usr/bin/env python3
"""TradingView's session calendar for the chart's symbol as a run input.

scripts/symbol_calendar.py turns a ``lab tv`` tape of the lane's synthetic
calendar probe (tests/fixtures/symbol_calendar/xc-cal-*) into a
``pineforge-symbol-calendar/v1`` document; run_strategy.py reads the one the
case runner names (PINEFORGE_RUN_SESSION_CALENDAR, held to its sha256) and
hands it to the Pine host as syminfo metadata ("symbol_calendar_*",
PineStrategyHost::set_symbol_calendar). A run whose lane names none is
unchanged.
"""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

import run_strategy
import symbol_calendar
from run_strategy import (
    SESSION_CALENDAR_ENV,
    SESSION_CALENDAR_SHA256_ENV,
    _session_calendar_metadata,
    build_runtime_provenance,
    inputs_run_kwargs,
    load_session_calendar,
)

FIXTURES = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "symbol_calendar"
H = 3_600_000


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class TapeTests(unittest.TestCase):
    def test_the_fixtures_calendars_are_their_tapes(self) -> None:
        for slug, symbol, days in (("xc-cal-xau15", "OANDA:XAUUSD", 1308),
                                   ("xc-cal-eurusd15-full", "OANDA:EURUSD", 1308),
                                   ("xc-cal-aapl15", "NASDAQ:AAPL", 275),
                                   ("xc-cal-f15", "NYSE:F", 275),
                                   ("xc-cal-nifty15", "NSE:NIFTY", 270)):
            with self.subTest(slug=slug):
                tape = FIXTURES / slug
                doc = symbol_calendar.calendar_document(tape)
                pinned = json.loads((tape / "calendar.json").read_text(encoding="utf-8"))
                self.assertEqual(doc, pinned)
                self.assertEqual(doc["symbol"], symbol)
                self.assertEqual(len(doc["sessions"]), days)
                self.assertEqual(doc["source"]["tvTradesSha256"], _sha(tape / "tv_trades.csv"))
                self.assertEqual(load_session_calendar(tape / "calendar.json"),
                                 (symbol, [tuple(s) for s in pinned["sessions"]]))

    def test_oanda_xauusd_holds_its_holidays_and_the_17_et_hour(self) -> None:
        _, sessions = load_session_calendar(FIXTURES / "xc-cal-xau15" / "calendar.json")
        days = dict(sessions)
        # Good Friday 2025-04-18: Thursday 17:00 EDT to Friday 17:00 EDT.
        good_friday = 1744923600000
        self.assertEqual(days[good_friday], good_friday + 24 * H)
        # Every day opens at 17:00 New York, the hour the feed holds no bar in.
        self.assertEqual(days[1751576400000], 1751662800000)   # Thu 07-03 -> Fri 07-04 17:00 EDT
        # TradingView's own correction: Thanksgiving 2024 closes 14:30 EST.
        self.assertEqual(days[1732744800000], 1732822200000)

    def test_a_reading_that_disagrees_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            csv = Path(tmp) / "tv_trades.csv"
            csv.write_text("Trade number,Type,Date and time,Signal\n"
                           "1,Entry long,2025-01-01 00:00,d|250101-0000|250101-0000~250102-0000\n"
                           "1,Exit long,2025-01-02 00:00,d|250101-0100|250101-0000~250102-0100\n",
                           encoding="utf-8")
            with self.assertRaises(ValueError):
                symbol_calendar.calendar_from_tape(csv)


class LoadTests(unittest.TestCase):
    def write(self, doc) -> Path:
        path = Path(self.tmp.name) / "calendar.json"
        path.write_text(json.dumps(doc), encoding="utf-8")
        return path

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_refusals(self) -> None:
        for doc in ({"schema": "other", "sessions": [[0, 1]]},
                    {"schema": "pineforge-symbol-calendar/v1", "sessions": []},
                    {"schema": "pineforge-symbol-calendar/v1", "sessions": [[2, 1]]},
                    {"schema": "pineforge-symbol-calendar/v1", "sessions": [[0, 5], [4, 9]]}):
            with self.subTest(doc=doc), self.assertRaises(ValueError):
                load_session_calendar(self.write(doc))


class MetadataTests(unittest.TestCase):
    def setUp(self) -> None:
        self.path = FIXTURES / "xc-cal-xau15" / "calendar.json"

    def test_none_without_the_env(self) -> None:
        self.assertEqual(_session_calendar_metadata({}, "OANDA:XAUUSD"), {})
        self.assertEqual(_session_calendar_metadata({SESSION_CALENDAR_ENV: ""}, None), {})

    def test_the_days_as_syminfo_metadata(self) -> None:
        meta = _session_calendar_metadata(
            {SESSION_CALENDAR_ENV: str(self.path),
             SESSION_CALENDAR_SHA256_ENV: _sha(self.path)}, "OANDA:XAUUSD")
        _, sessions = load_session_calendar(self.path)
        self.assertEqual(meta["symbol_calendar_days"], float(len(sessions)))
        self.assertEqual(len(meta), 1 + 2 * len(sessions))
        for i in (0, 500, len(sessions) - 1):
            self.assertEqual(meta[f"symbol_calendar_open:{i}"], float(sessions[i][0]))
            self.assertEqual(meta[f"symbol_calendar_close:{i}"], float(sessions[i][1]))
            # Exact: epoch milliseconds are integers a double holds.
            self.assertEqual(int(meta[f"symbol_calendar_close:{i}"]), sessions[i][1])

    def test_refusals(self) -> None:
        with self.assertRaises(ValueError):   # not the pinned bytes
            _session_calendar_metadata({SESSION_CALENDAR_ENV: str(self.path),
                                        SESSION_CALENDAR_SHA256_ENV: "0" * 64}, None)
        with self.assertRaises(ValueError):   # another symbol's calendar
            _session_calendar_metadata({SESSION_CALENDAR_ENV: str(self.path)}, "OANDA:EURUSD")
        with self.assertRaises(FileNotFoundError):
            _session_calendar_metadata({SESSION_CALENDAR_ENV: str(self.path) + ".missing"}, None)

    def test_the_run_fingerprint_names_it(self) -> None:
        meta = _session_calendar_metadata({SESSION_CALENDAR_ENV: str(self.path)}, "OANDA:XAUUSD")
        _, sessions = load_session_calendar(self.path)
        lines = "".join(f"{o},{c}\n" for o, c in sessions).encode("ascii")
        runtime = build_runtime_provenance({"syminfo_metadata": {"qty_step": 0.01, **meta}}, None)
        self.assertEqual(runtime["session_calendar"],
                         {"days": len(sessions), "sessions_sha256": hashlib.sha256(lines).hexdigest()})
        plain = build_runtime_provenance({"syminfo_metadata": {"qty_step": 0.01}}, None)
        self.assertNotIn("session_calendar", plain)
        self.assertEqual({k: v for k, v in runtime.items() if k != "session_calendar"}, plain)

    def test_inputs_run_kwargs_carries_it(self) -> None:
        params = {"runtime_overrides": {"tickerid": "OANDA:XAUUSD", "qty_step": 0.01},
                  "input_tf": "15", "script_tf": "15"}
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            chart = d / "chart.csv"
            chart.write_text("timestamp,open,high,low,close,volume\n", encoding="utf-8")
            _, plain = inputs_run_kwargs(params, d, chart, env={})
            _, fed = inputs_run_kwargs(params, d, chart,
                                       env={SESSION_CALENDAR_ENV: str(self.path)})
        self.assertEqual(plain["syminfo_metadata"], {"qty_step": 0.01})
        self.assertEqual(fed["syminfo_metadata"]["qty_step"], 0.01)
        self.assertEqual(fed["syminfo_metadata"]["symbol_calendar_days"], 1308.0)
        self.assertEqual({k: v for k, v in fed.items() if k != "syminfo_metadata"},
                         {k: v for k, v in plain.items() if k != "syminfo_metadata"})


if __name__ == "__main__":
    unittest.main()
