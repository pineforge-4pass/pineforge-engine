#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include "../../engine_internal.hpp"
namespace pineforge::compat::pine {
std::optional<exit_legs::Operation> select_exit_suspension(
        const source::PendingOrder& o, const ExitSuspensionContext& c) {
    if (c.open_slice_this_bar || !c.standing || o.type != OrderType::EXIT
        || o.cancellation.cancelled()
        || o.id.compare(0, internal::kClosePrefix.size(), internal::kClosePrefix) == 0)
        return std::nullopt;
    const auto& p = o.legs.prices();
    const bool has_trail = !std::isnan(p.trail_points) || !std::isnan(p.trail_price);
    if (std::isnan(p.stop_price) && std::isnan(p.limit_price) && !has_trail) return std::nullopt;
    exit_legs::ObservationWindow window{c.cause, exit_legs::absent(), exit_legs::absent()};
    std::vector<exit_legs::Leg> retire;
    if (has_trail) {
        window.best = window.prefix = c.prior_best;
        double activation = p.trail_price;
        if (!std::isnan(p.trail_points)) {
            const double ticks = internal::trail_points_to_ticks(p.trail_points);
            activation = internal::snap_trail_level_to_tick_grid(
                c.direction > 0 ? c.position_entry_price + ticks * c.tick
                                : c.position_entry_price - ticks * c.tick, c.tick);
        }
        if (std::isfinite(activation) && std::isfinite(c.open)
            && (c.direction > 0 ? c.open >= activation : c.open <= activation))
            retire.push_back(exit_legs::Leg::Trail);
    }
    return exit_legs::Suspend{{exit_legs::Leg::Stop, exit_legs::Leg::Limit},
                            {}, window, retire};
}
exit_legs::Operation select_pair_hold(const source::PendingOrder& o, exit_legs::Frame cause) {
    if (o.legs.dormant()) return exit_legs::CancelDeferredActivation{};
    return exit_legs::Suspend{{exit_legs::Leg::Stop, exit_legs::Leg::Limit},
                             exit_legs::Barrier{cause}, {}, {}};
}
exit_legs::Definition select_replacement_revival_definition(const source::PendingOrder& o) {
    if (o.legs.pending_replacement()) return *o.legs.suspension()->revival_definition;
    return o.legs.definition(o.incarnation);
}
double select_margin_revival_stop(const source::PendingOrder& o) {
    const double original = o.legs.original_stop();
    return std::isfinite(original) ? original : o.legs.prices().stop_price;
}
std::optional<exit_legs::Operation> select_exit_completion(
        const source::PendingOrder& o, exit_legs::Frame completed) {
    // Absence of a Pine raw-tick release hook is frontend timing policy.
    // The native reducer can fulfill an explicitly targeted RawTicks barrier.
    if (completed.domain == exit_legs::Domain::RawTicks
        || completed.phase != exit_legs::Phase::AfterMargin) return std::nullopt;
    const auto target = o.legs.release_barrier();
    if (!target) return std::nullopt;
    return exit_legs::CompleteBarrier{completed, target};
}
} // namespace pineforge::compat::pine
