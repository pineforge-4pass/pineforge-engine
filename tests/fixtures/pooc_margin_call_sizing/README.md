# A default-quantity entry sizes on the equity its bar's margin call leaves (lane W4-ENG-POOC-SAMEPASS)

TradingView's own trades for a short held at 100 % of equity (the v6 default
quantity) into a bar whose new high margin-calls it above that bar's close,
where the script enters the long, written for this lane. Each directory is one
`lab tv --no-note` export (channel `ws-report-v1`, `rangeProof` covered), byte
for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, with three cells
on 2025-04-03, each flattened two hours after its flip.
`tests/test_pooc_margin_call_sizing_tapes.cpp` replays every tape.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `w4-mcs-pooc` | process_orders_on_close | 12 | `2a82552b0e8afbb00979f684d095d1ae5918f68f31b04cbcc31e245994ba8896` |
| `w4-mcs-pooc-coof` | process_orders_on_close + calc_on_order_fills | 12 | the same file |
| `w4-mcs-plain` | -- | 11 | `7a369bf87509308d3f9d8c6a4cae9b9c976c39ad3df20260176b2208d7301e50` |
| `w4-mcs-plain-coof` | calc_on_order_fills | 11 | the same file |

What they show:

- Cells A and B: `strategy.close` of the short and the long entry on the flip
  bar; C: the long entry alone reverses the short there. Every short is
  margin-called in slices on the bars before the flip.
- Under `process_orders_on_close` cell C's flip bar is margin-called at its
  high (1798.57) before the reversal: TradingView closes the rest of the short
  at the close (1787.58) and opens the long sized on the equity the call leaves
  (53.8573), not on the equity before it.
- Without it the entries fill at the next open; cell C's reversal never fills,
  on TradingView as in the engine.

## Bars

`bars.inc` holds the replayed bars, 2025-04-03 00:00 .. 23:45 UTC, copied as text
from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`);
`exec/W4-ENG-POOC-SAMEPASS-scratch/tools/gen_bars_inc.py` wrote it.
