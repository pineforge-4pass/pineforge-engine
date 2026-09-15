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
int failed = 0;

#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

constexpr double na = std::numeric_limits<double>::quiet_NaN();

Action action(const Lifecycle& value, std::uint64_t event, int bar, Operation operation) {
    return {value.target(), value.revision(), {event, bar, Domain::Ordinary,
            Phase::Observation}, std::move(operation)};
}

void lifecycle_availability_contract() {
    Lifecycle value;
    value.attach(41, 7);
    value.set_prices({110, 95, na, na, na, na, na});
    CHECK(value.target().incarnation == 41 && value.target().owner == 7);
    CHECK(value.available(Leg::Stop, 0) && value.available(Leg::Limit, 0));
    const auto suspended = action(value, 1, 0, Suspend{{Leg::Stop}, {}, {}, {}});
    CHECK(value.apply(value.target(), suspended) == Result::Applied
          && !value.available(Leg::Stop, 1));
    CHECK(value.available(Leg::Limit, 1));
    const auto restored = action(value, 2, 1, Restore{{Leg::Stop}});
    CHECK(value.apply(value.target(), restored) == Result::Applied
          && value.available(Leg::Stop, 2));
    CHECK(value.generation(Leg::Stop) >= 1);
    const auto cancelled = action(value, 3, 2, Cancel{{Leg::Limit}});
    CHECK(value.apply(value.target(), cancelled) == Result::Applied
          && !value.available(Leg::Limit, 3));
    CHECK(value.retired(Leg::Limit));
    const auto replay = value.apply(value.target(), cancelled);
    CHECK(replay == Result::Replay);
    CHECK(value.prices().stop_price == 95);
    CHECK(value.prices().limit_price == 110);

    Lifecycle replacement;
    replacement.attach(42, 7);
    replacement.set_stop_price(90);
    const auto staged = action(replacement, 4, 3,
        StageReplacement{{41, value.definition(41), {{3, 2, Domain::Ordinary,
            Phase::Observation}, {}, 0}}});
    CHECK(replacement.apply(replacement.target(), staged) == Result::Applied);
    CHECK(replacement.pending_replacement());
    const auto completion = action(replacement, 5, 3,
        CompleteBarrier{{4, 3, Domain::Ordinary, Phase::Observation},
                         replacement.release_barrier()});
    CHECK(replacement.apply(replacement.target(), completion) == Result::Applied);
    CHECK(!replacement.pending_replacement());
    CHECK(!replacement.dormant());
}

class PublicRoute final : public pineforge::source::PineStrategyHost {
public:
    PublicRoute() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        margin_long_ = margin_short_ = 0.0;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, na, na, 1.0);
        if (bar_index_ == 1) {
            strategy_exit("X", "E", na, 95.0);
            snapshot = l4c_pending_orders();
        }
    }

    std::vector<pineforge::source::L4cPendingOrder> snapshot;
};

void public_route_contract() {
    PublicRoute host;
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 0},
        {100, 100, 100, 100, 1, 60000},
        {100, 101, 94, 96, 1, 120000},
    };
    host.run(bars, 3);
    CHECK(host.last_error().empty());
    CHECK(host.snapshot.size() == 1);
    if (host.snapshot.size() == 1) {
        const auto& pending = host.snapshot.front();
        CHECK(pending.id == "X");
        CHECK(pending.from_entry == "E");
        CHECK(pending.type == pineforge::source::L4cOrderType::EXIT);
        CHECK(std::abs(pending.stop_price - 95.0) < 1e-9);
    }
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& trade = host.get_trade(0);
        CHECK(trade.exit_id == "X");
        CHECK(std::abs(trade.exit_price - 95.0) < 1e-9);
    }
}

} // namespace

int main() {
    lifecycle_availability_contract();
    public_route_contract();
    std::printf("exit lifecycle availability: %d checks, %d failures\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
