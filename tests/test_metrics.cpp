/*
 * test_metrics.cpp -- per-trade commission capture and bar-index round-trip.
 *
 * Verifies that emit_close_trade stores the entry+exit commission into
 * Trade::commission and that fill_report copies it faithfully into TradeC.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

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

namespace {

class MomoFlip : public BacktestEngine {
public:
    double prev_close_ = std::numeric_limits<double>::quiet_NaN();
    MomoFlip() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.1;
        pyramiding_ = 1;
    }
    void on_bar(const Bar& bar) override {
        if (!std::isnan(prev_close_)) {
            if (bar.close > prev_close_)
                strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 1.0, "up");
            else if (bar.close < prev_close_)
                strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 1.0, "dn");
        }
        prev_close_ = bar.close;
    }
};

std::vector<Bar> make_feed(int n) {
    std::vector<Bar> bars(n);
    for (int i = 0; i < n; ++i) {
        int phase = i % 20;
        int tri = (phase < 10) ? phase : (20 - phase);
        double close = 100.0 + tri * 1.5 + (i % 3);
        bars[i].open = close;
        bars[i].high = close + 1.0;
        bars[i].low = close - 1.0;
        bars[i].close = close;
        bars[i].volume = 1000.0 + (i % 100);
        bars[i].timestamp = (int64_t)(i + 1) * 900'000;
    }
    return bars;
}

}  // namespace

static void test_trade_commission_and_bar_indexes() {
    std::printf("trade commission + bar indexes\n");
    MomoFlip s;
    std::vector<Bar> bars = make_feed(120);
    s.run(bars.data(), (int)bars.size());
    ReportC rep{};
    s.fill_report(&rep);
    CHECK(rep.trades_len > 0);
    for (int i = 0; i < rep.trades_len; ++i) {
        const TradeC& t = rep.trades[i];
        // commission must equal what calc_commission charges for both legs
        // 0.1% commission = price * qty * pointvalue * (0.1 / 100.0)
        //                 = price * qty * 0.001 (pointvalue defaults to 1.0)
        double expect = t.entry_price * t.qty * 0.001 + t.exit_price * t.qty * 0.001;
        CHECK(std::fabs(t.commission - expect) < 1e-9);
        CHECK(t.commission > 0.0);
        CHECK(t.entry_bar_index >= 0);
        CHECK(t.exit_bar_index >= t.entry_bar_index);
    }
    BacktestEngine::free_report(&rep);
}

int main() {
    test_trade_commission_and_bar_indexes();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
