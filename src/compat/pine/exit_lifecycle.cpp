#include <pineforge/compat/pine/exit_lifecycle.hpp>

#include "../../engine_internal.hpp"
#include <pineforge/compat/pine/trail_ticks.hpp>

#include <cmath>

namespace pineforge::compat::pine {

std::optional<exit_legs::Operation> select_exit_suspension(
        const exit_legs::Lifecycle& lifecycle, const ExitSuspensionContext& context) {
    if (context.open_slice_this_bar || !context.standing) return std::nullopt;
    const auto& prices = lifecycle.prices();
    const bool has_trail = !std::isnan(prices.trail_points) || !std::isnan(prices.trail_price);
    if (std::isnan(prices.stop_price) && std::isnan(prices.limit_price) && !has_trail)
        return std::nullopt;
    exit_legs::ObservationWindow window{context.cause, exit_legs::absent(), exit_legs::absent()};
    std::vector<exit_legs::Leg> retire;
    if (has_trail) {
        window.best = window.prefix = context.prior_best;
        double activation = prices.trail_price;
        if (!std::isnan(prices.trail_points)) {
            const double ticks = trail_points_to_ticks(prices.trail_points);
            activation = snap_trail_level_to_tick_grid(
                context.direction > 0
                    ? context.position_entry_price + ticks * context.tick
                    : context.position_entry_price - ticks * context.tick,
                context.tick);
        }
        if (std::isfinite(activation) && std::isfinite(context.open)
            && (context.direction > 0 ? context.open >= activation : context.open <= activation)) {
            retire.push_back(exit_legs::Leg::Trail);
        }
    }
    return exit_legs::Suspend{{exit_legs::Leg::Stop, exit_legs::Leg::Limit}, {}, window,
                              retire};
}

exit_legs::Operation select_pair_hold(const exit_legs::Lifecycle& lifecycle,
                                      exit_legs::Frame cause) {
    if (lifecycle.dormant()) return exit_legs::CancelDeferredActivation{};
    return exit_legs::Suspend{{exit_legs::Leg::Stop, exit_legs::Leg::Limit},
                               exit_legs::Barrier{cause}, {}, {}};
}

exit_legs::Definition select_replacement_revival_definition(
        const exit_legs::Lifecycle& lifecycle) {
    if (lifecycle.pending_replacement())
        return lifecycle.suspension()->replacement->revival_definition;
    return lifecycle.definition(lifecycle.target().incarnation);
}

double select_margin_revival_stop(const exit_legs::Lifecycle& lifecycle) {
    const double original = lifecycle.original_stop();
    return std::isfinite(original) ? original : lifecycle.prices().stop_price;
}

std::optional<exit_legs::Operation> select_exit_completion(
        const exit_legs::Lifecycle& lifecycle, exit_legs::Frame completed) {
    if (completed.domain == exit_legs::Domain::RawTicks
        || completed.phase != exit_legs::Phase::AfterMargin) {
        return std::nullopt;
    }
    const auto target = lifecycle.release_barrier();
    if (!target) return std::nullopt;
    return exit_legs::CompleteBarrier{completed, target};
}

} // namespace pineforge::compat::pine
