#pragma once

#include "execution.hpp"
#include <cstddef>
#include <cstdint>

namespace pineforge::execution {
inline namespace settlement_projection_v1 {

// Non-applying physical+account quote of one Action+Fill against the live book.
// Destroyed when the caller is done sizing. Not commit authority.
struct AccountEffectProjection {
    Status status = Status::NoEffect;

    // Same physical aggregates SettlementInspection reports for this Action.
    double closed_units = 0.0;
    double opened_units = 0.0;
    double resulting_abs_units = 0.0;
    std::size_t resulting_lot_count = 0;
    double resulting_abs_notional = 0.0;
    double current_ticket = 0.0;
    bool would_open = false;
    bool incoming_short = false;

    // Account facts SettlementInspection lacks. Units are account currency.
    // Copy net_profit_sum_, add each projected close-row pnl in roster order,
    // then add initial_capital_. Do not sum rows first or regroup.
    double realized_balance = 0.0;
    // All paid costs on the projected resulting open book, including a
    // prospective opening lot's allocated current-ticket share. 0 when empty.
    double remaining_entry_cost = 0.0;
    // Start from realized_balance; add each resulting lot's marked pnl minus
    // its paid remaining entry cost in roster order at fill.price. Do not
    // subtract opening cost twice.
    double marked_equity = 0.0;
    // 0 if projected book empty. Else the live position_cycle_seq_ when
    // survivors remain. If would_open from flat or after emptying the old
    // side, the peeked next_position_cycle_seq_ (NOT consumed).
    std::int64_t cycle_after = 0;
    // Signed resulting units (short negative). 0 when empty.
    double signed_units_after = 0.0;
};

} // inline namespace settlement_projection_v1
} // namespace pineforge::execution
