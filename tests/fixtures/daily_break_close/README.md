# A calendar chart bar's close (lane W11-ENG-TIME-COLOR)

OANDA stamps its XAUUSD daily bars at 17:00 ET. That is inside the 17:00 to
18:00 break of the 1800-1700 session, an hour before the session each bar
carries. TradingView's `time_close` on a D, W or M chart is the chart bar's
period's last traded close, to the millisecond, whatever time of day the
bar's stamp reads:

- OANDA:XAUUSD 1D: the next 17:00 ET, on all 256 bars of 2025. The daily bar
  of an early-close day keeps 17:00.
- OANDA:XAUUSD W: Friday 17:00 ET (120 hours after the Sunday stamp). The
  week of 2024-11-29 ends at 14:45 ET.
- OANDA:XAUUSD 1M: the month's last traded 17:00 ET. The month ending on
  2024-11-29 ends at 14:45 ET.
- OANDA:EURUSD 1D (1700-1700): the next 17:00 ET.
- NASDAQ:AAPL 1D (0930-1600): 16:00 ET, and 13:00 ET on 2025-07-03.
- BINANCE:BTCUSDT 1D: the next 00:00 UTC.

The engine models no exchange holidays or early closes. The three early
closes above are pinned as readings TradingView closes earlier than the
engine.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`),
byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json` and `meta.json`. Where `rangeProof` reads
`narrower-than-requested`, TradingView's first daily, weekly or monthly bar
sits after the requested `from` (a 17:00 ET or 13:30Z stamp) or its last
before `to`. Each reading is one bar's own, so the window does not enter
it. The probes alternate two positions and close one on every chart bar.
Each exit's comment spells what the script read on the bar before its fill
(`tests/exit_comment_tape.hpp`).

`w11-tclose3-*` spell, in milliseconds, `time_close - time`, then
`time_close("D") - time("D")`, `time_close("D") - time` and
`time_close("W") - time("W")`. `tests/test_engine_trade_accessors.cpp`
(`test_chart_time_close_tapes`) replays the first on each lane's symbol
facts (session, timezone) through `PineStrategyHost::time_close()`.

| tape | chart | range | trades | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `w11-tclose3-xau1d` | OANDA:XAUUSD 1D | 2025-01-01 .. 2025-12-31 | 257 | covered | `5ac957f6317e846c7f282569f2a9598235e4fc84b37d2f34c98c6db8458219a0` | `9ec382eff048` |
| `w11-tclose3-btc1d` | BINANCE:BTCUSDT 1D | 2025-03-01 .. 2025-04-01 | 31 | covered | `4fc6f280801cc686a7f797743f286b0300a1863dacfb2aaded6852a9dc3ee3bc` | `9ec382eff048` |
| `w11-tclose3-eur1d` | OANDA:EURUSD 1D | 2025-03-01 .. 2025-04-15 | 31 | narrower-than-requested | `dfd065395069a1b1d38639004c33327e7c5ab12eb10340206793de4f42b58652` | `9ec382eff048` |
| `w11-tclose3-aapl1d` | NASDAQ:AAPL 1D | 2025-06-01 .. 2025-08-01 | 41 | narrower-than-requested | `db9f99f159ba6675419ef1d55a564a88ea705cb50bcb1e365cbbe24fa986a42d` | `9ec382eff048` |
| `w11-tclose3-xauw` | OANDA:XAUUSD W | 2024-06-01 .. 2025-06-01 | 51 | narrower-than-requested | `35e30f134d1ac2cc2082ba5f05c2fd5707291ac9df922b3f3eab61afd5fae6d5` | `9ec382eff048` |
| `w11-tclose3-xaum` | OANDA:XAUUSD 1M | 2022-01-01 .. 2025-06-01 | 40 | narrower-than-requested | `3e6be4783cd39edf0d4946931eb58c1d1416d5d4289d5d693b0eb3feb6f7e8be` | `9ec382eff048` |

## What a request of another symbol reads there

`w11-tclose-xau1d` and `w11-tclose-la-xau1d` hold requests of TVC:DXY on
the OANDA:XAUUSD 1D chart. They spell the minutes from the requested bar's
`time` and `time_close` to the chart bar's `time`.

- A lookahead-off daily request reads the DXY bar that closes at 19:00 ET on
  the label's own day (`-120` for its `time_close`, 18 hours before the chart
  bar closes). On a Sunday label it reads Friday's bar. The rule is the one
  every chart has: the last requested bar closed by the chart bar's close,
  the next 17:00 ET.
- A lookahead-off hourly request reads the hour that closes at the next
  17:00 ET (`-1440`).
- A lookahead-on hourly request reads the hour that opens at the label
  (`0`), the bar opened by the chart bar's open.
- A lookahead-on daily request reads the DXY bar that opens inside the chart
  bar (`-120` for its `time`, 19:00 ET). That is one bar later than the one
  opened by the label on a Monday to Thursday label, and the same bar on a
  Sunday one.

`tests/test_native_instrument_feed.cpp` (`test_daily_stamp_in_the_break`)
rebuilds this shape on synthetic feeds through a bare kernel host with raw
labels. The kernel reads each label as its calendar interval, the session
that closes at a Monday to Thursday label, and the case pins that generic
reading: 38 of its 40 reads differ from TradingView's, all but the Sunday
labels' lookahead-off daily reads. Lane W11-ENG-TIME-COLOR's kernel rule
(`c6170f57`, a D/W/M label stamped in the break opens the next period) is
not on this tree: the Pine adapter never changes the kernel's generic
mechanics, so TradingView's reading is the adapter's to give.

| tape | chart | range | trades | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `w11-tclose-xau1d` | OANDA:XAUUSD 1D | 2025-03-01 .. 2025-07-01 | 85 | narrower-than-requested | `8209c666ff491ff9f744da0c03440f572c7c24377ba538427fccd59123e8fe24` | `fbccbf8d2870` |
| `w11-tclose-la-xau1d` | OANDA:XAUUSD 1D | 2025-01-06 .. 2025-04-15 | 70 | covered | `2f3ca398e2baf63eada8aea32f2d00f28950e6da5e255710dc7b7c67195b4430` | `a3bc63531615` |
