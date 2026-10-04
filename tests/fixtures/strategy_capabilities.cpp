#include <pineforge/checked_settings.hpp>

#ifndef PF_TEST_CAPABILITIES_DECLARATION
#define PF_TEST_CAPABILITIES_DECLARATION "calc_on_every_tick"
#endif
#ifndef PF_TEST_CAPABILITIES_VALUE
#define PF_TEST_CAPABILITIES_VALUE "false"
#endif
#ifndef PF_TEST_CAPABILITIES_VERSION
#define PF_TEST_CAPABILITIES_VERSION PF_CAPABILITIES_API_VERSION
#endif

extern "C" {
uint32_t strategy_capabilities_api_version(void) { return PF_TEST_CAPABILITIES_VERSION; }

int strategy_capabilities_receipt(pf_strategy_t strategy, char* json, size_t capacity,
                                  size_t* required, char* error, size_t error_capacity) {
    if (required) *required = 0;
    return pineforge::checked_settings::boundary(error, error_capacity, [&] {
        pineforge::checked_settings::require(strategy != nullptr, "null strategy");
        std::string document =
            "{\"declarations\":{\"backtest_fill_limits_assumption\":0,"
            "\"calc_on_every_history_tick\":false,\"calc_on_every_tick\":false,\"calc_on_order_fills\":false,"
            "\"currency\":\"currency.NONE\",\"dynamic_requests\":true,"
            "\"fill_orders_on_standard_ohlc\":false,\"process_orders_on_close\":false,"
            "\"timeframe\":\"\",\"timeframe_gaps\":true,\"use_bar_magnifier\":false},"
            "\"requests\":[],\"requirements\":{\"auxiliary_security_feeds\":false,"
            "\"fx_curve\":false,\"historical_probe_overrides\":false,"
            "\"intrabar_persistence\":false,\"native_security_feeds\":false,"
            "\"recorded_series\":false},\"unresolved\":[],\"version\":1}";
        const std::string key = std::string("\"") + PF_TEST_CAPABILITIES_DECLARATION + "\":";
        const auto begin = document.find(key) + key.size();
        const auto end = document.find_first_of(",}", begin);
        document.replace(begin, end - begin, PF_TEST_CAPABILITIES_VALUE);
        pineforge::checked_settings::receipt(document, json, capacity, required);
    });
}
}
