#include "capabilities.hpp"
#include <cassert>
#include <iostream>

using pineforge::live::Json;
using pineforge::live::parse_json;
using pineforge::live::require_close_only_capabilities;

int main() {
    auto legacy = parse_json(R"({"version":1,"declarations":{"calc_on_every_tick":false,"calc_on_order_fills":false,"process_orders_on_close":false,"use_bar_magnifier":false,"fill_orders_on_standard_ohlc":false,"backtest_fill_limits_assumption":0,"currency":"currency.NONE","timeframe":"","timeframe_gaps":true,"dynamic_requests":true,"calc_on_every_history_tick":false},"requests":[],"requirements":{"auxiliary_security_feeds":false,"native_security_feeds":false,"fx_curve":false,"recorded_series":false,"historical_probe_overrides":false,"intrabar_persistence":false},"unresolved":[]})");
    auto confirmed = parse_json(R"({"version":1,"requests":[],"orders":["entry:market"],"intrabar_persistence":false})");
    require_close_only_capabilities(legacy.dump());
    require_close_only_capabilities(legacy.dump(), confirmed.dump());
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
    require_close_only_capabilities(legacy.dump(), confirmed.dump());
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
    auto disagreement = confirmed;
    disagreement.members["requests"].items[0].members["heikinashi"] = Json::boolean(true);
    refused(legacy, disagreement, "receipt disagreement");
    disagreement = confirmed;
    disagreement.members["version"] = Json::number("2");
    refused(legacy, disagreement, "version");
    legacy.members["requests"].items.clear();
    confirmed.members["requests"].items.clear();
    legacy.members["declarations"].members["process_orders_on_close"] = Json::boolean(true);
    for (const auto& shape : {"entry:market", "entry:stop", "entry:limit", "exit:short_bracket", "close:market"}) {
        confirmed.members["orders"].items = {Json::string(shape)};
        require_close_only_capabilities(legacy.dump(), confirmed.dump());
    }
    refused(legacy, Json{}, "process_orders_on_close");
    refused(legacy, confirmed, "process_orders_on_close", "ticks");
    refused(legacy, confirmed, "process_orders_on_close", "bars", "1", "5");
    confirmed.members["orders"].items.clear();
    refused(legacy, confirmed, "process_orders_on_close");
    confirmed.members["orders"].items = {Json::string("strategy.entry (unproven order shape)")};
    refused(legacy, confirmed, "strategy.entry");
    legacy.members["declarations"].members["process_orders_on_close"] = Json::boolean(false);
    legacy.members["requirements"].members["intrabar_persistence"] = Json::boolean(true);
    confirmed.members["intrabar_persistence"] = Json::boolean(true);
    require_close_only_capabilities(legacy.dump(), confirmed.dump());
    refused(legacy, Json{}, "intrabar_persistence");
    refused(legacy, confirmed, "intrabar_persistence", "ticks");
    std::cout << "confirmed capability policy: self-contained header, five order families, clock allowlist, legacy and tick refusals=" << refusals << " PASS\n";
}
