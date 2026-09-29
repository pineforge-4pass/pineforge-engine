# The bar magnifier on a daily chart (lane XAU-CAL)

TradingView backtests a daily chart that declares `use_bar_magnifier = true`
on the chart's 60-minute intrabars (the help centre's table row for 1D,
`source::tradingview_intrabar_timeframe`) and dates every order by the daily
bar's own stamp. OANDA stamps its XAUUSD daily bars at 17:00 ET -- 22:00 UTC
in EST, 21:00 UTC in EDT -- in the break of the 1800-1700 session, an hour
before the session each bar carries opens.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`) of
the lane's own synthetic script on OANDA:XAUUSD 1D over 2026-02-16 ..
2026-04-01, byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json` and `meta.json`. Every entry comment spells the placing bar's
`time` and `time_close` in UTC (`t=0216-2200|tc=0217-2200` before the
2026-03-08 switch to daylight time, `t=0308-2100|tc=0309-2100` after it).

- `xc-dmag-dip-xau1d`: whenever flat, a buy limit 0.2 % under the close with
  a take-profit 0.4 % over it and a stop 3 % under the entry level. A day
  that dips to the limit and then rallies books both inside the day; one
  that rallies first only enters. The order of the day's extremes decides,
  so the magnifier's intrabars and the daily bar's open/high/low/close rule
  part on some days: TradingView books 20 trades, and 18 with the same
  script declared off (`xc-dmag-dip-off-xau1d`).
- `xc-dmag-xau1d`: whenever flat, a market long with a +-0.4 % bracket around
  the close. No day of the window reaches the two legs in an order the daily
  bar's rule misreads: the tape and its `-off` twin are the same 31 trades.

`bars.inc` holds the bars the replays read: `kXauDaily`, the lane's daily
chart feed (xauusd-1d chart, sha256 `79cdcfb6..`), and `kXauHourly`, the
lane's 1-minute feed (xauusd-1d finer, sha256 `b2389ad2..`) aggregated on the
hour -- the intrabars TradingView walks. The lane's feed rebuilds every one
of its daily bars this way (the first open, the extremes, the last close and
the summed volume of each bar's minutes, stamp to stamp), which is when
`scripts/run_strategy.py` runs a magnifier-declaring daily chart on it
(`_declared_magnifier_plan`, `_daily_bars_rebuilt`).

`tests/test_daily_magnifier_tapes.cpp` replays the four tapes through the
Pine adapter: the `-off` twins on the daily bars, the others on the hourly
bars with the magnifier on and the daily bars installed as the run's daily
feed, whose stamps date the days (the kernel's day-label rule,
`NativeExecutionConsumer::prepare_day_labels`). Every trade -- entry and exit
bar and price, and the entry comment -- is TradingView's. Without the daily
feed every trade of the magnified runs is an hour late and nothing else
differs.

| tape | chart | range | trades | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `xc-dmag-dip-xau1d` | OANDA:XAUUSD 1D | 2026-02-16 .. 2026-04-01 | 20 | covered | `ce53fcc24eee38c0c365bf9856fa500da0f6b8e7d64f3a8ee7b044299f14cbcb` | `46ded9a41d47` |
| `xc-dmag-dip-off-xau1d` | OANDA:XAUUSD 1D | 2026-02-16 .. 2026-04-01 | 18 | covered | `c2e584271d806dec80bbaa39ec12c503b214493fe9ed31affee3e33017b40039` | `83219a930ee4` |
| `xc-dmag-xau1d` | OANDA:XAUUSD 1D | 2026-02-16 .. 2026-04-01 | 31 | covered | `927ce59bf7ed8af8be24d7927c2808fdfe88ba7e2e57c4bcde8b301bb5a9cb41` | `9a85bc045d92` |
| `xc-dmag-off-xau1d` | OANDA:XAUUSD 1D | 2026-02-16 .. 2026-04-01 | 31 | covered | `6cca289f03560a6387eb7a249104537be3ee7a0c0e16129f9865d90310c73434` | `68de1e123068` |
