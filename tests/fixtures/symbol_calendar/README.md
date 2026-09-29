# TradingView's session calendar for a chart's symbol (lane XAU-CAL)

TradingView reads the bar after the current one from its calendar for the
chart's symbol, not from the chart's bars: `time("", "", -1)`,
`time_close("", -1)`, `time("", -2)`, `time("D", 0, -1)`, a timeframe's bar
stepped back (`time("60", 0, 1)`), `session.islastbar` and
`session.islastbar_regular`. The first bar of a session day
(`session.isfirstbar`, `_regular`) is the chart's: the bar after a bar of
another day.

A symbol's calendar is not always its feed:

- OANDA:XAUUSD's is a session every weekday from 17:00 ET to 17:00 ET,
  holidays included. Its feed has no bar from 17:00 to 18:00 ET, none on Good
  Friday, Christmas or New Year's Day, and stops early on US holidays (Memorial
  Day, Juneteenth, Independence Day, Labor Day, Thanksgiving Friday, Christmas
  Eve, MLK and Presidents' Day). TradingView's own correction: Thanksgiving
  2024 closes 14:30 ET (the only one in five years). Against the lane's
  15-minute feed over 2025-04-01 .. 2026-05-01: 4 calendar days with no feed
  bar, 278 days the feed opens an hour later, 8 days it closes earlier.
- OANDA:EURUSD's likewise; its feed has no bar on 7 Christmas and New Year
  days (2021-05 .. 2026-05) and opens 2 days late.
- NASDAQ:AAPL's, NYSE:F's and NSE:NIFTY's carry their exchanges' holidays and
  early closes. Their lanes' feeds hold exactly those sessions over
  2025-04-01 .. 2026-05-01: no day, open or close parts. Those lanes are given
  no calendar and read the feed, as they always have.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`) of the
lane's own synthetic script, byte for byte: `strategy.pine`, `tv_trades.csv`
(times at UTC+8), `metrics.json` and `meta.json`.

- `xc-cal-*`: the first chart bar of every session day spells that day's
  `time("D")` and `time_close("D")` and the next three days'
  (`timeframe_bars_back` -1 .. -3), so a day the chart holds no bar of is read
  from the day before it. `calendar.json` beside each tape is the calendar
  `scripts/symbol_calendar.py` makes of it (`pineforge-symbol-calendar/v1`:
  the tape's provenance and one `[open_ms, close_ms]` per session day);
  `scripts/test_symbol_calendar.py` checks that each is its tape. The XAUUSD
  and EURUSD documents cover their lanes' feeds (2021-05 .. 2026-05) and are
  the run inputs the case runner names (`PINEFORGE_RUN_SESSION_CALENDAR`,
  `scripts/run_strategy.py`).
- `xc-flags-*`: every chart bar spells, in UTC, the next bar's open and close,
  its weekday, the bar two ahead, the next day's open and the four session
  flags. `tests/test_symbol_calendar_tapes.cpp` replays them through the Pine
  host: with the symbol's calendar installed
  (`PineStrategyHost::set_symbol_calendar`) every cell of the five XAUUSD
  windows is TradingView's, where the feed parts on 391; AAPL's Thanksgiving
  week is TradingView's with its calendar and without it. The same test
  replays lane TAIL-E's `te-time-bb-*-xau15` tapes (`tests/fixtures/time_bars_back`)
  with the calendar: none of their 12,144 cells parts, where the feed parts on
  484.

| tape | chart | range | rows | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `xc-cal-xau15` | OANDA:XAUUSD 15 | 2021-05-01 .. 2026-05-01 | 1292 | narrower-than-requested | `fd3cbd9050fed0407f7e37fd3168cd2875833bfec544b14ed3b71aafaee61c96` | `1ef2fe99be7d` |
| `xc-cal-eurusd15-full` | OANDA:EURUSD 15 | 2021-05-01 .. 2026-05-01 | 1298 | narrower-than-requested | `b6e620f7f38e80261686afedbe655e6f57264d84ddd5c69e6c7221f6695d10d2` | `1ef2fe99be7d` |
| `xc-cal-aapl15` | NASDAQ:AAPL 15 | 2025-04-01 .. 2026-05-01 | 272 | covered | `ea42d0d3f334c98b042013ac465aa8a7c0ef5a83abdb7d7dec9f937559fa5701` | `1ef2fe99be7d` |
| `xc-cal-f15` | NYSE:F 15 | 2025-04-01 .. 2026-05-01 | 272 | covered | `19581862f0606218f550e2b2e6b28150fef88e6373f29339f9d1f34820830ee7` | `1ef2fe99be7d` |
| `xc-cal-nifty15` | NSE:NIFTY 15 | 2025-04-01 .. 2026-05-01 | 268 | covered | `e7e130901878a945bed94465d3ddd5170750082464e44fa520850227f2917701` | `1ef2fe99be7d` |
| `xc-flags-xau15-goodfriday` | OANDA:XAUUSD 15 | 2025-04-15 .. 2025-04-23 | 460 | covered | `0a4458256bb70eb4d682e015228d9c009de17bcbbae6231c4c1b2749da529d78` | `a1466926b16e` |
| `xc-flags-xau15-memorial` | OANDA:XAUUSD 15 | 2025-05-22 .. 2025-05-29 | 450 | covered | `af41d149b6d622eb09645f99d7d277561a029d919b46b924283058d92a461fed` | `a1466926b16e` |
| `xc-flags-xau15-jul4` | OANDA:XAUUSD 15 | 2025-07-02 .. 2025-07-09 | 444 | covered | `94add608c1a37dc03068318a6b48b7b7ac05be382dca034b5b02c9de62224007` | `a1466926b16e` |
| `xc-flags-xau15-thanksgiving` | OANDA:XAUUSD 15 | 2025-11-25 .. 2025-12-02 | 452 | covered | `d71e85836ca22857905eb2c528e4190103a421c619c40a517493459286785eac` | `a1466926b16e` |
| `xc-flags-xau15-yearend` | OANDA:XAUUSD 15 | 2025-12-22 .. 2026-01-06 | 816 | covered | `a928400f27b16cdcee86d6cd4ff28ad3a6e541f0675414f5ecad7f323e06fa45` | `a1466926b16e` |
| `xc-flags-aapl15-thanksgiving` | NASDAQ:AAPL 15 | 2025-11-25 .. 2025-12-02 | 92 | covered | `746518b4cff489e9a91033c0ee1f6cee07a097c759b04aa18733b31cc7fd8d93` | `a1466926b16e` |
