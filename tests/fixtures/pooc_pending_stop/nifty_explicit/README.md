# Explicit-quantity closing stop control

The campaign-authored synthetic places a naked quantity-one stop at the creation close. TradingView fills on that close, without a bracket or default-percent sizing. It pins the unchanged control beside the pending-bracket regression. The source and TradingView export are preserved byte-for-byte; timestamps are UTC+8. `bars.csv` is the same exact bounded chart feed as the adjacent regression fixture.

- Source SHA-256: `9f6d43105bf3a6e5e82ce4d7290cf03e75f7852a80ed3a73c629efee5df8a2ae`.
- TV tape SHA-256: `5aed04e5b2895078060b292746e5472a7a79dd1b9c1f9a0d0dbb8572ecb0d1b0`.
