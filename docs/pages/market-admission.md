# Market admission {#market_admission}

Admission keeps the original command and placement observation immutable. Later
review and sizing receipts describe what happened to that command; they do not
rewrite the original quantity, configuration or qualification. The executable
pending quantity and existing settlement kernel remain authoritative for fills.

`admission::Draft` owns the original observation, its first normal review and the
latest sizing revision. `admission::Journal` stores actual command outcomes,
reviewed instructions and sizing causes. Entry, raw-order, cancel and cancel-all
are actual command types. A no-target cancel records no removed instruction.

The Pine adapter derives its historical qualification rules from these facts at
the existing review checkpoints. Four independently stored candidate booleans,
two disqualified-bar sets and the latest-rejected-bar scalar are removed. Their
old C mirror fields remain read-only projections. The surviving qualification
predicates and paired settlement rules remain Pine compatibility behavior;
this extraction does not claim generic financial correctness for them.

Original placement configuration is distinct from current execution settings.
An actual margin fill can revise resolved sizing without changing the original
observation. Original opening qualification is also distinct from the later
broker `OpeningReceipt` Check/Exempt decision, which belongs to a committed fill.

Three historical placement views are fixed when an order is placed and kept in
the adapter's placement snapshot (`projection_after_close`,
`projection_over_pyramiding`, `projection_opposite_market_predecessor`
pine_adapter.hpp:269-271):

- The prior-close view compares the original accepted-close
  quantity with the existing quantity tolerance. This describes a source-time
  close claim, including an immediate close; it does not ask whether a position
  is currently flat.
- The entry-capacity view compares original placement side,
  requested direction and held-entry count with the original configured cap.
  Later fills, cancellation, sizing revisions or cap changes cannot alter it.
- The opposite-market predecessor view is decided at placement from the
  same-bar commands, pending entries and live requests the adapter holds. Earlier opposite MARKET instructions
  on the same source bar count even if later canceled or removed. A canceled
  peer that was still physically resident at placement also counts.

The admission journal records `BookObservation.buy`, the raw instruction
direction of every physical peer; held position side cannot supply this fact,
especially for flat-born or manually constructed MARKET peers. A manual order
without an original admission observation retains the historical false default.
Native callers that construct orders directly must supply their real original
observation when these placement facts matter; a later executable quantity or
position snapshot is not a substitute. The legacy C mirror columns remain at
their original offsets as derived outputs, alongside the original observation.

## Causal receipts

An event sequence is allocated once and consumed by a successful journal append.
Appending nested events out of allocation order is supported; reclaiming a record
does not make its sequence available again. An unfinished allocation is abandoned
by its call frame, and reset refuses outstanding operations.

A per-order review or sizing receipt names the original command. Its event must
follow that command and its bar cannot precede the placement bar. Review progress
is the first actual receipt, with identical replay allowed and conflicting
replacement refused. Sizing revisions similarly preserve the causal order of
their events, bars and committed fill causes. A batch review can issue a receipt
only for an original draft captured by that review.

## Reflection and retention

The pending-order C mirror preserves its prior prefix and appends actual
per-order observations and receipts. Its copier uses direct typed reads and
string views, with no dynamic allocation. A caller uses the returned field
descriptors, presence fields and struct size. Variable journal contents have
an internal typed visitor, shared with the broker fingerprint, including the
ordered arrays, optional presence and event discriminants.

Retention follows open source windows, live instructions and referenced causes.
It has no arbitrary last-N truncation. This currently retains a full prior-book
observation for each live command: N simultaneous resting orders can retain
N(N-1)/2 prior-book rows. This cost still needs a separate representation change;
the journal is not a fixed-memory or durable venue-event store.

The C pending-order accessor copies the predecessor column from the placement
snapshot, at its original offset.

These identities belong to one engine run. Reset begins a new run; callers must
not submit persisted receipts from a different engine or run. External broker
reconciliation requires a separate durable identity contract.
