// F3: the legacy SUMMARY_ONLY protocol exposes pending ratios explicitly.
// The host must run EquityMetricsSecondPass and install its result before
// presenting the four ratio fields; the engine does not run that pass.
#include <pineforge/metrics_fold.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);  \
        }                                                                       \
    } while (0)

using pineforge::Bar;
using pineforge::ReportC;
using pineforge::ReportMode;

class DefaultRecorderHost final : public pineforge::NativeStrategyHost {
public:
    int callbacks = 0;

    void on_native_bar(const Bar&, const pineforge::NativeDecisionContext&) override {
        if (callbacks++ == 0) {
            const pineforge::native_order::Request buy{
                pineforge::native_order::Transact{1.0}, "F3-long", ""};
            CHECK(submit(buy).status == pineforge::native_order::SubmitStatus::Accepted);
        }
    }
};

struct Report {
    ReportC value{};
    ~Report() { pineforge::BacktestEngine::free_report(&value); }
};

void configure(DefaultRecorderHost& host) {
    pineforge::NativeRunSpec spec;
    spec.identity = {"F3-summary-ratios", 1};
    spec.input_tf = spec.script_tf = "1D";
    spec.tickerid = "TEST:F3";
    spec.timezone = spec.chart_timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    CHECK(host.configure_native(spec).status == pineforge::NativeSetupStatus::Applied);
}

std::array<Bar, 12> four_month_tape() {
    // Three observations in each UTC month, January through April 2024.
    // A one-unit long fills at the second bar's open, 104. Month-end equity
    // is therefore 10006, 9992, 10008, 9997: gains and losses with variance.
    constexpr std::int64_t jan_1 = 1704067200000LL;
    constexpr std::int64_t day = 86400000LL;
    constexpr std::array<int, 12> days{{0, 14, 30, 31, 45, 59,
                                        60, 74, 90, 91, 105, 120}};
    constexpr std::array<double, 12> prices{{100, 104, 110, 108, 98, 96,
                                             102, 106, 112, 109, 103, 101}};
    std::array<Bar, 12> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double price = prices[i];
        bars[i] = {price, price + 1.0, price - 1.0, price, 1.0,
                   jan_1 + static_cast<std::int64_t>(days[i]) * day};
    }
    return bars;
}

bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

void require_finite_ratios(const pf_equity_stats_t& equity) {
    CHECK(std::isfinite(equity.sharpe_monthly));
    CHECK(std::isfinite(equity.sortino_monthly));
    CHECK(std::isfinite(equity.sharpe_bar));
    CHECK(std::isfinite(equity.sortino_bar));
}

void require_pending_ratios(const pf_equity_stats_t& equity) {
    CHECK(std::isnan(equity.sharpe_monthly));
    CHECK(std::isnan(equity.sortino_monthly));
    CHECK(std::isnan(equity.sharpe_bar));
    CHECK(std::isnan(equity.sortino_bar));
}

void require_same_non_ratio_metrics(const pf_metrics_t& actual,
                                    const pf_metrics_t& full) {
    CHECK(std::memcmp(&actual.all, &full.all, sizeof(pf_trade_stats_t)) == 0);
    CHECK(std::memcmp(&actual.longs, &full.longs, sizeof(pf_trade_stats_t)) == 0);
    CHECK(std::memcmp(&actual.shorts, &full.shorts, sizeof(pf_trade_stats_t)) == 0);
#define SAME(field) CHECK(same_bits(actual.equity.field, full.equity.field))
    SAME(max_equity_drawdown);
    SAME(max_equity_drawdown_pct);
    SAME(max_equity_runup);
    SAME(max_equity_runup_pct);
    SAME(buy_hold_return);
    SAME(buy_hold_return_pct);
    SAME(cagr);
    SAME(calmar);
    SAME(recovery_factor);
    SAME(time_in_market_pct);
    SAME(open_pl);
#undef SAME
}

void require_same_non_ratio_report(const ReportC& actual, const ReportC& full) {
#define SAME(field) CHECK(actual.field == full.field)
    SAME(total_trades);
    SAME(trades_len);
    CHECK(same_bits(actual.net_profit, full.net_profit));
    SAME(input_bars_processed);
    SAME(script_bars_processed);
    SAME(security_feeds_total);
    SAME(security_eval_complete_total);
    SAME(security_eval_partial_total);
    SAME(magnifier_sub_bars_total);
    SAME(magnifier_sample_ticks_total);
    SAME(input_tf_seconds);
    SAME(script_tf_seconds);
    SAME(script_tf_ratio);
    SAME(needs_aggregation);
    SAME(bar_magnifier_enabled);
#undef SAME
    CHECK(actual.security_diag == nullptr && actual.security_diag_len == 0);
    CHECK(full.security_diag == nullptr && full.security_diag_len == 0);
    CHECK(actual.trace == nullptr && actual.trace_len == 0);
    CHECK(full.trace == nullptr && full.trace_len == 0);
    CHECK(actual.trace_names == nullptr && actual.trace_names_len == 0);
    CHECK(full.trace_names == nullptr && full.trace_names_len == 0);
    CHECK(actual.broker_state_hash == nullptr && actual.broker_state_hash_len == 0);
    CHECK(full.broker_state_hash == nullptr && full.broker_state_hash_len == 0);
    require_same_non_ratio_metrics(actual.metrics, full.metrics);
    // SUMMARY_ONLY omits the curve while retaining its first-pass metrics.
    CHECK(actual.equity_curve == nullptr && actual.equity_curve_len == 0);
    CHECK(full.equity_curve != nullptr);
    CHECK(actual.trades != nullptr && full.trades != nullptr);
    if (actual.trades && full.trades && actual.trades_len == full.trades_len) {
        for (int i = 0; i < full.trades_len; ++i) {
            const auto& left = actual.trades[i];
            const auto& right = full.trades[i];
#define SAME(field) CHECK(left.field == right.field)
#define SAME_BITS(field) CHECK(same_bits(left.field, right.field))
            SAME(entry_time);
            SAME(exit_time);
            SAME_BITS(entry_price);
            SAME_BITS(exit_price);
            SAME_BITS(pnl);
            SAME_BITS(pnl_pct);
            SAME(is_long);
            SAME_BITS(max_runup);
            SAME_BITS(max_drawdown);
            SAME_BITS(qty);
            SAME_BITS(commission);
            SAME(entry_bar_index);
            SAME(exit_bar_index);
            SAME(open_at_end);
#undef SAME_BITS
#undef SAME
        }
    }
}

void test_summary_only_ratios_not_silently_pending() {
    const auto bars = four_month_tape();
    DefaultRecorderHost full, candidate;
    configure(full);
    configure(candidate);
    CHECK(full.equity_report_mode() == ReportMode::FULL);
    CHECK(candidate.equity_report_mode() == ReportMode::FULL);
    CHECK(candidate.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0);

    full.run(bars.data(), static_cast<int>(bars.size()));
    candidate.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(full.last_run_status() == 0 && full.last_error().empty());
    CHECK(candidate.last_run_status() == 0 && candidate.last_error().empty());
    CHECK(full.callbacks == static_cast<int>(bars.size()));
    CHECK(candidate.callbacks == static_cast<int>(bars.size()));
    Report reference, result;
    full.fill_report(&reference.value);
    candidate.fill_report(&result.value);
    CHECK(full.equity_report_mode() == ReportMode::FULL);
    CHECK(candidate.equity_report_mode() == ReportMode::SUMMARY_ONLY);
    CHECK(reference.value.script_bars_processed == static_cast<std::int64_t>(bars.size()));
    CHECK(result.value.script_bars_processed == static_cast<std::int64_t>(bars.size()));
    CHECK(reference.value.equity_curve_len == static_cast<std::int64_t>(bars.size()));
    CHECK(reference.value.equity_curve != nullptr);
    if (reference.value.equity_curve
        && reference.value.equity_curve_len == static_cast<std::int64_t>(bars.size())) {
        CHECK(reference.value.equity_curve[2].equity == 10006.0);
        CHECK(reference.value.equity_curve[5].equity == 9992.0);
        CHECK(reference.value.equity_curve[8].equity == 10008.0);
        CHECK(reference.value.equity_curve[11].equity == 9997.0);
    }
    CHECK(reference.value.total_trades == 1 && reference.value.trades_len == 1);
    CHECK(reference.value.net_profit == -3.0);
    require_finite_ratios(reference.value.metrics.equity);
    require_pending_ratios(result.value.metrics.equity);
    CHECK(!candidate.equity_summary_ratios_ready());
    require_same_non_ratio_report(result.value, reference.value);

    // Follow test_summary_metrics_allocations: build from the frozen first
    // pass, fold the FULL run's final points, install, then refill the report.
    const auto* first = candidate.equity_summary_snapshot();
    CHECK(first != nullptr);
    CHECK(candidate.equity_logical_point_count() == static_cast<std::int64_t>(bars.size()));
    if (first && reference.value.equity_curve) {
        CHECK(first->point_count == static_cast<std::int64_t>(bars.size()));
        CHECK(first->observation_count == static_cast<std::int64_t>(bars.size()));
        CHECK(first->bars_in_market == static_cast<std::int64_t>(bars.size()) - 1);
        CHECK(first->month_returns.count == 3); // Four month ends.
        CHECK(first->bar_returns.count == bars.size() - 1);
        CHECK(first->needs_second_pass());
        pineforge::metrics::EquityMetricsSecondPass second(*first);
        for (std::int64_t i = 0; i < reference.value.equity_curve_len; ++i) {
            const auto& point = reference.value.equity_curve[i];
            // This fixture is exposed from the second bar's fill onward.
            // Replay includes exposure in the ordered first-pass witness.
            second.fold(point, pineforge::metrics::equity_month_key(point.time_ms, "UTC"),
                        true, i > 0);
        }
        CHECK(candidate.install_equity_ratio_result(second.finish()));
        CHECK(candidate.equity_summary_ratios_ready());
        pineforge::BacktestEngine::free_report(&result.value);
        result.value = {};
        candidate.fill_report(&result.value);
        CHECK(candidate.equity_summary_ratios_ready());
        require_finite_ratios(result.value.metrics.equity);
        require_same_non_ratio_report(result.value, reference.value);
        // Includes all four ratio fields, with bit equality rather than tolerance.
        CHECK(std::memcmp(&result.value.metrics, &reference.value.metrics,
                          sizeof(pf_metrics_t)) == 0);
    }
}

} // namespace

int main() {
    test_summary_only_ratios_not_silently_pending();
    std::printf("F3 summary ratios: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
