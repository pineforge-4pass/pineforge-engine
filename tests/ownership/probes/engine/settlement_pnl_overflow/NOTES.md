# settlement_pnl_overflow

D6 row 21, AFTER-g134 follow-up expectation. Entry is four units at 1; the close requested at bar 3 fills at bar 4's finite 1e308 print. The row's PnL overflows and the existing money predicate rejects InvalidAccounting. The typed recode is deliberately deferred in this lane. Forty flat five-minute bars, first four at 1 and the rest at 1e308, volume 1; mintick=1 keeps all price normalization finite. Paired row: d6_21_settlement_pnl. No risk limit is configured. Main fails with engine_invariant, not the pinned settlement G. Do not claim green until the coordinated hunk and executed spot receipt.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
