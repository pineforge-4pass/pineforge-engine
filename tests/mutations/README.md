# Dormant-book mutation patches

These patches deliberately introduce dormant-book defects so the named tests
must fail. They replace product preprocessor hooks and are applied only in
scratch worktrees, never as build definitions or runtime switches.
Every failure expectation below is a layout estimate from source, not measured.

For each manifest row, start a separate scratch worktree at the unmodified
revision being checked. Run `git apply --check` on that row's patch, apply only
that patch, rebuild both the library and the named test executable, then run
the named test. Debug assertions are required for metadata alignment, command
waking and index membership checks. Also run middle-spill and keepalive checks
under `_GLIBCXX_DEBUG` and ASAN. Save the unmodified passing output, mutated
output and the executable's own exit status. A setup or compiler failure, a
missing fixture, a wrapper exit or a marker alone does not prove detection.
Compare transcript output with the frozen eager build where equality is the oracle.

Apply patches individually. Hunks include at least three unchanged lines on
each side; check application against the revision being tested.
`M25.patch` and `M26.patch` insert faulty keepalive behavior directly at the
allowance sweep's watermark update. No empty product helper or extra
begin/complete calls are needed to expose these defects.

The driver permission patches each change one class and trigger combination.
The former DRIVER_OBSERVED, CARRIED, TICK_AFTER_CALC, CONFIRMED_OPEN,
CONFIRMED_EXCURSION, CONFIRMED_AFTER_CALC and CURRENT aliases refer to the
corresponding LIMIT/OTHER patch, rather than additional mutations.
`DriverMatrixHost` checks actual dormant permissions against the independent
eager policy for every recorded combination, including both allowed and refused cases.

`M10.patch` tests the finite-segment predicate directly.
`native_bar_structurally_valid` rejects nonfinite public inputs before matching.
The `eligible()` guard uses the shared `dormant_segment_finite` predicate;
the patch omits that guard there. The reachability test compares the production
predicate with independently classified finite, infinite and NaN endpoints.
The margin and join/wake adapters still call their production functions.
There is no duplicate finite guard or product mutation switch; public input
validation is unchanged.

For assertion failures, inspect the assertion expression: libc may vary its
file, function and executable prefixes. Other failures appear in the test's
caught-exception output. An earlier independent assertion may detect a defect
before the check listed below; record its actual expression and cause.
For `M24.patch` and `M-middle-in-match-path.patch`, run the transcript target
with `--notification-middle-spill`, or use the CTest row
`test_native_dormant_book_notification_middle_spill`. The callback first
proves the consumed winner is absent, then uses fresh lookup I/O to prove a
dormant row before later winners moved to the spilled tier. Later points join
newer siblings; grouped tail rows and a forced eager point require promotion
while candidates are retained. The first patch fails `make_book_room`'s
publication assertion; the second fails candidate selection's scope assertion.
Both use `native retained candidates require back-only spills`.
The kernel itself must have Debug assertions enabled; compiling only the test
translation unit with `-UNDEBUG` against a Release kernel is insufficient.

`M-first-ensure-throws.patch` rethrows the first allocation failure while
creating the book implementation, instead of falling back to eager matching.
The allocation-fault test varies the failure position and must report that
the first book allocation failure never fell back to eager matching.

`M-resident-window.patch` removes both resident suffix refresh calls. Its
transcript callback proves that a consumed t=0 winner leaves a resident dormant
row beyond the watermark, reads its allowance and replaces it by id. The
failure must identify the stale callback allowance.

| Patch | Deliberate error | Test target | Required failure |
|---|---|---|---|
| M1.patch | Erase spilled stamp and dormancy. | `test_native_dormant_book_switch` | Eager and dormant transcripts differ. |
| M2.patch | Keep the current sequence stamp above an unfinished watermark. | `test_native_dormant_book_switch` | A callback spill at t=0 projects the unswept suffix incorrectly. |
| M3.patch | Omit promotion's index erase. | `test_native_dormant_book_switch` | `level_index->record_count()` disagrees with the independent membership count. |
| M4.patch | Omit promotion's aligned resident insertion. | `test_native_dormant_book_switch` | `resident.size()` no longer matches the live rows. |
| M5.patch | Omit cancellation's aligned resident erase. | `test_native_dormant_book_switch` | `resident.size()` no longer matches the live rows. |
| M6.patch | Use zero join margin. | `test_native_dormant_book_reachability` | Join and wake disagree, or the independent eager envelope disagrees. |
| M7.patch | Use strict wake boundaries. | `test_native_dormant_book_reachability` | A closed envelope boundary is excluded. |
| M8.patch | Omit the t=0 priority advance. | `test_native_dormant_book_transcript` | A fresh read projects the wrong allowance at the watermark. |
| M9.patch | Advance only to the first interior winner instead of completing. | `test_native_dormant_book_transcript` | The higher-priority dormant suffix remains unswept. |
| M10.patch | Omit the shared finite-segment guard. | `test_native_dormant_book_reachability` | The production predicate disagrees with independent endpoint classification. |
| M11.patch | Permit Other at CarriedOpen. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_OBSERVED_PRINT_LIMIT.patch | Flip only OBSERVED_PRINT_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_OBSERVED_PRINT_OTHER.patch | Flip only OBSERVED_PRINT_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CARRIED_OPEN_LIMIT.patch | Flip only CARRIED_OPEN_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CARRIED_OPEN_OTHER.patch | Flip only CARRIED_OPEN_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_TICK_AFTER_CALCULATION_LIMIT.patch | Flip only TICK_AFTER_CALCULATION_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_TICK_AFTER_CALCULATION_OTHER.patch | Flip only TICK_AFTER_CALCULATION_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CONFIRMED_OPEN_LIMIT.patch | Flip only CONFIRMED_OPEN_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CONFIRMED_OPEN_OTHER.patch | Flip only CONFIRMED_OPEN_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CONFIRMED_EXCURSION_LIMIT.patch | Flip only CONFIRMED_EXCURSION_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CONFIRMED_EXCURSION_OTHER.patch | Flip only CONFIRMED_EXCURSION_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CONFIRMED_AFTER_CALCULATION_CLOSE_LIMIT.patch | Flip only CONFIRMED_AFTER_CALCULATION_CLOSE_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CONFIRMED_AFTER_CALCULATION_CLOSE_OTHER.patch | Flip only CONFIRMED_AFTER_CALCULATION_CLOSE_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CURRENT_EXECUTION_LIMIT.patch | Flip only CURRENT_EXECUTION_LIMIT. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M11_CURRENT_EXECUTION_OTHER.patch | Flip only CURRENT_EXECUTION_OTHER. | `test_native_dormant_book_transcript` | Dormant permissions differ from the eager Limit/Other policy. |
| M12.patch | Omit the command-target wake. | `test_native_dormant_book_switch` | `!resident[at].dormant` fails before cancellation. |
| M13.patch | Omit kept-handle metadata rotation. | `test_native_dormant_book_switch` | `resident[i].priority` loses alignment with its live row. |
| M14.patch | Skip the spilled BookClose refresh walk. | `test_native_dormant_book_switch` | Eager and dormant transcripts differ with active spilled rows. |
| M15.patch | Ignore a carried spill-read failure in the fast-path guard. | `test_native_dormant_book_read_failure` | The carried tick reports the failure at the wrong location. |
| M16.patch | Roll back index I/O without retaining failure state. | `test_native_dormant_book_index_fault` | The failed index does not remain disabled for the run. |
| M17.patch | Project the current point above an unfinished watermark. | `test_native_dormant_book_transcript` | A fresh decode returns the wrong allowance. |
| M18.patch | Increment the resident-refresh epoch by one instead of two. | `test_native_dormant_book_core_contract` | The epoch delta is not two per refreshed resident row. |
| M19.patch | Ignore active trailing orders. | `test_native_dormant_book_transcript` | The active trail does not execute at 106 minus 2. |
| M20.patch | Omit the eligibility recheck before each scan. | `test_native_dormant_book_transcript` | An active spill during a point changes the remaining suffix. |
| M21.patch | Count a promoted row as still spilled and dormant. | `test_native_dormant_book_core_contract` | Index membership count or final-spilled-row epoch arithmetic disagrees. |
| M22.patch | Shrink the two-tick margin to one tick. | `test_native_dormant_book_reachability` | The production predicate disagrees with the independent eager envelope. |
| M23.patch | Omit the completion sweep while keeping the RAII calls. | `test_native_dormant_book_transcript` | The dormant suffix remains unswept before an interior winner or when no winner remains. |
| M24.patch | Permit middle spill with retained candidates and erase unconditionally. | `test_native_dormant_book_transcript` | The back-only spill assertion fails after callback promotion. |
| M25.patch | Re-project held side-ring copies when the watermark advances. | `test_native_dormant_book_tokens` | An already held copy changes. |
| M26.patch | Clear side-ring keepalives when the watermark advances. | `test_native_dormant_book_tokens` | The decoded keepalive ring loses a live copy before raw pointer access. |
| M-resident-window.patch | Skip resident suffix refresh after `advance` and `complete`. | `test_native_dormant_book_transcript` | Callback read and replace beyond the watermark sees a stale allowance. |
| M-middle-in-match-path.patch | Grant middle-spill permission throughout `match_path`. | `test_native_dormant_book_transcript` | Candidate selection rejects middle spills while candidates are retained. |
| M-first-ensure-throws.patch | Propagate the first book implementation allocation failure. | `test_native_dormant_book_allocation_fault` | Allocation failure does not fall back to eager matching. |
