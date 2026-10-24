// Counter-tape to an unconditional "latest two rows" retirement rule.
// Equal stops do not supersede. With requested quantities 10, 20, 30, a
// margin slice leaving between 10 and 20 units must select the MIDDLE row.
#include "k1_dormant_retirement_fixture.hpp"

#include <exception>

namespace {
using namespace dormant_retirement;

class FrontierHost final : public ps::PineStrategyHost {
public:
    FrontierHost() {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(false);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        require(i == callbacks_, "frontier consecutive callbacks");
        ++callbacks_;
        if (i >= 1 && i < 7)
            require(live_position_size() == -40.0, "frontier origin stays open");
        if (i >= 3 && i <= 5) {
            int prior = 0;
            for (const auto row : adapter_.*access(PlacementTag{})) {
                const auto& value = row.second;
                if (value.family != ps::PineOrderFamily::ExitStop || value.source_id != "X"
                    || value.placement_script_open_ms != kStart + (i - 1) * kMinute)
                    continue;
                require(value.legs.dormant() && !value.legs.retired(exit_legs::Leg::Stop)
                        && value.legs.target().incarnation != 0
                        && value.projection_predecessor == 0 && value.exit_levels.stop == 120.0
                        && value.requested_qty == 10.0 * (i - 2),
                        "each real quantity issue reached dormancy");
                for (const auto& request : native_working_requests())
                    require(request.definition->handle.incarnation != row.first,
                            "each frontier issue left the native working book");
                ++prior;
            }
            require(prior == 1, "exactly one previous quantity issue settled");
            ++settled_;
        }
        if (i == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        if (i >= 2 && i <= 4) {
            const double qty = 10.0 * (i - 1);
            strategy_exit("X", "S", kNa, 120.0, kNa, kNa, kNa, 100.0,
                          i == 2 ? "first" : i == 3 ? "middle" : "last", qty);
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
        if (i == 6) {
            std::size_t rows = 0;
            for (const auto row : adapter_.*access(PlacementTag{})) {
                const auto& value = row.second;
                if (value.family != ps::PineOrderFamily::ExitStop || value.source_id != "X"
                    || value.from_entry != "S" || value.bracket_origin.incarnation == 0)
                    continue;
                require(value.legs.target().incarnation != 0 && value.legs.dormant()
                        && !value.legs.retired(exit_legs::Leg::Stop)
                        && value.projection_predecessor == 0 && value.exit_levels.stop == 120.0,
                        "frontier rows are equal-stop, unwithdrawn dormant legs without links");
                for (const auto& request : native_working_requests())
                    require(request.definition->handle.incarnation != row.first,
                            "frontier native stop was not retired");
                ++rows;
            }
            std::printf("DORMANT FRONTIER before_margin retain=%d rows=%zu settled=%d\n",
                        ps::detail::retain_retired_rows() ? 1 : 0, rows, settled_);
            if (ps::detail::retain_retired_rows())
                require(rows == 3, "RETAIN has all three non-superseded quantity candidates");
            saw_frontier_ = true;
        }
    }

    Outcome finish() {
        require(last_error().empty(), "frontier run completed without error");
        require(callbacks_ == 8 && settled_ == 3 && saw_frontier_,
                "frontier tape reached all three real issues and its pre-margin state");
        Outcome out;
        out.retain = ps::detail::retain_retired_rows();
        out.position = live_position_size();
        out.equity = current_equity();
        for (int i = 0; i < trade_count(); ++i) out.trades.push_back(get_trade(i));
        return out;
    }

private:
    int callbacks_ = 0;
    int settled_ = 0;
    bool saw_frontier_ = false;
};

Outcome frontier(bool retain) {
    RetentionSwitch switch_for_run(retain);
    auto bars = tape(8, false);
    bars[5] = {117.0, 126.0, 114.0, 115.0, 1.0, kStart + 5 * kMinute};
    bars[6] = {115.0, 116.0, 114.0, 115.0, 1.0, kStart + 6 * kMinute};
    // The short's equity is 14000 - 40*p; required margin is 40*p.
    // Fourfold liquidation leaves Q = 56000/p - 280 units. At p=189.83,
    // 10 < Q < 20. Thus 10 cannot cover Q, and 20 is the FIRST full row.
    bars[7] = {115.0, 189.83, 114.0, 189.83, 1.0, kStart + 7 * kMinute};
    FrontierHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1",
             false, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

void controls(const Outcome& out) {
    int middle = 0, margin = 0;
    for (const auto& trade : out.trades) {
        if (trade.open_at_end) continue;
        require(trade.entry_id == "S" && trade.exit_bar_index == 7,
                "frontier has no premature close");
        std::printf("DORMANT FRONTIER retain=%d exit=%s comment=%s qty=%.17g price=%.17g\n",
                    out.retain ? 1 : 0, trade.exit_id.c_str(), trade.exit_comment.c_str(),
                    trade.qty, trade.exit_price);
        if (trade.exit_id == "X") {
            require(trade.exit_comment == "middle" && trade.qty > 10.0 && trade.qty < 20.0,
                    "margin must select the middle quantity candidate");
            ++middle;
        } else {
            require(trade.exit_id == "__margin_call__" && trade.qty > 0.0,
                    "frontier unexpected close");
            ++margin;
        }
    }
    require(middle == 1 && margin > 0, "frontier did not reach margin revival");
    require(out.position == 0.0, "frontier revival flattens the margin remainder");
}
} // namespace

int main() {
    try {
        const auto retained = frontier(true);
        controls(retained);
        const auto erased = frontier(false);
        controls(erased);
        require(erased.trades.size() == retained.trades.size(), "frontier trade count differs");
        for (std::size_t i = 0; i < erased.trades.size(); ++i)
            require(same_trade(erased.trades[i], retained.trades[i]), "frontier trade field differs");
        require(same_number(erased.equity, retained.equity), "frontier equity differs");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_k1_dormant_same_stop_frontier: %s\n", error.what());
        return 1;
    }
}
