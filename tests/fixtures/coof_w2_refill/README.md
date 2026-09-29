# calc_on_order_fills refills after a fill forced onto a leg's end (R5 lane TAIL-C)

TradingView's own trades for a priced entry R that the recalculation of an exit X places,
where X is a take profit the path reaches only at the END of the leg its entry P filled on,
so the adapter fills X at that extreme's print (a fill forced onto the point). P is a stop
entry 40 beyond the close of every even UTC hour's first bar (cancelled on the :45 bar when
unfilled); X is P's take profit 30 beyond its average; P is closed after six bars. R is
closed after two bars and cancelled on the next bar when it never filled. The script is
stateless (every decision reads strategy.* only), so the recalculation's rollback cannot
lose a decision. `tests/test_coof_w2_refill_tapes.cpp` replays each tape through the Pine
adapter over the btcusdt-15 lane's first four days (`../coof_refill_waypoint/bars.inc`).

What the tapes show, classified by where X filled on the chart bar's path
(O -> first extreme W1 -> second extreme W2 -> C) over the whole month:

- **X forced onto W2:** R is never filled on X's bar, whether or not it is executable at
  X's fill and even when the W2 -> C leg crosses its level (-e, -f). It is live from the
  next bar's open: filled at that open when it is executable there, otherwise at its own
  level later, or never. 1013 refills, none on X's bar.
- **X forced onto W1:** R executable at W2 fills there, at its print (-c, -d; also -e, -f
  whose R is reached at W2). A stop R that is not executable at W2 rests at its own level
  from W2 on: filled later in the bar, on a later bar or never (-a, -b).
- **X filled at its own level inside a leg:** the lane W8D-NOTRADES rules
  (`../coof_refill_waypoint`).

| tape | P | R | X on W2: R next open / next bar at level / never | X on W1: R same bar / later / never | trades | tv_trades.csv sha256 |
|---|---|---|---|---|---:|---|
| `tailc-w2r-a-buystop` | long | BUY STOP at P's entry price (through at X's fill) | 133 / 25 / 16 | 12 at level / 14 / 25 | 514 | `ce12c5524795b50f61a02e66ecccac0b04d44a51f9211a4be898eb0d65826ce8` |
| `tailc-w2r-b-sellstop` | short | SELL STOP at P's entry price | 128 / 34 / 12 | 11 at level / 12 / 24 | 516 | `1f02ee1918658785f4ad6a97b124586fa5ade4e76f4a6dc6fe985b9752e42cb1` |
| `tailc-w2r-c-buylimit` | long | BUY LIMIT 10 above X's fill (executable there) | 175 / 0 / 0 | 51 at W2 / 0 / 0 | 548 | `ead492d158bafc2d053015d6c463222644391de0009d5cb6f82dff8bb1927eca` |
| `tailc-w2r-d-selllimit` | short | SELL LIMIT 10 below X's fill | 174 / 0 / 0 | 47 at W2 / 0 / 0 | 538 | `6f7a0718c9e21c2142f78847e09da23eda57a7d5c15e63d408ac3416d4669a61` |
| `tailc-w2r-e-sellstop-resting` | long | SELL STOP 5 under X's fill (not executable there) | 159 / 14 / 2 | 51 at W2 / 0 / 0 | 545 | `b20153e058ab89dae5d5fcdd812ed180ecdfb6a9631e13cac854a56fb05cc937` |
| `tailc-w2r-f-buystop-resting` | short | BUY STOP 5 over X's fill | 154 / 17 / 1 | 47 at W2 / 0 / 0 | 532 | `bfbec52da95af5d0dd2a987eac1565483c285363399275a6ba2bc369787cfdf6` |

Each directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, BINANCE:BTCUSDT 15, 2025-04-01 .. 2025-05-01), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.
