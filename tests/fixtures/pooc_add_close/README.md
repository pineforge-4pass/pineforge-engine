# The process_orders_on_close pass fills priced adds and reversing limits, then their exits (lanes W10 diag-w5r, TAIL-C)

TradingView's own trades for the lane W10-DIAG-UNKNOWN (diag-w5r) synthetic script, cells on
2025-04-14 .. 04-16: each opens with a market entry on bar t0 (filled at t0's close) and
places its orders on t1 = t0 + 15m; fixed 1, slippage 3, commission 0.05 %, pyramiding 3,
cleanup two hours after t0. `tests/test_pooc_add_close_tape.cpp` replays all 23 trades
through the Pine adapter over the corpus 15m chart feed (`bars.inc`).

What the tape shows, under process_orders_on_close:

- **Close-pass entries.** Every entry the calculation placed whose price the close tick
  reaches fills in the close pass -- a limit at the close tick unslipped, a stop at the close
  tick slipped -- whatever the book: a limit add while its own side is held (cells A, B, C, E),
  a stop add (H), a flat limit (F), a flat stop (G) and a limit placed against the held side,
  reversing it (J). A limit add the close does not reach rests (D).
- **Close-pass exits.** Then every exit the calculation placed is decided against the same
  close, the exits naming an entry the pass just filled included: A2x/B2x at close +- 3 ticks,
  E2x (a limit, unslipped), F1x, G1x, H2x, J2x.
- **Pre-entry exit.** An exit placed before the entry it names is never applied (I2x).
- **Equity.** `strategy.equity` at t1's close is net of N1's paid entry fee (cell N, N2
  0.9047); `strategy.openprofit` stays gross of it (N3) -- lane W8E-EXITS's rule
  (`../open_entry_fee_charged`).

| tape | trades | tv_trades.csv sha256 |
|---|---:|---|
| `w5r-pooc-add-close` | 23 | `8414352ea60d8b07b9965ae975c46c77cc24ee4bd55f49268214783ced68dabf` |

The directory is one `lab tv` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01; exported by lane W10
into exec/W10-DIAG-UNKNOWN-scratch/tv/diag-w5r), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.

## Bars

`bars.inc` holds the replayed bars, 2025-04-14 00:00 .. 2025-04-16 12:00 UTC, copied as text
from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`);
`exec/TAIL-C-scratch/tools/gen_bars_inc.py` wrote it.
