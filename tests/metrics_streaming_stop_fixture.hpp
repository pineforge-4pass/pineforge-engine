#pragma once

// Test-only streaming-ratio fixtures and FROZEN feae3369 ratio walk. None of the
// expected values below calls the candidate or production metric helpers.
// Large generated rows are walked twice without retaining their curve.
#include <pineforge/metrics_fold.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>
#include "equity_stats_reference.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace metrics_streaming_test {
using namespace pineforge;
using namespace pineforge::metrics;
constexpr std::int64_t year = 31'557'600'000LL;
constexpr std::int64_t day = 86'400'000LL;
constexpr std::int64_t epoch = 1'704'067'200'000LL;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

inline std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
inline bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || bits(a) == bits(b);
}
inline std::string hex_bits(double value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << bits(value);
    return out.str();
}
inline std::string printed(double value, int precision = 17) {
    std::ostringstream out;
    out << std::setprecision(precision) << value;
    return out.str();
}
inline const char* route_name(MetricsRatioRouteV1 value) {
    switch (value) {
    case MetricsRatioRouteV1::Streaming: return "streaming";
    case MetricsRatioRouteV1::LegacyActualCurve: return "legacy_actual_curve";
    case MetricsRatioRouteV1::LegacyRerun: return "legacy_rerun";
    default: return "unavailable";
    }
}
struct PointSource {
    std::int64_t count = 0;
    std::function<pf_equity_point_t(std::int64_t)> point;
    pf_equity_point_t operator[](std::int64_t i) const { return point(i); }
};
inline PointSource source(std::vector<pf_equity_point_t> points) {
    auto storage = std::make_shared<const std::vector<pf_equity_point_t>>(std::move(points));
    return {static_cast<std::int64_t>(storage->size()),
            [storage](std::int64_t i) { return storage->at(static_cast<std::size_t>(i)); }};
}
inline PointSource equities(std::vector<double> values, std::int64_t span = year) {
    std::vector<pf_equity_point_t> points;
    const auto n = static_cast<std::int64_t>(values.size());
    for (std::int64_t i = 0; i < n; ++i)
        points.push_back({epoch + span / std::max<std::int64_t>(1, n - 1) * i,
                          values[static_cast<std::size_t>(i)], 0.0});
    return source(std::move(points));
}
inline MetricsExtentV1 extent(const PointSource& points) {
    MetricsExtentV1 out;
    out.count = points.count;
    out.empty = points.count == 0;
    if (points.count) {
        out.first_time_ms = points[0].time_ms;
        out.last_time_ms = points[points.count - 1].time_ms;
    }
    return out;
}
inline MetricsExtentPlanV1 plan(const PointSource& points) {
    MetricsExtentPlanV1 out;
    out.present = 1;
    out.extent = extent(points);
    out.attempt = 7;
    out.input_generation = 11;
    out.admitted_view = 13;
    out.spec_identity = 17;
    return out;
}
inline MetricsStreamingConfigV1 config() {
    MetricsStreamingConfigV1 out;
    // Inherit the owner's default; only explicit-switch rows override it.
    out.metrics_needed = kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1;
    out.attempt = 7;
    out.input_generation = 11;
    out.admitted_view = 13;
    out.spec_identity = 17;
    out.deterministic_replay = 1;
    return out;
}
inline bool same_extent(const MetricsExtentV1& a, const MetricsExtentV1& b) {
    return a.empty == b.empty && a.count == b.count
        && (a.empty || (a.first_time_ms == b.first_time_ms
                       && a.last_time_ms == b.last_time_ms));
}
struct Trace {
    std::string stage;
    std::int64_t index;
    std::string operation, code, field, reason;
    bool operator==(const Trace& other) const {
        return stage == other.stage && index == other.index
            && operation == other.operation && code == other.code
            && field == other.field && reason == other.reason;
    }
};
inline void collect_trace(void* context, const MetricsRatioTraceV1& value) {
    auto& out = *static_cast<std::vector<Trace>*>(context);
    // Diagnostic storage belongs to the harness, never the fold. The million
    // point rows use no trace callback; the overflow rows keep every operation.
    out.push_back({value.series_stage ? value.series_stage : "", value.return_index,
                   value.operation ? value.operation : "", value.code ? value.code : "",
                   value.field ? value.field : "", value.reason ? value.reason : ""});
}
struct LegacyRatios {
    double sharpe = nan, sortino = nan, mean = nan, var = 0.0, down = 0.0;
    double span = 0.0, rf = nan, ann = nan;
    std::size_t m = 0;
    std::vector<Trace> trace;
    std::string failure_code, failure_field, failure_reason;
    std::int64_t failure_index = -1;
    std::string failure_operation;
};
struct FrozenFailure {};
template<class Generate>
inline LegacyRatios frozen_series(Generate generate, double rf, double ann,
                                  bool enabled, bool trace_enabled = true) {
    LegacyRatios out;
    out.rf = rf;
    out.ann = ann;
    const char* sf = "metrics.equity.sharpe_bar";
    const char* df = "metrics.equity.sortino_bar";
    double sum = 0.0;
    std::int64_t deferred = -1;
    std::string deferred_operation;
    generate([&](double value) {
        if (!std::isfinite(value) && deferred < 0) {
            deferred = static_cast<std::int64_t>(out.m);
            deferred_operation = "return.first_pass";
        }
        if (trace_enabled)
            out.trace.push_back({"bar.first_pass", static_cast<std::int64_t>(out.m),
                                 "return.first_pass", "", sf, ""});
        sum += value;
        if (!std::isfinite(sum) && deferred < 0) {
            deferred = static_cast<std::int64_t>(out.m);
            deferred_operation = "sum";
        }
        if (trace_enabled)
            out.trace.push_back({"bar.first_pass", static_cast<std::int64_t>(out.m),
                                 "sum", "", sf, ""});
        ++out.m;
    });
    if (!enabled || out.m < 2) return out;
    const auto check = [&](double value, std::int64_t index,
                           const std::string& operation, const char* field) {
        const bool bad = !std::isfinite(value);
        if (trace_enabled)
            out.trace.push_back({"bar", index, operation,
                                 bad ? "report_result_invalid" : "", field,
                                 bad ? "nonfinite" : ""});
        if (bad) {
            out.failure_code = "report_result_invalid";
            out.failure_field = field;
            out.failure_reason = "nonfinite";
            out.failure_index = index;
            out.failure_operation = operation;
            throw FrozenFailure{};
        }
        return value;
    };
    try {
        check(rf, -1, "rf", sf);
        check(ann, -1, "annualizer", sf);
        if (deferred >= 0)
            check(nan, deferred, "promote." + deferred_operation, sf);
        check(sum, -1, "sum.final", sf);
        out.mean = check(sum / static_cast<double>(out.m), -1, "mean", sf);
        std::int64_t i = 0;
        generate([&](double value) {
            check(value, i, "return.second_pass", sf);
            const double delta = check(value - out.mean, i, "delta", sf);
            out.var = check(out.var + delta * delta, i, "variance.add_square", sf);
            const double d = std::min(0.0, check(value - rf, i, "downside.subtract", df));
            out.down = check(out.down + d * d, i, "downside.add_square", df);
            ++i;
        });
        const double sd = check(std::sqrt(out.var / static_cast<double>(out.m - 1)),
                                -1, "sample.sd", sf);
        const double dd = check(std::sqrt(out.down / static_cast<double>(out.m)),
                                -1, "downside.sd", df);
        if (sd > 0.0)
            out.sharpe = check((out.mean - rf) / sd * ann, -1, "sharpe", sf);
        if (dd > 0.0)
            out.sortino = check((out.mean - rf) / dd * ann, -1, "sortino", df);
    } catch (const FrozenFailure&) {
        // The oracle records a legacy failure. Candidate errors are never
        // caught here, converted to expected success, skipped or xfailed.
    }
    return out;
}
inline LegacyRatios legacy(const PointSource& points, bool trace_enabled = true) {
    double span = 0.0;
    if (points.count) {
        const auto first = points[0].time_ms, last = points[points.count - 1].time_ms;
        span = last >= first
            ? static_cast<double>(static_cast<std::uint64_t>(last)
                                  - static_cast<std::uint64_t>(first))
            : -static_cast<double>(static_cast<std::uint64_t>(first)
                                   - static_cast<std::uint64_t>(last));
    }
    const double years = span / (365.25 * 86400.0 * 1000.0);
    const bool enabled = points.count >= 3 && years > 0.0;
    const double density = enabled ? static_cast<double>(points.count - 1) / years : nan;
    auto out = frozen_series([&](auto emit) {
        for (std::int64_t i = 1; i < points.count; ++i)
            if (points[i - 1].equity > 0.0)
                emit(points[i].equity / points[i - 1].equity - 1.0);
    }, enabled ? .02 / density : nan, enabled ? std::sqrt(density) : nan,
       enabled, trace_enabled);
    out.span = span;
    return out;
}
// Independently check the stages BEFORE bar ratios for O-RETURN/O-ORDER:
// inputs, final open profit, ordered extrema, span/CAGR/Calmar and monthly
// domain. All points in those rows are in one chart month, initial/open/close
// are 1 and exposure/net profit are 0. The returned boolean is asserted.
inline bool overflow_prefix_is_finite(const PointSource& points) {
    if (!points.count || !std::isfinite(points[points.count - 1].open_profit)) return false;
    double peak = points[0].equity, trough = peak, max_dd = 0., max_ru = 0.;
    double dd_pct = 0.;
    const int month = equity_stats_reference::month_key_utc(points[0].time_ms);
    for (std::int64_t i = 0; i < points.count; ++i) {
        const auto p = points[i];
        if (!std::isfinite(p.equity) || !std::isfinite(p.open_profit)
            || equity_stats_reference::month_key_utc(p.time_ms) != month) return false;
        if (p.equity > peak) peak = trough = p.equity;
        if (p.equity < trough) trough = p.equity;
        const double dd = peak - p.equity, ru = p.equity - trough;
        if (!std::isfinite(dd) || !std::isfinite(ru)) return false;
        if (dd > max_dd) {
            max_dd = dd;
            dd_pct = peak > 0. ? dd / peak * 100. : nan;
            if (peak > 0. && !std::isfinite(dd_pct)) return false;
        }
        if (ru > max_ru) {
            max_ru = ru;
            if (trough > 0. && !std::isfinite(ru / trough * 100.)) return false;
        }
    }
    const double years = static_cast<double>(
        points[points.count - 1].time_ms - points[0].time_ms) / 31557600000.;
    if (!(years > 0.)) return false;
    if (points[points.count - 1].equity > 0.) {
        const double ratio = points[points.count - 1].equity;
        const double compounded = (std::pow(ratio, 1. / years) - 1.) * 100.;
        const double cagr = std::isfinite(compounded)
            ? compounded : (ratio - 1.) * (1. / years) * 100.;
        if (!std::isfinite(cagr)
            || (max_dd > 0. && dd_pct > 0. && !std::isfinite(cagr / dd_pct))) return false;
    }
    return true; // Monthly generator has one bucket and zero eligible returns.
}

inline std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
inline std::string tests_directory() {
    const std::string path = __FILE__;
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}
inline std::map<std::string, std::string> exact_reference(const PointSource& points) {
    char temporary[] = "/tmp/pineforge-streaming-reference-XXXXXX";
    const int fd = mkstemp(temporary);
    if (fd < 0) throw std::runtime_error("cannot create rational-reference input");
    FILE* data = fdopen(fd, "w");
    if (!data) { close(fd); unlink(temporary); throw std::runtime_error("fdopen reference input"); }
    std::fprintf(data, "%lld %lld %lld\n", static_cast<long long>(points.count),
        static_cast<long long>(points.count ? points[0].time_ms : 0),
        static_cast<long long>(points.count ? points[points.count - 1].time_ms : 0));
    for (std::int64_t i = 0; i < points.count; ++i) {
        const auto p = points[i];
        std::fprintf(data, "%lld %a %a\n", static_cast<long long>(p.time_ms),
                     p.equity, p.open_profit);
    }
    const int closed = std::fclose(data);
    if (closed != 0) { unlink(temporary); throw std::runtime_error("reference input write failed"); }
    const std::string command = "python3 "
        + shell_quote(tests_directory() + "/metrics_streaming_exact_reference.py")
        + " --tsv --points-file " + shell_quote(temporary);
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("cannot start exact rational reference");
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe)) output += buffer;
    const int status = pclose(pipe);
    unlink(temporary);
    if (status != 0) throw std::runtime_error("exact rational reference failed");
    std::map<std::string, std::string> result;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) throw std::runtime_error("bad exact-reference record");
        result.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
    if (!result.count("legacy_sharpe_bits") || !result.count("legacy_sortino_bits"))
        throw std::runtime_error("incomplete exact-reference output");
    return result;
}

enum class Injection { None, CompletedMark, IntraBar, MarginCallback, Cooperative };
struct StopSpec {
    int stop_marks = 0;
    Injection injection = Injection::None;
    bool margin = false;
    bool regular = false;
};
inline std::vector<Bar> stop_feed(bool regular = false) {
    std::vector<Bar> bars;
    for (int i = 0; i < 12; ++i) {
        const double price = 100. + (i % 2 ? 3. * i : -2. * i);
        const auto time = epoch + 60000LL * (regular ? i : i * (i + 1));
        bars.push_back({price, price + 1., price - 1., price, 1., time});
    }
    return bars;
}
inline NativeRunSpec native_spec() {
    NativeRunSpec s;
    s.identity = {"streaming-completed-mark", 1};
    s.input_tf = s.script_tf = "1";
    s.tickerid = "TEST:STREAMING";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 100000.;
    s.point_value = s.account_fx = 1.;
    s.price_tick = .01;
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.report_policy = NativeReportPolicy::KernelRecordedAtHostMarks;
    s.report_open_position_at_end = true;
    return s;
}
struct RunResult {
    std::vector<pf_equity_point_t> curve;
    std::vector<TradeC> trades;
    pf_metrics_t metrics{};
    std::vector<std::int64_t> report_counts;
    std::vector<SecurityDiagC> security;
    std::vector<TraceEntryC> trace;
    std::vector<std::string> trace_names;
    std::vector<std::uint64_t> hashes;
    std::int64_t inputs = 0, scripts = 0;
    int begun = 0, callbacks = 0, margins = 0, status = 0;
    double net_profit = 0.;
    std::string code, args, message;
    NativeLifecycleKind lifecycle = NativeLifecycleKind::Unconfigured;
    MetricsStopBoundaryV1 boundary;
    PointSource points() const { return source(curve); }
};
inline RunResult capture(NativeStrategyHost& host) {
    RunResult out;
    const auto failure = run_failure_value_of(host);
    out.code = run_failure_code_name(failure.code);
    out.args = failure.args ? *failure.args : "{}";
    out.status = host.last_run_status();
    out.message = host.last_error();
    out.lifecycle = host.native_state().kind;
    ReportC report{};
    host.fill_report(&report);
    out.metrics = report.metrics;
    out.report_counts = {
        report.total_trades, report.trades_len, report.equity_curve_len,
        report.input_bars_processed, report.script_bars_processed,
        report.security_feeds_total, report.security_eval_complete_total,
        report.security_eval_partial_total, report.magnifier_sub_bars_total,
        report.magnifier_sample_ticks_total, report.input_tf_seconds,
        report.script_tf_seconds, report.script_tf_ratio, report.needs_aggregation,
        report.bar_magnifier_enabled, report.security_diag_len, report.trace_len,
        report.trace_names_len, report.broker_state_hash_len,
    };
    if (report.security_diag_len)
        out.security.assign(report.security_diag, report.security_diag + report.security_diag_len);
    if (report.trace_len) out.trace.assign(report.trace, report.trace + report.trace_len);
    for (int i = 0; i < report.trace_names_len; ++i)
        out.trace_names.emplace_back(report.trace_names[i] ? report.trace_names[i] : "");
    if (report.broker_state_hash_len)
        out.hashes.assign(report.broker_state_hash,
                          report.broker_state_hash + report.broker_state_hash_len);
    out.inputs = report.input_bars_processed;
    out.scripts = report.script_bars_processed;
    out.net_profit = report.net_profit;
    if (report.equity_curve_len)
        out.curve.assign(report.equity_curve, report.equity_curve + report.equity_curve_len);
    if (report.trades_len)
        out.trades.assign(report.trades, report.trades + report.trades_len);
    BacktestEngine::free_report(&report);
    return out;
}
inline bool same_run(const RunResult& a, const RunResult& b) {
    if (a.code != b.code || a.args != b.args || a.message != b.message
        || a.status != b.status || a.lifecycle != b.lifecycle
        || a.inputs != b.inputs || a.scripts != b.scripts
        || a.report_counts != b.report_counts || a.hashes != b.hashes
        || a.trace_names != b.trace_names || a.trace.size() != b.trace.size()
        || a.security.size() != b.security.size()
        || !same(a.net_profit, b.net_profit)
        || a.curve.size() != b.curve.size() || a.trades.size() != b.trades.size()
        || std::memcmp(&a.metrics, &b.metrics, sizeof a.metrics) != 0) return false;
    for (std::size_t i = 0; i < a.security.size(); ++i) {
        const auto& x = a.security[i]; const auto& y = b.security[i];
        if (x.sec_id != y.sec_id || x.feed_count != y.feed_count
            || x.eval_complete_count != y.eval_complete_count
            || x.eval_partial_count != y.eval_partial_count) return false;
    }
    for (std::size_t i = 0; i < a.trace.size(); ++i) {
        const auto& x = a.trace[i]; const auto& y = b.trace[i];
        if (x.timestamp != y.timestamp || x.bar_index != y.bar_index
            || x.name_id != y.name_id || !same(x.value, y.value)) return false;
    }
    for (std::size_t i = 0; i < a.curve.size(); ++i) {
        const auto& x = a.curve[i]; const auto& y = b.curve[i];
        if (x.time_ms != y.time_ms || !same(x.equity, y.equity)
            || !same(x.open_profit, y.open_profit)) return false;
    }
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        const auto& x = a.trades[i]; const auto& y = b.trades[i];
        if (x.entry_time != y.entry_time || x.exit_time != y.exit_time
            || x.entry_bar_index != y.entry_bar_index || x.exit_bar_index != y.exit_bar_index
            || x.is_long != y.is_long || x.open_at_end != y.open_at_end
            || !same(x.entry_price, y.entry_price) || !same(x.exit_price, y.exit_price)
            || !same(x.pnl, y.pnl) || !same(x.pnl_pct, y.pnl_pct)
            || !same(x.qty, y.qty) || !same(x.commission, y.commission)
            || !same(x.max_runup, y.max_runup) || !same(x.max_drawdown, y.max_drawdown))
            return false;
    }
    return true;
}
class CompletedMarkHost final : public NativeStrategyHost {
public:
    explicit CompletedMarkHost(StopSpec spec) : stop(spec) {}
    StopSpec stop;
    int marks = 0, callbacks = 0, margins = 0, begun = 0, input = -1;
    MetricsStopBoundaryV1 boundary;
    std::size_t retained_capacity() const { return equity_curve_.capacity(); }
    void on_native_run_begin() override {
        ++begun;
        marks = callbacks = margins = 0;
        input = -1;
        boundary = {};
    }
    void on_native_input(const Bar&, const NativeInputContext& context) override {
        input = context.input_index;
    }
    [[noreturn]] void inject(MetricsStopBoundaryKindV1 kind) {
        boundary = {marks, input, kind, 0};
        throw std::runtime_error("streaming-ratio injected host stop");
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++callbacks;
        if (stop.injection == Injection::IntraBar && callbacks == stop.stop_marks)
            inject(MetricsStopBoundaryKindV1::IntraBar);
        if (callbacks == 1)
            (void)submit({native_order::Transact{100.}, "streaming-entry", ""});
        if (!mark_native_report_point(bar.timestamp))
            throw std::logic_error("completed-mark fixture could not publish a mark");
        ++marks;
        if (stop.injection == Injection::Cooperative && marks == stop.stop_marks) {
            boundary = {marks, input, MetricsStopBoundaryKindV1::CompletedMark, 0};
            request_abort();
        }
        if (stop.injection == Injection::CompletedMark && marks == stop.stop_marks) {
            if (stop.margin && margins == 0)
                throw std::logic_error("host stop preceded its real margin receipt");
            inject(MetricsStopBoundaryKindV1::CompletedMark);
        }
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        if (stop.margin && input == stop.stop_marks - 1 && margins == 0
            && view.position.signed_units > 0.)
            return NativeMarginDecision{view.required, view.equity, true};
        return std::nullopt;
    }
    std::optional<double> resolve_margin_call_units(const NativeMarginCallView& view) const override {
        return std::abs(view.position.signed_units); // Real kernel flatten.
    }
    void on_native_margin_call(const native_order::MarginCallEvent&) override {
        ++margins;
        if (stop.injection == Injection::MarginCallback)
            inject(MetricsStopBoundaryKindV1::MarginCallback);
    }
protected:
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        BacktestEngine::hash_host_extension(sink);
        sink.s("streaming-completed-mark/v1");
        sink.i(stop.stop_marks); sink.i(static_cast<int>(stop.injection));
        sink.i(stop.margin); sink.i(stop.regular);
        sink.i(marks); sink.i(callbacks); sink.i(margins); sink.i(begun); sink.i(input);
        sink.i(boundary.completed_marks); sink.i(boundary.input_index);
        sink.i(static_cast<int>(boundary.kind));
    }
};
inline RunResult run_stop(StopSpec stop) {
    CompletedMarkHost host(stop);
    auto spec = native_spec();
    if (stop.margin) {
        NativeMarginModel margin;
        margin.initial_long = margin.initial_short = .01;
        margin.maintenance_long = margin.maintenance_short = .01;
        margin.sizing = NativeLiquidationSizing::Flatten;
        // liquidation_level() is negative for this well-funded long
        // (src/native_execution_consumer.cpp:3993-3996). The default check
        // returns before the host requirement hook (:4409-4413); the mark
        // check reaches that hook and rests at the adverse waypoint (:4428).
        margin.check = NativeLiquidationCheck::PathAdverseExtremeMark;
        spec.margin = margin;
    }
    if (host.configure_native(spec).status != NativeSetupStatus::Applied)
        throw std::logic_error("invalid completed-mark fixture spec");
    const auto bars = stop_feed(stop.regular);
    host.run(bars.data(), static_cast<int>(bars.size()));
    auto out = capture(host);
    out.begun = host.begun; out.callbacks = host.callbacks; out.margins = host.margins;
    out.boundary = host.boundary;
    return out;
}

struct ReplayRun {
    MetricsStreamingStatusV1 status = MetricsStreamingStatusV1::NotImplemented;
    MetricsExtentReceiptV1 receipt;
    RunResult authoritative;
    int executions = 0, publications = 0;
    std::size_t first_curve_capacity = 0;
};
// An actual deterministic host adapter, not a precomputed-prefix replay.
// Its curve run repeats the original injection (including real liquidation),
// verifies the complete partial result, then publishes that result once.
inline ReplayRun run_replay_adapter(StopSpec stop, const MetricsExtentPlanV1& planned,
                                   bool replay_proved = true) {
    ReplayRun out;
    CompletedMarkHost attempt(stop);
    auto spec = native_spec();
    if (stop.margin) {
        NativeMarginModel margin;
        margin.initial_long = margin.initial_short = .01;
        margin.maintenance_long = margin.maintenance_short = .01;
        margin.sizing = NativeLiquidationSizing::Flatten;
        // Repeat run_stop's real mark-based liquidation, including its
        // receipt before the completed-mark stop (consumer :4409-4428).
        margin.check = NativeLiquidationCheck::PathAdverseExtremeMark;
        spec.margin = margin;
    }
    if (attempt.configure_native(spec).status != NativeSetupStatus::Applied)
        throw std::logic_error("invalid replay adapter fixture");
    out.status = attempt.set_metrics_streaming_v1(
        StreamingSharpeV1::On, kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1);
    if (out.status != MetricsStreamingStatusV1::Ok) return out;
    // The fixture supplies a trusted, finite, one-mark-per-input declaration.
    // Actual arbitrary host modules cannot infer this certificate from a base
    // class. Their absence/override rows retain today's curve route.
    MetricsPlainDeclarationV1 declaration;
    declaration.builtin_presenter = declaration.builtin_marks = declaration.builtin_scheduler = 1;
    MetricsPlainInputsV1 inputs;
    inputs.declaration = &declaration;
    inputs.module_contract_bound = inputs.historical_batch = inputs.equal_resolved_timeframes = 1;
    inputs.intrabar_none = inputs.builtin_host_marks = inputs.feed_tolerant_passthrough = 1;
    inputs.final_admitted_view_bound = 1;
    out.status = attempt.admit_metrics_plain_v1(inputs, planned, &out.receipt);
    if (out.status != MetricsStreamingStatusV1::Ok) return out;
    auto cfg = config();
    cfg.curve_available = replay_proved ? 0u : 1u;
    cfg.deterministic_replay = replay_proved ? 1u : 0u;
    out.status = attempt.prepare_metrics_streaming_v1(cfg, planned);
    if (out.status != MetricsStreamingStatusV1::Ok) return out;
    const auto bars = stop_feed(stop.regular);
    ++out.executions;
    attempt.run(bars.data(), static_cast<int>(bars.size()));
    out.first_curve_capacity = attempt.retained_capacity();
    const auto original_failure = run_failure_value_of(attempt);
    const std::string original_code = run_failure_code_name(original_failure.code);
    const std::string original_args = original_failure.args ? *original_failure.args : "{}";
    const auto original_status = attempt.last_run_status();
    out.status = attempt.metrics_extent_receipt_v1(&out.receipt);
    if (out.status != MetricsStreamingStatusV1::Ok) return out;
    if (out.receipt.route == MetricsRatioRouteV1::LegacyRerun) {
        ++out.executions;
        auto rerun = run_stop(stop);
        if (original_code != rerun.code || original_args != rerun.args
            || original_status != rerun.status
            || attempt.boundary.completed_marks != rerun.boundary.completed_marks
            || attempt.boundary.input_index != rerun.boundary.input_index)
            throw std::logic_error("streaming-ratio deterministic replay changed the original result/boundary");
        out.authoritative = std::move(rerun);
    } else {
        out.authoritative = capture(attempt);
    }
    ++out.publications;
    return out;
}

// Records release/amend events independently of candidate arithmetic.
// This is the invariant PLAIN relies on: after append(i+1), amend(i) is
// impossible. It is NOT "the curve contains only one logical point".
struct HeldEvent {
    bool amendment;
    std::int64_t index;
    pf_equity_point_t point;
};
struct HeldScript {
    std::vector<HeldEvent> events;
    std::vector<pf_equity_point_t> final;
    void append(pf_equity_point_t point) {
        events.push_back({false, static_cast<std::int64_t>(final.size()), point});
        final.push_back(point);
    }
    void amend(std::int64_t index, pf_equity_point_t point) {
        assert(index >= 0 && index + 1 == static_cast<std::int64_t>(final.size())
               && "a held point is never amended after a second point exists");
        events.push_back({true, index, point});
        final.at(static_cast<std::size_t>(index)) = point;
    }
};
inline PointSource append_time_mutant(const HeldScript& script) {
    // Deliberately wrong fold input R: amendments cannot undo a committed
    // return. Both mutation rows feed this through the frozen reference and
    // REQUIRE disagreement with F; --mutant=append routes the candidate to R.
    std::vector<pf_equity_point_t> points;
    for (const auto& event : script.events)
        if (!event.amendment) points.push_back(event.point);
    return source(std::move(points));
}
} // namespace metrics_streaming_test
