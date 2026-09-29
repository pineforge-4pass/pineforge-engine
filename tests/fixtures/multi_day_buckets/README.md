# TradingView's N-day request.security bars (lane TAIL-A)

`request.security(syminfo.tickerid, "<N>D", expr)` on TradingView groups the
symbol's trading dates N at a time, counting from the first trading date of
every calendar year; the year's last bar ends at the year's end (2025's last
8D bar on BINANCE:ETHUSDT.P runs Dec 27 .. 31, and 2026's first opens Jan 1).
The trading dates are the symbol's session calendar:

- every day on a 24x7 symbol;
- every weekday on OANDA's, a holiday too: EURUSD's and XAUUSD's buckets
  open on 2025-12-25 and 2026-01-01, dates their charts have no bar of;
- the exchange's sessions on a stock or an index: NYSE:F's 8D bar of
  2025-04-17 runs through 04-29 over Good Friday, and NSE's Muhurat session
  (2025-10-21, 13:45 IST) opens a bucket at its own daily bar's time.

The engine's calendar aggregator split an N-day bucket at every day boundary:
an "8D" request read one day.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`),
byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the sha256 of
`tv_trades.csv`. Every export runs the same synthetic read-out probe
(`strategy.pine` sha256
`d9289bd6e3c8736c6fbd8e8b4f1175266e2b8c74c89d9328f7611250ef58a061`) on the 1D
chart, 2024-01-01 .. 2026-05-01. The probe opens a position on every bar and
closes the previous one with process_orders_on_close, so every chart bar but
the first books an exit whose `Signal` spells:

    t2|t3|t5|t8|t13|i8|bar_index

the open time, in epoch days, of the "2D", "3D", "5D", "8D" and "13D" bar the
chart bar reads (lookahead_on), and the "8D" context's `bar_index`: the
requested context starts at the first bucket opening at or after the export's
first bar. The export's own close of the last bar carries no comment.

| export | symbol | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `pf-taila-ndgrid-eth-d` | BINANCE:ETHUSDT.P | 852 | `2892d6e977d3fc0bd3efa701dbc6b0dc377eebb488a6782490e8fc6236a57d67` |
| `pf-taila-ndgrid-eurusd-d` | OANDA:EURUSD | 605 | `e91d26688ea505b291dd192acbb5b1c6ba14408fe66868da7153a4ea08b5dc0d` |
| `pf-taila-ndgrid-xauusd-d` | OANDA:XAUUSD | 602 | `a12e019faefdc0c548e38094799f6ed5fec45a8b3689f040e1c10351b37ef052` |
| `pf-taila-ndgrid-aapl-d` | NASDAQ:AAPL | 584 | `6f49e66cdfe6c59d315f874ebde795aa9f2a0886e3648caa7c04d9d0269195cc` |
| `pf-taila-ndgrid-f-d` | NYSE:F | 584 | `cab66f8b797316541b2af08d4c5c441a85aa47b6ff68789d3edacf04c07e83bd` |
| `pf-taila-ndgrid-nifty-d` | NSE:NIFTY | 578 | `66bd4920b85d147488c1193f8d46cde24a6dbc7bf2e1225b6e297b440323730b` |

The AAPL and F exports report `rangeProof: narrower-than-requested`: NYSE was
closed on 2024-01-01, so TradingView's returned range opens on the first
session, 2024-01-02 14:30 UTC. Every other export is `covered`.

`tests/test_security_multi_day_buckets_tapes.cpp` replays every read-out. A
stock chart holds its sessions from its first bar on, so a year it starts
after a weekday holiday counts that weekday: the stock rows compare from 2025
on, the first year the chart holds whole.
