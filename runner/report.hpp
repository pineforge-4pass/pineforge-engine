#pragma once

#include "json.hpp"
#include "report_delta.hpp"
#include "../src/report_value_invalid.hpp"
#include "../src/live_report_patch.hpp"
#include <pineforge/pineforge.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <memory>
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

// Metrics alone may carry an explicit native NaN absence. Executed trade,
// summary and curve values have no such sentinel. Trace values keep their
// independent report_number representation above.
template<class Value>
inline Json report_result_number(Value value, const char* field,
                                 bool nullable = false, const char* prefix = nullptr) {
    if constexpr (!std::is_integral_v<Value>) {
        if (nullable && std::isnan(value)) return report_number(value);
        pineforge::report::finite_value(value, field, prefix);
    }
    return report_number(value);
}

inline Json report_trade(const pf_trade_t& trade) {
    Json result = Json::object({});
#define PF_REPORT_FIELD(field) result.members[#field] = report_result_number(trade.field, "trades." #field)
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

inline Json report_stats(const pf_trade_stats_t& stats, const char* prefix = "metrics.all") {
    Json result = Json::object({});
#define PF_REPORT_FIELD(field) result.members[#field] = report_result_number(stats.field, #field, true, prefix)
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
#define PF_REPORT_FIELD(field) result.members[#field] = report_result_number(stats.field, "metrics.equity." #field, true)
    PF_REPORT_FIELD(max_equity_drawdown); PF_REPORT_FIELD(max_equity_drawdown_pct);
    PF_REPORT_FIELD(max_equity_runup); PF_REPORT_FIELD(max_equity_runup_pct);
    PF_REPORT_FIELD(buy_hold_return); PF_REPORT_FIELD(buy_hold_return_pct);
    result.members["sharpe_monthly"] = report_result_number(
        stats.sharpe_monthly, "metrics.equity.sharpe_tv", true);
    result.members["sortino_monthly"] = report_result_number(
        stats.sortino_monthly, "metrics.equity.sortino_tv", true);
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

inline Json native_report_json(const pf_report_t& report,
                              const std::map<std::string, std::size_t>& starts = {}) {
    Json result = Json::object({});
    const auto array = [&](const std::string& name, const auto* values, std::int64_t count, auto serialize) {
        if (count < 0 || (count && !values)) throw std::runtime_error("invalid native report array");
        const auto found = starts.find(name);
        const auto offset = found == starts.end() ? 0 : std::min<std::size_t>(found->second, count);
        return report_array(values ? values + offset : nullptr, count - offset, serialize);
    };
#define PF_REPORT_FIELD(field) result.members[#field] = report_number(report.field)
    PF_REPORT_FIELD(total_trades); PF_REPORT_FIELD(trades_len);
    PF_REPORT_FIELD(input_bars_processed); PF_REPORT_FIELD(script_bars_processed);
    PF_REPORT_FIELD(security_feeds_total); PF_REPORT_FIELD(security_complete_total);
    PF_REPORT_FIELD(security_partial_total); PF_REPORT_FIELD(magnifier_sub_bars_total);
    PF_REPORT_FIELD(magnifier_sample_ticks_total); PF_REPORT_FIELD(input_tf_seconds);
    PF_REPORT_FIELD(script_tf_seconds); PF_REPORT_FIELD(script_tf_ratio);
    PF_REPORT_FIELD(needs_aggregation); PF_REPORT_FIELD(bar_magnifier_enabled);
    PF_REPORT_FIELD(security_diag_len); PF_REPORT_FIELD(trace_len); PF_REPORT_FIELD(trace_names_len);
    PF_REPORT_FIELD(equity_curve_len); PF_REPORT_FIELD(broker_state_hash_len);
#undef PF_REPORT_FIELD
    result.members["net_profit"] = report_result_number(report.net_profit, "summary.net_pnl");
    result.members["trades"] = array("trades", report.trades, report.trades_len, report_trade);
    result.members["equity_curve"] = array("equity_curve", report.equity_curve, report.equity_curve_len,
        [](const pf_equity_point_t& point) { return Json::object({
            {"time_ms", report_number(point.time_ms)},
            {"equity", report_result_number(point.equity, "equity_curve.equity")},
            {"open_profit", report_result_number(point.open_profit, "equity_curve.open_profit")}}); });
    result.members["security_diag"] = array("security_diag", report.security_diag, report.security_diag_len,
        [](const pf_security_diag_t& diag) { return Json::object({
            {"sec_id", report_number(diag.sec_id)}, {"feed_count", report_number(diag.feed_count)},
            {"complete_count", report_number(diag.complete_count)},
            {"partial_count", report_number(diag.partial_count)}}); });
    result.members["trace"] = array("trace", report.trace, report.trace_len,
        [](const pf_trace_entry_t& trace) { return Json::object({
            {"timestamp", report_number(trace.timestamp)}, {"bar_index", report_number(trace.bar_index)},
            {"name_id", report_number(trace.name_id)}, {"value", report_number(trace.value)}}); });
    result.members["trace_names"] = array("trace_names", report.trace_names, report.trace_names_len,
        [](const char* name) { return Json::string(name ? name : ""); });
    result.members["broker_state_hash"] = array("broker_state_hash", report.broker_state_hash, report.broker_state_hash_len,
        [](std::uint64_t hash) { return report_number(hash); });
    result.members["metrics"] = Json::object({{"all", report_stats(report.metrics.all)},
        {"longs", report_stats(report.metrics.longs, "metrics.longs")},
        {"shorts", report_stats(report.metrics.shorts, "metrics.shorts")},
        {"equity", report_equity_stats(report.metrics.equity)}});
    return result;
}

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

inline Json native_report_json(const pineforge::report::LiveReportPatch& patch) {
    // Reuse the scalar serializer with genuinely empty arrays. Only the
    // explicit patch views below ever index suffix storage.
    auto empty = patch.scalars;
    if (empty.trades || empty.equity_curve || empty.security_diag || empty.trace
        || empty.trace_names || empty.broker_state_hash || empty.trace_len)
        throw std::runtime_error("invalid carried report scalar block");
    empty.trades_len = empty.security_diag_len = 0;
    empty.trace_names_len = 0;
    empty.equity_curve_len = empty.broker_state_hash_len = 0;
    auto fields = native_report_json(empty);
    const auto array = [](const auto& suffix, auto serialize) {
        if (suffix.offset > suffix.logical_length
            || suffix.rows.size() != suffix.logical_length - suffix.offset)
            throw std::runtime_error("invalid carried report array patch");
        return report_array(suffix.rows.data(), static_cast<std::int64_t>(suffix.rows.size()), serialize);
    };
    fields.members["trades_len"] = report_number(patch.scalars.trades_len);
    fields.members["equity_curve_len"] = report_number(patch.scalars.equity_curve_len);
    fields.members["security_diag_len"] = report_number(patch.scalars.security_diag_len);
    fields.members["broker_state_hash_len"] = report_number(patch.scalars.broker_state_hash_len);
    fields.members["trace_names_len"] = report_number(patch.scalars.trace_names_len);
    fields.members["trades"] = array(patch.trades, report_trade);
    fields.members["equity_curve"] = array(patch.equity, [](const pf_equity_point_t& point) {
        return Json::object({{"time_ms", report_number(point.time_ms)},
            {"equity", report_result_number(point.equity, "equity_curve.equity")},
            {"open_profit", report_result_number(point.open_profit, "equity_curve.open_profit")}});
    });
    fields.members["security_diag"] = array(patch.security, [](const pf_security_diag_t& diag) {
        return Json::object({{"sec_id", report_number(diag.sec_id)},
            {"feed_count", report_number(diag.feed_count)},
            {"complete_count", report_number(diag.complete_count)},
            {"partial_count", report_number(diag.partial_count)}});
    });
    fields.members["broker_state_hash"] = array(patch.hashes, [](std::uint64_t hash) {
        return report_number(hash);
    });
    fields.members["trace_names"] = array(patch.trace_names, [](const std::string& name) {
        return Json::string(name);
    });
    return fields;
}

class ReportDeltas {
public:
    explicit ReportDeltas(bool retain_document = false)
        : document_(retain_document ? std::make_unique<Json>(Json::object({})) : nullptr) {}

    std::string update(const pf_report_t& report, const std::string& deployment,
                       std::uint64_t cursor, std::uint64_t state_hash) {
        if (report.trace_len != 0)
            throw std::runtime_error("cumulative report deltas require tracing to remain disabled");
        std::map<std::string, std::size_t> starts;
        for (const auto* name : {"equity_curve", "trace", "trace_names", "broker_state_hash"}) {
            const auto found = array_sizes_.find(name);
            const auto size = found == array_sizes_.end() ? 0 : found->second;
            starts[name] = size ? size - 1 : 0;
        }
        starts["trades"] = closed_trade_high_water_
            ? closed_trade_high_water_ - 1 : 0;
        starts["security_diag"] = 0;
        auto fields = native_report_json(report, starts);
        Json arrays = Json::object({});
        for (const auto* name : {"trades", "equity_curve", "security_diag", "trace", "trace_names", "broker_state_hash"}) {
            auto values = std::move(fields.members[name]);
            const auto current_size = [&]() -> std::size_t {
                if (name == std::string("trades"))
                    return static_cast<std::size_t>(report.trades_len);
                if (name == std::string("equity_curve"))
                    return static_cast<std::size_t>(report.equity_curve_len);
                if (name == std::string("security_diag"))
                    return static_cast<std::size_t>(report.security_diag_len);
                if (name == std::string("trace"))
                    return static_cast<std::size_t>(report.trace_len);
                if (name == std::string("trace_names"))
                    return static_cast<std::size_t>(report.trace_names_len);
                return static_cast<std::size_t>(report.broker_state_hash_len);
            }();
            const auto offset = std::min<std::size_t>(starts[name], current_size);
            arrays.members[name] = Json::object({{"offset", report_number(offset)}, {"items", std::move(values)}});
            fields.members.erase(name);
        }
        Json closed;
        closed.kind = Json::Kind::Array;
        if (closed_trade_high_water_ > 0) {
            if (closed_trade_high_water_ > static_cast<std::size_t>(report.trades_len)
                || report.trades[closed_trade_high_water_ - 1].open_at_end)
                throw std::runtime_error("cumulative report closed-trade prefix regressed");
            ++closed_trade_visits_;
            closed.items.push_back(report_trade(report.trades[closed_trade_high_water_ - 1]));
        }
        for (std::size_t index = closed_trade_high_water_;
             index < static_cast<std::size_t>(report.trades_len); ++index) {
            ++closed_trade_visits_;
            if (report.trades[index].open_at_end)
                break;
            closed.items.push_back(report_trade(report.trades[index]));
            ++closed_trade_high_water_;
        }
        arrays.members["closed_trades"] = Json::object({
            {"offset", report_number(std::min(starts["trades"], closed_trade_high_water_))},
            {"items", std::move(closed)}});
        const auto& curve = arrays.at("equity_curve").at("items").items;
        auto next = Json::object({{"schema_version", Json::string("pineforge-native-report/v1")},
            {"deployment", Json::string(deployment)}, {"input_cursor", report_number(cursor)},
            {"state_hash", Json::string(std::to_string(state_hash))},
            {"equity", curve.empty() ? Json{} : curve.back().at("equity")},
            {"open_profit", curve.empty() ? Json{} : curve.back().at("open_profit")}, {"report", std::move(fields)}});
        auto delta = Json::object({{"schema_version", Json::string("pineforge-native-report-delta/v1")},
            {"fields", changed_report_fields(scalars_, next)}, {"arrays", std::move(arrays)}});
        scalars_ = std::move(next);
        if (document_)
            apply_report_delta(*document_, delta);
        array_sizes_["trades"] = static_cast<std::size_t>(report.trades_len);
        array_sizes_["equity_curve"] = static_cast<std::size_t>(report.equity_curve_len);
        array_sizes_["security_diag"] = static_cast<std::size_t>(report.security_diag_len);
        array_sizes_["trace"] = static_cast<std::size_t>(report.trace_len);
        array_sizes_["trace_names"] = static_cast<std::size_t>(report.trace_names_len);
        array_sizes_["broker_state_hash"] = static_cast<std::size_t>(report.broker_state_hash_len);
        return delta.dump();
    }
    std::string update(const pineforge::report::LiveReportPatch& patch,
                       const std::string& deployment, std::uint64_t cursor,
                       std::uint64_t state_hash) {
        const auto expected_start = [&](const char* name) {
            const auto found = array_sizes_.find(name);
            return found == array_sizes_.end() || !found->second ? 0 : found->second - 1;
        };
        if (patch.trades.offset != (closed_trade_high_water_ ? closed_trade_high_water_ - 1 : 0)
            || patch.equity.offset != std::min(expected_start("equity_curve"), patch.equity.logical_length)
            || patch.hashes.offset != std::min(expected_start("broker_state_hash"), patch.hashes.logical_length)
            || patch.trace_names.offset != std::min(expected_start("trace_names"), patch.trace_names.logical_length)
            || patch.security.offset != 0
            || patch.closed_trades < closed_trade_high_water_
            || patch.closed_trades > patch.trades.logical_length
            || patch.scalars.trades_len < 0
            || static_cast<std::size_t>(patch.scalars.trades_len) != patch.trades.logical_length
            || patch.scalars.equity_curve_len < 0
            || static_cast<std::size_t>(patch.scalars.equity_curve_len) != patch.equity.logical_length
            || patch.scalars.security_diag_len < 0
            || static_cast<std::size_t>(patch.scalars.security_diag_len) != patch.security.logical_length
            || patch.scalars.broker_state_hash_len < 0
            || static_cast<std::size_t>(patch.scalars.broker_state_hash_len) != patch.hashes.logical_length
            || patch.scalars.trace_names_len < 0
            || static_cast<std::size_t>(patch.scalars.trace_names_len) != patch.trace_names.logical_length)
            throw std::runtime_error("cumulative carried report prefix or length regressed");
        auto fields = native_report_json(patch);
        Json arrays = Json::object({});
        const auto put = [&](const char* name, std::size_t offset, std::size_t length) {
            arrays.members[name] = Json::object({
                {"offset", report_number(offset)}, {"items", std::move(fields.members[name])}});
            fields.members.erase(name);
            array_sizes_[name] = length;
        };
        put("trades", patch.trades.offset, patch.trades.logical_length);
        put("equity_curve", patch.equity.offset, patch.equity.logical_length);
        put("security_diag", patch.security.offset, patch.security.logical_length);
        put("broker_state_hash", patch.hashes.offset, patch.hashes.logical_length);
        put("trace", 0, 0);
        put("trace_names", patch.trace_names.offset, patch.trace_names.logical_length);
        Json closed;
        closed.kind = Json::Kind::Array;
        for (std::size_t i = 0; i < patch.trades.rows.size(); ++i) {
            ++closed_trade_visits_;
            const auto absolute = patch.trades.offset + i;
            if (absolute >= patch.closed_trades) {
                if (!patch.trades.rows[i].open_at_end)
                    throw std::runtime_error("invalid carried report open-trade boundary");
                break;
            }
            if (patch.trades.rows[i].open_at_end)
                throw std::runtime_error("cumulative report closed-trade prefix regressed");
            closed.items.push_back(report_trade(patch.trades.rows[i]));
        }
        closed_trade_high_water_ = patch.closed_trades;
        arrays.members["closed_trades"] = Json::object({
            {"offset", report_number(std::min(patch.trades.offset, patch.closed_trades))},
            {"items", std::move(closed)}});
        const auto& curve = arrays.at("equity_curve").at("items").items;
        auto next = Json::object({{"schema_version", Json::string("pineforge-native-report/v1")},
            {"deployment", Json::string(deployment)}, {"input_cursor", report_number(cursor)},
            {"state_hash", Json::string(std::to_string(state_hash))},
            {"equity", curve.empty() ? Json{} : curve.back().at("equity")},
            {"open_profit", curve.empty() ? Json{} : curve.back().at("open_profit")},
            {"report", std::move(fields)}});
        auto delta = Json::object({{"schema_version", Json::string("pineforge-native-report-delta/v1")},
            {"fields", changed_report_fields(scalars_, next)}, {"arrays", std::move(arrays)}});
        if (patch.interval_ratios) {
            delta.members["ratios_as_of_input"] = report_number(patch.ratios_as_of_input);
            // Export needs the same civil-month clock as the live ratio pass.
            delta.members["ratio_context"] = Json::object({
                {"chart_timezone", Json::string(patch.chart_timezone)}});
        }
        scalars_ = std::move(next);
        if (document_) apply_report_delta(*document_, delta);
        return delta.dump();
    }
    std::string json() const {
        if (!document_)
            throw std::runtime_error("full cumulative report requires --report-jsonl=full");
        return document_->dump();
    }
    std::string full_json(const pf_report_t& report, const std::string& deployment,
                          std::uint64_t cursor, std::uint64_t state_hash) const {
        return native_report_document(report, deployment, cursor, state_hash).dump();
    }
    std::size_t closed_trade_visits() const noexcept { return closed_trade_visits_; }
private:
    std::unique_ptr<Json> document_;
    Json scalars_ = Json::object({});
    std::map<std::string, std::size_t> array_sizes_;
    std::size_t closed_trade_high_water_ = 0;
    std::size_t closed_trade_visits_ = 0;
};

}
