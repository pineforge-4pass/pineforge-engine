# Wave-J rules acting together (INT28)

INT28 integrated the wave-J lanes onto main 962960b3. Git merged every lane
into `src/source/pine_adapter.cpp` without a textual conflict, but several
lanes changed the same functions -- `on_applied` (TVDEF-DROPS R3, W3 rules B
and C, W5 M2, C2 and MK, W8E C1), the exit-reservation functions (W3 rule A)
beside W5's MR, and `entry` (W5 M1, W6 F10, W8D). The probes below are
synthetic, written for INT28, in which two lanes' rules act in one run; no
closed or scraped strategy is involved. The `entry` rules' scopes exclude one
another (`tests/test_int28_rule_compositions.cpp` says how), so no probe
combines them.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered) on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-17,
byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`. `tests/test_int28_rule_compositions.cpp`
replays each through the Pine host over the 15m bars of
`tests/fixtures/margin_v6/bars.inc` and requires every trade the tape closes
to be the engine's.

| tape | trades | `strategy()` declares | the rules that act (INT28's census) | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `int28-c1-two-parent-reversal-v6` | 40 | nothing (v6 defaults) | the cells of `exit_queue/w3f05-r1-two-parent-reversal-global-exit` every day at 100% of equity: W3 rule C (9) where a reversal voids the long's global exit, W5 MR (24) where the reversal is declined and the long keeps it, W4 F19a (10) where that exit sizes on the position it fills | `f0ac5087c18f51a9227c512f04f9c62837f6c203f68316cffdcfd4cb7856d57e` |
| `int28-c2-pooc-global-pair-opening` | 8 | `process_orders_on_close`, commission 0.1 % | a full stop-and-target A, then a nearer full target B, for every entry, armed at each opening: W3 rule B cancels B and W5 C2 moves the opening's call to the next open, at the same fill (8 each); B never fills | `8cfe08daff162a2df6037ab20b02a0c5ef4c57ac40725e402a355f6ee58b8fcb` |
| `int28-c3-exit-queue-explicit-pair-add` | 2 | capital 1000000, fixed 1, pyramiding 2 | control: A's exits AX1 (qty 1) and then AX2 (qty 1, a nearer target) are queued, and the same-bar add of A is covered at AX1's price (the shape of `ki62_same_id_cover/t3-same-id-add-cover`); neither W3's nor W8E's change acts | `636e27a5231356761b4f3c551f79143edb88e2b6cd0452884f097cae3c01b2fd` |

c1 and c2 fail on main 962960b3 and after TVDEF-DROPS, and pass from W3's picks
on; W5's rules act in them without moving a trade TradingView books.

The test's fourth row, c4, replays lane R1-CONSOLIDATE's `w8a-dca-open` tape
(`tests/fixtures/open_fill_order`, BINANCE:ETHUSDT.P 15; its bars are the same
corpus rows) trade for trade and in TradingView's order: W8A R-B admits E3's
P3 and E4b's HM (a pending limit does not count against pyramiding 3) and
R-A fills the market orders at one opening price before the buy limits the
open reached, lowest limit first -- in E4b both act on one fill point. It
fails on main and after W8A's picks and passes from R-A's pick on.

The late picks (W4-ENG-POOC-SAMEPASS, W6B-ENG-PAIRS, W8A-SIGSTATE-1,
R1-CONSOLIDATE, W3B-ENG-GRID) meet the same functions; their compositions with the earlier
lanes' rules are witnessed by the lanes' own tapes, all green on the
integrated tree, in which INT28's census finds both lanes' rules acting (the
test's header lists them), by c4 above, or are scope-disjoint (W6B's opening
margin checkpoint and W5's margin rules; W8A R3 and W5 M1; R1, a reversal of
a held position, and W6/W6B's pair rules from flat; R-A and the pair route of
a flat book holding MARKET entries of both sides). W3B-ENG-GRID's rules are
composed with W4 F19a and W5 MR in those picks, pinned by W3B's tapes
`w3bf05-g8` and `w3bf05-r1c` (`tests/fixtures/exit_queue`), which fail on the
uncomposed merge; the test's header says how.
