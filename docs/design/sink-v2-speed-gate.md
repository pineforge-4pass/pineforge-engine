# Stage-C lab verification and hard speed gate

Execution owner: WIN-CX, through the lab/cloud host. The editing lane ran none
of these commands. Use committed clean trees, retain raw logs and run IDs,
and bind every verdict to the exact engine tree and frozen codegen head.

## P0: freeze the reviewable diff

Commit the deliverable after hooks/guards; keep DESIGN-sinkv2.md and
REPORT-sinkv2.md outside the proof checkout. Record HEAD, HEAD^{tree},
`git diff --stat dad2530cd4..HEAD`, codegen/corpus revisions, compiler identity,
host class, uname, CPU governor/affinity, nproc/cgroup cpu.max (>=3 tee cores), relay/owner identities and filesystem/memory configuration.
No dirty-tree run is acceptance evidence. Do not post statuses from this lane.

## P1: fail-before and source checks

Before building each new C++ test unit, overlay only its test source/shared
test fixtures onto the frozen previous-epoch header/source closure and compile
there first. Do not overlay tools/sink_v2 production headers. Retain the first
actual diagnostic for each unit. A predicted diagnostic is not proof.

Then run on the final head:

```sh
python3 scripts/ci_preflight.py
python3 scripts/check_c_abi_runtime.py
python3 scripts/check_run_failure_codes.py
python3 scripts/gen_run_failure_catalog_diff.py --check
```

The source checks need the project's pinned actionlint/ShellCheck tools.
Catalog/header/registry/diff generator comparisons must be clean.

## P2: complete build and tests

```sh
python3 scripts/ci_verify.py release --build-dir build --jobs 4
python3 scripts/ci_verify.py debug --build-dir build-debug --jobs 4
python3 scripts/ci_verify.py sanitizers --build-dir build-sanitizers --jobs 4
```

The profile must build **all targets**, prepare the receipt providers and run
all CTest rows, including settlement ABI/checker self-tests. A library-only
build followed by CTest is stale evidence. There is no new CTest timeout.

Require execution of test_report_sink_reader, test_report_sink_transcode_v2,
test_report_sink_v2_writer/index/profiles, test_report_sink_reader_vectors,
test_sink_v2_vector_generate, all seven test_sink_v2_* reader groups,
test_sink_transcode_v2_cli, all A1/tee rows (including kernel VmHWM 100k/1M), the Release RSS row and test_sink_v2_speed_gate.
There is no engine-v2 setter ABI test because this stage adds no setter.
Codec-ON skips are not v2 conformance evidence. Retain summary lines/log paths.

## P3: generate and reproduce canonical vectors

On Linux, the pinned shared conformance codec is built with the executable.
Run, without editing any resulting artifact:

```sh
PF_SINK_ZSTD_LIBRARY="$PWD/build/lib/libpineforge_sink_zstd_reference.so" python3 -B tests/generate_sink_v2_vectors.py --transcoder build/bin/pf_sink_transcode_v2 --output-dir build/sink-v2-vectors
PF_SINK_ZSTD_LIBRARY="$PWD/build/lib/libpineforge_sink_zstd_reference.so" python3 -B tests/generate_sink_v2_vectors.py --transcoder build/bin/pf_sink_transcode_v2 --output-dir build/sink-v2-vectors --check
build/bin/test_report_sink_reader_vectors build/sink-v2-vectors
```

The generator derives the count and manifest from actual cases/tools, compares
every field/inverse, compares report bytes to the retained-v1 producer, and
records typed refusal sidecars where appropriate. No binary/checksum artifact
was supplied by the editing lane. Reader-only injected frames/null-E/forced
layouts are separate controls, never admitted stage-C success files; independently produced serial/tee COMPACT/FULL bytes, SUMMARY and receipts must match.

If the bundle must be committed under docs/design/sink-v2-vectors, run the same
generator with --force-bundle and that output directory on the lab, submit exactly its output,
commit it without hand editing, and repeat the complete required verification
on the final clean tree. Earlier runs are measurements of earlier trees.

## P4: real jail and admission

Use the production stage-C jail definition, not a simulated local wrapper.
Retain its exact argv/config, uid 10002, per-file --fsize, no-network namespace,
read-only image/mounts, scrubbed environment and inherited-fd list. Serial supplies immutable verified input;
tee drops C's A-verified-input prerequisite: only the owner's all-reaped runner/A/C/relay success and input-identity commit rule authorizes publication.

Exercise input cap exact fit/overflow (8), output cap at header/data/footer/tail
and exact fit (9), RLIMIT_FSIZE with SIGXFSZ ignored (9), and file-count zero/
one (10/success). Every failure must lack a published object/complete receipt;
show no clipping, no retained capped prefix, and private spool cleanup.
Prove SIGTERM/SIGKILL and hosting cleanup/I/O faults, plus a prelaunch-through-reap 10 ms publication watcher, dead peers and stopped-A backpressure. Raw
CLI conformance tests do not prove these hosting conditions.

Compare actual extents/block counts to transcode_bounds, across both profiles,
wide O/K, long UTF-8 strings, short blocks and SUMMARY extremes. Obtain the
separate certified image/allocator/container/stack/loader reserve for E_as.
Missing reserve/backing/inclusive memory or a hard runner-stdout bound (owner-drained nonblocking pipe plus <=2 MiB memory) is not admission; an after-read cap alone is insufficient.

## P5: four independent build replicates

Create four new clean checkouts of the proof head: SINK_GATE_LAB/rep1..rep4.
Prepare each pinned corpus and derive feeds once to freshness, sequentially.
Use empty compiler launchers and CCACHE_DISABLE=1. For each checkout, use its
own new build directory and run these commands, retaining separate logs:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPINEFORGE_VERSION_SOURCE=FILE -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON -DPINEFORGE_WITH_ZSTD=ON
cmake --build build --target all corpus_strategies -j4
```

Use one host class/compiler/flags and no shared/hardlinked artifacts. Both
engine archives and the tool archive must be newer than their source closures.
The default harness checks clean same-tree checkouts, fresh archives, distinct
artifacts, pinned codec identities and hashes before/after observation.
Never overlap a corpus rebuild/sweep with external consumers.

## P6: end-to-end hard gate

WIN-CX supplies the real feed with at least 8,000,000 bars, workload config and
names, artifact-bound inclusive-memory evidence, producer build/link log map
and actual caller SUMMARY extremes. The workload set must include a trade-heavy
fixture with >=10,000 trades, O/V/K, and trace. Keep ordinary and selected size
cells and both profile boundaries. Missing coverage fails the gate.

For the existing memory-backed deployment, put output/tmp/scratch on the
approved memory-backed run mounts and retain the backing/arena admission
evidence for the unchanged engine. Disk testing is additional evidence.
SINK_GATE_ZSTD is rep1/build/lib/libpineforge_sink_zstd_reference.so.
SINK_GATE_ZSTD_INFO maps each resolved build root and `reference` to its
actual producer artifact SHA256, codec build ID, raw build/link log and
observer zstd-library SHA256, as checked by bind_summary_evidence().

The exact hard-gate command (with supervisor-bound input paths) is:

```sh
python3 -B scripts/sink_v2_speed_gate.py --build-roots "$SINK_GATE_LAB/rep1" "$SINK_GATE_LAB/rep2" "$SINK_GATE_LAB/rep3" "$SINK_GATE_LAB/rep4" --ohlcv "$SINK_GATE_FEED" --strategy-config "$SINK_GATE_WORKLOADS" --sizes 10000 100000 1000000 8000000 --replicates 5 --warmups 1 --v2-mode tee --v2-compare-late-modes --v1-a-mode stdout --memory-evidence "$SINK_GATE_MEMORY" --zstd-library "$SINK_GATE_ZSTD" --zstd-build-info "$SINK_GATE_ZSTD_INFO" --incompressible-summary-json "$SINK_GATE_CALLER_SUMMARY" --ceiling-incompressible-summary-json "$SINK_GATE_CEILING_SUMMARY" --output "$SINK_GATE_RESULTS"
```

Both driver and codec default to scripts/sink_v2_split_driver.py; `--v2-mode serial` remains the oracle/default and keeps full verification, as does the transcoder CLI. Unset `--v2-late-verify {full,structure}` resolves to structure for tee and full for serial; tee defaults to 4 late threads. `--tee-late-verify full` (driver) or `--v2-late-verify full` (gate) opts into whole-stream verification and inverse; explicit structure requires tee.
`--v2-mode tee` starts a separate runner and a fixed 64 KiB relay feeding stdout-mode A and inline C; runner stdout is owner-drained nonblocking into <=2 MiB, overflow closes/cancels immediately;
the ruled v1 baseline is `--v1-a-mode stdout`. Engine arrays/ABI stay unchanged.
Whole-owner wall includes incremental validation, selected late verification, ranks, all late worker joins, fsync/checked close, all reaps, publication and receipt; full still runs whole-stream verify/inverse, while structure keeps the incremental input-data/I proof, every structural check and stored-prefix reread on at most four workers, without promoting a reader's whole-file verified state. One-strategy 1M-bar probe versus STDOUT-mode v1 on four clean builds: structure+4 threads 1.073x, structure+1 thread 1.118x, full 1.337x (about 1.34x); kernel VmHWM transcoder 9.8 MiB and relay 3.3 MiB flat from 100k to 1M. The 1.10 limit holds at default 4 late threads in the probe; a 1-core host misses it. Replace these probe numbers with the gate box's measurements; they are no gate verdict. `--v2-compare-late-modes` replays each from-v1 seed once AFTER all timed rows, outside every timed observation, as tee/full, tee/structure+4, tee/structure+1 and serial/full, comparing SHA-256 and length at every main cell and every size-grid cell from 100k bars, plus the timed level-3 encodes' own digests; raw rows, summary and receipts bind the equality, and any mismatch refuses the gate with differing files retained.
Per-child wait4 supplies runner user CPU and C user+system CPU to the legacy engine/writer columns;
A and relay CPU have separate names; no overlapping CHILDREN deltas or writer thread is claimed.
Lab timing stays outside SUMMARY. Sample per-process /proc VmHWM for C/relay, never inherited ru_maxrss; repeat the unchanged 100k/1M band/slope proofs with four late workers. Require actual-clock `incremental_validation_done`; structure emits `rank_bucket_done` before `structure_verify_done` and excludes `full_verify_done`/`to_v1_done`; compare both arms' phase logs.
Inclusive evidence must also charge stdout memory and its pipe capacity; sampling retains descendants, private run files and resident tmpfs minus mapped RSS, and reports gaps.

Require >=4 independent builds, >=5 timed runs/arm/build after a warm-up,
alternating arms, per-build medians and two-stage bootstrap. LARGE v2/v1 median
wall <=1.10; small/medium reported. Inclusive peak <=v1+16,777,216 B, flat slopes
<=1 MiB, no omitted output/rank/curve/spill bytes, no unknown backing or memory
gaps, and zero E/T/O/V/metrics/count differences. Keep the full N/T/f grid,
ordinary/selected corner and SUMMARY extremes; summary_limit is REFUSED,
never an accepted file/size ratio. Retain bytes per E/T, shared components,
CPU, wall, F, output/work-file peaks, backing and level table together; report median v2-minus-v1 accounted peak beside the +16 MiB band.
Entropy candidates are not proof of incompressibility. A skipped grid/custom
driver, missing evidence or one-run speed result is not a production PASS.

## P7: parity and final binding

Run the maintainers' complete campaign with frozen engine/codegen pair and the
full corpus; any drift requires investigation. The ordinary full sweep is:

```sh
./scripts/run_corpus.sh
```

The supervisor owns this sweep; do not overlap it with any external lib/derived-
feed consumer. Retain moved=0 and raw per-probe evidence. Rebind P0 after every
fix/generated-vector commit and rerun required proofs on that exact final tree.
Maintainers alone post pineforge/verify and pineforge/parity.
