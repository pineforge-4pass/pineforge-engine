# slippage_finite_control

D6 rows 13/14 finite control: the default market sizing and actual fills use two 0.01 ticks on a flat 100-price feed. Forty five-minute bars, volume 1. Completion with trades is pinned before execution. The C ABI rows d6_11_13_finite_bits and d6_11_14_adapter_finite_bits separately compare the old expression order and binary64 results.

Authoring only: no transpile, compilation, engine execution or driver run in this lane. The expectation comes from D6 FINAL before any observation. A nonzero tripwire hit requires the supervisor's KEEP/ruling procedure; this file is not a sweep verdict.
