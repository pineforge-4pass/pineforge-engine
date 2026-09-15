// A29 native-route twin for test_root_cancel_sole_stop.cpp.
//
// The old test constructed a PendingOrder and drove ExitLegLifecycle directly.
// This twin issues the same entry/stop/cancel shape through source commands;
// matching and exit-leg retirement remain wholly native-owned.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <limits>

using namespace pineforge;

namespace {

class Book final : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
    }

    void on_source_bar(const Bar&) override {
        const double missing = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("E", true, missing, missing, 1.0);
        if (bar_index_ == 1) {
            strategy_exit("X", "E", missing, 95.0);
            // The public cancellation command is the legal way to retire a
            // root-owned stop after the legacy exit-leg object was deleted.
            strategy_cancel("X");
        }
    }

    bool run_case() {
        const Bar bars[] = {
            {100, 100, 100, 100, 1, 0},
            {100, 101, 99, 100, 1, 60'000},
            {100, 101, 94, 100, 1, 120'000},
        };
        run(bars, 3);
        return last_error().empty() && pending_order_count() == 0
            && std::abs(live_position_size() - 1.0) < 1e-12;
    }
};

}  // namespace

int main() {
    Book book;
    return book.run_case() ? 0 : 1;
}

#undef PineStrategyHost
