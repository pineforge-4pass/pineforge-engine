# An explicit-quantity market buy's placement admission (R5 lane TAIL-C)

TradingView's own trades for an explicit-quantity MARKET entry placed from flat and sized
all-in at its signal close, `qty = math.floor(strategy.equity / close)`, so the units cost
no more than the equity at the close and less than one unit more than it. One cell per
session, placed on its last bar (15:45 New York): LONG on even sessions, SHORT on odd ones,
filled at the next session's open, flattened by `strategy.close_all()` on that session's
10:30 bar. 10000 of capital, margin 100 both ways, pyramiding 1. The three exports differ
only in `strategy()`.

What they show: TradingView costs a BUY at placement at its signal close's tick plus the
slippage ticks its fill will carry, and a SELL at the close's tick, never lowered by them.
At slippage 2 no long cell ever opens (`-a`: 0 of its long cells over the whole tape,
33 short cells), while without slippage longs open whether or not the entry pays a
commission (`-b`: 26 long cells, `-c`: 22), so the commission is no term of it. The engine
costed the buy at the plain close and opened every all-in long whose slipped fill came in
under that close (7 longs in `-a`'s window on 8988faff).

`tests/test_explicit_market_admission_tapes.cpp` replays each tape through the Pine adapter
over the NYSE:F 15m bars of `../slipped_short/bars.inc` (the f-15 lane chart feed,
2025-04-01 .. 2025-06-30) with TradingView's one-share lot as the `qty_step`, and requires
every trade the tape closes inside its window -- entry and exit time, side, price in ticks
of 0.01, quantity in shares -- to be the engine's. `-a`'s window ends on 2025-05-16: on
2025-05-19 its short takes a second one-unit margin call at the 10:30 bar's close that the
engine does not, and the close_all sized before it overshoots into a one-share long (the
one-unit calls at two slippage ticks and more that `../slipped_short/README.md` leaves
open). On 8988faff the `-a` row fails and the `-b` and `-c` rows pass.

| tape | declares | trades | compared | tv_trades.csv sha256 |
|---|---|---:|---:|---|
| `tailc-b-admission-a-slip2` | slippage 2 | 75 | 10 | `2aea1f1565bc0d02c64cf5fbdb3cbafdac79438347668097dc9475f27188cc89` |
| `tailc-b-admission-b-fee` | commission 0.05 % (control) | 72 | 45 | `d0c0cdb38f8c676bfc3f4b3b58c27c12a78fb7a97cc1b33b94fcca7901fcf0f6` |
| `tailc-b-admission-c-control` | neither (control) | 69 | 43 | `5bef7c74f6b46920da713621054d5312b0e4987a8e1974f5b56715d808cc105c` |

Each directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel
`ws-report-v1`, `rangeProof` covered, NYSE:F 15, 2025-04-01 .. 2025-10-01) of a synthetic
script written for this lane, byte for byte: `strategy.pine`, `tv_trades.csv` (times at
UTC+8), `metrics.json`, `meta.json`.
