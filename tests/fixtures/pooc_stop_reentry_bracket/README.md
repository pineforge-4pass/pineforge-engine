# process_orders_on_close stop re-entry bracket tapes (R5 lane INT26, round 2)

TradingView's own trades for the shape INT26's gate sweep lost a row on
(scrapper:data-BINANCE-BTCUSDT/standard/vasudevshenoy-manoj-betrayed-me, BTCUSDT
15m, 2025-08-03 10:00 UTC), as a synthetic script: under
`process_orders_on_close`, at each of three flat signal closes the script cancels
its pending stop entry and places it again with its `strategy.exit` bracket
(limit 3c beyond the close, stop 10c the other way) -- first 10c beyond the close
(unreached), then 1c beyond it (the next bar fills the entry and then the
bracket's limit, and closes flat), then at that close (reached: it fills there),
with a fresh bracket of the same exit id whose limit a later bar reaches. Long on
bars 39-41 (the limit on bar 42), short on bars 44-46 (the limit on bar 49).
`int26-reentry-noexit-chart` is the control: the same script, but the short's
re-entry on bar 46 places no `strategy.exit`, so its row 4 is still open when the
data ends (its exit signal empty) and no bracket leg is left to close it.
`tests/test_pooc_stop_reentry_bracket_tapes.cpp` replays each tape through the
Pine adapter on the 15m chart (`-chart`) or the 1m bars aggregated to 15m under
the magnifier (`-mag`; its row 4 is a recorded divergence, see the test).

Each directory is one `lab tv` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, NYSE:F 15, window 2025-07-01 .. 2025-07-08, `--no-note`),
byte-identical: `strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`,
`metrics.json`. The scripts count chart bars from 2025-07-02 09:30 ET, the first
bar of `tests/fixtures/session_islastbar/bars.inc`, whose 15m and 1m bars the rows
replay.

| probe | variant | trades | tvTradesSha256 |
|---|---|---|---|
| `int26-reentry-bracket-chart` | chart | 4 | `e0447b533954985426b7fa0d616c4d8455179d71ed206272e3212b1b4c1b6061` |
| `int26-reentry-bracket-mag` | bar magnifier on | 4 | `b7e89ce188ae1a3e67766b1e9093d9e207a8c618df4aaa91e66d48ca605d53e6` |
| `int26-reentry-noexit-chart` | chart, the short's re-entry without `strategy.exit` | 4 (the last open) | `efbd25e3c3f4f32101dd7524b551f898818a40a8c470128ee47c853d188e9039` |

Every row of the two bracket tapes is closed and the book ends flat; the control
ends short 100.
