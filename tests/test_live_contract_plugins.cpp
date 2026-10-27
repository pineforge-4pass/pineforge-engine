// SPDX-License-Identifier: Apache-2.0
// Current C ABI/stream-v1 test libraries for runner contract selection.
// These MODULE targets select a PF_LIVE_CONTRACT_* definition or an explicit ABI
// stamp, without linking libpineforge, so query-symbol absence is real.
#include <pineforge/pineforge.h>

namespace {
int handle = 1;
int abi_calls = 0;
int strategy_calls = 0;
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
PF_API int strategy_stream_begin(pf_strategy_t, const pf_bar_t*, int, const char*, const char*) {
    ++strategy_calls;
    return 0;
}
PF_API int strategy_stream_push_tick(pf_strategy_t, const pf_trade_tick_t*) { ++strategy_calls; return 0; }
PF_API int strategy_stream_push_bar(pf_strategy_t, const pf_bar_t*) { ++strategy_calls; return 0; }
PF_API int strategy_stream_advance_time(pf_strategy_t, int64_t) { ++strategy_calls; return 0; }
PF_API int strategy_stream_order_actions_len(pf_strategy_t) { ++strategy_calls; return 0; }
PF_API int strategy_stream_order_action_get(pf_strategy_t, int, pf_stream_order_action_t*) {
    ++strategy_calls;
    return -1;
}
PF_API void strategy_stream_order_actions_clear(pf_strategy_t) { ++strategy_calls; }
PF_API uint64_t strategy_stream_state_hash(pf_strategy_t) { ++strategy_calls; return 1; }
PF_API int strategy_stream_fill_report(pf_strategy_t, pf_report_t* report) {
    ++strategy_calls;
    if (!report) return -1;
    *report = {};
    return 0;
}
PF_API void report_free(pf_report_t*) { ++strategy_calls; }

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
