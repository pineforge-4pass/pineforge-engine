# settlement_pnl_overflow

D6 row 21, AFTER-g134 follow-up expectation. The expectation is unchanged; the probe was rebuilt (TX-PROBES, CR-139) because its first form never reached row 21.

First form: four units entered at 1 and closed at bar 4's finite 1e308 print with the default percent commission. The fee quote `abs(price) * quantity * pointvalue * fx * (commission / 100)` overflows to infinity and times zero is NaN, so the run failed earlier with `engine_command_value_invalid` (`fee.commission`, `numeric_overflow`, stage `fee`) and the settlement money predicate was never witnessed. With a percent commission of 0 and positive prices a single trade cannot get a nonfinite PnL without that fee quote going NaN first (the fee chain is bounded by the same product), so the fee had to become price-free.

Now: `cash_per_contract` with value 0 makes every fee quote `0 * quantity`, which cannot overflow. Two long lots of 2 at 1 (`pyramiding=2`, entries on bars 0 and 1) are closed together at bar 4's finite 1e308 print. Two lots take the staged settlement, where `prepare_native_settlement_commit` (`src/engine_execution.cpp:1247-1281`) rejects the row whose PnL is `fl(fl(1e308 - 1) * 2)` = +inf with `InvalidAccounting` (`:1271-1272`), reported as `engine_invariant` (KEEP, D6 row 21). The expectation is the ruled row, not an observation; the box run produced `failed / engine_invariant / {}` with 0 trades. The driver cannot tell the staged path from the fused one-lot path, so the staged path is the design (two lots), not a measured fact.

Forty flat five-minute bars, first four at 1 and the rest at 1e308, volume 1; mintick=1 keeps all price normalization finite. No risk limit is configured. Note: a nonfinite PnL always makes `pnl_pct` nonfinite too (row 22), so a single closed row cannot isolate row 21 from row 22.

Authoring note: the earlier "Paired row: d6_21_settlement_pnl" does not exist in `tests/test_native_settlement_row_finite.cpp`; the native control file named in `expect.json` is that file.
