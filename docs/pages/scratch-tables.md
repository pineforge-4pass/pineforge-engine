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
| `bounded_memory` / `PF_SCRATCH_TABLE_BOUNDED_MEMORY` | 1 | Scratch storage is backed by the host-supplied byte allowance. Historical tables grow in the attached spool, and exhaustion reports the requested and allowed bytes; population estimates do not refuse a run. |

Call #strategy_set_scratch_tables_v1 on an idle handle before running it.
The `pf_scratch_tables_v1` descriptor in `include/pineforge/pineforge.h`
has four `uint32_t` fields (`struct_size`, `version`, `backend`, `reserved`),
then eleven `uint64_t` maxima in this order:
`K_q`, `B_q`, `K_e`, `B_e`, `K_c`, `B_c`, `U`, `B_U`, `V`, `B_V`, `R`.
Set `struct_size=sizeof(pf_scratch_tables_v1)`, `version=1`, and `reserved=0`.
The targeted ABI size is 104 bytes, with the first maximum at byte 16.
The setter copies the values and retains no pointer into a ctypes temporary.

`K`, `U`, `V` and `R` remain ABI fields for callers that describe a sizing
hint. They are not population admission limits. `B` fields describe byte
estimates; physical allocation is charged to the shared spool allowance,
including links, key text, payload, indexes, fold data and chunk slack. The q
store is removed: its descriptor fields remain for compatibility and remain
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

## Default byte policy

`default_scratch_manifest(std::uint64_t bars)` is the header-only helper in
`include/pineforge/detail/scratch_defaults.hpp`. The Python host mirrors this
descriptor. Population fields are zero compatibility hints; the byte fields
remain available to describe expected work, while the attached spool is the
actual shared allowance.

| Descriptor field | Default value |
| --- | ---: |
| `K_q`, `B_q` | 0, 0 (q removed) |
| `K_e`, `B_e` | 0 population hint, 25,165,824 byte estimate |
| `K_c`, `B_c` | 0 population hint, 4,227,072 byte estimate |
| `U`, `B_U` | 0 population hint, 4,194,304 byte estimate |
| `V`, `B_V` | 27 descriptor units, 6,912 byte estimate |
| `R` | 0 population hint |

The no-argument C++ overload selects the same byte policy; Python uses
`default_scratch_manifest(None)`. The checked bar arithmetic remains a layout
validity check, not a population admission check. A caller may supply an
explicit byte allowance through the host's scratch budget.

The byte estimates do not impose a key or trade ceiling. A single ID longer
than 64 bytes remains a representation refusal. Repeated replacements and
large historical tables consume the shared spool and can exhaust its byte
allowance.

### Byte accounting

The budget is physical bytes, not rows. Charge serialized records, links,
key text, indexes, fold/undo records, replacement indexes and whole-chunk
slack. The arena claims whole 1 MiB chunks and reports the requested physical
need before a claim; mapped windows are a separate resident-byte limit.

The old page-sized reservation table is no longer an admission formula.
Charge serialized records, links, key text, indexes, fold/undo records,
replacement indexes and whole-chunk slack to the shared spool byte allowance.

The declared bar count and receipt per-bar needs can provide a byte estimate,
but they do not reserve a population-sized anonymous mapping. Region
directories, the existing 1,024-byte root tail, fixed caches, OHLCV, strategy
heap and reports are additional. Concurrent runs charge their own byte
allowances.

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
the reserved report channel carries the summary and failures, while native fd 1
diagnostics go to stderr; the trusted finalizer consumes the v1 records. Neither
jailed route constructs a sink file or JSON spool. The library can write the saved
report descriptor because it runs in this process. The descriptor is non-inheritable,
so exec'd children do not get it. With stdout and stderr kept separate, this is
transport isolation rather than a security boundary.

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
| `scratch_capacity_exceeded` | `limit`, `need`, `resource` (`spool_bytes` for byte exhaustion) | resource | false | Refuse completion and refund the run/trial reservation. The host may retry with a larger byte allowance. |
| `scratch_io_failed` | `{}` | resource | false | Refuse completion and refund the run/trial reservation. Investigate backing I/O/corruption. |

The population resources remain in the wire vocabulary for descriptor and
layout validity. Byte exhaustion uses `spool_bytes`; the operation reports its
physical limit and need before semantic mutation.
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

> Scratch tables use a shared byte allowance before the run, with typed failure on exhaustion. Historical distinct IDs, ID pairs, consumed-exit combinations and replacement successors consume physical spool bytes; population estimates do not cap the run. This does not make the strategy heap or OHLCV storage bounded.
