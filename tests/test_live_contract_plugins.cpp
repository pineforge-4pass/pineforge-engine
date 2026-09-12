// SPDX-License-Identifier: Apache-2.0
// ABI4/stream-v1 test libraries for runner contract selection. Built as
// MODULE targets with exactly one PF_LIVE_CONTRACT_* definition and without
// linking libpineforge, so query-symbol absence is real.
#include <pineforge/pineforge.h>

namespace {
int handle = 1;
}

extern "C" {

PF_API int pf_abi_version(void) { return PF_ABI_VERSION; }
PF_API int strategy_stream_api_version(void) { return 1; }
PF_API pf_strategy_t strategy_create(const char*) { return &handle; }
PF_API void strategy_free(pf_strategy_t) {}
PF_API const char* strategy_get_last_error(pf_strategy_t) { return ""; }
PF_API int strategy_stream_begin(pf_strategy_t, const pf_bar_t*, int, const char*, const char*) {
    return 0;
}
PF_API int strategy_stream_push_tick(pf_strategy_t, const pf_trade_tick_t*) { return 0; }
PF_API int strategy_stream_push_bar(pf_strategy_t, const pf_bar_t*) { return 0; }
PF_API int strategy_stream_advance_time(pf_strategy_t, int64_t) { return 0; }
PF_API int strategy_stream_order_actions_len(pf_strategy_t) { return 0; }
PF_API int strategy_stream_order_action_get(pf_strategy_t, int, pf_stream_order_action_t*) {
    return -1;
}
PF_API void strategy_stream_order_actions_clear(pf_strategy_t) {}
PF_API uint64_t strategy_stream_state_hash(pf_strategy_t) { return 1; }

#if defined(PF_LIVE_CONTRACT_UNKNOWN)
PF_API int strategy_execution_contract(pf_strategy_t) { return 99; }
#endif

#if defined(PF_LIVE_CONTRACT_NATIVE_NO_EXPORT)
PF_API int strategy_execution_contract(pf_strategy_t) { return 2; }
#endif

#if defined(PF_LIVE_CONTRACT_NATIVE_STUB)
PF_API int strategy_execution_contract(pf_strategy_t) { return 2; }
PF_API int strategy_configure_native_v1(pf_strategy_t, const pf_native_run_spec_v1* spec) {
    if (!spec || spec->struct_size != sizeof(pf_native_run_spec_v1))
        return -1;
    return 0;
}
#endif

#if defined(PF_LIVE_CONTRACT_ABSENT)
PF_API void strategy_set_input(pf_strategy_t, const char*, const char*) {}
PF_API void strategy_set_override(pf_strategy_t, const char*, const char*) {}
PF_API void strategy_set_syminfo_timezone(pf_strategy_t, const char*) {}
PF_API void strategy_set_chart_timezone(pf_strategy_t, const char*) {}
PF_API void strategy_set_syminfo_session(pf_strategy_t, const char*) {}
PF_API int strategy_set_syminfo_string(pf_strategy_t, const char*, const char*) { return 0; }
PF_API void strategy_set_syminfo_mintick(pf_strategy_t, double) {}
PF_API void strategy_set_syminfo_pointvalue(pf_strategy_t, double) {}
PF_API void strategy_set_syminfo_metadata(pf_strategy_t, const char*, double) {}
#endif

}  // extern "C"
