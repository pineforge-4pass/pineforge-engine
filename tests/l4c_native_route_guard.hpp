#pragma once

#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_native_host.hpp>

#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifndef PINEFORGE_HAS_NATIVE_LOWERING_V1
#error "L4c native-route twins require the v17 native lowering surface"
#endif

// The twins below are read-only test projections over the public pending
// mirror.  They deliberately have no mutation or matching API: commands still
// enter through on_source_bar and NativeExecutionConsumer remains the sole
// execution owner.
namespace pineforge::source {

enum class L4cOrderType { MARKET = 0, ENTRY = 1, EXIT = 2, RAW_ORDER = 3 };

struct L4cBirthCursor {
    BirthCursorDomain domain_value = BirthCursorDomain::None;
    BirthCursorPosition position_value = BirthCursorPosition::None;
    int index_value = -1;
    int count_value = 0;

    BirthCursorDomain domain() const noexcept { return domain_value; }
    BirthCursorPosition position() const noexcept { return position_value; }
    int index() const noexcept { return index_value; }
    int count() const noexcept { return count_value; }
    bool first_point() const noexcept {
        return position_value == BirthCursorPosition::Point && index_value == 0;
    }
    bool terminal_point() const noexcept {
        return position_value == BirthCursorPosition::Point && index_value + 1 == count_value;
    }
};

struct L4cOrderBirth {
    OrderBirthCause cause_value = OrderBirthCause::Unattributed;
    int bar_value = -1;
    std::int64_t timestamp_value = 0;
    L4cBirthCursor cursor_value{};
    double cursor_price_value = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t first_fill_value = 0;
    std::uint64_t last_fill_value = 0;
    std::uint64_t evaluation_ordinal_value = 0;

    OrderBirthCause cause() const noexcept { return cause_value; }
    bool from_fill() const noexcept { return cause_value == OrderBirthCause::FillEvaluation; }
    bool at_terminal_fill() const noexcept { return from_fill() && cursor_value.terminal_point(); }
    int bar() const noexcept { return bar_value; }
    std::int64_t timestamp() const noexcept { return timestamp_value; }
    const L4cBirthCursor& cursor() const noexcept { return cursor_value; }
    double cursor_price() const noexcept { return cursor_price_value; }
    std::uint64_t first_fill() const noexcept { return first_fill_value; }
    std::uint64_t last_fill() const noexcept { return last_fill_value; }
    std::uint64_t evaluation_ordinal() const noexcept { return evaluation_ordinal_value; }
};

struct L4cLegActivationBounds {
    std::int64_t position_cycle = 0;
    std::int64_t stop_first_bar = 0;
    std::int64_t limit_first_bar = 0;
};

struct L4cLegActivation {
    std::optional<L4cLegActivationBounds> value{};
    const std::optional<L4cLegActivationBounds>& bounds() const noexcept { return value; }
};

struct L4cExitActivationEvidence {
    std::int64_t position_cycle = 0;
    int entry_bar = -1;
    int direction = 0;
    double cursor_price = std::numeric_limits<double>::quiet_NaN();
    double stop_level = std::numeric_limits<double>::quiet_NaN();
    double limit_level = std::numeric_limits<double>::quiet_NaN();
};

struct L4cExitActivation {
    bool hold_stop = false;
    bool hold_limit = false;
    std::optional<L4cExitActivationEvidence> value{};

    bool holds_stop() const noexcept { return hold_stop; }
    bool holds_limit() const noexcept { return hold_limit; }
    const std::optional<L4cExitActivationEvidence>& evidence() const noexcept { return value; }
};

struct L4cExitLegPrices {
    double limit_price = std::numeric_limits<double>::quiet_NaN();
    double stop_price = std::numeric_limits<double>::quiet_NaN();
    double trail_points = std::numeric_limits<double>::quiet_NaN();
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double trail_offset = std::numeric_limits<double>::quiet_NaN();
    double profit_ticks = std::numeric_limits<double>::quiet_NaN();
    double loss_ticks = std::numeric_limits<double>::quiet_NaN();
};

struct L4cExitLegs {
    L4cExitLegPrices price_values{};
    bool dormant_value = false;
    bool pending_replacement_value = false;
    double original_stop_value = std::numeric_limits<double>::quiet_NaN();

    const L4cExitLegPrices& prices() const noexcept { return price_values; }
    bool dormant() const noexcept { return dormant_value; }
    bool pending_replacement() const noexcept { return pending_replacement_value; }
    double original_stop() const noexcept { return original_stop_value; }
};

struct L4cReservationExpansion {
    bool present = false;
    bool population_open_value = false;
    bool first_later_admission_present = false;

    bool population_open() const noexcept { return present && population_open_value; }
};

struct L4cPendingOrder {
    std::string id{};
    std::string from_entry{};
    L4cOrderType type = L4cOrderType::MARKET;
    bool is_long = false;
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
    std::string oca_name{};
    int oca_type = 0;
    int created_bar = -1;
    std::int64_t created_seq = 0;
    std::uint64_t incarnation = 0;
    std::uint64_t replaced_order_incarnation = 0;
    std::uint64_t recreated_after_named_cancelled_entry_incarnation = 0;
    std::uint64_t named_cancel_surviving_exit_incarnation = 0;
    PositionSide created_position_side = PositionSide::FLAT;
    std::int64_t created_position_cycle_seq = 0;
    bool stop_limit_activated = false;
    L4cLegActivation leg_activation{};
    L4cExitActivation pine_exit_activation{};
    L4cOrderBirth birth{};
    L4cExitLegs legs{};
    L4cReservationExpansion reservation_expansion{};
    compat::pine::HistoricalBirthReach pine_birth_reach =
        compat::pine::HistoricalBirthReach::Standard;
};

class L4cFixtureHost : public PineStrategyHost {
public:
    PineStrategyConfig& fixture_configuration() noexcept {
        return PineStrategyHost::fixture_configuration();
    }
    const PineStrategyConfig& fixture_configuration() const noexcept {
        return const_cast<L4cFixtureHost*>(this)->fixture_configuration();
    }

    const std::vector<L4cPendingOrder>& l4c_pending_orders() const {
        std::vector<L4cPendingOrder> result;
        const PendingIntentView& view = pending_intent_view();
        const int count = view.size();
        result.reserve(count > 0 ? static_cast<std::size_t>(count) : 0U);
        for (int index = 0; index < count; ++index) {
            pf_pending_order_v1_t row{};
            if (view.copy_v1(index, &row) != 0) continue;
            // Staged rows have no generic request handle. The fixture facade
            // below supplies their read-only legacy incarnation projection;
            // do not retain the public handle-zero copy alongside it.
            if (row.incarnation == 0) continue;
            L4cPendingOrder projection;
            projection.id = row.id;
            projection.from_entry = row.from_entry;
            projection.type = row.type == static_cast<int>(L4cOrderType::ENTRY)
                    && !std::isfinite(row.limit_price) && !std::isfinite(row.stop_price)
                    && !std::isfinite(row.trail_points) && !std::isfinite(row.trail_price)
                    && !std::isfinite(row.trail_offset)
                ? L4cOrderType::MARKET : static_cast<L4cOrderType>(row.type);
            projection.is_long = row.is_long != 0U;
            projection.limit_price = row.limit_price;
            projection.stop_price = row.stop_price;
            projection.trail_points = row.trail_points;
            projection.trail_price = row.trail_price;
            projection.trail_offset = row.trail_offset;
            projection.profit_ticks = row.profit_ticks;
            projection.loss_ticks = row.loss_ticks;
            projection.qty = row.qty;
            projection.qty_type = row.qty_type;
            projection.qty_percent = row.qty_percent;
            projection.oca_name = row.oca_name;
            projection.oca_type = row.oca_type;
            projection.created_bar = row.created_bar;
            projection.created_seq = row.created_seq;
            projection.incarnation = row.incarnation;
            projection.replaced_order_incarnation = row.replaced_order_incarnation;
            projection.recreated_after_named_cancelled_entry_incarnation =
                row.recreated_after_named_cancelled_entry_incarnation;
            projection.named_cancel_surviving_exit_incarnation =
                row.named_cancel_surviving_exit_incarnation;
            projection.created_position_side = static_cast<PositionSide>(row.created_position_side);
            projection.created_position_cycle_seq = row.created_position_cycle_seq;
            projection.stop_limit_activated = row.stop_limit_activated != 0U;
            if (row.leg_activation_present != 0U) {
                projection.leg_activation.value = {row.leg_activation_owner_cycle,
                                                   static_cast<int>(row.leg_activation_stop_first_bar),
                                                   static_cast<int>(row.leg_activation_limit_first_bar)};
            }
            projection.pine_exit_activation.hold_stop =
                row.coof_suppress_stop_on_entry_bar != 0U;
            projection.pine_exit_activation.hold_limit =
                row.coof_suppress_limit_on_entry_bar != 0U;
            if (row.pine_exit_activation_present != 0U) {
                projection.pine_exit_activation.value = {
                    row.pine_exit_activation_owner_cycle_at_birth,
                    row.pine_exit_activation_entry_bar_at_birth,
                    row.pine_exit_activation_direction_at_birth,
                    row.pine_exit_activation_cursor_price_at_birth,
                    row.pine_exit_activation_stop_level_at_birth,
                    row.pine_exit_activation_limit_level_at_birth};
            }
            projection.birth.cause_value = static_cast<OrderBirthCause>(row.birth_cause);
            projection.birth.bar_value = row.birth_bar;
            projection.birth.timestamp_value = row.birth_timestamp;
            projection.birth.cursor_value = {
                static_cast<BirthCursorDomain>(row.birth_cursor_domain),
                static_cast<BirthCursorPosition>(row.birth_cursor_position),
                row.birth_cursor_index, row.birth_cursor_count};
            projection.birth.cursor_price_value = row.birth_cursor_price;
            projection.birth.first_fill_value = row.birth_first_fill;
            projection.birth.last_fill_value = row.birth_last_fill;
            projection.birth.evaluation_ordinal_value = row.birth_evaluation_ordinal;
            projection.legs.price_values = {row.legs_definition_limit_price,
                                             row.legs_definition_stop_price,
                                             row.legs_definition_trail_points,
                                             row.legs_definition_trail_price,
                                             row.legs_definition_trail_offset,
                                             row.legs_definition_profit_ticks,
                                             row.legs_definition_loss_ticks};
            projection.legs.dormant_value = row.dormant_bracket != 0U;
            projection.legs.pending_replacement_value = row.dormant_reissue_pending != 0U;
            projection.legs.original_stop_value = row.dormant_original_stop_price;
            projection.reservation_expansion.present = row.reservation_expansion_present != 0U;
            projection.reservation_expansion.population_open_value =
                row.pooc_global_full_exit_dynamic_qty != 0U;
            projection.reservation_expansion.first_later_admission_present =
                row.reservation_expansion_first_later_admission_present != 0U;
            projection.pine_birth_reach =
                static_cast<compat::pine::HistoricalBirthReach>(row.pine_birth_reach);
            result.push_back(std::move(projection));
        }
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            if (!row.staged) continue;
            if (row.incarnation != 0
                && std::any_of(result.begin(), result.end(), [&](const L4cPendingOrder& value) {
                    return value.incarnation == row.incarnation;
                })) {
                continue;
            }
            const PlacementSnapshot& snapshot = row.snapshot;
            L4cPendingOrder projection;
            projection.incarnation = row.incarnation;
            projection.id = snapshot.source_id;
            projection.from_entry = snapshot.from_entry;
            switch (snapshot.family) {
            case PineOrderFamily::Entry:
                projection.type = L4cOrderType::ENTRY;
                break;
            case PineOrderFamily::Order:
                projection.type = L4cOrderType::RAW_ORDER;
                break;
            case PineOrderFamily::Close:
            case PineOrderFamily::CloseAll:
            case PineOrderFamily::ExitLimit:
            case PineOrderFamily::ExitStop:
            case PineOrderFamily::ExitTrail:
            case PineOrderFamily::Margin:
                projection.type = L4cOrderType::EXIT;
                break;
            }
            projection.is_long = snapshot.is_long;
            projection.limit_price = snapshot.exit_levels.limit;
            projection.stop_price = snapshot.exit_levels.stop;
            projection.trail_points = snapshot.exit_levels.trail_points;
            projection.trail_price = snapshot.exit_levels.trail_price;
            projection.trail_offset = snapshot.exit_levels.trail_offset;
            projection.profit_ticks = snapshot.exit_levels.profit_ticks;
            projection.loss_ticks = snapshot.exit_levels.loss_ticks;
            projection.qty = snapshot.requested_qty;
            projection.qty_type = snapshot.qty_type;
            projection.qty_percent = snapshot.qty_percent;
            projection.oca_name = snapshot.oca_name;
            projection.oca_type = snapshot.oca_type;
            projection.created_bar = snapshot.projection_created_bar;
            projection.created_seq = static_cast<std::int64_t>(snapshot.source_sequence);
            projection.replaced_order_incarnation = snapshot.projection_predecessor;
            projection.recreated_after_named_cancelled_entry_incarnation =
                snapshot.recreated_after_named_cancelled_entry_incarnation;
            projection.named_cancel_surviving_exit_incarnation =
                snapshot.named_cancel_surviving_exit_incarnation;
            projection.created_position_side =
                static_cast<PositionSide>(snapshot.projection_position_side);
            projection.created_position_cycle_seq = snapshot.placement_cycle;
            if (const auto& bounds = snapshot.leg_activation.bounds()) {
                projection.leg_activation.value = {bounds->position_cycle,
                                                   bounds->stop_first_bar,
                                                   bounds->limit_first_bar};
            }
            projection.pine_exit_activation.hold_stop = snapshot.exit_activation.holds_stop();
            projection.pine_exit_activation.hold_limit = snapshot.exit_activation.holds_limit();
            projection.birth.cause_value = snapshot.birth.cause();
            projection.birth.bar_value = snapshot.birth.bar();
            projection.birth.timestamp_value = snapshot.birth.timestamp();
            projection.birth.cursor_value = {snapshot.birth.cursor().domain(),
                                              snapshot.birth.cursor().position(),
                                              snapshot.birth.cursor().index(),
                                              snapshot.birth.cursor().count()};
            projection.birth.cursor_price_value = snapshot.birth.cursor_price();
            projection.birth.first_fill_value = snapshot.birth.first_fill();
            projection.birth.last_fill_value = snapshot.birth.last_fill();
            projection.birth.evaluation_ordinal_value = snapshot.birth.evaluation_ordinal();
            projection.legs.price_values = {snapshot.exit_levels.limit,
                                             snapshot.exit_levels.stop,
                                             snapshot.exit_levels.trail_points,
                                             snapshot.exit_levels.trail_price,
                                             snapshot.exit_levels.trail_offset,
                                             snapshot.exit_levels.profit_ticks,
                                             snapshot.exit_levels.loss_ticks};
            projection.legs.dormant_value = snapshot.legs.dormant();
            projection.legs.pending_replacement_value = snapshot.legs.pending_replacement();
            projection.legs.original_stop_value = snapshot.legs.original_stop();
            projection.reservation_expansion.present =
                snapshot.reservation_expansion.capture().has_value();
            projection.reservation_expansion.population_open_value =
                snapshot.pooc_global_full_exit_dynamic_qty;
            projection.reservation_expansion.first_later_admission_present =
                projection.reservation_expansion.present
                && snapshot.reservation_expansion.capture()->first_later_admission.has_value();
            projection.pine_birth_reach = snapshot.birth_reach;
            result.push_back(std::move(projection));
        }
        const auto same = [](const L4cPendingOrder& left,
                             const L4cPendingOrder& right) {
            const auto equal_number = [](double lhs, double rhs) {
                return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
            };
            return left.id == right.id && left.from_entry == right.from_entry
                && left.type == right.type && left.incarnation == right.incarnation
                && left.replaced_order_incarnation == right.replaced_order_incarnation
                && left.recreated_after_named_cancelled_entry_incarnation
                    == right.recreated_after_named_cancelled_entry_incarnation
                && left.named_cancel_surviving_exit_incarnation
                    == right.named_cancel_surviving_exit_incarnation
                && left.created_seq == right.created_seq
                && left.created_bar == right.created_bar
                && equal_number(left.qty, right.qty)
                && equal_number(left.limit_price, right.limit_price)
                && equal_number(left.stop_price, right.stop_price);
        };
        const bool unchanged = result.size() == pending_cache_.size()
            && std::equal(result.begin(), result.end(), pending_cache_.begin(), same);
        if (!unchanged) pending_cache_ = std::move(result);
        return pending_cache_;
    }

    bool l4c_coof_recalc_active() const noexcept {
        return adapter_.fixture_coof_recalc_active();
    }
    bool l4c_coof_cursor_is_bar_close() const noexcept {
        return adapter_.fixture_coof_cursor_is_bar_close();
    }
    const std::vector<std::uint64_t>& l4c_callsite_close_callsites() const noexcept {
        static const std::vector<std::uint64_t> none;
        return none;
    }
    bool l4c_named_entry_cancel_active(const std::string& id) const noexcept {
        return adapter_.fixture_named_entry_cancel_active(id);
    }
    void l4c_remove_entry_without_named_cancel(const std::string& id) {
        adapter_.fixture_remove_entry_without_named_cancel(id);
    }
    std::uint64_t& l4c_exit_leg_event_seq() noexcept { return l4c_exit_leg_event_seq_; }

private:
    std::uint64_t l4c_exit_leg_event_seq_ = 0;
    mutable std::vector<L4cPendingOrder> pending_cache_;
};

} // namespace pineforge::source

namespace pineforge {
using source::L4cOrderType;
using source::L4cPendingOrder;
} // namespace pineforge

#define PINEFORGE_L4C_NATIVE_ROUTE_TWIN 1
