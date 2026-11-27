/*
 * test_issue_1031_percent_stop_money_tie.cpp -- engine issue 1031.
 *
 * A default percent-of-equity stop entry is sized at its own level from the
 * ten-significant-digit money. A capital a hair under the cost of a whole
 * number of units therefore sizes that whole number: 92499.9999999 rounds to
 * 92500, and 92500 / 1.15625 is 80000 units, which cost 92500 -- 1e-7 more
 * than the capital. TradingView fills those 80000 units at the level and
 * closes 1 unit by "Margin call" on the fill bar, keeping 79999. With a
 * capital of exactly 92500, or one cent more, it opens 80000 units and calls
 * nothing.
 *
 * Before the fix the adapter sized the same 80000 units and then refused the
 * whole order when the stop was reached: the fill-time funds check compared
 * the cost with the binary equity and a float guard of
 * max(1e-9, equity * 1e-12) = 9.25e-8, which the 1e-7 shortfall exceeds. No
 * position opened and no margin call was booked.
 *
 *   A. capital 92499.9999999, whole-unit lot: 80000 filled at the level, one
 *      unit called on the fill bar, 79999 closed by the script.
 *      RED before the fix: no trade row at all.
 *   B. the same capital on a 0.01 lot step: the same rows.
 *      RED before the fix: no trade row at all.
 *   C. capital 92500, cost equal to the capital: 80000, no call. Control.
 *   D. capital 92500.01: 80000, no call. Control.
 *
 * Every number is invented. Each price is a multiple of 1/32 on the 0.00001
 * tick, so the products and differences the money is made of are exact in
 * binary64 and no comparison here rests on rounding residue.
 *
 * Not asserted: the price the called unit is closed at. The measurement gives
 * the bar of the call and its size, not its price.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kStop = 1.15625;        // 37 / 32, exact in binary64
constexpr double kFilledUnits = 80000.0; // 92500 / 1.15625
constexpr double kScriptClose = 1.1875;  // the open the script's close fills at
constexpr int kFillBar = 1;
constexpr int kScriptCloseBar = 3;

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

Bar mk_bar(std::int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

// Bar 0 places the stop above its close. Bar 1 opens under the level, goes to
// its low first (the open is nearer the low) and then up through the level to
// the high, so the stop fills at the level inside the bar with the high still
// ahead. The script closes "L" on bar 2 and that close fills at bar 3's open.
std::vector<Bar> bars() {
    return {
        mk_bar(1000, 1.125, 1.125, 1.125, 1.125),
        mk_bar(2000, 1.125, 1.21875, 1.09375, 1.1875),
        mk_bar(3000, 1.1875, 1.1875, 1.1875, 1.1875),
        mk_bar(4000, 1.1875, 1.1875, 1.1875, 1.1875),
        mk_bar(5000, 1.1875, 1.1875, 1.1875, 1.1875),
    };
}

// 100 % of equity, full margin both sides, no commission, no slippage,
// pyramiding 0: one stop entry placed once, one script close.
class Probe final : public source::PineStrategyHost {
public:
    Probe(double capital, double lot_step) {
        // The strategy declaration goes through the host's configuration:
        // the run is formed from it at begin, not from the kernel's members.
        source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.slippage = 0;
        config.pyramiding = 0;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        // The instrument: size step, point value and tick.
        qty_step_ = lot_step;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.00001;
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kStop);
        if (bar_index_ == 2) strategy_close("L");
    }

    bool margin_row(int i) const {
        return closed_trade_exit_comment(i) == std::string("Margin call");
    }
    int margin_rows() const {
        int count = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (margin_row(i)) ++count;
        }
        return count;
    }
    double size(int i) const { return closed_trade_size(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    int entry_bar(int i) const { return closed_trade_entry_bar_index(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
};

// The rows are read only when their count is the expected one: a different
// count leaves nothing to line the rows up against.
bool rows_comparable(const char* name, const Probe& eng, int expected_rows) {
    const int rows = eng.trade_count();
    CHECK(rows == expected_rows);
    if (rows == expected_rows) return true;
    std::printf("  NOT COMPARED  %s: trade rows actual=%d expected=%d; "
                "row sizes, prices and bars were not read\n",
                name, rows, expected_rows);
    ++tests_failed;
    return false;
}

// The script-closed row of a run: the units it holds, in at the level on the
// fill bar, out at the script close's open.
void check_script_row(const Probe& eng, int row, double units) {
    CHECK(!eng.margin_row(row));
    CHECK(near(eng.size(row), units));
    CHECK(near(eng.entry_price(row), kStop));
    CHECK(eng.entry_bar(row) == kFillBar);
    CHECK(near(eng.exit_price(row), kScriptClose));
    CHECK(eng.exit_bar(row) == kScriptCloseBar);
}

// A and B: the rounded-up size fills whole and one unit is called on the fill
// bar. RED before the fix: the order was refused at its fill, so there is no
// trade row and no "Margin call" row.
void hair_under_fills_whole_and_calls_one_unit(const char* name, double lot_step) {
    std::printf("%s\n", name);
    Probe eng(92499.9999999, lot_step);
    const auto feed = bars();
    eng.run(feed.data(), static_cast<int>(feed.size()));

    CHECK(eng.margin_rows() == 1);
    CHECK(near(eng.position_size(), 0.0));
    if (!rows_comparable(name, eng, 2)) return;
    const int called = eng.margin_row(0) ? 0 : 1;
    const int kept = 1 - called;
    if (!eng.margin_row(called)) {
        std::printf("  NOT COMPARED  %s: \"Margin call\" rows actual=%d expected=1; "
                    "the called and the kept row were not told apart\n",
                    name, eng.margin_rows());
        ++tests_failed;
        return;
    }
    // The called unit: one unit of the 80000 filled at the level, opened and
    // closed on the fill bar.
    CHECK(near(eng.size(called), 1.0));
    CHECK(near(eng.entry_price(called), kStop));
    CHECK(eng.entry_bar(called) == kFillBar);
    CHECK(eng.exit_bar(called) == kFillBar);
    // The rest stays until the script closes it.
    check_script_row(eng, kept, kFilledUnits - 1.0);
    CHECK(near(eng.size(called) + eng.size(kept), kFilledUnits));
}

// C and D: the cost fits the capital, exactly or with a cent to spare. The
// same 80000 units open and nothing is called, before and after the fix.
void funded_size_opens_whole_without_call(const char* name, double capital) {
    std::printf("%s\n", name);
    Probe eng(capital, 1.0);
    const auto feed = bars();
    eng.run(feed.data(), static_cast<int>(feed.size()));

    CHECK(eng.margin_rows() == 0);
    CHECK(near(eng.position_size(), 0.0));
    if (!rows_comparable(name, eng, 1)) return;
    check_script_row(eng, 0, kFilledUnits);
}

}  // namespace

int main() {
    std::printf("=== test_issue_1031_percent_stop_money_tie ===\n");

    hair_under_fills_whole_and_calls_one_unit("A hair-under capital, whole-unit lot", 1.0);
    hair_under_fills_whole_and_calls_one_unit("B hair-under capital, 0.01 lot step", 0.01);
    funded_size_opens_whole_without_call("C cost equal to the capital", 92500.0);
    funded_size_opens_whole_without_call("D one cent of slack", 92500.01);

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
