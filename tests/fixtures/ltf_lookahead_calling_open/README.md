# A finer lookahead_on request merges at the calling bar's open (lane W8C-SECURITY)

`request.security(syminfo.tickerid, <tf finer than the chart>, expr,
lookahead = barmerge.lookahead_on)` merges the requested series at the calling
chart bar's time: TradingView reads the requested bar that opens at or before
that instant, the latest such bar. Where the requested timeframe has a bar
opening at the chart bar's time, that is the chart bar's first intrabar (the
rule round 7 pinned on BINANCE:BTCUSDT 1D, `tests/test_ltf_lookahead_first_bucket.cpp`).
OANDA stamps its XAUUSD daily bars at 17:00 ET, inside the 17:00-18:00 break,
one hour before the session each bar carries: no requested bar opens at the
stamp, and TradingView reads the previous chart bar's last requested bar.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every export runs the same synthetic read-out probe
(`strategy.pine` sha256 `c95c40241a98d6efcb2c3b1c0866e519944ec1175837bd9d862f0d745af4224f`)
on the 1D chart, 2025-04-01 .. 2026-05-01 (the last row runs a second probe
of the same form). The probe reverses a position on every bar; each entry
order's id carries that chart bar's read-out:

    on|on1|off|gaps_on|close1

the minutes from the chart bar's `time` to the requested 15m bar each site
merged -- `time` with lookahead_on, `time[1]` with lookahead_on, `time` with
lookahead_off, `time` with gaps_on + lookahead_on -- and `close[1]` with
lookahead_on. An entry fills on the next chart bar, so a row's read-out is the
chart bar before its entry time.

| tape | symbol | trades | TradingView reads | tv_trades.csv sha256 |
|---|---|---:|---|---|
| `w8c-ltfon-xau1d` | OANDA:XAUUSD | 278 | `on` = -15 on 211 bars (16:45 ET, the previous bar's last), -2895 on the Sunday stamps (Friday 16:45 ET), -165 / -4335 after early closes and long weekends; `on1` one bar earlier; `off` = the chart bar's last 15m bar (+1425, 16:45 ET, on a full session); `gaps_on` = `on` | `c5e9c95da6b47511a4d6442c0afb35782fb42089c4712bedca4323c6e2c47e9b` |
| `w8c-ltfon-eth1d` | BINANCE:ETHUSDT.P | 395 | `on` = 0 on every bar (24x7: a 15m bar opens at 00:00 UTC) | `744c26845c67093ae86d096d435337763219817fd5929dbe6d38a3c43a8d933a` |
| `w8c-ltfon-eur1d` | OANDA:EURUSD | 280 | `on` = 0 on every bar (1700-1700: a 15m bar opens at the 17:00 ET stamp) | `d6453e3711c141fba7263865d9752c813a9bd5c897fef5cc4086ab2029a5406f` |
| `w8c-ltfon-aapl1d` | NASDAQ:AAPL | 271 | `on` = 0 on every bar (a 15m bar opens at 09:30 ET) | `f27f91b1196b1a426adb27c9142854d08e2c5f058fdae1b4c4148bcdecb12274` |
| `w8c-ltfon-tfs-xau1d` | OANDA:XAUUSD | 278 | the second probe (`strategy.pine` sha256 `21f59fe6a3a68991a5560fd514d34862ec8d18d1741e4a17ae2f2216cb0aa688`), id `t240\|t60\|t5\|t240off\|c240`: `t60` = -60 (16:00 ET; Friday's -2940 on the Sunday stamps), `t5` = -5 (16:55 ET), `t240` = 0 on every bar -- TradingView's 4h grid opens at the 17:00 ET stamp itself (`t240off` = +1200, 13:00 ET, the bar's last 4h bar) | `0c37d44765c66671a4072df419d36c25c686f09020683d9b8ad016cfd5bf7de4` |
| `w8c-ltfon-3m-xau1d` | OANDA:XAUUSD | 278 | a third probe (`strategy.pine` sha256 `73a2714dc5891bbbce04c46e97d2c2721fe83cbe7577058f15743aa96c5e011d`), id `tOn\|cOn\|cOn1\|tOff\|cOff` for "3": `tOn` = -3 (16:57 ET) on 215 bars. On the Black Friday bar (2025-11-27 17:00 ET stamp) it reads Thanksgiving's 21:57Z bucket, which holds the 21:59 minute alone (close 4158.8), and on 2025-07-08 the 20:57Z bucket holding two minutes (close 3301.72): a last bucket no completion rule closed is still the bar at or before the stamp. `tests/test_split_feed_partial_bucket.cpp` case (e) pins it | `bd8da3b8dd512914c54a08c4f56828afb5ae6c2a400f24c88c144caf1e275b95` |

On the XAUUSD lanes' own TradingView 15m bars, "the latest 15m bar opening at
or before the daily stamp" reproduces all 278 XAUUSD read-outs, `on` and
`close1` both.

`bars.inc` holds, for each symbol, the campaign lane's own TradingView daily
bars (2025-04-01 .. 2025-04-09) and 15m bars (2025-03-31 12:00 .. 2025-04-09
12:00 UTC), copied from the lane feeds by sha256 (named in `bars.inc`):
xauusd-1d `79cdcfb671e5` / xauusd-15 `248086b8b82d`, ethusdtp-1d `61704ec0cdfe`
/ eth 15 `27b62431096e`, eurusd-1d `e95d45d2d141` / eurusd-15 `f945f31a140a`,
aapl-1d `cac03bf7a41e` / aapl-15 `ae2b03d3736f`.

`tests/test_ltf_lookahead_calling_open_tapes.cpp` runs a port of the probe's
five request.security sites over those daily bars with the 15m bars as the
auxiliary feed, and requires every compared chart bar's read-out to be the
tape's; a second port reads the `tfs` tape's "240" and "60" fields the same
way ("5" is finer than those auxiliary bars). The first chart bar is warmup (the auxiliary routing feeds no bar
before the chart's first bar).
