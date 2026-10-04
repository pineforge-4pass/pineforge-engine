# explicit_qty_floor: explicit entry quantities and script-visible equity

Used by `tests/test_script_rule_tapes.py` (ctest row `test_explicit_qty_floor_generated_tapes`: the
product path, each tape's generated strategy) and
`tests/test_explicit_qty_floor_tapes.cpp` (hand-ported hosts that also read the script's equity).

## Rules

- **Explicit quantity floor.** An explicit `strategy.entry` quantity that the lot-grid floor
  would snap UP is floored on its shortest decimal. `(E/100)/0.3200000000000003` =
  3124.9999999999973 trades 3124 shares on NYSE:F (lot 1). Engine switch:
  `ScriptRuleSwitches::explicit_qty_decimal_floor`.
- **Equity mark.** `strategy.equity` and `strategy.openprofit` mark an open position at the
  tick-built close `floor(close / mintick + 0.5) * mintick`: 10.155 marks 10.15 and 10.265 marks
  10.27. Engine switch: `ScriptRuleSwitches::equity_tick_mark`.

## Provenance

- Exported 2026-10-04. These are self-written synthetic scripts run on the NYSE:F chart with
  `lab tv --no-note` over 2025-04-01 to 2026-05-01. No third-party source was sent to
  TradingView.
- Every control was exported twice, and the two exports are byte-identical. Each
  `provenance.json` records:
  - the control name and the export path;
  - the source, tape, bars and chart-feed sha256;
  - the repeats;
  - the sha of the predictor the rows were preregistered with (`a34016eb…`).
- `strategy.pine` and `tv_trades.csv` are the exported bytes. TradingView tape times are
  UTC+8.
- `bars.csv` holds the chart-feed rows of the fixture's window (`barsWindowUtc`):
  - NYSE:F 15m: `f-15-chart.csv`, 2025-04-01 to 2025-04-09.
  - NYSE:F 1D: `f-1d-chart.csv`, 2025-04-01 to 2025-11-01.
- `configuration.json` is the run configuration (the symbol facts the tapes ran under) and
  `generated.cpp` the strategy codegen emits for `strategy.pine`, frozen: byte-identical to the
  output of codegen ee04fc6 and of codegen 3cc1cf55 (v1.1.0). The test runs these frozen bytes,
  so a codegen change cannot move it; after one, regenerate each tape's file with
  `pineforge_codegen.transpile(<strategy.pine text>)` and run the test again. `tapes.txt` lists the
  tapes; `switch_off_departures.json` records what each switch, turned off, moves.

| Fixture | Control | What it pins |
|---|---|---|
| `equity-mark-long-fee` | n7-b1-long-fee | equity mark, long: 10.155 marks 10.15, fee 0.05%: E=102838.326425, qty 9110 |
| `equity-mark-long-nofee` | n7-b2-long-nofee | equity mark, fee 0: qty 9115 |
| `equity-mark-short-fee` | n7-b3-short-fee | equity mark, short: 9.565 marks 9.56: qty 3524 |
| `equity-mark-long-fee-round-up` | n7-b4-long-fee-markup | equity mark, 10.265 marks 10.27 (rounds up): qty 8666 |
| `computed-risk-1d` | n7-d1-computed | quantity floor: computed 3124.9999999999973 trades 3124 |
| `literal-below-5e-7-1d` | n7-d2-lit-5e-7 | quantity floor: literal 3124.9999995 trades 3124 |
| `literal-below-1e-5-1d` | n7-d3-lit-1e-5 | control: 3124.99999 trades 3124 with or without the floor |
| `literal-above-1d` | n7-d4-lit-above | control: 3125.0000000000005 trades 3125 |
| `computed-risk-split-1d` | n7-d5-computed-split | quantity floor with TP1 50% / MAIN stop legs: 1562 + 1562 |

The control names are the titles of the scripts TradingView ran (the key to the exported
bytes).

## What is compared

Each TradingView trade is compared, row for row, with the engine trade:

| Field | Tolerance |
|---|---|
| Side | exact |
| Entry and exit time | exact |
| Entry and exit price | to the tick (0.01; 6 decimals on the product path) |
| Quantity | 1e-8 |
| PnL | the ws report's float32 precision, max(1e-4, 1.2e-7·\|pnl\|) |

The hand-ported hosts also check, for the `equity-mark-*` fixtures, the read bar's TradingView
Signal `E=<equity>|O=<openprofit>|N=<netprofit>|P=<position_size>`
(`str.tostring(x, "#.##########")`) against the host's own values at that bar, rounded to 10
decimals: trades alone cannot show them.

Both tests then turn each switch off in turn:
- With `explicit_qty_decimal_floor` off, `computed-risk-1d`, `literal-below-5e-7-1d` and
  `computed-risk-split-1d` must fail (0 engine trades).
- With `equity_tick_mark` off, the four `equity-mark-*` fixtures must fail.
- Every other fixture must keep matching.

## Not pinned by these tapes

The quantity floor covers `strategy.entry` only: `strategy.order` with an explicit quantity
keeps the previous floor. A lot grid that is not an exact power of ten (0.5, 0.25) keeps the
previous floor as well. The equity mark uses the configured `syminfo.mintick`; a host that sets
none marks at the engine's 0.01 default, the tick its fills already use.
