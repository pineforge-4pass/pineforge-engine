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
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Cell { LongPre, LongPost, ShortPre, ShortPost };

class RestingBracketProbe final : public BacktestEngine {
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

    void on_bar(const Bar&) override {
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
    MultipleChildren,
};

class FreshParentProbe final : public BacktestEngine {
public:
    FreshParentProbe(Cell cell, int parent_first_factor,
                     BookVariant variant = BookVariant::ExactPair)
        : cell_(cell), variant_(variant) {
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

    void on_bar(const Bar&) override {
        const bool is_long =
            cell_ == Cell::LongPre || cell_ == Cell::LongPost;
        const double child_stop = is_long ? 90.0 : 110.0;
        const double child_limit = is_long ? 130.0 : 70.0;
        if (bar_index_ == 0) {
            strategy_entry("E", is_long, kNaN,
                           is_long ? 130.0 : 70.0,
                           kNaN, "original parent");
            if (variant_ != BookVariant::FreshChild
                && variant_ != BookVariant::PostCancelDoubleReissue) {
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "retained child");
            }
            if (variant_ == BookVariant::InterleavedThird) {
                strategy_entry("U", is_long, kNaN,
                               is_long ? 1'000.0 : 1.0,
                               1.0, "unrelated resting parent");
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
                pending_orders_.erase(
                    std::remove_if(
                        pending_orders_.begin(), pending_orders_.end(),
                        [](const PendingOrder& order) {
                            return order.type == OrderType::ENTRY
                                && order.id == "E";
                        }),
                    pending_orders_.end());
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
                               kNaN, "fresh parent");
                strategy_exit("X", "E", child_limit, child_stop,
                              kNaN, kNaN, kNaN, 100.0,
                              "retained child");
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
                && !parent->created_by_same_id_replacement
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
            cancel_token_consumed =
                named_entry_cancelled_incarnation_in_current_eval_.find("E")
                    == named_entry_cancelled_incarnation_in_current_eval_.end();
            parent_then_child_incarnations = parent != nullptr
                && child != nullptr
                && parent->incarnation
                    < std::numeric_limits<uint64_t>::max()
                && child->incarnation == parent->incarnation + 1;
            child_reissue_provenance_seen = child != nullptr
                && child->created_by_same_id_replacement;
            child_replacement_token_exact = child != nullptr
                && parent != nullptr
                && surviving_child_incarnation_at_cancel != 0
                && parent->named_cancel_surviving_exit_incarnation
                    == surviving_child_incarnation_at_cancel
                && child->replaced_exit_order_incarnation
                    == surviving_child_incarnation_at_cancel;
        } else if (bar_index_ == 2) {
            position_seen_on_trigger_bar = signed_position_size();
        }
    }

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

class CancelTokenScopeProbe final : public BacktestEngine {
public:
    CancelTokenScopeProbe() {
        process_orders_on_close_ = true;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
    }

    void on_bar(const Bar&) override {
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
            const auto token =
                named_entry_cancelled_incarnation_in_current_eval_.find("E");
            same_eval_token_seen = token
                != named_entry_cancelled_incarnation_in_current_eval_.end()
                && token->second.entry_incarnation == cancelled_incarnation
                && token->second.surviving_exit_incarnation
                    == surviving_child_incarnation;
        } else if (bar_index_ == 1) {
            token_cleared_before_next_eval =
                named_entry_cancelled_incarnation_in_current_eval_.empty();
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
    internal::RetainedChildFreshParentOrderContext context{
        true,  // enabled
        true,  // broker flat
        true,  // POOC
        false, // COOF option
        false, // COOF scheduler
        false, // magnifier
        false, // stream warmup
        true,  // stream idle
        2,
        2,
    };

    PendingOrder child{};
    child.id = "X";
    child.from_entry = "E";
    child.type = OrderType::EXIT;
    child.created_seq = 1;
    child.incarnation = 12;
    child.created_by_same_id_replacement = true;
    child.replaced_exit_order_incarnation = 10;
    child.created_bar = 1;
    child.created_position_side = PositionSide::FLAT;
    child.qty = kNaN;
    child.qty_percent = 100.0;
    child.stop_price = 90.0;
    child.limit_price = 130.0;
    child.profit_ticks = kNaN;
    child.loss_ticks = kNaN;
    child.trail_points = kNaN;
    child.trail_price = kNaN;
    child.trail_offset = kNaN;

    PendingOrder parent{};
    parent.id = "E";
    parent.type = OrderType::ENTRY;
    parent.created_seq = 2;
    parent.incarnation = 11;
    parent.recreated_after_named_cancelled_entry_incarnation = 9;
    parent.named_cancel_surviving_exit_incarnation = 10;
    parent.created_bar = 1;
    parent.created_position_side = PositionSide::FLAT;
    parent.qty = kNaN;
    parent.stop_price = 110.0;
    parent.limit_price = kNaN;
    parent.trail_points = kNaN;
    parent.trail_price = kNaN;
    parent.trail_offset = kNaN;

    switch (mutation) {
        case SortMutation::ExactDefaultOn:
            break;
        case SortMutation::FactorOff:
            context.enabled = false;
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
            parent.created_by_same_id_replacement = true;
            break;
        case SortMutation::MissingCancelToken:
            parent.recreated_after_named_cancelled_entry_incarnation = 0;
            break;
        case SortMutation::MissingSurvivingChildToken:
            parent.named_cancel_surviving_exit_incarnation = 0;
            break;
        case SortMutation::MismatchedChildReplacementToken:
            child.replaced_exit_order_incarnation = 8;
            break;
        case SortMutation::CancelTokenEqualsParent:
            parent.recreated_after_named_cancelled_entry_incarnation =
                parent.incarnation;
            break;
        case SortMutation::CancelTokenEqualsChild:
            parent.recreated_after_named_cancelled_entry_incarnation =
                child.incarnation;
            break;
        case SortMutation::ParentCreatedLive:
            parent.created_position_side = PositionSide::LONG;
            break;
        case SortMutation::ChildCreatedLive:
            child.created_position_side = PositionSide::LONG;
            break;
        case SortMutation::ParentAfterClose:
            parent.created_after_position_close_in_bar = true;
            break;
        case SortMutation::ChildAfterClose:
            child.created_after_position_close_in_bar = true;
            break;
        case SortMutation::ParentStopLimitActivated:
            parent.stop_limit_activated = true;
            break;
        case SortMutation::ChildDifferentCreatedBar:
            child.created_bar = 0;
            break;
        case SortMutation::ParentMissingStop:
            parent.stop_price = kNaN;
            break;
        case SortMutation::ParentHasLimit:
            parent.limit_price = 110.0;
            break;
        case SortMutation::ExplicitParentQty:
            parent.qty = 1.0;
            break;
        case SortMutation::ExplicitChildQty:
            child.qty = 1.0;
            break;
        case SortMutation::FreshChild:
            child.created_by_same_id_replacement = false;
            break;
        case SortMutation::ChildRequestedPartial:
            child.requested_partial = true;
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
            child.incarnation = 0;
            break;
        case SortMutation::ParentZeroIncarnation:
            parent.incarnation = 0;
            break;
        case SortMutation::EqualIncarnations:
            parent.incarnation = child.incarnation;
            break;
        case SortMutation::ChildReissuedBeforeParent:
            parent.incarnation = 12;
            child.incarnation = 11;
            break;
        case SortMutation::InterveningIncarnation:
            child.incarnation = 13;
            break;
        case SortMutation::NormalSourceOrder:
            child.created_seq = 2;
            parent.created_seq = 1;
            break;
    }
    return internal::retained_child_fresh_parent_order_pair(
        context, &parent, &child);
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

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
