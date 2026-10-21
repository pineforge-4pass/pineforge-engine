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
