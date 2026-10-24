# O1W B3 fixture and remote acceptance

The eight CSV bars drive three entries: long, reverse to short, reverse to
long. The real Pine host must report two reversal closes plus the final
open-at-end row, eight processed input/script bars and eight equity points.
`summary_only.json` pins those populations and the unsupported-mode response.
The fixture does not supply expected metrics computed by a replacement fold.

`test_o1_report_contract` checks that populated legacy behavior and compiles
the versioned declarations. Round 2 also implements TEST-ONLY providers over
four explicit completed rows and four equity points. It checks by-index
values, bounded pages, forward/backward row order (including ties and
nonmonotonic exit times), the newest-row tail barrier, exact floating bits,
early stopping, invalid/overflowing ranges, bounded text bytes and detached
point copies surviving provider destruction. The two directions produce
different known PnL folds, so a forward scan cannot substitute for backward.
These mock providers are consumer checks, not a storage/memory proof.

`o1_report_legacy_provider` builds the same real
strategy as a module. Its exact `legacy.exports` allowlist hides all newer
entry points, keeping it a genuine legacy provider when runtime support lands.
The transport test first checks the full report and the eight actual callback
timestamps, then requests SUMMARY_ONLY. A typed refusal must occur before
any callback. Missing providers, generic option errors and a full/empty
success report all fail the test; there is no skipped/xfail route.

Run nothing here on the executor Mac. On the Linux lab:

1. Compile the new C++ unit first against the authenticated frozen
   previous-epoch header closure, with only that closure and its matching
   generated headers on the include path. Do not add the changed checkout's
   include directory as a fallback. Record the compiler's first diagnostic.
   The expected diagnostic is
   `unsupported report mode: SUMMARY_ONLY (pineforge-report/v1 declarations absent)`.
   This is an expectation, not an executed receipt.
2. Also compile the updated unit against the authenticated **round-1** header
   closure, with no round-2 include fallback. This closure already contains
   `report_mode.h` and the round-1 engine methods, so the intended first
   diagnostic is now
   `O1 report visitor declarations absent (round-2 report_pages.hpp required)`.
   Record the actual first diagnostic and compiler exit status remotely.
   Do not replace this compile with a header-presence assertion or count an
   unrelated missing dependency as the intended failure.
3. Build the legacy provider against the frozen base and execute the transport
   test against the unchanged `583c2217d2` runner. Its populated control must
   succeed first. The expected RED is the mismatch between the runner's
   `run_request_invalid` unknown-option response and the fixture's
   `report_mode_unsupported`. Record the actual stdout/stderr, exit status and
   first failing assertion. This expectation has not been measured.
4. Run preflight, complete all-target release/debug verification and the named
   CTest rows on the integrated tree. The two CTest names are
   `test_o1_report_contract` and `test_o1_report_mode_unsupported`. The latter
   is a Linux source-layer integration row; sanitizer/coverage profiles do not
   load the ordinary Python-process module. Run the campaign/corpus only
   through the supervisor, respecting the derived-feed/build concurrency rule.

The transport owner implements the negotiated refusal without changing this
fixture to accept generic `run_request_invalid` or FULL fallback. Successful
SUMMARY_ONLY metric/replay/memory tests remain the implementation lanes'
obligation; a correct rejection is not evidence that summary execution works.

No compiler, script, CTest row or corpus was run by the contract lane. No
fail-before or green receipt is claimed.
