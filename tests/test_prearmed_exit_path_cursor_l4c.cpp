#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
/*
 * A resting strategy.exit bracket becomes eligible only once its priced
 * from_entry parent fills. On that entry bar it may consume the remaining
 * synthetic OHLC path, never a stop touch that preceded the parent fill.
 *
 * The four cells mirror the TradingView-pinned clean-room probe
 * order-pooc-resting-bracket-path-01: A/C have a pre-entry-only stop touch and
 * must exit next bar; B/D touch the stop after entry and must exit same bar.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/pineforge.h>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/engine_internal.hpp"

using namespace pineforge;
using pineforge::source::PendingOrder;

namespace pineforge::broker {
// Read-only fixture projection for the removed owner-local priority receipt.
// It has no execution or adapter authority.
struct OrderPriorityDecision {
    std::array<std::pair<std::uint64_t, int>, 2> entries{};
    int sequence(std::uint64_t incarnation, int fallback) const noexcept {
        for (const auto& entry : entries) {
            if (entry.first == incarnation) return entry.second;
        }
        return fallback;
    }
};
} // namespace pineforge::broker

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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Cell { LongPre, LongPost, ShortPre, ShortPost };

class RestingBracketProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit RestingBracketProbe(Cell cell) : cell_(cell) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = true;
        calc_on_order_fills_ = false;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        const bool is_long = cell_ == Cell::LongPre || cell_ == Cell::LongPost;
        const double entry = is_long ? 110.0 : 90.0;
        const double stop = is_long ? 90.0 : 110.0;
        strategy_entry("E", is_long, kNaN, entry, 1.0, "priced parent");
        strategy_exit("X", "E", kNaN, stop, kNaN, kNaN, kNaN,
                      100.0, "resting child");
    }

private:
    Cell cell_;
};

// Vasudev-shaped cancellation topology.  X is armed with an original E,
// survives an explicit E cancellation, then keeps its older priority when X is
// reissued alongside a freshly recreated E.  Both live objects have the same
// created_bar but X still has the lower created_seq.  At the next broker
// scan the legacy sequence order visits X while truly flat, skips it, and fills
// E afterwards.  With POOC the close-time on_bar therefore observes the
// transient position before the second broker scan revisits X.
enum class BookVariant {
    ExactPair,
    FreshChild,
    PostCancelDoubleReissue,
    MissingParentCancel,
    InterleavedThird,
    InterleavedFourth,
    IncarnationGap,
    ChildOca,
    SharedOca,
    MultipleChildren,
};

class FreshParentProbe final : public pineforge::source::PineStrategyHost {
public:
    FreshParentProbe(Cell cell, int parent_first_factor,
                     BookVariant variant = BookVariant::ExactPair,
                     bool pine_attachment = true)
        : cell_(cell), variant_(variant) {
        // Legacy Pine numeric assertions require an explicit frontend opt-in.
        if (pine_attachment) attach_pine_execution_adapter();
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = true;
        calc_on_order_fills_ = false;
        if (parent_first_factor >= 0) {
            set_syminfo_metadata(
                "flat_retained_child_fresh_parent_order",
                parent_first_factor ? 1.0 : 0.0);
        }
    }

    void on_source_bar(const Bar&) override {
        const bool is_long =
            cell_ == Cell::LongPre || cell_ == Cell::LongPost;
        const double child_stop = is_long ? 90.0 : 110.0;
        const double child_limit = is_long ? 130.0 : 70.0;
        if (bar_index_ == 0) {
            strategy_entry("E", is_long, kNaN,
                           is_long ? 130.0 : 70.0,
                           kNaN, "original parent",
                           variant_ == BookVariant::SharedOca ? "G" : "",
                           variant_ == BookVariant::SharedOca ? 1 : 0);
            if (variant_ != BookVariant::FreshChild
                && variant_ != BookVariant::PostCancelDoubleReissue) {
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "retained child");
            }
            if (variant_ == BookVariant::InterleavedThird
                || variant_ == BookVariant::InterleavedFourth) {
                strategy_entry("U", is_long, kNaN,
                               is_long ? 1'000.0 : 1.0,
                               1.0, "unrelated resting parent");
                if (variant_ == BookVariant::InterleavedFourth)
                    strategy_entry("V", is_long, kNaN,
                                   is_long ? 2'000.0 : 0.5,
                                   1.0, "fourth unrelated parent");
            } else if (variant_ == BookVariant::MultipleChildren) {
                strategy_exit("Y", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "second retained child");
            }
        } else if (bar_index_ == 1) {
            for (const PendingOrder& order : pending_orders_) {
                if (order.type == OrderType::ENTRY && order.id == "E") {
                    cancelled_parent_incarnation = order.incarnation;
                }
                if (order.type == OrderType::EXIT && order.id == "X"
                    && order.from_entry == "E") {
                    surviving_child_incarnation_at_cancel =
                        order.incarnation;
                }
            }
            if (variant_ == BookVariant::MissingParentCancel) {
                // Construct the same final topology after a non-command
                // removal. The production rule must require the named-cancel
                // tombstone, not merely infer cancellation from absence.
                l4c_remove_entry_without_named_cancel("E");
            } else {
                strategy_cancel("E");
            }
            if (variant_ == BookVariant::PostCancelDoubleReissue) {
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "post-cancel fresh child");
                strategy_entry("E", is_long, kNaN,
                               is_long ? 110.0 : 90.0,
                               kNaN, "fresh parent");
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "post-cancel reissued child");
            } else if (variant_ == BookVariant::FreshChild
                || variant_ == BookVariant::MissingParentCancel) {
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              variant_ == BookVariant::FreshChild
                                  ? "fresh child" : "retained child");
                strategy_entry("E", is_long, kNaN,
                               is_long ? 110.0 : 90.0,
                               kNaN, "fresh parent");
            } else {
                strategy_entry("E", is_long, kNaN,
                               is_long ? 110.0 : 90.0,
                               kNaN, "fresh parent",
                               variant_ == BookVariant::SharedOca ? "G" : "",
                               variant_ == BookVariant::SharedOca ? 1 : 0);
                if (variant_ == BookVariant::IncarnationGap) {
                    strategy_entry("U", is_long, kNaN,
                                   is_long ? 1'000.0 : 1.0, 1.0);
                    strategy_cancel("U");
                }
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "retained child", kNaN,
                              variant_ == BookVariant::ChildOca
                                  || variant_ == BookVariant::SharedOca ? "G" : "");
            }
            if (variant_ == BookVariant::MultipleChildren) {
                strategy_exit("Y", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "second retained child");
            }
            const PendingOrder* parent = nullptr;
            const PendingOrder* child = nullptr;
            for (const PendingOrder& order : pending_orders_) {
                if (order.type == OrderType::ENTRY && order.id == "E") {
                    parent = &order;
                }
                if (order.type == OrderType::EXIT && order.id == "X"
                    && order.from_entry == "E") {
                    child = &order;
                }
            }
            pending_book_size_on_reissue = pending_orders_.size();
            fresh_parent_shape_seen = parent != nullptr && child != nullptr
                && (parent->replaced_order_incarnation == 0)
                && child->created_seq < parent->created_seq
                && child->created_bar == parent->created_bar;
            parent_cancel_provenance_seen = parent != nullptr
                && parent->recreated_after_named_cancelled_entry_incarnation
                    != 0;
            parent_cancel_token_exact = parent != nullptr
                && cancelled_parent_incarnation != 0
                && parent->recreated_after_named_cancelled_entry_incarnation
                    == cancelled_parent_incarnation;
            parent_cancel_child_token_exact = parent != nullptr
                && surviving_child_incarnation_at_cancel != 0
                && parent->named_cancel_surviving_exit_incarnation
                    == surviving_child_incarnation_at_cancel;
            cancel_token_consumed = !l4c_named_entry_cancel_active("E");
            parent_then_child_incarnations = parent != nullptr
                && child != nullptr
                && parent->incarnation
                    < std::numeric_limits<uint64_t>::max()
                && child->incarnation == parent->incarnation + 1;
            child_reissue_provenance_seen = child != nullptr
                && (child->replaced_order_incarnation != 0);
            child_replacement_token_exact = child != nullptr
                && parent != nullptr
                && surviving_child_incarnation_at_cancel != 0
                && parent->named_cancel_surviving_exit_incarnation
                    == surviving_child_incarnation_at_cancel
                && child->replaced_order_incarnation
                    == surviving_child_incarnation_at_cancel;
        } else if (bar_index_ == 2) {
            position_seen_on_trigger_bar = signed_position_size();
        }
    }

    bool priority_attached() const { return adapter_.priority.attached(); }
    bool priority_enabled() const { return adapter_.priority.retained_parent_first(); }
    bool cap_attached() const {
        return adapter_.cap.attachment() != compat::pine::CapAttachment::None;
    }
    uint64_t fills() const { return fixture_applied_receipt_count(); }
    double position() const { return signed_position_size(); }
    bool fresh_parent_shape_seen = false;
    bool parent_cancel_provenance_seen = false;
    bool parent_cancel_token_exact = false;
    bool parent_cancel_child_token_exact = false;
    bool cancel_token_consumed = false;
    bool parent_then_child_incarnations = false;
    bool child_reissue_provenance_seen = false;
    bool child_replacement_token_exact = false;
    uint64_t cancelled_parent_incarnation = 0;
    uint64_t surviving_child_incarnation_at_cancel = 0;
    std::size_t pending_book_size_on_reissue = 0;
    double position_seen_on_trigger_bar = kNaN;

private:
    Cell cell_;
    BookVariant variant_;
};

class CancelTokenScopeProbe final : public pineforge::source::PineStrategyHost {
public:
    CancelTokenScopeProbe() {
        process_orders_on_close_ = true;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("E", true, kNaN, 130.0, kNaN,
                           "cancelled parent");
            strategy_exit("X", "E", 130.0, 90.0,
                          kNaN, kNaN, kNaN, 100.0,
                          "surviving child");
            for (const PendingOrder& order : pending_orders_) {
                if (order.type == OrderType::ENTRY && order.id == "E") {
                    cancelled_incarnation = order.incarnation;
                }
                if (order.type == OrderType::EXIT && order.id == "X") {
                    surviving_child_incarnation = order.incarnation;
                }
            }
            strategy_cancel("E");
            same_eval_token_seen = cancelled_incarnation != 0
                && surviving_child_incarnation != 0
                && l4c_named_entry_cancel_active("E");
        } else if (bar_index_ == 1) {
            token_cleared_before_next_eval = !l4c_named_entry_cancel_active("E");
            strategy_entry("E", true, kNaN, 130.0, kNaN,
                           "later fresh parent");
            for (const PendingOrder& order : pending_orders_) {
                if (order.type == OrderType::ENTRY && order.id == "E") {
                    later_parent_has_no_token =
                        order.recreated_after_named_cancelled_entry_incarnation
                            == 0;
                }
            }
        }
    }

    uint64_t cancelled_incarnation = 0;
    uint64_t surviving_child_incarnation = 0;
    bool same_eval_token_seen = false;
    bool token_cleared_before_next_eval = false;
    bool later_parent_has_no_token = false;
};

enum class SortMutation {
    ExactDefaultOn,
    FactorOff,
    BrokerLive,
    NonPooc,
    CalcOnFills,
    CoofScheduler,
    Magnifier,
    StreamWarmup,
    StreamRealtime,
    ParentReplacement,
    MissingCancelToken,
    MissingSurvivingChildToken,
    MismatchedChildReplacementToken,
    CancelTokenEqualsParent,
    CancelTokenEqualsChild,
    ParentCreatedLive,
    ChildCreatedLive,
    ParentAfterClose,
    ChildAfterClose,
    ParentStopLimitActivated,
    ChildDifferentCreatedBar,
    ParentMissingStop,
    ParentHasLimit,
    ExplicitParentQty,
    ExplicitChildQty,
    FreshChild,
    ChildRequestedPartial,
    ChildPercentPartial,
    TrailingChild,
    ChildOcaName,
    ChildOcaType,
    ProfitRelativeChild,
    LossRelativeChild,
    MismatchedFromEntry,
    ChildZeroIncarnation,
    ParentZeroIncarnation,
    EqualIncarnations,
    ChildReissuedBeforeParent,
    InterveningIncarnation,
    NormalSourceOrder,
};

static bool retained_child_predicate_accepts(SortMutation mutation) {
    compat::pine::OrderPriority policy;
    policy.attach();
    compat::pine::OrderPriorityContext context{
        true,  // broker flat
        true,  // POOC
        false, // COOF option
        false, // COOF scheduler
        false, // magnifier
        false, // stream warmup
        true,  // stream idle
        2,
    };

    compat::pine::OrderPriorityCandidate child;
    child.handle.incarnation = 12;
    child.kind = compat::pine::OrderPriorityKind::Exit;
    child.id = "X";
    child.from_entry = "E";
    child.source_sequence = 1;
    child.predecessor = 10;
    child.created_bar = 1;
    child.created_flat = true;
    child.requested_qty = kNaN;
    child.qty_percent = 100.0;
    child.stop = 90.0;
    child.limit = 130.0;

    compat::pine::OrderPriorityCandidate parent;
    parent.handle.incarnation = 11;
    parent.kind = compat::pine::OrderPriorityKind::Entry;
    parent.id = "E";
    parent.source_sequence = 2;
    parent.recreated_after_named_cancelled = 9;
    parent.named_cancel_surviving_exit = 10;
    parent.created_bar = 1;
    parent.created_flat = true;
    parent.default_quantity = true;
    parent.stop = 110.0;

    switch (mutation) {
        case SortMutation::ExactDefaultOn:
            break;
        case SortMutation::FactorOff:
            policy.metadata("flat_retained_child_fresh_parent_order", 0.0);
            break;
        case SortMutation::BrokerLive:
            context.broker_flat = false;
            break;
        case SortMutation::NonPooc:
            context.process_orders_on_close = false;
            break;
        case SortMutation::CalcOnFills:
            context.calc_on_order_fills = true;
            break;
        case SortMutation::CoofScheduler:
            context.coof_scheduler_active = true;
            break;
        case SortMutation::Magnifier:
            context.bar_magnifier_enabled = true;
            break;
        case SortMutation::StreamWarmup:
            context.stream_warmup_mode = true;
            break;
        case SortMutation::StreamRealtime:
            context.stream_idle = false;
            break;
        case SortMutation::ParentReplacement:
            parent.predecessor = 1;
            break;
        case SortMutation::MissingCancelToken:
            parent.recreated_after_named_cancelled = 0;
            break;
        case SortMutation::MissingSurvivingChildToken:
            parent.named_cancel_surviving_exit = 0;
            break;
        case SortMutation::MismatchedChildReplacementToken:
            child.predecessor = 8;
            break;
        case SortMutation::CancelTokenEqualsParent:
            parent.recreated_after_named_cancelled = parent.handle.incarnation;
            break;
        case SortMutation::CancelTokenEqualsChild:
            parent.recreated_after_named_cancelled = child.handle.incarnation;
            break;
        case SortMutation::ParentCreatedLive:
            parent.created_flat = false;
            break;
        case SortMutation::ChildCreatedLive:
            child.created_flat = false;
            break;
        case SortMutation::ParentAfterClose:
            parent.prior_close = true;
            break;
        case SortMutation::ChildAfterClose:
            child.prior_close = true;
            break;
        case SortMutation::ParentStopLimitActivated:
            parent.stop_limit_activated = true;
            break;
        case SortMutation::ChildDifferentCreatedBar:
            child.created_bar = 0;
            break;
        case SortMutation::ParentMissingStop:
            parent.stop = kNaN;
            break;
        case SortMutation::ParentHasLimit:
            parent.limit = 110.0;
            break;
        case SortMutation::ExplicitParentQty:
            parent.default_quantity = false;
            break;
        case SortMutation::ExplicitChildQty:
            child.requested_qty = 1.0;
            break;
        case SortMutation::FreshChild:
            child.predecessor = 0;
            break;
        case SortMutation::ChildRequestedPartial:
            child.requested_qty = 1.0;
            break;
        case SortMutation::ChildPercentPartial:
            child.qty_percent = 50.0;
            break;
        case SortMutation::TrailingChild:
            child.trail_points = 10.0;
            break;
        case SortMutation::ChildOcaName:
            child.oca_name = "group";
            break;
        case SortMutation::ChildOcaType:
            child.oca_type = 1;
            break;
        case SortMutation::ProfitRelativeChild:
            child.profit_ticks = 10.0;
            break;
        case SortMutation::LossRelativeChild:
            child.loss_ticks = 10.0;
            break;
        case SortMutation::MismatchedFromEntry:
            child.from_entry = "OTHER";
            break;
        case SortMutation::ChildZeroIncarnation:
            child.handle.incarnation = 0;
            break;
        case SortMutation::ParentZeroIncarnation:
            parent.handle.incarnation = 0;
            break;
        case SortMutation::EqualIncarnations:
            parent.handle.incarnation = child.handle.incarnation;
            break;
        case SortMutation::ChildReissuedBeforeParent:
            parent.handle.incarnation = 12;
            child.handle.incarnation = 11;
            break;
        case SortMutation::InterveningIncarnation:
            child.handle.incarnation = 13;
            break;
        case SortMutation::NormalSourceOrder:
            child.source_sequence = 2;
            parent.source_sequence = 1;
            break;
    }
    return policy.select(context, {parent, child}).has_value();
}

static Bar bar(double o, double h, double l, double c, int64_t ts) {
    return {o, h, l, c, 1'000.0, ts};
}

static void check_cell(Cell cell, bool is_long, bool pre_entry_touch) {
    RestingBracketProbe probe(cell);
    Bar bars[3] = {
        bar(100.0, 101.0, 99.0, 100.0, 900'000),
        // LongPre:  O->L->H->C, SL 90 before entry 110.
        // LongPost: O->H->L->C, entry 110 before SL 90.
        // ShortPre: O->H->L->C, SL 110 before entry 90.
        // ShortPost:O->L->H->C, entry 90 before SL 110.
        cell == Cell::LongPre
            ? bar(100.0, 120.0, 80.0, 105.0, 1'800'000)
            : cell == Cell::LongPost
                ? bar(100.0, 115.0, 80.0, 105.0, 1'800'000)
                : cell == Cell::ShortPre
                    ? bar(100.0, 115.0, 80.0, 95.0, 1'800'000)
                    : bar(100.0, 120.0, 85.0, 95.0, 1'800'000),
        is_long
            ? bar(105.0, 108.0, 85.0, 95.0, 2'700'000)
            : bar(95.0, 115.0, 90.0, 100.0, 2'700'000),
    };

    probe.run(bars, 3);

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& trade = probe.get_trade(0);
    CHECK(trade.is_long == is_long);
    CHECK(near(trade.entry_price, is_long ? 110.0 : 90.0));
    CHECK(near(trade.exit_price, is_long ? 90.0 : 110.0));
    CHECK(trade.entry_bar_index == 1);
    CHECK(trade.exit_bar_index == (pre_entry_touch ? 2 : 1));
}

static void check_fresh_parent_cell(Cell cell, bool is_long,
                                    bool pre_entry_touch,
                                    int parent_first_factor) {
    FreshParentProbe probe(cell, parent_first_factor);
    Bar bars[4] = {
        bar(100.0, 101.0, 99.0, 100.0, 900'000),
        bar(100.0, 105.0, 95.0, 100.0, 1'800'000),
        cell == Cell::LongPre
            ? bar(100.0, 120.0, 80.0, 105.0, 2'700'000)
            : cell == Cell::LongPost
                ? bar(100.0, 115.0, 80.0, 105.0, 2'700'000)
                : cell == Cell::ShortPre
                    ? bar(100.0, 115.0, 80.0, 95.0, 2'700'000)
                    : bar(100.0, 120.0, 85.0, 95.0, 2'700'000),
        is_long
            ? bar(105.0, 108.0, 85.0, 95.0, 3'600'000)
            : bar(95.0, 115.0, 90.0, 100.0, 3'600'000),
    };

    probe.run(bars, 4);

    CHECK(probe.last_error().empty());
    CHECK(probe.fresh_parent_shape_seen);
    CHECK(probe.parent_cancel_provenance_seen);
    CHECK(probe.parent_cancel_token_exact);
    CHECK(probe.parent_cancel_child_token_exact);
    CHECK(probe.cancel_token_consumed);
    CHECK(probe.parent_then_child_incarnations);
    CHECK(probe.child_reissue_provenance_seen);
    CHECK(probe.child_replacement_token_exact);
    CHECK(probe.pending_book_size_on_reissue == 2);
    const double signed_open_qty = is_long ? 1.0 : -1.0;
    const bool parent_first_enabled = parent_first_factor != 0;
    const double expected_visible_qty =
        parent_first_enabled && !pre_entry_touch ? 0.0 : signed_open_qty;
    CHECK(near(probe.position_seen_on_trigger_bar, expected_visible_qty));
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& trade = probe.get_trade(0);
    CHECK(trade.is_long == is_long);
    CHECK(near(trade.entry_price, is_long ? 110.0 : 90.0));
    CHECK(near(trade.exit_price, is_long ? 90.0 : 110.0));
    CHECK(trade.entry_bar_index == 2);
    CHECK(trade.exit_bar_index == (pre_entry_touch ? 3 : 2));
}

static void check_ambiguous_book_is_inert(BookVariant variant, bool is_long) {
    const Cell cell = is_long ? Cell::LongPost : Cell::ShortPost;
    FreshParentProbe probe(cell, /*parent_first_factor=*/true, variant);
    Bar bars[4] = {
        bar(100.0, 101.0, 99.0, 100.0, 900'000),
        bar(100.0, 105.0, 95.0, 100.0, 1'800'000),
        is_long
            ? bar(100.0, 115.0, 80.0, 105.0, 2'700'000)
            : bar(100.0, 120.0, 85.0, 95.0, 2'700'000),
        is_long
            ? bar(105.0, 108.0, 85.0, 95.0, 3'600'000)
            : bar(95.0, 115.0, 90.0, 100.0, 3'600'000),
    };

    probe.run(bars, 4);

    CHECK(probe.last_error().empty());
    CHECK(probe.fresh_parent_shape_seen);
    if (variant == BookVariant::InterleavedThird) {
        CHECK(probe.parent_cancel_provenance_seen);
        CHECK(probe.parent_cancel_token_exact);
        CHECK(probe.parent_cancel_child_token_exact);
    } else {
        CHECK(!probe.parent_cancel_provenance_seen);
        CHECK(!probe.parent_cancel_token_exact);
        CHECK(!probe.parent_cancel_child_token_exact);
    }
    CHECK(probe.cancel_token_consumed);
    CHECK(probe.parent_then_child_incarnations);
    CHECK(probe.child_reissue_provenance_seen);
    CHECK(probe.child_replacement_token_exact
          == (variant == BookVariant::InterleavedThird));
    CHECK(probe.pending_book_size_on_reissue == 3);
    CHECK(near(probe.position_seen_on_trigger_bar, is_long ? 1.0 : -1.0));
    CHECK(probe.trade_count() == 1);
}

static void check_missing_provenance_is_inert(BookVariant variant,
                                               bool is_long) {
    const Cell cell = is_long ? Cell::LongPost : Cell::ShortPost;
    FreshParentProbe probe(cell, /*parent_first_factor=*/true, variant);
    Bar bars[4] = {
        bar(100.0, 101.0, 99.0, 100.0, 900'000),
        bar(100.0, 105.0, 95.0, 100.0, 1'800'000),
        is_long
            ? bar(100.0, 115.0, 80.0, 105.0, 2'700'000)
            : bar(100.0, 120.0, 85.0, 95.0, 2'700'000),
        is_long
            ? bar(105.0, 108.0, 85.0, 95.0, 3'600'000)
            : bar(95.0, 115.0, 90.0, 100.0, 3'600'000),
    };

    probe.run(bars, 4);

    CHECK(probe.last_error().empty());
    CHECK(probe.fresh_parent_shape_seen);
    CHECK(probe.pending_book_size_on_reissue == 2);
    CHECK(!probe.parent_cancel_provenance_seen);
    CHECK(!probe.parent_cancel_token_exact);
    CHECK(!probe.parent_cancel_child_token_exact);
    CHECK(!probe.child_replacement_token_exact);
    if (variant == BookVariant::FreshChild) {
        CHECK(!probe.child_reissue_provenance_seen);
    } else {
        CHECK(probe.child_reissue_provenance_seen);
    }
    CHECK(probe.cancel_token_consumed);
    CHECK(probe.parent_then_child_incarnations
          == (variant == BookVariant::PostCancelDoubleReissue));
    CHECK(near(probe.position_seen_on_trigger_bar, is_long ? 1.0 : -1.0));
    CHECK(probe.trade_count() == 1);
}

static void check_sort_scope_guards() {
    CHECK(retained_child_predicate_accepts(
        SortMutation::ExactDefaultOn));

    for (SortMutation mutation : {
             SortMutation::FactorOff,
             SortMutation::BrokerLive,
             SortMutation::NonPooc,
             SortMutation::CalcOnFills,
             SortMutation::CoofScheduler,
             SortMutation::Magnifier,
             SortMutation::StreamWarmup,
             SortMutation::StreamRealtime,
             SortMutation::ParentReplacement,
             SortMutation::MissingCancelToken,
             SortMutation::MissingSurvivingChildToken,
             SortMutation::MismatchedChildReplacementToken,
             SortMutation::CancelTokenEqualsParent,
             SortMutation::CancelTokenEqualsChild,
             SortMutation::ParentCreatedLive,
             SortMutation::ChildCreatedLive,
             SortMutation::ParentAfterClose,
             SortMutation::ChildAfterClose,
             SortMutation::ParentStopLimitActivated,
             SortMutation::ChildDifferentCreatedBar,
             SortMutation::ParentMissingStop,
             SortMutation::ParentHasLimit,
             SortMutation::ExplicitParentQty,
             SortMutation::ExplicitChildQty,
             SortMutation::FreshChild,
             SortMutation::ChildRequestedPartial,
             SortMutation::ChildPercentPartial,
             SortMutation::TrailingChild,
             SortMutation::ChildOcaName,
             SortMutation::ChildOcaType,
             SortMutation::ProfitRelativeChild,
             SortMutation::LossRelativeChild,
             SortMutation::MismatchedFromEntry,
             SortMutation::ChildZeroIncarnation,
             SortMutation::ParentZeroIncarnation,
             SortMutation::EqualIncarnations,
             SortMutation::ChildReissuedBeforeParent,
             SortMutation::InterveningIncarnation,
             SortMutation::NormalSourceOrder,
         }) {
        CHECK(!retained_child_predicate_accepts(mutation));
    }
}

static void check_cancel_token_scope() {
    CancelTokenScopeProbe probe;
    Bar bars[2] = {
        bar(100.0, 101.0, 99.0, 100.0, 900'000),
        bar(100.0, 101.0, 99.0, 100.0, 1'800'000),
    };
    probe.run(bars, 2);
    CHECK(probe.last_error().empty());
    CHECK(probe.cancelled_incarnation != 0);
    CHECK(probe.surviving_child_incarnation != 0);
    CHECK(probe.same_eval_token_seen);
    CHECK(probe.token_cleared_before_next_eval);
    CHECK(probe.later_parent_has_no_token);
}

// Public command controls distinguish policy extraction from native activation.
static void check_explicit_attachment_boundary() {
    const Bar bars[] = {
        bar(100,101,99,100,900000), bar(100,105,95,100,1800000),
        bar(100,115,80,105,2700000), bar(105,108,85,95,3600000),
    };
    const char* key = "flat_retained_child_fresh_parent_order";
    const double values[] = {0.0, -0.0, -1.0, kNaN,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), 0.5, 1.0};
    for (double value : values) {
        for (bool attached : {false, true}) {
            FreshParentProbe probe(Cell::LongPost, -1, BookVariant::ExactPair, attached);
            // Exercise the actual base/C transport, not a derived shadow setter.
            strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&probe), key, value);
            if (!attached) probe.enable_pine_intraday_cap(); // cap-only is not execution attachment
            probe.run(bars, 4);
            CHECK(probe.priority_attached() == attached);
            CHECK(probe.cap_attached());
            const bool enabled = std::isfinite(value) && value > 0.0;
            CHECK(probe.priority_enabled() == enabled);
            CHECK(near(probe.position_seen_on_trigger_bar, attached && enabled ? 0 : 1));
            CHECK(probe.trade_count() == 1);
            CHECK(probe.fills() == 2);
            if (probe.trade_count() == 1) {
                CHECK(near(probe.get_trade(0).entry_price, 110));
                CHECK(near(probe.get_trade(0).exit_price, 90));
                CHECK(probe.get_trade(0).exit_bar_index == 2);
            }
        }
    }
    FreshParentProbe bare(Cell::LongPost, -1, BookVariant::ExactPair, false);
    CHECK(!bare.cap_attached());
    CHECK(!bare.priority_attached());
    bare.run(bars, 4);
    CHECK(near(bare.position_seen_on_trigger_bar, 1));
    CHECK(bare.trade_count() == 1 && bare.fills() == 2);

    for (BookVariant variant : {BookVariant::InterleavedFourth,
             BookVariant::IncarnationGap, BookVariant::ChildOca, BookVariant::SharedOca}) {
        FreshParentProbe probe(Cell::LongPost, -1, variant);
        probe.run(bars, 4);
        CHECK(probe.priority_attached());
        CHECK(near(probe.position_seen_on_trigger_bar, 1));
        CHECK(probe.pending_book_size_on_reissue ==
              (variant == BookVariant::InterleavedFourth ? 4u : 2u));
        CHECK(probe.parent_then_child_incarnations == (variant != BookVariant::IncarnationGap));
        if (variant == BookVariant::SharedOca) {
            CHECK(probe.trade_count() == 0 && probe.fills() == 1);
            CHECK(near(probe.position(), 1));
        } else {
            CHECK(probe.trade_count() == 1 && probe.fills() == 2);
            if (probe.trade_count() == 1) {
                CHECK(near(probe.get_trade(0).entry_price, 110));
                CHECK(near(probe.get_trade(0).exit_price, 90));
            }
        }
    }

    FreshParentProbe source(Cell::LongPost, -1, BookVariant::ExactPair, false);
    const auto native_hash = source.broker_state_hash();
    source.enable_pine_intraday_cap();
    const auto cap_hash = source.broker_state_hash();
    source.attach_pine_execution_adapter();
    CHECK(native_hash != cap_hash);
    CHECK(cap_hash != source.broker_state_hash()); // priority attachment has its own hash
    const auto attached_hash = source.broker_state_hash();
    source.set_syminfo_metadata(key, 0.0);
    CHECK(attached_hash != source.broker_state_hash());
    source.attach_pine_execution_adapter(); // idempotent, must preserve off
    CHECK(!source.priority_enabled());
    source.run(bars, 4);
    FreshParentProbe copy(Cell::LongPost, -1, BookVariant::ExactPair, false);
    copy.enable_pine_intraday_cap();
    copy.attach_pine_execution_adapter();
    copy.set_syminfo_metadata(key, 0.0);
    copy.run(bars, 4);
    CHECK(copy.broker_state_hash() == source.broker_state_hash());
    copy.run(nullptr, 0);
    CHECK(copy.priority_attached() && !copy.priority_enabled());
    copy.set_syminfo_metadata(key, 1.0);
    CHECK(!source.priority_enabled());
    copy.run(bars, 4);
    CHECK(near(copy.position_seen_on_trigger_bar, 0));
    CHECK(near(source.position_seen_on_trigger_bar, 1));

    FreshParentProbe delayed(Cell::LongPost, 0, BookVariant::ExactPair, false);
    delayed.attach_pine_execution_adapter(); // previously supplied off stays off
    CHECK(!delayed.priority_enabled());
    delayed.run(bars, 4);
    CHECK(near(delayed.position_seen_on_trigger_bar, 1));

    const broker::OrderPriorityDecision decision{{{{11, 2}, {12, 3}}}};
    CHECK(decision.sequence(11, 3) == 2);
    CHECK(decision.sequence(12, 2) == 3);
    CHECK(decision.sequence(13, 7) == 7); // replacement cannot inherit by priority
}

int main() {
    std::printf("pre-armed from_entry bracket path cursor\n");
    check_cell(Cell::LongPre, true, true);
    check_cell(Cell::LongPost, true, false);
    check_cell(Cell::ShortPre, false, true);
    check_cell(Cell::ShortPost, false, false);

    std::printf("retained child / fresh parent broker order\n");
    for (bool factor : {false, true}) {
        check_fresh_parent_cell(Cell::LongPre, true, true, factor);
        check_fresh_parent_cell(Cell::LongPost, true, false, factor);
        check_fresh_parent_cell(Cell::ShortPre, false, true, factor);
        check_fresh_parent_cell(Cell::ShortPost, false, false, factor);
    }
    check_fresh_parent_cell(Cell::LongPost, true, false,
                            /*production default=*/-1);
    check_fresh_parent_cell(Cell::ShortPost, false, false,
                            /*production default=*/-1);

    std::printf("ambiguous retained-child books stay inert\n");
    for (bool is_long : {true, false}) {
        check_ambiguous_book_is_inert(BookVariant::InterleavedThird, is_long);
        check_ambiguous_book_is_inert(BookVariant::MultipleChildren, is_long);
    }

    std::printf("missing lifecycle provenance stays inert\n");
    for (bool is_long : {true, false}) {
        check_missing_provenance_is_inert(BookVariant::FreshChild, is_long);
        check_missing_provenance_is_inert(
            BookVariant::PostCancelDoubleReissue, is_long);
        check_missing_provenance_is_inert(
            BookVariant::MissingParentCancel, is_long);
    }

    std::printf("exact scope guards and default-on behavior\n");
    check_sort_scope_guards();
    check_cancel_token_scope();
    check_explicit_attachment_boundary();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
