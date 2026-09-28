# How TradingView reads a session argument's clocks (lane W11-ENG-TIME-COLOR)

`time(timeframe, session, timezone)` and `time_close(...)` read each
`HHMM-HHMM` window of their session argument as minutes after a day's
midnight, `HH * 60 + MM`, and check neither part: `2400` is the day's end,
`2430` is 00:30 on the next day and `0060` is 01:00. An end at or before the
start is on the next day, which covers a wrapping window (`1800-1700`) and
TradingView's 24-hour spelling (`1700-1700`, `0000-0000`). A time of day is in
the window when it, or the same time on the next day, falls in `[start, end)`.
Every four-digit form below was accepted: none of them is an error on
TradingView.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json` and `meta.json`. Each probe alternates two
positions and closes one on every chart bar, so every bar from the second on
has an exit, filled at the next bar's open. The exit's comment spells what
the script read on that bar: `n` for na, otherwise the minutes from the
returned time to the bar's open (for `time_close`, from the bar's open to
the returned time). `tests/exit_comment_tape.hpp` reads them.

`w11-sess2400-*` runs one probe (`w11-sess2400-xau15` has the same source
with `default_qty_value=1`, since OANDA:XAUUSD takes no 0.001 lot). It spells
`a,b,c,d,e,f,g,k|h,i,j`:

| field | call |
|---|---|
| a | `time(timeframe.period, "0000-2400")` |
| b | `time(timeframe.period, "1700-2400")` |
| c | `time(timeframe.period, "2045-2400", "Asia/Tokyo")` |
| d | `time("D", "0000-2400")` |
| e | `time(timeframe.period, "0000-2400:23456")` |
| f | `time_close(timeframe.period, "1700-2400")` |
| g | `time(timeframe.period, "2300-2400,0000-0100")` |
| k | `time(timeframe.period, "2000-2400", "America/New_York")` |
| h, i, j | controls: `"0000-0000"`, `"0000-2359"`, `"1700-0000"` |

What TradingView reads on BINANCE:BTCUSDT 15 (a week spanning the US DST
switch on 2025-03-09): `a` is in on every bar, and `b` from 17:00 to 23:45. `c` is
in from 11:45Z to 14:45Z (20:45 to 23:45 JST). `g` is in from 23:00 to 00:45,
and `k` from 20:00 to 23:45 New York time on either side of the switch. `e`
admits weekdays only. `j` equals `b`. On the 1D chart the bar's 00:00 UTC
open is what each window is tested at. On OANDA:XAUUSD 15 the windows read
the exchange's New York clock.

`w11-edge-*` each spell `time(timeframe.period, <form>)` alone: the form is
the slug's eight digits.

| tape | form | in session on a UTC day |
|---|---|---|
| `w11-edge-24000100-btc15` | `2400-0100` | 00:00-00:45 |
| `w11-edge-24300100-btc15` | `2430-0100` | 00:30-00:45 |
| `w11-edge-00000060-btc15` | `0000-0060` | 00:00-00:45 |
| `w11-edge-23302430-btc15` | `2330-2430` | 23:30-23:45 and 00:00-00:15 |
| `w11-edge-17002500-btc15` | `1700-2500` | 17:00-23:45 and 00:00-00:45 |
| `w11-edge-00002401-btc15`, `-00002430-`, `-00002500-`, `-00002360-`, `-00009959-`, `-24002400-` | `0000-2401`, `0000-2430`, `0000-2500`, `0000-2360`, `0000-9959`, `2400-2400` | every bar |

`tests/test_session_predicates.cpp` (`test_session_2400_tapes`,
`test_session_clock_edge_tapes`) replays every reading through `pine_time` /
`pine_time_close` as generated code calls them. It compares minutes wherever
the reading is this clock's. Two readings of where a D period opens under a
session argument differ from TradingView, and the lane reports them as its
own findings. There the test compares only whether the bar is in the window:
`d` on the New York chart, where TradingView keys the day on
`syminfo.timezone` and the engine on UTC, and `k` on the 1D chart, where
TradingView answers the bar's own time and the engine the New York day's
midnight.

## Which days a session admits

A day list is taken as written. With no list, one window admits every day and
several windows admit Monday to Friday only, whether or not one of them opens
at 00:00. A list, given or implied, filters each window by its session day.
That is the day the window starts, except for a window that ends at or before
its start (past midnight), which belongs to the day it ends:

- `1700-1700:23456` runs from Sunday 17:00 to Friday 16:45.
- `1800-0200:23456` and `2330-0030:23456` admit Sunday evening and not Friday
  evening.
- `2330-2430:23456` keeps Friday's 00:00-00:30 tail on Saturday and drops
  Sunday's on Monday.
- `0000-0000:23456` is the calendar's weekdays.

`w11-sessmask-btc15`, `w11-sessmask2-btc15` and `w11-sessmask3-btc15` spell
these sessions on BINANCE:BTCUSDT 15, Thursday to the next Wednesday, weekend
included:

| tape | sessions |
|---|---|
| `w11-sessmask-btc15` | `0000-0100,1200-1300`, `2300-2400,0000-0100:1234567`, `1200-1300`, `0000-0100,1200-1300:17`, `0000-1200,1200-2400`, `0000-0100:1234567`, `0000-0100,0000-0100` |
| `w11-sessmask2-btc15` | `0930-1130,1300-1500` (in UTC, then in New York), `2330-2430,1200-1300`, `1700-2500,0900-1000`, `0930-1130`, `1800-0200,1200-1300` |
| `w11-sessmask3-btc15` | `0000-0000:23456`, `1700-1700:23456`, `1800-0200:23456`, `0900-1700:23456`, `2330-0030:23456`, `2330-2430:23456` |

Field `g` of `w11-sess2400-*` (`2300-2400,0000-0100`) reads the same way:
Sunday 23:00 and Saturday 00:00 are out. `test_session_day_list_tapes`
replays the three tapes, and `tests/test_session_ismarket_tape.cpp` holds the
effect on `session.ismarket`'s predicate for the `:23456` session strings.

| tape | chart | range | trades | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|
| `w11-sess2400-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-12 | 672 | `1ff7e4c52cc6d6613d423df49524bbed9e37bc43416f87bdc3fe0573cbfda2de` | `e957446611c8` |
| `w11-sess2400-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01 .. 2025-07-01 | 181 | `8a66f3a67cdb58eeaa3d6132dc1fb41d5cf6c8b9d7953f5296c8e4e615e04231` | `e957446611c8` |
| `w11-sess2400-xau15` | OANDA:XAUUSD 15 | 2025-03-05 .. 2025-03-12 | 464 | `fe968cc0781afde80b85de132dcfefa7e045dd4380f18c9c33c896a96a6772d9` | `19ed4a005d4c` |
| `w11-sessmask-btc15` | BINANCE:BTCUSDT 15 | 2025-03-06 .. 2025-03-12 | 576 | `4a22789e9214f932250b23502780918eae03d7a68d6b6ddc5c4cd5eb5c544e66` | `3b3c4ceb94e3` |
| `w11-sessmask2-btc15` | BINANCE:BTCUSDT 15 | 2025-03-06 .. 2025-03-12 | 576 | `868a0994a450a57bbd1615a5f992f6b5b2ba285dca3010fafc1bbdfb77037707` | `2394e393b1d3` |
| `w11-sessmask3-btc15` | BINANCE:BTCUSDT 15 | 2025-03-06 .. 2025-03-12 | 576 | `82ae8d9903f35c112d604597bd0507c71986d70ba1b0eb91e7d9ccf7fb70e2c9` | `eae1064366af` |
| `w11-edge-24000100-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `affb767b42f776c79f2e7a192e0df39b372b9e62d71a842f33231ac27925167d` | `035c48ffcf32` |
| `w11-edge-24300100-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `adef478120bd7fd74b410d86d3fb4f783b6b963e5f7b2952594401899013ac29` | `988aa2c18cd0` |
| `w11-edge-00000060-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `affb767b42f776c79f2e7a192e0df39b372b9e62d71a842f33231ac27925167d` | `fe560a3f2b26` |
| `w11-edge-23302430-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `a31842e10520f0062f4a91c3bcc9d905259190efeeeef35af7b6b7859dac8e39` | `2b4f5437fb1c` |
| `w11-edge-17002500-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `08a948ac002585f67a2e7663bc393badedf687372592355194981e66f90ff7a2` | `715e64320a5f` |
| `w11-edge-00002401-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `36f413506a42f9152951d7db1c4fce9d28d482da8c20640f2ca8323a74a2cb48` | `7ebeae165e08` |
| `w11-edge-00002430-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `36f413506a42f9152951d7db1c4fce9d28d482da8c20640f2ca8323a74a2cb48` | `71aa8fb822fb` |
| `w11-edge-00002500-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `36f413506a42f9152951d7db1c4fce9d28d482da8c20640f2ca8323a74a2cb48` | `d51ae093036f` |
| `w11-edge-00002360-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `36f413506a42f9152951d7db1c4fce9d28d482da8c20640f2ca8323a74a2cb48` | `9a224e88868e` |
| `w11-edge-00009959-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `36f413506a42f9152951d7db1c4fce9d28d482da8c20640f2ca8323a74a2cb48` | `fff727e56f5a` |
| `w11-edge-24002400-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-07 | 192 | `36f413506a42f9152951d7db1c4fce9d28d482da8c20640f2ca8323a74a2cb48` | `6076fea802a8` |
