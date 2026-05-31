# input-source-runtime-override-high-01

## Feature under test
Engine #23 / codegen #9 — **runtime override of `input.source`**. The engine
must resolve a runtime override string to the matching native source series
(`get_input_source`), not silently fall back to the script default.

## Expected behavior
- Script default source is `close`; `inputs.json` overrides the `"Source"`
  input to `"high"`.
- The harness calls `strategy_set_input("Source", "high")`; the engine's
  `get_input_source("Source", _src_close_)` resolves to `_src_high_`.
- `ta.sma(src, …)` is therefore computed on **high**, so the crossover
  entries differ from the close-sourced run and must match a TV run whose
  Source input is set to High.

## TV capture notes
- 15m chart, ETH-USDT-USDT (Binance USDT-M Perpetual), window matching
  `corpus/data/ohlcv_ETH-USDT-USDT_15m.csv`.
- **Set the strategy input "Source" to `high`** before exporting.
- Export "List of trades" → `tv_trades.csv`.
- Pass = identical trade list. A divergence where the engine matches a
  *close*-sourced TV run instead means the override was ignored (the pre-fix
  bug).

## Regression value
This is the only probe that exercises the override resolution branch of
`get_input_source` — the unit test `test_get_input_source` pins resolution in
isolation, but this proves end-to-end parity with TV under an override.
