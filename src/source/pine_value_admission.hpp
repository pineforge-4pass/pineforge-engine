#pragma once

// Selected-window value ownership, source side (revision 3 section 4, Appendix B).
// The one place a source-owned invalid value becomes strategy_value_invalid
// (class strategy). Header only: the lanes that add the admission rules put
// their code in new .cpp/.hpp files of their own and call these functions.
// `field` is ALWAYS one of the engine-owned literals of namespace value_field
// below, never text a script computed; the ownership manifest enumerates this
// list. Not installed API.

#include <pineforge/run_failure.hpp>

#include <stdexcept>

namespace pineforge::source {

enum class StrategyValueReason { nonfinite, invalid_unicode, numeric_overflow };

inline const char* strategy_value_reason_name(StrategyValueReason reason) noexcept {
    switch (reason) {
        case StrategyValueReason::nonfinite: return "nonfinite";
        case StrategyValueReason::invalid_unicode: return "invalid_unicode";
        case StrategyValueReason::numeric_overflow: return "numeric_overflow";
    }
    return "nonfinite";
}

/// The coded failure of a source-owned invalid actionable value. Thrown, so the
/// existing exception path classifies it (RunFailureInfo wins over the type).
[[noreturn]] inline void throw_strategy_value_invalid(const char* field, StrategyValueReason reason) {
    throw coded<std::invalid_argument>(
        RunFailureCode::strategy_value_invalid,
        RunFailureArgs{{"field", field}, {"reason", strategy_value_reason_name(reason)}},
        "the strategy supplied an invalid execution value");
}

/// The closed vocabulary of `field`: "<sink>.<argument>". One constant per
/// admitted argument of each sink of revision 3 Appendix B. A lane that needs a
/// name not listed here stops and reports; it does not invent one.
namespace value_field {
// strategy.entry
inline constexpr const char* entry_id = "entry.id";
inline constexpr const char* entry_limit_price = "entry.limit_price";
inline constexpr const char* entry_stop_price = "entry.stop_price";
inline constexpr const char* entry_qty = "entry.qty";
inline constexpr const char* entry_comment = "entry.comment";
inline constexpr const char* entry_oca_name = "entry.oca_name";
// strategy.order
inline constexpr const char* order_id = "order.id";
inline constexpr const char* order_limit_price = "order.limit_price";
inline constexpr const char* order_stop_price = "order.stop_price";
inline constexpr const char* order_qty = "order.qty";
inline constexpr const char* order_comment = "order.comment";
inline constexpr const char* order_oca_name = "order.oca_name";
// strategy.exit
inline constexpr const char* exit_id = "exit.id";
inline constexpr const char* exit_from_entry = "exit.from_entry";
inline constexpr const char* exit_limit_price = "exit.limit_price";
inline constexpr const char* exit_stop_price = "exit.stop_price";
inline constexpr const char* exit_trail_points = "exit.trail_points";
inline constexpr const char* exit_trail_offset = "exit.trail_offset";
inline constexpr const char* exit_trail_price = "exit.trail_price";
inline constexpr const char* exit_qty_percent = "exit.qty_percent";
inline constexpr const char* exit_qty = "exit.qty";
inline constexpr const char* exit_comment = "exit.comment";
inline constexpr const char* exit_oca_name = "exit.oca_name";
inline constexpr const char* exit_profit_ticks = "exit.profit_ticks";
inline constexpr const char* exit_loss_ticks = "exit.loss_ticks";
// strategy.close / close_all / cancel / cancel_all / exit_cancel_bracket
inline constexpr const char* close_id = "close.id";
inline constexpr const char* close_comment = "close.comment";
inline constexpr const char* close_qty = "close.qty";
inline constexpr const char* close_qty_percent = "close.qty_percent";
inline constexpr const char* exit_cancel_bracket_exit_id = "exit_cancel_bracket.exit_id";
inline constexpr const char* exit_cancel_bracket_from_entry = "exit_cancel_bracket.from_entry";
inline constexpr const char* exit_cancel_bracket_comment = "exit_cancel_bracket.comment";
inline constexpr const char* cancel_id = "cancel.id";
// the six risk setters
inline constexpr const char* risk_direction = "risk.direction";
inline constexpr const char* risk_max_cons_loss_days = "risk.max_cons_loss_days";
inline constexpr const char* risk_max_drawdown = "risk.max_drawdown";
inline constexpr const char* risk_max_intraday_loss = "risk.max_intraday_loss";
inline constexpr const char* risk_max_intraday_filled_orders = "risk.max_intraday_filled_orders";
inline constexpr const char* risk_max_position_size = "risk.max_position_size";
// strategy() declaration (PineStrategyConfig / StrategyOverrides)
inline constexpr const char* strategy_initial_capital = "strategy.initial_capital";
inline constexpr const char* strategy_default_qty_value = "strategy.default_qty_value";
inline constexpr const char* strategy_commission_value = "strategy.commission_value";
inline constexpr const char* strategy_margin_long = "strategy.margin_long";
inline constexpr const char* strategy_margin_short = "strategy.margin_short";
inline constexpr const char* strategy_default_qty_type = "strategy.default_qty_type";
inline constexpr const char* strategy_commission_type = "strategy.commission_type";
inline constexpr const char* strategy_pyramiding = "strategy.pyramiding";
inline constexpr const char* strategy_slippage = "strategy.slippage";
// a bound input value (declaration default or supplied value)
inline constexpr const char* input_value = "input.value";
}  // namespace value_field

}  // namespace pineforge::source
