// A fault must overwrite an uninitialized C output with an empty report
// and preserve an execution failure's earlier diagnostic.
#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
const char* row = "setup";
constexpr int kBars = 8;
constexpr std::int64_t kTime = 1704067200000LL;
constexpr double kCapital = 1e301;
constexpr double kPointValue = 1e300;
constexpr double kQty = 0.5;
constexpr double kLastClose = 2.5e8;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", row, __FILE__, __LINE__,    \
                     #condition);                                             \
        ++failures;                                                            \
    }                                                                          \
} while (0)

struct Host {
    pf_strategy_t handle = nullptr;
    bool fail_callback = false;
    int calculations = 0;
    int opened = 0;
    int closed = 0;
    int command_error = PF_NATIVE_OK;

    void submit(std::uint32_t intent, const char* label) {
        pf_native_request_v1 request{};
        request.struct_size = static_cast<std::uint32_t>(sizeof(request));
        request.version = PF_NATIVE_API_VERSION;
        request.intent = intent;
        request.intent_value = intent == PF_NATIVE_INTENT_TRANSACT ? kQty : 0.0;
        request.trigger = PF_NATIVE_TRIGGER_MARKET;
        request.label = label;
        const int rc = strategy_native_submit_v1(handle, &request, nullptr, nullptr);
        if (rc != PF_NATIVE_OK && command_error == PF_NATIVE_OK) command_error = rc;
    }
};

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    ++host.calculations;
    if (host.calculations == 1) host.submit(PF_NATIVE_INTENT_TRANSACT, "entry");
    if (host.calculations == 3) host.submit(PF_NATIVE_INTENT_FLATTEN, "exit");
    return host.fail_callback && host.calculations == kBars ? 1 : 0;
}

int on_applied(void* user, const pf_native_applied_v1* applied,
               const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    if (applied->opened_units != 0.0) ++host.opened;
    if (applied->closed_units != 0.0) ++host.closed;
    return 0;
}

pf_native_run_spec_v1 spec() {
    pf_native_run_spec_v1 value{};
    value.struct_size = static_cast<std::uint32_t>(sizeof(value));
    value.session_key = "c-host-report-failure";
    value.run_number = 1;
    value.input_tf = value.script_tf = "1";
    value.ticker = "METRICS";
    value.tickerid = "TEST:METRICS";
    value.type = "crypto";
    value.currency = "USDT";
    value.basecurrency = "ETH";
    value.description = value.volumetype = value.chart_timezone = "";
    value.timezone = "UTC";
    value.session = "24x7";
    value.initial_capital = kCapital;
    value.point_value = kPointValue;
    value.account_fx = 1;
    value.price_tick = 0;
    value.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
    value.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    return value;
}

std::array<pf_bar_t, kBars> tape() {
    std::array<pf_bar_t, kBars> bars{};
    for (int i = 0; i < kBars; ++i) {
        auto& bar = bars[static_cast<std::size_t>(i)];
        // Close the real trade at 2 before the buy/hold-only price jump.
        const double price = i < 3 ? 1.0 : i == 3 ? 2.0 : kLastClose;
        bar.open = bar.high = bar.low = bar.close = price;
        bar.volume = 1;
        bar.timestamp = kTime + static_cast<std::int64_t>(i) * 60'000;
    }
    return bars;
}

std::string text(const char* value) {
    return value ? value : "<null>";
}

void check_later_sections_empty(const pf_report_t& report) {
    CHECK(report.security_diag == nullptr && report.security_diag_len == 0);
    CHECK(report.security_feeds_total == 0);
    CHECK(report.security_complete_total == 0 && report.security_partial_total == 0);
    CHECK(report.trace == nullptr && report.trace_len == 0);
    CHECK(report.trace_names == nullptr && report.trace_names_len == 0);
    CHECK(report.broker_state_hash == nullptr && report.broker_state_hash_len == 0);
}

void check_arrays_empty(const pf_report_t& report) {
    check_later_sections_empty(report);
    CHECK(report.trades == nullptr && report.trades_len == 0);
    CHECK(report.equity_curve == nullptr && report.equity_curve_len == 0);
}

void check_empty_report(const pf_report_t& report) {
    check_arrays_empty(report);
    CHECK(report.total_trades == 0 && report.net_profit == 0);
    CHECK(report.input_bars_processed == 0 && report.script_bars_processed == 0);
    CHECK(report.magnifier_sub_bars_total == 0 && report.magnifier_sample_ticks_total == 0);
    CHECK(report.input_tf_seconds == 0 && report.script_tf_seconds == 0);
    CHECK(report.script_tf_ratio == 0 && report.needs_aggregation == 0
          && report.bar_magnifier_enabled == 0);
    const pf_trade_stats_t* blocks[]{&report.metrics.all, &report.metrics.longs,
                                    &report.metrics.shorts};
    for (const auto* stats : blocks) {
        CHECK(stats->num_trades == 0 && stats->num_wins == 0
              && stats->num_losses == 0 && stats->num_even == 0);
        CHECK(stats->max_consecutive_wins == 0 && stats->max_consecutive_losses == 0);
        const double values[]{
            stats->percent_profitable, stats->net_profit, stats->net_profit_pct,
            stats->gross_profit, stats->gross_profit_pct, stats->gross_loss, stats->gross_loss_pct,
            stats->profit_factor, stats->avg_trade, stats->avg_trade_pct, stats->avg_win,
            stats->avg_win_pct, stats->avg_loss, stats->avg_loss_pct, stats->ratio_avg_win_avg_loss,
            stats->largest_win, stats->largest_win_pct, stats->largest_loss, stats->largest_loss_pct,
            stats->commission_paid, stats->expectancy, stats->avg_bars_in_trade,
            stats->avg_bars_in_wins, stats->avg_bars_in_losses};
        for (double value : values) CHECK(value == 0 && !std::signbit(value));
    }
    const auto& equity = report.metrics.equity;
    const double values[]{
        equity.max_equity_drawdown, equity.max_equity_drawdown_pct,
        equity.max_equity_runup, equity.max_equity_runup_pct,
        equity.buy_hold_return, equity.buy_hold_return_pct,
        equity.sharpe_monthly, equity.sortino_monthly, equity.sharpe_bar, equity.sortino_bar,
        equity.cagr, equity.calmar, equity.recovery_factor, equity.time_in_market_pct, equity.open_pl};
    for (double value : values) CHECK(value == 0 && !std::signbit(value));
}

void run_case(bool fail_callback) {
    row = fail_callback ? "failed_execution_report_throw" : "completed_execution_report_throw";
    Host host;
    host.fail_callback = fail_callback;
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof(callbacks));
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &host;
    callbacks.on_bar = &on_bar;
    callbacks.on_applied = &on_applied;
    host.handle = strategy_native_host_create_v1(&callbacks);
    CHECK(host.handle != nullptr);
    if (!host.handle) return;

    const auto base = spec();
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = static_cast<std::uint32_t>(sizeof(ext));
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    const int configured = strategy_configure_native_ext_v1(host.handle, &base, &ext);
    CHECK(configured == PF_NATIVE_OK);
    if (configured != PF_NATIVE_OK) {
        strategy_native_host_free(host.handle);
        return;
    }
    strategy_set_broker_state_hash_recording(host.handle, 1);
    const auto bars = tape();
    // Same buy/hold overflow as the metrics fixture: pct and all real trade
    // arithmetic are finite, but buying with all capital exceeds binary64.
    const double buy_hold_ratio = bars.back().close / bars.front().open - 1.0;
    CHECK(std::isfinite(buy_hold_ratio * 100.0));
    CHECK(std::isinf(kCapital * buy_hold_ratio));
    const double pnl = (bars[3].open - bars[1].open) * kQty * kPointValue;
    CHECK(std::isfinite(pnl) && std::isfinite(kCapital + pnl));

    pf_report_t report;
    std::memset(&report, 0xA5, sizeof(report));
    const int rc = strategy_native_run_v1(host.handle, bars.data(), kBars, &report);
    CHECK(host.command_error == PF_NATIVE_OK);
    CHECK(host.calculations == kBars && host.opened == 1 && host.closed == 1);
    pf_native_state_v1 state{};
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    CHECK(strategy_native_state_v1(host.handle, &state) == PF_NATIVE_OK);

    if (fail_callback) {
        CHECK(rc == PF_NATIVE_E_RUN_FAILED);
        CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_FAILED);
        CHECK(state.failure_code == PF_NATIVE_FAILURE_CALLBACK_EXCEPTION);
        CHECK(text(strategy_get_last_error_code(host.handle)) == "engine_unclassified_error");
        CHECK(text(strategy_get_last_error_args(host.handle)) == "{}");
        CHECK(text(strategy_get_last_error(host.handle)) == "native C callback refused in on_bar");
        // The real round trip above still occurred. Its faulted report must
        // expose neither that trade nor a curve/metric prefix.
        check_empty_report(report);
    } else {
        CHECK(rc == PF_NATIVE_E_STATE);
        CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
        CHECK(text(strategy_get_last_error_code(host.handle)) == "report_result_invalid");
        CHECK(text(strategy_get_last_error_args(host.handle))
              == "{\"field\":\"metrics.equity.buy_hold_return\",\"reason\":\"nonfinite\"}");
        CHECK(report.total_trades == 0 && report.net_profit == 0);
        CHECK(report.input_bars_processed == 0 && report.script_bars_processed == 0);
        CHECK(report.metrics.all.num_trades == 0 && report.metrics.all.net_profit == 0);
        check_empty_report(report);
    }
    // This block was never assigned because buy/hold throws before the
    // equity-statistics value can be returned; its local starts at zero.
    CHECK(report.metrics.equity.buy_hold_return == 0);
    CHECK(report.metrics.equity.cagr == 0 && report.metrics.equity.calmar == 0);
    CHECK(report.metrics.equity.sharpe_bar == 0 && report.metrics.equity.sortino_bar == 0);
    CHECK(report.metrics.equity.sharpe_monthly == 0 && report.metrics.equity.sortino_monthly == 0);
    check_later_sections_empty(report);

    const std::string first_code = text(strategy_get_last_error_code(host.handle));
    const std::string first_args = text(strategy_get_last_error_args(host.handle));
    const std::string first_message = text(strategy_get_last_error(host.handle));
    pf_report_t later;
    std::memset(&later, 0xA5, sizeof(later));
    CHECK(strategy_stream_fill_report(host.handle, &later) == 0);
    check_empty_report(later);
    CHECK(text(strategy_get_last_error_code(host.handle)) == first_code);
    CHECK(text(strategy_get_last_error_args(host.handle)) == first_args);
    CHECK(text(strategy_get_last_error(host.handle)) == first_message);
    strategy_native_report_free_v1(&later);
    check_empty_report(later);
    strategy_native_report_free_v1(&later);
    check_empty_report(later);

    // Always use the public free, including after a failed CHECK. Garbage
    // pointers left by report assembly must be a sanitizer failure.
    strategy_native_report_free_v1(&report);
    check_arrays_empty(report);
    strategy_native_report_free_v1(&report);
    check_arrays_empty(report);
    strategy_native_host_free(host.handle);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "completed") != 0
                    && std::strcmp(argv[1], "failed") != 0)) {
        std::fprintf(stderr, "usage: %s [completed|failed]\n", argv[0]);
        return 2;
    }
    if (argc == 1 || std::strcmp(argv[1], "completed") == 0) run_case(false);
    if (argc == 1 || std::strcmp(argv[1], "failed") == 0) run_case(true);
    std::printf("native C-host report failure: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
