# The bar a session argument builds (lane W12-ENG-TIME)

`time(timeframe, session, timezone)` and `time_close(...)` with a D, W or M
timeframe read the bar TradingView builds on the session argument itself, in
the session's timezone: the `timezone` argument, else `syminfo.timezone`. It
is neither a calendar floor of the chart's zone nor the chart bar's own time.

- **D.** Every session day is one bar, from its first window's open to its
  last window's close. A window's day is the day it starts, or the day it ends
  for a window past midnight (`1800-1700`, `2000-0200`). A day list, given or
  implied (several windows with no list admit Monday to Friday), leaves days
  out. So on OANDA:XAUUSD 15:
  - `time("D", "0000-2400")` opens at New York's midnight, not UTC's;
  - `time("D", "1700-2400", "America/New_York")` opens at 17:00 New York;
  - `time("D", "1800-1700")` opens at 18:00 the day before;
  - a bar in the break of `0930-1130,1300-1500` reads the day that opened at
    09:30, whichever order the windows are written in.

  A bar that no day bar holds reads na. `time_close("D", ...)` is the day
  bar's close: 16:00 for `0930-1600`, 17:00 for `1800-1700`.
- **W and M.** A week or month bar opens at the first session day of its
  Monday-start week or calendar month, by the session day's date. It holds
  every instant until the next one opens, so it is never na. `time("M",
  "0930-1600")` opens on 1 March 09:30 although that is a Saturday: a single
  window with no list trades every day. `time("M", "1800-1700")` opens on the
  last evening of the month before. `time_close("W"/"M", ...)` is the next
  period's open on an intraday chart, and the close of the period's last
  session day on a daily chart.
- **On a D chart** the same bar is read at the chart bar's time, not the chart
  bar itself. `time(timeframe.period, "1700-2400", "America/New_York")` on
  BINANCE:BTCUSDT 1D reads 17:00 New York, two or three hours before the
  00:00 UTC bar. `time(timeframe.period, "0000-2400", "America/New_York")`
  reads New York's midnight.
- **A window spelled past midnight without wrapping** (`2330-2430`,
  `2300-2500`) ends at the week's end when it opens on a Saturday, and only on
  a daily chart. There `time("D", "2330-2430")` reads na on Sunday's 00:00
  bar, where a 60-minute chart reads Saturday's 23:30 open. A wrapping window
  (`2330-0030`) belongs to its end day and is never cut.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`),
byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json` and `meta.json`. The probes alternate two positions and close
one on every chart bar. Each exit's comment spells what the script read on the
bar before its fill (`tests/exit_comment_tape.hpp`): `n` for na, otherwise
the minutes from the reading to the bar's open (for `time_close`, from the
bar's open to the reading). `strategy.pine` lists the fields in order.
`tests/test_session_predicates.cpp` (`test_session_period_tapes`) replays
every reading through `pine_time` / `pine_time_close` as generated code calls
them, on each chart's symbol facts.

| tape | chart | range | trades | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `w12-tfd-btc15` | BINANCE:BTCUSDT 15 | 2025-03-05 .. 2025-03-12 | 672 | covered | `0947158817663d221fcfcf3793f79cef4e8e034d3d47c140a6f385fdc929d9e0` | `d4274598b587` |
| `w12-tfd-xau15` | OANDA:XAUUSD 15 | 2025-03-05 .. 2025-03-12 | 464 | covered | `7e1ff0dd2578fbfa782425729abb3421363c18bf3f351dcc02c2aa82f7b405eb` | `d4274598b587` |
| `w12-tfd-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01 .. 2025-07-01 | 181 | covered | `380581a1db805f06a940cd3816c2b32503d06e700c732dd5c6254ada5aa48461` | `d4274598b587` |
| `w12-tfd-xau1d` | OANDA:XAUUSD 1D | 2025-01-01 .. 2025-07-01 | 127 | covered | `5ec50e97f8a3021f8876971fc84027121099c27ae8487d2ca3528a8c32ffa774` | `d4274598b587` |
| `w12-tfd2-btc15` | BINANCE:BTCUSDT 15 | 2025-02-26 .. 2025-03-05 | 672 | covered | `25d6cbb3a01e6541ae1ddb289ea088a5307de001a7a2545894fdfc05ef0f40ae` | `ca1270d4ad12` |
| `w12-tfd2-xau15` | OANDA:XAUUSD 15 | 2025-02-26 .. 2025-03-05 | 460 | covered | `89b466df42b95ea5a5581bb764259896d70c2ec1e019a8b50e07be262de885fa` | `ca1270d4ad12` |
| `w12-tfd2-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01 .. 2025-07-01 | 181 | covered | `7c8109505d4015d464cf51a5d3a4175c2d683ef405fbcb6471fb74cc6d00d08f` | `ca1270d4ad12` |
| `w12-tfd2-xau1d` | OANDA:XAUUSD 1D | 2025-01-01 .. 2025-07-01 | 127 | covered | `093a5094db0779eb33335a328bd34718e3a90f85f8125c835dcd729ab22fa946` | `ca1270d4ad12` |
| `w12-tfd3-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01 .. 2025-03-01 | 59 | covered | `a18694320197a3d8786a919db921e6330354ab25cf862ee88439093be3618100` | `88e474766301` |
