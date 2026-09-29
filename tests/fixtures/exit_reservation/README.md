# A percentage exit's reservation, as TradingView keeps it (lane TAIL-H)

TradingView reserves an entry's quantity for its `strategy.exit` orders in the
order the exits were created (lane W3-ENG-EXIT-ALLOC, `tests/fixtures/exit_queue`):
a later exit gets only what the earlier ones leave. These tapes pin how a
percentage exit's share is sized inside that queue.

- **PS.** A percentage exit's share, `qty_percent` of the position, floors to
  the symbol's quantity step, but a share that floors to nothing keeps one
  step while the position holds one. 50 % of one 0.01 lot of OANDA:XAUUSD
  reserves the whole lot, as 50 % of one contract does; 50 % of three lots
  reserves one. `strategy.close` with `qty_percent` closes the same share.

Each tape is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json` and `meta.json`. Every probe declares
`initial_capital=100000`, a fixed default quantity of 1, `pyramiding=1`,
`process_orders_on_close=true` and `margin_long=margin_short=1`, and places a
long of a fixed quantity on the bar opening at one UTC instant:

| tape | chart, range | the probe | TradingView |
|---|---|---|---|
| `tailh-d1a` | OANDA:XAUUSD 15, 2025-10-15 .. 12-20 | 0.01 at 2025-10-21 08:45, then `TP1_L` (50 %, limit 4331.72) before `MAIN_L` (stop 4245.365, limit 4389.29) | `TP1_L` holds the lot: its limit exits 0.01 on 2025-12-12; the stop, holding nothing, never fires |
| `tailh-d1b` | the same | 0.03, the same exits | `TP1_L` holds 0.01, `MAIN_L` 0.02: the stop exits 0.02 at 12:00 on the entry day, the limit 0.01 on 12-12 |
| `tailh-d1c` | the same | 0.01, `MAIN_L` before `TP1_L` | the stop holds the lot and exits it at 12:00 |
| `tailh-d1e-01` | OANDA:XAUUSD 15, 2025-10-15 .. 10-25 | 0.01; `strategy.close("L", qty_percent=50)` on the next bar, `strategy.close("L")` an hour later | the first close exits the whole lot |
| `tailh-d1e-03` | the same | 0.03, the same closes | the first close exits 0.01, the second 0.02 |

`xau15_q4_bars.inc` holds OANDA:XAUUSD 15m bars 2025-10-15 00:00 .. 12-19 21:45
UTC from the lab lane xauusd-15's chart feed; `es15_bars.inc` CME_MINI:ES1! 15m
bars 2025-04-01 00:00 .. 04-09 23:45 UTC from the lab lane es1-15's.

`tests/test_exit_reservation_tapes.cpp` replays each tape through the Pine
adapter under the configuration its strategy() declares and requires every
trade the tape closes inside the bars to be the engine's.

| tape | trades | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---:|---|---|
| `tailh-d1a` | 1 | `753cc2d2541ff702a82c06774fbb726e530d989b992ac14c35a6348da9e1d4cb` | `e63e60c78047` |
| `tailh-d1b` | 2 | `306ffa214f8b4d02de9a2eaa06bfa9cfedad1b56a6c2ea91adf025b58db3d892` | `6cefaa8a1243` |
| `tailh-d1c` | 1 | `5da61f747f4cc26f227551d84eb70a6910768d25c229635c495604b422d7d775` | `403819fbb6ac` |
| `tailh-d1e-01` | 1 | `d8f332bf1c7309dd27e34709bd7faa76c1a906ad200801ff87b97c42c18a5edb` | `38d390f3fd60` |
| `tailh-d1e-03` | 2 | `ef9148ec4542a27973ddccd004939db17c759363e931be02fc54f745dbb616de` | `ed1dec8b9976` |
