# settlement_pct_overflow

D6 row 22, AFTER-g134 follow-up expectation. One unit enters at 1 and closes at the finite 1e308 print at bar 4. Money PnL remains finite while (pnl / entry_cost) * 100 overflows pnl_pct. This is one of the six previously unchecked Trade doubles. Forty five-minute bars, first four at 1 and the rest at 1e308, volume 1; mintick=1. Existing main can publish the invalid row; g134 first rejects it as engine_invariant and the coordinated EN-132 recode then owns it as G. Paired C ABI row d6_22_settlement_excursion exercises another newly guarded Trade double; the source probe covers pct directly. Nothing in either protected g134 function is edited here.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
