# coof close slippage

An immediate buy-close applies its own one-tick slippage to the raw opening print, not to the already-slipped short fill. Entry 5912.00, exit 5912.50, PnL -25 at point value 50.

Campaign-authored independent synthetic on CME_MINI:ES1! 1D. `strategy.pine`, `tv_trades.csv`, `metrics.json` and `meta.json` preserve the TradingView export. CSV timestamps are UTC+8; the replay subtracts eight hours. No population source was sent to TradingView.

- Source SHA-256: `fb180815adb53d74540c9533cf589a7ff79c9c34b13d17a878eb01fc6a0d0183`.
- TradingView tape SHA-256: `a4cd58b9e11c1c02fba5c5a000426d9fdd169fa05fc7b36e3a6437a7c622b7ca`.
- Chart feed SHA-256: `8da771d4bb794d41963d79298e99c0caf5d0bfba6eec7dd1892e26e50c8bbd68`.
- `bars.csv`: 5 exact chart-feed rows from 2025-05-27 00:00 through 2025-06-03 00:00 UTC, sufficient for this timestamp-driven synthetic.

The corresponding `test_coof_close_slippage_tapes` compares trade count, direction, entry/exit timestamps, tick prices, quantity and PnL.
