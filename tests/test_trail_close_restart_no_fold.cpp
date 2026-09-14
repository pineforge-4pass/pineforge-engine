/*
 * test_trail_close_restart_no_fold.cpp — round 10 family Y: a later-bar
 * strategy.exit re-issue that changes trail_points restarts the trailing
 * extreme from the issuing bar's CLOSE and nothing else — the issuing bar's
 * own high/low is NOT part of the new order's path (it starts at the next
 * bar's open); an offset-only re-issue keeps the running extreme (round 9
 * family Z's rule E holds on later bars too).
 *
 * winthetrade ema-9-vwap-strategy-with-atr-trailing-stop (CME_MINI:NQ1!
 * 15m, also ES1!/BTCUSDT/EURUSD/NQ1! 1D): process_orders_on_close=true,
 * calc_on_every_tick=true, strategy.exit(trail_points=atr*2,
 * trail_offset=atr*2) re-issued on EVERY bar. The ATR moves every bar, so
 * every bar's re-issue is a changed request: family Z's close restart. Under
 * process_orders_on_close the script body runs between the bar's two
 * process_pending_orders calls, and the second one folded the issuing bar's
 * own high/low into the just-restarted extreme, placing the trail at the
 * bar's extreme + offset instead of TradingView's close + offset.
 *
 * Pins (`lab tv`, ws-report tapes famy-*; pines
 * $PINEFORGE_PARITY_STATE/famy/pins; fixed qty, time-gated entry, exit
 * re-issued every bar, ta.atr(14) seeded at the tape's range start
 * 2025-04-01 00:00Z — the bar arrays below start there):
 *   famy-nq-A  short 2025-04-01 22:00Z @19652.75, trail_points=trail_offset
 *              =atr*2 -> 'Short Exit' 04-02 00:30Z @19586.25 (the fold
 *              printed 00:15Z @19606.0 = the 00:00Z low 19583.25 + 91t; TV:
 *              extreme restarted at the 00:15Z close 19572.75, then the
 *              00:30Z low 19563.25 + 92t on that bar's L->C leg).
 *   famy-nq-B  same entry, trail_points=100+bar_index%2, trail_offset=100
 *              -> 00:30Z @19588.25 (fold: 00:15Z @19608.25).
 *   famy-nq-C  control: trail_points=100, trail_offset=100+bar_index%2 ->
 *              00:15Z @19608.25 = the running low 19583.25 + 100t: an
 *              offset-only change does not restart.
 *   famy-nq-E  long 04-02 22:00Z @18915.75, atr*2 -> 04-03 00:45Z @18994.5
 *              (fold 19011.25; no restart at all: 23:30Z @18918.0).
 *   famy-btc-A long 04-01 14:45Z @84195.92, atr*2 -> 15:15Z @84897.85
 *              (fold 84901.85).
 *   famy-btc-B short 05-09 00:00Z @103054.24, atr*2 -> 00:30Z @102838.91
 *              (fold 102831.91); the ATR is seed-insensitive by then, the
 *              array starts 05-07 00:00Z.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/ta.hpp>

#include "../src/engine_internal.hpp"
#include "test_trail_close_restart_no_fold_data.hpp"

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        const double _a = (a), _b = (b);                                       \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %s  (%.6f vs %.6f)\n", __FILE__, \
                        __LINE__, #a, #b, _a, _b);                             \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The pins' broker: fixed qty, process_orders_on_close, no commission, no
// slippage, 100% margin, 1e8 capital (sizing never binds).
class WinTheTrade : public pineforge::source::PineStrategyHost {
public:
    enum Shape { ATR2 = 0, POINTS_ALT = 1, OFFSET_ALT = 2 };

    WinTheTrade(double mintick, double pointvalue, double qty_step, double qty)
        : atr_(14) {
        initial_capital_ = 100000000.0;
        syminfo_.pointvalue = pointvalue;
        syminfo_.mintick = mintick;
        syminfo_mintick_ = mintick;
        qty_step_ = qty_step;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = qty;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = true;
    }
    int64_t signal_ts = 0;
    bool signal_long = false;
    Shape shape = ATR2;

    void on_source_bar(const Bar& bar) override {
        const double atr = atr_.compute(bar.high, bar.low, bar.close);
        if (bar.timestamp == signal_ts) {
            strategy_entry(signal_long ? "Long" : "Short", signal_long);
        }
        double points = kNaN, offset = kNaN;
        switch (shape) {
            case ATR2:       points = atr * 2.0; offset = atr * 2.0; break;
            case POINTS_ALT: points = 100.0 + (bar_index_ % 2); offset = 100.0; break;
            case OFFSET_ALT: points = 100.0; offset = 100.0 + (bar_index_ % 2); break;
        }
        if (std::isnan(points)) return;   // ta.atr(14) warm-up: na request
        if (signal_long) {
            strategy_exit("Long Exit", "Long", kNaN, kNaN, points, offset);
        } else {
            strategy_exit("Short Exit", "Short", kNaN, kNaN, points, offset);
        }
    }
    bool flat() const { return position_side_ == PositionSide::FLAT; }

private:
    ta::ATR atr_;
};

void print_trades(const WinTheTrade& p) {
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        std::printf("      trade %d: %s entry bar %d @ %.4f qty %.4f exit bar %d @ %.4f pnl %.4f [%s|%s]\n",
                    i, t.is_long ? "long" : "short", t.entry_bar_index,
                    t.entry_price, t.qty, t.exit_bar_index, t.exit_price,
                    t.pnl, t.exit_comment.c_str(), t.exit_id.c_str());
    }
}

template <size_t N>
int bar_at(const Bar (&bars)[N], int64_t ts) {
    for (size_t i = 0; i < N; ++i) if (bars[i].timestamp == ts) return (int)i;
    return -1;
}

constexpr int64_t kNq_0401_2200 = 1743544800000LL;   // entry (A/B/C)
constexpr int64_t kNq_0402_0015 = 1743552900000LL;   // C's exit
constexpr int64_t kNq_0402_0030 = 1743553800000LL;   // A/B's exit
constexpr int64_t kNq_0402_2200 = 1743631200000LL;   // entry (E)
constexpr int64_t kNq_0403_0045 = 1743641100000LL;   // E's exit
constexpr int64_t kBtc_0401_1445 = 1743518700000LL;
constexpr int64_t kBtc_0401_1515 = 1743520500000LL;
constexpr int64_t kBtc_0509_0000 = 1746748800000LL;
constexpr int64_t kBtc_0509_0030 = 1746750600000LL;

template <size_t N>
void run_pin(const char* name, const Bar (&bars)[N], WinTheTrade& p,
             int64_t entry_ts, double entry_px, int64_t exit_ts,
             double exit_px, double pnl, const char* exit_id) {
    std::printf("%s\n", name);
    p.run(bars, (int)N);
    print_trades(p);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    CHECK(p.flat());
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long == p.signal_long);
        CHECK(t.entry_bar_index == bar_at(bars, entry_ts));
        CHECK_NEAR(t.entry_price, entry_px, 1e-9);
        CHECK(t.exit_bar_index == bar_at(bars, exit_ts));
        CHECK_NEAR(t.exit_price, exit_px, 1e-9);
        CHECK_NEAR(t.pnl, pnl, 1e-6);
        CHECK(t.exit_id == exit_id);
    }
}

WinTheTrade nq() { return WinTheTrade(0.25, 20.0, 1.0, 1.0); }
WinTheTrade btc() { return WinTheTrade(0.01, 1.0, 0.00001, 0.01); }

// famy-nq-A: the probe's shape. TV 'Short Exit' 04-02 00:30Z @19586.25,
// PnL 1330 = 66.5 x 20.
void test_nq_A_atr_restart_from_close_only() {
    WinTheTrade p = nq();
    p.signal_ts = kNq_0401_2200; p.signal_long = false; p.shape = WinTheTrade::ATR2;
    run_pin("test_nq_A_atr_restart_from_close_only", famy_data::kNq15, p,
            kNq_0401_2200, 19652.75, kNq_0402_0030, 19586.25, 1330.0, "Short Exit");
}

// famy-nq-B: trail_points alternates 100/101t per bar (a changed request
// every bar), offset fixed 100t. TV 00:30Z @19588.25 = the 00:30Z low
// 19563.25 + 100t after the restart at the 00:15Z close.
void test_nq_B_points_alternating_restart_from_close_only() {
    WinTheTrade p = nq();
    p.signal_ts = kNq_0401_2200; p.signal_long = false; p.shape = WinTheTrade::POINTS_ALT;
    run_pin("test_nq_B_points_alternating_restart_from_close_only", famy_data::kNq15, p,
            kNq_0401_2200, 19652.75, kNq_0402_0030, 19588.25, 64.5 * 20.0, "Short Exit");
}

// famy-nq-C (control): trail_offset alternates, trail_points fixed — no
// restart; the extreme keeps running from the entry and the 00:15Z bar
// fills at the 00:00Z low 19583.25 + 100t (the 00:00Z bar_index is even in
// the tape, as here: index 92).
void test_nq_C_offset_only_keeps_running_extreme() {
    WinTheTrade p = nq();
    p.signal_ts = kNq_0401_2200; p.signal_long = false; p.shape = WinTheTrade::OFFSET_ALT;
    CHECK(bar_at(famy_data::kNq15, 1743552000000LL) % 2 == 0);
    run_pin("test_nq_C_offset_only_keeps_running_extreme", famy_data::kNq15, p,
            kNq_0401_2200, 19652.75, kNq_0402_0015, 19608.25, 44.5 * 20.0, "Short Exit");
}

// famy-nq-E: the long mirror. TV 'Long Exit' 04-03 00:45Z @18994.5, PnL
// 1575 = 78.75 x 20.
void test_nq_E_long_atr_restart_from_close_only() {
    WinTheTrade p = nq();
    p.signal_ts = kNq_0402_2200; p.signal_long = true; p.shape = WinTheTrade::ATR2;
    run_pin("test_nq_E_long_atr_restart_from_close_only", famy_data::kNq15, p,
            kNq_0402_2200, 18915.75, kNq_0403_0045, 18994.5, 1575.0, "Long Exit");
}

// famy-btc-A: BINANCE:BTCUSDT 15m long. TV 15:15Z @84897.85, PnL 7.0193.
void test_btc_A_long_atr_restart_from_close_only() {
    WinTheTrade p = btc();
    p.signal_ts = kBtc_0401_1445; p.signal_long = true; p.shape = WinTheTrade::ATR2;
    run_pin("test_btc_A_long_atr_restart_from_close_only", famy_data::kBtc15_0401, p,
            kBtc_0401_1445, 84195.92, kBtc_0401_1515, 84897.85, 7.0193, "Long Exit");
}

// famy-btc-B: short. TV 00:30Z @102838.91, PnL 2.1533.
void test_btc_B_short_atr_restart_from_close_only() {
    WinTheTrade p = btc();
    p.signal_ts = kBtc_0509_0000; p.signal_long = false; p.shape = WinTheTrade::ATR2;
    run_pin("test_btc_B_short_atr_restart_from_close_only", famy_data::kBtc15_0509, p,
            kBtc_0509_0000, 103054.24, kBtc_0509_0030, 102838.91, 2.1533, "Short Exit");
}

}  // namespace

int main() {
    test_nq_A_atr_restart_from_close_only();
    test_nq_B_points_alternating_restart_from_close_only();
    test_nq_C_offset_only_keeps_running_extreme();
    test_nq_E_long_atr_restart_from_close_only();
    test_btc_A_long_atr_restart_from_close_only();
    test_btc_B_short_atr_restart_from_close_only();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
