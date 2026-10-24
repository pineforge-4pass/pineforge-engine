#pragma once

#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pineforge::report {

// Report validation only. Feed a point after its last execution rewrite, or
// scan a host-owned array after the host has finished rewriting its history.
// A copy can judge a held tail without consuming the recording-side prefix.
// No writer, point history or broker decision state belongs to this latch.
class SelectedCaptureLatches {
public:
    SelectedCaptureLatches(std::int64_t start_ms, std::int64_t end_ms,
                           double initial_capital) noexcept
        : start_ms_(start_ms), end_ms_(end_ms), initial_capital_(initial_capital) {}

    void note_trade(std::int64_t entry_time) noexcept {
        trade_before_window_ = trade_before_window_ || entry_time < start_ms_;
    }

    void note_open_lot(std::int64_t entry_time) noexcept {
        lot_before_window_ = lot_before_window_ || entry_time < start_ms_;
    }

    void observe_final(const pf_equity_point_t& point) {
        if (point.time_ms < start_ms_)
            account_changed_ = account_changed_
                || point.equity != initial_capital_ || point.open_profit != 0.0;
        if (point.time_ms >= start_ms_ && point.time_ms < end_ms_) {
            if (observations_ == 0) first_index_ = points_;
            not_contiguous_ = not_contiguous_ || gap_after_window_;
            if (observations_ == std::numeric_limits<std::uint64_t>::max())
                refuse("native selected report: the observation count overflowed");
            ++observations_;
        } else if (observations_ != 0) {
            gap_after_window_ = true;
        }
        ++points_;
    }

    // Defer semantic refusal until capture. Checking in this order preserves
    // the array scan's precedence even when several final values contradict
    // the window or the count.
    void validate(std::uint64_t expected_observations) const {
        if (trade_before_window_)
            refuse("native selected report: a trade was entered before the window start");
        if (lot_before_window_)
            refuse("native selected report: a position was carried in before the window start");
        if (account_changed_)
            refuse("native selected report: the account changed before the window start");
        if (not_contiguous_)
            refuse("native selected report: the window's equity observations are not one run");
        if (observations_ != expected_observations)
            refuse("native selected report: the window's equity observations differ from its script bars");
    }

    std::uint64_t observations() const noexcept { return observations_; }
    std::uint64_t first_index() const noexcept { return first_index_; }

private:
    [[noreturn]] static void refuse(const char* text) {
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, text);
    }

    std::int64_t start_ms_, end_ms_;
    double initial_capital_;
    std::uint64_t points_ = 0;
    std::uint64_t first_index_ = 0;
    std::uint64_t observations_ = 0;
    bool trade_before_window_ = false;
    bool lot_before_window_ = false;
    bool account_changed_ = false;
    bool gap_after_window_ = false;
    bool not_contiguous_ = false;
};

} // namespace pineforge::report
