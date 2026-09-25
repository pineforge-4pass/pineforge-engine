# All-in opening trim tape (R5 wave H, lane H-THIN, E19)

Which excursion does TradingView give the small residual an all-in
commissioned opening books and margin-calls again on its own entry bar?
Default 100 % of equity, 0.02 % commission, 2 ticks slippage, margin 100/100;
four scenarios, each flat at its start and closed by `strategy.close_all`
(`hthin-e19-allin-trim/strategy.pine` names them). TradingView books both
entry-bar margin calls (trades #1 and #4) with the fill alone: favorable 0,
adverse = the fill's loss plus the entry commission (#1:
`-(0.04 + 0.0002 * 1812.35) * 1 = -0.40247`). It does not fold the entry
bar's high or low into them. The Pine host's former owner model (ab9714be's
rule, `tests/test_l10y_full_equity_entry_split.cpp` before R5 lane H-THIN)
gave such a residual the complete entry bar; the kernel sampler books the
fill alone, as TradingView does.

| tape (`lab tv`, ws-report-v1, rangeProof covered) | chart | window | trades | tv_trades.csv sha256 | campaign note |
|---|---|---|---|---|---|
| `hthin-e19-allin-trim` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 6 | `c229f8fd7d1fd6c414db51a032d4c5ac8261bcee35c98989b7d9d168af5eb299` | `tv-tape-hthin-e19-allin-trim-c229f8fd` |

Replayed on the corpus 15m feed through a `PineStrategyHost` running the same
script ($EV/exec/H-THIN-scratch/probes/probe_allin_trim.cpp), scenario A's
residual (qty 1, 1812.35 -> 1812.31) reads favorable 0 / adverse 0.402470 on
the kernel sampler and 7.467530 / 12.082470 on the retired owner model; the
main lot (#2, 33.727846 / 118.842902) and #3 agree with the tape on both.
Scenario C's short trim is a different trade on both engines (the adapter
slices 0.1696 at 1651.81 where TradingView slices 1 at 1646.05) -- a
pre-existing margin-slice divergence, recorded, not an excursion question.
