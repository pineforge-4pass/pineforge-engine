# relative_loss_overflow

D6 row 15, known short entry. Loss places the stop above the entry: entry minus (-1)*loss*mintick. Entry, loss and tick product are each finite 2^1023; the resulting sum overflows. Source raw loss admission must succeed. The G producer is at the engine-relative construction.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
