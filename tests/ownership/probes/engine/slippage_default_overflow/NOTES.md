# slippage_default_overflow

D6 row 14: both the flat 1e308 feed price and mintick=1e308 metadata are finite. The snapshot mark snaps to one tick. Two slippage ticks overflow when the engine derives the default market sizing fill. All 40 bars are flat, five minutes apart, volume 1. Metadata is a caller-provided valid instrument fact, not a script-invalid declaration. The result guard is at the consumer of default_market_sizing_price because that helper is noexcept. Paired native/source fixture row: d6_14_default_snapshot (and d6_14_default_core). Main follows absence/no-order rather than G. This is authored reachability, not an executed receipt.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
