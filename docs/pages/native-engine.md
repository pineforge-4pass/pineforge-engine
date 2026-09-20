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
`<pineforge/native_module.hpp>` is separate: it is needed only to export a host
as a loadable module (see @ref native_engine_examples).

Coming from PineScript? **[PineScript to native C++](@ref pine_to_native)** maps
each `strategy.*` concept to its native counterpart
(@ref pine_to_native_map), migrates a small strategy end to end
(@ref pine_to_native_worked), and states how to diff a native port against its
Pine twin (@ref pine_to_native_parity).

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

`native_continuation_hash()` is a **local** identity: it folds the resolved
timezone identity of the run, whose `zoneinfo_root` and zone file paths are
absolute paths on the machine that ran it (`/usr/share/zoneinfo` on a glibc
host, a tzdata-versioned path such as
`/private/var/db/timezone/tz/2026c.1.0/zoneinfo` on macOS), so the same spec
over the same bars hashes differently on two machines even for `"UTC"`. Compare
it between runs in one process — to prove that stating a field at its default
changes nothing, or that opting in moves the identity — and never pin it as a
constant. For a portable constant use `native_run_spec_digest(spec)`
(`native_run_spec.hpp`): exactly the fields the consumer folds into the
continuation identity for a run spec and nothing else, seeded as the consumer
seeds them, so two specs with equal digests drive identical continuation
identities on every machine with the same timezone resources. It is not a
continuation hash and is never comparable with one.

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
- `calculation`: `BarClose` (default), `BarCloseAndFills` or
  `EveryModeledPoint`; `max_recalculations_per_point`: `8` (default, any
  value including 0 is legal); `open_bar_view`: `Complete` (default) or
  `OpenOnly`. See *Calculation timing* below.

Optional, absent unless set:

- `quantity_grid`: finite > 0; **admission only**, never resizes quantity
- `max_abs_units`: finite > 0; opening cap on the resulting book
- `max_open_lots`: positive; surviving + new lots
- `initial_margin_fraction`: finite > 0 as a fraction, not a percent. Opening
  admission only; no maintenance liquidation.
- `margin`: the generic per-side broker margin model. Mutually exclusive with
  `initial_margin_fraction` (setting both is `MarginModelConflict`). See
  *Margin and liquidation* below.
- `subscriptions`: declared higher-timeframe series of the run's own symbol.
  Empty is the whole default surface; see "Higher-timeframe series for native
  hosts" below.

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

`native_order::Request` values belong to `native_order_v6`
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

### Brackets and trailing

A live request is visible without replaying the event log:

```cpp
for (const auto& row : native_working_requests()) {
    row.definition->request;   // the accepted request
    row.remaining;             // what is left to execute
    row.trigger_state;         // where its own trigger has reached
}
std::size_t ended = cancel_all();              // one CancelledEvent per request
std::size_t dropped = cancel_where("bracket"); // by the request's comment
```

`native_working_requests()` copies owning value rows in live order; later
commands do not invalidate a row already returned. `cancel_all()` answers how
many requests left the book, dependants of a cancelled owner included;
`cancel_where(comment)` cancels exactly the live requests carrying that
comment and answers how many of those it cancelled. An unknown comment is not
a command.

A leg can be placed before its owner has a price. `Request::anchor` defaults
to `Absolute{}` — the level written in the trigger. `FromOwnerFill{offset,
ticks}` instead defers it: when the owner's fill arms the request, the level
becomes `fill + offset`, with `offset` signed (adverse is negative) and
spelled in price ticks when `ticks` is set. The anchor applies to `Limit` and
`Stop` prices and to a `Trail` arm threshold; the anchored field carries the
placeholder `0.0` until then, and `Market`, `StopLimit`, and any owner other
than `WaitForApplied` are rejected, because only that relation arms. The
`ArmedEvent` carries the materialized definition: from then on the request
reads as the absolute level it now is.

```cpp
native_order::Request stop{native_order::Reduce{native_order::OwnerOpenedUnits{}}, "sl", ""};
stop.owner = native_order::WaitForApplied{entry};
stop.trigger = native_order::Stop{0.0};
stop.anchor = native_order::FromOwnerFill{-10.0, /*ticks=*/true};  // ten ticks under the fill
```

Trailing offsets accept the same two spellings. `Trail::ticks` is a
`TrailTicks{n}` offset resolved against the run's price tick at acceptance:
the accepted request carries the resolved price distance and no spelling, so
a tick-spelled trail and its price-spelled equal behave identically bar for
bar. A zero offset is legal and means *ride the best*: the level is the
running best itself and the exit is the first move strictly past it. Negative
and nonfinite offsets remain rejected, and a tick spelling without a usable
price tick is rejected rather than read as a price.

Re-pricing a trail normally restarts it. `replace(handle, request,
ReplaceOptions{/*retain_trigger_state=*/true})` instead carries the
predecessor's live trigger state — a tracking trail's best, an already active
stop — into the successor. Predecessor and successor must hold the same
trigger alternative, and a retained best must still produce a representable
level for the successor's offset; otherwise the replacement is rejected and
the predecessor stays live.

### From strategy.exit to submit_bracket

`include/pineforge/native_toolkit.hpp` is header-only, Pine-free and additive:
it composes the primitives above and adds no kernel behaviour. A Pine
`strategy.exit("x", from_entry="e", limit=…, stop=…, trail_points=…)` maps
onto one call:

```cpp
#include <pineforge/native_toolkit.hpp>
namespace tk = pineforge::native_toolkit;

tk::BracketSpec bracket;
bracket.parent = entry;                    // the handle strategy.entry returned
bracket.take_profit = take_profit_request;  // limit leg
bracket.stop_loss = stop_loss_request;      // stop leg
bracket.trail = trail_request;              // optional trailing leg
const tk::BracketReceipt legs = tk::submit_bracket(*this, bracket);
```

Each present leg is submitted as the parent's `WaitForApplied` child in one
OCA group — group id the parent's incarnation, one cohort per leg, the
caller's `sibling_effect` (default `Cancel`) — in take-profit, stop-loss,
trail order. That is exactly the owner/group shape a host would write by
hand, and the legs' own intent, trigger and anchor are left untouched. The
receipt reports the handle each leg was accepted under; a rejected leg stays
empty and the others are still submitted.

`tk::OrderBook<Key>` is the id bookkeeping a strategy would otherwise write
itself: `submit_or_replace(key, request)` re-prices the key's own request
while it is still working and submits a fresh one when it is gone,
`cancel(key)` ends it and forgets the key, and `handle(key)` reports the
current handle. An unknown key never reaches the host.

## Close execution

`NativeCloseExecution::NextEligiblePoint` (default): a request born at a
script calculation waits for a **later eligible matching point** (next modeled
opening, observed print, or carried open). It does not fill on the same bar’s
already presented open/high/low/close.

`NativeCloseExecution::AfterCalculation`: after that calculation, a modeled
close point may match, still obeying birth ordinal/floor. It is not a replay
of observed prints.

## Margin and liquidation

`NativeRunSpec::margin` is the whole generic broker margin model, and it is
opt-in. A spec that leaves it unset keeps `initial_margin_fraction`'s
one-scalar opening gate, never enters a liquidation path, and folds nothing
new into the continuation digest. The two spellings are mutually exclusive:
declaring both fails validation with `MarginModelConflict`.

```cpp
NativeMarginModel margin;
margin.initial_long   = 0.5;   // fractions, not percents; both > 0
margin.initial_short  = 0.5;
margin.maintenance_long  = 0.25;   // absent = that side never liquidates
margin.maintenance_short = 0.25;
margin.sizing = NativeLiquidationSizing::RestoreMinimum;
margin.shortfall_multiple = 1.0;           // used by ShortfallMultiple
margin.liquidation_min_units = 1.0;        // optional broker minimum trade
margin.check = NativeLiquidationCheck::PathAdverseExtreme;
spec.margin = margin;
```

**Opening admission.** With a model set, `initial_long` / `initial_short`
replace the single scalar for that run: an opening is refused with
`MatchRejectReason::InitialMargin` when
`resulting_abs_notional × initial_<side> > marked_equity − ticket`. A host
that answers `AdmitWithHostMargin` from `validate_execution_precommit` still
takes that one check over, exactly as before.

**The liquidation level.** With a maintenance fraction for the live side, the
kernel solves the one price where the marked equity meets the maintenance
requirement:

```
equity(P)   = capital + realized − open entry fees + dir × (P × Q − Σ qty×price) × pv × fx
required(P) = Q × P × pv × fx × maintenance
L           : equity(L) == required(L)
```

Both sides are affine in `P`, so `L` is unique unless the slopes coincide —
`maintenance == 1.0` on a LONG, where equity and requirement move together and
no price solves the breach. `NativeStrategyHost::native_liquidation_price()`
answers `L`, or `nullopt` when the run declares no model, the side has no
maintenance fraction, the book is flat, or no finite price solves it. It is
the exact level: no tick rounding, which is a source-layer spelling.

**Arming (`PathAdverseExtreme`, the default).** At every script-bar open and
after every applied fill, the kernel measures the requirement against the most
adverse price the modeled script path still reaches after the current
waypoint — the same sizing mark a whole-bar broker check would use. If that
mark breaches, the kernel rests its own `Reduce` (or `Flatten`) with
`Stop{L}`, bound to the live book. The reduction therefore *fills at the
liquidation level*, where the account actually runs out of margin, while it is
*sized at* the adverse mark. With a declared `IntrabarPath` there is no
whole-bar waypoint model: the kernel re-evaluates at each delivered sample.

The units come from `sizing`:

| `sizing` | units |
| --- | --- |
| `RestoreMinimum` | `(required(mark) − equity(mark)) / (mark × pv × fx × maintenance)` — the fewest units that restore the requirement |
| `ShortfallMultiple` | that restore × `shortfall_multiple` |
| `Flatten` | the whole position |

The result is capped at the held quantity, floored onto `quantity_grid` when
one is configured, and replaced by a full flatten when `liquidation_min_units`
is set and the computed slice falls below it. A host override of
`resolve_margin_call_units(const NativeMarginCallView&)` has the last word and
is clamped into `(0, held]`.

**Re-pricing.** Exactly one kernel liquidation is live at a time. When the
level or the units move — typically after a host fill changes the book — the
previous one is withdrawn with `CancelReason::Superseded` before the new one
is accepted. A book that is flat or no longer breached withdraws it outright.

**`CalculationOnly`.** The kernel rests nothing and tests the requirement only
at a script calculation point, against that bar's close. A breach there is
liquidated immediately as a current execution. Nothing fills mid-path.

**Events and hooks.** A kernel-issued request carries
`RequestDefinition::origin == RequestOrigin::KernelLiquidation`; every host
request is `RequestOrigin::Host`. When it fills, a `MarginCallEvent` joins the
command history directly after its own `ExecutionAppliedEvent`, carrying the
mark, the marked equity and requirement at that mark, the solved liquidation
price, the filled units and the signed book on either side. The host sees
`on_native_applied` first and `on_native_margin_call` immediately after, with
the same cursor.

The TradingView margin call is **not** this model: its rounded-money rules,
its 4× shortfall default, its one-contract long money call and its
adverse-extreme fill pricing stay in the Pine adapter, which never sets
`margin`.

## Calculation timing

`NativeRunSpec::calculation` decides when the kernel asks the host to
calculate. It is `NativeCalculationTrigger::BarClose` by default, which is the
established surface exactly: one calculation per script bar, at its close. The
source layer never sets this field, so Pine-compatible runs are unchanged, and
the spec folds the block into the continuation hash only once the trigger or
the open-bar view is non-default.

Every calculation — including the script bar's own — is delivered through

```cpp
virtual void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                                   NativeCalculationReason reason,
                                   const native_order::ExecutionAppliedEvent* cause);
```

whose default forwards to `on_native_bar(bar, ctx)`. A host that implements
only `on_native_bar` therefore sees precisely what it saw before this field
existed. `NativeCalculationReason` is `BarClose` (the script bar's own
calculation, `cause == nullptr`), `OrderFill` (one recalculation at an applied
execution's cursor, `cause` being that event, valid only for the call),
`Tick` (one recalculation at a modeled point or an observed print) and
`SubBar`, which is reserved: a lower-timeframe sub-bar has its own hook and is
never delivered through `on_native_recalculate`.

The triggers are a strict superset chain, so opting in never removes a
calculation:

- `BarClose` (default): the script bar's calculation only.
- `BarCloseAndFills`: additionally one `OrderFill` recalculation per applied
  execution (Pine's `calc_on_order_fills`, without its TradingView specifics).
- `EveryModeledPoint`: additionally one `Tick` recalculation at every modeled
  point of the delivered path — each confirmed OHLC waypoint, each intrabar
  sample — and at every observed print, in batch and in a stream alike
  (Pine's `calc_on_every_tick`, generically).

### Chronology contract

At **one** point, in this order and no other:

1. Match and settle.
2. `on_native_applied` for each applied event, FIFO, through the existing
   notification drain under its re-entrancy guard.
3. With `BarCloseAndFills` or `EveryModeledPoint`, one `OrderFill`
   recalculation at that event's own cursor, immediately after its
   `on_native_applied`, driven from the same drain. It is bounded by
   `max_recalculations_per_point` (default 8) **per matching point**:
   executions a callback drives through `execute_current` land at the same
   cursor and spend the same budget, so a host that refills on its own fill
   cannot cascade without end. Beyond the bound the execution is still
   applied and still delivered to `on_native_applied`; only the calculation
   it would have driven is dropped. `native_recalculation_count()` and
   `native_recalculations_skipped()` report both totals.
4. With `EveryModeledPoint`, one `Tick` recalculation after the point's
   matching is finished. `on_native_tick` stays the observation hook and
   still runs **before** the print is matched.

A request born in any of these callbacks follows the existing birth rule
unchanged: it is eligible on the unconsumed rest of the bar, which for a
market request means the next discrete matching point. Nothing about the
drain order, the birth rule or language-state rollback moves — the kernel
never attempts rollback; that stays a source-layer concern.

A recalculation records **no** report point. Under
`NativeReportPolicy::KernelRecorded` the equity curve still has exactly one
point per script bar whatever the trigger is.

### Sub-bars

```cpp
virtual void on_native_sub_bar(const Bar& sub, const NativeDecisionContext& ctx);
```

fires once after each retained lower-timeframe sub-bar's whole matching path,
before the next sub-bar's. It is not a cadence: it fires whatever
`calculation` is. It requires a real lower feed
(`IntrabarPath::lower_tf`) — a synthesized path and a plain confirmed bar have
no sub-bars of their own, so it never fires for them. The decision point is
the sub-bar's last modeled point, so commands and `execute_current` are legal
and a request born there follows the ordinary birth rule.

### The bar so far, and the open-bar view

```cpp
std::optional<Bar> current_partial_bar() const;   // non-virtual
```

answers the lookahead-free bar so far at the current cursor: the open of this
script bar's first modeled point, the running high/low, and the close at the
cursor. `volume` accrues only activity actually consumed — the completed
lower-timeframe sub-bars of an intrabar path, or the observed prints of a
stream — and stays 0 for a modeled path that carries no intrabar volume of its
own. It is valid in the bar-open, applied, tick, sub-bar and recalculation
callbacks, and is `nullopt` outside a path walk, including in the bar's own
close calculation, where the host already holds the complete bar.

This matters because the mid-bar callbacks are handed the **complete** script
bar: `on_native_bar_open` receives the whole bar by default, and so does every
`OrderFill` / batch `Tick` recalculation. That is deliberate — a host that
schedules against the bar's own high/low needs it, and the adapter relies on
it — but it is lookahead. `current_partial_bar()` is the answer for a host
that must not see it.

`NativeRunSpec::open_bar_view` masks exactly one callback:
`NativeOpenBarView::OpenOnly` hands `on_native_bar_open` (and `current_bar_`
while it runs) `H = L = C = open` and volume 0. `Complete` (default) is
unchanged. The mask is presentation only: the complete bar is restored before
the open match, so matching, fills, excursions and every later callback are
byte-for-byte what `Complete` books.

### What stays in the source layer

TradingView's COOF specifics are **not** reproduced here (design ruling R5-5):
the waypoint-only refill, the two-fills-at-open rule, the script-state
snapshot/restore and the adapter's own cascade guard and deferral queue all
remain in `src/source`. A native `BarCloseAndFills` host running the adapter's
refill rule reaches the same book with the same order ids in the same order,
but the coordinates a request born in a recalculation fills at differ: the
adapter re-presents it at the chart bar's next waypoint, the kernel at the
next discrete matching point of the delivered path.

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

## Higher-timeframe series for native hosts

A bare `NativeStrategyHost` reads a coarser series of its own symbol — Pine's
`request.security(syminfo.tickerid, tf, …)` — by declaring it in the run spec.
The kernel aggregates the accepted input into the declared buckets; there is
no source layer and no Pine expression.

```cpp
NativeTimeframeSubscription hourly;
hourly.tf = "60";                    // pairs with input_tf like script_tf does
hourly.lookahead = false;            // barmerge.lookahead_off (the default)
// hourly.authoritative_bars = …;    // optional exchange bars, see below
spec.subscriptions.push_back(hourly);

class Host : public NativeStrategyHost {
    void on_native_timeframe_bar(const Bar& bucket,
                                 const NativeTimeframeBarContext& context) override {
        // context.subscription indexes spec.subscriptions
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (auto hour = native_series_bar(0)) { /* latest delivered bucket */ }
    }
};
```

`subscriptions` is empty by default and every part of this section is inert
for a spec that declares none: no evaluator is registered, no feed is
prepared, `on_native_timeframe_bar` is never called, and the run's
continuation hash is the pre-subscription one.

**Validation (at `configure_native`).** Each `tf` must parse and must pair
with `input_tf` exactly as `script_tf` does. Named refusals:

| `NativeRunSpecError` | Cause |
| --- | --- |
| `InvalidSubscriptionTimeframe` | unparseable literal, or a pairing `script_tf` would not accept |
| `SubscriptionFinerThanInput` | strictly finer than `input_tf` — a lower-timeframe array is a different contract and is not promoted |
| `DuplicateSubscriptionTimeframe` | two series of the same period (the feed store is keyed by duration, and every monthly literal is one period) |
| `UnorderedSubscriptionBars` | `authoritative_bars` not strictly increasing in time |
| `SubscriptionWithoutTimeframe` | declared together with `timeframe_undetected` |

**Delivery.** A bucket is delivered on an accepted input bar, before that
input is aggregated, matched or calculated — so `on_native_input` precedes it
and the input's `on_native_bar` follows it.

- `lookahead = false` (Pine's `barmerge.lookahead_off`): the bucket is
  delivered when its **last** contributing input bar is accepted, never
  earlier.
- `lookahead = true` (`barmerge.lookahead_on`): the completed bucket's final
  OHLCV is delivered at its **first** contributing input bar, and
  `native_series_bar` answers with it from then on.

`NativeTimeframeBarContext::completion` is `Confirmed` when the bucket closed
on its own last contributing bar and `LazyComplete` when the next period's
first input closed it. `interval` is the calendar span of the bucket's first
contributing input bar; `delivered_at_ms` is the input bar the delivery rides
on. A bucket still open at the end of the input is never delivered.

**Authoritative bars.** `authoritative_bars` are the exchange's own bars of
that timeframe. A completed bucket takes its OHLCV from the bar keyed to the
same period; the aggregator still decides *when* the bucket completes.
`native_security_substitutions()` and `native_security_misses()` report how
many completed buckets took one and how many found none.

**Inherited TradingView calibration.** The feed store these bars go into is
TradingView-calibrated, and a host that supplies them inherits its rules:

- A `"W"` / `"M"` series with no feed of its own is built from the installed
  **daily** bars of the same run — first session's daily open, the daily
  extremes, last session's daily close, summed volume — not from a
  re-aggregation of the intraday input.
- An installed feed's stamps become that series' period partition. A session
  with no stamp of its own (an exchange holiday session that pauses and
  reopens the same day) therefore folds into the **next trade date's** bar,
  and a data hole inside a stamped period is not a close.
- A period the supplied bars only partly cover yields a partial bucket,
  exactly as a partly covered chart would.

Declare no `authoritative_bars` and the buckets are a plain aggregation of the
run's own input, with no calibration to inherit.

**Streams.** `stream_begin` accepts a non-empty `subscriptions`, so a
forward-execution host reads the same series a backtest of the same bars
reads. The warmup resolves the series exactly as a `run()` over those same
warmup bars does — the same buckets, at the same delivery points, in the same
callback order — and each pushed live bar then extends the bucket the warmup
left open, delivering it before the calculation of the bar that completed it.
Live specifics:

- `lookahead = true` changes the **warmup** only. Pine's lookahead is a
  historical-resolution mode, and a realtime bar has no future to resolve
  over, so a live bucket is delivered when it completes under both modes.
- A calendar (`D` / `W` / `M`) bucket closes on its period end or its session
  close, and a stream reaches both on its own: a session-clipped daily series
  over a warmup that stops mid-session is the batch's series bucket for
  bucket. The one completion rule a stream cannot use is the fallback that
  closes a period on its last input because the *next* input's stamp is
  already known — a batch has that stamp for every bar but its last, and a
  stream never has it for the bar it has just received. A period whose input
  simply stops early therefore waits for the next period's first pushed bar
  and arrives as `LazyComplete`.
- `authoritative_bars` are installed once, at begin, and therefore cover the
  buckets the warmup completes. A live bucket with no authoritative bar of its
  own aggregates the pushed input and is counted by
  `native_security_misses()`.
- A bucket still open when the stream ends is not delivered by `stream_end`,
  exactly as a batch never delivers the input's trailing partial bucket.
- A stream that declares a series takes **confirmed bars only**: tick input
  and `stream_advance_time` are refused by name. An observed-tick slot is
  finalized after its own matching pass and a quiet-carried slot is a
  synthesized flat bar, so neither has a batch counterpart a bucket could be
  built from.

A spec that declares no series is untouched by all of this: its stream event
sequence, continuation hash and `stream_state_hash()` are the pre-subscription
ones.

**Limits.** A series finer than the input is refused at configure, not
emulated. Only the run's own symbol is addressable; there is no
auxiliary-symbol feed and no chart-slice mapping.

## Batch OHLCV vs ticks vs quiet

Two driver models only: confirmed OHLCV and observed ticks. Mixing them on
one stream is refused. The script-bar **calculation** callback is
`on_native_bar`, one per completed script bucket; the opening, tick and
post-fill hooks fire in addition to it, not instead of it. Calculation is
**close-only** unless the spec asks otherwise; see *Calculation timing* above
for `BarCloseAndFills` and `EveryModeledPoint`.

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
  **source-route** refusal, not a limit on the native hooks: `on_native_tick`
  and `on_native_applied` are delivered on a stream, and a native host's own
  `NativeRunSpec::calculation` is accepted there, where `EveryModeledPoint`
  recalculates per observed print
- A nonempty staged native FX curve on `stream_begin`; batch runs may use one.
- Auxiliary/native security feeds, source magnifier/tail/probe/hash/trace
  setters, `set_input`, and Pine
  entry/exit/cancel commands — native hosts latch `Failed`
  (`UnsupportedSource`) before mutation
- Tick input (`stream_push_tick` / `stream_push_ticks` /
  `stream_advance_time`) on a stream whose spec declares `subscriptions`;
  confirmed bars carry those series
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

### Sizing without a host override

`native_order::Sized` is the sixth `OrderIntent`: an opening whose size the
kernel resolves, so a bare host needs no `resolve_execution_terms` override to
buy a cash amount or a share of equity.

```cpp
no::Request request;
request.intent = no::Sized{no::Side::Long, no::EquityFraction{0.10}};
submit(request);                                  // 10 % of marked equity
```

`SizeBasis` is `CashValue{cash}` in account currency or `EquityFraction{f}`, a
fraction in `(0, 1]` of marked equity at the sizing point. Resolution is
`units = cash / (price * point_value * fx)`, where `price` is the candidate's
default resolved price. `SizeTime::AtMatch` (the default) resolves at the
matching candidate; `SizeTime::AtAcceptance` freezes the units against the
command point when the request is accepted. `reserve_percent_fee` divides the
sizing cash by `1 + fee` when the run's fee kind is `NativeFeeKind::Percent`,
and is an exact no-op for every other fee kind. `grid_policy` reuses
`ExecutionGridPolicy`: `SnapToGrid` floors the units onto the run's
`quantity_grid`; `ExplicitUnits` keeps the literal quotient, which the ordinary
on-grid terms gate then refuses as `InvalidTerms` when a grid is configured and
the quotient is off it. The two are identical on an ungridded run.

The kernel resolves the basis *before* the host hook and publishes the result
as the facts' `RemainingUnits`, so a `resolve_execution_terms` override still
has the last word: return your own units to replace the kernel's, or the
default identity terms to accept them. A basis that is not representable — not
finite, not positive, or below one grid step after snapping — is
`MatchRejectReason::TermsUnresolved` at the candidate. An invalid basis
(`fraction` outside `(0, 1]`, `cash <= 0`) is `RequestRejectReason::InvalidQuantityBasis`
at submit. A kernel-sized opening always settles as a signed book transaction
on its declared side; the `ReverseTo` and `CloseOpposite` shapes remain
`HostSized{Open}` only, and `Sized` requires the `Independent` owner.

### Fractional reduces

`ReductionSize` gains `ScopeFraction{fraction, claim}`, a fraction in `(0, 1]`
of the scope the reduce is bound to, resolved at the matching candidate:

```cpp
no::Request exit;
exit.intent = no::Reduce{no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings}};
exit.owner = no::BindOpening{opening, cycle};     // half of that one lot
```

The bound scope is the whole book for an unbound or book-bound close, the one
opening for `BindOpening`, and the live selected roster for `BindOpenings`.
`ScopeClaim::Gross` takes the fraction of that scope as it stands.
`ScopeClaim::NetOfSiblings` first subtracts the units already claimed by the
live sibling reduces bound to the same scope, so two 50 % siblings on one
10-unit lot claim 5 + 5 gross and 5 + 2.5 net. Scope identity is the request's
own authority — opening handles, position cycles and cohort handles — never a
source identifier. A bracket child that is still waiting for its parent has no
scope and resolves only after the parent fill. The fraction is floored onto
`quantity_grid` like every other engine quantity; a fraction that does not buy
one whole step is `TermsUnresolved`, and `fraction` outside `(0, 1]` is
`InvalidQuantityBasis` at submit.

Neither kind is emitted by the Pine adapter, which keeps resolving its own
`HostSized` terms; `native_order` values therefore belong to `native_order_v6`.

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
`examples/native/native_selected_strategy.cpp`. The accompanying runner test
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

### Examples and the export macro {#native_engine_examples}

Both hosts on this page are built sources, not listings. They live under
`examples/native/`, alongside a minimal `hello_kernel.cpp`:

```bash
cmake -S . -B build -DPINEFORGE_BUILD_EXAMPLES=ON
cmake --build build -j
./build/examples/native/hello_kernel
```

`PINEFORGE_BUILD_EXAMPLES` builds each one as a standalone executable and
registers it as a CTest row. The market and selected examples are also built,
from the same sources, as the MODULE targets the live runner `dlopen`s.

A host becomes such a module through one macro from
`<pineforge/native_module.hpp>`:

```cpp
PINEFORGE_EXPORT_NATIVE_STRATEGY(MyHost);
```

It defines `strategy_create`, `strategy_free`, `strategy_set_input`,
`strategy_set_override`, `strategy_set_magnifier_volume_weighted`,
`run_backtest`, `run_backtest_full` and `report_free` — and no other C symbol.
Every remaining runtime export (`strategy_configure_native_v1`, the
`strategy_stream_*` family, `strategy_execution_contract`,
`strategy_get_last_error`) is the engine's own; the generated `strategy_create`
references `pf_abi_version()` so a static link retains it. The host class must
derive from `NativeStrategyHost` and must not be `final`: the macro wraps it in
one derived class so the C boundary can write the presentation error string.

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
