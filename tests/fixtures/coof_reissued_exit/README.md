# An exit a recalculation re-issues unchanged keeps resting at its level (lane W4-ENG-POOC-SAMEPASS)

TradingView's own trades for a short's exit limit re-issued on every
calculation while short, written for this lane. Each directory is one
`lab tv --no-note` export (channel `ws-report-v1`, `rangeProof` covered), byte
for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, `pyramiding=2`,
with three cells on high-first bars (the open nearer the high), each short
placed at the close before its bar and flattened two bars after it.
`tests/test_coof_reissued_exit_tapes.cpp` replays every tape.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `w4-rex-coof` | calc_on_order_fills | 6 | `b5d6b99e5de029e7d2579c3f083915bf2ca681a3304400693e3bcf72a9b6e3d4` |
| `w4-rex-plain` | -- | 6 | `ebe30e7796bae6be26a8e8a1676ae192808ef90210717a9a1c147777fce4cec1` |
| `w4-rex-pooc` | process_orders_on_close | 6 | `46f7c977a2a41f5b9db0cc75e85f187a45c682ac2d5e6c28923162cfc1fd495f` |
| `w4-rex-pooc-coof` | process_orders_on_close + calc_on_order_fills | 6 | `216258ba99f31af5a40dc93b8a289eb741f16eb5bba4a4090ad5084030d7db76` |

What they show:

- MC (2025-04-10 13:30, O 1588.03 H 1591.66 L 1555.00): a short at 100 % of
  equity, its exit limit 1575; the high margin-calls it. Under
  `calc_on_order_fills` the exit is placed by the recalculation of the open
  fill and re-issued unchanged by the margin call's; it fills at 1575 on the
  way down.
- ADD (2025-04-13 19:30, O 1620.00 H 1623.91 L 1559.40): a short of 1, its exit
  limit 1580, a sell-stop add at 1600. The add's recalculation re-issues the
  exit unchanged; it fills at 1580.
- NEW (2025-04-16 05:15, O 1589.77 H 1591.51 L 1550.00): a short of 1 and a
  sell-stop add at 1575; the exit limit 1560 is first placed by the add's
  recalculation and fills at the bar's low, 1550.
- Under `process_orders_on_close` + `calc_on_order_fills` the shorts fill at the
  closes, so every exit is first placed by a recalculation of its bar and fills
  at that bar's low (1555, 1559.40, 1550). Without `calc_on_order_fills` the
  bar's close calculation places the exits: the close pass fills MC's at the
  close 1564.21 under `process_orders_on_close`, and plain, MC's gap-fills at
  the next open; ADD's and NEW's fill at their levels on the next bar.

## Bars

`bars.inc` holds the replayed bars, 2025-04-10 12:00 .. 2025-04-16 07:00 UTC,
copied as text from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`);
`exec/W4-ENG-POOC-SAMEPASS-scratch/tools/gen_bars_inc.py` wrote it.
