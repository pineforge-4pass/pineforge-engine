# Consumed partial-exit ordered fold (WL-PTAILFIX)

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

## Ordered structure and work

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

## RAM, disk, and addressable population

`IdPairStore`'s final static assertion bounds the complete folded store plus
the 8,840-byte update workspace plus an 8,192-byte allowance for fixed reader
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

No existing test assertions or the L18 fixture size/recording are changed.
Required remote checks, on the exact candidate tree, are preflight, docs,
full corpus parity, release and debug. In `lab remote` terminology:

```sh
lab remote submit --repo engine --worktree <candidate-checkout> \
  --steps preflight,docs,parity,release,debug --host auto --keep-going \
  --label core-cr150-ptailfix
```

The C++ targets/CTest names to inspect explicitly are
`test_id_pair_ordered_fold`, `test_id_pair_store`,
`test_adapter_auxiliary_disk`, `test_position_tail_spill`,
`test_position_tail_scale_out`, and `test_position_tail_scale_out_100k`.
The full release/debug CTest logs must also show every recorded-hash consumer
passing and include the unchanged 100k row's elapsed time. The previous-header
fail-before compilation of the new unit remains required; its expected first
diagnostic is the additional `IdPairStore` template argument, not a recorded
run result.

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
