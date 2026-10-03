#pragma once
// Per-rule switches for the N6 callback and lifecycle pins (TradingView
// synthetic tapes, tests/fixtures/n6_callback_lifecycle). Every switch is ON
// in the shipped library. They exist so a regression can be bisected one
// rule at a time: only tests clear them (test_n6_callback_lifecycle_tapes
// clears each in turn and requires exactly its tapes to depart), through this
// internal header; no public API, C ABI entry or strategy input reaches them.
// A switch is read when its rule applies and never stored, so it holds no run
// state. Process-wide: set one only around single-threaded runs. Not part of
// the installed API.

namespace pineforge::source::detail {

enum class PineN6Rule : int {
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
};

inline constexpr int kPineN6RuleCount = 3;

void set_pine_n6_rule(PineN6Rule rule, bool on) noexcept;
bool pine_n6_rule(PineN6Rule rule) noexcept;

}  // namespace pineforge::source::detail
