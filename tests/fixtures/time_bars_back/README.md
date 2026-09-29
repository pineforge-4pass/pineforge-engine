# time() / time_close() reading another bar: TradingView tapes

TradingView's own `time()` / `time_close()` values with `bars_back` and
`timeframe_bars_back` on every bar of six charts (R5 lane TAIL-E). Each
directory is one `lab tv --no-note` export (channel `ws-report-v1`), byte for
byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8, the exporter's
rendering), `metrics.json` and `meta.json`. The probes are synthetic and
public; no scraped or closed strategy is involved.

Two probes, `te_time_bb_chart` (strategy.pine sha256
`f99738effbfc0c7899be5236980ad40e58257efc0578a905046b5d4d346136d5`) and
`te_time_bb_tf` (`201116af10cfbf6fcbe8a8088f00ac10dcc52783e3f12889a3a32bfda5c93d1b`),
spell the values in UTC as `MMdd-HHmm`, joined by `|`: on a flat bar as the
entry's name, on a bar holding the position as the close's comment, so every
chart bar's values are on the tape, its own open first.

- `te_time_bb_chart`: `time`, `time("", -1)`, `time("", "", -1)`,
  `time("", -2)`, `time("", -5)`, `time("", 1)`, `time_close("", -1)`,
  `time_close("", 0)`, and `dayofweek(time("", "", -1))`.
- `te_time_bb_tf`: `time`, `time("60")`, `time("60", -1)`,
  `time("60", 0, -1)`, `time("60", 0, 1)`, `time("D")`, `time("D", -1)`,
  `time("D", 0, -1)`, `time("D", 0, 1)`, `time_close("D", 0, -1)`,
  `time("W", 0, -1)`, `time("M", 0, -1)`, `time("D", "", 1, -1)`.

## Where each tape comes from

| tape | chart | window | rows | tv_trades.csv sha256 | exported (UTC) | rangeProof |
|---|---|---|---|---|---|---|
| `te-time-bb-chart-eth15` | BINANCE:ETHUSDT.P 15 | 2025-04-01..04-03 | 192 | `0999f59d862be6fb584e54b517c4fb138c0894cf709eaefed176bff551da3fa5` | 2026-09-28 23:43:13 | covered |
| `te-time-bb-chart-aapl15` | NASDAQ:AAPL 15 | 2025-04-14..04-23 | 156 | `dc3d755c67c1acb8fa9f271ee635ef22bad7f65055054b469f1fceb01be9a459` | 2026-09-28 23:43:16 | covered |
| `te-time-bb-chart-aapl1d` | NASDAQ:AAPL 1D | 2025-01-01..06-01 | 102 | `644e77d790218dc9ad3198df2a0dc2b728a3f718806aec6ae2d5859e8c19bab5` | 2026-09-28 23:43:19 | narrower-than-requested |
| `te-time-bb-chart-xau15` | OANDA:XAUUSD 15 | 2025-04-14..04-23 | 552 | `9d5115217d5613e5ac6dcc230c2c32e18b11592a4c2bee54ecadd03bca2c25c0` | 2026-09-28 23:43:22 | covered |
| `te-time-bb-chart-eurusd1d` | OANDA:EURUSD 1D | 2025-01-01..06-01 | 106 | `1b82fb484da826efb983263e97875526227b76833a07c834da78c784f89c7d72` | 2026-09-28 23:43:24 | narrower-than-requested |
| `te-time-bb-chart-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01..03-01 | 60 | `60eba0ed00e26dacee413c8112520594c284e629680d367b86bea0bd4f843e93` | 2026-09-28 23:43:27 | covered |
| `te-time-bb-tf-eth15` | BINANCE:ETHUSDT.P 15 | 2025-04-01..04-03 | 192 | `ea67bc36f891ce04bcbe89e6625338430feab67a00b483112565c60ab1c6636a` | 2026-09-28 23:43:30 | covered |
| `te-time-bb-tf-aapl15` | NASDAQ:AAPL 15 | 2025-04-14..04-23 | 156 | `4f601f47aaf6feacf57c192d10f4e023dc99eceacbb95b9db0781b4c59ca4a8d` | 2026-09-28 23:43:33 | covered |
| `te-time-bb-tf-aapl1d` | NASDAQ:AAPL 1D | 2025-01-01..06-01 | 102 | `d82be5c9a5aa98fd8dd92972902600c2a074a00240626393106634d1f8d56ce4` | 2026-09-28 23:43:36 | narrower-than-requested |
| `te-time-bb-tf-xau15` | OANDA:XAUUSD 15 | 2025-04-14..04-23 | 552 | `cbb570ac7ddb507b3beeb78a3223992ee0caf554465e1d2cb22f2bdbcbdc38df` | 2026-09-28 23:43:39 | covered |
| `te-time-bb-tf-eurusd1d` | OANDA:EURUSD 1D | 2025-01-01..06-01 | 106 | `e58f8ea917f7fdc34d6549edda503cc56d7b56299e7e246da252c4eb36064302` | 2026-09-28 23:43:41 | narrower-than-requested |
| `te-time-bb-tf-btc1d` | BINANCE:BTCUSDT 1D | 2025-01-01..03-01 | 60 | `67afec55038f5817bef23c0017d42a383f70a9aec8f271476358c0c6c3903292` | 2026-09-28 23:43:43 | covered |

The daily AAPL and EURUSD exports returned from their first bar, not the
requested 00:00 UTC (AAPL 2025-01-02 14:30, EURUSD 2025-01-01 22:00; hence
`narrower-than-requested`). Every value names its own bar, so the tapes are
read cell by cell, not graded as a trade window.

## What they show

TradingView steps forward -- `bars_back < 0` -- and steps requested bars on
its symbol calendar, closed days included: NASDAQ:AAPL's Thursday 2025-04-17
15:45 ET bar reads its next bar at Monday 04-21 09:30 (Good Friday), the
daily bar of 01-08 the next at 01-10 (the 01-09 closure), of 01-17 at 01-21
(Martin Luther King Day), of 05-23 at 05-27 (Memorial Day); `time("W", 0,
-1)` opens those weeks on their first traded day (Tuesday 01-21, 02-18,
05-27). A past chart bar (`bars_back > 0`) is the chart's history, na before
its first bar. The hour, day, week and month are the symbol's own (the
09:30-anchored hour and day on AAPL). A timeframe finer than the chart's,
stepped, reads the chart bar holding the instant it reaches: `time("60", 0,
-1)` on a daily bar is that bar, `time("60", 0, 1)` the one before.

OANDA:XAUUSD's own history holds no bar in the 17:00-18:00 ET slot, nor on
Good Friday, where TradingView's calendar holds sessions: after 16:45 ET its
next bar is 17:00 ET, after Thursday 04-17's the next day is Friday's. The
host reads the bars the chart's history holds, which equal the calendar's on
every other chart here; `tests/test_time_bars_back_tapes.cpp` names those
cells (`xau_no_bar_slot`) and the two cells of AAPL 1D's first bar where
TradingView read a bar before its range that the run does not hold.
