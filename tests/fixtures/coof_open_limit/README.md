# An exit limit the open fill's recalculation places at or through the open print (R5 lane TAIL-D)

Under `calc_on_order_fills` the recalculation of an entry's fill at the bar's open places the exits a
script issues once a position exists. TradingView fills such an exit limit at once when it sits at
or through the open print, at the print and without slippage, on the entry bar; one short of the
print rests at its level for the rest of the bar. The adapter judged both against the slipped entry
fill and held them for the next bar (finnp17-cleantradequantum on NSE:NIFTY 1D).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, NSE:NIFTY 1D, 2025-05-01 .. 2025-08-01), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved. The returned range starts on the first
daily bar after 2025-05-01 (`narrower-than-requested`); every cell lies inside it.
`tests/test_coof_open_limit_tapes.cpp` replays the tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-fp17-coof-open` | one cell a week, slippage 2: LA/SB/LE exit limits 20 ticks through the open, LG 1 tick short of it, LF 10 ticks (control), LC/SD strategy.close (recorded, not fixed) | 7 | `48af95073a954d456b8ebae71783412cee0ff76c399c4e6eb4ca69a33619000d` | narrower-than-requested |

## Bars

`bars.inc`: 2025-05-02 .. 2025-07-31 of the nifty-1d lane chart feed (sha256
`b57ebb7b4e4b80743420136c06ac00b9b58c61e37e0c01ade3368ef15665aed0`).
