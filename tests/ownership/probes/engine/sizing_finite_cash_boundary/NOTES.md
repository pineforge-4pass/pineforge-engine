# sizing_finite_cash_boundary

Pinned before execution: D6 rows 1/4 finite boundary control.

Finite cash just below 300 at price 100 stays on the ordinary conversion path. The native finite-boundary test separately compares quantity and accounting double bytes with an explicit-units reference.

Feed: 40 synthetic one-minute bars, starting at 1736121600000 ms, volume 1. Each OHLC is the same deterministic price in its row. No market data, private fixture, generated C++, or output-derived expectation is used. Run through the P0 ordinary-source driver with input/script timeframe 1; no quantity, tick, FX, commission or margin overrides. Authored only: transpilation, compilation and runtime reachability have not been checked here.

The native boundary control names the requested source-free test TU. Its relevant native producer/control is an arithmetic counterpart; source-specific cash/equity/placement checks are in the proposed source companion. No source probe claims coverage of the internal pending-deduction corruption used for D6 row 6.
