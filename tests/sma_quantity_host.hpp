// The SMA quantity reconstruction on the real source host: the retained
// Pine's schedule, NA gate and sizing expression, with the host/TA call
// pattern used by the generated-strategy fixtures in test_tv_strategy_defaults.
// Shared by the tests that drive it. This is a caller fixture, not a claim of
// codegen/TV compile agreement.
#pragma once

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace sma_quantity {
using namespace pineforge;
constexpr std::int64_t first_time = 1704067200000LL;
constexpr std::int64_t hour = 3600000;
constexpr int samples = 1441;
constexpr double quantity_step = 0.00001;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

inline void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}

inline std::string word(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof bits == sizeof value, "binary64 required");
    std::memcpy(&bits, &value, sizeof bits);
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << bits;
    return out.str();
}

inline std::string quoted(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2)
                             << std::setfill('0') << static_cast<unsigned>(c);
        else out << c;
    }
    out << '"';
    return out.str();
}

struct Observation {
    std::int64_t time = 0, local = 0;
    int index = 0;
    bool in_window = false, entry = false, close = false;
    double average = missing, previous = missing, request = missing;
    double position = missing, placement = missing;
    int placements = 0;
};

class SmaQuantityHost final : public source::PineStrategyHost {
public:
    explicit SmaQuantityHost(double offset, double grid = quantity_step)
        : offset_(offset) {
        attach_pine_execution_adapter();
        fixture_retain_all_events();
        source::PineStrategyConfig config{};
        config.initial_capital = 1000000000.0;
        config.pyramiding = 0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = config.margin_short = 100.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_string("tickerid", "BINANCE:BTCUSDT");
        set_syminfo_string("currency", "USDT");
        set_syminfo_string("basecurrency", "BTC");
        set_syminfo_type("crypto");
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("account_currency_fx", 1.0);
        if (grid > 0) set_syminfo_metadata("qty_step", grid);
    }

    void on_source_bar(const Bar& bar) override {
        const bool advance = history_advances_new_bar();
        Observation row{};
        row.time = bar.timestamp;
        row.index = pine_bar_index();
        row.local = (bar.timestamp - first_time) / hour;
        row.in_window = bar.timestamp >= first_time
            && bar.timestamp <= first_time + (samples - 1) * hour;
        const double source = row.in_window ? bar.close : missing;
        row.average = advance ? average_.compute(source) : average_.recompute(source);
        row.previous = observations.empty() ? missing
            : (advance ? observations.back().average : observations.back().previous);
        row.request = row.average * 0.001 + offset_ * quantity_step;
        row.position = signed_position_size();
        const bool slot = row.in_window && row.local >= 2 && row.local % 3 == 2
            && row.local + 2 < samples;
        row.entry = slot && row.position == 0;
        row.close = row.in_window && row.local % 3 == 0 && row.position > 0;
        if (row.entry) {
            strategy_entry("L", true, missing, missing, row.request);
            // Read-only placement observation; absence stays explicit.
            for (const auto& pending : source_pending_view()) {
                if (pending.id == "L" && pending.created_bar == row.index) {
                    ++row.placements;
                    row.placement = pending.qty;
                }
            }
        }
        if (row.close) strategy_close("L");
        if (advance || observations.empty()) observations.push_back(row);
        else observations.back() = row;
    }

    void emit() const {
        for (const auto& row : observations) {
            std::cout << "{\"kind\":\"bar\",\"index\":" << row.index
                      << ",\"time\":" << row.time << ",\"local\":" << row.local
                      << ",\"in_window\":" << row.in_window
                      << ",\"entry\":" << row.entry << ",\"close\":" << row.close
                      << ",\"sma\":\"" << word(row.average)
                      << "\",\"previous_sma\":\"" << word(row.previous)
                      << "\",\"request\":\"" << word(row.request)
                      << "\",\"position\":\"" << word(row.position)
                      << "\",\"placement_qty\":\"" << word(row.placement)
                      << "\",\"placements\":" << row.placements << "}\n";
        }
        for (int i = 0; i < report_trade_count(); ++i) {
            const auto trade = get_report_trade(i);
            std::cout << "{\"kind\":\"trade\",\"index\":" << i
                      << ",\"entry_time\":" << trade.entry_time
                      << ",\"exit_time\":" << trade.exit_time
                      << ",\"entry_bar\":" << trade.entry_bar_index
                      << ",\"exit_bar\":" << trade.exit_bar_index
                      << ",\"entry_id\":" << quoted(trade.entry_id)
                      << ",\"exit_id\":" << quoted(trade.exit_id)
                      << ",\"long\":" << trade.is_long
                      << ",\"open_at_end\":" << trade.open_at_end
                      << ",\"qty\":\"" << word(trade.qty)
                      << "\",\"entry_price\":\"" << word(trade.entry_price)
                      << "\",\"exit_price\":\"" << word(trade.exit_price)
                      << "\",\"pnl\":\"" << word(trade.pnl) << "\"}\n";
        }
        for (const auto& event : native_events(0)) {
            if (!event.command) continue;
            const auto& command = *event.command;
            std::cout << "{\"kind\":\"event\",\"ordinal\":" << event.ordinal
                      << ",\"variant\":" << command.index();
            if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&command)) {
                std::cout << ",\"type\":\"accepted\"";
                emit_request(accepted->request());
            } else if (const auto* rejected = std::get_if<native_order::RejectedEvent>(&command)) {
                std::cout << ",\"type\":\"rejected\",\"reason\":"
                          << static_cast<unsigned>(rejected->reason);
                emit_request(rejected->request);
            } else if (const auto* rejected = std::get_if<native_order::ReplaceRejectedEvent>(&command)) {
                std::cout << ",\"type\":\"replace_rejected\",\"reason\":"
                          << static_cast<unsigned>(rejected->reason);
                emit_request(rejected->attempted);
            } else if (const auto* rejected = std::get_if<native_order::MatchRejectedEvent>(&command)) {
                std::cout << ",\"type\":\"match_rejected\",\"reason\":"
                          << static_cast<unsigned>(rejected->reason)
                          << ",\"time\":" << rejected->cursor.point.effective_time_ms;
                emit_request(rejected->request());
            } else if (const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(&command)) {
                std::cout << ",\"type\":\"applied\",\"time\":" << applied->effective_time_ms()
                          << ",\"bar\":" << applied->interval_index()
                          << ",\"incarnation\":" << applied->handle().incarnation
                          << ",\"opened\":\"" << word(applied->opened_units)
                          << "\",\"closed\":\"" << word(applied->closed_units)
                          << "\",\"filled\":\"" << word(applied->filled_working)
                          << "\",\"raw_price\":\"" << word(applied->raw_price)
                          << "\",\"price\":\"" << word(applied->resolved_price) << '"';
                emit_request(applied->request());
            } else {
                // Preserve every other command's ordinal/type. Its payload is
                // not used as a quantity observation by this fixture.
                std::cout << ",\"type\":\"other\"";
            }
            std::cout << "}\n";
        }
        std::cout << "{\"kind\":\"summary\",\"status\":" << last_run_status()
                  << ",\"error\":" << quoted(last_error())
                  << ",\"bars\":" << observations.size()
                  << ",\"trades\":" << report_trade_count()
                  << ",\"pending\":" << pending_order_count()
                  << ",\"position\":\"" << word(signed_position_size())
                  << "\",\"grid\":\"" << word(qty_step_)
                  << "\",\"offset\":\"" << word(offset_)
                  << "\",\"capital\":\"" << word(initial_capital_)
                  << "\",\"account_fx\":\"" << word(account_currency_fx_)
                  << "\",\"quote_currency\":" << quoted(syminfo_.currency) << "}\n";
    }

    std::vector<Observation> observations;

private:
    static void emit_request(const native_order::Request& request) {
        std::cout << ",\"id\":" << quoted(request.label);
        if (const auto* units = std::get_if<native_order::Transact>(&request.intent))
            std::cout << ",\"transact\":\"" << word(units->signed_units) << '"';
    }

    ta::SMA average_{3};
    double offset_;
};

}  // namespace sma_quantity
