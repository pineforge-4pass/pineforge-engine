#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// R25 covered TV controls: a pure STOP filled at the opening point exposes
// its completed margin event to the script; an unhit pending entry survives.
// Compact command fixtures use synthetic timestamps, not historical replay.
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

const std::vector<Bar> bars = {
    {114643.19, 114781.21, 114555.0, 114555.0, 1, 1000},
    {114555.0, 114564.69, 114350.57, 114400.0, 1, 2000},
    {114400.01, 114600.94, 114378.99, 114454.93, 1, 3000},
    {114454.93, 114521.97, 114402.65, 114437.7, 1, 4000},
    {114437.71, 114657.0, 114437.7, 114514.05, 1, 5000},
    {114514.05, 114865.32, 114449.91, 114697.22, 1, 6000},
};

class StopBook : public pineforge::source::PineStrategyHost {
public:
    bool opposite, half_close, smaller, carried_half;
    double first_view = qnan;
    double carried_view = qnan;
    std::size_t first_closed = 0;
    StopBook(bool other = true, double capital = 9064.3344809999962,
             bool half = false, bool less = false, bool carry = false)
        : opposite(other), half_close(half), smaller(less), carried_half(carry) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 1) {
            first_view = physical_position().signed_units;
            first_closed = static_cast<std::size_t>(trade_count());
        }
        if (index == 2) carried_view = physical_position().signed_units;
        if (index <= 1 && physical_position().signed_units == 0) {
            if (opposite) strategy_entry("Long", true, qnan, 117030.0, live_current_equity() / 117030.0);
            const double quantity = smaller ? 0.07911 : live_current_equity() / 114560.0;
            strategy_entry("Short", false, qnan, 114560.0, quantity);
        }
        if (physical_position().signed_units < 0) {
            strategy_exit("Exit Short", "Short", qnan, 117030.0);
            strategy_cancel("Long");
        }
        if (physical_position().signed_units > 0) {
            strategy_exit("Exit Long", "Long", qnan, 114560.0);
            strategy_cancel("Short");
        }
        if (half_close && index == 1) strategy_close("Short", "half", qnan, 50.0);
        if (carried_half && index == 2) strategy_close("Short", "carry half", qnan, 50.0);
        if (index == 4) strategy_close_all();
    }
    std::vector<Trade> rows() const {
        std::vector<Trade> result;
        for (int index = 0; index < trade_count(); ++index) result.push_back(get_trade(index));
        return result;
    }
};

void test_full_stop_liquidation_and_replacement() {
    for (bool opposite : {false, true}) {
        StopBook engine(opposite);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.first_view, 0));
        CHECK(engine.first_closed == 1);
        CHECK(engine.rows().size() == 3);
        if (engine.rows().size() != 3) continue;
        CHECK(engine.rows()[0].exit_id == "__margin_call__");
        CHECK(engine.rows()[0].exit_time == 2000);
        CHECK(near(engine.rows()[0].qty, 0.07912));
        CHECK(near(engine.rows()[0].exit_price, 114564.69));
        CHECK(engine.rows()[1].entry_time == 3000);
        CHECK(engine.rows()[1].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[1].qty, 0.00064));
        CHECK(near(engine.rows()[1].entry_price, 114400.01));
        CHECK(engine.rows()[2].exit_time == 6000);
        CHECK(near(engine.rows()[2].qty, 0.07847));
        CHECK(near(engine.rows()[2].exit_price, 114514.05));
    }
}

void test_partial_and_no_opening_event() {
    StopBook partial(true, 11456.0, true);
    partial.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(partial.first_view, -0.09996));
    CHECK(partial.first_closed == 1);
    CHECK(partial.rows().size() == 3);
    if (partial.rows().size() == 3) {
        CHECK(partial.rows()[0].exit_id == "__margin_call__");
        CHECK(near(partial.rows()[0].qty, 0.00004));
        CHECK(partial.rows()[1].exit_comment == "half");
        CHECK(near(partial.rows()[1].qty, 0.04998));
        CHECK(near(partial.rows()[2].qty, 0.04998));
    }
    StopBook funded(true, 9064.3344809999962, false, true);
    funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(funded.first_view, -0.07911));
    CHECK(funded.first_closed == 0);
    CHECK(funded.rows().size() == 2);
    if (funded.rows().size() == 2) {
        CHECK(funded.rows()[0].exit_time == 3000);
        CHECK(near(funded.rows()[0].qty, 0.00016));
        CHECK(near(funded.rows()[1].qty, 0.07895));
    }
    // TV's carried-bar comment reads -0.07895 before the 50% close; it then
    // closes 0.03947 and retains 0.03948. The original STOP's open provenance
    // remains attached to the same physical lot across this partial.
    StopBook carried(true, 9064.3344809999962, false, true, true);
    carried.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(carried.carried_view, -0.07895));
    CHECK(carried.rows().size() == 3);
    if (carried.rows().size() == 3) {
        CHECK(carried.rows()[0].exit_time == 3000);
        CHECK(near(carried.rows()[0].qty, 0.00016));
        CHECK(carried.rows()[1].exit_time == 4000);
        CHECK(carried.rows()[1].exit_comment == "carry half");
        CHECK(near(carried.rows()[1].qty, 0.03947));
        CHECK(near(carried.rows()[2].qty, 0.03948));
    }
}

class PathAndLifetime : public pineforge::source::PineStrategyHost {
public:
    bool preserve;
    double first_view = qnan;
    PathAndLifetime(bool keep) : preserve(keep) {
        initial_capital_ = 9064.3344809999962;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) {
            if (preserve) strategy_entry("Long", true, qnan, 117030.0, 0.01);
            strategy_entry("Short", false, qnan, preserve ? 114560.0 : 114500.0, 0.07912);
        }
        if (index == 1) {
            first_view = physical_position().signed_units;
            if (!preserve) strategy_close_all();
        }
        if (preserve && physical_position().signed_units > 0) strategy_close_all();
    }
    std::vector<Trade> rows() const {
        std::vector<Trade> result;
        for (int index = 0; index < trade_count(); ++index) result.push_back(get_trade(index));
        return result;
    }
};

void test_prior_high_and_pending_entry_lifetime() {
    PathAndLifetime path(false);
    path.run(bars.data(), 3);
    CHECK(near(path.first_view, -0.07912));
    CHECK(path.rows().size() == 1);
    if (path.rows().size() == 1) {
        CHECK(path.rows()[0].entry_time == 2000);
        CHECK(near(path.rows()[0].entry_price, 114500.0));
        CHECK(near(path.rows()[0].qty, 0.07912));
        CHECK(path.rows()[0].exit_time == 3000);
    }
    std::vector<Bar> later = {bars[0], bars[1],
        {116900.0, 117040.0, 116890.0, 117010.0, 1, 3000},
        {116686.43, 116800.0, 116600.0, 116700.0, 1, 4000}};
    PathAndLifetime keep(true);
    keep.run(later.data(), static_cast<int>(later.size()));
    CHECK(near(keep.first_view, 0));
    CHECK(keep.rows().size() == 2);
    if (keep.rows().size() == 2) {
        CHECK(keep.rows()[0].exit_id == "__margin_call__");
        CHECK(keep.rows()[1].is_long);
        CHECK(keep.rows()[1].entry_time == 3000);
        CHECK(near(keep.rows()[1].entry_price, 117030.0));
        CHECK(near(keep.rows()[1].qty, 0.01));
    }
}

}
int main() {
    test_full_stop_liquidation_and_replacement();
    test_partial_and_no_opening_event();
    test_prior_high_and_pending_entry_lifetime();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
