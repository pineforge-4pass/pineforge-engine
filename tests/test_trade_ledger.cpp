// CR-150 L8a: exercise the frozen ledger contract and its retaining native path.
#include <pineforge/trade_ledger.hpp>
#include <pineforge/run_failure.hpp>
#include "native_current_fixture.hpp"

#include <cstring>
#include <iterator>
#include <type_traits>
#include <utility>

using namespace r4_test;

namespace {

struct LedgerProbe : Host {
    using ClosedRows = decltype(trades_);
    static_assert(std::is_same_v<ClosedRows, TradeLedger>,
                  "L8a must replace the closed-row vector with TradeLedger");
    const auto& ledger() const { return trades_; }
};

bool same_double(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

Trade row(int n) {
    Trade t{};
    t.entry_time = T + n * 60000LL;
    t.exit_time = t.entry_time + 60000;
    t.entry_price = 100.0 + n;
    t.exit_price = 101.0 + n;
    t.qty = 0.25 * (n + 1);
    t.pnl = n % 3 == 0 ? -0.0 : (n % 3 == 1 ? -1.5 : 2.5);
    t.pnl_pct = t.pnl / t.entry_price;
    t.is_long = n % 2 == 0;
    t.entry_bar_index = n;
    t.exit_bar_index = n + 1;
    t.entry_id = "entry-" + std::to_string(n) + std::string(40, 'a');
    t.entry_comment = std::string("entry\0comment", 13);
    t.exit_id = "exit-" + std::to_string(n);
    t.exit_comment = std::string(70, 'x') + std::to_string(n);
    t.exit_from_bracket = n % 2 != 0;
    t.max_runup = n + 0.5;
    t.max_drawdown = n + 0.25;
    t.commission = n * 0.125;
    t.entry_incarnation = 1000 + n;
    t.open_at_end = n % 5 == 0;
    t.close_cause = ex::CloseCause::Bracket;
    return t;
}

void same_trade(const Trade& a, const Trade& b) {
    CHECK(a.entry_time == b.entry_time);
    CHECK(a.exit_time == b.exit_time);
    CHECK(same_double(a.entry_price, b.entry_price));
    CHECK(same_double(a.exit_price, b.exit_price));
    CHECK(same_double(a.qty, b.qty));
    CHECK(same_double(a.pnl, b.pnl));
    CHECK(same_double(a.pnl_pct, b.pnl_pct));
    CHECK(a.is_long == b.is_long);
    CHECK(a.entry_bar_index == b.entry_bar_index);
    CHECK(a.exit_bar_index == b.exit_bar_index);
    CHECK(a.entry_id == b.entry_id);
    CHECK(a.entry_comment == b.entry_comment);
    CHECK(a.exit_id == b.exit_id);
    CHECK(a.exit_comment == b.exit_comment);
    CHECK(a.exit_from_bracket == b.exit_from_bracket);
    CHECK(same_double(a.max_runup, b.max_runup));
    CHECK(same_double(a.max_drawdown, b.max_drawdown));
    CHECK(same_double(a.commission, b.commission));
    CHECK(a.entry_incarnation == b.entry_incarnation);
    CHECK(a.open_at_end == b.open_at_end);
    CHECK(a.close_cause == b.close_cause);
}

template<class F>
void invariant(F action) {
    try {
        action();
        CHECK(false);
    } catch (const std::logic_error& error) {
        CHECK(dynamic_cast<const RunFailureInfo*>(&error) != nullptr);
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
    }
}

void matches(const TradeLedger& ledger, const std::vector<Trade>& expected,
             std::size_t base = 0) {
    CHECK(ledger.base() == base);
    REQUIRE(ledger.size() == expected.size());
    CHECK(ledger.empty() == expected.empty());
    std::size_t index = base;
    for (const Trade& t : ledger) {
        REQUIRE(index < expected.size());
        same_trade(t, expected[index]);
        same_trade(ledger[index], expected[index]);
        same_trade(ledger.at(index), expected[index]);
        ++index;
    }
    CHECK(index == expected.size());
    if (base < expected.size()) same_trade(ledger.back(), expected.back());
}

void retaining_tail() {
    TradeLedger ledger;
    std::vector<Trade> expected;
    CHECK(ledger.empty());
    CHECK(ledger.begin() == ledger.end());
    ledger.reserve(8);
    for (int n = 0; n < 513; ++n) {
        Trade t = row(n);
        expected.push_back(t);
        if (n % 2 == 0) ledger.push_back(t);
        else ledger.push_back(std::move(t));
    }
    matches(ledger, expected);
    ledger.at(64).exit_comment = expected.at(64).exit_comment = "amended";
    ledger[65].exit_time = expected[65].exit_time = T;
    std::size_t index = 0;
    for (Trade& t : ledger) t.exit_id = expected[index++].exit_id = "sorted-tail";
    ledger.resize(400);
    expected.resize(400);
    for (int n = 600; n < 630; ++n) {
        ledger.push_back(row(n));
        expected.push_back(row(n));
    }
    ledger.resize(435);
    expected.resize(435);
    matches(ledger, expected);
    same_trade(ledger.back(), expected.back());
    ledger.clear();
    CHECK(ledger.size() == 0);
    CHECK(ledger.base() == 0);
    CHECK(ledger.empty());
    ledger.push_back(row(7));
    matches(ledger, {row(7)});
}

void global_window() {
    TradeLedger ledger;
    std::vector<Trade> expected;
    for (int n = 0; n < 9; ++n) {
        ledger.push_back(row(n));
        expected.push_back(row(n));
    }
    ledger.seal(3);
    matches(ledger, expected, 3);
    ledger.seal(3);
    matches(ledger, expected, 3);
    const TradeLedger& read = ledger;
    invariant([&] { (void)ledger[2]; });
    invariant([&] { (void)read[2]; });
    invariant([&] { (void)ledger.at(2); });
    invariant([&] { (void)read.at(2); });
    invariant([&] { (void)ledger.at(9); });
    invariant([&] { (void)read.at(9); });
    invariant([&] { (void)ledger[9]; });
    invariant([&] { (void)read[9]; });
    invariant([&] { ledger.resize(2); });
    invariant([&] { ledger.seal(2); });
    invariant([&] { ledger.seal(10); });
    matches(ledger, expected, 3);
    ledger.resize(5);
    expected.resize(5);
    ledger.push_back(row(20));
    expected.push_back(row(20));
    ledger.resize(7);
    expected.resize(7);
    matches(ledger, expected, 3);
    ledger.seal(ledger.size());
    CHECK(ledger.size() == 7);
    CHECK(ledger.base() == 7);
    CHECK(!ledger.empty());
    CHECK(ledger.begin() == ledger.end());
    CHECK(read.begin() == read.end());
    invariant([&] { (void)ledger.back(); });
    invariant([&] { (void)read.back(); });
    ledger.reserve(4);
    ledger.push_back(row(21));
    expected.push_back(row(21));
    matches(ledger, expected, 7);
    ledger.clear();
    CHECK(ledger.base() == 0);
    CHECK(ledger.empty());
    invariant([&] { (void)ledger.at(0); });
    invariant([&] { (void)ledger.back(); });
    ledger.resize(1);
    matches(ledger, std::vector<Trade>(1));
}

void copies_and_moves() {
    TradeLedger original;
    std::vector<Trade> expected;
    for (int n = 0; n < 5; ++n) {
        original.push_back(row(n));
        expected.push_back(row(n));
    }
    original.seal(2);
    TradeLedger copy(original);
    original.at(2).entry_id = "original changed";
    matches(copy, expected, 2);
    TradeLedger assigned;
    assigned.push_back(row(99));
    assigned = copy;
    assigned = assigned;
    matches(assigned, expected, 2);
    copy[3].exit_comment = "copy changed";
    matches(assigned, expected, 2);
    TradeLedger moved(std::move(assigned));
    matches(moved, expected, 2);
    assigned.clear();
    assigned.push_back(row(50));
    matches(assigned, {row(50)});
    TradeLedger destination;
    destination.push_back(row(100));
    destination = std::move(moved);
    matches(destination, expected, 2);
    moved.clear();
    moved.reserve(4);
    moved.push_back(row(51));
    matches(moved, {row(51)});
    destination = std::move(destination);
    matches(destination, expected, 2);
}

struct Snapshot {
    std::vector<Trade> rows;
    std::uint64_t hash;
};

Snapshot native_run(LedgerProbe& host, std::uint64_t run_number) {
    host.beginning = [](Host& h) { h.calculations = 0; h.notified.clear(); };
    host.calculation = [](Host& h) {
        apply(h, put(h, h.calculations % 2 == 1 ? tx(1.0) : flat()));
    };
    auto config = spec("cr150-l8a");
    config.identity.run_number = run_number;
    REQUIRE(host.configure_native(config).status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    for (int n = 0; n < 600; ++n) {
        const double price = 100.0 + n;
        bars.push_back({price, price, price, price, 1.0, T + n * 60000LL});
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    completed(host);
    CHECK(host.ledger().base() == 0);
    REQUIRE(host.ledger().size() == 300);
    const auto hash = host.broker_state_hash();
    ReportC first{}, repeated{};
    host.fill_report(&first);
    host.fill_report(&repeated);
    REQUIRE(first.total_trades == 300 && first.trades_len == 300);
    REQUIRE(repeated.total_trades == 300 && repeated.trades_len == 300);
    CHECK(first.net_profit == 300.0);
    CHECK(repeated.net_profit == first.net_profit);
    CHECK(first.equity_curve_len == static_cast<std::int64_t>(bars.size()));
    CHECK(repeated.equity_curve_len == first.equity_curve_len);
    for (int n = 0; n < first.trades_len; ++n) {
        const Trade& t = host.ledger().at(static_cast<std::size_t>(n));
        CHECK(first.trades[n].entry_time == t.entry_time);
        CHECK(first.trades[n].exit_time == t.exit_time);
        CHECK(same_double(first.trades[n].entry_price, t.entry_price));
        CHECK(same_double(first.trades[n].exit_price, t.exit_price));
        CHECK(same_double(first.trades[n].qty, t.qty));
        CHECK(same_double(first.trades[n].pnl, t.pnl));
        CHECK(same_double(first.trades[n].commission, t.commission));
        CHECK(same_double(repeated.trades[n].pnl, first.trades[n].pnl));
        CHECK(!t.open_at_end);
        CHECK(t.entry_bar_index == 2 * n);
        CHECK(t.exit_bar_index == 2 * n + 1);
    }
    CHECK(host.broker_state_hash() == hash);
    BacktestEngine::free_report(&repeated);
    BacktestEngine::free_report(&first);
    return {{host.ledger().begin(), host.ledger().end()}, hash};
}

void native_retention_and_reuse() {
    LedgerProbe reused, fresh;
    const auto first = native_run(reused, 1);
    const auto second = native_run(reused, 2);
    const auto reference = native_run(fresh, 2);
    CHECK(first.rows.size() == second.rows.size());
    REQUIRE(second.rows.size() == reference.rows.size());
    for (std::size_t n = 0; n < second.rows.size(); ++n)
        same_trade(second.rows[n], reference.rows[n]);
    CHECK(second.hash == reference.hash);
}

} // namespace

int main() {
    test("retaining tail matches vector", retaining_tail);
    test("global resident window", global_window);
    test("copy and move ownership", copies_and_moves);
    test("sink-off native retention and reuse", native_retention_and_reuse);
    std::printf("test_trade_ledger: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
