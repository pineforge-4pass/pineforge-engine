# relative_fill_profit_overflow

D6 row 15, fill-point materialization. The market parent is pending when the exit is queued. Explicit exit qty=1 makes anchorable_relative_exit refuse native anchoring, so row 17's already-typed refusal cannot preempt this witness. The later finite parent fill 2^1023 plus the admitted finite offset 2^1023 overflows in materialize_relative_exits. This pins G before a materialized infinity can re-enter a source price door.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
