/*
 * Integer-lot percent-exit reservation.
 *
 * TradingView allocates one minimum contract/share to the first positive
 * qty_percent strategy.exit request when a one-lot position cannot be split.
 * A later sibling sees the consumed capacity and reserves nothing. The rule
 * is identical for long/short and for brackets armed before the entry fills,
 * while fractional lots, explicit qty, full-percent exits and already-on-grid
 * percent quantities retain their established behavior.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static Bar bar(double o, double h, double l, double c, int i) {
    Bar b;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    b.volume = 1000.0;
    b.timestamp = static_cast<int64_t>(i + 1) * 60'000;
    return b;
}

class ExitProbe : public BacktestEngine {
public:
    enum class Mode {
        LiveLong,
        LiveShort,
        DeferredLong,
        FractionalLot,
        ExplicitQty,
        FullPercent,
        OnGridPartial,
    };

    explicit ExitProbe(Mode mode) : mode_(mode) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = mode == Mode::OnGridPartial ? 4.0 : 1.0;
        qty_step_ = mode == Mode::FractionalLot ? 0.1 : 1.0;
        syminfo_.qty_step = qty_step_;
        syminfo_.pointvalue = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 1;
    }

    int armed_exit_count = -1;
    std::vector<double> armed_qty;

    double position() const { return signed_position_size(); }

    void on_bar(const Bar&) override {
        const bool short_mode = mode_ == Mode::LiveShort;
        const bool deferred = mode_ == Mode::DeferredLong;

        if (bar_index_ == 0) {
            strategy_entry("E", !short_mode, kNaN, kNaN, kNaN, "entry");
            if (deferred) {
                arm_percent_pair(/*is_short=*/false);
            }
        }

        if (bar_index_ == 1) {
            if (!deferred) {
                if (mode_ == Mode::ExplicitQty) {
                    strategy_exit("X", "E", 105.0, kNaN,
                                  kNaN, kNaN, kNaN, 50.0,
                                  "explicit", 0.5, "");
                } else if (mode_ == Mode::FullPercent) {
                    strategy_exit("X", "E", 105.0, kNaN,
                                  kNaN, kNaN, kNaN, 100.0,
                                  "full", kNaN, "");
                } else if (mode_ == Mode::OnGridPartial) {
                    strategy_exit("X", "E", 105.0, kNaN,
                                  kNaN, kNaN, kNaN, 50.0,
                                  "on-grid", kNaN, "");
                } else {
                    arm_percent_pair(short_mode);
                }
            }
            snapshot_exits();
        }
    }

private:
    Mode mode_;

    void arm_percent_pair(bool is_short) {
        const double first_limit = is_short ? 95.0 : 105.0;
        const double second_limit = is_short ? 90.0 : 110.0;
        strategy_exit("TP1", "E", first_limit, kNaN,
                      kNaN, kNaN, kNaN, 50.0, "tp1", kNaN, "");
        strategy_exit("TP2", "E", second_limit, kNaN,
                      kNaN, kNaN, kNaN, 50.0, "tp2", kNaN, "");
    }

    void snapshot_exits() {
        armed_exit_count = 0;
        armed_qty.clear();
        for (const PendingOrder& order : pending_orders_) {
            if (order.type != OrderType::EXIT) continue;
            ++armed_exit_count;
            armed_qty.push_back(order.qty);
        }
    }
};

static void test_live_one_lot_pair_long() {
    std::printf("test_live_one_lot_pair_long\n");
    ExitProbe p(ExitProbe::Mode::LiveLong);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 0),
        bar(100, 100, 100, 100, 1),
        bar(100, 106, 99, 104, 2),
    };
    p.run(bars, 3);
    CHECK(p.armed_exit_count == 1);
    CHECK(p.armed_qty.size() == 1);
    if (!p.armed_qty.empty()) CHECK(near(p.armed_qty[0], 1.0));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).qty, 1.0));
        CHECK(near(p.get_trade(0).exit_price, 105.0));
    }
    CHECK(near(p.position(), 0.0));
}

static void test_live_one_lot_pair_short() {
    std::printf("test_live_one_lot_pair_short\n");
    ExitProbe p(ExitProbe::Mode::LiveShort);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 0),
        bar(100, 100, 100, 100, 1),
        bar(100, 101, 94, 96, 2),
    };
    p.run(bars, 3);
    CHECK(p.armed_exit_count == 1);
    CHECK(p.armed_qty.size() == 1);
    if (!p.armed_qty.empty()) CHECK(near(p.armed_qty[0], 1.0));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).qty, 1.0));
        CHECK(near(p.get_trade(0).exit_price, 95.0));
    }
    CHECK(near(p.position(), 0.0));
}

static void test_deferred_one_lot_pair() {
    std::printf("test_deferred_one_lot_pair\n");
    ExitProbe p(ExitProbe::Mode::DeferredLong);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 0),
        bar(100, 100, 100, 100, 1),
        bar(100, 106, 99, 104, 2),
    };
    p.run(bars, 3);
    CHECK(p.armed_exit_count == 1);
    CHECK(p.armed_qty.size() == 1);
    if (!p.armed_qty.empty()) CHECK(near(p.armed_qty[0], 1.0));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).qty, 1.0));
        CHECK(near(p.get_trade(0).exit_price, 105.0));
    }
    CHECK(near(p.position(), 0.0));
}

static void test_fractional_lot_keeps_two_half_exits() {
    std::printf("test_fractional_lot_keeps_two_half_exits\n");
    ExitProbe p(ExitProbe::Mode::FractionalLot);
    const Bar bars[] = {
        bar(100, 100, 100, 100, 0),
        bar(100, 100, 100, 100, 1),
        bar(100, 106, 99, 104, 2),
    };
    p.run(bars, 3);
    CHECK(p.armed_exit_count == 2);
    CHECK(p.armed_qty.size() == 2);
    if (p.armed_qty.size() == 2) {
        CHECK(near(p.armed_qty[0], 0.5));
        CHECK(near(p.armed_qty[1], 0.5));
    }
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) CHECK(near(p.get_trade(0).qty, 0.5));
    CHECK(near(p.position(), 0.5));
}

static void test_explicit_full_and_on_grid_controls() {
    std::printf("test_explicit_full_and_on_grid_controls\n");
    struct Case {
        ExitProbe::Mode mode;
        double expected_armed;
        double expected_closed;
        double expected_remaining;
    };
    const Case cases[] = {
        {ExitProbe::Mode::ExplicitQty, 0.5, 0.5, 0.5},
        {ExitProbe::Mode::FullPercent, 1.0, 1.0, 0.0},
        {ExitProbe::Mode::OnGridPartial, 2.0, 2.0, 2.0},
    };
    for (const Case& c : cases) {
        ExitProbe p(c.mode);
        const Bar bars[] = {
            bar(100, 100, 100, 100, 0),
            bar(100, 100, 100, 100, 1),
            bar(100, 106, 99, 104, 2),
        };
        p.run(bars, 3);
        CHECK(p.armed_exit_count == 1);
        CHECK(p.armed_qty.size() == 1);
        if (!p.armed_qty.empty()) {
            CHECK(near(p.armed_qty[0], c.expected_armed));
        }
        CHECK(p.trade_count() == 1);
        if (p.trade_count() == 1) {
            CHECK(near(p.get_trade(0).qty, c.expected_closed));
        }
        CHECK(near(p.position(), c.expected_remaining));
    }
}

int main() {
    test_live_one_lot_pair_long();
    test_live_one_lot_pair_short();
    test_deferred_one_lot_pair();
    test_fractional_lot_keeps_two_half_exits();
    test_explicit_full_and_on_grid_controls();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
