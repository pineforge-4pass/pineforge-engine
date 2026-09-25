# Aggregated `entry_bar_index` tape (R5 lane H-MEASURE, AUDIT4 X14 F1(e))

TradingView's answer to which bar indices a `process_orders_on_close` script
reads on an aggregated chart. `tests/test_aggregated_entry_bar_index_tape.cpp`
replays it through the Pine adapter on the 15m chart, on the corpus 1m bars
aggregated to 15m, and on the same aggregation under the bar magnifier.

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.

| probe | chart | window | trades | tvTradesSha256 | note |
|---|---|---|---|---|---|
| `hm-f1e-ebi-pooc-eth` | BINANCE:ETHUSDT.P 15 | 2025-06-11 .. 2025-06-13 | 4 | `e7004ca5e207f86d3535bd20157dbc16af0d221968bf3b18464ab80109506c5f` | `tv-tape-hm-f1e-ebi-pooc-eth-e7004ca5` |
| `hm-f1e-ebi-pooc-eth-mag` | same, `use_bar_magnifier=true` | same | 4 | `0c6badea92194fe4f9c2df36a7f9aadb496f6d953d1dcdb5884cda5ec307d839` | `tv-tape-hm-f1e-ebi-pooc-eth-mag-0c6badea` |

The script enters at 01:00, 05:00 and 09:00 UTC on 2025-06-12 and closes each
position on `bar_index - strategy.opentrades.entry_bar_index(0)` (held 1, 3,
then the last closed trade's `exit_bar_index - entry_bar_index` + 2 = 5), and
enters a fourth when `bar_index - strategy.closedtrades.exit_bar_index(2) == 4`
(held 2). The two tapes differ only in trade 2's favorable excursion (0.40
without the magnifier, 1.86 with it).

`bars.inc` holds BINANCE:ETHUSDT.P 1m rows 2025-06-12 00:00 .. 12:59 UTC copied
from the corpus 1m feed (gitlink `eede4a2`, sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`, no gap in
the span) and the same span resampled to 15m by `scripts/derive_corpus_feeds.py`'s
rule. The eight tape prices are the
15m closes of their bars.
