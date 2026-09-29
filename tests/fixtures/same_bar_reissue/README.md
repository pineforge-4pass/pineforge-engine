# A same-bar reissue of a reversing MARKET entry (R1, lane R1-CONSOLIDATE)

A `strategy.entry` called again with the same id on the same bar replaces the
order its first call placed. When that first order is a MARKET order that
reverses the position, TradingView sizes the reissue against the position the
first order would leave: the reissue is a plain transaction of its own
quantity X against the held side P. It closes up to X of P and opens only the
excess X - P, if any; it does not add the held side the way a reversal does.
This holds on either side, for every quantity kind (a default percent of
equity, a fixed or a cash default, and an explicit `qty`), and with or
without `process_orders_on_close`. On the buy side the held short keeps its
bracket; on the sell side a default-size reissue voids the held long's
bracket.

Two lanes wrote this rule independently, each with its own tapes: W8A-SIGSTATE-1
(the six `w8a-*` tapes: explicit `qty`, cash and fixed defaults, the controls)
and W8B-SIGSTATE-2 (the nine `w8b-*` tapes: the flip on both sides, brackets,
fixed defaults, `process_orders_on_close`). Lane R1-CONSOLIDATE merged the two
into one rule that passes all fifteen.

The engine kept the rule only for a default percent_of_equity SELL whose first
call had reached the native core (ab9714be's R18 pins,
`tests/test_replaced_percent_short_market.cpp`); it reversed a buy, a fixed or
cash default, an explicit qty, and every reissue whose first call waited in the
callback's same-bar batch (every fixed default, and a variable size over a
single-lot short). The scraped `you970716-snrz-0-7-1` calls `strategy.entry`
twice per signal (its `execute_trade()` body is written twice), so every
reversal it made under `percent_of_equity 10` opened a whole new position where
TradingView opened a sliver: BINANCE:BTCUSDT 15, 2025-10-19 08:30 UTC, TV long
0.00002 against the engine's 0.0093, and the equity cascade after it.

What keeps TradingView's full reversal (the controls):

- a single call (every `-single` tape, `w8b-flip-single`, and cells C and D of
  the fixed and cash tapes);
- a LIMIT first call re-issued as a MARKET order on the same bar (cell K);
- a `strategy.cancel` of the id between the two calls (cell N).

Each directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json`, `meta.json`. `metrics.json`
`tvTradesCsvHash` is the sha256 of `tv_trades.csv`. Every probe runs on
BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01.

- The `w8a-*` probes have fixed `timestamp("UTC", ...)` cells on 2025-04-08 ..
  04-09 and a `strategy.cancel_all()` plus `strategy.close_all()` cleanup two
  hours after each cell. None declares `initial_capital`, so each runs on v6's
  100000.
- The `w8b-*` probes declare `initial_capital = 10000` and have two cells:
  cell 1 seeds a position on 2025-04-08 (flip at 00:30 UTC, after the price
  rose, so X < P for a long flip), cell 2 on 2025-04-09 (flip at 00:15, after
  it fell, so X > P); each is flattened by `strategy.cancel_all()` plus
  `strategy.close_all()` at 02:00.

| tape | declares | cells / TradingView | trades | tv_trades.csv sha256 |
|---|---|---|---:|---|
| `w8a-dbl-p10` | percent_of_equity 10 | A sell reissue with its exit between, B buy reissue, C sell `qty=1` reissue, E limit re-called each bar, F flat buy reissue | 9 | `253f4382d81f095e2b47816d6c4acab97a34b914d2264cbb6e716b3101f3a2ca` |
| `w8a-dbl-p10-single` | percent_of_equity 10 | the same cells, every call once | 9 | `5c4e30f4ea50856b6947c81e2c9a627e264335e5570c3b47727a78ed78ec2d7c` |
| `w8a-dbl-scope3` | percent_of_equity 10 | G sell `qty=10` reissue over a smaller long, H buy `qty=1` reissue, J sell reissue then a same-side sibling, K limit then market, N entry, cancel, entry | 10 | `c5eed99dcf4f2cd4f3b7df8923899673299152b89e87d7a49a08df7e5496ee14` |
| `w8a-dbl-scope3-single` | percent_of_equity 10 | the same cells, every entry once | 10 | `2dce20e2555a8c8959409a5c61a2d6ca170c78a48439eb03087bfbbe60436401` |
| `w8a-dbl-fixed` | fixed 2 | A sell and B buy reissue over a 3-unit seed; C and D their single-call controls | 8 | `25535241dee653fee663459670345a2725653dd8a25921f192be666cb8747746` |
| `w8a-dbl-cash` | cash 5000 | the same four cells | 8 | `d36a76e00ec2a5fd2caca1f2ebf53f4ad1d61c2fe05c07ba745356050b4914c2` |
| `w8b-flip-single` | percent_of_equity 10 | control, one call per flip: each short closes whole and the long opens its own size (0.6426, 0.6814) | 4 | `185a57c05bd2169eda6013b981d0c326cadaac14a0468f941038c7bf4bb50570` |
| `w8b-flip-double` | percent_of_equity 10 | two calls: cell 1 closes 0.6426 of the 0.6436 short and 0.001 rides to the cleanup, no long; cell 2 closes the 0.6761 short and opens 0.0029 long | 4 | `a0f436a5730a9b4e9dcaa04102b57daf0cf62d4b5f85523ac415f966fdf0b3f9` |
| `w8b-flip-double-bracket` | percent_of_equity 10 | two calls with the new side's `strategy.exit` between and after them: the same rows (one file with `w8b-flip-double`) | 4 | `a0f436a5730a9b4e9dcaa04102b57daf0cf62d4b5f85523ac415f966fdf0b3f9` |
| `w8b-flip-double-seedbracket` | percent_of_equity 10 | the seed short carries its own bracket: the 0.001 short remainder exits by it (stop 1580 at 01:30), the held short's bracket survives the reissue | 4 | `d8c3c63e018eb9c1d788ebed9b03a6b6242b762c56af68706d5afe26e5dfe467` |
| `w8b-flip-double-sell` | percent_of_equity 10 | the sell side, the seed long bracketed: cell 1 closes 0.6428 of 0.6436 and the 0.0008 long remainder rides to the cleanup although its limit 1580 is reached at 01:30 (the held long's bracket is void); cell 2 opens 0.0024 short | 4 | `f9296f4a461e604c378f262032c0332339dd3a01f9e1e1aa4d628d9d49ab96bd` |
| `w8b-flip-double-fixed` | fixed 1 | seeds of qty 2 and 0.5: cell 1 closes 1 of the 2 short and 1 rides to the cleanup; cell 2 closes 0.5 and opens 0.5 long | 4 | `731417c29e2d80dc8237fabf66f2cbb5b0c99f27243d569602940fc7b5d54e64` |
| `w8b-flip-double-sell-fixed` | fixed 1 | the sell side of the fixed tape, seed longs bracketed: 1 of 2 closes, the remainder rides to the cleanup past its limit; 0.5 short opens | 4 | `a532a1ac820e999732687564bf19bc77025afcc4108f45ab136983eb3c13946c` |
| `w8b-flip-double-pooc` | percent_of_equity 10, process_orders_on_close | the `w8b-flip-double` cells filled at each bar's close: 0.001 short remainder; 0.0029 long | 4 | `7ccbe9aedb41dfa6a8329b4d7ed3dc55da1d17cf09a74e84254ec685244e3c2d` |
| `w8b-flip-double-sell-pooc` | percent_of_equity 10, process_orders_on_close | the `w8b-flip-double-sell` cells at the close: 0.0008 long remainder; 0.0024 short | 4 | `4f1f815215de59d3583eeb86ec732b45d85c65adae9f3e2385030e73a05341ed` |

`tests/test_same_bar_reissue_tapes.cpp` replays every script through the Pine
adapter under the configuration its generated constructor declares, over the
corpus 15m bars of `tests/fixtures/tvdef_drops/bars.inc` (the same chart and
window, 2025-04-07 00:00 .. 2025-04-10 12:00 UTC), trading from 2025-04-08
00:00 UTC with TradingView's 0.0001 lot as the `qty_step`, and requires each
trade the tape closes inside those bars -- entry and exit time, side, price in
ticks of 0.01, quantity in lots of 0.0001 -- to be the engine's. It also reads
the rule off TradingView's own rows. On the base 8633d944 ten of the fifteen
tapes fail (the four single-call controls and `w8b-flip-double-sell` pass).

Two neighbouring TradingView rules are left out of these tapes because the
engine does not model them yet (lane W8A-SIGSTATE-1's report has the tapes):

- a default percent_of_equity LIMIT entry is sized when it is placed or
  re-placed -- the equity at that bar's close, open profit included, over the
  limit price -- not at its fill (an earlier scope tape's cell M);
- a later same-side default MARKET entry of another id after a buy reissue
  (an earlier scope tape's cell I): the engine cancels the reissue in its
  favour, TradingView keeps the reissue and never fills the sibling.

The round-8 short-seed book (`tests/oracle/test_oracle_short_seed.cpp`, a
short seed, then `Long`, `Short`, `close(Long)`, `close(Short)` on one bar)
keeps its full-reversal model when `Long` is re-issued: its model yields
TradingView's materialized `Close entry(s) order Short` lot only from the full
reversal, and TradingView's own reissued book (lane W8A-SIGSTATE-1 scratch
tape `w8a-famS-reissue`, BINANCE:ETHUSDT.P 15, April 2025) books the reissue as
a plain transaction and still materializes that lot, which neither form of the
engine's model reproduces. The same-bar batch therefore applies the rule
outside that book only.
