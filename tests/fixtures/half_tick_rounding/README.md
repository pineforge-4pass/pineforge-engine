# Half-tick fills (R5 lane PAR-MARGIN-2, item 2)

A margin call at an exact half-cent low booked a tick below TradingView (lane
PAR-MARGIN's M10 tapes `../margin_entry_bar/pm-m10-f-k4-1007` and
`-gapup-0501`: 12.105 booked 12.10 where TradingView books 12.11, 10.025 booked
10.02 against 10.03). Which rounding does TradingView apply, and to which fill?
These three `lab tv` exports (ws-report-v1, rangeProof covered; the lane's own
scripts) answer it on two symbols with different mintick. Every order is
placed at a signal close and closed 2 bars later.

- `pm2-round-f` (NYSE:F, mintick 0.01) and `pm2-round-es` (CME_MINI:ES1!,
  mintick 0.25): a buy stop, a sell stop, a buy limit and a sell limit, each at
  a HALF-TICK LEVEL (a literal) strictly inside the next bar's range on its
  crossing side, never gapped. The F levels are chosen so the nearest tick of
  the stored double and the order's directional tick differ; on ES they are
  exact binary halves. TradingView books the DIRECTIONAL tick, 8 of 8: a stop
  its adverse tick, a limit its favourable one -- no nearest rule, exact or
  floating, and no half-even rule fits all eight.
- `pm2-round-f-print` (NYSE:F): a market buy, a market sell and a sell stop
  already through the open, each filled at a half-cent OPEN print whose nearest
  tick differs from the order's adverse one. TradingView books the print's
  NEAREST tick, 3 of 3.

TradingView's margin call is a MARKET execution at the print it fired at (its
raw report files the 1007 call as `"tp": "MARKET"`, `"p": 12.11`), so it
takes the print rule; the adapter's pre-open slice took the stop-level rule.
Since the lane it books the print's nearest tick (`source_margin_fill_price`),
and the adapter books all three tapes' rows before and after -- the shared
`directional_tick` helper is TradingView's for every stop and limit and is not
changed. `bars.inc` holds the bars around each order from the lab feeds (NYSE:F
`80f404ae85ef`, CME_MINI:ES1! `766e8149d7e1`); replayed by
`tests/test_adapter_margin_schedule_differential.cpp` ("half-tick fills").

| probe | window | entries TradingView books (signal id: price) | tvTradesCsvHash |
|---|---|---|---|
| `pm2-round-f` | NYSE:F 2025-09-29 .. 2025-10-10 | BS: 12.33, SS: 12.2, SL: 12.17, BL: 12.27 | `e8377a83d2be83c4` |
| `pm2-round-es` | CME_MINI:ES1! 2025-10-05 .. 2025-10-10 | BS: 6785.75, SS: 6786.5, BL: 6785.25, SL: 6791.75 | `9774fca0167ddff8` |
| `pm2-round-f-print` | NYSE:F 2025-09-08 .. 2025-10-31 | MS: 11.7, GS: 11.76, MB: 13.91 | `a144cc0c22eafe0d` |
