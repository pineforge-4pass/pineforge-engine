# A flat LIMIT entry's affordability at every call (lane W8A-SIGSTATE-1)

TradingView checks a default-quantity LIMIT `strategy.entry` that opens from
flat at every call, its placement and each re-issue, against the calling
bar's close:

- A call whose quantity costs more than the equity there is rejected, and it
  takes the resting order of that id with it, however affordable the order's
  own level is.
- A call it admitted is not re-checked on later bars.
- The fill is still refused when its own notional exceeds the equity.

The engine left LIMIT entries out of its placement check. A re-issue that
TradingView rejected therefore kept its resting order alive, and that order
filled later.

The scraped `algotorma-algo-torma-orb-strategy` (BINANCE:BTCUSDT 15, strong)
re-issues its breakout limit on every bar while it is armed. Its last
re-issue was at 2026-01-14 23:45, on a close of 96951.78 over an equity of
96846.71. TradingView rejected it; the engine filled the resting order on
01-15 03:15, the probe's one engine-only trade.

`w8a-btc15-limafford` is one `lab tv --no-note` export (maintainers' private pineforge-workflow,
channel `ws-report-v1`, `rangeProof` covered), byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` and
`meta.json`. It runs on BINANCE:BTCUSDT 15 with an equity of 95000, a default
fixed quantity of 1, margin 100 % and no commission, and every cell starts
flat:

| cell | orders | TradingView |
|---|---|---|
| E | sell limit 95250, placed once on an affordable close | never fills: its level costs more than the equity |
| A | buy limit 94640, re-issued every bar 04-25 18:00 .. 19:45; the last calls are on closes above the equity | never fills, though the market reaches it at 21:45 |
| C | buy limit 94680, placed once on an affordable close; later closes are above the equity, with no re-issue | fills 04-29 17:00 |
| B | control: every call affordable | fills 04-29 23:00 |

`tv_trades.csv` sha256
`15057f1d604c7fb9c18699742d1f16186efb5f3d2179f9a392a74511eeb52c30` (2 trades),
`strategy.pine` sha256
`0c897377d0db1c8a7b3a631f597a47fba775138fb774b5b9193937ebef65ac6d`.

`tests/test_limit_entry_affordability_tape.cpp` replays the script through the
Pine adapter under the configuration its generated constructor declares. It
runs over the btcusdt-15 lane's 15m bars in `bars.inc` (2025-04-25 00:00 ..
04-30 12:00 UTC), which cover every cell's placement. It requires each trade
to be the engine's: the entry signal, the entry and exit times, the side, the
price in ticks of 0.01 and the quantity in lots of 0.00001. It also reads the
rule off TradingView's rows and the bars.

The tape does not pin these cases: reversal LIMIT entries, explicit-qty or
cash LIMIT entries, and default percent_of_equity LIMIT entries.
