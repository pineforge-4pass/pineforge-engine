#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// R28 covered TV controls: integer short margin events, including a bracket
// revived after an opening declined reversal, precede close-time script state.
// Evidence: r28-killed-dynamic, r28-killed-explicit-child, r28-killed-funded,
// carried-half, opening-half and r28-unit-carried-high-state campaign tapes.
// The last one's TV CSV SHA is
// eb3a2e2fd74a9a56560b0525ff56126aa085d6117798fc49dfe0b8247cd14db1.
// These compact command fixtures use synthetic timestamps, fixed distances,
// and a short bar sequence; they do not run a corpus strategy or a verifier.
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

enum class Mode { OPENING_HALF, CARRIED_HALF, DYNAMIC, EXPLICIT, UNIT };
class IntegerScript : public pineforge::source::PineStrategyHost {
public:
    Mode mode;
    double opening_view = qnan, carried_view = qnan, carried_average = qnan;
    double carried_balance = qnan;
    std::size_t carried_closed = 0;
    explicit IntegerScript(Mode value, double capital = 10315.59)
        : mode(value) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = 1.0;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_short_ = 100.0;
        margin_long_ = mode == Mode::UNIT ? 50.0 : 100.0;
        pyramiding_ = 0;
    }
    void reverse(double distance) {
        const auto position = physical_position();
        const double average = position.signed_units == 0.0
            ? qnan : position.average_price;
        strategy_entry("L", true);
        if (mode == Mode::EXPLICIT && pine_bar_index() == 2) {
            strategy_exit("XL", "L", 10.63, 10.49);
        } else {
            strategy_exit("XL", "L", average + 2.0 * distance, average - distance);
        }
        strategy_exit("XS", "S", average - 2.0 * distance, average + distance);
        strategy_close("S");
    }
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) strategy_entry("S", false, qnan, qnan, mode == Mode::UNIT ? 1.0 : 991.0);
        if (index == 1) {
            opening_view = physical_position().signed_units;
            if (mode == Mode::OPENING_HALF) strategy_close("S", "half", std::floor(-opening_view / 2.0));
            if (mode == Mode::DYNAMIC || mode == Mode::EXPLICIT) reverse(0.034233333969624);
        }
        if (index == 2) {
            const auto position = physical_position();
            carried_view = position.signed_units;
            carried_average = carried_view == 0.0 ? qnan : position.average_price;
            carried_balance = current_equity();
            carried_closed = static_cast<std::size_t>(trade_count());
            if (mode == Mode::CARRIED_HALF) strategy_close("S", "half", std::floor(-carried_view / 2.0));
            if (mode == Mode::DYNAMIC || mode == Mode::EXPLICIT)
                reverse(0.040359524400365);
            if (mode == Mode::UNIT) strategy_entry("Observer", true, qnan, qnan, 1.0);
        }
        // The EXPLICIT child must survive on its original incarnation;
        // reissuing it here would mask a lost pending-parent bracket.
        if (physical_position().signed_units > 0.0 && index >= 3 && mode == Mode::DYNAMIC) {
            strategy_exit("XL", "L", 10.63, 10.49);
        }
        if (index == 5) strategy_close_all();
    }
    const Trade& row(int index) const { return get_trade(index); }
};

const std::vector<Bar> bars = {
    {10.415, 10.415, 10.39, 10.395, 1, 1000},
    {10.395, 10.44, 10.38, 10.44, 1, 2000},
    {10.50, 10.56, 10.49, 10.54, 1, 3000},
    {10.535, 10.60, 10.535, 10.56, 1, 4000},
    {10.565, 10.61, 10.56, 10.605, 1, 5000},
    {10.605, 10.66, 10.605, 10.645, 1, 6000},
    {10.61, 10.61, 10.61, 10.61, 1, 7000},
};

void test_revival_precedes_replacement_script() {
    for (Mode mode : {Mode::DYNAMIC, Mode::EXPLICIT}) {
        IntegerScript engine(mode);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.opening_view, -967.0));
        CHECK(near(engine.carried_view, 0.0));
        CHECK(std::isnan(engine.carried_average));
        CHECK(near(engine.carried_balance, 10159.91));
        CHECK(engine.carried_closed == 3);
        CHECK(engine.trade_count() == 4);
        if (engine.trade_count() != 4) continue;
        CHECK(engine.row(0).exit_id == "__margin_call__");
        CHECK(near(engine.row(0).qty, 24.0));
        CHECK(near(engine.row(0).exit_price, 10.44));
        CHECK(engine.row(1).exit_id == "__margin_call__");
        CHECK(near(engine.row(1).qty, 16.0));
        CHECK(engine.row(1).exit_time == 3000);
        CHECK(near(engine.row(1).exit_price, 10.56));
        CHECK(engine.row(2).exit_id == "XS");
        CHECK(near(engine.row(2).qty, 951.0));
        CHECK(engine.row(2).exit_time == 3000);
        CHECK(near(engine.row(2).exit_price, 10.56));
        CHECK(engine.row(3).entry_time == 4000);
        CHECK(near(engine.row(3).qty, 963.0));
        CHECK(near(engine.row(3).entry_price, 10.54));
        CHECK(engine.row(3).exit_id == "XL");
        CHECK(engine.row(3).exit_time == 6000);
        CHECK(near(engine.row(3).exit_price, 10.63));
    }
}

void test_partial_state_and_funded_control() {
    for (Mode mode : {Mode::OPENING_HALF, Mode::CARRIED_HALF}) {
        IntegerScript engine(mode);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.opening_view, -967.0));
        CHECK(engine.trade_count() == (mode == Mode::OPENING_HALF ? 3 : 4));
        if (mode == Mode::CARRIED_HALF) CHECK(near(engine.carried_view, -951.0));
        bool found_half = false;
        for (int index = 0; index < engine.trade_count(); ++index) {
            const auto& trade = engine.row(index);
            if (trade.exit_comment != "half") continue;
            found_half = true;
            CHECK(near(trade.qty, mode == Mode::OPENING_HALF ? 483.0 : 475.0));
        }
        CHECK(found_half);
    }
    IntegerScript funded(Mode::DYNAMIC, 11315.59);
    funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(funded.opening_view, -991.0));
    CHECK(near(funded.carried_view, -991.0));
    CHECK(funded.carried_closed == 0);
    CHECK(funded.trade_count() == 2);
    if (funded.trade_count() != 2) return;
    CHECK(funded.row(0).exit_time == 4000);
    CHECK(near(funded.row(0).qty, 991.0));
    CHECK(funded.row(1).entry_time == 4000);
    CHECK(funded.row(1).exit_time == 4000);
    CHECK(funded.row(1).exit_id == "XL");
    CHECK(near(funded.row(1).qty, 1060.0));
}

void test_one_unit_adverse_high() {
    for (double capital : {10.4, 10.5, 10.6}) {
        IntegerScript engine(Mode::UNIT, capital);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.opening_view, capital == 10.4 ? 0.0 : -1.0));
        CHECK(near(engine.carried_view, 0.0));
        CHECK(engine.carried_closed == 1);
        CHECK(engine.trade_count() == 2);
        if (engine.trade_count() != 2) continue;
        CHECK(engine.row(0).exit_id == "__margin_call__");
        CHECK(near(engine.row(0).qty, 1.0));
        CHECK(engine.row(0).exit_time == (capital == 10.4 ? 2000 : 3000));
        CHECK(near(engine.row(0).exit_price,
                   capital == 10.4 ? 10.44 : (capital == 10.5 ? 10.50 : 10.56)));
    }
}
}

int main() {
    test_revival_precedes_replacement_script();
    test_partial_state_and_funded_control();
    test_one_unit_adverse_high();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
