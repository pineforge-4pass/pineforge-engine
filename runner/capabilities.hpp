#pragma once

#include "json.hpp"
#include <cstddef>
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

inline bool matches_pooc_evidence_set(const Json& orders) {
    if (orders.kind != Json::Kind::Array)
        return false;
    const auto serialized = orders.dump();
    return serialized == R"(["entry:market"])" || serialized == R"(["entry:stop"])" ||
           serialized == R"(["entry:limit"])" || serialized == R"(["entry:market","exit:short_bracket"])" ||
           serialized == R"(["close:market","entry:market"])";
}

inline void require_close_only_capabilities(const std::string& receipt,
                                            const std::string& confirmed_receipt,
                                            const std::string& mode,
                                            const std::string& input_tf,
                                            const std::string& script_tf,
                                            bool proven_calendar) {
    const auto document = parse_json(receipt);
    only_fields(document, {"version", "declarations", "requests", "requirements", "unresolved"});
    const auto refuse = [](const std::string& name) {
        throw std::runtime_error("close-only stream cannot honour compiled declaration: " + name);
    };
    Json confirmed;
    bool has_confirmed = !confirmed_receipt.empty();
    if (has_confirmed) {
        confirmed = parse_json(confirmed_receipt);
        const auto& proof_version = confirmed.at("version");
        if (proof_version.kind != Json::Kind::Number)
            refuse("confirmed-bar capabilities version");
        has_confirmed = proof_version.value == "1";
    }
    if (has_confirmed) {
        only_fields(confirmed, {"version", "requests", "orders", "intrabar_persistence"});
        if (confirmed.at("requests").kind != Json::Kind::Array ||
            confirmed.at("orders").kind != Json::Kind::Array ||
            confirmed.at("intrabar_persistence").kind != Json::Kind::Bool)
            refuse("confirmed-bar capabilities metadata");
    }
    const auto& version = document.at("version");
    if (version.kind != Json::Kind::Number || version.value != "1")
        throw std::runtime_error("capabilities receipt version mismatch");
    const auto& declarations = document.at("declarations");
    only_fields(declarations, {"calc_on_every_tick", "calc_on_order_fills", "process_orders_on_close",
                              "use_bar_magnifier", "fill_orders_on_standard_ohlc",
                              "backtest_fill_limits_assumption", "currency", "timeframe",
                              "timeframe_gaps", "dynamic_requests", "calc_on_every_history_tick"});
    const auto& pooc_declaration = declarations.at("process_orders_on_close");
    if (pooc_declaration.kind != Json::Kind::Bool)
        refuse("process_orders_on_close");
    require_close_only_boolean("process_orders_on_close", pooc_declaration.value == "true");
    const auto& requests = document.at("requests");
    if (requests.kind != Json::Kind::Array)
        refuse("request.security metadata");
    std::size_t request_index = 0;
    for (const auto& request : requests.items) {
        const auto& function = request.at("function");
        const auto name = function.kind == Json::Kind::String ? function.value : "request.*";
        if (!has_confirmed)
            refuse(name + ": the native stream does not yet reproduce the batch for requested series");
        if (mode != "bars" || input_tf != "1" || !proven_calendar ||
            name != "request.security" || request_index >= confirmed.at("requests").items.size())
            refuse(name + " (no confirmed-bar equivalence proof)");
        const auto& proof = confirmed.at("requests").items[request_index++];
        only_fields(proof, {"function", "symbol", "timeframe", "lookahead", "gaps", "heikinashi", "feed", "expression"});
        for (const auto& field : {"function", "symbol", "timeframe", "lookahead", "gaps", "heikinashi", "feed"}) {
            if (request.at(field).dump() != proof.at(field).dump())
                refuse(name + " (receipt disagreement)");
        }
        if (request.at("feed").kind != Json::Kind::String || request.at("feed").value != "chart")
            refuse(name + ".feed (other-symbol or unpinned data)");
        if (request.at("lookahead").kind != Json::Kind::String || request.at("lookahead").value != "barmerge.lookahead_off")
            refuse(name + ".lookahead");
        const auto& timeframe = request.at("timeframe");
        const auto& expression = proof.at("expression");
        const auto& gaps = request.at("gaps");
        const auto& heikinashi = request.at("heikinashi");
        if (timeframe.kind != Json::Kind::String || expression.kind != Json::Kind::String ||
            gaps.kind != Json::Kind::String || heikinashi.kind != Json::Kind::Bool)
            refuse(name + " metadata");
        const auto& clock = timeframe.value;
        if (clock != "5" && clock != "15" && clock != "60" && clock != "D" && clock != "timeframe.period")
            refuse(name + ".timeframe (unproven clock: " + clock + ")");
        const bool plain = heikinashi.value == "false";
        const bool held = gaps.value == "barmerge.gaps_off";
        const bool proven =
            (clock == "5" && expression.value == "close" && held &&
                (script_tf == "1" || (plain && script_tf == "5"))) ||
            (clock == "15" && expression.value == "ta.sma(close,4)" && held && plain && script_tf == "1") ||
            (clock == "60" && expression.value == "close" && held && plain &&
                (script_tf == "1" || script_tf == "15")) ||
            (clock == "60" && expression.value == "ta.ema(close,3)" &&
                gaps.value == "barmerge.gaps_on" && plain && script_tf == "1") ||
            (clock == "D" && expression.value == "close" && held && plain && script_tf == "1") ||
            (clock == "timeframe.period" && expression.value == "close[1]" && held && plain && script_tf == "1");
        if (!proven)
            refuse(name + " (unproven expression, merge policy or chart clock)");
    }
    if (has_confirmed && request_index != confirmed.at("requests").items.size())
        refuse("request.security (receipt count disagreement)");
    const bool intrabar = document.at("requirements").at("intrabar_persistence").value == "true";
    if (requests.items.size() > 1 || (!requests.items.empty() && intrabar))
        refuse("request.security (unproven composition)");
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
        if (std::string(name) == "intrabar_persistence" && value.kind == Json::Kind::Bool &&
            value.value == "true" && has_confirmed && mode == "bars" &&
            input_tf == "1" && script_tf == "1" && proven_calendar &&
            confirmed.at("intrabar_persistence").value == "true")
            continue;
        if (value.kind != Json::Kind::Bool || value.value != "false")
            refuse(name);
    }
}

inline void require_close_only_capabilities(const std::string& receipt) {
    require_close_only_capabilities(receipt, {}, "unproven", {}, {}, false);
}

}
