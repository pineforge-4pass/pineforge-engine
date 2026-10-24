#include <pineforge/engine.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <cstdio>
#include <cstring>

namespace {

using pineforge::Trade;

int prefix_checks = 0;
int prefix_failures = 0;

void check_prefix(bool ok, const char* expression, int line) {
    ++prefix_checks;
    if (!ok) {
        ++prefix_failures;
        std::fprintf(stderr, "trade percent prefix: line %d: %s\n", line, expression);
    }
}

#define PREFIX_CHECK(expression) check_prefix((expression), #expression, __LINE__)

// The vector branch proves L8B without the container-swap lane. The other
// overload uses TradeLedger's frozen storage operation after integration.
template <class Rows>
auto retire_test_rows(Rows& rows, std::size_t count, int)
    -> decltype(rows.seal(rows.base() + count), void()) {
    rows.seal(rows.base() + count);
}

template <class Rows>
void retire_test_rows(Rows& rows, std::size_t count, long) {
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(count));
}

class PercentPrefixProbe final : public pineforge::BacktestEngine {
public:
    PercentPrefixProbe() : BacktestEngine(NativeConsumerBindTag{}) {}
    void on_bar(const pineforge::Bar&) override {}

    using BacktestEngine::avg_trade_percent;
    using BacktestEngine::avg_winning_trade_percent;
    using BacktestEngine::avg_losing_trade_percent;

    void append(const Trade& row) {
        trades_.push_back(row);
        if (row.pnl > 0.0) ++win_trades_count_;
        if (row.pnl < 0.0) ++loss_trades_count_;
    }

    void retire(std::size_t count) {
        std::size_t folded = 0;
        for (const auto& row : trades_) {
            if (folded == count) break;
            fold_trade_percent_prefix(row);
            ++folded;
        }
        PREFIX_CHECK(folded == count);
        retire_test_rows(trades_, count, 0);
    }

    std::size_t resident_count() const {
        std::size_t count = 0;
        for (const auto& row : trades_) { (void)row; ++count; }
        return count;
    }

    void replace_last(const Trade& row) {
        const Trade old = trades_.back();
        if (old.pnl > 0.0) --win_trades_count_;
        if (old.pnl < 0.0) --loss_trades_count_;
        // TradeLedger answers back()/at()/[] by value and mutates only through
        // modify()/replace(); an assignment to back() would write a temporary
        // and replace nothing.
        trades_.modify(trades_.size() - 1, [&](Trade& last) { last = row; });
        // A replacement that did nothing must fail here rather than leave the
        // prefix fold's replacement case unexercised.
        const Trade stored = trades_.back();
        PREFIX_CHECK(stored.pnl == row.pnl && stored.pnl_pct == row.pnl_pct);
        if (row.pnl > 0.0) ++win_trades_count_;
        if (row.pnl < 0.0) ++loss_trades_count_;
    }

    void truncate_last() {
        const auto& old = trades_.back();
        if (old.pnl > 0.0) --win_trades_count_;
        if (old.pnl < 0.0) --loss_trades_count_;
        trades_.resize(trades_.size() - 1);
    }

    void reset_prefix_and_rows() {
        reset_trade_percent_prefix();
        trades_.clear();
        win_trades_count_ = 0;
        loss_trades_count_ = 0;
    }

    void zero_win_loss_guards() {
        win_trades_count_ = 0;
        loss_trades_count_ = 0;
    }
};

Trade percent_row(double pnl, double pct) {
    Trade row{};
    row.pnl = pnl;
    row.pnl_pct = pct;
    return row;
}

// Frozen scalar loops from a6abb14e8, independent of the new prefix fold.
// Keep the signed losing percentage and the pnl-based filters verbatim.
double reference_percent(const std::vector<Trade>& rows, int filter) {
    if (filter == 0) {
        int n = static_cast<int>(rows.size());
        if (n <= 0) return 0.0;
        double s = 0.0;
        for (const auto& t : rows) s += t.pnl_pct;
        return s / static_cast<double>(n);
    }
    int guard = 0;
    for (const auto& t : rows) {
        if (filter > 0 ? t.pnl > 0.0 : t.pnl < 0.0) ++guard;
    }
    if (guard <= 0) return 0.0;
    double s = 0.0;
    int c = 0;
    for (const auto& t : rows) {
        if (filter > 0 ? t.pnl > 0.0 : t.pnl < 0.0) { s += t.pnl_pct; ++c; }
    }
    return (c > 0) ? (s / static_cast<double>(c)) : 0.0;
}

void compare_prefix(const PercentPrefixProbe& probe, const std::vector<Trade>& rows) {
    const double expected[] = {reference_percent(rows, 0), reference_percent(rows, 1),
                               reference_percent(rows, -1)};
    // Repeated reads must not consume or refold the sealed prefix.
    for (int read = 0; read != 2; ++read) {
        const double actual[] = {probe.avg_trade_percent(), probe.avg_winning_trade_percent(),
                                 probe.avg_losing_trade_percent()};
        for (int index = 0; index != 3; ++index)
            PREFIX_CHECK(std::memcmp(&actual[index], &expected[index], sizeof(double)) == 0);
    }
}

void every_prefix_split(const std::vector<Trade>& rows) {
    for (std::size_t split = 0; split <= rows.size(); ++split) {
        PercentPrefixProbe probe;
        for (const auto& row : rows) probe.append(row);
        compare_prefix(probe, rows);
        probe.retire(split);
        PREFIX_CHECK(probe.resident_count() == rows.size() - split);
        compare_prefix(probe, rows);
        const auto remainder = probe.resident_count();
        probe.retire(remainder);
        PREFIX_CHECK(probe.resident_count() == 0);
        compare_prefix(probe, rows);
    }
}

void test_literal_prefix_splits() {
    every_prefix_split({});
    every_prefix_split({percent_row(0.0, -0.0), percent_row(-0.0, 8.0)});
    every_prefix_split({percent_row(1.0, 1e16), percent_row(1.0, -1e16),
                        percent_row(1.0, 1.0)});
    every_prefix_split({percent_row(-1.0, -1e16), percent_row(-1.0, 1e16),
                        percent_row(-1.0, -1.0)});
    every_prefix_split({percent_row(2.0, -7.0), percent_row(-2.0, 11.0),
                        percent_row(0.0, 3.0), percent_row(5.0, 9.0),
                        percent_row(-3.0, -5.0)});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    every_prefix_split({percent_row(1.0, 2.0), percent_row(0.0, nan),
                        percent_row(-1.0, -3.0), percent_row(nan, 4.0)});
    every_prefix_split({percent_row(1.0, nan), percent_row(-1.0, nan),
                        percent_row(1.0, 5.0), percent_row(-1.0, -6.0)});
    every_prefix_split({percent_row(1.0, inf), percent_row(-1.0, -inf)});
}

void test_many_prefix_splits() {
    std::vector<Trade> rows;
    std::uint64_t bits = 0xb6a8e5c7113d92f1ULL;
    for (int index = 0; index != 256; ++index) {
        bits ^= bits << 13;
        bits ^= bits >> 7;
        bits ^= bits << 17;
        const double pnl = static_cast<double>(static_cast<int>(bits % 7) - 3);
        const int exponent = static_cast<int>((bits >> 8) % 201) - 100;
        const double pct = std::ldexp(static_cast<double>(static_cast<int>(bits >> 48) - 32768),
                                      exponent);
        rows.push_back(percent_row(pnl, pct));
    }
    every_prefix_split(rows);
}

void test_append_rewrite_truncate_and_reset() {
    PercentPrefixProbe probe;
    std::vector<Trade> rows;
    for (int index = 0; index != 96; ++index) {
        const auto row = percent_row(static_cast<double>(index % 3 - 1),
                                     static_cast<double>(index - 40) / 7.0);
        probe.append(row);
        rows.push_back(row);
        compare_prefix(probe, rows);
        if (probe.resident_count() > 2) probe.retire(probe.resident_count() - 2);
        compare_prefix(probe, rows);
    }
    rows.back() = percent_row(1.0, -17.0);
    probe.replace_last(rows.back());
    compare_prefix(probe, rows);
    rows.pop_back();
    probe.truncate_last();
    compare_prefix(probe, rows);
    probe.retire(probe.resident_count());
    compare_prefix(probe, rows);

    probe.zero_win_loss_guards();
    PREFIX_CHECK(probe.avg_winning_trade_percent() == 0.0);
    PREFIX_CHECK(probe.avg_losing_trade_percent() == 0.0);
    probe.reset_prefix_and_rows();
    compare_prefix(probe, {});
    rows = {percent_row(-1.0, -12.0), percent_row(1.0, 3.0)};
    for (const auto& row : rows) probe.append(row);
    compare_prefix(probe, rows);
    probe.retire(1);
    compare_prefix(probe, rows);
    probe.reset_prefix_and_rows();
    compare_prefix(probe, {});
}

#undef PREFIX_CHECK

}  // namespace

int main() {
    test_literal_prefix_splits();
    test_many_prefix_splits();
    test_append_rewrite_truncate_and_reset();
    std::printf("trade percent prefix: %d checks, %d failures\n", prefix_checks, prefix_failures);
    return prefix_failures == 0 ? 0 : 1;
}
