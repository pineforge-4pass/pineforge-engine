# Report sink v2 delta security read

Static read only. No executable, build, test, vector generator, jail or
performance proof was run during static review. This is a code-diff assessment,
not a deployment/security verdict.

Compared main `dad2530cd4f8e5cc8d77ac9407374a7db1be6bc4` with the selected
content of parked `46b476e2d6b4a28cb151cef353428f8a93ba37d3` and the stage-C
adaptations. Read the CLI; transcoder input/cursor/projection, staging,
verification and publication; writer Output and finalization; codec allocation
and framing; fixed-arena rank construction; CMake source selection; failure
catalog/registry; verified handoff; generator; and changed speed-driver/sampler
paths. No APP/COMPUTE jail implementation or other company's tree was read.

## Findings addressed in this diff

1. **Parked caps did not cover output.** `Input::fill()` previously folded the
   input limit into exit 5, and `Output` had no output-byte limit. It now reads
   at most one byte beyond an input cap (8). `Output::write()` checks the
   complete logical buffered extent before copying, and `reserve_footer()`
   checks its new extent before flush/ftruncate. Rewrites stay inside that
   reserved extent; repeated rank moves cannot bypass the high-water cap.
   RLIMIT_FSIZE is included in the effective bound. Byte failures propagate as
   `BlockFailure::spool_bytes` -> 9, including EFBIG and worker errors. The
   invalidation path does not truncate on a cap failure. No capped prefix is
   published. The normal initialization of an empty staging inode and
   non-cap error invalidation are separate operations.

2. **SIGXFSZ previously killed before classification.** CLI setup now requires
   successful SIGXFSZ ignore before parsing/conversion. The fsize test matrix
   covers header/data/footer/tail thresholds and exact fit. These tests are
   written, not executed evidence.

3. **File population was implicit.** `run()` reserves one file against
   `spool_max_files` before mkstemp; zero refuses with 10. All subsequent
   descriptors refer to that inode. The writer/index code has no run-file,
   SQLite, filesystem sort spool or separately growing rank generation.
   The independent Python reference encoder does use SQLite/temp files; it is
   confined to reader fault controls, and the production speed driver now
   invokes the real standalone encoder.

4. **Parked publication could overwrite a racing destination.** The old
   absent-check followed by rename had a check/use window. `run()` now uses
   no-replace link publication, then removes the staging name. An existing
   file or symlink makes publication fail, without overwriting that entry.
   Spool and destination must share a filesystem. The brief second name is a
   hard link to one inode, not an additional spool object.

5. **Parked mkstemp was closed and reopened for writing.** Staging now keeps
   its original descriptor, and Output duplicates that inode. The raw tool
   rejects a spool directory with a different owner, group/world write
   permission, or a symlink in serial mode (pathname verification). Tee C verifies
   the owner's retained staging fd; the stated threat boundary excludes other
   writers of that uid and requires trusted immutable ancestor directories.
   Sync uses the retained inode, whose final close is checked before publish.
   Ordinary failure cleanup unlinks only the owned generated path and reports
   cleanup errors/path. A failed receipt write removes its published result.

6. **Codec identity/workspace were not admission inputs.** Configure pins the
   manifest hash and every source hash. System substitution is refused.
   Compression is one-shot, dictionary-free, single-threaded, with fixed
   content/window/parameter policy. Fixed aligned contexts use the shipped
   estimator and cannot malloc/resize themselves; insufficient capacity is a
   failure. The receipt identifies source/compiler/C-flags/build policy.
   BSD-3-Clause notice retention is covered by NOTICE/LEGAL/install.
   The 64 manifest entries were hash-checked locally with shasum; this says
   nothing about their runtime behavior.

7. **Stage-A provenance could otherwise be only a calling convention.**
   Serial `pineforge_sink_encode_v2.py` binds A's counts/digest/bytes independently
   during C consumption of the retained fd (including identity/mtime). Tee drops
   “C consumes only A-verified bytes”: C ingests speculatively; only the owner can link.
   C's environment stays PATH/LANG/LC_ALL/TZ; artifacts are owner-trusted, not client attestations. The owner nonblock-drains stdout into <=2 MiB and closes/cancels on overflow. After all four children are reaped,
   require runner exit 0 with success JSON, A exit 0/complete, matching A/C input
   SHA/bytes/seven counts, and C exit 0 after fsync/checked close; then one no-replace link via retained directory fds, staging unlink and receipt.
   Owner abort closes fds/TERMs immediately; child faults drain <=2 s before terminate/reap/unlink; raw C stays a trusted stream/conformance interface. Neither receipt nor digest authenticates a client. Tee defaults to structure with 4 late threads and requires independent incremental input-record/I comparisons, descriptor commitments, every structural check and late stored-prefix/data reread; the receipt binds structure, which cannot promote a reader's whole-file verified state. `--tee-late-verify full` opts into whole-stream verification and inverse; serial and the transcoder CLI retain full.

8. **The old performance sampler saw only its direct child.** Split operation
   introduces runner, verifier, encoder and relay descendants. The sampler discovers
   the process family, accounts descendant fds/unlinked files and process RSS,
   and subtracts mapped file RSS from separately counted resident tmpfs bytes.
   Shared process pages may be conservatively counted more than once; this
   cannot hide memory. It remains sampled accounting and explicitly reports
   gaps. The default gate also binds four distinct clean same-tree builds and
   rechecks stage-C binary/archive hashes. Custom/fake drivers cannot yield a
   stage-C production PASS.

## Boundaries that remain required

- No fork, exec, network, getenv, setuid or shell call was found in the C++
  tool/source closure by static search. The Python handoff has one execve of
  the caller-pinned absolute encoder; the lab driver intentionally starts
  local verifier/encoder processes. Absence of network calls is not proof of
  a network namespace or inherited-fd policy.
- uid 10002, no network, read-only image, restricted input/output mounts,
  per-file `--fsize`, a scrubbed wrapper launch environment, limited inherited
  fds and run-owner cleanup after kill/cancellation are still hosting duties.
  The repository does not contain the deployment policy being approved.
  No hosting security requirement is waived by this read.
- A run owner's immutable source/summary/receipt binding is essential.
  fstat comparisons alone cannot defeat a malicious concurrent writer of the
  same uid. The executable path and its ancestors must also be immutable.
- Normal exceptions clean up staging; SIGKILL cannot run destructors.
  The job owner must remove the private run directory on every terminal path.
  Permission/I/O cleanup faults are diagnostic failures, not silently accepted
  successful partial reports. Test this in the real hosting jail.
- `E_as` needs a separately certified image/allocator/container/stack/loader
  reserve. The supplied function is a conditional address-space bound, charges
  buffers/Z, and does not establish whole-process or inclusive RSS. Overlapping
  owner/A/C/relay, pipe pages, <=2 MiB stdout, controls and output enter inclusive admission; the conservative output bound includes all ranks/pages and does not
  replace real inclusive admission or the hard +16 MiB / 1 MiB-slope proofs.
- F remains 1,836,032 bytes and its arena is shared/partitioned across rank directions; incremental input/decode buffers are separately charged and released before SUMMARY/F. At most four late workers have private charged buffers; join all workers and rethrow the first serial-order typed failure. Structure preserves global rank/figure proofs and cannot promote a reader's whole-file verified state.
  SUMMARY frame storage is freed before arena construction. Output bytes
  count toward resident scratch on tmpfs; D3 cannot be answered by dropping
  those bytes from memory accounting.
- Existing engine execution/settlement sources, frozen native headers and
  public runtime exports were left unchanged. The shared failure registry
  gains the four requested reason values; classification and code numbers
  stay stable. Actual zero-parity evidence is still required.

Required execution evidence: full preflight/Release verification (including
sink/transcoder rows), generated/check-reproduced vectors, sanitizers, real
uid-10002 jail caps/signals/env/network/kill cleanup, inclusive memory, four
independent builds with five timed runs per arm and zero-diff corpus parity.
No runtime or security PASS is asserted here. Structure/full equivalence, kernel memory and the final gate measurement remain required lab/security verification. One-strategy probe measurements at 1M bars versus STDOUT-mode v1 on four clean builds: structure+4 threads 1.073x, structure+1 thread 1.118x, full 1.337x (about 1.34x); kernel VmHWM transcoder 9.8 MiB and relay 3.3 MiB flat from 100k to 1M. The 1.10 limit holds at default 4 late threads in the probe; a 1-core host misses it. Replace these probe numbers with the gate box's numbers; no gate verdict is claimed.
