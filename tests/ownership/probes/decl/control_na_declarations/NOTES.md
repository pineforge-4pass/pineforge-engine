# control_na_declarations

Each mapped numeric argument is source na. The literal extractor returns None, so all five numeric assignments are omitted and finite host struct defaults survive. This is a completion control, not a runtime NaN producer probe. The native NaN controls are decl_default_qty_value_nan_preserved, decl_margin_long_nan_preserved, and decl_margin_short_nan_preserved in tests/test_value_admission_decl.cpp.

Contract pin: EN-132 D3-a conditional NaN ruling; MAP section 1 NaLiteral -> None.
These controls should already pass on main; only the paired invalid native
boundary rows are red before the declaration producer exists. No outcome
has been observed in this lane.

Tape: 40 hand-rule synthetic one-minute bars, timestamp 1736121600000 +
i*60000, O=C=100, H=101, L=99, V=1000; no market data. The body requests
one long contract at bar 0 and a close at bar 3, allowing next-open fills
and at least one completed trade. Use input/script timeframes "1" / "1".
Expectation uses the p0 brief item 2 schema and was written before execution.
