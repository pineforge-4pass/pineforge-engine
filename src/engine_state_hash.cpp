// Broker-state hash generic half. Source policy state is folded by the
// PineStrategyHost extension in src/source/pine_state_hash.cpp.
#include "engine_internal.hpp"
#include "broker_state_hash_internal.hpp"

namespace pineforge {

void BacktestEngine::hash_source_extension(BrokerStateHashSink& sink) const {
    sink.s("source:none");
}

uint64_t BacktestEngine::broker_state_hash() const {
    BrokerStateHashSink f;
    f.s("pineforge-broker-state/v16");
    f.u(execution_consumer().continuation_hash());

    // --- Position core ---
    f.i(static_cast<int64_t>(position_side_));
    f.d(position_entry_price_);
    f.i(position_entry_time_);
    f.d(position_qty_);
    f.i(position_entry_count_);
    f.i(position_open_bar_);
    f.i(position_cycle_seq_);
    f.i(next_position_cycle_seq_);

    f.b(opening_obligations_.pending());
    if (const auto& receipt = opening_obligations_.peek()) {
        const auto& owner = receipt->owner();
        f.i(owner.positionCycle);
        f.u(owner.producerFill);
        f.u(owner.orderIncarnation);
        f.i(owner.barIndex);
        f.i(owner.timestamp);
        f.i(static_cast<int64_t>(receipt->decision()));
        f.b(receipt->requires_adverse_pass());
        f.d(receipt->raw_fill_base());
    }

    // --- Physical lots ---
    f.u(pyramid_entries_.size());
    for (const auto& e : pyramid_entries_) {
        f.d(e.price); f.i(e.time); f.d(e.qty); f.s(e.entry_id);
        f.i(static_cast<int64_t>(e.entry_bar_index));
        f.s(e.entry_comment);
        f.d(e.max_runup); f.d(e.max_drawdown);
        f.b(e.skip_entry_bar_high); f.b(e.skip_entry_bar_low);
        f.b(e.market_pyramid_add);
        f.d(e.entry_path_position);
        f.d(e.entry_commission_account);
        f.u(e.entry_incarnation);
        f.b(e.bracket_slot_shadowed);
        f.b(e.ordinary_market_open);
        f.b(e.pooc_terminal_market_entry);
        f.b(e.ordinary_stop_open);
    }

    // A31 residue: these stage-coordinator inputs remain physical/generic.
    f.b(fold_exit_path_extremes_);
    f.d(fold_exit_trail_peak_);
    f.d(trail_best_price_);

    f.b(position_close_obligation_.pending());
    if (const auto& request = position_close_obligation_.peek()) {
        f.u(request->action_id);
        f.i(request->position_cycle);
        f.i(request->after_bar);
        f.s(request->comment);
    }

    f.d(net_profit_sum_);
    f.d(net_profit_roundoff_bound_);
    f.d(net_profit_roundoff_value_);

    f.d(max_equity_);
    f.d(max_drawdown_);
    f.d(min_equity_);

    f.u(next_order_incarnation_);
    f.u(broker_fill_event_seq_);

    f.b(account_currency_fx_broker_epoch_initialized_);
    f.u(static_cast<uint64_t>(account_currency_fx_broker_epoch_));
    f.d(account_currency_fx_broker_rate_);

    f.d(gross_profit_sum_);
    f.d(gross_loss_sum_);
    f.i(win_trades_count_);
    f.i(loss_trades_count_);
    f.i(eventrades_count_);
    f.d(max_runup_);
    f.d(max_contracts_held_all_);
    f.d(max_contracts_held_long_);
    f.d(max_contracts_held_short_);

    f.u(trades_.size());
    for (const auto& t : trades_) {
        f.i(t.entry_time); f.i(t.exit_time);
        f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.qty); f.d(t.pnl);
    }

    hash_source_extension(f);
    return f.h;
}

} // namespace pineforge
