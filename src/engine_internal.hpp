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
 *   engine_fills.cpp         - process_pending_orders (uses path helpers)
 *   engine_orders.cpp        - execute_market_* (uses path helpers)
 *   engine_security.cpp      - uses lower-TF helpers
 *   engine_run.cpp           - uses lower-TF helpers
 *
 * The pineforge::internal namespace makes the helpers cross-TU visible
 * without exposing them to external consumers (-fvisibility=hidden on
 * the runtime target keeps them out of any final .so symbol table).
 */

#include <pineforge/engine.hpp>

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
};


// ── Path-resolution helpers (defined in engine_path_resolve.cpp) ──


bool bar_path_uses_high_first(const Bar& bar);


// Returns: -1 = stop hit first, +1 = limit hit first, 0 = neither
// Walks a 4-waypoint intra-bar price path to determine fill priority.
int price_path_priority(const Bar& bar, double stop_level, double limit_level);


// Return earliest path position (segment index + [0..1] interpolation) where
// price level is crossed on OHLC path. Returns false if never crossed.
bool first_touch_position(const Bar& bar, double level, double* out_pos);


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


// For flat-position opposing stop entries (long stop vs short stop), return
// true if any opposite stop is touched earlier on the bar path than `current`.
bool opposing_stop_entry_hits_first(const Bar& bar,
                                           const std::vector<PendingOrder>& orders,
                                           std::size_t current_idx);


DualEntryStopPathWinner dual_entry_stop_path_winner(const Bar& bar,
                                                          const std::vector<PendingOrder>& orders);


// For OCA exit siblings (e.g., separate TP and SL strategy.order calls),
// compute first-touch position on OHLC path for a single-priced order.
bool exit_order_touch_position(const Bar& bar,
                                      const PendingOrder& order,
                                      PositionSide pos,
                                      double* out_pos);


bool oca_exit_sibling_hits_first(const Bar& bar,
                                        const std::vector<PendingOrder>& orders,
                                        std::size_t current_idx,
                                        PositionSide pos);


// strategy.exit → OrderType::EXIT; strategy.order → RAW_ORDER. When a raw order's
// direction opposes the open position, stop/limit/trail behave like closing orders,
// not entries (fixes wrong fill prices for bracket TP/SL from strategy.order).
bool order_is_exit_style(const PendingOrder& o, PositionSide pos);


void fill_bar_path_points(const Bar& bar, double path[4]);


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


bool resolve_entry_stop_limit_fill(const Bar& bar,
                                          bool is_long,
                                          double stop_price,
                                          double limit_price,
                                          double* fill_price);


// Earliest intra-bar path coordinate [0, 3) where this EXIT's stop/limit would
// first fill, ignoring trail. Orders sibling strategy.exit() calls with the same
// from_entry by TradingView OHLC path (e.g. partial TP vs full bracket).
// Returns +inf if no fill this bar or if the order uses trail (caller falls back
// to full-before-partial).
double exit_order_earliest_path_metric_no_trail(
    const Bar& bar,
    const PendingOrder& order,
    PositionSide position_side,
    bool is_entry_bar,
    double position_entry_price);


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
                                           double syminfo_mintick);


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
