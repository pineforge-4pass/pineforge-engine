#pragma once

#include "l4d_native_route_guard.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace pineforge::l8d_test {

inline constexpr double missing = std::numeric_limits<double>::quiet_NaN();

inline Bar point(double price, std::int64_t timestamp) {
    return {price, price, price, price, 1.0, timestamp};
}

inline source::PineStrategyConfig fixed_config(
        double capital = 10'000.0, double quantity = 1.0,
        int pyramiding = 10, bool process_on_close = false,
        bool calc_on_fills = false) {
    source::PineStrategyConfig config;
    config.initial_capital = capital;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = quantity;
    config.pyramiding = pyramiding;
    config.margin_long = 100.0;
    config.margin_short = 100.0;
    config.commission_type = static_cast<int>(CommissionType::PERCENT);
    config.commission_value = 0.0;
    config.slippage = 0;
    config.process_orders_on_close = process_on_close;
    config.calc_on_order_fills = calc_on_fills;
    return config;
}

inline std::vector<pf_pending_order_v1_t> pending_rows(pf_strategy_t strategy) {
    std::vector<pf_pending_order_v1_t> result;
    const int count = strategy_pending_orders_len(strategy);
    for (int index = 0; index < count; ++index) {
        pf_pending_order_v1_t row{};
        if (strategy_pending_order_get(strategy, index, &row, sizeof row) == 0)
            result.push_back(row);
    }
    return result;
}

inline const pf_pending_order_v1_t* find(
        const std::vector<pf_pending_order_v1_t>& rows, const char* id) {
    for (const auto& row : rows)
        if (std::strcmp(row.id, id) == 0) return &row;
    return nullptr;
}

inline bool near(double left, double right, double tolerance = 1e-12) {
    return std::fabs(left - right) <= tolerance;
}

inline std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof result == sizeof value, "binary64 expected");
    std::memcpy(&result, &value, sizeof result);
    return result;
}

} // namespace pineforge::l8d_test
