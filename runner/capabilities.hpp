#pragma once

#include "json.hpp"
#include <stdexcept>
#include <string>

namespace pineforge::live {

struct BooleanPolicy { const char* name; bool refuse_true; const char* reason; };
inline constexpr BooleanPolicy close_only_boolean_policy[] = {
    {"calc_on_every_tick", true, ""},
    {"calc_on_order_fills", true, ""},
    {"process_orders_on_close", true, " (priced-entry fill attribution is not batch-equivalent)"},
    {"use_bar_magnifier", true, ""},
    {"fill_orders_on_standard_ohlc", true, ""},
    {"calc_on_every_history_tick", true, ""},
    {"timeframe_gaps", false, ""},
    {"dynamic_requests", false, ""},
};

inline void require_close_only_boolean(const std::string& name, bool enabled) {
    for (const auto& policy : close_only_boolean_policy)
        if (name == policy.name && enabled && policy.refuse_true)
            throw std::runtime_error("close-only stream cannot honour compiled declaration: " +
                                     name + policy.reason);
}

inline void require_close_only_capabilities(const std::string& receipt) {
    const auto document = parse_json(receipt);
    only_fields(document, {"version", "declarations", "requests", "requirements", "unresolved"});
    const auto refuse = [](const std::string& name) {
        throw std::runtime_error("close-only stream cannot honour compiled declaration: " + name);
    };
    const auto& version = document.at("version");
    if (version.kind != Json::Kind::Number || version.value != "1")
        throw std::runtime_error("capabilities receipt version mismatch");
    const auto& declarations = document.at("declarations");
    only_fields(declarations, {"calc_on_every_tick", "calc_on_order_fills", "process_orders_on_close",
                              "use_bar_magnifier", "fill_orders_on_standard_ohlc",
                              "backtest_fill_limits_assumption", "currency", "timeframe",
                              "timeframe_gaps", "dynamic_requests", "calc_on_every_history_tick"});
    const auto& requests = document.at("requests");
    if (requests.kind != Json::Kind::Array)
        refuse("request.security metadata");
    for (const auto& request : requests.items) {
        const auto& function = request.at("function");
        refuse((function.kind == Json::Kind::String ? function.value : "request.*") +
               ": the native stream does not yet reproduce the batch for requested series");
    }
    const auto& unresolved = document.at("unresolved");
    if (unresolved.kind != Json::Kind::Array)
        refuse("unresolved execution requirements");
    for (const auto& value : unresolved.items) {
        if (value.kind != Json::Kind::String)
            refuse("unresolved execution requirements");
        refuse(value.value + " (unresolved at compilation)");
    }
    for (const auto& policy : close_only_boolean_policy) {
        const auto& value = declarations.at(policy.name);
        if (value.kind != Json::Kind::Bool)
            refuse(policy.name);
        require_close_only_boolean(policy.name, value.value == "true");
    }
    const auto& assumption = declarations.at("backtest_fill_limits_assumption");
    if (assumption.kind != Json::Kind::Number || assumption.value != "0")
        refuse("backtest_fill_limits_assumption");
    const auto& currency = declarations.at("currency");
    if (currency.kind != Json::Kind::String || currency.value != "currency.NONE")
        refuse("currency (account FX curve)");
    const auto& timeframe = declarations.at("timeframe");
    if (timeframe.kind != Json::Kind::String || !timeframe.value.empty())
        refuse("timeframe (declaration-owned clock)");
    const auto& requirements = document.at("requirements");
    only_fields(requirements, {"auxiliary_security_feeds", "native_security_feeds", "fx_curve",
                              "recorded_series", "historical_probe_overrides", "intrabar_persistence"});
    for (const auto& name : {"auxiliary_security_feeds", "native_security_feeds", "fx_curve",
                             "recorded_series", "historical_probe_overrides", "intrabar_persistence"}) {
        const auto& value = requirements.at(name);
        if (value.kind != Json::Kind::Bool || value.value != "false")
            refuse(name);
    }
}

}
