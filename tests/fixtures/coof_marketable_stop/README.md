# coof marketable stop

A short stop below the opening callback price is already marketable. Both entry and exit book 83075.15 at 10:00 UTC.

Campaign-authored independent synthetic on BINANCE:BTCUSDT 15m. `strategy.pine`, `tv_trades.csv`, `metrics.json` and `meta.json` preserve the TradingView export. CSV timestamps are UTC+8; the replay subtracts eight hours. No population source was sent to TradingView.

- Source SHA-256: `f9dbf8f4bfec777c2f108bb054923f65ed58e1b8c79ece1c1432565f15d4e59d`.
- TradingView tape SHA-256: `f0d02eb848b5bd7c5b91c0d52fee866763285207002c9fc1f20b07ee34b49e8e`.
- Chart feed SHA-256: `6b54c44ac6de8b7b588b3fcc6ddc29b09e5fe12ce9559d811dca7859217e6270`.
- `bars.csv`: 7 exact chart-feed rows from 2025-04-06 09:30 through 2025-04-06 11:00 UTC, sufficient for this timestamp-driven synthetic.

The corresponding `test_coof_marketable_stop_tapes` compares trade count, direction, entry/exit timestamps, tick prices, quantity and PnL.
## Supported scope

The original exports use a short position, zero slippage and zero short margin.
The `open-*` controls additionally cover margins 0 and 100, both sides,
and an opening scratch followed by a separately identified entry. The
`afterclose-confirmed-*` controls close a prior short at the opening print,
then create a new short in that callback. Its wrong-side stop scratches at
83075.15, and the post-scratch entry waits for the next waypoint, 82849.43.
The source adapter records that continuation on the submitted stop and hashes
the receipt; it does not advance or alter the native matching path.

The extension applies to an unslipped short market-entry callback at an
ordinary, non-magnified opening point whose tick-quantized open equals its high,
independently of margin or whether the parent was born in an earlier callback.
The `positive-leg-*` counter-controls distinguish an opening print of
82910.09 from its first high of 82960: the stop books the high, not the open,
and a post-scratch re-entry books the following low of 82716.49. Positive
opening legs retain their previous activation and conversion qualifications.
The `carried-m0-reenter` and `carried-m100-reenter` controls instead place the
Market parent on the preceding bar. Both fresh TradingView tapes scratch that
short at 83075.15 and book its callback's `second` re-entry at the same Open
print, not the Low. Only callback-born, non-first-open parents advance past
the coincident opening high after that scratch.
The untaped long-after-close coincident shape, slipped fills and magnified
paths retain their previous behavior. The witness
shape audit is recorded in the
quiet-bar and publication pinned includes; their stop-related digits are
unchanged from the baseline. Only the independently taped partial-close
Random37 expectations differ from baseline.

The immediate stop activation and conversion require the same ordinary path
and are restricted to an actual native opening
market-entry callback. A first-fill callback on any other path phase retains
its existing `Stop` trigger instead of becoming a `Market` request. This
keeps the frozen `ConfigFlags103` publication digest and the five live-state
transcripts (brackets seeds 314188 and 1780394; chains seeds 314189, 1780395
and 3246601) unchanged. No expectation is refreshed for those six checks.
The slipped-close projection applies to immediate closes and named closes
whose opening fill callback has only that entry cohort exposed.
A next-tick FIFO close that spans other entry cohorts keeps its existing
projection; the Random45 quiet-bar pins remain at their original baseline
values. The existing `../coof_open_limit/td-fp17-coof-open` TradingView export
also pins the default, non-immediate, single-cohort close: LC books 24977.75
(499555 ticks) and SD books 25180.00 (503600 ticks), not the slipped
callback prices. Its test continues to include both close cells.

Each new directory includes exact independently authored Pine bytes, a fresh
2026-10-03 TradingView WebSocket tape, and `provenance.json` with the source,
tape, report, frame and chart-feed hashes. Its minimal `bars.csv` retains the
exact chart rows from 2025-04-06 09:00 through 2025-04-07 01:15 UTC.
