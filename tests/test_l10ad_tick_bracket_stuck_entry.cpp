// R4-D L10ad: an explicit-qty entry stays ADMISSIBLE after the account has
// traded below zero equity when the strategy declared margin_long=0 /
// margin_short=0.  ab9714be gates its whole placement-affordability half on
// margin_pct > 0.0 (src/source/pine_strategy_commands.cpp:344-346, "margin_pct
// == 0 disables the check, as it does in TradingView").  The switched route's
// coarse explicit-qty admission boundary instead evaluated
// `required > equity` with required == 0, so once equity went negative EVERY
// later entry was dropped and the strategy went silent for the rest of the
// tape (NQ1 ORB probe: owner 201 trades, this tree 73).
//
// Rows pinned from the legacy owner on data-CME_MINI-NQ1 /
// zz-pop-santiagomunoz00-mes-orb-v6-santi (mintick 0.25, pointvalue 20,
// strategy.fixed 18, pyramiding=0, slippage 1, commission 0.85 cash per
// contract, margin_long=margin_short=0):
//   #73 Entry long 2025-08-21 14:15 @23281.75 q=18 / Exit long 14:15 @23272.50
//       Net PnL -3360.60  (tick bracket loss=36 stops out on the ENTRY bar and
//                          takes the account below zero equity)
//   #74 Entry long 2025-08-22 14:15 @23587.00 q=18 / Exit long 14:15 @23590.25
//       Net PnL 1139.40   (next day's ORB long, tick bracket profit=13)
// The same shape must replay with a 3,000 account so that trade #73's twin
// leaves equity at -360.60 and the #74 twin is still admitted.
//
// Bars are embedded literals copied from the probe's lane feed rows
// (CME_MINI:NQ1! 15m, 2025-08-21/22 UTC); this test must never open corpus
// files or absolute paths at runtime (CI has no corpus or lane checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

// 2025-08-21 and 2025-08-22 15m rows of the NQ1 lane feed (UTC).  Index 0 / 2
// are the ORB signal bars (the script places its orders on their close); index
// 1 / 3 are the fill bars: O 23281.50 -> fill 23281.75 with slippage 1 and
// L 23272.50 = the 36-tick stop slipped one tick, then O 23586.75 -> fill
// 23587.00 and H 23633.00 above the 13-tick target 23590.25.
std::vector<Bar> nq_orb_bars() {
    return {
        mk(1755784800000LL, 23184.50, 23303.25, 23156.00, 23281.50),  // 0: 08-21 14:00
        mk(1755785700000LL, 23281.50, 23348.25, 23272.50, 23344.75),  // 1: 08-21 14:15
        mk(1755871200000LL, 23268.75, 23596.50, 23268.75, 23587.00),  // 2: 08-22 14:00
        mk(1755872100000LL, 23586.75, 23633.00, 23492.00, 23553.75),  // 3: 08-22 14:15
    };
}

source::PineStrategyConfig orb_cfg(double initial_capital, double margin_pct) {
    source::PineStrategyConfig c;
    c.initial_capital = initial_capital;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 18.0;
    c.pyramiding = 0;
    c.process_orders_on_close = false;
    c.calc_on_order_fills = false;
    c.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
    c.commission_value = 0.85;
    c.slippage = 1;
    c.margin_long = margin_pct;
    c.margin_short = margin_pct;
    return c;
}

// The ORB script shape: strategy.entry("LONG", qty=18) plus the same-bar
// strategy.exit("EXIT LONG", "LONG", loss=36, profit=13) tick bracket, issued
// again with the SAME ids on the next signal bar (L10n re-issue).
class NqOrbHost : public source::PineStrategyHost {
public:
    NqOrbHost(double initial_capital, double margin_pct) {
        configure_pine_strategy(orb_cfg(initial_capital, margin_pct));
        set_syminfo_mintick(0.25);
        set_syminfo_pointvalue(20.0);
    }
    double equity() const { return current_equity(); }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if ((i == 0 || i == 2) && live_position_size() == 0.0) {
            strategy_entry("LONG", true, kNaN, kNaN, 18.0, "", "", 0, -1);
            strategy_exit("EXIT LONG", "LONG", kNaN, kNaN, kNaN, kNaN, kNaN,
                          100.0, "", kNaN, "", 13.0, 36.0);
        }
    }
};

void dump(const char* tag, const Trade& t) {
    std::printf("%s id=%s/%s %s entry=%.2f@%lld exit=%.2f@%lld qty=%.2f "
                "pnl=%.6f comm=%.6f\n",
                tag, t.entry_id.c_str(), t.exit_id.c_str(),
                t.is_long ? "L" : "S", t.entry_price,
                static_cast<long long>(t.entry_time), t.exit_price,
                static_cast<long long>(t.exit_time), t.qty, t.pnl,
                t.commission);
}

void expect_orb_trade(const char* tag, const Trade& t,
                      std::int64_t bar_ms, double entry_px, double exit_px,
                      double pnl) {
    dump(tag, t);
    CHECK(t.is_long);
    CHECK(t.entry_id == "LONG");
    CHECK(t.exit_id == "EXIT LONG");
    CHECK(near(t.qty, 18.0));
    CHECK(t.entry_time == bar_ms);
    CHECK(t.exit_time == bar_ms);          // entry and bracket exit on ONE bar
    CHECK(near(t.entry_price, entry_px));  // open + 1 slip tick
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.pnl, pnl));               // (exit-entry)*18*20 - 2*0.85*18
}

// Owner rows #73/#74: the same-bar stop-out drives equity to -360.60 on a
// 3,000 account and the NEXT day's entry is still admitted because a zero
// direction margin disables the placement affordability check.
void test_zero_margin_keeps_trading_below_zero_equity() {
    NqOrbHost host(3000.0, 0.0);
    const auto bars = nq_orb_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(near(host.equity(), 3000.0 - 3360.60 + 1139.40, 1e-6));
    CHECK(host.trade_count() == 2);
    if (host.trade_count() != 2) {
        for (int i = 0; i < host.trade_count(); ++i) dump("  got", host.get_trade(i));
        return;
    }
    // The account is below zero between the two trades: that is the state the
    // reverted admission boundary read as "unaffordable" (required 0 > equity).
    expect_orb_trade("#73 stop-out", host.get_trade(0), bars[1].timestamp,
                     23281.75, 23272.50, -3360.60);
    expect_orb_trade("#74 next day", host.get_trade(1), bars[3].timestamp,
                     23587.00, 23590.25, 1139.40);
}

// Control: with a POSITIVE direction margin the same placement check is still
// live, so the unaffordable 18-lot NQ opening (18 * 23281.50 * 20 = 8.38M
// against 3,000 of equity) is dropped on both signal bars.  The L10ad gate is
// margin_pct > 0, not a removal of the affordability rule.
void test_positive_margin_still_declines() {
    NqOrbHost host(3000.0, 100.0);
    const auto bars = nq_orb_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(host.trade_count() == 0);
    for (int i = 0; i < host.trade_count(); ++i) dump("  unexpected", host.get_trade(i));
}

}  // namespace

int main() {
    test_zero_margin_keeps_trading_below_zero_equity();
    test_positive_margin_still_declines();
    std::printf("test_l10ad_tick_bracket_stuck_entry: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
