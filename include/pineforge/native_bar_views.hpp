#pragma once

// CR-150 P00 declarations only. DESIGN.md revision 6, section 2.2:337-378.
// This separate namespace is the side channel; native_run_spec_v3 and
// native_host.hpp keep their existing layouts (2.2:339-345; 6:1328-1329).
#include <pineforge/bar.hpp>
#include <pineforge/native_run_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

/// RESOLVED V2: completed below with the protocol approved by CORE-TOP's
/// rev 6 amendment (10-18 19:24), proposal plus required changes 1-9.
/// DESIGN coordinates use the baseline identified in the contract map.
class NativeLowerPathSource;

/// Effective feed data (DESIGN 2.2:354-375; RESOLVED V1). Owned feeds keep
/// their independently observable close count for validation and the digest's
/// missing-close zero fallback. A borrowed feed has close_count == bars.size.
struct NativeInstrumentFeedReadView {
    NativeBarView bars;
    const std::int64_t* close_ms;
    std::size_t close_count;
};

/// Explicit read context (RESOLVED V1, required changes 1-4). Zero feed count
/// means owned vectors for every feed; the table pointer is then ignored.
/// Otherwise the nonnull table count equals the spec's metadata count, and
/// slot i binds entry i. Null slot: use owned vectors. Nonnull slot: present
/// borrowed data, including {{nullptr,0},nullptr} for an installed empty feed;
/// it never falls back to owned data. No metadata entry means absent.
/// Metadata/columns stay in NativeInstrumentFeed, with empty owning bar/close
/// vectors for a borrowed slot. Mixed owned/borrowed slots are allowed.
/// Nonempty borrowed arrays are nonnull, immutable, and have bars.size rows.
///
/// Consumer staging copies descriptors and the table, not the arrays. Caller
/// descriptors/table may die after staging; arrays live through every use of
/// the accepted binding, through the public run exit. Installed-feed owners
/// also keep arrays alive until replacement, clearing or strategy_free.
/// The active query exposes no pending slot. Context pointers are borrowed
/// until replacement/detachment/destruction; the segment is empty until the
/// kernel selects one. The const source prevents readers selecting a segment.
///
/// At every public run exit, including abort/error, detach tables/source and
/// segment, releasing borrowed pointers after retaining scalar counts/content
/// digests for identity. A detached Ready binding is never admitted: that is
/// engine_invariant. A lower-path source bound at stream_begin is likewise
/// engine_invariant. Staging/query are private consumer members, not functions
/// taking a public host (required changes 1-2).
struct NativeBarViews {
    const NativeInstrumentFeedView* const* instrument_feeds = nullptr;
    std::size_t instrument_feed_count = 0;
    const NativeLowerPathSource* lower_path = nullptr;
    NativeBarView lower_segment{nullptr, 0};
};

/// Common accessor (DESIGN 2.2:355-375). Always select a nonnull override,
/// including a present-empty one; otherwise expose owned data and its actual
/// close count. No padding/copying. The caller MUST pass its slot or nullptr
/// explicitly: no default argument (required change 3).
NativeInstrumentFeedReadView native_instrument_feed_view(
    const NativeInstrumentFeed& owned,
    const NativeInstrumentFeedView* borrowed) noexcept;

/// Existing full validation and zero-fee normalization over effective data.
/// At binding, table/spec count mismatch or nonempty null bars reports
/// {InvalidInstrumentFeedBar, InstrumentFeedBars}; nonempty null closes
/// reports {InvalidInstrumentFeedClose, InstrumentFeedClose}. A borrowed slot
/// with nonempty owning bars/closes reports the corresponding bar/close pair.
/// Existing key/timeframe/duplicate, bar value/order, close length/bounds and
/// column checks keep their order and typed calendar/allocation catches.
/// A source requires lower_tf metadata with empty owning lower bars; otherwise
/// report {InvalidIntrabarPath, IntrabarTimeframe}. No new C failure code.
///
/// Also route the adapter's projection validation through this overload,
/// using the context built from its store. The host stages after lending feed
/// bytes and before projection (required change 5). Pending state is not read
/// through the consumer's active query. Context-free v3 APIs stay unchanged.
NativeRunSpecValidation normalize_native_run_spec(
    NativeRunSpec& spec, const NativeBarViews& views) noexcept;

/// The same feed-only validation over effective data, retaining all existing
/// refusal ordering and typed catches (DESIGN 2.2:361-363).
NativeRunSpecValidation validate_native_instrument_feeds(
    const std::vector<NativeInstrumentFeed>& feeds,
    const NativeBarViews& views) noexcept;

/// The same feeds-first, auxiliary-feed, then subscription judgement over
/// effective data, also at both begin declarations (DESIGN 2.2:361-363).
NativeRunSpecValidation validate_native_timeframe_subscriptions(
    const std::vector<NativeTimeframeSubscription>& subscriptions,
    const std::string& input_tf,
    bool timeframe_undetected,
    const std::optional<NativeAuxiliaryFeed>& auxiliary_feed,
    const std::vector<NativeInstrumentFeed>& instrument_feeds,
    const NativeBarViews& views) noexcept;

/// Same count, metadata, ordered OHLCV/timestamp/close/columns fold and feed
/// NaN canonicalization (DESIGN 2.2:364-375). Valid live descriptors are a
/// precondition; owned close lengths remain total, missing closes fold zero.
std::uint64_t native_instrument_feeds_digest(
    const std::vector<NativeInstrumentFeed>& feeds,
    const NativeBarViews& views) noexcept;

/// Portable spec fold with an explicit context, sharing the existing fold
/// and relative run base; never infer external storage through a registry.
/// May re-walk a source and propagate exceptions; never invent a failure hash.
/// The one-argument v3 form remains the owned-value API.
///
/// DESIGN 5 timing amendment (required change 8): configure takes immutable
/// feed/lower component digests eagerly after normalization succeeds and
/// before its first member write. Lower-walk exceptions propagate unchanged;
/// nothing is published, and they are NOT converted into validation refusals.
/// Keep these components through combined-cache invalidation and detachment,
/// so the noexcept continuation query never walks released input. The outer
/// spec fold may stay lazy; standalone context overloads may walk and throw.
std::uint64_t native_run_spec_digest(
    const NativeRunSpec& spec, const NativeBarViews& views);

/// Four-operation source (DESIGN 2.2:350-351; 5:1237-1262, as amended).
/// Null source means owned-vector fallback; a nonnull source is present even
/// when count() == 0. The consumer exclusively owns the provider from applied
/// configure to detachment at public run exit. Immutable borrowed input lives
/// through that exit; destruction must not read it. Calls are serialized, with
/// no concurrent/reentrant walks. Concrete factories/policy/storage stay L16's.
/// Every operation exposes the same complete logical path. Whole-input fallback
/// is decided before emitting any aggregated prefix; no mixed prefix/suffix.
class NativeLowerPathSource {
public:
    /// Synchronous ordered callback; bar lives for this call only. true
    /// continues, false stops normally. Context may be null; callback is
    /// neither retained nor allowed to throw.
    using Visitor = bool (*)(void* context, const Bar& bar) noexcept;

    virtual ~NativeLowerPathSource() noexcept = default;

    /// Fixed lifetime row count, including zero (required change 7). The
    /// first call may walk or use the construction pre-scan; subsequent calls
    /// return the cached scalar in O(1). No segment/look-ahead invalidation.
    /// Operational failure throws. Overflow throws coded std::overflow_error,
    /// run_options_rejected with option intrabar_path, never a wrapped count.
    virtual std::size_t count() const = 0;

    /// Fresh whole-path walk, true only if complete and false only for visitor
    /// stop. Null visitor is coded engine_invariant / std::logic_error. Source
    /// failures throw. Never overwrite active segment/look-ahead, including
    /// when a digest is read inside a callback.
    virtual bool for_each(Visitor visitor, void* context) const = 0;

    /// Ordered contiguous rows with begin_ms <= timestamp < end_ms. Equal
    /// bounds give an empty view; reversed bounds are coded engine_invariant
    /// / std::logic_error. Each bound is non-decreasing across calls; windows
    /// may repeat OR overlap, including overlap rows. A decreasing bound is
    /// the same invariant error (required change 6). Repeats return the same
    /// active storage. Replace a window only after its script bar finishes.
    /// Prepare look-ahead here; failures expose no partial new segment.
    virtual NativeBarView segment(
        std::int64_t begin_ms, std::int64_t end_ms) = 0;

    /// Null before the first segment; otherwise the first path bar at/after
    /// its end, or null at exhaustion, including for an empty segment. The
    /// separate one-bar buffer stays fixed until a different segment is
    /// selected. It may skip multiple empty chart bars; repeated queries do
    /// not advance or overwrite the active segment. Within a segment, use its
    /// next contiguous row; only the last row needs this look-ahead.
    virtual const Bar* next_after_segment() const noexcept = 0;
};

/// Null delegates to the vector digest; nonnull replaces only lower count/
/// walk. Preserve variant/metadata and raw double bits (no NaN normalization),
/// folding count before visitation. Unexpected visitor stop or count/visit
/// mismatch is coded engine_invariant; operational failures propagate, never
/// a sentinel digest. Configure-time handling is the eager rule above.
/// Preflight still owns lower-bar validation and the INT_MAX refusal. Runtime
/// segment-generation failures are coded run_options_rejected, option
/// intrabar_path; allocation exceptions take the existing run-entry handler.
std::uint64_t native_intrabar_path_digest(
    const IntrabarPath& path,
    const NativeLowerPathSource* source);

// RESOLVED V1/V2 reader and guard work remains L15a/L15b/L16's implementation:
// the contract map lists all reads, including projection validation (change 5).
// Add this header/source and its namespace check to the native ownership guard,
// and pin the context spec-digest sibling block beside the existing fold.

} // inline namespace native_bar_views_v1
} // namespace pineforge
