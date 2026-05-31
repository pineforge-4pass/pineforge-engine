# input-source-subscript-hl2-01

## Feature under test
Engine #23 / codegen #9 — **history subscript on an `input.source` variable**
(`src[1]`, `src[2]`), with a **derived** native source (`hl2` → `_src_hl2_`).

## Expected behavior
- `src = input.source(hl2, "Source")`; the analyzer marks `src` a series
  (it is subscripted), so codegen emits `Series<double> src` advanced from
  `get_input_source("Source", _src_hl2_)[0]` each bar.
- `src[1]` / `src[2]` are real history reads (`hl2` of prior bars), so the
  two-bar momentum-turn entries match TV's `hl2`-based computation.

## TV capture notes
- 15m chart, ETH-USDT-USDT, window matching
  `corpus/data/ohlcv_ETH-USDT-USDT_15m.csv`.
- Defaults (Source = hl2). Export "List of trades" → `tv_trades.csv`.
- Pass = identical trade list. A compile failure or frozen-`src[1]` behavior
  is the pre-fix bug.

## Regression value
Exercises the series-binding path (subscripted source var → Series member)
and the derived-source mapping (`hl2` → `_src_hl2_`), which the unit tests
cover structurally but not against TV.
