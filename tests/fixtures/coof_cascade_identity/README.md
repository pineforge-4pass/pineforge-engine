# calc_on_order_fills refill-cascade tape (R5 lane W7, TAIL-TRIAGE family F13)

TradingView's own trades for a `calc_on_order_fills` refill cascade of
unique-id adds, and the engine's trades for the same script, graded by
`scripts/test_verify_corpus_metrics.py` (`CoofCascadeIdentityTapeTests`).

`w7-coof-cascade-identity/strategy.pine` places one add on the close of every
fourth hour's :30 bar; each fill's recalculation places the next, so the :45
bar books four entries: `L0` and `L1` at its open, `L2` at the extreme the bar's
path reaches first, `L3` at the other. The position is closed on the next
hour's :15 open. The open is therefore a key TradingView proves to hold two
physical entries (two distinct Signals at one time, price and side).

| tape (`lab tv --no-note`, ws-report-v1, rangeProof covered) | chart | window | trades | tv_trades.csv sha256 | pine sha256 |
|---|---|---|---|---|---|
| `w7-coof-cascade-identity` | BINANCE:BTCUSDT 15 | 2025-04-04 .. 2025-04-07 | 72 | `c7bda1d26f4662e51dd04ad9601a8d3f8a2b2fa620678a78713d39eb89b6d2ec` | `96c6d8b03e7983858013464bf578fb7954871dbe624f2cbef0282eb60e34348e` |

`strategy.pine`, `tv_trades.csv` (UTC+8), `meta.json` and `metrics.json` are
the export, byte-identical. `engine_trades.csv` is the engine's export of the
same script: engine d3b753c1 library, codegen d7e095f, run by the Lab verifier
(`scripts/verify-engine-local.py` at 3bac0b7b) on the btcusdt-15 lane's feeds
(chart `6b54c44ac6de8b7b588b3fcc6ddc29b09e5fe12ce9559d811dca7859217e6270`,
finer `787fd2e15bfeddb062cbb7905248ff4be2fe5c695bbe971f9322826f16528b86`).
All 72 trades equal the tape's (entry and exit time and price, qty, P&L), and
every entry carries its own `Engine entry incarnation`.

Two of the eighteen cascades open one tick below the bar's high, so `L2` fills
inside the strict entry tolerance (0.01%) of the proven open key:

| bar (UTC) | open: `L0`, `L1` | high: `L2` | low: `L3` | engine incarnations |
|---|---|---|---|---|
| 2025-04-05 04:45 | 83757.41 | 83757.42 | 83608.80 | 36, 37 / 38 / 39 |
| 2025-04-05 08:45 | 83566.71 | 83566.72 | 83505.69 | 41, 42 / 43 / 44 |

Before lane W7 the canonical grader projected the engine's `L2` row onto the
proven open key, because only proven keys were projection candidates: three
incarnations against two Signals, `distinct-entry multiplicity Δ 2`, tier
strong. An engine row exactly at another TradingView entry's price is that
entry's own; with that rule the tape grades excellent. The population probe
`coof-refill-cascade-probe` shows the same shape on seven 15-minute lanes.
