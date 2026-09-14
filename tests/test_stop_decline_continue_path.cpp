#include "placement_observation_fixture.hpp"
/*
 * When the path-first member of a same-signal, true-flat, unlinked opposing
 * pure-STOP pair is cancelled specifically by stop-margin admission, the
 * ordinary historical path scanner must consider the later member. All other
 * rejection causes and order-book/scheduler shapes retain their old paths.
 *
 * Round 7 family K (tests/test_default_pct_stop_sizing.cpp) changed what this
 * file's historical shape does: the all-in DEFAULT percent-of-equity sell
 * stop 90 below the signal close 100 is sized at its level (10000 / 90) and
 * REJECTED AT THE CALL (111.11 x 100 = 11,111 > 10,000), so it never reaches
 * the book — there is no path-first member to decline at the fill and no
 * fence on the later long, which fills its 110 touch alone in every
 * configuration below. (A sell stop that IS accepted at the call costs less
 * at its fill — the level or a lower open — than at its placement close, so
 * a fill-time margin decline of a true-flat pair's leading short needs an
 * equity drop while resting, which a true-flat pair cannot have. The
 * continuation is a dormant safety net on that shape.)
 *
 * What stays pinned here: the continuation scope predicate as a pure
 * function on a two-stop book (continuation_scope builds the book under
 * margin 0 so the family-K admission does not empty it), the kernel's
 * results on the historical shape, and — the buy-stop-leading orientation —
 * a long declined at a gap-up open (family K: sized at the level, costed at
 * the rounded open) followed by the later short touch, which TradingView
 * applies as the same-bar second touch (classify_order_eligibility's
 * LongFirst pass-1 rule) and which fills at its level with its placement
 * quantity.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/magnifier.hpp>

#include "../src/engine_internal.hpp"
#include "../src/source/pine_path_resolve_internal.hpp"

using namespace pineforge;
using pineforge::source::PendingOrder;

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

class DualStopProbe : public pineforge::source::PineStrategyHost {
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

    void on_source_bar(const Bar&) override {
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
                    placement_fixture::prior_close_quantity(order, 1.0);
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
    void set_lots(double step) { qty_step_ = step; }
    bool continuation_scope(bool magnifier = false) {
        bar_index_ = 0;
        // Build the two-stop book with the placement check disabled (margin
        // 0): the predicate under test reads the book's shape, not the
        // family-K admission that drops the all-in short at the call.
        const double ml = margin_long_, ms = margin_short_;
        margin_long_ = 0.0;
        margin_short_ = 0.0;
        on_bar(Bar{});
        margin_long_ = ml;
        margin_short_ = ms;
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
    bool pending(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id == id) return true;
        return false;
    }
};

// O=100 is tied between H=111 and L=89, so the synthesized path is
// O->L->H->C. Historically the short stop at 90 was first and declined at
// the open-costed fill; under family K its call is rejected (11,111 > 10,000
// at the signal close) and only the long rests, sized 10000/110 at its
// level (9,090.9 <= 10,000 placed; 10,000 <= 10,000 admitted at the touch).
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

void check_long_alone(const DualStopProbe& probe) {
    CHECK(probe.side() == PositionSide::LONG);
    CHECK(std::fabs(probe.entry_price() - 110.0) < 1e-9);
    CHECK(std::fabs(probe.qty() - (10'000.0 / 110.0)) < 1e-9);
    CHECK(probe.trade_count() == 0);
    CHECK(!probe.pending("S"));
}

void test_rejected_all_in_short_leaves_later_stop_to_fill() {
    std::printf("rejected all-in short (family K placement) leaves the later stop to fill alone\n");
    DualStopProbe scope_probe;
    CHECK(scope_probe.continuation_scope());
    DualStopProbe probe;
    run_pair(probe);
    check_long_alone(probe);
}

// The buy-stop-leading orientation, whole shares, signal close 100: the buy
// stop 105 sizes 95 = floor(10000 / 105) and places (9,500 <= 10,000); the
// sell stop 99.5 sizes 100 and places (100 x 100 = 10,000 <= 10,000 — the lot
// floor absorbs the half tick). Bar 1 opens 106 through the buy stop: the
// long is DECLINED at the fill (95 x 106 = 10,070 > 10,000, costed at the
// rounded open) and dropped; the sell stop is touched later on the path
// (l 99) and fills at its level with its placement quantity (100 x 99.5 =
// 9,950 <= 10,000).
void test_long_gap_decline_then_short_touch_fills() {
    std::printf("long declined at the gap-up open, the later short touch fills at its level\n");
    DualStopProbe probe;
    probe.set_lots(1.0);
    probe.long_stop = 105.0;
    probe.short_stop = 99.5;
    Bar bars[] = {
        bar(1'000, 100.0, 100.0, 100.0, 100.0),
        bar(2'000, 106.0, 107.0, 99.0, 100.0),
    };
    probe.run(bars, 2);
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.entry_price() - 99.5) < 1e-9);
    CHECK(std::fabs(probe.qty() - 100.0) < 1e-9);
    CHECK(probe.trade_count() == 0);
    CHECK(!probe.pending("L"));
}

void test_accepted_first_stop_preserves_existing_second_touch_result() {
    std::printf("accepted first stop preserves the existing dual-touch result\n");
    DualStopProbe probe;
    probe.short_stop = 100.0;  // at the close: market-at-next-open, qty*open == equity
    run_pair(probe);
    // The accepted short opens first; the existing path logic then applies the
    // later long touch, closing most of it. The margin-decline continuation
    // must not alter this pre-existing residual/trade shape.
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.qty() - (100.0 - 10'000.0 / 110.0)) < 1e-9);
    CHECK(std::fabs(probe.entry_price() - 100.0) < 1e-9);
    CHECK(probe.trade_count() == 1);
}

void test_non_margin_rejection_does_not_change_shape() {
    std::printf("risk direction leaves the historical shape unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.long_only();
    CHECK(scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.long_only();
    run_pair(probe);
    check_long_alone(probe);
}

void test_oca_pair_is_out_of_scope() {
    std::printf("OCA pair retains ordinary cancellation semantics\n");
    DualStopProbe scope_probe;
    scope_probe.use_oca = true;
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.use_oca = true;
    run_pair(probe);
    check_long_alone(probe);
}

void test_pooc_is_out_of_scope() {
    std::printf("POOC path remains unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.enable_pooc();
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.enable_pooc();
    run_pair(probe);
    check_long_alone(probe);
}

void test_coof_is_out_of_scope() {
    std::printf("COOF path remains unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.enable_coof();
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.enable_coof();
    run_pair(probe);
    check_long_alone(probe);
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
        // The third order fills its share (the market at the open 100, the
        // limit / raw limit at 95 on the way down) and the long stop adds
        // 10000/110 at 110 on the way up; nothing is fenced because the
        // short never reached the book.
        CHECK(probe.side() == PositionSide::LONG);
        const double stop_qty = 10'000.0 / 110.0;
        const double expected_qty = 1.0 + stop_qty;
        const double third_price =
            kind == DualStopProbe::MixedOrder::Market ? 100.0 : 95.0;
        const double expected_price =
            (third_price + stop_qty * 110.0) / expected_qty;
        CHECK(std::fabs(probe.qty() - expected_qty) < 1e-9);
        CHECK(std::fabs(probe.entry_price() - expected_price) < 1e-9);
        CHECK(probe.trade_count() == 0);
    }
}

void test_non_true_flat_pair_is_out_of_scope() {
    std::printf("post-close flat provenance: predicate false, shape unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.mark_as_after_close = true;
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.mark_as_after_close = true;
    run_pair(probe);
    check_long_alone(probe);
}

void test_different_signal_bars_are_out_of_scope() {
    std::printf("different-signal stop pair: predicate false, shape unchanged\n");
    DualStopProbe scope_probe;
    scope_probe.split_signal_bars = true;
    CHECK(!scope_probe.continuation_scope());
    DualStopProbe probe;
    probe.split_signal_bars = true;
    run_pair(probe);
    check_long_alone(probe);
}

void test_magnifier_is_out_of_scope() {
    std::printf("magnifier path remains unchanged\n");
    DualStopProbe scope_probe;
    CHECK(!scope_probe.continuation_scope(true));
    DualStopProbe probe;
    run_pair(probe, true);
    check_long_alone(probe);
}

}  // namespace

int main() {
    test_rejected_all_in_short_leaves_later_stop_to_fill();
    test_long_gap_decline_then_short_touch_fills();
    test_accepted_first_stop_preserves_existing_second_touch_result();
    test_non_margin_rejection_does_not_change_shape();
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
