# Native engine {#native_engine}

@tableofcontents

A hand-written C++ strategy runs the **standalone native** path: one
`NativeRunSpec`, one working request roster, one physical lot book, and the
host callbacks below. PineScript is not involved at any point — no
`strategy.*` command, no codegen, no source adapter — and the same kernel
answers a batch, a stream and a C caller.

Subclass `pineforge::NativeStrategyHost`. Configure with `configure_native`,
then `run` or `stream_*`. Submit from native begin/bar callbacks, or between
realtime inputs on the same thread. Do not override the inherited `on_bar`
(it is `final` and refused). Do not write protected engine fields.

**The callbacks.** Only `on_native_bar` (`native_host.hpp:874`) is
pure-virtual: it is the script-bar calculation, and a host that overrides
nothing else is a complete, correct host — every other callback below has a
default that is the established behaviour. The surface is **not** close-only.

| callback | when | section |
|---|---|---|
| `prepare_native_begin` (`native_host.hpp:846`) | once, with the begin's own arguments, before the run starts | *Lifecycle and run identity* |
| `on_native_run_begin` (`native_host.hpp:853`) | once, after the reset, before any bar; the one place `declare_timeframe_subscriptions` and `declare_auxiliary_feed` are legal | *Higher-timeframe series for native hosts* |
| `on_native_input` (`native_host.hpp:856`) | once per accepted confirmed input, before it is aggregated or matched | *Calendar, session, timeframes, warmup* |
| `on_native_tick` (`native_host.hpp:859`) | once per accepted realtime print, before it is matched | *Batch OHLCV vs ticks vs quiet* |
| `on_native_timeframe_bar` (`native_host.hpp:866`) | once per delivered bucket of a declared series | *Higher-timeframe series for native hosts* |
| `on_native_bar_open` (`native_host.hpp:870`) | at the modeled opening, before that point's matching pass | *Native requests* (and its lookahead warning) |
| `on_native_bar` (`native_host.hpp:874`) | the script bar's own calculation | *Calculation timing* |
| `on_native_recalculate` (`native_host.hpp:893`) | every calculation of the run, tagged with its reason; the default forwards to `on_native_bar` | *Calculation timing* |
| `on_native_sub_bar` (`native_host.hpp:908`) | after each retained lower-timeframe sub-bar's whole path | *Sub-bars* |
| `on_native_applied` (`native_host.hpp:918`) | after each applied execution — the calculate-on-fill point | *Native requests* |
| `on_native_margin_call` (`native_host.hpp:981`) | right after the `on_native_applied` of a kernel liquidation's own fill | *Margin and liquidation* |

**The answering hooks** — each is consulted, and each has a default that is
the kernel's own answer: `resolve_execution_terms` (`native_host.hpp:927`),
`validate_execution_precommit` (`native_host.hpp:940`),
`resolve_margin_requirement` (`native_host.hpp:956`), `margin_check_allowed`
(`native_host.hpp:968`), `resolve_margin_call_units` (`native_host.hpp:975`),
`resolve_anchored_level` (`native_host.hpp:992`), `owns_lot_excursions` /
`closed_lot_excursion` (`native_host.hpp:1010`) and the hash seam
`hash_host_extension`. Each is documented beside the feature it shapes.

**Declaring a hook the host does not have.** Two of those defaults still cost
the kernel work at every bar or fill. A host whose `on_native_bar_open` does
nothing says so with `declare_native_bar_open_hook(false)`
(`native_host.hpp:1027`), and one that keeps the default
`validate_execution_precommit` with `declare_native_precommit_hook(false)`
(`native_host.hpp:1035`). The kernel then makes no bar-open call, and neither
consults the precommit hook nor builds the settlement preview it would have been
shown, unless the host owns lot excursions, which that preview's closing rows
consult. It keeps every effect of its own that the skipped call's boundary has,
so a declaring host's run is its empty-hook run, value for value. A declaration
stands, across runs, until the host makes another. A C host's callback table
declares both (*Driving the kernel from C*).

**Where to go from here.** *Lifecycle and run identity* → *NativeRunSpec*
(price grid, feed policies, the intrabar path, validation) → *Native requests*
(triggers, intents, owners, groups, brackets, trails) → *Close execution* →
*Margin and liquidation* → *Risk limits* → *Calculation timing* → *Reporting
for native hosts* (the equity curve, the per-bar broker hashes, the closed
rows and their close cause) → *Reading the run back* → *Calendar, session,
timeframes, warmup* → *Higher-timeframe series* and *The auxiliary finer
feed* → *Batch OHLCV vs ticks vs quiet* → the worked *C++ example* → *Sizing
without a host override* → *Examples* → *Driving the kernel from C* →
*Building the kernel only*.

Headers: `<pineforge/native_host.hpp>`, `<pineforge/native_run_spec.hpp>`,
`<pineforge/native_fx_curve.hpp>`, `<pineforge/native_order.hpp>`,
`<pineforge/native_calendar.hpp>`,
`<pineforge/market_driver.hpp>`, `<pineforge/order_action.hpp>`,
`<pineforge/execution.hpp>`. Enumeration members live in those headers; this
page does not re-list every enumerator. Request-value members live in
`<pineforge/native_order.hpp>`.
`<pineforge/native_toolkit.hpp>` is header-only and additive — the bracket
builder and the id book over the primitives below.
`<pineforge/native_module.hpp>` is separate: it is needed only to export a host
as a loadable module (see @ref native_engine_examples).
`<pineforge/native_c_api.h>` is the C spelling of all of it.

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
| `Failed` | A non-recoverable failure is durable: use a fresh host. A cooperative `Aborted` failure is recoverable; `configure_native` may reuse the aborted host with the same session key and a higher run number. |

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

`native_continuation_hash()` folds the resolved timezone identity of the run by
its **content**: the source kind, the effective definition and
`TimezoneIdentityDescriptor::resource_digest`, an FNV-1a 64 over the bytes of
the zone files the resolver actually read. `zoneinfo_root` and `resource_paths`
stay on the descriptor as diagnostics and are not folded, so the same spec over
the same bars answers the same value on a glibc host reading
`/usr/share/zoneinfo` and on macOS reading
`/private/var/db/timezone/tz/2026c.1.0/zoneinfo` (R5 lane E23; before it the
paths were folded and no two hosts agreed). What still moves it is a tzdata
release that rewrites the zone's rules — the run then really did read different
rules — so it is an identity to compare, not a source constant: compare it
between runs — to prove that stating a field at its default changes nothing, or
that opting in moves the identity — and pin a constant only where the installed
zone data cannot enter it. For that use `native_run_spec_digest(spec)`
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
- `report_policy`: `HostRecorded` (default), `KernelRecorded` or
  `KernelRecordedAtHostMarks`; `report_open_position_at_end`: `false`
  (default). See *Reporting for native hosts* below.
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
- `margin`: the generic per-side broker margin model, including its two money
  bases (`basis`, `level_base`) and its check mode. Mutually exclusive with
  `initial_margin_fraction` (setting both is `MarginModelConflict`). Its
  presence is what enables the model at all. See *Margin and liquidation*
  below.
- `subscriptions`: declared higher-timeframe series of the run's own symbol.
  Empty is the whole default surface; see "Higher-timeframe series for native
  hosts" below.

`validate_native_run_spec` / `normalize_native_run_spec` report the first
error field. `configure_native` copies a candidate, normalizes it, then stages
it atomically. Admitted numeric `-0` fee becomes `+0`; other literals are not
rewritten.

### Feed shape, path order and abort reporting

Four spec fields shape how the kernel *reads* the feed and how it *presents* a
refusal. Each is generic, and all four are folded into the continuation identity
unconditionally, including their defaults; changing any of them changes the
identity.

- `slot_label_policy` (`NativeSlotLabelPolicy` `native_run_spec.hpp:323`) —
  `Canonical` (default) requires every confirmed input to carry its calendar
  slot label (`interval.open_ms`, or the scheduled clipped
  `eligible_open_ms`). `FeedTolerant` accepts a provider's own strictly
  increasing labels instead. `LegacyTolerant` is the deprecated spelling of
  the same value and hashes identically.
- `legacy_tolerance` (`NativeFeedTolerance` `native_run_spec.hpp:344`) — a bit
  mask of admission exceptions for a tolerated feed shape, separate from slot
  labels because a host may want the tolerant price admission and canonical
  labels. `BatchStructuralBars` admits finite but non-positive OHLC and reads
  a NaN volume as "activity unavailable"; `WarmupNonNegativeOHLC` admits
  finite non-negative interim warmup values, while the final warmup close
  stays strictly positive. `native_feed_tolerance_enabled` tests one bit.
  `NativeLegacyTolerance` is the deprecated spelling of the type.
- `path_order` (`NativePathOrder` `native_run_spec.hpp:334`) — `Auto`
  (default) keeps the open-proximity rule that decides whether a modeled bar
  walks its high or its low first. `HighFirst` / `LowFirst` state it, so a
  replay or a live host does not depend on that inference.
- `abort_reporting` (`NativeAbortReporting` `native_run_spec.hpp:41`) —
  `Error` (default) presents a cooperative abort as an error diagnostic;
  `Quiet` presents it as a status result, for a host that models cancellation
  itself. It is presentation only: the run still ends `Failed` (`Aborted`).

`timeframe_undetected` is the fifth: a public begin with fewer than two bars
may not establish a timeframe, and setting this flag preserves that state
explicitly instead of inventing a clock literal. It is incompatible with
`subscriptions` (`SubscriptionWithoutTimeframe`) and with `auxiliary_feed`
(`AuxiliaryFeedWithoutTimeframe`).

### The intrabar path

`NativeRunSpec::intrabar` (`IntrabarPath` `native_run_spec.hpp:377`) is a
`std::variant` the spec owns — deliberately a spec value, not a caller borrow,
because matching may need the finer bars later, while sealing an aggregated
script bar:

- `IntrabarPath::none{}` (default) — the confirmed OHLC path only.
- `IntrabarPath::lower_tf{bars, tf, samples, distribution, volume_weighted,
  volume_weighted_min_samples, volume_weighted_max_samples,
  sample_eligibility}` — a **retained** finer feed. It is what makes
  `on_native_sub_bar` reachable, and what gives the margin model a sample to
  re-evaluate at instead of a whole-bar waypoint.
  `IntrabarPath::SampleEligibility::ContinuousSegments` (default) keeps
  continuous matching between generated samples; `DistributionSamples`
  restricts eligibility to the sample points themselves.
- `IntrabarPath::synthesized{…}` — the same sampler over the script bar's own
  OHLC path, with no retained feed. Its eligibility is point-only by
  construction, so it carries no `sample_eligibility` member and delivers no
  sub-bars.

`is_none()`, `lower()` and `synthesized_path()` are the accessors. The C
spelling is the `PF_NATIVE_SPEC_EXT_INTRABAR` block of
`pf_native_run_spec_ext_v1`, and `PF_NATIVE_INTRABAR_LOWER_TF` is the only
value that retains a feed. Pinned by `tests/test_native_auto_path.cpp` and
the sub-bar sections of `tests/test_native_calc_timing.cpp`.

### Validating a spec

`validate_native_run_spec(spec)` (`native_run_spec.hpp:805`) answers a
`NativeRunSpecValidation` (`native_run_spec.hpp:788`): a
`NativeRunSpecError` (`native_run_spec.hpp:677`) and the
`NativeRunSpecField` (`native_run_spec.hpp:645`) it first failed on, with
`ok()` and an explicit `operator bool`. `normalize_native_run_spec(spec)`
(`native_run_spec.hpp:815`) validates and rewrites the one admitted literal —
a numeric `-0` fee becomes `+0` — leaving every other literal alone. Neither
allocates on the failure path, neither changes a spec it rejects, and the
field order is deterministic, so a host can report "which field" rather than
"invalid".

`configure_native` runs both over its own copy and answers a
`NativeSetupResult` (`native_host.hpp:385`): a `NativeSetupStatus`
(`native_host.hpp:380`, `Applied` or `Failed`) beside that same validation.
`configure_native_fx_curve` answers the curve's counterpart,
`NativeFxCurveSetupResult` (`native_host.hpp:393`), carrying a
`NativeFxCurveValidation` (`native_fx_curve.hpp:40`) — a
`NativeFxCurveError` (`native_fx_curve.hpp:28`: `LengthMismatch`,
`NotStrictlyIncreasing`, `NotFinitePositive`, `AllocationFailure`,
`WrongPhase`) and the index of the first bad point. Empty parallel arrays are
valid and clear the curve. `validate_native_fx_curve`
(`native_fx_curve.hpp:48`) is the same judgement as a pure query.

`native_run_spec_digest(spec)` (`native_run_spec.hpp:891`) is the portable
constant described under *Lifecycle and run identity*: exactly the fields the
consumer folds into a run's continuation identity, and nothing else. Each
feature suite pins the refusals of the fields it owns — the eighteen
`NativeRunSpecError` rows of `tests/test_native_auxiliary_feed.cpp`, for
instance — and the digest's "a field stated at its default moves nothing"
property is pinned beside the feature that added the field
(`tests/test_native_calc_timing.cpp`, `tests/test_native_risk_limits.cpp`,
`tests/test_native_htf_subscriptions.cpp`).

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
- **Exactness.** A ladder price is a fixed point of the grid: a level
  bit-identical to either binary64 spelling of its tick — `k * price_tick`, or
  for a decimal tick `k / (1 / price_tick)`, the double a decimal literal
  parses to — maps to index `k` whatever the quotient's last bit says (the
  nanotick guard alone misindexes ladder prices from about k = 1.7e7, a
  five-decimal instrument above 167.82), is booked as presented by
  `QuantizeFills`, and is its own limit-or-better cap. The `HalfUp` trigger
  threshold is the exact boundary of `grid_round_half_up` — the last raw price
  whose nearest tick is still inside the region — not the product
  `(k ± 0.5) * tick`: an exact half-tick print rounds away from zero and so
  lies outside a `<=` region. The Pine adapter still runs with `None`.
  TradingView quantizes per order kind — stop and limit legs and a trail's
  activation, with or without a trailing offset (R5 lane E5's tapes, below)
  and at the placement close too (lane E9), against the tick-quantized bar,
  a one-shot booking the tick that bar reaches; the trail stop, the running
  best, stop-limit entries and the calc_on_order_fills cursors against the raw path
  (engine.hpp, design-stop-tick-rounding and design-trail-activation-tick-bar)
  — whereas the run spec's grid is one rule for every trigger the matcher
  tests. A per-kind grid mask would spell that inconsistency into the kernel
  and is not generic. What was measured as this row's divergence — the trail
  stop, where `best - offset` lands one ULP under a ladder point on about 14%
  of (best, offset) pairs on a two-decimal feed, so the grid fired it a bar
  before the raw path did — was not the per-kind rule at all but a level the
  kernel spelled wrong on both paths, and R5 lane E16 closed it: see *A trail
  stop a whole number of ticks away* below. The two paths now agree on that
  row (tests/test_adapter_grid_relower.cpp section 5).
- **The reached rule.** Under `QuantizeFillsAndTriggers` the
  tick-quantized print **is** the reached price. The matcher's verdict is
  authoritative, and the core re-validates every activation — stop,
  stop-limit, trail arm, trail stop — on the same ladder with the same
  arithmetic: the consumer hands `native_order::ActivationGrid` to
  `prepare_trigger` the way acceptance receives `CommandContext::price_tick`,
  so a hit the matcher reports is never refused. Reached means the level
  itself (a crossing hit books the level) or a print inside the quantized
  region: its nearest tick under `HalfUp`, its enclosing tick on the region's
  side under `Directional`, at or past the level's own ladder point. The
  activation's recorded `reached_price` is that quantized print. A limit has
  no core re-validation; its gate is limit-or-better on the resolved price,
  which the grid's cap keeps. One rule for every kind: a trail's running best
  is the quantized print too (nearest under `HalfUp`, the favourable enclosing
  tick under `Directional`), `best - offset` is tested by the same rule, and a
  zero-offset ride exits on the first print strictly past the best on the
  ladder. TradingView's per-kind split (a raw best and a raw trail stop next
  to quantized stop and limit legs) is not spelled here; the adapter keeps it
  on `None`. The fill books where the mode already says a fill books: the
  level, or the gapped open, then the fill rounding, then
  `resolve_execution_terms`. Example: a buy stop at 100.50 on a 0.25 ladder
  with the bar opening at 100.40 activates at the open and books 100.50; under
  `None` and `QuantizeFills` the raw open never reaches it and nothing fills.
- Scope per mode. `None` and `QuantizeFills` keep the raw compare on the raw
  print for every activation, bit for bit (`QuantizeFills` quantizes fills
  only). Only `QuantizeFillsAndTriggers` tests and re-validates on the ladder.
  The alternative reading — the raw print stays authoritative and the matcher
  must not accept a quantized touch at a resting cursor — was rejected: an
  intrabar crossing already books a quantized touch at the level, so it would
  make a gapped open second-class for no reason but the core's compare, and
  it would collapse the trigger mode toward `QuantizeFills`. A leg born at a
  bar's calculation is first tested at the next point (its own birth print is
  excluded), so a `close_execution = AfterCalculation` leg reissued at the
  close and reached by that close's print fills at the next open
  (tests/test_native_price_grid.cpp, sections L8b-1 to L8b-4).
- The Pine adapter's grid re-lowering is waived: measured-infeasible. The
  trial has the
  adapter submit raw levels (`source_trigger_threshold` answering the level
  itself) and `project()` declare `QuantizeFillsAndTriggers` with `HalfUp`.
  The first blocker is closed — no run aborts any more (the first trial
  measured nine NYSE:F / AAPL zero-offset-trail tapes and six
  `process_orders_on_close` panels failing with "native working-request
  preparation failed"; now
  `test_zero_offset_trail_rides_l4c` is 449 of 450 and
  `test_pooc_short_close_tick_l4d` 183 of 183). The second blocker is what is
  left and it has no adapter-side remedy: TradingView quantizes per order
  kind, the grid is one rule for the run. The old N13 trial moved 30 pinned checks in four units; that is a historical
  measurement, not a current-head result. The current native-only ruling keeps
  `NativePriceGrid::None` for the adapter. The four units were — 24 in `test_coof_market_limit_recross_l4c` (an on-grid exit
  limit stays raw under `calc_on_order_fills`), 3 in
  `test_stop_tick_rounding_l4d` (a stop booked 14.01 on another bar where
  TradingView books 13.98), 2 in `test_adapter_grid_relower` (the trail stop
  one ULP under its ladder point exits a bar early) and 1 in
  `test_zero_offset_trail_rides_l4c` (the quantized running best) — plus 12
  checks with an adapter-side cause (the grid pre-rounds
  `default_resolved_price`, 196.135 booking 196.14 for TradingView's 196.13;
  a rounded-money liquidation twin; eight pending-book digests where a
  bit-equal re-issue keeps a request the adapter replaces today). The corpus
  cannot arbitrate: all 312 probes run a 0.01 tick on an on-grid feed, and
  under the trial 5 of them differ only in the engine-only entry-incarnation
  column. No class of triggers is byte-identical on its own, because the
  grid is a run-wide switch and a per-kind mask is not generic; the adapter
  stays on `None` and keeps `source_trigger_threshold`.
- **Native-only by ruling.** The waiver above is final, and it
  is not a gap: the grid is a native-host feature whose consumers are native
  hosts. ADR-0001 ("Kernel capabilities the Pine adapter does not declare")
  records the ruling and `scripts/check_native_feature_rulings.py` holds it:
  the check fails if `project()` ever starts assigning `price_grid` while the
  ruling stands, or if the ruling loses its executed consumers.
  `examples/native/native_price_grid_strategy.cpp` runs one strategy on one
  sub-tick tape under `None`, `QuantizeFills` with `HalfUp`, `QuantizeFills`
  with `Directional` and `QuantizeFillsAndTriggers`, and asserts every booked
  price against a hand-computed one (the same market entry books 100.10,
  100.00, 100.25 and 100.00; the breakout stop at 99.75 fills only under the
  trigger mode, from a raw high of 99.65); `native_price_grid_c.c` is the same
  host through the C API, with the same numbers.

Timeframe arguments on `run` / `stream_begin` must be **omitted/empty or
byte-identical** to the spec. Conflicting values are a preflight refusal:
`Ready`/`Running` is preserved. Magnifier/source-feed arguments are not native
spec fields.

The rich `run(bars, n, input_tf, script_tf, inputs, syminfo, overrides, …)`
overload (`engine.hpp:1623-1634`) is **not** refused as a source mutation: it
reaches `NativeExecutionConsumer::run_rich`
(`native_execution_consumer.cpp:8954-8994`), which admits the begin, checks the
timeframe arguments against the spec, preflights and pumps the batch exactly
like the plain overload. `inputs` / `syminfo` / `overrides` are carried only as
`NativeBeginArgs` fields to `prepare_native_begin` — the overrides as the
opaque `overrides_opaque` (`native_host.hpp:739`), which the kernel forwards
and never dereferences. No test pins either behaviour; prefer the plain
overload. What *is* refused is source **mutation** through the setters
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
never reentrantly during input processing. A host written in C issues the same
five commands through `strategy_native_submit_v1` / `_replace_v1` /
`_cancel_v1` / `_cancel_all_v1` / `_cancel_where_v1`
(`native_c_api.h:2689-2739`), under the same legality rule; see *Driving the
kernel from C* below.

`native_order::Request` values belong to `native_order_v7`
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

A limit is two facts. Its level gates the fill: the modeled path has to reach
it, and a crossing books the level itself while a point already inside the
region (a gap open) books that print. Its bound caps the resolved fill at the
level after slippage. `Limit{price, fill_through}` keeps the gate and drops
the bound (market-if-touched): slippage, or a host's own
`resolve_execution_terms` answer, may carry the fill past the level, where a
bounded limit is capped or — for host terms past the level — refused with
`InvalidTerms`. Without slippage the two book the same fill; the flag is
durable request state and is folded into the identities whether or not it
mattered. Pinned from a bare host, both sides, in
`tests/test_native_limit_fill_through.cpp`.

At host epoch `engine_script_run_v19` (`native_host.hpp:20`), general requests
also support explicit
`native_order::ReverseTo{signed_units}` and `HostSized`. A `HostSized{Open,
Side}` request binds its units at a matching candidate through the host's
`resolve_execution_terms` override. The host may choose `Transact`, exact
`ReverseTo`, or whole-opposite-book close shape there; it does not supply a
second matcher, book, or cash path. `submit_market` remains the deliberately
narrow market-default convenience surface.

### Selected exposure and current execution

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
notifications FIFO until the outer callback returns and passes the abort check.
Callback depth stays one; event values remain valid during later
submissions. Notifications never automatically execute newborn requests and
have no semantic 64-execution cap. Failure after a physical commit discards the
host; a failed host cannot retry.

The engine fields a run projects from its spec (the capital, point value, FX
scalar and curve, tick, commission and the instrument and zone strings) are
compared with the applied spec before every in-callback execution
(`execute_current`, whose own precondition it is), at run begin, and at the two
ends of every pump: a batch before its first input and after its last, a stream
at each public input (R5 lane V19-C). Every callback and policy-hook boundary
inside a pump checks the cooperative abort alone. A host that writes a projected
field inside a pump — outside the contract, which forbids writing protected
engine fields — fails with `ProjectionMismatch` at the pump's end, with the
pump's operation (`Input` or `Stream`) and ordinal 0; until then the run goes on
with the configuration it wrote, fills and notifications included.

Generated Pine code runs on this same consumer: `source::PineStrategyHost`
derives from `NativeStrategyHost` (`pine_strategy_host.hpp:242`) and lowers
every `strategy.*` command into the native requests above. What the source
layer keeps on top of them is TradingView's *policy* — the command batching,
the priority and activation quirks, the money rounding — never a second
matcher or a second book. The boundary, rule by rule, is
`docs/adr/0001-kernel-adapter-boundary.md`.

`on_native_applied` is the **calculate-on-fill** hook: it is the point at which
a host reacts to its own execution and may submit again. A request born there,
mid-bar on a continuous segment, is eligible on the **remaining path suffix** of
that segment — the birth is admitted at the current cursor and the geometric
search then sees only the unconsumed suffix (`born_on_remaining_path`,
`native_execution_consumer.cpp:5463-5467`). Requests accepted before the
segment, and discrete points, keep the ordinary birth gate above.

`on_native_bar_open` fires at the modeled opening, before that point's matching
pass (`native_execution_consumer.cpp:6910-6912`). **Lookahead warning:** the
`Bar` it receives is the *complete* script bar — the consumer has already set
`engine.current_bar_ = open_view` (`native_execution_consumer.cpp:6802`), the
complete bar unless the spec asks for `NativeOpenBarView::OpenOnly` — so its
high, low and close are the finished bar's, not what is known at the open. A
host that must decide on open-only information reads
`current_partial_bar()` (`native_host.hpp:1046`; C:
`strategy_native_partial_bar_v1`), the lookahead-free bar so far at this
cursor, or declares `NativeOpenBarView::OpenOnly`, which masks this one
callback's bar down to its open. Both are under *The bar so far, and the
open-bar view* below.

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

The same book lot by lot is `native_open_lots(mark)`: one
owning `NativeOpenLot` per open physical lot, oldest first, carrying the
lot's identity (`ordinal`, `entry_incarnation`, `cycle`), its booking facts
(`entry_label`, `entry_comment`, `entry_time_ms`, `entry_bar_index`,
`entry_price`, `signed_units`), the entry fee still on it
(`entry_commission`) and three fields marked at `mark`: `unrealized_pnl` —
the lot's own term of `native_marked_equity(mark)`, fee-net, so the realized
balance plus the rows is the marked equity — and the gross
`favorable_excursion` / `adverse_excursion` the kernel has sampled along the
delivered path with `mark` folded in. It reads only what the book holds and
moves nothing (no fill, no hash, no row); a NaN `mark` keeps the booking facts
and leaves `unrealized_pnl` NaN. It is the `strategy.opentrades.*` surface of
a bare host; the field-by-field map is in
[PineScript to native C++](@ref pine_to_native_map_trades). A kernel
liquidation is seen through it and through nothing else: the whole lot at
the bar's open with the level `native_liquidation_price()` solved, then —
after the path breached it — an empty book (`Flatten`) or the same lot,
identity kept, shrunk to the survivor with the level re-solved
(`RestoreMinimum`), and one closed row under the broker's own ticket with
`CloseCause::Liquidation`. Pinned by the two liquidation scenarios of
`tests/test_native_open_lots.cpp`.

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
std::size_t gone = cancel_where("entry-7", NativeRequestField::Label);
```

`native_working_requests()` copies owning value rows in live order; later
commands do not invalidate a row already returned. `cancel_all()` answers how
many requests left the book, dependants of a cancelled owner included;
`cancel_where(comment)` cancels exactly the live requests carrying that
comment and answers how many of those it cancelled. An unknown comment is not
a command.

`NativeRequestField` chooses which of the two identity texts the predicate
compares: `Comment` is the one-argument form, `Label` addresses the requests
by `Request::label` — the one call that withdraws every live request a host
issued under one of its own order ids. Both are free host text the kernel
only copies and compares, and neither is indexed: each form walks the live
book once, exactly as `cancel_all` does, so a label may be reused, replaced
or left empty with no second copy of the book to keep in step. The C spelling
is `strategy_native_cancel_where_v1(host, text, PF_NATIVE_FIELD_LABEL)`; an
id → handle book a host wants to *re-price* from, rather than a predicate, is
`native_toolkit::OrderBook<Key>`.

A leg can be placed before its owner has a price. `Request::anchor` defaults
to `Absolute{}` — the level written in the trigger. `FromOwnerFill{offset,
ticks}` instead defers it: when the owner's fill arms the request, the level
becomes `fill + offset`, with `offset` signed (adverse is negative) and
spelled in price ticks when `ticks` is set. The anchor applies to `Limit` and
`Stop` prices and to a `Trail` arm threshold. **The rule for the anchored
field is that you do not write it**: leave it at its own absence — the `0.0`
`Limit::price` and `Stop::price` already default to, and either an absent
`std::optional` or that same `0.0` for a `Trail`'s `arm_price`. Writing any
other level is rejected, because the arm would silently overwrite it.
`Market`, `StopLimit`, and any owner other
than `WaitForApplied` are rejected too, because only that relation arms. The
`ArmedEvent` carries the materialized definition: from then on the request
reads as the absolute level it now is.

```cpp
native_order::Request stop{native_order::Reduce{native_order::OwnerOpenedUnits{}}, "sl", ""};
stop.owner = native_order::WaitForApplied{entry};
stop.trigger = native_order::Stop{0.0};
stop.anchor = native_order::FromOwnerFill{-10.0, /*ticks=*/true};  // ten ticks under the fill
```

#### Anchored legs as bracket children

An anchored leg is hostable as a first-class bracket child through three
opt-in knobs, each defaulting to the behaviour above, and one measured
fact. The kernel owns the mechanism — the arm, the once-only
materialization, representability, the `ArmedEvent`, visibility, matching —
and the host supplies policy through a hook, exactly as
`resolve_execution_terms` owns the fill price.

- **Rounding.** `FromOwnerFill{offset, ticks, rounding}`: `rounding` is a
  `NativeAnchorRounding` — `Raw` (default: `fill + offset` exactly), `HalfUp`
  (nearest tick, ties away from zero) or `Directional` (toward the region the
  leg needs, relative to its own trigger kind and side exactly as
  `NativeGridRounding` documents: a buy limit down, a sell limit up, a stop the
  other way; a trail arm threshold is reached from the favourable side and
  rounds like a limit). The ladder is `NativeRunSpec::price_tick` and the
  arithmetic is the price grid's own. A non-`Raw` rounding needs a positive tick
  at acceptance (`InvalidTrigger` otherwise, like a tick spelling). It is a
  generic instrument grid; a source language's trigger projection is not a
  kernel option.
- **The arm hook.** `resolve_anchored_level(const NativeAnchoredLevelView&)`
  is consulted exactly once per materialization, before the `ArmedEvent` is
  built. The view is read-only: the owner (handle, its fill's ordinal, the lot
  it opened, its resolved price and cursor), the leg (handle, side — a closing
  leg trades against the lot the fill opened — and trigger kind), the
  resolved offset in price units, the run's price tick and `kernel_level`,
  the level the kernel would install after the rounding. `nullopt` keeps the
  kernel level; a returned value is installed, still subject to the kernel's
  representability check, whose failure is the existing `PreparationError`
  path (`NonrepresentableQuantity`, reported as a settlement failure). A
  throwing hook latches `CallbackException`. TradingView's half-tick arm and
  its ULP walk are a quirk the Pine adapter applies inside this hook, never a
  kernel option.
- **Visibility.** `WaitForApplied{parent, visibility}` — the one owner
  relation that arms — takes a `NativeArmVisibility`: `Working` (default) or
  `PendingUntilArmed`, under which an unarmed leg is not a working order. It
  is a live, accepted request the whole time; visibility governs
  enumeration, not addressing.
- **First match.** `WaitForApplied{parent, visibility, first_match}` takes a
  `NativeArmFirstMatch`. The arm happens inside the owner's fill settlement,
  at the owner's fill print. `AtArmPrint` (default) is the established book:
  the armed leg is a candidate at that very cursor, so a level the fill print
  already satisfies matches at the print. `AfterArmPrint` gives it the birth
  rule of a request submitted from the owner's fill callback: on the driver
  point that armed it the print is consumed — a level already reached does
  not match there, a later crossing on that point still does — and from the
  next driver point on it is an ordinary working request. It governs the
  level test of a priced trigger; a market trigger has no level. Both are
  broker models (a contingent child that enters the book after its parent's
  trade cannot trade on that print; a simulated bracket commonly may), and a
  leg under `AfterArmPrint` is, trade for trade, the leg a host would submit
  from `on_native_applied`.
- **Scope.** `WaitForApplied{parent, visibility, first_match, scope}` takes a
  `NativeArmScope`, which names what an armed *closing* leg closes.
  `OwnerLot` (default) is the established relation: the lot the owner's fill
  opened, and nothing a later add brings. `Book` binds the leg, at the arm,
  to the whole position that fill left — its `ArmedEvent` carries a
  `BookClose` for that cycle and side, bound at the fill's cursor, the very
  authority an `Independent` close acquires — so a protective leg placed with
  its entry also covers later adds and settles through the book like any
  other close. A `HostSized` close may wait for its owner only under `Book`
  (the owner-lot relation sizes from the units the owner opened; a book close
  is sized by `resolve_execution_terms` at the match); under `OwnerLot` it is
  still `InvalidOwner`. A waiting transaction closes nothing and must keep
  `OwnerLot`. Both fields fold into the continuation identity only when set,
  each under its own tag, and an unknown value is refused, never read as the
  default. The C request does not expose them (`native_c_api.h`).
- **Facts at the arm (measured).** `Reduce{OwnerOpenedUnits{}}` is bound at
  the arm to what the owner opened (`QuantityBoundEvent`); a
  `ScopeFraction` with the default `AtMatch` basis resolves at the candidate
  against the owner's lot; `PointBudget` is fixed at submit and metered per
  point after the arm; the group identity is fixed at submit and its effect
  runs at the filling member's fill, after the common arm; a rejected owner
  ends its waiting children (`OwnerGone`). Two rows do not fit a bracket
  child and are recorded, not repaired: a `Sized` intent is never a child
  (`InvalidOwner`), and a `ScopeFraction{AtAcceptance}` freezes the pre-fill
  book at submit. No `SizeTime::AtArm` exists; `OwnerOpenedUnits` already is
  the arm-time size.

The arm chronology of one leg:

1. `submit` — accepted with `Wait` authority (`AcceptedEvent`). Under
   `PendingUntilArmed` it is absent from `native_working_requests()`.
2. The owner fills — its `ExecutionAppliedEvent` seeds the arm drain.
3. Materialization, once: `fill + offset` → the rounding → the hook →
   representability → the level is written into the leg's trigger and the
   anchor becomes `Absolute`.
4. `ArmedEvent` — carries the materialized definition; every later reader
   (the live book, the C working rows, matching) sees that level.
5. Matching from the owner fill's remaining path on (suffix eligibility): a
   waiting leg never matches before its arm under either visibility.

What each reader of the live book sees under `PendingUntilArmed`:

| Reader | Before the arm | From the `ArmedEvent` on |
| --- | --- | --- |
| matching | never matches (waiting), under either value | matches |
| `native_working_requests()`, C `strategy_native_working_*` | hidden | listed |
| `replace` / `cancel` / `trail_state` (by handle) | address it | address it |
| `cancel_all` / `cancel_where` (comment or label) | cancel and count it | same |
| reservation / admission / projected-pending | none in the kernel for a resting request (sibling claims filter by reduction scope; admission reads the physical lots) | same |
| report / trade accessors | settled trades only, no request rows | same |
| continuation / broker-state hash | folded (the visibility folds only when set) | same |
| stream mode | the same consumer and book | same |
| `native_events` | the `AcceptedEvent` is there; the history is not the working book | plus the `ArmedEvent` |

The chosen enumeration semantics are *hidden*, not *flagged*: it is the
knob's definition, it is the shadow-row behaviour a broker that shows no
working child before the parent fills needs, and it keeps the C API's
working rows the same enumeration with no layout change. A
`native_toolkit::OrderBook` key bound to a pending child is re-priced by
handle rather than through the enumeration.

Trailing offsets accept the same two spellings. `Trail::ticks` is a
`TrailTicks{n}` offset resolved against the run's price tick at acceptance:
the accepted request carries the resolved price distance and no spelling, so
a tick-spelled trail and its price-spelled equal behave identically bar for
bar. A zero offset is legal and means *ride the best*: the level is the
running best itself and the exit is the first move strictly past it. Negative
and nonfinite offsets remain rejected, and a tick spelling without a usable
price tick is rejected rather than read as a price.

`Trail::best_seed` says where the running best STARTS (R5 lane E14). Absent —
every trail before the field — the best is the arm's own print: the arm
threshold's ladder point when a crossing armed it, the first print the trail
sees when it was submitted already armed. Present, it is a floor on that
start: at the arm the best becomes the favourable one of the seed and the arm
print, the higher for a sell trail and the lower for a buy trail. That is the
broker shape of a trail that rides from its activation instead of from the
next print, and the level is the host's own number — absolute (an anchor
moves the arm threshold, not the seed), finite and positive, and put on the
ladder like an observed print when the run declares a price grid. The
activation event still reports the print the arm happened at. The request
digest folds the seed only when it is present, so no established hash moves;
its C spelling is `pf_native_request_v1::trail_best_seed`.

**A trail stop a whole number of ticks away is the ladder point it names**
(R5 lane E16). The stop rides `offset` behind the running best, and a
tick-spelled offset is resolved as `ticks * price_tick` — exact arithmetic on
inexact numbers, so the level could land one binary64 ULP off the point the
count names: `11.44 - 5 * 0.01` is `11.389999999999998792` against the ladder
point `11.390000000000000568`, and a print that IS 11.39 then did not reach a
stop the run put five ticks under 11.44. When the run declares a price tick,
the best is itself a ladder point and the offset is a whole number of ticks,
the level is now derived by index arithmetic and spelled so no ULP hides it
from the side the stop is reached from (`native_matching::ladder_trail_stop`,
`ActivationGrid::ladder_tick`). Nothing is rounded ONTO the ladder: a
sub-tick best, a fractional-tick offset and a run with no declared tick keep
the raw subtraction bit for bit, the stop and the running best stay on the
raw path, and it is the level that moves, never the comparison. The core
re-validates an activation through the same geometry, so a level it disagreed
with the matcher on can no longer refuse a hit the matcher booked.

Re-pricing a trail normally restarts it. `replace(handle, request,
ReplaceOptions{/*retain_trigger_state=*/true})` instead carries the
predecessor's live trigger state — a tracking trail's best, an already active
stop — into the successor. Predecessor and successor must hold the same
trigger alternative, and a retained best must still produce a representable
level for the successor's offset; otherwise the replacement is rejected and
the predecessor stays live.

Two more options change what a replacement consumes, never what it matches.
`keep_handle` re-prices the request in place: the successor keeps the
predecessor's handle, its place in the chain (no chain-root pair is folded
again), its children and cohort members, and a trail's arm ordinal, and takes
the number a plain successor would have taken as its priority -- queue ties
still rank a re-priced request newest, and the next request is numbered as
after a plain replace. `keep_binding` carries a close's book binding: at its
next point the successor binds, with no `CloseBoundEvent`, to exactly the book
close the event would have installed when the position has not moved (taking
the timeline ordinal the event would have taken, so every later ordinal is the
plain replace's), and binds as a plain successor does when it has. Either may
be set alone; neither has a C spelling. The Pine adapter sets `keep_binding`
on every re-issue: its placement table, bracket families and pending-order
rows name each placement by the incarnation its command was issued, so it
keeps plain handles.

What the Pine adapter takes from this set is the tick spelling and the
anchored child. A source `trail_offset` is a tick count, so `exit()` hands the
kernel a `TrailTicks` and the acceptance path resolves it against the run's
`price_tick` — the very `syminfo.mintick` the adapter projects — instead of
multiplying the count by the tick itself. A relative bracket leg — a
`strategy.exit(from_entry=…, profit=…, loss=…, trail_points=…)` operand
queued while its parent entry has not filled — is the anchored child above:
at the end of the source evaluation in which the queued exit and its live
parent both exist, `anchor_relative_exits` submits each such leg as the very
request `exit()` submits at the parent's fill, placed ahead of it: a
`HostSized` close under `WaitForApplied{parent, PendingUntilArmed,
AfterArmPrint, Book}` with a tick-spelled `FromOwnerFill{±ticks, true,
Directional}` anchor in the exit's own OCA group. The kernel computes the
level (`fill ± ticks` on the tick ladder); `resolve_anchored_level` restates
only TradingView's spelling of that ladder point and its trigger projection —
the half-tick threshold of the quantized bar, a raw on-grid limit under
`calc_on_order_fills`, the one-shot trail touch — through the same two
functions `exit()` uses. The parent's fill point still runs the source
`exit()` pipeline, because the leg's reservation, birth reach, L4C policy and
same-bar ordering are facts of that point and have no kernel analogue; it
*adopts* the armed child instead of submitting whenever that child is the
request it was about to submit: the same trigger bits, the same OCA group, a
host-sized close of the book. Because the child is host-sized and
book-scoped, that holds for every quantity the source resolves at the match
(a percentage, a sibling's remainder) and for every book the parent's fill
leaves (flat, a reversal, a later same-id add), under `calc_on_order_fills`
and `process_orders_on_close` as well; and because it is `AfterArmPrint`, a
level the fill print already satisfies behaves as the callback-born leg
does. Otherwise the children are withdrawn before the path resumes and the
fill point submits as it always did. What stays off the kernel path, each
with its measurement (`tests/test_adapter_brackets_relower.cpp`):

| Shape | `{anchored, adopted, withdrawn}` | Why |
| --- | --- | --- |
| explicit `qty=` (`rel-qty-explicit`) | `{0,0,0}` | `exit()` stages an explicit quantity per origin (`pending_bracket_legs_`) and submits it at a later flush: the fill point has no request for a child to be |
| `strategy.cancel(exit id)` in the same evaluation (`rel-cancel-exit-id`) | `{0,0,0}` | the definition is gone before the evaluation ends: no leg exists |
| `close_entries_rule = "ANY"`, stream runs | not anchored | the fill-point leg is cohort-bound (`BindCohort`), a scope the arm does not spell; a stream's fill point stages the leg |
| a parent re-issued, cancelled or declined (`rel-limit-parent` `{12,4,8}`, `rel-reissue-changed` `{18,2,16}`, `rel-breakout-pair` `{40,14,24}`, `rel-declined` `{2,0,2}`) | withdrawn | the kernel ends a waiting child with its parent (`OwnerGone`), a replaced parent included; keeping the children across a replace needs a re-parent transition in the event log, which is an epoch decision |
| a sibling with no capacity left (`rel-two-exits` `{4,2,2}`), a level below zero (`rel-negative-short` `{2,1,1}`) | withdrawn | the fill point submits nothing for that leg |

Over the 49 pending-parent scenarios the suite pins, the split is
`{220, 123, 93}`, and every shape but the two recorded above runs on the
kernel. The queued definition and its
shadow row stay: the definition outlives parents (it may be declared before
any entry exists and is re-anchored when a parent is replaced) and is what
the fallback re-runs, and the shadow row is the source projection of a child
the kernel deliberately does not list. The remaining TradingView rules stay adapter
policy because no kernel primitive expresses them: the legacy broker rides
the *tick-quantized* running best, which the adapter still spells as a
half-a-tick trailing distance (`TrailTicks{0.5}`) rather than the kernel's
raw-best zero offset, and an offset-only trail re-issue keeps the live
request rather than replacing it, so `ReplaceOptions{retain_trigger_state}`
has no adapter consumer. See `docs/design/native-feature-parity.md` §1.2 B3
and A.6.

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
if (!legs.every_requested_leg_accepted()) { /* a leg this entry needs is not resting */ }
```

Each present leg is submitted as the parent's `WaitForApplied` child in one
OCA group — group id the parent's incarnation, one cohort per leg, the
caller's `sibling_effect` (default `Cancel`) — in take-profit, stop-loss,
trail order. That is exactly the owner/group shape a host would write by
hand, and the legs' own intent, trigger and anchor are left untouched. A leg
the kernel refuses does not stop the others: the remaining legs are still
submitted.

The receipt says what became of every leg, and nothing about a bracket is
silent. `legs.take_profit` / `.stop_loss` / `.trail` are the handles the legs
rest under and are empty otherwise, so beside each one the receipt carries a
`BracketLegOutcome` — `legs.outcome(tk::BracketLeg::StopLoss)`, or the
`*_outcome` member — whose `state` is one of four:

| `BracketLegState` | what it means | `handle()` | `reason()` |
| --- | --- | --- | --- |
| `NotRequested` | the spec left the leg unset; nothing was submitted | — | — |
| `NotSubmitted` | the leg was asked for, but `spec.parent` was never allocated, so the builder sent nothing and the kernel gave no verdict | — | — |
| `Accepted` | the kernel accepted it | the handle | — |
| `Rejected` | the kernel refused it | — | its `RequestRejectReason` |

`BracketLegOutcome::result` is the kernel's own `SubmitResult` verbatim —
status, event ordinal, handle, reason — so the toolkit spells a refusal in no
vocabulary of its own; `state` is that status widened by the two cases in
which no submit happened. `every_requested_leg_accepted()` is the one
question a host must ask before it treats the bracket as placed: reading only
the optional handles cannot tell a leg that was never asked for from one the
kernel refused, and a refused exit leg that goes unread leaves a position
running without the protection its strategy believes it placed.
`examples/native/native_bracket_strategy.cpp` reads and asserts every leg.

The toolkit is C++-only and has no C spelling. It is header-only composition
over the public request surface — `std::optional`, the request variants, a
class template — that emits no library symbol and adds no kernel behaviour,
so there is nothing for `native_c_api.h` to export. A C host composes the
same bracket from the primitives the C API already carries:
`strategy_native_submit_v1` per leg with `PF_NATIVE_OWNER_WAIT_FOR_APPLIED`
and `PF_NATIVE_GROUP_MEMBER`, which returns `PF_NATIVE_E_REJECTED` with the
same `RequestRejectReason` written out — the refusal this receipt surfaces in
C++ is already unmissable there, one call at a time.

Two more knobs cover anchored legs, both defaulting to "leave the leg as
written": `bracket.anchor_rounding` (an optional `NativeAnchorRounding`,
written onto every present leg whose anchor is `FromOwnerFill`) and
`bracket.visibility` (a `NativeArmVisibility`, written into the owner
relation the builder creates). A `strategy.exit(from_entry="e",
profit=…, loss=…)` therefore becomes tick-spelled `FromOwnerFill` legs with
`Directional` rounding and `PendingUntilArmed`, and the host's own trigger
projection, if it has one, lives in `resolve_anchored_level`.
`examples/native/native_bracket_strategy.cpp` is that shape end to end.

`tk::OrderBook<Key>` is the id bookkeeping a strategy would otherwise write
itself: `submit_or_replace(key, request)` re-prices the key's own request
while it is still working and submits a fresh one when it is gone,
`cancel(key)` ends it and forgets the key, and `handle(key)` reports the
current handle. An unknown key never reaches the host.

Those two commands answer one value each, and the book is as honest about
them as the receipt is about a leg. `submit_or_replace` can issue **two**
kernel commands for one call — a replace the kernel answers `NotWorking`,
then a fresh submit — so its optional handle names neither: a present handle
is a re-pricing in place or a brand new request born behind everything
already resting, and an empty one is a refused replacement (the previous
request untouched and still bound to the key) or a refused submit (the key
unbound), with the kernel's `RequestRejectReason` and event ordinal dropped
in both. `submit_or_replace_outcome(key, request)` is the same call, same
commands, same book state, reporting an `OrderBookOutcome`:

| `OrderBookAction` | what the book did | `handle()` | `reason()` |
| --- | --- | --- | --- |
| `Replaced` | `ReplaceStatus::Replaced`: the key's own request was amended, nothing was submitted | the successor | — |
| `ReplaceRejected` | `ReplaceStatus::ReplaceRejected`: nothing was submitted, the previous request is still working and still bound | — | its `RequestRejectReason` |
| `SubmitAccepted` | `SubmitStatus::Accepted` for a fresh submit; the key holds the new handle | the handle | — |
| `SubmitRejected` | `SubmitStatus::Rejected` for it; the key is left unbound | — | its `RequestRejectReason` |

`OrderBookOutcome::replace` and `::submit` are the kernel's own
`ReplaceResult` and `SubmitResult` verbatim and whole — status, event
ordinal, successor/handle, reason — each present exactly when that command
reached the kernel, so a replace the book abandoned as `NotWorking` before
submitting fresh is reported too, and the toolkit spells a refusal in no
vocabulary of its own. `action` is the one word it adds: the disjoint union
of the two kernel statuses restricted to the four the book stops on.
`submit_or_replace(...)` is `submit_or_replace_outcome(...).handle()`,
exactly.

`cancel_outcome(key)` does the same for the withdrawal. `cancel(key)` answers
`CancelStatus::NotWorking` for three different things — the kernel's own
verdict, a key that was never in the book, and a key whose request had
already left the working enumeration — and the last two reach no host command
at all. The outcome carries `known` (the key was bound when the call arrived)
beside `result`, the kernel's own `CancelResult`, present exactly when a
cancel reached the kernel; `cancel(key)` is `cancel_outcome(key).status()`,
exactly. `examples/native/native_bracket_strategy.cpp` reads a key through
all four actions and both cancel answers.

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
margin.initial_long   = 0.5;   // fractions, not percents; >= 0, 0 = host's
margin.initial_short  = 0.5;
margin.maintenance_long  = 0.25;   // absent = that side never liquidates
margin.maintenance_short = 0.25;
margin.sizing = NativeLiquidationSizing::RestoreMinimum;
margin.shortfall_multiple = 1.0;           // used by ShortfallMultiple
margin.liquidation_min_units = 1.0;        // optional broker minimum trade
margin.check = NativeLiquidationCheck::PathAdverseExtreme;
margin.basis = NativeMarginEquityBasis::MarkedEquity;          // policy, below
margin.level_base = NativeLiquidationLevelBase::MarkedEquity;  // policy, below
margin.liquidation_label = "";     // empty = "__kernel_liquidation__"
margin.liquidation_comment = "";   // empty = "Margin liquidation"
spec.margin = margin;
```

The kernel owns the margin **mechanism**: the level solve, the check points,
the kernel-originated request, its `Superseded` re-pricing, the receipt and
`on_native_margin_call`. It exposes the **policy** through the run spec's two
money bases and three host hooks, the same way `resolve_execution_terms`
exposes the fill price. Everything below is opt-in and every default
reproduces the pre-policy model exactly, digest included: the two bases fold
into the run-spec digest only once one of them is moved off `MarkedEquity`.

**Whether the model runs at all** is the presence of `spec.margin`. There is
no separate enable flag: a spec without a model never solves a level, never
reaches a check point and never issues a request, and a spec with one is
always live. A host that toggles its broker's margin call at runtime expresses
that as policy — `margin_check_allowed()` answering false — and not by
mutating the run spec mid-run.

**Opening admission.** With a model set, `initial_long` / `initial_short`
replace the single scalar for that run: an opening is refused with
`MatchRejectReason::InitialMargin` when
`resulting_abs_notional × initial_<side> > marked_equity − ticket`. A host
that answers `AdmitWithHostMargin` from `validate_execution_precommit` still
takes that one check over, exactly as before. The comparison's FX-bearing
terms — the notional, the marked equity and a percent-fee ticket — convert at
the rate of the gate's own point, as every check point below does: the
candidate gate's matching cursor, and for the placement gate the acceptance
point a `Sized{SizeTime::AtAcceptance}` froze its units at, never the later bar
clock an applied callback drained at its bar's calculation is presented. The
freeze takes its equity basis at that same point
(`tests/test_native_margin_fx_clock.cpp`, section 8).

**Opening admission and liquidation are two separate broker functions, and a
side may declare either without the other.** `initial_<side> == 0.0` is the
maintenance-only spelling: the kernel enforces no opening requirement on that
side and the HOST owns opening admission there, while the kernel keeps the
side's liquidation mechanism in full. This matters because the opening gate
has two sites, not one — the candidate gate, where a host opts out per
opening by answering `AdmitWithHostMargin`, and the placement gate that
freezes a `Sized{SizeTime::AtAcceptance}` quantity, where no candidate verdict
exists yet. A zero initial is the only spelling that covers both, so it is the
one a host with its own pre-trade money rule uses; the Pine adapter is exactly
that host, and declares its TradingView margin percents as maintenance only.
A side may waive either function but not both: a zero `initial_<side>` is
legal only where that side's `maintenance_<side>` is set, and a side with
neither is refused at configure with `MarginSideUndeclared`. A model whose
`initial_*` are positive is unchanged, bit for bit.

**The liquidation level.** With a maintenance fraction for the live side, the
kernel solves the one price where the marked equity meets the maintenance
requirement:

```
equity(P)   = capital + realized − open entry fees + dir × (P × Q − Σ qty×price) × pv × fx
required(P) = Q × P × pv × fx × maintenance
L           : equity(L) == required(L)
```

`fx` is the rate of the CHECK POINT being taken, not of the bar the engine
happens to be presenting: `account_currency_fx_at(cursor.point
.effective_time_ms)`, what the declared `NativeFxCurve` has in force at that
point's own instant, on the curve's own step semantics — a step is in force
from its timestamp onward, and the run's scalar `account_fx` holds before the
first one. It is the rate BOTH terms convert at, the requirement and the
marked equity it is compared with, and the rate `L` is solved at; at every
check kind (`BarOpen`, `AfterApplied`, `Calculation`, `FxRoll`), in batch and
in stream alike. A run that declares no curve has one rate at every instant,
so the rule is inert there by construction.

The presented bar clock is a LATER instant whenever the walk has moved past
the point being checked — an applied fill drained at its script bar's
calculation is presented that bar's close coordinate while its cursor still
stands on the opening print it filled on — and converting there calls a margin
the account never owed (`tests/test_native_margin_fx_clock.cpp`). The rest of
the kernel already converts at the cursor: a `Sized` request freezes its units
at the acceptance coordinate's rate, and an execution term records its
`active_fx` at its own cursor. A current execution does too: since R5 lane
B-ADAPTER, `execute_current` converts at its cursor's rate, threaded through
its terms facts, inspection, preview and settlement contexts, and it neither
writes nor restores the engine's presented clock, so its own hooks and the
host after it see the frame's clock. Its preview, `inspect_current_execution`,
still converts at the presented clock's rate, so the preview and the
execution disagree only when an FX step falls between the presented clock and
the cursor. The host queries are the exception, and they
are the rule restated: `native_liquidation_price()`, `native_open_lots(mark)`
and `native_marked_equity(mark)` have no cursor of their own, so they answer at
the rate of the instant the engine is presenting. So do the kernel's own
run-end rows (`record_open_position_report_rows`): they convert at the rate of
the run's last presented instant, while the shared producer they call,
`append_open_position_report_rows`, converts at whatever rate its caller hands
it — the Pine source layer hands it the rate of TradingView's range-end mark.

Both sides are affine in `P`, so `L` is unique unless the slopes coincide —
`maintenance == 1.0` on a LONG, where equity and requirement move together and
no price solves the breach. `NativeStrategyHost::native_liquidation_price()`
answers `L`, or `nullopt` when the run declares no model, the side has no
maintenance fraction, the book is flat, or no finite price solves it. It is
the exact level: no tick rounding, which is a source-layer spelling.

`level_base` chooses the base that solve starts from. `MarkedEquity` (the
default) is the intercept of `equity(P)` above — capital plus realized net
profit, less the open entries' commission. `RealizedOnly` drops that last
term, for a broker whose liquidation level does not answer for costs the open
position has already paid. The two agree whenever no open entry paid one.

**Arming (`PathAdverseExtreme`, the default).** At every script-bar open and
after every applied fill, the kernel measures the requirement against the most
adverse price the modeled script path still reaches after the current
waypoint — the same sizing mark a whole-bar broker check would use. If that
mark breaches, the kernel rests its own `Reduce` (or `Flatten`) with
`Stop{L}`, bound to the live book. The reduction therefore *fills at the
liquidation level*, where the account actually runs out of margin, while it is
*sized at* the adverse mark. With a declared `IntrabarPath` there is no
whole-bar waypoint model: the kernel re-evaluates at each delivered sample.

`basis` chooses the equity side of that comparison, at every check point.
`MarkedEquity` (the default) is `marked_equity(mark)` itself, which the open
entries' commission has already reduced. `MarkedEquityBeforeOpenCommission` is
the same mark-to-market equity taken before that reduction, for a broker that
does not charge a still-open entry's fee against the margin equity. It is one
term of one comparison: no cash is booked differently, and `marked_equity()`
itself does not move.

**Arming (`PathAdverseExtremeMark`).** The period-mark broker. It measures the
same breach at the same adverse mark, and rests the reduction **at that mark**
rather than at a solved level, so the fill belongs to the adverse waypoint the
breach was measured at. It never solves a level, which makes it the one mode
that still checks where no level exists — a LONG at `maintenance == 1.0`,
where `PathAdverseExtreme` rests nothing because there is no price to rest at.
The resting price is that fill's default resolved price, so a host that needs
a tick ladder or an exit-side slippage on the forced price applies it in
`resolve_execution_terms` like any other fill.

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

**The FX roll (`NativeMarginCheckKind::FxRoll`).** `required(P)` carries the
account rate, so under a declared `NativeFxCurve` an account can cross its
maintenance line while every price stands still. A step of the curve is
therefore a check point of its own: the first driver point the account
converts at a different rate than the point before it, offered immediately
before that point is matched. The walk has not moved yet — a discrete point is
measured at its own price, a segment at its origin with its destination among
the waypoints that remain — so the mark is the unchanged price and only the
rate is new. From there it is an ordinary path check: the level is re-solved
at the new rate and the reduction is rested, re-priced or withdrawn. A level
the new rate moved *inside* the segment that remains is reached by that same
segment and fills AT the level. A level already through the standing price is
treated like any slice rested mid-path: it is not owed the print its birth
segment has consumed and takes the next one. With the confirmed OHLC path, a
curve point stamped at a script bar's open activates at the PREVIOUS bar's
Close coordinate, whose effective time is that same instant — the roll the bar
open used to see one point late, after the Close segment had run under the new
rate against a level solved under the old one. A curve point that restates the
running rate is not a step; a run without a curve has no such point; and
`CalculationOnly`, which measures at its calculation alone, is not offered it
— its next calculation already converts at the new rate. The detection reads
the driver log and the immutable curve, so it adds no run state and moves no
continuation identity (`tests/test_native_margin_fx_roll.cpp`). It is stated
in the same cursor time every check is taken in — a point's rate is
`account_currency_fx_at(its own effective time)` — so "the rate the next match
will use differs from the one the last point used" and "each point converts at
its own instant" are one rule, not a special case beside one.

**The requirement hook.** At EVERY kernel check point, BEFORE the breach
test, the host is offered the two numbers the kernel is about to compare:

```cpp
struct NativeMarginRequirementView {
    NativeMarginCheckKind kind;        // BarOpen | AfterApplied | Calculation | FxRoll
    NativePhysicalPosition position;
    double mark, equity, required;     // equity on the model's basis
    native_order::MatchCursor cursor;
};
struct NativeMarginDecision { double required, equity; bool force_breach; };

virtual std::optional<NativeMarginDecision> resolve_margin_requirement(
        const NativeMarginRequirementView&) const;   // nullopt = the kernel's own
```

A returned decision replaces both numbers for that check point only, so a host
may **raise** a call the kernel would not make — a requirement kept to a
broker's own money precision, an equity its account model computes
differently — or **veto** one it would. `force_breach` proceeds past
`required > equity` even when the answered numbers do not meet it; the sizing
policy then runs as usual, and where the answered numbers leave no restore of
their own, `resolve_margin_call_units` is the whole sizing authority. Neither
answer moves the mechanism: the kernel still evaluates, schedules, places,
books and reports. Source-language money quirks — TradingView's
ten-significant-digit rounding, for one — live in this hook, never in the run
spec.

**The check gate.** Each kernel check point is offered first:

```cpp
struct NativeMarginCheckPoint {
    NativeMarginCheckKind kind;
    NativePhysicalPosition position;
    double mark;
    native_order::MatchCursor cursor;  // cursor.point.path_phase is the waypoint
    bool liquidation_resting;          // a slice is live from an earlier point
};
virtual bool margin_check_allowed(const NativeMarginCheckPoint&) const;  // true
```

Answering false suppresses the whole point: nothing is measured, the
requirement hook is not reached, and nothing is re-armed or withdrawn — the
margin state stays exactly as the last admitted point left it. A broker model
whose own schedule is not the kernel's expresses that here. `liquidation_resting`
is what lets such a model tell its two cases apart: a point that only exists to
RE-SIZE a slice already resting — because the book it was sized on has since
shrunk — from a point that would take a new one. A host that admits every point
(the default) never needs to look. Every point the
run's check mode reaches is offered, the ones where the book is flat or the
live side has no maintenance fraction included, because withdrawing a resting
liquidation is part of the check; `CalculationOnly`, which rests nothing,
offers only the points it could act on.

**Events and hooks.** A kernel-issued request carries
`RequestDefinition::origin == RequestOrigin::KernelLiquidation`; every host
request is `RequestOrigin::Host`. When it fills, a `MarginCallEvent` joins the
command history directly after its own `ExecutionAppliedEvent`, carrying the
mark, the equity on the model's basis and the requirement at that mark, the
solved liquidation price, the filled units and the signed book on either
side. The host sees
`on_native_applied` first and `on_native_margin_call` immediately after, with
the same cursor.

**Naming the ticket.** `liquidation_label` and `liquidation_comment` are the
ticket the kernel books its own liquidation under. Left empty they are the
kernel's own `"__kernel_liquidation__"` / `"Margin liquidation"`; set, they are
used verbatim, and they reach the closed row's exit id and comment. That
matters because the exit id is how a reporting layer says "this row was a
forced liquidation" — pineforge's own C ABI derives `close_cause`
`MARGIN_CALL` from exactly that id — and every broker spells its own. Each
folds into the model's digest only when set, so a model that does not name its
ticket digests as it did before they existed
(`tests/test_native_margin_kernel_r5.cpp`).

**The TradingView margin call is this model.** The Pine adapter sets `margin`
from `strategy(margin_long=, margin_short=)` — the two percents are the
maintenance fractions — with `PathAdverseExtremeMark`, the equity basis its
commission type implies, `RealizedOnly` for the reported level and
`"__margin_call__"` / `"Margin call"` for the ticket. It declares **no sizing
knob**: `resolve_margin_call_units` answers every call the kernel makes on its
behalf, so `sizing`, `shortfall_multiple` and `liquidation_min_units` would be
set only to be shadowed, and TradingView's slice — the restore lot-floored
*before* the 4×, floored again, and the one-contract whole-drop band for a
sub-lot restore — is not a generic policy the kernel could spell
(`scripts/check_adapter_spec_shadowing.py` fails the gate if a shadowed field
is ever declared again — a source guard of every `ci_verify.py` profile and of
`ci_preflight.py`, before anything is configured, as well as a CTest row —
and `tests/test_adapter_margin_relower.cpp` MG-F3 pins
the gridded case where the two orders of floor and multiple part: 4 lots, not
5). The kernel then solves,
schedules, places, re-prices, books and reports every resting liquidation; the
adapter answers `margin_check_allowed` with TradingView's scheduling — which
includes the post-exit re-size: when a priced bracket leg of the script bar
fills, the slice resting at that bar's adverse extreme was sized on the
pre-exit book, and the legacy broker cancelled and re-scheduled it there
(`margin_check_allowed` `pine_adapter.cpp:13844-13868`), so the adapter admits
the kernel's own point for that driver point while (and only while) a slice
rests —
`resolve_margin_requirement` with its ten-significant-digit money and
fee-adjusted equity, `resolve_margin_call_units` with its lot-floored 4×
restore and whole-drop band, and `resolve_execution_terms` with its
tick-quantized fill price. The presence of `margin` IS
`set_margin_call_enabled()`. What stays adapter-side is what no check point of
the kernel's can reach: the bar-open mark checkpoint and the script-close pass
(the kernel checks once per point, at the remaining path's adverse mark), the
account-currency FX rollover revaluation (the adapter refuses the kernel's
`FxRoll` point by kind: TradingView's rollover is its broker-open slice, taken
on the source's own sub-bar rate), the pre-open admission slice, and the
one-contract 1×-long money call — which is not a maintenance liquidation
at all and fires on the favorable side of the path. Measured against the
adapter as it stood before the re-lowering, on the same books, bit for bit:
`tests/test_adapter_margin_relower.cpp`.

### Risk limits

`NativeRunSpec::risk` is the generic account-risk block, and it is opt-in. A
spec that leaves it unset measures nothing, appends no event, refuses no
opening and folds nothing new into the continuation digest. The existing
per-opening caps — `max_abs_units`, `max_open_lots`,
`allowed_open_directions` — are unchanged and independent of it.

```cpp
NativeRiskLimits risk;
risk.max_drawdown = NativeLossLimit{1000.0, false};       // account currency
risk.max_intraday_loss = NativeLossLimit{2.0, true};      // 2 % of the basis
risk.max_consecutive_loss_days = 3;
risk.max_fills_per_day = 10;
risk.day_basis = NativeRiskDay::SessionDay;               // or CalendarDayInTimezone
risk.action = NativeRiskAction::BlockOpenings;            // or FlattenAndBlock
spec.risk = risk;
```

**Where it is measured.** At three points of every script bar: its open, its
own close calculation, and once after each drain of applied fills. The mark
is the marked equity at that point's own price — the bar's open, its close,
or the fill's resolved price. A breach that first exists at the close is
therefore acted on at that close: the openings of that same calculation are
already refused, not the next bar's. A breach that exists only between the
three points is seen at the next one.

| limit | breach | block lasts |
| --- | --- | --- |
| `max_drawdown` | the marked equity has fallen from its running peak by at least the limit (percent: of that peak) | to the end of the run |
| `max_intraday_loss` | the marked equity is below the day's opening equity by at least the limit (percent: of that opening equity) | to the end of that day |
| `max_consecutive_loss_days` | N consecutive days have each closed with a realized loss | to the end of the run |
| `max_fills_per_day` | N applied fills within the day | to the end of that day |

A percent limit is out of 100, and is resolved against its own basis at the
moment of the test. Limits are evaluated in the order above and one point
reports at most one breach.

**Counting fills.** `max_fills_per_day` counts every applied execution of the
day as it settles, whatever issued it, and the limit is tested at the
evaluation points like every other. The fills matched at one point therefore
all settle — reaching the limit blocks the NEXT admission, not the fill that
reached it. That is the generic rule; TradingView's intraday order cap, whose
forced close also withdraws the working orders (its cancel-pending latch),
stays an adapter quirk on top of it.

**Days.** `SessionDay` keys on the run's own session calendar
(`native_calendar::session_day_ordinal`), so an overnight session is one day;
`CalendarDayInTimezone` keys on the plain civil date of the spec's scheduling
timezone. Every point of a script bar takes the day of that bar's own open, so
a bar never straddles two risk days. A day's realized result is the change in
realized balance across it: a day that closes negative extends the
consecutive-loss streak, one that closes positive restarts it, and one that
realized nothing leaves it alone. The streak is settled when the next day
opens.

**The block.** While it lasts, every opening is refused with
`MatchRejectReason::RiskLimit`, ahead of the per-opening caps. A reduction is
never an opening, so exits, brackets and the margin model's own liquidation
still fill. With `NativeRiskAction::FlattenAndBlock` the kernel additionally
issues one `Flatten` of its own at the breach point, carrying
`RequestDefinition::origin == RequestOrigin::KernelRisk`, and executes it
there; the block is latched before that flatten is submitted, so its fill
cannot re-enter the same breach.

**The event.** Every breach appends a `NativeRiskEvent` to the command
history, which `native_events()` returns like any other command event:

```cpp
struct NativeRiskEvent {
    uint64_t ordinal;
    RiskLimitKind kind;     // MaxDrawdown | MaxIntradayLoss
                            // MaxConsecutiveLossDays | MaxFillsPerDay
    double limit;           // the threshold in the unit it was measured in
    double observed;        // the value that reached it
    std::int64_t day_ordinal;
    MatchCursor cursor;
};
```

`NativeStrategyHost::native_risk_state()` reads the ledger back at any time:
whether openings are blocked and why, the current risk day, the fills counted
in it, the consecutive-loss streak, the running peak equity and the day's
opening equity.

**TradingView's `strategy.risk.*` is not this model, and stays in the Pine
adapter.** The adapter never sets `risk`, for a structural
reason first: Pine's risk calls are per-bar script statements, so the adapter
learns a limit on the first script bar, after `configure_native` has fixed
and digested the spec — a begin-time declaration has nowhere to receive it.
Measured against the kernel on the same tapes, each rule also differs in
substance: the drawdown latch samples once per bar at the close and still
admits a reversal while latched (the kernel measures at the open, the close
and after each drain, and refuses every opening); the consecutive-loss streak
counts one losing trade per chart day and resets on any winning fill (the
kernel settles a day's net result at the next day's open); the intraday loss
is checked along the bar's path and closes at the adverse extreme, then
refuses every placement for the day and withdraws the working book (the
kernel checks at its three points, closes at that point under its own ticket
and blocks openings only); the filled-orders cap charges slots, transfers
quota, closes at the fill or the bar's better extreme and keys its day on the
chart timezone (the kernel counts settled fills, closes at the evaluation
point and keys on the spec timezone). Every difference is pinned, with the
adapter's rows harvested before the re-lowering, in
`tests/test_adapter_risk_relower.cpp`; the rulings and the corpus measurement
are in `docs/design/native-feature-parity.md` §3.6.

**Native-only by ruling.** That retention is final, and it is
not a gap: `risk` is a native-host feature whose consumers are native hosts,
in C++ and in C (`PF_NATIVE_SPEC_EXT_RISK`, `strategy_native_risk_state_v1`).
ADR-0001 ("Kernel capabilities the Pine adapter does not declare") records the
ruling and `scripts/check_native_feature_rulings.py` holds it. Two examples
exercise the block: `native_trail_risk_strategy.cpp` the fill-count cap with
`BlockOpenings`, and `native_risk_limits_strategy.cpp` the money limits with
`FlattenAndBlock` — a 1 % intraday loss limit that the kernel enforces with a
flatten of its own at the breach point (1200 observed against 1000, then 1000
against 988 of the next day's opening 98800), the re-arm at the next civil
day, and the consecutive-loss-days block that ends the run's openings.

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
   recalculation at that event's own cursor, right after its
   `on_native_applied` — and, for a kernel liquidation, after the
   `on_native_margin_call` that follows it — driven from the same drain. It is bounded by
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

The Pine adapter no longer schedules its own fill cascade: a
`calc_on_order_fills` strategy projects `calculation = BarCloseAndFills` with
`max_recalculations_per_point` set to TradingView's guard literal, and the
consumer drives every fill recalculation from its notification drain and
delivers it as `on_native_recalculate(..., OrderFill, cause)`. What remains in
`src/source` is only what TradingView's COOF adds on top of that cadence
: the language-state snapshot/restore/commit around a
recalculation, the waypoint-only refill deferral, the two-fills-at-open rule,
the first-open execution chain, and the two fills Pine refuses to recalculate
on at all — a `process_orders_on_close` fill at the already-consumed close, and
a grouped same-entry stop sibling. Those refusals compose with the kernel
cadence rather than replacing it: the recalculation is offered at the cursor
and the source layer publishes nothing. `calc_on_order_fills` is a batch-only
pairing; the Pine stream route refuses it before any spec is projected, so
`BarCloseAndFills` never reaches forward execution. The waypoint deferral is
still load-bearing: a native `BarCloseAndFills` host running the adapter's
refill rule reaches the same book with the same order ids in the same order,
but the coordinates a request born in a recalculation fills at differ — the
adapter re-presents it at the chart bar's next waypoint, the kernel at the next
discrete matching point of the delivered path.

#### The second fill simulation is gone

The repository holds **one** fill simulation: `src/native_matching.hpp` driven
by `NativeExecutionConsumer`. The TradingView exit-path resolver that used to
sit beside it — `internal::resolve_exit_path_fill`, a 930-line intrabar walk
in `src/source/pine_path_resolve.cpp` — has no declaration, no shipped body
and no test-only copy left; the re-lowerings took its last production caller
and the header that preserved its body for the trail suites was retired with
it. `src/source/pine_path_resolve.cpp` still holds
`internal::entry_stop_first_touch` (`pine_path_resolve.cpp:36`), the one
function of that file a live caller reaches.

Those rows are now a matcher-side projection. `tests/trail_exit_product_probe.hpp`
builds each resolver scenario — a position at an entry price with a carried
running extreme, one `strategy.exit` trail, one probe bar — as three script
bars through the real host: the adapter lowers the trail the way it lowers
every corpus strategy, the consumer walks the bar's synthesized path, and the
fill is read back from `NativeStrategyHost::native_events` — the applied
event's modeled price and match cursor, the terms event's price kind, the
closing request's trigger kind — beside the booked trade row. The four fields
the resolver reported are projected from that record: `at_bar_open` is the
cursor's `NativePathPhase::Open`, `path_position` is the cursor's waypoint
index and `t` in the legacy `first_touch_position` units, `is_trail` is the
kernel trigger the adapter chose (a generic `Trail`, or the one-shot `Limit`
spelling of a zero-tick offset), and a level fill is
`NativeCandidatePriceKind::TriggerLevel`. Where the two disagree on a
coordinate rather than on a price — a one-shot's crossing is the adapter's
half-tick arm threshold, half a tick before the activation, not the level —
the row carries its `expectation corrected:` note, and the three suites are
registered in `tests/twin_parity_inventory.json`'s `observableRewrites` with
their new assertion digests. Every TradingView number those suites pin is
unchanged.

P9 left one ruling open, and lane E5 settled it with TradingView's own tapes
(`tests/fixtures/offset_trail_arm`, replayed by
`tests/test_offset_trail_quantized_arm.cpp`): a trail WITH a trailing offset
arms on the tick-quantized path exactly like the one-shot. On NYSE:F 15m, six
trades whose bar's raw extreme stops inside the activation's tick cell
(9.415, 13.041, 12.641 lows; 11.899, 13.049, 13.419 highs) exit on that bar at
activation -/+ the offset; on the on-grid ETH feed a sub-tick activation is
never rounded onto the fill. The adapter therefore arms the generic `Trail`
at the half-tick threshold its one-shot leg rests at and books from a running
best that starts at the activation; the kernel still compares `arm_price`
with the raw path. The P9 row that asserted the hold in that window now
asserts the fill, with its `expectation corrected:` note.

Lane E9 closed the two findings E5 left
(`tests/fixtures/trail_activation_tick_reach`, replayed by
`tests/test_trail_activation_tick_reach.cpp`). A one-shot with a sub-tick
`trail_price` books the tick the quantized path reaches — its resting half-tick
level rounded away from the position (ETH fill 2550.85, level 2550.854,
@2550.86) — where the adapter had booked the snap toward it, which the kernel's
limit-or-better check rightly refused (`InvalidTerms`). And a placement close
inside the activation's tick cell (NYSE:F 11.295 under 11.30) has reached the
activation: 8 of 8 trades exit on the next bar, `trail_offset` 1 and 0 alike,
so P9's carried-best row (9.996 under 10.00) now asserts the fill @9.98, with
its `expectation corrected:` note. Both stay adapter policy; the kernel is
unchanged.

Lane E14 closed the residual both of them left. TradingView's running best
starts AT the activation; the kernel's started at the arm — half a tick short
of it on a crossing, at the next bar's first print when the placement close
armed it — so a print between the two fires TradingView's stop and not the
kernel's. None of the fourteen tapes fell in that window, but a shallower
next bar would. The generic answer is `Trail::best_seed` above: the leg names
the level its ride starts at, and the adapter names the activation, or the
favourable one of the activation and the placement print when that print
already armed it. The kernel's own crossing is then the price the adapter
books, which is what the three trail twin suites now assert. The sibling stop
an explicit-zero trail rests beside its `Trail` stays: a `Stop` is reached by
a touch and a zero-distance ride needs a move strictly past the best, so they
differ exactly on a print that lands ON the carried best.

Two kernel helpers went with it. `BacktestEngine::round_to_mintick_directional`
and `BacktestEngine::apply_slippage` were the pre-R5 spelling of the
directional tick snap and the slippage step; the kernel fills on
`native_matching::grid_round_directional` and `native_matching::apply_slippage`
instead (`native_execution_consumer.cpp`), so the pair had no caller in `src/`
at all and has been deleted from the public header. A native host that wants a
computed level on the ladder declares `NativeRunSpec::price_grid`; it does not
snap by hand.

## One physical book

There is one engine lot/account book. Native matching inspects settlement,
admits openings against the frozen spec, then commits once. Fees are quoted
on that execution (percent of absolute notional, cash per unit, or one cash
ticket per execution). Slippage is applied to the raw observed/modeled price
as described above. Native account rows follow lots, remaining entry costs,
and realized balance at the matching coordinate.

## Reading the run back

Every query below is a `const` member of `NativeStrategyHost` that copies
owning values out: a row already returned is never invalidated by a later
command or by the reset of the next run. Each has a C spelling unless the row
says otherwise, and the C spellings are in
`<pineforge/native_c_api.h>` (see *Driving the kernel from C*).

| query | answers | C spelling |
|---|---|---|
| `native_state()` | `NativeStateView` `native_host.hpp:282`: the lifecycle `kind`, the staged `spec`, the `phase`, the `completion`, the durable `failure`, the consumed high water and the decision floor | `strategy_native_state_v1` |
| `physical_position()` | `NativePhysicalPosition` `native_host.hpp:296`: `signed_units`, `average_price`, `lot_count` | `strategy_native_position_v1` |
| `native_open_lots(mark)` | one `NativeOpenLot` `native_host.hpp:328` per open physical lot, oldest first, marked at `mark` | `strategy_native_open_lot_count_v1` / `_get_v1` |
| `native_marked_equity(mark)` | the account's marked equity at `mark` | `strategy_native_marked_equity_v1` |
| `native_working_requests()` | one `NativeWorkingRequest` `native_host.hpp:669` per live request: its `definition`, its `remaining` and its `trigger_state` | `strategy_native_working_len_v1` / `_get_v1` |
| `trail_state(handle)` | `NativeTrailState` `native_host.hpp:657`: `activated`, `best_price`, `current_level`, `activation_ordinal`; `nullopt` when the handle is not a live trail | `strategy_native_trail_state_v1` |
| `native_events(after)` | `NativeMarketEvent` `native_host.hpp:370` rows: a `NativeEventKind` `native_host.hpp:360` (`Command`, `Driver`, `Account`) and exactly one of `command`, `driver` (`NativeDriverPoint` `market_driver.hpp:91`) or `account` (`NativeAccountObservation` `native_host.hpp:351`) — the rows the run's `event_retention` keeps (*What a run keeps of its events*) | `strategy_native_events_v1` |
| `native_event_window_start()` | the oldest ordinal a read can still return: every command event at or above it is retained; 1 while nothing was dropped | `strategy_native_event_window_v1` |
| `current_execution_point()` | `NativeCurrentPointView` `native_host.hpp:646`: the active callback's decision context, its price, the `NativeCurrentQuoteKind` `native_host.hpp:451` and the ordinal the quote came from; `nullopt` outside a decision point | `pf_native_decision_v1::price` / `::quote_kind` |
| `native_risk_state()` | `NativeRiskState` `native_host.hpp:631`: whether openings are blocked and why, the risk day, the fills counted in it, the loss-day streak, the peak equity and the day's opening equity | `strategy_native_risk_state_v1` |
| `native_liquidation_price()` | the solved level `L`, or `nullopt` | `strategy_native_liquidation_price_v1` |
| `current_partial_bar()` | the lookahead-free bar so far at this cursor | `strategy_native_partial_bar_v1` |
| `native_series_bar(i)` | the latest delivered bucket of subscription `i` | `strategy_native_series_bar_v1` |
| `native_recalculation_count()` / `native_recalculations_skipped()` | the calculations the cadence drove and the ones its per-point bound dropped | `strategy_native_recalculations_v1` |
| `native_decision_floor()` (`native_host.hpp:1320`) | the run's monotonic decision floor in epoch ms — the same value `NativeStateView::decision_floor_ms` carries, and the lower bound every request's birth is compared against | `pf_native_state_v1::decision_floor_ms` |
| `native_consumed_high_water()` (`native_host.hpp:1325`) | the highest `run_number` this host has consumed. It lives **outside** per-run reset, so the next configure on the same host needs a strictly larger number; a fresh host reads 0 | `pf_native_state_v1::consumed_high_water` |
| `native_continuation_hash()` | the consumer's continuation identity: a fold of its live state (*What the continuation and the broker-state hash fold*), the timezone folded by its content, so the same spec over the same bars and zone rules answers the same value on every host | `strategy_native_continuation_hash_v1` |
| `native_sized_units(sized, price, equity, fx)` | the kernel's own `Sized` resolution as a pure query | none — see *Previewing a basis* |
| `inspect_current_execution(cmd)` | `NativeCurrentExecutionPreview`, with a `NativeCurrentRefusal` `native_host.hpp:685` when the command cannot be consumed here | none — `strategy_native_execute_current_v1` answers the same verdicts |

**Reading a failure.** `native_state().failure` is a `NativeFailure`
(`native_host.hpp:237`): a `NativeFailureCode`, the `NativeFailureOperation`
it happened in, an optional ordinal and discriminator, and an
allocation-free `context`. The context is a tagged union chosen by
`NativeFailureContextKind` (`native_host.hpp:97`), and which of its three
in-run facts is populated is answered by three `constexpr` predicates rather
than by reading the tag yourself:
`native_failure_has_cause` (`native_host.hpp:145`),
`native_failure_has_recipient` (`native_host.hpp:149`) and
`native_failure_has_cursor` (`sha256:57ded95e69a90ce0e561eadbd0f81e8b130356dd7b480519e1904552d4647957` native_host.hpp:121) — each taking either the
kind or the whole context. `native_failure_context_in_run`
(`native_host.hpp:207`) builds one; `native_failed_run_identity` reads back
the `RunIdentity` the failed spec carried, and a foreign run is dropped
rather than relabelled. Failure copy and move do not allocate.
`last_error()` is presentation text beside it, never the authority.

**Callback contexts.** The bar callbacks receive
`NativeDecisionContext` (`market_driver.hpp:114`): the point's
`NativeCoordinate` (`market_driver.hpp:71` — ordinal, the interval stamps,
the effective time, the `NativePathPhase` and the `NativeCompletionKind`),
the decision floor, both calendar intervals, and the script bar's
session-day facts (R5 lane F5): `in_session` (`market_driver.hpp:154`),
`opens_session_day`, `closes_session_day` and
`closes_session_day_open_ended`. They read the run's own calendar and
session day — the cycle that rolls at the session's first window start,
keyed to its trading date, so an overnight session is one day across local
midnight — and the bar before or after the script bar: the one the run
holds (its batch input or stream warmup), otherwise the calendar's slot one
script width away. A run's first bar opens its session day and a batch's
final bar closes it; the open-ended close reads the calendar at that final
bar instead, for a host whose last input is still forming; a D/W/M bar holds
whole days, so all four are true. Every callback of one script bar carries
the same four, fills and fill recalculations included, and a C host reads
the first three from `pf_native_decision_v1`'s session bytes
(`native_c_api.h:1440`). The kernel resolves each session day once through
`native_calendar::session_day_at` (`native_calendar.hpp:407`), which a host
may call too. `tests/test_native_session_day_facts.cpp` replays the
TradingView session tapes through a bare host. It is a presentation snapshot
copied onto the callback stack: writing to it cannot move the floor, the
matching time or the after-calculation coordinate, and the session-day facts
fold into no digest. `on_native_input` receives
`NativeInputContext` (`native_host.hpp:754`) instead — the two intervals, the
input index and whether this input completes the script interval — and
`on_native_tick` receives `NativeTickContext` (`native_host.hpp:766`), the
decision context plus the print's sequence, where zero keeps the public
`TradeTick` sentinel meaning "the provider supplied none".

**Cohorts.** A cohort is a host-built roster of openings a later close binds
to: `cohort_open()` answers a `native_order::CohortHandle`, `cohort_add`
(`native_host.hpp:1250`) enrolls one accepted opening's handle,
`cohort_remove` (`native_host.hpp:1253`) takes it back off, and a request with
`owner = native_order::BindCohort{cohort}` closes what the roster holds at
the match. The C spellings are `strategy_native_cohort_open_v1` / `_add_v1` /
`_remove_v1`; in C the pairing is fixed — a `BindCohort` owner is reachable
only with `PF_NATIVE_INTENT_HOST_SIZED`, whose units the `on_close_units`
hook answers. `tests/test_native_adapter_lowering_l1.cpp` pins enrollment and
the cohort close from a bare host, and the cohort scenario of
`tests/test_native_c_api.c` does the same from C.

### What a run keeps of its events {#native_engine_event_retention}

`NativeRunSpec::event_retention` (`NativeEventRetention`, R5 lane V19-B)
chooses what `native_events()` can still return. It is reporting: no fill, no
decision and no value the continuation folds depends on it, except that the
spec digest folds a retention other than the default.

| retention | what the kernel keeps | memory |
|---|---|---|
| `Window` (the default) | the command journal, only until the host has read it; no driver point, no account row | O(live) |
| `Commands` | the whole command journal and every account row; no driver point | O(run) |
| `Full` | the whole command journal, every driver point and every account row — the record every run kept before the field existed | O(run) |

Under `Window` a host that polls says what it has read:
`native_acknowledge_events(through_ordinal)` records that every event through
that ordinal is consumed, and the kernel drops the acknowledged command events
at the next script-bar boundary, keeping every one above it. Until a host
acknowledges, the kernel treats it as served by its callbacks alone and closes
its window at every script-bar end, so a polling host acknowledges `0` from
`on_native_run_begin` ("nothing read yet") and then its cursor as it reads.
An acknowledgement is never lowered by a later, smaller one; one above the
event high water acknowledges the high water, never an event that has not
happened; and before a run begins, or once it has ended, it records nothing.
`Full` and `Commands` keep the whole journal whatever is acknowledged.
`native_event_window_start()` answers the oldest ordinal a read can still
return, and `native_events(after)` for an `after` below it starts there.

The kernel never drops an event its own live state still reads. A queued
applied notification (and the margin receipt it carries) pins the window at
its execution, a live deferred group-adjustment chain pins it at its head,
and the rest of what the journal used to answer is state now: a request's
replace-chain root is `RequestDefinition::root` (`native_order.hpp:736`) and
the order core's chain index, which is all a cohort command reads about a
request that is no longer working; a trail's arm ordinal
(`NativeTrailState::activation_ordinal`) is its tracking state's
(`TrailTrack` `native_order.hpp:650`); the FX-roll margin check reads the
last two driver points' instants; and a group-effect receipt whose outcome
event was dropped stands on its own record. A host that wants the whole run
after it has ended asks for `Full` (or `Commands`); the Pine adapter declares
`Window` and acknowledges its receipt cursor as it observes the journal.

A C host spells the retention in `pf_native_run_spec_ext_v1`'s fifth layout
(below). A C caller that does not send it — `strategy_configure_native_v1`,
any earlier layout, a clear bit — keeps `Full`, the record its layout was
published with.

`tests/test_native_event_retention.cpp` (kernel-only) holds the readbacks to a
`Full` record pinned before the window existed, row for row, over randomized
books; pins every acknowledgement edge above; and runs the same books with the
window closing at every driver point (a test switch), booking the trades
`Full` books. `tests/test_native_journal_window.cpp` holds the chain index to
a journal oracle and a live deferred chain to its pin.

## Reporting for native hosts

`fill_report` publishes closed trades, diagnostics, an equity curve and the
metrics derived from it. The curve is **host-owned by default**: nothing in
the kernel records a point, so a bare `NativeStrategyHost` that leaves
`report_policy` at `HostRecorded` reports `equity_curve_len == 0`, and every
metric *derived from that curve* (`pf_equity_stats_t`: its drawdown, run-up,
Sharpe/Sortino, CAGR, time in market) degenerates over the empty series. A
host that marks its own equity keeps this default and owns the whole series.

**The scalar extremes are not part of that bargain.** `max_drawdown_`,
`max_runup_` and `max_contracts_held_all_` / `_long_` / `_short_` — read back
through `max_drawdown_percent()` (`engine.hpp:1507`), `max_runup_percent()`
(`engine.hpp:972`) and `max_contracts_held_all/long/short()`
(`max_contracts_held_all` `engine.hpp:1836-1838`) — are a property of the RUN: what it drew down, what
it ran up, the most it ever held. The kernel folds them (`update_equity_extremes`, `engine.hpp:1362`) at every script
calculation under `HostRecorded` and `KernelRecorded`. Under
`KernelRecordedAtHostMarks`, it folds them at the host-mark callbacks instead, so
the host controls the cadence. A `HostRecorded` host reads them truthfully without
asking the kernel to record a curve. Before R5 lane E2 the fold was
reachable only through the two recording policies, and a defaulted host read
all five back as zero for the whole run. What stays policy-scoped is the
recording: the curve point, and with it every `pf_equity_stats_t` figure.

`NativeReportPolicy::KernelRecorded` asks the consumer to record instead. Once
per script calculation — after the callback returns, and after the
`AfterCalculation` close when that mode is on — it appends one point labelled
with the **script interval's open**, at the same instant as that
calculation's extremes fold, so the curve is identical with and without an
intrabar path and a re-walk of it reproduces the scalars above bit for bit
(`compute_equity_stats`, `engine_metrics.cpp:219`, "MUST mirror
update_equity_extremes"). The result is one point per script bar, a finite
drawdown/run-up walk, and metrics computed over a real series.

The per-bar **broker-state hash** is a row of that same report, so
`KernelRecorded` records it too. It stays behind the recording switch it
always had — `set_broker_state_hash_recording(true)`
(`engine.hpp:2126`; C: `strategy_set_broker_state_hash_recording`), off by
default, set while no run is active — because each row is a full
`broker_state_hash()` over the lots and the closed rows (since v19 a row costs
the live state, not the run's length: the closed rows enter through a running
digest). With the switch on,
one row follows each point, after the extremes that point just folded
(`record_script_report_point`, `native_execution_consumer.cpp:7631`), so

```text
broker_state_hash_len == equity_curve_len == script_bars_processed
```

in batch and across a stream's warmup and realtime legs alike. That length
identity is the only part of the array that holds across drivings.

**What a row is, and what it is for.** A row is the run's *continuation
identity* at that bar, not its trade outcome. `broker_state_hash()` is
`broker_state_hash_from_execution_hash(continuation_hash())`, so a row folds
the kernel's broker state — the lots, the realized sums, the equity extremes,
the closed rows — and, ahead of it, the state a resume would continue from.
`NativeRunPhase` (`Batch` / `Warmup` / `Realtime`, readable as
`native_state().phase`) is folded into that continuation **on purpose**: a
consumer mid-warmup and a consumer mid-realtime are not interchangeable
continuations, which is exactly what `native_continuation_hash()` is for and
why `stream_state_hash()` leads with this value. So the array is a replay
check **within one driving mode** and deliberately not across modes:

- *Same driving.* Two runs driven the same way over the same bars record the
  same rows, and the row after bar *k* equals the last row of a run driven the
  same way that ended at bar *k*. That prefix closure — in a batch, and in a
  stream at any warmup split — is what makes the array a replay check.
- *Different driving.* The same bars booking the same trades record
  **different** rows under `run()`, under `stream_begin(warmup=1)` +
  `stream_push_bar` and under `stream_begin(warmup=all)`, differing from index
  0. Two streams do not promise identical warmup rows once subscriptions are declared: each
  subscription carries its own delivery cursor and warmup boundary. Compare outcomes
  through the native twin; continuation rows remain a within-driving replay check.
- *The broker half alone is driving-mode invariant.* Factor the continuation
  out — `broker_state_hash_from_execution_hash(fixed)`, protected on
  `BacktestEngine` — and the remaining fold is identical at every bar in every
  driving. The divergence is the continuation and nothing else.

**The batch↔stream oracle is the outcome, not the hash.** What pins that a
stream books what a batch books is the outcome twin: section 8 of
`tests/test_native_margin_fx_roll.cpp` compares every offered check point,
every measured requirement, every receipt, every closed row and the book that
is left; `tests/test_streaming.cpp` compares trade count, position, equity and
each closed row's identity, prices and size. For the Pine adapter the same job
is done over the whole validation corpus by `scripts/check_corpus_parity.sh`.
Both directions of the row contract are pinned in
`tests/test_native_report_truth.cpp`.

Recording is reporting — the switch is not continuation state and no row is
ever read back, so a run records the same trades, curve, continuation and
broker state with it on or off. Under `HostRecorded` the report is the host's
and the kernel appends nothing; a C++ host that records its own report
appends its own rows.

`NativeReportPolicy::KernelRecordedAtHostMarks` records the very same series
at the points the host marks, for a host whose report cadence is not one point
per calculation. The consumer never records on its own initiative under it:
the host calls the kernel's report mark from inside its own callback, naming
the label the point carries, and the kernel performs the extremes fold and the
curve append. The Pine adapter is that host — its report has one point per
published *source* slot, which is not the same series of instants: a
`calc_on_order_fills` re-entry marks the slot it opened at the fill and the
bar's ordinary close calculation then marks nothing, and a suppressed probe
tail advances source history, and marks, without running generated code at
all. The point also has to land inside the callback, before the adapter takes
that bar's broker-state hash and before its range-end rows re-mark the curve's
last point. Under this policy the kernel owns what a report point is and the
host owns only when. The marking host also appends its own broker-state hash
row — the adapter's row has to follow a continuation snapshot only it can
name — so the kernel appends none under this policy.

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

The rows come from one generic producer inside the consumer, which appends a
mark-to-market row per open lot and answers the summed net row P&L. That
producer is policy-free: it does not choose when to mark, what to clear first,
or whether anything downstream of the rows is re-derived from them. This is
what lets a host whose report has a different shape — TradingView's range-end
report re-marks the equity curve's last point off the net row P&L, re-folds
every extreme from it and does so at three marks on the terminal bar — drive
the same producer at its own marks instead of restating the loop, while
`KernelRecorded` keeps the plain run-end behaviour described above. The two
shapes are measured against each other in
`docs/design/native-feature-parity.md` §3.7.

Both fields are opt-in and fold into the continuation hash only under
`KernelRecorded` — the one policy under which the consumer decides, on its
own, to act between two points of a run. `HostRecorded` and
`KernelRecordedAtHostMarks` fold nothing, so a spec that does not hand the
kernel its cadence keeps the continuation identity it had before these fields
existed. Recording does move the broker-state hash, because the equity
extremes it folds are durable engine state.

Per-trade reads: `closed_trade_count()` / `closed_trade(i)` return the closed
rows this run booked; `report_trade_count()` / `get_report_trade(i)` span those
rows followed by the range-end rows, in the order `fill_report` lays them out.
`closed_trade(i)` is the row itself, by reference — the object `get_trade(i)`
and `get_report_trade(i)` read at the same index and the row `fill_report`
publishes there, field for field; a range-end row counts in the report space
only. Pinned by `closed_rows_by_index` in
`tests/test_native_report_truth.cpp`.

### Why a row closed

Every closed row carries the cause its closer recorded:
`execution::CloseCause` (`execution.hpp:34`) — `Unspecified`, `Script`,
`Bracket`, `Liquidation`, `RiskLimit`, `FillCap`, `RangeEnd`. A
kernel-originated close states it through the settling fill, which is how a
bare host's own rows get theirs: the margin model's liquidation books
`Liquidation` and a `FlattenAndBlock` risk breach books `RiskLimit`, both
under the ticket the model or the run named. A host running its own forced
close states the cause on the row it produced.

`closed_trade_close_cause(i)` (`engine.hpp:1833`) is the C++ read and
`strategy_closed_trade_close_cause` (`pineforge.h:1253`) the C one, with the
same numbering: `-1` for a bad index or a NULL handle, `0` UNKNOWN, `1`
SCRIPT, `2` BRACKET, `3` MARGIN_CALL, `4` INTRADAY_LOSS_CAP, `5`
INTRADAY_FILL_CAP, `6` RANGE_END. A row closed at the end of the run
(`open_at_end`) always answers `6`, ahead of every other cause. The ticket a
row was booked under is `strategy_closed_trade_entry_id` /
`_exit_id` / `_exit_comment` (`pineforge.h:1176-1206`), which index exactly the
rows of `fill_report`'s trade array and take any handle this engine produces
— including a `pf_strategy_t` from `strategy_native_host_create_v1`, which is
how a C host reads back the ticket its own margin model declared.

### Folding host state into the broker-state hash

`broker_state_hash()` folds the kernel's own broker state: the execution
continuation, the position and its lots, the realized sums, the equity
extremes, the closed rows. A host whose next decision also depends on state
the kernel does not own — a regime, a counter, a model — folds that state in
through one generic hook, so a replay that diverges there diverges in the
hash:

```cpp
struct RegimeHost : pineforge::NativeStrategyHost {
    std::int64_t regime = 0;
    void hash_host_extension(pineforge::BrokerStateHashSink& sink) const override {
        sink.s("regime-host/v1");   // your own domain tag first
        sink.i(regime);             // then each durable value, in a fixed order
    }
    // on_native_bar(...) reads and moves `regime`
};
```

- `hash_host_extension` (`engine.hpp:405`, protected virtual on
  `BacktestEngine`) is called exactly once per hash, last, after the kernel's
  fold. What it writes is part of the scalar `broker_state_hash()`, of every
  per-bar row a `KernelRecorded` run records, and of `stream_state_hash()`.
- `BrokerStateHashSink` (`engine.hpp:350`) is a complete public type: FNV-1a
  over a canonical byte spelling — `d` (a double; `-0.0` folds as `0.0`, every
  NaN as one quiet NaN), `i`, `u`, `b`, `s` (length, then bytes), `bytes`.
- An override **replaces** the default. A host that overrides nothing folds
  the marker `"source:none"`, exactly the bytes it folded before the hook
  existed; call `BacktestEngine::hash_host_extension(sink)` first to keep the
  marker and append to it.
- The extension is input to the hash only. It moves no fill and is not part
  of `native_continuation_hash()`: two hosts that trade identically keep
  identical trades and continuations however their own folded state differs.
- `hash_source_extension` is the deprecated spelling of the same seam, from
  before a bare host could extend the fold. The generic default forwards to
  it, so a subclass still written against it compiles and hashes unchanged;
  new hosts override `hash_host_extension`. The Pine adapter folds its own
  state through the generic hook like any other host.
- A C host folds its own durable state through
  `pf_native_callbacks_v1::on_hash_extension`, the C route of
  `hash_host_extension`: it answers a 64-bit digest of that state, which the
  kernel folds after its own bytes with a domain tag, in every per-bar row, the
  final and the stream hash. Without it the C host's broker-state hash is the
  kernel's own fold. The per-bar rows need no hook and are available to a C
  host as described above.

### What the continuation and the broker-state hash fold

Both digests fold **state**, not history (the v19 value epoch, R5 lane V19-A:
`native-consumer/v9`, `pineforge-broker-state/v19`, stream fingerprint 19).

- `native_continuation_hash()` folds what a resume continues from: the three
  semantic versions; the lifecycle and its phase; the bound session, the
  decision floor and the callback context; the current execution frame and the
  queued notifications; the driver's cursors, its script and forming bars and
  its statistics; the declared series' cursors and an auxiliary feed's
  appended rows (their count and digest); the applied spec as one digest,
  taken once per spec and run generation, and the calculation-timing, margin
  and risk state a spec opts into; the live request table and the cohort
  rosters; and the order core's two counters, its last committed ordinal and
  incarnation. What the core carries forward
  beyond its live tables folds as three running digests, each record folded
  once when it commits: the group-effect receipts, the cohort receipts, and a
  compact record of every committed event — its kind and reason — which keeps
  visible a difference that leaves no other trace, such as a Cancelled
  receipt's reason (the host's own cancel against an owner's). The command
  history, the driver log and the account log are **not** folded: they are
  readbacks of how the run got there. What the journal window made state (R5
  lane V19-B) folds where it exists: the order core's chain index — each
  replace successor's root, once, at the replace — a trail's arm ordinal in
  its tracking state, and, under a staged FX curve and a margin model, the
  two driver-point instants the FX-roll check reads. The acknowledgement and
  the event high waters are readback bookkeeping and fold nowhere. So equal state answers an equal value,
  two command histories that reach one state answer one value, and a read costs
  the live state whatever the run's length. The fold takes one
  multiply-xorshift per 64-bit word; a string folds as its length, then its
  bytes eight at a time. `native_run_spec_digest(spec)` keeps its byte-wise
  FNV-1a and its values.
- `broker_state_hash()` folds that continuation, the position and its lots,
  the realized sums and the equity extremes as before, and the closed rows as
  their count and a running digest of each row's entry and exit time, entry
  and exit price, quantity and P&L, folded once per row. A row is **final**
  once the applied notification of the execution that booked it has
  returned; the host may amend it inside that notification, and a read before
  then folds the row without freezing it. A host that changes a final row —
  the Pine adapter reorders a bar's trailing same-bar bracket exits after
  their notifications — names the first row it changed through
  `NativeStrategyHost::native_closed_rows_amended(first_row)` before the next
  read, and the rows fold again from there. Rows removed from the end need no
  name: the kernel only appends, so a read over fewer rows, or a booking below
  the final mark, forgets them. Debug builds re-fold every digested row at each
  run's end and abort on a final row changed without a name. A C host never
  writes a closed row and has nothing to name.
- `stream_state_hash()` leads with fingerprint version 19 and the
  broker-state hash above.
- The continuation a run's final `broker_state_hash()` folds is latched at
  the last script point and taken at once (R5 lane PERF-P1 had latched a view
  folded on first read; with no history in the fold there is nothing a view
  saves).

The witness is `tests/test_native_state_continuation.cpp` (kernel-only):
every folded member moves the value and restoring it restores the value; two
histories, one state, one value; the Cancelled reason still differs; per-bar
recorded broker hashes cost linear time; and the closed-row finality rule.

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
hourly.gaps = false;                 // barmerge.gaps_off (the default)
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

**A subscription is a series instance, not a period.** Several may declare the
same `tf`; each gets its own evaluator and bucket state, and each is
identified by its own index — in `NativeTimeframeBarContext::subscription` and
in `native_series_bar(index)`. Two `"60"` series over a 15-minute input
deliver the same buckets twice, once per index, in declaration order. The one
thing same-period instances cannot each own is a different
`authoritative_bars` feed: the feed store is keyed by the period's duration,
so declaring two conflicting feeds for one period is refused
(`DuplicateSubscriptionTimeframe`), while declaring the same bars twice — or
leaving one, or both, empty — is accepted.

**Validation (at `configure_native`).** Each `tf` must parse and must pair
with `input_tf` exactly as `script_tf` does. Named refusals:

| `NativeRunSpecError` | Cause |
| --- | --- |
| `InvalidSubscriptionTimeframe` | unparseable literal, or a pairing `script_tf` would not accept |
| `SubscriptionFinerThanInput` | strictly finer than `input_tf` — a lower-timeframe array is a different contract and is not promoted |
| `DuplicateSubscriptionTimeframe` | two series of the same period declaring **different** `authoritative_bars` (the feed store is keyed by duration, and every monthly literal is one period, so they cannot each own one) |
| `UnorderedSubscriptionBars` | `authoritative_bars` not strictly increasing in time |
| `SubscriptionWithoutTimeframe` | declared together with `timeframe_undetected` |
| `SubscriptionWithoutAuxiliaryFeed` | a series whose `source` is `AuxiliaryFeed` in a spec that declares no feed |
| `SubscriptionFinerThanAuxiliaryFeed` | a series built from the feed and strictly finer than it |
| `UnknownSeriesSource` | a `source` outside its enumeration |

**Delivery.** A bucket is delivered on an accepted input bar, before that
input is aggregated, matched or calculated — so `on_native_input` precedes it
and the input's `on_native_bar` follows it.

- `lookahead = false` (Pine's `barmerge.lookahead_off`): the bucket is
  delivered on the input bar that completes it, never earlier — its **last**
  contributing input bar when that bar closes it (`Confirmed`, a bucket the
  session close or a break in the session clips short of its nominal end
  included: the calendar knows that bar is its window's last), or, for a
  bucket only a later input reveals as complete (a hole over its last slot),
  the next period's **first** input bar (`LazyComplete`), which contributes
  nothing to it.
- `lookahead = true` (`barmerge.lookahead_on`): the completed bucket's final
  OHLCV is delivered at its **first** contributing input bar, and
  `native_series_bar` answers with it from then on.
- `gaps = false` (Pine's `barmerge.gaps_off`, the default): a delivered bucket
  stands until the next delivery replaces it.
- `gaps = true` (`barmerge.gaps_on`): the series is **cleared** on every
  accepted input bar it delivers no bucket on — `native_series_bar` answers
  `nullopt` there, the empty that stands for `na` — and carries the bucket
  only on the bars it publishes on. Which buckets complete, when they are
  delivered and what they contain are unchanged; `on_native_timeframe_bar` is
  still called exactly once per delivered bucket and never for a cleared bar.
  The push side of a cleared bar is `clear_security(sec_id)`, the counterpart
  of the `evaluate_security` the step dispatches; a bare host leaves both at
  their no-op defaults, a source host's reaches its generated
  `clear_security()`.

`NativeTimeframeBarContext::completion` is `Confirmed` when the bucket closed
on its own last contributing bar and `LazyComplete` when the next period's
first input closed it. `interval` is the bucket's own calendar interval — its
period at the subscription's timeframe, located by its first contributing bar;
`delivered_at_ms` is the input bar the delivery rides on. A bucket still open at the end of the input is never delivered.
A C host, whose `on_timeframe_bar` signature is frozen, reads that interval
with `strategy_native_timeframe_bar_interval_v1` from inside the callback:
`pf_native_timeframe_interval_v1` (`open_ms`, `eligible_open_ms`,
`last_traded_close_ms`, `next_period_open_ms`, `next_input_open_ms`) is a copy
of the kernel's interval, never inferred from the delivered bar or its
delivery time. Anywhere else the call answers `PF_NATIVE_E_STATE`.

**Chronology against the script interval.** The pump is ordered against the
*script* interval, not the raw input. With `script_tf` coarser than
`input_tf`, a script bar can still be open when the first input of a later
interval arrives: a session close clipped it short of its nominal end — the
last `"60"` bar of a 09:30–16:00 session, every `"D"` bar over an intraday
input — or the feed has a hole over its last slot. That input seals it
**lazily** (its calculation's coordinate says `LazyComplete`), and the kernel
runs, in this order and no other:

1. `on_native_input` for input *i+1*;
2. the lazily sealed calculation of script bar *k*, reading every series
   exactly as bar *k*'s own contributing inputs left it;
3. the deliveries riding on input *i+1*: a bucket it completes, a boundary it
   closes, the clear of a `gaps` series it delivers nothing on, a `lookahead`
   bucket it opens;
4. input *i+1*'s own aggregation, matching and calculation point.

`seal(k) → deliver(i+1) → calc(i+1)`: a host never calculates a bar against a
bucket that already holds a later input. One consequence: a bucket the kernel
only learns is complete from input *i+1* (`LazyComplete`) reaches the host
after bar *k*'s calculation even when every one of its inputs lies inside
bar *k* — the same input that closes the bucket is the one that seals the bar.

That ordering matters only for a bare host that combines `subscriptions` with
a lazily sealed script interval. A run without subscriptions is untouched, and
so is every run whose `input_tf` equals its `script_tf`: each input seals its
own interval there, no script bar is ever sealed lazily, and the order is the
one under **Delivery** above. The Pine adapter is untouched as well: a
`request.security` site reaches the kernel's pump only at `input_tf ==
script_tf`, and on an aggregated chart the adapter keeps its own drive, which
already defers the next input past the lazily sealed calculation — this same
ordering. Pinned by the lazy-seal section of
`tests/test_native_htf_subscriptions.cpp`.

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
run's own input, with no calibration to inherit. That is the whole policy
knob, and it is a ruling of record (ADR-0001, "What the kernel-only archive
still names"): the three rules above follow from "the supplied
bars are the venue's own bars of that timeframe", so the kernel gates them on
the feed's presence rather than on a separate partition field.

One more rule holds with or without authoritative bars and is keyed by the
instrument class in `spec.type`: an early close (a session-day that ends
before its template's end) completes a calendar period for the exchange
classes (`"stock"`, `"futures"`, `"index"`, `"fund"`), and does **not** for
the continuous-session classes `"forex"`, `"cfd"` and `"crypto"`, whose period
completes on the next session's first bar instead. The vocabulary is the one
the C ABI's `strategy_set_syminfo_type` fixes.

**Declaring at begin.** A host whose series are known only to its own
begin-time registration calls
`declare_timeframe_subscriptions(std::vector<NativeTimeframeSubscription>)`
from inside `on_native_run_begin`. The list replaces the staged spec's
`subscriptions`, so the staged spec keeps naming exactly what ran and the run's
continuation identity folds the declared series. The call is legal only there:
anywhere else, and for a list this run's `input_tf` would refuse (the same
validation `configure_native` applies), it stages nothing, changes nothing and
answers `false`. It is not virtual — the host calls the kernel here.

Those are two refusals and one bit, so the call has a second spelling that
names them: `declare_timeframe_subscriptions_result(...)` answers a
`NativeSetupResult`, exactly as `configure_native` answers the identical
validation. A judged list carries that validation's own first error and field
(`SubscriptionFinerThanInput` at `SubscriptionTimeframe`, and the rest of the
table under *A series finer than the input*); a call made anywhere but inside
`on_native_run_begin` — or on a host that has already failed — judged no list
at all and is `NativeRunSpecError::WrongPhase` at `NativeRunSpecField::None`.
The bool spelling is `...status == NativeSetupStatus::Applied` and nothing
else: same staging, same commands, same answer.

The kernel registers the declared series **after** `on_native_run_begin`
returns, which is what makes that hook usable: a host that registers its own
`request.security` evaluators there (clearing
`BacktestEngine::security_eval_states_` first, as generated code does) can no
longer erase the kernel's registration, and the kernel appends its own states
after the host's rather than in place of them. The previous run's declared
series are torn down **before** that callback, so an evaluator state the
callback registers itself — even one identical to what the kernel registered
last run, as a host moving a series from the kernel's drive to its own does —
is the host's and stays. Two consequences worth stating: `native_series_bar`
answers `nullopt` for every index *inside* `on_native_run_begin` (nothing is
registered yet), and the kernel feeds only the evaluator states it registered
itself.

**Feeds the host installed itself survive.** `set_native_security_feed` — the
store the C ABI's `strategy_set_native_security_feed` writes — is host ingress
installed before a run. A later begin re-registers the kernel's own series and
removes only the feeds that registration installed; a feed the host put there
stays, and is simply replaced if a later begin declares `authoritative_bars`
of its own for the same period.

**The Pine adapter on subscriptions.** The source host runs its own
plain `request.security` sites through exactly this path. After generated
`configure_security_evaluators()` has registered them inside
`on_native_run_begin`, the adapter declares one series instance per site —
`tf` from the site, `gaps` from `barmerge.gaps_on`, `lookahead` off, no
`authoritative_bars` (a feed installed through `set_native_security_feed`
stays the store's) — through `declare_timeframe_subscriptions`, drops its own
registration, and the kernel registers the same sites `sec_id` by index and
steps them with its pump; the adapter's own pump feeds nothing for that run
(`PineStrategyHost::declare_security_sites_to_kernel`). The route is taken only
where the kernel step *is* the Pine step: a batch run with `input_tf ==
script_tf`, no auxiliary feed, no range-start warmup flag, no historical
lookahead projection, no forex/cfd intraday daily request (the OTC pins), and
every site `lookahead_off`, not Heikin-Ashi and not lower-timeframe. Every
other shape — streams (the kernel takes confirmed bars only), the bar
magnifier and aggregated charts (the calling-bar deferrals), `lookahead_on`
(per-input peeks and the merge latch), `ticker.heikinashi`, both
`request.security_lower_tf` paths, the auxiliary slice — keeps the adapter's
own drive, unchanged. The corpus' 23 `request.security` probes are all of the
first kind but the two lower-timeframe ones; their trades are byte-identical
on either drive, which is the parity evidence for the kernel pump.

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
  stream never has it for the bar it has just received. A period whose input simply stops early therefore waits for the next period's first
  pushed bar and arrives as `LazyComplete`. A session-clipped bucket is different: the
  calendar closes it on its own last contributing bar and delivers it `Confirmed`.
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
emulated, unless it is built from an auxiliary finer feed the host declares
("The auxiliary finer feed" below). Only the run's own symbol is addressable;
there is no auxiliary-symbol feed and no chart-slice mapping.

### The auxiliary finer feed

A host whose input is 15-minute bars may also hold the 1-minute bars of the
same symbol. It hands them to the kernel as the run's **auxiliary feed** and
names, per series, which bars the series is built from:

```cpp
NativeAuxiliaryFeed feed;
feed.tf = "1";                       // strictly finer than input_tf
feed.bars = one_minute_bars;         // strictly increasing; may be empty
spec.auxiliary_feed = feed;

NativeTimeframeSubscription five;
five.tf = "5";                       // finer than the 15-minute input
five.source = NativeSeriesSource::AuxiliaryFeed;
spec.subscriptions.push_back(five);
```

`auxiliary_feed` is absent by default and `source` defaults to
`NativeSeriesSource::Input`, so every part of this section is inert for a spec
that uses neither: nothing is stored, nothing is routed, and both the series
digest and the run-spec digest are the ones they were before the fields
existed (each folds only where a host opted in). The feed drives nothing by
itself — no matching point, no calculation, no script bar; it is read only by
the series that name it.

A series built from the feed pairs with the **feed's** timeframe exactly as
`script_tf` pairs with `input_tf`, so it may be finer than the input (`"5"`
over a `"15"` input and a `"1"` feed), equal to it, or coarser (`"60"`, `"D"`)
— and a series of the feed's own timeframe passes every feed bar through.
`lookahead`, `gaps`, `authoritative_bars`, the series-instance rule, the
callback, the pull accessor and the chronology against the script interval
are all as above; a series still built from the input is unchanged by the
feed's presence, `SubscriptionFinerThanInput` included.

**Routing is by time, and by nothing else.** When an input bar is accepted,
every feed bar not yet consumed that opened **before that input's period
ended** (`NativeInterval::next_period_open_ms` of the input's own interval) is
folded, in feed order, into each `AuxiliaryFeed` series, at the delivery point
every series has: after `on_native_input`, before that input is aggregated,
matched or calculated. So:

- bars inside an input ride on it, and a series finer than the input delivers
  several buckets on one input, oldest first (`NativeTimeframeBarContext::
  interval` is then the bucket's own span, read over the feed's timeframe,
  and `delivered_at_ms` stays the input bar);
- bars in a hole of the input ride on the next accepted input;
- bars **earlier than the first input** are folded on that first input — this
  is how a host supplies history the input does not reach, and a bucket that
  history completes is delivered there, ahead of the input's own;
- bars later than the last input's period are never folded;
- a bucket the *feed* leaves short is learned complete only from the next feed
  bar (`LazyComplete`), which rides on a later input.

Under `lookahead = true` a bucket's final values are delivered on the input
whose slice held its **first** feed bar. Deliveries riding on one input arrive
series by series in declaration order, each series' buckets oldest first.

**Validation.** At `configure_native`, and by the same functions at begin
(`validate_native_auxiliary_feed`, and
`validate_native_timeframe_subscriptions` with the feed as its fourth
argument):

| `NativeRunSpecError` | Cause |
| --- | --- |
| `InvalidAuxiliaryFeedTimeframe` | unparseable literal, or a pairing under `input_tf` an input would not accept under a `script_tf` (`"10"` under `"15"`) |
| `AuxiliaryFeedNotFinerThanInput` | the input's own period, or coarser |
| `UnorderedAuxiliaryFeedBars` | bars not strictly increasing in time |
| `InvalidAuxiliaryFeedBar` | a bar `native_bar_structurally_valid` refuses (non-finite or non-positive price, `low`/`high` not bracketing `open`/`close`, negative volume) |
| `AuxiliaryFeedWithoutTimeframe` | declared together with `timeframe_undetected` |

**Declaring at begin.** `declare_auxiliary_feed(std::optional<
NativeAuxiliaryFeed>)` is the feed's counterpart of
`declare_timeframe_subscriptions`: legal only inside `on_native_run_begin`,
replacing the staged spec's feed (`nullopt` withdraws it) before the kernel
registers, so the staged spec names what ran and the continuation identity
folds it. The feed is judged together with the series staged at that moment —
a host that names both at begin declares the **feed first and its series
second** — and a declaration that would leave a staged `AuxiliaryFeed` series
without its bars, or finer than them, changes nothing and answers `false`.
`declare_auxiliary_feed_result(...)` is the same call answering a
`NativeSetupResult`: `AuxiliaryFeedNotFinerThanInput` at
`AuxiliaryFeedTimeframe` for a feed the input refuses,
`SubscriptionWithoutAuxiliaryFeed` at `SubscriptionSource` for a withdrawal
that would strand a staged series, and `WrongPhase` at `None` for a call made
outside `on_native_run_begin`. The bool is that result's `status`.

**Streams.** The begin-time feed covers what the host knows then; the warmup
resolves its series exactly as a `run()` over the same inputs and the same
feed does. Live, the host appends each input's finer bars and then pushes the
input:

```cpp
host.append_auxiliary_bars(finer.data(), finer.size());  // the 15 one-minute bars
host.stream_push_bar(quarter_hour_bar);                   // the input they belong to
```

Appended bars join the feed behind every bar it holds and are routed by the
very rule above, so a stream fed this way reads the batch's series, bucket for
bucket and delivery point for delivery point
(`tests/test_native_auxiliary_feed_stream.cpp`). Bars the begin-time feed
already holds beyond the warmup are consumed by the live inputs the same way.
`append_auxiliary_bars` refuses **by name, changing nothing and without
failing the host**: a run that is not realtime, a run that declared no feed,
bars out of order or not after the feed's last bar, a bar with invalid OHLCV,
and a bar that opened inside an input period already accepted — its slice is
closed, and folding it late would build a series no batch of the same bars
could. Called from inside a callback it is the contract failure every
reentrant stream input is.

Those names are values, not just text: `append_auxiliary_bars_result(...)`
answers a `NativeAuxiliaryAppendResult` — a `NativeSetupStatus`, a
`NativeAuxiliaryAppendError` (`NotRealtime`, `NoAuxiliaryFeed`,
`InvalidBarArray`, `InvalidBar`, `UnorderedBars`,
`InputPeriodAlreadyAccepted`, plus `HostFailed` and `Reentrant` for the two
legality refusals every public stream input shares, and `AllocationFailure`)
and the `index` of the bar **of that call** it stopped on. `last_error()` is
the presentation text beside it, unchanged, and the bool spelling is that
result's `status`. Appended bars are durable input and are folded
into the continuation hash (count and running content digest), as is each
feed-built series' cursor; the one completion rule a stream cannot use —
closing a period on its last bar because the *next* bar's stamp is already
known — applies to the feed's own successor exactly as it does to the input's.

**What this is not.** It is not the Pine source host's split-feed path.
`BacktestEngine::set_aux_security_feed` — the C ABI's
`strategy_set_aux_security_feed` — stays that host's door: on a bare host it
answers `false`, exactly as before, and the generic door is
`NativeRunSpec::auxiliary_feed`. TradingView's
chart-slice mapping (chart bars re-keyed by the session they cover, calendar
charts routed by their actual stamps, pre-range coverage left inert, the
range-start cut), the calling-bar completions and the deferred first-bucket
publication are state and code of `source::PineStrategyHost`
(`src/source/pine_aux_security.cpp`) and are not reachable from a native host.

**The Pine adapter keeps its own auxiliary drive — retained, measured.** The
adapter's plain sites already run on kernel subscriptions
where the kernel step *is* the Pine step ("The Pine adapter on subscriptions"
above), and an auxiliary feed is one of the shapes that route excludes. It
stays excluded, on three measurements:

1. **No population can witness the move.** 0 of the corpus' 312 probes install
   an auxiliary feed (`git -C corpus grep -l aux_security` is empty at both
   the recorded and the regenerated pin), so whole-corpus byte-identity says
   nothing about a re-lowered auxiliary path either way.
2. **The slice is not the generic one.** TradingView leaves feed coverage
   before the first chart bar inert and cuts the bucket in progress at the
   range start; the kernel routes by time and folds that history on the first
   input. Over one feed with an hour of history the adapter publishes two
   hourly buckets and a bare host three
   (`tests/test_native_auxiliary_feed_twin.cpp`, row B) — and every
   split feed the adapter serves carries such history. Under an auxiliary
   feed every site finer than the chart additionally carries a Pine-only
   completion rule (`calling_close_completes_partial` /
   `calling_open_latches_first`), set unconditionally at validation.
3. **The evaluation point differs on every shape**, the congruent one
   included. The adapter publishes a chart bar's slice at that bar's
   *calculation*, after the bar's own matching pass; the kernel delivers a
   series on the accepted input, *before* it is matched. With a position
   opened at bar 1's open, the adapter evaluates bar 1's bucket against a
   position of 1 and the kernel delivers it against 0 (same twin, row C). A
   generated `evaluate_security` body is opaque to any routing predicate, so
   no predicate can prove that move neutral.

On the congruent shape — a feed that begins at the first chart bar, a plain
`lookahead_off` site no finer than the chart — the two series *are* the same
buckets read at the same calculations (row A): the generic feed can express
it. Re-lowering it waits on a population that exercises it.

**What the kernel keeps, and what it does not.** The machinery a subscription
runs on is the kernel's and is generic: the evaluator registry
(`BacktestEngine::SecurityEvalState` — series id, timeframe, its
`TimeframeAggregator`, the current bucket, the feed / evaluation counters and
the native-feed routing), `register_security_eval(sec_id, tf, input_tf)`, the
feed store (`set_native_security_feed`, `prepare_native_security_feeds`) and
one evaluator step, `feed_security_eval_state(state, bar)`: aggregate the
input, give a completed bucket the authoritative bar keyed to its period, and
publish it. That step has no publication modes — `lookahead` and `gaps` above
are the consumer's delivery rules, applied around it.

TradingView's `request.security` semantics are **not** in the kernel. The
`barmerge.lookahead_on` merge latch, `barmerge.gaps_on` and the
calling-bar publication gates, `ticker.heikinashi` substitution, the KI-55
range-start cut with its OTC-daily pins, the historical lookahead projection,
`request.security_lower_tf` emulation, the deferred auxiliary chart slice and
the `validate_security_timeframes` diagnostics are state and code of the Pine
source host (`source::PineStrategyHost`, `src/source/pine_security_eval.cpp`):
a per-`sec_id` table, `source::PineSecurityEvalState`, kept beside the generic
state of the same id and folded into the source hash extension under its own
domain (`pineforge-source-security/vN`, only when a site is registered). For
the sites that need those rules the source host composes its own evaluator
step from the same kernel primitives and the kernel never calls into it; its
plain sites it declares as subscriptions ("The Pine adapter on subscriptions"
above), and the kernel's step is then the only step. A native host therefore
cannot reach any of those rules, and none of them can change a subscription's
buckets.

### Aggregating a coarser bar without declaring a series

A host that wants only the running aggregate — no series instance, no
`gaps`/`lookahead` rule, nothing folded into the continuation identity —
aggregates the input itself. `TimeframeAggregator` (`timeframe.hpp:288`) is
public and engine-free; feed it from `on_native_input`, which runs once per
accepted confirmed input bar before that bar is aggregated or matched
(`on_native_input` `native_host.hpp:856`). Include
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
construction. It is the same class the kernel's own subscription evaluator
aggregates with, and the one the kernel's `script_bucket_completions` query
feeds when the Pine scheduler asks how its input span buckets
(`TimeframeAggregator` `native_execution_consumer.cpp:7711`). What it does
**not** give you is what a
declared subscription does: an `authoritative_bars` feed, the `gaps` and
`lookahead` delivery rules, the lazy-seal chronology, a C spelling, and the
series' place in the run's continuous identity. Prefer `subscriptions` unless
you want none of those.

`set_native_security_feed` (`engine.hpp:1730`) is the host ingress for
`authoritative_bars` installed before a run — see *Authoritative bars* above —
and not a way to register a series: registration is
`NativeRunSpec::subscriptions` or `declare_timeframe_subscriptions`. In-run the
setter is a source mutation and **throws**, latching `Failed`
(`UnsupportedSource`) through `guard_native_mutation`
(`engine_aux_security.cpp:78`, `guard_native_mutation` `engine_consumer.cpp:42`).

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
  with `calc_on_order_fills`, `pine_strategy_host.cpp:283-286`). This is a
  **source-route** refusal, not a limit on the native hooks: `on_native_tick`
  and `on_native_applied` are delivered on a stream, and a native host's own
  `NativeRunSpec::calculation` is accepted there, where `EveryModeledPoint`
  recalculates per observed print
- Tick-driven input (`stream_push_tick` / `stream_push_ticks` /
  `stream_advance_time`) on a stream that declares a `NativeFxCurve`; confirmed
  bars run under one (see @ref native_engine_stream_fx)
- A timestamped FX series staged through the mutable setter ingress
  (`set_account_currency_fx_series`) on `stream_begin`; its owner revalues on a
  broker clock of its own, which has no realtime route
- Auxiliary/native security feeds, source magnifier/tail/probe/hash/trace
  setters, `set_input`, and Pine
  entry/exit/cancel commands — native hosts latch `Failed`
  (`UnsupportedSource`) before mutation
- Tick input (`stream_push_tick` / `stream_push_ticks` /
  `stream_advance_time`) on a stream whose spec declares `subscriptions`;
  confirmed bars carry those series

A C host has the same stream and the same commands. Streaming needs no new
symbol — `strategy_stream_begin` and its family (`sha256:2e963d6ab1630db1535bd944dc7406ba649e9d14a589065db25347569fbad150` native_c_api.h:37-39) take
a `pf_strategy_t` from `strategy_native_host_create_v1` unchanged — and
`strategy_native_submit_v1` (`native_c_api.h:2639`) obeys the one legality
rule its C++ spelling does.

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
| Cooperative abort | `Failed` (`Aborted`) | reuse with `configure_native` using the same session key and a higher run number |

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

## Terms, reversal, precommit, FX curve

The two const host hooks are present at the current host epoch,
`engine_script_run_v19` (`native_host.hpp:20`).
`resolve_execution_terms` sees read-only
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

A `Transact` that crosses the book (it closes the opposite position and opens
the rest on its own side) is charged exactly its units. The settlement
computes the opening in binary64 as the units minus the closed part, and
closed + |opened| can round one ulp either side of the units; the fill's
`filled_working` is the request's units, so the one fill finishes it (R5 lane
B-ENGINE). Before, a sum one ulp above stopped the run, and one ulp below left
a 2^-49-unit remainder that a second fill opened as a dust lot.

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
`units = cash / (price * point_value * fx)`, where `price` is the sizing-point
price. `SizeTime::AtMatch` (the default) resolves at the matching candidate;
`SizeTime::AtAcceptance` freezes the units against the command point when the
request is accepted. `reserve_percent_fee` divides the
sizing cash by `1 + fee_value / 100` when the run's fee kind is
`NativeFeeKind::Percent`, and is an exact no-op for every other fee kind.
`NativeRunSpec::fee_value` is a percent, exactly as the charge reads it
(`calc_commission` bills `fee_value / 100` of the account notional), so a
0.1 % fee reserves 0.1 % of the sizing cash. `grid_policy` reuses
`ExecutionGridPolicy`: `SnapToGrid` floors the units onto the run's
`quantity_grid`; `ExplicitUnits` keeps the literal quotient, which the ordinary
on-grid terms gate then refuses as `InvalidTerms` when a grid is configured and
the quotient is off it. The two are identical on an ungridded run.

The grid floor is the largest grid multiple at or below the quotient: it never
lands above it, and a quotient of at least one whole step is never refused for
being off the grid. Its only tolerance is the engine's own on-grid predicate
(`quantity_on_grid`, about four ulp and strictly inside half a step), which
keeps an already-on-grid quotient in its own binary64 representation instead of
rebuilding it one ulp away. No proportional epsilon is applied: a floor that
rounded 2.9999995 up to a full three units on a one-unit grid would book a
quantity the basis cannot fund.

The kernel resolves the basis *before* the host hook and publishes the result
as the facts' `RemainingUnits`, so a `resolve_execution_terms` override still
has the last word: return your own units to replace the kernel's, or the
default identity terms to accept them. That holds for both sizing points:
an `AtAcceptance` request reaches the candidate with a deferred quantity
carrying its frozen number, so there is exactly one terms pass in which a host
quirk — a money band, an affordability rule, a floor of its own — sees the
kernel's quantity and may answer with a different one. A basis that is not
representable — not finite, not positive, or below one grid step after
snapping — is `MatchRejectReason::TermsUnresolved` at the candidate. An invalid
basis (`fraction` outside `(0, 1]`, `cash <= 0`) is
`RequestRejectReason::InvalidQuantityBasis` at submit. `Sized` requires the
`Independent` owner.

A kernel-sized opening settles as a signed book transaction on its declared
side, or through the `ReverseTo` and `CloseOpposite` shapes when the host's
terms name one — the same shapes a `HostSized{Open}` may name. The units are
the sized opening on the declared side: a `ReverseTo` closes the opposite
position and opens them, a `CloseOpposite` closes that many units of the
opposite book (the whole-book `Flatten` path when it consumes all of it) and
opens nothing. Both still require an opposite book
(`MatchRejectReason::NoOppositeExposure`), and a `CloseOpposite` claim larger
than that book is `InvalidTerms`.

#### The sizing price

`SizePrice` chooses *which* price the basis converts at. `Resolved` (the
default) keeps today's price — the candidate's default resolved price for
`AtMatch`, the raw acceptance-point price for `AtAcceptance`. `Signal` sizes
against the expected market fill instead: the decision-point price at
placement (the current bar's close, or the last print, at submit), moved by
the run's slippage on the request's own side
(`price ± slippage_ticks * price_tick`) and then rounded onto the run's price
grid when `NativeRunSpec::price_grid` is set (`HalfUp` is the nearest tick;
`NativePriceGrid::None` leaves it unrounded).

The signal price is frozen when the request is accepted. It therefore composes
with `SizeTime`: with `AtAcceptance` both the price and the units are decided
at placement, and with `AtMatch` the basis still converts at that frozen signal
price at every later candidate, however far the market has moved. A `Signal`
request accepted with no decision point has no price to freeze and stays
`TermsUnresolved`.

`SignalOnTick` is the same rule measured on the instrument's own tick ladder
(`NativeRunSpec::price_tick`) instead of the run's fill grid, and quantized
*before* the slippage as well as after: `round_tick(round_tick(price) ± slippage_ticks · price_tick)`.
Two things make it distinct from `Signal`, and both are generic. First,
`price_tick` is the instrument's resolution and the run already declares it as
the slippage multiplier, so a price the market *prints* lives on that ladder
whether or not the run also quantizes the prices it *books* — `price_grid`
governs fills, not what a signal was worth, and a run that leaves it at `None`
can still size against the printed price. Second, slippage is a whole number of
ticks, so it carries a ladder price to another ladder price: pre-rounding makes
`round(p) ± n·tick` exact and leaves the second rounding a binary64
re-normalization, where rounding only afterwards composes two different
quantizations of the same print.

The rule is generic — "size against the price you expect to pay" — and names no
source language. A source layer that divides by
`nearest_tick(nearest_tick(signal_close) ± slippage · mintick)` while booking
its own fill prices spells that as `{SizePrice::SignalOnTick,
price_tick = its mintick, slippage_ticks = its slippage}`; one that has adopted
the kernel's fill grid spells the single-rounding form as
`{SizePrice::Signal, price_grid = QuantizeFills, grid_rounding = HalfUp}`.

#### What a source adapter keeps

The Pine adapter lowers its declaration-level default quantity
(`default_qty_type` cash and percent-of-equity, on a market entry whose
quantity is frozen at the command) onto `Sized{CashValue, AtAcceptance,
SignalOnTick, ExplicitUnits}` and keeps exactly three things of its own, all of
them rounding or admission quirks the kernel deliberately does not model:
*which* equity a percentage is taken of and its ten-significant-digit money
rounding, which it hands over as the `CashValue` basis; its two lot floors,
which is why it asks for `ExplicitUnits` and floors the kernel's quotient in
its `resolve_execution_terms` override; and its own placement-time money-band
and affordability gates, which consume a quantity before any request exists and
so cannot be a kernel decision. The percentage fee reserve is the kernel's
(`reserve_percent_fee`). The generic conversion is available through
`native_sized_units()`, while source paths that keep `HostSized{Open}` restate
their source-specific quotient and floor in `resolve_execution_terms`; this
paragraph does not claim that every source conversion is implemented at one
call site. The two
lot floors stay the adapter's because neither is the kernel's `SnapToGrid` on
every input — the cash floor keeps a quotient a millionth of a lot under a
boundary raw, and the percent floor has no on-grid tolerance, so an exact lot
multiple such as `0.0392` on a `0.0001` grid comes out `0.0391` — both measured
in the same test ("the source lot floors are not the kernel floor"). Paths whose
sizing price is not the signal rule — a fill-time resize, a pure-stop entry
sized at its trigger level, a typed percentage reversal sized from the
hypothetical flatten's balance — keep `HostSized{Open}` and their own terms
branch (same test, "pure-stop default entry keeps its own branch"), but that
branch converts through the same query: the at-fill default resize is
`default_sizing_units()` at the fill price, and a typed per-call quantity
(`qty_type` cash or percent-of-equity) is `typed_quantity_units()` — the kernel's
conversion under the source's grid floor — wherever it is read: at the fill, in
the placement admission, and in the pending-order fill-quantity probe
(`strategy_pending_order_fill_qty`), which answers the booked quantity at the
price it is asked about (`tests/test_adapter_typed_entry_admission.cpp`,
`tests/test_adapter_fill_qty_probe.cpp`). No `cash / (price × point_value × fx)`
quotient is restated in the adapter. Two computations stay its own. The
percentage exits: the kernel resolves a `ScopeFraction` as `scope * fraction`,
which is not `scope * percent / 100`, and no field reconciles the association.
And the money band's *affordable price*, `sig10(sig10(equity) / notional per
unit of price)` compared against the sizing price at placement, at the terms
boundary and in the paired-reversal and carried-money checks: that is not a
conversion of money into units but TradingView's ten-significant-digit
whole-drop price (the famr / famr3 tapes of `tests/test_tv_money_precision_l4b.cpp`
and `tests/test_tv_money_band_l4b.cpp`), and on a decimal tie the kernel's
conversion and plain binary64 both refuse the lot the rule admits
(`tests/test_adapter_fill_qty_probe.cpp`, section 2; design row SZ10a).

#### Previewing a basis

`NativeStrategyHost::native_sized_units(sized, price, equity, fx)` is the
kernel's resolution as a pure query: `units = cash / (price × point_value × fx)`
with `cash` the basis value or `fraction × equity`, net of the percent fee
reserve when the intent asks for it, then the intent's grid policy — the very
function the kernel runs at acceptance and at the candidate. It moves nothing
and freezes nothing. `nullopt` means the run is not configured or the basis is
unresolvable at those inputs: a non-positive money or denominator, or a
below-one-step quotient under `SnapToGrid`. A host that gates a command on its
quantity before it submits — an affordability check, a sibling reservation —
reads the number here instead of keeping its own copy of the conversion
(`tests/test_native_sizing_bases.cpp`, "sizing preview").

#### Placement-time admission

An `AtAcceptance` quantity is also an admission input at placement, not only at
the candidate: when the request is accepted the kernel runs the run's own
opening admission — `allowed_open_directions`, `max_abs_units`,
`max_open_lots` and the initial-margin gate — against the frozen quantity at
the sizing price, converting at the acceptance point's rate, the one the
quantity was frozen at. A quantity the run cannot admit is
`RequestRejectReason::PlacementAdmission` at submit, so no request is ever
created for it. `AtMatch` has no placement quantity and keeps the candidate
gate it always had. A host that owns its own margin rule declares no kernel
margin, exactly as it does for the candidate gate, and only the caps apply.

### Fractional reduces

`ReductionSize` gains `ScopeFraction{fraction, claim, basis}`, a fraction in
`(0, 1]` of the scope the reduce is bound to:

```cpp
no::Request exit;
exit.intent = no::Reduce{no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings}};
exit.owner = no::BindOpening{opening, cycle};     // half of that one lot
```

The bound scope is the whole book for an unbound or book-bound close, the one
opening for `BindOpening`, and the live selected roster for `BindOpenings`.
`ScopeClaim::Gross` takes the fraction of that scope as it stands.
`ScopeClaim::NetOfSiblings` first subtracts the units already claimed by the
live sibling reduces bound to the same scope, so a 50 % fraction beside a live
5-unit sibling on one 10-unit lot claims 5 gross and 2.5 net. Scope identity is
the request's own authority — opening handles, position cycles and cohort
handles — never a source identifier. A bracket child that is still waiting for
its parent has no scope and resolves only after the parent fill.

`ScopeBasis` chooses *when* the scope is measured. `AtMatch` (the default)
reads the bound scope as it stands at the matching candidate, so two 50 %
siblings executed in turn on one 10-unit lot close 5 then 2.5.
`ScopeBasis::AtAcceptance` freezes the scope's size when the request is
accepted — the placement-time live basis — so the same two siblings each keep
the ten they were placed against and close 5 + 5, and `NetOfSiblings`
subtracts live sibling claims from that frozen basis. The frozen basis sizes
the claim only; the live exposure still bounds what actually closes. A
replacement is a new command and re-freezes against its own acceptance. A
request accepted with no measurable scope never becomes resolvable.

The resolution is `units = scope * fraction`, one binary64 multiplication: a
caller that spells its size as a percent converts percent → fraction itself,
because `scope * percent / 100` and `scope * (percent / 100)` are different
binary64 values. The result is floored onto `quantity_grid` like every other
engine quantity; a fraction that does not buy one whole step is
`TermsUnresolved`, and `fraction` outside `(0, 1]` is `InvalidQuantityBasis` at
submit.

Neither kind is emitted by the Pine adapter, which keeps resolving its own
`HostSized` terms; `native_order` values therefore belong to `native_order_v7`.

### Source-layer boundary

Pine/generated hosts derive from `pineforge::source::PineStrategyHost`, which
derives from `NativeStrategyHost`; handwritten native hosts also derive from
`NativeStrategyHost`. The source adapter/scheduler hash domain is
`kSourceAdapterDomain` `pine_adapter.hpp:43`
(`"pineforge-source-adapter/v4"`), while the public C ABI remains version 4.

The ownership switch is complete: the compatibility loop and the source
pending-order type are gone, and source commands lower into native requests.
The installed-header check removes `source/` and `compat/pine/`, then compiles
the declared native roots and native examples; its dependency files and `nm`
output are the evidence for this include boundary
(`scripts/check_native_include_independence.py`). The separately linkable
proof is `PineForge::kernel` — see *Building the kernel only* below.

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
`configure_native` and before a batch `run` or a `stream_begin`. Its parallel
timestamp/rate arrays must have equal length, strictly increasing timestamps,
and finite positive rates; an empty curve clears the staged value.
`account_fx` remains the fallback before the first curve point.

```cpp
pineforge::NativeFxCurve curve{{0, 900000}, {1.0, 1.01}};
const auto fx = host.configure_native_fx_curve(curve);
if (fx.status != pineforge::NativeSetupStatus::Applied) {
    // Read fx.validation; engine storage was not changed.
}
```

From C, `strategy_configure_native_fx_curve_v1` answers `-1` for every
refusal. `strategy_configure_native_fx_curve_ext_v1` answers the same and, when
the kernel judged the curve, also writes `pf_native_fx_curve_error_t` — the C
twin of `NativeFxCurveError`: `LENGTH_MISMATCH`, `NOT_STRICTLY_INCREASING`,
`NOT_FINITE_POSITIVE`, `ALLOCATION_FAILURE`, `WRONG_PHASE` — and the offending
point's index (0 for a length mismatch). A refusal of the C layer itself (a
NULL argument, a bad handle, a wrong layout) leaves both untouched.

#### Stream FX: the declared curve is the stream's FX epoch {#native_engine_stream_fx}

A curve declared through `configure_native_fx_curve` (C:
`strategy_configure_native_fx_curve_v1`) is the run's **FX epoch**: immutable
from Ready to the end of the run, refused with `WrongPhase` once the run is
Running, and named by the continuation identity before the first warmup bar. A
stream runs under it exactly as a batch does. The account converts at
`account_currency_fx_at(effective time)` of each driver point, confirmed input
reaches those points through the same `consume_confirmed_input` a batch uses,
and so a stream of confirmed bars converts — and takes its
`NativeMarginCheckKind::FxRoll` check points — exactly where a batch of the same
bars does, whether a step lands in the warmup replay or in realtime
(`tests/test_native_margin_fx_roll.cpp`, the stream twin: every offered check
point, requirement, receipt, closed row and the book left behind, ordinal for
ordinal). The last point's rate carries forward for as long as the stream
lives.

Two limits are permanent parts of that contract, not pending work:

- **No realtime rate ingestion.** The epoch is fixed at begin. A host that
  learns a new rate ends the stream and begins the next one under the new
  curve; the two runs have different continuation identities, which is the
  point — a rate that arrived mid-run would retroactively be a different run.
- **Confirmed bars only.** Tick-driven input is refused on a stream that
  declares a curve (`"a declared native FX curve requires confirmed-bar stream
  input"`; a refusal, not a failure — the stream keeps running). The reason is
  the conversion clock, not the curve: an observation hook runs before its
  print has moved the engine's presented clock, and a partially finalized slot
  calculates at its own OPEN, behind prints it has already matched. Re-measured
  with a step at T+90s: the T+100s and T+110s prints were observed with the
  presented clock still standing at T+70s, so `active_account_currency_fx()`
  answered the pre-step rate there. That clock is what every SCRIPT-VISIBLE
  conversion reads — `open_trade_profit()`, `marked_equity()`, the equity
  curve — which is what the refusal guards, and it is a different surface from
  the margin model's own conversion: a check point converts at its cursor
  ("Margin and liquidation", above) and would be correct on the tick route
  too, but the money the script reads there would not be. Rather than convert one run
  on two clocks, the tick route stays closed. Moving that clock changes what
  the run's timestamp sinks (the stream state hash, trace rows) present on the
  tick route, so it is a change of its own, with its own pins, and not a
  condition this contract waits on.

The series of the mutable setter ingress (`set_account_currency_fx_series`,
the source providers' route) keeps its `stream_begin` refusal
(`"timestamped account-currency FX is not supported by streaming"`): that
series belongs to an owner that revalues on a broker-open clock of its own,
which has no realtime route.

### Examples and the export macro {#native_engine_examples}

Both hosts on this page are built sources, not listings. They live under
`examples/native/` with one standalone, Pine-free host per feature family the
kernel exposes. Each includes only `<pineforge/native_host.hpp>` (or the
toolkit / module header over it), links `PineForge::kernel`, and prints its
summary line — the one carrying `closed trades:` — only after every check of
its own has passed:

| example | demonstrates | the suite that pins the same surface |
|---|---|---|
| `hello_kernel.cpp` | the smallest complete host: one market entry, one flatten | `tests/test_native_example_batch.cpp` |
| `hello_kernel_c.c` | the same host from C, through `<pineforge/native_c_api.h>` | `tests/test_native_c_api.c` |
| `native_market_strategy.cpp` | batch and stream lifecycles; also the live runner's MODULE | `tests/test_native_example_batch.cpp`, `tests/test_streaming.cpp` |
| `native_selected_strategy.cpp` | `HostSized`, `BindOpening(s)`, `execute_current`, `ReverseTo`; also a MODULE | `tests/test_native_example_selected.cpp`, `tests/test_native_current_execution.cpp` |
| `native_bracket_strategy.cpp` | `submit_bracket` with three `FromOwnerFill` legs on a tick ladder — take-profit, stop-loss and a trail's arm threshold, each snapped `Directional` and asserted on the ladder (103.25, 98.00, 101.75); `PendingUntilArmed`, so the working rows go from 1 to 3 at the fill; the one-cancels-all group, whose take-profit fill cancels both siblings with `CancelReason::Group` | `tests/test_native_toolkit_bracket.cpp`, `tests/test_native_anchored_legs.cpp` |
| `native_sized_report_strategy.cpp` | `Sized{CashValue}` / `Sized{EquityFraction}` resolved by the kernel; `report_policy = KernelRecorded`, `report_open_position_at_end`, reading `fill_report` | `tests/test_native_sizing_bases.cpp`, `tests/test_native_report_truth.cpp` |
| `native_margin_strategy.cpp` | `NativeMarginModel` (initial gate, `native_liquidation_price`, kernel liquidation, `MarginCallEvent`); `margin_check_allowed`, `resolve_margin_requirement` | `tests/test_native_margin_model.cpp`, `tests/test_native_margin_hooks.cpp` |
| `native_calc_on_fills_strategy.cpp` | `NativeCalculationTrigger::BarCloseAndFills`, `on_native_recalculate`, `current_partial_bar`, `NativeOpenBarView::OpenOnly` | `tests/test_native_calc_timing.cpp` |
| `native_htf_strategy.cpp` | `declare_timeframe_subscriptions`, `on_native_timeframe_bar`, `native_series_bar`, `gaps`, a `LazyComplete` bucket | `tests/test_native_htf_subscriptions.cpp` |
| `native_trail_risk_strategy.cpp` | `Trail` with `TrailTicks`, `trail_state`, `native_working_requests`, `cancel_where`; `NativeRiskLimits::max_fills_per_day`, `native_risk_state`, `NativeRiskEvent` | `tests/test_native_risk_limits.cpp`, `tests/test_native_trail_state_l5k.cpp` |
| `native_price_grid_strategy.cpp` | `NativePriceGrid` `None` / `QuantizeFills` / `QuantizeFillsAndTriggers` and `NativeGridRounding` `HalfUp` / `Directional` on one sub-tick tape: raw against booked price per fill, a ladder level as a fixed point, the stop only the quantized path reaches, `GridRequiresPriceTick` | `tests/test_native_price_grid.cpp` |
| `native_price_grid_c.c` | the same four runs from C: `PF_NATIVE_SPEC_EXT_PRICE_GRID` through `strategy_configure_native_ext_v1`, fills read back from `strategy_native_events_v1` with the C++ host's numbers | `tests/test_native_c_api.c` |
| `native_risk_limits_strategy.cpp` | the money limits: `max_intraday_loss` as a percent of the day's opening equity, `max_consecutive_loss_days`, `max_drawdown`; `FlattenAndBlock` and the kernel's own flatten (`RequestOrigin::KernelRisk`, ticket `__kernel_risk__`); `CalendarDayInTimezone`, the next-day re-arm, the ledger per day | `tests/test_native_risk_limits.cpp` |
| `native_auxiliary_feed_strategy.cpp` | `NativeRunSpec::auxiliary_feed`: a `"5"` series over 15-minute inputs built from a 1-minute feed (`NativeSeriesSource::AuxiliaryFeed`), every bucket against its hand aggregation, routing by time, and the refusal without a feed (`SubscriptionWithoutAuxiliaryFeed`) | `tests/test_native_auxiliary_feed.cpp`, `tests/test_native_auxiliary_feed_stream.cpp` |
| `native_open_lots_strategy.cpp` | `native_open_lots(mark)` lot by lot and `native_marked_equity(mark)`; a FIFO partial reduce taking its share of the entry fee; the report accessors a host inherits; which report policy folds the extremes | `tests/test_native_open_lots.cpp` |
| `native_fee_reserve_strategy.cpp` | `Sized{CashValue}` with `reserve_percent_fee`, from a JPY account on a USD-quoted instrument: 46 shares with the reserve and 47 without, both as `native_sized_units` and as fills; the commission of each leg (`ExecutionAppliedEvent::current_ticket`, `NativeOpenLot::entry_commission`, the closed row); the reserve is a no-op under `CashPerExecution` | `tests/test_native_sizing_bases.cpp` |
| `native_fx_roll_strategy.cpp` | a `NativeFxCurve` staged with `configure_native_fx_curve` under a maintenance-only margin model: a restating point that is not a step, a 150 → 160 step offered as the `NativeMarginCheckKind::FxRoll` check point (its units, lot count, mark, time and the requirement at the new rate), `native_liquidation_price` moving with the rate, and the same run without the step, which liquidates nothing | `tests/test_native_fx_curve.cpp`, `tests/test_native_margin_fx_roll.cpp` |
| `native_broker_hash_strategy.cpp` | the per-bar broker-state hashes (`set_broker_state_hash_recording`, `KernelRecorded`): one row per script bar in a batch and a stream; `hash_host_extension`, which moves every row and the scalar and no fill or continuation; replay and prefix closure within one driving; across drivings no shared row, the same closed rows and the same broker half (`broker_state_hash_from_execution_hash`) | `tests/test_native_report_truth.cpp`, `tests/test_native_host_hash_extension.cpp` |

```bash
cmake -S . -B build -DPINEFORGE_BUILD_EXAMPLES=ON
cmake --build build -j
./build/examples/native/hello_kernel
ctest --test-dir build -R '^example_'
```

`PINEFORGE_BUILD_EXAMPLES` (default OFF) builds each one as a standalone
executable and registers it as a CTest row that asserts two things: the host
exits 0, and it printed its summary line — `closed trades: [1-9]`; the market
host's line must also show a closed trade from its stream drive, the
selected host's pins its one host-sized opening of 2 units and its one
selected close, and the bracket, fee-reserve, FX-roll and broker-hash hosts'
lines pin their headline numbers: the three arm levels, the working rows and
the two group cancels; 46 against 47 shares and both commissions; the roll's
units and mark and the step-free run's zero margin calls; one hash row per
script bar, all of them moved by the extension, none shared between a batch
and a stream. CTest cannot assert both on one row (`PASS_REGULAR_EXPRESSION`
replaces the exit-code check), so every row runs its host through
`examples/native/run_example.cmake`, which fails on a nonzero exit, a signal, a
timeout or a missing line; an example registered without a summary line is a
configure error. The `test_example_runner` row
(`scripts/test_example_runner.py`) proves the runner fails in each of those
ways and that no `example_*` row sets a property that would override its
verdict. Each example is compiled with `-UNDEBUG` after the build-type flags,
as every test target is, so an `assert()` added to one aborts its row instead
of compiling to a no-op under Release's `-DNDEBUG`. The `release` and `kernel`
profiles of `scripts/ci_verify.py` turn the option on, so every `example_*`
row runs in the gate both with and without the source layer compiled (their
`examples-assert-live` stage refuses a configure in which an example's
compile command leaves `NDEBUG` defined);
`scripts/check_native_include_independence.py` compiles every source there
against the installed headers with the source trees removed, the two C hosts
with the C compiler. The market and selected examples are also built, from the
same sources, as the MODULE targets the live runner `dlopen`s
(`native_market_example`, `native_selected_example`). Those two compiles take
`-UNDEBUG` too, and `examples-assert-live` checks them wherever the runner is
built: in the `kernel` profile beside the `example_*` executables, and in the
`native` profile, which builds no `example_*` target, for them alone. The
runner's own two example modules — `native_live_example` and
`native_live_parser_example`, built from `runner/examples/` — also compile
with `-UNDEBUG`. Neither holds an `assert()`, and the stage does not read
them: neither is an `examples/native` source.

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

### Driving the kernel from C

A host that is not written in C++ does not subclass `NativeStrategyHost`: it
hands the runtime a callback table and gets the same kernel back.
`<pineforge/native_c_api.h>` (included by `pineforge.h`) is that surface —
43 additive `PF_API` symbols implemented in `src/native_c_host.cpp` by
`CCallbackHost`, a `final NativeStrategyHost` that forwards each existing
virtual to the table. No new virtual, no epoch bump, and nothing about the
established C ABI moves: the 58 compiled-strategy runtime symbols and their
counts are untouched, and `scripts/check_c_abi_runtime.py` pins the new set as
a second, disjoint inventory.

```c
#include <pineforge/pineforge.h>

static int on_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    struct host_state* state = user;
    pf_native_request_v1 request = {0};
    request.struct_size = (uint32_t)sizeof(request);
    request.version = PF_NATIVE_API_VERSION;
    request.intent = PF_NATIVE_INTENT_TRANSACT;   /* or SIZED, REDUCE, REVERSE_TO, FLATTEN */
    request.intent_value = 1.0;
    request.label = "hello-long";
    return strategy_native_submit_v1(state->handle, &request, NULL, NULL) < 0 ? 0 : 0;
}
```

The complete host is `examples/native/hello_kernel_c.c`, the C twin of
`hello_kernel.cpp`; it is built and registered as a CTest row under
`-DPINEFORGE_BUILD_EXAMPLES=ON` and prints the same `closed trades: 1`.

**Lifecycle.** `strategy_native_host_create_v1` returns an ordinary
`pf_strategy_t`: `strategy_configure_native_v1`, the whole `strategy_stream_*`
family, `strategy_get_last_error` and the read-only accessors all take it
unchanged, which is why streaming needs no new symbol. Release it with
`strategy_native_host_free` — not `strategy_free`, which does not own this
allocation. A batch run is `strategy_native_run_v1`; its report is released
with `strategy_native_report_free_v1`, because the unprefixed `report_free` is
one of the eight per-strategy exports the transpiler emits and is absent from
a runtime a C host links on its own.

**Commands.** `strategy_native_submit_v1`, `_replace_v1` / `_replace_ext_v1`,
`_cancel_v1`, `_cancel_all_v1`, `_cancel_where_v1` and `_execute_current_v1`
follow the kernel's existing legality rule: inside a callback, or between realtime inputs. A command issued
anywhere else answers `PF_NATIVE_E_STATE` and changes nothing — the kernel
throws there, and the C boundary contains that throw rather than letting it
unwind through the C frame. A rejected command names its
`RequestRejectReason` in the `reject` out-parameter:
`strategy_native_submit_v1` has always had one, and
`strategy_native_replace_ext_v1` is `_replace_v1` with the same parameter — a
second symbol rather than a wider signature, because this header's signatures
never change within a major version (the reason
`strategy_configure_native_ext_v1` exists beside `strategy_configure_native_v1`).
`_cancel_v1` needs no such spelling: its status is its reason.
`strategy_native_position_v1`,
`_working_len_v1` / `_working_get_v1` (the working view, copied out),
`_open_lot_count_v1(s, mark)` / `_open_lot_get_v1` (the open-lot snapshot,
copied out into `pf_native_open_lot_v1` — the C spelling of
`native_open_lots(mark)`, with the two strings borrowed until the next count
call, exactly like the working rows) and `_events_v1` read the run back; `_state_v1` reads the lifecycle and its typed
failure. `_cancel_where_v1` takes the text and a
`pf_native_request_field_e` (`PF_NATIVE_FIELD_COMMENT` / `_LABEL`): an
unknown selector is `PF_NATIVE_E_TAG` and a NULL text is
`PF_NATIVE_E_ARGUMENT`, because `""` is the text every request carrying no
such field matches.

**Reading the kernel back.** Eight accessors answer what the C++ host reads
off itself. Four of them are `std::optional` in C++, and `PF_NATIVE_ABSENT`
(1, a non-negative *outcome*, not an error) is the C spelling of that empty:
`strategy_native_partial_bar_v1` (`current_partial_bar()` — the bar so far at
this cursor, absent in the bar's own close calculation, where the callback
already holds the complete bar), `_series_bar_v1` (`native_series_bar()` — the
latest completed bucket of a declared subscription, absent before its first
delivery and on every bar a `PF_NATIVE_GAPS_CLEAR` series publishes nothing on),
`_trail_state_v1` (`trail_state()` — activation, running best, current level,
absent when the handle is not a live trail) and `_liquidation_price_v1`
(`native_liquidation_price()`, which also writes NaN when it is absent). The
other four always answer: `_risk_state_v1` (the risk ledger, all zeros for a run
that declares no risk block), `_marked_equity_v1`, `_recalculations_v1` (the
driven and suppressed counts — the script bar's own close calculation is not
a recalculation, so a `BarClose` run drives zero) and `_continuation_hash_v1`.

`strategy_native_cancel_where_v1` is `cancel_where()`: it matches the text
the request carries in the field the third argument names — its **comment**,
what a group of requests can share, or its **label**, one request's own
identity — and answers how many left the book. `""` is the text a request
carrying no such field matches, so it takes the comment-less requests rather
than all of them; NULL is `PF_NATIVE_E_ARGUMENT`.
`strategy_native_declare_subscriptions_v1` is
`declare_timeframe_subscriptions()`: called from inside `on_run_begin`, it
replaces the `subscriptions` the extension staged, and anywhere else — or for
a list this run's input timeframe would refuse — it stages nothing and
answers `PF_NATIVE_E_STATE`.

**The request.** `pf_native_request_v1` is translated field by field into
`native_order::Request` and is never cast onto it. It carries the intent
(including `Sized`, with basis, side, time and grid policy), the trigger
(`Market` / `Limit` with `fill_through` / `Stop` / `StopLimit` / `Trail`, with
the tick spellings, the arm price and the seeded start of the running best),
the `FromOwnerFill` anchor, the
capacity, the owner relation with its incarnations and cycle, the group and
its effect, and the label and comment. The two anchored-leg knobs ride an
additive tail behind `PF_NATIVE_REQUEST_V1_BASE_SIZE` (`anchor_rounding`,
`visibility`), and later tails add the sizing detail, the trail seed and the
arm relation (`arm_first_match`, `arm_scope`): the runtime accepts every
published length, so a caller compiled against an earlier layout keeps working
and gets the defaults. Where an anchored leg is armed is answered through
`pf_native_callbacks_v1::on_anchored_level`, the C route of
`resolve_anchored_level`, or left to the kernel's level.
`PF_NATIVE_INTENT_HOST_SIZED` is refused with `PF_NATIVE_E_UNSUPPORTED` but for
the two closes the kernel spells no other way — a cohort close
(`PF_NATIVE_OWNER_BIND_COHORT`) and a `WAIT_FOR_APPLIED` child that carries
the arm tail — whose units `on_close_units` answers: `HostSized` is otherwise
the adapter's sizing seam, and a C host sizes with `Sized`.

**The run specification.** `strategy_configure_native_v1` still takes the v1
spec. The fields added after it — report policy and the open-position row,
the price grid and its rounding, calculation timing and its recalculation
bound, the open-bar view, the generic margin model, higher-timeframe
subscriptions, and the generic risk limits — travel in
`pf_native_run_spec_ext_v1`, passed together with the base spec to
`strategy_configure_native_ext_v1`. It replaces
`strategy_configure_native_v1` rather than following it, because the kernel
configures each run exactly once: a second configure of a Ready handle fails
it. The one field of `NativeRunSpec` it deliberately does not carry is
`identity`, which the base spec owns.

The nine enum-valued words of the two specs are `uint32_t` words holding a
value of a C enumeration that names its kernel enumeration's values one for
one, each integer pinned by a `static_assert` in `src/native_c_host.cpp`:

| word | spec | C enumeration | values, the default first |
|---|---|---|---|
| `fee_kind` | base | `pf_native_fee_kind_e` | `PF_NATIVE_FEE_PERCENT`, `_CASH_PER_UNIT`, `_CASH_PER_EXECUTION` |
| `close_execution` | base | `pf_native_close_execution_e` | `PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT`, `_AFTER_CALCULATION` |
| `allowed_open_directions` | base | `pf_native_open_directions_e` | `PF_NATIVE_OPEN_DIRECTIONS_BOTH`; also `_NONE` (the zero word: a zero-filled spec admits no opening), `_LONG`, `_SHORT` |
| `report_policy` | ext | `pf_native_report_policy_e` | `PF_NATIVE_REPORT_HOST_RECORDED`, `_KERNEL_RECORDED` |
| `price_grid` | ext | `pf_native_price_grid_e` | `PF_NATIVE_PRICE_GRID_NONE`, `_QUANTIZE_FILLS`, `_QUANTIZE_FILLS_AND_TRIGGERS` |
| `grid_rounding` | ext | `pf_native_grid_rounding_e` | `PF_NATIVE_GRID_ROUNDING_HALF_UP`, `_DIRECTIONAL` |
| `calculation` | ext | `pf_native_calc_trigger_e` | `PF_NATIVE_CALC_TRIGGER_BAR_CLOSE`, `_BAR_CLOSE_AND_FILLS`, `_EVERY_MODELED_POINT` |
| `open_bar_view` | ext | `pf_native_open_bar_view_e` | `PF_NATIVE_OPEN_BAR_VIEW_COMPLETE`, `_OPEN_ONLY` |
| `margin_sizing` | ext | `pf_native_liquidation_sizing_e` | `PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM`, `_SHORTFALL_MULTIPLE`, `_FLATTEN` |
| `event_retention` | ext | `pf_native_event_retention_e` | `PF_NATIVE_EVENT_RETENTION_WINDOW`, `_FULL`, `_COMMANDS`; a caller that does not send the word keeps `_FULL` |

A value outside its enumeration is `PF_NATIVE_E_TAG` from
`strategy_configure_native_ext_v1`, and the handle stays unconfigured.
`NativeReportPolicy::KernelRecordedAtHostMarks` has no C value: its host
marks each report point from inside its own callbacks, and the callback table
has no call that marks one, so its integer, 2, is refused like any other.
`strategy_configure_native_v1` has no boundary check of its own for the three
base-spec words: the kernel's validation judges them, and a bad one answers
-1 and leaves the host failed. That call answers every refusal so — Failed
with `PF_NATIVE_FAILURE_INVALID_SPECIFICATION` for an invalid spec, with
`PF_NATIVE_FAILURE_CONTRACT` for a Ready or Running handle or a refused reuse
— and has no out-parameters; a handle an abort failed keeps that failure,
and no configure call accepts a handle a configure failed.

`strategy_configure_native_ext_result_v1` is the typed spelling of
`strategy_configure_native_ext_v1`: the same inputs plus two out-parameters
that receive the kernel's `pf_native_spec_error_t` / `pf_native_spec_field_t`
pair. With both NULL it is exactly `strategy_configure_native_ext_v1`. With
either set it also configures the next run of a Completed handle, or of one
Failed by a cooperative abort, as `strategy_configure_native_v1` reuses a
host, and it names a refused reuse: a changed session key
(`PF_NATIVE_SPEC_ERROR_SESSION_KEY_CHANGED_ON_REUSE`, field `SESSION_KEY`) or a
run number not above the highest the handle has run
(`PF_NATIVE_SPEC_ERROR_RUN_NUMBER_NOT_ABOVE_CONSUMED_HIGH_WATER`, field
`RUN_NUMBER`), each `PF_NATIVE_E_ARGUMENT`. That refusal latches a Completed
handle Failed with `PF_NATIVE_FAILURE_CONTRACT`; an aborted handle keeps its
abort and may be configured again. A Ready or Running handle, and one Failed
by anything but an abort, answer `PF_NATIVE_E_STATE` with
`PF_NATIVE_SPEC_ERROR_WRONG_PHASE` and nothing changes; success writes `NONE`
/ `NONE` and leaves the handle Ready. The third word appended with them,
`PF_NATIVE_SPEC_ERROR_ABORTED_HOST_NO_REUSABLE_RUN_SPEC`, is defensive: a
cooperative abort latches only while a run holds its spec, and the Failed
state keeps it, so no public call reaches it.

`pf_native_run_spec_ext_v1`'s third published length,
`PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE`, ends the tail after the base layout
(`PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE`) and the risk tail
(`PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE`); the runtime accepts it and the other
three. That tail appends what the header used to list as unrepresentable — the retained intrabar path
(`PF_NATIVE_SPEC_EXT_INTRABAR`), the four feed-shape and presentation
policies (`PF_NATIVE_SPEC_EXT_FEED_POLICY`: slot labels, feed tolerance, the
forced path order, abort reporting), and the margin model's remaining knobs
(equity basis, level base, the liquidation ticket strings, and
`NativeLiquidationCheck::PathAdverseExtremeMark`, which had no C value at
all). A caller sending an earlier length cannot set a mask bit its struct has
no fields for: that is `PF_NATIVE_E_STRUCT`, exactly as the risk bit already
was.

The intrabar block is also what makes `on_sub_bar` reachable: only
`PF_NATIVE_INTRABAR_LOWER_TF` retains a finer feed, and only a retained feed
has sub-bars of its own. A `PF_NATIVE_SLOT_LABEL_FEED_TOLERANT` run keeps the
caller's own labels and delivers none.

`pf_native_request_v1` likewise has four published lengths now — base, plus
the anchored-leg tail (`PF_NATIVE_REQUEST_V1_ANCHOR_SIZE`), plus the
sizing detail (`PF_NATIVE_REQUEST_V1_SIZING_SIZE`): `size_price` (`SizePrice`:
`RESOLVED`, `SIGNAL`, `SIGNAL_ON_TICK`) and `reduce_basis` (`ScopeBasis`:
`AT_MATCH`, `AT_ACCEPTANCE`), plus the trail seed: `trail_best_seed` and
`trail_has_best_seed` (`Trail::best_seed`, where the running best starts).

`pf_native_working_v1` is the first **readout** with an additive tail:
`trail_has_arm_price`, the request's own flag read back, so a trail with no
arm price and one armed at `0.0` — both of which read `p2` = 0 — are different
rows. An anchored trail reads back the spelling it was submitted with until
its owner fills and the arm installs the level. A readout owes its caller one
thing an input does not: the runtime writes it, so a caller sending the base
length (`PF_NATIVE_WORKING_V1_BASE_SIZE`) is filled exactly that far and never
past it, and any other length is `PF_NATIVE_E_STRUCT`.

The risk block (`PF_NATIVE_SPEC_EXT_RISK`) was the first **additive tail** in
this header. It appends `risk_*` fields — the two loss limits as a value plus
a percent flag, the two counts, the day basis and the breach action, each
limit opt-in through its own `has_` flag — past `reserved0`, which gave
`pf_native_run_spec_ext_v1` its second published length — beside
`PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE` (the layout this header first shipped,
defined as the offset of the first appended field rather than as a literal, so
it stays right on every target) — ending at `PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE`.
Each subscription row (`pf_native_subscription_v1`) carries `lookahead` and
`gaps` as `uint32_t` words holding a `pf_native_lookahead_e`
(`PF_NATIVE_LOOKAHEAD_AT_COMPLETION` / `_AT_FIRST_INPUT`) and a
`pf_native_gaps_e` (`PF_NATIVE_GAPS_HOLD` / `_CLEAR`) value, and may repeat a
`tf`: the rows are series instances,
delivered under their own index, and only their `authoritative_bars` are
shared. `gaps` occupies the word the row published as `reserved0` — a
reserved word every layout required to be zero — so the row's size and field
offsets are unchanged, a caller that zero-fills it keeps `PF_NATIVE_GAPS_HOLD`
(`barmerge.gaps_off`), and a value outside either enumeration is
`PF_NATIVE_E_TAG`.

A caller sending the base length keeps working unchanged and is refused with
`PF_NATIVE_E_STRUCT` if it sets the risk bit it has no fields for.
`tests/test_native_c_api_frozen_header.cpp`
configures a host from the frozen v1 copy of the struct, so that acceptance is
executed rather than asserted.

The auxiliary finer feed (`PF_NATIVE_SPEC_EXT_AUXILIARY_FEED`) is the fourth
additive tail, behind the risk one and the intrabar / policy one:
`auxiliary_tf`, `auxiliary_bars`, `auxiliary_n`, a reserved word that must be
zero, and `subscription_sources` — an optional array of
`pf_native_series_source_e`, one word per subscription row, `NULL` meaning
every series is built from the input (the subscription row itself has no spare
word left, so the source rides beside it rather than in it). The event
retention (`PF_NATIVE_SPEC_EXT_EVENT_RETENTION`, R5 lane V19-B) is the last:
`event_retention`, a `pf_native_event_retention_e` word, and a reserved word
that must be zero. The struct therefore has five published lengths and the
runtime accepts each: `PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE`,
`PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE`, `PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE`,
`PF_NATIVE_RUN_SPEC_EXT_V1_AUXILIARY_SIZE` (each the offset of the first field
of the tail behind it, not a literal) and the current `sizeof`; a caller is
refused with `PF_NATIVE_E_STRUCT` for a bit whose tail it does not carry, and
any other length is refused outright. A caller that does not set the
retention bit keeps `PF_NATIVE_EVENT_RETENTION_FULL`, so every caller
compiled before the tail existed reads back the record it always did. A realtime stream appends later feed
bars with `strategy_native_append_auxiliary_bars_v1`, the C spelling of
`append_auxiliary_bars`: `PF_NATIVE_OK`, or `PF_NATIVE_E_STATE` for every
by-name refusal above with the reason in `strategy_get_last_error`.
`tests/test_native_c_api.c` runs the batch, the stream and each refusal from
pure C.

**The callback table.** `pf_native_callbacks_v1` has **four published
lengths** and the runtime accepts each: the layout this header first shipped
(`PF_NATIVE_CALLBACKS_V1_BASE_SIZE`, again the offset of the first appended
field rather than a literal), that plus six hooks
(`PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE`), that plus the policy-hook tail
(`PF_NATIVE_CALLBACKS_V1_POLICY_SIZE`), and the current one, whose trailing
`reserved1` marker (documented zero, not checked) says the host reads the
lot-excursion facts' `entry_commission` tail. A host compiled against an
earlier layout keeps working and simply has none of the later hooks
installed, which is exactly the kernel's own default for each.

Two of the six are ordinary observation callbacks. `on_recalculate` is
`on_native_recalculate`: every calculation of the run arrives there, tagged
with a `pf_native_calc_reason_e` and, for an `ORDER_FILL`, the applied
execution that caused it. Installing it **replaces** `on_bar` for every
calculation, exactly as overriding `on_native_recalculate` replaces the C++
default forwarding; leaving it NULL keeps the established contract where the
kernel forwards every calculation to `on_bar`. `on_sub_bar` is
`on_native_sub_bar`, delivered once per completed lower-timeframe sub-bar of
a retained lower feed.

The other four are **answering** hooks — `on_margin_requirement`
(`resolve_margin_requirement`), `on_margin_check` (`margin_check_allowed`),
`on_margin_call_units` (`resolve_margin_call_units`) and `on_lot_excursion`
(`owns_lot_excursions` + `closed_lot_excursion`) — and they read their return
value differently: it is a `pf_native_answer_e` selecting WHOSE answer the
kernel uses, not a success code. Every value is therefore in contract and an
answering hook can never fail the run. That split is not cosmetic: those four
are consulted from kernel paths that are **not** inside the consumer's
callback guard, so a failure raised there could not be latched without
unwinding through them, which is the one thing this boundary must never do. A
host that must abort does it from an observation callback, where the
established "non-zero ends the run `Failed`" rule is untouched.

A fifth answering hook, `on_close_units`, is the **units half** of
`resolve_execution_terms` — and the only half this header exposes. It is
consulted for `PF_NATIVE_INTENT_HOST_SIZED` candidates and nothing else, and
it is what makes `PF_NATIVE_OWNER_BIND_COHORT` reachable: the kernel pairs
that owner with exactly one intent, a host-sized close, whose quantity comes
from the terms pass. `pf_native_close_view_v1` carries `scope_exposure_units`
— for a cohort close, the live units of the roster's own openings — plus the
price the kernel would settle at and the cursor; the price and opening-shape
halves of the terms answer stay the kernel's.

So a C cohort close is: `strategy_native_cohort_open_v1`, one
`strategy_native_cohort_add_v1` per opening (and `_remove_v1` to take one back
off), then a `PF_NATIVE_INTENT_HOST_SIZED` request with
`owner = PF_NATIVE_OWNER_BIND_COHORT` and `cohort` set, with `on_close_units`
answering how much of the roster's exposure to take. HOST_SIZED under any
other owner stays `PF_NATIVE_E_UNSUPPORTED`: every other host-sized shape
needs the parts of the terms answer that are not exposed, so it is refused at
submit rather than accepted and then left unresolvable at the candidate.

Installing `on_lot_excursion` at all is `owns_lot_excursions() == true`: the
consumer then stops sampling excursion at matched trigger
prices for the whole run and every closing row takes both magnitudes from the
hook — so a lot the hook declines gets the kernel's own zero magnitudes,
because nothing was sampled for it. The facts it is handed,
`pf_native_lot_excursion_v1`, end in an additive tail: `entry_commission`, this
closing slice's share of the entry fee in account currency (the C++
`ClosedLotExcursionFacts::entry_commission`). Only a table sent at the current
length receives it; one sent at an earlier length is handed the facts with
`struct_size` = `PF_NATIVE_LOT_EXCURSION_V1_BASE_SIZE`, which stops before it.
The three margin hooks share one view
POD, `pf_native_margin_view_v1`, whose fields are documented per hook and
zero where that hook has no such fact, exactly as `pf_native_event_v1`'s
union is.

The owner of the excursions also says where each lot's opening fill sat on
its entry bar: `strategy_native_declare_opened_lot_entry_bar_mask_v1` is the
C spelling of the protected `BacktestEngine::declare_opened_lot_entry_bar_mask`.
It takes the lot's entry incarnation
(`pf_native_applied_v1::opened_lot_incarnation`), the whole entry bar and a
`pf_native_opened_lot_fill_point_e` — `ON_PATH` for a fill at a price the
bar's path reaches, `AFTER_PATH` for one at the bar's closing point — and the
kernel derives which ends of the bar the path had reached before the fill,
walking the bar in the run's own leg order (the spec extension's
`path_order`, the order the matcher walks; the open-proximity rule under
`AUTO`), and hands both flags back as
`pf_native_lot_excursion_v1::entry_bar_high_masked` /
`entry_bar_low_masked`. It is legal inside `on_applied` alone and
`PF_NATIVE_E_STATE` everywhere else. Being a member of the base rather than
of `NativeStrategyHost`, it is a row of the COVERAGE block's second list,
BASE-CLASS SEAMS (below). The entry-bar mask scenario of
`tests/test_native_c_api.c` reproduces the C++ witness
`tests/test_e6_entry_bar_mask_declaration.cpp` number for number.

**What is not exposed, and why.** The header opens with a **COVERAGE** block:
one line per public member of `NativeStrategyHost`, carrying either the C
spelling (`[C]`) or the reason there is none (`[--]`). Five members are excluded today — `prepare_native_begin` (it borrows the codegen ingress a C
host never supplies), `inspect_current_execution` (its preview carries the
account-effect projection and a variable-length closed-row P&L vector with no
size-prefixed POD; `strategy_native_execute_current_v1` answers the same
verdicts) and `submit_market` / `replace_market` (C++ conveniences that refuse
non-market extras; `native_closed_rows_amended` is the C++-only row-amendment
notification; the same request is `strategy_native_submit_v1` with
`PF_NATIVE_TRIGGER_MARKET`). Every policy hook has a C route since R5 lane F4:
`validate_execution_precommit` is `on_precommit`, `resolve_anchored_level`
`on_anchored_level`, the price and shape half of `resolve_execution_terms`
`on_execution_terms`, `declare_auxiliary_feed`
`strategy_native_declare_auxiliary_feed_v1` and `native_sized_units`
`strategy_native_sized_units_v1`. A table that leaves `on_bar_open` or
`on_precommit` out declares that hook absent when the host is created
(`declare_native_bar_open_hook` / `declare_native_precommit_hook`, R5 lane
D2-A), which moves no value. `scripts/check_native_c_api_surface.py`
proves the block is exactly that class's public surface and runs as a source
guard in every `ci_verify.py` profile, so the list cannot silently go stale;
`scripts/test_check_native_c_api_surface.py` proves the guard can fail. A
second list, **BASE-CLASS SEAMS**, is the same census for the members of the
base a host is documented to call or override from its callbacks, opted in
one by one by a `@host-seam` line in `engine.hpp`: today the entry-bar mask
declaration and `hash_host_extension` (both spelled, the second as
`on_hash_extension`) and its deprecated spelling `hash_source_extension`
(excluded: C only ever had the current spelling).

**Errors and hardening.** Every struct is tagged and size-prefixed
(`struct_size`, `version`); a mismatch is `PF_NATIVE_E_STRUCT`, an enumerator
outside its enumeration is `PF_NATIVE_E_TAG`, and neither mutates anything.
A C callback must not unwind; returning non-zero latches
`NativeFailureCode::CallbackException` (`PF_NATIVE_FAILURE_CALLBACK`, 5) and
the run ends `Failed`, readable through `strategy_native_state_v1`.
`tests/test_native_c_api_frozen_header.cpp` compiles a fixture against the
byte-frozen v1 copy of the header in `tests/fixtures/native_c_api/v1/` and
links it against the current runtime, so an unnoticed layout change shows up
as the refusal the header promises rather than as silent misreading.

**Events.** `strategy_native_events_v1` flattens `native_events()` into one
tagged POD: the nineteen `CommandEvent` alternatives, plus the driver point
and the account observation — the rows the run's retention keeps. A C host
under `PF_NATIVE_EVENT_RETENTION_WINDOW` acknowledges what it read with
`strategy_native_acknowledge_events_v1` (from `on_run_begin` with 0, then its
cursor) and reads where the window starts with
`strategy_native_event_window_v1`, the C spellings of
`native_acknowledge_events` and `native_event_window_start`. One kind named in the design is **not**
represented and never appears: a completed higher-timeframe bucket, which is
delivered through the `on_timeframe_bar` callback and never recorded in the
event history. Ordinals are non-decreasing rather than strictly increasing —
an applied execution and the account observation it produced share one — so a
page never ends in the middle of such a group and a poller can advance by the
last returned ordinal.

`NativeRiskEvent` is the nineteenth alternative and arrived after this
header froze, so it took a tag of its own past the two observations —
`PF_NATIVE_EVENT_RISK` (21) — rather than taking 19 and renumbering
`PF_NATIVE_EVENT_DRIVER_POINT` and `_ACCOUNT`. A reader compiled before it
skips it by tag, which is how any unknown tag must already be treated, and
only ever meets it in a run whose spec declares `risk` — a spec that reader
cannot write. The event names no request, because the block it opens is an
account fact: `incarnation` stays 0, `reason` is the
`pf_native_risk_limit_e` that breached, `price` is the observed value,
`raw_price` the limit it reached (already resolved against its basis equity
when the limit was a percent), `cycle_before` the risk day on the spec's own
day basis, `successor` the cursor's matching-point ordinal, and the cursor
fields are set. There is no new callback: the history is the delivery path,
so `pf_native_callbacks_v1` does not move.

### Known limits

These are contract, not pending work.

- **No broker-open FX epoch clock, and none is needed.** The declared
  `NativeFxCurve` is the run's FX epoch, a step of it is the `FxRoll` margin
  check point, and a confirmed-bar stream runs under it as a batch does
  (@ref native_engine_stream_fx). Tick-driven input under a declared curve
  stays refused, for the conversion-clock reason stated there.
- **The precommit verdict is not previewed.**
  `inspect_current_execution` reports typed terms outcomes — facts about the
  proposed terms — and never the host's own `validate_execution_precommit`
  verdict, which runs once, on the physical attempt.
- **A series finer than the input needs a declared feed.** Without
  `NativeRunSpec::auxiliary_feed` a subscription finer than `input_tf` is
  refused at configure rather than emulated, and only the run's own symbol is
  addressable: there is no auxiliary-symbol feed and no chart-slice mapping.
- **TradingView's `request.security` rules are not reachable from a bare
  host.** The lookahead merge latch, the calling-bar publication gates,
  Heikin-Ashi substitution, the range-start cut and `request.security_lower_tf`
  emulation are source-layer code, and none of them can change a
  subscription's buckets.
- **The adapter leaves nine capabilities outside its declaration surface** — eight
  are native-only or adapter-policy (`price_grid`, `risk`, `max_abs_units`,
  `max_open_lots`, `initial_margin_fraction`, `report_open_position_at_end`,
  `open_bar_view`, `auxiliary_feed`) and one is an adapter-hook
  (`subscriptions`) — each with a ruling and an executed consumer, with the
  measurement that decided it: `docs/adr/0001-kernel-adapter-boundary.md`
  ("Kernel capabilities the Pine adapter does not declare") and
  `docs/design/native-feature-parity.md` §3.6 / §3.7.

### What still requires Pine compatibility to build

The standalone native host has no Pine decision path at runtime, and the
constructor/member cut has since landed: `engine.hpp` has **zero** references to
`CapAttachment`, `OrderPriority` or `IntradayCap`. `NativeStrategyHost` is
zero-argument (`native_host.hpp:826`); the `CapAttachment` constructor belongs
to `source::PineStrategyHost` (`pine_strategy_host.hpp:241-244`), and the cap
type itself lives in the adapter (`intraday_cap.hpp:18`).

Nor is there a build-level one. The two source sets are disjoint:
`PINEFORGE_SOURCE_LAYER_SOURCES` (`CMakeLists.txt:91`) holds all six
`src/compat/pine/` units and all ten `src/source/` ones, and
`PINEFORGE_KERNEL_SOURCES` (`CMakeLists.txt:113`) holds the kernel's own
thirty-five, which are what `add_library` (`CMakeLists.txt:153`) compiles into
`pineforge_kernel`. `libpineforge.a` still carries both sets when
`PINEFORGE_BUILD_SOURCE_LAYER` is ON, which is the default; the kernel archive
exists either way. The installed-header closure is clean too, which the
independence checker proves
(`sha256:7ae64b598bf26ee68b06a7746c27c3ae62f620b0443b6c8c12781b20fe20bcd5` check_native_include_independence.py:36-46). See
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
`PINEFORGE_KERNEL_SOURCES` with exactly the flags `pineforge` uses. It does not
mention the source layer at all — the rich begin bridge carries its host
overrides as an opaque `const void*` — so it links standalone;
`scripts/check_native_include_independence.py --kernel-archive` asserts that
with `nm` over the archive's defined and undefined symbols, with no whitelisted
symbol.

With the option OFF the build excludes, each with a CMake STATUS line:

- `src/source/*.cpp` and `src/compat/pine/*.cpp`, so `libpineforge.a` holds
  the kernel objects alone (equivalent to `libpineforge_kernel.a`);
- the installed `include/pineforge/source/` and `include/pineforge/compat/`
  headers, which would otherwise declare functions with no definition;
- every Pine-bound target: the corpus and bench strategies, the tutorial
  strategy, and the live runner's `runner/examples/strategy.cpp`
  (`native-market-example` and `native-selected-example` still build);
- every test translation unit that reaches a `pineforge/source/` or
  `compat/pine/` header, and the receipt-backed ABI rows whose pairing TU
  derives from `PineStrategyHost`. The remaining CTest rows all run.

A kernel witness therefore never shares a translation unit with its adapter
twin: the native half is source-free, the twin lives in a sibling
`tests/test_<feature>_twin.cpp` that the reach check drops, and what both need
sits in a source-free `tests/<feature>_fixture.hpp`. The feature suites follow
that split — `test_native_report_truth`, `test_native_margin_hooks`,
`test_native_calc_timing`, `test_native_htf_subscriptions`,
`test_native_price_grid` and `test_l11a_host_excursion` each run kernel-only
with their twin beside them — so the kernel-only profile proves the kernel's
own features, not only the rows that happened to be source-free.

`python3 scripts/ci_verify.py kernel` is the profile that verifies the
kernel-only build (Release, live runner ON, tutorial OFF, source layer OFF);
CI runs it as the `kernel-only` job. The profile carries a **row floor**: `KERNEL_MIN_TESTS` in
`scripts/ci_verify.py` is the count the kernel-only CTest set is
expected to run, and the `ctest-floor` stage fails the run when fewer rows
ran or CTest printed no count it can read, so a test TU that silently becomes
source-bound (or a filter that empties the suite) is a refusal rather than a
smaller green. A row counts only when its test ran to a verdict: a row CTest
skipped (`test_native_live_websocket` exits 77 wherever libcurl lacks
WebSocket support) or could not start is listed beside the count, in
`ci-logs/ctest-floor.log` and as `ctestSkipped` / `ctestNotRun` in
`ci-summary.json`, and never counted, so a row that stops executing fails the
floor as well. The `release` profile carries the same gate with
`RELEASE_MIN_TESTS`, so a row that leaves the default build fails CI too.
Raise the constant when a row lands; `--min-tests N` overrides it for one run
of any profile (`debug`, `sanitizers` and `native` carry no default floor).

### What the kernel-only archive still names

An audit of `strings libpineforge_kernel.a` for Pine / TradingView vocabulary
is now a gate:
`scripts/check_kernel_residuals.py --archive build-kernel/lib/libpineforge_kernel.a`
scans what a consumer LINKS and only that — the symbol table (`nm -C` over the
archive, defined and undefined, demangled) and the string literals (`strings -a`
over a copy whose debug information has been stripped with `objcopy
--strip-debug`, `llvm-objcopy --strip-debug` or the platform `strip -S`, and
exit 2 when the host has none of them) — so the verdict is the same in every
build type: a `-g` archive's DWARF names every block-scope local, every struct
member and every source path, and none of those is a residual surface. It
matches a fixed residual vocabulary against whole identifiers only: at least
three bytes, delimited by non-identifier bytes, so machine code `strings` prints
as `C0"TV"` is not a name. The vocabulary (an identifier containing `pine` other
than `pineforge`, `tradingview`, `barmerge`, `coof`, `pooc`,
`market_admission`, `calc_on_order_fills`, `process_orders_on_close` or a `tv`
segment; a text containing `strategy.<name>`, `ta.<name>`, `request.security`,
`barmerge.<name>` or `__margin_call__`, where `<name>` is a Pine member and not
a C/C++ file suffix — `ta.ema` is a call, `ta.hpp` is this project's header, and
a sanitizer build writes every source path into rodata as a real literal)
requires every match to be listed,
by name, in the first column of the ADR's residual tables — and every listed
match to still be in the archive. The `kernel` profile runs it as the
`kernel-residuals` stage right after the build; every profile runs it as the
CTest row `test_kernel_residuals` (with the checker's own must-fail
self-tests). What remains is listed with its ruling in
`docs/adr/0001-kernel-adapter-boundary.md` ("Residual TradingView-named
surface"); the short version:

- **Neutral spellings.** The kernel's public utility names are the neutral
  ones: `NumericMatrix` / `GenericMatrix<T>` (`matrix.hpp`,
  `generic_matrix.hpp`), `timeframe_time` / `local_hour` /
  `session_in_market` (`session_time.hpp`), `str_format` / `str_tostring`
  (`str_utils.hpp`), `deterministic_random` (`math.hpp`) and
  `float_band_eq` … (`ta_compare_band.hpp`). The `Pine*` / `pine_*`
  spellings are exact deprecated aliases kept for generated code; the
  `pine_float_*` shim ships only with the source layer
  (`include/pineforge/source/pine_float_compare.hpp`).
- **Frozen pending-row field names.** The seventeen `pine_exit_activation_*`,
  `pine_frozen_market_instruction_*` and `pine_birth_reach` reflection
  strings, `tv_carry_qty`, the seven `coof_*` (calc-on-order-fills), the
  three `pooc_*` (process-orders-on-close) and the seventy-eight
  `market_admission_*` names (the frozen mirror of the source-layer admission
  journal) belong to the frozen `pf_pending_order_v1_t` mirror, an
  append-only C ABI contract published by `strategy_pending_order_layout`.
  The kernel translation unit holds each reflection row — name, C type,
  offset, size — and never a value: the values are projected by the source
  layer alone, and a bare host's `strategy_pending_orders_len` is 0. The
  neutral view is `native_working_requests()`. Each family has its ADR row,
  listing every name, and the gate above holds the list.
- **Deprecated public spellings** (`docs/adr/0001-kernel-adapter-boundary.md`,
  "Deprecated public spellings" — a separate table, deliberately outside the
  residual section the gate parses, because none of these names is a symbol or
  a literal in the archive). The C ABI's `pf_equity_stats_t::sharpe_tv` /
  `sortino_tv` are now `sharpe_monthly` / `sortino_monthly` — the same `double`
  at the same offset behind a C11 anonymous union, so nothing an FFI consumer
  links or reads moves; the old spelling is deprecated and removed at the next
  `PF_ABI_VERSION`, and the serialized report key stays `sharpe_tv`. The
  standalone `lifecycle_v1` enumerators `exit_legs::Domain::Coof` /
  `MagnifierCoof` are now `FillRecalc` / `MagnifierFillRecalc` (the
  fill-recalculation re-entry pass), with the old names kept as
  value-identical aliases until `lifecycle_v2`. <!-- verified HEAD -->
- **The ambient EMA seeding default.** `ta::EMA` seeds from its first finite
  input (`EmaSeeding::FirstValue`, the default) or from the simple average
  of its first `length` inputs (`EmaSeeding::SimpleAverage`, na for the
  warm-up window the way `RMA` and `SMA` warm up). A host names the seeding
  per instance, `ta::EMA ema(length, ta::EmaSeeding::SimpleAverage)`, and
  touches no global. `ta::ema_na_warmup_flag()` is the calling thread's
  ambient default (held in its runtime block: the thread's own, or the
  running pump's while one runs) that an instance naming no seeding latches
  on its first `compute()`; the Pine adapter raises it around one evaluation context
  under its opt-in run flags, the way any host may. It is ruled in the ADR as
  a generic ambient indicator option (a mechanism the vocabulary gate cannot
  see); nothing in the kernel raises it.
- **Host-only modes behind frozen C setters.** `strategy_set_realtime_tail`
  and `strategy_set_probe_suppress_tail_logic` are the Pine source host's
  live-probe protocol. On a bare `NativeStrategyHost` the kernel's virtual
  seams `set_realtime_tail` / `set_probe_suppress_tail_logic` are accepted
  and inert, as the C setters always were on a native module: the C++ seam
  answers `false` ("no such mode here"), `last_error()` stays empty, and the
  run is byte-identical to one that never asked.
- **`request.security` in three feed-store messages.** The last_error texts
  of `set_native_security_feed` name the feature the frozen C export
  `strategy_set_native_security_feed` documents; the mechanism is the
  generic authoritative-feed store above.

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
`PINEFORGE_BUILD_TUTORIAL` ON, `PINEFORGE_BUILD_EXAMPLES` OFF (ON in the
`release` and `kernel` CI profiles), `PINEFORGE_ENABLE_SANITIZERS` OFF,
`PINEFORGE_STRICT_WARNINGS` OFF. The
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
