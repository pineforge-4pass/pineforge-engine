# TV-Parity Probe Spec (handoff)

> These probes need a **TradingView export** (`tv_trades.csv`) as ground truth,
> which cannot be generated from this repo. Each entry is a clean-room
> `strategy.pine` + the exact capture recipe + expected outcome, ready for
> someone with a TV account to run and drop into `corpus/validation/` (or
> `corpus/symbol-specified/` for non-default feeds). The capture mechanics mirror
> `corpus/special-validation/README.md` (SPX example) and
> `corpus/validation/symbol-specified/` (AAPL/QQQ/SPY scaffolds).
>
> Scope: all within PineForge's compute mandate. Charts / import / cross-symbol
> MTF remain out of scope.

## Capture recipe (common to all)

1. Open the named symbol + timeframe on TradingView; set the chart timezone
   (bottom-right clock) to the `tv_trades_csv_tz` below (default UTC+8 to match
   the existing corpus, or pin per probe).
2. Apply `strategy.pine` → Strategy Tester → List of Trades → Export → CSV →
   `tv_trades.csv` in the probe dir.
3. Export the chart OHLCV (menu → Export chart data) → build the engine feed
   (see `corpus/special-validation/make_feed.py`). For licensed data (equities,
   indices) the OHLCV stays git-ignored; only `strategy.pine` + `generated.cpp`
   + `inputs.json` + `tv_trades.csv`(if redistributable) are committed.
4. `inputs.json` keys: `ohlcv_csv`, `engine_chart_timezone`, `tv_trades_csv_tz`,
   and `runtime_overrides.{timezone,session}` as needed.
5. Verify with `scripts/verify_corpus.py <dir>`; target `excellent`.

## Family A — Instrument / asset-class realism (the loudest gap)

Every headline probe is Binance ETH/USDT 15m at $1.5k–5k, 0.01 tick, 1×, 24×7,
zero gaps. None of the following market-structure properties are exercised:

| Probe | Symbol / TF | Proves |
|---|---|---|
| `symbol-equity-rth-gaps-aapl-01` | `NASDAQ:AAPL` 5m, RTH `0930-1600` America/New_York | overnight gap fills, RTH session filter, holiday calendar, exchange-tz vs chart-tz day boundary |
| `symbol-index-rth-spx-01` | `SP:SPX` 5m (extends `special-validation/`) | RTH session + chart-tz intraday-cap rollover on a real session (already scaffolded) |
| `symbol-futures-pointvalue-es-01` | `CME_MINI:ES1!` 5m, tick 0.25, point value $50 | **point-value PnL multiplier** (currently `syminfo.pointvalue` is not applied to PnL) + 0.25-tick directional snap economics |
| `symbol-fx-5dp-eurusd-01` | `OANDA:EURUSD` 15m, mintick 0.00001 | 5-decimal pricing, sub-pip slippage rounding, FX session |
| `symbol-pricemag-penny-01` / `-sixfig-01` | a sub-$1 stock / `BINANCE:BTCUSDT` | price-magnitude extremes (qty precision at $0.x and $100k) |
| `commission-per-contract-es-01` | ES, `commission_type=cash_per_contract` | the commission type never exercised by the crypto-% corpus |
| `slippage-nonzero-01` | any, `slippage=3` | slippage>0 economics end-to-end (no corpus probe sets slippage>0) |

Each `strategy.pine` is a plain dual-MA or RSI cross (the *instrument* is the
surface under test, not the logic) so a divergence is unambiguously a
market-structure bug, not a TA bug.

Sketch (representative — futures point value, ES):
```pine
//@version=6
strategy("ES point-value parity", overlay=true, initial_capital=100000,
     default_qty_type=strategy.fixed, default_qty_value=1, commission_type=strategy.commission.cash_per_contract, commission_value=2.10, slippage=1)
fast = ta.sma(close, 9), slow = ta.sma(close, 21)
if ta.crossover(fast, slow)
    strategy.entry("L", strategy.long)
if ta.crossunder(fast, slow)
    strategy.close("L")
```
Expected: PnL per point == `(exit-entry) * $50 * qty − commission`. **Will FAIL
until point-value wiring lands** — capture TV first, then implement to match.

## Family B — Leverage / margin-call / forced liquidation

`margin_liquidation_price()` is hardwired `na` and margin is only checked at
signal time, so a leveraged position that should be liquidated runs to its own
exit. A skeptic calls this "your equity curve is fiction for a leveraged
account."

- `leverage-margin-call-perp-5x-01` — `BINANCE:ETHUSDT.P` 15m, a 5× position
  entered then held through an adverse move that crosses the maintenance margin.
  Capture TV's **Margin Call** rows. Expected: engine emits a liquidation exit at
  TV's liquidation price/time. Requires implementing the in-position liquidation
  latch to match; **engine-only invariants** (a liquidation can never realize a
  loss greater than posted collateral; position is flat after liquidation) can be
  guarded now, but the *price* needs the TV oracle.

## Family C — TA volume/state family TV parity

12 built-ins appear in **zero** corpus probes and are guarded only by
author-computed references that share the formula with the impl:
`ta.tsi`, `ta.cog`, `ta.obv`, `ta.accdist`, `ta.nvi`, `ta.pvi`, `ta.pvt`,
`ta.mfi`, `ta.cmo`, `ta.rci`, `ta.wpr`, `pivot_point_levels`. Confirmed
formula suspects to scrutinize against TV: **TSI ×100 scaling**, **III
multiply-vs-divide**, **RCI d²-shortcut vs Pearson-on-ranks on ties**.

One probe per indicator (`ta-tsi-25-13-cross-01`, `ta-rci-extreme-revert-01`, …),
same shape as the existing `ta-*` probes. The engine-only analytic invariants
(bounds, forward-sum identities) already ship in
`tests/test_ta_volume_state_oracle.cpp`; these add the missing TV oracle.

## Family D — Weekly / Monthly HTF parity

`request.security(syminfo.tickerid, "W"/"M", …)` — corpus HTF literals are only
60/240/D. The engine-only aggregator oracle ships in
`tests/test_calendar_aggregation_wm.cpp` (month-length, leap, year rollover);
this promotes it to TV-backed:
- `mtf-htf-weekly-sma-01` (`"W"`), `mtf-htf-monthly-ema-01` (`"M"`) over a span
  covering a leap February and a Dec→Jan rollover.

## Family E — Per-trade field parity (qty / pnl_pct / MFE / MAE)

Once the validator field extension lands (report-only first), capture probes that
specifically stress the under-checked fields:
- fractional/`percent_of_equity` qty (qty parity), a high-MFE-then-reversal trade
  (MFE/MAE parity), and a commissioned trade (pnl_pct parity — pins the gross-vs-net convention).

## Suggested order

1. Family A futures + FX (forces point-value + tick economics).
2. Family E commissioned probe (pins the pnl_pct convention) — pairs with the validator
   field extension.
3. Family B leverage/liquidation (largest engine work).
4. Families C + D (breadth; lower per-probe risk).
