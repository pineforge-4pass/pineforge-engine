#pragma once

// CR-150 P00 declarations only. DESIGN.md revision 6, section 2.2:334-375.
// This separate namespace is the side channel; native_run_spec_v3 and
// native_host.hpp keep their existing layouts (2.2:336-342; 6:1305-1306).
#include <pineforge/bar.hpp>

#include <cstddef>
#include <cstdint>

namespace pineforge {
inline namespace native_bar_views_v1 {

/// Borrowed OHLCV span, exactly the layout specified in 2.2:344-345.
/// The caller owns the bars. No owning vector or copied bar is introduced.
struct NativeBarView {
    const Bar* data;
    std::size_t size;
};

/// Borrowed instrument feed, exactly 2.2:346. bars.size is also the number
/// of close_ms entries. Validation still checks the same bars, close times,
/// ordering and column lengths as today's vector feed (2.2:358-360).
/// The borrowed-feed caller keeps both arrays alive until replacement,
/// clearing or strategy_free (6:1298-1304).
struct NativeInstrumentFeedView {
    NativeBarView bars;
    const std::int64_t* close_ms;
};

/// Lower-path side-channel type named by 2.2:347-348 and section 5:1214-1239.
/// Operations in DESIGN 2.2:347-348 count and visit the whole path,
/// select a segment, and return the next bar after it. Counting and visiting
/// re-walk the path without retaining it; the segment supplies the current
/// script bar's half-open range.
/// Its buffer stays fixed for the whole script bar, until the next one.
/// Look-ahead uses a separate cursor/one-bar buffer and never overwrites
/// that segment, even across empty chart bars (5:1230-1234).
///
/// OPEN V2: DESIGN names these operations but gives no typed signatures,
/// visitor protocol or dispatch/error model. Only the class is forward-
/// declared here; no speculative vtable or function signature is frozen.
class NativeLowerPathSource;

// Binding obligations for L15a (2.2:349-375): stage before begin with
// the staging operations specified in DESIGN 2.2:349-350, then drop
// staged state with the run. Keep NativeRunSpec metadata and empty owning
// bar vectors for view-backed entries. All reads must use the one accessor
// that returns the staged view when present, including validation and both
// cached/uncached digests, never just the kernel delivery loop.
//
// Digests fold the same count, bars and closes in the same order, with the
// same quiet-NaN canonicalization; hashed host counts read the view's count
// too (2.2:369-372). No digest/continuation identity changes are permitted.
// L15a adds this header and its source to the native ownership guard and
// adds the native_bar_views_v1 namespace check (2.2:373-375).
//
// Source-guard read-set: docs/design/cr150-contracts.md, "L15a read inventory"
// (2.2:354-368; 5:1198-1205). That inventory includes delivery, validation,
// counts, digests, store upkeep and the lower path, with DESIGN source refs.
// OPEN V1: staging signatures, feed binding/lookup and the distinction
// between an absent staged view and a present empty feed are unspecified.
// No consumer/host/frozen-header declaration is changed to guess them.

} // inline namespace native_bar_views_v1
} // namespace pineforge
