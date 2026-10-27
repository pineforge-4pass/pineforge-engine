# Consumed partial-exit ordered fold (WL-PTAILFIX, O1W slab fold)

`PineExecutionAdapter::hash_state` retains its existing count followed by
`BrokerStateHashSink::s(key)` and `::i(cycle)` in `std::string` lexical order.
The consumed-cycle `IdPairStore` opts into an auxiliary ordered fold; the
ordinary minima stores do not. The table's hash index, record format, exact
key comparison, and first-seen disk iteration remain its lookup authority.

## Exact seed action

For any byte sequence S, let F be its action on a 64-bit incoming sink state.
One byte changes only the low eight bits before multiplication. Consequently,
modulo 2^64:

```
F(256*q + r) = F(r) + q * (F(256) - F(0)),   0 <= r < 256.
```

`BrokerStateByteFold` stores F(0)..F(256), 257 words / 2,056 bytes.
Concatenating X followed by Y replaces each stored X(r) with Y(X(r)). This
uses the incoming seed, including every higher bit, rather than a cached
digest for one seed. Construction calls the real `BrokerStateHashSink` byte
loop. String length and signed cycle value use its native byte spelling;
there is no format, domain, endian, count, or version change.

## Ordered disk structure and work

The auxiliary index is a compressed binary radix tree with buckets of at
most 16 original table offsets. Only interior branches store transforms.
End-of-key is the nine-bit symbol 0 and byte b is symbol b+1. Comparing these
symbols most-significant-bit first preserves unsigned byte order, including
prefixes, empty strings, NUL, 0x1f, and 0xff. A pivot stores its byte index and
bit mask separately, avoiding a key-length multiplication/overflow limit.

A new key that differs in a compressed prefix inserts a branch above the
existing subtree. A full bucket splits at the first differing symbol of its
least and greatest keys. Replacements and erase/reinsert retain original
table records and bucket offsets; an erased row contributes the identity
map. Ancestors compose the changed child with their unchanged sibling.

For maximum key length L, radix depth D is at most 9*(L+1), independent of
the number of keys. There is no growing RAM stack: the descent path lives in
the disk journal. Each update visits that path and folds at most 16 records
per leaf it reads. A conservative byte-operation bound is
O(257 * 16 * (L+16) * (D+1)), hence O(1) at fixed L. The existing hash-index
lookup and geometric growth retain their expected amortised bound.

Each hash folds the existing eight-byte count and applies the resident root
map: O(1), zero auxiliary file reads/writes and zero allocation. Ordinary
runs maintain the same root, so an unrecorded run's terminal hash also has
the same byte semantics.

## Incremental slab fold

The slab uses design (b), maintained transforms. Deferring the sort until a
hash request would put the work on the per-bar path. The scheduler calls
`scheduler_record_broker_hash` once per terminal script dispatch, including
its quiet carried-callback branch
(`src/source/pine_scheduler_native.cpp:639`,
`src/source/pine_scheduler_native.cpp:725`). With recording enabled this
calls `broker_state_hash` (`src/source/pine_strategy_host.cpp:3255`).
`on_native_bar` then refreshes the recorded row after the adapter's close
(`src/source/pine_strategy_host.cpp:934`); its `recording` guard
(`src/source/pine_strategy_host.cpp:950`) enables
`capture_script_continuation_hash`
(`src/source/pine_strategy_host.cpp:865`). Thus an ordinary recorded script
bar folds the table twice. Recorded magnifier sub-bars can refresh it too.
Terminal input updates also call `capture_script_continuation_hash`
(`src/source/pine_strategy_host.cpp:890`). Without recording, that refresh
does not call the broker hash; explicit broker/stream hash queries still do.
Each broker hash reaches `hash_host_extension`
(`src/engine_state_hash.cpp:129`); the adapter's `hash_state`
(`src/source/pine_state_hash.cpp:315`) calls exactly one `fold_sorted`
(`src/source/pine_state_hash.cpp:570`). A full-table lazy rebuild per dirty
bar is therefore unsuitable.

`IdPairSlabFold` (`include/pineforge/detail/id_pair_slab_fold.hpp:21`)
maintains a sorted roster of original record offsets, including dead rows,
partitioned into contiguous buckets. A binary search finds the changed row
or the new key's insertion point using the same length-aware byte comparison
as the former selection loop. A new key shifts roster offsets through a
fixed 512-byte buffer. Only the receiving bucket gains a row; later buckets
retain their rows and maps even though their roster positions shift.
An overflowing bucket splits into two contiguous halves. Erase and
resurrection retain the original offset and bucket membership.

Each bucket caches the disk fold's `BrokerStateByteFold` for its live rows.
The changed bucket, or the two halves of a split, is refolded using the
unchanged `s(key), i(value)` bytes. All bucket maps are then composed in
roster order into the resident root
(`include/pineforge/detail/id_pair_slab_fold.hpp:71`).
These are derived indexes and transforms, with no additional logical hash
leaf: concatenating the contiguous, sorted live subsequences is exactly the
old sorted live sequence. Dead rows contribute identity. The existing
257-seed composition law therefore proves equality for every state and
every incoming seed, including binary keys and prefixes. `fold_sorted`
still folds the live count and applies this root in O(1)
(`include/pineforge/detail/id_pair_store.hpp:363`).

### Fixed budget and publication

The existing consumed index region is exactly 16*Q bytes, with Q a power of
two and Q >= 2*K. Only the folded slab packs its hash slots as offsets;
the full hash is read from the existing record header, and probing still
compares complete keys (`include/pineforge/detail/id_pair_store.hpp:465`).
There is no change to data records, admission maxima, page spans or slab
allocation. The disk and ordinary non-folded slab slot layouts are unchanged.

Configuration (`include/pineforge/detail/id_pair_slab_fold.hpp:35`) chooses
w=4 bytes when the entire admitted data region fits UINT32_MAX; otherwise
w=8. The bucket maximum M is respectively 256 or 1,024. Split halves contain
at least M/2 ever-keys; no key is removed from the roster. Reserving
ceil(K/(M/2)) maps is sufficient, independently of mutation order or erasures.
A map record is 2,064 bytes: count plus the unchanged 2,056-byte transform.
The storage inside the index region is:

```
w*Q hash offsets + w*K sorted offsets + 2064*ceil(K/(M/2)) bucket bytes
```

For K <= M no bucket records are needed: the resident root is the sole map.
For K > M, Q is divisible by 256 (w=4) or 1,024 (w=8), so the ceiling term is
at most Q/256 or Q/1024. Total storage is at most 14.0625*Q or 14.015625*Q
bytes, both below the already admitted 16*Q. This also bounds each
configuration arithmetic intermediate; no new capacity limit is introduced.
At the default K=4,096, Q=8,192, w=4, the reserved storage is 115,200 bytes
inside the existing 131,072-byte index region.

The root and scalar layout state stay in the store; the slab update workspace
is 4,632 bytes, below the existing disk workspace allowance. The final store
static assertion still bounds the store plus update workspace and fixed
buffer/frame allowance to 32 KiB
(`include/pineforge/detail/id_pair_store.hpp:825`), excluding the separately
admitted, fixed slab. All slab access copies through `read_at`/`write_at`;
there are no retained payload pointers, allocations or file operations.

The existing read-only key/data admission still precedes the first write.
After admission, the roster and map writes fit spans that were made readable
at bind; they cannot introduce a capacity refusal. Slab publication uses the
prospective header while folding, then copies the admitted header and slot
(`include/pineforge/detail/id_pair_store.hpp:789`). A refused operation
changes no row, map, slot, count, root, byte or extent. The disk pending-write
and undo protocol remains the one described below. Move carries the roster
layout, bucket count and root with the slab owner. Clear zeros the same
regions and resets the root and bucket count while retaining configuration.

### Slab work bound

Let N be ever-keys, L the maximum key length, and B the bucket count.
Each write uses at most ceil(log2(N+1)) fold row comparisons. A new key
copies at most w*N roster bytes and at most B map records on a split;
replacements, erase and resurrection move no roster offsets. At most M+1
rows' bytes are folded, even on a split. The remaining fold work is B
compositions of 257 seed actions, with B <= ceil(K/(M/2)). Thus byte folding
is bounded by 257*(M+1)*(16+L), rather than 257*N*(16+L).
The existing hash lookup cost is separate; forced full-hash collisions
still require linear probing and complete-key comparisons.

At K=4,096, L=8, the conservative per-write bounds are 13 fold comparisons,
1,585,176 real sink byte steps, and 32 map compositions / 8,224 seed actions.
The old full-live selection loop makes N*N + N*(N-1)/2 = 25,163,776 row
comparisons, plus up to 25,264,128 byte steps at eight-byte keys, per write.
These are code-derived bounds, not timings or executed measurements.
No unchanged bucket's key or value bytes are folded during composition.
Even a smaller manifest's single-bucket rebuild is bounded by the fixed M.

## RAM, disk, and addressable population

For the disk backend, `IdPairStore`'s final static assertion bounds the
complete folded store plus the 8,840-byte update workspace and an
8,192-byte allowance for fixed reader
buffers, rehash buffers, headers and scalar frames to 32 KiB. There is no
per-distinct-key resident allocation. The explicit memory-reference fixture
is excluded from that production disk-backend bound.

Let N be distinct keys ever seen, B be bucket count, and J be the deepest
saved path (including its leaf). The fold adds exactly these logical bytes:

```
136*B + 2088*(B-1) + 2096*J     for a nonempty tree
```

The ordinary table still uses `40 + key_length` bytes per record, plus its
16-byte index slots at half load (32..64 bytes/key after warmup). At 8..16
records/bucket the fold's node cost is about 139..278 bytes/key, plus the
reusable journal; bucket occupancy is input-dependent. The conservative
degenerate bound is less than 4,320 extra bytes/key including the journal
(B <= N and J <= B). This is a storage tradeoff, not a claim of measured
occupancy. The new 10,000-key binary fixture asserts total table+fold storage
below 700 bytes/key and prints its actual logical bytes for lab readback.
Updates reuse nodes and reuse the journal after the deepest path is warmed.

A single transform file would impose an earlier `off_t` limit than the old
table. `IdPairFoldFile` instead uses fixed arrays of lazily opened segments
covering the old table's entire `(OFF_MAX/64)+1` distinct-key population.
On 64-bit hosts there are three possible leaf segments, 33 branch segments,
and 33 journal segments; normal populations open one of each. Offsets are
computed within a segment, rather than multiplying the total population by
the map size. Disk-byte accounting saturates at UINT64_MAX instead of
refusing a population whose combined files exceed that reporting range.
All segments use `IdPairScratchFile::open` and its existing scratch directory.

## Publication and failures

The files are anonymous per-run scratch, not crash-recovery storage. Before
any published radix node is modified, the complete old path and leaf are
written to the reusable disk journal. A journal failure changes no live
node. A subsequent failure leaves the previous resident root and table
visible. The next mutation restores the old node images before using the
tree; restoration can itself fail and be retried without invalidating
lookup, iteration, or the cached old root.

The table keeps one committed header and, for an append, one index slot as
fixed resident pending writes. Every header/slot read consults those values.
Both are flushed before either overlay is discarded or the next mutation
starts. Thus even a partial in-place table flush leaves the committed values
readable. A new record's strings are staged beyond the old logical end.
After radix IO succeeds, root, pending table writes, logical end and counts
are published without any further throwing work. A failed index growth
still discards only its incomplete replacement file.

Move carries the cached root, pending table writes, all segment descriptors
and any recovery journal. Clear closes them, restores the identity map and
retains the fixture backend choice and scratch-directory configuration.

## Tests and remote verification

`test_id_pair_ordered_fold` adds the following checks:

- The former enumerate/sort/relookup loop is the test-only oracle. It uses
  the actual `BrokerStateHashSink`; table contents are independently checked
  against a reference map at every mutation.
- Shuffled random binary sets, random replacements, erase/reinsert, prefixes,
  empty/NUL/0x1f/high-byte keys, 513/4,097-byte keys, extreme signed values,
  unrelated full-width seeds and all 256 low-byte residues.
- Move, reset, first-seen disk iteration, memory-fixture independence,
  10-to-10,000-key flat heap, allocation refusal during hashing, actual IO
  counts, bounded amortised mutation IO, and disk reuse.
- Short writes at each write boundary for append/index growth, long-key
  append, replacement and erase; a second failure during retry; move with
  recovery outstanding; equality of all old rows and their fold after faults.
- Fixed segmented-file capacity under a simulated small offset ceiling.
- The original 12-key folded slab fixture, including long keys, full hash
  collisions, refusal, first-seen iteration, move and clear.
- `slab_work_counters_observe_the_old_byte_loop`
  (`tests/test_id_pair_ordered_fold.cpp:285`) calibrates real comparisons and
  all 257 seeds' byte steps on one- and two-row buckets. The observation
  sites are shared `FoldReader::compare`, `compare_rows` and `append`;
  they observe the previous slab selection loop as well as the new path.
- `slab_work_bound_and_oracle_4096`
  (`tests/test_id_pair_ordered_fold.cpp:309`) requires 4,096 distinct live
  binary keys before asserting the per-write bounds, with and without
  forced full-hash collisions. It measures every insert, replace, erase,
  resurrection and mixed mutation; prohibits allocation during mutation and
  hashing; checks zero file IO and unchanged allocation identity; and covers
  move/clear with multiple cached buckets. Eight independent seeds at each
  checkpoint use the old enumerate/sort/relookup oracle on an independently
  mutated memory fixture, with a real-table sort/relookup at each completed
  phase. Iterated table contents must also equal an independent map.
- `slab_fold_layout_fits_every_index_tier`
  (`tests/test_id_pair_ordered_fold.cpp:485`) checks power-of-two boundaries
  through the largest admitted index tier without allocating those slabs,
  and checks actual wide-offset copies with high bits set.
- `slab_data_refusal_preserves_fold_and_index`
  (`tests/test_id_pair_ordered_fold.cpp:522`) checks data exhaustion with a
  key slot still available. This and the 4,096-key index exhaustion check
  compare every region's admitted bytes and extent, including cached maps.

The slab fail-before base is ba5bfdcfb2. With only its frozen headers, the
expected first diagnostic is the missing `id_pair_slab_fold.hpp` include;
after providing that standalone header, the missing `Store::FixtureFoldWork`.
Neither compiler diagnostic is work-bound evidence. To obtain the intended
runtime RED, retain that base's store algorithm, backport only the
`FixtureFoldWork` type/pointer,
fixture setter, comparison counters, and the `FoldReader` byte/compose
observation wrappers; overlay the new test source and the standalone slab
header needed to compile its layout/codec checks. Do not wire that header into
the base store or change its `publish_fold`, slots or binder. The calibrated
old loop must then fail at
`4096-key slab fold exceeds 13 row comparisons/write` after the full
population assertion. The independently observed byte bound is also exceeded.
No local compiler, build, test, preflight or corpus execution was performed
by this edit-only lane; the supervisor must obtain both RED and GREEN in lab.

No existing test assertions or the L18 fixture size/recording are changed.
Required remote checks, on the exact candidate tree, are preflight, docs,
full corpus parity, release and debug. In `lab remote` terminology:

```sh
lab remote submit --repo engine --worktree <candidate-checkout> \
  --steps preflight,docs,parity,release,debug --host auto --keep-going \
  --label core-cr150-o1w-slabfold
```

The C++ targets/CTest names to inspect explicitly are
`test_id_pair_ordered_fold`, `test_id_pair_store`,
`test_id_pair_store_ram`, `test_adapter_idpair_ram`, `test_scratch_slab_layout`,
`test_scratch_defaults`, `test_adapter_auxiliary_disk`, `test_position_tail_spill`,
`test_position_tail_scale_out`, and `test_position_tail_scale_out_100k`.
The full release/debug CTest logs must also show every recorded-hash consumer
passing and include the unchanged 100k row's elapsed time. The original
WL-PTAILFIX fixture targeted c15d224588, whose `IdPairStore` lacked the
ordered-fold template argument. The slab fixture instead uses the
ba5bfdcfb2 fail-before protocol above; neither expectation is a recorded run.

## Separate percentage-exit history walk (unchanged)

`PineExecutionAdapter::exit` still walks closed trades in reverse to
the position-start barrier, then continues through `PositionTailSpill` when
the resident walk reaches its base. Every qualifying percentage exit repeats
that walk, including rows whose incarnation no longer qualifies. Its work
is Theta(sum of history lengths), hence Theta(N^2) for an unbroken position.

The 100k feed has 400,004 bars and 100,001 qualifying exit calls. In its FIFO
shape the first exit closes one slice and every subsequent exit closes two.
Before call j+1 there are 2*j-1 closed rows (j>=1). Summing the preceding
histories gives `sum(j=1..100000, 2*j-1) = 10,000,000,000` row visits per
principal execution, or 20,000,000,000 across sink-off and sink-on. This is
a code-derived count, not a timing measurement. The streamed rows have
96-byte IDs plus 30-byte footers, about 1.26 TB of repeated window coverage
for its principal execution, apart from its small resident tail.

That remaining walk can still take hours after hashing is fixed. A future
fix must preserve position-start filtering, direction, live-incarnation
ancestry, FIFO/from_entry selection, the distinct margin-call rule, and the
reverse-order binary64 accumulation used for percentage reservation. A
reassociated sum can change quantities and trades. No such fix is included.
