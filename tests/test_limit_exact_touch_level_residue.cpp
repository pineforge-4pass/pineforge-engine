/*
 * test_limit_exact_touch_level_residue.cpp — issue #177 (dfl3xrs, 2026-09-04):
 * a strategy.exit(limit=) target that the bar's extreme reaches EXACTLY is
 * filled by TradingView on that bar; the engine filled it bars later at the
 * same price whenever the level carried a floating-point residue.
 *
 * TradingView rule (round 8 family T, pinned 2026-09-05 by 40 lab tv sensor
 * tapes on NYSE:F / CME_MINI:ES1! / OANDA:EURUSD 15m, scratchpad famT/pins):
 * a resting stop/limit LEVEL within 0.01 / pricescale^2 of a price-grid
 * value (k / pricescale) is stored AS that grid value and tested against
 * the tick-quantized bar (round 6 stop-tick-rounding). A level computed as
 * `avg_price + r` (2683.8 + (2683.8 - 2682.7) = 2684.9000000000005) is
 * therefore 2684.9 to TradingView and fills on the bar whose high is
 * 2684.9. The engine compared the RAW level with the quantized bar, so the
 * +4.5e-13 residue skipped the touch bar. The reporter's reading — a strict
 * `>` where TV uses `>=` — is not the mechanism: an exact literal level
 * fills on the touch bar on main (the compare is already inclusive); only
 * a residue-laden level misses.
 *
 * The bar is the reporter's first case verbatim (MGC 1-minute, mintick 0.1,
 * 2025-01-14 12:22 UTC: O 2683.9 H 2684.9 L 2683.7 C 2684.8, limit 2684.90,
 * TV filled 12:22 @2684.90; PineForge exited later). The short case mirrors
 * the fourth (buy limit 2038.20 on a bar whose low is 2038.2).
 *
 * The fix in flight is the order-level price-grid snap on round8/famT
 * (level_on_price_grid at strategy.entry/exit/order storage); this test is
 * the reporter's shape as a regression check for it.
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
constexpr int64_t kT0 = 1736857200000LL;  // 2025-01-14 12:20 UTC
constexpr int64_t k1m = 60'000LL;

static Bar mk(int i, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = 1000;
    b.timestamp = kT0 + i * k1m;
    return b;
}

// mintick 0.1 (COMEX micro gold), fixed 1 contract, no slippage/commission.
class LimitTouchProbe : public BacktestEngine {
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
    void on_bar(const Bar&) override {
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
    // Entry at bar 1 open 2683.8; bar 2 is the reporter's 12:22 bar (high == 2684.9).
    const std::vector<Bar> bars = {
        mk(0, 2683.5, 2683.9, 2683.2, 2683.8),
        mk(1, 2683.8, 2684.0, 2683.6, 2683.9),
        mk(2, 2683.9, 2684.9, 2683.7, 2684.8),   // exact touch: TV fills here @2684.90
        mk(3, 2684.8, 2684.8, 2684.2, 2684.5),
        mk(4, 2684.5, 2684.7, 2684.0, 2684.3),
        mk(5, 2684.3, 2686.0, 2684.1, 2685.5),   // where the engine used to fill
        mk(6, 2685.5, 2685.6, 2685.0, 2685.2),
    };
    const double ep = 2683.8, lvl = 2682.7;      // r = ep - lvl = 1.0999999999999091
    run_case("literal 2684.9", true, 2684.9, 2670.0, bars, 2, 2684.9);
    run_case("ep + (ep - lvl) = 2684.9000000000005", true, ep + (ep - lvl), 2670.0, bars, 2, 2684.9);
    run_case("2683.6 + (2683.6 - 2682.3) = 2684.8999999999996", true, 2683.6 + (2683.6 - 2682.3), 2670.0, bars, 2, 2684.9);
}

static void test_short_buy_limit_exact_touch() {
    std::printf("test_short_buy_limit_exact_touch\n");
    // Entry at bar 1 open 2038.3; bar 2 low == 2038.2 (the reporter's 4th case shape).
    const std::vector<Bar> bars = {
        mk(0, 2038.6, 2038.9, 2038.2, 2038.3),
        mk(1, 2038.3, 2038.5, 2038.3, 2038.4),
        mk(2, 2038.4, 2038.6, 2038.2, 2038.3),   // exact touch: TV fills here @2038.20
        mk(3, 2038.3, 2038.7, 2038.3, 2038.6),
        mk(4, 2038.6, 2038.9, 2038.4, 2038.8),
        mk(5, 2038.8, 2039.0, 2037.0, 2037.5),   // where the engine used to fill
        mk(6, 2037.5, 2037.9, 2037.2, 2037.6),
    };
    const double ep = 2038.3, lvl = 2038.4;      // buy limit = ep - (lvl - ep) = 2038.1999999999998
    run_case("literal 2038.2", false, 2038.2, 2050.0, bars, 2, 2038.2);
    run_case("ep - (lvl - ep) = 2038.1999999999998", false, ep - (lvl - ep), 2050.0, bars, 2, 2038.2);
}

int main() {
    test_long_sell_limit_exact_touch();
    test_short_buy_limit_exact_touch();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
