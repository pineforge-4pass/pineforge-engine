#include "capabilities.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using pineforge::live::Json;
using pineforge::live::parse_json;
using pineforge::live::require_close_only_capabilities;

int main() {
    auto legacy = parse_json(R"({"version":1,"declarations":{"calc_on_every_tick":false,"calc_on_order_fills":false,"process_orders_on_close":false,"use_bar_magnifier":false,"fill_orders_on_standard_ohlc":false,"backtest_fill_limits_assumption":0,"currency":"currency.NONE","timeframe":"","timeframe_gaps":true,"dynamic_requests":true,"calc_on_every_history_tick":false},"requests":[],"requirements":{"auxiliary_security_feeds":false,"native_security_feeds":false,"fx_curve":false,"recorded_series":false,"historical_probe_overrides":false,"intrabar_persistence":false},"unresolved":[]})");
    auto confirmed = parse_json(R"({"version":1,"requests":[],"orders":["entry:market"],"intrabar_persistence":false})");
    const auto admitted = [](const Json& base, const Json& proof) {
        require_close_only_capabilities(base.dump(), proof.dump(), "bars", "1", "1", true);
    };
    require_close_only_capabilities(legacy.dump());
    admitted(legacy, confirmed);
    auto future = confirmed;
    future.members["version"] = Json::number("2");
    admitted(legacy, future);
    int refusals = 0;
    const auto refused = [&](const Json& base, const Json& proof, const std::string& name,
                             const std::string& mode = "bars", const std::string& input_tf = "1",
                             const std::string& script_tf = "1", bool proven_calendar = true) {
        try {
            require_close_only_capabilities(base.dump(), proof.kind == Json::Kind::Null ? "" : proof.dump(),
                                            mode, input_tf, script_tf, proven_calendar);
            assert(false);
        } catch (const std::runtime_error& error) {
            assert(std::string(error.what()).find(name) != std::string::npos);
            ++refusals;
        }
    };
    auto request = parse_json(R"({"function":"request.security","symbol":"syminfo.tickerid","timeframe":"5","lookahead":"barmerge.lookahead_off","gaps":"barmerge.gaps_off","heikinashi":false,"feed":"chart"})");
    legacy.members["requests"].items = {request};
    request.members["expression"] = Json::string("close");
    confirmed.members["requests"].items = {request};
    admitted(legacy, confirmed);
    refused(legacy, Json{}, "request.security");
    refused(legacy, confirmed, "request.security", "ticks");
    refused(legacy, confirmed, "request.security", "bars", "5");
    refused(legacy, confirmed, "request.security", "bars", "1", "15");
    refused(legacy, confirmed, "request.security", "bars", "1", "1", false);
    for (const auto& clock : {"M", "W", "5D", "30S", "7", "1", "240", "future"}) {
        auto unproven = legacy;
        auto proof = confirmed;
        unproven.members["requests"].items[0].members["timeframe"] = Json::string(clock);
        proof.members["requests"].items[0].members["timeframe"] = Json::string(clock);
        refused(unproven, proof, "request.security.timeframe");
    }
    for (const auto& clock : {"5", "15", "60", "D", "timeframe.period"}) {
        for (const auto& expression : {"close", "ta.sma(close,4)", "ta.ema(close,3)", "close[1]"}) {
            for (const auto& script : {"1", "5", "15"}) {
                const bool proven = (std::string(clock) == "5" && std::string(expression) == "close" &&
                    (std::string(script) == "1" || std::string(script) == "5")) ||
                    (std::string(clock) == "60" && std::string(expression) == "close" &&
                    (std::string(script) == "1" || std::string(script) == "15")) ||
                    (std::string(clock) == "D" && std::string(expression) == "close" && std::string(script) == "1") ||
                    (std::string(clock) == "15" && std::string(expression) == "ta.sma(close,4)" && std::string(script) == "1") ||
                    (std::string(clock) == "timeframe.period" && std::string(expression) == "close[1]" && std::string(script) == "1");
                if (proven) continue;
                auto base = legacy;
                auto proof = confirmed;
                base.members["requests"].items[0].members["timeframe"] = Json::string(clock);
                proof.members["requests"].items[0].members["timeframe"] = Json::string(clock);
                proof.members["requests"].items[0].members["expression"] = Json::string(expression);
                refused(base, proof, "request.security", "bars", "1", script);
            }
        }
    }
    auto ha_base = legacy;
    auto ha_proof = confirmed;
    ha_base.members["requests"].items[0].members["heikinashi"] = Json::boolean(true);
    ha_proof.members["requests"].items[0].members["heikinashi"] = Json::boolean(true);
    refused(ha_base, ha_proof, "request.security", "bars", "1", "5");
    for (const auto& clock : {"15", "60", "D", "timeframe.period"}) {
        ha_base.members["requests"].items[0].members["timeframe"] = Json::string(clock);
        ha_proof.members["requests"].items[0].members["timeframe"] = Json::string(clock);
        ha_proof.members["requests"].items[0].members["expression"] = Json::string(
            std::string(clock) == "15" ? "ta.sma(close,4)" :
            std::string(clock) == "timeframe.period" ? "close[1]" : "close");
        refused(ha_base, ha_proof, "request.security");
    }
    ha_base.members["requests"].items[0].members["timeframe"] = Json::string("60");
    ha_proof.members["requests"].items[0].members["timeframe"] = Json::string("60");
    ha_base.members["requests"].items[0].members["gaps"] = Json::string("barmerge.gaps_on");
    ha_proof.members["requests"].items[0].members["gaps"] = Json::string("barmerge.gaps_on");
    ha_proof.members["requests"].items[0].members["expression"] = Json::string("ta.ema(close,3)");
    refused(ha_base, ha_proof, "request.security");
    auto persistent_request_base = legacy;
    auto persistent_request_proof = confirmed;
    persistent_request_base.members["requirements"].members["intrabar_persistence"] = Json::boolean(true);
    persistent_request_proof.members["intrabar_persistence"] = Json::boolean(true);
    refused(persistent_request_base, persistent_request_proof, "unproven composition");
    auto multiple_base = legacy;
    auto multiple_proof = confirmed;
    multiple_base.members["requests"].items.push_back(multiple_base.at("requests").items.front());
    multiple_proof.members["requests"].items.push_back(multiple_proof.at("requests").items.front());
    refused(multiple_base, multiple_proof, "unproven composition");
    auto disagreement = confirmed;
    disagreement.members["requests"].items[0].members["heikinashi"] = Json::boolean(true);
    refused(legacy, disagreement, "receipt disagreement");
    disagreement = confirmed;
    disagreement.members["version"] = Json::number("2");
    refused(legacy, disagreement, "request.security");
    legacy.members["requests"].items.clear();
    confirmed.members["requests"].items.clear();
    legacy.members["declarations"].members["process_orders_on_close"] = Json::boolean(true);
    const std::vector<std::string> families{"close:market", "entry:limit", "entry:market", "entry:stop", "exit:short_bracket"};
    for (unsigned mask = 1; mask < 32; ++mask) {
        confirmed.members["orders"].items.clear();
        for (std::size_t index = 0; index < families.size(); ++index)
            if (mask & (1u << index)) confirmed.members["orders"].items.push_back(Json::string(families[index]));
        const bool proven = mask == 2 || mask == 4 || mask == 8 || mask == 5 || mask == 20;
        assert(matches_pooc_evidence_set(confirmed.at("orders")) == proven);
        refused(legacy, confirmed, "process_orders_on_close");
    }
    confirmed.members["orders"].items = {Json::string("entry:market")};
    refused(legacy, confirmed, "process_orders_on_close", "bars", "1", "1", false);
    refused(legacy, Json{}, "process_orders_on_close");
    refused(legacy, confirmed, "process_orders_on_close", "ticks");
    refused(legacy, confirmed, "process_orders_on_close", "bars", "1", "5");
    confirmed.members["orders"].items = {Json::string("entry:stop")};
    refused(legacy, confirmed, "process_orders_on_close", "bars", "1", "5");
    confirmed.members["orders"].items.clear();
    refused(legacy, confirmed, "process_orders_on_close");
    confirmed.members["orders"].items = {Json::string("strategy.entry (unproven order shape)")};
    refused(legacy, confirmed, "process_orders_on_close");
    legacy.members["declarations"].members["process_orders_on_close"] = Json::boolean(false);
    legacy.members["requirements"].members["intrabar_persistence"] = Json::boolean(true);
    confirmed.members["intrabar_persistence"] = Json::boolean(true);
    admitted(legacy, confirmed);
    refused(legacy, Json{}, "intrabar_persistence");
    refused(legacy, confirmed, "intrabar_persistence", "ticks");
    refused(legacy, confirmed, "intrabar_persistence", "bars", "1", "5");
    refused(legacy, confirmed, "intrabar_persistence", "bars", "1", "1", false);
    legacy.members["declarations"].members["process_orders_on_close"] = Json::boolean(true);
    refused(legacy, confirmed, "process_orders_on_close");
    std::cout << "confirmed capability policy: blanket POOC refusal, five retained evidence sets, mixed request matrix, calendar and composition refusals=" << refusals << " PASS\n";
}
