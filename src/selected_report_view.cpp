/*
 * selected_report_view.cpp — the selected-window report's equity curve
 * (selected_report_view.hpp): the window anchor followed by the script-bar
 * observations, assembled without a callback or any engine state.
 */
#include "selected_report_view.hpp"

#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pineforge {
namespace report {

namespace {

// A call the engine itself got wrong, coded the way the recorded outputs code
// a broken recorder contract (engine_invariant); never a strategy refusal.
[[noreturn]] void refuse_view(const char* text) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, text);
}

}  // namespace

SelectedEquityCurve build_selected_equity_curve(
    int64_t window_start_ms, double initial_capital,
    const pf_equity_point_t* observations, int64_t observation_count) {
    static_assert(std::is_trivially_copyable<pf_equity_point_t>::value,
                  "observations are copied byte for byte");
    if (observation_count < 0)
        refuse_view("selected report view: the observation count is negative");
    if (observation_count > 0 && observations == nullptr)
        refuse_view("selected report view: observations is null for a nonzero count");
    // M + 1 points must be countable by the int64_t `count` and their bytes must
    // fit a size_t, whatever size_t is. Compared as M >= most_points, never
    // M + 1 > most_points: M + 1 itself could overflow int64_t.
    const uint64_t countable = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const uint64_t sizable = static_cast<uint64_t>(
        std::numeric_limits<std::size_t>::max() / sizeof(pf_equity_point_t));
    const uint64_t most_points = countable < sizable ? countable : sizable;
    if (static_cast<uint64_t>(observation_count) >= most_points)
        refuse_view("selected report view: the observation count exceeds the largest curve");

    // One array, from `new[]` because whoever takes it frees it with `delete[]`
    // (BacktestEngine::free_report on ReportC::equity_curve). The empty `()`
    // value-initializes every element; the anchor and the copy below write all of them.
    const std::size_t observed = static_cast<std::size_t>(observation_count);
    std::unique_ptr<pf_equity_point_t[]> points(new pf_equity_point_t[observed + 1]());
    points[0].time_ms = window_start_ms;
    points[0].equity = initial_capital;
    points[0].open_profit = 0.0;
    if (observed > 0)
        std::memcpy(points.get() + 1, observations, observed * sizeof(pf_equity_point_t));
    return SelectedEquityCurve{std::move(points), observation_count + 1};
}

}  // namespace report
}  // namespace pineforge

namespace pineforge {
namespace report {

SelectedReportStream::SelectedReportStream(std::int64_t start_ms, std::int64_t end_ms,
                                          double initial_capital) noexcept
    : start_ms_(start_ms), end_ms_(end_ms), initial_capital_(initial_capital) {}

void SelectedReportStream::note_trade(std::int64_t entry_time) noexcept {
    trade_before_window_ = trade_before_window_ || entry_time < start_ms_;
}

void SelectedReportStream::note_open_lot(std::int64_t entry_time) noexcept {
    lot_before_window_ = lot_before_window_ || entry_time < start_ms_;
}

void SelectedReportStream::write_point(report_sink::LineWriter&,
                                      const pf_equity_point_t& point, bool) {
    if (point.time_ms < start_ms_) {
        account_changed_ = account_changed_
            || point.equity != initial_capital_ || point.open_profit != 0.0;
    }
    const bool in_window = point.time_ms >= start_ms_ && point.time_ms < end_ms_;
    if (!in_window) {
        if (observations_ != 0) gap_after_window_ = true;
        return;
    }
    not_contiguous_ = not_contiguous_ || gap_after_window_;
    if (observations_ == std::numeric_limits<std::uint64_t>::max())
        refuse_view("native selected report: the observation count overflowed");
    ++observations_;
}

void SelectedReportStream::finish(report_sink::LineWriter&,
                                 std::uint64_t expected_observations,
                                 bool) {
    if (trade_before_window_)
        refuse_view("native selected report: a trade was entered before the window start");
    if (lot_before_window_)
        refuse_view("native selected report: a position was carried in before the window start");
    if (account_changed_)
        refuse_view("native selected report: the account changed before the window start");
    if (not_contiguous_)
        refuse_view("native selected report: the window's equity observations are not one run");
    if (observations_ != expected_observations)
        refuse_view("native selected report: the window's equity observations differ from its script bars");
}

}  // namespace report
}  // namespace pineforge
