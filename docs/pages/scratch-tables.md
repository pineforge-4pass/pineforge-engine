# Scratch tables {#scratch_tables}

Scratch-table storage is selected independently of the report sink. The
versioned descriptor and fixed-slab foundation are present; the host wiring
and default header described here are authored, with remote verification
pending. Joined backend enforcement, engine expansion of an all-zero
manifest, disk/RAM identity and the complete jailed zero-file-operation proof
are **planned** until those changes and their proof land. Host tests using a
fake library establish orchestration only.

## Backends and descriptor

| Backend | Value | Contract |
| --- | ---: | --- |
| `disk_scratch` / `PF_SCRATCH_TABLE_DISK_SCRATCH` | 0 | Existing temporary-file tables. This remains the unjailed default. |
| `bounded_memory` / `PF_SCRATCH_TABLE_BOUNDED_MEMORY` | 1 | Planned fixed slab for historical table bytes, prepared before the run, with count/byte admission and no growth or disk fallback on exhaustion. |

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

The planned engine rule is: **bounded_memory plus an all-zero manifest uses
the default tier for that run's bar count**. Resolve it once in common
prepare, before allocation/provider mutation, without replacing the stored
all-zero configuration. Any nonzero maximum makes the whole manifest
explicit: zero fields in that manifest stay zero. Disk mode never activates
this default. Layout arithmetic must fit `uint64_t`, `size_t` and `off_t`
before allocation; actual allocation failure remains `out_of_memory`.

## Default P0 admission policy

`default_scratch_manifest(std::uint64_t bars)` is the header-only helper in
`include/pineforge/detail/scratch_defaults.hpp`. These are **policy limits,
not measured bounds**, and do not prove that an arbitrary strategy fits.
The Python host mirrors this tier explicitly, so its fresh-handle wiring does
not depend on the planned all-zero expansion.

| Maximum | P0 value |
| --- | ---: |
| `K_q`, `B_q` | 0, 0 (q removed) |
| `K_e`, `B_e` | 8,192 keys, 3,145,728 key bytes |
| `K_c`, `B_c` | 4,096 keys, 2,101,248 key bytes |
| `U`, `B_U` | 4,096 IDs, 1,048,576 ID bytes |
| `V`, `B_V` | 27 IDs, 6,912 ID bytes |
| `R` for a finite batch | `2*bars`, with checked multiplication |
| `R` without a declared stream lifetime bar count | 65,536 |

The no-argument C++ overload selects the stream tier; Python uses
`default_scratch_manifest(None)`. A zero-bar finite batch has `R=0`.
The current stream-begin API supplies a warmup length, not a declared total
lifetime bar count: even with warmup bars, the planned all-zero stream
default is 65,536. A caller with a finite stream envelope supplies an explicit
manifest for the whole admitted lifetime.

The key/byte choices came from an admission policy using 4,096 roster entries
and 256-byte components; they are aggregate descriptor limits, not a new
per-string length check. Two roots per input bar is an allowance, not an
orders-per-bar bound. Repeated intrabar replacements or loops can exhaust it.
For larger permitted workloads, the host must admit a larger representable
manifest before execution and charge the corresponding memory.

### Hand arithmetic (4096-byte pages)

Define `Q(0)=0`; for positive K, `Q(K)=pow2ceil(max(64,2*K))`.
Data bytes are `40*K+B`, index bytes `16*Q(K)`, cohort bytes
`1+96*U+B_U`, rank bytes `1+60*V+B_V`, and root bytes `16*R`.
Empty cohort/rank populations reserve zero. Round each region separately:
`page(x)=4096*ceil(x/4096)`, `page(0)=0`; the slab is at least one page.

| Region | Unrounded P0 bytes | Pages |
| --- | ---: | ---: |
| Sequence data / index (q) | 0 / 0 | 0 / 0 |
| Erased data (e) | `40*8192+3145728 = 3,473,408` | 848 |
| Erased index (e) | `16*16384 = 262,144` | 64 |
| Consumed data (c) | `40*4096+2101248 = 2,265,088` | 553 |
| Consumed index (c) | `16*8192 = 131,072` | 32 |
| Cohorts | `1+96*4096+1048576 = 1,441,793` | 353 |
| Exit rank | `1+60*27+6912 = 8,533` | 3 |
| Successor roots | `16*(2*bars) = 32*bars` | `ceil(bars/128)` |

The fixed portion is `848+64+553+32+353+3 = 1853` pages, or 7,589,888
bytes. The following totals are hand-derived policy reservations, not an
allocation/RSS measurement:

| Input bars | Root bytes | Root pages | Total slab pages | Slab bytes |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 32,000 | 8 | 1,861 | 7,622,656 |
| 100,000 | 3,200,000 | 782 | 2,635 | 10,792,960 |
| 1,000,000 | 32,000,000 | 7,813 | 9,666 | 39,591,936 |
| 100,000,000 | 3,200,000,000 | 781,250 | 783,103 | 3,207,589,888 |

The stream tier reserves 1,048,576 root bytes (256 pages), for a total of
2,109 pages / 8,638,464 bytes. Region descriptors, shared-owner metadata,
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

For the short waiting-child parent-terminal fixture, the book's 4,096-row
cap and the journal's 2,048-event cap give a per-engine definition plateau
of 1,890,980 bytes (about 1.9 MB) on GNU 13.4 x86-64, independent of symbols,
window and trades. The derivation and exclusions are in
[the scratch-store contract](../scratch-store.md#resident-bound-of-the-parent-terminal-drain-native-order).
This budget is separate from the historical-table slab arithmetic above.

## Python host and HPO

Select `--profile jailed` in `docker/run_json.py`, or set
`PINEFORGE_RUN_JSON_PROFILE=jailed`. Accepted values are `jailed` and
`unjailed`; the default is `unjailed`, and an explicit CLI value wins.
The Python host reads the environment once at startup, before loading a
library or creating a handle. No C++ table/order/bar path reads this knob.
The option configures storage; the external launcher supplies the jail.

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

The host sets `PF_SCRATCH_ARENA_JAILED=1` for jailed arena attachment. Only
exactly `"1"` enables this requirement; unset or exactly `"0"` preserves
the unjailed backing rules, and every other value (including an empty string)
is refused as a malformed profile. A jailed arena must be a sealed memfd
with at least `F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL`; ordinary files,
sealing-disabled memfds and unsupported seals queries are refused before
arena setup, store opening or execution with typed `ReportScratchIoError`
(`report_sink_failed` / `arena_backing`, `EINVAL`). With no
`PF_SCRATCH_ARENA_FD`, attachment remains a no-op; direct `attach(fd, bytes)`
keeps its unjailed meaning.

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
The planned backend checks cover the entire failing operation's count and
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

Planned release wording, pending joined implementation and remote proof:

> Scratch tables use a single allocation before the run, with enforced count and byte capacities and typed failure on exhaustion. Historical distinct IDs, ID pairs, consumed-exit combinations and replacement successors consume that budget; the default replacement reserve depends on input-bar count. This is not a guarantee that only OHLCV memory scales.
