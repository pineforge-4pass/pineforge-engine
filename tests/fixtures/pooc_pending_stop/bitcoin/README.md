# pooc pending stop / bitcoin

A default-sized closing-print stop with a pending bracket fills on its creation close, not the next open.

Campaign-authored independent synthetic on BINANCE:BTCUSDT 15m. `strategy.pine`, `tv_trades.csv`, `metrics.json` and `meta.json` preserve the TradingView export. CSV timestamps are UTC+8; the replay subtracts eight hours. No population source was sent to TradingView.

- Source SHA-256: `9bea5e930e0d67b93dff52e473eeea5b2178b901633dbd77523f5bf589545fba`.
- TradingView tape SHA-256: `5140fe7c7386b3980a46e5f19952aa8b74afc7a5dfff2b630f2a8683b552ffc3`.
- Chart feed SHA-256: `6b54c44ac6de8b7b588b3fcc6ddc29b09e5fe12ce9559d811dca7859217e6270`.
- `bars.csv`: 7 exact chart-feed rows from 2025-06-03 14:15 through 2025-06-03 15:45 UTC, sufficient for this timestamp-driven synthetic.

The corresponding `test_pooc_pending_stop_tapes` compares trade count, direction, entry/exit timestamps, tick prices, quantity and PnL.
