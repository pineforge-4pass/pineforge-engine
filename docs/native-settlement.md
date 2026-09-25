# Native resolved execution settlement

`BacktestEngine::settle_native_execution_at` is a protected C++ extension for
trusted native matching adapters. It applies an already admitted, matched
execution using a supplied `PhysicalExecutionContext` and the engine's one
physical lot book. Scoped and selected variants use the same financial owner.
Use `PhysicalExecutionContext{}` when zero time/index and no preceding-path
facts are appropriate; native settlement has no implicit source chart clock.

There is no implicit-chart-context compatibility alias. Callers use the
explicit native/context seams, while the remaining selected/reversal source
coordinators supply their chart context directly. Every route books physical
and financial effects once.

The caller supplies an `execution::Action` and `execution::Fill` from
`<pineforge/execution.hpp>`. These values describe immediate effects:

| Action | Effect |
| --- | --- |
| `Flatten{}` | Close every physical lot. |
| `order_action::Reduce{units}` | Close up to a finite, nonnegative quantity in FIFO order; never open or reverse. |
| `order_action::Transact{signed_units}` | Buy positive units or sell negative units; reduce opposite exposure first, then open any remainder. |

`Fill` carries the resolved finite price, order ID, comment, incarnation and
an optional total commission in account currency. Zero and negative prices
are representable; the caller decides which prices its instrument permits.
Settlement does not apply another tick rounding, slippage, lot step,
pyramiding cap or affordability decision.

## Physical lots and quantities

FIFO allocation retains surviving lot identities, entry prices, paid costs
and proportional excursions. The surviving physical quantities determine the
resulting position and weighted entry price. For example, reducing lots
`{0.1, 0.2}` by `0.1` leaves the existing `0.2` lot, even when regrouping
binary64 arithmetic as `aggregate_before - reduction` differs by one ulp.

A finite quantity request can leave a positive floating residual. No native
epsilon silently discards it. `Flatten` explicitly closes all lots without
comparing any quantity; a request is never taken for a whole-book close because
it is near the book's aggregate, and it closes a lot whole only where its
binary64 FIFO sum reaches the request exactly (below). Quantities whose changes
cannot be represented are refused before settlement.

A close that spans lots and ends inside one of them is charged exactly its
units. Every lot before the one it ends in closes whole, and C is the binary64
sum of what they closed; that lot closes the rest `r = fl(units - C)` and keeps
`fl(qty - r)`, or closes whole when `r` is its full size. The execution's
`closed_units` is the request itself, not `fl(C + r)`. That sum can land one ulp
either side of the request (`C + r` is then a tie on the request's grid), and
then no binary64 quantity `x` has `fl(C + x)` equal to the request, so no choice
of rows adds up to it. Before this rule (R5 lane K-ULP2) a sum above the
request was refused with `UnrepresentableQuantity`, and one below left a
2^-51 rest for a second fill, for a later lot, or as an opening beside the
survivor. For example, lots `{fl(106/84.5), 10}` reduced by `fl(277/84.5)` close
`fl(106/84.5)` and `fl(277/84.5 - 106/84.5)`: the two rows add up one ulp above
the request, and the execution reports `fl(277/84.5)` closed. A whole lot whose
sum falls short of the request is not where it ends; the rest beyond it is real
and the next lot closes it. A split that binary64 cannot hold is still refused:
a rest below half an ulp of its lot, a rest that a whole lot's close cannot
decrement, and a reduction that cannot move the position.

A close whose units are exactly the binary64 FIFO sum of the lots through one
of them -- `fl(C + qty)` equals the units for that lot's full size, as a close
of the book's own held total or of a FIFO prefix of it does -- closes that lot
whole. Its rest `r = fl(units - C)` can come out below the lot's size: the
units are themselves a rounded sum, and `units - C` is rounded again when `C`
is below half of them. The lot then kept `fl(qty - r)`, at most one ulp of the
units (2^-55 to 2^-50 for lots near one unit). Both splits add up to the units
exactly, so neither the charge nor the rows told them apart, yet the kept part
stayed in the book as a dust lot, and after a close of the whole book as a dust
position (before R5 lane K-ULP3). For example, lots
`{fl(80/84.5), fl(92/84.5)}` reduced by their own sum `0x1.048b5c670183cp+1`
closed `0x1.16b8ce030792ep+0` of the second lot and kept `2^-52` of it; now
both lots close whole, the rows add up to the units, and the book is flat.
Units strictly between two such sums still end inside a lot and keep its
residual. A selection closed by its whole sum was already consumed whole, and
`Flatten` needs no quantity at all.

The source `execute_partial_exit_qty` adapter retains its existing `1e-10`
FIFO endpoint policy. After its existing whole-book Flatten check, it may
translate a quantity ending at an interior whole-lot prefix into one selected
Flatten. It walks physical lots in FIFO order, stops before the next lot once
the source endpoint is reached, and selects complete opening incarnations,
independently of the logical order label that supplied the close quantity.
A genuine partial lot, an unowned selected lot, or an opening with a fragment
outside the prefix keeps the original scalar Reduce path. Selected Flatten
closes the exact live quantities and all their remaining paid entry costs in
one execution; it does not reproduce historical residual-cost discards.
The native Reduce contract, entry-scoped adapters and transaction-flow requests
retain their existing behavior. This source translation requires separate
compatibility measurement; it does not establish that every observed extra
trade row has the same cause.

Not every full exit reaches the kernel as a `Flatten`: a default
`strategy.close(id)` on a pyramided id is a Book-scope `Reduce` of the id's
quantity, and so are a bracket exit answered with the book's total, a margin
liquidation of the whole position and an explicit-quantity order equal to
it -- the closes the exact-sum rule above now closes lot by lot (R5 lane
K-ULP3).
The Pine source host keeps one quantity rule of its own: after every applied
execution, `source::PineStrategyHost::on_native_applied` erases any lot of at
most `kQtyEpsilon` (`1e-10`) without a closing row, the settle rule of the
legacy engine it restates. That is source-layer TradingView policy, not the
kernel's; since K-ULP3 an exact-sum close no longer leaves such a lot for it.

## Reversal to an exact exposure

`execution::reverse_to_v1::ReverseTo{signed_units}`, declared in
`<pineforge/execution_reverse_to.hpp>`, closes the entire opposite live book
and opens exactly `abs(signed_units)` units in the requested direction. Positive
targets open long; negative targets open short. It is a separate, call-local
resolved-execution value available to trusted C++ adapters through four protected,
nonvirtual methods:

| Method | Result and context |
| --- | --- |
| `inspect_native_reversal_v1(reversal, fill)` | Non-mutating `SettlementInspection` of physical effects and the current ticket. |
| `project_native_reversal_v1(reversal, fill)` | Non-mutating `AccountEffectProjection`, including account effects and the next cycle without consuming it. |
| `settle_native_reversal_at_v1(reversal, fill, context)` | `Result` from settlement using the supplied `PhysicalExecutionContext` and empty lifecycle effects. |
| `settle_reversal_with_lifecycle_v1(reversal, fill, lifecycle)` | Source-coordinated `Result` using current chart context, supplied lifecycle effects and source-day preflight/observation. |

For example, reversing a held long position of `1` unit to a short target of
`0.1` opens the exact binary64 quantity supplied as `-0.1`. The opening quantity
is never reconstructed by adding the old position to the target and subtracting
the closed position. Every existing lot closes in roster order using whole-lot
Flatten allocation, and one new lot opens at the same resolved `Fill::price`.
Close and open effects share one current ticket and the existing financial
commit owner. No selection or partial-close scope participates in this seam.

A valid call requires a nonflat book opposite to a finite, nonzero target.
Zero or nonfinite targets return `InvalidQuantity`; a flat or same-side book
returns `InvalidCloseTarget`; malformed physical books return `InvalidBook`.
There is no valid no-effect reversal. Callers use `Flatten` to close without
opening. If finite closed and opening quantities have a nonfinite gross sum,
the call returns `UnrepresentableQuantity` before quoting fees. A tiny target
such as `0.1` against a held quantity of `1e16` remains valid even when their
floating-point sum absorbs the target: after all old lots close, the exact new
quantity is representable on its own. Existing accounting, lifecycle and
sequence preflight checks still apply before effects.

Inspection, projection and settlement share allocation and fee calculations.
Successful `opened_units` and projected `signed_units_after` preserve the signed
target bits; the resulting book contains one lot of exactly its absolute size.
Projection includes the opening's paid cost once and peeks the fresh cycle;
settlement revalidates the live book and consumes that cycle when it opens.
Invalid projections contain no usable account quote. These values grant no
saved or replayable settlement authority.

`Transact` continues to describe signed transaction flow, including its existing
opposite-book remainder arithmetic. Native queued requests and their action and
event algebra are unchanged; `ReverseTo` is not an `Action` or request variant.
The source F7 reversal adapter uses this seam after resolving its opening size.
Other flow callers, including F8, retain `Transact`. This opening-intent repair
does not establish that the seven additional trade rows or the EURUSD (ERA)
quantity/margin differences observed during refactor verification are repaired;
those remain separate compatibility acceptance work.

## Costs and marked equity

Every opening lot receives its paid entry commission before its entry
observation is emitted. Partial closes allocate this stored payment in
proportion to closed quantity for every commission type. The survivor keeps
the unconsumed cost. Later fee schedule or FX changes do not rewrite a payment
already made.

The current execution is quoted separately. Percent and per-contract charges
follow each physical quantity. A cash-per-order charge belongs to the entire
native execution, including both legs of a reversal; it is allocated across
the close/open effects with the final effect receiving the floating residue.
An explicit `Fill::commission_account` overrides the modeled total and can
represent a zero-fee waiver or a negative rebate. A nonfinite quote, or a
nonzero quote attached to a no-effect quantity, is refused.

For a modeled native percent schedule, the resolved notional uses the absolute
fill price: `abs(fill.price) × quantity × pointvalue × account FX × rate`.
Thus a positive 1% schedule charges 2 on a two-unit execution at a resolved
price of -100; the signed price cannot turn that modeled charge into a rebate.
An explicit negative `Fill::commission_account` remains an observed rebate and
is preserved as such. Excursion fields remain nonnegative magnitudes, so a
flat-price rebate may increase run-up but cannot produce a negative drawdown.

For example, with a cash ticket of 6, opening 3 units and reducing 1 unit
allocates 2 of the paid entry ticket plus 6 for the reduction to the closed
row. The remaining 2 units carry entry cost 4. Flattening those units costs
another ticket of 6: total paid across the three executions is 18.

`marked_equity(price)` is initial capital plus realized PnL and marked open
PnL, minus remaining paid entry costs of every fee type. At an unchanged mark,
a native execution decreases this value by its current charge, subject to
ordinary floating-point rounding. A rebate increases it.

## Selected exposure and account projection

`execution::SelectedOpeningSet` selects one or more opening identities in the
current position cycle. The separately named selected settlement methods accept
it by const reference; it is not another `CloseScope` alternative or a queued
request field. All fragments of each selected opening participate in roster FIFO
order. Empty sets, zero or duplicate identities, missing openings and stale cycles
are refused before effects. Source adapters construct distinct identities from
`from_entry` or other source predicates; the native owner does not interpret them.

`project_native_settlement_v1`, `project_native_settlement_scoped_v1` and
`project_native_settlement_selected_v1` quote the resulting physical and account
facts without changing the engine. The projection includes realized balance,
remaining paid entry costs, marked equity, signed exposure and resulting cycle.
It includes unselected surviving lots and any prospective opening's paid cost.
A valid no-effect action quotes the unchanged book at the supplied mark; invalid
input supplies no usable quote. An unavailable fresh cycle throws without
consuming it. Same-side additions retain the existing cycle.

Projection and settlement share allocation and fee logic. Realized balance adds
close-row PnL in commit order, and marked equity applies each resulting lot's mark
and paid cost in roster order. A projection is data, not commit authority: later
settlement revalidates against the current book. Source percent sizing can consume
a close-only projection's equity before resolving one reversal opening quantity.

## Source-day observation boundary

Native Book/scoped/selected/ReverseTo settlement and the generic
`settle_with_context` seam do not read or update Pine intraday PnL or
consecutive-loss-day counters. Their inspection and account projection methods
are likewise independent of these source fields. An exhausted source day count
or nonfinite cached source intraday value cannot refuse an otherwise valid
native execution. Financial win/loss/even, entry/cycle/stream capacity and
lifecycle checks remain generic and still run before physical effects.

Two source coordinators preserve the existing source behavior:
`settle_execution_selected_with_lifecycle` and
`settle_reversal_with_lifecycle_v1`. They share the native stage and quote,
prepare this execution's close rows once, validate source observations before
effects, and use the same physical/financial commit. Only after Applied do they
observe the newly committed slice identified by the Result. Historical and
range-end report rows are not observed again.

Source preflight first completes the full ordered intraday-PnL sum and checks
that it is finite. It then simulates consecutive-loss-day counters, using the
existing chart timezone and `dayofmonth * 100 + month` key. That day is captured
before effects, so the committed-row observer performs no timezone work. A loss
increments the count only on a different last-loss day; a win resets the count
without clearing the last-loss-day key; zero PnL leaves the count unchanged.
The observer updates source fields only and never applies cash or fees again.

Source intraday invalidity remains `InvalidAccounting`, before source or
financial close-counter overflow. Source day exhaustion retains
`"closed trade counter exhausted"` before later lifecycle/stream checks.
Generic cycle readiness still precedes source preflight. A Ready opening-only
source call also requires finite cached intraday PnL, despite having no close
rows; it performs no observer writes. Invalid and NoEffect stages return before
source preflight. These checks and observations are private to the synchronous
call and introduce no saved settlement plan or persistent state.

Source risk/halt evaluation stays at its existing fill-loop call sites after
the coordinator returns. Native admission remains its own direction, resulting
position and initial-margin gate; Pine entry/risk/quota rules stay in source
adapters. Physical positions and marked equity continue to follow actual lots,
while the existing script/C position view may remain frozen. Range-end reporting
can update report rows/equity-curve state without committing physical closes or
source-day observations. Existing source fields remain represented in hashes.

## Integration and limits

The kernel serves native execution and the source adapters' full, partial,
bound and percent closes, scratch fills, reversals, RAW orders and opening/add
paths. Callers resolve source scheduling, quantity grids, price and slot policy;
the owner applies the physical effects and accounting once. A separately matched
scratch fill remains its own execution. Reversal closing and opening share one
already-resolved price and one current ticket.
`compat::pine` suspension selection stays at the replacement caller. Transient
lifecycle effects consumed by the active context/selected/reversal seams name
exact pending identities, revisions and operations; `created_seq` 0 and
`Target{0,0}` are literal expected values.
Source selection may preview the upcoming lifecycle frame without consuming it
and must supply a literal operation payload. They are not stored, hashed, or
reusable execution authority. Empty effects leave other settlement callers
unchanged.

Native requests settle through one owner. Source-specific placement and
receipt facts remain in the adapter; they do not reintroduce a second pending
book or a source-conditioned settlement path. Native settlement does not
rewrite supplied window/barrier facts or install a callback/plan. Migrated
frozen transactions and the final short-seed crossing settle their close/open
effects in one native call.

The shared close builder now consumes historical entry costs. This changes
the former reconstruction that converted both commission legs at exit-time
FX, and the former reuse of a complete entry ticket at every partial close.
TradingView compatibility must be measured against unchanged references.
Pine sizing and range-end reporting still have separate conventions; the
native marked-equity helper does not replace all existing report fields.

This API is synchronous. It does not place queued orders, establish matching
or cancellation authority, reconcile external broker messages, deduplicate
replayed fills or accept a stored plan as execution authority. Callers own
those responsibilities and the current event context. The return status
distinguishes application, no effect and validation failures. Allocation or
lifecycle exceptions abort the owning run; callers must discard that failed
run rather than retry a partially committed execution in place. Strong
rollback on allocation failure is not promised.

L3b replaces the legacy ShortSeed role storage with a plan-derived public
projection over live native handles. Source quota, TV-money/day-loss policy,
and source-day observation remain adapter responsibilities, separated from the
native financial owner; existing executable state remains represented in ABI
projections and fingerprints.
