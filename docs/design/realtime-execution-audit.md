# Realtime execution contract: audit and development plan

Status: implementation in progress on the draft PR; the completion gate below
remains authoritative

Date: 2026-07-11 (revised same day after a fourth, multi-perspective review
pass; see the review disposition section)

Worktree branch: `codex/realtime-execution-contract`

Implemented in the first contract slice:

- close-only profile rejection before warmup for every-tick,
  every-history-tick, and order-fill recalculation modes;
- fixed-grid and data-driven gap policies, with clock events forbidden from
  producing price-contingent broker transitions;
- exact last-trade process-on-close provenance;
- durable cross-event realtime stop-limit activation and per-order realtime
  trailing activation/watermarks;
- immutable command-revision, order-leg, fill, and entry-lot identities;
- bounded structured lifecycle diagnostics with an overflow-independent
  canonical count/hash, ABI v3, Python mirrors, docs, and tutorial output;
- realtime OCA exclusion by exact order-leg identity while retaining the
  historical public-ID behavior.

This slice does not close the completion gate: P1-P9 exports, full per-lot exit
fan-out, a canonical whole-engine handoff digest, and the frozen three-session
pre/post experiment remain explicit follow-up work and must not be represented
as complete by the draft PR.

## Decision summary

PineForge should implement a deterministic historical-to-live simulator with a
documented execution contract. It should not claim to reproduce TradingView's
private realtime feed, broker emulator, or exchange execution.

The first supported realtime calculation profile should be Pine's default
close-only strategy cadence:

- historical confirmed OHLCV warms the same strategy and broker instance;
- positions, open entry lots, equity, pending orders, series, TA state,
  `request.security()` state, and partial timeframe aggregation survive the
  handoff;
- ordered executed-trade events update the forming bar and immediately evaluate
  the broker's resting orders;
- strategy code executes only when a script-timeframe bar becomes confirmed;
- `calc_on_every_tick` rollback and realtime `calc_on_order_fills` re-execution
  are separate calculation profiles and must not be silently approximated.

There is no single source-of-truth ordering for every question:

| Question | Authority |
|---|---|
| Event time, order, price, and quantity | Normalized input tape plus its versioned normalization manifest |
| Pine commands, IDs, pyramiding, exit reservation, and calculation semantics | TradingView documentation, followed by focused TradingView probes where documentation is ambiguous |
| Undocumented realtime simulation choices | PineForge's published contract and executable invariants |
| Historical confirmed-bar regression | Pinned TradingView exports |
| Alternative implementation ideas | Other engines as design evidence, never as an oracle |

## Audit sources

### TradingView

Primary references:

- [Strategies](https://www.tradingview.com/pine-script-docs/concepts/strategies/)
- [Execution model](https://www.tradingview.com/pine-script-docs/language/execution-model/)
- [Alerts FAQ](https://www.tradingview.com/pine-script-docs/faq/alerts/)
- [Webhook configuration](https://www.tradingview.com/support/solutions/43000529348-how-to-configure-webhook-alerts/)

Relevant documented behavior:

- An order is an instruction; a trade is a transaction produced by an order
  fill. A net position can contain multiple open trades.
- By default, a strategy calculates at a bar's closing tick and newly created
  market orders fill on the next available price tick.
- `pyramiding` limits same-direction open trades created by `strategy.entry()`;
  it does not make an entry ID unique and does not constrain
  `strategy.order()`.
- The same entry ID can identify multiple open trades. With `pyramiding = 2`,
  two `strategy.entry("buy", ...)` fills can coexist, and one
  `strategy.exit(..., from_entry = "buy")` call can create exit orders for
  both entry trades.
- Price-based entries created on the same tick can exceed the declared
  pyramiding limit when several eligible orders trigger.
- `calc_on_every_tick = true` can create and fill the same `"Buy"` entry ID
  more than once on one realtime bar.
- `calc_on_order_fills = true` can re-run the script after each fill. The
  official `strategy.entry("Buy", ...)` example can produce four same-ID
  entries on one historical bar.
- A stop-limit order has durable activation: after the stop triggers, its limit
  leg remains active until it fills, is replaced, or is cancelled.
- `process_orders_on_close` adds a closing-tick processing opportunity. It is
  not a claim that a real venue will fill an order after the session closes.
- If a `strategy.exit()` call occurs before its referenced entry order
  executes, the strategy waits and creates the exit orders only after the
  entry order fills; entry-relative exit prices depend on that actual fill.
- The `strategy.exit()` reference states that a call with both stop-loss and
  trailing arguments "places only the order that is supposed to fill first,
  because both orders are of the 'stop' type". Pinned corpus exports prove
  this is working-order bookkeeping, not mutual exclusion: probe
  `bracket-exit-three-way-set-once-entry-01` (one set-once exit with stop,
  limit, and `trail_points`) shows 18 fixed-stop fills and 774 trail fills
  from the same call, with exact engine parity. Both protections stay
  behaviorally in force; at any moment one effective stop-type working order
  exists and the first-triggered level wins.
- Trailing legs are not always created inactive: a `trail_price` less
  favorable than the market at creation, or a negative `trail_points`, creates
  the trailing stop immediately; when both `trail_price` and `trail_points`
  are supplied, the level expected to activate first is used.
- `slippage` is documented to shift market/stop fill prices unfavorably, and
  `backtest_fill_limits_assumption` gates limit fills by a tick margin. The
  engine already implements `slippage_`; both must have a defined realtime
  rule.
- The strategy declaration also has `calc_on_every_history_tick`, which
  changes historical (warmup) execution cadence — a distinct calculation
  profile dimension.
- OCA cancellation is not documented as instantaneous: "if order prices are
  the same or they are close, more than 1 order of the same group may be
  filled". An OCA group's identity is `(oca_name, oca_type)` — same name with
  different types is two distinct groups.
- Broker state is exempt from Pine's realtime rollback: "The data from
  strategy orders placed or filled on the ticks within a bar is not subject
  to rollback" (execution model). Any future `calc_on_every_tick` profile
  must roll back Pine variables but never command/leg/fill state.

### PineTS

Audited revision:
[`fdb7650`](https://github.com/QuantForgeOrg/PineTS/tree/fdb7650fde7b5cfff1851d03d1602cc826237c9e)
(reported as v0.9.28 by the checkout).

Relevant design observations:

- PineTS now contains a strategy simulator; PineForge's existing benchmark
  prose that calls it indicator-only is stale relative to this revision.
- Its strategy state separates pending orders, open trades, closed trades, and
  the net-position scalars.
- [`strategy.entry()`](https://github.com/QuantForgeOrg/PineTS/blob/fdb7650fde7b5cfff1851d03d1602cc826237c9e/src/namespaces/strategy/methods/entry.ts)
  appends order objects to `pending_orders`. Its tests explicitly queue and
  fill three entries on the same bar when pyramiding permits them.
- Its current fill engine evaluates pending orders from bar OHLC. The live
  stream tests cover indicator/runtime streaming, but the audited strategy
  tests do not establish a tick-by-tick live broker contract.
- `entry()` performs no same-ID pending replacement — it appends
  unconditionally — so PineTS contributes no evidence for the command
  replacement rule; only PyneCore and TradingView probes do.
- Therefore PineTS is useful evidence for separating order, trade, and
  position identity, but not a realtime fill oracle for PineForge.

### PyneCore

Audited revision:
[`ffeab9e`](https://github.com/PyneSys/pynecore/tree/ffeab9e5dfe6f063ed2728626df290dae8a0c5e6).

Relevant design observations:

- PyneCore's simulator keeps one pending entry/normal order per order ID and
  replaces the still-unfilled object on reissue. Exit intents are keyed by
  `(exit_id, from_entry)`, gaining a third `book_seq` component for same-bar
  stacked partial closes, allowing one exit ID to fan out across entry scopes.
- Its open-trade list is distinct from the pending-order maps and net position.
- Its live-mode design suppresses strategy commands during historical warmup:
  `lib/__init__.py:127-128` defines a `_strategy_suppressed` flag ("prevents
  strategy order placement during historical phase in live mode") gating
  every strategy command, though nothing in the OSS repository at the pinned
  revision sets it, and PyneCore's docs describe live market support as
  planned. (An earlier revision of this document cited
  `pynecore.org/docs/advanced/live-mode/`, which no longer resolves.) That
  enter-live-flat model is not PineForge's required model because PineForge
  must preserve warmup positions, equity, and pending orders.
- PyneCore's abstract `PositionBase` seam is designed for an out-of-repo
  `BrokerPosition`; only the simulated `SimPosition` ships in the audited
  revision. That separation still supports keeping PineForge's deterministic
  simulator distinct from future real order routing.
- PyneCore has already probed one rule this plan defers: a reissued trailing
  leg keeps its activated watermark only when the trailing parameters compare
  equal; a reissue with changed parameters is a cancel+replace that re-arms
  from the reissue bar's close (`strategy/__init__.py` ~4226-4262, annotated
  "Verified against a TV reference"). Use this as the prior for the plan's
  own trailing-reissue probe. Its OCA cancellation also excludes by exact
  order object identity, not public ID — direct precedent for the
  `order_leg_id` exclusion rule below.
- Neither counterpart implements durable stop-limit activation: PineTS treats
  stops and limits independently, and PyneCore turns an order with both stop
  and limit into two OCA legs — which conflicts with TradingView's documented
  stop-limit. The stop-limit rules in this contract therefore rest on
  TradingView documentation plus new focused probes alone; those probes
  cannot be deprioritized.

## The named-order hypothesis

The proposed rule—"each named position triggers at most once for entry and
exit per bar, even with pyramiding"—is a reasonable operational safety instinct,
but the unit and scope need correction.

An ID does not name a position in Pine:

| Concept | Identity | Cardinality |
|---|---|---|
| Net position | symbol + strategy instance | One signed aggregate |
| Open trade / entry lot | immutable `entry_lot_id`, plus public entry ID | Many per net position; several can share a public ID |
| Command revision | immutable `command_revision_id` for one placement/reissue | One creation snapshot; may generate several executable legs |
| Executable order leg | immutable `order_leg_id`, parent command revision, entry-lot scope, and leg kind | Market, limit, stop, or trail state machine; one command can own many |
| Fill event | immutable `fill_id`, order leg, and market-event provenance | In the v1 close-only profile every fill is a full terminal execution, so one fill per leg; the schema reserves several `fill_id`s per leg so a later partial-execution profile is additive, not breaking |
| Closed-trade row | FIFO/ANY allocation of a fill to entry lots | One fill can create several rows |

A global `(ID, bar) -> at most one fill` rule would conflict with documented
Pine behavior for pyramiding, `calc_on_every_tick`, and
`calc_on_order_fills`. It would also conflate a command with its executable
legs, one broker fill with the several trade rows that fill may close, and a
public display ID with an immutable broker identity.

The recommended invariant is:

> Every executable order leg has an immutable identity and creation snapshot,
> mutable broker lifecycle state, and at most one terminal transition. A
> command reissue replaces its still-pending command revision and executable
> legs according to the documented replacement rule. After terminal
> completion, the same command key may create a new revision with the same
> public ID.

Close-only scheduling does not create a once-per-ID-per-bar guarantee. An old
resting order can fill on an intrabar trade; the confirmed-close calculation
can then issue a new command revision with the same ID, and
`process_orders_on_close` or `immediately = true` can fill a new executable leg
on that bar's observed closing event. Any operational duplicate guard for
webhook consumers must therefore be downstream idempotency keyed by `fill_id`,
not Pine broker semantics keyed by public ID and bar.

One `strategy.exit()` command revision can generate TP, SL, and trailing legs
for multiple matching entry lots. Entry-relative prices can differ by lot, so
those child legs require independent activation, watermark, reservation, OCA,
and terminal state even when they share one exit ID. Per lot, the SL and
trailing legs collapse to one effective stop-type working order at any moment
(first-triggered level wins, per the documented fill-first bookkeeping and the
pinned `bracket-exit-three-way-set-once-entry-01` export); the internal legs
model both levels so the working order can be re-selected as the trail
ratchets. An exit revision whose referenced entry order has not yet filled is
deferred: its legs are created only by the entry fill event and are priced off
the actual fill.

## PineForge current-state audit

Audited branch base: `main` at `7a8fc3b`; re-verified against `98ad849`
(PR #89, "Keep within-cap same-direction entries co-queued with a deferred
close"). #89 does not alter any row below, but it is directly relevant to the
contract: it pins TradingView's pyramiding gate at order PLACEMENT (the engine
previously gated at fill) via a new immutable
`PendingOrder::over_pyramiding_cap_at_placement` creation snapshot — exactly
the creation-snapshot/lifecycle-state split this plan mandates — and it adds
another silent-cancel discriminator to the deferred-close wipe, reinforcing
the diagnostics gap.

| Requirement | Current evidence | Assessment |
|---|---|---|
| Same-instance historical warmup and handoff | `stream_begin()` calls `run()` once, then preserves the instance; `test_streaming` verifies position, pending order, equity/trade continuity | Implemented for the tested path |
| Ordered provider-neutral trade input | `TradeTick` validates price, quantity, nondecreasing time, and strictly increasing nonzero sequence | Implemented; zero sequence delegates tie ordering to the caller |
| Forming OHLCV without lookahead | Events update O/H/L/C/V before confirmation; exact-boundary ticks are assigned after the prior bar is finalized | Implemented for fixed input timeframes; broader cases need tests |
| Higher-timeframe partial aggregation survives handoff | Existing streaming unit test spans historical and realtime input bars inside one 5-minute candle | Implemented for the tested case |
| Default close-only script scheduling | Broker evaluates every trade; `on_bar()` runs only when a script bar confirms | Implemented |
| Same-ID pending replacement | `strategy_entry()` and `strategy_order()` erase pending objects with the same raw ID (bare `o.id == id`, no `OrderType` filter); exits replace by `(id, from_entry)`; all preserve the old `created_seq` | Partial: command, priority, and executable-leg identity are conflated. The namespace rule is already half-pinned in-repo: `clear_existing_exit_order`'s regression comment records that a bare-ID predicate once deleted a still-pending entry (zero trades) and that entry and exit IDs are independent namespaces — yet the entry/order-side erase still treats the namespace as global, so it is a likely live bug, not merely an unproven rule. The remaining probe targets entry-vs-raw-order collision |
| One terminal transition per executable leg | Filled pending objects are erased from `pending_orders_` | Implicit only; generated legs, terminal state, and reason are not retained or observable |
| Market and stop fills on observed trade events | Streaming tests cover next-event market fill and stop gap-through at the observed price/time | Implemented for tested cases |
| Limit fill in streaming | Uses the generic point-bar evaluator | Not directly tested |
| Stop-limit activation across events | `PendingOrder::stop_limit_activated` exists, but normal `process_pending_orders()` does not persist the activation returned by `resolve_entry_stop_limit_fill()` (the out-value is a discarded local); persistence is currently gated to the historical COOF scheduler | Defect. Strictly worse in streaming: events are point bars (O=H=L=C), so a stop-limit entry can only ever fill when one single tick satisfies both the stop and the limit — cross-event activation is impossible today |
| Trailing stop across events | Tick points update one global `trail_best_price_` for the position; `clear_existing_exit_order` resets that global watermark to the current close whenever a fresh trail request with no matching prior `(id, from_entry)` order arrives in position, and entry-relative activation uses the aggregate position entry price, not the per-lot entry | Architectural defect, and not hypothetical: arming a second trailing exit destroys the first trail's already-ratcheted watermark today |
| OCA cancel/reduce on one event | Generic fill loop contains OCA behavior and excludes siblings by public ID | Historical tests exist; public-ID exclusion cannot distinguish same-ID executable siblings or revisions |
| `process_orders_on_close` | A close-price point pass prevents newly created orders from inspecting the elapsed wick | Partial: it reuses the bar-open timestamp and has no closing-event sequence, so fill provenance is wrong; it can also fill a no-trade synthetic bar |
| Quiet intervals | Clock advancement creates zero-volume carry-forward bars | Contract issue: both the pre-script broker pass and the post-script POC pass can create synthetic fills, excursions, trailing changes, or margin actions without an observed trade |
| Out-of-session intervals | Empty closed-session intervals are skipped; an actual input event remains authoritative | Partial; session-edge and first-event-after-reopen tests are missing |
| Event-level diagnostics | No command-revision, executable-leg, fill, or decision log in the public report | Missing |
| Replay/live equivalence | `push_ticks()` is a loop over `push_tick()` and a basic ordering test exists | Structural implementation is sound; result equivalence needs an explicit test |
| Handoff state equivalence | Tests inspect selected visible fields after warmup | Missing a canonical digest covering all broker, Pine, TA, security, aggregation, risk, session, and sequence state |
| Three full corpus handoffs ending 2025-05-01 | `run_stream_corpus_mmap.py` supports exactly three starts and one contiguous mmap tape | Runner exists, but exit status currently fails only on runtime errors, not parity/invariant failure. Whole-session feasibility is already proven: a stored three-session run through 2025-05-01 exists (see below), though with clustered starts and pre-#86..#89 binaries |
| Calculation-profile validation at `stream_begin()` | None exists: realtime dispatch calls `on_bar()` directly and never the COOF scheduler (`coof_scheduler_active_` is set only inside `run()`), so a COOF strategy warms up with COOF semantics then silently degrades to close-only after handoff. `calc_on_every_tick` does not exist as an engine property at all (codegen drops the declaration; both corpus probes declaring it compile to close-only artifacts) | Defect: exactly the silent-approximation hazard section 4 forbids, live today; plus a codegen prerequisite before the gate can inspect every-tick profiles |
| `bar_magnifier` across handoff | `stream_begin()` silently forces warmup to run with magnifier off, then enables `bar_magnifier_enabled_` for the realtime phase; four corpus probes configure `"bar_magnifier": true` | Unaddressed contract question: for those probes a magnifier-on normal run cannot digest-equal the magnifier-off warmup, so the handoff-digest invariant must be scoped and section 4 must pin accept/reject/override |
| Historical TradingView regression | Required `ctest` and `scripts/run_corpus.sh` gates exist | Must be rerun after implementation |
| Public docs/tutorial | Streaming lifecycle and Python tutorial exist | Lifecycle is documented; normative state machine and diagnostics are missing |

Stored artifacts (correcting an earlier revision of this document, which
wrongly claimed only ten-minute sessions existed): besides the ten-minute
smoke reports, the build directory holds three 60-minute sweeps and one full
three-session run through `2025-05-01T00:00:00Z`
(`stream_corpus_to_20250501.json`): handoffs 2025-03-29/03-30/03-31T00:00Z,
252 probes per session over 154.9M-161.8M ticks each, 216.7 seconds total
elapsed, and per-session summaries errors=0, input/script bars equal 252/252,
trade-count equal 250/252, ordered structural match 243/252. That run proves
whole-session feasibility and gives a real parity reference level, but it
does not satisfy section D as specified: its starts are clustered midnights
rather than the spread non-round starts, it predates the behavior-changing
merges #86-#89, and the runner still lacks the expanded scoring and enforced
exit status. The section D baseline must be re-run on current `main`.

## Goal

Deliver PineForge Realtime Execution Contract v1:

> A deterministic, inspectable transition from confirmed historical OHLCV to
> an ordered executed-trade stream, preserving the complete strategy and
> broker state, confirming bars without lookahead, and expanding each command
> revision into explicit executable order legs with deterministic state
> transitions and fills. Identical configuration, warmup bars, normalized bar
> policy, market events, clock events, and session metadata must produce
> identical command, leg, fill, trade, equity, and report state.

TradingView historical corpus parity remains a regression requirement. Exact
TradingView realtime feed or executor parity is not a goal — and is not even
well-defined: TradingView documents that reloading a chart erases and
re-simulates the Strategy Tester from OHLC-only history ("not a bug"), and
realtime tick streams are plan-tier dependent and conflated, so no two
TradingView sessions see the same tape. The published contract should state
that the v1 profile corresponds to TradingView's REALTIME fill model
(tick-driven resting-order evaluation with close-only script calculation),
not its historical bar-path model, so users are not surprised when streamed
results differ from their Strategy Tester backtest.

## Scope

### In scope for v1

- One long-lived strategy instance across warmup and realtime.
- Fixed-duration input timeframes and existing script-timeframe aggregation.
- Executed-trade events with timestamp, source sequence, price, and quantity.
- Explicit bar-close/clock advancement plus a documented provider/session gap
  policy for deciding whether a quiet interval produces a confirmed bar.
- Default close-only strategy scheduling.
- Command revisions that generate independently stateful market, limit, stop,
  stop-limit, and trailing executable legs.
- `strategy.entry`, `strategy.order`, `strategy.exit`, `strategy.close`,
  `strategy.close_all`, cancellation, pyramiding, reversal, OCA cancel/reduce,
  and `process_orders_on_close` as already supported by the historical engine.
- `strategy.exit()` fan-out, entry-relative leg prices, default persistence,
  `from_entry` creation-time cutoff semantics, and wait-for-entry deferral
  (exit issued before its entry fills expands only on the entry fill).
- `slippage` semantics for event fills (the engine already implements
  historical slippage; the realtime rule is part of this contract).
  `backtest_fill_limits_assumption` is explicitly unsupported and assumed 0,
  so the limit-or-better rule is unambiguous.
- A `request.security()` realtime decision: either pin what a close-only
  realtime calculation sees for a higher-timeframe series whose parent bar is
  still forming (per `barmerge.lookahead` mode) with unit tests, or reject
  strategies using `request.security()` at `stream_begin()` for v1. Silent
  approximation is not an option; this is a known-sharp area.
- Gaps between observed prices, quiet in-session bars, closed-session gaps, and
  first events after a session reopens.
- Stable lifecycle diagnostics for create, replace, activate, ratchet, fill,
  cancel, reduce, reject, and expire decisions.
- C++ and C API documentation, Python mirrors, and a runnable tutorial.
- Targeted state-machine tests, deterministic replay tests, and three full
  corpus handoffs.

### Explicitly out of scope for v1

- Exchange queue position, liquidity, partial execution from tape quantity,
  market impact, latency, spread, bid/ask, or order-book simulation.
- Network reconnect, persistence, broker routing, acknowledgements, and live
  exchange order IDs.
- Exact TradingView realtime feed or private broker-emulator reproduction.
- `calc_on_every_tick` rollback and `varip` semantics. The future profile
  must honor the documented asymmetry: Pine variables roll back per tick, but
  broker order/fill state never does. The command/leg/fill separation defined
  here (immutable creation snapshots, mutable leg state outside Pine's
  rollback domain) is the intended substrate for that profile.
- `calc_on_every_history_tick` warmup cadence (see section 4's gate).
- Realtime `calc_on_order_fills` re-execution. Historical COOF support remains
  unchanged; a streaming start using this profile must fail clearly until a
  dedicated realtime scheduler is implemented.
- Session restart continuity. A restarted session is NOT replay-equivalent to
  the continuous stream: re-warming from derived OHLCV re-simulates through
  the historical bar-path kernel intervals previously executed tick-by-tick,
  so fills can move or disappear (the exact analog of TradingView's
  documented reload divergence). Determinism holds per tape; operators
  needing continuity must persist and replay the raw trade tape.
- Alert/webhook delivery. The lifecycle events added here are intended to be
  the deterministic source for a later alert layer, including JSON payloads.
  Lifecycle events are simulator facts, not account truth; delivery will be
  at-least-once in any transport, so consumers must dedupe on `fill_id`, and
  broker adapters own account reconciliation.

## Contract and implementation method

### 1. Freeze public terminology and keys

Use these distinct identities:

- `command_key`: stable public intent key:
  - entry: `(entry, id)`;
  - raw order: `(order, id)`;
  - exit: `(exit, id, from_entry)`;
  - close: its explicit close target and internal command identity.
- `command_revision_id`: fresh monotonic identity for every initial placement
  or replacement, with immutable command arguments and creation snapshot.
- `order_leg_id`: fresh immutable identity for every executable market, limit,
  stop, or trailing child generated by a command revision. Exit revisions can
  own several entry-lot groups and several legs per group.
- `priority_sequence`: broker ordering identity, distinct from revision
  identity. Focused parity probes must define when replacement preserves or
  resets it.
- `event_sequence`: monotonic normalized market-event ordering key.
- `fill_id`: monotonic broker execution identity and downstream idempotency key.
- `entry_lot_id`: immutable identity for each filled entry trade.
- `bar_close_provenance`: the confirming boundary plus the timestamp and
  sequence of the last observed executable trade inside the bar, if any.

The `command_key` schema above is provisional until probe P1 (section 7)
concludes. Whether the same public text ID is one global order key or belongs
to separate entry/raw/exit namespaces must be pinned by focused TradingView
probes before changing the current implementation; kind-tagged keys can still
express a global-collision answer via a cross-kind replacement rule, but the
schema must not be treated as frozen. Half the rule is already pinned
in-repo: `clear_existing_exit_order`'s regression comment records, from a
real zero-trades bug, that entry-order IDs and exit-order IDs are independent
namespaces — which makes the current entry/order-side bare-ID erase a likely
live bug. `strategy.cancel(id)` is documented to cancel all pending orders
with that ID, but that does not by itself prove the replacement collision
rule between different placement commands. The internal revision model must
represent the proven public rule without relying on a raw string comparison
accidentally.

The command revision is immutable; the executable leg's lifecycle state is
mutable. Activation, trailing watermark, active stop, remaining quantity, and
OCA reductions live on the leg, not on the command revision and not globally on
the net position.

### 2. Define the order state machine

Command-revision states (the revision itself has a small lifecycle distinct
from its legs):

- `WAITING_FOR_ENTRY`: an exit revision whose referenced entry order has not
  filled; it owns no legs yet and expands deterministically on the entry fill
  event, pricing entry-relative legs off the actual fill;
- `EXPANDED`: legs created;
- terminal `REJECTED_AT_PLACEMENT`: the command is refused before any leg
  exists (for example TradingView's placement-time pyramiding gate, pinned by
  PR #89), with a reason code;
- terminal `REPLACED`: superseded by a same-key reissue.

Executable-leg states:

- `PENDING_MARKET`
- `PENDING_LIMIT`
- `PENDING_STOP`
- `PENDING_STOP_LIMIT`
- `ACTIVE_STOP_LIMIT`
- `PENDING_TRAIL_ACTIVATION`
- `ACTIVE_TRAIL` (a leg can be born in this state: `trail_price` already
  reached or less favorable than market at creation, or negative
  `trail_points`)
- terminal `FILLED`, `CANCELLED`, `REJECTED`, `EXPIRED`, `REPLACED`

Required transitions:

- command creation -> one command revision -> one or more pending executable
  legs (or `WAITING_FOR_ENTRY` deferral for early exits);
- same-key reissue while pending -> old command revision and remaining legs
  `REPLACED`, new command revision and legs with a fresh revision identity;
- exit fan-out -> independently priced/reserved legs for each eligible entry
  lot or lot group, including distinct TP, SL, and trailing legs; per lot the
  stop-type legs collapse to one effective working order (first-triggered
  level wins), matching the documented fill-first bookkeeping and the pinned
  three-way bracket export;
- stop-limit stop touch -> `ACTIVE_STOP_LIMIT`, durably retaining the limit;
- trail activation -> `ACTIVE_TRAIL`; favorable events ratchet its watermark;
- eligible price event -> exactly one `FILLED` transition (v1 fills are full
  terminal executions; the schema reserves several fills per leg for a later
  partial-execution profile);
- OCA fill -> sibling leg `CANCELLED` or quantity-reduced event, excluding only
  the exact filled `order_leg_id` rather than every sibling with the same
  public ID;
- risk/margin gate -> `REJECTED` with a reason code;
- explicit cancellation -> `CANCELLED`;
- `EXPIRED` is produced only by the enumerated expiry rules: a POC/immediate
  market leg whose only eligible closing event has passed (the existing
  POC/COOF rule). Expiry is order management, not a fill, so it may be
  clock-driven; no other expiry producer exists in v1;
- stream end does not implicitly fill a partial bar.

Every transition records the before/after state, command revision, executable
leg, public IDs, entry-lot scope, bar index, event timestamp/sequence, observed
price, evaluated trigger level, resulting quantity, and stable reason code.

The contract must separately specify:

- whether unchanged stop-limit parameters preserve activation across a
  same-key reissue;
- whether unchanged trailing parameters preserve activation/watermark across a
  reissue, and when changed parameters reset them (PyneCore's TV-probed
  parameter-equality rule is the prior);
- trailing creation-time activation: the immediate-activation
  parameterizations (unfavorable `trail_price`, negative `trail_points`) and
  the fill-first selection when both `trail_price` and `trail_points` are
  given;
- how `strategy.exit()` without `from_entry` persists for later entry lots;
- the creation-time cutoff for `strategy.exit(..., from_entry = X)`;
- the realtime `slippage` rule (expected: applied on top of the observed
  trade price exactly as the historical kernel applies it to market/stop
  fills);
- the OCA cancellation rule for several same-group legs eligible on one
  event. TradingView documents that same/close-priced same-group orders "may
  be filled" together, and the historical kernel encodes a probed TV quirk
  (group cancel only after a full fill). A strict
  one-winner-per-event rule is a deliberate, named divergence unless a
  focused probe pins TV's realtime behavior; either way the historical
  kernel's corpus-parity OCA behavior must be preserved verbatim;
- exact simultaneous-event priority among carried market orders, price-based
  entries, exits, trailing legs, OCA effects, reversals, risk gates, and margin
  actions, including whether eligibility is recomputed after every fill.

### 3. Separate scheduler, bar builder, and broker

Treat these as independent normalized events:

- `TRADE(timestamp, sequence, price, quantity)` supplies an executable price
  and updates a forming bar.
- `BAR_CLOSE(boundary, policy)` confirms a bar selected by the configured
  provider/session gap policy. It carries no new executable price by itself.

Clock events are validated like trade events: boundaries must be strictly
increasing, a boundary earlier than the last accepted trade timestamp is
rejected (or resolved by an explicitly documented tie rule), and a duplicate
boundary is rejected or idempotent — never double-confirming. An invalid
clock event fails before state mutation, exactly like an invalid trade,
because clock events trigger strategy calculation. Hard-failing repeated
trade sequences is deliberate for the deterministic core: real feeds are
at-least-once, and dedup belongs to the caller/adapter layer above this API.

For each trade event:

1. Validate time, sequence, price, and quantity.
2. Confirm every bar boundary strictly before or at the event timestamp.
3. Run close-only strategy calculations for newly confirmed script bars.
4. Apply the event to the new/current input bar (`close` is this exact trade
   price; high/low/volume update monotonically).
5. Evaluate eligible executable legs at that exact observed price.
6. Apply fills, OCA/risk effects, excursion state, margin effects, and
   diagnostics using the published priority rules, recomputing eligibility
   after each state-changing fill when the contract requires it.

For a bar-close/clock event with no new trade:

1. Use the explicit gap policy and session calendar to decide which elapsed
   intervals become bars. V1 supports exactly two policies:
   - `fixed-grid` (default, and the one used by the section D corpus
     experiments): every elapsed in-session interval becomes a zero-volume
     carry-forward bar on the input-timeframe grid, keeping `bar_index`
     aligned with the 1m OHLCV grid;
   - `data-driven`: a bar confirms only on the first trade after its
     boundary — TradingView's actual behavior, where a tickless bar never
     confirms and the script never runs.
   Fixed-grid is a published, named divergence from TradingView: clock-
   confirmed quiet bars RUN strategy logic (bars-since exits, time/session
   flattening, na-volume-sensitive conditions) at moments a purely
   data-driven TradingView chart would be silent. The session calendar
   source is the engine's `syminfo` session/timezone metadata. Note the
   24/7 perp corpus cannot exercise closed-session rows; those exist as
   synthetic unit tests only.
2. Confirm those bars and run close-only strategy calculations. Open-position
   equity and `strategy.openprofit` on a quiet confirmed bar are marked at
   the last observed trade price (the carried close); the mark price never
   moves without an observed trade.
3. Do not fill market, limit, stop, stop-limit, trailing, immediate, or POC
   legs; do not ratchet trails, update price excursions, or run price-triggered
   margin liquidation. A clock confirms time; it is not an executed trade.
   Order management is not restricted: the clock-confirmed calculation may
   create, cancel, and replace command revisions and legs (transitions carry
   clock provenance); only price-contingent transitions — fill, trigger,
   ratchet, excursion, margin — require an observed trade.

When a bar with at least one trade confirms, retain the bar's last trade
timestamp and sequence as its closing-event provenance. A POC/immediate leg
created by the confirmed-close calculation may evaluate only against that
observed close event and must report its actual event timestamp/sequence, not
the OHLCV bar-open timestamp. Previously evaluated carried legs are not walked
over the close a second time. When a confirmed bar has no observed trade, new
POC/immediate legs remain pending until a later eligible trade because no
executable closing event exists.

When the confirming trigger is a later event (the next bar's first trade or a
clock event), the retroactive POC evaluation against the retained closing
event completes before any leg is evaluated at the new boundary event, within
one dispatch. Diagnostics order by processing order; a fill's provenance
timestamp/sequence is carried as data and may be older than events already
processed (see section 5's canonical-ordering rule).

For an event exactly on a boundary, the preceding bar confirms first and the
event belongs to the new bar. An order created by the preceding close can then
fill on that boundary event because it is the next observed price event.

### 4. Make unsupported calculation profiles explicit

`stream_begin()` must inspect effective strategy properties. V1 accepts only
the close-only profile. It rejects realtime `calc_on_every_tick`,
`calc_on_every_history_tick`, or `calc_on_order_fills` with a stable
diagnostic instead of executing a subtly different model. Validation must
occur before warmup mutates the handle, or the entire begin operation must be
transactional and restore a clean pre-call state on rejection. Tests cover
declaration defaults and runtime property overrides.

`bar_magnifier` belongs in the same gate: `stream_begin()` currently forces
warmup magnifier off and enables it for the realtime phase, silently. The
contract must pin whether magnifier configurations are rejected, honored
during warmup, or documented as forced-off — and the handoff-digest invariant
in section B is scoped by that decision. The `request.security()` decision
from the scope section (pin realtime semantics or reject) is also enforced
here.

Current-state notes that shape this work:

- The silent-approximation hazard is live today: realtime dispatch calls
  `on_bar()` directly and never the COOF scheduler, so a COOF strategy warms
  up with COOF semantics and then silently degrades to close-only after
  handoff. `calc_on_order_fills_` exists and can be validated immediately.
- `calc_on_every_tick` does not exist as an engine property: codegen drops
  the declaration entirely (no member, no override field, no emission). The
  two corpus probes declaring `calc_on_every_tick = true` therefore compile
  to effectively close-only artifacts and stream and score normally in the
  section D gate today. Plumbing the property from codegen is a prerequisite
  for enforcing this half of the gate; when that lands and corpus artifacts
  are regenerated, the section D acceptance rule must first define
  expected-rejection scoring (expected-reject counts as pass, or the probes
  are excluded from the denominator) so the gate and this section can
  coexist.

### 5. Add diagnostics without coupling to transport

Add a bounded or caller-drainable order-event stream to the engine report/API.
Diagnostics are simulator facts, not log strings. JSON serialization,
webhooks, retries, and delivery belong above this API.

Batching and caller drain timing must not change simulator results or the
canonical lifecycle sequence. Maintain a rolling canonical event hash/count
independent of retention, and define deterministic capacity, overflow, and
drop counters. Equivalence tests compare the canonical hash and, when no
overflow occurs, the complete retained records.

The diagnostic stream's processing order (equivalently a global transition
sequence, of which `fill_id` order is a subsequence) is the sole canonical
total order. Event timestamp/sequence on a transition is provenance only —
non-unique (a POC fill and a resting fill can share one event) and
non-monotonic across transitions (a retroactive POC fill carries provenance
older than events already processed). Consumers must never order by
provenance.

The canonical hash input is the raw IEEE 754 bit patterns of the canonical
fields in a fixed byte order — never formatted decimals. The equality claim
is scoped: same binary plus same input gives the same hash; cross-platform
equality is best-effort unless floating-point arithmetic is pinned.

At minimum expose:

- command revision, executable leg, priority, fill, and entry-lot identities;
- timestamp, source sequence, input/script bar index;
- command kind, leg kind, order state, transition, reason;
- `id`, `from_entry`, OCA name/type, and on every OCA cancel/reduce the
  resolved member `order_leg_id` set (or an `oca_group_instance_id`), so
  membership at the moment of the effect is observable without re-deriving
  name resolution across revisions;
- a monotonic `position_episode_id` that increments each time the net
  position leaves flat, so the later alert layer never reconstructs episode
  boundaries from fill deltas;
- side, requested/remaining/filled quantity;
- observed, stop, limit, trail activation, trail watermark, and fill prices;
- position size and equity immediately before and after the transition.

Today's fully invisible transitions must become first-class events with
reason codes: risk-rejected entries are currently dropped silently inside the
fill loop, and OCA cancels, the post-full-close same-direction wipe, and the
flat-position purge all erase in place with no record. The streaming
fast path that skips the broker pass when no orders are pending and the
position is flat must either remain provably event-free or be removed, so
canonical event hashes are replay-invariant.

### 6. Keep historical and realtime kernels consistent but not conflated

Reuse risk, sizing, commission, slippage, trade allocation, and position
mutation logic.
Do not send one-price realtime events through a full inferred-OHLC path helper
when that helper has path assumptions or state side effects. The event evaluator
should choose eligibility at one observed point; the historical evaluator can
continue resolving an inferred or magnified path into the same transition and
fill-application kernel.

As part of this split, move stop-limit activation and every trailing activation,
watermark, and active-stop value onto the relevant executable leg. The current
global position trail watermark cannot represent concurrent trails created at
different times or for different entry lots — and it already fails today:
arming a second trailing exit resets the global watermark to the current
close, destroying the first trail's ratchet.

The historical kernel's corpus-parity behaviors are load-bearing regression
anchors and must be preserved verbatim under this split — in particular its
probed OCA behavior (multi-fill at coincident path points, group cancel after
a full fill). The realtime leg-ID exclusion rule must not leak into the
historical path, or the historical corpus gate will drift.

### 7. TradingView probe plan

The state machine's replacement rule, exit fan-out, and fill loop cannot be
finalized without answers to the semantics this document defers to "focused
TradingView probes". Those probes are scheduled work with a defined method,
not a hand-wave; the completion gate requires every one resolved and
archived.

Method: every probe below concerns deterministic broker-emulator behavior and
is observable in historical Strategy Tester exports — the corpus method this
repository already uses — so no live-session observation is required for v1.
Each probe is one minimal Pine script plus its exported trade list, archived
as a corpus probe under `corpus/validation/` with the pinned export, and its
conclusion recorded in this document before the dependent implementation
lands.

| # | Open semantic | Prior / partial evidence |
|---|---|---|
| P1 | Public-ID collision across command kinds on placement (entry vs raw order vs exit sharing a text ID) | Exit-vs-entry independence already pinned in-repo by the `clear_existing_exit_order` regression; remaining: entry-vs-raw-order and placement-collision direction |
| P2 | Whether replacement preserves or resets broker priority (`priority_sequence`) | None |
| P3 | Stop-limit activation persistence across a same-key reissue (unchanged vs changed parameters) | No counterpart precedent exists; TradingView docs pin durability without reissue only |
| P4 | Trailing activation/watermark persistence across a reissue | PyneCore's TV-probed parameter-equality rule is the prior |
| P5 | `strategy.exit()` without `from_entry`: persistence for later entry lots | Docs give the creation-time cutoff for `from_entry`; the no-`from_entry` persistence needs an export |
| P6 | `strategy.exit(..., from_entry = X)` creation-time cutoff edge cases (same-bar entry, reissue) | Docs pin the base rule |
| P7 | Simultaneous-event priority and per-fill eligibility recomputation (carried market vs price-based entry vs exit vs trail vs OCA vs reversal vs risk/margin) | Partial coverage by existing corpus probes; needs targeted coincident-level probes |
| P8 | OCA same-event multi-trigger: does TV cancel before the second same-tick fill? | TV docs say close-priced same-group orders "may be filled"; historical kernel encodes multi-fill; decide divergence or parity |
| P9 | Per-lot stop-type working-order selection when entry-relative levels differ by lot | Three-way bracket probe pins the single-lot case |

Section 1's `command_key` schema is provisional until P1 concludes.

## Verification method

### A. State-machine unit tests

For both long and short directions where applicable:

| Surface | Required event sequence |
|---|---|
| Market | created at confirmed close; fills once at next observed trade, with the configured `slippage` applied by the documented realtime rule |
| Limit | no fill on wrong side; fills limit-or-better on first eligible observed trade (`backtest_fill_limits_assumption` fixed at 0); gap behavior pinned |
| Stop | no fill before trigger; gaps to first observed price through the stop; `slippage` applied by the documented realtime rule |
| Stop-limit | limit seen before stop does nothing; stop activation persists across events/bars; later limit touch fills once |
| Trailing | inactive before activation, and a leg born `ACTIVE_TRAIL` for the immediate-activation parameterizations; per-leg activation persists; watermark only ratchets favorably; two concurrent lots/offsets remain independent; reversal fills at the active level/gap rule |
| Deferred exit | exit issued in the same calculation as its entry produces no legs until the entry fills; legs are then priced off the actual fill; interaction with same-event eligibility recomputation pinned |
| OCA cancel | the rule pinned by probe P8 (strict one-winner exclusion of every sibling except the exact filled leg, including same-public-ID siblings — or TV's documented same-event multi-fill), and the historical kernel's behavior stays unchanged |
| OCA reduce | fill quantity reduces sibling remaining quantities exactly once |
| Pyramiding | multiple entry lots can share an entry ID; cap applies to open `strategy.entry` trades, not raw orders; the placement-time gate pinned by #89 rejects at the command revision (`REJECTED_AT_PLACEMENT`), not at fill |
| Same-ID lifecycle | reissue before fill replaces with a fresh command revision while priority follows the pinned rule; reissue after fill creates new legs; no leg fills twice; #89's within-cap co-queued entry + deferred close case is covered |
| Cross-command ID | focused entry/raw/exit/cancel probes (P1) pin whether identical public text collides or coexists |
| Exit fan-out | two same-ID entry lots at different prices receive independently priced relative TP/SL/trailing legs; per lot exactly one effective stop-type working order exists at any moment (first-triggered wins, re-selected as the trail ratchets); no-`from_entry` persistence and `from_entry` cutoff are pinned |
| Process on close | only an observed closing event is visible to a newly created leg; event time/sequence are exact; an old same-ID resting leg and a new POC leg can both fill in one bar without elapsed-wick lookahead |
| Simultaneous eligibility | exact market/entry/exit/trail/OCA/reversal/risk/margin priority and eligibility recomputation are pinned |
| Gap | fills use the first observed post-gap event under the order-type rule |
| Quiet interval | provider policy decides bar existence; strategy may calculate, but market/limit/stop/trail/POC/margin paths emit no synthetic price transition |
| Session boundary | closed intervals are skipped; first valid reopen event belongs to the correct new bar |

### B. Invariant and metamorphic tests

- Replaying the same events one by one and through one contiguous
  `strategy_stream_push_ticks()` call must produce the same canonical lifecycle
  hash/count, trades, equity, and report metrics. Retained diagnostics are
  byte-equivalent when neither run overflows its documented capacity.
- Repeating an already accepted nonzero sequence, reordering events, or moving
  time backwards must fail before state mutation.
- Clock events obey the section 3 validation rules: regressed or duplicate
  boundaries, and boundaries earlier than the last accepted trade timestamp,
  fail before state mutation.
- Appending future events cannot change any state snapshot taken before those
  events.
- Every command revision and executable leg has exactly one creation; every
  executable leg has at most one terminal transition.
- Filled quantity conservation holds across position, entry lots, OCA
  reductions, and closed-trade allocation.
- Confirmed bars reconstructed from the raw tape equal the trade-derived 1m
  OHLCV source for open/high/low/close/volume and timestamps, subject only to
  documented source-cleaning differences.
- A canonical handoff-state digest must match between a normal OHLCV run stopped
  at T and the historical portion of `stream_begin()` stopped at T. The digest
  covers entry lots, command/leg state, cash/equity, risk counters, session
  state, Pine series/variables, TA state, security evaluators, timeframe
  aggregators, and deterministic sequence counters. The comparison run uses
  the same effective `bar_magnifier` setting that `stream_begin()` applies
  during warmup, per the section 4 decision — otherwise the invariant is
  unsatisfiable for magnifier-configured probes.

### C. Historical regression gates

Run the repository-mandated gates after every behavior change:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_TESTS=ON \
  -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
./scripts/run_corpus.sh
```

Any historical corpus drift is a blocker unless the change fixes a documented
historical defect and the reference is deliberately updated.

### D. Three whole-session corpus experiments

Use all compiled corpus probes and three fixed, minute-aligned, non-round
handoffs, each ending exclusively at `2025-05-01T00:00:00Z`. Suggested starts,
all within the available local raw-trade interval and the requested range:

- `2025-02-07T03:17:00Z`
- `2025-03-11T14:23:00Z`
- `2025-03-29T22:41:00Z`

Data and resource prerequisites (explicit, so the gate is schedulable):

- Raw daily ETHUSDT-perp trade archives live on the external mount
  `/Volumes/PineforgeData/binance_ethusdtp_1y/raw/trades` (2025-01-01 through
  2026-07-08), not in the repository or LFS; the experiments depend on that
  mount being present.
- The existing mmap tape starts 2025-03-29; the 2025-02-07 start requires
  rebuilding one contiguous tape roughly 2.5x larger (~83 days).
- Expected volume: session tick counts approximately 424.8M / 230.6M /
  158.2M (813.6M per probe across the three sessions), about 205 billion
  tick evaluations per full experiment, doubled by the pre-change baseline.
  The stored three-session run (33-day sessions) completed in 216.7 seconds
  total, so the full experiment is an hours-scale job, not days.
- The runner warms all 756 engine instances up-front before the worker pool,
  so peak resident memory scales with 756 warmed states; verify headroom or
  add a per-session sequencing option.

For each probe and start:

1. Slice warmup OHLCV strictly before the handoff from the canonical source.
2. Slice one contiguous raw-trade tape from handoff through the common end.
3. Run a bar-only baseline from the same OHLCV origin through the common end.
4. Run warmup plus the one contiguous event tape on the same strategy instance.
5. Produce per-probe and aggregate scores.

Before implementation, run and freeze the same three-session experiment on the
current merged streaming implementation. That baseline does not define correct
order semantics, but it prevents an arbitrary post-change acceptance threshold
and exposes performance/parity regressions introduced by the refactor. The
frozen baseline artifact must record the engine commit hash, corpus revision,
and the scoring-harness commit hash; "no worse than baseline" is meaningful
only against the same scorer version. The stored 2025-05-01 run is the
feasibility and format precedent, but it predates #86-#89 and cannot be
adopted as the frozen baseline.

Required report scores:

- runtime success count;
- input/script confirmed-bar count equality;
- trade-count equality;
- ordered structural match percentage using direction, entry/exit minute, bar
  indices, and quantity — reported both with and without trailing-exit
  trades, because tick-path trailing vs the baseline's bar-path trailing is
  the expected dominant divergence class and must not mask regressions in
  other order surfaces;
- entry/exit price absolute and basis-point p50/p90/p99 deltas;
- net-profit absolute and relative delta;
- first divergence with order-event diagnostic context;
- deterministic rerun hash for every session.

Slight price/P&L differences between point-event execution and bar-path exports
are reportable, not automatically defects. State discontinuity, lookahead,
nondeterminism, impossible duplicate leg fills, or unexplained bar-count
differences are hard failures.

Acceptance thresholds are frozen before the post-change runs:

- hard `756/756` runtime success and equal input/script confirmed-bar counts;
- exact deterministic rerun hashes and zero lifecycle invariant violations;
- exact handoff-state digest equality at every start;
- exact trade-tape-to-trade-derived-OHLCV reconciliation under the versioned
  normalization rules;
- no historical corpus regression;
- per-session aggregate ordered structural match no worse than the frozen
  pre-change baseline; any individual-probe regression requires an identified
  intended semantic correction and published first-divergence trace. Because
  the baseline embeds behaviors this document classifies as defects (quiet-bar
  synthetic fills, POC provenance, the global trail watermark), an intended
  systemic correction may lower the aggregate: a documented aggregate
  re-baseline is permitted when the divergence class is traced to an
  enumerated intended correction, published with its trace — the bound has an
  escape hatch for corrections, never for unexplained drift;
- if profile-rejection ever applies to compiled probes (see section 4), the
  expected-rejection scoring rule is defined before the run;
- price and P&L distributions are reported and reviewed rather than hidden by
  the structural threshold.

The existing `scripts/run_stream_corpus_mmap.py` is the correct transport shape:
one mmap-backed contiguous tape per session and no artificial engine chunks.
It needs scoring expansion, enforced invariant/threshold exit status, and a
completed run with the fixed dates above. The report records normalization
version, source hashes, timezone/session metadata, interval inclusivity, tape
hash, OHLCV hash, and tape/OHLCV reconciliation results.

### E. Documentation and tutorial verification

- Publish the normative contract and state transition table in cdocs.
- Document every C ABI field and its ownership/lifetime.
- Keep Python `ctypes` layouts synchronized with the C structs.
- Extend the tutorial to print a small order lifecycle trace, including a
  persistent stop-limit activation and a quiet-bar confirmation.
- Run the tutorial under CTest and sanitizer configurations.

## Independent review disposition

Three independent review passes challenged the initial plan from
trading-system architecture, PineForge implementation, and concise red-team
perspectives. Their high-severity corrections are incorporated above:

- replace the two-level revision/fill model with command revision -> executable
  legs -> fills;
- remove every claimed once-per-public-ID-per-bar guarantee;
- separate immutable creation snapshots from mutable leg lifecycle state;
- make trailing and OCA state leg-specific;
- distinguish `TRADE` from `BAR_CLOSE`, including the no-trade POC case;
- preserve actual closing-event timestamp/sequence for POC fills;
- validate unsupported profiles before warmup mutation;
- add handoff-state digests, explicit priority rules, deterministic diagnostic
  overflow semantics, and enforced corpus acceptance thresholds.

Those three passes are summarized here without archived artifacts; future
review rounds must link their artifacts and record rejected findings, not
only accepted ones.

A fourth, seven-perspective review pass (2026-07-11: official TradingView
docs verification, PineTS/PyneCore re-verification at the pinned revisions,
industry architecture comparison against FIX/NautilusTrader/LEAN, two
independent codebase verifications at `98ad849`, an internal red-team, and
TradingView practitioner/community evidence — every serious finding
adversarially re-verified by a second agent) produced this revision. Its
material corrections:

- corrected the stored-artifact claim (a whole-session 2025-05-01 run
  already existed) and the dead PyneCore live-mode citation;
- re-based the current-state audit on `98ad849` and folded #89's
  placement-time pyramiding snapshot into the command-revision model
  (`REJECTED_AT_PLACEMENT`);
- added the documented wait-for-entry exit deferral, slippage,
  trailing immediate-activation, and `calc_on_every_history_tick` to the
  contract; recorded the exit REMARKS fill-first sentence as working-order
  bookkeeping proven by the pinned three-way bracket export (two proposed
  "corrections" here were themselves refuted by corpus evidence and did NOT
  change the leg model — the adversarial verify pass earned its cost);
- named the two deliberate TradingView divergences (fixed-grid quiet-bar
  calculation; OCA one-winner rule pending P8) instead of leaving them
  implicit;
- replaced the open-ended probe deferrals with the section 7 probe plan and
  a completion-gate item;
- scoped the handoff digest for `bar_magnifier`, enumerated the gap
  policies, pinned quiet-bar equity marking, canonical event ordering, hash
  serialization, clock-event validation, diagnostics for today's silent
  drops, `position_episode_id`, OCA membership observability, fill-cardinality
  wording (partial fills stay additive), the restart-divergence caveat, and
  the re-baselining escape hatch with pinned baseline/scorer hashes.

## Completion gate

The feature is complete only when all of the following are simultaneously
true:

- the normative contract is public and matches the implementation;
- every section 7 probe (P1-P9) is resolved, its export archived as a corpus
  probe, and its conclusion recorded in this document;
- every in-scope order surface has event-level tests;
- command revisions, executable legs, fills, and their lifecycle are observable
  and deterministic;
- unsupported calculation profiles fail explicitly;
- unit, integration, sanitizer, ABI, and full historical corpus gates pass;
- all probes complete all three whole-session experiments through 2025-05-01,
  with the frozen baseline's engine/corpus/scorer hashes recorded in the
  published report;
- aggregate scores and every accepted divergence are published;
- the tutorial runs from a clean build.
