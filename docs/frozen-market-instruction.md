# Frozen Pine market instruction

This is a bounded representation migration of an existing Pine compatibility
policy. The adapter placement snapshot represents a frozen market instruction
as one exclusive source operation. It does not make
the existing cohort selector a generic native execution contract.

## Source operation and state ownership

| Role | Live owned facts | Existing facts consumed |
| --- | --- | --- |
| Ordinary | None | Ordinary sizing and execution state |
| Transaction | Own units; frozen total transaction units | Requested side; immutable placement-cap snapshot |
| TargetedClose | Explicit target entry ID | Original QuantityRequest Units; immutable created-position side |

Only complete role construction and whole-operation revocation are supported.
Transaction and TargetedClose are mutually exclusive; there are no independent
membership, cap-retention or close-side switches. Revocation destroys the active
payload. A targeted close has no second quantity authority: later reservation
changes leave QuantityRequest's original Units amount intact.

A source transaction captures the same expression as before:

    own + opposite position held net of earlier same-bar closes
        + opposite pending entries' own units

Its retained-over-cap property is true only when the Transaction role is active
and `over_pyramiding_cap_at_placement` is true. The former duplicate calculation
and the placement snapshot use the same position-side/count/cap expression;
none of those inputs changes between the two capture sites. A targeted close's
buy direction is its captured position side being SHORT.

The own amount must be positive and finite. A positive-infinite total caused
by source-sum overflow remains representable because the prior source capture
allowed it. This is not a validated native execution amount. The existing
finite-total execution guards still exclude it from frozen transaction sizing;
ordinary sizing follows the same prior path. No new overflow admission policy
is introduced by this structural change.

## Compatibility boundary retained

The selector, full-book admission and execution scopes are unchanged: historical
close calculation; FIXED sizing; default FIFO; pyramiding at most one; no costs,
slippage, magnifier, stream or risk extensions. Exact source conditions remain
in `same_bar_market_tx_scope_is_live` and its command producers.

The complete pending book must contain only accepted operations and at most
two MARKET entries with distinct IDs. An unrelated priced/raw/bracket/close-all
order, a third market entry, or leaving the scope revokes the source operations
for the whole book. Subsequent native dispatch uses the prior ordinary paths.
Same-ID replacement and named cancellation/recreation construct a fresh
operation with a new incarnation and current source snapshots.

TargetedClose is intentionally not named ReducePosition. Existing Pine behavior
can materialize an opening artifact lot if the originally targeted side is gone
and a matching entry remains pending later in the source execution order. That
path remains intact. Lowering this source operation into separate generic
native transaction/reduction instructions remains future adapter work.

L3b removes the retired compatibility-order booleans. The Pine cohort selector
and source-operation discriminator remain adapter facts and must remain in
compatibility audits.
No Pine selection predicate has been moved into the native contract by renaming.

## Observation and compatibility

The existing 142-field public pending-order mirror is append-only. Every old
field retains its type, size and offset, including the old full-prefix trailing
padding. The six old `sbmt_*` outputs are derived projections only; they are not
writable engine state. Ordinary/revoked operations project false flags and NaN
legacy quantity values exactly as the old inactive sidecars did.

Six fields append the source kind, own/total units and target ID (char[64],
truncation indicator and full string hash). The broker hash folds the kind and
only its live payload; QuantityRequest and placement facts are already folded
at their owning order. Metadata mutation checks refuse hidden nested fields,
changed variant/enum alternatives, removed or conditional folds, and waivers.
The representation is now adapter-owned over native requests. The frozen public
mirror layout remains append-only and is projected from those facts.
Public C ABI4 and stream API1 are unchanged.

`test_frozen_market_instruction` uses literal price-100 fixtures and direct
native command transitions. It pins role construction, amounts in both
orientations, quantity ownership, whole-book revocation, replacement,
cancellation, hash sensitivity, string truncation and the full public prefix.
It does not use external tapes, expected provider trades or a grader.
