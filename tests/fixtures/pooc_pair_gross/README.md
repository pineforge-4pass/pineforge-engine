# A flat opposite pair over the equity keeps its earlier call, under POOC and with costs (R5 lane TAIL-D)

A flat script that places two opposite fixed-quantity market entries on one bar keeps only the
earlier call on TradingView when the pair's gross is over the equity at full margin; a pair that fits
keeps its two-leg shape. The adapter's gross decline held only under `calc_on_order_fills` without
slippage or commission; under `process_orders_on_close` alone, or with costs, the later call became a
full reversal (officialjackofalltrades-helios on OANDA:EURUSD 1D).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, OANDA:EURUSD 1D, 2025-04-01 .. 2026-05-01), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved. `process_orders_on_close` on,
pyramiding 0, 100000 of capital. `tests/test_pooc_pair_gross_tapes.cpp` replays every tape.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `hel-h1-pooc-pair-gross-slip` | a 12-bar cycle of O pairs (80 % legs, a 160 % gross) and U pairs (30 % legs), the long or the short call first; slippage 1, a 0.01 % commission | 140 | `a701588127a118ccbcc1429b4676bfc1de2731eef9353fba332f4431fb5b25c7` | covered |
| `hel-h2-pooc-pair-gross-zero` | the same without costs | 140 | `f6227623f643d1dfc9ac5b5d99a14a33d8be9adf4ec480856932fd440ae944ae` | covered |

## Bars

`bars.inc`: 2025-04-01 .. 2026-04-30 of the eurusd-1d lane chart feed (sha256
`e95d45d2d141baf44e0a5b2de384057bb40573e2ad4dce9f5eaf5c232c866ddf`).
