# Order execution model {#fill_model}

PineForge is being developed as an independent C++ backtest and forward-execution
engine. Its native contracts describe orders, exposure, reservations and actual
execution events. Pine source interpretation belongs to the frontend boundary.
TradingView comparisons are compatibility evidence for a declared configuration;
they do not define every native operation.

The migration is incomplete. The current engine still contains Pine-specific
admission and scheduling rules, and the ordinary bar scan and callback-driven
scan are separate. The responsibilities below guide their consolidation without
claiming that a single unified scheduler already exists.

The current pending-order representation has five direct boolean members. Two
duplicate placement fields derive from the original command observation;
the opposite-market predecessor fact derives from its accepted command's
original book and removal records. The book records each instruction's raw
buy/sell direction, including manually constructed peers, without storing the
derived predecessor result. Cancellation has an explicit terminal receipt. This declaration count is
an intermediate result: it does not prove that all remaining Pine predicates
are generic, or that the whole engine has fewer than five compatibility choices.

## Responsibilities

| Submodel | Owned facts and transitions | Boundary |
|---|---|---|
| Command admission | Original quantity request, placement observation, accepted instruction identity, rejection cause | A source frontend interprets its calls; native admission operates on explicit transaction or position intent. |
| Exposure and reservation | Position cycle, physical quantity, reservation owner, admitted growth sources, committed growth and retirement | A proposed add is not an executed add. There is one mutable reservation capacity. |
| Trigger lifecycle | Stop/limit/trail definitions, activation bounds and owner binding, stop-limit activation and trail progress | An unready leg cannot supply a price or authorize a ready sibling. |
| Execution schedule | Candidate identity, event coordinate, dependency and explicit priority | Ordering does not grant admission, create quantity or activate a leg. |
| Settlement and observation | Executed quantity, physical lots, paid fees, OCA effects, risk follow-up and callbacks | Publish observations after the corresponding state change; report projections must not change admission equity. |

These are responsibilities, not five independently switchable modes. Direction,
order kind, quantity intent and an outstanding owned claim are normal domain
state. Encoding unrelated permissions as enum values or moving them to another
object does not simplify the model.

## What qualifies as a generic flag

A retained flag must represent a defined user setting or a causal execution
fact with an owner, a producer and an expiry rule. Examples are a requested
execution mode or a stop leg that actually activated. Multiple overlapping
facts describing one lifecycle need explicit states and transitions instead.
An experimental interpretation switch is not a user setting merely because
it can be passed through metadata.

Review both the fact and its consumers. A generic fact such as "market fill
at the open" does not justify every financial exception that reads it. Each
consumer must have an economic, temporal or ownership explanation and a
counterfactual that can refute it independently of the originating strategy.
Changing the strategy's name, adding an unreachable order, splitting physical
lots or varying price scale can reveal a condition that encodes a narrow
example rather than the proposed rule; the comparison must hold the relevant
economics and information constant.

In particular, a conjunction of direction, commission, sizing and book shape
does not become generic by being renamed as an enum or captured as a receipt.
The opening extraction below fixes ownership; the surviving financial
eligibility predicates still require that separate review. Existing parity
scores are regression evidence, not a justification for keeping a compensating
flag. Actual conflicting TradingView observations belong in an anomaly review
record, separate from an unknown rule or an engine defect.

## Identity and quantity

Every accepted pending object receives a fresh `incarnation`. A user-supplied ID
can be reused, and same-ID replacement can retain queue priority; neither implies
that the new object owns the old object's claims. `replaced_order_incarnation`
records the exact predecessor. A position cycle similarly distinguishes two
positions that happen to have the same direction.

The adapter placement snapshot retains the original Units, Fraction, or All
request basis. Native working quantity can change through group reduction or
committed reserved growth. That does not rewrite the original request,
reclassify its historical partial/full meaning, or create another
quantity ledger.

An expansion capture belongs to an exact EXIT object and exposure cycle. Selected
source orders carry the receiving EXIT incarnation. After an actual primary fill,
the receiver gains only the positive same-side quantity increase belonging to its
captured cycle. A canceled or logically retired receiver cannot redirect that
growth to another exit with the same user ID.

The first later successful entry-like admission closes the capture's population.
Canceling that later admission does not erase its historical cause. Previously
captured sources can still pay their exact receiver; later unrelated adds do not
join the capture. An old-cycle capture loses live-All authority while its ordinary
finite reservation remains available for the normal settlement path.

### Native quantity actions

`order_action::plan` in `<pineforge/order_action.hpp>` resolves physical units
against a supplied signed position. `Reduce{units}` accepts a finite,
nonnegative magnitude, caps it at the held exposure and never opens or flips.
`Transact{signed_units}` closes opposing exposure first and opens only its
remaining units. For example, selling 2, 3 or 5 units against a long position
of 3 leaves long 1, flat or short 2, respectively. A zero request is valid.
Nonfinite values, overflow and a nonzero request lost to binary64 rounding
produce no plan.

The plan is immutable arithmetic output. It owns no position ledger, source
policy, quantity step, cycle counter or execution permission. It does not
authorize a delayed fill: a scheduler must revalidate its order identity and
current book before settlement. Pending cancellation and replay receipts are
separate lifecycle contracts.

The native [resolved settlement extension](../native-settlement.md) applies
`Flatten`, `Reduce` and `Transact` to the existing physical FIFO book, including
paid entry costs, current execution charges and ordered observations. Full
market exits and selected frozen-transaction/materialization paths use it.
The native request core owns matching; the source adapter owns source policy,
admission, slippage, and scheduling. Short-seed readback is now a
plan-derived `0/1/2/3` projection rather than a compatibility enum or stored
role. This step does not reduce the public pending-row ABI or
provide broker-account reconciliation.

## Opening checkpoint

An accepted opening or add can create an opening-affordability checkpoint.
`broker::OpeningReceipt` carries its producing broker fill, source order
incarnation, position cycle, bar and timestamp, and the raw matched price.
The booked entry price is separate: the financial check reads the current
position book and uses the raw price only where the execution policy requires
it. It does not reconstruct ownership from FIFO rows or entry names.

The live states are absent, pending check, and pending exemption. A check can
carry a remaining-adverse-path continuation; an exemption cannot. The
decision is made by the successful-fill policy when it creates the receipt.
There is no later transition that promotes an exemption into eligibility.

| Operation | Transition |
|---|---|
| Qualifying successful opening/add | Replace with the new producer's check or exemption |
| Rejected or zero-effect attempt | Preserve the pending receipt |
| Accepted incompatible short opening/add | Invalidate the prior receipt |
| Full close, reversal, fresh cycle, run reset | Invalidate prior-position ownership |
| Ordinary margin checkpoint | Take the receipt before any early return or recursive check; reject a stale position-cycle owner |
| Checkpoint before a priced exit | Inspect without consuming; after an actual margin slice, consume only the same producer's receipt |

This is one coalescing checkpoint over the aggregate book. It is not a queue
that retroactively settles every individual fill. A later qualifying add
replaces the raw price and producer; a no-op cannot replace them. A partial
FIFO close may leave the position cycle alive even if it drains the original
physical lot, so the receipt owns the aggregate cycle rather than requiring
that original lot to survive.

Disabled margin, invalid financial data, exemption and no shortfall can all
consume the ordinary checkpoint without generating a trade. A scoped short
check may then visit the remaining adverse path once using its detached
receipt. The pre-priced-exit consumer retains its different existing contract:
a no-action inspection leaves the receipt pending for the ordinary check.

Four historical long/short lifecycle labels had no economic consumers. They
have been removed, along with their producers; the numerical floor-zero rules
and their trade fixtures remain. Those labels are not alternate model states.

## Current dispatch sequence

Admission records the original command and its placement configuration separately
from subsequent review and sizing receipts. The [admission model](@ref market_admission)
describes their ownership and retention. Pine qualification predicates remain in
the explicit adapter; recording a predicate's inputs does not establish that the
predicate is a generic financial rule.

The [trigger lifecycle](@ref exit_leg_lifecycle) owns immutable price definitions,
working generations and exact suspension/replacement obligations. An unavailable
stop, limit or trail cannot turn a conditional exit into an unpriced market close.
Completion identifies the outstanding obligation and its occurrence separately
from the processing receipt, with causal ordering checked before any mutation.

The ordinary pending-order scan updates risk state, processes due opening work,
finalizes source cohorts, updates trailing/relative prices and orders the book.
It then classifies and matches an exact pending handle. A pre-exit margin slice
can change the book between matching and dispatch, so the handle is resolved
again before applying the selected order.

The primary fill updates physical exposure. Reservation growth is settled at the
existing post-primary checkpoint, before that order's OCA and risk follow-up.
Logically retired objects cannot be dispatched again while awaiting compaction.
Callbacks use committed fill events; a declined or zero-effect attempt is not
itself a fill event. Pine's optional quota interpretation separately observes
the attempt stages its own contract specifies.

There is no universal `risk > exit > entry` priority. A forced action and a user
order have event coordinates, and an earlier event must be accounted before a
later one. Forced liquidation cannot be suppressed by an unready user stop.
An earlier risk fill can also invalidate the quantity or owner of a later
candidate, requiring another resolution of its identity and eligibility.

## Distinct execution domains

An opening receipt is broker-local. It is not an identity for every output:

- A physical fill may produce several FIFO trade rows.
- A COOF notification may group several resting-order fills.
- A stream action is an observer projection with its own delivery sequence.
- A range-end report row marks an open position for reporting; it does not
  close the broker book or create a stream action.
- Pine variables and source-series history have separate speculative and
  committed states. A Pine rollback does not roll back committed broker fills.

Historical OHLC, magnifier, confirmed-bar and actual-tick inputs expose
different information. The opening ownership extraction changes none of their
path, callback or warmup policies. Terminal-close deferral is still selected
by the existing financial policy; it has not been generalized by this model.

## Remaining consolidation

The complete scheduler should build immutable candidates, choose the earliest
eligible event coordinate, satisfy causal dependencies, and then apply explicit
priority and stable submission identity. Losing candidate scans must not commit
trigger activation. Dependency cycles must be reported rather than hidden by
another pairwise preference.

The existing two-sort arrangement does not yet provide that contract. In
particular, the sibling comparator has reproduced ordering-law failures for
unrelated interleavings and mixed trailing/non-trailing exits. Grouping by owner
alone does not repair the mixed-leg case. Replacing these sorts is separate from
the reservation-ownership change and requires its own native ordering tests and
Cloud compatibility assessment.

A native reduction must never create new exposure. Some existing Pine close
interpretations can produce a new transaction after their old target disappears;
that interpretation must remain explicit at the source boundary until lowering
to native operations is complete. The current engine also has separate forward
ingress, callback, affordability and per-leg lifecycle work. This page does not
claim those migrations or the large-function cleanup are finished.

## Verification and compatibility

Literal state tests exercise consume-before-use, exempt presence, replacement,
stale-owner rejection, no-op preservation and independent copies. Existing
trade fixtures retain their financial expectations. Cloud comparison of the
fixed reference population is required before reporting parity preservation.
Hashes supplement those comparisons; a matching fingerprint is not a proof
that all hidden strategy state is equal.

Internal C++ layouts and broker/stream fingerprint domains are versioned separately
from the public C ABI and its append-only pending-order mirror. Rebuild generated
and native modules against matching headers and runtime. See
[ABI stability](@ref abi_stability) for the current versions and stale-object
pairing checks. A lower Boolean field count is not proof that every compatibility
policy or possible order history has been covered.
