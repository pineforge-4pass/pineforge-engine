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

private:
    std::string text_;
};

std::size_t fill_count(NumericHost& host, ReportC& report) {
    const auto before = global_allocation::allocations;
    global_allocation::recording = true;
    host.fill_report(&report);
    global_allocation::recording = false;
    return global_allocation::allocations - before;
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
        require(std::memcmp(&left.trades[index], &right.trades[index],
                            sizeof(TradeC)) == 0,
                "numeric-only report changed a trade row");
    for (std::int64_t index = 0; index < left.equity_curve_len; ++index)
        require(std::memcmp(&left.equity_curve[index], &right.equity_curve[index],
                            sizeof(pf_equity_point_t)) == 0,
                "numeric-only report changed an equity point");
}

} // namespace

int main() {
    try {
        NumericHost empty(1, ""), long_strings(2, std::string(512, 'x'));
        const auto bars = tape();
        empty.run(bars.data(), static_cast<int>(bars.size()));
        long_strings.run(bars.data(), static_cast<int>(bars.size()));
        require(empty.last_error().empty() && long_strings.last_error().empty(),
                "numeric read fixture run failed");

        ReportC empty_report{}, long_report{};
        const auto empty_allocations = fill_count(empty, empty_report);
        const auto long_allocations = fill_count(long_strings, long_report);
        require(empty_report.total_trades == 1 && empty_report.trades_len == 1
                    && long_report.total_trades == 1 && long_report.trades_len == 1,
                "numeric read fixture produced no real trade row");
        equal_reports(empty_report, long_report);
        require(long_allocations >= empty_allocations + 4,
                "full report did not expose the four string decodes");

        BacktestEngine::free_report(&empty_report);
        BacktestEngine::free_report(&long_report);
        std::puts("B2-15 numeric report pass: equal values and four-string decode counter");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "B2-15: %s\n", error.what());
        return 1;
    }
}
