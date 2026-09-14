#include "placement_observation_fixture.hpp"
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
using namespace pineforge;
using pineforge::source::PendingOrder;
namespace pine_cap = pineforge::compat::pine;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

Bar flat_bar(double p, int64_t ts) { return Bar{p, p, p, p, 1.0, ts}; }
const std::vector<Bar> kBars = {flat_bar(100, 0), flat_bar(101, 60'000), flat_bar(102, 120'000)};

class Probe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override { if (bar_index_ == 1) strategy_entry("L", true); }

    // Mutation pin: {name, mutate}. `mutate` perturbs exactly one hashed
    // member of an already-built Probe.
    struct Pin { const char* name; std::function<void(Probe&)> mutate; };
    // Order-independence pin: {name, seed} inserts the SAME elements into
    // two probes in two different orders; broker_state_hash() must agree.
    struct OrderPin { const char* name; std::function<void(Probe&, Probe&)> seed; };

    // Both factories below are Probe member functions so their lambda
    // bodies -- lexically nested here -- inherit Probe's access to
    // BacktestEngine's protected state (a lambda's access rights follow
    // where it is DEFINED, not where it is later invoked; a free function
    // or a lambda defined in main() would not have this access even when
    // called with a Probe& parameter).

    // One entry per hashed BacktestEngine scalar/enum member (spec:
    // "every hashed member changes the hash" -- task-5-review.md Important
    // #3). Kept in the same order as engine_state_hash.cpp so a future
    // reviewer can diff the two lists directly.
    void seed_opening() {
        opening_obligations_.replace(broker::OpeningReceipt::check(
            {position_cycle_seq_, 17, 19, 2, 120000}, 102.0));
    }
    void mutate_opening_owner(const std::function<void(broker::OpeningOwner&)>& mutate) {
        auto owner = opening_obligations_.peek()->owner();
        mutate(owner);
        opening_obligations_.replace(broker::OpeningReceipt::check(
            owner, opening_obligations_.raw_fill_base()));
    }
    static std::vector<Pin> OpeningPins() {
        return {
            {"presence", [](Probe& s) { s.opening_obligations_.invalidate(); }},
            {"positionCycle", [](Probe& s) { s.mutate_opening_owner([](auto& o) { ++o.positionCycle; }); }},
            {"producerFill", [](Probe& s) { s.mutate_opening_owner([](auto& o) { ++o.producerFill; }); }},
            {"orderIncarnation", [](Probe& s) { s.mutate_opening_owner([](auto& o) { ++o.orderIncarnation; }); }},
            {"barIndex", [](Probe& s) { s.mutate_opening_owner([](auto& o) { ++o.barIndex; }); }},
            {"timestamp", [](Probe& s) { s.mutate_opening_owner([](auto& o) { ++o.timestamp; }); }},
            {"decision", [](Probe& s) {
                s.opening_obligations_.replace(broker::OpeningReceipt::exempt(
                    s.opening_obligations_.peek()->owner(), s.opening_obligations_.raw_fill_base()));
            }},
            {"continuation", [](Probe& s) {
                s.opening_obligations_.replace(broker::OpeningReceipt::check(
                    s.opening_obligations_.peek()->owner(), s.opening_obligations_.raw_fill_base(),
                    broker::OpeningContinuation::RemainingAdversePath));
            }},
            {"raw_fill_base", [](Probe& s) {
                s.opening_obligations_.replace(broker::OpeningReceipt::check(
                    s.opening_obligations_.peek()->owner(), 103.0));
            }},
        };
    }

    // Pure literal serialization state; mutable views are confined to this
    // test and never change the production policy's mutation interface.
    pine_cap::CapConfiguration& literal_cap_configuration() {
        return const_cast<pine_cap::CapConfiguration&>(adapter_.cap.configuration());
    }
    pine_cap::IntradayOrderBudget& literal_budget() {
        return const_cast<pine_cap::IntradayOrderBudget&>(adapter_.cap.budget());
    }
    void seed_intraday(pine_cap::CapAttachment attachment = pine_cap::CapAttachment::LegacySource) {
        adapter_.cap = pine_cap::IntradayCap(attachment);
        literal_cap_configuration() = {9, false, true, false};
        // Construct due-cause/action state with the real value transition.
        adapter_.cap.post_dispatch(
            {pine_cap::Dispatch::Allow, pine_cap::QuotaTrigger{{41}, 1}},
            {true, false, false, false, false, true, true, 3},
            {pine_cap::OrderKind::Market, 23, 3, true, pine_cap::Side::Long, 1, 0},
            pine_cap::Side::Long, 31, {100, 100, 101, 99});
        literal_budget().count_committed_close({41}, 9, 17, 3, 23);
        position_close_obligation_.schedule({1, 31, 3, "literal close"});
    }
    // Perturb one stored field at a time, including states public lifecycle
    // transitions cannot create. Seeding/removing several fields together
    // would conceal a missing child fold. No dispatch uses mutated fixtures.
    void mutate_literal_transfer(const std::function<void(pine_cap::CloseQuotaTransfer&)>& mutate) {
        mutate(const_cast<pine_cap::CloseQuotaTransfer&>(*literal_budget().transfer()));
    }
    void mutate_literal_due(const std::function<void(pine_cap::CloseCause&)>& mutate) {
        mutate(const_cast<pine_cap::CloseCause&>(*adapter_.cap.due_cause()));
    }
    void mutate_literal_request(const std::function<void(broker::PositionCloseRequest&)>& mutate) {
        auto request = *position_close_obligation_.peek();
        mutate(request);
        position_close_obligation_.schedule(request);
    }
    static std::vector<Pin> IntradayPins() {
        return {
            {"attachment", [](Probe& s) { s.seed_intraday(pine_cap::CapAttachment::None); }},
            {"configuration.limit", [](Probe& s) { ++s.literal_cap_configuration().limit; }},
            {"configuration.skip_noop_market", [](Probe& s) {
                s.literal_cap_configuration().skip_noop_market = true;
            }},
            {"configuration.defer_pooc_close", [](Probe& s) {
                s.literal_cap_configuration().defer_pooc_close = false;
            }},
            {"configuration.count_pooc_full_close", [](Probe& s) {
                s.literal_cap_configuration().count_pooc_full_close = true;
            }},
            {"risk_day.presence", [](Probe& s) {
                const_cast<std::optional<pine_cap::OrderRiskDay>&>(s.literal_budget().day()).reset();
            }},
            {"risk_day.key", [](Probe& s) {
                ++const_cast<pine_cap::OrderRiskDay&>(*s.literal_budget().day()).key;
            }},
            {"charged_slots", [](Probe& s) {
                s.literal_budget().admit_matched_attempt({41}, 9, 3, 99, 17);
            }},
            {"latched", [](Probe& s) { s.literal_budget().latch(); }},
            {"transfer.presence", [](Probe& s) { s.literal_budget().expire_transfer(); }},
            {"transfer.day.key", [](Probe& s) {
                s.mutate_literal_transfer([](auto& transfer) { ++transfer.day.key; });
            }},
            {"transfer.close_fill", [](Probe& s) {
                s.mutate_literal_transfer([](auto& transfer) { ++transfer.close_fill; });
            }},
            {"transfer.source_bar", [](Probe& s) {
                s.mutate_literal_transfer([](auto& transfer) { ++transfer.source_bar; });
            }},
            {"transfer.inheritor", [](Probe& s) {
                s.mutate_literal_transfer([](auto& transfer) { ++transfer.inheritor; });
            }},
            {"due_cause.presence", [](Probe& s) {
                const_cast<std::optional<pine_cap::CloseCause>&>(
                    s.adapter_.cap.due_cause()).reset();
            }},
            {"due_cause.action_id", [](Probe& s) {
                s.mutate_literal_due([](auto& due) { ++due.action_id; });
            }},
            {"due_cause.charged_day.key", [](Probe& s) {
                s.mutate_literal_due([](auto& due) { ++due.charged_day.key; });
            }},
            {"due_cause.charged_slots", [](Probe& s) {
                s.mutate_literal_due([](auto& due) { ++due.charged_slots; });
            }},
            {"due_cause.trigger_bar", [](Probe& s) {
                s.mutate_literal_due([](auto& due) { ++due.trigger_bar; });
            }},
            {"due_cause.trigger_order", [](Probe& s) {
                s.mutate_literal_due([](auto& due) { ++due.trigger_order; });
            }},
            {"next_action", [](Probe& s) {
                // Immediate decision advances the action counter without
                // altering existing due cause, quota or generic obligation.
                s.adapter_.cap.post_dispatch(
                    {pine_cap::Dispatch::Allow, pine_cap::QuotaTrigger{{41}, 1}},
                    {false, false, false, false, false, true, true, 3},
                    {pine_cap::OrderKind::Market, 23, 3, true, pine_cap::Side::Long, 1, 0},
                    pine_cap::Side::Long, 31, {100, 100, 101, 99});
            }},
            {"obligation.presence", [](Probe& s) { s.position_close_obligation_ = {}; }},
            {"obligation.action_id", [](Probe& s) {
                s.mutate_literal_request([](auto& request) { ++request.action_id; });
            }},
            {"obligation.position_cycle", [](Probe& s) {
                s.mutate_literal_request([](auto& request) { ++request.position_cycle; });
            }},
            {"obligation.after_bar", [](Probe& s) {
                s.mutate_literal_request([](auto& request) { ++request.after_bar; });
            }},
            {"obligation.comment", [](Probe& s) {
                s.mutate_literal_request([](auto& request) { request.comment += " changed"; });
            }},
        };
    }

    static std::vector<Pin> ScalarPins() {
        return {
            // Position core
            {"position_side_", [](Probe& s) {
                s.position_side_ = (s.position_side_ == PositionSide::LONG)
                    ? PositionSide::SHORT : PositionSide::LONG;
            }},
            {"position_entry_price_", [](Probe& s) { s.position_entry_price_ = 424242.5; }},
            {"position_entry_time_", [](Probe& s) { s.position_entry_time_ += 1; }},
            {"position_qty_", [](Probe& s) { s.position_qty_ += 1.0; }},
            {"position_entry_count_", [](Probe& s) { s.position_entry_count_ += 1; }},
            {"position_open_bar_", [](Probe& s) { s.position_open_bar_ += 1; }},
            {"position_cycle_seq_", [](Probe& s) { s.position_cycle_seq_ += 1; }},
            {"next_position_cycle_seq_", [](Probe& s) { s.next_position_cycle_seq_ += 1; }},

            // KI-64 POOC freeze snapshot + same-bar close carry
            {"pos_view_freeze_bar_", [](Probe& s) { s.pos_view_freeze_bar_ += 1; }},
            {"pos_view_frozen_side_", [](Probe& s) {
                s.pos_view_frozen_side_ = (s.pos_view_frozen_side_ == PositionSide::LONG)
                    ? PositionSide::SHORT : PositionSide::LONG;
            }},
            {"pos_view_frozen_qty_", [](Probe& s) { s.pos_view_frozen_qty_ = 424242.5; }},
            {"pending_close_qty_in_bar_", [](Probe& s) { s.pending_close_qty_in_bar_ = 424242.5; }},

            // Order book scalars

            // Dual-entry per-bar snapshot (underlying type is `int`; the
            // enumerators themselves are not visible from the public header,
            // so pin it through an int round-trip -- see engine.hpp's
            // forward declaration of internal::DualEntryStopPathWinner).
            {"last_bar_dual_entry_decision_", [](Probe& s) {
                const int v = static_cast<int>(s.last_bar_dual_entry_decision_);
                s.last_bar_dual_entry_decision_ =
                    static_cast<internal::DualEntryStopPathWinner>(v + 1);
            }},

            // Trailing stop state
            {"trail_best_price_", [](Probe& s) { s.trail_best_price_ = 123.0; }},
            {"trail_close_restart_bar_", [](Probe& s) { s.trail_close_restart_bar_ += 1; }},
            {"trail_best_before_bar_", [](Probe& s) { s.trail_best_before_bar_ = 424242.5; }},
            {"trail_best_before_bar_index_", [](Probe& s) { s.trail_best_before_bar_index_ += 1; }},
            {"trail_best_before_bar_position_cycle_", [](Probe& s) { s.trail_best_before_bar_position_cycle_ += 1; }},
            {"trail_best_before_bar_fill_seq_", [](Probe& s) { s.trail_best_before_bar_fill_seq_ += 1; }},
            {"priced_entry_activity_bar_", [](Probe& s) { s.priced_entry_activity_bar_ += 1; }},
            {"priced_entry_filled_this_bar_", [](Probe& s) { s.priced_entry_filled_this_bar_ = !s.priced_entry_filled_this_bar_; }},

            // Equity / roundoff
            {"net_profit_sum_", [](Probe& s) { s.net_profit_sum_ += 0.5; }},
            {"net_profit_roundoff_bound_", [](Probe& s) { s.net_profit_roundoff_bound_ = 424242.5; }},
            {"net_profit_roundoff_value_", [](Probe& s) { s.net_profit_roundoff_value_ = 424242.5; }},
            {"max_equity_", [](Probe& s) { s.max_equity_ += 1.0; }},
            {"max_drawdown_", [](Probe& s) { s.max_drawdown_ += 1.0; }},
            {"min_equity_", [](Probe& s) { s.min_equity_ += 1.0; }},

            // Risk latches
            {"cons_loss_day_count_", [](Probe& s) { s.cons_loss_day_count_ += 1; }},
            {"last_loss_day_", [](Probe& s) { s.last_loss_day_ += 1; }},
            {"risk_halted_", [](Probe& s) { s.risk_halted_ = !s.risk_halted_; }},
            {"intraday_pnl_", [](Probe& s) { s.intraday_pnl_ += 1.0; }},
            {"intraday_pnl_day_", [](Probe& s) { s.intraday_pnl_day_ += 1; }},
            {"intraday_loss_day_start_equity_", [](Probe& s) { s.intraday_loss_day_start_equity_ = 424242.5; }},
            {"intraday_loss_day_", [](Probe& s) { s.intraday_loss_day_ += 1; }},
            {"intraday_loss_block_day_", [](Probe& s) { s.intraday_loss_block_day_ += 1; }},
            {"intraday_loss_cancel_pending_", [](Probe& s) { s.intraday_loss_cancel_pending_ = !s.intraday_loss_cancel_pending_; }},

            // Margin-call chronology
            {"last_margin_call_event_bar_", [](Probe& s) { s.last_margin_call_event_bar_ += 1; }},
            {"intrabar_exit_margin_call_bar_", [](Probe& s) { s.intrabar_exit_margin_call_bar_ += 1; }},
            {"open_margin_slice_bar_", [](Probe& s) { s.open_margin_slice_bar_ += 1; }},

            // Identity generators
            {"next_order_seq_", [](Probe& s) { s.next_order_seq_ += 1; }},
            {"next_order_incarnation_", [](Probe& s) { s.next_order_incarnation_ += 1; }},
            {"broker_fill_event_seq_", [](Probe& s) { s.broker_fill_event_seq_ += 1; }},

            // Account-currency FX broker clock
            {"account_currency_fx_broker_epoch_initialized_", [](Probe& s) { s.account_currency_fx_broker_epoch_initialized_ = !s.account_currency_fx_broker_epoch_initialized_; }},
            {"account_currency_fx_broker_epoch_", [](Probe& s) { s.account_currency_fx_broker_epoch_ += 1; }},
            {"account_currency_fx_broker_rate_", [](Probe& s) { s.account_currency_fx_broker_rate_ = 424242.5; }},

            // Script-visible report accumulators (controller ruling, fix round 1)
            {"gross_profit_sum_", [](Probe& s) { s.gross_profit_sum_ = 424242.5; }},
            {"gross_loss_sum_", [](Probe& s) { s.gross_loss_sum_ = 424242.5; }},
            {"win_trades_count_", [](Probe& s) { s.win_trades_count_ += 1; }},
            {"loss_trades_count_", [](Probe& s) { s.loss_trades_count_ += 1; }},
            {"eventrades_count_", [](Probe& s) { s.eventrades_count_ += 1; }},
            {"max_runup_", [](Probe& s) { s.max_runup_ = 424242.5; }},
            {"max_contracts_held_all_", [](Probe& s) { s.max_contracts_held_all_ = 424242.5; }},
            {"max_contracts_held_long_", [](Probe& s) { s.max_contracts_held_long_ = 424242.5; }},
            {"max_contracts_held_short_", [](Probe& s) { s.max_contracts_held_short_ = 424242.5; }},
        };
    }

    // One element-mutation pin per hashed container, plus a handful of
    // extra PendingOrder field pins (fix round 1 added ~20 fields to the
    // per-order hash; a single stop_price pin does not exercise them).
    static std::vector<Pin> ContainerPins() {
        return {
            {"pyramid_entries_[].price", [](Probe& s) {
                if (!s.pyramid_entries_.empty()) s.pyramid_entries_[0].price += 1.0;
            }},
            {"pyramid_entries_[].entry_incarnation", [](Probe& s) {
                if (!s.pyramid_entries_.empty()) s.pyramid_entries_[0].entry_incarnation += 1;
            }},
            {"pyramid_entries_[].pooc_terminal_market_entry", [](Probe& s) {
                if (!s.pyramid_entries_.empty())
                    s.pyramid_entries_[0].pooc_terminal_market_entry =
                        !s.pyramid_entries_[0].pooc_terminal_market_entry;
            }},
            {"cycle_filled_entry_ids_ (insert)", [](Probe& s) {
                s.cycle_filled_entry_ids_.insert("new_id");
            }},
            {"id_unclosed_qty_ (insert)", [](Probe& s) {
                s.id_unclosed_qty_["k"] = 1.0;
            }},
            {"close_reserved_qty_ (insert)", [](Probe& s) {
                s.close_reserved_qty_["k"] = 1.0;
            }},
            {"close_two_call_first_qty_ (insert)", [](Probe& s) {
                s.close_two_call_first_qty_["k"] = 1.0;
            }},
            {"callsite_close_reserved_qty_ (insert)", [](Probe& s) {
                s.callsite_close_reserved_qty_[1]["k"] = 1.0;
            }},
            {"callsite_close_two_call_first_qty_ (insert)", [](Probe& s) {
                s.callsite_close_two_call_first_qty_[1]["k"] = 1.0;
            }},
            {"pos_view_frozen_entry_qty_ (insert)", [](Probe& s) {
                s.pos_view_frozen_entry_qty_["k"] = 1.0;
            }},
            {"consumed_partial_exit_ids_ (insert)", [](Probe& s) {
                s.consumed_partial_exit_ids_.insert("new_id");
            }},
            {"trades_ (push)", [](Probe& s) {
                s.trades_.push_back(Trade{});
            }},
            {"trades_[].pnl", [](Probe& s) {
                if (s.trades_.empty()) s.trades_.push_back(Trade{});
                s.trades_[0].pnl += 1.0;
            }},

            // pending_orders_: several distinct fields, incl. fix-round-1
            // additions, so a copy-paste onto the wrong field is caught.
            // A resting default MARKET order's NaN-sentinel doubles (e.g.
            // stop_price) must be ASSIGNED, not incremented: NaN + 1.0
            // preserves the exact same bit pattern (IEEE-754), which would
            // make the pin a silent no-op regardless of hash correctness
            // (see the comment on Build2Bars() below for the repro).
            {"pending_orders_[].legs.prices().stop_price", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].legs.set_stop_price(999.0);
            }},
            {"pending_orders_[].tv_carry_qty", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].tv_carry_qty = 42.0;
            }},
            {"pending_orders_[].sizing_equity", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].sizing_equity = 12345.0;
            }},
            {"pending_orders_[].over_pyramiding_cap_at_placement", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    placement_fixture::change(s.pending_orders_[0], [](auto& observation) {
                        observation.placement_side = static_cast<int>(observation.buy ? PositionSide::LONG : PositionSide::SHORT);
                        observation.held_entries = 1;
                        observation.configuration.pyramiding = observation.configuration.pyramiding == 1 ? 2 : 1;
                    });
            }},
            {"pending_orders_[].pine_frozen_market_instruction", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    auto& instruction = s.pending_orders_[0].pine_frozen_market_instruction;
                    if (instruction.active()) instruction.revoke();
                    else instruction = PineFrozenMarketInstruction::transaction(1, 2);
                }
            }},
            {"pending_orders_[].short_seed_collision_role", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].short_seed_collision_role =
                        (s.pending_orders_[0].short_seed_collision_role == ShortSeedCollisionRole::NONE)
                            ? ShortSeedCollisionRole::FINAL_SHORT : ShortSeedCollisionRole::NONE;
            }},
            {"pending_orders_[].paired_flat_market_peer_seq", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].paired_flat_market_peer_seq += 1;
            }},
            {"pending_orders_[].signal_close_mc_bar", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].signal_close_mc_bar += 1;
            }},
            {"pending_orders_[].suppress_as_declined_reversal_close", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    [&] {
                        const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9001, 9001, target, target);
                    }();
            }},

            // Task 7 (carried task-5 ruling): every PendingOrder member the
            // hash gained when coverage was extended to the whole struct
            // (scripts/check_broker_state_hash_coverage.py now reflects the
            // member list). One pin per newly hashed field, same rules as
            // above: bools flip, integers step, NaN-sentinel doubles ASSIGN.
            {"pending_orders_[].created_position_side", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].created_position_side =
                        (s.pending_orders_[0].created_position_side == PositionSide::LONG)
                            ? PositionSide::SHORT : PositionSide::LONG;
            }},
            {"pending_orders_[].created_after_position_close_in_bar", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    placement_fixture::prior_close_quantity(s.pending_orders_[0],
                        placement_has_prior_close(s.pending_orders_[0]) ? 0.0 : 1.0);
            }},
            {"pending_orders_[].rounded_signal_cost_close_only", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].rounded_signal_cost_close_only = !s.pending_orders_[0].rounded_signal_cost_close_only;
            }},
            {"pending_orders_[].replaced_order_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    ++s.pending_orders_[0].replaced_order_incarnation;
            }},
            {"pending_orders_[].declined_by_replaced_short_market", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    [&] {
                        const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Replacement, 9002, 9002, target, target);
                    }();
            }},
            {"pending_orders_[].leg_activation", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].leg_activation.bind({1,2,3});
            }},
            {"pending_orders_[].pine_exit_activation", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].pine_exit_activation =
                    PineExitActivationPolicy({1,0,1,100,110,90,std::nullopt});
            }},
            {"pending_orders_[].birth.fill_origin", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].birth = OrderBirth::fill_evaluation(0, 0,
                        BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4), 100, 1, 1, 1);
            }},
            {"pending_orders_[].birth.terminal_cursor", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].birth = OrderBirth::fill_evaluation(0, 0,
                        BirthCursor::point(BirthCursorDomain::HistoricalPath, 3, 4), 100, 1, 1, 1);
            }},
            {"pending_orders_[].pine_birth_reach", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].pine_birth_reach = PineHistoricalBirthReach::ExtremeWaypoints;
            }},
            {"pending_orders_[].coof_cascade_inflight_fires", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].coof_cascade_inflight_fires = !s.pending_orders_[0].coof_cascade_inflight_fires;
            }},
            // The predecessor result has no storage to mutate. The canonical
            // before-book direction leaves are independently mutated against
            // the actual broker hash in test_market_admission_state.
            {"pending_orders_[].reservation_expansion", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].reservation_expansion.capture(50,7,PositionSide::LONG,10);
            }},
            {"pending_orders_[].reservation_growth_source", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].reservation_growth_source.assign_capture(41,50);
            }},
            {"pending_orders_[].market_admission", [](Probe& s) {
                if(!s.pending_orders_.empty()) {
                    auto observed=*s.pending_orders_[0].market_admission.observation();
                    observed.configuration.default_quantity_value+=1;
                    s.pending_orders_[0].market_admission={};
                    s.pending_orders_[0].market_admission.bind(std::make_shared<const admission::CommandObservation>(observed));
                }
            }},
            {"pending_orders_[].signal_close_mc_remaining_qty", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].signal_close_mc_remaining_qty = 424242.5;
            }},
            {"pending_orders_[].affordability_placement_equity", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].affordability_placement_equity = 424242.5;
            }},
            {"pending_orders_[].affordability_signal_price", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].affordability_signal_price = 424242.5;
            }},
            {"pending_orders_[].affordability_held_qty", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].affordability_held_qty = 424242.5;
            }},
            {"pending_orders_[].explicit_placement_equity", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].explicit_placement_equity = 424242.5;
            }},
            {"pending_orders_[].explicit_slipped_signal_close", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].explicit_slipped_signal_close = 424242.5;
            }},
            {"pending_orders_[].default_stop_placement_signal_close", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].default_stop_placement_signal_close = 424242.5;
            }},
            {"pending_orders_[].suppressed_close_consumed_ledger_qty", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.bind_close_claim(424242.5, 0.0);
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9003, 9003, target, target);
                }
            }},
            {"pending_orders_[].suppressed_close_retired_ledger_qty", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.bind_close_claim(1.0, 424242.5);
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9004, 9004, target, target);
                }
            }},
            {"pending_orders_[].cancellation.cause", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9101, 1, target, target);
                }
            }},
            {"pending_orders_[].cancellation.state", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Replacement, 9102, 2, target, target);
                }
            }},
            {"pending_orders_[].cancellation.close_claim_release", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].cancellation.bind_close_claim(2.0, 0.0);
            }},
            {"pending_orders_[].cancellation.source_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9103, 3, target, target);
                }
            }},
            {"pending_orders_[].cancellation.source_sequence", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9104, 4, target, target);
                }
            }},
            {"pending_orders_[].cancellation.target_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation + 1, 0, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9105, 5, target, target);
                }
            }},
            {"pending_orders_[].cancellation.target_owner", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 7, 0};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9106, 6, target, target);
                }
            }},
            {"pending_orders_[].cancellation.target_revision", [](Probe& s) {
                if (!s.pending_orders_.empty()) {
                    const CancellationTarget target{s.pending_orders_[0].incarnation, 0, 8};
                    s.pending_orders_[0].cancellation.cancel(
                        CancellationCause::Dependency, 9107, 7, target, target);
                }
            }},
            {"pending_orders_[].cancellation.close_claim_consumed", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].cancellation.bind_close_claim(123.0, 0.0);
            }},
            {"pending_orders_[].cancellation.close_claim_retired", [](Probe& s) {
                if (!s.pending_orders_.empty())
                    s.pending_orders_[0].cancellation.bind_close_claim(1.0, 123.0);
            }},
            {"pending_orders_[].replaced_default_market_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].replaced_default_market_incarnation += 1;
            }},
            {"pending_orders_[].recreated_after_named_cancelled_entry_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].recreated_after_named_cancelled_entry_incarnation += 1;
            }},
            {"pending_orders_[].named_cancel_surviving_exit_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].named_cancel_surviving_exit_incarnation += 1;
            }},
            {"pending_orders_[].same_id_stop_deferred_close_all_incarnation", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].same_id_stop_deferred_close_all_incarnation += 1;
            }},
            {"pending_orders_[].coof_cascade_seg_i", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].coof_cascade_seg_i += 1;
            }},
            {"pending_orders_[].same_id_stop_deferred_close_all_bar", [](Probe& s) {
                if (!s.pending_orders_.empty()) s.pending_orders_[0].same_id_stop_deferred_close_all_bar += 1;
            }},
        };
    }

    // Order independence: insert the SAME elements into two probes in two
    // different orders. Every std::unordered_* member the hash covers gets
    // one entry here (std::set members like cycle_filled_entry_ids_ do not
    // need this -- their iteration order cannot depend on insertion order).
    static std::vector<OrderPin> OrderIndependencePins() {
        return {
            {"consumed_partial_exit_ids_", [](Probe& a, Probe& b) {
                a.consumed_partial_exit_ids_.insert("x1");
                a.consumed_partial_exit_ids_.insert("x2");
                a.consumed_partial_exit_ids_.insert("x3");
                b.consumed_partial_exit_ids_.insert("x3");
                b.consumed_partial_exit_ids_.insert("x1");
                b.consumed_partial_exit_ids_.insert("x2");
            }},
            {"id_unclosed_qty_", [](Probe& a, Probe& b) {
                a.id_unclosed_qty_["k1"] = 1.0; a.id_unclosed_qty_["k2"] = 2.0; a.id_unclosed_qty_["k3"] = 3.0;
                b.id_unclosed_qty_["k3"] = 3.0; b.id_unclosed_qty_["k1"] = 1.0; b.id_unclosed_qty_["k2"] = 2.0;
            }},
            {"close_reserved_qty_", [](Probe& a, Probe& b) {
                a.close_reserved_qty_["k1"] = 1.0; a.close_reserved_qty_["k2"] = 2.0; a.close_reserved_qty_["k3"] = 3.0;
                b.close_reserved_qty_["k3"] = 3.0; b.close_reserved_qty_["k1"] = 1.0; b.close_reserved_qty_["k2"] = 2.0;
            }},
            {"close_two_call_first_qty_", [](Probe& a, Probe& b) {
                a.close_two_call_first_qty_["k1"] = 1.0; a.close_two_call_first_qty_["k2"] = 2.0; a.close_two_call_first_qty_["k3"] = 3.0;
                b.close_two_call_first_qty_["k3"] = 3.0; b.close_two_call_first_qty_["k1"] = 1.0; b.close_two_call_first_qty_["k2"] = 2.0;
            }},
            {"pos_view_frozen_entry_qty_", [](Probe& a, Probe& b) {
                a.pos_view_frozen_entry_qty_["k1"] = 1.0; a.pos_view_frozen_entry_qty_["k2"] = 2.0; a.pos_view_frozen_entry_qty_["k3"] = 3.0;
                b.pos_view_frozen_entry_qty_["k3"] = 3.0; b.pos_view_frozen_entry_qty_["k1"] = 1.0; b.pos_view_frozen_entry_qty_["k2"] = 2.0;
            }},
            {"callsite_close_reserved_qty_ (nested)", [](Probe& a, Probe& b) {
                a.callsite_close_reserved_qty_[1]["a"] = 1.0;
                a.callsite_close_reserved_qty_[1]["b"] = 2.0;
                a.callsite_close_reserved_qty_[2]["c"] = 3.0;
                b.callsite_close_reserved_qty_[2]["c"] = 3.0;
                b.callsite_close_reserved_qty_[1]["b"] = 2.0;
                b.callsite_close_reserved_qty_[1]["a"] = 1.0;
            }},
            {"callsite_close_two_call_first_qty_ (nested)", [](Probe& a, Probe& b) {
                a.callsite_close_two_call_first_qty_[1]["a"] = 1.0;
                a.callsite_close_two_call_first_qty_[1]["b"] = 2.0;
                a.callsite_close_two_call_first_qty_[2]["c"] = 3.0;
                b.callsite_close_two_call_first_qty_[2]["c"] = 3.0;
                b.callsite_close_two_call_first_qty_[1]["b"] = 2.0;
                b.callsite_close_two_call_first_qty_[1]["a"] = 1.0;
            }},
        };
    }
};

Probe Build3Bars() { Probe s; s.run(kBars.data(), 3); return s; }

// bar 1 issues a market entry; a MARKET order fills at the NEXT bar's open,
// so with only 2 bars fed the order is still resting (pending_orders_ is
// non-empty, pyramid_entries_/trades_ are still empty). A resting default
// MARKET order's stop_price is the struct's "NaN = not set" sentinel, and
// IEEE-754 NaN + 1.0 preserves the exact same bit pattern (repro: quiet_NaN()
// + 1.0 == 0x7ff8000000000000 on both sides) -- every pending_orders_[]
// double-field pin above therefore ASSIGNS a concrete value rather than
// incrementing, so the pin is never a silent no-op regardless of the
// field's starting value.
Probe Build2Bars() { Probe s; s.run(kBars.data(), 2); return s; }

}  // namespace

int main() {
    Probe a = Build3Bars(), b = Build3Bars();
    CHECK(a.broker_state_hash() == b.broker_state_hash());        // deterministic
    CHECK(a.broker_state_hash() != 0);

    // Order independence, every std::unordered_* member the hash covers.
    for (const auto& op : Probe::OrderIndependencePins()) {
        Probe x = Build3Bars(), y = Build3Bars();
        op.seed(x, y);
        if (x.broker_state_hash() != y.broker_state_hash()) {
            std::fprintf(stderr, "FAIL order-independence %s: hash differs by insertion order\n", op.name);
            ++failures;
        }
    }

    // Mutation pin: every hashed BacktestEngine scalar/enum member.
    for (const auto& p : Probe::ScalarPins()) {
        Probe s = Build3Bars();
        const uint64_t before = s.broker_state_hash();
        p.mutate(s);
        if (s.broker_state_hash() == before) {
            std::fprintf(stderr, "FAIL scalar pin %s: hash unchanged\n", p.name);
            ++failures;
        }
    }

    // Start from the same present receipt for every pin. Seeding presence
    // inside a mutation would conceal a missing nested field from this test.
    for (const auto& p : Probe::OpeningPins()) {
        Probe s = Build3Bars();
        s.seed_opening();
        const uint64_t before = s.broker_state_hash();
        p.mutate(s);
        if (s.broker_state_hash() == before) {
            std::fprintf(stderr, "FAIL opening receipt pin %s: hash unchanged\n", p.name);
            ++failures;
        }
    }

    // Pure literal state: each nested cap owner, presence and due field must
    // affect the hash independently, without running an engine or strategy.
    for (const auto& p : Probe::IntradayPins()) {
        Probe s;
        s.seed_intraday();
        const uint64_t before = s.broker_state_hash();
        p.mutate(s);
        if (s.broker_state_hash() == before) {
            std::fprintf(stderr, "FAIL intraday ownership pin %s: hash unchanged\n", p.name);
            ++failures;
        }
    }

    // Mutation pin: one element mutation per hashed container.
    for (const auto& p : Probe::ContainerPins()) {
        // pending_orders_[] pins need the order still resting (2 bars);
        // everything else (pyramid_entries_, trades_, the always-empty
        // maps/sets) uses the 3-bar baseline.
        const bool needs_resting_order =
            std::string(p.name).rfind("pending_orders_", 0) == 0;
        Probe s = needs_resting_order ? Build2Bars() : Build3Bars();
        const uint64_t before = s.broker_state_hash();
        p.mutate(s);
        if (s.broker_state_hash() == before) {
            std::fprintf(stderr, "FAIL container pin %s: hash unchanged\n", p.name);
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}
