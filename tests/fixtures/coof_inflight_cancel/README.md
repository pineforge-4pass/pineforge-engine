# What a mid-leg recalculation cancels or closes applies at the leg's end (R5 lane TAIL-D)

Under `calc_on_order_fills` a fill inside a leg of the bar's path starts a recalculation there, and
TradingView applies what that recalculation cancels or closes -- `strategy.cancel`,
`strategy.cancel_all`, a full `strategy.close` -- at the END of that leg: an exit resting from before
whose level the rest of the leg reaches still fills there, at its level; one the leg does not reach
is gone after it (job-2898-repost64-orb-meeeeeks-quote-ccy on BINANCE:BTCUSDT 15).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-05), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved.
`tests/test_coof_inflight_cancel_tapes.cpp` replays every tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-m7a` | a position of 2 with take-profits TP1 (3 points) and TP2 (6 points) of one unit; the recalculation of TP1's fill calls cancel_all() and close("P", immediately = true); process_orders_on_close | 46 | `400b3e8126e4a4fdbca21d77beca8afefb010ad1321ababbdc1cbf380a5f29de` | covered |
| `td-m7b` | cancel_all() alone | 46 | `3e0380aa1fd8d2fa9fdbe73dca8418b66aae03dd936e380c8eed055b28ff81c9` | covered |
| `td-m7c` | cancel("TP2") alone | 46 | `3e0380aa1fd8d2fa9fdbe73dca8418b66aae03dd936e380c8eed055b28ff81c9` | covered |
| `td-m7d` | close("P", immediately = true) alone (no cancel) | 46 | `400b3e8126e4a4fdbca21d77beca8afefb010ad1321ababbdc1cbf380a5f29de` | covered |

## Bars

`bars.inc` holds 2025-04-01 00:00 .. 2025-04-04 23:45 UTC of the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`), copied as text by `exec/TAIL-D-scratch/tools/gen_bars_inc.py`.
