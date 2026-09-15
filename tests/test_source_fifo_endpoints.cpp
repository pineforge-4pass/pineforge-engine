// Literal source FIFO endpoint calls paired with the actual native Reduce owner.
// No BacktestEngine::run(), generated strategy, tape, corpus or grading loop.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace pineforge;
using pineforge::source::PendingOrder;
namespace x = pineforge::execution;

namespace {
int checks = 0, failures = 0;
const char* scenario = "setup";
struct Abort {};
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #value); } } while (0)
#define REQUIRE(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #value); throw Abort{}; } } while (0)

template<class Tag, auto Member> struct Access { friend auto access(Tag) { return Member; } };
struct Partial { friend auto access(Partial); };
struct ByQuantity { friend auto access(ByQuantity); };
struct Drain { friend auto access(Drain); };
struct ComputeClose { friend auto access(ComputeClose); };
struct ImmediateClose { friend auto access(ImmediateClose); };
struct ExitFill { friend auto access(ExitFill); };
template struct Access<Partial, &pineforge::source::PineStrategyHost::execute_partial_exit_qty>;
template struct Access<ByQuantity, &pineforge::source::PineStrategyHost::execute_partial_exit_by_entry_qty>;
template struct Access<Drain, &BacktestEngine::fifo_drain>;
template struct Access<ComputeClose, &pineforge::source::PineStrategyHost::compute_close_target_qty>;
template struct Access<ImmediateClose, &pineforge::source::PineStrategyHost::execute_immediate_close>;
template struct Access<ExitFill, &pineforge::source::PineStrategyHost::apply_exit_order_fill>;
template<class> struct Args;
template<class R, class C, class... A> struct Args<R(C::*)(A...)> { using tuple = std::tuple<A...>; };
using Cause = std::tuple_element_t<2, typename Args<decltype(access(Partial{}))>::tuple>;
constexpr Cause Script = static_cast<Cause>(0), Bracket = static_cast<Cause>(1), Margin = static_cast<Cause>(2);

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

struct Book final : pineforge::source::PineStrategyHost {
    Book() {
        initial_capital_ = 1000;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 0;
        syminfo_.pointvalue = 1;
        syminfo_.mintick = .01;
        syminfo_mintick_ = .01;
        account_currency_fx_ = 1;
        qty_step_ = 0;
        slippage_ = 0;
        pyramiding_ = 100;
        stream_observe_actions_ = true;
        bar(1, 100);
    }
    void on_source_bar(const Bar&) override {}
    void bar(int index, double price) {
        current_bar_ = {price, price + 20, price - 20, price, 1, 1736121600000LL + index * 60000};
        bar_index_ = index;
    }
    x::PhysicalExecutionContext context() const { return {current_bar_.timestamp, bar_index_, {}, {}}; }
    void open(double quantity, double price, uint64_t incarnation, const char* label,
              std::optional<double> paid = 0.0) {
        REQUIRE(settle_native_execution_at(order_action::Transact{quantity},
            x::Fill{price, label, "historical", incarnation, paid}, context()).status == x::Status::Applied);
    }
    void source(double quantity, double price = 100, Cause cause = Script) {
        (this->*access(Partial{}))(price, quantity, cause);
    }
    void native(double quantity, double price = 100) {
        REQUIRE(settle_native_execution_at(order_action::Reduce{quantity},
            x::Fill{price, {}, {}, 0}, context()).status == x::Status::Applied);
    }
    void by_entry(double quantity, double price = 100) {
        (this->*access(ByQuantity{}))(price, std::string("prefix"), quantity, Script);
    }
    void compatibility_drain(double quantity, double price = 100) {
        (void)(this->*access(Drain{}))(nullptr, quantity, price, position_side_ == PositionSide::LONG);
    }
    void close_logical(const char* id) {
        const double na = std::numeric_limits<double>::quiet_NaN();
        double matching = 0, quantity = 0, retired = 0;
        bool all = false;
        REQUIRE((this->*access(ComputeClose{}))(std::string(id), na, na, false,
            matching, quantity, all, retired));
        exact(matching, .8);
        exact(quantity, .8);
        CHECK(!all && retired == 0);
        (this->*access(ImmediateClose{}))(std::string(id), std::string("funded close"),
            quantity, matching, false, true, false, false, false);
    }
    void exit(PendingOrder& order, double price) {
        int closed_bar = -1;
        uint64_t closed_incarnation = 0;
        bool closed_long = false;
        (this->*access(ExitFill{}))(order, price, closed_bar, closed_incarnation, closed_long);
        CHECK(closed_bar == -1 && closed_incarnation == 0 && !closed_long);
    }
    void schedule(CommissionType kind, double value, double pointvalue = 1, double fx = 1) {
        commission_type_ = kind;
        commission_value_ = value;
        syminfo_.pointvalue = pointvalue;
        account_currency_fx_ = fx;
    }
    void funded(const char* id, double quantity) { id_unclosed_qty_[id] = quantity; }
    bool funded(const char* id) const { return id_unclosed_qty_.count(id) != 0; }
    void slots(int count) { position_entry_count_ = count; }
    int slots() const { return position_entry_count_; }
    void slippage(int ticks, double tick) { slippage_ = ticks; syminfo_mintick_ = tick; syminfo_.mintick = tick; }
    const auto& lots() const { return pyramid_entries_; }
    auto& lots() { return pyramid_entries_; }
    const auto& rows() const { return trades_; }
    size_t actions() const { return stream_order_actions_.size(); }
    int64_t cycle() const { return position_cycle_seq_; }
    int64_t next_cycle() const { return next_position_cycle_seq_; }
    void cycle_for_fixture(int64_t value) { position_cycle_seq_ = value; }
    double quantity() const { return position_qty_; }
    double balance() const { return initial_capital_ + net_profit_sum_; }
    int index() const { return bar_index_; }
    int64_t timestamp() const { return current_bar_.timestamp; }
    uint64_t broker_fills() const { return broker_fill_event_seq_; }
};

void same_lot(const PyramidEntry& actual, const PyramidEntry& expected) {
    exact(actual.qty, expected.qty);
    exact(actual.price, expected.price);
    exact(actual.entry_commission_account, expected.entry_commission_account);
    exact(actual.max_runup, expected.max_runup);
    exact(actual.max_drawdown, expected.max_drawdown);
    exact(actual.entry_path_position, expected.entry_path_position);
    CHECK(actual.time == expected.time && actual.entry_bar_index == expected.entry_bar_index);
    CHECK(actual.entry_id == expected.entry_id && actual.entry_comment == expected.entry_comment);
    CHECK(actual.entry_incarnation == expected.entry_incarnation);
    CHECK(actual.skip_entry_bar_high == expected.skip_entry_bar_high);
    CHECK(actual.skip_entry_bar_low == expected.skip_entry_bar_low);
    CHECK(actual.market_pyramid_add == expected.market_pyramid_add);
    CHECK(actual.bracket_slot_shadowed == expected.bracket_slot_shadowed);
    CHECK(actual.ordinary_market_open == expected.ordinary_market_open);
    CHECK(actual.pooc_terminal_market_entry == expected.pooc_terminal_market_entry);
    CHECK(actual.ordinary_stop_open == expected.ordinary_stop_open);
}
void same_row(const Trade& actual, const Trade& expected) {
    exact(actual.qty, expected.qty);
    exact(actual.entry_price, expected.entry_price);
    exact(actual.exit_price, expected.exit_price);
    exact(actual.pnl, expected.pnl);
    exact(actual.pnl_pct, expected.pnl_pct);
    exact(actual.commission, expected.commission);
    exact(actual.max_runup, expected.max_runup);
    exact(actual.max_drawdown, expected.max_drawdown);
    CHECK(actual.entry_time == expected.entry_time && actual.exit_time == expected.exit_time);
    CHECK(actual.entry_bar_index == expected.entry_bar_index && actual.exit_bar_index == expected.exit_bar_index);
    CHECK(actual.entry_incarnation == expected.entry_incarnation && actual.is_long == expected.is_long);
    CHECK(actual.entry_id == expected.entry_id && actual.exit_id == expected.exit_id);
    CHECK(actual.entry_comment == expected.entry_comment && actual.exit_comment == expected.exit_comment);
    CHECK(actual.exit_from_bracket == expected.exit_from_bracket && actual.open_at_end == expected.open_at_end);
}
void same_native_effects(const Book& source, const Book& native) {
    REQUIRE(source.rows().size() == native.rows().size());
    REQUIRE(source.lots().size() == native.lots().size());
    for (size_t index = 0; index < source.rows().size(); ++index) same_row(source.rows()[index], native.rows()[index]);
    for (size_t index = 0; index < source.lots().size(); ++index) same_lot(source.lots()[index], native.lots()[index]);
    exact(source.quantity(), native.quantity());
    exact(source.balance(), native.balance());
    CHECK(source.cycle() == native.cycle() && source.next_cycle() == native.next_cycle());
    CHECK(source.actions() == native.actions());
}
void seed(Book& book, double sign, const std::vector<uint64_t>& identities = {11, 12, 13},
          const char* sibling_label = "sibling") {
    REQUIRE(identities.size() == 3);
    book.open(sign * .7, 100, identities[0], "prefix", 7);
    book.bar(2, 100);
    book.open(sign * .1, 100, identities[1], "prefix", 1);
    book.bar(3, 100);
    book.open(sign, 100, identities[2], sibling_label, 17);
    book.lots().back().max_runup = .123;
    book.lots().back().max_drawdown = .456;
    book.lots().back().entry_path_position = .75;
    book.lots().back().skip_entry_bar_low = true;
    book.bar(7, 100);
}

void endpoint(double sign, double request) {
    scenario = "source complete FIFO endpoint closes exact prefix and preserves sibling";
    Book book;
    seed(book, sign);
    book.schedule(CommissionType::CASH_PER_ORDER, 6);
    const auto sibling = book.lots()[2];
    const auto cycle = book.cycle(), next_cycle = book.next_cycle();
    const auto actions = book.actions();
    const auto time = book.timestamp();
    book.source(request);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    exact(book.rows()[0].qty, .7);
    exact(book.rows()[1].qty, .1);
    CHECK(book.rows()[0].entry_incarnation == 11 && book.rows()[1].entry_incarnation == 12);
    same_lot(book.lots()[0], sibling);
    exact(book.quantity(), 1);
    CHECK(book.cycle() == cycle && book.next_cycle() == next_cycle);
    CHECK(book.actions() == actions + 2);
    CHECK(book.timestamp() == time && book.index() == 7);
    CHECK(book.slots() == 1);
    for (const auto& row : book.rows()) {
        CHECK(row.exit_time == time && row.exit_bar_index == 7);
        exact(row.exit_price, 100);
    }
    near(book.rows()[0].commission + book.rows()[1].commission, 14); // paid8 + one current6
    near(book.balance(), 986);
}

void reduce_control(double sign, double request) {
    scenario = "real interior source quantity retains original native Reduce effects";
    Book source, native;
    seed(source, sign);
    seed(native, sign);
    source.schedule(CommissionType::CASH_PER_ORDER, 6);
    native.schedule(CommissionType::CASH_PER_ORDER, 6);
    source.source(request);
    native.native(request);
    same_native_effects(source, native);
    if (request < .8) {
        REQUIRE(source.lots().size() == 2 && source.rows().size() == 2);
        CHECK(source.lots()[0].qty > 1e-10 && source.lots()[0].entry_incarnation == 12);
        exact(source.lots()[1].qty, 1);
    } else {
        REQUIRE(source.lots().size() == 1 && source.rows().size() == 3);
        CHECK(source.rows()[2].qty > 0 && source.rows()[2].entry_incarnation == 13);
        CHECK(source.lots()[0].qty < 1);
    }
}

void native_spill_and_scope_walls(double sign) {
    scenario = "native Reduce and retained compatibility drain preserve exact spill";
    Book native, drain;
    seed(native, sign);
    seed(drain, sign);
    native.native(.8);
    drain.compatibility_drain(.8);
    same_native_effects(drain, native);
    REQUIRE(native.rows().size() == 3 && native.lots().size() == 1);
    exact(native.rows()[2].qty, 1.1102230246251565e-16);
    exact(native.lots()[0].qty, .99999999999999989);
    CHECK(native.rows()[2].entry_incarnation == 13);

    scenario = "entry-scoped interior dust stays outside source FIFO translation";
    Book scoped;
    scoped.open(sign, 100, 11, "prefix", 6);
    scoped.open(sign * 3, 100, 12, "sibling", 17);
    const auto sibling = scoped.lots()[1];
    const double request = 1 - 5e-11;
    scoped.by_entry(request);
    REQUIRE(scoped.rows().size() == 1 && scoped.lots().size() == 2);
    exact(scoped.rows()[0].qty, request);
    CHECK(scoped.lots()[0].qty > 0 && scoped.lots()[0].qty < 1e-10);
    same_lot(scoped.lots()[1], sibling);
}

void identity_fallback(double sign, const std::vector<uint64_t>& identities) {
    scenario = "unowned or split physical identities fall back without scope expansion";
    Book source, native;
    seed(source, sign, identities);
    seed(native, sign, identities);
    source.source(.8);
    native.native(.8);
    same_native_effects(source, native);
    REQUIRE(source.rows().size() == 3 && source.lots().size() == 1);
    exact(source.rows()[2].qty, 1.1102230246251565e-16);
    CHECK(source.lots()[0].entry_incarnation == identities[2]);
}

void complete_fragments(double sign) {
    scenario = "complete repeated identity inside prefix is selected once and closes all fragments";
    Book book;
    seed(book, sign, {11, 11, 13});
    const auto sibling = book.lots()[2];
    book.source(.8);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    CHECK(book.rows()[0].entry_incarnation == 11 && book.rows()[1].entry_incarnation == 11);
    exact(book.rows()[0].qty, .7);
    exact(book.rows()[1].qty, .1);
    same_lot(book.lots()[0], sibling);
}

void unavailable_selection_cycle(double sign) {
    scenario = "unavailable selected opening cycle keeps scalar source behavior";
    Book source, native;
    seed(source, sign);
    seed(native, sign);
    source.cycle_for_fixture(0);
    native.cycle_for_fixture(0);
    source.source(.8);
    native.native(.8);
    same_native_effects(source, native);
    REQUIRE(source.rows().size() == 3 && source.lots().size() == 1);
    exact(source.rows()[2].qty, 1.1102230246251565e-16);
}

void stop_before_tiny_sibling(double sign) {
    scenario = "source stops before next sibling without using its tiny size";
    Book book;
    book.open(sign * .7, 100, 11, "prefix");
    book.open(sign * .1, 100, 12, "prefix");
    book.open(sign * 5e-11, 100, 13, "tiny-unselected", .25);
    book.open(sign, 100, 14, "later", 17);
    const auto tiny = book.lots()[2], later = book.lots()[3];
    book.source(.8);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 2);
    same_lot(book.lots()[0], tiny);
    same_lot(book.lots()[1], later);
}

void logical_funding_physical_fifo(double sign) {
    scenario = "logical close funding selects oldest physical FIFO identities";
    Book book;
    seed(book, sign, {11, 12, 13}, "L5");
    // The logical credit has diverged from L5's live physical1 after prior
    // default-FIFO attribution. It funds .8 while the old prefix bears other IDs.
    book.funded("L5", .8);
    const auto sibling = book.lots()[2];
    const auto fills = book.broker_fills();
    book.close_logical("L5");
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    CHECK(book.rows()[0].entry_incarnation == 11 && book.rows()[1].entry_incarnation == 12);
    CHECK(book.rows()[0].entry_id == "prefix" && book.rows()[1].entry_id == "prefix");
    CHECK(book.rows()[0].exit_id == "__close__L5" && book.rows()[1].exit_id == "__close__L5");
    CHECK(book.rows()[0].exit_comment == "funded close" && book.rows()[1].exit_comment == "funded close");
    CHECK(!book.funded("L5"));
    CHECK(book.broker_fills() == fills + 1);
    same_lot(book.lots()[0], sibling);
}

void frozen_reservation(double sign) {
    scenario = "priced frozen reservation closes old prefix and preserves newer sibling";
    Book book;
    book.open(sign * .7, 100, 11, "old-first", 7);
    book.bar(2, 100);
    book.open(sign * .1, 100, 12, "old-second", 1);
    PendingOrder order{};
    order.type = OrderType::EXIT;
    order.id = "frozen-basket";
    order.from_entry = "";
    order.qty = .8;
    order.qty_percent = 100;
    order.incarnation = 90;
    order.created_seq = 90;
    order.created_bar = 3;
    order.created_position_side = sign > 0 ? PositionSide::LONG : PositionSide::SHORT;
    order.created_position_cycle_seq = book.cycle();
    order.quantity_request.request(QuantityIntent::units(.8));
    order.quantity_request.reserve(.8, .8);
    order.legs.set_limit_price(100);
    book.bar(4, 100);
    book.open(sign, 100, 13, "newer", 17);
    const auto newer = book.lots()[2];
    const auto cycle = book.cycle(), next_cycle = book.next_cycle();
    book.bar(7, 100);
    book.schedule(CommissionType::CASH_PER_ORDER, 6);
    book.slots(9);
    book.exit(order, 100);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    exact(book.rows()[0].qty, .7);
    exact(book.rows()[1].qty, .1);
    same_lot(book.lots()[0], newer);
    exact(order.qty, .8);
    CHECK(book.cycle() == cycle && book.next_cycle() == next_cycle && book.slots() == 9);
    CHECK(book.rows()[0].exit_time == book.timestamp() && book.rows()[1].exit_time == book.timestamp());
    near(book.rows()[0].commission + book.rows()[1].commission, 14);
}

void fee_schedule(double sign, CommissionType type, double fee,
                  double expected_first, double expected_second, double expected_balance) {
    scenario = "source prefix uses one existing commission quote and full paid historical costs";
    Book book;
    seed(book, sign);
    const auto sibling = book.lots()[2];
    book.schedule(type, fee);
    // Wipe-side endpoint must realize full .1 and all historical cost1.
    book.source(.8 - 5e-11);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    exact(book.rows()[0].qty, .7);
    exact(book.rows()[1].qty, .1);
    near(book.rows()[0].commission, expected_first);
    near(book.rows()[1].commission, expected_second);
    near(book.balance(), expected_balance);
    same_lot(book.lots()[0], sibling);
}

void historical_fx(double sign) {
    scenario = "prefix Flatten realizes historical percentage costs at original FX";
    Book book;
    book.schedule(CommissionType::PERCENT, 1, 2, 2);
    book.open(sign * .7, 100, 11, "prefix", std::nullopt);
    book.open(sign * .1, 100, 12, "prefix", std::nullopt);
    book.open(sign, 100, 13, "sibling", std::nullopt);
    near(book.lots()[0].entry_commission_account, 2.8);
    near(book.lots()[1].entry_commission_account, .4);
    const auto sibling = book.lots()[2];
    book.schedule(CommissionType::PERCENT, 1, 2, 3);
    book.source(.8 - 5e-11);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    near(book.rows()[0].commission, 7);
    near(book.rows()[1].commission, 1);
    near(book.balance(), 992);
    same_lot(book.lots()[0], sibling);
}

void source_slots_price_and_clock(double sign, Cause cause) {
    scenario = "prefix settlement retains source slot policy and applies slippage once";
    Book book;
    seed(book, sign);
    const auto sibling = book.lots()[2];
    const auto cycle = book.cycle(), next_cycle = book.next_cycle();
    const auto timestamp = book.timestamp();
    book.slots(9);
    book.slippage(2, .25);
    book.source(.8, 110, cause);
    REQUIRE(book.rows().size() == 2 && book.lots().size() == 1);
    CHECK(book.slots() == (cause == Bracket ? 9 : 1));
    CHECK(book.cycle() == cycle && book.next_cycle() == next_cycle);
    CHECK(book.timestamp() == timestamp && book.index() == 7);
    same_lot(book.lots()[0], sibling);
    for (const auto& row : book.rows()) {
        exact(row.exit_price, 110 - sign * .5);
        CHECK(row.exit_time == timestamp && row.exit_bar_index == 7);
    }
}

void whole_book_and_noop(double sign) {
    scenario = "existing whole-book source endpoint promotion remains unchanged";
    Book book;
    seed(book, sign);
    const auto next = book.next_cycle();
    book.source(book.quantity() - 5e-11);
    REQUIRE(book.rows().size() == 3 && book.lots().empty());
    exact(book.rows()[0].qty, .7);
    exact(book.rows()[1].qty, .1);
    exact(book.rows()[2].qty, 1);
    CHECK(book.cycle() == 0 && book.next_cycle() == next);

    scenario = "empty sub-epsilon source prefix remains a no-op";
    Book noop;
    seed(noop, sign);
    const auto broker = noop.broker_state_hash(), stream = noop.stream_state_hash();
    const auto actions = noop.actions();
    noop.source(5e-11);
    CHECK(noop.broker_state_hash() == broker && noop.stream_state_hash() == stream);
    CHECK(noop.rows().empty() && noop.lots().size() == 3 && noop.actions() == actions);
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
        for (double quantity : {.8, .8 - 5e-11, .8 + 5e-11})
            run([&] { endpoint(sign, quantity); });
        for (double quantity : {.8 - 2e-10, .8 + 1e-10, .75, .85})
            run([&] { reduce_control(sign, quantity); });
        run([&] { native_spill_and_scope_walls(sign); });
        for (const auto& identities : {std::vector<uint64_t>{0, 0, 13}, {0, 12, 13}, {11, 0, 13}, {11, 12, 11}})
            run([&] { identity_fallback(sign, identities); });
        run([&] { complete_fragments(sign); });
        run([&] { unavailable_selection_cycle(sign); });
        run([&] { stop_before_tiny_sibling(sign); });
        run([&] { logical_funding_physical_fifo(sign); });
        run([&] { frozen_reservation(sign); });
        run([&] { fee_schedule(sign, CommissionType::CASH_PER_ORDER, 6, 12.25, 1.75, 986); });
        run([&] { fee_schedule(sign, CommissionType::CASH_PER_ORDER, -6, 1.75, .25, 998); });
        run([&] { fee_schedule(sign, CommissionType::CASH_PER_CONTRACT, 2, 8.4, 1.2, 990.4); });
        run([&] { fee_schedule(sign, CommissionType::PERCENT, 1, 7.7, 1.1, 991.2); });
        run([&] { historical_fx(sign); });
        for (Cause cause : {Script, Bracket, Margin})
            run([&] { source_slots_price_and_clock(sign, cause); });
        run([&] { whole_book_and_noop(sign); });
    }
    std::printf("%s source FIFO endpoints: %d checks, %d failures\n",
        failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
