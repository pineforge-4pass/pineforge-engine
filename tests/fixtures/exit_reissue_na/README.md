# A strategy.exit re-issue that leaves a level na withdraws that leg (R5 lane TAIL-D)

A `strategy.exit` call that names a level replaces the whole exit order: a price leg the new call
leaves na is withdrawn on TradingView, a stop and a limit alike. The adapter re-issued only the
levels the call named and kept the old leg resting (fran-pineda-strategy-501 / -502 on
OANDA:XAUUSD 15).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (pineforge-workflow, channel `ws-report-v1`, OANDA:XAUUSD 15, 2025-08-14 .. 2025-08-20), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved. Pyramiding 2, a fixed size of 1; the
na levels are a runtime `var float` na. `tests/test_exit_reissue_na_tapes.cpp` replays the tape.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `fran-x1-exit-reissue-na-limit` | longs C/D with CX/DX (stop 3336, limit 3345), CX re-issued with stop = na; shorts A/B with AX/BX, AX re-issued on the Sunday reopen with a new stop and limit = na; DX/BX controls | 4 | `ac2f93a3d5d8501b33dc6bfafc903a8124410d589ac52575224f0a1f004f6769` | covered |

## Bars

`bars.inc`: 2025-08-14 00:00 .. 2025-08-19 23:45 UTC of the xauusd-15 lane chart feed (sha256
`248086b8b82d6560277cdf7bb877a269f04067b8cdb8c1577a02323772c48579`).
