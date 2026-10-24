#pragma once

#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pineforge::report {

// The existing selected presentation contract holds two editable points.
// A released point goes directly to its consumer (an exact metric pass or an
// external output writer); this object never owns the released prefix.
class PresentationCaptureWindow {
public:
    static constexpr std::int64_t kPoints = 2;

    // Bind an already recorded prefix and fill data()[0..size()) from its
    // final tail. No prefix is copied or retained by this binding.
    void bind(std::int64_t logical_count) {
        if (logical_count < 0)
            refuse("native selected report: invalid metric curve counts");
        logical_count_ = logical_count;
        size_ = std::min(logical_count, kPoints);
        flags_ = {};
    }

    // Feed final points in their metric order, anchor first when selected.
    // release is called before the slot is reused, with exactly the value
    // supplied by the caller. It must not retain a reference to that slot.
    template<class Emit>
    void append_final(const pf_equity_point_t& point, Emit&& release) {
        append_metric_point(point, true, false,
            [&](const pf_equity_point_t& released, bool, bool) { release(released); });
    }

    // A summary recorder also carries the original exposure facts. Open P/L
    // cannot reconstruct them: an open position can have exactly zero P/L.
    // The selected anchor has observation=false and in_market=false.
    template<class Emit>
    void append_metric_point(const pf_equity_point_t& point, bool observation,
                             bool in_market, Emit&& release) {
        const auto countable =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto sizable = static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max() / sizeof(pf_equity_point_t));
        if (static_cast<std::uint64_t>(logical_count_) >= std::min(countable, sizable))
            refuse("selected report view: the observation count exceeds the largest curve");
        if (size_ == kPoints) {
            release(points_[0], flags_[0].observation, flags_[0].in_market);
            points_[0] = points_[1];
            flags_[0] = flags_[1];
        } else {
            ++size_;
        }
        points_[static_cast<std::size_t>(size_ - 1)] = point;
        flags_[static_cast<std::size_t>(size_ - 1)] = {observation, in_market};
        ++logical_count_;
    }

    // Presentation may hide a suffix, but all held metric slots still feed
    // the fold. The publication count is separate from the metric count.
    std::int64_t published_count(std::int64_t visible_tail_points) const {
        if (visible_tail_points < 0 || visible_tail_points > size_)
            refuse("native selected report: presentation exceeded its tail window");
        return logical_count_ - (size_ - visible_tail_points);
    }

    template<class Emit>
    void emit_final_tail(Emit&& emit) const {
        emit_metric_tail([&](const pf_equity_point_t& point, bool, bool) { emit(point); });
    }

    template<class Emit>
    void emit_metric_tail(Emit&& emit) const {
        for (std::int64_t i = 0; i < size_; ++i)
            emit(points_[static_cast<std::size_t>(i)],
                 flags_[static_cast<std::size_t>(i)].observation,
                 flags_[static_cast<std::size_t>(i)].in_market);
    }

    std::int64_t logical_count() const noexcept { return logical_count_; }
    std::int64_t size() const noexcept { return size_; }
    pf_equity_point_t* data() noexcept { return points_.data(); }
    const pf_equity_point_t* data() const noexcept { return points_.data(); }
    pf_equity_point_t& operator[](std::size_t i) noexcept { return points_[i]; }
    const pf_equity_point_t& operator[](std::size_t i) const noexcept { return points_[i]; }

private:
    [[noreturn]] static void refuse(const char* text) {
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, text);
    }

    std::array<pf_equity_point_t, kPoints> points_{};
    struct Flags { bool observation = false; bool in_market = false; };
    std::array<Flags, kPoints> flags_{};
    std::int64_t logical_count_ = 0;
    std::int64_t size_ = 0;
};

} // namespace pineforge::report
