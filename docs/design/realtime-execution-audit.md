# Realtime execution contract: audit and development plan

Status: design audit; implementation is not approved by this document alone

Date: 2026-07-11

Worktree branch: `codex/realtime-execution-contract`

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
- Therefore PineTS is useful evidence for separating order, trade, and
  position identity, but not a realtime fill oracle for PineForge.

### PyneCore

Audited revision:
[`ffeab9e`](https://github.com/PyneSys/pynecore/tree/ffeab9e5dfe6f063ed2728626df290dae8a0c5e6).

Relevant design observations:

- PyneCore's simulator keeps one pending entry/normal order per order ID and
  replaces the still-unfilled object on reissue. Exit intents are keyed by
  `(exit_id, from_entry)`, allowing one exit ID to fan out across entry scopes.
- Its open-trade list is distinct from the pending-order maps and net position.
- Its [live-mode contract](https://pynecore.org/docs/advanced/live-mode/)
  deliberately suppresses strategy commands during historical warmup and
  starts paper trading in the live phase. That is not PineForge's required
  model because PineForge must preserve warmup positions, equity, and pending
  orders.
- PyneCore separates its simulated `SimPosition` path from exchange-backed
  broker plugins. That separation supports keeping PineForge's deterministic
  simulator distinct from future real order routing.

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
| Fill event | immutable `fill_id`, order leg, and market-event provenance | One terminal fill per executable leg |
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
and terminal state even when they share one exit ID.

## PineForge current-state audit

Audited branch base: `main` at `7a8fc3b`.

| Requirement | Current evidence | Assessment |
|---|---|---|
| Same-instance historical warmup and handoff | `stream_begin()` calls `run()` once, then preserves the instance; `test_streaming` verifies position, pending order, equity/trade continuity | Implemented for the tested path |
| Ordered provider-neutral trade input | `TradeTick` validates price, quantity, nondecreasing time, and strictly increasing nonzero sequence | Implemented; zero sequence delegates tie ordering to the caller |
| Forming OHLCV without lookahead | Events update O/H/L/C/V before confirmation; exact-boundary ticks are assigned after the prior bar is finalized | Implemented for fixed input timeframes; broader cases need tests |
| Higher-timeframe partial aggregation survives handoff | Existing streaming unit test spans historical and realtime input bars inside one 5-minute candle | Implemented for the tested case |
| Default close-only script scheduling | Broker evaluates every trade; `on_bar()` runs only when a script bar confirms | Implemented |
| Same-ID pending replacement | `strategy_entry()` and `strategy_order()` erase pending objects with the same raw ID; exits replace by `(id, from_entry)`; all preserve the old `created_seq` | Partial: command, priority, and executable-leg identity are conflated, and raw-ID erasure crosses command categories without a targeted parity probe proving the intended collision rule |
| One terminal transition per executable leg | Filled pending objects are erased from `pending_orders_` | Implicit only; generated legs, terminal state, and reason are not retained or observable |
| Market and stop fills on observed trade events | Streaming tests cover next-event market fill and stop gap-through at the observed price/time | Implemented for tested cases |
| Limit fill in streaming | Uses the generic point-bar evaluator | Not directly tested |
| Stop-limit activation across events | `PendingOrder::stop_limit_activated` exists, but normal `process_pending_orders()` does not persist the activation returned by `resolve_entry_stop_limit_fill()`; persistence is currently gated to the historical COOF scheduler | Defect |
| Trailing stop across events | Tick points update one global `trail_best_price_` for the position | Architectural defect for concurrent trailing legs with different lots, creation times, activation levels, or offsets |
| OCA cancel/reduce on one event | Generic fill loop contains OCA behavior and excludes siblings by public ID | Historical tests exist; public-ID exclusion cannot distinguish same-ID executable siblings or revisions |
| `process_orders_on_close` | A close-price point pass prevents newly created orders from inspecting the elapsed wick | Partial: it reuses the bar-open timestamp and has no closing-event sequence, so fill provenance is wrong; it can also fill a no-trade synthetic bar |
| Quiet intervals | Clock advancement creates zero-volume carry-forward bars | Contract issue: both the pre-script broker pass and the post-script POC pass can create synthetic fills, excursions, trailing changes, or margin actions without an observed trade |
| Out-of-session intervals | Empty closed-session intervals are skipped; an actual input event remains authoritative | Partial; session-edge and first-event-after-reopen tests are missing |
| Event-level diagnostics | No command-revision, executable-leg, fill, or decision log in the public report | Missing |
| Replay/live equivalence | `push_ticks()` is a loop over `push_tick()` and a basic ordering test exists | Structural implementation is sound; result equivalence needs an explicit test |
| Handoff state equivalence | Tests inspect selected visible fields after warmup | Missing a canonical digest covering all broker, Pine, TA, security, aggregation, risk, session, and sequence state |
| Three full corpus handoffs ending 2025-05-01 | `run_stream_corpus_mmap.py` supports exactly three starts and one contiguous mmap tape | Runner exists, but exit status currently fails only on runtime errors, not parity/invariant failure; current build artifacts prove only short 10-minute sessions |
| Historical TradingView regression | Required `ctest` and `scripts/run_corpus.sh` gates exist | Must be rerun after implementation |
| Public docs/tutorial | Streaming lifecycle and Python tutorial exist | Lifecycle is documented; normative state machine and diagnostics are missing |

The currently stored short-session reports cover 252 probes per run. They show
zero runtime errors and equal processed-bar counts, but they are ten-minute
experiments and therefore do not satisfy the required three whole-session
experiments through 2025-05-01.

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
TradingView realtime feed or executor parity is not a goal.

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
  and `from_entry` creation-time cutoff semantics.
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
- `calc_on_every_tick` rollback and `varip` semantics.
- Realtime `calc_on_order_fills` re-execution. Historical COOF support remains
  unchanged; a streaming start using this profile must fail clearly until a
  dedicated realtime scheduler is implemented.
- Alert/webhook delivery. The lifecycle events added here are intended to be
  the deterministic source for a later alert layer, including JSON payloads.

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

Whether the same public text ID is one global order key or belongs to separate
entry/raw/exit namespaces must be pinned by focused TradingView probes before
changing the current implementation. `strategy.cancel(id)` is documented to
cancel all pending orders with that ID, but that does not by itself prove the
replacement collision rule between different placement commands. The internal
revision model must represent the proven public rule without relying on a raw
string comparison accidentally.

The command revision is immutable; the executable leg's lifecycle state is
mutable. Activation, trailing watermark, active stop, remaining quantity, and
OCA reductions live on the leg, not on the command revision and not globally on
the net position.

### 2. Define the order state machine

Executable-leg states:

- `PENDING_MARKET`
- `PENDING_LIMIT`
- `PENDING_STOP`
- `PENDING_STOP_LIMIT`
- `ACTIVE_STOP_LIMIT`
- `PENDING_TRAIL_ACTIVATION`
- `ACTIVE_TRAIL`
- terminal `FILLED`, `CANCELLED`, `REJECTED`, `EXPIRED`, `REPLACED`

Required transitions:

- command creation -> one command revision -> one or more pending executable
  legs;
- same-key reissue while pending -> old command revision and remaining legs
  `REPLACED`, new command revision and legs with a fresh revision identity;
- exit fan-out -> independently priced/reserved legs for each eligible entry
  lot or lot group, including distinct TP, SL, and trailing legs;
- stop-limit stop touch -> `ACTIVE_STOP_LIMIT`, durably retaining the limit;
- trail activation -> `ACTIVE_TRAIL`; favorable events ratchet its watermark;
- eligible price event -> exactly one `FILLED` transition;
- OCA fill -> sibling leg `CANCELLED` or quantity-reduced event, excluding only
  the exact filled `order_leg_id` rather than every sibling with the same
  public ID;
- risk/margin gate -> `REJECTED` with a reason code;
- explicit cancellation -> `CANCELLED`;
- stream end does not implicitly fill a partial bar.

Every transition records the before/after state, command revision, executable
leg, public IDs, entry-lot scope, bar index, event timestamp/sequence, observed
price, evaluated trigger level, resulting quantity, and stable reason code.

The contract must separately specify:

- whether unchanged stop-limit parameters preserve activation across a
  same-key reissue;
- whether unchanged trailing parameters preserve activation/watermark across a
  reissue, and when changed parameters reset them;
- how `strategy.exit()` without `from_entry` persists for later entry lots;
- the creation-time cutoff for `strategy.exit(..., from_entry = X)`;
- exact simultaneous-event priority among carried market orders, price-based
  entries, exits, trailing legs, OCA effects, reversals, risk gates, and margin
  actions, including whether eligibility is recomputed after every fill.

### 3. Separate scheduler, bar builder, and broker

Treat these as independent normalized events:

- `TRADE(timestamp, sequence, price, quantity)` supplies an executable price
  and updates a forming bar.
- `BAR_CLOSE(boundary, policy)` confirms a bar selected by the configured
  provider/session gap policy. It carries no new executable price by itself.

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
   intervals become bars. Fixed-grid carry-forward bars are one supported
   policy, not a universal inference from a trade tape.
2. Confirm those bars and run close-only strategy calculations.
3. Do not fill market, limit, stop, stop-limit, trailing, immediate, or POC
   legs; do not ratchet trails, update price excursions, or run price-triggered
   margin liquidation. A clock confirms time; it is not an executed trade.

When a bar with at least one trade confirms, retain the bar's last trade
timestamp and sequence as its closing-event provenance. A POC/immediate leg
created by the confirmed-close calculation may evaluate only against that
observed close event and must report its actual event timestamp/sequence, not
the OHLCV bar-open timestamp. Previously evaluated carried legs are not walked
over the close a second time. When a confirmed bar has no observed trade, new
POC/immediate legs remain pending until a later eligible trade because no
executable closing event exists.

For an event exactly on a boundary, the preceding bar confirms first and the
event belongs to the new bar. An order created by the preceding close can then
fill on that boundary event because it is the next observed price event.

### 4. Make unsupported calculation profiles explicit

`stream_begin()` must inspect effective strategy properties. V1 accepts only
the close-only profile. It rejects realtime `calc_on_every_tick` or
`calc_on_order_fills` with a stable diagnostic instead of executing a subtly
different model. Validation must occur before warmup mutates the handle, or the
entire begin operation must be transactional and restore a clean pre-call state
on rejection. Tests cover declaration defaults and runtime property overrides.

### 5. Add diagnostics without coupling to transport

Add a bounded or caller-drainable order-event stream to the engine report/API.
Diagnostics are simulator facts, not log strings. JSON serialization,
webhooks, retries, and delivery belong above this API.

Batching and caller drain timing must not change simulator results or the
canonical lifecycle sequence. Maintain a rolling canonical event hash/count
independent of retention, and define deterministic capacity, overflow, and
drop counters. Equivalence tests compare the canonical hash and, when no
overflow occurs, the complete retained records.

At minimum expose:

- command revision, executable leg, priority, fill, and entry-lot identities;
- timestamp, source sequence, input/script bar index;
- command kind, leg kind, order state, transition, reason;
- `id`, `from_entry`, OCA name/type;
- side, requested/remaining/filled quantity;
- observed, stop, limit, trail activation, trail watermark, and fill prices;
- position size and equity immediately before and after the transition.

### 6. Keep historical and realtime kernels consistent but not conflated

Reuse risk, sizing, commission, trade allocation, and position mutation logic.
Do not send one-price realtime events through a full inferred-OHLC path helper
when that helper has path assumptions or state side effects. The event evaluator
should choose eligibility at one observed point; the historical evaluator can
continue resolving an inferred or magnified path into the same transition and
fill-application kernel.

As part of this split, move stop-limit activation and every trailing activation,
watermark, and active-stop value onto the relevant executable leg. The current
global position trail watermark cannot represent concurrent trails created at
different times or for different entry lots.

## Verification method

### A. State-machine unit tests

For both long and short directions where applicable:

| Surface | Required event sequence |
|---|---|
| Market | created at confirmed close; fills once at next observed trade |
| Limit | no fill on wrong side; fills limit-or-better on first eligible observed trade; gap behavior pinned |
| Stop | no fill before trigger; gaps to first observed price through the stop |
| Stop-limit | limit seen before stop does nothing; stop activation persists across events/bars; later limit touch fills once |
| Trailing | inactive before activation; per-leg activation persists; watermark only ratchets favorably; two concurrent lots/offsets remain independent; reversal fills at the active level/gap rule |
| OCA cancel | deterministic winner cancels every sibling except the exact filled leg before another can fill on the same event, including same-public-ID siblings |
| OCA reduce | fill quantity reduces sibling remaining quantities exactly once |
| Pyramiding | multiple entry lots can share an entry ID; cap applies to open `strategy.entry` trades, not raw orders |
| Same-ID lifecycle | reissue before fill replaces with a fresh command revision while priority follows the pinned rule; reissue after fill creates new legs; no leg fills twice |
| Cross-command ID | focused entry/raw/exit/cancel probes pin whether identical public text collides or coexists |
| Exit fan-out | two same-ID entry lots at different prices receive independently priced relative TP/SL/trailing legs; no-`from_entry` persistence and `from_entry` cutoff are pinned |
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
  aggregators, and deterministic sequence counters.

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

For each probe and start:

1. Slice warmup OHLCV strictly before the handoff from the canonical source.
2. Slice one contiguous raw-trade tape from handoff through the common end.
3. Run a bar-only baseline from the same OHLCV origin through the common end.
4. Run warmup plus the one contiguous event tape on the same strategy instance.
5. Produce per-probe and aggregate scores.

Before implementation, run and freeze the same three-session experiment on the
current merged streaming implementation. That baseline does not define correct
order semantics, but it prevents an arbitrary post-change acceptance threshold
and exposes performance/parity regressions introduced by the refactor.

Required report scores:

- runtime success count;
- input/script confirmed-bar count equality;
- trade-count equality;
- ordered structural match percentage using direction, entry/exit minute, bar
  indices, and quantity;
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
  intended semantic correction and published first-divergence trace;
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

## Completion gate

The feature is complete only when all of the following are simultaneously
true:

- the normative contract is public and matches the implementation;
- every in-scope order surface has event-level tests;
- command revisions, executable legs, fills, and their lifecycle are observable
  and deterministic;
- unsupported calculation profiles fail explicitly;
- unit, integration, sanitizer, ABI, and full historical corpus gates pass;
- all probes complete all three whole-session experiments through 2025-05-01;
- aggregate scores and every accepted divergence are published;
- the tutorial runs from a clean build.
