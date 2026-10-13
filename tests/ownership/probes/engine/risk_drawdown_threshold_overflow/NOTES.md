# risk_drawdown_threshold_overflow

D6 row 18. Capital/peak are finite 2^1023; configured drawdown percentage is an ordinary finite 50. The original peak*value/100 evaluation overflows at multiplication even though the mathematical percentage would fit. This avoids depending on a language accepting a percentage above 100. Preserve the arithmetic and pin risk.threshold/risk/numeric_overflow, rather than silently disabling the limit.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
