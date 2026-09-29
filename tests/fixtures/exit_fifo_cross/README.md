# A qty= exit keeps its own reservation after a FIFO close of its lot (R5 lane TAIL-D)

TradingView reserves a qty= `strategy.exit` on its own entry's quantity and pairs the position it
closes first in, first out: an exit of entry B that fills while A still holds its last unit closes
A's lot, and A's own second exit still fills later, closing B's oldest lot. When A is the reversal a
`strategy.close()` of the held short and `strategy.entry("A")` place on one bar, A waits in the
adapter for that close; its exits reached the book at once, bound to A's cohort, and never filled
once the FIFO close left that cohort no lot (thulashimohanr on OANDA:XAUUSD 15).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-03), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved. Pyramiding 2, fixed cells on
2025-04-01 UTC. `tests/test_exit_fifo_cross_tapes.cpp` replays every tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-m6a` | A (2) at 11:30 with AT1/AT2, B (2) at 12:00 with BT1/BT2; BT1 fills while A holds its last unit (control) | 4 | `b1e69f916897c7c81f13554eb313f298a274304b06000063c51f44151a485887` | covered |
| `td-m6g` | the same after a short S of 2 that the 11:30 bar reverses with close("S"), close("SA") and entry("A"); bracket stops; B called again at 12:30 | 5 | `2acc6c1dc45a9ca43a7f959d02be7e50572cc8ecea8421e0114cbeb329d12c45` | covered |

## Bars

`bars.inc`: 2025-04-01 00:00 .. 2025-04-02 23:45 UTC of the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`).
