// B2-15 RED: numeric-only report passes must not decode the four long ledger
// strings. The allocation delta is the decode counter: a full row read is the
// independent control and must allocate all four payload strings.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<Bar> tape() {
    std::vector<Bar> result;
    for (int index = 0; index < 3; ++index) {
        const double price = 100.0 + index * 0.25;
        result.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                          1.0, INT64_C(1704067200000) + index * INT64_C(60000)});
    }
    return result;
}

class NumericHost final : public NativeStrategyHost {
public:
    NumericHost(std::uint64_t run_number, std::string text)
        : text_(std::move(text)) {
        NativeRunSpec spec;
        spec.identity = {"p2-numeric-read", run_number};
        spec.input_tf = spec.script_tf = "1";
        spec.ticker = "P2";
        spec.tickerid = "TEST:P2";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == NativeSetupStatus::Applied,
                "numeric read fixture configure refused");
    }

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        const int index = context.coordinate.interval_index;
        if (index == 0) {
            const auto result = submit({native_order::Transact{1.0},
                                        text_ + "-entry", text_ + "-entry-comment"});
            require(result.status == native_order::SubmitStatus::Accepted
                        && result.handle.has_value(),
                    "numeric read entry refused");
        } else if (index == 1) {
            const auto result = submit({native_order::Flatten{},
                                        text_ + "-exit", text_ + "-exit-comment"});
            require(result.status == native_order::SubmitStatus::Accepted
                        && result.handle.has_value(),
                    "numeric read exit refused");
        }
    }

    const TradeLedger& ledger() const { return trades_; }

private:
    std::string text_;
};

template<class Action>
std::size_t allocation_count(Action action) {
    const auto before = global_allocation::allocations;
    global_allocation::recording = true;
    action();
    global_allocation::recording = false;
    return global_allocation::allocations - before;
}

template<class Left, class Right>
void equal_field(const Left& left, const Right& right, int index,
                 const char* field, const char* context) {
    bool equal;
    if constexpr (std::is_same_v<Left, double> && std::is_same_v<Right, double>)
        equal = std::memcmp(&left, &right, sizeof(double)) == 0;
    else
        equal = left == right;
    if (!equal)
        throw std::runtime_error(std::string(context) + ": row " + std::to_string(index)
                                 + " field " + field + " differs");
}

// TradeC has all fourteen report numbers and no strings. Compare every field,
// preserving double bits, without comparing its unspecified padding bytes.
// The same list checks a report row against the independent full ledger read.
template<class Left, class Right>
void equal_report_numbers(const Left& left, const Right& right, int index,
                          const char* context) {
    equal_field(left.entry_time, right.entry_time, index, "entry_time", context);
    equal_field(left.exit_time, right.exit_time, index, "exit_time", context);
    equal_field(left.entry_price, right.entry_price, index, "entry_price", context);
    equal_field(left.exit_price, right.exit_price, index, "exit_price", context);
    equal_field(left.pnl, right.pnl, index, "pnl", context);
    equal_field(left.pnl_pct, right.pnl_pct, index, "pnl_pct", context);
    equal_field(left.is_long, right.is_long, index, "is_long", context);
    equal_field(left.max_runup, right.max_runup, index, "max_runup", context);
    equal_field(left.max_drawdown, right.max_drawdown, index, "max_drawdown", context);
    equal_field(left.qty, right.qty, index, "qty", context);
    equal_field(left.commission, right.commission, index, "commission", context);
    equal_field(left.entry_bar_index, right.entry_bar_index, index, "entry_bar_index", context);
    equal_field(left.exit_bar_index, right.exit_bar_index, index, "exit_bar_index", context);
    equal_field(left.open_at_end, right.open_at_end, index, "open_at_end", context);
}

void equal_ledger_numbers(const Trade& full, const Trade& numeric, int index) {
    const char* context = "full/numeric ledger read";
    equal_report_numbers(full, numeric, index, context);
    // These three numeric/flag fields belong to Trade but not TradeC.
    equal_field(full.exit_from_bracket, numeric.exit_from_bracket, index,
                "exit_from_bracket", context);
    equal_field(full.entry_incarnation, numeric.entry_incarnation, index,
                "entry_incarnation", context);
    equal_field(full.close_cause, numeric.close_cause, index, "close_cause", context);
    require(numeric.entry_id.empty() && numeric.entry_comment.empty()
                && numeric.exit_comment.empty() && numeric.exit_id.empty(),
            "numeric ledger read materialized a string");
}

void equal_reports(const ReportC& left, const ReportC& right) {
    require(left.total_trades == right.total_trades
                && left.trades_len == right.trades_len,
            "numeric-only report changed trade counts");
    require(std::memcmp(&left.metrics, &right.metrics, sizeof left.metrics) == 0,
            "numeric-only report changed metrics");
    require(left.equity_curve_len == right.equity_curve_len,
            "numeric-only report changed equity length");
    for (int index = 0; index < left.trades_len; ++index)
        equal_report_numbers(left.trades[index], right.trades[index], index,
                             "short/long-string report");
    for (std::int64_t index = 0; index < left.equity_curve_len; ++index)
        require(std::memcmp(&left.equity_curve[index], &right.equity_curve[index],
                            sizeof(pf_equity_point_t)) == 0,
                "numeric-only report changed an equity point");
}

} // namespace

int main() {
    try {
        const std::string long_text(512, 'x');
        NumericHost empty(1, ""), long_strings(2, long_text);
        const auto bars = tape();
        empty.run(bars.data(), static_cast<int>(bars.size()));
        long_strings.run(bars.data(), static_cast<int>(bars.size()));
        require(empty.last_error().empty() && long_strings.last_error().empty(),
                "numeric read fixture run failed");

        ReportC empty_report{}, long_report{};
        const auto empty_allocations = allocation_count([&] { empty.fill_report(&empty_report); });
        const auto long_allocations = allocation_count([&] { long_strings.fill_report(&long_report); });
        require(empty_report.total_trades == 1 && empty_report.trades_len == 1
                    && long_report.total_trades == 1 && long_report.trades_len == 1,
                "numeric read fixture produced no real trade row");
        equal_reports(empty_report, long_report);
        for (int index = 0; index < empty_report.trades_len; ++index) {
            Trade short_full{}, long_full{}, short_numeric{}, long_numeric{};
            const auto short_full_allocations = allocation_count([&] {
                short_full = empty.ledger()[static_cast<std::size_t>(index)];
            });
            const auto long_full_allocations = allocation_count([&] {
                long_full = long_strings.ledger()[static_cast<std::size_t>(index)];
            });
            require(long_full.entry_id == long_text + "-entry"
                        && long_full.entry_comment == long_text + "-entry-comment"
                        && long_full.exit_id == long_text + "-exit"
                        && long_full.exit_comment == long_text + "-exit-comment",
                    "full-read control did not retain the four input strings");
            require(long_full_allocations >= short_full_allocations + 4,
                    "full ledger read did not expose the four string decodes");
            const auto short_numeric_allocations = allocation_count([&] {
                short_numeric = empty.ledger().numeric_at(static_cast<std::size_t>(index));
            });
            const auto long_numeric_allocations = allocation_count([&] {
                long_numeric = long_strings.ledger().numeric_at(static_cast<std::size_t>(index));
            });
            equal_ledger_numbers(short_full, short_numeric, index);
            equal_ledger_numbers(long_full, long_numeric, index);
            equal_report_numbers(empty_report.trades[index], short_full, index,
                                 "short-string report/full ledger read");
            equal_report_numbers(long_report.trades[index], long_full, index,
                                 "long-string report/full ledger read");
            require(long_numeric_allocations == short_numeric_allocations,
                    "numeric ledger read allocated for strings");
        }
        // The real full-read control above must expose at least four decodes;
        // the numeric-only fill_report pass must expose zero extra allocations.
        if (long_allocations != empty_allocations)
            throw std::runtime_error("fill_report string decode counter is nonzero (short="
                + std::to_string(empty_allocations) + ", long="
                + std::to_string(long_allocations) + " allocations)");

        BacktestEngine::free_report(&empty_report);
        BacktestEngine::free_report(&long_report);
        std::puts("B2-15 numeric report pass: equal fields; string decode allocation delta 0");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "B2-15: %s\n", error.what());
        return 1;
    }
}
