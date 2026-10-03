# pooc pending stop / nifty

The same closing-print stop rule on a whole-unit quantity grid; the entry is 22552.30 at 06:30 UTC.

Campaign-authored independent synthetic on NSE:NIFTY 15m. `strategy.pine`, `tv_trades.csv`, `metrics.json` and `meta.json` preserve the TradingView export. CSV timestamps are UTC+8; the replay subtracts eight hours. No population source was sent to TradingView.

- Source SHA-256: `5eb56bec8c5a1a2dd71e04bab24e6e68e148d1c01bc8b3159f1c5db60ffb6deb`.
- TradingView tape SHA-256: `75bccb34c81a64959fcb96c87fcfa18a9d329a3d662121722b91f85244cdbda6`.
- Chart feed SHA-256: `ca2cb44a4e4c3f67e7f11b3052f4456d4a978d34cab2f10afba942cdc0e02e09`.
- `bars.csv`: 17 exact chart-feed rows from 2025-04-08 06:15 through 2025-04-09 04:00 UTC, sufficient for this timestamp-driven synthetic.

The corresponding `test_pooc_pending_stop_tapes` compares trade count, direction, entry/exit timestamps, tick prices, quantity and PnL.
