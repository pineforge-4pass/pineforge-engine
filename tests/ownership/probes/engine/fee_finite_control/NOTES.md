# fee_finite_control

D6 row 11 finite control: the same two-unit entry and close use a finite 0.125-per-unit fee. Forty flat five-minute bars at 100 with volume 1. This control should already complete on main. Exact fee, price and PnL bits are checked by d6_11_13_finite_bits and d6_11_fee_evaluation_bits in the paired C ABI test; the probe driver checks completion and at least one trade.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
