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

The jailed/no-sink stdout route, J0, keeps the legacy result behavior: a FULL
result may retain `O(T)` closed-trade rows and its FULL report projection is `O(T)` heap. With a report sink and a script that never reads closed-trade history by index, closed trades below the published base are not retained in the arena or scratch file beyond the newest retired index and payload bytes per file, fewer than 4,096 bytes (about 24 trade descriptors); disk scratch is anonymous, unlinked and never reopened, while hosted arena bytes stay in the run-owned chunk until the run ends, and the bytes are released when the ledger is cleared or destroyed;
otherwise full history is kept (O(T)). Retirement requires the optional compiled-module symbol `strategy_reads_trade_history()` to return zero and the existing streamable policy; a missing or nonzero symbol keeps full history. A below-base read on a retiring run refuses with `engine_invariant`.

## Report memory: resident buffers, disk and the jailed arena {#memory_sink}

Default C/C++ and Python callers without a report sink keep the native equity
curve on the heap: **24 B per script bar plus vector spare capacity**
(`pf_equity_point_t` include/pineforge/pineforge.h:326-330;
`equity_curve_` include/pineforge/engine.hpp:760).
Building the default FULL report then adds an owning **24 B per script bar**
copy (`equity_curve` src/engine_report.cpp:768-770).

On the default sink route, the engine releases the resident vector and installs
a scratch writer (`equity_curve_` src/engine_report.cpp:1553-1563), keeping an editable
tail for reporting (`fill_report_sink_metrics_section`
src/engine_report.cpp:1814-1828).
The writer batches **2,730 points of 24 B**, within a **64 KiB** window
(`kScratchWindowPoints` src/report_scratch.hpp:21-23;
`ScratchCurveWriter::State` src/report_scratch.cpp:154-161).
Metrics re-read scratch through bounded windows
(`compute_report_sink_metrics` src/engine_report.cpp:1791-1812;
`ScratchCurveWindow` src/report_scratch.cpp:278-311).
This bounds resident equity-report buffers; the scratch backing and output
still grow with the run. The replay needs no descriptor of its own in a jailed run (the arena's inherited descriptor serves it) and exactly one transient anonymous file otherwise; a run leaves the process's descriptor count as it found it.

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
docker/run_json.py:5989-6021; `_ReportSink` docker/run_json.py:6601-6631).
The full equity curve is **not resident**: with disk scratch and no arena
attached, its resident equity-curve buffers are **O(1) in bars**.
Binary curve scratch on **disk** still costs **24 B per script bar**
(`flush` src/report_scratch.cpp:229-235):
**6 MiB at 262,144 bars** and **48 MiB at 2,097,152 bars**, calculated payload
sizes excluding other scratch and output.
The sink's `E` output and the default route's JSON artifact also grow with
the bar count: `_build_sink_report` docker/run_json.py:8014-8112 supplies lazy
record arrays, and `_prepare_stream_report` docker/run_json.py:7871-7894
serializes them to a temporary artifact. The final artifact's aligned I/O
window is fixed at **1 MiB** (`_json_temporary` docker/run_json.py:7875).
Columns, spools and the sink reader retain their **64 KiB** window
(`_BoundedJSONFile.window` docker/run_json.py:7202;
`_UncachedSinkFile` docker/run_json.py:7411;
`_JSONColumn` docker/run_json.py:7512).
Lazy serialization bounds buffers, not output bytes.
Python artifact windows share the per-run constant
`_ARTIFACT_WINDOW_BUDGET_BYTES = 6 * 1024 * 1024` (**6 MiB**).
The final report's **1 MiB** window is reserved first, even though that file
is created last. The equity spool and trade spool receive large windows next,
then the open-time, close-time and event columns in creation order, then one
column per recorded output series in slot order. Each file receives **1 MiB**
while the remaining budget covers it; the crossing file and every later file
use the **64 KiB** class window. The sink reader keeps its own **64 KiB** page.
Peak artifact window memory is at most **6 MiB + 64 KiB x (recorded series,
at most 64) + the 64 KiB reader page**, independent of report size and the
number of trades. Its worst case is **10,551,296 bytes (10.06 MiB)**. The ceiling
of 64 is TradingView's plot-count limit; the engine refuses a larger declaration,
so the bound holds for a library built from this version or later (`run_json`
does not itself refuse an older library that declares more).
Window selection changes no output bytes.
The opt-in corpus RSS comparison (each producer's own peak) allows the sink
route to exceed the full route by **2.5 MiB + 2 × 1 MiB + 64 KiB = 4,784,128
bytes**: the fixed excess measured with 64 KiB windows (at most 1.86 MB, plus a
host-to-host swing), the two sink-only large windows of a run without recorded
outputs (the final report window is on both routes) and the reader page, with
no bar or trade term. It replaces the earlier 65,520-byte figure.
The test that checks the resident claim runs with a fixed minimal table
admission (one entry per table); the default bounded-memory table manifest,
which sizes the id tables from the bar count, is outside this claim.

Whenever a scratch arena is attached (a **jailed run with `--report-sink-fd`**
always, and any other run that inherits one), curve scratch instead consumes
the **RAM-backed arena**, so those same **24 B per script bar are memory**.
The fd setter attaches the arena (`attach_from_environment`
src/engine_report.cpp:1411), and `ScratchFile::open`
include/pineforge/detail/scratch_file.hpp:168-175 selects it.
Linux jailed admission requires sealed memfd backing
(`strategy_require_report_scratch_arena_v1` src/c_abi.cpp:1507-1526).
The hosting wrapper supplies `PF_SCRATCH_ARENA_BYTES`
src/scratch_arena.cpp:400-401.
The engine never resizes that backing (the sizing comment above `ScratchArena`
include/pineforge/detail/scratch_arena.hpp:82-121).
Arena allocation rounds to **1 MiB chunks**
(`kChunkBytes` include/pineforge/detail/scratch_arena.hpp:123;
`grow` src/scratch_arena.cpp:312-324); metadata and other stores are additional.
Bounded mapping windows do not remove that arena memory cost.
Exhaustion fails closed with typed `report_sink_failed`
(`ReportScratchIoError` include/pineforge/detail/scratch_arena.hpp:16-21),
reason `arena_exhausted` src/scratch_arena.cpp:249-253.
There is no RAM fallback (the comment on `ScratchFile`
include/pineforge/detail/scratch_file.hpp:31-37).

A **jailed run without `--report-sink-fd` has no sink at all**:
`_new_report_sink` docker/run_json.py:6009-6027 returns `None`, so it retains
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

The native live runner keeps the cumulative JSON document, the accumulator's
raw equity curve and ratio workspace, and the engine's own arrays resident in
every mode, including the default stream and runs without JSONL output.
This is O(report) memory that grows with equity points and trades, the cost of
one builder of the cumulative document. The numbers come from
`test_native_live_report_memory`: it drives the real runner's default cumulative
stream and default ratio interval on seeded inputs and always prints
`mem n=<N> peak_rss_kib=<K>` for each completed size. On Linux it reads the
runner's `/proc/<pid>/status` `VmHWM` at the final committed input before
releasing stdin EOF, then includes any later peak from `wait4` for that exact
child at exit; macOS skips with a printed reason. The row measures 1,000 and
10,000 inputs and adds 100,000 only when the smaller run's measured time projects
within the remaining work deadline for its build kind, printing the omission
reason otherwise. A peak below 2 GiB and growth below 4 KiB per additional input
between the two largest sizes are deliberate sanity ceilings, not measured
baselines. Measured on the lab 10-28
with the default stream and default ratio interval, on a native test host that
records one equity point per bar: peak RSS 22,880 KiB at 1,000 inputs and
40,024 KiB at 10,000 inputs, about 1.9 KiB per additional input. These are the
runner's own `VmHWM` readings for one fixture, including the accumulator curve,
retained cumulative document and engine arrays. The native market example,
which records no equity points, read 22,472 KiB and 29,828 KiB (about 0.82 KiB
per additional input), consistent with the per-bar equity series being the
difference (the two hosts differ in other ways too).
The row omitted 100,000 inputs because its cumulative-stream projection assumes
output volume quadratic in the input count, exceeding the work deadline for
that build. These two points do not establish a flat memory bound.

For the plain `.bin` input route the input file is mapped, 48 B per bar of file
payload (`_map_bar_file` docker/run_json.py:2799-2822), and the touched pages
count toward resident size as the run reads them. A read of the source sites
in `load_bars_bin` (docker/run_json.py:2893-2925) finds no further per-bar copy of the chart, but
this page publishes no measured figure for the route and does not give the
reading as a bound; the sink and arena paragraphs above govern the report
bytes.

The sink-off jailed route buffers the whole report text before writing to
stdout, so a serialization error publishes no partial report
(`_prepare_report_json`, `docker/run_json.py:7896`; publication in
`_write_report`, `docker/run_json.py:4819`). Its UTF-8 byte buffer uses about
one byte of resident text per output byte, plus bounded encoding and
read-back buffers, on top of the input mapping and the engine's storage:
this is `O(output)` memory for the report the caller requested. Passing
`--report-sink-fd` selects the fd sink before the jailed no-sink fallback
(`_new_report_sink`, `docker/run_json.py:6009`), avoiding the full report-text
buffer; stdout then buffers only the bounded summary.

## The report-sink descriptor: what a caller passes and what it gets {#memory_sink_fd}

This section is for a host that starts the release container (or `docker/run_json.py`) and reads the
report from an inherited descriptor instead of a file. It states what the engine tree guarantees. A
statement the tree does not settle says so.

**Which image has it.** The environment variable `PINEFORGE_REPORT_SINK_FD` is read by the release
entrypoint only from engine 1.8.0 on (`docker/entrypoint.sh:296`).
On a 1.7.0 image the entrypoint forwards no sink descriptor (the v1.7.0 `docker/entrypoint.sh` has no
such line): the variable is ignored without an error and the run produces its report as it does without
the variable. The flag
`--report-sink-fd N` of `docker/run_json.py` exists in both, so on 1.7.0 only a direct exec of
`run_json.py` can use the descriptor. The hub image runs `/opt/pineforge/entrypoint-dispatch.sh`, which
passes every argument that is not `hpo` to `/opt/pineforge/bin/entrypoint.sh`, the same file as
`docker/entrypoint.sh` (this is read from the hub repository, not from this tree). A caller cannot tell the sink support from a version string alone. The probe that
decides it is `grep -c REPORT_SINK_FD /opt/pineforge/bin/entrypoint.sh` (0 on 1.7.0, 1 from 1.8.0), or
the sha256 of that file against the release's `HARNESS-SHA256SUMS`. After a run, a descriptor that
received no header line, with the trade and equity arrays on stdout, means the variable was ignored.
`run_json.py --capabilities-json` (`_capabilities_main` at `docker/run_json.py:4892`) carries no
report-sink key, so it is not a probe.

**What the caller passes.** A descriptor number N, open for writing in the process that executes the
entrypoint: a FIFO or an `AF_UNIX` stream socket. N is from 3 through `INT_MAX` and is not 5. A request
that breaks any rule below fails at argument validation, before any bar is read, as one failure line on
stdout with code `run_request_invalid` and exit status 2 (`_validate_sink_options` at
`docker/run_json.py:5943-5986`):

| Refused | Why |
| --- | --- |
| N below 3 or above `INT_MAX` | outside the range |
| a value that is not an integer, an empty string included | the option is an integer option (`--report-sink-fd` at `docker/run_json.py:5408`), so the argument parser refuses it with the same code and exit status |
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
descriptor uses it (`_new_report_sink` at `docker/run_json.py:6009-6027`). A strategy library without the five descriptor-sink
symbols fails with the typed `symbol_missing` reason (same lines).

**What is written to the descriptor.** The `pf-report-sink/v1` stream described in [Report
schema](@ref report_schema): newline-delimited JSON, each line at most 1 MiB (`MAX_LINE_BYTES`,
`docker/pf_sink.py:30`), one header line, then equity (`E`) and trade (`T`) records, and when requested
the `O`, `V`, `K`, `X` and `H` records, then one trailer line
`{"trailer":"pf-report-sink","version":1,"counts":{...},"sha256":"..."}`. The sha256 covers the header
and every data line with its LF, not the trailer. The writer and the header are byte-identical in v1.7.0
and at main. `run_json.py` closes the descriptor after the run through the `close` method of `_InheritedSinkFd`
(`docker/run_json.py:6044-6058`), so a reader sees EOF.

**What stdout carries.** Only the bounded summary document: about 9 KB in the measured runs (9,274 bytes
at 2 M bars), with no trade or equity array. Do not use it to verify the stream: the stdout summary of a
measured jailed descriptor run carried no sha256 of the stream, so a caller has nothing there to compare
the trailer against.

The process entry reserves the caller's original stdout as a private report descriptor before argument parsing or
strategy loading (`_ReportOutput` at `docker/run_json.py:8359`; `_host_main` at
`docker/run_json.py:8519`). It redirects native fd 1 to fd 2, and writes the summary and every typed
failure through the saved descriptor. The native stream N stays on the descriptor supplied by the caller.
Isolation holds only when the harness keeps stdout and stderr separate. If they name the same file
(for example, with `2>&1`), the host writes one stderr line,
`report channel not isolated: stdout and stderr are the same file`, and continues the run and report
publication through the saved descriptor.
The host refuses a report descriptor that shares a number or open file with N, the phase descriptor, or
arena fd 5, using `run_request_invalid` (`_validate_report_fd_separation` at
`docker/run_json.py:8481`). The library can write the saved descriptor because it runs in this process.
The descriptor is non-inheritable, so exec'd children do not get it. Code that deliberately writes
through that file description can still affect the report. This transport arrangement isolates
ordinary native fd 1 output when the streams stay separate; it is not a security boundary.

Imported library calls write reports and failures to the current Python stdout without reserving or
redirecting descriptors (`_report_output` at `docker/run_json.py:8442`). Each call to the library entry
gets its own publication guard, so repeated runs remain independent (`_LibraryReportRun` at
`docker/run_json.py:8338`).

**When the report is complete.** The verified trailer is the completion marker, and EOF alone is not.
Read every record as provisional until the trailer's counts and sha256 equal what the reader counted and
hashed. The reader `SinkReader` in
`docker/pf_sink.py` does this check: its `_finish` rejects a count or sha256 mismatch and bytes
after the trailer (`SinkReader._finish` at `docker/pf_sink.py:381-399`), and a stream that ends without a
trailer raises `missing header or trailer`.

**When it fails.** A failure of `run_json.py` itself is one failure line on the reserved report channel
and exit status 1
(2 for the refusals above). The separate sink finalizer tool,
`docker/pineforge_sink_finalize.py`, has its own exit statuses: 0 complete and verified, 2 usage error,
3 malformed stream or host-profile refusal, 4 verification failure, 5 input or output failure, 6 end of input without a
trailer, 7 internal error, 8 `sink_exhausted` (the stream exceeds `--max-bytes`;
`sink_exhausted` at `docker/pineforge_sink_finalize.py:104-113`). Its output is provisional until
exit 0, and a consumer must discard everything it received on exit 3 to 8 (`PROVISIONAL` at
`docker/pineforge_sink_finalize.py:24-26`). The failure catalog maps exit 8 to `report_sink_failed`
with `reason` `sink_exhausted` (`sink_exhausted` at `docker/run_failure_codes.json:1105`). A sink failure carries code `report_sink_failed` and a `reason`
from the closed list of the catalog (`report_sink_failed` at `docker/run_failure_codes.json:1045`):
`arena_exhausted`, `arena_io`, `arena_backing`, `scratch_io`, `sink_io`, `sink_peer_closed` and
`sink_exhausted`. Only some reasons carry sizes: `arena_exhausted` carries `high_water_bytes`,
`arena_bytes`, `claimed_bytes`, `store` and `needed_bytes_lower_bound`, `sink_exhausted` carries
`sink_bytes` and `cap_bytes`, and every other reason carries `reason` alone, a closed reader included.
A write error while `run_json.py` prepares the report in scratch is raised as `RunFailure` with
`scratch_io` (`docker/run_json.py:7887-7888`); it is not a write to the descriptor. `sink_peer_closed` is also raised when the descriptor setup itself
finds a closed peer (`EPIPE`, `ECONNRESET` or `ENOTCONN`, with the message "report sink fd setup
failed"; `RunFailure` at `docker/run_json.py:6084-6087`). The descriptor then holds records without a
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
| `--spill-dir DIR` | `docker/run_json.py` flag (`docker/run_json.py:5419`), available only when `run_json.py` is invoked directly: the release entrypoint forwards no command-line flags | the system temporary directory, then `/var/tmp` if disk backing is required | The writable directory for engine scratch and unlinked report temporaries when no arena is attached. The jailed profile refuses it |
| `--profile jailed` or `PINEFORGE_RUN_JSON_PROFILE=jailed` | `docker/run_json.py` flag or environment (`docker/run_json.py:5340,5457`); the release entrypoint forwards the environment form only | `unjailed` | Selects the bounded-memory table backend and the inherited arena for the Python host. It is the scratch profile of the engine run, not a read-only container: the entrypoint itself still compiles in `${TMPDIR:-/tmp}`. Every executed success report carries `diagnostics.run_profile` (`"jailed"` or `"unjailed"`) and `diagnostics.scratch_arena_jailed` (true only when the jailed profile ran and the arena admission succeeded; `_add_scratch_arena_diagnostics` at `docker/run_json.py:5895-5900`); the marker records the checked profile and the admission, not an independent check of operating-system confinement |
| `PINEFORGE_REPORT_SINK_FD` | environment, read by the release entrypoint (`PINEFORGE_REPORT_SINK_FD` at `docker/entrypoint.sh:296`) | unset | When set, the entrypoint passes `--report-sink-fd <value>` to `run_json.py`, so the report records stream to that inherited descriptor. `run_json.py` validates the value: a descriptor number from 3 through `INT_MAX`, not 5, not combined with `--report-sink`, not in a benchmark run (`PINEFORGE_BENCH`), not in a validate-only run and not the same number as `--run-phase-fd` (`_validate_sink_options` at `docker/run_json.py:5943-5986`); a refusal is a `run_request_invalid` failure with exit status 2. Unset, nothing changes |
| `pf_cache_policy_v1` through `strategy_set_cache_policy_v1` | optional bytes per supported cache kind, per handle | internal defaults (next section) | Chooses optional resident cache sizes. Omitted kinds use defaults; explicit zero bypasses caching. Nonzero ledger bytes must convert to 1–1,048,576 decoded entries; other kinds bypass below one line. A cold read uses backing. Settings freeze at run admission; an idle replacement affects the next run. A standalone C++ ledger also accepts an optional row budget through its constructor or `TradeLedger::set_cache_rows`, including zero to bypass. The engine defaults to 65,536 ledger entries with or without a report sink (`TradeLedger::set_cache_rows`, `src/native_execution_consumer.cpp:3207-3208`) |
| Calendar projection and recorded-tape pages | C++ overloads | 256 KiB per calendar column and per recorded tape | Selects optional resident payload and complete shared read-through-cache metadata, allocated lazily; zero, sub-page sizes and optional allocation failures read from the provider through bypass. Each active read has a separate fixed transfer page. Logical rows and raw-array ownership are unchanged; see [Feed projection readers](../feed-projections.md) |
| Resident event and row limits of the command journal and order book | implementation constants | fixed constants (next section) | The optional cache policy does not resize these fixed workspaces or cap retained history. The journal's resident-event threshold and the book's resident-row limit remain fixed |
| `PF_SCRATCH_ARENA_JAILED` | environment, exactly `1` or `0` | unset | Read with the arena variables by `ScratchArena::attach_from_environment` (declared at `include/pineforge/detail/scratch_arena.hpp:142`, defined in `src/scratch_arena.cpp`). `1` additionally requires the attached arena to be a sealed memfd at attachment, else the attach fails with `EINVAL`; `0` or unset adds no requirement (a direct attach may then use ordinary files); any other value fails with `EINVAL`. The separate admission call `strategy_require_report_scratch_arena_v1` requires a sealed memfd on Linux whatever this variable says (stated in the comment on the `ScratchArena` class in `include/pineforge/detail/scratch_arena.hpp`; see [Scratch tables](@ref scratch_tables)) |

## Fixed resident bounds {#memory_fixed}

These are the engine's default sizes and fixed workspaces. The sizes bound the
resident part of each store; the part that spills grows in the scratch backing.
Supported optional caches can use the policy below; their retained data remain
in backing.

| Store | Resident bound | Bytes per entry | Spills to scratch |
| --- | --- | --- | --- |
| Trade ledger (`TradeLedger`), descriptor window | 64 rows (`kResidentRows`, `include/pineforge/trade_ledger.hpp:42`), and a fixed payload tail of the same size | 168 B per row descriptor (`kRecordBytes`), so 10,752 B for the rows and 10,752 B for the tail; the four strings of a row are variable | Retiring sink runs erase published rows and return complete head chunks, keeping at most one head chunk per extent plus the unsealed suffix; each head retains fewer than 4,096 B of the newest retired index and payload bytes per file (about 24 trade descriptors) until the next zero-fill block completes or the chunk holding them is returned; other runs retain 168 B plus text per closed trade |
| Trade ledger, read cache | Default cap of 65,536 occupied entries with or without a report sink (`kSinkCacheRows = kDefaultCacheRows`; the row-setting ceiling `kMaxCacheRows` is 1,048,576). The ledger cap is policy-controlled: frozen host bytes for `PF_CACHE_KIND_LEDGER_CACHE` select the largest row count R with `R*kLedgerEntryBytes + ceil(R/256)*sizeof(void*) <= capacity_bytes`, including the private page directory. The host and standalone byte setters share `TradeLedger::cache_rows_for_capacity` (`include/pineforge/trade_ledger.hpp:61`); run begin applies the host result through `TradeLedger::set_cache_rows` (`src/native_execution_consumer.cpp:3207-3208`). Zero bypasses; nonzero settings below one entry plus one pointer and values converting above the row ceiling are typed setting errors. Storage is private: global index modulo the fixed cap chooses a slot; the first read creates `ceil(cap/256)` owning pointers, and the first touch of page `p` allocates `min(256,cap-256*p)` slots (`TradeLedger::cache_resident_bytes`, `include/pineforge/trade_ledger.hpp:126-132`). Page growth preserves entries through the 64/128/256 boundaries. Writes publish to backing before an optional cache fill. Retirement frees pages outside the retained window, bounding allocated slots by `min(cap,(ceil(retained_rows/256)+2)*256)`. Directory/page allocation failure permanently bypasses further cache allocation for that ledger; reads, appends and replacements continue through backing | 312 B per slot (`kLedgerEntryBytes`: 168 B record, 16 B key and generation, 128 inline string bytes); up to 79,872 B per page, with the last page clipped to the cap. The charge `resident_cache_bytes(cap)` is `sizeof(TradeLedger)+sizeof(State)+cap*312+ceil(cap/256)*sizeof(void*)`: the default optional allowance is 20,449,280 B on LP64, comprising 20,447,232 entry bytes (19.5 MiB) plus 2,048 table bytes. The fixed base is separate. `cache_resident_bytes()` reports the fixed base, allocated directory and allocated pages once in cache resident bytes. The base includes the descriptor window, payload tail and zero-row read workspace; allocator overhead, returned owning Trades and explicit legacy materialization are excluded. Accounted allocation never exceeds the charge. The cap is independent of trade and bar counts | Not applicable (a cache over the retained rows) |
| Command journal | 2,048 events (`JournalStore::kMaxResident`, `src/native_order.cpp:1757`), in 32-event chunks (`kChunk`); spilling stops once the resident count is at most 128 (`kKeepResident`, a stopping threshold rather than a floor) | Not a constant: an event is a variant with variable payload and `sizeof` of a resident slot is not stated in the source and was not compiled. Plus one lazy 163,840 B spill buffer per journal: a 32,768 B write buffer and four 32,768 B reload buffers (`SpillStorage`) | Older events, in pages of 32,768 B (`kSpillPageBytes`, `src/native_order.cpp:1778`) read back through four decoded page slots. An event whose serialized form exceeds one page is refused with `EFBIG` (about 1,360 external openings) and stays resident |
| Order book | 4,096 rows | See the drain plateau below | Older rows |
| Command-store resident prefix / read cache | Up to five 64 KiB payloads, each with at most 128 direct-mapped lines in `ReadThroughCache`. Default K is `5 * (65536 + 128 * (ReadThroughCache::line_bytes(512) - 512) + 2048)` bytes (`CachedScratchBacking::kCommandScratchMemoryBytes`, `src/command_scratch_cache.hpp`): the slab includes all shared tags, pins and replacement metadata; the 2 KiB allowance bounds each decorator's fixed control. For another representable build default C = `kCacheBytes`, the same bound is `K = 5 * (C + floor(C/512) * (ReadThroughCache::line_bytes(512) - 512) + 2048)`. Owners are trade, dependency, exit, lazy dormant book and lazy retained-sequence index. Pages larger than the selected payload (64 KiB by default) or smaller than 512 B bypass it | Independent of extent and bars. For internal payload capacity C and page size P, N = floor(C/P) and optional slab bytes are `C + N * (ReadThroughCache::line_bytes(P) - P)`; control bytes are separate. Capacities are `std::size_t` and page ids are 64-bit; no capacity-sized array enlarges the fixed object when the default is raised. Zero, sub-page or unrepresentable capacity bypasses. The host policy supplies independent inclusive allowances for the supported owners. Optional slab allocation failure selects direct backing I/O. The former 330 KiB K did not include the shared cache's larger metadata and no longer applies | Each disk store holds its prefix in the same slab until its extent exceeds the selected prefix capacity, then spills the prefix once and lends the slab to the shared read cache. Shared tags are constructed only after the entire prefix has landed. Failed spills retain the authoritative prefix for rollback and retry. Direct mode still charges logical extent against the same byte budget. Attached arenas retain direct mapped admission |
| Id-table read caches | Defaults remain 192 KiB of page bytes **per adapter, not per engine** (`IdTableReadCache::kAdapterCacheBytes`, `include/pineforge/detail/id_table_read_cache.hpp`): the cohort AVL and exit-rank AVL use 64 KiB each, and erased minima and consumed cycles use 32 KiB each (data/index share one cache per pair store). Other instances default to zero capacity and allocate nothing. The detail constructors accept `IdTableReadCache::Policy{line_bytes, lines}`; defaults are 4 KiB lines and zero lines. Payload capacity is the product, independent of logical ids and backing extent. Typed `CacheCapacity` constructors and setup setters reserve the shared line metadata within the inclusive byte allowance and forward `Policy{4096, lines}`; zero and sub-line allowances bypass, and line counts are clipped only to representable allocation geometry. The existing byte-count constructors retain their 4 KiB alignment and 64 KiB ceiling. Each adapter uses the shared `ReadThroughCache` with direct mapping and immediate owner writes. Lazy optional-allocation failure takes the exact original backing read; a later logical read can retry allocation. Slab and arena paths bypass allocation and cache lookup, including unjailed regular-file arenas and inherited sealed memfds | The shared allocation is exactly `lines * ReadThroughCache::line_bytes(line_bytes)`, including payload, tags, pins and replacement metadata; the 192 KiB default names payload only. Add `sizeof(IdTableReadCache)` per instance (including the shared cache control), plus a 4 KiB stack transfer workspace per active read. Policies wider than 4 KiB also use one optional line-sized transfer allocation, separately counted; its allocation failure also reads directly. No separate private tags or payload cache remain, and no cache allocation grows with ids. One adapter owns each enabled cache; no sharing across threads. Every write reaches the file immediately; overlapping writes invalidate before I/O, appends invalidate from the old tail page onward, and index growth, clear, moves and backend changes invalidate all lines. Writes spanning more pages than the configured slots conservatively invalidate the following pages of that file too. These are configuration/layout bounds, not measurements | Not applicable (copy-in/copy-out reads over private disk files; formats and logical folds unchanged) |
| Scratch files read through a bounded window | one 64 KiB window per file that enables it (`ScratchFile::kCacheBytes`, `include/pineforge/detail/scratch_file.hpp:40`) | Counted in bytes. It bounds the operating-system page cache the file pins (the kernel is asked to drop clean pages); it is not an engine heap cache | Not applicable |
| Report sink writer | a fixed 64 KiB buffer; a record line may not exceed 1 MiB | Counted in bytes | The sink file or stream |
| Position-tail read window | one fixed 64 KiB window | Counted in bytes | The position tail: a 30-byte footer plus the entry id per trade |
| Cohort id table | Rows kept in memory: at most twice the rows left by the last spill pass, plus 64, plus the rows pinned by live exposure or an outstanding reference (the comment above `kMaxResidentCohorts` at `include/pineforge/detail/scratch_defaults.hpp:16`; the threshold is set in `DiskResidentIdTable::spill`, with the floor `kSpillFloor` of 64). That is not a bound in terms of live rows. In the bounded scratch (jailed) profile, admission also refuses a run that would hold more than 4,096 live (non-spillable) cohorts (`kMaxResidentCohorts`, `include/pineforge/detail/scratch_defaults.hpp:16`) with `scratch_capacity_exceeded`. The rows that hold an opening or live units are also tracked in an ordered set, one map node each. `DiskResidentIdTable::find` walks the disk AVL on every lookup, even for resident rows; those disk reads use the id-table page cache above. `exit_rank_keys_` is a separate plain `DiskIdTable`. The id-pair store's index file starts at 64 slots and doubles when it grows (`kInitialSlots` and `grow_index`, `include/pineforge/detail/id_pair_store.hpp`) | Not measured | The on-disk ordered tree |
| Cohort roster (native working-request core, `cohorts_`) | One in-memory map node per cohort that holds a live origin: `WorkingRequestCore::cohort_add` inserts it and `cohort_remove` erases it with its last origin (`src/native_order.cpp`), and nothing checks the map's size. It is bounded by the strategy's open lots, like the open-trade list, not by bars or closed trades. No engine constant caps open lots in the default profile: `pyramiding` is the script's own input, it limits same-side entries only when above 0 and does not cap `strategy.order`, and the native `max_open_lots` is optional and left unset by the Pine projection (`src/source/pine_adapter.cpp`). Only the bounded scratch (jailed) profile caps the number of cohort rows, because it refuses a run that would hold more than 4,096 live cohorts (`kMaxResidentCohorts`, above) with `scratch_capacity_exceeded`; it does not cap the origins inside one cohort, so the roster still follows the strategy's open lots | 96 B per open lot with one origin (estimate: the map node plus one `RequestHandle`, allocator headers not counted), which is 393,216 B (384 KiB) for 4,096 cohorts of one origin each; every further origin of a cohort adds one `RequestHandle` and vector growth. Measured on one run, trades equal to a build without the counter: 268 open lots at 10,000 bars and 269 at 100,000 bars (25,824 B; that plateau is most likely that script's own sizing, not a cap), and at most 2 on a script that adds and closes one lot per bar (10,000 and 35,000 adds), so it does not follow the bar count or the closed-trade count; the row is freed on close | Does not spill |
| Cohort-target index (native consumer) | Up to 4,096 rows: `kCohortOpeningIndexBytes` (64 KiB, a constant with no setter; production code uses the constant) divided by the row size, applied in `read_indexed_cohort_target` (`src/native_execution_consumer.cpp`). Reserved once, on the first indexed cohort read, even for a few lots; kept across commands and run begin; held until the native consumer is destroyed, so a reused engine run object keeps it. | 16 B per row, so 65,536 B (64 KiB) per native consumer (one per engine run object), independent of bars, trades and lots | Does not spill: above 4,096 open lots, and in the other fallback cases, the original reader answers instead |
| Report artifact I/O windows (`run_json`, unjailed route) | The final report artifact has a 1 MiB aligned I/O window (the only call that passes a window, in the preparation of the final report); every column and spool file and the sink reader page keep the 64 KiB class window. Fixed: the window is a positive multiple of 4096 chosen by the code, not set by a host. Up to eight files are alive in the output-series route (the final artifact plus seven columns and spools) and one reader page. | 1 MiB for the final artifact and 64 KiB for each other file or page, so about 1.5 MiB (1 MiB + 8 x 64 KiB) at most, independent of bars, trades, report size and the number of recorded output series | Disk-backed temporary files; the windows are the resident part |
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
the longest id in bytes (at most 64 in the bounded profile: the id checks run only when the table slab exists, because `preflight_keys` at `include/pineforge/detail/id_pair_store.hpp:277-278` and `preflight_joined_single` at `include/pineforge/detail/id_pair_store.hpp:326-328` return at once without one, and the cohort table checks `bounded_cohorts()`), `Q` distinct id pairs (`D x D` at most), `R`
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

Hosts may optionally choose resident cache sizes per supported type with
`strategy_set_cache_policy_v1`. Defaults are internal. These sizes affect speed
and resident cache bytes only; all retained data remain readable from backing
on a miss. They do not limit logical records or backing extent. A run may read
scratch backing inside its bar loop at any setting.

The caller owns the `pf_cache_policy_v1` descriptor and its entry array. Set
`struct_size = sizeof(pf_cache_policy_v1)`, `version = 1`, and zero both
descriptor reserved words and every entry's reserved word. Supply
`entry_count` entries of `{kind, reserved, capacity_bytes}`. The engine copies their
values during the setter call and retains neither pointer. Each replacement
starts from defaults, so an omitted kind does not retain a previous override.
A null descriptor restores all defaults. Duplicate kinds, unknown kinds,
malformed descriptors and unrepresentable sizes fail atomically with typed
`setting_rejected`; nonzero reserved words use
`{"input":"cache_policy","reason":"unparseable_value"}`. Zero capacity is a
valid bypass setting. Cache kind zero
is reserved and rejected as unknown. The `PF_CACHE_KIND_` values in
`pineforge.h` form an append-only vocabulary numbered 1 through 19; new kinds
use the next unused value. A kind whose cache a later release deletes remains
accepted as a documented retired no-op; it never becomes an unknown-kind error.
The current nineteen kinds all have active store mappings.

Every capacity is in bytes. `PF_CACHE_KIND_LEDGER_CACHE` (value 1) includes
decoded entries and the private page directory. It selects the largest row
count R satisfying
`R * TradeLedger::kLedgerEntryBytes + ceil(R / 256) * sizeof(void*) <= capacity_bytes`.
The entry width is 312 bytes and each page pointer is 8 bytes on LP64.
Both host and standalone byte settings use `TradeLedger::cache_rows_for_capacity`
(`include/pineforge/trade_ledger.hpp:61`). Explicit zero selects the engine's existing
zero-row bypass minimum. A nonzero setting below one decoded entry plus one
page pointer fails with
`setting_rejected` and
`{"input":"cache_policy","reason":"value_below_minimum"}`.
The existing 1,048,576-row ceiling is unchanged; a capacity converting above
it fails with reason `value_above_maximum` and the same input. A remainder is
unused, with no clamping. Allocated pointers and pages are reported once
in cache resident bytes; the fixed ledger/State base is separate. All other kinds include their
optional line metadata; a capacity below one line bypasses those caches.

Ordinary C alignment applies. The entry fields have widths 32, 32 and 64 bits.
The descriptor fields are `struct_size` (32 bits), `version` (32 bits),
`entry_count` (64 bits), `entries` (pointer), then `reserved[2]` (32 bits each).
On LP64 the entry is 16 bytes with offsets 0/4/8; the descriptor is 32 bytes
with offsets 0/4/8/16/24. Both align to 8 bytes.

Set the policy before admission, before the run's first store begin or append.
Begin callbacks, an executing batch and an active stream cannot change it.
Between completed runs, a new setting applies to the next run, including its
warmup, without resizing the preceding run's retained stores or borrowed views.
This setter adds no environment variables or JSON limits.

| Supported cache owner | C++ policy field | Default |
| --- | --- | --- |
| Decoded ledger cache (`PF_CACHE_KIND_LEDGER_CACHE`) | `ledger_rows` (entry and private-directory bytes) | 20,449,280 B on LP64 for 65,536 entries, with or without a report sink: 20,447,232 entry bytes plus 2,048 B of page pointers |
| Trade scratch, dependency scratch, source exit scratch | `trade_pages`, `dependency_pages`, `exit_pages` | 64 KiB per owner |
| Projection-owned exit service | `projection_exit_pages` | 64 KiB; separate from the platform-page source exit service |
| Dormant-book derived index and retained placement sequence index | `book_pages`, `retained_sequence_pages` | 64 KiB per owner; the book's decoded live rows are separate |
| Cohort AVL and exit-rank AVL | `cohort_id_pages`, `exit_rank_pages` | 64 KiB each; decoded cohort residency is separate |
| Erased minima and consumed cycles | `erased_minima_pages`, `consumed_cycle_pages` | 32 KiB each, shared by that owner's data/index files |
| Position-tail read window | `position_tail_pages` | 64 KiB in 16 complete 4 KiB pages, plus one shared metadata charge per page |
| Successor-root, group-receipt and chain-segment read caches | `successor_root_pages`, `receipt_tail_pages`, `chain_segment_pages` | 64, 64 and 16 fixed-width payload rows respectively; pending write batches are separate fixed transaction workspace |
| Ordinary report-sink metric curve reader | `equity_curve_pages` | 2,730 complete 24-byte points; the writer's retry batch is mandatory workspace |
| Numeric Series/rings, HeldHistory, extrema and WMA windows constructed by the run | `ta_history_pages` | Existing owning logical window; finite allowances select backed numeric pages |
| Native-day trade-day/week/month columns | `feed_calendar_pages` | 256 KiB inclusive per column |
| Recorded request tapes installed without an explicit C++ capacity | `recorded_series_pages` | 256 KiB inclusive per tape |

Allowances include the ledger's private entries and page directory or other
caches' optional line payload, tags and replacement/pin metadata;
fixed control state and mandatory transfer/transaction workspaces are
accounted separately.
An explicit capacity on either C++ recorded-tape setter takes precedence
over `recorded_series_pages`, including zero or the internal-default value.
Omitting the capacity selects the admitted run policy at each preparation;
replacing the tape without a capacity restores that inheritance. See
[Feed projection readers](../feed-projections.md).
In particular, host tail allowances size committed-row read caches only;
they do not also resize the 64/64/16-row pending write batches. The retained
sequence owner reuses one optional slab for its prefix and read phases.
The default line counts preserve the existing payload capacities. Optional
cache allocation failure bypasses. For the ledger's private directory and
pages, failure permanently disables further cache allocation for that ledger,
while backing reads, appends and replacements continue normally.
Real backing I/O, corruption, arena/slab
exhaustion and explicit output-materialization failures retain their typed
failures. Journal cache sizing is unavailable until oversized-event storage is
migrated. Generic numeric scratch sequences, selected `SpillCurve` readers,
other TA buffers, nonnumeric Series and raw caller-owned feeds are outside
the supported kinds above.

Scratch-table manifest maxima and the command arena's 1/16 share remain
admission quantities, separate from cache policy. The defaults below let a
host add up the existing owners and workspaces; see
[Fixed resident bounds](#memory_fixed).

| Kind of store | Default budget | Resident memory at that budget |
| --- | --- | --- |
| Trade ledger | Descriptor window of 64 rows; private read cache defaults to up to 65,536 occupied entries with or without a report sink. Hosts control its inclusive entry-and-directory byte cap through `PF_CACHE_KIND_LEDGER_CACHE`, within the existing 1,048,576-row setting ceiling; zero bypasses. Fixed slot mapping uses pages of at most 256 slots, clipped to the cap, allocated on first touch and freed outside a retiring ledger's retained window. Directory/page allocation failure permanently bypasses further cache allocation; backing reads and writes continue | 21,504 B for the window and its tail, plus a default optional allowance of 20,449,280 B on LP64 (20,447,232 B of entries and a lazy 2,048 B pointer table); `resident_cache_bytes(kSinkCacheRows)` charges entries and table plus `sizeof(TradeLedger)+sizeof(State)` (which already includes the window, tail and zero-row read workspace). `cache_resident_bytes()` counts only allocated pages and the directory above that base, once; accounted allocation never exceeds the charge |
| Command journal | 2,048 resident events; spill stopping threshold 128 | Not a constant per event; plus 163,840 B of spill buffers per journal once it first spills |
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
| Same route (no sink): closed trades | `T`: 168 B plus text per trade | Scratch, with 64 descriptor rows resident and a read cache of up to 65,536 entries (312 B each, in 256-slot pages allocated on first touch plus a lazy 2,048 B pointer table on 64-bit targets); the FULL report built from them is `O(T)` heap | Install a report sink |
| Default report sink route (native or Pine host, history streamed or not) | Equity-report buffers are bounded (an editable tail and one 64 KiB writer batch, plus bounded metric-read windows); the ledger has 64 descriptor rows and a policy-controlled private read cache with a default cap of 65,536 entries, in pages allocated on first touch (20,447,232 entry bytes plus the fixed ledger/State base and a lazy 2,048 B pointer table on 64-bit targets). Closed-trade rows below the published base are released only with an explicit no-history capability and a streamable policy (a read below the base then refuses with `engine_invariant`); retirement also frees cache pages outside the retained window; otherwise the full history is kept (`O(T)`). The trailing block of rows that share the last exit time stays resident only when history is streamed (`K` lots closed at one time, `O(K)`). Binary curve scratch grows at 24 B per script bar; `E` output and the default path route's JSON artifact also grow | Disk scratch without an arena; RAM-backed scratch in the jailed arena, plus resident buffers and the sink file or stream; sources in [Report memory](#memory_sink) | Not applicable |
| Pine product host (`PineStrategyHost`) | It cannot select `STREAMED` or `SUMMARY_ONLY`. With a sink installed and the default report mode (the legacy route) its resident equity buffers and ledger cache are bounded as in the row above: the curve through the scratch writer (`ScratchCurveWriter` at `src/engine_report.cpp:1559`), selected by the Pine host's report policy (`KernelRecordedAtHostMarks` at `src/source/pine_adapter.cpp:3044`) and the install condition (`report_policy` at `src/engine_report.cpp:1553`); and the ledger at 64 descriptor rows (`kResidentRows` at `include/pineforge/trade_ledger.hpp:42`) with its policy-controlled private read cache allocated lazily and defaulting to 65,536 entries. Adapter state (placement rows, settled bracket families, script containers) is separate and has its own rows below | Resident equity buffers and ledger cache bounded; curve scratch grows on disk or in the jailed RAM-backed arena; adapter state as the rows below | Not applicable |
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
| `scratch_io_failed` | A scratch read, write, create or resize failed, including real syscall `ENOMEM`, or the command-scratch byte cap was reached |
| `report_sink_failed`, reason `scratch_io` | The native command journal's scratch read or write failed, or its stored bytes or page directory are damaged. An event whose serialized form exceeds the 32,768 B page is refused with `EFBIG` and stays resident. Genuine file-offset exhaustion, disk errors and missing event bytes remain typed failures |
| `scratch_configuration_invalid` | `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES` is not a positive decimal |
| `out_of_memory` | A required C++ allocation failed, including script scratch, arena directories/metadata, store workspace, owning decode, caller-owned output values or error-diagnostic construction. It is not relabelled as I/O or a capacity failure |

Failure to allocate the optional command-cache slab disables both the cache and
its resident prefix for that backing's entire lifetime. Reads and writes use the
same backing, observer and byte budget, preserving the transcript; no later
access retries the allocation. The existing C++ backing diagnostics,
`CachedScratchBacking::read_cache_stats().allocation_bypass_count`, report `1`
immediately after the failure and retain that value across accesses; a cache
disabled by policy reports `0`. A bypassed cache holds no slab. The ledger's
private directory and entry pages are also optional: allocation failure sets a
sticky bypass flag, and no missing allocation is retried until `clear()`
starts a fresh ledger state. Existing cached hits remain usable; misses,
appends and replacements continue through backing. The ledger exposes
`TradeLedger::read_cache_stats().allocation_bypass_count == 1` with typed reason
`TradeLedger::CacheBypassReason::allocation_failed`; policy zero reports zero
failures. Required allocations still fail with `out_of_memory`; backing
I/O failures retain their original typed failure, including `scratch_io_failed`
for syscall `ENOMEM`.

Hosted callers read both cache families through the additive
`pf_report_cache_stats(s, &resident_bytes, &allocation_bypass_count)` export,
after report finalization and before strategy release. `run_json` writes
`diagnostics.cache_resident_bytes` only when the getter reports a positive allocation bypass count. This is the
current live command-cache control/slab and ledger workspace/directory/page
total, not arena claims, backing extents or a sum of cache peaks. The production
sink is `RunCacheDiagnostics::resident_bytes` in `src/run_cache_diagnostics.hpp`:
each live allocation owner registers one reader; the ledger reader uses the
same `State::cache_resident_bytes` formula as `TradeLedger::cache_resident_bytes`.
There is no second ledger byte counter or charge for the command core's borrowed
slab. Embedded registrations allocate nothing and detach when their owner or
sink dies; a new admitted run detaches previous registrations and resets events.

When optional command-slab or ledger-directory/page allocation fails,
`diagnostics.cache_bypass_reason` is `"alloc_failed"` and
`diagnostics.allocation_bypass_count` is the run's total number of sticky
allocation bypasses, including released command owners. All three fields are absent
at zero. Explicitly disabled caches add no failure event. Older modules without
the getter omit all three fields. Existing C ABI structs, including
`pf_report_sink_diagnostics_v1`, stay unchanged at ABI 5.

See [Run-failure codes](@ref run_failure_codes) for the arguments and the
retry rules.
