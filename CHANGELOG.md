# Changelog

Notable changes to pineforge-engine. Earlier releases are summarized in the
README's *Releases* section and on the GitHub releases page. From 1.0.0 the
version number follows semantic versioning over the surfaces the
[public contract](docs/pages/public-contract.md) lists.

## 1.8.0 — 2026-10-27

**[1.8.0](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.8.0)**,
paired with [pineforge-codegen 1.8.0](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.8.0).
A minor release. It speeds up the `run_json` wrapper (the wrapper only, not the
engine), adds one unused storage building block to the library, states where
scratch files are placed, carries release-gate and CI changes, and stops
building and publishing macOS. The C ABI (`PF_ABI_VERSION` 4) and the
run-failure code catalog are unchanged (0 codes added, changed or removed
since 1.7.0). The report keys are unchanged except for two additive
diagnostics keys, described below.

### Compatibility and migration

- **Supported platforms: Linux x86_64 and arm64.** From 1.8.0 the engine is built, tested and published for Linux only: the release carries `pineforge-v1.8.0-linux-x86_64.tar.gz` and `pineforge-v1.8.0-linux-aarch64.tar.gz` (each with a `.sha256`) and no macOS archive, and CI no longer runs macOS jobs. A macOS build is neither tested nor supported.
- Pairs with codegen 1.8.0 exactly. Regenerate strategy C++ with codegen 1.8.0
  and relink every strategy library against the 1.8.0 headers and
  `libpineforge.a`, as for every pair change; replacing the archive alone does
  not update a statically linked strategy library.
- No public header changed. The only new header is
  `include/pineforge/detail/o1_store.hpp`, an internal building block that no
  engine path uses yet (below); its object code is part of `libpineforge.a`,
  which therefore grows slightly.
- Hosts that read the codegen input manifest must recognize the new input type
  `color` (a packed unsigned `0xAARRGGBB` integer default). That is a codegen
  change, described in the codegen 1.8.0 notes; the engine side is unchanged:
  color inputs are still stored and reported as `int64` through
  `get_input_int64`.
- The release harness file `docker/run_json.py` changed (below). The hub image
  for this pair carries the 1.8.0 copy.

### Run profile in the report diagnostics

- **Two additive success keys.** Every executed success report of
  `run_json.py` now carries `diagnostics.run_profile` (`"jailed"` or
  `"unjailed"`, the profile fixed when the run started) and
  `diagnostics.scratch_arena_jailed` (a boolean, true only when the jailed
  profile ran and the native scratch arena admission succeeded). A host that
  needs to know the run was jailed can read them instead of inferring it from
  the arena sizes. The marker records the checked profile and the admission; it
  is not an independent check of operating-system confinement. Failure,
  capability and validate-only documents, and every existing key and its order,
  are unchanged. The replay converter keeps both keys.

### Faster `run_json` report handling

- **Fewer passes over the report sink (#517).** `docker/run_json.py`
  now takes about three full passes over the report sink instead of about
  eleven. The verify pass, which already reads the whole sink for its digest,
  also folds the trade summary, the first and last equity points and the equity
  finiteness check; the annualization block gets a two-point object instead of
  five full re-reads (`point_at`); and the report writer emits flat records of
  exact scalar types with one `json.dumps`, joined in batches fixed at 512
  records and 256 KiB (a string field over 4096 characters keeps the windowed
  path). The sink reader (`pf_sink.py`) is untouched. Trades and report
  bytes are unchanged (identical under the accepted normalization of the timing
  fields in all six measured cells) and peak RSS is unchanged (72.9 MB against
  72.7 MB at 100k bars). Measured on one spot box, four repetitions per cell,
  same tag build and shared library, only the wrapper replaced:

  | bars | script | 1.6.3 | 1.7.0 | 1.8.0 |
  |---|---|---|---|---|
  | 20k | fixed ids | 0.36 s | 4.61 s | 1.77 s |
  | 100k | fixed ids | 1.35 s | 22.31 s | 8.26 s |
  | 100k | unique ids | 5.39 s | 22.99 s | 8.94 s |

  This removes the wrapper's multi-pass cost, and it is a partial recovery: the
  1.7.0 wrapper took about 4 to 17 times the 1.6.3 time on these cells (12.8x,
  16.5x and 4.3x), and 1.8.0 takes about 5 times at 20k bars with fixed ids,
  about 6 times at 100k bars with fixed ids and about 1.7 times at 100k bars
  with unique ids. The gap to 1.6.3 is **not** closed. The engine's own
  per-record sink parse and the dense-script close path are separate work, not in
  this release.

### Native live report checkpoints

The live runner now writes immutable cumulative report checkpoints at K, 2K,
4K, ... committed inputs. `--checkpoint-interval K` sets the first/base interval
(default 1024). For fixed K and bounded per-input report growth, geometric
checkpoints use linear payload storage. Report export starts at the nearest
verified checkpoint P, reading its witness snapshot and C-P tail deltas for
cursor C; the tail approaches half the history just before the next doubling.
The runner README gives the exact report-row and stored-payload bounds.

Checkpoint preparation and its separate insert transaction run after the input
commit. A failed checkpoint records a typed diagnostic and leaves that input
durable so the service can continue. An old ledger gets its first checkpoint
during open/startup recovery, before new inputs; this initial reconstruction
reads the full history. Each boundary still builds an O(T) document for a
report of size T.

Default export and restart verify the report chain only from the nearest
checkpoint onward. `report --verify-full` re-verifies the whole committed chain.
A payload or integrity witness failure falls back to full report
reconstruction; startup failures are published on stderr and in the service
status file's `checkpoint_diagnostic` field.

The checkpoint contains report state only. The engine has no state restore
entry point, so restart recovery still replays committed strategy inputs and
validates their state hashes and actions. The diagnostic measurement row prints
the lab's file, WAL, RSS, descriptor and timing values; this release does not
claim those unmeasured values in its documentation.

### Native live report stream

- **Delta JSONL is opt-in; the default stream is unchanged.**
  `pineforge-live run --report-jsonl` continues to emit one cumulative
  `pineforge-native-report/v1` document per committed input, with the same
  bytes and field order. `--report-jsonl=full` is an explicit alias with
  identical output. The new `--report-jsonl=delta` emits the ledger's
  `pineforge-native-report-delta/v1` patch with an additional top-level integer
  `input_cursor` as the first key of the line (the stored ledger row has no
  such key), beginning at the same commit point as the cumulative stream.
- **The runner no longer rescans the closed-trade prefix per input; delta
  output is linear in the appended rows.** The native `fill_report` call still
  rebuilds the engine's trade and equity/metrics report on each input; removing
  that remaining term needs an engine-side carried report accumulator and a
  separate batch-byte equivalence proof. The runner carries a closed-trade
  high-water mark in every mode. Delta output costs O(new rows) per input,
  avoiding quadratic output as history grows. Runs without a report stream
  and runs with `--report-jsonl=delta` retain only scalar state and array
  counts. Plain `--report-jsonl` and `--report-jsonl=full` retain the cumulative
  document and keep the old O(cumulative document) output cost per input,
  which can be quadratic over a growing run.

### Scratch placement (behaviour of 1.7.0 and later, restated)

- **Where scratch goes and when a run is refused.** The text is in
  [run-failure-codes](docs/pages/run-failure-codes.md) (#518) and restates the
  1.7.0 rules for hosts that place scratch on memory-backed paths. Without an
  explicit scratch directory, a native run with no scratch arena attached
  creates one unlinked command-scratch file at begin, under the nonempty
  `TMPDIR`, or under `/tmp` when `TMPDIR` is unset or empty. If that candidate
  is proven tmpfs/ramfs, the engine falls back to `/var/tmp`, and only when
  `/var/tmp` is disk-backed. If both candidates are memory-backed the run is
  refused at begin: the engine raises `scratch_io_failed`, and the default
  unjailed `run_json` profile refuses first with `run_request_invalid`;
  through the hub entrypoint the refusal surfaces as container exit code 4.
  An explicit `PINEFORGE_COMMAND_SCRATCH_DIR` on tmpfs/ramfs never falls back
  and raises `scratch_io_failed`. Example: in a container image where `/run`
  is tmpfs while `/tmp` and `/var/tmp` are on ext4, a work directory under
  `/run` is memory-backed; a run with `TMPDIR` set there falls back to
  `/var/tmp`. To choose the location, set `TMPDIR` to a disk-backed directory
  such as `/var/tmp`; `PINEFORGE_COMMAND_SCRATCH_DIR` covers the command stores
  only, not the trade ledger or the report files. This is not a change of
  behaviour in 1.8.0.

### Refused internal report sink

- **The typed failure is reported.** When the engine refused the internal
  report sink of the default route, because the scratch arena descriptor, size
  or profile flag in the environment was unusable, `run_json.py` reported
  `engine_invariant` and dropped the failure the engine had recorded. It now
  reports that failure (`report_sink_failed` with reason `arena_backing`). If
  the engine recorded nothing, the previous message is kept. Successful runs
  are unchanged.

### Storage building block

- **Fixed-shape O(1) store for Pine collections (#515).**
  `pineforge::detail` gains a chunked store (`o1_store.hpp`, `o1_store.cpp`)
  whose chunk buffer and resident pages are capped in the constructor
  (`kMaxChunkBytes` 64 KiB, `kMaxResidentPages` 1024) with a typed refusal at
  the cap, and transactions with rollback. Nothing in the engine uses it yet,
  so no run changes. It is tested in `tests/test_o1_store.cpp`, and the
  source allocation gate lists its new sites as unreviewed entries
  (no bound is claimed for them).

### Run-failure classification

- **A storage fault on the scratch that orders waiting requests is reported
  as `scratch_io_failed`.** A read, write or resize fault on the scratch that
  holds waiting requests used to end the run as `engine_invariant`; it now ends
  it as `scratch_io_failed` (a non-retryable `resource` failure), as the
  other scratch consumers already did. A typed failure the storage layer had
  already recorded keeps its own code. The catalog is unchanged: no code is
  added, changed or removed.

### Known limits

- **Memory with never-filled pending orders.** On dense scripts,
  memory still grows with the number of pending orders that never fill, by
  about 2.5 kB each. A fix is planned for a later release.

### Opt-in report descriptor in the container entrypoint

- **`PINEFORGE_REPORT_SINK_FD`.** When set, the entrypoint passes
  `--report-sink-fd <n>` to `run_json.py`, so a caller that opened a
  descriptor for the report records can use it through the container. The
  value is forwarded unchanged and `run_json.py` validates it (a number from 3
  up, not 5, different from any phase descriptor); a refusal ends the run with
  the usual entrypoint status. With the variable unset nothing changes.

### CI and release process

- **Light jobs on a pinned hosted image.** The light, secret-free CI jobs run
  on a pinned GitHub-hosted image (#514); the AGENTS.md runner
  routing is documented (#513).
- **Actions pinned to commit shas.** Every third-party GitHub Action is pinned
  to a full commit sha with a version comment, and preflight lints it
  (#508).
- **Release guard hardening.** The permission-claim text guard also catches
  backslash-wrapped, bytes-decoded and fullwidth respellings (#506); the
  release ruleset probe lists every branch ruleset and changes no gate verdict
  (#505); the promote-baseline ancestry check and the doc-reverts default base
  fully qualify their remote-tracking refs (#507).
- **Source allocation bound gate.** CI checks the engine and generated C++
  against an allocation-bound inventory with scoped site identities
  (#469). The inventory is not yet reviewed: its entries are unreviewed or
  pending flat-memory work, and the gate fails on a new container site.
- **Release gate reads the post-merge run (#526).** The release gate
  (`scripts/ci_release_gate.py`) now also requires a green, non-skipped
  `ci-post` run of the commit being released, with all Linux x86_64
  and arm64 build legs (Release and Debug) successful. Until now it
  read only the `pineforge/verify` and `pineforge/parity` statuses. When the
  release commit is documentation-only it checks the nearest earlier commit that
  is not (at most 20 documentation-only commits), and every skipped
  documentation-only commit needs a successful `ci-post` run of its own. A newer
  run of the same commit with a failed, cancelled or timed-out build job
  refuses the release; so does any other non-success result. A newer run whose build legs
  were all skipped is passed over in favour of an older full green run. Runs on
  other branches are not counted. The gate only reads results: it changes no
  build or engine behaviour, and its command line is unchanged.
- **Live equivalence row.** Corpus equivalence runs as a required CTest row in
  the live suites (#481).
- **Kernel residuals check.** `scripts/check_kernel_residuals.py` no longer
  reports a free-standing `strings` line as an unruled identifier when no
  symbol, installed header or decoded source literal corroborates it: the macOS
  arm64 Release archive prints a four-byte machine-code run through
  `strings -a`, which the check had read as an identifier. The limit is stated
  in the change: a bare archive identifier with no corroborating evidence is no
  longer detected by this check (#525, checker and its tests only, no engine
  behaviour change).
- **Scratch-on-tmpfs tests ignore co-tenant files.** The default and jailed
  scratch-on-tmpfs tests no longer fail when another job on the same CI machine
  creates files in `/tmp`, `/var/tmp` or `/dev/shm`: the tmpfs candidate is a
  private directory owned by the test, and in `/tmp` and `/var/tmp` only files
  this process creates through `mkstemp` are counted. Tests only, no change to the
  engine or its headers; the stated limit is that scratch created through
  another call in those two roots would not be seen (#521).
- **Post-runner capability canary.** A dispatch-only workflow
  (`fleet-post-capability.yml`) checks that a new post runner VM can run the
  post jobs before it joins the post pool. It has no push, pull request or
  schedule trigger, builds nothing and runs no engine suite (#520).
- **Tests and docs.** The `TIMEOUT 180` ctest property is dropped (#504); the
  run-failure diff tests hold before and after a release reset (#509); the
  doc-reverts test no longer inherits the caller's base (#516); release notes
  for earlier engine releases are backfilled (#510); the three borrowed-fd
  tests compare a descriptor's flags without Darwin's "descriptor was written"
  bit, with diagnostics that name the flag and step that changed (#519, tests
  only, no `src/` or `include/` change); the pytest-backed
  `test_run_json_stream_fastpath` row no longer carries the `fast` label, so the
  pull request release job's `fast` selection, which installs no pytest, does
  not run it (#522, tests only).

### Run-failure catalog

- `engine_invariant` may now carry `reason=order_key_hash_collision` (two order keys whose 64-bit hashes collide are refused instead of one silently replacing the other).

## 1.7.0 — 2026-10-25

This release adds report-sink transport, scratch-backed history and bounded
storage admission, with new C++ views and command-line report routes.
Regenerate strategy C++ with the matching codegen, rebuild every C++ object
that includes an engine header, and relink against the matching engine
headers and archive. Replacing the archive alone does not update a statically
linked strategy library. `get_trade()` and `closed_trade()` now return
`Trade` by value; working-book, journal and cohort accessors return views,
and some reads can now throw on scratch I/O. The C++ epoch namespace is still
`engine_script_run_v19` although the layout of `BacktestEngine` changed (its
trade list is now a `TradeLedger`), so an object compiled against 1.6.x headers
is not reliably rejected at link time: rebuild every object.

### Breaking for hosts

- **Disk-backed scratch and default directory selection (CR-150).**
  Closing a trade writes `TradeLedger` scratch even without a report sink.
  An attached scratch arena takes precedence over files. Otherwise, report
  and ledger scratch use the configured scratch directory; a path sink
  defaults that directory to its parent. With no explicit directory,
  `resolve_scratch_directory` uses nonempty `TMPDIR`, or `/tmp` if it is
  unset or empty. Only a successful probe proving that candidate is tmpfs
  or ramfs permits trying `/var/tmp`. If that fallback is also memory-backed,
  selection is refused. A failed default-directory query stops with its
  errno; it does not trigger fallback. An explicit directory proven to be
  tmpfs/ramfs is now refused (`ENODEV`), without redirecting it elsewhere. Path
  sinks also refuse non-regular files and, on Linux, tmpfs/ramfs backing, with
  `EINVAL`. The `jailed` profile is unchanged: it requires a sealed arena and
  never creates a backing file or falls back to `TMPDIR`, `/tmp` or `/var/tmp`.

- **Command scratch can refuse a run at begin.**
  `PINEFORGE_COMMAND_SCRATCH_DIR` selects an explicit command-store directory;
  unset or empty uses the default policy above. Default-directory query
  failures and memory-backed directory refusals raise
  `scratch_io_failed` (69), a non-retryable `resource` failure. The host gets
  a failed run status and an error naming the failed candidate and errno;
  the run stops before strategy callbacks and has no successful completion.
  Command creation errors retain the `command scratch create` prefix;
  selection/refusal notes add the candidate details. Report/ledger file
  I/O errors instead use `report_sink_failed`, reason `scratch_io`.

A default `/tmp` or `TMPDIR` on tmpfs/ramfs is not used. Every run without an
attached scratch arena creates its command scratch at begin, so with a
memory-backed `TMPDIR` and no usable `/var/tmp` even a run with no trades is
refused with `scratch_io_failed`. The command scratch needs a disk-backed
directory: `TMPDIR`, or `PINEFORGE_COMMAND_SCRATCH_DIR` for the command stores
only. That variable is not read for the trade ledger or the report files: a
run that closes a trade, spills or writes a report sink without an explicit
scratch directory also needs a disk-backed `TMPDIR`, or its first closed
trade fails with `report_sink_failed`, reason `scratch_io`. This includes
deployments using Docker `--tmpfs /tmp` or Kubernetes `emptyDir` with
`medium: Memory`.

### Report sink and scratch spill

- **Version-1 report sinks (CR-150).** The additive `report_sink.h` API,
  announced by `PINEFORGE_HAS_REPORT_SINK_V1` and queried through
  `pf_report_sink_version()`, writes `pf-report-sink/v1` to a path or a
  caller-selected FIFO or Unix stream socket. The stream carries equity,
  trades, output rows, events, constants, trace and broker hashes, with
  canonical binary64 spelling, UTF-8 JSON, LF framing, a 1 MiB line limit
  and a final SHA-256 receipt (the seven-count C struct
  `pf_report_sink_receipt_v1`; the trailer counts only the enabled record
  types, always `E` and `T`). `pf_report_t` remains ABI 4.
  With a sink, `trades`, `equity_curve`, `broker_state_hash`, `trace` and
  `trace_names` are NULL with zero lengths; `security_diag` is still
  populated. `total_trades` is the logical total, independent of
  `trades_len`. The library borrows the fd and never closes it; a FIFO
  host must ignore SIGPIPE to receive a typed closed-peer failure.

- **Sink summaries and lifecycle (CR-150).** Receipt, T-row summary and
  `strategy_report_sink_diagnostics_v1` annualization queries publish
  completion only with the trailer. They do not replay a report. A failed
  or aborted sink has no valid trailer. Setters refuse changes during
  execution or a stream. NULL through either setter refuses removal with
  live equity scratch or sealed trade rows, except that a completed,
  successful fd generation can detach while preserving its receipt,
  summary, scratch and borrowed descriptor. Later reads keep that
  generation's sink shape, including selected reports and envelopes.
  A refused begin does not expose the preceding generation's receipt or
  summary. Each begin checks the borrowed fd's device/inode identity;
  replacing it requires installing the descriptor again, or begin fails
  with `report_sink_failed`, reason `sink_io`.
  A stream-begin request with a sink is refused as
  `run_mode_unsupported`, feature `stream_report_sink`.

- **Native report modes and reader declarations (CR-150).** C++ clients
  gain `set_report_mode_v1`, `fill_report_envelope_v1` and the separate
  version-1 envelope. Logical counts, capture and external storage are
  distinct. FULL and SUMMARY_ONLY conflict with an installed sink;
  STREAMED requires one. Omitted sequences have `captured=0` unless the
  sink supplies them; completed counts do not count retained tails twice.
  `complete=1` requires the current run to have completed successfully,
  any required equity ratios to be ready and, for STREAMED, a complete
  sink receipt. A host overriding `present_report` must also declare its
  eligibility through `report_presenter_supports_non_full`; without an
  exact adapter it must refuse non-FULL capture.
  `PineStrategyHost` refuses SUMMARY_ONLY (`-5` without a sink, `-4` with one)
  and explicit STREAMED (`-4`); its legacy FULL sink route remains supported.
  `report_pages.hpp` declares value, page, forward/backward visitor and
  separate-text reader contracts. The trade-reader providers and engine
  accessors for those contracts are not implemented.
  These methods do not supply a C/ctypes envelope provider or a complete
  fixed-memory SUMMARY_ONLY host. The separate equity-summary helper
  requires a host-run `EquityMetricsSecondPass` and matching
  `install_equity_ratio_result()` when a second pass is needed; until then
  its four Sharpe/Sortino fields are NaN and the envelope is incomplete.

- **Scratch-backed trade history and report metrics (CR-150).**
  `TradeLedger` replaces the vector of closed trades. C++ closed-row access
  returns a value; `TradeLedger::read()`, `visit()` and `previous()` reach
  retired history. Rows and text use scratch storage; sink sealing,
  trade-stat accumulators and the three average-percent prefix sums avoid
  rebuilding retired rows for reports. Numeric report folds use
  `numeric_at()` without decoding trade text. Equity metrics can read arrays
  or scratch through a 64 KiB window and generate returns in ordered passes
  without retaining return vectors. Terminal open-position rows are
  reconstructed from retained marks and excursion facts; host excursion
  answers are retained in scratch and reused during reconstruction.
  Source trade sorting and global-exit recuts use `exit_phase_value()` and
  `set_exit_phase()` to translate global indices relative to the sealed
  trade base, preserving exit phases after older rows are sealed.

- **Selected-window sink reports (CR-150).** Selected runs can write a named
  sink, keep bounded equity tails and trade captures, and derive their
  metrics from scratch instead of copying the selected curve and all trade
  rows. Sealed rows are counted and emitted once in global trade order.
  A live `fill_report` during an open selected sink run is refused with
  `report_shape_unsupported`, shape `live_selected_sink`.

- **Streamed trade history: T records can be interleaved with E records in the
  sink file (CR-271).** Regenerated scripts that opt into streamed trade history
  (`set_trade_history_streamable`) write T records interleaved with E records in
  the sink file as trades close; the record set and every report value are
  unchanged; consumers must not rely on E-before-T order. When rows are sealed
  and published before the run ends, the byte order of the sink file can differ
  from a run of the same strategy that does not opt in, and so can the file's
  trailer `sha256`, although counts and values are equal; the last exit-time
  rows stay resident until the end, so a run with few trades can be byte-identical.
  The validation corpus C++ is regenerated with the 1.7.0 codegen
  (`89b48cb6e2fe`); its gitlink is `8425ca820c4b`, and the committed
  per-probe trade digests are unchanged.

- **Optional sequences and borrowed native reads (CR-150).** Native optional
  sequence policies can retain arrays, stream records or fold summary
  witnesses while releasing rows. Summary output-event row indices advance
  by discarded rows, including across repeated calculations. A rewritten
  pending broker-hash row replaces its old summary leaf instead of leaving
  a stale witness. Native working requests, open lots and
  request/placement/pending payloads gain callback and chunk readers.
  `WorkingRequestCore::live()` and `history()` return `LiveView` and
  `JournalView`; spilled references can expire on cache refill or the next
  command, and old-event reads can throw on I/O. `cohorts()` returns a
  logical `CohortView`, synthesizing empty allocated rosters.
  `refresh_point_allowances` and `has_waiting_children` lose their
  `noexcept` guarantee. Opening readback/cursors and `external_openings`
  let enrollment freeze membership without retaining a caller's cursor.
  Borrowed values require the source generation to remain alive and forbid
  host mutation inside visitors.

- **Spilled native working books (CR-150).** The matcher holds spilled
  rows across sibling sizing, trail observation and other book reads;
  `WorkingRequestCore::hold_live()` gives native hosts the same lifetime
  control. A spilled row stays alive with its returned shared pointer;
  a resident row is still borrowed until the next command. Reused winners
  can use their scanned table location while its generation matches,
  avoiding another book search for that lookup. Group-recipient visitors
  can read paged journal events without invalidating the walk's source
  event. Accepted definitions preserve external opening membership through
  spill; refused requests snapshot and reload their original opening lists.
  A spilled-book read failure is carried through noexcept folds and
  rethrown by continuation and broker-hash readers, rather than publishing
  a truncated hash or terminating in the fold. Journal retirement stages
  fallible reads before changing its retained window.

- **Admitted scratch-table storage (CR-150).**
  `PINEFORGE_HAS_SCRATCH_TABLES_V1` announces
  `strategy_set_scratch_tables_v1`, which selects disk scratch or a fixed
  memory slab before execution, independently of report capture. Explicit
  maxima include distinct historical keys, erased keys and generated
  successors; `K_q/B_q` are reserved zero and `V` cannot exceed 27.
  All-zero bounded maxima select the default admission policy: finite
  batches reserve `R=2*bars`, and streams without a declared lifetime
  reserve 65,536 roots. The other defaults are 8,192 erased-minima keys,
  4,096 partial-cycle keys, 4,096 cohort IDs and 27 exit-rank IDs, with
  fixed key-byte allowances of 3,145,728, 2,101,248, 1,048,576 and 6,912
  bytes respectively. Exceeding a region fails; it never grows the slab
  or falls back to disk.

- **Inherited scratch arenas and opt-in admission (CR-150, CR-260,
  CR-267).** The arena can serve report scratch, command stores, ID-pair
  tables, ordered folds, successor roots and openings.
  `PF_SCRATCH_ARENA_FD=5` and `PF_SCRATCH_ARENA_BYTES` select its backing.
  A host requiring an arena calls
  `strategy_require_report_scratch_arena_v1` on each handle before execution;
  `run_json.py --profile jailed` does this on every fresh handle.
  Missing or inadmissible backing fails with `report_sink_failed`,
  reason `arena_backing`. On Linux this admission requires a memfd with
  `F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL` and neither `F_SEAL_WRITE` nor
  `F_SEAL_FUTURE_WRITE`, independently of `PF_SCRATCH_ARENA_JAILED`;
  an ordinary file is refused. Other platforms use an attached regular-file
  arena. Unjailed hosts need not opt in. Environment attachment occurs
  through this call or installation of a report sink. Without
  `PF_SCRATCH_ARENA_FD`, the attachment helper is a no-op and
  `PF_SCRATCH_ARENA_JAILED` is not consulted; the explicit admission call
  still requires an arena. With the fd configured,
  `PF_SCRATCH_ARENA_JAILED=1` also requires sealed-memfd backing.
  `PF_SCRATCH_WINDOW_BYTES` sets the total mapping cap, default 32 MiB,
  rounded down to platform pages; invalid/sub-page values are refused.
  Each command store has a one-chunk minimum and capacity equal to the
  arena's provisional 1/16 share rounded down to 1 MiB chunks.
  Crossing that share carries `arena_exhausted`, including the
  needed-arena lower bound. `pf_report_scratch_arena_stats` exposes
  attachment-wide high-water and supplied bytes outside report/hash bytes.
  **A successful new arena attachment sets process-wide SIGXFSZ to
  `SIG_IGN`; detach does not restore it.**

- **Command scratch backing and byte budgets (CR-150, CR-221).** Source and
  native run setup provision the command, trade and dependency stores
  through the production backing. The store supplies transactional
  put/erase, rollback, stable handles, ordered readers and content/state
  digests. The reference definition refuses page sizes above 1 MiB or page
  counts above 16 Mi, key or value lengths at least 2^61 bytes and handle
  exhaustion at 2^62. Invalid transaction/durability requests are programming
  errors; a backing failure leaves a rollback-only transaction.
  `PINEFORGE_COMMAND_SCRATCH_MAX_BYTES` admits a positive decimal uint64
  budget shared by the environment-backed command stores created on the
  same thread; malformed values fail with `scratch_configuration_invalid`.
  Backing faults and budget exhaustion return store-level `io_error`;
  exit/trade command-scratch consumers surface these as `scratch_io_failed`.
  Carried arena failures retain their typed reason through rollback and
  state-hash reads.

- **Scratch I/O buffering and exit-scope cleanup (CR-257, CR-265).**
  Command backings use up to 64 KiB of read-cache page bytes per store,
  at most 192 KiB for three stores, plus metadata. Pages outside the
  supported cache geometry bypass it. Trade payloads gain a buffered tail.
  Scratch-file page-cache advice is charged by bytes rather than every
  chunk. Logical I/O observations still count cache hits.
  `kExitScopeEraseBatch=64` batches scope cleanup into transactions of at
  most 64 erases. `ExitScratchScope` retains teardown faults during
  unwinding and raises them at a throwing boundary; a cleanup failure
  cannot silently become a successful run. Its `teardown_failure` is
  run-local diagnostic state, excluded from the broker-state hash.
  These changes do not establish a bound on total writes or cleanup time.

- **Bounded resident command journals (CR-150).** The working book and older
  journal, chain and receipt populations spill to scratch. The journal's
  non-codec resident limit is 2,048 events; spill pages hold at most
  32 events and 32,768 bytes; spilling keeps at least 128 newest events
  resident. A page closes before the next event that will not fit.
  When paging is required, a single event above 32,768 serialized bytes
  fails with `report_sink_failed`, reason `scratch_io` (EFBIG), even
  without a report sink. External opening lists count toward this limit.
  Spilled-book reads reuse a resident journal definition when available.
  Admission records share one codec; the non-owning birth scan rejects
  unknown causes as `corrupt`, and filtering a retired prefix refolds
  the retained tail. Cohort and group receipts have absolute retained
  bases and lifetime counts; consumers fold receipts before retiring
  them. Group receipts below the retained base assert in Debug and throw
  `out_of_range` otherwise. Adjustment chains longer than 256 links
  keep their IDs externally; use `prior_adjustment_count` and
  `visit_prior_adjustments` to read the complete chain instead of only
  the event's inline IDs.

### run_json and CLI

- **Binary bar inputs and conversion (CR-150).** `run_json.py csv2bin
  --chart|--feed --symbol SYMBOL --tf TF IN.csv OUT.bin` writes headerless
  48-byte little-endian `pf_bar_t/v1` rows, a JSON sidecar and, for feeds,
  `.close.bin` close times. The sidecar is published last after durable
  replacement; a refusal exits 2 without publishing it. `--ohlcv-bin` and
  binary symbol feeds use mapped input and the borrowed-feed setter.
  A symbol-feed index may select the binary chart; exactly one chart source
  is required, and an index chart and explicit binary must name the same
  file. `--verify-bin full` is the default; `sidecar` skips digest comparison,
  while both modes check sizes, metadata and timestamp order.
  Selected-window charts still require CSV; binary symbol feeds are
  supported. The capability probe advertises `bars_bin_version=1` only
  with the required borrowed-feed support and
  `selected_window_bars_bin_version=0`.

- **Streaming JSON report assembly and named-sink stdout (CR-150).** With
  a complete sink-v1 API, unjailed `run_json.py` uses a temporary path sink
  by default and streams report arrays back from the strict reader instead
  of building an owning result document. `--report-sink PATH` retains the
  stream: stdout becomes the summary projection, replacing `trades` in
  place with `report_sink: {path, schema, version, run_id, counts, sha256}`
  and omitting `equity_curve`. The trade and equity arrays remain in the
  sink file. `--run-id` selects the run ID and `--spill-dir` selects the
  scratch location. The default sink directory is checked for disk backing,
  with `/var/tmp` as fallback; failure raises `run_request_invalid`,
  option `arguments`, naming `TMPDIR` and the rejected candidates.
  An explicit `--spill-dir` is passed to the engine without fallback.
  Ordinary and selected runs with a library exporting
  `pf_report_scratch_arena_stats` add
  `diagnostics.scratch_arena_high_water_bytes` and
  `diagnostics.scratch_arena_bytes`, including zeros without an attachment.
  Unless the profile is jailed or the run uses the fd route, the report JSON on
  stdout is now assembled through an unlinked temporary file in the default
  temporary directory or `--spill-dir`, also for a module without a sink API
  (1.6.3 assembled it in memory), so such a run needs writable temporary storage
  the size of the report.
  A module without any sink API keeps the legacy route unless a sink was
  explicitly requested; a partial or incompatible sink API is refused.

- **Jailed and fd CLI routes (CR-150, CR-267).** `--profile jailed`, also
  selected by `PINEFORGE_RUN_JSON_PROFILE=jailed`, installs bounded scratch
  tables and requires the inherited arena on every fresh handle.
  The default is `unjailed`. Jailed runs refuse path sinks and `--spill-dir`
  and leave the sink off unless `--report-sink-fd N` selects a stream.
  Jailed and fd stdout assembly creates no report artifact file; the
  sink-off jailed array report still occupies memory.
  The fd route emits summary/provenance JSON without reading its own sink.
  It requires `3 <= N <= INT_MAX`, reserves fd 5 for the arena, and refuses
  path/fd combinations, `--bench`, validation-only runs and a shared phase
  fd. `--report-sink-format` defaults to 1; format 2 is refused in favor
  of the external transcoder. Explicit `--report-mode FULL` keeps the legacy
  report route; SUMMARY_ONLY and STREAMED fail before input or library
  loading with catalog code `report_mode_unsupported` (65), arguments
  `mode` and `contract=pineforge-report/v1`, because this ctypes host has
  no envelope provider bridge.

- **Sink finalization and JSON conversion (CR-150, CR-289).**
  `pineforge-sink-finalize` verifies an inherited stream, counts, SHA, run ID
  and ordering before publishing a retained file, or forwards provisional
  verified lines with `--out -`. Its options include `--in-fd`, `--out`,
  `--run-id`, `--spool-dir`, `--max-bytes` and `--receipt-fd`; stdout mode
  requires a positive byte cap and a separate receipt fd.
  `pineforge-sink-to-json` reads a retained regular file with `--in` or
  `--in-fd`, presentation from `--stdout-json` or `--stdout-json-fd`, and
  requires `--out - --max-bytes B`. It verifies before emission, replays
  arrays, drops invalid presentation metadata with diagnostics and bounds
  metadata capture to 2 MiB. Valid metadata includes
  `diagnostics.annualization`, `diagnostics.phase_timing` and a null
  `calendar.session`. Finalizer version `1.1.0` writes typed-refusal fields
  to the receipt when `--receipt-fd 2` reserves stderr; with a different
  receipt fd, those fields appear in a separate stderr JSON diagnostic.
  Both tools use exit 2 for usage, 3 for malformed input or line/order
  errors, 4 for receipt verification, 5 for I/O, 6 for an incomplete sink,
  7 for an internal error and 8 for exceeding the byte cap.
  An overlong LF-terminated line is 3. Consumers must discard stdout on
  any nonzero exit and require successful completion.

- **Strategy-runner sink opt-in (CR-150).** `PINEFORGE_REPORT_SINK=1`
  requires sink-v1 and reads reports through it. Each probe prints a
  `sink_canary:` line including bytes, records, trades and equity count
  checked against the engine receipt. `PINEFORGE_KEEP_REPORT_SINK=all` or
  a probe list keeps named mode-0600 temporary sinks and prints
  `sink_kept:` paths. `PINEFORGE_CANARY_LOG` retains the sweep's canary
  lines; the corpus subset command adds a second, sink-on pass.
  `PINEFORGE_SUBSET_SINK=0` skips that second pass for a developer loop.

- **Bounded scratch in the HPO example (CR-150).** The throughput
  `grid_search_repro.py` example installs the default bounded scratch
  manifest on each fresh trial handle and checks run status before ranking
  its results. A failed trial cannot enter the ranking.

### Run-failure codes and refusals

- **Append-only run-failure numbers.** The 64 codes in v1.6.3 keep their
  numbers 1 through 64; `none` stays 0. New codes are
  `report_mode_unsupported=65`, `report_sink_failed=66`,
  `scratch_capacity_exceeded=67`, `scratch_configuration_invalid=68`
  and `scratch_io_failed=69`; `kRunFailureCodeCount` is 70.
  The C failure getters return names.

- **Typed storage failure details (CR-150, CR-267).** `report_sink_failed`
  is a non-retryable `engine_fault` and distinguishes `arena_exhausted`,
  `arena_io`, `arena_backing`, `scratch_io`, `sink_io`,
  `sink_peer_closed` and `sink_exhausted`. Exhaustion diagnostics can
  carry arena, claimed, high-water and needed bytes, store identity, or
  sink bytes and cap. File I/O failures of the native journal, chain and
  opening spill storage also raise
  `report_sink_failed`/`scratch_io`, with or without a sink; arena
  backing failures retain their arena reasons. Auxiliary ID-table,
  ID-pair and placement-file I/O use `scratch_io_failed`, and so do the
  spilled book's row and link tables and the successor-root index, which
  carry their I/O errors as auxiliary scratch errors.
  A `run_json.py` `report_sink_failed` line mirrors `reason`,
  `high_water_bytes` and `arena_bytes` from its arguments at the top
  level when present. Binary inputs extend `chart_bars_unreadable` and
  `symbol_feeds_refused` reason vocabularies.

- **SUMMARY_ONLY capture refusals (X1, N5).** Without a sink, an execution
  observer and SUMMARY_ONLY are refused together in either call order. A non-null
  observer setter returns `-2` while the v1 mode or pending legacy
  equity mode is SUMMARY_ONLY; clearing it remains allowed while quiescent.
  With an observer registered, `set_equity_report_mode(SUMMARY_ONLY)`
  returns `PF_REPORT_MODE_REPLAY_UNAVAILABLE` (`-5`) without changing
  the pending mode; `set_report_mode_v1(SUMMARY_ONLY)` also returns `-5`.
  Begin refuses a selected window or observer with either SUMMARY_ONLY
  setting and no sink, before run callbacks or bars, with
  `report_mode_unsupported` (65), arguments `mode=SUMMARY_ONLY` and
  `contract=pineforge-report/v1`. Drop the window or observer, or select
  FULL; bounded capture uses STREAMED with a sink.
  The envelope read's `-5` capture check judges only an admitted capture.
  Configuring a window for the next run does not invalidate the envelope
  of an already completed SUMMARY_ONLY generation.

- **Codec-bound execution preparation (CR-150).** Native hosts that call
  `bind_journal_codec` now get `PreparationError` with
  `CoreFailure::UnsupportedTransition` from plan-form
  `WorkingRequestCore::prepare_execution`, before state changes.
  The plan's noexcept installation cannot report codec I/O failures;
  refusing preparation avoids a later `std::terminate`.

- **Nonfinite sink equity is refused (CR-289).** The path and fd writers
  reject nonfinite E-row `equity` or `open_profit` before writing the row,
  using `report_result_invalid`, `reason=nonfinite` and
  `field=equity_curve.equity` or `equity_curve.open_profit`.
  The converter and finalizer refuse a null in either E field with the
  same code/arguments and exit 3, even with a valid receipt.
  The v1 schema keeps `f64?` for compatibility; the writer does not emit
  null E values. For multiple defects, streaming checks the earliest
  released point, equity first; completed array/metric routes check
  final `open_profit` first, so the reported field can differ.

- **Resource retries and boolean arguments (CR-150).** The `resource`
  class no longer implies retryability. Read each code's `retryable`:
  `scratch_capacity_exceeded` and `scratch_io_failed` are not retryable;
  `out_of_memory` still is. The catalog adds the argument kind `boolean`,
  used by `run_aborted.strategy_started`; `strategy_get_last_error_args`
  emits it as JSON `true` or `false`.

- **A latched `report_sink_failed` yields an empty report (CR-150).**
  Every later report read returns NULL arrays and zero counts instead of
  retrying failed storage, including native spill failures with no sink
  and a sink failure at finish time after execution succeeded. The typed
  status, code, arguments and message stay latched. Other failure codes
  retain their partial-report behavior.

- **Cooperative SIGTERM abort (CR-150).** The opt-in process-wide
  `pf_run_abort_install_sigterm_v1`, `pf_run_abort_requested_v1` and
  `pf_run_abort_restore_sigterm_v1` API lets a batch stop after its
  in-flight input bar. `run_json.py` blocks SIGTERM from startup on
  platforms with `pthread_sigmask`, until the native flag-only handler
  can take ownership; an earlier pending signal is reported before start.
  It publishes `run_aborted`, exits 1 and leaves no sink trailer.
  Arguments `bars_count` and `strategy_started` are mirrored as
  `bars.count` and `strategy_started`; the abort document carries no
  retryable field. An abort during report assembly also clears sink-owned
  sequence arrays while preserving security diagnostics and scalar fields.

### State hashes

- **Broker-state hash values of Pine source-layer runs on 1.7.0 differ from
  1.6.3; the domain tags are unchanged.** The tags stay
  `pineforge-broker-state/v19` and `pineforge-source-adapter/v4`: the recipe
  gains folded state and changes what some folds contain (the causes are
  listed below), and the placement retirement rules deliberately change
  values inside the retirement epoch. A host that stored hashes from 1.6.3 or earlier
  will see different values on 1.7.0 for every run of a Pine source-layer host
  (`PineStrategyHost`): `strategy_broker_state_hash`, the per-bar
  `pf_report_t::broker_state_hash` rows, and `strategy_stream_state_hash`,
  which folds the broker-state hash. A stored hash will not match a recomputed
  one; compare these hashes only between runs of one release. A bare native
  host folds only the constant `source:none` for the source layer, so none of
  the causes below applies to it. Apart from the last cause, the changes
  alter hash values only (the re-pin commits report unchanged trades, counts
  and curves); the last cause, CR-277, also alters trades in one measured
  configuration. Causes:
  - the trade command store: the host extension folds a presence flag and, when
    the store exists, its state digest (order id, next handle, record count and
    every live row's handle, key and value, independent of backing and cache
    placement); after a run releases the store the same presence and digest
    bytes are folded (CR-150);
  - the exit command store: the adapter folds, right after its domain, a
    presence flag and, when an exit scratch exists, the run identity, presence
    flags for its version provider, its lots callback and its working callback,
    and the exit store's presence flag and state digest (same contents as
    above) (CR-150); the exit context's temporary scope-allocation counter is
    not folded (LAB-QUIETFIX), which is not a difference from 1.6.3, which
    never folded it;
  - retired limit-only placements (O1W): a terminally filled, non-revivable
    limit is erased from the hashed placement sequence and survives only in the
    placement table's high-water, erased-row count and erased-identity digest,
    and a filled-exit placement folds an extra marker; hash values change
    wherever such a limit retires;
  - placement retirement rules (CR-150): entry and order placements retire by
    their script-bar open time instead of an input-bar index (on a chart that
    aggregates input bars, 1.6.3 kept every entry row of the run), and for the
    dormant exit legs of one origin, exit id, family and entry only the oldest
    is kept as the representative, with superseded dormant legs released (except a dormant leg that
    was not withdrawn while its origin is still open: it is kept until its position cycle ends, see
    Costs that remain); the retired rows leave the hashed
    placement sequence, so hash values change wherever such a placement
    retires;
  - settled bracket families (O1W): wholly settled families are archived out of
    the working families; when any is archived the adapter folds a versioned
    part (`pineforge-settled-bracket-families/v1`, the family count and a keyed
    digest) after the working families, and it is absent when none is
    archived;
  - `strategy.close("")` and `strategy.close_all()` keep a pending entry's real
    id as its replacement key (CR-277; it was an empty string), and the hash
    folds every pending entry's key, so a hash taken while such an entry is
    pending differs. A same-id `strategy.entry` re-issued behind such a close
    now replaces the pending entry instead of opening a second one, as
    TradingView's order-id rule requires; one measured random reversal
    configuration (seed 837832, magnifier 1, margin 0, pyramiding 1) therefore
    closes 89 trades instead of 90 (the lost trade was an exact duplicate
    short opened beside the first). Reverting only the key capture restores the
    90. TradingView equality of the exact close-then-reissue sequence is not
    proven.
  The cohort table, the consumed partial-exit cycles, the admission-event
  journal, the exit phases, the source-money fold and the scheduler's consumed
  digests were rewritten to bound memory; the source comments state that their
  bytes are unchanged. See the public contract's rule on state-hash recipes
  ([public contract](docs/pages/public-contract.md)).

### Adapter behavior

- **Adapter history retirement (CR-150).** Origin/cohort receipts, bracket
  rosters, historical IDs, successor roots and position tails use retirement,
  bounded resident windows or scratch instead of retaining all history in
  RAM, except that rows pinned for the current position cycle and one record
  per settled bracket family stay in process memory (see Costs that remain).
  Named-exit retention preserves closed-origin exit evidence while its
  position cycle still holds the side. Placement aging uses script-bar time.
  The reissue scan memoizes its first witness and scans new candidates
  incrementally; eligible terminally filled named limits retire, while
  global filled limits remain revival evidence. Settled bracket families
  are archived and restored before append or a consumed-leg probe.
  The partial-exit-cycle sorted hash fold is maintained on mutation rather
  than rebuilt at each hash read; its mutations are not constant-time.

- **Bracket cancels of members no longer working (CR-150).** When an exit
  bracket family is cancelled, members the kernel no longer works,
  including settled members, record a `NotWorking` outcome against one
  never-issued incarnation (`UINT64_MAX`) of the same run, in roster order.
  Working members keep their own handles. Recorded cancel targets therefore
  do not depend on whether non-working rows have already been retired.

- **Entry incarnation numbers can differ from 1.6.3 after a same-id re-entry
  behind a full close (CR-277).** The public C ABI value
  `strategy_closed_trade_entry_incarnation`, the C++ `Trade::entry_incarnation`
  and the `entry_incarnation` of the sink T records and the `run_json` trade
  rows can differ from 1.6.3 in scripts that reissue an entry with the
  same id behind a full close: the superseded definition is no longer
  submitted, so it no longer takes an incarnation number. Numbers stay
  non-zero, unique and monotonic; `entry_id` is unchanged. `pf_trade_t` has no
  such member and is unchanged.

- **Store-once bars and bounded calendar views (CR-150).** Native bar views,
  the chart scheduler and the magnifier share borrowed input or generate
  bounded path segments instead of copying whole feeds and completion
  arrays. `PINEFORGE_HAS_SYMBOL_FEED_BORROWED_V1` announces
  `strategy_set_symbol_feed_borrowed_v1`, which requires bars and close
  times to outlive their installation and refuses stream-begin while
  installed. N-day `request.security` partitions are generated on demand
  with a window of 16 period opens. Calendar memo and certified-cycle span
  buffers are reserved once.

- **Fixed N-day request bars across a DST change (F16/F17).** On the 1.7.0
  development line, `2D` and longer `request.security` bars could use the
  wrong trading dates when increasing feed timestamps resolved to session
  dates that went backward across a clock change. This is fixed in 1.7.0;
  v1.6.3's stored-table builder handled these dates correctly.
  `make_multi_day_period_source` now selects a calendar-year rescan for
  that case, keeping the first stamp per date, chart before auxiliary
  feed, in `kUnorderedRescanDays=366` date slots. The usual ordered path
  and its 16-open window are unchanged.

### CI and docs

- **Scratch sizing and ABI inventories (CR-150, CR-249, CR-260).** The public
  scratch-table and arena documentation distinguishes policy admission from
  a certified bound. The arena estimator accounts for the ordered fold,
  excludes openings for its stock-source profile and reserves the three
  command-store shares in its closed form.
  `scripts/emit_abi_export_list.py` emits checked runtime and independent
  native C inventories as TSV, to stdout or atomically through `--output`;
  overwriting requires `--force`.

- **CTest sharding and Debug defaults.** `scripts/ci_verify.py
  --ctest-shard I/N` accepts `1 <= I <= N <= 8`. Multi-shard runs support
  release, debug, sanitizers and kernel, with deterministic SHA-256
  name assignment and registry, selection and execution evidence for
  `ctest-shard-union/v1`; the full union is required for a verification
  verdict. Debug and sanitizers exclude the pinned `slow` rows by default.
  Release, kernel, native and runner-only live profiles have no default
  exclusion. Release is the gate for the slow rows; Debug's default is
  not full-population verification. PR CI selects only `fast` rows;
  post-merge Release, kernel and native run every registered row, while
  Debug and sanitizers retain their default `slow` exclusion.

- **Ownership manifest controls run in four CTest shards (CR-196).** The same
  loader-discovered cases keep every assertion and the 180-second timeout per
  row; a cheap guard checks complete, disjoint assignment for shard counts
  1 through 8. Preflight continues to run the complete unsharded suite. This
  was already in the v1.6.2 and v1.6.3 sources; it is first documented here.

- **Selected-window completeness (EN-115).** (Already declared by the v1.6.2 and
  v1.6.3 sources; first documented here.) The image declares selected-window
  contract revision `1.3`: the section-4 producers and manifest are present,
  `value_ownership` is `complete`, and revision `1.3` pairs only with a
  `complete` receipt. `docker/run_json.py --capabilities-json` reports
  `capabilities.selected_window_contract_revisions` as `["1.3"]` on a
  supported runtime and `[]` otherwise. A parity build declares
  `1.3-parity`; a tuple holding both is invalid. The selected-window wire
  version stays 1, the phase version 1 and the fingerprint/schema version 2.

- **Corpus regeneration tooling (CR-271).**
  `scripts/regen_corpus_cpp.sh` makes its fetched codegen checkout readable
  by the runtime image's non-root user and prints the last three stderr
  lines when transpilation fails.

### Costs that remain

- **1.7.0 is slower than 1.6.3 on some scripts.** The cause is scratch-file
  reads and writes per bar that 1.6.3 did not make. Measured on Linux machines
  of one type (16 vCPU, GCC 13.4, mean of four repetitions) over six benchmark
  scripts, the two median scripts of the parity sweep take 1.1 and 1.6 times as
  long as on 1.6.3 for compile plus run (1.1 and 3.3 times for the run phase
  alone), and the worst of the six takes 5.7 times as long (16.2 times for the
  run phase alone). The six are not a sample of all scripts, and the results
  are identical in every one of those six measured cases. The fix is planned for 1.7.1, with a
  target of at most 1.1 times the 1.6.3 run time on every script in the
  measured set. These figures were measured through `scripts/run_strategy.py`,
  which installs no report sink unless `PINEFORGE_REPORT_SINK=1` is set (it
  was not), so the 65,536-row closed-trade cache described below was in
  effect. The release image's `docker/run_json.py` default (unjailed)
  profile installs a temporary report sink on every handle, so there the
  cache holds 128 rows and a script that loops over its closed trades reads
  them from scratch files on every pass: hungpixi (24,238 closed trades),
  one run each started together on one 16 vCPU machine, took 74.18 s on
  1.6.3 and did not finish within 1800 s on 1.7.0, where the run was
  stopped at that limit. The trigger is a report sink installed together
  with a loop over more than 128 closed trades. Not affected:
  `scripts/run_strategy.py` without a sink, which the hosted case runner,
  the verifier and the sweep run (hungpixi takes 107.2 s there, mean of
  three runs; the corpus-parity canary pass sets
  `PINEFORGE_REPORT_SINK=1`), and the HPO native adapter, which installs no
  sink.

- **Script state keeps its resident layout.** No paged or external store for
  Pine script state (arrays, strings, maps, matrices, user-defined type
  instances, Series and indicator history, drawing objects) is part of this
  release. The engine's own containers for it (`series.hpp`, `matrix.hpp`,
  `generic_matrix.hpp`, `drawing.hpp` and the `ta_*` sources) are unchanged
  from v1.6.3 and stay in process memory, so a script that grows one of them
  grows resident memory with it. The paths whose resident memory is bounded
  are the trade-ledger rows, the sink writer and the command journals
  described above; the files they spill to grow with the run, and the trade
  rows are O(T) on disk in a writable scratch directory even with no sink.
  Without a report sink the equity curve stays in process memory (one point
  per script bar) and FULL reports build owning arrays. With a report sink
  installed the engine keeps only the editable tail of the curve in memory
  and writes the rest to scratch, on the Pine host's legacy FULL sink route
  as on the native route. `PineStrategyHost` cannot select `STREAMED` or
  `SUMMARY_ONLY`.

- **Pending-book reads still depend on the candidate population.** Let R be
  live handles plus staged same-bar commands, pending entries, bracket legs,
  fill-recalculation requests and shadow requests, before duplicate exit
  legs are collapsed; journal history is excluded. `projected_pending_size()`
  and `copy_v1()` now skip predecessor and sibling comparisons for non-exit
  rows. Exit rows still scan predecessors and siblings. The worst case,
  already present in v1.6.3, remains O(R²) candidate operations for size or
  a late indexed copy, and O(P R²) for a full indexed walk of P projected
  rows, reaching O(R³) when P=R. String comparison, payload reconstruction
  and backing I/O add costs.

- **Pyramiding and cohort exposure keep population-dependent work.** In the
  configured pyramiding-above-one close-ledger path, booking scans retained
  opening records for the armed incarnation, named ID and remaining
  quantity, then removes depleted rows; the held-record check sums them
  again. `cohort_exposure_for` repeatedly searches `live_units_by_origin`
  and origin positions to fold units in roster order. Its rosters and
  live-unit/origin indices depend on the position population. These paths
  do not establish a whole-run time or memory bound.

- **Full arrays and borrowed-string compatibility still cost memory.**
  FULL owning reports grow with captured sequences. Legacy C closed-trade
  string pointers materialize requested rows until the next run, and
  terminal-row pointer compatibility retains the prefix through the
  requested row. C consumers needing bounded transfer buffers use the
  report sink; C++ hosts can use `TradeLedger::read/visit/previous`.
  Their `Trade` values own text; they do not implement the declared
  separate-text or paged trade-reader contracts.

- **Spill storage and repeated passes still cost time and backing space.**
  Scratch digests walk live records and their bytes. Settled bracket
  families retain one fixed record per archived family in a resident map.
  Cold journal/book reads can reconstruct owning definitions and reload
  pages. Partially occupied ordered-index nodes are not rebalanced on erase,
  and committed journal pages stay in a reuse pool. Sink metrics and JSON
  output replay ordered records; output-column conversion performs a replay
  per column plus emission preflight. Exit-scope cleanup still seeks and
  erases each record, although commits now batch up to 64 erases.

- **A failed read of spilled storage can end the process.** Some reads that
  now come from scratch files reach callers declared `noexcept`, so an I/O
  error or an allocation failure there calls `std::terminate` where the run
  should fail with a typed code such as `scratch_io_failed`. Two paths can
  terminate; the C accessors fail silently. `NativeExecutionConsumer::first_command_after` is `noexcept` and
  bisects `WorkingRequestCore::history()`, a `JournalView` whose events past
  the 2,048-event resident limit are read back from spill pages; it is
  reached from the Pine adapter's `visit_commands_after`, from `native_events`
  and from `strategy_native_events_v1`. `WorkingRequestCore::commit(MutationPlan&)`
  is `noexcept` and writes through the journal codec that `bind_journal_codec`
  attaches, so a failed write terminates; no production code binds that codec
  on a `WorkingRequestCore` in this release, so only a host or test that does
  reaches it. The C accessors `strategy_closed_trade_entry_id`,
  `strategy_closed_trade_exit_id`, `strategy_closed_trade_exit_comment`,
  `strategy_closed_trade_close_cause` and
  `strategy_closed_trade_entry_incarnation` swallow a failed ledger read
  and answer as for an out-of-range index (NULL, -1 or 0) without setting
  `strategy_get_last_error`. Tracked as card ORG-171-IO, a
  1.7.1 priority.

- **The closed-trade cache, and the reads that still cost I/O.** The
  prefix sums described above apply to sealed rows only. The previous
  release walked an in-memory vector (`std::vector<Trade>`); the trade
  ledger keeps 64 rows resident and the rest on scratch files, with a read
  cache of closed rows in process memory in front of them. A Pine run
  allocates that cache at its first closed trade, whether or not the
  script reads trade history, because the Pine host reads each booked row.
  Which cache a run gets depends on whether a report sink is installed.
  A run with no report sink holds at most `TradeLedger::kDefaultCacheRows`
  = 65,536 rows of `TradeLedger::kLedgerEntryBytes` = 312 bytes, 20,447,232
  bytes, allocated at the smaller of that budget and the next power of two
  at or above the number of closed rows (at least 64): 19,968 bytes up to 64
  closed trades, 10,223,616 bytes at 24,238, 20,447,232 bytes above 32,768.
  A run with a report sink installed holds at most
  `TradeLedger::kSinkCacheRows` = 128 rows, 39,936 bytes, a fixed bound
  that does not depend on the number of closed trades (19,968 bytes up to
  64 closed trades, 39,936 bytes above), so that the sink run's working
  memory stays flat. `scripts/run_strategy.py` default path (no sink),
  hl2-01, 100,000 bars, n=3: peak RSS 144.9 MiB on 1.6.3 vs 125.7 MiB on
  1.7.0. A handle has no sink until its host installs one with
  `strategy_set_report_sink_v1` or `strategy_set_report_sink_fd_v1`. By
  entry point, the large cache (no sink) belongs to: the C API used
  directly, C++ hosts including the Pine host, the `runner/` command line,
  `scripts/run_strategy.py` unless `PINEFORGE_REPORT_SINK=1` is set, and
  `docker/run_json.py --profile jailed` without `--report-sink-fd`. The
  128-row cache (sink installed) belongs to: `docker/run_json.py` in its
  default, unjailed profile, which installs a temporary sink on every
  handle even when no `--report-sink` is given and is the release image's
  JSON entry point; `docker/run_json.py --profile jailed` with
  `--report-sink-fd`; `scripts/run_strategy.py` with
  `PINEFORGE_REPORT_SINK=1`; and any host that calls the two functions
  above. Which of the two the App's jailed host uses is not stated here.
  The cache stays until the next run begins or the engine is destroyed,
  and each engine holds its own, so K engines alive together hold K times
  that. A cached row serves `BacktestEngine::avg_trade_percent`,
  `avg_winning_trade_percent`, `avg_losing_trade_percent`, the
  `closed_trade_*` accessors and `get_trade` with no scratch read. Above
  the budget the cache is direct-mapped, so rows that share a slot evict
  each other and a loop over the whole history reads them from scratch
  files again on every pass: with no sink, 2 x (N - 65,536) rows per pass
  for N closed trades below 131,072, and every row from 131,072 closed
  trades on; with a sink, 2 x (N - 128) rows per pass below 256 closed
  trades, and every row from 256 on. A script that loops over its closed
  trades re-reads rows from scratch files on every pass on every
  sink-installed entry point above: the colliding rows when there are
  between 129 and 255 closed trades, and every row from 256 on; from there
  the slowdown described earlier in this section applies to it. Rows whose
  four strings together exceed 128 bytes keep their numbers cached but
  read their text from scratch files. A script that calls the accessors
  each bar then pays one scratch read per such row per call. The two
  figures cannot be changed by a user in this release:
  `NativeExecutionConsumer::begin_ready` sets the budget at every run
  begin, to 128 rows with a report sink and to 65,536 rows without, and
  installing a sink or not is the only choice. When closed rows are
  streamed (a
  report sink whose trade history is streamable), once per script bar
  `NativeExecutionConsumer::closed_rows_sealable` decodes again every row of
  the trailing block of closed rows that share the last exit time. Cohort
  lookups through `DiskResidentIdTable::find` now spill resident rows and walk
  an on-disk ordered tree, inside loops such as `cohort_exposure_for`, where
  the previous release used an `std::unordered_map`. The complexity in the
  number of rows is unchanged for closed-trade reads. A cohort lookup was
  O(1) and is now O(resident rows + log of the id population x key length)
  per call. None of the three is
  measured: this release pins no throughput bound for them. Tracked as card
  ORG-171-COSTS.

- **A refused begin can empty the report sink file.** A path report sink is
  opened with `O_TRUNC` when a run begins (`BacktestEngine::begin_report_sink`,
  reached from the consumer's begin before the dependency command scratch is
  provisioned). A begin that is refused after that point, for example because
  `PINEFORGE_COMMAND_SCRATCH_DIR` names a tmpfs, leaves the file at that path
  created and truncated to zero bytes, so a file from an earlier run at the
  same path is lost even though the new run was refused. Use a sink path
  whose old contents you can lose, or check the scratch directory before
  `begin`. Tracked as card ORG-171-SINK, a 1.7.1 priority.

- **A touched dormant leg that is re-issued keeps one placement row per
  re-issue.** A script that re-issues `strategy.exit` on a leg held dormant (a
  declined-reversal pair or pair hold) of a still open origin, after the
  leg's previous stop was touched so that the re-issue arrives as a new
  instance, keeps one `PlacementSnapshot` row per such re-issue in the
  heap-resident `PlacementTable` until the position cycle ends or the origin
  closes. This is the retention rule v1.6.3 applied to unwithdrawn dormant
  legs; the growth on this pattern is not measured. An untouched re-issue
  with the same exit id replaces its row in place and does not grow. A
  strategy that never goes flat and keeps re-issuing a touched dormant leg
  therefore grows resident memory with the number of such re-issues, and it
  is tracked as card ORG-171-DORMANT, a bounded retirement for this pattern
  with a RETAIN-ladder bound test that exercises an unwithdrawn dormant
  re-issue.

- **On macOS, do not use the descriptor-based report sink in 1.7.0.** In the
  post-merge macOS Debug build, the tests of `strategy_set_report_sink_fd_v1`,
  the C++ fd sink and the fd-sink NULL removal fail
  (`test_report_nonfinite_equity_cabi_fd`, `test_report_nonfinite_equity_cpp_fd`
  and `test_report_sink_fd`, ctest #982, #983 and #1014): the descriptor lent
  to the engine is not left unchanged (its status or descriptor flags differ
  afterwards, or it is closed); the cause is not yet diagnosed.
  `docker/run_json.py --report-sink-fd N` uses the same entry point; its own
  tests passed in that build. The path-based report sink and runs without a
  sink pass their tests in the same build. Also on macOS,
  `test_sink_finalize` (#1016), stdout mode of the 64 MiB stream, measures a
  peak-memory growth of 1.05 MiB against its 1 MiB flat bound (file mode
  passes). The Linux Debug build of the same run passes. Both are tracked
  for 1.7.1.

## 1.6.3 — 2026-10-23

**[1.6.3](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.3)**,
paired with [pineforge-codegen 1.6.3](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.6.3).
This patch release carries CI, release-gate and test-only changes after 1.6.2.
Nothing under `src/`, `include/`, `cmake/` or `runner/` changed between `v1.6.2`
and `v1.6.3`; under `docker/` only `run_failure_codes_diff.json` changed, which
is the run-failure code catalog diff published with the release.

- **CI runs on the GCP fleet.** The engine's CI runs on the GCP fleet
  (`pf-gcp-pr`, `pf-gcp-main`), fork-gated (#474). The pull-request wall is
  split from full-event verification (CR-233 A and B, squash of #477, #480).
- **A release needs full verify and parity green first.** The release job
  requires both before an engine release (C2, #479).
- **Test-only.** The live test matches delivery receipts by request sequence
  (CR-223, #478); a session tape comment no longer names a private repository
  (#476).

## 1.6.2 — 2026-10-21

**[1.6.2](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.2)**,
paired with [pineforge-codegen 1.6.2](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.6.2).
It ships the
[1.6.2-rc.1](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.2-rc.1)
candidate (2026-10-19) plus the changes merged since it. The release publishes
a run-failure code catalog (`run_failure_codes-v1.6.2.json`) and its diffs since
`v1.6.1` and since `v1.6.2-rc.1`. The `pineforge-v1.6.2-linux-x86_64.tar.gz`
archive was built by the maintainers outside CI, from tag `v1.6.2` (commit
`dfe5f367e8935ef549ee7a8a38c1d437d1e8e42e`), because a CI runner restriction
stopped the release job's Linux x86-64 leg; SHA-256
`7f66c21750982ddcb476758bcf711b9f474a66de60bb658581ba9f0e4853019f`. Regenerate
strategy C++ with the matching codegen and relink against the matching engine
headers and archive.

- **Selected-window contract revision `1.3`, with the EN-115 completeness
  proof (#412).** The engine and `docker/run_json.py --capabilities-json`
  declare `capabilities.selected_window_contract_revisions` as `["1.3"]` on a
  supported runtime and `[]` otherwise; 1.6.1 declared `1.3-parity`. The
  ownership probes were fixed so proof B reads 118/118 (CR-139, #446), and the
  ownership manifest-controls test is sharded four ways so the Debug and ASan
  runs stop timing out (CR-196, #445).
- **Proportional annualisation for runs shorter than a day, never null
  (CR-202, W-73, #456).**
- **CI.** Linux x64 jobs run on the pf-ci-spot pool (#420) and release jobs on
  pf-ci-release (#461); `timeout-minutes` is removed (#462); the actions of
  `promote-baseline.yml` are pinned by SHA (#443); native-live and the Ubuntu
  Debug leg are pool-only required pull-request checks (#464); macOS Debug is
  deferred from pull requests (D3, CR-195, #454); the pull-request tier
  excludes the slow rows on Release and kernel-only, with registration floors,
  and defers the slow verifier self-test classes (D1, CR-195, #449); only
  superseded pull-request runs of the docs workflow are cancelled (#444);
  the App-token owner follows the repository owner (#439).

## 1.6.1 — 2026-10-18

**[1.6.1](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.1)**,
paired with [pineforge-codegen 1.6.1](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.6.1).
This patch release declares selected-window contract revision `1.3-parity`
rather than `1.3` until a completeness proof exists, records per-stage
`startedAt` and `seconds` in `preflight-summary.json`, and includes the API
documentation updates since 1.6.0. It ships the
[1.6.1-rc.1](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.1-rc.1)
candidate (tag commit `196592ddba0cd8f1b3d5549c96973d1816607c0e`) plus the
CI, documentation and test-only changes merged since the candidate ([#404],
[#405], [#406]); no file under `src/`, `include/`, `cmake/`, `docker/` or
`runner/` changed after `v1.6.1-rc.1`. Regenerate strategy C++ with the
matching codegen and relink against the matching engine headers and archive;
replacing the archive alone does not update a statically linked strategy
library.

- **The selected-window contract is revision `1.3-parity` (EN-115, [#401]).** Since
  [#388], `docker/selected_window_report.py` (`CONTRACT_REVISIONS`) and the
  `docker/run_json.py --capabilities-json` probe declared
  `capabilities.selected_window_contract_revisions` as `["1.3"]` in 1.6.0.
  Under amendment 6 of the selected-window contract, revision `"1.3"` pairs
  only with `value_ownership` `"complete"`, and no completeness proof has run
  (EN-115). The probe now declares `["1.3-parity"]` when the runtime supports
  selected windows and `[]` otherwise (`value_ownership`
  `parity_with_reference`, as 1.5.0 declared): value parity with the 1.4.0
  reference on the tested cases, with the four typed value-ownership codes
  (`strategy_value_invalid`, `engine_command_value_invalid`,
  `report_result_invalid` and `report_post_execution_failed`) emitted, and no
  completeness claim. A build declaring `1.3-parity` cannot be promoted to a
  stable app pin; the app pins it only as a prerelease (`1.6.1-rc.N`), and
  only a completeness proof (EN-115) allows `"1.3"` again. The C layouts stay
  at ABI 4, `PF_ABI_VERSION` stays 4, the selected-window wire version stays
  1, the phase version 1 and the fingerprint/schema version 2.
- **Preflight stage timings ([#402]).** `scripts/ci_preflight.py` records `startedAt` (UTC ISO-8601) and `seconds` (monotonic duration; `null` while a stage is running) on each stage and on the top-level `preflight-summary.json` summary.
- **API documentation (`noindex` on Doxygen source listings and report-schema wording, [#398], [#400]).**
  `docs/seo_postprocess.py` adds `<meta name="robots" content="noindex">` in
  the `<head>` of raw Doxygen source listings whose file name matches
  `*_8<ext>_source.html` and whose stripped `<title>` ends with
  `"Source File"`, replacing any existing `robots` meta tag, and omits pages
  carrying a `noindex` or `none` robots directive from `sitemap.xml` while
  keeping `Allow: /` in `robots.txt` and leaving API reference pages,
  hand-written pages (including colliding explicit page ids), the landing
  page and `404.html` unchanged; `docs/build.sh --self-test-seo` and
  `docs/ci.md` cover the source-listing pass ([#398]).
  `docs/pages/report-schema.md` replaced "meaning complete value ownership
  with" by "covering value ownership through" for the 1.6.0 selected-window
  contract revision sentence before [#401] updated the page for `1.3-parity`
  ([#400]).
- **CI, documentation and test-only changes since `v1.6.1-rc.1` ([#404], [#405], [#406]).**
  - **Verifier tests run in parallel shards ([#404]).**
    `scripts/ci_preflight.py` runs `verifier-tests` (`scripts/test_ci_verify.py`)
    across parallel `unittest` subprocess shards with per-shard `TMPDIR`
    isolation via `scripts/run_unittest_shards.py`
    (`scripts/test_run_unittest_shards.py`).
  - **Release-candidate notes link and tag commit ([#405]).**
    The `1.6.1-rc.1` entry links its release and names its tag commit
    `196592dd`.
  - **Lower-feed timing ratio under AddressSanitizer ([#406]).**
    `tests/test_native_intrabar_lower_lookup.cpp` skips the lower-feed timing
    ratio only under AddressSanitizer; release and debug builds keep the
    `ratio < kShapeBound` check.
- **HPO.** Pair with the matching `pineforge-codegen 1.6.1` and `pineforge-hpo`
  1.6.1 (`pineforge-hpo` 0.13.2 pinned `pineforge-codegen <1.4`, and 0.14.0 added
  support for codegen 1.4 and 1.5).

## 1.6.1-rc.1 — 2026-10-17

**[1.6.1-rc.1](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.1-rc.1)**,
tag commit `196592ddba0cd8f1b3d5549c96973d1816607c0e`, paired with pineforge-codegen's release candidate.
This first candidate for 1.6.1 declares selected-window contract revision
`1.3-parity` rather than `1.3` until a completeness proof exists ([#401]),
records per-stage `startedAt` and `seconds` in `preflight-summary.json`
([#402]), and includes the API documentation updates since 1.6.0 ([#398],
[#400]). The changes below are since 1.6.0. Regenerate strategy C++ with the
matching codegen release candidate and relink against the matching engine
headers and archive; replacing the archive alone does not update a statically
linked strategy library.

- **The selected-window contract is revision `1.3-parity` (EN-115, [#401]).** Since
  [#388], `docker/selected_window_report.py` (`CONTRACT_REVISIONS`) and the
  `docker/run_json.py --capabilities-json` probe declared
  `capabilities.selected_window_contract_revisions` as `["1.3"]` in 1.6.0.
  Under amendment 6 of the selected-window contract, revision `"1.3"` pairs
  only with `value_ownership` `"complete"`, and no completeness proof has run
  (EN-115). The probe now declares `["1.3-parity"]` when the runtime supports
  selected windows and `[]` otherwise (`value_ownership`
  `parity_with_reference`, as 1.5.0 declared): value parity with the 1.4.0
  reference on the tested cases, with the four typed value-ownership codes
  (`strategy_value_invalid`, `engine_command_value_invalid`,
  `report_result_invalid` and `report_post_execution_failed`) emitted, and no
  completeness claim. A build declaring `1.3-parity` cannot be promoted to a
  stable app pin; the app pins it only as a prerelease (`1.6.1-rc.N`), and
  only a completeness proof (EN-115) allows `"1.3"` again. The C layouts stay
  at ABI 4, `PF_ABI_VERSION` stays 4, the selected-window wire version stays
  1, the phase version 1 and the fingerprint/schema version 2.
- **Preflight stage timings ([#402]).** `scripts/ci_preflight.py` records `startedAt` (UTC ISO-8601) and `seconds` (monotonic duration; `null` while a stage is running) on each stage and on the top-level `preflight-summary.json` summary.
- **API documentation (`noindex` on Doxygen source listings and report-schema wording, [#398], [#400]).**
  `docs/seo_postprocess.py` adds `<meta name="robots" content="noindex">` in
  the `<head>` of raw Doxygen source listings whose file name matches
  `*_8<ext>_source.html` and whose stripped `<title>` ends with
  `"Source File"`, replacing any existing `robots` meta tag, and omits pages
  carrying a `noindex` or `none` robots directive from `sitemap.xml` while
  keeping `Allow: /` in `robots.txt` and leaving API reference pages,
  hand-written pages (including colliding explicit page ids), the landing
  page and `404.html` unchanged; `docs/build.sh --self-test-seo` and
  `docs/ci.md` cover the source-listing pass ([#398]).
  `docs/pages/report-schema.md` replaced "meaning complete value ownership
  with" by "covering value ownership through" for the 1.6.0 selected-window
  contract revision sentence before [#401] updated the page for `1.3-parity`
  ([#400]).

[#398]: https://github.com/pineforge-4pass/pineforge-engine/pull/398
[#400]: https://github.com/pineforge-4pass/pineforge-engine/pull/400
[#401]: https://github.com/pineforge-4pass/pineforge-engine/pull/401
[#402]: https://github.com/pineforge-4pass/pineforge-engine/pull/402
[#404]: https://github.com/pineforge-4pass/pineforge-engine/pull/404
[#405]: https://github.com/pineforge-4pass/pineforge-engine/pull/405
[#406]: https://github.com/pineforge-4pass/pineforge-engine/pull/406

## 1.6.0 — 2026-10-17

**[1.6.0](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.0)**,
paired with [pineforge-codegen 1.6.0](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.6.0).
This minor release extends typed failures from source values through engine
arithmetic and report production. It ships the
[1.6.0-rc.1](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.0-rc.1)
candidate (`04607c191b9f149dd7bbac05996c58f7988aacb5`, with [#388] merged
before that tag) plus the documentation and test-only changes merged since the
candidate ([#390], [#393], [#394], [#395], [#396], [#397]); no code or
harness file under `src/`, `include/`, `cmake/`, `docker/` or `runner/`
changed after `v1.6.0-rc.1` (these notes reword `docker/README.md` only). Regenerate strategy C++ with the matching codegen and relink
against the matching engine headers and archive; replacing the archive alone
does not update a statically linked strategy library.

- **The selected-window contract is revision 1.3 ([#388]).** The
  `docker/run_json.py --capabilities-json` probe declares
  `capabilities.selected_window_contract_revisions` as `["1.3"]` when the
  runtime supports selected windows and `[]` otherwise. This revision covers
  value ownership through `strategy_value_invalid`,
  `engine_command_value_invalid`, `report_result_invalid` and
  `report_post_execution_failed`. The selected-window wire version stays 1,
  the phase version 1 and the fingerprint/schema version 2.
- **Invalid values receive typed failures ([#388]).** Source entry, exit,
  order, cancel, risk and declaration/margin paths check actionable text,
  levels and quantities through typed codes. Invalid derived sizing, margin,
  fee/commission, slippage, relative-price, risk threshold/equity and
  settlement arithmetic fails closed as `engine_command_value_invalid`.
  Native settlement guards refuse an invalid closed row.
- **Trade and summary doubles must be finite ([#384], [#388]).** In ordinary
  and selected-window runs, a non-finite trade or summary double fails with
  `report_result_invalid`, `field` naming `trades.<leaf>` or
  `summary.<leaf>` and `reason` equal to `nonfinite`. Every report writer
  enforces JSON, so the document carries no bare `NaN` or `Infinity` token.
  After an acknowledged execution boundary, a writer fault stays
  `report_post_execution_failed` with `phase: serialization`; without
  `--run-phase-fd` in ordinary mode, a non-finite value elsewhere in the report
  document fails with `report_result_invalid`, `field: report.document` and
  `reason: nonfinite`. Other encoder `ValueError`s in unphased runs, and
  encoder refusals in the capabilities and `--validate-window-only` documents,
  keep `harness_internal_error` ([#384]). Metrics, the equity curve and
  recorded outputs keep their existing handling; a non-finite metric stays
  `null`.
- **Retained refusals and diagnostic gaps ([#388]).** Native arming overflow
  of `relative_price.level` retains its typed `NonrepresentableQuantity`
  refusal, and the retained settlement row guards still use
  `engine_invariant`. The pinned signal-time rejection keeps precedence.
  An unknown `commission_type` integer still becomes `Percent`, and the
  generated legacy `strategy_set_override` export can lose a carried reason.
- **Limits of the value-ownership proof ([#388], [#396], [#397]).** Eight
  changed report tests pass on both engine main and the candidate and provide
  no fail-before evidence; they have no advance preservation/control
  declaration. They cover post-boundary `out_of_memory` as reason
  `resource`, post-boundary `engine_unclassified_error` as reason
  `exception`, a missing observer acknowledgement stopping before report
  access, and a results-digest request refused before execution, each in
  ordinary and selected-window mode. Evidence for those behaviors rests on
  passing checks, the sweep and parity.
  Of 163 mutant pairings, 160 were killed by an assertion in the paired test.
  Two were detected by errors rather than assertions and are not counted as
  kills.
  `quantity_defined_fallback_becomes_error` in
  `test_value_admission_arith`: the test process aborted.
  `06-stream-half-report-to-stdout` in
  `test_encoder_failure_after_report_chunks[exception-selected]`: the test
  body's JSON decode of the child output raised.
  The one survivor, `d6_13_slippage_evaluation_order`, was closed in this release:
  the new row `d6_13_current_slippage_bits` passes on the head and fails under
  the mutant.
  All 163 restored trees pass, and all 10 settlement pairings were killed.
  The two follow-ups planned before the 1.6.0 final to make the two
  error-detected tests fail by an assertion landed after `v1.6.0-rc.1`
  ([#396], [#397], each checked against its own deliberate bug), described
  below; the figures above are the candidate's.
  The proof does not close every
  word-composition or census gap: four mutant rows and an equivalent mutant
  remain uncovered.
- **The C API documentation uses the shared PineForge favicon ([#355]).**
  The documentation selects the canonical icon through Doxygen's
  `PROJECT_ICON` setting.
- **Tests, CI and benchmarks.** More test sources move from the public tree to
  private fixtures; rows that need them register SKIP placeholders when absent,
  and these source moves do not affect the release-built assets ([#375],
  [#386]). The rewritten public tests keep their intent, registration and
  pinned assertions ([#382]). Corpus
  updates keep the default validation sweep's scope and the baseline digests;
  corpus regeneration is pinned to codegen 1.5.0 ([#376], [#377]). Corpus
  probe directories that need non-public inputs are removed; the default
  validation sweep never included them and the baseline digests stay unchanged
  ([#376]). CI removes stage and subprocess deadlines and native-live test wait
  bounds; a stop at the platform job maximum is labelled `HANG (re-run)` and
  the check fails. Two registered rows retain a 600-second wait and a 5-second
  read bound ([#378]). The native-live service test's cleanup no longer has a
  kill fallback ([#378]).
- **Removed command-line options ([#378]).** `scripts/ci_verify.py` removes
  `--test-timeout`, `--ctest-timeout`, their config fields and the summary
  fields `testTimeoutSeconds` and `ctestTimeoutSeconds` ([#378]).
- **Benchmark comparison and quiet gate.** Benchmarks describe the closed
  strategies as held-out private strategies, not redistributed ([#385]). The
  external comparison harness is optional: without it the comparison leg is
  skipped and a run leaves committed speed results intact ([#383]). The
  external comparison harness and the 2026-09-29 load-time table leave this
  repository; the optional leg requires a supplied harness directory, and speed
  reports without its timings omit that comparison's columns and rows ([#383]).
  Without `ps`, the busy count is `unknown (ps absent)` and timing figures are
  labelled `quiet-unverified`; those figures are not performance evidence and
  do not replace committed speed results ([#387]). Without `ps`, a benchmark
  run proceeds with an unknown busy count; `QUIET_LOAD_MAX` then uses load
  average alone and remains bounded by `QUIET_WAIT_S` ([#387]).
- **Documentation and test-only changes since `v1.6.0-rc.1` ([#390], [#393], [#394], [#395], [#396], [#397]).**
  - **API documentation theme, canonical URLs, `robots.txt`, `sitemap.xml` and version label ([#390], [#394]).**
    The Doxygen footer links `https://pineforge.dev` in both footer branches,
    `docs/_theme/404.html` and `docs/_theme/robots.txt` are copied to the site
    root through `HTML_EXTRA_FILES` in `docs/Doxyfile`, and
    `docs/_theme/header.html` uses per-page title descriptions ([#390], [#394]).
    `docs/seo_postprocess.py` runs after Doxygen in `docs/build.sh` to add one
    `rel="canonical"` link per top-level page (`https://cdocs.pineforge.dev/<page>`,
    the landing page as the site root), rewrite relative `.html` page links
    (`index.html` to `./`, anchors preserved), set one meta description per
    page, and generate `sitemap.xml` from the built top-level pages excluding
    `404.html`. `docs/build.sh` sets the site version label to the tag name
    only for a clean checkout of an exact tag and to `unreleased` otherwise.
    The `.github/workflows/docs.yml` build job runs
    `bash docs/build.sh --self-test-seo` before building the site, and
    `scripts/ci_docs_only.py` (`PROOF_READS`), `scripts/test_ci_docs_only.py`
    and `docs/ci.md` list `docs/seo_postprocess.py` among the five
    documentation-build inputs excluded from docs-only CI skips ([#394]).
  - **Release-candidate notes, README apex link and 1.5.0 notes additions ([#393]).**
    Added the `1.6.0-rc.1` changelog entry, updated the `README.md` header
    link from `https://www.pineforge.dev` to `https://pineforge.dev`, and added
    *Additions to the 1.5.0 notes* in `CHANGELOG.md` for the opt-in
    selected-window mode ([#363]) and `_typed_failure` in `docker/run_json.py`.
  - **Value-failure raise sites on the run-failure-codes page ([#395]).**
    `docs/pages/run-failure-codes.md` lists fifteen of the 24 selected-window
    and report-contract codes as having a raise site in this tree:
    `strategy_value_invalid` in the C++ source layer,
    `engine_command_value_invalid` in the engine kernel, native engine and Pine
    adapter, and `report_result_invalid` in `docker/run_json.py`, alongside the
    twelve `docker/` wrapper codes, leaving nine codes with no producer in this
    tree.
  - **Assertion on the quantity-grid overflow fallback ([#396]).**
    In `tests/test_value_admission_arith.cpp`,
    `quantity_infinite_intermediate_fallback` places a cash-typed entry (`cash`
    `1e308`, `price` `2`, `grid` `0.001`) through `strategy_entry`, catches a
    thrown failure as a failed `CHECK` rather than terminating through a
    `noexcept` boundary, and asserts the admitted contracts equal `cash / price`
    bit for bit; the explicit fixed-quantity overflow check stays in
    `quantity_explicit_overflow_fallback` when the cash row records no failure.
  - **Assertion that a failed report writes one whole JSON document on stdout ([#397]).**
    In `tests/test_run_json_post_execution.py`, `assert_failure` requires the
    owned stdout to be one JSON-object line (turning a JSON decode error into
    an `AssertionError` naming the prefix and length), and
    `test_encoder_failure_after_report_chunks` asserts from the recorded fault
    prefix that no report chunk reached stdout and that `stdout.write` was
    called once.
- **HPO.** Pair with the matching `pineforge-codegen 1.6.0` and `pineforge-hpo`
  1.6.0 (`pineforge-hpo` 0.13.2 pinned `pineforge-codegen <1.4`, and 0.14.0 added
  support for codegen 1.4 and 1.5).

[#390]: https://github.com/pineforge-4pass/pineforge-engine/pull/390
[#393]: https://github.com/pineforge-4pass/pineforge-engine/pull/393
[#394]: https://github.com/pineforge-4pass/pineforge-engine/pull/394
[#395]: https://github.com/pineforge-4pass/pineforge-engine/pull/395
[#396]: https://github.com/pineforge-4pass/pineforge-engine/pull/396
[#397]: https://github.com/pineforge-4pass/pineforge-engine/pull/397

## 1.6.0-rc.1 — 2026-10-14

**[1.6.0-rc.1](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.6.0-rc.1)**,
tag commit `04607c191b9f149dd7bbac05996c58f7988aacb5`, paired with pineforge-codegen's release candidate.
This first candidate for 1.6.0 extends typed failures from source values
through engine arithmetic and report production. The changes below are since
1.5.0, with [#388] merged before the tag. Regenerate strategy C++ with the
matching codegen release candidate and relink against the matching engine
headers and archive; replacing the archive alone does not update a statically
linked strategy library.

- **The selected-window contract is revision 1.3 ([#388]).** The
  `docker/run_json.py --capabilities-json` probe declares
  `capabilities.selected_window_contract_revisions` as `["1.3"]` when the
  runtime supports selected windows and `[]` otherwise. This revision covers
  value ownership through `strategy_value_invalid`,
  `engine_command_value_invalid`, `report_result_invalid` and
  `report_post_execution_failed`. The selected-window wire version stays 1,
  the phase version 1 and the fingerprint/schema version 2.
- **Invalid values receive typed failures ([#388]).** Source entry, exit,
  order, cancel, risk and declaration/margin paths check actionable text,
  levels and quantities through typed codes. Invalid derived sizing, margin,
  fee/commission, slippage, relative-price, risk threshold/equity and
  settlement arithmetic fails closed as `engine_command_value_invalid`.
  Native settlement guards refuse an invalid closed row.
- **Trade and summary doubles must be finite ([#384], [#388]).** In ordinary
  and selected-window runs, a non-finite trade or summary double fails with
  `report_result_invalid`, `field` naming `trades.<leaf>` or
  `summary.<leaf>` and `reason` equal to `nonfinite`. Every report writer
  enforces JSON, so the document carries no bare `NaN` or `Infinity` token.
  After an acknowledged execution boundary, a writer fault stays
  `report_post_execution_failed` with `phase: serialization`; without
  `--run-phase-fd` in ordinary mode, a non-finite value elsewhere in the report
  document fails with `report_result_invalid`, `field: report.document` and
  `reason: nonfinite`. Other encoder `ValueError`s in unphased runs, and
  encoder refusals in the capabilities and `--validate-window-only` documents,
  keep `harness_internal_error` ([#384]). Metrics, the equity curve and
  recorded outputs keep their existing handling; a non-finite metric stays
  `null`.
- **Retained refusals and diagnostic gaps ([#388]).** Native arming overflow
  of `relative_price.level` retains its typed `NonrepresentableQuantity`
  refusal, and the retained settlement row guards still use
  `engine_invariant`. The pinned signal-time rejection keeps precedence.
  An unknown `commission_type` integer still becomes `Percent`, and the
  generated legacy `strategy_set_override` export can lose a carried reason.
- **Limits of the value-ownership proof ([#388]).** Eight changed report
  tests pass on both engine main and this candidate and provide no
  fail-before evidence; they have no advance preservation/control
  declaration. They cover post-boundary `out_of_memory` as reason
  `resource`, post-boundary `engine_unclassified_error` as reason
  `exception`, a missing observer acknowledgement stopping before report
  access, and a results-digest request refused before execution, each in
  ordinary and selected-window mode. Evidence for those behaviors rests on
  passing checks, the sweep and parity.
  Of 163 mutant pairings, 160 were killed by an assertion in the paired test.
  Two were detected by errors rather than assertions and are not counted as
  kills.
  `quantity_defined_fallback_becomes_error` in
  `test_value_admission_arith`: the test process aborted.
  `06-stream-half-report-to-stdout` in
  `test_encoder_failure_after_report_chunks[exception-selected]`: the test
  body's JSON decode of the child output raised.
  The one survivor, `d6_13_slippage_evaluation_order`, was closed in this release:
  the new row `d6_13_current_slippage_bits` passes on the head and fails under
  the mutant.
  All 163 restored trees pass, and all 10 settlement pairings were killed.
  Two follow-ups are planned before the 1.6.0 final to make the two
  error-detected tests fail by an assertion.
  The proof does not close every
  word-composition or census gap: four mutant rows and an equivalent mutant
  remain uncovered.
- **The C API documentation uses the shared PineForge favicon ([#355]).**
  The documentation selects the canonical icon through Doxygen's
  `PROJECT_ICON` setting.
- **Tests, CI and benchmarks.** More test sources move from the public tree to
  private fixtures; rows that need them register SKIP placeholders when absent,
  and these source moves do not affect the release-built assets ([#375],
  [#386]). The rewritten public tests keep their intent, registration and
  pinned assertions ([#382]). Corpus
  updates keep the default validation sweep's scope and the baseline digests;
  corpus regeneration is pinned to codegen 1.5.0 ([#376], [#377]). Corpus
  probe directories that need non-public inputs are removed; the default
  validation sweep never included them and the baseline digests stay unchanged
  ([#376]). CI removes stage and subprocess deadlines and native-live test wait
  bounds; a stop at the platform job maximum is labelled `HANG (re-run)` and
  the check fails. Two registered rows retain a 600-second wait and a 5-second
  read bound ([#378]). The native-live service test's cleanup no longer has a
  kill fallback ([#378]).
- **Removed command-line options ([#378]).** `scripts/ci_verify.py` removes
  `--test-timeout`, `--ctest-timeout`, their config fields and the summary
  fields `testTimeoutSeconds` and `ctestTimeoutSeconds` ([#378]).
- **Benchmark comparison and quiet gate.** Benchmarks describe the closed
  strategies as held-out private strategies, not redistributed ([#385]). The
  external comparison harness is optional: without it the comparison leg is
  skipped and a run leaves committed speed results intact ([#383]). The
  external comparison harness and the 2026-09-29 load-time table leave this
  repository; the optional leg requires a supplied harness directory, and speed
  reports without its timings omit that comparison's columns and rows ([#383]).
  Without `ps`, the busy count is `unknown (ps absent)` and timing figures are
  labelled `quiet-unverified`; those figures are not performance evidence and
  do not replace committed speed results ([#387]). Without `ps`, a benchmark
  run proceeds with an unknown busy count; `QUIET_LOAD_MAX` then uses load
  average alone and remains bounded by `QUIET_WAIT_S` ([#387]).

[#375]: https://github.com/pineforge-4pass/pineforge-engine/pull/375
[#376]: https://github.com/pineforge-4pass/pineforge-engine/pull/376
[#377]: https://github.com/pineforge-4pass/pineforge-engine/pull/377
[#355]: https://github.com/pineforge-4pass/pineforge-engine/pull/355
[#378]: https://github.com/pineforge-4pass/pineforge-engine/pull/378
[#384]: https://github.com/pineforge-4pass/pineforge-engine/pull/384
[#382]: https://github.com/pineforge-4pass/pineforge-engine/pull/382
[#385]: https://github.com/pineforge-4pass/pineforge-engine/pull/385
[#383]: https://github.com/pineforge-4pass/pineforge-engine/pull/383
[#386]: https://github.com/pineforge-4pass/pineforge-engine/pull/386
[#387]: https://github.com/pineforge-4pass/pineforge-engine/pull/387
[#388]: https://github.com/pineforge-4pass/pineforge-engine/pull/388

## 1.5.0 — 2026-10-12

**[1.5.0](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.5.0)**,
paired with [pineforge-codegen 1.5.0](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.5.0).
This minor release adds recorded outputs and the call-scoped report rule
below, and moves the engine's test inputs and strategy scripts out of this
repository. Regenerate strategy C++ with the matching codegen and relink
against the matching engine headers and archive; replacing the archive alone
does not update a statically linked strategy library.

- **Recorded outputs.** A module can record what it computes on each bar
  besides its trades: doubles in named slots per bar, doubles kept once per
  run, and events with an optional message, one row per bar the module
  publishes (`docs/outputs.md`). A generated module or a C++ host writes them
  through new protected `BacktestEngine` members (`declare_outputs`,
  `output_run_begin`, `output_bar`, `output_value`, `output_event`,
  `output_constant`); a caller reads them through nine new runtime exports of
  `pineforge.h`, group `pf_outputs` (`strategy_outputs_set_enabled`, the
  `strategy_outputs_*` readers, the size-prefixed `pf_output_event_v1_t`),
  present in every module built on this engine, and a recording module adds
  `strategy_outputs_api_version`, `strategy_outputs_manifest` and
  `strategy_signal_safety_receipt`. `docker/run_json.py --outputs` writes the
  record as the report's `outputs` block. Every refusal carries a run-failure
  code: the new `outputs_rejected` (reasons `not_declared`, `run_in_progress`,
  `recalculated_after_clear`), the new `outputs_limit` (reasons
  `too_many_events`, `too_many_rows`), `strategy_library_incompatible` with the
  new reasons `outputs_api_mismatch` and `outputs_manifest_invalid`, and
  `engine_invariant` for a broken recorder precondition (all additions to
  `docker/run_failure_codes.json`). Recording is off until a caller
  turns it on, and nothing a run computes depends on it: trades, reports,
  metrics and every hash are byte-identical, `pf_report_t` and every existing
  symbol are unchanged, and `PF_ABI_VERSION` stays 4. The new members enter
  `engine_script_run_v19` without an epoch; a C++ object compiled against an
  earlier v19 header is rebuilt with the library.
- **A run's report belongs to the call that began it ([#361]).** Once a call
  reaches the engine, its batch report belongs to that call's attempt. A call
  the engine refuses before it begins a run (a begin outside `Ready`, a refused
  bar array or run option, a calendar or timezone refusal, a begin over a live
  stream) now gets the empty report (every array `NULL`, every count 0) instead
  of the rows the handle kept from an earlier call, and
  `strategy_native_run_v1` answers it `PF_NATIVE_E_RUN_FAILED`, never
  `PF_NATIVE_OK`; the reason stays in `strategy_get_last_error`. The C entry's
  `PF_NATIVE_E_HANDLE` and `PF_NATIVE_E_ARGUMENT` checks, and native-module
  argument preflight, still return before the engine runs and leave caller
  output untouched. A run that began and then failed still publishes its
  partial report, and `strategy_stream_fill_report`, the explicit stream
  snapshot, still returns a live stream's rows, even after a batch call was
  refused over the stream. The gate is the engine's own report reader,
  `BacktestEngine::fill_report`, so the C and native-module entry points and
  the generated `run_backtest` / `run_backtest_full` wrappers all obey it, with
  no change to emitted source.
- **The test inputs and strategy scripts moved to a private repository.** The
  engine's non-public test inputs — price series, strategy scripts and the
  sources generated from them, and some test programs — are no longer in this
  repository; the maintainers keep them in a private repository. A test that
  needs one skips, naming the missing file with the pinned message
  `SKIP private fixtures not present:` followed by that file's path, so a tree
  without them still configures and runs the rest of the suite.
- **LEGAL.md, "Data not distributed".** LEGAL.md now carries a *Data not
  distributed* section — pointed to from its submodule paragraph — recording
  that some test inputs are not distributed in this repository or in its
  submodules, and that the engine does not depend on them: it builds without
  them, and the public corpus and the kernel suites run without them.
- **A private-fixture overlay for the maintainers' verification.** With the
  private inputs overlaid back into the tree — signalled by
  `PINEFORGE_PRIVATE_FIXTURES` — the verification requires every pinned row to
  run: a row that skips there fails the run. Without the overlay, those rows
  skip in public runs and the row floors account for them.
- **A new installed header, `<pineforge/native_calc_point.hpp>`.** It holds the
  plain-data vocabulary of the calculation points of a driving-event schedule:
  the driving-event kinds (`EventKind`, `Event`), the cadence bits an instance
  computes on, and the phase and finality of one planned calculation. The
  headers install with the others (CMakeLists.txt, the `install(DIRECTORY
  include/pineforge ...)` rule); it is part of the native C++ host surface.
- **HPO.** pineforge-hpo 0.13.2 pins pineforge-codegen <1.4; HPO support for codegen 1.4 and 1.5 (enum options as Enum.member tags) comes in pineforge-hpo 0.14.0.

### Additions to the 1.5.0 notes

Both changes below shipped in 1.5.0 and were missing from the notes above.

- **Selected-window backtests (opt-in).** A run can evaluate retained bars
  from before the requested window to initialize strategy state, then report
  only the activity inside the window ([#363]). The last pre-roll evaluation
  may produce an order that fills at the first bar of the window, never
  earlier. The selected report starts flat at initial capital, carries a
  separate anchor and reports retained, pre-roll and window bar counts
  separately. Request it in `docker/run_json.py` with
  `--report-policy selected-window/v1`, `--window-start-ms`, `--window-end-ms`,
  `--preroll-bars` and `--fed-start-ms`, with explicit `--input-tf` and
  `--script-tf`; `--validate-window-only` ends after validation with no
  strategy run. With the mode off, every existing output is unchanged. This
  is a development-only parity candidate: it advertises the selected-window
  contract revision `1.3-parity` only, makes no claim of complete
  value-ownership coverage, and cannot be promoted to stable.
- **Typed failures from the selected-window helpers.** In
  `docker/run_json.py`, `_typed_failure` maps exceptions with the class names
  `SelectedWindowError`, `InventoryAdmissionError`, `PlanBridgeError`,
  `ObserverBindingError` or `PhaseTransportError` to a `RunFailure` carrying
  that helper module's own stable code, message and code arguments, provided
  the code has valid syntax. Any other exception is not mapped by it.

[#361]: https://github.com/pineforge-4pass/pineforge-engine/pull/361
[#363]: https://github.com/pineforge-4pass/pineforge-engine/pull/363

## 1.4.0 — 2026-10-08

**[1.4.0](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v1.4.0)**,
paired with [pineforge-codegen 1.4.0](https://github.com/pineforge-4pass/pineforge-codegen-oss/releases/tag/v1.4.0).
The pre-tag behavior candidate paired engine `b3192bfc` with codegen
`bfc4ddce`; its version files still named 1.3.0 when the samples were built.
This minor release adds run-failure codes, typed report provenance
and the Pine adapter and live-runner corrections below. Regenerate strategy
C++ with the matching codegen and relink against the matching engine headers
and archive; replacing the archive alone does not update a statically linked
strategy library.

The C header adds `strategy_get_last_error_code` and
`strategy_get_last_error_args`, behind `PINEFORGE_HAS_RUN_FAILURE_CODES_V1`.
No existing C signature, struct layout or enumerator changes:
`PF_ABI_VERSION` stays 4 and `PF_CAPABILITIES_API_VERSION` stays 1.
The native C header and frozen header fixtures are unchanged. The script
epoch stays `engine_script_run_v19`; *State hashes* below describes the
recipe additions. *Report keys and provenance* describes the JSON changes
that consumers must handle.

The 1.4.0 facts map to baseline
`pineforge-parity-baseline-20261007-engine-b3192bfc`: its recorded grading
outcomes are **<!-- pf:releases[1.4.0].scoreboard.excellent|int -->7,983<!-- /pf -->
excellent / <!-- pf:releases[1.4.0].scoreboard.strong|int -->6<!-- /pf --> strong**
on <!-- pf:releases[1.4.0].scoreboard.graded|int -->7,989<!-- /pf --> graded probes,
with <!-- pf:releases[1.4.0].scoreboard.belowStrong|int -->0<!-- /pf --> below strong
and <!-- pf:releases[1.4.0].scoreboard.engineErrors|int -->0<!-- /pf --> engine errors.
These are the recorded population outcomes, not a claim that every internal
attempt succeeded or that every supported configuration was measured.

### Changes since 1.3.0

| Request | Change and measured scope |
|---|---|
| [#346] | Adds routing, service and retirement-cursor regression coverage in seven test/documentation files. It changes no production behavior; the final release-pair comparison grades all 7,989 probes. |
| [#349] | Reduces symbol-feed copies per strategy handle. Ownership checks preserve trades and hashes while measuring copy bounds; the comparison records no grade change on the 7,989 graded probes. This is a memory/ownership change, with no new throughput claim. |
| [#350] | Corrects the exit behaviors explained under *Pine adapter corrections* below. Recorded synthetic examples establish those fixes; the population comparison records no grade change on the 7,989 graded probes. That result does not imply unchanged trades. |
| [#351] | Adds coded failures and the checked-settings path described below. The paired engine/codegen comparison records no grade change on the 7,989 graded probes; failure checks cover empty `runtime.error` and checked-setting refusals. |
| [#352] | Corrects the same-bar location of covered forced fills, as described below. Its population comparison moves one graded probe from strong to excellent and none down. Ten zero-commission examples match fill fields; five trades retain known favorable/adverse excursion differences. The forward stream still refuses `calc_on_order_fills`. |
| [#353] | Uses chart-timeframe input in `pineforge-live` and drains the final webhook delivery. Native/live checks cover this change; the batch grades do not certify real broker fills. |
| [#354] | Separates native CI profiles and makes verification from a clean build reproducible. The final behavior candidate passed all eight required verification steps; these are verification changes, not a trading improvement. |
| [#357] | Preserves certified scalar types and explicit unresolved provenance. Its comparison records no grade change on the 7,989 graded probes. Separate pre-release checks compare input provenance in 1,523 cases and find no newly incorrect or newly unresolved values among those previously resolved uniquely and correctly. |

### Symbol-feed ownership

- **Fewer copies of another symbol's feed (memory only, [#349]).** A Pine strategy
  that reads another symbol through `request.security` held up to five copies
  of every installed feed (`strategy_set_symbol_feed`) while a run was set up
  and four for the rest of the run: the host's store, two staged
  configurations, the projected run spec and the kernel's own copies of it
  (one of them never read). A handle now holds one: a configure the kernel
  applies takes the installed bars, closes and column values into its run spec
  (moved, not copied) and the host reads them there, keeping each feed's
  symbol, timeframe and column names; a door (`strategy_set_symbol_*`,
  `strategy_set_recorded_series`) or a later run of the same handle copies
  them back first, so such a handle holds two until its next configure.
  Staging carries each feed's key and sizes only
  (`StagedConfiguration::instrument_feeds` in
  `pineforge/source/pine_adapter.hpp` is now a list of
  `StagedInstrumentFeed`), the kernel moves its spec into the run and out of
  it instead of copying it and keeps no unread copy, and installing a feed no
  longer copies it to judge it. At 56 bytes per bar, a year of one-minute bars
  is 29.4 MB per feed per copy. This ownership request alone does not change
  what a run computes: trades, reports, metrics and state hashes are
  byte-identical with and without #349, and the C ABI, the layout versions
  and the hash domains are unchanged. Other 1.4.0 changes can change trades,
  reports and hashes, as the sections below describe.

### Pine adapter corrections that can change trades ([#350], [#352])

Strategies that use these corrected paths can produce different trades
than under 1.3.0, including different fill times, prices or quantities, and
different report metrics and state-hash values. Re-run an affected strategy
before relying on its 1.3.0 results. The covered behaviors are:

- **An exit placed before any entry exists ([#350]).** Without
  `calc_on_order_fills`, a `strategy.exit` without `from_entry`, called while
  flat with no entry order working, is now treated as void for a subsequent
  market opening. Its working legs are withdrawn at that fill and do not
  bind to the new lot; 1.3.0 could let that earlier exit affect the opening.
  An exit called while an entry order is already working retains its binding
  behavior. A stop or limit entry filling first keeps the previous handling;
  that case and fill recalculation are outside this correction's evidence.
- **Competing orders during fill recalculation ([#350]).** A stop or limit
  leg of `strategy.exit` created during `calc_on_order_fills` keeps the
  trigger level installed by the exit call even when other orders compete
  beside it. The former competing-order rule shifted a level it classified
  as off-grid outward by half a tick, which could postpone a touched exit
  until a later bar or re-issue. That extra shift is removed. The recorded
  examples cover absolute and relative stop/limit legs, touched and crossed
  levels on or off the tick grid, and multiple entry/exit order combinations;
  this is not a change to every order kind or every half-tick rule.
- **Relative exits over pyramided entries ([#350]).** A held position with
  multiple `strategy.entry` lots can now receive one bracket per entry from
  a `strategy.exit` without `from_entry`, with positive relative `profit`
  and/or `loss` distances measured from each entry's fill price. Previously
  one exit was repriced from the position's average price. Each bracket
  closes that entry's quantity, while fills close the oldest lots first
  (FIFO); an entry filling after the call receives its bracket at its fill.
  This path uses the chart bars without magnifier intrabars, a positive
  tick size and FIFO, with both `calc_on_order_fills` and
  `process_orders_on_close` off. It excludes absolute limit/stop levels,
  trails, explicit quantities, partial `qty_percent` and named OCA groups.
  Other configurations retain the existing whole-position exit path. The
  retained fill-recalculation examples are outside this rule and include
  known divergences from TradingView.
- **The price-path location of forced fills ([#352]).** On an unmagnified
  chart path, a covered fill forced during `calc_on_order_fills` on the
  order's placement bar is now located at its corresponding price point on
  that bar. Previously the location test used the booked price, including
  slippage, which could put a same-bar cascade at the wrong point. Slippage
  still changes the booked price. The new location is used only while the
  forced price still exactly matches the recorded target's rounded tick plus or
  minus slippage. Magnified paths, repriced forced orders, later-bar fills
  and resting non-forced orders retain their previous location tests.
  Priced entries, `strategy.exit` legs and `strategy.order` do not record this
  target. The ten zero-commission examples match fill time, type, price,
  quantity and net PnL. Nine exercise cascades after a one-unit long margin
  call; the tenth checks an initial entry that is not admitted. Five trades
  retain known favorable/adverse excursion differences. The forward stream
  still refuses `calc_on_order_fills`.

On the measured population of 7,989 graded probes, #350 recorded no grade
change, #352 moved one probe from strong to excellent with none down, and
the later #357 comparison recorded no grade change. Those are grading
results on that population, not cross-version trade or hash identity.

### Run-failure codes ([#351])

- **Every run failure carries a stable code.** `<pineforge/pineforge.h>`
  declares `strategy_get_last_error_code()` and
  `strategy_get_last_error_args()` beside `strategy_get_last_error()`, behind
  the probe `PINEFORGE_HAS_RUN_FAILURE_CODES_V1`. The code is lower snake_case
  from a closed vocabulary, `""` exactly when the most recent run or setter
  recorded no failure; the arguments are one canonical JSON object (`"{}"`
  without arguments). The English of every existing engine failure is
  unchanged byte for byte. A code is chosen where the failure is raised and never read from
  the text, so a script's `runtime.error` always reads
  `strategy_runtime_error`, whatever English it copies; an argument is never a
  value the script computed while it ran. Both getters are additive runtime
  exports that every strategy library links, so `PF_ABI_VERSION` stays 4; a
  harness looks them up with `dlsym` (Python: `hasattr`), and a library built
  against an earlier runtime lacks them.
  [Run-failure codes](docs/pages/run-failure-codes.md) lists every code, its
  class, its retry flag and its arguments, and the rules.
- **The vocabulary is published, and each release says what changed.** The
  catalog is `docker/run_failure_codes.json` (schema
  `pineforge-run-failure-catalog/v1`); the engine's `RunFailureCode` enum and
  its registry are generated from it. A code is never removed, renamed or
  reused: one that is retired stays, deprecated, with its replacement. Each
  GitHub release attaches the catalog as `run_failure_codes-<tag>.json` and
  its changes since the release its notes start at as
  `run_failure_codes_diff-<tag>.json` (schema
  `pineforge-run-failure-catalog-diff/v1`): for a final release that is the
  previous final release, so a final release that follows release candidates
  lists everything they added. Such a final release also attaches the step
  from its last candidate as `run_failure_codes_diff-<tag>-from-<candidate
  tag>.json`, so the diffs chain through every release. From them a
  localizing consumer learns which codes, arguments and values to translate.
- **Generated code names its own stops.** `<pineforge/run_failure.hpp>` adds
  the helpers a strategy generated by a matching pineforge-codegen calls for
  its stops (`pine_no_data_stop`, `pine_other_symbol_stop`, `pine_array_stop`,
  `pine_collection_stop`, `pine_na_stop`, `pine_limit_stop`,
  `pine_unsupported_stop`, `pine_string_stop`, `pine_engine_invariant`), each
  with the English it prints today, and `pine_runtime_error` now throws a
  coded `std::runtime_error`. Until a codegen that calls them pairs with
  this engine, a generated array, string or request stop still reads
  `strategy_runtime_error` (or, for a C++ library refusal such as an
  out-of-range `str.substring`, the type's safety net). The runtime library's
  own refusals carry their codes already: the checks of `matrix<int>`,
  `matrix<string>` and `matrix<bool>`, maps, drawings and built-in arguments,
  so a script's `matrix.add_col` on an empty `matrix<int>` reads
  `pine_matrix_error`, not an engine fault, whatever C++ type is thrown
  (`matrix<float>` still checks no index).
- **A failed run no longer reads as a success.** A script stopped by
  `runtime.error()` with an empty message left an empty
  `strategy_get_last_error()`, and `docker/run_json.py`, which tested the text
  alone, printed a success report of the bars it had reached. The run now
  reports the code `strategy_runtime_error`, and `run_json.py` fails a run
  when the code or the text is non-empty or `strategy_last_run_status()` is 1.
  Five execution-consumer handlers that recorded a failure without any text
  now write one (`native settlement exception`, `native current execution
  exception`, `native recalculation callback exception`, `native sub-bar
  callback exception`, `native applied callback exception`) when the run
  reports no failure yet and was not aborted. A C setter that returns -1
  because it threw now leaves that exception's text and code (`unknown C++
  exception` for a non-standard one) instead of dropping its reason, unless
  the handle's run already failed, whose text and code it keeps; and a legacy
  input or override setter failure the generated strategy latched surfaces at
  the run's begin as `setting_rejected`. `scripts/run_strategy.py`,
  `scripts/run_stream_corpus.py` and the tutorial runners fail a run by the
  same rule as `run_json.py`.
- **`docker/run_json.py`'s error line carries the code.** With a library that
  exports the getters, the line is
  `{"engine":"pineforge","error":"<text>","code":"<code>","args":{...}}`;
  with an earlier library it is the line it was. Its own refusals and its
  former tracebacks carry codes of their own, and a strategy whose
  `strategy_create` returns no handle fails with `strategy_create_failed`
  before any setter runs, where the harness used to crash.
  `docker/README.md` gives the line, its caps and the failure rule.
- **`docker/run_json.py` refuses a setting the strategy cannot take.** With a
  strategy library that exports the checked settings API
  (`strategy_settings_api_version() == 1`), the harness creates and configures
  the strategy through it: an unknown input title or override key, an option
  outside its list, or a value that does not parse or is out of range fails the
  run before it starts (`setting_rejected`). Through the legacy setters,
  1.3.0 accepted some settings, ignored others and already failed some with
  text-only errors, as the executed contrasts below show. A
  library without that API keeps the legacy setters; one that exports only
  part of it, or another version, is refused
  (`strategy_library_incompatible`). A `true`, `false`, `null`, array or
  object in `--inputs` or `--overrides` is refused (`run_request_invalid`)
  where it used to reach the setter as `True` or `None`; a number is still
  passed as Python spells it.
- **Native modules record the code too.** `pineforge/native_module.hpp`'s
  `note_error` takes the code beside the text; the text-only form it had
  stays and records `engine_unclassified_error`.

#### Executed setting and self-stop contrasts

Six synthetic requests were run through the installed entrypoint on
the exact v1.3.0 pair and the pinned candidate pair, using the same 40-bar
feed. 1.4.0 refuses three requests that 1.3.0 allowed to complete:
setting a duplicated `Period` title to `"7"`, passing `"color.blue"` to
`Color`, and setting `Choice` to `"15"` outside its `[10, 20, 30]` options.
The old pair returns success reports for these requests; that wire echo
alone does not prove which native values were applied rather than ignored.

| Request | v1.3.0 result | Candidate result |
|---|---|---|
| `Period="7"` with two declarations sharing the title | Success report, exit 0 | `setting_rejected`, reason `ambiguous_key`, exit 4 |
| `Color="color.blue"` | Success report, exit 0 | `setting_rejected`, reason `expected_integer`, exit 4 |
| `Choice="15"`, options `[10, 20, 30]` | Success report, exit 0 | `setting_rejected`, reason `invalid_input_option`, exit 4 |
| `initial_capital="abc"` | Text-only error `strategy_set_override: stod`, exit 4 | `setting_rejected`, reason `expected_finite_decimal`, exit 4 |
| `initial_capital="-1"` | Text-only error `Pine adapter produced invalid native run spec field 15`, exit 4 | `setting_rejected`, reason `value_below_minimum`, exit 4 |
| `runtime.error("REL140 deliberate stop")` at `bar_index == 10` | Text-only error `REL140 deliberate stop`, exit 4 | Same text, `strategy_runtime_error`, `args: {}`, exit 4 |

These are entrypoint exits, not direct harness exits. Both invalid-capital
requests already failed in 1.3.0; the candidate reports them as coded setting
refusals before execution. The candidate's 38-code
[catalog](docker/run_failure_codes.json) classifies all five
`setting_rejected` results as **`input`**, and the deliberate script stop as
**`strategy`**. Consumers can distinguish request refusal from a script's
self-stop using the code and catalog class; exit 4 alone does not make that
distinction. These six requests illustrate the change; they are not the
complete list of refusals.

**Known diagnostic limit:** a `setting_rejected` refusal of a strategy
override carries `args.entrypoint: "strategy_set_override"` and
`args.reason`, but no override key. Its text does not name the key either:
the non-numeric capital example reads
`strategy_set_override: expected a finite decimal number`. `args.input`
names an input only when the strategy declares that title, so an unknown
input title is not named. `setting_unsupported` carries `args: {}`. The
harness stops at the first refused setting, processing inputs before
overrides. A host sending several overrides cannot identify the refused
key from that failure alone.

### Live runner: chart-timeframe input ([#353])

- **`pineforge-live` runs a Pine script on its chart clock.** `--input-tf`
  defaults to `--script-tf`, and the warmup, the confirmed-bar feed and the
  tick time boundaries are chart bars on the configured session calendar,
  each labelled at the session-aware opening of its slot. In bars mode the run
  equals the chart-timeframe batch (`run_backtest_full` with the ENDPOINTS
  magnifier distribution) on every report field and physical action; in ticks
  mode it equals the engine fed the same prints. Until now a coarser script
  was fed one-minute bars, a bracket born at a script open was held to that
  bar's close, and the runner's report differed from the batch's. A script
  and input of `1`, and a hand-written `--native-config` strategy, keep the
  handling they had. The C ABI and the report layout are unchanged.
- **Redeploy a deployment that ran a coarser script on one-minute input.** An
  explicit `--input-tf 1` with a coarser script is refused before a ledger
  exists, and a ledger an earlier runner wrote for that shape is refused on
  resume without modifying ledger database content in the read-only legacy
  identity check. This does not promise unchanged SQLite sidecar files.
  Stop the old deployment, resolve or
  redeliver its unsent actions, reconcile its open position with the receiver
  or broker (the runner never closes or migrates one), keep the old ledger for
  audit, then start a new ledger with chart-timeframe warmup and feed and no
  `--input-tf`. The runner README's *Upgrading from 1-minute input* has the
  steps.
- **Some calendars are refused until calendar-aware chart labels ship.**
  Daily, weekly and multi-day charts are refused in both modes on a calendar
  with daylight-saving transitions from warmup onward; that detection has no
  three-year bound. Standard-offset changes are checked by the tiling scan
  only within the warmup plus 1,098 days after its last bar (about three years).
  Multi-day charts are refused on a weekday-masked calendar when eligible
  opening-label gaps do not tile the chart period, as with `2D` and `3D` on
  UTC `24x7:23456`; this is not a blanket `nD` refusal (`7D` can tile). In bars mode an intraday
  period that does not tile the session's trading days is refused too: 7
  minutes on UTC 24x7; 45, 120 and 240 minutes on New York calendars (60
  works); and a split session whose reopen is off the chart grid (60 minutes
  on `0930-1130,1300-1500`; 30 works). Some split-session charts are refused
  conservatively. The tiling scan stays anchored at the original warmup end
  on restart. Startup names the refusal reason and creates no new ledger.
- **A run could exit 0 with one webhook still pending; fixed.** The delivery
  worker decided that nothing was left from a ledger scan taken before it
  read the end-of-input request, so an action committed between the two
  stayed unsent until the next start (`webhooks_pending` 1, no error). It
  reads the request first now.

### State hashes

The broker-state and source-adapter domains remain
`pineforge-broker-state/v19` and `pineforge-source-adapter/v4`; the stream
fingerprint version remains 19 and the native consumer identity remains
`native-consumer/v9`. The source adapter adds conditional folds for state
used by the exit and waypoint fixes:

- [#350] folds `void_global_issue` when set; a per-entry placement's
  `per_entry_origin` and `per_entry_units` when its origin is nonzero; and a
  nonempty per-entry exit book under `pineforge-source-per-entry-exits/v1`.
  That book includes each exit's id, cycle, side, profit/loss ticks, comment
  and children, and each child's origin, fill price, units and consumed bit.
- [#352] folds a non-NaN `forced_waypoint_print` and, during fill
  recalculation, a non-NaN `coof_fill_target_print_`, each with its own marker.
- [#349] substitutes stored feed bar/column counts for the same vector sizes,
  with the same unsigned encoding and order; no hash input is removed by
  that ownership change.

Across #350 and #352, **29 added fold lines (23 + 6)** observe state.
Adding those observations does not itself change trading behavior or
non-hash report values. The accompanying adapter corrections can change
trades, reports and the values being hashed on the affected paths.
No existing hashed field is removed or re-encoded in this range. The
unchanged enclosing epoch/domain names are not a promise of equal 1.3.0 and
1.4.0 hash values. The release harness's
SHA-256 provenance fingerprint is a separate identity, described next.

### Report keys and provenance

Successful reports keep their top-level trade, summary, diagnostics and
metrics keys, including `sharpe_tv` and `sortino_tv`. Top-level
`applied_inputs` and `applied_overrides` remain setter-wire strings.
Inside `fingerprint.provenance`, [#357] changes certified input defaults,
effective values and applied settings to their native scalar types: an
integer input value such as Length `"21"` is represented as `21`, a decimal
strategy override such as `initial_capital="12000"` as `12000.0`, a boolean
as a JSON boolean, and a string or source selector as a string. Consumers
must accept these typed values instead of assuming every applied value is a string.

A value that cannot be certified remains explicit: the input's `default`
and `value` are `null`, with a `resolution` object carrying
`status: "unresolved"`, a `reason` and `raw_default`. Its applied wire value
is retained. On the legacy-settings path, a native receipt with several
distinct `(type, default)` pairs for one title produces
`reason: "duplicate_title"` and the integer
`distinct_native_inputs` count; repeated identical pairs are not counted as
distinct inputs. Refused strategy defaults are `null` in `strategy`, with
their records under the optional `strategy_resolution` key. Unresolved
content normally remains in the fingerprint; `null` is not a certified
native value.

Only Pine transpiled during the invocation establishes producer origin.
Supplied C++ needs a supported native receipt agreeing on the value, or its
row is `foreign_unverified_source`; such a receipt is self-attested by the
supplied module. The resolver does not evaluate arbitrary C++ expressions.
The canonical encoder and feed canonicalization are unchanged, but changed
types, values, reasons/counts, generated source and build identities change
the token and digest. Existing out-of-domain refusals can still make the
whole fingerprint `null`.
The checked-settings path also makes the whole fingerprint `null` when its
receipt repeats a title; it does not emit the legacy path's per-title
duplicate record. A successful backtest can therefore have no fingerprint.
For the demonstrated repeated-title script, giving the two inputs distinct
titles is the source-derived remedy and is expected to restore the
checked-path fingerprint; the retained sample did not execute that renamed
variant. This differs from an unresolved legacy row, which can remain
inside a non-null fingerprint.

Failure objects gain string `code` and typed-object `args` beside `engine`
and `error` ([#351]); legacy libraries without the getters retain the earlier
text-only form. Empty `runtime.error` now fails, and checked-settings
refusals stop the run before execution. See the [release harness schema and
source-trust rules](docker/README.md#backtest-fingerprint) and
[run-failure vocabulary](docs/pages/run-failure-codes.md).
The [public before/after sample](docs/pages/release-1.4.0-sample.md) retains
actual 1.3.0-pair and candidate-pair observations, including the checked
path's null fingerprint and the legacy path's per-title records.

### Migration

Use engine 1.4.0 with codegen 1.4.0; earlier paired
releases remain historical examples. Regenerate and rebuild every generated
strategy and every C++ host against the new headers and archive. C loaders
can probe the two new getters before using them with older libraries.
Update JSON readers for typed/unresolved provenance and coded failures;
decode and hash the original fingerprint token bytes to verify a digest.
For inputs sent to the checked-settings API, use unique declared titles,
send colors as decimal integers for packed `0xAARRGGBB` values (opaque red
is `"4294901760"`, not a CSS/hex string or `"color.red"`), and choose values
from each input's declared `options`. A shared title is refused as
`ambiguous_key`; rename one input in the Pine source. Textual colors are
refused with `setting_rejected` and an integer-parse reason: the measured
`"color.blue"` request reports `expected_integer`; source inspection gives
`invalid_integer_or_trailing_bytes` for `"#FF0000"`, `"0xFF0000"` and `"black"`,
which were not run. The measured out-of-options `Choice` request reports
`invalid_input_option`. These are input settings in `--inputs`, separate
from `strategy()` parameters in `--overrides`. Unknown input titles or
override keys and declared numeric range violations are also refused;
see [checked settings](docs/checked-settings.md) and the
[harness setting rules](docker/README.md#the-failure-line) for the remaining
refusals and supported encodings.
For a live deployment that fed a coarser script one-minute input, reconcile
pending deliveries and positions, retain its ledger for audit, and start a
new deployment with chart-timeframe warmup/feed as described above and in
the [runner upgrade guide](runner/README.md#upgrading-from-1-minute-input).

[#346]: https://github.com/pineforge-4pass/pineforge-engine/pull/346
[#349]: https://github.com/pineforge-4pass/pineforge-engine/pull/349
[#350]: https://github.com/pineforge-4pass/pineforge-engine/pull/350
[#351]: https://github.com/pineforge-4pass/pineforge-engine/pull/351
[#352]: https://github.com/pineforge-4pass/pineforge-engine/pull/352
[#353]: https://github.com/pineforge-4pass/pineforge-engine/pull/353
[#354]: https://github.com/pineforge-4pass/pineforge-engine/pull/354
[#357]: https://github.com/pineforge-4pass/pineforge-engine/pull/357

## 1.3.0 — 2026-10-06

A minor release: `pineforge-live` admits the confirmed-bar request shapes and
the standalone close-only `varip` that a strategy generated by
pineforge-codegen 1.3.0 proves through its confirmed-bar receipt ([#342]), and
the Pine adapter follows TradingView in more exit-binding, margin-call and
fill cases ([#341], [#345], [#347]). Engine v1.3.0 pairs with
pineforge-codegen 1.3.0, the pair the release hub's image
`ghcr.io/pineforge-4pass/pineforge-release:1.3.0` carries. The GitHub release
attaches the three prebuilt tarballs, `pineforge-v1.3.0-linux-x86_64.tar.gz`,
`pineforge-v1.3.0-linux-aarch64.tar.gz` and
`pineforge-v1.3.0-macos-universal.tar.gz`, each with a `.sha256`.
`<pineforge/pineforge.h>` is v1.2.0's byte for byte, so `PF_ABI_VERSION` stays
4 and `PF_CAPABILITIES_API_VERSION` stays 1; the two confirmed-bar calls are
per-strategy symbols the generated module defines, not runtime exports.
`native_c_api.h` and the native C++ host headers are unchanged, so no
published layout moves; the frozen historical headers and archives are
untouched. The script ABI epoch stays `engine_script_run_v19` and the state
hashes keep their domain tags; *State hashes* below lists every field 1.3.0
adds to their recipes. No report key changes (*Report keys*). The four rows of
the 1.0 C boundary that were scheduled for 1.1.0 (C-SURFACE-2: a non-mutating
execution preview, the origin and label of an applied event, a closed-trade
entry-comment accessor and a replace-options word) are not in this release
either.

- **Confirmed-bar admission in `pineforge-live` ([#342]).** A strategy
  generated by codegen 1.3.0 also defines `strategy_confirmed_bar_api_version`
  (1) and `strategy_confirmed_bar_receipt`, with the original receipt's
  signature and buffer and error protocol: canonical JSON of the lowered
  request expressions, the classified order shapes and intrabar persistence
  ([confirmed-bar extension](docs/strategy-capabilities.md#confirmed-bar-extension)).
  The runner checks that the two receipts agree. On confirmed one-minute bars
  with UTC/24x7 source settings and no external native configuration, it now
  admits one same-chart `request.security` from the proven table, each on its
  tested script clocks: `close` at `5`, `60` and `D`, `ta.sma(close, 4)` at
  `15`, `ta.ema(close, 3)` with gaps at `60`, `close[1]` at
  `timeframe.period` and Heikin-Ashi `close` at `5`; and a standalone
  close-only `varip` on script clock `1`. It still refuses by name, before
  any ledger exists, `process_orders_on_close=true` for every order shape, two
  requests, a request together with `varip`, other clocks, expressions and
  merge policies, other-symbol or unpinned feeds (symbol `""` included),
  lower-timeframe arrays and observed ticks; 1.2.0's other refusals stay. A
  library from codegen 1.2.0 has no confirmed-bar receipt and keeps 1.2.0's
  refusal of every `request.*` site and of `varip`, as does a receipt of an
  unrecognised version; a library without the capability extension runs as
  before, after the warning that its eligibility cannot be proved. The
  receipt's bytes are bound into the deployment identity. Batch backtests are
  unchanged.

- **Pine adapter: limit reach, exit binding, the carried trail and the
  margin-call schedule ([#341]).** A resting on-grid `calc_on_order_fills`
  limit that closes the position held when it was placed fills on its
  tick-built threshold, on the chart path (no magnifier intrabars), when its
  placement point does not already reach that threshold. `strategy.exit`
  binds at its call: to its entry's open lots if there are any, else to the
  entry id while that id has a working entry order, and the binding survives
  `close_all`, the close of a sibling, a flat and a reversal. That rule covers
  whole exits at absolute levels with a limit or stop parent, without
  `calc_on_order_fills`, with margins off. Under `process_orders_on_close`,
  without `calc_on_order_fills` and with margins off, a priced add of an id
  holding no lot that is still at the pyramiding cap once its bar's closes
  are done leaves the book at the next opening. A carried trail (without
  `calc_on_order_fills`, on the chart path) is reached on the tick-built print
  where that print reaches first, at its raw level, and the point order counts
  the stop and limit legs of exits that also trail. Margin calls follow
  TradingView's schedule in more cases: a short's call of `X` units is taken
  only where `X·p′·m > Q·p′·m − E` at its slipped print `p′` (`Q` the units
  held, `m` the margin requirement, `E` the equity); after a short's call the
  book is checked again lot by lot and its lagged follow-up comes at the
  bar's next path point (on a fractional lot grid, with a percent commission
  or none and one FX rate); a long's lagged follow-up also runs on whole-share
  lot grids; a `process_orders_on_close` short's call sized at the previous
  close and executed at the open is followed at that open; a deficit the
  open's check vetoed stays pending and is checked at the first path point
  whose segment touches a resting exit, before that exit; a commission-free
  long's one-unit call after a trade history uses initial capital plus the
  gains and losses sums; and a reversal or close placed at a close where a
  call is booked after it keeps the quantity the script saw. A same-side add
  at full margin is judged as the whole book at the tick-built signal close
  against the fee-charged equity there.

- **Pine adapter: margin calls at the open, the exact reversal tie and global
  exits on a held position ([#345]).** A full-margin, one-lot
  `process_orders_on_close` position (percent commission, slippage, a lot grid
  of at most 1) is called at the open after its close fill on TradingView's
  schedule: the call is sized at the source close, for longs as for shorts,
  its one-unit band and short gate are priced at the open's print, and the
  booked book is re-checked at the open's mark at that print, gated like any
  other check. Where that re-check is not short, the close-sized call's lagged
  follow-up books at the print; where it is short but books nothing (its units
  vetoed by the gate, or under one restore step), the sequence ends; and a
  short whose re-check books owes its own lagged follow-up to the bar's first
  extreme (outside the path walk's scope, to the open's print). In the
  `process_orders_on_close` path walk the lagged follow-up also runs on
  whole-share books, chains, and is dropped at a point whose own check is
  short. A leveraged book's open check runs after a market add filling there
  (slippage 0, percent commission, not `process_orders_on_close`). An
  explicit-quantity long from flat that fills at the next open is dropped
  where `sig10(sig10(E) / Q)` is below the slipped open tick (full margin,
  with slippage, not `process_orders_on_close`). A default 100 % reversal
  whose equity `E` is under its cost `sig10(Q·c′)` while `sig10(E)` reaches it
  keeps only its close leg beside resting `strategy.exit` stop and trail legs
  on a whole-unit lot grid, as it does on a clear book. A `strategy.exit` with
  no `from_entry`, called while a position is held beside an entry order of
  the other side, now binds to the held position: it took that order as its
  parent, so its exit filled bars late or only on a gapped open, a stop a tick
  late, and a relative profit or loss leg waited for a fill that never came.
  That fix applies at any margin setting.

- **Pine adapter: the position view after a close fill, a close's owed
  follow-up, open-marketable exits first and same-side global exits
  ([#347]).** Under `process_orders_on_close`, a re-issued `strategy.exit` of
  a one-lot short whose stop or limit is already through the bar's close at
  its call keeps the script's position view, as `close_all` already does for
  the size: the rest of the pass reads the pre-fill `strategy.position_size`
  and, at this exit, the pre-fill `strategy.position_avg_price`, as on
  TradingView, where it read a flat book, so an observer such as
  `strategy.position_size[1] < 0 and strategy.position_size == 0` no longer
  fires a bar early. `close_all`'s own average read is unchanged, a known
  divergence. A carried `process_orders_on_close` short's lagged follow-up
  owed to its bar's close is held through the close's own check, which still
  drops it where the close is short, and is otherwise booked after the script,
  ahead of the close's fills, at the same print; a `strategy.close(id)` of the
  whole one-lot book or a `close_all()` placed there keeps the size the script
  saw, and the excess opens as a lot of its own. An explicit stop entry placed
  at that close at pyramiding 0 is now refused as a same-side add beside that
  lot; it used to fill on the next bar. On a fractional lot grid, the
  close-sized call at the next open takes a book of several lots first in
  first out and checks its follow-up lot by lot. At a market entry that opens
  the book from flat at a bar's open (one lot, neither
  `process_orders_on_close` nor `calc_on_order_fills`), the exit legs it
  released that are already marketable at that open fill first, and the
  opening margin check runs on the book they leave. A `strategy.exit` with no
  `from_entry`, called while a position is held beside an add of the held side
  (resting at a limit or a stop, placed earlier in the same calculation, or
  queued by a fill recalculation), now binds to the held position: it took the
  pending add as its parent, so in a pyramiding script a profit or trail leg
  rested at the add's limit plus its ticks and filled at the next open, which
  could book thousands of one-bar trades, and a loss leg, or any leg beside a
  stop add, waited for the add's fill while the position ran to the end of the
  range.

- **What a script author sees ([#341], [#345], [#347]).** A script that
  reaches one of the cases above can get different fills, sizes or trade
  counts than under 1.2.0, following TradingView's trade lists in the cases
  its tapes pin. Three of the fixes: a `strategy.exit` with no `from_entry`
  binds to the held position beside an entry order of the other side
  ([#345]) and beside an add of the same side ([#347]), and a
  `process_orders_on_close` position is called at the open on TradingView's
  schedule ([#345]). Each rule is pinned by synthetic TradingView controls,
  predicted before export and committed as tests. Each has an internal switch
  that ships on; the switches are process-wide test hooks, not API, inputs or
  strategy settings. Release 1.3.0 is graded on registry baseline <!-- pf:releases[1.3.0].scoreboard.id|code -->`pineforge-parity-baseline-20261006-engine-7a1f01c0`<!-- /pf -->
  (engine <!-- pf:releases[1.3.0].scoreboard.engineCommit|short -->7a1f01c0<!-- /pf -->, the merge of [#347], which v1.3.0 equals in behaviour, and codegen <!-- pf:releases[1.3.0].scoreboard.codegenCommit|short -->3e50082f<!-- /pf -->), which
  holds <!-- pf:releases[1.3.0].scoreboard.excellent|int -->7,982<!-- /pf --> excellent / <!-- pf:releases[1.3.0].scoreboard.strong|int -->7<!-- /pf --> strong of <!-- pf:releases[1.3.0].scoreboard.graded|int -->7,989<!-- /pf --> probes,
  and <!-- pf:releases[1.3.0].scoreboard.belowStrong|int -->0<!-- /pf --> below strong. Release 1.2.0 grades <!-- pf:releases[1.2.0].scoreboard.excellent|int -->7,970<!-- /pf --> / <!-- pf:releases[1.2.0].scoreboard.strong|int -->19<!-- /pf -->.

- **Tests and documentation ([#343], [#344]).** A test pins that the
  exit-binding rule's pyramiding-cap part judges an add once, and the README's
  active scoreboard renders from the release hub's facts tokens. Neither
  changes the library.

### State hashes

The script ABI epoch (`engine_script_run_v19`), the domain tags
(`pineforge-broker-state/v19`, `pineforge-source-adapter/v4`) and the stream
fingerprint's version 19 are unchanged. Under the public contract's state-hash
rule a minor release may change a recipe only by adding a hashed field, with
the domain tags unchanged and the added field changing no trade or report, and
discloses it here; a hash only reads state, so no addition below changes a
trade or report by itself. 1.3.0 adds hashed fields in two pull requests and
removes, renames or re-encodes none. All of them are in the Pine adapter's
part of the broker-state hash, which `strategy_stream_state_hash` folds too,
and each is folded only while its new state is set, so for a run that never
reaches that state the recipe is 1.2.0's; state hashes from 1.2.0 do not
match 1.3.0's for the runs that reach it. A native C++ or C host that does not
run the Pine adapter folds none of them:

- [#341] adds, each folded only while set: a flag on a placed exit bound to
  its entry id (a whole `strategy.exit` at absolute levels, without
  `calc_on_order_fills`, called while a position is held and its entry id
  holds no lot but has a priced entry order working, with margins off); the
  exit calls kept for an entry id after a `strategy.cancel`
  withdrew the entry order they waited on (exit and entry ids, levels,
  quantity, comment and OCA name); a short's lagged margin-call follow-up owed
  at a point of a bar's path (the script bar, the point and the units); a
  margin call booked at a close after the script (the script bar, the book the
  script saw and the units called); and the carried trails re-priced for the
  bar in flight (each request and its trailing distance), which are emptied
  between bars.
- [#347] adds the opening margin checkpoint a market entry owes once the exit
  legs marketable at its open have filled (the script bar, the entry's request
  and id, and its fill), folded only while it is owed.

[#345] changes no recipe: the two adapter members it adds are transient and
never folded. The behaviour changes above also change state-hash values for
the runs they affect, with the recipe unchanged.

### Report keys

The JSON report of `docker/run_json.py`, the release image's
[report schema](docs/pages/report-schema.md), is v1.2.0's: the harness is
byte-identical, so no key is added, removed or renamed, and
`EQUITY_REPORT_KEYS` (`metrics.equity.sharpe_tv`, `sortino_tv`) stays.
`pf_report_t` and `pf_trade_t` are unchanged. Values change where the Pine
adapter changes above change trades. The runner's versioned formats
(`pineforge-native-report/v1`, `pineforge-native-report-delta/v1`,
`pineforge-live-status/v1`, `pineforge-redelivery-request/v1`) are unchanged;
the summary `pineforge-live run` prints when it stops adds a
`confirmed_bar_capabilities` member, null for a library without the receipt.

### Migration

- Regenerate strategy C++ with pineforge-codegen 1.3.0 and relink it against
  v1.3.0's headers and `libpineforge.a`. Only a regenerated module carries the
  confirmed-bar receipt. A C++ host that compiles against the Pine source
  layer rebuilds too: `PineExecutionAdapter`, `PlacementSnapshot` and
  `PineStrategyHost` under `include/pineforge/source/` gain members, so their
  layouts change inside the epoch, as the public contract allows for those
  headers. The new switch structs in `pineforge::source::detail` are test
  hooks, not API.
- A Pine script's trades can change from 1.2.0's where it reaches the adapter
  changes above, and state hashes from 1.2.0 do not match 1.3.0's for the runs
  described under *State hashes*: re-baseline stored results and hashes.
- `pineforge-live`: the ledger format is 1.2.0's, so a ledger 1.2.0 wrote
  needs no migration; a ledger written before 1.2.0 is migrated additively and
  one way, as 1.2.0 does, so back it up before upgrading from an earlier
  runner. A strategy library rebuilt against v1.3.0 is a new deployment,
  because the identity binds the library's bytes and its receipts: it starts a
  new ledger and cannot resume one an earlier build wrote. The new admission
  needs a library regenerated with codegen 1.3.0; one from codegen 1.2.0 keeps
  1.2.0's refusals.

[#341]: https://github.com/pineforge-4pass/pineforge-engine/pull/341
[#342]: https://github.com/pineforge-4pass/pineforge-engine/pull/342
[#343]: https://github.com/pineforge-4pass/pineforge-engine/pull/343
[#344]: https://github.com/pineforge-4pass/pineforge-engine/pull/344
[#345]: https://github.com/pineforge-4pass/pineforge-engine/pull/345
[#347]: https://github.com/pineforge-4pass/pineforge-engine/pull/347


## 1.2.0 — 2026-10-05

A minor release: it adds to the C ABI. `<pineforge/pineforge.h>` declares the
compiled execution-capabilities extension, the define
`PF_CAPABILITIES_API_VERSION` (1) and the calls
`strategy_capabilities_api_version` and `strategy_capabilities_receipt`, which
a strategy generated by pineforge-codegen 1.2.0 defines ([#332]). Engine v1.2.0
pairs with pineforge-codegen 1.2.0, the pair the release hub's image
`ghcr.io/pineforge-4pass/pineforge-release:1.2.0` carries. The GitHub release
attaches the three prebuilt tarballs, `pineforge-v1.2.0-linux-x86_64.tar.gz`,
`pineforge-v1.2.0-linux-aarch64.tar.gz` and
`pineforge-v1.2.0-macos-universal.tar.gz`, each with a `.sha256`.
`PF_ABI_VERSION` stays 4: `pineforge.h` only gains declarations and one
updated comment, and `native_c_api.h` and the native C++ host headers are
unchanged, so no published layout moves; the frozen historical headers and
archives are untouched. The script ABI epoch stays `engine_script_run_v19` and
the state hashes keep their domain tags; *State hashes* below lists every
field 1.2.0 adds to their recipes. No report key changes (*Report keys*). The
four rows of the 1.0 C boundary that were scheduled for 1.1.0 (C-SURFACE-2: a
non-mutating execution preview, the origin and label of an applied event, a
closed-trade entry-comment accessor and a replace-options word) are not in this
release either.

- **Compiled execution capabilities ([#332]).** The extension is
  `PF_CAPABILITIES_API_VERSION` 1, `strategy_capabilities_api_version()` and
  `strategy_capabilities_receipt()`. A strategy generated by codegen 1.2.0
  defines both calls and the runtime library does not, so a host looks them up
  with `dlsym`; a module from an earlier codegen lacks them. The receipt is
  immutable canonical JSON of the strategy's compiled execution declarations
  and its analyzed request and feed requirements, read through the checked
  settings caller-buffer protocol. It proves declarations, not that a stream
  reproduces the batch. [docs/strategy-capabilities.md](docs/strategy-capabilities.md)
  gives the version 1 schema.

- **Close-only admission in `pineforge-live` ([#332]).** Before it opens the
  ledger, the runner reads a library's receipt and refuses, by name, what its
  close-only stream cannot honour: intrabar calculation (`calc_on_every_tick`,
  `calc_on_order_fills`, `calc_on_every_history_tick`),
  `process_orders_on_close`, the bar magnifier, standard-OHLC fills, a nonzero
  `backtest_fill_limits_assumption`, a declaration-owned timeframe,
  account-currency conversion, auxiliary, native and recorded request feeds,
  historical probe overrides, `varip`, unresolved declarations, and every
  `request.*` site whatever its timeframe. Uses of `barstate.islast`,
  `barstate.islastconfirmedhistory`, `last_bar_index`, `last_bar_time`,
  `barstate.isrealtime` and `timenow` are refused too, display-only uses
  included. Accepted receipt bytes are bound into the deployment identity. A
  library without the extension runs as before, its requests included, after a
  warning that its eligibility cannot be proved; the warning is no proof.
  Capability checks do not change default batch computation, matching or
  margin.

- **Native runner service operation ([#331]).** `run --status-file PATH`
  publishes health, readiness and metrics (`pineforge-live-status/v1`)
  atomically, at most every `--status-interval S` seconds (default 1, 1..300)
  and at once on a lifecycle or readiness change and at exit; a publication I/O
  failure exits 1 after the committed input is kept. `probe --status-file PATH
  --max-age S [--ready]` (S 1..86400) gives a container probe exit 0 or 1.
  `--feed-idle-timeout S` and `--feed-message-timeout S` (default 15 each,
  1..300) bound intake separately; a WebSocket PONG cannot extend a message's
  assembly. With `run --control-dir PATH`, `redeliver --ledger L --deployment
  D --target T [--from N] [--failed-only] --control-dir PATH` submits a live
  redelivery to the running owner, with unchanged delivery IDs, durable
  deduplication and an append-only attempt audit; a bad control entry is kept,
  counted as `control_errors` and logged once, never fatal, and at most 256
  acknowledgements, each with a sanitized reason, are kept. Offline redelivery
  still refuses a running ledger. `--max-ledger-bytes N` (default 0,
  unlimited) stops after a whole message, with its input, actions and reports
  intact, and exits 3. SIGINT and SIGTERM finish or roll back the current
  message and cancel delivery promptly; a graceful signal stop of `run` or of
  offline `redeliver` now exits 0, not 130 (`actions --follow` keeps 130).
  Fatal errors exit 1 and an offline redelivery that leaves selected actions
  failed or pending exits 2. [Service operation](runner/README.md#service-operation)
  gives the schemas, bounds, restart semantics and supervisor grace periods.
  The runner still opens no inbound listener and ingests no venue fills.

- **Native cumulative reports ([#331]).** `pineforge-live report --ledger L
  [--deployment D] [--at-input N]` exports the cumulative engine report
  (`pineforge-native-report/v1` JSON) at any committed input cursor: every
  `pf_report_t` field, equity, open profit and the closed trades. The ledger
  stores reports as append-only `pineforge-native-report-delta/v1` rows
  instead of a full copy per input, and export rebuilds the byte-identical
  report. Each export re-verifies an append-only SHA-256 integrity chain over
  the committed inputs, stored state hashes, actions and report rows, so a
  ledger written before 1.2.0 needs one verified `run` replay to fill in its
  reports and digests before it exports. `run --report-jsonl` mirrors each new
  cumulative report to stdout as before; `run --report-jsonl=full` is an
  explicit alias, and `run --report-jsonl=delta` opts into delta output.
  A closed pipe exits 1, not SIGPIPE, and loses no input.
  The runner keeps tracing off and refuses to export a nonempty trace. A
  confirmed-bar run's report is checked field by field against a batch
  backtest of the same bars; the one tested difference is that a batch seals a
  trailing partial script bucket the live run keeps provisional ([cumulative
  reports](runner/README.md#cumulative-reports)). This runner change leaves the
  engine library and batch behaviour as they were.

- **Delivery wording ([#333]).** The runner guide states that a receiver can
  get the same request more than once, from a transport retry or
  libcurl replaying a POST on a reused connection, and must deduplicate on the
  stable `Idempotency-Key`: the runner retries a failed delivery at most twice
  and then does not re-send it by itself. The routing end-to-end test no longer
  races on wall-clock timing.

- **Pine adapter: exit children at one point and the callback lifecycle
  ([#328]).** When one point of a bar's path triggers the children of a
  `strategy.exit` that names no entry, the adapter fills them one at a time in
  TradingView's entry-id order, one closed row per settled slice with its
  share of the fees, where it booked one fill for the whole position; the
  shapes TradingView's tapes do not reach keep the one fill, and so does every
  run under `calc_on_order_fills`, `process_orders_on_close` or an intrabar
  feed.
  `strategy.wintrades`, `losstrades`, `eventrades` and the gross sums count the
  added rows. A named stop whose limit entry fills at a point the stop is
  already through fills at once, at the entry's fill with the stop's slippage
  (without `calc_on_order_fills` or an intrabar feed).
  `trail_points` convert to `ceil(trail_points - 8e-3 * mintick)` ticks, a
  tolerance that scales with the tick, instead of `ceil(trail_points - 5e-5)`,
  which stays the rule without a usable mintick; the comment of
  `strategy_pending_order_effective_levels` in `<pineforge/pineforge.h>` states
  the new rounding. After a declined reversal kills the held position's stop
  and limit exits, a later re-issue, changed or not, is a new live order; an
  unchanged re-issue used to keep the killed row. Under
  `calc_on_order_fills`, a limit exit born in a recalculation inside a leg
  reaches the leg's end only on that waypoint's tick-built print.

- **Pine adapter: margin, sizing and admission for one-position books
  ([#330]).** With no commission reserved out of the sizing money, point
  value 1, no FX series and a power-of-ten lot step, a default
  percent-of-equity quantity is the floor of the shortest decimal of
  TradingView's ten-significant-digit equity over the price (6.279, not
  6.2789); elsewhere the earlier floor stands. Under `process_orders_on_close`
  a default percent market order with a percentage commission is sized on its
  slipped price, grossed up by that commission. An explicit quantity opening
  from flat at full margin is judged at its signal close. Inside the regime
  TradingView's tapes pin (no fee, point value 1, no FX series, a power-of-ten
  lot step, at most one open lot), realized money is TradingView's two sums,
  gains and losses, rather than the engine's equity. Margin calls follow
  TradingView in more cases: a dust restore calls one unit; a single long
  lot's one-unit residual call is checked at the open, the low and the close
  without slippage, and at each bar's open with slippage, where negative free
  cash also calls the unit; a close placed after a call under
  `calc_on_order_fills` fills at the bar's next point; and a `close_all` is
  sized before the close point's call.

- **Pine adapter: explicit quantities, the script's equity mark, bar fills
  under `process_orders_on_close` and the session clock ([#334]).** On a
  power-of-ten lot grid, an explicit `strategy.entry` quantity that the grid's
  1e-6 tolerance snapped up to a grid point now floors to its shortest decimal
  (3124.9999999999973 trades 3124); it used to stay off the grid and never
  fill. Script-visible `strategy.equity` and `strategy.openprofit` mark an open
  position at the close rounded to the tick when the close lies off the tick
  grid (half-cent closes); the report and the kernel's own marks are
  unchanged. Under `process_orders_on_close`, a stop entry and its bracket that
  both fill inside one bar settle before the close-time script. `time()` and
  `time_close()` with an intraday timeframe and the script's own session
  argument read TradingView's session clock across a daylight-saving switch;
  the tapes cover New York and London only, and other zones follow the same
  form untaped.

- **Pine adapter: admission at the fill, close-first entries, the opening's
  order, exit tombstones and pyramiding records ([#335]).** TradingView admits
  an order in two halves, at the signal close and at the fill, and the adapter
  now applies the fill half: an order drops whole when the price its
  signal-close equity affords, on TradingView's ten significant digits, is
  under the slipped tick-built fill quote, and a reversal refused at its
  signal stays placed and drops at its fill. An entry the script places on
  the same bar behind its own `strategy.close(<held id>)` or
  `strategy.close_all()` of the opposite position is admitted by the money
  check alone and fills from flat with its frozen quantity. At the open,
  queued orders fill first, then the exits the opening print reaches, then the
  margin check on what is left; two shapes keep the earlier order, which
  [docs/pine-adapter-kernel-notes.md](docs/pine-adapter-kernel-notes.md)
  lists. A partial exit child that filled is not
  revived for the entry fill it filled under. Under pyramiding above one, the
  cap counts one record per entry fill not yet booked instead of the physical
  lots. These rules apply only on a power-of-ten lot grid, and some shapes
  keep the earlier rules.

- **What a script author sees ([#328], [#330], [#334], [#335]).** A script
  that reaches one of the cases above can get different fills, sizes or trade
  counts than under 1.1.0, following TradingView's trade lists in the cases
  its tapes pin. Where a rule has an internal switch it ships on; the switches
  are process-wide test hooks, not API, inputs or strategy settings. Release
  1.2.0 is graded on registry baseline
  `pineforge-parity-baseline-20261005-engine-52292db9` (engine 52292db9, the
  merge of [#339], which v1.2.0 equals in behaviour, with codegen 48e7a13b):
  7,970 excellent / 19 strong of 7,989 probes, none below strong. Release
  1.1.0 grades 7,951 / 38.

- **Per-bar cost ([#336], [#338], [#339]).** #336 removes per-bar work that
  the margin, sizing and admission rules above added. #338 removes temporary
  strings and repeated lookups on the native stream's confirmed-bar path.
  #339 reads each day's session occurrences once per call of the session
  clock behind `time()` and `time_close()` and stops copying their string
  arguments. None of them changes a trade, a report or a state-hash value.

- **Documentation ([#337]).** The README's main scoreboard renders from the
  release hub's facts tokens at engine 6b77f061 and codegen 285ac035; release
  scoreboards keep their own numbers.

### State hashes

The script ABI epoch (`engine_script_run_v19`), the domain tags
(`pineforge-broker-state/v19`, `pineforge-source-adapter/v4`) and the stream
fingerprint's version 19 are unchanged. Under the public contract's state-hash
rule a minor release may change a recipe only by adding a hashed field, with
the domain tags unchanged and the added field changing no trade or report, and
discloses it here; a hash only reads state, so no addition below changes a
trade or report by itself. 1.2.0 adds hashed fields in three places and
removes, renames or re-encodes none. All three are in the Pine adapter's part
of the broker-state hash, which `strategy_stream_state_hash` folds too, so
state hashes from 1.0.0, 1.0.1 and 1.1.0 do not match 1.2.0's for the runs
that reach them; a native C++ or C host that does not run the Pine adapter
folds none of them:

- [#328] adds the key count of TradingView's entry-id table, a running digest
  of its ids and its two peak sizes, folded once the table holds two ids or a
  peak passed 13 keys. The table takes each `strategy.entry` or
  `strategy.order` id at its first opening fill, so a script that opens
  positions under two or more ids folds it from the second id's first fill on.
- [#330] adds TradingView's realized money as two sums, gains and losses, with
  the closed-trade count, folded only when their sum differs in any bit from
  the engine's net profit.
- [#335] adds, each folded only while set: two flags on a placed order (a
  close-first entry, a reversal refused at its signal), the pyramiding records
  under pyramiding above one, the tombstones of partial exit children that
  filled, and the close-first fact of the current script calculation.

[#338] hands three string literals to the native continuation hash without a
temporary `std::string`; the words it folds are the same. The behaviour
changes above also change state-hash values for the runs they affect, with the
recipe unchanged.

### Report keys

The JSON report of `docker/run_json.py`, the release image's
[report schema](docs/pages/report-schema.md), is v1.1.0's: the harness is
byte-identical, so no key is added, removed or renamed, and
`EQUITY_REPORT_KEYS` (`metrics.equity.sharpe_tv`, `sortino_tv`) stays.
`pf_report_t` and `pf_trade_t` are unchanged. Values change where the Pine
adapter changes above change trades. The runner's new outputs are versioned
runner formats, not report keys: the `pineforge-live report` document
(`pineforge-native-report/v1`), the ledger's `pineforge-native-report-delta/v1`
rows, the status file (`pineforge-live-status/v1`) and the redelivery request
(`pineforge-redelivery-request/v1`).

### Migration

- Regenerate strategy C++ with pineforge-codegen 1.2.0 and relink it against
  v1.2.0's headers and `libpineforge.a`. Only a regenerated module carries the
  capability receipt.
- A Pine script's trades can change from 1.1.0's where it reaches the adapter
  changes above, and state hashes from 1.1.0 do not match 1.2.0's for the runs
  described under *State hashes*: re-baseline stored results and hashes.
- `pineforge-live`: back up a ledger before upgrading; 1.2.0 migrates it
  additively and one way, and earlier runners cannot open the migrated ledger.
  A ledger written before 1.2.0 needs one verified `run` replay before
  `report` exports it. A strategy library rebuilt against v1.2.0 is a new
  deployment, because the identity binds the library's bytes (and a
  regenerated library's capability receipt): it starts a new ledger and
  cannot resume one an earlier build wrote. A graceful signal stop of `run` or
  of offline `redeliver` exits 0 instead of 130, so a supervisor that read 130
  as a clean stop now reads 0. A regenerated library whose receipt names a
  requirement the close-only stream cannot honour is refused before the
  ledger opens.

[#328]: https://github.com/pineforge-4pass/pineforge-engine/pull/328
[#330]: https://github.com/pineforge-4pass/pineforge-engine/pull/330
[#331]: https://github.com/pineforge-4pass/pineforge-engine/pull/331
[#332]: https://github.com/pineforge-4pass/pineforge-engine/pull/332
[#333]: https://github.com/pineforge-4pass/pineforge-engine/pull/333
[#334]: https://github.com/pineforge-4pass/pineforge-engine/pull/334
[#335]: https://github.com/pineforge-4pass/pineforge-engine/pull/335
[#336]: https://github.com/pineforge-4pass/pineforge-engine/pull/336
[#337]: https://github.com/pineforge-4pass/pineforge-engine/pull/337
[#338]: https://github.com/pineforge-4pass/pineforge-engine/pull/338
[#339]: https://github.com/pineforge-4pass/pineforge-engine/pull/339

## 1.1.0 — 2026-10-04

A minor release: it adds to the C ABI. `<pineforge/pineforge.h>` declares six
more functions, the checked settings calls that a strategy generated by
pineforge-codegen 1.1.0 exports ([#317]). It also fixes the v1.0.0 and v1.0.1
known issue of stale `request.security` values in live streams ([#325]). Engine
v1.1.0 pairs with pineforge-codegen 1.1.0, the pair the release hub's image
`ghcr.io/pineforge-4pass/pineforge-release:1.1.0` carries; codegen 1.1.0 also
lists the other symbols' feeds a script requests
(`transpile_full()["requests"]`), the feeds the harness's `--symbol-feeds`
installs ([#327]). The GitHub release attaches the three prebuilt tarballs,
`pineforge-v1.1.0-linux-x86_64.tar.gz`, `pineforge-v1.1.0-linux-aarch64.tar.gz`
and `pineforge-v1.1.0-macos-universal.tar.gz`, each with a `.sha256`.
`PF_ABI_VERSION` stays 4: `pineforge.h` only gains declarations, and
`native_c_api.h` and the native C++ host headers are unchanged, so no published
layout moves; the frozen historical headers and archives are untouched. The
script ABI epoch stays `engine_script_run_v19` and the state hashes keep their
domain tags; *State hashes* below lists every change to their values. The
installed header `pineforge/live_parser.h` is removed with the runner's parser
plugins ([#320]); no surface of the [public
contract](docs/pages/public-contract.md) covers it. The four rows of the 1.0 C
boundary that were scheduled for 1.1.0 (C-SURFACE-2: a non-mutating execution
preview, the origin and label of an applied event, a closed-trade entry-comment
accessor and a replace-options word) are not in this release.

- **Per-bar performance.** Per-bar cost restored on the batch and stream paths
  (no behaviour change).

- **Checked strategy settings ([#317]).** The six functions are
  `strategy_settings_api_version`, `strategy_create_checked`,
  `strategy_set_input_checked`, `strategy_set_override_checked`,
  `strategy_get_effective_settings` and `run_backtest_full_checked`, with the
  status codes `pf_settings_status_t` and `PF_SETTINGS_API_VERSION` 1. With them
  a C host refuses a bad input or override before a run (an unknown or ambiguous
  key, an invalid enum, boolean or number, a declared range violation) with a
  status code and a message, keeping the previous value; reads a JSON receipt of
  every declared input and supported override with its default and effective
  value; and runs a batch backtest that returns a status instead of letting a
  C++ exception out. Look up `strategy_settings_api_version` with `dlsym`: a
  module generated by an earlier codegen lacks all six and keeps the legacy
  calls. The generated code uses `include/pineforge/checked_settings.hpp`;
  [docs/checked-settings.md](docs/checked-settings.md) is the guide.

- **C++ exceptions stop at the C ABI ([#317]).** A module generated by codegen
  1.1.0 catches them at every C entry point, the legacy ones included: a legacy
  run or setter that throws leaves the entry point and the message in
  `strategy_get_last_error`, and a batch call returns the existing empty-report
  failure. Two behaviours change. A legacy setter that throws refuses every
  later batch run and stream begin on that handle; recreate the handle. A
  stream begin whose script preparation throws fails, with NOT_COMPLETED from
  `strategy_last_run_status`, instead of starting; batch keeps its behaviour
  for those exceptions. `pineforge-live` uses the checked calls when the
  strategy has them: it refuses bad settings before it opens the ledger, and it
  adds the effective-settings receipt to the deployment identity and to the run
  summary (`effective_settings`). A strategy regenerated with codegen 1.1.0
  therefore cannot resume a ledger its earlier build wrote. With a strategy
  from an earlier codegen the runner warns and keeps the legacy behaviour.

- **Confirmed-bar streams compute what the batch computes ([#325]).** A native
  stream fed confirmed bars now applies the same TradingView-parity rules as a
  batch backtest over the same bars, so its trades can differ from 1.0.1's and
  now follow the batch backtest's, apart from the look-ahead and
  calendar-boundary limits the streaming guide's new *Backtest vs live* section
  lists. Rules that streams had switched off are now chosen by the input:
  historical semantics for batch and confirmed-bar input, observed-print
  semantics only for genuine tick input. This fixes the v1.0.0 and v1.0.1 known
  issue ([#323]): in a stream whose input and script timeframes are equal,
  same-chart `request.security` values, Heikin-Ashi requests included, no
  longer stop updating after the first realtime bar, because each newly
  observed confirmed bar now reaches the requested-series evaluator. Two
  stream-only fill differences also go: both-sided stop entries with default
  sizing, and `process_orders_on_close` attribution. Batch results are
  unchanged. A stream still refuses a missing confirmed bar and
  `calc_on_order_fills`.

- **Pine adapter parity ([#315], [#316]).** The adapter follows TradingView's
  trade lists in more cases: same-bar entries under pyramiding; opening margin
  checks on TradingView's opening schedule, gap-open checks kept; stop
  reversals sized from their placed units; FIFO closes that split a lot, which
  no longer leave a zero-quantity remainder; the trail extreme and the closing
  stop carried through an opening fill's callback; all-in entries admitted
  against the buying power at the order's source time; named percentage exit
  targets sized from the live entry's ancestry; `request.security` calendar
  history that no longer depends on auxiliary feeds, with full-day minute
  requests published at the session close, early closes included; a stream
  report's cumulative mark kept at a range end; a repeated short margin check
  that reuses a resolved margin-call fill instead of applying slippage twice;
  and exact fractional reversals under default and one-pyramid sizing, with a
  short opening stop on a bar that opens at its high booked the way
  TradingView's callbacks book it. The range-end change moves no fill: a
  realtime stream that reaches its range end no longer re-marks its stored
  terminal equity point or re-folds its equity extremes, and the report shows
  the re-marked last point through a presentation hook instead, so that
  stream's equity extremes and per-bar state hashes change while its trades
  stay the same. A script that reaches one of these cases can get different
  fills, sizes or trade counts than under 1.0.1. #315 moves 35 probes from
  strong to excellent and #316 three more.

- **Tick-built bar volume ([#319]).** In a stream fed with ticks, the forming
  bar sums its prints' quantities exactly when the run's quantity grid is a
  decimal step (1, 0.1, 0.01, …) and with compensated summation otherwise, so
  it carries the volume that the same bar built offline from the same prints
  has. Confirmed-bar input and batch runs are unchanged. Its hash change is
  listed under *State hashes*.

- **Report-only terminal quote ([#321]).** A magnified intraday batch run can be
  given the chart's terminal quote: `scripts/run_strategy.py` takes the terminal
  bar's time and close from the chart feed that
  `PINEFORGE_RUN_REPORT_CHART_QUOTE` names (pinned by
  `PINEFORGE_RUN_REPORT_CHART_QUOTE_SHA256`), a C host sets the
  metadata keys `report_terminal_quote_time_ms` and
  `report_terminal_quote_close` with `strategy_set_syminfo_metadata`, and a C++
  host calls `PineStrategyHost::set_report_terminal_quote`. The report then
  prices a position still open at the range end, the synthetic `open_at_end`
  row, at that quote: the row's exit fields, P&L and commission, the net
  profit, the last equity point and the metrics derived from them change;
  fills, the book, the hashes and the trace do not. Without the input nothing
  changes, and `docker/run_json.py` does not read it.
  [docs/report-terminal-quote.md](docs/report-terminal-quote.md) lists the
  fields and the limits.

- **Grader distinct-entry identity ([#313]).** `scripts/verify_corpus.py`
  checks an engine entry against every TradingView entry key with the same time
  and direction, not only the keys with several Signals: a price equal to one
  key counts only there, and a price within tolerance of a multi-Signal key and
  of another key refuses *excellent*. Trade matching and the thresholds are
  unchanged; the README's *Distinct-entry identity* section states the rule.

- **Harness symbol feeds ([#327]).** `docker/run_json.py --symbol-feeds`
  (`PINEFORGE_SYMBOL_FEEDS` in the image entrypoint) installs other symbols'
  bars for `request.security` on another symbol, which the harness did not do
  before: a JSON index keyed by the exact symbol string the script passes and
  by timeframe, each symbol with its catalog `syminfo` and one OHLCV CSV per
  timeframe, installed through `strategy_set_symbol_facts` and
  `strategy_set_symbol_feed` (C ABI calls since 1.0.0). Nothing aggregates
  another symbol's bars, so a `1` feed serves neither a `240` nor a `D`
  request; the run is historical only, with at most 256 symbols and 256 feeds
  in one index. Unset, nothing changes. An index or feed the harness cannot
  install fails the run (one `{"engine":"pineforge","error":...}` line whose
  error begins `--symbol-feeds:`, harness exit 1, entrypoint exit 4), and what
  it installed is recorded as `applied_runtime.symbol_feeds`, so such a run has
  its own fingerprint digest. The change is in the harness alone; the release
  hub's image passes `PINEFORGE_SYMBOL_FEEDS` to it as `--symbol-feeds`
  (pineforge-release#23). [docker/README.md](docker/README.md) gives the index
  format and the limits.

- **Harness lot grid ([#322]).** `docker/run_json.py` applies `mincontract`
  from the `--syminfo` (`PINEFORGE_SYMINFO`) file as the engine's lot grid, the
  metadata key `qty_step` plus the `mincontract` key a script's
  `syminfo.mincontract` reads, so order quantities are floored to the lot and a
  percent-of-equity strategy no longer trades sub-lot quantities; v1.0.1's
  harness ignored `mincontract`. Absent or `null` changes nothing: for the same
  strategy library the report and its fingerprint are as before, apart from
  `elapsed_seconds`. Instead of running without the grid, the run fails (one
  `{"engine":"pineforge","error":...}` line, harness exit 1, entrypoint exit 4)
  when `mincontract` is not a positive finite number, with the message
  `syminfo.mincontract must be a positive finite number, got <the value as
  JSON>`, or when `mincontract` is set and the strategy library has no
  `strategy_set_syminfo_metadata`, with the message `the strategy library has
  no strategy_set_syminfo_metadata, so syminfo.mincontract cannot be applied`.
  An applied grid is recorded as `applied_runtime.syminfo`, so a gridded run has
  its own fingerprint digest. The change is in the harness alone; the release
  hub copies `docker/run_json.py` from the engine tag, so `pineforge-release`
  1.1.0 carries it.

- **Native runner routing and delivery ([#320]).** Webhooks are optional;
  `--webhook-routes` adds first-match per-action targets and payload
  `pineforge-native-order-action/v2`.
  New `actions`, `status` and offline `redeliver` commands expose the journal and
  delivery audit. `redeliver` requires the ledger's `--deployment` identity.
  Existing `--webhook-url` deployments keep one default target, exact v1 payload
  bytes and event-id idempotency keys, but delivery behavior changes: HTTP errors
  are final, normal runs exit 0 even with failed deliveries, and `--max-attempts N`
  caps transport retries at `min(2, N-1)` rather than stopping computation.
  Default delivery timeouts are now 2 s connect / 5 s total (formerly 5 s / 15 s).
  Fatal exits drain for at most one total timeout and report unsent actions;
  SIGINT/SIGTERM exit 130. Redelivery exits 2 for failed or pending selections.
  Schema-1 native ledgers migrate additively to an append-only delivery log without
  rewriting actions. This migration is one-way: older runner binaries cannot open
  the migrated ledger. Back up the ledger before upgrading. This runner change
  leaves the engine library as it was.

- **Runner tooling removal ([#320]).** The native live runner accepts only
  normalized PineForge feed events from stdin, files, or a user's own
  HTTP/WebSocket feed service. The installed `pineforge/live_parser.h` header, its
  `PF_LIVE_PARSER_*` types/constants and `pf_live_parser_abi_version` /
  `pf_live_parse_message` plugin exports, the `--parser` / `--parser-config`
  flags, and the example plugin are removed. This breaks callers that included
  that header, authored native parser plugins, or passed raw provider messages
  through the runner. Migrate translation to an **external feed adapter** that
  emits [PineForge feed events](runner/README.md#feed-format); feed URLs address
  that adapter, not an exchange. The outbound order-action webhook remains.
  Strict native runner configurations now also require 1m input; higher script
  timeframes still aggregate those minutes.
  This change itself alters no versioned engine `PF_API` export, native C++
  surface, script ABI epoch or engine behavior. Frozen historical ABI header
  manifests and archives remain intact. For a strategy without the checked
  settings calls, plugin-free ledger identity bytes remain unchanged; ledgers
  bound to removed plugins cannot be resumed by this runner and must not be
  silently rewritten.

- **Documentation ([#312], [#318], [#323], [#324]).** The README's new *How a
  change is judged* section states the merge rule's zero-score outcome as it is
  now: a score of exactly 0 passes only when nothing got worse (#312). The
  parity figures of the README and the docs pages render from the release hub's
  public facts tokens, kept in step by `lab facts render` and `lab facts check`,
  and the README keeps a release's grades apart from main's scoreboard (#318);
  the README's Parity and Trades badges render as images again (#324). The
  streaming page gains a *Known issues* section (#323).

- **Known issue fixed ([#323], [#325]).** The stream `request.security` known
  issue that #323 documented for v1.0.0 and v1.0.1 is fixed in 1.1.0 by #325
  (above); the README keeps the note and links to the streaming page, which
  keeps the workaround for those versions.

### State hashes

The script ABI epoch (`engine_script_run_v19`) and the domain tags
(`pineforge-broker-state/v19`, `pineforge-source-adapter/v4`) are unchanged. A
behaviour change above changes state-hash values for the runs it affects while
the recipe (which fields are hashed and how they are encoded) stays the same;
#315's range-end change, for example, changes a realtime stream's per-bar
hashes and leaves its trades as they were. Under the public contract's
state-hash rule, amended in this release, a minor release may also change a
recipe by adding a hashed field, with the domain tags unchanged, when the added
field changes no trade or report, and discloses it here. 1.1.0 adds hashed
fields in four places and removes, renames or re-encodes none, so state hashes
from 1.0.0 and 1.0.1 do not match 1.1.0's for the runs that reach them:

- [#315] adds a record to the Pine adapter's part of the broker-state hash: the
  original request, placement and submission context of a same-bar market entry
  that joins a fixed market batch, folded only while such an entry is pending,
  a state #315 introduces.
- [#316] adds an opening stop's next-waypoint flag
  (`coof_open_stop_next_waypoint`) to the same adapter state, on each hashed
  placement and on the adapter, each folded only when set; the witness rows
  `Random44/0/0` and `Random44/0/1` were re-pinned with their trades hash
  unchanged (13 trades each).
- [#319] adds to the native continuation hash, which the broker-state hash
  folds, both tick-volume accumulators together with the partial bar and its
  script-bar open, in an observed-tick stream and only where an accumulator is
  not at its default or redundant with the bar's published volume; every
  existing tick, bar and batch pin holds.
- [#325] adds stream-only state to the Pine scheduler's part of the
  broker-state hash: whether the input is observed ticks, the confirmed-input
  counters and the retained confirmed input bars. A batch run folds none of it.
  The witness rows `Stream/0/0` and `Stream/0/1` were re-pinned with their
  trades hash too (the trade count stays 5), because the same change makes the
  stream compute what the batch computes.

### Report keys

The JSON report of `docker/run_json.py`, the release image's
[report schema](docs/pages/report-schema.md), from v1.0.1 to v1.1.0:

- No key is removed or renamed. `EQUITY_REPORT_KEYS` is unchanged:
  `metrics.equity.sharpe_tv` and `sortino_tv` stay.
- Added: `applied_runtime.syminfo`, `{"qty_step": <float>, "mincontract":
  <float>}`, present only when the `--syminfo` (`PINEFORGE_SYMINFO`) file
  carries a valid `mincontract` and absent otherwise.
  `fingerprint.provenance.runtime` holds the same fields as `applied_runtime`,
  so a gridded run's fingerprint differs from the gridless run's, and a gridless
  report is the one v1.0.1's harness writes for the same strategy library,
  apart from `elapsed_seconds`.
- Added: `applied_runtime.symbol_feeds`, `{"canonicalization":
  "pf-symbol-feed-barc-close-le-v1", "symbols": {"<symbol string>": {"facts":
  {...}, "feeds": {"<timeframe>": {"bars": <int>, "first_ts": <int>, "last_ts":
  <int>, "source_values_sha256": "<hex>"}}}}}`, present only when
  `--symbol-feeds` (`PINEFORGE_SYMBOL_FEEDS`) installed at least one symbol and
  absent otherwise; `first_ts` and `last_ts` appear only for a feed with bars.
  `facts` holds the catalog facts that were set, `canonical`, `type`,
  `timezone`, `session` and `currency` as strings and `mintick` as a number,
  each only when the index gives it. `fingerprint.provenance.runtime` holds the
  same fields, so a run with symbol feeds has its own fingerprint.
- Failure output: the existing one-line `{"engine":"pineforge","error":...}` on
  stdout, harness exit 1 and image entrypoint exit 4 (the existing
  backtest-failure code), with new messages: the lot grid's
  `syminfo.mincontract must be a positive finite number, got <the value as
  JSON, cut at 80 characters>` and `the strategy library has no
  strategy_set_syminfo_metadata, so syminfo.mincontract cannot be applied`, and
  the symbol feeds' messages, each beginning `--symbol-feeds:` (an index or
  feed it cannot read or refuses, a library without `strategy_set_symbol_facts`
  / `strategy_set_symbol_feed`, or a fact or feed the engine refuses).

### Migration

- Regenerate strategy C++ with pineforge-codegen 1.1.0 and relink it against
  v1.1.0's headers and `libpineforge.a`. Only a regenerated module has the
  checked settings calls and stops C++ exceptions at its C entry points.
- Back up a native runner ledger before upgrading: its migration to the
  delivery log is one-way, and older runner binaries cannot open the migrated
  ledger. A ledger bound to a removed parser plugin cannot be resumed, and a
  strategy regenerated with codegen 1.1.0 needs a new deployment.
- A confirmed-bar stream's trades can change from 1.0.1's ([#325]), and state
  hashes from 1.0.0 and 1.0.1 do not match 1.1.0's for the runs *State hashes*
  lists: re-baseline stored stream results and hashes.
- Code that included `pineforge/live_parser.h` or wrote a parser plugin moves
  that translation into an external feed adapter that emits
  [PineForge feed events](runner/README.md#feed-format).

[#312]: https://github.com/pineforge-4pass/pineforge-engine/pull/312
[#313]: https://github.com/pineforge-4pass/pineforge-engine/pull/313
[#315]: https://github.com/pineforge-4pass/pineforge-engine/pull/315
[#316]: https://github.com/pineforge-4pass/pineforge-engine/pull/316
[#317]: https://github.com/pineforge-4pass/pineforge-engine/pull/317
[#318]: https://github.com/pineforge-4pass/pineforge-engine/pull/318
[#319]: https://github.com/pineforge-4pass/pineforge-engine/pull/319
[#320]: https://github.com/pineforge-4pass/pineforge-engine/pull/320
[#321]: https://github.com/pineforge-4pass/pineforge-engine/pull/321
[#322]: https://github.com/pineforge-4pass/pineforge-engine/pull/322
[#323]: https://github.com/pineforge-4pass/pineforge-engine/pull/323
[#324]: https://github.com/pineforge-4pass/pineforge-engine/pull/324
[#325]: https://github.com/pineforge-4pass/pineforge-engine/pull/325
[#327]: https://github.com/pineforge-4pass/pineforge-engine/pull/327

## 1.0.1 — 2026-10-02

A documentation-only release that pairs with pineforge-codegen 1.0.1, the pair
the release hub's image `ghcr.io/pineforge-4pass/pineforge-release:1.0.1`
carries. Since v1.0.0 no file under `src/`, `include/`, `cmake/` or
`examples/`, no CMake file and nothing under `docker/` but its README changed:
the library, the C ABI (`PF_ABI_VERSION` 4), the native C++ API, the script ABI
epoch `engine_script_run_v19` and its state-hash values, and the JSON report
keys of `docker/run_json.py` (`metrics.equity.sharpe_tv` / `sortino_tv`
included) are v1.0.0's. The GitHub release attaches the three prebuilt
tarballs, `pineforge-v1.0.1-linux-x86_64.tar.gz`,
`pineforge-v1.0.1-linux-aarch64.tar.gz` and
`pineforge-v1.0.1-macos-universal.tar.gz`, each with a `.sha256`.

- **Why a release.** From 1.0.0 the engine and pineforge-codegen release one
  version number, and codegen 1.0.1 fixes C++ that did not compile and a value
  that came out silently wrong (its
  [changelog](https://github.com/pineforge-4pass/pineforge-codegen-oss/blob/main/CHANGELOG.md)).
  Regenerate strategy C++ with codegen 1.0.1 and relink it against v1.0.1's
  headers and `libpineforge.a`: the pair changed, though no ABI number did.
- **Documentation.** The README, the docs pages and this changelog describe
  the v1.0.0 release (#309). The docs no longer point at the retired Python
  `pineforge-live` runtime; the native `pineforge-live` runner in `runner/`
  replaces it (#310).

## 1.0.0 — 2026-09-30

The first stable release under semantic versioning. From it on the engine and
pineforge-codegen release one version number, and the only supported pair is
the same version, prerelease included: engine v1.0.0 pairs with
pineforge-codegen 1.0.0, the pair the release hub's image
`ghcr.io/pineforge-4pass/pineforge-release:1.0.0` carries. The GitHub release
attaches the prebuilt static libraries, headers and CMake package for three
platforms, `pineforge-v1.0.0-linux-x86_64.tar.gz`,
`pineforge-v1.0.0-linux-aarch64.tar.gz` and
`pineforge-v1.0.0-macos-universal.tar.gz`, each with a `.sha256`
(docs/pages/install.md).

The release candidate v1.0.0-rc.1 (bb9ec14f) came out the same day. v1.0.0
(5718c5dc, whose only change is `VERSION`) is built from main 133f5714, the
one commit after the candidate (#308). That commit fixes a CI flake of
`test_adapter_lookup_index_scaling`, which now times its two tapes in turn, in
user CPU; lets CI skip its proof jobs on a documentation-only change
(`scripts/ci_docs_only.py`), as docs/ci.md, CONTRIBUTING.md and AGENTS.md now
say; and refreshes the benchmark, re-measured at engine 35db01c8 against
PyneCore 6.10.3 on an AWS c7a.8xlarge, with README.md's figures. No file under
`src/`, `include/`, `cmake/` or `examples/`, and no CMake file, changed
between the candidate and the release.

The notes below are the notable changes since v0.13.1 (2026-09-06). Development
builds from 439bd520 until the candidate read `VERSION` 0.14.0, a version that
was never tagged.

### What 1.0 means

- **The kernel runs without any Pine adapter.** A C++ host
  (`NativeStrategyHost`, `<pineforge/native_host.hpp>` and the native headers)
  or a C host (`<pineforge/native_c_api.h>`) drives the execution kernel that
  generated PineScript strategies run on: batch and stream runs, the native
  request vocabulary, margin and liquidation, risk limits, FX conversion, the
  per-bar broker-state hash and the report. Every build also installs that
  kernel alone, `libpineforge_kernel.a` (`PineForge::kernel`, #257), and
  `-DPINEFORGE_BUILD_SOURCE_LAYER=OFF` builds the kernel alone (`libpineforge.a`
  then holds exactly its objects); `examples/native` holds 18 Pine-free hosts
  (16 C++, 2 C) that run as CTest rows under `-DPINEFORGE_BUILD_EXAMPLES=ON`.
- **Pine is an adapter over that kernel.** The Pine source layer was cut out of
  `BacktestEngine` (#253), and generated Pine execution was lowered onto the
  native kernel, retiring the legacy loop (#254). Its host
  (`PineStrategyHost`) and headers live under `include/pineforge/source/`.
- **From 1.0.0 the version number promises something:** the C ABI, the
  native C++ API, the script ABI epoch and its state-hash values, the 1.0
  C-surface boundary and the pairing with codegen, as the
  [public contract](docs/pages/public-contract.md) states.

### Breaking changes: what a 0.x user must act on

- **C ABI version 4.** `PF_ABI_VERSION` is 4 (0.13.1: 3). `pf_report_t`
  appends `broker_state_hash` and `broker_state_hash_len`, and it is
  caller-allocated, so a mirror built for ABI 3 under-sizes it: update FFI
  mirrors and check `pf_abi_version() == PF_ABI_VERSION` before a run
  (439bd520, d9b80809). No `PF_API` function was removed or renamed; `pineforge.h`
  declares 39 more (71 against 0.13.1's 32) and `native_c_api.h` adds the
  native host API.
- **Two C fields renamed.** `pf_equity_stats_t::sharpe_tv` / `sortino_tv` are
  `sharpe_monthly` / `sortino_monthly`, at the same offsets (48 and 56); the
  old spellings, kept as deprecated aliases during development, are removed
  for 1.0. Rename them in C code and in ctypes or Rust mirrors. The JSON
  report keys stay `sharpe_tv` / `sortino_tv`. Development builds' C++
  `exit_legs::Domain::Coof` / `MagnifierCoof` are likewise gone; the
  enumerators are `FillRecalc` / `MagnifierFillRecalc`, with the same values.
- **Regenerate and relink every generated strategy.** The C++ a generated
  strategy compiles against now carries the script ABI epoch
  `engine_script_run_v19` (an inline namespace 0.13.1 did not have), so an
  object built against 0.13.1's headers does not link. Transpile with
  pineforge-codegen 1.0.0, the release that pairs with this one (its 0.10.4
  predates the epoch), and rebuild; from 1.0.0 on the pair is the codegen
  release of the same version. `PF_ABI_VERSION` equality alone does not make
  a pair.
- **State hashes are new, and v19's.** The broker-state hash, the per-bar hash
  rows, the stream fingerprint and the native continuation hash did not exist
  in 0.13.1; their values are the v19 epoch's as 1.0.0 computes them (#282,
  #283, #284, #286, #289; #294, #295 and #297 fold new source-layer state
  where a run sets it), and a value recorded by an earlier development build
  is not comparable. From 1.0.0 their recipe stays fixed for all of 1.x.
- **Event retention defaults to a window.** `NativeRunSpec::event_retention`
  defaults to `NativeEventRetention::Window`, which keeps the command journal
  only until every reader has consumed it (#283). A C++ host that reads the
  whole run's events after the run sets `Full` (or `Commands`). A C host that
  does not send the retention word keeps `PF_NATIVE_EVENT_RETENTION_FULL`.
- **`-ffp-contract=off` reaches consumers.** The CMake package hands it to
  every translation unit that links `PineForge::pineforge` or
  `PineForge::kernel` (#282); 0.13.1 compiled the library alone with it. A
  build outside CMake compiles generated strategy code with
  `-ffp-contract=off` itself (docs/pages/integration-cmake.md). The package
  also hands every Clang-compiled consumer `-fbracket-depth=1024`, so the
  deeply nested C++ codegen emits for deep Pine compiles; a Clang build
  outside CMake adds that flag itself.
- **One header moved.** `<pineforge/pine_float_compare.hpp>` is now
  `<pineforge/source/pine_float_compare.hpp>`, a source-layer header
  (e71e589c); new code includes `<pineforge/ta_compare_band.hpp>`. A
  kernel-only build installs neither `include/pineforge/source/` nor
  `include/pineforge/compat/`.
- **CMake pins move to 1.** The package's version file is
  `SameMajorVersion`, so `find_package(PineForge 0.N ...)` does not find a 1.x
  install: pin `find_package(PineForge 1.0 REQUIRED)`
  (docs/pages/integration-cmake.md).
- **Versions may carry a prerelease.** `VERSION` is `X.Y.Z` or `X.Y.Z-rc.N`.
  `PINEFORGE_VERSION_STRING`, `PineForge_VERSION` and `pf_version_get()` stay
  numeric; the full version is `PINEFORGE_VERSION_FULL`, the package's new
  `PineForge_VERSION_FULL` and `pf_version_string()`, so
  `find_package(PineForge 1.0.0 EXACT)` also accepts `1.0.0-rc.N`. The CMake
  smoke consumer (`cmake/smoke_consumer`) prints `pf_version_string()`.

### What a native host must handle

- **An unrepresentable quantity is a refusal, not a failed run.** A request
  the settlement cannot book exactly in binary64 (a close that would leave a
  sub-ulp rest, an opening a surviving dust lot absorbs) is refused with
  `MatchRejectReason::UnrepresentableQuantity` (C
  `PF_NATIVE_MATCH_REJECT_UNREPRESENTABLE_QUANTITY`, 10) and the run goes on.
  Handle it where you handle the other match rejections (#289).
- **Quantity tolerance is opt-in.** `NativeRunSpec::quantity_tolerance` (C:
  the sixth `pf_native_run_spec_ext_v1` layout, flag
  `PF_NATIVE_SPEC_EXT_QUANTITY_TOLERANCE`) lets a close within that distance
  of a FIFO lot boundary end on it; leave it unset and nothing changes. On a
  quantity grid, a whole-scope `ScopeFraction` and a close of one lot's own
  size are on the grid (#289).
- **OCA groups never stop a run on binary64 dust.** A sibling deduction too
  small to move the sibling's units is absorbed and recorded with a zero
  deduction (#289). A sibling re-priced with `ReplaceOptions::keep_handle`
  inside a group no longer fails the run when another member fills; the run
  records what a plain replace records. On a quantity grid, a request whose
  pending group deduction its units absorb settles in one fill, so a
  whole-scope close closes its lot whole.
- **Aggregated runs count script bars.** When the script timeframe aggregates
  the input, `NativeCoordinate::interval_index` and every lot, trade-row and
  metric index are script-bar indices; read the input slot from
  `NativeCoordinate::input_interval_index` (#289).
- **Report points at the host's cadence.** Under
  `NativeReportPolicy::KernelRecordedAtHostMarks`, a C++ host calls
  `NativeStrategyHost::mark_native_report_point` from its own callbacks; it
  returns `false` outside such a run. The C surface has no such call (#289).
- **Final calendar buckets calculate.** A batch, or `stream_end(true)`,
  calculates its last pending script bucket when that bucket's last input
  reached the session close, so the final daily bucket of an hourly feed now
  calculates; warmup and `stream_end(false)` still carry it (#289).
- **A new margin check kind.** On a continuous intrabar path
  (`IntrabarPath::lower_tf` with its default `ContinuousSegments`
  eligibility) the kernel checks the margin model at every later sample of
  the bar, at that sample's price:
  `NativeMarginCheckKind::IntrabarSample` (C
  `PF_NATIVE_MARGIN_CHECK_INTRABAR_SAMPLE`, 4). A requirement hook sees it; a
  host that switches on the kind must handle it.
- **A current execution refuses a request bound to a host roster.**
  `execute_current` on a request whose owner is a host roster (`BindCohort`)
  answers `NativeCurrentRefusal::UnsupportedRequest` (C
  `PF_NATIVE_REFUSAL_UNSUPPORTED_REQUEST`, 5) before its point is taken; the
  request stays queued and fills at the next match like any queued close.
  When the roster held a live lot it used to fail the run. To close a
  cohort's lots at once, bind the request to its openings (`BindOpenings`).
- **The post-fill margin check measures the fill's own segment.** The
  kernel's `AfterApplied` margin point measures from the waypoint a fill was
  reached on the way to, with the fill price as that point's mark, so a limit
  or stop filled on the way to a bar's extreme is checked at that extreme on
  its own bar. A host with a margin model can book a margin call a bar
  earlier than before; a requirement hook sees that point.
- **Two C++ calendar queries.** `NativeStrategyHost::native_aggregates_input_bars()`
  says whether the run's script bars are buckets the kernel gathers from its
  input bars, and `native_calendar::native_civil_date` / `native_civil_days`
  convert between days since the epoch and a civil date. Neither has a C
  spelling (the 1.0 C boundary table).
- **The C callback table's marker must be zero.** A C host built against the
  current `pf_native_callbacks_v1` leaves its trailing `reserved1` at 0:
  `strategy_native_host_create_v1` returns NULL for any other value. Tables of
  the three earlier published lengths keep working (#289).
- **A script bar's session facts are read at its own label.**
  `NativeCoordinate::in_session`, `opens_session_day` and
  `closes_session_day` read a bar at its label, or at its interval's first
  eligible instant when that is later, never at an earlier instant of its
  interval: on an extended-hours NASDAQ:AAPL 60-minute chart the 16:00 bar is
  out of session and the regular day closes on the 15:00 bar, as TradingView
  flags them. No hash or layout moves (#293, 962960b3).
- **A calendar bucket completes once.** A day, week or month bucket closes on
  its period's last input bar, and a later bar of the period merges into it
  without completing it again: a week holding a session its template does not
  declare (NSE's Budget-day Saturday and Sunday sessions) completed again on
  each of that session's bars. With a daily feed installed, a day whose
  session opens late -- after the day stamp and before its scheduled close,
  as NSE's Muhurat session of 2025-10-21 opened at 13:45 IST -- starts the
  intraday grid of every width at that open (`native_late_day_opens`,
  `TimeframeAggregator::set_native_day_opens`,
  `NativeDayPartition::late_opens`) (#296, 7239ab37).
- **A daily feed dates the day bars it covers.** When an intraday input
  aggregates into "D" script bars and a daily feed is installed (the feed
  store's, or a declared series' authoritative bars), each daily stamp labels
  the D bar holding the session instant it covers: OANDA:XAUUSD's bar is
  labelled at its 17:00 ET stamp, not at the session's 18:00 ET open. Only the
  label moves; the bar's span, closes and values stay the calendar's
  aggregate. On a 1D script a C host's `pf_native_decision_v1`
  `session_day_ordinal` is the trading date of the bar's first in-session
  instant when that is later than its label (#298, 35db01c8).

### New C ABI exports

- **Another symbol's data for `request.security`.** `strategy_set_symbol_feed`,
  `strategy_set_symbol_feed_column`, `strategy_set_symbol_facts` and
  `strategy_set_recorded_series`, each behind its own `PINEFORGE_HAS_*_V1`
  probe, install another symbol's bars (each with its own close), their named
  columns, the symbol's `syminfo` facts and recorded request series before a
  historical run. A `request.security` of another symbol then runs on that
  symbol's bars in the requested context -- history, TA, `bar_index`,
  `time_close` and `syminfo.*` are the requested symbol's -- where only the
  run's own symbol was addressable; pineforge-codegen 1.0.0 lowers
  such a site (its b8197fcf). An invalid symbol reads `na` under
  `ignore_invalid_symbol` and stops the run without it, a symbol with no feed
  stops the run naming it, an aggregated chart is refused, and a feed or
  series installed with `n == 0` reads `na`. Each setter answers -1 during a
  run or on a host with no source layer, and `stream_begin()` fails while a
  symbol feed or a recorded series is installed. A C++ host installs another
  instrument's bars with `NativeRunSpec::instrument_feeds` and reads them
  through a `NativeSeriesSource::InstrumentFeed` series; neither has a C
  spelling (#292, 7f807b96).
- **`strategy_declares_bar_magnifier`.** A strategy whose `strategy()` call
  declares `use_bar_magnifier = true` exports it (it returns 1; every other
  `.so` lacks the symbol), so a host can run the script on a finer input with
  the magnifier on, as TradingView backtests it (#294, 74491fa1).

### Behaviour that moves results against 0.13.1

These change a strategy's trades or report values; each commit states the
TradingView behaviour it matches.

- Pine v6 `strategy()` defaults are TradingView's current ones: a v6 script
  that omits `initial_capital`, `default_qty_type` or `default_qty_value` runs
  with 100000, `strategy.percent_of_equity` and 100 (100 whatever the type:
  100 contracts under `strategy.fixed`, 100 of the account currency under
  `strategy.cash`), which pineforge-codegen 1.0.0 (78715160, after its
  v0.10.4) declares in the generated constructor. Code generated by an earlier codegen and a hand-built
  `PineStrategyConfig` keep 1000000, `strategy.fixed` and 1. To keep a v6 script's old sizing, declare
  the three in its `strategy()` call and transpile it again.
- `str.tostring` / `str.format` render numbers by the rule TradingView's tapes
  pin (3872c46a, #287).
- Exact quantities: a Transact that crosses the book is charged exactly its
  units, a close that spans lots and ends inside one exactly its request, and
  a close whose binary64 sum reaches its request exactly at a lot closes that
  lot whole (deb344b1, 99cf9252, 1e9e0761; #287).
- Indicators: `ta::ALMA` takes the floor input, `ta::KC` / `ta::KCW` take
  use_true_range, KC's middle band is its source's EMA on every bar,
  `ta::AnchoredVWAP` restarts on a caller-supplied anchor, and
  `ta::PivotPointLevels` computes an anchored period's pivot levels
  (f2ae16c7, de612473, 24c173ed, be41c1bb, 1eef78eb; #281); the six-argument
  `ta::pivot_point_levels` overload computes every type (adb8f2b6, #287).
- `ta.supertrend` runs Pine's reference from bar 0: its direction is 1 while
  the ATR warms up and its line is 0 on bar 0, where both read `na` until the
  ATR was valid, and a recalculated bar advances its ATR once.
  `ta.percentile_nearest_rank` and `ta.percentile_linear_interpolation` keep
  TradingView's persistent window, so an `na` in it no longer upsets the
  order; linear interpolation reads rank p/100 × n − 0.5 (25 % of
  {20, 30, 40, 50} is 25, not 27.5); `ta.percentrank` reads an `na` value or
  an all-`na` lookback as 0. `color.t` reads a whole transparency back as set
  (`color.new(c, 90)` read 91), and `color.red`, `blue`, `black`, `yellow`,
  `silver` and `teal` are Pine v6's (#F23645, #2962FF, #363A45, #FDD835,
  #B2B5BE, #089981) (#295, 9e6196ca; #297, 0411892c).
- Sessions: `session.isfirstbar` / `session.islastbar` are the kernel's
  session-day facts, a session ends at the session day, and an aggregated
  chart and a stream's realtime bar read `session.islastbar` as TradingView
  does (c68fab7e, b531a8d9, 8b843499, be19463d).
- `session.ispremarket` / `session.ispostmarket` read every window of the
  session: pre-market runs from 04:00 to the session day's first open and
  post-market from its last close to 20:00, a break between windows is
  neither, and an overnight or 24-hour session has neither; they read the
  in-market predicate's day list. On an extended-hours chart
  `session.isfirstbar` / `session.islastbar` mark the chart's session day, its
  pre- and post-market bars included, and `session.isfirstbar_regular` /
  `session.islastbar_regular` the regular session's, which
  pineforge-codegen 1.0.0 reads (its fdcdcbb9); the two pairs were one
  (#293, 962960b3; #295, 9e6196ca).
- `time()` / `time_close()` and the session predicates read a session
  argument as TradingView does: each `HHMM` is minutes after midnight,
  unchecked, so a `"0000-2400"` window, which was skipped, is the whole day; a
  session of several windows and no day list trades Monday to Friday, and a
  day list filters each window by its session day; under a D, W or M
  timeframe the bar is the session's own day, week or month in its timezone
  (the explicit one, else `syminfo.timezone`), and under an intraday one the
  bars are cut from each window's open; `time_close("D")`, `("W")` and
  `("M")` are the exact boundary, not one millisecond before it; a 24-hour
  session's day runs wall clock to wall clock across a daylight-saving
  switch. A D, W or M chart bar's `time_close` is its period's last traded
  close (OANDA:XAUUSD 1D's bars, stamped in the session's break, close at the
  next 17:00 ET), and an intraday chart bar's closes on its session's grid
  (NASDAQ:AAPL 60's 15:30 bar at 16:00). Exchange holidays and early closes
  are not modelled (#295, 9e6196ca).
- Pine v6's `time()` / `time_close()` with `bars_back` and
  `timeframe_bars_back` read another bar through
  `PineStrategyHost::pine_time_offset`, which pineforge-codegen 1.0.0
  lowers them onto (its 70c2b4af; 0.10.4 ignored both arguments and read the
  current bar's time): a
  past bar from the chart's history, a bar ahead from the input the run holds
  and then the symbol's session calendar, closed days included, and a
  timeframe's bar stepped on that calendar (#297, 0411892c).
- A margin call never revives an exit the script cancelled (4b00da92, #287),
  whether the exit was live, dormant, or a gapped stop a declined reversal
  had parked for the next margin call (#289).
- Margin: a leveraged position (margin below 100 %) is checked on the rest of
  the bar its fill was on, from the fill to the extreme it was heading for
  (the low for a long, the high for a short), so its margin call lands where
  TradingView books it, not a bar later or never. This holds for an opening
  and for an add to a carried position, under `process_orders_on_close` and
  `calc_on_order_fills` too; a timestamped FX curve keeps its own schedule. A
  default `percent_of_equity` stop entry sized above 100 % keeps the quantity
  it was sized at when placed (at the signal close, or at the snapped stop
  level when not yet marketable), as TradingView does; it was re-sized at the
  fill. A gap-open stop entry's margin slice is sized by the rule every other
  margin call takes, its one-contract minimum included. Under the bar
  magnifier, the margin call of a leveraged long, or of a short at any margin,
  lands at the first lower-timeframe price that crosses its line, and again at
  each later one that crosses the reduced position's line, under
  `calc_on_order_fills` too and, for a long, under `process_orders_on_close`.
  A margin call books the nearest tick of the price it fired at, as
  TradingView's market fill does; stop and limit fills keep their directional
  rounding.
- Margin at placement and fill: an add to a position of its own side is
  judged on its combined margin with no lot of slack, and a default
  `percent_of_equity` add is dropped at placement when that exceeds the
  equity; a bar's same-side market entries from flat all fill, and adds
  placed on a held position are each judged beside the position held when
  they were placed, the grown book then called once after the last fill, four
  times its lot-floored shortfall; a default-size reversal of a position
  opened at the same open must fit with that position still margined (without
  slippage); a default `percent_of_equity` short with slippage opens from flat
  only while its units fit the equity at the signal close's tick; an
  explicit-quantity market buy from flat is costed at its signal close's tick
  plus its slippage; under `process_orders_on_close` a gapped default-size
  stop entry the equity cannot pay for at the open is refused and gone; and a
  percentage of equity floors the order's money, not the equity rounded first
  (#294, 74491fa1; #295, 9e6196ca; #297, 0411892c).
- Margin calls: a `process_orders_on_close` short is margined at its signal
  close, a carried one is called on its path before the script runs, and a
  commissioned opening at the next open; a slipped market short's opening
  call, and the call on a bar's market adds to a long, are sized at the print
  the fill slipped from; a call the bar's close owes is taken there after the
  script, a `strategy.close_all` placed on that bar keeping its size, and
  `strategy.cancel_all` withdraws it, the call then placed again behind the
  script's orders; on whole-unit lots a call is followed by TradingView's
  one-unit follow-ups, a sub-unit restore takes its unit only where that unit
  restores the book at its fill, and a short slipped on whole lots is checked
  at every point of its bar's path; a call that closes several lots of a long
  takes the broker's follow-up, and under a percent commission any call is
  followed and an explicit-quantity short is checked from its entry bar's
  opening print; under `process_orders_on_close` a one-lot full-margin long's
  one-unit money call no longer depends on `pyramiding`; a `qty_percent` exit
  sizes on its entry's filled quantity, the units margin calls took included,
  and after a call the exits share what remains in the order they were
  created (#294, 74491fa1; #295, 9e6196ca; #297, 0411892c).
- A trailing `strategy.exit` placed while its entry is still pending starts
  its running best at the activation, as TradingView's does; on TradingView's
  tapes it exited 1 to 24 bars late.
- `calc_on_order_fills`: after a resting order fills at its own level on a
  bar's high or low, the orders its recalculation places act from that point,
  as TradingView does: a market order, a `strategy.close`,
  `strategy.close_all` or a market `strategy.order` fills there, and an exit
  stop the price has already passed fills there. An exit limit placed on such
  a fill at the bar's first extreme is live at its own level on the leg to
  the second; it was forced onto the second extreme. An order an earlier fill
  of the same bar pushed onto that point still moves on -- to the other
  extreme from the first, to the close or the next open from the second --
  and a fill booked at an off-grid extreme's tick counts as a fill at that
  extreme. Under the bar magnifier, a market order placed after such a fill
  at an intrabar's second extreme fills there too.
- `calc_on_order_fills`: what the recalculation of a fill inside a leg of the
  bar's path places, cancels or closes acts from the end of that leg -- an
  entry already executable fills at the next extreme's print, a close fills
  there with or without `immediately = true`, and after a cancel or a full
  close an exit the rest of the leg reaches still fills at its level; every
  exit one path point triggers fills there before that point recalculates; an
  exit re-issued unchanged keeps resting at its level, and a resting entry's
  bracket stop re-issued at its level stays live there; an exit stop through
  the first open fill, or an exit limit at or through the open print, that the
  fill's recalculation places fills there on the entry bar, and one a later
  open fill's recalculation places is live from the bar's first extreme; after
  a fill forced onto a leg's end the recalculation's priced entry is live from
  TradingView's next fill point, with the bar magnifier the next point of its
  intrabar path; and `timeframe.change()` compares every calculation of a
  recalculated bar with the bar before it. A `strategy.close` /
  `strategy.close_all` with `immediately = true` that the bar's calculation
  executes is final for the bar: no recalculation follows it, and an entry
  sent behind it fills at the next open (#294, 74491fa1; #297, 0411892c).
- `process_orders_on_close`: a stop entry whose stop the placing bar's close
  already reached, and the bar's only entry marketable there, fills at that
  close (slippage applied), on the chart, an aggregated chart and under the
  bar magnifier, and so does one placed while the position still holds the
  other side, once a `strategy.close` on the same bar has closed it; each
  filled at the next open. Such an entry placed again at the close of the
  bar whose `strategy.exit` limit closed the previous one keeps the fresh
  bracket it was placed with; the previous bracket's fill withdrew it at the
  next open, and the position ran on without an exit. A limit entry placed at
  a bar's close fills at that close when the close's tick reaches its level,
  with or without `calc_on_order_fills`; one only the bar's range reached
  still fills on the next bar's touch. It filled a bar late.
  An add can no longer exceed `pyramiding` because an opposite entry was
  resting from an earlier bar. The corpus probe
  `order-deferred-flip-pooc-cross-bar-01` now books all 792 of its
  TradingView tape's trades as TradingView does.
- `process_orders_on_close`: the close fills the calculation's market orders
  first, then the priced adds and reversing limits its tick reaches, and then
  decides every exit the calculation placed against that tick, the exits
  placed with the entry that just filled included (a stop fills slipped, a
  limit unslipped, at most one leg per exit order); an entry the script calls
  after a same-pass `strategy.close_all` or whole-position `strategy.close`
  is judged against the position the close flattens; a bracket placed after
  the close's paired entry survives that flat; an exit re-issued while its
  entry holds no lot is that entry's new exit; and a default entry takes the
  sizing its bar's margin call revised (#294, 74491fa1; #297, 0411892c).
- An aggregated chart under the bar magnifier dates every fill at its chart
  bar's open, as TradingView does.
- The bar magnifier walks TradingView's own intrabars, built from the finer
  feed you supply (`input_tf` finer than the chart): the intrabar timeframe
  is TradingView's table's (a 15-minute chart walks 2-minute bars, 30 minutes
  5, 1 hour 10, 4 hours 30, a daily chart hourly bars), each intrabar starts
  on the session day's grid and belongs to the chart bar that holds its last
  minute, and the chart bar's own open and close are fill points. Stops,
  limits, margin calls and `calc_on_order_fills` refills fill where
  TradingView's magnified broker fills them. A 1- or 5-minute chart needs
  10- or 30-second bars, which a 1-minute feed cannot build, so it still
  walks its input bars; an exchange-listed daily chart's bar is still the
  aggregate of its minutes, where TradingView prices it from its daily feed.
- A lower-timeframe bar (`IntrabarPath::lower_tf`) with a leg of zero length,
  such as an open at its own low, is walked through its own four turning
  points, and a bar with volume 0 at a single price is one point, so margin
  checks and fills see only prices the bar printed, where the sampler filled
  in a price the bar never printed.
- A closed trade's run-up and drawdown (`strategy.closedtrades.max_runup` /
  `max_drawdown` and the report's columns) come from the kernel's per-lot
  sampler for Pine strategies, as for every other host. In 19 corpus probes
  the excursions of 335 trades move: 324 now equal TradingView's and 11 are
  closer.
- Under FIFO closing, a default `strategy.exit(from_entry)` reserves its own
  entry's quantity, oldest lots first, not the whole position, as
  TradingView does; where it closed lots of other entries, the trade rows
  split differently.
- The `strategy.exit` orders of one entry queue in the order they were
  created: a re-issue keeps its place and a later exit gets only what the
  earlier ones leave, so of two exits without `qty` only the first created
  fills; exits for every entry (`from_entry = ""`) armed before the position
  opens queue alike. An exit whose entry has neither an open trade nor a
  waiting order is void and does not wait for a later entry of that id. A
  `from_entry = ""` exit without a quantity sizes on the whole position when
  it fills, the lots a later add opened included. A percentage exit keeps one
  quantity step on a fractional lot grid too. A priced exit that fills on the
  bar a market add of its own id opened covers that add only when its own
  fill closed an older lot of that id, and a `qty` exit of an entry a same-bar
  reversal is still placing waits for that entry to open (#294, 74491fa1;
  #297, 0411892c).
- `strategy.close(id)` without `qty` sizes from TradingView's close ledger,
  the units entered under `id` that no fill has booked yet, with or without
  `process_orders_on_close`: every fill that reduces the position books the
  ledger (a close against its call site's first id, an exit against its
  `from_entry`, anything else oldest first), what an id cannot cover spills
  over the other ids' units in fill order, a call site's later calls on a bar
  re-size its one order, and a call whose id has nothing left places nothing
  (#294, 74491fa1).
- A `percent_of_equity` default quantity under a cash commission sizes from
  `strategy.equity`, with the open entries' fees charged, and keeps back the
  fee its own order pays, as TradingView does: per order
  `floor((pct × equity − fee) / (price × point value))`, per contract
  `floor(pct × equity / (price × point value + fee))`. It used to size too
  large. The corpus probe `order-percent-equity-cash-commission-01` now books
  355 of its tape's 366 quantities within TradingView's 0.0001 cell (none
  before).
- `strategy.netprofit` and `strategy.equity`, and so
  `strategy.netprofit_percent` and `strategy.openprofit_percent`, charge the
  entry fees the open lots have paid, as TradingView does; `strategy.openprofit`
  stays gross. A script that sizes from `strategy.equity` with a commission
  declared and a position open sizes smaller. `PineStrategyHost::current_equity()`
  and `net_profit()` read so, `closed_trade_equity()` keeps the old figure,
  and `BacktestEngine`'s accessors, the report and the C ABI do not change
  (#297, 0411892c).
- Reversals and same-bar entries: an entry reversal voids the reversed
  side's `from_entry = ""` exits, and under a declined reversal such an exit
  stays the held position's; at Pine v6's all-in defaults a
  `strategy.close_all` placed after a reversal entry belongs to that
  reversal, and a protective stop the open is already through fills there
  when the open declines the reversal (#294). Opposite entries marketable at
  one fill point from flat fill in TradingView's order -- buy market, buy
  stop, sell market, sell stop, buy limit, sell limit -- the later one a
  single transaction of its own quantity plus the pending market's, admitted
  at the signal close without its commission and dropped past the equity,
  under `pyramiding`, `process_orders_on_close` and `calc_on_order_fills` too;
  same-side lots at one point fill market, then stop, then limit (#294); a
  flat pair of opposite fixed-quantity market entries over the equity keeps
  only its earlier call (#297). At one opening price, outside
  `process_orders_on_close` and `calc_on_order_fills`, the market orders fill
  before the buy limit entries the open reached, lowest limit first, and a
  same-bar re-issue of a reversing market entry moves only its own quantity
  (#294). A pending limit entry does not count against `pyramiding`, and a
  flat limit entry of a fixed default quantity is checked against the equity
  at every call (#294). A default all-in entry on whole lots whose units cost
  more than the equity rounded to ten significant digits is dropped from flat
  and keeps only its closing leg as a reversal, whatever the next open does
  (#297) (74491fa1, 0411892c).
- `strategy.margin_liquidation_price` reads `na` when margin calls are
  switched off (`set_margin_call_enabled(false)`, which TradingView has no
  counterpart for); it used to return a price.
- A `ta.*` call whose length is neither a constant nor an input compiles and
  computes as TradingView does, with pineforge-codegen 1.0.0
  (29bfd210, after its v0.10.4; it refused every such call): a simple length -- fixed for the run, such as one derived
  from `syminfo` or chosen by an input -- gives exactly the constant length's
  answers; a series length re-windows `ta.highest`, `ta.lowest`,
  `ta.highestbars` and `ta.lowestbars` at every call; `ta.supertrend` keeps
  the factor of its first execution. A length of 0, a negative length or
  `na` stops the run on the bar TradingView stops it. A series length for `ta.sma`
  and the other window averages is still refused, and one for `ta.rma`,
  `ta.ema`, `ta.rsi`, `ta.atr` and the others TradingView refuses too.
- A `request.security` of a timeframe finer than every feed the run was given
  (a "5S" request beside a 1-minute feed) reads `na` on every bar, as
  TradingView reads a timeframe it holds no bars of, instead of stopping the
  run with "requested timeframe '5S' is finer than input '1'"; supply bars of
  that timeframe to read values. A chart fed only its own bars still refuses
  it, so a caller can retry with a finer feed.
- A `request.security` of a finer timeframe with `lookahead_on` reads the
  requested bar that opens at or before the calling bar's time, as on
  OANDA:XAUUSD's daily bars, stamped 17:00 ET inside the session's break
  (#294, 74491fa1). An N-day timeframe (`"2D"`, `"8D"`) groups the symbol's
  trading dates N at a time from each year's first, where it read one day;
  and on a D, W or M chart stamped in its session's break a request of
  another symbol reads the chart bar's own period (#297, 0411892c).
- A daily run with an auxiliary feed no longer stops on a session held after
  the regular close, such as NSE's Muhurat sessions (2022-10-24,
  2023-11-12), with "native chart feed trading-period identities must be
  unique and strictly increasing with an auxiliary security feed"; each such
  bar keeps its own slice of the auxiliary bars.
- `strategy.risk.max_intraday_loss` does not count a fill at the day's first
  tick -- a carried order closing a position at that open -- as the day's
  loss (#294). A script's own `strategy.risk.max_intraday_filled_orders`
  counts as TradingView does: an entry `pyramiding` turns into a no-op is no
  fill, a `strategy.close` that fills is one, the cap's close for a
  `process_orders_on_close` fill is taken at the next open, and a close's
  slot passes to an opposite entry only when the script placed that entry
  before the close (#295). The corpus probe `cap-gatekeeper-intraday-risk-01`
  now books all 302 of its TradingView tape's trades exactly (#294,
  74491fa1; #295, 9e6196ca).
- A higher-timeframe series completes each calendar bucket once: on NSE:NIFTY
  a requested weekly series no longer advances again on the bars of a weekend
  session its template does not declare, and with the daily feed installed a
  late-opening day (NSE's Muhurat session of 2025-10-21) starts its intraday
  buckets, `time("60")` and `timeframe.change("60")` at its 13:45 IST open
  (#296, 7239ab37). A daily chart run magnified on a finer feed dates its bars
  and orders by the installed daily feed's stamps -- OANDA:XAUUSD's 17:00 ET --
  not by the session's 18:00 ET open (#298, 35db01c8).
- A `strategy.entry` quantity of +Infinity or -Infinity trades the strategy's
  default quantity, as `na` does on TradingView (#297, 0411892c).

### Performance

- A `calc_on_order_fills` strategy that draws labels, lines, boxes or
  linefills no longer slows down with every drawing it has made: the
  checkpoint of the script's state each fill recalculation takes shares the
  drawings between its copies instead of copying all of them, so the run's
  time grows with its bars, not with their square. What it books and draws
  does not change.

### Reports and tools

- The run's provenance -- the `strategy` block of `run_strategy.py
  --fingerprint-json` and of the Docker image's `fingerprint` -- records the
  `strategy()` values the generated constructor declares (capital, order size
  and type, commission, slippage, pyramiding, `process_orders_on_close`, the
  close-entries rule), then the run's overrides. It recorded the defaults for
  every script, so a fingerprint of a run whose script declares other values
  changes; the trades do not.
- `run_strategy.py` installs a strategy's pinned request data when
  `PINEFORGE_REQUESTS_ROOT` is set and `<root>/<strategy directory
  name>/requests.json` exists: the manifest (`pineforge-probe-requests/v1`)
  names other symbols' feeds, recorded tapes and symbol facts under that
  directory's `files/`, each checked against its sha256, and the run's
  provenance records each feed's and tape's key, sha and length and the
  manifest's sha. A malformed manifest stops the run before any engine call,
  and `--runner docker` refuses one; unset, the run is what it was (#292,
  7f807b96).
- `run_strategy.py` runs a strategy that declares `use_bar_magnifier = true`
  magnified, as TradingView backtests it, on the 1-minute feed
  `PINEFORGE_RUN_MAGNIFIER_FEED` names (held to
  `PINEFORGE_RUN_MAGNIFIER_FEED_SHA256` when set) unless
  `runtime_overrides.bar_magnifier` is false: on an intraday chart coarser
  than 1 minute, and on a daily chart whose daily bars that feed rebuilds
  exactly (the chart's own bars then date the days). Weekly, monthly and
  multi-day charts, a run with an auxiliary `request.security` feed and one
  with no feed log `declared-not-run`; `--runner docker` never reads the
  declaration (#294, 74491fa1; #298, 35db01c8).
- On a strategy directory whose `metrics.json` records TradingView's returned
  range, `run_strategy.py` opens the engine's window on TradingView's first
  bar when the loaded feed starts there, so an order placed before
  TradingView's first entry can make it; the rows it reports keep their
  window. It declares `syminfo.mincontract` from `PINEFORGE_VERIFY_QTY_STEP`
  when that is a positive finite number and the strategy declares none
  (`--runner docker` passes no `syminfo` metadata) (#294, 74491fa1).
- `scripts/symbol_calendar.py` writes a `pineforge-symbol-calendar/v1`
  document of a symbol's session days from a TradingView tape, and
  `run_strategy.py` hands the run's symbol the calendar
  `PINEFORGE_RUN_SESSION_CALENDAR` names (held to
  `PINEFORGE_RUN_SESSION_CALENDAR_SHA256`; another symbol's is refused) as
  `syminfo` metadata. The Pine host installs it
  (`PineStrategyHost::set_symbol_calendar`) and reads the bar after the
  current one from it -- `time()` / `time_close()` with a bar ahead, a
  timeframe's bars stepped, `session.islastbar` and
  `session.islastbar_regular` -- holidays included, and the fingerprint
  records it (`runtime.session_calendar`) (#298, 35db01c8).
