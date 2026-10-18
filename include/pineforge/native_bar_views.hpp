#pragma once

// CR-150 P00 declarations only. DESIGN.md revision 6, section 2.2:337-378.
// This separate namespace is the side channel; native_run_spec_v3 and
// native_host.hpp keep their existing layouts (2.2:339-345; 6:1328-1329).
#include <pineforge/bar.hpp>

#include <cstddef>
#include <cstdint>

namespace pineforge {
inline namespace native_bar_views_v1 {

/// Borrowed OHLCV span, exactly the layout specified in 2.2:347-348.
/// The caller owns the bars. No owning vector or copied bar is introduced.
struct NativeBarView {
    const Bar* data;
    std::size_t size;
};

/// Borrowed instrument feed, exactly 2.2:349. bars.size is also the number
/// of close_ms entries. Validation still checks the same bars, close times,
/// ordering and column lengths as today's vector feed (2.2:361-363).
/// The borrowed-feed caller keeps both arrays alive until replacement,
/// clearing or strategy_free (6:1321-1327).
struct NativeInstrumentFeedView {
    NativeBarView bars;
    const std::int64_t* close_ms;
};

/// Lower-path side-channel type named by 2.2:350-351 and section 5:1237-1262.
/// Operations in DESIGN 2.2:350-351 count and visit the whole path,
/// select a segment, and return the next bar after it. Counting and visiting
/// re-walk the path without retaining it; the segment supplies the current
/// script bar's half-open range.
/// Its buffer stays fixed for the whole script bar, until the next one.
/// Look-ahead uses a separate cursor/one-bar buffer and never overwrites
/// that segment, even across empty chart bars (5:1253-1257).
///
/// OPEN V2: DESIGN names these operations but gives no typed signatures,
/// visitor protocol or dispatch/error model. Only the class is forward-
/// declared here; no speculative vtable or function signature is frozen.
class NativeLowerPathSource;

// Binding obligations for L15a (2.2:352-378): stage before begin with
// the staging operations specified in DESIGN 2.2:352-353, then drop
// staged state with the run. Keep NativeRunSpec metadata and empty owning
// bar vectors for view-backed entries. All reads must use the one accessor
// that returns the staged view when present, including validation and both
// cached/uncached digests, never just the kernel delivery loop.
//
// Digests fold the same count, bars and closes in the same order, with the
// same quiet-NaN canonicalization; hashed host counts read the view's count
// too (2.2:372-375). No digest/continuation identity changes are permitted.
// L15a adds this header and its source to the native ownership guard and
// adds the native_bar_views_v1 namespace check (2.2:376-378).
//
// Source-guard read-set: docs/design/cr150-contracts.md, "L15a read inventory"
// (2.2:357-371; 5:1221-1228). That inventory includes delivery, validation,
// counts, digests, store upkeep and the lower path, with DESIGN source refs.
// OPEN V1: staging signatures, feed binding/lookup and the distinction
// between an absent staged view and a present empty feed are unspecified.
// No consumer/host/frozen-header declaration is changed to guess them.

} // inline namespace native_bar_views_v1
} // namespace pineforge
