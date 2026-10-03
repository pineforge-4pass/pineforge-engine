# Same-bar pyramiding entry tapes

These independently authored synthetic sources and TradingView exports are public,
self-contained evidence. Every export uses BINANCE:ETHUSDT.P, 15 minutes,
April 1-4, 2025, through `lab tv --no-note`; every `rangeProof` is `covered`.
Only the synthetic strategy titles were renamed before re-export. The close
fixture additionally exports explicit/default X00/X01 variants. No population
source was sent to TradingView. `meta.json` binds each exact source and CSV hash.
Transport account identity fields are omitted from metrics; the CSV path is
relative. Range proof and raw/decoded report digests are retained.

## Scope and evidence limits

The new route requires pyramiding >= 2, fixed default sizing, margin_long ==
margin_short == 0, no commission, slippage, process_orders_on_close,
calc_on_order_fills, OCA or active risk gate, ordinary batch phase, FIFO and no
active intrabar sampler. Same-bar close/close_all commands fall back to the old
path, including entries staged before the close. Pyramiding <= 1 retains main's
old block. This change does not generalize projected closes or margin admission.

The 173 campaign guards never take this route; byte-identical guard and corpus
results are out-of-scope regression controls, not behavioural coverage of the
new route. Its only behavioural evidence is the 16 family lanes of one strategy
and these synthetic tapes. No population identity, symbol, time or price selects
the route.

## Unit coverage

`test_samebar_pyramiding_entries_tapes` embeds the entry and close tape topology
in `tests/samebar_pyramiding_entries_tapes.hpp`. The three entry matrices cover
flat / short 1-2 / long 1-2 and LS, SL, LSL, SLS, LLS, SSL. Explicit and default
pyramiding-7 tapes agree. All 60 pyramiding-7 rows, the 21 already-matching
pyramiding-1 rows, and X00, X01, X00d, X01d are asserted: 85 full-tape cases.
X04 additionally asserts only the flat book after the batch fill bar and the
absence of any trade closed by the final close_all: 86 checks total.

The unit test replays seven constant-price bars (100) to isolate side, quantity,
entry/exit identity, fill times, zero-length trades, flat terminal position and
zero errors/PnL. CSV times are UTC+8; embedded epochs subtract eight hours.
TradingView's close display label maps to the adapter's internal close identity.
This structural test does not claim to replay ETH prices or raw PnL.

## Recorded known-open rows (full tapes not asserted)

- Pyramiding 1: C02, C08, C10, C15, C17, C20, C22, C27, C29 match TradingView only
  under the excluded projected-entry route. Keeping main's old block deliberately
  leaves these nine discrepancies open; their exact TV rows remain recorded.
- Close/entry: X02, X03, X04, X05 remain open. X02/X03/X05 need general projected
  close ordering to retain the surviving qty-1 lot. X04 includes TradingView's
  close-owned long-1 artifact, absent on main. Its full trade list remains open;
  only the flat-after-batch result and absence of a final close_all trade are
  asserted, since both TradingView and main agree on those properties.
- Raw `strategy.order`: the entire p7-explicit-order matrix is an unasserted
  boundary control; baseline discrepancies include C01, C03, C05, C08, C17.
- Process-on-close: the entire p7-explicit-entry-pooc matrix is an unasserted
  timing control. It does not justify widening this route.

The executable records the 13 embedded known-open entry/close full tapes
separately and never substitutes main's divergent values for the TradingView
expectations. X04's partial position check does not assert its artifact lot.

## Export digests

| Fixture | Source SHA-256 | Trade CSV SHA-256 |
| --- | --- | --- |
| `p7-explicit-entry` | `88fe4cd6d22752b33519d43c708f5e0fd17d98240f4e3954c74c0b4e5d31799a` | `bea6dd9796fef1220c589e197049d12155279202dddf580208cfcf5fcc3271ac` |
| `p1-explicit-entry` | `520ae721ea5ffe6ea055499235b0656ee3df3524af67c289866eee7561818a9f` | `cfdf25a693c663d42526cf999fa2153fcce004f58740f45553738af75fdb9bfb` |
| `p7-default-entry` | `b2e309195a0aa2d0b8eaf8fcaa217ecfaef63868db7f207c927de793bc79b3dc` | `bea6dd9796fef1220c589e197049d12155279202dddf580208cfcf5fcc3271ac` |
| `p7-explicit-order` | `0bb32695611f7c4e0cd341c20cc01d802f1cbe46be36e483deb53232af125210` | `be6478557dff6e33d2b444fa8c7002cd2859c46ebeab1bce870288e38753b9d4` |
| `p7-explicit-entry-pooc` | `6c4cf9db7868eb609f3c5ffddab8f84ffdbe87bf9f5c55cc3de75756dddacad1` | `7e916690667c9e421f634354468f9614e506f5f372ba82569e18631cc5e8e92b` |
| `entry-close` | `e8fe0ede40a3fb4726cc9a7518a0520ac9d3cf3e2efcda5afe2bc074e5e81155` | `7b1d99d40d376ec08de02619d68f9e4df49cbf6a0814fd500b4715c9ffa2fcdc` |
