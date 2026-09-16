// R4-D L10j: trailing exits and same-bar stop-limit-trail bracket parity.
// Pins the earliest divergent trades with legacy owner literals by replaying exact bars.
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do {                                                           \
    if (x) {                                                                   \
        ++passed;                                                              \
    } else {                                                                   \
        ++failed;                                                              \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);               \
    }                                                                          \
} while (0)

bool near(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) < tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

void expect_trade(const char* tag, const Trade& t, bool is_long,
                  double entry_px, double exit_px, double pnl, double fav, double adv) {
    std::printf("%s %s @%.4f->%.4f pnl=%.6f mfe=%.6f mae=%.6f\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price,
                t.pnl, t.max_runup, t.max_drawdown);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px));
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.pnl, pnl));
    CHECK(near(t.max_runup, fav));
    CHECK(near(t.max_drawdown, adv));
}

// ---------------------------------------------------------------------------
// Scenario 1: bracket-exit-stop-limit-trail-same-bar-01
// Trade #2: Entry short 2025-03-31 16:30 @ 1845.31 -> Exit short 16:45 @ 1843.12
// ---------------------------------------------------------------------------
class TripleExitShortT2Host : public source::PineStrategyHost {
public:
    TripleExitShortT2Host() {
        configure_pine_strategy(cfg());
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S", false, kNaN, kNaN, 1.0, "probe short");
        } else if (i == 1) {
            const double atr = 12.341403481243358;
            const double shortStop = position_avg_price() + atr * 0.8;
            const double shortLimit = position_avg_price() - atr * 1.6;
            strategy_exit("SX", "S", shortLimit, shortStop, atr, kNaN, kNaN, 100.0, "triple exit short");
        }
    }
};

// ---------------------------------------------------------------------------
// Scenario 1 (continued): Trade #466
// Entry short 2025-11-18 16:30 @ 3155.66 -> Exit short 16:45 @ 3148.74
// ---------------------------------------------------------------------------
class TripleExitShortT466Host : public source::PineStrategyHost {
public:
    TripleExitShortT466Host() {
        configure_pine_strategy(cfg());
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S", false, kNaN, kNaN, 1.0, "probe short");
        } else if (i == 1) {
            const double atr = 28.273395025503916;
            const double shortStop = position_avg_price() + atr * 0.8;
            const double shortLimit = position_avg_price() - atr * 1.6;
            strategy_exit("SX", "S", shortLimit, shortStop, atr, kNaN, kNaN, 100.0, "triple exit short");
        }
    }
};

// ---------------------------------------------------------------------------
// Scenario 2: bracket-trail-points-with-offset-only-01
// Trade #20: Entry long 2025-04-10 09:30 @ 1595.16 -> Exit long 09:45 @ 1596.95
// ---------------------------------------------------------------------------
class TrailPointsOffsetT20Host : public source::PineStrategyHost {
public:
    TrailPointsOffsetT20Host() {
        configure_pine_strategy(cfg());
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("L", true, kNaN, kNaN, 1.0, "trail long");
        } else if (i == 1) {
            strategy_exit("LX", "L", kNaN, kNaN, 80.0, 40.0, kNaN, 100.0, "trail long");
        }
    }
};

// ---------------------------------------------------------------------------
// Scenario 3: bracket-trailing-activation-offset-path-01
// Trade #235: Entry short 2025-08-08 15:30 @ 3952.56 -> Exit short 15:45 @ 3931.76
// ---------------------------------------------------------------------------
class TrailPathT235Host : public source::PineStrategyHost {
public:
    TrailPathT235Host() {
        configure_pine_strategy(cfg());
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("S", false, kNaN, kNaN, 1.0, "trail path short");
        } else if (i == 1) {
            strategy_exit("SX", "S", kNaN, kNaN, 800.0, 400.0, kNaN, 100.0, "trail path short");
        }
    }
};

} // namespace

int main() {
    // 1. Scenario 1, Trade 2: bracket-exit-stop-limit-trail-same-bar-01
    {
        TripleExitShortT2Host host;
        std::vector<Bar> bars = {
            mk(1743437700000LL, 1834.56, 1845.88, 1833.58, 1845.31),
            mk(1743438600000LL, 1845.31, 1845.77, 1841.07, 1843.14),
            mk(1743439500000LL, 1843.12, 1846.29, 1839.11, 1839.16),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("bracket-exit-stop-limit-trail#2", host.get_trade(0),
                         false, 1845.31, 1843.12, 2.19, 4.24, 0.46);
        }
    }

    // 2. Scenario 1, Trade 466: excursion parity for stop-limit-trail bracket
    {
        TripleExitShortT466Host host;
        std::vector<Bar> bars = {
            mk(1763482500000LL, 3119.18, 3159.98, 3115.3, 3155.66),
            mk(1763483400000LL, 3155.66, 3167.89, 3145.0, 3148.74),
            mk(1763484300000LL, 3148.74, 3158.21, 3144.48, 3150.88),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("bracket-exit-stop-limit-trail#466", host.get_trade(0),
                         false, 3155.66, 3148.74, 6.92, 11.18, 12.23);
        }
    }

    // 3. Scenario 2, Trade 20: bracket-trail-points-with-offset-only-01
    {
        TrailPointsOffsetT20Host host;
        std::vector<Bar> bars = {
            mk(1744276500000LL, 1591.39, 1598.59, 1591.38, 1595.17),
            mk(1744277400000LL, 1595.16, 1604.64, 1594.41, 1597.35),
            mk(1744278300000LL, 1597.34, 1606.02, 1595.85, 1603.23),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("bracket-trail-points-offset#20", host.get_trade(0),
                         true, 1595.16, 1596.95, 1.79, 9.48, 0.75);
        }
    }

    // 4. Scenario 3, Trade 235: bracket-trailing-activation-offset-path-01
    {
        TrailPathT235Host host;
        std::vector<Bar> bars = {
            mk(1754666100000LL, 3954.46, 3961.8, 3946.39, 3952.56),
            mk(1754667000000LL, 3952.56, 3963.77, 3925.0, 3927.76),
            mk(1754667900000LL, 3927.77, 3954.98, 3927.77, 3953.0),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("bracket-trailing-activation#235", host.get_trade(0),
                         false, 3952.56, 3931.76, 20.80, 27.56, 11.21);
        }
    }

    std::printf("test_l10j_corpus_parity: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
