# The script's position view after a close fill under process_orders_on_close

Under `process_orders_on_close`, TradingView executes the orders a script
places in its pass at the bar's close, after that pass. An order that fills
there is invisible to the pass that placed it: for the rest of the pass,
`strategy.position_size` and `strategy.position_avg_price` keep their pre-fill
values, and the next bar's pass is the first to read the fill.

The adapter fills two kinds of call at the call itself, ahead of the orders
placed after them: an ordinary `strategy.close_all()`, and a re-issued
`strategy.exit` of a one-lot short whose stop or limit is already through the
bar's close (the current-close path of `PineExecutionAdapter::exit()`). `close_all`
froze the script's position view (KI-64); the re-issued exit did not. The rest
of its pass read `strategy.position_size == 0` and a reset average. A late
observer, `strategy.position_size[1] < 0 and strategy.position_size == 0`,
then fired one bar early. Behind an exit cooldown, it blocked the next bar's
entry.

The scraped `job-2947-seanvtran-svt-30m-options-swing-indicator` on
BINANCE:ETHUSDT.P 15 lost its 2026-01-08 23:30 UTC short and every trade after
it (668 TradingView rows) to that cooldown.

## The tapes

All are `lab tv --no-note` exports (maintainers' private pineforge-workflow,
channel `ws-report-v1`) on BINANCE:ETHUSDT.P 15, 2026-01-08 .. 2026-01-10. Each
was exported twice, the two exports are byte-identical, and `rangeProof` is
`covered` on both. Every prediction was written before its export, and
TradingView equals it on every row. Each directory holds `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `configuration.json`, `provenance.json` and
the frozen `generated.cpp` (codegen c1df8a6). `bars/` holds the chart's
from..to window. That is the history TradingView loaded for the script, so
`ta.atr` seeds at its start.

Every script declares `pyramiding=0` and `process_orders_on_close=true`, with
no slippage; only `short-reissued-exit-reads` charges a commission. A seed
short of 1 enters at 19:45 UTC (3110.13). The scripts that re-issue their exit
place `strategy.exit("x", "seed", stop=3200)` on every bar before 23:15
(`limit=3000` in `short-reissued-limit-exit`). At 23:15 (close 3116.97), each
makes the call below and then reads the position. A `strategy.close_all()` at
01:00 ends every script.

| tape | 23:15 call, then | TradingView | engine before the rule |
|---|---|---|---|
| `short-reissued-exit-size` | exit re-issued with stop 3109; `seen := strategy.position_size`; 23:30 enters 1 if `seen < 0`, 2 if `seen == 0` | x 23:15 @3116.97; 1 at 23:30 | 2 at 23:30 |
| `long-reissued-exit-size` | the long mirror, stop 3125 | 1 at 23:30 | = TV |
| `short-close-size` | `strategy.close("seed")` | 1 at 23:30 | = TV |
| `short-close-all-size` | `strategy.close_all()` | 1 at 23:30 | = TV (KI-64) |
| `short-first-exit-size` | first exit call, stop 3109 | 1 at 23:30 | = TV |
| `short-reissued-exit-cooldown` | exit re-issued with stop 3109; a late observer sets the exit bar; a 4-bar exit cooldown gates a 23:30 entry | the 23:30 entry | no 23:30 entry |
| `atr-exit-cooldown-pyramiding-0` | 100 % of equity (capital 69001.53831500027, margin 100), the seed's exit stop re-issued every bar from `strategy.position_avg_price` and a rolling ATR, the same observer and cooldown | margin calls .1892 and .3432, 21.6536 out at 23:15; the 23:30 entry split .2172 (margin call 23:45) + 21.934 | the 4 rows of the 23:30 entry missing |
| `short-reissued-exit-average` | exit re-issued with stop 3109; `strategy.position_avg_price` read right after; 23:30 enters `AVGNA` 2 when na, `AVGPRE` 1 at the pre-fill 3110.13, `AVGOTHER` 3 otherwise | `AVGPRE` 1 | `AVGNA` 2 (with the size frozen only: `AVGOTHER` 3) |
| `short-reissued-limit-exit` | exit re-issued with limit 3000 before 23:15 and limit 3120 (through the close) at 23:15; size and average read right after; 23:30 enters `SAWPOSPRE` 1 when short at 3110.13, `SAWPOSOTHER` 3 when short at another average, `SAWFLAT` 2 when flat | x 23:15 @3116.97; `SAWPOSPRE` 1 | `SAWFLAT` 2 |
| `short-reissued-exit-reads` | as `short-reissued-exit-size` with a 0.1 % commission; `strategy.opentrades`, `closedtrades`, `openprofit`, `netprofit` and `equity` read right after, encoded in the 23:30 entry id | `O1C0PpreNpreEpre`: 1, 0, -6.84, -3.11013, 99990.04987 | `O0C1PzeroNpostEpost` (known divergence) |
| `short-close-all-average` | `strategy.close_all()`, then the average read as in `short-reissued-exit-average` | `AVGPRE` 1 | `AVGOTHER` 3 (known divergence) |

`atr-exit-cooldown-pyramiding-0` is the `cooldown-atr-entry-clear` control of
`tests/fixtures/margin_call_rules/order-controls` with `pyramiding=0`
declared. TradingView's default is already 0, so its tape is byte-identical
to that control's. That control declares no pyramiding, which the engine
reads as 1, and the current-close path needs 0. The four `cooldown-*` controls
there stay equal to TradingView.

## The rule and its switches

`PoocCloseFillViewSwitches` (`include/pineforge/source/pine_adapter.hpp`) has
one switch per part. Both are on, and only tests change them. Both act only at
the re-issued exit that `exit()` fills at its call:

- `current_exit_keeps_position_view`: the adapter freezes the script's
  position view before it executes the exit, as an ordinary `close_all` does.
- `current_exit_keeps_average_price`: once the fill has left the book flat,
  generated `strategy.position_avg_price` reads (`position_entry_price_`
  behind a non-zero `signed_position_size()`) return the pre-fill average
  until the pass ends. Then `release_script_average_price()` restores the
  flat book's 0, before the orders the pass placed settle.

`test_pooc_close_fill_view_tapes` requires every tape to equal TradingView
row for row, with the entry ids too, except the two known divergences in
`known_divergences.json`. For those it requires exactly the recorded first
departure. It clears each part in turn and requires exactly the tapes in
`switch_off_departures.json` to move, each away from TradingView. Clearing the
average part moves `short-reissued-exit-average` and
`short-reissued-limit-exit`. Clearing the view part moves the five re-issued
short tapes. It also replays every tape as a forward stream with both parts on
and with both off, and requires the stream's trades to equal the backtest's.
Last, it records every tape's per-bar broker-state hash with the average part
on and off. `position_entry_price_` is hashed, so the hashes must be equal
wherever the trades are, and otherwise first differ after the exit bar's fold:
a held average that outlived its pass fails the test.

## Scope

Both parts act at the re-issued exit's current-close path for its stop and its
limit legs (`short-reissued-limit-exit` pins the limit). The other close fills
keep their course:

- An ordinary `close_all` keeps its size freeze (KI-64), and its average read
  is unchanged. TradingView keeps the pre-fill average there too
  (`short-close-all-average`, a known divergence).
- A `strategy.close` under an active intraday cap also fills at the call
  without a freeze. No tape covers it.
- After the re-issued exit, script reads of `strategy.opentrades`,
  `strategy.closedtrades`, `strategy.openprofit`, `strategy.netprofit` and
  `strategy.equity` read the book after the fill. TradingView keeps their
  pre-fill values (`short-reissued-exit-reads`, a known divergence).
  Generated code reads the first two from host members (`pyramid_entries_`,
  `trades_`).
