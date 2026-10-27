# Dormant-book mutation patches

These patches replace the former product preprocessor hooks. They are lab
inputs, never build definitions, runtime switches or patches to the delivery
tree. All red expectations below are source predictions; no mutation was run
on the edit Mac.

The supervisor uses a separate scratch copy of the final HK1 tree for EACH
manifest row, applies exactly its patch, checks application there, rebuilds
the library AND the named executable, and runs the named row. Use Debug
assertions for I-1/I-4/I-5 rows; also run the middle-spill and keepalive rows
under _GLIBCXX_DEBUG and ASAN. Keep unmutated green results, mutant output and
the executable's own exit status. A setup/compiler failure, a missing private
fixture, a wrapper exit or a marker alone is not a kill. Compare T3/T5 stdout
to the frozen base when a row's oracle is transcript equality.

Patch hunks carry at least three unchanged lines on each side. Their
application was reviewed against source text only here; git apply --check
is a LAB step, not a command executed on this Mac. Apply patches individually.
M25/M26 put the former bad keepalive bodies directly at sweep's watermark
update; the removed empty product helper and its extra begin/complete calls
are unnecessary to expose those two defects.

The driver-cell patches use their original M11 names. The former
DRIVER_OBSERVED/ CARRIED/ TICK_AFTER_CALC/ CONFIRMED_OPEN/
CONFIRMED_EXCURSION/ CONFIRMED_AFTER_CALC/ CURRENT aliases select the SAME
respective LIMIT/OTHER patch, not a second mutant. DriverMatrixHost now checks
the actual dormant permission against the independent base policy for every
recorded class and trigger cell; both enabled and disabled answers are tested.

M10 is a T8 property kill, not a public nonfinite-run claim. The previous C-T
report records that native_bar_structurally_valid rejects nonfinite inputs
before matching. The exact eligible() finite guard now lives in the shared
dormant_segment_finite predicate; M10 omits that same guard there. T8 calls the
production predicate against independently classified finite/infinity/NaN
endpoints. The margin and join/wake adapters retain their actual functions.
No second finite-gate implementation or product mutation switch was added.
Upstream public validation remains unchanged.

For assertion rows, the prefix column identifies the assertion-expression
prefix in libc's diagnostic (file/function/executable prefixes vary by host).
Other prefixes are the test executable's own caught-failure line. An earlier
independent assertion may kill a mutant before the listed later row; record
that actual expression and reason instead of substituting the predicted text.
For M24 and M-middle-in-match-path, run the transcript target with
`--notification-middle-spill` (ctest row
`test_native_dormant_book_notification_middle_spill`). The callback first
proves that the consumed winner is absent, then proves via fresh lookup I/O
that a dormant row before later winners moved to the spilled tier. Later
points join its newer siblings; grouped tail rows and a forced eager point
then require promotion while candidates are retained. M24 is caught at
make_book_room's independent publication assertion; M-middle-in-match-path
is caught at candidate selection's scope assertion. Both diagnoses contain
the exact prefix `native retained candidates require back-only spills`.
These assertion kills require a Debug kernel; do not infer them from a
Release test TU's -UNDEBUG flag alone.

M-first-ensure-throws restores the original allocation divergence without
adding a product hook: rethrow instead of degrading the first pimpl
allocation. Its fault-position sweep must fail with
`test_native_dormant_book_allocation_fault: HK1 first Book ensure allocation fault never degraded to eager`.

| ID | Mutant | Expected-red row | Expected failure text prefix |
|---|---|---|---|
| M1 | Erase spilled stamp/dormancy. | T4 eager/dormant transcript comparison | `test_native_dormant_book_switch:` |
| M2 | Leave the current sequence stamp above an unfinished watermark. | T4 t=0 callback spill / unswept suffix | `test_native_dormant_book_switch:` |
| M3 | Omit promotion's index erase. | I-5 index census after promotion | `Assertion `level_index->record_count()` |
| M4 | Omit promotion's aligned resident insertion. | I-1 promotion metadata alignment | `Assertion `resident.size()` |
| M5 | Omit cancellation's aligned resident erase. | I-1 cancellation metadata alignment | `Assertion `resident.size()` |
| M6 | Use zero join margin. | T8 join/wake equality or independent base envelope | `test_native_dormant_book_reachability: T8` |
| M7 | Use strict wake boundaries. | T8 closed envelope boundary | `test_native_dormant_book_reachability: T8` |
| M8 | Omit the t=0 priority advance. | T5 t=0 watermark / fresh read | `test_native_dormant_book_transcript: T5` |
| M9 | Advance only to the first interior winner instead of completing. | T3 complete-before-interior | `test_native_dormant_book_transcript: T3 complete left the higher-priority dormant suffix unswept` |
| M10 | Omit eligible's shared finite-segment guard. | T8 classified finite/infinity/NaN endpoints | `test_native_dormant_book_reachability: T8 production finite-segment guard disagrees with endpoint classification` |
| M11 | Permit Other at CarriedOpen. | T3 driver matrix CarriedOpen/Other | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_OBSERVED_PRINT_LIMIT | Flip only OBSERVED_PRINT_LIMIT in the production class decision. | T3 driver matrix OBSERVED_PRINT_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_OBSERVED_PRINT_OTHER | Flip only OBSERVED_PRINT_OTHER in the production class decision. | T3 driver matrix OBSERVED_PRINT_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CARRIED_OPEN_LIMIT | Flip only CARRIED_OPEN_LIMIT in the production class decision. | T3 driver matrix CARRIED_OPEN_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CARRIED_OPEN_OTHER | Flip only CARRIED_OPEN_OTHER in the production class decision. | T3 driver matrix CARRIED_OPEN_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_TICK_AFTER_CALCULATION_LIMIT | Flip only TICK_AFTER_CALCULATION_LIMIT in the production class decision. | T3 driver matrix TICK_AFTER_CALCULATION_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_TICK_AFTER_CALCULATION_OTHER | Flip only TICK_AFTER_CALCULATION_OTHER in the production class decision. | T3 driver matrix TICK_AFTER_CALCULATION_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CONFIRMED_OPEN_LIMIT | Flip only CONFIRMED_OPEN_LIMIT in the production class decision. | T3 driver matrix CONFIRMED_OPEN_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CONFIRMED_OPEN_OTHER | Flip only CONFIRMED_OPEN_OTHER in the production class decision. | T3 driver matrix CONFIRMED_OPEN_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CONFIRMED_EXCURSION_LIMIT | Flip only CONFIRMED_EXCURSION_LIMIT in the production class decision. | T3 driver matrix CONFIRMED_EXCURSION_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CONFIRMED_EXCURSION_OTHER | Flip only CONFIRMED_EXCURSION_OTHER in the production class decision. | T3 driver matrix CONFIRMED_EXCURSION_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CONFIRMED_AFTER_CALCULATION_CLOSE_LIMIT | Flip only CONFIRMED_AFTER_CALCULATION_CLOSE_LIMIT in the production class decision. | T3 driver matrix CONFIRMED_AFTER_CALCULATION_CLOSE_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CONFIRMED_AFTER_CALCULATION_CLOSE_OTHER | Flip only CONFIRMED_AFTER_CALCULATION_CLOSE_OTHER in the production class decision. | T3 driver matrix CONFIRMED_AFTER_CALCULATION_CLOSE_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CURRENT_EXECUTION_LIMIT | Flip only CURRENT_EXECUTION_LIMIT in the production class decision. | T3 driver matrix CURRENT_EXECUTION_LIMIT | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M11_CURRENT_EXECUTION_OTHER | Flip only CURRENT_EXECUTION_OTHER in the production class decision. | T3 driver matrix CURRENT_EXECUTION_OTHER | `test_native_dormant_book_transcript: T3 dormant driver disagrees with the base Limit/Other permission matrix` |
| M12 | Omit the command-target wake. | I-4 command wake / cancellation | `Assertion `!resident[at].dormant` |
| M13 | Omit kept-handle metadata rotation. | I-1 kept-handle reprice metadata alignment | `Assertion `resident[i].priority` |
| M14 | Skip the spilled BookClose refresh walk. | T4 active-spill eager/dormant comparison | `test_native_dormant_book_switch:` |
| M15 | Ignore a carried spill-read failure in the fast-path guard. | T6 carried tick failure location | `test_native_dormant_book_read_failure: T6` |
| M16 | Roll back index I/O without latching poison. | T7 permanent index poison witness | `test_native_dormant_book_index_fault: T7` |
| M17 | Project the current point above an unfinished watermark. | T5 above-watermark fresh decode | `test_native_dormant_book_transcript: T5` |
| M18 | Increment the resident-refresh epoch by one instead of two. | T2b resident epoch arithmetic | `test_native_dormant_book_core_contract: T2b resident epoch delta is not two per row` |
| M19 | Ignore active trailing orders. | T3 active-trail 106-2 execution | `test_native_dormant_book_transcript: T3 active trail` |
| M20 | Omit the per-scan eligibility recheck. | T3 mid-point active-spill remaining suffix | `test_native_dormant_book_transcript: T3` |
| M21 | Retain a promoted row in the spilled-dormant census. | T2b final-spilled-row epoch arithmetic / I-5 | `Assertion `level_index->record_count()` |
| M22 | Shrink the two-tick margin to one tick. | T8 two-tick independent base envelope | `test_native_dormant_book_reachability: T8 production predicate disagrees with the specified base envelope` |
| M23 | Omit complete's sweep, preserving the RAII calls. | T3 complete-before-interior / no-winner completion | `test_native_dormant_book_transcript: T3 complete left the higher-priority dormant suffix unswept` |
| M24 | Permit middle spill without a candidate-free scope and erase unconditionally (both original hook bodies). | T3 --notification-middle-spill retained promotion after callback | `native retained candidates require back-only spills` (assertion message) |
| M25 | Re-project held side-ring copies when the watermark advances. | T5 frozen held ring copies | `test_native_dormant_book_tokens: T5 watermark update re-projected an already held copy` |
| M26 | Clear the side-ring keepalives when the watermark advances. | T5 keepalive lifetime before raw dereference | `test_native_dormant_book_tokens: T5 watermark update cleared the decoded keepalive ring` |
| M-middle-in-match-path | Grant middle-spill permission throughout match_path. | T3 --notification-middle-spill scope invariant | `native retained candidates require back-only spills` (assertion message) |
| M-first-ensure-throws | Propagate first pimpl allocation failure instead of degrading. | HK1 allocation-fault sweep | `test_native_dormant_book_allocation_fault: HK1 first Book ensure allocation fault never degraded to eager` |
