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
//   kSegmentDenomEps — degenerate path-segment denominator guard.
//   kPathTimeEps     — magnifier t-value dedupe tolerance.
inline constexpr double kFullPercentEps  = 1e-9;
inline constexpr double kFullQtyEps      = 1e-9;
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


// Return earliest path position (segment index + [0..1] interpolation) where
// price level is crossed on OHLC path. Returns false if never crossed.
bool first_touch_position(const Bar& bar, double level, double* out_pos);

// design-stop-tick-rounding: the path helpers below also come in a form whose
// LEG ORDER (O->H->L->C vs O->L->H->C) is chosen by the caller. The engine
// walks the tick-quantized twin of a bar (BacktestEngine::broker_trigger_bar)
// with the RAW bar's order, so a path coordinate produced here — the entry
// cursor a same-bar bracket resumes from, a sibling / opposing-order
// tie-break — lives in the same coordinate system as the matcher's own walk
// (src/native_matching.hpp). The single-bar forms derive the order from
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


// Same 4-waypoint path, but with the leg order chosen by the caller (so a
// tick-quantized twin of a bar walks the raw bar's leg order).
void fill_bar_path_points_ordered(const Bar& bar, bool high_first, double path[4]);


// ── Finer-timeframe sub-bar synthesis (defined in engine_lower_tf.cpp) ──
// Generic primitives: a fixed intraday timeframe parser, the integer
// input : requested ratio, and sub-bars sampled evenly along one input
// bar's path. A source language's own admission rules for such a request
// live with its evaluator, not here.


bool is_fixed_intraday_minute_tf(const std::string& tf);


bool supports_lower_tf_emulation(const std::string& input_tf,
                                        const std::string& requested_tf,
                                        int* out_ratio,
                                        int* out_requested_seconds);


std::vector<Bar> synthesize_lower_tf_bars(const Bar& input_bar,
                                                 int ratio,
                                                 int requested_seconds);


}  // namespace internal
}  // namespace pineforge
