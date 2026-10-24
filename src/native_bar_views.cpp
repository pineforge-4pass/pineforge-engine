#include <pineforge/native_bar_views.hpp>
#include <pineforge/run_failure.hpp>

#include <stdexcept>

namespace pineforge {
inline namespace native_bar_views_v1 {

NativeInstrumentFeedReadView native_instrument_feed_view(
        const NativeInstrumentFeed& owned,
        const NativeInstrumentFeedView* borrowed) noexcept {
    if (borrowed != nullptr)
        return {borrowed->bars, borrowed->close_ms, borrowed->bars.size};
    return {{owned.bars.data(), owned.bars.size()},
            owned.close_ms.data(), owned.close_ms.size()};
}

std::uint64_t native_intrabar_path_digest(
        const IntrabarPath& path, const NativeLowerPathSource* source) {
    if (source == nullptr)
        return native_run_spec_v3::native_intrabar_path_digest(path);
    const auto* lower = path.lower();
    if (lower == nullptr || !lower->bars.empty())
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "lower source lost its metadata");
    struct Fold {
        std::uint64_t h = UINT64_C(1469598103934665603);
        std::size_t seen = 0;
        std::size_t count = 0;
        void bytes(const void* data, std::size_t n) noexcept {
            const auto* at = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < n; ++i) {
                h ^= at[i];
                h *= UINT64_C(1099511628211);
            }
        }
        void u(std::uint64_t value) noexcept { bytes(&value, sizeof(value)); }
        void i(std::int64_t value) noexcept { bytes(&value, sizeof(value)); }
        void d(double value) noexcept { bytes(&value, sizeof(value)); }
    } fold;
    fold.u(path.value.index());
    fold.u(lower->tf.size());
    fold.bytes(lower->tf.data(), lower->tf.size());
    fold.i(lower->samples);
    fold.u(static_cast<std::uint64_t>(lower->distribution));
    fold.u(lower->volume_weighted ? 1u : 0u);
    fold.i(lower->volume_weighted_min_samples);
    fold.i(lower->volume_weighted_max_samples);
    fold.u(static_cast<std::uint64_t>(lower->sample_eligibility));
    fold.count = source->count();
    fold.u(fold.count);
    const bool complete = source->for_each(
        [](void* state, const Bar& bar) noexcept {
            auto& f = *static_cast<Fold*>(state);
            if (f.seen == f.count) return false;
            ++f.seen;
            f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close);
            f.d(bar.volume); f.i(bar.timestamp);
            return true;
        }, &fold);
    if (!complete || fold.seen != fold.count)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "lower source count and walk disagree");
    return fold.h;
}

} // inline namespace native_bar_views_v1
} // namespace pineforge
