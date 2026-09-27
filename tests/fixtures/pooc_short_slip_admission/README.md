# A slipped percent short opens only while its margin at the close fits (lane W4-ENG-POOC-SAMEPASS, F14)

TradingView's own trades for default-quantity percent_of_equity MARKET entries
under `process_orders_on_close` with slippage, written for this lane. Each
directory is one `lab tv --no-note` export (channel `ws-report-v1`, `rangeProof`
covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01.
`tests/test_pooc_short_slip_admission_tapes.cpp` replays every tape.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `w4-f14-pooc-v6-slip` | POOC, v6 default (percent 100), slippage 15 | 6 | `7c52f8d69b05480777102693aaf5d0e39b2b91e94e7cb32f3a589049021201e5` |
| `w4-f14-pooc-pct50` | POOC, percent 50 | 13 | `91f81c834cecc5486a906b62cba4b188da3082ea9a08423f6330483a8d3f4df5` |
| `w4-f14-plain-fixed1` | fixed 1 | 13 | `d928a7989c6865dbd844fae49d2c548afb5182875f4253bae976d1ad1d6db093` |
| `w4-f14m-p99995` | POOC, percent 99.995, slippage 15 | 1 | `107d88e96721447c958721506d2c2db14c84eaba277287f72fa6f736c5ae7791` |
| `w4-f14m-p9999` | POOC, percent 99.99, slippage 15 | 4 | `a6e150ecf9a1674d3bf0c6ad25b5c5fb7e5f2abe1981fd441d33422948e96d7b` |
| `w4-f14m-p99985` | POOC, percent 99.985, slippage 15 | 5 | `572d7f9904044e043dd241c1cfd6bb1dcbbac10ce4f9c7bbe98ab6e0eb952db8` |
| `w4-f14m-p9998` | POOC, percent 99.98, slippage 15 | 3 | `d944a8bcd87b65557a065dd15c679d3bdb7f84979885bde50e47adf9ac0bd6fc` |

What they show:

- `w4-f14-*` (cells A..G on 2025-04-08 / 09, each flattened two hours later): a
  long held, then on one calculation its `strategy.close` and the short entry
  (A), the short entry and the close (B), the short entry alone (C), a short held
  and its close with a long entry (D), a close of a side not held and a short
  entry from flat (E), A with a bracket for the short (F), `strategy.close_all`
  and the short entry (G). At 100 % of equity and slippage 15 no short opens,
  from flat or by reversal, and every reversal still closes its long; at 50 %
  without slippage every short opens.
- `w4-f14m-*`: one flat short at 2025-04-08 00:00 (close 1559.12), one flat long
  at 04:00. The short is sized at the slipped fill (1558.97) and opens only while
  its units at the close fit the equity: 99.99 % (64.1385 x 1559.12 = 99999.6 of
  100000) opens, 99.995 % does not. On 99.99 % and 99.985 % a margin call slices
  the short on its entry bar (the margin model's, not this rule's).
- Cells F: the bracket placed after its short entry on the calculation that
  closes the long survives that close's flat and fills for the short.

## Bars

`bars.inc` holds the replayed bars, 2025-04-08 00:00 .. 2025-04-09 08:00 UTC,
copied as text from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`);
`exec/W4-ENG-POOC-SAMEPASS-scratch/tools/gen_bars_inc.py` wrote it.
