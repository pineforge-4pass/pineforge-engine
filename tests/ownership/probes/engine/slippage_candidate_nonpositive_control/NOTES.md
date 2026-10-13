# slippage_candidate_nonpositive_control

D6 row 12 KEEP: a finite non-current short candidate on a 1-price feed with two 1-price slippage ticks resolves to -1. It is a defined NonpositivePrice rejection and must complete without a G failure. Forty flat five-minute bars, volume 1. The p0 schema has a minimum trade count only; min_trades=0 does not assert the absence of trades. d6_12_candidate_keep in the C ABI unit asserts the exact rejection reason, no applied events and no trades, using a non-finite candidate supplied by the public terms callback. No failure producer belongs in shared apply_slippage.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
