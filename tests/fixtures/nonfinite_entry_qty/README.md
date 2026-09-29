# A non-finite strategy.entry quantity (R5 lane TAIL-C)

TradingView's own trades for `strategy.entry` quantities that are not finite numbers. One leg
per UTC hour (`hour % 9`), entered at the hh:00 bar's close under `process_orders_on_close` and
closed by `strategy.close_all()` at the hh:15 bar's close; the default quantity is 0.01 fixed,
far from what the 1 000 000 of capital buys, so a row's size tells the default rule apart from
any equity rule. `z = close - close` is a series zero, so nothing is folded at compile time.

| leg | `qty =` | TradingView |
|---|---|---|
| C | `0.02` (control) | 0.02 |
| FINF | `100 / z`, a float +Infinity | 0.01, the default |
| IINF | `math.floor(100 / z)` in an `int` | 0.01 |
| GINF | the same, entered only `if iInf > 0` | 0.01: the infinity orders above 0 |
| FNA | `na` | 0.01 |
| INA | an `int` declared `na` | 0.01 |
| INAF | `math.floor(z / z)` in an `int` | 0.01 |
| NINF | `-100 / z`, a float -Infinity | 0.01 |

(A 1e12 leg is off: TradingView refuses that quantity with RE10024.)

So every infinite quantity trades the default quantity, as na does. The adapter dropped an
infinite one. `tests/test_nonfinite_entry_qty_tape.cpp` ports the legs whose quantity reaches
the adapter as a double -- C, FINF, FNA, NINF; the int legs are the codegen's, which keeps
`math.floor(100 / z)`'s infinity for `>`/`<` and hands the order its na (pineforge-codegen,
lane TAIL-C) -- replays the tape through the Pine adapter over the btcusdt-15 lane's first four
days (`../coof_refill_waypoint/bars.inc`) and requires each of those legs' trades the tape
closes inside the bars (44) to be the engine's: entry and exit time, side, price in ticks of
0.01, quantity in lots of 0.00001. On 8988faff the row fails: FINF and NINF enter nothing.

| tape | trades | tv_trades.csv sha256 |
|---|---:|---|
| `tailc-a-qty-nonfinite2` | 155 | `9d02d2a0fc994b4049641ee7f1c99c550bb2c32a4030faf9531b0b77d89c3afa` |

The directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel `ws-report-v1`,
`rangeProof` covered, BINANCE:BTCUSDT 15, 2025-04-01 .. 2025-04-08) of a synthetic script
written for this lane, byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`.
