// Expected wall time: 5-40 seconds Release on a 10-core host (unmeasured).
// Frozen-head red: 499/1,999 resident partial-exit tombstones at B=2,000/8,000.
// One keeper prevents flat/reset from hiding the historical growth.
#include "cr307_g8b_fixture.hpp"

namespace {
using namespace cr307_g8b;
class ReuseHost final : public pineforge::source::PineStrategyHost {
public:
    ReuseHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e9;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 2;
        config.close_entries_rule_any = true;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        qty_step_ = 1.0;
    }
    std::uint64_t opening(const std::string& id) const {
        for (const auto& row : native_open_lots(100.0))
            if (row.entry_label == id) return row.entry_incarnation;
        throw std::runtime_error("expected real opening is absent");
    }
    void partial(const std::string& id) {
        strategy_exit(id, id, 100.5, na, na, na, na, 50.0, {}, 1.0);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const auto& stones = adapter_.*access(Tombstones{});
        const auto& units = adapter_.*access(CloseUnits{});
        switch (pine_bar_index()) {
        case 0: strategy_entry("keeper", true, na, na, 1.0); break;
        case 1: strategy_entry("repeat", true, na, na, 2.0); break;
        case 2:
            first_origin = opening("repeat");
            partial("repeat");
            break;
        case 3:
            require(value_for(stones, "repeat\x1frepeat") == first_origin,
                    "first partial stores the actual first incarnation");
            strategy_close("repeat");
            break;
        case 4:
            require(live_position_size() == 1.0, "first repeat retired while keeper survives");
            strategy_entry("repeat", true, na, na, 2.0);
            break;
        case 5:
            second_origin = opening("repeat");
            require(first_origin != second_origin, "reuse produced a fresh incarnation");
            require(adapter_.fixture_close_logical_units("repeat") == 4.0 && units.size() == 2,
                    "same-id credits accumulate without adding a key");
            // A full reissue bypasses the separate consumed-cycle gate.
            // ANY restricts its actual close to the new repeat lot; the
            // keeper survives, so the fill updates the existing tombstone.
            strategy_exit("repeat", "repeat", 100.5, na, na, na, na, 100.0, {}, 3.0);
            break;
        case 6:
            require(live_position_size() == 1.0 && stones.size() == 1
                        && value_for(stones, "repeat\x1frepeat") == second_origin,
                    "new fill overwrites the existing key with its fresh incarnation");
            strategy_close_all();
            break;
        case 7:
            require(live_position_size() == 0.0 && units.size() == 0 && stones.size() == 0,
                    "flat clears logical and spilled scalar history");
            strategy_entry("long", true, na, na, 2.0);
            break;
        case 8: partial("long"); break;
        case 9:
            require(live_position_size() == 1.0 && units.size() == 1 && stones.size() == 1,
                    "new cycle repopulates both histories after flat");
            strategy_entry("short", false, na, na, 2.0);
            break;
        case 10:
            require(live_position_size() == -2.0 && stones.size() == 0 && units.size() == 1
                        && adapter_.fixture_close_logical_units("short") == 2.0,
                    "reversal clears prior-side history before crediting the new side");
            checked = true;
            break;
        }
    }
    std::uint64_t first_origin = 0, second_origin = 0;
    bool checked = false;
};
}

int main() {
    if (const int status = cr307_g8b::history_main(true); status != 0) return status;
    try {
        const auto bars = chart(12);
        ReuseHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
        require(host.checked, "reuse, overwrite, flat and reversal checks all executed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
