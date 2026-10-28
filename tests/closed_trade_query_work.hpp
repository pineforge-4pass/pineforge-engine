#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Defined before engine.hpp: the build-directory header overlay inserts only
// calls to this forwarding observer at the original percentage loop bodies.
namespace closed_trade_query_work {

struct Work {
    std::array<std::uint64_t, 3> percent_rows{};
    std::uint64_t percent_control_rows = 0;
    std::uint64_t percent_fold_rows = 0;
    std::uint64_t net_rows = 0;
    std::uint64_t metric_rows = 0;
    std::uint64_t other_numeric_rows = 0;
    std::uint64_t accumulator_inputs = 0;
};

inline Work* counting = nullptr;

template<class Ledger, class = void>
struct has_percent_fold_rows : std::false_type {};

template<class Ledger>
struct has_percent_fold_rows<Ledger, std::void_t<
    decltype(std::declval<const Ledger&>().percent_fold_rows_consumed())>>
    : std::true_type {};

template<class Ledger>
std::uint64_t observed_percent_fold_rows(const Ledger& ledger) noexcept {
    if constexpr (has_percent_fold_rows<Ledger>::value)
        return ledger.percent_fold_rows_consumed();
    // The base tree exposes its work only through the accessor-loop overlay.
    return 0;
}

class CountScope {
public:
    explicit CountScope(Work& work) noexcept
        : work_(&work), previous_(counting) {
        counting = &work;
    }
    template<class Ledger>
    CountScope(Work& work, const Ledger& ledger) noexcept
        : work_(&work), previous_(counting), ledger_(&ledger),
          read_rows_(&read_percent_fold_rows<Ledger>),
          before_rows_(observed_percent_fold_rows(ledger)) {
        counting = &work;
    }
    ~CountScope() noexcept {
        if (read_rows_)
            work_->percent_fold_rows += read_rows_(ledger_) - before_rows_;
        counting = previous_;
    }
    CountScope(const CountScope&) = delete;
    CountScope& operator=(const CountScope&) = delete;
private:
    using ReadRows = std::uint64_t (*)(const void*) noexcept;

    template<class Ledger>
    static std::uint64_t read_percent_fold_rows(const void* value) noexcept {
        return observed_percent_fold_rows(*static_cast<const Ledger*>(value));
    }

    Work* work_;
    Work* previous_;
    const void* ledger_ = nullptr;
    ReadRows read_rows_ = nullptr;
    std::uint64_t before_rows_ = 0;
};

template<class Row>
const Row& percent_input(const char* accessor, const Row& row) {
    if (counting) {
        if (std::strcmp(accessor, "avg_trade_percent") == 0)
            ++counting->percent_rows[0];
        else if (std::strcmp(accessor, "avg_winning_trade_percent") == 0)
            ++counting->percent_rows[1];
        else if (std::strcmp(accessor, "avg_losing_trade_percent") == 0)
            ++counting->percent_rows[2];
        else if (std::strcmp(accessor, "percent_overlay_control") == 0)
            ++counting->percent_control_rows;
        else
            throw std::runtime_error("setup failure: unknown percentage loop observer");
    }
    return row;
}

inline std::size_t numeric_index(const char* reader, std::size_t index) noexcept {
    if (counting) {
        if (std::strcmp(reader, "fill_trades_section") == 0
            || std::strcmp(reader, "report_closed_net_profit") == 0)
            ++counting->net_rows;
        else if (std::strcmp(reader, "compute_report_sink_trade_metrics") == 0)
            ++counting->metric_rows;
        else
            ++counting->other_numeric_rows;
    }
    return index;
}

inline void accumulator_input() noexcept {
    if (counting) ++counting->accumulator_inputs;
}

inline std::uint64_t overlay_percent_visits(const Work& work) {
    return work.percent_rows[0] + work.percent_rows[1] + work.percent_rows[2];
}

inline std::uint64_t percent_visits(const Work& work) {
    return overlay_percent_visits(work) + work.percent_fold_rows;
}

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline std::uint64_t bits(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t), "binary64 required");
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

inline void equal_double(double actual, double expected, const char* field) {
    if (bits(actual) != bits(expected))
        throw std::runtime_error(std::string("value mismatch: ") + field);
}

} // namespace closed_trade_query_work

#include <pineforge/native_host.hpp>

namespace closed_trade_query_work {

using namespace pineforge;

// Fixture sizes and predicted visit counts are layout estimates from source,
// not measured here. Execution and red/green verdicts belong to the lab.
inline constexpr int small_trades = 48;
inline constexpr int large_trades = 4 * small_trades;
inline constexpr std::size_t query_count = 5;
inline constexpr double initial_capital = 1000000.0;
inline constexpr std::int64_t start_ms = INT64_C(1704067200000);
inline constexpr std::int64_t minute_ms = INT64_C(60000);

inline bool long_side(int trade) { return trade % 2 == 0; }
inline double quantity(int trade) { return 1.0 + (trade % 4) * 0.25; }
inline double entry_price(int trade) { return 64.0 + (trade % 11) * 0.25; }
inline double favorable_move(int trade) {
    if (trade % 3 == 0) return 0.75 + (trade % 5) * 0.125;
    if (trade % 3 == 1) return -(0.5 + (trade % 7) * 0.125);
    return 0.0;
}
inline double exit_price(int trade) {
    return entry_price(trade) + (long_side(trade) ? 1.0 : -1.0) * favorable_move(trade);
}
inline double expected_pnl(int trade) {
    // Hand arithmetic: a unit point value and account FX, no fees/slippage.
    // The signed price move times the dyadic quantity is exactly representable.
    return favorable_move(trade) * quantity(trade);
}

inline std::vector<Bar> tape(int trades) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(4 * trades));
    for (int index = 0; index < 4 * trades; ++index) {
        const int trade = index / 4;
        // Signal entry at phase 0, fill it at phase 1; signal the close at
        // phase 1, fill it at phase 2. Phase 3 is an idle evaluation.
        const double price = index % 4 < 2 ? entry_price(trade) : exit_price(trade);
        bars.push_back({price, price, price, price, 1000.0,
                        start_ms + index * minute_ms});
    }
    return bars;
}

inline void check_closed_row(const Trade& row, int index) {
    equal_double(row.entry_price, entry_price(index), "closed row entry price");
    equal_double(row.exit_price, exit_price(index), "closed row exit price");
    equal_double(row.qty, quantity(index), "closed row quantity");
    equal_double(row.pnl, expected_pnl(index), "closed row hand-arithmetic PnL");
    equal_double(row.commission, 0.0, "closed row commission");
    const double cost = entry_price(index) * quantity(index);
    equal_double(row.pnl_pct, (expected_pnl(index) / cost) * 100.0,
                 "closed row hand-arithmetic return on cost");
    require(row.is_long == long_side(index), "value mismatch: closed row direction");
    require(!row.open_at_end, "value mismatch: terminal row substituted for a close");
    require(row.entry_bar_index == 4 * index + 1
                && row.exit_bar_index == 4 * index + 2,
            "value mismatch: closed row fill coordinates");
}

inline TradeC public_report_row(const Trade& row) {
    TradeC result{};
    result.entry_time = row.entry_time;
    result.exit_time = row.exit_time;
    result.entry_price = row.entry_price;
    result.exit_price = row.exit_price;
    result.pnl = row.pnl;
    result.pnl_pct = row.pnl_pct;
    result.is_long = row.is_long ? 1 : 0;
    result.max_runup = row.max_runup;
    result.max_drawdown = row.max_drawdown;
    result.qty = row.qty;
    result.commission = row.commission;
    result.entry_bar_index = row.entry_bar_index;
    result.exit_bar_index = row.exit_bar_index;
    result.open_at_end = row.open_at_end ? 1 : 0;
    return result;
}

inline std::vector<TradeC> public_rows(const BacktestEngine& host, int expected_count) {
    require(host.closed_trade_count() == static_cast<std::size_t>(expected_count),
            "setup failure: real run did not produce the requested closed population");
    std::vector<TradeC> rows;
    rows.reserve(static_cast<std::size_t>(expected_count));
    for (int index = 0; index < expected_count; ++index) {
        const auto row = host.closed_trade(static_cast<std::size_t>(index));
        check_closed_row(row, index);
        rows.push_back(public_report_row(row));
    }
    return rows;
}

struct PercentOracle {
    std::array<double, 3> sums{};
    std::array<std::size_t, 3> counts{};
    std::size_t evens = 0;

    void add(const Trade& row) {
        // Independent ordered scalar left fold, including signed losing
        // percentages. Report loss percentages use a different convention.
        sums[0] += row.pnl_pct;
        ++counts[0];
        if (row.pnl > 0.0) {
            sums[1] += row.pnl_pct;
            ++counts[1];
        } else if (row.pnl < 0.0) {
            sums[2] += row.pnl_pct;
            ++counts[2];
        } else {
            ++evens;
        }
    }

    std::array<double, 3> values() const {
        std::array<double, 3> result{};
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = counts[index]
                ? sums[index] / static_cast<double>(counts[index]) : 0.0;
        return result;
    }
};

inline PercentOracle percent_oracle(const BacktestEngine& host, int count) {
    require(host.closed_trade_count() == static_cast<std::size_t>(count),
            "setup failure: quiet evaluation has the wrong closed population");
    PercentOracle result;
    for (int index = 0; index < count; ++index) {
        const auto row = host.closed_trade(static_cast<std::size_t>(index));
        check_closed_row(row, index);
        result.add(row);
    }
    require(result.counts[1] == static_cast<std::size_t>((count + 2) / 3)
                && result.counts[2] == static_cast<std::size_t>((count + 1) / 3)
                && result.evens == static_cast<std::size_t>(count / 3),
            "value mismatch: winning/losing/even population");
    return result;
}

inline PercentOracle percent_oracle(const BacktestEngine& host) {
    // Amendments, rewinds and copies retain the public row order but need
    // not retain the original tape's percentages or population.
    PercentOracle result;
    for (std::size_t index = 0; index < host.closed_trade_count(); ++index)
        result.add(host.closed_trade(index));
    return result;
}

inline void equal_percent(const std::array<double, 3>& actual,
                          const PercentOracle& expected) {
    const auto values = expected.values();
    equal_double(actual[0], values[0], "avg_trade_percent");
    equal_double(actual[1], values[1], "avg_winning_trade_percent");
    equal_double(actual[2], values[2], "avg_losing_trade_percent");
}

inline void equal_stats(const pf_trade_stats_t& actual, const pf_trade_stats_t& expected) {
#define CHECK_STAT_INTEGER(field) \
    require(actual.field == expected.field, "value mismatch: " #field)
    CHECK_STAT_INTEGER(num_trades);
    CHECK_STAT_INTEGER(num_wins);
    CHECK_STAT_INTEGER(num_losses);
    CHECK_STAT_INTEGER(num_even);
    CHECK_STAT_INTEGER(max_consecutive_wins);
    CHECK_STAT_INTEGER(max_consecutive_losses);
#undef CHECK_STAT_INTEGER
#define CHECK_STAT_DOUBLE(field) equal_double(actual.field, expected.field, #field)
    CHECK_STAT_DOUBLE(percent_profitable);
    CHECK_STAT_DOUBLE(net_profit);
    CHECK_STAT_DOUBLE(net_profit_pct);
    CHECK_STAT_DOUBLE(gross_profit);
    CHECK_STAT_DOUBLE(gross_profit_pct);
    CHECK_STAT_DOUBLE(gross_loss);
    CHECK_STAT_DOUBLE(gross_loss_pct);
    CHECK_STAT_DOUBLE(profit_factor);
    CHECK_STAT_DOUBLE(avg_trade);
    CHECK_STAT_DOUBLE(avg_trade_pct);
    CHECK_STAT_DOUBLE(avg_win);
    CHECK_STAT_DOUBLE(avg_win_pct);
    CHECK_STAT_DOUBLE(avg_loss);
    CHECK_STAT_DOUBLE(avg_loss_pct);
    CHECK_STAT_DOUBLE(ratio_avg_win_avg_loss);
    CHECK_STAT_DOUBLE(largest_win);
    CHECK_STAT_DOUBLE(largest_win_pct);
    CHECK_STAT_DOUBLE(largest_loss);
    CHECK_STAT_DOUBLE(largest_loss_pct);
    CHECK_STAT_DOUBLE(commission_paid);
    CHECK_STAT_DOUBLE(expectancy);
    CHECK_STAT_DOUBLE(avg_bars_in_trade);
    CHECK_STAT_DOUBLE(avg_bars_in_wins);
    CHECK_STAT_DOUBLE(avg_bars_in_losses);
#undef CHECK_STAT_DOUBLE
}

inline void check_completed(const NativeStrategyHost& host) {
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "setup failure: real command run failed");
    const auto state = host.native_state();
    require(state.kind == NativeLifecycleKind::Completed
                && state.failure.code == NativeFailureCode::None,
            "setup failure: native run did not complete without an error code");
    require(host.physical_position().signed_units == 0.0,
            "setup failure: fixture left an open position");
}

} // namespace closed_trade_query_work
