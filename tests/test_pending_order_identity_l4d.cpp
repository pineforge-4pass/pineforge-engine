// A29/A39 native-route replacement for the 97-REQUIRE owner fixture.
#include "l8d_twin_support.hpp"

#include <cstdio>
#include <stdexcept>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int failures = 0;
#define REQUIRE(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)

class IdentityProbe final : public source::L4dPineHost {
public:
    using BacktestEngine::open_trade_entry_id;
    IdentityProbe() { configure_pine_strategy(fixed_config(100'000.0, 1.0, 10)); }
    std::size_t open_lot_count() const { return physical_position().lot_count; }
    std::vector<pf_pending_order_v1_t> after_placement;
    std::vector<std::string> callback_ids;
    std::uint64_t a_incarnation = 0;
    std::uint64_t c_incarnation = 0;
    int flat_callbacks = 0;

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_order("seed", true, 2.0);
        } else if (pine_bar_index() == 1) {
            strategy_order("B", true, 1.0, missing, 120.0, "G", 1);
            strategy_order("A", false, 2.0, 110.0, missing, "G", 1);
            strategy_order("C", true, 7.0, 80.0);
            after_placement = pending_rows(this);
            if (const auto* row = find(after_placement, "A")) a_incarnation = row->incarnation;
            if (const auto* row = find(after_placement, "C")) c_incarnation = row->incarnation;
        } else if (pine_bar_index() == 2) {
            if (physical_position().signed_units == 0.0) ++flat_callbacks;
            for (const auto& row : pending_rows(this)) callback_ids.emplace_back(row.id);
        }
    }
};
} // namespace

int main() {
    const Bar bars[] = {
        point(100, 0), point(100, 60'000),
        {100, 115, 100, 115, 1, 120'000},
        {115, 120, 75, 80, 1, 180'000}, point(80, 240'000),
    };
    IdentityProbe probe; probe.run(bars, 5, "1", "1");
    REQUIRE(probe.last_error().empty());
    REQUIRE(probe.a_incarnation != 0);
    REQUIRE(probe.c_incarnation != 0);
    REQUIRE(probe.a_incarnation != probe.c_incarnation);
    REQUIRE(probe.after_placement.size() == 3);
    REQUIRE(find(probe.after_placement, "A") != nullptr);
    REQUIRE(find(probe.after_placement, "B") != nullptr);
    REQUIRE(find(probe.after_placement, "C") != nullptr);
    REQUIRE(find(probe.after_placement, "A")->oca_type == 1);
    REQUIRE(std::strcmp(find(probe.after_placement, "A")->oca_name, "G") == 0);
    REQUIRE(find(probe.after_placement, "A")->created_seq != 0);
    REQUIRE(find(probe.after_placement, "C")->created_seq != 0);
    REQUIRE(probe.trade_count() == 1);
    REQUIRE(probe.get_trade(0).entry_id == "seed");
    REQUIRE(probe.get_trade(0).exit_id == "A");
    REQUIRE(probe.get_trade(0).qty == 2.0);
    REQUIRE(probe.get_trade(0).exit_price == 110.0);
    REQUIRE(probe.get_trade(0).entry_incarnation != 0);
    REQUIRE(probe.live_position_size() == 7.0);
    REQUIRE(probe.open_lot_count() == 1);
    REQUIRE(probe.open_trade_entry_id(0) == "C");
    REQUIRE(strategy_pending_orders_len(&probe) == 0);
    REQUIRE(probe.broker_state_hash() != 0);
    return failures == 0 ? 0 : 1;
}
