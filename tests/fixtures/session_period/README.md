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
- **An intraday timeframe** is built on the session too. Its bars open at each
  window's open and every tf after it, the last one cut at the window's close;
  a bar in no window reads na. On BINANCE:BTCUSDT 60,
  `time(timeframe.period, "0930-1600")` reads 09:30 for the 10:00 bar,
  `time("240", "0930-1600")` reads 09:30 and then 13:30 (closing at 16:00),
  and a second window opens its own grid (`0930-1130,1245-1500`: 12:45,
  13:45). On OANDA:XAUUSD 15, `time("240", "1800-1700")` opens at 18:00 ET,
  where `time("240")` keeps the symbol's 17:00 grid. A window that opens on
  the chart's grid reads each chart bar's own time.

- **The chart's own `time_close`** on an intraday chart is the same grid on
  `syminfo.session`: a bar closes one timeframe after its open, the session's
  last bar at the session's close. NASDAQ:AAPL 60 opens 09:30 .. 15:30 ET and
  its 15:30 bar closes at 16:00; AAPL 45 steps 45 minutes from 09:30; NSE:NIFTY
  60 opens 09:15 .. 15:15 IST and its 15:15 bar closes at 15:30 (the
  `w12-ctclose-*` tapes, replayed through the host's chart accessor by
  `tests/test_engine_trade_accessors.cpp`,
  `test_chart_time_close_intraday_session_grid`).

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
| `w12-tfd3-btc60` | BINANCE:BTCUSDT 60 | 2025-01-02 .. 2025-01-08 | 144 | covered | `ad20eecb3305244ac2f4e6e4c110f517bbec7425a1f2b052bf381b1512476d0d` | `88e474766301` |
| `w12-tfd4-btc60` | BINANCE:BTCUSDT 60 | 2025-03-05 .. 2025-03-12 | 168 | covered | `52bf39d49ea7ca8ea6dd40f6876e20b895a9fee0f87c4645e595ff3d887c266f` | `9487e11d54d5` |
| `w12-tfd4-btc15` | BINANCE:BTCUSDT 15 | 2025-03-06 .. 2025-03-09 | 288 | covered | `929342e4514f41b31af2e75c71536714d80a748abb9b3b6371e1b64109272f53` | `9487e11d54d5` |
| `w12-tfd4-aapl15` | NASDAQ:AAPL 15 | 2025-03-03 .. 2025-03-08 | 129 | covered | `5a52d190283c0f1df2e304f0e94f0892f525bc97104522bc5116306ffd4848bf` | `9487e11d54d5` |
| `w12-tfd5-xau15` | OANDA:XAUUSD 15 | 2025-04-01 .. 2025-04-04 | 276 | covered | `278bce8ed01ef9e6db572872a51b70c7e367b26ac1ebbc9699bc8ca829003a6b` | `e6d52af10cb5` |
| `w12-ctclose-aapl60` | NASDAQ:AAPL 60 | 2025-03-03 .. 2025-03-08 | 34 | covered | `d9fa1ec1ef02880138cdf78d8a525105c284bbf1059942567773d1c5eb5d033f` | `1fcaf7f96681` |
| `w12-ctclose-aapl45` | NASDAQ:AAPL 45 | 2025-03-03 .. 2025-03-08 | 44 | covered | `ccaa26b1f9b177b0dde0fbf6557cdb6d2ad2df4c749fc991949bdddd5109c04a` | `1fcaf7f96681` |
| `w12-ctclose-nifty60` | NSE:NIFTY 60 | 2025-03-03 .. 2025-03-08 | 34 | covered | `d068ca353e647ccf9ac7fc4745b43478fcba554d431fc610112b2ac8bf5a6daa` | `1fcaf7f96681` |
