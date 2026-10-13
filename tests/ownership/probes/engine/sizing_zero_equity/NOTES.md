# sizing_zero_equity

Pinned before execution: D6 row 2 zero equity KEEP.

Ten units enter at 100. A mark of 10 loses exactly 900 on 900 capital, so the later percent sizing basis is exactly zero. It remains no-order and the run completes. The seed is explicitly closed to give a report trade; min_trades alone does not prove absence of the second order, which the C++ zero-sizing control asserts separately.

Feed: 40 synthetic one-minute bars, starting at 1736121600000 ms, volume 1. Each OHLC is the same deterministic price in its row. No market data, private fixture, generated C++, or output-derived expectation is used. Run through the P0 ordinary-source driver with input/script timeframe 1; no quantity, tick, FX, commission or margin overrides. Authored only: transpilation, compilation and runtime reachability have not been checked here.

The native boundary control names the requested source-free test TU. Its relevant native producer/control is an arithmetic counterpart; source-specific cash/equity/placement checks are in the proposed source companion. No source probe claims coverage of the internal pending-deduction corruption used for D6 row 6.
