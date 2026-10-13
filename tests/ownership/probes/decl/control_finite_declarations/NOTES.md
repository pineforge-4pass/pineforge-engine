# control_finite_declarations

The quantity literal 1e300 is finite, within parser/C++ range, and must be admitted at the declaration door. Margins 25/50 are ordinary finite controls. The body supplies qty=1 so it never asks sizing to consume the huge declared quantity. This isolates declaration admission from later arithmetic and still requires a real completed trade.

Contract pin: EN-132 D3 default_qty_value/margins: finite values of any size are not S.
These controls should already pass on main; only the paired invalid native
boundary rows are red before the declaration producer exists. No outcome
has been observed in this lane.

Tape: 40 hand-rule synthetic one-minute bars, timestamp 1736121600000 +
i*60000, O=C=100, H=101, L=99, V=1000; no market data. The body requests
one long contract at bar 0 and a close at bar 3, allowing next-open fills
and at least one completed trade. Use input/script timeframes "1" / "1".
Expectation uses the p0 brief item 2 schema and was written before execution.
