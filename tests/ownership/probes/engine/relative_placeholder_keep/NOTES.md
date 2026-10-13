# relative_placeholder_keep

D6 row 16 KEEP. A pending short parent fills at 100 with profit=200 and mintick=1. Its finite projected target is -100, which a nonnegative trigger cannot hold. resolve_anchored_level must return the deliberate tick=1 placeholder and store NaN; the same fill notification withdraws that child. The explicit later close completes with a trade. The C ABI source witness also inspects Armed/Cancelled events and compares every trade money field with a no-exit twin.

Authoring only. Expectations were pinned before execution. Feed rule: 40 one-minute bars starting at 1700000040000 ms, O=H=L=C at the prices described above, volume=1. Each next timestamp is exactly 60000 ms later. Data is synthetic and independent of market/private fixtures. run.json uses P0's options schema; syminfo fixes tick/point value/quantity step to 1 and a UTC 24x7 calendar.
