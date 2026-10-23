// CR-150 L8a: exercise the frozen ledger contract and its retaining native path.
#include <pineforge/trade_ledger.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/scratch_sequence.hpp>
#include "native_current_fixture.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
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
    expected.at(64).exit_comment = "amended";
    ledger.replace(64, expected.at(64));
    expected[65].exit_time = T;
    ledger.replace(65, expected[65]);
    std::size_t index = 0;
    for (auto& t : expected) {
        t.exit_id = "sorted-tail";
        ledger.replace(index++, t);
    }
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
    original.modify(2, [](Trade& t) { t.entry_id = "original changed"; });
    matches(copy, expected, 2);
    TradeLedger assigned;
    assigned.push_back(row(99));
    assigned = copy;
    assigned = assigned;
    matches(assigned, expected, 2);
    copy.modify(3, [](Trade& t) { t.exit_comment = "copy changed"; });
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

void external_values_and_backward_walk() {
    TradeLedger ledger;
    std::vector<Trade> expected;
    for (int n = 0; n < 1025; ++n) {
        expected.push_back(row(n));
        ledger.push_back(expected.back());
    }
    REQUIRE(ledger.size() == 1025);
    CHECK(ledger.resident_size() == TradeLedger::kResidentRows);
    REQUIRE(ledger.resident_base() > 64);
    const Trade retained = ledger.read(0);
    expected[0].pnl = -0.0;
    expected[0].entry_comment = std::string("new\0external", 12);
    ledger.replace(0, expected[0]);
    same_trade(retained, row(0));
    same_trade(ledger.read(0), expected[0]);
    ledger.visit(0, ledger.size(), [&](std::size_t i, const Trade& value) {
        // Nested reads cannot invalidate the visitor's owning row.
        const Trade newest = ledger.read(ledger.size() - 1);
        same_trade(value, expected[i]);
        same_trade(newest, expected.back());
        return true;
    });
    std::size_t cursor = ledger.size(), second = ledger.size();
    Trade value{}, interleaved{};
    while (ledger.previous(cursor, value)) {
        same_trade(value, expected[cursor]);
        REQUIRE(ledger.previous(second, interleaved));
        same_trade(value, interleaved);
    }
    CHECK(cursor == 0 && second == 0);
    CHECK(!ledger.previous(cursor, value));
    ledger.seal(900);
    same_trade(ledger.read(0), expected[0]);
    cursor = 901;
    REQUIRE(ledger.previous(cursor, value));
    same_trade(value, expected[900]);
    REQUIRE(ledger.previous(cursor, value)); // cross the sealed base
    same_trade(value, expected[899]);
    // A rewind crosses the physical resident window without crossing the
    // publication boundary. Replacements retain their global row numbers.
    ledger.resize(902);
    expected.resize(902);
    ledger.push_back(row(77));
    expected.push_back(row(77));
    matches(ledger, expected, 900);
    CHECK(ledger.resident_size() <= TradeLedger::kResidentRows);
}

struct ReaderProbe : Host {
    using BacktestEngine::avg_trade_percent;
    using BacktestEngine::avg_winning_trade_percent;
    using BacktestEngine::avg_losing_trade_percent;
    using BacktestEngine::closed_trade_profit;
    using BacktestEngine::closed_trade_profit_percent;
    using BacktestEngine::closed_trade_commission;
    using BacktestEngine::closed_trade_entry_bar_index;
    using BacktestEngine::closed_trade_exit_bar_index;
    using BacktestEngine::closed_trade_entry_comment;
    using BacktestEngine::closed_trade_exit_comment;
    using BacktestEngine::closed_trade_entry_id;
    using BacktestEngine::closed_trade_exit_id;
    using BacktestEngine::closed_trade_entry_incarnation;
    using BacktestEngine::closed_trade_entry_price;
    using BacktestEngine::closed_trade_exit_price;
    using BacktestEngine::closed_trade_entry_time;
    using BacktestEngine::closed_trade_exit_time;
    using BacktestEngine::closed_trade_size;
    using BacktestEngine::closed_trade_max_runup;
    using BacktestEngine::closed_trade_max_runup_percent;
    using BacktestEngine::closed_trade_max_drawdown;
    using BacktestEngine::closed_trade_max_drawdown_percent;
    using BacktestEngine::closed_trade_direction;
    using BacktestEngine::fill_trades_section;
    void seed(const Trade& value) {
        trades_.push_back(value);
        if (value.pnl > 0) ++win_trades_count_;
        else if (value.pnl < 0) ++loss_trades_count_;
    }
    const TradeLedger& ledger() const { return trades_; }
};

void migrated_historical_readers() {
    ReaderProbe host;
    std::vector<Trade> expected;
    double sum = 0.0, wins = 0.0, losses = 0.0;
    std::size_t winning = 0, losing = 0;
    for (int n = 0; n < 1025; ++n) {
        expected.push_back(row(n));
        host.seed(expected.back());
        const auto& value = expected.back();
        sum += value.pnl_pct;
        if (value.pnl > 0) { wins += value.pnl_pct; ++winning; }
        else if (value.pnl < 0) { losses += value.pnl_pct; ++losing; }
    }
    REQUIRE(host.closed_trade_count() == 1025);
    REQUIRE(host.ledger().resident_base() > 2);
    for (int i = 0; i < 3; ++i) {
        const auto& value = expected[static_cast<std::size_t>(i)];
        same_trade(host.get_trade(i), value);
        same_trade(host.closed_trade(static_cast<std::size_t>(i)), value);
        same_trade(host.get_report_trade(i), value);
        CHECK(same_double(host.closed_trade_profit(i), value.pnl));
        CHECK(same_double(host.closed_trade_profit_percent(i), value.pnl_pct));
        CHECK(same_double(host.closed_trade_commission(i), value.commission));
        CHECK(host.closed_trade_entry_bar_index(i) == value.entry_bar_index);
        CHECK(host.closed_trade_exit_bar_index(i) == value.exit_bar_index);
        CHECK(host.closed_trade_entry_comment(i) == value.entry_comment);
        CHECK(host.closed_trade_exit_comment(i) == value.exit_comment);
        CHECK(host.closed_trade_entry_id(i) == value.entry_id);
        CHECK(host.closed_trade_exit_id(i) == value.exit_id);
        CHECK(host.closed_trade_entry_incarnation(i) == value.entry_incarnation);
        CHECK(same_double(host.closed_trade_entry_price(i), value.entry_price));
        CHECK(same_double(host.closed_trade_exit_price(i), value.exit_price));
        CHECK(host.closed_trade_entry_time(i) == value.entry_time);
        CHECK(host.closed_trade_exit_time(i) == value.exit_time);
        CHECK(same_double(host.closed_trade_size(i), value.qty));
        CHECK(same_double(host.closed_trade_max_runup(i), value.max_runup));
        CHECK(same_double(host.closed_trade_max_drawdown(i), value.max_drawdown));
        const double cost = value.entry_price * value.qty;
        CHECK(same_double(host.closed_trade_max_runup_percent(i), value.max_runup / cost * 100.0));
        CHECK(same_double(host.closed_trade_max_drawdown_percent(i), value.max_drawdown / cost * 100.0));
        const auto cause = value.open_at_end ? ex::CloseCause::RangeEnd : value.close_cause;
        CHECK(host.closed_trade_close_cause(i) == static_cast<int>(cause));
        CHECK(host.closed_trade_direction(i) == (value.is_long ? "long" : "short"));
    }
    CHECK(same_double(host.avg_trade_percent(), sum / static_cast<double>(expected.size())));
    CHECK(same_double(host.avg_winning_trade_percent(), wins / static_cast<double>(winning)));
    CHECK(same_double(host.avg_losing_trade_percent(), losses / static_cast<double>(losing)));
    std::size_t cursor = host.closed_trade_count();
    Trade value{};
    while (host.previous_closed_trade(cursor, value)) same_trade(value, expected[cursor]);
    ReportC report{};
    host.fill_trades_section(&report);
    REQUIRE(report.trades_len == 1025);
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& original = expected[static_cast<std::size_t>(i)];
        CHECK(same_double(report.trades[i].pnl, original.pnl));
        CHECK(same_double(report.trades[i].commission, original.commission));
        CHECK(report.trades[i].entry_time == original.entry_time);
        CHECK(report.trades[i].exit_time == original.exit_time);
    }
    BacktestEngine::free_report(&report);
    CHECK(host.ledger().resident_size() <= TradeLedger::kResidentRows);
}

// D-10: the test-only merge owns its sequence and read buffers on the heap.
template<class T, class Compare>
void stable_sort_scratch_sequence(detail::ScratchSequence<T>& sequence, Compare before) {
    const auto count = sequence.size();
    if (count < 2) return;
    class PageReader {
    public:
        explicit PageReader(const detail::ScratchSequence<T>& source)
            : source_(source),
              values_(std::make_unique<T[]>(detail::ScratchSequence<T>::resident_capacity())) {}
        T at(std::size_t index) {
            if (index < first_ || index - first_ >= count_) {
                const auto capacity = detail::ScratchSequence<T>::resident_capacity();
                const auto first = index - index % capacity;
                const auto count = std::min(capacity, source_.size() - first);
                for (std::size_t i = 0; i < count; ++i)
                    values_[i] = source_[first + i];
                first_ = first;
                count_ = count;
            }
            return values_[index - first_];
        }
    private:
        const detail::ScratchSequence<T>& source_;
        std::unique_ptr<T[]> values_;
        std::size_t first_ = 0, count_ = 0;
    };
    auto scratch = std::make_unique<detail::ScratchSequence<T>>();
    auto* source = &sequence;
    auto* destination = scratch.get();
    for (std::size_t width = 1; width < count;) {
        destination->clear();
        PageReader left_page(*source), right_page(*source);
        for (std::size_t start = 0; start < count;) {
            const auto middle = start + std::min(width, count - start);
            const auto stop = middle + std::min(width, count - middle);
            auto left = start, right = middle;
            while (left < middle || right < stop) {
                if (right == stop || (left < middle
                    && !before(right_page.at(right), left_page.at(left))))
                    destination->push_back(left_page.at(left++));
                else
                    destination->push_back(right_page.at(right++));
            }
            start = stop;
        }
        // Swap pointers, avoiding a full sequence temporary on the stack.
        std::swap(source, destination);
        if (width > count / 2) break;
        width *= 2;
    }
    if (source != &sequence) sequence = std::move(*source);
}

void phase_pages_and_stable_permutation() {
    detail::ScratchSequence<std::uint32_t> phases;
    constexpr std::size_t population = 100003;
    for (std::size_t i = 0; i < population; ++i)
        phases.push_back(static_cast<std::uint32_t>(i % 5));
    REQUIRE(phases.size() == population);
    CHECK(phases.resident_capacity() * sizeof(std::uint32_t) == 64 * 1024);
    phases.set(0, 4); // force an old-page amendment
    CHECK(phases[population - 1] == (population - 1) % 5);
    CHECK(phases[0] == 4);
    phases.erase(phases.begin(), phases.begin() + 70000);
    CHECK(phases.size() == population - 70000);
    CHECK(phases[0] == 70000 % 5);
    phases.resize(3);
    phases.push_back(17);
    CHECK(phases[3] == 17);
    phases.clear();
    phases.push_back(23);
    CHECK(phases.size() == 1 && phases[0] == 23);

    struct Ordered { std::uint32_t key, ordinal; };
    detail::ScratchSequence<Ordered> indices;
    std::vector<Ordered> oracle;
    for (std::size_t i = 0; i < population; ++i) {
        const Ordered value{static_cast<std::uint32_t>((population - i) % 17),
                            static_cast<std::uint32_t>(i)};
        indices.push_back(value);
        oracle.push_back(value);
    }
    const auto before = [](Ordered a, Ordered b) { return a.key < b.key; };
    std::stable_sort(oracle.begin(), oracle.end(), before);
    stable_sort_scratch_sequence(indices, before);
    for (std::size_t i = 0; i < population; ++i) {
        const auto actual = indices[i];
        CHECK(actual.key == oracle[i].key && actual.ordinal == oracle[i].ordinal);
    }
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
    config.report_policy = NativeReportPolicy::KernelRecorded;
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
    test("external values and backwards global rows", external_values_and_backward_walk);
    test("migrated readers on external-only rows", migrated_historical_readers);
    test("phase pages and stable permutation", phase_pages_and_stable_permutation);
    test("sink-off native retention and reuse", native_retention_and_reuse);
    std::printf("test_trade_ledger: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
