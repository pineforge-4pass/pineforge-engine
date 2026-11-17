// A margin call of a full-margin long reports the lot's excursion as of the
// path point it is booked at.
//
// The long-tick margin walk (PineExecutionAdapter::
// submit_tv_money_long_margin_call; tests/test_margin_call_long_tick_mark.cpp)
// takes the checks inside a bar from that bar's calculation, after the kernel
// has walked the bar's whole path, and books each call at its own waypoint's
// print. The kernel samples a lot's favorable and adverse extremes at every
// point of the path it walks (NativeExecutionConsumer::apply_excursion), so at
// the calculation the lot has also seen the prices printed after the call's
// waypoint, and a row closed from it then would report them.
//
// TradingView folds no extreme after a closing fill (class B of
// tests/test_e19_excursion_tape.cpp), and it counts a bar's open print ahead of
// a fill of the same bar (class C). A call booked inside the bar is read by
// class B's rule: the row of a call reports what the lot had sampled at the
// call's waypoint, and the units the call leaves open, which were open through
// the whole bar, keep the whole bar.
//
// What the rows read. The walk checks and sells at the tick mark of a path
// point, floor(price / tick + .5) * tick; the kernel samples a lot at the raw
// path prices. A bar walks its high first only when |high - open| is below
// |open - low|. A closed row's two columns are never below its own fill's
// move (the kernel folds the fill in), so a call sold at mark 2 off an entry
// at 1 reports a favorable excursion of at least 1 a unit.
//
// What the rows are. They are mechanism rows on constructed bars, not a
// TradingView observation: they pin what the kernel samples, its raw path
// prices, the first extreme included. Rows 3, 4, 5 and 7 print off the tick,
// and no TradingView reading of an off-tick first extreme is claimed. The only
// TradingView facts the comments cite are the two of
// tests/test_e19_excursion_tape.cpp: class B and class C above, and class C
// again in row 3, where the open print is one TradingView counts and the kernel
// does not sample.
//
// The book of rows 1 to 6: the one-unit call of
// test_margin_call_long_tick_mark.cpp's later_open_on_tick row.
//   T = 10000.000003 units, entry F = 1, capital E = 10000.0000055, tick 1,
//   lot .000001, margin 100, no fee. r10 rounds half up to ten significant
//   digits. Before any sale equity(P) = E + T (P - 1) at mark P:
//     P = 1: required r10(10000.000003) = 10000        < 10000.0000055
//     P = 2: required r10(20000.000006) = 20000.00001  > 20000.0000085; the
//            deficit .0000015 is under one lot's worth at 2: sell ONE unit
//     P = 4: required r10(40000.000012) = 40000.00001  < 40000.0000145
//     P = 7: required r10(70000.000021) = 70000.00002  < 70000.0000235
//     P = 8: required r10(80000.000024) = 80000.00002  < 80000.0000265
//   After the unit sold at 2 the book holds R = 9999.000003 and
//   equity(P) = 10001.0000055 + R (P - 1) = R P + 2.0000025, above the
//   ten-digit requirement of R P at every price: no second call.
// So every bar of these rows calls ONE unit at its first point whose mark is
// 2, once, and the close_all placed on bar 3 sells the rest at the open of
// bar 4. The raw prices these rows add mark at 1 (.625, .75, 1.375) or at 2
// (2.0625, 2.25): no mark outside the five above is visited.
//
//   1. low first: bar 1, the entry bar, runs from 1 to 4, so the lot carries
//      a favorable 3 a unit into bar 2. Bar 2 opens at 4 and prints its low 2
//      before its high 8 (|8 - 4| is not below |4 - 2|). The unit called at
//      the low was closed before the 8 was printed: its favorable excursion
//      is the 3 it carried, not the 7 of that later high.
//   2. high first, the control: bar 1 is flat. Bar 2 opens at 7 and prints
//      its high 8 before its low 2 (|8 - 7| is below |7 - 2|). The 8 was
//      printed while the called unit was still open: 7.
//   3. low first, the low itself: bar 1 is flat, and bar 2 prints its low
//      2.25 (mark 2) before its high 8. The unit is sold at 2, and the lot was
//      open at the 2.25 the kernel sampled on the way: 1.25, above the fill's
//      own 1 and below the later high's 7. The first extreme is counted, the
//      second is not. Bar 2 opens at 4, above that low: the open print is not
//      sampled; TradingView counts it (class C of
//      tests/test_e19_excursion_tape.cpp).
//   4. high first, the call at the first extreme: bar 1 is flat, and bar 2
//      (open 1.375, high 2.0625, low .625, close 1) prints its high first
//      (.6875 is below .75). The open marks at 1 and is covered; the high
//      marks at 2 and is the call. Favorable 1.0625, the high as sampled.
//      The low .625 is printed after the call: the called unit's adverse
//      excursion is 0, and the rest of the lot, open through it, has .375.
//   5. the adverse side carried: as row 4, with an entry bar that dips to .75
//      (mark 1, covered). The called unit reports the .25 it carried into
//      bar 2, neither 0 nor the .375 printed after its call.
//   6. the open's call, booked from the calculation: bar 1 runs from 1 to 4,
//      and its script sets a risk rule (a consecutive-loss-days limit no run
//      of these bars reaches), which the script of bar 2 lifts. While a rule
//      is set the book is outside the walk's scope, so bar 2, which opens at
//      2, takes no check at its open or inside it; once the rule is lifted
//      the walk resumes at that bar's calculation, from the open. The unit
//      is sold at the open's mark 2, and the 8 printed afterwards is not its
//      excursion: 3, what the lot carried as the bar opened.
// In rows 1, 2, 3 and 6 the rest of the lot saw the 8 and reports 7 a unit.
//
//   7. two sales in one bar, the second from a lot the first made smaller:
//      the book and bars of test_margin_call_long_tick_mark.cpp's
//      owed_sale_made row. T = 99999.000048, F = .1, E = 9999.9000055, tick
//      .1. Bar 2 (2.01, 3.01, .91, 1.01) walks its high first (1 is below
//      1.1). Its open marks at 2: 4 lots are called there, at the open, and
//      the re-check owes 4 more to the next point; the high marks at 3 and is
//      covered, so the owed 4 lots are sold there, booked from the bar's
//      calculation, from a lot 4 lots below the one the bar opened with.
//      Their favorable excursion is (3.01 - .1) a unit, the high as sampled,
//      .01 a unit above the fill's own (3 - .1). The close_all placed on
//      bar 2 sells the rest at the open of bar 3. The call at the open
//      reports its own fill's move, (2 - .1) a unit, where the open printed
//      2.01: the open print is not sampled (class C of
//      tests/test_e19_excursion_tape.cpp). 2.01 is off this row's tick, so no
//      TradingView value is claimed for it.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kLot = 0.000001;
constexpr double kUnits = 10000.000003;
constexpr double kCapital = 10000.0000055;

int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

Bar bar(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, (index + 1) * std::int64_t{86'400'000}};
}

// The book of test_margin_call_long_tick_mark.cpp: an explicit quantity at
// full margin on a sub-unit lot, one long lot opened at a bar's open.
class LongLot final : public source::PineStrategyHost {
public:
    LongLot(double capital, double tick, double units) : units_(units) {
        attach_pine_execution_adapter();
        set_syminfo_mintick(tick);
        set_syminfo_metadata("qty_step", kLot);
        source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.pyramiding = 1;
        config.slippage = 0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, units_);
        // strategy.risk.max_cons_loss_days: set on one bar, lifted on the next.
        if (rule_bar >= 0 && pine_bar_index() == rule_bar) set_pine_risk_max_cons_loss_days(1000);
        if (rule_bar >= 0 && pine_bar_index() == rule_bar + 1) set_pine_risk_max_cons_loss_days(0);
        if (pine_bar_index() == close_bar) strategy_close_all();
    }

    // The bar whose script closes the position; the close fills at the next open.
    int close_bar = 3;
    // The bar whose script sets the risk rule the next bar's script lifts.
    int rule_bar = -1;

private:
    double units_;
};

// One closed row as its test row expects it.
struct Want {
    const char* what;
    bool margin_call;    // a margin call's row, else the script's close
    int exit_bar;
    double qty;
    double qty_tol;
    double exit_price;
    double pnl;
    double favorable;
    double adverse;
    double tol;          // for the pnl and the two excursion columns
};

void run_row(const char* tag, LongLot& book, const std::vector<Bar>& bars, double entry_price,
             const std::vector<Want>& wants) {
    book.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(book.last_error().empty());
    CHECK(book.trade_count() == static_cast<int>(wants.size()));
    if (book.trade_count() != static_cast<int>(wants.size())) {
        std::printf("%s: %d trades, want %d\n", tag, book.trade_count(),
                    static_cast<int>(wants.size()));
        return;
    }
    for (int index = 0; index < book.trade_count(); ++index) {
        const Trade row = book.get_trade(index);
        const Want& want = wants[static_cast<std::size_t>(index)];
        std::printf("%s %s %.6f @%.4f->%.4f bar %d \"%s\" pnl=%.12g mfe=%.12g mae=%.12g "
                    "(want pnl=%.12g mfe=%.12g mae=%.12g)\n",
                    tag, want.what, row.qty, row.entry_price, row.exit_price, row.exit_bar_index,
                    row.exit_comment.c_str(), row.pnl, row.max_runup, row.max_drawdown,
                    want.pnl, want.favorable, want.adverse);
        // The fill: whose it is, where, how much, at what price.
        CHECK((row.exit_comment == std::string("Margin call")) == want.margin_call);
        CHECK(row.is_long);
        CHECK(row.exit_bar_index == want.exit_bar);
        CHECK(row.entry_price == entry_price);
        CHECK(row.exit_price == want.exit_price);
        CHECK(near(row.qty, want.qty, want.qty_tol));
        CHECK(near(row.pnl, want.pnl, want.tol));
        // The excursions: a called row's are the lot's at its waypoint, the
        // rest of the lot's are the whole bar's. A column that must be zero
        // is held to 1e-9 at most.
        const double zero_tol = std::min(want.tol, 1e-9);
        CHECK(near(row.max_runup, want.favorable, want.favorable == 0.0 ? zero_tol : want.tol));
        CHECK(near(row.max_drawdown, want.adverse, want.adverse == 0.0 ? zero_tol : want.tol));
    }
}

// Rows 1 to 6: one unit called on bar 2 at mark 2, the rest closed at the
// open of bar 4 at `rest_exit`.
void run_unit_call(const char* tag, const std::vector<Bar>& bars, int rule_bar,
                   double call_favorable, double call_adverse, double rest_exit,
                   double rest_favorable, double rest_adverse) {
    LongLot book(kCapital, 1.0, kUnits);
    book.rule_bar = rule_bar;
    const double rest = kUnits - 1.0;
    run_row(tag, book, bars, 1.0, {
        {"call", true, 2, 1.0, 1e-10, 2.0, 1.0, call_favorable, call_adverse, 1e-9},
        {"rest", false, 4, rest, 1e-9, rest_exit, (rest_exit - 1.0) * rest,
         rest_favorable * rest, rest_adverse * rest, 1e-6}});
}
} // namespace

int main() {
    // 1. Low first: the high of bar 2 is printed after the call.
    run_unit_call("low-first", {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 4.0, 1.0, 4.0),
        bar(2, 4.0, 8.0, 2.0, 4.0),
        bar(3, 4.0, 4.0, 4.0, 4.0),
        bar(4, 4.0, 4.0, 4.0, 4.0)}, -1, 3.0, 0.0, 4.0, 7.0, 0.0);

    // 2. High first, the control: the high of bar 2 is printed before it.
    run_unit_call("high-first", {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 7.0, 8.0, 2.0, 4.0),
        bar(3, 4.0, 4.0, 4.0, 4.0),
        bar(4, 4.0, 4.0, 4.0, 4.0)}, -1, 7.0, 0.0, 4.0, 7.0, 0.0);

    // 3. Low first, the low itself: sampled at 2.25, sold at its mark 2.
    run_unit_call("low-first-off-tick", {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 4.0, 8.0, 2.25, 4.0),
        bar(3, 4.0, 4.0, 4.0, 4.0),
        bar(4, 4.0, 4.0, 4.0, 4.0)}, -1, 1.25, 0.0, 4.0, 7.0, 0.0);

    // 4. High first, the call at the first extreme: the high as sampled is
    //    counted, the low printed after the call is not.
    run_unit_call("first-extreme-high", {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 1.375, 2.0625, 0.625, 1.0),
        bar(3, 1.0, 1.0, 1.0, 1.0),
        bar(4, 1.0, 1.0, 1.0, 1.0)}, -1, 1.0625, 0.0, 1.0, 1.0625, 0.375);

    // 5. The adverse side carried: .25 from the entry bar, not the later .375.
    run_unit_call("adverse-carried", {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 1.0, 0.75, 1.0),
        bar(2, 1.375, 2.0625, 0.625, 1.0),
        bar(3, 1.0, 1.0, 1.0, 1.0),
        bar(4, 1.0, 1.0, 1.0, 1.0)}, -1, 1.0625, 0.25, 1.0, 1.0625, 0.375);

    // 6. The open's call, booked from the calculation: the rule set on bar 1
    //    is lifted on bar 2, whose open is the call.
    run_unit_call("open-from-calculation", {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 4.0, 1.0, 4.0),
        bar(2, 2.0, 8.0, 2.0, 4.0),
        bar(3, 4.0, 4.0, 4.0, 4.0),
        bar(4, 4.0, 4.0, 4.0, 4.0)}, 1, 3.0, 0.0, 4.0, 7.0, 0.0);

    // 7. Two sales in one bar: 4 lots at the open, then the 4 lots owed to the
    //    high, booked from the calculation on the smaller lot.
    {
        const double units = 99999.000048;
        const double sold = 0.000004;
        const double rest = units - 2.0 * sold;
        LongLot book(9999.9000055, 0.1, units);
        book.close_bar = 2;
        run_row("two-sales", book, {
            bar(0, 0.11, 0.11, 0.11, 0.11),
            bar(1, 0.11, 0.11, 0.11, 0.11),
            bar(2, 2.01, 3.01, 0.91, 1.01),
            bar(3, 1.01, 1.01, 1.01, 1.01)}, 0.1, {
            {"open call", true, 2, sold, 1e-10, 2.0, (2.0 - 0.1) * sold,
             (2.0 - 0.1) * sold, 0.0, 1e-12},
            {"owed sale", true, 2, sold, 1e-10, 3.0, (3.0 - 0.1) * sold,
             (3.01 - 0.1) * sold, 0.0, 1e-12},
            {"rest", false, 3, rest, 1e-9, 1.0, (1.0 - 0.1) * rest,
             (3.01 - 0.1) * rest, 0.0, 1e-6}});
    }

    std::printf("test_margin_call_long_tick_excursion: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
