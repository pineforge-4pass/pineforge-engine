// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// R39 covered BTC/XAU TradingView controls pin price-scale admission for
// ordinary fractional market books whose minimum lot is worth >=1. These
// literal command fixtures use synthetic timestamps and constant fill bars
// to isolate admission from the later intrabar margin trims. No corpus,
// indicator, historical strategy or grader is executed by this test.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr double seed_price = 106909.09;
constexpr double signal_price = 106318.18;
constexpr double base_equity = 830017.5259234;
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
enum class Ordering { Bare, EntryFirst, CloseFirst };

class Reversal : public pineforge::source::PineStrategyHost {
public:
    Ordering ordering;
    bool seed_long;
    double literal_qty;
    double frozen = na, after = na;
    Reversal(Ordering order, bool direction, double extra = 0.0,
             double literal = na, double percent = 100.0)
        : ordering(order), seed_long(direction), literal_qty(literal) {
        initial_capital_ = base_equity + extra
            + (seed_long ? 590.91 : -590.91);
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = percent;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("Seed", seed_long, na, na, 1.0);
        if (bar_index_ == 1) {
            if (ordering == Ordering::CloseFirst) strategy_close("Seed");
            strategy_entry("New", !seed_long, na, na, literal_qty);
            for (const auto& order : pending_orders_)
                if (order.id == "New") frozen = order.frozen_default_qty;
            if (ordering == Ordering::EntryFirst) strategy_close("Seed");
        }
        if (bar_index_ == 2) { after = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void execute(Reversal& engine) {
    const Bar bars[] = {
        {seed_price, seed_price, seed_price, seed_price, 1, 1000},
        {seed_price, seed_price, signal_price, signal_price, 1, 2000},
        {signal_price, signal_price, signal_price, signal_price, 1, 3000},
        {signal_price, signal_price, signal_price, signal_price, 1, 4000},
    };
    engine.run(bars, 4);
}

void test_ordering_and_direction() {
    for (bool seed_long : {false, true}) {
        for (Ordering ordering : {Ordering::Bare, Ordering::EntryFirst, Ordering::CloseFirst}) {
            Reversal engine(ordering, seed_long);
            execute(engine);
            const double sign = seed_long ? 1.0 : -1.0;
            CHECK(near(engine.frozen, 7.80692));
            if (ordering == Ordering::CloseFirst) {
                CHECK(near(engine.after, -sign * 7.80692));
                CHECK(engine.rows().size() == 2);
            } else if (ordering == Ordering::EntryFirst) {
                CHECK(near(engine.after, 0.0));
                CHECK(engine.rows().size() == 1);
            } else {
                CHECK(near(engine.after, sign));
                CHECK(engine.rows().size() == 1);
            }
            CHECK(!engine.rows().empty());
            if (!engine.rows().empty()) {
                CHECK(engine.rows()[0].entry_id == "Seed");
                CHECK(engine.rows()[0].exit_time == (ordering == Ordering::Bare ? 4000 : 3000));
            }
        }
    }
}

void test_funding_and_sizing_controls() {
    for (double extra : {-0.0002, -0.0001, 0.0, 0.0002, 0.0003}) {
        Reversal engine(Ordering::EntryFirst, true, extra);
        execute(engine);
        const bool admitted = extra < 0.0 || extra >= 0.0003;
        const double qty = extra < 0.0 ? 7.80691 : 7.80692;
        CHECK(near(engine.frozen, qty));
        CHECK(near(engine.after, admitted ? -qty : 0.0));
        CHECK(engine.rows().size() == (admitted ? 2u : 1u));
    }
    // The exact explicit-quantity reversal is a separately recorded TV
    // mismatch in the existing explicit admission path. This default-sizing
    // fix does not claim to repair it. The one-lot-less and 99% admissions
    // below remain covered controls for unaffected sizing paths.
    Reversal less(Ordering::EntryFirst, true, 0.0, 7.80691);
    execute(less);
    CHECK(near(less.after, -7.80691));
    CHECK(less.rows().size() == 2);
    Reversal fractional(Ordering::EntryFirst, true, 0.0, na, 99.0);
    execute(fractional);
    CHECK(near(fractional.after, -7.72885));
    CHECK(fractional.rows().size() == 2);
}

class Flat : public pineforge::source::PineStrategyHost {
public:
    bool is_long;
    double after = na;
    Flat(double equity, double step, double tick, bool direction) : is_long(direction) {
        initial_capital_ = equity;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = step;
        syminfo_mintick_ = tick;
        syminfo_.pointvalue = 1.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("New", is_long);
        if (bar_index_ == 1) { after = signed_position_size(); strategy_close_all(); }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_flat_across_price_scales() {
    struct Case { double equity, funded_extra, price, step, tick, qty; };
    const Case cases[] = {
        {base_equity, 0.0003, signal_price, 0.00001, 0.01, 7.80692},
        {1034160.0001, 0.0006, 3447.2, 0.01, 0.001, 300.0},
    };
    for (const auto& c : cases) {
        for (bool is_long : {false, true}) {
            for (bool funded : {false, true}) {
                Flat engine(c.equity + (funded ? c.funded_extra : 0.0), c.step, c.tick, is_long);
                const Bar bars[] = {
                    {c.price, c.price, c.price, c.price, 1, 1000},
                    {c.price, c.price, c.price, c.price, 1, 2000},
                    {c.price, c.price, c.price, c.price, 1, 3000},
                };
                engine.run(bars, 3);
                CHECK(near(engine.after, funded ? (is_long ? c.qty : -c.qty) : 0.0));
                CHECK(engine.rows().size() == (funded ? 1u : 0u));
            }
        }
    }
}
} // namespace

int main() {
    test_ordering_and_direction();
    test_funding_and_sizing_controls();
    test_flat_across_price_scales();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
