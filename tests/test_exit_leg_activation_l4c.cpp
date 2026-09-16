#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/pending_order_mirror.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace pineforge;
using namespace pineforge::compat::pine;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
constexpr double na = std::numeric_limits<double>::quiet_NaN();

void activation_bounds_contract() {
    ExitLegActivation activation;
    CHECK(!activation.bounds().has_value());
    CHECK(activation.stop_ready(1, 0));
    CHECK(activation.limit_ready(1, 0));
    activation.bind({7, 3, 5});
    CHECK(activation.bounds().has_value());
    CHECK(activation.bounds()->position_cycle == 7
          && activation.bounds()->stop_first_bar == 3
          && activation.bounds()->limit_first_bar == 5);
    CHECK(!activation.stop_ready(7, 2) && activation.stop_ready(7, 3)
          && activation.stop_ready(7, 6));
    CHECK(!activation.stop_ready(8, 6));
    CHECK(!activation.limit_ready(7, 4) && activation.limit_ready(7, 5)
          && activation.limit_ready(7, 8));
    CHECK(!activation.limit_ready(8, 8));
    activation.unbind();
    CHECK(!activation.bounds().has_value());
    CHECK(activation.stop_ready(99, 0));
    CHECK(activation.limit_ready(99, 0));
    CHECK(activation.stop_ready(1, 0) && activation.limit_ready(1, 0));
    bool rejected = false;
    try { activation.bind({0, 1, 1}); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { activation.bind({1, -1, 1}); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { activation.bind({1, 1, -1}); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

void policy_contract() {
    ExitActivationContext context{7, 2, 2, 1, 100, true, true, false, false,
                                  false, true, true, 0, false, false, 0, 9};
    ExitActivationRequest request{false, true, false, true};
    request.birth_reach = HistoricalBirthReach::ExtremeWaypoints;
    const auto stop = select_exit_activation(request, 105, na, context);
    CHECK(stop.evidence().has_value());
    CHECK(stop.holds_stop());
    CHECK(!stop.holds_limit());
    CHECK(stop.resolve(7, 2).stop_first_bar == 3);
    const auto limit = select_exit_activation(request, na, 95, context);
    CHECK(limit.evidence().has_value());
    CHECK(!limit.holds_stop());
    CHECK(!limit.holds_limit());
    CHECK(limit.resolve(7, 2).limit_first_bar == 2);
    context.cursor_price = 110;
    const auto continuation = select_exit_activation(request, na, 105, context);
    CHECK(continuation.evidence().has_value());
    CHECK(continuation.continues_at_later_open());
    CHECK(continuation.evidence()->limit_continuation.has_value());
    CHECK(continuation.evidence()->limit_continuation->observed_fill_sequence == 9);
}

class Route final : public pineforge::source::PineStrategyHost {
public:
    explicit Route(bool long_side) : long_side_(long_side) {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        margin_long_ = margin_short_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", long_side_, na, na, 1);
        if (bar_index_ == 1) {
            strategy_exit("X", "E", na, long_side_ ? 95 : 105);
            pending = l4c_pending_orders();
        }
    }
    bool long_side_;
    std::vector<pineforge::source::L4cPendingOrder> pending;
};

void public_projection_contract() {
    for (const bool long_side : {false, true}) {
        Route route(long_side);
        const Bar bars[] = {{100,100,100,100,1,0}, {100,100,100,100,1,60000},
                            {100,106,94,100,1,120000}};
        route.run(bars, 3);
        CHECK(route.last_error().empty());
        CHECK(route.pending.size() == 1);
        if (route.pending.size() == 1) {
            const auto& pending = route.pending.front();
            CHECK(pending.id == "X");
            CHECK(pending.from_entry == "E");
            CHECK(!pending.leg_activation.bounds().has_value());
            CHECK(pending.stop_price == (long_side ? 95 : 105));
            CHECK(pending.type == pineforge::source::L4cOrderType::EXIT);
        }
        CHECK(route.trade_count() == 1);
        if (route.trade_count() == 1) CHECK(route.get_trade(0).exit_id == "X");
    }
}
} // namespace

int main() {
    activation_bounds_contract();
    policy_contract();
    public_projection_contract();
    std::printf("exit leg activation: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
