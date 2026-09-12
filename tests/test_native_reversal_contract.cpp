// Direct native reversal contract witnesses.
//
// This file deliberately does not call BacktestEngine::run, feed a bar
// sequence, compile generated strategy code, or consume a reference tape.
// The first group records the contract that a future native reversal seam must
// satisfy.  On d3996b4 these checks are expected to fail: they are failing-
// before witnesses for the migration, not a compatibility claim about
// TradingView output.  The purge flag checks characterize an existing caller
// contract and must remain stable while the seam is introduced.

#include <pineforge/engine.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(value) do { \
    ++checks; \
    if (!(value)) { \
        ++failures; \
        std::printf("FAIL %d: %s\n", __LINE__, #value); \
    } \
} while (0)

void near(double actual, double expected, double tolerance = 1e-12) {
    CHECK(std::isfinite(actual) && std::abs(actual - expected) <= tolerance);
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
}

template<class Tag, auto Member>
struct Access {
    friend auto access(Tag) { return Member; }
};

struct CloseOppositeAccess { friend auto access(CloseOppositeAccess); };
struct MarketEntryAccess { friend auto access(MarketEntryAccess); };
struct SettleAccess { friend auto access(SettleAccess); };
struct EffectsSettleAccess { friend auto access(EffectsSettleAccess); };
struct SelectPreCloseAccess { friend auto access(SelectPreCloseAccess); };
struct LegRevisionAccess { friend auto access(LegRevisionAccess); };

template struct Access<CloseOppositeAccess,
                       &BacktestEngine::close_opposite_then_enter>;
template struct Access<MarketEntryAccess,
                       &BacktestEngine::execute_market_entry>;
template struct Access<SettleAccess,
                       &BacktestEngine::settle_resolved_execution>;
template struct Access<EffectsSettleAccess,
                       &BacktestEngine::settle_execution_with_lifecycle>;
template struct Access<SelectPreCloseAccess,
                       &BacktestEngine::select_declined_reversal_pre_close>;
template struct Access<LegRevisionAccess, &exit_legs::Lifecycle::revision_>;

class Book final : public BacktestEngine {
public:
    Book() {
        initial_capital_ = 10'000.0;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 6.0;
        syminfo_mintick_ = 0.01;
        slippage_ = 0;
        current_bar_ = {100.0, 125.0, 95.0, 120.0, 1.0, 60'000};
        bar_index_ = 1;
    }

    void on_bar(const Bar&) override {}

    // A two-lot LONG book with historical entry tickets already paid.  The
    // physical roster is authoritative; the aggregate fields are projections
    // needed by the legacy helper under test.
    void seed_two_lots(PositionSide side = PositionSide::LONG) {
        position_side_ = side;
        position_cycle_seq_ = 4;
        next_position_cycle_seq_ = 5;
        position_entry_price_ = 106.0;
        position_qty_ = 5.0;
        position_entry_count_ = 2;
        position_open_bar_ = 0;
        pyramid_entries_.clear();

        PyramidEntry first{100.0, 1'000, 2.0, "A", 0};
        first.entry_incarnation = 11;
        first.entry_commission_account = 6.0;
        PyramidEntry second{110.0, 2'000, 3.0, "B", 0};
        second.entry_incarnation = 12;
        second.entry_commission_account = 6.0;
        pyramid_entries_.push_back(first);
        pyramid_entries_.push_back(second);

        id_unclosed_qty_.clear();
        id_unclosed_qty_["A"] = 2.0;
        id_unclosed_qty_["B"] = 3.0;
        cycle_filled_entry_ids_.clear();
        cycle_filled_entry_ids_.insert("A");
        cycle_filled_entry_ids_.insert("B");
        trades_.clear();
        range_end_trades_.clear();
        net_profit_sum_ = 0.0;
        gross_profit_sum_ = 0.0;
        gross_loss_sum_ = 0.0;
        intraday_pnl_ = 0.0;
        win_trades_count_ = 0;
        loss_trades_count_ = 0;
        eventrades_count_ = 0;
    }

    void add_stale_exit() {
        PendingOrder stale{};
        stale.id = "stale-exit";
        stale.from_entry = "A";
        stale.type = OrderType::EXIT;
        stale.incarnation = 500;
        stale.legs.attach(stale.incarnation, position_cycle_seq_);
        pending_orders_.push_back(std::move(stale));
    }

    size_t pending_count() const { return pending_orders_.size(); }
    double position() const { return signed_position_size(); }
    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
    const std::vector<Trade>& trades() const { return trades_; }
    uint64_t fingerprint() const { return broker_state_hash(); }
    void set_slippage(int ticks) { slippage_ = ticks; }

    void set_limit_fill(bool enabled) { current_fill_is_limit_ = enabled; }
    void observe_actions(bool enabled) { stream_observe_actions_ = enabled; }
    void exhaust_stream_actions() {
        stream_action_sequence_ = std::numeric_limits<uint64_t>::max();
    }
    void exhaust_lifecycle_events() {
        exit_leg_event_seq_ = std::numeric_limits<uint64_t>::max() - 1;
    }
    void exhaust_trade_wins() {
        win_trades_count_ = std::numeric_limits<int>::max();
    }
    uint64_t lifecycle_events() const { return exit_leg_event_seq_; }
    uint64_t lifecycle_revision() const {
        return pending_orders_.empty() ? 0 : pending_orders_.front().legs.revision();
    }
    int64_t lifecycle_owner() const {
        return pending_orders_.empty() ? -1 : pending_orders_.front().legs.target().owner;
    }
    PendingOrder* first_pending_address() {
        return pending_orders_.empty() ? nullptr : &pending_orders_.front();
    }
    bool lifecycle_last_is_bind_to(int64_t owner) const {
        if (pending_orders_.empty() || !pending_orders_.front().legs.last_action()) return false;
        const auto& op = pending_orders_.front().legs.last_action()->operation;
        const auto* bind = std::get_if<exit_legs::BindOwner>(&op);
        return bind && bind->owner == owner;
    }

    execution::Result settle_reduce(double units, double price = 120.0,
                                    std::optional<double> fee = std::nullopt) {
        execution::Fill fill{price, "N", "native", 901, fee};
        return (this->*access(SettleAccess{}))(
            order_action::Reduce{units}, fill);
    }

    execution::Result settle_flatten(double price = 120.0) {
        execution::Fill fill{price, "N", "native", 902};
        return (this->*access(SettleAccess{}))(
            execution::Flatten{}, fill);
    }

    execution::Result settle_effects(const execution::Action& action,
                                     const execution::LifecycleEffects& effects) {
        return (this->*access(EffectsSettleAccess{}))(
            action, execution::Fill{120.0, "N", "native", 903}, effects);
    }
    std::optional<execution::LifecycleBatch> selected_pre_close() const {
        return (this->*access(SelectPreCloseAccess{}))(current_bar_);
    }
    void set_leg_owner(int64_t owner) {
        exit_legs::Lifecycle replacement;
        replacement.attach(pending_orders_.front().incarnation, owner);
        pending_orders_.front().legs = std::move(replacement);
    }
    void set_leg_incarnation(uint64_t incarnation, int64_t owner) {
        exit_legs::Lifecycle replacement;
        replacement.attach(incarnation, owner);
        pending_orders_.front().legs = std::move(replacement);
    }
    void bind_current_cycle_activation() {
        pending_orders_.front().leg_activation.bind({position_cycle_seq_, 5, 5});
    }
    void exhaust_leg_revision() {
        pending_orders_.front().legs.*access(LegRevisionAccess{}) = UINT64_MAX;
    }
    void set_lifecycle_events(uint64_t value) { exit_leg_event_seq_ = value; }
    const exit_legs::Lifecycle& first_legs() const { return pending_orders_.front().legs; }
    void define_stop() { pending_orders_.front().legs.set_stop_price(90.0); }
    void add_second_exit() {
        PendingOrder order{};
        order.id = "second-exit";
        order.from_entry = "B";
        order.type = OrderType::EXIT;
        order.incarnation = 501;
        order.created_seq = 1;
        order.legs.attach(order.incarnation, position_cycle_seq_);
        pending_orders_.push_back(std::move(order));
    }
    void seed_prior_leg_event() {
        auto& legs = pending_orders_.front().legs;
        const exit_legs::Frame frame{7, 1, exit_legs::Domain::Ordinary,
                                     exit_legs::Phase::Observation};
        const exit_legs::Action action{legs.target(), legs.revision(), frame,
                                       exit_legs::BindOwner{4}};
        if (legs.apply(legs.target(), action) != exit_legs::Result::Applied)
            throw std::logic_error("invalid prior-event test fixture");
        exit_leg_event_seq_ = 7;
    }

    // Calls the legacy compatibility helper directly.  `price` is intentionally
    // the raw source price here; the helper owns one fill-side slippage step per
    // physical leg in its current implementation.
    void reverse_raw(double price, double quantity, bool purge, uint64_t incarnation,
                     bool requested_long = false) {
        (this->*access(CloseOppositeAccess{}))(
            "R", requested_long, price, quantity, -1, purge, true, incarnation);
    }

    // Reproduces the production caller's current boundary: execute_market_entry
    // applies entry slippage before delegating to close_opposite_then_enter.
    void reverse_from_market_entry(double raw_price, double quantity,
                                   uint64_t incarnation) {
        (this->*access(MarketEntryAccess{}))(
            "R", false, raw_price, quantity, -1,
            PositionSide::FLAT, true, false, 0.0, bar_index_, false, false,
            false, incarnation);
    }

    void exhaust_next_cycle() {
        next_position_cycle_seq_ = std::numeric_limits<int64_t>::max();
    }
};

// Desired native contract: one accepted reversal execution has one current
// ticket, even when FIFO closes multiple lots and opens a remainder. Historical
// entry costs are not part of this assertion; only the current execution's
// charge is summed. d3996b4's close helper charges one CASH_PER_ORDER ticket
// per closed row and another on the opening row, so this is a failing-before
// witness for the proposed native seam.
void one_ticket_multi_lot_reversal_before() {
    Book book;
    book.seed_two_lots();
    book.reverse_raw(120.0, 6.0, false, 30);

    CHECK(book.position() == -1.0);
    CHECK(book.trades().size() == 2);
    CHECK(book.lots().size() == 1);
    if (book.trades().size() == 2 && book.lots().size() == 1) {
        // Trade::commission includes the proportional historical entry cost
        // as well as the current exit charge. Remove the two already-paid
        // entry tickets before isolating this reversal's current ticket.
        const double historical_entries = 6.0 + 6.0;
        const double current_ticket = book.trades()[0].commission
            + book.trades()[1].commission
            + book.lots()[0].entry_commission_account
            - historical_entries;
        near(current_ticket, 6.0);
    }
}

// Desired native contract: cycle allocation is preflighted before any physical
// close, report row, or queue cleanup. The existing helper closes the old lots,
// purges exits, and only then discovers that opening the remainder would exhaust
// the position-cycle counter. The fingerprint catches all of those mutations.
void cycle_exhaustion_is_strong_preflight_before() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    book.exhaust_next_cycle();
    const auto before = book.fingerprint();

    bool threw = false;
    try {
        book.reverse_raw(120.0, 6.0, true, 31);
    } catch (const std::overflow_error&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(book.fingerprint() == before);
    CHECK(book.position() == 5.0);
    CHECK(book.lots().size() == 2);
    CHECK(book.trades().empty());
    CHECK(book.pending_count() == 1);
}

// Desired native boundary: the source caller resolves slippage once and passes
// that resolved execution to settlement. The current execute_market_entry path
// slips the sell price, then close_opposite_then_enter slips both physical legs
// again. This direct witness expects the one adverse sell step from 100 to 99.91
// and fails on the current 99.82 result.
void production_caller_slips_once_before() {
    Book book;
    book.seed_two_lots();
    book.set_slippage(9);
    book.reverse_from_market_entry(100.0, 6.0, 32);

    CHECK(book.trades().size() == 2);
    CHECK(book.lots().size() == 1);
    if (book.trades().size() == 2 && book.lots().size() == 1) {
        near(book.trades()[0].exit_price, 99.91);
        near(book.trades()[1].exit_price, 99.91);
        near(book.lots()[0].price, 99.91);
    }
}

// Existing caller contract characterization. The purge flag is intentionally
// explicit because process_pending_orders iterates pending_orders_: callers
// passing false retain ownership of cleanup and avoid invalidating a live
// PendingOrder reference; a direct, already-detached caller may pass true.
void purge_flag_is_explicit_characterization() {
    Book purging;
    purging.seed_two_lots();
    purging.add_stale_exit();
    purging.reverse_raw(120.0, 5.0, true, 33);
    CHECK(purging.position() == 0.0);
    CHECK(purging.pending_count() == 0);
    CHECK(purging.lifecycle_events() == 1);

    Book retained;
    retained.seed_two_lots();
    retained.add_stale_exit();
    retained.reverse_raw(120.0, 5.0, false, 34);
    CHECK(retained.position() == 0.0);
    CHECK(retained.pending_count() == 1);
}

// The reverse direction is the same native transaction with the sign flipped.
// Keep the lot roster and one-ticket invariant symmetric; a one-sided test can
// accidentally leave sell-side slippage or FIFO logic unexercised.
void one_ticket_reverse_direction() {
    Book book;
    book.seed_two_lots(PositionSide::SHORT);
    book.reverse_raw(100.0, 6.0, false, 35, true);

    CHECK(book.position() == 1.0);
    CHECK(book.trades().size() == 2);
    CHECK(book.lots().size() == 1);
    if (book.trades().size() == 2 && book.lots().size() == 1) {
        const double historical_entries = 6.0 + 6.0;
        const double current_ticket = book.trades()[0].commission
            + book.trades()[1].commission
            + book.lots()[0].entry_commission_account
            - historical_entries;
        near(current_ticket, 6.0);
    }
}

// A resolved LIMIT price is already at the broker level.  The lifecycle must
// consume it verbatim even when the engine's configured slippage is non-zero.
void resolved_limit_price_is_not_slipped() {
    Book book;
    book.seed_two_lots();
    book.set_slippage(9);
    book.set_limit_fill(true);
    book.reverse_raw(100.0, 6.0, false, 36);

    CHECK(book.trades().size() == 2);
    CHECK(book.lots().size() == 1);
    if (book.trades().size() == 2 && book.lots().size() == 1) {
        near(book.trades()[0].exit_price, 100.0);
        near(book.trades()[1].exit_price, 100.0);
        near(book.lots()[0].price, 100.0);
    }
}

// A partial native reduction never resets the position cycle, unbinds exits,
// or binds a new owner.  It is a close-only execution with one physical FIFO
// trade and no lifecycle event.
void partial_close_keeps_cycle_and_lifecycle() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    const auto events = book.lifecycle_events();
    const auto result = book.settle_reduce(2.0);

    CHECK(result.status == execution::Status::Applied);
    CHECK(book.position() == 3.0);
    CHECK(book.lots().size() == 1);
    CHECK(book.trades().size() == 1);
    CHECK(book.lifecycle_events() == events);
    CHECK(book.lifecycle_owner() == 4);
}

// A full flatten is still one resolved execution and consumes the old-cycle
// unbind event exactly once.  It does not create a new owner or activation.
void full_close_unbinds_once() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    const auto events = book.lifecycle_events();
    const auto result = book.settle_flatten();

    CHECK(result.status == execution::Status::Applied);
    CHECK(book.position() == 0.0);
    CHECK(book.lots().empty());
    CHECK(book.trades().size() == 2);
    CHECK(book.lifecycle_events() == events + 1);
    CHECK(book.lifecycle_owner() == 0);
}

// Retaining the EXIT gives the old-cycle unbind, an explicit owner rebind,
// and the requested BindOwner operation. Preserve all three existing receipts.
// The expected event/revision values are literal fixture
// facts, rather than a candidate-derived positive delta.
void retained_exit_rebinds_after_close() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    book.reverse_raw(120.0, 6.0, false, 37);

    CHECK(book.position() == -1.0);
    CHECK(book.pending_count() == 1);
    CHECK(book.lifecycle_events() == 3);
    CHECK(book.lifecycle_revision() == 3);
    CHECK(book.lifecycle_owner() == 5);
    CHECK(book.lifecycle_last_is_bind_to(5));
}

// A caller that retains cleanup ownership must not have its PendingOrder
// reference invalidated by the reversal's open leg.  The vector address is a
// direct characterization of that contract; no copied/swapped queue is
// allowed as an implementation shortcut.
void retained_cleanup_preserves_pending_address() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    PendingOrder* before = book.first_pending_address();
    book.reverse_raw(120.0, 6.0, false, 39);
    CHECK(book.first_pending_address() == before);
    CHECK(book.lifecycle_owner() == 5);
}

void lifecycle_exhaustion_precedes_mutation() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    book.exhaust_lifecycle_events();
    const auto before = book.fingerprint();
    bool threw = false;
    try {
        book.reverse_raw(120.0, 6.0, false, 38);
    } catch (const std::overflow_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(book.fingerprint() == before);
    CHECK(book.position() == 5.0);
    CHECK(book.trades().empty());
    CHECK(book.pending_count() == 1);
}

void stream_and_trade_exhaustion_precede_mutation() {
    Book stream;
    stream.seed_two_lots();
    stream.observe_actions(true);
    stream.exhaust_stream_actions();
    const auto stream_before = stream.fingerprint();
    bool stream_threw = false;
    try { stream.settle_flatten(); } catch (const std::overflow_error&) { stream_threw = true; }
    CHECK(stream_threw);
    CHECK(stream.fingerprint() == stream_before);
    CHECK(stream.position() == 5.0);
    CHECK(stream.trades().empty());

    Book trades;
    trades.seed_two_lots();
    trades.exhaust_trade_wins();
    const auto trades_before = trades.fingerprint();
    bool trades_threw = false;
    try { trades.settle_flatten(); } catch (const std::overflow_error&) { trades_threw = true; }
    CHECK(trades_threw);
    CHECK(trades.fingerprint() == trades_before);
    CHECK(trades.position() == 5.0);
    CHECK(trades.trades().empty());
}

void invalid_native_requests_are_noops() {
    Book zero;
    zero.seed_two_lots();
    const auto before_zero = zero.fingerprint();
    const auto no_effect = zero.settle_reduce(0.0);
    CHECK(no_effect.status == execution::Status::NoEffect);
    CHECK(zero.fingerprint() == before_zero);

    Book invalid;
    invalid.seed_two_lots();
    const auto before_invalid = invalid.fingerprint();
    const auto bad_qty = invalid.settle_reduce(-1.0);
    CHECK(bad_qty.status == execution::Status::InvalidQuantity);
    CHECK(invalid.fingerprint() == before_invalid);
    const auto bad_price = invalid.settle_reduce(1.0, std::numeric_limits<double>::quiet_NaN());
    CHECK(bad_price.status == execution::Status::InvalidPrice);
    CHECK(invalid.fingerprint() == before_invalid);
    const auto bad_fee = invalid.settle_reduce(
        1.0, 120.0, std::numeric_limits<double>::quiet_NaN());
    CHECK(bad_fee.status == execution::Status::InvalidAccounting);
    CHECK(invalid.fingerprint() == before_invalid);
}

execution::LifecycleIntent fixed_intent(exit_legs::Operation operation) {
    return {500, 0, {500, 4}, 0, std::move(operation)};
}

execution::PendingRemoval fixed_removal() { return {500, 0, {500, 4}, 0}; }

// Every supplied target fact is a precondition, including zero-valued sequence
// and unbound targets. Refusal cannot spend money or mutate pending orders.
void malformed_lifecycle_effects_are_refused() {
    for (int kind = 0; kind < 15; ++kind) {
        Book book;
        book.seed_two_lots();
        book.add_stale_exit();
        execution::LifecycleEffects effects;
        effects.pre_close.emplace();
        effects.pre_close->operations.push_back(fixed_intent(exit_legs::BindOwner{4}));
        auto& intent = effects.pre_close->operations.front();
        switch (kind) {
        case 0: intent.order_incarnation = 0; break;
        case 1: intent.order_incarnation = 501; intent.target.incarnation = 501; break;
        case 2: intent.created_seq = 1; break;
        case 3: intent.target = {}; break;
        case 4: intent.target.owner = 5; break;
        case 5: intent.expected_revision = 1; break;
        case 6: effects.pre_close->operations.push_back(intent); break;
        case 7:
            effects.pre_close->operations.clear();
            effects.pre_close->phase = static_cast<exit_legs::Phase>(255);
            break;
        default:
            effects.pre_close.reset();
            effects.removals.push_back(fixed_removal());
            if (kind == 8) effects.removals.front().expected_revision = 1;
            if (kind == 9) effects.removals.front().target.owner = 5;
            if (kind == 10) effects.removals.push_back(fixed_removal());
            if (kind == 11) effects.removals.front().incarnation = 0;
            if (kind == 12) effects.removals.front().incarnation = 501;
            if (kind == 13) effects.removals.front().target = {};
            if (kind == 14) effects.removals.front().created_seq = 1;
            break;
        }
        const auto before = book.fingerprint();
        const auto result = book.settle_effects(order_action::Reduce{1.0}, effects);
        CHECK(result.status == execution::Status::InvalidLifecycle);
        CHECK(book.fingerprint() == before);
        CHECK(book.position() == 5.0);
        CHECK(book.trades().empty());
    }
}

// A restored, prearmed EXIT can have concrete activation bounds for cycle 4
// while its lifecycle still owns 0, or another stored owner. Flat cleanup at
// d3996b4 unbound the currently stored target; it did not first bind cycle 4.
// Both owners therefore get one unbind receipt, followed by the normal two
// new-owner receipts only when the execution opens a remainder.
void stored_exit_owner_is_unbound_on_full_close() {
    for (int64_t owner : {int64_t{0}, int64_t{99}}) {
        for (bool reversal : {false, true}) {
            Book book;
            book.seed_two_lots();
            book.add_stale_exit();
            book.set_leg_owner(owner);
            book.bind_current_cycle_activation();
            PendingOrder* const pending = book.first_pending_address();
            CHECK(book.first_legs().target().incarnation == 500);
            CHECK(book.lifecycle_owner() == owner);
            CHECK(book.lifecycle_revision() == 0);
            CHECK(book.lifecycle_events() == 0);

            const execution::Action action = reversal
                ? execution::Action{order_action::Transact{-6.0}}
                : execution::Action{execution::Flatten{}};
            execution::Result result;
            try {
                result = book.settle_effects(action, {});
            } catch (const std::exception& error) {
                std::fprintf(stderr, "stored owner %lld, reversal %d: %s\n",
                             static_cast<long long>(owner), reversal, error.what());
                CHECK(false);
                continue;
            }
            CHECK(result.status == execution::Status::Applied);
            CHECK(result.closed_units == 5.0);
            CHECK(result.opened_units == (reversal ? -1.0 : 0.0));
            CHECK(book.position() == (reversal ? -1.0 : 0.0));
            CHECK(book.lots().size() == (reversal ? 1u : 0u));
            CHECK(book.trades().size() == 2);
            CHECK(book.pending_count() == 1);
            CHECK(book.first_pending_address() == pending);
            CHECK(book.first_legs().target().incarnation == 500);
            CHECK(book.lifecycle_owner() == (reversal ? 5 : 0));
            CHECK(book.lifecycle_revision() == (reversal ? 3u : 1u));
            CHECK(book.lifecycle_events() == (reversal ? 3u : 1u));
            CHECK(book.first_legs().last_action().has_value());
            if (book.first_legs().last_action()) {
                const auto& receipt = *book.first_legs().last_action();
                CHECK(receipt.target.incarnation == 500);
                CHECK(receipt.target.owner == (reversal ? 5 : owner));
                CHECK(receipt.expected_revision == (reversal ? 2u : 0u));
                CHECK(receipt.cause.event == (reversal ? 3u : 1u));
                CHECK(receipt.cause.bar == 1);
                CHECK(receipt.cause.domain == exit_legs::Domain::Ordinary);
                CHECK(receipt.cause.phase == exit_legs::Phase::Observation);
                CHECK(book.lifecycle_last_is_bind_to(reversal ? 5 : 0));
            }
            const auto& activation = pending->leg_activation.bounds();
            CHECK(activation.has_value() == reversal);
            if (activation) {
                CHECK(activation->position_cycle == 5);
                CHECK(activation->stop_first_bar == 1);
                CHECK(activation->limit_first_bar == 1);
            }
        }
    }
}

// Accepting the actual stored owner does not authorize a caller to request an
// unrelated owner. This explicit instruction still fails before any effect.
void requested_foreign_owner_is_refused_before_close() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    execution::LifecycleEffects effects;
    effects.pre_close.emplace();
    effects.pre_close->operations.push_back(fixed_intent(exit_legs::BindOwner{5}));
    const auto before = book.fingerprint();
    const auto result = book.settle_effects(execution::Flatten{}, effects);
    CHECK(result.status == execution::Status::InvalidLifecycle);
    CHECK(book.fingerprint() == before);
    CHECK(book.position() == 5.0);
    CHECK(book.trades().empty());
    CHECK(book.lifecycle_events() == 0);
    CHECK(book.lifecycle_owner() == 4);
    CHECK(book.lifecycle_revision() == 0);
}

// The stored owner is not a wildcard for identity: pending incarnation 500
// cannot act on lifecycle incarnation 999, even during whole-book cleanup.
void mismatched_exit_incarnation_is_refused_before_close() {
    for (int64_t owner : {int64_t{0}, int64_t{99}}) {
        for (bool reversal : {false, true}) {
            Book book;
            book.seed_two_lots();
            book.add_stale_exit();
            book.set_leg_incarnation(999, owner);
            book.bind_current_cycle_activation();
            PendingOrder* const pending = book.first_pending_address();
            const auto before = book.fingerprint();
            bool refused = false;
            const execution::Action action = reversal
                ? execution::Action{order_action::Transact{-6.0}}
                : execution::Action{execution::Flatten{}};
            try {
                const auto result = book.settle_effects(action, {});
                refused = result.status == execution::Status::InvalidLifecycle;
            } catch (const std::logic_error& error) {
                refused = std::string(error.what()) == "exit lifecycle flat unbind refused";
            }
            CHECK(refused);
            CHECK(book.fingerprint() == before);
            CHECK(book.position() == 5.0);
            CHECK(book.lots().size() == 2);
            CHECK(book.trades().empty());
            CHECK(book.pending_count() == 1);
            CHECK(book.first_pending_address() == pending);
            CHECK(pending->incarnation == 500);
            CHECK(book.first_legs().target().incarnation == 999);
            CHECK(book.lifecycle_owner() == owner);
            CHECK(book.lifecycle_revision() == 0);
            CHECK(book.lifecycle_events() == 0);
            CHECK(!book.first_legs().last_action());
            const auto& activation = pending->leg_activation.bounds();
            CHECK(activation.has_value());
            if (activation) {
                CHECK(activation->position_cycle == 4);
                CHECK(activation->stop_first_bar == 5);
                CHECK(activation->limit_first_bar == 5);
            }
        }
    }
}

// Exact explicit snapshots remain preconditions, including owner 0. Capture
// the real target, then change either its owner or definition revision before
// submitting that snapshot. Neither removal nor pre-close operation may apply.
void stale_exit_effect_snapshots_are_refused_before_close() {
    for (int64_t owner : {int64_t{0}, int64_t{99}}) {
        for (bool stale_revision : {false, true}) {
            for (bool removal : {false, true}) {
                Book book;
                book.seed_two_lots();
                book.add_stale_exit();
                book.set_leg_owner(owner);
                book.bind_current_cycle_activation();
                execution::LifecycleEffects effects;
                if (removal) {
                    effects.removals.push_back({500, 0, {500, owner}, 0});
                } else {
                    effects.pre_close.emplace();
                    effects.pre_close->operations.push_back(
                        {500, 0, {500, owner}, 0, exit_legs::BindOwner{4}});
                }
                if (stale_revision) {
                    book.define_stop();
                } else {
                    book.set_leg_owner(owner == 0 ? 99 : 0);
                }
                PendingOrder* const pending = book.first_pending_address();
                const auto before = book.fingerprint();
                const auto result = book.settle_effects(order_action::Transact{-6.0}, effects);
                CHECK(result.status == execution::Status::InvalidLifecycle);
                CHECK(book.fingerprint() == before);
                CHECK(book.position() == 5.0);
                CHECK(book.lots().size() == 2);
                CHECK(book.trades().empty());
                CHECK(book.pending_count() == 1);
                CHECK(book.first_pending_address() == pending);
                CHECK(book.first_legs().target().incarnation == 500);
                CHECK(book.lifecycle_owner() == (stale_revision ? owner : (owner == 0 ? 99 : 0)));
                CHECK(book.lifecycle_revision() == (stale_revision ? 1u : 0u));
                CHECK(book.lifecycle_events() == 0);
                CHECK(!book.first_legs().last_action());
                const auto& activation = pending->leg_activation.bounds();
                CHECK(activation.has_value());
                if (activation) {
                    CHECK(activation->position_cycle == 4);
                    CHECK(activation->stop_first_bar == 5);
                    CHECK(activation->limit_first_bar == 5);
                }
            }
        }
    }
}

void operation_window_is_literal() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    book.set_lifecycle_events(10);
    const exit_legs::Frame previous{4, 0, exit_legs::Domain::Ordinary,
                                   exit_legs::Phase::Observation};
    const exit_legs::ObservationWindow window{previous, 120.0, 115.0};
    const exit_legs::Suspend suspend{{exit_legs::Leg::Stop, exit_legs::Leg::Limit},
                                     {}, window, {}};
    execution::LifecycleEffects effects;
    effects.pre_close.emplace();
    effects.pre_close->operations.push_back(fixed_intent(suspend));
    const auto result = book.settle_effects(order_action::Reduce{1.0}, effects);
    CHECK(result.status == execution::Status::Applied);
    CHECK(book.position() == 4.0);
    CHECK(book.lifecycle_events() == 11);
    CHECK(book.first_legs().suspension().has_value());
    if (book.first_legs().suspension()) {
        const auto& state = *book.first_legs().suspension();
        CHECK(state.cause.event == 11);
        CHECK(state.window.has_value());
        if (state.window) {
            CHECK(state.window->excluded.event == 4);
            CHECK(state.window->excluded.bar == 0);
            CHECK(state.window->best == 120.0 && state.window->prefix == 115.0);
        }
    }
}

void source_selection_is_pure_and_empty_batch_is_real() {
    for (bool selected : {false, true}) {
        Book book;
        book.seed_two_lots();
        book.add_stale_exit();
        if (selected) book.define_stop();
        const auto before = book.fingerprint();
        const auto batch = book.selected_pre_close();
        CHECK(batch.has_value());
        CHECK(book.fingerprint() == before);
        if (!batch) continue;
        CHECK(batch->operations.size() == (selected ? 1u : 0u));
        execution::LifecycleEffects effects;
        effects.pre_close = batch;
        const auto result = book.settle_effects(order_action::Reduce{1.0}, effects);
        CHECK(result.status == execution::Status::Applied);
        CHECK(book.lifecycle_events() == 1);
        CHECK(book.position() == 4.0);
        CHECK(book.first_legs().suspension().has_value() == selected);
        if (selected && book.first_legs().suspension()) {
            CHECK(book.first_legs().suspension()->window.has_value());
            if (book.first_legs().suspension()->window)
                CHECK(book.first_legs().suspension()->window->excluded.event == 1);
        }
    }

    Book flat;
    flat.add_stale_exit();
    execution::LifecycleEffects effects;
    effects.pre_close.emplace();
    effects.removals.push_back({500, 0, {500, 0}, 0});
    const auto before = flat.fingerprint();
    const auto result = flat.settle_effects(order_action::Reduce{1.0}, effects);
    CHECK(result.status == execution::Status::NoEffect);
    CHECK(flat.fingerprint() == before);
    CHECK(flat.pending_count() == 1);
}

// The source selector historically applies through the current-owner binding
// transition. Preserve that transition even when the stored leg owner is 0;
// an exact snapshot of owner 0 must not be confused with a wildcard request.
void selected_pre_close_preserves_current_owner_binding() {
    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    book.set_leg_owner(0);
    book.define_stop();
    const auto before = book.fingerprint();
    const auto batch = book.selected_pre_close();
    CHECK(batch && batch->operations.size() == 1);
    CHECK(book.fingerprint() == before);
    if (!batch || batch->operations.empty()) return;
    CHECK(batch->operations.front().target.owner == 0);
    execution::LifecycleEffects effects;
    effects.pre_close = batch;
    const auto result = book.settle_effects(order_action::Reduce{1.0}, effects);
    CHECK(result.status == execution::Status::Applied);
    CHECK(book.position() == 4.0);
    CHECK(book.lifecycle_owner() == 4);
    CHECK(book.lifecycle_revision() == 3); // Definition + owner bind + suspend.
    CHECK(book.lifecycle_events() == 3); // Batch, owner bind, requested operation.
    CHECK(book.first_legs().suspension().has_value());
    if (book.first_legs().suspension()) {
        CHECK(book.first_legs().suspension()->cause.event == 3);
        CHECK(book.first_legs().suspension()->window.has_value());
        if (book.first_legs().suspension()->window)
            CHECK(book.first_legs().suspension()->window->excluded.event == 1);
    }
}

void complete_lifecycle_preflight_handles_exhaustion() {
    for (bool revision : {false, true}) {
        Book book;
        book.seed_two_lots();
        book.add_stale_exit();
        execution::LifecycleEffects effects;
        if (revision) book.exhaust_leg_revision();
        else {
            book.set_lifecycle_events(UINT64_MAX);
            effects.pre_close.emplace();
        }
        const auto before = book.fingerprint();
        bool threw = false;
        try { book.settle_effects(execution::Flatten{}, effects); }
        catch (const std::overflow_error&) { threw = true; }
        CHECK(threw);
        CHECK(book.fingerprint() == before);
        CHECK(book.position() == 5.0);
        CHECK(book.trades().empty());
    }

    Book book;
    book.seed_two_lots();
    book.add_stale_exit();
    book.define_stop();
    execution::LifecycleEffects effects;
    effects.pre_close = book.selected_pre_close();
    book.exhaust_next_cycle();
    const auto before = book.fingerprint();
    bool threw = false;
    try { book.settle_effects(order_action::Transact{-6.0}, effects); }
    catch (const std::overflow_error&) { threw = true; }
    CHECK(threw);
    CHECK(book.fingerprint() == before);
    CHECK(!book.first_legs().suspension());
    CHECK(book.lifecycle_events() == 0);
}

void multiple_exits_and_prior_receipts_preserve_order() {
    for (bool purge : {false, true}) {
        Book book;
        book.seed_two_lots();
        book.add_stale_exit();
        book.add_second_exit();
        book.reverse_raw(120.0, 6.0, purge, 904);
        CHECK(book.position() == -1.0);
        CHECK(book.pending_count() == (purge ? 0u : 2u));
        CHECK(book.lifecycle_events() == (purge ? 2u : 6u));
        if (!purge) CHECK(book.lifecycle_revision() == 3);
    }
    Book prior;
    prior.seed_two_lots();
    prior.add_stale_exit();
    prior.seed_prior_leg_event();
    prior.reverse_raw(120.0, 6.0, false, 905);
    CHECK(prior.lifecycle_events() == 10);
    CHECK(prior.lifecycle_revision() == 4);
    CHECK(prior.lifecycle_owner() == 5);
}

// Adapter quantities are magnitudes. Invalid magnitudes must not silently
// reverse direction or return success to a caller that would then cancel
// sibling orders. Explicit native Transact signed units are a separate API.
void invalid_adapter_magnitudes_fail_before_effects() {
    for (double quantity : {-1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()}) {
        Book book;
        book.seed_two_lots();
        book.add_stale_exit();
        const auto before = book.fingerprint();
        bool threw = false;
        try { book.reverse_raw(120.0, quantity, true, 906); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        CHECK(book.fingerprint() == before);
        CHECK(book.position() == 5.0);
        CHECK(book.trades().empty());
    }
    Book zero;
    zero.seed_two_lots();
    zero.add_stale_exit();
    const auto before = zero.fingerprint();
    zero.reverse_raw(120.0, 0.0, true, 907);
    CHECK(zero.fingerprint() == before);
    CHECK(zero.pending_count() == 1);
}

} // namespace

int main() {
    one_ticket_multi_lot_reversal_before();
    cycle_exhaustion_is_strong_preflight_before();
    production_caller_slips_once_before();
    purge_flag_is_explicit_characterization();
    one_ticket_reverse_direction();
    resolved_limit_price_is_not_slipped();
    partial_close_keeps_cycle_and_lifecycle();
    full_close_unbinds_once();
    retained_exit_rebinds_after_close();
    retained_cleanup_preserves_pending_address();
    lifecycle_exhaustion_precedes_mutation();
    stream_and_trade_exhaustion_precede_mutation();
    invalid_native_requests_are_noops();
    malformed_lifecycle_effects_are_refused();
    stored_exit_owner_is_unbound_on_full_close();
    requested_foreign_owner_is_refused_before_close();
    mismatched_exit_incarnation_is_refused_before_close();
    stale_exit_effect_snapshots_are_refused_before_close();
    operation_window_is_literal();
    source_selection_is_pure_and_empty_batch_is_real();
    selected_pre_close_preserves_current_owner_binding();
    complete_lifecycle_preflight_handles_exhaustion();
    multiple_exits_and_prior_receipts_preserve_order();
    invalid_adapter_magnitudes_fail_before_effects();
    std::printf("native reversal contract checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
