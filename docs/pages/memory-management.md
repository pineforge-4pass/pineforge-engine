# Memory management {#memory_management}

@tableofcontents

This page tells a host how much memory a run can use, which part of it is
bounded, and which part grows with the bars, the trades or the open orders of
the strategy it runs. Each figure is one of three kinds: a compile-time
constant of the engine, a documented admission policy value, or a formula
evaluated by hand from the pages it names. None is an RSS measurement unless
the text says it was measured. A figure that was not measured says so in its
row.

## Where a run's memory lives {#memory_places}

| Place | What is in it | Grows with |
| --- | --- | --- |
| Process heap | The engine's working book window, the resident windows of the stores below, retained placement rows, settled bracket-family records, the strategy's own containers (arrays, strings, maps, matrices, user-defined types, series and indicator history, drawing objects), the equity curve when no report sink is installed, and the owning arrays of a FULL report | See [what scales with what](#memory_scaling) |
| Scratch backing | The spilled part of the trade ledger, the command journals, the order book, the id tables and the report chain. Without an arena these are unlinked files in a writable, disk-backed directory (`TMPDIR`, else `/tmp`; a tmpfs is refused). In the jailed profile they live in the inherited arena (descriptor 5, a sealed memfd), which the host charges as memory | The run's own counts (bars, closed trades, distinct ids, successor rows) |
| Input bars | The OHLCV arrays. The engine borrows them and copies nothing; they must outlive the run. `docker/run_json.py` maps a binary bars file privately, copy-on-write, so touched pages count toward its resident size | Input bars N |

A default run with no report sink needs a writable, disk-backed scratch
directory even when it makes no trade. The spilled trade rows are `O(T)` on
disk with a small resident window (see below).

## Use the report sink for bounded memory {#memory_sink}

The default route of the C API, with no report sink, keeps the equity curve in process
memory: 24 bytes per script bar, so `O(N)` heap. **Install a report sink to
keep it out of memory.** With a sink the engine keeps only the editable tail
of the curve in memory (`BacktestEngine::store_equity_point` overwrites the held
point when the scratch curve is set, defined at
`src/native_execution_consumer.cpp:13611`). Beginning a sink empties the in-memory vector
(`equity_curve_` at `src/engine_report.cpp:1208`) and installs a
`ScratchCurveWriter` (`src/engine_report.cpp:1211`) that holds at most one
64 KiB batch of 2,730 points (class `ScratchCurveWriter`,
`src/report_scratch.hpp:63`). This holds on the native route and on the Pine
host's legacy FULL sink route alike. The sink file or stream grows with the
run; its writer window is bounded.

One line to install it, on an idle handle before the run (it returns 0 when
accepted):

```c
pf_report_sink_v1 sink = { sizeof sink, 1, "/path/report.sink", "run-1", NULL };
int status = strategy_set_report_sink_v1(handle, &sink);   /* fd form: strategy_set_report_sink_fd_v1 */
```

`NULL` for `scratch_dir` derives the scratch directory from the path's parent.
Three limits to know. The first successful attach of a sink descriptor also
attaches the environment arena if one is configured and sets `SIGXFSZ` to
`SIG_IGN` for the whole process (`include/pineforge/report_sink.h`, the comment
on `strategy_set_report_sink_v1`). A path sink must be a disk-backed regular
file: a non-regular file or, on Linux, a tmpfs or ramfs is refused with
`EINVAL`, and the jailed `run_json.py` profile refuses `--report-sink` paths, so
use `--report-sink-fd` there. The path setter and the fd setter are mutually
exclusive: either one replaces the other kind.
From `docker/run_json.py`, invoked directly (the release entrypoint forwards no
command-line flags; it passes `--report-sink-fd` only when `PINEFORGE_REPORT_SINK_FD`
is set), the same choice is `--report-sink PATH` (a file) or
`--report-sink-fd N` (an inherited FIFO or stream). With a sink the returned
report carries `NULL` trades and equity curve and the totals in its summary;
read the records from the sink. The release container's default (unjailed)
profile installs a temporary path sink, so its equity curve is not the `O(N)`
heap case above; with `PINEFORGE_REPORT_SINK_FD` set it streams to that
inherited descriptor instead. See [Report schema](@ref report_schema).

## What you can set {#memory_controls}

| Control | Kind | Default | What it does |
| --- | --- | --- | --- |
| `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES` | environment, positive decimal `uint64` | unset: no cap | Caps the bytes the command-store backings of one thread may extend, in total. A malformed value fails the run with `scratch_configuration_invalid`; exceeding it fails with `scratch_io_failed` |
| `PINEFORGE_COMMAND_SCRATCH_DIR`, `TMPDIR` | environment, a directory | `TMPDIR`, else `/tmp` | Where scratch files go when no arena is attached. An explicit directory never falls back; a tmpfs or ramfs is refused |
| `PF_SCRATCH_ARENA_FD`, `PF_SCRATCH_ARENA_BYTES` | environment | none | The inherited arena: descriptor 5 (only 5 is accepted) and its size in bytes, in whole 1 MiB chunks (under 1 MiB is refused). A run that crosses it fails with `report_sink_failed`, reason `arena_exhausted`, and the failure carries a lower bound of the bytes needed |
| `PF_SCRATCH_WINDOW_BYTES` | environment | 32 MiB | Total cap on the arena bytes mapped at one time, rounded down to whole pages. Mapped pages count toward the resident size while mapped |
| `pf_scratch_tables_v1` through `strategy_set_scratch_tables_v1` | descriptor of eleven maxima | disk tables; with `bounded_memory`, the policy values of [Scratch tables](@ref scratch_tables) | Sizes the fixed slab of the historical tables: `K_e`, `B_e`, `K_c`, `B_c`, `U`, `B_U`, `V`, `B_V`, `R` and the unused q pair. Exceeding a maximum fails the run with `scratch_capacity_exceeded` |
| A report sink (`strategy_set_report_sink_v1`, `strategy_set_report_sink_fd_v1`) | setter | none | Streams the trades and the equity curve to the sink instead of keeping them in the report. See [Report schema](@ref report_schema) |
| `--spill-dir DIR` | `docker/run_json.py` flag (`docker/run_json.py:5230`), available only when `run_json.py` is invoked directly: the release entrypoint forwards no command-line flags | the system temporary directory, then `/var/tmp` if disk backing is required | The writable directory for engine scratch and unlinked report temporaries when no arena is attached. The jailed profile refuses it |
| `--profile jailed` or `PINEFORGE_RUN_JSON_PROFILE=jailed` | `docker/run_json.py` flag or environment (`docker/run_json.py:5155,5268`); the release entrypoint forwards the environment form only | `unjailed` | Selects the bounded-memory table backend and the inherited arena for the Python host. It is the scratch profile of the engine run, not a read-only container: the entrypoint itself still compiles in `${TMPDIR:-/tmp}`. Every executed success report carries `diagnostics.run_profile` (`"jailed"` or `"unjailed"`) and `diagnostics.scratch_arena_jailed` (true only when the jailed profile ran and the arena admission succeeded; `_add_scratch_arena_diagnostics` at `docker/run_json.py:5697-5702`); the marker records the checked profile and the admission, not an independent check of operating-system confinement |
| `PINEFORGE_REPORT_SINK_FD` | environment, read by the release entrypoint (`PINEFORGE_REPORT_SINK_FD` at `docker/entrypoint.sh:272`) | unset | When set, the entrypoint passes `--report-sink-fd <value>` to `run_json.py`, so the report records stream to that inherited descriptor. `run_json.py` validates the value: a descriptor number from 3 through `INT_MAX`, not 5, not combined with `--report-sink`, not in a benchmark run (`PINEFORGE_BENCH`), not in a validate-only run and not the same number as `--run-phase-fd` (`_validate_sink_options` at `docker/run_json.py:5744-5771`); a refusal is a `run_request_invalid` failure with exit status 2. Unset, nothing changes |
| Resident sizes of the ledger, journal, book and id caches | none in this tree | fixed constants (next section) | There is no setter for a resident size in this release. The trade-ledger read cache is chosen by the engine itself (65,536 entries without a report sink, 128 with one; `src/native_execution_consumer.cpp:3172`); the command journal, the order book, the position-tail window and the id tables have fixed constants (next section) |
| `PF_SCRATCH_ARENA_JAILED` | environment, exactly `1` or `0` | unset | Read with the arena variables by `ScratchArena::attach_from_environment` (declared at `include/pineforge/detail/scratch_arena.hpp:131`, defined in `src/scratch_arena.cpp`). `1` additionally requires the attached arena to be a sealed memfd at attachment, else the attach fails with `EINVAL`; `0` or unset adds no requirement (a direct attach may then use ordinary files); any other value fails with `EINVAL`. The separate admission call `strategy_require_report_scratch_arena_v1` requires a sealed memfd on Linux whatever this variable says (stated in the comment on the `ScratchArena` class in `include/pineforge/detail/scratch_arena.hpp`; see [Scratch tables](@ref scratch_tables)) |

## Fixed resident bounds {#memory_fixed}

These sizes are constants of the engine. They bound the resident part of each
store; the part that spills grows in the scratch backing.

| Store | Resident bound | Bytes per entry | Spills to scratch |
| --- | --- | --- | --- |
| Trade ledger (`TradeLedger`), descriptor window | 64 rows (`kResidentRows`, `include/pineforge/trade_ledger.hpp:38`), and a fixed payload tail of the same size | 168 B per row descriptor (`kRecordBytes`), so 10,752 B for the rows and 10,752 B for the tail; the four strings of a row are variable | Older rows and all string payloads: 168 B plus text per closed trade |
| Trade ledger, read cache | Without a report sink: up to 65,536 entries (`kDefaultCacheRows`; the ceiling `kMaxCacheRows` is 1,048,576). With a report sink installed: 128 entries (`kSinkCacheRows`). Direct-mapped, allocated at the first read at the smaller of the cap and the next power of two that is at least the larger of 64 and the trade count (the comment on `TradeLedger::cache_resident_bytes`, `include/pineforge/trade_ledger.hpp:105`), so a run with few trades holds few entries. Chosen by the engine when the run begins, through `TradeLedger::set_cache_rows` (`src/native_execution_consumer.cpp:3172-3173`); the C API has no setter for it in this release | 312 B per entry (`kLedgerEntryBytes`: 168 B record, 16 B key and generation, 128 inline string bytes), so up to 20,447,232 B (about 19.5 MiB) without a sink and 39,936 B with one; independent of the number of bars | Not applicable (a cache over the spilled rows) |
| Command journal | 2,048 events (`kMaxResident`, `src/native_order.cpp:1705`), in 32-event chunks (`kChunk`), with a floor of 128 events kept (`kKeepResident`) | Not a constant: an event is a variant with variable payload and `sizeof` of a resident slot is not stated in the source and was not compiled. Plus one lazy 163,840 B spill buffer per journal: a 32,768 B write buffer and four 32,768 B reload buffers (`SpillStorage`) | Older events, in pages of 32,768 B (`kSpillPageBytes`, `:1726`) read back through four decoded page slots. An event whose serialized form exceeds one page is refused with `EFBIG` (about 1,360 external openings) and stays resident |
| Order book | 4,096 rows | See the drain plateau below | Older rows |
| Command-store read cache | up to 64 KiB of page bytes per store on the heap (`CachedScratchBacking`, `src/command_scratch_cache.hpp`), at most three stores per run, so 192 KiB per engine, plus slot metadata; pages larger than 64 KiB or smaller than 512 B bypass it | Counted in bytes; independent of extent and bars | Not applicable |
| Scratch files read through a bounded window | one 64 KiB window per file that enables it (`ScratchFile::kCacheBytes`, `include/pineforge/detail/scratch_file.hpp:39`) | Counted in bytes. It bounds the operating-system page cache the file pins (the kernel is asked to drop clean pages); it is not an engine heap cache | Not applicable |
| Report sink writer | a fixed 64 KiB buffer; a record line may not exceed 1 MiB | Counted in bytes | The sink file or stream |
| Position-tail read window | one fixed 64 KiB window | Counted in bytes | The position tail: a 30-byte footer plus the entry id per trade |
| Cohort id table | Rows kept in memory: at most twice the rows left by the last spill pass, plus 64, plus the rows pinned by live exposure or an outstanding reference (the comment above `kMaxResidentCohorts` at `include/pineforge/detail/scratch_defaults.hpp:16`; the threshold is set in `DiskResidentIdTable::spill` at `include/pineforge/detail/disk_resident_id_table.hpp:450-467`, with the floor `kSpillFloor` of 64). That is not a bound in terms of live rows. In the bounded scratch (jailed) profile, admission also refuses a run that would hold more live (non-spillable) cohorts than its sized limit, which is at least 65,536 (`kMaxResidentCohorts`, `include/pineforge/detail/scratch_defaults.hpp:16`) and at most 1,048,576 (`kMaxResidentCohortsCap`), with `scratch_capacity_exceeded`; heap rows and variable payloads are outside the ceiling. The rows that hold an opening or live units are also tracked in an ordered set, one map node each. The ordered index lives on disk and is read through one 64 KiB window per scratch file (`ScratchFile::kCacheBytes`, above); the id-pair store's index file starts at 64 slots and doubles when it grows (`kInitialSlots`, `include/pineforge/detail/id_pair_store.hpp:72`; `grow_index`, `include/pineforge/detail/id_pair_store.hpp:604-608`) | Not measured | The on-disk ordered tree |
| Placement table (retained placement rows) | not a constant. It holds the live rows plus the rows the retirement rules pin, one heap node each; retirement erases the rest every bar, so it grows only for the patterns in the dormant-leg and settled-family rows below | Not measured (`sizeof` of a placement snapshot plus its node was not compiled) | Not applicable |

For the short waiting-child parent-terminal fixture (short ids), the book and
journal caps give a per-engine definition plateau of 1,890,980 bytes (about
1.9 MB) on GCC 13.4 on x86-64. That figure moves with definition size and is not
a bound on total engine memory.

## Scratch bytes: the formula and a worked example {#memory_formula}

The scratch backing of a run is a sum of terms, one per store, each a count
multiplied by a bytes-per-entry figure:

```text
scratch bytes  =  base  +  sum over k of ( count_k x bytes_k )
```

`base` is the part that does not depend on the run's counts: the fixed
portion of the table slab (below) and the chunk and page rounding. The counts
are the run's own: `S` script bars, `T` closed trades, `D` distinct ids, `L`
the longest id in bytes (at most 64 in the bounded profile: the id checks run only when the table slab exists, because `preflight_keys` at `include/pineforge/detail/id_pair_store.hpp:268-269` and `preflight_joined_single` at `include/pineforge/detail/id_pair_store.hpp:317-319` return at once without one, and the cohort table checks `bounded_cohorts()`), `Q` distinct id pairs (`D x D` at most), `R`
successor-root rows and `E` stored equity points. The per-store figures below
are logical peak bytes before the arena's 1 MiB chunk rounding. They are
derived by hand in [the arena bound](../design/arena-bound.md), which also
gives the rounding and the three command stores that this table leaves out.
That derivation is authored and not certified on the corpus.

| Store | Logical peak bytes |
| --- | --- |
| Equity writer, when the curve is spilled | `24 x E` |
| Position tail | `(30 + L) x T` |
| Cohort id table | `(96 + L) x D + 1` |
| Entry-rank id table | `(60 + L) x min(D, 27) + 1` |
| Erased minima data | `(40 + L) x D + (40 + 2L) x Q` |
| Erased minima index | `1024` up to 32 keys, else `24 x slots(D + Q)` |
| Consumed partial-cycle data | `(41 + 2L) x Q` |
| Consumed partial-cycle index | `1024` up to 32 keys, else `24 x slots(Q)` |
| Ordered consumed-cycle fold | `136 x Q + 2088 x max(Q - 1, 0) + 2096 x savedPath` |
| Successor-root rows | `16 x R` |

Here `slots(k)` is the larger of 64 and the next power of two at or above
`2k`, and `savedPath` is `1 + min(Q - 1, 9 x (2L + 2))` for `Q > 0`.

**Worked example.** A run of `S = 100,000` script bars on a C or C++ host
that installs no report sink (the same figures hold for the jailed Python
profile without `--report-sink-fd`; unjailed `docker/run_json.py` installs a
temporary path sink by default, so it is not this case), `T = 1,000` closed trades, `D = 10` distinct
ids of at most `L = 16` bytes (so `Q = 100`), and `R = 2 x S = 200,000`
successor rows:

| Term | Bytes |
| --- | ---: |
| Position tail, `46 x 1,000` | 46,000 |
| Cohort id table, `112 x 10 + 1` | 1,121 |
| Entry-rank id table, `76 x 10 + 1` | 761 |
| Erased minima data, `56 x 10 + 72 x 100` | 7,760 |
| Erased minima index, `24 x 256` | 6,144 |
| Consumed partial-cycle data, `73 x 100` | 7,300 |
| Consumed partial-cycle index, `24 x 256` | 6,144 |
| Ordered fold, `13,600 + 206,712 + 209,600` | 429,912 |
| Successor-root rows, `16 x 200,000` | 3,200,000 |
| **Listed stores, logical peak** | **3,705,142** |

The equity curve is not in this sum: with no sink it stays on the heap, at
`24 x 100,000 = 2,400,000` bytes plus the vector's spare capacity. The trade
ledger window adds a fixed `10,752 + 10,752 = 21,504` bytes of heap, and the
spilled rows add `168 x 1,000 = 168,000` bytes of scratch plus their text.
These are hand-derived logical peaks, not an allocation or RSS measurement,
and they exclude the engine's own objects, the strategy's containers and the
input bars.

**The table slab.** With the `bounded_memory` backend the historical id tables
live in one slab prepared before the run. Its size, for a finite batch of
`S` input bars under the default manifest, is:

```text
slab bytes = 4096 x ( 14924 + ceil( 32 x S / 4096 ) )
```

The fixed `14924` pages (61,128,704 bytes) hold the erased, consumed, cohort and
exit-rank regions at their policy maxima, and `32 x S` bytes is the successor
root region. For `S = 100,000` the slab is `4096 x (14924 + 782) = 64,331,776`
bytes. The jailed profile charges this slab and the inherited arena together;
[Scratch tables](@ref scratch_tables) has the full table.

## Default budgets and what they cost {#memory_defaults}

This release has no per-kind resident budget that a host can set, and the page
makes no claim that a run stays out of the scratch backing inside its bar loop.
The defaults that exist are the engine's own constants, listed here so a host
can add them up; each is read from the source named in
[Fixed resident bounds](#memory_fixed).

| Kind of store | Default budget | Resident memory at that budget |
| --- | --- | --- |
| Trade ledger | Descriptor window of 64 rows; read cache of 65,536 entries without a report sink, 128 with one. Chosen by the engine, not settable from the C API | 21,504 B for the window and its tail, plus up to 20,447,232 B of cache without a sink (39,936 B with one) |
| Command journal | 2,048 resident events, 128 kept at least | Not a constant per event; plus 163,840 B of spill buffers per journal once it first spills |
| Placement rows | No budget: the live rows plus the rows the retirement rules pin | Not measured |
| Id tables | Cohort table: resident rows follow the spill rule above (at most twice the rows left by the last pass, plus 64, plus pinned rows), and the bounded profile admits at most its sized limit of live cohorts (65,536 at least, 1,048,576 at most). The ordered index is read through a 64 KiB window per scratch file | 64 KiB per file; the index itself is on disk |

## Getting the bound before a run {#memory_before}

* The slab: evaluate the formula above, or call `default_scratch_manifest`
  with the number of bars (header `include/pineforge/detail/scratch_defaults.hpp`;
  the Python host mirrors it) and size the slab from the manifest. A host can
  also set its own maxima through `pf_scratch_tables_v1`.
* The arena: the host chooses `PF_SCRATCH_ARENA_BYTES`. The calculator
  `scripts/arena_bound.py` returns a certified byte count or marks the term
  `unbounded`. It certifies nothing for a general strategy in this release, because
  the compiler facts it needs are not produced yet, so a host starts from its
  default arena and may re-place the run on a larger one after `arena_exhausted`
  (a host that makes one attempt, such as the current development service,
  returns the failure instead).
* An engine export or report field that returns a planned byte count before a
  run: none exists in this release, so the formula and the constants above are the
  only way to size a run before it starts.

## What scales with what {#memory_scaling}

`N` is the number of script bars, `T` the number of closed trades and `R` the
number of live and pending orders. The last column is the version that fixes
or bounds the row.

| What | Grows with | Where it lives | Fix |
| --- | --- | --- | --- |
| C or C++ host with no sink, and jailed Python without `--report-sink-fd` (the default route of the C API): equity curve | `N`: 24 B per script bar | Heap (a vector, so spare capacity is extra) | Install a report sink (above); a bounded default route is planned, with no release date |
| Same route (no sink): closed trades | `T`: 168 B plus text per trade | Scratch, with 64 descriptor rows resident and a read cache of up to 65,536 entries (312 B each, allocated as trades accumulate); the FULL report built from them is `O(T)` heap | Install a report sink; no release date for the default route |
| Any report sink installed (native or Pine host, history streamed or not) | The heap is bounded for the equity curve (one held point and one 64 KiB batch) and for the ledger (64 descriptor rows and a 128-entry read cache). The trailing block of rows that share the last exit time stays resident only when history is streamed (`K` lots closed at one time, `O(K)`). The sink file and the scratch curve grow with the run | Heap, scratch and the sink file or stream | Not applicable |
| Pine product host (`PineStrategyHost`) | It cannot select `STREAMED` or `SUMMARY_ONLY`. With a sink installed and the default report mode (the legacy route) its report storage is bounded as in the row above: the curve through the scratch writer (`ScratchCurveWriter` at `src/engine_report.cpp:1211`), selected by the Pine host's report policy (`KernelRecordedAtHostMarks` at `src/source/pine_adapter.cpp:3039`) and the install condition (`report_policy` at `src/engine_report.cpp:1205`); and the ledger at 64 descriptor rows (`kResidentRows` at `include/pineforge/trade_ledger.hpp:38`) with the 128-entry sink read cache. Adapter state (placement rows, settled bracket families, script containers) is separate and has its own rows below | Heap bounded for the report; adapter state as the rows below | Not applicable |
| A touched dormant leg that is re-issued | One placement row per re-issue, until the position cycle ends or the origin closes; an untouched re-issue with the same exit id replaces its row in place | Heap | Planned, with no release date |
| Settled bracket families | One fixed record per archived family, which grows with `T` on a bracket strategy; not measured | Heap | Not scheduled |
| Pending-book reads | `R`: `O(R^2)` for a size or a late indexed copy, `O(P R^2)` for a full indexed walk of `P` projected rows, up to `O(R^3)` when `P = R`; this is time, and it was already so in v1.6.3 | CPU | Planned, with no release date |
| Cohort lookups | A cohort lookup was `O(1)` before 1.7.0 and was `O(resident rows + log of the id population x key length)` per call in 1.7.0. Since 1.8.0 a lookup of a resident cohort is `O(log r)` in the number `r` of resident rows, without allocation or disk | CPU, plus scratch reads | Partly fixed in 1.8.0: a full close on N owners seeks the scratch index 2N+1 times (it was N(N+1)/2), and the walks that need only the cohorts holding an opening or live units no longer visit every pending-only cohort. Not fixed: the per-bar retire pass is still proportional to the resident rows, and a close event is still quadratic in N in in-memory work, so in the release's own measurement a dense script (many pending orders) did not finish within 600 s at 10,000 bars and above, and a script with many orders and few ids was only 12.5 to 16 percent faster at 30,000 to 100,000 bars; no release date for the rest |
| Pending orders that never fill | `R`: about 2.5 kB each on dense scripts (the release notes' known limit) | Heap | Planned for a later release, with no date |
| Closed-trade reads from a script | `O(T)` per call, one scratch read per older row; not measured | CPU and scratch I/O | Planned, with no release date |
| The strategy's own containers (arrays, strings, maps, matrices, user-defined types, series and indicator history, drawing objects) | What the script stores | Heap, unchanged from v1.6.3 | Planned, with no release date |
| Distinct ids, pairs and successor rows | `D`, `Q`, `R`, by the formulas above | Scratch, or the slab | Bounded by the admitted maxima |
| Input bars | `N` bars of OHLCV | The host | Not applicable |

## When a bound is exceeded {#memory_failures}

| Signal | Meaning |
| --- | --- |
| `scratch_capacity_exceeded`, argument `resource` | A slab region ran out, more cohorts are live in the bounded profile than its sized limit (65,536 at least, 1,048,576 at most), or an id is longer than 64 bytes in the bounded profile (refused before any store change). Admit larger maxima or shorter ids before another attempt |
| `report_sink_failed`, reason `arena_exhausted` | The inherited arena filled. A host may re-place the run on a larger arena; one that makes a single attempt returns the failure |
| `scratch_io_failed` | A scratch read, write, create or resize failed, or the command-scratch byte cap was reached |
| `scratch_configuration_invalid` | `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES` is not a positive decimal |
| `out_of_memory` | The allocator failed. It is not relabelled as a capacity failure |

See [Run-failure codes](@ref run_failure_codes) for the arguments and the
retry rules.
