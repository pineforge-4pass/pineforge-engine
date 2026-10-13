# relative_finite_boundary_control

D6 row 15 finite boundary control. Entry and profit are 2^1022 with tick=1; the computed target is finite 2^1023. The later explicit close yields one trade with zero P&L. The unit test pairs a relative level with the exact absolute level and uses byte comparisons of doubles.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
