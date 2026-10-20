# CR-223 receipt assertion handoff

Worktree: `~/.local/state/pineforge-top-core/lanes/core-cr223-receipts/wt`
Branch: `core/cr223-receipts`
Base: `7d79006208666dcd05c04e441316c37eb01b9c20`

## Inventory recorded before fixture edits

All locations in this inventory refer to the base above. The three requested
fixtures were read in full. Searches covered the whole `tests/` and `scripts/`
trees, including `receipts[`, `sorted(receipts)`, `== original`, HTTP receiver
definitions, and comparisons/slices of `received`, `received_bytes`, `rows`,
`attempts`, and `effects`.

| Site | Existing comparison and disposition |
| --- | --- |
| `tests/native_live_service_e2e.py:41` | Collects `(Idempotency-Key, raw body)` receipts; body contains the service's `sequence`. |
| `tests/native_live_service_e2e.py:296`, `:297`, `:299` | Four completed failures/four HTTP receipts; snapshots by sorting whole tuples. Replace snapshot with a unique sequence index retaining the complete tuple. |
| `tests/native_live_service_e2e.py:310`, `:311`, `:313`, `:314` | Four selected/completed replays and eight total receipts; compares the replay phase with the original by sorting. Keep every count and compare complete sequence-indexed tuples. |
| `tests/native_live_service_e2e.py:320` | Duplicate control request must leave four completed attempts. Preserve. |
| `tests/native_live_service_e2e.py:322`, `:324`, `:326`, `:327` | `--from 4` selects one completion/nine total receipts; last receipt merely belongs to original. Compare the whole replay phase to the exact sequence-selected original subset. |
| `tests/native_live_service_e2e.py:346`, `:348`, `:351` | No-op/restart must leave audit rows unchanged and nine receipts; offline replay produces ten. Preserve counts and audit equality. |
| `tests/native_live_routing_e2e.py:158`, `:163`, `:166`, `:170` | Attempt history already selects the action by payload sequence, then checks that action's attempt/phase/backoff order. Preserve chronological checks within one action. |
| `tests/native_live_routing_e2e.py:229`, `:236` | Journal/actions-follow output is a serial export ordered by ledger ordinal, not HTTP receipts. Preserve. |
| `tests/native_live_routing_e2e.py:253`, `:265`, `:280`, `:331` | Target receipt counts, replay immutability, and four-way delivery barrier. Preserve all counts and the barrier. |
| `tests/native_live_routing_e2e.py:347`, `:348`, `:351`, `:352`, `:374`, `:378`, `:379`, `:384`, `:391` | Retry/redelivery checks already filter by action sequence and compare bodies/keys; preserve those checks. |
| `tests/native_live_routing_e2e.py:414`, `:415`, `:418`, `:419`, `:420`, `:423` | Keep-alive replay already compares exact bytes by idempotency key, with four unique deliveries and one completion per action. Preserve. |
| `tests/native_live_routing_e2e.py:450`, `:476`, `:501`, `:523` | Sequence-filtered retry count, timeout/disconnect counts, and immutable ledger snapshots. Preserve. |
| `tests/native_live_routing_e2e.py:557`, `:558`, `:560` | Crash recovery compares HTTP arrival sequence list to `[1,2,3,4]`. Compare unique sequence-indexed complete bodies to ledger payloads; keep four receipts and stored-state equality. |
| `tests/native_live_equivalence_e2e.py:414`, `:415`, `:417` | Complete contiguous sequences and unique event IDs. Retain exact coverage and reject duplicate sequences before indexing. |
| `tests/native_live_equivalence_e2e.py:424`, `:430`, `:432`, `:435` | Header/body identity, exact retry bytes, first-attempt arrival list, and complete effect/body equality. Preserve identity/bytes/coverage; key first attempts/effects by sequence. |
| `tests/native_live_equivalence_e2e.py:438`, `:444`, `:445` | First-pass effects must arrive in order. Replace arrival-order assumptions with sequence-keyed payload/coverage assertions. |
| `tests/native_live_equivalence_e2e.py:447`, `:451`, `:456`, `:459`, `:460` | Known redelivery selection and validated invocation boundaries. Keep boundaries; slices describe process phases, not submission sequence. |
| `tests/native_live_equivalence_e2e.py:461`, `:463`, `:465`, `:467`, `:470`, `:474`, `:476`, `:478` | Redelivery attempts/effects and non-redelivered effects are compared by arrival order. Compare by sequence, preserving success provenance, selection, and exact payload equality. |
| `tests/native_live_equivalence_e2e.py:485` | Ack-loss replay must have a successful attempt after the restart boundary. Preserve. |
| `tests/native_live_equivalence_e2e.py:528`, `:529` | Chooses crash action by number of completed effects, not submission sequence. Withhold acknowledgement for the requested payload sequence instead. |
| `tests/native_live_equivalence_e2e.py:710`, `:712`, `:719`, `:720`, `:723` | Full modeled action equality, fail-first count per event, and positional crash-effect lookup. Keep comparisons/counts; look up the crashed action by its sequence. |
| `tests/native_live_e2e.py:28`, `:117`, `:118`, `:120`, `:123`, `:126`, `:131`, `:141`, `:149`, `:151`, `:224`, `:226`, `:244` | Existing helper already sorts specifically by sequence for complete payload comparisons. Retain sequence ordering and add duplicate rejection. |
| `tests/native_live_e2e.py:159`, `:160`, `:162`, `:169`, `:170` | Failed-only replay compares receipt at append position four to position zero. Determine failed sequence from the ledger; compare the complete indexed replay phase and preserve counts/unique IDs. |
| `tests/native_live_e2e.py:192`, `:194`, `:201` | Same positional failure in batch replay. Apply the same ledger-sequence selection, exact payload equality, and receipt count checks. |
| `tests/native_live_startup_e2e.py:263`, `:268`, `:269` | Two failed receipts; assumes receipt zero is ledger event zero. Compare the entire raw-byte receipt set to ledger payloads by payload sequence. |
| `tests/native_live_startup_e2e.py:275`, `:282`, `:283`, `:288` | Two initial/four total receipts; compares first receipt to replay position two and replay array to ledger array. Keep counts; compare complete raw payloads by sequence. |
| `tests/native_live_startup_e2e.py:299`, `:303`, `:304` | Idle replay compares the same append-only receipt buffer to its snapshot plus exact ledger rows. This detects any added request, not relative arrival order; preserve. |
| `tests/native_live_genuine_retry_e2e.py:43`, `:47`, `:50`, `:51` | Complete invocation-boundary checks and phase slices. Preserve; these are process boundaries, not submission positions. |
| `tests/native_live_genuine_retry_e2e.py:52`, `:53`, `:55`, `:56`, `:59`, `:60`, `:64` | Requires phase/redelivery/first-pass arrival order and matches effects to successful attempts. Compare exact phase effects to new successful payloads by sequence, retaining boundaries and success provenance. |
| `tests/native_live_genuine_retry_e2e.py:80`, `:82`, `:106`, `:107` | Full modeled action equality and exact fail-first multiplicity. Preserve. |
| `tests/native_live_genuine_retry_e2e.py:94`, `:95`, `:96`, `:102` | Process cost receipts, ordered synchronously with launches and keyed additionally by command/PID. No service request sequence exists or is needed here; these are not delivery receipts. Preserve. |
| `tests/native_live_tick_tape.py:390`, `:391`, `:400`, `:401` | Uses the shared equivalence receipt assertion and exact modeled actions/fail-first coverage. Inherits its sequence fix; preserve comparisons. |
| `tests/test_native_live_equivalence_harness.py:85`, `:87`, `:97`, `:108`, `:118`, `:124`, `:126`, `:132`, `:135`, `:140`, `:149`, `:152`, `:158`, `:161`, `:169`, `:174`, `:187`, `:190`, `:200`, `:207`, `:209`, `:211` | Self-tests pin arrival-order rejections alongside bytes/keys/unique coverage/boundary/restart checks. Update order expectations to accept permutations and retain/add negative coverage for changed payloads, keys, sequences, counts, phase attribution, and replay. |
| `tests/live_phase_a_migration.py:64`, `:65`, `:66`, `:74` | Already compares exact sequence population `[2,3,4]`, selects recovered sequence 2, and checks exact event ID/raw payload/count. Preserve. |
| `tests/native_live_websocket_e2e.py:139`, `:142`, `:144`, `:145`, `:148`, `:169` | Every case requires zero or exactly one receipt. Singleton indexing follows the exact count, with no multi-request order assumption. Preserve finality assertions. |
| `tests/native_live_chart_calendar_e2e.py:119`, `:133`, `:154`, `:168`, `:173`, `:174` | Calendar result summaries, not service request receipts; no receipt-array equality. Preserve. |
| `tests/native_live_tick_oracle.py:125`, `:127`, `:130`, `:132` | Synchronous engine execution observations matched by order identity; not concurrent HTTP completion. Preserve. |
| `tests/native_live_chart_input_e2e.py:231` | `== original` compares ledger file bytes. Not delivery receipts; preserve. |

Other whole-tree literal hits are synchronous C++ event tapes or unrelated
state assertions, not live service delivery receipts:

- `tests/test_native_resting_replay_contract.cpp:1697`, `:1698` (one event).
- `tests/test_native_execution_terms.cpp:811`, `:812`, `:1032`, `:1033`,
  `:1034`, `:1062`, `:1063`, `:1064`, `:1330`, `:1331`, `:1344`, `:1345`,
  `:1346`, `:1374`, `:1375`, `:1383`, `:1384`, `:1385`, `:1427`, `:1428`,
  `:1441`, `:1442`, `:1443`, `:1700`, `:1937`, `:1983` (native event order).
- `tests/native_order_full_fold.hpp:557`, `:558`, `:559`, `:560` (state hashing).
- `tests/test_native_definition_index.cpp:426` (last synchronous event).
- `tests/test_native_group_keep_handle.cpp:831`, `:832`, `:896`, `:916`,
  `:1506` (counts and complete synchronous snapshots).
- `tests/test_settlement_observation_boundary.cpp:438`,
  `tests/test_native_order_core.cpp:708`, `tests/test_market_admission_state.cpp:70`,
  `tests/test_pending_placement_receipts.cpp:105`, and twin-parity inventory
  mirrors (`tests/twin_parity_base.json:18072`, `:20857`, `:27346`;
  `tests/twin_parity_ledger.md:157`, `:263`, `:464`) compare engine state/handles.
- `scripts/` has ABI/compiler/process/CI receipt dictionaries and source-checker
  rows, but no additional concurrent HTTP delivery receipt-array comparisons.
  HTTP receiver search also inspected `tests/checked_settings_e2e.py:17`, `:48`,
  `:88` and `tests/strategy_capabilities_e2e.py:18`, `:71`, `:78`, `:89`.
  These collect only request-path markers and assert absence/presence, with no
  positional receipt comparison. They have no service sequence in their marker
  arrays; no sequence is needed for their existing absence/presence assertions.

No affected HTTP fixture lacks a service sequence: each raw delivery body has
`sequence`. No synthetic replacement sequence is needed.

## Execution constraints

No tests, builds, preflight, corpus, or benchmarks may run on this Mac, per the
explicit task brief. Verification is pending supervisor lab proof on the exact
committed tree; no runtime PASS is claimed. Production and runner files are
outside scope. `REPORT.md` and `DONE` are the explicitly requested handoff files.

## Implementation and assertion strength

Implementation HEAD: `f6a72083b5b9a44b13c0549ae59faece8b5b81dd`
(`test(live): match delivery receipts by request sequence`). The following
handoff commit adds only this report and `DONE`; its exact HEAD is supplied in
the executor's final response. Supervisor proof must use that final HEAD.

- `tests/native_live_receipts.py:4` builds a sequence index of complete receipts
  and raises on duplicates before any dictionary overwrite can hide a receipt.
  It never derives a sequence from append position.
- Service redelivery (`tests/native_live_service_e2e.py:302`, `:319`, `:335`,
  `:360`) compares the entire idempotency-key/raw-body tuple by sequence.
  Counts remain four failures, four selected/completed full replays, eight
  receipts, one selected/completed `--from` replay, nine receipts, and ten
  after offline replay. Duplicate-control/audit/restart checks remain. The
  original membership assertion for selected redelivery is strengthened to
  equality against precisely the original sequences at or above `--from`.
- Basic live retry (`tests/native_live_e2e.py:168`, `:177`, `:206`, `:215`)
  obtains the failed request's sequence from the actual ledger. It compares
  the complete replay receipt to that exact original payload, keeping four
  initial receipts/four unique event IDs and explicitly requiring five after
  each one-request replay. The existing sequence ordering helper now also
  rejects duplicates.
- Startup (`tests/native_live_startup_e2e.py:271`, `:287`, `:293`) compares
  complete raw UTF-8 payloads to ledger payloads by sequence in both phases.
  It keeps the two/four receipt counts, attempt counts, identities, physical
  action/trade assertions, and idle replay immutability. Raw equality now
  covers both original failed payloads instead of only the first arrival.
- Routing recovery (`tests/native_live_routing_e2e.py:558`) still requires four
  receipts with sequences 1 through 4 and unchanged ledger snapshots, and
  additionally requires exact raw-body equality to all pre-crash ledger
  payloads by sequence. Existing sequence-filtered retry and key/body checks,
  the concurrency barrier, and attempt/backoff chronology are unchanged.
- Equivalence (`tests/native_live_equivalence_e2e.py:416`) keeps contiguous
  complete sequences, unique event IDs, every header/idempotency check, exact
  retry bytes, and full effect-versus-delivery payload equality. First attempts
  and effects must have equal sequence populations and equal complete payloads.
  Canonical engine comparison order is obtained by looking up those sequences.
  Redelivery retains known-event and invocation-boundary checks, compares each
  phase's entire new-effect map to new successful attempt payloads, and requires
  the final invocation to replay the full selected sequence population.
- The equivalence crash gate (`tests/native_live_equivalence_e2e.py:532`)
  withholds acknowledgement for `restart_action`'s actual payload sequence.
  Its post-restart successful replay check (`:726`) looks up that same sequence,
  regardless of how many later submissions completed before it.
- Genuine retry (`tests/native_live_genuine_retry_e2e.py:42`) validates all
  invocation boundaries first, then compares each complete phase effect map
  to precisely the new successful payloads keyed by sequence. It also applies
  the shared complete-population/body/key checks before comparing phases.
  Modeled actions, hashes, fail-first multiplicity, and process/PID receipts
  remain checked. The tick-tape caller inherits the same shared receipt fix;
  its delivery-policy metadata now accurately states eight requests in flight.
- Harness self-tests (`tests/test_native_live_equivalence_harness.py:102`)
  accept arrival permutations while retaining full expected payload equality.
  Existing negative coverage for bytes, keys, unique/count-complete effects,
  boundaries, and post-restart success remains. Added negative cases cover
  missing final selections, sequence gaps/aliases, missing first attempts,
  duplicate event IDs, changed effects, and effects attributed to the wrong
  invocation. These tests were edited and reviewed, not executed on this Mac.

Remaining receipt slices delimit fully counted process/delivery phases:
`receipts[4:]`, `[8:]`, `[9:]`, `received[4:]`, `received_bytes[2:]`, and the
recorded invocation boundaries. They never select the first N submissions or
the crash request. Every comparison within a phase is keyed by service sequence.

## Concurrency and scope notes

The base had `max_in_flight=2` in the service fixture and `1` in the shared
equivalence receiver. Both are now **8**. Existing routing concurrency/isolation
settings remain **8**. No concurrency limit was lowered and no equality was
replaced by membership.

Three routing scenarios already used `1` at base lines 407, 529, and 583:
keep-alive connection reuse, a crash with exactly one started/uncompleted
attempt, and fatal-drain one-request accounting. Those settings and their
existing assertions are unchanged. The CR-223 fix does not serialize any
concurrent scenario to obtain a pass.

All implementation edits are Python fixtures/helpers under `tests/`. No
production, runner, C++, header, build, or benchmark file was changed. No new
dependency, generated sequence, disabled row, placeholder, push, or PR was added.
There was no need for a runner change. `REPORT.md` and `DONE` are the requested
handoff artifacts. No unresolved implementation question remains; execution
and proof are pending the supervisor.

## Review and verification handoff

The service, routing, and equivalence fixtures were re-read in full after edits.
The additional basic-live, startup, and genuine-retry fixtures and the shared
helper were re-read after edits; the modified tick-tape caller and harness
checks were also re-read. The full fixture diff was reviewed against the
inventory and the original assertions.

Mechanical source review only:

- `git diff --check`: exit 0, no output.
- `git diff --cached --check` before the implementation commit: exit 0, no output.
- Repeated whole-tree grep: remaining positional live hits are the documented
  singleton WebSocket assertions, completed-phase slices, and synchronous
  process receipts; no remaining multi-request completion-position comparison
  was found.
- Unit tests, preflight, profiles, corpus, benchmarks: **NOT RUN**, as explicitly
  instructed for this Mac. There are no local test/build/benchmark log paths or
  verification summary lines to report. No unit/corpus/parity PASS is claimed.
- The supervisor must run lab proof on the exact final committed tree and
  obtain the required maintainer verification/parity statuses.

`git diff --stat 7d79006208666dcd05c04e441316c37eb01b9c20..f6a72083b5b9a44b13c0549ae59faece8b5b81dd`:

```text
 tests/native_live_e2e.py                      |  24 ++++--
 tests/native_live_equivalence_e2e.py          |  82 +++++++++---------
 tests/native_live_genuine_retry_e2e.py        |  36 ++++----
 tests/native_live_receipts.py                 |  12 +++
 tests/native_live_routing_e2e.py              |   9 +-
 tests/native_live_service_e2e.py              |  21 +++--
 tests/native_live_startup_e2e.py              |  13 ++-
 tests/native_live_tick_tape.py                |   2 +-
 tests/test_native_live_equivalence_harness.py | 118 ++++++++++++++++++--------
 9 files changed, 210 insertions(+), 107 deletions(-)
```
