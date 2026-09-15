# Pending-order placement and replacement facts

The adapter placement snapshot records the immediate live predecessor whose
priority slot the newly accepted native request retains. Zero means fresh
construction. The new request still receives its own fresh `incarnation`;
`created_seq` remains its scheduling priority, not identity.

The receipt is populated by high-level MARKET/ENTRY, RAW, and primary EXIT
replacement. Named cancel followed by recreation is fresh; its separate
cancel/recreate receipt does not become a replacement. When a Pine exit
reissue materializes multiple legs, the primary leg inherits the preceding
primary's priority and predecessor; additional legs are fresh. This receipt
does not claim to enumerate all sibling objects erased by that reissue.

The former native `created_by_same_id_replacement` Boolean and redundant
`replaced_exit_order_incarnation` scalar are removed. Replacement readers use
the authoritative predecessor. The conditional
`replaced_default_market_incarnation` remains a Pine qualification receipt:
it records additional predecessor kind, sizing, side, source-bar and cycle
conditions that generic replacement identity alone cannot establish later.

The former `created_while_in_position` Boolean is also removed. Its production
meaning was EXIT-only: `strategy.exit` derived it from the same `effectively_flat`
calculation used for `created_position_side`; a positive deferred close used the
nonflat side it targeted. EXIT consumers now read that existing placement side.
The two non-EXIT checks were vacuous because their producers always left the
old Boolean false; those checks are removed without requiring flat placement.

`created_position_side` is not renamed or reinterpreted as a universal physical
snapshot. MARKET/ENTRY/RAW capture physical exposure; a Pine EXIT captures
exposure after earlier same-evaluation close claims. The physical position can
still be open when such an EXIT captures FLAT. Existing cycle/carry fields and
their scopes are unchanged. A complete physical placement/close-claim model is
separate work.

The public size-aware `pf_pending_order_v1_t` retains every existing field at
its original offset. That is a layout guarantee, not a claim that every value
is a permanent constant or that the retired `PendingOrder` remains executable.
`PendingIntentView` projects each row from live native request facts, adapter
placement facts, and terminal receipts. Dynamic-layout readers can discover
appended fields while older prefix readers retain their layout. Mirror-fidelity
twins check those value projections separately from the immutable v1 prefix.

The retired compatibility-order Boolean census is no longer a runtime design
surface. Native request definitions, adapter placement snapshots, and terminal
receipts carry the corresponding causal facts. Public C ABI version 4 and the
pending mirror v1 layout remain unchanged; value fidelity is enforced by the
native-route mirror tests rather than asserted by this document.
