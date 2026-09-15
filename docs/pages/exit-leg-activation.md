Exit stop and limit matching now consumes concrete activation bounds attached
to a position-cycle identity. Each bound names the earliest script bar where
that leg may be evaluated for that owner. A mismatch cannot reuse another
cycle's constraint; an unbound value imposes no lower-bound constraint.
This is one exposure-cycle scope, not a physical lot or `from_entry` claim.
Existing quantity/entry ownership and other path/trailing rules remain separate.

The lower bounds also apply when a dormant bracket is revived after a margin
partial and when a prearmed bracket selects a stop or limit at the open.
Revival can restore the bracket without executing an unready stop. Open-gap
selection checks each leg before applying stop/limit precedence, so an eligible
sibling retains its own execution and slippage semantics. Risk liquidation
itself is independent of a bracket's activation time.
The historical chart-extreme rounding fallback checks the same per-leg bounds;
a normal no-fill result cannot become an early fill through that fallback.

The transitions are connected to the current book. New or replaced EXIT
construction binds against live exposure. Both fresh-position producers bind
retained exits after establishing the new cycle, before later matching or
callbacks. Going flat clears bindings while retained policy evidence stays
with the order. Partial reductions and same-cycle adds do not refresh a
deadline. Order replacement creates fresh placement evidence and activation;
cancellation and OCA erasure remove the values with their owning order.

For Pine compatibility, immutable `ExitPlacementEvidence` retains the original
callback marketability inputs: placement cycle/entry bar, physical exposure
direction, cursor price, raw stop/limit levels and any named limit continuation.
Current trigger fields can be neutralized or modified without rewriting those
facts. `LaterSameOpen` and `FirstHighRecross` continuations remain explicit
Pine exceptions; their existing predicates are evaluated in the adapter.
Their recorded observed fill sequence is the sequence visible at policy
selection; the separate `OrderBirth` still owns the actual callback interval.

At binding, the adapter resolves the retained original decision to concrete
bounds for the new owner. A held leg receives `owner_entry_bar + 1`; the other
leg receives `owner_entry_bar`. The arithmetic uses a wider bar coordinate.
The matcher never refreshes these numbers from the current position. In
particular a surviving same-ID exit can bind a new cycle and receive another
hold, even though its original birth-bar deadline has already passed. New
marketability is not recalculated from the replacement owner's opposite side.

The two native `coof_suppress_*_on_entry_bar` booleans are removed. Their old
C mirror fields are read-only projections from the Pine evidence; no native
decision reads those outputs. All 128 pre-existing mirror fields and the full
old byte prefix remain unchanged, and new activation/evidence fields append.
Every new owner, coordinate, evidence value and optional-presence discriminator
is hashed and mirrored. There is no side map or second pending-order book.

This slice moves the two decisions out of the matching masks, but does not
eliminate the Pine policies or detach their producer from existing native
callback use. The Pine adapter is still invoked by the current exit producer.
`PineHistoricalBirthReach`, cascade leg/gap permissions, dormant-leg revival and
other historical execution rules remain separate work. The effective-level
accessor still reports resolved levels rather than masking eligibility.

Historical internal object layouts require matching C++ headers/library. L3b
removes the compatibility order object and projects the frozen pending-row ABI
from native requests plus adapter facts. Public C ABI 4, stream API 1, and
pending mirror version 1 are preserved.
