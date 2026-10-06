# margin_open_rules

TradingView strategy tapes of synthetic controls that pin the opening-call
rules behind `pineforge::source::detail::MarginOpeningSwitches`: 99 tapes (283
trades), each exported twice with `lab tv --no-note` over the ws-report-v1
channel, both exports byte-identical, every export's range proof `covered`.
Every prediction was written before its export (`provenance.json` names the
preregistration file of each round). All are commission 0.05 % (percent),
margin 100 % but `add-open/` (1 %), slippage 1 or 2 but `add-open/` (0);
`exit-order/` has two tapes at commission 0.1 %, two at margin 50 % and four
at slippage 0. Two tapes are known divergences the test lists
(`kKnownDivergences`).

## Groups

- `po/` (24, OANDA:EURUSD, BINANCE:ETHUSDT.P and NYSE:F 15,
  process_orders_on_close): a full-margin position one market entry opened
  at a close is called at the
  next open sized at that close c' -- money and unit margin at c' -- long or
  short (`close_sized_long_call`): `long-divisor-up` / `-down` put the open
  14 ticks above and 8 below c', so the divisor decides 100.00 against 99.96;
  `long-residue-call` / `-open-call` put the ten-digit residue of the
  requirement on either side of zero at c' and at the open. The call's
  one-unit band and the short gate are priced at its own print, the open's
  tick moved by the slippage (`close_call_gate_at_print`; `short-vetoed-at-print`:
  the close-sized call vetoed there leaves the open's own call). After it the
  booked book is checked again at the open's mark, at the same print
  (`booked_open_recheck`; `short-booked-recheck`: 3.52 + 1.68,
  `short-weekend-gap`: 14.08 + 12.76 across a weekend), and then the lagged
  follow-up of the last call at that print, with the one-unit fallback
  (`open_print_follow_up`; `short-follow-up-one-unit`: 0.0004 + 1 ETH).
  That re-check is a check like any other (`short-recheck-*`, an open 1 to 5
  ticks above the close; EURUSD, but the `-eth` one and the NYSE:F `-whole`
  ones, whose open is the session's): its call passes the short gate at the
  print, and where it finds the book short but books nothing -- its units
  vetoed there (`short-recheck-vetoed`: 0.48 against the gate at slippage 1,
  `-slip2`: 1.0 at slippage 2, `-whole`: 4 shares) or under one restore step
  and outside the one-unit band (`short-recheck-rounds-to-zero`, 150000 units)
  -- the sequence ends: the close-sized call's follow-up is neither booked at
  the open nor owed to the next point (`-low-first`: a low the book is not
  short at books nothing), and the next call is the path's own. Where the
  re-check books, its own follow-up is owed to the bar's next point instead of
  the open's print: booked at that point's print where the book is not short
  there (`short-recheck-owes-low`: 1.16 at the low; `-slip2`: 2.56; `-eth`:
  0.0032 ETH; `short-recheck-owes-high-whole`: 20 shares at the high), dropped
  where the point is short itself (`short-recheck-owed-dropped-high`: the
  high's own 89.84). A re-check that owes nothing books alone
  (`short-recheck-no-follow-up`: 4.24 + 3.68), and where the booked book is
  not short at the open the close-sized call's follow-up books at its print
  (`short-open-not-short-follow-up`: 2.72 + 2.8).
- `replica/` (8): population members' bars, quantities and TradingView-derived
  cash on the same rules (NYSE:F 2026-02-09 and 02-27, ETH 2025-05-24, 06-02 and
  08-08, EURUSD 2025-04-29, 06-18 and 10-17). EURUSD 04-29 and 10-17 book no
  trade: TradingView refuses the explicit quantity at the slipped close, as the
  engine does. `eur-short-recheck-vetoed-0618` is the caldera cohort of
  `short-recheck-vetoed`: 5.52 at the open, the re-check's 0.16 vetoed (0.18
  against 0.84), then the high's 522.36.
- `admission/` (5, BINANCE:ETHUSDT.P 15, slippage 2, and one NYSE:F): an
  explicit-quantity market entry from flat filling at the next open is dropped
  where sig10(sig10(E) / Q) is below the slipped open tick
  (`open_fill_admission`; `long-dropped-at-open-0502`: 4.9204 on 9083.616855
  at 1846.12; 0.05 more cash admits it). `short-refused-at-close` (NYSE:F,
  process_orders_on_close, slippage 2): a short of 5000 at the 10.34 close on
  51696.55 books no trade -- 5000 x 10.34 is above the capital, 5000 x the
  10.32 slipped fill is not.
- `add-open/` (1, EURUSD, margin 1 %, slippage 0): a short add filling at an
  open is in the book, with its entry fee, when that open is checked: 3069.72
  at the open, then 51027.16 at the high (`open_check_after_add_fill`).
- `lots/` (10, OANDA:EURUSD 15, process_orders_on_close): a
  carried short's lagged follow-up owed to its bar's close is booked after the
  script, ahead of the close's fills (`MarginScheduleSwitches::
  close_follow_up_after_script`), and the script's orders keep the sizes they
  were placed with: `strategy.close("S")` (`close-id-overfill-*`) or
  `close_all()` (`close-all-overfill-gap`) buys the units the script saw and
  opens the excess long as its own lot (41.52, 34.4, 0.56), the reversal entry
  "L" opening its own leg beside it; a reversal entry alone takes the excess
  into its own lot (`reversal-entry-joins-lot`, 1.4). At the next open the
  call sized at the close takes the whole book first in first out, and its
  follow-up is checked lot by lot, the last still-short check deciding
  (`lot_by_lot_open_follow_up`): 41.52 + 1069.84 then 950.68 across a weekend
  gap; on books two or three entries of one close opened, 20 + 338.96 then
  279.12 (`two-longs-one-close`), 12 + 18 + 309.08 then 219.24
  (`three-longs-last-short`), shorts 20 + 186.64 then 167.8
  (`two-shorts-one-close`). `close-id-overfill-0409` is OANDA:EURUSD
  2026-04-09 01:00-02:15's own bars at a lane-chosen capital. The over-fill
  lot is a lot of its own in the close ledger: after the open's calls took it
  first, `strategy.close("L")` closes all of L (18619.72) and a short S2 opens
  its 18000 (`overfill-then-close-entry`); with L default-sized at 100 % of
  equity it opens 18664.13 beside the lot at the close it was placed at, and
  the close takes 18586.61 (`overfill-default-then-close-entry`; the config
  line's ninth field is the default percent of equity, a `nan` quantity a
  default-sized entry).
- `lag/` (5, NYSE:F and EURUSD, process_orders_on_close): the lagged follow-up
  of a short's call on whole-share books with the one-unit fallback
  (`whole_share_lagged_short`), a follow-up that books leaving its own for the
  next point (`chained_follow_up`; `whole-share-chain`: 1 at the open, 1 at the
  high, 1 at the low), and an owed follow-up dropped at a point whose own check
  finds the book short, the open's own call owing its follow-up to the first
  extreme in the opening-call scope (`short_point_drops_owed`; `owed-dropped-fractional`: 41.52 at the
  high, not 0.64 at the open and 38.96).

- `exit-order/` (31, BINANCE:ETHUSDT.P and NYSE:F 15, not
  process_orders_on_close): a market entry from flat
  filling at a bar's open with a `strategy.exit` bracket placed on its signal
  bar. A leg already marketable at that open -- a short's stop at or below
  the open's tick or its limit at or above it, a long's mirror -- fills there
  first and the opening check runs on the book it leaves: nothing after a
  whole exit (`short-stop-at-open`, `-equal-open`, `-noslip`, `-margin50`,
  `short-limit-at-open`, `long-stop-at-open`, `long-limit-at-open`,
  `short-equity-sized-stop-at-open`), 0.0032 called at the open's print after
  an exit of 0.0004 (`short-partial-stop-at-open`). A leg the bar reaches
  only after the open, one tick after it included, fills after the check's
  call at the fill (`short-stop-on-path`: 0.0048 called, then the stop fills
  3.4952; `-limit-`, `-noslip`, `-one-unit`, `-margin50`, `-one-tick-above-open`,
  `short-partial-stop-on-path`, `long-stop-on-path`, `-one-tick-below-open`,
  `f-short-stop-on-path`, `short-equity-sized-stop-on-path`), and
  `short-fee-stop-on-path` / `-open-at-low` take the settings and bar shape
  of `test_margin_call_1x_long_entry_fill` scenario E: 0.008 called at the
  open, then the stop fills 3.492 (`open_marketable_exit_first`).
  `eth-short-stop-on-path-0623` replicates a population entry (ETH 15,
  2025-06-23 13:30 UTC: its bar, quantity, TradingView-derived cash and stop):
  0.0068 at 2247.38, then 3.5224 at 2267.78. `short-stop-on-path-no-deficit`
  books no call, and `f-short-refused` no trade.
  A `strategy.exit` without `from_entry` placed with the entry is ordered the
  same way (`global-short-stop-on-path`, `-stop-at-open`,
  `global-long-stop-at-open`, `-stop-on-path`: each is its bound control
  with the exit made global, and TradingView books the bound control's
  rows). Known divergences, kept on ab9714be's rule by the
  engine (a short gives way to any leg its bar touches): a short reversal
  (`reversal-short-stop-on-path`: out of a 0.001 long) and a short stop entry
  gapped at the open (`stop-entry-short-stop-on-path`: a sell stop at 2490.0
  above the 2483.19 close), where TradingView calls 0.0048 at the open print
  before the stop fills 3.4952, as for the market entry; the engine books the
  stop 3.5 and no call. No tape covers adds, books of several entries, limit
  entries, process_orders_on_close or calc_on_order_fills here; the engine
  keeps ab9714be's rule for them too.
  A `strategy.exit` without `from_entry` placed *before* the entry, while the
  book is flat, is the `exit-before-entry/` group's.

- `exit-before-entry/` (15, BINANCE:ETHUSDT.P 15, commission 0.05 %,
  slippage 2): each is one of `exit-order/`'s global-exit controls with the
  `strategy.exit` call moved ahead of the entry, so it runs while no position
  is held. With no entry order of any id working, TradingView voids it
  (`void_flat_global_exit`): the entry placed after it opens a book with no
  exit, its opening check is booked at the open print as with none, and the
  exit never fills -- not at the open, not on the path, not under
  process_orders_on_close. A short calls 0.0048 at the open print and 0.0416
  at the high's waypoint, and `close_all` closes 3.4536
  (`short-stop-on-path`); a void stop already through the open fills nothing
  there (`short-stop-at-open`: 0.0048, then 0.128 at the path's waypoint;
  `long-stop-at-open`: 0.0048, then `close_all`), nor does one the path
  reaches (`long-stop-on-path`). Called at the script's top level on every
  bar, the exit is void while flat and live from the next bar once the
  position is held (`-every-bar`: X 3.4536 and 3.3672 at the 00:45 open).
  Relative legs give the same tapes (`*-loss-ticks-*`, byte-identical to
  their absolute pairs), as does process_orders_on_close (`*-pooc`: the void
  stop fills neither at the next bar's path nor at the entry's own close). A
  long limit entry `W` at 1000, which never fills, working when the exit is
  called -- placed on the bar before (`short-stop-on-path-limit-entry-working`)
  or in the signal block ahead of the exit (`-same-bar`) -- makes the exit
  bind, and the short's fill takes it: those two tapes are
  `global-short-stop-on-path`'s byte for byte. No tape covers
  calc_on_order_fills, nor a void call followed by a stop or limit entry;
  the engine keeps its former course in both (the exit's legs rest, and
  those still working when the entry fills close the book it opens).

## Layout

- `<group>/<name>/`: `strategy.pine` (the script TradingView ran),
  `tv_trades.csv` (its trade list, byte-identical to the export),
  `metrics.json` (the export's summary) and `spec.txt`, the replay
  `tests/test_margin_open_rules_tapes.cpp` drives (format in that file's
  header: the strategy settings, the bars window, the exchange session for
  NYSE:F and for the two EURUSD windows spanning a weekend, and the script's
  `strategy.*` calls at the bars they name, `*` for a call on every bar).
- `bars/`: 15-minute OHLCV windows (`timestamp,open,high,low,close,volume`,
  UTC milliseconds), every bar from the chart's `from` 00:00 UTC through its
  `to` 00:00 UTC inclusive, cut from the lane feeds named in
  `provenance.json` (`feed_sha256`).
- `provenance.json`: per tape the fixture path, the export directory it was
  taken from (`origin`, relative to the pinning lane's scratch root), the chart
  window, the sha256 of both exports' `tv_trades.csv` and of `strategy.pine`,
  the export start times, tool, channel and the preregistration; per bars file
  its sha256, row count and source feed.
- `tapes.txt`: the tapes the test replays.

## What the test asserts

Every tape's TradingView rows -- each Exit row paired with the Entry row of
its trade number -- against the engine's report rows at the same index:
side, quantity in lots, entry and exit price in ticks, entry and exit time,
and the row count. Each switch, off alone, costs at least one tape. And every
tape is replayed forward -- a stream over its first bar, then every later bar
pushed -- whose closed trades must equal the backtest's, field for field.
