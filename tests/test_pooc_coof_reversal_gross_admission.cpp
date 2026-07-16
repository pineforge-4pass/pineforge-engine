/*
 * TradingView terminal-C gross admission for the Fran-470 shape.
 *
 * Two distinct explicit-FIXED opposite MARKET strategy.entry calls are queued
 * from true flat with process_orders_on_close + calc_on_order_fills.  Each own
 * qty passes the ordinary signal-time margin check.  TV nevertheless declines
 * the later source call when the pair's gross reversal transaction exceeds
 * placement equity.  For fixed/smaller pairs, the duration-one survivor pins
 * the second source call; TV's scratch-row direction is only report
 * attribution and is not asserted here.
 *
 * Clean-room TV anchors:
 *   pf-probe-coof-pooc-opposite-market-ordering
 *   pf-probe-coof-pooc-opposite-market-fran-factors
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

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        const double _a = (a);                                                 \
        const double _b = (b);                                                 \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar bar(int64_t ts, double price) {
    return Bar{price, price, price, price, 1.0, ts};
}

class Probe final : public BacktestEngine {
public:
    Probe(bool first_long, double qty) : first_long_(first_long), qty_(qty) {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
        calc_on_order_fills_ = true;
        margin_call_enabled_ = false;
    }

    bool coof = true;
    bool pooc = true;
    bool add_third = false;
    double commission_pct = 0.0;
    double margin_pct = 100.0;
    int slip_ticks = 0;

    void on_bar(const Bar&) override {
        calc_on_order_fills_ = coof;
        process_orders_on_close_ = pooc;
        commission_value_ = commission_pct;
        margin_long_ = margin_pct;
        margin_short_ = margin_pct;
        slippage_ = slip_ticks;

        if (bar_index_ == 0) {
            strategy_entry("E1", first_long_, kNaN, kNaN, qty_);
            strategy_entry("E2", !first_long_, kNaN, kNaN, qty_);
            if (add_third) {
                strategy_entry("E3", first_long_, kNaN, kNaN, qty_);
            }
        } else if (position_side_ != PositionSide::FLAT) {
            strategy_cancel_all();
            strategy_close_all();
        }
    }

    double signed_size() const { return signed_position_size(); }

private:
    bool first_long_;
    double qty_;
};

static std::vector<Bar> flat_feed() {
    return {bar(0, 100.0), bar(900'000, 100.0), bar(1'800'000, 100.0)};
}

static void assert_first_only(bool first_long) {
    Probe p(first_long, 95.0);  // own=95%, gross=190% of equity
    auto bars = flat_feed();
    p.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    CHECK_NEAR(p.signed_size(), 0.0, 1e-9);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long == first_long);
        CHECK_NEAR(t.qty, 95.0, 1e-9);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 1);
    }
}

static void assert_both_fill(bool first_long, double qty) {
    Probe p(first_long, qty);
    auto bars = flat_feed();
    p.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    CHECK_NEAR(p.signed_size(), 0.0, 1e-9);
    if (p.trade_count() == 2) {
        const Trade& scratch = p.get_trade(0);
        const Trade& survivor = p.get_trade(1);
        CHECK(scratch.is_long == first_long);
        CHECK(survivor.is_long != first_long);
        CHECK(scratch.entry_bar_index == 0);
        CHECK(scratch.exit_bar_index == 0);
        CHECK(survivor.entry_bar_index == 0);
        CHECK(survivor.exit_bar_index == 1);
        CHECK_NEAR(scratch.qty, qty, 1e-9);
        CHECK_NEAR(survivor.qty, qty, 1e-9);
    }
}

static void test_red_gross_over_equity_declines_later_call() {
    std::printf("test_red_gross_over_equity_declines_later_call\n");
    assert_first_only(true);
    assert_first_only(false);
}

static void test_green_fixed_probe_and_margin_equality_policy() {
    std::printf("test_green_fixed_probe_and_margin_equality_policy\n");
    for (bool first_long : {true, false}) {
        assert_both_fill(first_long, 1.0);
        // Equality admission preserves the engine's ordinary `required >
        // equity` margin model. The TV probes bracket low/high gross cases but
        // do not claim to pin the exact equality point.
        assert_both_fill(first_long, 50.0);
    }
}

class MutationProbe final : public BacktestEngine {
public:
    enum class Shape {
        SameBarReplacement,
        PriorBarReplacement,
        CanceledThird,
        CancelRearm,
        PriorRestingCancelRearm,
        RejectedExtra,
        RejectedInfiniteExtra,
        CancelAllThenPair,
        NextBarCleanPair,
    };

    explicit MutationProbe(Shape shape) : shape_(shape) {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        process_orders_on_close_ = true;
        calc_on_order_fills_ = true;
        margin_call_enabled_ = false;
    }

    double observed_size = 0.0;
    int observed_trades = 0;

    void on_bar(const Bar&) override {
        if (shape_ == Shape::SameBarReplacement) {
            if (bar_index_ == 0) {
                strategy_entry("SR-A", true, kNaN, kNaN, 55.0);
                strategy_entry("SR-B", false, kNaN, kNaN, 55.0);
                strategy_entry("SR-A", true, kNaN, kNaN, 55.0);
            } else if (bar_index_ == 1) {
                observe_and_close();
            }
            return;
        }

        if (shape_ == Shape::PriorBarReplacement) {
            if (bar_index_ == 0) {
                strategy_entry("REP-A", true, 90.0, kNaN, 1.0);
            } else if (bar_index_ == 1) {
                strategy_entry("REP-B", false, kNaN, kNaN, 55.0);
                strategy_entry("REP-A", true, kNaN, kNaN, 55.0);
            } else if (bar_index_ == 2) {
                observe_and_close();
            }
            return;
        }

        if (shape_ == Shape::PriorRestingCancelRearm) {
            if (bar_index_ == 0) {
                strategy_entry("PR-A", true, 90.0, kNaN, 1.0);
            } else if (bar_index_ == 1) {
                strategy_cancel("PR-A");
                strategy_entry("PR-B", false, kNaN, kNaN, 55.0);
                strategy_entry("PR-A", true, kNaN, kNaN, 55.0);
            } else if (bar_index_ == 2) {
                observe_and_close();
            }
            return;
        }

        if (shape_ == Shape::NextBarCleanPair) {
            if (bar_index_ == 0) {
                strategy_entry("NB-X", true, kNaN, kNaN, 1.0);
                strategy_cancel_all();
            } else if (bar_index_ == 1) {
                strategy_entry("NB-A", true, kNaN, kNaN, 95.0);
                strategy_entry("NB-B", false, kNaN, kNaN, 95.0);
            } else if (bar_index_ == 2) {
                observe_and_close();
            }
            return;
        }

        if (bar_index_ != 0) {
            if (bar_index_ == 1) observe_and_close();
            return;
        }
        if (shape_ == Shape::CanceledThird) {
            strategy_entry("CT-A", true, kNaN, kNaN, 55.0);
            strategy_entry("CT-B", false, kNaN, kNaN, 55.0);
            strategy_entry("CT-C", true, kNaN, kNaN, 55.0);
            strategy_cancel("CT-C");
        } else if (shape_ == Shape::CancelRearm) {
            strategy_entry("CR-A", true, kNaN, kNaN, 55.0);
            strategy_entry("CR-B", false, kNaN, kNaN, 55.0);
            strategy_cancel("CR-A");
            strategy_entry("CR-C", true, kNaN, kNaN, 55.0);
        } else if (shape_ == Shape::RejectedExtra) {
            strategy_entry("RX-X", true, kNaN, kNaN, 101.0);
            strategy_entry("RX-A", true, kNaN, kNaN, 55.0);
            strategy_entry("RX-B", false, kNaN, kNaN, 55.0);
        } else if (shape_ == Shape::RejectedInfiniteExtra) {
            strategy_entry(
                "RI-X", true, kNaN, kNaN,
                std::numeric_limits<double>::infinity());
            strategy_entry("RI-A", true, kNaN, kNaN, 55.0);
            strategy_entry("RI-B", false, kNaN, kNaN, 55.0);
        } else if (shape_ == Shape::CancelAllThenPair) {
            strategy_entry("CA-X", true, kNaN, kNaN, 1.0);
            strategy_cancel_all();
            strategy_entry("CA-A", true, kNaN, kNaN, 55.0);
            strategy_entry("CA-B", false, kNaN, kNaN, 55.0);
        }
    }

private:
    void observe_and_close() {
        observed_size = signed_position_size();
        observed_trades = trade_count();
        strategy_cancel_all();
        strategy_close_all();
    }

    Shape shape_;
};

static void test_green_mutated_two_order_books_fail_closed() {
    auto run = [](const char* label, MutationProbe::Shape shape,
                  double expected_size, bool prior_bar = false,
                  int expected_trades = 1) {
        std::printf("%s\n", label);
        MutationProbe probe(shape);
        auto bars = prior_bar
            ? std::vector<Bar>{
                  bar(0, 100.0), bar(900'000, 100.0),
                  bar(1'800'000, 100.0), bar(2'700'000, 100.0)}
            : flat_feed();
        probe.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(probe.last_error().empty());
        CHECK(probe.observed_trades == expected_trades);
        CHECK_NEAR(probe.observed_size, expected_size, 1e-9);
    };

    run("same-bar replacement stays on ordinary path",
        MutationProbe::Shape::SameBarReplacement, -55.0);
    run("prior-bar replacement stays on ordinary path",
        MutationProbe::Shape::PriorBarReplacement, -55.0,
        /*prior_bar=*/true);
    run("canceled third call stays on ordinary path",
        MutationProbe::Shape::CanceledThird, -55.0);
    run("cancel-rearm three-call set stays on ordinary path",
        MutationProbe::Shape::CancelRearm, 55.0);
    run("prior-resting cancel-rearm stays on ordinary path",
        MutationProbe::Shape::PriorRestingCancelRearm, 55.0,
        /*prior_bar=*/true);
    run("signal-rejected extra call stays on ordinary path",
        MutationProbe::Shape::RejectedExtra, -55.0);
    run("infinite signal-rejected call stays on ordinary path",
        MutationProbe::Shape::RejectedInfiniteExtra, -55.0);
    run("cancel-all then pair stays on ordinary path",
        MutationProbe::Shape::CancelAllThenPair, -55.0);
    run("prior-bar tombstone does not suppress a clean next-bar pair",
        MutationProbe::Shape::NextBarCleanPair, 95.0,
        /*prior_bar=*/true, /*expected_trades=*/0);
}

static void assert_excluded_pair_uses_legacy_result(
        const char* label, bool coof, bool pooc, double commission,
        double margin, int slippage, int expected_trades = 2) {
    std::printf("%s\n", label);
    Probe p(true, 95.0);
    p.coof = coof;
    p.pooc = pooc;
    p.commission_pct = commission;
    p.margin_pct = margin;
    p.slip_ticks = slippage;
    auto bars = flat_feed();
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == expected_trades);
}

static void test_green_scope_exclusions() {
    assert_excluded_pair_uses_legacy_result(
        "exclude without COOF", false, true, 0.0, 100.0, 0);
    assert_excluded_pair_uses_legacy_result(
        "exclude without POOC", true, false, 0.0, 100.0, 0,
        /*expected_trades=*/1);
    assert_excluded_pair_uses_legacy_result(
        "exclude commissioned pair", true, true, 0.1, 100.0, 0);
    assert_excluded_pair_uses_legacy_result(
        "exclude custom margin", true, true, 0.0, 50.0, 0);
    assert_excluded_pair_uses_legacy_result(
        "exclude slippage", true, true, 0.0, 100.0, 1);

    std::printf("exclude three-call book\n");
    Probe three(true, 30.0);
    three.add_third = true;
    auto bars = flat_feed();
    three.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(three.last_error().empty());
    CHECK(three.trade_count() == 3);
}

int main() {
    test_red_gross_over_equity_declines_later_call();
    test_green_fixed_probe_and_margin_equality_policy();
    test_green_mutated_two_order_books_fail_closed();
    test_green_scope_exclusions();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
