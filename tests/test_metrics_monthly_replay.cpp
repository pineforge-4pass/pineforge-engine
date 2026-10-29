// Monthly replay is checked against the actual legacy curve and independent
// frozen references. All floating comparisons include NaN payloads and zeros.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/metrics.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "metrics_streaming_stop_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::metrics;
namespace reference = equity_stats_reference;
namespace fixture = metrics_streaming_test;

constexpr MetricsNeedMaskV1 kScalarNeeds =
    kMetricsNeedAllV1 & ~(kMetricsNeedReturnsV1 | kMetricsNeedCurveV1);
constexpr MetricsNeedMaskV1 kBarNeeds = kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1;
constexpr std::int64_t kHour = 3'600'000;
constexpr std::size_t kBatchBytes = 64U * 1024U;
static_assert(sizeof(EquityMetricsFold) <= 256, "fixed scalar summary");
static_assert(sizeof(EquityFoldSnapshot) <= 160, "fixed summary snapshot");
static_assert(sizeof(void*) != 8 || sizeof(MetricsStreamingResultV1) == 304,
              "streaming result includes the final-equity receipt");
static_assert(sizeof(MetricsMonthlyResultV1) <= 32, "fixed monthly companion");

std::string selected_row;
int rows = 0, checks = 0, failures = 0;

void check(bool condition, const char* expression, const std::string& name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s\n", name.c_str(), expression);
    }
}
#define CHECK(name, expression) check((expression), #expression, (name))

template<class Function>
void row(const std::string& name, Function function) {
    if (!selected_row.empty() && name.find(selected_row) == std::string::npos) return;
    ++rows;
    try {
        function(name);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] unexpected exception: %s\n", name.c_str(), error.what());
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] unexpected nonstandard exception\n", name.c_str());
    }
}

bool same(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

void same_point(const std::string& name, const pf_equity_point_t& a,
                const pf_equity_point_t& b) {
    CHECK(name, a.time_ms == b.time_ms);
    CHECK(name, same(a.equity, b.equity));
    CHECK(name, same(a.open_profit, b.open_profit));
}

void same_equity(const std::string& name, const pf_equity_stats_t& a,
                 const pf_equity_stats_t& b, MetricsNeedMaskV1 need = kScalarNeeds) {
#define FIELD(field) CHECK(name, same(a.field, b.field))
    FIELD(max_equity_drawdown); FIELD(max_equity_drawdown_pct);
    FIELD(max_equity_runup); FIELD(max_equity_runup_pct);
    FIELD(buy_hold_return); FIELD(buy_hold_return_pct);
    FIELD(sharpe_monthly); FIELD(sortino_monthly);
    if (need & kMetricsNeedSharpeBarV1) { FIELD(sharpe_bar); }
    if (need & kMetricsNeedSortinoBarV1) { FIELD(sortino_bar); }
    FIELD(cagr); FIELD(calmar); FIELD(recovery_factor);
    FIELD(time_in_market_pct); FIELD(open_pl);
#undef FIELD
}

void same_trades(const std::string& name, const pf_trade_stats_t& a,
                 const pf_trade_stats_t& b) {
    CHECK(name, a.num_trades == b.num_trades);
    CHECK(name, a.num_wins == b.num_wins);
    CHECK(name, a.num_losses == b.num_losses);
    CHECK(name, a.num_even == b.num_even);
    CHECK(name, a.max_consecutive_wins == b.max_consecutive_wins);
    CHECK(name, a.max_consecutive_losses == b.max_consecutive_losses);
#define FIELD(field) CHECK(name, same(a.field, b.field))
    FIELD(percent_profitable); FIELD(net_profit); FIELD(net_profit_pct);
    FIELD(gross_profit); FIELD(gross_profit_pct); FIELD(gross_loss); FIELD(gross_loss_pct);
    FIELD(profit_factor); FIELD(avg_trade); FIELD(avg_trade_pct);
    FIELD(avg_win); FIELD(avg_win_pct); FIELD(avg_loss); FIELD(avg_loss_pct);
    FIELD(ratio_avg_win_avg_loss); FIELD(largest_win); FIELD(largest_win_pct);
    FIELD(largest_loss); FIELD(largest_loss_pct); FIELD(commission_paid);
    FIELD(expectancy); FIELD(avg_bars_in_trade); FIELD(avg_bars_in_wins);
    FIELD(avg_bars_in_losses);
#undef FIELD
}

void same_stopped_result(const std::string& name, const fixture::RunResult& a,
                         const fixture::RunResult& b) {
    CHECK(name, a.code == b.code && a.args == b.args && a.message == b.message);
    CHECK(name, a.status == b.status && a.lifecycle == b.lifecycle);
    CHECK(name, a.inputs == b.inputs && a.scripts == b.scripts);
    CHECK(name, a.report_counts == b.report_counts);
    CHECK(name, a.hashes == b.hashes && a.trace_names == b.trace_names);
    CHECK(name, same(a.net_profit, b.net_profit));
    same_equity(name, a.metrics.equity, b.metrics.equity);
    same_trades(name, a.metrics.all, b.metrics.all);
    same_trades(name, a.metrics.longs, b.metrics.longs);
    same_trades(name, a.metrics.shorts, b.metrics.shorts);
    CHECK(name, a.curve.size() == b.curve.size());
    for (std::size_t i = 0; i < std::min(a.curve.size(), b.curve.size()); ++i)
        same_point(name, a.curve[i], b.curve[i]);
    CHECK(name, a.trades.size() == b.trades.size());
    for (std::size_t i = 0; i < std::min(a.trades.size(), b.trades.size()); ++i) {
        const auto& x = a.trades[i];
        const auto& y = b.trades[i];
        CHECK(name, x.entry_time == y.entry_time && x.exit_time == y.exit_time);
        CHECK(name, x.entry_bar_index == y.entry_bar_index && x.exit_bar_index == y.exit_bar_index);
        CHECK(name, x.is_long == y.is_long && x.open_at_end == y.open_at_end);
#define FIELD(field) CHECK(name, same(x.field, y.field))
        FIELD(entry_price); FIELD(exit_price); FIELD(pnl); FIELD(pnl_pct);
        FIELD(max_runup); FIELD(max_drawdown); FIELD(qty); FIELD(commission);
#undef FIELD
    }
    CHECK(name, a.security.size() == b.security.size());
    for (std::size_t i = 0; i < std::min(a.security.size(), b.security.size()); ++i) {
        const auto& x = a.security[i];
        const auto& y = b.security[i];
        CHECK(name, x.sec_id == y.sec_id && x.feed_count == y.feed_count);
        CHECK(name, x.eval_complete_count == y.eval_complete_count);
        CHECK(name, x.eval_partial_count == y.eval_partial_count);
    }
    CHECK(name, a.trace.size() == b.trace.size());
    for (std::size_t i = 0; i < std::min(a.trace.size(), b.trace.size()); ++i) {
        const auto& x = a.trace[i];
        const auto& y = b.trace[i];
        CHECK(name, x.timestamp == y.timestamp && x.bar_index == y.bar_index);
        CHECK(name, x.name_id == y.name_id && same(x.value, y.value));
    }
}

std::int64_t utc_ms(int year, int month, int day = 1, int hour = 0) {
    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    return static_cast<std::int64_t>(::timegm(&parts)) * 1000;
}

std::int64_t month_start(int index) {
    return utc_ms(2020 + index / 12, 1 + index % 12);
}

std::uint64_t splitmix64(std::uint64_t value) {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

double noise(std::int64_t index) {
    return static_cast<double>(splitmix64(static_cast<std::uint64_t>(index)) >> 11)
        * 0x1p-52 - 1.0;
}

// Every month has its exact first/last millisecond. Intra-month points make a
// premature/late end distinguishable from either adjacent month's true end.
std::vector<pf_equity_point_t> calendar_points(int months, int bars_per_month = 9) {
    std::vector<pf_equity_point_t> out;
    for (int m = 0; m < months; ++m) {
        const auto first = month_start(m), span = month_start(m + 1) - first;
        for (int i = 0; i < bars_per_month; ++i) {
            const auto time = first + (span - 1) * i / (bars_per_month - 1);
            const auto index = static_cast<std::int64_t>(out.size());
            const double equity = 100.0 + 17.0 * noise(index) + 0.125 * m;
            out.push_back({time, equity, i % 2 ? -0.0 : equity - 100.0});
        }
    }
    return out;
}

std::vector<pf_equity_point_t> timezone_points() {
    std::vector<pf_equity_point_t> out;
    const std::int64_t offsets[] = {-1, 0, 4 * kHour - 1, 4 * kHour,
                                    5 * kHour - 1, 5 * kHour, 12 * kHour};
    for (int m = 0; m < 15; ++m) {
        const auto boundary = month_start(m + 1);
        for (const auto offset : offsets) {
            const auto index = static_cast<std::int64_t>(out.size());
            const double equity = 100.0 + 23.0 * noise(index) + 0.25 * m;
            out.push_back({boundary + offset, equity, equity - 100.0});
        }
    }
    return out;
}

std::vector<pf_equity_point_t> special_months(const std::string& kind) {
    auto out = calendar_points(12);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto month = i / 9;
        double& equity = out[i].equity;
        if (kind == "flat" && month == 4) equity = out[3 * 9 + 8].equity;
        if (kind == "zero-return" && month == 7) equity = out[6 * 9 + 8].equity;
        if (kind == "negative-return" && month % 3 == 1)
            equity = out[(month - 1) * 9 + 8].equity / 2.0;
        if (kind == "heavy-tail") {
            const double scale = month % 4 == 0 ? 0x1p-30 : month % 4 == 1 ? 0x1p30 : 1.0;
            equity *= scale;
        }
        if (kind == "nonpositive" && month % 4 == 1) equity = 0.0;
        if (kind == "nonpositive" && month % 4 == 2) equity = -equity;
        out[i].open_profit = equity - 100.0;
    }
    if (kind == "one-bar")
        out.erase(out.begin() + 4 * 9 + 1, out.begin() + 5 * 9);
    if (kind == "empty-gap")
        out.erase(out.begin() + 4 * 9, out.begin() + 8 * 9);
    return out;
}

MetricsStreamingConfigV1 config(MetricsNeedMaskV1 need) {
    auto out = fixture::config();
    out.metrics_needed = need;
    out.curve_available = 0;
    return out;
}

MetricsExtentPlanV1 plan_for(const std::vector<pf_equity_point_t>& points) {
    return fixture::plan(fixture::source(points));
}

int checked_key(const pf_equity_point_t& point, const std::string& zone) {
    return monthly_replay_time_supported_v1(point.time_ms)
        ? equity_month_key(point.time_ms, zone) : 0;
}

struct Provider {
    const std::vector<pf_equity_point_t>& points;
    std::size_t reads = 0;
    static pf_equity_point_t read(void* context, std::int64_t index) {
        auto& self = *static_cast<Provider*>(context);
        ++self.reads;
        return self.points.at(static_cast<std::size_t>(index));
    }
    MetricsRatioCurveV1 view() {
        return {this, static_cast<std::int64_t>(points.size()), &read};
    }
};

MetricsMonthlyResultV1 monthly_result(const std::string& name,
                                     const StreamingEquityRatiosV1& fold) {
    MetricsMonthlyResultV1 out;
    CHECK(name, streaming_monthly_result_v1(fold, out) == MetricsStreamingStatusV1::Ok);
    return out;
}

std::unique_ptr<StreamingEquityRatiosV1> make_fold(
        const std::string& name, const std::vector<pf_equity_point_t>& points,
        MetricsNeedMaskV1 need = kScalarNeeds) {
    std::unique_ptr<StreamingEquityRatiosV1> out;
    CHECK(name, make_streaming_equity_ratios_v1(config(need), plan_for(points), {}, out)
        == MetricsStreamingStatusV1::Ok);
    if (!out) throw std::logic_error("monthly fold absent");
    CHECK(name, enable_streaming_monthly_replay_v1(*out) == MetricsStreamingStatusV1::Ok);
    return out;
}

EquityRatioResult ratios_for(const EquityFoldSnapshot& first,
                             const MetricsStreamingResultV1& bars,
                             const MetricsMonthlyResultV1& months) {
    EquityRatioResult out;
    out.expected = out.replayed = first;
    out.complete = months.complete != 0;
    out.sharpe_monthly = months.sharpe_monthly;
    out.sortino_monthly = months.sortino_monthly;
    out.sharpe_bar = bars.sharpe_bar;
    out.sortino_bar = bars.sortino_bar;
    out.nonfinite_field = bars.failure_field;
    return out;
}

const EquityMetricInputs kInputs{100.0, 95.0, 103.0, 7.25};

void compare_finish(const std::string& name, StreamingEquityRatiosV1& fold,
                    const EquityMetricsFold& summary,
                    const std::vector<pf_equity_point_t>& points,
                    const std::string& zone, MetricsNeedMaskV1 need = kScalarNeeds) {
    const auto snapshot = summary.first_pass();
    std::int64_t exposure = 0;
    for (std::size_t i = 0; i < points.size(); ++i) if (i % 3 != 0) ++exposure;
    const auto expected = compute_equity_stats(std::as_const(points).data(), points.size(),
        kInputs.initial_capital, zone, kInputs.first_open, kInputs.last_close,
        exposure, kInputs.net_profit);
    const auto independent = reference::compute_equity_stats(points.data(), points.size(),
        kInputs.initial_capital, zone, kInputs.first_open, kInputs.last_close,
        exposure, kInputs.net_profit);
    same_equity(name, expected, independent);
    // A switch-Off control reads the real actual curve. Its provider, rather
    // than a speculative append prefix, is authoritative on that route.
    auto old_config = config(kBarNeeds);
    old_config.streaming_sharpe = StreamingSharpeV1::Off;
    old_config.curve_available = 1;
    std::unique_ptr<StreamingEquityRatiosV1> old_fold;
    CHECK(name, make_streaming_equity_ratios_v1(old_config, plan_for(points), {}, old_fold)
        == MetricsStreamingStatusV1::Ok);
    if (!old_fold) throw std::logic_error("legacy curve control absent");
    Provider old_provider{points};
    MetricsStreamingResultV1 old_bars;
    CHECK(name, old_fold->finish(old_bars, old_provider.view()) == MetricsStreamingStatusV1::Ok);
    CHECK(name, old_bars.receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
    CHECK(name, points.empty() || old_provider.reads > 0);
    CHECK(name, same(old_bars.sharpe_bar, expected.sharpe_bar));
    CHECK(name, same(old_bars.sortino_bar, expected.sortino_bar));
    Provider provider{points};
    MetricsStreamingResultV1 actual;
    CHECK(name, fold.finish(actual, provider.view()) == MetricsStreamingStatusV1::Ok);
    const auto months = monthly_result(name, fold);
    CHECK(name, actual.receipt.route == MetricsRatioRouteV1::Streaming);
    CHECK(name, actual.receipt.route_reason == MetricsRouteReasonV1::OrderedReplay);
    CHECK(name, actual.receipt.metrics_needed == need);
    CHECK(name, provider.reads == 0);
    CHECK(name, months.complete);
    CHECK(name, actual.failure_code == nullptr);
    const auto ratios = ratios_for(snapshot, actual, months);
    same_equity(name, finalize_equity_metrics(snapshot, kInputs, &ratios), expected, need);
    const auto bytes = fold.state_size_bytes();
    MetricsStreamingResultV1 again;
    CHECK(name, fold.finish(again) == MetricsStreamingStatusV1::Ok);
    const auto repeated = monthly_result(name, fold);
    CHECK(name, fold.state_size_bytes() == bytes);
    CHECK(name, same(again.sharpe_bar, actual.sharpe_bar));
    CHECK(name, same(again.sortino_bar, actual.sortino_bar));
    CHECK(name, same(again.centered_sum, actual.centered_sum));
    CHECK(name, same(again.downside_sum, actual.downside_sum));
    CHECK(name, same(repeated.sharpe_monthly, expected.sharpe_monthly));
    CHECK(name, same(repeated.sortino_monthly, expected.sortino_monthly));
}

std::size_t pure_series(const std::string& name,
                        const std::vector<pf_equity_point_t>& points,
                        const std::string& zone = "UTC",
                        MetricsNeedMaskV1 need = kScalarNeeds) {
    auto fold = make_fold(name, points, need);
    EquityMetricsFold summary;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const int key = checked_key(points[i], zone);
        CHECK(name, fold->append(points[i], key) == MetricsStreamingStatusV1::Ok);
        summary.fold(points[i], key, true, i % 3 != 0);
    }
    CHECK(name, fold->state_size_bytes()
        <= kBatchBytes + 4096 + 64 + kMetricsMonthEndLimitV1 * sizeof(double));
    compare_finish(name, *fold, summary, points, zone, need);
    return fold->state_size_bytes();
}

class CalendarHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_run_begin() override { callbacks = 0; }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1)
            (void)submit({native_order::Transact{10.0}, "calendar-entry", ""});
        else if (callbacks % 7 == 1)
            (void)submit({native_order::ReverseTo{10.0}, "calendar-long", ""});
        if (callbacks % 7 == 4)
            (void)submit({native_order::ReverseTo{-7.0}, "calendar-short", ""});
        if (!mark_native_report_point(bar.timestamp))
            throw std::logic_error("calendar fixture mark refused");
    }
    std::size_t curve_capacity() const { return equity_curve_.capacity(); }
    bool no_curve() const { return metrics_streaming_.no_curve; }
    void amend_tail(const pf_equity_point_t& point) {
        auto* tail = last_equity_point();
        if (!tail) throw std::logic_error("missing amendable tail");
        *tail = point;
        refold_equity_tail();
    }
    pf_equity_point_t tail() const {
        const auto* point = last_equity_point();
        if (!point) throw std::logic_error("missing held point");
        return *point;
    }
protected:
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        BacktestEngine::hash_host_extension(sink);
        sink.s("calendar-metrics/v1");
        sink.i(callbacks);
    }
};

class CommissionCalendarHost final : public source::PineStrategyHost {
public:
    CommissionCalendarHost() {
        source::PineStrategyConfig settings;
        settings.initial_capital = 100000.0;
        settings.default_qty_value = 100.0;
        settings.pyramiding = 2;
        settings.process_orders_on_close = true;
        settings.commission_value = .1;
        configure_pine_strategy(settings);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("calendar-first", true);
        if (pine_bar_index() == 2) strategy_entry("calendar-second", true);
    }
    std::size_t curve_capacity() const { return equity_curve_.capacity(); }
    bool no_curve() const { return metrics_streaming_.no_curve; }
};

NativeRunSpec host_spec(const std::string& zone) {
    auto out = fixture::native_spec();
    out.identity = {"calendar-metrics", 1};
    out.timezone = "UTC";
    out.chart_timezone = zone;
    return out;
}

void prepare_host(const std::string& name, BacktestEngine& host,
                   const std::vector<pf_equity_point_t>& planned_points,
                   MetricsNeedMaskV1 need, bool recovery = true) {
    // Leave literal All untouched to exercise the actual default preference.
    if (need != kMetricsNeedAllV1)
        CHECK(name, host.set_metrics_streaming_v1(StreamingSharpeV1::On, need)
            == MetricsStreamingStatusV1::Ok);
    MetricsPlainDeclarationV1 declaration;
    declaration.builtin_presenter = declaration.builtin_marks = declaration.builtin_scheduler = 1;
    MetricsPlainInputsV1 inputs;
    inputs.declaration = &declaration;
    inputs.module_contract_bound = inputs.historical_batch = inputs.equal_resolved_timeframes = 1;
    inputs.intrabar_none = inputs.builtin_host_marks = inputs.feed_tolerant_passthrough = 1;
    inputs.final_admitted_view_bound = 1;
    const auto planned = plan_for(planned_points);
    MetricsExtentReceiptV1 receipt;
    CHECK(name, host.admit_metrics_plain_v1(inputs, planned, &receipt)
        == MetricsStreamingStatusV1::Ok);
    CHECK(name, receipt.plain_subset);
    auto cfg = config(need);
    cfg.deterministic_replay = recovery ? 1u : 0u;
    CHECK(name, host.prepare_metrics_streaming_v1(cfg, planned) == MetricsStreamingStatusV1::Ok);
}

struct OwnedReport {
    ReportC value{};
    OwnedReport() = default;
    OwnedReport(const OwnedReport&) = delete;
    OwnedReport& operator=(const OwnedReport&) = delete;
    ~OwnedReport() { BacktestEngine::free_report(&value); }
};

std::vector<Bar> bars_for(const std::vector<pf_equity_point_t>& points) {
    std::vector<Bar> out;
    for (const auto& point : points)
        out.push_back({point.equity, point.equity + 1.0, point.equity - 1.0,
                       point.equity, 1.0, point.time_ms});
    return out;
}

void compare_reports(const std::string& name, const ReportC& actual, const ReportC& expected,
                     MetricsNeedMaskV1 need) {
    same_equity(name, actual.metrics.equity, expected.metrics.equity, need);
    same_trades(name, actual.metrics.all, expected.metrics.all);
    same_trades(name, actual.metrics.longs, expected.metrics.longs);
    same_trades(name, actual.metrics.shorts, expected.metrics.shorts);
    CHECK(name, same(actual.net_profit, expected.net_profit));
    CHECK(name, actual.total_trades == expected.total_trades);
    CHECK(name, actual.trades_len == expected.trades_len);
    CHECK(name, actual.input_bars_processed == expected.input_bars_processed);
    CHECK(name, actual.script_bars_processed == expected.script_bars_processed);
}

void engine_series(const std::string& name,
                    const std::vector<pf_equity_point_t>& points,
                    const std::string& zone = "UTC",
                    MetricsNeedMaskV1 need = kScalarNeeds, bool expect_streaming = true,
                    bool recovery = true, bool amend = false,
                    const std::vector<pf_equity_point_t>* plan_override = nullptr) {
    CalendarHost control, actual;
    CHECK(name, control.configure_native(host_spec(zone)).status == NativeSetupStatus::Applied);
    CHECK(name, actual.configure_native(host_spec(zone)).status == NativeSetupStatus::Applied);
    CHECK(name, control.set_metrics_streaming_v1(StreamingSharpeV1::Off, kMetricsNeedAllV1)
        == MetricsStreamingStatusV1::Ok);
    const auto& planned = plan_override ? *plan_override : points;
    prepare_host(name, actual, planned, need, recovery);
    const auto bars = bars_for(points);
    control.run(bars.data(), static_cast<int>(bars.size()));
    actual.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(name, control.last_run_status() == 0 && control.last_error().empty());
    CHECK(name, actual.last_run_status() == 0 && actual.last_error().empty());
    CHECK(name, actual.callbacks == control.callbacks);
    CHECK(name, actual.no_curve() == expect_streaming);
    if (amend) {
        auto replacement = control.tail();
        replacement.equity *= 0.91;
        replacement.open_profit = -replacement.open_profit;
        control.amend_tail(replacement);
        actual.amend_tail(replacement);
    }
    OwnedReport expected, report, again;
    control.fill_report(&expected.value);
    actual.fill_report(&report.value);
    actual.fill_report(&again.value);
    CHECK(name, expected.value.equity_curve_len == static_cast<std::int64_t>(points.size()));
    compare_reports(name, report.value, expected.value, need);
    compare_reports(name, again.value, expected.value, need);
    CHECK(name, actual.broker_state_hash() == control.broker_state_hash());
    CHECK(name, actual.equity_logical_point_count() == expected.value.equity_curve_len);
    MetricsExtentReceiptV1 receipt;
    CHECK(name, actual.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
    CHECK(name, receipt.metrics_needed == need);
    CHECK(name, receipt.actual_last_equity_valid ==
        (expected.value.equity_curve_len > 0 ? 1u : 0u));
    if (expected.value.equity_curve && expected.value.equity_curve_len > 0)
        CHECK(name, same(receipt.actual_last_equity,
            expected.value.equity_curve[expected.value.equity_curve_len - 1].equity));
    const bool matching = fixture::same_extent(plan_for(planned).extent, receipt.actual_extent);
    CHECK(name, receipt.witness == (matching ? MetricsExtentWitnessV1::Match
                                             : MetricsExtentWitnessV1::Mismatch));
    if (expect_streaming) {
        CHECK(name, receipt.route == MetricsRatioRouteV1::Streaming);
        CHECK(name, receipt.route_reason == MetricsRouteReasonV1::OrderedReplay);
        CHECK(name, actual.curve_capacity() <= 1);
        CHECK(name, report.value.equity_curve == nullptr && report.value.equity_curve_len == 0);
        CHECK(name, actual.equity_summary_ratios_ready());
    } else {
        CHECK(name, receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
        CHECK(name, report.value.equity_curve_len == expected.value.equity_curve_len);
        for (std::int64_t i = 0; i < report.value.equity_curve_len; ++i)
            same_point(name, report.value.equity_curve[i], expected.value.equity_curve[i]);
    }
}

void calendar_rows() {
    for (const int months : {1, 2, 3, 12, 60}) {
        for (const auto need : {kMetricsNeedSharpeBarV1, kMetricsNeedSortinoBarV1,
                                kBarNeeds, kScalarNeeds})
            row("generated-months-" + std::to_string(months) + "-mask-" + std::to_string(need),
                [=](const std::string& name) {
                    const auto points = calendar_points(months);
                    pure_series(name, points, "UTC", need);
                    engine_series(name, points, "UTC", need);
                });
    }
    for (const auto* kind : {"one-bar", "flat", "empty-gap", "negative-return",
                             "zero-return", "heavy-tail", "nonpositive"}) {
        row(std::string("months-") + kind, [=](const std::string& name) {
            const auto points = special_months(kind);
            pure_series(name, points);
            if (std::string(kind) != "nonpositive" && std::string(kind) != "heavy-tail")
                engine_series(name, points);
        });
    }
    for (const auto* zone : {"UTC", "Etc/UTC", "America/New_York", "Asia/Taipei", "Europe/Berlin"})
        row(std::string("chart-calendar-") + zone, [=](const std::string& name) {
            const auto points = timezone_points();
            pure_series(name, points, zone);
            engine_series(name, points, zone);
        });
    for (const auto mask : {kMetricsNeedSharpeBarV1, kMetricsNeedSortinoBarV1, kBarNeeds,
                            kMetricsNeedMonthlyRatiosV1,
                            kMetricsNeedMonthlyRatiosV1 | kMetricsNeedSharpeBarV1,
                            kMetricsNeedMonthlyRatiosV1 | kMetricsNeedSortinoBarV1,
                            kMetricsNeedMonthlyRatiosV1 | kBarNeeds, kScalarNeeds,
                            kMetricsNeedEquitySummaryV1, kMetricsNeedTradeStatsV1,
                            MetricsNeedMaskV1{0}})
        row("scalar-mask-" + std::to_string(mask), [=](const std::string& name) {
            const auto points = calendar_points(60);
            pure_series(name, points, "UTC", mask);
            engine_series(name, points, "UTC", mask);
        });
    row("default-mask-retains-every-curve-point", [](const std::string& name) {
        CalendarHost fresh;
        MetricsExtentReceiptV1 defaults;
        CHECK(name, fresh.metrics_extent_receipt_v1(&defaults) == MetricsStreamingStatusV1::Ok);
        CHECK(name, defaults.metrics_needed == kMetricsNeedAllV1);
        CHECK(name, MetricsStreamingConfigV1{}.metrics_needed == kMetricsNeedAllV1);
        engine_series(name, timezone_points(), "America/New_York", kMetricsNeedAllV1, false);
    });
    for (const auto sequence : {kMetricsNeedReturnsV1, kMetricsNeedCurveV1})
        row("sequence-mask-" + std::to_string(sequence), [=](const std::string& name) {
            engine_series(name, calendar_points(12), "UTC", kScalarNeeds | sequence, false);
        });
    row("no-recovery-certificate-keeps-curve", [](const std::string& name) {
        engine_series(name, calendar_points(12), "UTC", kScalarNeeds, false, false);
    });
    row("amended-engine-month-end", [](const std::string& name) {
        engine_series(name, calendar_points(12), "UTC", kScalarNeeds, true, true, true);
    });
    row("source-range-end-commission-multi-month", [](const std::string& name) {
        const auto points = calendar_points(12);
        const auto bars = bars_for(points);
        CommissionCalendarHost control, actual;
        CHECK(name, control.set_metrics_streaming_v1(StreamingSharpeV1::Off, kMetricsNeedAllV1)
            == MetricsStreamingStatusV1::Ok);
        prepare_host(name, actual, points, kScalarNeeds);
        control.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        actual.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        CHECK(name, control.last_run_status() == 0 && control.last_error().empty());
        CHECK(name, actual.last_run_status() == control.last_run_status());
        CHECK(name, actual.last_error() == control.last_error());
        OwnedReport expected, report, again;
        control.fill_report(&expected.value);
        actual.fill_report(&report.value);
        actual.fill_report(&again.value);
        CHECK(name, expected.value.equity_curve_len == static_cast<std::int64_t>(points.size()));
        compare_reports(name, report.value, expected.value, kScalarNeeds);
        compare_reports(name, again.value, expected.value, kScalarNeeds);
        CHECK(name, actual.no_curve() && actual.curve_capacity() <= 1);
        CHECK(name, report.value.equity_curve == nullptr);
        CHECK(name, actual.broker_state_hash() == control.broker_state_hash());
        MetricsExtentReceiptV1 receipt;
        CHECK(name, actual.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
        CHECK(name, receipt.route == MetricsRatioRouteV1::Streaming);
        if (expected.value.equity_curve && expected.value.equity_curve_len > 0) {
            const auto old_bars = fixture::legacy(
                fixture::source(std::vector<pf_equity_point_t>(expected.value.equity_curve,
                    expected.value.equity_curve + expected.value.equity_curve_len)));
            CHECK(name, same(report.value.metrics.equity.sharpe_bar, old_bars.sharpe));
            CHECK(name, same(report.value.metrics.equity.sortino_bar, old_bars.sortino));
        } else {
            CHECK(name, false);
        }
    });
    row("empty-curve", [](const std::string& name) { pure_series(name, {}); });
    row("one-point-curve", [](const std::string& name) {
        pure_series(name, {{month_start(0), 103.0, -0.0}});
    });
    row("flat-multi-month-curve", [](const std::string& name) {
        auto points = calendar_points(12);
        for (auto& point : points) { point.equity = 100.0; point.open_profit = -0.0; }
        pure_series(name, points);
        engine_series(name, points);
    });
}

void mutation_rows() {
    row("mutation-merge-two-months", [](const std::string& name) {
        const auto points = calendar_points(12);
        pure_series(name, points);
        engine_series(name, points);
    });
    row("mutation-drop-month-end", [](const std::string& name) {
        const auto points = special_months("one-bar");
        pure_series(name, points);
        engine_series(name, points);
    });
    row("mutation-wrong-chart-timezone", [](const std::string& name) {
        const auto points = timezone_points();
        const auto boundary = month_start(1);
        CHECK(name, equity_month_key(boundary, "UTC")
            != equity_month_key(boundary, "America/New_York"));
        engine_series(name, points, "America/New_York");
    });
    row("mutation-month-end-one-bar-late", [](const std::string& name) {
        const auto points = calendar_points(12);
        pure_series(name, points);
        engine_series(name, points);
    });
}

void held_rows() {
    row("held-engine-clock-recovery-and-repair", [](const std::string& name) {
        const auto points = calendar_points(12);
        const auto bars = bars_for(points);
        CalendarHost control, actual;
        CHECK(name, control.configure_native(host_spec("UTC")).status == NativeSetupStatus::Applied);
        CHECK(name, actual.configure_native(host_spec("UTC")).status == NativeSetupStatus::Applied);
        CHECK(name, control.set_metrics_streaming_v1(StreamingSharpeV1::Off, kMetricsNeedAllV1)
            == MetricsStreamingStatusV1::Ok);
        prepare_host(name, actual, points, kScalarNeeds);
        control.run(bars.data(), static_cast<int>(bars.size()));
        actual.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(name, control.last_run_status() == 0 && actual.last_run_status() == 0);
        const auto original = control.tail();
        auto outside = original;
        outside.time_ms = (std::int64_t{1} << 40) * 1000;
        actual.amend_tail(outside);
        MetricsExtentReceiptV1 receipt;
        CHECK(name, actual.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
        CHECK(name, receipt.route == MetricsRatioRouteV1::LegacyRerun);
        OwnedReport unpublished;
        actual.fill_report(&unpublished.value);
        CHECK(name, unpublished.value.script_bars_processed == 0);
        actual.amend_tail(original);
        OwnedReport expected, report;
        control.fill_report(&expected.value);
        actual.fill_report(&report.value);
        compare_reports(name, report.value, expected.value, kScalarNeeds);
        CHECK(name, actual.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
        CHECK(name, receipt.route == MetricsRatioRouteV1::Streaming);
    });
    row("held-repair-month-change-repeat-and-append", [](const std::string& name) {
        auto points = calendar_points(4);
        // Without Sortino's extent proof requirement the same fold can be
        // queried before/after a time amendment and a subsequent append.
        const auto need = kMetricsNeedMonthlyRatiosV1 | kMetricsNeedSharpeBarV1;
        auto fold = make_fold(name, points, need);
        EquityMetricsFold summary;
        for (std::size_t i = 0; i < points.size(); ++i) {
            CHECK(name, fold->append(points[i], checked_key(points[i], "UTC"))
                == MetricsStreamingStatusV1::Ok);
            summary.fold(points[i], checked_key(points[i], "UTC"), true, i % 3 != 0);
        }
        compare_finish(name, *fold, summary, points, "UTC", need);
        const auto held = static_cast<std::int64_t>(points.size()) - 1;
        const auto final = points.back();
        auto bad = final;
        bad.time_ms = (std::int64_t{1} << 40) * 1000;
        bad.equity = std::numeric_limits<double>::quiet_NaN();
        CHECK(name, fold->amend_held(held, bad, 0) == MetricsStreamingStatusV1::Ok);
        MetricsStreamingResultV1 refused;
        CHECK(name, fold->finish(refused) == MetricsStreamingStatusV1::Ok);
        CHECK(name, refused.receipt.route == MetricsRatioRouteV1::LegacyRerun);
        CHECK(name, !monthly_result(name, *fold).complete);
        // Repair before release removes the speculative domain/numeric fault.
        points.back().equity *= 0.875;
        CHECK(name, fold->amend_held(held, points.back(), checked_key(points.back(), "UTC"))
            == MetricsStreamingStatusV1::Ok);
        summary.replace_last(points.back(), checked_key(points.back(), "UTC"));
        compare_finish(name, *fold, summary, points, "UTC", need);
        // Move the held point into a new month, then release that amended end.
        points.back().time_ms = month_start(4);
        CHECK(name, fold->amend_held(held, points.back(), checked_key(points.back(), "UTC"))
            == MetricsStreamingStatusV1::Ok);
        summary.replace_last(points.back(), checked_key(points.back(), "UTC"));
        compare_finish(name, *fold, summary, points, "UTC", need);
        points.push_back({month_start(5), final.equity * 1.125, final.open_profit});
        CHECK(name, fold->append(points.back(), checked_key(points.back(), "UTC"))
            == MetricsStreamingStatusV1::Ok);
        summary.fold(points.back(), checked_key(points.back(), "UTC"), true,
                     (points.size() - 1) % 3 != 0);
        compare_finish(name, *fold, summary, points, "UTC", need);
    });
    row("held-merge-back-into-current-month", [](const std::string& name) {
        auto points = calendar_points(4);
        points.push_back({month_start(4), 91.0, -9.0});
        const auto need = kMetricsNeedMonthlyRatiosV1 | kMetricsNeedSharpeBarV1;
        auto fold = make_fold(name, points, need);
        EquityMetricsFold summary;
        for (std::size_t i = 0; i < points.size(); ++i) {
            CHECK(name, fold->append(points[i], checked_key(points[i], "UTC"))
                == MetricsStreamingStatusV1::Ok);
            summary.fold(points[i], checked_key(points[i], "UTC"), true, i % 3 != 0);
        }
        points.back().time_ms = points[points.size() - 2].time_ms;
        points.back().equity *= 1.125;
        CHECK(name, fold->amend_held(points.size() - 1, points.back(),
            checked_key(points.back(), "UTC")) == MetricsStreamingStatusV1::Ok);
        summary.replace_last(points.back(), checked_key(points.back(), "UTC"));
        compare_finish(name, *fold, summary, points, "UTC", need);
    });
}

struct Failure {
    std::string code, args, message;
    bool operator==(const Failure& other) const {
        return code == other.code && args == other.args && message == other.message;
    }
};

template<class Function>
Failure failure_of(Function function) {
    try {
        function();
    } catch (const std::exception& error) {
        const auto value = classify_run_failure(error);
        return {run_failure_code_name(value.code), value.args ? *value.args : "{}", error.what()};
    }
    return {};
}

void numeric_rows() {
    for (const auto* kind : {"monthly-overflow", "earlier-point", "final-open-first"}) {
        row(std::string("numeric-order-") + kind, [=](const std::string& name) {
            std::vector<pf_equity_point_t> points{
                {month_start(0), 1e-300, 0.0}, {month_start(1), 1e100, 0.0},
                {month_start(2), 1e300, 0.0}, {month_start(3), 100.0, 0.0}};
            if (std::string(kind) != "monthly-overflow")
                points[1].equity = std::numeric_limits<double>::infinity();
            if (std::string(kind) == "final-open-first")
                points.back().open_profit = std::numeric_limits<double>::infinity();
            const auto expected = failure_of([&] {
                (void)compute_equity_stats(std::as_const(points).data(), points.size(), kInputs.initial_capital,
                    "UTC", kInputs.first_open, kInputs.last_close, 0, kInputs.net_profit);
            });
            auto fold = make_fold(name, points);
            EquityMetricsFold summary;
            for (const auto& point : points) {
                const auto key = checked_key(point, "UTC");
                CHECK(name, fold->append(point, key) == MetricsStreamingStatusV1::Ok);
                summary.fold(point, key);
            }
            MetricsStreamingResultV1 result;
            CHECK(name, fold->finish(result) == MetricsStreamingStatusV1::Ok);
            const auto ratios = ratios_for(summary.first_pass(), result, monthly_result(name, *fold));
            const auto actual = failure_of([&] {
                (void)finalize_equity_metrics(summary.first_pass(), kInputs, &ratios);
            });
            CHECK(name, !expected.code.empty());
            CHECK(name, actual == expected);
        });
    }
}

std::vector<Bar> monthly_stop_feed() {
    auto bars = fixture::stop_feed();
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i].timestamp = month_start(static_cast<int>(i)) + 14 * fixture::day;
    return bars;
}

std::vector<pf_equity_point_t> planned_points(const std::vector<Bar>& bars) {
    std::vector<pf_equity_point_t> out;
    for (const auto& bar : bars) out.push_back({bar.timestamp, bar.close, 0.0});
    return out;
}

NativeRunSpec stop_spec(bool margin) {
    auto spec = fixture::native_spec();
    if (margin) {
        NativeMarginModel model;
        model.initial_long = model.initial_short = .01;
        model.maintenance_long = model.maintenance_short = .01;
        model.sizing = NativeLiquidationSizing::Flatten;
        model.check = NativeLiquidationCheck::PathAdverseExtremeMark;
        spec.margin = model;
    }
    return spec;
}

void stop_trial(const std::string& name, fixture::StopSpec stop, MetricsNeedMaskV1 need) {
    const auto bars = monthly_stop_feed();
    const auto plan = planned_points(bars);
    fixture::CompletedMarkHost control(stop), actual(stop), rerun(stop);
    for (auto* host : {&control, &actual, &rerun})
        CHECK(name, host->configure_native(stop_spec(stop.margin)).status == NativeSetupStatus::Applied);
    for (auto* host : {&control, &rerun})
        CHECK(name, host->set_metrics_streaming_v1(StreamingSharpeV1::Off, kMetricsNeedAllV1)
            == MetricsStreamingStatusV1::Ok);
    prepare_host(name, actual, plan, need);
    control.run(bars.data(), bars.size());
    actual.run(bars.data(), bars.size());
    const auto expected = fixture::capture(control);
    CHECK(name, actual.last_run_status() == control.last_run_status());
    CHECK(name, actual.last_error() == control.last_error());
    const auto first_failure = run_failure_value_of(actual);
    const auto old_failure = run_failure_value_of(control);
    CHECK(name, first_failure.code == old_failure.code);
    CHECK(name, (first_failure.args ? *first_failure.args : "{}")
        == (old_failure.args ? *old_failure.args : "{}"));
    CHECK(name, actual.marks == control.marks);
    CHECK(name, actual.callbacks == control.callbacks);
    CHECK(name, actual.margins == control.margins);
    CHECK(name, actual.boundary.completed_marks == control.boundary.completed_marks);
    CHECK(name, actual.boundary.input_index == control.boundary.input_index);
    CHECK(name, actual.boundary.kind == control.boundary.kind);
    CHECK(name, actual.retained_capacity() <= 1);
    MetricsExtentReceiptV1 receipt;
    CHECK(name, actual.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
    CHECK(name, receipt.route == MetricsRatioRouteV1::LegacyRerun);
    CHECK(name, receipt.route_reason == MetricsRouteReasonV1::ExistingFailure);
    CHECK(name, receipt.replay_attempts == 1 && receipt.authoritative_attempt == 0);
    OwnedReport unpublished;
    actual.fill_report(&unpublished.value);
    CHECK(name, unpublished.value.equity_curve == nullptr);
    CHECK(name, unpublished.value.trades == nullptr && unpublished.value.trades_len == 0);
    CHECK(name, unpublished.value.script_bars_processed == 0);
    // Execute the authoritative recovery, repeating the injection. Expected
    // values come from the separate switch-Off control's real partial curve.
    rerun.run(bars.data(), bars.size());
    const auto authoritative = fixture::capture(rerun);
    same_stopped_result(name, authoritative, expected);
    CHECK(name, actual.begun == control.begun && rerun.begun == control.begun);
}

void stop_rows() {
    for (const int marks : {3, 6, 11, 12})
        row("stopped-completed-months-" + std::to_string(marks), [=](const std::string& name) {
            stop_trial(name, {marks, fixture::Injection::CompletedMark, false, false}, kScalarNeeds);
        });
    for (const auto mask : {kMetricsNeedSharpeBarV1, kMetricsNeedSortinoBarV1, kBarNeeds})
        row("interrupted-mask-" + std::to_string(mask), [=](const std::string& name) {
            stop_trial(name, {12, fixture::Injection::CompletedMark, false, false}, mask);
        });
    row("stopped-intra-bar-multi-month", [](const std::string& name) {
        stop_trial(name, {6, fixture::Injection::IntraBar, false, false}, kScalarNeeds);
    });
    row("stopped-margin-callback-multi-month", [](const std::string& name) {
        stop_trial(name, {6, fixture::Injection::MarginCallback, true, false}, kScalarNeeds);
    });
    row("stopped-after-margin-multi-month", [](const std::string& name) {
        stop_trial(name, {6, fixture::Injection::CompletedMark, true, false}, kScalarNeeds);
    });
    row("stopped-cooperative-multi-month", [](const std::string& name) {
        stop_trial(name, {6, fixture::Injection::Cooperative, false, false}, kScalarNeeds);
    });
}

void recovery_rows() {
    row("month-budget-recovers-complete-legacy-report", [](const std::string& name) {
        std::vector<pf_equity_point_t> points;
        for (std::size_t m = 0; m < kMetricsMonthEndLimitV1 + 3; ++m)
            points.push_back({month_start(static_cast<int>(m)), 100.0 + noise(m), 0.0});
        auto fold = make_fold(name, points);
        for (const auto& point : points)
            CHECK(name, fold->append(point, checked_key(point, "UTC")) == MetricsStreamingStatusV1::Ok);
        Provider provider{points};
        MetricsStreamingResultV1 result;
        CHECK(name, fold->finish(result, provider.view()) == MetricsStreamingStatusV1::Ok);
        CHECK(name, result.receipt.route == MetricsRatioRouteV1::LegacyRerun);
        CHECK(name, result.receipt.route_reason == MetricsRouteReasonV1::CurveNeeded);
        CHECK(name, !monthly_result(name, *fold).complete && provider.reads == 0);
        CHECK(name, fold->state_size_bytes()
            <= kBatchBytes + 4096 + 64 + kMetricsMonthEndLimitV1 * sizeof(double));
        CalendarHost control, actual, rerun;
        for (auto* host : {&control, &actual, &rerun})
            CHECK(name, host->configure_native(host_spec("UTC")).status == NativeSetupStatus::Applied);
        for (auto* host : {&control, &rerun})
            CHECK(name, host->set_metrics_streaming_v1(StreamingSharpeV1::Off, kMetricsNeedAllV1)
                == MetricsStreamingStatusV1::Ok);
        prepare_host(name, actual, points, kScalarNeeds);
        const auto bars = bars_for(points);
        control.run(bars.data(), bars.size());
        actual.run(bars.data(), bars.size());
        MetricsExtentReceiptV1 receipt;
        CHECK(name, actual.metrics_extent_receipt_v1(&receipt) == MetricsStreamingStatusV1::Ok);
        CHECK(name, receipt.route == MetricsRatioRouteV1::LegacyRerun);
        OwnedReport unpublished, expected, report;
        actual.fill_report(&unpublished.value);
        CHECK(name, unpublished.value.script_bars_processed == 0);
        rerun.run(bars.data(), bars.size());
        control.fill_report(&expected.value);
        rerun.fill_report(&report.value);
        compare_reports(name, report.value, expected.value, kScalarNeeds);
        CHECK(name, report.value.equity_curve_len == expected.value.equity_curve_len);
        for (std::int64_t i = 0; i < report.value.equity_curve_len; ++i)
            same_point(name, report.value.equity_curve[i], expected.value.equity_curve[i]);
    });
    row("unsupported-month-clock-keeps-legacy-curve", [](const std::string& name) {
        // Just outside the optimized calendar domain but safely inside int
        // month keys. Do not evaluate the legacy overflowing int64 endpoints.
        const auto edge = (std::int64_t{1} << 40) * 1000;
        for (const auto first : {-edge - fixture::year, edge}) {
            std::vector<pf_equity_point_t> points;
            for (int i = 0; i < 12; ++i)
                points.push_back({first + i * 20 * fixture::day, 100.0 + 7.0 * noise(i), 0.0});
            auto fold = make_fold(name, points);
            for (const auto& point : points)
                CHECK(name, fold->append(point, checked_key(point, "UTC"))
                    == MetricsStreamingStatusV1::Ok);
            MetricsStreamingResultV1 result;
            CHECK(name, fold->finish(result) == MetricsStreamingStatusV1::Ok);
            CHECK(name, result.receipt.route == MetricsRatioRouteV1::LegacyRerun);
            CHECK(name, !monthly_result(name, *fold).complete);
            const auto expected = compute_equity_stats(std::as_const(points).data(), points.size(), kInputs.initial_capital,
                "UTC", kInputs.first_open, kInputs.last_close, 0, kInputs.net_profit);
            same_equity(name, expected, reference::compute_equity_stats(points.data(), points.size(),
                kInputs.initial_capital, "UTC", kInputs.first_open, kInputs.last_close, 0, kInputs.net_profit));
            // An unsupported plan keeps the curve from bar one, even when
            // its eventual actual feed has a perfectly ordinary clock.
            engine_series(name, calendar_points(12), "UTC", kScalarNeeds, false,
                          true, false, &points);
        }
    });
    row("reversed-month-prefix-recovers", [](const std::string& name) {
        auto points = calendar_points(12);
        std::swap(points[8].time_ms, points[9].time_ms);
        auto fold = make_fold(name, points);
        for (const auto& point : points)
            CHECK(name, fold->append(point, checked_key(point, "UTC")) == MetricsStreamingStatusV1::Ok);
        MetricsStreamingResultV1 result;
        CHECK(name, fold->finish(result) == MetricsStreamingStatusV1::Ok);
        CHECK(name, result.receipt.route == MetricsRatioRouteV1::LegacyRerun);
        CHECK(name, !monthly_result(name, *fold).complete);
        // The recovery reference remains the actual legacy input order.
        const auto expected = compute_equity_stats(std::as_const(points).data(), points.size(), kInputs.initial_capital,
            "UTC", kInputs.first_open, kInputs.last_close, 0, kInputs.net_profit);
        same_equity(name, expected, reference::compute_equity_stats(points.data(), points.size(),
            kInputs.initial_capital, "UTC", kInputs.first_open, kInputs.last_close, 0, kInputs.net_profit));
    });
}

void exact_reference_row() {
    row("independent-rational-month-and-bar-references", [](const std::string& name) {
        const auto points = calendar_points(7);
        auto fold = make_fold(name, points);
        for (const auto& point : points)
            CHECK(name, fold->append(point, checked_key(point, "UTC")) == MetricsStreamingStatusV1::Ok);
        MetricsStreamingResultV1 result;
        CHECK(name, fold->finish(result) == MetricsStreamingStatusV1::Ok);
        const auto months = monthly_result(name, *fold);
        const auto bars_reference = fixture::exact_reference(fixture::source(points));
        CHECK(name, fixture::hex_bits(result.sharpe_bar) == bars_reference.at("legacy_sharpe_bits"));
        CHECK(name, fixture::hex_bits(result.sortino_bar) == bars_reference.at("legacy_sortino_bits"));
        // Collect real chart-month ends independently, then feed the exact
        // reference a clock whose density is exactly 12/year. Its rf and
        // annualizer are then precisely the monthly operands.
        std::vector<pf_equity_point_t> ends;
        int last_key = 0;
        for (const auto& point : points) {
            const int key = reference::month_key_utc(point.time_ms);
            if (ends.empty() || key != last_key) ends.push_back(point);
            else ends.back() = point;
            last_key = key;
        }
        for (std::size_t i = 0; i < ends.size(); ++i)
            ends[i].time_ms = fixture::epoch + static_cast<std::int64_t>(i) * (fixture::year / 12);
        const auto span = static_cast<double>(ends.back().time_ms - ends.front().time_ms)
            / static_cast<double>(fixture::year);
        const auto density = static_cast<double>(ends.size() - 1) / span;
        CHECK(name, same(0.02 / density, 0.02 / 12.0));
        CHECK(name, same(std::sqrt(density), std::sqrt(12.0)));
        const auto month_reference = fixture::exact_reference(fixture::source(ends));
        CHECK(name, fixture::hex_bits(months.sharpe_monthly)
            == month_reference.at("legacy_sharpe_bits"));
        CHECK(name, fixture::hex_bits(months.sortino_monthly)
            == month_reference.at("legacy_sortino_bits"));
    });
}

void memory_rows() {
    row("monthly-state-fixed-horizon-vary-bars", [](const std::string& name) {
        std::size_t bytes = 0;
        for (const std::int64_t count : {10'000LL, 100'000LL, 1'000'000LL}) {
            std::vector<pf_equity_point_t> points;
            const auto first = month_start(0), span = month_start(60) - first;
            points.reserve(static_cast<std::size_t>(count));
            for (std::int64_t i = 0; i < count; ++i)
                points.push_back({first + (span - 1) * i / (count - 1),
                    100.0 + 2.0 * std::sin(static_cast<double>(i) * 1e-4) + 0.05 * noise(i), 0.0});
            const auto current = pure_series(name, points);
            if (bytes == 0) bytes = current;
            CHECK(name, current == bytes);
        }
        std::printf("monthly replay resident state, fixed sixty-month horizon: %zu bytes\n", bytes);
    });
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--row" && i + 1 < argc) selected_row = argv[++i];
        else { std::fprintf(stderr, "unknown argument: %s\n", argument.c_str()); return 2; }
    }
    calendar_rows();
    held_rows();
    numeric_rows();
    stop_rows();
    recovery_rows();
    exact_reference_row();
    memory_rows();
    mutation_rows();
    if (rows == 0) {
        std::fprintf(stderr, "no row matched: %s\n", selected_row.c_str());
        return 2;
    }
    std::printf("monthly replay: %d rows, %d checks, %d failures\n", rows, checks, failures);
    return failures == 0 ? 0 : 1;
}
