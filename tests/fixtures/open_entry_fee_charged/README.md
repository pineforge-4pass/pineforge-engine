# Open entry fees are out of strategy.netprofit and strategy.equity (lane W8E-EXITS)

TradingView charges an entry's commission when the entry fills. While lots
are open, the entry fees they have paid (the share still on each lot after a
partial close) are already out of `strategy.netprofit`, so out of
`strategy.equity` and of `strategy.netprofit_percent` /
`strategy.openprofit_percent` too; `strategy.openprofit` stays gross of them.
Each rule is pinned by synthetic probes written for this lane; no closed or
scraped strategy is involved.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15,
2025-04-01 .. 2025-04-03, with an initial capital of 100000 and a fixed
default size of 1, and uses fixed `timestamp("UTC", 2025, 4, 1, ...)` cells.

A probe opens a position at 12:00, reads `strategy.openprofit`,
`strategy.equity` and `strategy.netprofit` (and the two percentages) while it
is open, and later encodes each value as the quantity of a one-bar trade: long
when the value is at least zero, of `|value| / 10` lots (`|value| * 10` for
`strategy.netprofit_percent`, `|value| * 1000` for
`strategy.openprofit_percent`) plus 0.00003 against a binary64 floor, each
opened at an hour cell and closed 15 minutes later (OP 15:00, EQ 16:00, NP
17:00, NPP 18:00, OPP 19:00). TradingView floors the quantity to its 0.0001
lot, so a row reads its value to the lot.

`tests/test_open_entry_fee_charged.cpp` replays every tape through the Pine
host under the configuration the generated constructor declares for that
probe, over the corpus 15m bars embedded in `bars.inc`, trading from the bar
before TradingView's first entry (2025-04-01 12:00 UTC), with TradingView's
0.0001 lot as the `qty_step`, and requires each trade the tape closes -- entry
and exit time, side, price in ticks of 0.01, quantity in lots of 0.0001 -- to
be the engine's. It also reads the rule off TradingView's own rows.

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `s5-equity-fee-pct` | 6 | `commission_type = strategy.commission.percent, commission_value = 1.0` | A buys 10 at 1868.83 (a 186.883 entry fee); at 13:00, open profit -2.1 (OP 0.21), equity - capital -188.983 (EQ 18.8983), net profit -186.883 (NP 18.6883), netprofit_percent -0.186883 (NPP 1.8688), openprofit_percent -0.0021039 = -2.1 / 99813.117 (OPP 2.1039) | `75c3560ef22d7063c0a3178d2d3d23c20ebbd57a445907b1afa8b0445852fafa` |
| `s5c-equity-nofee-pct` | 4 | `commission_value = 0.0` | the same open profit (OP 0.21) and equity (EQ 0.21); net profit and its percent are zero, so NP and NPP never open; OPP 2.1 = -2.1 / 100000 | `34c2176d48940176fe50b0cf0f02c9237c38cacbfd50becb41f83ee9140617cc` |
| `s2-netprofit-pyramid-partial` | 6 | `commission_value = 1.0, pyramiding = 2` | A buys 10 at 1868.83, B 6 at 1862.52, half of A closes at 1868.62 (row -187.9225 with half of A's fee); at 13:45 net profit -393.115 = -187.9225 - 93.4415 - 111.7512 (the fee left on A's half and all of B's) | `a61dbf29f79f71c8d272ee3dda44521462ec6f47c3c490b6a83048da7a0b4072` |
| `s3-netprofit-cash-order` | 4 | `commission_type = strategy.commission.cash_per_order, commission_value = 25` | net profit -25 (NP 2.5), equity - capital -27.1 (EQ 2.71) | `c1dc06b11981ff860c73af5165b9bc05abba9dd561dcecfd304d6af51c47f2bf` |
| `s4-netprofit-cash-contract` | 4 | `commission_type = strategy.commission.cash_per_contract, commission_value = 3` | net profit -30 (NP 3), equity - capital -32.1 (EQ 3.21) | `c5492281561a34d3f6405fbdd383a4238bdb26e2774a137bac31782f87a28aff` |

The Pine host read the closed trades' net profit only (`BacktestEngine::
net_profit()` / `current_equity()`), so its EQ and NP quantities were those of
the fee-free control on every fee tape. `PineStrategyHost::net_profit()` and
`current_equity()` now charge the open lots' paid entry fees; the engine's own
accessors, the report and the adapter's margin money keep their figures.
