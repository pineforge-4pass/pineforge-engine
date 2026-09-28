# A 24-hour day across a daylight-saving switch (lane W12-ENG-TIME)

A symbol whose session day lasts 24 hours on the wall clock and that trades
on the day a switch falls on closes its daily bar at the next day's same
wall-clock time. That is 23 or 25 hours after it opens, not its open plus 24
hours:

- CAPITALCOM:BTCUSD (`America/New_York`, daily bars stamped 17:00 every day,
  a `1700-1700` session) closes its Saturday 2025-03-08 17:00 EST bar at
  Sunday 17:00 EDT, 82,800,000 ms later.
- ACTIVTRADES:BTCUSD (`Europe/Amsterdam`, daily bars stamped at midnight)
  closes its Sunday 2025-03-30 00:00 CET bar at Monday 00:00 CEST.

ACTIVTRADES closes Fridays at 23:00 and opens Saturdays at 09:00. That is a
venue schedule no session string spells, so the tests read its Sunday to
Thursday bars only. A 24x7 symbol in New York is not on TradingView; the
tests pin that case from the same rule.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`),
byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json` and `meta.json`. The probes alternate two positions and
close one on every chart bar. Each exit's comment spells what the script read
on the bar before its fill (`tests/exit_comment_tape.hpp`):

- `w12-tclose-cap1d` is the probe of `tests/fixtures/time_close_function`.
- The `w12-tzscan-*` probe spells `syminfo.timezone`, then
  `time_close - time` in milliseconds, then the exchange-clock weekday and
  HHMM of `time`. It was run to find a 24-hour symbol in a daylight-saving
  zone. BINANCE, COINBASE, GEMINI, BITSTAMP, KRAKEN, CRYPTO, INDEX, BNC and
  CRYPTOCAP report `Etc/UTC`.

`tests/test_engine_trade_accessors.cpp` (`test_chart_time_close_dst_tapes`)
replays the chart's `time_close`, and `tests/test_session_predicates.cpp`
(`test_time_close_function_tapes`, `test_wall_clock_day_24x7_new_york`)
replays `time("D")` and `time_close("D")`.

| tape | chart | range | trades | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `w12-tclose-cap1d` | CAPITALCOM:BTCUSD 1D | 2025-01-01 .. 2025-07-01 | 180 | covered | `f5eaabcfef47c45469128b11019e361691f2ae010b686c4f4821833d8f371b5b` | `0143b9b1dfc3` |
| `w12-tzscan-capitalcom-btcusd` | CAPITALCOM:BTCUSD 1D | 2025-03-01 .. 2025-04-05 | 34 | covered | `8b0dcb3fae0b5ff665aaf63b39febb256915d728b089f36303fd04f7cf679285` | `f49a41362632` |
| `w12-tzscan-activtrades-btcusd` | ACTIVTRADES:BTCUSD 1D | 2025-03-01 .. 2025-04-05 | 34 | narrower-than-requested | `7df372964b6798a0efdc2eeb88b5d8d32e6337d860ea6b12a318cae139ad6dff` | `f49a41362632` |
