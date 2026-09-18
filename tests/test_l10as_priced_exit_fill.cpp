// R4-D L10as: a priced EXIT leg (strategy.exit stop= / trail_points=) fills on
// the same bar and at the same price as the legacy owner (ab9714be).
//
// Pinned rows (window-mode lane replays, owner values):
//   corpus/validation/zz-pop-stevenygabbyperez-fast-scalper-with-stops on
//   NYSE:F (mintick 0.01):
//     #32 Entry long 2025-04-21 19:45 @9.45, Exit long 2025-04-22 13:30 @9.64
//     (omitted-offset trail_points activation 9.45 + 19 ticks = 9.64; the
//     13:30 high 9.635 reaches it on the tick-quantized path; the native
//     route used to wait for the raw 15:00 high).
//   corpus/validation/zz-pop-thulashimohanr-prev-day-week-levels-or-vwap-strategy
//   on OANDA:EURUSD (mintick 1e-05):
//     #10/#11 Entry short 2025-04-17 13:45 @1.13594 q=1 each,
//             Exit short 2025-04-17 14:15 @1.13690 (stop = OR high).
//
// Bars are embedded literals copied out of the lane feeds; this test must
// never open corpus files or absolute paths at runtime.
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

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

// ohlcv_NYSE-F_15m.csv, 2025-04-21 19:30 .. 2025-04-22 15:00 (UTC).
std::vector<Bar> nyse_f_bars() {
    return {
        mk(1745263800000LL, 9.39, 9.45, 9.39, 9.445),    // 0: 04-21 19:30 (signal)
        mk(1745264700000LL, 9.445, 9.49, 9.44, 9.47),    // 1: 04-21 19:45 (entry)
        mk(1745328600000LL, 9.55, 9.635, 9.53, 9.605),   // 2: 04-22 13:30 (owner exit)
        mk(1745329500000LL, 9.605, 9.63, 9.59, 9.59),    // 3: 04-22 13:45
        mk(1745330400000LL, 9.59, 9.595, 9.55, 9.56),    // 4: 04-22 14:00
        mk(1745331300000LL, 9.56, 9.595, 9.54, 9.57),    // 5: 04-22 14:15
        mk(1745332200000LL, 9.56, 9.62, 9.56, 9.615),    // 6: 04-22 14:30
        mk(1745333100000LL, 9.61, 9.615, 9.58, 9.605),   // 7: 04-22 14:45
        mk(1745334000000LL, 9.61, 9.67, 9.61, 9.665),    // 8: 04-22 15:00 (raw reach)
    };
}

class NyseFTrailHost : public source::PineStrategyHost {
public:
    explicit NyseFTrailHost(std::vector<Bar> bars) : bars_(std::move(bars)) {
        source::PineStrategyConfig c;
        c.initial_capital = 100000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100.0;
        configure_pine_strategy(c);
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        syminfo_.ticker = "NYSE:F";
        syminfo_.tickerid = "NYSE:F";
        syminfo_.type = "stock";
        syminfo_.timezone = "America/New_York";
        syminfo_.session = "0930-1600";
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        const double close = bars_[0].close;
        strategy_entry("Long", true);
        strategy_exit("Exit Long", "Long", kNaN, close * 0.99,
                      close * 0.02 / 0.01);
    }

private:
    std::vector<Bar> bars_;
};

void check_long_trail_exit(const std::vector<Bar>& bars, int exit_bar,
                           double exit_price) {
    NyseFTrailHost host(bars);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() < 1) return;
    const Trade& t = host.get_trade(0);
    CHECK(t.is_long);
    CHECK(t.entry_id == "Long");
    CHECK(t.exit_id == "Exit Long");
    CHECK(near(t.entry_price, 9.45));
    CHECK(t.entry_time == bars[1].timestamp);
    CHECK(t.exit_time == bars[static_cast<std::size_t>(exit_bar)].timestamp);
    CHECK(near(t.exit_price, exit_price));
    CHECK(near(t.qty, 100.0));
    if (t.exit_time != bars[static_cast<std::size_t>(exit_bar)].timestamp
        || !near(t.exit_price, exit_price)) {
        std::printf("  got exit_time=%lld exit_price=%.10g\n",
                    static_cast<long long>(t.exit_time), t.exit_price);
    }
}

// ab9714be engine_path_resolve.cpp:297-308: the omitted-offset activation
// 9.64 is reached by the 13:30 high 9.635 on the tick-quantized path.
void test_stevenygabbyperez_trail_fills_on_tick_reach() {
    check_long_trail_exit(nyse_f_bars(), 2, 9.64);
}

// The same book with the 13:30 open moved to 9.635 (half a tick short of the
// activation, a raw print the owner does not gap-fill,
// engine_path_resolve.cpp:667-705): the owner reaches 9.64 on the first
// tick-path leg and books the activation, not the open's directional tick.
void test_open_short_of_activation_books_activation() {
    auto bars = nyse_f_bars();
    bars[2] = mk(1745328600000LL, 9.635, 9.66, 9.60, 9.65);
    check_long_trail_exit(bars, 2, 9.64);
}

// ohlcv_OANDA-EURUSD_15m.csv, 2025-04-17 13:00 .. 14:30 (UTC).
std::vector<Bar> eurusd_bars() {
    return {
        mk(1744894800000LL, 1.13566, 1.1378, 1.13509, 1.13554),   // 0: 13:00 (long signal)
        mk(1744895700000LL, 1.13554, 1.1369, 1.13518, 1.13656),   // 1: 13:15 (long fill; OR bar)
        mk(1744896600000LL, 1.13657, 1.13686, 1.13544, 1.13595),  // 2: 13:30 (short signal)
        mk(1744897500000LL, 1.13594, 1.13652, 1.13548, 1.13614),  // 3: 13:45 (reversal fill)
        mk(1744898400000LL, 1.13616, 1.13638, 1.13571, 1.13635),  // 4: 14:00
        mk(1744899300000LL, 1.13636, 1.1386, 1.13624, 1.13779),   // 5: 14:15 (stop fill)
        mk(1744900200000LL, 1.13778, 1.13799, 1.13674, 1.13742),  // 6: 14:30
    };
}

class EurUsdStopHost : public source::PineStrategyHost {
public:
    EurUsdStopHost() {
        source::PineStrategyConfig c;
        c.initial_capital = 500000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 2;
        c.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
        c.commission_value = 20.0;
        configure_pine_strategy(c);
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

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("Long", true, kNaN, kNaN, 2.0);
        } else if (i == 2) {
            const double or_high = 1.1369;
            strategy_entry("Short", false, kNaN, kNaN, 2.0);
            strategy_exit("ShortT1", "Short", bar.close - 40.0, or_high,
                          kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
            strategy_exit("ShortT2", "Short", bar.close - 100.0, or_high,
                          kNaN, kNaN, kNaN, 100.0, "T2 Exit", 1.0);
        }
    }
};

// ab9714be pine_fills.cpp:7913-7958: the short's stop 1.13690 is first
// reached by the 14:15 high; neither leg fills on the 14:00 bar.
void test_thulashimohanr_short_stop_fill() {
    EurUsdStopHost host;
    const auto bars = eurusd_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 3);
    if (host.trade_count() < 3) {
        std::printf("  got trade_count=%d\n", host.trade_count());
        return;
    }
    const Trade& t9 = host.get_trade(0);
    CHECK(t9.is_long);
    CHECK(t9.exit_time == bars[3].timestamp);
    CHECK(near(t9.exit_price, 1.13594));
    for (int k = 1; k <= 2; ++k) {
        const Trade& t = host.get_trade(k);
        CHECK(!t.is_long);
        CHECK(t.entry_id == "Short");
        CHECK(near(t.qty, 1.0));
        CHECK(near(t.entry_price, 1.13594));
        CHECK(t.entry_time == bars[3].timestamp);
        CHECK(t.exit_time == bars[5].timestamp);
        CHECK(near(t.exit_price, 1.1369));
        CHECK(near(t.pnl, -40.00096, 1e-6));
        if (t.exit_time != bars[5].timestamp || !near(t.exit_price, 1.1369)) {
            std::printf("  got exit_time=%lld exit_price=%.10g\n",
                        static_cast<long long>(t.exit_time), t.exit_price);
        }
    }
}

// ohlcv_OANDA-EURUSD_15m.csv, 2025-04-25 13:15 .. 14:15 (UTC).
std::vector<Bar> eurusd_flat_short_bars() {
    return {
        mk(1745586900000LL, 1.13466, 1.1351, 1.13338, 1.13405),   // 0: 13:15 (OR bar)
        mk(1745587800000LL, 1.13404, 1.13418, 1.1333, 1.13406),   // 1: 13:30 (short signal)
        mk(1745588700000LL, 1.13407, 1.13556, 1.1339, 1.13536),   // 2: 13:45 (fill + stop)
        mk(1745589600000LL, 1.13536, 1.13717, 1.13526, 1.13647),  // 3: 14:00
        mk(1745590500000LL, 1.13646, 1.137, 1.13578, 1.13696),    // 4: 14:15
    };
}

class EurUsdFlatShortHost : public EurUsdStopHost {
public:
    void on_source_bar(const Bar& bar) override {
        if (pine_bar_index() != 1) return;
        const double or_high = 1.1351;
        strategy_entry("Short", false, kNaN, kNaN, 2.0);
        strategy_exit("ShortT1", "Short", bar.close - 40.0, or_high,
                      kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
        strategy_exit("ShortT2", "Short", bar.close - 100.0, or_high,
                      kNaN, kNaN, kNaN, 100.0, "T2 Exit", 1.0);
    }
};

// Owner rows #12/#13: a flat explicit q2 short opened at the 13:45 open
// 1.13407 is stopped at the OR high 1.13510 on its entry bar (high 1.13556).
void test_thulashimohanr_flat_short_entry_bar_stop() {
    EurUsdFlatShortHost host;
    const auto bars = eurusd_flat_short_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    for (int k = 0; k < host.trade_count(); ++k) {
        const Trade& t = host.get_trade(k);
        CHECK(!t.is_long);
        CHECK(near(t.qty, 1.0));
        CHECK(near(t.entry_price, 1.13407));
        CHECK(t.entry_time == bars[2].timestamp);
        CHECK(t.exit_time == bars[2].timestamp);
        CHECK(near(t.exit_price, 1.1351));
        CHECK(near(t.pnl, -40.00103, 1e-6));
        if (t.exit_time != bars[2].timestamp || !near(t.exit_price, 1.1351)) {
            std::printf("  got exit_time=%lld exit_price=%.10g\n",
                        static_cast<long long>(t.exit_time), t.exit_price);
        }
    }
}

// ohlcv_OANDA-EURUSD_15m.csv, 2025-10-23 13:30/13:45 and 2025-10-24
// 13:30 .. 14:15 (UTC).
std::vector<Bar> eurusd_addon_bars() {
    return {
        mk(1761226200000LL, 1.16039, 1.16107, 1.16036, 1.16068),  // 0: 10-23 13:30 (signal)
        mk(1761227100000LL, 1.16069, 1.16084, 1.16034, 1.16069),  // 1: 10-23 13:45 (fill)
        mk(1761312600000LL, 1.16335, 1.1637, 1.16284, 1.16342),   // 2: 10-24 13:30 (add-on signal)
        mk(1761313500000LL, 1.16336, 1.16336, 1.16204, 1.16272),  // 3: 10-24 13:45 (fill + stop)
        mk(1761314400000LL, 1.16271, 1.1634, 1.16209, 1.1633),    // 4: 10-24 14:00
        mk(1761315300000LL, 1.16328, 1.16359, 1.16259, 1.16263),  // 5: 10-24 14:15
    };
}

class EurUsdAddOnHost : public EurUsdStopHost {
public:
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i != 0 && i != 2) return;
        const double or_low = i == 0 ? 1.15 : 1.16261;
        strategy_entry("Long", true, kNaN, kNaN, 2.0);
        strategy_exit("LongT1", "Long", bar.close + 40.0, or_low,
                      kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
        strategy_exit("LongT2", "Long", bar.close + 100.0, or_low,
                      kNaN, kNaN, kNaN, 100.0, "T2 Exit", 1.0);
    }
};

// Owner rows #204-#206: the carried 10-23 lot (2 units) and the same-id
// add-on filled at the 10-24 13:45 open 1.16336 (2 units) are all stopped at
// the re-issued OR low 1.16261 on the add-on's entry bar.
void test_thulashimohanr_addon_entry_bar_stop() {
    EurUsdAddOnHost host;
    const auto bars = eurusd_addon_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 3);
    // Owner order: the first leg's FIFO unit of the carried lot, the KI-62
    // cover of the whole same-bar add, then the carried lot's last unit.
    const double entries[] = {1.16069, 1.16336, 1.16069};
    const double qtys[] = {1.0, 2.0, 1.0};
    const double pnls[] = {-39.99808, -80.0015, -39.99808};
    for (int k = 0; k < host.trade_count() && k < 3; ++k) {
        const Trade& t = host.get_trade(k);
        CHECK(t.is_long);
        CHECK(near(t.entry_price, entries[k]));
        CHECK(near(t.qty, qtys[k]));
        CHECK(near(t.pnl, pnls[k], 1e-6));
        CHECK(t.exit_time == bars[3].timestamp);
        CHECK(near(t.exit_price, 1.16261));
        if (t.exit_time != bars[3].timestamp || !near(t.exit_price, 1.16261)
            || !near(t.qty, qtys[k])) {
            std::printf("  got entry=%.10g exit_time=%lld exit_price=%.10g qty=%g\n",
                        t.entry_price, static_cast<long long>(t.exit_time),
                        t.exit_price, t.qty);
        }
    }
}

// ohlcv_OANDA-EURUSD_15m.csv, 2026-03-27 17:00/17:15, 2026-03-30 13:30/13:45
// and 2026-03-31 01:00 .. 01:30 (UTC).
std::vector<Bar> eurusd_fifo_bars() {
    return {
        mk(1774630800000LL, 1.1519, 1.15292, 1.15142, 1.15148),   // 0: 03-27 17:00 (ShortAdd signal)
        mk(1774631700000LL, 1.15149, 1.15154, 1.15094, 1.15096),  // 1: 03-27 17:15 (fill)
        mk(1774877400000LL, 1.1479, 1.14808, 1.14674, 1.1469),    // 2: 03-30 13:30 (Short signal)
        mk(1774878300000LL, 1.1469, 1.1469, 1.14622, 1.14642),    // 3: 03-30 13:45 (fill)
        mk(1774918800000LL, 1.14747, 1.14776, 1.14734, 1.14775),  // 4: 03-31 01:00
        mk(1774919700000LL, 1.14774, 1.14899, 1.1477, 1.14893),   // 5: 03-31 01:15 (stop fill)
        mk(1774920600000LL, 1.14894, 1.14908, 1.14834, 1.14839),  // 6: 03-31 01:30
    };
}

class EurUsdFifoHost : public EurUsdStopHost {
public:
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("ShortAdd", false, kNaN, kNaN, 2.0);
            strategy_exit("ShortAddT1", "ShortAdd", bar.close - 40.0, 1.16,
                          kNaN, kNaN, kNaN, 100.0, "Add T1", 1.0);
            strategy_exit("ShortAddT2", "ShortAdd", bar.close - 100.0, 1.16,
                          kNaN, kNaN, kNaN, 100.0, "Add T2", 1.0);
        } else if (i == 2) {
            strategy_entry("Short", false, kNaN, kNaN, 2.0);
            strategy_exit("ShortT1", "Short", bar.close - 40.0, 1.1488,
                          kNaN, kNaN, kNaN, 100.0, "T1 Exit", 1.0);
            strategy_exit("ShortT2", "Short", bar.close - 100.0, 1.1488,
                          kNaN, kNaN, kNaN, 100.0, "T2 Exit", 1.0);
        }
    }
};

// Owner rows #359/#360: the Short legs' 1.14880 stop fills at 03-31 01:15
// and, under FIFO, retires the older ShortAdd lot (entry 1.15149), not the
// Short lot its from_entry names.
void test_thulashimohanr_fifo_bracket_lot() {
    EurUsdFifoHost host;
    const auto bars = eurusd_fifo_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    for (int k = 0; k < host.trade_count(); ++k) {
        const Trade& t = host.get_trade(k);
        CHECK(!t.is_long);
        CHECK(t.entry_id == "ShortAdd");
        CHECK(near(t.entry_price, 1.15149));
        CHECK(near(t.qty, 1.0));
        CHECK(t.exit_time == bars[5].timestamp);
        CHECK(near(t.exit_price, 1.1488));
        CHECK(near(t.pnl, -39.99731, 1e-6));
        if (t.entry_id != "ShortAdd" || t.exit_time != bars[5].timestamp) {
            std::printf("  got entry_id=%s entry=%.10g exit_time=%lld exit_price=%.10g\n",
                        t.entry_id.c_str(), t.entry_price,
                        static_cast<long long>(t.exit_time), t.exit_price);
        }
    }
}

}  // namespace

int main() {
    test_thulashimohanr_fifo_bracket_lot();
    test_thulashimohanr_addon_entry_bar_stop();
    test_thulashimohanr_flat_short_entry_bar_stop();
    test_stevenygabbyperez_trail_fills_on_tick_reach();
    test_open_short_of_activation_books_activation();
    test_thulashimohanr_short_stop_fill();
    std::printf("test_l10as_priced_exit_fill: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
