# B: trusted scratch service, fallback to ScratchArena

Design handoff for core-cr150-l22b / CX-SINK-BDESIGN, 2026-10-20.
Base: `a35a64c23611a3eae9c1fe5ddc3d2ae3995b40ce`.
REL170 keeps the strategy child's `RLIMIT_FSIZE=0`. D, the pre-sized
shared-mapping arena, remains the first implementation. B moves random-access
scratch I/O to one trusted process outside that jail if D fails the real
engine/jail/box proof. This document specifies future implementation work;
no implementation, timing measurement, or verification result is claimed.

## 1. Scope, source evidence, and all 11 scratch stores

The common byte interface is `pineforge::detail::ScratchFile`:
`open(prefix)`, `write(offset,data,len)`, `read(offset,data,len) const`,
`clear() noexcept`, directory selection and move ownership. Its existing
error is `ScratchIoError`, carrying `report_sink_failed`
(`include/pineforge/detail/scratch_file.hpp:22-26`); its extent guard and
exact positional I/O are at
`include/pineforge/detail/scratch_file.hpp:78-108`.
File, arena and service are three backends of this one owner.

**Source provenance matters.** The base above does not yet contain
`src/report_scratch.cpp`, `src/metrics_spill_curve.cpp`, or the L18 spill.
Their actual implementations were read in the sibling worktrees below.
The source labels in the census refer to these specific snapshots, not to
nonexistent files at the base. All other repository citations refer to the
base. These external citations are kept in this evidence block so repository
anchor checking does not mistake a sibling draft for a local source.

```text
Evidence root E = /Users/haoliangwen/.local/state/pineforge-app/platform/evidence/
E/JAIL-FLAGS-FOR-CORE.md:17          300,000 ms deadline ceiling; CPU limit 2520 s
E/JAIL-FLAGS-FOR-CORE.md:22,33-39    single kept fd today; AF_UNIX stream; recvmsg denied
E/JAIL-FLAGS-FOR-CORE.md:55-57       FSIZE 0; sockets/pipes work; residual filesystem access
E/JAIL-FLAGS-FOR-CORE.md:61-71       required kept-fd set; instance accounting; concurrency 1
E/SEAL-PROBE-RESULT.md:11           arena child used equivalent rlimits, not changed pf-jail
E/SEAL-PROBE-RESULT.md:21,37,41     P3 RAM charge; C11 hole punch; C15 mapping/AS charge
E/SEAL-PROBE-RESULT.md:47-48,56     pipe proof; existing jail rejects memfd; remaining box proof

Lane root L = /Users/haoliangwen/.local/state/pineforge-top-core/lanes/
S = L/core-cr150-l22s/brief.md:10-23
    fd 5, arena attach/env, <=32 MiB windows, facade and ensure_size requirement
W = L/core-cr150-l6b4/wt/
W/src/report_scratch.cpp:88-129     own O_TMPFILE / mkstemp+unlink creation
W/src/report_scratch.cpp:145-170    positional writes/reads; truncate
W/src/report_scratch.cpp:173-237    pending batch, append, replace_last, flush, region
W/src/report_scratch.cpp:239-296    exact reads and fixed window refills
W/src/report_scratch.cpp:299-320    Debug prefix replay
W/src/report_scratch.hpp:17-19      65,536-byte budget / sizeof(point)
M = L/core-cr150-l7c/wt/
M/src/metrics_spill_curve.cpp:23-25,75-125  2,730-point cache; positional callback
M/src/metrics_spill_curve.cpp:44-70        raw descriptor binding to replace
M/include/pineforge/spill_curve_reader.hpp:29  callback/context point reader
M/src/engine_metrics.cpp:54-75,323-415     five full walks plus endpoint reads
P = L/core-cr150-l18/wt/
P/src/source/pine_adapter.cpp:28020-28028  preserve_position_tail_before_seal hook
P/src/source/pine_position_tail_binding.hpp:35-55  preserve before ledger retirement
P/src/source/pine_position_tail.cpp:38-75  <=64 KiB writes; reverse read window
P/src/source/pine_position_tail.cpp:84-108 id + 30-byte footer; publish after both writes
P/src/source/pine_position_tail.cpp:121-155 reverse footer walk; 256-byte id compares
P/src/source/pine_position_tail.hpp:19-20  window=65,536; footer=30
H = L/core-cr150-l17b/wt/
H/src/native_execution_consumer.cpp:12523-12535,12571-12578
    selected M+1 points, same ScratchCurveWriter, presented tail appended then flushed
H/src/native_execution_consumer.cpp:13055-13071
    selected emission rereads one 2,730-point batch
```

Here “11” counts persistent **backing-store roles**, not eleven independent
algorithms: curve (1), position tail (1), AVL tables (2), three pair tables
with data and index files (6), successor roots (1). SpillCurve reads the
curve; selected capture uses the same writer class. A selected capture can
own an additional curve instance while an ordinary curve is alive. A pair
rehash owns a temporary `next` index. Neither is omitted from quota or the
32-open-store limit below.

For the numerical examples let `N=8,000,000` script bars, `T=400,000`
trades, and `T/N=0.05`. Let `K=32` identifier bytes **only for the planning
example**, `P=T` placement rows folded into each minima store, `X=T` distinct
partial-exit keys and `R=T` replacement successors. These extra equalities
are workload assumptions, not consequences of the LARGE shape. `h` means
visited AVL levels; `p` means hash probes. Operation sizes below describe
the existing logical I/O; every service frame is independently bounded.

| Store/user | Access pattern and byte sizes, with evidence | Rate at the planning shape; worst case and batching |
|---|---|---|
| 1. Equity curve, including selected hold-back, SpillCurve and the internal Debug reader | Append 24-byte points; occasional positional replacement of the final point. W flushes up to `2,730*24=65,520` bytes, not 65,536. A partial flush can be 24 bytes. W `replace_last` changes pending RAM, or brings the last disk row back into that batch; it performs no immediate read. W `region()` flushes. M and W refill one 65,520-byte window, with a final partial window. Point layout: `include/pineforge/pineforge.h:326-330`, size assertion `include/pineforge/metrics_stream.hpp:14`. | Ordinary append: 1 point/bar, 24 B/bar, 192,000,000 B total; `ceil(N/2730)=2,931` WRITE round trips, about 0.0003664/bar. Five metric passes: 14,655 READ refills plus a small number of endpoint misses (reserve 16), 960,000,000 payload bytes, about 0.001832 refills/bar. An adversarial random-index walk misses once per point and requests up to one whole window each time. Explicit `region()/flush()` per bar can force one small WRITE/bar; report reads/corrections are not rate-capped by this code. Selected has `M+1` points, even `M=0`, and one additional forward emission pass; use `ceil((M+1)/2730)`, with the same batching. It may need a second store; it does not need an RPC for each held point. Debug prefix replays add one pass **per invocation**, not a fixed five-pass allowance. |
| 2. L18 position tail | Append the identifier and a 30-byte binary footer in separate writes (P). Average example: 32 B + 30 B = 62 B/retained trade; individual writes are up to 64 KiB, splitting long ids. Reverse footer reads are 30 B; comparisons consume 256 B pieces from an existing 64 KiB backwards cache. Clear/reset drops the current position history. | If all T rows survive into one open-position tail: 24,800,000 B. Current short-id path has 2 WRITEs/trade = 800,000 calls, 0.1/bar. A same-store WRITEV combines id+footer into 1 acknowledged round trip/trade = 0.05/bar, preserving publish-after-both. One complete reverse scan needs about `ceil(24,800,000/65,536)=379` refills (allow boundary overlap). Worst per query scans T rows; q repeated percentage-exit queries cost q scans. `preserve` can process an entire seal prefix at once. Id length is checked against uint32, not 32 bytes, so write-call bound per row is `ceil(id_bytes/65536)+1`, and no constant scan rate/bar is proved. |
| 3. `cohorts_by_id_`, `DiskResidentIdTable<CohortFacts>` | AVL key lookup, positional node/header/key reads, 40-byte retired-value overwrites and parent-link rotations. Node = `56+40+K=128` B in the example. Links are seven uint64s, 56 B (`include/pineforge/detail/disk_id_table.hpp:26-30`); value read/write at `include/pineforge/detail/disk_id_table.hpp:85-94`. Value size is asserted at `src/source/pine_adapter.cpp:28168`. Eviction writes before erasing the resident row (`include/pineforge/detail/disk_resident_id_table.hpp:184-188`). | One new cohort/trade means 400,000 nodes, 51,200,001 B including null-offset byte. Example lookup `h=19`: 38 small reads (56 B and K B/level), plus a 40 B get; restoration also reads the key again (`include/pineforge/detail/disk_resident_id_table.hpp:200-204`). One lookup/bar would therefore mean at least 39 RPCs/bar under a naive byte mapping. Insert cost is derived below; recurrence/retirement writes fixed slots rather than appending duplicate ids. No cap of one cohort operation/bar or one distinct id/trade exists. Use a server-side bounded AVL operation, not one RPC/node; bulk iteration batches returned rows. |
| 4. `exit_rank_keys_`, `DiskIdTable<uint32_t>` | Same AVL implementation, 4-byte values; node = `60+K`, 92 B example. Unlike cohorts, this table is capped at **27 keys**, because the check is `size()>26` before insert (`src/source/pine_adapter.cpp:6372-6380`). Insertion-order enumeration and rank lookups: `src/source/pine_adapter.cpp:6420-6435`. | At most 27 new nodes/run (2,485 B example); not 400,000. A balanced height around 5 gives 10 link/key reads + a 4 B value read per hit; conservative height bound 8 gives <=17 calls/hit. Above the cap, note-key does no lookup I/O. Rank enumeration can still occur repeatedly; its invocation rate is not bounded by T. Batch each rank request's keys or the <=27-row walk. |
| 5. Placement `sequence_index_` data | `IdPairStore<uint64_t>` record = 40-byte header + single id, or header + both ids. Two key domains per folded placement (`include/pineforge/source/pine_adapter.hpp:878-883`). Example records: 72 B single, 104 B pair. Append 2 or 3 writes; an update/erase overwrites one 40 B header. Successful lookup reads the header during locate, compares strings in <=512 B pieces, then reads the header again (`include/pineforge/detail/id_pair_store.hpp:343-364`, `include/pineforge/detail/id_pair_store.hpp:423-433`). | P new folds yield at most 2P records, 176 P = 70,400,000 B example; 5 data writes/fold = 2,000,000 writes, 0.25/bar. A short single-key hit has 3 data reads (40,32,40 B), paired hit 4 (40,32,32,40 B). Repeated `min_assign` without improvement writes nothing. Improving minima performs lookup then assign lookup again; double these reads and add one header write. `command_sequence_for` folds every pending row before lookup (`src/source/pine_adapter.cpp:901-924`); one bar can trigger the whole pending population. Coalesce a complete lookup/min operation on the trusted side; raw WRITEV can at least join a new record's pieces. |
| 6. Placement `sequence_index_` index, including transient `next` | Random 16 B slots, linear probing at load <=1/2; doubling growth from 64 slots. Old-index scan uses 128 slots = 2,048 B per read. Each relocated occupied slot probes and writes the new file (`include/pineforge/detail/id_pair_store.hpp:368-420`). Fresh growth must provide real zero bytes. | For 2P=800,000 distinct records, S=2,097,152 slots = 32 MiB. Under **uniform hashing**, successful probes average <=1.5 and unsuccessful <=2.5 at half load; these are a model, not measurements or adversarial guarantees. Each new `min_assign` normally has an absent lookup plus an assign lookup: about 5 slot reads + 1 slot write/key, or 12 index I/Os/fold for two keys (4,800,000 calls, 0.6/bar), before growth. Last growth scans 1,048,576 old slots in 8,192 reads and moves 524,288 occupied slots; old 16 MiB and new 32 MiB coexist. Worst probe chain <=S, and all growth can fall on one bar. Server-side probe/rehash with bounded continuations avoids S socket round trips. |
| 7. Placement `erased_minima_` data | Same record layout and single+pair minima as store 5; `remember_erased` folds at `src/source/pine_adapter.cpp:852`; field `include/pineforge/source/pine_adapter.hpp:935`. | With P erased rows use the same 70,400,000 B and 5 data writes/fold example. Unlike the lazy sequence fold, this work is triggered by erasure; neither erasures/bar nor distinct erased pairs/T is capped. Use the same server operation and reference tests, not a second implementation. |
| 8. Placement `erased_minima_` index, including `next` | Same 16 B slots, probes and 2,048 B growth scan as store 6. | Same 32 MiB terminal index / additional 16 MiB old-index overlap at P=T. Same 12 modeled index I/Os/fold and <=S worst chain. `command_sequence_for` may query both minima stores, up to four key lookups in total (`src/source/pine_adapter.cpp:914-923`); batching those four reduces four round trips to one when the common helper collects them. |
| 9. `consumed_partial_exit_cycles_` data | Single-key `IdPairStore<int64_t>`. The key is `exit_id + separator + from_entry` (`src/source/pine_adapter.cpp:11212`); K here means the **combined** key length. Lookup at `src/source/pine_adapter.cpp:11224`; assign on consumption at `src/source/pine_adapter.cpp:24874`. 40 B header, K B key; fixed header overwrite on recurrence. | X=T distinct combined keys => 72 X = 28,800,000 B. New assign: two data writes/key = 800,000 writes, 0.1/bar; one successful lookup/key has three data reads, 1,200,000 reads = 0.15/bar. Recurring assignments overwrite 40 B, and key count is historical, not only current live keys. Many exit commands without trades can issue many lookups; no T-based rate bound. Share the pair-operation implementation. |
| 10. `consumed_partial_exit_cycles_` index, including `next` | Same hash slots and rehash as stores 6/8; this store normally uses direct assign, not `min_assign`'s preliminary lookup. | X=400,000 => 1,048,576 slots = 16 MiB. A direct new assign models 2.5 slot reads + 1 slot write; a successful lookup models 1.5 reads. Last growth scans 524,288 slots in 4,096 reads, relocating 262,144 records; old 8 MiB overlaps new 16 MiB. Worst probe rate <=S per operation plus growth; it is not “one small write/trade” in the worst case. |
| 11. Successor roots | 16 B `(successor,root)` rows; resident tail 64 rows = 1,024 B (`src/native_order.cpp:5908-5922`). Spill writes one complete 1,024 B tail before append (`src/native_order.cpp:5963-5987`). Old lookups binary-search positional 16 B reads (`src/native_order.cpp:5997-6035`); tail lookups stay in RAM. Raw creation/destruction at `src/native_order.cpp:5928-5960` must use the facade. | R=400,000 successors => 6,400,000 B including current RAM tail; after exactly R appends, 6,249 full spill WRITEs and 64 resident rows. Mean 1/64 WRITE/successor, about 0.000781/bar. Successful old-file lookup averages roughly 18 comparisons near this population, <=19 reads; a tail hit needs none. One old lookup/trade models 7.2M tiny RPCs, or 36 s at the unmeasured 5 us assumption below. R counts replacements, not completed trades, and is not bounded by T. One server-side binary search collapses that to one RPC/lookup; independent lookup keys can also batch. |

The modeled “uniform” pair probe counts require nonadversarial hashes.
With chosen collisions, probes can be linear in all historical distinct
keys, including dead keys; this is explicit in
`include/pineforge/detail/id_pair_store.hpp:366-388`. Strings also have no
small compile-time size cap. The service therefore enforces storage/work
bounds and supports streamed large keys; it does not assert that N and T
alone bound scratch or execution time.

For those short-id examples, mean existing data-write call sizes are
about 65,512 B for a complete curve flush sequence, 31 B for L18's two
writes, 35.2 B for a minima fold's five data writes, and 36 B for a new
consumed-cycle record's two writes. A pair single-key hit averages
112/3 B across its three data reads; a paired hit averages 36 B across
four. AVL search reads alternate 56 B links and 32 B keys, averaging
44 B before the value read. Index slot operations are exactly 16 B
(growth scan 2,048 B); roots reads are 16 B and spills 1,024 B.
These are derived operation-weighted means at K=32, not measured
distributions. Worst raw AVL/IdPair key writes/reads can contain the whole
caller string; no small source bound exists. B splits them into <=65,536 B
frames. IdPair comparisons already split into <=512 B reads; L18 already
splits writes into <=65,536 B. Event counts above translate to per-second
rates by dividing by actual run time: consuming the whole 300 s ceiling
would require 26,666.7 bars/s and 1,333.3 trades/s.

The example's steady bytes are approximately 527,888,566 B: 192,000,000
curve + 24,800,000 tail + 51,200,001 cohorts + 2,485 ranks +
2*(70,400,000 + 33,554,432) minima + (28,800,000 + 16,777,216)
cycles + 6,400,000 roots. Counting the whole roots tail is conservative.
An additional selected curve adds up to 192,000,024 B for M=N; retaining all
three old rehash indexes adds at most another 40 MiB in this example.
This is a sizing illustration, **not a worst-case admission formula**.

## 2. Transport, framing, and bounded back-pressure

Use an inherited `socketpair(AF_UNIX, SOCK_STREAM, 0)`, one connection/run.
Proposed strategy fd is **6**, supplied as `PF_SCRATCH_SERVICE_FD=6`;
fd 3 remains selected phase receipts, fd 4 sink stream, fd 5 is reserved
for D's arena and absent in B mode. Each endpoint has exactly one owner
after spawn. No listening socket, pathname socket, reconnect, SCM_RIGHTS,
`recvmsg`, or network call is needed. The evidence block records why this
transport is permitted: AF_UNIX sockets and ordinary send/recv survive the
filter, while `recvmsg` is denied. Two inherited pipes can carry the same
bytes but require two kept fds; they are not the selected implementation.

Use one mutex for the connection, held from request construction through
complete response validation; at most **one request is in flight**.
Existing single-thread use of each ScratchFile is unchanged. The shared
connection serializes different stores. This bounds memory under a slow
server; it deliberately gives no asynchronous queue or growing backlog.

All integers in the **48-byte** header are unsigned little-endian. Encode
fields explicitly, never send a C++ struct's padding or host byte order.

| Byte offset | Width | Field |
|---|---|---|
| 0 | 4 | Magic `0x31534650` (bytes `PFS1`) |
| 4 | 2 | Version = 1 |
| 6 | 2 | Operation code |
| 8 | 4 | Flags: bit 0=response; all other bits zero |
| 12 | 4 | Status; zero on requests |
| 16 | 4 | Store id; zero for connection operations |
| 20 | 4 | Payload byte count |
| 24 | 8 | Logical byte offset; zero if unused |
| 32 | 8 | Logical length; zero if unused |
| 40 | 8 | Request id, starting at 1, strictly increasing |

Maximum payload is **65,536 B**, maximum complete frame **65,584 B**.
For READ, logical length is requested bytes, while request payload is empty.
For WRITE it equals payload bytes. Neither exceeds 65,536. Larger facade
calls split at frame boundaries with checked arithmetic; exact byte
semantics survive that split. Responses echo op, store, request id, offset
and length, except OPEN returns its assigned store id. Error/MORE responses
retain those header fields and use the bounded error/continuation payload
specified below. Validate the header before reading a declared payload;
an oversized/malformed header closes the connection without allocating or
draining an attacker-selected length.

Set both owned endpoints O_NONBLOCK before serving/attaching, so a single
kernel send/recv cannot defeat the timeout. This is a timed blocking API
implemented by nonblocking syscalls plus poll, not asynchronous queuing.
`send_all`/`recv_exact` advance pointers after positive partial transfers;
retry EINTR without replaying prior bytes. Handle EAGAIN/EWOULDBLOCK by
polling the fd until the operation's absolute monotonic deadline. The
logical behavior is blocking send/receive, including when the inherited
endpoint is nonblocking. Use `send(..., MSG_NOSIGNAL)` on Linux
(`SO_NOSIGPIPE` on hosts needing it), not a process-wide SIGPIPE change.
Zero send, EOF before a complete header/payload, EPIPE or ECONNRESET poisons
the session and throws ScratchIoError. The library never tries to create a
regular file in response.

Set both endpoints' requested SO_SNDBUF/SO_RCVBUF to 64 KiB in the wrapper;
record actual values (Linux commonly reports double). Reject actual buffer
limits above 128 KiB each for this profile. Kernel socket accounting is
additional instance memory, not child heap: reserve **1 MiB/connection**
for queues/overhead and prove the actual charge on the box. At one
outstanding bounded frame there is no data-sized userspace queue.

## 3. Protocol, dependent operations, ordering, and errors

### Byte operations and scatter lists

| Op | Name | Exact request and successful reply |
|---|---|---|
| 0 | HELLO | First request only; store/offset/length/status=0 and empty payload, with normal magic/version/request id. Reply payload: `u64 quota_bytes, u32 max_stores=32, u32 max_payload=65536, u32 request_timeout_ms=5000, u32 reserved=0, u64 deadline_monotonic_ms`. A version/capability mismatch refuses attach. The client learns the wrapper's absolute deadline from this 32-byte reply. |
| 1 | OPEN | Store=offset=0, length=prefix bytes, payload 1..64 ASCII letters/digits/`_`/`-`. Prefix is diagnostic only, never a path. Reply: new nonzero store id, no payload; store starts empty. |
| 2 | WRITE | Store, offset, length, exactly length payload bytes. Reply empty after exact positional write. Gaps read as zeros and growth is quota-reserved before I/O. |
| 3 | READ | Store, offset, length; empty request payload. Reply exactly length bytes, or typed EIO if any requested byte is beyond logical EOF. A hole **within** EOF is zero. Zero-length read/write checks the active store and representable extent, succeeds without an EOF check, and never grows EOF/quota. |
| 4 | CLEAR | Store, other fields zero, empty payload. Close/release the store and its quota. Repeated CLEAR of an already issued id succeeds. Its next `open(prefix)` obtains a new id. |
| 5 | READV | Payload: `u32 count, u32 zero`, then count descriptors `{u64 offset,u32 length,u32 zero}`. Same header store, header offset=0, header length=sum(length). Count 1..128, descriptor payload and total returned bytes each <=65,536. Reply concatenates exact byte ranges in list order. |
| 6 | WRITEV | Same list followed by concatenated bytes. Descriptor bytes + data <=65,536; length=sum(data lengths). Apply in list order, last overlap wins. Reply empty only after every write succeeds. |
| 7 | ENSURE_SIZE | Empty payload, offset=0, length=new minimum EOF. Grow only, zero-filled semantics; never shrink live data. This is the wire implementation of l22s's `ensure_size(n)`/IdPairScratchFile resize need, not a child ftruncate. |
| 8 | BARRIER | Connection op with zero fields/payload; empty reply confirms all previous operations succeeded. The child also checks local sticky errors before sending it. Call before emitting a success trailer/report. |
| 9 | CONTINUE | Store/offset/length=0; payload is the 8-byte continuation token from MORE. Reply is the completed original operation's payload (the result envelope for an index op), carried under op 9 and this new request id. The client retains the original operation context for validation. |

For READV validate every nonzero extent/EOF before reading; for WRITEV
reserve the rounded extension to the maximum of the old EOF and every
nonzero range end, once per store, before writing. Do not sum overlapping
ranges as independent growth. A subsequent OS I/O error may have written
a prefix; the whole run fails, so no rollback log or misleading “retry as
absent key” is allowed. A success acknowledgement means visible to later
requests on this connection, not durable across host loss. No fsync is
required for scratch correctness.

### Choose complete storage operations for the dependent indexes

**READV/WRITEV alone do not fix the AVL problem.** At each level the next
node's address depends on bytes just read. NODE_GET/NODE_PUT still leave
one round trip/level and rotation. B therefore includes fixed server-side
AVL, pair-table and successor-search operations; scatter lists handle
independent reads and record pieces.

The four core byte methods keep their signatures. Add one **internal**
storage-operation dispatcher beside them, behind ScratchFile, for the
shared `DiskIdTable`/`IdPairStore` helpers and roots index. Its concrete
contract is a bounded `execute_index(op, request_bytes, response_bytes)`
on an internal `ScratchIoAccess` friend/access class, using the same 65,536-byte limits;
it is not a PF_API export and is not exposed to source/strategy callers.
In file/arena mode it executes the same operation locally over ScratchFile;
in B it sends the operation below. Application users do not inspect an fd,
environment variable or backend tag. This is real additional B work, not a
claim that merely replacing pread with RPC gives acceptable performance.
It must not block l22s's byte-facade freeze.

The same internal access class supplies
`writev(ScratchFile&, const ScratchWriteSpan*, std::size_t count)`,
where a span is `{std::uint64_t offset; const void* data; std::size_t length;}`.
It validates/splits into the bounded WRITEV format in B; in file/D mode
it invokes the ordinary writes in list order. This is the common L18
id+footer helper; callers retain their buffers until it returns. No
deferred write or early acknowledgement is introduced. An analogous
bounded readv helper uses destination spans. Both keep the four-method
byte API intact and add no backend branch to application callers.

Factor the existing table algorithms into a shared local executor and call
that executor from the trusted server with a forced file backend. Preserve
node layout, full byte-string comparisons, insertion order, offsets,
parent links, live flags and exact integer values. The server must use
streaming key comparison rather than allocating `std::string(key_size)`.
Keep the original public table methods; their common implementation calls
the dispatcher. Pair grouping can be a shared helper used by `fold_sequence`
and `command_sequence_for`; the source adapter gets no B-only branch.

The index request envelope, all little-endian, is:
`u32 subop, u32 flags, u64 arg0, u64 arg1, u32 key_count, u32 value_bytes`
(32 B), then zero, one or two **24 B** key descriptors, `value_bytes`
literal value bytes, then optional inline key bytes. `value_bytes` is
4 or 40 only for AVL INSERT/SET and is zero for all other requests.
PAIR's value travels in arg0 instead. A key descriptor is
`{u32 source_store,u32 reserved=0,u64 offset,u64 length}`.
Store zero denotes inline bytes at offset in the frame payload; a nonzero
store denotes a validated range of an ordinary byte store. Combined inline
keys must fit after all fixed fields. For larger keys, the backend uploads
them in bounded WRITEs to at most two temporary stores and sends references;
clear these after the operation. They count against quota and store count.
Do not invent a 32 B or 64 KiB semantic key limit: current code accepts
larger ids, and L18 permits up to uint32 length. The server compares/hashes
referenced keys in 512 B chunks. Key-source stores cannot alias a table's
data/index stores and stay immutable until that operation completes.

| Op | Family, subops and state contract |
|---|---|
| 16 | AVL: 0=BIND, 1=FIND, 2=INSERT, 3=GET, 4=SET, 5=SCAN_INSERTED, 6=SCAN_SORTED, 7=KEY, 8=NEXT_SORTED, 9=NEXT_INSERTED, 10=FIRST_SORTED. BIND is on an empty store; arg0 is the fixed record width, restricted initially to 4 or 40, while flags/key_count/value_bytes/arg1=0. FIND/INSERT carry one key; GET/SET/KEY/NEXT carry node offset in arg0; SET/INSERT carry the exact fixed value. Scan carries the next node offset in arg0 (zero means start), arg1=max rows 1..128. Reply state includes root/first/last/end/count. FIND returns found+node offset+fixed value; INSERT also returns inserted and the existing/new value; GET returns value; scan returns bounded rows and next cursor. KEY returns one scan-format row for that node; NEXT/FIRST return result0=offset (zero=end). Unused args/flags must be zero. |
| 17 | PAIR: 0=BIND, 1=LOOKUP, 2=ASSIGN, 3=MIN_ASSIGN, 4=ERASE, 5=SCAN, 6=REHASH. Header store is data id; BIND arg0=index id, both stores empty, arg1=0 for uint64 minima or 1 for int64 cycles; flags/key_count/value_bytes=0. Other key operations carry one/two keys; flag bit 0 distinguishes a pair from a single key even when the second string is empty. Assign/min value is arg0 as raw uint64 bits (signed cycles preserve bits); MIN compares using the bound signedness. SCAN arg0 is next data offset, arg1=max rows 1..128; zero starts the first scan. Reply state includes data_bytes/slots/used/live_size and current index store id. Rehash builds a zero-filled `next`, publishes only after complete placement, releases old index, and returns the replacement id. Auto-growth is part of ASSIGN/MIN_ASSIGN; callers do not separately recreate it. Unused args/flags must be zero. |
| 18 | ROOT_FIND: subop=0; header store=roots, arg0=successor, arg1=disk_rows. Require length `16*disk_rows` within EOF and validate each row read exactly as the existing index. Return found+root. Existing 64-row child tail and 1,024 B spill batch remain. |
| 19 | INDEX_BATCH: header store/offset/length=0; payload `u32 count,u32 zero`, then count entries `{u32 op,u32 store,u32 payload_bytes,u32 zero}` followed immediately by that entry's envelope/keys/value/inline bytes, repeated in order. Count 1..128, all bytes/results must fit a frame. Allow AVL FIND/INSERT/GET/SET, PAIR LOOKUP/ASSIGN/MIN_ASSIGN/ERASE and ROOT_FIND only. All inline key offsets are in the outer payload address space and must stay inside their entry's inline-key area. Reply is `u32 count,u32 zero` followed by `{u32 result_bytes,u32 zero}` and that entry's result, repeated in order; reserve the maximum result size before execution. Use for two minima updates or up to four independent minima lookups. |

Every completed index result starts with **64 B**:
`u32 result_flags, u32 value_bytes, u64 result0, u64 result1,
u64 state0, u64 state1, u64 state2, u64 state3, u64 state4`.
Bit 0=found, bit 1=inserted/changed; others zero. AVL uses result0=node,
state0..4=root/first/last/end/count; ROOT_FIND uses result0=root and zero
state; PAIR uses result0=value bits, result1=current index id,
state0..3=data_bytes/slots/used/live_size and state4=0. Any value bytes follow.
Scans use result0=next node/data offset (zero means completed),
result1=row count and append rows
as `{u64 node_or_data_offset,u64 first_length,u64 second_length,
u32 row_flags,u32 value_length}`, value and key bytes. AVL has second_length=0.
Pair row_flags bit 0 means paired. Stop before a row would exceed the frame;
if a single row cannot fit, return its lengths/value plus byte-store
references in a one-row response (row_flags bit 31, two key descriptors
instead of inline key bytes; absent second key has length zero).
References are valid until mutation/CLEAR. The caller streams that key through READ;
no table-sized key list is made resident. Success/error flags apply to the
whole operation; a failed batch never returns partially successful values.

All index-family frame headers have offset/length=0; their suboperation
envelopes supply arguments. Each response's `value_bytes` counts only
immediately appended fixed value bytes, not scan rows; PAIR values are
already in result0, so value_bytes=0. KEY uses result0=that node and
result1=1, with one row and no iterator advance. Non-scan/KEY rows are absent.
For a cohort `find(key)`, pass that already-owned key into `load` with
the returned value, avoiding the old extra `index_.key(at)`/get reads.
For iterators that can perform nested lookups, request one scan row and
copy its key/value into the existing pinned current-row owner before any
other operation; never retain references into the shared response buffer.
Larger scan batches are for consumers that finish consuming the frame
before nested operations; no per-iterator frame allocation is permitted.

Once BIND claims stores, raw READ remains available for bounded inspection
and streamed scan keys, but raw WRITE/WRITEV/ENSURE_SIZE is refused on those
stores; only the executor mutates them. This prevents stale server metadata.
Child root/count/offset fields are updated only after a complete validated
reply. The common local executor returns the same metadata in file/arena
mode. After PAIR growth, the internal owner adopts the server-assigned
replacement index id from the reply without sending another OPEN; the
released old id is never reused. CLEAR on any bound data/index/AVL member
releases its group; CLEAR on an
already released index id remains harmless. `clear() noexcept` behavior
and sticky failure handling below apply to both.

Limit each service turn to **4,096 positional I/O actions or 65,536 key
bytes processed**, whichever comes first. This is a work quantum, not a
logical-operation size limit. Return status MORE with an opaque uint64
token; keep one fixed continuation state, including at most 128 AVL
ancestor offsets, key cursors, batch position and rehash cursors. Only the
matching CONTINUE or connection closure is accepted until completion.
MORE has exactly one 8-byte token, changed on every yield and checked
against the currently expected token. Retain the original request buffer
and any accumulated batch reply across yields. Read/write CONTINUE and MORE
through a fixed 56-byte control buffer included in the metadata budget;
do not overwrite retained inline keys or partial results. The same mechanism
can resume a fragmented raw I/O operation; its final response retains the
original operation's payload semantics under op 9.
Client-side batching that exceeds a result frame is split before send.
Complete a READ/READV into the bounded response buffer **before** sending
an OK header, so a backing-read error can still be returned as typed IO.

Validate all node extents, heights 1..128 and links before dereference;
abort a path exceeding 128 links. Bound pair probes by current slots, bound
scans by count/EOF and growth by quota/checked doubling. Continuations
retain loop counters: restarting them must not reset a work/cycle bound.
Fresh pair indexes retain the code's sparse-zero semantics
(`include/pineforge/detail/id_pair_store.hpp:410`).

### Why this cost is necessary

Naive cohort FIND reads links and key once/level
(`include/pineforge/detail/disk_id_table.hpp:75-81`): `2h` calls, then a
get. At 400k keys, use h=19 as a representative depth; h<=28 is a
conservative AVL bound, not a measured mean. A new insert additionally
does three new-node writes, up to two read/write link pairs, and rebalance.
Each rebalance level does <=7 I/Os: row, up to two child heights, write,
up to two balance heights, parent reread. At most two rotations add <=27
I/Os including the child-balance test. Thus a conservative short-key
insert budget is `2h+7+7h+27 = 9h+34`: 205 at h=19, <=286 at h=28
(`include/pineforge/detail/disk_id_table.hpp:95-136`,
`include/pineforge/detail/disk_id_table.hpp:181-225`). Actual averages
depend on hits and null children; these are costing bounds per operation,
not observed call counts.

Assume **5 microseconds per small local socketpair round trip**, and also
evaluate **20 microseconds** under scheduling/contention. Both are
**UNMEASURED planning assumptions**, excluding server filesystem time,
large payload copy time, and the engine's own CPU time.

| Workload | Naive byte RPC | Transport time at 5 us / 20 us | Chosen service operation |
|---|---|---|---|
| One AVL find+get/bar | 39 RPC/bar, 312M/run; <=57 RPC/bar using h=28 | 1,560 s / 6,240 s | FIND returning value: 1 RPC/bar, 8M/run, 40 s / 160 s, plus real local tree I/O |
| Cohort find restoring a retired resident row each bar | `2h+3` calls: 41/bar at h=19, <=59/bar at h=28; load rereads links/key/value | 1,640 s / 6,560 s at h=19 | Same 1 FIND/bar, with caller-owned lookup key and returned value |
| One new cohort/trade | Up to 205 RPC/trade in representative-depth budget, 82M/run, 10.25/bar | 410 s / 1,640 s | INSERT: normally 1 RPC/trade, 400k/run, 2 s / 8 s, plus real local insert I/O |
| One old roots lookup/trade | About 18 RPC/trade, 7.2M/run, 0.9/bar | 36 s / 144 s | ROOT_FIND: 400k/run, 2 s / 8 s |
| Five curve passes + append | About 17,586 window RPC/run, not 48M point RPC | 0.088 s / 0.352 s, excluding about 1.15 GB of payload movement | Keep existing windows/batches |

FIND in the cost table must include its fixed value in the reply, so the
common table helper can satisfy an immediately following get from that
reply rather than incur a second RPC. This is one result-sized cache, not
a growing node cache. Invalidate it on any mutation or unrelated operation.
Batched FINDs likewise return their values; the normal fixed result buffer
contains them until the next operation.

The APP run ceiling is **300 s wall time**, not its 2,520 s CPU rlimit
(evidence E). Budget **<=60 s extra scratch time** for a workload whose
existing file-mode run takes <=240 s; use actual remaining deadline when
the baseline is slower. At 5 us, 60 s buys at most 12M small RPCs before
disk/copy cost; at 20 us only 3M. A 400k-trade event example with one
cohort insert+find, two minima folds (each one two-key batch), one four-key
command-sequence batch, one partial lookup+assign, one roots lookup and
one tail append/trade is 9T=3.6M RPCs, plus ~24k curve/root-spill requests:
about 18.1 s / 72.5 s transport alone. The latter already misses the 60 s
allowance. One lookup/bar or many exit calls/bar changes this budget
materially. The box must record operation counts and total wall time on
real LARGE traces; neither batching nor N/T alone proves the deadline.

### Ordering, idempotence, and typed refusal

Execute requests in order and acknowledge complete visibility before the
next request. Repeating an identical positional WRITE without intervening
mutation is naturally idempotent; READ and grow-only ENSURE_SIZE are also
idempotent. CLEAR is idempotent for any previously issued id. OPEN allocates
a fresh id and is not replayed. INSERT returns existing on duplicate keys,
but that does not authorize replay of arbitrary mutations/batches.

There is **no automatic RPC retry, reconnect, or crash recovery**. Request
ids detect duplicate/out-of-order replies; they are not a promise of
exactly-once recovery. Reject a duplicate/decreasing request id and fail
before uint64 wrap. Store ids increase monotonically and are never reused;
keep only the next-id counter plus <=32 active entries. Refuse before
uint32 wrap; an already-issued inactive id can be recognized without
remembering every historical id.

| Status | Meaning | Child mapping |
|---|---|---|
| 0 | OK | Publish the complete result |
| 1 | EXHAUSTED: quota, store limit, offset/id exhaustion, ENOSPC/EDQUOT | `ScratchIoError(EFBIG, "scratch service exhausted")`; include cause in diagnostics |
| 2 | IO: short regular-file read/write, EIO or other storage failure | `ScratchIoError(EIO, "scratch service I/O")` |
| 3 | PROTOCOL: framing, unknown op, invalid store, bad metadata/token | `ScratchIoError(EPROTO, "scratch service protocol")` |
| 4 | TIMEOUT: no complete operation within its deadline | `ScratchIoError(ETIMEDOUT, "scratch service timeout")` |
| 5 | MORE: bounded continuation required | Internal continuation, not successful publication |

Error payload is exactly 16 B:
`u32 cause, u32 errno_hint, u32 failed_batch_index, u32 zero`.
Cause values: 1=quota, 2=store-count, 3=offset/id overflow,
4=backing-full, 5=backing-I/O, 6=malformed-frame, 7=bad-store/token,
8=bad-index-metadata, 9=deadline; remaining values reserved.
Use failed_batch_index=UINT32_MAX outside a batch; hints never change the
typed classification. EOF/EPIPE/ECONNRESET map to ScratchIoError with that
local errno (EOF uses ECONNRESET). All these errors carry
`RunFailureCode::report_sink_failed`; they must cross the existing catcher,
not become an absent id, partial curve, memory fallback or success trailer.
The typed inheritance already exists in the facade, whereas current
IdPairStoreError has equivalent RunFailureInfo and the raw roots index
currently throws uncoded system_error
(`include/pineforge/detail/id_pair_store.hpp:31-35`,
`src/native_order.cpp:5978-5980`). The common ports normalize these paths.

`clear()` and destructors remain noexcept. On transport/cleanup failure,
record a connection-wide sticky error and close the endpoint; a later
operation or pre-success BARRIER throws that typed error. Cleanup cannot
silently erase it. The wrapper also requires successful child, scratch
service **and** finalizer exit before accepting/publishing the report;
a finalizer trailer alone is insufficient after a service failure.

## 4. Trusted process, quota, lifecycle, and memory

The same root wrapper that spawns the finalizer spawns **one**
`pineforge-scratch-serve` per run, outside the strategy jail and its FSIZE
limit. Proposed argv (fd 6 here is the service's own peer endpoint):

```text
pineforge-scratch-serve --fd 6 --scratch-dir DIR --max-bytes 2147483648 \
  --request-timeout-ms 5000 --deadline-monotonic-ms D
```

`D` is the wrapper's absolute CLOCK_MONOTONIC deadline in milliseconds.
`--max-bytes` is a positive admission-approved uint64, <=off_t max,
mandatory; **2 GiB** above is the initial LARGE profile proposal, not an
unbounded allocation or a claim all strategies fit. Smaller profiles use
their admitted quota. Refuse invalid/missing limits before serving. HELLO
returns the effective limits, which cannot be increased by the child.
Use exit 0 for clean EOF at a frame boundary without pending continuation;
2 setup/configuration failure; 3 protocol; 4 exhaustion; 5 storage I/O;
6 timeout. These are scratch-service exits, separate from finalizer exits.
After sending a terminal error, close and exit with its nonzero category.

APP's wrapper must add these concrete steps:

1. Admit scratch quota and reserve process/socket headroom. Create a root-owned
   0700 per-run directory, not uid-10001's writable work directory; umask 0077.
   Open that directory once. Service files use trusted `openat` names /
   O_TMPFILE, 0600, immediately unlinked; prefix bytes never select paths.
   No file descriptor referring to the directory/backing files enters the
   strategy child. The jail has no path namespace, so ownership is material.
2. Create the AF_UNIX stream pair, configure buffer caps, spawn the service,
   then the strategy with its endpoint as fd 6. The service inherits only its
   endpoint and wrapper-controlled diagnostics; the finalizer inherits no
   scratch endpoint. Close every parent/other-child duplicate promptly so
   child death really produces EOF. A small native spawn helper is needed
   if the APP launch API cannot create/map a real socketpair; do not silently
   substitute a TCP socket or depend on child socket creation.
3. Extend `pf-jail` from one kept fd to a validated **set**: selected B keeps
   `{3,4,6}`, ordinary B `{4,6}`. Validate fd 6 as AF_UNIX SOCK_STREAM using
   the existing domain/type checks; split close_range around the sorted set.
   Leave fd 5 closed in B. D uses its separately validated arena at 5.
   Preserve canary behavior: no extra inherited descriptors for canary,
   transpile, compile or unrelated HPO children. No seccomp or rlimit
   relaxation is required (evidence E).
4. Pass only `PF_SCRATCH_SERVICE_FD=6` for B, scrub arena env, and leave
   `--report-sink-fd 4` and stdout/stderr as contracted. Require HELLO to
   finish within 5,000 ms of child attach. Establish the service before any
   scratch user opens.
5. Supervise child/service/finalizer together. On timeout, OOM, child death or
   either trusted helper failing, terminate/reap the other participants,
   discard output and remove the run directory. The service sees EOF,
   closes all <=32 files, and releases their blocks even if CLEAR never ran.
   If the wrapper dies, peer EOF plus the service's absolute deadline ends
   the service; wrapper/reaper owns any directory removal left after SIGKILL.
   A success requires child=0, service=0, finalizer=0 and verified finalizer
   receipt. Only then expose the finalizer's private output as an artifact.

Quota is the sum of **4,096-byte-rounded logical EOF reservations** for
all live stores, including sparse holes, key uploads and old+new rehash
indexes. Round with checked arithmetic and reserve **before** write/resize;
this prevents sparse-offset attacks. Add an allocation-unit reserve per
live file if the mounted filesystem allocates blocks above 4 KiB; establish
that unit at deployment and refuse an unsupported filesystem profile.
An overwrite within reserved EOF consumes no new quota. CLEAR returns the
reservation. Raw writes are not append-only and may leave zero-filled gaps.
Real ENOSPC/EDQUOT still refuses even when logical quota remains. Never
reclaim an old rehash index before the complete replacement is published.

The service can use tmpfs or a genuine disk-backed mount:

- On tmpfs, bytes outside the child's RSS are still **O(live scratch) instance
  RAM**. Cloud Run's ordinary `/tmp` is in memory (evidence E). Subtract the
  full quota plus fixed helper/socket budget from instance admission; moving
  the writer out of the jail does not make those bytes free.
- On actual disk, scratch can exceed a fixed D arena and does not require
  O(data) nonreclaimable shmem. Disk is still finite and quota-bound.
  File page cache/dirty pages are charged to the instance and must be
  measured under pressure; they are reclaimable/writeback-backed, not
  “zero RAM charge.” The worker may use bounded writeback/fadvise, but
  neither fadvise nor this design promises a hard kernel-cache RSS limit.
  No existing Cloud Run disk mount is assumed.

Application-managed service memory has a hard **256 KiB/run** budget:
two 65,584 B frame buffers; <=32 store/group entries; <=128 scatter/batch
descriptors; one bounded continuation (including <=128 ancestor offsets);
512 B streaming key buffers and fixed record buffers. Descriptors and
continuation structures must fit within the remaining 130,976 B; enforce
static size assertions and no allocation from child counts. No in-memory
map of historical ids, table cache, whole-file mmap, queued work, or
unbounded log. Limit its thread stack to 1 MiB and count executable/libc/
allocator baseline separately in measured RSS. Per-run diagnostics are
bounded counters and one <=8 KiB terminal diagnostic, not per-request logs.
Bytes live in backing files; OS cache accounting remains as above.

## 5. Child RAM after the move

The service backend allocates two fixed 65,584 B frame buffers plus at most
**32 KiB** metadata/descriptor/continuation storage, totaling **163,936 B
(<161 KiB)**. One fixed index result can live in the response buffer until
the next request; no independent node/key cache is added. Streaming a
large caller buffer/key uses these frames repeatedly. Serialize threads
on this one budget; do not allocate two frames per ScratchFile or thread.
Kernel socket buffers are in the separate instance reserve in section 2.

Keep existing caller caches, rather than count them as a new O(data) buffer:
65,520 B curve writer batch; 65,520 B per active SpillCurve/internal
curve reader; 65,536 B position-tail reverse window; 1,024 B roots tail;
and the existing small stack comparison/rehash buffers (512 B pair key,
2,048 B slot scan, 256 B L18 comparison). Selected emission uses one
65,520 B stack batch (H). For one writer, one reader, one selected batch,
one tail window, roots and one set of those small temporaries, transport
plus these caches is **under 512 KiB**, before ordinary engine/live-state
allocations and library/stack baselines. Additional concurrent readers
each still cost their existing 64 KiB; do not claim a process-wide bound
on reader count from this storage design.

The additional backend memory is constant in N, T and distinct historical
ids. B adds no mapped scratch window. D's contract instead allows **<=32
MiB total mapped windows**, plus chunk allocator/table metadata proportional
to admitted arena capacity (S). Both preserve current live-state RAM.
C, an in-child RAM replacement, holds O(scratch bytes): even just the LARGE
curve is 183.1 MiB; the illustrative steady total is about 503.4 MiB before
containers/strings and selected capture. C does not meet the flat-in-history
child-memory objective.

## 6. Failure semantics and hostile-child limits

| Event | Required behavior |
|---|---|
| Service dies before/during request | Child sees EOF/EPIPE/reset, throws typed report_sink_failed, aborts success. Socket send cannot kill it with SIGPIPE. Wrapper reaps both peers and finalizer; no accepted artifact. |
| Service stops reading, short-writes, or is very slow | Finite socket buffers apply back-pressure. A request has a 5,000 ms **total** timeout, clamped to run deadline; partial progress does not restart it. The entire index operation, including all MORE continuations, shares that deadline. Timeout poisons the connection, typed ETIMEDOUT; wrapper enforces its own absolute run deadline even if the child is malicious. |
| EOF at clean frame boundary after child exit | Service closes all files and exits 0 if it has no pending work/error. Wrapper still needs child and finalizer success; EOF is not evidence that the strategy succeeded. |
| Partial backing I/O, quota hit, or failed rehash | Do not publish new logical sizes/roots or a partial replacement; fail the run. Already changed scratch need not be rolled back because there is no retry/recovery path. Finalizer gets no valid successful run from the wrapper. |
| Garbage header, absurd length, bad op/id/token | Reject before allocation/arithmetic/I/O. Terminal bounded error if framing permits, else close. No attempt to resynchronize an arbitrary stream. |
| Child requests huge offsets/holes, many stores, hash collisions or cyclic nodes | Validate `offset<=off_t_max`, `len<=off_t_max-offset`, logical EOF/quota, active stores<=32, frame<=65,584, scalar counts<=128. Charge holes at logical EOF. Validate every node/slot and retain bounded counters across continuations. I/O quantum and absolute timeout prevent an unbounded trusted-side loop. |
| Child opens/clears forever or dribbles a frame | Monotonic fixed-width ids, no retained closed-store history, a nonresetting frame/operation timeout, and the wrapper's run deadline bound CPU/time. Waiting for a frame's first byte is allowed until the run deadline; the 5 s frame timer starts at its first byte, so an idle service does not kill a child doing legitimate computation. No data-dependent logs. |
| Destruction/CLEAR fails | Noexcept owner records sticky session failure; BARRIER/success path and wrapper exit checks surface it. EOF cleanup does not depend on the child's cooperation. |

The service is trusted; a malicious service is out of scope. The child and
all supplied bytes remain untrusted: no raw pointers, client paths,
executable callbacks or uploaded programs cross the boundary. Its socket
can affect only that run's quota/files. Neither B nor D is a general ban on
empty-inode creation in the strategy's existing jail; the jail facts record
that residual surface. No new jail-hardening project is required here.

## 7. Implementation order, Codex executor hours, and green proof

### Integration with l22s's facade

After the S facade exists, add `ScratchServiceBackend` and explicit
`attach_from_environment()` at the same startup site selected by l22s,
before scratch opens. The process attachment decision is exact:

| Attachment inputs | Choice |
|---|---|
| Neither service nor arena env; no explicit attach | Existing file mode, unchanged |
| `PF_SCRATCH_ARENA_FD` only, valid arena attachment | D, following S's size/seal/window checks |
| `PF_SCRATCH_SERVICE_FD` only, valid AF_UNIX stream and HELLO | B |
| Both service and arena selectors, malformed fd, wrong type, missing peer/HELLO | Typed refusal; never choose an implicit priority or file fallback |

An embedding host can attach explicitly and remains unaffected if it never
does. The wrapper controls env; `set_directory` is ignored for service I/O,
but existing “different directory after open” lifecycle checks stay
consistent. ScratchFile move transfers its store handle once. `clear()`
invalidates the handle; later open creates a fresh store. Adopt/duplicate
fd ownership at attach consistently with S: the B backend should duplicate
the supplied fd CLOEXEC and own that duplicate, while the host closes its
original at its existing lifecycle boundary. The wrapper must ensure no
unused duplicate survives child termination.

Three common ports are prerequisites shared with D, not repeated B
implementations: replace report_sink::ScratchFile/raw descriptor reads
with detail::ScratchFile and its reader callback; compose IdPairScratchFile
over detail::ScratchFile plus grow-only ensure_size; port raw roots
creation/I/O to the facade. The current public `ScratchCurveRegion`
contains a real descriptor (`include/pineforge/metrics_stream.hpp:68-71`).
A service store id is **not** a descriptor: use M's existing
ScratchCurveReader callback/context path for B and D, retain the descriptor
constructor for legacy file consumers, and make owner lifetime explicit.
Report scratch `clear()` must clear/reopen the facade rather than merely
truncate a fake fd. These are actual dependency seams, not permissions to
edit another lane silently.

### Effort per store/user

Hours below are **active Codex executor hours**, including reading,
implementation and meaningful differential tests written/fixed. They are
estimates, not measured executor throughput. Each row prices B integration
after D's common facade ports; the one-time algorithm/transport work is
listed separately to avoid charging the same pair engine six times.

| Store/user | Hours | Derivation |
|---|---:|---|
| 1. Curve writer, SpillCurve, selected and Debug consumers | 2.0 | 0.75 callback/lifetime and clear/flush audit; 0.75 file/B binary point and selected M=0/window-edge comparison; 0.5 read/flush counts and failure propagation |
| 2. L18 tail | 1.5 | 0.5 WRITEV id+footer integration; 0.5 reverse-cache/oversized-id boundary cases; 0.5 failure-before-seal differential |
| 3. Cohort AVL | 2.0 | 1.0 40-byte value/FIND+GET and metadata integration; 0.75 recurrent/retire/restore and insertion-order comparison; 0.25 count tracing |
| 4. Rank AVL | 0.5 | Reuse AVL engine; 0.25 4-byte record/batch integration + 0.25 27-key cap/order comparison |
| 5. Sequence data | 0.5 | 0.25 single/pair minima binding + 0.25 update/read comparison |
| 6. Sequence index | 0.75 | 0.25 grouping/growth id handoff + 0.5 old/new overlap, zero slots and failed publish |
| 7. Erased data | 0.25 | Reuse pair engine; bind erasure and recurring-id comparison |
| 8. Erased index | 0.25 | Reuse growth tests; cover retained tombstone and second group |
| 9. Consumed-cycle data | 0.5 | 0.25 signed cycle bits/combined key + 0.25 suppression and recurrence comparison |
| 10. Consumed-cycle index | 0.25 | Reuse pair engine; third group quota/growth coverage |
| 11. Roots | 1.5 | 0.5 ROOT_FIND and 64-row split; 0.5 search/overflow equivalence; 0.5 EOF/typed-error path |
| **User integration subtotal** | **10.0** | Each of the eleven stores is accounted for; selected/SpillCurve are priced under store 1 |

| Shared implementation | Hours | Derivation |
|---|---:|---|
| Codec, ScratchServiceBackend and mode attachment | 5 | 2 fixed-frame codec/exact I/O/deadlines; 1 attach/ownership/mutex; 1 scatter and sticky failure/BARRIER; 1 malformed/fragmented-peer tests |
| `pineforge-scratch-serve`, files/quota/lifecycle | 4 | 1 parser/dispatch; 1 openat/anonymous files/EOF/quota; 1 bounded buffers/continuations/counters; 1 failure tests/CLI/wiring |
| Shared index executor and internal dispatch | 8 | 3 AVL refactor preserving bytes/streamed keys; 3 pair lookup/min/erase/zeroed rehash/group state; 1 roots search; 1 bounded scan/batch/large-key framing |
| Real-jail and fault-injection proof fixtures | 4 | 1 prlimit/fd harness; 1 independent reference traces across backends; 1 timeout/death/quota/protocol cases; 1 count/RSS/large-shape wiring |
| Integration/review fix allowance | 3 | Compile/link/anchor review and one supervisor lab failure round; not a promise to skip further fixes |
| **CORE shared subtotal** | **24** | |
| **CORE total** | **34 executor h** | 24 shared + 10 user integration; practical range **28–44 h** |
| APP wrapper/pf-jail work, additional | 4–6 | 1–2 socketpair/spawn/env/kept-set; 1 quota/admission/cleanup; 1–2 supervision/deadline/output gate; 1 real engine image/box hook-up |
| Common D facade ports if not yet delivered | 3–5 | Report/callback, IdPair resize composition and raw roots ports; shared prerequisite, not included twice |

Total staffed work is **38–40 executor h after common ports**, or **41–45 h**
if those ports are also missing, before queue time or repeated lab failures.
One lane doing the entire CORE implementation needs about 34 active hours;
this is not a same-hour finished fallback. It **can start the same hour**
without choosing transport, bounds, table semantics or wrapper behavior.

Implementation order / critical path:

1. Freeze byte codec/limits and attachment decision; build byte backend,
   server and reference trace tests first. APP implements kept-set/spawn in
   parallel against section 4. Common facade ports can proceed independently.
2. Factor/bind AVL and pair operations; these are mandatory for a performance
   proof. In parallel, another executor can finish curve/L18/roots integration
   and failure fixtures once the codec is fixed. The shared-index stage is
   the longest dependency; a byte-only demo is not the green LARGE proof.
3. Supervisor builds all targets and runs tests under `prlimit --fsize=0`,
   then the **real** modified pf-jail with uid 10001, --no-fork, actual --as,
   fd set and sink/finalizer path. Use the real engine image/16 GiB shape;
   E's seal probe alone does not prove this launch path.
4. Supervisor runs preflight, full release/debug C++ verification and full
   corpus verification, obtains the maintainer parity verdict, then runs
   the LARGE RSS/latency and cleanup proof.
   Match the exact final tree. This edit-only lane runs none of these.

Required proof assertions (implementation work, all future):

- Drive the same random write/read/overwrite/hole/clear trace through file,
  D and B against an independent byte-vector model. Force one-byte socket
  fragments, EINTR/nonblocking behavior and zero-length/boundary cases.
  Verify byte counts and logical EOF, not constants returned by a mock.
- Replay real cohort/rank/minima/partial-cycle/root sequences against
  independent map/vector reference state; compare values, sorted/insertion
  order and engine hashes/report bytes. Exercise >27 rank ids, recurring
  and colliding ids, pair-vs-single empty strings, growth `next` failure,
  and keys crossing 512 B/64 KiB/frame boundaries.
- Prove read/write under `prlimit --fsize=0` uses only inherited fd 6:
  strategy has no scratch regular fd and issues no data-bearing scratch
  open/pwrite/ftruncate. The trusted parent remains outside that limit.
  Test wrong-type fd, both env modes, missing service and jail-scrubbed fd.
- Kill either endpoint mid-header, mid-payload and during rehash; fill quota
  with a sparse high-offset request; exceed stores/frame/counts; supply
  bad pointers/cyclic node links; stall service and send a slow byte stream.
  Assert typed refusal, live child error handling, bounded timeout/memory,
  no accepted sink, and zero remaining trusted files/processes.
- Record actual RPC family counts, payload bytes, round-trip distributions,
  wall time, child RSS, service RSS and cgroup/shmem/cache counters for
  N=8M/T=400k and chosen-collision/high-command traces. Verify fixed buffer
  high water, D <=32 MiB mappings, and B's selected backing charge.
  A regression to per-node/per-point RPC must fail count assertions.

D costs less: no wire parser, service supervisor, index executor dispatch
or extra process; current users retain direct local random access with
bounded mappings. B offers actual disk growth beyond a pre-sized arena,
central quota enforcement and typed disk errors without child mmap/SIGBUS
exposure. On tmpfs it offers **no escape from instance scratch RAM cost**;
on real disk the cache still costs RAM and service I/O still costs time.

## 8. Decision table: D vs B vs C (RAM)

| Axis | D: pre-sized ScratchArena | B: trusted scratch service | C: child RAM scratch |
|---|---|---|---|
| Child RAM / address space | <=32 MiB mapped windows + existing caches + capacity-dependent chunk metadata; every mapping counts toward --as | <161 KiB backend frames/metadata + existing caches; no scratch mappings; example transport+caches <512 KiB | O(live scratch), curve alone 183.1 MiB at 8M bars; illustrative stores ~503.4 MiB before overhead |
| Instance RAM | Memfd/tmpfs pages O(touched scratch), proven charged by E/P3; regular-disk arena changes this to file-cache accounting | tmpfs O(scratch), same fundamental instance charge; actual disk avoids O(data) shmem but retains charged page cache/dirty pages plus helper/socket baseline | O(scratch) anonymous heap, generally not reclaimable as scratch file cache |
| Latency | Local loads/stores, window misses/mapping and allocator overhead; must measure | Local socket copy/scheduling + server filesystem I/O; 5 us/20 us are unmeasured. Byte-RPC AVL is disallowed for performance; complete index operations mandatory | Local RAM reads/writes until pressure/OOM; allocation/copy costs |
| Additional LOC, estimated C++17, not counted/measured | About 400–700 implementation + 400–700 tests, excluding shared user ports | About 1,500–2,200 implementation (codec/backend 350–500, server/quota 400–600, executor/dispatch 750–1,100) + 900–1,300 tests; APP about 150–250 | About 150–300 byte-backend + 200–400 tests, but O(data) growth remains |
| Main risk | Sizing before run, arena exhaustion, instance OOM/page fault, real wrapper integration; kernel primitives already probed on gen2 | Parser/hostile-child boundary, lifecycle/deadlock, timeout, disk availability, dependent-operation parity and latency | Violates flat child RAM objective; OOM at large/unbounded histories |
| Jail changes | Keep fd set including sink 4/arena 5 and optional receipt 3; validate regular fd, exact size and memfd seals | Keep fd set including sink 4/service 6 and optional receipt 3; reuse AF_UNIX stream validation; fd 5 closed | Sink fd-set work still needed, but no scratch fd. FSIZE remains 0 |
| Proof effort | Same byte/table/reference and full parity proof, plus real image/jail arena/seals/AS/instance pressure; seal probe is partial evidence | All D semantic comparisons plus framing/fragmentation/malformed input/service death/slow peers/quota/continuations/cleanup and measured RPC budget | Byte/table equivalence easy to compare; cannot pass a flat-in-history RSS claim |
| Effort from common facade | Smaller backend and user ports; B's 24 shared h of IPC/service/index work largely absent; D schedule belongs to l22s | CORE 34 h, APP 4–6 h; add 3–5 h if common ports absent; 28–44 h CORE uncertainty range | Cheap initial backend; no honest finite fix estimate for the flat-memory failure without changing options |
| Decision | **Keep as primary while real box proof is green** | **Fallback when D fails box/fit proof and B has measured latency + admitted backing/quota** | **Reject for the historical scratch flat-RAM acceptance** |

Activation does not require a new product decision: if CORE-TOP drops D,
assign the CORE and APP work in section 7, choose an actually available
backing mount and its admitted quota using section 4, and implement the
specified protocol. Mount availability, real operation rates and local
latency remain empirical proof inputs; none is disguised as a measured
result or a code-derived bound.
