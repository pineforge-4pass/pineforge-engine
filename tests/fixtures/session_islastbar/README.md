# Session-bar tapes (R5 lane E25)

TradingView's own answer to which chart bars `session.islastbar` and
`session.isfirstbar` hold on. Lane E20 found that since R4 slice C
(`73817c1d`) an AGGREGATED run (input timeframe finer than the chart's)
reported `session.islastbar` on every in-session bar;
`tests/test_session_islastbar_aggregation.cpp` replays the days below through
the Pine adapter on both the aggregated and the chart-timeframe path.

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The sha256 of each
tape is `metrics.json` `tvTradesCsvHash`. Every probe enters with
`process_orders_on_close=true` on the flagged bar and closes on the next one,
so each trade's entry time IS a bar TradingView flagged.

| probe | chart | window | trades | TradingView flags |
|---|---|---|---|---|
| `e25-f-islastbar` | NYSE:F 15 | 2025-04-10 .. 2026-04-17 | 255 | `session.islastbar` on the last bar of every session day: 15:45 ET on 252 days, 12:45 ET on the three half days (2025-07-03, 2025-11-28, 2025-12-24) |
| `e25-f-isfirstbar` | NYSE:F 15 | same | 255 | `session.isfirstbar` on 09:30 ET of every session day |
| `e25-eth-islastbar` | BINANCE:ETHUSDT.P 15 | 2025-04-20 .. 2026-04-25 | 370 | `session.islastbar` on 23:45 UTC of every day (a 24x7 symbol's session day ends at midnight) |

The flag belongs to the session DAY, not to a gap in the data. The registry's
NYSE:F feeds hold regular-hours bars only, so the bar after 15:45 is the next
day's 09:30, in session again: a rule that asks only whether the next bar is
out of session never sees these boundaries, and neither does it on a 24x7
feed. That shared residual of the chart-timeframe and the aggregated path is
recorded, not fixed, by the witness (its header says why).

`bars.inc` holds the replayed days, rows copied from the feeds the tapes ran
on (`exec/E25-probes/gen_inc.py`):

- NYSE:F 2025-07-02, 2025-07-03 (half day), 2025-07-07: the registry lane
  `f-15` feeds, 1m `finer` sha256
  `50bccb38da9cf08aeaca6c19ff9c3a266f00885fe2894c3e470b98f94e24bcc8` and 15m
  `chart` sha256
  `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`
  (`lab evidence get <sha> --out <file>`). No minute is missing on those days,
  and every 15m chart bar is the aggregate of its fifteen 1m bars.
- BINANCE:ETHUSDT.P 2025-06-10 23:00 .. 06-11 00:59 UTC: the corpus 1m feed
  at gitlink 442d497 (sha256
  `db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`) and
  the 15m feed `scripts/derive_corpus_feeds.py` derives from it (sha256
  `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`).
