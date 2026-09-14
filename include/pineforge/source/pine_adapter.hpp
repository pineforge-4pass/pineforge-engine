#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/intraday_cap.hpp>
#include <pineforge/compat/pine/order_priority.hpp>

namespace pineforge::source {

inline constexpr char kSourceAdapterDomain[] = "pineforge-source-adapter/v1";

struct PineStrategyConfig {
    bool process_orders_on_close = false;
    bool calc_on_order_fills = false;
    double initial_capital = 1000000.0;
    int default_qty_type = static_cast<int>(QtyType::FIXED);
    double default_qty_value = 1.0;
    int pyramiding = 1;
    double commission_value = 0.0;
    int commission_type = static_cast<int>(CommissionType::PERCENT);
    int slippage = 0;
    double margin_long = 100.0;
    double margin_short = 100.0;
    bool close_entries_rule_any = false;
    bool src_series_active = false;
};

struct StrategyOverrides {
    double initial_capital = std::numeric_limits<double>::quiet_NaN();
    double commission_value = std::numeric_limits<double>::quiet_NaN();
    double default_qty_value = std::numeric_limits<double>::quiet_NaN();
    int pyramiding = -1;
    int slippage = -1;
    int commission_type = -1;
    int default_qty_type = -1;
    int process_orders_on_close = -1;
    int calc_on_order_fills = -1;
    int close_entries_rule = -1;
};

struct PineExecutionAdapter {
    explicit PineExecutionAdapter(
            compat::pine::CapAttachment attachment = compat::pine::CapAttachment::None)
        : cap(attachment) {}

    compat::pine::IntradayCap cap;
    compat::pine::OrderPriority priority;
    MarketAdmissionJournal admission_journal;
};

} // namespace pineforge::source
