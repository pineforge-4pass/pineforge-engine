# risk_preclose_threshold_overflow

D6 row 18, preclose fill threshold. Capital/day-opening equity is finite 2^1023 and configured percent is ordinary finite 50. Enable it immediately before closing the already-open lot inside the calculation, then disable it after the call returns. The applied callback computes an overflowing threshold before a later marked-price checkpoint. Disabling isolates this witness from the other producer. The spot box must establish the callback routing; no run was performed here.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
