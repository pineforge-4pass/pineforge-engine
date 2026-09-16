// R4-D L10d: priced entries that fill mid-bar must not count the bar extreme
// reached BEFORE the fill (ab9714be pine_fills.cpp:42 skip_entry_bar_high/low
// + pine_risk.cpp:248-300). Literals are the legacy corpus engine_trades.csv
// rows that first diverge on 890da75.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
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

#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

bool near(double a, double b, double tol = 1e-8) { return std::abs(a - b) < tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

std::vector<Bar> load_15m(std::int64_t from_ms, std::int64_t to_ms) {
    const char* paths[] = {
        "corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv",
        "../corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv",
        "../../corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv",
        "/Users/haoliangwen/code/pineforge-engine-wt/adapter-lowering-l10d-20260917/corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv",
    };
    std::ifstream in;
    for (const char* path : paths) {
        in.open(path);
        if (in) break;
        in.clear();
    }
    std::vector<Bar> out;
    if (!in) return out;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string tok;
        std::int64_t ts = 0;
        double o = 0, h = 0, l = 0, c = 0, v = 0;
        if (!std::getline(ss, tok, ',')) continue;
        ts = std::stoll(tok);
        if (ts < from_ms || ts > to_ms) continue;
        if (!std::getline(ss, tok, ',')) continue; o = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue; h = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue; l = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue; c = std::stod(tok);
        if (std::getline(ss, tok, ',')) v = std::stod(tok);
        out.push_back(Bar{o, h, l, c, v, ts});
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
    return c;
}

class DualStop : public source::PineStrategyHost {
public:
    DualStop() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("SE2", false, kNaN, current_bar_.close);
            strategy_entry("LE2", true, kNaN, current_bar_.close);
        }
    }
};

class GapShort : public source::PineStrategyHost {
public:
    GapShort() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("SE", false, kNaN, current_bar_.high * 10.0);
        if (pine_bar_index() == 24) strategy_close_all();
    }
};

class TouchShort : public source::PineStrategyHost {
public:
    TouchShort() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 && std::isfinite(prev_low_))
            strategy_entry("SE", false, kNaN, prev_low_ - 0.01);
        if (pine_bar_index() == 17) strategy_close_all();
        prev_low_ = current_bar_.low;
    }
private:
    double prev_low_ = kNaN;
};

class CompositeShort : public source::PineStrategyHost {
public:
    CompositeShort() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("ShortOnGap", false, kNaN, current_bar_.close);
        if (pine_bar_index() == 1) strategy_close_all();
    }
};

class RangeLong : public source::PineStrategyHost {
public:
    RangeLong() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 && std::isfinite(prev_high_))
            strategy_entry("LongOnRng", true, kNaN, prev_high_);
        if (pine_bar_index() == 28)
            strategy_entry("ShortOnRng", false, kNaN, 1813.0);
        prev_high_ = current_bar_.high;
    }
private:
    double prev_high_ = kNaN;
};

void expect_trade(const char* tag, const Trade& t, bool is_long,
                  double entry_px, double exit_px, double fav, double adv) {
    std::printf("%s %s @%.4f->%.4f mfe=%.6f mae=%.6f (want mfe=%.6f mae=%.6f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price,
                t.max_runup, t.max_drawdown, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px, 1e-6));
    CHECK(near(t.exit_price, exit_px, 1e-6));
    CHECK(near(t.max_runup, fav, 1e-6));
    CHECK(near(t.max_drawdown, adv, 1e-6));
}

} // namespace

int main() {
    {
        DualStop host;
        auto bars = load_15m(1745340300000LL, 1745341200000LL);
        if (bars.empty()) {
            bars = {
                mk(1745339100000LL, 1693.52, 1696.22, 1685.32, 1691.13),
                mk(1745340000000LL, 1691.11, 1700.56, 1690.25, 1692.61),
            };
        }
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() >= 1);
        if (host.trade_count() >= 1)
            expect_trade("dual-stop-open-tie#46", host.get_trade(0), false,
                         1691.11, 1691.13, 0.0, 0.02);
    }
    {
        GapShort host;
        auto bars = load_15m(1744617600000LL, 1744640100000LL);
        if (bars.empty()) {
            std::printf("FAIL gap-short: missing 15m csv\n");
            ++failed;
        } else {
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() >= 1);
            if (host.trade_count() >= 1)
                expect_trade("deferred-flip-gap-stops#29", host.get_trade(0), false,
                             1630.11, 1661.93, 0.0, 61.46);
        }
    }
    {
        TouchShort host;
        auto bars = load_15m(1746885600000LL, 1746901800000LL);
        if (bars.empty()) {
            std::printf("FAIL touch-short: missing 15m csv\n");
            ++failed;
        } else {
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() >= 1);
            if (host.trade_count() >= 1)
                expect_trade("stop-entry-touch-boundary#60", host.get_trade(0), false,
                             2413.99, 2467.86, 0.0, 98.01);
        }
    }
    {
        CompositeShort host;
        auto bars = load_15m(1743663600000LL, 1743665400000LL);
        if (bars.empty()) {
            bars = {
                mk(1743660000000LL, 1816.62, 1821.4, 1810.19, 1812.34),
                mk(1743660900000LL, 1812.34, 1820.76, 1805.56, 1819.24),
                mk(1743661800000LL, 1819.23, 1819.76, 1808.0, 1811.14),
            };
        }
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() >= 1);
        if (host.trade_count() >= 1)
            expect_trade("composite-bracket-cap#11", host.get_trade(0), false,
                         1812.34, 1819.23, 0.0, 8.42);
    }
    {
        RangeLong host;
        auto bars = load_15m(1743638400000LL, 1743664500000LL);
        if (bars.empty()) {
            std::printf("FAIL range-long: missing 15m csv\n");
            ++failed;
        } else {
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() >= 1);
            if (host.trade_count() >= 1)
                expect_trade("range-expansion-pending-stop#22", host.get_trade(0), true,
                             1811.09, 1812.34, 33.88, 5.53);
        }
    }
    std::printf("test_l10d_entry_bar_excursion_masks: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
