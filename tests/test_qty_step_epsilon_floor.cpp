/*
 * test_qty_step_epsilon_floor.cpp — regular-order quantity flooring must
 * absorb only binary64 residue immediately below a lot boundary.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                \
    do {                                                                       \
        const double a_ = (actual);                                            \
        const double e_ = (expected);                                          \
        if (!(std::fabs(a_ - e_) <= (tolerance))) {                            \
            std::printf("  FAIL  %s:%d  %.17g != %.17g\n",                    \
                        __FILE__, __LINE__, a_, e_);                            \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class QtyFloorProbe : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}

    double regular(double qty, double step) {
        qty_step_ = step;
        return apply_qty_step(qty);
    }

    double partial_exit(double qty, double step) {
        qty_step_ = step;
        return apply_exit_qty_step(qty);
    }
};

void test_binary_residue_snaps_without_broad_rounding() {
    std::printf("test_binary_residue_snaps_without_broad_rounding\n");
    QtyFloorProbe probe;

    // In binary64, 1 / 1e-5 is 99999.99999999999. A plain floor loses one
    // complete lot and returns 0.99999.
    CHECK(1.0 / 1e-5 < 100000.0);
    CHECK_NEAR(probe.regular(1.0, 1e-5), 1.0, 0.0);

    // More than 1e-6 of a lot below the next integer is genuine off-grid
    // quantity and must still floor, never ceil.
    constexpr double step = 1e-5;
    const double outside = (100000.0 - 2e-6) * step;
    CHECK(100000.0 - outside / step > 1e-6);
    CHECK_NEAR(probe.regular(outside, step), 99999.0 * step, 1e-15);
    CHECK(probe.regular(outside, step) < outside);
}

void test_integer_and_fractional_controls() {
    std::printf("test_integer_and_fractional_controls\n");
    QtyFloorProbe probe;

    CHECK_NEAR(probe.regular(7.0, 1.0), 7.0, 0.0);
    CHECK_NEAR(probe.regular(7.75, 1.0), 7.0, 0.0);
    CHECK_NEAR(probe.regular(1.234567, 0.0001), 1.2345, 1e-15);

    // Preserve the caller's original representation when flooring is a
    // no-op; reconstructing 3 * 0.1 would be one ulp larger than 0.3.
    CHECK(probe.regular(0.3, 0.1) == 0.3);

    // Factor A remains a distinct helper and retains its established
    // epsilon-safe 50% exit quantity.
    CHECK_NEAR(probe.partial_exit(0.5, 1e-5), 0.5, 0.0);
}

class TwoHalfExitProbe : public BacktestEngine {
public:
    TwoHalfExitProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        qty_step_ = 1e-5;
        syminfo_mintick_ = 0.01;
        commission_value_ = 0.0;
        slippage_ = 0;
        margin_call_enabled_ = false;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 1.0);
        } else if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
            strategy_exit("TP", "L", 110.0, kNaN,
                          kNaN, kNaN, kNaN, 50.0, "half-limit");
            strategy_exit("SL", "L", kNaN, 95.0,
                          kNaN, kNaN, kNaN, 50.0, "half-stop");
        }
    }

    double position_qty() const { return position_qty_; }
};

Bar bar(int64_t timestamp, double open, double high, double low,
        double close) {
    return Bar{open, high, low, close, 1000.0, timestamp};
}

void test_two_half_exits_flat_exactly() {
    std::printf("test_two_half_exits_flat_exactly\n");
    TwoHalfExitProbe probe;
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 100.0, 100.0, 100.0),
        bar(2'000, 100.0, 101.0,  99.0, 100.0),
        bar(3'000, 100.0, 112.0,  94.0, 100.0),
        bar(4'000, 100.0, 101.0,  99.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK_NEAR(probe.position_qty(), 0.0, 0.0);
    CHECK(probe.trade_count() == 2);
    double closed_qty = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        CHECK_NEAR(trade.qty, 0.5, 0.0);
        closed_qty += trade.qty;
    }
    CHECK_NEAR(closed_qty, 1.0, 0.0);
}

}  // namespace

int main() {
    std::printf("--- qty_step_epsilon_floor ---\n");
    test_binary_residue_snaps_without_broad_rounding();
    test_integer_and_fractional_controls();
    test_two_half_exits_flat_exactly();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
