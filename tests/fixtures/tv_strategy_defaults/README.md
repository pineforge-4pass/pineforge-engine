# strategy() default tapes (lane TV-DEFAULTS)

TradingView changed three Pine v6 `strategy()` defaults around
2026-09-24T20:45Z. For a v6 script that omits them it now uses
`initial_capital = 100000`, `default_qty_type = strategy.percent_of_equity`
and `default_qty_value = 100` -- 100 whatever the type: 100 contracts under
`strategy.fixed`, 100 of the account currency under `strategy.cash` -- where
it used 1000000, `strategy.fixed` and 1. Pine v5 still uses the old three,
and no other `strategy()` default moved (the lane's report holds the whole
matrix: every parameter, v5 and v6, four charts, each omitted, at its old
default, and at a value that moves the tape).

`PineStrategyConfig` keeps 1000000 / fixed / 1 as its member defaults: they
are Pine v5's, and every hand-built source host relies on them. The
translation declares the v6 values: pineforge-codegen's generated constructor
emits all three for a v6 script that omits them (`PINE_V6_STRATEGY_DEFAULTS`),
the same seven tapes being its end-to-end rows
(`tests/test_e2e_strategy_defaults.py` there).

Each directory is one `lab tv` export (channel `ws-report-v1`, `rangeProof`
covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the sha256
of `tv_trades.csv`. The probes are synthetic, written for this lane; no closed
or scraped strategy is involved. Every probe trades the same signal (a
10/30-bar SMA cross, long only, entered from flat and closed on the opposite
cross), so the tapes differ only in how TradingView sized each entry.
Exported on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, with `--no-note`
(no campaign note was recorded).

| tape | trades | `strategy()` declares | first entry (2025-04-01 23:30 UTC+8, 1910) | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `tvd-all-omit-v6-eth15` | 42 | nothing | 52.356 = 100% of 100000 | `30d90f4509ec6d5623b191685d050fca90b53fbe142404f9c146a8fe2989ed19` |
| `tvd-cap-omit-v6-eth15` | 42 | `default_qty_type = strategy.percent_of_equity, default_qty_value = 100` | 52.356 = 100% of 100000 | `30d90f4509ec6d5623b191685d050fca90b53fbe142404f9c146a8fe2989ed19` |
| `tvd-qty-value1-v6-eth15` | 58 | `initial_capital = 1000000, default_qty_value = 1` | 5.2356 = 1% of 1000000 | `830e24358640c613ab4a379c28a991131e77509763c30dbdf6b97e3e4052c287` |
| `tvd-qty-typefixed-v6-eth15` | 58 | `initial_capital = 1000000, default_qty_type = strategy.fixed` | 100 contracts | `7198c515b7852a220af3d467130ab99029304cace3c602e7a8063b4889d6e1e8` |
| `tvd-qty-typecash-v6-eth15` | 58 | `initial_capital = 1000000, default_qty_type = strategy.cash` | 0.0523 = 100 USDT | `9bff4e749ac8250cd601f613739420c46753298912c43c4673c5798d32d6fa56` |
| `tvd-qty-fixed1-v6-eth15` | 58 | `initial_capital = 1000000, default_qty_type = strategy.fixed, default_qty_value = 1` | 1 contract | `df7cc20f01d8a81a2b4f202d17ab280c935d04be2e7a5e772485e4cff206edb8` |
| `tvd-cap-x1m-v6-eth15` | 42 | `default_qty_type = strategy.percent_of_equity, default_qty_value = 100, initial_capital = 1000000` | 523.5602 = 100% of 1000000 | `c86447297abb3b8bdf948da033bf78256e62f74eb4118d359b7c719063926a11` |

The 42-trade tapes enter with 100% of equity: v6's default margin of 100%
refuses the 16 entries whose fill (the next bar's open) is above the close
the order was sized at, which the 58-trade tapes, sized at a fraction of
equity, all fill (checked on the corpus feed: 16 of 16 refused entries open
above the signal close, 0 of 42 filled ones do).

`bars.inc` holds the replayed bars, 2025-03-31 14:00 .. 2025-04-06 00:00 UTC,
copied as text from the corpus 15m chart feed `scripts/derive_corpus_feeds.py`
derives (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`) from the
corpus 1m feed (sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`);
`exec/TV-DEFAULTS-scratch/gen_bars_inc.py` wrote it. Inside them each
42-trade tape closes 6 trades (two refused entries among them) and each
58-trade tape 8. `tests/test_tv_strategy_defaults.cpp` replays every tape
through the Pine adapter under the configuration the generated constructor
declares for that probe, trading from the bar before TradingView's first
entry as `run_strategy.py` does for a corpus tape, with TradingView's 0.0001
lot as the `qty_step`, and requires each of those trades -- entry and exit
time, side, price in ticks of 0.01, quantity in lots of 0.0001 -- to be the
engine's.
