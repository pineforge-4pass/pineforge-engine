// Literal calls to the real F7 and F8 adapters. No run(), tape or strategy loop.
#include <pineforge/engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using namespace pineforge;
namespace x = pineforge::execution;
namespace {
int checks = 0, failures = 0;
const char* scenario = "setup";
struct Abort {};
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #value); } } while (0)
#define REQUIRE(value) do { const bool ok_ = bool(value); CHECK(ok_); \
    if (!ok_) throw Abort{}; } while (0)

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct FlipTag { friend auto access(FlipTag); };
struct SequentialTag { friend auto access(SequentialTag); };
template struct Access<FlipTag, &BacktestEngine::flip_market_position_to>;
template struct Access<SequentialTag, &BacktestEngine::sequential_same_tick_reversal_fill>;

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

struct Book final : BacktestEngine {
    Book() {
        initial_capital_ = 1000;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 0;
        slippage_ = 0;
        qty_step_ = 0;
        pyramiding_ = 100;
        syminfo_.pointvalue = 1;
        syminfo_.mintick = .01;
        syminfo_mintick_ = .01;
        account_currency_fx_ = 1;
        stream_observe_actions_ = true;
        current_bar_ = {100, 130, 70, 110, 1, 1736121660000LL};
        bar_index_ = 7;
    }
    void on_bar(const Bar&) override {}
    void open(double quantity, double price, uint64_t incarnation) {
        const x::PhysicalExecutionContext context{1736121600000LL, 6, {}, {}};
        REQUIRE(settle_native_execution_at(order_action::Transact{quantity},
            x::Fill{price, "old", "historical", incarnation, 0}, context).status == x::Status::Applied);
    }
    void flip(bool buy, double price, double quantity, int type = -1,
              bool frozen = true, bool close_only = false) {
        (this->*access(FlipTag{}))(std::string("flip"), buy, price, quantity,
            type, frozen, close_only, 90);
    }
    void sequential(bool buy, double price, double transaction) {
        (this->*access(SequentialTag{}))(std::string("sequential"), buy, price,
            transaction, -1, 91);
    }
    void fee(double value) { commission_value_ = value; }
    void step(double value) { qty_step_ = value; }
    void scale(double pointvalue, double fx) {
        syminfo_.pointvalue = pointvalue;
        account_currency_fx_ = fx;
    }
    void default_percent(double value) {
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = value;
    }
    void already_resolved_slippage() { slippage_ = 99; }
    void retain_exit() {
        PendingOrder order;
        order.id = "retained";
        order.from_entry = "old";
        order.type = OrderType::EXIT;
        order.incarnation = 700;
        order.created_seq = 700;
        order.legs.attach(order.incarnation, position_cycle_seq_);
        pending_orders_.push_back(std::move(order));
    }
    const auto& lots() const { return pyramid_entries_; }
    const auto& rows() const { return trades_; }
    const auto& pending() const { return pending_orders_; }
    double quantity() const {
        return position_side_ == PositionSide::SHORT ? -position_qty_ : position_qty_;
    }
    double balance() const { return initial_capital_ + net_profit_sum_; }
    double marked(double price) const { return marked_equity(price); }
    int64_t cycle() const { return position_cycle_seq_; }
    int64_t next_cycle() const { return next_position_cycle_seq_; }
    size_t actions() const { return stream_order_actions_.size(); }
};

void exact_f7_and_unchanged_f8(double held_sign) {
    scenario = "F7 preserves exact requested target; F8 preserves transaction remainder";
    const bool buy = held_sign < 0;
    const double price = 100 + held_sign * 10;
    Book flip;
    flip.open(held_sign, 100, 11);
    flip.retain_exit();
    const auto* pending = flip.pending().data();
    const auto next_cycle = flip.next_cycle();
    const auto actions = flip.actions();
    flip.already_resolved_slippage();
    flip.step(1); // Frozen source quantity must not be floored a second time.
    flip.flip(buy, price, .1);
    REQUIRE(flip.lots().size() == 1 && flip.rows().size() == 1);
    CHECK(bits(flip.lots()[0].qty) == UINT64_C(0x3fb999999999999a));
    exact(flip.quantity(), -held_sign * .1);
    exact(flip.rows()[0].qty, 1);
    near(flip.rows()[0].pnl, 10);
    exact(flip.rows()[0].exit_price, price);
    exact(flip.lots()[0].price, price);
    CHECK(flip.rows()[0].entry_incarnation == 11);
    CHECK(flip.lots()[0].entry_incarnation == 90 && flip.lots()[0].entry_id == "flip");
    CHECK(flip.rows()[0].exit_id == "flip");
    CHECK(flip.rows()[0].exit_time == 1736121660000LL && flip.rows()[0].exit_bar_index == 7);
    CHECK(flip.lots()[0].time == 1736121660000LL && flip.lots()[0].entry_bar_index == 7);
    CHECK(flip.cycle() == next_cycle && flip.next_cycle() == next_cycle + 1);
    CHECK(flip.actions() == actions + 2);
    REQUIRE(flip.pending().size() == 1);
    CHECK(flip.pending().data() == pending && flip.pending()[0].incarnation == 700);
    CHECK(flip.pending()[0].legs.target().owner == flip.cycle());

    Book class_c;
    class_c.open(held_sign, 100, 11);
    class_c.sequential(buy, price, 1.1);
    REQUIRE(class_c.lots().size() == 1 && class_c.rows().size() == 1);
    // These are the old and current F8 witness bits, not exact source Q bits.
    CHECK(bits(class_c.lots()[0].qty) == UINT64_C(0x3fb99999999999a0));
    CHECK(bits(class_c.lots()[0].qty) != bits(.1));
    exact(class_c.rows()[0].qty, 1);
    near(class_c.rows()[0].pnl, 10);

    Book class_b;
    class_b.open(held_sign, 100, 11);
    class_b.sequential(buy, price, .1);
    REQUIRE(class_b.lots().empty() && class_b.rows().size() == 1);
    exact(class_b.rows()[0].qty, 1);
    CHECK(class_b.cycle() == 0);
}

void f7_non_dyadic_roster(double sign) {
    scenario = "F7 closes non-dyadic roster and opens exact target with one ticket";
    Book book;
    book.open(sign * .1, 100, 11);
    book.open(sign * .2, 100, 12);
    book.open(sign * .3, 100, 13);
    book.fee(6);
    book.flip(sign < 0, 100, .1);
    REQUIRE(book.rows().size() == 3 && book.lots().size() == 1);
    exact(book.lots()[0].qty, .1);
    const double quantities[] = {.1, .2, .3};
    double paid = book.lots()[0].entry_commission_account;
    for (size_t index = 0; index < 3; ++index) {
        CHECK(book.rows()[index].entry_incarnation == 11 + index);
        exact(book.rows()[index].qty, quantities[index]);
        paid += book.rows()[index].commission;
    }
    near(paid, 6);
    near(book.marked(100), 994);
}

void f7_absorbed_quantities(double sign, bool tiny_target) {
    scenario = tiny_target ? "F7 accepts tiny target absorbed by old held quantity"
                           : "F7 accepts large target that absorbs old held quantity";
    const double held = tiny_target ? 1e16 : .1;
    const double quantity = tiny_target ? .1 : 1e16;
    Book book;
    book.open(sign * held, 1, 11);
    book.flip(sign < 0, 1, quantity);
    REQUIRE(book.lots().size() == 1 && book.rows().size() == 1);
    exact(book.lots()[0].qty, quantity);
    exact(book.rows()[0].qty, held);
}

void f7_zero_and_close_only(double sign) {
    for (bool close_only : {false, true}) {
        scenario = close_only ? "F7 close-only stays Flatten" : "F7 resolved zero stays Flatten";
        Book book;
        book.open(sign, 100, 11);
        book.retain_exit();
        const auto* pending = book.pending().data();
        const auto next_cycle = book.next_cycle();
        book.fee(6);
        book.flip(sign < 0, 100 + sign * 10, close_only ? 99 : 0, -1, true, close_only);
        REQUIRE(book.rows().size() == 1 && book.lots().empty());
        exact(book.rows()[0].qty, 1);
        near(book.rows()[0].commission, 6);
        CHECK(book.cycle() == 0 && book.next_cycle() == next_cycle);
        REQUIRE(book.pending().size() == 1);
        CHECK(book.pending().data() == pending && book.pending()[0].legs.target().owner == 0);
    }
}

void f7_cash_size(double sign) {
    scenario = "F7 cash sizing preserves pointvalue and current FX conversion";
    Book book;
    book.scale(2, 2);
    book.open(sign * 3, 100, 11);
    book.fee(6);
    book.flip(sign < 0, 100, 1000, static_cast<int>(QtyType::CASH), false);
    REQUIRE(book.lots().size() == 1 && book.rows().size() == 1);
    exact(book.quantity(), -sign * 2.5);
    near(book.rows()[0].commission + book.lots()[0].entry_commission_account, 6);
    near(book.marked(100), 994);
}

void f7_projected_percent(bool use_default) {
    scenario = use_default ? "F7 default percent keeps Flatten-projected sizing"
                           : "F7 explicit percent keeps Flatten-projected sizing";
    Book book;
    book.open(1, 100, 11);
    book.open(3, 100, 12);
    book.fee(6);
    book.default_percent(50);
    book.flip(false, 110, use_default ? std::numeric_limits<double>::quiet_NaN() : 50,
        use_default ? -1 : static_cast<int>(QtyType::PERCENT_OF_EQUITY), false);
    REQUIRE(book.lots().size() == 1 && book.rows().size() == 2);
    // Closing 4 @110 first quotes a balance of 1034 for sizing. Half at110 is4.7.
    exact(book.lots()[0].qty, 4.7000000000000002);
    near(book.rows()[0].commission, .68965517241379315);
    near(book.rows()[1].commission, 2.0689655172413794);
    near(book.lots()[0].entry_commission_account, 3.2413793103448274);
    near(book.balance(), 1037.2413793103448);
    near(book.marked(110), 1034);
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
    for (double sign : {1.0, -1.0}) {
        run([&] { exact_f7_and_unchanged_f8(sign); });
        run([&] { f7_non_dyadic_roster(sign); });
        run([&] { f7_absorbed_quantities(sign, true); });
        run([&] { f7_absorbed_quantities(sign, false); });
        run([&] { f7_zero_and_close_only(sign); });
        run([&] { f7_cash_size(sign); });
    }
    run([] { f7_projected_percent(false); });
    run([] { f7_projected_percent(true); });
    std::printf("%s source exact reversal: %d checks, %d failures\n",
        failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
