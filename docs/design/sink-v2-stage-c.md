# Standalone report-sink stage C

SINK-V2-171 selectively restores the round-10 codec from `parked/l22t`
(`46b476e2d6b4a28cb151cef353428f8a93ba37d3`) on main
`dad2530cd4f8e5cc8d77ac9407374a7db1be6bc4`. This document describes the
standalone adapter. The parked [wire specification](sink-v2.md) remains the
format reference; its proposed engine setters and direct `run_json` v2
integration are outside this stage.

`cmake/PineForgeSinkV2.cmake` builds `pf_sink_transcode_v2` and Unix `pf_sink_tee`, with a
separate `pineforge_sink_v2` archive. Its implementation lives in
`tools/sink_v2/`. Neither engine archive receives v2 objects or zstd, and no
engine setter, runtime export, frozen native header or execution source changes.

## Verified handoff

Serial mode (the default oracle) takes immutable same-run `sink.v1`, A's receipt,
and compact `stdout.json` from the hosting owner. Its invocation remains:

```sh
python3 -I -B docker/pineforge_sink_encode_v2.py \
  --transcoder /opt/pineforge/bin/pf_sink_transcode_v2 \
  --verified-input /run/job/sink.v1 --stage-a-receipt /run/job/stage-a.json \
  --summary /run/job/stdout.json --out /run/job/report.pfs2 \
  --spool-dir /run/job/stage-c --spool-max-bytes "$SPOOL_BOUND" --spool-max-files 1
```

The serial wrapper delegates receipt/count/digest/byte binding to C++ on its retained fd.
Tee mode drops “C consumes only A-verified bytes”: a fixed relay feeds A and C concurrently.
A uses `--out -`, positive `--max-bytes`, a non-1 receipt fd and owner-opened /dev/null stdout; no raw v1 spool.
Clients cannot supply receipt/paths/executable; the owner alone nonblock-drains runner stdout into <=2 MiB, closes/cancels on overflow, and feeds exact same-run SUMMARY after runner exit 0/success JSON; C checks folds/schema. Receipts are not signatures.

The stage-C jail still requires uid **10002**, its per-file `--fsize`,
scrubbed environment, no network, restricted mounts and inherited descriptors,
and unconditional run-directory cleanup after exit, cancellation or a kill.
The wrapper execs with only PATH, LANG, LC_ALL and TZ. It does not create
namespaces or install hosting policy. Those requirements need a real jail
proof; a standalone codec test cannot establish them.

Serial C checks `--spool-dir` ownership; tee's owner checks the publication directory and absent output.
The owner creates a 0700 private run directory and mkstemp inode on the output filesystem; no sort spool.
Tee C borrows `--staging-fd N --publish none`, verifies by fd, fsyncs/checks close and emits internal readiness.
After reaping runner, A, C and relay, commit requires runner exit 0 with compact success JSON,
A exit 0 with `complete=true`, and A SHA/bytes/seven counts equal C's ready input identity; C must exit 0.
Commit is exactly one no-replace link via retained run/publication directory fds, staging unlink, then receipt; two names briefly refer to one inode.
Owner-originated cancel closes fds/TERMs immediately; child faults get <=2 s drain, then TERM/KILL (1 s grace); reap/remove staging; hosting recovers owner kills after link.

## Caps, failures and receipt

The trusted CLI accepts sequential `--in-fd` (default 0), `--out` or paired `--staging-fd N --publish none`,
and `--summary PATH|fd:N` / `--summary-fd`, `--run-id`, `--level 1..19`, `--receipt-fd`, `--late-verify {full,structure}` (CLI default full), `--late-threads 1..4` (default 4; 1 selects serial late work); tee defaults to structure with 4 late threads, with `--tee-late-verify full` opting into whole-stream verification and inverse; serial stays full;
serial `--expect-v1-{sha256,bytes,counts}` are all-or-none (disagreement: 4). The independent caps remain:

| Option / limit | Accounted object | Exit |
|---|---|---:|
| `--max-bytes` | Complete v1 input, including header/trailer/LFs; one overflow byte | 8 |
| `--spool-max-bytes` | Output high-water extent, including buffered writes, SUMMARY, reserved footer, root and tail | 9 |
| Jail `--fsize` / RLIMIT_FSIZE | Per-file extent; encoder takes the minimum with its own byte cap | 9 |
| `--spool-max-files` | One owned output inode; reservation checked before creation | 10 |

Every breach aborts and leaves no published object or complete receipt. Caps
never clip a write or retain a capped prefix as success. SIGXFSZ is ignored
before I/O; an EFBIG remains exit 9. Footer rewrites do not count bytes twice.
Exits 0/2/3/4/5/6/7 retain success/usage/malformed/verification/I/O/no-trailer/
internal meanings.

| `report_sink_failed.reason` | Exits |
|---|---|
| `v2_input_mismatch` | 4, including verification disagreement and summary_limit |
| `v2_unrepresentable` | 2, 3, 6 |
| `v2_resource` | 5, 7, 8 |
| `v2_spool_exhausted` | 9, 10 |

The catalog retains engine-fault classification and existing numeric code IDs.
The receipt adds `closed_count`, `open_at_end_count`, `open_at_end_pnl`,
`block_count`, `scratch_flags`, and `zstd_build`. `sha256` is the **logical**
identity. `sink_bytes` is the complete v2 object length. The storage owner
computes the whole-object address; tee adds internal `ready`/`input_sha256`, never publication authority. Only structure receipts add `late_verify:"structure"`; full receipt bytes stay unchanged.

`--block-records E=N,T=N,...` is an explicit codec policy for size/conformance
probes; default invocations use the canonical caps. The transcoder runs inline, decoding each written block with the reader's checks and comparing each reformatted record against its original input fingerprint; input I order and per-type line digests must match too.
It admits only the current producer's canonical header and representable ABI
field ranges. Unsupported historical headers/ranges refuse with exit 3. Incremental disagreement is 4/v2_input_mismatch, before publication; structure keeps the incremental input-data/I proof and every structural check, replacing only late data/inverse walks while retaining HEAD/tail/root/SUMMARY, descriptors, counts/folds/digests, Merkle padding, all ranks and figures, stored-prefix reread, fsync/checked close; it never promotes a reader's whole-file verified state. One-strategy probe at 1M bars versus STDOUT-mode v1 on four clean builds: structure+4 threads 1.073x, structure+1 thread 1.118x, full 1.337x (about 1.34x); kernel VmHWM transcoder 9.8 MiB and relay 3.3 MiB flat from 100k to 1M. The 1.10 limit holds at default 4 late threads in the probe; a 1-core host misses it. Replace these probe measurements with the gate box's numbers; no gate verdict is claimed.

## Admission functions

`transcode_bounds(TranscodeShape, level, process_reserve, policy)` in
`tools/sink_v2/report_sink_v2_bounds.cpp` takes all seven certified upper counts,
descriptor widths and complete input byte bound. Counts come from stage A or
stronger compiler certificates; a stream header alone has no counts.

The spool bound includes canonical payload, per-block framing, short-page
headers, ten rank directions, figure pages, SUMMARY, root and tail. It charges
FULL indexes even when COMPACT wins. Next-fit byte/record-cap bounds limit the
descriptor population. All operations check the safe-integer domain; overflow
or missing proof refuses, and there is no `85*T` assumption.

The live byte-buffer term conservatively charges 16*R: eight builders, two writer scratch buffers, four R for live/retained decoded capacities, one R for stored/raw column buffers and one R for current-block input fingerprints (32*sum of seven record caps plus the I cap), plus
codec, metadata and finalization/I/O terms. Late admission charges every private rank scratch/decoder and every reader lane's 6R+4*65536+Z_decoder, original metadata and thread reserves, taking the maximum live phase. Validation buffers are reused and freed before SUMMARY/F; validated stored data bytes are committed at block close and compared during the late reread. Z comes
from the shipped zstd estimator. The codec uses fixed aligned contexts, which
cannot grow; allocation/codec failure aborts. `E_as` adds a **separately
certified image/allocator/container/stack/loader reserve**. It is conditional
address-space admission, not a measured process-RSS certificate. Resident tmpfs
output plus overlapping owner/A/C/relay, <=2 MiB runner stdout plus its pipe capacity, controls and <=262,144 bytes of relay slab/data pipes enter inclusive admission.

```sh
build/bin/pf_sink_transcode_v2 --bounds --level 3 \
  --input-bytes "$U" --process-reserve "$IMAGE_RESERVE" \
  --count-e "$E" --count-t "$T" --count-o "$O" --count-v "$V" \
  --count-k "$K" --count-x "$X" --count-h "$H" \
  --outputs "$HAS_OUTPUTS" --trace "$HAS_TRACE" --hashes "$HAS_HASHES" \
  --series-slots "$SLOTS" --constant-count "$CONSTANTS"
```

F remains **1,836,032 bytes**. SUMMARY frame storage is released before rank
arena construction. Rank positions are sorted/merged in their final output
slots; the ten directions share one arena, partitioned between at most two rank lanes alongside the independent figure lane; extra control/stack uses F's unused fixed-work-file allowances. No work files are used.

## Pin and verification

zstd 1.5.7 is vendored offline. Configure checks the literal SHA256 of
MANIFEST.sha256 and every listed file. System zstd is refused for this tool.
The build uses no assembly, legacy decoder, dictionary builder or zstd worker
pool. A separately compiled shared copy of that verified closure supports
standard-library-only Python conformance via `PF_SINK_ZSTD_LIBRARY`.

`tests/generate_sink_v2_vectors.py` runs stage A, the verified-only wrapper and
the real transcoder for each release vector, verifies every decoded field and
byte inverse, and compares report output with the current retained-v1 producer.
Its manifest binds actual executable/source hashes and derives the vector
count from actual cases. `--check` must MATCH; full/structure require serial/threaded/oracle byte identity on fixtures/generator cases and a split-arena merge crossing, honest incremental codec/record/I faults, unchanged full-reader negatives, real phase boundaries, kill/refusal nonpublication and kernel VmHWM; two independent late faults must retain the serial diagnostic for at least 20 threaded runs, and typed spawned-worker errors must reach the caller after joining.
`tests/generate_sink_v2_reference_controls.py` retains independent reader
fault controls; its synthetic files are not stage-C release vectors.

Main's stage A rejects null E fields. Historical null-E, unsafe ABI-range,
forced-profile and injected-frame controls remain reader tests rather than
admitted stage-C success files. The current generator uses default policy,
both canonical profiles/boundaries, ETOVKXH, nullable T, UTF-8/NUL, safe integers,
signed zero, cancellation-sensitive folds and the SUMMARY ceiling. Entropy
candidates are not claimed to prove incompressibility.

See [the lab proof recipe](sink-v2-speed-gate.md) and
[the delta security read](sink-v2-security-read.md). Builds, conformance,
generated artifacts, parity and performance remain unproven until the lab
executes them on the committed tree.
