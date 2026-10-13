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
 * test_limit_exact_touch_level_residue.cpp — exact-touch level regression.
 * A resting stop/limit level within 0.01 / pricescale^2 of k / pricescale
 * is stored on that grid before comparison with the tick-quantized bar.
 * Pinned against TradingView behaviour (recorded tapes are kept outside
 * this repository). Every bar and timestamp below is invented.
 *
 * At tick 0.1, 100.4 + (100.4 - 98.6) is 102.20000000000002:
 * comparing the raw level skips a high of 102.2 and fills only at bar 5.
 * The literal and negative-residue controls also fill at bar 2, so this
 * distinguishes storage snapping from merely changing an inclusive compare.
 * The short mirrors the failure with a residue below its exact-touch low.
 *
 * NDEBUG-PROOF: every assertion uses the returning CHECK macro.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);\
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kT0 = 1'000'000LL;  // invented epoch start
constexpr int64_t k1m = 60'000LL;

static Bar mk(int i, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = 1000;
    b.timestamp = kT0 + i * k1m;
    return b;
}

// Synthetic instrument: mintick 0.1, fixed 1 contract, no slippage/commission.
class LimitTouchProbe : public pineforge::source::PineStrategyHost {
public:
    bool is_long_;
    double limit_, stop_;
    LimitTouchProbe(bool is_long, double limit, double stop)
        : is_long_(is_long), limit_(limit), stop_(stop) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 1;
        syminfo_mintick_ = 0.1;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", is_long_, kNaN, kNaN, 1.0);
        if (bar_index_ >= 1) strategy_exit("X", "E", limit_, stop_);
    }
};

static void run_case(const char* name, bool is_long, double limit, double stop,
                     const std::vector<Bar>& bars, int want_exit_bar, double want_exit_px) {
    std::printf("  %s (limit %.17g)\n", name, limit);
    LimitTouchProbe p(is_long, limit, stop);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(p.get_trade(0).exit_bar_index == want_exit_bar);
        CHECK(near(p.get_trade(0).exit_price, want_exit_px));
    }
}
}  // namespace

static void test_long_sell_limit_exact_touch() {
    std::printf("test_long_sell_limit_exact_touch\n");
    // Bar 1 enters at 100.4; only bar 2 touches 102.2 until bar 5 exceeds it.
    const std::vector<Bar> bars = {
        mk(0, 100.0, 100.6, 99.5, 100.2),
        mk(1, 100.4, 100.8, 100.2, 100.6),
        mk(2, 100.6, 102.2, 100.1, 101.6),
        mk(3, 101.6, 101.9, 100.8, 101.0),
        mk(4, 101.0, 101.5, 100.5, 101.2),
        mk(5, 101.2, 104.0, 100.9, 103.0),
        mk(6, 103.0, 103.8, 102.5, 103.5),
    };
    const double ep = 100.4, lvl = 98.6;
    // Decimal target: 100.4 + (100.4 - 98.6) = 102.2; both residues snap
    // to that target, so all three exits must be bar 2 at 102.2.
    run_case("literal 102.2", true, 102.2, 90.0, bars, 2, 102.2);
    run_case("ep + (ep - lvl) = 102.20000000000002", true, ep + (ep - lvl), 90.0, bars, 2, 102.2);
    run_case("100.1 + (100.1 - 98.0) = 102.19999999999999", true, 100.1 + (100.1 - 98.0), 90.0, bars, 2, 102.2);
}

static void test_short_buy_limit_exact_touch() {
    std::printf("test_short_buy_limit_exact_touch\n");
    // Bar 2 alone touches 81.2; the later bar 5 trades strictly below it.
    const std::vector<Bar> bars = {
        mk(0, 82.0, 82.4, 81.4, 81.8),
        mk(1, 81.3, 81.8, 81.3, 81.6),
        mk(2, 81.6, 82.1, 81.2, 81.7),
        mk(3, 81.7, 82.5, 81.5, 82.0),
        mk(4, 82.0, 82.8, 81.6, 82.4),
        mk(5, 82.4, 82.6, 80.0, 80.5),
        mk(6, 80.5, 81.0, 80.2, 80.8),
    };
    const double ep = 81.3, lvl = 81.4;
    // 81.3 - (81.4 - 81.3) = 81.19999999999999 in binary64, but the
    // decimal grid target is 81.2: both exits must fill at bar 2 at 81.2.
    run_case("literal 81.2", false, 81.2, 90.0, bars, 2, 81.2);
    run_case("ep - (lvl - ep) = 81.19999999999999", false, ep - (lvl - ep), 90.0, bars, 2, 81.2);
}

int main() {
    test_long_sell_limit_exact_touch();
    test_short_buy_limit_exact_touch();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
