# Explicit-quantity closing stop control

The campaign-authored synthetic places a naked quantity-one stop at the creation close. TradingView fills on that close, without a bracket or default-percent sizing. It pins the unchanged control beside the pending-bracket regression. The source and TradingView export are preserved byte-for-byte; timestamps are UTC+8. `bars.csv` is the same exact bounded chart feed as the adjacent regression fixture.

- Source SHA-256: `5796bd3f45ceb324e98618d8e87d302a335b930aefe40d9184f9fab4e1ec3a3c`.
- TV tape SHA-256: `968ffa78193c051087e5a95385b72d489ced0f85e9a0b6ebeec3df4188257a0c`.
