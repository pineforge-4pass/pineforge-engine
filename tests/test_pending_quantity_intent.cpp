// Literal native request/reservation contracts. No external tapes, embedded
// platform expected trades, strategy compilation or campaign measurement.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace prior_mirror {
#include "fixtures/pending_quantity/c45_pending_order_mirror.hpp"
}

using namespace pineforge;
using pineforge::source::PendingOrder;
static_assert(offsetof(pf_pending_order_v1_t, struct_version) == offsetof(prior_mirror::pf_pending_order_v1_t, struct_version), "legacy struct_version offset");
static_assert(offsetof(pf_pending_order_v1_t, size) == offsetof(prior_mirror::pf_pending_order_v1_t, size), "legacy size offset");
static_assert(offsetof(pf_pending_order_v1_t, id) == offsetof(prior_mirror::pf_pending_order_v1_t, id), "legacy id offset");
static_assert(offsetof(pf_pending_order_v1_t, id_truncated) == offsetof(prior_mirror::pf_pending_order_v1_t, id_truncated), "legacy id_truncated offset");
static_assert(offsetof(pf_pending_order_v1_t, id_hash64) == offsetof(prior_mirror::pf_pending_order_v1_t, id_hash64), "legacy id_hash64 offset");
static_assert(offsetof(pf_pending_order_v1_t, from_entry) == offsetof(prior_mirror::pf_pending_order_v1_t, from_entry), "legacy from_entry offset");
static_assert(offsetof(pf_pending_order_v1_t, from_entry_truncated) == offsetof(prior_mirror::pf_pending_order_v1_t, from_entry_truncated), "legacy from_entry_truncated offset");
static_assert(offsetof(pf_pending_order_v1_t, from_entry_hash64) == offsetof(prior_mirror::pf_pending_order_v1_t, from_entry_hash64), "legacy from_entry_hash64 offset");
static_assert(offsetof(pf_pending_order_v1_t, type) == offsetof(prior_mirror::pf_pending_order_v1_t, type), "legacy type offset");
static_assert(offsetof(pf_pending_order_v1_t, is_long) == offsetof(prior_mirror::pf_pending_order_v1_t, is_long), "legacy is_long offset");
static_assert(offsetof(pf_pending_order_v1_t, limit_price) == offsetof(prior_mirror::pf_pending_order_v1_t, limit_price), "legacy limit_price offset");
static_assert(offsetof(pf_pending_order_v1_t, stop_price) == offsetof(prior_mirror::pf_pending_order_v1_t, stop_price), "legacy stop_price offset");
static_assert(offsetof(pf_pending_order_v1_t, trail_points) == offsetof(prior_mirror::pf_pending_order_v1_t, trail_points), "legacy trail_points offset");
static_assert(offsetof(pf_pending_order_v1_t, trail_price) == offsetof(prior_mirror::pf_pending_order_v1_t, trail_price), "legacy trail_price offset");
static_assert(offsetof(pf_pending_order_v1_t, trail_offset) == offsetof(prior_mirror::pf_pending_order_v1_t, trail_offset), "legacy trail_offset offset");
static_assert(offsetof(pf_pending_order_v1_t, profit_ticks) == offsetof(prior_mirror::pf_pending_order_v1_t, profit_ticks), "legacy profit_ticks offset");
static_assert(offsetof(pf_pending_order_v1_t, loss_ticks) == offsetof(prior_mirror::pf_pending_order_v1_t, loss_ticks), "legacy loss_ticks offset");
static_assert(offsetof(pf_pending_order_v1_t, qty) == offsetof(prior_mirror::pf_pending_order_v1_t, qty), "legacy qty offset");
static_assert(offsetof(pf_pending_order_v1_t, qty_type) == offsetof(prior_mirror::pf_pending_order_v1_t, qty_type), "legacy qty_type offset");
static_assert(offsetof(pf_pending_order_v1_t, qty_percent) == offsetof(prior_mirror::pf_pending_order_v1_t, qty_percent), "legacy qty_percent offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_name) == offsetof(prior_mirror::pf_pending_order_v1_t, oca_name), "legacy oca_name offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_name_truncated) == offsetof(prior_mirror::pf_pending_order_v1_t, oca_name_truncated), "legacy oca_name_truncated offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_name_hash64) == offsetof(prior_mirror::pf_pending_order_v1_t, oca_name_hash64), "legacy oca_name_hash64 offset");
static_assert(offsetof(pf_pending_order_v1_t, oca_type) == offsetof(prior_mirror::pf_pending_order_v1_t, oca_type), "legacy oca_type offset");
static_assert(offsetof(pf_pending_order_v1_t, created_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, created_bar), "legacy created_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, created_seq) == offsetof(prior_mirror::pf_pending_order_v1_t, created_seq), "legacy created_seq offset");
static_assert(offsetof(pf_pending_order_v1_t, incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, incarnation), "legacy incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, created_by_same_id_replacement) == offsetof(prior_mirror::pf_pending_order_v1_t, created_by_same_id_replacement), "legacy created_by_same_id_replacement offset");
static_assert(offsetof(pf_pending_order_v1_t, replaced_default_market_incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, replaced_default_market_incarnation), "legacy replaced_default_market_incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, declined_by_replaced_short_market) == offsetof(prior_mirror::pf_pending_order_v1_t, declined_by_replaced_short_market), "legacy declined_by_replaced_short_market offset");
static_assert(offsetof(pf_pending_order_v1_t, replaced_exit_order_incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, replaced_exit_order_incarnation), "legacy replaced_exit_order_incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, recreated_after_named_cancelled_entry_incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, recreated_after_named_cancelled_entry_incarnation), "legacy recreated_after_named_cancelled_entry_incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, named_cancel_surviving_exit_incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, named_cancel_surviving_exit_incarnation), "legacy named_cancel_surviving_exit_incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, stop_limit_activated) == offsetof(prior_mirror::pf_pending_order_v1_t, stop_limit_activated), "legacy stop_limit_activated offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_suppress_stop_on_entry_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, coof_suppress_stop_on_entry_bar), "legacy coof_suppress_stop_on_entry_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_suppress_limit_on_entry_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, coof_suppress_limit_on_entry_bar), "legacy coof_suppress_limit_on_entry_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, created_during_coof_recalc) == offsetof(prior_mirror::pf_pending_order_v1_t, created_during_coof_recalc), "legacy created_during_coof_recalc offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_born_at_close_recalc) == offsetof(prior_mirror::pf_pending_order_v1_t, coof_born_at_close_recalc), "legacy coof_born_at_close_recalc offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_born_mid_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, coof_born_mid_bar), "legacy coof_born_mid_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_cascade_seg_i) == offsetof(prior_mirror::pf_pending_order_v1_t, coof_cascade_seg_i), "legacy coof_cascade_seg_i offset");
static_assert(offsetof(pf_pending_order_v1_t, coof_cascade_inflight_fires) == offsetof(prior_mirror::pf_pending_order_v1_t, coof_cascade_inflight_fires), "legacy coof_cascade_inflight_fires offset");
static_assert(offsetof(pf_pending_order_v1_t, created_position_side) == offsetof(prior_mirror::pf_pending_order_v1_t, created_position_side), "legacy created_position_side offset");
static_assert(offsetof(pf_pending_order_v1_t, created_position_cycle_seq) == offsetof(prior_mirror::pf_pending_order_v1_t, created_position_cycle_seq), "legacy created_position_cycle_seq offset");
static_assert(offsetof(pf_pending_order_v1_t, created_after_position_close_in_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, created_after_position_close_in_bar), "legacy created_after_position_close_in_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, over_pyramiding_cap_at_placement) == offsetof(prior_mirror::pf_pending_order_v1_t, over_pyramiding_cap_at_placement), "legacy over_pyramiding_cap_at_placement offset");
static_assert(offsetof(pf_pending_order_v1_t, same_id_stop_deferred_close_all_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, same_id_stop_deferred_close_all_bar), "legacy same_id_stop_deferred_close_all_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, same_id_stop_deferred_close_all_incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, same_id_stop_deferred_close_all_incarnation), "legacy same_id_stop_deferred_close_all_incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, reverses_same_bar_market_from_flat) == offsetof(prior_mirror::pf_pending_order_v1_t, reverses_same_bar_market_from_flat), "legacy reverses_same_bar_market_from_flat offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_candidate) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_candidate), "legacy paired_flat_market_candidate offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_own_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_own_qty), "legacy paired_flat_market_own_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_close) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_signal_close), "legacy paired_flat_market_signal_close offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_equity) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_signal_equity), "legacy paired_flat_market_signal_equity offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_margin_pct) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_signal_margin_pct), "legacy paired_flat_market_signal_margin_pct offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_pointvalue) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_signal_pointvalue), "legacy paired_flat_market_signal_pointvalue offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_signal_fx) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_signal_fx), "legacy paired_flat_market_signal_fx offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_peer_seq) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_peer_seq), "legacy paired_flat_market_peer_seq offset");
static_assert(offsetof(pf_pending_order_v1_t, paired_flat_market_transaction_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, paired_flat_market_transaction_qty), "legacy paired_flat_market_transaction_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, default_flat_market_gross_candidate) == offsetof(prior_mirror::pf_pending_order_v1_t, default_flat_market_gross_candidate), "legacy default_flat_market_gross_candidate offset");
static_assert(offsetof(pf_pending_order_v1_t, tv_carry_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, tv_carry_qty), "legacy tv_carry_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, frozen_default_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, frozen_default_qty), "legacy frozen_default_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_placement_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, default_stop_placement_qty), "legacy default_stop_placement_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_placement_equity) == offsetof(prior_mirror::pf_pending_order_v1_t, default_stop_placement_equity), "legacy default_stop_placement_equity offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_placement_signal_close) == offsetof(prior_mirror::pf_pending_order_v1_t, default_stop_placement_signal_close), "legacy default_stop_placement_signal_close offset");
static_assert(offsetof(pf_pending_order_v1_t, default_stop_sizing_price) == offsetof(prior_mirror::pf_pending_order_v1_t, default_stop_sizing_price), "legacy default_stop_sizing_price offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_equity) == offsetof(prior_mirror::pf_pending_order_v1_t, sizing_equity), "legacy sizing_equity offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_price) == offsetof(prior_mirror::pf_pending_order_v1_t, sizing_price), "legacy sizing_price offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_fx) == offsetof(prior_mirror::pf_pending_order_v1_t, sizing_fx), "legacy sizing_fx offset");
static_assert(offsetof(pf_pending_order_v1_t, sizing_mark) == offsetof(prior_mirror::pf_pending_order_v1_t, sizing_mark), "legacy sizing_mark offset");
static_assert(offsetof(pf_pending_order_v1_t, opening_affordability_exemption_candidate) == offsetof(prior_mirror::pf_pending_order_v1_t, opening_affordability_exemption_candidate), "legacy opening_affordability_exemption_candidate offset");
static_assert(offsetof(pf_pending_order_v1_t, explicit_flat_admission_candidate) == offsetof(prior_mirror::pf_pending_order_v1_t, explicit_flat_admission_candidate), "legacy explicit_flat_admission_candidate offset");
static_assert(offsetof(pf_pending_order_v1_t, explicit_placement_equity) == offsetof(prior_mirror::pf_pending_order_v1_t, explicit_placement_equity), "legacy explicit_placement_equity offset");
static_assert(offsetof(pf_pending_order_v1_t, explicit_slipped_signal_close) == offsetof(prior_mirror::pf_pending_order_v1_t, explicit_slipped_signal_close), "legacy explicit_slipped_signal_close offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_placement_equity) == offsetof(prior_mirror::pf_pending_order_v1_t, affordability_placement_equity), "legacy affordability_placement_equity offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_signal_price) == offsetof(prior_mirror::pf_pending_order_v1_t, affordability_signal_price), "legacy affordability_signal_price offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_held_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, affordability_held_qty), "legacy affordability_held_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, affordability_close_only) == offsetof(prior_mirror::pf_pending_order_v1_t, affordability_close_only), "legacy affordability_close_only offset");
static_assert(offsetof(pf_pending_order_v1_t, rounded_signal_cost_close_only) == offsetof(prior_mirror::pf_pending_order_v1_t, rounded_signal_cost_close_only), "legacy rounded_signal_cost_close_only offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, signal_close_mc_bar), "legacy signal_close_mc_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_entry_incarnation) == offsetof(prior_mirror::pf_pending_order_v1_t, signal_close_mc_entry_incarnation), "legacy signal_close_mc_entry_incarnation offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_fill_seq) == offsetof(prior_mirror::pf_pending_order_v1_t, signal_close_mc_fill_seq), "legacy signal_close_mc_fill_seq offset");
static_assert(offsetof(pf_pending_order_v1_t, signal_close_mc_remaining_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, signal_close_mc_remaining_qty), "legacy signal_close_mc_remaining_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, comment) == offsetof(prior_mirror::pf_pending_order_v1_t, comment), "legacy comment offset");
static_assert(offsetof(pf_pending_order_v1_t, comment_truncated) == offsetof(prior_mirror::pf_pending_order_v1_t, comment_truncated), "legacy comment_truncated offset");
static_assert(offsetof(pf_pending_order_v1_t, comment_hash64) == offsetof(prior_mirror::pf_pending_order_v1_t, comment_hash64), "legacy comment_hash64 offset");
static_assert(offsetof(pf_pending_order_v1_t, requested_partial) == offsetof(prior_mirror::pf_pending_order_v1_t, requested_partial), "legacy requested_partial offset");
static_assert(offsetof(pf_pending_order_v1_t, full_percent_exit_request) == offsetof(prior_mirror::pf_pending_order_v1_t, full_percent_exit_request), "legacy full_percent_exit_request offset");
static_assert(offsetof(pf_pending_order_v1_t, pooc_global_full_exit_dynamic_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, pooc_global_full_exit_dynamic_qty), "legacy pooc_global_full_exit_dynamic_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, pooc_global_full_exit_tracks_bound_adds) == offsetof(prior_mirror::pf_pending_order_v1_t, pooc_global_full_exit_tracks_bound_adds), "legacy pooc_global_full_exit_tracks_bound_adds offset");
static_assert(offsetof(pf_pending_order_v1_t, pooc_global_full_exit_bound_add) == offsetof(prior_mirror::pf_pending_order_v1_t, pooc_global_full_exit_bound_add), "legacy pooc_global_full_exit_bound_add offset");
static_assert(offsetof(pf_pending_order_v1_t, created_while_in_position) == offsetof(prior_mirror::pf_pending_order_v1_t, created_while_in_position), "legacy created_while_in_position offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_member) == offsetof(prior_mirror::pf_pending_order_v1_t, sbmt_member), "legacy sbmt_member offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_own_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, sbmt_own_qty), "legacy sbmt_own_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_tx_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, sbmt_tx_qty), "legacy sbmt_tx_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_kept_over_cap) == offsetof(prior_mirror::pf_pending_order_v1_t, sbmt_kept_over_cap), "legacy sbmt_kept_over_cap offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_close_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, sbmt_close_qty), "legacy sbmt_close_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, sbmt_close_buy) == offsetof(prior_mirror::pf_pending_order_v1_t, sbmt_close_buy), "legacy sbmt_close_buy offset");
static_assert(offsetof(pf_pending_order_v1_t, suppress_as_declined_reversal_close) == offsetof(prior_mirror::pf_pending_order_v1_t, suppress_as_declined_reversal_close), "legacy suppress_as_declined_reversal_close offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_bracket) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_bracket), "legacy dormant_bracket offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_reissue_pending) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_reissue_pending), "legacy dormant_reissue_pending offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_original_stop_price) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_original_stop_price), "legacy dormant_original_stop_price offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_hold_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_hold_bar), "legacy dormant_hold_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_reversal_kill_bar) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_reversal_kill_bar), "legacy dormant_reversal_kill_bar offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_trail_best) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_trail_best), "legacy dormant_trail_best offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_trail_best_start) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_trail_best_start), "legacy dormant_trail_best_start offset");
static_assert(offsetof(pf_pending_order_v1_t, dormant_trail_leg_dead) == offsetof(prior_mirror::pf_pending_order_v1_t, dormant_trail_leg_dead), "legacy dormant_trail_leg_dead offset");
static_assert(offsetof(pf_pending_order_v1_t, suppressed_close_consumed_ledger_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, suppressed_close_consumed_ledger_qty), "legacy suppressed_close_consumed_ledger_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, suppressed_close_retired_ledger_qty) == offsetof(prior_mirror::pf_pending_order_v1_t, suppressed_close_retired_ledger_qty), "legacy suppressed_close_retired_ledger_qty offset");
static_assert(offsetof(pf_pending_order_v1_t, short_seed_collision_role) == offsetof(prior_mirror::pf_pending_order_v1_t, short_seed_collision_role), "legacy short_seed_collision_role offset");
namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
}
namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
bool partial(const PendingOrder& o) { return o.quantity_request.is_partial(1e-9, 1e-9); }

void quantity_values_have_distinct_meaning() {
    QuantityRequest request;
    CHECK(!request.intent() && !request.reservation());
    bool refused = false;
    try { request.reserve(1,4); }
    catch (const std::logic_error&) { refused = true; }
    CHECK(refused && !request.reservation());
    for (double denominator : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
        bool invalid = false;
        try { (void)QuantityIntent::fraction(1,denominator); }
        catch (const std::invalid_argument&) { invalid = true; }
        CHECK(invalid);
    }
    request.request(QuantityIntent::fraction(1,4));
    CHECK(request.is_partial(0,0));
    request.reserve(4,4);
    CHECK(!request.is_partial(0,0));
    CHECK(request.intent()->numerator() == 1 && request.intent()->denominator() == 4);
    request.request(QuantityIntent::units(3));
    CHECK(!request.reservation() && !request.requests_all());
    CHECK(request.intent()->units() == 3);
    request.reserve(2,4);
    CHECK(request.is_partial(0,0) && request.intent()->units() == 3);
    request.request(QuantityIntent::all());
    CHECK(request.requests_all() && !request.reservation());
}

class Book : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 100000;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 0;
        pyramiding_ = 10;
        current_bar_ = {100,100,100,100,1,0};
    }
    void on_source_bar(const Bar&) override {}
    void entry(double qty) { strategy_entry("E", true, nan, nan, qty); }
    void step(double price = 100) {
        ++bar_index_;
        current_bar_ = {price,price,price,price,1,int64_t(bar_index_) * 60000};
        process_pending_orders(current_bar_);
    }
    void seed(double qty) { entry(qty); step(); }
    void exit(const char* id, double percent = 100, double units = nan,
              const char* group = "") {
        strategy_exit(id,"E",120,90,nan,nan,nan,percent,"",units,group);
    }
    void cancel(const char* id) { strategy_cancel(id); }
    void reduce_group(double qty) { strategy_order("reduce",true,qty,nan,nan,"Q",2); }
    void lot_step(double value) { qty_step_ = value; }
    void reset() { run(nullptr,0); }
    double position() const { return position_qty_; }
    const std::vector<PendingOrder>& orders() const { return pending_orders_; }
    PendingOrder& order(const std::string& id) {
        for (auto& o : pending_orders_) if (o.id == id) return o;
        throw std::logic_error("missing literal order");
    }
};

void requested_amount_is_separate_from_reserved_amount() {
    Book units; units.seed(4); units.exit("X",100,1);
    const auto& u = units.order("X");
    CHECK(u.qty == 1 && partial(u));
    CHECK(u.quantity_request.intent()->kind() == QuantityIntent::Kind::Units);
    CHECK(u.quantity_request.intent()->units() == 1);
    CHECK(u.quantity_request.reservation()->units == 1);
    CHECK(u.quantity_request.reservation()->basis_units == 4);
    Book fraction; fraction.seed(4); fraction.exit("X",25);
    const auto& f = fraction.order("X");
    CHECK(f.qty == 1 && partial(f));
    CHECK(f.quantity_request.intent()->kind() == QuantityIntent::Kind::Fraction);
    CHECK(f.quantity_request.intent()->numerator() == 25);
    CHECK(f.quantity_request.intent()->denominator() == 100);
    Book clipped; clipped.seed(4); clipped.exit("first",25); clipped.exit("all");
    const auto& a = clipped.order("all");
    CHECK(a.quantity_request.requests_all());
    CHECK(a.qty == 3 && partial(a));
    CHECK(a.quantity_request.reservation()->basis_units == 4);
}

void minimum_slot_does_not_rewrite_fraction_intent() {
    Book b; b.lot_step(1); b.seed(1); b.exit("half",50);
    const auto& o = b.order("half");
    CHECK(o.qty == 1 && !partial(o));
    CHECK(!o.quantity_request.requests_all());
    CHECK(o.quantity_request.intent()->numerator() == 50);
    CHECK(o.quantity_request.reservation()->units == 1);
    CHECK(o.quantity_request.reservation()->basis_units == 1);
    b.exit("blocked",50);
    CHECK(b.orders().size() == 1);
}

void normalized_full_amount_does_not_invent_all_intent() {
    Book fraction; fraction.seed(4); fraction.exit("X",150);
    const auto& f = fraction.order("X");
    CHECK(f.qty == 4 && f.qty_percent == 100 && !partial(f));
    CHECK(!f.quantity_request.requests_all());
    CHECK(f.quantity_request.intent()->numerator() == 150);
    Book units; units.seed(4); units.exit("X",50,4);
    const auto& u = units.order("X");
    CHECK(u.qty == 4 && !partial(u) && !u.quantity_request.requests_all());
    CHECK(u.quantity_request.intent()->kind() == QuantityIntent::Kind::Units);
    CHECK(u.quantity_request.intent()->units() == 4);
}

void deferred_reservation_binds_and_keeps_original_all() {
    Book b; b.entry(4); b.exit("quarter",25); b.exit("rest");
    CHECK(std::isnan(b.order("quarter").qty));
    CHECK(!b.order("quarter").quantity_request.reservation());
    CHECK(partial(b.order("quarter")));
    CHECK(b.order("rest").quantity_request.requests_all());
    CHECK(!partial(b.order("rest")));
    b.step();
    CHECK(b.position() == 4);
    CHECK(b.order("quarter").qty == 1);
    CHECK(b.order("rest").qty == 3);
    CHECK(partial(b.order("quarter")) && partial(b.order("rest")));
    CHECK(b.order("rest").quantity_request.requests_all());
    CHECK(b.order("rest").quantity_request.reservation()->basis_units == 4);
}

void executable_reduction_does_not_change_reservation_history() {
    Book b; b.seed(4); b.exit("all",100,nan,"Q");
    CHECK(!partial(b.order("all")));
    b.reduce_group(1); b.step();
    const auto& o = b.order("all");
    CHECK(o.qty < 4); // Actual OCA reduction, independent of source intent.
    CHECK(o.quantity_request.requests_all() && !partial(o));
    CHECK(o.quantity_request.reservation()->units == 4);
    CHECK(o.quantity_request.reservation()->basis_units == 4);
}

void replacement_copy_cancel_and_reset() {
    Book b; b.seed(4); b.exit("X",25);
    const auto old = b.order("X").incarnation;
    Book copy = b;
    CHECK(copy.broker_state_hash() == b.broker_state_hash());
    b.exit("X");
    CHECK(b.order("X").incarnation != old);
    CHECK(b.order("X").quantity_request.requests_all());
    CHECK(b.order("X").qty == 4);
    CHECK(copy.order("X").qty == 1 && partial(copy.order("X")));
    copy.cancel("X"); CHECK(copy.orders().empty());
    CHECK(b.orders().size() == 1);
    b.reset(); CHECK(b.orders().empty());
}

void partial_fill_preserves_existing_reissue_policy() {
    Book b; b.seed(4); b.exit("X",25); b.step(90);
    CHECK(b.position() == 3 && b.orders().empty());
    b.exit("X",25);
    CHECK(b.orders().empty());
    b.exit("X");
    CHECK(b.orders().size() == 1);
    CHECK(b.order("X").qty == 3 && b.order("X").quantity_request.requests_all());
}

void per_binding_leg_preserves_request() {
    Book b; b.seed(1); b.exit("X",100,1); b.entry(2); b.exit("X",100,1);
    int legs = 0;
    for (const auto& o : b.orders()) if (o.type == OrderType::EXIT && o.id == "X") {
        ++legs;
        CHECK(o.quantity_request.intent()->kind() == QuantityIntent::Kind::Units);
        CHECK(o.quantity_request.intent()->units() == 1);
        CHECK(o.quantity_request.reservation()->units == o.qty);
        CHECK(o.quantity_request.reservation()->basis_units == 1);
    }
    CHECK(legs == 2);
}

void equal_legacy_flags_do_not_hide_distinct_intents_from_hash() {
    Book a; a.seed(4); a.exit("X",25);
    Book b = a;
    b.order("X").quantity_request.request(QuantityIntent::units(1));
    b.order("X").quantity_request.reserve(1,4);
    CHECK(partial(a.order("X")) && partial(b.order("X")));
    CHECK(a.broker_state_hash() != b.broker_state_hash());
    Book c = a; c.order("X").quantity_request.reserve(1,5);
    CHECK(a.broker_state_hash() != c.broker_state_hash());
    Book d = a; d.order("X").quantity_request.request(QuantityIntent::fraction(25,100));
    CHECK(a.broker_state_hash() != d.broker_state_hash());
}

void legacy_mirror_prefix_and_new_facts() {
    static_assert(offsetof(pf_pending_order_v1_t, quantity_intent_kind)
                  >= sizeof(prior_mirror::pf_pending_order_v1_t), "new fields append after old prefix");
    Book b; b.lot_step(1); b.seed(1); b.exit("half",50);
    pf_pending_order_v1_t out{};
    CHECK(strategy_pending_order_get(&b,0,&out,sizeof(out)) == 0);
    CHECK(out.requested_partial == 0 && out.full_percent_exit_request == 0);
    CHECK(out.quantity_intent_kind == 2);
    CHECK(out.quantity_intent_numerator == 50 && out.quantity_intent_denominator == 100);
    CHECK(out.quantity_reservation_present == 1);
    CHECK(out.quantity_reservation_units == 1 && out.quantity_reservation_basis_units == 1);
    std::vector<unsigned char> prefix(sizeof(out),0xA5);
    CHECK(strategy_pending_order_get(&b,0,prefix.data(),sizeof(prior_mirror::pf_pending_order_v1_t)) == 0);
    CHECK(std::memcmp(prefix.data(),&out,sizeof(prior_mirror::pf_pending_order_v1_t)) == 0);
    for (size_t i=sizeof(prior_mirror::pf_pending_order_v1_t); i<prefix.size(); ++i)
        CHECK(prefix[i] == 0xA5);
    PendingOrder plain{};
    fill_pending_order_mirror(plain,&out);
    CHECK(out.quantity_intent_kind == 0 && out.quantity_reservation_present == 0);
    CHECK(out.requested_partial == 0 && out.full_percent_exit_request == 0);
}
}

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"quantity values", quantity_values_have_distinct_meaning},
        {"original versus reserved", requested_amount_is_separate_from_reserved_amount},
        {"minimum slot", minimum_slot_does_not_rewrite_fraction_intent},
        {"normalized full amount", normalized_full_amount_does_not_invent_all_intent},
        {"deferred binding", deferred_reservation_binds_and_keeps_original_all},
        {"OCA reduction", executable_reduction_does_not_change_reservation_history},
        {"replacement/copy/reset", replacement_copy_cancel_and_reset},
        {"partial fill/reissue", partial_fill_preserves_existing_reissue_policy},
        {"extra bindings", per_binding_leg_preserves_request},
        {"hash identity", equal_legacy_flags_do_not_hide_distinct_intents_from_hash},
        {"mirror prefix", legacy_mirror_prefix_and_new_facts},
    };
    for (const auto& test : cases) {
        std::fprintf(stderr, "case: %s\n", test.first);
        try { test.second(); }
        catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.first, error.what());
        }
    }
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
