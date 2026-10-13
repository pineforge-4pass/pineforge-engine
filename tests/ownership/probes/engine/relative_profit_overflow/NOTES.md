# relative_profit_overflow

D6 row 15, known long entry. Each bar is the finite binary64 value 2^1023; profit is also 2^1023 ticks and mintick is 1. Both the tick count and its tick product are finite; adding the engine-owned entry price overflows to infinity. This is G at relative_price.level, not a source tick-count failure. Before the producer, the relative leg can be dropped. No output was used to choose this expectation.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
