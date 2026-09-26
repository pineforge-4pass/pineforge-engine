

## PAR-MARGIN-2: the M7 shapes PAR-MARGIN left unmeasured (R5 lane PAR-MARGIN-2)

Twelve more `lab tv` exports on BINANCE:ETHUSDT.P 15m (same channel and
layout), each the lane's own synthetic script: one leveraged entry (or an entry
and an add) sized `K x strategy.equity / close` on a 0.001 grid at a signal
bar, closed 3 bars (4 for the adds) after the first open trade's entry bar.
`tests/test_adapter_margin_schedule_differential.cpp` (section "M7 shapes")
replays every one FROM ITS OWN `strategy.pine` (parsed, not restated), and
the adapter books TradingView's rows on all of them:

- `pm2-m7-lim-*`: a limit opening (margin 5, `limit = math.round(close * 0.997, 2)`)
  filled on the next bar on its way down, whose low then breaches: the call on
  that bar at its LOW -- three high-first bars and a low-first one (0402-1945).
- `pm2-m7-slim-*` and `pm2-m7-s1lim-*`: a SHORT limit opening
  (`limit = math.round(close * 1.003, 2)`) filled on the next bar on its way UP,
  whose high then breaches, leveraged (margin 5) and at full margin (1 x): the
  call on that bar at its HIGH -- three low-first bars and a high-first one
  (0509-1100).

`pm2_bars.inc` holds each event's bars from the corpus feed
`corpus/data/ohlcv_ETH-USDT-USDT_1m.csv` (15m bars aggregated from it as the
derived feed does; the magnified scripts' one-minute bars as they are).

| probe | window | TradingView's margin calls | tvTradesCsvHash |
|---|---|---|---|
| `pm2-m7-lim-0402-1945` | 2025-03-30 .. 2025-04-05 | 4.516 @1881 | `c8d52c248bfae6ad` |
| `pm2-m7-lim-0402-2000` | 2025-03-30 .. 2025-04-05 | 3.7668 @1872.48 | `c50706a1de9fa157` |
| `pm2-m7-lim-0801-0030` | 2025-07-29 .. 2025-08-04 | 2.22 @3613.67 | `07b081a7560f112c` |
| `pm2-m7-lim-1201-0215` | 2025-11-28 .. 2025-12-04 | 1.7024 @2824.6 | `8dbe22c6e8867a97` |
| `pm2-m7-s1lim-0402-1330` | 2025-03-30 .. 2025-04-05 | 0.0152 @1892.7 | `d633d75493717e91` |
| `pm2-m7-s1lim-0509-1100` | 2025-05-06 .. 2025-05-12 | 0.0132 @2375.39 | `5f62b5db446e583a` |
| `pm2-m7-s1lim-0709-1930` | 2025-07-06 .. 2025-07-12 | 0.0144 @2795.73 | `8d7299c2d5e6ae4f` |
| `pm2-m7-s1lim-1001-0830` | 2025-09-28 .. 2025-10-04 | 0.0096 @4329 | `8981c3291ddf2d14` |
| `pm2-m7-slim-0402-1330` | 2025-03-30 .. 2025-04-05 | 3.2528 @1892.7 | `1d5f09118992a3fd` |
| `pm2-m7-slim-0509-1100` | 2025-05-06 .. 2025-05-12 | 2.6232 @2375.39 | `8b448336731119f7` |
| `pm2-m7-slim-0709-1930` | 2025-07-06 .. 2025-07-12 | 2.5484 @2795.73 | `02f292a474aef979` |
| `pm2-m7-slim-1001-0830` | 2025-09-28 .. 2025-10-04 | 1.5688 @4329 | `320a3e2199cfaa91` |
