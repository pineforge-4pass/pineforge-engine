#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

#define pending_orders_ source_pending_view()

// A carried short's completed adverse-path liquidation is visible to the
// process_orders_on_close script, before a close or reversal sizes its order.
// Compact command fixtures use synthetic timestamps, with quantities/prices
// independently pinned by the R26 bare, reversal, half, funded and trail TV
// controls. The original historical probe remains unchanged.
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
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

enum class Action { HOLD, REVERSE, HALF };
class CarriedShort : public pineforge::source::PineStrategyHost {
public:
    Action action;
    bool trail = false;
    bool parked_entry = false;
    double first_view = qnan, second_view = qnan, final_view = qnan;
    std::size_t second_closed = 0;
    explicit CarriedShort(Action value, double capital = 1392521.546177, double price_scale = 1.0)
        : action(value) {
        initial_capital_ = capital * price_scale;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01 * price_scale;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("S", false, qnan, qnan, 12.60172);
        if (bar_index_ == 1) first_view = signed_position_size();
        if (bar_index_ == 2) {
            second_view = signed_position_size();
            second_closed = trades_.size();
            if (action == Action::REVERSE) strategy_entry("L", true, qnan, qnan, 2.0);
            if (action == Action::HALF) strategy_close("S", "half", qnan, 50.0);
        }
        if (trail) strategy_exit("Trail", "S", qnan, qnan, 1000.0, 1000.0);
        if (parked_entry && bar_index_ == 0) strategy_entry("Parked", true, 1.0, qnan, 0.001);
        if (bar_index_ == 3) { final_view = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

const std::vector<Bar> bars = {
    {110727.28, 110920.00, 110502.44, 110502.45, 1, 1000},
    {110502.44, 110675.31, 110500.00, 110675.30, 1, 2000},
    {110675.31, 111326.20, 110666.66, 110981.97, 1, 3000},
    {110981.98, 111168.00, 110818.18, 110820.93, 1, 4000},
};

void test_carried_script_reads_partial_before_close_or_reverse() {
    for (Action action : {Action::HOLD, Action::REVERSE, Action::HALF}) {
        for (bool trail : {false, true}) {
            CarriedShort engine(action);
            engine.trail = trail;
            engine.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(near(engine.first_view, -12.44432));
            CHECK(near(engine.second_view, -12.33168));
            CHECK(engine.second_closed == 2);
            CHECK(engine.rows().size() == (action == Action::HOLD ? 3u : 4u));
            if (engine.rows().size() < 3) continue;
            CHECK(engine.rows()[0].exit_id == "__margin_call__");
            CHECK(engine.rows()[0].exit_time == 2000);
            CHECK(near(engine.rows()[0].qty, 0.1574));
            CHECK(near(engine.rows()[0].exit_price, 110675.31));
            CHECK(engine.rows()[1].exit_id == "__margin_call__");
            CHECK(engine.rows()[1].exit_time == 3000);
            CHECK(near(engine.rows()[1].qty, 0.11264));
            CHECK(near(engine.rows()[1].exit_price, 111326.2));
            if (action == Action::HALF) {
                CHECK(near(engine.rows()[2].qty, 6.16584));
                CHECK(near(engine.final_view, -6.16584));
            } else if (action == Action::REVERSE) {
                CHECK(near(engine.rows()[2].qty, 12.33168));
                CHECK(near(engine.final_view, 2.0));
            } else {
                CHECK(near(engine.final_view, -12.33168));
            }
        }
    }
}

void test_funded_and_competing_order_controls() {
    CarriedShort funded(Action::REVERSE, 2000000.0);
    funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(funded.first_view, -12.60172));
    CHECK(near(funded.second_view, -12.60172));
    CHECK(funded.second_closed == 0);
    CHECK(funded.rows().size() == 2);
    CHECK(near(funded.final_view, 2.0));
    // A competing pending ENTRY keeps its established transaction scheduling.
    CarriedShort competing(Action::REVERSE);
    competing.parked_entry = true;
    competing.run(bars.data(), static_cast<int>(bars.size()));
    // A competing pending ENTRY keeps its established transaction scheduling
    // (base literal; ab9714be tests/test_carried_pooc_short_margin_state.cpp:109).
    CHECK(near(competing.second_view, -12.44432));
    // The same command topology with prices and capital rescaled together
    // enters the broker's separate rounded-margin financial class. Keep its
    // established script timing until that class has its own complete proof.
    std::vector<Bar> smaller = bars;
    for (auto& bar : smaller) {
        bar.open *= 0.00001; bar.high *= 0.00001;
        bar.low *= 0.00001; bar.close *= 0.00001;
    }
    CarriedShort rounded_margin(Action::REVERSE, 1392521.546177, 0.00001);
    rounded_margin.run(smaller.data(), static_cast<int>(smaller.size()));
    CHECK(near(rounded_margin.first_view, -12.60172));
    CHECK(rounded_margin.second_closed == 1);
}

class FreshShort : public pineforge::source::PineStrategyHost {
public:
    double view = qnan;
    explicit FreshShort(bool stop) : stop_(stop) {
        initial_capital_ = 1000.0;
        qty_step_ = 0.01;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        process_orders_on_close_ = true;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, qnan, qnan, 10.0);
            if (stop_) strategy_exit("Stop", "S", qnan, 101.0);
        }
        if (bar_index_ == 1) { view = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
private:
    bool stop_;
};

void test_fresh_close_fill_and_earlier_stop_are_not_replayed() {
    const std::vector<Bar> fresh = {
        {100.0, 200.0, 99.0, 100.0, 1, 1000},
        {100.0, 100.0, 99.0, 99.0, 1, 2000},
    };
    FreshShort engine(false);
    engine.run(fresh.data(), static_cast<int>(fresh.size()));
    CHECK(near(engine.view, -10.0));
    CHECK(engine.rows().size() == 1);
    CHECK(!engine.rows().empty() && engine.rows()[0].exit_id != "__margin_call__");
    const std::vector<Bar> stop = {
        {100.0, 100.0, 100.0, 100.0, 1, 1000},
        {100.0, 120.0, 99.0, 110.0, 1, 2000},
    };
    FreshShort stopped(true);
    stopped.run(stop.data(), static_cast<int>(stop.size()));
    CHECK(near(stopped.view, 0.0));
    CHECK(stopped.rows().size() == 1);
    CHECK(!stopped.rows().empty() && stopped.rows()[0].exit_id == "Stop");
}

class FullReplacement : public pineforge::source::PineStrategyHost {
public:
    double view = qnan;
    bool dead_bracket_visible = false;
    FullReplacement() {
        initial_capital_ = 9064.334481;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        process_orders_on_close_ = true;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, qnan, qnan, 0.07912);
            strategy_exit("Owned", "S", qnan, 130000.0);
        }
        if (bar_index_ == 1) {
            view = signed_position_size();
            for (const auto& order : pending_orders_) {
                if (order.id == "Owned") dead_bracket_visible = true;
            }
            if (view == 0.0) {
                strategy_entry("S", false, qnan, qnan, 0.07);
                strategy_exit("Owned", "S", qnan, 114460.0);
            }
        }
        if (bar_index_ == 3) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_full_liquidation_retires_only_the_old_owned_bracket() {
    const std::vector<Bar> replacement = {
        {114643.19, 114781.21, 114555.00, 114555.00, 1, 1000},
        {114555.00, 114564.69, 114350.57, 114400.00, 1, 2000},
        {114400.01, 114600.94, 114378.99, 114454.93, 1, 3000},
        {114454.93, 114521.97, 114402.65, 114437.70, 1, 4000},
    };
    FullReplacement engine;
    engine.run(replacement.data(), static_cast<int>(replacement.size()));
    CHECK(near(engine.view, 0.0));
    CHECK(!engine.dead_bracket_visible);
    CHECK(engine.rows().size() == 2);
    if (engine.rows().size() != 2) return;
    CHECK(engine.rows()[0].exit_id == "__margin_call__");
    CHECK(near(engine.rows()[0].qty, 0.07912));
    CHECK(near(engine.rows()[0].exit_price, 114564.69));
    CHECK(engine.rows()[1].entry_time == 2000);
    CHECK(near(engine.rows()[1].qty, 0.07));
    CHECK(near(engine.rows()[1].entry_price, 114400.0));
    CHECK(engine.rows()[1].exit_id == "Owned");
    CHECK(engine.rows()[1].exit_time == 3000);
    CHECK(near(engine.rows()[1].exit_price, 114460.0));
}
}

int main() {
    test_carried_script_reads_partial_before_close_or_reverse();
    test_funded_and_competing_order_controls();
    test_fresh_close_fill_and_earlier_stop_are_not_replayed();
    test_full_liquidation_retires_only_the_old_owned_bracket();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
