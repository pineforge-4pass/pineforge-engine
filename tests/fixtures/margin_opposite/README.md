# A margin call at a bar's close, and orders that cross zero (lane W13-ENG-MARGIN-OPP)

TradingView checks a book at a bar's close after the script has run there,
and a margin call or a close it has placed is an order sized when it was
placed, executed as a plain market order: one whose book shrank before it
filled opens the rest on the other side. Each directory is one
`lab tv --no-note` export of a synthetic probe written for this lane (channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json`, `meta.json`; `metrics.json`
`tvTradesCsvHash` is the sha256 of `tv_trades.csv`. No closed or scraped
strategy is involved.

`tests/test_margin_opposite_tapes.cpp` replays every tape through the Pine
adapter under the configuration its `strategy()` declares, over the NYSE:F
15m bars of `tests/fixtures/slipped_short/bars.inc`, the OANDA:XAUUSD 15m
bars in `xau15_bars.inc` or the OANDA:EURUSD 15m bars of
`tests/fixtures/margin_residual/eur15_bars.inc`, with TradingView's lot (one
NYSE:F share, 0.01 XAUUSD or EURUSD) as the `qty_step`, and requires each
trade the tape closes inside the replayed bars -- entry and exit time, side,
price in ticks, quantity in lots -- to be the engine's, before the row's
window end where it names one and outside its open cells (below, "Open").
On the commit before each rule every rule row fails there and every control
row passes.

The `w13-b*` and `w13-a3` probes run on NYSE:F 15m, 2025-04-01 .. 2025-07-01,
10000 of capital, margin 100 both ways: from flat every 4 bars (`bar_index %
4 == 0`) a short sized at the close (`qty = strategy.equity / close`), a stop
entry one tick under that close (`w13-a3`: a MARKET entry at the next open).
On the bar it fills the script calls `strategy.close_all()` (`-close`),
`strategy.cancel_all()` and then `strategy.close_all()` (`-cancel-close`),
`strategy.cancel_all()` alone (`-cancel`) or nothing (`-none`); anything still
open on a later bar is closed with `strategy.close_all()`. A stop that fills
on its way down to the bar's low, on a bar whose high came first, leaves the
bar's close as the only point where the short is adverse. The `w13-x*`
probes are the same on OANDA:XAUUSD 15m, 2025-04-01 .. 2025-05-30, from flat
at the top of every hour, ten ticks under the close.

## CP: a call the close owes is taken after the script

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w13-b1-close-f` | 358 | `-close`, slippage 0 | the call is booked at the close after the script: the script's `strategy.close_all()` was sized with the called shares, so at the next open it closes the rest and opens them long (28 times; 04-10 17:15 UTC: 44 called at the 9.01 close, 1149 closed and 44 opened long at 9.00) | `d5c2a963bb77b821d620914d6088036efdf08ab0e82327657aec5f849ec03dab` |
| `w13-x1-close-xau2` | 1384 | the same on XAUUSD | the same, twelve times | `f88e80e83cc4e428e22864241b5f2f4990d14597a4534703fa43b54b17e5646d` |
| `w13-p1-eur` | 511 | OANDA:EURUSD 15m, 2025-04-01 .. 2025-04-30, 10000000 of capital, 100 % of equity, `process_orders_on_close`; from flat every 3 bars a default long, `strategy.close_all()` at the next bar's close | the one-unit money call at a close is executed there ahead of the close order, which keeps the size it was placed with: one unit short opens by the close order (8 times) | `ebafd0cf9bb87e4f49cbbf1d73f023b9f391333525515649c229a27af2fecf50` |
| `w13-p2-eur` | 299 | the same without `process_orders_on_close`: the long fills at the next open, and the bar it fills on calls `strategy.close_all()` | the one-unit money call the long's close owes is taken there after the script, and the close order, placed with the whole long, closes the rest and opens one unit short at the next open (2025-04-03 23:30 UTC: one unit called at the 1.10467 close, one unit short at the 1.10468 open) | `2dd3c4ee6653911e6fa88f1f590a79499e078562db1ddf4bbc27c855f02abb8f` |
| `w13-b5-none-f` | 437 | `-none` | the control: the same calls at the same closes, where nothing else fills | `68c8b3cd722f41bd7d8b3e671dc0c98ec5e53dfed2abfc4660dc15686f94df5c` |
| `w13-x5-none-xau2` | 1478 | the same on XAUUSD | the control | `7b6ef7a8058f6fb11839b0288e2c570b174567778d850e644606cded2736ff9b` |
| `w13-a3-mkt-flip-f` | 435 | MARKET entries, `strategy.close_all()` on the fill bar | the control: a short filled at the open is adverse at the high before the close, called there before the script, and the close closes the rest (191 calls, no overshoot) | `c3e39abce75c8c1d50c5953078fdaa1a808eec1c3931a1694a96c2170c0202c8` |

Before this lane the adapter rested the call at the close's mark, so the path
filled it at the close before the script ran: the script read the called book
and the close order closed the rest.

## CQ: the script's cancel_all withdraws the close's call

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w13-b2-cancel-close-f` | 331 | `-cancel-close` | the call is withdrawn with the script's orders and placed again behind them: at the next open the close order closes the whole short and the call, sized at the close, opens its shares long under the id "Margin call" (27 times; 04-10 17:15 UTC: 1133 closed and 40 opened long at 9.00) | `2cc1dc11b45e52a083aedc7ce3a7205eb0976350b01fdb127cad4d1f98485acf` |
| `w13-b2s-cancel-close-f` | 355 | the same, slippage 1 | the same (35 times) | `c66a54fc6a879decb2d18de60a62758614767fbe352484f7ba4c0bb880176907` |
| `w13-b4-cancel-f` | 435 | `-cancel` | nothing precedes the call at the next open: it reduces the short there, sized at the close, and the open's own check then reads the smaller book (04-07 17:45 UTC: 16 at the 9.29 open, then one more at the 9.33 high) | `f248fcb44f66cc6ac7bfbb4a6658973cbb3962aa437bed18b1893db5e792f8ec` |
| `w13-x2-cancel-close-xau2` | 1358 | `-cancel-close` on XAUUSD | the same, ten one-unit longs | `cb8e94b4fb910d659b05c01458be907918fa1ad7c656ed16b72f100cf19255d3` |

A one-unit call placed again so is taken only at an open whose fill restores
the book: `w13-b2-cancel-close-f` 2025-06-10 19:45 and 2025-06-30 14:15 UTC
take none. The population probes `pf-probe-ki62-margin-deferral` on
NYSE:F and OANDA:XAUUSD are this shape (their cells call
`strategy.cancel_all()` before `strategy.close_all()`).

## Open

Measured, not modelled; each is a row's open cell or lies past its window end,
or is a probe of this lane not kept here:

- `w13-p2-eur` 2025-04-02 16:45 and 2025-04-03 06:15 UTC: TradingView takes
  no call on the long; the engine takes one unit at the next open, beside the
  close order, as on 0cfaa782.
- Stop-entry shorts with commission 0.04 % and slippage 1 (`w13-b1c-close-f`,
  `w13-b2c-cancel-close-f`): the engine takes a call on the fill bar that
  TradingView does not (2025-04-01 14:45 UTC: 16 shares at 9.99), as on
  0cfaa782.

## Bars

`xau15_bars.inc` is the OANDA:XAUUSD 15m chart feed of the lab lane
xauusd-15 (evidence sha256
`248086b8b82d6560277cdf7bb877a269f04067b8cdb8c1577a02323772c48579`), rows
2025-04-01 00:00 .. 2025-05-29 23:45 UTC copied as text.
