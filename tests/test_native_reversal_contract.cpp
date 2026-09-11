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

template struct Access<CloseOppositeAccess,
                       &BacktestEngine::close_opposite_then_enter>;
template struct Access<MarketEntryAccess,
                       &BacktestEngine::execute_market_entry>;
template struct Access<SettleAccess,
                       &BacktestEngine::settle_resolved_execution>;

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
    std::printf("native reversal contract checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
