# ScratchStore, ScratchReadback and ScratchBacking

This page states the shared contract for per-run state that must not grow with
the run: an exact external record store (`ScratchStore`), the interface every
consumer reads it through (`ScratchReadback`), and the boundary to the medium
that holds its pages (`ScratchBacking`). The declarations are
`include/pineforge/detail/scratch_store.hpp`,
`include/pineforge/detail/scratch_readback.hpp` and
`include/pineforge/detail/scratch_backing.hpp`. Where a header comment and this
page differ, the header is right and this page has a defect. The fixtures that
hold the contract are `tests/test_scratch_backing.cpp`,
`tests/test_scratch_store.cpp` and `tests/test_scratch_contract_guard.py`.

The contract changes no engine behaviour, no source policy, no field of the
D3.7 sink and no report schema. A change to the three headers after the first
publication is additive, or it is announced to every consumer lane before it
lands.

## Ownership

The host that starts a run owns the medium. It creates one `ScratchBacking`,
chooses its kind explicitly (`disk`, `memfd`, `tmpfs` or `test_memory`) and its
page size, provisions its extent, and decides what the I/O observer charges the
traffic to. Nothing in this API reads `TMPDIR`, opens a file or falls back to
another medium: the kind is a constructor argument of every backing and appears
in every I/O charge.

A `ScratchStore` borrows the backing and the observer context for its whole
life. One store holds a backing at a time; a second store on the same backing
is a reopen after the first one ended. Destroying a store never commits.

Nothing a backing writes outlives the backing object. A disk or tmpfs file is
unlinked no later than the backing's destruction, and is created unlinked where
the OS allows; a memfd closes with the backing. No other object or process
reopens the file. A reopen means a new `ScratchStore` on the same live backing
object: it does not replay the journal. Consumers must not assume persistence
across backing objects or processes, or crash recovery. If the backing refuses
the rollback of a destroyed store, its contents are unspecified and the host
discards it. Per-run commits use `buffered`.

Callers own every source, buffer, visitor context and page they pass in. The
store keeps none of them after the call returns, and it never hands out a
pointer into its cache. The store itself owns only the fixed workspace it
reserves in its constructor, sized by the config alone.

Calls are serialized: there is no concurrent use of one store.

## Records, handles and ordering

A record is a key and a value, each a byte string of any length including
zero. The caller's comparator defines both identity and order. A `put` whose key
is equal to a live record's key replaces that record's key bytes and value
bytes and keeps its handle; any other `put` adds a record. The comparator
reads whole keys through bounded reads of a `ScratchByteSource`, so a key can be
larger than a page. It must be a deterministic strict weak ordering, and it must
not throw, allocate in proportion to the population, touch the store, retain
its sources, depend on addresses or consult mutable external state. The order
id in `ScratchKeyOrder` is a nonzero schema version: reopening under another id
is refused.

A handle is a 64-bit nonzero number. The first record of a created store gets
1, each further new record the next number, an upsert keeps its number, and an
erased number is never issued again. Handles survive page eviction, commit and
reopen. The one way a number is issued twice is rollback: it restores the
allocation counter, so numbers first handed out inside an aborted transaction
are issued again, and everything obtained inside it must be discarded.

`for_each` visits live records in ascending comparator order. `first`, `next`
and `lower_bound` are the same order by seek and keep no resident cursor: a
cursor is a handle, and `next` finds the immediate successor of a live handle.
`lower_bound` is inclusive of equivalence. The same operation sequence yields
the same order and the same digests under any page size, cache size, journal
and index slot count, and transaction batching.

## Fixed memory

Only fixed cache, journal and index slots reside in userspace, sized by
`ScratchStoreConfig` when the store is built. No page directory, free list, undo
vector, dirty map, traversal stack or key cache grows with the number of
records, the number of operations, the number of bytes or the length of a
transaction, and nothing allocates after construction. Recursion over the store
therefore needs caller-supplied fixed buffers: the store cannot keep per-read or
per-visitor state in a container. `ScratchStore::stats` reports the fixed
workspace (`resident_bytes`, constant for the life of the object), the backing
extent and the number of backing calls the store issued; it exists so a growth
gate and an I/O audit can read these without an implementation detail.

Supported geometry is exactly: at least two cache pages, a power-of-two page
size of at least 512 bytes that equals the backing's page size, at least one
journal slot and at least one index slot, a nonzero order id and a comparator.
A definition may bound the page size and the cache from above and must say so;
the reference definition refuses a page size above 1 MiB or a cache above 16 Mi
pages. Anything else leaves `status()` at `programming_error` and performs no I/O.
Cache and slot counts are not part of the stored format: a reopen may use
different ones.

## Transactions, rollback and commit

One transaction is active at a time. `begin` snapshots the logical state
including handle allocation, and reads inside it include the pending edits.
`put`, `erase` and `commit` need an active transaction. `commit(durability)`
publishes a committed state only when it returns `ok`; `buffered` permits an
orderly store close and reopen on the same live backing object, `data` and
`full` additionally call the backing's durability hook, journal and data before
publication. A memory backing does not
advertise durability: asking it for `data` or `full` fails the commit. For
those two levels the hook is called once more after the new state is published,
so an `ok` means the publication itself reached the medium; if that last call is
refused the commit reports it and `rollback` takes the publication back.

A failed `put`, `erase` or `commit` leaves the transaction active and
rollback-only. From then on only `rollback` is accepted; `begin`, `put`, `erase`,
`commit` and every read, walk and digest return `programming_error` and change
nothing. `rollback` restores every pre-begin byte, handle, count, allocation
counter and the exact iteration order, however many pages were evicted or
spilled meanwhile, and the space the aborted transaction used stays reusable:
repeated begin, put and rollback cycles do not grow the backing without bound.
A mutation attempted from inside a read, comparison or source callback returns
`programming_error` with no effect at all; it does not poison the transaction.
A `rollback` that the backing refuses reports the failing call's status and
leaves the transaction active and rollback-only; calling `rollback` again
resumes where it stopped and completes once the backing answers. A nested
`begin` and a `commit` that names an unknown durability level are refused
before any work and change nothing. The state of the transaction is judged
before any handle or key: outside a transaction, or in a rollback-only one,
even an absent handle gets `programming_error` and not `not_found`. The store
never retries a failed backing call.

Destroying a store with an active transaction never commits it. If its rollback
succeeds, reopening on the same live backing yields the last successful commit;
if rollback is refused, the host discards the backing. Reopen does not replay
the journal. A commit whose publication was torn or refused by the backing
publishes nothing after a successful rollback.

## The no-refusal rule

`ScratchStatus` has no capacity member and the API has no `RunFailureCode`
conversion. A `put` never refuses because a resident slot is full: it spills to
the backing. `put` and `erase` return `io_error` when a backing call inside them
returned `io_error`, whether from a medium failure or a per-run extent-growth
byte-cap overrun. Other failures, including exhausted backing, a failing
comparator or source, a call outside a transaction or an exhausted handle
domain, return `programming_error`. A failed `put` leaves its output untouched.
`programming_error` denotes misuse or insufficient provisioning, not a run
failure or retry. The host provisions the backing: a production backing
extends or spills, and a fixed test backing is sized for the whole workload.
A backing MAY additionally cap bytes of extent growth per run, as defined
under Backing and charged I/O; overruns are typed scratch I/O failures with
status `io_error`. The guard test fails if the closed status vocabulary gains
a member or if a contract header gains an exception, a growing container or
a heap call.

## Digests

`ordered_digest` is a noncryptographic FNV-1a-64 over the content only: basis
14695981039346656037, prime 1099511628211, folding `LE64(record_count)` and then,
for each record in ascending order, `LE64(key_bytes)`, the key bytes,
`LE64(value_bytes)` and the value bytes. It is independent of handles, cache,
chunk sizes, insertion history and transactions. `state_digest` is for consumer
state hashing and is legal outside a transaction only: it folds
`LE64(order id)`, `LE64(next handle)` and `LE64(record_count)`, and then each
record as `LE64(handle)` followed by the same four items. Both take a
caller-owned page of exactly `page_size()` bytes and write their output only on
`ok`.

## Readback

`ScratchReadback` replaces owning-sequence reads. `point` gives a record's
handle and lengths. `read` copies exactly the destination's size from a byte
offset of one field. `visit` pushes one record to a chunk visitor through a
caller-owned page: all key chunks, then all value chunks, each at most one page,
offsets contiguous from zero in each field, the last chunk of a field marked
final, and one empty final chunk for an empty field. `for_each` pushes one
`ScratchRecordView` per live record, whose key and value are byte sources that
resolve logical bytes rather than cache pointers. A nonempty field never gets an
empty chunk: the chunk that reaches its size is the one marked final. A source
read at an offset beyond the size is a `programming_error` even for an empty
destination.

A visitor that returns `false` cancels the walk with `cancelled`, including on
the last record, and receives no further call. An error is never reported as
cancellation or as an empty stream. A visitor may issue nested reads, walks and
digests on the same store with disjoint fixed buffers; the outer views stay
valid. A probe given to `lower_bound` may itself be a source backed by this
store, such as a key a walk delivered; the reads the comparator then makes nest
like any other read. Views, sources and chunk bytes live only until their
callback returns.

An owning-return compatibility wrapper may exist only as a non-flat test oracle.
It must not be used by a per-run or per-bar path, advertised as O(1), or
substituted for point, page or visitor reads by a consumer.

## Backing and charged I/O

A `ScratchBacking` is positional and page granular: pages are exactly one page
size, ids are below `scratch_no_page`, a read at or past the extent is
`not_found`, and a successful write extends the extent to cover the id and
zero-fills any gap. A failed write may have changed its target, which is why a
transaction must protect previously committed pages. No operation throws.
Exhaustion is a `programming_error`, never a capacity code. An optional per-run
byte cap bounds extent growth, measured in bytes from the backing's extent at
run start (`page_count() * page_size()`). Before each write that would extend
the backing changes anything, the backing checks the proposed extent minus
that initial extent against the cap, including any zero-filled gap. A write
past the cap returns the typed scratch I/O failure `io_error`. The cap counts
extent growth, not total bytes written, and does not reset on `begin`, `commit`
or `rollback`. Writes below the current extent, including rollback writes,
remain serviceable under the cap.

The backing file's life is the backing object's life: nothing written outlives
it. Disk/tmpfs files are unlinked no later than destruction (created unlinked
where the OS allows), and memfds close with the object. No other object or
process reopens the file. Reopen uses the same live backing with a new store
and does not replay the journal; consumers cannot assume persistence or crash
recovery. If a destroyed store's rollback fails, discard the backing rather
than reopen its unspecified contents. Per-run commits use `buffered`.

Every attempted `read_page`, `write_page` and `sync` is reported once to the
`ScratchIoObserver`, errors and refused arguments included, with the backing's
kind, the page id (the sentinel for a sync), the requested byte count and the
status. That includes journal and rollback traffic and memory-backed traffic, so
the charge is the charged boundary: the store's own counters in `stats` equal
what the observer saw. The observer runs synchronously inside store and backing
calls; it must not call any member of the store or of the backing.

`FixedMemoryScratchBacking` is the in-process test backing over caller-owned
bytes with no allocation, file or growth. It supports `buffered` only. The
backings for `disk`, `memfd` and `tmpfs` are written against the same base class;
choosing among them is the host's decision at construction.

## Reference definition

`include/pineforge/detail/scratch_store_impl.hpp` defines `ScratchStore`, and
`scratch_store.hpp` includes it, so one include gives a working store. It is a
reference definition: the declarations are the contract, and a replacement that
keeps them and passes the fixtures is equally valid. It uses fixed-size pages
whose last sixteen bytes are reserved, a copy-in copy-out page cache that never
hands out a pointer, an in-place undo journal whose page images are written
through before the first change of a page in a transaction, two alternating
superblocks written at commit, a B+tree of handles ordered through the caller's
comparator, a radix table from handle to blob locations, and blobs held as
chains of 64-byte cells or of whole pages. The resident workspace is one block
sized by the config and allocated once, in the constructor. `journal_slots` and
`index_slots` are validated floors: the definition's fixed work buffers exceed
them.

Its limits, which a replacement need not share: a page size of at most 1 MiB, a
cache of at most 16 Mi pages, records below 2^61 bytes and handle numbers below
2^62; index nodes are not merged after erasures; cell pages never return to the
page pool; the journal pool keeps as many pages as the largest transaction so
far touched; reopen starts with an empty journal pool because a refused pool
write may have torn a saved link, forfeiting at most the largest transaction's
journal per reopen; a journal write the backing refuses forfeits the one pool
page it was aimed at; and the handle table never shrinks in height, so a root
that grew over an empty node leaves that node in place.

## Statuses

| Status | Meaning in this API |
| --- | --- |
| `ok` | The call succeeded. |
| `end` | An ordered seek found no record; the output handle is `scratch_no_handle`. |
| `cancelled` | A visitor returned `false`. |
| `not_found` | An absent handle, a page past the extent, or nothing to reopen. |
| `programming_error` | Contract misuse or a request the host did not provision for. |
| `io_error` | The medium failed or a backing's per-run extent-growth byte cap was exceeded. |
| `corrupt` | Stored bytes failed validation. |

`put` and `erase` preserve `io_error` from backing calls inside them; other
failures are `programming_error`. An `erase` of an absent handle is `not_found`
and is not a failure. `commit` reports the failing backing call's own status.
Reads return backing failures (including `programming_error` if an eviction
inside a transaction hits exhausted backing), `corrupt` and comparator
failures unchanged.

## What a consumer lane must do

1. Read through `ScratchReadback` or the store's point, seek and walk calls. Do
   not collect a stream into a vector, a map or a string on a per-run path.
2. Size buffers once. Give each nesting level of a visitor its own page.
3. Hold handles, not pointers or offsets. A handle is the only durable
   reference to a record.
4. Supply a deterministic comparator and bump its order id when the key schema
   changes.
5. Group edits in a transaction, and call `rollback` on any non-`ok` result of
   `put`, `erase` or `commit`.
6. Never mutate the store from a callback, and never pass `put` a source that
   reads the same store.
7. Use `state_digest` for state hashing and `ordered_digest` for content
   comparison. Do not hash physical placement.
8. Choose the backing kind explicitly and provision its extent. Do not map
   `programming_error` to a capacity or retry code. Route backing `io_error`
   from `put`, `erase` or `commit`, including a per-run extent-growth byte-cap
   overrun, to the typed scratch I/O failure without retrying the operation.
9. Use `test_memory` only in tests. Per-run commits use `buffered` on every
   backing kind.
10. Add a status, a field or a header only additively and announce it.

## Fixtures

Each row names the clause, the fixture function that holds it and the failure
before the contract holds. Against the frozen base the three headers are absent,
so every body fails to compile with the missing-header diagnostic; against the
declarations alone, without `scratch_store_impl.hpp`, the store fixture compiles
and fails at the link of `ScratchStore`, which they declare and do not define.

| Clause | Fixture | Fails before because |
| --- | --- | --- |
| Backing extent, zero fill, isolation, charges, explicit kind, durability | `tests/test_scratch_backing.cpp` | the header is absent |
| Closed status set, no exception, container or heap call in the headers | `tests/test_scratch_contract_guard.py` | the headers are absent |
| Exact ascending order after insert and erase, caller comparator not byte order, descending order | `ascending_after_insert_and_erase` | no store to open |
| Successive stable never-recycled handles; rollback reissue | `handles_are_successive_stable_and_never_recycled` | no store to open |
| Forced eviction of a two-page cache, re-read by handle | `forced_eviction_then_reread_by_handle` | no store to open |
| Nested reads, walks, seeks and digests during iteration; refused mutations | `reentrant_reads_during_iteration`, `comparator_cannot_mutate_the_store` | no store to open |
| Rollback after partial writes, torn writes at twelve write positions, rollback-only state | `rollback_after_partial_writes`, `rollback_survives_torn_writes_at_every_phase` | no store to open |
| A failed commit publishes nothing, durability refusal, transaction state machine | `failed_commit_publishes_nothing`, `durability_failure_and_transaction_state_machine` | no store to open |
| Commit then reopen on other geometry, abandoned transaction, open failures | `commit_then_reopen_and_abandon` | no store to open |
| Digest independent of page size, cache, journal and batching; digest covers every byte | `digest_is_throughput_neutral`, `digest_covers_every_stored_byte` | no store to open |
| No heap allocation or refusal after construction | `no_heap_after_construction` | no store to open |
| Bounded backing under churn and repeated rollback | `space_is_reused_under_churn`, `rollback_does_not_leak_space` | no store to open |
| Backing follows the live range although handle numbers are never reissued | `handle_table_space_follows_the_live_range` | no store to open |
| A rollback refused at every write position, torn and clean, is retried to the last commit | `rollback_can_be_retried_after_a_refused_write` | no store to open |
| Durable commit: hook before and after publication, a refused first or final sync is taken back | `durable_commit_publishes_between_two_syncs` | no store to open |
| An exhausted backing, a failing source and a refused workspace are programming errors | `named_failure_causes_are_programming_errors` | no store to open |
| A per-run extent-growth cap returns `io_error` from put, erase and commit; rollback and reopen preserve the last commit | `extent_cap_reports_io_error_without_partial_state` | stage 2.2 reclassifies put/erase backing `io_error` as `programming_error` |
| Keys and values larger than the cache, formula sources | `keys_and_values_larger_than_the_cache` | no store to open |
| Equal keys, key bytes, empty key and value | `equal_keys_upsert_and_key_bytes` | no store to open |
| Buffer misuse, cancellation, failing medium, failing comparator | `misuse_and_error_surface` | no store to open |
| Geometry floor and refused opens | `geometry_and_open_validation` | no store to open |
| Randomized differential walk against a sorted model | `differential_random_walk` | no store to open |

The store fixture is registered unconditionally in
`tests/cr150.d/core-cr150-o1c.cmake`, together with the backing fixture and the
guard, and runs against the reference definition below.

The store fixture checks every expected value against an independent sorted
model and an independent FNV fold. It reads the population it claims to test
from counters it does not control: backing reads and writes, comparator calls
and injected fault hits. `tests/global_allocation_replacement.hpp` closes the
heap for the no-allocation fixture.

## Not specified

These are named so nobody builds on an assumption:

* A partial in-place write inside a value (`patch` by handle and offset) and a
  value replacement by handle alone. A consumer chunks a large object into
  records keyed by object and chunk index, or asks for an additive patch.
* Reverse iteration (`last`, `prev`) and range erase. A descending comparator
  gives descending order.
* The implementations of the `disk`, `memfd` and `tmpfs` backings, and any
  upper bound on page size or record size beyond what the backing provides.
* Crash recovery: backing files never outlive their object and reopen does not
  replay the journal; durability-hook ordering is not a recovery protocol.
* Sharing one backing between stores, and any concurrent use.

## O(1) seams s3 and s4

This section binds the journal codec (s3) and the queue owner and its current
membership queries (s4). It specifies consumer obligations, not new C++
declarations or a claim that the present consumers satisfy them. The red-test
names below are requirements for later implementation lanes; no such test is
claimed to have run. The ScratchStore contract above still applies.

Here Q is the working and queued request population, P the open physical-lot
population, J the journal population and C the cohort-membership population.
None is a constant, including within one bar, transaction or retained window.
O(1) means a fixed memory bound at fixed construction geometry; it does not
promise constant operation count or I/O. A small observed Q, P, J or C proves
nothing about that bound.

### Ownership

There are two journals. The native order-command journal is
`WorkingRequestCore::history_`, beside its working book `live_`
(`include/pineforge/native_order.hpp:2301`,
`include/pineforge/native_order.hpp:2302`); the native consumer owns that core
as `requests_` (`src/native_execution_consumer.hpp:1739`). The admission
journal belongs to the source adapter
(`include/pineforge/source/pine_adapter.hpp:1851`), which the source host owns
(`include/pineforge/source/pine_strategy_host.hpp:1072`). Neither journal is
the ScratchStore transaction undo journal. They do not share a sequence
domain, record tag, retention frontier or digest by implication.

The core owns the executable working queue. Source queues hold instructions
awaiting submission and source placement evidence, not a second executable
book. `remember` and `retire` maintain the adapter's handle projection
(`src/source/pine_adapter.cpp:3507`, `src/source/pine_adapter.cpp:3522`);
the host's pending observer reads both queued instructions and that projection
(`src/source/pine_strategy_host.cpp:1444`,
`src/source/pine_strategy_host.cpp:1454`). Moving storage preserves this split.

Each clause row gives its observable input, invariant, bound, refusal and one
required red test. A refusal of the bounded claim is a failed qualification,
not a newly declared runtime status. The bounds B_u and B_p are defined below
and cover simultaneous work, rather than being a fresh allowance per row.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S3-O | Append to both journals, reset the source adapter, then begin another run with reused incarnation numbers. | Ownership and sequence frontiers stay separate; no record or prefix from the previous run is read as current. | Combined userspace <= B_u; charged physical memory <= B_p. | Stale-run read, cross-journal alias or population-sized copy refuses qualification; scratch failures follow S34-S. | `s3_separate_journal_owners_and_reset_red` |
| S4-O | Submit, replace normally, re-price with keep_handle, cancel and execute while source instructions remain queued. | One native executable row per live handle; source projection and queued intent retain their existing meanings; matching order is ascending LiveRequest::priority, including a kept handle moved to the back (`include/pineforge/native_order.hpp:1706`, `include/pineforge/native_order.hpp:781`). | Combined userspace <= B_u, including pending observers and replacement scratch. | Duplicate execution authority, stale projection, changed tie order or memory above B_u refuses qualification. | `s4_one_queue_owner_kept_handle_order_red` |

### Records and ordering

The native record set is every alternative of `CommandEvent`, in declaration
order: AcceptedEvent, RejectedEvent, ReplacedEvent, ReplaceRejectedEvent,
CancelledEvent, NotWorkingEvent, InvalidHandleEvent, NoEffectEvent,
MatchRejectedEvent, ExecutionAppliedEvent, CloseBoundEvent, ActivatedEvent,
ReservationReducedEvent, DeferredGroupAdjustmentEvent, QuantityBoundEvent,
ArmedEvent, TermsResolvedEvent, MarginCallEvent and NativeRiskEvent
(`include/pineforge/native_order.hpp:1312`). Each record carries its complete
existing payload, including the values reached through DefinitionRef, optional
values, nested variants, text and ordered arrays. For example, both definitions
of a replacement survive independently
(`include/pineforge/native_order.hpp:1021`); a compact outcome digest cannot
reconstruct them. Pointer addresses, padding and shared_ptr control blocks are
not record values. Definition values include predecessor, root, priority and
kept_binding (`include/pineforge/native_order.hpp:726`,
`include/pineforge/native_order.hpp:755`).

Native commits append in increasing command ordinal; the retained sequence has
absolute positions [history_base(), history_end()), oldest first
(`include/pineforge/native_order.hpp:1710`). `ordinal_index_` maps those ordinals
to absolute positions, not vector offsets
(`include/pineforge/native_order.hpp:2306`). Admission records are CommandEvent,
ReviewEvent and SizingEvent, in that variant order
(`include/pineforge/source/market_admission.hpp:181`). Their observation,
before/removed, book/reviewed/resolutions/causes, sizing and receipt values are
the existing fields (`include/pineforge/source/market_admission.hpp:75`,
`include/pineforge/source/market_admission.hpp:145`). Admission order is ascending
`admission::sequence`, not append completion order: append inserts at a sequence
lower_bound and consumes an outstanding allocation
(`src/source/market_admission.cpp:154`). The present adapter append sites are
`src/source/pine_adapter.cpp:5017`, `src/source/pine_adapter.cpp:21429` and
`src/source/pine_adapter.cpp:21493`.

The codec spells existing values field by field. A native record is its variant
and active payload, whose fields follow their declarations recursively. Handle
identities encode their session text and run number, never an interned address
(`include/pineforge/native_order_identity.hpp:125`). An admission record is the
ordered sequence of existing Field {path, value} rows visited by
reflect(event, "event"), framed as an array of those rows
(`src/source/market_admission.cpp:266`,
`src/source/market_admission.cpp:286`, `src/source/market_admission.cpp:326`).
Its kind, presence and nested-array size fields are already in that sequence;
there is no second event tag or presence marker outside it. FieldValue's own
variant index still precedes its value. The stream need not be collected to
count or encode its rows.

For storage, an unsigned integer is LE64; a signed integer is sign-extended to
64 bits and written as little-endian two's complement. An enum follows its
underlying integer's signedness. A bool is one byte 0 or 1, and a double is its
eight IEEE-754 bits in little-endian order. A string is LE64(byte length) then
exactly those bytes, including embedded NULs; an array is LE64(element count)
then the elements in their existing order. A variant starts with LE64(its
existing alternative index); an optional or shared value starts with one
presence byte and, if present, its value. Structs concatenate their fields;
an empty alternative contributes no payload. No object padding or pointer is
copied. These are framing rules, not added broker fields. Signed zero and NaN
payloads survive storage even when the existing hash normalizes them. Existing
base/frontier/prefix counters remain separate journal state and use the same
scalar spelling. Length arithmetic is checked before reads or publication. A
value larger than the cache is read and written in chunks; there is no
maximum-record-sized resident temporary. Decoding checks representability in
the original integer type before narrowing. Unknown enum numbers already
captured in an attempted/rejected command remain values, not structural tags
to discard or reinterpret.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S3-NR | Every native event alternative, including two versions of a kept-handle definition, long text, embedded NULs, absent/present optionals, signed zeros, distinct NaN bits and adjustment arrays larger than the cache. | Decode reproduces every active payload value and ordinal in absolute journal order; changing sharing or eviction changes no value. | Userspace <= B_u for arbitrary record length; no native variant array is collected. | Bad tag, presence byte, bool, truncated value or impossible length is corrupt; buffer misuse is programming_error; scratch I/O is io_error. | `s3_native_codec_all_values_cross_page_red` |
| S3-AR | Reserve admission sequences, complete them in reverse order, abandon one, and include all three event kinds with nested Draft observations and book arrays larger than the cache. | Only completed allocated events appear, sorted by sequence with legal gaps; fields and array order match the original reflection and value model. | Userspace <= B_u, including captures, observations, outstanding allocations and nested arrays. | Unallocated/reused sequence retains the existing engine_invariant refusal (`src/source/market_admission.cpp:157`); invalid stored framing is corrupt; a lost or reordered value refuses qualification. | `s3_admission_codec_allocations_and_order_red` |

### Fixed memory

Fix, before the run, a finite set of stores and their geometry. Let W_i be
store i's constant `ScratchStoreStats::resident_bytes`
(`include/pineforge/detail/scratch_store.hpp:44`), s_i its page size and n_i
the fixed number of disjoint caller pages needed at maximum callback nesting.
Let F be the byte census of all other fixed owners, codec frames, counters,
construction-time text buffers and maximum simultaneous stack frames, excluding
bytes already in W_i or the caller pages. Then

    B_u = F + sum_i(W_i + n_i * s_i)
    B_p = B_u + A + M

A is a proved upper bound on allocator rounding/metadata for that fixed
allocation list; M is a proved bound on charged backing/cache/staging physical
pages not already counted. Every term, object count and nesting depth is fixed
independently of Q, P, J, C and record/text length. The implementation handoff
must enumerate the list and give its byte values for the measured ABI. A
symbol without that census is not a proof. The store already forbids allocation
after construction (`include/pineforge/detail/scratch_store.hpp:55`); the codec
and queue cannot put a growing allocation outside its stats and call it fixed.

Count each physical allocation island once, including unused capacity and
allocator overhead. For a vector the payload charge is capacity * sizeof(T),
not size * sizeof(T). Add each distinct shared observation/definition allocation
and its control block, out-of-line string capacity, nested vector capacity,
map node, hash bucket array, free node and decode/callback temporary. An inline
string or embedded variant is already in its containing object. A shared
observation still held by a Draft after journal retirement remains charged.
Deallocation does not prove physical release when the allocator retains pages;
that retention belongs in A. A peak-sized cleared vector remains an island.

For admission, the census therefore includes events_.capacity() *
sizeof(admission::Event), distinct CommandObservation/control-block allocations,
their text and every before/book/reviewed/resolutions/causes/removed allocation,
as well as outstanding_sequences_ and producer scratch. The brief's design
estimate is 480 + 288 * 1.31072 = 857.48736 bytes, about 0.86 decimal KB per
admitted entry/order, before additional payloads and allocator costs. It is a
planning estimate, not a portable sizeof assertion or an upper bound. For the
native journal also count history_, ordinal_index_, shared definitions, nested
payloads, pending chains and notification queues. A retain window does not make
any of these populations constant.

For s4 count live_, repriced_, issued_, successor roots, group/cohort receipts
and rosters (`include/pineforge/native_order.hpp:2320`,
`include/pineforge/native_order.hpp:2329`,
`include/pineforge/native_order.hpp:2337`), every source queue/placement and its
indexes (`include/pineforge/source/pine_adapter.hpp:2598`), and OriginRoster's
items_, positions_ map, buckets and inner position vectors
(`include/pineforge/source/pine_adapter.hpp:995`). The successor-root record is
provably 16 bytes with a 64-row inline tail
(`src/native_order.cpp:5878`, `src/native_order.cpp:5879`); neither its disk row count nor its predecessors
are bounded by 64. The brief's 40 bytes per cohort receipt, 24 bytes plus a map
node and inner vector per roster opening, and roughly 119.5 bytes per trade are
also planning estimates, not exemptions from the census. O(current Q/P/C),
O(the largest bar), and a capacity proportional to a past peak all fail B_u.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S3-M | With geometry fixed, increase J and payload length separately, delay acknowledgements, pin a long deferred chain, and emit arbitrarily many admission events inside one bar before draining. | Every allocation island above is counted through construction, append, folding, eviction, retirement and drain; excess records remain exact in backing. | Userspace <= B_u and charged physical memory <= B_p at every phase, including transient peaks. | Any growing allocation, peak capacity, uncharged shared payload or unproved A/M refuses the bounded claim; resident-slot exhaustion must spill, not truncate or cap the workload. | `s3_journal_islands_fixed_under_pins_red` |
| S4-M | Increase Q, P and C independently; churn replacements, cohort receipts and duplicate roster entries, drain them, then repeat with the same geometry. | Queue, indexes, roster positions and receipts remain exact; neither live populations nor old peak capacity are a resident allowance. | Userspace <= B_u and charged physical memory <= B_p during updates, observers, retirement and empty-state reuse. | Population-sized scratch, nodes, buckets, free lists or retained capacity refuse qualification; a workload cap is not an O(1) proof. | `s4_queue_and_membership_islands_fixed_red` |
| S4-RE | Mark P open lots twice, read terminal rows, clear, then mark again. | Preserve lot order, row values and the existing left-fold P&L; retain only the terminal mark descriptor, as the current producer already does (`src/native_execution_consumer.cpp:9084`, `src/native_execution_consumer.cpp:9091`). | Descriptor plus fixed row/chunk scratch <= B_u; no P-row/string cache. | Non-finite rows retain the current refusal (`src/native_execution_consumer.cpp:9096`); any materialized range_end_trades_ population refuses qualification. | `s4_terminal_mark_descriptor_stays_fixed_red` |

### Transactions and retain windows

For native Window retention, retire only the acknowledged prefix (or the
callback-only frontier), before any queued applied notification's cause, and
before the oldest event a live deferred chain still reads
(`src/native_execution_consumer.cpp:3432`, `src/native_order.cpp:1107`,
`src/native_order.cpp:1129`). The source polling host acknowledges zero at run
begin (`src/source/pine_strategy_host.cpp:406`). No expiry measured in bars or
entries replaces these dependencies. Full and Commands preserve their existing
readback policy; Window alone is not a memory proof. The presizer explicitly
skips Window but may reserve by projected batch length for other policies
(`src/native_execution_consumer.cpp:10475`); such reservations cannot qualify.

Admission already has a retired Prefix of count, digest, last_sequence and
last_rejected_command_bar
(`include/pineforge/source/market_admission.hpp:193`). The ordinary source bar
boundary folds and then retires the completed tail
(`src/source/pine_adapter.cpp:24254`, `src/source/pine_adapter.cpp:24264`);
a completed process-on-close tail has its additional boundary
(`src/source/pine_strategy_host.cpp:3084`). Retirement requires no unfinished
allocation/capture and the exact folded count and final sequence
(`src/source/market_admission.cpp:184`). `retain(sequences)` is a different,
destructive filter (`src/source/market_admission.cpp:166`); it has no production
caller in this tree and supplies no prefix-fold proof. `reset` is a run reset,
not a way to bound a running journal (`src/source/market_admission.cpp:171`).

The retain proof names every remaining reader and shows either that its exact
bytes remain addressable or that its existing summary is sufficient. Admission
also has a command-gate reader, which searches the tail backwards and falls back
to Prefix::last_rejected_command_bar
(`src/compat/pine/market_admission.cpp:141`,
`src/source/pine_adapter.cpp:4373`); the running hash is not its sole reader.
The diagnostic admission_history folds only events() today
(`src/compat/pine/market_admission.cpp:153`); prefix retirement must not silently
advertise that projection as complete historical readback.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S34-T | A logical record/index update with injected failure at each scratch put, erase and commit, followed by rollback and same-backing reopen. | Publish the new scratch records and their lookup relations only after successful commit; failure preserves the old committed relation, ordering, handles and digests. Preserve existing native command/counter refusal semantics outside the scratch transaction. | Undo, index work and rollback scratch together <= B_u regardless of transaction length. | put/erase/commit failure makes the transaction rollback-only; only rollback may proceed; failed rollback is reported and never treated as successful cleanup. | `s34_record_index_publication_rollback_red` |
| S3-NW | Advance acknowledgements above and below the high water, queue applied notifications, keep and then resolve a deferred chain, and compare Full/Commands/Window at each boundary. | Retire only the dependency-free prefix; preserve absolute positions and totals; later reads of every pinned cause still return its original value. | All pinned and retained bytes use backing beyond B_u; no projected-batch reserve or per-pin resident directory. | Retiring a referenced cause, losing an unacknowledged event, capacity growth or silent policy downgrade refuses qualification; storage errors follow S34-S. | `s3_native_window_dependency_frontier_red` |
| S3-AW | Complete and unfinished captures across terminal bars and process-on-close tails; rejected commands in both prefix and tail; attempt destructive retain after a fold. | Retire only an exact, fully folded, insertion-closed prefix; preserve the gate's last rejected bar in sequence order and every still-referenced Draft value. | Prefix and fold cursors are fixed-size; all unfinished/tail payloads together still fit B_u through backing. | Unfinished or mismatched retirement keeps the existing engine_invariant refusal; an unproved retain/filter or a refold from zero outside a true run reset refuses the bounded claim. | `s3_admission_window_requires_closed_prefix_red` |

### The current-membership indexes and the five predicates

The five predicates are the five disjuncts in the actual OriginRoster prefix
keep decision (`src/source/pine_adapter.cpp:28338`). They answer whether an
origin must stay addressable. They are not five invented order filters or an
exhaustive count of all boolean helpers in the engine. In that loop the first
true disjunct stops retirement: only a contiguous prefix for which all five
are false is released. A pinned first origin may therefore retain an arbitrarily
large suffix. The early front-row shortcut
(`src/source/pine_adapter.cpp:28270`) is the same retained-placement test.

The index set is the existing logical lookup relations: native handle-to-live
row and priority (`src/native_order.cpp:1073`), source-key-to-live-handle and
incarnation-to-placement (`include/pineforge/source/pine_adapter.hpp:2600`,
`include/pineforge/source/pine_adapter.hpp:2601`),
incarnation-to-global-roster-positions
(`include/pineforge/source/pine_adapter.hpp:979`), the named-incarnation workset,
per-cohort live_units_by_origin and opened
(`include/pineforge/source/pine_adapter.hpp:1882`), and the native cohort's
canonical-root membership (`src/native_order.cpp:1291`). The core's issued
ranges and successor-root relation authenticate that last lookup
(`src/native_order.cpp:1190`). The journal's ordinal index and its receipt
lookup relations remain separate from current working membership. No
historical journal scan is a substitute for a current member.

`named` is currently a sorted, deduplicated transient vector, not an existing
persistent index. Its exact inputs are the nonzero names in snapshot fields
bracket_origin, paired_reversal_parent, preserved_by_close_all,
projection_predecessor, legs.target() and reservation_growth_owner_incarnation,
and the request owners WaitForApplied, BindOpening and every BindOpenings
member (`src/source/pine_adapter.cpp:28277`). The enumeration includes source
live handles and their snapshots, native live rows/their request owners/roots,
pending bracket legs, pending entries, delayed markets, deferred market sells,
throttled rearms, same-bar commands, shadow pending, coof requests, margin
revivals and anchored-relative leg handles/parents/owners
(`src/source/pine_adapter.cpp:28303`). A bounded implementation may stream
these same inputs instead of collecting them; it cannot omit a queue, or drop
a name because one of several referring owners was removed.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S4-P1 retained placement | An origin whose native order has ended but whose source placement remains, then actual placement erasure. | Exactly placement_.contains(origin.incarnation), including a terminal retained row (`src/source/pine_adapter.cpp:28338`); not find_live(origin). | Lookup and update scratch <= B_u with arbitrary retained placements. | A stale/failed lookup is not absence; wrong truth or a resident placement-sized index refuses qualification; scratch errors follow S34-S. | `s4_predicate_retained_placement_red` |
| S4-P2 named by work | For each enumerated owner/queue, pin an otherwise released origin; have two owners name it, remove one, then remove the other; include zero and native definition roots. | Exactly binary_search(named, origin.incarnation), excluding zero; true until the last current reference disappears (`src/source/pine_adapter.cpp:28339`). | Enumeration, deduplication or equivalent lookup <= B_u for arbitrary Q and BindOpenings length. | Missing enumeration, premature unpin, stale name or a materialized named population refuses qualification; scratch errors follow S34-S. | `s4_predicate_named_by_each_current_owner_red` |
| S4-P3 live source units | The same incarnation in two cohorts, a present map entry with zero units, then erasure of that key from only one cohort. | Exactly key presence in this facts.live_units_by_origin, irrespective of its numeric value (`src/source/pine_adapter.cpp:28340`); not a quantity > 0 test. | Cohort lookup/update scratch <= B_u for arbitrary C. | Wrong cohort, treating zero as absence or a resident member-sized map refuses qualification; lookup I/O cannot return false success. | `s4_predicate_live_units_key_presence_red` |
| S4-P4 opened source member | An origin in facts.opened with no other pin, duplicate opened entries, and equal incarnation numbers under different RunIdentity values. | Exactly full RequestHandle equality in this facts.opened (`src/source/pine_adapter.cpp:28342`); removal of one duplicate is not removal of the remaining member. | Full-handle membership and ordered readback scratch <= B_u for arbitrary C and identity text length. | Incarnation-only equality, lost duplicates or a resident opened-sized set refuses qualification; scratch errors follow S34-S. | `s4_predicate_opened_full_handle_red` |
| S4-P5 native cohort member | A native cohort containing a canonical root, its live and terminal replace successors after history retirement, kept-handle re-prices, an unknown handle and a foreign-run handle. | Exactly core.cohort_contains(facts.handle, origin) (`src/source/pine_adapter.cpp:28343`); authenticate issuance, normalize to the root, then test that cohort's sorted canonical origins (`src/native_order.cpp:1190`, `src/native_order.cpp:1291`). | Membership and successor-root reads <= B_u for arbitrary replacement history and C. | Unknown/foreign/absent members return false as today; root-read I/O propagates and must not become false or self-root (`src/native_order.cpp:1197`); population-sized indexes refuse qualification. | `s4_predicate_native_canonical_cohort_red` |
| S4-I | Exercise each pin independently with the other four false, all combinations, duplicate roster positions, a pin near the front of a large suffix, then release pins and reset the run. | Release only the all-false prefix; positions stay global and ascending, full-handle filtering stays exact, and roster size()/digest() survive retirement (`src/source/pine_adapter.cpp:28217`, `src/source/pine_adapter.cpp:28225`). Derived caches must agree after insertion, replacement, erasure, spill and reset. | All indexes, including old_ placement nodes, OriginRoster positions_ and adapter caches, share B_u; no C-sized release bitmap or Q-sized temporary. | A wrong position, stale membership, released pin, unbounded suffix in RAM or a missing dependency proof refuses qualification. | `s4_five_pin_frontier_and_indexes_red` |

### Digests

Storage bytes and semantic hash bytes are different contracts. Native
hash_event_record folds the variant index and compact outcome/reason only
(`src/native_execution_consumer.cpp:949`). note_committed_events folds it once
at commit together with successor roots and group receipts, before retirement
(`src/native_execution_consumer.cpp:11894`); cohort receipts have their own
running fold (`src/native_execution_consumer.cpp:12074`). Dropping a prefix
advances history_base and the ordinal/receipt windows; it does not subtract
that prefix, reset a digest, fold the new base, or reissue a logical index.
Continuation requires the lifetime counts to agree
(`src/native_execution_consumer.cpp:2051`).

Admission's v4 digest folds fields reflected under the literal path "event",
in ascending sequence, each field's path, value-alternative index and value
(`src/source/pine_state_hash.cpp:21`, `src/source/pine_state_hash.cpp:803`).
It uses BrokerStateHashSink, including its zero/NaN normalization
(`include/pineforge/engine.hpp:481`), rather than native StateFold's exact-bit
word mixing (`src/native_execution_consumer.cpp:150`). The parent hash includes
sequence_frontier, lifetime folded count and digest
(`src/source/pine_state_hash.cpp:774`), not the number of resident events.
The present cursor mismatch path restarts at the retired prefix, not necessarily
at zero (`src/source/pine_state_hash.cpp:793`). A zero restart after shortening
or filtering is not successful bounded qualification. A legitimate run reset
starts a new fold; a validated nonzero prefix may seed streaming of its tail.

Source roster size and append digest, opened order, live-unit keys/values and
placement values keep their existing hash order
(`src/source/pine_state_hash.cpp:307`). The adapter's lookup cache is derived
and unhashed (`src/source/pine_adapter.cpp:1530`); its cohort sides, unbound
legs, legs_by_origin and immediate_closes_by_bar still count as memory
(`src/source/pine_adapter.cpp:1559`, `src/source/pine_adapter.cpp:1576`).
No new independent durable state may hide behind the word index. Store
state_digest checks the store's own committed state; it is not a replacement
for any of these existing native/source folds.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S3-ND | Replay the same commands under multiple page/cache geometries and retention schedules; retire to a nonzero base, then append and resolve deferred receipts. | Existing continuation digests and lifetime event/root/group/cohort counts match the independent pre-retirement fold after every commit; prefix retirement adds no hash bytes. | Fixed accumulators and streaming scratch <= B_u. | A missed/double fold, cursor behind an unavailable prefix, changed version/hash bytes or history-sized refold buffer refuses qualification. | `s3_native_digest_survives_nonzero_base_red` |
| S3-AD | Fold all admission kinds, retire, append more, perturb the folded cursor, shorten/filter the tail, and reset for a new run; include normalized zeros/NaNs and a rejected-command prefix. | Preserve original v4 field paths/order/counts and gate summary; a recovery may use a validated prefix only, with no silent change to lifetime history. | Prefix/cursors plus streaming scratch <= B_u for arbitrary J. | Refold from zero outside run reset, an unvalidated prefix, an event inserted behind a retired frontier or changed hash bytes refuses qualification, even if a later final digest happens to match. | `s3_admission_digest_refuses_zero_refold_red` |
| S4-D | Compare queue/roster/index state across eviction, compaction, cache discard, spill, rollback and reset using identical commands. | Existing source/native semantic digests and ordered values agree; rebuildable indexes add no hash leaf, while every independent durable value remains covered by its existing fold. | Hashing, sorting or cache rebuilding scratch <= B_u for arbitrary Q/P/C. | An unhashed independent state, address/placement-dependent digest or population-sized hash temporary refuses qualification. | `s4_membership_hashes_ignore_storage_red` |

### Readback

Production reads use ScratchReadback point/page/visitor sources and store
seeks, including nested disjoint pages; a decoded array, text or key may exceed
the cache. Native history_at below base still throws out_of_range and event_at
for a retired event still returns null (`src/native_order.cpp:1080`,
`src/native_order.cpp:1096`). The public native_events snapshot starts at the
window floor when asked from below it
(`include/pineforge/native_host.hpp:1330`); that owning compatibility surface is
not proof of a bounded production reader. An adapter cannot collect those rows
internally and then advertise a fixed-memory codec. Original snapshot lifetimes
and policies remain valid for explicit diagnostic callers.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S34-R | Point reads, ordered walks and nested callbacks after eviction, including records larger than the cache, retired positions, cancelled visitors and injected read faults. | Return exactly the requested logical bytes/order; distinguish absence, end, cancellation and error; retain no callback pointer or page view. | All nested reader pages are included in n_i * s_i; remaining scratch <= F, independent of the walked population. | not_found/end/cancelled keep their existing meanings; corrupt/io_error/programming_error propagate; an owning collection or an error reported as empty/false refuses qualification. | `s34_readback_never_collects_population_red` |

### Backing

Both seams borrow explicitly chosen run-owned backing, with buffered per-run
commits and the same lifetime, rollback and observer rules as above. Backing
extent and total I/O may grow; every attempted page operation is charged.
Neither moving J/Q/P/C bytes to memfd/tmpfs nor mapping a growing file proves a
physical O(1) claim. Disk page cache, readahead, dirty pages and I/O buffers must
fit the proved M; a flat userspace RSS by itself is insufficient.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S34-B | Identical large populations on explicitly selected backings, repeated reads/writes, per-run extent cap failures and owner destruction. | Observer counts every attempted operation once with its real kind; preserve buffered reopen/rollback and backing lifetime; report actual extent growth separately from resident memory. | Userspace <= B_u; all charged physical islands including backing cache <= B_p. | Unproved M, growing charged tmpfs/memfd/cache pages, hidden I/O, a cap overrun treated as success, or persistence beyond the backing owner refuses qualification; cap/medium failure is io_error. | `s34_backing_physical_charge_and_lifetime_red` |

### Statuses

These seams add no status, capacity knob or retry fallback. ScratchStatus keeps
the meanings in this page. A present empty relation and a proved absent member
are successful answers; a failed lookup is neither. Existing native command
rejections and admission engine_invariant exceptions retain their meanings.
The bounded-claim refusals in the tables are evidence failures, not new enums.

| Clause | Observable input | Invariant | Fixed-memory bound | Refusal state(s) | Required red test |
| --- | --- | --- | --- | --- | --- |
| S34-S | Fill resident slots, exceed a backing growth cap, inject medium/corruption faults, use an invalid geometry/handle/buffer, and cancel a walk. | Full resident slots spill; no drop, sampling, retry or alternate-medium fallback makes a failed operation succeed; readers do not convert faults to false membership. | Failure paths and rollback use the same B_u/B_p, with no population-sized diagnostic. | Preserve io_error for medium/cap failure, corrupt for invalid stored bytes, programming_error for misuse/provisioning, not_found/end for proved absence and cancelled for visitor cancellation; route backing io_error to the typed scratch I/O failure as required above. | `s34_failures_are_not_empty_membership_red` |

### What a consumer lane must do

1. Implement the two journals separately, preserving their complete value sets,
   order, retention dependencies and existing digest loops. Demonstrate the
   five cited roster tests individually and their combined release frontier.
2. Give the exact F/W_i/n_i/A/M census and close every growing island above,
   including source queues, observation sharing, indexes and all temporary
   read/hash/retirement work. Keep geometry fixed while Q, P, J and C grow
   separately, together, inside one bar and behind a pinned prefix.
3. Write the one red test named by each row against an independent value/order
   model; compile it first against the frozen previous-epoch header closure and
   record its first diagnostic or executed failure. Use real allocations,
   backing traffic and fault hits as witnesses. Existing correct value tests
   alone are not red evidence for fixed memory. Test all native alternatives,
   all admission kinds and the real per-run/per-bar readers, recording off too.
4. Prove retain before dropping bytes, publish related scratch edits in a
   transaction, and report every zero-refold, unsupported read, unproved memory
   charge and necessary interface change. An implementation that cannot yet
   stream an existing owning API does not qualify by changing its semantics.
5. Keep implementation, CMake and fixtures in their separately assigned lanes.
   This section authorizes no frozen-header edit, performance verdict or
   verification status. Maintainer verification and parity gates still judge
   the resulting implementation tree.

### Not specified

* `work/DESIGN.md` and `work/STREAM-MODE.md` are absent from this tree. The
  brief's admission/order-core byte figures above cannot be verified here as
  measured ABI sizes, allocator charges or bounds. The cited declarations and
  the successor-root static assertion establish the structural census instead.
* The code establishes exactly the five disjuncts at the cited roster release
  decision. It does not identify a different canonical set called "the five
  predicates" for all order filters. Any intended different set needs its own
  source-derived clauses; no additional predicate is inferred here.
* A C++ binding of the fieldwise codec to the frozen owning live()/history()/
  events() surfaces is not declared by this document. These surfaces presently
  expose vectors; an additive or separately authorized interface decision is
  needed before claiming their production callers are bounded. No new method,
  index member, broker field, ABI layout or persisted cross-process format is
  implied. The framing above is a run-local value encoding, not crash recovery.
* Numeric values of F, W_i, n_i, A and M, a production physical-memory proof,
  and executed red/green receipts belong to the implementing and measuring
  lanes. A compile-time slot limit or the observed largest live window supplies
  none of them.
