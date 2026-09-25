# Pyramiding count tapes (H-MEASURE row M13 / G2-15)

TradingView's own trades for Pine `pyramiding = 2` (fixed 100 shares, orders
filled at the next bar's open), fifteen scenarios on fifteen NYSE:F sessions,
each flat at its start and closed by `strategy.close_all`.
`tests/test_pyramiding_count_differential.cpp` runs every scenario three ways
on the same bars: the Pine adapter (its per-cycle `accepted_in_cycle` count,
`project()` leaving `max_open_lots` unset), a bare `NativeStrategyHost` with
`max_open_lots = 2`, and this tape.

Each directory is one `lab tv` export (`ws-report-v1`, `rangeProof: covered`,
NYSE:F 15), byte-identical: `strategy.pine`, `tv_trades.csv` (UTC+8),
`meta.json`, `metrics.json`.

| probe | window | trades | tvTradesSha256 |
|---|---|---|---|
| `hm-orders-m13-f-pyramiding` (P1-P4, P4b, P6, P8-P10) | 2025-05-01 .. 2025-07-31 | 24 | `de780cacf8ee965d787ef715d3af6ddf439370652dea820e71dbad55c4b0909d` |
| `hm-orders-m13-f-pyramiding-rule` (D1, D2, D3, D3b) | 2025-06-02 .. 2025-07-31 | 9 | `44c68d01446717e491eb9389ac5e6db8ef9f501a437bf7640cd6e1b3ebe0cfa4` |
| `hm-orders-m13-f-pyramiding-cap-orders` (D4, D4s) | 2025-07-01 .. 2025-09-30 | 4 | `67cb6805a7b9cb20c8542e1e4c0805dbd539d1f3182dafe4a020a0c01c77a1b5` |

| scenario | shape | TradingView | adapter | `max_open_lots` |
|---|---|---|---|---|
| P1 | `close(qty)` closes lot 1 whole, re-add | re-add fills | = | = |
| P2 | `close(qty)` inside a lot, re-add | refused | = | = |
| P3 | close one id of two, third id | fills | = | = |
| P4 | resting far limit L2, then a market add L3 | L3 fills | refuses L3 | = |
| P4b | the resting limit fills after the add | L, L3 AND L2 (3 lots) | L, L2 | L, L3 |
| P6 | three market entries on one flat bar | 2 fill | 3 fill | = |
| P8 | `exit(qty)` from B stops out lot A (FIFO), third id | fills | refuses | = |
| P9 | `exit(qty)` from A stops out lot A, third id | fills | = | = |
| P10 | `exit` from B, default quantity, lots A+B | closes 100 (A) | closes 200 (100 since R5 lane H-THIN) | = (Reduce 100) |
| D1 | one lot; same bar: far limit, then market add | add fills | refuses | = |
| D2 | flat; same bar: far limit, two markets | both fill | = | = |
| D3 | at the cap; same bar: `close("A")` then `entry("C")` | C fills | refuses | = |
| D3b | at the cap; same bar: `entry("C")` then `close("A")` | refused | = | = |
| D4 | at the cap: a limit entry placed, reached later | never fills | = | = (refused at the fill) |
| D4s | at the cap: a stop entry placed, reached later | never fills | = | = (refused at the fill) |

What the fifteen imply about TradingView's rule: an entry is checked once,
when it is first processed at the next open (orders in placement order),
against the trades then open in its direction; a resting limit or stop entry
that passed that check is never checked again when it fills (P4b). The
kernel's `max_open_lots` (surviving + new lots at every match) agrees on 14 of
15, all but P4b; the adapter's command-time count on 8 of 15. P10 is not a
pyramiding question (the quantity a default `strategy.exit(from_entry)` closes
under FIFO); it was pinned as a separate recorded divergence and R5 lane
H-THIN fixed it (the exit reserves its entry's own quantity; 9 of 15 now).

R5 lane H-THIN measured the lowering the fifteen suggest -- `project()`
declaring `max_open_lots = max(1, pyramiding)` -- on the corpus: with the
adapter's count kept beside it, 4 probes move, each away from its TradingView
tape (`bracket-tp-sl-oca-reduce-isolate-01` 2240 -> 2150 trades matched,
`composite-bracket-cap-range-pending-stop-01` 1178 -> 1170,
`order-dual-stop-both-touch-priority-01` 755 -> 738,
`order-opposite-entry-close-same-pass-01` 828 -> 824); without the count, 11
move. TradingView's first-eligible-point rule (P4b) decides those corpus
trades, and the kernel's cap, checked at every match, has no such mode, so
the count stays until the kernel does.

`bars.inc`: per scenario the 12 bars from its first signal bar, from the
registry feed `lab bars NYSE:F 15`, evidence sha256
`80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`. Every
market fill lands on an on-grid open; each limit or stop level is first
reached on its intended bar with a tick of margin and no gap.
`expectations.inc`: each scenario's commands and pinned outcomes.
