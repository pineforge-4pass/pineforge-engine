// The two bounds of a late margin call's booking, held on a note the test
// sets itself.
//
// A margin call of the long-tick walk that belongs to its bar's open or first
// extreme is booked from the lot's extremes as of that waypoint: what the
// adapter noted of the lot as the bar opened, scaled to the units the lot
// holds at the call, and never more than the lot holds at the bar's
// calculation (PineExecutionAdapter::submit_long_tick_margin_units;
// tests/test_margin_call_long_tick_excursion.cpp pins the booking on runs).
//
// No run reaches the two cases below. A lot's noted extremes are the lot's
// own, so they never exceed what it holds later; and a sale between the bar's
// open and the call that changed the lot's units by a visible share would
// leave the book covered, so no later call of that bar would follow it. The
// booking's arithmetic still has both lines, and the rows hold them by
// writing the note between the bar's open and the walk:
//
//   1. the control, the note as the adapter took it: units T, favorable 3 a
//      unit, adverse 0. The called unit reports the 3 it carried.
//   2. the bound: the note claims a favorable 100 and an adverse 50 a unit,
//      more than the lot ever sampled. The called unit reports what the lot
//      holds at the bar's calculation, 7 and 0, not the claim.
//   3. the scaling: the note is of a lot twice as large with the same 3 a
//      unit, as if half of it had been closed since the bar opened. The
//      called unit reports 3, the noted 6 T scaled to the T the lot holds,
//      not 6.
//
// The run: the book and the low-first bars of row 1 of
// tests/test_margin_call_long_tick_excursion.cpp (one unit called at the low
// of bar 2, mark 2, before its high 8). The script of bar 1 sets a risk rule
// and the script of bar 2 lifts it, so bar 2's checks are all taken at its
// calculation, after the script: the script of bar 2 is where the note is
// read and written.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace long_tick_bounds {
using namespace pineforge;
using Adapter = pineforge::source::PineExecutionAdapter;

// Explicit member instantiation follows the adapter storage tests' access
// convention without changing the class or its access specifiers.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct OpenLotNote { friend auto access(OpenLotNote); };
template struct Access<OpenLotNote, &Adapter::long_tick_open_lot_>;

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

class NotedLot final : public source::PineStrategyHost {
public:
    NotedLot() {
        attach_pine_execution_adapter();
        set_syminfo_mintick(1.0);
        set_syminfo_metadata("qty_step", kLot);
        source::PineStrategyConfig config;
        config.initial_capital = kCapital;
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
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, kUnits);
        if (pine_bar_index() == 1) set_pine_risk_max_cons_loss_days(1000);
        if (pine_bar_index() == 2) {
            auto& note = adapter_.*access(OpenLotNote{});
            noted_units = note.units;
            noted_favorable = note.favorable;
            noted_adverse = note.adverse;
            if (!std::isnan(set_units)) note.units = set_units;
            if (!std::isnan(set_favorable)) note.favorable = set_favorable;
            if (!std::isnan(set_adverse)) note.adverse = set_adverse;
            set_pine_risk_max_cons_loss_days(0);
        }
        if (pine_bar_index() == 3) strategy_close_all();
    }

    // What the note is set to before the walk; NaN leaves a field as taken.
    double set_units = kNaN;
    double set_favorable = kNaN;
    double set_adverse = kNaN;
    // The note as the adapter took it at the open of bar 2.
    double noted_units = kNaN;
    double noted_favorable = kNaN;
    double noted_adverse = kNaN;
};

void run_row(const char* tag, double set_units, double set_favorable, double set_adverse,
             double call_favorable) {
    NotedLot book;
    book.set_units = set_units;
    book.set_favorable = set_favorable;
    book.set_adverse = set_adverse;
    const std::vector<Bar> bars = {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 4.0, 1.0, 4.0),
        bar(2, 4.0, 8.0, 2.0, 4.0),
        bar(3, 4.0, 4.0, 4.0, 4.0),
        bar(4, 4.0, 4.0, 4.0, 4.0)};
    book.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(book.last_error().empty());
    // The note the adapter took as bar 2 opened: the lot bar 1 left.
    std::printf("%s note units=%.6f favorable=%.6f adverse=%.9f\n", tag, book.noted_units,
                book.noted_favorable, book.noted_adverse);
    CHECK(near(book.noted_units, kUnits, 1e-9));
    CHECK(near(book.noted_favorable, 3.0 * kUnits, 1e-6));
    CHECK(near(book.noted_adverse, 0.0, 1e-9));
    CHECK(book.trade_count() == 2);
    if (book.trade_count() != 2) {
        std::printf("%s: %d trades, want 2\n", tag, book.trade_count());
        return;
    }
    const Trade call = book.get_trade(0);
    const Trade rest = book.get_trade(1);
    std::printf("%s call %.6f @%.2f->%.2f bar %d \"%s\" mfe=%.9f mae=%.9f (want mfe=%.9f mae=0)\n",
                tag, call.qty, call.entry_price, call.exit_price, call.exit_bar_index,
                call.exit_comment.c_str(), call.max_runup, call.max_drawdown, call_favorable);
    std::printf("%s rest %.6f @%.2f->%.2f bar %d mfe=%.6f mae=%.9f (want mfe=%.6f mae=0)\n",
                tag, rest.qty, rest.entry_price, rest.exit_price, rest.exit_bar_index,
                rest.max_runup, rest.max_drawdown, 7.0 * (kUnits - 1.0));
    // The fills are row 1's: one unit called at the low's mark on bar 2, the
    // rest closed at the open of bar 4.
    CHECK(call.exit_comment == std::string("Margin call"));
    CHECK(call.exit_bar_index == 2);
    CHECK(call.exit_price == 2.0);
    CHECK(near(call.qty, 1.0, 1e-10));
    CHECK(rest.exit_comment != std::string("Margin call"));
    CHECK(rest.exit_bar_index == 4);
    CHECK(rest.exit_price == 4.0);
    CHECK(near(rest.qty, kUnits - 1.0, 1e-9));
    // The called unit: the note, scaled and bounded.
    CHECK(near(call.max_runup, call_favorable, 1e-9));
    CHECK(near(call.max_drawdown, 0.0, 1e-9));
    // The rest of the lot has its own extremes back whatever the note said.
    CHECK(near(rest.max_runup, 7.0 * (kUnits - 1.0), 1e-6));
    CHECK(near(rest.max_drawdown, 0.0, 1e-9));
}
} // namespace long_tick_bounds

int main() {
    using namespace long_tick_bounds;
    // 1. The control: the note as taken.
    run_row("as-noted", kNaN, kNaN, kNaN, 3.0);
    // 2. The bound: a note above the lot is held to the lot.
    run_row("bounded", kNaN, 100.0 * kUnits, 50.0 * kUnits, 7.0);
    // 3. The scaling: a note of twice the units is halved.
    run_row("scaled", 2.0 * kUnits, 6.0 * kUnits, kNaN, 3.0);
    std::printf("test_margin_call_long_tick_excursion_bounds: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
