// Native order-action integration seams. This test does not call Engine::run,
// load a feed, compile Pine, or compare against a reference engine.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace pineforge;
using pineforge::source::PendingOrder;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (0)

template<class Tag, auto Member>
struct Access { friend auto access(Tag) { return Member; } };
struct PartialExitAccess { friend auto access(PartialExitAccess); };
struct SameSideAccess { friend auto access(SameSideAccess); };
struct SameBarTransactionAccess { friend auto access(SameBarTransactionAccess); };
struct MarketFillAccess { friend auto access(MarketFillAccess); };
template struct Access<PartialExitAccess, &pineforge::source::PineStrategyHost::execute_partial_exit_qty>;
template struct Access<SameSideAccess, &BacktestEngine::append_same_side_fill>;
template struct Access<SameBarTransactionAccess, &pineforge::source::PineStrategyHost::apply_same_bar_market_tx_reversal>;
template struct Access<MarketFillAccess, &pineforge::source::PineStrategyHost::apply_market_order_fill>;

template<class T> struct MemberArguments;
template<class C, class R, class A, class B, class D>
struct MemberArguments<R (C::*)(A, B, D)> { using third = D; };
using ReductionCause = typename MemberArguments<
    decltype(access(PartialExitAccess{}))>::third;

class Book final : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 10000.0;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 1.0;
        slippage_ = 0;
        syminfo_mintick_ = 0.01;
        current_bar_ = {100.0, 130.0, 90.0, 120.0, 1.0, 60000};
        bar_index_ = 1;
    }

    void on_source_bar(const Bar&) override {}

    void seed_two_lots() {
        position_side_ = PositionSide::LONG;
        position_cycle_seq_ = 4;
        next_position_cycle_seq_ = 5;
        position_entry_price_ = 106.0;
        position_qty_ = 5.0;
        position_entry_count_ = 2;
        position_open_bar_ = 0;
        pyramid_entries_.clear();
        PyramidEntry a{100.0, 1000, 2.0, "A", 0};
        a.entry_incarnation = 11;
        a.entry_commission_account = 2.0;
        PyramidEntry b{110.0, 2000, 3.0, "B", 0};
        b.entry_incarnation = 12;
        b.entry_commission_account = 3.0;
        b.max_runup = 60.0;
        b.max_drawdown = 30.0;
        pyramid_entries_.push_back(a);
        pyramid_entries_.push_back(b);
        id_unclosed_qty_.clear();
        id_unclosed_qty_["A"] = 2.0;
        id_unclosed_qty_["B"] = 3.0;
        trades_.clear();
        net_profit_sum_ = 0.0;
        gross_profit_sum_ = 0.0;
        gross_loss_sum_ = 0.0;
        win_trades_count_ = loss_trades_count_ = eventrades_count_ = 0;
    }

    void seed_three_units() {
        seed_two_lots();
        position_qty_ = 3.0;
        position_entry_price_ = (100.0 + 220.0) / 3.0;
        pyramid_entries_[0].qty = 1.0;
        pyramid_entries_[1].qty = 2.0;
        pyramid_entries_[0].entry_commission_account = 1.0;
        pyramid_entries_[1].entry_commission_account = 2.0;
        id_unclosed_qty_["A"] = 1.0;
        id_unclosed_qty_["B"] = 2.0;
    }

    void partial(double raw_price, double qty) {
        (this->*access(PartialExitAccess{}))(raw_price, qty,
            static_cast<ReductionCause>(0));
    }

    void append(PyramidEntry lot) {
        (this->*access(SameSideAccess{}))(std::move(lot));
    }

    void enable_stream_actions() { stream_observe_actions_ = true; }
    void set_slippage(int ticks) { slippage_ = ticks; }
    void make_short() { position_side_ = PositionSide::SHORT; }
    void per_order_fees() {
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 3.0;
        for (auto& lot : pyramid_entries_) lot.entry_commission_account = 3.0;
    }

    void transact(PendingOrder& order, double raw_price) {
        double trail = std::numeric_limits<double>::quiet_NaN();
        (this->*access(SameBarTransactionAccess{}))(order, raw_price,
            current_bar_, trail);
    }
    void exhaust_cycles() { next_position_cycle_seq_ = std::numeric_limits<int64_t>::max(); }
    uint64_t fingerprint() const { return broker_state_hash(); }
    void seed_short_collision(double source, double materialized) {
        seed_two_lots();
        position_open_bar_ = bar_index_;
        position_qty_ = source + materialized;
        position_entry_price_ = 100;
        pyramid_entries_[0].qty = source;
        pyramid_entries_[1].qty = materialized;
        pyramid_entries_[0].price = pyramid_entries_[1].price = 100;
        pyramid_entries_[0].entry_id = "Long";
        pyramid_entries_[1].entry_id = "__close__Short";
        for (auto& lot : pyramid_entries_) lot.entry_bar_index = bar_index_;
        PendingOrder first{}, final{}, materialize{};
        first.id = "Long";
        first.type = final.type = materialize.type = OrderType::MARKET;
        first.is_long = true;
        first.short_seed_collision_role = ShortSeedCollisionRole::LONG_ENTRY;
        final.id = "Short";
        final.is_long = false;
        final.incarnation = 44;
        final.created_bar = bar_index_ - 1;
        final.tv_carry_qty = materialized;
        final.short_seed_collision_role = ShortSeedCollisionRole::FINAL_SHORT;
        materialize.id = "__close__Short";
        materialize.short_seed_collision_role = ShortSeedCollisionRole::MATERIALIZE_LONG;
        pending_orders_ = {first, final, materialize};
    }
    void settle_short_collision() {
        double trail = std::numeric_limits<double>::quiet_NaN();
        (this->*access(MarketFillAccess{}))(pending_orders_[1], 100, current_bar_, trail, false);
    }

    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
    const std::vector<Trade>& trades() const { return trades_; }
    double signed_position() const { return signed_position_size(); }
    PositionSide side() const { return position_side_; }
    int64_t cycle() const { return position_cycle_seq_; }
    int stream_actions() const { return stream_order_actions_len(); }
};

void reduce_preserves_fifo_identity_and_scales_survivor() {
    Book book;
    book.seed_two_lots();
    book.partial(120.0, 3.0);

    CHECK(book.trades().size() == 2);
    CHECK(book.trades()[0].entry_id == "A");
    CHECK(book.trades()[0].qty == 2.0);
    CHECK(book.trades()[1].entry_id == "B");
    CHECK(book.trades()[1].qty == 1.0);
    CHECK(book.lots().size() == 1);
    CHECK(book.lots()[0].entry_id == "B");
    CHECK(book.lots()[0].entry_incarnation == 12);
    CHECK(book.lots()[0].qty == 2.0);
    CHECK(book.lots()[0].entry_commission_account == 2.0);
    CHECK(book.lots()[0].max_runup == 40.0);
    CHECK(book.lots()[0].max_drawdown == 20.0);
    CHECK(book.signed_position() == 2.0);
    CHECK(book.cycle() == 4);

    Book zero;
    zero.seed_two_lots();
    zero.partial(120.0, 0.0);
    CHECK(zero.trades().empty() && zero.signed_position() == 5.0);
    zero.partial(120.0, -1.0);
    CHECK(zero.trades().empty() && zero.signed_position() == 5.0);
    zero.partial(120.0, std::numeric_limits<double>::quiet_NaN());
    CHECK(zero.trades().empty() && zero.signed_position() == 5.0);

    Book oversized;
    oversized.seed_two_lots();
    oversized.partial(120.0, 99.0);
    CHECK(oversized.signed_position() == 0.0);
    CHECK(oversized.lots().empty());
    CHECK(oversized.trades().size() == 2);
    CHECK(oversized.cycle() == 0);

    Book order_fee;
    order_fee.seed_two_lots();
    order_fee.per_order_fees();
    order_fee.partial(120.0, 3.0);
    CHECK(order_fee.lots().size() == 1);
    // One cash-per-order ticket is allocated proportionally to the physical
    // FIFO slices: B survives with 2/3 of its original 3-unit lot, so its
    // retained paid fee is 3 * 2/3 = 2, not a second full ticket.
    CHECK(order_fee.lots()[0].entry_commission_account == 2.0);
}

void reduce_handles_short_side_and_slippage_once() {
    Book book;
    book.seed_two_lots();
    book.make_short();
    book.set_slippage(2);
    book.partial(120.0, 2.0);
    CHECK(book.signed_position() == -3.0);
    CHECK(book.trades().size() == 1);
    CHECK(!book.trades()[0].is_long);
    CHECK(std::abs(book.trades()[0].exit_price - 120.02) < 1e-12);

    Book long_side;
    long_side.seed_two_lots();
    long_side.set_slippage(2);
    long_side.partial(120.0, 2.0);
    CHECK(long_side.trades().size() == 1);
    CHECK(std::abs(long_side.trades()[0].exit_price - 119.98) < 1e-12);
}

void append_preserves_lot_metadata_and_stream_action() {
    Book book;
    book.seed_two_lots();
    book.enable_stream_actions();
    PyramidEntry lot{120.0, 3000, 1.0, "C", 1};
    lot.entry_comment = "native add";
    lot.entry_incarnation = 13;
    lot.market_pyramid_add = false;
    book.append(lot);

    CHECK(book.lots().size() == 3);
    CHECK(book.lots().back().entry_id == "C");
    CHECK(book.lots().back().entry_comment == "native add");
    CHECK(book.lots().back().entry_incarnation == 13);
    CHECK(!book.lots().back().market_pyramid_add);
    CHECK(book.lots().back().entry_commission_account == 1.0);
    CHECK(book.signed_position() == 6.0);
    CHECK(book.stream_actions() == 1);
    CHECK(book.stream_order_action_at(0).is_entry);
    CHECK(book.stream_order_action_at(0).quantity == 1.0);
    CHECK(book.stream_order_action_at(0).entry_incarnation == 13);
}

PendingOrder transaction_order(bool is_long, double own, double total) {
    PendingOrder order{};
    order.id = is_long ? "BUY" : "SELL";
    order.type = OrderType::MARKET;
    order.is_long = is_long;
    order.incarnation = 44;
    order.pine_frozen_market_instruction =
        PineFrozenMarketInstruction::transaction(own, total);
    return order;
}

void transact_closes_fifo_and_crosses_flat() {
    for (bool from_short : {false, true}) {
        Book partial;
        partial.seed_three_units(); // FIFO A=1, B=2
        if (from_short) partial.make_short();
        partial.enable_stream_actions();
        auto two = transaction_order(from_short, 2.0, 2.0);
        partial.transact(two, 120.0);
        CHECK(partial.signed_position() == (from_short ? -1.0 : 1.0));
        CHECK(partial.cycle() == 4);
        CHECK(partial.trades().size() == 2);
        if (partial.trades().size() == 2) {
            CHECK(partial.trades()[0].entry_id == "A" && partial.trades()[0].qty == 1.0);
            CHECK(partial.trades()[1].entry_id == "B" && partial.trades()[1].qty == 1.0);
        }
        CHECK(partial.lots().size() == 1 && partial.lots()[0].entry_id == "B");
        CHECK(partial.stream_actions() == 2);
        if (partial.stream_actions() == 2) {
            CHECK(!partial.stream_order_action_at(0).is_entry);
            CHECK(!partial.stream_order_action_at(1).is_entry);
            CHECK(partial.stream_order_action_at(0).quantity == 1.0);
            CHECK(partial.stream_order_action_at(1).quantity == 1.0);
        }

        Book exact;
        exact.seed_three_units();
        if (from_short) exact.make_short();
        auto three = transaction_order(from_short, 3.0, 3.0);
        exact.transact(three, 120.0);
        CHECK(exact.signed_position() == 0.0 && exact.cycle() == 0);
        CHECK(exact.lots().empty());
        CHECK(exact.trades().size() == 2);
        if (exact.trades().size() == 2) {
            CHECK(exact.trades()[0].qty == 1.0 && exact.trades()[1].qty == 2.0);
        }

        Book crossing;
        crossing.seed_three_units();
        if (from_short) crossing.make_short();
        crossing.enable_stream_actions();
        auto five = transaction_order(from_short, 5.0, 5.0);
        crossing.transact(five, 120.0);
        CHECK(crossing.signed_position() == (from_short ? 2.0 : -2.0));
        CHECK(crossing.cycle() == 5);
        CHECK(crossing.lots().size() == 1);
        if (!crossing.lots().empty()) {
            CHECK(crossing.lots()[0].entry_id == (from_short ? "BUY" : "SELL"));
            CHECK(crossing.lots()[0].qty == 2.0);
            CHECK(crossing.lots()[0].entry_incarnation == 44);
        }
        CHECK(crossing.trades().size() == 2);
        CHECK(crossing.stream_actions() == 3);
        if (crossing.stream_actions() == 3) {
            CHECK(!crossing.stream_order_action_at(0).is_entry);
            CHECK(!crossing.stream_order_action_at(1).is_entry);
            CHECK(crossing.stream_order_action_at(2).is_entry);
            CHECK(crossing.stream_order_action_at(2).quantity == 2.0);
        }
    }
}

void adapter_crossings_are_one_execution() {
    for (bool from_short : {false, true}) {
        Book fees;
        fees.seed_three_units();
        fees.per_order_fees(); // two historical tickets of3, one current ticket of3
        if (from_short) fees.make_short();
        fees.enable_stream_actions();
        auto five = transaction_order(from_short, 5, 5);
        fees.transact(five, 120);
        CHECK(fees.trades().size() == 2 && fees.lots().size() == 1);
        if (fees.trades().size() == 2 && fees.lots().size() == 1) {
            const double paid = fees.trades()[0].commission + fees.trades()[1].commission
                + fees.lots()[0].entry_commission_account;
            CHECK(std::abs(paid - 9) < 1e-12);
            CHECK(std::abs(fees.lots()[0].entry_commission_account - 1.2) < 1e-12);
        }
        CHECK(fees.stream_actions() == 3);

        Book exhausted;
        exhausted.seed_three_units();
        if (from_short) exhausted.make_short();
        exhausted.enable_stream_actions();
        exhausted.exhaust_cycles();
        const auto before = exhausted.fingerprint();
        bool threw = false;
        try { exhausted.transact(five, 120); }
        catch (const std::overflow_error&) { threw = true; }
        CHECK(threw);
        CHECK(exhausted.fingerprint() == before);
        CHECK(exhausted.trades().empty() && exhausted.stream_actions() == 0);
        CHECK(exhausted.signed_position() == (from_short ? -3 : 3));
    }

    Book short_seed;
    short_seed.seed_short_collision(3, 1);
    short_seed.per_order_fees();
    short_seed.enable_stream_actions();
    short_seed.settle_short_collision();
    CHECK(short_seed.signed_position() == -2 && short_seed.cycle() == 5);
    CHECK(short_seed.trades().size() == 2 && short_seed.lots().size() == 1);
    if (short_seed.trades().size() == 2 && short_seed.lots().size() == 1) {
        CHECK(short_seed.trades()[0].entry_id == "Long");
        CHECK(short_seed.trades()[1].entry_id == "__close__Short");
        const double paid = short_seed.trades()[0].commission + short_seed.trades()[1].commission
            + short_seed.lots()[0].entry_commission_account;
        CHECK(std::abs(paid - 9) < 1e-12);
        CHECK(std::abs(short_seed.lots()[0].entry_commission_account - 1) < 1e-12);
    }
    CHECK(short_seed.stream_actions() == 3);

    Book refused;
    refused.seed_short_collision(3, 1);
    refused.enable_stream_actions();
    refused.exhaust_cycles();
    const auto before = refused.fingerprint();
    bool threw = false;
    try { refused.settle_short_collision(); }
    catch (const std::overflow_error&) { threw = true; }
    CHECK(threw && refused.fingerprint() == before);
    CHECK(refused.signed_position() == 4 && refused.trades().empty());
    CHECK(refused.stream_actions() == 0);
}

}

int main() {
    reduce_preserves_fifo_identity_and_scales_survivor();
    reduce_handles_short_side_and_slippage_once();
    append_preserves_lot_metadata_and_stream_action();
    transact_closes_fifo_and_crosses_flat();
    adapter_crossings_are_one_execution();
    std::printf("order action integration: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
