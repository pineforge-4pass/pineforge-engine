// Documentation witness for D-A, D-K and D-L.
//
// expectation corrected: the old migration rows treated trail_points as the
// ride offset, treated C loss fields as signed, and counted report range-end
// rows as Pine closed trades. These assertions pin the corrected translations
// against the public types without changing engine behaviour.
#include <pineforge/native_order.hpp>
#include <pineforge/pineforge.h>

#include <cassert>
#include <cmath>

int main() {
    constexpr double entry = 101.0;
    constexpr double tick = 0.25;
    constexpr double arm_ticks = 30.0;
    constexpr double offset_ticks = 8.0;
    const double arm = entry + arm_ticks * tick;
    const double ride = offset_ticks * tick;
    const double stop = arm - ride;
    assert(std::fabs(arm - 108.5) < 1e-12);
    assert(std::fabs(ride - 2.0) < 1e-12);
    assert(std::fabs(stop - 106.5) < 1e-12);

    pf_trade_stats_t c_stats{};
    c_stats.gross_loss = 10.0;  // C report: positive magnitude.
    const double protected_signed_loss = -c_stats.gross_loss;
    assert(protected_signed_loss == -10.0);

    pf_trade_t rows[2]{};
    rows[0].open_at_end = 0;
    rows[1].open_at_end = 1;  // report-only range-end row.
    int pine_closed = 0;
    for (const auto& row : rows) pine_closed += row.open_at_end == 0;
    assert(pine_closed == 1);
    return 0;
}
