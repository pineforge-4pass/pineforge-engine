# Report channel contract, revision 1

Status: PROPOSED for engine 1.8.0, covering the report channel contract and gaps 4 and 5 in the resource-control audit of the engine at 95f4fd1f. This document specifies engine and hosting work; it does not declare that work implemented.

`EM:path:line` identifies engine commit `95f4fd1f408bf0ded941ecc4144b278a026a677f`, with a one-based source line. All descriptions of existing implementation below refer to that commit. The design follows the separation of transport negotiation, native modes and compatibility used by `docs/design/o1-report-contract.md`.

## Purpose and scope

A host needs an intact report, bounded diagnostics, and an authoritative account of delivery and resource refusal. A report-looking string or a process exit code alone cannot provide all three. This contract puts publication and terminal authority outside native execution, meters both report representations and diagnostics, and specifies failure at every publication boundary.

The threat model is an arbitrary native strategy sharing the Python producer's process, address space and native file descriptors. It can print, call system calls, corrupt that producer, invent report or error data, exit, close or duplicate descriptors, and attempt to fork. The host, its supervisor, its isolated sink validator, their installed code and the operating system are trusted. The strategy receives no capability to write the external report or terminal channel, inspect trusted processes, or modify their files. This establishes authentic transport and measured boundary outcomes; it cannot prove that arbitrary native code computed truthful trading results.

Scope includes direct backtest `run_json.py` invocations and the container entrypoint's preparation, compile and backtest stages. Capabilities and validation-only JSON use the same publication boundary when negotiated. Help, `csv2bin`, transpile-only text and an explicitly requested external `--run-phase-fd` are separate protocols and are refused under this negotiation. Internal selected-window phase tracking remains available; adding an external progress relay needs its own bounded contract.

Non-goals are changing trading semantics, implementing a SUMMARY_ONLY/STREAMED provider, changing the sink grammar, defining refund policy, or completing the independent heap, CPU, scratch, inode and descriptor admission designs. The finite lifetime and storage reservations below are necessary for this channel contract; a byte limit is not a whole-process memory bound.

Every normative requirement has an identifier R01–R26 and linked outside-in tests. The detailed rules following a requirement belong to that requirement. All new names, limits, transport choices and policy defaults in the proposed contract are **PROPOSED**, not existing constants or registered codes. Existing values are identified separately with EM citations.

## Current behaviour at EM

### Producers, profiles and modes

The entrypoint advertises JSON on stdout and logs on stderr, but accepts caller C++, links it into a shared library, and invokes the Python runner. That runner loads the library with `ctypes.CDLL` before its ABI/export checks and calls native execution directly. Thus the shown runner and strategy share the destination of native fd 1; there is no process boundary between these calls. (`docker/entrypoint.sh`, `docker/run_json.py`.)

| Request | Harness fd 1 | Other report representation | Source |
| --- | --- | --- | --- |
| Unjailed, omitted mode or explicit FULL, current sink API, no explicit sink | Full JSON reconstructed from a temporary native sink | Temporary path containing sink-v1 NDJSON | `docker/run_json.py` |
| Unjailed with an older module lacking the sink API | Legacy full JSON from native report arrays | No native sink | `docker/run_json.py` |
| Unjailed, `--report-sink PATH` | Summary projection with a sink reference | Caller-named, persistent sink-v1 path | `docker/run_json.py` |
| Either profile, `--report-sink-fd N` | Summary JSON without trades, equity array or child-local sink path | Sink-v1 on inherited N; the Python owner closes N after capture | `docker/run_json.py` |
| Jailed, no explicit fd sink | Full array report; no automatic native path sink | Resident UTF-8 JSON preparation | `docker/run_json.py` |
| Jailed with a path sink or spill directory | Failure JSON | Request refused | `docker/run_json.py` |
| Either profile, explicit SUMMARY_ONLY or STREAMED | `report_mode_unsupported` failure with mode and `pineforge-report/v1` args | Refused before library load/execution; this ctypes host has no provider bridge | `docker/run_json.py` |
| Selected-window execution | The corresponding full/path-summary/fd-summary projection above | Same sink selection; separate observed-phase handling | `docker/run_json.py` |
| Capabilities | One capabilities JSON; the library is loaded, no strategy constructed | No execution sink | `docker/run_json.py` |

Native FULL, SUMMARY_ONLY and STREAMED envelopes are distinct from those Python projections. The schema describes their retention and sink conditions, while the design explicitly warns that a C/ctypes host needs a negotiated provider; a header or sink API is insufficient. (`docs/pages/report-schema.md`, `docs/design/o1-report-contract.md`.)

Both Python profiles retain the shown stdio-writing paths: `write_failure` writes/flushed stdout, `_write_report` publishes success there, unexpected exceptions print a traceback to stderr, and argument errors print stderr usage plus stdout failure JSON. The profile selects scratch admission and report storage, not a replacement output destination in these paths. (`docker/run_json.py`.)

The log helpers write `[INFO]`, `[WARN]` and `[ERROR]` messages to `std::cerr`, without a byte counter or cap in the helpers. The shell emits progress and transpile errors on stderr; its compiler invocation has no explicit stdout redirection. Native writes to fd 1 can therefore interleave with or precede/follow the Python report in either profile. This last corruption scenario is a consequence of the shared descriptors, not a measurement performed for this document. (`include/pineforge/log.hpp`, `docker/entrypoint.sh`, `docker/run_json.py`.)

### Publication, limits and existing failures

Normal success is serialized completely before publication and native cleanup precedes `_write_report`. Jailed/fd mode uses `TextIOWrapper(BytesIO())`; other paths prepare a file before publication. The writer transfers up to 1 MiB per iteration, checks artifact-branch short writes, flushes, and returns 1 on a handled publication error without deliberately appending failure JSON to a success prefix. `_discard_stdout` then duplicates `/dev/null` over stdout; it is an error-discard operation, not pre-load strategy isolation. A subsequent completed-phase handoff can still fail after the full report has been written. (`docker/run_json.py`.)

The native writer bounds each sink line to 1 MiB including LF and exact JSON integers to ±(2^53−1). Its header, records and trailer are multiple JSON values, not one report document. The shown append/count checks do not impose a total-byte limit. Stream writes retry positive partial writes and EINTR, use `MSG_NOSIGNAL` for sockets, and wait on EAGAIN using `poll(..., -1)`. A blocking write also has no timeout in this loop. (`src/report_sink_writer.cpp`.)

A successful native trailer follows checked flush/synchronization. Abandonment flushes retained records and fails without a trailer; destruction does not synthesize completion. The strict reader requires the trailer's counts/digest and EOF immediately after it. These are content-completion conditions, not proof that the subsequent Python JSON or host publication succeeded. (`src/report_sink_writer.cpp`, `docker/pf_sink.py`.)

The separate finalizer already has a bounded stream mode: `--out -` requires positive `--max-bytes B`, reads at most B+1, never forwards the overflow byte, and returns 8 with `complete=false`, `reason=sink_exhausted`, `sink_bytes=B+1`, `cap_bytes=B`. Its stdout remains provisional until exit 0 and receipt completion; receipt/cleanup failures can instead return 5. File mode permits an omitted cap, and a supplied zero is an actual zero-byte cap. Stream mode rejects zero. None of these meanings matches the proposed byte-cap convention in which zero means no caller limit. (`docker/pineforge_sink_finalize.py`.)

Other existing zero semantics also differ: the catalog explicitly describes zero command-scratch bytes as `scratch_configuration_invalid`, not unlimited. The resource-control audit of the engine at 95f4fd1f, Part C X1/X4, records the zero/no-count-limit policy as a proposal, not an implemented general byte-budget API. This proposal adopts that convention only for its new caller ceilings and leaves both existing interfaces unchanged. (`docker/run_failure_codes.json`.)

Failure JSON has an existing 16 KiB initial text cut, 1,024-byte string-argument cuts and a 60 KiB JSON-body target plus LF. Those bounds do not cap successful reports or total diagnostics. Ordinary `RunFailure` defaults to exit 1; argparse errors exit 2. The shell maps backtest failure to 4 and separately documents missing input 2, compile/binding failure 3 and transpile failure 5. (`docker/run_json.py`, `docker/entrypoint.sh`.)

The catalog is `pineforge-run-failure-catalog/v1`; code spelling, typed args, closed vocabularies and validation rules are documented. In particular, the registered `report_sink_failed` is **engine_fault**, retryable false, including planned sink exhaustion and closed peers. Its existing args and finalizer-8 mapping are preserved here. Existing report-post-execution phase/reason vocabularies do not include a generic new “output” phase. (`docker/run_failure_codes.json`, `docs/pages/run-failure-codes.md`.)

## Contract and negotiation

### R01 — Explicit activation and compatibility

The engine and host **MUST** negotiate `supervised/v1` explicitly and refuse an unavailable requested contract without executing a strategy or silently retrying legacy mode. Tests: T1, T2, T3, T36.

The new direct-run flag is `--report-channel supervised/v1`; the entrypoint equivalent is `PINEFORGE_REPORT_CHANNEL=supervised/v1`. Unset means OFF for existing local/direct callers. There is no automatic activation merely from `--profile jailed`, an arena, or a report-sink fd. A new `--terminal-status-fd M` / `PINEFORGE_TERMINAL_STATUS_FD` is required with activation. Only the literal supported version is accepted; new knobs without activation are usage errors, rather than ignored controls.

The hosted/billed 1.8.0 policy is ON and fail closed: the hosting layer supplies the contract and status endpoint for every covered run. An installation using the unchanged legacy route has not closed gap 5 in the resource-control audit of the engine at 95f4fd1f. Keeping the bare CLI default OFF avoids silently changing fd inheritance, platform support, limits and failure publication for existing callers. It is a migration compatibility choice, not an allowed hosted fallback.

Successful JSON bytes, key order, projections, LF and existing failure-object shapes remain unchanged for the same logical request and fixed nondeterministic inputs. The report transport version is independent of `pineforge-report/v1`, selected-window versions and sink version 1. SUMMARY_ONLY/STREAMED requests retain the current typed refusal until their provider is implemented; supervision does not advertise that provider.

### R02 — Authority outside the strategy

The hosting launcher **MUST** establish the trusted supervisor S and its isolation before any customer library load, native initializer, strategy creation, callback or destructor can run. Tests: T4, T5, T6, T7, T8, T22.

S is the only writer to the host's report and terminal endpoints. U is the untrusted native/Python producer process. V is the trusted sink finalizer in a separate process that never loads customer code. A Python redirect, saved descriptor, CLOEXEC, different PID or secret fd number inside U does not satisfy this requirement.

The chosen Linux boundary uses a separate unprivileged U identity, no capabilities, private PID and mount namespaces, and an OS policy denying inspection/control of S/V, their `/proc` descriptors, memory, filesystem, signals and IPC. S/V's writable work directory and external report/status endpoints are absent from U's namespace. U has no network or ancillary-fd route to acquire those endpoints. Installed supervisor/validator code is immutable to U. S/V and their reserved terminal-reporting resources live outside U's kill/OOM domain. An unavailable boundary is refused before load; the Python profile name is not installation evidence.

This specifies required isolation properties and the selected mechanism, without claiming that EM installs them. The hosting layer supplies the actual launcher policy and proof; the engine implementers supply its admission checks. Trusted loader/configuration errors established by S can be harness faults. A child assertion, signal, error string or alleged `harness_internal_error` is not such proof.

### R03 — Descriptors and ownership

S **MUST** open or validate the following channels, remove every external writing endpoint from U before its first instruction, and keep U's redirected stdio in place until U has exited and been reaped. Tests: T4–T9, T18, T23, T24.

| Endpoint | Transport and opener | Writer → reader | Lifetime |
| --- | --- | --- | --- |
| Report R | Host-created pipe or AF_UNIX stream, installed as S fd 1 | S → host | Exactly one JSON document on a successful covered request; close after publication |
| Diagnostic D | Host-created pipe or AF_UNIX stream, installed as S fd 2 | S → host | Metered raw diagnostic prefix, then EOF |
| Terminal M | Host-created connected AF_UNIX SOCK_SEQPACKET pair | S → host | One bounded packet, then EOF; never inherited by U |
| Optional external sink N | Existing caller fd stream, or caller's named target owned by S | S → host/artifact | Frozen sink-v1 bytes; never directly inherited/opened by U |
| U diagnostics | S-created private stream | U fd 1 and fd 2 → S | Both fd numbers duplicated onto the same diagnostic writer before load |
| Candidate JSON C | S-created private stream at a dynamically allocated U fd | U report owner → S | Untrusted candidate, never public stdout |
| Native sink K | S-created private streams at dynamically allocated fds | Native writer in U → S's bounded relay → V | Frozen sink bytes, counted by S before retention by V |
| Snapshot handoff | S-created private SOCK_SEQPACKET control pair | S → U, read-only fd via SCM_RIGHTS | At most one snapshot response for an internal full-report projection |

There are no new fixed fd numbers. R/D use the existing stdout/stderr convention. M, C, K and private control descriptors are allocated, passed explicitly and collision-checked. M follows the existing sink-number range of 3..INT_MAX excluding reserved arena fd 5; N retains its existing validation. The range/reservation comes from `docker/run_json.py`; the assignment of M is proposed.

S validates actual endpoint identity and direction, including duplicated aliases, rather than just unequal integers. R, D, M and N are distinct open channels; an alias is a pre-load usage refusal. An explicit external phase endpoint is also refused before load under this version, including aliases; it is not silently dropped or inherited. U stdin is `/dev/null` for a backtest. All inherited descriptors other than its explicit allowlist are closed; no external endpoint survives through a duplicate, environment path, working directory or writable mapping.

The whole entrypoint is launched below S, so shell, transpiler, compiler and binder stdout/stderr enter the same bounded diagnostic path. The runner's report functions use C explicitly. The trusted compiler stage may launch its normal helpers; U's native stage has the stricter process rule R18. Closing U fd 1 or fd 2 cannot close R, D or M in S. S never restores R onto U fd 1 at shutdown. Below, B denotes the sink-byte allowance, K the private sink transport, and P only the publication stall interval; U always denotes the untrusted process.

### R04 — One report, separate from native prints

S **MUST** publish at most one complete JSON document followed by LF, validate a candidate before publication, and route every ordinary native fd-1/fd-2 write solely to diagnostics. Tests: T4–T10, T17, T25, T35.

Candidate validation requires exactly one UTF-8 JSON object with the existing requested report/failure shape and trailing LF, no duplicate keys, non-JSON numbers, extra values, trailing payload or unterminated fragment. Existing success encodings are preserved, not parsed and reserialized into a different byte ordering. A valid candidate is still untrusted result data: U can intentionally write to C.

Both `_write_report` and `write_failure`, including parser/capability/validation paths, use C in U. Failure text never selects a host failure code. A valid failure candidate, including a legacy failure without code/args, may be relayed in its existing shape within the remaining report budget, while the terminal record identifies its producer provenance. A signal/nonzero U exit cannot be overridden by a success-looking candidate.

### R05 — All native sink routes are bounded

Supervised execution **MUST** interpose K/S/V on every native sink representation, including an automatically created internal sink, without changing its wire bytes or the caller's chosen JSON projection. Tests: T1, T11, T12, T13, T21, T35.

U does not open caller-named sink targets. A named sink is published by S; an inherited external N stays with S and U receives a different private K. S relays to V through a fixed-size queue and counts/hashes input independently of V; these observations survive V being killed without a receipt. For the unjailed automatic internal sink, V retains/strictly verifies the bounded sink in S's private area and S supplies U a read-only completed snapshot for the existing full-JSON assembly. The inode is owned by the trusted identity, is not writable/reopenable by U and is not subsequently modified. This replaces an internal temporary transport, not its JSON projection. A named projection retains its original caller-visible reference.

U closes its last K writer after native capture/receipt checks, before waiting for a snapshot or emitting candidate JSON. This explicit close is earlier than final strategy cleanup and is needed for V to observe EOF; native ownership remains borrowed. S sends the snapshot proactively after verification: one H-bounded packet `{"record":"snapshot","version":1,"sink_bytes":B_actual,"remaining_report_bytes":R-B_actual,"sha256":WIRE_HASH}` and exactly one read-only fd. The control channel is used only in that direction; S never reads child requests or accepts child-supplied fds/pathnames. U validates the response before its ordinary assembly. Native cleanup cannot emit additional sink bytes after that close. V failure closes the handoff and S terminates U instead of leaving it waiting.

Jailed execution without an explicit sink can keep its existing array route. A legacy module without any sink API can keep its array fallback. An explicit sink request always requires a verified sink; an automatic route with zero sink bytes may use the legacy array fallback. Once any K byte exists, a missing trailer is failure, never evidence that the sink was disabled.

### R06 — Exact bounded diagnostic policy

S **MUST** meter all diagnostic sources as raw bytes, retain/forward at most the first D bytes in aggregate, and fail the run on observing byte D+1. Tests: T4–T9, T14, T15, T16, T30, T33.

The budget includes U fd 1, U fd 2, Pine log prefixes/messages/LFs, loading and cleanup output, shell/compile/transpile/binder output, V diagnostics and S diagnostics. Terminal control records are separate. Both U stdio numbers share one stream, so the contract promises aggregate order as received, not independent stdout/stderr source counts or globally ordered concurrent writes.

S reads no more than the remaining allowance plus one overflow-probe byte. Byte D+1 is counted as observed but never retained/forwarded. There is no truncation marker added to raw diagnostics and no further drain-to-discard loop. S latches `diagnostic_budget_exceeded`, sets diagnostic `truncated=true`, closes its private readers and stops the producer domain. A strategy that attempts gigabytes therefore gets bounded captured output and a typed refusal, not success with silently missing logs. Buffered bytes not read before termination are not claimed as counted.

Partial lines, invalid UTF-8, NUL and stderr text resembling a receipt remain ordinary raw diagnostics. They are not line-buffered, parsed as status, or copied into terminal args. Exact fit D followed by EOF is allowed. A stopped diagnostic consumer follows R16's transport deadline; diagnostics cannot hold the report or terminal path indefinitely.

### R07 — Budgets, defaults and zero

S **MUST** resolve finite effective limits before starting any covered stage, enforce R and D at their trusted input boundaries, and record requested, granted and effective limits without silently clamping a positive request. Tests: T2, T11–T16, T21, T31, T32, T33.

| New policy item | Proposed default | Meaning and reason |
| --- | --- | --- |
| Aggregate report allowance R, `PINEFORGE_REPORT_MAX_BYTES` / `--report-max-bytes` | 67,108,864 bytes (64 MiB) | A finite initial policy, not a measured maximum or the audit's hosted 8 MiB observation |
| Diagnostic allowance D, `PINEFORGE_DIAGNOSTIC_MAX_BYTES` / `--diagnostic-max-bytes` | 1,048,576 bytes (1 MiB) | Enough for useful diagnostics; bounded even under native flooding |
| Terminal/control packet maximum H | 65,536 bytes (64 KiB), including LF | Separate reserved capacity so diagnostic/report exhaustion cannot suppress terminal status |
| Child failure-claim args retained in status | 8,192 encoded bytes | Bound untrusted metadata independently of authoritative status facts |
| Pending user-space queue per relay direction | 65,536 bytes | Fixed backpressure window, not additional retained report allowance |
| Whole-request wall limit W, `--channel-wall-ms` / `PINEFORGE_CHANNEL_WALL_MS` | 600,000 ms | External finite lifetime, including compile, native waits, verification and publication |
| Publication no-progress interval P, `--channel-stall-ms` / `PINEFORGE_CHANNEL_STALL_MS` | 5,000 ms | A stopped report/diagnostic/sink/status reader cannot wait forever |
| Stop grace G | 1,000 ms | Upper allowance for cooperative termination before forced domain kill |

For the new caller ceilings only, **0 means no caller-imposed limit**, following the proposed byte-cap convention. It does not mean a zero-byte allowance, disable capture, disable accounting, or remove the host's limit. Omitted values use the table; zero resolves to the host grant, whose defaults are the same finite R/D/W/P values. A positive request is accepted exactly if within the host grant; otherwise S refuses before work with `run_request_invalid{"option":"arguments"}`. CLI overrides the corresponding environment value. Empty, signed, whitespace, nondecimal and out-of-range values are refused. Values are decimal integers in 0..(2^53−2), using the existing safe-JSON bound as the representability ceiling, not as a recommended allocation.

Host grants for R/D/W/P are positive; zero/unavailable grants are `run_isolation_unavailable{"reason":"policy"}` before U. No standalone unlimited supervised mode exists. This is compatible with “zero = no caller limit” while keeping every billed run bounded.

The existing finalizer CLI is unchanged: S translates the resolved positive sink allowance into `--max-bytes B`. It never forwards a new zero ceiling as finalizer zero. If a certified/planned B is supplied through trusted host admission, it is positive and no larger than R; a larger plan is an admission `report_budget_exceeded` with channel aggregate, used 0 and requested B. With no certified B, B=R is a policy ceiling, not a claimed prediction or successful-fit guarantee.

### R08 — Report-byte accounting and refusal

S and V **MUST** enforce the following aggregate accounting before retention/publication, including header, trailer, JSON escaping and every LF. Tests: T11, T12, T13, T17, T21, T31, T35.

`report_charged_bytes` is the sum of unique accepted native-sink bytes, candidate JSON bytes and any replacement failure JSON generated by S. Re-reading a snapshot, moving an artifact or publishing the same accepted bytes does not charge that representation again. The internal sink is charged even when it is not exported. Thus an internal sink and its expanded full JSON are two representations sharing R; a summary and external sink also share R. Record counts, line size, disk padding and transfer chunk size are not substitutes for this cap.

Capture order is native sink, EOF/verification, then candidate JSON. The candidate read allowance is R minus accepted sink bytes, with one overflow probe. All producer failure paths also close their K reference before C, including failures before native capture; a sinkless request has no active K. S services queued K data/EOF before classifying simultaneous C readiness. Candidate data while K remains open is a candidate protocol refusal, not a second independently usable R allowance. U's serializers also check the resolved remaining allowance before growing a JSON buffer/file, but that cooperative guard is not the trusted enforcement boundary. OS reservations for U's memory/work area remain necessary for allocations preceding emission.

A native stream exceeding its positive B uses the existing finalizer-8 receipt and `report_sink_failed{"reason":"sink_exhausted","sink_bytes":B+1,"cap_bytes":B}`. S's relay observes at most B+1, approves at most B, forwards the one probe so V can emit its existing exhaustion receipt, then reads no more native bytes. If V cannot finish, S still has independent overflow evidence and can raise the same typed sink refusal; it records V's missing/incomplete receipt honestly rather than inventing component exit 8. A candidate or additional representation exceeding R uses proposed `report_budget_exceeded`: channel aggregate when native bytes have consumed any R, otherwise channel json. Neither publishes a successful document or artifact.

S can emit an existing-shape failure document only if its complete encoded bytes fit the remaining R. It charges that replacement too. Otherwise R carries zero JSON bytes and M carries the typed failure. This explicitly versioned failed-attempt behaviour avoids an unbounded emergency exemption. Diagnostic output never borrows from R or H.

### R09 — Storage and backpressure reservations

S **MUST** reserve bounded capture/control storage before work and keep queues bounded under slow or absent readers. Tests: T13, T14, T18–T21, T32, T33.

The chosen sink validator uses existing file-finalizer mode with mandatory B into a private target, not the caller's final name. Native sink and candidate artifacts together retain at most R logical bytes; a failure replacement also fits that ledger. Diagnostics retain at most D. Reserve these extents, H-sized control records, fixed relay windows, validator/parser working memory and filesystem allocation overhead before admission. The host records the backing and charged storage grant. It does not equate a logical-byte cap with allocated filesystem blocks or Python RSS.

No second full sink copy is required: verification, read-only snapshot and later host publication use the retained inode. S's candidate validation is streaming over bounded chunks; it does not accumulate an unbounded JSON tree. A validator or filesystem operation runs in a killable worker, leaving S's deadline/status loop responsive. Failure to reserve/control this storage is a pre-start `run_isolation_unavailable{"reason":"policy"}`; a later established sink/spool I/O failure keeps the existing sink I/O code.

### R10 — Terminal record and measured channels

S **MUST** produce exactly one `pf-run-terminal/v1` record for every handled terminal outcome, after data publication has stopped, using this bounded schema and no untrusted text as authority. Tests: T1, T10–T16, T18–T29, T31–T35.

M carries one compact UTF-8 JSON object plus LF in one SOCK_SEQPACKET message. No native or Python producer endpoint can reach M. S retries EAGAIN/EINTR only within the absolute deadlines; it never deliberately sends two packets. Successful packet delivery is atomic; a failed terminal write produces no valid record and is handled by the host under R14.

| Field | Proposed required content |
| --- | --- |
| `schema`, `version`, `contract` | `"pf-run-terminal"`, `1`, `"supervised/v1"` |
| `attempt_id` | Host-generated 128-bit random identity, 32 lowercase hex characters, bound to this launch; not an authentication secret |
| `invocation` | `run_json` or `entrypoint` |
| `complete` | True only for a successful fully delivered report and every requested sink; false on any refusal/failure |
| `exit_class` | `success`, `input_refusal`, `resource_refusal`, `producer_failure`, `transport_failure`, `harness_fault` or `aborted` |
| `failure` | Null on success; otherwise `{code,args,authority}` with `authority:"supervisor"` and the catalog-validated facts S established |
| `exit_status` | The facade exit S intends, checked against actual wait status by the host |
| `producer` | Actual wait exit/signal, stage, optional validated child failure claim and its `authority:"producer"` |
| `limits` | Requested/granted/effective R/D/W/P, actual B if used, H and G |
| `report_charged_bytes` | Unique bytes accepted against R |
| `channels` | Fixed entries `candidate`, `report`, `sink`, `diagnostic`, `status`, as defined below |
| `sink_verification` | Null if disabled; otherwise V exit, completion, seven counts, existing logical sink digest and finalizer version |
| `usage` | Fixed measured-usage receipt integration block specified by R12 |
| `cleanup` | `producer_reaped`, `domain_empty`, `artifacts_removed`, and `measurement_state` for each; no arbitrary pathname list |

Wire member names are fixed. `producer` is `{stage,exit_status,signal,failure,failure_claim_omitted}`: stage is prepare/compile/run/finalize/publish or null before any producer starts; exit_status/signal are integers or null according to actual wait evidence; failure is null or `{code,args,authority:"producer"}`; failure_claim_omitted is boolean. `limits` has `report_bytes,diagnostic_bytes,wall_ms,stall_ms`, each `{requested,granted,effective}` with requested null when omitted, and scalar `sink_bytes` (B or null), `terminal_bytes` (H), `stop_grace_ms` (G), `relay_queue_bytes` and `producer_claim_bytes`. `sink_verification` is null or `{exit_status,signal,complete,counts,sha256,finalizer_version}`; counts is the fixed E/T/O/V/K/X/H map and unavailable values are null. Each cleanup entry is `{value,measurement_state}`, with boolean-or-null value and measured/unavailable state.

The exit-class mapping is fixed: successful requested operation → success; trusted option/mode refusal → input_refusal; report/diagnostic budget, process, lifetime or isolation refusal → resource_refusal; `run_producer_failed` → producer_failure; channel/sink/canonical-value refusal → transport_failure; S/V's own `harness_internal_error` → harness_fault; host cancellation → aborted. These are terminal dispositions, separate from each code's preserved catalog class. `artifacts_removed` covers unused/provisional artifacts, not a requested retained successful output.

Each data-channel entry has `observed_bytes`, `accepted_bytes`, `written_bytes`, `limit_bytes`, `truncated`, `complete`, `source_total_known` and `sha256`. `observed_bytes` counts bytes actually read at the designated boundary, including an overflow probe. `accepted_bytes` counts retained/approved bytes. For externally published data, `written_bytes` counts successful write syscall return values, not requested lengths.

For `candidate`, observation is at C; its private producer write total is known from received bytes only after complete EOF, otherwise `written_bytes=null`. For `report`, observed/accepted bytes are the selected complete document approved for publication, while writes are S's external R writes. For `sink`, observation/acceptance is at S's K relay and writes are S's external N/artifact logical bytes; an internal-only sink has zero external writes. For `diagnostic`, observation is aggregated capture and writes are the forwarded external diagnostic prefix. `source_total_known=false` records an interrupted source or unread kernel-buffered data. No exact native attempted-write total is invented after a kill or overflow. `limit_bytes` is the remaining JSON allowance for candidate, R for report, B for sink and D for diagnostics.

`truncated=true` means known bytes were discarded, a cap overflowed, or an intended delivered representation was shortened; `complete` independently records validated EOF/full delivery. An absent optional sink is complete with zero bytes and no digest. Candidate SHA-256 covers accepted C bytes; report SHA-256 covers external R bytes; diagnostic SHA-256 covers the forwarded prefix. Sink `sha256` covers external bytes (null for internal-only capture), with additional `accepted_sha256` over approved K bytes and `logical_sha256` from a verified pre-trailer fold. Incomplete prefixes can have prefix hashes without asserting success. V's unavailable counts/digest after a kill are null with verification complete false, rather than fabricated zeros.

For `status`, observed/accepted/written bytes are the encoded packet length including LF, `limit_bytes=H`, complete/source_total_known are true and truncated is false. S computes that length to a stable value including the digits of these fields before the atomic send; the receiver confirms it against the actual message length. A received record therefore confirms those bytes, while a failed send has no self-certifying record. Its self-hash is null; no recursive digest is defined.

### R11 — Authority and typed failure provenance

S and the host **MUST** distinguish a producer's claimed failure from an independently established host/supervisor fault, including when the child claims an existing engine-fault code. Tests: T10, T22, T25, T26, T27, T29.

Normal child failure JSON can keep its existing code/args for compatibility. M records its validated claim under `producer`, not as authoritative `failure`. The authoritative outcome is `run_producer_failed` unless S independently established a more specific refusal. Unknown codes or invalid args are protocol failure, not newly minted catalog entries. Claims whose args exceed the proposed 8 KiB sublimit are omitted, with `failure_claim_omitted=true` and the candidate digest retained; required args are never replaced by a misleading empty object.

This narrows the older failure-page claim about scripts not forging `runtime.error` codes to its supported language API. Arbitrary native code shares the entire producer, beyond that API's guarantee. The API behaviour remains unchanged; the terminal contract makes no stronger refund claim from it. (`docs/pages/run-failure-codes.md`, `include/pineforge/log.hpp`, `docker/run_json.py`.)

A validated V refusal based on its own strict parser may carry the existing `report_result_invalid` it establishes. An actual V I/O/overflow outcome can carry the existing `report_sink_failed` mapping. `harness_internal_error{}` is reserved here for S/V's own established internal fault; it is never inferred from U's signal, missing phase, stderr, candidate text or nonzero status. Neither a native trailer nor a content hash authenticates the trading computation.

### R12 — Measured-usage receipt integration

S **MUST** collect usage for both successful and failed attempts before teardown destroys the evidence, preserve the fields under output exhaustion, and label unavailable or untrusted measurements explicitly. Tests: T14, T21, T26, T28, T29, T32, T34.

The following is a proposed transport mapping for the planned measured-usage receipt, not a claim that the receipt's final spelling or these measurements exist at EM. `usage` is `{version:1,stages,job}`. `stages` has fixed entries `prepare`, `compile`, `run`, `finalize`, `publish`; each entry and job is `{state,resources}`. Absent stages have `state:"not_started"` and the fixed resource cells with null quantities. Each resource cell is `{used,requested,limit,peak,unit,scope,source,state}`. Numeric values are nonnegative safe JSON integers or null; an unrepresentable observation is unavailable, never silently rounded. `state` is `measured`, `sampled_lower_bound`, `unavailable`, `producer_reported` or `not_started`. `unit` is bytes/count/ms/us; `scope` is stage_process/stage_domain/job_domain/attachment; `source` is wait4/cgroup/filesystem/fd_observation/channel/clock/admission/engine/none. These are closed vocabularies, not arbitrary strings. Stage/job state describes coverage; each cell states the quality of its own observations.

| Resource cells | Units / population |
| --- | --- |
| `wall`, `cpu_user`, `cpu_system` | ms for elapsed wall; µs for kernel-accounted CPU |
| `memory_rss`, `memory_private`, `memory_job` | bytes; process RSS/private and separately measured job/cgroup charge |
| `scratch_logical`, `scratch_allocated`, `work_logical`, `work_allocated` | bytes; logical extents and allocated backing kept distinct |
| `scratch_files`, `work_files`, `open_fds`, `processes` | counts with measured scope and peak quality |
| `arena_claimed`, `arena_capacity` | bytes; engine observations marked `producer_reported` unless independently measured |
| `report_bytes`, `sink_bytes`, `diagnostic_bytes` | trusted boundary byte counters, consistent with `channels` and the R ledger |

Requested/limit fields come from trusted admission, not child text. Kernel wait/cgroup measurements identify their process/stage/job scope; per-process peaks are not added and relabelled a simultaneous job peak. Sampling a descriptor count gives a lower bound on its peak. Missing private-memory/physical-storage telemetry is null with `unavailable`, never zero. A hard-killed U still gets S's available counters and explicit unknowns. Existing arena high-water diagnostics are attachment-lifetime observations, not private RSS or an OS-jail certificate (`docker/run_json.py`).

The fixed cell/stage set fits H without diagnostic messages or arbitrary key arrays. Measured-usage receipt additions require a versioned mapping or a proven fit within H; they do not silently expand the packet. Required authoritative failure/cap/byte facts are retained when optional producer claims are omitted.

### R13 — Ordering and the success predicate

S **MUST** apply this event order and issue success only when every listed condition holds. Tests: T1, T11, T18–T21, T23–T29, T35.

1. Host creates distinct data/status endpoints and attempt identity; S validates negotiation, policy, grants and reservations.
2. S installs isolation/stdio/descriptor rules before starting preparation or U. A setup refusal starts no customer code.
3. S drains diagnostics concurrently. U captures native output to K; S counts/relays and V enforces B, verifies trailer and EOF, and retains a private artifact.
4. U closes K before any snapshot wait. S returns a read-only verified snapshot only for a full internal projection. U serializes one candidate to C, performs cleanup, closes C/diagnostics and exits.
5. S reaps U and all stage helpers, confirms an empty native execution domain, verifies candidate completion/shape, V exit/receipt and budgets. No success report has yet reached R/N.
6. S publishes an external sink first, if requested, then R JSON, using deadline-bounded I/O. Named artifacts are still provisional at this point. Diagnostics finish or fail under their own bound.
7. S closes data writers, freezes usage/cleanup facts, sends M, closes M, and exits with the matching facade status.
8. Host validates the record, EOF, bytes/digests and actual S wait status before accepting/renaming results or billing from this contract.

Success requires U exit 0, a candidate that represents successful completion of the requested operation (not a failure object), no latched policy/transport/internal failure, complete candidate, any required V exit 0 plus complete verification, exact delivered counts/hashes, budget compliance, closed data channels, empty U domain, delivered M with `complete=true`, and S exit 0. A failure candidate with U exit 0 is still `run_producer_failed{reason:claim}`. An observed native trailer, JSON closing brace, progress `completed`, V receipt, or one of these exits alone is insufficient.

### R14 — Host verification and missing status

The host **MUST** validate M as a launch-bound record and reject missing, duplicate, truncated, replayed or exit-inconsistent status before accepting a result. Tests: T19, T23, T24, T27, T28, T29, T35.

The host reads R/D/N/M concurrently and checks the expected attempt identity, schema/version/contract, allowed fields/types, typed-code args, packet length, no MSG_TRUNC, exactly one packet and subsequent EOF. It independently counts/hashes data bytes and compares them with M. It waits for S itself, not merely U or a shell descendant. Candidate and internal-sink counters are S/V attestations; external counts/hashes are independently checkable.

The endpoint's exclusive ownership and OS isolation authenticate a local record; a nonce or SHA alone does not. A remote host relay authenticates the existing trusted connection and binds that same identity; no signature key is put in U. No new cryptographic signing service is assumed.

If S is killed or M fails, the surviving host produces exactly one local terminal disposition `report_channel_failed{"channel":"status","reason":"io"}` or `reason:"deadline"` when its watchdog expires, records the actual S signal/exit and observed prefixes, and rejects all candidate results. This local disposition is not a forged S packet. If a packet arrived but its identity/framing/count/exit checks fail, the local code uses `reason:"protocol"`. It never invents a successful receipt from complete-looking report bytes.

### R15 — Atomicity, failures and replacement JSON

S and the host **MUST** treat all emitted report/sink bytes as provisional until R13–R14 succeed and never append a failure document after publication has started. Tests: T17–T21, T23–T29, T35.

A stream is not an atomic multi-byte document or a transaction across fds. Before any success byte on R/N, a refusal can publish one existing-shape failure JSON if R08 permits it. Once any success publication starts on either R or N, an error is terminal-only; no replacement JSON is written even if R is still empty. Prefixes, or even a whole apparently valid report whose later close/status fails, are discarded by the host.

Caller-named sink paths are owned by S, use a private same-filesystem temporary and atomic rename, and are not final accepted artifacts before M plus S exit. An old target is removed before receiving this attempt, following the existing finalizer's stale-success rule (`docker/pineforge_sink_finalize.py`). A crash can leave a provisional file or a renamed but unaccepted file; the host keeps a per-attempt manifest and reaps those files. No fsync/power-loss or cross-fd atomicity guarantee is inferred from pipe completion.

### R16 — Liveness and the native infinite poll

S and the host **MUST** enforce absolute W outside U/V, without renewing W for progress or phase records, and enforce P on pending publication without resetting P solely for signals or retries. Tests: T18, T19, T20, T21, T28, T30.

W starts before the first preparation stage and never resets. P starts when a trusted publisher has pending bytes or waits to deliver M; successful positive writes reset only that channel's no-progress interval. W still bounds a peer that accepts one byte per P interval. P does not expire merely because a strategy is computing without diagnostic output.

During native capture, W is the hard deadline for the existing `poll(-1)` at `src/report_sink_writer.cpp`, blocking writes and a V reader that stops. The watchdog remains outside those waits. It closes transport, requests termination, then forces domain kill at G and reaps; a dead native process cannot keep that poll alive. The original limit is latched before termination, so the kill signal is not substituted as the failure cause. Publication stalls use `report_channel_failed{channel,reason:deadline}`; capture/whole-request expiry uses `run_deadline_exceeded{stage}`.

P and W include data close/flush and finalizer-receipt waits. At W, data work stops; at most G remains to stop/reap the producer domain, followed by at most one P interval solely for terminal control delivery. This bounded failure-reporting grace does not renew the data budget. EINTR does not restart an absolute deadline. The host separately watches S and allows at most W plus G plus P from launch for a received terminal outcome; after that it synthesizes the R14 failure. A stuck filesystem worker cannot stall S's status event loop.

Deadline stage comes from trusted launch/descriptor state, never U's phase claims: prepare before compiler launch, compile while the compiler stage is active, run after U launch while native K remains open (or until U exits on a sinkless route), finalize after K EOF while snapshot/candidate verification and producer cleanup finish, publish once external report/sink publication starts. The same stage names label usage populations; overlapping process measurements keep their actual scopes.

### R17 — Checked writes, close and signals

S/V **MUST** account only successful positive writes, retry recoverable short writes/EINTR within the same deadlines, and convert zero progress, closed descriptors/peers and unrecoverable I/O into terminal failure. Tests: T17, T18, T19, T20, T26, T30.

New publisher descriptors are nonblocking; S/V suppress SIGPIPE for their own report transports and classify EPIPE/ECONNRESET/ENOTCONN instead of dying before M. No global signal disposition is trusted inside U. Close is attempted once per owned descriptor; EINTR/close errors do not trigger unsafe retries of a possibly reused number. Completion includes successful required flush/close/synchronization. A signal that kills U is observable producer failure unless S already latched a more specific cause.

### R18 — Fork, dup2 and descriptor attacks

The host **MUST** deny native-stage process creation and prevent descriptor manipulation in U from acquiring a trusted writing endpoint or extending channel lifetime beyond the external deadlines. Tests: T6, T7, T8, T9, T21, T22.

The selected v1 policy uses a trusted seccomp notification monitor to refuse `fork`, `vfork`, `clone` and `clone3` in U, including thread creation, and latches `run_process_refused{operation}` on an observed attempt. It does not rely on U reporting EPERM or on interpreting a self-generated SIGSYS as a policy event. This is a versioned restriction for supervised native code; compiler helpers run in the earlier trusted-tool stage under the same W/D bounds.

The native domain is separately contained and killed/reaped as a whole. Escape primitives, privilege changes, external fd acquisition, and inspection of S/V are denied by the isolation policy. An unexpected surviving descendant is `run_isolation_unavailable{reason:policy}` with completion false, rather than an invented fork-operation arg or success plus a background process. `dup2`, close and reopen of U-local diagnostics/candidate/sink fds can damage U's own output, but cannot affect S's endpoints. There is no claim that dup2 is harmless to the candidate: bad/missing candidates fail validation.

### R19 — Deterministic failure selection

S **MUST** latch one primary outcome from trusted observations and preserve it through termination and secondary cleanup errors. Tests: T14, T21, T26, T29, T33.

Earlier independently latched evidence wins. EOF-derived empty/partial candidate or missing-trailer errors are deferred until U's actual wait result is known, so a nonzero exit/signal is the cause rather than its resulting EOF fragment. A fully received malformed value/invalid code is an independent protocol refusal. For observations in the same event-loop batch, the order is host cancellation, isolation/process violation, R/D budget refusal, independent transport/validator failure, W/P expiry, producer exit/claim, deferred EOF protocol failure, then success. Within budget ties diagnostic precedes report; within transport ties terminal, report, sink, diagnostic, candidate is the stable order. A validated failure candidate selects reason claim; otherwise a nonzero wait selects exit/signal. This ordering affects cause selection, not authentication.

Secondary cleanup/kill/close facts remain in fixed cleanup/usage fields and make completion false. They do not turn a budget refusal into a strategy crash. If cleanup itself prevents M or a valid facade exit, the host rejects the packet under R14. An existing finalizer receipt is mapped only from its actual final status: a finalizer that ends 5 after cleanup is not falsely mapped as finalizer 8 merely because overflow happened earlier (`docker/pineforge_sink_finalize.py`).

### R20 — Catalog additions and exit compatibility

The engine implementers **MUST** register and document the proposed rows below before advertising this contract, preserve existing code meanings/args, and apply the facade exit mapping below. Tests: T2, T3, T11–T21, T25–T30, T33, T36.

All proposed codes follow the catalog's lower snake_case length rule, existing `integer`/`vocab` kinds, central validation and explicit retryability. They have proposed `since:"1.8.0"`; they are **not registered at EM**. Each listed argument is required unless marked optional. Resource-counter args are produced by S/V, never parsed from a strategy's message. Like existing sink-exhaustion args, they are measured boundary facts; the failure-code documentation needs that explicit resource-measurement allowance alongside its prohibition on arbitrary script-computed args (`docs/pages/run-failure-codes.md`, `docker/run_failure_codes.json`).

| Proposed code | Class; retryable | Typed args | Fixed English |
| --- | --- | --- | --- |
| `report_budget_exceeded` | resource; false | `channel`: vocab `json,aggregate`; `cap_bytes,used_bytes,requested_bytes`: integer | The report exceeds its admitted byte budget. |
| `diagnostic_budget_exceeded` | resource; false | `cap_bytes,used_bytes,requested_bytes`: integer | The diagnostics exceed their admitted byte budget. |
| `report_channel_failed` | resource; false | `channel`: vocab `report,sink,candidate,diagnostic,status`; `reason`: vocab `setup,io,peer_closed,deadline,protocol` | A report transport could not complete. |
| `run_isolation_unavailable` | unsupported; false | `reason`: vocab `platform,policy,descriptor` | The requested report isolation is unavailable. |
| `run_process_refused` | resource; false | `operation`: vocab `fork,vfork,clone,clone3` | The native process attempted forbidden process creation. |
| `run_deadline_exceeded` | resource; false | `stage`: vocab `prepare,compile,run,finalize,publish` | The admitted channel lifetime expired. |
| `run_producer_failed` | resource; false | `reason`: vocab `exit,signal,claim`; `exit_status`: integer optional; `signal`: integer optional | The native report producer did not complete successfully. |

The proposed use of class `resource` extends its current machine-memory/scratch description to host process/transport availability and budgets, including an unavailable producer. That description change is additive catalog work; it does not establish blame or refund eligibility. Existing `report_sink_failed` remains engine_fault, including its exhaustion mapping, even though the new policy ceilings use resource. No existing class or numeric native failure identifier is renumbered.

For runtime overflows, `used_bytes` is the accepted allowance consumed and `requested_bytes=1` is the trusted overflow probe; the observed lower bound is used+1. For an admission refusal, used=0 and requested is the trusted planned representation size. These values are not a claim about unread native output.

| Invocation | Successful result | Usage/admission option error | Runtime/transport/resource/isolation/producer/harness failure |
| --- | --- | --- | --- |
| Direct run_json facade | 0 | 2 | 1 |
| Entrypoint backtest facade | 0 | 4 | 4 |
| Entrypoint earlier stages | 0 only if the covered request ultimately succeeds | Missing mounts 2 | Compile/binding 3; transpile 5; a supervisor-origin channel failure 4 |

These facade numbers retain the existing direct and shell mappings cited above. Finalizer 2/3/4/5/6/7/8 are **component exits**, not new top-level shell meanings. Preserve its 8 plus exhaustion receipt mapping exactly. A direct child parsing failure can remain a producer claim; a usage refusal established by S itself has authoritative `run_request_invalid{option:arguments}`. A host-requested stop uses existing `run_aborted{}` and exit 1/direct or 4/entrypoint; no child-reported bars are promoted into trusted billing evidence (`docker/run_failure_codes.json`).

## Failure modes

This table is normative detail of R08, R14–R20. `1/4` means direct/entrypoint facade exits; `F` means finalizer component exit. “Failure JSON if possible” always means R08/R15, not an exception to byte or no-second-document rules.

| Trigger | Observable result | Authoritative code/args | Exit; tests |
| --- | --- | --- | --- |
| Missing/aliased/wrong-type terminal or external fd | No customer load; failure JSON if possible, failure M when valid | `run_request_invalid{option:arguments}` | 2/4; T2, T3 |
| Required OS isolation, grant or storage reservation absent | No customer load; no fallback | `run_isolation_unavailable{reason:platform/policy/descriptor}` | 1/4; T2, T32 |
| Native printf/write, partial diagnostic line, diagnostic fd close | R unchanged; raw prefix/EOF on D | No failure solely for these actions within D | 0 if otherwise successful; T4–T9 |
| Candidate EOF empty/partial, multiple documents, bad shape/extra bytes | No success publication | `report_channel_failed{channel:candidate,reason:protocol}` | 1/4; T17, T25 |
| Positive short write or EINTR | Exact eventual bytes, or a later bounded failure; no double-count | None solely for recoverable syscall | 0 if recovered; T30 |
| Zero write, EBADF, EIO, flush/close failure | Prefix or zero bytes; no replacement after publication | `report_channel_failed{channel:report/diagnostic,reason:io}` | 1/4; T26, T30 |
| R/D reader closes; EPIPE, reset, SIGPIPE condition | S survives to attempt M, stops output | `report_channel_failed{channel:report/diagnostic,reason:peer_closed}` | 1/4; T19, T20 |
| Native sink/finalizer peer closes or sink I/O fails, independently observed by V/S | No accepted sink/report | Existing `report_sink_failed{reason:sink_peer_closed/sink_io}` | 1/4, F5; T20 |
| External N closes during publication | Provisional sink prefix, R empty or provisional; no success | Existing `report_sink_failed{reason:sink_peer_closed}` | 1/4; T20 |
| External R/D/N/M reader stops reading | Pending stream times out, then close; no accepted result | `report_channel_failed{channel:report/diagnostic/sink/status,reason:deadline}` | 1/4 or host-local failure if M unavailable; T18, T19, T20 |
| V stops reading K relay; native `poll(-1)` or blocking sink write stalls | External W fires, U/V killed/reaped | `run_deadline_exceeded{stage:run/finalize}` | 1/4; T21 |
| Peer trickles bytes or repeated EINTR prevents completion | W remains absolute | `run_deadline_exceeded{stage:publish}` | 1/4; T18, T30 |
| Native sink crosses B | At most B approved, B+1 observed; no accepted artifact | Existing `report_sink_failed{reason:sink_exhausted,sink_bytes:B+1,cap_bytes:B}` | 1/4, F8 when V completes its overflow path; T12 |
| Combined representations/candidate crosses R | At most R charged; overflow probe discarded | `report_budget_exceeded{channel:json/aggregate,cap_bytes,used_bytes,requested_bytes}` | 1/4; T11, T13 |
| Diagnostics cross D | At most D retained/forwarded, D+1 observed, truncated flag | `diagnostic_budget_exceeded{cap_bytes,used_bytes,requested_bytes:1}` | 1/4; T14–T16 |
| Sink fragment, missing trailer, wrong counts/hash, bytes after trailer | Strict V refusal; no completion | `report_channel_failed{channel:sink,reason:protocol}`; preserve V's independently typed canonical-value refusal when applicable | 1/4, F3/4/6; T17, T35 |
| U dup2/closes candidate or sink fd | Only private transport damaged; no authority acquired | Protocol failure above, or `run_producer_failed` for resulting exit/signal | 1/4; T8, T9, T17 |
| U attempts fork/vfork/clone/clone3 | Trusted notification, denied creation, native domain stopped | `run_process_refused{operation}` | 1/4; T22 |
| U exits nonzero or is killed mid-report, without an earlier latched cause | No accepted result even with a full candidate or trailer | `run_producer_failed{reason:exit/signal,exit_status/signal}` | 1/4; T23, T26 |
| S is killed before/after report or M | Host rejects prefixes, missing/mismatched status | Host-local `report_channel_failed{channel:status,reason:io/protocol}` | Actual S signal/exit, no invented success exit; T24, T27 |
| M closes, packet exceeds H, second/replayed/truncated packet, actual exit differs | No host acceptance | Host-local `report_channel_failed{channel:status,reason:io/protocol}` | S nonzero for known send failure; actual wait otherwise; T19, T27–T29 |
| Trusted S internal exception | Bounded failure/control record, no trace promoted from U | Existing `harness_internal_error{}` | 1/4; T29 |
| Host cancellation | Stop then G-bounded forced kill; no success trailer synthesized | Existing `run_aborted{}` | 1/4; T28 |
| Post-trailer verification/cleanup/rename/receipt error | Trailer alone remains insufficient; final component status checked | Existing sink I/O code where established, otherwise specific transport/internal failure | 1/4, F5/7 as applicable; T26, T29, T35 |

## Acceptance tests

These are implementation acceptance specifications, not tests executed for this design-only change. All launch installed real processes with real pipe/socket descriptors and collect bytes, packets, timestamps, artifacts and wait statuses externally. No test imports the producer to monkeypatch its writer. Prepared native/generated-code fixtures and an external syscall fault injector are test infrastructure, not changes to the report protocol.

Unless overridden, use valid small input and a zero-trade fixture, a fresh attempt identity, distinct endpoints, generous finite admitted limits, and concurrent host readers. Repeat ordinary execution cases in jailed/unjailed profiles where supported; jailed setup includes the existing admitted arena. Check M schema/types/catalog args, exact external byte counters, facade status, no accepted residue and the success predicate in every applicable test. Test-specific small sizes/timings below are proposed fixtures, not production defaults.

**T1 — Compatible success matrix (R01, R05, R10, R13).** Setup: pin input, clock-dependent report values and binaries; retain unsupervised reference bytes. Action: run both profiles with omitted mode/FULL; exercise no sink, older sinkless module, unjailed named sink, inherited sink, selected-window and capabilities/validation-only requests. Expected: the same logical JSON projection and exact JSON/LF bytes as reference; exact exported sink bytes where requested; one valid success M and exit 0. The internal automatic sink still produces full JSON. No new envelope capability appears.

**T2 — Negotiation, defaults and admission (R01, R02, R03, R07, R20).** Setup: a library initializer that would emit a diagnostic marker; a valid terminal pair for cases that permit it. Action: try an unknown version, absent status fd, pipe instead of status SOCK_SEQPACKET, fd 5, conflicting flags, new knobs without activation, missing isolation support, zero host grants, then a clean default request. Expected: refused cases never emit the initializer marker or start compile/native work; invalid configuration exits 2/direct or 4/entrypoint, unsupported isolation 1/4. The clean M records the exact R/D/H/W/P/G defaults. Invalid M produces no claimed packet on another channel.

**T3 — Real descriptor aliases (R01, R03, R20).** Setup: give different fd numbers duplicated from the same report/status/diagnostic/sink/phase endpoint, including a valid numeric N. Action: request supervision for each alias. Expected: pre-load usage refusal 2/4, no strategy marker, no report/status mixture; after correcting aliases, success. Repeat with wrong-direction and closed descriptors.

**T4 — Native fd-1 writes across lifecycle (R02, R03, R04, R06).** Setup: native fixtures call raw `write(1,...)` in library initialization, strategy creation, callback, getter and destructor. Action: complete a real run. Expected: every marker only in D, identical reference R JSON, no marker on N/M, exit 0 and exact diagnostic bytes. In particular, initializer output is isolated before CDLL.

**T5 — printf in generated code (R02, R03, R04, R06).** Setup: a generated C++ fixture contains buffered `printf`/`std::cout` in its callback and exit cleanup, with no explicit flush at the callback. Action: compile through the real entrypoint and execute it. Expected: eventual native output only on D, including output flushed at process shutdown; one intact reference JSON, sink unchanged, success M, exit 0. Compiler stdout is also diagnostic.

**T6 — Partial line then exit (R02, R03, R04, R06, R18).** Setup: fixtures write `abc` with no LF to fd 1, then finish normally or call `_exit(7)`. Action: launch both. Expected: normal case D is exactly that prefix plus any separately expected harness diagnostics and R is intact, exit 0; `_exit` case has producer wait exit 7, authoritative `run_producer_failed{reason:exit,exit_status:7}`, exit 1/4 and no accepted success. No line-drain hang.

**T7 — Close fd 1 (R02, R03, R04, R06, R18).** Setup: strategy closes numeric fd 1, completes, and optionally opens another private file that reuses 1. Action: execute through native cleanup. Expected: S's R/M stay valid; no private-file text reaches R. A clean otherwise valid run succeeds; closing U's diagnostic reference does not close S's external writer.

**T8 — Close/dup2 stdio (R02, R03, R04, R06, R18).** Setup: strategy closes fd 2, or `dup2(2,1)`, or replaces either with `/dev/null`, while returning a valid result. Action: run each. Expected: reference R/N and success status remain intact; only actual captured diagnostic bytes are counted. Attempted writes to closed fds are not invented as captured bytes.

**T9 — Discover and damage private fds (R03, R04, R06, R18).** Setup: hostile native fixtures enumerate their available fds, duplicate/close C/K, redirect fd 1 to a private report candidate, and try every accessible endpoint for a fake terminal record. Action: run separately from successful stdio-only cases. Expected: corruption yields typed candidate/sink protocol or producer failure, never a second public JSON or accepted forged M. R/M themselves cannot be acquired.

**T10 — Forged errors and invalid typed args (R04, R10, R11).** Setup: native fixtures print a perfect `harness_internal_error` failure on fd 1, then separately place a valid such failure on C and exit 1; another submits an unknown code/wrong arg type. Action: collect full streams. Expected: fd-1 print is raw diagnostics only; a relayed C failure remains a producer claim and authoritative M is `run_producer_failed{reason:claim}`; malformed code/args is candidate protocol failure. No trusted refund/harness-fault classification follows from the claim.

**T11 — JSON exact fit and one over (R05, R07, R08, R10, R20).** Setup: a sinkless module produces known encoded document length L including LF. Action: set R=L, then R=L−1. Expected: exact fit succeeds; overflow observes at most R+1, charges at most R, returns `report_budget_exceeded{channel:json,cap_bytes:R,used_bytes:R,requested_bytes:1}`, exit 1/4, no accepted R. Where no allowance remains, R is empty and M still contains the refusal.

**T12 — Sink exact fit and finalizer compatibility (R05, R07, R08, R10, R20).** Setup: known valid stream length L and small summary; set R large enough for both. Action: supply planned B=L, then B=L−1, using the real V executable. Expected: success at L; finalizer component exit 8 at the overflow probe with its existing exact receipt fields, authoritative existing `report_sink_failed` args, top-level 1/4, no accepted artifact. Compare finalizer receipt bytes against its existing contract, not M's new schema. Pause/kill V after the probe in another run: S still reports its independently observed overflow, with V's actual signal/unknown receipt and no fabricated component exit.

**T13 — Aggregate representations and internal snapshot (R05, R07, R08, R09, R20).** Setup: sink length L and candidate length J individually below R but L+J=R+1. Action: run both an internal full projection and an exported sink/summary projection. Expected: `report_budget_exceeded{channel:aggregate,cap_bytes:R,used_bytes:R,requested_bytes:1}`, exactly the remaining candidate allowance plus one probe, no success publication, all retained report extents together at most R. Increase R by one: both succeed with the original projection; read-only snapshot mutation from U is denied.

**T14 — Gigabytes to stderr (R06, R07, R09, R10, R12, R19).** Setup: a native loop attempts gigabytes on fd 2 and suppresses/ignores any cooperative stop. Action: run with small positive D. Expected: no more than D captured/forwarded, exactly D+1 observed before stop, `diagnostic_budget_exceeded{cap_bytes:D,used_bytes:D,requested_bytes:1}`, diagnostic truncated flag, native domain killed/reaped within the stated deadlines, exit 1/4 and a usage-bearing M. No drain-to-discard gigabyte loop continues.

**T15 — Combined diagnostics and exact boundary (R06, R07, R20).** Setup: fixture emits known binary sequences, no LF, across native fd 1, fd 2 and log helpers; include NUL and invalid UTF-8. Action: total exactly D, then D+1. Expected: exact bytes and success at fit; aggregate refusal at one over with no extra marker byte. Individual fd/log budgets cannot each consume D.

**T16 — Diagnostics in preparation and cleanup (R06, R07, R10, R20).** Setup: real entrypoint/tool fixtures emit more than D in compiler stdout, transpile stderr, binder stderr or native destructor, one per run. Action: execute each stage. Expected: the same aggregate diagnostic refusal and cap applies before/during/after native execution; no successful report is accepted. Genuine compile/transpile failure without a channel overflow retains shell 3/5.

**T17 — Fragments and premature EOF (R04, R08, R15, R17).** Setup: fixtures write a candidate JSON prefix then close C, close K after a partial sink line, send C while deliberately keeping K open, or use empty K after an explicit sink request. Action: terminate normally. Expected: candidate/sink protocol failure, V6 for incomplete sink, no success report/artifact, no synthetic trailer or second document. An empty automatic sink with a genuine older array module and an ordinary pre-execution failure that closes K before C are positive controls.

**T18 — Report stalls and trickles (R03, R09, R13, R15–R17).** Setup: report larger than the real nonblocking socket buffer; host keeps R open without reading, then a variant reads one byte before each P expiry. Action: run with small W/P and wait externally. Expected: stall variant reports channel report/deadline within P plus G; trickle variant reaches absolute W with stage publish, not an endlessly renewed timer. Prefix byte counts are exact, queues/storage remain bounded, exit 1/4 and no success M.

**T19 — Terminal transport failure (R09, R14–R17).** Setup: close M before terminal send; separately fill its real send queue so no record can be delivered, or close R before publication. Action: run. Expected: M failure yields no valid terminal packet and nonzero S exit; the surviving host records one local status I/O/deadline failure. R close yields an S failure packet if M works, with report/peer_closed. Neither accepts a complete-looking JSON.

**T20 — Sink/diagnostic peers close or stall (R09, R13, R15–R17, R20).** Setup: close or stop reading external N/D, with enough output to fill their buffers; separately close V's relay-input reader while native output is active. Action: run every case. Expected: peer closure versus P timeout is distinguished as in the failure table; sink peer I/O uses existing `report_sink_failed` where independently established by the relay failure. S survives SIGPIPE conditions long enough to attempt M; partial N/R data never become accepted.

**T21 — Stopped native sink reader (R05, R09, R13, R16, R18, R19).** Setup: pause the real V process while U generates a stream larger than all relay/socket buffering but below B, so its native writer blocks or enters the EM `poll(-1)` path. Action: let small absolute W expire. Expected: authoritative `run_deadline_exceeded{stage:run}`, U/V killed/reaped within G, no snapshot/report deadlock, no success trailer manufactured, 1/4 and bounded usage. S retains its independently observed byte counters; V's unreported verification facts are null. Repeat pausing V after U closes K: stage finalize is reported.

**T22 — Fork and isolation attacks (R02, R11, R18).** Setup: native fixtures attempt fork/vfork/clone/clone3, including an attempted thread and a child intended to hold K open; other fixtures try `/proc` reopening, ptrace/process-memory access, signals to S, namespace escape and writes to S/V files. Action: execute one attack per run under the real deployed policy; in a separate negative control, the trusted test launcher deliberately leaves a descendant in the native domain. Expected: trusted process-creation notification gives the matching typed refusal; no accepted run has a descendant holding EOF. The negative control reports isolation/policy failure. Other access attempts cannot alter R/M or trusted code/files. A self-generated SIGSYS is only producer signal evidence.

**T23 — Kill U at report boundaries (R03, R13–R15).** Setup: an external controller knows real K/C byte counts and U PID. Action: SIGKILL U before any output, mid-sink, after a valid trailer, mid-candidate, and after a full candidate before U's normal exit. Expected: authoritative producer signal failure, no accepted result in every case; observed prefixes/unknown source totals are truthful. Full trailer/candidate bytes cannot override wait status.

**T24 — Kill S during publication (R03, R13–R15).** Setup: large report and external byte-counting readers. Action: SIGKILL S before first publication, mid-N, mid-R and after the last JSON byte before M. Expected: host status/I/O failure with actual S signal, no accepted report, local disposition exactly once, provisional artifacts reaped by the surviving host. No missing packet is interpreted as success.

**T25 — Candidate framing and schema (R04, R10, R11, R20).** Setup: native fixtures submit two JSON values, duplicate keys, invalid UTF-8, NaN, wrong report shape, a second LF/payload, or a syntactically complete report without its required LF. Action: launch each as a real producer. Expected: candidate protocol failure, no success publication, bounded control facts. A valid failure object followed by U exit 0 still gets producer_failure/claim and facade 1/4. A valid child failure with oversized args omits the claim from M with the explicit omitted flag, never fabricated empty required args.

**T26 — Late write/flush/close and producer failure (R11–R13, R15, R17, R19, R20).** Setup: real descriptors and an external syscall fault injector target S's actual write, flush-backed I/O, close, file sync or rename after some/all report bytes; separately U exits nonzero after writing a valid candidate. Action: trigger each once. Expected: exact successful-byte counters, no appended error document, failure M/1-or-4, and whole-looking output still rejected. Producer failure is not a harness fault; independently established sink file I/O retains its typed reason.

**T27 — Replay, packet corruption and exit disagreement (R11, R14, R15).** Setup: launch two distinct identities; an external trusted test relay can replace, truncate or duplicate received status packets. Action: feed the old identity, two packets, a packet over H, MSG_TRUNC, mismatched digest/count, or success M followed by externally killing S before normal exit. Expected: host status/protocol failure and no acceptance; it does not authenticate by matching report text alone.

**T28 — Host cancellation and supervisor watchdog (R12–R16, R20).** Setup: U stalls indefinitely; separately S is externally stopped. Action: request cancellation through the host's real control path, or wait W+G+P when S cannot respond. Expected: cancellation records authoritative `run_aborted{}` and 1/4, with no trusted child progress invented; stopped S gets exactly one host-local status/deadline outcome. Child-only forged abort output is covered by T10.

**T29 — Trusted fault, cleanup and ordering (R10–R15, R19, R20).** Setup: external syscall faults cause S's own internal operation to fail, V cleanup to fail after an overflow, or a close to fail after a budget violation. Action: run separately and repeat two events queued in one controlled event batch. Expected: actual S internal error alone may use `harness_internal_error{}`; V ending 5 is not mapped as 8; latched budget causes survive secondary child kill/close. Tie ordering matches R19, completion false, and actual S exit/status always agree or host rejects.

**T30 — Short writes, EINTR, zero progress (R06, R16, R17, R20).** Setup: use tiny real socket buffers/read fragments and external syscall fault injection on actual S/V I/O. Action: force positive short writes, EINTR, EAGAIN, one zero return, EBADF, and a signal storm through deadline expiry. Expected: recoverable cases deliver exact reference bytes with no count duplication and exit 0; zero/EBADF fails I/O; retries never renew W, close is not unsafely retried, and a signal storm ends under W.

**T31 — Zero and parser semantics (R07, R08).** Setup: host grants differ from explicit caller ceilings; include a direct invocation of the existing finalizer. Action: new ceilings omitted, zero, positive smaller, positive above grant, empty, negative, whitespace, overflow; compare new zero with existing file-finalizer `--max-bytes 0` and stream-finalizer zero. Expected: new zero resolves to finite recorded grants, positive is exact or refused without clamp, malformed new knobs are usage errors; existing finalizer file zero still allows no bytes and stream zero still exits 2. Host never forwards new zero as finalizer zero.

**T32 — Reservation failure and bounded backing (R07, R09, R12).** Setup: provision a real per-attempt filesystem/memory reservation smaller than the admitted retained storage, then a sufficient one; observe allocated blocks externally. Action: run with report/diagnostic capture near limits. Expected: insufficient reservation refuses before customer load; sufficient capture fits the admitted logical and allocation-overhead reservation, with measured backing labelled correctly and no second whole sink copy. Missing telemetry is null/unavailable.

**T33 — Combined overflow, preserved terminal capacity (R06, R07, R09, R10, R19, R20).** Setup: saturate report and diagnostic inputs, small R/D, separate reserved H, and arrange both overflow notifications in one batch. Action: execute. Expected: R19's diagnostic-first budget tie rule, at most R/D accepted bytes, intact bounded M with required refusal/usage facts, no borrowing of H by raw logs. Repeat with an earlier report refusal: earlier evidence remains primary.

**T34 — Usage and provenance (R12).** Setup: real stages consume known CPU, allocate/touch memory and create known scratch/work extents while the trusted host records kernel/file counters; include success, typed refusal and SIGKILL cases. Action: run and compare M with those external observations. Expected: exact output counters, correct units/scopes, available kernel CPU/wall/storage measurements, sampled peaks labelled lower bounds, unavailable private memory null, producer arena figures labelled producer_reported; per-process peaks are never summed as simultaneous job peak. The same fixed cells survive failure.

**T35 — Frozen bytes and independent completion (R04, R05, R08, R10, R13–R15).** Setup: fixtures cover all sink tags E/T/O/V/K/X/H, Unicode escaping and line-boundary sizes, with reference sink-v1 bytes. Action: run unchanged and with altered trailer digest/counts, extra trailing bytes, premature EOF, and canonical E-null refusal. Expected: valid sink bytes/digest framing identical; strict refusals retain finalizer component 3/4/6 or its existing typed `report_result_invalid`. Verify full-wire hashes separately from the pre-trailer sink hash. A complete sink followed by JSON/status failure is never accepted.

**T36 — Unsupported modes and protocol exclusions (R01, R20).** Setup: real direct/shell invocations. Action: request SUMMARY_ONLY/STREAMED with supervision; request `--help`, `csv2bin`, transpile-only or an external phase fd under supervision; then run these protocols with negotiation absent. Expected: S's pre-load validation retains `report_mode_unsupported` with unchanged args/exit facade for modes; excluded protocols get pre-execution `run_request_invalid{option:arguments}` 2/4 instead of false backtest success. Internal selected-window phase tracking still works in T1. Unsupervised utilities/phase transport retain their existing protocol and successful old callers still work.

### R21 — Host conformance checklist

The hosting layer **MUST** pass T1–T36 for its shipped launcher/image pairing and enforce the following acceptance checklist before marking a hosted run successful. This is checked outside-in by the corresponding tests, not by a profile string.

- Negotiation is mandatory for covered billed runs; unsupported isolation never falls back (T1–T3, T36).
- It owns identity/endpoints, removes all aliases from U and proves the deployed OS boundary against native attacks (T3–T9, T22).
- It grants/reserves finite bytes/lifetime/control capacity and continuously drains every channel (T11–T21, T31–T33).
- It receives exactly one launch-bound packet, independently checks external bytes/hashes, EOF and S's actual exit, then commits artifacts (T23–T29, T35).
- It retains usage/unknowns on every disposition and owns crash cleanup outside S (T24, T28, T32, T34).

### R22 — Engine conformance checklist

The engine implementers **MUST** ship the paired engine-side implementation, catalog/docs updates and outside-in evidence for the following checklist before advertising `supervised/v1`. Tests are the referenced real-process cases.

- Every Python report/failure/capability/validation writer targets C; pre-load fd redirection includes constructors and final destructors (T1, T4–T10).
- Internal/named/inherited sink capture uses K/S/V with correct EOF/snapshot ownership and unchanged projections (T1, T5, T12, T13, T17, T21, T35).
- Aggregate report/diagnostic guards, bounded serializers/storage and component-exit mapping agree with trusted enforcement (T11–T21, T26, T30–T33).
- Terminal provenance, counter definitions, usage, failure precedence and facade exits match this document and the catalog (T10, T23–T34).
- Frozen sink bytes and all legacy defaults remain compatible; no unimplemented report-mode capability is advertised (T1, T12, T31, T35, T36).

## Open decisions and assumptions

The mechanism, opt-in version, defaults, codes, event order and conformance rules above are decided for this proposal. The following deployment/integration facts cannot be established from the supplied engine files. They are not alternate mechanisms or permission gates for writing the document.

### R23 — Policy ownership

The release owner and the hosting layer **MUST** publish the actual admitted hosting grants and rollout pairing before calling the output gap in the resource-control audit of the engine at 95f4fd1f closed. Tests: T2, T31–T33, plus T21/T28 on the shipped pairing.

| Deployment/integration decision | Decider | Recommended/default assumption |
| --- | --- | --- |
| Production R/D/W/P tiers and measured supervisor/validator storage/memory reservation | Release owner + hosting layer | Start with the proposed finite defaults, calibrate using billing/workload measurements; change declared grants explicitly |
| Installed namespace/identity/seccomp-notification policy and image provenance | Hosting layer | Linux boundary in R02/R18; absence fails closed, including on a local unsupported platform when explicitly requested |
| Exact measured-usage receipt field spelling and measurement sources | Engine implementers responsible for the measured-usage receipt + hosting layer | R12 mapping with explicit unavailable/producer-reported cells; do not claim unimplemented measurements |
| Catalog numeric enum allocation and release asset regeneration | Engine implementers | Add rows for 1.8.0 without renumbering; keep failure-catalog/v1 schema and central validation |
| Hub/image packaging and hosted activation date | Engine implementers + hosting layer, release owner | Ship launcher, engine harness, validator, catalog and host verification as a pinned pairing; billed policy ON |
| Remote receipt carriage and billing/refund disposition | Hosting layer + release owner | Authenticated existing host connection; only supervisor-established facts are authoritative; no refund inferred from U's engine_fault claim |
| Future bare-CLI default ON or support for native worker threads | Release owner + engine implementers | Keep this revision OFF outside hosting and deny native creation; negotiate a later version for a changed restriction/default |

If a deployment cannot supply the selected OS policy or telemetry, it refuses isolation or records the explicit measurement unknown as specified. It does not implement a same-process “best effort” variant bearing the same contract name.

## Implementation notes for engine implementers

This is dependency order, with current insertion points cited. New file names are proposed. This proposal does not modify these implementation files.

### R24 — Catalog and trusted boundary first

The engine implementers **MUST** land the catalog, standalone supervisor and trusted host handshake before enabling any advertised capability. Tests: T2, T3, T10, T22, T27, T29, T36.

1. Extend `docker/run_failure_codes.json`, central validators and generated documentation/release assets with R20's rows and resource-measurement rules. Existing catalog schema/kinds and validation policy are the compatibility base (`docker/run_failure_codes.json`, `docs/pages/run-failure-codes.md`). Actual enum-generation files beyond the supplied inputs need inspection by the engine implementers; no unread implementation is asserted here.
2. Add proposed `docker/pineforge_report_supervise.py` plus the small installed launcher integration. Implement endpoint admission, isolation attestation, wait/deadline loop, bounded relays/reservations, status schema and the host protocol as a component that never loads customer libraries. The hosting layer implements the OS launcher and receiving verifier, including a surviving watchdog/reaper.
3. Put entrypoint activation before work-directory creation/transpilation, not just around backtest Python. Current insertion boundaries are the startup/workdir at `docker/entrypoint.sh`, transpile-only branch at `docker/entrypoint.sh`, compiler at `docker/entrypoint.sh`, fd forwarding at `docker/entrypoint.sh` and runtime facade at `docker/entrypoint.sh`. Strip private recursion/dispatch state before a nested launch; U cannot opt itself out.

### R25 — Producer, sink and serialization integration

The engine implementers **MUST** implement explicit channel ownership in every producer path and retain the existing projections/encoders while adding bounded preparation. Tests: T1, T4–T17, T21, T26, T30, T35.

4. Dispatch direct-run supervision before `load_strategy`; the current load occurs at `docker/run_json.py`, including capabilities at `docker/run_json.py`. Parameterize `write_failure` and `_write_report` with an owned candidate destination instead of ambient `sys.stdout` (`docker/run_json.py`). Update `_ArgumentParser.error`, outer exception handling, selected and ordinary success publication together (`docker/run_json.py`). `_discard_stdout` becomes candidate-owner discard in U; it cannot redirect S's R (`docker/run_json.py`).
5. Add a supervised sink owner behind `_new_report_sink`, `_FdReportSink` and `_ReportSink` (`docker/run_json.py`). Distinguish capture transport from requested projection. Release K's last native writer before the snapshot wait; keep that close idempotent through the existing inherited owner (`docker/run_json.py`). Preserve typed setter failures already read by `_ReportSink`, rather than reintroducing the older unconditional invariant classification (`docker/run_json.py`).
6. Use V's existing `_receive`, `_verify` and final receipt logic with a mandatory positive B and private target (`docker/pineforge_sink_finalize.py`). S counts/relays independently, watches V externally and bounds the receipt; no timer or total-byte enforcement is inferred merely from its “trusted” docstring. Reuse `SinkReader` verification instead of weakening grammar or accepting a trailer without EOF (`docker/pf_sink.py`).
7. Add a counted destination before all JSON-buffer/file growth in `_prepare_stream_report` and `_prepare_report_json`; preserve `_stream_json` encoding, key order and LF (`docker/run_json.py`). Enforce the trusted remaining budget again at S. Preserve `_build_sink_report`'s small-fd projection, named reference and internal full reconstruction as projection choices independent of new K transport (`docker/run_json.py`).
8. Keep the native writer's borrowed-fd and checked trailer lifecycle (`src/report_sink_writer.cpp`). Host W, not a mutable in-U timer, is the required enforcement for `State::wait_writable`/`flush` (`src/report_sink_writer.cpp`). A future cooperative native cap/deadline can improve early error reporting but cannot replace S/V's budgets/watchdog. No v1 descriptor-layout change is required by this design.
9. Leave `pine_log_info/warning/error` content unchanged; fd-level D metering covers them and arbitrary native output alike (`include/pineforge/log.hpp`). Put trusted usage collection for the planned measured-usage receipt in S before teardown; retain current arena values only with their actual provenance/lifetime (`docker/run_json.py`).

### R26 — Frozen surfaces and release evidence

The engine implementers and the hosting layer **MUST** retain the following frozen surfaces and pass the complete paired E2E checklist before the hosted rollout. Tests: T1–T36, especially T1, T12, T31, T35, T36.

- Do not alter sink-v1 header/schema columns, record field/order/escaping rules, tags, integer/line bounds, trailer counts/digest grammar or what the digest includes. The current wire constructors and strict consumer are the reference (`src/report_sink_writer.cpp`, `docker/pf_sink.py`).
- Do not append usage/status fields to the frozen trailer or legacy report structs, overload a phase record as a receipt, or change existing sink-finalizer receipt bytes/exits. The new M record is independent (`docs/design/o1-report-contract.md`, `docker/pineforge_sink_finalize.py`).
- Do not globally change existing finalizer-zero or command-scratch-zero semantics; do not rename/reclassify existing failure codes, their args or shell exits (`docker/pineforge_sink_finalize.py`, `docker/run_failure_codes.json`, `docker/entrypoint.sh`).
- Preserve legacy successful stdout JSON and failed-publication no-second-document behaviour. The explicit supervised/v1 differences are protected stdio/process inheritance, finite admission, the terminal channel, delayed sink publication, refusal of excluded utility/external-progress protocols, and zero JSON on a failed attempt when R has no room for a replacement. Legacy callers that do not negotiate retain their old protocol; hosted conformance requires negotiation.
- Publish paired process/fd E2E evidence and deployment identity. Source review, a Python writer unit test, or the presence of this design document alone does not establish the isolation or resource guarantees.

## Refusals and authoring verification

No hook/guard refusal occurred during authoring. The supplied audits and pinned engine sources were read; no build, test, strategy, compiler, deployment, network or repository mutation was run. The acceptance tests above remain implementation/release obligations, not claimed results.
