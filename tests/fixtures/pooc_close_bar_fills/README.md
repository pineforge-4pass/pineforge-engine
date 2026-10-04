# pooc_close_bar_fills: the close-time script sees its bar's fills

Used by `tests/test_script_rule_tapes.py` (ctest row `test_pooc_close_bar_fills_generated_tapes`:
the product path, each tape's generated strategy) and `tests/test_pooc_close_bar_fills_tapes.cpp`
(hand-ported hosts that also read the guarded branch's readouts).

## Rule

Under `process_orders_on_close`, a stop entry and its bracket exit that both fill inside one bar
settle before the close-time script runs. So `strategy.position_size == 0` there, and a stop
re-armed at the close fills at that close.

The case is BINANCE:BTCUSDT 15m, 2025-08-03 10:00 UTC. Stop 113689.29 fills, then limit 113771.41.
The re-arm stop 113771.47 (= the close) fills at 10:00 and exits at 11:00 @114004.53. This holds
even when the book still holds an exit whose entry is neither open nor working, such as the
"Sell Exit" of the cancelled 2025-08-02 "Sell" re-arms. Engine switch:
`ScriptRuleSwitches::pooc_bracket_skips_inert_exits`.

## Provenance

- Exported 2026-10-04: self-written synthetic scripts run on the BINANCE:BTCUSDT chart with
  `lab tv --no-note` over 2025-04-01 to 2026-05-01. Every control was exported twice, and the two
  exports are byte-identical. No third-party source was sent to TradingView.
- The four `scheduled-stop-*` fixtures are earlier tapes of the same bar, one export each
  (2026-10-03).
- `strategy.pine` and `tv_trades.csv` are the exported bytes. Tape times are UTC+8.
- `bars.csv` holds the rows of `btc-15-chart.csv` (sha `6b54c44a…`) from 2025-08-02 00:00 to
  2025-08-03 13:15 UTC (exclusive). Every control acts only on 2025-08-02/03.
- Each `provenance.json` records the source, tape, bars and feed sha256, the repeats, the export
  path and the sha of the predictor the rows were preregistered with (`a34016eb…`; none for the
  four earlier tapes).
- `configuration.json` is the run configuration (the symbol facts the tapes ran under) and
  `generated.cpp` the strategy codegen emits for `strategy.pine`, frozen: byte-identical to the
  output of codegen ee04fc6 and of codegen 3cc1cf55 (v1.1.0). The test runs these frozen bytes,
  so a codegen change cannot move it; after one, regenerate each tape's file with
  `pineforge_codegen.transpile(<strategy.pine text>)` and run the test again. `tapes.txt` lists the
  tapes; `switch_off_departures.json` records what each switch, turned off, moves.

| Fixture | Control | Varies |
|---|---|---|
| `rearm-pop-capital` | n8-v1q-pop | prior same-ID fill+exit, cancel + re-arm, capital 92861.56 |
| `rearm-capital-100k` | n8-v1p-cap100k | capital 100000 |
| `rearm-fixed-qty` | n8-v1f-fixed | fixed 0.5 |
| `rearm-no-cancel` | n8-v2q-nocancel | no `cancel("Buy")` before the re-arm |
| `rearm-no-prior` | n8-v3q-noprior | no prior order |
| `rearm-prior-unfilled` | n8-v4q-unfilled | prior order never fills |
| `rearm-pooc-off` | n8-v5q-nopooc | negative control: POOC off, the re-arm fills 10:15 |
| `rearm-prior-other-id` | n8-v6q-otherid | prior under another ID |
| `guard-prior-fill` | n8-v7q-guard | `if position_size == 0` branch readout |
| `guard-pooc-off` | n8-v8q-guard-nopooc | guard, POOC off |
| `guard-no-prior` | n8-v10q-guard-noprior | guard, no prior |
| `guard-margin-prior` | n8-v11q-marginprior-guard | 08-02 margin-at-open short, guarded |
| `guard-fixed-prior` | n8-v12q-nomargin-guard | 08-02 short of 0.5 (no margin call), guarded |
| `rearm-margin-prior` | n8-v13q-marginprior-unguarded | margin ancestry, unguarded |
| `guard-dormant-exit-history` | n8-v15r-rearm-guard-popcap | the 08-02 re-arm history, guarded (pins the rule) |
| `rearm-dormant-exit-history` | n8-v16r-rearm-unguarded-popcap | the same, unguarded (pins the rule) |
| `guard-margin-history-no-rearms` | n8-v17r-norearm-guard-popcap | negative control: no 08-02 re-arms |
| `scheduled-stop-{pooc,next}-{cancel,keep}` | px-f-close-{pooc,next}-{cancel,keep} | earlier tapes: creation-close fill and next-bar cancel |

The control names are the titles and export names of the scripts TradingView ran (the key to the
exported bytes).

## What is compared

Each TradingView trade is compared, row for row, with the engine trade:

| Field | Tolerance |
|---|---|
| Side | exact |
| Entry and exit time | exact |
| Entry and exit price | to the tick (0.01; 6 decimals on the product path) |
| Quantity | 1e-8 |
| PnL | max(1e-4, 1.2e-7·\|pnl\|) |

The hand-ported hosts also check, for the guarded controls, the re-arm's Signal
`flatAtClose|pos=0|ct=N|ot=0` (or `openAtClose|...`): the branch the engine's close-time script
took, with its position, closed-trade and open-trade readouts.

Both tests then turn `pooc_bracket_skips_inert_exits` off. The two `*-dormant-exit-history`
fixtures must fail: the engine takes the `openAtClose` branch, or loses the second trade. Every
other fixture must keep matching.

## Backtest and stream

Run as a confirmed-bar stream (`strategy_stream_*`), each of these sources books the same trades as
the backtest, every trade field equal: the rule, the in-bar bracket exit included, applies
identically in both modes, and so does the re-arm filled at the close.
