# A calc_on_order_fills refill under the bar magnifier (R5 lane TAIL-C)

TradingView's own trades for a priced entry R that the recalculation of an exit fill places,
with `use_bar_magnifier = true`: the magnified twin of lane W8D-NOTRADES's chart-path tapes
(`../coof_refill_waypoint`). P is a one-unit short on every even UTC hour's first bar; X is
its buy stop at the average + 3, P is closed after 8 bars. R is placed by the recalculation
of X's fill, and re-placed by that bar's close while still flat: in cell A (X on an hour
% 4 < 2) a SELL limit at X's fill - 3, executable at X's fill and at the end of the rising
leg X filled on; in cell B a BUY limit at X's fill + 0.20, executable at X's fill but
usually not at that leg's end. R is closed after two bars and cancelled on the next bar when
it never filled. The script is stateless (every decision reads `strategy.*` only), so the
recalculation's rollback cannot lose a decision.

What the tape shows: R is live from the next fill point of TradingView's magnified path --
the end of the leg X filled on inside its intrabar, or the next intrabar's open after that
intrabar's second extreme. Executable there, it fills at that point's print (every cell A);
a limit that is not rests at its own level from that point on (cell B). After a fill on the
chart bar's last path point, its close print, the next fill point is the next bar's open,
and R is live from there (2025-04-05 02:30 UTC: X at the 1819.51 close print, R filled at
the next bar's 1819.5 open).

`tests/test_magnified_coof_refill_tape.cpp` replays 2025-04-05 through the Pine adapter on the
eth-scraped-15 lane's 1-minute feed with the magnifier on (`bars_1m.inc`, 1440 bars) and
requires every trade the tape opens and closes inside that day -- 15: 9 P, 4 RA filled at
the leg-end print, the RB resting at its level and the RB filled at the next bar's open --
to be the engine's, on its chart bars: entry and exit bar, side, price in ticks of 0.01,
quantity in lots of 0.0001. On 8988faff the row fails: the four RA fill at their own level
on X's leg. Over the whole tape the engine books 534 of 534 trades.

| tape | trades | tv_trades.csv sha256 |
|---|---:|---|
| `tailc-mag-coof-refill-leg` | 534 | `ec0b4db942cc4fb6d2d285727716d1c6a00923778881023c187949edc4181253` |

The directory is one `lab tv --no-note` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof` covered, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01) of a synthetic script
written for this lane, byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`. The script's header comment names an adapter line of the
lane's base; it is the exported text, left as exported.
