#pragma once

#include "json.hpp"
#include <pineforge/pineforge.h>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>

namespace pineforge::live {

template<class Value> inline Json report_number(Value value) {
    if constexpr (std::is_integral_v<Value>) {
        return Json::number(std::to_string(value));
    } else {
        if (std::isnan(value)) return Json::string("NaN");
        if (std::isinf(value)) return Json::string(value < 0 ? "-Infinity" : "Infinity");
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::setprecision(17) << value;
        return Json::number(text.str());
    }
}

inline Json report_trade(const pf_trade_t& trade) {
    Json result = Json::object({});
#define PF_REPORT_FIELD(field) result.members[#field] = report_number(trade.field)
    PF_REPORT_FIELD(entry_time); PF_REPORT_FIELD(exit_time);
    PF_REPORT_FIELD(entry_price); PF_REPORT_FIELD(exit_price);
    PF_REPORT_FIELD(pnl); PF_REPORT_FIELD(pnl_pct); PF_REPORT_FIELD(is_long);
    PF_REPORT_FIELD(max_runup); PF_REPORT_FIELD(max_drawdown);
    PF_REPORT_FIELD(qty); PF_REPORT_FIELD(commission);
    PF_REPORT_FIELD(entry_bar_index); PF_REPORT_FIELD(exit_bar_index);
    PF_REPORT_FIELD(open_at_end);
#undef PF_REPORT_FIELD
    return result;
}

inline Json report_stats(const pf_trade_stats_t& stats) {
    Json result = Json::object({});
#define PF_REPORT_FIELD(field) result.members[#field] = report_number(stats.field)
    PF_REPORT_FIELD(num_trades); PF_REPORT_FIELD(num_wins); PF_REPORT_FIELD(num_losses);
    PF_REPORT_FIELD(num_even); PF_REPORT_FIELD(percent_profitable);
    PF_REPORT_FIELD(net_profit); PF_REPORT_FIELD(net_profit_pct);
    PF_REPORT_FIELD(gross_profit); PF_REPORT_FIELD(gross_profit_pct);
    PF_REPORT_FIELD(gross_loss); PF_REPORT_FIELD(gross_loss_pct);
    PF_REPORT_FIELD(profit_factor); PF_REPORT_FIELD(avg_trade); PF_REPORT_FIELD(avg_trade_pct);
    PF_REPORT_FIELD(avg_win); PF_REPORT_FIELD(avg_win_pct);
    PF_REPORT_FIELD(avg_loss); PF_REPORT_FIELD(avg_loss_pct);
    PF_REPORT_FIELD(ratio_avg_win_avg_loss); PF_REPORT_FIELD(largest_win);
    PF_REPORT_FIELD(largest_win_pct); PF_REPORT_FIELD(largest_loss);
    PF_REPORT_FIELD(largest_loss_pct); PF_REPORT_FIELD(commission_paid);
    PF_REPORT_FIELD(expectancy); PF_REPORT_FIELD(max_consecutive_wins);
    PF_REPORT_FIELD(max_consecutive_losses); PF_REPORT_FIELD(avg_bars_in_trade);
    PF_REPORT_FIELD(avg_bars_in_wins); PF_REPORT_FIELD(avg_bars_in_losses);
#undef PF_REPORT_FIELD
    return result;
}

inline Json report_equity_stats(const pf_equity_stats_t& stats) {
    Json result = Json::object({});
#define PF_REPORT_FIELD(field) result.members[#field] = report_number(stats.field)
    PF_REPORT_FIELD(max_equity_drawdown); PF_REPORT_FIELD(max_equity_drawdown_pct);
    PF_REPORT_FIELD(max_equity_runup); PF_REPORT_FIELD(max_equity_runup_pct);
    PF_REPORT_FIELD(buy_hold_return); PF_REPORT_FIELD(buy_hold_return_pct);
    PF_REPORT_FIELD(sharpe_monthly); PF_REPORT_FIELD(sortino_monthly);
    PF_REPORT_FIELD(sharpe_bar); PF_REPORT_FIELD(sortino_bar); PF_REPORT_FIELD(cagr);
    PF_REPORT_FIELD(calmar); PF_REPORT_FIELD(recovery_factor);
    PF_REPORT_FIELD(time_in_market_pct); PF_REPORT_FIELD(open_pl);
#undef PF_REPORT_FIELD
    return result;
}

template<class Value, class Serialize>
inline Json report_array(const Value* values, std::int64_t count, Serialize serialize) {
    if (count < 0 || (count && !values)) throw std::runtime_error("invalid native report array");
    Json result;
    result.kind = Json::Kind::Array;
    for (std::int64_t index = 0; index < count; ++index)
        result.items.push_back(serialize(values[index]));
    return result;
}

inline Json native_report_json(const pf_report_t& report) {
    Json result = Json::object({});
#define PF_REPORT_FIELD(field) result.members[#field] = report_number(report.field)
    PF_REPORT_FIELD(total_trades); PF_REPORT_FIELD(trades_len); PF_REPORT_FIELD(net_profit);
    PF_REPORT_FIELD(input_bars_processed); PF_REPORT_FIELD(script_bars_processed);
    PF_REPORT_FIELD(security_feeds_total); PF_REPORT_FIELD(security_complete_total);
    PF_REPORT_FIELD(security_partial_total); PF_REPORT_FIELD(magnifier_sub_bars_total);
    PF_REPORT_FIELD(magnifier_sample_ticks_total); PF_REPORT_FIELD(input_tf_seconds);
    PF_REPORT_FIELD(script_tf_seconds); PF_REPORT_FIELD(script_tf_ratio);
    PF_REPORT_FIELD(needs_aggregation); PF_REPORT_FIELD(bar_magnifier_enabled);
    PF_REPORT_FIELD(security_diag_len); PF_REPORT_FIELD(trace_len); PF_REPORT_FIELD(trace_names_len);
    PF_REPORT_FIELD(equity_curve_len); PF_REPORT_FIELD(broker_state_hash_len);
#undef PF_REPORT_FIELD
    result.members["trades"] = report_array(report.trades, report.trades_len, report_trade);
    result.members["equity_curve"] = report_array(report.equity_curve, report.equity_curve_len,
        [](const pf_equity_point_t& point) { return Json::object({
            {"time_ms", report_number(point.time_ms)}, {"equity", report_number(point.equity)},
            {"open_profit", report_number(point.open_profit)}}); });
    result.members["security_diag"] = report_array(report.security_diag, report.security_diag_len,
        [](const pf_security_diag_t& diag) { return Json::object({
            {"sec_id", report_number(diag.sec_id)}, {"feed_count", report_number(diag.feed_count)},
            {"complete_count", report_number(diag.complete_count)},
            {"partial_count", report_number(diag.partial_count)}}); });
    result.members["trace"] = report_array(report.trace, report.trace_len,
        [](const pf_trace_entry_t& trace) { return Json::object({
            {"timestamp", report_number(trace.timestamp)}, {"bar_index", report_number(trace.bar_index)},
            {"name_id", report_number(trace.name_id)}, {"value", report_number(trace.value)}}); });
    result.members["trace_names"] = report_array(report.trace_names, report.trace_names_len,
        [](const char* name) { return Json::string(name ? name : ""); });
    result.members["broker_state_hash"] = report_array(report.broker_state_hash, report.broker_state_hash_len,
        [](std::uint64_t hash) { return report_number(hash); });
    result.members["metrics"] = Json::object({{"all", report_stats(report.metrics.all)},
        {"longs", report_stats(report.metrics.longs)}, {"shorts", report_stats(report.metrics.shorts)},
        {"equity", report_equity_stats(report.metrics.equity)}});
    return result;
}

}
