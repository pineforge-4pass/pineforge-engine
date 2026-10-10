#pragma once

// The selected-window report's equity curve: pure data construction, owned by
// the selected-window report boundary (wire contract v1.3). Internal to the
// kernel's report path and to the rows that hold it to that contract
// (tests/test_selected_report_view.cpp); not installed.
// Engine-generic: it reads no strategy, callback, timezone, calendar, source
// adapter or engine state, and a selected caller supplies everything.

#include <pineforge/pineforge.h>

#include <cstdint>
#include <memory>

namespace pineforge {
namespace report {

// A built curve: one array of exactly `count` points, owned by `points`. The
// array comes from a single `new[]`, so whoever takes it frees it the way
// BacktestEngine::free_report frees ReportC::equity_curve -- `delete[]`.
// `count` is plain data and does not follow `points`: it keeps its value after
// `points` is moved from or released.
struct SelectedEquityCurve {
    std::unique_ptr<pf_equity_point_t[]> points;
    int64_t count;
};

// The curve of a window that starts at `window_start_ms` (T) over
// `observation_count` (M) script-bar observations: exactly M + 1 points, so
// count = M + 1. Index 0 is the window anchor -- time_ms = T,
// equity = initial_capital, open_profit = 0 -- and indices 1..M copy the M
// observations byte for byte, so observation i sits at position i + 1. The
// anchor is a curve point, not a script bar: no bar index belongs to it, no
// synthetic bar is inserted, and an observation stamped T (the window's first
// bar opening at its start) stands beside the anchor with the same time.
// M = 0 returns the anchor alone, and only then may `observations` be null.
//
// `observations` is the already selected, chronologically ordered run of
// script-bar points from the engine's execution state, not a caller-sliced
// report; selecting it and ordering it are the caller's invariants. No point is
// filtered, reordered or recomputed, and no argument is judged beyond what the
// construction itself needs: any T and any initial_capital are taken as given.
//
// Giving the curve to a report: `out->equity_curve = curve.points.release();`
// and `out->equity_curve_len = curve.count;` cannot throw, and from there the
// report owns the array, as it owns any array it holds -- whoever frees the
// report frees it, with delete[]. Do it where the report takes its curve today,
// before the presentation hook and the statistics, which read (and may re-mark)
// the report's own copy; the statistics then take n = equity_curve_len and
// observation_count = equity_curve_len - 1. A refusal or allocation failure in
// the build happens before the release and leaves the report untouched, and
// nothing copies the curve.
//
// Refusals: a negative M, a null `observations` with M > 0, and an M whose
// M + 1 points cannot be counted by the int64_t `count` or sized in bytes by a
// size_t are a call the engine itself got wrong, all checked before anything is
// added, allocated or read; each throws coded<std::logic_error> carrying
// RunFailureCode::engine_invariant (the way the recorded outputs refuse a broken
// recorder contract). They are not strategy refusals. An allocation failure is
// std::bad_alloc, which the failure channel already classifies as out_of_memory.
SelectedEquityCurve build_selected_equity_curve(
    int64_t window_start_ms, double initial_capital,
    const pf_equity_point_t* observations, int64_t observation_count);

}  // namespace report
}  // namespace pineforge
