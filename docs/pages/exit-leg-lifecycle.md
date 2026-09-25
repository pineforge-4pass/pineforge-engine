# Exit-leg lifecycle {#exit_leg_lifecycle}

This model changes the internal C++ object layout. Consumers must rebuild against
the matching engine headers and archive; old v7 objects are incompatible.
Public C ABI 4 and stream API 1 stay unchanged. The full 155-field pending-mirror
prefix is preserved; 163 canonical lifecycle fields follow it, with admission
facts in a separate append segment.
See [reflection and completion](exit-leg-lifecycle-reflection.md).

Native request definitions own trigger state. The source adapter retains an
immutable placement-level snapshot for public projections, so no compatibility
order object or second executable price store remains. ENTRY/RAW trigger
calculations and quantities are unchanged. A retained definition handle can be
created only by its canonical owner, and retains old values when a successor
replaces its current definition. Deferred closes acquire an incarnation before
lifecycle attachment.

The native reducer in `exit_leg_lifecycle.hpp` accepts exact target incarnation,
owner and revision, a cause frame, and a typed action: bind owner, suspend selected
legs, stage replacement, cancel deferred activation, restore generations, complete
an explicit barrier, fold an observation or retire working legs. A retired
working generation cannot execute; restoration advances its generation. Stop,
limit and trail availability are queried independently of their trigger presence
and existing activation readiness. Suspension of one leg grants no other leg's
permission.

`compat::pine::exit_lifecycle` selects the existing suspension, pair hold and
predecessor-stop policy. It preserves legacy bar-only/cycle-ID tests without
claiming the old cause proves ownership of the new target. Native validation
uses the actual current target. Repeated decline does not clear a retirement.
Queue predecessor and selected revival-definition predecessor are distinct.
Pair hold cancels scheduled release while retaining old-stop evidence; a later
replacement follows the established current-versus-retained selection rule.

The selected old stop participates only in the immediate margin-event comparison,
even if the successor lacks a stop. Successor prices remain afterward. Revival
activates all qualifying definitions before the independent readiness check;
the already-committed risk action is unaffected by an unready user leg. Existing
vector-first, whole-survivor, attribution and OCA behavior stays on its current
direct execution path. No agenda dispatch or financial-accounting change is added.

One observation window owns its current and pre-observation extrema. It omits the
selected decline observation in the reducer itself, keeps disabled-trail observations for historical
mirror/hash output, and preserves existing shared-anchor restart/fallback policy.
It is the sole mutable window, not a duplicate mirror book. Hold/replacement
barriers retain their requesting event/domain; completion identifies that exact
barrier and an actual after-margin hook. Completion cannot follow the action's
processing receipt; separate events caused by an owner bind retain that hook's
phase. Ordinary, fill-recalculation (`Domain::FillRecalc`, the host's
re-entry pass after a fill — spelled `Domain::Coof`, for TradingView's
`calc_on_order_fills`, before 1.0) and both magnifier hooks
(`Domain::Magnifier`, `Domain::MagnifierFillRecalc`, the latter spelled
`Domain::MagnifierCoof` before 1.0) remain explicit. Raw-tick bar advancement supplies no such completion.

## Bounded replay contract

Instruction identities are non-reused within an engine run; action events advance
independently of broker fills. The reducer validates structure/target/revision
before committing a copied next state. Rejected actions leave it unchanged. It
retains **one latest accepted action payload**, sufficient for exact bitwise
payload replay comparison (the receipt hash retains raw binary64 bits despite
the broker's usual NaN/zero canonicalization): an immediate exact duplicate is a no-op; conflicting
reuse is refused; older event identities are expired. Definition changes also
invalidate prior action revisions. A bind that changed owner causes an old-owner
replay to be refused, never applied twice.

There is no unbounded receipt map or restoration of arbitrary prior results.
These in-process action handles belong to the current run/object lineage. Engine
reset destroys that lineage and restarts deterministic generators; actions must
not be submitted across reset, persisted as venue messages or applied to a new
engine that reuses numeric IDs. A durable cross-run ingress is outside this
component and would require explicit externally supplied run identity. Copies
preserve canonical facts for deterministic continuation; fresh quantity-binding
forks receive a new instruction identity and no inherited replay receipt.

All nested definition, generation, cause, obligation, window and latest-receipt
facts are reflected/hashed once from canonical state. Legacy D/R/T/O/H/K/B/B0
fields are read-only projections. This model removes three stored booleans.
Admission, cancellation, and placement facts are represented by the native
request/receipt model and adapter journal. The retired compatibility-order
boolean census is no longer maintained.
Optional obligations, three leg generations,
retirement receipts, immutable definition references and action/domain variants
remain disclosed domain state. This is not a whole-engine fewer-than-five claim.
