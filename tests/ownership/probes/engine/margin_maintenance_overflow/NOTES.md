# margin_maintenance_overflow

Pinned before execution: D6 row 10 maintenance requirement. The expectation is unchanged; the probe was rebuilt (TX-PROBES, CR-139) because its first form never reached the row.

The entry quantity was 1e-10; the run completed with no trade and no position, so the maintenance producer (`src/native_execution_consumer.cpp:3836-3863`, which returns while the book is flat) was never reached. The quantity is now 1: the entry fills at the bar-2 open at price 1 with a finite placement requirement (1e306 against equity 1e307) and a finite kernel unit maintenance (1e306). After the price jumps to 100000 at bar 6, the kernel unit-maintenance product overflows, despite a finite position P&L, and the maintenance producer must fail with `engine_command_value_invalid` (`margin.maintenance`, `nonfinite`, stage `margin`). Fractions are finite and positive so the model is built. Why the 1e-10 entry did not fill is not established; the fix does not depend on it.

Feed: 40 synthetic one-minute bars, starting at 1736121600000 ms, volume 1. Each OHLC is the same deterministic price in its row. No market data, private fixture, generated C++, or output-derived expectation is used. Run through the P0 ordinary-source driver with input/script timeframe 1; no quantity, tick, FX, commission or margin overrides.

The expectation is the contract row (revision 3: a value derived by the engine from its own margin state that is nonfinite is `engine_command_value_invalid`), not an observation. The box run with qty 1 produced that result with 0 trades and exit 1.

The native boundary control names the requested source-free test TU. Its relevant native producer/control is an arithmetic counterpart; source-specific cash/equity/placement checks are in the proposed source companion. No source probe claims coverage of the internal pending-deduction corruption used for D6 row 6.

Known edge, not covered here: a 1x long (`margin_long=100`) with a point value other than 1 and no lot grid is never maintenance-checked by the adapter (`src/source/pine_adapter.cpp:18548-18559`, `:22630`), so this row is witnessed on the checked route (margin other than 100, or a short).
