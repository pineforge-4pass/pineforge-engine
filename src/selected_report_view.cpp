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
