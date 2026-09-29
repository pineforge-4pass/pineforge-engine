# An aggregated chart's admission receipts name their command's bar (R5 lane FIX-E1E2)

TradingView's own trades for two synthetic scripts written for this lane, each
run on its 60-minute chart and replayed by the engine on that chart's bars and
on the same two days aggregated from 15- and 5-minute bars.
`tests/test_aggregated_admission_receipt_tapes.cpp` replays every tape.

Each directory is one `lab tv --no-note` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the
sha256 of `tv_trades.csv`), `meta.json`. Chart BINANCE:ETHUSDT.P 60, range
2025-04-01 .. 2025-05-01; both scripts trade only on 2025-04-03 and 2025-04-04
(UTC). No closed or scraped strategy is involved.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `fe2-pooc-mc-cost-m50` | process_orders_on_close, commission 0.04 %, margin_long 50, the v6 default quantity (100 % of equity) | 21 | `1027af8afac96a194eba7b2dd3e3a394ffe8f4935f3e04f405c9802fcacb5615` |
| `fe2-pooc-pair` | process_orders_on_close, pyramiding 0, fixed quantities | 24 (23 close inside the replayed bars) | `3adbef2a8187e6c2ec8c3f28249521a7a5334886f58f32b8876171e9249b2c0b` |

What they show:

- `fe2-pooc-mc-cost-m50`: each cell shorts at 100 % of equity and reverses on
  the first bar whose high is above the short's average price. TradingView
  margin-calls the short on that bar (at the opening print and at the high)
  and books the reversal at the close. In the engine the margin call is booked
  after the script's reversal command, so it revises the pending
  default-quantity long (`refresh_pending_sizing_after_margin`). The long
  margin of 50 % keeps the reversal long clear of a margin call of its own.
- `fe2-pooc-pair`: flat pairs of opposite fixed-quantity market entries on one
  bar. A pair over the equity (80 % legs) keeps only its first call; a pair
  within it (30 % legs) fills both at the close, the second reversing the
  first, which TradingView books as a trade that opens and closes on that bar.
  The engine reviews the pair at the terminal checkpoint
  (`apply_terminal_explicit_market_policy`).
- On an aggregated chart both runs stopped at 35db01c8 with
  `admission receipt requires its exact earlier command`: the command
  observation carries the row's projection bar (the input slot) and the
  sizing-revision and review receipts carried the script-bar index, which an
  aggregated chart keeps smaller. With the receipts in the command's bar space
  the pair's same-bar round trips were still dated at the next bar's open on
  an aggregated chart: the reversing call closed the first leg's lot before
  the first leg's own notification re-dated it to the chart bar's open.

## Bars

`bars.inc` holds BINANCE:ETHUSDT.P 2025-04-03 00:00 .. 2025-04-04 23:59 UTC at
5, 15 and 60 minutes, resampled from the corpus 1m feed (sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`, no gap in
the span) by `scripts/derive_corpus_feeds.py`'s rule; the 15-minute rows equal
the committed 15m derived feed's (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`).
`FIX-E1E2-scratch/tools/gen_bars_inc.py` wrote it.
