# control_omitted

All five mapped numeric members are omitted. The constructor emits v6 initial_capital=100000 and default_qty_value=100; the struct supplies commission_value=0 and both margins=100. Explicit qty=1 makes the trade assertion independent of default sizing. This proves completion through the ordinary toolchain; it does not inspect constructor scalar values.

Contract pin: EN-132 D3-a omission is finite; codegen Pine v6 defaults.
These controls should already pass on main; only the paired invalid native
boundary rows are red before the declaration producer exists. No outcome
has been observed in this lane.

Tape: 40 hand-rule synthetic one-minute bars, timestamp 1736121600000 +
i*60000, O=C=100, H=101, L=99, V=1000; no market data. The body requests
one long contract at bar 0 and a close at bar 3, allowing next-open fills
and at least one completed trade. Use input/script timeframes "1" / "1".
Expectation uses the p0 brief item 2 schema and was written before execution.
