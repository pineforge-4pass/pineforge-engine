#pragma once

#include "native_current_fixture.hpp"
#include <pineforge/run_failure.hpp>
#include <pineforge/trade_ledger.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace p2cx_tledger {
using namespace r4_test;

inline bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

inline Trade row(int n) {
    Trade value{};
    value.entry_time = T + n * 60000LL;
    value.exit_time = value.entry_time + 60000;
    value.entry_price = 100.0 + n;
    value.exit_price = 101.0 + n;
    value.qty = 0.25 * (n + 1);
    value.pnl = n % 3 == 0 ? -0.0 : (n % 3 == 1 ? -1.5 : 2.5);
    value.pnl_pct = value.pnl / value.entry_price;
    value.is_long = n % 2 == 0;
    value.entry_bar_index = n;
    value.exit_bar_index = n + 1;
    value.entry_id = std::string(value.is_long ? "Long" : "Short") + std::to_string(n);
    value.entry_comment = std::string("entry\0comment", 13) + std::string(20, 'a');
    value.exit_id = "exit-" + std::to_string(n);
    value.exit_comment = std::string(32, 'x') + std::to_string(n);
    value.exit_from_bracket = n % 2 != 0;
    value.max_runup = n + 0.5;
    value.max_drawdown = n + 0.25;
    value.commission = n * 0.125;
    value.entry_incarnation = 1000 + n;
    value.close_cause = ex::CloseCause::Bracket;
    return value;
}

// Compare every field, including floating-point object representations and
// embedded NULs. The expected rows are constructed separately from the ledger.
inline void same_trade(const Trade& actual, const Trade& expected) {
    CHECK(actual.entry_time == expected.entry_time);
    CHECK(actual.exit_time == expected.exit_time);
    CHECK(same_bits(actual.entry_price, expected.entry_price));
    CHECK(same_bits(actual.exit_price, expected.exit_price));
    CHECK(same_bits(actual.qty, expected.qty));
    CHECK(same_bits(actual.pnl, expected.pnl));
    CHECK(same_bits(actual.pnl_pct, expected.pnl_pct));
    CHECK(actual.is_long == expected.is_long);
    CHECK(actual.entry_bar_index == expected.entry_bar_index);
    CHECK(actual.exit_bar_index == expected.exit_bar_index);
    CHECK(actual.entry_id == expected.entry_id);
    CHECK(actual.entry_comment == expected.entry_comment);
    CHECK(actual.exit_id == expected.exit_id);
    CHECK(actual.exit_comment == expected.exit_comment);
    CHECK(actual.exit_from_bracket == expected.exit_from_bracket);
    CHECK(same_bits(actual.max_runup, expected.max_runup));
    CHECK(same_bits(actual.max_drawdown, expected.max_drawdown));
    CHECK(same_bits(actual.commission, expected.commission));
    CHECK(actual.entry_incarnation == expected.entry_incarnation);
    CHECK(actual.open_at_end == expected.open_at_end);
    CHECK(actual.close_cause == expected.close_cause);
}

inline void same_numeric(const Trade& actual, Trade expected) {
    expected.entry_id.clear();
    expected.entry_comment.clear();
    expected.exit_comment.clear();
    expected.exit_id.clear();
    same_trade(actual, expected);
}

inline void same_numeric_fields(const TradeLedger& ledger, std::size_t i,
                                const Trade& expected) {
    CHECK(same_bits(ledger.pnl_at(i), expected.pnl));
    CHECK(same_bits(ledger.pnl_pct_at(i), expected.pnl_pct));
    CHECK(same_bits(ledger.commission_at(i), expected.commission));
    CHECK(ledger.entry_bar_index_at(i) == expected.entry_bar_index);
    CHECK(ledger.exit_bar_index_at(i) == expected.exit_bar_index);
    CHECK(ledger.entry_incarnation_at(i) == expected.entry_incarnation);
    CHECK(same_bits(ledger.entry_price_at(i), expected.entry_price));
    CHECK(same_bits(ledger.exit_price_at(i), expected.exit_price));
    CHECK(ledger.entry_time_at(i) == expected.entry_time);
    CHECK(ledger.exit_time_at(i) == expected.exit_time);
    CHECK(same_bits(ledger.qty_at(i), expected.qty));
    CHECK(same_bits(ledger.max_runup_at(i), expected.max_runup));
    CHECK(same_bits(ledger.max_drawdown_at(i), expected.max_drawdown));
    CHECK(ledger.is_long_at(i) == expected.is_long);
}

inline void same_string_fields(const TradeLedger& ledger, std::size_t i,
                               const Trade& expected) {
    CHECK(ledger.entry_id_at(i) == expected.entry_id);
    CHECK(ledger.entry_comment_at(i) == expected.entry_comment);
    CHECK(ledger.exit_comment_at(i) == expected.exit_comment);
    CHECK(ledger.exit_id_at(i) == expected.exit_id);
}

struct Reader : Host {
    TradeLedger& ledger() { return trades_; }
    const TradeLedger& ledger() const { return trades_; }

    void compare_numeric_fields(int i, const Trade& expected) const {
        CHECK(same_bits(closed_trade_profit(i), expected.pnl));
        CHECK(same_bits(closed_trade_profit_percent(i), expected.pnl_pct));
        CHECK(same_bits(closed_trade_commission(i), expected.commission));
        CHECK(closed_trade_entry_bar_index(i) == expected.entry_bar_index);
        CHECK(closed_trade_exit_bar_index(i) == expected.exit_bar_index);
        CHECK(closed_trade_entry_incarnation(i) == expected.entry_incarnation);
        CHECK(same_bits(closed_trade_entry_price(i), expected.entry_price));
        CHECK(same_bits(closed_trade_exit_price(i), expected.exit_price));
        CHECK(closed_trade_entry_time(i) == expected.entry_time);
        CHECK(closed_trade_exit_time(i) == expected.exit_time);
        CHECK(same_bits(closed_trade_size(i), expected.qty));
        CHECK(same_bits(closed_trade_max_runup(i), expected.max_runup));
        CHECK(same_bits(closed_trade_max_drawdown(i), expected.max_drawdown));
        const double cost = expected.entry_price * expected.qty;
        CHECK(same_bits(closed_trade_max_runup_percent(i),
                        expected.max_runup / cost * 100.0));
        CHECK(same_bits(closed_trade_max_drawdown_percent(i),
                        expected.max_drawdown / cost * 100.0));
    }

    void compare_string_fields(int i, const Trade& expected) const {
        CHECK(closed_trade_entry_comment(i) == expected.entry_comment);
        CHECK(closed_trade_exit_comment(i) == expected.exit_comment);
        CHECK(closed_trade_entry_id(i) == expected.entry_id);
        CHECK(closed_trade_exit_id(i) == expected.exit_id);
        CHECK(closed_trade_direction(i) == (expected.is_long ? "long" : "short"));
    }

    void compare_fields(int i, const Trade& expected) const {
        same_trade(get_trade(i), expected);
        compare_numeric_fields(i, expected);
        compare_string_fields(i, expected);
        const auto cause = expected.open_at_end ? ex::CloseCause::RangeEnd : expected.close_cause;
        CHECK(closed_trade_close_cause(i) == static_cast<int>(cause));
    }
};
} // namespace p2cx_tledger
