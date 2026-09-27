# A from_entry "" exit sizes on the position it fills against (lane W4-ENG-POOC-SAMEPASS, F19)

TradingView's own trades for a `strategy.exit` with `from_entry ""` and no
quantity placed while one lot is held, together with a market add of another id
(the DCA take-profit shape of job-2388), written for this lane. Each directory is
one `lab tv --no-note` export (channel `ws-report-v1`, `rangeProof` covered), byte
for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, `pyramiding=5`,
fixed 1, with cells on 2025-04-11 each flattened a little over an hour later.
`tests/test_global_exit_fill_size_tapes.cpp` replays every tape.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `w4-f19a-plain` | -- | 15 | `81a958ed081ed79147a71af7ea66bad5ffbf27a31850a22b4d3eb1bcc258eb73` |
| `w4-f19a-plain-coof` | calc_on_order_fills | 23 | `b121dd03a9ef0ab41638b369e9b4f98ce5a804fd4db2e3cc80004d9237480e54` |
| `w4-f19a-pooc` | process_orders_on_close | 15 | `370fe3eb8134ef5d9a3166e306e7e30310c88d5fc19fed631dd08d6d1d54044f` |
| `w4-f19a-pooc-coof` | both | 15 | the same file |

What they show: the take-profit closes both lots -- the add filled at the same
open the exit fills at (A; B with the exit placed before the add; G a stop), the
add filled at that open and the limit reached later in the bar (C), the add
filled on a later bar than the exit's placement (F), at a process_orders_on_close
close (A, B, G on the POOC tapes); 50 percent of 2 + 2 lots closes 2 (E, reported
as two rows of 1 from the first lot); an exit that names its entry closes that
entry's lot only (D). Under calc_on_order_fills cells E and F refill their entry
on the recalculations of the entry bar (a cascade outside this rule).

## Bars

`bars.inc` holds the replayed bars, 2025-04-10 00:00 .. 2025-04-11 23:45 UTC,
copied as text from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`, derived from
the corpus 1m feed sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`);
`exec/W4-ENG-POOC-SAMEPASS-scratch/tools/gen_bars_inc.py` wrote it.
