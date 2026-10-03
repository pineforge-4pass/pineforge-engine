# Cross-mechanism TradingView tapes

These are newly authored synthetic strategies, exported with `lab tv --no-note`
on 2026-10-03. TradingView's closed trades are the only parity oracle.
`test_cross_mechanism_tapes` implements each Pine strategy as a separate
`PineStrategyHost` subclass and replays the committed bars.

## Cases

The ETH cases use `BINANCE:ETHUSDT.P`, 15-minute bars, 2025-04-01 through
2025-04-07 inclusive, tick 0.01 and quantity step 0.0001. The carried trail uses
`NASDAQ:AAPL`, 15-minute bars, 2026-03-04 through 2026-03-06, tick 0.01 and
quantity step 1. There are nine cases, including both margin variants.

| Case | Combination | TV trades | Candidate agrees | Main agrees | Parity asserted |
| --- | --- | ---: | --- | --- | --- |
| bracket_one | Frozen three-entry batch + same-bar named bracket | 4 | Yes | Yes | Yes |
| bracket_all | Frozen three-entry batch + same-bar global bracket | 3 | Yes | Yes | Yes |
| cancel_pending | Frozen three-entry batch + same-bar pending cancel | 2 | Yes | Yes | Yes |
| opposite_partial_fifo | Opposite-side batch + next-bar 50% FIFO close | 4 | No (5 trades) | No (5 trades) | No |
| margin_batch_100 | Three entries + 100% margin + opening cash fees | 5 | Yes | No (4 trades) | Yes |
| margin_batch_50 | Three entries + 50% margin + opening cash fees | 6 | No (4 trades) | No (4 trades) | No |
| margin_fractional_boundary | Fractional margin call at the A+B lot boundary | 3 | No (wrong fills) | No (same wrong fills) | No |
| allin_percent_reversal | All-in entry + reversal + margin + recreated 50% exits | 5 | Yes | No (6 trades) | Yes |
| batch_carried_trail | Frozen three-entry batch + activated carried sub-tick trail | 3 | Yes | No (wrong trail price) | Yes |

The measured runtime revisions are candidate `a747ad5b` and main `227c2236`.
The CSVs `engine_wave.csv` and `engine_main.csv` preserve the x86-64 observations
for diagnosis; they are never read as expected test results. The three
disagreements already fail main, so none is a newly failing candidate case.
The half-margin tape changes between the two revisions and remains divergent.

## Executed mechanism witnesses

The test observes `source_pending_view()` immediately after placement, before
any dependent exit/cancel. Finite `frozen_market_own_units` prove the frozen
batch route was eligible: three of three entries for the brackets, cancel and
trail, four of four for the short-to-long batch. Margin and percent entries have
zero frozen entries. The cancellation leaves two pending entries. The opposite
batch freezes a 0.0008 reversal transaction and invokes one 50% close next bar.

The all-in case records a positive position before reversal and executes two
recreated short percentage exits. Its TV witness confirms the first recreated
50% target uses the whole short entry incarnation, including a margin-closed
slice, rather than half the reduced position. The carried-trail witness reads
activated native trail state with best price 262.34 before the next raw 262.345
opening; TV then fills at the tick-rounded opening 262.35. FIFO means this
`from_entry="B"` exit closes A first.

TV witnesses also require named/global bracket quantities 2/6, cancellation
absence, exactly two FIFO-close slices totalling 0.0011, real margin-call rows,
and an exact 0.0008 fractional margin prefix consisting of A and B only.
Default test execution checks these witnesses and the candidate's mechanism
counters for **all nine** cases, including the three recorded findings. It
checks count, side, entry ID, entry/exit timestamps, entry/exit prices and
quantity against TV for the six agreeing cases. Recorded findings still replay
and print all differing rows; their engine values are not asserted.

`--record` emits all TV/engine rows and proof counters for comparison against
another runtime revision. Its successful exit is only an observation receipt,
not a parity verdict or a claim that that revision has the candidate's route.

## Recorded disagreements

All ETH entry fills below are 2025-04-01 00:15 UTC at 1821.48.

- **Opposite partial FIFO:** both revisions add a C close fragment of
  `2.1684043449710089e-19` at 00:30 / 1826.37. TV drains exactly A 0.0005 and
  B 0.0006, leaving C 0.0022 for cleanup. Main also reports the opening scratch
  as short S closed by A, whereas TV and the candidate report long A closed by S.
  Suspected remaining issue: percentage FIFO resolution after reversal when
  mirror quantities appear to equal a live prefix.
- **Half margin:** TV liquidates A 0.0008 at 00:15 / 1821.48, 0.0064 at
  00:15 / 1820.11 and 0.004 at 00:45 / 1816.64. The candidate instead liquidates
  0.0096 at 00:15 / 1820.11; main liquidates 0.0196 at 00:45 / 1816.64.
  Suspected remaining issue: leveraged opening-fee checkpoint/follow-up scope.
- **Fractional margin boundary:** TV closes A 0.0003 and B 0.0005 at
  00:15 / 1829.36 and leaves C 0.0492 until 00:45 / 1824.93. Both revisions
  instead close all three at 00:15 / 1826.38. Suspected existing issue:
  fractional small-book margin sizing or its close-time fallback.

These are findings, not corrected engine behavior or alternate oracle values.

## Provenance and tolerances

Each `provenance.json` records the exact source SHA-256, export time, chart,
requested/returned range and coverage proof, report/frame hashes, source and
committed CSV hashes, and bar lineage. Auth and account metadata are omitted.
CSV normalization only changes line endings to LF and adds the final newline;
TV row contents are unchanged. Bar milliseconds are UTC, TV date strings are
UTC+8, and the observation CSVs use UTC milliseconds. ETH bars are extracted
mechanically from the existing `percent_entry_reservation` feed. The four
contiguous AAPL bars cover every command/fill and retain the raw half-tick open
from the existing `carried_trail_open` feed.

Prices compare in 0.01 symbol ticks and quantities within 1e-8, matching
`order_print_tape_fixture.hpp`. Counts, side, entry identity and timestamps are
exact. The test does not compare native internal exit labels or change any
grader, metric or tolerance.
