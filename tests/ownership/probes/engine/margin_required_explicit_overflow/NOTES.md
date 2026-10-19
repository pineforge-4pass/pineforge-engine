# margin_required_explicit_overflow

Pinned from D6 row 7 explicit placement requirement: finite explicit overflow KEEP per L6 10-14 ruling (`f2a455b14`).

The explicit quantity and mark are finite; the left-associated units * mark multiplication overflows in the source placement requirement. The ruling preserves signal-time refusal with cancellation and callback continuation for finite explicit overflow. G (`engine_command_value_invalid`) remains for nonfinite placement inputs/equity and for declaration-derived requirements. The probe pins that the run COMPLETES with 0 trades and the pending order is dropped.

Feed: 40 synthetic one-minute bars, starting at 1736121600000 ms, volume 1. Each OHLC is the same deterministic price in its row. No market data, private fixture, generated C++, or output-derived expectation is used. Run through the P0 ordinary-source driver with input/script timeframe 1; no quantity, tick, FX, commission or margin overrides.

The box run b4/b5 observation (completed, 0 trades, equity constant) is consistent with the ruling. The L6 10-14 ruling (`f2a455b14`) determines this expectation; the observation only corroborates it.

The native boundary control is the existing `tests/test_value_invalid_sizing_margin_source.cpp`: the L6 split is pinned at lines 196-197, and `keep_finite_explicit_placement_overflow()` at lines 232-262 checks completion and callback continuation after refusing the overflowing entry. The placement boundary is `src/source/pine_adapter.cpp:7365-7378`; the signal-time refusal and applicable cancellation are at lines 7430-7445. No source probe claims coverage of the internal pending-deduction corruption used for D6 row 6.
