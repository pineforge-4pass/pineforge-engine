# The exit a fill's recalculation places acts on the entry bar (lane W4-ENG-POOC-SAMEPASS, F19)

TradingView's own trades for an exit a script places only once the position
exists (`strategy.position_size` non-zero), priced off
`strategy.position_avg_price`, written for this lane. Each directory is one
`lab tv --no-note` export (channel `ws-report-v1`, `rangeProof` covered), byte for
byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, fixed 1, with three
cells on 2025-04-11: A a stop 1 point on the wrong side of the average price
(through at the fill), B a limit 10 / stop 5 and C a limit 5 / stop 10 around it,
on bars whose remaining path reaches one leg. `tests/test_coof_fill_bar_exit_tapes.cpp`
replays every tape.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `w4-f19c-long-plain-coof` | calc_on_order_fills | 3 | `c85b908c31890023274ff7fd5bb1bdd809f0d83c9cfebfd732955f1bc292a92b` |
| `w4-f19c-long-plain` | -- | 3 | `07faacb8ff8fa5beb8b066c3e5d995e90e180a09f7e003f948d35b2ea99649b7` |
| `w4-f19c-long-pooc-coof` | process_orders_on_close + calc_on_order_fills | 3 | `5909cb9bf6356e1b509c21c56b9884b9a91722a31d6ca8cd9db43f1c54acbd51` |
| `w4-f19c-long-pooc` | process_orders_on_close | 3 | the same file |
| `w4-f19c-short-plain-coof` | calc_on_order_fills | 3 | `af61e8d5b545e98098f3f2afbec5ede47ea77ec5c821a71dde5685b1946f8003` |
| `w4-f19c-short-plain` | -- | 3 | `d8eb7feb2f134bc03075442d2577f51f1d59127fc63516c380ec836ab527488a` |
| `w4-f19c-short-pooc-coof` | process_orders_on_close + calc_on_order_fills | 3 | `8e4cea5a3384c64c6f3fcb1251487cce2e862e1a06a3ea7df35ee9c0cc8e42bb` |
| `w4-f19c-short-pooc` | process_orders_on_close | 3 | the same file |

What they show: under calc_on_order_fills the recalculation of the entry's open
fill places the exit, which acts on the rest of the entry bar -- the through stop
at the fill itself, a zero-length trade (LA, SA), the limit or stop the bar's path
reaches at its level (LB, LC, SB, SC). Without calc_on_order_fills the exit is
placed by the bar's close calculation and acts from the next bar. Under
process_orders_on_close the entry fills at the close, whose fill recalculates
nothing, with or without calc_on_order_fills.

## Bars

`bars.inc` holds the replayed bars, 2025-04-11 00:00 .. 05:00 UTC, copied as text
from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`);
`exec/W4-ENG-POOC-SAMEPASS-scratch/tools/gen_bars_inc.py` wrote it.
