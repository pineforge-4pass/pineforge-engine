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

/*
 * Close-first all-in admission (round 12 AG-C1): strategy.close(current)
 * followed by strategy.entry(opposite) still checks the rounded SIGNAL
 * cost (rule 2), but does not take the price-scale whole-drop (rule 5).
 *
 * Six TradingView sensors, famag-C-cf-d{-3,+0,+1,+2,+3,+5}, pin the capital
 * boundary at signal close 1.13384 after short 870000 @ 1.13523. Their exact
 * declared capitals are used below in a synthetic five-bar fixture; this
 * is not a corpus, feed, or grader replay. Source and TV tape readback:
 * campaign log-20260906t001223z-0fda20b7. B-z-tie-cf separately proves that
 * passing rule 2 must not activate rule 5 for this ordering. The gap control
 * protects the existing demete1226 contract: judge frozen signal equity,
 * then let a fill-time deficit be trimmed rather than declining the entry.
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_policy_support.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
using pineforge::source::tv_money_round;
using pineforge::source::PendingOrder;

static int passed = 0;
static int failed = 0;
#define CHECK(expr) do { \
    if (expr) { ++passed; } else { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failed; \
    } \
} while (0)

namespace {
constexpr double kSeedPrice = 1.13523;
constexpr double kSignalPrice = 1.13384;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Ordering { CloseFirst, EntryFirst };

class CloseFirstProbe : public pineforge::source::PineStrategyHost {
public:
    CloseFirstProbe(double capital, Ordering ordering = Ordering::CloseFirst,
                    bool seed_long = false)
        : ordering_(ordering), seed_long_(seed_long) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin_short_ = 100.0;
        pyramiding_ = 1;
        slippage_ = 0;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        qty_step_ = 0.01;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }

    double fill_position = kNaN;
    double settled_position = kNaN;
    double frozen_qty = kNaN;
    double signal_equity = kNaN;
    double signal_price = kNaN;
    int margin_rows = 0;

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Seed", seed_long_, kNaN, kNaN, 870000.0);
        } else if (bar_index_ == 1) {
            if (ordering_ == Ordering::CloseFirst) strategy_close("Seed");
            strategy_entry("Next", !seed_long_);
            for (const PendingOrder& order : pending_orders_) {
                if (order.id != "Next") continue;
                frozen_qty = order.frozen_default_qty;
                signal_equity = order.sizing_equity;
                signal_price = order.sizing_price;
            }
            if (ordering_ == Ordering::EntryFirst) strategy_close("Seed");
        } else if (bar_index_ == 2) {
            fill_position = signed_position_size();
        } else if (bar_index_ == 3) {
            settled_position = signed_position_size();
            for (const Trade& trade : trades_) {
                if (trade.exit_comment == "Margin call") ++margin_rows;
            }
            strategy_close_all();
        }
    }

private:
    Ordering ordering_;
    bool seed_long_;
};

void execute(CloseFirstProbe& engine, double fill = kSignalPrice) {
    const std::vector<Bar> bars = {
        {kSeedPrice, kSeedPrice, kSeedPrice, kSeedPrice, 1, 1000},
        {kSeedPrice, kSeedPrice, kSignalPrice, kSignalPrice, 1, 2000},
        {fill, fill, fill, fill, 1, 3000},
        {fill, fill, fill, fill, 1, 4000},
        {fill, fill, fill, fill, 1, 5000},
    };
    engine.run(bars.data(), static_cast<int>(bars.size()));
}

void six_pinned_capitals() {
    struct Case { double capital; double expected; };
    const Case cases[] = {
        {998790.990214, 881958.90}, // d-3: one lot below the rounded-cost tie
        {998790.990514, 0.0},       // d+0..3: E_s below sig10(cost)
        {998790.990614, 0.0},
        {998790.990714, 0.0},
        {998790.990814, 0.0},
        {998790.991014, 881958.91}, // d+5: E_s covers the rounded cost
    };
    for (const Case& c : cases) {
        CloseFirstProbe engine(c.capital);
        execute(engine);
        std::printf("capital %.6f: position %.2f, expected %.2f\n",
                    c.capital, engine.fill_position, c.expected);
        CHECK(std::abs(engine.fill_position - c.expected) < 1e-6);
        CHECK(engine.margin_rows == 0);
        CHECK((engine.signal_equity + 1e-9
               < tv_money_round(engine.frozen_qty * engine.signal_price))
              == (c.expected == 0.0));
    }
}

void rule5_is_ordering_specific() {
    // B-z-tie-cf: same money passes rule 2 but fails rule 5. Close-first
    // admits; entry-first drops the reversal while its separate close fills.
    CloseFirstProbe close_first(998790.695916);
    execute(close_first);
    CHECK(close_first.signal_equity + 1e-9 >= tv_money_round(
        close_first.frozen_qty * close_first.signal_price));
    CHECK(tv_money_round(tv_money_round(close_first.signal_equity)
                         / close_first.frozen_qty) < close_first.signal_price);
    CHECK(std::abs(close_first.fill_position - 881958.65) < 1e-6);

    CloseFirstProbe entry_first(998790.695916, Ordering::EntryFirst);
    execute(entry_first);
    CHECK(entry_first.fill_position == 0.0);
}

void short_direction_and_fill_gap_controls() {
    // Mirrored money boundary: the close survives and the short is declined.
    CloseFirstProbe short_drop(1001209.590514, Ordering::CloseFirst, true);
    execute(short_drop);
    CHECK(short_drop.fill_position == 0.0);

    // Rule 2 passes at the signal, then the gap worsens both the short's
    // closing equity and the new long's cost. The entry must fill and trim.
    CloseFirstProbe gap(998790.991014);
    execute(gap, 1.13394);
    CHECK(gap.signal_equity + 1e-9 >= tv_money_round(
        gap.frozen_qty * gap.signal_price));
    CHECK(gap.fill_position > 0.0);
    CHECK(gap.settled_position > 0.0);
    CHECK(gap.settled_position < gap.frozen_qty);
    CHECK(gap.margin_rows > 0);
}
} // namespace

int main() {
    six_pinned_capitals();
    rule5_is_ordering_specific();
    short_direction_and_fill_gap_controls();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
