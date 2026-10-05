#pragma once
// Per-rule switches for the callback and lifecycle pins (TradingView
// synthetic tapes, tests/fixtures/callback_lifecycle). Every switch is ON
// in the shipped library. They exist so a regression can be bisected one
// rule at a time: only tests clear them (test_callback_lifecycle_tapes
// clears each in turn and requires exactly its tapes to depart), through this
// internal header; no public API, C ABI entry or strategy input reaches them.
// A switch is read when its rule applies and never stored, so it holds no run
// state. Process-wide: set one only around single-threaded runs. Not part of
// the installed API.

namespace pineforge::source::detail {

enum class PineCallbackLifecycleRule : int {
    // trail_points become whole ticks with a mintick-scaled tolerance,
    // ceil(trail_points - c * mintick), instead of the constant 5e-5 ticks
    // (compat::pine::trail_points_to_ticks).
    TrailPointsMintickTolerance = 0,
    // A declined reversal kills the held position's stop and limit exits; a
    // later re-issue of such an exit, changed or not, is a new live order.
    // Without the switch an unchanged re-issue keeps the killed row
    // (reissue_revives_declined_exit).
    DeclinedReversalReissueRevives = 1,
    // A limit exit born in a calc_on_order_fills recalculation reaches the
    // end of its in-flight leg only when that waypoint's tick-built print
    // does; the level itself stays raw (exit()'s in-flight remainder).
    CallbackLimitTickReach = 2,
    // A limit exit under calc_on_order_fills that closes the position held
    // when it is placed, on the chart path, and that its placement point does
    // not already reach, rests at its half-tick threshold: a print half a
    // tick short of an on-grid level reaches it (H 9.725 prints 9.73 and
    // fills a 9.73 sell limit). Without the switch every on-grid level stays
    // raw under calc_on_order_fills (exit_limit_trigger).
    RestingLimitTickReach = 3,
    // A carried trailing stop (calc_on_order_fills off) is reached at the
    // first point where the bar's tick-built print reaches it, when that comes
    // before the raw print would; the level stays the raw running best +/- the
    // offset (retune_carried_trails_for_tick_reach).
    CarriedTrailTickReach = 4,
};

inline constexpr int kPineCallbackLifecycleRuleCount = 5;

void set_pine_callback_lifecycle_rule(PineCallbackLifecycleRule rule, bool on) noexcept;
bool pine_callback_lifecycle_rule(PineCallbackLifecycleRule rule) noexcept;

}  // namespace pineforge::source::detail
