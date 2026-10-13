# risk_intraday_threshold_overflow

D6 row 18, marked intraday threshold. Day-opening equity is finite 2^1023 and the configured percent is ordinary finite 50. Entry fills at 100; bar 2 and later mark at 99. The original multiplication overflows before division by 100; the guard must run before the old loss/epsilon predicates. No source operand is infinity or an out-of-range percentage.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
