# A default all-in entry whose whole lots tie the rounded equity (R5 lane TAIL-D)

A default percent_of_equity (100 %) market entry on whole lots is sized on the signal's equity
rounded to ten significant digits. When that rounding ties -- the whole lots it sizes cost more than
the equity -- TradingView drops a flat entry and keeps only the closing leg of a reversal, whatever
the next open does (jaydeepp095-candle-harry on NYSE:F 1D). The equity is the capital plus the
realized profit plus the open profit in floating point, whose residue decides an exact tie (the
`jd-drift-*` tapes). A running sum of many closed profits carries a residue of its own the engine
does not reproduce; where the engine's sum and that sum on the money grid disagree on the tie, the
adapter keeps the no-gap rule it had (population probes, cited in the adapter's resolve_terms).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, NYSE:F 1D, 2025-04-01 .. 2026-05-01), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved. 100 % of equity, 100 % margin.
`tests/test_whole_lot_tie_tapes.cpp` replays every tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `jd-rev-tie-pair` | an all-in long reversed on 2025-12-15 by entry("Short") and close("Long"); the equity a whole-share multiple of the close; a gap up | 1 | `7aa4c448c7f407a546cc9e0c7e2d15511ac49700c03ad3cdff32b97d18d64822` | covered |
| `jd-rev-notie-pair` | one cent more capital (control: no tie, the long is held) | 1 | `de5002fa62016da7b298009d7b8b646e4d8b5b043138a50b79c48dde02769e1b` | covered |
| `jd-rev-tie-bare` | the tie without strategy.close | 1 | `7aa4c448c7f407a546cc9e0c7e2d15511ac49700c03ad3cdff32b97d18d64822` | covered |
| `jd-rev-tie-gapdown` | a tie 0.00003 under the multiple, the next open below the close | 1 | `284c8ce3641557347dba7ef145ffae4459f5d1eea38755e6df09fcfd574572fc` | covered |
| `jd-rev-tie-nogap` | a tie 0.00003 under, the next open equal to the close | 1 | `c4efe5ecafea4998e1f56d0087dfa57b7bd72ecea17149c13d6a555bd4001aa0` | covered |
| `jd-flat-tie-gapdown` | the flat entry itself on a tie (dropped) | 0 | `86e88f3e8067b4b2595036db9feaa39a14df66b43e6b306397c0ed2445acd76c` | covered |
| `jd-drift-under` | a fixed 101-share trade (2025-04-04 .. 04-08) whose loss and the capital sum one ulp under their cent value; an all-in long (2025-09-02) reversed by entry("Short") on 09-04, where that cent value is the whole shares' exact cost; a gap up (close-only) | 2 | `b750e1d7ef67e8951ec81399d7987a3750e9e00cf50611e26e4fb284eef6f781` | covered |
| `jd-drift-over` | the same with 102 shares, one ulp over (no tie: the long is held) | 2 | `f7f9c3bff31ee0b392ed7ca2a601d76d6e342afea12e6dd276cab5461199bb76` | covered |

## Bars

`bars.inc`: 2025-04-01 .. 2026-04-30 of the f-1d lane chart feed (sha256
`e3dd3a88e85bde802b65c26e1ad35cb17b3dcf66ad56f70e30a2ff711f2297bb`).
