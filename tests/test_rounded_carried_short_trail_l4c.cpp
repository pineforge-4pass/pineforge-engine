#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
// Literal command fixtures pinned by independent TradingView controls. Synthetic
// timestamps avoid any strategy/date routing; no historical feed is loaded.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }

enum class Action { DEFAULT_REVERSE, SMALL_REVERSE, HALF, HOLD };
enum class ExitShape { ABSOLUTE_TRAIL, RELATIVE_TRAIL, NONE, PARTIAL_TRAIL, PRICED };

const std::vector<Bar> bars = {
    {1.12214, 1.12228, 1.12180, 1.12224, 1405, 1000},
    {1.12224, 1.12282, 1.12224, 1.12280, 1597, 2000},
    {1.12284, 1.12396, 1.12284, 1.12384, 2272, 3000},
    {1.12382, 1.12516, 1.12379, 1.12455, 2317, 4000},
    {1.12456, 1.12464, 1.12399, 1.12414, 1550, 5000},
    {1.12413, 1.12418, 1.12359, 1.12385, 1391, 6000},
    {1.12386, 1.12424, 1.12383, 1.12420, 1294, 7000},
};

class RoundedShort : public pineforge::source::PineStrategyHost {
public:
    Action action;
    ExitShape exit_shape;
    bool funded;
    bool competing = false;
    double first_view = qnan, boundary_view = qnan, boundary_equity = qnan;
    double after_action_view = qnan;
    std::size_t boundary_closed = 0;
    RoundedShort(Action a, ExitShape e = ExitShape::ABSOLUTE_TRAIL, bool extra_cash = false)
        : action(a), exit_shape(e), funded(extra_cash) {
        initial_capital_ = 1000928.7880272 + (funded ? 10000.0 : 0.0);
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = 0.01;
        syminfo_mintick_ = 0.00001;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, qnan, qnan, funded ? 891902.61 : qnan);
            if (competing) strategy_entry("Parked", true, 0.50, qnan, 1.0);
        }
        if (bar_index_ == 1) first_view = signed_position_size();
        if (bar_index_ == 3) {
            boundary_view = signed_position_size();
            boundary_equity = current_equity() + open_profit(bar.close);
            boundary_closed = trades_.size();
            if (action == Action::DEFAULT_REVERSE) strategy_entry("L", true);
            if (action == Action::SMALL_REVERSE) strategy_entry("L", true, qnan, qnan, 1.0);
            if (action == Action::HALF) strategy_close("S", "half", qnan, 50.0);
        }
        if (exit_shape == ExitShape::ABSOLUTE_TRAIL || exit_shape == ExitShape::PARTIAL_TRAIL) {
            strategy_exit("SX", "S", qnan, qnan, qnan, 1.0, 1.10,
                          exit_shape == ExitShape::PARTIAL_TRAIL ? 50.0 : 100.0);
        } else if (exit_shape == ExitShape::RELATIVE_TRAIL) {
            strategy_exit("SX", "S", qnan, qnan, 0.001, 0.001);
        } else if (exit_shape == ExitShape::PRICED) {
            strategy_exit("SX", "S", qnan, 1.30);
        }
        if (bar_index_ == 4) after_action_view = signed_position_size();
        if (bar_index_ == 6) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_margin_is_visible_before_script_actions() {
    for (ExitShape shape : {ExitShape::ABSOLUTE_TRAIL, ExitShape::RELATIVE_TRAIL}) {
        for (Action action : {Action::DEFAULT_REVERSE, Action::SMALL_REVERSE, Action::HALF, Action::HOLD}) {
            RoundedShort engine(action, shape);
            engine.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(near(engine.first_view, -888216.89));
            CHECK(near(engine.boundary_view, -884473.25));
            CHECK(near(engine.boundary_equity, 998872.5856733001));
            CHECK(engine.boundary_closed == 2);
            CHECK(engine.rows().size() == (action == Action::HOLD ? 3u : 4u));
            if (engine.rows().size() < 3) continue;
            CHECK(engine.rows()[0].exit_id == "__margin_call__");
            CHECK(engine.rows()[0].exit_time == 2000);
            CHECK(near(engine.rows()[0].qty, 3685.72));
            CHECK(near(engine.rows()[0].exit_price, 1.12282));
            CHECK(engine.rows()[1].exit_id == "__margin_call__");
            CHECK(engine.rows()[1].exit_time == 4000);
            CHECK(near(engine.rows()[1].qty, 3743.64));
            CHECK(near(engine.rows()[1].exit_price, 1.12516));
            if (action == Action::DEFAULT_REVERSE || action == Action::SMALL_REVERSE) {
                CHECK(engine.rows()[2].exit_time == 4000);
                CHECK(near(engine.rows()[2].qty, 884473.25));
                CHECK(near(engine.rows()[2].exit_price, 1.12455));
                CHECK(near(engine.after_action_view, action == Action::DEFAULT_REVERSE ? 888242.03 : 1.0));
            } else if (action == Action::HALF) {
                CHECK(near(engine.rows()[2].qty, 442236.62));
                CHECK(near(engine.after_action_view, -442236.63));
            } else {
                CHECK(near(engine.after_action_view, -884473.25));
            }
        }
    }
}

void test_funded_control_does_not_create_margin() {
    RoundedShort engine(Action::DEFAULT_REVERSE, ExitShape::ABSOLUTE_TRAIL, true);
    engine.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(engine.boundary_view, -891902.61));
    CHECK(near(engine.boundary_equity, 1008868.4929981));
    CHECK(engine.boundary_closed == 0);
    CHECK(engine.rows().size() == 2);
    CHECK(near(engine.after_action_view, 897130.84));
}

void test_other_order_shapes_retain_the_existing_checkpoint() {
    for (ExitShape shape : {ExitShape::NONE, ExitShape::PARTIAL_TRAIL, ExitShape::PRICED}) {
        RoundedShort engine(Action::HOLD, shape);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.first_view, -891902.61));
        CHECK(near(engine.boundary_view, -888216.89));
        CHECK(engine.boundary_closed == 1);
        CHECK(engine.rows().size() == 3);
    }
    RoundedShort competing(Action::HOLD);
    competing.competing = true;
    competing.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(competing.boundary_view, -888216.89));
    CHECK(competing.boundary_closed == 1);
}

class ExcursionShort : public pineforge::source::PineStrategyHost {
public:
    ExcursionShort() {
        initial_capital_ = 1532722.4186011;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("S", false);
        strategy_exit("SX", "S", qnan, qnan, qnan, 1.0, 60000.0);
        if (bar_index_ == 2) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_margin_excursion_samples_only_the_traversed_prefix() {
    // The first two price bars and opening budget reconstruct the independent
    // daily control's 8.34992 margin slice. Its low precedes the adverse high.
    // Synthetic timestamps and an early terminal close isolate that slice.
    const std::vector<Bar> low_first = {
        {65971.20, 69516.65, 65821.97, 68432.16, 1, 1000},
        {68432.16, 71777.00, 68391.41, 69948.63, 1, 2000},
        {69948.64, 71321.00, 68977.91, 70191.86, 1, 3000},
    };
    ExcursionShort engine;
    engine.run(low_first.data(), static_cast<int>(low_first.size()));
    CHECK(engine.rows().size() == 2);
    if (engine.rows().size() == 2) {
        const auto& margin = engine.rows()[0];
        CHECK(margin.exit_id == "__margin_call__");
        CHECK(margin.exit_time == 2000);
        CHECK(near(margin.qty, 8.34992));
        CHECK(near(margin.exit_price, 71777.0));
        CHECK(near(margin.max_runup, 8.34992 * 40.75));
        CHECK(near(engine.rows()[1].max_runup, 14.04777 * 40.75));
    }
    // Reverse the waypoint order: the favorable low comes AFTER liquidation.
    // It belongs to the survivor, never to the already-closed margin slice.
    const std::vector<Bar> high_first = {
        low_first[0],
        {68432.16, 69000.0, 67000.0, 68500.0, 1, 2000},
        {68500.0, 68600.0, 68400.0, 68500.0, 1, 3000},
    };
    ExcursionShort later_low;
    later_low.run(high_first.data(), static_cast<int>(high_first.size()));
    CHECK(later_low.rows().size() == 2);
    if (later_low.rows().size() == 2) {
        CHECK(later_low.rows()[0].exit_id == "__margin_call__");
        CHECK(near(later_low.rows()[0].exit_price, 69000.0));
        CHECK(near(later_low.rows()[0].max_runup, 0.0));
        CHECK(later_low.rows()[1].max_runup > 0.0);
    }
}
} // namespace

int main() {
    test_margin_is_visible_before_script_actions();
    test_funded_control_does_not_create_margin();
    test_other_order_shapes_retain_the_existing_checkpoint();
    test_margin_excursion_samples_only_the_traversed_prefix();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
