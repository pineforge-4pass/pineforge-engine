# Drawing-rollback tape (R5 lane K-DRAWSNAP)

TradingView's own answer to whether a `calc_on_order_fills` recalculation's
drawing edits survive it. They do not: the drawings roll back with the
script's `var` variables. `tests/test_coof_drawing_rollback_tape.cpp` checks the
tape and replays its window through the Pine host; the engine's
generated-state checkpoint therefore has to hold the drawing arenas exactly,
which lane K-DRAWSNAP made cheap (`include/pineforge/drawing.hpp`).

`coof-drawing-rollback/` is one `lab tv --no-note` export by lane CG-POPFIX
(pineforge-workflow, channel `ws-report-v1`, `rangeProof: covered`), copied
byte-identical from `exec/CG-POPFIX-scratch/tv/coof-drawing-rollback/out/`:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.
The tape's sha256 is `metrics.json` `tvTradesCsvHash`
(`acf0087f1bb17679aab9cac097dc82ecb75a0731df1914c10795f36bff0a82cd`).

| probe | chart | window | trades | TradingView |
|---|---|---|---|---|
| `coof-drawing-rollback` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-04-03 | 41 | `x2 == n` on 82/82 order comments, `m > n` on 81 |

The probe bumps a `var line`'s x2 on every execution, counts executions in a
`var int n` (rolled back by a recalculation) and a `varip int m` (never rolled
back), deletes and re-creates one label per execution, and spells all of them
into its order comments (`x2=5 n=5 m=5 labels=1`). x2 equal to n while m runs
ahead means every recalculation ran and its line edit rolled back.

`bars.inc` holds the window's 192 15m bars, resampled from the corpus 1m feed
(`corpus/data/ohlcv_ETH-USDT-USDT_1m.csv`, sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`) with
`scripts/derive_corpus_feeds.py`'s rule (`exec/K-DRAWSNAP-scratch/tape/gen_inc.py`).
Every entry and exit price of the tape is a bar open of that feed.
