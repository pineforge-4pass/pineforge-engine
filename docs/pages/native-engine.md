# Native engine {#native_engine}

@tableofcontents

Hand-written C++ strategies can run a **standalone native** path: one
`NativeRunSpec`, one working request roster, one physical lot book, and five
host callbacks — `on_native_input`, `on_native_tick`, `on_native_bar_open`, the
pure-virtual `on_native_bar`, and the post-fill `on_native_applied`
(`native_host.hpp:440-452`). The script-bar calculation itself is
`on_native_bar`; the surface is **not** close-only. Pine `strategy.*` commands,
cap/priority adapters, default source sizing, and complete Pine policy
extraction are **not** this surface.
Codegen and source adapters select those policies separately. Resting requests
use the general host commands; request-value members live in
`<pineforge/native_order.hpp>` and are not restated here.

Subclass `pineforge::NativeStrategyHost`. Configure with `configure_native`,
then `run` or `stream_*`. Submit from native begin/bar callbacks, or between
realtime inputs on the same thread. Do not override
the inherited `on_bar` (it is `final` and refused). Do not write protected
engine fields.

Headers: `<pineforge/native_host.hpp>`, `<pineforge/native_run_spec.hpp>`,
`<pineforge/native_fx_curve.hpp>`, `<pineforge/native_order.hpp>`,
`<pineforge/native_calendar.hpp>`,
`<pineforge/market_driver.hpp>`, `<pineforge/order_action.hpp>`,
`<pineforge/execution.hpp>`. Enumeration members live in those headers; this
page does not re-list every enumerator.

## Lifecycle and run identity

`NativeStrategyHost` is noncopyable and nonmovable. The constructor binds the
native consumer; there is no attach/replace switch.

`configure_native(const NativeRunSpec&)` is the only setup. Observation is
`native_state()`. Kinds:

| Kind | Meaning |
| --- | --- |
| `Unconfigured` | Fresh host. |
| `Ready` | Spec staged; not begun. |
| `Running` | `Batch`, `Warmup`, or `Realtime`. |
| `Completed` | `BatchComplete` or `StreamEnded`. Open lots and live requests remain visible, not actionable. |
| `Failed` | Durable first failure. Discard the host. Replay on a **fresh** instance. Do not reconfigure in place. |

`RunIdentity` is `(session_key, run_number)`. `session_key` is nonempty UTF-8
and becomes fixed on first successful begin. `run_number` is a positive
integer. A consumed high-water lives **outside** per-run reset and advances
only at successful begin. Later runs on the **same** host need a strictly
larger number. A **fresh** host has high-water zero, so it may replay the same
logical run.

Configure:

- `Unconfigured` → validate/copy the whole spec atomically → `Ready`, or
  `Failed` with no partial apply.
- `Ready` again → `Failed` (`Contract`). Use a new host to change unconsumed
  setup.
- `Completed` → same session key, `run_number` above high-water → `Ready`.
  Prior-run records stay inspectable until the next successful begin.
- `Running` or `Failed` → refused; `Failed` keeps the first failure.

Begin (`run` / `stream_begin`) is allowed only from `Ready`. Success consumes
the run number, projects the spec onto execution storage, **resets once**
(book, live requests, native events, decision floor), enters `Running`, then
calls `on_native_run_begin()` and bar callbacks. Reset does not clear the
host high-water.

`stream_begin` is one logical begin plus internal warmup, then `Realtime`,
**without** a second public `run` and without resetting lots, requests,
events, ordinals, floor, or identity at the handoff.

`last_error()` is presentation text. `native_state().failure` is the durable
record (`code`, `operation`, optional `ordinal`, `discriminator`, and
allocation-free `context`). Cause/recipient/cursor facts use
`NativeInRunCause` / `NativeInRunRecipient` / `NativeInRunCursor` selected by
`NativeFailureContextKind`; identifiers belong to the failed spec's
`RunIdentity` (`native_failure_context_in_run`, `native_failed_run_identity`).
A foreign run is dropped, not relabeled. Failure copy/move does not allocate.
See `NativeFailureCode` / `NativeFailureOperation` in `native_host.hpp`.

## NativeRunSpec

`NativeRunSpec` defaults are **incomplete**. Empty required strings and zero
capital, point value, or account FX fail validation. There is no
UTC/1-minute/24x7 substitution for a missing native spec.

Required:

- `identity.session_key` nonempty; `identity.run_number` > 0
- `input_tf` and `script_tf` (exact literals; see calendar below)
- `tickerid`
- scheduling `timezone` (must resolve; empty is not UTC)
- `initial_capital`, `point_value`, `account_fx`: finite, strictly positive.
  `price_tick`: finite and nonnegative; zero means raw, unquantized prices.
  `account_fx` is the pre-first-rate fallback; an optional immutable native FX
  curve is staged separately below.

Always set, with documented defaults in the header:

- `ticker`, `type`, `currency`, `basecurrency`, `description`, `volumetype`
  (may be empty UTF-8 except `tickerid`)
- `session`: empty and `"24x7"` are **distinct** all-day literals
- `chart_timezone`: optional observation metadata; empty stays empty and is
  not the scheduling calendar
- `slippage_ticks`: `0` .. `INT_MAX`. Buy adds, sell subtracts
  `ticks * price_tick` **once** to form the kernel default price. The terms
  resolver receives that default; an accepted override is final and is not
  slipped a second time. No mintick snap.
- `fee_kind` / `fee_value`: `Percent`, `CashPerUnit`, `CashPerExecution`;
  `fee_value` finite and ≥ 0. A percent fee is
  `abs(units) × price × point_value × account_fx × fee_value / 100`.
  Cash kinds are account currency per unit or per execution.
- `close_execution`: `NextEligiblePoint` (default) or `AfterCalculation`
- `allowed_open_directions`: `None`, `Long`, `Short`, `Both` (default)
- `report_policy`: `HostRecorded` (default) or `KernelRecorded`;
  `report_open_position_at_end`: `false` (default). See *Reporting for native
  hosts* below.

Optional, absent unless set:

- `quantity_grid`: finite > 0; **admission only**, never resizes quantity
- `max_abs_units`: finite > 0; opening cap on the resulting book
- `max_open_lots`: positive; surviving + new lots
- `initial_margin_fraction`: finite > 0 as a fraction, not a percent. Opening
  admission only; no maintenance liquidation.

`validate_native_run_spec` / `normalize_native_run_spec` report the first
error field. `configure_native` copies a candidate, normalizes it, then stages
it atomically. Admitted numeric `-0` fee becomes `+0`; other literals are not
rewritten.

### Price grid

`price_grid` is an opt-in instrument tick ladder, `None` by default. `None` is
exactly the established behaviour: `price_tick` is only the slippage
multiplier and every booked price is the raw modeled one. The source layer
never sets it, so Pine-compatible runs are unchanged.

- `QuantizeFills`: the kernel rounds the fill basis onto `price_tick`
  **before** slippage, which is itself a whole number of ticks. A limit
  order's protection cap moves to the tick on its own side, so limit-or-better
  survives the grid.
- `QuantizeFillsAndTriggers`: a resting trigger is additionally tested against
  the tick-quantized path instead of the raw one. The order's own level is
  untouched and stays the modeled price a crossing books. This is the generic
  mechanism only; TradingView's exact half-tick rule is a source-layer quirk
  and stays there.
- `grid_rounding`: `HalfUp` (default) is the nearest tick with ties away from
  zero. `Directional` rounds toward the region the order needs — a buy limit
  down, a sell limit up, a stop and a market fill to the adverse side.
- `price_grid != None` requires `price_tick > 0`. A zero tick keeps its
  documented unquantized meaning instead of silently disabling the grid:
  `configure_native` fails with `GridRequiresPriceTick` on field `PriceGrid`.
- The grid is folded into the run-spec hash only when it is set, so a default
  spec keeps its established continuation identity.
- The grid shapes the kernel's default resolved price. A host that overrides
  the price through `resolve_execution_terms` owns that value itself.

Timeframe arguments on `run` / `stream_begin` must be **omitted/empty or
byte-identical** to the spec. Conflicting values are a preflight refusal:
`Ready`/`Running` is preserved. Magnifier/source-feed arguments are not native
spec fields.

The rich `run(bars, n, input_tf, script_tf, inputs, syminfo, overrides, …)`
overload (`engine.hpp:3115-3123`) is **not** refused as a source mutation: it
reaches `NativeExecutionConsumer::run_rich`
(`native_execution_consumer.cpp:4990-5028`), which admits the begin, checks the
timeframe arguments against the spec, preflights and pumps the batch exactly
like the plain overload. `inputs` / `syminfo` / `overrides` are carried only as
`NativeBeginArgs` fields to `prepare_native_begin` — `overrides` as an opaque
pointer (`native_host.hpp:380-387`). No test pins either behaviour; prefer the
plain overload. What *is* refused is source **mutation** through the setters
(below).

## Native requests

From a native callback in `Batch` / `Warmup` / `Realtime`:

```cpp
submit(request);
replace(handle, request);
cancel(handle);
submit_market(request);
replace_market(handle, request);
```

`submit` / `replace` are the complete host commands. `submit_market` /
`replace_market` keep the existing market-only call sites and must reject
nondefault trigger, capacity, owner, or group extras rather than drop them.
Serialized external C++ calls may command only **between realtime inputs**,
never reentrantly during input processing. There is no C request API in this
slice.

`native_order::Request` values belong to `native_order_v5`
(`native_order.hpp:25`); identity types stay `native_order_v1`. Label/comment
remain inert text. The market default path still constructs from:

- `order_action::Transact{signed_units}` — finite nonzero
- `native_order::Reduce{native_order::ExplicitUnits{units}}` — finite positive
- `execution::Flatten{}` — quantity-free whole-book close

General requests also select typed `Market`, `Limit`, `Stop`, `StopLimit` or
`Trail` triggers; `ImmediateRemaining` or per-point `PointBudget` capacity;
and explicit owner/group relationships. Partial executions retain live
remaining quantity. Owner-bound closes use committed opening exposure and
cycle identity through scoped settlement. Group cancellation/reduction is
caused by committed execution events. See the request header for the exact
value types; source-specific Pine lowering remains codegen/adapter work.

At host epoch v17 (`native_host.hpp:18`), general requests also support explicit
`native_order::ReverseTo{signed_units}` and `HostSized`. A `HostSized{Open,
Side}` request binds its units at a matching candidate through the host's
`resolve_execution_terms` override. The host may choose `Transact`, exact
`ReverseTo`, or whole-opposite-book close shape there; it does not supply a
second matcher, book, or cash path. `submit_market` remains the deliberately
narrow market-default convenience surface.

### Selected exposure and current execution (R4-A)

`BindOpenings{{first, last}, cycle}` on a Flatten or explicit-unit Reduce binds
one fixed cohort of already-live opening provenances. All handles must be
unique, nonzero, in this run and physically live in the supplied positive
cycle. Enrollment rejects the entire request if any member is invalid.
Accepted definitions use canonical handle order; execution still closes lots
in physical FIFO order. Later retirement narrows the live subset, while the
immutable definition retains the original cohort. Several fragments of one
opening share its provenance. A new handle with the same label is excluded.

One selected request produces one execution and ticket, even when it closes
several rows. `ExecutionAppliedEvent::scope` uses native `ExecutionScope`
(Book, OpeningExposure, SelectedExposure). Financial `execution::CloseScope`
remains the original two-alternative type. The event's committed row range
identifies actual contributors to a quantity-limited close.

Inside `on_native_bar` or `on_native_applied`, a request accepted or replaced
in that exact callback can be consumed synchronously:

```cpp
native_order::Request request{native_order::Flatten{}, "close cohort", ""};
request.owner = native_order::BindOpenings{{first, last}, cycle};
auto accepted = submit(request);
if (accepted.handle) {
    NativeCurrentExecution command{*accepted.handle, NativeCurrentPriceRule::NearestTick};
    auto preview = inspect_current_execution(command);
    // The adapter may validate its own policy here. Preview is never an apply token.
    if (!preview.refusal && preview.settlement_readiness == execution::Status::Applied) {
        auto outcome = execute_current(command);
        // Applied effects and relationship drains are visible before this statement.
    }
}
```

The command contains only a target and price rule. Membership comes from the
request. It cannot supply another selection, a saved cursor, price, ticket,
or prepared financial plan. Current execution supports Market with
ImmediateRemaining, resolved units, Independent/BindOpening/BindOpenings
ownership and existing groups; current Transact remains Independent.
Priced, budgeted and waiting requests retain queued semantics and receive an
explicit current-execution refusal. Only the chosen target is consumed.

`current_execution_point()` returns an owning presentation of the active
callback's quote and calendar-derived decision context. AsPresented uses that
price; NearestTick uses the existing rounding primitive. Configured directional
slippage applies once. An ordinary execution notification anchors at its
resolved execution price. A current execution notification inherits its cause's
quote, so a chain does not compound slippage. Finite nonpositive current prices
are permitted for pure closes; opening effects and ordinary matching keep their
positive-price restriction. Native opening admission still decides separately.

`NativeCurrentExecutionPreview::settlement_readiness` reports the financial
pre-source preparation boundary. It is independent of the account projection's
status and excludes later counter/lifecycle checks and opening admission.
Refusals leave readiness absent. A HostSized preview can instead report typed
`terms_rejection` or `terms_cancellation`; those are facts about the proposed
terms, not a saved host verdict. Invalid/NoEffect readiness skips source
preflight. Applied readiness requires source preflight even for opening-only
commands whose account projection overflows. Ordered `closed_row_pnl` values
exist only for Applied readiness and use execution's pinned ticket allocation.
Execution revalidates; editing the preview cannot authorize or alter a fill.

The default-empty `on_native_applied(event, context)` notification follows the
Account record and group/owner/dependency drains. Synchronous commands enqueue
notifications FIFO until the outer callback returns and passes projection and
abort checks. Callback depth stays one; event values remain valid during later
submissions. Notifications never automatically execute newborn requests and
have no semantic 64-execution cap. Failure after a physical commit discards the
host; a failed host cannot retry. Configuration projection is checked before
in-callback execution as well as after callback return.

This native slice does not switch generated Pine code to the native consumer
or complete source scheduling, ownership transfer, or parity acceptance.

`on_native_applied` is the **calculate-on-fill** hook: it is the point at which
a host reacts to its own execution and may submit again. A request born there,
mid-bar on a continuous segment, is eligible on the **remaining path suffix** of
that segment — the birth is admitted at the current cursor and the geometric
search then sees only the unconsumed suffix (`born_on_remaining_path`,
`native_execution_consumer.cpp:3253-3257`). Requests accepted before the
segment, and discrete points, keep the ordinary birth gate above.

`on_native_bar_open` fires at the modeled opening, before that point's matching
pass (`native_execution_consumer.cpp:4394-4396`). **Lookahead warning:** the
`Bar` it receives is the *complete* script bar — the consumer has already set
`engine.current_bar_ = bar` (`native_execution_consumer.cpp:4187`) — so its
high, low and close are the finished bar's, not what is known at the open. A
host that must decide on open-only information has to restrict itself to
`bar.open` and its own history. There is no partial-bar view in this slice.

A `quantity_grid`, when present, admits Transact/Reduce quantities on the
exact binary64 grid in `native_order.hpp`. Flatten is not gridded. Rejection
does not rewrite the attempted bits.

**Acceptance is not a fill.** `submit_market` returns `SubmitResult`:

- `Accepted` — timeline ordinal + `RequestHandle` `(session, run, incarnation)`.
  Immutable birth is `(acceptance_ordinal, decision_floor)`. Lots and fees do
  not change.
- `Rejected` — rejection ordinal, no handle (`InvalidQuantity` / `OffGrid`).

Ordinary queued fills appear later as `ExecutionAppliedEvent` on the same command history
(`native_events(0)`). Default market requests retain acceptance/incarnation
priority at an eligible matching driver point. Their birth eligibility is
`point_ordinal > birth.acceptance_ordinal` and
`effective_time_ms >= birth.decision_time_lower_bound` (`point_eligible` in
the header). A request accepted on bar *N* cannot fill on that bar’s already
delivered opening.

Replace is ordinary and causal: validate first; on success retire that live
incarnation and birth a successor (new handle/priority, predecessor link).
`ReplaceRejected` leaves the target live. Cancel live → `Cancelled`. Same-run
absent, replaced, or already terminal → `NotWorking`. Foreign or malformed
handle → `InvalidHandle`. Every outcome is an event. Commands never move
lots or cash.

For independent market requests, Reduce/Flatten use **current** exposure,
not the acceptance-cycle book: a flatten accepted while flat can still close a
same-point opening that already filled. If the book is flat at execution,
Reduce/Flatten terminalize `NoEffectEvent`: no execution identity, no fill,
no fee, no physical action. `MatchRejectedEvent` is an event ordinal without
execution identity (opening direction, max units/lots, initial margin, or
nonpositive resolved price). An opening denial rejects the **entire**
Transact, including a proposed close remainder; it does not close old
exposure first. Closing-only Reduce/Flatten remain allowed while opening
limits are already exceeded.

Query the book with `physical_position()` (`signed_units`, `average_price`,
`lot_count`) and `native_marked_equity(mark)`. `native_events(after_ordinal)`
returns owning snapshots: command, driver, and account rows. Later
commands/reset do not invalidate copies already returned.

A run-start request uses the first provided input's opening time as its initial
decision floor. An empty batch has no market time or price to deliver.
After the first realtime tick or time-advance call, confirmed-bar input is
refused; time advance is likewise refused after confirmed-bar input.

Rows with the same ordinal describe one execution and its account projection.
Process the whole ordinal group before advancing an event-reader cursor.

## Close execution

`NativeCloseExecution::NextEligiblePoint` (default): a request born at a
script calculation waits for a **later eligible matching point** (next modeled
opening, observed print, or carried open). It does not fill on the same bar’s
already presented open/high/low/close.

`NativeCloseExecution::AfterCalculation`: after that calculation, a modeled
close point may match, still obeying birth ordinal/floor. It is not a replay
of observed prints.

## One physical book

There is one engine lot/account book. Native matching inspects settlement,
admits openings against the frozen spec, then commits once. Fees are quoted
on that execution (percent of absolute notional, cash per unit, or one cash
ticket per execution). Slippage is applied to the raw observed/modeled price
as described above. Native account rows follow lots, remaining entry costs,
and realized balance at the matching coordinate.

## Reporting for native hosts

`fill_report` publishes closed trades, diagnostics, an equity curve and the
metrics derived from it. The curve is **host-owned by default**: nothing in
the kernel records a point, so a bare `NativeStrategyHost` that leaves
`report_policy` at `HostRecorded` reports `equity_curve_len == 0`, and every
equity metric (drawdown, run-up, Sharpe/Sortino, CAGR, time in market)
degenerates over that empty series. A host that marks its own equity keeps
this default and owns the whole series.

`NativeReportPolicy::KernelRecorded` asks the consumer to record instead. Once
per script calculation — after the callback returns, and after the
`AfterCalculation` close when that mode is on — it folds the equity extremes
and appends one point labelled with the **script interval's open**, so the
curve is identical with and without an intrabar path. The result is one point
per script bar, a finite drawdown/run-up walk, and metrics computed over a
real series.

`report_open_position_at_end` (`KernelRecorded` only) adds the rows a close of
the still-open position at the last bar's close would record — one per
physical lot, through the same row builder every full close uses, with
`open_at_end` set. The mark is the raw close on the price grid with **no
slippage**: slippage models a market order's fill uncertainty, and this row is
a mark, not an order. It is reporting only. The live position, the pending
orders, the realized sums, the equity curve and the broker state are left
exactly as the run left them; the rows appear in `fill_report` and in
`report_trade_count()` / `get_report_trade()`, never in `closed_trade_count()`
/ `closed_trade()`.

Both fields are opt-in and fold into the continuation hash only once
`report_policy` is non-default, so a spec that does not ask for kernel
recording keeps the continuation identity it had before these fields existed.
Recording does move the broker-state hash, because the equity extremes it
folds are durable engine state.

Per-trade reads: `closed_trade_count()` / `closed_trade(i)` return the closed
rows this run booked; `report_trade_count()` / `get_report_trade(i)` span those
rows followed by the range-end rows, in the order `fill_report` lays them out.

## Calendar, session, timeframes, warmup

Timestamps are **Unix milliseconds**. Confirmed bars require positive finite
OHLC (high ≥ max(open, close), low ≤ min(open, close)) and finite nonnegative volume,
strictly increasing non-overlapping input slots, and a canonical slot label
(`interval.open_ms` or the scheduled clipped `eligible_open_ms`). Supply
**positive** prices: ticks require a positive finite price, and matching
rejects a nonpositive resolved price.

Timeframe literals (spec is the authority; duration is never inferred from
the first two bars):

- seconds `nS` (`"15S"`)
- minutes `"1"`, `"5"`, `"60"`, `"240"` (no `H` suffix; `"1H"` is not a minute)
- days `"D"` / `"nD"`, weeks `"W"` / `"nW"`, months `"M"` / `"nM"`

Count ≥ 1, no leading zeros, no whitespace. `"D"` is not rewritten to `"1D"`.
See `native_calendar.hpp` for parse/pairing enumerators.

Supported pairings: equal-literal passthrough; same unit with script count a
multiple of input; fixed second/minute durations where script ms is a
multiple of input ms; fixed → D/W/M; D/nD → W/M and W/nW → M. Script finer
than input, or indivisible fixed (`"5"` → `"7"`), is refused at setup.
**Batch** admits monthly input (`M` / `nM`) on those pairings. **Stream**
refuses monthly **input** before begin (and the runner refuses it before
binding a ledger). Stream D/W uses civil/session next-open stepping.

Each confirmed input is attributed **wholly** to the script period that
contains its input open (OHLC/volume are not split across a calendar
boundary). Script calculation time is not earlier than the latest included
contributor close. An unfinished trailing coarser script bucket does not
become complete merely because batch input ended.

Session strings: empty or `"24x7"`; comma-separated `HHMM-HHMM` windows;
`2400` as a legal end; overnight wrap; equal start/end as a full day from
that clock; optional `:1234567` mask (`1` = Sunday … `7` = Saturday). The
first declared window start is the civil-cycle origin. Timezone names,
offsets, and POSIX forms are accepted or refused by
`native_calendar::timezone_accepted`; see that header rather than guessing
IANA aliases.

**Batch** may be sparse (missing in-session slots are allowed; no synthetic
prices). **Stream warmup** requires at least one input and every **provided**
input slot to be a complete confirmed interval; missing in-session bars are
refused. An unfinished coarser **script** bucket is retained through
Warmup → Realtime. The handoff does not invent a script callback, a future
exclusive close, or a raised floor to a future seal. Empty batch is a valid
completed run with no events.

## Batch OHLCV vs ticks vs quiet

Two driver models only: confirmed OHLCV and observed ticks. Mixing them on
one stream is refused. The script-bar **calculation** callback is
`on_native_bar`, one per completed script bucket; the opening, tick and
post-fill hooks fire in addition to it, not instead of it.

Confirmed OHLC retains the modeled **Opening**, high/low in the existing
AUTO order, close, calculation and optional AfterCalculation close sequence.
Default market requests match at the modeled opening and eligible
AfterCalculation close; high/low/close traversal updates their excursions.
Resting requests additionally match along the remaining continuous modeled
price segments. The earliest eligible hit is resolved before later hits;
trigger activation, partial fills and newly eligible relationships retain
their causal cursor. Geometric fractions keep the original point's time
and identity rather than inventing intermediate timestamps.

Observed tick: the real price at its timestamp/sequence (**match** /
excursion). Sequence, when nonzero, must increase. Off-session **observed**
prints are still delivered and may fill an eligible live request. Resting
triggers evaluate each actual print without interpolating between prints.

Quiet **tradable** interval with no prints: explicit **carried open** at that
interval’s tradable open, last known price (**match** if eligible), then
calculation if the script bucket completes. Configured **closed** spans
synthesize neither bars nor matching points.

`stream_advance_time(ms)` finishes elapsed intervals and raises the accepted
time floor; it is not an extra print. `stream_end(false)` completes without
inventing a final callback or fill. `stream_end(true)` may partial-finalize
an existing forming observed input; it does not synthesize a missing input
or force an incomplete coarser script bucket complete.

The decision floor is monotonic (delivered event time, accepted input
completion, tick time, time-advance). A refused preflight does not raise it.
Birth compares to that immutable floor, not to a later lowered clock.

## Stream limitations (current)

These are existing refusals, not implied future features:

- Monthly **input** (`M` / `nM`) on `stream_begin` / runner native warmup
- Mixed confirmed bars and ticks
- In-session gaps on stream/warmup
- Source `calc_on_every_tick` / `calc_on_order_fills` enabled (the runner
  rejects an explicit true override, and the Pine host refuses a stream begin
  with `calc_on_order_fills`, `pine_strategy_host.cpp:266-269`). This is a
  limit on the *source* calculation policies, not on the native hooks:
  `on_native_tick` and `on_native_applied` are delivered on a stream.
- A nonempty staged native FX curve on `stream_begin`; batch runs may use one.
- Auxiliary/native security feeds, source magnifier/tail/probe/hash/trace
  setters, `set_input`, and Pine
  entry/exit/cancel commands — native hosts latch `Failed`
  (`UnsupportedSource`) before mutation
- C-level native request submit/replace/cancel

Rebuild strategy libraries against this engine. An ABI-v4 module without the
native contract is legacy and cannot take `--native-config`.

## Errors

| Situation | Host state | What to read |
| --- | --- | --- |
| Invalid spec at configure | `Failed` (`InvalidSpecification`) | `native_state().failure`, `last_error()` |
| Configure while `Ready` / `Running`; session-key change; run number ≤ high-water | `Failed` (`Contract`) | same |
| Timeframe args ≠ spec; unaligned/invalid bars; in-session stream gap; monthly stream input | **Not** `Failed` if still `Ready`/`Running` | `last_error()`; `run` leaves `Ready`; `stream_*` returns `false` |
| Source command/setter | `Failed` (`UnsupportedSource`) | discard host |
| Exception from `on_native_bar` / `on_native_run_begin` | `Failed` (`CallbackException`) | discard host |
| Unexpected settlement/allocation/counter exhaustion after begin | `Failed` | discard host; no rollback promise |
| Submit/replace/cancel outside the allowed phase | throws; not a fill | command only from callbacks or between realtime inputs |
| Cooperative abort | `Failed` (`Aborted`) | discard host |

`last_run_status()` is not failure authority. After `Failed`, later `run` /
`stream_*` / `configure_native` refuse (`native host already failed`).

## C++ example

Complete host: fill every `NativeRunSpec` default that validation requires,
submit `Transact` / `Flatten` from the callback, query `physical_position()`.
Five UTC `24x7` five-minute bars, Unix ms, positive finite OHLC.

```cpp
#include <pineforge/native_host.hpp>

#include <iostream>
#include <variant>

namespace {

class NativeMarketExample final : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1 && physical_position().signed_units == 0.0) {
            submit_market({pineforge::order_action::Transact{1.0}, "native-open", ""});
        } else if (bars_ == 4 && physical_position().signed_units != 0.0) {
            submit_market({pineforge::execution::Flatten{}, "native-flat", ""});
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-guide";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 6.0;
    spec.close_execution = pineforge::NativeCloseExecution::NextEligiblePoint;
    spec.allowed_open_directions = pineforge::NativeOpenDirections::Both;
    return spec;
}

const pineforge::Bar kBars[] = {
    {100.0, 102.0, 99.0, 101.0, 4.0, 0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 104.0, 103.0, 103.5, 4.0, 900000},
    {104.0, 105.0, 103.5, 104.0, 4.0, 1200000},
};
constexpr int kBarCount = 5;

bool failed(const pineforge::NativeStrategyHost& host, const char* where) {
    const auto state = host.native_state();
    if (state.kind == pineforge::NativeLifecycleKind::Failed) {
        std::cerr << where << ": Failed code="
                  << static_cast<int>(state.failure.code)
                  << " op=" << static_cast<int>(state.failure.operation)
                  << " " << host.last_error() << '\n';
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto spec = make_spec();

    NativeMarketExample batch;
    const auto setup = batch.configure_native(spec);
    if (setup.status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << batch.last_error() << '\n';
        return 1;
    }
    batch.run(kBars, kBarCount);
    if (failed(batch, "run()")) return 1;
    if (batch.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run() not completed: " << batch.last_error() << '\n';
        return 1;
    }

    NativeMarketExample batch_tf;
    if (batch_tf.configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
        return 1;
    batch_tf.run(kBars, kBarCount, "5", "5");
    if (failed(batch_tf, "run(tf)") ||
        batch_tf.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run(tf) " << batch_tf.last_error() << '\n';
        return 1;
    }

    bool accepted = false;
    bool filled = false;
    for (const auto& event : batch.native_events(0)) {
        if (!event.command) continue;
        if (std::holds_alternative<pineforge::native_order::AcceptedEvent>(*event.command))
            accepted = true;
        if (std::holds_alternative<pineforge::native_order::ExecutionAppliedEvent>(*event.command))
            filled = true;
    }
    if (!accepted || !filled) {
        std::cerr << "expected AcceptedEvent and ExecutionAppliedEvent\n";
        return 1;
    }
    if (batch.physical_position().signed_units != 0.0) {
        std::cerr << "expected flat book after flatten fill\n";
        return 1;
    }

    NativeMarketExample stream;
    if (stream.configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
        return 1;
    if (!stream.stream_begin(kBars, 1, "5", "5")) {
        std::cerr << "stream_begin: " << stream.last_error() << '\n';
        return 1;
    }
    for (int i = 1; i < kBarCount; ++i) {
        if (!stream.stream_push_bar(kBars[i])) {
            std::cerr << "stream_push_bar: " << stream.last_error() << '\n';
            return 1;
        }
    }
    if (!stream.stream_end()) {
        std::cerr << "stream_end: " << stream.last_error() << '\n';
        return 1;
    }
    if (failed(stream, "stream") ||
        stream.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "stream not completed: " << stream.last_error() << '\n';
        return 1;
    }
    return 0;
}
```

Intended path on this equal-TF feed, `NextEligiblePoint`: callback 1 accepts
`Transact{+1}` (no fill yet); the next bar’s modeled open is the fill;
callback 4 accepts `Flatten` while long; the next open closes. Acceptance
events and `ExecutionAppliedEvent` are separate rows in `native_events`.

Empty timeframe strings are also valid (`run(bars, n)` and
`stream_begin(bars, n, "", "")`). `"5"` / `"5"` must match the spec bytes.

## Higher timeframes for a native host (interim)

There is no native subscription API for `request.security`-style series in this
slice. `set_native_security_feed` (`engine.hpp:3061`) is public but **inert** for
a bare host: it only installs bars, and the routing is built per run from
security evaluators that a native host has no sanctioned way to register —
`configure_security_evaluators` is an empty virtual (`engine.hpp:2346`) and
`prepare_native_security_feeds` is protected (`engine.hpp:2897`), each with a
single caller inside the Pine host. In-run the setter is a source mutation and
**throws**, latching `Failed` (`UnsupportedSource`) via
`guard_native_mutation` (`engine_aux_security.cpp:91`,
`native_execution_consumer.cpp:991-1007`).

The documented interim is **self-aggregation**. `TimeframeAggregator`
(`timeframe.hpp:288`) is public and engine-free; feed it from `on_native_input`,
which is called once per accepted confirmed input bar before that bar is
aggregated or matched (`native_host.hpp:438-440`). Include
`<pineforge/timeframe.hpp>`:

```cpp
class Htf final : public pineforge::NativeStrategyHost {
    pineforge::TimeframeAggregator daily_{"D", "15"};   // script_tf, input_tf
    std::optional<pineforge::Bar> last_daily_;

    void on_native_input(const pineforge::Bar& bar,
                         const pineforge::NativeInputContext&) override {
        const pineforge::AggregatedBar aggregate = daily_.feed(bar);
        if (aggregate.is_complete) last_daily_ = aggregate.bar;
    }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        if (!last_daily_) return;   // confirmed-only HTF value
        // ... submit against last_daily_->close ...
    }
};
```

Only completed buckets are published, so this recipe has no lookahead by
construction. The Pine scheduler uses the same class
(`pine_scheduler_native.cpp:117-128`).

## Terms, reversal, precommit, FX curve

The two const host hooks introduced at v15 remain present at the current v17
host epoch (`native_host.hpp:18`). `resolve_execution_terms` sees read-only
candidate facts and returns a resolved price plus units only for an unresolved
`HostSized` request. Its default is the identity price with no units. A
`NativePrecommitView` is then available to
`validate_execution_precommit` after the ordinary execution is prepared and
before any physical effect; returning `Refuse` records a nonfinancial
`HostPrecommit` rejection. The validator runs once only for an Applied-ready
physical attempt and never during `inspect_current_execution`.

`ReverseTo` names an exact signed target exposure. Its receipt records target
units separately from `filled_working` turnover, so a reversal remains one
ticket and one settlement cycle. A `HostSized{Open}` is sized once; later
candidate rematches may re-resolve price but not size. `CloseOpposite` uses the
existing whole-book Flatten path when it must close an absorbed roster.

### Source-layer boundary (R4-C)

Pine/generated hosts derive from `pineforge::source::PineStrategyHost`, which
derives from `NativeStrategyHost`; handwritten native hosts also derive from
`NativeStrategyHost`. The source adapter/scheduler hash domain is
`pineforge-source-adapter/v2`, while the public C ABI remains version 4.

L3b completes the local ownership switch: the compatibility loop and source
pending-order type are gone, and source commands lower into native requests.
The installed-header check removes `source/` and `compat/pine/`, then compiles
the declared native roots and native examples; its dependency files and `nm`
output are the evidence for this include boundary. It does not establish a
broader policy or runtime-independence claim.

The second runner module is a deliberately small example of those public
seams. It contains no Pine command calls, formula, or protected engine write:

```cpp
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <optional>
#include <variant>

namespace {
namespace no = pineforge::native_order;

class NativeSelectedExample final : public pineforge::NativeStrategyHost {
    std::optional<no::RequestHandle> opening_;
    std::optional<no::RequestHandle> reversal_;
    std::optional<std::int64_t> opening_cycle_;
    int bars_ = 0;
    bool child_submitted_ = false;

    no::ExecutionTerms resolve_execution_terms(
            const pineforge::NativeExecutionTermsFacts& facts) const override {
        if (std::holds_alternative<no::RemainingDeferred>(facts.remaining)) {
            return {facts.default_resolved_price, 2.0, no::OpeningShape::Transact};
        }
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }

    pineforge::NativePrecommitVerdict validate_execution_precommit(
            const pineforge::NativePrecommitView&) const override {
        return pineforge::NativePrecommitVerdict::Proceed;
    }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit(no::Request{no::Transact{1.0}, "baseline-open", ""});
            no::Request open;
            open.intent = no::HostSized{no::HostSizedKind::Open, no::Side::Long};
            open.label = "sized-limit";
            open.trigger = no::Limit{101.0};
            opening_ = submit(open).handle;
            return;
        }
        if (!child_submitted_ && opening_ && opening_cycle_) {
            no::Request child;
            child.intent = no::Reduce{no::OwnerOpenedUnits{}};
            child.label = "bound-stop";
            child.trigger = no::Stop{99.0};
            child.owner = no::BindOpening{*opening_, *opening_cycle_};
            submit(child);
            child_submitted_ = true;
        }
        if (bars_ == 3 && opening_ && opening_cycle_) {
            no::Request selected{no::Flatten{}, "selected-flatten", ""};
            selected.owner = no::BindOpenings{{*opening_}, *opening_cycle_};
            const auto accepted = submit(selected);
            if (accepted.handle) {
                const pineforge::NativeCurrentExecution current{
                    *accepted.handle, pineforge::NativeCurrentPriceRule::NearestTick};
                const auto preview = inspect_current_execution(current);
                if (!preview.refusal &&
                    preview.settlement_readiness == pineforge::execution::Status::Applied) {
                    execute_current(current);
                }
            }
        }
        if (bars_ == 4 && !reversal_) {
            no::Request reverse;
            reverse.intent = no::ReverseTo{-1.0};
            reverse.label = "exact-reverse";
            reversal_ = submit(reverse).handle;
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (opening_ && event.handle() == *opening_) opening_cycle_ = event.cycle_after;
    }
};

}  // namespace
```

This is the shape of
`runner/examples/native_selected_strategy.cpp`. The accompanying runner test
executes it once in batch and once through `stream_begin` / `stream_push_bar` /
`stream_end`; its selected close uses the current-point preview only as a
readiness observation before `execute_current`.

An immutable `NativeFxCurve` is staged only while the host is Ready, after
`configure_native` and before a batch `run`. Its parallel timestamp/rate arrays
must have equal length, strictly increasing timestamps, and finite positive
rates; an empty curve clears the staged value. `account_fx` remains the
fallback before the first curve point.

```cpp
pineforge::NativeFxCurve curve{{0, 900000}, {1.0, 1.01}};
const auto fx = host.configure_native_fx_curve(curve);
if (fx.status != pineforge::NativeSetupStatus::Applied) {
    // Read fx.validation; engine storage was not changed.
}
```

### Known limits

The test-only Pine oracle is a comparison aid and earns no native-independence
or adapter credit. There is no generic native FX broker-open epoch clock in
this slice: the O7 clock work is deferred to slice B. Native streaming refuses
a nonempty staged FX curve. The precommit verdict is not previewed; preview
terms outcomes are typed facts, not the host's verdict. Generated Pine code
remains on its compatibility route until the later adapter slice.

### What still requires Pine compatibility to build

The standalone native host has no Pine decision path at runtime, and the
constructor/member cut has since landed: `engine.hpp` has **zero** references to
`CapAttachment`, `OrderPriority` or `IntradayCap`. `NativeStrategyHost` is
zero-argument (`native_host.hpp:427`); the `CapAttachment` constructor belongs
to `source::PineStrategyHost` (`pine_strategy_host.hpp:21-25`), and the cap type
itself lives in the adapter (`intraday_cap.hpp:18`).

What remains is a **build**-level dependency, not a header or object one: the
root CMake target is one static library that always appends
`PINEFORGE_SOURCE_LAYER_SOURCES` (`CMakeLists.txt:81-134`), and one adapter
unit, `src/compat/pine/market_admission.cpp`, is still listed outside that set
(`CMakeLists.txt:111`). There is no kernel-only target yet. The installed-header
closure is already clean, which the independence checker proves
(`check_native_include_independence.py:36-46`). See
`docs/adr/0001-kernel-adapter-boundary.md`.

### Building the kernel only

`-DPINEFORGE_BUILD_SOURCE_LAYER=OFF` (default **ON**) configures a build with
no source-adapter translation unit compiled at all:

```bash
cmake -S . -B build-kernel -DCMAKE_BUILD_TYPE=Release \
    -DPINEFORGE_BUILD_SOURCE_LAYER=OFF
cmake --build build-kernel -j
```

The default build is unchanged: `libpineforge.a` still contains every kernel
object plus the source layer, and `PineForge::pineforge` still links a Pine
host. What the option adds is the kernel target itself, which exists in both
builds:

```cmake
find_package(PineForge REQUIRED)
target_link_libraries(my_native_consumer PRIVATE PineForge::kernel)
```

`PineForge::kernel` (`libpineforge_kernel.a`) is built from
`PINEFORGE_KERNEL_SOURCES` with exactly the flags `pineforge` uses. Its only
mention of the source layer is the opaque `pineforge::source::StrategyOverrides
const*` forward declaration in the rich begin bridge, so it links standalone;
`scripts/check_native_include_independence.py --kernel-archive` asserts that
with `nm` over the archive's defined and undefined symbols.

With the option OFF the build excludes, each with a CMake STATUS line:

- `src/source/*.cpp` and `src/compat/pine/*.cpp`, so `libpineforge.a` holds
  the kernel objects alone (equivalent to `libpineforge_kernel.a`);
- the installed `include/pineforge/source/` and `include/pineforge/compat/`
  headers, which would otherwise declare functions with no definition;
- every Pine-bound target: the corpus and bench strategies, the tutorial
  strategy, and the live runner's `examples/strategy.cpp`
  (`native-market-example` and `native-selected-example` still build);
- every test translation unit that reaches a `pineforge/source/` or
  `compat/pine/` header, and the receipt-backed ABI rows whose pairing TU
  derives from `PineStrategyHost`. The remaining CTest rows all run.

`python3 scripts/ci_verify.py kernel` is the profile that verifies this lane
(Release, live runner ON, tutorial OFF, source layer OFF); CI runs it as the
`kernel-only` job.

## Runner JSON and command

`pineforge-live` is optional (`-DPINEFORGE_BUILD_LIVE_RUNNER=ON`, default
**OFF**). That option also builds `native-market-example` and
`native-selected-example`. Native modules
need `--native-config FILE` instead of `--input` / `--override` / `--syminfo`
(those flags are refused). The file is a strict JSON object: unknown and
duplicate keys fail; every listed key is required; there are **no**
environment defaults for spec fields. Omitted CLI clock/symbol flags take the
file. Explicit `--input-tf`, `--script-tf`, `--timezone`, `--session`,
`--chart-timezone`, or `--symbol` must equal the file.

`input_mode` is the CLI `--mode` (`bars` or `ticks`), not a JSON key.
Timestamps in the spec, warmup CSV, and JSONL are Unix milliseconds.

Complete file (optional numeric payloads are JSON `null`, not omitted keys).
`fee_kind` and `close_execution` are the C++ enumerator **names**.
`allowed_open_directions` is the numeric mask (`3` = `Both`; see
`NativeOpenDirections`).

```json
{
  "run": {
    "session_key": "native-guide",
    "run_number": 1
  },
  "clock": {
    "input_tf": "5",
    "script_tf": "5",
    "timezone": "UTC",
    "session": "24x7",
    "chart_timezone": ""
  },
  "instrument": {
    "ticker": "MOCK",
    "tickerid": "TEST:MOCK",
    "type": "crypto",
    "currency": "USDT",
    "basecurrency": "ETH",
    "description": "",
    "volumetype": ""
  },
  "execution": {
    "initial_capital": 10000,
    "point_value": 1,
    "account_fx": 1,
    "price_tick": 0.01,
    "slippage_ticks": 0,
    "fee_kind": "CashPerExecution",
    "fee_value": 6,
    "quantity_grid": null,
    "close_execution": "NextEligiblePoint",
    "max_abs_units": null,
    "max_open_lots": null,
    "allowed_open_directions": 3,
    "initial_margin_fraction": null
  }
}
```

Save the configuration as `native-run.json`. Warmup uses the configured
calendar. For this example, save one confirmed bar as `history-5m.csv`:

```csv
timestamp,open,high,low,close,volume
0,100,102,99,101,4
```

Save these subsequent bars as `events.jsonl`. The first callback occurs
during warmup; the opening and flatten orders fill on subsequent inputs.

```jsonl
{"type":"bar","bar":{"ts_open":300000,"o":102,"h":103,"l":101,"c":102,"v":4}}
{"type":"bar","bar":{"ts_open":600000,"o":103,"h":104,"l":102,"c":103.5,"v":4}}
{"type":"bar","bar":{"ts_open":900000,"o":103.5,"h":104,"l":103,"c":103.5,"v":4}}
{"type":"bar","bar":{"ts_open":1200000,"o":104,"h":105,"l":103.5,"c":104,"v":4}}
```

```sh
cmake -S . -B build-live \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_LIVE_RUNNER=ON
cmake --build build-live -j --target pineforge-live native_market_example

build-live/bin/pineforge-live run \
  --strategy build-live/lib/native-market-example.so \
  --native-config native-run.json \
  --warmup history-5m.csv \
  --feed events.jsonl \
  --mode bars \
  --ledger orders.sqlite3 \
  --webhook-url https://receiver.example/order-actions \
  --name native-market
```

`--mode ticks` is the other input mode. Tick JSONL uses `ts` (Unix ms),
positive `seq`, `price`, `qty`. Confirmed-bar JSONL uses
`bar.ts_open` (Unix ms) plus `o,h,l,c,v`. `{"type":"time","ts":…}` is tick
mode only. Mixing modes is refused. Monthly `input_tf` is refused on this
path before the process continues into the run.

Other CMake options (unchanged defaults): `PINEFORGE_BUILD_TESTS` ON,
`PINEFORGE_BUILD_TUTORIAL` ON, `PINEFORGE_BUILD_EXAMPLES` OFF,
`PINEFORGE_ENABLE_SANITIZERS` OFF, `PINEFORGE_STRICT_WARNINGS` OFF. The
runner also needs SQLite3, libcurl 7.86+, and OpenSSL Crypto.

The C ABI query `strategy_execution_contract` identifies `NativeMarketV1`; the
runner then uses `strategy_configure_native_v1` to apply its versioned run
specification. `strategy_configure_native_fx_curve_v1` additionally stages an
immutable curve on a Ready native handle (or clears it with `n == 0`); invalid
pointer/size/phase/curve input returns `-1` without mutation. The legacy
`strategy_set_account_currency_fx_series` remains unavailable on a native
handle. Prefer the C++ `NativeRunSpec` and `NativeFxCurve` shown here over
hand-maintaining either versioned C struct.

For enumerator payloads, pairing names, session grammar, and failure codes,
read the headers cited above.
