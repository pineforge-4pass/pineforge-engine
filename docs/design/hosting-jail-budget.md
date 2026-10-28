# Hosting jail budget contract

**Status: PROPOSED for 1.8.0.** This document addresses the identified
resource-accounting gap. It covers the byte-cap convention in which 0 means
no caller limit and the cores-per-run analysis. It specifies work for the hosting layer,
the engine implementers, the certified-facts producer and the verification
team. It does not certify a deployed host.

## Purpose and scope

Every hosted invocation needs a finite resource reservation, installed limits,
authenticated installation evidence, and measured usage on both success and
failure. Compilation is part of that invocation. A successful scratch-arena
admission is insufficient evidence that the invocation is resource confined.

The threat model is untrusted native strategy code in the Python run process,
including shared-library constructors, callbacks, threads, destructors and
direct syscalls. The compiler and code generator process untrusted input.
The hosting supervisor, kernel, immutable runtime image and host verification
agent are trusted. Strategy-visible output, error codes and phase messages
are untrusted evidence of host actions. A strategy must not possess the
supervisor's signing capability, mutable policy, terminal-receipt endpoint,
or authority to change another stage's resources.

This contract covers source preparation/codegen, compile/link and inventory
binding, strategy execution, report preparation, stream verification,
publication, diagnostics, and teardown. It includes descendants and resources
retained after a process exits. It does not promise that arbitrary strategies
finish within the reservation, predict arbitrary compiler complexity, change
trading semantics, specify billing/refund policy, or certify the durable live
runner or a whole HPO job.

The [report-channel contract](report-channel.md) owns channel
separation, framing and publication. That document is a parallel dependency,
not an implemented EM feature. This contract supplies its byte reservations,
deadlines, trusted terminal outcome and usage. It does not introduce another
report wire format.

### Evidence and terminology

`EM` means engine `95f4fd1f408bf0ded941ecc4144b278a026a677f`;
`CM` means codegen `af1778b9069d534506db46dbec945362e3550a4a`.
`EM:path:line` and `CM:path:line` identify those immutable sources.
Documentation citations describe the published engine contract; they are
identified as such rather than offered as a host installation check.

The other read inputs are an older `ANSWER-v2.md` draft, the cores-per-run
analysis, the arena capacity analysis, the report-channel contract and
`hub-entrypoint-v1` handoffs. Their older source pins and measurements do not
establish what is installed on today's host.

Everything normative below is **PROPOSED**, including all new schema names,
field names, code additions, policy numbers and command-line options.
Existing names and constants are explicitly cited. A proposed policy
reservation is a hard admission ceiling, not a certified workload bound or
a measured default. Requirements are identified as R1–R18; their tests and
the contract-table tests form the conformance criteria.

Bytes mean octets; MiB and GiB mean powers of 1024. Durations in new records
are integer microseconds unless the name says otherwise. Counts are
nonnegative integers. Arithmetic is checked before rounding; admission
values fit signed 64-bit integers and the relevant kernel type. Unknown is
distinct from zero. Kernel `max`/`RLIM_INFINITY` is not a finite value.

## Current behaviour at EM

| Stage | Engine-local behaviour and limits | Host assumption; consequence when absent |
| --- | --- | --- |
| Source preparation / codegen | The shell creates `WORK` under `TMPDIR` or `/tmp`, installs an EXIT cleanup trap, and writes generated C++ and optional inventory. Transpile exceptions handled there become shell exit 5. It installs no OS resource limits in these functions. `docker/entrypoint.sh`. | Codegen's own source-character limit is 5 MiB, nesting limit 512, and cooperative transpile timer 120 seconds at CM; these are not a hard process/tree budget. `pineforge_codegen/limits.py`. The engine wrapper supplies no memory, temp, fd, task or hard-deadline installer: `docker/entrypoint.sh`. |
| Compile/link / bind | One literal `g++ -std=c++17 -O2 -ffp-contract=off -fPIC -shared` invocation links the whole static archive. Compiler failure becomes shell exit 3; selected inventory binding follows under the same shell, also with exit 3 on failure. No CPU/wall/RSS/temp limit surrounds these calls. `docker/entrypoint.sh`. | Compiler descendants, peak RSS, optimizer time, diagnostics and temporary amplification are not bounded by this command. The codegen syntax-check helper likewise calls `subprocess.run` without a timeout; it is a test helper, not a production compiler jail. `tests/_compile.py`. A source-size check does not establish compiler cost. |
| Load / run | `ctypes.CDLL` loads the caller's native module into the harness process. Jailed setup installs a bounded scratch manifest and requires arena admission for each fresh handle; the profile rejects path sinks and `--spill-dir`. `docker/run_json.py`. | `jailed` does not install or verify AS, cgroup memory, disk quota, CPU, wall, fd or task limits. The source calls the already loaded library rather than a resource launcher. `docker/run_json.py`, `docker/entrypoint.sh`. Missing OS limits leave resources outside the engine-local sublimits uncontrolled. |
| Report preparation inside run_json | Jailed/fd routes serialize into `TextIOWrapper(BytesIO())`; other routes can use a file temporary. Buffer preparation precedes native cleanup, so native state, arena backing and JSON can coexist. There is no whole-report byte check in this preparer. `docker/run_json.py`. | AS/RSS and report budgets are external assumptions. The output-column route creates three temporaries plus one per series entry; it does not first admit a descriptor budget. `docker/run_json.py`. Whole-process memory and output-slot fan-out remain outside the arena capacity check. |
| Publication from run_json | `_write_report` writes and flushes stdout; a partial-report write failure returns 1 and does not append another JSON document in that handler. Phase-aware post-execution exceptions have typed classification. No deadline is installed there. `docker/run_json.py`. | A blocked writer needs an outside deadline. `report_post_execution_failed` already permits `reason=deadline`, but vocabulary alone does not install a watchdog. `docker/run_failure_codes.json`. |
| Separate sink finalizer / host publication | `--out -` requires a positive `--max-bytes`; file mode makes that cap optional, and omission leaves total input uncapped. Overflow reads at most B+1 bytes and exits 8. File mode creates a spool, syncs it, verifies it, then renames it. `docker/pineforge_sink_finalize.py`. | Its `RLIMIT_FSIZE=0`/`--no-fork` text describes a host jail; it does not install one. File mode explicitly needs a writable environment without that zero file-size limit. `docker/pineforge_sink_finalize.py`. No EOF/deadline or aggregate spool filesystem quota is established by the helper. Its I/O failure is exit 5, not proof of a particular quota cause. `docker/pineforge_sink_finalize.py`. |

### Engine-local sublimits are not whole-invocation limits

Arena attachment checks the existing backing's exact size and seals; it does
not create, resize or preallocate it. Allocatable capacity is the supplied
size rounded down to 1 MiB chunks. Claim/grow checks precede allocation;
exhaustion carries `report_sink_failed(reason=arena_exhausted)` and an
arena snapshot. The mapping cap defaults to 32 MiB, rounded down to pages,
and evicts before replacing a mapping. These are actual implementation
checks: `src/scratch_arena.cpp`.

Neither capacity nor the mapping window equals charged physical memory.
Attachment initializes bitmap/window metadata; growth zeroes entire newly
claimed chunks. Unmapping does not release the backing's charged pages.
`src/scratch_arena.cpp`. The public scratch-table descriptor also
reserves a separate slab; its P0 root allowance is 2N, not arena space.
`docker/run_json.py`, `include/pineforge/pineforge.h`.

The published EM memory contract specifies a 64-row trade-ledger descriptor
window and equally sized payload tail, plus a separate read cache. Those
windows do not bound total ledger text, backing or private heap.
`docs/pages/memory-management.md`. The same published contract
describes `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES`: positive decimal uint64,
unset means uncapped, and the cap covers command backings of one thread,
not all run files. Malformation is `scratch_configuration_invalid`;
exhaustion is `scratch_io_failed`.
`docs/pages/memory-management.md`,
`docker/run_failure_codes.json`. These published sublimits are
retained; neither is accepted as evidence of a host quota.

The host dependency is explicit: the embedding host sets `RLIMIT_FSIZE`;
arena attachment ignores SIGXFSZ so EFBIG can be reported rather than
killing that process. `include/pineforge/pineforge.h`,
`include/pineforge/detail/scratch_arena.hpp`.
The arena calculator's `RLIMIT_AS` / typed `memory_limit` language is
likewise a hosting assumption, not a limit installer.
`scripts/arena_bound.py`. With an omitted host limit there is no
corresponding whole-resource protection; with an OS kill there is no
guarantee that in-process exception handling produces any failure line.
`docker/run_json.py`.

Current success diagnostics expose profile and arena capacity/high water,
not OS confinement or whole-process usage.
`docker/run_json.py`.
`elapsed_seconds` surrounds the native run interval and excludes the
earlier compiler and later JSON publication.
`docker/run_json.py`. The finalizer receipt has sink bytes/counts
and a digest, not a compiler/RSS/CPU/disk/fd/task usage block.
`docker/pineforge_sink_finalize.py`.

## Stage and reservation model

**R1 — complete ownership (T1, T18, T19, T26, T30).** The host MUST put
every invocation process and helper in the following ownership tree before
it processes untrusted input. Each stage has a fresh child cgroup, a
process-start identity, a proof and a deadline. Descendants inherit that
stage. Compiler exit alone does not end its stage while descendants live.
The job root has a `workload` subtree containing the five workload leaves
and a separately protected `supervisor` leaf. A job stop kills `workload`,
not the receipt-producing supervisor. The trusted outer host agent remains
able to finish the terminal record if that supervisor itself fails.

| Stage identifier | Charged interval and owner |
| --- | --- |
| `codegen` | Source snapshot/read, Python startup, transpile and generated-source/inventory writes. The direct-C++ path records this stage as not started if no equivalent preparation runs. |
| `compile` | Compiler driver, frontend, assembler, linker, all helpers, and selected inventory binding. Helpers get subordinate launch identities under the same cumulative stage budget; they do not require a modified GCC binary or a new stage allowance. |
| `run` | Python startup, feed preparation, every strategy load including capabilities queries, settings, native execution, native/Python report preparation, cleanup, buffered/destructor output, and process exit. |
| `report_assembly` | A trusted external report-channel consumer's validation/assembly of provisional producer output. It never loads the strategy module. Its work can overlap `run`. |
| `finalize` | Sink verification/finalizer where used, output spooling, flush/sync and publication preparation. Every finalizer, drainer or publisher is assigned here or to `report_assembly`, never left outside the reservation. |
| `supervisor` | Proof/admission, causal-event collection, terminal receipt and cleanup coordination. It is isolated from the untrusted leaves and has its own finite reserve. |
| `job` | Parent envelope from initial preparation through terminal publication and bounded cleanup. It includes all stages, retained page cache, arena backing and provisional artifacts. |

The internal Python `result_assembly` phase remains inside `run` for
resource enforcement. A phase message from a process containing native
strategy code cannot authorize a larger cap, reset a timer, or establish
that the strategy has ceased executing. An independently budgeted trusted
report worker requires a real host-created process boundary. This prevents
the existing preparation-before-free lifetime from escaping the reservation.

**R2 — finite plan (T1–T4, T25, T26, T30).** The host and engine MUST
resolve every applicable table row to a finite installed value before
admission. A request cannot select a larger tier after a failure without a
new attempt identity and reservation. Missing workload bounds select a
finite policy tier, with the unknown terms recorded; they never select
unlimited. No bar/trade/object count is invented to manufacture a proof.

### Proposed reference policy

These are implementation defaults recommended to the hosting layer, not current
engine defaults. The policy identifier is **PROPOSED**
`hosting-jail-budget/v1-reference`. A different deployed policy supplies
all the same values and passes the same tests.

Let A be arena capacity, W its mapping window, N the admitted input-bar
count, U the admitted sink-stream bytes, and J the admitted final JSON bytes.
The receipt records each value, source and whether it is a certified bound
or a policy allowance.

| Quantity | PROPOSED default or resolution rule | Derivation / limitation |
| --- | --- | --- |
| A | Certified arena requirement if available; otherwise 4 GiB, rounded up to whole engine chunks | A finite starting policy, not a promise arbitrary strategies fit. The existing calculator requires certified facts and leaves unknowns explicit: `docs/design/arena-bound.md`, `scripts/arena_bound.py`. |
| W | Existing 32 MiB default, or an explicitly admitted positive page multiple | Existing engine default: `include/pineforge/detail/scratch_arena.hpp`, `src/scratch_arena.cpp`. It is not substituted for A in physical-memory admission. |
| S(N), the slab | Exact admitted manifest layout; for the documented P0 batch: `61,128,704 + 4096*ceil(32*N/4096)` bytes | Existing P0 hand arithmetic and extra-memory exclusions: `docs/pages/scratch-tables.md`. Explicit manifests use their actual layout, not this shortcut. |
| Input resident/AS allowances, O_r/O_a | Corresponding independently certified OHLCV terms; otherwise 1 GiB each | A resident observation cannot certify AS. Unknown CSV/parser/copy overhead remains subject to the whole-stage caps. Existing calculator separates these terms: `scripts/arena_bound.py`. |
| U | Certified `finalizerStream.bytes` if available; otherwise 64 MiB | A finite policy cap, not a prediction. Existing stream arithmetic is conditional: `scripts/arena_bound.py`. |
| J | Explicit report-channel reservation; otherwise 8 MiB | New policy recommendation only. There is no EM 8 MiB report cap in `_prepare_report_json`: `docker/run_json.py`. U and J are independent, including escaping/expanded JSON. |
| Run physical cap M_r | `page(A + S(N) + O_r + 1 GiB + 2*J + 64 KiB)` | A full arena-charge allowance, slab, input allowance, finite private allowance, two encoded-report allowances and a transfer buffer. This is a policy envelope, not a proof of allocator overhead or native-array sufficiency. |
| Run AS cap V_r | `page(S(N) + O_a + W + 2 GiB + 2*J + 64 KiB)` | Includes virtual image/stack/private headroom as a finite allowance. Only the arena mapping window enters AS; all possible arena backing enters M_r. Actual mappings and address-space fragmentation still have to fit. |
| Shared disk quota D | `fsblock(64 MiB + 512 MiB + 2*U + 2*J + 64 MiB)` | Reserves preparation, compiler temporary growth, coexisting spool/output copies and control/metadata allowance. With default U/J this is 784 MiB before filesystem rounding. This is one aggregate ceiling, not independent 512 MiB compiler enforcement. Compile's hard temp ceiling is the remaining shared D. |
| Shared inode/file quota F | 512 charged inodes, including directories, linked artifacts and unlinked-but-open files | A finite policy allowance for compiler and report fan-out, not a source-derived universal file count. Required report-column/artifact counts are checked against the remaining allowance before their creation. |
| Deadline delivery allowance E | 100,000 microseconds | Proposed maximum host stop latency, separately recorded and tested. A host that cannot bound this uses a stronger outer runtime stop or refuses admission. |
| Cleanup allowance | 10 seconds | Proposed finite job reservation. Quotas/reservations remain owned if cleanup fails; they are not made available to another job. |

`page` rounds up to the installed host page size; `fsblock` rounds up to
the quota allocation unit. Neither rounding operation silently increases
a caller-authorized ceiling: the rounded reservation itself is admitted.

| Stage | Physical cap / AS cap | Wall budget / per-process RLIMIT_CPU soft=hard | RLIMIT_NOFILE | RLIMIT_NPROC / pids.max |
| --- | --- | --- | --- | --- |
| `codegen` | 512 MiB / 1 GiB | 120 s / 120 CPU s | 64 | 1 / 1 |
| `compile` | 2 GiB / 4 GiB | 120 s / 120 CPU s | 128 | 32 / 32 |
| `run` | M_r / V_r | 120 s / 120 CPU s | 64 | 1 / 1 |
| `report_assembly` | 512 MiB / 1 GiB | 30 s / 30 CPU s | 64 | 1 / 1 |
| `finalize` | 256 MiB / 512 MiB | 30 s / 30 CPU s | 64 | 1 / 1 |
| `supervisor` | 64 MiB / 256 MiB | job lifetime / 30 CPU s | 64 | 2 / 2 |

All cells in this stage-policy table are PROPOSED. The codegen 120-second
choice follows CM's cooperative two-minute budget; the other times are
finite starting service policies, not measured performance targets.
Compiler memory/task headroom accommodates an unknown toolchain tree and
requires the real-toolchain tests before deployment. One source-level
compiler invocation is not a process/thread-count certificate.

The reference job parent permits one logical CPU in
`cpuset.cpus.effective`, with `cpu.max="100000 100000"` and
`cpu.max.burst=0`; every child stays within that cpuset. The same maximum
applies to the stage leaves. This follows the one-execution-lane
finding as a reservation recommendation, not a claim about installed
affinity or compiler internals. The synchronous run call is visible at
`docker/run_json.py`.

The job's workload stop deadline is the sum of the five workload-stage wall
budgets: 420 seconds after job start by default. The receipt/cleanup deadline
is a further 10 seconds, giving a 430-second total reservation. Overlap does
not increase either deadline. Stage expiry is the earlier of its own expiry
and the job workload-stop deadline. The final cleanup interval cannot be
spent on new strategy, compile or report production.

The workload parent physical/task caps are the sums of the five workload
leaf caps. The job root physical cap/reservation is the sum of all six,
including retained charges; no sharing credit is taken without a separate
charge/lifetime proof. The job task cap is 38; the workload-parent cap is 36.
All six process leaves and the workload parent use `memory.oom.group=1`;
the job accounting root
uses 0 so its OOM policy does not deliberately group-kill the protected
supervisor. That supervisor's reserve cannot be lent to a workload.
The job CPU reservation is at most one logical CPU times `430 seconds + E`.
A configured wider cpuset instead reserves its cardinality times that bound.
An explicitly admitted external memory-charge owner adds its finite
reservation to that job physical reservation; it does not enlarge any
stage cgroup or disappear as a sharing credit. The default has no such
external owner. The exact rule is given under charge closure below.

`cpu.max` is a rate limit, not a cumulative CPU timer. Aggregate CPU is
hard-bounded here by the finite cpuset and outside wall stop, even if a
compiler repeatedly replaces children and resets their per-process
RLIMIT_CPU accounting. For one run process RLIMIT_CPU additionally caps
that process's user+system CPU. A tighter aggregate CPU ceiling requires
a correspondingly shorter wall envelope or another host enforcement
mechanism with its own proven bound; polling `cpu.stat` alone is insufficient.

## Contract table

The host enforces OS limits. The engine enforces admission and its existing
suballocators. Each row is part of R2 and R3. Proof identifiers P1–P6 are
defined below; failure abbreviations are defined in the following section.

| Limit | Mechanism / unit | Stages; enforcer | Value rule / default | Typed refusal or limit outcome | Installation evidence | Tests |
| --- | --- | --- | --- | --- | --- | --- |
| Address space | Inherited `RLIMIT_AS`, soft=hard; bytes | All processes, including every compiler descendant; host | V_r or stage-policy AS cap | B if absent/weaker; M for a proven AS-denied allocation; ordinary allocator failure retains existing code | P1 | T3, T7, T18, T31 |
| Charged physical memory | cgroup v2 `memory.max`; group OOM on process leaves/workload parent, off at accounting root; bytes | Every leaf and workload/job parent; host | Stage M; workload/job sums as above, with all arena/shmem/cache charges assigned | B at admission; M on proven cgroup OOM/denial | P2, P5 | T3, T7–T9, T26 |
| Memory pressure threshold | `memory.high`; bytes | Every leaf and job parent; host | `floor(9*M/10)`, rounded down to pages; positive and below/equal to M | Throttling/events are usage, not terminal failure. `memory.high` without finite `memory.max` is B | P2 | T3, T7, T31 |
| Swap | `memory.swap.max`; bytes | Every leaf and job parent; host | 0 in reference policy; memory and swap ceilings are recorded separately | B for omitted/nonzero installation under this policy; memory outcome M if actually exhausted | P2 | T3, T7, T8 |
| Arena backing capacity | Existing fd 5 size/seals and allocator chunk checks; bytes | `run`; host creates/charges, engine validates/enforces region capacity | A, exact supplied bytes with whole-chunk capacity | Existing `report_sink_failed{reason:arena_backing}` or `{reason:arena_exhausted,...snapshot}` | P1, P5 plus engine attachment | T6, T8, T9 |
| Arena mapping window | Existing `PF_SCRATCH_WINDOW_BYTES` / allocator LRU; bytes | `run`; engine, admitted by host | W, default 32 MiB; positive page multiple | Existing backing/I/O refusal; never treated as a physical-memory ceiling | P1, P5 | T8, T9 |
| Scratch-table slab/counts | Existing bounded manifest and exact layout; rows/key bytes/layout bytes | `run`; engine | Exact manifest, default P0 S(N), including 2N roots | Existing `scratch_capacity_exceeded{resource}`; existing allocator classification otherwise | P1 binds manifest; engine setter/admission | T1, T9, T29 |
| Ledger resident window | Published descriptor/payload/cache geometry; rows/bytes | `run`; engine-local sublimit | Preserve published 64-row window and distinct cache policy; no total-ledger cap inferred | Preserve existing failures; backing/heap also subject to D/A/M | P1 binds runtime image; not OS proof | T1, T9, T29 |
| Optional command extent cap | Published `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES`; bytes | `run`; engine-local subset | Record unset or an explicit positive decimal. Unset disables only this optional subset cap; A/M/D still impose finite hardware ceilings | Existing `scratch_configuration_invalid` / `scratch_io_failed`; arena share keeps its existing sink code | P1 records effective environment | T1, T10, T29 |
| Aggregate work/storage bytes | Enforced XFS project hard block quota with project inheritance, on a disk-backed per-run directory; bytes after quota-unit normalization | All stage directories, compiler temps, spool/provisional/retained outputs; host | Shared D, remaining capacity is D minus current charge. No grace-period-only/soft quota | L(work_bytes) for proven quota denial; existing I/O code when no quota cause is established | P3, P5 | T3, T10, T18, T21, T26, T28 |
| Aggregate files/inodes | Same project's inode hard quota; charged inode count | All stage directories; host | Shared F=512; includes directories and unlinked open inodes until final release | L(work_files) on proven inode quota denial | P3 | T3, T11, T18, T28 |
| Individual file extent | `RLIMIT_FSIZE`, soft=hard; bytes | `run`: 0; codegen/compile: D; report/finalize: 0 in stream-only worker or D in file/spool worker; supervisor: 64 MiB; host | Stage role selects exact value before launch. No writable regular-file exception in `run` except the already sized, sealed arena mapping | L(file_bytes) on proven EFBIG/limit signal; preserve native specific scratch/sink failures when those are the terminal cause | P1, P3, P5 | T3, T10, T12, T21 |
| File descriptors | `RLIMIT_NOFILE`, soft=hard; descriptor-table index/count bound | All processes; host; engine checks predicted own demand | Stage table; inherited fd numbers below cap. Required column route demand includes current/base fds, 3+P columns, final artifact and readers | L(file_descriptors) for admission demand or proven EMFILE; no generic code inferred from stderr | P1, P4 | T3, T13, T18, T31 |
| Processes and threads | `RLIMIT_NPROC` plus cgroup `pids.max`; tasks | Every leaf and job parent; host | Stage table; dedicated non-root uid for an untrusted stage, no privileged exemption. `run` permits no fork/vfork/clone/clone3-created process or thread | L(tasks) for kernel task limit; V(process_creation) for a prohibited creation syscall | P1, P2, P4 | T3, T14, T15, T19, T31 |
| Per-process CPU time | `RLIMIT_CPU`, soft/hard seconds; actual user+system CPU | All stage processes; host | Stage table, inherited by compiler children; never described as an aggregate tree counter | L(cpu_time) on a kernel-confirmed RLIMIT_CPU event | P1, P4 | T3, T16, T18 |
| CPU rate and placement | cgroup `cpu.max`, burst 0, finite `cpuset.cpus.effective`; microseconds/period and CPU set | Leaves and job parent; host | Reference one logical CPU, quota/period 100000/100000 | Throttling is usage; missing/weaker rate/cpuset is B. Aggregate charged CPU bound uses placement plus wall | P2 | T3, T16, T26 |
| Aggregate CPU allowance | Finite cpuset × outside wall stop, plus per-process RLIMIT_CPU; CPU microseconds | Each stage tree and job; host | `cpuset cardinality * (wall_budget_us + E)` safe reservation; actual `cpu.stat` usage is measured | L(cpu_time) if an independently installed tighter CPU timer fires; otherwise the finite wall stop is L(wall_time) | P2, P4 | T16, T17, T19, T30 |
| Stage wall deadline | Supervisor `timerfd` using `CLOCK_BOOTTIME`, absolute expiry; cgroup-wide kill/outer runtime stop | Each stage including compile and sink/drain; host outside strategy | Stage table; starts before stage tool/strategy input is released; includes waits and teardown | L(wall_time); existing post-execution deadline mapping only at a trusted report boundary | P4 | T2, T3, T17, T18, T22, T30 |
| Job wall deadline | Independent workload stop and receipt/cleanup deadline; microseconds | `job`; host | Default stop at 420 s; cleanup/receipt by 430 s; unaffected by stage restarts/messages | L(wall_time, stage=job); protected terminal writer survives workload kill | P4 | T17, T25, T27, T28, T30 |
| Diagnostic stdout/stderr | Host pipe/socket collectors count raw bytes before retention; bytes | Per stage and job; host | 1 MiB combined diagnostics per workload stage, 1 MiB supervisor reserve; job sum. Both stdout and stderr count, including compiler diagnostics | L(stdout_bytes) or L(stderr_bytes), identifying the stream that crosses the shared cap | P4, P6 | T18, T20, T27 |
| Report JSON | Report-channel collector and engine prepublication encoded-byte check; bytes | `run`, trusted assembly/publication; host and engine | J independently admitted; report/failure/diagnostic channels are disjoint | L(report_bytes); preserve an existing more specific report production failure | P4, P6 | T9, T20, T29 |
| Native sink / finalizer stream | Mandatory `--max-bytes U` plus host forwarding count; bytes | `run`, `finalize`; engine finalizer and host | Positive U on every hosted path, including file mode | Existing finalizer exit 8 + complete=false receipt maps to `report_sink_failed{reason:sink_exhausted,sink_bytes:U+1,cap_bytes:U}` | P4, P6 | T3, T20–T22 |
| Compile budget as a whole | All AS/memory/CPU/wall/fd/task/disk/output rows, over entire driver/child/binder tree | `compile`; host | 2 GiB charged memory, 4 GiB AS, 120 s wall and CPU/process, 32 tasks, remaining D/F, diagnostic cap; all PROPOSED | M, L or V with `stage=compile`; original compiler syntax failure remains shell exit 3 | P1–P6 | T2, T3, T18, T19, T27, T31 |
| Network | Private network namespace, no routed interfaces, seccomp denies new sockets/connect/send-to network destinations; existing authorized local stream endpoints only | All workload stages; host | Zero new network destinations/connections | B if not installed; V(network) for a trusted policy-denial event | P4, P5 | T3, T23 |
| Writable paths / bypasses | Read-only image/inputs; private mount and IPC namespaces; Landlock/path policy; no writable regular fds beyond declared stage roles; dropped capabilities/no_new_privs | All stages; host | Only declared per-run directories and endpoints; none writable by `run` except arena fd mapping. All storage-bearing destinations join D or M | B if a destination escapes accounting; V(writable_path/resource_escape) for prohibited attempts | P1, P3–P5 | T3, T12, T23, T26 |
| Core dumps and unaccounted kernel objects | `RLIMIT_CORE=0` plus host core-collection suppression; deny new memfd, mount/setns/unshare, ptrace, bpf, perf_event_open, keyctl, io_uring_setup and unauthorized IPC/locking operations in the untrusted profile | Untrusted stages; host | Zero core-dump allowance; fixed versioned syscall policy. Compiler exec allowlist is its immutable toolchain; compile workdir is noexec | B if missing; V(resource_escape) on prohibited syscall; fatal strategy exit remains nonzero without creating a dump | P1, P4, P5 | T3, T14, T23 |

Reaching `memory.high` or consuming a CPU quota period is ordinary throttled
operation. Denied capacity allocation, an enforced deadline, or a forbidden
resource operation is terminal under R3. `RLIMIT_FSIZE` does not limit
aggregate storage, inode count, mmap writes or existing backing allocation.
`RLIMIT_NPROC` is uid-wide and has privileged exemptions; neither it nor
`RLIMIT_NOFILE` replaces descendant containment.

### Failure vocabulary and precedence

Existing catalog meanings remain authoritative:

- `out_of_memory` has no args and class `resource`; a caught allocation
  failure does not prove an installed memory limit was reached.
  `docker/run_failure_codes.json`.
- `scratch_capacity_exceeded` takes only its closed `resource` vocabulary;
  `scratch_io_failed` takes no args.
  `docker/run_failure_codes.json`.
- `report_sink_failed` is class `engine_fault`, including `sink_io`,
  `scratch_io`, arena exhaustion and sink exhaustion. `sink_io` is a
  **reason**, not a separate code. Existing size/store arguments apply only
  to their documented reasons. `docker/run_failure_codes.json`.
- `report_post_execution_failed` already has phase
  `result_assembly|results_digest|serialization` and reason
  `deadline|exception|io|resource`. `run_aborted` remains cooperative
  cancellation, not a CPU/wall-budget failure.
  `docker/run_failure_codes.json`.

`memory_limit` occurs in the existing hosting design assumption, but is
**not present in the EM failure catalog**. Registering it is proposed work,
not reuse of an already implemented engine raiser.
`scripts/arena_bound.py`; catalog resource entries above.

The following additions are PROPOSED. All have `since=1.8.0` and
`retryable=false`; a larger attempt remains an explicit placement decision.
Their spelling follows the current code pattern and closed argument rules.
`docker/run_json.py`,
`docs/pages/run-failure-codes.md`.

| Abbreviation / proposed code | Class | Required typed args | Optional typed args |
| --- | --- | --- | --- |
| B / `hosting_budget_invalid` | `engine_fault` | `stage`, `resource`, `reason` | `required`, `installed` nonnegative integers when meaningful |
| L / `hosting_budget_exceeded` | `resource` | `stage`, `resource`, `limit` nonnegative integer | `used`, `requested` nonnegative integers from host measurement or trusted admission facts |
| M / `memory_limit` | `resource` | `stage`, `resource` equal to `address_space` or `memory_bytes`, nonnegative integer `limit` | Nonnegative integers `used`, `requested`; `reason` equal to `rlimit_as` or `cgroup_oom` |
| V / `hosting_policy_violation` | `strategy` | `stage`, `reason` | None |

Shared `stage` vocabulary is
`codegen|compile|run|report_assembly|finalize|supervisor|job`.
Shared `resource` vocabulary is
`proof|address_space|memory_bytes|swap_bytes|arena_bytes|arena_window_bytes|work_bytes|work_files|file_bytes|file_descriptors|tasks|cpu_time|cpu_rate|wall_time|stdout_bytes|stderr_bytes|report_bytes|sink_bytes|network|writable_paths|measurement`.
B reasons are
`absent|unparsable|unsupported|unauthenticated|stale|identity_mismatch|weak|configuration_mismatch|reservation_lost|measurement_lost`.
V reasons are
`process_creation|network|writable_path|resource_escape`.
These vocabularies are PROPOSED in full. Units follow the table; CPU and
wall `limit/used/requested` args use microseconds.

**R3 — trustworthy terminal outcome (T7, T10–T23, T27, T29, T31).**
The supervisor MUST make a proven denial/limit event terminal even if the
strategy catches or ignores the syscall error. It records the first causal
event, stops the whole stage tree, stops dependent work, and prevents
publication as success. Kernel/host evidence, not error-text parsing,
establishes the cause. A signal number, ENOMEM, EAGAIN, EDQUOT, child JSON
or `exit 0` alone is insufficient attribution.

For AS, the host records the kernel's limit-denial decision and requested
mapping/brk growth, not every ENOMEM as `memory_limit`. For cgroup OOM it
uses the fresh cgroup's event delta and task identity. CPU limit signals
require kernel-origin evidence; a strategy sending itself SIGXCPU/SIGKILL
does not mint a host limit receipt. FD, task, file and quota denials are
observed by the trusted syscall/VFS enforcement observer, including cases
with no signal. Required causal observers can use privileged kernel
instrumentation or a syscall broker; unsupported observation is a B
admission refusal, not an assumed attribution.

With no proven host limit event, specific existing native/finalizer errors
keep their code and args. No new usage args are stuffed into old closed
schemas. Trusted finalizer exit 8 plus its matching receipt uses the
existing sink-exhaustion mapping before a generic output-cap mapping.
Cleanup can replace the finalizer's exit-8 outcome with I/O failure today;
only its final exit 8 gets exhaustion fields.
`docker/pineforge_sink_finalize.py`.

A deadline in the isolated trusted report worker maps to the existing
`report_post_execution_failed{phase:result_assembly,reason:deadline}`;
publication/serialization maps to `phase:serialization`. An untrusted
in-process phase does not establish that classification for the host.
The receipt additionally records the physical stage/resource and usage.

**R4 — bounded stopping and publication (T17, T19, T21, T22, T27–T30).**
The host MUST arm independent stage and job deadlines before release,
terminate the entire affected cgroup at expiry without depending on a
cooperative callback, and stop resource execution within E. The reference
action is immediate `cgroup.kill` on the affected workload leaf/subtree;
the job watchdog targets the workload parent and leaves the protected
terminal writer alive. A host needing a stronger outer runtime stop
supplies it before admission. A polite cancellation is allowed only
inside the already reserved time, not as an extension.
No stage/child/benchmark repetition restarts these budgets. Only a trusted
new attempt obtains a new reservation.

Output remains provisional until the report-channel success conditions,
all required stage exits, complete verification and terminal usage are
satisfied. A producer trailer or process exit 0 is insufficient.
The trusted publisher uses its own bounded storage, retains the terminal
receipt on failure, and never appends a second failure JSON after a partial
legacy report. A retained artifact stays charged until deletion or an
explicitly reserved ownership transfer.

## Installation proof and fail-closed admission

### Record and trust boundary

**R5 — authenticated proof (T2–T6, T24, T25).** The host MUST issue one
PROPOSED `pineforge-hosting-proof/v1` envelope for each physical stage
entry, with an inherited-tree authorization covering compiler descendants.
An envelope contains exactly `payload`, `key_id` and `signature`.
`signature` is 128 lowercase hexadecimal digits encoding the 64 signature
bytes; `key_id` is a nonempty ASCII identifier in the immutable trust policy.
The payload is canonical JSON: recursively sorted ASCII keys, no duplicate
or unknown keys, no whitespace, integer decimal numbers without leading
zeros, and no NaN/floats. String escaping is exactly the result of
`json.dumps(..., ensure_ascii=True, sort_keys=True, separators=(",", ":"),
allow_nan=False)` on the validated integer-only object. Parsers preserve
integer precision; converting a signed-64-bit value through a binary64
number is forbidden. Lone Unicode surrogates are invalid.
The signature is Ed25519 over the ASCII domain separator
`pineforge-hosting-proof/v1` followed by LF and the exact payload bytes.
The engine's immutable image trust policy pins allowed public keys and
host policy digests; request data cannot supply a replacement trust root.
Signing private material is never inherited or mounted into a workload.

For engine stage entries the envelope is passed in a read-only sealed proof
memfd, distinct from arena fd 5. The proposed engine option is `--hosting-proof-fd N`;
the launcher allocates an otherwise unused descriptor and checks endpoint
nonaliasing with sink, report, phase and diagnostic channels. The proof
memfd has grow, shrink, write and seal seals. Its maximum encoded size is
PROPOSED 64 KiB, an explicit bounded-reader policy.
Seals establish immutability, not host authorship; signature verification
is still required.

| Required payload group | PROPOSED fields and meaning |
| --- | --- |
| Version/policy | `schema`, `policy_id`, `policy_sha256`, `mode` (`hosted` or `trusted_local_dev`) |
| Invocation | `run_id`, `attempt_id` (fresh host-generated 128-bit identity), `request_sha256`, `facts_sha256` or explicit null, `engine_sha256`, `harness_sha256`, `codegen_sha256`, `toolchain_sha256`, and stage-applicable `source_sha256` / `artifact_sha256` |
| Stage | `stage`, `stage_seq`, `parent_stage_seq` or null, `launch_id`, `parent_launch_id` or null; `descendants` binds allowed executable hashes and inherited-tree policy |
| Process | `host_boot_id`, `host_pid`, `namespace_pid`, `pid_start_ticks`, `pid_namespace_inode`, `uid`, `gid`, and expected capability/no_new_privs/seccomp status |
| Time | `issued_boottime_us`, `admit_by_boottime_us`, `stage_start_boottime_us`, `stage_deadline_boottime_us`, `job_start_boottime_us`, `job_deadline_boottime_us`, `receipt_deadline_boottime_us`, `stop_allowance_us` |
| Cgroups | `cgroups`: job, workload and stage identities/readbacks, ancestor records and initial counters, as defined below |
| Limits | `plan`, `installed`, `derivations`: every applicable contract row, soft/hard rlimit pairs, rounded quotas, memory/swap/CPU/task values, output caps and their certified/policy provenance |
| Storage | `storage`: root identity, project hard limits, reservation/baseline, allowed subdirectories and initial artifacts |
| Backing | `arena`: backing identity/seals, creator/charge/aliases, A/W/manifest; null for stages not using an arena |
| External charges | `external_charges`: explicit finite reservations for input/image/cache/arena pages charged outside the job; empty in the reference unshared path |
| Runtime policy | `isolation`, `fds`, `descendants`: namespace/policy identities and inherited descriptor/toolchain authorization |
| Watchdogs/meters | `watchdogs`, `meters`: protected supervisor, armed deadlines, kill targets, causal and peak observers |
| Opt-out | `opt_out`: null for hosted; otherwise explicit disabled limits, replacement values, local authorization and closed reason |

The following PROPOSED schema fixes the field names within those groups.
`i` is an integer in `[0, INT64_MAX]`, `h` is 64 lowercase hex digits,
`id` is a nonempty host identifier, `str` is valid Unicode text, and `?T`
means T or null, never omitted. Object keys listed here are exhaustive.
Array ordering is preserved; identity/path arrays are sorted by their
canonical identity, and cpus are sorted numerically.

```text
Identity = {mount_namespace_inode:i, mount_id:i, device:i, inode:i,
            generation:id, path:str}
Rlimit = {soft:i, hard:i}
Manifest = {K_q:i, B_q:i, K_e:i, B_e:i, K_c:i, B_c:i,
            U:i, B_U:i, V:i, B_V:i, R:i}
LimitSet = {
  as_bytes:?i, memory_max_bytes:i, memory_high_bytes:i, swap_max_bytes:i,
  memory_oom_group:i, fsize_bytes:?i, nofile:?i, nproc:?i, pids_max:i,
  cpu_soft_seconds:?i, cpu_hard_seconds:?i, cpu_quota_us:i, cpu_period_us:i,
  cpu_burst_us:i, cpus:[i], cpu_upper_us:i, wall_budget_us:i,
  core_bytes:?i, disk_bytes:i, files:i, diagnostic_bytes:i,
  report_bytes:i, sink_bytes:i, arena_bytes:?i, arena_window_bytes:?i,
  slab_bytes:?i, manifest:?Manifest
}
plan = {codegen:LimitSet, compile:LimitSet, run:LimitSet,
        report_assembly:LimitSet, finalize:LimitSet, supervisor:LimitSet,
        workload:LimitSet, job:LimitSet}
installed = {rlimits:{
  RLIMIT_AS:Rlimit, RLIMIT_FSIZE:Rlimit, RLIMIT_NOFILE:Rlimit,
  RLIMIT_NPROC:Rlimit, RLIMIT_CPU:Rlimit, RLIMIT_CORE:Rlimit
}, limits:LimitSet}
Cgroup = {
  identity:Identity, controllers:[str], subtree_control:[str],
  memory_max_bytes:i, memory_high_bytes:i, memory_swap_max_bytes:i,
  memory_oom_group:i, pids_max:i, cpu_quota_us:i, cpu_period_us:i,
  cpu_burst_us:i, cpus:[i],
  baseline:{cpu_usage_us:i, cpu_user_us:i, cpu_system_us:i,
            memory_current_bytes:i, memory_events_max:i,
            memory_events_oom:i, memory_events_oom_kill:i, pids_events_max:i}
}
cgroups = {job:Cgroup, workload:Cgroup, stage:Cgroup, ancestors:[Cgroup]}
storage = {
  root:Identity, project_id:i, block_unit_bytes:i, block_hard_units:i,
  inode_hard:i, block_enforced:bool, inode_enforced:bool,
  project_inherit:bool, baseline_bytes:i, baseline_inodes:i,
  reservation_id:id, reserved_bytes:i, reserved_inodes:i,
  stage_directories:[{stage:Stage, root:Identity, uid:i, gid:i}],
  artifacts:[{identity:Identity, bytes:i, sha256:?h, owner_stage:Stage}]
}
arena = ?{
  identity:Identity, supplied_bytes:i, window_bytes:i, seals:[str],
  manifest:Manifest, slab_bytes:i, creator_launch_id:id,
  creator_cgroup:Identity, charge_cgroup:Identity,
  external_reserved_bytes:i, external_reservation_id:?id,
  aliases:[{owner_launch_id:id, fd:i}]
}
external_charges = [{
  purpose:"input"|"image"|"arena", owner_cgroup:Identity,
  memory_max_bytes:i, reserved_bytes:i, reservation_id:id,
  objects:[{identity:Identity, immutable_bytes:i}],
  charge_proof_sha256:h
}]
isolation = {
  namespaces:{mount:i, network:i, ipc:i, pid:i, cgroup:i},
  no_new_privs:bool, capability_bits:i, seccomp_mode:i,
  seccomp_sha256:h, landlock_sha256:h, mounts_sha256:h, bootstrap_sha256:h,
  core_pattern:str, core_pipe_disabled:bool,
  writable_paths:[{root:Identity, owner_stage:Stage, reservation_id:id}]
}
fds = [{fd:i, role:str, access:str, device:i, inode:i,
        endpoint_id:id, stream_id:?id}]
descendants = {mode:"none"|"inherited_tree", policy_sha256:h,
               executable_sha256:[h], deny_privilege_transition:bool}
watchdogs = {
  supervisor_launch_id:id, stage_timer_id:id, job_timer_id:id,
  receipt_timer_id:id, armed:bool, kill_target:Identity,
  outer_stop_id:id, stop_allowance_us:i
}
meters = [{name:str, implementation_sha256:h, config_sha256:h,
           coverage:"kernel_high_water"|"kernel_lifecycle"|"trusted_collector",
           active:bool}]
opt_out = ?{authorization_id:id, reason:"local_debug"|"local_compatibility",
            disabled:[{stage:Stage, resource:Resource,
                       replacement:i|"unlimited"}], confined:false}
```

`Stage`/`Resource` are the code vocabularies above, plus `workload` only
for internal plan/tree keys. All null LimitSet values mean inapplicable:
the workload/job cgroups have no process rlimits; non-arena stages have
null arena/manifest fields. An applicable hosted value cannot be null.
An unused output channel has capacity zero. Parent limits do not use
zero as an unlimited sentinel.

The payload consists exactly of the individually named policy/invocation/
stage/process/time fields in the table, plus these named groups.
The process fields additionally include `no_new_privs:bool`,
`capability_bits:i` and `seccomp_mode:i`. Hashes use type h; identifiers
and `run_id` use id; PID/sequence/credential/clock fields use i.
`attempt_id` and `launch_id` are 32 lowercase hex digits.
`parent_launch_id` and `parent_stage_seq` are explicitly nullable.
Only `facts_sha256`, pre-build `artifact_sha256`, and inapplicable
codegen/toolchain/source identities can be null. The immutable image
policy fixes engine/harness identities even for an early stage.

`derivations` is an array of
`{stage:Stage, resource:Resource, term:str, value:i,
kind:"certified"|"policy", source_sha256:?h, formula_id:str}`.
The closed `term` vocabulary is
`arena|arena_window|input_bars|slab|input_resident|input_as|
private_resident|private_as|report_buffer|sink_stream|report_json|
work_bytes|work_files|workload_memory|job_memory|workload_tasks|
job_tasks|cpu_upper|wall`. There is at most one record per stage/term.
For `run`, A/W/N/S(N)/O_r/O_a/U/J and private/report allowances are
mandatory terms even when their provenance is policy rather than certified.
Certified terms have a nonnull certificate digest.

The reference `formula_id` vocabulary is
`policy_v1|certified_v1|p0_slab_v1|run_memory_v1|run_as_v1|
work_disk_v1|sum_v1|cpuset_wall_v1`. The first two mean a signed fixed
policy value or validated certificate value; the remaining formulas are
exactly the arithmetic in the reference-policy section. They are not
executable expressions. A policy adding formulas has a new policy digest
and an engine validator that recognizes those formulas before admission.
The signed `request_sha256` hashes a canonical immutable request manifest
containing source and every input/feed/settings/data-plan artifact's
logical role, byte length and digest. `facts_sha256` binds the corresponding
certificate bytes. A file pathname without these bytes is insufficient.

Raw kernel readbacks in `installed` and `cgroups` can use the literal
string `"max"` for RLIM_INFINITY or an unlimited kernel ceiling.
This is a readback-type extension to i, not an admissible finite plan.
An applicable required local or effective ceiling of `"max"` is B(weak);
unlimited outer ancestors are harmless only when the required local and
effective installed controls remain finite. `plan` never accepts `"max"`.
`installed.limits` reports effective constraints; job/workload/stage
Cgroup records separately report the actual local files. Both are checked,
so an unlimited leaf cannot hide behind a finite ancestor.

Each Identity names its mount namespace, avoiding equality tests between
unrelated host and container mount ids. Host-private roots/hidden ancestor
identities are signed host observations; the verifier corroborates the
visible stage-directory/cgroup identities in its own mount namespace.
The host ties both views to the same underlying project/backing objects.

The `fds.role` vocabulary is `input|proof|arena|diagnostic_stdout|
diagnostic_stderr|report|sink|phase|control`; `access` is
`read|write|read_write`. `endpoint_id` denotes a host-created endpoint,
not just its numeric fd; `stream_id` follows the same logical stream
through forwarding copies. Report/sink limits are checked per logical
producer stream, not multiplied by counting each forwarding hop.
Diagnostics from all compiler helpers share their compile-stage allowance.

The required `meters.name` vocabulary is
`cpu|rss|address_space|memory|quota|inodes|file_io|fds|tasks|channels|causes`.
A policy digest fixes exact platform hook/broker implementations.
Observer completeness is checked during admission and by T31.

An inherited-tree authorization is verified once before the compiler
driver reads input. Kernel inheritance/containment is already active for
every fork/clone; the trusted host records each helper's PID/start/cgroup/
executable and observed inherited limits, bound to `parent_launch_id`.
No helper receives a fresh CPU, memory, temp, output or wall allowance.
A helper exec outside the immutable allowlist is denied by the host
exec/path policy; normal GCC helpers do not need to parse a proof file.
The separately started inventory binder has its own entry proof, still
under the same compile-stage cumulative limits and deadline. It keeps the
compile `stage_seq` and original stage start/deadline, but gets a new
`launch_id`; a second entry is not a new stage reservation.

Request/facts digests bind inputs; a digest alone does not certify their
truth. Before compilation, no SO digest exists: it is explicitly null.
After compilation, the host snapshots/hashes the artifact and issues a
new `run` proof containing that digest. No stage reuses a proof with a
future hash placeholder.
The compiler work mount remains noexec. The finalized SO is exposed through
a distinct immutable read-only artifact mount that permits the executable
mappings needed by `dlopen`; it remains charged to D. The `run` syscall
policy denies execve/execveat, and nobody in the workload can mutate the
artifact between hashing and loading.

### What is read, by whom

**R6 — installed-state readback (T1–T4, T6–T8, T12–T16, T23, T26).**
The host MUST populate installed values from the kernel/trusted manager
after installation, while the trusted launch stub is paused before
untrusted input or library loading. Configuration files, environment
strings and a successful limit-setting call are not sufficient evidence.
The trusted bootstrap uses pinned executable/library paths and an
allowlisted environment before starting an interpreter or dynamic loader.
Caller-controlled `LD_PRELOAD`, library search paths, Python startup/import
hooks and compiler executable/spec/plugin search settings cannot execute
code before admission or select an unapproved tool. A stage stub itself
and its imports come from the immutable image, never the writable workdir.
The bootstrap policy digest includes this environment/import/loader policy.
For core dumps, the reference host disables piped collection and reads
`/proc/sys/kernel/core_pattern` on its trusted side. A pipe collector can
otherwise allocate outside the process's file-size/quota envelope despite
RLIMIT_CORE=0. Such a host needs separately proven suppression/accounting
or refuses admission; a zero rlimit alone is insufficient.

| Proof identifier | Trusted-side read | Engine/stub corroboration before admission |
| --- | --- | --- |
| P1 | `prlimit64`/`/proc/<host-pid>/limits`; `/proc/<pid>/stat` start time; `/proc/<pid>/status` credentials, NSpid, capabilities, NoNewPrivs and Seccomp; inherited-fd inventory | Read own rlimits, `/proc/self/stat`, `/proc/self/status`, expected fd access modes/identities and arena `fstat`/seals. Signature binds the host/namespace PID mapping. |
| P2 | Resolve cgroup membership from the target process; retain job/workload/stage cgroup directory handles. Read `cgroup.controllers`, `cgroup.subtree_control`, `memory.max`, `memory.high`, `memory.swap.max`, `memory.oom.group`, `pids.max`, `cpu.max`, `cpu.max.burst`, `cpuset.cpus.effective`, and applicable ancestors | Read `/proc/self/cgroup`, cgroup2 mount identity and all visible matching files through the actual read-only cgroup mount. Hidden ancestors are supplied by signed host readback, not guessed from a namespace-local path. |
| P3 | `quotactl(Q_XGETQUOTA)` and filesystem enforcement status for the exact project; inspect project inheritance and per-run mount/root identities; read reservation-manager allocation | `stat`/mount identity of the visible stage directory matches its signed `stage_directories` entry. The private aggregate root, privileged quota and free-space reservations are host attestations; a workload file cannot assert them. |
| P4 | Protected supervisor timerfd configuration, absolute expiry, armed state, target directory/pidfds; installed kernel observers and syscall policies; current channel collectors/caps | Verify signed identities, deadlines still in the future, own confinement status, inherited endpoints and nonaliasing. Timer ownership, causal tracing and collector caps remain trusted-host assertions. |
| P5 | Mount/namespace handles, read-only input/image mounts, writable-fd/path inventory; arena creator/first-charge event in the job tree; retained alias and page-charge ownership | Verify own mount identities, fd types/access, exact arena backing and manifest. Size/seals do not independently prove memcg charge origin. |
| P6 | Host-owned channel endpoint identities and cap counters; finalizer argv/cap; report-channel policy and reservation | Verify the admitted J/U and distinct descriptor roles; finalizer verifies its own mandatory cap before reading. No in-process message creates host terminal authority. |

For cgroups, the host computes effective ancestor restrictions and rejects
a plan that cannot be installed exactly. A finite leaf value under an
unexpected stricter ancestor can also make the requested reservation
unavailable. Installation readback records both; it does not pretend that
the leaf controls the whole machine.

**R7 — charge and storage closure (T8–T12, T21, T23, T26, T28).**
The host MUST create/first-charge arena backing inside the invocation's
charged tree, or prove an equivalent separately reserved charge whose full
lifetime joins the job total. Moving a process or passing a memfd does not
move previously charged pages. Tmpfs/shmem, anonymous memory, file cache,
dirty spool pages and kernel-accounted memory are included; `memory.max`
does not by itself charge pages created outside that tree.
This also applies to a read-only input or image whose cached pages were
charged before this invocation. The reference unshared input path creates
private input artifacts under the admitted preparation/storage budgets.
Any external input/image/arena charge instead has an `external_charges`
entry: its owner cgroup has a read-back finite memory.max, its reservation
covers that cap, and the host retains the reservation/owner through all
mapping and cache lifetimes. The signed charge certificate binds the
immutable objects and owner; unknown charge origin is B, not zero charge.

For this version there is no subtraction for shared owners:
`job physical reservation = plan.job.memory_max_bytes +
sum(external_charges.reserved_bytes)`. Repeated references to one owner use
one entry; a distinct owner requires a distinct reservation. A true sharing
credit between jobs needs a separately versioned host admission proof.
The internal job cgroup maximum remains the sum of its leaf budgets.
The receipt separately reports external-owner reservations/peaks, labelled
with their owner scope; their peaks are not the job's simultaneous usage.

The default work root is a host-owned private mode-0700 directory, with
stage-owned subdirectories and distinct credentials for the trusted
publisher. The host exposes a stage's own subdirectory by private bind
mount; workloads do not traverse the host-private parent or other stages.
All writable disk destinations share the per-run enforced project quota.
`TMPDIR`, compiler temp settings, explicit spill paths, output paths and
runtime fallback directories cannot escape it. Writable tmpfs is either
absent or has a separately finite byte/inode cap joined to M/D/F.
New strategy-created memfds/IPC files are prohibited. Only the pre-sized
sealed arena has the `run` writable-fd exception.

The host reserves underlying capacity for admitted D/F across simultaneous
jobs; a hard quota alone does not reserve free space. It holds ownership
through last-close of unlinked files, dirty-page writeback and retained
artifacts. A compiler/spool may consume the remaining shared D and then
fail; no other stage is entitled to an additional unrecorded allowance.
An unproven reservation or a writable escape is B before launch.
The quota's D is allocated block charge, not the sum of sparse logical
file lengths. Per-file FSIZE plus F also gives a finite logical-extent
ceiling; neither is misreported as physical bytes used.

### Admission algorithm

**R8 — fail closed before work (T2–T6, T18, T24, T30).**
Every engine/host entry path MUST perform the following checks before its
first untrusted-input operation. `run_json` checks before *any*
`load_strategy`/`ctypes.CDLL`, not merely before the first callback.
Compiler/codegen/finalizer checks run in a trusted stub before their
respective stage-entry tool is allowed to read input. Compiler descendants
are admitted by the already installed inherited-tree authorization above.

1. Obtain the immutable trusted expected run/attempt/stage/launch identity
   from the hosting invocation. Read the bounded sealed envelope; reject
   absence, truncation, duplicates, wrong types, unknown schema or fields,
   malformed integers/signature and excessive size.
2. Verify the signature and pinned policy/image identities. Match request,
   source, settings/data-plan/facts and compiled-artifact digests as applicable.
   Match process start identity, boot identity, cgroup generation and
   work-root identity. Paths, PID numbers or `run_id` alone are insufficient.
3. Check `stage_start <= issued <= now <= admit_by` on the same `CLOCK_BOOTTIME`.
   The reference admission window is PROPOSED 5 seconds. Also check that
   both stage and job deadlines are armed and still future. Admission
   expiry is not a five-second stage runtime; the separate deadline applies.
   Verify absolute expiries from the signed starts and plan: stage expiry
   is `min(stage_start + stage wall budget, job workload-stop deadline)`;
   job workload-stop and receipt deadlines follow the sum/cleanup rules.
   Binder/helper launches cannot supply a later original stage start.
4. Perform P1–P6 readback/corroboration. Resolve every table row and compare
   actual effective normalized values with the signed admitted plan.
   Missing, unlimited, disabled or larger ceilings, weaker policy, wrong
   quota units, absent controller, or a watchdog/meter without coverage
   fails closed. A different stricter installation also requires a newly
   admitted plan; it is not silently substituted.
5. Verify checked derivations: A/W/manifest, N and OHLCV allowances, U/J,
   report-column demand, remaining D/F, stage concurrency, job totals,
   inherited descriptor range and compiler descendant containment.
   Exact certified minimum demand above a finite plan is a typed L
   admission refusal with requested/limit, before strategy execution.
6. Close the proof reader's writable authority (there is none in the
   workload), retain only necessary immutable evidence, and release the
   tool through the host gate. Start/retain the already armed timers.
   Record admission success and the exact proof payload hash in the host
   receipt. The trusted publisher requires that host record before success.

Absent proof is B(absent); parsing/schema failure is B(unparsable) or
B(unsupported); invalid authorship is B(unauthenticated); expiry is B(stale);
wrong run/process/stage/generation/hash is B(identity_mismatch); missing or
weaker enforcement is B(weak); other installation differences are
B(configuration_mismatch). `resource` identifies the failed row, or
`proof` when it cannot be safely identified.

The engine emits direct-harness exit 1 with the typed failure when its
report channel is available. The external supervisor can always report a
typed terminal admission refusal even when the child never starts.
Existing shell/tool statuses are retained in `raw_exit`: the shell maps a
failed backtest to 4 and compile to 3 today, rather than preserving the
Python status. `docker/entrypoint.sh`. The proposed hosting adapter returns
nonzero and carries the authoritative typed outcome separately; it does
not infer codes by reversing these coarse statuses.

**R9 — nonreplay and no relaxation (T3–T5, T14, T24, T30).**
The host MUST keep entry launch identities single-use and settings immutable for
their admitted lifetime. Limit decreases terminate/re-admit rather than
silently changing the plan; increases require a new authorized attempt.
A killed supervisor closes the execution gate and triggers the protected
outer runtime stop. Workloads cannot modify cgroup files, quota metadata,
namespace membership, host policies, meter maps, proof records or watchdogs.
The host records configuration mutations as B and prevents success.

The signature proves what the trusted host attested at admission. Visible
readback corroborates part of that state. Neither proves host honesty,
kernel correctness, future host availability, exact strategy cost, trusted
compiler facts, absence of application bugs, or a particular refund.
The proof also does not make later strategy-generated usage/phase/error
messages authentic. Ongoing containment and terminal host measurements
are separate obligations.

### Explicit local/development opt-out

**R10 — recorded opt-out only (T25, T29).** A trusted local launcher MAY
offer a PROPOSED `--allow-unconfined-local` option. If used, it MUST create
a `trusted_local_dev` receipt before loading code, list every disabled
limit and every literal `unlimited` choice, record local authorization
and `confined=false`, and display that mode to its caller. No missing
proof, `--profile unjailed`, environment toggle, or strategy request
implicitly selects it.

A hosted/paid dispatcher rejects this mode and ignores request-supplied
opt-out switches. The immutable local launch policy, not a field writable
by a strategy, enables the exception. Hosted receipts always say
`mode=hosted`, have no opt-out and contain finite values for every
applicable row. This exception preserves trusted local experimentation
without weakening the hosted default.
This local-only branch explicitly waives R5's host signature and R8's
hosted enforcement checks for the listed disabled limits. Its receipt is
labelled unauthenticated local evidence; it cannot be accepted by a hosted
publisher or presented as a hosting installation proof.

## Measured usage and receipt handoff

### Host-owned resource receipt

**R11 — usage on every outcome (T1, T7–T22, T27, T30, T31).**
The supervisor MUST emit a PROPOSED `pineforge-hosting-usage/v1` receipt
for success, refusal, cancellation, exception, signal, OOM and deadline.
It survives loss of the strategy/report process. The planned measured-usage
receipt uses a new external `hosting_budget` block; field names below are proposed
handoff names, not fields currently emitted by EM.

Its identity fields repeat the proof binding and `proof_sha256`.
It contains `admission`, `limits`, `stages`, `job_usage`, `terminal`,
`publication` and `cleanup`. `terminal` includes typed `code/args`, trusted
`cause_event_id` or null, stage, raw exits/signals and report-disposition.
Every planned stage has an entry: `not_started` has null measurements and
that explicit status, not fabricated zero consumption.

| Proposed per-stage measurement | Unit / exact meaning | Producer and observation |
| --- | --- | --- |
| `start_boottime_us`, `end_boottime_us`, `wall_us` | Host-observed interval, includes stalls, descendants and teardown | Host clock around gate release through all-task exit; separately records deadline/stop overshoot |
| `cpu_user_us`, `cpu_system_us`, `cpu_total_us` | Cumulative CPU of all tasks in the fresh stage cgroup | Host `cpu.stat` user/system/usage; preserve kernel totals and reconciliation difference, never add waited-child totals a second time |
| `rss_peak_bytes` | Maximum kernel-reported per-process RSS high water among stage processes, **not** sum of simultaneous RSS | Host wait/exit accounting, with kernel exit observation for descendants reaped by the compiler; Linux `ru_maxrss` KiB normalized to bytes |
| `memory_peak_bytes`, `memory_end_bytes` | Peak/end charged cgroup memory, including cache/shmem; distinct from process RSS | Host `memory.peak` / `memory.current`, plus `memory.stat` anon/file/shmem breakdown and event deltas |
| `external_memory` | Array of reservation id, owner cgroup, reserved bytes and owner-scoped charged peak | Host readback/meter for every external charge entry; separate scope, never added to stage peaks as measured simultaneous usage |
| `address_space_peak_bytes` | Peak process VM extent, with measurement method | Host kernel VM meter; never inferred from RSS |
| `storage_peak_bytes`, `storage_end_bytes` | Per-run project quota charge in bytes; shared usage identified by reservation id | Host quota/VFS meter; stage intervals are observations of the shared reservation, not independent charges to sum |
| `files_peak`, `files_end`, `files_created_total` | Charged inode high water/current and successful creates, including directories/O_TMPFILE/unlinked-open lifetime | Host project quota plus kernel inode lifecycle meter |
| `file_write_bytes`, `storage_io_write_bytes` | Successful regular-file write/pwrite/writev returned bytes; separately actual block-I/O bytes | Host syscall/VFS meter; `io.stat` for block bytes. Rewrites count again; holes, tmpfs and mmap dirtying are not disguised as syscall bytes |
| `fds_peak_process`, `fd_entries_peak_tree` | Highest descriptor count in one process; highest sum of descriptor entries across live processes | Host kernel fd lifecycle meter, seeded with inherited fds; dup/fork/close/exit included; thread-shared tables counted once per process |
| `tasks_peak`, `tasks_created_total` | Threads plus processes, high water and cumulative creation | Host `pids.peak` where supported plus trusted task lifecycle meter and `pids.events` |
| `diagnostic_stdout_bytes`, `diagnostic_stderr_bytes` | Raw bytes received before any retention/truncation | Host collectors; separate `retained_bytes` / `discarded_bytes` |
| `report_bytes`, `sink_bytes` | Encoded bytes actually received, provisional and published counts separate | Host report-channel collectors; trusted finalizer input receipt corroborates sink bytes |
| `limit_events`, `stop_overshoot_us` | First/all causal enforcement events and actual deadline-stop latency | Host kernel/watchdog ledger; contains mechanism, resource, installed limit and known used/requested values |
| `arena_capacity_bytes`, `arena_high_water_bytes`, `arena_claimed_bytes` | Engine scratch populations, **not** physical RSS or quota usage | Engine stats/snapshot, tagged `source=engine`, attachment identity and lifecycle scope |
| `engine_phase_usage` | Optional internal phase durations, arena/suballocator counters and byte checks | Engine, tagged diagnostic/untrusted after native loading; cannot replace authoritative host-stage totals |

Each measurement carries `source`, `method` and `quality`; host peaks use
kernel high-water counters or kernel-maintained lifecycle maps. A polling
sample is labelled `sampled_lower_bound`, never silently called a peak.
Mmap stores have no general exact user-byte counter: their dirty/charged
pages and extents are included in memory/storage peaks, with this coverage
recorded; `file_write_bytes` explicitly excludes them.

The compiler entry has all these fields, including its descendants,
peak RSS, CPU, wall, temp/inode and diagnostic usage. A syntax-only helper
or run-only timing result is not compiler usage evidence.

**R12 — measurement integrity (T8, T11, T13, T15, T19, T27, T31).**
The host MUST establish required meter support before admission, retain
kernel high waters across rapid allocate/free and child exit, and snapshot
usage before removing cgroups or releasing quota ownership. Kernel maps
maintain counts even if userspace event delivery is delayed. Lost required
coverage is B(measurement_lost), prevents success, and marks affected
measurements unknown; it never fabricates zero or a certified peak.
A host lacking a required high-water primitive installs the specified
equivalent lifecycle meter or refuses as unsupported.

Shared project peaks and job-cgroup peaks are measured independently.
Do not sum stage RSS peaks, shared disk peaks, arena peaks or overlapping
phase high waters into a purported simultaneous peak. Remaining page-cache
charges after process exit stay in `job_usage` until release. An
unreachable stage's null usage is different from a started stage whose
meter failed.

### Existing receipt/failure compatibility

Current failure serialization is `engine,error,code,args`, with arena and
abort mirrors and bounded text/argument/line sizes.
`docker/run_json.py`.
Current profile/arena diagnostics are success-only diagnostics, not
host proof. `docs/pages/report-schema.md`.

**R13 — additive carrier, frozen payloads (T1, T20, T27, T29).**
The hosting integration MUST carry complete limits/usage in the external
receipt, including when the legacy failure line is unavailable or trimmed.
It must not alter frozen report/sink/fingerprint bytes, append usage to
the native sink trailer, change existing error text/class/retryability/args,
or make an old optional arg required. New hosting codes require the
catalog/registry/docs/release-diff additions first. Existing catalog
versioning rules apply: `docs/pages/run-failure-codes.md`.

The engine's admission error can use the existing failure-line encoder
with the newly registered code. The host stores that failure object
alongside authoritative usage; the host receipt is not additional stdout
JSON. For an OS-killed producer, the supervisor constructs the terminal
hosting outcome on its protected channel. A child-provided error retains
its origin label and is not promoted into a host-confirmed limit/refund.

## Host conformance and real-host acceptance

### Outside-in test rules

**R14 — real enforcement evidence (T1–T31).** The verification team MUST run these tests on
the actual Linux image, filesystem, kernel, compiler and launcher intended
for deployment. Each test records image/toolchain/kernel/policy digests,
run/attempt/stage identities, installed-state readback, raw process exits,
typed terminal outcome, usage and final artifact disposition. A mock,
source citation, configured-value dump or skipped test is not conformance.
The below tests are future acceptance procedures, not results of this
document's authoring session.

Smaller signed test tiers make exhaustion practical. They retain the same
mechanisms and install/readback checks. Test input programs independently
emit a start sentinel to a test observer; that sentinel has no receipt
authority. Admission failures leave the sentinel absent. Successful
small fixtures keep frozen output bytes.

1. **T1 — normal full pipeline and exact sublimits.** Run small Pine and
   supplied-C++ fixtures through real codegen/compile/run/report/finalize;
   cover jailed arrays and fd sinks, ordinary and selected modes.
   Check finite values, admission before load, per-stage proof/usage,
   job totals, manifest/slab/arena counters, original failure-code shapes
   and byte-identical accepted reports. Exercise an explicit command cap
   and its existing failure, plus a ledger history larger than the
   published descriptor window.

2. **T2 — absent proof at every entry.** Remove the proof separately for
   codegen, compiler, binder/helper, run, report worker and finalizer.
   Each refuses nonzero with B(absent), no stage-input/start sentinel,
   no later stage and no success artifact. Direct run_json invocation
   without the hosted proof also refuses before a constructor.

3. **T3 — missing or weaker installation matrix.** For each table row,
   sign the required plan but actually omit its installer or install a
   weaker value: unlimited AS/FSIZE/CPU/fds/tasks, larger memory/quota,
   memory.high alone, swap enabled, missing controller/ancestor, CPU
   cpuset/rate weaker, missing disk/inode quota, disabled watchdog,
   unbounded output collector, missing confinement or meter.
   Expect B(weak/unsupported), nonzero and no sentinel. Repeat with an
   unexpected stricter value: B(configuration_mismatch), not silent substitution.

4. **T4 — malformed/untrusted proof.** Feed a truncated/oversize record,
   duplicate/unknown key, wrong schema, negative/overflow/float/infinite
   value, invalid signature/key, unsealed or mutable fd and mismatched
   executable/policy digest. Expect the specified B reason before work.
   A sealed workload-written document is still unauthenticated.

5. **T5 — forgery and mutation.** Strategy code rewrites/replaces the
   visible proof fd, emits a forged host receipt/phase/error, supplies
   its own key and tries to access the signer/supervisor. None changes
   host admission or terminal authority. Attempts to relax a live limit
   through the management plane are refused or terminate as B; a
   strategy-side phase cannot grant another budget.

6. **T6 — constructors and arena refusal precede callbacks.** Use a
   native constructor with an observable start action. Bad host proof
   prevents even that constructor. With valid host proof, missing,
   wrongly sized or incorrectly sealed arena backing retains the
   existing `report_sink_failed(reason=arena_backing)` and runs no strategy
   callback; no tempfile fallback appears. Malicious loader/Python startup
   hooks and compiler search-path overrides cannot run before proof
   validation or select code outside the pinned image/toolchain.

7. **T7 — heap/AS/cgroup memory.** Allocate and touch until failure in
   constructors and callbacks. In separate tiers make AS, then cgroup
   memory, decisive. Also allocate/free rapidly and create file-cache
   pressure. Check denial/OOM evidence, M with correct resource/cap,
   no success even if the program ignores allocation failure, bounded
   memory peak and usage on kill. Self-generated signals/ENOMEM text
   must not produce a host-confirmed M. Native `bad_alloc` without host
   limit evidence retains existing classification.

8. **T8 — arena, shmem and charge origin.** Grow through many arena
   windows while W stays small; verify charged shmem can exceed W but
   stays under M. Repeat with a backing pre-touched outside the job:
   admission refuses unless its separately reserved charge/lifetime is
   proven and joined. Repeat with pre-cached inputs/image files owned
   outside the job; verify the external cap/reservation or refusal.
   Check creator/alias identity, seals, shrink refusal, whole-chunk
   touches and memory/cache metrics.

9. **T9 — report/private/slab overlap.** Use a large slab, native
   retained state and a large escaped report with a small arena window.
   Confirm the run cap includes preparation through process exit and
   catches the simultaneous peak. A report exceeding J is L(report_bytes);
   an actual host memory denial is M. A slab or arena breach retains
   its existing specific code. Verify no sum of independent high waters
   is presented as the measured simultaneous peak.

10. **T10 — aggregate byte quota.** In a quota-enabled test stage,
    write many files individually below RLIMIT_FSIZE until D is consumed;
    include sparse extents, rewrites, fallocate and writable mappings.
    Expect real quota denial and L(work_bytes), bounded quota charge,
    no files outside the project and no successful terminal result.
    Exercise native scratch/sink I/O separately to preserve existing
    specific error mappings rather than infer cause from text.

11. **T11 — inode/file exhaustion.** Create empty files, directories and
    O_TMPFILE objects until F; hold unlinked files open and rapidly
    create/close objects between polling intervals. Expect inode denial
    and L(work_files). Peak/current/create counters and last-close
    release match an independent observer; directory scans alone fail
    this test.

12. **T12 — per-file limit and stage writable roles.** With shared D
    still free, cross a smaller signed RLIMIT_FSIZE and expect L(file_bytes).
    In `run`, regular-file writes/truncation/mmap escape attempts cannot
    grow storage; pre-sized arena mapped writes still work. Compile
    and file-mode finalizer use their admitted nonzero file allowance;
    stream-only finalizer works with zero.

13. **T13 — descriptor exhaustion.** Open/dup descriptors to NOFILE;
    include `dup2` to an out-of-range fd and many inherited fds.
    Expect kernel denial and L(file_descriptors), even if ignored.
    A report-column demand of base+3+P+artifact/readers above the cap
    refuses before creation with requested/limit. Check rapid fd peaks
    and channel nonaliasing, not just endpoint numbers.

14. **T14 — no-fork run and escape resistance.** Attempt fork, vfork,
    clone and clone3, including a thread and double-fork. Expect
    V(process_creation) or a kernel task-denial L, no surviving child
    and no success. Attempts to change cgroup/namespace/limit authority
    cannot escape the job or affect another run.

15. **T15 — compiler task tree.** A host-controlled test compiler helper
    and a real-toolchain stress fixture exercise the 32-task tier,
    including threads. Exceed pids.max and NPROC in separate cases;
    expect L(tasks), correct kernel event and tree cleanup. Dedicated
    unprivileged credentials prevent root exemption and another job's
    uid population from silently consuming the reservation.

16. **T16 — CPU seconds versus rate.** Spin under a short RLIMIT_CPU
    with a longer wall deadline and verify kernel CPU-limit evidence and
    L(cpu_time). Separately run concurrent/churning compiler children
    under quota/cpuset and the outside deadline. CPU never escapes the
    derived cpuset×wall bound. A half-core quota throttles rather than
    being misreported as a cumulative timeout. Self-signals are not
    kernel limit events.

17. **T17 — wall limits and whole-job limit.** Test an infinite callback,
    a sleeping producer, a hung codegen/compiler and a stage blocked on
    output. The outside timer stops each entire tree within budget+E,
    returns L(wall_time), preserves usage and publishes no success.
    Then make individually valid stages consume the job's workload-stop
    budget; stage transitions never reset its absolute deadline. The
    protected supervisor survives that stop and completes the receipt
    inside the remaining cleanup interval.

18. **T18 — exploding real compile.** Compile adversarial native input
    that separately exhausts optimizer memory/CPU, emits huge diagnostics,
    or fills compiler temps/inodes. Capture the real g++/helper tree.
    Each decisive mechanism produces its M/L/V with stage=compile,
    nonzero raw status, bounded peak RSS/time/temp/output, no run start
    and compiler usage on failure. Ordinary syntax errors keep exit 3
    without a fabricated resource cause.

19. **T19 — descendants outlive the driver.** A test compile driver exits
    leaving busy/orphaned/double-forked helpers; one helper ignores polite
    signals. The stage remains active, and the hard stop kills its whole
    cgroup within E. No descendant remains outside the job, no usage is
    lost on parent exit, and no later stage starts on driver status alone.

20. **T20 — every output byte channel.** Flood compiler stdout/stderr,
    native fd 1/fd 2, report JSON, and sink separately. Cross the signed
    diagnostic sum, J and U by one byte. Test heavy escaping so sink
    bytes do not stand in for expanded JSON bytes. Verify stream-specific
    L, existing sink-exhaustion mapping where applicable, counted dropped
    bytes, bounded retention and no valid published artifact after overflow.

21. **T21 — finalizer spool and aggregate disk.** Fill the same per-run
    quota using compiler/artifact residue, then spool a valid sink.
    Observe real quota failure, finalizer I/O exit 5, L(work_bytes) from
    trusted quota evidence, provisional deletion and complete usage.
    Separately cross U with disk still free: finalizer final exit 8,
    complete=false, sink_bytes=U+1/cap_bytes=U and the existing
    `report_sink_failed(reason=sink_exhausted)`. A final cleanup I/O
    failure preserves its actual final status instead of inventing exit 8.

22. **T22 — no EOF / no consumer / publication stall.** Keep a sink
    producer connected without EOF, stop reading its pipe, and stall
    final output flush/sync in separate cases. Outside deadlines end
    waits and all writers/readers within E; use existing post-execution
    deadline mapping only for the trusted external report phase.
    There is no success receipt or retained provisional success file.

23. **T23 — network, writable paths and kernel-resource bypasses.**
    Attempt IPv4/IPv6/local-new-socket access, absolute/symlink/`..` paths,
    hostile TMPDIR/fallback directories, inherited writable-file aliases,
    mmap writes outside the arena, memfd/IPC creation, core dumps,
    io_uring, namespace changes and unauthorized execs. Verify denied
    syscalls with the specified V or typed admission B, no outside storage
    or network traffic, and unchanged normal inherited stream operation.
    Also crash a native fixture: the exit is nonzero, and neither a core
    file nor an out-of-envelope pipe collector is created.

24. **T24 — stale/replayed identity.** Replay a valid signed proof after
    admission expiry, across run/attempt/stage/launch, after PID reuse,
    cgroup/work-root recreation, changed source/SO/settings/data, and a
    changed boot identity. Expect B(stale/identity_mismatch), no sentinel.
    A pathname or matching numeric PID does not rescue the replay.

25. **T25 — opt-out and downgrade.** Submit all local opt-out switches,
    unjailed profile and environment overrides through the paid
    dispatcher; none bypasses proof/admission. Explicit trusted local
    opt-out records disabled/unlimited values and confined=false before
    load. Hosted finite defaults remain effective, including all stage
    and job timers across retries.

26. **T26 — concurrent reservations.** Run competing jobs through
    compiler/run/report overlap. Check aggregate memory/page-cache,
    disk/inode, task and CPU reservations, distinct project/cgroup/uid
    identities and no shared-feed credit without proof. Overbooking or
    unavailable underlying storage is B(reservation_lost) before work;
    one job cannot spend another's allowance.

27. **T27 — failures cannot erase usage.** Inject allocation failure,
    SIGKILL/OOM, early exception, compiler failure, broken report/receipt
    pipe and producer exit after partial publication. The protected host
    channel retains exactly one terminal outcome and all started-stage
    usage, with raw statuses. Missing legacy JSON does not mean success;
    failed usage collection is explicit B(measurement_lost).

28. **T28 — crash residue and reservation lifetime.** Kill producers and
    restart the supervisor/reaper with linked spools, open-unlinked files,
    dirty pages and retained arena aliases. Ownership survives restart;
    no quota is reused before last-close/delete/charge transfer. Cleanup
    is attempted within its 10-second reservation; failure quarantines
    capacity and records B(reservation_lost), never unlimited free reuse.

29. **T29 — compatibility and publication authority.** Compare frozen
    accepted report/sink/fingerprint bytes and existing failure shapes
    against EM fixtures. Test native output during load/cleanup/exit,
    forged engine-fault JSON, valid-looking trailer and exit 0 after a
    limit. Only the trusted report-channel terminal decision authorizes
    publication. No receipt is appended to partial stdout. Existing
    scratch/sink/post-execution/cancellation semantics remain distinct.

30. **T30 — stage boundaries and repetitions.** Attempt to trigger a
    new budget using fake phases, benchmark repeats, helper execs or
    restarts. No cap/timer resets. Deny a binder/finalizer stage proof
    after compile succeeds and verify it never reads input. Retain the
    run budget through JSON preparation and destructors; separate trusted
    report workers have independent signed launches and deadlines.

31. **T31 — meter conformance.** Use independently instrumented
    allocate/free, dirty/unmap, file create/unlink/last-close, open/dup,
    rapid child/thread churn and child-CPU fixtures, plus a successful
    real compile. Cross-check kernel high waters, normalized RSS units,
    cgroup CPU totals without double-counted waited children, shared
    project usage and channel byte counts. Remove/overflow a required
    meter: admission/runtime fails explicitly; sampled lower bounds and
    unknown/not-started fields never appear as complete measured peaks.

### Hosting-layer conformance checklist

**R15 — deployable host evidence (T1–T5, T23–T31).** The hosting layer MUST ship a
versioned host policy and conformance bundle identifying:

- Implemented limits/defaults and the exact P1–P6 readback adapters, including
  quota units, cpuset/CPU distinction, causal observers and high-water meters.
- Trusted launch stubs for codegen, compile/helpers, run and report/finalizer;
  signing/public-key distribution, single-use identity and local opt-out policy.
- Protected stage/job watchdogs, whole-tree stop and measured E; failure of
  the supervisor cannot leave an unconfined running child.
- Workdir/project/uid/arena charge ownership and concurrent reservations;
  crash recovery, retained artifact transfer and bounded reaper behaviour.
- Report-channel collector integration and terminal usage, with actual image
  and toolchain digests rather than an assumed engine checkout.
- Passing T1–T31 records on that host configuration. An unsupported kernel,
  filesystem or toolchain feature is an explicit admission refusal.

### Real-host verification procedure

**R16 — reproducible acceptance (T1–T31).** The verification team MUST:

1. Pin the image, engine/harness/codegen/toolchain and host policy identities;
   capture kernel/cgroup/filesystem and meter capabilities from the trusted side.
2. Run T1 first, then missing/weaker/forged/stale admission tests before
   destructive exhaustion fixtures. Use isolated finite test reservations
   on the actual host, not the production shared root filesystem.
3. Save the issued signed proof, independent installed-state capture,
   real child status/signal, typed trusted outcome, usage and artifact
   disposition for every test variant. Record absent start sentinels
   for every denied stage.
4. Compare observed caps, stop latency and accounting against the exact
   signed plan, including descendants, overlap, cache/shmem, files/inodes
   and failure paths. Run concurrency and restart/residue cases.
5. Produce a pass/fail matrix mapping every R1–R18 and table row to its
   test evidence. A larger hand-edited policy after failure is a new
   tested configuration, not evidence that the original tier passed.

## Open decisions and unread host facts

These decisions select an implementable policy, not whether fail-closed
admission and complete stage coverage are optional.

| Decision role | Decision | Recommended default |
| --- | --- | --- |
| the hosting layer / the release owner | Numeric service tiers, stop allowance, reservation cost and escalation | Start with the explicitly proposed reference policy; measure it with the verification team. Any change is a signed policy revision/new attempt, with no unlimited hosted defaults. |
| the hosting layer | Quota backend and guaranteed space/inode reservation | XFS enforced project hard quotas plus admission reservations on a disk-backed per-run root; no directory-size polling as enforcement. |
| the hosting layer | Kernel causal observers and peak meters | Kernel-maintained per-cgroup/task/fd/inode maps plus standard cgroup counters; refuse kernels without the required equivalent coverage. |
| the engine implementers / the hosting layer | Physical report-worker boundary and host receipt transport | Preserve the full producer lifetime in `run`; separate trusted consumer/publisher processes per report-channel contract; never trust a native-process phase to relax resources. |
| the engine implementers / the hosting layer | Final external receipt schema and catalog implementation | Adopt the proposed fields/code additions additively, keep legacy report bytes and current code meanings, record host versus engine provenance explicitly. |
| the certified-facts producer | Certified facts producer and exact manifest/output/input requirements | Bind certified facts to source/settings/data/artifact. Unknown facts use a finite policy reservation; they do not become zero or an invented count bound. |
| the hosting layer / the release owner | HPO worker concurrency, shared feeds, live/durable runner hosting | Per-trial private/slab/arena/report reservations plus separately budgeted coordinator/sampler/watchers; require a job-specific contract before claiming conformance. No implicit adoption for durable live service. |
| the hosting layer / the release owner | Development exception and capacity retained after failure | Explicit local-only opt-out; hosted dispatcher rejects it. Failed cleanup quarantines the reservation until reaped or transferred. |

UNVERIFIED about the current host: no actual installed RLIMIT values,
cgroup hierarchy/controller/ancestor settings, quota/enforcement/reservation
state, arena creator/charge/pre-touch/alias lifetime, seccomp or path policy,
network namespace, compiler internal tree, actual fd/task peaks, stop
latency, signer/receipt deployment, finalizer/drainer topology, crash reaper,
HPO concurrency/sharing, or deployed image digest was read. The supplied
audits explicitly leave these external facts unverified. This document
makes no claim that the hosting layer currently implements any of its proposals.

The durable live runner's ledger/WAL/cache and repeated-report lifetimes
need a separate finite service/job envelope; this proposal does not infer
conformance from the batch scratch profile. EM's scratch documentation
also distinguishes that product: `docs/pages/scratch-tables.md`.

## Implementation notes

The following is dependency order, not a claim these changes exist at EM.

1. **Catalog and carrier first.** Register the four proposed codes/closed
   vocabularies in `docker/run_failure_codes.json`; regenerate the
   catalog-dependent registry/docs/release diff through the repository's
   normal implementation workflow. Preserve existing classes, retryability,
   English and args. Current catalog/versioning anchors:
   `docker/run_failure_codes.json`,
   `docs/pages/run-failure-codes.md`.

2. **Trusted launcher, proof validator and host adapter.** Add a shared
   engine-side hosting-proof module/launch stub, PROPOSED
   `docker/hosting_budget.py`, implementing bounded parsing, signature/
   identity/freshness checks, P1–P6 corroboration and typed failures.
   The hosting layer supplies signed kernel readback and protected supervision;
   engine code cannot manufacture quota/watchdog installation proof.
   Add this module to the shipped harness/image as a versioned dependency.

3. **Cover preparation and compiler before run_json.** Change the launch
   choreography at `docker/entrypoint.sh`.
   Invoke trusted gated stages rather than placing only run_json in a jail.
   Route compile/transpile diagnostics through bounded report-channel collectors.
   Preserve compiler flags, especially `-ffp-contract=off` and whole-archive;
   compiler optimization experiments are outside this change.

4. **Pre-load run admission on every entry.** Extend `main`/`_main` and
   the shared pre-load path before ordinary, selected and capabilities
   calls to `load_strategy`. Current anchors:
   `docker/run_json.py`.
   Keep the existing per-handle manifest and arena checks; add host proof
   before library constructors, rather than replacing those checks.
   Current functions: `default_scratch_manifest`, `apply_scratch_tables`,
   `_require_jailed_arena` at
   `docker/run_json.py`.

5. **Bound report preparation and publication.** Add J checks at the
   encoded-byte writer/preparer, column demand admission before creation,
   and report-channel protected publication coordination. Existing anchors:
   `docker/run_json.py`. Do not exempt the BytesIO route or free/
   destructor interval from `run`; preparation and cleanup currently occur
   at `docker/run_json.py`.

6. **Gate and meter finalizer/publication.** Require the positive hosted
   U cap in both file and stdout modes before input, validate its stage
   proof, and bind usage to the protected host terminal outcome.
   Preserve finalizer exit/receipt order, overflow U+1 accounting,
   provisional-output rules and actual cleanup failure precedence:
   `docker/pineforge_sink_finalize.py`.

7. **Join usage without rewriting frozen bytes.** Keep engine attachment
   counters in their existing diagnostics/snapshot roles and hand them to
   the planned measured-usage receipt with attachment identity. Merge host usage in the external
   receipt, not the fingerprint or sink trailer.
   `docker/run_json.py`,
   `include/pineforge/detail/scratch_arena.hpp`.
   Update the API/hosting documentation around
   `include/pineforge/pineforge.h` and
   `scripts/arena_bound.py` so host assumptions refer to this
   verified contract rather than implying an installed limit.

8. **Ship and qualify the actual image.** Carry the changed harness,
   validator and trusted launch integration into the hosting image.
   The entrypoint handoff describes separately packaged harness copies;
   updating an engine archive alone is not deployment evidence. Bind the verification team's
   T1–T31 results to the exact deployed image/toolchain/policy.

**R17 — existing semantics stay frozen (T1, T21, T27, T29).**
Implementation MUST retain existing arena/manifest failure meanings,
fd 5 ownership, native sink framing/counts/digests, finalizer statuses,
failure-line bounds, report/fingerprint bytes and cancellation progress
semantics. New hosting information belongs in the proposed additive
receipt and registered code additions. It must not relabel
`scratch_io_failed`, `report_sink_failed`, `out_of_memory` or
`run_aborted` based solely on a host guess.

**R18 — conformance is a release condition (T1–T31).**
The hosting integration MUST reject a supposedly hosted invocation until
all required mechanisms, proof validation, outcome collection and applicable
real-host tests are available. A docs-only merge establishes this design;
it does not establish that an image or host is conformant.

## Refusals

None. This authoring work did not build, test, compile or execute a workload.
All acceptance procedures above remain to be performed by the implementing
and real-host verification lanes.
