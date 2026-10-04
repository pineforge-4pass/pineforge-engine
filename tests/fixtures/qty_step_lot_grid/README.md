# qty_step_lot_grid: the lot-grid regression case (binding, byte-identical)

Used by `tests/test_qty_step_lot_grid_case.cpp`.

The case: BINANCE:BTCUSDT 4h, EMA 20/50 crossover, long only, 100 % of equity, 0.1 % commission,
initial capital 10000.
- **With the grid** (`qty_step` = `mincontract` = 1e-05): 22 rows, first quantity 0.08166.
- **Gridless:** 27 rows, 5 of them below one lot.

## Provenance

The case is a frozen release-harness case. Its recorded
rows come from engine and codegen 1.0.1 (release image
`ghcr.io/pineforge-4pass/pineforge-release:1.0.1`). These files are its exact bytes:
- `case.json`
- `strategy.pine` (sha `4637eabe…`)
- `ohlcv.csv` (2,188 bars, sha `a53fc5ab…`)
- `expected-rows-with-grid.json`, `expected-rows-without-grid.json`
- `syminfo-*.json`
- `check_case.py`

`generated.cpp` is the codegen output of `strategy.pine` (`pineforge_codegen.transpile`), frozen:
byte-identical to the output of codegen ee04fc6 and of codegen 3cc1cf55 (v1.1.0). The test links it
in as a fixture object, like `checked_settings`; after a codegen change, regenerate it the same way
and run the test again.

## What is compared

The test runs the case through the C ABI as the release harness does:
- all 2,188 bars;
- empty input and script timeframes (auto-detected 240);
- syminfo mintick 0.01, pointvalue 1, timezone UTC, session 24x7;
- with the grid, the metadata `qty_step` and `mincontract` set to 1e-05.

Every recorded row must come back field for field with **exact double equality**: side, entry and
exit time, entry and exit price, qty, pnl, commission, entry incarnation, and the exit id (or
`open_at_end` for the range-end row). This applies to both the grid run and the gridless run.
