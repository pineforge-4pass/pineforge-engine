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
#include <cstdint>
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
// The open-proximity rule: O -> H -> L -> C when the open is nearer the high
// (|H-O| < |O-L|), otherwise O -> L -> H -> C, ties low-first. A run that
// declares a leg order (NativeRunSpec::path_order) is walked in it by the
// consumer (NativeExecutionConsumer::path_high_first), which hands that order
// to the intrabar sampler itself (sample_price_path_ordered below). Until R5
// lane D2-A a thread-local override, installed around the sampler alone,
// carried it in here; it was AUTO everywhere else, which is what this
// answers.
bool bar_path_uses_high_first(const Bar& bar);
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
bool entry_stop_first_touch(const Bar& bar, bool high_first, double stop_level,
                            bool is_long, double* out_pos);
// Same 4-waypoint path, but with the leg order chosen by the caller (so a
// tick-quantized twin of a bar walks the raw bar's leg order).
void fill_bar_path_points_ordered(const Bar& bar, bool high_first, double path[4]);
// ── The magnifier sampler in a caller's leg order (defined in magnifier.cpp) ──
// sample_price_path and sample_price_path_volume_weighted
// (<pineforge/magnifier.hpp>) walk the open-proximity order; these forms walk
// the order the caller resolved and give, bit for bit, what the public forms
// give when bar_path_uses_high_first(bar) answers `high_first`. The native
// intrabar driver samples through them in its run's declared order. Neither
// keeps state between calls.
void sample_price_path_ordered(const Bar& bar, bool high_first, int n_samples,
                               MagnifierDistribution dist, std::vector<double>& out);
void sample_price_path_volume_weighted_ordered(const Bar& bar, bool high_first,
                                               int base_samples, double mean_volume,
                                               int min_samples, int max_samples,
                                               MagnifierDistribution dist,
                                               std::vector<double>& out);

// The four ENDPOINTS samples of one bar -- the open, the two turning points
// and the close -- computed directly (R5 lane D2-A): the same values, from
// the same operations, as the general routine gives for four samples, without
// its t-value pass. It answers false, writing nothing, for every bar whose
// four turning times are not four distinct ones (a zero, non-finite or
// zero-length leg), which the general routine serves.
bool sample_endpoints4(const Bar& bar, bool high_first, double out[4]) noexcept;

// sample_price_path_ordered takes sample_endpoints4 for every four-sample
// ENDPOINTS call unless set_direct_endpoints(false) turns it off, which
// restores the general routine for every call. The path is exact, so it is
// not a run-spec choice: the switch and the counts exist so
// tests/test_magnifier_endpoints4.cpp can hold the two computations equal bit
// for bit and see which one ran. Both are process-wide and a sampler call
// only reads them; no host reaches either.
struct EndpointPathCounts {
    std::uint64_t direct = 0;
    std::uint64_t general = 0;
};

void set_direct_endpoints(bool enabled) noexcept;
// Enabled, zeroes the counts and counts every later four-sample ENDPOINTS
// call by the path it took; disabled, stops counting.
void count_endpoint_paths(bool enabled) noexcept;
EndpointPathCounts endpoint_path_counts() noexcept;
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
// ── Fused settlement (R5 lane PERF-L2, defined in engine_execution.cpp) ──
// A settlement call that a book of at most one lot settles in one pass
// (BacktestEngine::NativeSettlementStage::OneLot) takes that pass unless
// set_fused_settlement(false) turns it off, which restores the staged chain
// for every call. The pass is exact, so it is not a run-spec choice: the
// switch and the counts exist so tests/test_native_fused_settlement.cpp can
// hold the two computations equal bit for bit and see which one ran. Both are
// process-wide and a run only reads them; no host reaches either.
// The fused-capable entries, as the counts index them: the inspection
// (inspect_with_membership, inspect_native_reversal_v1), the projection
// (project_with_membership, project_native_reversal_v1), the precommit
// preview (both preview_native_settlement_commit) and the settlement
// (settle_with_membership, settle_native_reversal_at_v1).
enum class SettlementEntry : int { Inspect = 0, Project = 1, Preview = 2, Settle = 3 };
inline constexpr int kSettlementEntries = 4;

struct SettlementPathCounts {
    std::uint64_t fused[kSettlementEntries] = {};
    std::uint64_t staged[kSettlementEntries] = {};
};

void set_fused_settlement(bool enabled) noexcept;
// Enabled, zeroes the counts and counts every later call of a fused-capable
// entry by the path it took; disabled, stops counting.
void count_settlement_paths(bool enabled) noexcept;
SettlementPathCounts settlement_path_counts() noexcept;


}  // namespace internal
}  // namespace pineforge
