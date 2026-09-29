# An exit stop born through at a LATER fill of the bar's open (R5 lane TAIL-C)

TradingView's own trades for an exit stop that the calc_on_order_fills recalculation of the
SECOND fill at a bar's open places already through that fill. The first entry is placed at
the hh:30 close and fills at the hh:45 open; the recalculation of that first open fill places
a market entry on the other side, which the same open executes (a newborn reversing the first
fill); the recalculation of that reversal places the exit stop 5 through its average price.
`tests/test_coof_open_newborn_exit_tape.cpp` replays the tape through the Pine adapter over
the corpus 15m chart feed's first four days of the window (`bars.inc`).

Cells on an 8-hour UTC cycle, over the tape's two weeks (where the stop filled on the entry
bar, on the chart bar's O -> first extreme W1 -> second extreme W2 -> C path):

| cell | shape | at W1 (through there) | at the open (W1 is the open) | at its level after W1 (not through at W1) |
|---|---|---:|---:|---:|
| A (h % 8 == 0) | S at the open, reversed there by L; LX stop = average + 5 | 40 | 0 | 2 |
| B (h % 8 == 2) | as A, LX with qty = 1 | 37 | 1 | 4 |
| C (h % 8 == 4) | the mirror: L, reversed by S; SX stop = average - 5 | 37 | 3 | 2 |
| D (h % 8 == 6) | control: L alone at the open; LX stop = average + 5 | 0 | 42 | 0 |

What it shows: the stop born from the later open fill is live from W1, not from the open --
filled at W1's print when still through there, at its own level from W1 on otherwise. After
a single open fill it fills at that open (cell D, as lane W4's `coof_fill_bar_exit` cells
LA/SA). The engine held it for the next bar (the job-1856 DPO/MACD/DI/SMA reversals on
OANDA:EURUSD and BINANCE:ETHUSDT.P 1D, where the first fill is a zero-length short).

| tape | trades | tv_trades.csv sha256 |
|---|---:|---|
| `tailc-1856-open-newborn` | 294 | `fca223ebfddd361aee8d4c5c47f1cf937ae5ef8e0ed426bd71d06ce42198efd8` |

The directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-15), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.

## Bars

`bars.inc` holds the replayed bars, 2025-04-01 00:00 .. 2025-04-05 00:00 UTC, copied as text
from the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`);
`exec/TAIL-C-scratch/tools/gen_bars_inc.py` wrote it.
