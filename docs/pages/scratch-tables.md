# Scratch tables {#scratch_tables}

Scratch-table storage is selected independently of the report sink. The
versioned descriptor and fixed-slab foundation are present; the host wiring
and default header described here are authored, with remote verification
pending. Bounded-memory enforcement and all-zero manifest expansion are
implemented in the native begin path. Disk/RAM identity and the complete jailed
zero-file-operation proof remain **planned**. Host tests using a
fake library establish orchestration only.

## Backends and descriptor

| Backend | Value | Contract |
| --- | ---: | --- |
| `disk_scratch` / `PF_SCRATCH_TABLE_DISK_SCRATCH` | 0 | Existing temporary-file tables. This remains the unjailed default. |
| `bounded_memory` / `PF_SCRATCH_TABLE_BOUNDED_MEMORY` | 1 | Slab prepared once before the run, sized from the declared maxima and the input-bar count (about 32 bytes per bar for the root region), with count/byte admission and no growth or disk fallback on exhaustion; historical keys are not reclaimed, and exceeding a cap fails the run. |

Call #strategy_set_scratch_tables_v1 on an idle handle before running it.
The `pf_scratch_tables_v1` descriptor in `include/pineforge/pineforge.h`
has four `uint32_t` fields (`struct_size`, `version`, `backend`, `reserved`),
then eleven `uint64_t` maxima in this order:
`K_q`, `B_q`, `K_e`, `B_e`, `K_c`, `B_c`, `U`, `B_U`, `V`, `B_V`, `R`.
Set `struct_size=sizeof(pf_scratch_tables_v1)`, `version=1`, and `reserved=0`.
The targeted ABI size is 104 bytes, with the first maximum at byte 16.
The setter copies the values and retains no pointer into a ctypes temporary.

`K` counts distinct ever-stored keys across the single-key and exact-pair
domains; `B` counts their total stored key bytes, including embedded NULs and
both pair components. Erasure does not reclaim this historical budget.
`U/B_U` cover historical cohort IDs and their bytes; `V/B_V` cover exit-rank
IDs and bytes, with `V<=27`; `R` covers rooted replacement successors,
including internally generated replacements. The q store is being removed:
its descriptor fields remain for compatibility, and the default sets both to
zero. No descriptor field is a measured occupancy or a live-book count.

The return value is 0 on success, -1 for a null handle or invalid
size/version/backend/reserved/V, -2 during a stream, or -3 during a batch
(including begin callbacks). Refusal preserves the installed configuration.
An idle NULL descriptor restores disk defaults.

The native engine expands an **all-zero bounded_memory manifest using
the default batch or stream tier**. It resolves this once in common
prepare, before allocation/provider mutation, without replacing the stored
all-zero configuration. Any nonzero maximum makes the whole manifest
explicit: zero fields in that manifest stay zero. Disk mode never activates
this default. Layout arithmetic must fit `uint64_t`, `size_t` and `off_t`
before allocation; actual allocation failure remains `out_of_memory`.

## Default P0 admission policy

`default_scratch_manifest(bars, fresh_per_bar=1, literals=0, fixed_fresh=0,
declared=false)` is the header-only helper in
`include/pineforge/detail/scratch_defaults.hpp`. These are **policy limits,
not measured bounds**, and do not prove that an arbitrary strategy fits.
The Python host mirrors this tier explicitly, so its fresh-handle wiring does
not depend on the engine's all-zero expansion. The six historical id maxima
use the **same** `resident_cohort_sizing()` row function as resident admission:
`S = max(65536, min(1048576, literals + bars*fresh_per_bar + fixed_fresh))`.
Resident admission itself applies the function to the bar count alone (`R/2`),
so the receipt sizes the historical regions, not the live limit.
Demand multiplication and additions saturate at `UINT64_MAX`. A declared need
above the cap refuses with typed `scratch_capacity_exceeded`, resource
`cohort`, naming the limit and need; a default-derived excess uses the cap.
The defaults are one fresh ID per bar and zero literal/fixed allowances.

| Maximum | Sized value (exact historical value at the floor) |
| --- | ---: |
| `K_q`, `B_q` | 0, 0 (q removed) |
| `K_e`, `B_e` | `4*S` keys, `96*K_e` bytes (262,144; 25,165,824) |
| `K_c`, `B_c` | `floor(S/2)` keys, `129*K_c` bytes (32,768; 4,227,072) |
| `U`, `B_U` | `S` IDs, `64*U` bytes (65,536; 4,194,304) |
| `V`, `B_V` | 27 IDs, 6,912 ID bytes |
| `R` for a finite batch | `2*bars`, with checked multiplication |
| `R` without a declared stream lifetime bar count | 65,536 |

The no-argument C++ overload selects the stream tier; Python uses
`default_scratch_manifest(None)`. A zero-bar finite batch has `R=0`.
The current stream-begin API supplies a warmup length, not a declared total
lifetime bar count: even with warmup bars, the all-zero stream
default keeps the floor ID tier and 65,536 roots. A caller with a finite stream envelope supplies an explicit
manifest for the whole admitted lifetime.

At the floor every descriptor field retains its historical value. The bounded
profile refuses a single ID longer than 64 bytes before any store mutation;
the byte budgets are aggregate descriptor limits. Two roots per input bar is an allowance, not an
orders-per-bar bound. Repeated intrabar replacements or loops can exhaust it.
For larger permitted workloads, the host must admit a larger representable
manifest before execution and charge the corresponding memory.

The bounded profile additionally admits at most its sized limit of live,
non-spillable resident cohorts: order IDs placed and not yet closed or
cancelled. The limit ranges from 65,536 to 1,048,576; admission beyond it
refuses with the limit, the live count and the approved maximum; the text offers no
declaration remedy, because the per-bar count comes from the module and the cap is the maximum.
This is separate from `U`, which counts historical IDs too; the smaller
remaining allowance governs admission. Live rows stay on the heap and cannot
spill just to fit this ceiling. Closing or cancelling can release resident
admission without reclaiming a historical ID slot.

The internal `resident_cohort_limit(std::size_t requested)` accessor defaults
to 65,536 and clamps to the 1,048,576 cap. The finite-run binder derives bars
from `R/2` and calls `resident_cohort_sizing()`; a stream stays at the floor.
The manifest helper accepts sizing inputs, but no CLI/C-ABI declaration
setting is added here. A caller supplying different demand terms must pass
the same sizing result to its table bind. This ceiling does not bound
pending-order bytes or the variable heap payload of each cohort.

### Hand arithmetic (4096-byte pages)

Define `Q(0)=0`; for positive K, `Q(K)=pow2ceil(max(64,2*K))`.
Data bytes are `40*K+B`, index bytes `16*Q(K)`, cohort bytes
`1+96*U+B_U`, rank bytes `1+60*V+B_V`, and root bytes `16*R`.
Empty cohort/rank populations reserve zero. Round each region separately:
`page(x)=4096*ceil(x/4096)`, `page(0)=0`; the slab is at least one page.

| Region | Unrounded floor-tier bytes | Pages |
| --- | ---: | ---: |
| Sequence data / index (q) | 0 / 0 | 0 / 0 |
| Erased data (e) | `40*262144+25165824 = 35,651,584` | 8,704 |
| Erased index (e) | `16*524288 = 8,388,608` | 2,048 |
| Consumed data (c) | `40*32768+4227072 = 5,537,792` | 1,352 |
| Consumed index (c) | `16*65536 = 1,048,576` | 256 |
| Cohorts | `1+96*65536+4194304 = 10,485,761` | 2,561 |
| Exit rank | `1+60*27+6912 = 8,533` | 3 |
| Successor roots | `16*(2*bars) = 32*bars` | `ceil(bars/128)` |

The floor's fixed portion is `8704+2048+1352+256+2561+3 = 14924` pages,
or 61,128,704 bytes. For sized rows `S`, the fixed bytes are
`page(544*S) + page(16*Q(4*S)) + page(169*floor(S/2)) +
page(16*Q(floor(S/2))) + page(160*S+1) + page(8533)`.
The q regions stay empty; rank admission stays unchanged.

| Sized rows S | Erased data/index pages | Consumed data/index pages | Cohort/rank pages | Fixed pages | Fixed bytes | Added bytes over floor |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65,536 | 8,704 / 2,048 | 1,352 / 256 | 2,561 / 3 | 14,924 | 61,128,704 | 0 |
| 262,144 | 34,816 / 8,192 | 5,408 / 1,024 | 10,241 / 3 | 59,684 | 244,465,664 | 183,336,960 |
| 1,048,576 | 139,264 / 32,768 | 21,632 / 4,096 | 40,961 / 3 | 238,724 | 977,813,504 | 916,684,800 |

Add `page(32*bars)` for roots. The following finite-batch totals use the default
demand (`S=max(65536,min(1048576,bars))`). They are hand-derived policy reservations, not an
allocation/RSS measurement:

| Input bars | Root bytes | Root pages | Total slab pages | Slab bytes |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 32,000 | 8 | 14,932 | 61,161,472 |
| 100,000 | 3,200,000 | 782 | 24,645 | 100,945,920 |
| 1,000,000 | 32,000,000 | 7,813 | 237,186 | 971,513,856 |
| 100,000,000 | 3,200,000,000 | 781,250 | 1,019,974 | 4,177,813,504 |

The stream tier reserves 1,048,576 root bytes (256 pages), for a total of
15,180 pages / 62,177,280 bytes. Region descriptors, shared-owner metadata,
the existing 1,024-byte root tail, live rows/strings, allocator overhead,
OHLCV, strategy heap and reports are additional. Page padding is not extra
admitted capacity. Concurrent HPO trials charge the sum of their slabs;
retiring readers can keep a preceding slab alive too.

## Native request journal residency

The native core's unbound request journal has a resident cap of 2,048 events,
in 32-event chunks, and keeps its 128-event floor. It spills older events even
within one bar when that bar exceeds the cap before retirement. Page size
is still 32,768 bytes and the decoded cache still has four slots.
The bound `CommandScratchStore` codec instead uses its configured workspace.

For the short waiting-child parent-terminal fixture (short ids), the book's 4,096-row
cap and the journal's 2,048-event cap give a per-engine definition plateau of
1,890,980 bytes (about 1.9 MB) on GNU 13.4 x86-64; it moves with definition size and is
not a bound on total engine memory. The derivation and exclusions are in
[the scratch-store contract](../scratch-store.md#resident-bound-of-the-parent-terminal-drain-native-order).
This budget is separate from the historical-table slab arithmetic above.

## Python host and HPO

Select `--profile jailed` in `docker/run_json.py`, or set
`PINEFORGE_RUN_JSON_PROFILE=jailed`. Accepted values are `jailed` and
`unjailed`; the default is `unjailed`, and an explicit CLI value wins.
The Python host reads the environment once at startup, before loading a
library or creating a handle. No C++ table/order/bar path reads this knob.
The option configures storage; the external launcher supplies the jail.

Every successful executed report records the frozen effective profile as
`diagnostics.run_profile`, exactly `"unjailed"` or `"jailed"`.
`diagnostics.scratch_arena_jailed` is a boolean: it is `true` only when the
host selected the jailed profile and native arena admission succeeded,
and `false` for an unjailed run even with an attached sealed arena.
Both fields appear in ordinary and selected reports, including array
reports and FD small reports; failures, capabilities and validation-only
documents carry neither field. They follow the existing optional arena
size counters without changing those counters.

In this profile every fresh handle gets backend 1 and a fresh P0 descriptor
after settings/feed selection and before execution: every warmup, repeat,
ordinary body run, and selected-window execution. Selected mode uses the
retained fed-input count, including pre-roll, rather than the supplied file
length. A trial/run receives fresh occupancy, never the prior trial's usage.

The report sink stays **OFF** unless `--report-sink-fd N` explicitly supplies
a stream. Without it, the host uses ordinary report arrays and the in-memory
JSON path, including lazy arrays; optional recorded outputs use the ordinary
retained-output reader. This preserves the existing report format but does
not make array report assembly bounded in bars or trades. With an fd sink,
stdout carries the summary and diagnostics; the trusted finalizer consumes
the v1 records. Neither jailed route constructs a sink file or JSON spool.

Every jailed handle also calls `strategy_require_report_scratch_arena_v1`
after table admission and before sink installation/execution. It attaches and
validates the inherited arena (`PF_SCRATCH_ARENA_FD=5`, positive
`PF_SCRATCH_ARENA_BYTES`, optional `PF_SCRATCH_WINDOW_BYTES`), or refuses with
typed `report_sink_failed` / `arena_backing`. No attached arena means no
execution and no TMPDIR fallback. `--spill-dir` is refused in this profile;
`--report-sink-fd 5` is refused in every profile because 5 is the engine's
fixed arena slot. The wrapper must charge the arena as well as the table slab.

On Linux, a jailed arena must be a sealed memfd whose seal mask contains
`F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL`. `F_SEAL_EXEC` and other
non-write seals may be added, for example by `MFD_NOEXEC_SEAL`. A mask
containing `F_SEAL_WRITE` or `F_SEAL_FUTURE_WRITE` is refused at attachment
because it prevents writable shared mappings. The jailed admission
(`strategy_require_report_scratch_arena_v1`) enforces the sealed-memfd
requirement regardless of `PF_SCRATCH_ARENA_JAILED`; write seals are refused
in every attachment profile, including direct attachment. Ordinary files,
sealing-disabled memfds and unsupported seals queries are also refused by
the Linux admission before store opening or execution with typed
`ReportScratchIoError` (`report_sink_failed` / `arena_backing`, `EINVAL`),
the same envelope as a missing arena: exit status 1, `code`
`report_sink_failed`, `args` `{"reason":"arena_backing"}`, top-level `reason`
`arena_backing`. These admission refusals mean the run never started. The
admission judges the backend the attachment retained, so an arena attached
earlier is not re-judged by a rebound fd 5. Off Linux, admission accepts any
attached arena.

The success marker records that checked host profile and successful native
admission, not independent OS jail enforcement or a fresh end-of-run backend
query. In particular, `true` off Linux does not certify Linux seals.
Neither positive arena sizes nor `PF_SCRATCH_ARENA_JAILED=1` selects the host
profile or substitutes for admission. A consumer requiring this policy must
require exact `"jailed"` and boolean `true`, refusing absent or malformed
values rather than treating an older report as admitted.

`PF_SCRATCH_ARENA_JAILED=1` is an additional, optional way to demand the same
seals already at attach time, for hosts that attach before they call the
admission. With `PF_SCRATCH_ARENA_FD` set, only exactly `"1"` enables it;
unset or exactly `"0"` permits ordinary-file attachment, but does not relax
the Linux admission's sealed-memfd requirement or permit write-sealed memfds.
Every other value (including an empty string) is refused as a malformed
profile. With no `PF_SCRATCH_ARENA_FD`, attachment remains a no-op (the
admission then refuses for lack of an arena); direct `attach(fd, bytes)` also
permits ordinary files, with the same write-seal refusal on Linux.

The setter is an optional ctypes binding for old libraries in the unjailed
profile. In the jailed profile, an absent setter refuses before execution as
`strategy_library_incompatible` with `{"reason":"setter_missing"}`. The
export name is in the message; the optional `missing` argument is omitted
because its closed catalog vocabulary does not include this name. A setter
refusal is an `engine_invariant`. Neither condition permits disk fallback.
The arena admission export is also required; an older library missing it is
`strategy_library_incompatible` with `{"reason":"symbol_missing"}`.
An invalid environment profile is `run_request_invalid` with the existing
`{"option":"arguments"}` classification.

`benchmarks/throughput/grid_search_repro.py` shares the Python descriptor and
policy helpers. Each of its 27 fresh trial handles installs that trial's
manifest after inputs and before running, and a failed run never enters the
ranking. `scripts/run_strategy.py` is the ordinary corpus/validation driver;
it has no HPO or jailed-profile route and is unchanged here. Production APP
and HPO drivers are in other repositories; their integration is **planned**.

## Typed failures and host refunds

| Code | Typed args | Class | Retryable | Required host action |
| --- | --- | --- | --- | --- |
| `scratch_capacity_exceeded` | `resource` (closed name) | resource | false | Refuse completion and refund the run/trial reservation. Admit a different envelope before a new attempt. |
| `scratch_io_failed` | `{}` | resource | false | Refuse completion and refund the run/trial reservation. Investigate backing I/O/corruption. |

Capacity resources are `layout`, `sequence_data`, `sequence_index`,
`erased_data`, `erased_index`, `consumed_data`, `consumed_index`, `cohort`,
`exit_rank`, and `successor_roots`. q names remain in the wire vocabulary.
The implemented bounded-memory checks cover the entire operation's count and
byte workset before its first semantic mutation, with no grow/rehash/fallback.
Physical allocator failure stays `out_of_memory`; it is not relabeled as a
logical capacity breach. Report-sink failures retain their separate code.
See [Run-failure codes](@ref run_failure_codes) for the channel and catalog.

The host must preserve these engine-originated codes and closed arguments,
withhold a successful report/result, attribute them to infrastructure, and
refund the full reserved charge for ordinary, selected and HPO paths,
including mid-run failure. `retryable=false` forbids blind retries of the
same envelope. Catalog synchronization, billing/refund transport and those
APP acceptance tests remain **planned**, not established by engine host fakes.

## Jail responsibilities and exclusions

**The HOST must configure SIGXFSZ.** APP's `pf-jail.c` must install
`SIG_IGN` for SIGXFSZ **before** applying `RLIMIT_FSIZE`, including the zero
limit, and check that setup succeeds. A successful new inherited-arena attach sets SIGXFSZ to `SIG_IGN` process-wide; detach does not restore it.
Hosts relying on their SIGXFSZ handler or default termination must account for this before attaching an arena. Ignoring the signal makes forbidden file growth report EFBIG;
it does not permit a disk write. Without it, an accidental disk write may
terminate the child before a typed failure/refund can be transported.

APP-visible integration items (external production paths are not asserted):

1. Select the jailed host profile explicitly and supply a runtime with the
   setter plus all bounded backends. Load read-only feeds/modules as required
   by the launcher; do not provision an unused second scratch arena without
   charging both copies.
2. Reserve per-run/trial memory from the checked manifest and concurrency
   entitlement. Cover effective HPO parameters and internal replacements;
   incomplete codegen receipts do not prove a smaller budget.
3. Install the signal disposition above, then enforce the filesystem/file-size
   restrictions. Supply the separately charged inherited arena; keep the sink
   off for array reports or pass an fd stream for trusted v1 finalization.
4. Synchronize the error catalog, preserve typed engine failures, refund them
   in plain/selected/HPO execution, and test that failed trials cannot rank.
5. Prove on the actual Linux jail: no writable directory, `RLIMIT_FSIZE=0`,
   real trades with every table family reached, no table file operations,
   disk/RAM trade/report/digest identity, and typed EFBIG disk controls that
   exit normally rather than by SIGXFSZ. These proofs are **planned**.

This work covers historical scratch tables with the sink OFF. The sink-ON
report chain, position-tail spill (L18), and the durable runner are outside
this coverage; it does not certify their jailed execution or storage bounds.

Release wording for implemented bounded-memory tables; remote disk/RAM identity and jailed proof remain pending:

> Scratch tables use a single allocation before the run, with enforced count and byte capacities and typed failure on exhaustion. Historical distinct IDs, ID pairs, consumed-exit combinations and replacement successors consume that budget; the default replacement reserve depends on input-bar count. This is not a guarantee that only OHLCV memory scales.
