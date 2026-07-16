/*
 * When the path-first member of a same-signal, true-flat, unlinked opposing
 * pure-STOP pair is cancelled specifically by stop-margin admission, the
 * ordinary historical path scanner must consider the later member. All other
 * rejection causes and order-book/scheduler shapes retain their old paths.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/magnifier.hpp>

#include "../src/engine_internal.hpp"

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

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar bar(int64_t timestamp, double open, double high, double low,
        double close) {
    return {open, high, low, close, 1.0, timestamp};
}

class DualStopProbe : public BacktestEngine {
public:
    DualStopProbe() {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0;
        set_margin_call_enabled(false);
    }

    double long_stop = 110.0;
    double short_stop = 90.0;
    bool use_oca = false;
    bool mark_as_after_close = false;
    bool split_signal_bars = false;
    enum class MixedOrder { None, Market, Limit, Raw };
    MixedOrder mixed_order = MixedOrder::None;

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        const std::string oca = use_oca ? "PAIR" : "";
        const int oca_type = use_oca ? 1 : 0;
        strategy_entry("L", true, kNaN, long_stop, kNaN, "", oca,
                       oca_type);
        strategy_entry("S", false, kNaN, short_stop, kNaN, "", oca,
                       oca_type);
        if (split_signal_bars) {
            pending_orders_.back().created_bar -= 1;
        }
        if (mark_as_after_close) {
            for (PendingOrder& order : pending_orders_) {
                if (order.type == OrderType::ENTRY) {
                    order.created_after_position_close_in_bar = true;
                }
            }
        }
        if (mixed_order == MixedOrder::Market) {
            strategy_entry("M", true, kNaN, kNaN, 1.0);
        } else if (mixed_order == MixedOrder::Limit) {
            strategy_entry("X", true, 95.0, kNaN, 1.0);
        } else if (mixed_order == MixedOrder::Raw) {
            strategy_order("R", true, 1.0, 95.0, kNaN);
        }
    }

    void long_only() { risk_direction_ = RiskDirection::LONG_ONLY; }
    void enable_pooc() { process_orders_on_close_ = true; }
    void enable_coof() { calc_on_order_fills_ = true; }
    bool continuation_scope(bool magnifier = false) {
        bar_index_ = 0;
        on_bar(Bar{});
        const bool scoped = internal::dual_stop_margin_decline_can_continue_path(
            pending_orders_, internal::DualEntryStopPathWinner::ShortFirst,
            process_orders_on_close_, calc_on_order_fills_, magnifier);
        pending_orders_.clear();
        bar_index_ = -1;
        return scoped;
    }
    PositionSide side() const { return position_side_; }
    double qty() const { return position_qty_; }
    double entry_price() const { return position_entry_price_; }
};

// O=100 is tied between H=111 and L=89, so the synthesized path is
// O->L->H->C. The short stop at 90 is first. Its all-in qty is 10000/90,
// but admission costs that qty at open 100, so it declines. The later long
// stop is affordable because its qty is 10000/110.
Bar low_first_dual_touch() {
    return bar(2'000, 100.0, 111.0, 89.0, 100.0);
}

void run_pair(DualStopProbe& probe, bool magnifier = false) {
    Bar bars[] = {
        bar(1'000, 100.0, 100.0, 100.0, 100.0),
        low_first_dual_touch(),
    };
    if (magnifier) {
        probe.run(bars, 2, "15", "15", true, 4,
                  MagnifierDistribution::ENDPOINTS);
    } else {
        probe.run(bars, 2);
    }
}

void test_margin_decline_continues_to_affordable_later_stop() {
    std::printf("margin decline continues to the affordable later stop\n");
    DualStopProbe scope_probe;
    CHECK(scope_probe.continuation_scope());
    DualStopProbe probe;
    run_pair(probe);
    CHECK(probe.side() == PositionSide::LONG);
    CHECK(std::fabs(probe.entry_price() - 110.0) < 1e-9);
    CHECK(std::fabs(probe.qty() - (10'000.0 / 110.0)) < 1e-9);
    CHECK(probe.trade_count() == 0);
}

void test_accepted_first_stop_preserves_existing_second_touch_result() {
    std::printf("accepted first stop preserves the existing dual-touch result\n");
    DualStopProbe probe;
    probe.short_stop = 100.0;  // marketable at open; qty*open == equity
    run_pair(probe);
    // The accepted short opens first; the existing path logic then applies the
    // later long touch, closing most of it. The margin-decline continuation
    // must not alter this pre-existing residual/trade shape.
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.qty() - (100.0 - 10'000.0 / 110.0)) < 1e-9);
    CHECK(std::fabs(probe.entry_price() - 100.0) < 1e-9);
    CHECK(probe.trade_count() == 1);
}

void test_non_margin_rejection_does_not_continue() {
    std::printf("risk rejection does not release the later stop\n");
    DualStopProbe scope_probe;
    scope_probe.long_only();
    CHECK(scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.long_only();
    run_pair(probe);
    CHECK(probe.side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 0);
}

void test_oca_pair_is_out_of_scope() {
    std::printf("OCA pair retains ordinary cancellation semantics\n");
    DualStopProbe scope_probe;
    scope_probe.use_oca = true;
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.use_oca = true;
    run_pair(probe);
    CHECK(probe.side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 0);
}

void test_pooc_is_out_of_scope() {
    std::printf("POOC path remains unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.enable_pooc();
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.enable_pooc();
    run_pair(probe);
    CHECK(probe.side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 0);
}

void test_coof_is_out_of_scope() {
    std::printf("COOF path remains unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.enable_coof();
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.enable_coof();
    run_pair(probe);
    CHECK(probe.side() == PositionSide::LONG);
    CHECK(std::fabs(probe.qty() - (10'000.0 / 110.0)) < 1e-9);
    CHECK(std::fabs(probe.entry_price() - 110.0) < 1e-9);
    CHECK(probe.trade_count() == 0);
}

void test_mixed_order_books_are_out_of_scope() {
    std::printf("market/limit/raw books remain unchanged\n");
    const DualStopProbe::MixedOrder kinds[] = {
        DualStopProbe::MixedOrder::Market,
        DualStopProbe::MixedOrder::Limit,
        DualStopProbe::MixedOrder::Raw,
    };
    for (DualStopProbe::MixedOrder kind : kinds) {
        DualStopProbe scope_probe;
        scope_probe.mixed_order = kind;
        CHECK(!scope_probe.continuation_scope());
        DualStopProbe probe;
        probe.mixed_order = kind;
        run_pair(probe);
        CHECK(probe.side() == PositionSide::LONG);
        if (kind == DualStopProbe::MixedOrder::Market) {
            const double stop_qty = 10'000.0 / 110.0;
            const double expected_qty = 1.0 + stop_qty;
            const double expected_price =
                (100.0 + stop_qty * 110.0) / expected_qty;
            CHECK(std::fabs(probe.qty() - expected_qty) < 1e-9);
            CHECK(std::fabs(probe.entry_price() - expected_price) < 1e-9);
        } else {
            CHECK(std::fabs(probe.qty() - 1.0) < 1e-9);
            CHECK(std::fabs(probe.entry_price() - 95.0) < 1e-9);
        }
        CHECK(probe.trade_count() == 0);
    }
}

void test_non_true_flat_pair_is_out_of_scope() {
    std::printf("post-close flat provenance does not release the later stop\n");
    DualStopProbe scope_probe;
    scope_probe.mark_as_after_close = true;
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.mark_as_after_close = true;
    run_pair(probe);
    CHECK(probe.side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 0);
}

void test_different_signal_bars_are_out_of_scope() {
    std::printf("different-signal stop pair does not release the later stop\n");
    DualStopProbe scope_probe;
    scope_probe.split_signal_bars = true;
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.split_signal_bars = true;
    run_pair(probe);
    CHECK(probe.side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 0);
}

void test_magnifier_is_out_of_scope() {
    std::printf("magnifier path remains unchanged\n");
    DualStopProbe scope_probe;
    CHECK(!scope_probe.continuation_scope(true));
    DualStopProbe probe;
    run_pair(probe, true);
    CHECK(probe.side() == PositionSide::LONG);
    CHECK(std::fabs(probe.qty() - (10'000.0 / 110.0)) < 1e-9);
    CHECK(std::fabs(probe.entry_price() - 110.0) < 1e-9);
    CHECK(probe.trade_count() == 0);
}

}  // namespace

int main() {
    test_margin_decline_continues_to_affordable_later_stop();
    test_accepted_first_stop_preserves_existing_second_touch_result();
    test_non_margin_rejection_does_not_continue();
    test_oca_pair_is_out_of_scope();
    test_pooc_is_out_of_scope();
    test_coof_is_out_of_scope();
    test_mixed_order_books_are_out_of_scope();
    test_non_true_flat_pair_is_out_of_scope();
    test_different_signal_bars_are_out_of_scope();
    test_magnifier_is_out_of_scope();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
