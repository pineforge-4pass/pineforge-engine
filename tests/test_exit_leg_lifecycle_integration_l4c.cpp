#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/compat/pine/exit_lifecycle.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

using namespace pineforge;
using namespace pineforge::exit_legs;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
const double nan = std::numeric_limits<double>::quiet_NaN();

Action action(const Lifecycle& state, std::uint64_t event, int bar, Operation op) {
    return {state.target(), state.revision(), {event, bar, Domain::Ordinary,
            Phase::Observation}, std::move(op)};
}

void lifecycle_replacement_contract() {
    Lifecycle state;
    state.attach(50, 7);
    state.set_stop_price(95);
    CHECK(state.target().incarnation == 50 && state.prices().stop_price == 95);
    CHECK(state.apply(state.target(), action(state, 1, 0,
        Suspend{{Leg::Stop, Leg::Limit}, {}, {}, {}})) == Result::Applied);
    CHECK(state.dormant());
    CHECK(state.apply(state.target(), action(state, 2, 1,
        Restore{{Leg::Stop, Leg::Limit}})) == Result::Applied);
    CHECK(!state.dormant());
    const auto staged = action(state, 3, 2,
        StageReplacement{{49, state.definition(50), {{2, 1, Domain::Ordinary,
            Phase::Observation}, {}, 0}}});
    CHECK(state.apply(state.target(), staged) == Result::Applied);
    CHECK(state.pending_replacement());
    CHECK(state.release_barrier().has_value());
    const auto completed = action(state, 4, 2,
        CompleteBarrier{{3, 2, Domain::Ordinary, Phase::Observation}, state.release_barrier()});
    CHECK(state.apply(state.target(), completed) == Result::Applied);
    CHECK(!state.pending_replacement() && state.last_action().has_value());
}

class Route final : public pineforge::source::PineStrategyHost {
public:
    Route() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        margin_long_ = margin_short_ = 0.0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, nan, nan, 1.0);
        if (bar_index_ == 1) {
            strategy_exit("X", "E", nan, 96.0);
            strategy_exit("X", "E", nan, 95.0);
            pending = l4c_pending_orders();
        }
    }
    std::vector<pineforge::source::L4cPendingOrder> pending;
};

void public_reissue_contract() {
    Route route;
    const Bar bars[] = {
        {100,100,100,100,1,0}, {100,100,100,100,1,60000},
        {100,101,94,96,1,120000},
    };
    route.run(bars, 3);
    CHECK(route.last_error().empty());
    CHECK(route.pending.size() == 1);
    if (route.pending.size() == 1) {
        CHECK(route.pending.front().id == "X" && route.pending.front().from_entry == "E");
        CHECK(!route.pending.front().legs.pending_replacement());
        CHECK(std::abs(route.pending.front().stop_price - 95.0) < 1e-9);
    }
    CHECK(route.trade_count() == 1);
    if (route.trade_count() == 1) {
        CHECK(route.get_trade(0).exit_id == "X");
        CHECK(std::abs(route.get_trade(0).exit_price - 95.0) < 1e-9);
    }
}
} // namespace

int main() {
    lifecycle_replacement_contract();
    public_reissue_contract();
    std::printf("exit lifecycle integration: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
