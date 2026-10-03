# carried trail open

An armed short trailing stop must test the projected opening print before the favourable low advances its best. TradingView exits at 262.35, not 262.17.

Campaign-authored independent synthetic on NASDAQ:AAPL 15m. `strategy.pine`, `tv_trades.csv`, `metrics.json` and `meta.json` preserve the TradingView export. CSV timestamps are UTC+8; the replay subtracts eight hours. No population source was sent to TradingView.

- Source SHA-256: `881403f7a66b53247ab7c2fc4c018931ad45310d425fc1521fe783ac438c47a6`.
- TradingView tape SHA-256: `966808484e0aa4fafb17a549a7cbad68de0c12fe35fb3b4917e8a032c3254f02`.
- Chart feed SHA-256: `ae2b03d3736f057cf4072185d637b7c2edd9350d7a1d83ddf0178cbf292279a0`.
- `bars.csv`: 4 exact chart-feed rows from 2026-03-04 20:00 through 2026-03-04 21:15 UTC, sufficient for this timestamp-driven synthetic.

The corresponding `test_carried_trail_open_tapes` compares trade count, direction, entry/exit timestamps, tick prices, quantity and PnL.
