## Fix round 1 — CX-PERF441

Lane source edits ready for supervisor review; **UNVERIFIED**. Rank 1 removes
owning reflection from the admission hash path; rank 4 fuses the newly folded
events' rejection indexing with that fold. Rank 5 remains open because sharing
the cancellation lookup requires a kernel seam change outside this lane.
Required hash-byte processing and payload destruction remain. This is not a
claim that all three ranked costs or the 1.7.0 speed blocker are resolved.

This section is append-ready for `../REPORT.md`. That file was read in full,
but this session permits writes only inside `wt2`, so the supervisor must
append this section to the parent report. The continuation explicitly forbids
staging and commits; the supervisor commits the source diff. `DONE-fix1` marks
the end of this edit-only lane, not a verification or parity PASS.

### Identity and base recheck

- Worktree: `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-perf441/wt2`.
- Branch: `core/cr150-perf441-fix1`.
- Base and unchanged HEAD: `b4d20117b6d40f110a2be7f3fe3759adecba9e24`.
- Read the whole `fix1-brief.md`, the whole prior `../REPORT.md`, and
  `AGENTS.md`. The continuation replaces local verification with source
  reasoning and lab verification submitted by the supervisor.
- Re-read the ranked hunks using `git show HEAD:<file>` with numbered output,
  rather than treating the audit's `2fff6306b` or `993ff10e7d` line numbers as
  current. At this base all three priority hunks are still present:
  `src/source/pine_adapter.cpp:24198` and `:24208`,
  `src/source/pine_state_hash.cpp:779`,
  `src/source/market_admission.cpp:184`, and
  `src/source/pine_adapter.cpp:14579` / `:14610`.

All base citations below mean that exact HEAD; implementation citations mean
the working tree. Source complexity and counts are expected savings, not
measurements. Let E be the live-window events at a completed boundary, K the
events already folded by a within-bar query after any required cursor repair,
F the leaves of the newly folded events, B their canonical hash bytes, P the
sum of their path lengths, S the copied string-value bytes, C the completed
boundaries, A the materialized bracket members and L the live kernel rows.

### Rank 1 / F1 — direct canonical admission hashing

**Base hunk and cost.** At base `src/source/pine_adapter.cpp:24208`, every
completed terminal bar calls `fold_admission_events()` then `retire_prefix()`.
Base `src/source/pine_state_hash.cpp:793` repairs the global cursor and `:803`
folds each new event through owning `Field` reflection. Base
`src/source/market_admission.cpp:261` constructs `path + "." + name` and copies
string-valued leaves into `FieldValue`. Base `src/source/pine_state_hash.cpp:21`
then hashes the path, variant tag and value. Nested books and drafts are
visited at base `src/source/market_admission.cpp:305`, `:311`, and `:326`.
The successful path costs O(C + E - K + F + B), plus path/value materialization
traffic O(P + S) and temporary intermediate paths.

**Edit.** `src/source/market_admission.cpp:304` retains the owning diagnostic
writer and `:340` adds a direct hash writer over the same schema and field
order (`:375`). `HashPath` at `:321` borrows path segments from stack frames;
indices at `:363` use decimal `to_chars` into a stack array large enough for
every size_t plus brackets. There is no maximum path length or new refusal.
`src/source/market_admission_fold.hpp:14` declares the source-only hash
overload; its `:25` inline cursor helper preserves the base repair condition
without adding an out-of-line cursor call.
`src/source/pine_state_hash.cpp:780` uses that overload for hash queries.
`src/source/market_admission.cpp:228` uses it at completed boundaries, reached
through `src/source/pine_adapter.cpp:24208`.

**Expected saving and coefficients.** Exactly F owning Field constructions
and destructions, their variant dispatches in the hash visitor, and their
path/value copies disappear from this fold. Hash projection performs zero
per-leaf dynamic allocations. The old allocator-call count depends on string
sizes, SSO and library growth policy; no fixed allocator count is claimed.
At least P final path bytes and S string-value bytes no longer need to be
materialized into owning Fields, in addition to intermediate path copying.
The B FNV byte updates remain exactly B; they are not claimed as saved.

The base schema was counted again at `src/source/market_admission.cpp:266`
(11 birth leaves), `:273` (24 configuration leaves), `:286` (58 command
observation leaves before optional sizing), and `:326` (five event/array
leaves). A CommandEvent with empty before/removed arrays therefore replaces
63 owning Fields with 63 direct leaf writes, or 68 with original sizing
present; two command string-value copies (`id`, `oca_name`) disappear on
each such traversal. ReviewEvent's fixed part is 36 leaves (kind + four
receipt + 24 configuration + three scalar + four array-size leaves);
SizingEvent is 18 (kind + four receipt + incarnation + ten sizing + two
equity leaves). Every nested book/draft still contributes its complete
original leaves. These counts are per event, not an assumed event rate.

**Byte-identity argument.** For a name literal containing N bytes including
its terminator, `path.size + N` is the old path length: parent length plus
one dot plus N - 1 name bytes. The writer emits that uint64 length, then
parent segments in order, dot and name, then the original uint64 variant tag.
Bool/uint64 use tag 0 and `u`; int/enums/int64 use tag 1 and `i`; double uses
tag 2 and `d`; string uses tag 3 and `s`. The unchanged
`include/pineforge/engine.hpp:355` sink supplies the same FNV recurrence and
NaN/negative-zero normalization. Splitting a string into adjacent bytes()
calls preserves the recurrence. Diagnostic fields, their spelling, indices,
order, presence leaves and value conversions remain in the same schema.
The old owning hash helper stays for placement Draft hashing at
`src/source/pine_state_hash.cpp:153`; that separate pre-existing cost is
outside this #441 admission-event fold.

### Rank 4 / F4 — fuse the retirement rejection scan

**Base hunk and cost.** Base `src/source/market_admission.cpp:184` validates
unfinished producers, count overflow, exact window/count, final sequence
and empty digest. `:197` then separately visits E events to remember the
newest rejected command, assigns the prefix and clears payloads at `:214`.
That adds E loop iterations after the hash fold's E - K iterations, plus
the required O(payload destruction) work. Empty boundaries still enter the
fold and retirement functions and check their invariants.

**Edit.** `src/source/market_admission.cpp:187` shares the identical retirement
checks between the original `retire_prefix()` API and the new completed-boundary
fold at `:228`. The latter derives the rejection bar as it folds new events
(`:246`). It still reads the K already-folded events at `:242` to recover
their rejection results, without hashing them again. The rejection predicate
and event ordering are unchanged. The prefix is committed only after all
retirement checks succeed (`:255`), and clear() still destroys the window
(`:259`). No new persistent state or hash leaf is added.

**Expected saving and coefficients.** The two event traversals, totaling
(E - K) + E iterations at the base, become one new-event traversal plus the
K-event already-folded scan, totaling (E - K) + K = E iterations.
The saving is exactly E - K separate event-loop iterations per successful
window. The outcome test still runs once per event; its removal is not
claimed. On the ordinary no-intermediate-query path K = 0, this removes one
complete E-event second walk. If K = E, this rank saves no event iterations.
Payload destruction and vector capacity are unchanged.

The empty fast path at `src/source/market_admission.cpp:231` checks both an
empty vector and no unfinished producer, synchronizes all three cursor
scalars to the actual retired prefix and returns. It replaces the two-function
fold/retirement sequence with one boundary call and constant checks/assignments
per such boundary. With an empty window, those synchronized values necessarily
satisfy the original count/last/digest/overflow checks. Unfinished producers
still take the original coded logic_error through `check_retirement`; the
fast path does not return an unchecked stale digest or turn that failure into
success. Full-history mode still folds without retiring events or updating
the rejection prefix. Insertion, retain(), reset and journal replacement
retain the original cursor-repair rule.

**Not fixed.** The K-event scan after an intermediate hash query remains.
Removing it requires preserving additional rejection-index cache state with
correct insertion/retain/reset semantics, beyond this no-layout-change fold.
Payload destruction remains at the same boundary because retaining those
objects would defeat retirement's lifetime/capacity contract; existing
`tests/test_admission_journal_window.cpp:279` checks payload release.

### Rank 5 / F5 — OPEN; no safe source-only removal

**Base hunk and cost.** Base `src/source/pine_adapter.cpp:14579` copies and
erases a bracket roster before cancellation. At `:14608` it issues the
settled-prefix sentinel cancels, and `:14610` / `:14613` checks each of A
materialized handles using the current kernel `find_live()` before choosing
the real or never-issued target. This rank is present, not already removed.

At this base `src/native_order.cpp:1073` implements find_live(), not the
audit's old :1108. Its `book_position()` at `:666` bisects to at most 16
candidates; the final walk can also inspect their bounding row, for at
most 17 final row-key reads. `repriced_position()` at `:699` adds the
repriced-key lookup when needed. The added precheck costs approximately
O(A * (log(L + 1) + 17)), plus the repriced search, on bracket-cancel calls.
No per-event allocation is attributable to this lookup.

**Edit and expected saving.** None; expected saving for rank 5 is zero.
Passing the historical handle directly is not byte-preserving:
base `src/source/pine_adapter.cpp:3851` roots NotWorkingEvent.target in
placement retirement, and base `src/source/pine_state_hash.cpp:332` hashes
the retained/erased placement population. Adapter `live_handles_` is not an
authoritative replacement: terminal receipts are consumed later at base
`src/source/pine_adapter.cpp:6515`, and cancellation synchronously drains
parent-dependent terminal state in `src/native_execution_consumer.cpp:11600`.
A precomputed whole-roster liveness mask could become stale between cancels.

**Owner follow-up.** `src/native_order.cpp:2423` (`prepare_cancel`) and
`:2670` (direct cancel classification) already classify the target, reached
from `src/native_execution_consumer.cpp:11546`. A generic cancellation seam
that shares that classification while preserving the caller's nonworking
event target could remove the adapter precheck. Its declaration/implementation
belongs to the kernel seam owner; this brief expressly forbids changing
another lane's seam. No such file or frozen header was edited. Command count,
sentinel targets, synchronous ordering and retirement remain unchanged.

### Other audit ranks and exclusions

Ranks 2/3/6 remain already absent at this base: `git show HEAD` shows
replacement without scratch rebuild at
`include/pineforge/source/pine_adapter.hpp:783`, erase without pending-id
removal at `:804`, and a retained-row/minima query without pending-vector
discard at `src/source/pine_adapter.cpp:902`. None is credited as a new
saving. No borrowed-feed validation/digest, placement retirement rule,
bracket membership/digest, measurement configuration, workflow, test
expectation or other lane's test/CMake surface was changed.

### New lane-owned controls for the lab

`tests/test_admission_fold_perf441.cpp:111` compares direct hashing with the
old owning Field/variant protocol from `:24`. It covers all three event
variants, nested books/reviews/drafts, multi-digit indices, repeated shared
observations, a 1024-byte root path, long/embedded-NUL strings, signed and
unsigned values, infinities, negative zero, and a noncanonical NaN payload,
with three independent starting sink values.

At `:128` it compares fused retirement with the separate fold/retire
protocol for K = 0, 1, 2, 3 over successive nonzero prefixes; `:182` covers
unfinished producers, insertion before the cursor, retain(), reset,
replacement, stale empty cursors and actual weak_ptr payload release.
`:233` covers full-history retention and repeat boundaries.
`tests/cr150.d/core-cr150-perf441.cmake:1` registers only this lane's row,
conditionally on the source layer, through the existing unit-test helper.
No existing expectation was changed or disabled.

### UNVERIFIED

Nothing was built or run for verification in this continuation. No compiler,
cmake, ctest, test executable, repository Python/shell script, package install,
preflight, lab/spot job, corpus sweep or performance harness was invoked.
Only file reads/searches, git inspection and edits were used. The reported
prior local preflight attempt was stopped by LB-446 with exit 77; it is not
passing evidence.

- Preflight/source guards: UNVERIFIED.
- C++ compilation and complete unit/ABI suites: UNVERIFIED.
- New unit fail-before compilation against frozen headers: UNVERIFIED;
  no diagnostic was produced or claimed. Source inspection shows the new
  helper header and fold_and_retire() API absent from the base.
- Full corpus, every trade, diagnostic leaves, per-bar/final digests and
  parity: UNVERIFIED; static byte reasoning above is not a lab verdict.
- Sink-OFF 100k/1M throughput and the +10% acceptance bar: UNVERIFIED.
- Build/test/measurement log paths and summary lines: none.

The supervisor submits all execution to the lab (or SM-CX spot host). Required
maintainer statuses and measurement acceptance remain outstanding.

### Diff and handoff

No git add, commit or push. `git diff --stat b4d20117b6..HEAD` is empty because
HEAD is unchanged. The actual tracked working-tree diff is:

```text
 include/pineforge/source/market_admission.hpp |   3 +
 src/source/market_admission.cpp               | 189 +++++++++++++++++++++-----
 src/source/pine_adapter.cpp                   |   3 +-
 src/source/pine_state_hash.cpp                |  32 +----
 4 files changed, 165 insertions(+), 62 deletions(-)
```

Additional untracked source/test files, counted with read-only git no-index
diffs against /dev/null: `src/source/market_admission_fold.hpp` (41 added
lines), `tests/test_admission_fold_perf441.cpp` (258 added lines), and
`tests/cr150.d/core-cr150-perf441.cmake` (5 added lines). The source/test total
is seven files, 469 insertions and 62 deletions, excluding this handoff and
`DONE-fix1`. No-index diff exit 1 denotes the expected added-file difference.
The tracked `git diff --check` exited 0; that is a whitespace check only.

Deviations/open items: supervisor commit instead of lane commit (explicit
continuation instruction); append this local section to the parent report
(sandbox write boundary); all verification and measurements deferred
(Mac guard/continuation); rank 5 needs a kernel-owner seam; rank 4 still
destroys payloads and reads K previously folded events; rank 1 still hashes
every required canonical byte. Suggested source commit message:
`Remove per-leaf admission hash allocations and fuse per-bar retirement scan`.
