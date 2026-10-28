#pragma once

#include "../runner/report.hpp"

namespace pineforge::live {

// Test-only contract document from a complete batch ABI report. This route
// does not consume live patches or use the production cumulative fold.
inline Json native_report_document(const pf_report_t& report, const std::string& deployment,
                                   std::uint64_t cursor, std::uint64_t state_hash) {
    auto fields = native_report_json(report);
    Json closed;
    closed.kind = Json::Kind::Array;
    for (std::int64_t index = 0; index < report.trades_len; ++index)
        if (!report.trades[index].open_at_end)
            closed.items.push_back(report_trade(report.trades[index]));
    const auto& curve = fields.at("equity_curve").items;
    return Json::object({
        {"schema_version", Json::string("pineforge-native-report/v1")},
        {"deployment", Json::string(deployment)},
        {"input_cursor", report_number(cursor)},
        {"state_hash", Json::string(std::to_string(state_hash))},
        {"equity", curve.empty() ? Json{} : curve.back().at("equity")},
        {"open_profit", curve.empty() ? Json{} : curve.back().at("open_profit")},
        {"report", std::move(fields)},
        {"closed_trades", std::move(closed)}});
}

}
