# Callback Stop After A Carried Entry

Independent synthetic on BINANCE:BTCUSDT 15m, exported October 3, 2026 through
TradingView's WebSocket strategy report with `--no-note`. The prior short stops
at 10:00 UTC. A market entry placed on the preceding bar fills at the same opening
print, after that first fill. Its callback's buy stop below the market closes at
83075.15 immediately; it is not a market newborn from the prior fill's callback.

The source and TradingView export are preserved byte-for-byte. Timestamps in
`tv_trades.csv` are UTC+8. `bars.csv` contains the exact pinned chart bars from
09:00 through 11:00 UTC on April 6, 2025. The tape has two closed trades, including
the callback's zero-length short. No population source was sent to TradingView.

- Source SHA-256: `6000a9830d16ada728710fbfd0b4e08dcbf77cb0629dd23dc85676f5489c9954`.
- TradingView tape SHA-256: `fbd5930e99a8543c9dad1dd2895d87182f5512d91f14e572d360da10f9503f0f`.
- Chart SHA-256: `6b54c44ac6de8b7b588b3fcc6ddc29b09e5fe12ce9559d811dca7859217e6270`.
