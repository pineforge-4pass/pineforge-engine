#pragma once
/*
 * trail_exit_product_probe.hpp — one-bar trailing-exit scenarios run through
 * the PRODUCT and read back from the kernel's own event record.
 *
 * R5 lane P9. The three trail suites (test_trail_open_arm_subtick_offset,
 * test_trail_fill_snap, test_trail_activation_tick_bar) used to pin their
 * finest-grained rows against `internal::resolve_exit_path_fill`, the
 * TradingView exit-path resolver that lane N10 carried out of the shipped
 * layer into tests/exit_path_resolver_oracle.hpp because no production
 * caller reached it any more. That header was a second fill simulation kept
 * alive only for those rows. This probe replaces it with the one simulation
 * the repository ships: the Pine adapter lowers strategy.exit's trail
 * arguments to a kernel request exactly as it does for every corpus
 * strategy, NativeExecutionConsumer walks the bar's synthesized
 * O -> X1 -> X2 -> C path, and the fill is read from the public record
 * (NativeStrategyHost::native_events): the applied fill's modeled price and
 * match cursor, the terms event's price kind, the request's trigger kind.
 * Nothing here simulates a fill; nothing here is reachable from src/.
 *
 * Scenario shape, the resolver's seven arguments as three script bars:
 *   bar 0  the signal bar, degenerate at `entry`: the market entry is issued
 *          at its calculation;
 *   bar 1  O = entry (the entry fills at this open), C = best_start: the exit
 *          is issued at this bar's calculation, so the trail's carried
 *          running extreme is the placement close — TradingView restarts a
 *          trail's best at the issuing close (round 9 family Z) and the
 *          adapter reads "already reached" against that same price — which
 *          is what the resolver's `trail_best_start` argument modelled;
 *   bar 2  the probe bar: the resting exit sees it from its open.
 * Fixed 1 unit, no commission, no slippage, orders processed at the next
 * bar's open: the resolver's is_entry_bar = false, magnifier = false form.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/native_order.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pineforge::trail_probe {

constexpr double kProbeNaN = std::numeric_limits<double>::quiet_NaN();

struct TrailExitScenario {
    Bar bar{};                    // the probe bar (script bar 2)
    bool is_long = true;
    double trail_points = kProbeNaN;   // strategy.exit's spelling (ticks)
    double trail_offset = kProbeNaN;   // strategy.exit's spelling (ticks; NaN = omitted)
    double entry = kProbeNaN;          // the position's entry price
    double best_start = kProbeNaN;     // the carried running extreme (NaN: the entry itself)
    double mintick = 0.01;
};

// The public facts of the probe bar's closing fill, as the kernel records
// them, beside the booked outcome the source host reports.
struct TrailExitProjection {
    // The position closed on the probe bar.
    bool filled = false;
    // The booked exit price (closed_trade_exit_price): TradingView's number.
    double exit_price = kProbeNaN;
    int exit_bar = -1;
    // ExecutionAppliedEvent::raw_price — the matcher's modeled price of the
    // fill: the request's level on a crossing, the cursor's print otherwise.
    double raw_price = kProbeNaN;
    // The fill's match cursor is the bar's open point.
    bool at_bar_open = false;
    // Where on the O -> X1 -> X2 -> C path the fill happened, in the
    // legacy first_touch_position units (0 = open, 1 and 2 = the extremes
    // in the bar's leg order, 3 = close, fractional inside a segment):
    // waypoint index - 1 + the cursor's t; 0 at the open point.
    double path_position = kProbeNaN;
    // The kernel trigger the adapter lowered the exit to.
    bool leg_is_trail = false;    // native_order::Trail (a trailing distance)
    bool leg_is_limit = false;    // native_order::Limit (the one-shot activation spelling)
    bool leg_is_market = false;   // native_order::Market (activation already reached at placement)
    bool leg_is_stop = false;     // native_order::Stop
    // TermsResolvedEvent price kind: TriggerLevel (a level crossing) or the
    // point's own print (PointPrice).
    bool level_fill = false;
    double position_after = kProbeNaN;
    std::string error;
};

namespace detail {

inline Bar degenerate_bar(double price, int64_t ts) {
    Bar b{};
    b.open = price; b.high = price; b.low = price; b.close = price;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// NativePathOrder::Auto, the rule every synthesized path uses.
inline bool path_high_first(const Bar& bar) {
    return std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
}

inline int waypoint_index(const Bar& bar, NativePathPhase phase) {
    const bool high_first = path_high_first(bar);
    switch (phase) {
    case NativePathPhase::Open: return 0;
    case NativePathPhase::High: return high_first ? 1 : 2;
    case NativePathPhase::Low: return high_first ? 2 : 1;
    case NativePathPhase::Close: return 3;
    case NativePathPhase::None: break;
    }
    return -1;
}

class ProbeHost : public pineforge::source::PineStrategyHost {
public:
    ProbeHost(const TrailExitScenario& scenario) : scenario_(scenario) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_value = 0.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.slippage = 0;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        syminfo_mintick_ = scenario.mintick;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("E", scenario_.is_long, kProbeNaN, kProbeNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            strategy_exit("x", "E", /*limit=*/kProbeNaN, /*stop=*/kProbeNaN,
                          scenario_.trail_points, scenario_.trail_offset,
                          /*trail_price=*/kProbeNaN);
        }
    }

    // The closed-trade accessors are protected on the engine; the twins'
    // engine-level fixtures forward them the same way.
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }

private:
    TrailExitScenario scenario_;
};

}  // namespace detail

// Run the scenario and project the probe bar's closing fill.
inline TrailExitProjection trail_exit(const TrailExitScenario& scenario) {
    using namespace detail;
    TrailExitProjection out;
    const double placement = std::isfinite(scenario.best_start) ? scenario.best_start
                                                                : scenario.entry;
    Bar issue = degenerate_bar(scenario.entry, 2000);
    issue.close = placement;
    issue.high = std::max(scenario.entry, placement);
    issue.low = std::min(scenario.entry, placement);
    Bar probe = scenario.bar;
    probe.timestamp = 3000;
    if (probe.volume <= 0.0) probe.volume = 1000.0;
    const std::vector<Bar> bars = {degenerate_bar(scenario.entry, 1000), issue, probe};

    ProbeHost host(scenario);
    host.run(bars.data(), static_cast<int>(bars.size()));
    out.error = host.last_error();
    out.position_after = host.physical_position().signed_units;
    if (host.trade_count() >= 1) {
        out.exit_bar = host.exit_bar(0);
        out.filled = out.exit_bar == 2;
        if (out.filled) out.exit_price = host.exit_price(0);
    }
    if (!out.filled) return out;

    std::optional<native_order::RequestHandle> closing;
    native_order::MatchCursor cursor{};
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!applied || applied->closed_units == 0.0) continue;
        closing = applied->handle();
        cursor = applied->cursor;
        out.raw_price = applied->raw_price;
        const auto& trigger = applied->request().trigger;
        out.leg_is_trail = std::holds_alternative<native_order::Trail>(trigger);
        out.leg_is_limit = std::holds_alternative<native_order::Limit>(trigger);
        out.leg_is_market = std::holds_alternative<native_order::Market>(trigger);
        out.leg_is_stop = std::holds_alternative<native_order::Stop>(trigger);
        out.at_bar_open = cursor.point.path_phase == NativePathPhase::Open;
        const int index = waypoint_index(probe, cursor.point.path_phase);
        out.path_position = index == 0 ? 0.0
            : (index > 0 ? static_cast<double>(index - 1) + cursor.t : kProbeNaN);
    }
    if (closing) {
        for (const auto& event : host.native_events(0)) {
            if (!event.command) continue;
            const auto* terms = std::get_if<native_order::TermsResolvedEvent>(&*event.command);
            if (!terms || terms->handle() != *closing || !(terms->cursor == cursor)) continue;
            out.level_fill = terms->input.price_kind
                == native_order::NativeCandidatePriceKind::TriggerLevel;
        }
    }
    return out;
}

}  // namespace pineforge::trail_probe
