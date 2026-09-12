// Literal R2a kernel witnesses. No strategy driver, Pine policy, or helper oracle.
#include <pineforge/engine.hpp>
#include <pineforge/execution_close_scope.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int checks = 0, failures = 0;
const char* current_case = "setup";
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s line %d: %s\n", current_case, __LINE__, #x); } } while (0)

void near(double actual, double expected) {
    const bool same = std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected));
    if (!same) std::printf("actual=%.17g expected=%.17g\n", actual, expected);
    CHECK(same);
}

bool same_bits(double a, double b) {
    std::uint64_t aa = 0, bb = 0;
    static_assert(sizeof(aa) == sizeof(a));
    std::memcpy(&aa, &a, sizeof(a));
    std::memcpy(&bb, &b, sizeof(b));
    return aa == bb;
}

// Compare all declared lot fields, including NaN payloads, never object padding.
void same_lot(const PyramidEntry& a, const PyramidEntry& b) {
    CHECK(same_bits(a.price, b.price));
    CHECK(a.time == b.time);
    CHECK(same_bits(a.qty, b.qty));
    CHECK(a.entry_id == b.entry_id);
    CHECK(a.entry_bar_index == b.entry_bar_index);
    CHECK(a.entry_comment == b.entry_comment);
    CHECK(same_bits(a.max_runup, b.max_runup));
    CHECK(same_bits(a.max_drawdown, b.max_drawdown));
    CHECK(a.skip_entry_bar_high == b.skip_entry_bar_high);
    CHECK(a.skip_entry_bar_low == b.skip_entry_bar_low);
    CHECK(a.market_pyramid_add == b.market_pyramid_add);
    CHECK(same_bits(a.entry_path_position, b.entry_path_position));
    CHECK(same_bits(a.entry_commission_account, b.entry_commission_account));
    CHECK(a.entry_incarnation == b.entry_incarnation);
    CHECK(a.bracket_slot_shadowed == b.bracket_slot_shadowed);
    CHECK(a.ordinary_market_open == b.ordinary_market_open);
    CHECK(a.pooc_terminal_market_entry == b.pooc_terminal_market_entry);
    CHECK(a.ordinary_stop_open == b.ordinary_stop_open);
}

PyramidEntry lot(const char* label, std::uint64_t owner, double qty, double price,
                 double paid_cost, std::int64_t time, int index,
                 double runup = 60, double drawdown = 30) {
    PyramidEntry value{price, time, qty, label, index};
    value.entry_comment = std::string("entry-") + label;
    value.entry_incarnation = owner;
    value.entry_commission_account = paid_cost;
    value.max_runup = runup;
    value.max_drawdown = drawdown;
    return value;
}

std::vector<PyramidEntry> standard_lots() {
    return {lot("A", 11, 2, 100, 4, 1000, 10),
            lot("B", 22, 3, 110, 6, 2000, 20),
            lot("C", 33, 4, 120, 8, 3000, 30)};
}

std::vector<PyramidEntry> fragmented_lots() {
    return {lot("A", 11, 2, 100, 4, 1000, 10),
            lot("B1", 22, 1, 110, 2, 2000, 20, 20, 10),
            lot("C", 33, 4, 120, 8, 3000, 30),
            lot("B2", 22, 2, 115, 4, 4000, 40, 40, 20)};
}

class Fixture final : public BacktestEngine {
public:
    Fixture() {
        // Deliberately different from the supplied physical close coordinate.
        current_bar_ = {100, 125, 95, 120, 1, 60000};
        bar_index_ = 1;
        initial_capital_ = 10000;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 6;
        account_currency_fx_ = 1;
        syminfo_.pointvalue = 1;
        stream_observe_actions_ = true;
        slippage_ = 9;
        qty_step_ = 10;
        pyramiding_ = 1;
    }
    void on_bar(const Bar&) override {}

    void seed(std::vector<PyramidEntry> values, bool short_side = false,
              std::int64_t cycle = 7) {
        CHECK(pyramid_entries_.empty() && trades_.empty());
        CHECK(!values.empty());
        if (values.empty()) return;
        pyramid_entries_ = std::move(values);
        position_side_ = short_side ? PositionSide::SHORT : PositionSide::LONG;
        position_cycle_seq_ = cycle;
        next_position_cycle_seq_ = cycle + 1;
        position_open_bar_ = pyramid_entries_.front().entry_bar_index;
        position_entry_time_ = pyramid_entries_.front().time;
        position_entry_count_ = static_cast<int>(pyramid_entries_.size());
        position_qty_ = 0;
        double weighted = 0;
        for (const auto& entry : pyramid_entries_) {
            position_qty_ += entry.qty;
            weighted += entry.qty * entry.price;
            id_unclosed_qty_[entry.entry_id] += entry.qty;
            cycle_filled_entry_ids_.insert(entry.entry_id);
        }
        position_entry_price_ = weighted / position_qty_;
        trail_best_price_ = position_entry_price_;
    }

    execution::SettlementInspection inspect(const execution::Action& action,
                                            const execution::Fill& fill,
                                            execution::CloseScope scope) const {
        return inspect_native_settlement_scoped(action, fill, scope);
    }
    execution::Result settle(const execution::Action& action,
                             const execution::Fill& fill,
                             execution::CloseScope scope) {
        execution::PhysicalExecutionContext context;
        context.effective_time_ms = 9000;
        context.interval_index = 90;
        return settle_native_execution_scoped_at(action, fill, context, scope);
    }
    void fee(double cash) { commission_value_ = cash; }
    void fx(double value) { account_currency_fx_ = value; }
    double position() const { return signed_position_size(); }
    double average() const { return position_entry_price_; }
    double net() const { return net_profit_sum_; }
    double marked(double price) const { return marked_equity(price); }
    std::int64_t cycle() const { return position_cycle_seq_; }
    const auto& lots() const { return pyramid_entries_; }
    const auto& rows() const { return trades_; }
    const auto& actions() const { return stream_order_actions_; }
    std::uint64_t fingerprint() const { return broker_state_hash(); }
};

execution::CloseScope owner(std::uint64_t incarnation = 22, std::int64_t cycle = 7) {
    return execution::OpeningExposure{incarnation, cycle};
}
execution::Fill fill(double price = 120, std::optional<double> fee = {}) {
    return execution::Fill{price, "close-child", "scoped", 900, fee};
}

struct Expected {
    double closed;
    double signed_units;
    std::size_t lots;
    double average;
    double notional;
    double ticket;
    std::size_t rows;
    std::int64_t cycle = 7;
};

execution::Result applied(Fixture& book, const execution::Action& action,
                          execution::CloseScope scope, execution::Fill execution_fill,
                          const Expected& expected) {
    const auto fingerprint = book.fingerprint();
    const auto original_lots = book.lots();
    const auto first_row = book.rows().size();
    const auto first_action = book.actions().size();
    const auto inspection = book.inspect(action, execution_fill, scope);
    CHECK(book.fingerprint() == fingerprint);
    CHECK(book.lots().size() == original_lots.size());
    for (std::size_t i = 0; i < std::min(book.lots().size(), original_lots.size()); ++i)
        same_lot(book.lots()[i], original_lots[i]);
    CHECK(book.rows().size() == first_row && book.actions().size() == first_action);
    CHECK(inspection.status == execution::Status::Applied);
    CHECK(inspection.closed_units == expected.closed);
    CHECK(inspection.opened_units == 0);
    CHECK(!inspection.would_open);
    CHECK(inspection.resulting_abs_units == std::abs(expected.signed_units));
    CHECK(inspection.resulting_lot_count == expected.lots);
    near(inspection.resulting_abs_notional, expected.notional);
    near(inspection.current_ticket, expected.ticket);

    // The trusted caller locks the inspected execution cost. No saved index/plan.
    execution_fill.commission_account = inspection.current_ticket;
    const auto result = book.settle(action, execution_fill, scope);
    CHECK(result.status == inspection.status);
    CHECK(result.status == execution::Status::Applied);
    CHECK(result.closed_units == inspection.closed_units);
    CHECK(result.closed_units == expected.closed);
    if (std::holds_alternative<execution::OpeningExposure>(scope)) {
        CHECK(inspection.closed_units > 0);
        CHECK(result.closed_units > 0);
    }
    CHECK(result.opened_units == 0 && result.opened_lot_incarnation == 0);
    near(result.current_ticket, expected.ticket);
    CHECK(result.first_trade_index == first_row);
    CHECK(result.closed_trade_count == expected.rows);
    CHECK(book.rows().size() == first_row + expected.rows);
    CHECK(book.actions().size() == first_action + expected.rows);
    CHECK(book.lots().size() == expected.lots);
    CHECK(book.position() == expected.signed_units);
    near(book.average(), expected.average);
    CHECK(book.cycle() == expected.cycle);
    for (std::size_t i = first_row; i < book.rows().size(); ++i) {
        const auto& row = book.rows()[i];
        CHECK(row.exit_time == 9000 && row.exit_bar_index == 90);
        CHECK(row.exit_id == execution_fill.id && row.exit_comment == execution_fill.comment);
        CHECK(row.exit_price == execution_fill.price);
        CHECK(row.entry_incarnation != execution_fill.incarnation);
        if (const auto* selected = std::get_if<execution::OpeningExposure>(&scope))
            CHECK(row.entry_incarnation == selected->incarnation);
    }
    for (std::size_t i = first_action; i < book.actions().size(); ++i) {
        const auto& action_row = book.actions()[i];
        CHECK(!action_row.is_entry);
        CHECK(action_row.timestamp_ms == 9000 && action_row.bar_index == 90);
        CHECK(action_row.order_id == execution_fill.id && action_row.comment == execution_fill.comment);
        CHECK(action_row.closed_trade_index == first_row + i - first_action);
        CHECK(action_row.sequence == i + 1);
        if (action_row.closed_trade_index < book.rows().size()) {
            const auto& row = book.rows()[action_row.closed_trade_index];
            CHECK(action_row.entry_incarnation == row.entry_incarnation);
            CHECK(action_row.quantity == row.qty && action_row.price == row.exit_price);
            CHECK(action_row.is_long == row.is_long);
        }
    }
    return result;
}

void refused(Fixture& book, const execution::Action& action,
             execution::CloseScope scope, execution::Fill execution_fill,
             execution::Status status) {
    const auto fingerprint = book.fingerprint();
    const auto original_lots = book.lots();
    const auto row_count = book.rows().size();
    const auto action_count = book.actions().size();
    const auto net = book.net();
    const auto position = book.position();
    const auto cycle = book.cycle();
    const auto inspection = book.inspect(action, execution_fill, scope);
    CHECK(inspection.status == status);
    CHECK(book.fingerprint() == fingerprint);
    const auto result = book.settle(action, execution_fill, scope);
    CHECK(result.status == status && result.status == inspection.status);
    CHECK(result.closed_units == 0 && result.opened_units == 0);
    CHECK(result.current_ticket == 0 && result.closed_trade_count == 0);
    CHECK(result.opened_lot_incarnation == 0);
    CHECK(book.fingerprint() == fingerprint);
    CHECK(book.net() == net && book.position() == position && book.cycle() == cycle);
    CHECK(book.rows().size() == row_count && book.actions().size() == action_count);
    CHECK(book.lots().size() == original_lots.size());
    for (std::size_t i = 0; i < std::min(book.lots().size(), original_lots.size()); ++i)
        same_lot(book.lots()[i], original_lots[i]);
}

void check_row(const Fixture& book, std::size_t index, const char* id,
               double qty, double entry_price, double commission, double pnl,
               std::int64_t entry_time, int entry_index, bool is_long = true) {
    CHECK(index < book.rows().size());
    if (index >= book.rows().size()) return;
    const auto& row = book.rows()[index];
    CHECK(row.entry_incarnation == 22);
    CHECK(row.entry_id == id && row.entry_comment == std::string("entry-") + id);
    CHECK(row.qty == qty && row.entry_price == entry_price);
    CHECK(row.entry_time == entry_time && row.entry_bar_index == entry_index);
    CHECK(row.is_long == is_long);
    near(row.commission, commission);
    near(row.pnl, pnl);
}

void long_non_oldest() {
    current_case = "01-long-non-oldest";
    Fixture book;
    const auto initial = standard_lots();
    book.seed(initial);
    near(book.marked(120), 10052);
    applied(book, order_action::Reduce{1}, owner(), fill(), {1, 8, 3, 112.5, 960, 6, 1});
    check_row(book, 0, "B", 1, 110, 8, 2, 2000, 20);
    if (book.lots().size() == 3) {
        same_lot(book.lots()[0], initial[0]);
        same_lot(book.lots()[2], initial[2]);
        auto expected = initial[1];
        expected.qty = 2;
        expected.entry_commission_account = 4;
        expected.max_runup = 40;
        expected.max_drawdown = 20;
        same_lot(book.lots()[1], expected);
    }
    near(book.marked(120), 10046);
}

void short_non_oldest() {
    current_case = "02-short-non-oldest";
    Fixture book;
    const auto initial = standard_lots();
    book.seed(initial, true);
    near(book.marked(100), 10092);
    applied(book, order_action::Reduce{1}, owner(), fill(100), {1, -8, 3, 112.5, 800, 6, 1});
    check_row(book, 0, "B", 1, 110, 8, 2, 2000, 20, false);
    if (book.lots().size() == 3) {
        same_lot(book.lots()[0], initial[0]);
        same_lot(book.lots()[2], initial[2]);
        auto expected = initial[1];
        expected.qty = 2;
        expected.entry_commission_account = 4;
        expected.max_runup = 40;
        expected.max_drawdown = 20;
        same_lot(book.lots()[1], expected);
    }
    near(book.marked(100), 10086);
}

void repeated_fragments_partial() {
    current_case = "03-repeated-fragments-partial";
    Fixture book;
    const auto initial = fragmented_lots();
    book.seed(initial);
    applied(book, order_action::Reduce{2}, owner(), fill(), {2, 7, 3, 795.0 / 7, 840, 6, 2});
    check_row(book, 0, "B1", 1, 110, 5, 5, 2000, 20);
    check_row(book, 1, "B2", 1, 115, 5, 0, 4000, 40);
    if (book.lots().size() == 3) {
        same_lot(book.lots()[0], initial[0]);
        same_lot(book.lots()[1], initial[2]);
        auto expected = initial[3];
        expected.qty = 1;
        expected.entry_commission_account = 2;
        expected.max_runup = 20;
        expected.max_drawdown = 10;
        same_lot(book.lots()[2], expected);
    }
    near(book.net(), 5);
}

void repeated_fragments_flatten() {
    current_case = "04-repeated-fragments-flatten";
    Fixture book;
    const auto initial = fragmented_lots();
    book.seed(initial);
    near(book.marked(120), 10042);
    applied(book, execution::Flatten{}, owner(), fill(120, 9), {3, 6, 2, 680.0 / 6, 720, 9, 2});
    check_row(book, 0, "B1", 1, 110, 5, 5, 2000, 20);
    check_row(book, 1, "B2", 2, 115, 10, 0, 4000, 40);
    if (book.lots().size() == 2) {
        same_lot(book.lots()[0], initial[0]);
        same_lot(book.lots()[1], initial[2]);
    }
    near(book.net(), 5);
    near(book.marked(120), 10033);
}

void decimal_residue_then_flatten() {
    current_case = "05-decimal-residue";
    Fixture book;
    const auto sibling = lot("A", 11, 1, 100, 0, 1000, 10, 0, 0);
    book.seed({sibling, lot("B1", 22, 0.1, 100, 0, 2000, 20, 0, 0),
               lot("B2", 22, 0.2, 100, 0, 4000, 40, 0, 0)});
    book.fee(0);
    applied(book, order_action::Reduce{0.3}, owner(), fill(100), {0.3, 1, 2, 100, 100, 0, 2});
    if (book.lots().size() == 2) {
        same_lot(book.lots()[0], sibling);
        CHECK(book.lots()[1].qty == 0x1p-55);
        CHECK(book.lots()[1].entry_incarnation == 22);
        CHECK(book.lots()[1].time == 4000 && book.lots()[1].entry_bar_index == 40);
    }
    CHECK(book.rows().size() == 2);
    if (book.rows().size() == 2) {
        CHECK(book.rows()[0].qty == 0.1);
        CHECK(book.rows()[1].qty == 0x1.9999999999999p-3);
    }
    applied(book, execution::Flatten{}, owner(), fill(100), {0x1p-55, 1, 1, 100, 100, 0, 1});
    check_row(book, 2, "B2", 0x1p-55, 100, 0, 0, 4000, 40);
    if (book.lots().size() == 1) same_lot(book.lots()[0], sibling);
}

void oversized_selected_reduce() {
    current_case = "06-oversized-reduce";
    Fixture book;
    const auto initial = standard_lots();
    book.seed(initial);
    applied(book, order_action::Reduce{std::numeric_limits<double>::max()}, owner(), fill(),
            {3, 6, 2, 680.0 / 6, 720, 6, 1});
    check_row(book, 0, "B", 3, 110, 12, 18, 2000, 20);
    if (book.lots().size() == 2) {
        same_lot(book.lots()[0], initial[0]);
        same_lot(book.lots()[1], initial[2]);
    }
}

void historical_rebate_waiver() {
    current_case = "07-historical-rebate-waiver";
    Fixture book;
    const auto initial = standard_lots();
    book.seed(initial);
    book.fx(2);
    book.fee(99);
    near(book.marked(120), 10122);
    applied(book, order_action::Reduce{1}, owner(), fill(120, -6), {1, 8, 3, 112.5, 1920, -6, 1});
    check_row(book, 0, "B", 1, 110, -4, 24, 2000, 20);
    if (book.lots().size() == 3) near(book.lots()[1].entry_commission_account, 4);
    near(book.marked(120), 10128);
    book.fx(3);
    book.fee(77);
    near(book.marked(120), 10188);
    applied(book, execution::Flatten{}, owner(), fill(120, 0), {2, 6, 2, 680.0 / 6, 2160, 0, 1});
    check_row(book, 1, "B", 2, 110, 4, 56, 2000, 20);
    if (book.lots().size() == 2) {
        same_lot(book.lots()[0], initial[0]);
        same_lot(book.lots()[1], initial[2]);
    }
    near(book.net(), 80);
    near(book.marked(120), 10188);
}

void invalid_targets() {
    current_case = "08-invalid-targets";
    Fixture book;
    book.seed(standard_lots());
    for (const auto target : {execution::OpeningExposure{22, 6},
                              execution::OpeningExposure{22, 0},
                              execution::OpeningExposure{22, -1},
                              execution::OpeningExposure{0, 7},
                              execution::OpeningExposure{99, 7}})
        refused(book, order_action::Reduce{1}, target, fill(), execution::Status::InvalidCloseTarget);
    Fixture flat;
    refused(flat, execution::Flatten{}, owner(), fill(), execution::Status::InvalidCloseTarget);
    Fixture next_cycle;
    next_cycle.seed(standard_lots(), false, 8);
    refused(next_cycle, order_action::Reduce{1}, owner(), fill(), execution::Status::InvalidCloseTarget);
    applied(next_cycle, order_action::Reduce{1}, owner(22, 8), fill(), {1, 8, 3, 112.5, 960, 6, 1, 8});
}

void inspection_is_not_authority() {
    current_case = "09-inspection-not-authority";
    Fixture book;
    const auto initial = standard_lots();
    book.seed(initial);
    const auto before = book.fingerprint();
    const auto earlier = book.inspect(order_action::Reduce{1}, fill(), owner());
    CHECK(book.fingerprint() == before);
    CHECK(earlier.status == execution::Status::Applied);
    CHECK(earlier.closed_units == 1 && earlier.resulting_abs_units == 8);
    CHECK(earlier.resulting_lot_count == 3 && earlier.current_ticket == 6);
    applied(book, order_action::Reduce{2}, execution::Book{}, fill(120, 0),
            {2, 7, 2, 810.0 / 7, 840, 0, 1});
    if (book.lots().size() == 2) {
        same_lot(book.lots()[0], initial[1]);
        same_lot(book.lots()[1], initial[2]);
    }
    // Locked price/ticket and selector are reused; old roster indexes are not.
    const auto result = applied(book, order_action::Reduce{1}, owner(), fill(120, earlier.current_ticket),
                                {1, 6, 2, 700.0 / 6, 720, 6, 1});
    CHECK(result.first_trade_index == 1);
    check_row(book, 1, "B", 1, 110, 8, 2, 2000, 20);
    if (book.lots().size() == 2) same_lot(book.lots()[1], initial[2]);
    applied(book, execution::Flatten{}, owner(), fill(120, 0), {2, 4, 1, 120, 480, 0, 1});
    refused(book, order_action::Reduce{1}, owner(), fill(), execution::Status::InvalidCloseTarget);
    if (book.lots().size() == 1) same_lot(book.lots()[0], initial[2]);
}

void selected_transact_refused() {
    current_case = "10-selected-transact-refused";
    Fixture book;
    book.seed(standard_lots());
    for (double units : {1.0, -1.0, 0.0})
        refused(book, order_action::Transact{units}, owner(), fill(), execution::Status::InvalidCloseTarget);
}

void valid_target_validation() {
    current_case = "11-valid-target-validation";
    Fixture book;
    book.seed(standard_lots());
    refused(book, order_action::Reduce{0}, owner(), fill(), execution::Status::NoEffect);
    refused(book, order_action::Reduce{0}, owner(), fill(120, 0), execution::Status::NoEffect);
    for (double fee : {1.0, -1.0})
        refused(book, order_action::Reduce{0}, owner(), fill(120, fee), execution::Status::InvalidAccounting);
    for (double units : {-1.0, std::numeric_limits<double>::infinity()})
        refused(book, order_action::Reduce{units}, owner(), fill(), execution::Status::InvalidQuantity);
    refused(book, order_action::Reduce{1}, owner(), fill(std::numeric_limits<double>::quiet_NaN()),
            execution::Status::InvalidPrice);
    for (double fee : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        refused(book, order_action::Reduce{1}, owner(), fill(120, fee), execution::Status::InvalidAccounting);

    // Independent review's exact mixed-invalid precedence addendum. Target
    // validation follows ordinary value validation but precedes zero effects.
    refused(book, order_action::Reduce{1}, owner(0, 0),
            fill(std::numeric_limits<double>::quiet_NaN()), execution::Status::InvalidPrice);
    refused(book, order_action::Transact{std::numeric_limits<double>::quiet_NaN()}, owner(),
            fill(), execution::Status::InvalidQuantity);
    refused(book, order_action::Transact{0}, owner(), fill(120, 0),
            execution::Status::InvalidCloseTarget);
    refused(book, order_action::Reduce{0}, owner(99, 7), fill(120, 1),
            execution::Status::InvalidCloseTarget);
    Fixture flat;
    refused(flat, execution::Flatten{}, owner(), fill(120, 0),
            execution::Status::InvalidCloseTarget);
}

void huge_unrelated_sibling() {
    current_case = "12-huge-unrelated-sibling";
    Fixture book;
    const auto sibling = lot("A", 11, 0x1p60, 1, 0, 1000, 10, 0, 0);
    book.seed({sibling, lot("B", 22, 1, 1, 0, 2000, 20, 0, 0)});
    CHECK(book.position() == 0x1p60);
    near(book.marked(1), 10000);
    applied(book, order_action::Reduce{1}, owner(), fill(1), {1, 0x1p60, 1, 1, 0x1p60, 6, 1});
    check_row(book, 0, "B", 1, 1, 6, -6, 2000, 20);
    CHECK(book.position() == 0x1p60);
    if (book.lots().size() == 1) same_lot(book.lots()[0], sibling);
    near(book.marked(1), 9994);
}
} // namespace

int main() {
    static_assert(static_cast<int>(execution::Status::InvalidCloseTarget) == 8);
    long_non_oldest();
    short_non_oldest();
    repeated_fragments_partial();
    repeated_fragments_flatten();
    decimal_residue_then_flatten();
    oversized_selected_reduce();
    historical_rebate_waiver();
    invalid_targets();
    inspection_is_not_authority();
    selected_transact_refused();
    valid_target_validation();
    huge_unrelated_sibling();
    std::printf("%s test_native_scoped_close: %d checks, %d failures, 12 cases\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
