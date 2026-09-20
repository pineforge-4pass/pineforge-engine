#pragma once
/*
 * engine_internal.hpp — RUNTIME-PRIVATE header. Lives in src/, not in
 * include/pineforge/, so external consumers cannot reach these symbols.
 *
 * Declares the path-resolution + lower-TF emulation helpers shared
 * between the runtime's translation units after the phase-7 split:
 *
 *   engine_path_resolve.cpp  - definitions of path::* helpers
 *   engine_lower_tf.cpp      - definitions of lower-TF helpers
 *   native_execution_consumer.cpp - request matching (uses path helpers)
 *   engine_orders.cpp        - execute_market_* (uses path helpers)
 *   engine_security.cpp      - uses lower-TF helpers
 *   engine_run.cpp           - uses lower-TF helpers
 *
 * The pineforge::internal namespace makes the helpers cross-TU visible
 * without exposing them to external consumers (-fvisibility=hidden on
 * the runtime target keeps them out of any final .so symbol table).
 */

#include <pineforge/engine.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace pineforge {
namespace internal {

// Shared quantity-comparison epsilons. Both values are unchanged from the
// bare literals they replaced — they exist purely to name the magic numbers
// the order/fill mechanics use when deciding whether a residual quantity is
// effectively zero.
//
//   kQtyEpsilon    — general partial-exit / position-quantity slack (1e-10).
//   kOcaQtyEpsilon — tighter slack used by OCA residual-qty bookkeeping
//                    (reduce_oca_group / OCA fully-filled check) (1e-12).
inline constexpr double kQtyEpsilon = 1e-10;
inline constexpr double kOcaQtyEpsilon = 1e-12;

// Additional named comparison thresholds. Values are byte-identical to the
// bare literals they replace — naming them prevents accidental "harmonizing".
//
//   kFullPercentEps  — qty_percent >= 100 - eps means "full (100%) exit".
//   kFullQtyEps      — qty-domain "is this a full exit" slack: qty within
//                      1e-9 of the open position counts as full. Same value
//                      as kFullPercentEps by coincidence, conceptually
//                      distinct — do not merge.
//   kPathPosEps      — intra-bar path-position comparisons (segment + [0..1]).
//   kSegmentDenomEps — degenerate path-segment denominator guard.
//   kPathTimeEps     — magnifier t-value dedupe tolerance.
inline constexpr double kFullPercentEps  = 1e-9;
inline constexpr double kFullQtyEps      = 1e-9;
inline constexpr double kPathPosEps      = 1e-12;
inline constexpr double kSegmentDenomEps = 1e-15;
inline constexpr double kPathTimeEps     = 1e-12;

// The id prefix the engine stamps on every EXIT order materialised from a
// strategy.close / strategy.close_all instruction ("__close__" + target id,
// bare "__close__" for close_all). It is the ONLY structural marker that
// separates a close-path reduction from a strategy.exit bracket leg fill at
// the exit-fill site, and three call sites previously each carried their own
// function-local copy. One definition, so the predicate cannot drift.
inline const std::string kClosePrefix = "__close__";

// Among flat pending opposite ENTRY stop-only orders, which stop price is touched
// first on the synthesized OHLC path (exactly one long + one short, both touched).
// Forward-declared in <pineforge/engine.hpp> with the same underlying type
// so BacktestEngine method signatures can reference it without including
// this private header.
enum class DualEntryStopPathWinner : int {
    None,
    LongFirst,
    ShortFirst,
    Tie,
};

// Kind of price-cross event on the synthesized OHLC path. Used by the
// helpers in engine_path_resolve.cpp; exposed in this header purely so
// the helpers' declarations compile — external code should not depend
// on these values.
enum class PathCrossKind { STOP, LIMIT, TRAIL };

struct PathCrossEvent {
    double price;
    double path_pos;
    PathCrossKind kind;
};

// Fixed-capacity event list: at most one STOP, one LIMIT, one TRAIL event
// can exist per path segment. Replaces a heap vector in the innermost
// fill-resolution loop.
struct CrossEventList {
    PathCrossEvent ev[3];
    int n = 0;
    const PathCrossEvent* begin() const { return ev; }
    const PathCrossEvent* end() const { return ev + n; }
};

struct ExitPathFill {
    bool should_fill = false;
    double fill_price = std::numeric_limits<double>::quiet_NaN();
    // True when the TRAIL leg produced the fill (vs stop/limit/gap-open).
    // Consumers use it to reconstruct the trail's peak (fill +/- offset)
    // for per-trade excursion reporting.
    bool is_trail = false;
    // True when the LIMIT leg produced the fill (intra-bar touch of the
    // limit, or a gap-open beyond the limit). TradingView fills limit
    // orders at limit-or-better with NO slippage (see apply_limit_fill in
    // engine.hpp); the fill-application code needs to know which leg fired
    // because price equality cannot distinguish a gap fill at the open.
    bool is_limit = false;
    // True when the fill is the RAW BAR OPEN (open-gap shortcut through a
    // trail / stop / limit level) rather than a level. finding-446: the
    // consumer nearest-tick rounds a bar-price fill (bar_fill_price) while a
    // level fill keeps its directional snap; price equality cannot tell the
    // two apart when a sub-tick open coincides with a level.
    bool at_bar_open = false;
    // True when an at_bar_open fill is the trail's LEVEL rather than the raw
    // print: a one-shot (zero-offset) trail whose activation the open already
    // sits past arms AT the open with best = open and fills at open -/+ 0 —
    // a COMPUTED level, so the consumer snaps it directionally (sell floor,
    // buy ceil) instead of nearest-rounding the bar print. `lab tv`
    // NASDAQ:AAPL 15m (round 7 family G, scratchpad/r7/pins/scalper-trail-*):
    // long exit at the 2025-04-22 13:30Z open 196.135 -> TV 196.13 (engine
    // booked bar_fill_price = 196.14); short exit at the 05-23 13:30Z open
    // 193.665 -> 193.67.
    bool open_is_trail_level = false;
    // Where the fill happened on the bar's 4-waypoint synthesized path, in
    // first_touch_position units (0 = open, 1/2 = the extremes, 3 = close;
    // fractional inside a segment). This is the fill's ACTUAL chronology,
    // which for a TRAIL leg is not recoverable from the fill price alone —
    // a trail's level is not a resting one, so its first path touch can
    // precede the moment it arms and fires. finding-308's pre-exit
    // margin-call slice compares this against the adverse extreme.
    double path_position = std::numeric_limits<double>::quiet_NaN();
};


// ── Trailing-exit tick arithmetic ─────────────────────────────────────
//
// TradingView reads strategy.exit's trail_points / trail_offset as TICK
// COUNTS: trail_points is ceiled WITH a tolerance that swallows floating-
// point residue — a value computed as close * 0.02 / syminfo.mintick that
// lands a few ulps past a whole number is that whole number, not the next
// one — while trail_offset is floored EXACTLY. Pinned with `lab tv` on
// NYSE:F 15m 2025-04-01..05, short from the 04-02 19:00Z signal (entry
// 10.11, mintick 0.01; scratchpad/r7/pins/trail-eq-*), each zero-offset
// exit filling at its activation level:
//   trail_points 14.00001                -> 14 ticks (9.97)
//   trail_points 14.0001 / 14.001        -> 15 ticks (9.96)
//   trail_points 0.14 / syminfo.mintick  (= 14.000000000000002) -> 14 (9.97)
//   trail_points 14.0000001              -> 14 (9.97)
//   trail_points 18.2                    -> 19 (9.92)
//   trail_offset 0.3 / (syminfo.mintick * 10)  (= 2.9999999999999996)
//     -> 2 ticks (trail_points 18: fills 04-03 14:15Z @9.85 = trough 9.83
//     + 2t; a tolerant floor would trail 3t and print 9.86)
// and BINANCE:BTCUSDT 15m short 2025-08-17 23:30Z @117559.99 with
// trail_points = 117560 * 0.02 / 0.01 = 235120.00000000003: TV exits
// 08-18 03:30Z @115208.79 (235120 ticks); std::ceil gave 235121 -> .78.
// The ceil tolerance is bounded in [1e-5, 1e-4) by the 14.00001 / 14.0001
// pair; 5e-5 sits in the middle. Round 5's sub-tick pins hold: 0.0006 ->
// 1 tick, offsets 0 / 0.5 / 0.9 -> 0, 1.4 -> 1.
constexpr double kTrailPointsCeilEps = 5e-5;

inline double trail_points_to_ticks(double trail_points) {
    return std::ceil(trail_points - kTrailPointsCeilEps);  // NaN stays NaN
}

inline double trail_offset_to_ticks(double trail_offset) {
    return std::floor(trail_offset);  // exact; NaN stays NaN
}

// The broker keeps trailing levels ON the tick grid: an activation level
// entry -/+ N ticks and a trailing level best -/+ K ticks are tick counts,
// and a bar extreme landing exactly on the level TOUCHES it. Built in
// doubles, 10.11 - 21 * 0.01 is 9.899999999999999 — one ulp under the 9.9
// low — and the inclusive segment test reads it as "not reached" (NYSE:F
// 15m 2025-04-03 13:45Z, bar O 10.165 H 10.18 L 9.9 C 9.9: TV fills the
// trail there @9.90, `lab tv` trail-eq-S-off0-tp21; the engine gap-filled
// the next open @9.89). Materialize a level within a millionth of a tick
// of the grid as the grid point, spelled the way tick_grid_price
// (engine.hpp) spells it so it compares EQUAL to a feed print bit for bit;
// a genuinely sub-tick level (best 196.135 - 1 tick = 196.125) stays raw
// and takes the directional fill snap downstream.
inline double snap_trail_level_to_tick_grid(double price, double mintick) {
    if (std::isnan(price) || !(mintick > 0.0)) return price;
    const double r = price / mintick;
    const double k = std::floor(r + 0.5);
    if (std::abs(r - k) > 1e-6) return price;
    const double inv = 1.0 / mintick;
    const double inv_int = std::floor(inv + 0.5);
    if (inv_int > 0.0 && std::abs(inv - inv_int) <= 1e-6 * inv_int) {
        return k / inv_int;
    }
    return k * mintick;
}


// ── Path-resolution helpers (defined in engine_path_resolve.cpp) ──


bool bar_path_uses_high_first(const Bar& bar);

// ABI v4 live-runtime surface (task 4): force bar_path_uses_high_first's
// verdict for the calling thread -- 0 AUTO (the real |H-O| vs |O-L| rule,
// unchanged), 1 HIGH_FIRST, 2 LOW_FIRST. thread_local: a handle is
// single-threaded per run, and this is installed/cleared for exactly one
// run's duration by the PathOrderScope guard in engine_run.cpp. See
// BacktestEngine::set_path_order (engine.hpp).
void set_path_order_override(int mode);

// Current thread-local override value (see above). PathOrderScope reads this
// before installing its own mode so it can restore the prior value on scope
// exit, rather than hardcoding AUTO -- correct even if a future caller ever
// nests two overridden runs on the same thread.
int path_order_override();


// Returns: -1 = stop hit first, +1 = limit hit first, 0 = neither
// Walks a 4-waypoint intra-bar price path to determine fill priority.
int price_path_priority(const Bar& bar, double stop_level, double limit_level);


// Return earliest path position (segment index + [0..1] interpolation) where
// price level is crossed on OHLC path. Returns false if never crossed.
bool first_touch_position(const Bar& bar, double level, double* out_pos);

// design-stop-tick-rounding: the path helpers below also come in a form whose
// LEG ORDER (O->H->L->C vs O->L->H->C) is chosen by the caller. The engine
// walks the tick-quantized twin of a bar (BacktestEngine::broker_trigger_bar)
// with the RAW bar's order, so a path coordinate produced here — the entry
// cursor a same-bar bracket resumes from, a sibling / opposing-order
// tie-break — lives in the same coordinate system as resolve_exit_path_fill's
// walk. The single-bar forms derive the order from
// the bar they are given (bar_path_uses_high_first) and are unchanged.
bool first_touch_position(const Bar& bar, bool high_first, double level,
                          double* out_pos);


// First path position where a stop ENTRY can fire, accounting for direction:
// long stops only fire on up-segments (price rising through the stop), short
// stops only fire on down-segments. The gap-fill shortcut uses non-strict
// comparisons: when bar.open already sits at or beyond the stop level in the
// firing direction the order fills at open (path position 0). For the
// open-equals-stop case both legs return 0 simultaneously and the dual-stop
// arbitration breaks the tie in favour of the long leg — this matches TV's
// broker emulator on probe 83.
bool entry_stop_first_touch(const Bar& bar, double stop_level,
                                   bool is_long, double* out_pos);
bool entry_stop_first_touch(const Bar& bar, bool high_first, double stop_level,
                            bool is_long, double* out_pos);


void fill_bar_path_points(const Bar& bar, double path[4]);

// Same 4-waypoint path, but with the leg order chosen by the caller (so a
// tick-quantized twin of a bar walks the raw bar's leg order).
void fill_bar_path_points_ordered(const Bar& bar, bool high_first, double path[4]);


int path_cross_kind_priority(PathCrossKind kind);


void append_cross_event(CrossEventList* events,
                               double from_price,
                               double to_price,
                               double level,
                               PathCrossKind kind);


CrossEventList collect_cross_events(double from_price,
                                                        double to_price,
                                                        double stop_level,
                                                        double limit_level,
                                                        double trail_level);

// design-stop-tick-rounding: stop and limit crossings are taken on the
// tick-quantized segment (tick_from -> tick_to), an ACTIVE trail's level
// crossing on the raw segment (from -> to); the merged list keeps
// collect_cross_events's order. design-trail-activation-tick-bar: a dormant
// exit-at-activation trail's activation level (trail_activation_level, set
// only when trail_level is NaN) is reached on the tick-quantized segment and
// reported as the TRAIL event.
CrossEventList collect_cross_events_split(double from_price,
                                          double to_price,
                                          double tick_from_price,
                                          double tick_to_price,
                                          double stop_level,
                                          double limit_level,
                                          double trail_level,
                                          double trail_activation_level
                                              = std::numeric_limits<double>::quiet_NaN());


// fill_at_bar_point (optional) reports whether the returned fill price is a
// RAW OHLC path point (the limit was already marketable there) rather than
// the stop or limit level itself — the caller nearest-tick rounds bar-point
// fills (finding-446) while level fills keep their limit-or-better snap.
bool resolve_entry_stop_limit_fill(const Bar& bar,
                                          bool is_long,
                                          double stop_price,
                                          double limit_price,
                                          double* fill_price,
                                          bool* activated,
                                          bool* fill_at_bar_point = nullptr);


// design-stop-tick-rounding: `tick_bar` is the bar the STOP / LIMIT legs are
// tested against (the engine passes BacktestEngine::broker_trigger_bar(bar):
// OHLC quantized to the tick, level raw); the TRAIL leg walks `bar` itself.
// The two share the raw bar's leg order and segment cursor.
ExitPathFill resolve_exit_path_fill(const Bar& bar,
                                           const Bar& tick_bar,
                                           PositionSide position_side,
                                           double stop_price,
                                           double limit_price,
                                           double trail_points,
                                           double trail_price,
                                           double trail_offset,
                                           double position_entry_price,
                                           double trail_best_start,
                                           bool is_entry_bar,
                                           bool magnifier_active,
                                           double syminfo_mintick,
                                           bool cascade_wp_gap = false,
                                           double path_start_position = 0.0);

// Raw-only form (tick_bar == bar): every leg walks the raw bar.
ExitPathFill resolve_exit_path_fill(const Bar& bar,
                                           PositionSide position_side,
                                           double stop_price,
                                           double limit_price,
                                           double trail_points,
                                           double trail_price,
                                           double trail_offset,
                                           double position_entry_price,
                                           double trail_best_start,
                                           bool is_entry_bar,
                                           bool magnifier_active,
                                           double syminfo_mintick,
                                           bool cascade_wp_gap = false,
                                           double path_start_position = 0.0);


// KI-67 exit cascade (Model S "R-cascade-gapjump"). Given the in-flight LEG
// index seg_i (0 = O->W1, 1 = W1->W2, 2 = W2->C; supplied by the dispatch loop's
// real cursor position), the full script bar, and the recalc price ap the
// triggering fill landed on, returns true when the exit level lies inside the
// in-flight remainder (ap -> leg-end waypoint W0 = path[seg_i+1]) in the trigger
// direction on a NON-terminal leg — i.e. it gap-fills same-bar at W0 (limits
// fill better than the level, stops worse). False for a terminal (seg_i>=2) or
// off-path (seg_i<0) leg, and whenever the level is not swept in the remainder.
bool cascade_exit_inflight_fires(const Bar& bar, double ap, int seg_i,
                                 PositionSide position_side,
                                 double stop_price, double limit_price);


// ── Lower-TF emulation helpers (defined in engine_lower_tf.cpp) ──


bool is_fixed_intraday_minute_tf(const std::string& tf);


bool supports_lower_tf_emulation(const std::string& input_tf,
                                        const std::string& requested_tf,
                                        int* out_ratio,
                                        int* out_requested_seconds);


void ensure_supported_lower_tf_emulation_flags(bool lookahead_on, bool gaps_on);


std::vector<Bar> synthesize_lower_tf_bars(const Bar& input_bar,
                                                 int ratio,
                                                 int requested_seconds);


}  // namespace internal
}  // namespace pineforge
