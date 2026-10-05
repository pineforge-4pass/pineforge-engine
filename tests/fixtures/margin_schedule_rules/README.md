# margin_schedule_rules

TradingView strategy tapes of synthetic controls that pin the margin-call
schedule rules behind `pineforge::source::detail::MarginScheduleSwitches`:
39 tapes (670 trade rows), each exported twice with `lab tv --no-note` over
the ws-report-v1 channel, both exports byte-identical. Every prediction was
written before its export.

## Groups

- `gate/` (8, OANDA:EURUSD 15, process_orders_on_close): a short's call of
  `X` units found at mark `p` is taken only where `X p' m > Q p' m - E(p)`,
  `p'` the call's slipped print (`short_call_gate`); the `vetoed-*` /
  `passes-*` pairs straddle that line at slippage 1, 2 and 3, and the two
  `high-first-*` tapes put the lagged follow-up at the low of a high-first
  bar (`lagged_short_follow_up`, `short_path_points`).
- `lag/` (4, EURUSD): two short lots of 0.5 and 60000 at slippage 0; the
  first lot's check after the call is still short and gives up 1.48 at the
  close; the control's larger first lot gives up nothing. The two
  `outside-pooc-*` tapes are the same lots filled at the next open: a call at
  the second extreme of a low-first bar is followed at the close (9.6 at
  1.13864), one at the first extreme of a high-first bar at the other
  extreme (10.32 at 1.13846). Their predictions were written before the
  export (`lag/outside-pooc-preregistered.json`).
- `veto/` (6, NYSE:F 15): a short whose one-unit call is vetoed at the open
  with a `strategy.exit` stop resting: the path's high is checked before the
  stop the path touches on the way, and the stop then fills at the high's
  print (`pending_veto_first`); the controls without a pending veto fill the
  stop at its level.
- `frozen/` (4, NYSE:F): a reversal `strategy.entry` placed at a close where
  a margin follow-up is booked after the script keeps the close quantity the
  script saw; the excess opens long (`frozen_reversal_close`). `close-id` is
  the same with `strategy.close("S")`.
- `money/` (7, NYSE:F, commission 0, slippage 2): a long of 8192 shares at
  13.68 after histories of one-bar round trips whose money -- sequential
  cash, initial capital plus the sequential net, initial capital plus the
  gain and loss sums -- straddles the cost; only the G + L sums decide the
  13.66 opening call as TradingView does (`long_call_gain_loss`). The
  history of each is replayed from its own tape: each earlier trade an
  entry at its entry bar and a `strategy.close_all()` at its exit bar.
- `add/` (4, NYSE:F, commission 0.1 %, slippage 3, pyramiding 10): a
  same-side add is judged as the whole book at the tick-built signal close
  against the fee-charged equity there, with and without
  process_orders_on_close (`add_signal_close`).
- `follow/` (2, NYSE:F): lots of 7, 29 and 700 shares called 36 at an open
  give up 8 more -- at the same fill under process_orders_on_close, at the
  next path point without it (`whole_share_lagged_follow_up`).
- `add-fee/` (4, EURUSD, margin 1 %): an add filled at an open on a book of
  500000; the book's call at that open is not implemented (three are in the
  test's known-divergence list).

## Layout

- `<group>/<name>/`: `strategy.pine` (the script TradingView ran),
  `tv_trades.csv` (its trade list, byte-identical to the export),
  `metrics.json` (the export's summary) and `spec.txt`, the replay
  `tests/test_margin_schedule_rules_tapes.cpp` drives (format in that file's
  header: the strategy settings, the bars window, the exchange session for
  NYSE:F, and the script's `strategy.*` calls at the bars they name).
- `bars/`: 15-minute OHLCV windows (`timestamp,open,high,low,close,volume`,
  UTC milliseconds), every bar from the chart's `from` 00:00 UTC through its
  `to` 00:00 UTC inclusive, cut from the lane feeds named in
  `provenance.json` (`feed_sha256`).
- `provenance.json`: per tape the fixture path, the export directory it was
  taken from (`origin`), the chart window, the sha256 of both exports'
  `tv_trades.csv` and of `strategy.pine`, the export times, tool and
  channel; per bars file its sha256, row count and source feed.
- `tapes.txt`: the tapes the test replays.

## What the test asserts

Every tape's TradingView rows -- each Exit row paired with the Entry row of
its trade number -- against the engine's report rows at the same index:
side, quantity in lots, entry and exit price in ticks, entry and exit time,
and the row count. A tape in the test's known-divergence list must still
differ. Each switch, off alone, costs at least one tape (the close call's
follow-up at the open is pinned by `margin_call_rules` `literals/literal-21`
instead). And every tape is replayed forward -- a stream over its first bar,
then every later bar pushed -- whose closed trades must equal the
backtest's, field for field.
