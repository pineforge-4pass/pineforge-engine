# Native resolved execution settlement

`BacktestEngine::settle_resolved_execution` is a protected C++ extension for
trusted matching adapters. It applies an already admitted, matched execution
to the engine's existing physical lot book and emits the resulting close and
open observations. It adds no second position ledger or persistent selector.
Callers that supply transient EXIT lifecycle effects use the separately named
`settle_execution_with_lifecycle` seam; the two-argument symbol is unchanged
and forwards empty effects.

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
epsilon silently discards it. `Flatten` explicitly closes all lots and avoids
using rounded aggregate equality to mean a whole-book close. Quantities whose
changes cannot be represented are refused before settlement.

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
a close-only projection's equity before resolving one reversal transaction.

## Integration and limits

The kernel serves native execution and the source adapters' full, partial,
bound and percent closes, scratch fills, reversals, RAW orders and opening/add
paths. Callers resolve source scheduling, quantity grids, price and slot policy;
the owner applies the physical effects and accounting once. A separately matched
scratch fill remains its own execution. Reversal closing and opening share one
already-resolved price and one current ticket.
`compat::pine` suspension selection stays at the
replacement caller. `settle_resolved_execution` remains the original
two-argument symbol and forwards empty effects to
`settle_execution_with_lifecycle`, the protected seam that consumes transient
lifecycle effects. Those effects name exact pending identities, revisions and
operations; `created_seq` 0 and `Target{0,0}` are literal expected values.
Source selection may preview the upcoming lifecycle frame without consuming it
and must supply a literal operation payload. They are not stored, hashed, or
reusable execution authority. Empty effects leave other settlement callers
unchanged.

Authorized pre-close events run first, then close observations and the existing
flat unbind, then the listed pending removals, then `open_quoted_position`,
which still binds only remaining exits. False removal lists do not replace or
reallocate `pending_orders_`. Native settlement does not recognize source
cases, rewrite supplied window/barrier facts, or install a callback/plan.
Migrated frozen transactions and the
final short-seed crossing settle their close/open effects in one native call.
Production fill paths no longer use the old per-row close loops as a separate
accounting owner. Historical private helpers remain for source compatibility and
tests; they are not an alternate production execution path.

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

The work retains `ShortSeedCollisionRole` while its source-policy consumers
remain. Admission, source day/quota counters, script-visible observations and
complete Pine lowering remain separate refactor work. Existing executable state
remains represented in ABI projections and fingerprints.
