# Memory management {#memory_management}

@tableofcontents

This page tells a host how much memory a run can use, which part of it is
bounded, and which part grows with the bars, the trades or the open orders of
the strategy it runs. Figures are engine constants, documented admission
policy values, or formulas evaluated by hand from the sources named. None is
an RSS measurement unless the text says it was measured. A figure that was not measured says so in its
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

## Report memory: resident buffers, disk and the jailed arena {#memory_sink}

Default C/C++ and Python callers without a report sink keep the native equity
curve on the heap: **24 B per script bar plus vector spare capacity**
(`pf_equity_point_t` include/pineforge/pineforge.h:326-330;
`equity_curve_` include/pineforge/engine.hpp:719).
Building the default FULL report then adds an owning **24 B per script bar**
copy (`equity_curve` src/engine_report.cpp:481-484).

On the default sink route, the engine releases the resident vector and installs
a scratch writer (`equity_curve_` src/engine_report.cpp:1205-1211), keeping an editable
tail for reporting (`fill_report_sink_metrics_section`
src/engine_report.cpp:1460-1467).
The writer batches **2,730 points of 24 B**, within a **64 KiB** window
(`kScratchWindowPoints` src/report_scratch.hpp:18-20;
`ScratchCurveWriter::State` src/report_scratch.cpp:154-161).
Metrics re-read scratch through bounded windows
(`compute_report_sink_metrics` src/engine_report.cpp:1439-1450;
`ScratchCurveWindow` src/report_scratch.cpp:270-310).
This bounds resident equity-report buffers; the scratch backing and output
still grow with the run.

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
read the records from the sink. See [Report schema](@ref report_schema).

The default **unjailed** `run_json` route, with a sink-capable module,
installs a temporary path sink (`_new_report_sink`
docker/run_json.py:5790-5822; `_ReportSink` docker/run_json.py:6183-6220).
The full equity curve is **not resident**: with disk scratch and no arena
attached, its resident equity-curve buffers are **O(1) in bars**.
Binary curve scratch on **disk** still costs **24 B per script bar**
(`flush` src/report_scratch.cpp:229-235):
**6 MiB at 262,144 bars** and **48 MiB at 2,097,152 bars**, calculated payload
sizes excluding other scratch and output.
The sink's `E` output and the default route's JSON artifact also grow with
the bar count: `_build_sink_report` docker/run_json.py:7102-7148 supplies lazy
record arrays, and `_prepare_stream_report` docker/run_json.py:6983-7003
serializes them to a temporary artifact.
Lazy serialization bounds buffers, not output bytes.
The test that checks the resident claim runs with a fixed minimal table
admission (one entry per table); the default bounded-memory table manifest,
which sizes the id tables from the bar count, is outside this claim.

Whenever a scratch arena is attached (a **jailed run with `--report-sink-fd`**
always, and any other run that inherits one), curve scratch instead consumes
the **RAM-backed arena**, so those same **24 B per script bar are memory**.
The fd setter attaches the arena (`attach_from_environment`
src/engine_report.cpp:1067), and `ScratchFile::open`
include/pineforge/detail/scratch_file.hpp:127-134 selects it.
Linux jailed admission requires sealed memfd backing
(`strategy_require_report_scratch_arena_v1` src/c_abi.cpp:1431-1448).
The hosting wrapper supplies `PF_SCRATCH_ARENA_BYTES`
src/scratch_arena.cpp:385-386.
The engine never resizes that backing (the sizing comment above `ScratchArena`
include/pineforge/detail/scratch_arena.hpp:71-110).
Arena allocation rounds to **1 MiB chunks**
(`kChunkBytes` include/pineforge/detail/scratch_arena.hpp:112;
`grow` src/scratch_arena.cpp:297-309); metadata and other stores are additional.
Bounded mapping windows do not remove that arena memory cost.
Exhaustion fails closed with typed `report_sink_failed`
(`ReportScratchIoError` include/pineforge/detail/scratch_arena.hpp:16-21),
reason `arena_exhausted` src/scratch_arena.cpp:234-238.
There is no RAM fallback (the comment on `ScratchFile`
include/pineforge/detail/scratch_file.hpp:30-36).

A **jailed run without `--report-sink-fd` has no sink at all**:
`_new_report_sink` docker/run_json.py:5809-5826 returns `None`, so it retains
the native curve and owning FULL report copy described above. It also builds
the whole report text in memory before writing stdout, and that text is the
largest per-bar term of this route. It is held as UTF-8 bytes: about one byte
of resident text per output byte, plus bounded encoding and read-back buffers
(it used to be held as a string of four-byte characters, about five bytes per
output byte). Measured at 8,000,000 bars (kernel peak RSS slope, stdout
byte-equal): 191.0 B per bar for a module with 63 B per bar of stdout and
289.7 B per bar for one with 144 B per bar of stdout, down from 363.0 and
767.4 B per bar; the remainder of those slopes is not decomposed here. This is
`O(output)`: it is the report the caller asked for.
**"O(1) in bars" is not claimed for the jailed arena or the sink-off path.**
These report bounds also exclude input mappings, strategy histories and the
other stores listed on this page.

For the plain `.bin` input route the input file is mapped, 48 B per bar of file
payload (`_map_bar_file` docker/run_json.py:2685-2708), and the touched pages
count toward resident size as the run reads them. A read of the source sites
in `load_bars_bin` (docker/run_json.py:2779-2811) finds no further per-bar copy of the chart, but
this page publishes no measured figure for the route and does not give the
reading as a bound; the sink and arena paragraphs above govern the report
bytes.

The sink-off jailed route buffers the whole report text before writing to
stdout, so a serialization error publishes no partial report
(`_prepare_report_json`, `docker/run_json.py:7025`; publication in
`_write_report`, `docker/run_json.py:4662`). Its UTF-8 byte buffer uses about
one byte of resident text per output byte, plus bounded encoding and
read-back buffers, on top of the input mapping and the engine's storage:
this is `O(output)` memory for the report the caller requested. Passing
`--report-sink-fd` selects the fd sink before the jailed no-sink fallback
(`_new_report_sink`, `docker/run_json.py:5809`), avoiding the full report-text
buffer; stdout then buffers only the bounded summary.

## The report-sink descriptor: what a caller passes and what it gets {#memory_sink_fd}

This section is for a host that starts the release container (or `docker/run_json.py`) and reads the
report from an inherited descriptor instead of a file. It states what the engine tree guarantees. A
statement the tree does not settle says so.

**Which image has it.** The environment variable `PINEFORGE_REPORT_SINK_FD` is read by the release
entrypoint only from engine 1.8.0 on (`docker/entrypoint.sh:272`; it was added by engine PR #536, after
v1.7.0). On a 1.7.0 image the entrypoint forwards no sink descriptor (the v1.7.0 `docker/entrypoint.sh` has no
such line): the variable is ignored without an error and the run produces its report as it does without
the variable. The flag
`--report-sink-fd N` of `docker/run_json.py` exists in both, so on 1.7.0 only a direct exec of
`run_json.py` can use the descriptor. The hub image runs `/opt/pineforge/entrypoint-dispatch.sh`, which
passes every argument that is not `hpo` to `/opt/pineforge/bin/entrypoint.sh`, the same file as
`docker/entrypoint.sh` (this is read from the hub repository, not from this tree). A caller cannot tell the sink support from a version string alone. The probe that
decides it is `grep -c REPORT_SINK_FD /opt/pineforge/bin/entrypoint.sh` (0 on 1.7.0, 1 from 1.8.0), or
the sha256 of that file against the release's `HARNESS-SHA256SUMS`. After a run, a descriptor that
received no header line, with the trade and equity arrays on stdout, means the variable was ignored.
`run_json.py --capabilities-json` (`_capabilities_main` at `docker/run_json.py:4731`) carries no
report-sink key, so it is not a probe.

**What the caller passes.** A descriptor number N, open for writing in the process that executes the
entrypoint: a FIFO or an `AF_UNIX` stream socket. N is from 3 through `INT_MAX` and is not 5. A request
that breaks any rule below fails at argument validation, before any bar is read, as one failure line on
stdout with code `run_request_invalid` and exit status 2 (`_validate_sink_options` at
`docker/run_json.py:5744-5771`):

| Refused | Why |
| --- | --- |
| N below 3 or above `INT_MAX` | outside the range |
| a value that is not an integer, an empty string included | the option is an integer option (`--report-sink-fd` at `docker/run_json.py:5223`), so the argument parser refuses it with the same code and exit status |
| N = 5 | the engine's scratch arena owns descriptor 5 (`PF_SCRATCH_ARENA_FD`) |
| together with `--report-sink PATH` | the file sink and the descriptor sink are exclusive |
| together with `--bench` or `--validate-window-only` | the descriptor carries one run's stream |
| N equal to `--run-phase-fd` | two streams on one descriptor (the release entrypoint never forwards `--run-phase-fd`) |
| `--report-sink-format 2` | the descriptor route writes format 1 only |

Through the release entrypoint a set value is forwarded as it stands, an empty string included, and
`run_json.py` applies the rules above. Whether the descriptor survives the entrypoint and dispatch `exec`
chain of a built 1.8.0 image is a property of the image, not of this tree; it needs a run in the built
image to confirm.

**Which numbers are free.** 0, 1 and 2 are stdio and 5 is the arena. The engine reserves no other
number: in particular 6 is not reserved. A host that gives 6 to its own sink and also passes 6 as the
`--progress-fd` of the HPO command line has chosen the same number twice, and the engine will not catch
it.

**Route.** The descriptor sink is chosen before the jailed check, so a jailed run that is given the
descriptor uses it (`_new_report_sink` at `docker/run_json.py:5809-5826`). A strategy library without the five descriptor-sink
symbols fails with the typed `symbol_missing` reason (same lines).

**What is written to the descriptor.** The `pf-report-sink/v1` stream described in [Report
schema](@ref report_schema): newline-delimited JSON, each line at most 1 MiB (`MAX_LINE_BYTES`,
`docker/pf_sink.py:29`), one header line, then equity (`E`) and trade (`T`) records, and when requested
the `O`, `V`, `K`, `X` and `H` records, then one trailer line
`{"trailer":"pf-report-sink","version":1,"counts":{...},"sha256":"..."}`. The sha256 covers the header
and every data line with its LF, not the trailer. The writer and the header are byte-identical in v1.7.0
and at main. `run_json.py` closes the descriptor after the run through the `close` method of `_InheritedSinkFd`
(`docker/run_json.py:5844-5858`), so a reader sees EOF.

**What stdout carries.** Only the bounded summary document: about 9 KB in the measured runs (9,274 bytes
at 2 M bars), with no trade or equity array. Do not use it to verify the stream: the stdout summary of a
measured jailed descriptor run carried no sha256 of the stream, so a caller has nothing there to compare
the trailer against.

The host reserves the caller's original stdout as a private report descriptor before argument parsing or
strategy loading (`_ReportOutput` at `docker/run_json.py:7372`; host startup at
`docker/run_json.py:7510`). It redirects native fd 1 to fd 2, and writes the summary and every typed
failure through the saved descriptor. The native stream N stays on the descriptor supplied by the caller.
The host refuses a report descriptor that shares a number or open file with N, the phase descriptor, or
arena fd 5, using `run_request_invalid` (`_validate_report_fd_separation` at
`docker/run_json.py:7472`). Native code runs in the same process and inherits the saved
descriptor, so code that deliberately writes through that inherited file description can still affect
the report. This transport arrangement isolates ordinary native fd 1 output; it is not a security
boundary.

**When the report is complete.** The verified trailer is the completion marker, and EOF alone is not.
Read every record as provisional until the trailer's counts and sha256 equal what the reader counted and
hashed. The reader `SinkReader` in
`docker/pf_sink.py` does this check: its `_finish` rejects a count or sha256 mismatch and bytes
after the trailer (`SinkReader._finish` at `docker/pf_sink.py:341-358`), and a stream that ends without a
trailer raises `missing header or trailer`.

**When it fails.** A failure of `run_json.py` itself is one failure line on the reserved report channel
and exit status 1
(2 for the refusals above). The separate sink finalizer tool,
`docker/pineforge_sink_finalize.py`, has its own exit statuses: 0 complete and verified, 2 usage error,
3 malformed stream, 4 verification failure, 5 input or output failure, 6 end of input without a
trailer, 7 internal error, 8 `sink_exhausted` (the stream exceeds `--max-bytes`;
`sink_exhausted` at `docker/pineforge_sink_finalize.py:100-109`). Its output is provisional until
exit 0, and a consumer must discard everything it received on exit 3 to 8 (`PROVISIONAL` at
`docker/pineforge_sink_finalize.py:24-26`). The failure catalog maps exit 8 to `report_sink_failed`
with `reason` `sink_exhausted` (`sink_exhausted` at `docker/run_failure_codes.json:1025`). A sink failure carries code `report_sink_failed` and a `reason`
from the closed list of the catalog (`report_sink_failed` at `docker/run_failure_codes.json:965`):
`arena_exhausted`, `arena_io`, `arena_backing`, `scratch_io`, `sink_io`, `sink_peer_closed` and
`sink_exhausted`. Only some reasons carry sizes: `arena_exhausted` carries `high_water_bytes`,
`arena_bytes`, `claimed_bytes`, `store` and `needed_bytes_lower_bound`, `sink_exhausted` carries
`sink_bytes` and `cap_bytes`, and every other reason carries `reason` alone, a closed reader included.
A write error while `run_json.py` prepares the report in scratch is raised as `RunFailure` with
`scratch_io` (`docker/run_json.py:7014-7017`); it is not a write to the descriptor. `sink_peer_closed` is also raised when the descriptor setup itself
finds a closed peer (`EPIPE`, `ECONNRESET` or `ENOTCONN`, with the message "report sink fd setup
failed"; `RunFailure` at `docker/run_json.py:5879-5887`). The descriptor then holds records without a
verified trailer, and a caller treats it as absent. The engine writer loops a short write to completion,
retries `EINTR` and fails on a zero-byte write with `EIO`. On a nonblocking descriptor it waits with
`poll` on `EAGAIN`, and a closed peer shows as `POLLERR` or `POLLHUP` (`wait_writable` at
`src/report_sink_writer.cpp:380-387`); on a blocking descriptor the write itself fails with `EPIPE`, as
measured below (the write loop of `flush` at `src/report_sink_writer.cpp:407-431`). Sockets use `send`
with `MSG_NOSIGNAL`. A FIFO host must ignore
`SIGPIPE` for a closed reader to become `report_sink_failed` ([ABI stability](@ref abi_stability)
states this: Python ignores it by default, a C host calls `signal(SIGPIPE, SIG_IGN)`). In a measured run with a reader that
closed early, on a FIFO and on an `AF_UNIX` stream alike, `run_json.py` exited with status 1, was not killed
by a signal (the write failed with `EPIPE`), wrote one failure line with code `report_sink_failed` and
`reason` `sink_peer_closed`, and left stderr empty; the reader's prefix had no trailer. A reader that read
to EOF got exit status 0 and a complete verified trailer.

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
| `--profile jailed` or `PINEFORGE_RUN_JSON_PROFILE=jailed` | `docker/run_json.py` flag or environment (`docker/run_json.py:5155,5268`); the release entrypoint forwards the environment form only | `unjailed` | Selects the bounded-memory table backend and the inherited arena for the Python host. It is the scratch profile of the engine run, not a read-only container: the entrypoint itself still compiles in `${TMPDIR:-/tmp}`. Every executed success report carries `diagnostics.run_profile` (`"jailed"` or `"unjailed"`) and `diagnostics.scratch_arena_jailed` (true only when the jailed profile ran and the arena admission succeeded; `_add_scratch_arena_diagnostics` at `docker/run_json.py:5716-5721`); the marker records the checked profile and the admission, not an independent check of operating-system confinement |
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
| Command-store resident prefix / read cache | Fixed K = 202,752 B (198 KiB) per run: three 64 KiB page buffers plus 2 KiB per decorator, including slot keys (`CachedScratchBacking::kCommandScratchMemoryBytes`, `src/command_scratch_cache.hpp:28`; `static_assert(sizeof(CachedScratchBacking)` at `src/command_scratch_cache.hpp:76`); pages larger than 64 KiB or smaller than 512 B bypass the buffer | Counted in bytes; independent of extent and bars; replaces the previous read-cache allowance | Each disk store holds its prefix in the same buffer until its extent exceeds 64 KiB, then spills the prefix once and reuses the buffer as a read cache. Attached arenas retain direct mapped admission |
| Scratch files read through a bounded window | one 64 KiB window per file that enables it (`ScratchFile::kCacheBytes`, `include/pineforge/detail/scratch_file.hpp:39`) | Counted in bytes. It bounds the operating-system page cache the file pins (the kernel is asked to drop clean pages); it is not an engine heap cache | Not applicable |
| Report sink writer | a fixed 64 KiB buffer; a record line may not exceed 1 MiB | Counted in bytes | The sink file or stream |
| Position-tail read window | one fixed 64 KiB window | Counted in bytes | The position tail: a 30-byte footer plus the entry id per trade |
| Cohort id table | Rows kept in memory: at most twice the rows left by the last spill pass, plus 64, plus the rows pinned by live exposure or an outstanding reference (the comment above `kMaxResidentCohorts` at `include/pineforge/detail/scratch_defaults.hpp:16`; the threshold is set in `DiskResidentIdTable::spill` at `include/pineforge/detail/disk_resident_id_table.hpp:450-467`, with the floor `kSpillFloor` of 64). That is not a bound in terms of live rows. In the bounded scratch (jailed) profile, admission also refuses a run that would hold more than 4,096 live (non-spillable) cohorts (`kMaxResidentCohorts`, `include/pineforge/detail/scratch_defaults.hpp:16`) with `scratch_capacity_exceeded`. The rows that hold an opening or live units are also tracked in an ordered set, one map node each. The ordered index lives on disk and is read through one 64 KiB window per scratch file (`ScratchFile::kCacheBytes`, above); the id-pair store's index file starts at 64 slots and doubles when it grows (`kInitialSlots`, `include/pineforge/detail/id_pair_store.hpp:72`; `grow_index`, `include/pineforge/detail/id_pair_store.hpp:604-608`) | Not measured | The on-disk ordered tree |
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
| Id tables | Cohort table: resident rows follow the spill rule above (at most twice the rows left by the last pass, plus 64, plus pinned rows), and the bounded profile admits at most 4,096 live cohorts. The ordered index is read through a 64 KiB window per scratch file | 64 KiB per file; the index itself is on disk |

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
| C/C++ or Python host with no sink, including jailed Python without `--report-sink-fd` (the default route of the C API): equity curve | `N`: 24 B per script bar plus vector spare capacity, then another 24 B per bar for the owning FULL report copy when the default FULL report is built, plus the whole report text held until written (`O(output)`, about one byte of resident text per stdout byte on the jailed route; see [Report memory](#memory_sink)); sources in [Report memory](#memory_sink) | Heap | Install a report sink (no bounded default route exists in this release); account for disk scratch or jailed arena memory as above |
| Same route (no sink): closed trades | `T`: 168 B plus text per trade | Scratch, with 64 descriptor rows resident and a read cache of up to 65,536 entries (312 B each, allocated as trades accumulate); the FULL report built from them is `O(T)` heap | Install a report sink |
| Default report sink route (native or Pine host, history streamed or not) | Equity-report buffers are bounded (an editable tail and one 64 KiB writer batch, plus bounded metric-read windows); the ledger has 64 descriptor rows and a 128-entry read cache. The trailing block of rows that share the last exit time stays resident only when history is streamed (`K` lots closed at one time, `O(K)`). Binary curve scratch grows at 24 B per script bar; `E` output and the default path route's JSON artifact also grow | Disk scratch without an arena; RAM-backed scratch in the jailed arena, plus resident buffers and the sink file or stream; sources in [Report memory](#memory_sink) | Not applicable |
| Pine product host (`PineStrategyHost`) | It cannot select `STREAMED` or `SUMMARY_ONLY`. With a sink installed and the default report mode (the legacy route) its resident equity buffers and ledger cache are bounded as in the row above: the curve through the scratch writer (`ScratchCurveWriter` at `src/engine_report.cpp:1211`), selected by the Pine host's report policy (`KernelRecordedAtHostMarks` at `src/source/pine_adapter.cpp:3039`) and the install condition (`report_policy` at `src/engine_report.cpp:1205`); and the ledger at 64 descriptor rows (`kResidentRows` at `include/pineforge/trade_ledger.hpp:38`) with the 128-entry sink read cache. Adapter state (placement rows, settled bracket families, script containers) is separate and has its own rows below | Resident equity buffers and ledger cache bounded; curve scratch grows on disk or in the jailed RAM-backed arena; adapter state as the rows below | Not applicable |
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
| `scratch_capacity_exceeded`, argument `resource` | A slab region ran out, more than 4,096 cohorts are live in the bounded profile, or an id is longer than 64 bytes in the bounded profile (refused before any store change). Admit larger maxima or shorter ids before another attempt |
| `report_sink_failed`, reason `arena_exhausted` | The inherited arena filled. A host may re-place the run on a larger arena; one that makes a single attempt returns the failure |
| `scratch_io_failed` | A scratch read, write, create or resize failed, or the command-scratch byte cap was reached |
| `scratch_configuration_invalid` | `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES` is not a positive decimal |
| `out_of_memory` | The allocator failed. It is not relabelled as a capacity failure |

See [Run-failure codes](@ref run_failure_codes) for the arguments and the
retry rules.
