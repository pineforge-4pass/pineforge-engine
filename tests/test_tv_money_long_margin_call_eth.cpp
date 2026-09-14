/*
 * test_tv_money_long_margin_call_eth.cpp — round 10 family AB: TradingView's
 * 10-significant-digit margin call on an EXPLICIT-qty margin-100 long, on the
 * BINANCE:ETHUSDT.P@15 hard lane (the corpus probe
 * anomaly-equity-mirror-strategy-equity-01; campaign notes
 * log-20260905t213120z-d5f9e282 and the family AB pin note).
 *
 * The broker marks the position's required margin on money rounded to ten
 * significant digits (3 decimals at >= 1e6) while the equity is exact. A 1x
 * long whose free cash after the fill is smaller than the value's rounding
 * residual at some bar path point (the post-fill points of the fill bar, then
 * every bar's open / extremes in leg order / close) is liquidated one whole
 * contract there, tagged "Margin call", at that raw path price.
 *
 * Fixtures are the registry's ETHUSDT.P 15-minute bars (lab bars, feed
 * 27b62431096e) around three Monday 00:00Z signals; the quantities are the
 * probe's round3(equity / close); the capitals are lab tv capital sweeps
 * (scratchpad/r10/famAB/pins in pineforge-workflow, tapes famab-0421-c*,
 * famab-0721-c*, famab-1124-c*; "c00020" = cost + 0.0002 of free cash):
 *
 *   04-21  Q 623.163 @1592.52 (00:15Z open == 00:00Z close). Residuals along
 *          the path: 00:15Z low 1592.52 +0.00004, high 1613.8 -0.0004, close
 *          -0.00048; 00:30Z open -0.00048, low 1606.17 +0.00029, high -0.00018,
 *          close -0.00014. TV: cash 0.00013 (the probe's own ledger, E
 *          992399.54089) / 0.0001 / 0.0002 -> 'Margin call' 1 @1606.17 on the
 *          00:30Z bar, then 622.163 flattened @1613.78 (the 00:45Z open);
 *          cash 0.0003 / 0.0004 / 0.0005 / 0.001 -> one 623.163 trade, no call.
 *   07-21  Q 270.621 @3731.72. Residuals: fill bar high 3734.89 +0.00031, low
 *          3709.27 +0.00033, close +0.00025; next bar open +0.00025, high
 *          -0.00049, low 0, close +0.00016. TV: cash 0.0004 / 0.001 -> one
 *          270.621 trade, no call. (cash 0.0001 / 0.0003: TV does not fill the
 *          entry at all — family R's rounded-cost admission on a decimal tie,
 *          round8/famR-eurusd, NOT part of this change; not asserted here.)
 *   11-24  Q 356.701 @2778.39. Residuals: fill bar low 2761.38 +0.00002, high
 *          2786.26 +0.00004, close -0.00003; next bar open -0.00004, low
 *          -0.00002, high +0.00005, close -0.00002. TV: cash 0.00003 ->
 *          'Margin call' 1 @2786.26 on the FILL bar (its high, after the low
 *          passed with a residual under the cash), then 355.701 @2788.12; cash
 *          0.0001 / 0.0003 / 0.001 -> one 356.701 trade. (cash 0.00001: TV drops
 *          the entry — E 991054.4913999999 < the rounded cost 991054.4914,
 *          family R's rule 2; not asserted here.)
 *
 * The probe's whole tape (24 TV rows) reproduces row for row once this event
 * is in the ledger: the 7.61 USDT the engine used to book on 04-21 was the
 * seed of every later quantity's divergence (25 vs 24 rows, weak 65.2 %).
 *
 * Scope controls: the trigger is scoped by tv_money_scope (a lot-stepped
 * instrument whose lot is worth under one account unit; ETHUSDT.P at qty
 * step 0.0001), so qty_step 0 keeps the exact arithmetic; the emulator
 * switch turns it off.
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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

namespace {

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

// bar 0 = the 23:45Z bar, bar 1 = the Monday 00:00Z signal bar, bar 2 = the
// 00:15Z fill bar, bar 3 = 00:30Z, bar 4 = 00:45Z (the flatten fills at its
// open), bar 5 = 01:00Z.
static std::vector<Bar> bars_0421() {
    return {
        mk_bar(1745192700000LL, 1583.8, 1587, 1583.49, 1586.56),
        mk_bar(1745193600000LL, 1586.57, 1593.75, 1585.28, 1592.52),
        mk_bar(1745194500000LL, 1592.52, 1613.8, 1592.52, 1608.96),
        mk_bar(1745195400000LL, 1608.96, 1619.86, 1606.17, 1613.78),
        mk_bar(1745196300000LL, 1613.78, 1620, 1608.08, 1609.49),
        mk_bar(1745197200000LL, 1609.5, 1618, 1607.26, 1610.81),
    };
}
static std::vector<Bar> bars_0721() {
    return {
        mk_bar(1753055100000LL, 3757.89, 3765.37, 3752.07, 3755.67),
        mk_bar(1753056000000LL, 3755.68, 3756.49, 3730.32, 3731.72),
        mk_bar(1753056900000LL, 3731.72, 3734.89, 3709.27, 3728.75),
        mk_bar(1753057800000LL, 3728.75, 3734.69, 3712, 3728.04),
        mk_bar(1753058700000LL, 3728.03, 3746.86, 3721.01, 3746.13),
        mk_bar(1753059600000LL, 3746.13, 3753.24, 3736.3, 3738.05),
    };
}
static std::vector<Bar> bars_1124() {
    return {
        mk_bar(1763941500000LL, 2801.8, 2807.06, 2797.08, 2800.73),
        mk_bar(1763942400000LL, 2800.74, 2800.74, 2775.68, 2778.39),
        mk_bar(1763943300000LL, 2778.39, 2786.26, 2761.38, 2783.63),
        mk_bar(1763944200000LL, 2783.64, 2794.65, 2773.52, 2788.12),
        mk_bar(1763945100000LL, 2788.12, 2802.08, 2780.02, 2786.69),
        mk_bar(1763946000000LL, 2786.68, 2797.88, 2778.1, 2787.99),
    };
}

// The probe's shape: an explicit-qty long at the 00:00Z bar's close (fills
// at the next open), strategy.close two bars later; commission 0, slippage 0,
// margin 100/100, ETHUSDT.P lane facts (mintick 0.01, qty step 0.0001).
class MirrorProbe : public pineforge::source::PineStrategyHost {
public:
    MirrorProbe(double capital, double qty, double qty_step = 0.0001,
                bool emulator = true)
        : qty_(qty) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 0;
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.01;
        if (!emulator) set_margin_call_enabled(false);
    }

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 1 && signed_position_size() == 0.0) {
            strategy_entry("E", true,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), qty_,
                           "qty = equity/close");
        }
        if (signed_position_size() > 0.0 && bar_index_ > entry_bar_) {
            if (entry_bar_ < 0) entry_bar_ = bar_index_;
            if (bar_index_ > entry_bar_) strategy_close("E", "next-bar flatten");
        }
    }

    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    double trade_pnl(int i) const { return closed_trade_profit(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }

private:
    double qty_;
    int entry_bar_ = -1;
};

static int margin_call_rows(const MirrorProbe& eng) {
    int n = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) == std::string("Margin call")) ++n;
    }
    return n;
}

struct Expect {
    double cash;          // capital - qty * fill
    bool call;            // TV books a 'Margin call' 1 row
    int call_bar;         // its exit bar index
    double call_price;    // its raw path price
};

static void run_sweep(const char* name, const std::vector<Bar>& bars,
                      double qty, double fill, double flatten,
                      const std::vector<Expect>& cases) {
    for (const Expect& e : cases) {
        const double capital = qty * fill + e.cash;
        MirrorProbe eng(capital, qty);
        eng.run(bars.data(), (int)bars.size());
        std::printf("  %s cash %.5f: %d trade(s), %d margin-call row(s)\n",
                    name, e.cash, eng.trade_count(), margin_call_rows(eng));
        if (e.call) {
            CHECK(eng.trade_count() == 2);
            if (eng.trade_count() != 2) continue;
            CHECK(eng.exit_comment(0) == std::string("Margin call"));
            CHECK(near(eng.trade_size(0), 1.0));
            CHECK(near(eng.entry_price(0), fill));
            CHECK(near(eng.exit_price(0), e.call_price));
            CHECK(eng.exit_bar(0) == e.call_bar);
            CHECK(near(eng.trade_pnl(0), e.call_price - fill, 1e-6));
            CHECK(eng.exit_comment(1) == std::string("next-bar flatten"));
            CHECK(near(eng.trade_size(1), qty - 1.0));
            CHECK(near(eng.exit_price(1), flatten));
            CHECK(eng.exit_bar(1) == 4);
        } else {
            CHECK(eng.trade_count() == 1);
            CHECK(margin_call_rows(eng) == 0);
            if (eng.trade_count() != 1) continue;
            CHECK(near(eng.trade_size(0), qty));
            CHECK(near(eng.entry_price(0), fill));
            CHECK(near(eng.exit_price(0), flatten));
            CHECK(eng.exit_bar(0) == 4);
        }
        CHECK(near(eng.position_size(), 0.0));
    }
}

}  // namespace

// The probe's own 2025-04-21 ledger: E 992399.54089 (1e6 - 14680.57446 +
// 7080.11535), Q 623.163, fill 1592.52 -> cash 0.00013. TV rows 3 and 4 of
// the corpus tape: 'Margin call' 1 @1606.17 (00:30Z), 622.163 @1613.78.
static void test_probe_ledger_0421() {
    std::printf("probe ledger 04-21\n");
    MirrorProbe eng(992399.54089, 623.163);
    auto bars = bars_0421();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 2);
    if (eng.trade_count() == 2) {
        CHECK(eng.exit_comment(0) == std::string("Margin call"));
        CHECK(near(eng.trade_size(0), 1.0));
        CHECK(near(eng.exit_price(0), 1606.17));
        CHECK(eng.exit_bar(0) == 3);
        CHECK(near(eng.trade_pnl(0), 13.65, 1e-6));
        CHECK(near(eng.trade_size(1), 622.163));
        CHECK(near(eng.exit_price(1), 1613.78));
        CHECK(near(eng.trade_pnl(1), 13227.18538, 1e-6));
        CHECK(eng.exit_bar(1) == 4);
    }
}

static void test_sweep_0421() {
    std::printf("capital sweep 04-21 (Q 623.163 @1592.52)\n");
    run_sweep("0421", bars_0421(), 623.163, 1592.52, 1613.78, {
        {0.0001, true, 3, 1606.17},
        {0.0002, true, 3, 1606.17},
        {0.0003, false, 0, 0.0},
        {0.0004, false, 0, 0.0},
        {0.0005, false, 0, 0.0},
        {0.0010, false, 0, 0.0},
    });
}

static void test_sweep_0721() {
    std::printf("capital sweep 07-21 (Q 270.621 @3731.72)\n");
    run_sweep("0721", bars_0721(), 270.621, 3731.72, 3728.03, {
        {0.0004, false, 0, 0.0},
        {0.0010, false, 0, 0.0},
    });
}

static void test_sweep_1124() {
    std::printf("capital sweep 11-24 (Q 356.701 @2778.39)\n");
    run_sweep("1124", bars_1124(), 356.701, 2778.39, 2788.12, {
        {0.00003, true, 2, 2786.26},
        {0.00010, false, 0, 0.0},
        {0.00030, false, 0, 0.0},
        {0.00100, false, 0, 0.0},
    });
}

// Scope: qty_step 0 (the corpus' continuous default when no lane override
// names the step) stays on exact arithmetic; the emulator switch is honoured.
static void test_scope_controls() {
    std::printf("scope controls\n");
    {
        MirrorProbe eng(992399.54089, 623.163, /*qty_step=*/0.0);
        auto bars = bars_0421();
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trade_count() == 1);
        CHECK(margin_call_rows(eng) == 0);
    }
    {
        MirrorProbe eng(992399.54089, 623.163, 0.0001, /*emulator=*/false);
        auto bars = bars_0421();
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trade_count() == 1);
        CHECK(margin_call_rows(eng) == 0);
    }
}

int main() {
    test_probe_ledger_0421();
    test_sweep_0421();
    test_sweep_0721();
    test_sweep_1124();
    test_scope_controls();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
