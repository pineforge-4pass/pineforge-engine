// Literal tests of the actual resolved ReverseTo seam; no strategy/tape loop.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/execution_projection.hpp>
#include <pineforge/execution_reverse_to.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;
using pineforge::source::PendingOrder;
using pineforge::source::PineStrategyHost;
namespace x = pineforge::execution;
using ReverseTo = x::reverse_to_v1::ReverseTo;

namespace {
int checks = 0, failures = 0;
const char* scenario = "setup";
struct Abort {};
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #value); } } while (0)
#define REQUIRE(value) do { const bool ok_ = bool(value); CHECK(ok_); \
    if (!ok_) throw Abort{}; } while (0)

uint64_t bits(double value) {
    uint64_t result;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
void exact(double actual, double expected) {
    if (bits(actual) != bits(expected))
        std::printf(" actual=%.17g (%016llx) expected=%.17g (%016llx)\n",
            actual, static_cast<unsigned long long>(bits(actual)),
            expected, static_cast<unsigned long long>(bits(expected)));
    CHECK(bits(actual) == bits(expected));
}
void near(double actual, double expected) {
    const bool ok = std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected));
    if (!ok) std::printf(" actual=%.17g expected=%.17g\n", actual, expected);
    CHECK(ok);
}

struct Book final : PineStrategyHost {
    Book() {
        initial_capital_ = 1000;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 0;
        syminfo_.pointvalue = 1;
        account_currency_fx_ = 1;
        stream_observe_actions_ = true;
        current_bar_ = {100, 130, 70, 100, 1, 1736121600000LL};
        bar_index_ = 3;
    }
    void on_source_bar(const Bar&) override {}
    x::PhysicalExecutionContext context() const {
        // Native execution coordinates intentionally differ from chart state.
        return {1736121660000LL, 7, {}, {}};
    }
    x::SettlementInspection inspect(double target, const x::Fill& fill) const {
        return inspect_native_reversal_v1(ReverseTo{target}, fill);
    }
    x::AccountEffectProjection project(double target, const x::Fill& fill) const {
        return project_native_reversal_v1(ReverseTo{target}, fill);
    }
    x::Result reverse(double target, const x::Fill& fill) {
        return settle_native_reversal_at_v1(ReverseTo{target}, fill, context());
    }
    x::Result reverse_with(double target, const x::Fill& fill,
                           const x::LifecycleEffects& effects) {
        return settle_reversal_with_lifecycle_v1(ReverseTo{target}, fill, effects);
    }
    x::Result flow(const x::Action& action, const x::Fill& fill) {
        return settle_native_execution_at(action, fill, context());
    }
    void open(double quantity, double price, uint64_t incarnation,
              std::optional<double> paid = 0.0) {
        const auto result = flow(order_action::Transact{quantity},
            {price, "seed-" + std::to_string(incarnation), "historical", incarnation, paid});
        REQUIRE(result.status == x::Status::Applied);
    }
    void schedule(CommissionType kind, double value, double pointvalue = 1,
                  double fx = 1) {
        commission_type_ = kind;
        commission_value_ = value;
        syminfo_.pointvalue = pointvalue;
        account_currency_fx_ = fx;
    }
    const auto& lots() const { return pyramid_entries_; }
    auto& lots() { return pyramid_entries_; }
    const auto& rows() const { return trades_; }
    size_t actions() const { return stream_order_actions_.size(); }
    double balance() const { return initial_capital_ + net_profit_sum_; }
    double net() const { return net_profit_sum_; }
    double marked(double price) const { return marked_equity(price); }
    double quantity() const {
        return position_side_ == PositionSide::SHORT ? -position_qty_ : position_qty_;
    }
    int64_t cycle() const { return position_cycle_seq_; }
    int64_t next_cycle() const { return next_position_cycle_seq_; }
    uint64_t next_order() const { return next_order_incarnation_; }
    uint64_t event_sequence() const { return exit_leg_event_seq_; }
    void next_cycle(int64_t value) { next_position_cycle_seq_ = value; }
    void corrupt_side(PositionSide value) { position_side_ = value; }
    void initial(double value) { initial_capital_ = value; }
    void exhaust_stream() { stream_action_sequence_ = UINT64_MAX; }
    void exhaust_wins() { win_trades_count_ = std::numeric_limits<int>::max(); }
    void exhaust_lifecycle() { exit_leg_event_seq_ = UINT64_MAX; }
    void pending_exit() {
        PendingOrder order{};
        order.type = OrderType::EXIT;
        order.id = "retained";
        order.from_entry = "seed-11";
        order.incarnation = 700;
        order.created_seq = 700;
        order.legs.attach(order.incarnation, position_cycle_seq_);
        pending_orders_.push_back(std::move(order));
        CHECK(!pending_orders_.back().is_long);
    }
    size_t pending_count() const { return pending_orders_.size(); }
    const PendingOrder* pending_data() const { return pending_orders_.data(); }
};

x::Fill fill(double price = 100, std::optional<double> ticket = {}) {
    return {price, "reverse", "resolved once", 90, ticket};
}
struct Snapshot {
    uint64_t broker, stream, next_order, event_sequence;
    int64_t cycle, next_cycle;
    size_t lots, rows, actions, pending;
    double balance, quantity;
    const PendingOrder* pending_data;
    explicit Snapshot(const Book& book)
        : broker(book.broker_state_hash()), stream(book.stream_state_hash()),
          next_order(book.next_order()), event_sequence(book.event_sequence()),
          cycle(book.cycle()), next_cycle(book.next_cycle()), lots(book.lots().size()),
          rows(book.rows().size()), actions(book.actions()), pending(book.pending_count()),
          balance(book.balance()), quantity(book.quantity()), pending_data(book.pending_data()) {}
    void unchanged(const Book& book) const {
        CHECK(book.broker_state_hash() == broker);
        CHECK(book.stream_state_hash() == stream);
        CHECK(book.next_order() == next_order && book.event_sequence() == event_sequence);
        CHECK(book.cycle() == cycle && book.next_cycle() == next_cycle);
        CHECK(book.lots().size() == lots && book.rows().size() == rows);
        CHECK(book.actions() == actions && book.pending_count() == pending);
        CHECK(book.pending_data() == pending_data);
        exact(book.balance(), balance);
        exact(book.quantity(), quantity);
    }
};

x::AccountEffectProjection agree_and_apply(Book& book, double target, const x::Fill& f) {
    const Snapshot before(book);
    const auto inspection = book.inspect(target, f);
    before.unchanged(book);
    const auto projection = book.project(target, f);
    before.unchanged(book);
    const auto repeated = book.project(target, f);
    before.unchanged(book);
    REQUIRE(inspection.status == x::Status::Applied);
    REQUIRE(projection.status == x::Status::Applied);
    REQUIRE(repeated.status == x::Status::Applied);
    exact(inspection.opened_units, target);
    exact(projection.opened_units, target);
    exact(projection.signed_units_after, target);
    exact(inspection.resulting_abs_units, std::abs(target));
    exact(projection.resulting_abs_units, std::abs(target));
    CHECK(inspection.resulting_lot_count == 1 && projection.resulting_lot_count == 1);
    CHECK(inspection.would_open && projection.would_open);
    CHECK(inspection.incoming_short == (target < 0));
    CHECK(projection.incoming_short == (target < 0));
    exact(inspection.closed_units, projection.closed_units);
    exact(inspection.current_ticket, projection.current_ticket);
    exact(inspection.resulting_abs_notional, projection.resulting_abs_notional);
    exact(repeated.realized_balance, projection.realized_balance);
    CHECK(projection.cycle_after == before.next_cycle);
    CHECK(repeated.cycle_after == before.next_cycle);

    const auto result = book.reverse(target, f);
    REQUIRE(result.status == x::Status::Applied);
    REQUIRE(book.lots().size() == 1);
    exact(result.closed_units, projection.closed_units);
    exact(result.opened_units, target);
    exact(result.current_ticket, projection.current_ticket);
    exact(book.lots()[0].qty, std::abs(target));
    exact(book.quantity(), target);
    exact(book.balance(), projection.realized_balance);
    exact(book.marked(f.price), projection.marked_equity);
    exact(book.lots()[0].entry_commission_account, projection.remaining_entry_cost);
    CHECK(book.cycle() == projection.cycle_after && book.next_cycle() == before.next_cycle + 1);
    CHECK(result.first_trade_index == before.rows);
    CHECK(result.closed_trade_count == before.lots);
    CHECK(result.opened_lot_incarnation == f.incarnation);
    CHECK(book.rows().size() == before.rows + before.lots);
    CHECK(book.actions() == before.actions + before.lots + 1);
    CHECK(book.lots()[0].entry_incarnation == f.incarnation);
    CHECK(book.lots()[0].entry_id == f.id && book.lots()[0].entry_comment == f.comment);
    exact(book.lots()[0].price, f.price);
    CHECK(book.lots()[0].time == book.context().effective_time_ms);
    CHECK(book.lots()[0].entry_bar_index == book.context().interval_index);
    for (size_t index = before.rows; index < book.rows().size(); ++index) {
        CHECK(book.rows()[index].exit_time == book.context().effective_time_ms);
        CHECK(book.rows()[index].exit_bar_index == book.context().interval_index);
        exact(book.rows()[index].exit_price, f.price);
        CHECK(book.rows()[index].exit_id == f.id && book.rows()[index].exit_comment == f.comment);
    }
    return projection;
}

void exact_target(double sign) {
    scenario = "ReverseTo exact non-dyadic target and one cycle per reversal";
    Book book;
    book.open(sign, 100, 11);
    const auto projection = agree_and_apply(book, -sign * .1, fill(100 + sign * 10));
    REQUIRE(book.rows().size() == 1);
    exact(book.rows()[0].qty, 1);
    near(book.rows()[0].pnl, 10);
    near(projection.realized_balance, 1010);
    CHECK(bits(book.lots()[0].qty) == UINT64_C(0x3fb999999999999a));
    agree_and_apply(book, sign * .2, fill(100));
    CHECK(book.cycle() == 3 && book.next_cycle() == 4);
}

void non_dyadic_roster(double sign) {
    scenario = "ReverseTo closes each non-dyadic roster member in order";
    Book book;
    book.open(sign * .1, 100, 11);
    book.open(sign * .2, 100, 12);
    book.open(sign * .3, 100, 13);
    agree_and_apply(book, -sign * .1, fill());
    REQUIRE(book.rows().size() == 3);
    const double quantities[] = {.1, .2, .3};
    for (size_t index = 0; index < 3; ++index) {
        CHECK(book.rows()[index].entry_incarnation == 11 + index);
        exact(book.rows()[index].qty, quantities[index]);
    }
}

void absorbed_quantity(double sign, bool tiny_target) {
    scenario = tiny_target ? "large held and absorbed tiny target remains representable"
                           : "tiny held and absorbed large target remains representable";
    const double held = tiny_target ? 1e16 : .1;
    const double target = tiny_target ? .1 : 1e16;
    Book book;
    book.open(sign * held, 1, 11);
    agree_and_apply(book, -sign * target, fill(1));
    REQUIRE(book.rows().size() == 1);
    exact(book.rows()[0].qty, held);
}

void tiny_roster_member(double sign) {
    scenario = "ReverseTo whole-book close retains a tiny physical roster member";
    Book book;
    book.open(sign * 1e16, 1, 11);
    // Construct the valid physical roster directly: a Transact add intentionally
    // cannot express this absorbed member. ReverseTo must still close both lots.
    PyramidEntry tiny{1, 1736121600000LL, .1, "tiny", 3};
    tiny.entry_incarnation = 12;
    tiny.entry_commission_account = 0;
    book.lots().push_back(tiny);
    agree_and_apply(book, -sign * .2, fill(1));
    REQUIRE(book.rows().size() == 2);
    CHECK(book.rows()[0].entry_incarnation == 11 && book.rows()[1].entry_incarnation == 12);
    exact(book.rows()[0].qty, 1e16);
    exact(book.rows()[1].qty, .1);
}

void one_ticket(double sign, CommissionType type, double value,
                std::optional<double> explicit_ticket, double ticket,
                double first_commission, double second_commission,
                double opening_paid, double balance, double equity) {
    scenario = "ReverseTo one current ticket conserves historical paid entry costs";
    Book book;
    book.open(sign * 2, 120 - sign * 20, 11, 4);
    book.open(sign * 3, 120 - sign * 10, 12, 6);
    book.schedule(type, value);
    const auto projection = agree_and_apply(book, -sign * 5, fill(120, explicit_ticket));
    REQUIRE(book.rows().size() == 2);
    near(projection.current_ticket, ticket);
    near(book.rows()[0].commission, first_commission);
    near(book.rows()[1].commission, second_commission);
    near(book.lots()[0].entry_commission_account, opening_paid);
    near(book.balance(), balance);
    near(book.marked(120), equity);
    // Existing paid costs are 10; one current ticket is shared by all effects.
    near(book.rows()[0].commission + book.rows()[1].commission
         + book.lots()[0].entry_commission_account, 10 + ticket);
}

void historical_fx(double sign) {
    scenario = "ReverseTo percentage fee uses current FX without repricing paid entry costs";
    Book book;
    book.schedule(CommissionType::PERCENT, 1, 2, 2);
    book.open(sign * 2, 120 - sign * 20, 11, std::nullopt);
    book.open(sign * 3, 120 - sign * 10, 12, std::nullopt);
    const double first_paid = sign > 0 ? 8 : 11.2;
    const double second_paid = sign > 0 ? 13.2 : 15.6;
    near(book.lots()[0].entry_commission_account, first_paid);
    near(book.lots()[1].entry_commission_account, second_paid);
    book.schedule(CommissionType::PERCENT, 1, 2, 3);
    const auto projection = agree_and_apply(book, -sign * 5, fill(120));
    REQUIRE(book.rows().size() == 2);
    near(projection.current_ticket, 72);
    near(book.rows()[0].commission, first_paid + 14.4);
    near(book.rows()[1].commission, second_paid + 21.6);
    near(book.rows()[0].pnl, sign > 0 ? 217.6 : 214.4);
    near(book.rows()[1].pnl, sign > 0 ? 145.2 : 142.8);
    near(projection.remaining_entry_cost, 36);
    near(projection.realized_balance, sign > 0 ? 1362.8 : 1357.2);
    near(projection.marked_equity, sign > 0 ? 1326.8 : 1321.2);
}

void invalid_inspection(const x::SettlementInspection& value, x::Status status) {
    CHECK(value.status == status);
    CHECK(value.closed_units == 0 && value.opened_units == 0 && value.resulting_abs_units == 0);
    CHECK(value.resulting_lot_count == 0 && value.resulting_abs_notional == 0 && value.current_ticket == 0);
    CHECK(!value.would_open && !value.incoming_short);
}
void refused(Book& book, double target, const x::Fill& f, x::Status status) {
    const Snapshot before(book);
    invalid_inspection(book.inspect(target, f), status);
    before.unchanged(book);
    const auto projection = book.project(target, f);
    CHECK(projection.status == status);
    CHECK(projection.closed_units == 0 && projection.opened_units == 0 && projection.resulting_abs_units == 0);
    CHECK(projection.resulting_lot_count == 0 && projection.resulting_abs_notional == 0 && projection.current_ticket == 0);
    CHECK(!projection.would_open && !projection.incoming_short);
    CHECK(projection.realized_balance == 0 && projection.remaining_entry_cost == 0 && projection.marked_equity == 0);
    CHECK(projection.cycle_after == 0 && projection.signed_units_after == 0);
    before.unchanged(book);
    const auto result = book.reverse(target, f);
    CHECK(result.status == status);
    CHECK(result.closed_units == 0 && result.opened_units == 0 && result.current_ticket == 0);
    CHECK(result.first_trade_index == 0 && result.closed_trade_count == 0 && result.opened_lot_incarnation == 0);
    before.unchanged(book);
}

void quantity_and_book_refusals(double sign) {
    scenario = "ReverseTo has no NoEffect quantity or side case";
    Book book;
    book.open(sign, 100, 11);
    for (double target : {0.0, -0.0, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        refused(book, target, fill(), x::Status::InvalidQuantity);
    refused(book, sign * .1, fill(), x::Status::InvalidCloseTarget);
    Book flat;
    refused(flat, -sign * .1, fill(), x::Status::InvalidCloseTarget);
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        refused(book, -sign * .1, fill(invalid), x::Status::InvalidPrice);
        refused(book, -sign * .1, fill(100, invalid), x::Status::InvalidAccounting);
    }
    scenario = "ReverseTo invalid physical book refuses before mutation";
    for (double quantity : {-1.0, 0.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        Book invalid;
        invalid.open(sign, 100, 11);
        invalid.lots()[0].qty = quantity;
        refused(invalid, -sign * .1, fill(), x::Status::InvalidBook);
    }
    Book invalid_price;
    invalid_price.open(sign, 100, 11);
    invalid_price.lots()[0].price = std::numeric_limits<double>::infinity();
    refused(invalid_price, -sign * .1, fill(), x::Status::InvalidBook);
    Book invalid_side;
    invalid_side.open(sign, 100, 11);
    invalid_side.corrupt_side(static_cast<PositionSide>(99));
    refused(invalid_side, -sign * .1, fill(), x::Status::InvalidBook);
    Book absent_roster;
    absent_roster.open(sign, 100, 11);
    absent_roster.lots().clear();
    refused(absent_roster, -sign * .1, fill(), x::Status::InvalidBook);
}

void overflow_refusals(double sign) {
    scenario = "ReverseTo finite gross overflow precedes commission quote";
    Book gross;
    const double large = std::numeric_limits<double>::max() * .75;
    gross.open(sign * large, 0, 11);
    // A fee quote would itself be invalid. Quantity overflow must win first.
    gross.schedule(CommissionType::CASH_PER_ORDER, std::numeric_limits<double>::quiet_NaN());
    refused(gross, -sign * large, fill(0), x::Status::UnrepresentableQuantity);

    scenario = "ReverseTo finite component fees cannot overflow the aggregate ticket";
    Book ticket;
    ticket.open(sign, 0, 11);
    ticket.open(sign, 0, 12);
    ticket.schedule(CommissionType::CASH_PER_CONTRACT, std::numeric_limits<double>::max() / 2);
    refused(ticket, -sign, fill(0), x::Status::InvalidAccounting);

    scenario = "ReverseTo overflowing held roster is invalid book";
    Book held;
    held.open(sign * large, 0, 11);
    auto copy = held.lots()[0];
    copy.entry_incarnation = 12;
    held.lots().push_back(copy);
    refused(held, -sign, fill(0), x::Status::InvalidBook);
}

template<class F> void throws_atomically(Book& book, F call) {
    const Snapshot before(book);
    bool threw = false;
    try { call(); } catch (const std::overflow_error&) { threw = true; }
    CHECK(threw);
    before.unchanged(book);
}
void exception_preflight() {
    scenario = "ReverseTo cycle projection peeks without consuming exhausted sequence";
    for (int64_t next : {int64_t{0}, int64_t{-1}, std::numeric_limits<int64_t>::max()}) {
        Book book;
        book.open(1, 100, 11);
        book.next_cycle(next);
        const Snapshot before(book);
        CHECK(book.inspect(-.1, fill()).status == x::Status::Applied);
        before.unchanged(book);
        throws_atomically(book, [&] { (void)book.project(-.1, fill()); });
        throws_atomically(book, [&] { (void)book.reverse(-.1, fill()); });
    }
    scenario = "ReverseTo exhausted close counter refuses before close or opening";
    Book counter;
    counter.open(1, 100, 11);
    counter.exhaust_wins();
    throws_atomically(counter, [&] { (void)counter.reverse(-.1, fill(110)); });
    scenario = "ReverseTo exhausted stream refuses before close or opening";
    Book stream;
    stream.open(1, 100, 11);
    stream.exhaust_stream();
    throws_atomically(stream, [&] { (void)stream.reverse(-.1, fill()); });
    scenario = "ReverseTo exhausted pending lifecycle refuses atomically";
    Book lifecycle;
    lifecycle.open(1, 100, 11);
    lifecycle.pending_exit();
    lifecycle.exhaust_lifecycle();
    throws_atomically(lifecycle, [&] { (void)lifecycle.reverse(-.1, fill()); });
}

void invalid_lifecycle() {
    scenario = "ReverseTo lifecycle wrapper validates batch before financial effects";
    Book book;
    book.open(1, 100, 11);
    book.pending_exit();
    x::LifecycleEffects effects;
    effects.removals.push_back({999, 999, {}, 0});
    const Snapshot before(book);
    const auto result = book.reverse_with(-.1, fill(), effects);
    CHECK(result.status == x::Status::InvalidLifecycle);
    CHECK(result.closed_units == 0 && result.opened_units == 0 && result.current_ticket == 0);
    before.unchanged(book);
}

void fresh_commit() {
    scenario = "ReverseTo projection is read-only and does not authorize a stale close";
    Book book;
    book.open(1, 100, 11);
    const auto earlier = book.project(-.1, fill(110));
    REQUIRE(earlier.status == x::Status::Applied);
    exact(earlier.closed_units, 1);
    REQUIRE(book.flow(order_action::Reduce{.5}, fill(110, 0)).status == x::Status::Applied);
    const auto fresh = agree_and_apply(book, -.1, fill(110));
    exact(fresh.closed_units, .5);
    CHECK(fresh.closed_units != earlier.closed_units);
    const Snapshot after(book);
    CHECK(book.reverse(-.1, fill(110)).status == x::Status::InvalidCloseTarget);
    after.unchanged(book);
}

void sequential_balance() {
    scenario = "ReverseTo projection preserves sequential realized PnL association";
    Book book;
    book.initial(1);
    const double high = 10000000000000100.0;
    book.open(1, high, 11);
    REQUIRE(book.flow(x::Flatten{}, fill(100, 0)).status == x::Status::Applied);
    exact(book.net(), -1e16);
    book.open(1, 100, 12);
    book.open(.5, high - 2, 13);
    const auto projection = agree_and_apply(book, -.1, fill(high, 0));
    REQUIRE(book.rows().size() == 3);
    exact(book.rows()[1].pnl, 1e16);
    exact(book.rows()[2].pnl, 1);
    exact(book.net(), 1);
    exact(projection.realized_balance, 2);
    exact(projection.marked_equity, 2);
}

template<class F> void run(F call) {
    try { call(); }
    catch (const Abort&) {}
    catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL %s exception: %s\n", scenario, error.what());
    }
}
} // namespace

int main() {
    static_assert(std::variant_size_v<x::Action> == 3);
    for (double sign : {1.0, -1.0}) {
        run([&] { exact_target(sign); });
        run([&] { non_dyadic_roster(sign); });
        run([&] { absorbed_quantity(sign, true); });
        run([&] { absorbed_quantity(sign, false); });
        run([&] { tiny_roster_member(sign); });
        run([&] { one_ticket(sign, CommissionType::CASH_PER_ORDER, 10, {}, 10, 6, 9, 5, 1055, 1050); });
        run([&] { one_ticket(sign, CommissionType::CASH_PER_CONTRACT, 2, {}, 20, 8, 12, 10, 1050, 1040); });
        run([&] { one_ticket(sign, CommissionType::CASH_PER_ORDER, 99, 0, 0, 4, 6, 0, 1060, 1060); });
        run([&] { one_ticket(sign, CommissionType::CASH_PER_ORDER, 99, -10, -10, 2, 3, -5, 1065, 1070); });
        run([&] { one_ticket(sign, CommissionType::CASH_PER_ORDER, -10, {}, -10, 2, 3, -5, 1065, 1070); });
        run([&] { historical_fx(sign); });
        run([&] { quantity_and_book_refusals(sign); });
        run([&] { overflow_refusals(sign); });
    }
    run(exception_preflight);
    run(invalid_lifecycle);
    run(fresh_commit);
    run(sequential_balance);
    std::printf("%s native ReverseTo: %d checks, %d failures\n",
        failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
