# CommandScratchStore, ScratchReadback and ScratchBacking

The command record store is named `CommandScratchStore` on the integration
line. `pineforge::detail::ScratchStore` remains the existing report-arena
diagnostic enum; its vocabulary and consumers are unchanged.

This page states the shared contract for per-run state that must not grow with
the run: an exact external record store (`CommandScratchStore`), the interface every
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

A `CommandScratchStore` borrows the backing and the observer context for its whole
life. One store holds a backing at a time; a second store on the same backing
is a reopen after the first one ended. Destroying a store never commits.

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
per-visitor state in a container. `CommandScratchStore::stats` reports the fixed
workspace (`resident_bytes`, constant for the life of the object), the backing
extent and the number of backing calls the store issued; it exists so a growth
gate and an I/O audit can read these without an implementation detail.

Supported geometry is exactly: at least two cache pages, a power-of-two page
size of at least 512 bytes that equals the backing's page size, at least one
journal slot and at least one index slot, a nonzero order id and a comparator.
Anything else leaves `status()` at `programming_error` and performs no I/O.
Cache and slot counts are not part of the stored format: a reopen may use
different ones.

### Resident bound of the parent-terminal drain (native order)

The native request core's `JournalStore` has a resident cap of 2,048
committed events in 32-event chunks, with a keep-resident floor of 128.
A single bar can exceed the cap before its journal is retired; older events
then spill to pages and remain readable at their original positions and
ordinals. The spill page remains 32,768 bytes and the decoded cache has four
slots. The separately bound `CommandScratchStore` codec uses its own fixed
workspace instead of this resident tail.

For the short waiting-child definitions in the Wide parent-terminal fixture,
the book cap is 4,096 rows and the journal cap is 2,048 events. Its per-engine
drain plateau is 1,890,980 bytes (about 1.9 MB) on the GNU 13.4 x86-64
allocation sizes, constant and independent of symbols, window and trades.
`tests/test_parent_terminal_scratch.cpp` derives `kDrainPlateauBytes` from
the caps and `sizeof` terms: `456*(2048 + (4096/2+2) + (32+1)) + 6720 + 524`.
The terms are the journal definitions, promoted half-book plus two roots,
one decoded cause chunk with a two-definition replacement, eight side owners,
and overlapping book-writer buffers. During growth, the side owners and a
216-byte book read instead contribute the fixed `6720+216=6936` bytes;
that read buffer is not the plateau's 524-byte writer overlap.
The fixture keeps the separate 4,096-byte drain allowance through population
`2048/2-1=1023` and pins the plateau for larger spilled books.
This definition budget excludes the journal event slots and spill buffers
reserved before the drain, and does not bound arbitrary request strings or
nested payloads.

`test_journal_resident_cap` samples residency after every command in a single
unretired 18,000-event window, requires a peak no greater than `2048+32`,
and reads expired original definitions back from spilled events. Its receipt
and core-state oracles are constructed independently; a one-bar settlement
fixture separately checks 3,000 literal round-trip trades.

## Transactions, rollback and commit

One transaction is active at a time. `begin` snapshots the logical state
including handle allocation, and reads inside it include the pending edits.
`put`, `erase` and `commit` need an active transaction. `commit(durability)`
publishes a committed state only when it returns `ok`; `buffered` permits an
orderly close and reopen, `data` and `full` additionally call the backing's
durability hook, journal and data before publication. A memory backing does not
advertise durability: asking it for `data` or `full` fails the commit.

A failed `put`, `erase` or `commit` leaves the transaction active and
rollback-only. From then on only `rollback` is accepted; `begin`, `put`, `erase`,
`commit` and every read, walk and digest return `programming_error` and change
nothing. `rollback` restores every pre-begin byte, handle, count, allocation
counter and the exact iteration order, however many pages were evicted or
spilled meanwhile, and the space the aborted transaction used stays reusable:
repeated begin, put and rollback cycles do not grow the backing without bound.
A mutation attempted from inside a read, comparison or source callback returns
`programming_error` with no effect at all; it does not poison the transaction.

Destroying a store with an active transaction never commits it, so reopening
yields the last successful commit. A commit whose publication was torn or
refused by the backing publishes nothing.

## The no-refusal rule

`ScratchStatus` has no capacity member and the API has no `RunFailureCode`
conversion. A `put` never refuses because a resident slot is full: it spills to
the backing. A `put` that cannot be served at all, whether the cause is an
exhausted backing, a backing I/O failure, a failing comparator or source, a call
outside a transaction or an exhausted handle domain, returns `programming_error`
and leaves its output untouched. That is a defect in the caller or in the
provisioning, not a run failure and not a retry. The host provisions the
backing: a production backing extends or spills, and a fixed test backing is
sized by its test for the whole workload. The guard test fails if the closed
status vocabulary gains a member or if a contract header gains an exception,
a growing container or a heap call.

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
resolve logical bytes rather than cache pointers.

A visitor that returns `false` cancels the walk with `cancelled`, including on
the last record, and receives no further call. An error is never reported as
cancellation or as an empty stream. A visitor may issue nested reads, walks and
digests on the same store with disjoint fixed buffers; the outer views stay
valid. Views, sources and chunk bytes live only until their callback returns.

An owning-return compatibility wrapper may exist only as a non-flat test oracle.
It must not be used by a per-run or per-bar path, advertised as O(1), or
substituted for point, page or visitor reads by a consumer.

## Backing and charged I/O

A `ScratchBacking` is positional and page granular: pages are exactly one page
size, ids are below `scratch_no_page`, a read at or past the extent is
`not_found`, and a successful write extends the extent to cover the id and
zero-fills any gap. A failed write may have changed its target, which is why a
transaction must protect previously committed pages. No operation throws.
Backing exhaustion is `io_error`, never a capacity status. Typed arena
exhaustion is the one exhaustion case carried as `report_sink_failed` for the
owner to rethrow. `programming_error` is reserved for caller contract violations.

The production command backing uses an explicitly attached `ScratchArena`
when present and otherwise retains its unlinked disk-file implementation.
Command-store bytes scale with the strategy's LIVE commands/exits/lots:
pending orders, open lots and dependency fan-out, not window length or trade
count. Each store's capacity uses the exact ratio `kCommandScratchArenaShare`
(f, provisionally 1/16 from M1) of the attached arena, with a one-chunk floor.
At open, K = max(chunk, floor_to_chunk(f × `ScratchArena::stats().bytes_supplied`)),
using the arena's 1 MiB chunks. Each store has at least one chunk of capacity.
The complete K/chunk directory is reserved once at open; writes do not allocate
a directory.
Crossing K is typed `arena_exhausted` before claiming any chunks,
at every arena size including the default and largest hosting rung.
The run byte budget is reserved before growth and released on
failed growth or destruction. Arena mode supports `buffered` sync; `data` and
`full` return `programming_error`.

An arena backing also exposes its original `ReportScratchIoError` through
`carried_failure()`. The store retains that diagnostic across successful
rollback I/O, clearing it on a new transaction. Owners rethrow the original
exception before applying their existing status-only error mapping. In
particular, arena exhaustion remains `report_sink_failed` with reason
`arena_exhausted`, store `command`, and the allocator's locked snapshot of
`high_water_bytes`, `arena_bytes` and `claimed_bytes`.
`needed_bytes_lower_bound` is the ARENA size that can satisfy this store:
ceil(chunkCeil(required store extent after the refused write) / f), using exact
integer arithmetic and saturating at INT64_MAX, the failure-argument transport
maximum. It is not the store's own shortfall. The hosting layer re-places a run
on this typed failure; a need beyond its largest arena continues to raise the
same failure until the host stops re-placement. A separate command byte-budget
refusal retains its existing status-only `io_error`.

Every attempted `read_page`, `write_page` and `sync` is reported once to the
`ScratchIoObserver`, errors and refused arguments included, with the backing's
kind, the page id (the sentinel for a sync), the requested byte count and the
status. That includes journal and rollback traffic and memory-backed traffic, so
the charge is the charged boundary: the store's own counters in `stats` equal
what the observer saw.

`FixedMemoryScratchBacking` is the in-process test backing over caller-owned
bytes with no allocation, file or growth. It supports `buffered` only. The
backings for `disk`, `memfd` and `tmpfs` are written against the same base class;
choosing among them is the host's decision at construction.

## Reference definition

`include/pineforge/detail/scratch_store_impl.hpp` defines `CommandScratchStore`, and
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
page count of at most 16 Mi, records below 2^61 bytes and handle numbers below
2^62; index nodes are not merged after erasures; cell pages never return to the
page pool; the journal pool keeps as many pages as the largest transaction so
far touched; and the interior nodes of the handle table are not reclaimed, so
the backing grows by well under one byte per handle ever issued even when the
live set is bounded.

## Statuses

| Status | Meaning in this API |
| --- | --- |
| `ok` | The call succeeded. |
| `end` | An ordered seek found no record; the output handle is `scratch_no_handle`. |
| `cancelled` | A visitor returned `false`. |
| `not_found` | An absent handle, a page past the extent, or nothing to reopen. |
| `programming_error` | Contract misuse or a request the host did not provision for. |
| `io_error` | The medium failed. |
| `corrupt` | Stored bytes failed validation. |

`put` and `erase` report every failure as `programming_error`; an `erase` of an
absent handle is `not_found` and is not a failure. `commit` reports the failing
backing call's own status. Reads return a backing `io_error` or `corrupt`
unchanged, and a comparator's failure unchanged.

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
   `programming_error` to a capacity or retry code.
9. Use `test_memory` only in tests, and commit with `buffered` there.
10. Add a status, a field or a header only additively and announce it.

## Fixtures

Each row names the clause, the fixture function that holds it and the failure
before the contract holds. Against the frozen base the three headers are absent,
so every body fails to compile with the missing-header diagnostic; against the
first published declarations alone the store fixture compiles and fails at the
link of `CommandScratchStore`, which they declare and do not define.

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
* Crash consistency beyond the ordering of the durability hooks.
* Sharing one backing between stores, and any concurrent use.
