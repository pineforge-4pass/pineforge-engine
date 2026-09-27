# `syminfo.mincontract` on every campaign lane (lane RUN-HARNESS, item 3)

What does TradingView report as `syminfo.mincontract` on the charts the campaign
measures? Each directory is one `lab tv --no-note` export (channel
`ws-report-v1`, `rangeProof: covered`) of one synthetic read-out script, byte
for byte: `strategy.pine`, `tv_trades.csv` (times UTC+8), `metrics.json`,
`meta.json`; `metrics.json` `tvTradesCsvHash` is the sha256 of `tv_trades.csv`.
The script (`pf-w2-mincontract-readout`, identical in every directory) enters
`qty = syminfo.mincontract * 1000` on its second bar with the numeric symbol
facts as the entry comment (`mincontract=...|mintick=...|pointvalue=...`) and
closes on its third bar with the identity as the exit comment. Exported by lane
W2-CG-LOWERING-TRIO (finding F06) on 2026-09-27; copied here unchanged from its
scratch (`exec/W2-CG-LOWERING-TRIO-scratch/tv/mincontract/`).

| directory | chart | TradingView's mincontract | entry qty | tv_trades.csv sha256 |
|---|---|---|---:|---|
| `eth-15` | BINANCE:ETHUSDT.P 15 | 0.0001 | 0.1 | `a00911fb2b40e9d5` |
| `eth-1d` | BINANCE:ETHUSDT.P 1D | 0.0001 | 0.1 | `9cc0bf0bf58a278b` |
| `btcusdt-15` | BINANCE:BTCUSDT 15 | 0.00001 | 0.01 | `5378a79fe72878cb` |
| `btcusdt-1d` | BINANCE:BTCUSDT 1D | 0.00001 | 0.01 | `97027921d1ba07ef` |
| `eurusd-15` | OANDA:EURUSD 15 | 0.01 | 10 | `26d494e05e215460` |
| `eurusd-1d` | OANDA:EURUSD 1D | 0.01 | 10 | `894e714320df9c81` |
| `xauusd-15` | OANDA:XAUUSD 15 | 0.01 | 10 | `9cde55c211cf0baf` |
| `xauusd-1d` | OANDA:XAUUSD 1D | 0.01 | 10 | `8cc9342a3ddaa858` |
| `aapl-15` | NASDAQ:AAPL 15 | 1 | 1000 | `3070d874e05d420c` |
| `aapl-1d` | NASDAQ:AAPL 1D | 1 | 1000 | `f9edb75cb84c487a` |
| `f-15` | NYSE:F 15 | 1 | 1000 | `8cda588ebc015955` |
| `f-1d` | NYSE:F 1D | 1 | 1000 | `741050639a260fe2` |
| `nifty-15` | NSE:NIFTY 15 | 1 | 1000 | `fb6a18f36e2f06b8` |
| `nifty-1d` | NSE:NIFTY 1D | 1 | 1000 | `666b9b308c54263e` |
| `es1-15` | CME_MINI:ES1! 15 | 1 | 1000 | `6da200236fedfd7c` |
| `es1-1d` | CME_MINI:ES1! 1D | 1 | 1000 | `c7340cc55d53aa28` |
| `nq1-15` | CME_MINI:NQ1! 15 | 1 | 1000 | `a12dfc0dd10a645c` |
| `nq1-1d` | CME_MINI:NQ1! 1D | 1 | 1000 | `30d0bf793ea2fd65` |

Every value equals the lane's quantity step, the `PINEFORGE_VERIFY_QTY_STEP`
its registry template (`lane_input_templates.environment`, read 2026-09-28)
declares: aapl, es1, f, nifty, nq1 (15 and 1d) `1`; btcusdt-15/-1d `1e-05`;
eurusd and xauusd (15 and 1d) `0.01`; ethusdtp-1d `0.0001`. The two ETH 15m
templates (eth-scraped-15, eth-corpus-15) declare no step; the Lab verifier
substitutes ETHUSDT.P's 0.0001 there and records it as its default, not as lane
evidence.

`scripts/run_strategy.py` (`inputs_run_kwargs`) therefore declares
`syminfo.mincontract` from the lane template's step as the case runner hands it
to every run (`PINEFORGE_VERIFY_QTY_STEP`), and only from it: generated code
reads the fact through `get_syminfo_metadata("mincontract")` (codegen F06), and
a probe that declares its own keeps it. `scripts/test_run_strategy_mincontract.py`
reads every directory here.
