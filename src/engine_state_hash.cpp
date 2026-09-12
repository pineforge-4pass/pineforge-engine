// Broker-state hash for the live runtime's G1 check (spec §3.4). FNV-1a 64
// over the enumerated broker state (task-5-brief.md); unordered containers
// are hashed in SORTED order so two engines in equal broker state hash
// equally regardless of insertion history. Scope: every piece of state that
// decides the outcome of a FUTURE fill — position, resting-order book,
// pyramid lots, trail scalars, cycle/intraday/risk latches, frozen sizing
// snapshots, and equity sums that gate risk latches. Pure report/diagnostic
// accumulators, engine configuration set once from strategy()/metadata, and
// scratch that a bar-index/eval guard makes dead before it is ever read
// again are deliberately NOT hashed — see scripts/broker_state_hash_waivers.txt
// for the full list and the reason for each.
#include "engine_internal.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pineforge {
namespace {

struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    void d(double v) {
        if (v == 0.0) v = 0.0;                                  // -0.0 == 0.0
        // Canonicalise every NaN to one bit pattern: two producers of "not
        // set" (e.g. 0.0/0.0 vs quiet_NaN(), or a sign-flipped NaN) must not
        // hash differently when the broker-visible meaning ("not set") is
        // identical.
        if (v != v) v = std::numeric_limits<double>::quiet_NaN();
        bytes(&v, sizeof v);
    }
    void i(int64_t v) { bytes(&v, sizeof v); }
    void u(uint64_t v) { bytes(&v, sizeof v); }
    void b(bool v) { const unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void s(const std::string& v) { u(v.size()); bytes(v.data(), v.size()); }
};

void hash_admission_field(Fnv& f,const admission::Field& field) {
    f.s(field.path);f.u(field.value.index());
    std::visit([&](const auto& value){
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,uint64_t>)f.u(value);
        else if constexpr(std::is_same_v<T,int64_t>)f.i(value);
        else if constexpr(std::is_same_v<T,double>)f.d(value);
        else f.s(value);
    },field.value);
}

// unordered_map<string, double>, hashed in key-sorted order.
void hash_str_double_map(Fnv& f, const std::unordered_map<std::string, double>& m) {
    std::vector<std::pair<std::string, double>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    f.u(v.size());
    for (const auto& kv : v) { f.s(kv.first); f.d(kv.second); }
}

// unordered_map<uint64_t, unordered_map<string, double>>: sort outer keys,
// then delegate each inner map to hash_str_double_map (already sorted).
void hash_token_owned_map(
    Fnv& f,
    const std::unordered_map<uint64_t, std::unordered_map<std::string, double>>& m) {
    std::vector<uint64_t> keys;
    keys.reserve(m.size());
    for (const auto& kv : m) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    f.u(keys.size());
    for (uint64_t k : keys) {
        f.u(k);
        hash_str_double_map(f, m.at(k));
    }
}

void hash_str_set(Fnv& f, const std::unordered_set<std::string>& s) {
    std::vector<std::string> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    f.u(v.size());
    for (const auto& x : v) f.s(x);
}

}  // namespace

uint64_t BacktestEngine::broker_state_hash() const {
    Fnv f;
    // v6 includes resolved exit-leg owner/coordinates and immutable Pine placement evidence.
    // It is a serialization boundary, independent of the public C ABI version.
    f.s("pineforge-broker-state/v13");
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

    // Owned opening checkpoint. Serialize semantic fields, never variant
    // storage or padding. Version 2 deliberately retires four dead labels.
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

    // --- Physical lots: every PyramidEntry field is continuation state ---
    // Comments/excursions are Pine-readable; the other provenance fields
    // choose financial or same-bar path transitions. Preserve explicit typed
    // folds: never hash struct padding, string storage or native object bytes.
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
        // Physical-lot provenance (task-7 carried ruling): bracket
        // ownership and same-id replacement rules match lots by the
        // incarnation of the PendingOrder that filled them, not by entry_id.
        f.u(e.entry_incarnation);
        f.b(e.bracket_slot_shadowed);
        f.b(e.ordinary_market_open);
        f.b(e.pooc_terminal_market_entry);
        f.b(e.ordinary_stop_open);
    }

    // cycle_filled_entry_ids_ is std::set<string>: already ordered.
    f.u(cycle_filled_entry_ids_.size());
    for (const auto& id : cycle_filled_entry_ids_) f.s(id);

    // Per-entry-id unclosed-quantity ledger (strategy.close(id) FIFO sizing).
    hash_str_double_map(f, id_unclosed_qty_);

    // --- Cross-bar strategy.close replacement/reservation ledgers ---
    // (the per-source-bar batching scratch that feeds these is waived; these
    // two pairs are the durable provenance that survives across bars.)
    hash_str_double_map(f, close_reserved_qty_);
    hash_str_double_map(f, close_two_call_first_qty_);
    hash_token_owned_map(f, callsite_close_reserved_qty_);
    hash_token_owned_map(f, callsite_close_two_call_first_qty_);

    // --- KI-64 POOC script-visible position freeze snapshot ---
    // While armed, an ordinary POOC strategy.close(qty_percent) sizes against
    // this snapshot instead of the live position.
    f.i(pos_view_freeze_bar_);
    f.i(static_cast<int64_t>(pos_view_frozen_side_));
    f.d(pos_view_frozen_qty_);
    hash_str_double_map(f, pos_view_frozen_entry_qty_);

    // Cumulative same-on_bar strategy.close/close_all qty; read by a later
    // strategy.entry in the same on_bar to compute tv_carry_qty (sizing).
    f.d(pending_close_qty_in_bar_);

    // --- Resting order book ---
    f.u(pending_orders_.size());
    for (const auto& o : pending_orders_) {
        f.s(o.id); f.s(o.from_entry); f.i(static_cast<int64_t>(o.type)); f.b(o.is_long);
        o.legs.visit(f); f.d(o.qty);
        f.i(static_cast<int64_t>(o.qty_type)); f.d(o.qty_percent); f.s(o.oca_name);
        f.i(static_cast<int64_t>(o.oca_type));
        f.i(static_cast<int64_t>(o.created_bar)); f.i(o.created_seq); f.u(o.incarnation);
        f.b(o.stop_limit_activated);
        f.d(o.default_stop_placement_qty); f.d(o.frozen_default_qty);
        // Frozen fill-time admission/sizing snapshot (design-market-entry-
        // affordability / KI-54 / round-7 stop-entry-placement-admission).
        f.d(o.sizing_equity); f.d(o.sizing_price); f.d(o.sizing_fx); f.d(o.sizing_mark);
        f.d(o.default_stop_placement_equity); f.d(o.default_stop_sizing_price);
        f.d(o.tv_carry_qty);
        f.b(o.affordability_close_only);
        f.i(static_cast<int64_t>(o.created_position_cycle_seq));
        f.b(o.quantity_request.intent().has_value());
        if (const auto& intent = o.quantity_request.intent()) {
            f.i(static_cast<int64_t>(intent->kind()));
            if (intent->kind() == QuantityIntent::Kind::Units) f.d(intent->units());
            else if (intent->kind() == QuantityIntent::Kind::Fraction) {
                f.d(intent->numerator()); f.d(intent->denominator());
            }
        }
        f.b(o.quantity_request.reservation().has_value());
        if (const auto& reservation = o.quantity_request.reservation()) {
            f.d(reservation->units); f.d(reservation->basis_units);
        }
        // Round-14 signal-close-margin-call receipt (cross-bar: compared
        // against broker_fill_event_seq_ on the bar AFTER the one it was
        // stamped on).
        f.i(static_cast<int64_t>(o.signal_close_mc_bar));
        f.u(o.signal_close_mc_entry_incarnation);
        f.u(o.signal_close_mc_fill_seq);
        // Round-8 family S same-bar MARKET transaction (sizing frozen at
        // placement) and its strategy.close(id) companion.
        f.i(static_cast<int64_t>(o.pine_frozen_market_instruction.kind()));
        if (const auto* transaction = o.pine_frozen_market_instruction.transaction()) {
            f.d(transaction->own_units); f.d(transaction->transaction_units);
        }
        if (const auto* close = o.pine_frozen_market_instruction.targeted_close()) {
            f.s(close->target_id);
        }
        // KI-65 dual same-bar opposite-MARKET pairing candidate/finalization.
        admission::reflect(o.market_admission,"draft",[&](const auto& field){hash_admission_field(f,field);});
        f.d(o.paired_flat_market_own_qty);
        f.d(o.paired_flat_market_signal_close);
        f.d(o.paired_flat_market_signal_equity);
        f.d(o.paired_flat_market_signal_margin_pct);
        f.d(o.paired_flat_market_signal_pointvalue);
        f.d(o.paired_flat_market_signal_fx);
        f.i(o.paired_flat_market_peer_seq);
        f.d(o.paired_flat_market_transaction_qty);
        f.i(static_cast<int64_t>(o.short_seed_collision_role));
        f.i(static_cast<int64_t>(o.cancellation.cause()));
        f.i(static_cast<int64_t>(o.cancellation.state()));
        f.i(static_cast<int64_t>(o.cancellation.close_claim_release()));
        f.u(o.cancellation.source_incarnation());
        f.i(o.cancellation.source_sequence());
        f.u(o.cancellation.target_incarnation());
        f.i(o.cancellation.target_owner());
        f.u(o.cancellation.target_revision());
        f.d(o.cancellation.close_claim_consumed());
        f.d(o.cancellation.close_claim_retired());
        // --- Task-7 carried ruling (task-5 re-review): every remaining
        // PendingOrder member that a fill/admission/eligibility path reads.
        // Coverage is now enforced for the whole struct by
        // scripts/check_broker_state_hash_coverage.py (reflected member list
        // from scripts/gen_pending_order_mirror.py); `comment` is the one
        // waiver (trade-report label only). ---
        // Placement-side position/close provenance.
        f.i(static_cast<int64_t>(o.created_position_side));
        // Round-14 rounded-signal-cost decline receipt + its remaining qty.
        f.b(o.rounded_signal_cost_close_only);
        f.d(o.signal_close_mc_remaining_qty);
        // Same-id replacement / named-cancel recreate provenance
        // (clean-room two-call rules fail closed on these).
        f.u(o.replaced_order_incarnation);
        f.u(o.replaced_default_market_incarnation);
        f.u(o.recreated_after_named_cancelled_entry_incarnation);
        f.u(o.named_cancel_surviving_exit_incarnation);
        // calc_on_order_fills birth provenance and per-leg suppression
        // (decide which waypoints / legs the order may fill at).
        f.b(o.leg_activation.bounds().has_value());
        if (const auto& bounds = o.leg_activation.bounds()) {
            f.i(bounds->position_cycle); f.i(bounds->stop_first_bar); f.i(bounds->limit_first_bar);
        }
        f.b(o.pine_exit_activation.evidence().has_value());
        if (const auto& evidence = o.pine_exit_activation.evidence()) {
            f.i(evidence->position_cycle); f.i(evidence->entry_bar); f.i(evidence->direction);
            f.d(evidence->cursor_price); f.d(evidence->stop_level); f.d(evidence->limit_level);
            f.b(evidence->limit_continuation.has_value());
            if (const auto& continuation = evidence->limit_continuation) {
                f.i(static_cast<int64_t>(continuation->cause)); f.u(continuation->observed_fill_sequence);
            }
        }
        f.i(static_cast<int64_t>(o.birth.cause()));
        f.i(o.birth.bar());
        f.i(o.birth.timestamp());
        f.i(static_cast<int64_t>(o.birth.cursor().domain()));
        f.i(static_cast<int64_t>(o.birth.cursor().position()));
        f.i(o.birth.cursor().index());
        f.i(o.birth.cursor().count());
        f.d(o.birth.cursor_price());
        f.u(o.birth.first_fill());
        f.u(o.birth.last_fill());
        f.u(o.birth.evaluation_ordinal());
        f.i(static_cast<int64_t>(o.pine_birth_reach));
        f.i(static_cast<int64_t>(o.coof_cascade_seg_i));
        f.b(o.coof_cascade_inflight_fires);
        // Deferred close_all same-id stop preservation token.
        f.i(static_cast<int64_t>(o.same_id_stop_deferred_close_all_bar));
        f.u(o.same_id_stop_deferred_close_all_incarnation);
        // KI-65 dual same-bar opposite entry / gross-admission candidacy.
        // design-market-entry-affordability placement snapshot (the fill
        // check costs held + own against THIS equity at THIS price).
        f.d(o.affordability_placement_equity);
        f.d(o.affordability_signal_price);
        f.d(o.affordability_held_qty);
        // KI-61 exemption / explicit-qty admission snapshot.
        f.d(o.explicit_placement_equity);
        f.d(o.explicit_slipped_signal_close);
        // Round-7 default-percent stop placement basis.
        f.d(o.default_stop_placement_signal_close);
        f.b(o.reservation_expansion.capture().has_value());
        if (const auto& capture = o.reservation_expansion.capture()) {
            f.i(capture->position_cycle);
            f.i(static_cast<int64_t>(capture->side));
            f.b(capture->first_later_admission.has_value());
            if (const auto& admission = capture->first_later_admission) {
                f.u(*admission);
            }
        }
        f.b(o.reservation_growth_source.reservation_owner().has_value());
        if (const auto& receiver = o.reservation_growth_source.reservation_owner()) {
            f.u(*receiver);
        }
    }
    market_admission_journal_.reflect("journal",[&](const auto& field){hash_admission_field(f,field);});

    // strategy.exit partial orders are one-shot per open position per id.
    hash_str_set(f, consumed_partial_exit_ids_);

    // Per-bar dual-entry-stop arbitration snapshot (ABI v4 task 4). The
    // per-PASS working state (dual_entry_path_) is waived — see
    // scripts/broker_state_hash_waivers.txt.
    f.i(static_cast<int64_t>(last_bar_dual_entry_decision_));

    // --- Trailing stop state ---
    f.d(trail_best_price_);
    f.i(trail_close_restart_bar_);
    f.d(trail_best_before_bar_);
    f.i(trail_best_before_bar_index_);
    f.i(trail_best_before_bar_position_cycle_);
    f.u(trail_best_before_bar_fill_seq_);

    // At-most-one-priced-entry-open-per-bar arbiter and its result.
    f.i(priced_entry_activity_bar_);
    f.b(priced_entry_filled_this_bar_);

    // Explicit priority policy is future-behavioral configuration. No cached
    // decision survives a broker boundary; hash semantic ownership only.
    f.u(compat::pine::OrderPriority::schema_version);
    f.b(pine_order_priority_.attached());
    f.b(pine_order_priority_.retained_parent_first());

    // --- Pine compatibility policy and its day-owned quota ---
    f.u(compat::pine::IntradayCap::schema_version);
    f.i(static_cast<int64_t>(max_intraday_filled_orders_.attachment()));
    f.i(max_intraday_filled_orders_.configuration().limit);
    f.b(max_intraday_filled_orders_.configuration().skip_noop_market);
    f.b(max_intraday_filled_orders_.configuration().defer_pooc_close);
    f.b(max_intraday_filled_orders_.configuration().count_pooc_full_close);
    f.b(max_intraday_filled_orders_.budget().day().has_value());
    if (const auto& day = max_intraday_filled_orders_.budget().day()) {
        f.i(day->key);
    }
    f.i(max_intraday_filled_orders_.budget().charged_slots());
    f.b(max_intraday_filled_orders_.budget().latched());
    f.b(max_intraday_filled_orders_.budget().transfer().has_value());
    if (const auto& transfer = max_intraday_filled_orders_.budget().transfer()) {
        f.i(transfer->day.key);
        f.u(transfer->close_fill);
        f.i(transfer->source_bar);
        f.u(transfer->inheritor);
    }
    f.b(max_intraday_filled_orders_.due_cause().has_value());
    if (const auto& due = max_intraday_filled_orders_.due_cause()) {
        f.u(due->action_id);
        f.i(due->charged_day.key);
        f.i(due->charged_slots);
        f.i(due->trigger_bar);
        f.u(due->trigger_order);
    }
    f.u(max_intraday_filled_orders_.next_action());

    // The one-use position close obligation is independent of Pine risk days.
    f.b(position_close_obligation_.pending());
    if (const auto& request = position_close_obligation_.peek()) {
        f.u(request->action_id);
        f.i(request->position_cycle);
        f.i(request->after_bar);
        f.s(request->comment);
    }

    // --- Cached net-profit sum + its roundoff-bound provenance (both read
    // by a fill-time arithmetic-tolerance gate; see engine_fills.cpp). ---
    f.d(net_profit_sum_);
    f.d(net_profit_roundoff_bound_);
    f.d(net_profit_roundoff_value_);

    // --- Equity extremes that gate strategy.risk.max_drawdown ---
    f.d(max_equity_);
    f.d(max_drawdown_);
    f.d(min_equity_);

    // --- Risk latches ---
    f.i(cons_loss_day_count_);
    f.i(last_loss_day_);
    f.b(risk_halted_);
    f.d(intraday_pnl_);
    f.i(intraday_pnl_day_);
    f.d(intraday_loss_day_start_equity_);
    f.i(intraday_loss_day_);
    f.i(intraday_loss_block_day_);
    f.b(intraday_loss_cancel_pending_);

    // --- Margin-call intrabar chronology latches ---
    f.i(last_margin_call_event_bar_);
    f.i(intrabar_exit_margin_call_bar_);
    f.i(open_margin_slice_bar_);

    // --- Order/cycle identity generators (seed created_seq / incarnation /
    // position_cycle_seq_ of the NEXT order or cycle; two engines that agree
    // on every field above but would mint different ids for the next order
    // are not in the same broker state). ---
    f.i(next_order_seq_);
    f.u(next_order_incarnation_);
    f.u(exit_leg_event_seq_);
    f.u(broker_fill_event_seq_);

    // --- Account-currency FX broker clock (the injected rate SERIES is
    // configuration; the clock's consumption progress is runtime state). ---
    f.b(account_currency_fx_broker_epoch_initialized_);
    f.u(static_cast<uint64_t>(account_currency_fx_broker_epoch_));
    f.d(account_currency_fx_broker_rate_);

    // --- Script-visible report accumulators (controller ruling): these are
    // never read by the ENGINE's own fill/order decisions, but they ARE
    // exposed to the script via strategy.* accessors (strategy.wintrades,
    // strategy.grossprofit, strategy.max_contracts_held_all, ...), so a
    // script can branch on them and place a different order. G1 must cover
    // script-observable broker facts, not only the engine's own decisions.
    f.d(gross_profit_sum_);
    f.d(gross_loss_sum_);
    f.i(win_trades_count_);
    f.i(loss_trades_count_);
    f.i(eventrades_count_);
    f.d(max_runup_);
    f.d(max_contracts_held_all_);
    f.d(max_contracts_held_long_);
    f.d(max_contracts_held_short_);

    // Completed-trade ledger: also script-visible (strategy.closedtrades and
    // the per-trade strategy.closedtrades.* accessors read it directly), so
    // it is hashed for the same reason as the accumulators above. Only the
    // fields a script can actually read back are included, in append order
    // (chronological — not an unordered container, no sort needed).
    f.u(trades_.size());
    for (const auto& t : trades_) {
        f.i(t.entry_time); f.i(t.exit_time);
        f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.qty); f.d(t.pnl);
    }

    return f.h;
}

}  // namespace pineforge
