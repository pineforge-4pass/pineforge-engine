# Callback and lifecycle tapes

87 independently authored synthetic strategies, each exported by
TradingView four times through `lab tv --no-note` with byte-identical results.
They pin how TradingView runs `calc_on_order_fills` recalculations on
historical bars, how newborn and resting exits reach the tick-built path, how
`trail_points` become ticks, what a declined reversal leaves live, and that a
re-issue does not lift the entry/close pair's barrier. No population source
was sent to TradingView.

Each directory holds:

- `strategy.pine`: the exact bytes TradingView ran.
- `tv_trades.csv`: the first export's List of Trades. Timestamps are UTC+8.
- `bars.csv`: the chart-feed rows the script runs on, epoch milliseconds UTC.
- `configuration.json`: symbol, session, mintick and quantity step for
  `scripts/run_strategy.py`.
- `generated.cpp`: the strategy as frozen codegen d9b92c7 emits it.
- `provenance.json`: source and tape SHA-256, export channel and range, the
  chart feed's SHA-256 and row window, and the generated file's SHA-256.
- `prediction.json` (the five fix-round controls only): the trades predicted
  before the first export.

CMake compiles every `generated.cpp` into one module,
`tests/callback_lifecycle_tapes.cpp`: one translation unit and one copy of
the library, each strategy in a namespace of its own with its C entry points
prefixed `callback_lifecycle_<tape>__`. `test_callback_lifecycle_tapes` replays every tape
and requires the engine's trades to equal TradingView's row for row: count,
side, quantity (8 decimals), entry and exit time, entry and exit price, both
sides sorted by the same key. PnL is not compared, because TradingView's
report rounds it through a 32-bit float.

`known_divergences.json` lists the tapes whose rule the engine does not
apply yet, each with its reason and the first row where the engine departs.
The test requires exactly that departure, so any movement fails until the
file is updated. Engine main 288f189c departs on 31 of the first 82 tapes;
this change leaves 21 of 87.

## Rules the tapes pin and the engine applies

Each rule has its own switch in `src/compat/pine/callback_lifecycle_rules.hpp`; all are on.
The test clears each in turn and requires exactly the departures
`rule_off_departures.json` records for it, so the rule-to-tape map is pinned
and every switch's off branch runs.

- Trail points to ticks: `trail_points` become
  `ceil(trail_points - 8e-3 * mintick)` ticks (`eur-trailpts-*`,
  `xau-trailpts-*` and `range-breakout-trailpts-*` on minticks 1e-5, 0.001
  and 0.01; off, 4 tapes depart).
- Declined-reversal re-issue: a declined reversal kills the held position's
  stop and limit exits; a later re-issue of one, changed or not, is a new
  live order, whatever the side, sizing or exit kind
  (`range-breakout-may-rev`, `-rev-sl`, `declined-reissue-short`,
  `declined-reissue-stop`; off, those 4 depart). An exit issued once stays
  dead, as round 9 family X pins. Fixed and cash sizing never reach it: their
  unaffordable reversal keeps its close leg (`declined-reissue-fixed`,
  `-cash`).
- Callback limit reach: a limit exit born in a mid-leg
  `calc_on_order_fills` recalculation reaches its leg's end only on that
  waypoint's tick-built print, the level staying raw (`runner-target-r1`,
  `long-high-raw`, `short-low-raw`, `short-limit-plus-quarter`; off, those 4
  and the later rows of `repeated-raw` and `repeated-rounded` depart).

`pair-hold-reissue` runs without a quantity grid, so the engine takes the
entry/close pair-hold path; every quantity in it is whole. TradingView judges
the admitted pair first at the gapped open and the long's stop, re-issued
after `strategy.close` in the pair's calculation, never fills. The test also
reads that stop's book row after the pair's calculation: it must still be
held behind the pair's barrier.

`known_divergences.json` names the rest, among them the
`calc_on_order_fills` tick schedule and ownership queue (`dual-entry-*`,
`coof-dual-*`, `signed-*`) and the resting `calc_on_order_fills` limit on
the tick-built path (`newborn-limit-*`).
