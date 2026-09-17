// R4-D L10y: a default 100%-of-equity opening that the legacy owner (ab9714be)
// ADMITS and books as a main lot plus a small residual lot closed again on the
// entry bar must replay the same way on the switched route instead of being
// refused as unaffordable, and the split-off residual must inherit the entry
// bar's COMPLETE H/L excursion sample (pine_scheduler.cpp:257/:363 runs
// update_per_trade_extremes() over the full script bar before the non-POOC
// end-of-bar opening margin trim; only the POOC pre-script pass samples the
// traversed waypoint prefix, pine_fills.cpp:2014-2023).
//
// Shape pinned from corpus/validation/zz-pop-yukozb-gold-ny-orb-v19-close-20-00
// owner rows #2/#3: Entry long 2025-04-04 15:15 @1812.35 is booked as a
// residual q=0.00012464 closed on the SAME bar @1812.31 by the __margin_call__
// slice plus a main lot q=5.64597797 that survives to the 20:15 session close
// @1809.53.  The owner's residual excursion there is the full bar scaled by
// its quantity and shifted by the per-unit commission:
//   fav = ((1820.18 - 1812.35) - 0.0002*1812.35) * 0.00012464 = 0.000931
//   adv = -((1812.35 - 1800.63) + 0.0002*1812.35) * 0.00012464 = -0.001506
// i.e. max_runup/max_drawdown carry the un-commissioned (H-entry)/(entry-L)
// product and the report folds the fee in.
//
// Bars are embedded from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv —
// this test must never open corpus files or absolute paths (CI has no corpus
// checkout).
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

// 2025-04-04 (UTC) rows of corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv.
// Index 0 is the signal bar (15:00), index 1 the entry fill bar (15:15, the
// owner's #2/#3 bar: O 1812.33 -> fill 1812.35 with slippage 2, H 1820.18,
// L 1800.63), index 2 an ordinary bar and index 3 the session-close bar
// (20:15, open 1809.55 -> market close 1809.53 with slippage 2).
std::vector<Bar> yukozb_bars() {
    return {
        mk(1743778800000LL, 1789.47, 1814.99, 1788.34, 1812.32),  // 0: 15:00
        mk(1743779700000LL, 1812.33, 1820.18, 1800.63, 1802.28),  // 1: 15:15
        mk(1743780600000LL, 1802.27, 1806.77, 1786.40, 1791.59),  // 2: 15:30
        mk(1743797700000LL, 1809.55, 1810.48, 1806.43, 1809.49),  // 3: 20:15
    };
}

source::PineStrategyConfig all_in_cfg(double commission_pct) {
    source::PineStrategyConfig c;
    c.initial_capital = 10000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = commission_pct;
    c.slippage = 2;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

// The ORB shape: one all-in market long plus a resting bracket whose levels
// the entry bar never reaches, then an unconditional session close.
class AllInOrbHost : public source::PineStrategyHost {
public:
    explicit AllInOrbHost(double commission_pct) {
        configure_pine_strategy(all_in_cfg(commission_pct));
        set_syminfo_metadata("ETHUSDT", 0.01);
        set_syminfo_metadata("qty_step", 0.00000001);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("Long", true);
            strategy_exit("LongX", "Long", 1830.00, 1780.00, kNaN, kNaN, kNaN,
                          100.0);
        }
        if (i == 2 && live_position_size() > 0.0) strategy_close_all();
    }
};

void dump(const char* tag, const Trade& t) {
    std::printf("%s id=%s/%s %s entry=%.2f@%lld exit=%.2f@%lld qty=%.8f "
                "pnl=%.6f mfe=%.6f mae=%.6f comm=%.6f\n",
                tag, t.entry_id.c_str(), t.exit_id.c_str(),
                t.is_long ? "L" : "S", t.entry_price,
                static_cast<long long>(t.entry_time), t.exit_price,
                static_cast<long long>(t.exit_time), t.qty, t.pnl,
                t.max_runup, t.max_drawdown, t.commission);
}

// A commissioned all-in opening is admitted and split: the residual lot is
// closed again on the entry bar by the margin-call slice and the main lot
// survives to the session close.  The residual inherits the FULL entry bar.
void test_all_in_entry_splits_and_residual_sees_full_bar() {
    AllInOrbHost host(0.02);
    const auto bars = yukozb_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), 0.0));
    // The owner books two lots here; a refused all-in opening books none.
    CHECK(host.trade_count() == 2);
    if (host.trade_count() != 2) {
        for (int i = 0; i < host.trade_count(); ++i)
            dump("  got", host.get_trade(i));
        return;
    }
    const Trade& residual = host.get_trade(0);
    const Trade& main_lot = host.get_trade(1);
    dump("residual", residual);
    dump("main", main_lot);

    // --- the residual lot: the entry-bar margin-call slice -----------------
    CHECK(residual.is_long);
    CHECK(residual.entry_id == "Long");
    CHECK(residual.exit_id == "__margin_call__");
    CHECK(residual.exit_comment == "Margin call");
    CHECK(!residual.exit_from_bracket);
    CHECK(near(residual.entry_price, 1812.35, 1e-9));   // open + 2 ticks
    CHECK(near(residual.exit_price, 1812.31, 1e-9));     // open - 2 ticks
    CHECK(residual.entry_time == bars[1].timestamp);
    CHECK(residual.exit_time == bars[1].timestamp);      // closed on the entry bar
    CHECK(residual.qty > 0.0);
    CHECK(residual.qty < main_lot.qty * 0.01);           // a dust residual

    // --- the main lot survives to the session close ------------------------
    CHECK(main_lot.is_long);
    CHECK(main_lot.entry_id == "Long");
    CHECK(near(main_lot.entry_price, 1812.35, 1e-9));
    CHECK(near(main_lot.exit_price, 1809.53, 1e-9));     // 20:15 open - 2 ticks
    CHECK(main_lot.entry_time == bars[1].timestamp);
    CHECK(main_lot.exit_time == bars[3].timestamp);
    CHECK(main_lot.qty > residual.qty);

    // --- the L10y excursion rule: the FULL entry bar, not the open prefix --
    // owner model: (H - entry) * qty and (entry - L) * qty, sampled before the
    // non-POOC end-of-bar opening trim.  The reverted submit-time prefix
    // sample printed max_runup == 0 here.
    // The owner's report folds the per-side commission (0.02% of notional)
    // into the excursion, so the pinned products carry the same shift:
    //   fav = ((H - entry) - 0.0002*entry) * qty, adv = ((entry - L) + 0.0002*entry) * qty
    // (owner row #2: ((1820.18-1812.35)-0.0002*1812.35)*0.00012464 = 0.000931).
    const double fee_px = 0.0002 * 1812.35;
    const double full_bar_runup = (1820.18 - 1812.35 - fee_px) * residual.qty;
    const double full_bar_drawdown = (1812.35 - 1800.63 + fee_px) * residual.qty;
    CHECK(near(residual.max_runup, full_bar_runup, 1e-9));
    CHECK(near(residual.max_drawdown, full_bar_drawdown, 1e-9));
    CHECK(residual.max_runup > 0.0);
    // The main lot is sampled by the ordinary per-bar walk over the same bar.
    CHECK(near(main_lot.max_runup, (1820.18 - 1812.35 - fee_px) * main_lot.qty, 1e-6));
    CHECK(near(main_lot.max_drawdown, (1812.35 - 1786.40 + fee_px) * main_lot.qty, 1e-6));
}

}  // namespace

int main() {
    test_all_in_entry_splits_and_residual_sees_full_bar();
    std::printf("test_l10y_full_equity_entry_split: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
