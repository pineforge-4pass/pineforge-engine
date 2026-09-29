# Exit lifecycle reflection and completion {#exit_lifecycle_reflection}

The existing `strategy_pending_order_layout` / size-limited pending-order C API
exposes named POD fields. Its first 155 descriptors, types, offsets and full
1272-byte prefix are preserved; 163 canonical lifecycle fields append, followed
by the admission model's facts. Query the layout or use
`PF_PENDING_ORDER_FIELD_COUNT` for the complete current field count. No new runtime
export or public C ABI/schema version is introduced. C++ objects built against an earlier epoch remain
incompatible and must be rebuilt against matching headers and archive.

The `legs_` fields cover target identity/owner, current lifecycle and definition
revisions, immutable definition presence/prices, three working generations and
retirement receipts, suspension and its ordered leg list, exact hold/replacement
barriers, selected predecessor definition, observation window, and the latest
replay action including all variant payloads. Lists are bounded to 3 and retain
count and order. Latest-action history is bounded to one receipt.

A reader uses each optional `_present` flag and `legs_last_operation` (the native
Operation declaration order 0–7) before interpreting descendants. Inactive variant
or absent-optional descendants have no semantic value: zero/NaN/sentinel storage
there is not a substitute for a present value. Unused list slots are UINT32_MAX;
only indices below the actual count are meaningful. Present doubles preserve
actual value bits, including a replay payload's NaN payload and signed zero.
Definition value presence distinguishes an absent definition from present prices
whose specific leg is unset/NaN. Use the supplied field descriptors and struct size
rather than assuming the enlarged object fits an old buffer.

`scripts/pending_intent_view.json` records the projection source of every
mirror field; `scripts/test_pending_intent_view.py` (CTest
`test_pending_intent_view_schema`) checks all 406 projections and their
approved provenance, and `scripts/test_pending_intent_view_mutations.py`
holds that the check fails on a mutated projection.

A barrier's identity is its issued instruction/owner/revision binding plus its
requested event/domain/bar/phase. The action's current target/revision and actual
completion cause are separate. Completion must name the exact outstanding
barrier; absence is not a broad release signal. Same-domain completion cannot
precede the declared request coordinate `(bar, phase)` or its event ordering.
Different domains have no implicit coordinate conversion; an explicit caller
selects the actual old target, and its completion event remains causally later.

`Action.cause` records processing of the action. `CompleteBarrier.completed`
records occurrence of the completion being processed. An intervening owner bind
can make these distinct events. The request must not follow either event, and
completion must not follow processing: every pair obeys event ordering and,
within the same domain, `(bar, phase)` ordering. A cross-domain completion cannot
hide a contradiction between a request and processing in the same domain. When
Pine needs a fresh receipt at an after-margin hook, rebinding preserves that hook
phase; it does not record the completion as processed in the earlier observation
phase.

The native Observe action excludes its window's entire originating bar and
earlier bars within that domain. Its causal event must also follow the excluded
event. Rejection preserves the extrema, revision and replay receipt. Observations
in another domain require explicit caller selection, with causal event ordering
still enforced. Pine retains its legacy bar-only filter across modes before
issuing the operation; the native rule does not infer a cross-domain conversion.

Only one outstanding barrier is supported. Staging a replacement while a hold
or another replacement exists is rejected with no mutation. Completing a hold
clears only that hold; it does not restore legs. Completing the sole replacement
fulfills its declared restoration. A new explicit Suspend may supersede an
old episode as before; that is not a completion of an unrelated obligation.

The generic native operation has no RawTicks prohibition or implicit after-margin
permission. Pine's `select_exit_completion` issues targeted completions only at
its existing historical after-margin hooks and selects an actual old obligation
across modes when needed. No raw-tick bar advancement creates a completion.
Malformed target/coordinate negatives are native API controls, not assertions
that ordinary production hooks emit them.

Conditional EXIT definitions remain conditional when all defined triggers are
unavailable: they yield NoFill, never a market reduction. A genuinely unpriced
close remains an unpriced operation. Per-leg lifecycle availability is checked
in normal resolution, prearmed gaps, chart-extreme fallback and the fixed-only
path metric. The fixed-only helper still excludes any trailing definition; the
broader mixed-leg comparator/agenda repair is separate. Lifecycle leg actions in
this integration apply to EXIT triggers; ENTRY/RAW trigger-policy redesign and
whole-instruction cancellation semantics are not introduced here.
