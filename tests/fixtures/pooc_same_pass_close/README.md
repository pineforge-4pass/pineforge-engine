# An entry after a same-pass close under process_orders_on_close (lane W8A-SIGSTATE-1)

Under `process_orders_on_close`, a `strategy.close_all()` or a whole-position
`strategy.close(id)` the script issues before a `strategy.entry` in the same
pass settles before that entry. TradingView then treats the entry as follows:

- **Same side as the closed position.** The entry is judged against the
  position the close flattens. Its margin is the resulting position,
  (held + own) x close x pointvalue x margin %, and the entry is dropped when
  that exceeds the equity. The held lots are valued at the close, not at
  their average price.
- **Opposite side.** The entry is not charged for the held lots.
- **Once admitted.** The entry opens after the close as a fresh position, so
  `pyramiding=0` does not block it.
- **Called before the close.** A same-side entry the script issued earlier in
  the pass survives the close too, when the cap admitted it at its call.

The engine charged the entry for its own quantity only, so it admitted the
entries TradingView drops. After a `strategy.close(id)` it dropped the entries
TradingView fills: under `pyramiding=0` as a same-side add, and in every case
when the queued close flattened the position, which cancelled the pass's
same-side market entries whichever call came first.

The scraped `pf-probe-concord-frangestate` (CME_MINI:NQ1! 15, strong) runs a
`close_all` and then a same-side entry in one pass on 22 bars. TradingView
re-enters on the 7 bars where (held + qty) x close x 20 fits the equity and
drops the entry on the other 15; the engine re-entered on all 22.

`w8a-pooc-sameside-closeall` is one `lab tv --no-note` export
(maintainers' private pineforge-workflow, channel `ws-report-v1`, `rangeProof` covered), byte for
byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. It runs on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2026-05-01, with
`process_orders_on_close`, `pyramiding=0`, margin 100/100, 10000 of capital
and an explicit `qty` on every call. Each day opens a position H worth 60 % of
the equity at the 10:00 UTC close. One bar later (16:00 on Saturdays) the
script closes it and then places one entry in the same pass:

| day | close, then entry | TradingView |
|---|---|---|
| Mon, A | `close_all`, long 50 % | dropped: 60 % + 50 % of the equity |
| Tue, B | `close_all`, long 30 % | fills after the close |
| Wed, C | `close_all`, short 50 % | fills: the held long is not charged |
| Thu, D | `close("H")`, long 50 % | dropped |
| Fri, E | `close_all`, short 50 % over a short H | dropped |
| Sat, V | `close_all`, long (equity - held x (avg + close) / 2) / close | fills when the close is below the average: the held lots are valued at the close |
| Sun, D2 | `close("H")`, long 30 % | fills after the close |

`tv_trades.csv` sha256
`d6a435a486a154d25d79374586ad559c3fa48c6650b113aab513548ff1c4f0d6` (589
trades), `strategy.pine` sha256
`575baae2d74db30e68fcd3fe832122d0b8ada22b85de94ea45b024d037a2dc44`.

`w8a-pooc-close-reentry-pyr2` (same channel and chart, `pyramiding=2`, fixed 1,
100000 of capital) enters L at 10:00 UTC each day; at
10:15 it calls `strategy.close("L")` then `strategy.entry("L_add")` on even
days and the two in the other order on odd days, and cleans up at 11:00.
TradingView closes L and opens L_add at the 10:15 close on every day and holds
L_add to the cleanup. `tv_trades.csv` sha256
`5fb317f671ed54e623d800d8362e344e48f74c019a266315d0acc04084601590` (20
trades), `strategy.pine` sha256
`6232b92b77820f326d74d6bce9752595a6c7be9926b14807feebdb64ee2393d9`. The legacy
integration row `test_strategy_close_pooc_keeps_same_bar_market_reentry`
(tests/test_integration_l4d.cpp) is its close-first shape.

`tests/test_pooc_same_pass_close_tape.cpp` replays both scripts through the
Pine adapter under the configuration each generated constructor declares. It
runs over the corpus 15m bars of `bars.inc` (2025-04-01 12:00 .. 04-09 00:00
UTC, every arm once) and requires each trade TradingView books there to be the
engine's: entry id, entry and exit time, side, price in ticks of 0.01, and
quantity in lots of 0.0001. It also reads the rule off TradingView's own rows.
