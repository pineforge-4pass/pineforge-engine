// R4-D L10k: dual-stop both-touch fills the older book stop first
// (order-dual-stop-both-touch-priority-01 #34/#35), and
// order-stop-entry-reversal-grouping-01 replays identical to ab9714be
// (the host owned excursion accounting, so the half-tick residual was gone;
// since R5 lane H-THIN the kernel samples the Pine host's lots and it is
// back, see #801 below).
// Bars are embedded from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv —
// this test must never open corpus files (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
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

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

source::PineStrategyConfig cfg(int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyr;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

void expect_trade(const char* tag, const Trade& t, bool is_long,
                  double entry_px, double exit_px, double fav, double adv,
                  double pnl) {
    std::printf("%s %s @%.4f->%.4f pnl=%.4f mfe=%.4f mae=%.4f "
                "(want @%.4f->%.4f pnl=%.4f mfe=%.4f mae=%.4f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price, t.pnl,
                t.max_runup, t.max_drawdown, entry_px, exit_px, pnl, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px));
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.pnl, pnl));
    CHECK(near(t.max_runup, fav) /* exact: host owns excursion, L11a */);
    CHECK(near(t.max_drawdown, adv));
}

// Probe 80: leftover morning long stop 1600.84 plus afternoon LE2 1596.36 /
// SE2 1583.64. The 15:15 bar is low-first and touches both longs; the owner
// fills the older book stop first (pine_fills.cpp pending-order scan), then
// pyramids the nearer stop. SE2 reduces FIFO lot 1 at 16:15; evening
// close_all exits lot 2 at 18:30 open.
class DualStopPriority : public source::PineStrategyHost {
public:
    DualStopPriority() {
        configure_pine_strategy(cfg(1));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0)
            strategy_entry("LE", true, kNaN, 1600.84, 1.0, "leftover morning long");
        if (i == 1 && live_position_size() == 0.0) {
            strategy_entry("LE2", true, kNaN, 1596.36, 1.0, "afternoon long");
            strategy_entry("SE2", false, kNaN, 1583.64, 1.0, "afternoon short");
        }
        if (i == 15 && live_position_size() != 0.0)
            strategy_close_all();
    }
};

std::vector<Bar> dual_stop_bars() {
    return {
        mk(1744813800000LL, 1583.69, 1596.6, 1583.63, 1594.22),
        mk(1744814700000LL, 1594.22, 1598.88, 1586.6, 1590.0),
        mk(1744815600000LL, 1590.0, 1593.2, 1584.12, 1589.97),
        mk(1744816500000LL, 1589.97, 1614.95, 1588.16, 1604.46),
        mk(1744817400000LL, 1604.46, 1606.42, 1590.0, 1592.61),
        mk(1744818300000LL, 1592.62, 1594.6, 1588.05, 1589.91),
        mk(1744819200000LL, 1589.91, 1596.33, 1588.76, 1592.08),
        mk(1744820100000LL, 1592.09, 1593.45, 1580.26, 1584.09),
        mk(1744821000000LL, 1584.1, 1586.85, 1575.25, 1581.5),
        mk(1744821900000LL, 1581.49, 1594.78, 1579.39, 1591.69),
        mk(1744822800000LL, 1591.69, 1597.7, 1590.2, 1594.61),
        mk(1744823700000LL, 1594.61, 1612.36, 1594.5, 1603.76),
        mk(1744824600000LL, 1603.77, 1608.0, 1575.73, 1583.44),
        mk(1744825500000LL, 1583.45, 1583.46, 1537.56, 1545.02),
        mk(1744826400000LL, 1545.03, 1568.75, 1542.43, 1567.8),
        mk(1744827300000LL, 1567.8, 1578.72, 1555.36, 1576.62),
        mk(1744828200000LL, 1576.62, 1579.69, 1556.83, 1559.6),
    };
}

class StopReversal : public source::PineStrategyHost {
public:
    StopReversal() {
        configure_pine_strategy(cfg(2));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 1 && live_position_size() == 0.0)
            strategy_entry("L1", true, kNaN, kNaN, 1.0, "long lot 1");
        if (i == 2 && live_position_size() > 0.0)
            strategy_entry("L2", true, kNaN, kNaN, 1.0, "long lot 2");
        if (i == 3 && live_position_size() > 0.0)
            strategy_entry("SREV", false, kNaN, bar.high, 1.0, "short stop reversal");
        if (i == 5 && live_position_size() < 0.0)
            strategy_entry("LREV", true, kNaN, bar.low, 1.0, "long stop reversal");
        if (i == 7 && live_position_size() != 0.0)
            strategy_close_all();
    }
};

}  // namespace

int main() {
    {
        DualStopPriority host;
        auto bars = dual_stop_bars();
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 2);
        CHECK(near(host.live_position_size(), 0.0));
        if (host.trade_count() >= 1)
            expect_trade("dual-stop#34", host.get_trade(0), true,
                         1600.84, 1583.64, 14.11, 17.20, -17.20);
        if (host.trade_count() >= 2)
            // expectation corrected (R5 lane H-THIN, E19: the kernel samples
            // the Pine host's lots; the host-owned model is gone): favorable
            // 18.59 -> 16.00. The engine books LE2's 1596.36 fill after LE's
            // and after the entry bar's 1614.95 high (its fill order on this
            // bar, unchanged by E19), so the lot's best price is the 17:15
            // high 1612.36. TradingView fills LE2 first and closes it with
            // another exit (tape trade #34, 18.59 to 16:15), so no tape row
            // grades this one.
            expect_trade("dual-stop#35", host.get_trade(1), true,
                         1596.36, 1576.62, 16.00, 58.80, -19.74);
    }
    {
        StopReversal host;
        const std::vector<Bar> bars = {
            mk(1760659200000LL, 3892.02, 3903.63, 3886.15, 3901.02),
            mk(1760660100000LL, 3901.01, 3913.4, 3896.69, 3912.13),
            mk(1760661000000LL, 3912.15, 3924.0, 3906.38, 3918.01),
            mk(1760661900000LL, 3918.01, 3925.79, 3912.0, 3925.79),
            mk(1760662800000LL, 3925.8, 3948.06, 3920.1, 3933.02),
            mk(1760663700000LL, 3933.02, 3940.74, 3924.57, 3928.91),
            mk(1760664600000LL, 3928.92, 3932.0, 3907.93, 3917.78),
            mk(1760665500000LL, 3917.79, 3921.7, 3904.0, 3918.53),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        bool found = false;
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& t = host.get_trade(i);
            if (t.is_long && near(t.entry_price, 3912.15) && near(t.exit_price, 3925.79)) {
                found = true;
                // expectation corrected (R5 lane H-THIN, E19: the kernel
                // samples the Pine host's lots): favorable 13.64 -> 13.645,
                // the kernel's sample at the stop's half-tick crossing
                // (3925.795) under this run's per-kind tick rules. TradingView
                // books 13.65 (corpus tape trade #801: it counts the exit
                // bar's 3925.80 open print, which neither model samples), so
                // the row moves 0.005 closer to the tape and is not its number.
                expect_trade("reversal#801", t, true, 3912.15, 3925.79, 13.645, 5.77, 13.64);
                break;
            }
        }
        CHECK(found);
    }
    std::printf("test_l10k_corpus_parity: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
