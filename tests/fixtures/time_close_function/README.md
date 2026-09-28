# The function forms of time_close (lane W12-ENG-TIME)

`time_close("D")`, `time_close("W")` and `time_close("M")` with no session
argument read the symbol's own D, W or M bar. TradingView returns the exact
boundary, never its last millisecond, and where a week or month closes depends
on the chart:

- **D** closes at the session day's close on every chart: the next 17:00 ET
  on OANDA:XAUUSD and OANDA:EURUSD, 16:00 ET on NASDAQ:AAPL, the next 00:00
  UTC on BINANCE:BTCUSDT.
- **W and M on an intraday chart** close where the next period opens.
  OANDA:XAUUSD's week closes at the next Sunday 17:00 ET, 167 hours after the
  one it opens on across the 2025-03-09 switch. A month opens and closes at the
  first traded session of a month, and a Saturday or Sunday trading date is
  not one: XAUUSD's February 2025 runs from Sunday 02-02 17:00 ET to Sunday
  03-02 17:00 ET, and NASDAQ:AAPL's March from Monday 03-03 09:30 ET to Tuesday
  04-01 09:30 ET.
- **W and M on a daily chart** close at the period's last traded close, the
  chart bar's own `time_close` reading: XAUUSD's week on Friday 17:00 ET,
  AAPL's on Friday 16:00 ET.

The engine models no exchange holidays. On `w12-tclose-aapl1d`, 39 W and M
readings of periods that a holiday shortens close earlier, or open later, on
TradingView than on the session's weekdays: the MLK, Presidents Day, Good
Friday and Memorial Day weeks, and January, whose first traded day is the 2nd.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`),
byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json` and `meta.json`. The probe alternates two positions and closes
one on every chart bar. Each exit's comment spells, in milliseconds, what the
script read on the bar before its fill (`tests/exit_comment_tape.hpp`):
`time_close - time`, then for D, W and M the function's close minus its own
open and minus the bar's time. `tests/test_session_predicates.cpp`
(`test_time_close_function_tapes`) replays the D, W and M readings through
`pine_time` / `pine_time_close` as generated code calls them, on each chart's
symbol facts. Where `rangeProof` reads `narrower-than-requested`, AAPL's first
daily bar sits after the requested `from`; each reading is one bar's own.

| tape | chart | range | trades | rangeProof | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---|---:|---|---|---|
| `w12-tclose-btc15` | BINANCE:BTCUSDT 15 | 2025-02-26 .. 2025-03-05 | 672 | covered | `eeadc4f75174eb36ad9def575717cf6d47b2a205f0ba67f3a9475952ada052b8` | `0143b9b1dfc3` |
| `w12-tclose-xau15` | OANDA:XAUUSD 15 | 2025-02-26 .. 2025-03-05 | 460 | covered | `791519350b7cdcd43176a4c28334215ba3cf175e3791d263dd5ba621a3d32bde` | `0143b9b1dfc3` |
| `w12-tclose-aapl15` | NASDAQ:AAPL 15 | 2025-02-24 .. 2025-03-07 | 233 | covered | `db29358aeebd34067f160daf4c08f2e5dd7fa4dfaf5530af4929a2cd3ac90a11` | `0143b9b1dfc3` |
| `w12-tclose-eur15` | OANDA:EURUSD 15 | 2025-02-26 .. 2025-03-05 | 480 | covered | `4d98d365c23878e2cbfaca1622402550a062b719c14647fbf6d802d0503465f8` | `0143b9b1dfc3` |
| `w12-tclose-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01 .. 2025-07-01 | 181 | covered | `6758df6efadb7ed1d66ac37efa716c0cfc7d2db7282304adc70e7f05c7b12ac5` | `0143b9b1dfc3` |
| `w12-tclose-xau1d` | OANDA:XAUUSD 1D | 2025-01-01 .. 2025-07-01 | 127 | covered | `c0cac2d2deaaa5035470bc2f1d7a3315c9b27e500039c08b17a17f6103c48e88` | `0143b9b1dfc3` |
| `w12-tclose-aapl1d` | NASDAQ:AAPL 1D | 2025-01-01 .. 2025-07-01 | 121 | narrower-than-requested | `b2c2fef5eb13f23ceddaefecf224d799e0a7d253c2809819117c4ab68e05c077` | `0143b9b1dfc3` |
