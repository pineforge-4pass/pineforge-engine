// R4-D L10h: bar-magnifier tick-distribution fills book the stop level on
// an intra-bar path cross (ab9714be pine_scheduler.cpp:911-916 keeps the
// script-bar open while samples update H/L/C; pine_fills.cpp:7913 +
// engine_path_resolve.cpp:905-927 fill at the stop, not the sample quote).
// Literals are engine ab9714b corpus/validation engine_trades.csv trade #1
// for magnifier-tick-dist-endpoints-01, volume-weighted-on-01, and
// endpoints-rsi-cross-08a.
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/magnifier.hpp>
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

#define CHECK(x) do { if (x) ++passed; else { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failed; } } while (0)

bool near(double a, double b, double tol = 1e-8) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

// The 15m ETH-USDT bars of the replayed ranges, copied from the corpus derived
// feed so the test runs without a corpus checkout (CI has no derived feeds).
struct FeedRow { std::int64_t ts; double o, h, l, c, v; };
const FeedRow kFeed[] = {
    {1743397200000LL, 1804, 1813, 1803.33, 1811.96, 49634.773},
    {1743398100000LL, 1811.96, 1812, 1801.08, 1808.93, 51943.482},
    {1743420600000LL, 1801.93, 1807.96, 1800.92, 1806.37, 37418.258},
    {1743421500000LL, 1806.37, 1819, 1805.97, 1812.52, 92807.927},
    {1743422400000LL, 1812.51, 1815, 1809.24, 1809.48, 39810.958},
    {1743423300000LL, 1809.49, 1818.8, 1808.15, 1816.41, 45388.38},
    {1743424200000LL, 1816.41, 1821.41, 1813.07, 1820.02, 64916.79},
    {1743425100000LL, 1820.02, 1845.78, 1817.89, 1833.49, 221572.936},
    {1743426000000LL, 1833.5, 1836.7, 1824.41, 1824.68, 59094.131},
    {1743426900000LL, 1824.79, 1829.13, 1821.36, 1822.92, 51538.87},
    {1743427800000LL, 1822.93, 1825.64, 1792.6, 1803.91, 256839.71},
    {1745173800000LL, 1579.37, 1579.84, 1577.5, 1579.35, 18196.504},
    {1745174700000LL, 1579.34, 1580.85, 1578.67, 1580.58, 9656.684},
};

std::vector<Bar> load_15m(std::int64_t from_ms, std::int64_t to_ms) {
    std::vector<Bar> out;
    for (const auto& r : kFeed) {
        if (r.ts < from_ms || r.ts > to_ms) continue;
        out.push_back(Bar{r.o, r.h, r.l, r.c, r.v, r.ts});
    }
    return out;
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = (int)QtyType::FIXED;
    c.default_qty_value = 1;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.commission_type = (int)CommissionType::PERCENT;
    c.slippage = 0;
    return c;
}

class MidBarStop : public source::PineStrategyHost {
public:
    explicit MidBarStop(bool half_open_high) : half_open_high_(half_open_high) {
        configure_pine_strategy(cfg());
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar& bar) override {
        if (placed_) return;
        strategy_entry("L", true, kNaN, kNaN, 1.0, "entry long");
        const double stop = half_open_high_
            ? bar.open + (bar.high - bar.open) * 0.5
            : (bar.open + bar.high) * 0.5;
        strategy_exit("X", "L", kNaN, stop, kNaN, kNaN, kNaN, 100.0, "mid-bar stop");
        placed_ = true;
    }
private:
    bool half_open_high_;
    bool placed_ = false;
};

void expect_trade(const char* tag, const Trade& t,
                  double entry_px, double exit_px,
                  double pnl, double fav, double adv) {
    std::printf("%s %s@%.2f->%s@%.2f pnl=%.4f mfe=%.4f mae=%.4f\n",
                tag, t.entry_id.c_str(), t.entry_price,
                t.exit_id.c_str(), t.exit_price,
                t.pnl, t.max_runup, t.max_drawdown);
    CHECK(t.is_long);
    CHECK(near(t.qty, 1.0, 1e-6));
    CHECK(near(t.entry_price, entry_px, 1e-6));
    CHECK(near(t.exit_price, exit_px, 1e-6));
    CHECK(near(t.pnl, pnl, 1e-6));
    CHECK(near(t.max_runup, fav, 1e-6));
    CHECK(near(t.max_drawdown, adv, 1e-6));
}

void run_magnifier(MidBarStop& host, const std::vector<Bar>& bars, bool volume_weighted) {
    if (volume_weighted) host.set_magnifier_volume_weighted(true);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15",
             true, 4, MagnifierDistribution::ENDPOINTS);
}

} // namespace

int main() {
    {
        // magnifier-tick-dist-endpoints-01 trade #1: signal 11:30, entry
        // 11:45 @1806.37, mid-bar stop 1804.945 fills at 1804.94 on the
        // 13:30 H->L path (not at the 1792.60 low sample).
        MidBarStop host(true);
        auto bars = load_15m(1743420600000LL, 1743427800000LL);
        if (bars.empty()) {
            bars = {
                mk(1743420600000LL, 1801.93, 1807.96, 1800.92, 1806.37, 37418.258),
                mk(1743421500000LL, 1806.37, 1819.00, 1805.97, 1812.52, 92807.927),
                mk(1743422400000LL, 1812.51, 1815.00, 1809.24, 1809.48, 39810.958),
                mk(1743423300000LL, 1809.49, 1818.80, 1808.15, 1816.41, 45388.38),
                mk(1743424200000LL, 1816.41, 1821.41, 1813.07, 1820.02, 64916.79),
                mk(1743425100000LL, 1820.02, 1845.78, 1817.89, 1833.49, 221572.936),
                mk(1743426000000LL, 1833.50, 1836.70, 1824.41, 1824.68, 59094.131),
                mk(1743426900000LL, 1824.79, 1829.13, 1821.36, 1822.92, 51538.87),
                mk(1743427800000LL, 1822.93, 1825.64, 1792.60, 1803.91, 256839.71),
            };
        }
        run_magnifier(host, bars, false);
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            CHECK(t.entry_time == 1743421500000LL);
            CHECK(t.exit_time == 1743427800000LL);
            expect_trade("endpoints-01#1", t, 1806.37, 1804.94, -1.43, 39.41, 1.43);
        }
    }
    {
        // Same tape and stop with volume-weighted ENDPOINTS samples: owner
        // engine_trades.csv is byte-identical to endpoints-01 trade #1.
        MidBarStop host(true);
        auto bars = load_15m(1743420600000LL, 1743427800000LL);
        if (bars.empty()) {
            bars = {
                mk(1743420600000LL, 1801.93, 1807.96, 1800.92, 1806.37, 37418.258),
                mk(1743421500000LL, 1806.37, 1819.00, 1805.97, 1812.52, 92807.927),
                mk(1743422400000LL, 1812.51, 1815.00, 1809.24, 1809.48, 39810.958),
                mk(1743423300000LL, 1809.49, 1818.80, 1808.15, 1816.41, 45388.38),
                mk(1743424200000LL, 1816.41, 1821.41, 1813.07, 1820.02, 64916.79),
                mk(1743425100000LL, 1820.02, 1845.78, 1817.89, 1833.49, 221572.936),
                mk(1743426000000LL, 1833.50, 1836.70, 1824.41, 1824.68, 59094.131),
                mk(1743426900000LL, 1824.79, 1829.13, 1821.36, 1822.92, 51538.87),
                mk(1743427800000LL, 1822.93, 1825.64, 1792.60, 1803.91, 256839.71),
            };
        }
        run_magnifier(host, bars, true);
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            CHECK(t.entry_time == 1743421500000LL);
            CHECK(t.exit_time == 1743427800000LL);
            expect_trade("volume-weighted-on-01#1", t, 1806.37, 1804.94, -1.43, 39.41, 1.43);
        }
    }
    {
        // magnifier-tick-dist-endpoints-rsi-cross-08a trade #1: signal 05:00
        // stop (open+high)/2 = 1808.50, entry+exit on 05:15 @1811.96->1808.50.
        MidBarStop host(false);
        auto bars = load_15m(1743397200000LL, 1743398100000LL);
        if (bars.empty()) {
            bars = {
                mk(1743397200000LL, 1804.00, 1813.00, 1803.33, 1811.96, 49634.773),
                mk(1743398100000LL, 1811.96, 1812.00, 1801.08, 1808.93, 51943.482),
            };
        }
        run_magnifier(host, bars, false);
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            CHECK(t.entry_time == 1743398100000LL);
            CHECK(t.exit_time == 1743398100000LL);
            expect_trade("rsi-cross-08a#1", t, 1811.96, 1808.50, -3.46, 0.04, 3.46);
        }
    }
    {
        // endpoints-01 trade #41: wrong-side magnifier gap at the 18:45
        // open books 1579.34/1579.34 (0 pnl). Owner adverse is 0; native
        // must not fold the 18:45 low 1578.67 (0.67).
        MidBarStop host(true);
        auto bars = load_15m(1745173800000LL, 1745174700000LL);
        if (bars.size() < 2) {
            bars = {
                mk(1745173800000LL, 1579.37, 1579.84, 1577.50, 1579.35),
                mk(1745174700000LL, 1579.34, 1580.85, 1578.67, 1580.58),
            };
        }
        run_magnifier(host, bars, false);
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            expect_trade("endpoints-01#41", t, 1579.34, 1579.34, 0.0, 0.0, 0.0);
        }
    }
    std::printf("test_l10h_corpus_parity: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
