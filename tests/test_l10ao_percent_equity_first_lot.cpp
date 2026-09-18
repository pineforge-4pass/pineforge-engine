// R4-D L10ao: a percent-of-equity entry under 1x margin
// (margin_long = margin_short = 100) books the same lots as the legacy owner
// (ab9714be) — including the owner's one-step residual first lot (qty 0.01 on a
// 0.01-step lane) and that lot's own exit.
//
// Pinned rows: corpus/validation/zz-pop-vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2
// on OANDA:EURUSD (qty_step 0.01, mintick 1e-05, pointvalue 1.0, margin 100/100):
//   #1: Entry short 2025-04-07 08:30 @1.097070, Exit short 2025-04-07 08:45 @1.098680 q=0.01 (TSL)
//   #2: Entry short 2025-04-07 08:30 @1.097070, Exit short 2025-04-07 11:15 @1.094210 q=36446.5 (TP1 40%)
//
// Bars are embedded literals copied out of the lane feed; this test must never
// open corpus files or absolute paths at runtime.
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

// 2025-04-07 (UTC) rows of ohlcv_OANDA-EURUSD_15m.csv.
// Index 0 is the signal bar (08:15), index 1 the entry fill bar (08:30),
// index 2 the TSL fill bar (08:45), index 12 the TP1 fill bar (11:15).
std::vector<Bar> vimalboiling_eurusd_bars() {
    return {
        mk(1744013700000LL, 1.10056, 1.10103, 1.09706, 1.09706),  // 0: 08:15 (signal)
        mk(1744014600000LL, 1.09707, 1.09895, 1.09680, 1.09868),  // 1: 08:30 (entry fill)
        mk(1744015500000LL, 1.09868, 1.10087, 1.09864, 1.09920),  // 2: 08:45 (TSL fill)
        mk(1744016400000LL, 1.09918, 1.09965, 1.09720, 1.09730),  // 3: 09:00
        mk(1744017300000LL, 1.09730, 1.09789, 1.09578, 1.09596),  // 4: 09:15
        mk(1744018200000LL, 1.09596, 1.09684, 1.09500, 1.09548),  // 5: 09:30
        mk(1744019100000LL, 1.09548, 1.09660, 1.09501, 1.09628),  // 6: 09:45
        mk(1744020000000LL, 1.09628, 1.09734, 1.09627, 1.09685),  // 7: 10:00
        mk(1744020900000LL, 1.09684, 1.09854, 1.09676, 1.09853),  // 8: 10:15
        mk(1744021800000LL, 1.09852, 1.09908, 1.09714, 1.09719),  // 9: 10:30
        mk(1744022700000LL, 1.09718, 1.09722, 1.09578, 1.09588),  // 10: 10:45
        mk(1744023600000LL, 1.09590, 1.09656, 1.09498, 1.09514),  // 11: 11:00
        mk(1744024500000LL, 1.09512, 1.09556, 1.09351, 1.09442),  // 12: 11:15 (TP1 fill)
    };
}

source::PineStrategyConfig vimalboiling_cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 10.0;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = 0.04;
    c.close_entries_rule_any = true;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

class VimalboilingEurUsdHost : public source::PineStrategyHost {
public:
    VimalboilingEurUsdHost() {
        configure_pine_strategy(vimalboiling_cfg());
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 1e-05;
        syminfo_mintick_ = 1e-05;
        syminfo_.ticker = "OANDA:EURUSD";
        syminfo_.tickerid = "OANDA:EURUSD";
        syminfo_.type = "forex";
        syminfo_.timezone = "America/New_York";
        syminfo_.session = "1700-1700";
        set_syminfo_metadata("qty_step", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("Short", false);
        }
        if (live_position_size() < 0.0) {
            strategy_exit("TP1 Short", "Short", 1.09421, kNaN, kNaN, kNaN, kNaN, 40.0, "TP1 40%");
            strategy_exit("TP2 Short", "Short", 1.09087, kNaN, kNaN, kNaN, kNaN, 30.0, "TP2 30%");
            strategy_exit("TP3 Short", "Short", 1.09068, kNaN, kNaN, kNaN, kNaN, 30.0, "TP3 30%");
            strategy_exit("TSL Short", "Short", kNaN, 0.0, kNaN, kNaN, kNaN, 100.0, "TSL");
        }
    }
};

void test_vimalboiling_first_lot_exit() {
    VimalboilingEurUsdHost host;
    const auto bars = vimalboiling_eurusd_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    if (host.trade_count() < 2) {
        std::printf("got trade_count=%d (expected at least 2)\n", host.trade_count());
        return;
    }

    // Trade 1: TSL Short residual lot (0.01 units) that exits on bar 2 (08:45)
    const Trade& t1 = host.get_trade(0);
    CHECK(!t1.is_long);
    CHECK(t1.entry_id == "Short");
    CHECK(t1.exit_id == "TSL Short");
    CHECK(t1.exit_comment == "TSL");
    CHECK(near(t1.entry_price, 1.09707, 1e-6));
    CHECK(near(t1.exit_price, 1.09868, 1e-6));
    CHECK(near(t1.qty, 0.01, 1e-6));
    CHECK(near(t1.pnl, -0.000025, 1e-6));
    CHECK(t1.entry_time == bars[1].timestamp);
    CHECK(t1.exit_time == bars[2].timestamp);

    // Trade 2: TP1 Short sized lot (36446.5 units) that exits on bar 12 (11:15)
    const Trade& t2 = host.get_trade(1);
    CHECK(!t2.is_long);
    CHECK(t2.entry_id == "Short");
    CHECK(t2.exit_id == "TP1 Short");
    CHECK(t2.exit_comment == "TP1 40%");
    CHECK(near(t2.entry_price, 1.09707, 1e-6));
    CHECK(near(t2.exit_price, 1.09421, 1e-6));
    CHECK(near(t2.qty, 36446.5, 1e-4));
    CHECK(near(t2.pnl, 72.291195, 1e-4));
    CHECK(t2.entry_time == bars[1].timestamp);
    CHECK(t2.exit_time == bars[12].timestamp);
}

}  // namespace

int main() {
    test_vimalboiling_first_lot_exit();
    std::printf("test_l10ao_percent_equity_first_lot: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
