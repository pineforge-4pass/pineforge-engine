#include <pineforge/pineforge.h>
#include <pineforge/native_module.hpp>
#include <pineforge/selected_window.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace module_metrics_test {
using namespace pineforge;
using namespace pineforge::metrics;

// Identical trading and mark implementations; only CertifiedHost has the
// exact-type specialization below. Neither host changes the presenter.
class TradingHost : public NativeStrategyHost {
public:
    int callbacks = 0, marks = 0, input_index = -1;
    int stop_after = 0;
    bool stop_before_mark = false;

    void on_native_run_begin() override {
        callbacks = marks = 0;
        input_index = -1;
    }
    void on_native_input(const Bar&, const NativeInputContext& context) override {
        input_index = context.input_index;
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++callbacks;
        if (stop_before_mark && callbacks == stop_after)
            throw std::runtime_error("module metrics injected stop");
        switch ((callbacks - 1) % 12) {
        case 0: (void)submit({native_order::Transact{100.}, "long", ""}); break;
        case 4: (void)submit({native_order::Transact{-100.}, "close-long", ""}); break;
        case 5: (void)submit({native_order::Transact{-60.}, "short", ""}); break;
        case 9: (void)submit({native_order::Transact{60.}, "close-short", ""}); break;
        case 10: (void)submit({native_order::Transact{25.}, "terminal-long", ""}); break;
        case 11: (void)submit({native_order::Transact{-25.}, "close-terminal", ""}); break;
        default: break;
        }
        if (!mark_native_report_point(bar.timestamp))
            throw std::logic_error("module metrics mark refused");
        ++marks;
        if (!stop_before_mark && marks == stop_after)
            throw std::runtime_error("module metrics injected stop");
    }
protected:
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        BacktestEngine::hash_host_extension(sink);
        sink.s("module-metrics-fixture/v1");
        sink.i(callbacks); sink.i(marks); sink.i(input_index);
        sink.i(stop_after); sink.i(stop_before_mark);
    }
};
class CertifiedHost : public TradingHost {};
class UncertifiedHost : public TradingHost {};
class DerivedCertifiedHost : public CertifiedHost {};
} // namespace module_metrics_test

namespace pineforge::native_module {
template <>
struct MetricsPlainHostV1<module_metrics_test::CertifiedHost> {
    // Harness controls concern the declaration reader, never trading state.
    static inline int calls = 0;
    static inline bool throw_on_read = false;
    static inline bool unknown_version = false;
    static std::optional<MetricsPlainDeclarationV1> declaration() {
        ++calls;
        if (throw_on_read) throw std::runtime_error("module declaration unavailable");
        MetricsPlainDeclarationV1 out;
        out.builtin_presenter = out.builtin_marks = out.builtin_scheduler = 1;
        if (unknown_version) ++out.version;
        return out;
    }
};
} // namespace pineforge::native_module

// Compile the production macro twice into this test, with distinct C names for
// the second module. No test-only implementation of run or capability exists.
PINEFORGE_EXPORT_NATIVE_STRATEGY(module_metrics_test::CertifiedHost);
// The macro defines fixed C names, so a second expansion in one binary would collide: the
// uncertified module is written out with the SAME helpers the macro calls (one line per export,
// same bodies), which keeps every call a production entry of native_module.
namespace module_metrics_test {
inline pf_strategy_t uncertified_strategy_create(const char*) {
    return ::pineforge::native_module::create<UncertifiedHost>();
}
inline void uncertified_strategy_free(pf_strategy_t s) { ::pineforge::native_module::destroy(s); }
inline void uncertified_strategy_set_input(pf_strategy_t s, const char* key, const char* value) {
    ::pineforge::native_module::set_input<UncertifiedHost>(s, key, value);
}
inline void uncertified_strategy_set_override(pf_strategy_t s, const char* key,
                                              const char* value) {
    ::pineforge::native_module::set_override<UncertifiedHost>(s, key, value);
}
inline void uncertified_strategy_set_magnifier_volume_weighted(pf_strategy_t s, int on) {
    ::pineforge::native_module::set_magnifier_volume_weighted<UncertifiedHost>(s, on);
}
inline std::int32_t uncertified_strategy_metrics_only_capability_v1(void) noexcept {
    return ::pineforge::native_module::metrics_only_capability_v1<UncertifiedHost>();
}
inline void uncertified_run_backtest(pf_strategy_t s, pf_bar_t* bars, int n, pf_report_t* out) {
    ::pineforge::native_module::run_backtest<UncertifiedHost>(s, bars, n, out);
}
inline void uncertified_run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n,
                                          const char* input_tf, const char* script_tf,
                                          int bar_magnifier, int magnifier_samples,
                                          pf_magnifier_distribution_t magnifier_dist,
                                          pf_report_t* out) {
    ::pineforge::native_module::run_backtest_full<UncertifiedHost>(
        s, bars, n, input_tf, script_tf, bar_magnifier, magnifier_samples, magnifier_dist, out);
}
inline void uncertified_report_free(pf_report_t* report) {
    ::pineforge::native_module::report_free(report);
}
}  // namespace module_metrics_test

namespace module_metrics_test {
using Certification = native_module::MetricsPlainHostV1<CertifiedHost>;
constexpr MetricsNeedMaskV1 scalar_need = kMetricsNeedAllV1
    & ~(kMetricsNeedReturnsV1 | kMetricsNeedCurveV1);
static_assert(scalar_need == 0x37, "scalar-only request");
static_assert(std::is_same<decltype(&strategy_metrics_only_capability_v1),
    std::int32_t (*)(void) noexcept>::value, "capability ABI");
static_assert(std::is_same<decltype(&uncertified_strategy_metrics_only_capability_v1),
    std::int32_t (*)(void) noexcept>::value, "uncertified capability ABI");

int checks = 0, failures = 0, rows = 0;
std::string current_row, filter;
bool list_only = false;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", current_row.c_str(), \
                 __FILE__, __LINE__, #expr); } } while (0)

bool row(const char* name) {
    if (list_only) { std::puts(name); return false; }
    if (!filter.empty() && std::string(name).find(filter) == std::string::npos)
        return false;
    current_row = name;
    ++rows;
    return true;
}
template <typename T>
bool same_word(const T& a, const T& b) {
    static_assert(std::is_arithmetic<T>::value, "compare scalar result words");
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

NativeRunSpec run_spec() {
    NativeRunSpec spec;
    spec.identity = {"module-metrics", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:METRICS";
    spec.timezone = spec.chart_timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 100000.;
    spec.point_value = spec.account_fx = 1.;
    spec.price_tick = .01;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = .07;
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.report_policy = NativeReportPolicy::KernelRecordedAtHostMarks;
    spec.report_open_position_at_end = true;
    return spec;
}
std::vector<pf_bar_t> multi_month_bars() {
    constexpr std::int64_t epoch = 1704067200000LL; // 2024-01-01 UTC
    constexpr std::int64_t day = 86400000;
    const int month_day[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
    std::vector<pf_bar_t> bars;
    for (int month = 0; month < 12; ++month) {
        for (int j = 0; j < 4; ++j) {
            const int i = month * 4 + j;
            const double price = 100. + 1.25 * month
                + (i % 3 == 0 ? -4.5 : (i % 3 == 1 ? 7.25 : -1.75));
            bars.push_back({price, price + 2., price - 3., price + .5,
                10. + i, epoch + (month_day[month] + 1 + j * 7) * day + i * 60000LL});
        }
    }
    return bars;
}

struct ModuleApi {
    pf_strategy_t (*create)(const char*);
    void (*destroy)(pf_strategy_t);
    void (*run)(pf_strategy_t, pf_bar_t*, int, pf_report_t*);
    void (*run_full)(pf_strategy_t, pf_bar_t*, int, const char*, const char*,
                     int, int, pf_magnifier_distribution_t, pf_report_t*);
    void (*free_report)(pf_report_t*);
    std::int32_t (*capability)(void) noexcept;
};
const ModuleApi certified_api{strategy_create, strategy_free, run_backtest,
    run_backtest_full, report_free, strategy_metrics_only_capability_v1};
const ModuleApi uncertified_api{uncertified_strategy_create, uncertified_strategy_free,
    uncertified_run_backtest, uncertified_run_backtest_full, uncertified_report_free,
    uncertified_strategy_metrics_only_capability_v1};

struct Instance {
    const ModuleApi& api;
    pf_strategy_t strategy;
    pf_report_t report{};
    explicit Instance(const ModuleApi& module) : api(module), strategy(api.create(nullptr)) {
        CHECK(strategy != nullptr);
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    ~Instance() {
        api.free_report(&report);
        api.destroy(strategy);
    }
    NativeStrategyHost* host() const {
        return static_cast<NativeStrategyHost*>(static_cast<BacktestEngine*>(strategy));
    }
    TradingHost* trading() const { return static_cast<TradingHost*>(host()); }
    void configure(const NativeRunSpec& spec = run_spec()) {
        CHECK(strategy != nullptr);
        if (strategy)
            CHECK(host()->configure_native(spec).status == NativeSetupStatus::Applied);
    }
    void preference(StreamingSharpeV1 mode, MetricsNeedMaskV1 need = scalar_need) {
        CHECK(strategy_set_metrics_streaming_v1(strategy,
            static_cast<std::uint32_t>(mode), need)
            == static_cast<std::int32_t>(MetricsStreamingStatusV1::Ok));
    }
    void run(std::vector<pf_bar_t>& bars, bool full = true) {
        if (full)
            api.run_full(strategy, bars.data(), static_cast<int>(bars.size()), "1", "1",
                         0, 4, PF_MAGNIFIER_ENDPOINTS, &report);
        else
            api.run(strategy, bars.data(), static_cast<int>(bars.size()), &report);
    }
    MetricsExtentReceiptV1 receipt() const {
        MetricsExtentReceiptV1 out;
        CHECK(strategy_metrics_extent_receipt_v1(strategy, &out)
            == static_cast<std::int32_t>(MetricsStreamingStatusV1::Ok));
        return out;
    }
    void successful() const {
        CHECK(host()->last_error().empty());
        CHECK(host()->last_run_status() == 0);
        CHECK(host()->native_state().kind == NativeLifecycleKind::Completed);
    }
};

void compare_trade_stats(const pf_trade_stats_t& a, const pf_trade_stats_t& b) {
#define WORD(field) CHECK(same_word(a.field, b.field))
    WORD(num_trades); WORD(num_wins); WORD(num_losses); WORD(num_even);
    WORD(percent_profitable); WORD(net_profit); WORD(net_profit_pct);
    WORD(gross_profit); WORD(gross_profit_pct); WORD(gross_loss); WORD(gross_loss_pct);
    WORD(profit_factor); WORD(avg_trade); WORD(avg_trade_pct); WORD(avg_win); WORD(avg_win_pct);
    WORD(avg_loss); WORD(avg_loss_pct); WORD(ratio_avg_win_avg_loss);
    WORD(largest_win); WORD(largest_win_pct); WORD(largest_loss); WORD(largest_loss_pct);
    WORD(commission_paid); WORD(expectancy);
    WORD(max_consecutive_wins); WORD(max_consecutive_losses);
    WORD(avg_bars_in_trade); WORD(avg_bars_in_wins); WORD(avg_bars_in_losses);
#undef WORD
}
void compare_equity(const pf_equity_stats_t& a, const pf_equity_stats_t& b) {
#define WORD(field) CHECK(same_word(a.field, b.field))
    WORD(max_equity_drawdown); WORD(max_equity_drawdown_pct);
    WORD(max_equity_runup); WORD(max_equity_runup_pct);
    WORD(buy_hold_return); WORD(buy_hold_return_pct);
    WORD(sharpe_monthly); WORD(sortino_monthly); WORD(sharpe_bar); WORD(sortino_bar);
    WORD(cagr); WORD(calmar); WORD(recovery_factor); WORD(time_in_market_pct); WORD(open_pl);
#undef WORD
}
void compare_report_values(const pf_report_t& a, const pf_report_t& b) {
    compare_trade_stats(a.metrics.all, b.metrics.all);
    compare_trade_stats(a.metrics.longs, b.metrics.longs);
    compare_trade_stats(a.metrics.shorts, b.metrics.shorts);
    compare_equity(a.metrics.equity, b.metrics.equity);
#define WORD(field) CHECK(same_word(a.field, b.field))
    WORD(net_profit); WORD(total_trades); WORD(trades_len);
    WORD(input_bars_processed); WORD(script_bars_processed);
    WORD(security_feeds_total); WORD(security_complete_total); WORD(security_partial_total);
    WORD(magnifier_sub_bars_total); WORD(magnifier_sample_ticks_total);
    WORD(input_tf_seconds); WORD(script_tf_seconds); WORD(script_tf_ratio);
    WORD(needs_aggregation); WORD(bar_magnifier_enabled);
    WORD(broker_state_hash_len);
#undef WORD
    for (int i = 0; i < std::min(a.trades_len, b.trades_len); ++i) {
        const auto& x = a.trades[i];
        const auto& y = b.trades[i];
#define WORD(field) CHECK(same_word(x.field, y.field))
        WORD(entry_time); WORD(exit_time); WORD(entry_price); WORD(exit_price);
        WORD(pnl); WORD(pnl_pct); WORD(is_long); WORD(max_runup); WORD(max_drawdown);
        WORD(qty); WORD(commission); WORD(entry_bar_index); WORD(exit_bar_index); WORD(open_at_end);
#undef WORD
    }
    for (std::int64_t i = 0; i < std::min(a.broker_state_hash_len, b.broker_state_hash_len); ++i)
        CHECK(a.broker_state_hash[i] == b.broker_state_hash[i]);
}
void compare_curves(const pf_report_t& a, const pf_report_t& b) {
    CHECK(a.equity_curve_len == b.equity_curve_len);
    for (std::int64_t i = 0; i < std::min(a.equity_curve_len, b.equity_curve_len); ++i) {
        const auto& x = a.equity_curve[i];
        const auto& y = b.equity_curve[i];
        CHECK(x.time_ms == y.time_ms);
        CHECK(same_word(x.equity, y.equity));
        CHECK(same_word(x.open_profit, y.open_profit));
    }
}
void compare_failure(const Instance& a, const Instance& b) {
    CHECK(a.host()->last_error() == b.host()->last_error());
    CHECK(a.host()->last_run_status() == b.host()->last_run_status());
    const auto x = run_failure_value_of(*a.host());
    const auto y = run_failure_value_of(*b.host());
    CHECK(x.code == y.code);
    CHECK((x.args ? *x.args : "{}") == (y.args ? *y.args : "{}"));
    CHECK(a.host()->native_state().failure.code == b.host()->native_state().failure.code);
}

void capability_row() {
    if (!row("module-capability-status-and-exact-host")) return;
    CHECK(certified_api.capability() == static_cast<std::int32_t>(MetricsStreamingStatusV1::Ok));
    CHECK(uncertified_api.capability()
        == static_cast<std::int32_t>(MetricsStreamingStatusV1::NotImplemented));
    CHECK(native_module::metrics_only_capability_v1<DerivedCertifiedHost>()
        == static_cast<std::int32_t>(MetricsStreamingStatusV1::NotImplemented));
}
void certified_rows() {
    for (bool full : {true, false}) {
        if (!row(full ? "certified-on-multi-month-full" : "certified-on-multi-month-batch"))
            continue;
        auto bars = multi_month_bars();
        Instance old(certified_api), next(certified_api);
        old.configure(); next.configure();
        old.preference(StreamingSharpeV1::Off);
        next.preference(StreamingSharpeV1::On);
        old.host()->set_broker_state_hash_recording(true);
        next.host()->set_broker_state_hash_recording(true);
        old.run(bars, full); next.run(bars, full);
        old.successful(); next.successful();
        const auto control = old.receipt(), actual = next.receipt();
        CHECK(control.route == MetricsRatioRouteV1::LegacyActualCurve);
        CHECK(actual.route == MetricsRatioRouteV1::Streaming);
        CHECK(actual.route_reason == MetricsRouteReasonV1::OrderedReplay);
        CHECK(actual.plain_subset == 1 && actual.failed_plain_inputs == 0);
        CHECK(actual.witness == MetricsExtentWitnessV1::Match && actual.mismatch_fields == 0);
        CHECK(actual.actual_extent.count == static_cast<std::int64_t>(bars.size()));
        CHECK(actual.actual_extent.first_time_ms == bars.front().timestamp);
        CHECK(actual.actual_extent.last_time_ms == bars.back().timestamp);
        CHECK(actual.extent_plan.attempt != 0);
        const auto state = next.host()->native_state();
        CHECK(state.spec != nullptr);
        if (state.spec)
            CHECK(actual.extent_plan.spec_identity == native_run_spec_digest(*state.spec));
        CHECK(actual.metrics_needed == scalar_need);
        CHECK(next.report.equity_curve == nullptr && next.report.equity_curve_len == 0);
        CHECK(old.report.equity_curve_len == static_cast<std::int64_t>(bars.size()));
        CHECK(old.report.metrics.longs.num_trades > 0 && old.report.metrics.shorts.num_trades > 0);
        CHECK(std::isfinite(old.report.metrics.equity.sharpe_monthly));
        CHECK(std::isfinite(old.report.metrics.equity.sortino_monthly));
        CHECK(std::isfinite(old.report.metrics.equity.sharpe_bar));
        CHECK(std::isfinite(old.report.metrics.equity.sortino_bar));
        compare_report_values(next.report, old.report);
        CHECK(next.host()->broker_state_hash() == old.host()->broker_state_hash());
        // A repeated read must retain the same metric words and not publish a curve.
        pf_report_t reread{};
        native_module::publish_report(next.host(), &reread);
        compare_report_values(reread, old.report);
        CHECK(reread.equity_curve_len == 0);
        report_free(&reread);
    }
}
void uncertified_row() {
    if (!row("uncertified-on-keeps-actual-curve")) return;
    auto bars = multi_month_bars();
    Instance old(uncertified_api), next(uncertified_api);
    old.configure(); next.configure();
    old.preference(StreamingSharpeV1::Off); next.preference(StreamingSharpeV1::On);
    old.run(bars); next.run(bars);
    old.successful(); next.successful();
    const auto receipt = next.receipt();
    CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
    CHECK(receipt.plain_subset == 0 && receipt.extent_plan.present == 0);
    CHECK(next.report.equity_curve_len == static_cast<std::int64_t>(bars.size()));
    compare_report_values(next.report, old.report);
    compare_curves(next.report, old.report);
}
void off_and_default_rows() {
    if (row("certified-off-does-not-admit")) {
        auto bars = multi_month_bars();
        Instance old(certified_api), next(certified_api);
        old.configure(); next.configure();
        old.preference(StreamingSharpeV1::Off, kMetricsNeedAllV1);
        next.preference(StreamingSharpeV1::Off);
        Certification::calls = 0;
        old.run(bars); next.run(bars);
        CHECK(Certification::calls == 0);
        old.successful(); next.successful();
        const auto receipt = next.receipt();
        CHECK(receipt.streaming_sharpe == StreamingSharpeV1::Off);
        CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
        CHECK(receipt.plain_subset == 0 && receipt.extent_plan.present == 0);
        compare_report_values(next.report, old.report);
        compare_curves(next.report, old.report);
    }
    if (row("certified-default-all-keeps-actual-curve")) {
        auto bars = multi_month_bars();
        Instance old(certified_api), next(certified_api);
        old.configure(); next.configure();
        old.preference(StreamingSharpeV1::Off, kMetricsNeedAllV1);
        Certification::calls = 0;
        old.run(bars); next.run(bars);
        CHECK(Certification::calls == 0);
        old.successful(); next.successful();
        CHECK(next.receipt().metrics_needed == kMetricsNeedAllV1);
        CHECK(next.receipt().route == MetricsRatioRouteV1::LegacyActualCurve);
        CHECK(next.receipt().extent_plan.present == 0);
        compare_report_values(next.report, old.report);
        compare_curves(next.report, old.report);
    }
}
void exclusions() {
    for (bool selected : {true, false}) {
        if (!row(selected ? "certified-selected-window-is-not-plain"
                          : "certified-intrabar-magnifier-is-not-plain"))
            continue;
        auto bars = multi_month_bars();
        auto spec = run_spec();
        if (!selected) spec.intrabar.value = IntrabarPath::synthesized{};
        Instance old(certified_api), next(certified_api);
        old.configure(spec); next.configure(spec);
        if (selected) {
            const pf_selected_window_config_v1 window{
                sizeof(pf_selected_window_config_v1), 1,
                bars.front().timestamp, bars.back().timestamp + 60000};
            CHECK(strategy_set_selected_window_v1(old.strategy, &window) == 0);
            CHECK(strategy_set_selected_window_v1(next.strategy, &window) == 0);
        }
        old.preference(StreamingSharpeV1::Off); next.preference(StreamingSharpeV1::On);
        old.run(bars); next.run(bars);
        old.successful(); next.successful();
        const auto receipt = next.receipt();
        CHECK(receipt.route == MetricsRatioRouteV1::LegacyActualCurve);
        CHECK(receipt.plain_subset == 0 && receipt.failed_plain_inputs != 0);
        CHECK(next.report.equity_curve_len > 0);
        if (selected)
            CHECK(old.report.equity_curve_len == static_cast<std::int64_t>(bars.size()) + 1);
        else
            CHECK(next.report.bar_magnifier_enabled == 1);
        compare_report_values(next.report, old.report);
        compare_curves(next.report, old.report);
    }
}
void unavailable_declaration_rows() {
    for (bool throwing : {true, false}) {
        if (!row(throwing ? "throwing-certification-keeps-actual-curve"
                          : "unknown-declaration-keeps-actual-curve"))
            continue;
        auto bars = multi_month_bars();
        Instance old(certified_api), next(certified_api);
        old.configure(); next.configure();
        old.preference(StreamingSharpeV1::Off); next.preference(StreamingSharpeV1::On);
        Certification::throw_on_read = throwing;
        Certification::unknown_version = !throwing;
        CHECK(certified_api.capability()
            == static_cast<std::int32_t>(MetricsStreamingStatusV1::NotImplemented));
        old.run(bars); next.run(bars);
        Certification::throw_on_read = Certification::unknown_version = false;
        old.successful(); next.successful();
        CHECK(next.receipt().route == MetricsRatioRouteV1::LegacyActualCurve);
        CHECK(next.receipt().extent_plan.present == 0);
        compare_report_values(next.report, old.report);
        compare_curves(next.report, old.report);
    }
}
void identity_row() {
    if (!row("invocation-identities-bind-input-spec-and-view")) return;
    auto bars = multi_month_bars();
    Instance first(certified_api), replay(certified_api), changed(certified_api), new_spec(certified_api);
    first.configure(); replay.configure(); changed.configure();
    auto adjusted = run_spec();
    adjusted.fee_value = .09;
    new_spec.configure(adjusted);
    first.preference(StreamingSharpeV1::On);
    replay.preference(StreamingSharpeV1::On);
    changed.preference(StreamingSharpeV1::On);
    new_spec.preference(StreamingSharpeV1::On);
    first.run(bars); replay.run(bars); new_spec.run(bars);
    auto altered = bars;
    altered[7].volume += 1.; // Same endpoints, different frozen input words.
    changed.run(altered);
    first.successful(); replay.successful(); changed.successful(); new_spec.successful();
    const auto a = first.receipt(), b = replay.receipt(), c = changed.receipt(), d = new_spec.receipt();
    CHECK(a.route == MetricsRatioRouteV1::Streaming && b.route == MetricsRatioRouteV1::Streaming);
    CHECK(a.extent_plan.attempt != b.extent_plan.attempt);
    CHECK(a.extent_plan.input_generation == b.extent_plan.input_generation);
    CHECK(a.extent_plan.spec_identity == b.extent_plan.spec_identity);
    CHECK(a.extent_plan.admitted_view == b.extent_plan.admitted_view);
    CHECK(c.extent_plan.input_generation != a.extent_plan.input_generation);
    CHECK(c.extent_plan.admitted_view != a.extent_plan.admitted_view);
    CHECK(d.extent_plan.input_generation == a.extent_plan.input_generation);
    CHECK(d.extent_plan.spec_identity != a.extent_plan.spec_identity);
    CHECK(d.extent_plan.admitted_view != a.extent_plan.admitted_view);
    compare_report_values(replay.report, first.report);
}
void stopped_rows() {
    for (bool before_mark : {false, true}) {
        if (!row(before_mark ? "stopped-intra-bar-keeps-whole-run-recovery"
                             : "stopped-at-final-mark-keeps-whole-run-recovery"))
            continue;
        auto bars = multi_month_bars();
        const int stop_after = before_mark ? 19 : static_cast<int>(bars.size());
        Instance old(certified_api), candidate(certified_api), authoritative(certified_api);
        for (auto* instance : {&old, &candidate, &authoritative}) {
            instance->configure();
            instance->trading()->stop_after = stop_after;
            instance->trading()->stop_before_mark = before_mark;
        }
        old.preference(StreamingSharpeV1::Off);
        candidate.preference(StreamingSharpeV1::On);
        authoritative.preference(StreamingSharpeV1::Off, kMetricsNeedAllV1);
        old.run(bars); candidate.run(bars);
        const auto receipt = candidate.receipt();
        CHECK(!candidate.host()->last_error().empty());
        CHECK(receipt.route == MetricsRatioRouteV1::LegacyRerun);
        CHECK(receipt.route_reason == MetricsRouteReasonV1::ExistingFailure);
        CHECK(receipt.replay_attempts == 1 && receipt.authoritative_attempt == 0);
        CHECK(receipt.actual_extent.count == (before_mark ? stop_after - 1 : stop_after));
        CHECK(receipt.stop_boundary.completed_marks == candidate.trading()->marks);
        CHECK(receipt.stop_boundary.input_index == candidate.trading()->input_index);
        CHECK(receipt.stop_boundary.kind == (before_mark
            ? MetricsStopBoundaryKindV1::IntraBar : MetricsStopBoundaryKindV1::CompletedMark));
        if (!before_mark) CHECK(receipt.witness == MetricsExtentWitnessV1::Match);
        CHECK(candidate.report.equity_curve == nullptr && candidate.report.equity_curve_len == 0);
        CHECK(candidate.report.trades == nullptr && candidate.report.trades_len == 0);
        compare_failure(candidate, old);
        // Fresh authoritative handle, same original callback and stop injection.
        authoritative.run(bars);
        compare_failure(authoritative, old);
        CHECK(authoritative.receipt().route == MetricsRatioRouteV1::LegacyActualCurve);
        compare_report_values(authoritative.report, old.report);
        compare_curves(authoritative.report, old.report);
        CHECK(authoritative.report.equity_curve_len == candidate.trading()->marks);
    }
}
} // namespace module_metrics_test

int main(int argc, char** argv) {
    using namespace module_metrics_test;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") list_only = true;
        else if (arg == "--row" && i + 1 < argc) filter = argv[++i];
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    capability_row();
    certified_rows();
    uncertified_row();
    off_and_default_rows();
    exclusions();
    unavailable_declaration_rows();
    identity_row();
    stopped_rows();
    if (!list_only && rows == 0) {
        std::fprintf(stderr, "no matching native module metrics-only rows\n");
        return 2;
    }
    std::printf("native module metrics-only: %d rows, %d checks, %d failures\n",
                rows, checks, failures);
    return failures ? 1 : 0;
}
