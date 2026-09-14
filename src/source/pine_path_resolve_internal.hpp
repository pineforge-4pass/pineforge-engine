#pragma once

// Source-private path predicates.  The generic engine kernel deliberately
// does not expose declarations whose signatures carry the Pine PendingOrder
// type; source translation units and focused source tests include this header
// explicitly instead.
#include "../engine_internal.hpp"
#include <pineforge/source/pine_pending_intent.hpp>

namespace pineforge::internal {

bool opposing_stop_entry_hits_first(
    const Bar& bar, const std::vector<source::PendingOrder>& orders,
    std::size_t current_idx, int current_bar_index = -1);
bool opposing_stop_entry_hits_first(
    const Bar& bar, bool high_first,
    const std::vector<source::PendingOrder>& orders,
    std::size_t current_idx, int current_bar_index);

DualEntryStopPathWinner dual_entry_stop_path_winner(
    const Bar& bar, const std::vector<source::PendingOrder>& orders,
    int current_bar_index = -1);
DualEntryStopPathWinner dual_entry_stop_path_winner(
    const Bar& bar, bool high_first,
    const std::vector<source::PendingOrder>& orders,
    int current_bar_index);

bool dual_stop_margin_decline_can_continue_path(
    const std::vector<source::PendingOrder>& orders,
    DualEntryStopPathWinner winner,
    bool process_orders_on_close, bool calc_on_order_fills,
    bool bar_magnifier);

bool exit_order_touch_position(
    const Bar& bar, const source::PendingOrder& order,
    PositionSide pos, double* out_pos);
bool exit_order_touch_position(
    const Bar& bar, bool high_first, const source::PendingOrder& order,
    PositionSide pos, double* out_pos);

bool oca_exit_sibling_hits_first(
    const Bar& bar, const std::vector<source::PendingOrder>& orders,
    std::size_t current_idx, PositionSide pos);
bool oca_exit_sibling_hits_first(
    const Bar& bar, bool high_first,
    const std::vector<source::PendingOrder>& orders,
    std::size_t current_idx, PositionSide pos);

bool order_is_exit_style(const source::PendingOrder& order, PositionSide pos);

double exit_order_earliest_path_metric_no_trail(
    const Bar& bar, const source::PendingOrder& order,
    PositionSide position_side, bool is_entry_bar,
    double position_entry_price, int64_t position_cycle = 0,
    int64_t bar_index = 0);
double exit_order_earliest_path_metric_no_trail(
    const Bar& bar, bool high_first, const source::PendingOrder& order,
    PositionSide position_side, bool is_entry_bar,
    double position_entry_price, int64_t position_cycle = 0,
    int64_t bar_index = 0);

} // namespace pineforge::internal
