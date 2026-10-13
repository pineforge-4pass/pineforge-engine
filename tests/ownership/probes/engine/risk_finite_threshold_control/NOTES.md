# risk_finite_threshold_control

D6 row 18 finite control. Both configured percentages are 10 and equity is 10000, so both thresholds are exactly 1000 and no breach occurs. One finite entry and explicit close produce one zero-P&L trade. Native unit controls also compare percent/absolute money and equity-curve values byte for byte, including a finite near-overflow boundary.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
