# Jailed invocation contract: sink stream and ScratchArena

CR-150 / 1.7.0, round 7, read against batch-C **`a2f3dfdb84`** on
2026-10-20. This replaces round 6's sibling-worktree descriptions. Engine
file:line references below are to this checkout. Source availability is
not an executed verification result; no build, test, checker, corpus
sweep or other repository program was run for this revision.

APP owns the root wrapper, jail, artifact publication and cleanup.
COMPUTE owns `planEngineRun()`, its default arena, class ladder, storage
placement and escalation. CORE supplies the engine interfaces and bound
calculator. **Required wrapper behavior** describes APP's handoff
obligations, not a claim that the wrapper already implements them.
Section 6 names current integration gaps.

The APP source inspected here is the read-only checkout
`/Users/haoliangwen/code/pineforge-app-wt-appabort`. An `APP` citation is
relative to that root. Historical census counts, sibling delivery dates
and old RSS slopes are not current sizing facts and are not repeated.
MiB means 1,048,576 bytes; GiB means 1,073,741,824 bytes. Runtime
diagnostics are observations, not proofs of a future request's maximum.
Below C=1,048,576 and `chunkCeil(x)=C*(x//C + (x%C != 0))`.

## 1. Invocation, descriptors and arena behavior

**Required wrapper behavior:** use one fresh child, sink stream and
private arena per run. Keep stdin/stdout/stderr at 0/1/2. On selected
runs preserve the existing phase socket at 3; supply the sink write
endpoint at 4 and the private O_RDWR arena at 5. The finalizer receives
only the sink's blocking read peer and its own receipt write endpoint,
in addition to stdio. It receives neither the arena nor a sink write end.

The sink ABI has no default fd: `pf_report_sink_fd_v1` contains
struct_size, version=1, fd and copied run_id
(`include/pineforge/report_sink.h:57`). run_json accepts a caller-selected
fd from 3 through INT_MAX, rejecting a shared phase/sink fd, simultaneous
path sink, --bench and validation-only fd execution
(`docker/run_json.py:5374`). The wrapper convention is
`--report-sink-fd 4`. The library borrows the endpoint without closing or
duplicating it (`include/pineforge/report_sink.h:40`); run_json owns
closing it (`docker/run_json.py:5490`).

The writer validates FIFO or socket type and SOCK_STREAM
(`src/report_sink_writer.cpp:316`). It does not check socket family;
AF_UNIX is the wrapper/ABI requirement. Socket sends use MSG_NOSIGNAL,
FIFO writes use write, EINTR retries, and EAGAIN waits in poll with a
fixed 64-KiB buffer (`src/report_sink_writer.cpp:265`,
`src/report_sink_writer.cpp:343`). FIFO hosts must ignore SIGPIPE
(`include/pineforge/report_sink.h:53`). This transport uses no MSG_OOB.

ScratchArena takes `PF_SCRATCH_ARENA_FD=5`,
`PF_SCRATCH_ARENA_BYTES=<positive decimal bytes>` and optional
`PF_SCRATCH_WINDOW_BYTES`; there is no backend-kind variable.
No FD variable makes attachment a no-op, even if size is present.
A supplied FD value other than decimal 5 is refused
(`src/scratch_arena.cpp:348`). Constants are one-MiB chunks, a default
32-MiB total mapping budget and inherited fd 5
(`include/pineforge/detail/scratch_arena.hpp:93`).

The arena owns a F_DUPFD_CLOEXEC duplicate; the supplied descriptor stays
caller-owned (`src/scratch_arena.cpp:77`). Attach requires a regular file
of exactly the supplied size. On Linux, F_GET_SEALS exactly F_SEAL_SEAL,
or EINVAL, selects regular_file; a mask containing GROW|SHRINK|SEAL
selects sealed_memfd. Other incomplete masks/errors fail arena_backing
(`src/scratch_arena.cpp:96`). Classification uses fstat/fcntl, not paths.
A tmpfs regular file still costs RAM; the enum does not certify placement.

**Required wrapper behavior:** ship sealed memfd. For a disk profile,
create a unique root-owned 0700 directory on the selected volume and a
root-owned mode-0600 O_RDWR file (named or O_TMPFILE); preallocate its full
extent before spawn. Memfd needs GROW|SHRINK|SEAL without WRITE.
Reserve real storage and retain exact size until the child stops.
These are trusted provisioning steps, not syscalls performed by attach.

Attach floors a non-chunk tail: `K=floor(A/C)` allocatable chunks, while
stats retain exact supplied A. It rejects zero chunks, bytes beyond
off_t, or more than UINT32_MAX chunks (`src/scratch_arena.cpp:141`).
For signed-64-bit off_t the largest accepted **whole-chunk** extent is
`UINT32_MAX*C = 4,503,599,626,321,920 B`. The calculator uses the same
ceiling (`scripts/arena_bound.py:19`); this is representation, not a
request-derived growth bound. Metadata/filesystem/provider limits may
bind earlier. Use the returned whole-chunk size when certified.

Host page size must divide C. The window budget rounds down to pages
and must retain at least one page. With rounded Wn,
`window_bytes=min(Wn,C)` and
`slots=min(floor(Wn/window_bytes), K*ceil(C/window_bytes))`
(`src/scratch_arena.cpp:148`). Eviction unmaps before replacement
(`src/scratch_arena.cpp:198`). Only windows consume child virtual
mapping space; full memfd backing is an instance RAM reservation.

Arena writes use MAP_SHARED stores, not write/pwrite or growth ftruncate.
Release's PUNCH_HOLE|KEEP_SIZE is best-effort; claimed chunks are zeroed
through mappings independently (`src/scratch_arena.cpp:246`,
`src/scratch_arena.cpp:265`). New mappings recheck size
(`src/scratch_arena.cpp:207`); cached-window return precedes that check
(`src/scratch_arena.cpp:185`). Short pread refuses, with size
revalidation, rather than retrying its tail (`src/scratch_arena.cpp:432`).
Cached mappings and the fstat-to-access race remain exposed to a
concurrent regular-file shrink: SIGBUS can kill the process, without a
typed exception. Successful attach ignores SIGXFSZ process-wide;
detach does not restore it (`src/scratch_arena.cpp:327`).

Attachment is explicit. `detail::ScratchFile::open` uses an already
attached arena or legacy file mode; it does not attach from environment
(`include/pineforge/detail/scratch_file.hpp:70`). Merely supplying fd 5
and environment does not make this head's engine startup attach it.

## 2. Pre-start bounds and placement

Call pure `arena_bound(facts)` (`scripts/arena_bound.py:427`).
[arena-bound.md](arena-bound.md) specifies its proof obligations.
It returns four terms, but **only arena, ohlcv and finalizerStream enter
the sizing verdict** (`scripts/arena_bound.py:462`).
childPrivate is reported independently; unknown RSS/AS never puts it in
unboundedTerms. The matching statement is explicit at
`docs/design/arena-bound.md:59`.

“Bounded=false” in discussion means no certified finite bound is
available. The emitted spelling is **unbounded=true**, with top-level
reason and unresolved sizing terms; there is no emitted bounded key.
Unknown bytes are null. Arena reasons distinguish UNBOUNDED (proved
unbounded growth), UNCERTIFIED (absent/unsupported proof) and
PLATFORM_RANGE (finite arithmetic beyond representation).
Calculator integers are checked uint64 values, not boolean/float facts
(`scripts/arena_bound.py:36`); a TypeScript port must retain exact
integer arithmetic rather than silently round them through Number.
Malformed facts raise FactsError; missing proofs/checked overflow use
explicit unavailable paths (`scripts/arena_bound.py:150`,
`scripts/arena_bound.py:456`). Unknown is not mathematical infinity.

**Placement policy:** attempt 0 starts with COMPUTE's default arena;
certified facts may tighten it. An unresolved sizing term means no
tightening, **not largest rung + disk** (`docs/design/arena-bound.md:81`).
Typed arena_exhausted re-places on the next trusted ladder rung, with
escalation cap 2, raises an alert and is never charged to the user
(`docs/design/arena-bound.md:88`). The calculator does no routing/retry.
Default size, rung sizes, backend selection and handling after that cap
are **DECISION FOR COMPUTE/APP**.

### Formula and certification boundary

S bounds all script bars including pre-roll; W bounds selected script
bars, `0<=W<=S`; selected is integer 0/1. Pre-start timestamp/timeframe/
session geometry must establish these, not an assumed ratio for arbitrary
gapped/calendar inputs. The implemented formulas are:

```text
T = (S*fills.perBar + fills.fixed)*fills.rowsPerFill + fills.fixedRows
D = |unique(ids.literals union {empty})| + S*ids.freshPerBar + ids.fixedFresh
L = ids.maxBytes                                      # UTF-8 bytes/component
Q = D*D                                              # cartesian proof
  or min(D*D, S*pairKeys.perBar + pairKeys.fixed)       # opportunity proof
R = S*successors.perBar + successors.fixed
E = selected ? W+1 : S
```

Sources are `scripts/arena_bound.py:179`, `scripts/arena_bound.py:195`,
`scripts/arena_bound.py:207`, `scripts/arena_bound.py:228` and
`scripts/arena_bound.py:250`. T includes partial-close durable slices;
Q covers distinct-ever keys including erased/unaccepted opportunities;
R includes issued unfilled replacements. Neither Q=T nor R=S is a rule.

Let `J=min(D,27)`, `slots(0)=0`, otherwise
`slots(k)=max(64,nextPowerOfTwo(2*k))`.
`indexPeak(0)=0`, `indexPeak(k)=1024` for `1<=k<=32`, otherwise
`indexPeak(k)=24*slots(k)` (`scripts/arena_bound.py:96`).
The eleven-user model is:

```text
B = 24*E + (30+L)*T + (96+L)*D + 1[D>0]
    + (60+L)*J + 1[J>0]
    + 2*((40+L)*D + (40+2*L)*Q + indexPeak(D+Q))
    + (41+2*L)*Q + indexPeak(Q) + 16*R
A = chunkCeil(B) + 14*C
```

`scripts/arena_bound.py:118` implements contributions;
`scripts/arena_bound.py:258` sums/rounds. Fourteen chunks cover eleven
persistent regions and three replacement indexes, not OHLCV, sink bytes
or private heap.

**Certification on this head:** checked arithmetic, modeled layouts,
rounding and representation rules are implemented. A finite result is
conditional on trusted, complete population/geometry facts; a source
string is not itself a validated proof. The general compiler/lowering
facts producer is absent (`docs/design/arena-bound.md:8`,
`docs/design/arena-bound.md:210`). T/D/Q/L/R cannot be synthesized from
bars, a prior run, pyramiding or examples. Pine projection deliberately
leaves max_open_lots unbounded (`src/source/pine_adapter.cpp:2861`).
The entry-rank cutoff bounds that one population to 27
(`src/source/pine_adapter.cpp:6375`), not other stores or ID lengths.
A finite modeled answer does not prove every storage user is ported.

OHLCV needs a complete physical feed inventory: separately page-round
`48*bars+overheadBytes` for each Bar buffer and `8*bars` for each
close/value column, then apply resident/AS copy counts
(`scripts/arena_bound.py:279`). Borrowing adds no second payload; each
mapping still counts in AS. Include owned capacities and CSV conversion
copies. Selected chart input still rejects --ohlcv-bin and loads CSV
(`docker/run_json.py:4493`); a binary-payload formula does not bound its
loader peak.

The full v1 stream bound U includes header, trailer and every LF,
separately from arena and finalizer heap (`scripts/arena_bound.py:328`):

```text
nT = T + openAtEndRows; nE = E
nO = outputs ? S*outputRowsPerBar + outputRowsFixed : 0
nV = outputs ? S*eventsPerBar + eventsFixed : 0
nK = outputs
nX = trace ? S*tracePerBar + traceFixed : 0
nH = hashes ? S*hashesPerBar + hashesFixed : 0
U = 2*1048576 + sum(nTag*maxLineBytesTag), tags E,T,O,V,K,X,H
```

Line maxima including LF are E=154, H=25,
T=min(1048576,675+12*L+6*commentBytes),
O=min(1048576,60+65*seriesSlots),
K=min(1048576,6+65*constantSlots),
V=min(1048576,239+6*messageBytes),
X=min(1048576,110+6*traceNameBytes). Disabled O/K sizes are zero.
Missing string maxima use the full-line ceiling; missing count proofs
cannot invent populations (`scripts/arena_bound.py:355`). Open-at-end
rows add to T; output/trace/loop/callback effects need their own proofs.

Private RSS and AS are independent optional observations/proofs.
measured describes RSS only. Unknown values or missing certifications
leave that informational term unbounded (`scripts/arena_bound.py:392`).
The calculator installs no historical RSS or fixed allowance. This does
not block tightening when the three sizing terms are certified
(`docs/design/arena-bound.md:518`). That document's policy keeps private
growth under RLIMIT_AS, with APP's memory_limit charging policy. That
policy label is not an engine sink reason or new class: the engine
catalog calls bad_alloc out_of_memory (`docker/run_failure_codes.json:495`),
class resource (`src/run_failure_registry.inc:547`). A killed process
need not emit typed JSON. **DECISION FOR COMPUTE/APP:** private-AS
ceiling, post-port RSS/AS evidence and lifecycle classification.

### APP reservations

Existing LARGE is 16 GiB, with 10-GiB whole-child AS ceiling, 2-GiB raw
report cap, 512-MiB stored cap and 4-GiB input spool cap
(`APP packages/backtest-core/src/memory-admission.ts:45`).
SMALL retains a 3.25-GiB ceiling, 256-MiB raw and 64-MiB stored caps
(`APP packages/backtest-core/src/memory-admission.ts:31`).
The current budget subtracts measured input spools, both report caps,
at least 256 MiB service headroom and 64 MiB slack
(`APP packages/backtest-core/src/memory-admission.ts:66`).

Keep both report reservations until APP changes their collector,
publication and gzip consumers together. The uploader still creates a
separate .gz artifact and caps compressed bytes
(`APP apps/engine/src/report-upload.ts:34`). Its small metadata summary
check (`APP apps/engine/src/report-upload.ts:30`) is not a run_json fd
stdout cap. The whole provisional sink is **extra storage**.

Reserve full A in instance RAM for memfd/other tmpfs, and full U in RAM
if sink staging is tmpfs. Disk profiles reserve the corresponding file
extents instead; mappings/cache still cost resources. Child AS includes
OHLCV maps, Wn and private virtual allocations, not full A. Do not
subtract A from the whole-child AS ceiling. Account trusted finalizer/
converter runtime and overlapping artifacts separately.
**DECISION FOR COMPUTE/APP:** backend selection, service-allowance
overlap, usable disk/quota and changed report pipeline. A finite arena
does not certify private RAM, finalizer heap or complete placement.

## 3. Finalizer, failures and stdout

The current jailed v1 command is:

```text
pineforge-sink-finalize --in-fd N --out - --max-bytes U --receipt-fd M
    [--run-id ID]
```

`docker/pineforge_sink_finalize.py` accepts the same arguments.
For --out -, U is **required and positive**, M must differ from stdout
and N, and missing/invalid U is usage exit 2 without a receipt
(`docker/pineforge_sink_finalize.py:372`). The wrapper establishes N's
blocking read-peer type/direction. Stdout mode ignores spool-dir and
creates no output file. It forwards exact verified v1 lines; root stores
the whole provisional stream outside the finalizer file-size jail.
Required placement is a separate uid-10002 finalizer jail with FSIZE=0
and no-fork, distinct from the uid-10001 child. The CLI creates no jail.

SinkReader verifies schema/LF/line limit, SHA-256, counts and EOF after
trailer (`docker/pf_sink.py:280`). The finalizer additionally checks
run ID, nondecreasing E times, contiguous T numbers from 1, O rows from
0, and at most one K (`docker/pineforge_sink_finalize.py:274`).
Record types may interleave. V1 integers fit ±(2^53-1), u64 fields are
nonnegative, and hash64 is sixteen lowercase hex digits
(`docker/pf_sink.py:209`). Records are positional arrays.

Exit outcomes:

- **0:** complete stream verified, delivered/flushed and receipt written.
- **2:** usage, no receipt.
- **3:** malformed header/record/line/schema/order/run ID.
- **4:** trailer/count/hash mismatch or bytes after trailer.
- **5:** input/output/receipt I/O, including stdout EPIPE and file-mode
  spool/rename/cleanup.
- **6:** missing trailer, including empty input or unterminated fragment.
  An overlong fragment remains line error 3.
- **7:** internal error.
- **8:** stream exceeds U; receipt reason sink_exhausted.

EOF/trailer classification is at `docker/pineforge_sink_finalize.py:155`;
I/O/internal/cleanup/flush/receipt handling is at
`docker/pineforge_sink_finalize.py:410`. Overflow is distinct from I/O
(`docker/pineforge_sink_finalize.py:229`,
`docker/pineforge_sink_finalize.py:245`). Both modes read at most U+1
bytes. Exact-U valid stream plus EOF succeeds. Exit 8 reads exactly
U+1 and never forwards the excess byte. Stdout mode delivers the allowed
prefix to the reader before a separate one-byte overflow probe, so the
stop point/prefix counts do not depend on read splitting. File-mode
overflow precedes verification and has zero counts. Earlier grammar,
line or I/O failure can stop first. Cleanup may replace overflow with
exit 5; only final status 8 adds exhaustion receipt fields
(`docker/pineforge_sink_finalize.py:443`).

Receipt counts are reader-folded counts; absent types are zero.
sink_bytes is bytes read including header/trailer/LFs, not bytes
forwarded. Failure sha256 is empty. Receipt follows the last stdout
write and successful flush; a failed flush has no receipt. Failed
receipt write returns 5. All stdout remains provisional until exit 0
**and** complete receipt. Receipt is outside the sink hash.

Unjailed --out PATH differs: optional nonnegative cap, remove old PATH,
receive into mode-0600 spool, fsync/close, verify, os.replace; spool/PATH
must share a filesystem. Handled failures remove owned files
(`docker/pineforge_sink_finalize.py:394`); SIGKILL cannot run cleanup.
run_json rejects --report-sink-format 2 (`docker/run_json.py:5393`);
this module implements no v2 extension. **V1/v2 formats are frozen:
no arena or cap field may be added to a sink trailer.**

### Exact failure vocabulary

report_sink_failed is **engine_fault, retryable=false** for all reasons
(`docker/run_failure_codes.json:997`, `src/run_failure_registry.inc:564`).
There is no cap-refusal class. The seven reasons
(`docker/run_failure_codes.json:961`) are:

- arena_exhausted: locked complete-grow capacity refusal, with the
  captured arena size, current claimed bytes, high-water, additional
  whole-chunk demand and store.
- arena_io: arena syscall/map/read/logical-bounds failures. EFBIG/EPERM
  alone does not prove allocator exhaustion.
- arena_backing: attach/configuration/backing validation.
- scratch_io: legacy file scratch operations.
- sink_io: sink setup/open/write without classified closed peer.
- sink_peer_closed: closed stream peer. run_json fd-setup OSError maps
  EPIPE/ECONNRESET/ENOTCONN (`docker/run_json.py:5459`); C++ writer
  translators still have section 6's payload gap.
- sink_exhausted: **finalizer exit 8 plus receipt** supplies
  sink_bytes=cap_bytes+1 and cap_bytes=U. APP maps it to report_sink_failed,
  refunds, and COMPUTE re-places with **2*U**
  (`docker/run_failure_codes.json:998`). Finalizer emits exit/receipt,
  not a run_json failure document. A child seeing the closed peer has
  its separate peer-closure outcome.

All eight catalog args are optional for compatibility, not proof of
conditional producer rules. Arena exhaustion has its five size/store
details plus reason; sink exhaustion has reason/sink_bytes/cap_bytes.
Other reasons carry no sizes/store. Store literals are curve,
position_tail, cohort, exit_rank, sequence_index, sequence_index_idx,
erased_minima, erased_minima_idx, partial_cycles, partial_cycles_idx,
successor_roots and unknown (`docker/run_failure_codes.json:978`).
They are physical roles, not paths; the facade defaults to unknown.

The allocator throws ScratchIoError(EFBIG) under its capacity lock,
before any partial claim for a refused grow (`src/scratch_arena.cpp:217`,
`src/scratch_arena.cpp:277`). High-water is peak simultaneously claimed
chunk bytes since attach; claimed is current chunk bytes at refusal;
arena is exact supplied bytes including tail. Demand is
`max(1,ceil(request_end/C)-existing_region_chunks)*C`, without subtracting
free chunks, not a net arena-size deficit. Snapshot survives cleanup.
The producer has no clamp for rounded demand 2^63 before int64 transport
(`src/scratch_arena.cpp:226`, `include/pineforge/run_failure.hpp:181`);
do not claim exact positive transport there.

Single-threaded identical scratch-operation sequences exhaust at the
same operation/high-water across allowed window caps, absent independent
I/O/resource failures (`include/pineforge/detail/scratch_arena.hpp:80`).
Concurrent stores share accounting; scheduling can change the failing
store/operation and high-water.

### Success and failure stdout

Fd success stdout is the existing small report minus trades/equity_curve.
**No report_sink descriptor, run ID, sink digest or seven-count receipt
object is added** (`docker/run_json.py:5785`). Named paths add a
descriptor; temporary paths reconstruct arrays (`docker/run_json.py:5791`).
With outputs, fd mode retains schema_version, message_format,
manifest_sha256, manifest, constants, hlines; no bars/series/events
(`docker/run_json.py:5892`). The single table below gives exact paths.

Arena diagnostics appear only if the module exports the getter and it
returns 0 (`docker/run_json.py:5359`). No attachment gives two zeros;
an unused attachment gives zero high-water and exact size.
NULL/aliased pointers or snapshot failure give -1 with outputs unchanged
(`src/c_abi.cpp:1329`). The calls precede native cleanup
(`docker/run_json.py:5219`, `docker/run_json.py:4680`).
These are attachment-lifetime observations outside sink bytes and
semantic fingerprints.

W2 implements the typed failure line: reason/high_water_bytes/arena_bytes
are mirrored from args to top level only when supplied
(`docker/run_json.py:2207`). It invents nothing and does not query stats
after cleanup. claimed_bytes/needed_bytes_lower_bound/store stay under
args, as do sink_bytes/cap_bytes if supplied. Class/retryability remain
catalog metadata, not line keys. Old modules without a code getter can
still emit engine/error only. Oversized-args handling drops args and
mirrors together by replacing args with an empty object and removing
the mirrors; the args key remains.

Fd-mode handled failures free report, free strategy and close sink via
nested finally before main prints one failure line and returns 1
(`docker/run_json.py:5589`, `docker/run_json.py:4812`).
Close clears its saved fd first; EBADF is tolerated, never retried.
Cleanup synthesizes no trailer; pre-trailer EOF gives finalizer 6.
A later summary/cleanup failure can follow a native trailer: finalizer
0 still cannot authorize a failed child.

Fd stdout uses StringIO without a 2-MiB check (`docker/run_json.py:5255`,
`docker/run_json.py:4733`). 2 MiB including LF is the desired wrapper
summary limit, not a producer guarantee. stdout.json is child-written/
untrusted regardless of file ownership. A converter must cap reads,
validate types/schema and use verified-sink rows/counts. Failed stdout
publication after a prefix reports stderr and exits 1, without a second
JSON document (`docker/run_json.py:4381`). Completed-phase failure
after flush also uses stderr/exit 1 (`docker/run_json.py:4765`).
Signals/OOM need not print a line.

## 4. One emitted-key inventory

This is the single table for sink header/positional fields, frozen
trailer, finalizer receipt, sink failure, success report, phase receipt
and calculator results. B/U are JSON-compatible Python return objects,
not CLI JSON. Facts sidecars are inputs, not engine-produced receipts.
Other failure codes' args retain their catalog schemas; the generic
forwarded-arg row records that code-dependent namespace.

Prefixes denote separate documents, **not emitted wrapper objects**:
H=header; E/T/O/V/K/X/HR=record array; Z=trailer (also pf_sink CLI output);
F=finalizer receipt; D=failure stdout; J=success stdout; P=phase record;
B=arena_bound return; U=per_user_bytes return. Each $ starts at that
root. [] means every element; {name} means dynamic key; set selectors
mean each displayed path. Positional rows show descriptor names and
actual array indexes, not invented object keys.

RT expands to J $.applied_runtime and, if fingerprint exists,
J $.fingerprint.provenance.runtime. RW expands to the three selected
placements J $.report_window and RT.report_window. PROV means
J $.fingerprint.provenance. TS means each J $.metrics.{all,longs,shorts};
EQ means J $.metrics.equity. BD means derivations under B's four terms
and six arena inputs. These aliases introduce no JSON keys.

**Prefix** in the last column means outside the trailer but included in
logical sink SHA-256. **Trailer** means a frozen trailer field, itself
excluded from that hash (`src/report_sink_writer.cpp:501`).
**Outside** means neither. No arena/receipt/failure/summary key belongs
in the trailer. Fingerprint hashes are independent of the sink hash.

| Key / descriptor field | JSON path | Type | When present | Emitter file:line | Trailer / sink hash |
|---|---|---|---|---|---|
| `schema` | H `$.schema` | string `"pf-report-sink"` | every header | `src/report_sink_writer.cpp:143` | Prefix |
| `version` | H `$.version` | integer 1 | every header | `src/report_sink_writer.cpp:143` | Prefix |
| `run_id` | H `$.run_id` | string | every header; empty default | `src/report_sink_writer.cpp:144` | Prefix |
| `engine` | H `$.engine` | string, engine version | every header | `src/report_sink_writer.cpp:146` | Prefix |
| `records` | H `$.records` | array of descriptors | every header, ET mandatory; OVK together, X/H optional | `src/report_sink_writer.cpp:147` | Prefix |
| `type` | H `$.records[].type` | string tag | each selected descriptor | `src/report_sink_writer.cpp:119` | Prefix |
| `fields` | H `$.records[].fields` | string array | each descriptor, fixed names plus vN/cN | `src/report_sink_writer.cpp:122` | Prefix |
| `types` | H `$.records[].types` | string array | each descriptor | `src/report_sink_writer.cpp:122` | Prefix |
| `units` | H `$.records[].units` | string array | each descriptor | `src/report_sink_writer.cpp:122` | Prefix |
| `tag` (positional) | E/T/O/V/K/X/HR `$[0]` | string E/T/O/V/K/X/H | each emitted record | `src/report_sink_writer.cpp:439`; `src/report_sink_writer.cpp:460`; `src/report_sink_writer.cpp:545`; `src/report_sink_writer.cpp:555`; `src/report_sink_writer.cpp:570`; `src/report_sink_writer.cpp:579`; `src/report_sink_writer.cpp:586` | Prefix |
| `time_ms` | E `$[1]` | integer | every E | `src/report_sink_writer.cpp:439` | Prefix |
| `equity` | E `$[2]` | number or null | every E | `src/report_sink_writer.cpp:439` | Prefix |
| `open_profit` | E `$[3]` | number or null | every E | `src/report_sink_writer.cpp:440` | Prefix |
| `n` | T `$[1]` | integer, contiguous from 1 | every T | `src/report_sink_writer.cpp:460` | Prefix |
| `side` | T `$[2]` | string long/short | every T | `src/report_sink_writer.cpp:460` | Prefix |
| `entry_time` | T `$[3]` | integer | every T | `src/report_sink_writer.cpp:461` | Prefix |
| `exit_time` | T `$[4]` | integer | every T | `src/report_sink_writer.cpp:461` | Prefix |
| `entry_price` | T `$[5]` | number or null | every T | `src/report_sink_writer.cpp:462` | Prefix |
| `exit_price` | T `$[6]` | number or null | every T | `src/report_sink_writer.cpp:462` | Prefix |
| `qty` | T `$[7]` | number or null | every T | `src/report_sink_writer.cpp:462` | Prefix |
| `pnl` | T `$[8]` | number or null | every T | `src/report_sink_writer.cpp:462` | Prefix |
| `pnl_pct` | T `$[9]` | number or null | every T | `src/report_sink_writer.cpp:463` | Prefix |
| `max_runup` | T `$[10]` | number or null | every T | `src/report_sink_writer.cpp:463` | Prefix |
| `max_drawdown` | T `$[11]` | number or null | every T | `src/report_sink_writer.cpp:463` | Prefix |
| `commission` | T `$[12]` | number or null | every T | `src/report_sink_writer.cpp:463` | Prefix |
| `entry_bar_index` | T `$[13]` | integer | every T | `src/report_sink_writer.cpp:465` | Prefix |
| `exit_bar_index` | T `$[14]` | integer | every T | `src/report_sink_writer.cpp:465` | Prefix |
| `open_at_end` | T `$[15]` | boolean | every T | `src/report_sink_writer.cpp:466` | Prefix |
| `entry_incarnation` | T `$[16]` | nonnegative integer | every T | `src/report_sink_writer.cpp:467` | Prefix |
| `close_cause` | T `$[17]` | integer | every T | `src/report_sink_writer.cpp:467` | Prefix |
| `entry_id` | T `$[18]` | string | every T | `src/report_sink_writer.cpp:468` | Prefix |
| `exit_id` | T `$[19]` | string | every T | `src/report_sink_writer.cpp:468` | Prefix |
| `exit_comment` | T `$[20]` | string | every T | `src/report_sink_writer.cpp:468` | Prefix |
| `row` | O `$[1]` | integer, contiguous from 0 | output rows | `src/report_sink_writer.cpp:545` | Prefix |
| `open_ms` | O `$[2]` | integer or null | output rows | `src/report_sink_writer.cpp:545` | Prefix |
| `close_ms` | O `$[3]` | integer or null | output rows | `src/report_sink_writer.cpp:546` | Prefix |
| `vN` | O `$[4+N]`; H O descriptor's fields | number or null; descriptor string | N from 0 to series_slots-1 | `src/report_sink_writer.cpp:105`; `src/report_sink_writer.cpp:547` | Prefix |
| `sequence` | V `$[1]` | nonnegative integer | output events | `src/report_sink_writer.cpp:555` | Prefix |
| `output_index` | V `$[2]` | integer | output events | `src/report_sink_writer.cpp:556` | Prefix |
| `bar_index` | V `$[3]`; X `$[2]` | integer | event/trace records respectively | `src/report_sink_writer.cpp:556`; `src/report_sink_writer.cpp:579` | Prefix |
| `bar_open_ms` | V `$[4]` | integer or null | output events | `src/report_sink_writer.cpp:557` | Prefix |
| `bar_close_ms` | V `$[5]` | integer or null | output events | `src/report_sink_writer.cpp:557` | Prefix |
| `ordinal_in_bar` | V `$[6]` | integer | output events | `src/report_sink_writer.cpp:558` | Prefix |
| `phase` | V `$[7]` | integer | output events | `src/report_sink_writer.cpp:558` | Prefix |
| `confirmed` | V `$[8]` | integer | output events | `src/report_sink_writer.cpp:559` | Prefix |
| `value` | V `$[9]`; X `$[4]` | number or null | event/trace records respectively | `src/report_sink_writer.cpp:559`; `src/report_sink_writer.cpp:580` | Prefix |
| `message_hash64` | V `$[10]` | string, hex64 | output events | `src/report_sink_writer.cpp:560` | Prefix |
| `message` | V `$[11]` | string or null | output events | `src/report_sink_writer.cpp:561` | Prefix |
| `cN` | K `$[1+N]`; H K descriptor's fields | number or null; descriptor string | constants enabled; N from 0 to constant_count-1 | `src/report_sink_writer.cpp:111`; `src/report_sink_writer.cpp:571` | Prefix |
| `timestamp` | X `$[1]` | integer | trace records | `src/report_sink_writer.cpp:579` | Prefix |
| `name` | X `$[3]` | string | trace records | `src/report_sink_writer.cpp:580` | Prefix |
| `hash` | HR `$[1]` | string, hex64 | broker hash records | `src/report_sink_writer.cpp:586` | Prefix |
| `trailer` | Z `$.trailer` | string `"pf-report-sink"` | successful trailer only | `src/report_sink_writer.cpp:488` | Trailer |
| `version` | Z `$.version` | integer 1 | successful trailer | `src/report_sink_writer.cpp:488` | Trailer |
| `counts` | Z `$.counts` | object | successful trailer | `src/report_sink_writer.cpp:488` | Trailer |
| `E` | Z `$.counts.E` | nonnegative integer | every trailer | `src/report_sink_writer.cpp:491` | Trailer |
| `T` | Z `$.counts.T` | nonnegative integer | every trailer | `src/report_sink_writer.cpp:491` | Trailer |
| `O` | Z `$.counts.O` | nonnegative integer | outputs enabled | `src/report_sink_writer.cpp:491` | Trailer |
| `V` | Z `$.counts.V` | nonnegative integer | outputs enabled | `src/report_sink_writer.cpp:491` | Trailer |
| `K` | Z `$.counts.K` | nonnegative integer | outputs enabled | `src/report_sink_writer.cpp:491` | Trailer |
| `X` | Z `$.counts.X` | nonnegative integer | trace enabled | `src/report_sink_writer.cpp:491` | Trailer |
| `H` | Z `$.counts.H` | nonnegative integer | hashes enabled | `src/report_sink_writer.cpp:491` | Trailer |
| `sha256` | Z `$.sha256` | string, lowercase SHA-256 | successful trailer; digest covers header/records/LFs before it | `src/report_sink_writer.cpp:500` | Trailer |
| `complete` | F `$.complete` | boolean | each emitted receipt; true only status 0 | `docker/pineforge_sink_finalize.py:182` | Outside |
| `count_e` | F `$.count_e` | nonnegative integer | each receipt | `docker/pineforge_sink_finalize.py:183` | Outside |
| `count_t` | F `$.count_t` | nonnegative integer | each receipt | `docker/pineforge_sink_finalize.py:183` | Outside |
| `count_o` | F `$.count_o` | nonnegative integer | each receipt, zero if absent | `docker/pineforge_sink_finalize.py:183` | Outside |
| `count_v` | F `$.count_v` | nonnegative integer | each receipt, zero if absent | `docker/pineforge_sink_finalize.py:183` | Outside |
| `count_k` | F `$.count_k` | nonnegative integer | each receipt, zero if absent | `docker/pineforge_sink_finalize.py:183` | Outside |
| `count_x` | F `$.count_x` | nonnegative integer | each receipt, zero if absent | `docker/pineforge_sink_finalize.py:183` | Outside |
| `count_h` | F `$.count_h` | nonnegative integer | each receipt, zero if absent | `docker/pineforge_sink_finalize.py:183` | Outside |
| `reason` | F `$.reason` | string `"sink_exhausted"` | final exit 8 only, after count_h | `docker/pineforge_sink_finalize.py:184` | Outside |
| `sink_bytes` | F `$.sink_bytes` | nonnegative integer | each receipt; U+1 on exit 8 | `docker/pineforge_sink_finalize.py:185` | Outside |
| `cap_bytes` | F `$.cap_bytes` | integer | final exit 8 only, immediately after sink_bytes | `docker/pineforge_sink_finalize.py:186` | Outside |
| `sha256` | F `$.sha256` | string | logical digest on success, empty otherwise | `docker/pineforge_sink_finalize.py:187` | Outside |
| `finalizer_version` | F `$.finalizer_version` | string `"1.0.0"` | each receipt, last key | `docker/pineforge_sink_finalize.py:188`; `docker/pineforge_sink_finalize.py:128` | Outside |
| `engine` | D `$.engine` | string `"pineforge"` | each failure line | `docker/run_json.py:2214` | Outside |
| `error` | D `$.error` | string | each failure line; bounded human text | `docker/run_json.py:2214` | Outside |
| `code` | D `$.code` | string | typed failures; omitted for uncoded old-module errors | `docker/run_json.py:2217` | Outside |
| `args` | D `$.args` | object | whenever code exists, possibly empty | `docker/run_json.py:2218` | Outside |
| `{arg}` | D `$.args.{arg}` | catalog scalar type | other codes' supplied args; strings are bounded | `docker/run_json.py:2218` | Outside |
| `reason` | D `$.reason`; D `$.args.reason` | string, sink reason vocabulary | report_sink_failed with supplied reason; mirrors agree | `docker/run_json.py:2223`; `src/scratch_arena.cpp:224`; `docker/run_json.py:5458` | Outside |
| `high_water_bytes` | D `$.high_water_bytes`; D `$.args.high_water_bytes` | integer bytes | supplied arena-exhaustion snapshot; mirrored | `src/scratch_arena.cpp:224`; `docker/run_json.py:2223` | Outside |
| `arena_bytes` | D `$.arena_bytes`; D `$.args.arena_bytes` | integer bytes | supplied arena-exhaustion snapshot; mirrored | `src/scratch_arena.cpp:225`; `docker/run_json.py:2223` | Outside |
| `claimed_bytes` | D `$.args.claimed_bytes` | integer bytes | supplied arena-exhaustion snapshot; no top-level mirror | `src/scratch_arena.cpp:225`; `docker/run_json.py:2218` | Outside |
| `needed_bytes_lower_bound` | D `$.args.needed_bytes_lower_bound` | integer bytes, int64 edge caveat above | supplied arena-exhaustion snapshot | `src/scratch_arena.cpp:226`; `docker/run_json.py:2218` | Outside |
| `store` | D `$.args.store` | string, twelve-value vocabulary | supplied arena-exhaustion snapshot | `src/scratch_arena.cpp:227`; `docker/run_json.py:2218` | Outside |
| `sink_bytes` | D `$.args.sink_bytes` | integer bytes | generic forwarding if supplied; catalog permits it, no local run_json sink-exhaustion raiser | `docker/run_json.py:2218`; `docker/run_failure_codes.json:974` | Outside |
| `cap_bytes` | D `$.args.cap_bytes` | integer bytes | generic forwarding if supplied; actual overflow producer is F | `docker/run_json.py:2218`; `docker/run_failure_codes.json:945` | Outside |
| `engine` | J `$.engine` | string `"pineforge"` | success report | `docker/run_json.py:3888` | Outside |
| `input` | J `$.input` | object | success report | `docker/run_json.py:3889` | Outside |
| `ohlcv` | J `$.input.ohlcv` | string | success report | `docker/run_json.py:3890` | Outside |
| `bars` | J `$.input.bars` | integer | success report, evaluated feed count | `docker/run_json.py:3891` | Outside |
| `first_ts` | J `$.input.first_ts` | integer or null | null only for empty selected evaluated feed | `docker/run_json.py:3894` | Outside |
| `last_ts` | J `$.input.last_ts` | integer or null | same condition | `docker/run_json.py:3895` | Outside |
| `first_time` | J `$.input.first_time` | string or null | same condition | `docker/run_json.py:3896` | Outside |
| `last_time` | J `$.input.last_time` | string or null | same condition | `docker/run_json.py:3897` | Outside |
| `applied_inputs` | J `$.applied_inputs` | object | success report | `docker/run_json.py:3899` | Outside |
| `{input}` | J `$.applied_inputs.{input}` | string | each supplied input; raw applied map | `docker/run_json.py:3986`; `docker/run_json.py:3899` | Outside |
| `applied_overrides` | J `$.applied_overrides` | object | success report | `docker/run_json.py:3900` | Outside |
| `{override}` | J `$.applied_overrides.{override}` | string | each supplied override; raw applied map | `docker/run_json.py:3986`; `docker/run_json.py:3900` | Outside |
| `applied_runtime` | J `$.applied_runtime` | object | success report | `docker/run_json.py:3901` | Outside |
| `elapsed_seconds` | J `$.elapsed_seconds` | number | success report | `docker/run_json.py:3902` | Outside |
| `summary` | J `$.summary` | object | sink success report | `docker/run_json.py:5783` | Outside |
| `total_trades` | J `$.summary.total_trades` | integer | sink success | `docker/run_json.py:5762` | Outside |
| `wins` | J `$.summary.wins` | integer | sink success | `docker/run_json.py:5763` | Outside |
| `losses` | J `$.summary.losses` | integer | sink success | `docker/run_json.py:5764` | Outside |
| `win_rate_pct` | J `$.summary.win_rate_pct` | number | sink success | `docker/run_json.py:5765` | Outside |
| `net_pnl` | J `$.summary.net_pnl` | number | sink success | `docker/run_json.py:5766` | Outside |
| `avg_trade` | J `$.summary.avg_trade` | number | sink success | `docker/run_json.py:5767` | Outside |
| `best_trade` | J `$.summary.best_trade` | number | sink success | `docker/run_json.py:5768` | Outside |
| `worst_trade` | J `$.summary.worst_trade` | number | sink success | `docker/run_json.py:5769` | Outside |
| `max_drawdown` | J `$.summary.max_drawdown` | number | sink success | `docker/run_json.py:5770` | Outside |
| `bars_processed` | J `$.summary.bars_processed` | integer | sink success | `docker/run_json.py:5771` | Outside |
| `diagnostics` | J `$.diagnostics` | object | success report | `docker/run_json.py:3915` | Outside |
| `input_bars_processed` | J `$.diagnostics.input_bars_processed` | integer | success report | `docker/run_json.py:3916` | Outside |
| `script_bars_processed` | J `$.diagnostics.script_bars_processed` | integer | success report | `docker/run_json.py:3917` | Outside |
| `magnifier_sub_bars_total` | J `$.diagnostics.magnifier_sub_bars_total` | integer | success report | `docker/run_json.py:3918` | Outside |
| `magnifier_sample_ticks_total` | J `$.diagnostics.magnifier_sample_ticks_total` | integer | success report | `docker/run_json.py:3919` | Outside |
| `bar_magnifier_enabled` | J `$.diagnostics.bar_magnifier_enabled` | boolean | success report | `docker/run_json.py:3920` | Outside |
| `scratch_arena_high_water_bytes` | J `$.diagnostics.scratch_arena_high_water_bytes` | nonnegative integer | getter exported and status 0; zero without attachment | `docker/run_json.py:5368` | Outside |
| `scratch_arena_bytes` | J `$.diagnostics.scratch_arena_bytes` | nonnegative integer | getter exported and status 0; exact supplied extent or zero | `docker/run_json.py:5369` | Outside |
| `report_sink` | J `$.report_sink` | object | named path sink only; absent in fd mode | `docker/run_json.py:5803` | Outside |
| `path` | J `$.report_sink.path` | string | named path sink | `docker/run_json.py:5793` | Outside |
| `schema` | J `$.report_sink.schema` | string | named path sink | `docker/run_json.py:5794` | Outside |
| `version` | J `$.report_sink.version` | integer 1 | named path sink | `docker/run_json.py:5795` | Outside |
| `run_id` | J `$.report_sink.run_id` | string | named path sink | `docker/run_json.py:5796` | Outside |
| `counts` | J `$.report_sink.counts` | object | named path sink, selected tags only | `docker/run_json.py:5797` | Outside |
| `E` | J `$.report_sink.counts.E` | integer | named path sink | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `T` | J `$.report_sink.counts.T` | integer | named path sink | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `O` | J `$.report_sink.counts.O` | integer | named path, outputs enabled | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `V` | J `$.report_sink.counts.V` | integer | named path, outputs enabled | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `K` | J `$.report_sink.counts.K` | integer | named path, outputs enabled | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `X` | J `$.report_sink.counts.X` | integer | named path, trace enabled | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `H` | J `$.report_sink.counts.H` | integer | named path, hashes enabled | `docker/run_json.py:5797`; `docker/pf_sink.py:253` | Outside |
| `sha256` | J `$.report_sink.sha256` | string | named path sink | `docker/run_json.py:5798` | Outside |
| `trades` | J `$.trades` | array | temporary sink only among sink routes | `docker/run_json.py:5807` | Outside |
| `n` | J `$.trades[].n` | integer | temporary sink | `docker/run_json.py:5708` | Outside |
| `side` | J `$.trades[].side` | string long/short | temporary sink | `docker/run_json.py:5708` | Outside |
| `entry_time` | J `$.trades[].entry_time` | integer | temporary sink | `docker/run_json.py:5708` | Outside |
| `exit_time` | J `$.trades[].exit_time` | integer | temporary sink | `docker/run_json.py:5708` | Outside |
| `entry_price` | J `$.trades[].entry_price` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `exit_price` | J `$.trades[].exit_price` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `qty` | J `$.trades[].qty` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `pnl` | J `$.trades[].pnl` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `pnl_pct` | J `$.trades[].pnl_pct` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `max_runup` | J `$.trades[].max_runup` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `max_drawdown` | J `$.trades[].max_drawdown` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `commission` | J `$.trades[].commission` | number | temporary sink, finite canonical trade | `docker/run_json.py:5708` | Outside |
| `entry_bar_index` | J `$.trades[].entry_bar_index` | integer | temporary sink | `docker/run_json.py:5708` | Outside |
| `exit_bar_index` | J `$.trades[].exit_bar_index` | integer | temporary sink | `docker/run_json.py:5708` | Outside |
| `open_at_end` | J `$.trades[].open_at_end` | boolean | temporary sink | `docker/run_json.py:5708` | Outside |
| `entry_incarnation` | J `$.trades[].entry_incarnation` | integer | temporary sink; later T fields remain sink-only | `docker/run_json.py:5708` | Outside |
| `equity_curve` | J `$.equity_curve` | array | temporary sink only among sink routes | `docker/run_json.py:5808` | Outside |
| `time_ms` | J `$.equity_curve[].time_ms` | integer | temporary sink | `docker/run_json.py:5808`; `docker/pf_sink.py:44` | Outside |
| `equity` | J `$.equity_curve[].equity` | number or null | temporary sink | `docker/run_json.py:5808`; `docker/pf_sink.py:44` | Outside |
| `open_profit` | J `$.equity_curve[].open_profit` | number or null | temporary sink | `docker/run_json.py:5808`; `docker/pf_sink.py:44` | Outside |
| `metrics` | J `$.metrics` | object | every success report | `docker/run_json.py:3923` | Outside |
| `all` | J `$.metrics.all` | object | every success report | `docker/run_json.py:3869` | Outside |
| `longs` | J `$.metrics.longs` | object | every success report | `docker/run_json.py:3870` | Outside |
| `shorts` | J `$.metrics.shorts` | object | every success report | `docker/run_json.py:3871` | Outside |
| `equity` | J `$.metrics.equity` | object | every success report | `docker/run_json.py:3872` | Outside |
| `num_trades` | TS `.num_trades` | integer | each trade-stats block; field-driven emission | `docker/run_json.py:2326`; `docker/run_json.py:3815` | Outside |
| `num_wins` | TS `.num_wins` | integer | each trade-stats block | `docker/run_json.py:2326`; `docker/run_json.py:3815` | Outside |
| `num_losses` | TS `.num_losses` | integer | each trade-stats block | `docker/run_json.py:2327`; `docker/run_json.py:3815` | Outside |
| `num_even` | TS `.num_even` | integer | each trade-stats block | `docker/run_json.py:2327`; `docker/run_json.py:3815` | Outside |
| `percent_profitable` | TS `.percent_profitable` | number or null | each trade-stats block | `docker/run_json.py:2328`; `docker/run_json.py:3815` | Outside |
| `net_profit` | TS `.net_profit` | number or null | each trade-stats block | `docker/run_json.py:2329`; `docker/run_json.py:3815` | Outside |
| `net_profit_pct` | TS `.net_profit_pct` | number or null | each trade-stats block | `docker/run_json.py:2329`; `docker/run_json.py:3815` | Outside |
| `gross_profit` | TS `.gross_profit` | number or null | each trade-stats block | `docker/run_json.py:2330`; `docker/run_json.py:3815` | Outside |
| `gross_profit_pct` | TS `.gross_profit_pct` | number or null | each trade-stats block | `docker/run_json.py:2330`; `docker/run_json.py:3815` | Outside |
| `gross_loss` | TS `.gross_loss` | number or null | each trade-stats block | `docker/run_json.py:2331`; `docker/run_json.py:3815` | Outside |
| `gross_loss_pct` | TS `.gross_loss_pct` | number or null | each trade-stats block | `docker/run_json.py:2331`; `docker/run_json.py:3815` | Outside |
| `profit_factor` | TS `.profit_factor` | number or null | each trade-stats block | `docker/run_json.py:2332`; `docker/run_json.py:3815` | Outside |
| `avg_trade` | TS `.avg_trade` | number or null | each trade-stats block | `docker/run_json.py:2333`; `docker/run_json.py:3815` | Outside |
| `avg_trade_pct` | TS `.avg_trade_pct` | number or null | each trade-stats block | `docker/run_json.py:2333`; `docker/run_json.py:3815` | Outside |
| `avg_win` | TS `.avg_win` | number or null | each trade-stats block | `docker/run_json.py:2334`; `docker/run_json.py:3815` | Outside |
| `avg_win_pct` | TS `.avg_win_pct` | number or null | each trade-stats block | `docker/run_json.py:2334`; `docker/run_json.py:3815` | Outside |
| `avg_loss` | TS `.avg_loss` | number or null | each trade-stats block | `docker/run_json.py:2335`; `docker/run_json.py:3815` | Outside |
| `avg_loss_pct` | TS `.avg_loss_pct` | number or null | each trade-stats block | `docker/run_json.py:2335`; `docker/run_json.py:3815` | Outside |
| `ratio_avg_win_avg_loss` | TS `.ratio_avg_win_avg_loss` | number or null | each trade-stats block | `docker/run_json.py:2336`; `docker/run_json.py:3815` | Outside |
| `largest_win` | TS `.largest_win` | number or null | each trade-stats block | `docker/run_json.py:2337`; `docker/run_json.py:3815` | Outside |
| `largest_win_pct` | TS `.largest_win_pct` | number or null | each trade-stats block | `docker/run_json.py:2337`; `docker/run_json.py:3815` | Outside |
| `largest_loss` | TS `.largest_loss` | number or null | each trade-stats block | `docker/run_json.py:2338`; `docker/run_json.py:3815` | Outside |
| `largest_loss_pct` | TS `.largest_loss_pct` | number or null | each trade-stats block | `docker/run_json.py:2338`; `docker/run_json.py:3815` | Outside |
| `commission_paid` | TS `.commission_paid` | number or null | each trade-stats block | `docker/run_json.py:2339`; `docker/run_json.py:3815` | Outside |
| `expectancy` | TS `.expectancy` | number or null | each trade-stats block | `docker/run_json.py:2340`; `docker/run_json.py:3815` | Outside |
| `max_consecutive_wins` | TS `.max_consecutive_wins` | integer | each trade-stats block | `docker/run_json.py:2341`; `docker/run_json.py:3815` | Outside |
| `max_consecutive_losses` | TS `.max_consecutive_losses` | integer | each trade-stats block | `docker/run_json.py:2341`; `docker/run_json.py:3815` | Outside |
| `avg_bars_in_trade` | TS `.avg_bars_in_trade` | number or null | each trade-stats block | `docker/run_json.py:2342`; `docker/run_json.py:3815` | Outside |
| `avg_bars_in_wins` | TS `.avg_bars_in_wins` | number or null | each trade-stats block | `docker/run_json.py:2342`; `docker/run_json.py:3815` | Outside |
| `avg_bars_in_losses` | TS `.avg_bars_in_losses` | number or null | each trade-stats block | `docker/run_json.py:2343`; `docker/run_json.py:3815` | Outside |
| `max_equity_drawdown` | EQ `.max_equity_drawdown` | number or null | equity stats | `docker/run_json.py:2350`; `docker/run_json.py:3815` | Outside |
| `max_equity_drawdown_pct` | EQ `.max_equity_drawdown_pct` | number or null | equity stats | `docker/run_json.py:2350`; `docker/run_json.py:3815` | Outside |
| `max_equity_runup` | EQ `.max_equity_runup` | number or null | equity stats | `docker/run_json.py:2351`; `docker/run_json.py:3815` | Outside |
| `max_equity_runup_pct` | EQ `.max_equity_runup_pct` | number or null | equity stats | `docker/run_json.py:2351`; `docker/run_json.py:3815` | Outside |
| `buy_hold_return` | EQ `.buy_hold_return` | number or null | equity stats | `docker/run_json.py:2352`; `docker/run_json.py:3815` | Outside |
| `buy_hold_return_pct` | EQ `.buy_hold_return_pct` | number or null | equity stats | `docker/run_json.py:2352`; `docker/run_json.py:3815` | Outside |
| `sharpe_tv` | EQ `.sharpe_tv` | number or null | JSON spelling of sharpe_monthly | `docker/run_json.py:3803`; `docker/run_json.py:3815` | Outside |
| `sortino_tv` | EQ `.sortino_tv` | number or null | JSON spelling of sortino_monthly | `docker/run_json.py:3803`; `docker/run_json.py:3815` | Outside |
| `sharpe_bar` | EQ `.sharpe_bar` | number or null | equity stats | `docker/run_json.py:2357`; `docker/run_json.py:3815` | Outside |
| `sortino_bar` | EQ `.sortino_bar` | number or null | equity stats | `docker/run_json.py:2357`; `docker/run_json.py:3815` | Outside |
| `cagr` | EQ `.cagr` | number or null | equity stats | `docker/run_json.py:2358`; `docker/run_json.py:3815` | Outside |
| `calmar` | EQ `.calmar` | number or null | equity stats | `docker/run_json.py:2358`; `docker/run_json.py:3815` | Outside |
| `recovery_factor` | EQ `.recovery_factor` | number or null | equity stats | `docker/run_json.py:2359`; `docker/run_json.py:3815` | Outside |
| `time_in_market_pct` | EQ `.time_in_market_pct` | number or null | equity stats | `docker/run_json.py:2359`; `docker/run_json.py:3815` | Outside |
| `open_pl` | EQ `.open_pl` | number or null | equity stats | `docker/run_json.py:2360`; `docker/run_json.py:3815` | Outside |
| `input_tf` | RT `.input_tf` | string | success | `docker/run_json.py:5179`; `docker/run_json.py:4642` | Outside |
| `script_tf` | RT `.script_tf` | string | success | `docker/run_json.py:5180`; `docker/run_json.py:4643` | Outside |
| `input_tf_seconds` | RT `.input_tf_seconds` | integer | success | `docker/run_json.py:5181`; `docker/run_json.py:4644` | Outside |
| `script_tf_seconds` | RT `.script_tf_seconds` | integer | success | `docker/run_json.py:5182`; `docker/run_json.py:4645` | Outside |
| `script_tf_ratio` | RT `.script_tf_ratio` | integer | success | `docker/run_json.py:5183`; `docker/run_json.py:4646` | Outside |
| `needs_aggregation` | RT `.needs_aggregation` | boolean | success | `docker/run_json.py:5184`; `docker/run_json.py:4647` | Outside |
| `bar_magnifier` | RT `.bar_magnifier` | boolean | success | `docker/run_json.py:5185`; `docker/run_json.py:4648` | Outside |
| `magnifier_samples` | RT `.magnifier_samples` | integer | success | `docker/run_json.py:5186`; `docker/run_json.py:4649` | Outside |
| `magnifier_dist` | RT `.magnifier_dist` | string | success | `docker/run_json.py:5187`; `docker/run_json.py:4650` | Outside |
| `magnifier_volume_weighted` | RT `.magnifier_volume_weighted` | boolean | success | `docker/run_json.py:5188`; `docker/run_json.py:4651` | Outside |
| `trade_start_ms` | RT `.trade_start_ms` | integer or null | selected uses null | `docker/run_json.py:5189`; `docker/run_json.py:4652` | Outside |
| `chart_tz` | RT `.chart_tz` | string | success, empty default | `docker/run_json.py:5190`; `docker/run_json.py:4653` | Outside |
| `outputs` | RT `.outputs` | boolean true | outputs requested | `docker/run_json.py:5204`; `docker/run_json.py:4665` | Outside |
| `syminfo` | RT `.syminfo` | object | nonempty applied lot grid | `docker/run_json.py:5193`; `docker/run_json.py:4656` | Outside |
| `qty_step` | RT `.syminfo.qty_step` | number | applied mincontract grid | `docker/run_json.py:3119` | Outside |
| `mincontract` | RT `.syminfo.mincontract` | number | applied mincontract grid | `docker/run_json.py:3119` | Outside |
| `symbol_feeds` | RT `.symbol_feeds` | object | extra feeds installed | `docker/run_json.py:5195`; `docker/run_json.py:4658` | Outside |
| `canonicalization` | RT `.symbol_feeds.canonicalization` | string | extra feeds installed | `docker/run_json.py:3542` | Outside |
| `symbols` | RT `.symbol_feeds.symbols` | object | extra feeds installed | `docker/run_json.py:3543` | Outside |
| `{symbol}` | RT `.symbol_feeds.symbols.{symbol}` | object | each installed symbol | `docker/run_json.py:3543` | Outside |
| `facts` | RT `.symbol_feeds.symbols.{symbol}.facts` | object | each symbol, possibly empty | `docker/run_json.py:3544` | Outside |
| `canonical` | RT `.symbol_feeds.symbols.{symbol}.facts.canonical` | string | tickerid supplied | `docker/run_json.py:3162`; `docker/run_json.py:3544` | Outside |
| `type` | RT `.symbol_feeds.symbols.{symbol}.facts.type` | string | supplied nonempty fact | `docker/run_json.py:3162`; `docker/run_json.py:3544` | Outside |
| `timezone` | RT `.symbol_feeds.symbols.{symbol}.facts.timezone` | string | supplied nonempty fact | `docker/run_json.py:3162`; `docker/run_json.py:3544` | Outside |
| `session` | RT `.symbol_feeds.symbols.{symbol}.facts.session` | string | supplied nonempty fact | `docker/run_json.py:3163`; `docker/run_json.py:3544` | Outside |
| `currency` | RT `.symbol_feeds.symbols.{symbol}.facts.currency` | string | supplied nonempty fact | `docker/run_json.py:3163`; `docker/run_json.py:3544` | Outside |
| `mintick` | RT `.symbol_feeds.symbols.{symbol}.facts.mintick` | number | supplied positive finite fact | `docker/run_json.py:3338`; `docker/run_json.py:3544` | Outside |
| `feeds` | RT `.symbol_feeds.symbols.{symbol}.feeds` | object | each symbol | `docker/run_json.py:3545` | Outside |
| `{timeframe}` | RT `.symbol_feeds.symbols.{symbol}.feeds.{timeframe}` | object | each installed feed | `docker/run_json.py:3545` | Outside |
| `bars` | RT `.symbol_feeds.symbols.{symbol}.feeds.{timeframe}.bars` | integer | each feed, zero allowed | `docker/run_json.py:3307`; `docker/run_json.py:2699` | Outside |
| `source_values_sha256` | RT `.symbol_feeds.symbols.{symbol}.feeds.{timeframe}.source_values_sha256` | string | each feed | `docker/run_json.py:3307`; `docker/run_json.py:2699` | Outside |
| `first_ts` | RT `.symbol_feeds.symbols.{symbol}.feeds.{timeframe}.first_ts` | integer or null | nonempty CSV feed, or binary record including empty | `docker/run_json.py:3309`; `docker/run_json.py:2700` | Outside |
| `last_ts` | RT `.symbol_feeds.symbols.{symbol}.feeds.{timeframe}.last_ts` | integer or null | same condition | `docker/run_json.py:3309`; `docker/run_json.py:2700` | Outside |
| `outputs` | J `$.outputs` | object | outputs requested | `docker/run_json.py:5226`; `docker/run_json.py:4685` | Outside |
| `schema_version` | J `$.outputs.schema_version` | string `"pineforge-outputs/v1"` | output metadata, all sink routes | `docker/run_json.py:3772`; `docker/run_json.py:5906` | Outside |
| `message_format` | J `$.outputs.message_format` | JSON value, normally string `"pineforge/v1"` | copied manifest value or default; no type check here | `docker/run_json.py:3773`; `docker/run_json.py:5906` | Outside |
| `manifest_sha256` | J `$.outputs.manifest_sha256` | string | raw manifest byte digest | `docker/run_json.py:3774`; `docker/run_json.py:5906` | Outside |
| `manifest` | J `$.outputs.manifest` | object | output metadata | `docker/run_json.py:3775`; `docker/run_json.py:5906` | Outside |
| `{manifest property}` | J `$.outputs.manifest.{property}` recursively | JSON value | raw strategy manifest object is passed through; keys are strategy-defined | `docker/run_json.py:3734`; `docker/run_json.py:3775` | Outside |
| `constants` | J `$.outputs.constants` | array of number/integer/null | output metadata; rgba-u32 values are integers | `docker/run_json.py:3758`; `docker/run_json.py:3779`; `docker/run_json.py:5906` | Outside |
| `hlines` | J `$.outputs.hlines` | array | output metadata | `docker/run_json.py:3780`; `docker/run_json.py:5906` | Outside |
| `output` | J `$.outputs.hlines[].output` | string | hline manifest entries | `docker/run_json.py:3765` | Outside |
| `price` | J `$.outputs.hlines[].price` | number/integer/null | hline manifest entries | `docker/run_json.py:3766` | Outside |
| `bars` | J `$.outputs.bars` | object | temporary sink only among sink routes | `docker/run_json.py:5946` | Outside |
| `open_ms` | J `$.outputs.bars.open_ms` | integer/null array | temporary sink | `docker/run_json.py:5946` | Outside |
| `close_ms` | J `$.outputs.bars.close_ms` | integer/null array | temporary sink | `docker/run_json.py:5946` | Outside |
| `series` | J `$.outputs.series` | array | temporary sink only among sink routes | `docker/run_json.py:3778` | Outside |
| `slot` | J `$.outputs.series[].slot` | integer | each temporary-sink series | `docker/run_json.py:3755` | Outside |
| `output` | J `$.outputs.series[].output` | string | each temporary-sink series | `docker/run_json.py:3755` | Outside |
| `values` | J `$.outputs.series[].values` | number/integer/null array | temporary sink, populated from O | `docker/run_json.py:5948` | Outside |
| `events` | J `$.outputs.events` | array | temporary sink only among sink routes | `docker/run_json.py:5949` | Outside |
| `sequence` | J `$.outputs.events[].sequence` | integer | temporary-sink events | `docker/run_json.py:5826` | Outside |
| `output` | J `$.outputs.events[].output` | string | temporary-sink events, manifest ID | `docker/run_json.py:5827` | Outside |
| `bar_index` | J `$.outputs.events[].bar_index` | integer | temporary-sink events | `docker/run_json.py:5828` | Outside |
| `bar_open_ms` | J `$.outputs.events[].bar_open_ms` | integer or null | temporary-sink events | `docker/run_json.py:5829` | Outside |
| `bar_close_ms` | J `$.outputs.events[].bar_close_ms` | integer or null | temporary-sink events | `docker/run_json.py:5830` | Outside |
| `ordinal_in_bar` | J `$.outputs.events[].ordinal_in_bar` | integer | temporary-sink events | `docker/run_json.py:5831` | Outside |
| `phase` | J `$.outputs.events[].phase` | string | mapped phase name or numeric string | `docker/run_json.py:5832` | Outside |
| `value` | J `$.outputs.events[].value` | number or null | temporary-sink events | `docker/run_json.py:5833` | Outside |
| `message` | J `$.outputs.events[].message` | string or null | temporary-sink events | `docker/run_json.py:5834` | Outside |
| `freq` | J `$.outputs.events[].freq` | JSON value or null | alert output events only, manifest value copied | `docker/run_json.py:5837` | Outside |
| `fingerprint` | J `$.fingerprint` | object or null | success; ordinary failures to build it yield null, selected requires it | `docker/run_json.py:5248`; `docker/run_json.py:4710` | Outside |
| `token` | J `$.fingerprint.token` | string, base64 canonical bytes | fingerprint available | `docker/run_json.py:664` | Outside |
| `digest` | J `$.fingerprint.digest` | string, sha256-prefixed | fingerprint available | `docker/run_json.py:665` | Outside |
| `provenance` | J `$.fingerprint.provenance` | object | fingerprint available | `docker/run_json.py:666` | Outside |
| `version` | J `$.fingerprint.version` | integer 2 | selected only | `docker/run_json.py:4709` | Outside |
| `schema_version` | PROV `.schema_version` | integer 2 | selected only | `docker/run_json.py:4707` | Outside |
| `engine` | PROV `.engine` | object | fingerprint available | `docker/run_json.py:423` | Outside |
| `version_string` | PROV `.engine.version_string` | string | fingerprint available, empty on older module | `docker/run_json.py:2455`; `docker/run_json.py:2460` | Outside |
| `major` | PROV `.engine.major` | integer or null | fingerprint available | `docker/run_json.py:2455`; `docker/run_json.py:2464` | Outside |
| `minor` | PROV `.engine.minor` | integer or null | fingerprint available | `docker/run_json.py:2455`; `docker/run_json.py:2464` | Outside |
| `patch` | PROV `.engine.patch` | integer or null | fingerprint available | `docker/run_json.py:2456`; `docker/run_json.py:2464` | Outside |
| `commit_sha` | PROV `.engine.commit_sha` | string | fingerprint available, empty fallback | `docker/run_json.py:2456`; `docker/run_json.py:2465` | Outside |
| `feed` | PROV `.feed` | object | fingerprint available | `docker/run_json.py:424` | Outside |
| `canonicalization` | PROV `.feed.canonicalization` | string | fingerprint available | `docker/run_json.py:425` | Outside |
| `source_values_sha256` | PROV `.feed.source_values_sha256` | string | fingerprint available | `docker/run_json.py:426` | Outside |
| `codegen` | PROV `.codegen` | object | fingerprint available | `docker/run_json.py:428` | Outside |
| `version` | PROV `.codegen.version` | string | package version or unknown | `docker/run_json.py:429` | Outside |
| `generated_cpp_sha256` | PROV `.codegen.generated_cpp_sha256` | string or null | fingerprint available | `docker/run_json.py:5240`; `docker/run_json.py:4699` | Outside |
| `transpiled_from_pine` | PROV `.codegen.transpiled_from_pine` | boolean | fingerprint available | `docker/run_json.py:431` | Outside |
| `strategy` | PROV `.strategy` | object | fingerprint available | `docker/run_json.py:433` | Outside |
| `initial_capital` | PROV `.strategy.initial_capital` | number or null | resolved value or unresolved null | `docker/run_json.py:672`; `docker/run_json.py:2112`; `docker/run_json.py:2116` | Outside |
| `commission_value` | PROV `.strategy.commission_value` | number or null | same | `docker/run_json.py:673`; `docker/run_json.py:2116` | Outside |
| `default_qty_value` | PROV `.strategy.default_qty_value` | number or null | same | `docker/run_json.py:674`; `docker/run_json.py:2116` | Outside |
| `pyramiding` | PROV `.strategy.pyramiding` | integer or null | same | `docker/run_json.py:675`; `docker/run_json.py:2116` | Outside |
| `slippage` | PROV `.strategy.slippage` | integer or null | same | `docker/run_json.py:676`; `docker/run_json.py:2116` | Outside |
| `process_orders_on_close` | PROV `.strategy.process_orders_on_close` | boolean or null | same | `docker/run_json.py:677`; `docker/run_json.py:2116` | Outside |
| `calc_on_order_fills` | PROV `.strategy.calc_on_order_fills` | boolean or null | same | `docker/run_json.py:678`; `docker/run_json.py:2116` | Outside |
| `close_entries_rule` | PROV `.strategy.close_entries_rule` | string or null | same | `docker/run_json.py:679`; `docker/run_json.py:2116` | Outside |
| `default_qty_type` | PROV `.strategy.default_qty_type` | string or null | same | `docker/run_json.py:680`; `docker/run_json.py:2116` | Outside |
| `commission_type` | PROV `.strategy.commission_type` | string or null | same | `docker/run_json.py:681`; `docker/run_json.py:2116` | Outside |
| `{override}` | PROV `.strategy.{override}` | string | additional applied override keys retained by the seed builder | `docker/run_json.py:344`; `docker/run_json.py:433` | Outside |
| `inputs` | PROV `.inputs` | object | fingerprint available | `docker/run_json.py:1920`; `docker/run_json.py:2046` | Outside |
| `{input}` | PROV `.inputs.{input}` | object | each declared/applied input | `docker/run_json.py:1908`; `docker/run_json.py:2038` | Outside |
| `type` | PROV `.inputs.{input}.type` | string | each input metadata | `docker/run_json.py:1908`; `docker/run_json.py:2038` | Outside |
| `default` | PROV `.inputs.{input}.default` | scalar or null | each input metadata | `docker/run_json.py:1908`; `docker/run_json.py:2025` | Outside |
| `value` | PROV `.inputs.{input}.value` | scalar or null | each input metadata | `docker/run_json.py:1909`; `docker/run_json.py:2025` | Outside |
| `resolution` | PROV `.inputs.{input}.resolution` | object | unresolved input | `docker/run_json.py:2025`; `docker/run_json.py:1904` | Outside |
| `strategy_resolution` | PROV `.strategy_resolution` | object | at least one unresolved strategy value | `docker/run_json.py:2121` | Outside |
| `{override}` | PROV `.strategy_resolution.{override}` | object | each unresolved strategy value | `docker/run_json.py:2113` | Outside |
| `status` | PROV `.inputs.{input}.resolution.status`; PROV `.strategy_resolution.{override}.status` | string unresolved | unresolved metadata | `docker/run_json.py:2026`; `docker/run_json.py:2113` | Outside |
| `reason` | PROV `.inputs.{input}.resolution.reason`; PROV `.strategy_resolution.{override}.reason` | string | unresolved metadata | `docker/run_json.py:2026`; `docker/run_json.py:2113` | Outside |
| `raw_default` | PROV `.inputs.{input}.resolution.raw_default`; PROV `.strategy_resolution.{override}.raw_default` | string or null | unresolved metadata | `docker/run_json.py:2026`; `docker/run_json.py:2114` | Outside |
| `distinct_native_inputs` | PROV `.inputs.{input}.resolution.distinct_native_inputs` | integer | duplicate_title resolution only | `docker/run_json.py:2028` | Outside |
| `applied` | PROV `.applied` | object | fingerprint available | `docker/run_json.py:435` | Outside |
| `inputs` | PROV `.applied.inputs` | object | fingerprint available | `docker/run_json.py:436` | Outside |
| `{input}` | PROV `.applied.inputs.{input}` | scalar | normalized recognized value, otherwise raw string | `docker/run_json.py:1916`; `docker/run_json.py:2034` | Outside |
| `overrides` | PROV `.applied.overrides` | object | fingerprint available | `docker/run_json.py:437` | Outside |
| `{override}` | PROV `.applied.overrides.{override}` | scalar | normalized recognized value, otherwise raw string | `docker/run_json.py:2119` | Outside |
| `runtime` | PROV `.runtime` | object | same RT record as applied_runtime | `docker/run_json.py:439` | Outside |
| `report_window` | J `$.report_window`; RT `.report_window` | object | selected success, same R in three placements | `docker/run_json.py:4668`; `docker/run_json.py:4681` | Outside |
| `wire_version` | RW `.wire_version` | integer | selected R | `docker/selected_window_report.py:502` | Outside |
| `policy` | RW `.policy` | string | selected R | `docker/selected_window_report.py:503` | Outside |
| `applied` | RW `.applied` | boolean true | selected execution success | `docker/selected_window_report.py:504` | Outside |
| `window` | RW `.window` | object | selected R | `docker/selected_window_report.py:505` | Outside |
| `start_ms` | RW `.window.start_ms` | integer | selected R | `docker/selected_window_report.py:506` | Outside |
| `end_ms` | RW `.window.end_ms` | integer | selected R | `docker/selected_window_report.py:507` | Outside |
| `first_data_ms` | RW `.window.first_data_ms`; RW `.feed.first_data_ms` | integer or null | selected R, null for empty corresponding feed/window | `docker/selected_window_report.py:508`; `docker/selected_window_report.py:514` | Outside |
| `last_data_ms` | RW `.window.last_data_ms`; RW `.feed.last_data_ms` | integer or null | same condition | `docker/selected_window_report.py:509`; `docker/selected_window_report.py:515` | Outside |
| `coverage` | RW `.window.coverage` | string nonempty/empty | selected R | `docker/selected_window_report.py:510` | Outside |
| `feed` | RW `.feed` | object | selected R | `docker/selected_window_report.py:512` | Outside |
| `requested_start_ms` | RW `.feed.requested_start_ms` | integer | selected R | `docker/selected_window_report.py:513` | Outside |
| `supplied_first_data_ms` | RW `.feed.supplied_first_data_ms` | integer or null | selected R | `docker/selected_window_report.py:516` | Outside |
| `supplied_last_data_ms` | RW `.feed.supplied_last_data_ms` | integer or null | selected R | `docker/selected_window_report.py:517` | Outside |
| `input_tf_seconds` | RW `.feed.input_tf_seconds` | integer | selected R | `docker/selected_window_report.py:518` | Outside |
| `script_tf` | RW `.feed.script_tf` | string | selected R | `docker/selected_window_report.py:519` | Outside |
| `source_bytes_sha256` | RW `.feed.source_bytes_sha256` | string | selected R | `docker/selected_window_report.py:520` | Outside |
| `source_values_sha256` | RW `.feed.source_values_sha256` | string | selected R | `docker/selected_window_report.py:521` | Outside |
| `evaluated_source_values_sha256` | RW `.feed.evaluated_source_values_sha256` | string | selected R | `docker/selected_window_report.py:522` | Outside |
| `calendar` | RW `.calendar` | object | selected R | `docker/selected_window_report.py:524` | Outside |
| `timezone` | RW `.calendar.timezone` | string | selected R | `docker/selected_window_report.py:524` | Outside |
| `session` | RW `.calendar.session` | string or null | selected R | `docker/selected_window_report.py:524` | Outside |
| `preroll` | RW `.preroll` | object | selected R | `docker/selected_window_report.py:525` | Outside |
| `requested_script_bars` | RW `.preroll.requested_script_bars` | integer | selected R | `docker/selected_window_report.py:526` | Outside |
| `available_script_bars` | RW `.preroll.available_script_bars` | integer | selected R | `docker/selected_window_report.py:527` | Outside |
| `used_script_bars` | RW `.preroll.used_script_bars` | integer | selected R | `docker/selected_window_report.py:528` | Outside |
| `trimmed_script_bars` | RW `.preroll.trimmed_script_bars` | integer | selected R | `docker/selected_window_report.py:529` | Outside |
| `trim_start_ms` | RW `.preroll.trim_start_ms` | integer | selected R | `docker/selected_window_report.py:530` | Outside |
| `trimmed_input_bars` | RW `.preroll.trimmed_input_bars` | integer | selected R | `docker/selected_window_report.py:531` | Outside |
| `first_bar_ms` | RW `.preroll.first_bar_ms` | integer or null | selected R | `docker/selected_window_report.py:532` | Outside |
| `last_bar_ms` | RW `.preroll.last_bar_ms` | integer or null | selected R | `docker/selected_window_report.py:533` | Outside |
| `shortfall` | RW `.preroll.shortfall` | boolean | selected R | `docker/selected_window_report.py:534` | Outside |
| `lookback` | RW `.lookback` | object | selected R; producer not implemented | `docker/selected_window_report.py:536` | Outside |
| `declared_required_chart_bars` | RW `.lookback.declared_required_chart_bars` | null | this build always | `docker/selected_window_report.py:430` | Outside |
| `status` | RW `.lookback.status` | string unknown | this build always | `docker/selected_window_report.py:431` | Outside |
| `has_unbounded_state` | RW `.lookback.has_unbounded_state` | null | this build always, not true/false | `docker/selected_window_report.py:432` | Outside |
| `counts` | RW `.counts` | object | selected R | `docker/selected_window_report.py:537` | Outside |
| `supplied_input_bars` | RW `.counts.supplied_input_bars` | integer | selected R | `docker/selected_window_report.py:538` | Outside |
| `supplied_script_bars` | RW `.counts.supplied_script_bars` | integer | selected R | `docker/selected_window_report.py:539` | Outside |
| `fed_input_bars` | RW `.counts.fed_input_bars` | integer | selected R | `docker/selected_window_report.py:540` | Outside |
| `fed_script_bars` | RW `.counts.fed_script_bars` | integer | selected R | `docker/selected_window_report.py:541` | Outside |
| `preroll_input_bars` | RW `.counts.preroll_input_bars` | integer | selected R | `docker/selected_window_report.py:542` | Outside |
| `window_input_bars` | RW `.counts.window_input_bars` | integer | selected R | `docker/selected_window_report.py:543` | Outside |
| `window_script_bars` | RW `.counts.window_script_bars` | integer | selected R | `docker/selected_window_report.py:544` | Outside |
| `equity_points` | RW `.counts.equity_points` | integer | selected success: window_script_bars+1 | `docker/selected_window_report.py:545` | Outside |
| `anchor_points` | RW `.counts.anchor_points` | integer 1 | selected success | `docker/selected_window_report.py:546` | Outside |
| `metering` | RW `.metering` | object | selected R | `docker/selected_window_report.py:548` | Outside |
| `billable_input_bars` | RW `.metering.billable_input_bars` | integer | selected R | `docker/selected_window_report.py:549` | Outside |
| `preroll_billable_input_bars` | RW `.metering.preroll_billable_input_bars` | integer 0 | selected R | `docker/selected_window_report.py:550` | Outside |
| `basis` | RW `.metering.basis` | string | selected R | `docker/selected_window_report.py:551` | Outside |
| `indices` | RW `.indices` | object | selected R | `docker/selected_window_report.py:553` | Outside |
| `fed_script_index_of_window_first` | RW `.indices.fed_script_index_of_window_first` | integer or null | null for empty script window | `docker/selected_window_report.py:554` | Outside |
| `supplied_script_index_of_window_first` | RW `.indices.supplied_script_index_of_window_first` | integer or null | same condition | `docker/selected_window_report.py:555` | Outside |
| `report_shape` | J `$.report_shape` | object | selected success, including fd route | `docker/run_json.py:4682` | Outside |
| `version` | J `$.report_shape.version` | integer 1 | selected report shape | `docker/selected_window_report.py:563` | Outside |
| `name` | J `$.report_shape.name` | string | selected report shape | `docker/selected_window_report.py:564` | Outside |
| `curve_point_budget` | J `$.report_shape.curve_point_budget` | null | current full/v1 shape | `docker/selected_window_report.py:565` | Outside |
| `canonical_equity_points` | J `$.report_shape.canonical_equity_points` | integer | current full/v1 shape | `docker/selected_window_report.py:566` | Outside |
| `emitted_equity_points` | J `$.report_shape.emitted_equity_points` | integer | same count, even though fd stdout omits rows | `docker/selected_window_report.py:567` | Outside |
| `canonical_trades` | J `$.report_shape.canonical_trades` | integer | current full/v1 shape | `docker/selected_window_report.py:568` | Outside |
| `emitted_trades` | J `$.report_shape.emitted_trades` | integer | same count, even though fd stdout omits rows | `docker/selected_window_report.py:569` | Outside |
| `retained_point_indices` | J `$.report_shape.retained_point_indices` | null | current full/v1 shape | `docker/selected_window_report.py:570` | Outside |
| `curve_decimated` | J `$.report_shape.curve_decimated` | boolean false | current full/v1 shape | `docker/selected_window_report.py:571` | Outside |
| `phase_timing` | J `$.diagnostics.phase_timing` | object | selected success only | `docker/run_json.py:4732` | Outside |
| `version` | J `$.diagnostics.phase_timing.version` | integer 1 | phase timing | `docker/run_phase_transport.py:286` | Outside |
| `execution_ms` | J `$.diagnostics.phase_timing.execution_ms` | number | phase timing | `docker/run_phase_transport.py:350` | Outside |
| `result_assembly_ms` | J `$.diagnostics.phase_timing.result_assembly_ms` | number | phase timing | `docker/run_phase_transport.py:351` | Outside |
| `results_digest_ms` | J `$.diagnostics.phase_timing.results_digest_ms` | number or null | null if digest phase skipped | `docker/run_phase_transport.py:352` | Outside |
| `timing` | J `$.diagnostics.timing` | object | --bench, path routes only; fd forbids --bench | `docker/run_json.py:5221` | Outside |
| `mode` | J `$.diagnostics.timing.mode` | string run_backtest_full | benchmark timing | `docker/run_json.py:4057` | Outside |
| `warmup` | J `$.diagnostics.timing.warmup` | integer | benchmark timing | `docker/run_json.py:4058` | Outside |
| `repeats` | J `$.diagnostics.timing.repeats` | integer | benchmark timing | `docker/run_json.py:4059` | Outside |
| `samples_ns` | J `$.diagnostics.timing.samples_ns` | integer array | benchmark timing | `docker/run_json.py:4060` | Outside |
| `magnifier` | J `$.diagnostics.timing.magnifier` | object | benchmark timing | `docker/run_json.py:4061` | Outside |
| `enabled` | J `$.diagnostics.timing.magnifier.enabled` | boolean | benchmark timing | `docker/run_json.py:4062` | Outside |
| `samples` | J `$.diagnostics.timing.magnifier.samples` | integer | benchmark timing | `docker/run_json.py:4063` | Outside |
| `dist` | J `$.diagnostics.timing.magnifier.dist` | string | benchmark timing | `docker/run_json.py:4064` | Outside |
| `volume_weighted` | J `$.diagnostics.timing.magnifier.volume_weighted` | boolean | benchmark timing | `docker/run_json.py:4065` | Outside |
| `throughput` | J `$.diagnostics.throughput` | object | --bench, path routes only | `docker/run_json.py:5222` | Outside |
| `items_processed` | J `$.diagnostics.throughput.items_processed` | integer | benchmark throughput | `docker/run_json.py:4075` | Outside |
| `samples_ns` | J `$.diagnostics.throughput.samples_ns` | integer array | benchmark throughput | `docker/run_json.py:4076` | Outside |
| `magnifier_mode` | J `$.diagnostics.throughput.magnifier_mode` | string with_magnifier/no_magnifier | benchmark throughput | `docker/run_json.py:4077` | Outside |
| `record` | P `$.record` | string run_phase | each phase receipt | `docker/run_phase_transport.py:358` | Outside |
| `version` | P `$.version` | integer 1 | each phase receipt | `docker/run_phase_transport.py:359` | Outside |
| `sequence` | P `$.sequence` | integer, contiguous from 1 | each phase receipt | `docker/run_phase_transport.py:360` | Outside |
| `scope` | P `$.scope` | string run | each phase receipt | `docker/run_phase_transport.py:361` | Outside |
| `trial_id` | P `$.trial_id` | null | each run phase receipt | `docker/run_phase_transport.py:362` | Outside |
| `phase` | P `$.phase` | string phase name | each phase receipt | `docker/run_phase_transport.py:363` | Outside |
| `elapsed_ms` | P `$.elapsed_ms` | number | each phase receipt | `docker/run_phase_transport.py:364` | Outside |
| `execution_ms` | P `$.execution_ms` | number | completed only | `docker/run_phase_transport.py:350`; `docker/run_phase_transport.py:369` | Outside |
| `result_assembly_ms` | P `$.result_assembly_ms` | number | completed only | `docker/run_phase_transport.py:351`; `docker/run_phase_transport.py:369` | Outside |
| `results_digest_ms` | P `$.results_digest_ms` | number or null | completed only, null when skipped | `docker/run_phase_transport.py:352`; `docker/run_phase_transport.py:369` | Outside |
| `serialization_ms` | P `$.serialization_ms` | number | completed only, after stdout flush | `docker/run_phase_transport.py:368` | Outside |
| `terms` | B `$.terms` | object | every calculator return | `scripts/arena_bound.py:465` | Outside |
| `arena` | B `$.terms.arena` | object | every return | `scripts/arena_bound.py:449` | Outside |
| `ohlcv` | B `$.terms.ohlcv` | object | every return | `scripts/arena_bound.py:451`; `scripts/arena_bound.py:455` | Outside |
| `finalizerStream` | B `$.terms.finalizerStream` | object | every return | `scripts/arena_bound.py:452`; `scripts/arena_bound.py:455` | Outside |
| `childPrivate` | B `$.terms.childPrivate` | object | every return; informational | `scripts/arena_bound.py:453`; `scripts/arena_bound.py:455` | Outside |
| `unbounded` | B `$.unbounded` | boolean | exactly whether any of three sizing terms is unresolved | `scripts/arena_bound.py:465` | Outside |
| `unboundedTerms` | B `$.unboundedTerms` | string array | always; ordered subset of arena/ohlcv/finalizerStream only | `scripts/arena_bound.py:462`; `scripts/arena_bound.py:465` | Outside |
| `reason` | B `$.reason` | string | always, empty if three sizing terms finite | `scripts/arena_bound.py:466` | Outside |
| `arenaBytes` | B `$.terms.arena.arenaBytes` | integer or null | every return | `scripts/arena_bound.py:163`; `scripts/arena_bound.py:267` | Outside |
| `bytes` | B `$.terms.arena.bytes` | integer or null | always, identical alias | `scripts/arena_bound.py:444` | Outside |
| `inputs` | B `$.terms.arena.inputs` | object | always, including unknown values | `scripts/arena_bound.py:163`; `scripts/arena_bound.py:267` | Outside |
| `T` | B `$.terms.arena.inputs.T` | object | always | `scripts/arena_bound.py:89`; `scripts/arena_bound.py:202` | Outside |
| `D` | B `$.terms.arena.inputs.D` | object | always | `scripts/arena_bound.py:89`; `scripts/arena_bound.py:215` | Outside |
| `Q` | B `$.terms.arena.inputs.Q` | object | always | `scripts/arena_bound.py:89`; `scripts/arena_bound.py:248` | Outside |
| `L` | B `$.terms.arena.inputs.L` | object | always | `scripts/arena_bound.py:89`; `scripts/arena_bound.py:224` | Outside |
| `R` | B `$.terms.arena.inputs.R` | object | always | `scripts/arena_bound.py:89`; `scripts/arena_bound.py:252` | Outside |
| `E` | B `$.terms.arena.inputs.E` | object | always | `scripts/arena_bound.py:89`; `scripts/arena_bound.py:179` | Outside |
| `value` | B `$.terms.arena.inputs.{T,D,Q,L,R,E}.value` | integer or null | every input; partly derived inputs can survive a later missing proof | `scripts/arena_bound.py:84` | Outside |
| `derivation` | B `$.terms.arena.inputs.{T,D,Q,L,R,E}.derivation`; B `$.terms.{arena,ohlcv,finalizerStream,childPrivate}.derivation` | object | every input and term | `scripts/arena_bound.py:85`; `scripts/arena_bound.py:445`; `scripts/arena_bound.py:276` | Outside |
| `formula` | BD `.formula` | string | every derivation | `scripts/arena_bound.py:85`; `scripts/arena_bound.py:276`; `scripts/arena_bound.py:446` | Outside |
| `sources` | BD `.sources` | string array | every derivation | `scripts/arena_bound.py:85`; `scripts/arena_bound.py:276`; `scripts/arena_bound.py:447` | Outside |
| `unbounded` | B `$.terms.{arena,ohlcv,finalizerStream,childPrivate}.unbounded` | boolean | each term; private flag never enters top-level verdict | `scripts/arena_bound.py:164`; `scripts/arena_bound.py:275`; `scripts/arena_bound.py:418` | Outside |
| `reason` | B `$.terms.arena.reason` | string | arena unavailable only; other terms use derivation.sources | `scripts/arena_bound.py:164`; `scripts/arena_bound.py:443` | Outside |
| `residentBytes` | B `$.terms.ohlcv.residentBytes` | integer or null | always | `scripts/arena_bound.py:282`; `scripts/arena_bound.py:317` | Outside |
| `asBytes` | B `$.terms.ohlcv.asBytes` | integer or null | always | `scripts/arena_bound.py:282`; `scripts/arena_bound.py:317` | Outside |
| `bytes` | B `$.terms.finalizerStream.bytes` | integer or null | always | `scripts/arena_bound.py:331`; `scripts/arena_bound.py:383` | Outside |
| `recordCounts` | B `$.terms.finalizerStream.derivation.recordCounts` | object | certified finite stream term | `scripts/arena_bound.py:387` | Outside |
| `maxLineBytes` | B `$.terms.finalizerStream.derivation.maxLineBytes` | object | certified finite stream term | `scripts/arena_bound.py:387` | Outside |
| `E` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.E` | integer | finite stream | `scripts/arena_bound.py:342`; `scripts/arena_bound.py:367` | Outside |
| `T` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.T` | integer | finite stream | `scripts/arena_bound.py:342`; `scripts/arena_bound.py:367` | Outside |
| `O` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.O` | integer | finite stream, zero when disabled | `scripts/arena_bound.py:343`; `scripts/arena_bound.py:368` | Outside |
| `V` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.V` | integer | finite stream; disabled count zero, size can be nonzero | `scripts/arena_bound.py:343`; `scripts/arena_bound.py:374` | Outside |
| `K` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.K` | integer | finite stream, zero when disabled | `scripts/arena_bound.py:353`; `scripts/arena_bound.py:371` | Outside |
| `X` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.X` | integer | finite stream; disabled count zero | `scripts/arena_bound.py:346`; `scripts/arena_bound.py:375` | Outside |
| `H` | B `$.terms.finalizerStream.derivation.{recordCounts,maxLineBytes}.H` | integer | finite stream; disabled count zero | `scripts/arena_bound.py:347`; `scripts/arena_bound.py:376` | Outside |
| `rssBytes` | B `$.terms.childPrivate.rssBytes` | integer or null | always; supplied numeric value may remain uncertified | `scripts/arena_bound.py:396`; `scripts/arena_bound.py:407` | Outside |
| `asBytes` | B `$.terms.childPrivate.asBytes` | integer or null | always, not inferred from RSS | `scripts/arena_bound.py:396`; `scripts/arena_bound.py:407` | Outside |
| `measured` | B `$.terms.childPrivate.measured` | boolean | always, RSS provenance only | `scripts/arena_bound.py:396`; `scripts/arena_bound.py:418` | Outside |
| `rssCertified` | B `$.terms.childPrivate.derivation.rssCertified` | boolean | complete private record processed | `scripts/arena_bound.py:422` | Outside |
| `asCertified` | B `$.terms.childPrivate.derivation.asCertified` | boolean | complete private record processed | `scripts/arena_bound.py:422` | Outside |
| `equity` | U `$.equity` | integer | helper called with finite inputs | `scripts/arena_bound.py:136` | Outside |
| `positionTail` | U `$.positionTail` | integer | same | `scripts/arena_bound.py:137` | Outside |
| `cohorts` | U `$.cohorts` | integer | same | `scripts/arena_bound.py:138` | Outside |
| `exitRank` | U `$.exitRank` | integer | same | `scripts/arena_bound.py:139` | Outside |
| `sequenceData` | U `$.sequenceData` | integer | same | `scripts/arena_bound.py:140` | Outside |
| `sequenceIndex` | U `$.sequenceIndex` | integer | same | `scripts/arena_bound.py:141` | Outside |
| `erasedData` | U `$.erasedData` | integer | same | `scripts/arena_bound.py:142` | Outside |
| `erasedIndex` | U `$.erasedIndex` | integer | same | `scripts/arena_bound.py:143` | Outside |
| `consumedData` | U `$.consumedData` | integer | same | `scripts/arena_bound.py:144` | Outside |
| `consumedIndex` | U `$.consumedIndex` | integer | same | `scripts/arena_bound.py:145` | Outside |
| `successorRoots` | U `$.successorRoots` | integer | same | `scripts/arena_bound.py:146` | Outside |

The C structs are separate ABI objects, not JSON documents:
pf_report_sink_fd_v1, the 144-byte pf_report_sink_receipt_v1 and the
64-byte pf_report_sink_summary_v1 keep their layouts
(`include/pineforge/report_sink.h:57`,
`include/pineforge/report_sink.h:73`,
`include/pineforge/report_sink.h:107`). In particular struct_size,
reserved, trades_retained, nonfinite_t_field and sha256_hex are not
additional finalizer JSON receipt keys. ScratchArenaStats also has
internal total/used/free/mapped/backend fields
(`include/pineforge/detail/scratch_arena.hpp:44`); the additive getter
exposes only high-water and supplied bytes. Do not invent JSON paths
for those internal members.

## 5. Required wrapper lifecycle

1. Resolve request/data and any trusted compiler facts, call the one
   calculator, retain unresolved sizing reasons, and obtain COMPUTE's
   attempt-0 placement. Unknown does not select the largest rung.
   Before starting the stdout finalizer obtain a positive planned U;
   its fallback value when stream facts are unavailable is
   **DECISION FOR COMPUTE/APP**, not a number supplied by this calculator.
2. Provision private staging/output directories and the exact arena
   extent. Create sink/receipt pipes and the selected phase socket.
   Start the finalizer with its blocking sink read peer, separate
   receipt writer, --out - and --max-bytes U. Root drains provisional
   stdout to its own mode-0600 staging file.
3. Spawn the child with the wrapper fd convention, environment and
   --report-sink-fd 4. Close all unused endpoint copies. Required engine
   integration must attach before any scratch store opens; section 6
   explains why that is not a delivered startup guarantee yet.
4. Drain finalizer stdout/receipt/stderr and child stdout/stderr
   concurrently. The child captures its native receipt/summary, queries
   optional arena diagnostics, frees state and closes the sink before
   publishing its success JSON. It never waits on a finalizer that
   still needs EOF from that endpoint.
5. Require child exit 0 plus a valid success document, finalizer exit 0
   plus a complete receipt, requested run ID validation and a successful
   output drain/fsync/close before root renames staging to its dedicated
   PATH on the same filesystem. Current fd stdout exposes neither sink
   digest nor all seven engine receipt counts; do not require comparison
   to a nonexistent descriptor. Validate available summary/selection
   counts against the verified stream. Any additional engine-receipt
   transport/agreement is **DECISION FOR COMPUTE/APP**.
6. On any failed child, nonzero finalizer, incomplete/missing receipt,
   signal or failed output drain, discard all provisional bytes. A
   trailer or finalizer exit 0 cannot override a failed child. Close
   wrapper-held sink writers so EOF can arrive; stop/reap both processes
   as needed, close descriptors and remove unaccepted output, spools,
   arena and private directories. The external reaper owns cleanup
   after SIGKILL/OOM.

Arena exhaustion follows its captured typed failure through cleanup,
then uses the trusted next-rung policy and alert. Sink exhaustion is
selected from finalizer exit 8 and its receipt, even if the writer also
reports a peer closure; preserve both diagnostics and use the
catalog-prescribed refund/2*U re-placement. Exact wrapper precedence
against an independently failing child, and mappings for other
finalizer statuses, are **DECISION FOR COMPUTE/APP**. The finalizer
does not manufacture another catalog reason for those statuses.

Trusted-side disk preallocation failure happens before spawn. It is
COMPUTE/APP provisioning work, not a child growth syscall or a newly
invented strategy cap-refusal class. The finalizer neither inherits nor
hashes arena contents; its verification covers the sink stream.

## 6. Current gaps and decisions

These are source findings at a2f3dfdb84, not test results or predictions
about a later W1/port merge:

- **Attachment and ports:** both sink setters configure state without
  attaching (`src/engine_report.cpp:699`, `src/engine_report.cpp:735`).
  The detail facade supports explicit attachment, but IdPairScratchFile
  still grows a real file (`include/pineforge/detail/id_pair_store.hpp:84`).
  The report ScratchFile is a different file-backed class: its constructor
  opens a private file and its writes use pwrite
  (`src/report_scratch.cpp:134`, `src/report_scratch.cpp:152`).
  Position tail binds that class (`src/source/pine_position_tail_binding.hpp:30`).
  A fd sink therefore does not yet imply a file-free, fully arena-backed
  strategy under FSIZE=0. All store labels/remaining readers must be
  integrated before that claim.
- **Typed C++ transport:** begin, finish and callback writer translations
  still construct report_sink_failed with empty args
  (`src/engine_report.cpp:841`, `src/engine_report.cpp:913`,
  `src/engine_report.cpp:1051`). The selected/held/trade translations do
  likewise (`src/native_execution_consumer.cpp:13208`,
  `src/native_execution_consumer.cpp:13380`,
  `src/native_execution_consumer.cpp:13502`).
  Legacy IdPair/report-scratch errors also carry empty args
  (`include/pineforge/detail/id_pair_store.hpp:35`,
  `src/report_scratch.cpp:35`). W2 forwards/mirrors supplied args;
  it does not repair those missing C++ causes. Keep the 2^63 demand
  edge and cached-window size-validation gap visible until fixed.
- **APP keep-set:** the inspected jail stores one keep_fd and scrubs
  around it (`APP apps/engine/src/jail/pf-jail.c:270`,
  `APP apps/engine/src/jail/pf-jail.c:314`). It cannot yet preserve the
  required set {3,4,5}. Its exit-118 failure constant is at
  `APP apps/engine/src/jail/pf-jail.c:63`. APP must implement the set,
  descriptor validation and root staging lifecycle; no new keep-set
  CLI spelling is assumed here.
- **Summary/converter:** producer-side 2-MiB enforcement is absent.
  The proposed post-success sink-to-JSON bridge is not a delivered tool
  in this tree. **DECISION FOR COMPUTE/APP:** its validated metadata
  schema, reader cap, verified-count reconciliation, upload pipeline
  and memory/storage overlap. Do not infer a numeric Python RSS bound
  from 64-KiB I/O buffers or a one-MiB wire-line ceiling.
- **Abort/progress:** the proposed SIGTERM cooperative handler, completed
  bars/start witnesses and fd-6 progress slot are not this invocation's
  emitted interface. No fd 6 is added to the keep-set and no proposed
  failure keys appear in the table. **DECISION FOR COMPUTE/APP:** the
  cancellation/progress interface and ownership. Existing catalog
  run_aborted has empty args (`docker/run_failure_codes.json:1109`);
  its name does not implement that signal path.
- **Facts and placement:** the compiler/lowering facts producer,
  applicable private RSS/AS evidence, default arena/U, trusted rung
  sizes, post-escalation behavior, disk rollout/quotas, service headroom
  and report-pipeline changes remain **DECISION FOR COMPUTE/APP** with
  CORE/compiler/lab deliverables where stated above. No census number
  or measured flat-heap acceptance is claimed by this edit.

The supervisor's committed head, TypeSafe review and lab doc-anchor/
verification tooling judge the delivered document. No local validation
result is implied by this source audit.
