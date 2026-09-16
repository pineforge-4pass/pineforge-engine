#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/compat/pine/exit_activation.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
using namespace pineforge::compat::pine;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
constexpr double na = std::numeric_limits<double>::quiet_NaN();

void policy_routes() {
    ExitActivationContext context;
    context.cycle = 7;
    context.bar_index = 3;
    context.position_open_bar = 3;
    context.direction = 1;
    context.cursor_price = 100;
    context.fill_recalc = true;
    context.scheduler = true;
    context.after_first_open_fill = true;
    context.current_fill = 11;
    ExitActivationRequest request{false, true, false, true};
    // The legacy LaterSameOpen route is born by a later fill callback; the
    // restored selector consumes that causal reach instead of inferring it
    // from the raw from_fill bit alone.
    request.birth_reach = HistoricalBirthReach::ExtremeWaypoints;
    CHECK(context.cycle == 7);
    CHECK(context.position_open_bar == context.bar_index);
    CHECK(request.full_quantity);
    CHECK(request.has_from_entry);
    CHECK(!request.requested_trailing);
    const auto held_stop = select_exit_activation(request, 105, na, context);
    CHECK(held_stop.evidence().has_value());
    CHECK(held_stop.holds_stop());
    CHECK(!held_stop.holds_limit());
    const auto bounds = held_stop.resolve(7, 3);
    CHECK(bounds.position_cycle == 7);
    CHECK(bounds.stop_first_bar == 4);
    CHECK(bounds.limit_first_bar == 3);

    const auto held_limit = select_exit_activation(request, na, 95, context);
    CHECK(held_limit.evidence().has_value());
    CHECK(!held_limit.holds_stop());
    CHECK(!held_limit.holds_limit());
    CHECK(held_limit.resolve(7, 3).limit_first_bar == 3);

    context.cursor_price = 110;
    context.recalc_leg = 0;
    const auto continuation = select_exit_activation(request, na, 105, context);
    CHECK(continuation.evidence().has_value());
    CHECK(continuation.continues_at_later_open());
    CHECK(!continuation.holds_limit());
    CHECK(continuation.evidence()->limit_continuation.has_value());

    context.fill_recalc = false;
    const auto inactive = select_exit_activation(request, 105, 95, context);
    CHECK(!inactive.evidence().has_value());
}

class Route final : public pineforge::source::PineStrategyHost {
public:
    Route(bool long_side, bool stop_leg) : long_side_(long_side), stop_leg_(stop_leg) {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        margin_long_ = margin_short_ = 0.0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", long_side_, na, na, 1.0);
        if (bar_index_ == 1) {
            const double level = long_side_ == stop_leg_ ? 95.0 : 105.0;
            strategy_exit("X", "E", stop_leg_ ? na : level, stop_leg_ ? level : na);
            pending = l4c_pending_orders();
        }
    }
    bool long_side_;
    bool stop_leg_;
    std::vector<pineforge::source::L4cPendingOrder> pending;
};

void public_routes() {
    for (const bool long_side : {false, true}) {
        for (const bool stop_leg : {false, true}) {
            Route route(long_side, stop_leg);
            const Bar bars[] = {
                {100,100,100,100,1,0}, {100,100,100,100,1,60000},
                {100,106,94,100,1,120000},
            };
            route.run(bars, 3);
            CHECK(route.last_error().empty());
            CHECK(route.pending.size() == 1);
            if (route.pending.size() == 1) {
                CHECK(route.pending.front().id == "X");
                CHECK(route.pending.front().from_entry == "E");
                CHECK(route.pending.front().type == pineforge::source::L4cOrderType::EXIT);
            }
            CHECK(route.trade_count() == 1);
            if (route.trade_count() == 1) {
                const auto& trade = route.get_trade(0);
                CHECK(trade.exit_id == "X");
                CHECK(trade.is_long == long_side);
                CHECK(std::isfinite(trade.exit_price));
            }
        }
    }
}
} // namespace

int main() {
    policy_routes();
    public_routes();
    std::printf("native activation routes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
