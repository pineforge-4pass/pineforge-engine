# The order of the fills at one opening price (R-A, lane R1-CONSOLIDATE)

At one opening price TradingView fills every MARKET order first, entries and
exits alike (a `strategy.close_all` queued at the previous close among them),
then the buy LIMIT entries the open has already reached, lowest limit first.
It does so whether a limit was placed at the previous close or has rested
since an earlier bar; neither the order the orders were placed in nor their
ids decide. A limit filled that way keeps the quantity it was placed with: a
buy limit of 100 placed flat, filled after a market short of 200 opened at
the same price, closes 100 of the short and opens nothing.

The native matcher breaks a same-point tie by queue order, so the engine
filled such orders in placement order: a limit resting beside a close opened
and was closed by it at once, and limits placed in descending order filled in
that order. The scraped `job-2712-3commas-3commas-gold-vault-long` and
`3commas-3commas-silicon-vault-long-intc-dca-strategy` (BINANCE:ETHUSDT.P 1D)
keep their DCA safety-order limits resting beside the take-profit
`close_all`, and an open that gaps through them fills them with the close at
one price. Lane W8A-SIGSTATE-1 pinned the rule on `w8a-dca-open` and left it
as a patch; lane R1-CONSOLIDATE measured it against lanes W6 and W6B (their
rank and pair rules do not order these books: W6's same-rank tie-break is
placement order, and E1 is price order) and added `r1c-ra-prior-limit` for
limits that rested since an earlier bar.

Each directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel
`ws-report-v1`), byte for byte: `strategy.pine`, `tv_trades.csv` (times at
UTC+8), `metrics.json`, `meta.json`.

| tape | chart | cells | trades | tv_trades.csv sha256 |
|---|---|---|---:|---|
| `w8a-dca-open` | BINANCE:ETHUSDT.P 15, 2025-04-01 .. 05-01, pyramiding 3, cash 100 default, an explicit qty on every call | E1 flat, buy limits B (+1.0 %), A (+3.0 %), C (+2.0 %) above the close fill as B, C, A; E2a a limit L1, then `close_all`: the close fills first, L1 opens after it and is held to the cleanup; E2b the control, `close_all` then L2; E3, E4a, E4b lane W8A-SIGSTATE-1's rule R-B (a pending LIMIT entry does not count against pyramiding), E4b's market HM filling before the limit HL | 18 | `17289e79b7821acb80c19adb056cfd27d15e62fd32f46435bdeef1060e17117c` |
| `r1c-ra-prior-limit` | NYSE:F 1D, 2024-12-01 .. 2025-08-01, initial capital 10000, pyramiding 2, explicit qty | each cell rests a buy limit from day d0 inside the gap between d1's low and d2's open, places market orders at d1's close and is flattened at d2's close. E then a same-side market entry: the market fills first; X in position, then `close_all`: the close fills first, the limit opens after it; O then an opposite market entry of 200: the short opens first and the limit's 100 closes half of it; M, N (a flat opposite market pair, about 55 % and 18 % of the equity each) and P (the pair alone): the pair fills first, as one transaction of the later call's own and the pending market's quantity, admitted at its gross -- M's later call is dropped | 13 | `af9a3a84e72f0dcb4662cfa53175c9a78c09a0ffa5561aa3cf263eaa28c73ae8` |

`w8a-dca-open` is a byte-identical copy of lane W8A-SIGSTATE-1's
`tests/fixtures/pyramiding_open_order/w8a-dca-open` (its `eth15_bars.inc` is
that fixture's `bars.inc`). `r1c-ra-prior-limit`'s `metrics.json` says
`rangeProof: narrower-than-requested`: on a daily NYSE chart the returned
range runs from the first session bar after the requested 00:00 UTC start
(2024-12-02 14:30 UTC) to the last one before the requested end (2025-07-31
13:30 UTC), so every bar of the window is covered; its cells trade from
2024-12-16 to 2025-07-17.

`tests/test_open_fill_order_tapes.cpp` replays both scripts through the Pine
adapter under the configuration each generated constructor declares, over
the lane feeds embedded here (`eth15_bars.inc`: the corpus 15m chart feed
rows 2025-04-14 00:00 .. 04-16 12:00 UTC; `f1d_bars.inc`: the NYSE:F 1D lane
feed, `lab bars` sha256
`e3dd3a88e85bde802b65c26e1ad35cb17b3dcf66ad56f70e30a2ff711f2297bb`, rows
2024-12-02 .. 2025-07-31), and requires cells E1, E2a and E2b of
`w8a-dca-open` and E, X and O of `r1c-ra-prior-limit` to be TradingView's
trade for trade, in TradingView's order. It reads the rule off TradingView's
rows in every cell.

Cells left to other rules (read off TradingView's rows only):

- E3, E4a and E4b of `w8a-dca-open` are R-B's (W8A-SIGSTATE-1's
  `tests/test_pyramiding_open_order_tape.cpp` pins them with R-B).
- M, N and P of `r1c-ra-prior-limit` are the opposite-pair family (lanes W6
  and W6B: rank, one transaction, gross admission). The adapter leaves a flat
  book holding MARKET entries of both sides to that family's route, whose
  legacy pin `test_dual_entry_placement_sizing`'s
  `test_MM_prior_bar_gapped_limit_disqualifies_current_pair` (a prior-bar
  gapped buy limit beside a same-bar 55 % pair) books the limit first and
  the pair as two reversals; cell M is TradingView's answer for that shape:
  the pair's sell first, the later buy dropped at its gross, then the limit
  closing 100 of the short.
