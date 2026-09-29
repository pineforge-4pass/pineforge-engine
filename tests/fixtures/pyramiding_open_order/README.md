# The pyramiding count of pending entries (lane W8A-SIGSTATE-1)

**R-B, the pyramiding count.** `strategy.entry` checks the `pyramiding` cap
when it is called. With a position open on the same side, the call is ignored
when the open trades plus the pending same-side MARKET entries of other ids
reach the cap. A pending LIMIT entry never counts, and a LIMIT entry the call
admitted fills however many trades are open by then, so the cap can be
exceeded. The engine counted every pending same-side entry at the call and,
under a non-fixed default quantity, refused a priced entry at its fill once
the open trades reached the cap.

The scraped `job-2712-3commas-3commas-gold-vault-long` (BINANCE:BTCUSDT and
BINANCE:ETHUSDT.P 1D) rests its safety-order limits at the cap: TradingView
books five lots at `pyramiding=4` where the engine refused the fifth.

`w8a-dca-open` is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json`, `meta.json`. It runs on
BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2026-05-01, with `pyramiding=3`, a cash
default of 100 and an explicit `qty` on every call, in fixed
`timestamp("UTC", ...)` cells on 2025-04-15 .. 04-16; a buy limit set above
the close is marketable at the next open. Each cell ends with
`strategy.cancel_all()` and `strategy.close_all()` about two hours later.

| cell | placement (call order) | TradingView |
|---|---|---|
| E1, flat | limits B +1.0 %, A +3.0 %, C +2.0 % | B, C, A at the open |
| E2a | limit L1, then `close_all` | the close first; L1 opens after it and is held to the cleanup |
| E2b (control) | `close_all`, then limit L2 | the same |
| E3, one lot open | limits P1, P2, P3 | all three fill: four lots |
| E4a (control), two lots open | market GM, then limit GL | GL ignored: the pending market entry counts |
| E4b, two lots open | limit HL, then market HM | HM, then HL: four lots |

`tv_trades.csv` sha256
`17289e79b7821acb80c19adb056cfd27d15e62fd32f46435bdeef1060e17117c` (18 trades),
`strategy.pine` sha256
`ef7d1ef1d20fb0b9903fb4a9ec959cbed5f74f6b1738d33894c36747e43fbbae`.

`tests/test_pyramiding_open_order_tape.cpp` replays the script through the
Pine adapter under the configuration its generated constructor declares, over
the corpus 15m bars of `bars.inc` (2025-04-14 00:00 .. 04-16 12:00 UTC). It
requires every trade outside E2a to be the engine's -- entry id, entry and
exit time, side, price in ticks of 0.01, quantity in lots of 0.0001 -- in any
order, and reads R-B off TradingView's own rows.

The tape also pins a second TradingView rule the engine does not model yet:
at one opening price TradingView fills every MARKET order first, entries and
exits alike (E2a's `close_all`, E4b's HM), then the buy LIMIT entries the open
has already reached, lowest limit first (E1's B, C, A); the engine fills them
in placement order. That rule belongs to the same-point fill-order family of
lanes W6-ENG-FILL-ORDER and W6B-ENG-PAIRS; the lane's report carries it.

Not covered by the tape: stop entries, the sell side, and `strategy.order`.
