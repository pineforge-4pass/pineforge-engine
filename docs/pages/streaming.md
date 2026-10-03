# Historical to realtime streaming {#streaming}

@tableofcontents

Use the streaming lifecycle when a strategy must warm on confirmed OHLCV and
then continue on normalized ordered trades **without replacing its instance**.
The handoff preserves broker state, position and equity, pending orders, Pine
series and variables, TA objects, `request.security()` evaluators, and a
partially formed higher-timeframe candle.

Stream begin fails when script preparation throws, for both standard and unknown
C++ exceptions. It returns failure, exposes the preparation message through
#strategy_get_last_error, and reports NOT_COMPLETED through
#strategy_last_run_status; no warmup or realtime computation starts. A thrown
legacy setter likewise permanently refuses later begins on that handle. An
already-active stream keeps its begin-time settings until it ends.
After a failed stream begin, recreate the strategy handle before starting again.

The optional **native C++ `pineforge-live` executable** in this repository
uses this lifecycle directly. It includes native normalized-feed transport,
SQLite recovery and order-action webhooks; see the
[native runner guide](https://github.com/pineforge-4pass/pineforge-engine/blob/main/runner/README.md).
Webhooks are optional: use `--webhook-routes` for per-action routing or read
committed actions with `pineforge-live actions --ledger L --after 0 --follow`.
Delivery timing and receiver errors never influence computation.

A host can instead recompute `run_backtest_full` over its accumulated bars
with the [ABI v4 live surface](@ref live_surface); the earlier Python runtime
that did so is retired. Tick-stream fills at observed
prices/times can differ from batch OHLC path assumptions.

## Backtest vs live {#backtest_vs_live}

Backtest and live are modes of one engine. Confirmed-bar streams use the same
modeled OHLC and Pine order policies as backtests; only genuine observed trades
use tick prices instead of the historical OHLC path. The following historical
look-ahead information is unavailable to a continuing live run:

- Opening sibling-stop arbitration can inspect a complete script candle's high
  and low in history. Live aggregation has only the children received so far;
  the opening script-bar seed likewise cannot contain its later children.
- Historical `request.security(..., lookahead_on)` can project a requested
  period's final OHLC before that period closes. Live publishes only information
  that has arrived, not the future period close.
- Historical next-source-bar lookup can inspect a retained future candle.
  Live has no such candle; requested-context feeding also cannot inspect the
  next actual input timestamp before that input arrives.
- A one-bar historical run can accept an undetected timeframe. Live startup
  needs explicit timeframe metadata or enough confirmed timestamps to infer
  cadence; both the adapter fallback and startup validation have this limit.
- History knows the final retained source-bar index. A continuing warmup cannot
  know that eventual index, while realtime `barstate.islast` describes the latest
  available bar rather than hindsight's final bar.
- Historical EOF can seal the final run/session boundary. Live needs an explicit
  end event or a known calendar boundary; provisional last-regular-session-bar
  facts cannot assume an unseen next bar or session closure.

These look-ahead differences are deliberate, like TradingView history versus
realtime. They are not emulated by supplying future data to live execution.
Batch also tolerates gappy historical exports, but a stream refuses a missing
confirmed bar: the feed adapter must recover it from REST or stop, never invent
a candle. `calc_on_order_fills` is refused in streams today because the native
stream's close-only source callback contract does not support fill-triggered
re-entry; this is future source-layer work, not an inherent look-ahead limit.

This is the runtime model used by a continuously running strategy:

1. Call #strategy_stream_begin with every confirmed historical input bar.
2. Feed normalized trades with #strategy_stream_push_tick, or pass one contiguous
   replay tape to #strategy_stream_push_ticks.
3. Call #strategy_stream_advance_time when the provider confirms no earlier
   ticks remain, including quiet periods. A wall clock alone does not prove
   that delayed source records have all arrived.
4. Call #strategy_stream_end, then snapshot cumulative state with
   #strategy_stream_fill_report.
5. Release report arrays with #report_free and the instance with
   #strategy_free.

## Run the tutorial

The repository ships a complete Python `ctypes` example. It uses the frozen
MACD tutorial strategy, warms on the first 640 confirmed candles, and converts
the remaining 32 candles into a deterministic ordered-trade replay:

```bash
bash tutorial/run.sh
python3 tutorial/run_stream.py
```

Expected output has this shape (trade and P&L values follow the frozen data):

```text
MACD historical -> realtime stream
  warmup:     640 confirmed 15m bars
  realtime:   32 bars from 128 ordered trades
  handoff:    2026-05-06 10:15 UTC
  processed:  672 input / 672 script bars
```

The tutorial's OHLC expansion keeps it self-contained. A production service
should normalize any provider payload into #pf_trade_tick_t before calling the
engine. Authentication, transport, symbol mapping, and provider-only metadata
remain outside the runtime.

## C lifecycle

```c
pf_strategy_t strategy = strategy_create(NULL);

if (strategy_stream_begin(strategy, history, history_n, "1", "1") != 0)
    fail(strategy_get_last_error(strategy));

/* A live service normally calls this once for every received trade. */
pf_trade_tick_t tick = {
    .timestamp = 1743206400123LL,
    .sequence = 1234567,
    .price = 1823.45,
    .quantity = 0.125,
};
if (strategy_stream_push_tick(strategy, &tick) != 0)
    fail(strategy_get_last_error(strategy));

/* Confirm elapsed bars even if the market was quiet. */
if (strategy_stream_advance_time(strategy, confirmed_boundary_ms) != 0)
    fail(strategy_get_last_error(strategy));
if (strategy_stream_end(strategy, 0) != 0)
    fail(strategy_get_last_error(strategy));

pf_report_t report = {0};
if (strategy_stream_fill_report(strategy, &report) != 0)
    fail(strategy_get_last_error(strategy));
consume(&report);
report_free(&report);
strategy_free(strategy);
```

Every `strategy_stream_*` call above returns `0` on success and `-1` on failure. Read
#strategy_get_last_error immediately after a failure.

An attempt to begin a stream that is already realtime is rejected without
replaying warmup, ending the active lifecycle, or disabling its action
observations. An overflowing tick-volume accumulation is rejected before
changing the forming bar's OHLCV or consuming the tick's timestamp/sequence.

These validation checks do not make every failure transactional. Errors after
processing starts can leave partially advanced state; discard/recover the
instance according to the input-processing failure contract. A plural tick
call retains repeated single-call semantics: an accepted prefix remains
applied if a later tick fails.

## Tick and bar semantics

- Warmup bars must be strictly increasing, confirmed, and use a fixed-duration
  input timeframe. The first normalized trade belongs at or after the next
  input-bar open.
- Timestamps may be equal but cannot move backwards. Non-zero normalized
  sequence values must increase strictly; use zero if the source has no stable
  ordering key.
- Price must be finite and positive. Quantity must be finite and non-negative;
  quantities accumulate into the forming bar's volume.
- The default Pine strategy cadence remains close-only. Strategy code runs
  when a realtime bar closes, while broker orders resting from an earlier
  calculation evaluate against every normalized trade.
- A market order created at the preceding close fills at the first subsequent
  trade. Stops and limits see the observed trade path, not an inferred OHLC
  traversal.
- #strategy_stream_advance_time materializes elapsed quiet in-session intervals
  as zero-volume carry-forward bars. Intervals outside the configured syminfo
  session are skipped, so closed markets do not acquire synthetic bars.

## One tick versus a contiguous replay

#strategy_stream_push_tick is the natural live-feed API. The plural
#strategy_stream_push_ticks is semantically identical and exists to avoid FFI
overhead during replay. One call can cover an entire memory-mapped tick tape;
it does not end a bar, checkpoint the strategy, or introduce a session
boundary between records.

## Ending and reports

Normally advance to a confirmed boundary and call
`strategy_stream_end(strategy, 0)`. Passing a non-zero
`finalize_partial_input_bar` explicitly treats the currently forming input bar
as complete, which is rarely appropriate for a stopped live service.

#strategy_stream_fill_report may be used after ending to return the cumulative
warmup plus realtime result. The report follows the normal ownership rules in
[Report schema](@ref report_schema).

## Current calculation scope

The first streaming release implements TradingView's default close-only
strategy cadence and raw-trade broker fills. `calc_on_every_tick`,
`calc_on_order_fills`, realtime rollback/`varip`, and alert delivery are
separate surfaces and are not implied by using this lifecycle.

## Known issues {#streaming_known_issues}

- **`request.security` when the input and script timeframes are equal**
  (v1.0.0, v1.0.1). In a stream whose input timeframe equals its script
  timeframe, a strategy that requests its own chart symbol with
  `request.security` (including `ticker.heikinashi` of the chart symbol and
  requests at `timeframe.period`) stops receiving new requested values after
  the first realtime bar, so its live results can differ from a backtest of
  the same bars. Batch backtests are not affected. Workaround: run the stream
  with a script timeframe larger than the input timeframe (for example input
  `1`, script `5`), or use the batch backtest. A fix is in progress.

See [Lifecycle](@ref lifecycle) for handle ownership and
[FFI from Python](@ref ffi_python) for the complete POD mirrors.
