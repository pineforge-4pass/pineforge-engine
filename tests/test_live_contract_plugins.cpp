// SPDX-License-Identifier: Apache-2.0
// Current C ABI/stream-v1 test libraries for runner contract selection.
// These MODULE targets select a PF_LIVE_CONTRACT_* definition or an explicit ABI
// stamp, without linking libpineforge, so query-symbol absence is real.
#include <pineforge/pineforge.h>

namespace {
int handle = 1;
int abi_calls = 0;
int strategy_calls = 0;
#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
int warmup_count = 0;
int64_t stream_origin = 0;

double cumulative_profit(int count) { return 1.25 * count * (count + 1) / 2; }
#endif
}

extern "C" {

PF_API int pf_abi_version(void) {
    ++abi_calls;
#if defined(PF_LIVE_ABI_STAMP)
    return PF_LIVE_ABI_STAMP;
#else
    return PF_ABI_VERSION;
#endif
}
PF_API int strategy_stream_api_version(void) { ++strategy_calls; return 1; }
PF_API pf_strategy_t strategy_create(const char*) { ++strategy_calls; return &handle; }
PF_API void strategy_free(pf_strategy_t) { ++strategy_calls; }
PF_API const char* strategy_get_last_error(pf_strategy_t) { ++strategy_calls; return ""; }
PF_API int strategy_stream_begin(pf_strategy_t, const pf_bar_t* bars, int n, const char*, const char*) {
    ++strategy_calls;
#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
    if (!bars || n < 1) return -1;
    handle = 1;
    warmup_count = n;
    stream_origin = bars[n - 1].timestamp + 60000;
#endif
    return 0;
}
PF_API int strategy_stream_push_tick(pf_strategy_t, const pf_trade_tick_t*) { ++strategy_calls; return 0; }
PF_API int strategy_stream_push_bar(pf_strategy_t, const pf_bar_t*) {
    ++strategy_calls;
#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
    ++handle;
#endif
    return 0;
}
PF_API int strategy_stream_advance_time(pf_strategy_t, int64_t) { ++strategy_calls; return 0; }
PF_API int strategy_stream_order_actions_len(pf_strategy_t) { ++strategy_calls; return 0; }
PF_API int strategy_stream_order_action_get(pf_strategy_t, int, pf_stream_order_action_t*) {
    ++strategy_calls;
    return -1;
}
PF_API void strategy_stream_order_actions_clear(pf_strategy_t) { ++strategy_calls; }
PF_API uint64_t strategy_stream_state_hash(pf_strategy_t) {
    ++strategy_calls;
    return static_cast<uint64_t>(handle);
}
PF_API int strategy_stream_fill_report(pf_strategy_t, pf_report_t* report) {
    ++strategy_calls;
    if (!report) return -1;
    *report = {};
#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
    // Deliberately not an engine: the runner must obtain every value here.
    const int count = handle - 1;
    report->total_trades = report->trades_len = count;
    report->net_profit = cumulative_profit(count);
    report->input_bars_processed = report->script_bars_processed = warmup_count + count;
    report->input_tf_seconds = report->script_tf_seconds = 60;
    report->script_tf_ratio = 1;
    report->trades = new pf_trade_t[count]{};
    for (int i = 0; i < count; ++i) {
        auto& trade = report->trades[i];
        trade.entry_time = stream_origin + i * 60000;
        trade.exit_time = trade.entry_time + 30000;
        trade.entry_price = 100 + i;
        trade.exit_price = trade.entry_price + 1.5;
        trade.pnl = 1.25 * (i + 1);
        trade.pnl_pct = 100 * trade.pnl / (trade.entry_price * (i + 1));
        trade.is_long = 1;
        trade.max_runup = 2.0 * (i + 1);
        trade.max_drawdown = 0.5 * (i + 1);
        trade.qty = i + 1;
        trade.commission = 0.25 * (i + 1);
        trade.entry_bar_index = trade.exit_bar_index = warmup_count + i;
    }
    report->equity_curve_len = report->broker_state_hash_len = count + 1;
    report->equity_curve = new pf_equity_point_t[count + 1]{};
    report->broker_state_hash = new uint64_t[count + 1]{};
    for (int i = 0; i <= count; ++i) {
        report->equity_curve[i] = {stream_origin + (i - 1) * 60000,
                                  10000 + cumulative_profit(i) + 0.5 * i, 0.5 * i};
        report->broker_state_hash[i] = static_cast<uint64_t>(i + 1);
    }
    report->metrics.all.num_trades = report->metrics.all.num_wins = count;
    report->metrics.all.net_profit = report->metrics.all.gross_profit = report->net_profit;
    report->metrics.all.percent_profitable = count ? 100 : 0;
    report->metrics.all.commission_paid = 0.25 * count * (count + 1) / 2;
    report->metrics.all.avg_trade = count ? report->net_profit / count : 0;
    report->metrics.longs = report->metrics.all;
    report->metrics.equity.open_pl = 0.5 * count;
    report->metrics.equity.sharpe_monthly = 0.125 * count;
    report->metrics.equity.sortino_monthly = 0.25 * count;
    report->metrics.equity.sharpe_bar = 0.375 * count;
    report->metrics.equity.sortino_bar = 0.5 * count;
#endif
    return 0;
}
PF_API void report_free(pf_report_t* report) {
    ++strategy_calls;
#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
    if (!report) return;
    delete[] report->trades;
    delete[] report->equity_curve;
    delete[] report->broker_state_hash;
    *report = {};
#endif
}

#if defined(PF_LIVE_CONTRACT_UNKNOWN)
PF_API int strategy_execution_contract(pf_strategy_t) { ++strategy_calls; return 99; }
#endif

#if defined(PF_LIVE_CONTRACT_NATIVE_NO_EXPORT)
PF_API int strategy_execution_contract(pf_strategy_t) { ++strategy_calls; return 2; }
#endif

#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
PF_API int strategy_execution_contract(pf_strategy_t) { ++strategy_calls; return 2; }
PF_API int strategy_configure_native_v1(pf_strategy_t, const pf_native_run_spec_v1* spec) {
    ++strategy_calls;
    if (!spec || spec->struct_size != sizeof(pf_native_run_spec_v1))
        return -1;
    return 0;
}
#endif

#if defined(PF_LIVE_CONTRACT_ABSENT) || defined(PF_LIVE_ABI_STAMP)
PF_API void strategy_set_input(pf_strategy_t, const char*, const char*) { ++strategy_calls; }
PF_API void strategy_set_override(pf_strategy_t, const char*, const char*) { ++strategy_calls; }
PF_API void strategy_set_syminfo_timezone(pf_strategy_t, const char*) { ++strategy_calls; }
PF_API void strategy_set_chart_timezone(pf_strategy_t, const char*) { ++strategy_calls; }
PF_API void strategy_set_syminfo_session(pf_strategy_t, const char*) { ++strategy_calls; }
PF_API int strategy_set_syminfo_string(pf_strategy_t, const char*, const char*) { ++strategy_calls; return 0; }
PF_API void strategy_set_syminfo_mintick(pf_strategy_t, double) { ++strategy_calls; }
PF_API void strategy_set_syminfo_pointvalue(pf_strategy_t, double) { ++strategy_calls; }
PF_API void strategy_set_syminfo_metadata(pf_strategy_t, const char*, double) { ++strategy_calls; }
#endif

#if defined(PF_LIVE_ABI_STAMP)
PF_API void run_backtest_full(pf_strategy_t, pf_bar_t*, int,
                             const char*, const char*, int, int,
                             pf_magnifier_distribution_t, pf_report_t* report) {
    ++strategy_calls;
    if (report) *report = {};
}
// The counters observe the actual exported entry points above. Their readers
// are test instrumentation, separate from strategy calls and the ABI query.
PF_API int pf_test_abi_calls(void) { return abi_calls; }
PF_API int pf_test_strategy_calls(void) { return strategy_calls; }
#endif

}  // extern "C"
