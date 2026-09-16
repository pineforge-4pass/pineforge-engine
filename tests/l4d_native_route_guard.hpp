#pragma once

// Every L4d parity twin binds the switched source host. The historical test
// spelling is macro-mapped only after this header has completed, so product
// headers retain their real `PineStrategyHost` declarations.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifndef PINEFORGE_HAS_NATIVE_LOWERING_V1
#error "L4d native-route twins require the v17 native lowering surface"
#endif

namespace pineforge::source {

// Test-only, read-only projection of a live PendingIntentView row. It is not
// a compatibility order or a second matching book: every populated field is
// copied from strategy_pending_order_get / the frozen public POD. Mutating a
// returned row changes only that test's local snapshot; it can never affect a
// future native decision. A29 twins use public commands for executable paths
// and ledger any historical owner-only mutation that has no projection.
enum class L4dOrderType { MARKET = 0, ENTRY = 1, EXIT = 2, RAW_ORDER = 3 };
enum class L4dShortSeedRole : std::uint8_t {
    NONE = 0, LONG_ENTRY = 1, MATERIALIZE_LONG = 2, FINAL_SHORT = 3,
};

struct L4dLegPrices {
    double limit_price = std::numeric_limits<double>::quiet_NaN();
    double stop_price = std::numeric_limits<double>::quiet_NaN();
    double trail_points = std::numeric_limits<double>::quiet_NaN();
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double trail_offset = std::numeric_limits<double>::quiet_NaN();
};

struct L4dLegTarget {
    std::uint64_t incarnation = 0;
    std::uint64_t owner = 0;
    std::uint64_t revision = 0;
};

struct L4dLegs {
    L4dLegPrices prices_{};
    L4dLegTarget target_{};
    const L4dLegPrices& prices() const noexcept { return prices_; }
    const L4dLegTarget& target() const noexcept { return target_; }
    std::uint64_t revision() const noexcept { return target_.revision; }
    void attach(std::uint64_t incarnation, std::int64_t owner) noexcept {
        target_.incarnation = incarnation;
        target_.owner = static_cast<std::uint64_t>(owner);
    }
    double set_limit_price(double value) noexcept { prices_.limit_price = value; return value; }
    double set_stop_price(double value) noexcept { prices_.stop_price = value; return value; }
    double set_trail_points(double value) noexcept { prices_.trail_points = value; return value; }
    double set_trail_price(double value) noexcept { prices_.trail_price = value; return value; }
    double set_trail_offset(double value) noexcept { prices_.trail_offset = value; return value; }
};

struct L4dQuantityRequest {
    double requested = std::numeric_limits<double>::quiet_NaN();
    double reserved = std::numeric_limits<double>::quiet_NaN();
    bool partial = false;
    template <typename T> void request(T) noexcept {}
    void reserve(double request, double held) noexcept { requested = request; reserved = held; }
    bool is_partial(double, double) const noexcept { return partial; }
};

struct L4dFrozenMarketTransaction {
    double transaction_units = std::numeric_limits<double>::quiet_NaN();
    double own_units = std::numeric_limits<double>::quiet_NaN();
};

struct L4dFrozenMarketInstruction {
    bool active_ = false;
    L4dFrozenMarketTransaction transaction_{};
    bool active() const noexcept { return active_; }
    const L4dFrozenMarketTransaction* transaction() const noexcept {
        return active_ ? &transaction_ : nullptr;
    }
    L4dFrozenMarketTransaction* transaction() noexcept {
        return active_ ? &transaction_ : nullptr;
    }
};

struct L4dIntentRow {
    std::string id;
    std::string from_entry;
    std::string comment;
    std::string oca_name;
    L4dOrderType type = L4dOrderType::MARKET;
    bool is_long = true;
    double limit_price = std::numeric_limits<double>::quiet_NaN();
    double stop_price = std::numeric_limits<double>::quiet_NaN();
    double trail_points = std::numeric_limits<double>::quiet_NaN();
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double trail_offset = std::numeric_limits<double>::quiet_NaN();
    double profit_ticks = std::numeric_limits<double>::quiet_NaN();
    double loss_ticks = std::numeric_limits<double>::quiet_NaN();
    double qty = std::numeric_limits<double>::quiet_NaN();
    int qty_type = -1;
    double qty_percent = std::numeric_limits<double>::quiet_NaN();
    int oca_type = 0;
    int created_bar = -1;
    std::int64_t created_seq = 0;
    std::uint64_t incarnation = 0;
    bool over_pyramiding_cap_at_placement = false;
    PositionSide created_position_side = PositionSide::FLAT;
    std::int64_t created_position_cycle_seq = 0;
    double tv_carry_qty = std::numeric_limits<double>::quiet_NaN();
    double frozen_default_qty = std::numeric_limits<double>::quiet_NaN();
    double default_stop_placement_qty = std::numeric_limits<double>::quiet_NaN();
    double default_stop_sizing_price = std::numeric_limits<double>::quiet_NaN();
    double sizing_equity = std::numeric_limits<double>::quiet_NaN();
    double sizing_price = std::numeric_limits<double>::quiet_NaN();
    double sizing_fx = std::numeric_limits<double>::quiet_NaN();
    double sizing_mark = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t replaced_order_incarnation = 0;
    std::uint64_t replaced_default_market_incarnation = 0;
    std::uint64_t recreated_after_named_cancelled_entry_incarnation = 0;
    std::uint64_t named_cancel_surviving_exit_incarnation = 0;
    std::uint64_t same_id_stop_deferred_close_all_incarnation = 0;
    int same_id_stop_deferred_close_all_bar = -1;
    int coof_cascade_seg_i = -1;
    L4dShortSeedRole short_seed_collision_role = L4dShortSeedRole::NONE;
    double signal_close_mc_remaining_qty = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t signal_close_mc_entry_incarnation = 0;
    int signal_close_mc_bar = -1;
    L4dLegs legs{};
    L4dQuantityRequest quantity_request{};
    L4dFrozenMarketInstruction pine_frozen_market_instruction{};
    MarketAdmissionDraft market_admission{};
    OrderCancellationReceipt cancellation{};
};

// A37(4): mutable fixture view over the adapter-owned cancellation receipt.
// It reproduces the retired receipt API without copying the frozen C mirror
// or creating a second execution owner; every successful transition writes
// the PlacementSnapshot fields consumed by both source hashing and copy_v1.
class L4dCancellationReceiptView {
public:
    explicit L4dCancellationReceiptView(PineCancellationReceipt& receipt) noexcept
        : receipt_(receipt) {}

    CancellationResult cancel(CancellationCause cause,
            std::uint64_t source_incarnation, std::int64_t source_sequence,
            CancellationTarget target, CancellationTarget current_target) {
        const bool valid = cause != CancellationCause::None
            && source_incarnation != 0 && source_sequence > 0
            && target.incarnation != 0 && target.owner >= 0
            && target.revision != std::numeric_limits<std::uint64_t>::max()
            && current_target.incarnation == target.incarnation
            && current_target.owner == target.owner
            && current_target.revision == target.revision;
        if (!valid) return CancellationResult::Invalid;
        if (receipt_.state != 0) {
            return receipt_.cause == static_cast<PineCancellationCause>(cause)
                && receipt_.source_incarnation == source_incarnation
                && receipt_.source_sequence == source_sequence
                && receipt_.target_incarnation == target.incarnation
                && receipt_.target_owner == target.owner
                && receipt_.target_revision == target.revision
                ? CancellationResult::Replay
                : CancellationResult::AlreadyTerminal;
        }
        receipt_.cause = static_cast<PineCancellationCause>(cause);
        receipt_.state = 1;
        receipt_.source_incarnation = source_incarnation;
        receipt_.source_sequence = source_sequence;
        receipt_.target_incarnation = target.incarnation;
        receipt_.target_owner = target.owner;
        receipt_.target_revision = target.revision;
        return CancellationResult::Applied;
    }

    bool bind_close_claim(double consumed, double retired) {
        if (receipt_.state != 0 || receipt_.close_claim_release != 0) return false;
        const bool no_claim = std::isnan(consumed) && retired == 0.0;
        const bool valid_claim = std::isfinite(consumed) && consumed > 0.0
            && std::isfinite(retired) && retired >= 0.0;
        if (!no_claim && !valid_claim) return false;
        receipt_.close_claim_consumed = consumed;
        receipt_.close_claim_retired = retired;
        receipt_.close_claim_release = valid_claim ? 2 : 1;
        return true;
    }

    bool release_close_claim_once(double& ledger) {
        if (receipt_.state != 1 || receipt_.close_claim_release != 2
            || !std::isfinite(ledger)) {
            return false;
        }
        const double credit = receipt_.close_claim_consumed
            + receipt_.close_claim_retired;
        if (!std::isfinite(credit) || !std::isfinite(ledger + credit)) return false;
        ledger += credit;
        receipt_.close_claim_release = 3;
        return true;
    }

private:
    PineCancellationReceipt& receipt_;
};

class L4dPineHost : public PineStrategyHost {
protected:
    using PineStrategyHost::fixture_configuration;
    using PineStrategyHost::fixture_default_qty_type_slot;
    using PineStrategyHost::fixture_commission_type_slot;
    using PineStrategyHost::fixture_risk_direction_slot;
    using PineStrategyHost::source_id_ledger_view;
    using PineStrategyHost::source_pending_view;

    const PineStrategyConfig& fixture_configuration() const noexcept {
        return const_cast<L4dPineHost*>(this)->PineStrategyHost::fixture_configuration();
    }
    QtyType fixture_default_qty_type_slot() const noexcept {
        return static_cast<QtyType>(fixture_configuration().default_qty_type);
    }
    CommissionType fixture_commission_type_slot() const noexcept {
        return static_cast<CommissionType>(fixture_configuration().commission_type);
    }
    bool l4d_coof_fill_recalc_active() const noexcept {
        return adapter_.fixture_coof_recalc_active();
    }
    bool l4d_coof_cursor_is_bar_close() const noexcept {
        return adapter_.fixture_coof_cursor_is_bar_close();
    }
    double l4d_close_logical_units(const std::string& id) const noexcept {
        return adapter_.fixture_close_logical_units(id);
    }
    double l4d_close_reserved_units(const std::string& id) const noexcept {
        return adapter_.fixture_close_reserved_units(id);
    }
    double l4d_close_first_units(const std::string& id) const noexcept {
        return adapter_.fixture_close_first_units(id);
    }
    double l4d_callsite_reserved_units(
            std::uint64_t token, const std::string& id) const noexcept {
        return adapter_.fixture_callsite_close_reserved_units(token, id);
    }
    double l4d_callsite_first_units(
            std::uint64_t token, const std::string& id) const noexcept {
        return adapter_.fixture_callsite_close_first_units(token, id);
    }
    std::size_t l4d_close_reservation_count() const noexcept {
        return adapter_.fixture_close_reservation_count();
    }
    std::size_t l4d_close_first_count() const noexcept {
        return adapter_.fixture_close_first_count();
    }
    std::size_t l4d_close_logical_count() const noexcept {
        return adapter_.fixture_close_logical_count();
    }
    std::size_t l4d_callsite_reservation_count() const noexcept {
        return adapter_.fixture_callsite_close_reservation_count();
    }
    std::size_t l4d_callsite_first_count() const noexcept {
        return adapter_.fixture_callsite_close_first_count();
    }
    double l4d_callsite_reserved_total() const noexcept {
        return adapter_.fixture_callsite_close_reserved_total();
    }
    double l4d_close_pending_debt() const noexcept {
        return adapter_.fixture_close_pending_debt();
    }
    double l4d_close_admitted_total() const noexcept {
        return adapter_.fixture_close_admitted_total();
    }
    std::vector<PineExecutionAdapter::FixtureCloseCallsite>
    l4d_close_callsites() const {
        return adapter_.fixture_close_callsites();
    }

    std::vector<L4dIntentRow>& l4d_pending_rows() const {
        l4d_pending_rows_.clear();
        const int count = pending_order_count();
        for (int index = 0; index < count; ++index) {
            pf_pending_order_v1_t row{};
            if (observe_pending_copy_v1(index, &row) != 0) continue;
            L4dIntentRow view;
            view.id = row.id; view.from_entry = row.from_entry; view.comment = row.comment;
            view.oca_name = row.oca_name;
            view.type = row.type == static_cast<int>(L4dOrderType::ENTRY)
                    && !std::isfinite(row.limit_price) && !std::isfinite(row.stop_price)
                    && !std::isfinite(row.trail_points) && !std::isfinite(row.trail_price)
                    && !std::isfinite(row.trail_offset)
                ? L4dOrderType::MARKET : static_cast<L4dOrderType>(row.type);
            view.is_long = row.is_long != 0;
            view.limit_price = row.limit_price; view.stop_price = row.stop_price;
            view.trail_points = row.trail_points; view.trail_price = row.trail_price;
            view.trail_offset = row.trail_offset; view.profit_ticks = row.profit_ticks;
            view.loss_ticks = row.loss_ticks; view.qty = row.qty; view.qty_type = row.qty_type;
            view.qty_percent = row.qty_percent; view.oca_type = row.oca_type;
            view.created_bar = row.created_bar; view.created_seq = row.created_seq;
            view.incarnation = row.incarnation;
            view.over_pyramiding_cap_at_placement =
                row.over_pyramiding_cap_at_placement != 0;
            view.created_position_side = static_cast<PositionSide>(row.created_position_side);
            view.created_position_cycle_seq = row.created_position_cycle_seq;
            view.tv_carry_qty = row.tv_carry_qty; view.frozen_default_qty = row.frozen_default_qty;
            view.default_stop_placement_qty = row.default_stop_placement_qty;
            view.default_stop_sizing_price = row.default_stop_sizing_price;
            view.sizing_equity = row.sizing_equity; view.sizing_price = row.sizing_price;
            view.sizing_fx = row.sizing_fx; view.sizing_mark = row.sizing_mark;
            view.replaced_order_incarnation = row.replaced_order_incarnation;
            view.replaced_default_market_incarnation = row.replaced_default_market_incarnation;
            view.recreated_after_named_cancelled_entry_incarnation = row.recreated_after_named_cancelled_entry_incarnation;
            view.named_cancel_surviving_exit_incarnation = row.named_cancel_surviving_exit_incarnation;
            view.same_id_stop_deferred_close_all_incarnation = row.same_id_stop_deferred_close_all_incarnation;
            view.same_id_stop_deferred_close_all_bar = row.same_id_stop_deferred_close_all_bar;
            view.coof_cascade_seg_i = row.coof_cascade_seg_i;
            view.over_pyramiding_cap_at_placement =
                row.over_pyramiding_cap_at_placement != 0;
            view.short_seed_collision_role =
                static_cast<L4dShortSeedRole>(row.short_seed_collision_role);
            view.signal_close_mc_remaining_qty = row.signal_close_mc_remaining_qty;
            view.signal_close_mc_entry_incarnation = row.signal_close_mc_entry_incarnation;
            view.signal_close_mc_bar = row.signal_close_mc_bar;
            if (row.market_admission_observation_present != 0
                && row.market_admission_observation_command != 0) {
                auto observation = std::make_shared<admission::CommandObservation>();
                observation->command = row.market_admission_observation_command;
                observation->kind = static_cast<admission::CommandKind>(
                    row.market_admission_observation_kind);
                observation->birth = OrderBirth::direct_command(
                    static_cast<int>(row.market_admission_observation_birth_bar),
                    row.market_admission_observation_birth_timestamp);
                observation->id = row.market_admission_observation_id;
                observation->requested_quantity =
                    row.market_admission_observation_requested_quantity;
                observation->quantity_type = static_cast<int>(
                    row.market_admission_observation_quantity_type);
                observation->buy = row.market_admission_observation_buy != 0;
                observation->prices = {row.market_admission_observation_prices_limit,
                                       row.market_admission_observation_prices_stop};
                observation->oca_name = row.market_admission_observation_oca_name;
                observation->oca_type = static_cast<int>(row.market_admission_observation_oca_type);
                auto& configuration = observation->configuration;
                configuration.process_on_close =
                    row.market_admission_observation_configuration_process_on_close != 0;
                configuration.calc_on_fills =
                    row.market_admission_observation_configuration_calc_on_fills != 0;
                configuration.magnifier =
                    row.market_admission_observation_configuration_magnifier != 0;
                configuration.fill_recalculation =
                    row.market_admission_observation_configuration_fill_recalculation != 0;
                configuration.scheduler =
                    row.market_admission_observation_configuration_scheduler != 0;
                configuration.slippage = static_cast<int>(
                    row.market_admission_observation_configuration_slippage);
                configuration.pyramiding = static_cast<int>(
                    row.market_admission_observation_configuration_pyramiding);
                configuration.default_quantity_type = static_cast<int>(
                    row.market_admission_observation_configuration_default_quantity_type);
                configuration.default_quantity_value =
                    row.market_admission_observation_configuration_default_quantity_value;
                configuration.long_margin =
                    row.market_admission_observation_configuration_long_margin;
                configuration.short_margin =
                    row.market_admission_observation_configuration_short_margin;
                configuration.commission_value =
                    row.market_admission_observation_configuration_commission_value;
                configuration.commission_type = static_cast<int>(
                    row.market_admission_observation_configuration_commission_type);
                configuration.pointvalue =
                    row.market_admission_observation_configuration_pointvalue;
                configuration.fx = row.market_admission_observation_configuration_fx;
                configuration.quantity_step =
                    row.market_admission_observation_configuration_quantity_step;
                configuration.mintick =
                    row.market_admission_observation_configuration_mintick;
                configuration.risk_direction = static_cast<int>(
                    row.market_admission_observation_configuration_risk_direction);
                configuration.loss_days_limit = static_cast<int>(
                    row.market_admission_observation_configuration_loss_days_limit);
                configuration.drawdown_limit =
                    row.market_admission_observation_configuration_drawdown_limit;
                configuration.intraday_loss_limit =
                    row.market_admission_observation_configuration_intraday_loss_limit;
                configuration.position_limit =
                    row.market_admission_observation_configuration_position_limit;
                configuration.fill_cap_active =
                    row.market_admission_observation_configuration_fill_cap_active != 0;
                configuration.risk_halted =
                    row.market_admission_observation_configuration_risk_halted != 0;
                observation->bar = static_cast<int>(row.market_admission_observation_bar);
                observation->placement_side = static_cast<int>(
                    row.market_admission_observation_placement_side);
                observation->placement_cycle = row.market_admission_observation_placement_cycle;
                observation->prior_close_quantity =
                    row.market_admission_observation_prior_close_quantity;
                observation->held_quantity = row.market_admission_observation_held_quantity;
                observation->held_entries = static_cast<int>(
                    row.market_admission_observation_held_entries);
                observation->realized_equity = row.market_admission_observation_realized_equity;
                observation->placement_equity = row.market_admission_observation_placement_equity;
                observation->signal_close = row.market_admission_observation_signal_close;
                observation->quantized_fixed_quantity =
                    row.market_admission_observation_quantized_fixed_quantity;
                if (row.market_admission_observation_original_sizing_present != 0) {
                    observation->original_sizing = admission::SizingObservation{
                        row.market_admission_observation_original_sizing_quantity,
                        row.market_admission_observation_original_sizing_equity,
                        row.market_admission_observation_original_sizing_price,
                        row.market_admission_observation_original_sizing_mark,
                        row.market_admission_observation_original_sizing_fx};
                }
                observation->explicit_equity =
                    row.market_admission_observation_explicit_equity;
                observation->explicit_price = row.market_admission_observation_explicit_price;
                view.market_admission.bind(std::move(observation));
            }
            view.legs.set_limit_price(row.limit_price);
            view.legs.set_stop_price(row.stop_price);
            view.legs.set_trail_points(row.trail_points);
            view.legs.set_trail_price(row.trail_price);
            view.legs.set_trail_offset(row.trail_offset);
            view.legs.attach(row.incarnation, row.created_position_cycle_seq);
            view.pine_frozen_market_instruction.active_ = row.pine_frozen_market_instruction_kind != 0;
            view.pine_frozen_market_instruction.transaction_.own_units =
                row.pine_frozen_market_instruction_own_units;
            view.pine_frozen_market_instruction.transaction_.transaction_units =
                row.pine_frozen_market_instruction_transaction_units;
            l4d_pending_rows_.push_back(std::move(view));
        }
        for (const auto& fixture : source_pending_view()) {
            const bool present = std::any_of(
                l4d_pending_rows_.begin(), l4d_pending_rows_.end(),
                [&](const L4dIntentRow& row) { return row.id == fixture.id; });
            if (present) continue;
            L4dIntentRow view;
            view.id = fixture.id;
            view.from_entry = fixture.from_entry;
            switch (fixture.type) {
            case FixtureIntentKind::MARKET: view.type = L4dOrderType::MARKET; break;
            case FixtureIntentKind::ENTRY: view.type = L4dOrderType::ENTRY; break;
            case FixtureIntentKind::EXIT: view.type = L4dOrderType::EXIT; break;
            case FixtureIntentKind::RAW_ORDER: view.type = L4dOrderType::RAW_ORDER; break;
            }
            view.is_long = fixture.is_long;
            view.qty = fixture.qty;
            view.qty_percent = fixture.qty_percent;
            view.created_bar = static_cast<int>(fixture.created_bar);
            view.created_seq = fixture.created_seq;
            view.incarnation = fixture.incarnation;
            view.over_pyramiding_cap_at_placement =
                fixture.over_pyramiding_cap_at_placement;
            view.frozen_default_qty = fixture.frozen_default_qty;
            view.default_stop_placement_qty = fixture.default_stop_placement_qty;
            view.default_stop_sizing_price = fixture.default_stop_sizing_price;
            view.sizing_equity = fixture.default_stop_placement_equity;
            view.market_admission = fixture.market_admission;
            l4d_pending_rows_.push_back(std::move(view));
        }
        return l4d_pending_rows_;
    }

public:
    const L4dIntentRow& pending_order_at(int index) const {
        return l4d_pending_rows().at(static_cast<std::size_t>(index));
    }

    int probe_fill_qty(int index, double fill_price, double* qty,
                       int* close_only, int* partition) const {
        const bool injected_short_seed = std::any_of(
            l4d_pending_rows_.begin(), l4d_pending_rows_.end(),
            [](const L4dIntentRow& row) {
                return row.short_seed_collision_role != L4dShortSeedRole::NONE;
            });
        if (!injected_short_seed) {
            const int result = PineStrategyHost::probe_fill_qty(
                index, fill_price, qty, close_only, partition);
            pf_pending_order_v1_t row{};
            if (result == 0 && partition && *partition == 1
                && observe_pending_copy_v1(index, &row) == 0
                && row.type == static_cast<std::int32_t>(L4dOrderType::ENTRY)
                && std::isnan(row.qty) && std::isnan(row.limit_price)
                && std::isfinite(row.stop_price)
                && std::isfinite(row.default_stop_placement_qty)) {
                // The deleted owner named this frozen-stop branch partition
                // 2; the generic frozen-placement projection uses 1. Preserve
                // the historical fixture spelling without changing product
                // execution or the L0 public oracle.
                *partition = 2;
            }
            return result;
        }
        if (!qty || !close_only || !partition || index < 0
            || index >= static_cast<int>(l4d_pending_rows_.size())) return -1;
        const auto& row = l4d_pending_rows_[static_cast<std::size_t>(index)];
        *close_only = 0;
        if (row.short_seed_collision_role == L4dShortSeedRole::FINAL_SHORT
            && pyramid_entries_.size() >= 2U) {
            *qty = pyramid_entries_[0].qty - pyramid_entries_[1].qty;
            *partition = 1;
            *close_only = *qty > 1e-10 ? 0 : 1;
            return 0;
        }
        *qty = fixture_configuration().default_qty_value;
        *partition = 3;
        return 0;
    }

private:
    mutable std::vector<L4dIntentRow> l4d_pending_rows_;
};

using L4dPendingOrder = L4dIntentRow;

}  // namespace pineforge::source

namespace pineforge {

using L4dOrderType = source::L4dOrderType;
using L4dPendingOrder = source::L4dPendingOrder;
using L4dShortSeedRole = source::L4dShortSeedRole;

inline bool placement_has_opposite_market_predecessor(
        const MarketAdmissionJournal&, const L4dPendingOrder&) noexcept {
    return false;
}
inline bool placement_at_entry_capacity(const L4dPendingOrder& order) noexcept {
    return order.over_pyramiding_cap_at_placement;
}

}  // namespace pineforge
